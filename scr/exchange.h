#ifndef EXCHANGE_H
#define EXCHANGE_H

#include <string>
#include <atomic>
#include "orderbook.h"
#include "disruptor.h"
#include "trades.h"

class Exchange {
protected:
    std::string name;
    std::string symbol;
    std::string ws_url;
    Disruptor<1024>& disruptor;
    SharedTrades*    shared_trades{nullptr};
    std::atomic<bool> running{false};

public:
    Exchange(const std::string& name,
             const std::string& symbol,
             const std::string& ws_url,
             Disruptor<1024>& disruptor)
        : name(name)
        , symbol(symbol)
        , ws_url(ws_url)
        , disruptor(disruptor)
    {}

    virtual ~Exchange() = default;

    // Pure virtual — each exchange must implement these
    virtual void connect()    = 0;
    virtual void subscribe()  = 0;
    virtual void disconnect() = 0;
    virtual void run()        = 0;

    // Optional — re-subscribe to a new base/quote pair while connected.
    // Each handler overrides with exchange-specific symbol formatting.
    virtual void change_symbol(const std::string& /*base*/,
                               const std::string& /*quote*/) {}

    // Common — same for all exchanges
    void set_shared_trades(SharedTrades* st) { shared_trades = st; }

    void stop() { running.store(false, std::memory_order_release); }
    bool is_running() const { return running.load(std::memory_order_acquire); }
    const std::string& get_name()   const { return name;   }
    const std::string& get_symbol() const { return symbol; }
};

#endif