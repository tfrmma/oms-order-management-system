# Smart Order Router + Order Management System

A C++20 SOR and the institutional OMS wrapped around it. Built as two
independent layers on purpose, not two halves of one blob: the SOR is a
stateless decision function (given a book snapshot and a target size, what do
I send where), the OMS owns everything stateful around it (risk, position,
lifecycle, retries). You can lift `sor/` out and drop it into a much dumber
bot; you cannot lift `oms/` out without the SOR, it's built assuming that
decision function exists underneath it.

Both layers share the same constraints: no heap on the hot path, no
exceptions, `alignas(64)` on anything a network thread and the execution
thread touch concurrently, integer arithmetic until a decision is made and
doubles only show up to report it.

---

## Architecture

```mermaid
flowchart TB
    SIG["StrategyOrderSignal<br/>(strategy thread)"]

    subgraph EXEC["ExecutionCore::run(), single thread, busy-spin"]
        direction TB
        GATE["NotionalConfirmationGate<br/>auto-approve / block / confirm"]
        RISK["PreTradeRiskEngine<br/>fat-finger · position · notional · margin"]
        POOL["OMSOrderPool<br/>alloc ParentOrder"]
        SOR["sor::RoutingEngine<br/>calculate_optimal_split"]
        DISP["dispatch_child_orders<br/>+ TimerWheel.insert (cancel-on-timeout)"]
        REP["on_execution_report"]
        FILL["handle_fill / handle_cancel_reject"]
        RRT["reroute_leaves<br/>(leaves_qty > 0, all children terminal)"]
    end

    subgraph GW["Gateway threads, one per exchange"]
        G0["gateway 0"]
        G1["gateway 1"]
        GN["..."]
    end

    subgraph SIDE["Side threads"]
        FEED["feed handler thread(s)<br/>writes ExchangeState.latency, book, BookSnapshotCache"]
        LOGT["AsyncLogger drain thread"]
        DASH["Dashboard thread<br/>polls pool_ / pos_ every refresh_ms"]
        REST["REST reconciliation thread<br/>MarginMonitor::on_rest_update"]
    end

    SIG -->|"InboundQueue&lt;StrategyOrderSignal&gt;"| GATE --> RISK --> POOL --> SOR --> DISP
    DISP -->|"GatewayQueue&lt;OutboundOrder&gt;, per exchange"| G0
    DISP -->|"GatewayQueue&lt;OutboundOrder&gt;, per exchange"| G1
    G0 -->|"InboundQueue&lt;ExecutionReport&gt;"| REP
    G1 -->|"InboundQueue&lt;ExecutionReport&gt;"| REP
    REP --> FILL --> RRT --> SOR

    FEED -.->|atomics, no lock| SOR
    REST -.->|atomics, no lock| RISK
```

