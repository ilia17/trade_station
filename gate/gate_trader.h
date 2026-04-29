#ifndef GATE_TRADER_H
#define GATE_TRADER_H

#include <string>
#include <vector>
#include <memory>
#include "../trading/order.h"

// Forward-declare the WebSocket implementation to keep Beast headers out of this header.
struct GateWsImpl;

class GateTrader {
public:
    // events: optional shared queue for real-time order status updates (spot.orders WS channel)
    GateTrader(const std::string& api_key, const std::string& api_secret,
               SharedOrderEvents* events = nullptr);
    ~GateTrader();

    OrderResult place_limit_order(Side side, const std::string& symbol,
                                  double price, double qty);
    OrderResult cancel_limit_order(const std::string& symbol,
                                   const std::string& order_id);
    OrderResult cancel_all_orders(const std::string& symbol);
    std::vector<PlacedOrder> fetch_open_orders(const std::string& symbol);

private:
    std::string api_key_, api_secret_;
    std::unique_ptr<GateWsImpl> ws_;   // owns all WS state + threads
};

#endif
