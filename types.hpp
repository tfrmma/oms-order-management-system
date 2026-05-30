#pragma once

#include <cstdint>
#include <cmath>
#include <array>

namespace sor {

inline constexpr uint32_t kMaxExchanges   = 5;
inline constexpr uint32_t kMaxDepth       = 20;
inline constexpr uint32_t kMaxChildOrders = kMaxExchanges * kMaxDepth;
inline constexpr uint32_t kCacheLineBytes = 64;

// prices in ticks, quantities in lots. yes, int64. no, I don't want to discuss it.
using price_t = int64_t;
using qty_t   = int64_t;

// call these from ingestion / reporting only. call from hot path and I will find you.
[[nodiscard]] inline price_t to_ticks(double price, double tick_size) noexcept {
    return static_cast<price_t>(std::round(price / tick_size));
}
[[nodiscard]] inline qty_t to_lots(double qty, double lot_size) noexcept {
    return static_cast<qty_t>(qty / lot_size);
}
[[nodiscard]] inline double from_ticks(price_t ticks, double tick_size) noexcept {
    return static_cast<double>(ticks) * tick_size;
}
[[nodiscard]] inline double from_lots(qty_t lots, double lot_size) noexcept {
    return static_cast<double>(lots) * lot_size;
}

enum class Side      : uint8_t { BID = 0, ASK = 1 };
enum class OrderDir  : uint8_t { BUY = 0, SELL = 1 };
enum class OrderType : uint8_t { MAKER = 0, TAKER = 1 };

enum class ExchangeId : uint8_t {
    BINANCE = 0, BYBIT = 1, OKX = 2, DERIBIT = 3, BITGET = 4, INVALID = 0xFF
};

struct alignas(kCacheLineBytes) FeeMatrix {
    double rates[kMaxExchanges][2];
    [[nodiscard]] double taker(uint8_t ex) const noexcept { return rates[ex][1]; }
    [[nodiscard]] double maker(uint8_t ex) const noexcept { return rates[ex][0]; }
};

struct ChildOrder {
    double    price;
    double    qty;
    double    effective_cost;
    uint8_t   exchange_id;
    uint8_t   level_idx;
    OrderType type;
    OrderDir  dir;
    uint8_t   _pad[4];
};
static_assert(sizeof(ChildOrder) == 32);

// all three 64-bit fields are integer — FPU doesn't touch this during merge or fill.
// double conversions happen when we write ChildOrder, i.e. when the decision is done.
//
// effective_ticks encoding:
//   BUY  (+price_ticks + penalty_ticks):  ascending → min = cheapest ask
//   SELL (-price_ticks + penalty_ticks):  ascending → min = most negative = best bid
struct alignas(32) CostSlice {
    int64_t  effective_ticks;
    qty_t    available_lots;
    price_t  raw_price_ticks;   // kept for reporting: price_usd = ticks * tick_size
    uint8_t  exchange_id;
    uint8_t  level_idx;
    uint8_t  _pad[6];
};
static_assert(sizeof(CostSlice) == 32);

struct SplitResult {
    bool     success;
    uint32_t child_count;
    double   filled_qty;
    double   unfilled_qty;
    double   total_effective_cost;
    double   avg_effective_price;
    double   spread_capture_bps;
    double   market_impact_bps;
    double   adverse_selection_est;
};

} // namespace sor
