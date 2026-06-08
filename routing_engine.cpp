#include "routing_engine.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace sor {

// fill rate model. rough exponential decay with vol + imbalance penalties.
// numbers are eyeballed against our own fill data; recalibrate if you have better.
// TODO: kLatDecay should probably be per-exchange — deribit and binance are not
// the same beast at 800us. leaving it flat for now.
static constexpr double kLatDecay       = 2e-4;
static constexpr double kVolFillPenalty = 0.10;
static constexpr double kImbFillPenalty = 0.08;
static constexpr double kMinFillRate    = 0.50;   // floor at 50%, not zero

RoutingEngine::RoutingEngine(const FeeMatrix& fees) noexcept
    : fees_(fees), workspace_{}
{}

void RoutingEngine::update_fees(const FeeMatrix& fees) noexcept {
    fees_ = fees;
}

double RoutingEngine::estimate_fill_rate(
    double rtt_us, double vol_factor, double directional_imb
) noexcept {
    const double lat     = std::exp(-kLatDecay * rtt_us);
    const double vol     = 1.0 - kVolFillPenalty * vol_factor;
    const double imb_pos = (directional_imb > 0.0) ? directional_imb : 0.0;
    const double imb     = 1.0 - kImbFillPenalty * imb_pos;
    const double rate    = lat * vol * imb;
    return (rate > kMinFillRate) ? rate : kMinFillRate;
}

// fully integer: limit check against c.limit_ticks, qty check against c.min_lots.
// terminates the cursor early on limit violation because levels are sorted —
// if pt > limit (BUY) then pt+1, pt+2... are also > limit. not rocket science.
void RoutingEngine::seek_next_valid(Cursor& c) noexcept {
    while (c.level_idx < c.levels->count) {
        const price_t pt = c.levels->price_ticks[c.level_idx];

        if (c.limit_ticks != 0) {
            if (c.dir_sign > 0 && pt > c.limit_ticks) [[unlikely]] { c.exhausted = true; return; }
            if (c.dir_sign < 0 && pt < c.limit_ticks) [[unlikely]] { c.exhausted = true; return; }
        }

        if (c.levels->qty_lots[c.level_idx] >= c.min_lots) [[likely]] return;

        ++c.level_idx;
    }
    c.exhausted = true;
}

// Phase 1.
//
// each exchange's book side is already sorted. we're merging K=5 sorted sequences,
// not sorting an unsorted array. std::sort here would be embarrassing.
//
// inner loop: linear scan of ≤5 cursors to find min effective_ticks.
// two integer ops per cursor. the FPU is asleep until we emit a slice.
//
// penalty_ticks approximation: we use best-level price as the reference.
// the actual fee+latency penalty grows slightly with price depth (by adj ticks
// per level), but at adj≈0.0005 and depth=20 the drift is ~0.01 ticks.
// close enough that it has never mattered in practice.
void RoutingEngine::build_cost_slices(
    const RoutingContext& ctx,
    SplitWorkspace&       ws
) noexcept {
    ws.reset();

    const uint8_t side_idx = (ctx.dir == OrderDir::BUY)
        ? static_cast<uint8_t>(Side::ASK)
        : static_cast<uint8_t>(Side::BID);

    const double directional_imb = (ctx.dir == OrderDir::BUY)
        ? ((ctx.book_imbalance > 0.0) ? ctx.book_imbalance : 0.0)
        : ((ctx.book_imbalance < 0.0) ? -ctx.book_imbalance : 0.0);

    std::array<Cursor, kMaxExchanges> cursors;
    uint32_t ncursors = 0;

    for (uint32_t ex = 0; ex < ctx.active_exchanges; ++ex) {
        const ExchangeState& state = ctx.states[ex];
        if (!state.enabled || !state.book.is_valid) [[unlikely]] continue;

        Cursor& c   = cursors[ncursors];
        c.state     = &state;
        c.levels    = &state.book.sides[side_idx];
        c.tick_size = state.book.tick_size;
        c.lot_size  = state.book.lot_size;

        // FPU work for this cursor — all happens once, here, before the merge loop.
        c.fill_rate = estimate_fill_rate(
            state.latency.get_rtt_us(), ctx.short_vol_factor, directional_imb);

        const double combined_adj = fees_.taker(state.exchange_id)
            + state.latency.dynamic_cost_adj(ctx.short_vol_factor, directional_imb);
        const int64_t ref_ticks   = (c.levels->count > 0) ? c.levels->price_ticks[0] : 0LL;
        c.penalty_ticks = static_cast<int64_t>(
            std::round(static_cast<double>(ref_ticks) * combined_adj));

        c.dir_sign = (ctx.dir == OrderDir::BUY) ? int64_t(1) : int64_t(-1);

        c.limit_ticks = (ctx.limit_price > 0.0)
            ? static_cast<price_t>(std::round(ctx.limit_price / c.tick_size))
            : price_t(0);

        c.min_lots = static_cast<qty_t>(std::round(state.lot.min_qty / c.lot_size));
        c.max_lots = static_cast<qty_t>(state.lot.max_qty / c.lot_size);

        c.level_idx = 0;
        c.exhausted = (c.levels->count == 0);

        if (!c.exhausted) seek_next_valid(c);
        if (!c.exhausted) ++ncursors;
    }

    if (__builtin_expect(ncursors == 0, 0)) return;

    // merge loop. at each step: scan cursors, take the one with the smallest
    // effective_ticks, emit a CostSlice, advance that cursor.
    while (ws.slice_count < kMaxChildOrders) {

        uint32_t best_k   = kMaxExchanges;
        int64_t  best_eff = std::numeric_limits<int64_t>::max();

        for (uint32_t k = 0; k < ncursors; ++k) {
            if (cursors[k].exhausted) [[unlikely]] continue;
            const int64_t eff =
                cursors[k].dir_sign * cursors[k].levels->price_ticks[cursors[k].level_idx]
                + cursors[k].penalty_ticks;
            if (eff < best_eff) { best_eff = eff; best_k = k; }
        }

        if (best_k == kMaxExchanges) [[unlikely]] break;

        Cursor& c = cursors[best_k];

        const price_t pt      = c.levels->price_ticks[c.level_idx];
        const qty_t   ql      = c.levels->qty_lots[c.level_idx];
        const uint8_t lvl_idx = static_cast<uint8_t>(c.level_idx);

        const qty_t clamped  = (ql < c.max_lots) ? ql : c.max_lots;
        const qty_t adj_lots = static_cast<qty_t>(static_cast<double>(clamped) * c.fill_rate);

        ++c.level_idx;
        seek_next_valid(c);

        if (adj_lots == 0) [[unlikely]] continue;

        CostSlice& s      = ws.slices[ws.slice_count++];
        s.effective_ticks  = best_eff;
        s.available_lots   = adj_lots;
        s.raw_price_ticks  = pt;
        s.exchange_id      = c.state->exchange_id;
        s.level_idx        = lvl_idx;
    }
}

