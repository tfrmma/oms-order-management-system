// unit tests for the pieces that don't need a live ExecutionCore: the SOR
// merge/fill algorithm, the order pool, position tracking, pre-trade risk,
// the SPSC queue, and the timer wheel.
//
// tick_size = lot_size = 1.0 everywhere here on purpose: price_ticks == price
// and qty_lots == qty, so expected values are exact integers instead of
// floating garbage. exchange_state_populated below spells out what each
// field means for anyone who wants a book with realistic tick/lot sizes.

#include "catch_amalgamated.hpp"

#include "sor/routing_engine.hpp"
#include "order_pool.hpp"
#include "position_tracker.hpp"
#include "risk_engine.hpp"
#include "spsc_queue.hpp"
#include "timer_wheel.hpp"
#include "execution_core.hpp"

#include <vector>

using namespace sor;
using namespace oms;

namespace {

// fills one side of the book, best-first, from {price, qty} pairs.
void set_levels(LevelSide& side, std::initializer_list<std::pair<int64_t, int64_t>> levels) {
    uint32_t i = 0;
    for (const auto& [px, qty] : levels) {
        side.price_ticks[i] = px;
        side.qty_lots[i]    = qty;
        ++i;
    }
    side.count = i;
    side.recompute_cumulative();
}

// tick_size = lot_size = 1, zero fees, zero latency, no min/max clamp.
// fill_rate collapses to exactly 1.0 (see estimate_fill_rate), so adj_lots
// always equals the raw level quantity, no rounding to fight with.
void init_exchange(ExchangeState& ex, uint8_t exchange_id) {
    ex.exchange_id       = exchange_id;
    ex.enabled           = true;
    ex.book.exchange_id  = exchange_id;
    ex.book.is_valid     = true;
    ex.book.tick_size    = 1.0;
    ex.book.lot_size     = 1.0;
    ex.book.last_sequence = 1;
    ex.lot = { 1.0, 1.0, 0.0, 1e9, 0.0 };  // lot_size, tick_size, min_qty, max_qty, max_notional
    ex.latency.ewma_rtt_us.store(0.0, std::memory_order_relaxed);
}

RoutingContext make_context(ExchangeState* states, uint32_t n_exchanges,
                            OrderDir dir, qty_t target_lots, double limit_price = 0.0) {
    RoutingContext ctx{};
    ctx.states           = states;
    ctx.active_exchanges = n_exchanges;
    ctx.dir               = dir;
    ctx.target_lots       = target_lots;
    ctx.limit_price       = limit_price;
    return ctx;
}

} // namespace

TEST_CASE("RoutingEngine: exact fill against a single level") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.asks(), {{100, 50}});

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_optimal_split(
        make_context(&ex, 1, OrderDir::BUY, 50), out);

    REQUIRE(r.success);
    REQUIRE(r.child_count == 1);
    REQUIRE(r.filled_qty  == Catch::Approx(50.0));
    REQUIRE(r.unfilled_qty == Catch::Approx(0.0));
    REQUIRE(out.orders[0].price == Catch::Approx(100.0));
    REQUIRE(out.orders[0].qty   == Catch::Approx(50.0));
}

TEST_CASE("RoutingEngine: sweeps multiple levels in ascending price order") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.asks(), {{100, 10}, {101, 10}, {102, 10}});

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_optimal_split(
        make_context(&ex, 1, OrderDir::BUY, 25), out);

    REQUIRE(r.success);
    REQUIRE(r.child_count == 3);
    REQUIRE(r.filled_qty  == Catch::Approx(25.0));

    REQUIRE(out.orders[0].price == Catch::Approx(100.0));
    REQUIRE(out.orders[0].qty   == Catch::Approx(10.0));
    REQUIRE(out.orders[1].price == Catch::Approx(101.0));
    REQUIRE(out.orders[1].qty   == Catch::Approx(10.0));
    REQUIRE(out.orders[2].price == Catch::Approx(102.0));
    REQUIRE(out.orders[2].qty   == Catch::Approx(5.0));

    // avg = (10*100 + 10*101 + 5*102) / 25 = 100.8
    REQUIRE(r.avg_effective_price == Catch::Approx(100.8));
    // worst - best = 102 - 100 = 2 ticks over a 100-tick base = 200 bps
    REQUIRE(r.market_impact_bps == Catch::Approx(200.0));
}

