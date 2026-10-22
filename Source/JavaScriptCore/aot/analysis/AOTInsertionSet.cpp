/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInsertionSet.h"

#if ENABLE(AOT)

#include <algorithm>
#include <wtf/Insertion.h>

namespace JSC { namespace AOT { namespace Typed {

void InsertionSet::insert(ProgramPoint point, ValueID value)
{
    m_insertions.append(QueuedInsertion { point.block, point.index, value });
}

void InsertionSet::execute(Function& function, NOESCAPE const ScopedLambda<void(BlockID, std::span<const ValueID>)>& didInsert)
{
    std::stable_sort(m_insertions.begin(), m_insertions.end(), [](const QueuedInsertion& a, const QueuedInsertion& b) {
        if (a.block != b.block)
            return a.block < b.block;
        return a.index < b.index;
    });

    Vector<WTF::Insertion<ValueID>, 8> blockInsertions;
    Vector<ValueID, 8> insertedValues;
    for (size_t next = 0; next < m_insertions.size();) {
        BlockID id = m_insertions[next].block;
        Block& block = function.edit(id);
        size_t maxIndex = block.values.size();
        for (; next < m_insertions.size() && m_insertions[next].block == id; ++next) {
            const QueuedInsertion& insertion = m_insertions[next];
            RELEASE_ASSERT(insertion.index <= maxIndex);
            blockInsertions.append(WTF::Insertion<ValueID>(insertion.index, insertion.value));
            insertedValues.append(insertion.value);
        }
        WTF::executeInsertions(block.values, blockInsertions);
        didInsert(id, insertedValues.span());
        insertedValues.shrink(0);
    }
    m_insertions.shrink(0);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
