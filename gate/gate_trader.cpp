#include "gate_trader.h"
#include "../trading/rest_client.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <iostream>
#include <atomic>
#include <thread>
#include <mutex>
#include <map>

// Beast / ASIO WebSocket (synchronous blocking pattern)
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = boost::asio::ssl;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

// ============================================================================
// GateWsImpl — owns the persistent WebSocket connection + background threads
// ============================================================================

struct GateWsImpl {
    using ws_t = websocket::stream<beast::ssl_stream<tcp::socket>>;

    const std::string  api_key;
    const std::string  api_secret;
    SharedOrderEvents* events;   // may be nullptr

    net::io_context   ioc;
    ssl::context      ctx{ssl::context::tlsv12_client};
    std::unique_ptr<ws_t> ws;

    std::atomic<bool> connected{false};
    std::atomic<bool> authenticated{false};
    std::atomic<bool> running{false};
    std::atomic<int>  req_counter{0};

    mutable std::mutex write_mtx;

    // client_id ("t-<req_id>") → Gate exchange order ID
    std::map<std::string, std::string> order_id_map;
    std::mutex order_id_map_mtx;

    std::thread listen_thread;
    std::thread ping_thread;

    // ── helpers ──────────────────────────────────────────────────────────────

    std::string generate_req_id() {
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return std::to_string(ms) + "-" + std::to_string(++req_counter);
    }

    static long long now_secs() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Gate WS API-command signature: HMAC-SHA512("api\n<channel>\n<req_param>\n<ts>")
    std::string sign(const std::string& channel, const std::string& req_param,
                     long long ts) {
        std::string data = "api\n" + channel + "\n" + req_param + "\n" + std::to_string(ts);
        return hmac_sha512_hex(api_secret, data);
    }

    // Gate WS subscription-channel signature: HMAC-SHA512("channel={ch}&event=subscribe&time={ts}")
    std::string sub_sign(const std::string& channel, long long ts) {
        std::string data = "channel=" + channel + "&event=subscribe&time=" + std::to_string(ts);
        return hmac_sha512_hex(api_secret, data);
    }

    // Subscribe to spot.orders with "!all" to receive updates for any currency pair
    void subscribe_orders() {
        long long ts = now_secs();
        json msg = {
            {"time",    ts},
            {"channel", "spot.orders"},
            {"event",   "subscribe"},
            {"payload", {"!all"}},
            {"auth", {
                {"method", "api_key"},
                {"KEY",    api_key},
                {"SIGN",   sub_sign("spot.orders", ts)}
            }}
        };
        ws_write(msg.dump());
    }

    void ws_write(const std::string& msg) {
        std::lock_guard<std::mutex> lk(write_mtx);
        if (ws && connected) {
            try { ws->write(net::buffer(msg)); }
            catch (...) { connected = false; }
        }
    }

    void send_ping() {
        json ping = {{"time", now_secs()}, {"channel", "spot.ping"}};
        ws_write(ping.dump());
    }

