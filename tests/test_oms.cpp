// unit tests for the SOR merge/fill algorithm, the order pool, position
// tracking, pre-trade risk, the SPSC queue, and the timer wheel, plus
// integration tests that drive a real ExecutionCore end to end through its
// public API (on_strategy_order/on_execution_report/tick), not mocks.
//
// ExecutionCore is a few MB (OMSOrderPool + TimerWheel dominate), heap-
// allocate it via std::make_unique like the README's own usage example
// does, don't put it on the stack.
//
// tick_size = lot_size = 1.0 everywhere here on purpose: price_ticks == price
// and qty_lots == qty, so expected values are exact integers instead of
// floating garbage. exchange_state_populated below spells out what each
// field means for anyone who wants a book with realistic tick/lot sizes.

#include "catch_amalgamated.hpp"
#include <cstdio>

#include "sor/routing_engine.hpp"
#include "order_pool.hpp"
#include "position_tracker.hpp"
#include "risk_engine.hpp"
#include "spsc_queue.hpp"
#include "timer_wheel.hpp"
#include "execution_core.hpp"

#include <chrono>
#include <memory>
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

// ExecutionCore's internal now_ns() reads CLOCK_REALTIME (see execution_core.cpp),
// which is what dispatch_child_orders stamps a child's sent_ns with before
// inserting it into the timer wheel. tests that drive ExecutionCore::tick()
// manually need timestamps on that same clock, std::chrono::system_clock
// maps to CLOCK_REALTIME on glibc, small fixed numbers like 1000 don't mean
// anything to a timer wheel whose deadlines are computed from the real epoch.
uint64_t real_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
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

TEST_CASE("RoutingEngine: maker BUY posts on the bid side, not the ask") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.bids(), {{99, 10}});
    set_levels(ex.book.asks(), {{101, 10}});

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_maker_placement(
        make_context(&ex, 1, OrderDir::BUY, 5), out);

    REQUIRE(r.success);
    REQUIRE(r.child_count == 1);
    REQUIRE(out.orders[0].price == Catch::Approx(99.0));   // the bid, not the ask
    REQUIRE(out.orders[0].qty   == Catch::Approx(5.0));
    REQUIRE(out.orders[0].type  == OrderType::MAKER);
    REQUIRE(out.orders[0].dir   == OrderDir::BUY);
    // nothing fills at placement time, it's a resting order
    REQUIRE(r.filled_qty   == Catch::Approx(0.0));
    REQUIRE(r.unfilled_qty == Catch::Approx(5.0));
}

TEST_CASE("RoutingEngine: maker SELL posts on the ask side, not the bid") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.bids(), {{99, 10}});
    set_levels(ex.book.asks(), {{101, 10}});

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_maker_placement(
        make_context(&ex, 1, OrderDir::SELL, 5), out);

    REQUIRE(r.success);
    REQUIRE(out.orders[0].price == Catch::Approx(101.0));  // the ask, not the bid
    REQUIRE(out.orders[0].type  == OrderType::MAKER);
}

TEST_CASE("RoutingEngine: maker picks the exchange with the best fee-adjusted price") {
    // both exchanges quote the identical raw bid, but exchange 1 pays a 1%
    // maker rebate, exchange 0 charges a flat 0% maker fee. same touch price,
    // different net economics, the rebate should win even though the raw
    // price is tied.
    ExchangeState ex[2]{};
    init_exchange(ex[0], 0);
    init_exchange(ex[1], 1);
    set_levels(ex[0].book.bids(), {{100, 10}});
    set_levels(ex[1].book.bids(), {{100, 10}});

    FeeMatrix fees{};
    fees.rates[0][0] = 0.0;    // exchange 0 maker fee: flat
    fees.rates[1][0] = -0.01;  // exchange 1 maker fee: 1% rebate

    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_maker_placement(
        make_context(ex, 2, OrderDir::BUY, 5), out);

    REQUIRE(r.success);
    REQUIRE(out.orders[0].exchange_id == 1);  // the rebate venue, not index 0
}

TEST_CASE("RoutingEngine: maker won't post through the caller's limit price") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.bids(), {{100, 10}});

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    // willing to pay at most 95, but the current best bid is 100, posting
    // there would mean paying more than the limit if it fills
    const SplitResult r = engine.calculate_maker_placement(
        make_context(&ex, 1, OrderDir::BUY, 5, /*limit_price=*/95.0), out);

    REQUIRE_FALSE(r.success);
    REQUIRE(out.count == 0);
}

TEST_CASE("RoutingEngine: maker fails cleanly when no exchange has a touch to post against") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    // bid side left empty on purpose, nothing to post a BUY maker against

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_maker_placement(
        make_context(&ex, 1, OrderDir::BUY, 5), out);

    REQUIRE_FALSE(r.success);
    REQUIRE(out.count == 0);
}

