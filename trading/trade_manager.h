#ifndef TRADE_MANAGER_H
#define TRADE_MANAGER_H

#include <string>
#include <memory>
#include <future>
#include <vector>
#include <chrono>
#include <cstdlib>

#include "order.h"
#include "../mexc/mexc_trader.h"
#include "../gate/gate_trader.h"
#include "../bingx/bingx_trader.h"
#include "../lbank/lbank_trader.h"

// Loads API keys from environment variables at construction.
// Exposes has_keys(exchange) and async submit().
class TradeManager {
public:
    // order_events: optional queue for Gate.io real-time order updates via spot.orders WS channel
    explicit TradeManager(SharedOrderEvents* order_events = nullptr) {
        auto load = [](const char* k, const char* s) -> std::pair<std::string,std::string> {
            const char* key = std::getenv(k);
            const char* sec = std::getenv(s);
            return {key ? key : "", sec ? sec : ""};
        };

        auto [mk, ms] = load("MEXC_API_KEY",  "MEXC_API_SECRET");
        auto [gk, gs] = load("GATE_API_KEY",  "GATE_API_SECRET");
        auto [bk, bs] = load("BINGX_API_KEY", "BINGX_API_SECRET");
        auto [lk, ls] = load("LBANK_API_KEY", "LBANK_API_SECRET");

        if (!mk.empty() && !ms.empty()) mexc_  = std::make_unique<MexcTrader>(mk, ms);
        if (!gk.empty() && !gs.empty()) gate_  = std::make_unique<GateTrader>(gk, gs, order_events);
        if (!bk.empty() && !bs.empty()) bingx_ = std::make_unique<BingXTrader>(bk, bs);
        if (!lk.empty() && !ls.empty()) lbank_ = std::make_unique<LBankTrader>(lk, ls);
    }

    // Stop all exchange connections (call before joining market-data threads).
    void shutdown() {
        gate_.reset();   // destroys GateTrader → GateWsImpl threads stop
        mexc_.reset();
        bingx_.reset();
        lbank_.reset();
    }

    bool has_keys(const std::string& exchange) const {
        if (exchange == "MEXC")  return mexc_  != nullptr;
        if (exchange == "Gate")  return gate_  != nullptr;
        if (exchange == "BingX") return bingx_ != nullptr;
        if (exchange == "LBank") return lbank_ != nullptr;
        return false;
    }

    // Non-blocking: spawns a thread and returns a future with the result.
    std::future<OrderResult> submit(const std::string& exchange,
                                    Side side,
                                    const std::string& symbol,
                                    double price, double qty) {
        return std::async(std::launch::async, [=, this]() -> OrderResult {
            if (exchange == "MEXC"  && mexc_)  return mexc_->place_limit_order(side, symbol, price, qty);
            if (exchange == "Gate"  && gate_)  return gate_->place_limit_order(side, symbol, price, qty);
            if (exchange == "BingX" && bingx_) return bingx_->place_limit_order(side, symbol, price, qty);
            if (exchange == "LBank" && lbank_) return lbank_->place_limit_order(side, symbol, price, qty);
            return {false, "", "No trader for " + exchange};
        });
    }

    // Non-blocking: fetch all open orders for a symbol on one exchange.
    std::future<std::vector<PlacedOrder>> fetch_orders(const std::string& exchange,
                                                        const std::string& symbol) {
        return std::async(std::launch::async, [=, this]() -> std::vector<PlacedOrder> {
            try {
                if (exchange == "MEXC"  && mexc_)  return mexc_->fetch_open_orders(symbol);
                if (exchange == "Gate"  && gate_)  return gate_->fetch_open_orders(symbol);
                if (exchange == "BingX" && bingx_) return bingx_->fetch_open_orders(symbol);
                if (exchange == "LBank" && lbank_) return lbank_->fetch_open_orders(symbol);
            } catch(...) {}
            return {};
        });
    }

    // Non-blocking bulk cancel: one API call cancels all open orders for the symbol.
    std::future<OrderResult> cancel_all(const std::string& exchange,
                                        const std::string& symbol) {
        return std::async(std::launch::async, [=, this]() -> OrderResult {
            try {
                if (exchange == "MEXC"  && mexc_)  return mexc_->cancel_all_orders(symbol);
                if (exchange == "Gate"  && gate_)  return gate_->cancel_all_orders(symbol);
                if (exchange == "BingX" && bingx_) return bingx_->cancel_all_orders(symbol);
                if (exchange == "LBank" && lbank_) return lbank_->cancel_all_orders(symbol);
            } catch(...) {}
            return {false, "", "No trader for " + exchange};
        });
    }

    // Non-blocking cancel: returns a future with the result.
    std::future<OrderResult> cancel(const std::string& exchange,
                                    const std::string& symbol,
                                    const std::string& order_id) {
        return std::async(std::launch::async, [=, this]() -> OrderResult {
            if (exchange == "MEXC"  && mexc_)  return mexc_->cancel_limit_order(symbol, order_id);
            if (exchange == "Gate"  && gate_)  return gate_->cancel_limit_order(symbol, order_id);
            if (exchange == "BingX" && bingx_) return bingx_->cancel_limit_order(symbol, order_id);
            if (exchange == "LBank" && lbank_) return lbank_->cancel_limit_order(symbol, order_id);
            return {false, "", "No trader for " + exchange};
        });
    }

private:
    std::unique_ptr<MexcTrader>  mexc_;
    std::unique_ptr<GateTrader>  gate_;
    std::unique_ptr<BingXTrader> bingx_;
    std::unique_ptr<LBankTrader> lbank_;
};

#endif
