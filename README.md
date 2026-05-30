# sor-engine + oms

Smart Order Router with an institutional OMS layer on top.

The SOR was already fast (1–3 µs, zero heap, integer-only hot path). The OMS wraps it without ruining that — same constraints: no heap on the critical path, `alignas(64)` everywhere, SPSC queues between threads, pre-trade risk in ~100 ns.

---

## Files

**SOR**
- `types.hpp` — integer price/qty types, constants, `ChildOrder`, `SplitResult`
- `normalized_book.hpp` — SoA order book, precomputed cumulative qty
- `exchange_state.hpp` — `LatencyTracker` (EWMA + vol/imbalance penalty), `LotConstraints`
- `routing_engine.hpp/cpp` — the SOR: K-way merge → greedy fill

**OMS**
- `oms_types.hpp` — signals, reports, enums, `Result<T>`
- `order_pool.hpp` — `ParentOrder`, `ChildOrderState`, `OMSOrderPool` (flat pre-alloc)
- `position_tracker.hpp` — `InstrumentPosition` (signed net), `MarginState` (async update)
- `risk_engine.hpp` — `PreTradeRiskEngine`: fat-finger, position limit, notional, margin
- `spsc_queue.hpp` — SPSC ring buffer, `GatewayQueue`/`InboundQueue` aliases, `OutboundOrder`
- `execution_core.hpp/cpp` — `ExecutionCore`: main loop, order lifecycle, rerouting

**Examples**
- `main_example.cpp` — standalone SOR demo
- `oms_example.cpp` — full OMS pipeline demo

---

## How the SOR works

Three phases, all on the stack:

1. **Build** — one `Cursor` per exchange. Per-cursor cost = taker fee + dynamic latency penalty (EWMA RTT scaled by vol and book imbalance). Penalty expressed as integer tick offset so the merge loop is pure integer.

2. **Merge** — K-way merge of the K sorted book sides. Linear scan across K=5 cursors per step, not a heap. At K=5 the linear scan wins.

3. **Fill** — greedy sweep over the sorted slices. Monotonic effective prices make greedy provably optimal here. Integer throughout until the fill decision is made, then doubles for reporting.

Result: 11 child orders for a 5 BTC BUY across 5 exchanges, p99 under 2 µs on a Xeon Gold.

---

## How the OMS works

```
StrategyOrderSignal  →  PreTradeRiskEngine  →  OMSOrderPool (alloc parent)
                                     ↓
                            RoutingEngine::calculate_optimal_split
                                     ↓
                          dispatch_child_orders  →  SpscQueue[ex] → gateway threads
                                     ↑
                          on_execution_report  ←  SpscQueue ← gateway threads
                                     ↓
                     handle_fill / handle_cancel_reject
                                     ↓
                    leaves_qty > 0 + all terminal?  →  reroute_leaves → SOR again
```

Single execution thread, busy-spinning. Two inbound SPSC queues (strategy signals, execution reports). One outbound queue per exchange gateway thread.

`OMSOrderPool` is ~2.8 MB (4096 parents + 32768 children). It lives on the heap or in `.bss` — never on the stack. O(1) alloc/free via LIFO free lists. Access by ID is a direct array index.

---

## Build

```bash
git clone https://github.com/tfrmma/sor-engine.git
cd sor-engine

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

./sor_example     # standalone SOR demo
./oms_example     # full OMS pipeline demo
```

Requires C++20 and GCC or Clang. Tested on GCC 13, Ubuntu 24. `-march=native` is on by default in Release.

ASan + UBSan build:
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make oms_example_san -j$(nproc)
./oms_example_san
```

---

## Using it

```cpp
ExecCoreConfig cfg{};
cfg.fees             = fees;
cfg.exchange_states  = states;   // your live book pointers
cfg.active_exchanges = 5;
for (uint32_t i = 0; i < 5; ++i) {
    cfg.gateways[i].outbound    = &your_gateway_queue[i];
    cfg.gateways[i].exchange_id = i;
    cfg.gateways[i].connected   = true;
}
cfg.risk_limits.max_order_lots[BTC_PERP] = to_lots(10.0, 0.001);
cfg.risk_limits.max_net_lots[BTC_PERP]   = to_lots(50.0, 0.001);
cfg.risk_limits.max_notional_usd         = 500'000.0;

// ~2.8 MB — don't put it on the stack
auto core = std::make_unique<ExecutionCore>(cfg);

std::thread exec_thread([&]{ core->run(); });

// strategy pushes signals
StrategyOrderSignal sig{};
sig.instr_id         = BTC_PERP;
sig.dir              = OrderDir::BUY;
sig.qty_lots         = to_lots(5.0, 0.001);
sig.limit_price_usd  = 65115.0;
sig.short_vol_factor = 0.3;
sig.book_imbalance   = 0.15;
core->strategy_queue.push(sig);

// gateway threads push fills
ExecutionReport rep{};
rep.child_id         = child_id_from_exchange_ack;
rep.fill_price_ticks = to_ticks(65100.0, 0.5);
rep.fill_qty_lots    = to_lots(0.374, 0.001);
rep.exec_type        = ExecType::FILL;
core->exec_report_queue.push(rep);
```

`limit_price_usd` is in USD. The OMS converts to ticks internally. If a child gets canceled or rejected, the OMS computes the remaining `leaves_qty`, rebuilds the `RoutingContext`, and re-runs the SOR automatically.

---

## Cost model

Effective price for a level = `raw_price ± (taker_fee + latency_penalty)`.

Latency penalty: `base(RTT) × (1 + 2.0×vol) × (1 + 1.5×imbalance)`

At `vol=1, imbalance=1` that's a 7.5× multiplier. A venue at 820 µs during a CPI print looks a lot more expensive than the same venue at 3am. That's the point.

Fill rate estimate: `exp(-2e-4 × RTT) × (1 - 0.10×vol) × (1 - 0.08×imbalance)`, floored at 50%.

Constants were calibrated against internal fill data. Recalibrate for your exchanges if you have better data.

---

## Known limitations / TODO

- No cancel-on-timeout. Child orders with no ack sit in `PENDING_NEW` indefinitely. Needs a timer wheel keyed by `sent_ns`.
- Pool exhaustion drops the signal silently. Should push a reject back to the strategy queue.
- Lot size hardcoded to 0.001 in the notional check and VWAP. Needs an instrument table.
- Vol/imbalance at reroute time reuse the original signal values. Should re-read from the book.
- Mixed lot sizes across exchanges (e.g. Deribit USD contracts vs BTC contracts) will break the routing context.
- No maker routing. 100% taker.

---

## License

MIT.