TEST_CASE("RoutingEngine: maker reports spread_capture_bps against the opposite touch") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.bids(), {{100, 10}});
    set_levels(ex.book.asks(), {{102, 10}});

    FeeMatrix fees{};
    RoutingEngine engine(fees);
    ChildOrderBuffer out;
    const SplitResult r = engine.calculate_maker_placement(
        make_context(&ex, 1, OrderDir::BUY, 5), out);

    REQUIRE(r.success);
    // (102-100)/100 * 10000 = 200 bps
    REQUIRE(r.spread_capture_bps == Catch::Approx(200.0));
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
    InstrumentTable instruments{};  // unconfigured, relies on InstrumentTable::kFallbackLotSize (0.001)

    SECTION("fat finger") {
        limits.max_order_lots[0] = 100;
        PreTradeRiskEngine risk(limits, instruments, pos);

        StrategyOrderSignal sig{};
        sig.instr_id = 0;
        sig.qty_lots = 150;
        REQUIRE(risk.validate(sig) == RiskRejectReason::FAT_FINGER);
    }

    SECTION("position limit") {
        limits.max_net_lots[0] = 50;
        pos.instruments[0].net_qty_lots.store(40);
        PreTradeRiskEngine risk(limits, instruments, pos);

        StrategyOrderSignal sig{};
        sig.instr_id = 0;
        sig.dir      = OrderDir::BUY;
        sig.qty_lots = 20;  // 40 + 20 = 60 > 50
        REQUIRE(risk.validate(sig) == RiskRejectReason::POSITION_LIMIT);
    }

    SECTION("notional cap") {
        limits.max_notional_usd = 10'000.0;
        PreTradeRiskEngine risk(limits, instruments, pos);

        StrategyOrderSignal sig{};
        sig.instr_id        = 0;
        sig.qty_lots        = 1000;
        sig.limit_price_usd = 50'000.0;  // 1000 * 0.001 (fallback lot_size) * 50000 = 50000 > 10000
        REQUIRE(risk.validate(sig) == RiskRejectReason::NOTIONAL);
    }

    SECTION("margin") {
        limits.min_margin_required = 1000.0;
        pos.margin.update(/*equity=*/500.0, /*used=*/0.0);
        PreTradeRiskEngine risk(limits, instruments, pos);

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
        PreTradeRiskEngine risk(limits, instruments, pos);

        StrategyOrderSignal sig{};
        sig.instr_id        = 0;
        sig.dir              = OrderDir::BUY;
        sig.qty_lots         = 10;
        sig.limit_price_usd  = 100.0;
        REQUIRE(risk.validate(sig) == RiskRejectReason::OK);
    }
}

