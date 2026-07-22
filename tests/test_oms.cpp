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
