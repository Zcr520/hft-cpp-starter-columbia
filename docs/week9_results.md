# Week 9 Performance Challenge — One-Writer / Many-Reader SeqLock

## Files and implementation

- `seqlock.hpp`: header-only `SeqLock<T>` with a single-writer, multi-reader `write(const T&)` / `bool read(T&)` interface. The default uses acquire/release; `SeqLock<T, true>` uses stronger sequential consistency as a **before-optimization** comparison.
- `seqlock_test.cpp`: single-thread round trips and a concurrent quote-consistency test with one active writer and four readers.
- `seqlock_bench.cpp`: live-writer performance comparison against a `std::shared_mutex` baseline, reporting throughput, sampled latency percentiles, and x86 TSC ticks per reader operation.
- `week9_results.md`: methodology, observed measurements, correctness argument, and limitations.

The workload is an HFT-style quote: `struct Quote { double bid, ask; uint32_t bq, aq; };`. Each publication has `ask = 2 * bid`, `bq = bid`, and `aq = 3 * bq`. This permits every accepted snapshot to be checked for mixed versions.

## Correctness and memory-order reasoning

The **single writer** increments a 64-bit sequence from an even value to an odd value (`fetch_add(1, acq_rel)`), atomically writes all payload words (`store(release)`), and then publishes the next even sequence (`fetch_add(1, release)`). Readers use an acquire load of the sequence before and after acquiring the payload words; a read succeeds only if the two sequence values match and are even. The reader returns `false` after at most eight internal attempts, leaving `out` unchanged on failure; callers retry when necessary. Sequence wraparound within one read is excluded by the intended operating assumptions.

**Why the payload itself is atomic.** A classical seqlock that reads a plain `T` while the writer modifies `T` has a **C++ data race**: checking the sequence *after* a conflicting non-atomic read cannot retroactively remove undefined behavior. Therefore, this C++17 implementation packs `T`'s object representation into `std::atomic<uint64_t>` words. The 64-bit atomic type is required to be lock-free by a static assertion. This also makes the implementation suitable for ThreadSanitizer checks. It supports trivially copyable, default-constructible `T`, not a deep copy of objects referenced through pointers. The writer must remain unique.

**Why acquire/release is sufficient in this implementation.** If the first sequence load sees a published even value, its acquire synchronizes with the final release of that version, so the previously published payload writes happen-before subsequent reader word loads. Atomic coherence prevents an older word value from being observed. If a payload load instead observes a release store from a *newer* writer, that load's acquire synchronizes with the newer store. The newer writer's odd sequence update precedes that store; thus the odd update happens-before the reader's second sequence load, which cannot return the older even sequence. The reader must reject the snapshot. This reasoning **relies on acquire loads for the individual payload words**; changing all payload operations to relaxed without a new proof is not justified. Stronger `seq_cst` is correct but adds unnecessary ordering requirements.

There is no lock in the reader path, but this is not wait-free: repeated interference from a writer may cause failed `read()` calls, and the caller may have to retry. There is no multi-writer guarantee.

## Correctness tests actually performed

Test host: Linux x86-64 execution container. Compilers tested: GCC 14.2.0 (C++17 `-O2`) and Clang 17 (`-O2`). A GCC 14.2.0 ThreadSanitizer build (`-O1 -g -fsanitize=thread`) also finished with **no TSan data-race warnings**. These checks have **not** been run on my Mac M2 or in my GitHub fork yet.

Representative GCC C++17 `-O2` output:

```text
RESULT|seqlock_single_thread|pass
RESULT|seqlock_seq_cst_variant|pass
RESULT|seqlock_one_writer_four_readers|pass
METRIC|inconsistent_snapshots|0
```

The concurrent test executes 100,000 writer updates, starts four readers together, requires each reader to obtain at least 50,000 successful snapshots and to see the final publication, and verifies every returned quote. The exact number of successful/failed reads while the writer runs depends on scheduling. The test and TSan provide evidence; the argument above is the actual memory-model justification.

## Benchmark setup

Measured host: virtualized **Intel Xeon Platinum 8573C**, Linux x86-64, GCC 14.2.0, `-std=c++17 -O2 -pthread`. These are **not** Mac M2 or GitHub CI measurements.

The test uses one continuously publishing writer and **four readers**, with periodic `std::this_thread::yield()` in the writer. A reader retries `read()` if it returns `false`, so the measured completion time includes failed attempts. The baseline uses `std::shared_lock<std::shared_mutex>` for reading and `std::unique_lock` for writing. Each variant gets one warm-up, then five throughput trials of 60,000 successful reads per reader; the output is the median of the five trials. Separately, three latency trials take a `steady_clock` sample around one in four complete reads (including retries) and report p50/p99/p99.9. The clock's own measurement overhead is **included** in those percentiles. The benchmark also prints actual publication counts, since each implementation's achieved writer rate differs under contention.