TEST_CASE("PreTradeRiskEngine: clamp_reduce_only") {
    PositionTable pos{};
    RiskLimits limits{};
    InstrumentTable instruments{};
    PreTradeRiskEngine risk(limits, instruments, pos);

    SECTION("not reduce-only, no unwind: passes through unchanged") {
        pos.instruments[0].net_qty_lots.store(5);
        StrategyOrderSignal sig{};
        sig.instr_id = 0;
        sig.dir      = OrderDir::BUY;
        sig.qty_lots = 100;  // would grow the long position, fine, not reduce-only
        REQUIRE(risk.clamp_reduce_only(sig) == 100);
    }

    SECTION("wrong direction: long position, BUY reduce-only rejected (0)") {
        pos.instruments[0].net_qty_lots.store(5);  // long 5
        StrategyOrderSignal sig{};
        sig.instr_id     = 0;
        sig.dir          = OrderDir::BUY;  // would grow the long, not reduce it
        sig.qty_lots     = 3;
        sig.reduce_only  = true;
        REQUIRE(risk.clamp_reduce_only(sig) == 0);
    }

    SECTION("flat position: any reduce-only rejected (0)") {
        pos.instruments[0].net_qty_lots.store(0);
        StrategyOrderSignal sig{};
        sig.instr_id    = 0;
        sig.dir         = OrderDir::SELL;
        sig.qty_lots    = 1;
        sig.reduce_only = true;
        REQUIRE(risk.clamp_reduce_only(sig) == 0);
    }

    SECTION("right direction, oversized: clamped to exactly what closes the position") {
        pos.instruments[0].net_qty_lots.store(-32);  // short 32
        StrategyOrderSignal sig{};
        sig.instr_id    = 0;
        sig.dir         = OrderDir::BUY;   // BUY reduces a short
        sig.qty_lots    = 50;              // asking for more than the position has
        sig.reduce_only = true;
        REQUIRE(risk.clamp_reduce_only(sig) == 32);
    }

    SECTION("right direction, undersized: passes through unchanged") {
        pos.instruments[0].net_qty_lots.store(-32);
        StrategyOrderSignal sig{};
        sig.instr_id    = 0;
        sig.dir         = OrderDir::BUY;
        sig.qty_lots    = 10;  // well within the 32 headroom
        sig.reduce_only = true;
        REQUIRE(risk.clamp_reduce_only(sig) == 10);
    }

    SECTION("SELL reduces a long, same clamp logic mirrored") {
        pos.instruments[0].net_qty_lots.store(7);  // long 7
        StrategyOrderSignal sig{};
        sig.instr_id    = 0;
        sig.dir         = OrderDir::SELL;
        sig.qty_lots    = 20;
        sig.reduce_only = true;
        REQUIRE(risk.clamp_reduce_only(sig) == 7);
    }

    SECTION("forced by unwind_only even without the signal's own flag set") {
        pos.instruments[0].net_qty_lots.store(-32);
        pos.instruments[0].unwind_only.store(true);  // ops kill switch, not the strategy's doing
        StrategyOrderSignal sig{};
        sig.instr_id    = 0;
        sig.dir         = OrderDir::BUY;
        sig.qty_lots    = 50;
        sig.reduce_only = false;  // strategy didn't ask for this, kill switch overrides anyway
        REQUIRE(risk.clamp_reduce_only(sig) == 32);
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
    cfg.instruments.instruments[0].lot_size = 1.0;
    cfg.margin.equity_usd              = 1e9;
    cfg.notional_gate.auto_approve_usd = 1e12;  // market orders skip the gate anyway (limit_price_usd = 0)
    cfg.dashboard.enabled              = false; // no need for a live snapshot thread in a unit test

    auto core = std::make_unique<ExecutionCore>(cfg);

    StrategyOrderSignal filler{};
    filler.instr_id = 0;
    filler.dir      = OrderDir::BUY;
    filler.qty_lots = 16;

    // pack the child pool with full 16-child parents, stop with fewer than
    // 16 slots left so the next 16-lot order can't fully dispatch
    while (core->pool().children_in_use() + 16 <= kMaxTotalChildren - 11)
        core->on_strategy_order(filler);

    // top up to exactly 11 free slots with a smaller filler (sweeps only
    // the first 5 levels), landing on a number that isn't a multiple of 16
    StrategyOrderSignal small_filler = filler;
    small_filler.qty_lots = 5;
    core->on_strategy_order(small_filler);

    const uint32_t used_before    = core->pool().children_in_use();
    const uint32_t free_before    = kMaxTotalChildren - used_before;
    const int64_t  open_before    = core->positions().instruments[0].open_buy_lots.load();
    REQUIRE(free_before < 16);
    REQUIRE(free_before > 0);

    // this 16-lot order can only get free_before children out the door
    core->on_strategy_order(filler);

    const uint32_t dispatched = core->pool().children_in_use() - used_before;
    REQUIRE(dispatched == free_before);   // took exactly what was available, no more
    REQUIRE(dispatched < 16);             // proves the gap actually happened
    REQUIRE(dispatched > 0);              // and it wasn't a total failure either

    // the open-order reservation grew by exactly what got dispatched, not by
    // the full 16 lots the signal asked for, this is the actual fix: before
    // it, this would still show +16 (the gap's margin leaked, never released)
    const int64_t open_after = core->positions().instruments[0].open_buy_lots.load();
    REQUIRE(open_after - open_before == static_cast<int64_t>(dispatched));

    // and the parent itself must not be orphaned: it's still tracked, holding
    // exactly the children that got dispatched, waiting on those to terminate
    // so a later reroute can pick up the gap (DISPATCH_GAP doesn't shrink
    // leaves_qty_lots on purpose, see the comment at the call site)
    REQUIRE(core->pool().parents_in_use() > 0);
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
    cfg.instruments.instruments[0].lot_size = 1.0;
    cfg.margin.equity_usd              = 1e9;
    cfg.notional_gate.auto_approve_usd = 1e12;
    cfg.dashboard.enabled              = false;

    auto core = std::make_unique<ExecutionCore>(cfg);

    StrategyOrderSignal sig{};
    sig.instr_id = 0;
    sig.dir      = OrderDir::BUY;
    sig.qty_lots = 20;

    core->on_strategy_order(sig);
    REQUIRE(core->pool().parents_in_use()  == 1);
    REQUIRE(core->pool().children_in_use() == 16);  // capped, 4-lot gap parked in leaves_qty_lots

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
        core->on_execution_report(rep);
    }

    // without the handle_fill fix: all 16 children are now FILLED and freed,
    // nothing else will ever generate an execution report for this parent, so
    // its remaining 4 lots would sit in leaves_qty_lots forever and the parent
    // pool slot would never free. with the fix, the 16th fill's
    // all_children_terminal() check (parent.leaves_qty_lots=4, all children
    // terminal) triggers reroute_leaves(), which dispatches fresh children
    // for the gap instead.
    REQUIRE(core->pool().children_in_use() == 4);
    REQUIRE(core->pool().parents_in_use()  == 1);  // same parent, still tracked, not orphaned
    REQUIRE(core->positions().instruments[0].net_qty_lots.load() == 16);
}

