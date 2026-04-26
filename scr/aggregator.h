#ifndef AGGREGATOR_H
#define AGGREGATOR_H

#include <atomic>
#include "orderbook.h"
#include "disruptor.h"

inline void aggregator_run(int consumer_id,
                           Disruptor<1024>& disruptor,
                           SharedDisplay& display,
                           std::atomic<bool>& running)
{
    OrderBookUpdate update;
    while (running.load(std::memory_order_relaxed)) {
        if (disruptor.consume(update, consumer_id)) {
            display.update(update);
        }
    }
}

#endif
