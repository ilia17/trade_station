#include <iostream>
#include <thread>
#include "scr/disruptor.h"
#include "mexc/mexc_handler.h"

// Shared disruptor — 1024 slots
Disruptor<1024> disruptor;

// Display consumer — reads from disruptor and prints
void display(int consumer_id) {
    OrderBookUpdate update;
    std::cout << "Display thread started\n";
    while (true) {
        if (disruptor.consume(update, consumer_id)) {
            std::cout << "\n[" << update.exchange << "] " << update.symbol << "\n";
            std::cout << "BIDS:\n";
            for (int i = 0; i < update.bid_count; i++) {
                std::cout << "  " << update.bids[i].price
                          << " | " << update.bids[i].quantity << "\n";
            }
            std::cout << "ASKS:\n";
            for (int i = 0; i < update.ask_count; i++) {
                std::cout << "  " << update.asks[i].price
                          << " | " << update.asks[i].quantity << "\n";
            }
        }
    }
}

int main() {
    // register display consumer
    int consumer_id = disruptor.add_consumer();

    // start display thread
    std::thread display_thread(display, consumer_id);

    // start MEXC
    MexcHandler mexc(disruptor);
    std::thread mexc_thread([&mexc]{ mexc.run(); });

    mexc_thread.join();
    display_thread.join();

    return 0;
}