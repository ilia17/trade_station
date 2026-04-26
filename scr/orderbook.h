#ifndef ORDERBOOK_H
#define ORDERBOOK_H

#include <string>
#include <array>

//one price level - price and quantity
struct PriceLevel {
    double price{0.0};
    double quantity{0.0};
};

//one full orderbook snapshot from any exchange

struct OrderBookUpdate {
    char exchange[16];
    char symbol[16];
    std::array<PriceLevel, 5> bids;
    std::array<PriceLevel, 5> asks;
    long timestamp_ms{0};
    int bid_count{0};
    int ask_count{0};
};


#endif