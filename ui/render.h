#ifndef RENDER_H
#define RENDER_H

#include <atomic>
#include "../scr/orderbook.h"
#include "../scr/trades.h"
#include "../scr/exchange.h"
#include "../trading/trade_manager.h"
#include "../trading/order.h"

void render_run(SharedDisplay&      display,
                SharedTrades&       shared_trades,
                TradeManager&       trader,
                Exchange*           handlers[4],
                std::atomic<bool>&  running,
                SharedOrderEvents*  order_events = nullptr);

#endif
