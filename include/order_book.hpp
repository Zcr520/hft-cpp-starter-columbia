#ifndef IEOR4741_ORDER_BOOK_HPP
#define IEOR4741_ORDER_BOOK_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Week 7 (HW7): BBO reads are O(1). The expensive work is on updates.
//
// Prices are represented in 1-cent integer ticks. The first accepted order
// centers a FIXED window of 65,536 ticks around the observed price (subject
// to a non-negative base). Unlike an absolute-from-zero array, this supports
// symbols priced above $655.35 without corrupting adjacent memory.
// Orders outside the initial window are REFUSED, without disturbing an existing
// order with the same id. Phase 2 can re-base and re-key resting orders instead.
//
// Level aggregates are uint64_t to avoid wrapping when many uint32_t orders
// rest at one tick. The arrays live on the heap (vector), not on the stack.
struct Book {
private:
    static constexpr std::int64_t kTicksPerDollar = 100;
    static constexpr int kLevels = 1 << 16;
    static constexpr std::uint64_t kEmptyQty = 0;

    struct Rec {
        int slot;
        std::uint32_t qty;
        char side;
    };

    std::vector<std::uint64_t> bid_qty_ = std::vector<std::uint64_t>(kLevels, kEmptyQty);
    std::vector<std::uint64_t> ask_qty_ = std::vector<std::uint64_t>(kLevels, kEmptyQty);
    std::unordered_map<std::uint64_t, Rec> by_id_;
    std::int64_t base_tick_ = 0;
    bool based_ = false;
    int best_bid_slot_ = -1;       // highest occupied bid slot
    int best_ask_slot_ = -1;       // lowest occupied ask slot
    double best_bid_px_ = 0.0;    // cached to make hot-path read a single load
    double best_ask_px_ = 0.0;
    std::uint32_t bid_levels_ = 0; // number of NONEMPTY bid levels
    std::uint32_t ask_levels_ = 0; // number of NONEMPTY ask levels

    static bool price_to_tick(double px, std::int64_t& tick) noexcept {
        // Avoid integer overflow / NaN / inf; price is non-negative on this grid.
        if (!std::isfinite(px) || px < 0.0) return false;
        const double scaled = px * static_cast<double>(kTicksPerDollar);
        if (scaled > static_cast<double>(std::numeric_limits<std::int64_t>::max()) - 1024.0)
            return false;
        tick = static_cast<std::int64_t>(std::llround(scaled));
        return true;
    }

    static double slot_to_price(std::int64_t base, int slot) noexcept {
        return static_cast<double>(base + slot) / static_cast<double>(kTicksPerDollar);
    }

public:
    void add(std::uint64_t id, char side, double px, std::uint32_t qty) {
        if (side != 'B' && side != 'S')
            throw std::invalid_argument("side must be B or S");
        if (qty == 0) return;

        std::int64_t tick = 0;
        if (!price_to_tick(px, tick)) return;

        // Derive a prospective base without mutating the book. In particular,
        // rejecting an out-of-band *replacement* must not cancel its old order.
        const std::int64_t prospective_base = based_
            ? base_tick_
            : std::max<std::int64_t>(0, tick - kLevels / 2);
        const std::int64_t offset = tick - prospective_base;
        if (offset < 0 || offset >= kLevels) return; // guard BOTH ends
        const int i = static_cast<int>(offset);
        if (!based_) {
            base_tick_ = prospective_base;
            based_ = true;
        }

        cancel(id); // repeated id: replace rather than double-count
        auto& qty_by_slot = side == 'B' ? bid_qty_ : ask_qty_;
        auto& occupied = side == 'B' ? bid_levels_ : ask_levels_;
        if (qty_by_slot[static_cast<std::size_t>(i)] == 0) ++occupied;
        qty_by_slot[static_cast<std::size_t>(i)] += qty;
        by_id_.emplace(id, Rec{i, qty, side});

        if (side == 'B') {
            if (i > best_bid_slot_) {
                best_bid_slot_ = i;
                best_bid_px_ = slot_to_price(base_tick_, i);
            }
        } else if (best_ask_slot_ < 0 || i < best_ask_slot_) {
            best_ask_slot_ = i;
            best_ask_px_ = slot_to_price(base_tick_, i);
        }
    }

