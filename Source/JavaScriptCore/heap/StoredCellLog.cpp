/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "StoredCellLog.h"

#include "Collector.h"
#include "HeapInlines.h"
#include "JSCellInlines.h"
#include "JSObject.h"
#include "SlotVisitorInlines.h"
#include <algorithm>
#include <span>
#include <wtf/FastMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// An old array may hold a young cell without being remembered if the cell is in the log. The log's cells are roots of the next Eden collection.

StoredCellLog::~StoredCellLog()
{
    fastFree(m_begin);
}

unsigned StoredCellLog::vectorLengthOf(const JSCell* owner)
{
    if (!owner->isObject())
        return 0;
    IndexingType indexingType = owner->indexingType();
    if (!hasAnyArrayStorage(indexingType) && !hasInt32(indexingType) && !hasDouble(indexingType) && !hasContiguous(indexingType))
        return 0;
    if (isCopyOnWrite(owner->indexingMode()))
        return 0;
    const Butterfly* butterfly = asObject(const_cast<JSCell*>(owner))->butterfly();
    if (!butterfly)
        return 0;
    return butterfly->vectorLength();
}

bool StoredCellLog::tryRemember(const JSCell* owner, JSCell* stored)
{
    unsigned length = vectorLengthOf(owner);
    if (length < minVectorLengthOfOwner)
        return false;
    if (stored->cellState() == CellState::PossiblyBlack)
        return true;
    if (m_cursor == m_end) {
        // From here on scanning the owner is cheaper, and once it is remembered its stores stop coming.
        if (size() >= length)
            return false;
        Locker locker { m_lock };
        if (!tryGrow())
            return false;
    }
    *m_cursor++ = stored;
    return true;
}

bool StoredCellLog::tryGrow()
{
    size_t oldSize = size();
    size_t newCapacity = std::max(2 * oldSize, minCapacity);
    void* newBegin;
    if (!WTF::tryFastRealloc(m_begin, newCapacity * sizeof(JSCell*)).getValue(newBegin))
        return false;
    m_begin = static_cast<JSCell**>(newBegin);
    m_cursor = m_begin + oldSize;
    m_end = m_begin + newCapacity;
    return true;
}

void StoredCellLog::visitAndReset(SlotVisitor& visitor)
{
#if ASSERT_ENABLED
    visitor.collector().forEachHeap([](JSC::Heap& heap) {
        ASSERT(heap.worldIsStopped());
    });
#endif

    Locker locker { m_lock };
    if (visitor.collectionScope() == CollectionScope::Eden) {
        for (JSCell* cell : std::span { m_begin, m_cursor })
            visitor.appendUnbarriered(cell);
    }
    if (capacity() > minCapacity) {
        fastFree(m_begin);
        m_begin = nullptr;
        m_cursor = nullptr;
        m_end = nullptr;
    } else
        m_cursor = m_begin;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