TEST_CASE("RoutingEngine: picks the better price across exchanges regardless of exchange index") {
    ExchangeState ex[2]{};
    init_exchange(ex[0], 0);
    init_exchange(ex[1], 1);
    set_levels(ex[0].book.asks(), {{101, 100}});  // worse price, deeper book
    set_levels(ex[1].book.asks(), {{100, 20}});   // better price, thinner book

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_optimal_split(
        make_context(ex, 2, OrderDir::BUY, 30), out);

    REQUIRE(r.success);
    REQUIRE(r.child_count == 2);

    // cheaper exchange 1 fills first even though it's index 1, not 0
    REQUIRE(out.orders[0].exchange_id == 1);
    REQUIRE(out.orders[0].price       == Catch::Approx(100.0));
    REQUIRE(out.orders[0].qty         == Catch::Approx(20.0));

    REQUIRE(out.orders[1].exchange_id == 0);
    REQUIRE(out.orders[1].price       == Catch::Approx(101.0));
    REQUIRE(out.orders[1].qty         == Catch::Approx(10.0));

    // (20*100 + 10*101) / 30 = 100.333...
    REQUIRE(r.avg_effective_price == Catch::Approx(100.3333).margin(0.001));
}

TEST_CASE("RoutingEngine: reports partial fill when the book can't cover the order") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.asks(), {{100, 10}});

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_optimal_split(
        make_context(&ex, 1, OrderDir::BUY, 50), out);

    REQUIRE_FALSE(r.success);
    REQUIRE(r.child_count   == 1);
    REQUIRE(r.filled_qty    == Catch::Approx(10.0));
    REQUIRE(r.unfilled_qty  == Catch::Approx(40.0));
}

TEST_CASE("RoutingEngine: limit price clips levels beyond it instead of crossing them") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.asks(), {{100, 10}, {105, 10}, {110, 10}});

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    // limit=104: level 0 (100) qualifies, levels at 105/110 don't
    const SplitResult r = engine.calculate_optimal_split(
        make_context(&ex, 1, OrderDir::BUY, 25, /*limit_price=*/104.0), out);

    REQUIRE_FALSE(r.success);
    REQUIRE(r.child_count  == 1);
    REQUIRE(r.filled_qty   == Catch::Approx(10.0));
    REQUIRE(r.unfilled_qty == Catch::Approx(15.0));
}

TEST_CASE("RoutingEngine: SELL fills against bids highest-price-first") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.bids(), {{100, 10}, {99, 10}});

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_optimal_split(
        make_context(&ex, 1, OrderDir::SELL, 15), out);

    REQUIRE(r.success);
    REQUIRE(r.child_count == 2);
    REQUIRE(out.orders[0].price == Catch::Approx(100.0));
    REQUIRE(out.orders[0].qty   == Catch::Approx(10.0));
    REQUIRE(out.orders[1].price == Catch::Approx(99.0));
    REQUIRE(out.orders[1].qty   == Catch::Approx(5.0));
    REQUIRE(out.orders[0].dir   == OrderDir::SELL);
}