    void handle_message(const std::string& msg) {
        try {
            auto j = json::parse(msg);

            // Respond to server-initiated ping
            if (j.contains("channel") && j["channel"] == "spot.ping") {
                json pong = {{"channel", "spot.pong"}, {"time", now_secs()}};
                ws_write(pong.dump());
                return;
            }

            // spot.orders subscription update (standard subscription format — no header)
            if (j.value("channel", "") == "spot.orders" &&
                j.value("event", "") == "update" &&
                j.contains("result") && events) {
                for (auto& o : j["result"]) {
                    std::string ev_type   = o.value("event",     "");   // put / update / finish
                    std::string finish_as = o.value("finish_as", "");   // filled / cancelled / open
                    std::string gate_id   = o.value("id",   "");
                    std::string client_id = o.value("text", "");

                    if (ev_type.empty() || gate_id.empty()) continue;

                    OrderEvent ev;
                    ev.exchange  = "Gate";
                    ev.order_id  = gate_id;
                    ev.client_id = client_id;
                    ev.finish_as = finish_as;
                    ev.symbol    = o.value("currency_pair", "");

                    if (ev_type == "put") {
                        // New order created (by us or an external algo)
                        ev.event_type = "new";
                        std::string side_str = o.value("side", "buy");
                        ev.side = (side_str == "buy") ? 0 : 1;
                        try { ev.price = std::stod(o.value("price",  "0")); } catch (...) {}
                        try { ev.qty   = std::stod(o.value("amount", "0")); } catch (...) {}

                    } else if (ev_type == "update") {
                        // Partial fill
                        ev.event_type = "update";
                        try { ev.avg_price = std::stod(o.value("avg_deal_price", "0")); } catch (...) {}
                        try {
                            double amount = std::stod(o.value("amount", "0"));
                            double left   = std::stod(o.value("left",   "0"));
                            ev.cum_qty = amount - left;
                        } catch (...) {}

                    } else if (ev_type == "finish") {
                        // Order terminal — filled or cancelled
                        ev.event_type = "finish";
                        try { ev.avg_price = std::stod(o.value("avg_deal_price", "0")); } catch (...) {}
                        try {
                            double amount = std::stod(o.value("amount", "0"));
                            double left   = std::stod(o.value("left",   "0"));
                            ev.cum_qty = amount - left;
                        } catch (...) {}

                        // Keep order_id_map clean
                        if (!client_id.empty() && !gate_id.empty()) {
                            std::lock_guard<std::mutex> lk(order_id_map_mtx);
                            order_id_map.erase(client_id);
                        }
                    } else {
                        continue;
                    }

                    events->push(std::move(ev));
                }
                return;
            }

            if (!j.contains("header")) return;
            auto& hdr = j["header"];
            std::string chan = hdr.value("channel", "");

            // Login confirmation
            if (chan == "spot.login") {
                if (j.contains("data") && j["data"].contains("result") &&
                    j["data"]["result"].contains("uid")) {
                    authenticated = true;
                    // Subscribe to real-time order updates after authentication
                    if (events) subscribe_orders();
                }
                return;
            }

            // Order-place response — store client_id → exchange order_id mapping
            if (chan == "spot.order_place") {
                if (j.contains("data") && j["data"].contains("result")) {
                    auto& res = j["data"]["result"];
                    if (res.contains("id") && res.contains("text")) {
                        std::string gate_id = res["id"].is_string()
                            ? res["id"].get<std::string>()
                            : std::to_string(res["id"].get<long long>());
                        std::string client_id = res["text"].get<std::string>();
                        std::lock_guard<std::mutex> lk(order_id_map_mtx);
                        order_id_map[client_id] = gate_id;
                    }
                }
                // Log errors
                if (j.contains("data") && j["data"].contains("errs")) {
                    auto& errs = j["data"]["errs"];
                    std::cerr << "[GateWS] Order rejected: "
                              << errs.value("label", "") << " "
                              << errs.value("message", "") << "\n";
                }
                return;
            }

            // Cancel response
            if (chan == "spot.order_cancel") {
                if (j.contains("data") && j["data"].contains("result")) {
                    auto& res = j["data"]["result"];
                    if (res.contains("text")) {
                        std::lock_guard<std::mutex> lk(order_id_map_mtx);
                        order_id_map.erase(res["text"].get<std::string>());
                    }
                }
                if (j.contains("data") && j["data"].contains("errs")) {
                    auto& errs = j["data"]["errs"];
                    std::cerr << "[GateWS] Cancel rejected: "
                              << errs.value("message", "") << "\n";
                }
            }
        } catch (...) {}
    }

    void listen() {
        beast::flat_buffer buf;
        while (running && connected) {
            try {
                buf.clear();
                ws->read(buf);
                handle_message(beast::buffers_to_string(buf.data()));
            } catch (...) { break; }
        }
        connected     = false;
        authenticated = false;
    }

    // ── lifecycle ─────────────────────────────────────────────────────────────

