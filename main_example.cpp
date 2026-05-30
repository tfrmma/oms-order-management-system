// synthetic 5-exchange demo. good enough to shake out bugs.
// don't read too much into the numbers — books are made up.
//
// compile:
//   g++ -std=c++20 -O3 -march=native -fno-exceptions
//       main_example.cpp sor/routing_engine.cpp -I. -o sor_example

#include "sor/routing_engine.hpp"

#include <iomanip>
#include <iostream>

using namespace sor;

// fills one ExchangeState with a synthetic book. best_ask is the top of ask;
// depth grows thicker as you go further out (realistic-ish shape).
static void populate_mock_exchange(
    ExchangeState& state,
    uint8_t        ex_id,
    double         best_ask,
    double         tick,
    double         qty_per_level,
    double         rtt_us,
    const FeeMatrix& fees
) noexcept {
    // can't memset: atomic members in LatencyTracker
    state.book.invalidate();
    state.book.last_sequence  = 0;
    state.book.last_update_ns = 0;
    state.book.tick_size      = tick;
    state.book.lot_size       = 0.001;

    state.exchange_id = ex_id;
    state.enabled     = true;
    state.fees        = fees;

    state.lot = { 0.001, tick, 0.001, 20.0, 0.0 };

    state.latency.ewma_rtt_us.store(rtt_us, std::memory_order_relaxed);

    state.book.is_valid      = true;
    state.book.exchange_id   = ex_id;
    state.book.last_sequence = 1;

    const price_t best_ask_ticks = to_ticks(best_ask, tick);

    auto& asks = state.book.sides[static_cast<uint8_t>(Side::ASK)];
    asks.count = kMaxDepth;
    for (uint32_t i = 0; i < kMaxDepth; ++i) {
        asks.price_ticks[i] = best_ask_ticks + static_cast<price_t>(i);
        asks.qty_lots[i]    = to_lots(qty_per_level * (0.5 + 0.5 * static_cast<double>(i + 1)), 0.001);
    }
    asks.recompute_cumulative();

    auto& bids = state.book.sides[static_cast<uint8_t>(Side::BID)];
    bids.count = kMaxDepth;
    for (uint32_t i = 0; i < kMaxDepth; ++i) {
        bids.price_ticks[i] = (best_ask_ticks - 1) - static_cast<price_t>(i);
        bids.qty_lots[i]    = to_lots(qty_per_level * (0.5 + 0.5 * static_cast<double>(i + 1)), 0.001);
    }
    bids.recompute_cumulative();
}

static const char* exchange_name(uint8_t id) noexcept {
    static const char* names[] = { "BINANCE", "BYBIT", "OKX", "DERIBIT", "BITGET" };
    return (id < 5) ? names[id] : "???";
}

int main() {
    FeeMatrix global_fees{};
    // maker / taker
    global_fees.rates[0][0] = -0.0001; global_fees.rates[0][1] =  0.0004; // binance
    global_fees.rates[1][0] =  0.0001; global_fees.rates[1][1] =  0.0006; // bybit
    global_fees.rates[2][0] = -0.0002; global_fees.rates[2][1] =  0.0005; // okx
    global_fees.rates[3][0] =  0.0000; global_fees.rates[3][1] =  0.0003; // deribit
    global_fees.rates[4][0] =  0.0002; global_fees.rates[4][1] =  0.0006; // bitget

    alignas(kCacheLineBytes) ExchangeState states[kMaxExchanges];
    populate_mock_exchange(states[0], 0, 65100.0, 0.5, 0.40, 120.0, global_fees);
    populate_mock_exchange(states[1], 1, 65101.0, 0.5, 0.25, 350.0, global_fees);
    populate_mock_exchange(states[2], 2, 65099.0, 0.5, 0.70, 190.0, global_fees);
    populate_mock_exchange(states[3], 3, 65097.5, 0.5, 0.15, 820.0, global_fees);
    populate_mock_exchange(states[4], 4, 65102.0, 0.5, 0.30, 260.0, global_fees);

    RoutingEngine engine(global_fees);

    // TODO: wrap this in an rdtsc loop and print p50/p99 before shipping
    RoutingContext ctx{};
    ctx.states           = states;
    ctx.active_exchanges = kMaxExchanges;
    ctx.dir              = OrderDir::BUY;
    ctx.target_lots      = to_lots(5.0, 0.001);  // 5 BTC
    ctx.limit_price      = 65115.0;
    ctx.decision_ns      = 0;
    ctx.short_vol_factor = 0.3;
    ctx.book_imbalance   = 0.15;

    ChildOrderBuffer out_buf{};
    const SplitResult result = engine.calculate_optimal_split(ctx, out_buf);

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "══════════════════════════════════════════════════════════\n";
    std::cout << "  Smart Order Router — Split Result\n";
    std::cout << "══════════════════════════════════════════════════════════\n";
    std::cout << "  Direction:          BUY\n";
    std::cout << "  Target:             " << from_lots(ctx.target_lots, 0.001)
              << " BTC  (" << ctx.target_lots << " lots)\n";
    std::cout << "  Limit:              " << ctx.limit_price      << " USD\n";
    std::cout << "  Vol factor:         " << ctx.short_vol_factor << "\n";
    std::cout << "  Book imbalance:     " << ctx.book_imbalance   << "\n";
    std::cout << "──────────────────────────────────────────────────────────\n";
    std::cout << "  Status:             " << (result.success ? "FULLY FILLED" : "PARTIAL") << "\n";
    std::cout << "  Filled:             " << result.filled_qty          << " BTC\n";
    std::cout << "  Unfilled:           " << result.unfilled_qty        << " BTC\n";
    std::cout << "  Avg eff. price:     " << result.avg_effective_price << " USD\n";
    std::cout << "  Total eff. cost:    " << result.total_effective_cost << " USD\n";
    std::cout << "──────────────────────────────────────────────────────────\n";
    std::cout << "  PnL\n";
    std::cout << "  ├─ Market impact:   " << result.market_impact_bps    << " bps\n";
    std::cout << "  └─ Adverse sel.:    " << result.adverse_selection_est << " USD\n";
    std::cout << "──────────────────────────────────────────────────────────\n";
    std::cout << "  Child orders (" << result.child_count << ")\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    double cumfill = 0.0;
    for (const ChildOrder& o : out_buf) {
        cumfill += o.qty;
        std::cout << "  [" << std::left << std::setw(7) << exchange_name(o.exchange_id) << "] "
                  << "lvl=" << std::setw(2) << static_cast<int>(o.level_idx) << "  "
                  << "qty=" << std::setw(7) << o.qty << "  "
                  << "px="  << std::setw(10) << o.price << "  "
                  << "ecost=" << std::setw(12) << o.effective_cost << "  "
                  << "cum=" << cumfill << "\n";
    }
    std::cout << "══════════════════════════════════════════════════════════\n";

    return result.success ? 0 : 1;
}