    void cancel(std::uint64_t id) {
        const auto it = by_id_.find(id);
        if (it == by_id_.end()) return;
        const Rec r = it->second;
        auto& qty_by_slot = r.side == 'B' ? bid_qty_ : ask_qty_;
        auto& occupied = r.side == 'B' ? bid_levels_ : ask_levels_;
        auto& total = qty_by_slot[static_cast<std::size_t>(r.slot)];
        total -= r.qty;
        by_id_.erase(it);
        if (total != 0) return; // the price level still has resting orders
        --occupied;

        if (r.side == 'B' && r.slot == best_bid_slot_) {
            if (occupied == 0) {
                best_bid_slot_ = -1; // empty side: no 32K-slot tail scan
                best_bid_px_ = 0.0;
            } else {
                do { --best_bid_slot_; }
                while (best_bid_slot_ >= 0 && bid_qty_[static_cast<std::size_t>(best_bid_slot_)] == 0);
                best_bid_px_ = slot_to_price(base_tick_, best_bid_slot_);
            }
        } else if (r.side == 'S' && r.slot == best_ask_slot_) {
            if (occupied == 0) {
                best_ask_slot_ = -1;
                best_ask_px_ = 0.0;
            } else {
                do { ++best_ask_slot_; }
                while (best_ask_slot_ < kLevels && ask_qty_[static_cast<std::size_t>(best_ask_slot_)] == 0);
                best_ask_px_ = slot_to_price(base_tick_, best_ask_slot_);
            }
        }
    }

    double best_bid() const noexcept {
        return best_bid_px_;
    }
    double best_ask() const noexcept {
        return best_ask_px_;
    }
};

// Open-addressing linear-probing symbol -> id map (no per-symbol heap nodes).
// Slot owns a null-terminated 17-byte array, so all 16 symbol bytes survive.
// Hash is standard 64-bit FNV-1a; comparisons use strcmp.
// Grow at ~70% load to guarantee empty slots.
struct SymMap {
private:
    struct Slot {
        char key[17]{}; // 16 bytes plus terminator; no truncation
        std::uint64_t id = 0;
        bool used = false;
    };
    std::vector<Slot> slots_;
    std::size_t size_ = 0;

    static std::uint64_t hash_key(const char* s, std::size_t* len = nullptr) {
        if (!s) throw std::invalid_argument("null symbol");
        std::uint64_t h = UINT64_C(14695981039346656037);
        std::size_t n = 0;
        while (n <= 16 && s[n]) {
            h ^= static_cast<unsigned char>(s[n]);
            h *= UINT64_C(1099511628211);
            ++n;
        }
        if (n > 16) throw std::invalid_argument("symbol longer than 16 chars");
        if (len) *len = n;
        return h;
    }

    void insert_no_grow(const char* key, std::size_t len, std::uint64_t h, std::uint64_t id) {
        const auto mask = slots_.size() - 1;
        std::size_t i = static_cast<std::size_t>(h) & mask;
        while (slots_[i].used) i = (i + 1) & mask;
        std::memcpy(slots_[i].key, key, len + 1);
        slots_[i].id = id;
        slots_[i].used = true;
        ++size_;
    }
    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.resize(old.size() * 2);
        size_ = 0;
        for (const Slot& s : old) {
            if (!s.used) continue;
            std::size_t len = 0;
            const auto h = hash_key(s.key, &len);
            insert_no_grow(s.key, len, h, s.id);
        }
    }
public:
    explicit SymMap(std::size_t expected_symbols = 2048) {
        std::size_t capacity = 16;
        while (capacity < expected_symbols + expected_symbols / 2 + 1)
            capacity *= 2;
        slots_.resize(capacity);
    }

    void put(const char* sym, std::uint64_t id) {
        std::size_t len = 0;
        const auto h = hash_key(sym, &len);
        const auto mask = slots_.size() - 1;
        std::size_t i = static_cast<std::size_t>(h) & mask;
        while (slots_[i].used) {
            if (std::strcmp(slots_[i].key, sym) == 0) { slots_[i].id = id; return; }
            i = (i + 1) & mask;
        }
        if ((size_ + 1) * 10 >= slots_.size() * 7) grow();
        insert_no_grow(sym, len, h, id);
    }
    std::uint64_t get(const char* sym) const {
        const auto h = hash_key(sym);
        const auto mask = slots_.size() - 1;
        std::size_t i = static_cast<std::size_t>(h) & mask;
        while (slots_[i].used) {
            if (std::strcmp(slots_[i].key, sym) == 0) return slots_[i].id;
            i = (i + 1) & mask;
        }
        return static_cast<std::uint64_t>(-1);
    }
};

#endif // IEOR4741_ORDER_BOOK_HPP
