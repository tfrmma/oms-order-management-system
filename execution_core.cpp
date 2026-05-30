#include "execution_core.hpp"

#include <cassert>
#include <cstring>

static inline uint64_t now_ns() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

namespace oms {

ExecutionCore::ExecutionCore(const ExecCoreConfig& cfg) noexcept
    : risk_(cfg.risk_limits, pos_)
    , sor_(cfg.fees)
    , cfg_(cfg)
{}

void ExecutionCore::run() noexcept {
    running_ = true;
    StrategyOrderSignal sig{};
    ExecutionReport     rep{};

    while (__builtin_expect(running_, 1)) {
        while (strategy_queue.pop(sig))
            on_strategy_order(sig);
        while (exec_report_queue.pop(rep))
            on_execution_report(rep);
        // TODO: timer wheel for cancel-on-timeout (acks that never arrive)
        __builtin_ia32_pause();
    }
}

void ExecutionCore::on_strategy_order(const StrategyOrderSignal& sig) noexcept {
    const auto id_res = pool_.alloc_parent();
    if (!id_res.ok) [[unlikely]] {
        // pool exhausted. shouldn't happen — TODO: push reject back to strategy
        return;
    }

    ParentOrder& parent = pool_.parent(id_res.value);
    parent.instr_id        = sig.instr_id;
    parent.dir             = sig.dir;
    parent.total_qty_lots  = sig.qty_lots;
    parent.leaves_qty_lots = sig.qty_lots;
    parent.strategy_id     = sig.strategy_id;
    parent.create_ns       = sig.signal_ns ? sig.signal_ns : now_ns();
    parent.state           = OrderState::NEW;

    // all exchanges share the same tick size in this universe. if that changes,
    // this needs to be per-instrument. not today's problem.
    const double tick_sz = cfg_.exchange_states[0].book.tick_size;
    parent.limit_price   = (sig.limit_price_usd > 0.0)
        ? sor::to_ticks(sig.limit_price_usd, tick_sz)
        : price_t(0);

    if (risk_.validate(sig) != RiskRejectReason::OK) [[unlikely]] {
        parent.state = OrderState::REJECTED;
        pool_.free_parent(id_res.value);
        return;
    }

    build_routing_context(parent, sig.qty_lots, routing_ctx_);
    routing_ctx_.short_vol_factor = sig.short_vol_factor;
    routing_ctx_.book_imbalance   = sig.book_imbalance;
    routing_ctx_.decision_ns      = now_ns();

    child_buf_.reset();
    const sor::SplitResult split = sor_.calculate_optimal_split(routing_ctx_, child_buf_);

    if (child_buf_.count == 0) [[unlikely]] {
        parent.state = OrderState::REJECTED;
        pool_.free_parent(id_res.value);
        return;
    }

    parent.state = OrderState::PENDING_NEW;
    pos_.instruments[sig.instr_id].reserve_open(sig.dir, sig.qty_lots);
    dispatch_child_orders(parent, child_buf_, routing_ctx_);
    (void)split;
}

void ExecutionCore::build_routing_context(const ParentOrder&    parent,
                                          qty_t                 target_lots,
                                          sor::RoutingContext&  ctx) const noexcept {
    ctx.states           = cfg_.exchange_states;
    ctx.active_exchanges = cfg_.active_exchanges;
    ctx.dir              = parent.dir;
    ctx.target_lots      = target_lots;
    ctx.limit_price      = (parent.limit_price > 0)
        ? static_cast<double>(parent.limit_price) * cfg_.exchange_states[0].book.tick_size
        : 0.0;
    ctx.short_vol_factor = 0.0;
    ctx.book_imbalance   = 0.0;
}

void ExecutionCore::dispatch_child_orders(ParentOrder&                 parent,
                                          const sor::ChildOrderBuffer& buf,
                                          const sor::RoutingContext&   ctx) noexcept {
    const uint64_t ts = now_ns();

    for (uint32_t i = 0; i < buf.count; ++i) {
        if (parent.child_count >= ParentOrder::kMaxChildrenPerParent) [[unlikely]]
            break;

        const sor::ChildOrder& co = buf.orders[i];
        const auto cid_res = pool_.alloc_child();
        if (!cid_res.ok) [[unlikely]] break;

        const double tick_sz = cfg_.exchange_states[co.exchange_id].book.tick_size;
        const double lot_sz  = cfg_.exchange_states[co.exchange_id].book.lot_size;

        ChildOrderState& child  = pool_.child(cid_res.value);
        child.parent_id         = parent.parent_id;
        child.price             = (co.price > 0.0) ? sor::to_ticks(co.price, tick_sz) : price_t(0);
        child.qty_lots          = sor::to_lots(co.qty, lot_sz);
        child.leaves_qty_lots   = child.qty_lots;
        child.cum_qty_lots      = 0;
        child.exchange_id       = co.exchange_id;
        child.level_idx         = co.level_idx;
        child.order_type        = co.type;
        child.dir               = ctx.dir;
        child.state             = OrderState::PENDING_NEW;
        child.sent_ns           = ts;

        parent.add_child(static_cast<uint8_t>(i), cid_res.value);

        OutboundOrder out{};
        out.child_id    = cid_res.value;
        out.price       = child.price;
        out.qty_lots    = child.qty_lots;
        out.dir         = ctx.dir;
        out.order_type  = co.type;
        out.exchange_id = co.exchange_id;
        out.instr_id    = parent.instr_id;

        const uint8_t ex = co.exchange_id;
        if (ex < sor::kMaxExchanges && cfg_.gateways[ex].outbound) [[likely]]
            (void)cfg_.gateways[ex].outbound->push(out);
        // push failure = queue full = order lost. cancel-on-timeout will catch it.
        // TODO: handle this properly
    }
}

void ExecutionCore::on_execution_report(const ExecutionReport& rep) noexcept {
    if (rep.child_id >= kMaxTotalChildren) [[unlikely]] return;

    ChildOrderState& child = pool_.child(rep.child_id);
    if (child.parent_id >= kMaxParentOrders) [[unlikely]] return;

    ParentOrder& parent = pool_.parent(child.parent_id);

    switch (rep.exec_type) {
        case ExecType::PARTIAL:
        case ExecType::FILL:     handle_fill(child, parent, rep);          break;
        case ExecType::CANCELED:
        case ExecType::REJECTED: handle_cancel_reject(child, parent, rep); break;
        case ExecType::NEW:      child.state = OrderState::PENDING_NEW;    break;
        default:                                                            break;
    }
}

void ExecutionCore::handle_fill(ChildOrderState&       child,
                                ParentOrder&           parent,
                                const ExecutionReport& rep) noexcept {
    const double tick_sz  = cfg_.exchange_states[child.exchange_id].book.tick_size;
    const double lot_sz   = cfg_.exchange_states[child.exchange_id].book.lot_size;
    const double fill_px  = static_cast<double>(rep.fill_price_ticks) * tick_sz;

    child.cum_qty_lots   += rep.fill_qty_lots;
    child.leaves_qty_lots = child.qty_lots - child.cum_qty_lots;
    child.state = (child.leaves_qty_lots == 0)
        ? OrderState::FILLED : OrderState::PARTIALLY_FILLED;

    parent.cum_qty_lots   += rep.fill_qty_lots;
    parent.leaves_qty_lots = parent.total_qty_lots - parent.cum_qty_lots;
    parent.last_update_ns  = rep.recv_ns;

    // running VWAP
    const double prev_cum  = static_cast<double>(parent.cum_qty_lots - rep.fill_qty_lots) * lot_sz;
    const double fill_qty  = static_cast<double>(rep.fill_qty_lots) * lot_sz;
    const double total_cum = static_cast<double>(parent.cum_qty_lots) * lot_sz;
    parent.avg_fill_price  = (prev_cum > 0.0)
        ? (parent.avg_fill_price * prev_cum + fill_px * fill_qty) / total_cum
        : fill_px;

    pos_.instruments[parent.instr_id].apply_fill(parent.dir, rep.fill_qty_lots, fill_px);
    pos_.instruments[parent.instr_id].release_open(parent.dir, rep.fill_qty_lots);

    if (parent.leaves_qty_lots == 0) {
        parent.state = OrderState::FILLED;
        for (uint8_t s = 0; s < ParentOrder::kMaxChildrenPerParent; ++s)
            if (parent.has_child(s)) pool_.free_child(parent.children[s]);
        pool_.free_parent(parent.parent_id);
    } else {
        parent.state = OrderState::PARTIALLY_FILLED;
    }
}

void ExecutionCore::handle_cancel_reject(ChildOrderState&       child,
                                         ParentOrder&           parent,
                                         const ExecutionReport& rep) noexcept {
    const qty_t lost_lots  = child.leaves_qty_lots;
    child.leaves_qty_lots  = 0;
    child.state = (rep.exec_type == ExecType::CANCELED)
        ? OrderState::CANCELED : OrderState::REJECTED;

    pos_.instruments[parent.instr_id].release_open(parent.dir, lost_lots);
    parent.last_update_ns = rep.recv_ns;

    if (parent.leaves_qty_lots > 0) {
        if (!all_children_terminal(parent)) return;
        reroute_leaves(parent);
    } else {
        parent.state = OrderState::FILLED;
        for (uint8_t s = 0; s < ParentOrder::kMaxChildrenPerParent; ++s)
            if (parent.has_child(s)) pool_.free_child(parent.children[s]);
        pool_.free_parent(parent.parent_id);
    }
}

void ExecutionCore::reroute_leaves(ParentOrder& parent) noexcept {
    build_routing_context(parent, parent.leaves_qty_lots, routing_ctx_);
    // vol/imbalance are stale by a few ms here. good enough for a reroute.
    // TODO: pull fresh values from the book at reroute time
    routing_ctx_.decision_ns = now_ns();

    for (uint8_t s = 0; s < ParentOrder::kMaxChildrenPerParent; ++s) {
        if (!parent.has_child(s)) continue;
        const ChildOrderState& c = pool_.child(parent.children[s]);
        if (c.state == OrderState::CANCELED  ||
            c.state == OrderState::REJECTED  ||
            c.state == OrderState::FILLED)
            pool_.free_child(parent.children[s]);
    }
    parent.child_count = 0;
    parent.child_mask  = 0;

    child_buf_.reset();
    sor_.calculate_optimal_split(routing_ctx_, child_buf_);

    if (child_buf_.count == 0) [[unlikely]] {
        parent.state = OrderState::PARTIALLY_FILLED;
        return;
    }

    parent.state = OrderState::PENDING_NEW;
    dispatch_child_orders(parent, child_buf_, routing_ctx_);
}

bool ExecutionCore::all_children_terminal(const ParentOrder& parent) const noexcept {
    for (uint8_t s = 0; s < ParentOrder::kMaxChildrenPerParent; ++s) {
        if (!parent.has_child(s)) continue;
        const OrderState cs = pool_.child(parent.children[s]).state;
        if (cs == OrderState::PENDING_NEW || cs == OrderState::PARTIALLY_FILLED)
            return false;
    }
    return true;
}

} // namespace oms
