#ifndef TRADES_H
#define TRADES_H

#include <cstring>
#include <mutex>

struct Trade {
    double    price{0};
    double    qty{0};
    bool      is_buy{false};    // true = taker BUY (green), false = taker SELL (red)
    long long time_ms{0};
};

// Holds the latest 20 trades per exchange, written by handler threads,
// read (via snapshot) by the render thread.
struct SharedTrades {
    static constexpr int MAX      = 20;
    static constexpr int MAX_EX   = 4;

    Trade  trades[MAX_EX][MAX]{};
    int    counts[MAX_EX]{};
    char   names[MAX_EX][16]{};
    int    exchange_count{0};
    mutable std::mutex mtx;

    // Called by exchange handler thread — newest trade prepended
    void add(const char* exchange, const Trade& t) {
        std::lock_guard<std::mutex> lock(mtx);
        int idx = find_or_create(exchange);
        if (idx < 0) return;
        // Shift right, prepend at [0]
        for (int i = MAX - 1; i > 0; i--)
            trades[idx][i] = trades[idx][i - 1];
        trades[idx][0] = t;
        if (counts[idx] < MAX) counts[idx]++;
    }

    // Called by render thread — copy under lock
    void snapshot(const char* exchange, Trade out[MAX], int& count) const {
        std::lock_guard<std::mutex> lock(mtx);
        int idx = find_idx(exchange);
        if (idx < 0) { count = 0; return; }
        count = counts[idx];
        for (int i = 0; i < count; i++) out[i] = trades[idx][i];
    }

    // Wipe trades on symbol switch
    void clear_all() {
        std::lock_guard<std::mutex> lock(mtx);
        for (int i = 0; i < exchange_count; i++) {
            counts[i] = 0;
            for (int j = 0; j < MAX; j++) trades[i][j] = Trade{};
        }
    }

private:
    int find_idx(const char* name) const {
        for (int i = 0; i < exchange_count; i++)
            if (strcmp(names[i], name) == 0) return i;
        return -1;
    }
    int find_or_create(const char* name) {
        int i = find_idx(name);
        if (i >= 0) return i;
        if (exchange_count >= MAX_EX) return -1;
        i = exchange_count++;
        strncpy(names[i], name, 15);
        return i;
    }
};

#endif
