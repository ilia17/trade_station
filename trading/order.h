#ifndef ORDER_H
#define ORDER_H

#include <string>
#include <future>
#include <vector>
#include <memory>
#include <mutex>
#include <utility>
#include <atomic>

enum class Side { BUY, SELL };

enum class OrderStatus { IDLE, PENDING, OK, ERROR };

struct OrderRequest {
    std::string exchange;
    std::string symbol;
    Side        side;
    double      price{0.0};
    double      quantity{0.0};
};

struct OrderResult {
    bool        success{false};
    std::string order_id;
    std::string message;
};

// A successfully placed order tracked in the UI
struct PlacedOrder {
    std::string order_id;
    std::string exchange;   // "MEXC" | "Gate" | "BingX" | "LBank"
    std::string symbol;
    Side        side{Side::BUY};
    double      price{0};
    double      qty{0};
    double      cum_qty{0};  // cumulative filled quantity (updated by WS stream)

    // Cancel flow
    std::shared_ptr<std::future<OrderResult>> cancel_fut;
    bool        cancelling{false};
    bool        cancelled{false};
    bool        cancel_error{false};
    std::string cancel_msg;   // "Filled" | "Cancelled" | "Partial Fill" | error text

    // Monotonic sequence for UI sort (newest / highest first when displaying lists)
    long long ui_seq{0};
};

inline long long next_placed_order_seq() {
    static std::atomic<long long> s{0};
    return ++s;
}

// Real-time order event pushed by exchange WS listeners
struct OrderEvent {
    std::string exchange;    // "MEXC" | "Gate"
    std::string order_id;    // exchange-assigned order ID
    std::string client_id;   // client/text order ID (Gate "t-xxx", MEXC clientId)
    std::string finish_as;   // Gate: "filled" | "cancelled" | "open" | ...
    std::string event_type;  // "new" | "update" | "finish"
    std::string symbol;      // trading pair (e.g. "BTC_USDT", "BTCUSDT")
    int         status{0};   // MEXC: 1=new, 2=filled, 3=partial, 4=cancelled, 5=partial-cancel
    double      cum_qty{0};
    double      avg_price{0};
    // Full order details — populated for "new" events (external orders need this to display)
    double      price{0};
    double      qty{0};
    int         side{0};     // 0=buy, 1=sell
};

// Lock-free-style event queue: stream thread pushes, render thread drains each frame
struct SharedOrderEvents {
    std::mutex              mtx;
    std::vector<OrderEvent> events;

    void push(OrderEvent e) {
        std::lock_guard<std::mutex> lk(mtx);
        events.push_back(std::move(e));
    }
    std::vector<OrderEvent> drain() {
        std::lock_guard<std::mutex> lk(mtx);
        return std::exchange(events, {});
    }
};

// Per-panel UI state — one instance per exchange panel
struct OrderFormState {
    int  side{0};             // 0 = BUY, 1 = SELL
    char price_buf[32]{};
    char qty_buf[32]{};

    std::future<OrderResult>              pending;
    OrderStatus                           status{OrderStatus::IDLE};
    std::string                           status_msg;

    std::vector<PlacedOrder>              orders;   // open orders for this panel
    std::future<std::vector<PlacedOrder>> fetch_fut;
    bool                                  fetching{false};
    std::future<OrderResult>              cancel_all_fut;
    bool                                  cancelling_all{false};
};

#endif
