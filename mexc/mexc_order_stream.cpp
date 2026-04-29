#include "mexc_order_stream.h"
#include "../trading/rest_client.h"
#include <iostream>
#include <chrono>

using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

MexcOrderStream::MexcOrderStream(const std::string& api_key,
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
                                       order_ws_client::message_ptr msg) {
        on_message(hdl, msg);
    });
    client_.set_close_handler([this](websocketpp::connection_hdl hdl) {
        on_close(hdl);
    });
    client_.set_fail_handler([this](websocketpp::connection_hdl hdl) {
        on_fail(hdl);
    });
}

MexcOrderStream::~MexcOrderStream() { stop(); }

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop
// ─────────────────────────────────────────────────────────────────────────────

void MexcOrderStream::start() {
    if (running_.load()) return;

    if (!get_listen_key()) {
        std::cerr << "[MexcOrderStream] Failed to get listenKey — "
                     "check MEXC_API_KEY is set\n";
        return;
    }

    running_ = true;

    // WebSocket thread — runs client_.run() which blocks until stopped
    ws_thread_ = std::thread([this]() {
        std::string url = "wss://wbs-api.mexc.com/ws?listenKey=" + listen_key_;
        websocketpp::lib::error_code ec;
        auto con = client_.get_connection(url, ec);
        if (ec) {
            std::cerr << "[MexcOrderStream] WS connect error: " << ec.message() << "\n";
            running_ = false;
            return;
        }
        client_.connect(con);
        client_.run();
    });

    // Renew thread — keeps the listenKey alive (PUT every 30 min)
    renew_thread_ = std::thread([this]() {
        // Wake every 5 s so stop() doesn't wait long
        int ticks = 0;
        while (running_.load()) {
            std::this_thread::sleep_for(5s);
            if (!running_.load()) break;
            ticks++;
            if (ticks >= 360) {   // 360 × 5 s = 30 min
                ticks = 0;
                if (!renew_listen_key())
                    std::cerr << "[MexcOrderStream] listenKey renewal failed\n";
            }
        }
    });
}

void MexcOrderStream::stop() {
    if (!running_.load()) return;
    running_   = false;
    connected_ = false;

    client_.stop();

    if (ws_thread_.joinable())     ws_thread_.join();
    if (renew_thread_.joinable())  renew_thread_.join();
}

// ─────────────────────────────────────────────────────────────────────────────
// listenKey management (MEXC REST — no HMAC signing needed)
// ─────────────────────────────────────────────────────────────────────────────

// Build a signed query string with timestamp + HMAC-SHA256 signature.
static std::string mexc_signed_query(const std::string& api_secret,
                                      const std::string& extra_params = "") {
    long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string qs = (extra_params.empty() ? "" : extra_params + "&")
                   + "timestamp=" + std::to_string(ts);
    qs += "&signature=" + hmac_sha256_hex(api_secret, qs);
    return qs;
}

bool MexcOrderStream::get_listen_key() {
    try {
        std::string qs   = mexc_signed_query(api_secret_);
        std::string resp = https_post("api.mexc.com",
            "/api/v3/userDataStream?" + qs, "",
            {{"X-MEXC-APIKEY", api_key_},
             {"Content-Type",  "application/json"}});

        auto j = nlohmann::json::parse(resp);
        if (!j.contains("listenKey")) {
            std::cerr << "[MexcOrderStream] No listenKey in response: " << resp << "\n";
            return false;
        }
        listen_key_ = j["listenKey"].get<std::string>();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[MexcOrderStream] get_listen_key error: " << e.what() << "\n";
        return false;
    }
}

