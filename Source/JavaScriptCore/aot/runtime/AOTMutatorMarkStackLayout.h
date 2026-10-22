/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "MarkStack.h"

namespace JSC { namespace AOT {

// Where a stub finds what MarkStackArray::append() reads and writes.
class MutatorMarkStackLayout final : public MarkStackArray {
public:
    using Segment = GCArraySegment<const JSCell*>;

    static constexpr size_t segmentCapacity = CapacityFromSize<Segment::blockSize>::value;
    static constexpr ptrdiff_t offsetOfFirstCellInSegment = sizeof(Segment);
    static ptrdiff_t offsetOfTop() { return OBJECT_OFFSETOF(MutatorMarkStackLayout, m_top); }
    // The list of segments begins with its head. initializeCollectorWords() checks that.
    static ptrdiff_t offsetOfHeadSegment() { return OBJECT_OFFSETOF(MutatorMarkStackLayout, m_segments); }
    static Segment* headSegmentOf(MarkStackArray& stack) { return static_cast<MutatorMarkStackLayout&>(stack).m_segments.head(); }
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
