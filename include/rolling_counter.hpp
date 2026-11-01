#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// Week 8: exact sliding-window event count.
// Precondition: timestamps passed to add() are non-decreasing; count(now) is
// non-decreasing, and added timestamps are not in the future relative to now.
// A timestamp is live precisely when now - window < ts <= now.
// This class is single-threaded; external synchronization is required if shared.
struct RollingCounter {
    explicit RollingCounter(std::uint64_t window_ns)
        : window_(window_ns), buf_(kInitialCapacity) {}

    // O(1) with preallocated capacity; amortized O(1) even when it grows.
    void add(std::uint64_t ts_ns) {
        if (tail_ - head_ == buf_.size()) {
            grow(); // rare safety fallback: never overwrite a live timestamp
        }
        buf_[tail_ & (buf_.size() - 1)] = ts_ns;
        ++tail_;
    }

    // O(k+1), where k is the number of events expired by this call.
    // Each event expires at most once => O(1) amortized over the stream.
    std::uint64_t count(std::uint64_t now_ns) {
        // The strict boundary is ts > now - window.
        // Do NOT subtract while now < window: uint64_t would underflow.
        // Equality DOES have cutoff zero: ts==0 must expire at now==window.
        if (now_ns >= window_) {
            const std::uint64_t cutoff = now_ns - window_;
            const std::size_t mask = buf_.size() - 1;
            while (head_ != tail_ && buf_[head_ & mask] <= cutoff) {
                ++head_;
            }
        }
        return static_cast<std::uint64_t>(tail_ - head_);
    }

private:
    static constexpr std::size_t kInitialCapacity = std::size_t{1} << 20;
    static_assert((kInitialCapacity & (kInitialCapacity - 1)) == 0,
                  "Ring capacity must be a power of two");

    std::uint64_t window_;
    std::vector<std::uint64_t> buf_; // contiguous, preallocated timestamp ring
    std::size_t head_ = 0;          // logical indices, not physical positions
    std::size_t tail_ = 0;

    void grow() {
        const std::size_t old_capacity = buf_.size();
        if (old_capacity > std::numeric_limits<std::size_t>::max() / 2) {
            throw std::length_error("RollingCounter capacity overflow");
        }
        const std::size_t live = tail_ - head_;
        std::vector<std::uint64_t> larger(old_capacity * 2);
        for (std::size_t i = 0; i < live; ++i) {
            larger[i] = buf_[(head_ + i) & (old_capacity - 1)];
        }
        buf_.swap(larger);
        head_ = 0;
        tail_ = live;
    }
};
