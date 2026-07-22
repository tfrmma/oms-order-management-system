#pragma once

// SPSC ring buffer. N must be power of 2.

#include "oms_types.hpp"
#include <atomic>
#include <array>

namespace oms {

template<typename T, uint32_t N>
class SpscQueue {
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    static constexpr uint32_t kMask = N - 1;

public:
    [[nodiscard]] bool push(const T& v) noexcept {
        const uint32_t h      = head_.load(std::memory_order_relaxed);
        const uint32_t next_h = (h + 1) & kMask;
        if (next_h == tail_.load(std::memory_order_acquire)) [[unlikely]]
            return false;
        buf_[h] = v;
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(T& v) noexcept {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) [[unlikely]]
            return false;
        v = buf_[t];
        tail_.store((t + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

private:
    alignas(kCacheLine) std::array<T, N> buf_{};
    alignas(kCacheLine) std::atomic<uint32_t> head_{0};
    alignas(kCacheLine) std::atomic<uint32_t> tail_{0};
};

template<typename T> using GatewayQueue = SpscQueue<T, 512>;
template<typename T> using InboundQueue  = SpscQueue<T, 1024>;

// what goes on the wire to a gateway thread
struct OutboundOrder {
    child_id_t    child_id;
    price_t       price;
    qty_t         qty_lots;
    sor::OrderDir dir;
    sor::OrderType order_type;
    uint8_t       exchange_id;
    uint8_t       instr_id;
    uint8_t       _pad[1];
};
static_assert(sizeof(OutboundOrder) == 32);

} // namespace oms