TEST_CASE("ExecutionCore: fills from exchanges with different native lot sizes convert to canonical units") {
    // exchange 0: native lot_size 1.0 (1 lot = 1 real unit), best price.
    // exchange 1: native lot_size 4.0 (1 lot = 4 real units), worse price so
    // it only gets used for the remainder after exchange 0's depth is spent.
    // ExecutionReport.fill_qty_lots always arrives in the reporting child's
    // OWN native unit, this is what used to get summed directly into
    // parent.cum_qty_lots / pos_.net_qty_lots without conversion.
    ExchangeState ex[2]{};
    init_exchange(ex[0], 0);
    init_exchange(ex[1], 1);
    ex[1].book.lot_size = 4.0;
    ex[1].lot.lot_size  = 4.0;
    set_levels(ex[0].book.asks(), {{100, 10}});  // 10 real units
    set_levels(ex[1].book.asks(), {{101, 5}});   // 5 native lots = 20 real units, worse price

    ExecCoreConfig cfg{};
    cfg.exchange_states  = ex;
    cfg.active_exchanges = 2;
    cfg.risk_limits.max_order_lots[0]  = 1000;
    cfg.risk_limits.max_net_lots[0]    = 1000;
    cfg.instruments.instruments[0].lot_size = 1.0;  // canonical unit: 1 real unit
    cfg.margin.equity_usd              = 1e9;
    cfg.notional_gate.auto_approve_usd = 1e12;
    cfg.dashboard.enabled              = false;

    auto core = std::make_unique<ExecutionCore>(cfg);

    StrategyOrderSignal sig{};
    sig.instr_id = 0;
    sig.dir      = OrderDir::BUY;
    sig.qty_lots = 18;   // 10 from exchange 0 (its full depth) + 8 real units from exchange 1

    core->on_strategy_order(sig);
    REQUIRE(core->pool().children_in_use() == 2);

    // deterministic ids on a fresh pool: child 0 is exchange 0's leg (10 real
    // units, native lot_size 1.0 so native == canonical == 10), child 1 is
    // exchange 1's leg (8 real units -> to_lots(8.0, 4.0) = 2 native lots).
    ExecutionReport fill0{};
    fill0.child_id        = 0;
    fill0.exec_type       = ExecType::FILL;
    fill0.exchange_id     = 0;
    fill0.fill_qty_lots   = 10;   // native, exchange 0
    fill0.fill_price_ticks = 100;
    fill0.recv_ns          = 1;
    core->on_execution_report(fill0);

    ExecutionReport fill1{};
    fill1.child_id        = 1;
    fill1.exec_type       = ExecType::FILL;
    fill1.exchange_id     = 1;
    fill1.fill_qty_lots   = 2;    // native, exchange 1: 2 native lots * 4.0 = 8 real units
    fill1.fill_price_ticks = 101;
    fill1.recv_ns          = 1;
    core->on_execution_report(fill1);

    // 10 (native==canonical on exchange 0) + 8 (2 native lots * 4.0 lot_size,
    // converted) = 18 canonical units, exactly the original order size.
    // before the fix this would read 10 + 2 = 12: exchange 1's native lot
    // count summed directly instead of converted to real units first.
    REQUIRE(core->positions().instruments[0].net_qty_lots.load() == 18);
    REQUIRE(core->pool().parents_in_use()  == 0);  // fully filled, freed
    REQUIRE(core->pool().children_in_use() == 0);
}

