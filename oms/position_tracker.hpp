#pragma once

// position + margin state. network/reconciliation thread writes, risk reads.
// only net_qty_lots and margin_available are truly hot, everything else
// is updated between cycles.

#include "oms_types.hpp"
#include <atomic>

namespace oms {

struct alignas(kCacheLine) InstrumentPosition {
    std::atomic<int64_t>  net_qty_lots{0};    // signed: + long, - short
    std::atomic<int64_t>  open_buy_lots{0};
    std::atomic<int64_t>  open_sell_lots{0};
    std::atomic<double>   avg_entry_price{0.0};

    void apply_fill(sor::OrderDir dir, qty_t lots, double price) noexcept {
        const int64_t signed_lots = (dir == sor::OrderDir::BUY)
            ? static_cast<int64_t>(lots) : -static_cast<int64_t>(lots);

        const int64_t prev = net_qty_lots.load(std::memory_order_relaxed);
        const int64_t next = prev + signed_lots;

        const double prev_avg = avg_entry_price.load(std::memory_order_relaxed);
        const double prev_abs = static_cast<double>(prev < 0 ? -prev : prev);
        const double fill_abs = static_cast<double>(lots);
        const double next_abs = static_cast<double>(next < 0 ? -next : next);

        double next_avg = 0.0;
        if (next_abs > 1e-9) {
            // side flip resets the cost basis, could argue for a smarter model
            if ((prev >= 0) == (next >= 0))
                next_avg = (prev_avg * prev_abs + price * fill_abs) / (prev_abs + fill_abs);
            else
                next_avg = price;
        }

        net_qty_lots.store(next, std::memory_order_release);
        avg_entry_price.store(next_avg, std::memory_order_release);
    }

    void reserve_open(sor::OrderDir dir, qty_t lots) noexcept {
        auto& target = (dir == sor::OrderDir::BUY) ? open_buy_lots : open_sell_lots;
        target.fetch_add(static_cast<int64_t>(lots), std::memory_order_relaxed);
    }

    void release_open(sor::OrderDir dir, qty_t lots) noexcept {
        auto& target = (dir == sor::OrderDir::BUY) ? open_buy_lots : open_sell_lots;
        target.fetch_sub(static_cast<int64_t>(lots), std::memory_order_relaxed);
    }
};
static_assert(alignof(InstrumentPosition) == kCacheLine);

// margin updated async via REST reconciliation, not real-time, not perfect.
// real-time margin would need a dedicated WS feed per exchange. out of scope.
struct alignas(kCacheLine) MarginState {
    std::atomic<double>   total_equity{0.0};
    std::atomic<double>   margin_used{0.0};
    std::atomic<double>   margin_available{0.0};
    std::atomic<uint64_t> last_update_ns{0};

    void update(double equity, double used) noexcept {
        total_equity.store(equity, std::memory_order_relaxed);
        margin_used.store(used, std::memory_order_relaxed);
        margin_available.store(equity - used, std::memory_order_release);
    }

    [[nodiscard]] double available() const noexcept {
        return margin_available.load(std::memory_order_acquire);
    }
};
static_assert(alignof(MarginState) == kCacheLine);

struct PositionTable {
    InstrumentPosition instruments[kMaxInstruments]{};
    MarginState        margin{};
};

} // namespace oms
