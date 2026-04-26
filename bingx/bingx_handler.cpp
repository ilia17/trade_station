#include "bingx_handler.h"
#include <iostream>
#include <cstring>
#include <ctime>
#include <chrono>
#include <algorithm>
#include <vector>
#include <zlib.h>

BingXHandler::BingXHandler(Disruptor<1024>& disruptor)
    : Exchange("BingX", "BTC-USDT", "wss://open-api-ws.bingx.com/market", disruptor)
{
    client.clear_access_channels(websocketpp::log::alevel::all);
    client.clear_error_channels(websocketpp::log::elevel::all);

    client.init_asio();
    client.set_tls_init_handler([this](websocketpp::connection_hdl) {
        return on_tls_init();
    });

    client.set_open_handler([this](websocketpp::connection_hdl hdl) {
        on_open(hdl);
    });
    client.set_message_handler([this](websocketpp::connection_hdl hdl,
                                       ws_client::message_ptr msg) {
        on_message(hdl, msg);
    });
    client.set_close_handler([this](websocketpp::connection_hdl hdl) {
        on_close(hdl);
    });
    client.set_fail_handler([this](websocketpp::connection_hdl hdl) {
        on_fail(hdl);
    });
}

ssl_context_ptr BingXHandler::on_tls_init() {
    ssl_context_ptr ctx = websocketpp::lib::make_shared<
        boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
    ctx->set_verify_mode(boost::asio::ssl::verify_none);
    return ctx;
}

void BingXHandler::on_open(websocketpp::connection_hdl hdl) {
    connection = hdl;
    running.store(true, std::memory_order_release);
    std::cout << "[BingX] Connected\n";
    subscribe();
    subscribe_trades();
    schedule_ping();
}

void BingXHandler::schedule_ping() {
    client.set_timer(20000, [this](websocketpp::lib::error_code const& ec) {
        if (ec || !running.load()) return;
        json ping = {{"ping", std::time(nullptr)}};
        websocketpp::lib::error_code send_ec;
        client.send(connection, ping.dump(), websocketpp::frame::opcode::text, send_ec);
        if (!send_ec) schedule_ping();
    });
}

void BingXHandler::subscribe() {
    long long ts = std::time(nullptr);
    json sub = {
        {"id",       std::to_string(ts)},
        {"reqType",  "sub"},
        {"dataType", symbol + "@depth20"}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text);
    std::cout << "[BingX] Subscribed to " << symbol << " depth20\n";
}

void BingXHandler::on_message(websocketpp::connection_hdl hdl,
                               ws_client::message_ptr msg) {
    std::string raw = msg->get_payload();

    // BingX sends gzip-compressed frames — decompress first
    if (raw.size() >= 2 &&
        (unsigned char)raw[0] == 0x1f &&
        (unsigned char)raw[1] == 0x8b) {
        try {
            raw = decompress_gzip(raw);
        } catch (const std::exception& e) {
            std::cout << "[BingX] decompress error: " << e.what() << "\n";
            return;
        }
    }

    try {
        json j = json::parse(raw);

        // Server-sent ping — reply with matching pong
        if (j.contains("ping")) {
            json pong = {{"pong", j["ping"]}};
            client.send(hdl, pong.dump(), websocketpp::frame::opcode::text);
            return;
        }

        // Our own ping was acknowledged — nothing to do
        if (j.contains("pong")) return;

        // Subscription ack or other control message
        if (!j.contains("dataType") || !j.contains("data")) {
            std::cout << "[BingX] server msg: " << raw << "\n";
            return;
        }

        const std::string& dataType = j["dataType"];

        // Trade update
        if (dataType.find("@trade") != std::string::npos) {
            if (j.contains("data")) parse_trade(j["data"]);
            return;
        }

        if (dataType.find("@depth") == std::string::npos) return;

        // Guard against stale frames — dataType is "SYMBOL@depth20", check prefix
        std::string expected = symbol + "@depth";
        if (dataType.find(expected) == std::string::npos) return;

        OrderBookUpdate update = parse_snapshot(j["data"]);
        if (update.bid_count > 0 || update.ask_count > 0) {
            disruptor.publish(update);
        }
    } catch (const std::exception& e) {
        std::cout << "[BingX] error: " << e.what() << "\n";
    }
}

// Safely parse a price/qty value that may be a JSON string or a JSON number
static double level_to_double(const nlohmann::json& v) {
    if (v.is_string()) return std::stod(v.get<std::string>());
    return v.get<double>();
}