    GateWsImpl(const std::string& key, const std::string& secret,
               SharedOrderEvents* ev)
        : api_key(key), api_secret(secret), events(ev)
    {
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_none);
        try_connect();
    }

    ~GateWsImpl() { shutdown(); }

    void try_connect() {
        try {
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve("api.gateio.ws", "443");

            ws = std::make_unique<ws_t>(ioc, ctx);

            if (!SSL_set_tlsext_host_name(ws->next_layer().native_handle(),
                                          "api.gateio.ws")) {
                boost::system::error_code ec{static_cast<int>(::ERR_get_error()),
                                             net::error::get_ssl_category()};
                throw boost::system::system_error{ec, "Failed to set SNI"};
            }

            net::connect(beast::get_lowest_layer(*ws), results);

            // Disable Nagle for lower latency order placement
            beast::get_lowest_layer(*ws).set_option(
                boost::asio::ip::tcp::no_delay(true));

            ws->next_layer().handshake(ssl::stream_base::client);
            ws->handshake("api.gateio.ws", "/ws/v4/");

            // We manage keep-alive ourselves; disable Beast idle-timeout pings
            websocket::stream_base::timeout topt{
                std::chrono::seconds(90),
                websocket::stream_base::none(),
                false
            };
            ws->set_option(topt);

            connected = true;
            running   = true;

            // Login
            long long ts       = now_secs();
            std::string req_id = generate_req_id();
            json login = {
                {"time",    ts},
                {"channel", "spot.login"},
                {"event",   "api"},
                {"payload", {
                    {"api_key",   api_key},
                    {"signature", sign("spot.login", "", ts)},
                    {"timestamp", std::to_string(ts)},
                    {"req_id",    req_id}
                }}
            };
            ws_write(login.dump());

            // Block briefly to collect the auth reply before returning
            beast::flat_buffer buf;
            ws->read(buf);
            handle_message(beast::buffers_to_string(buf.data()));

            // Background threads
            listen_thread = std::thread([this]{ listen(); });
            ping_thread   = std::thread([this]{
                int elapsed = 0;
                while (running) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (!running) break;
                    elapsed += 500;
                    if (elapsed >= 10000) { elapsed = 0; send_ping(); }
                }
            });

        } catch (const std::exception& e) {
            std::cerr << "[GateWS] Connect failed: " << e.what() << "\n";
            connected     = false;
            authenticated = false;
        }
    }

    void shutdown() {
        if (!running) return;
        running       = false;
        authenticated = false;

        // Unblock the blocking ws->read() in listen_thread by closing the socket
        if (ws) {
            try {
                boost::system::error_code ec;
                beast::get_lowest_layer(*ws).close(ec);
            } catch (...) {}
        }
        if (ping_thread.joinable())   ping_thread.join();
        if (listen_thread.joinable()) listen_thread.join();
        ws.reset();
        connected = false;
    }

    // ── order operations (WebSocket) ─────────────────────────────────────────

    OrderResult place_order_ws(Side side, const std::string& symbol,
                               double price, double qty) {
        try {
            long long ts       = now_secs();
            std::string req_id = generate_req_id();
            std::string chan   = "spot.order_place";

            json param = {
                {"text",          "t-" + req_id},
                {"currency_pair", symbol},
                {"type",          "limit"},
                {"account",       "spot"},
                {"side",          side == Side::BUY ? "buy" : "sell"},
                {"amount",        std::to_string(qty)},
                {"price",         std::to_string(price)},
                {"time_in_force", "gtc"}
            };
            std::string param_str = param.dump();

            json req = {
                {"time",    ts},
                {"channel", chan},
                {"event",   "api"},
                {"payload", {
                    {"req_id",    req_id},
                    {"req_param", param},
                    {"api_key",   api_key},
                    {"signature", sign(chan, param_str, ts)},
                    {"timestamp", std::to_string(ts)}
                }}
            };
            ws_write(req.dump());
            // Return immediately (fire & forget); the listen thread records the
            // exchange-assigned order ID via the order_id_map for future cancels.
            return {true, "t-" + req_id, "Sent"};
        } catch (const std::exception& e) {
            return {false, "", e.what()};
        }
    }

    OrderResult cancel_order_ws(const std::string& symbol,
                                const std::string& order_id) {
        try {
            // Resolve our client ID to the exchange ID if available
            std::string gate_id = order_id;
            {
                std::lock_guard<std::mutex> lk(order_id_map_mtx);
                auto it = order_id_map.find(order_id);
                if (it != order_id_map.end()) gate_id = it->second;
                else {
                    it = order_id_map.find("t-" + order_id);
                    if (it != order_id_map.end()) gate_id = it->second;
                }
            }

            long long ts       = now_secs();
            std::string req_id = generate_req_id();
            std::string chan   = "spot.order_cancel";

            json param = {{"order_id", gate_id}, {"currency_pair", symbol}};
            std::string param_str = param.dump();

            json req = {
                {"time",    ts},
                {"channel", chan},
                {"event",   "api"},
                {"payload", {
                    {"req_id",    req_id},
                    {"req_param", param},
                    {"api_key",   api_key},
                    {"signature", sign(chan, param_str, ts)},
                    {"timestamp", std::to_string(ts)}
                }}
            };
            ws_write(req.dump());
            return {true, order_id, "Cancel sent"};
        } catch (const std::exception& e) {
            return {false, "", e.what()};
        }
    }

    OrderResult cancel_all_ws(const std::string& symbol) {
        try {
            long long ts       = now_secs();
            std::string req_id = generate_req_id();
            std::string chan   = "spot.order_cancel_cp";

            json param = {{"currency_pair", symbol}};
            std::string param_str = param.dump();

            json req = {
                {"time",    ts},
                {"channel", chan},
                {"event",   "api"},
                {"payload", {
                    {"req_id",    req_id},
                    {"req_param", param},
                    {"api_key",   api_key},
                    {"signature", sign(chan, param_str, ts)},
                    {"timestamp", std::to_string(ts)}
                }}
            };
            ws_write(req.dump());
            return {true, "", "Cancel-all sent"};
        } catch (const std::exception& e) {
            return {false, "", e.what()};
        }
    }
};

