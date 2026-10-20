/*
 * Copyright (C) 2017-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "StructureAlignedMemoryAllocator.h"

#include "AddressSpaceBudget.h"
#include "JSCConfig.h"
#include "MarkedBlock.h"
#include "Options.h"
#include "ReservedRangeHeap.h"
#include "StructureID.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/RAMSize.h>

#if OS(DARWIN)
#include <wtf/cocoa/Entitlements.h>
#endif

#include <wtf/OSAllocator.h>

namespace JSC {

StructureAlignedMemoryAllocator::StructureAlignedMemoryAllocator() = default;
StructureAlignedMemoryAllocator::~StructureAlignedMemoryAllocator() = default;

void StructureAlignedMemoryAllocator::dump(PrintStream& out) const
{
    out.print("Structure(", RawPointer(this), ")");
}

void* StructureAlignedMemoryAllocator::tryAllocateMemory(size_t)
{
    return nullptr;
}

void StructureAlignedMemoryAllocator::freeMemory(void*)
{
    // Structures do not support Precise allocations right now.
    RELEASE_ASSERT_NOT_REACHED();
}

void* StructureAlignedMemoryAllocator::tryReallocateMemory(void*, size_t)
{
    // Structures do not support Precise allocations right now.
    RELEASE_ASSERT_NOT_REACHED();
}

#if CPU(ADDRESS64)

class StructureMemoryManager {
public:
    StructureMemoryManager()
    {
        size_t preferredStructureHeapSize = computePreferredStructureHeapReservationSize();
        if (Options::structureHeapSizeInKB())
            preferredStructureHeapSize = static_cast<size_t>(Options::structureHeapSizeInKB()) * KB;
        RELEASE_ASSERT(hasOneBitSet(preferredStructureHeapSize));

        // We need to align the address range to mappedHeapSize to ensure that the range does not span
        // across 4GB granules. Otherwise, the top 32 bits of the address may not be constant for all
        // addresses in the range. The top 32 bits being constant is an invariant that we rely on in
        // order to encode StructureIDs.
        m_heap = ReservedRangeHeap::tryCreate({
            .name = "Structure Heap"_s,
            .size = preferredStructureHeapSize,
            .minSize = std::min<size_t>(preferredStructureHeapSize, MB),
            // Don't use the first page because zero is used as the empty StructureID and the first allocation will conflict.
            .unusedPrefixSize = MarkedBlock::blockSize,
            .addressSpaceBudget = addressSpaceToReserve() / addressSpaceDivisorForStructures,
            .usage = OSAllocator::StructureAllocatorPages,
        });
        RELEASE_ASSERT(m_heap, preferredStructureHeapSize);
        g_jscConfig.startOfStructureHeap = m_heap->start();
        uintptr_t mappedHeapSize = m_heap->size();
        RELEASE_ASSERT(g_jscConfig.startOfStructureHeap, g_jscConfig.startOfStructureHeap, preferredStructureHeapSize, mappedHeapSize);
        RELEASE_ASSERT(hasOneBitSet(mappedHeapSize), mappedHeapSize);
        uintptr_t alignmentMask = mappedHeapSize - 1;
        RELEASE_ASSERT(!(g_jscConfig.startOfStructureHeap & alignmentMask), g_jscConfig.startOfStructureHeap, mappedHeapSize, alignmentMask);
        g_jscConfig.sizeOfStructureHeap = mappedHeapSize;
        g_jscConfig.structureIDBase = g_jscConfig.startOfStructureHeap & ~StructureID::structureIDMask;
    }

    void* tryMallocStructureBlock()
    {
        return m_heap->tryAllocateAligned(MarkedBlock::blockSize, MarkedBlock::blockSize);
    }

    void freeStructureBlock(void* blockPtr)
    {
        m_heap->free(blockPtr);
    }

private:
    static size_t computePreferredStructureHeapReservationSize();

    // The actual preferred size will be the next power-of-two below this value
    // This value results in a 512MiB reservation on 3GiB devices, 1GiB on 4GiB
    static constexpr size_t maximumPercentageOfPhysicalMemoryToReserveWhenVAConstrained = 20;
    ReservedRangeHeap* m_heap { nullptr };
};

static LazyNeverDestroyed<StructureMemoryManager> s_structureMemoryManager;

void* StructureAlignedMemoryAllocator::tryAllocateAlignedMemory(size_t alignment, size_t size)
{
    ASSERT_UNUSED(alignment, alignment == MarkedBlock::blockSize);
    ASSERT_UNUSED(size, size == MarkedBlock::blockSize);
    return s_structureMemoryManager->tryMallocStructureBlock();
}

void StructureAlignedMemoryAllocator::freeAlignedMemory(void* block)
{
    s_structureMemoryManager->freeStructureBlock(block);
}

void StructureAlignedMemoryAllocator::initializeStructureAddressSpace()
{
    s_structureMemoryManager.construct();
}

size_t StructureMemoryManager::computePreferredStructureHeapReservationSize()
{
    static size_t cachedSize;
    static std::once_flag onceKey;
    std::call_once(onceKey, [] {
        size_t baseSize { 0 };
#if PLATFORM(PLAYSTATION) || OS(QNX)
        baseSize = 128 * MB;
#elif (PLATFORM(IOS_FAMILY) && !CPU(ARM64E)) || PLATFORM(WATCHOS) || PLATFORM(APPLETV)
        baseSize = 512 * MB;
#elif PLATFORM(IOS_FAMILY)
        baseSize = 2 * GB;
#else
        baseSize = 4 * GB;
#endif

#if PLATFORM(IOS_FAMILY)
        // For larger reservation sizes, its backing mapping can
        // monopolize a significant fraction of all virtual memory.
        // Preferably, we only want to do so when we have reason to expect that
        // this reservation may actually be utilized. On systems with relatively
        // little physical memory available vs. the size of the reservation, we
        // should thus reserve less virtual memory to not waste it unnecessarily.

        size_t physicalMemorySize = WTF::ramSizeDisregardingJetsamLimit();
        RELEASE_ASSERT(physicalMemorySize);

        bool isVaConstrained = !WTF::processHasEntitlement("com.apple.developer.kernel.extended-virtual-addressing");
        if (isVaConstrained) {
            size_t sizeBoundedByPhysicalMemory = std::min(baseSize, (physicalMemorySize * maximumPercentageOfPhysicalMemoryToReserveWhenVAConstrained) / 100);
            RELEASE_ASSERT(sizeBoundedByPhysicalMemory);
            cachedSize = std::bit_floor(sizeBoundedByPhysicalMemory);
        } else
            cachedSize = std::bit_floor(baseSize);
#else
        cachedSize = baseSize;
#endif

#if CPU(ADDRESS64)
        RELEASE_ASSERT(cachedSize - 1 <= StructureID::structureIDMask, "StructureID relies on only the lower 32 bits of Structure addresses varying");
#endif
    });
    return cachedSize;
}

#else // not CPU(ADDRESS64)

void StructureAlignedMemoryAllocator::initializeStructureAddressSpace()
{
    g_jscConfig.startOfStructureHeap = 0;
    g_jscConfig.structureIDBase = 0;
    g_jscConfig.sizeOfStructureHeap = UINTPTR_MAX;
}

void* StructureAlignedMemoryAllocator::tryAllocateAlignedMemory(size_t alignment, size_t size)
{
    ASSERT_UNUSED(alignment, alignment == MarkedBlock::blockSize);
    ASSERT_UNUSED(size, size == MarkedBlock::blockSize);
    return tryFastCompactAlignedMalloc(MarkedBlock::blockSize, MarkedBlock::blockSize);
}

void StructureAlignedMemoryAllocator::freeAlignedMemory(void* block)
{
    fastFree(block);
}

#endif // CPU(ADDRESS64)

} // namespace JSC
