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
