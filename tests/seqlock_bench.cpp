#include "seqlock.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define HAS_RDTSC 1
#else
#define HAS_RDTSC 0
#endif

struct Quote {
    double bid;
    double ask;
    std::uint32_t bq;
    std::uint32_t aq;
};

static Quote make_quote(std::uint32_t k) {
    return {static_cast<double>(k), static_cast<double>(2ULL * k),
            k, static_cast<std::uint32_t>(3ULL * k)};
}

// Read-mostly baseline: multiple readers may hold the shared lock,
// but all readers coordinate with a writer through a real mutex.
template<class T>
struct SharedMutexQuote {
    mutable std::shared_mutex mutex_;
    T quote_{};

    void write(const T& q) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        quote_ = q;
    }
    bool read(T& out) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        out = quote_;
        return true;
    }
};

using Clock = std::chrono::steady_clock;

#if HAS_RDTSC
static inline std::uint64_t tick_count() {
    _mm_lfence();
    const std::uint64_t ticks = __rdtsc();
    _mm_lfence();
    return ticks;
}
#endif

static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double idx = (v.size() - 1) * p;
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - lo);
}

struct Trial {
    double ns_per_reader_success = 0;
    double ns_per_global_success = 0;
    double cycles_per_reader_success = 0;
    std::uint64_t writers = 0;
    std::uint64_t retry_calls = 0;
    std::uint64_t inconsistent = 0;
    std::uint64_t successful = 0;
    std::vector<double> latency_ns;
};

