#pragma once

#include "types.hpp"
#include <cstdint>
#include <cstring>
#include <array>
#include <atomic>

namespace oms {

using sor::price_t;
using sor::qty_t;

inline constexpr uint32_t kMaxParentOrders   = 4096;
inline constexpr uint32_t kMaxTotalChildren  = kMaxParentOrders * 8;
inline constexpr uint32_t kMaxInstruments    = 64;
inline constexpr uint32_t kCacheLine         = sor::kCacheLineBytes;

using order_id_t  = uint32_t;
using child_id_t  = uint32_t;
using instr_id_t  = uint8_t;
inline constexpr order_id_t kInvalidOrderId = 0xFFFF'FFFF;
inline constexpr child_id_t kInvalidChildId = 0xFFFF'FFFF;

enum class OrderState : uint8_t {
    NEW              = 0,
    PENDING_NEW      = 1,
    PARTIALLY_FILLED = 2,
    FILLED           = 3,
    PENDING_CANCEL   = 4,
    CANCELED         = 5,
    REJECTED         = 6,
};

enum class ExecType : uint8_t {
    NEW      = 0,
    PARTIAL  = 1,
    FILL     = 2,
    CANCELED = 3,
    REJECTED = 4,
    REPLACED = 5,
};

// strategy speaks USD. OMS converts to ticks internally.
struct alignas(kCacheLine) StrategyOrderSignal {
    double        limit_price_usd;  // 0.0 = no limit
    qty_t         qty_lots;
    instr_id_t    instr_id;
    sor::OrderDir dir;
    uint8_t       strategy_id;
    uint8_t       _pad[5];
    uint64_t      signal_ns;
    double        short_vol_factor;
    double        book_imbalance;
};

struct alignas(kCacheLine) ExecutionReport {
    child_id_t  child_id;
    price_t     fill_price_ticks;
    qty_t       fill_qty_lots;
    ExecType    exec_type;
    uint8_t     exchange_id;
    uint8_t     _pad[2];
    uint64_t    exchange_ns;
    uint64_t    recv_ns;
};

template<typename T>
struct Result {
    T    value{};
    bool ok{false};
    static Result<T> success(T v) noexcept { return {v, true};  }
    static Result<T> fail()        noexcept { return {{}, false}; }
};

} // namespace oms
