#pragma once

// per-exchange state: book, latency tracker, lot constraints.
// also home to RoutingContext — the thing you fill in before calling the engine.
//
// thread model: network thread writes book + updates EWMA, routing thread reads.
// the only shared mutable state is ewma_rtt_us (atomic<double>). everything else
// is written once at init or between decision cycles.

#include "types.hpp"
#include "normalized_book.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace sor {

// EWMA latency with a regime-aware penalty.
//
// original model was flat: penalty = k * rtt. fine in calm markets.
// problem: deribit at 820us during a CPI print is NOT the same as deribit at
// 820us at 3am. the flat model doesn't know the difference and happily routes
// there while HFTs are sweeping the book. so now we scale by vol and imbalance.
//
// penalty = base(rtt) * (1 + 2.0*vol) * (1 + 1.5*imb)
// at vol=1, imb=1 that's a 7.5x multiplier. deribit becomes expensive. good.
//
// kVolSensitivity and kImbSensitivity are eyeballed from backtests. calibrate
// per-exchange properly if you have the data. we didn't. good enough for now.
struct alignas(kCacheLineBytes) LatencyTracker {
    static constexpr double kAlpha           = 0.1;
    static constexpr double kMaxRttUs        = 10000.0;
    static constexpr double kPenaltyBpsPerUs = 0.001;
    static constexpr double kVolSensitivity  = 2.0;
    static constexpr double kImbSensitivity  = 1.5;

    std::atomic<double>   ewma_rtt_us{500.0};  // conservative default
    std::atomic<uint64_t> sample_count{0};

    void update(double rtt_us) noexcept {
        const double capped  = (rtt_us < kMaxRttUs) ? rtt_us : kMaxRttUs;
        const double current = ewma_rtt_us.load(std::memory_order_relaxed);
        ewma_rtt_us.store(kAlpha * capped + (1.0 - kAlpha) * current,
                          std::memory_order_release);
        sample_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] double get_rtt_us() const noexcept {
        return ewma_rtt_us.load(std::memory_order_acquire);
    }

    // kept for the adverse-selection estimate in greedy_fill (static, no regime context needed)
    [[nodiscard]] double latency_cost_adj() const noexcept {
        return kPenaltyBpsPerUs * get_rtt_us() * 1e-4;
    }

    // vol_factor: normalised short-term realised vol. 0=quiet, ~1=2σ event.
    // directional_imb: imbalance leaning against our order direction. range [0,1].
    [[nodiscard]] double dynamic_cost_adj(double vol_factor,
                                          double directional_imb) const noexcept {
        const double base      = kPenaltyBpsPerUs * get_rtt_us() * 1e-4;
        const double vol_scale = 1.0 + kVolSensitivity * vol_factor;
        const double imb_pos   = (directional_imb > 0.0) ? directional_imb : 0.0;
        return base * vol_scale * (1.0 + kImbSensitivity * imb_pos);
    }
};
static_assert(alignof(LatencyTracker) == kCacheLineBytes);

struct LotConstraints {
    double lot_size;
    double tick_size;
    double min_qty;
    double max_qty;
    double max_notional;
};

struct alignas(kCacheLineBytes) ExchangeState {
    NormalizedBook  book;
    LatencyTracker  latency;
    LotConstraints  lot;
    FeeMatrix       fees;   // duplicated per-exchange for data locality

    uint8_t exchange_id;
    bool    enabled;
    uint8_t _pad[6];

    ExchangeState()                                 = default;
    ExchangeState(const ExchangeState&)             = delete;
    ExchangeState& operator=(const ExchangeState&) = delete;
    ExchangeState(ExchangeState&&)                  = delete;
    ExchangeState& operator=(ExchangeState&&)       = delete;
};

// snapshot of everything the engine needs for one routing decision.
//
// target_lots: order size in integer lots — the engine doesn't know what a "Bitcoin" is.
// caller converts via to_lots() before filling this in.
//
// short_vol_factor / book_imbalance: if you don't have these yet, pass 0 for both.
// the cost model degrades gracefully to the original static penalty.
//
// TODO: multi-lot-size support. right now we assume all active exchanges share
// the same lot granularity (target_lots uses the common unit). works for our
// current universe; becomes wrong if you add e.g. Deribit USD contracts.
struct RoutingContext {
    ExchangeState* states;
    uint32_t       active_exchanges;
    OrderDir       dir;
    qty_t          target_lots;
    double         limit_price;    // 0 = no limit
    uint64_t       decision_ns;
    double         short_vol_factor;
    double         book_imbalance;
};

struct ChildOrderBuffer {
    std::array<ChildOrder, kMaxChildOrders> orders{};
    uint32_t count{0};

    void reset() noexcept { count = 0; }

    [[nodiscard]] ChildOrder*       begin()       noexcept { return orders.data(); }
    [[nodiscard]] ChildOrder*       end()         noexcept { return orders.data() + count; }
    [[nodiscard]] const ChildOrder* begin() const noexcept { return orders.data(); }
    [[nodiscard]] const ChildOrder* end()   const noexcept { return orders.data() + count; }

    [[nodiscard]] bool full() const noexcept { return count >= kMaxChildOrders; }
};

} // namespace sor
