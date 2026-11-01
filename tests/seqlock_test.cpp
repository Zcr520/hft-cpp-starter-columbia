#include "seqlock.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

struct Quote {
    double bid;
    double ask;
    std::uint32_t bq;
    std::uint32_t aq;
};

static Quote quote_for(std::uint32_t k) {
    return {static_cast<double>(k), static_cast<double>(2ULL * k),
            k, static_cast<std::uint32_t>(3ULL * k)};
}

static bool consistent(const Quote& q) {
    return q.ask == 2 * q.bid &&
           q.bq == static_cast<std::uint32_t>(q.bid) &&
           q.aq == 3ULL * q.bq;
}

int main() {
    SeqLock<Quote> slot;
    Quote got{999, 999, 999, 999};
    assert(slot.read(got));
    assert(consistent(got));
    for (std::uint32_t k = 1; k <= 1000; ++k) {
        slot.write(quote_for(k));
        assert(slot.read(got));
        assert(got.bid == static_cast<double>(k));
        assert(consistent(got));
    }
    SeqLock<Quote, true> seq_cst_variant;
    seq_cst_variant.write(quote_for(123));
    assert(seq_cst_variant.read(got));
    assert(consistent(got) && got.bq == 123);
    std::cout << "RESULT|seqlock_single_thread|pass\n";
    std::cout << "RESULT|seqlock_seq_cst_variant|pass\n";

    constexpr std::uint32_t kWrites = 100'000;
    constexpr unsigned kReaders = 4;
    std::atomic<bool> start{false};
    std::atomic<bool> finished{false};
    std::atomic<std::uint64_t> successful{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> inconsistent_count{0};
    std::atomic<std::uint64_t> live_reads{0};

    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (std::uint32_t k = 1001; k <= kWrites; ++k) {
            slot.write(quote_for(k));
            if ((k & 1023U) == 0)
                std::this_thread::yield();
        }
        finished.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (unsigned i = 0; i < kReaders; ++i) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {}
            std::uint64_t ok = 0, fail = 0, bad = 0, live = 0;
            bool done = false;
            // Keep reading for at least 50k successful snapshots per reader;
            // also observe the writer's final publication.
            while (!done || ok < 50'000) {
                Quote q{};
                if (slot.read(q)) {
                    ++ok;
                    if (!consistent(q)) ++bad;
                    if (!finished.load(std::memory_order_acquire)) ++live;
                    if (finished.load(std::memory_order_acquire) &&
                        q.bid == static_cast<double>(kWrites))
                        done = true;
                    if ((ok & 4095ULL) == 0)
                        std::this_thread::yield();
                } else {
                    ++fail;
                    std::this_thread::yield();
                }
            }
            successful.fetch_add(ok, std::memory_order_relaxed);
            rejected.fetch_add(fail, std::memory_order_relaxed);
            inconsistent_count.fetch_add(bad, std::memory_order_relaxed);
            live_reads.fetch_add(live, std::memory_order_relaxed);
        });
    }

    start.store(true, std::memory_order_release);
    writer.join();
    for (auto& t : readers) t.join();

    assert(successful.load() >= 50'000 * kReaders);
    assert(inconsistent_count.load() == 0);
    assert(live_reads.load() > 0);
    assert(slot.read(got));
    assert(got.bid == static_cast<double>(kWrites));
    assert(consistent(got));

    std::cout << "RESULT|seqlock_one_writer_four_readers|pass\n";
    std::cout << "METRIC|successful_reads|" << successful.load() << '\n';
    std::cout << "METRIC|failed_read_calls|" << rejected.load() << '\n';
    std::cout << "METRIC|reads_while_writer_active|" << live_reads.load() << '\n';
    std::cout << "METRIC|inconsistent_snapshots|" << inconsistent_count.load() << '\n';
    return 0;
}