TEST_CASE("ExecutionCore: rejected signals land on reject_queue with the right reason") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.asks(), {{100, 1000}});  // plenty of liquidity for the paths that need it

    ExecCoreConfig cfg{};
    cfg.exchange_states  = &ex;
    cfg.active_exchanges = 1;
    cfg.instruments.instruments[0].lot_size = 1.0;
    cfg.margin.equity_usd = 1e9;
    cfg.dashboard.enabled = false;

    SECTION("risk fat-finger reject") {
        cfg.risk_limits.max_order_lots[0]  = 5;
        cfg.notional_gate.auto_approve_usd = 1e12;
        auto core = std::make_unique<ExecutionCore>(cfg);

        StrategyOrderSignal sig{};
        sig.instr_id = 0;
        sig.dir      = OrderDir::BUY;
        sig.qty_lots = 500;  // way over max_order_lots
        core->on_strategy_order(sig);

        OrderReject rej{};
        REQUIRE(core->reject_queue.pop(rej));
        REQUIRE(rej.reason      == RejectReason::RISK_FAT_FINGER);
        REQUIRE(rej.instr_id    == 0);
        REQUIRE(rej.qty_lots    == 500);
        REQUIRE(core->pool().parents_in_use() == 0);  // never made it past risk, freed
    }

    SECTION("no liquidity reject") {
        cfg.risk_limits.max_order_lots[0]  = 100'000;
        cfg.notional_gate.auto_approve_usd = 1e12;
        ExchangeState empty{};
        init_exchange(empty, 0);  // valid book, zero levels: no liquidity, not a bad book
        cfg.exchange_states = &empty;
        auto core = std::make_unique<ExecutionCore>(cfg);

        StrategyOrderSignal sig{};
        sig.instr_id = 0;
        sig.dir      = OrderDir::BUY;
        sig.qty_lots = 10;
        core->on_strategy_order(sig);

        OrderReject rej{};
        REQUIRE(core->reject_queue.pop(rej));
        REQUIRE(rej.reason == RejectReason::NO_LIQUIDITY);
    }

    SECTION("notional gate hard block") {
        cfg.risk_limits.max_order_lots[0]  = 100'000;
        cfg.notional_gate.hard_block_usd   = 1'000.0;
        auto core = std::make_unique<ExecutionCore>(cfg);

        StrategyOrderSignal sig{};
        sig.instr_id        = 0;
        sig.dir              = OrderDir::BUY;
        sig.qty_lots         = 100;
        sig.limit_price_usd  = 500.0;  // 100 * 1.0 * 500 = 50,000 USD, way over the 1,000 hard block
        core->on_strategy_order(sig);

        OrderReject rej{};
        REQUIRE(core->reject_queue.pop(rej));
        REQUIRE(rej.reason == RejectReason::NOTIONAL_GATE_BLOCK);
        REQUIRE(core->pool().parents_in_use() == 0);  // blocked before any parent was even allocated
    }
}

TEST_CASE("ExecutionCore: maker order posts passively, then sweeps as taker if unfilled by maker_timeout_ns") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.bids(), {{100, 10}});   // maker BUY posts here
    set_levels(ex.book.asks(), {{102, 20}});   // taker sweep lands here if it happens

    ExecCoreConfig cfg{};
    cfg.exchange_states  = &ex;
    cfg.active_exchanges = 1;
    cfg.risk_limits.max_order_lots[0]  = 1000;
    cfg.risk_limits.max_net_lots[0]    = 1000;
    cfg.instruments.instruments[0].lot_size = 1.0;
    cfg.margin.equity_usd              = 1e9;
    cfg.notional_gate.auto_approve_usd = 1e12;
    cfg.dashboard.enabled              = false;
    cfg.maker_timeout_ns               = 5'000'000ULL;  // 5ms, short so the test stays fast

    auto core = std::make_unique<ExecutionCore>(cfg);

    StrategyOrderSignal sig{};
    sig.instr_id   = 0;
    sig.dir        = OrderDir::BUY;
    sig.qty_lots   = 8;
    sig.order_type = OrderType::MAKER;

    core->on_strategy_order(sig);

    // posted, not filled, resting passively at the touch
    REQUIRE(core->pool().parents_in_use()  == 1);
    REQUIRE(core->pool().children_in_use() == 1);
    REQUIRE(core->pool().child(0).order_type == OrderType::MAKER);
    REQUIRE(core->pool().child(0).price      == Catch::Approx(100.0));
    REQUIRE(core->pool().parent(0).order_type == OrderType::MAKER);
    REQUIRE(core->pool().parent(0).state      == OrderState::PENDING_NEW);

    // well before the deadline: still resting, untouched
    core->tick(real_now_ns() + 1'000ULL);
    REQUIRE(core->pool().child(0).state == OrderState::PENDING_NEW);
    REQUIRE(core->pool().children_in_use() == 1);

    // past maker_timeout_ns with no fill ever arriving
    core->tick(real_now_ns() + cfg.maker_timeout_ns + 1'000'000ULL);

    // the timed-out maker child got canceled and freed, and reroute_leaves
    // swept the remainder as taker, landing on the ask liquidity. LIFO free
    // list means the new child reuses the same id the canceled one just
    // vacated, not a fresh one, that's not a bug, see OMSOrderPool.
    REQUIRE(core->pool().children_in_use() == 1);
    REQUIRE(core->pool().child(0).order_type == OrderType::TAKER);
    REQUIRE(core->pool().child(0).price      == Catch::Approx(102.0));
    REQUIRE(core->pool().child(0).qty_lots   == 8);

    // the parent remembers how it started, reroute_leaves doesn't rewrite that
    REQUIRE(core->pool().parent(0).order_type == OrderType::MAKER);
}

