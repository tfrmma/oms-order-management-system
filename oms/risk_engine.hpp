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

    RiskLimits              limits_;
    InstrumentTable         instruments_;
    const PositionTable&    pos_;
};

} // namespace oms
