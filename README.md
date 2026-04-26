# Trade Station — Multi-Exchange Order Book & Trading Terminal

A high-performance,low-latency C++ trading terminal that streams real-time order books and recent trades from four cryptocurrency exchanges simultaneously, renders them side-by-side in a native ImGui GUI, and supports live limit order placement on all four exchanges — all from a single window.

---

## What It Does

| Feature | Detail |
|---|---|
| **Order books** | Top-5 bids + asks per exchange, refreshed in real time |
| **Recent trades** | Last 20 trades per exchange (green = buy, red = sell) |
| **Click-to-fill** | Click any price level to fill Price **and** Qty in the order form |
| **Currency labels** | "Qty (BTC)" / "Total (USDT)" update when you switch symbol |
| **Symbol switching** | One input bar changes all four feeds simultaneously |
| **Limit orders** | BUY / SELL on MEXC, Gate, BingX, LBank — async, non-blocking |
| **Comma formatting** | Prices, quantities, and totals all use thousands separators |
| **Native cursor** | Uses the OS mouse cursor (not an ImGui software cursor) |

---

## Architecture

```
┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
│   MEXC   │  │   Gate   │  │  BingX   │  │  LBank   │
│ Protobuf │  │   JSON   │  │   Gzip   │  │   JSON   │
│  thread  │  │  thread  │  │  thread  │  │  thread  │
└────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘
     │  order book │             │              │
     └─────────────┴──── Disruptor<1024> ───────┘
                              │  (lock-free ring buffer)
                   ┌──────────▼──────────┐
                   │  Aggregator thread  │
                   │  → SharedDisplay    │
                   └──────────┬──────────┘
                              │  mutex snapshot per frame
                   ┌──────────▼──────────────────────────┐
                   │       ImGui render thread            │
                   │  4 panels: asks · spread · bids      │
                   │           recent trades              │
                   │           limit order form           │
                   └──────────┬──────────────────────────┘
                              │  std::async (non-blocking)
                   ┌──────────▼──────────┐
                   │    TradeManager     │
                   │  MexcTrader         │
                   │  GateTrader         │
                   │  BingXTrader        │
                   │  LBankTrader        │
                   └─────────────────────┘

 Each handler also writes directly to SharedTrades (mutex-protected)
 for the recent-trades column — bypasses the Disruptor.
```

---

## Project Structure

```
trade_station/
├── main.cpp                    — entry point: threads, SharedTrades, render_run()
├── Makefile                    — build script
├── .env.example                — API key template (copy to .env and fill in)
│
├── scr/
│   ├── exchange.h              — abstract base class (Exchange)
│   ├── orderbook.h             — PriceLevel, OrderBookUpdate, SharedDisplay
│   ├── trades.h                — Trade, SharedTrades (recent trades ring buffer)
│   ├── disruptor.h             — lock-free SPMC ring buffer (Disruptor<N>)
│   └── aggregator.h            — aggregator thread: drains Disruptor → SharedDisplay
│
├── mexc/
│   ├── mexc_handler.h/.cpp     — WS: protobuf aggre.depth + aggre.deals
│   └── mexc_trader.h/.cpp      — REST: HMAC-SHA256, X-MEXC-APIKEY header
│
├── gate/
│   ├── gate_handler.h/.cpp     — WS: JSON spot.order_book + spot.trades
│   └── gate_trader.h/.cpp      — REST: HMAC-SHA512 5-line canonical signature
│
├── bingx/
│   ├── bingx_handler.h/.cpp    — WS: gzip-JSON depth20 + trade stream
│   └── bingx_trader.h/.cpp     — REST: HMAC-SHA256, X-BX-APIKEY header
│
├── lbank/
│   ├── lbank_handler.h/.cpp    — WS: JSON depth snapshots + trade stream
│   └── lbank_trader.h/.cpp     — REST: HMAC-SHA256 form-encoded
│
├── trading/
│   ├── order.h                 — Side, OrderRequest, OrderResult, OrderFormState
│   ├── rest_client.h           — hmac_sha256/512, sha512, url_encode, https_post
│   └── trade_manager.h         — loads env vars, owns traders, async submit()
│
├── ui/
│   ├── render.h                — render_run() declaration
│   └── render.cpp              — full ImGui/SDL2/OpenGL3 render loop
│
├── proto/                      — MEXC protobuf generated files (.pb.h / .pb.cc)
└── imgui/                      — Dear ImGui (vendored)
```

