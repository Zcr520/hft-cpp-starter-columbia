#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// One writer, many readers. No reader-side mutex, and no non-atomic
// access to storage that the writer might concurrently modify.
//
// This is a C++17-friendly atomic-word seqlock, not the classic
// (data-racy in ISO C++) seqlock over a plain struct T.
template <class T, bool StrongOrdering = false>
struct SeqLock {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SeqLock<T> requires trivially copyable T");
    static_assert(std::is_default_constructible_v<T>,
                  "SeqLock<T> requires default-constructible T");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "SeqLock<T> requires lock-free 64-bit atomics");

    static constexpr std::size_t kWords =
        (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
    static constexpr unsigned kReadAttempts = 8;

    // The StrongOrdering variant is a correctness-preserving "before"
    // benchmark. The default uses just the acquire/release edges needed
    // for publication and a consistent snapshot.
    static constexpr std::memory_order kStartOrder =
        StrongOrdering ? std::memory_order_seq_cst
                       : std::memory_order_acq_rel;
    static constexpr std::memory_order kPayloadWriteOrder =
        StrongOrdering ? std::memory_order_seq_cst
                       : std::memory_order_release;
    static constexpr std::memory_order kPayloadReadOrder =
        StrongOrdering ? std::memory_order_seq_cst
                       : std::memory_order_acquire;
    static constexpr std::memory_order kSequenceReadOrder =
        StrongOrdering ? std::memory_order_seq_cst
                       : std::memory_order_acquire;
    static constexpr std::memory_order kFinishOrder =
        StrongOrdering ? std::memory_order_seq_cst
                       : std::memory_order_release;

    SeqLock() noexcept {
        // Initialize the actual object representation of T{} (which need
        // not be all-bits-zero for every trivially copyable T).
        const T initial{};
        std::array<std::uint64_t, kWords> local{};
        std::memcpy(local.data(), &initial, sizeof(T));
        for (std::size_t i = 0; i < kWords; ++i)
            words_[i].store(local[i], std::memory_order_relaxed);
    }

    SeqLock(const SeqLock&) = delete;
    SeqLock& operator=(const SeqLock&) = delete;

    // Only one thread may call write(). No concurrent writers.
    void write(const T& value) noexcept {
        std::array<std::uint64_t, kWords> local{};
        std::memcpy(local.data(), &value, sizeof(T));

        // Even -> odd. acq_rel establishes an ordering edge between this
        // marker and all following payload operations (including on ARM).
        seq_.fetch_add(1, kStartOrder);

        // Atomic payload avoids the ISO C++ data race of a plain struct
        // read concurrently with a writer. Each release/acquire pair also
        // makes a seen future payload imply visibility of its odd marker.
        for (std::size_t i = 0; i < kWords; ++i)
            words_[i].store(local[i], kPayloadWriteOrder);

        // Odd -> even: publishes the completed version.
        seq_.fetch_add(1, kFinishOrder);
    }

    // Bounded, lock-free reader. On false, 'out' is left untouched;
    // the caller may retry. In particular, no reader waits on a mutex.
    bool read(T& out) const noexcept {
        for (unsigned attempt = 0; attempt < kReadAttempts; ++attempt) {
            const std::uint64_t before =
                seq_.load(kSequenceReadOrder);
            if (before & 1U)
                continue;

            std::array<std::uint64_t, kWords> local{};
            for (std::size_t i = 0; i < kWords; ++i)
                local[i] = words_[i].load(kPayloadReadOrder);

            const std::uint64_t after =
                seq_.load(kSequenceReadOrder);
            if (before != after)
                continue;

            std::memcpy(&out, local.data(), sizeof(T));
            return true;
        }
        return false;
    }

private:
    alignas(64) std::atomic<std::uint64_t> seq_{0};
    alignas(64) std::array<std::atomic<std::uint64_t>, kWords> words_{};
};
