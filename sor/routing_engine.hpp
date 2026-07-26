#pragma once

// two phases:
//   1. K-way merge, merges K sorted book sides into one sorted CostSlice array. O(N).
//   2. Greedy fill, sweeps slices, allocates lots. pure integer. O(N).
//
// K=5 exchanges. "find minimum across 5 cursors" is a linear scan, not a heap.
// yes I know about priority_queue. at K=5 the linear scan is faster.

#include "types.hpp"
#include "exchange_state.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace sor {

struct SplitWorkspace {
    std::array<CostSlice, kMaxChildOrders> slices{};
    uint32_t slice_count{0};
    void reset() noexcept { slice_count = 0; }
};

class RoutingEngine {
public:
    explicit RoutingEngine(const FeeMatrix& fees) noexcept;

    void update_fees(const FeeMatrix& fees) noexcept;

    SplitResult calculate_optimal_split(
        const RoutingContext& ctx,
        ChildOrderBuffer&     out
    ) noexcept;

    // posts one passive order at the touch on whichever active exchange
    // gives the best maker-fee-adjusted price, instead of walking the book
    // like calculate_optimal_split does. BUY posts on the BID side, SELL on
    // the ASK side, the opposite of which side calculate_optimal_split reads,
    // a maker order adds liquidity to your own side, it doesn't cross into
    // the other one. out.count is 0 or 1: this never splits across venues,
    // posting the same size on multiple books at once risks a double fill
    // with no coordination between them, out of scope for v1. Result carries
    // no fill yet, filled_qty is always 0 here, it's a placement decision,
    // not a fill, the caller's downstream execution-report handling is what
    // eventually fills it.
    SplitResult calculate_maker_placement(
        const RoutingContext& ctx,
        ChildOrderBuffer&     out
    ) noexcept;

    // ctx.reference_lot_size, or states[0]'s native lot_size if unset. public
    // because ExecutionCore needs the exact same resolution when it converts
    // a real-world qty gap (e.g. an undispatched child) back into the
    // canonical lots that ParentOrder::leaves_qty_lots is expressed in.
    static double resolve_reference_lot_size(const RoutingContext& ctx) noexcept;

private:
    // one cursor per exchange. all the per-exchange scalars are precomputed here
    // so the inner merge loop only does: dir_sign * price_ticks + penalty_ticks.
    //
    // penalty_ticks is the combined fee + dynamic latency cost expressed as an
    // integer tick offset from the best level. approximation error is ~0.01 ticks
    // at depth 20. not worth caring about.
    //
    //   BUY  (dir_sign=+1):  eff = +price_ticks + penalty_ticks
    //   SELL (dir_sign=-1):  eff = -price_ticks + penalty_ticks
    //
    // min(eff) across cursors = best fill, for both directions.
    struct Cursor {
        const ExchangeState* state;
        const LevelSide*     levels;
        double               tick_size;
        double               lot_size;
        double               lot_ratio;     // native lot_size / ctx.reference_lot_size
        double               fill_rate;
        int64_t              penalty_ticks;
        int64_t              dir_sign;
        price_t              limit_ticks;   // 0 = no limit; per-exchange (tick size varies)
        qty_t                min_lots;
        qty_t                max_lots;
        uint32_t             level_idx;
        bool                 exhausted;
        uint8_t              _pad[3];
    };

    // advance to the next level that clears limit_ticks + min_lots checks.
    // for sorted sides, a limit violation means all deeper levels also fail → exhaust.
    static void seek_next_valid(Cursor& c) noexcept;

    static double estimate_fill_rate(
        double rtt_us, double vol_factor, double directional_imb) noexcept;

    void build_cost_slices(const RoutingContext& ctx, SplitWorkspace& ws) noexcept;

    SplitResult greedy_fill(
        SplitWorkspace& ws, const RoutingContext& ctx, ChildOrderBuffer& out) noexcept;

    FeeMatrix      fees_;
    SplitWorkspace workspace_;
};

} // namespace sor
