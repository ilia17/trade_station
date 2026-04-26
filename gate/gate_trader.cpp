#include "gate_trader.h"
#include "../trading/rest_client.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <chrono>

using json = nlohmann::json;

GateTrader::GateTrader(const std::string& api_key, const std::string& api_secret)
    : api_key_(api_key), api_secret_(api_secret) {}

OrderResult GateTrader::place_limit_order(Side side, const std::string& symbol,
                                           double price, double qty) {
    try {
        std::string ts = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        json body_j = {
            {"currency_pair", symbol},
            {"type",          "limit"},
            {"side",          side == Side::BUY ? "buy" : "sell"},
            {"amount",        std::to_string(qty)},
            {"price",         std::to_string(price)},
            {"time_in_force", "gtc"}
        };
        std::string body_str = body_j.dump();

        std::string url        = "/api/v4/spot/orders";
        std::string sign_input = "POST\n" + url + "\n\n" +
                                 sha512_hex(body_str) + "\n" + ts;
        std::string sig = hmac_sha512_hex(api_secret_, sign_input);

        std::string resp = https_post("api.gateio.ws", url, body_str,
            {{"KEY",          api_key_},
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

OrderResult GateTrader::cancel_limit_order(const std::string& symbol,
                                            const std::string& order_id) {
    try {
        std::string ts = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        std::string url        = "/api/v4/spot/orders/" + order_id;
        std::string query      = "currency_pair=" + symbol;
        std::string sign_input = "DELETE\n" + url + "\n" + query + "\n" +
                                 sha512_hex("") + "\n" + ts;
        std::string sig = hmac_sha512_hex(api_secret_, sign_input);

        std::string resp = https_delete("api.gateio.ws",
            url + "?" + query,
            {{"KEY",       api_key_},
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
