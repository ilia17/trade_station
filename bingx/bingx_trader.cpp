#include "bingx_trader.h"
#include "../trading/rest_client.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <chrono>

using json = nlohmann::json;

BingXTrader::BingXTrader(const std::string& api_key, const std::string& api_secret)
    : api_key_(api_key), api_secret_(api_secret) {}

OrderResult BingXTrader::place_limit_order(Side side, const std::string& symbol,
                                            double price, double qty) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::ostringstream ps;
        ps << "symbol="    << symbol
           << "&side="     << (side == Side::BUY ? "BUY" : "SELL")
           << "&type=LIMIT"
           << "&quantity=" << std::fixed << std::setprecision(8) << qty
           << "&price="    << std::fixed << std::setprecision(8) << price
           << "&timeInForce=GTC"
           << "&timestamp=" << ts;
        std::string params = ps.str();
        params += "&signature=" + hmac_sha256_hex(api_secret_, params);

        std::string resp = https_post("open-api.bingx.com",
            "/openApi/spot/v1/trade/order", params,
            {{"X-BX-APIKEY",  api_key_},
             {"Content-Type", "application/x-www-form-urlencoded"}});

        auto j = json::parse(resp);
        if (j.contains("data") && j["data"].contains("orderId"))
            return {true, std::to_string(j["data"]["orderId"].get<long long>()), "OK"};
        std::string msg = j.contains("msg") ? j["msg"].get<std::string>() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

OrderResult BingXTrader::cancel_limit_order(const std::string& symbol,
                                             const std::string& order_id) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::ostringstream ps;
        ps << "symbol="    << symbol
           << "&orderId="   << order_id
           << "&timestamp=" << ts;
        std::string params = ps.str();
        params += "&signature=" + hmac_sha256_hex(api_secret_, params);

        std::string resp = https_get("open-api.bingx.com",
            "/openApi/spot/v1/trade/cancel?" + params,
            {{"X-BX-APIKEY", api_key_}});

        auto j = json::parse(resp);
        if (j.contains("data") && j["data"].contains("orderId"))
            return {true, order_id, "Cancelled"};
        std::string msg = j.contains("msg") ? j["msg"].get<std::string>() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

OrderResult BingXTrader::cancel_all_orders(const std::string& symbol) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string qs = "symbol=" + symbol + "&timestamp=" + std::to_string(ts);
        qs += "&signature=" + hmac_sha256_hex(api_secret_, qs);

        std::string resp = https_delete("open-api.bingx.com",
            "/openApi/spot/v1/trade/allOpenOrders?" + qs,
            {{"X-BX-APIKEY",  api_key_},
             {"Content-Type", "application/json"}});

        if (resp.empty()) return {true, "", "All cancelled"};
        auto j = json::parse(resp);
        if (j.contains("code") && j["code"].get<int>() == 0)
            return {true, "", "All cancelled"};
        std::string msg = j.contains("msg") ? j["msg"].get<std::string>() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

std::vector<PlacedOrder> BingXTrader::fetch_open_orders(const std::string& symbol) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string qs = "symbol=" + symbol + "&timestamp=" + std::to_string(ts);
        qs += "&signature=" + hmac_sha256_hex(api_secret_, qs);

        std::string resp = https_get("open-api.bingx.com",
            "/openApi/spot/v1/trade/openOrders?" + qs,
            {{"X-BX-APIKEY", api_key_}});

        auto j = json::parse(resp);
        std::vector<PlacedOrder> out;
        if (!j.contains("data") || !j["data"].contains("orders")) return out;
        for (auto& item : j["data"]["orders"]) {
            PlacedOrder o;
            o.exchange = "BingX";
            o.symbol   = symbol;
            o.order_id = item.contains("orderId")
                ? std::to_string(item["orderId"].get<long long>()) : "";
            std::string side = item.value("side", "BUY");
            o.side = (side == "BUY") ? Side::BUY : Side::SELL;
            try { o.price = std::stod(item.value("price",    "0")); } catch(...) {}
            // BingX REST uses "origQty" (original base qty); fall back to "quantity"
            // and "origQuoteOrderQty" (quote amount) for market-by-amount orders.
            try {
                std::string qty_str = item.contains("origQty") && !item["origQty"].is_null()
                    ? item.value("origQty", "0") : item.value("quantity", "0");
                o.qty = std::stod(qty_str);
                if (o.qty <= 0) {
                    std::string q_str = item.value("origQuoteOrderQty", "0");
                    if (q_str.empty()) q_str = "0";
                    o.qty = std::stod(q_str);
                }
            } catch(...) {}
            if (!o.order_id.empty()) out.push_back(std::move(o));
        }
        return out;
    } catch(...) { return {}; }
}
