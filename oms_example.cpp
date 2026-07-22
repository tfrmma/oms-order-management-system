// OMS integration demo. Same 5-exchange setup as the SOR example.
// Shows the full path: strategy signal -> risk check -> SOR -> dispatch -> fills.
//
// Single-threaded for demo purposes, in production the queues connect to
// separate strategy, execution, and gateway threads.

#include "execution_core.hpp"

#include <iomanip>
#include <iostream>
#include <memory>
#include <cstdint>
#include <cstdio>

using namespace oms;
using namespace sor;

static void populate_mock_exchange(
    ExchangeState& state, uint8_t ex_id,
    double best_ask, double tick, double qty_per_level,
    double rtt_us, const FeeMatrix& fees
) noexcept {
    state.book.invalidate();
    state.book.last_sequence  = 1;
    state.book.tick_size      = tick;
    state.book.lot_size       = 0.001;
    state.book.is_valid       = true;
    state.book.exchange_id    = ex_id;
    state.exchange_id = ex_id;
    state.enabled     = true;
    state.fees        = fees;
    state.lot         = { 0.001, tick, 0.001, 20.0, 0.0 };
    state.latency.ewma_rtt_us.store(rtt_us, std::memory_order_relaxed);

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
    FeeMatrix fees{};
    fees.rates[0][0] = -0.0001; fees.rates[0][1] =  0.0004;
    fees.rates[1][0] =  0.0001; fees.rates[1][1] =  0.0006;
    fees.rates[2][0] = -0.0002; fees.rates[2][1] =  0.0005;
    fees.rates[3][0] =  0.0000; fees.rates[3][1] =  0.0003;
    fees.rates[4][0] =  0.0002; fees.rates[4][1] =  0.0006;

    // logger first, before anything can go wrong
    g_log.init("oms.log");
    g_log.info("DEMO_START  exchanges=%u", kMaxExchanges);

    alignas(kCacheLineBytes) ExchangeState states[kMaxExchanges]{};
    populate_mock_exchange(states[0], 0, 65100.0, 0.5, 0.40, 120.0, fees);
    populate_mock_exchange(states[1], 1, 65101.0, 0.5, 0.25, 350.0, fees);
    populate_mock_exchange(states[2], 2, 65099.0, 0.5, 0.70, 190.0, fees);
    populate_mock_exchange(states[3], 3, 65097.5, 0.5, 0.15, 820.0, fees);
    populate_mock_exchange(states[4], 4, 65102.0, 0.5, 0.30, 260.0, fees);

    // these are ~83KB together, too big for the stack in some environments
    static GatewayQueue<OutboundOrder> gw_queues[kMaxExchanges];

    ExecCoreConfig cfg{};
    cfg.fees             = fees;
    cfg.exchange_states  = states;
    cfg.active_exchanges = kMaxExchanges;
    for (uint32_t i = 0; i < kMaxExchanges; ++i) {
        cfg.gateways[i].outbound    = &gw_queues[i];
        cfg.gateways[i].exchange_id = static_cast<uint8_t>(i);
        cfg.gateways[i].connected   = true;
    }
    for (uint8_t i = 0; i < kMaxInstruments; ++i) {
        cfg.risk_limits.max_order_lots[i] = to_lots(10.0, 0.001);
        cfg.risk_limits.max_net_lots[i]   = to_lots(50.0, 0.001);
        cfg.risk_limits.lot_size[i]       = 0.001;
    }
    cfg.risk_limits.max_notional_usd    = 400'000.0;
    cfg.risk_limits.min_margin_required = 0.0;

    // notional gate: auto-approve below 50K, hard block above 400K
    cfg.notional_gate.auto_approve_usd = 50'000.0;
    cfg.notional_gate.hard_block_usd   = 400'000.0;

    // dashboard config
    cfg.dashboard.lot_size        = 0.001;
    cfg.dashboard.tick_size       = 0.5;
    cfg.dashboard.instrument_name = "BTC-PERP";
    cfg.dashboard.refresh_ms      = 250;
    cfg.dashboard.instr_id        = 0;

    // ExecutionCore is ~2.6MB (order pool), allocate it properly, not on the stack.
    // In production this lives as a member of the execution thread object.
    auto core_ptr = std::make_unique<ExecutionCore>(cfg);
    ExecutionCore& core = *core_ptr;

    // ── fire a BUY 5 BTC signal ─────────────────────────────────────────
    StrategyOrderSignal sig{};
    sig.instr_id         = 0;
    sig.dir              = OrderDir::BUY;
    sig.qty_lots         = to_lots(5.0, 0.001);
    sig.limit_price_usd  = 65115.0;
    sig.strategy_id      = 1;
    sig.short_vol_factor = 0.3;
    sig.book_imbalance   = 0.15;

    core.on_strategy_order(sig);

    // ── capture all dispatched child orders before simulating fills ──────
    // snapshot: {child_id, exchange_id, qty_lots, price}
    struct ChildSnap { child_id_t cid; uint8_t ex; qty_t qty; price_t price; };
    ChildSnap snaps[kMaxChildOrders]{};
    uint32_t snap_count = 0;

    OutboundOrder out{};
    for (uint32_t ex = 0; ex < kMaxExchanges; ++ex) {
        while (gw_queues[ex].pop(out) && snap_count < kMaxChildOrders) {
            snaps[snap_count++] = { out.child_id, out.exchange_id, out.qty_lots, out.price };
        }
    }

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "══════════════════════════════════════════════════════════\n";
    std::cout << "  OMS: BUY 5 BTC across " << kMaxExchanges << " exchanges\n";
    std::cout << "══════════════════════════════════════════════════════════\n";
    std::cout << "  Risk:     PASSED\n";
    std::cout << "  Children: " << snap_count << "\n";
    std::cout << "──────────────────────────────────────────────────────────\n";

    double cum_qty = 0.0;
    for (uint32_t i = 0; i < snap_count; ++i) {
        const double qty = from_lots(snaps[i].qty, 0.001);
        const double px  = static_cast<double>(snaps[i].price) * 0.5;
        cum_qty += qty;
        std::cout << "  [" << std::left << std::setw(7) << exchange_name(snaps[i].ex) << "] "
                  << "child_id=" << std::setw(4) << snaps[i].cid << "  "
                  << "qty="      << std::setw(7) << qty            << "  "
                  << "px="       << std::setw(10) << px             << "  "
                  << "cum="      << cum_qty << "\n";
    }

    std::cout << "──────────────────────────────────────────────────────────\n";
    std::cout << "  Total dispatched: " << cum_qty << " BTC\n";
    std::cout << "  Pool parents:     " << core.pool().parents_in_use()  << "\n";
    std::cout << "  Pool children:    " << core.pool().children_in_use() << "\n";

    // ── simulate fills for all children ─────────────────────────────────
    std::cout << "══════════════════════════════════════════════════════════\n";
    std::cout << "  Simulating fills\n";
    std::cout << "══════════════════════════════════════════════════════════\n";

    for (uint32_t i = 0; i < snap_count; ++i) {
        ExecutionReport rep{};
        rep.child_id         = snaps[i].cid;
        rep.fill_price_ticks = snaps[i].price;
        rep.fill_qty_lots    = snaps[i].qty;
        rep.exec_type        = ExecType::FILL;
        rep.exchange_id      = snaps[i].ex;

        core.on_execution_report(rep);
        std::cout << "  fill child_id=" << snaps[i].cid
                  << " exchange=" << exchange_name(snaps[i].ex)
                  << " qty=" << from_lots(snaps[i].qty, 0.001) << " BTC\n";
    }

    const auto& pos = core.positions().instruments[0];
    std::cout << "──────────────────────────────────────────────────────────\n";
    std::cout << "  Pool parents after fills: " << core.pool().parents_in_use()  << "\n";
    std::cout << "  BTC net position:         "
              << (static_cast<double>(pos.net_qty_lots.load()) * 0.001) << " BTC\n";
    std::cout << "══════════════════════════════════════════════════════════\n";

    return (core.pool().parents_in_use() == 0) ? 0 : 1;
}