// ============================================================================
// REST helpers (used as fallback when WS isn't authenticated, and for fetch)
// ============================================================================

static std::string gate_ts() {
    return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

static OrderResult gate_place_rest(const std::string& api_key,
                                   const std::string& api_secret,
                                   Side side, const std::string& symbol,
                                   double price, double qty) {
    try {
        std::string ts = gate_ts();
        json body_j = {
            {"currency_pair", symbol},
            {"type",          "limit"},
            {"side",          side == Side::BUY ? "buy" : "sell"},
            {"amount",        std::to_string(qty)},
            {"price",         std::to_string(price)},
            {"time_in_force", "gtc"}
        };
        std::string body_str   = body_j.dump();
        std::string url        = "/api/v4/spot/orders";
        std::string sign_input = "POST\n" + url + "\n\n" +
                                 sha512_hex(body_str) + "\n" + ts;
        std::string sig = hmac_sha512_hex(api_secret, sign_input);

        std::string resp = https_post("api.gateio.ws", url, body_str,
            {{"KEY",          api_key},
             {"SIGN",         sig},
             {"Timestamp",    ts},
             {"Content-Type", "application/json"},
             {"Accept",       "application/json"}});

        auto j = json::parse(resp);
        if (j.contains("id"))
            return {true, j["id"].get<std::string>(), "OK"};
        std::string msg = j.contains("message") ? j["message"].get<std::string>() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

static OrderResult gate_cancel_rest(const std::string& api_key,
                                    const std::string& api_secret,
                                    const std::string& symbol,
                                    const std::string& order_id) {
    try {
        std::string ts         = gate_ts();
        std::string url        = "/api/v4/spot/orders/" + order_id;
        std::string query      = "currency_pair=" + symbol;
        std::string sign_input = "DELETE\n" + url + "\n" + query + "\n" +
                                 sha512_hex("") + "\n" + ts;
        std::string sig = hmac_sha512_hex(api_secret, sign_input);

        std::string resp = https_delete("api.gateio.ws", url + "?" + query,
            {{"KEY",       api_key},
             {"SIGN",      sig},
             {"Timestamp", ts},
             {"Accept",    "application/json"}});

        if (resp.empty()) return {true, order_id, "Cancelled"};
        auto j = json::parse(resp);
        if (j.contains("id"))
            return {true, j["id"].get<std::string>(), "Cancelled"};
        std::string msg = j.contains("message") ? j["message"].get<std::string>() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

static OrderResult gate_cancel_all_rest(const std::string& api_key,
                                        const std::string& api_secret,
                                        const std::string& symbol) {
    try {
        std::string ts         = gate_ts();
        std::string url        = "/api/v4/spot/orders";
        std::string query      = "currency_pair=" + symbol;
        std::string sign_input = "DELETE\n" + url + "\n" + query + "\n" +
                                 sha512_hex("") + "\n" + ts;
        std::string sig = hmac_sha512_hex(api_secret, sign_input);

        std::string resp = https_delete("api.gateio.ws", url + "?" + query,
            {{"KEY",       api_key},
             {"SIGN",      sig},
             {"Timestamp", ts},
             {"Accept",    "application/json"}});

        if (resp.empty()) return {true, "", "All cancelled"};
        auto j = json::parse(resp);
        if (j.is_array()) return {true, "", "All cancelled"};
        std::string msg = j.contains("message") ? j["message"].get<std::string>() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

// ============================================================================
// GateTrader public interface
// ============================================================================

GateTrader::GateTrader(const std::string& api_key, const std::string& api_secret,
                       SharedOrderEvents* events)
    : api_key_(api_key), api_secret_(api_secret)
    , ws_(std::make_unique<GateWsImpl>(api_key, api_secret, events))
{}

GateTrader::~GateTrader() = default;

OrderResult GateTrader::place_limit_order(Side side, const std::string& symbol,
                                           double price, double qty) {
    if (ws_->authenticated)
        return ws_->place_order_ws(side, symbol, price, qty);
    // Fallback: REST (e.g. WS not yet authenticated on first call)
    return gate_place_rest(api_key_, api_secret_, side, symbol, price, qty);
}

OrderResult GateTrader::cancel_limit_order(const std::string& symbol,
                                            const std::string& order_id) {
    if (ws_->authenticated)
        return ws_->cancel_order_ws(symbol, order_id);
    return gate_cancel_rest(api_key_, api_secret_, symbol, order_id);
}

OrderResult GateTrader::cancel_all_orders(const std::string& symbol) {
    if (ws_->authenticated)
        return ws_->cancel_all_ws(symbol);
    return gate_cancel_all_rest(api_key_, api_secret_, symbol);
}

std::vector<PlacedOrder> GateTrader::fetch_open_orders(const std::string& symbol) {
    // Always use REST — WebSocket doesn't provide a synchronous query interface
    try {
        std::string ts         = gate_ts();
        std::string url        = "/api/v4/spot/orders";
        std::string query      = "currency_pair=" + symbol + "&status=open";
        std::string sign_input = "GET\n" + url + "\n" + query + "\n" +
                                 sha512_hex("") + "\n" + ts;
        std::string sig = hmac_sha512_hex(api_secret_, sign_input);

        std::string resp = https_get("api.gateio.ws", url + "?" + query,
            {{"KEY",       api_key_},
             {"SIGN",      sig},
             {"Timestamp", ts},
             {"Accept",    "application/json"}});

        auto j = json::parse(resp);
        std::vector<PlacedOrder> out;
        if (!j.is_array()) return out;
        for (auto& item : j) {
            PlacedOrder o;
            o.exchange = "Gate";
            o.symbol   = symbol;
            o.order_id = item.value("id", "");
            std::string s = item.value("side", "buy");
            o.side = (s == "buy") ? Side::BUY : Side::SELL;
            try { o.price = std::stod(item.value("price",  "0")); } catch (...) {}
            try { o.qty   = std::stod(item.value("amount", "0")); } catch (...) {}
            if (!o.order_id.empty()) out.push_back(std::move(o));
        }
        // Seed the client→gate ID map so future WS cancels work for fetched orders
        {
            std::lock_guard<std::mutex> lk(ws_->order_id_map_mtx);
            for (auto& o : out)
                ws_->order_id_map[o.order_id] = o.order_id;  // real ID on both sides
        }
        return out;
    } catch (...) { return {}; }
}
