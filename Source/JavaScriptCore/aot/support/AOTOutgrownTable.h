/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <span>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>

namespace JSC { namespace AOT {

// Whoever still reads the table without the lock finds its entries or zeros.
template<typename Slot>
void letGoOfPagesOfOutgrownTable(std::span<Slot> slots)
{
    uintptr_t mask = WTF::pageSize() - 1;
    uintptr_t begin = (reinterpret_cast<uintptr_t>(slots.data()) + mask) & ~mask;
    uintptr_t end = reinterpret_cast<uintptr_t>(slots.data() + slots.size()) & ~mask;
    if (begin < end)
        OSAllocator::hintMemoryNotNeededSoon(reinterpret_cast<void*>(begin), end - begin);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
