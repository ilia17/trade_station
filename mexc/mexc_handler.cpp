#include "mexc_handler.h"
#include <iostream>
#include <cstring>
#include <chrono>

MexcHandler::MexcHandler(Disruptor<1024>& disruptor)
    : Exchange("MEXC", "BTCUSDT", "wss://wbs-api.mexc.com/ws", disruptor)
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

ssl_context_ptr MexcHandler::on_tls_init() {
    ssl_context_ptr ctx = websocketpp::lib::make_shared<
        boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
    ctx->set_verify_mode(boost::asio::ssl::verify_none);
    return ctx;
}

void MexcHandler::on_open(websocketpp::connection_hdl hdl) {
    connection = hdl;
    running.store(true, std::memory_order_release);
    subscribe();
    subscribe_trades();
}

void MexcHandler::schedule_ping() {
    client.set_timer(20000, [this](websocketpp::lib::error_code const& ec) {
        if (ec || !running.load()) return;
        json ping = {{"msg", "ping"}};
        websocketpp::lib::error_code send_ec;
        client.send(connection, ping.dump(), websocketpp::frame::opcode::text, send_ec);
        if (!send_ec) schedule_ping();
    });
}

void MexcHandler::subscribe() {
    // Protobuf channel — unblocked on MEXC, 100ms cadence
    json sub = {
        {"method", "SUBSCRIPTION"},
        {"params", {"spot@public.aggre.depth.v3.api.pb@100ms@" + symbol}}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text);
}

void MexcHandler::on_message(websocketpp::connection_hdl hdl,
                              ws_client::message_ptr msg) {
    const std::string& raw = msg->get_payload();

    // JSON frames: subscription acks and server-sent pings
    if (!raw.empty() && raw[0] == '{') {
        try {
            json j = json::parse(raw);
            if (j.contains("msg") && j["msg"] == "ping") {
                json pong = {{"msg", "pong"}};
                client.send(hdl, pong.dump(), websocketpp::frame::opcode::text);
                return;
            }
            if (j.contains("msg") && j["msg"].is_string()) {
                std::string ack_msg = j["msg"].get<std::string>();
                // Only the depth protobuf ack clears the transition gate
                std::string depth_ch = "spot@public.aggre.depth.v3.api.pb@100ms@" + symbol;
                if (ack_msg.find(depth_ch) != std::string::npos) {
                    bids_book.clear();
                    asks_book.clear();
                    awaiting_sub_ack.store(false, std::memory_order_release);
                }
                // Suppress channel subscription/unsubscription acks — they are noisy
                if (ack_msg.find("spot@") != std::string::npos) return;
            }
            // Silence pong echoes too
            if (j.contains("msg") && j["msg"] == "pong") return;
            return;
        } catch (...) {}
        return;
    }

    // Binary frame — decode as PushDataV3ApiWrapper protobuf.
    // Discard while a symbol switch is in progress (old-symbol leftovers).
    if (awaiting_sub_ack.load(std::memory_order_acquire)) return;

    try {
        PushDataV3ApiWrapper wrapper;
        if (!wrapper.ParseFromString(raw)) {
            return;
        }

        if (wrapper.has_publicaggredepths()) {
            OrderBookUpdate update = apply_depth(wrapper.publicaggredepths());
            if (update.bid_count > 0 || update.ask_count > 0) {
                disruptor.publish(update);
            }
        }

        if (wrapper.has_publicaggredeals()) {
            // Guard: only accept trades for the currently subscribed symbol
            if (!wrapper.has_symbol() || wrapper.symbol() == symbol)
                parse_trade_pb(wrapper.publicaggredeals());
        }
    } catch (const std::exception& e) {
        std::cerr << "[MEXC] error: " << e.what() << "\n";
    }
}

