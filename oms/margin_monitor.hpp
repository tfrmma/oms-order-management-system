#pragma once

// margin monitor. two-layer model:
//
// layer 1 (fast): local estimate updated on every fill and open order change.
//   computed from: known positions × mark price × margin rate.
//   latency: zero, happens inline in the execution thread.
//   accuracy: good in normal conditions, drifts if mark price moves fast.
//
// layer 2 (authoritative): REST response from the exchange.
//   latency: 100ms–2s depending on exchange.
//   accuracy: ground truth.
//
// the risk engine uses the more conservative of the two.
// if the local estimate says "no margin" we block even if the REST says ok, // REST data could be stale. if REST says "no margin" we block regardless.
//
// margin_rate: fraction of notional required as margin. 0.1 = 10x leverage.

#include "oms_types.hpp"
#include "position_tracker.hpp"
#include "logger.hpp"
#include <atomic>
#include <cstdint>
#include <cmath>

namespace oms {

struct MarginConfig {
    double initial_margin_rate{0.10};   // 10x leverage default
    double maintenance_margin_rate{0.05};
    double equity_usd{0.0};            // total account equity, set at startup
    // warn if available margin drops below this fraction of total equity
    double warn_threshold_fraction{0.20};
};

class MarginMonitor {
public:
    explicit MarginMonitor(const MarginConfig& cfg,
                           PositionTable&      pos) noexcept
        : cfg_(cfg), pos_(pos)
    {
        // seed the position-table margin with startup equity
        pos_.margin.update(cfg_.equity_usd, 0.0);
        local_equity_.store(cfg_.equity_usd, std::memory_order_relaxed);
    }

    // called by execution thread on every fill, keeps local estimate current
    void on_fill(qty_t fill_lots, double fill_price_usd, double lot_size) noexcept {
        const double notional = static_cast<double>(fill_lots) * lot_size * fill_price_usd;
        const double margin_delta = notional * cfg_.initial_margin_rate;

        // locked_margin_ tracks margin committed to open positions
        const double prev = locked_margin_.load(std::memory_order_relaxed);
        locked_margin_.store(prev + margin_delta, std::memory_order_release);
        recompute_available();
    }

    // called when a child is canceled/rejected, releases reserved margin
    void on_release(qty_t lots, double price_usd, double lot_size) noexcept {
        const double notional     = static_cast<double>(lots) * lot_size * price_usd;
        const double margin_delta = notional * cfg_.initial_margin_rate;

        const double prev = locked_margin_.load(std::memory_order_relaxed);
        const double next = (prev > margin_delta) ? (prev - margin_delta) : 0.0;
        locked_margin_.store(next, std::memory_order_release);
        recompute_available();
    }

    // REST reconciliation thread calls this with ground truth from exchange
    void on_rest_update(double equity_usd, double used_margin_usd,
                        uint64_t ts_ns) noexcept {
        local_equity_.store(equity_usd, std::memory_order_relaxed);
        // REST is authoritative, override our local locked estimate
        locked_margin_.store(used_margin_usd, std::memory_order_release);
        rest_update_ns_.store(ts_ns, std::memory_order_relaxed);
        recompute_available();

        g_log.info("MARGIN_REST_UPDATE  equity=%.2f  used=%.2f  available=%.2f  ts=%lu",
                   equity_usd, used_margin_usd,
                   equity_usd - used_margin_usd, (unsigned long)ts_ns);
    }

    // mark price update, recalculates unrealised PnL effect on available margin.
    // called by feed handler thread.
    void on_mark_price(double mark_usd, double lot_size, instr_id_t instr_id) noexcept {
        const auto& instr  = pos_.instruments[instr_id];
        const int64_t net  = instr.net_qty_lots.load(std::memory_order_acquire);
        const double avg   = instr.avg_entry_price.load(std::memory_order_acquire);

        if (net == 0 || avg <= 0.0) return;

        const double qty   = static_cast<double>(net) * lot_size;
        const double upnl  = (mark_usd - avg) * qty;

        upnl_.store(upnl, std::memory_order_release);
        recompute_available();

        if (upnl < -cfg_.equity_usd * 0.10) {
            g_log.warn("MARGIN_UPNL_WARNING  upnl=%.2f USD  mark=%.2f  avg=%.2f",
                       upnl, mark_usd, avg);
        }
    }

    // used by PreTradeRiskEngine, returns the conservative (lower) estimate
    [[nodiscard]] double available_conservative() const noexcept {
        return pos_.margin.available();
    }

    [[nodiscard]] bool is_rest_fresh(uint64_t now_ns,
                                     uint64_t max_stale_ns = 5'000'000'000ULL) const noexcept {
        const uint64_t ts = rest_update_ns_.load(std::memory_order_acquire);
        return (ts > 0) && ((now_ns - ts) < max_stale_ns);
    }

    void check_warnings(uint64_t now_ns) noexcept {
        const double avail = available_conservative();
        const double equity = local_equity_.load(std::memory_order_relaxed);
        if (equity > 0.0 && avail < equity * cfg_.warn_threshold_fraction) {
            g_log.warn("MARGIN_LOW  available=%.2f USD  equity=%.2f  fraction=%.2f",
                       avail, equity, avail / equity);
        }
        if (!is_rest_fresh(now_ns)) {
            g_log.warn("MARGIN_REST_STALE  last_update_ns=%lu",
                       (unsigned long)rest_update_ns_.load(std::memory_order_relaxed));
        }
    }

private:
    void recompute_available() noexcept {
        const double equity   = local_equity_.load(std::memory_order_relaxed);
        const double locked   = locked_margin_.load(std::memory_order_relaxed);
        const double upnl     = upnl_.load(std::memory_order_relaxed);
        // conservative: negative upnl reduces available margin, positive doesn't count
        const double adj_equity = equity + (upnl < 0.0 ? upnl : 0.0);
        pos_.margin.update(adj_equity, locked);
    }

    MarginConfig    cfg_;
    PositionTable&  pos_;

    std::atomic<double>   local_equity_{0.0};
    std::atomic<double>   locked_margin_{0.0};
    std::atomic<double>   upnl_{0.0};
    std::atomic<uint64_t> rest_update_ns_{0};
};

} // namespace oms
