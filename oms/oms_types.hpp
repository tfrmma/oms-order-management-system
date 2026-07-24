#pragma once

#include "sor/types.hpp"
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

// single source of truth for "what does 1 lot mean" per instrument, real-world
// qty (e.g. 0.001 BTC). PreTradeRiskEngine, NotionalConfirmationGate, and
// ExecutionCore's routing/fill accounting all read the same table instead of
// each carrying their own copy that has to be kept in sync by convention.
struct InstrumentSpec {
    double lot_size{0.0};   // 0 = unconfigured, falls back to kFallbackLotSize
};

struct InstrumentTable {
    static constexpr double kFallbackLotSize = 0.001;

    InstrumentSpec instruments[kMaxInstruments]{};

    [[nodiscard]] double lot_size(instr_id_t id) const noexcept {
        return (id < kMaxInstruments && instruments[id].lot_size > 0.0)
            ? instruments[id].lot_size
            : kFallbackLotSize;
    }
};

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
    qty_t         qty_lots;         // canonical unit: InstrumentTable::lot_size(instr_id)
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
    qty_t       fill_qty_lots;      // native unit: that child's own exchange's book.lot_size, NOT canonical
    ExecType    exec_type;
    uint8_t     exchange_id;
    uint8_t     _pad[2];
    uint64_t    exchange_ns;
    uint64_t    recv_ns;
};

// every place a signal can die before (or instead of) becoming a live order.
// deliberately flat, no separate "retryable" flag: RISK_* and NOTIONAL_BLOCK
// mean "fix the order", POOL_EXHAUSTED and NO_LIQUIDITY mean "try again
// later", that's already fully determined by which value this is, a strategy
// reading this can switch on it directly.
enum class RejectReason : uint8_t {
    RISK_FAT_FINGER      = 0,
    RISK_POSITION_LIMIT  = 1,
    RISK_NOTIONAL        = 2,
    RISK_MARGIN          = 3,
    NOTIONAL_GATE_BLOCK  = 4,  // hard block, or operator said no to the confirmation prompt
    PARENT_POOL_EXHAUSTED = 5,
    CHILD_POOL_EXHAUSTED  = 6, // zero children made it out, not a partial dispatch
    NO_LIQUIDITY          = 7, // SOR found nothing routable, first dispatch or reroute
};

// pushed to ExecutionCore::reject_queue, one per dead signal. strategy_id and
// qty_lots echo what was asked for, not what (if anything) partially happened,
// a DISPATCH_GAP style partial dispatch is not a reject, that portion is still
// alive and tracked on its parent, see the README's known limitations.
struct alignas(kCacheLine) OrderReject {
    qty_t          qty_lots;
    instr_id_t     instr_id;
    sor::OrderDir  dir;
    uint8_t        strategy_id;
    RejectReason   reason;
    uint8_t        _pad[2];
    uint64_t       reject_ns;
};

template<typename T>
struct Result {
    T    value{};
    bool ok{false};
    static Result<T> success(T v) noexcept { return {v, true};  }
    static Result<T> fail()        noexcept { return {{}, false}; }
};

} // namespace oms