TEST_CASE("ExecutionCore: maker order that fills before the timeout never sweeps") {
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.bids(), {{100, 10}});
    set_levels(ex.book.asks(), {{102, 20}});

    ExecCoreConfig cfg{};
    cfg.exchange_states  = &ex;
    cfg.active_exchanges = 1;
    cfg.risk_limits.max_order_lots[0]  = 1000;
    cfg.risk_limits.max_net_lots[0]    = 1000;
    cfg.instruments.instruments[0].lot_size = 1.0;
    cfg.margin.equity_usd              = 1e9;
    cfg.notional_gate.auto_approve_usd = 1e12;
    cfg.dashboard.enabled              = false;
    cfg.maker_timeout_ns               = 5'000'000ULL;

    auto core = std::make_unique<ExecutionCore>(cfg);

    StrategyOrderSignal sig{};
    sig.instr_id   = 0;
    sig.dir        = OrderDir::BUY;
    sig.qty_lots   = 8;
    sig.order_type = OrderType::MAKER;
    core->on_strategy_order(sig);

    // someone hits the resting bid before the timeout
    ExecutionReport fill{};
    fill.child_id         = 0;
    fill.exec_type        = ExecType::FILL;
    fill.exchange_id      = 0;
    fill.fill_qty_lots    = 8;
    fill.fill_price_ticks = 100;
    fill.recv_ns           = 1;
    core->on_execution_report(fill);

    REQUIRE(core->positions().instruments[0].net_qty_lots.load() == 8);
    REQUIRE(core->pool().parents_in_use()  == 0);  // fully filled, freed
    REQUIRE(core->pool().children_in_use() == 0);

    // ticking well past the maker timeout now must not do anything, there's
    // nothing left in the timer wheel for this order, cancel() already ran
    // when the fill came in
    core->tick(real_now_ns() + cfg.maker_timeout_ns + 1'000'000ULL);
    REQUIRE(core->pool().parents_in_use()  == 0);
    REQUIRE(core->pool().children_in_use() == 0);
}

