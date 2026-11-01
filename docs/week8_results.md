# IEOR E4741 — Week 8 Performance Challenge

## Files and purpose

- `include/rolling_counter.hpp`: my amortized-O(1) sliding-window event counter, using a power-of-two circular array with monotonically advancing logical indices.
- `tests/week8_extra_test.cpp`: additional boundary/wraparound/capacity correctness checks, plus the Lab's Welford running mean and sample-variance exercise (five hand-checked values and 1,000 samples).
- `tests/week8_bench.cpp`: comparison between `std::deque` and the optimized ring, including amortized throughput and per-pair p50/p99/p99.9 latency.

The course's `tests/rolling_counter_test.cpp`, `tests/bench.hpp`, `tests/run_ci.py`, and `.github/workflows/grade.yml` are unchanged.

## Algorithm, boundary, and complexity

The event timestamps are inserted in non-decreasing order, and `count(now_ns)` is called with a non-decreasing clock. All added events are assumed not to be in the future relative to the query. The exact window is `(now_ns - window_ns, now_ns]`: an event on the *left* boundary is expired.

I preallocate a power-of-two ring of 2^20 timestamps. `add(ts)` inserts at `tail & (capacity-1)` and increments `tail`. `count(now)` advances `head` only while the oldest timestamp is expired. The current number of events is `tail - head`. To avoid unsigned underflow, I compute `cutoff = now - window` only when `now >= window`; equality is important, since timestamp 0 expires exactly at `now == window`.

Each timestamp is inserted once and removed at most once. Across `A` adds and `Q` counts, the cost is `O(A+Q)` plus occasional geometrically growing capacity, hence **amortized O(1)** per operation. Individual `count()` calls can take O(k) when k timestamps expire together. Initial allocation is outside the hot path; in the usual bounded workload, add/count do not allocate. If a window ever exceeds the capacity, I grow the ring by 2x and copy live timestamps rather than silently overwrite live events.

The ring's contiguous storage and bitmask indexing eliminate deque block management and pointer/block transitions on the hot path. Unlike the initial capacity, worst-case burst expiry is not bounded to constant time: amortized O(1) is not a worst-case guarantee.

## Correctness tests

Command from repository root:

```bash
make rolling
c++ -std=c++17 -O2 -Wall -Wextra -pedantic -Iinclude \
  tests/week8_extra_test.cpp -o /tmp/week8_extra
/tmp/week8_extra
```

The course's current public test logic (`rc_window`, `rc_mixed`) was replicated locally and passed. The extra checks all passed on the reference Linux build:

```
RESULT|rc_lab_window|pass
RESULT|rc_cutoff_equality|pass
RESULT|rc_lab_mixed|pass
RESULT|rc_empty_after_expiry|pass
RESULT|rc_duplicates|pass
RESULT|rc_duplicate_expiry|pass
RESULT|rc_zero_window|pass
RESULT|rc_wraparound|pass
RESULT|rc_capacity_growth|pass
RESULT|rc_growth_expiry|pass
RESULT|welford_five_values|pass
RESULT|welford_1000_values|pass
```

### Welford lab extension

I used Welford's one-pass updates:

- `delta = x - mean; mean += delta / n; m2 += delta * (x - mean)`
- Sample variance is `m2 / (n-1)`, and sample standard deviation is its square root.

For `1,2,3,4,5`: mean = 3, sample variance = 2.5, sample stddev ≈ 1.58114. For 1,000 observations `0..999`: mean = 499.5, sample variance ≈ 83416.666667, sample stddev ≈ 288.819. The Welford state uses O(1) memory and O(1) work per observation.

## Benchmark methodology

Reference run on **Linux x86-64, GCC 14.2.0, `-std=c++17 -O2 -pthread`**, not on my local Mac or the GitHub Actions runner. Therefore, the numbers below are *reference measurements*, not a claim about the eventual CI machine. The exact performance will vary with CPU, compiler, frequency scaling and workload.

- Window = `100,000 ns`, event separation = `100 ns`, steady-state ~1,000 live timestamps.
- An *operation pair* means one `add(ts)` followed by one `count(ts)`.
- Warm up 100,000 pairs before measurement.
- Throughput: nine independent runs of one million pairs; report median nanoseconds per pair. The course's `METRIC|rolling_ns_per_op` similarly times an `add()+count()` lambda as **one** benchmark iteration.
- Latency: five independent 100,000-pair runs with per-pair `steady_clock` sampling; report the median across runs of p50/p99/p99.9. These samples include timer overhead and are best used for *relative* comparisons.
- Keep the workload, initial state, compiler optimization and output-consumption method identical for deque and ring.

Command:

```bash
c++ -std=c++17 -O2 -pthread -Iinclude \
  tests/week8_bench.cpp -o /tmp/week8_bench
/tmp/week8_bench
```

### Measured results (reference Linux run)

| Implementation | ns / add+count pair | ns / call (pair divided by 2) | p50 pair ns | p99 pair ns | p99.9 pair ns |
|---|---:|---:|---:|---:|---:|
| std::deque baseline | 3.14 | 1.57 | 24 | 47 | 63 |
| Preallocated ring | 1.79 | 0.89 | 23 | 26 | 40 |

Measured throughput speedup: `3.14 / 1.79 = 1.75x`. Latency percentiles are clock-instrumented pair latencies, so do not directly compare them with amortized throughput latency. The exact CI metric should be copied from the Actions Summary when it runs.

### Most helpful change

The biggest single change was replacing the deque with a preallocated, contiguous power-of-two ring. Instead of maintaining deque blocks, I store timestamps in one array and advance logical `head` and `tail` indices using a cheap bitmask for physical addressing. In the tested steady-state workload, this reduced the median throughput cost from 3.14 to 1.79 ns per `add()+count()` pair (about 1.75x), while reducing the measured p99.9 pair latency from 63 to 40 ns. The algorithm remains amortized O(1) in both implementations; the gain is primarily from data layout and simpler hot-path bookkeeping.

## Fork CI procedure

1. Copy only my header and supplemental files into the *same relative paths* of the fork checkout; do not edit the course's original tests/autograder.
2. Run `make rolling` and the two supplementary binaries locally. `make test` runs all course challenges, so unrelated incomplete stubs may show failures; inspect the RollingCounter rows specifically.
3. Commit and push to `origin main` in my fork `Zcr520/hft-cpp-starter-columbia`.
4. Open `https://github.com/Zcr520/hft-cpp-starter-columbia/actions`, select `autograde`, then inspect the latest run's **Summary**, including `RollingCounter: window count`, `RollingCounter: expiry as clock advances`, and `rolling_ns_per_op`.
5. Download the `report` artifact if I need the machine-readable `report.json`.

The fork already has `.github/workflows/grade.yml` configured for `push`, `pull_request`, and `workflow_dispatch`, so I do not need to modify that file. A green workflow job is not a guarantee that every course task is complete: the autograder prints a per-test score and can finish successfully despite unrelated unimplemented challenge stubs.
