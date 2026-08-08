#pragma once

// standalone TWAP, lives outside ExecutionCore on purpose. it just emits
// StrategyOrderSignal on a schedule, same shape any strategy would produce
// by hand, so ExecutionCore doesn't need to know this thing exists.
//
// jitter on size and interval isn't optional here. fixed clip, fixed
// interval is a signature, not an execution algo.

#include "oms/oms_types.hpp"
#include <algorithm>
#include <cstdint>
#include <random>

namespace algos {

using oms::instr_id_t;
using oms::qty_t;
using oms::StrategyOrderSignal;

struct TwapConfig {
    instr_id_t     instr_id;
    sor::OrderDir  dir;
    uint8_t        strategy_id;
    qty_t          total_qty_lots;
    uint64_t       duration_ns;
    uint32_t       num_slices;
    double         limit_price_usd{0.0};
    bool           reduce_only{false};
    sor::OrderType order_type{sor::OrderType::TAKER};

    // symmetric around 1.0, so avg slice size / avg interval still converge
    // to plain TWAP even though any single slice is off. 0 = no jitter.
    double   size_jitter_frac{0.30};
    double   interval_jitter_frac{0.30};

    uint64_t rng_seed{0};   // 0 = std::random_device
};

// not thread-safe. drive it from whatever thread owns the strategy layer,
// call tick() on your own cadence and push the signal onto strategy_queue
// when it fires.
// caps requested slice count to what's actually sliceable: can't hand out
// more slices than lots, each one needs at least 1. total_lots <= 0 returns
// 1, doesn't matter, done() short-circuits on remaining_lots_ <= 0 anyway.
[[nodiscard]] inline uint32_t clamp_slice_count(uint32_t requested, qty_t total_lots) noexcept {
    if (requested == 0 || total_lots <= 0) return 1;
    return static_cast<uint32_t>(std::min<qty_t>(requested, total_lots));
}

class TwapScheduler {
public:
    explicit TwapScheduler(const TwapConfig& cfg) noexcept
        : cfg_(cfg)
        , rng_(cfg.rng_seed != 0 ? cfg.rng_seed : std::random_device{}())
        , remaining_lots_(cfg.total_qty_lots)
        , total_slices_(clamp_slice_count(cfg.num_slices, cfg.total_qty_lots))
        , slices_remaining_(total_slices_)
    {}

    // first slice fires immediately at start_ns, no dead gap before the
    // first clip goes out.
    void start(uint64_t start_ns) noexcept { next_deadline_ns_ = start_ns; }

    [[nodiscard]] bool tick(uint64_t now_ns, StrategyOrderSignal& out) noexcept {
        if (done() || now_ns < next_deadline_ns_) return false;
        out = make_slice_signal();
        schedule_next(now_ns);
        return true;
    }

    [[nodiscard]] bool done() const noexcept {
        return remaining_lots_ <= 0 || slices_remaining_ == 0;
    }

    [[nodiscard]] uint64_t next_deadline_ns() const noexcept { return next_deadline_ns_; }
    [[nodiscard]] qty_t    remaining_lots()   const noexcept { return remaining_lots_; }

private:
    [[nodiscard]] StrategyOrderSignal make_slice_signal() noexcept {
        qty_t slice_lots;

        if (slices_remaining_ == 1) {
            // tail slice, no jitter, eats whatever rounding drift the earlier
            // draws left behind. this is what keeps sum(slices) == total exact.
            slice_lots = remaining_lots_;
        } else {
            const double nominal = static_cast<double>(cfg_.total_qty_lots)
                                  / static_cast<double>(total_slices_);
            slice_lots = static_cast<qty_t>(std::llround(nominal * jittered(cfg_.size_jitter_frac)));

            // min_reserve leaves 1 lot per remaining slice, otherwise a fat
            // draw here can starve the ones after it.
            const qty_t min_reserve = static_cast<qty_t>(slices_remaining_ - 1);
            slice_lots = std::clamp<qty_t>(slice_lots, 1, remaining_lots_ - min_reserve);
        }

        remaining_lots_ -= slice_lots;
        --slices_remaining_;

        StrategyOrderSignal sig{};
        sig.instr_id        = cfg_.instr_id;
        sig.dir              = cfg_.dir;
        sig.strategy_id      = cfg_.strategy_id;
        sig.qty_lots         = slice_lots;
        sig.limit_price_usd  = cfg_.limit_price_usd;
        sig.reduce_only      = cfg_.reduce_only;
        sig.order_type       = cfg_.order_type;
        sig.signal_ns        = 0;   // ExecutionCore stamps the real send time
        sig.short_vol_factor = 0.0;
        sig.book_imbalance   = 0.0;
        return sig;
    }

    void schedule_next(uint64_t now_ns) noexcept {
        if (done()) { next_deadline_ns_ = UINT64_MAX; return; }

        const double nominal_interval_ns =
            static_cast<double>(cfg_.duration_ns) / static_cast<double>(total_slices_);
        const auto interval_ns =
            static_cast<uint64_t>(nominal_interval_ns * jittered(cfg_.interval_jitter_frac));

        next_deadline_ns_ = now_ns + interval_ns;
    }

    [[nodiscard]] double jittered(double frac) noexcept {
        if (frac <= 0.0) return 1.0;
        std::uniform_real_distribution<double> dist(1.0 - frac, 1.0 + frac);
        return dist(rng_);
    }

    TwapConfig      cfg_;
    std::mt19937_64 rng_;

    qty_t    remaining_lots_;
    uint32_t total_slices_;
    uint32_t slices_remaining_;
    uint64_t next_deadline_ns_{0};

    // TODO: interval jitter can occasionally push the last real slice past
    // duration_ns if every draw lands on the high side. fine for now, nobody's
    // holding us to the millisecond on a TWAP, but flag it if that changes.
};

} // namespace algos