`reader_ns_per_success` is the sum of the readers' elapsed durations divided by the total number of successful reads; unlike `global_ns_per_success` (overall wall time divided by all reads), this accounts for each reader's elapsed time even when multiple readers overlap. The x86 `__rdtsc()` values are **TSC ticks per successful read**, not necessarily actual variable-frequency CPU core cycles. On ARM64, the program prints `N/A` instead of inventing cycles/op.

### Measured output — representative run

| Metric | `shared_mutex` baseline | `SeqLock<T,true>` before (`seq_cst`) | `SeqLock<T>` after (acquire/release) |
|---|---:|---:|---:|
| Reader ns/success | 367.27 | 61.48 | 47.26 |
| Global wall ns/success | 221.30 | 20.80 | 15.19 |
| TSC ticks/reader success | 844.71 | 141.38 | 108.69 |
| Sampled p50 read (ns) | 105.00 | 30.00 | 30.00 |
| Sampled p99 read (ns) | 1,295.05 | 12,194.01 | 6,509.00 |
| Sampled p99.9 read (ns) | 40,721.26 | 29,800.01 | 12,019.00 |
| Failed `read()` calls in throughput trials | 0 | 3,263,184 | 1,151,012 |
| Successful reads in throughput trials | 1,200,000 | 1,200,000 | 1,200,000 |
| Inconsistent snapshots | 0 | 0 | 0 |

In this **one** run, acquire/release was **1.30x faster** than `seq_cst` (61.48/47.26 reader ns/success) and **7.77x faster** than `shared_mutex` (367.27/47.26). These ratios must not be generalized across all runs or machines.

### Repeatability / scheduling effects

To avoid selecting only the most favorable result, I ran the entire benchmark executable three times. Below each row represents a separate launch (and itself contains five throughput trials, with medians reported).

| Launch | Mutex reader ns/op | Seq-cst reader ns/op | Acquire/release reader ns/op | Mutex TSC ticks/op | Seq-cst TSC ticks/op | Acquire/release TSC ticks/op |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 367.27 | 61.48 | 47.26 | 844.71 | 141.38 | 108.69 |
| 2 | 273.47 | 4.27 | 5.44 | 628.99 | 9.82 | 12.51 |
| 3 | 193.67 | 13.61 | 7.71 | 445.44 | 31.31 | 17.73 |

The shared-mutex version was slower than both seqlock versions in all three launches. Switching from `seq_cst` to acquire/release helped in launches 1 and 3 but **not in launch 2**. This virtualized test machine has significant OS scheduling / contention noise, and the writer achieved different publication counts across variants. Consequently, a claim of a consistent reduction in p99.9 or a fixed speedup from weaker ordering would be unsupported. A controlled repeat on the target Mac, with competing programs closed, is needed for the final personal results.

## Single most important change

The biggest and most repeatable change was **removing reader-side mutex acquisition**, not simply relaxing atomic memory orders. With `std::shared_mutex`, every reader must acquire and release a shared lock and coordinate with a live writer; this creates contention, coherence traffic, and possible blocking, even though the snapshot is tiny. In the atomic-word seqlock, readers perform only atomic loads and sequence comparisons, returning `false` and retrying when a writer intervenes, rather than taking a reader lock. In all three observed launches, both seqlock variants had substantially lower median reader ns/success than the mutex baseline. Replacing sequential consistency with the weaker, proven acquire/release protocol was a secondary optimization whose benefit was workload- and scheduling-dependent.

## Build and reproduce in the course fork

Place `seqlock.hpp` in `include/`, `seqlock_test.cpp` and `seqlock_bench.cpp` in `tests/`, and this note in `docs/`. From the repo root:

```bash
c++ -std=c++17 -O2 -Wall -Wextra -pedantic -pthread -Iinclude \
  tests/seqlock_test.cpp -o /tmp/seqlock_test
/tmp/seqlock_test

c++ -std=c++17 -O2 -pthread -Iinclude \
  tests/seqlock_bench.cpp -o /tmp/seqlock_bench
/tmp/seqlock_bench 4
/tmp/seqlock_bench 1  # optional: one-reader sensitivity test
```

On Linux with GCC, run TSan separately (do **not** use the sanitizer build for performance numbers):

```bash
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -Iinclude \
  tests/seqlock_test.cpp -o /tmp/seqlock_test_tsan
TSAN_OPTIONS=halt_on_error=1 /tmp/seqlock_test_tsan
```

On an Apple Silicon Mac, `__rdtsc` does not exist. The same benchmark provides ns/op from `steady_clock` and reports TSC ticks as N/A. To obtain true hardware CPU-cycle counts on ARM64, use a suitable hardware performance-counter profiler; do not convert ns/op to cycles/op using an assumed frequency without explicitly labeling it as an estimate.

Finally, a successful run of the existing `grade.yml` does **not** by itself prove this standalone Week 9 challenge passed: verify that the workflow actually compiles/runs the new test. The code here has not been pushed to the fork.
