#pragma once

// live terminal snapshot of pool + position state. runs on its own thread,
// polls atomics every refresh_ms and writes to stderr. read-only: never
// touches pool_/pos_ mutation paths, so there's no synchronization needed
// beyond what InstrumentPosition/OMSOrderPool already give you.
//
// this is a stdout dashboard for the prototype. in production you'd swap
// print_snapshot() for a push to a metrics endpoint and drop the thread
// entirely, but the polling loop / refresh interval design carries over.

#include "oms_types.hpp"
#include "order_pool.hpp"
#include "position_tracker.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace oms {

struct DashboardConfig {
    double      lot_size{0.001};
    double      tick_size{0.5};
    const char* instrument_name{"UNKNOWN"};
    uint32_t    refresh_ms{500};
    instr_id_t  instr_id{0};
    bool        enabled{true};    // false disables the thread entirely
};

class Dashboard {
public:
    Dashboard(const OMSOrderPool& pool, const PositionTable& pos,
              const DashboardConfig& cfg) noexcept
        : pool_(pool), pos_(pos), cfg_(cfg)
    {}

    ~Dashboard() noexcept { stop(); }

    Dashboard(const Dashboard&)            = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    void start() noexcept {
        if (!cfg_.enabled || running_.load(std::memory_order_relaxed)) return;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { run_loop(); });
    }

    void stop() noexcept {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

private:
    void run_loop() const noexcept {
        while (running_.load(std::memory_order_acquire)) {
            print_snapshot();
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.refresh_ms));
        }
    }

    void print_snapshot() const noexcept {
        if (cfg_.instr_id >= kMaxInstruments) return;

        const InstrumentPosition& instrument = pos_.instruments[cfg_.instr_id];
        const int64_t net_lots = instrument.net_qty_lots.load(std::memory_order_acquire);
        const double  net_qty  = static_cast<double>(net_lots) * cfg_.lot_size;
        const double  avg_px   = instrument.avg_entry_price.load(std::memory_order_acquire);
        const double  margin   = pos_.margin.available();

        // \r + no trailing \n: overwrites the same line instead of scrolling.
        // fine for a terminal, useless if this ever gets piped to a file.
        std::fprintf(stderr,
            "\r  [DASH] %-10s  net=%+9.4f  avg_px=%10.2f  "
            "parents=%4u  children=%5u  margin=%12.2f USD  ",
            cfg_.instrument_name, net_qty, avg_px,
            pool_.parents_in_use(), pool_.children_in_use(), margin);
        std::fflush(stderr);
    }

    const OMSOrderPool&   pool_;
    const PositionTable&  pos_;
    DashboardConfig       cfg_;
    std::atomic<bool>     running_{false};
    std::thread           thread_;
};

} // namespace oms
