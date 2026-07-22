#pragma once

// per-exchange book snapshot for the execution thread.
// updated by the feed handler thread, read by execution on every reroute.
//
// stores pre-computed vol_factor and book_imbalance so the rerouter doesn't
// need to reconstruct them from raw ticks. the feed handler does that work.
//
// vol_factor: normalised short-term realised vol [0, ~1]. 0 = quiet, 1 = 2σ event.
// book_imbalance: signed bid/ask qty imbalance [-1, +1].
//   +1 = all bids (buy pressure), -1 = all asks (sell pressure).
//
// atomic<double> pairs are slightly ugly but correct, each field is
// independently consistent. if you need joint consistency across vol+imb,
// use a seqlock instead. for now the approximation is fine.

#include "oms_types.hpp"
#include "sor/types.hpp"
#include "logger.hpp"
#include <atomic>
#include <array>
#include <cstdint>

namespace oms {

struct alignas(kCacheLine) ExchangeSnapshot {
    std::atomic<double>   vol_factor{0.0};
    std::atomic<double>   book_imbalance{0.0};
    std::atomic<double>   mid_price_usd{0.0};
    std::atomic<uint64_t> last_update_ns{0};

    // feed handler thread calls this
    void update(double vol, double imb, double mid, uint64_t ts_ns) noexcept {
        vol_factor.store(vol,    std::memory_order_relaxed);
        book_imbalance.store(imb, std::memory_order_relaxed);
        mid_price_usd.store(mid,  std::memory_order_relaxed);
        last_update_ns.store(ts_ns, std::memory_order_release);
    }

    // execution thread reads this, acquire pairs with the release above
    struct Reading {
        double   vol_factor;
        double   book_imbalance;
        double   mid_price_usd;
        uint64_t ts_ns;
        bool     fresh;   // false if data is older than max_stale_ns
    };

    [[nodiscard]] Reading read(uint64_t now_ns,
                               uint64_t max_stale_ns = 2'000'000'000ULL) const noexcept {
        const uint64_t ts  = last_update_ns.load(std::memory_order_acquire);
        const bool fresh   = (ts > 0) && ((now_ns - ts) < max_stale_ns);
        return {
            vol_factor.load(std::memory_order_relaxed),
            book_imbalance.load(std::memory_order_relaxed),
            mid_price_usd.load(std::memory_order_relaxed),
            ts,
            fresh
        };
    }
};
static_assert(alignof(ExchangeSnapshot) == kCacheLine);

// one snapshot per exchange, shared between feed handler and execution thread.
// the execution thread picks the most relevant one (e.g. highest-volume exchange
// or a weighted average). for now: use exchange 0 as the reference.
struct BookSnapshotCache {
    std::array<ExchangeSnapshot, sor::kMaxExchanges> exchanges{};

    // returns vol/imbalance for routing, uses the freshest available snapshot.
    // if all are stale, falls back to 0/0 (conservative: no vol/imb penalty).
    struct RoutingInputs {
        double   short_vol_factor{0.0};
        double   book_imbalance{0.0};
        bool     data_fresh{false};
    };

    [[nodiscard]] RoutingInputs get_routing_inputs(
        sor::OrderDir dir, uint64_t now_ns) const noexcept
    {
        uint64_t best_ts   = 0;
        double   best_vol  = 0.0;
        double   best_imb  = 0.0;
        bool     any_fresh = false;

        for (uint32_t i = 0; i < sor::kMaxExchanges; ++i) {
            const auto r = exchanges[i].read(now_ns);
            if (r.fresh && r.ts_ns > best_ts) {
                best_ts   = r.ts_ns;
                best_vol  = r.vol_factor;
                // flip sign for SELL, imbalance leaning against us
                best_imb  = (dir == sor::OrderDir::BUY) ? r.book_imbalance : -r.book_imbalance;
                any_fresh = true;
            }
        }

        if (!any_fresh) {
            g_log.warn("%s", "BOOK_SNAPSHOT  ALL_STALE  using_zero_vol_imb");
        }

        return { best_vol, best_imb, any_fresh };
    }
};

} // namespace oms