TEST_CASE("RoutingEngine: reference_lot_size reconciles exchanges with different native lot sizes") {
    // exchange 0 quotes in 1.0-unit lots, exchange 1 quotes the same instrument
    // in 2.0-unit lots (e.g. an inverse contract). without reference_lot_size,
    // "10 lots" on exchange 1 would get treated as 10 real units instead of the
    // 20 it actually represents, corrupting the remaining_lots bookkeeping.
    ExchangeState ex[2]{};
    init_exchange(ex[0], 0);
    init_exchange(ex[1], 1);
    ex[1].book.lot_size = 2.0;
    ex[1].lot.lot_size  = 2.0;
    set_levels(ex[0].book.asks(), {{100, 10}});  // 10 real units
    set_levels(ex[1].book.asks(), {{100, 10}});  // 10 native lots = 20 real units, same price

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    RoutingContext ctx = make_context(ex, 2, OrderDir::BUY, 25);
    ctx.reference_lot_size = 1.0;  // canonical unit: 1 real unit

    const SplitResult r = engine.calculate_optimal_split(ctx, out);

    REQUIRE(r.success);
    REQUIRE(r.child_count == 2);
    // exchange 0 exhausts its 10 real units first (tied price, scanned first)
    REQUIRE(out.orders[0].exchange_id == 0);
    REQUIRE(out.orders[0].qty         == Catch::Approx(10.0));
    // exchange 1 covers the remaining 15 real units out of its 20 available,
    // not "15 native lots" (which would be 30 real units) and not capped at
    // its raw qty_lots figure of 10 (which would under-fill and misreport)
    REQUIRE(out.orders[1].exchange_id == 1);
    REQUIRE(out.orders[1].qty         == Catch::Approx(15.0));

    // filled_qty must equal target exactly, never exceed it
    REQUIRE(r.filled_qty   == Catch::Approx(25.0));
    REQUIRE(r.unfilled_qty == Catch::Approx(0.0));
}

TEST_CASE("OMSOrderPool: alloc/free is LIFO") {
    OMSOrderPool pool;
    REQUIRE(pool.parents_in_use() == 0);

    const auto p1 = pool.alloc_parent();
    const auto p2 = pool.alloc_parent();
    REQUIRE(p1.ok);
    REQUIRE(p2.ok);
    REQUIRE(pool.parents_in_use() == 2);

    pool.free_parent(p2.value);
    const auto p3 = pool.alloc_parent();
    REQUIRE(p3.ok);
    REQUIRE(p3.value == p2.value);  // LIFO free list hands the id straight back

    pool.free_parent(p1.value);
    pool.free_parent(p3.value);
    REQUIRE(pool.parents_in_use() == 0);
}

TEST_CASE("OMSOrderPool: exhausts cleanly and recovers after freeing everything") {
    OMSOrderPool pool;
    std::vector<order_id_t> ids;
    ids.reserve(kMaxParentOrders);

    for (uint32_t i = 0; i < kMaxParentOrders; ++i) {
        const auto r = pool.alloc_parent();
        REQUIRE(r.ok);
        ids.push_back(r.value);
    }
    REQUIRE(pool.parents_in_use() == kMaxParentOrders);

    const auto overflow = pool.alloc_parent();
    REQUIRE_FALSE(overflow.ok);  // pool exhaustion fails cleanly, no UB, no wraparound

    for (const auto id : ids) pool.free_parent(id);
    REQUIRE(pool.parents_in_use() == 0);
}

TEST_CASE("OMSOrderPool: alloc failures are counted, not just dropped") {
    OMSOrderPool pool;
    REQUIRE(pool.parent_alloc_failures() == 0);
    REQUIRE(pool.child_alloc_failures()  == 0);

    std::vector<order_id_t> ids;
    ids.reserve(kMaxParentOrders);
    for (uint32_t i = 0; i < kMaxParentOrders; ++i)
        ids.push_back(pool.alloc_parent().value);

    // three failed allocs on top of a full pool -> three counted failures,
    // regardless of how many succeeded before it filled up
    REQUIRE_FALSE(pool.alloc_parent().ok);
    REQUIRE_FALSE(pool.alloc_parent().ok);
    REQUIRE_FALSE(pool.alloc_parent().ok);
    REQUIRE(pool.parent_alloc_failures() == 3);
    REQUIRE(pool.child_alloc_failures()  == 0);  // independent counters, child pool untouched

    for (const auto id : ids) pool.free_parent(id);

    // freeing capacity doesn't reset the cumulative counter, it's "since
    // process start", not "currently exhausted"
    REQUIRE(pool.parent_alloc_failures() == 3);
    REQUIRE(pool.alloc_parent().ok);
    REQUIRE(pool.parent_alloc_failures() == 3);
}

