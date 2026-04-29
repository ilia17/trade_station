#include "bingx_order_stream.h"
#include "../trading/rest_client.h"
#include <iostream>
#include <chrono>
#include <zlib.h>

using json = nlohmann::json;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Gzip decompression helper (BingX compresses all WebSocket frames)
// ─────────────────────────────────────────────────────────────────────────────

static std::string gzip_decompress(const std::string& in) {
    z_stream zs{};
    // inflateInit2 with windowBits = 15+32 auto-detects gzip or deflate
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return in;
    zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out;
    char buf[4096];
    int  ret;
    do {
        zs.next_out  = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) break;
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END && zs.avail_out == 0);
    inflateEnd(&zs);
    return out.empty() ? in : out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

BingXOrderStream::BingXOrderStream(const std::string& api_key,
                                   const std::string& api_secret,
                                   SharedOrderEvents* events)
    : api_key_(api_key), api_secret_(api_secret), events_(events)
{
    client_.clear_access_channels(websocketpp::log::alevel::all);
    client_.clear_error_channels(websocketpp::log::elevel::all);
    client_.init_asio();

    client_.set_tls_init_handler([this](websocketpp::connection_hdl) {
        return on_tls_init();
    });
    client_.set_open_handler([this](websocketpp::connection_hdl hdl) {
        on_open(hdl);
    });
    client_.set_message_handler([this](websocketpp::connection_hdl hdl,
                                       bingx_order_ws_t::message_ptr msg) {
        on_message(hdl, msg);
    });
    client_.set_close_handler([this](websocketpp::connection_hdl hdl) {
        on_close(hdl);
    });
    client_.set_fail_handler([this](websocketpp::connection_hdl hdl) {
        on_fail(hdl);
    });
}

BingXOrderStream::~BingXOrderStream() { stop(); }

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────────────────────

void BingXOrderStream::start() {
    if (running_.load()) return;

    if (!get_listen_key()) {
        std::cerr << "[BingXOrderStream] Failed to get listenKey — "
                     "check BINGX_API_KEY is set\n";
        return;
    }

    running_ = true;

    ws_thread_ = std::thread([this]() {
        std::string url = "wss://open-api-ws.bingx.com/market?listenKey=" + listen_key_;
        websocketpp::lib::error_code ec;
        auto con = client_.get_connection(url, ec);
        if (ec) {
            std::cerr << "[BingXOrderStream] WS connect error: " << ec.message() << "\n";
            running_ = false;
            return;
        }
        client_.connect(con);
        client_.run();
    });

    // Renew the listenKey every 30 min (valid for 60 min, so this is safe margin)
    renew_thread_ = std::thread([this]() {
        int ticks = 0;
        while (running_.load()) {
            std::this_thread::sleep_for(5s);
            if (!running_.load()) break;
            if (++ticks >= 360) {   // 360 × 5s = 30 min
                ticks = 0;
                if (!renew_listen_key())
                    std::cerr << "[BingXOrderStream] listenKey renewal failed\n";
            }
        }
    });
}