template<class Buffer>
static Trial run_trial(unsigned readers, unsigned reads_per_reader,
                       bool collect_latency) {
    Buffer buffer;
    buffer.write(make_quote(1));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> published{0};
    std::atomic<std::uint64_t> checksum{0};
    std::thread writer([&] {
        std::uint64_t k = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            // Limit k so all quote fields remain exactly representable.
            const auto value = static_cast<std::uint32_t>(
                (k++ % 1'000'000ULL) + 1);
            buffer.write(make_quote(value));
            if ((k & 63ULL) == 0) {
                published.fetch_add(64, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
    });

    // Guarantee that the writer is live before starting reader measurements.
    while (published.load(std::memory_order_relaxed) < 128)
        std::this_thread::yield();

    struct ReaderResult {
        double ns = 0;
        std::uint64_t cycles = 0;
        std::uint64_t retries = 0;
        std::uint64_t bad = 0;
        std::uint64_t sum = 0;
        std::vector<double> samples;
    };
    std::vector<ReaderResult> output(readers);
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(readers);

    for (unsigned r = 0; r < readers; ++r) {
        workers.emplace_back([&, r] {
            ReaderResult& result = output[r];
            if (collect_latency)
                result.samples.reserve(reads_per_reader / 4 + 2);
            while (!start.load(std::memory_order_acquire)) {}
            const auto begin = Clock::now();
#if HAS_RDTSC
            const auto begin_cycles = tick_count();
#endif
            for (unsigned i = 0; i < reads_per_reader; ++i) {
                const bool sample = collect_latency && (i % 4 == 0);
                const auto t0 = sample ? Clock::now() : Clock::time_point{};
                Quote q{};
                while (!buffer.read(q)) ++result.retries;
                if (sample) {
                    const auto t1 = Clock::now();
                    result.samples.push_back(
                        std::chrono::duration<double, std::nano>(t1 - t0).count());
                }
                if (q.ask != 2.0 * q.bid ||
                    q.bq != static_cast<std::uint32_t>(q.bid) ||
                    q.aq != 3ULL * q.bq)
                    ++result.bad;
                result.sum += q.bq;
            }
#if HAS_RDTSC
            const auto end_cycles = tick_count();
            result.cycles = end_cycles - begin_cycles;
#endif
            const auto end = Clock::now();
            result.ns = std::chrono::duration<double, std::nano>(end - begin).count();
            checksum.fetch_add(result.sum, std::memory_order_relaxed);
        });
    }

    const auto global_start = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    const auto global_end = Clock::now();
    stop.store(true, std::memory_order_relaxed);
    writer.join();

    Trial trial;
    trial.successful = std::uint64_t(readers) * reads_per_reader;
    trial.writers = published.load(std::memory_order_relaxed);
    double total_reader_ns = 0;
    double total_reader_cycles = 0;
    for (auto& r : output) {
        trial.retry_calls += r.retries;
        trial.inconsistent += r.bad;
        total_reader_ns += r.ns;
        total_reader_cycles += static_cast<double>(r.cycles);
        trial.latency_ns.insert(trial.latency_ns.end(),
                                r.samples.begin(), r.samples.end());
    }
    trial.ns_per_reader_success = total_reader_ns / trial.successful;
    trial.ns_per_global_success =
        std::chrono::duration<double, std::nano>(global_end - global_start)
            .count() / trial.successful;
#if HAS_RDTSC
    trial.cycles_per_reader_success = total_reader_cycles / trial.successful;
#endif
    // Prevent eliminating the read loop or its results.
    if (checksum.load(std::memory_order_relaxed) == UINT64_MAX)
        std::cerr << "checksum sentinel\n";
    return trial;
}

template<class Buffer>
static void report(const char* name, unsigned nreaders) {
    // One warm-up, then 5 throughput trials and 3 separately instrumented
    // latency trials. p50/p99/p99.9 include the clock sampling overhead.
    run_trial<Buffer>(nreaders, 10'000, false);
    std::vector<double> reader_ns, global_ns, cycles;
    std::uint64_t retries = 0, successes = 0, writer_updates = 0, errors = 0;
    for (int i = 0; i < 5; ++i) {
        Trial t = run_trial<Buffer>(nreaders, 60'000, false);
        reader_ns.push_back(t.ns_per_reader_success);
        global_ns.push_back(t.ns_per_global_success);
        cycles.push_back(t.cycles_per_reader_success);
        retries += t.retry_calls;
        successes += t.successful;
        writer_updates += t.writers;
        errors += t.inconsistent;
    }
    std::vector<double> latencies;
    for (int i = 0; i < 3; ++i) {
        Trial t = run_trial<Buffer>(nreaders, 25'000, true);
        errors += t.inconsistent;
        latencies.insert(latencies.end(), t.latency_ns.begin(), t.latency_ns.end());
    }
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "BENCH|" << name << "|reader_ns_per_success|"
              << percentile(reader_ns, 0.5) << '\n';
    std::cout << "BENCH|" << name << "|global_ns_per_success|"
              << percentile(global_ns, 0.5) << '\n';
#if HAS_RDTSC
    std::cout << "BENCH|" << name << "|tsc_ticks_per_reader_success|"
              << percentile(cycles, 0.5) << '\n';
#else
    std::cout << "BENCH|" << name
              << "|tsc_ticks_per_reader_success|N/A (non-x86)\n";
#endif
    std::cout << "BENCH|" << name << "|p50_read_ns|"
              << percentile(latencies, 0.5) << '\n';
    std::cout << "BENCH|" << name << "|p99_read_ns|"
              << percentile(latencies, 0.99) << '\n';
    std::cout << "BENCH|" << name << "|p99_9_read_ns|"
              << percentile(latencies, 0.999) << '\n';
    std::cout << "BENCH|" << name << "|retry_calls|" << retries << '\n';
    std::cout << "BENCH|" << name << "|successful_reads|" << successes << '\n';
    std::cout << "BENCH|" << name << "|writer_publishes_approx|"
              << writer_updates << '\n';
    std::cout << "BENCH|" << name << "|inconsistent_snapshots|" << errors << '\n';
}

int main(int argc, char** argv) {
    unsigned nreaders = 4;
    if (argc > 1)
        nreaders = static_cast<unsigned>(std::stoul(argv[1]));
    if (nreaders == 0 || nreaders > 16) {
        std::cerr << "readers must be in [1,16]\n";
        return 1;
    }
    std::cout << "INFO|threads|1 writer, " << nreaders << " readers\n";
    std::cout << "INFO|build|C++17 -O2 -pthread\n";
    std::cout << "INFO|throughput_trials|5 measured + 1 warmup\n";
    std::cout << "INFO|latency_trials|3 measured, one in four read samples\n";
    std::cout << "INFO|tsc|invariant TSC ticks if x86_64, not calibrated core cycles\n";
    report<SharedMutexQuote<Quote>>("shared_mutex", nreaders);
    report<SeqLock<Quote, true>>("seqlock_seq_cst_before", nreaders);
    report<SeqLock<Quote>>("seqlock_acq_rel_after", nreaders);
}
