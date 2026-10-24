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
#include <algorithm>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImmortalCells);

void ImmortalCells::add(JSCell* cell)
{
    ASSERT(cell);
    Locker locker { m_state.caches->lock() };
    m_cells.append(cell);
}

template<typename Functor>
ALWAYS_INLINE void ImmortalCells::forEachSegment(size_t firstIndex, const Functor& functor)
{
    size_t size = m_cells.size();
    for (size_t begin = firstIndex; begin < size;) {
        size_t end = std::min(begin - begin % cellsPerSegment + cellsPerSegment, size);
        functor(unsafeMakeSpan(&m_cells[begin], end - begin));
        begin = end;
    }
}

// Needs no lock: a segment never moves, and these cells were added before the visit that deferred them.
static void visitDeferredSegment(SlotVisitor& visitor, void* segment, size_t begin, size_t end)
{
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        for (JSCell* cell : unsafeMakeSpan(static_cast<JSCell**>(segment), end).subspan(begin))
            appender.appendUnbarriered(cell);
    });
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
        bool visitsEveryCell = isFirstVisitInCollection && heap.collectionScope() != CollectionScope::Eden;
        if (!visitsEveryCell)
            first = m_numberOfCellsVisited;
        m_numberOfCellsVisited = m_cells.size();

        if (visitsEveryCell && visitor.defersRootRegions()) {
            forEachSegment(0, [&](std::span<JSCell*> cells) {
                visitor.deferRootRegion({ visitDeferredSegment, cells.data(), 0, cells.size() });
            });
            return;
        }
    }
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        forEachSegment(first, [&](std::span<JSCell*> cells) ALWAYS_INLINE_LAMBDA {
            for (JSCell* cell : cells)
                appender.appendUnbarriered(cell);
        });
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
