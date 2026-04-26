#ifndef ORDER_H
#define ORDER_H

#include <string>
#include <future>

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

// Per-panel UI state — one instance per exchange panel
struct OrderFormState {
    int  side{0};             // 0 = BUY, 1 = SELL
    char price_buf[32]{};
    char qty_buf[32]{};

    std::future<OrderResult> pending;
    OrderStatus              status{OrderStatus::IDLE};
    std::string              status_msg;
};

#endif
