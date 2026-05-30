#pragma once

// SoA order book. One side = three int64 arrays + a count.
// prices[] and qtys[] are stored best-first (lowest ask / highest bid at [0]).
// cumulative_lots[] is a prefix sum, recomputed after each batch of updates.
//
// We went SoA because AoS (price,qty pairs) was interleaving fields the vectoriser
// couldn't cope with. This way a price sweep is a straight sequential read.
//
// Depth is padded to 24 elements (3 cache lines per array) so SIMD overread
// doesn't hit unmapped memory. The extra 4 slots are always zero.

#include "types.hpp"
#include <cstring>
#include <cassert>

namespace sor {

struct alignas(kCacheLineBytes) LevelSide {
    static constexpr uint32_t kPaddedDepth = 24;

    price_t price_ticks[kPaddedDepth];      // 192 bytes
    qty_t   qty_lots[kPaddedDepth];         // 192 bytes
    qty_t   cumulative_lots[kPaddedDepth];  // 192 bytes

    uint32_t count;
    uint32_t _pad;

    void recompute_cumulative() noexcept {
        qty_t running = 0;
        for (uint32_t i = 0; i < count; ++i) {
            running += qty_lots[i];
            cumulative_lots[i] = running;
        }
    }

    [[nodiscard]] qty_t lots_through_level(uint32_t level) const noexcept {
        const uint32_t clamped = (level < count) ? level : count - 1;
        return cumulative_lots[clamped];
    }
    [[nodiscard]] qty_t total_lots() const noexcept {
        return (count > 0) ? cumulative_lots[count - 1] : 0;
    }
    [[nodiscard]] qty_t lots_at(uint32_t level) const noexcept {
        return (level < count) ? qty_lots[level] : 0;
    }

    void invalidate() noexcept {
        count = 0;
        std::memset(price_ticks,     0, sizeof(price_ticks));
        std::memset(qty_lots,        0, sizeof(qty_lots));
        std::memset(cumulative_lots, 0, sizeof(cumulative_lots));
    }
};
static_assert(alignof(LevelSide) == kCacheLineBytes);
static_assert(sizeof(LevelSide) == 640);  // load-bearing. leave it alone.

// tick_size / lot_size live here (mirrored from LotConstraints) so that
// any analytics path can reconstruct doubles without touching ExchangeState.
struct alignas(kCacheLineBytes) NormalizedBook {
    LevelSide sides[2];   // [0]=BID, [1]=ASK

    uint64_t last_sequence;
    uint64_t last_update_ns;

    double   tick_size;
    double   lot_size;

    uint8_t  exchange_id;
    bool     is_valid;
    uint8_t  _pad[6];

    [[nodiscard]] LevelSide&       bids()       noexcept { return sides[0]; }
    [[nodiscard]] LevelSide&       asks()       noexcept { return sides[1]; }
    [[nodiscard]] const LevelSide& bids() const noexcept { return sides[0]; }
    [[nodiscard]] const LevelSide& asks() const noexcept { return sides[1]; }

    // reporting helpers only — don't call these during routing
    [[nodiscard]] double best_bid_price() const noexcept {
        return static_cast<double>(bids().price_ticks[0]) * tick_size;
    }
    [[nodiscard]] double best_ask_price() const noexcept {
        return static_cast<double>(asks().price_ticks[0]) * tick_size;
    }
    [[nodiscard]] double mid_price() const noexcept {
        return 0.5 * static_cast<double>(bids().price_ticks[0] + asks().price_ticks[0]) * tick_size;
    }
    [[nodiscard]] double spread_bps() const noexcept {
        const double mid = mid_price();
        if (__builtin_expect(mid <= 0.0, 0)) return 0.0;
        return static_cast<double>(asks().price_ticks[0] - bids().price_ticks[0])
               * tick_size / mid * 10000.0;
    }

    // returns false on sequence gap → caller should re-snapshot
    [[nodiscard]] bool apply_sequence(uint64_t incoming_seq) noexcept {
        if (__builtin_expect(last_sequence != 0 && incoming_seq != last_sequence + 1, 0)) {
            is_valid = false;
            return false;
        }
        last_sequence = incoming_seq;
        return true;
    }

    void invalidate() noexcept {
        is_valid = false;
        bids().invalidate();
        asks().invalidate();
    }
};

struct alignas(kCacheLineBytes) ConsolidatedBook {
    NormalizedBook books[kMaxExchanges];
    uint32_t       active_count;
    uint32_t       _pad;
};

} // namespace sor
