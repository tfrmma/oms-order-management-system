#pragma once

#include "oms_types.hpp"
#include <cstring>
#include <cassert>

namespace oms {

struct alignas(32) ChildOrderState {
    child_id_t  child_id;
    order_id_t  parent_id;
    price_t     price;
    qty_t       qty_lots;
    qty_t       cum_qty_lots;
    qty_t       leaves_qty_lots;
    OrderState  state;
    uint8_t     exchange_id;
    uint8_t     level_idx;
    sor::OrderType order_type;
    sor::OrderDir  dir;
    uint8_t     _pad[3];
    uint64_t    sent_ns;
};
static_assert(sizeof(ChildOrderState) == 64);
static_assert(alignof(ChildOrderState) == 32);

struct alignas(kCacheLine) ParentOrder {
    order_id_t  parent_id;
    instr_id_t  instr_id;
    sor::OrderDir dir;
    OrderState  state;
    uint8_t     strategy_id;
    uint8_t     child_count;

    qty_t       total_qty_lots;
    qty_t       cum_qty_lots;
    qty_t       leaves_qty_lots;

    price_t     limit_price;
    double      avg_fill_price;

    uint64_t    create_ns;
    uint64_t    last_update_ns;

    // if you're consistently hitting >16 legs per parent, the SOR config
    // or your target qty is wrong
    static constexpr uint8_t kMaxChildrenPerParent = 16;
    child_id_t  children[kMaxChildrenPerParent];
    uint16_t    child_mask;
    uint8_t     _pad[6];

    void reset() noexcept {
        std::memset(this, 0, sizeof(*this));
        parent_id = kInvalidOrderId;
        state     = OrderState::NEW;
    }

    [[nodiscard]] bool has_child(uint8_t slot) const noexcept {
        return (child_mask >> slot) & 1u;
    }
    void add_child(uint8_t slot, child_id_t cid) noexcept {
        children[slot]  = cid;
        child_mask     |= static_cast<uint16_t>(1u << slot);
        ++child_count;
    }
};
static_assert(alignof(ParentOrder) == kCacheLine);

// two flat pools, pre-allocated at startup. LIFO free lists.
// O(1) alloc/free, O(1) lookup by id. no heap on the hot path.
class OMSOrderPool {
public:
    OMSOrderPool() noexcept { init(); }

    [[nodiscard]] Result<order_id_t> alloc_parent() noexcept {
        if (parent_free_top_ == 0) [[unlikely]] return Result<order_id_t>::fail();
        const order_id_t id = parent_free_[--parent_free_top_];
        parents_[id].reset();
        parents_[id].parent_id = id;
        return Result<order_id_t>::success(id);
    }

    [[nodiscard]] Result<child_id_t> alloc_child() noexcept {
        if (child_free_top_ == 0) [[unlikely]] return Result<child_id_t>::fail();
        const child_id_t id = child_free_[--child_free_top_];
        std::memset(&children_[id], 0, sizeof(ChildOrderState));
        children_[id].child_id = id;
        return Result<child_id_t>::success(id);
    }

    void free_parent(order_id_t id) noexcept {
        assert(id < kMaxParentOrders);
        parent_free_[parent_free_top_++] = id;
    }

    void free_child(child_id_t id) noexcept {
        assert(id < kMaxTotalChildren);
        child_free_[child_free_top_++] = id;
    }

    [[nodiscard]] ParentOrder&       parent(order_id_t id)       noexcept { return parents_[id]; }
    [[nodiscard]] const ParentOrder& parent(order_id_t id) const noexcept { return parents_[id]; }
    [[nodiscard]] ChildOrderState&       child(child_id_t id)       noexcept { return children_[id]; }
    [[nodiscard]] const ChildOrderState& child(child_id_t id) const noexcept { return children_[id]; }

    [[nodiscard]] uint32_t parents_in_use()  const noexcept { return kMaxParentOrders  - parent_free_top_; }
    [[nodiscard]] uint32_t children_in_use() const noexcept { return kMaxTotalChildren - child_free_top_;  }

private:
    void init() noexcept {
        for (uint32_t i = 0; i < kMaxParentOrders; ++i)
            parent_free_[i] = static_cast<order_id_t>(kMaxParentOrders - 1 - i);
        parent_free_top_ = kMaxParentOrders;

        for (uint32_t i = 0; i < kMaxTotalChildren; ++i)
            child_free_[i] = static_cast<child_id_t>(kMaxTotalChildren - 1 - i);
        child_free_top_ = kMaxTotalChildren;
    }

    alignas(kCacheLine) ParentOrder      parents_[kMaxParentOrders]{};
    alignas(kCacheLine) ChildOrderState  children_[kMaxTotalChildren]{};

    order_id_t  parent_free_[kMaxParentOrders]{};
    uint32_t    parent_free_top_{0};

    child_id_t  child_free_[kMaxTotalChildren]{};
    uint32_t    child_free_top_{0};
};

} // namespace oms
