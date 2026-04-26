#include "lbank_handler.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <vector>

LBankHandler::LBankHandler(Disruptor<1024>& disruptor)
    : Exchange("LBank", "btc_usdt", "wss://www.lbkex.net/ws/V2/", disruptor)
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

ssl_context_ptr LBankHandler::on_tls_init() {
    ssl_context_ptr ctx = websocketpp::lib::make_shared<
        boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
    ctx->set_verify_mode(boost::asio::ssl::verify_none);
    return ctx;
}

void LBankHandler::on_open(websocketpp::connection_hdl hdl) {
    connection = hdl;
    running.store(true, std::memory_order_release);
    std::cout << "[LBank] Connected\n";
    subscribe();
    subscribe_trades();
    schedule_ping();
}

void LBankHandler::schedule_ping() {
    client.set_timer(20000, [this](websocketpp::lib::error_code const& ec) {
        if (ec || !running.load()) return;
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // LBank V2 requires both "action" and the "ping" timestamp field
        json ping = {{"action", "ping"}, {"ping", ts}};
        websocketpp::lib::error_code send_ec;
        client.send(connection, ping.dump(), websocketpp::frame::opcode::text, send_ec);
        if (!send_ec) schedule_ping();
    });
}

void LBankHandler::subscribe() {
    json sub = {
        {"action",    "subscribe"},
        {"subscribe", "depth"},
        {"depth",     20},
        {"pair",      symbol}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text);
    std::cout << "[LBank] Subscribed to " << symbol << " depth\n";
}

void LBankHandler::on_message(websocketpp::connection_hdl hdl,
                               ws_client::message_ptr msg) {
    const std::string& raw = msg->get_payload();
    try {
        json j = json::parse(raw);

        // Server-sent ping — reply with action + pong field (LBank V2 requirement)
        if (j.contains("ping")) {
            json pong = {{"action", "pong"}, {"pong", j["ping"]}};
            client.send(hdl, pong.dump(), websocketpp::frame::opcode::text);
            return;
        }

        // Our own ping was acknowledged
        if (j.contains("pong")) return;

        // Trade update
        if (j.contains("type") && j["type"] == "trade") {
            parse_trade(j);
            return;
        }

        // Depth snapshot — guard against stale frames from the old subscription
        if (j.contains("type") && j["type"] == "depth" && j.contains("depth")) {
            // LBank includes "pair" in the depth update; skip if it doesn't match
            // the current subscription (avoids showing BTC data after a symbol switch)
            if (j.contains("pair") && j["pair"].get<std::string>() != symbol) {
                return;
            }
            OrderBookUpdate update = parse_snapshot(j["depth"]);
            if (update.bid_count > 0 || update.ask_count > 0) {
                disruptor.publish(update);
            }
            return;
        }

        // Anything else — subscription acks etc.
        std::cout << "[LBank] server msg: " << raw << "\n";

    } catch (const std::exception& e) {
        std::cout << "[LBank] error: " << e.what() << "\n";
    }
}

OrderBookUpdate LBankHandler::parse_snapshot(const json& depth) {
    OrderBookUpdate update;
    strncpy(update.exchange, "LBank",        sizeof(update.exchange) - 1);
    strncpy(update.symbol,   symbol.c_str(), sizeof(update.symbol)   - 1);
    update.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto to_d = [](const json& v) -> double {
        return v.is_string() ? std::stod(v.get<std::string>()) : v.get<double>();
    };

    // LBank sends unsorted arrays — collect, sort, then take top 5
    if (depth.contains("bids") && depth["bids"].is_array()) {
        std::vector<PriceLevel> bids;
        for (const auto& level : depth["bids"]) {
            if (level.is_array() && level.size() >= 2)
                bids.push_back({ to_d(level[0]), to_d(level[1]) });
        }
        std::sort(bids.begin(), bids.end(),
                  [](const PriceLevel& a, const PriceLevel& b){ return a.price > b.price; });
        int n = std::min((int)bids.size(), 5);
        for (int i = 0; i < n; i++) update.bids[i] = bids[i];
        update.bid_count = n;
    }

    if (depth.contains("asks") && depth["asks"].is_array()) {
        std::vector<PriceLevel> asks;
        for (const auto& level : depth["asks"]) {
            if (level.is_array() && level.size() >= 2)
                asks.push_back({ to_d(level[0]), to_d(level[1]) });
        }
        std::sort(asks.begin(), asks.end(),
                  [](const PriceLevel& a, const PriceLevel& b){ return a.price < b.price; });
        int n = std::min((int)asks.size(), 5);
        for (int i = 0; i < n; i++) update.asks[i] = asks[i];
        update.ask_count = n;
    }

    return update;
}

