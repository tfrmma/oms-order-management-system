#pragma once

#include "oms_types.hpp"
#include "order_pool.hpp"
#include "position_tracker.hpp"
#include "risk_engine.hpp"
#include "spsc_queue.hpp"
#include "logger.hpp"
#include "notional_gate.hpp"
#include "dashboard.hpp"
#include "timer_wheel.hpp"
#include "book_snapshot.hpp"
#include "margin_monitor.hpp"
#include "sor/routing_engine.hpp"
#include "sor/exchange_state.hpp"

namespace oms {

struct GatewayHandle {
    GatewayQueue<OutboundOrder>* outbound{nullptr};
    uint8_t exchange_id{0xFF};
    bool    connected{false};
};

struct ExecCoreConfig {
    sor::FeeMatrix       fees;
    RiskLimits           risk_limits;
    InstrumentTable      instruments{};
    NotionalGateConfig   notional_gate{};
    DashboardConfig      dashboard{};
    MarginConfig         margin{};
    uint64_t             child_timeout_ns{2'000'000'000ULL};  // 2s default, taker child ack/fill timeout
    uint64_t             maker_timeout_ns{5'000'000'000ULL};  // 5s default, how long a posted order waits before the unfilled remainder gets swept as taker
    sor::ExchangeState*  exchange_states{nullptr};
    uint32_t             active_exchanges{0};
    GatewayHandle        gateways[sor::kMaxExchanges]{};
};

class ExecutionCore {
public:
    explicit ExecutionCore(const ExecCoreConfig& cfg) noexcept;

    // single-threaded busy-spin loop. call from the execution thread.
    void run() noexcept;
    void stop() noexcept { running_ = false; }

    // one iteration of what run()'s loop does: drain both inbound queues,
    // advance the timer wheel, check margin warnings. public so tests (and
    // any caller that wants its own scheduling instead of a dedicated
    // busy-spin thread) can drive time forward deterministically without
    // actually sleeping past child_timeout_ns/maker_timeout_ns.
    void tick(uint64_t now) noexcept;

    void on_strategy_order(const StrategyOrderSignal& sig)  noexcept;
    void on_execution_report(const ExecutionReport& report) noexcept;

    // ops-controlled kill switch, safe from any thread (single atomic store).
    // forces reduce-only for this instrument regardless of what any signal's
    // own reduce_only flag says, see PreTradeRiskEngine::clamp_reduce_only.
    void set_unwind_mode(instr_id_t instr_id, bool enabled) noexcept {
        if (instr_id < kMaxInstruments)
            pos_.instruments[instr_id].unwind_only.store(enabled, std::memory_order_release);
    }

    InboundQueue<StrategyOrderSignal>  strategy_queue;
    InboundQueue<ExecutionReport>      exec_report_queue;
    OutboundQueue<OrderReject>         reject_queue;  // strategy thread drains this

    // feed handler / REST reconciliation threads call these
    void on_mark_price(double mark_usd, double lot_size, instr_id_t instr_id) noexcept {
        margin_monitor_.on_mark_price(mark_usd, lot_size, instr_id);
    }
    void on_rest_margin(double equity, double used, uint64_t ts_ns) noexcept {
        margin_monitor_.on_rest_update(equity, used, ts_ns);
    }
    void on_book_snapshot(uint8_t exchange_id, double vol, double imb,
                          double mid, uint64_t ts_ns) noexcept {
        if (exchange_id < sor::kMaxExchanges)
            book_cache_.exchanges[exchange_id].update(vol, imb, mid, ts_ns);
    }

    [[nodiscard]] const PositionTable& positions() const noexcept { return pos_; }
    [[nodiscard]] const OMSOrderPool&  pool()      const noexcept { return pool_; }

private:
    void build_routing_context(const ParentOrder& parent,
                               qty_t              target_lots,
                               sor::RoutingContext& ctx) const noexcept;

    void dispatch_child_orders(ParentOrder&                 parent,
                               const sor::ChildOrderBuffer& buf,
                               const sor::RoutingContext&   ctx) noexcept;

    void handle_fill(ChildOrderState& child, ParentOrder& parent,
                     const ExecutionReport& rep) noexcept;

    void handle_cancel_reject(ChildOrderState& child, ParentOrder& parent,
                              const ExecutionReport& rep) noexcept;

    void reroute_leaves(ParentOrder& parent) noexcept;

    [[nodiscard]] bool all_children_terminal(const ParentOrder& parent) const noexcept;

    // a child's fill/leaves qty is in ITS exchange's native lot_size, but
    // everything at the parent/position level (cum_qty_lots, net_qty_lots,
    // open_*_lots) is tracked in the instrument's canonical unit. convert
    // before touching any of those, or fills from exchanges with different
    // native lot sizes silently corrupt each other's accounting.
    [[nodiscard]] qty_t to_canonical_lots(const ChildOrderState& child,
                                          instr_id_t             instr_id,
                                          qty_t                  native_lots) const noexcept;

    void notify_reject(const StrategyOrderSignal& sig, RejectReason reason) noexcept;
    void notify_reject(instr_id_t instr_id, sor::OrderDir dir, uint8_t strategy_id,
                       qty_t qty_lots, RejectReason reason) noexcept;

    OMSOrderPool            pool_{};
    PositionTable           pos_{};
    PreTradeRiskEngine      risk_;
    sor::RoutingEngine      sor_;
    NotionalConfirmationGate notional_gate_;
    Dashboard               dashboard_;
    TimerWheel              timer_wheel_;
    BookSnapshotCache       book_cache_{};
    MarginMonitor           margin_monitor_;
    ExecCoreConfig          cfg_;

    sor::ChildOrderBuffer   child_buf_{};
    sor::RoutingContext     routing_ctx_{};

    bool running_{false};
};

} // namespace oms
