#pragma once

// pre-trade risk. inline reads against in-memory tables.
// must stay under ~200ns, it runs on every order before touching the SOR.

#include "oms_types.hpp"
#include "position_tracker.hpp"

namespace oms {

struct RiskLimits {
    qty_t   max_order_lots[kMaxInstruments]{};
    qty_t   max_net_lots[kMaxInstruments]{};
    double  max_notional_usd{0.0};
    double  min_margin_required{0.0};
};

enum class RiskRejectReason : uint8_t {
    OK             = 0,
    FAT_FINGER     = 1,
    POSITION_LIMIT = 2,
    MARGIN         = 3,
    NOTIONAL       = 4,
    DISABLED       = 5,
};

class PreTradeRiskEngine {
public:
    explicit PreTradeRiskEngine(const RiskLimits&      limits,
                                const InstrumentTable& instruments,
                                const PositionTable&   pos) noexcept
        : limits_(limits), instruments_(instruments), pos_(pos)
    {}

    [[nodiscard]] __attribute__((always_inline))
    RiskRejectReason validate(const StrategyOrderSignal& sig) const noexcept {
        if (__builtin_expect(sig.instr_id >= kMaxInstruments, 0))
            return RiskRejectReason::DISABLED;
        if (check_fat_finger(sig) != RiskRejectReason::OK) [[unlikely]]
            return RiskRejectReason::FAT_FINGER;
        if (check_position(sig)   != RiskRejectReason::OK) [[unlikely]]
            return RiskRejectReason::POSITION_LIMIT;

        // check_margin() returns NOTIONAL or MARGIN depending on which limit
        // tripped, propagate it as-is instead of collapsing both to MARGIN
        const RiskRejectReason margin_result = check_margin(sig);
        if (margin_result != RiskRejectReason::OK) [[unlikely]]
            return margin_result;
        return RiskRejectReason::OK;
    }

    void update_limits(const RiskLimits& l) noexcept { limits_ = l; }
    void update_instruments(const InstrumentTable& t) noexcept { instruments_ = t; }

    // returns the qty_lots that's actually safe to route for this signal:
    // unchanged if reduce-only doesn't apply, clamped down to exactly what
    // closes the position if it does and the direction is right, 0 if the
    // direction is wrong or there's nothing to reduce (caller should treat 0
    // as RejectReason::REDUCE_ONLY_VIOLATION, not "route zero lots").
    //
    // reduce-only is in effect if sig.reduce_only is set OR
    // InstrumentPosition::unwind_only is set for this instrument, whichever
    // the signal says is never the only thing that can turn this on, an
    // ops-forced unwind can't be skipped by a strategy that forgot the flag.
    [[nodiscard]] __attribute__((always_inline))
    qty_t clamp_reduce_only(const StrategyOrderSignal& sig) const noexcept {
        if (sig.instr_id >= kMaxInstruments) [[unlikely]] return sig.qty_lots;
        return clamp_reduce_only_impl(sig.instr_id, sig.dir, sig.qty_lots, sig.reduce_only);
    }

    // same clamp, called from reroute_leaves against a parent's CURRENT
    // leaves_qty_lots. the position may have moved since the original clamp
    // if another parent on the same instrument filled in between, always
    // re-reads net_qty_lots live rather than trusting whatever was true when
    // the parent was first created. pass parent.reduce_only for
    // reduce_only_requested: once a parent is constrained, for any reason, it
    // stays constrained for its whole lifecycle even if the live kill switch
    // is toggled off in the meantime, see the README's design decisions.
    [[nodiscard]] __attribute__((always_inline))
    qty_t clamp_reduce_only(instr_id_t instr_id, sor::OrderDir dir,
                            qty_t qty_lots, bool reduce_only_requested) const noexcept {
        if (instr_id >= kMaxInstruments) [[unlikely]] return qty_lots;
        return clamp_reduce_only_impl(instr_id, dir, qty_lots, reduce_only_requested);
    }

private:
    [[nodiscard]] __attribute__((always_inline))
    RiskRejectReason check_fat_finger(const StrategyOrderSignal& sig) const noexcept {
        const qty_t max = limits_.max_order_lots[sig.instr_id];
        return (max > 0 && sig.qty_lots > max)
            ? RiskRejectReason::FAT_FINGER
            : RiskRejectReason::OK;
    }

    [[nodiscard]] __attribute__((always_inline))
    RiskRejectReason check_position(const StrategyOrderSignal& sig) const noexcept {
        const qty_t max = limits_.max_net_lots[sig.instr_id];
        if (max == 0) return RiskRejectReason::OK;

        const int64_t net = pos_.instruments[sig.instr_id].net_qty_lots
                                .load(std::memory_order_acquire);
        const int64_t proposed = (sig.dir == sor::OrderDir::BUY)
            ? net + static_cast<int64_t>(sig.qty_lots)
            : net - static_cast<int64_t>(sig.qty_lots);
        const int64_t abs_proposed = (proposed < 0) ? -proposed : proposed;

        return (abs_proposed > static_cast<int64_t>(max))
            ? RiskRejectReason::POSITION_LIMIT
            : RiskRejectReason::OK;
    }

    [[nodiscard]] __attribute__((always_inline))
    RiskRejectReason check_margin(const StrategyOrderSignal& sig) const noexcept {
        if (limits_.max_notional_usd > 0.0 && sig.limit_price_usd > 0.0) {
            const double lot_size = instruments_.lot_size(sig.instr_id);
            const double notional = static_cast<double>(sig.qty_lots) * lot_size * sig.limit_price_usd;
            if (notional > limits_.max_notional_usd)
                return RiskRejectReason::NOTIONAL;
        }
        return (pos_.margin.available() < limits_.min_margin_required)
            ? RiskRejectReason::MARGIN
            : RiskRejectReason::OK;
    }

    [[nodiscard]] __attribute__((always_inline))
    qty_t clamp_reduce_only_impl(instr_id_t instr_id, sor::OrderDir dir,
                                 qty_t qty_lots, bool reduce_only_requested) const noexcept {
        const bool forced = pos_.instruments[instr_id].unwind_only.load(std::memory_order_acquire);
        if (!reduce_only_requested && !forced) return qty_lots;

        const int64_t net = pos_.instruments[instr_id].net_qty_lots
                                .load(std::memory_order_acquire);
        // BUY reduces a short (net < 0), SELL reduces a long (net > 0).
        // wrong direction or already flat: this order can't legitimately
        // reduce anything.
        const bool reduces_something =
            (dir == sor::OrderDir::BUY  && net < 0) ||
            (dir == sor::OrderDir::SELL && net > 0);
        if (!reduces_something) return 0;

        // headroom has to account for exposure OTHER outstanding orders on
        // this instrument already committed (open_buy_lots/open_sell_lots),
        // not just the confirmed position. two reduce-only orders submitted
        // before either fills would otherwise both clamp against the SAME
        // net_qty_lots independently and could together overshoot flat and
        // flip the position, exactly what reduce-only exists to prevent.
        const int64_t already_committed = (dir == sor::OrderDir::BUY)
            ? pos_.instruments[instr_id].open_buy_lots.load(std::memory_order_acquire)
            : pos_.instruments[instr_id].open_sell_lots.load(std::memory_order_acquire);

        const int64_t abs_net  = (net < 0) ? -net : net;
        const int64_t headroom = abs_net - already_committed;
        if (headroom <= 0) return 0;

        return (qty_lots < headroom) ? qty_lots : static_cast<qty_t>(headroom);
    }

    RiskLimits              limits_;
    InstrumentTable         instruments_;
    const PositionTable&    pos_;
};

} // namespace oms
