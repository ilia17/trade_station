#ifndef MEXC_TRADER_H
#define MEXC_TRADER_H

#include <string>
#include "../trading/order.h"

class MexcTrader {
public:
    MexcTrader(const std::string& api_key, const std::string& api_secret);
    OrderResult place_limit_order(Side side, const std::string& symbol,
                                  double price, double qty);
private:
    std::string api_key_;
    std::string api_secret_;
};

#endif
