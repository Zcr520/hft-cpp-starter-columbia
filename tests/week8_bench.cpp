// Supplemental, repeatable Week 8 benchmark; CI's official benchmark is
// tests/rolling_counter_test.cpp, which should remain unchanged.
// One "pair" means one add(timestamp) followed by one count(timestamp).
// Build from repo root:
// c++ -std=c++17 -O2 -pthread -Iinclude tests/week8_bench.cpp -o /tmp/week8_bench
// /tmp/week8_bench
#include "rolling_counter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <vector>

using Clock = std::chrono::steady_clock;

struct DequeCounter {
    explicit DequeCounter(uint64_t w) : window_(w) {}
    void add(uint64_t ts) { data_.push_back(ts); }
    uint64_t count(uint64_t now) {
        if (now >= window_) {
            const uint64_t cutoff = now - window_;
            while (!data_.empty() && data_.front() <= cutoff)
                data_.pop_front();
        }
        return static_cast<uint64_t>(data_.size());
    }
private:
    uint64_t window_;
    std::deque<uint64_t> data_;
};

template <typename T>
inline void use(const T& x) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(x) : "memory");
#else
    static volatile std::uint64_t sink = 0;
    sink = static_cast<std::uint64_t>(x);
#endif
}

static constexpr uint64_t WINDOW = 100'000;
static constexpr uint64_t STEP = 100;
static constexpr int WARMUP = 100'000;
static constexpr int THROUGHPUT_PAIRS = 1'000'000;
static constexpr int LATENCY_PAIRS = 100'000;

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

double percentile(std::vector<double>& v, double p) {
    std::sort(v.begin(), v.end());
    const size_t index = static_cast<size_t>(std::ceil(p * v.size())) - 1;
    return v[index];
}

template<class Counter>
void measure(const char* name) {
    std::vector<double> throughput, p50, p99, p999;
    throughput.reserve(9);
    p50.reserve(5);
    p99.reserve(5);
    p999.reserve(5);

    for (int trial = 0; trial < 9; ++trial) {
        Counter rc(WINDOW);
        uint64_t t = 0;
        for (int i = 0; i < WARMUP; ++i, t += STEP) {
            rc.add(t);
            use(rc.count(t));
        }
        const auto a = Clock::now();
        for (int i = 0; i < THROUGHPUT_PAIRS; ++i, t += STEP) {
            rc.add(t);
            use(rc.count(t));
        }
        const auto b = Clock::now();
        throughput.push_back(
            std::chrono::duration<double, std::nano>(b - a).count()
            / THROUGHPUT_PAIRS
        );
    }

    // Per-pair latency sampling, deliberately separate from throughput.
    // This includes steady_clock read overhead; use relative comparisons.
    for (int trial = 0; trial < 5; ++trial) {
        Counter rc(WINDOW);
        uint64_t t = 0;
        for (int i = 0; i < WARMUP; ++i, t += STEP) {
            rc.add(t);
            use(rc.count(t));
        }
        std::vector<double> samples;
        samples.reserve(LATENCY_PAIRS);
        for (int i = 0; i < LATENCY_PAIRS; ++i, t += STEP) {
            const auto a = Clock::now();
            rc.add(t);
            use(rc.count(t));
            const auto b = Clock::now();
            samples.push_back(
                std::chrono::duration<double, std::nano>(b - a).count());
        }
        p50.push_back(percentile(samples, .50));
        p99.push_back(percentile(samples, .99));
        p999.push_back(percentile(samples, .999));
    }

    std::cout << std::fixed << std::setprecision(2)
              << "BENCH|" << name << "|ns_per_pair|" << median(throughput) << '\n'
              << "BENCH|" << name << "|ns_per_call_amortized|" << median(throughput)/2 << '\n'
              << "BENCH|" << name << "|p50_pair_ns|" << median(p50) << '\n'
              << "BENCH|" << name << "|p99_pair_ns|" << median(p99) << '\n'
              << "BENCH|" << name << "|p99_9_pair_ns|" << median(p999) << '\n';
}

int main() {
    std::cout << "INFO|one_pair|one add plus one count\n";
    std::cout << "INFO|window_ns|" << WINDOW << '\n';
    std::cout << "INFO|event_step_ns|" << STEP << '\n';
    std::cout << "INFO|warmup_pairs|" << WARMUP << '\n';
    std::cout << "INFO|throughput_trials|9\n";
    std::cout << "INFO|latency_trials|5\n";
    measure<DequeCounter>("deque");
    measure<RollingCounter>("ring");
}