void BingXOrderStream::stop() {
    if (!running_.load()) return;
    running_   = false;
    connected_ = false;
    client_.stop();
    if (ws_thread_.joinable())    ws_thread_.join();
    if (renew_thread_.joinable()) renew_thread_.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// listenKey management
// ─────────────────────────────────────────────────────────────────────────────

bool BingXOrderStream::get_listen_key() {
    try {
        // POST with empty body; only X-BX-APIKEY header required (no HMAC signing)
        std::string resp = https_post("open-api.bingx.com",
            "/openApi/user/auth/userDataStream", "",
            {{"X-BX-APIKEY",  api_key_},
             {"Content-Type", "application/json"}});

        auto j = json::parse(resp);
        if (!j.contains("listenKey")) {
            std::cerr << "[BingXOrderStream] No listenKey in response: " << resp << "\n";
            return false;
        }
        listen_key_ = j["listenKey"].get<std::string>();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[BingXOrderStream] get_listen_key error: " << e.what() << "\n";
        return false;
    }
}

bool BingXOrderStream::renew_listen_key() {
    try {
        https_put("open-api.bingx.com",
            "/openApi/user/auth/userDataStream?listenKey=" + listen_key_, "",
            {{"X-BX-APIKEY",  api_key_},
             {"Content-Type", "application/json"}});
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[BingXOrderStream] renew_listen_key error: " << e.what() << "\n";
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket callbacks
// ─────────────────────────────────────────────────────────────────────────────

bingx_order_ssl_t BingXOrderStream::on_tls_init() {
    auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::tlsv12_client);
    ctx->set_verify_mode(boost::asio::ssl::verify_none);
    return ctx;
}

void BingXOrderStream::on_open(websocketpp::connection_hdl hdl) {
    connection_ = hdl;
    connected_  = true;
    subscribe_orders();
    schedule_ping();
}

void BingXOrderStream::on_message(websocketpp::connection_hdl /*hdl*/,
                                   bingx_order_ws_t::message_ptr msg) {
    // All BingX frames are gzip-compressed; decompress first
    std::string raw = gzip_decompress(msg->get_payload());

    // Server-initiated keep-alive ping
    if (raw == "Ping" || raw == "ping") {
        websocketpp::lib::error_code ec;
        client_.send(connection_, "Pong", websocketpp::frame::opcode::text, ec);
        return;
    }

    try {
        auto j = json::parse(raw);
        if (j.value("dataType", "") == "spot.executionReport" && j.contains("data")) {
            handle_order_event(j["data"]);
        }
    } catch (...) {}
}

void BingXOrderStream::on_close(websocketpp::connection_hdl) {
    connected_ = false;
}

void BingXOrderStream::on_fail(websocketpp::connection_hdl) {
    connected_ = false;
    running_   = false;
    std::cerr << "[BingXOrderStream] Connection failed\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Subscription + ping
// ─────────────────────────────────────────────────────────────────────────────

void BingXOrderStream::subscribe_orders() {
    json sub = {
        {"id",       "bx-orders"},
        {"reqType",  "sub"},
        {"dataType", "spot.executionReport"}
    };
    websocketpp::lib::error_code ec;
    client_.send(connection_, sub.dump(), websocketpp::frame::opcode::text, ec);
}

void BingXOrderStream::schedule_ping() {
    client_.set_timer(20000, [this](websocketpp::lib::error_code const& ec) {
        if (ec || !running_.load()) return;
        websocketpp::lib::error_code send_ec;
        // Send application-level ping to keep connection alive
        client_.send(connection_, "ping", websocketpp::frame::opcode::text, send_ec);
        if (!send_ec) schedule_ping();
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Order event parsing
// ─────────────────────────────────────────────────────────────────────────────

void BingXOrderStream::handle_order_event(const json& d) {
    // X = order status: NEW / PARTIALLY_FILLED / FILLED / CANCELED
    std::string status = d.value("X", "");
    if (status.empty()) return;

    OrderEvent ev;
    ev.exchange = "BingX";

    // Order ID — numeric in BingX, stored as string for uniformity
    try {
        if (d.contains("i")) {
            if (d["i"].is_number())
                ev.order_id = std::to_string(d["i"].get<long long>());
            else
                ev.order_id = d["i"].get<std::string>();
        }
    } catch (...) {}

    ev.client_id = d.value("C", "");
    ev.symbol    = d.value("s", "");

    std::string side_str = d.value("S", "BUY");
    ev.side = (side_str == "BUY") ? 0 : 1;

    // Prices/quantities can be number or string depending on order type
    auto parse_num = [&](const char* key) -> double {
        try {
            if (!d.contains(key)) return 0.0;
            const auto& v = d[key];
            if (v.is_number()) return v.get<double>();
            if (v.is_string()) return std::stod(v.get<std::string>());
        } catch (...) {}
        return 0.0;
    };

    ev.price     = parse_num("p");   // original order price
    ev.qty       = parse_num("q");   // original order quantity (base)
    ev.cum_qty   = parse_num("z");   // cumulative filled quantity (base)
    ev.avg_price = parse_num("L");   // last filled price

    // Fallback for market-by-amount orders where "q" (base qty) is 0:
    // "Q" = original order amount in quote currency; use it so the row isn't blank.
    if (ev.qty <= 0.0) ev.qty = parse_num("Q");
    // If still 0, fall back to whatever has been filled so far
    if (ev.qty <= 0.0) ev.qty = ev.cum_qty;

    if      (status == "NEW")              { ev.event_type = "new";    }
    else if (status == "PARTIALLY_FILLED") { ev.event_type = "update"; }
    else if (status == "FILLED")           { ev.event_type = "finish"; ev.finish_as = "filled";    }
    else if (status == "CANCELED" ||
             status == "CANCELLED")        { ev.event_type = "finish"; ev.finish_as = "cancelled"; }
    else return;

    if (events_) events_->push(std::move(ev));
}