---

## Step-by-Step Setup (Ubuntu / WSL2)

### 1 — System packages

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    libssl-dev \
    libboost-all-dev \
    libwebsocketpp-dev \
    nlohmann-json3-dev \
    libprotobuf-dev \
    protobuf-compiler \
    zlib1g-dev \
    libsdl2-dev \
    libgl1-mesa-dev \
    libgles-dev \
    libxrandr-dev \
    libxext-dev \
    pkg-config
```

### 2 — Verify tool versions

```bash
g++ --version          # needs g++ 9+ for C++17
protoc --version       # needs 3.x or later
sdl2-config --version  # needs 2.x
```

### 3 — Clone / place the project

```bash
# If you have the folder already:
cd "/mnt/c/Users/<you>/OneDrive - MultiBank Group/Desktop/Trade_station/trade_station"
```

### 4 — Vendored ImGui (already included)

The `imgui/` directory must contain:

```
imgui/
├── imgui.h / imgui.cpp
├── imgui_draw.cpp
├── imgui_tables.cpp
├── imgui_widgets.cpp
└── backends/
    ├── imgui_impl_sdl2.h / imgui_impl_sdl2.cpp
    ├── imgui_impl_opengl3.h / imgui_impl_opengl3.cpp
    └── imgui_impl_opengl3_loader.h