void LBankHandler::on_close(websocketpp::connection_hdl hdl) {
    running.store(false, std::memory_order_release);
    auto con = client.get_con_from_hdl(hdl);
    std::cout << "[LBank] Disconnected — code=" << con->get_remote_close_code()
              << " reason=" << con->get_remote_close_reason() << "\n";
}

void LBankHandler::on_fail(websocketpp::connection_hdl hdl) {
    running.store(false, std::memory_order_release);
    std::cout << "[LBank] Connection failed\n";
}

void LBankHandler::connect() {
    websocketpp::lib::error_code ec;
    ws_client::connection_ptr con = client.get_connection(ws_url, ec);
    if (ec) {
        std::cout << "[LBank] Connection error: " << ec.message() << "\n";
        return;
    }
    client.connect(con);
}

void LBankHandler::disconnect() {
    running.store(false, std::memory_order_release);
    client.stop();
}

void LBankHandler::run() {
    connect();
    client.run();
}

void LBankHandler::subscribe_trades() {
    json sub = {
        {"action",    "subscribe"},
        {"subscribe", "trade"},
        {"pair",      symbol}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text);
}

void LBankHandler::parse_trade(const json& j) {
    if (!shared_trades) return;
    auto process = [&](const json& t) {
        Trade tr;
        auto to_d = [](const json& v) -> double {
            return v.is_string() ? std::stod(v.get<std::string>()) : v.get<double>();
        };
        if (t.contains("price"))  tr.price = to_d(t["price"]);
        // LBank V2 sends quantity as "amount"; "vol" is a fallback
        if      (t.contains("amount")) tr.qty = to_d(t["amount"]);
        else if (t.contains("vol"))    tr.qty = to_d(t["vol"]);
        else if (t.contains("volume")) tr.qty = to_d(t["volume"]);
        tr.is_buy = t.contains("direction") &&
                    t["direction"].get<std::string>() == "buy";
        shared_trades->add(name.c_str(), tr);
    };
    // "trade" field may be object or array
    if (j.contains("trade")) {
        if (j["trade"].is_array()) { for (const auto& t : j["trade"]) process(t); }
        else process(j["trade"]);
    }
}

void LBankHandler::change_symbol(const std::string& base, const std::string& quote) {
    if (!running.load()) return;
    // LBank: base_quote lowercase, e.g. eth_usdt
    std::string b = base, q = quote;
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    std::string new_sym = b + "_" + q;
    std::string old_sym = symbol;
    // Unsubscribe old — LBank V2 requires "depth" field even in unsubscribe
    json unsub = {
        {"action",    "unsubscribe"},
        {"subscribe", "depth"},
        {"depth",     20},
        {"pair",      old_sym}
    };
    websocketpp::lib::error_code ec;
    client.send(connection, unsub.dump(), websocketpp::frame::opcode::text, ec);
    symbol = new_sym;
    // Subscribe new
    json sub = {
        {"action",    "subscribe"},
        {"subscribe", "depth"},
        {"depth",     20},
        {"pair",      symbol}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text, ec);
    // Resubscribe trades
    json tunsub = {{"action","unsubscribe"},{"subscribe","trade"},{"pair",old_sym}};
    client.send(connection, tunsub.dump(), websocketpp::frame::opcode::text, ec);
    json tsub = {{"action","subscribe"},{"subscribe","trade"},{"pair",symbol}};
    client.send(connection, tsub.dump(), websocketpp::frame::opcode::text, ec);
    std::cout << "[LBank] Resubscribed to " << symbol << "\n";
}
