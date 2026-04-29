#include <atomic>
#include <thread>
#include "scr/disruptor.h"
#include "scr/aggregator.h"
#include "scr/orderbook.h"
#include "scr/trades.h"
#include "scr/exchange.h"
#include "mexc/mexc_handler.h"
#include "mexc/mexc_order_stream.h"
#include "bingx/bingx_order_stream.h"
#include "gate/gate_handler.h"
#include "bingx/bingx_handler.h"
#include "lbank/lbank_handler.h"
#include "trading/trade_manager.h"
#include "ui/render.h"

Disruptor<1024>      disruptor;
SharedDisplay        display;
SharedTrades         shared_trades;
SharedOrderEvents    order_events;   // MEXC real-time order status updates
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

    // Trade manager — loads API keys; passes order_events to Gate for spot.orders WS stream
    TradeManager trader(&order_events);

    // MEXC private order stream
    const char* mexc_key    = std::getenv("MEXC_API_KEY");
    const char* mexc_secret = std::getenv("MEXC_API_SECRET");
    MexcOrderStream* mexc_order_stream = nullptr;
    if (mexc_key    && mexc_key[0]    != '\0' &&
        mexc_secret && mexc_secret[0] != '\0') {
        mexc_order_stream = new MexcOrderStream(mexc_key, mexc_secret, &order_events);
        mexc_order_stream->start();
    }

    // BingX private order stream
    const char* bingx_key    = std::getenv("BINGX_API_KEY");
    const char* bingx_secret = std::getenv("BINGX_API_SECRET");
    BingXOrderStream* bingx_order_stream = nullptr;
    if (bingx_key    && bingx_key[0]    != '\0' &&
        bingx_secret && bingx_secret[0] != '\0') {
        bingx_order_stream = new BingXOrderStream(bingx_key, bingx_secret, &order_events);
        bingx_order_stream->start();
    }

    // Handler pointer array for symbol switching from the UI
    Exchange* handlers[4] = { &mexc, &gate, &bingx, &lbank };

    // Render — blocks until window is closed, then sets g_running = false
    render_run(display, shared_trades, trader, handlers, g_running, &order_events);

    // ── Shutdown sequence ──────────────────────────────────────────────────────
    // 1. Stop private order streams
    if (mexc_order_stream)  { mexc_order_stream->stop();  delete mexc_order_stream;  }
    if (bingx_order_stream) { bingx_order_stream->stop(); delete bingx_order_stream; }

    // 2. Stop trading WebSocket connections (GateWsImpl listen/ping threads)
    trader.shutdown();

    // 3. Stop market-data feeds and join their threads
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