TEST_CASE("InstrumentPosition: weighted average price on same-side adds") {
    InstrumentPosition pos;
    pos.apply_fill(OrderDir::BUY, 10, 100.0);
    REQUIRE(pos.net_qty_lots.load() == 10);
    REQUIRE(pos.avg_entry_price.load() == Catch::Approx(100.0));

    pos.apply_fill(OrderDir::BUY, 10, 110.0);
    REQUIRE(pos.net_qty_lots.load() == 20);
    // (100*10 + 110*10) / 20
    REQUIRE(pos.avg_entry_price.load() == Catch::Approx(105.0));
}

TEST_CASE("InstrumentPosition: side flip resets cost basis to the flipping fill's price") {
    InstrumentPosition pos;
    pos.apply_fill(OrderDir::BUY, 10, 100.0);   // net +10 @ 100
    pos.apply_fill(OrderDir::SELL, 15, 120.0);  // crosses through flat, ends net -5 @ 120

    REQUIRE(pos.net_qty_lots.load() == -5);
    REQUIRE(pos.avg_entry_price.load() == Catch::Approx(120.0));
}

TEST_CASE("PreTradeRiskEngine: rejects and passes for the right reasons") {
    PositionTable pos{};
    RiskLimits limits{};

    SECTION("fat finger") {
        limits.max_order_lots[0] = 100;
        PreTradeRiskEngine risk(limits, pos);

        StrategyOrderSignal sig{};
        sig.instr_id = 0;
        sig.qty_lots = 150;
        REQUIRE(risk.validate(sig) == RiskRejectReason::FAT_FINGER);
    }

    SECTION("position limit") {
        limits.max_net_lots[0] = 50;
        pos.instruments[0].net_qty_lots.store(40);
        PreTradeRiskEngine risk(limits, pos);

        StrategyOrderSignal sig{};
        sig.instr_id = 0;
        sig.dir      = OrderDir::BUY;
        sig.qty_lots = 20;  // 40 + 20 = 60 > 50
        REQUIRE(risk.validate(sig) == RiskRejectReason::POSITION_LIMIT);
    }

    SECTION("notional cap") {
        limits.max_notional_usd = 10'000.0;
        PreTradeRiskEngine risk(limits, pos);

        StrategyOrderSignal sig{};
        sig.instr_id        = 0;
        sig.qty_lots        = 1000;
        sig.limit_price_usd = 50'000.0;  // 1000 * 0.001 * 50000 = 50000 > 10000
        REQUIRE(risk.validate(sig) == RiskRejectReason::NOTIONAL);
    }

    SECTION("margin") {
        limits.min_margin_required = 1000.0;
        pos.margin.update(/*equity=*/500.0, /*used=*/0.0);
        PreTradeRiskEngine risk(limits, pos);

        StrategyOrderSignal sig{};
        sig.instr_id = 0;
        sig.qty_lots = 1;
        REQUIRE(risk.validate(sig) == RiskRejectReason::MARGIN);
    }

    SECTION("clean signal passes") {
        limits.max_order_lots[0]   = 100;
        limits.max_net_lots[0]     = 50;
        limits.max_notional_usd    = 1'000'000.0;
        limits.min_margin_required = 0.0;
        pos.margin.update(50'000.0, 0.0);
        PreTradeRiskEngine risk(limits, pos);

        StrategyOrderSignal sig{};
        sig.instr_id        = 0;
        sig.dir              = OrderDir::BUY;
        sig.qty_lots         = 10;
        sig.limit_price_usd  = 100.0;
        REQUIRE(risk.validate(sig) == RiskRejectReason::OK);
    }
}

TEST_CASE("SpscQueue: FIFO order and one-slot-reserved capacity") {
    SpscQueue<int, 4> q;  // usable capacity is N-1: the ring reserves a slot to tell full from empty
    REQUIRE(q.empty());

    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    REQUIRE(q.push(3));
    REQUIRE_FALSE(q.push(4));

    int v = 0;
    REQUIRE(q.pop(v));
    REQUIRE(v == 1);
    REQUIRE(q.pop(v));
    REQUIRE(v == 2);

    REQUIRE(q.push(4));  // room freed by the two pops above

    REQUIRE(q.pop(v)); REQUIRE(v == 3);
    REQUIRE(q.pop(v)); REQUIRE(v == 4);
    REQUIRE(q.empty());
    REQUIRE_FALSE(q.pop(v));
}

TEST_CASE("TimerWheel: fires once, exactly at the deadline, not before") {
    TimerWheel wheel(1'000'000ULL);  // 1ms tick, matches the production default
    int  fire_count = 0;
    child_id_t fired_id = kInvalidChildId;
    wheel.set_callback([&](child_id_t cid) { fired_id = cid; ++fire_count; });

    REQUIRE(wheel.insert(/*child_id=*/7, /*now_ns=*/0, /*timeout_ns=*/5'000'000ULL));

    wheel.tick(4'000'000ULL);
    REQUIRE(fire_count == 0);

    wheel.tick(5'000'000ULL);
    REQUIRE(fire_count == 1);
    REQUIRE(fired_id == 7);

    wheel.tick(10'000'000ULL);  // one-shot: no re-fire on later ticks
    REQUIRE(fire_count == 1);
}

TEST_CASE("TimerWheel: cancel before the deadline suppresses the callback") {
    TimerWheel wheel(1'000'000ULL);
    int fire_count = 0;
    wheel.set_callback([&](child_id_t) { ++fire_count; });

    REQUIRE(wheel.insert(3, 0, 5'000'000ULL));
    wheel.cancel(3);
    wheel.tick(10'000'000ULL);

    REQUIRE(fire_count == 0);
}

// ---------------------------------------------------------------------------
// ExecutionCore integration: proves the DISPATCH_GAP fix for real, driven
// through the public API only (on_strategy_order), not by reaching into
// private state. Deliberately exhausts the real child pool instead of
// mocking it, kMaxTotalChildren is only 32768 and each filler order eats
// exactly 16 slots, so this is a few thousand cheap calls, not a stress test.
// ---------------------------------------------------------------------------
TEST_CASE("ExecutionCore: undispatched children release their margin reservation") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    // 16 ask levels, 1 lot each: a 16-lot order sweeps all of them, producing
    // exactly 16 children, exactly ParentOrder::kMaxChildrenPerParent.
    set_levels(ex.book.asks(), {
        {100,1},{101,1},{102,1},{103,1},{104,1},{105,1},{106,1},{107,1},
        {108,1},{109,1},{110,1},{111,1},{112,1},{113,1},{114,1},{115,1}
    });

    ExecCoreConfig cfg{};
    cfg.exchange_states  = &ex;
    cfg.active_exchanges = 1;
    cfg.risk_limits.max_order_lots[0]  = 100'000;
    cfg.risk_limits.max_net_lots[0]    = 100'000;
    cfg.risk_limits.lot_size[0]        = 1.0;
    cfg.margin.equity_usd              = 1e9;
    cfg.notional_gate.auto_approve_usd = 1e12;  // market orders skip the gate anyway (limit_price_usd = 0)
    cfg.dashboard.enabled              = false; // no need for a live snapshot thread in a unit test

    ExecutionCore core(cfg);

    StrategyOrderSignal filler{};
    filler.instr_id = 0;
    filler.dir      = OrderDir::BUY;
    filler.qty_lots = 16;

    // pack the child pool with full 16-child parents, stop with fewer than
    // 16 slots left so the next 16-lot order can't fully dispatch
    while (core.pool().children_in_use() + 16 <= kMaxTotalChildren - 11)
        core.on_strategy_order(filler);

    // top up to exactly 11 free slots with a smaller filler (sweeps only
    // the first 5 levels), landing on a number that isn't a multiple of 16
    StrategyOrderSignal small_filler = filler;
    small_filler.qty_lots = 5;
    core.on_strategy_order(small_filler);

    const uint32_t used_before    = core.pool().children_in_use();
    const uint32_t free_before    = kMaxTotalChildren - used_before;
    const int64_t  open_before    = core.positions().instruments[0].open_buy_lots.load();
    REQUIRE(free_before < 16);
    REQUIRE(free_before > 0);

    // this 16-lot order can only get free_before children out the door
    core.on_strategy_order(filler);

    const uint32_t dispatched = core.pool().children_in_use() - used_before;
    REQUIRE(dispatched == free_before);   // took exactly what was available, no more
    REQUIRE(dispatched < 16);             // proves the gap actually happened
    REQUIRE(dispatched > 0);              // and it wasn't a total failure either

    // the open-order reservation grew by exactly what got dispatched, not by
    // the full 16 lots the signal asked for, this is the actual fix: before
    // it, this would still show +16 (the gap's margin leaked, never released)
    const int64_t open_after = core.positions().instruments[0].open_buy_lots.load();
    REQUIRE(open_after - open_before == static_cast<int64_t>(dispatched));

    // and the parent itself must not be orphaned: it's still tracked, holding
    // exactly the children that got dispatched, waiting on those to terminate
    // so a later reroute can pick up the gap (DISPATCH_GAP doesn't shrink
    // leaves_qty_lots on purpose, see the comment at the call site)
    REQUIRE(core.pool().parents_in_use() > 0);
}