TEST_CASE("ExecutionCore: reduce_only clamps or rejects based on current position") {
    SECTION("oversized reduce-only gets clamped to exactly what closes the position") {
        ExchangeState ex{};
        init_exchange(ex, 0);
        set_levels(ex.book.bids(), {{100, 100}});
        set_levels(ex.book.asks(), {{101, 100}});

        ExecCoreConfig cfg{};
        cfg.exchange_states  = &ex;
        cfg.active_exchanges = 1;
        cfg.risk_limits.max_order_lots[0]  = 1000;
        cfg.risk_limits.max_net_lots[0]    = 1000;
        cfg.instruments.instruments[0].lot_size = 1.0;
        cfg.margin.equity_usd              = 1e9;
        cfg.notional_gate.auto_approve_usd = 1e12;
        cfg.dashboard.enabled              = false;

        auto core = std::make_unique<ExecutionCore>(cfg);

        // establish a short position of 10 via a normal SELL + fill
        StrategyOrderSignal open{};
        open.instr_id = 0;
        open.dir      = OrderDir::SELL;
        open.qty_lots = 10;
        core->on_strategy_order(open);

        ExecutionReport fill{};
        fill.child_id         = 0;
        fill.exec_type        = ExecType::FILL;
        fill.exchange_id      = 0;
        fill.fill_qty_lots    = 10;
        fill.fill_price_ticks = 100;
        fill.recv_ns           = 1;
        core->on_execution_report(fill);

        REQUIRE(core->positions().instruments[0].net_qty_lots.load() == -10);
        REQUIRE(core->pool().parents_in_use() == 0);  // fully filled, freed

        // LIFO: parent 0 and child 0 just got freed, this reuses those ids
        StrategyOrderSignal reduce{};
        reduce.instr_id    = 0;
        reduce.dir         = OrderDir::BUY;
        reduce.qty_lots    = 25;  // more than the 10 short
        reduce.reduce_only = true;
        core->on_strategy_order(reduce);

        REQUIRE(core->pool().parents_in_use()   == 1);
        REQUIRE(core->pool().parent(0).total_qty_lots  == 10);  // clamped, not 25
        REQUIRE(core->pool().parent(0).leaves_qty_lots == 10);
        REQUIRE(core->pool().parent(0).reduce_only);
        REQUIRE(core->pool().child(0).qty_lots == 10);
    }

    SECTION("wrong-direction reduce-only is rejected, not silently ignored") {
        ExchangeState ex{};
        init_exchange(ex, 0);
        set_levels(ex.book.bids(), {{100, 100}});
        set_levels(ex.book.asks(), {{101, 100}});

        ExecCoreConfig cfg{};
        cfg.exchange_states  = &ex;
        cfg.active_exchanges = 1;
        cfg.risk_limits.max_order_lots[0]  = 1000;
        cfg.risk_limits.max_net_lots[0]    = 1000;
        cfg.instruments.instruments[0].lot_size = 1.0;
        cfg.margin.equity_usd              = 1e9;
        cfg.notional_gate.auto_approve_usd = 1e12;
        cfg.dashboard.enabled              = false;

        auto core = std::make_unique<ExecutionCore>(cfg);

        StrategyOrderSignal open{};
        open.instr_id = 0;
        open.dir      = OrderDir::SELL;
        open.qty_lots = 10;
        core->on_strategy_order(open);
        ExecutionReport fill{};
        fill.child_id = 0; fill.exec_type = ExecType::FILL; fill.exchange_id = 0;
        fill.fill_qty_lots = 10; fill.fill_price_ticks = 100; fill.recv_ns = 1;
        core->on_execution_report(fill);
        REQUIRE(core->positions().instruments[0].net_qty_lots.load() == -10);  // short

        // SELL reduce-only while already short would grow the short, wrong direction
        StrategyOrderSignal bad{};
        bad.instr_id    = 0;
        bad.dir         = OrderDir::SELL;
        bad.qty_lots    = 5;
        bad.reduce_only = true;
        core->on_strategy_order(bad);

        REQUIRE(core->pool().parents_in_use() == 0);  // rejected, no parent created
        OrderReject rej{};
        REQUIRE(core->reject_queue.pop(rej));
        REQUIRE(rej.reason == RejectReason::REDUCE_ONLY_VIOLATION);
    }

    SECTION("reduce-only on a flat position is rejected") {
        ExchangeState ex{};
        init_exchange(ex, 0);
        set_levels(ex.book.bids(), {{100, 100}});
        set_levels(ex.book.asks(), {{101, 100}});

        ExecCoreConfig cfg{};
        cfg.exchange_states  = &ex;
        cfg.active_exchanges = 1;
        cfg.instruments.instruments[0].lot_size = 1.0;
        cfg.margin.equity_usd              = 1e9;
        cfg.notional_gate.auto_approve_usd = 1e12;
        cfg.dashboard.enabled              = false;

        auto core = std::make_unique<ExecutionCore>(cfg);
        REQUIRE(core->positions().instruments[0].net_qty_lots.load() == 0);

        StrategyOrderSignal sig{};
        sig.instr_id    = 0;
        sig.dir         = OrderDir::BUY;
        sig.qty_lots    = 5;
        sig.reduce_only = true;
        core->on_strategy_order(sig);

        REQUIRE(core->pool().parents_in_use() == 0);
        OrderReject rej{};
        REQUIRE(core->reject_queue.pop(rej));
        REQUIRE(rej.reason == RejectReason::REDUCE_ONLY_VIOLATION);
    }

    SECTION("set_unwind_mode forces the clamp even when the signal doesn't ask for it") {
        ExchangeState ex{};
        init_exchange(ex, 0);
        set_levels(ex.book.bids(), {{100, 100}});
        set_levels(ex.book.asks(), {{101, 100}});

        ExecCoreConfig cfg{};
        cfg.exchange_states  = &ex;
        cfg.active_exchanges = 1;
        cfg.risk_limits.max_order_lots[0]  = 1000;
        cfg.risk_limits.max_net_lots[0]    = 1000;
        cfg.instruments.instruments[0].lot_size = 1.0;
        cfg.margin.equity_usd              = 1e9;
        cfg.notional_gate.auto_approve_usd = 1e12;
        cfg.dashboard.enabled              = false;

        auto core = std::make_unique<ExecutionCore>(cfg);

        StrategyOrderSignal open{};
        open.instr_id = 0;
        open.dir      = OrderDir::SELL;
        open.qty_lots = 10;
        core->on_strategy_order(open);
        ExecutionReport fill{};
        fill.child_id = 0; fill.exec_type = ExecType::FILL; fill.exchange_id = 0;
        fill.fill_qty_lots = 10; fill.fill_price_ticks = 100; fill.recv_ns = 1;
        core->on_execution_report(fill);
        REQUIRE(core->positions().instruments[0].net_qty_lots.load() == -10);

        core->set_unwind_mode(0, true);  // ops kill switch, not the strategy's doing

        StrategyOrderSignal sig{};
        sig.instr_id    = 0;
        sig.dir         = OrderDir::BUY;
        sig.qty_lots    = 999;       // strategy asking for way more than the position
        sig.reduce_only = false;     // and NOT setting the flag itself
        core->on_strategy_order(sig);

        REQUIRE(core->pool().parents_in_use() == 1);
        REQUIRE(core->pool().parent(0).total_qty_lots == 10);  // clamped anyway
        REQUIRE(core->pool().parent(0).reduce_only);
    }
}

