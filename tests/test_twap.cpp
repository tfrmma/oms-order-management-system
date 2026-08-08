// unit tests for TwapScheduler (algos/twap_scheduler.hpp). lives in its own
// file rather than test_oms.cpp: algos/ is a separate architectural layer,
// its tests should be too.

#include "catch_amalgamated.hpp"

#include "algos/twap_scheduler.hpp"

using namespace oms;
using namespace algos;

namespace {

TwapConfig base_config(qty_t total_lots, uint32_t num_slices, uint64_t duration_ns) {
    TwapConfig cfg{};
    cfg.instr_id       = 0;
    cfg.dir            = sor::OrderDir::BUY;
    cfg.strategy_id    = 1;
    cfg.total_qty_lots = total_lots;
    cfg.duration_ns    = duration_ns;
    cfg.num_slices     = num_slices;
    return cfg;
}

// drains a scheduler completely, returns every slice signal in order.
std::vector<StrategyOrderSignal> drain(TwapScheduler& sched, uint64_t start_ns) {
    sched.start(start_ns);
    std::vector<StrategyOrderSignal> out;
    while (!sched.done()) {
        StrategyOrderSignal sig{};
        REQUIRE(sched.tick(sched.next_deadline_ns(), sig));
        out.push_back(sig);
    }
    return out;
}

} // namespace

