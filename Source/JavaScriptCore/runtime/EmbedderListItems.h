/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/JSCellButterfly.h>
#include <JavaScriptCore/JSDestructibleObject.h>
#include <wtf/Atomics.h>

namespace JSC {

static constexpr JSType EmbedderListType = static_cast<JSType>(0b11100010);

// The first member behind the JSDestructibleObject of a cell of EmbedderListType, which has no indexed storage and intercepts
// indexed reads. What it publishes here is read in place of getOwnPropertySlotByIndex until the embedder advances the clock.
// An empty item, an index behind the last and a clock that has moved all mean that the object is asked.
class EmbedderListItems {
    WTF_MAKE_NONCOPYABLE(EmbedderListItems);
public:
    EmbedderListItems() = default;

    static constexpr ptrdiff_t offsetInCell = sizeof(JSDestructibleObject);
    static constexpr ptrdiff_t offsetOfItems() { return offsetInCell + OBJECT_OFFSETOF(EmbedderListItems, m_items); }
    static constexpr ptrdiff_t offsetOfTime() { return offsetInCell + OBJECT_OFFSETOF(EmbedderListItems, m_time); }
    static constexpr ptrdiff_t offsetOfClock() { return offsetInCell + OBJECT_OFFSETOF(EmbedderListItems, m_clock); }

    static EmbedderListItems& of(JSCell* cell)
    {
        ASSERT(cell->type() == EmbedderListType);
        return *std::bit_cast<EmbedderListItems*>(std::bit_cast<uint8_t*>(cell) + offsetInCell);
    }

    static ALWAYS_INLINE JSValue tryGet(JSCell* cell, uint32_t index)
    {
        if (cell->type() != EmbedderListType)
            return { };
        JSCellButterfly* items = of(cell).currentItems();
        return items && index < items->publicLength() ? items->toButterfly()->contiguous().atUnsafe(index).get() : JSValue();
    }

    ALWAYS_INLINE JSCellButterfly* currentItems() const { return m_time == *m_clock ? itemsOfAnyTime() : nullptr; }

    // The collector takes away items that are not current, at any moment.
    ALWAYS_INLINE JSCellButterfly* itemsOfAnyTime() const { return WTF::opaque(m_items.get()); }

    // The clock outlives the VM and never shows a time twice. Without one the items are current for good.
    void publish(VM& vm, JSCell* owner, JSCellButterfly* items, const uint64_t* clock)
    {
        ASSERT(this == &of(owner));
        RELEASE_ASSERT(hasContiguous(items->indexingType()));
        m_clock = clock ? clock : &m_time;
        m_time = *m_clock;
        m_items.set(vm, owner, items);
    }

    // What was published last still holds.
    ALWAYS_INLINE void renew() { m_time = *m_clock; }

    ALWAYS_INLINE void withdraw() { m_items.clear(); }

    template<typename Visitor> void visit(Visitor& visitor)
    {
        if (WTF::atomicLoad(&m_time) == WTF::atomicLoad(const_cast<uint64_t*>(WTF::atomicLoad(&m_clock))))
            visitor.append(m_items);
        else
            m_items.clear();
    }

private:
    WriteBarrier<JSCellButterfly> m_items;
    uint64_t m_time { 0 };
    const uint64_t* m_clock { &m_time };
};

} // namespace JSC