TEST_CASE("ExecutionCore: reroute_leaves re-clamps reduce_only against in-flight reservations") {
    // 20 ask levels, 1 lot each: enough for a 20-lot BUY to need every level,
    // but ParentOrder::kMaxChildrenPerParent (16) caps the first dispatch,
    // leaving a real, deterministic 4-lot gap that reroute_leaves has to
    // pick up later. that's the exact moment the re-clamp has to fire.
    ExchangeState ex{};
    init_exchange(ex, 0);
    set_levels(ex.book.bids(), {{100, 1000}});
    LevelSide& asks = ex.book.asks();
    for (uint32_t i = 0; i < 20; ++i) {
        asks.price_ticks[i] = 200 + static_cast<int64_t>(i);
        asks.qty_lots[i]    = 1;
    }
    asks.count = 20;
    asks.recompute_cumulative();

    ExecCoreConfig cfg{};
    cfg.exchange_states  = &ex;
    cfg.active_exchanges = 1;
    cfg.risk_limits.max_order_lots[0]  = 1000;
    cfg.risk_limits.max_net_lots[0]    = 1000;
    cfg.instruments.instruments[0].lot_size = 1.0;
    cfg.margin.equity_usd              = 1e9;
    cfg.notional_gate.auto_approve_usd = 1e12;
    cfg.dashboard.enabled              = false;

    auto core = std::make_unique<ExecutionCore>(cfg);

    // short 20 via a SELL against the deep bid
    StrategyOrderSignal open{};
    open.instr_id = 0;
    open.dir      = OrderDir::SELL;
    open.qty_lots = 20;
    core->on_strategy_order(open);
    ExecutionReport open_fill{};
    open_fill.child_id = 0; open_fill.exec_type = ExecType::FILL; open_fill.exchange_id = 0;
    open_fill.fill_qty_lots = 20; open_fill.fill_price_ticks = 100; open_fill.recv_ns = 1;
    core->on_execution_report(open_fill);
    REQUIRE(core->positions().instruments[0].net_qty_lots.load() == -20);
    REQUIRE(core->pool().parents_in_use() == 0);  // opening order fully filled, freed

    // parent A (id 0, LIFO reuse): reduce-only BUY for the FULL 20. clamp at
    // signal time sees net=-20, open_buy_lots=0 (nothing else outstanding
    // yet) -> headroom 20, unclamped. dispatch caps at 16 children though,
    // DISPATCH_GAP releases margin for the undispatched 4, leaving
    // open_buy_lots at 16 and parent A's leaves_qty_lots at 4.
    StrategyOrderSignal a{};
    a.instr_id    = 0;
    a.dir         = OrderDir::BUY;
    a.qty_lots    = 20;
    a.reduce_only = true;
    core->on_strategy_order(a);
    REQUIRE(core->pool().parent(0).total_qty_lots  == 20);
    // leaves_qty_lots = total - cum, still 20 here: nothing has FILLED yet,
    // dispatch alone doesn't touch it. the 4-lot gap from the
    // kMaxChildrenPerParent cap only becomes visible once the 16 dispatched
    // children actually fill and cum_qty_lots catches up.
    REQUIRE(core->pool().parent(0).leaves_qty_lots == 20);
    REQUIRE(core->pool().children_in_use() == 16);

    // parent B (id 1): a SEPARATE reduce-only BUY for 4, submitted before A
    // fills anything. clamp sees net=-20 (still, A hasn't filled), but
    // open_buy_lots is now 16 (A's dispatched exposure) -> headroom is only
    // 20-16=4, so B's own 4-lot request passes through unclamped, using up
    // the LAST of the shared headroom. this is the part that would silently
    // overshoot without accounting for open_buy_lots: without it, B would
    // ALSO see a naive 20 lots of headroom.
    StrategyOrderSignal b{};
    b.instr_id    = 0;
    b.dir         = OrderDir::BUY;
    b.qty_lots    = 4;
    b.reduce_only = true;
    core->on_strategy_order(b);
    REQUIRE(core->pool().parent(1).total_qty_lots == 4);
    REQUIRE(core->pool().parents_in_use()  == 2);
    REQUIRE(core->pool().children_in_use() == 20);  // A's 16 + B's 4

    // fill all 16 of A's children. the LAST fill flips A's leaves_qty_lots
    // (4) to a reroute attempt, exactly when the re-clamp has to run.
    for (child_id_t cid = 0; cid < 16; ++cid) {
        ExecutionReport rep{};
        rep.child_id = cid; rep.exec_type = ExecType::FILL; rep.exchange_id = 0;
        rep.fill_qty_lots = 1;
        rep.fill_price_ticks = 200 + static_cast<price_t>(cid);
        rep.recv_ns = 1;
        core->on_execution_report(rep);
    }
    REQUIRE(core->positions().instruments[0].net_qty_lots.load() == -4);

    // without the fix: A's stale leaves_qty_lots (4) would get re-dispatched
    // as-is, on top of B's ALREADY-reserved 4, trying to commit 8 lots of BUY
    // exposure against only 4 lots of remaining short. with the fix: A's
    // reroute re-clamps against net=-4 minus B's still-outstanding
    // open_buy_lots=4, finds zero headroom left, and closes out instead of
    // dispatching anything new.
    REQUIRE(core->pool().parent(0).state == OrderState::FILLED);
    // parent B is untouched by any of this, its own 4 children are still
    // exactly what it originally dispatched
    REQUIRE(core->pool().parent(1).leaves_qty_lots == 4);
    REQUIRE(core->pool().children_in_use() == 4);  // only B's 4 children remain live
}
