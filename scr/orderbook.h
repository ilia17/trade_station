#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include <array>
#include <cstring>
#include <mutex>

struct PriceLevel {
    double price{0.0};
    double quantity{0.0};
};

struct OrderBookUpdate {
    char exchange[16]{};
    char symbol[16]{};
    std::array<PriceLevel, 5> bids{};
    std::array<PriceLevel, 5> asks{};
    long timestamp_ms{0};
    int bid_count{0};
    int ask_count{0};
};

// ── Per-exchange display state — read by ImGui render thread ─────────────────

struct SharedDisplay {
    // One slot per exchange, updated independently
    OrderBookUpdate exchanges[4]{};
    int exchange_count{0};
    std::mutex mtx;

    // Called by the aggregator thread on every disruptor message
    void update(const OrderBookUpdate& u) {
        std::lock_guard<std::mutex> lock(mtx);
        for (int i = 0; i < exchange_count; i++) {
            if (strcmp(exchanges[i].exchange, u.exchange) == 0) {
                exchanges[i] = u;
                return;
            }
        }
        if (exchange_count < 4) {
            exchanges[exchange_count++] = u;
        }
    }

    // Called by the render thread — returns a copy to avoid holding the lock
    // while drawing
    void snapshot(OrderBookUpdate out[4], int& count) const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mtx));
        count = exchange_count;
        for (int i = 0; i < exchange_count; i++) out[i] = exchanges[i];
    }

    // Called by the render thread on symbol switch — wipes stale price levels
    // but keeps the exchange name slots so panels don't vanish.
    void clear_books() {
        std::lock_guard<std::mutex> lock(mtx);
        for (int i = 0; i < exchange_count; i++) {
            exchanges[i].bids.fill({});
            exchanges[i].asks.fill({});
            exchanges[i].bid_count = 0;
            exchanges[i].ask_count = 0;
            exchanges[i].timestamp_ms = 0;
            // symbol field will be overwritten by the first real update
            exchanges[i].symbol[0] = '\0';
        }
    }
};

#endif