```

Download from [github.com/ocornut/imgui](https://github.com/ocornut/imgui) (v1.90+) if missing.

### 5 — MEXC Protobuf files

The `proto/` directory must contain the generated `.pb.h` and `.pb.cc` files for MEXC's market data schema. Required files:

```
proto/
├── PushDataV3ApiWrapper.pb.h/.cc
├── PublicAggreDepthsV3Api.pb.h/.cc
├── PublicAggreDealsV3Api.pb.h/.cc
└── ... (other MEXC proto schemas)
```

These are generated from MEXC's official `.proto` files:

```bash
# Download MEXC proto files from their GitHub, then generate:
protoc --cpp_out=proto/ proto/*.proto
```

### 6 — API keys (optional — order book works without them)

```bash
cp .env.example .env
nano .env          # fill in your keys
```

`.env` format:

```bash
export MEXC_API_KEY="your_key"
export MEXC_API_SECRET="your_secret"
export GATE_API_KEY="your_key"
export GATE_API_SECRET="your_secret"
export BINGX_API_KEY="your_key"
export BINGX_API_SECRET="your_secret"
export LBANK_API_KEY="your_key"
export LBANK_API_SECRET="your_secret"
```

Load them before running:

```bash
source .env
```

> Without keys, the order book feed and recent trades still work. The "Place Limit Order" button is simply disabled for any exchange missing credentials.

### 7 — Build

```bash
make
```

Expected output: a single `orderbook_stream` binary. Build time is ~30–60 seconds on first compile (ImGui + all protobuf files).

To clean and rebuild from scratch:

```bash
make clean && make
```

### 8 — Run

```bash
# Make sure keys are loaded if you want trading
source .env

./orderbook_stream
```

A native window opens. The four exchange panels appear as data arrives (usually within 1–2 seconds).

---

## Using the Terminal

### Symbol bar (top of window)

```
Symbol  [ BTC ]  /  [ USDT ]   [ Apply All ]
```

Type any base and quote symbol, then click **Apply All**. All four exchanges unsubscribe from the old pair and subscribe to the new one simultaneously. The order book and trades columns clear immediately.

### Order book panels

Each panel shows:
- **Red rows (asks)** — cheapest ask at the bottom of the block
- **Spread** — best ask minus best bid
- **Green rows (bids)** — best bid at the top of the block
- **Trades** — last 20 trades, newest first, green/red by direction

**Click any row** → fills both Price and Qty in the order form below.

### Order form

1. Toggle **BUY** or **SELL**
2. Adjust Price and Qty if needed
3. Check the calculated **Total**
4. Click **Place Limit Order**

Order status appears inline: yellow "Sending…" → green "OK  \<order-id\>" or red "ERR \<message\>".

---

## Exchange Details

### WebSocket Feeds

| Exchange | URL | Symbol format | Protocol | Stream type |
|---|---|---|---|---|
| MEXC  | `wss://wbs-api.mexc.com/ws` | `BTCUSDT` | Binary Protobuf | Incremental depth deltas + aggre deals |
| Gate  | `wss://api.gateio.ws/ws/v4/` | `BTC_USDT` | JSON | Full snapshots + trades |
| BingX | `wss://open-api-ws.bingx.com/market` | `BTC-USDT` | Gzip JSON | Full snapshots + trades |
| LBank | `wss://www.lbkex.net/ws/V2/` | `btc_usdt` | JSON | Full snapshots + trades |

### REST Order Placement

| Exchange | Endpoint | Auth |
|---|---|---|
| MEXC  | `api.mexc.com/api/v3/order` | HMAC-SHA256 + `X-MEXC-APIKEY` header |
| Gate  | `api.gateio.ws/api/v4/spot/orders` | HMAC-SHA512 5-line canonical sig |
| BingX | `open-api.bingx.com/openApi/spot/v1/trade/order` | HMAC-SHA256 + `X-BX-APIKEY` header |
| LBank | `api.lbkex.com/v2/supplement/create_order.do` | HMAC-SHA256 form-encoded params |

---

## Design Decisions

### Lock-Free Disruptor (order book path)
Exchange threads push `OrderBookUpdate` structs into a `Disruptor<1024>` ring buffer. The aggregator consumer drains it into `SharedDisplay`. Zero mutex in the hot path.

### Separate Trades Path
Recent trade data bypasses the Disruptor entirely. Each handler writes directly to `SharedTrades` (a mutex-protected circular buffer of the last 20 trades per exchange). This keeps the latency-sensitive depth path clean.

### MEXC Protobuf + Incremental Book
MEXC's JSON depth channels are geo-blocked. The protobuf `aggre.depth` channel is not. Each binary frame carries incremental deltas; the handler merges them into a `std::map<double,double>` and publishes a top-5 snapshot after each merge. Trades arrive via the `aggre.deals` protobuf binary channel.

### Aggregator / Render Isolation
The aggregator is the only writer to `SharedDisplay`. The render thread calls `snapshot()` once per frame — a lock-guarded copy — and then draws without holding any lock.

### Non-Blocking Order Placement
Each "Place Limit Order" click dispatches the REST call via `std::async(std::launch::async)`. The UI polls the `std::future` with a zero-timeout `wait_for` each frame. The GUI never stalls.

### Clean Shutdown
A global `std::atomic<bool> g_running` is set to `false` when the ImGui window is closed. All exchange threads, the aggregator, and the trade manager observe this flag and exit before `main()` returns.

---

## Adding a New Exchange

**Market data:**
1. Create `newexchange/newexchange_handler.h/.cpp` — inherit `Exchange`, implement `run()`, `disconnect()`, `subscribe()`, `subscribe_trades()`, `change_symbol()`
2. Add 2 lines in `main.cpp` — construct handler + `set_shared_trades(&shared_trades)` + thread
3. Add source file to `Makefile`

**Order placement:**
1. Create `newexchange/newexchange_trader.h/.cpp` — implement `place_limit_order()`
2. Add env var load + dispatch entry in `trading/trade_manager.h`
3. Add source file to `Makefile`

Zero changes to core infrastructure.

---

## TODO
- Price spread alerts / notifications
- Order cancellation and open-orders panel
- Configurable number of depth levels (currently fixed at 5)
