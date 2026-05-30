#pragma once

#include "oms_types.hpp"
#include "order_pool.hpp"
#include "position_tracker.hpp"
#include "risk_engine.hpp"
#include "spsc_queue.hpp"
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

    void on_strategy_order(const StrategyOrderSignal& sig)  noexcept;
    void on_execution_report(const ExecutionReport& report) noexcept;

    InboundQueue<StrategyOrderSignal>  strategy_queue;
    InboundQueue<ExecutionReport>      exec_report_queue;

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

    OMSOrderPool       pool_{};
    PositionTable      pos_{};
    PreTradeRiskEngine risk_;
    sor::RoutingEngine sor_;
    ExecCoreConfig     cfg_;

    sor::ChildOrderBuffer child_buf_{};
    sor::RoutingContext   routing_ctx_{};

    bool running_{false};
};

} // namespace oms
