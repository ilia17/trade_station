#include "gate_handler.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <algorithm>

GateHandler::GateHandler(Disruptor<1024>& disruptor)
    : Exchange("Gate", "BTC_USDT", "wss://api.gateio.ws/ws/v4/", disruptor)
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

ssl_context_ptr GateHandler::on_tls_init() {
    ssl_context_ptr ctx = websocketpp::lib::make_shared<
        boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
    ctx->set_verify_mode(boost::asio::ssl::verify_none);
    return ctx;
}

void GateHandler::on_open(websocketpp::connection_hdl hdl) {
    connection = hdl;
    running.store(true, std::memory_order_release);
    std::cout << "[Gate] Connected\n";
    subscribe();
    subscribe_trades();
    schedule_ping();
}

void GateHandler::schedule_ping() {
    client.set_timer(10000, [this](websocketpp::lib::error_code const& ec) {
        if (ec || !running.load()) return;
        long long ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json ping = {{"time", ts}, {"channel", "spot.ping"}};
        websocketpp::lib::error_code send_ec;
        client.send(connection, ping.dump(), websocketpp::frame::opcode::text, send_ec);
        if (!send_ec) schedule_ping();
    });
}

void GateHandler::subscribe() {
    long long ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // spot.order_book sends full snapshots at 100ms cadence, depth=5
    json sub = {
        {"time",    ts},
        {"channel", "spot.order_book"},
        {"event",   "subscribe"},
        {"payload", json::array({symbol, "5", "100ms"})}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text);
    std::cout << "[Gate] Subscribed to " << symbol << " order book\n";
}

void GateHandler::on_message(websocketpp::connection_hdl hdl,
                              ws_client::message_ptr msg) {
    const std::string& raw = msg->get_payload();
    try {
        json j = json::parse(raw);

        // Acks and pings — not data; suppress noisy spot.pong acks
        if (!j.contains("event") || j["event"] != "update") {
            if (j.contains("channel") && j["channel"] == "spot.pong") return;
            std::cout << "[Gate] server msg: " << raw << "\n";
            return;
        }
        if (j.contains("channel") && j["channel"] == "spot.trades" &&
            j.contains("result")) {
            parse_trade(j["result"]);
            return;
        }
        if (!j.contains("channel") || j["channel"] != "spot.order_book") return;
        if (!j.contains("result")) return;

        // Guard against stale frames from the old subscription
        if (j.contains("result") && j["result"].contains("currency_pair") &&
            j["result"]["currency_pair"].get<std::string>() != symbol) return;

        OrderBookUpdate update = parse_snapshot(j["result"]);
        if (update.bid_count > 0 || update.ask_count > 0) {
            disruptor.publish(update);
        }
    } catch (const std::exception& e) {
        std::cout << "[Gate] error: " << e.what() << "\n";
    }
}

OrderBookUpdate GateHandler::parse_snapshot(const json& result) {
    OrderBookUpdate update;
    strncpy(update.exchange, "Gate",         sizeof(update.exchange) - 1);
    strncpy(update.symbol,   symbol.c_str(), sizeof(update.symbol)  - 1);
    update.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto to_d = [](const json& v) -> double {
        return v.is_string() ? std::stod(v.get<std::string>()) : v.get<double>();
    };

    // Gate sends full snapshots — bids are already sorted best-first
    if (result.contains("bids")) {
        int i = 0;
        for (const auto& level : result["bids"]) {
            if (i >= 5) break;
            if (!level.is_array() || level.size() < 2) continue;
            update.bids[i++] = { to_d(level[0]), to_d(level[1]) };
        }
        update.bid_count = i;
    }

    if (result.contains("asks")) {
        int i = 0;
        for (const auto& level : result["asks"]) {
            if (i >= 5) break;
            if (!level.is_array() || level.size() < 2) continue;
            update.asks[i++] = { to_d(level[0]), to_d(level[1]) };
        }
        update.ask_count = i;
    }

    return update;
}

void GateHandler::on_close(websocketpp::connection_hdl hdl) {
    running.store(false, std::memory_order_release);
    auto con = client.get_con_from_hdl(hdl);
    std::cout << "[Gate] Disconnected — code=" << con->get_remote_close_code()
              << " reason=" << con->get_remote_close_reason() << "\n";
}

void GateHandler::on_fail(websocketpp::connection_hdl hdl) {
    running.store(false, std::memory_order_release);
    std::cout << "[Gate] Connection failed\n";
}

void GateHandler::connect() {
    websocketpp::lib::error_code ec;
    ws_client::connection_ptr con = client.get_connection(ws_url, ec);
    if (ec) {
        std::cout << "[Gate] Connection error: " << ec.message() << "\n";
        return;
    }
    client.connect(con);
}

void GateHandler::disconnect() {
    running.store(false, std::memory_order_release);
    client.stop();
}

void GateHandler::run() {
    connect();
    client.run();
}

void GateHandler::subscribe_trades() {
    long long ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    json sub = {
        {"time", ts}, {"channel", "spot.trades"},
        {"event", "subscribe"}, {"payload", json::array({symbol})}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text);
}

void GateHandler::parse_trade(const json& result) {
    if (!shared_trades) return;
    // result may be a single object or an array
    auto process = [&](const json& r) {
        Trade t;
        if (r.contains("price"))  t.price  = std::stod(r["price"].get<std::string>());
        if (r.contains("amount")) t.qty    = std::stod(r["amount"].get<std::string>());
        t.is_buy = r.contains("side") && r["side"].get<std::string>() == "buy";
        if (r.contains("create_time_ms"))
            t.time_ms = std::stoll(r["create_time_ms"].get<std::string>());
        shared_trades->add(name.c_str(), t);
    };
    if (result.is_array()) { for (const auto& r : result) process(r); }
    else process(result);
}

void GateHandler::change_symbol(const std::string& base, const std::string& quote) {
    if (!running.load()) return;
    long long ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string new_sym = base + "_" + quote;  // Gate: BASE_QUOTE, e.g. ETH_USDT
    std::string old_sym = symbol;
    std::string new_sym_old = old_sym; // alias for the trades unsub below
    // Unsubscribe old order book
    json unsub = {
        {"time",    ts},
        {"channel", "spot.order_book"},
        {"event",   "unsubscribe"},
        {"payload", json::array({old_sym, "5", "100ms"})}
    };
    websocketpp::lib::error_code ec;
    client.send(connection, unsub.dump(), websocketpp::frame::opcode::text, ec);
    symbol = new_sym;
    // Subscribe new
    json sub = {
        {"time",    ts},
        {"channel", "spot.order_book"},
        {"event",   "subscribe"},
        {"payload", json::array({symbol, "5", "100ms"})}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text, ec);
    // Resubscribe trades
    long long ts2 = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    json tunsub = {{"time",ts2},{"channel","spot.trades"},{"event","unsubscribe"},{"payload",json::array({new_sym_old})}};
    client.send(connection, tunsub.dump(), websocketpp::frame::opcode::text, ec);
    json tsub = {{"time",ts2+1},{"channel","spot.trades"},{"event","subscribe"},{"payload",json::array({symbol})}};
    client.send(connection, tsub.dump(), websocketpp::frame::opcode::text, ec);
    std::cout << "[Gate] Resubscribed to " << symbol << "\n";
}