TEST_CASE("TwapScheduler: no jitter gives exact even slices") {
    auto cfg = base_config(1200, 12, 60'000'000'000ULL);
    cfg.size_jitter_frac     = 0.0;
    cfg.interval_jitter_frac = 0.0;

    TwapScheduler sched(cfg);
    auto slices = drain(sched, 0);

    REQUIRE(slices.size() == 12);
    for (const auto& s : slices) REQUIRE(s.qty_lots == 100);
}

TEST_CASE("TwapScheduler: first slice fires immediately at start_ns, no dead gap") {
    auto cfg = base_config(1000, 10, 10'000'000'000ULL);
    TwapScheduler sched(cfg);
    sched.start(555);

    StrategyOrderSignal sig{};
    REQUIRE(sched.tick(555, sig));
}

TEST_CASE("TwapScheduler: sum of slices always equals total_qty_lots exactly") {
    // jitter on, several seeds, this is the property that actually matters:
    // the tail slice has to absorb whatever rounding drift the randomized
    // slices left behind.
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        auto cfg = base_config(5000, 17, 60'000'000'000ULL);
        cfg.rng_seed = seed;

        TwapScheduler sched(cfg);
        auto slices = drain(sched, 0);

        qty_t sum = 0;
        for (const auto& s : slices) {
            REQUIRE(s.qty_lots > 0);   // no zero or negative slices, ever
            sum += s.qty_lots;
        }
        REQUIRE(sum == 5000);
    }
}

TEST_CASE("TwapScheduler: deadlines are monotonically non-decreasing") {
    auto cfg = base_config(5000, 17, 60'000'000'000ULL);
    cfg.rng_seed = 42;

    TwapScheduler sched(cfg);
    sched.start(0);

    uint64_t last = 0;
    while (!sched.done()) {
        uint64_t deadline = sched.next_deadline_ns();
        REQUIRE(deadline >= last);
        last = deadline;
        StrategyOrderSignal sig{};
        REQUIRE(sched.tick(deadline, sig));
    }
}

TEST_CASE("TwapScheduler: tick before the next deadline does nothing") {
    auto cfg = base_config(1000, 10, 10'000'000'000ULL);
    cfg.interval_jitter_frac = 0.0;
    TwapScheduler sched(cfg);
    sched.start(0);

    StrategyOrderSignal sig{};
    REQUIRE(sched.tick(0, sig));                       // first slice fires immediately
    REQUIRE_FALSE(sched.tick(500'000'000ULL, sig));    // next deadline is 1s out, not due yet
    REQUIRE(sched.tick(1'000'000'000ULL, sig));         // now it's due
}

TEST_CASE("TwapScheduler: more requested slices than lots, no negative or zero slices") {
    // this used to hit std::clamp with lo > hi (min_reserve exceeding
    // remaining_lots_) and hand back a negative qty_lots. num_slices gets
    // capped to total_qty_lots at construction now.
    auto cfg = base_config(3, 10, 10'000'000'000ULL);
    cfg.size_jitter_frac     = 0.0;
    cfg.interval_jitter_frac = 0.0;

    TwapScheduler sched(cfg);
    auto slices = drain(sched, 0);

    REQUIRE(slices.size() == 3);   // capped, not 10
    qty_t sum = 0;
    for (const auto& s : slices) {
        REQUIRE(s.qty_lots > 0);
        sum += s.qty_lots;
    }
    REQUIRE(sum == 3);
}

TEST_CASE("TwapScheduler: capped slice count still spans the full duration") {
    // the bug above had a second half: even after capping slice count so it
    // no longer went negative, the interval was still computed against the
    // originally requested num_slices, so the whole thing front-loaded into
    // the first ~30% of duration_ns instead of spreading across all of it.
    auto cfg = base_config(3, 10, 9'000'000'000ULL);   // 3 lots, 10 slices requested, 9s window
    cfg.size_jitter_frac     = 0.0;
    cfg.interval_jitter_frac = 0.0;

    TwapScheduler sched(cfg);
    sched.start(0);

    std::vector<uint64_t> deadlines;
    while (!sched.done()) {
        StrategyOrderSignal sig{};
        uint64_t deadline = sched.next_deadline_ns();
        REQUIRE(sched.tick(deadline, sig));
        deadlines.push_back(deadline);
    }

    REQUIRE(deadlines.size() == 3);
    // 3 effective slices over 9s -> nominal interval 3s, not 0.9s
    REQUIRE(deadlines[0] == 0);
    REQUIRE(deadlines[1] == 3'000'000'000ULL);
    REQUIRE(deadlines[2] == 6'000'000'000ULL);
}

TEST_CASE("TwapScheduler: single slice hands out everything at once") {
    auto cfg = base_config(777, 1, 5'000'000'000ULL);
    TwapScheduler sched(cfg);
    sched.start(0);

    StrategyOrderSignal sig{};
    REQUIRE(sched.tick(0, sig));
    REQUIRE(sig.qty_lots == 777);
    REQUIRE(sched.done());
}

TEST_CASE("TwapScheduler: num_slices of 0 in config doesn't divide by zero") {
    auto cfg = base_config(500, 0, 5'000'000'000ULL);   // caller forgot to set num_slices
    TwapScheduler sched(cfg);
    auto slices = drain(sched, 0);

    REQUIRE(slices.size() == 1);
    REQUIRE(slices[0].qty_lots == 500);
}

TEST_CASE("TwapScheduler: zero or negative total_qty_lots is immediately done") {
    auto cfg_zero = base_config(0, 10, 10'000'000'000ULL);
    TwapScheduler sched_zero(cfg_zero);
    REQUIRE(sched_zero.done());

    auto cfg_neg = base_config(-5, 10, 10'000'000'000ULL);
    TwapScheduler sched_neg(cfg_neg);
    REQUIRE(sched_neg.done());
}

TEST_CASE("TwapScheduler: signal fields carry through from config unchanged") {
    auto cfg = base_config(1000, 5, 5'000'000'000ULL);
    cfg.strategy_id     = 7;
    cfg.limit_price_usd = 65115.0;
    cfg.reduce_only     = true;
    cfg.order_type      = sor::OrderType::MAKER;
    cfg.size_jitter_frac     = 0.0;
    cfg.interval_jitter_frac = 0.0;

    TwapScheduler sched(cfg);
    sched.start(0);

    StrategyOrderSignal sig{};
    REQUIRE(sched.tick(0, sig));
    REQUIRE(sig.strategy_id     == 7);
    REQUIRE(sig.dir              == sor::OrderDir::BUY);
    REQUIRE(sig.limit_price_usd == Catch::Approx(65115.0));
    REQUIRE(sig.reduce_only     == true);
    REQUIRE(sig.order_type      == sor::OrderType::MAKER);
}

TEST_CASE("TwapScheduler: remaining_lots decreases monotonically to zero") {
    auto cfg = base_config(1000, 5, 5'000'000'000ULL);
    cfg.rng_seed = 9;
    TwapScheduler sched(cfg);
    sched.start(0);

    qty_t last = sched.remaining_lots();
    while (!sched.done()) {
        StrategyOrderSignal sig{};
        REQUIRE(sched.tick(sched.next_deadline_ns(), sig));
        REQUIRE(sched.remaining_lots() <= last);
        REQUIRE(sched.remaining_lots() >= 0);
        last = sched.remaining_lots();
    }
    REQUIRE(last == 0);
}