TEST_CASE("ExecutionCore: a parent whose last dispatched child fills still gets rerouted for its gap") {
    // 20 levels (kMaxDepth), 1 lot each. a 20-lot order needs all of them,
    // but ParentOrder::kMaxChildrenPerParent (16) caps one dispatch round,
    // so this always leaves a 4-lot gap without touching the real child
    // pool's capacity, cheap to set up compared to genuinely exhausting it.
    ExchangeState ex{};
    init_exchange(ex, 0);
    LevelSide& asks = ex.book.asks();
    for (uint32_t i = 0; i < 20; ++i) {
        asks.price_ticks[i] = 100 + static_cast<int64_t>(i);
        asks.qty_lots[i]    = 1;
    }
    asks.count = 20;
    asks.recompute_cumulative();

    ExecCoreConfig cfg{};
    cfg.exchange_states  = &ex;
    cfg.active_exchanges = 1;
    cfg.risk_limits.max_order_lots[0]  = 1000;
    cfg.risk_limits.max_net_lots[0]    = 1000;
    cfg.risk_limits.lot_size[0]        = 1.0;
    cfg.margin.equity_usd              = 1e9;
    cfg.notional_gate.auto_approve_usd = 1e12;
    cfg.dashboard.enabled              = false;

    ExecutionCore core(cfg);

    StrategyOrderSignal sig{};
    sig.instr_id = 0;
    sig.dir      = OrderDir::BUY;
    sig.qty_lots = 20;

    core.on_strategy_order(sig);
    REQUIRE(core.pool().parents_in_use()  == 1);
    REQUIRE(core.pool().children_in_use() == 16);  // capped, 4-lot gap parked in leaves_qty_lots

    // fill all 16 dispatched children. LIFO alloc on a fresh pool is
    // deterministic: the only parent this test ever creates is id 0, and its
    // children were allocated sequentially as ids 0..15.
    for (child_id_t cid = 0; cid < 16; ++cid) {
        ExecutionReport rep{};
        rep.child_id        = cid;
        rep.exec_type       = ExecType::FILL;
        rep.exchange_id     = 0;
        rep.fill_qty_lots   = 1;
        rep.fill_price_ticks = 100 + static_cast<price_t>(cid);
        rep.recv_ns          = 1;
        core.on_execution_report(rep);
    }

    // without the handle_fill fix: all 16 children are now FILLED and freed,
    // nothing else will ever generate an execution report for this parent, so
    // its remaining 4 lots would sit in leaves_qty_lots forever and the parent
    // pool slot would never free. with the fix, the 16th fill's
    // all_children_terminal() check (parent.leaves_qty_lots=4, all children
    // terminal) triggers reroute_leaves(), which dispatches fresh children
    // for the gap instead.
    REQUIRE(core.pool().children_in_use() == 4);
    REQUIRE(core.pool().parents_in_use()  == 1);  // same parent, still tracked, not orphaned
    REQUIRE(core.positions().instruments[0].net_qty_lots.load() == 16);
}