// Phase 2. greedy sweep over the sorted slices.
//
// remaining_lots is qty_t the whole way through. fill decision is a single
// integer min. remaining_lots == 0 is an exact test — no epsilon, no rounding
// drama. that's the whole point of going integer in the first place.
//
// FPU shows up only when writing ChildOrder / SplitResult fields, i.e. after
// the fill decision has already been made.
SplitResult RoutingEngine::greedy_fill(
    SplitWorkspace&       ws,
    const RoutingContext& ctx,
    ChildOrderBuffer&     out
) noexcept {
    SplitResult result{};
    result.success     = false;
    result.child_count = 0;
    result.filled_qty  = 0.0;

    if (ws.slice_count == 0) [[unlikely]] {
        result.unfilled_qty = from_lots(ctx.target_lots, ctx.states[0].lot.lot_size);
        return result;
    }

    qty_t   remaining_lots  = ctx.target_lots;
    price_t best_raw_ticks  = 0;
    price_t worst_raw_ticks = 0;
    double  adverse         = 0.0;

    for (uint32_t i = 0; i < ws.slice_count; ++i) {
        if (remaining_lots == 0) break;

        const CostSlice&     slice = ws.slices[i];
        const ExchangeState& state = ctx.states[slice.exchange_id];

        const qty_t fill_lots = (remaining_lots < slice.available_lots)
                                ? remaining_lots
                                : slice.available_lots;

        // TODO: move min_lots into CostSlice to avoid this division per fill.
        // fine for now — it's not the comparison inner loop.
        const qty_t min_lots = static_cast<qty_t>(
            std::round(state.lot.min_qty / state.lot.lot_size));
        if (fill_lots < min_lots) [[unlikely]] continue;

        // from here on: double conversions for reporting only
        const double tick_sz   = state.book.tick_size;
        const double lot_sz    = state.book.lot_size;
        const double fill_qty  = static_cast<double>(fill_lots) * lot_sz;
        const double raw_price = static_cast<double>(slice.raw_price_ticks) * tick_sz;
        const double eff_price = static_cast<double>(slice.effective_ticks)  * tick_sz;
        const double eff_cost  = fill_qty * eff_price;

        assert(!out.full());
        ChildOrder& child    = out.orders[out.count++];
        child.price          = raw_price;
        child.qty            = fill_qty;
        child.effective_cost = eff_cost;
        child.exchange_id    = slice.exchange_id;
        child.level_idx      = slice.level_idx;
        child.type           = OrderType::TAKER;
        child.dir            = ctx.dir;

        result.total_effective_cost += eff_cost;
        result.filled_qty           += fill_qty;
        remaining_lots              -= fill_lots;
        result.child_count++;

        if (result.child_count == 1) best_raw_ticks = slice.raw_price_ticks;
        worst_raw_ticks = slice.raw_price_ticks;

        adverse += fill_qty * raw_price * state.latency.latency_cost_adj();
    }

    result.success      = (remaining_lots == 0);
    result.unfilled_qty = from_lots(remaining_lots, ctx.states[0].lot.lot_size);

    if (result.filled_qty > 1e-9) {
        result.avg_effective_price   = result.total_effective_cost / result.filled_qty;
        result.adverse_selection_est = adverse;
        result.spread_capture_bps    = 0.0;

        if (best_raw_ticks > 0) {
            const int64_t tick_diff = (ctx.dir == OrderDir::BUY)
                ? (worst_raw_ticks - best_raw_ticks)
                : (best_raw_ticks  - worst_raw_ticks);
            result.market_impact_bps =
                static_cast<double>(tick_diff) / static_cast<double>(best_raw_ticks) * 10000.0;
        }
    }

    return result;
}

SplitResult RoutingEngine::calculate_optimal_split(
    const RoutingContext& ctx,
    ChildOrderBuffer&     out
) noexcept {
    assert(ctx.states           != nullptr);
    assert(ctx.target_lots      >  0);
    assert(ctx.active_exchanges <= kMaxExchanges);
    assert(out.count            == 0);

    build_cost_slices(ctx, workspace_);
    return greedy_fill(workspace_, ctx, out);
}

} // namespace sor
