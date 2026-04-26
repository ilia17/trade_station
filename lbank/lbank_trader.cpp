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

OrderResult LBankTrader::place_limit_order(Side side, const std::string& symbol,
                                            double price, double qty) {
    try {
        long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::string type = (side == Side::BUY) ? "buy_limit" : "sell_limit";

        std::map<std::string,std::string> params;
        params["api_key"]          = api_key_;
        params["symbol"]           = symbol;
        params["type"]             = type;
        params["price"]            = std::to_string(price);
        params["amount"]           = std::to_string(qty);
        params["timestamp"]        = std::to_string(ts);
        params["signature_method"] = "HmacSHA256";

        std::ostringstream sign_input;
        for (auto& [k, v] : params)
            sign_input << k << "=" << url_encode(v) << "&";
        std::string si = sign_input.str();
        if (!si.empty()) si.pop_back();

        params["sign"] = hmac_sha256_hex(api_secret_, si);

        std::ostringstream body;
        for (auto& [k, v] : params)
            body << url_encode(k) << "=" << url_encode(v) << "&";
        std::string body_str = body.str();
        if (!body_str.empty()) body_str.pop_back();

        std::string resp = https_post("api.lbkex.com",
            "/v2/supplement/create_order.do", body_str,
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
