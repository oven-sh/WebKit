/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowParts.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"

namespace JSC { namespace AOT { namespace Typed {

BuiltinSlotID indexedElementSlotOf(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    if (!builtinClass)
        return { };

    BuiltinSlotID slot = builtins.findSlot(builtinClass, ContentsSlot::Element);
    if (!slot)
        return { };

    AbstractHeap heap = builtins.at(slot).heap;
    if (heap == AbstractHeap::Elements || heap == AbstractHeap::TypedArrayElements)
        return slot;
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
