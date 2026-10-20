/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CagedAlignedMemoryAllocator.h"

#include <wtf/FastMalloc.h>

namespace JSC {

CagedAlignedMemoryAllocator::CagedAlignedMemoryAllocator() = default;

CagedAlignedMemoryAllocator::~CagedAlignedMemoryAllocator() = default;

void* CagedAlignedMemoryAllocator::tryAllocateAlignedMemory(size_t alignment, size_t size)
{
    if (void* result = m_cage.tryAllocateAligned(alignment, size))
        return result;
    if (MarkedBlockCage::isRequired())
        return nullptr;
    MarkedBlockCage::didAllocateOutside();
    return tryFastCompactAlignedMalloc(alignment, size);
}

void CagedAlignedMemoryAllocator::freeAlignedMemory(void* basePtr)
{
    if (m_cage.contains(basePtr))
        m_cage.free(basePtr);
    else
        fastFree(basePtr);
}

void CagedAlignedMemoryAllocator::dump(PrintStream& out) const
{
    out.print("Caged(", RawPointer(this), ")");
}

void* CagedAlignedMemoryAllocator::tryAllocateMemory(size_t size)
{
    if (void* result = m_cage.tryAllocate(size))
        return result;
    if (MarkedBlockCage::isRequired())
        return nullptr;
    MarkedBlockCage::didAllocateOutside();
    return FastCompactMalloc::tryMalloc(size);
}

void CagedAlignedMemoryAllocator::freeMemory(void* pointer)
{
    if (m_cage.contains(pointer))
        m_cage.free(pointer);
    else
        FastCompactMalloc::free(pointer);
}

void* CagedAlignedMemoryAllocator::tryReallocateMemory(void* pointer, size_t size)
{
    if (m_cage.contains(pointer))
        return m_cage.tryReallocate(pointer, size);
    return FastCompactMalloc::tryRealloc(pointer, size);
}

} // namespace JSC
