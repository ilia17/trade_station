#include "lbank_trader.h"
#include "../trading/rest_client.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <map>
#include <chrono>

using json = nlohmann::json;

LBankTrader::LBankTrader(const std::string& api_key, const std::string& api_secret)
    : api_key_(api_key), api_secret_(api_secret) {}

// Helper: build signed form body from a sorted param map
static std::string lbank_signed_body(
    const std::string& api_key, const std::string& api_secret,
    std::map<std::string,std::string> params)
{
    params["api_key"]          = api_key;
    params["signature_method"] = "HmacSHA256";

    std::ostringstream sign_input;
    for (auto& [k, v] : params)
        sign_input << k << "=" << url_encode(v) << "&";
    std::string si = sign_input.str();
    if (!si.empty()) si.pop_back();
    params["sign"] = hmac_sha256_hex(api_secret, si);

    std::ostringstream body;
    for (auto& [k, v] : params)
        body << url_encode(k) << "=" << url_encode(v) << "&";
    std::string s = body.str();
    if (!s.empty()) s.pop_back();
    return s;
}

OrderResult LBankTrader::place_limit_order(Side side, const std::string& symbol,
                                            double price, double qty) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::map<std::string,std::string> params;
        params["symbol"]    = symbol;
        params["type"]      = (side == Side::BUY) ? "buy_limit" : "sell_limit";
        params["price"]     = std::to_string(price);
        params["amount"]    = std::to_string(qty);
        params["timestamp"] = std::to_string(ts);

        std::string resp = https_post("api.lbkex.com",
            "/v2/supplement/create_order.do",
            lbank_signed_body(api_key_, api_secret_, params),
            {{"Content-Type", "application/x-www-form-urlencoded"}});

        auto j = json::parse(resp);
        if (j.contains("result") && j["result"] == "true") {
            std::string oid = j.contains("order_id")
                ? j["order_id"].get<std::string>() : "";
            return {true, oid, "OK"};
        }
        std::string msg = j.contains("error_code")
            ? "error_code=" + j["error_code"].dump() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

OrderResult LBankTrader::cancel_limit_order(const std::string& symbol,
                                             const std::string& order_id) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::map<std::string,std::string> params;
        params["symbol"]    = symbol;
        params["orderId"]   = order_id;
        params["timestamp"] = std::to_string(ts);

        std::string resp = https_post("api.lbkex.com",
            "/v2/supplement/cancel_order.do",
            lbank_signed_body(api_key_, api_secret_, params),
            {{"Content-Type", "application/x-www-form-urlencoded"}});

        auto j = json::parse(resp);
        if (j.contains("result") && j["result"] == "true")
            return {true, order_id, "Cancelled"};
        std::string msg = j.contains("error_code")
            ? "error_code=" + j["error_code"].dump() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

OrderResult LBankTrader::cancel_all_orders(const std::string& symbol) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::map<std::string,std::string> params;
        params["symbol"]    = symbol;
        params["timestamp"] = std::to_string(ts);

        std::string resp = https_post("api.lbkex.com",
            "/v2/supplement/cancel_order_by_symbol.do",
            lbank_signed_body(api_key_, api_secret_, params),
            {{"Content-Type", "application/x-www-form-urlencoded"}});

        if (resp.empty()) return {true, "", "All cancelled"};
        auto j = json::parse(resp);
        if (j.contains("result") && j["result"] == "true")
            return {true, "", "All cancelled"};
        std::string msg = j.contains("error_code")
            ? "error_code=" + j["error_code"].dump() : resp;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}

std::vector<PlacedOrder> LBankTrader::fetch_open_orders(const std::string& symbol) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::map<std::string,std::string> params;
        params["symbol"]       = symbol;
        params["current_page"] = "1";
        params["page_length"]  = "50";
        params["timestamp"]    = std::to_string(ts);

        std::string resp = https_post("api.lbkex.com",
            "/v2/supplement/orders_info_no_deal.do",
            lbank_signed_body(api_key_, api_secret_, params),
            {{"Content-Type", "application/x-www-form-urlencoded"}});

        auto j = json::parse(resp);
        std::vector<PlacedOrder> out;
        if (!j.contains("orders") || !j["orders"].is_array()) return out;
        for (auto& item : j["orders"]) {
            PlacedOrder o;
            o.exchange = "LBank";
            o.symbol   = symbol;
            o.order_id = item.value("order_id", "");
            std::string type = item.value("type", "buy_limit");
            o.side = (type.find("buy") != std::string::npos) ? Side::BUY : Side::SELL;
            auto get_num = [&](const char* key) -> double {
                if (!item.contains(key)) return 0;
                if (item[key].is_number()) return item[key].get<double>();
                try { return std::stod(item[key].get<std::string>()); } catch(...) {}
                return 0;
            };
            o.price = get_num("price");
            o.qty   = get_num("amount");
            if (!o.order_id.empty()) out.push_back(std::move(o));
        }
        return out;
    } catch(...) { return {}; }
}
