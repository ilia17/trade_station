#ifndef DISRUPTOR_H
#define DISRUPTOR_H

#include <atomic>
#include <array>
#include <thread>
#include <climits>
#include "orderbook.h"


template<int SIZE>
class Disruptor {
    static_assert((SIZE & (SIZE - 1)) == 0, "SIZE must be a power of 2");

    private:
        static const int MASK = SIZE - 1;

        // Ring buffer — pre-allocated, zero heap in hot path
        std::array<OrderBookUpdate, SIZE> buffer;
        
         // Producer sequence
        std::atomic<long> producer_sequence{-1};


        // Consumer sequences — pre-allocated for max 64 consumers
        static const int MAX_CONSUMERS = 64;
        std::atomic<long> consumer_sequences[MAX_CONSUMERS];
        std::atomic<bool> consumer_active[MAX_CONSUMERS];
        std::atomic<int> consumer_count{0};

        long find_minimum() {
            long min = LONG_MAX;
            int count = consumer_count.load(std::memory_order_acquire);
            for (int i = 0; i < count; i++) {
                if (consumer_active[i].load(std::memory_order_acquire)) {
                    long cs = consumer_sequences[i].load(std::memory_order_acquire);
                    if (cs < min) min = cs;
                }
            }
            // if no consumers yet — don't block producer
            return (min == LONG_MAX) ? producer_sequence.load() - SIZE : min;
        }


    public:
        Disruptor() {
            for (int i = 0; i < MAX_CONSUMERS; i++) {
                consumer_sequences[i].store(-1,std::memory_order_relaxed);
                consumer_active[i].store(false,std::memory_order_relaxed);
            }
        }

        // Add a new consumer — returns consumer ID
        int add_consumer() {
            int id = consumer_count.fetch_add(1,std::memory_order_seq_cst);
            consumer_sequences[id].store(
                producer_sequence.load(std::memory_order_acquire),
                std::memory_order_release
            );
            consumer_active[id].store(true,std::memory_order_release);
            return id;
        }

        // Producer — publish one update
        void publish(const OrderBookUpdate& update) {
            long next = producer_sequence.load(std::memory_order_relaxed) + 1;

            //wait untill slowest consumer has moved past this slot
            while (next - find_minimum() >= SIZE){
                std::this_thread::yield();
            }

            buffer[next & MASK] = update;
            producer_sequence.store(next,std::memory_order_release);
        }

        // Consumer - read next update for this consumer ID
        bool consume(OrderBookUpdate& update, int consumer_id) {
            long next = consumer_sequences[consumer_id].load(std::memory_order_relaxed) + 1;

            if (next > producer_sequence.load(std::memory_order_acquire)){
                return false;
            }

            update = buffer[next & MASK];
            consumer_sequences[consumer_id].store(next,std::memory_order_release);
            return true;
        }



    };

#endif