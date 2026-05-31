#pragma once

// hierarchical timer wheel. O(1) insert, O(1) per-tick expiry check.
//
// two levels: coarse (seconds) + fine (ms slots within a second).
// good enough for cancel-on-timeout at the 500ms–5s range typical in crypto.
//
// not a general-purpose timer. it knows about ChildOrderState and fires
// cancel callbacks directly. keeping it domain-specific keeps it fast.
//
// called from the execution thread on every spin iteration via tick().
// tick() is cheap when nothing expires — just a counter increment and
// a slot check. no heap, no locks.

#include "oms_types.hpp"
#include "order_pool.hpp"
#include "logger.hpp"
#include <array>
#include <cstdint>
#include <functional>

namespace oms {

// callback: called when a child order times out
using TimeoutCallback = std::function<void(child_id_t)>;

struct TimerEntry {
    child_id_t  child_id{kInvalidChildId};
    uint64_t    deadline_ns{0};
    bool        active{false};
};

// 1024 fine slots × 64 coarse slots = ~65 seconds of coverage at 1ms resolution.
// slots are hashed by deadline — collisions are resolved by scanning the slot.
// slot depth of 8 handles bursts of simultaneous orders without spilling.
inline constexpr uint32_t kFineSlots   = 1024;
inline constexpr uint32_t kSlotDepth  = 8;     // max concurrent timers per slot

class TimerWheel {
public:
    explicit TimerWheel(uint64_t tick_ns = 1'000'000ULL) noexcept  // 1ms default
        : tick_ns_(tick_ns)
    {}

    void set_callback(TimeoutCallback cb) noexcept {
        callback_ = std::move(cb);
    }

    // insert a timeout for child_id, deadline_ns nanoseconds from now.
    // returns false if the slot is full — caller should log and treat as cancel.
    bool insert(child_id_t child_id, uint64_t now_ns, uint64_t timeout_ns) noexcept {
        const uint64_t deadline = now_ns + timeout_ns;
        const uint32_t slot     = slot_for(deadline);

        for (uint32_t i = 0; i < kSlotDepth; ++i) {
            TimerEntry& e = wheel_[slot][i];
            if (!e.active) {
                e.child_id   = child_id;
                e.deadline_ns = deadline;
                e.active      = true;
                return true;
            }
        }

        g_log.error("TIMER_WHEEL  SLOT_FULL  slot=%u  child_id=%u  treating_as_timeout",
                    slot, child_id);
        return false;
    }

    // cancel a timer — called when a fill or ack arrives before timeout.
    void cancel(child_id_t child_id) noexcept {
        // scan all slots of the expected deadline range — we don't store the slot
        // index per child_id to avoid extra state. at kFineSlots=1024 this is
        // acceptable: cancel is rare (it's the happy path).
        for (uint32_t s = 0; s < kFineSlots; ++s) {
            for (uint32_t i = 0; i < kSlotDepth; ++i) {
                TimerEntry& e = wheel_[s][i];
                if (e.active && e.child_id == child_id) {
                    e.active = false;
                    return;
                }
            }
        }
        // not found — probably already fired or never inserted. not an error.
    }

    // call on every execution loop iteration.
    // advances the internal clock and fires callbacks for expired entries.
    void tick(uint64_t now_ns) noexcept {
        // advance clock in tick_ns_ steps to avoid firing the same slot twice
        while (now_ns_ + tick_ns_ <= now_ns) {
            now_ns_ += tick_ns_;
            check_slot(slot_for(now_ns_));
        }
    }

    void reset_clock(uint64_t now_ns) noexcept { now_ns_ = now_ns; }

private:
    [[nodiscard]] uint32_t slot_for(uint64_t ts_ns) const noexcept {
        // divide by tick resolution and mask into the ring
        return static_cast<uint32_t>((ts_ns / tick_ns_) & (kFineSlots - 1));
    }

    void check_slot(uint32_t slot) noexcept {
        for (uint32_t i = 0; i < kSlotDepth; ++i) {
            TimerEntry& e = wheel_[slot][i];
            if (!e.active) continue;
            if (now_ns_ >= e.deadline_ns) {
                g_log.warn("TIMER_WHEEL  TIMEOUT  child_id=%u  deadline_ns=%lu  now_ns=%lu",
                           e.child_id, (unsigned long)e.deadline_ns, (unsigned long)now_ns_);
                e.active = false;
                if (callback_) callback_(e.child_id);
            }
        }
    }

    std::array<std::array<TimerEntry, kSlotDepth>, kFineSlots> wheel_{};
    TimeoutCallback callback_;
    uint64_t        tick_ns_{1'000'000ULL};
    uint64_t        now_ns_{0};
};

} // namespace oms