Everything inside the `ExecutionCore` box runs on one thread. Nothing in that
box blocks except the notional gate's stdin prompt, and that's intentional,
see [Design decisions](#design-decisions). Everything outside it talks to the
execution thread through SPSC queues or plain atomics, never a mutex.

---

## Design decisions

The parts that aren't obvious from reading the code top to bottom.

**Two libraries, one boundary.** `sor::RoutingEngine::calculate_optimal_split`
takes a `RoutingContext` and returns a `SplitResult`. No side effects, no
state carried between calls beyond a reusable scratch workspace. `oms::ExecutionCore`
is the opposite: it's all state (pools, positions, margin, timers) and it
calls the SOR the same way it'd call any pure function. This split means the
SOR's correctness can be reasoned about (and tested) with zero knowledge of
order lifecycles, retries, or risk, and the OMS's correctness can be reasoned
about while treating "what does the SOR return" as a black box with a
contract, not an implementation to trust.

**Integer ticks and lots on the hot path.** `price_t`/`qty_t` are `int64_t`.
`remaining_lots == 0` is an exact test. No epsilon, no float drift across a
K-way merge, no "close enough" bugs that only show up after 40,000 orders.
Doubles only appear once a fill decision has already been made, purely for
reporting USD values back out.

**Cache-line alignment + SPSC, not locks.** The network thread updates
`LatencyTracker.ewma_rtt_us` (an atomic) while the execution thread reads it
mid-decision. `alignas(64)` on the structs that get touched from both sides
stops false sharing from turning a lock-free read into a cache-coherency
tax. Every queue between threads is a single-producer single-consumer ring
buffer (`SpscQueue<T, N>`), not a general MPSC queue, because SPSC is the one
lock-free queue shape you can get right without a paper. If you need multiple
strategies feeding one `ExecutionCore`, that's multiple SPSC queues drained
in sequence, not one shared queue, the moment you need MPSC here the whole
lock-free argument needs revisiting.

**No heap, no exceptions, no RTTI on the hot path.** `-fno-exceptions
-fno-rtti` is a project-wide compile flag, not just a hot-path one, so there's
one build config to reason about instead of two. It forces the `Result<T>`
pattern (`{value, ok}`, checked explicitly) everywhere an exchange SDK would
normally throw. The tradeoff shows up in the test suite: Catch2 auto-detects
`-fno-exceptions` and switches `REQUIRE` from "unwind to the next test case"
to "abort the whole binary", which is a real cost, see [Testing](#testing).

**Flat pre-allocated pools, LIFO free lists.** `OMSOrderPool` is ~2.8 MB,
sized for 4096 parents and 32768 children at compile time, allocated once,
never resized. `alloc_parent()`/`alloc_child()` are an array index and a
pointer decrement, `free_*()` is a push back onto the free list. No allocator
jitter, no fragmentation, and the failure mode when you run out is a checked
`Result<T>::fail()`, not an `std::bad_alloc` from three call frames away
during a fill.

**Single-threaded execution core.** `ExecutionCore::run()` busy-spins on one
thread: pop a signal, pop a report, tick the timer wheel, repeat. No lock ever
sits between "decide to reroute" and "actually reroute", because there's only
one thread that could contend for it. Scaling past what one core can do means
sharding by instrument across multiple `ExecutionCore` instances, not adding
threads inside this one.

**Margin is two numbers, and the risk engine trusts whichever is worse.**
`MarginMonitor` keeps a local, zero-latency estimate (updated inline on every
fill) alongside the exchange's REST-reported ground truth (100ms-2s stale).
`PreTradeRiskEngine` always reads the conservative one. A fast, slightly wrong
local number that leans towards blocking trades is a much safer default than
a slow, correct one that might arrive after you've already blown through your
margin.

**Lot sizes are canonicalized per routing decision, not globally.**
`RoutingContext::reference_lot_size` declares what "1 lot" means for one
specific `calculate_optimal_split` call. Every active exchange's book gets
converted into that unit before the merge/fill math runs, so mixing a linear
BTC book (0.001 BTC/lot) with an inverse USD contract book (1 USD/lot) in the
same call produces a correct split instead of silently treating "10 lots" as
the same real-world quantity on both venues. Leave it at 0 and it falls back
to the first active exchange's native lot size, which is exactly the old
(single-lot-size-only) behavior, so nothing breaks if you don't need this.

---

## Repository layout

```
oms-order-management-system/
├── sor/                      # pure decision function, no lifecycle state
│   ├── types.hpp             # price_t/qty_t, ChildOrder, CostSlice, SplitResult
│   ├── normalized_book.hpp   # SoA order book, precomputed cumulative qty
│   ├── exchange_state.hpp    # LatencyTracker, LotConstraints, RoutingContext
│   └── routing_engine.hpp/cpp
│
├── oms/                      # everything stateful: lifecycle, risk, position
│   ├── oms_types.hpp         # StrategyOrderSignal, ExecutionReport, Result<T>
│   ├── order_pool.hpp        # ParentOrder, ChildOrderState, OMSOrderPool
│   ├── position_tracker.hpp  # InstrumentPosition, MarginState
│   ├── risk_engine.hpp       # PreTradeRiskEngine
│   ├── notional_gate.hpp     # NotionalConfirmationGate
│   ├── margin_monitor.hpp    # MarginMonitor, two-layer margin model
│   ├── spsc_queue.hpp        # SpscQueue, GatewayQueue/InboundQueue aliases
│   ├── timer_wheel.hpp       # TimerWheel, cancel-on-timeout
│   ├── book_snapshot.hpp     # BookSnapshotCache, feed-handler-written vol/imb
│   ├── logger.hpp            # AsyncLogger, lock-free ring + drain thread
│   ├── dashboard.hpp         # Dashboard, live terminal snapshot thread
│   └── execution_core.hpp/cpp # ExecutionCore, ties all of the above together
│
├── tests/
│   ├── test_oms.cpp          # Catch2 suite, SOR + OMS component tests
│   └── catch_amalgamated.*   # vendored Catch2, see tests/LICENSE.txt
│
├── main_example.cpp          # standalone SOR demo, no OMS involved
├── oms_example.cpp           # full pipeline demo, 5 exchanges, one order
└── CMakeLists.txt
```

### Component responsibilities

| Component | Owns | Explicitly does not |
|---|---|---|
| `sor::RoutingEngine` | Splitting one target size across N exchange books at one point in time | Remember anything about a previous call, know what an "order" is beyond a size and a direction |
| `oms::PreTradeRiskEngine` | Fat-finger, position limit, notional, margin checks, ~100ns budget | Know about exchanges, books, or the SOR at all |
| `oms::NotionalConfirmationGate` | The one deliberately blocking call in the pipeline | Run on the execution thread's hot path for small orders (auto-approves below threshold) |
| `oms::OMSOrderPool` | Parent/child order storage, O(1) alloc/free | Allocate anything after startup |
| `oms::MarginMonitor` | Reconciling a fast local estimate against slow REST ground truth | Trust the fast estimate when it disagrees optimistically with REST |
| `oms::TimerWheel` | Cancel-on-timeout for dispatched children | Retry logic, that's `reroute_leaves`'s job once a cancel report comes back |
| `oms::ExecutionCore` | Wiring all of the above into one order lifecycle | Run on more than one thread |

---

## How the SOR works

Three phases, all on the stack, `RoutingEngine::calculate_optimal_split`:

1. **Build** (`build_cost_slices`): one `Cursor` per active exchange. Per-cursor
   cost = taker fee + dynamic latency penalty (EWMA RTT scaled by
   short-term vol and book imbalance), expressed as an integer tick offset so
   the merge loop never touches the FPU. Native exchange lot sizes get
   converted into the call's canonical unit here too, see
   [reference_lot_size](#design-decisions).

2. **Merge**: K-way merge of up to `kMaxExchanges` (5) sorted book sides.
   Linear scan across the cursors to find the minimum effective price per
   step, not a heap, `std::priority_queue` loses to a linear scan at K=5.

3. **Fill** (`greedy_fill`): greedy sweep over the merged, cost-sorted slices.
   Monotonic effective prices make greedy provably optimal here. Integer the
   whole way through, `remaining_lots -= fill_lots` with no rounding, doubles
   only appear once writing `ChildOrder`/`SplitResult` for reporting.

Depth is bounded at `kMaxDepth` (20) levels per exchange side,
`kMaxChildOrders = kMaxExchanges × kMaxDepth = 100` child orders is the hard
ceiling for one split.

---

## How the OMS works

`ExecutionCore::on_strategy_order`, one signal end to end:

1. `NotionalConfirmationGate::approve`, auto-approve, hard-block, or a
   blocking stdin confirmation depending on USD notional.
2. `pool_.alloc_parent()`, fails cleanly and gets counted
   (`parent_alloc_failures()`) instead of corrupting state if the pool's full.
3. `PreTradeRiskEngine::validate`, fat-finger, then position limit, then margin
   (which itself can reject for `NOTIONAL` or `MARGIN`, propagated as the real
   reason, not collapsed into one).
4. `build_routing_context` + `sor_.calculate_optimal_split`, the SOR runs
   exactly once per signal here, with no memory of anything before it.
5. `dispatch_child_orders`, one `alloc_child()` + `timer_wheel_.insert()`
   per child, pushed onto that exchange's `GatewayQueue<OutboundOrder>`.
6. Fills and cancels arrive on `exec_report_queue`, `on_execution_report`
   routes to `handle_fill` or `handle_cancel_reject`.
7. If a parent still has `leaves_qty_lots > 0` once all its current children
   are terminal, `reroute_leaves` rebuilds the `RoutingContext` from the
   current book state and calls the SOR again for the remainder.

The timer wheel fires independently of all of this: if a child sits in
`PENDING_NEW`/`PARTIALLY_FILLED` past `child_timeout_ns`, the wheel's callback
synthesizes a `CANCELED` execution report and routes it through the exact
same `on_execution_report` path a real exchange cancel would take, no special
casing for timeouts anywhere downstream of that.

---

## Threading model

| Thread | Reads | Writes | Sync |
|---|---|---|---|
| Execution (`ExecutionCore::run`) | `strategy_queue`, `exec_report_queue`, all `ExchangeState`, `BookSnapshotCache` | `pool_`, `pos_`, `routing_ctx_`, `timer_wheel_` | SPSC pop, relaxed/acquire loads on shared atomics |
| Feed handler(s) | market data | `ExchangeState.latency`, `.book`, `BookSnapshotCache` | atomic stores, release |
| Gateway (per exchange) | `GatewayQueue<OutboundOrder>` | exchange socket, `exec_report_queue` | SPSC pop/push |
| REST reconciliation | exchange REST API | `MarginMonitor` (`on_rest_update`) | atomic stores |
| Logger drain | `AsyncLogger`'s ring buffer | stdout/file | SPSC-style ring, lock-free |
| Dashboard | `pool_`, `pos_` (const refs) | stderr | none, read-only polling |

Nothing here takes a lock. The only place the pipeline can block is the
notional gate's stdin confirmation, and that's a deliberate, named exception,
not an oversight.

---

## Build

```bash
git clone https://github.com/tfrmma/oms-order-management-system.git
cd oms-order-management-system

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

## Testing

Catch2 (vendored amalgamated in `tests/`, see `tests/LICENSE.txt`):
```bash
make oms_tests -j$(nproc)
./oms_tests
```

Covers the SOR merge/fill algorithm (single and multi-exchange, partial
fills, limit price clipping, BUY/SELL sign handling, mismatched lot sizes
across exchanges), `OMSOrderPool` alloc/free and failure counting,
`InstrumentPosition` avg price tracking, `PreTradeRiskEngine` rejection
paths, `SpscQueue`, `TimerWheel`, and two `ExecutionCore` integration tests
that drive a real pool to exhaustion through `on_strategy_order` (the public
API, not a mock) to prove the dispatch-gap margin/reroute fix above actually
holds end to end.

Built with `-fno-exceptions` like the rest of the project. Catch2
auto-detects this and switches `REQUIRE` to abort the whole binary on
failure instead of unwinding to just the failing test case, there's no
exception to unwind with. Fine for CI (fail fast, investigate immediately),
but it means one regression stops the run before the rest of the suite gets
a chance to report, unlike a normal Catch2 build where you'd see every
failure in one pass. `CHECK` doesn't have this problem if you need to see
multiple failures in a single run.

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
cfg.risk_limits.lot_size[BTC_PERP]       = 0.001;
cfg.risk_limits.max_notional_usd         = 500'000.0;

// ~2.8 MB, don't put it on the stack
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

`limit_price_usd` is in USD, the OMS converts to ticks internally. If a child
gets canceled or rejected, the OMS computes the remaining `leaves_qty`,
rebuilds the `RoutingContext` from current book state, and re-runs the SOR
automatically, you never call the SOR directly once `ExecutionCore` owns the
order.

---

## Cost model

Effective price for a level = `raw_price ± (taker_fee + latency_penalty)`.

Latency penalty: `base(RTT) × (1 + 2.0×vol) × (1 + 1.5×imbalance)`

At `vol=1, imbalance=1` that's a 7.5× multiplier. A venue at 820 µs during a CPI print looks a lot more expensive than the same venue at 3am. That's the point.

Fill rate estimate: `exp(-2e-4 × RTT) × (1 - 0.10×vol) × (1 - 0.08×imbalance)`, floored at 50%.

Constants were calibrated against internal fill data. Recalibrate for your exchanges if you have better data.

---

## Known limitations / TODO

- **Fixed**, was previously the top item here: `alloc_child()` running out
  mid-dispatch used to leave `parent.leaves_qty_lots` and the margin
  `reserve_open()` reserved upfront out of sync with what actually made it to
  an exchange, permanently, since nothing ever triggered a reroute or freed
  the parent. `dispatch_child_orders` now releases the margin for whatever
  didn't get dispatched, and `handle_fill` now checks `all_children_terminal`
  the same way `handle_cancel_reject` already did, so a parent whose last
  live child gets *filled* (not canceled) still gets rerouted for any
  remaining gap instead of sitting there forever. Found the same "parked
  forever, never freed" shape in `reroute_leaves`'s `REROUTE_NO_LIQUIDITY`
  path while fixing this and closed that too. Covered by two
  `ExecutionCore`-level tests in `test_oms.cpp` that drive the real pool to
  exhaustion through the public API, not mocks.
- No channel back to the strategy layer to notify it a signal was rejected.
  Risk-reject, the notional gate's hard-block, and pool exhaustion (counted
  via `OMSOrderPool::parent_alloc_failures()` / `child_alloc_failures()` and
  logged, but not surfaced anywhere else) all just drop the signal today.
  Needs a new outbound queue and a reject-reason enum, then wiring three
  existing call sites through it, not a small change.
- Lot size is configured in three independent places that all have to agree
  by convention, not by construction: `RiskLimits.lot_size[instr]`,
  `NotionalConfirmationGate`'s constructor param, and `RoutingContext`'s
  fallback to `states[0].book.lot_size`. Works today because the demos keep
  them in sync by hand. A real instrument registry that all three read from
  would remove the "by convention" part.
- No maker routing. 100% taker.
- `oms_example_san` duplicates sources instead of linking `sor_lib`/`oms_lib`, easy to forget to update if a new file gets added to either.

---

## License

MIT.
