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
