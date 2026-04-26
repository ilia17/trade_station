#ifndef MEXC_TRADER_H
#define MEXC_TRADER_H

#include <string>
#include "../trading/order.h"

class MexcTrader {
public:
    MexcTrader(const std::string& api_key, const std::string& api_secret);
    OrderResult place_limit_order(Side side, const std::string& symbol,
                                  double price, double qty);
    OrderResult cancel_limit_order(const std::string& symbol,
                                   const std::string& order_id);
    OrderResult cancel_all_orders(const std::string& symbol);
    std::vector<PlacedOrder> fetch_open_orders(const std::string& symbol);
private:
    std::string api_key_;
    std::string api_secret_;
};

#endif
