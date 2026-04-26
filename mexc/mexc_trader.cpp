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
        qs << "symbol="   << symbol
           << "&side="    << (side == Side::BUY ? "BUY" : "SELL")
           << "&type=LIMIT"
           << "&price="    << std::fixed << std::setprecision(8) << price
           << "&quantity=" << std::fixed << std::setprecision(8) << qty
           << "&timestamp=" << ts;

        std::string query = qs.str();
        query += "&signature=" + hmac_sha256_hex(api_secret_, query);

        std::string target = "/api/v3/order?" + query;
        std::string body   = https_post("api.mexc.com", target, "",
            {{"X-MEXC-APIKEY",    api_key_},
             {"Content-Type",     "application/json"}});

        auto j = json::parse(body);
        if (j.contains("orderId"))
            return {true, j["orderId"].dump(), "OK"};
        std::string msg = j.contains("msg") ? j["msg"].get<std::string>() : body;
        return {false, "", msg};
    } catch (const std::exception& e) {
        return {false, "", e.what()};
    }
}
