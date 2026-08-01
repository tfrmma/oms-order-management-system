#include "execution_core.hpp"

#include <cassert>
#include <cmath>
#include <cstring>

static inline uint64_t now_ns() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

namespace oms {

ExecutionCore::ExecutionCore(const ExecCoreConfig& cfg) noexcept
    : risk_(cfg.risk_limits, cfg.instruments, pos_)
    , sor_(cfg.fees)
    , notional_gate_(cfg.notional_gate, cfg.instruments)
    , dashboard_(pool_, pos_, cfg.dashboard)
    , timer_wheel_(1'000'000ULL)   // 1ms tick resolution
    , margin_monitor_(cfg.margin, pos_)
    , cfg_(cfg)
{
    timer_wheel_.set_callback([this](child_id_t cid) {
        // fired from the execution thread via tick(), safe to touch pool directly
        if (cid >= kMaxTotalChildren) return;
        ChildOrderState& child = pool_.child(cid);
        if (child.state != OrderState::PENDING_NEW &&
            child.state != OrderState::PARTIALLY_FILLED) return;

        g_log.error("CHILD_TIMEOUT  child_id=%u  exchange=%u  sent_ns=%lu",
                    cid, (unsigned)child.exchange_id, (unsigned long)child.sent_ns);

        // synthesise a cancel report and process it through the normal path
        ExecutionReport rep{};
        rep.child_id    = cid;
        rep.exec_type   = ExecType::CANCELED;
        rep.exchange_id = child.exchange_id;
        rep.recv_ns     = child.sent_ns; // best we have
        on_execution_report(rep);
    });

    // TimerWheel's internal clock defaults to 0. tick(now) advances it
    // tick_ns_ (1ms) at a time until it reaches now, so without this, the
    // first ever tick() call would try to walk from 0 up to the real epoch,
    // ~1.7e18 ns away, roughly 1.7e12 iterations. that's not a slow start,
    // it's an effectively permanent hang the moment run() is ever actually
    // called for real. reset_clock() already existed for exactly this,
    // nothing was calling it.
    timer_wheel_.reset_clock(now_ns());

    dashboard_.start();
    g_log.info("EXEC_CORE  INIT  exchanges=%u  child_timeout_ns=%lu",
               cfg_.active_exchanges, (unsigned long)cfg_.child_timeout_ns);
}

void ExecutionCore::run() noexcept {
    running_ = true;
    while (__builtin_expect(running_, 1)) {
        tick(now_ns());
        __builtin_ia32_pause();
    }
}

void ExecutionCore::tick(uint64_t now) noexcept {
    StrategyOrderSignal sig{};
    ExecutionReport     rep{};

    while (strategy_queue.pop(sig))
        on_strategy_order(sig);
    while (exec_report_queue.pop(rep))
        on_execution_report(rep);

    timer_wheel_.tick(now);

    // check margin warnings every ~1s (cheap, just reads atomics)
    if ((now & 0x3FFF'FFFF) == 0) [[unlikely]]
        margin_monitor_.check_warnings(now);
}

void ExecutionCore::on_strategy_order(const StrategyOrderSignal& sig) noexcept {
    g_log.debug("ORDER_NEW  instr=%u  dir=%s  qty=%ld  limit_usd=%.2f  vol=%.3f  imb=%.3f",
        (unsigned)sig.instr_id,
        (sig.dir == sor::OrderDir::BUY) ? "BUY" : "SELL",
        (long)sig.qty_lots,
        sig.limit_price_usd,
        sig.short_vol_factor,
        sig.book_imbalance);

    if (!notional_gate_.approve(sig)) {
        g_log.warn("ORDER_BLOCKED_NOTIONAL_GATE  instr=%u  qty=%ld", (unsigned)sig.instr_id, (long)sig.qty_lots);
        notify_reject(sig, RejectReason::NOTIONAL_GATE_BLOCK);
        return;
    }

    const auto id_res = pool_.alloc_parent();
    if (!id_res.ok) [[unlikely]] {
        g_log.error("POOL_EXHAUSTED  parents_in_use=%u  total_failures=%lu  SIGNAL_DROPPED",
            pool_.parents_in_use(), (unsigned long)pool_.parent_alloc_failures());
        notify_reject(sig, RejectReason::PARENT_POOL_EXHAUSTED);
        return;
    }

    ParentOrder& parent = pool_.parent(id_res.value);
    parent.instr_id         = sig.instr_id;
    parent.dir               = sig.dir;
    parent.order_type        = sig.order_type;
    parent.strategy_id      = sig.strategy_id;
    parent.create_ns        = sig.signal_ns ? sig.signal_ns : now_ns();
    parent.state            = OrderState::NEW;

    // reduce-only clamp, before anything else touches qty_lots: fat-finger
    // and notional checks below need to see the EFFECTIVE size, not the
    // originally-requested one, or a legitimately-clamped-down order could
    // fail a limit check it would have passed at its real size.
    //
    // instr_id bounds guarded here directly: risk_.validate()'s own DISABLED
    // check runs AFTER this block, pos_.instruments[] can't wait for it.
    const bool instr_ok = sig.instr_id < kMaxInstruments;
    const qty_t effective_qty = risk_.clamp_reduce_only(sig);
    parent.reduce_only = instr_ok && (sig.reduce_only ||
        pos_.instruments[sig.instr_id].unwind_only.load(std::memory_order_acquire));

    if (instr_ok && effective_qty == 0) [[unlikely]] {
        g_log.error("REDUCE_ONLY_VIOLATION  parent_id=%u  instr=%u  dir=%s  requested=%ld  net=%ld",
            id_res.value, (unsigned)sig.instr_id,
            (sig.dir == sor::OrderDir::BUY) ? "BUY" : "SELL",
            (long)sig.qty_lots,
            (long)pos_.instruments[sig.instr_id].net_qty_lots.load(std::memory_order_acquire));
        parent.state = OrderState::REJECTED;
        pool_.free_parent(id_res.value);
        notify_reject(sig, RejectReason::REDUCE_ONLY_VIOLATION);
        return;
    }
    if (instr_ok && effective_qty != sig.qty_lots) [[unlikely]] {
        g_log.warn("REDUCE_ONLY_CLAMPED  parent_id=%u  instr=%u  requested=%ld  clamped_to=%ld",
            id_res.value, (unsigned)sig.instr_id, (long)sig.qty_lots, (long)effective_qty);
    }

    parent.total_qty_lots  = effective_qty;
    parent.leaves_qty_lots = effective_qty;

    // all exchanges share the same tick size in this universe. if that changes,
    // this needs to be per-instrument. not today's problem.
    const double tick_sz = cfg_.exchange_states[0].book.tick_size;
    parent.limit_price   = (sig.limit_price_usd > 0.0)
        ? sor::to_ticks(sig.limit_price_usd, tick_sz)
        : price_t(0);

    // risk checks (fat-finger, position, notional, margin) run against the
    // EFFECTIVE size, a signal with the requested (pre-clamp) qty_lots would
    // check the wrong number.
    StrategyOrderSignal eff_sig = sig;
    eff_sig.qty_lots = effective_qty;

    const RiskRejectReason risk_result = risk_.validate(eff_sig);
    if (risk_result != RiskRejectReason::OK) [[unlikely]] {
        g_log.error("RISK_REJECT  parent_id=%u  instr=%u  qty=%ld  limit_usd=%.2f  reason=%u",
            id_res.value, (unsigned)sig.instr_id, (long)effective_qty, sig.limit_price_usd,
            (unsigned)risk_result);
        parent.state = OrderState::REJECTED;
        pool_.free_parent(id_res.value);

        RejectReason reason = RejectReason::RISK_FAT_FINGER;
        switch (risk_result) {
            case RiskRejectReason::POSITION_LIMIT: reason = RejectReason::RISK_POSITION_LIMIT; break;
            case RiskRejectReason::NOTIONAL:       reason = RejectReason::RISK_NOTIONAL;       break;
            case RiskRejectReason::MARGIN:         reason = RejectReason::RISK_MARGIN;         break;
            // FAT_FINGER and DISABLED (malformed instr_id) both fall through
            // to RISK_FAT_FINGER, the closest fit: "this order as sent is bad".
            default: break;
        }
        notify_reject(sig, reason);
        return;
    }

    build_routing_context(parent, effective_qty, routing_ctx_);
    routing_ctx_.short_vol_factor = sig.short_vol_factor;
    routing_ctx_.book_imbalance   = sig.book_imbalance;
    routing_ctx_.decision_ns      = now_ns();

    child_buf_.reset();
    const sor::SplitResult split = (sig.order_type == sor::OrderType::MAKER)
        ? sor_.calculate_maker_placement(routing_ctx_, child_buf_)
        : sor_.calculate_optimal_split(routing_ctx_, child_buf_);

    g_log.info("SOR_RESULT  parent_id=%u  style=%s  children=%u  filled_qty=%.4f  unfilled=%.4f  avg_px=%.2f  success=%d",
        id_res.value,
        (sig.order_type == sor::OrderType::MAKER) ? "MAKER" : "TAKER",
        child_buf_.count,
        split.filled_qty, split.unfilled_qty,
        split.avg_effective_price, (int)split.success);

    if (child_buf_.count == 0) [[unlikely]] {
        g_log.error("SOR_NO_LIQUIDITY  parent_id=%u  instr=%u  limit_usd=%.2f",
            id_res.value, (unsigned)sig.instr_id, sig.limit_price_usd);
        parent.state = OrderState::REJECTED;
        pool_.free_parent(id_res.value);
        notify_reject(sig, RejectReason::NO_LIQUIDITY);
        return;
    }

    parent.state = OrderState::PENDING_NEW;
    pos_.instruments[sig.instr_id].reserve_open(sig.dir, effective_qty);
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
    // same per-instrument lot_size the risk engine and notional gate use,
    // one source of truth (InstrumentTable) instead of three places each
    // assuming their own default.
    ctx.reference_lot_size = cfg_.instruments.lot_size(parent.instr_id);
    ctx.short_vol_factor = 0.0;
    ctx.book_imbalance   = 0.0;
}

void ExecutionCore::dispatch_child_orders(ParentOrder&                 parent,
                                          const sor::ChildOrderBuffer& buf,
                                          const sor::RoutingContext&   ctx) noexcept {
    const uint64_t ts = now_ns();
    uint32_t i = 0;

    for (; i < buf.count; ++i) {
        if (parent.child_count >= ParentOrder::kMaxChildrenPerParent) [[unlikely]]
            break;

        const sor::ChildOrder& co = buf.orders[i];
        const auto cid_res = pool_.alloc_child();
        if (!cid_res.ok) [[unlikely]] {
            g_log.error("CHILD_POOL_EXHAUSTED  parent_id=%u  dispatched=%u  of=%u  total_failures=%lu",
                parent.parent_id, i, buf.count, (unsigned long)pool_.child_alloc_failures());
            break;
        }

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

        g_log.debug("CHILD_DISPATCH  child_id=%u  parent_id=%u  exchange=%u  qty_lots=%ld  price_ticks=%ld",
            cid_res.value, parent.parent_id,
            (unsigned)co.exchange_id,
            (long)child.qty_lots,
            (long)child.price);

        // maker children get the longer, separate maker_timeout_ns clock,
        // taker children get the short ack/fill-oriented child_timeout_ns.
        // same TimerWheel, same callback, same reroute_leaves() on expiry
        // either way, only the duration differs.
        const uint64_t timeout_ns = (co.type == sor::OrderType::MAKER)
            ? cfg_.maker_timeout_ns
            : cfg_.child_timeout_ns;
        timer_wheel_.insert(cid_res.value, ts, timeout_ns);

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

    if (i < buf.count) [[unlikely]] {
        // buf.orders[i..count) never made it out. co.qty is already expressed
        // in this ctx's canonical unit (see sor::RoutingEngine's reference_lot_size),
        // same unit parent.leaves_qty_lots uses, so summing it directly and
        // converting once is correct even if the undispatched legs were headed
        // to exchanges with different native lot sizes.
        double gap_qty = 0.0;
        for (uint32_t j = i; j < buf.count; ++j) gap_qty += buf.orders[j].qty;

        const double ref_lot_size = sor::RoutingEngine::resolve_reference_lot_size(ctx);
        const qty_t  gap_lots     = static_cast<qty_t>(std::round(gap_qty / ref_lot_size));

        if (gap_lots > 0) {
            // this portion never reached an exchange, it was never really "at
            // risk", release the margin reserve_open() took for it upfront.
            // parent.leaves_qty_lots is untouched on purpose: it's derived from
            // total_qty_lots - cum_qty_lots and already correctly counts this
            // gap as still outstanding, which is what lets it get picked up by
            // a later reroute_leaves() instead of vanishing.
            pos_.instruments[parent.instr_id].release_open(parent.dir, gap_lots);
            g_log.error("DISPATCH_GAP  parent_id=%u  dispatched=%u  of=%u  gap_lots=%ld  margin_released",
                parent.parent_id, i, buf.count, (long)gap_lots);
        }
    }

    if (parent.child_count == 0) [[unlikely]] {
        // nothing at all made it out, this parent will never receive another
        // execution report (no children exist to report on), so nobody will
        // ever come back to close it out. handle it here instead of leaving
        // it stuck in PENDING_NEW/PARTIALLY_FILLED forever.
        g_log.error("DISPATCH_EMPTY  parent_id=%u  leaves_lots=%ld  cum_qty_lots=%ld",
            parent.parent_id, (long)parent.leaves_qty_lots, (long)parent.cum_qty_lots);
        parent.state = (parent.cum_qty_lots > 0) ? OrderState::PARTIALLY_FILLED
                                                  : OrderState::REJECTED;
        notify_reject(parent.instr_id, parent.dir, parent.strategy_id,
                     parent.leaves_qty_lots, RejectReason::CHILD_POOL_EXHAUSTED);
        pool_.free_parent(parent.parent_id);
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
    const double fill_px  = static_cast<double>(rep.fill_price_ticks) * tick_sz;

    // child-level accounting stays in this child's native exchange lot units,
    // that's what it was dispatched in and what the exchange reports fills in.
    child.cum_qty_lots   += rep.fill_qty_lots;
    child.leaves_qty_lots = child.qty_lots - child.cum_qty_lots;
    child.state = (child.leaves_qty_lots == 0)
        ? OrderState::FILLED : OrderState::PARTIALLY_FILLED;

    // fill arrived, cancel the timeout
    timer_wheel_.cancel(child.child_id);

    g_log.info("FILL  child_id=%u  parent_id=%u  exchange=%u  fill_lots=%ld  fill_px_ticks=%ld  child_leaves=%ld",
        child.child_id, parent.parent_id,
        (unsigned)child.exchange_id,
        (long)rep.fill_qty_lots,
        (long)rep.fill_price_ticks,
        (long)child.leaves_qty_lots);

    // everything from here down is parent/instrument level, which lives in
    // the instrument's canonical unit, not this child's native exchange lots.
    // convert once, here, rather than let native quantities leak into
    // cum_qty_lots/net_qty_lots/open_*_lots where they'd silently corrupt
    // accounting the moment two exchanges for the same instrument disagree
    // on lot size (see InstrumentTable in oms_types.hpp).
    const double ref_lot_size   = cfg_.instruments.lot_size(parent.instr_id);
    const qty_t  fill_canonical = to_canonical_lots(child, parent.instr_id, rep.fill_qty_lots);

    // update local margin estimate
    margin_monitor_.on_fill(fill_canonical, fill_px, ref_lot_size);

    const qty_t prev_cum_lots = parent.cum_qty_lots;
    parent.cum_qty_lots    += fill_canonical;
    parent.leaves_qty_lots  = parent.total_qty_lots - parent.cum_qty_lots;
    parent.last_update_ns   = rep.recv_ns;

    // running VWAP
    const double prev_cum  = static_cast<double>(prev_cum_lots)       * ref_lot_size;
    const double fill_qty  = static_cast<double>(fill_canonical)      * ref_lot_size;
    const double total_cum = static_cast<double>(parent.cum_qty_lots) * ref_lot_size;
    parent.avg_fill_price  = (prev_cum > 0.0)
        ? (parent.avg_fill_price * prev_cum + fill_px * fill_qty) / total_cum
        : fill_px;

    pos_.instruments[parent.instr_id].apply_fill(parent.dir, fill_canonical, fill_px);
    pos_.instruments[parent.instr_id].release_open(parent.dir, fill_canonical);

    if (parent.leaves_qty_lots == 0) {
        g_log.info("PARENT_FILLED  parent_id=%u  avg_fill_px=%.4f  total_qty=%.4f",
            parent.parent_id, parent.avg_fill_price,
            static_cast<double>(parent.total_qty_lots) * ref_lot_size);
        parent.state = OrderState::FILLED;
        for (uint8_t s = 0; s < ParentOrder::kMaxChildrenPerParent; ++s)
            if (parent.has_child(s)) pool_.free_child(parent.children[s]);
        pool_.free_parent(parent.parent_id);
    } else {
        g_log.debug("PARENT_PARTIAL  parent_id=%u  leaves_lots=%ld",
            parent.parent_id, (long)parent.leaves_qty_lots);
        parent.state = OrderState::PARTIALLY_FILLED;

        // normally there's still a PENDING_NEW sibling child out there that'll
        // eventually report back and re-trigger this check. but if every child
        // this parent currently has is already terminal (e.g. some of them
        // never got dispatched in the first place, see DISPATCH_GAP), nothing
        // else is coming, this is the only chance to pick the remainder back up.
        if (all_children_terminal(parent))
            reroute_leaves(parent);
    }
}

void ExecutionCore::handle_cancel_reject(ChildOrderState&       child,
                                         ParentOrder&           parent,
                                         const ExecutionReport& rep) noexcept {
    const qty_t lost_lots_native = child.leaves_qty_lots;  // this child's own exchange's native unit
    child.leaves_qty_lots  = 0;
    child.state = (rep.exec_type == ExecType::CANCELED)
        ? OrderState::CANCELED : OrderState::REJECTED;

    // release_open()/pos_ are in the instrument's canonical unit, same
    // conversion as handle_fill, see to_canonical_lots.
    const double ref_lot_size    = cfg_.instruments.lot_size(parent.instr_id);
    const qty_t  lost_canonical  = to_canonical_lots(child, parent.instr_id, lost_lots_native);

    timer_wheel_.cancel(child.child_id);
    margin_monitor_.on_release(lost_canonical,
        static_cast<double>(child.price) * cfg_.exchange_states[child.exchange_id].book.tick_size,
        ref_lot_size);

    g_log.warn("CHILD_%s  child_id=%u  parent_id=%u  exchange=%u  lost_lots=%ld",
        (rep.exec_type == ExecType::CANCELED) ? "CANCELED" : "REJECTED",
        child.child_id, parent.parent_id,
        (unsigned)child.exchange_id, (long)lost_lots_native);

    pos_.instruments[parent.instr_id].release_open(parent.dir, lost_canonical);
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
    g_log.warn("REROUTE  parent_id=%u  leaves_lots=%ld",
        parent.parent_id, (long)parent.leaves_qty_lots);

    build_routing_context(parent, parent.leaves_qty_lots, routing_ctx_);

    // fetch fresh vol/imbalance, don't reuse the original signal values
    const uint64_t now    = now_ns();
    const auto inputs     = book_cache_.get_routing_inputs(parent.dir, now);
    routing_ctx_.short_vol_factor = inputs.short_vol_factor;
    routing_ctx_.book_imbalance   = inputs.book_imbalance;
    routing_ctx_.decision_ns      = now;

    if (!inputs.data_fresh)
        g_log.warn("REROUTE  STALE_BOOK_DATA  parent_id=%u  using_zero_vol_imb",
                   parent.parent_id);

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

    // the position may have moved since the ORIGINAL clamp if another parent
    // on this same instrument filled in between, always re-check against the
    // CURRENT position rather than trusting that leaves_qty_lots is still
    // safe to route in full. this is the entire reason reduce_only lives on
    // the parent, not just the original signal. deliberately placed after
    // the cleanup loop above: every child here is already terminal (that's
    // reroute_leaves's precondition) and needs to be freed either way, an
    // early return before that loop ran would leak them.
    if (parent.reduce_only) {
        const qty_t safe_qty = risk_.clamp_reduce_only(
            parent.instr_id, parent.dir, parent.leaves_qty_lots, /*reduce_only_requested=*/true);

        if (safe_qty < parent.leaves_qty_lots) {
            const qty_t excess = parent.leaves_qty_lots - safe_qty;
            pos_.instruments[parent.instr_id].release_open(parent.dir, excess);
            g_log.warn("REROUTE_REDUCE_ONLY_SHRUNK  parent_id=%u  was_leaves=%ld  now_leaves=%ld  excess_released=%ld",
                parent.parent_id, (long)parent.leaves_qty_lots, (long)safe_qty, (long)excess);
            parent.total_qty_lots  -= excess;
            parent.leaves_qty_lots  = safe_qty;
        }

        if (parent.leaves_qty_lots == 0) [[unlikely]] {
            // position closed out from under this order entirely, nothing
            // left that's still safe to reduce, this isn't NO_LIQUIDITY, it's
            // "the job is already done or no longer applicable"
            g_log.warn("REROUTE_REDUCE_ONLY_DONE  parent_id=%u  cum_qty_lots=%ld",
                parent.parent_id, (long)parent.cum_qty_lots);
            parent.state = (parent.cum_qty_lots > 0) ? OrderState::FILLED
                                                      : OrderState::REJECTED;
            pool_.free_parent(parent.parent_id);
            return;
        }

        // leaves_qty_lots may have shrunk, keep the SOR's routing context in
        // sync instead of routing against the pre-shrink target.
        routing_ctx_.target_lots = parent.leaves_qty_lots;
    }

    child_buf_.reset();
    sor_.calculate_optimal_split(routing_ctx_, child_buf_);

    if (child_buf_.count == 0) [[unlikely]] {
        // same situation as DISPATCH_EMPTY in dispatch_child_orders: zero
        // children means zero future execution reports, so this is the last
        // chance to close this parent out instead of parking it forever.
        g_log.error("REROUTE_NO_LIQUIDITY  parent_id=%u  leaves_lots=%ld  cum_qty_lots=%ld",
            parent.parent_id, (long)parent.leaves_qty_lots, (long)parent.cum_qty_lots);
        parent.state = (parent.cum_qty_lots > 0) ? OrderState::PARTIALLY_FILLED
                                                  : OrderState::REJECTED;
        notify_reject(parent.instr_id, parent.dir, parent.strategy_id,
                     parent.leaves_qty_lots, RejectReason::NO_LIQUIDITY);
        pool_.free_parent(parent.parent_id);
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

qty_t ExecutionCore::to_canonical_lots(const ChildOrderState& child,
                                       instr_id_t             instr_id,
                                       qty_t                  native_lots) const noexcept {
    const double native_lot_size = cfg_.exchange_states[child.exchange_id].book.lot_size;
    const double ref_lot_size    = cfg_.instruments.lot_size(instr_id);
    return static_cast<qty_t>(
        std::round(static_cast<double>(native_lots) * native_lot_size / ref_lot_size));
}

void ExecutionCore::notify_reject(const StrategyOrderSignal& sig, RejectReason reason) noexcept {
    notify_reject(sig.instr_id, sig.dir, sig.strategy_id, sig.qty_lots, reason);
}

void ExecutionCore::notify_reject(instr_id_t instr_id, sor::OrderDir dir, uint8_t strategy_id,
                                  qty_t qty_lots, RejectReason reason) noexcept {
    OrderReject rej{};
    rej.qty_lots    = qty_lots;
    rej.instr_id    = instr_id;
    rej.dir         = dir;
    rej.strategy_id = strategy_id;
    rej.reason      = reason;
    rej.reject_ns   = now_ns();

    if (!reject_queue.push(rej)) [[unlikely]]
        g_log.error("REJECT_QUEUE_FULL  instr=%u  reason=%u  DROPPED",
            (unsigned)instr_id, (unsigned)reason);
}

} // namespace oms
