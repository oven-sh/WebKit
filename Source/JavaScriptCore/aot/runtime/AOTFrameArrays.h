/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "JSArray.h"

namespace JSC { namespace AOT {

struct Instance;

// Compiled code lends an array to a call whose callees only read it as a cell in its own frame: ParameterFact::MayTakeFrameArray of aot/facts/AOTFactTables.h.
// Compiled code and stubs look at the header, the length and the elements where they lie. An operation that looks for a member by a key, and may allocate, file an answer or run code on the way, asks a copy on the heap.
ALWAYS_INLINE bool isFrameArray(JSCell* cell)
{
    return static_cast<uint8_t>(cell->cellState()) == frameCellState && cell->type() == ArrayType;
}

JSArray* copyFrameArrayToHeap(Instance*, JSCell* frameArray);

ALWAYS_INLINE JSValue heapValueOf(Instance* instance, JSValue value)
{
    if (value.isCell() && isFrameArray(value.asCell())) [[unlikely]]
        return copyFrameArrayToHeap(instance, value.asCell());
    return value;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
