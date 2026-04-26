#include <atomic>
#include <thread>
#include "scr/disruptor.h"
#include "scr/aggregator.h"
#include "scr/orderbook.h"
#include "scr/trades.h"
#include "scr/exchange.h"
#include "mexc/mexc_handler.h"
#include "gate/gate_handler.h"
#include "bingx/bingx_handler.h"
#include "lbank/lbank_handler.h"
#include "trading/trade_manager.h"
#include "ui/render.h"

Disruptor<1024>      disruptor;
SharedDisplay        display;
SharedTrades         shared_trades;
std::atomic<bool>    g_running{true};

int main() {
    // Aggregator — drains disruptor into SharedDisplay
    int agg_id = disruptor.add_consumer();
    std::thread agg_thread([&]{ aggregator_run(agg_id, disruptor, display, g_running); });

    // Exchange feed threads
    MexcHandler  mexc(disruptor);  mexc.set_shared_trades(&shared_trades);
    GateHandler  gate(disruptor);  gate.set_shared_trades(&shared_trades);
    BingXHandler bingx(disruptor); bingx.set_shared_trades(&shared_trades);
    LBankHandler lbank(disruptor); lbank.set_shared_trades(&shared_trades);

    std::thread t_mexc([&]{ mexc.run(); });
    std::thread t_gate([&]{ gate.run(); });
    std::thread t_bingx([&]{ bingx.run(); });
    std::thread t_lbank([&]{ lbank.run(); });

    // Trade manager — loads API keys from environment
    TradeManager trader;

    // Handler pointer array for symbol switching from the UI
    Exchange* handlers[4] = { &mexc, &gate, &bingx, &lbank };

    // Render — blocks until window is closed, then sets g_running = false
    render_run(display, shared_trades, trader, handlers, g_running);

    mexc.disconnect();
    gate.disconnect();
    bingx.disconnect();
    lbank.disconnect();

    t_mexc.join();
    t_gate.join();
    t_bingx.join();
    t_lbank.join();
    agg_thread.join();

    return 0;
}
