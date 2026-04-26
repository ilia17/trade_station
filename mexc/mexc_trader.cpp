#include "mexc_trader.h"
#include "../trading/rest_client.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <chrono>

using json = nlohmann::json;

MexcTrader::MexcTrader(const std::string& api_key, const std::string& api_secret)
    : api_key_(api_key), api_secret_(api_secret) {}

OrderResult MexcTrader::place_limit_order(Side side, const std::string& symbol,
                                           double price, double qty) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::ostringstream qs;
        qs << "symbol="    << symbol
           << "&side="     << (side == Side::BUY ? "BUY" : "SELL")
           << "&type=LIMIT"
           << "&price="    << std::fixed << std::setprecision(8) << price
           << "&quantity=" << std::fixed << std::setprecision(8) << qty
           << "&timestamp=" << ts;
        std::string query = qs.str();
        query += "&signature=" + hmac_sha256_hex(api_secret_, query);

        std::string body = https_post("api.mexc.com", "/api/v3/order?" + query, "",
            {{"X-MEXC-APIKEY", api_key_},
             {"Content-Type",  "application/json"}});

        auto j = json::parse(body);
        if (j.contains("orderId")) {
            std::string oid = j["orderId"].is_string()
                ? j["orderId"].get<std::string>() : j["orderId"].dump();
            return {true, oid, "OK"};
        }
        std::string msg = j.contains("msg") ? j["msg"].get<std::string>() : body;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

OrderResult MexcTrader::cancel_limit_order(const std::string& symbol,
                                            const std::string& order_id) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::ostringstream qs;
        qs << "symbol="    << symbol
           << "&orderId="   << order_id
           << "&timestamp=" << ts;
        std::string query = qs.str();
        query += "&signature=" + hmac_sha256_hex(api_secret_, query);

        std::string resp = https_delete("api.mexc.com",
            "/api/v3/order?" + query,
            {{"X-MEXC-APIKEY", api_key_},
             {"Content-Type",  "application/json"}});

        if (resp.empty()) return {true, order_id, "Cancelled"};
        auto j = json::parse(resp);
        if (j.contains("orderId")) {
            std::string oid = j["orderId"].is_string()
                ? j["orderId"].get<std::string>() : j["orderId"].dump();
            return {true, oid, "Cancelled"};
        }
        std::string msg = j.contains("msg") ? j["msg"].get<std::string>() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

OrderResult MexcTrader::cancel_all_orders(const std::string& symbol) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string qs = "symbol=" + symbol + "&timestamp=" + std::to_string(ts);
        qs += "&signature=" + hmac_sha256_hex(api_secret_, qs);

        std::string resp = https_delete("api.mexc.com",
            "/api/v3/openOrders?" + qs,
            {{"X-MEXC-APIKEY", api_key_},
             {"Content-Type",  "application/json"}});

        if (resp.empty()) return {true, "", "All cancelled"};
        auto j = json::parse(resp);
        if (j.is_array()) return {true, "", "All cancelled"};
        std::string msg = j.contains("msg") ? j["msg"].get<std::string>() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

std::vector<PlacedOrder> MexcTrader::fetch_open_orders(const std::string& symbol) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string qs = "symbol=" + symbol + "&timestamp=" + std::to_string(ts);
        qs += "&signature=" + hmac_sha256_hex(api_secret_, qs);

        std::string resp = https_get("api.mexc.com",
            "/api/v3/openOrders?" + qs,
            {{"X-MEXC-APIKEY", api_key_}});

        auto j = json::parse(resp);
        std::vector<PlacedOrder> out;
        if (!j.is_array()) return out;
        for (auto& item : j) {
            PlacedOrder o;
            o.exchange = "MEXC";
            o.symbol   = symbol;
            o.order_id = item.contains("orderId")
                ? (item["orderId"].is_string()
                    ? item["orderId"].get<std::string>()
                    : item["orderId"].dump())
                : "";
            std::string side = item.value("side", "BUY");
            o.side = (side == "BUY") ? Side::BUY : Side::SELL;
            try { o.price = std::stod(item.value("price",   "0")); } catch(...) {}
            try { o.qty   = std::stod(item.value("origQty", "0")); } catch(...) {}
            if (!o.order_id.empty()) out.push_back(std::move(o));
        }
        return out;
    } catch(...) { return {}; }
}
