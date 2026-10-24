/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "MarkedSpace.h"
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class JSCell;

namespace AOT {

struct State;

class ImmortalCells {
    WTF_MAKE_NONCOPYABLE(ImmortalCells);
    WTF_MAKE_TZONE_ALLOCATED(ImmortalCells);
public:
    explicit ImmortalCells(State&);

    void add(JSCell*);
    template<typename Visitor> void visitRoots(Visitor&);

private:
    static constexpr size_t cellsPerSegment = 512;

    template<typename Functor> void forEachSegment(size_t firstIndex, const Functor&);

    State& m_state;
    SegmentedVector<JSCell*, cellsPerSegment> m_cells;
    size_t m_numberOfCellsVisited { 0 };
    HeapVersion m_lastVisitVersion { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
