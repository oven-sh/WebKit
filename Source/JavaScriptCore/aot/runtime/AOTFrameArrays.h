/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTRuntimeTable.h"
#include "JSArray.h"

namespace JSC { namespace AOT {

struct Instance;

// Compiled code keeps an array that nothing writes to and nobody keeps in its frame (aot/analysis/AOTFrameArrayParameters.h).
ALWAYS_INLINE bool isFrameArray(JSCell* cell)
{
    return static_cast<uint8_t>(cell->cellState()) == frameCellState && cell->type() == ArrayType;
}

// Every operation that compiled code or a stub may hand such an array: what readsArrayWhereItLies() (aot/passes/AOTFrameAllocationPointers.h) lists ends in no other.
constexpr bool mayBeGivenFrameArray(Entry entry)
{
    switch (entry) {
    case Entry::operationAOTGetByVal:
    case Entry::operationAOTApply:
    case Entry::operationAOTAppendElements:
    case Entry::operationAOTNewArrayOfElements:
    case Entry::operationAOTCreateUnmappedArguments:
    case Entry::operationAOTCreateMappedArguments:
    case Entry::operationAOTContentsHaveType:
        return true;
    default:
        return false;
    }
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
