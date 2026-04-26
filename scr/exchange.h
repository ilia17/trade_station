# Multi-Exchange Order Book Streamer

A high-performance, low-latency C++ order book streaming system that aggregates real-time market data from 4 cryptocurrency exchanges simultaneously using WebSocket connections and a lock-free Disruptor ring buffer.

---

## Architecture

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│    MEXC      │     │    Gate     │     │   LBank     │     │   BingX     │
│  WebSocket  │     │  WebSocket  │     │  WebSocket  │     │  WebSocket  │
└──────┬──────┘     └──────┬──────┘     └──────┬──────┘     └──────┬──────┘
       │                   │                   │                   │
       └───────────────────┴───────────────────┴───────────────────┘
                                       │
                            ┌──────────▼──────────┐
                            │   Disruptor Ring     │
                            │      Buffer          │
                            │  (Lock Free SPMC)    │
                            └──────────┬──────────┘
                                       │
                            ┌──────────▼──────────┐
                            │   Display Consumer   │
                            │  (Order Book View)   │
                            └─────────────────────┘
```

---

## Project Structure

```
orderbook_stream/
├── bingx/
│   ├── bingx_handler.h      — BingX WebSocket class declaration
│   └── bingx_handler.cpp    — BingX connection + JSON parsing
├── gate/
│   ├── gate_handler.h       — Gate WebSocket class declaration
│   └── gate_handler.cpp     — Gate connection + JSON parsing
├── lbank/
│   ├── lbank_handler.h      — LBank WebSocket class declaration
│   └── lbank_handler.cpp    — LBank connection + JSON parsing
├── mexc/
│   ├── mexc_handler.h       — MEXC WebSocket class declaration
│   └── mexc_handler.cpp     — MEXC connection + JSON parsing
├── src/
│   ├── orderbook.h          — OrderBookUpdate + PriceLevel structs
│   ├── disruptor.h          — Lock free ring buffer (SPMC Disruptor)
│   └── exchange.h           — Abstract base class for all exchanges
├── main.cpp                 — Entry point, wires all components
├── Makefile                 — Build script
└── README.md                — This file
```

---

## Design Decisions

### Lock Free Disruptor
All four exchange threads push `OrderBookUpdate` objects into a shared Disruptor ring buffer. The display consumer reads from the ring buffer independently. No mutex in the hot path. Write once, read many.

### Abstract Exchange Base Class
Each exchange inherits from a common `Exchange` base class with pure virtual methods: `connect()`, `subscribe()`, `disconnect()`, `on_message()`. Adding a new exchange means implementing one class — zero changes to main or infrastructure.

### Pre-allocated Memory
`OrderBookUpdate` objects are pre-allocated in the ring buffer at startup. Zero heap allocation during trading. No `new` or `delete` in the hot path.

### One Thread Per Exchange
Each exchange runs on its own dedicated thread. MEXC slow or disconnected — Gate, LBank, BingX are completely unaffected. Full isolation.

### Async Logging
Exchange threads never write to cout directly. Log messages pushed to a lock free queue. Separate logger thread reads and prints. Zero IO in the hot path.

---

## Dependencies

| Library | Purpose | Install |
|---|---|---|
| libssl / libcrypto | TLS for secure WebSocket | `sudo apt-get install libssl-dev` |
| libboost | Required by websocketpp | `sudo apt-get install libboost-all-dev` |
| websocketpp | WebSocket client | `sudo apt-get install libwebsocketpp-dev` |
| nlohmann/json | JSON parsing | `sudo apt-get install nlohmann-json3-dev` |

---

## WebSocket Endpoints

| Exchange | WebSocket URL | Symbol Format |
|---|---|---|
| MEXC | wss://wbs.mexc.com/ws | BTC_USDT |
| Gate | wss://api.gateio.ws/ws/v4/ | BTC_USDT |
| LBank | wss://www.lbkex.net/ws/V2/ | btc_usdt |
| BingX | wss://open-api-ws.bingx.com/market | BTC-USDT |

---

## Build

```bash
make
./orderbook_stream
```

---

## Data Flow

```
1. Exchange thread connects to WebSocket endpoint
2. Subscribes to BTC/USDT order book channel
3. Receives JSON message on every book update
4. Parses top 5 bid and ask levels
5. Constructs OrderBookUpdate struct
6. Pushes to Disruptor ring buffer (lock free)
7. Display consumer reads from ring buffer
8. Prints aggregated order book to terminal
```

---

## Performance Characteristics

| Metric | Target |
|---|---|
| Hot path allocations | Zero |
| Mutex in hot path | Zero |
| Latency per update | < 1 microsecond (internal) |
| Ring buffer size | 1024 slots (power of 2) |
| Max exchanges | 64 (pre-allocated) |

---

## Extending — Adding a New Exchange

1. Create a new folder: `mkdir newexchange/`
2. Create `newexchange/newexchange_handler.h` — inherit from `Exchange`
3. Create `newexchange/newexchange_handler.cpp` — implement 4 virtual methods
4. Add thread in `main.cpp` — 3 lines
5. Add to Makefile — 1 line

Zero changes to core infrastructure.


## TODO 
will implement trading for each exchange later
---