OrderBookUpdate MexcHandler::apply_depth(const PublicAggreDepthsV3Api& depths) {
    // Merge incremental deltas into the in-memory book maps.
    // qty > 0 → set/update the level; qty == 0 → remove it.
    for (int i = 0; i < depths.bids_size(); i++) {
        double price = std::stod(depths.bids(i).price());
        double qty   = std::stod(depths.bids(i).quantity());
        if (qty == 0.0)
            bids_book.erase(price);
        else
            bids_book[price] = qty;
    }
    for (int i = 0; i < depths.asks_size(); i++) {
        double price = std::stod(depths.asks(i).price());
        double qty   = std::stod(depths.asks(i).quantity());
        if (qty == 0.0)
            asks_book.erase(price);
        else
            asks_book[price] = qty;
    }

    // Build a top-5 snapshot from the sorted maps.
    OrderBookUpdate update;
    strncpy(update.exchange, "MEXC",          sizeof(update.exchange) - 1);
    strncpy(update.symbol,   symbol.c_str(),  sizeof(update.symbol)   - 1);
    update.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    int i = 0;
    for (auto& [price, qty] : bids_book) {
        if (i >= 5) break;
        update.bids[i++] = {price, qty};
    }
    update.bid_count = i;

    i = 0;
    for (auto& [price, qty] : asks_book) {
        if (i >= 5) break;
        update.asks[i++] = {price, qty};
    }
    update.ask_count = i;

    return update;
}

void MexcHandler::on_close(websocketpp::connection_hdl hdl) {
    running.store(false, std::memory_order_release);
}

void MexcHandler::on_fail(websocketpp::connection_hdl hdl) {
    running.store(false, std::memory_order_release);
}

void MexcHandler::connect() {
    websocketpp::lib::error_code ec;
    ws_client::connection_ptr con = client.get_connection(ws_url, ec);
    if (ec) {
        std::cerr << "[MEXC] Connection error: " << ec.message() << "\n";
        return;
    }
    client.connect(con);
}

void MexcHandler::disconnect() {
    running.store(false, std::memory_order_release);
    client.stop();
}

void MexcHandler::run() {
    connect();
    client.run();
}

void MexcHandler::subscribe_trades() {
    // Channel: spot@public.aggre.deals.v3.api.pb@100ms@SYMBOL (JSON text frame)
    json sub = {
        {"method", "SUBSCRIPTION"},
        {"params", {"spot@public.aggre.deals.v3.api.pb@100ms@" + symbol}}
    };
    client.send(connection, sub.dump(), websocketpp::frame::opcode::text);
}

void MexcHandler::parse_trade_pb(const PublicAggreDealsV3Api& deals) {
    if (!shared_trades) return;
    for (int i = 0; i < deals.deals_size(); i++) {
        const auto& d = deals.deals(i);
        Trade t;
        try { t.price = std::stod(d.price());    } catch (...) {}
        try { t.qty   = std::stod(d.quantity()); } catch (...) {}
        t.is_buy  = (d.tradetype() == 1);   // 1=buy, 2=sell
        t.time_ms = d.time();
        shared_trades->add(name.c_str(), t);
    }
}

void MexcHandler::change_symbol(const std::string& base, const std::string& quote) {
    if (!running.load()) return;
    std::string old_sym = symbol;
    std::string new_sym = base + quote;  // MEXC: no separator, e.g. ETHUSDT

    websocketpp::lib::error_code ec;

    // Unsubscribe old depth + deals
    json unsub_depth = {{"method","UNSUBSCRIPTION"},
        {"params", {"spot@public.aggre.depth.v3.api.pb@100ms@" + old_sym}}};
    json unsub_deals = {{"method","UNSUBSCRIPTION"},
        {"params", {"spot@public.aggre.deals.v3.api.pb@100ms@" + old_sym}}};
    client.send(connection, unsub_depth.dump(), websocketpp::frame::opcode::text, ec);
    client.send(connection, unsub_deals.dump(), websocketpp::frame::opcode::text, ec);

    // Gate: discard all binary frames until the new depth subscription is acked
    awaiting_sub_ack.store(true, std::memory_order_release);
    bids_book.clear();
    asks_book.clear();
    symbol = new_sym;

    // Subscribe new depth + deals
    json sub_depth = {{"method","SUBSCRIPTION"},
        {"params", {"spot@public.aggre.depth.v3.api.pb@100ms@" + symbol}}};
    json sub_deals = {{"method","SUBSCRIPTION"},
        {"params", {"spot@public.aggre.deals.v3.api.pb@100ms@" + symbol}}};
    client.send(connection, sub_depth.dump(), websocketpp::frame::opcode::text, ec);
    client.send(connection, sub_deals.dump(), websocketpp::frame::opcode::text, ec);
}