OrderBookUpdate BingXHandler::parse_snapshot(const json& data) {
    OrderBookUpdate update;
    strncpy(update.exchange, "BingX",        sizeof(update.exchange) - 1);
    strncpy(update.symbol,   symbol.c_str(), sizeof(update.symbol)   - 1);
    update.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Collect into vectors and sort explicitly — BingX sort order varies by pair
    if (data.contains("bids") && data["bids"].is_array()) {
        std::vector<PriceLevel> bids;
        for (const auto& level : data["bids"]) {
            if (!level.is_array() || level.size() < 2) continue;
            bids.push_back({ level_to_double(level[0]), level_to_double(level[1]) });
        }
        // Best bid first (highest price first)
        std::sort(bids.begin(), bids.end(),
                  [](const PriceLevel& a, const PriceLevel& b){ return a.price > b.price; });
        int n = std::min((int)bids.size(), 5);
        for (int i = 0; i < n; i++) update.bids[i] = bids[i];
        update.bid_count = n;
    }

    if (data.contains("asks") && data["asks"].is_array()) {
        std::vector<PriceLevel> asks;
        for (const auto& level : data["asks"]) {
            if (!level.is_array() || level.size() < 2) continue;
            asks.push_back({ level_to_double(level[0]), level_to_double(level[1]) });
        }
        // Best ask first (lowest price first)
        std::sort(asks.begin(), asks.end(),
                  [](const PriceLevel& a, const PriceLevel& b){ return a.price < b.price; });
        int n = std::min((int)asks.size(), 5);
        for (int i = 0; i < n; i++) update.asks[i] = asks[i];
        update.ask_count = n;
    }

    return update;
}

std::string BingXHandler::decompress_gzip(const std::string& compressed) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK)
        throw std::runtime_error("inflateInit2 failed");

    zs.next_in  = (Bytef*)compressed.data();
    zs.avail_in = (uInt)compressed.size();

    std::string out;
    char buf[32768];
    int ret;
    do {
        zs.next_out  = (Bytef*)buf;
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, 0);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret == Z_OK);

    inflateEnd(&zs);
    if (ret != Z_STREAM_END)
        throw std::runtime_error("gzip decompression failed");
    return out;
}

void BingXHandler::on_close(websocketpp::connection_hdl hdl) {
    running.store(false, std::memory_order_release);
    auto con = client.get_con_from_hdl(hdl);
    std::cout << "[BingX] Disconnected — code=" << con->get_remote_close_code()
              << " reason=" << con->get_remote_close_reason() << "\n";
}

void BingXHandler::on_fail(websocketpp::connection_hdl hdl) {
    running.store(false, std::memory_order_release);
    std::cout << "[BingX] Connection failed\n";
}

void BingXHandler::connect() {
    websocketpp::lib::error_code ec;
    ws_client::connection_ptr con = client.get_connection(ws_url, ec);
    if (ec) {
        std::cout << "[BingX] Connection error: " << ec.message() << "\n";
        return;
    }
    client.connect(con);
}

void BingXHandler::disconnect() {
    running.store(false, std::memory_order_release);
    client.stop();
}

void BingXHandler::run() {
    connect();
    client.run();
}

void BingXHandler::subscribe_trades() {
    long long ts = std::time(nullptr);
    json sub = {
        {"id",       std::to_string(ts)},
        {"reqType",  "sub"},
        {"dataType", symbol + "@trade"}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text);
}

void BingXHandler::parse_trade(const json& data) {
    if (!shared_trades) return;
    Trade t;
    auto to_d = [](const json& v) -> double {
        return v.is_string() ? std::stod(v.get<std::string>()) : v.get<double>();
    };
    if (data.contains("p")) t.price = to_d(data["p"]);
    if (data.contains("q")) t.qty   = to_d(data["q"]);
    // m = isBuyerMaker: true→seller aggressive→SELL; false→buyer aggressive→BUY
    t.is_buy  = data.contains("m") && !data["m"].get<bool>();
    t.time_ms = data.contains("T") ? data["T"].get<long long>() : 0;
    shared_trades->add(name.c_str(), t);
}

void BingXHandler::change_symbol(const std::string& base, const std::string& quote) {
    if (!running.load()) return;
    long long ts = std::time(nullptr);
    std::string new_sym = base + "-" + quote;  // BingX: BASE-QUOTE, e.g. ETH-USDT
    std::string old_sym = symbol;
    // Unsubscribe old depth + trades
    json unsub = {{"id",std::to_string(ts)},{"reqType","unsub"},{"dataType",old_sym+"@depth20"}};
    websocketpp::lib::error_code ec;
    client.send(connection, unsub.dump(), websocketpp::frame::opcode::text, ec);
    symbol = new_sym;
    // Subscribe new
    json sub = {
        {"id",       std::to_string(ts + 1)},
        {"reqType",  "sub"},
        {"dataType", symbol + "@depth20"}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text, ec);
    // Resubscribe trades
    long long ts2 = std::time(nullptr);
    json tunsub = {{"id",std::to_string(ts2)},{"reqType","unsub"},{"dataType",old_sym+"@trade"}};
    client.send(connection, tunsub.dump(), websocketpp::frame::opcode::text, ec);
    json tsub = {{"id",std::to_string(ts2+1)},{"reqType","sub"},{"dataType",symbol+"@trade"}};
    client.send(connection, tsub.dump(), websocketpp::frame::opcode::text, ec);
    std::cout << "[BingX] Resubscribed to " << symbol << "\n";
}