bool MexcOrderStream::renew_listen_key() {
    try {
        std::string qs = mexc_signed_query(api_secret_,
                                           "listenKey=" + listen_key_);
        https_request(http::verb::put, "api.mexc.com",
            "/api/v3/userDataStream?" + qs, "",
            {{"X-MEXC-APIKEY", api_key_}});
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[MexcOrderStream] renew_listen_key error: " << e.what() << "\n";
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket callbacks
// ─────────────────────────────────────────────────────────────────────────────

order_ssl_ptr MexcOrderStream::on_tls_init() {
    auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
        boost::asio::ssl::context::tlsv12_client);
    ctx->set_verify_mode(boost::asio::ssl::verify_none);
    return ctx;
}

void MexcOrderStream::on_open(websocketpp::connection_hdl hdl) {
    connection_ = hdl;
    connected_  = true;
    subscribe_orders();
    schedule_ping();
}

void MexcOrderStream::schedule_ping() {
    // MEXC closes idle connections after ~20s; send {"msg":"ping"} every 15s.
    client_.set_timer(15000, [this](websocketpp::lib::error_code const& ec) {
        if (ec || !running_.load()) return;
        json ping = {{"msg", "ping"}};
        websocketpp::lib::error_code send_ec;
        client_.send(connection_, ping.dump(),
                     websocketpp::frame::opcode::text, send_ec);
        if (!send_ec) schedule_ping();
    });
}

void MexcOrderStream::subscribe_orders() {
    json sub = {
        {"method", "SUBSCRIPTION"},
        {"params", {"spot@private.orders.v3.api.pb"}}
    };
    websocketpp::lib::error_code ec;
    client_.send(connection_, sub.dump(), websocketpp::frame::opcode::text, ec);
    if (ec)
        std::cerr << "[MexcOrderStream] subscribe send error: " << ec.message() << "\n";
}

void MexcOrderStream::on_message(websocketpp::connection_hdl /*hdl*/,
                                  order_ws_client::message_ptr msg) {
    const std::string& raw = msg->get_payload();

    // JSON frames: ping / ack
    if (!raw.empty() && raw[0] == '{') {
        try {
            auto j = nlohmann::json::parse(raw);
            if (j.contains("msg") && j["msg"] == "ping") {
                json pong = {{"msg", "pong"}};
                websocketpp::lib::error_code ec;
                client_.send(connection_, pong.dump(),
                             websocketpp::frame::opcode::text, ec);
            }
        } catch (...) {}
        return;
    }

    // Binary frame — PushDataV3ApiWrapper protobuf
    try {
        PushDataV3ApiWrapper wrapper;
        if (!wrapper.ParseFromString(raw)) return;

        if (wrapper.has_privateorders()) {
            handle_order_proto(wrapper);
        }
    } catch (const std::exception& e) {
        std::cerr << "[MexcOrderStream] parse error: " << e.what() << "\n";
    }
}

void MexcOrderStream::on_close(websocketpp::connection_hdl /*hdl*/) {
    connected_ = false;
}

void MexcOrderStream::on_fail(websocketpp::connection_hdl /*hdl*/) {
    connected_ = false;
    running_   = false;
    std::cerr << "[MexcOrderStream] Connection failed\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Protobuf order update handler
// ─────────────────────────────────────────────────────────────────────────────

void MexcOrderStream::handle_order_proto(const PushDataV3ApiWrapper& wrap) {
    const PrivateOrdersV3Api& o = wrap.privateorders();
    // id()       = proto field 1 (exchange-assigned numeric ID)
    // clientid() = proto field 2 (C02__... client order ID, matches REST orderId)
    if (o.id().empty() && o.clientid().empty()) return;

    OrderEvent ev;
    ev.exchange   = "MEXC";
    ev.order_id   = o.clientid().empty() ? o.id() : o.clientid(); // primary key
    ev.client_id  = o.id();   // secondary — render checks both
    ev.status     = o.status(); // 1=new, 2=filled, 3=partial, 4=cancelled, 5=partial-cancel

    // Full order details needed to surface externally-placed orders
    try { ev.price     = std::stod(o.price());              } catch (...) {}
    try { ev.qty       = std::stod(o.quantity());           } catch (...) {}
    try { ev.cum_qty   = std::stod(o.cumulativequantity()); } catch (...) {}
    try { ev.avg_price = std::stod(o.avgprice());           } catch (...) {}
    // tradeType: 1=BUY, 2=SELL
    ev.side   = (o.tradetype() == 1) ? 0 : 1;
    // Symbol: inner fields are sometimes empty for algo/third-party orders; the
    // PushDataV3ApiWrapper often carries symbol / symbolId at the envelope level.
    auto non_empty = [](const std::string& s) { return !s.empty(); };
    if (o.has_market() && non_empty(o.market())) ev.symbol = o.market();
    else if (o.has_symbolid() && non_empty(o.symbolid())) ev.symbol = o.symbolid();
    else if (o.has_marketid() && non_empty(o.marketid())) ev.symbol = o.marketid();
    else if (wrap.has_symbol() && non_empty(wrap.symbol())) ev.symbol = wrap.symbol();
    else if (wrap.has_symbolid() && non_empty(wrap.symbolid())) ev.symbol = wrap.symbolid();

    // Map to event_type for the render layer
    if      (o.status() == 1) ev.event_type = "new";
    else if (o.status() == 3) ev.event_type = "update";
    else                       ev.event_type = "finish";

    if (events_) events_->push(std::move(ev));
}
