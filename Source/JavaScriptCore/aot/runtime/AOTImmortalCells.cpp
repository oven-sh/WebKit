/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImmortalCells.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTState.h"
#include "FastMarkAppender.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImmortalCells);

void ImmortalCells::add(JSCell* cell)
{
    ASSERT(cell);
    Locker locker { m_state.caches->lock() };
    m_cells.append(cell);
}

template<typename Visitor>
void ImmortalCells::visitRoots(Visitor& visitor)
{
    size_t first = 0;
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        JSC::Heap& heap = m_state.instance.vm->heap;
        HeapVersion version = heap.objectSpace().newlyAllocatedVersion();
        bool isFirstVisitInCollection = version != m_lastVisitVersion;
        m_lastVisitVersion = version;
        if (!isFirstVisitInCollection || heap.collectionScope() == CollectionScope::Eden)
            first = m_numberOfCellsVisited;
        m_numberOfCellsVisited = m_cells.size();
    }
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        for (JSCell* cell : m_cells.span().subspan(first))
            appender.appendUnbarriered(cell);
    });
}

template void ImmortalCells::visitRoots(AbstractSlotVisitor&);
template void ImmortalCells::visitRoots(SlotVisitor&);

ImmortalCells::ImmortalCells(State& state)
    : m_state(state)
{
    state.caches->add(CacheKind::StrongRoots, "immortal cells"_s, *this, RootVolatility::GreyedByExecution);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
