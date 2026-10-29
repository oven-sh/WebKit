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
#include <bmalloc/bmalloc.h>
#include <wtf/BitVector.h>
#include <wtf/RAMSize.h>
#include <wtf/Vector.h>

#if OS(DARWIN)
#include <wtf/cocoa/Entitlements.h>
#endif
#if CPU(ADDRESS64)
#include <wtf/NeverDestroyed.h>
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#if USE(LIBPAS)
#include <bmalloc/bmalloc_heap.h>
#include <bmalloc/bmalloc_heap_config.h>
#include <bmalloc/bmalloc_heap_inlines.h>
#include <bmalloc/bmalloc_heap_ref.h>
#include <bmalloc/pas_primitive_heap_ref.h>
#elif USE(MIMALLOC)
#include <bmalloc/mimalloc.h>
#if OS(LINUX)
#include <sys/mman.h>
#include <errno.h>
#elif OS(DARWIN)
#include <mach/mach.h>
#endif
#endif
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
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
#if USE(LIBPAS)

static const bmalloc_type structureHeapType { BMALLOC_TYPE_INITIALIZER(MarkedBlock::blockSize, MarkedBlock::blockSize, "Structure Heap") };
static pas_primitive_heap_ref structureHeap { BMALLOC_AUXILIARY_HEAP_REF_INITIALIZER(&structureHeapType) };

#elif USE(MIMALLOC)

static mi_arena_id_t structureArena { };
static mi_heap_t* structureHeap { };

#endif

class StructureMemoryManager {
public:
    StructureMemoryManager()
    {
        size_t preferredStructureHeapSize = computePreferredStructureHeapReservationSize();
        if (Options::structureHeapSizeInKB())
            preferredStructureHeapSize = static_cast<size_t>(Options::structureHeapSizeInKB()) * KB;
        RELEASE_ASSERT(hasOneBitSet(preferredStructureHeapSize));

        uintptr_t mappedHeapSize = preferredStructureHeapSize;
        if (Options::runsCompiledImage() && !Options::structureHeapSizeInKB())
            mappedHeapSize = structureHeapSizeWithinBudget(mappedHeapSize);
#if OS(LINUX) && defined(BUN_HARDENED)
        bool drawsWindow = true;
#else
        bool drawsWindow = Options::runsCompiledImage();
#endif
        for (unsigned i = 0; i < 8; ++i) {
            // We need to align the address range to mappedHeapSize to ensure that the range does not span
            // across 4GB granules. Otherwise, the top 32 bits of the address may not be constant for all
            // addresses in the range. The top 32 bits being constant is an invariant that we rely on in
            // order to encode StructureIDs.
            if (drawsWindow) {
                g_jscConfig.startOfStructureHeap = std::bit_cast<uintptr_t>(ReservedRangeHeap::tryReserveRandomWindow(mappedHeapSize, mappedHeapSize, OSAllocator::StructureAllocatorPages, true));
                if (g_jscConfig.startOfStructureHeap)
                    break;
            }
            g_jscConfig.startOfStructureHeap = reinterpret_cast<uintptr_t>(OSAllocator::tryReserveUncommittedAligned(mappedHeapSize, mappedHeapSize, OSAllocator::StructureAllocatorPages));
            if (g_jscConfig.startOfStructureHeap)
                break;
            mappedHeapSize /= 2;
        }
        RELEASE_ASSERT(g_jscConfig.startOfStructureHeap, g_jscConfig.startOfStructureHeap, preferredStructureHeapSize, mappedHeapSize);
        RELEASE_ASSERT(hasOneBitSet(mappedHeapSize), mappedHeapSize);
        uintptr_t alignmentMask = mappedHeapSize - 1;
        RELEASE_ASSERT(!(g_jscConfig.startOfStructureHeap & alignmentMask), g_jscConfig.startOfStructureHeap, mappedHeapSize, alignmentMask);
        g_jscConfig.sizeOfStructureHeap = mappedHeapSize;
        g_jscConfig.structureIDBase = g_jscConfig.startOfStructureHeap & ~StructureID::structureIDMask;

        // Don't use the first page because zero is used as the empty StructureID and the first allocation will conflict.
        m_useSystemHeap = !bmalloc::api::isEnabled();
#if USE(MIMALLOC)
        if (Options::runsCompiledImage() && mappedHeapSize < smallestStructureHeapSizeForMimalloc)
            m_useSystemHeap = true;
#endif
#if USE(LIBPAS)
        if (!m_useSystemHeap) [[likely]] {
#if PLATFORM(PLAYSTATION)
            // libpas isn't calling pas_page_malloc commit, so we've got to commit the region ourselves
            // https://bugs.webkit.org/show_bug.cgi?id=292771
            OSAllocator::commit((void *) g_jscConfig.startOfStructureHeap, MarkedBlock::blockSize, true, false);
#endif
            bmalloc_force_auxiliary_heap_into_reserved_memory(&structureHeap, reinterpret_cast<uintptr_t>(g_jscConfig.startOfStructureHeap) + MarkedBlock::blockSize, reinterpret_cast<uintptr_t>(g_jscConfig.startOfStructureHeap) + g_jscConfig.sizeOfStructureHeap);
            return;
        }
        m_usedBlocks.set(0);
#elif USE(MIMALLOC)
        if (!m_useSystemHeap) [[likely]] {
            void* memory = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(g_jscConfig.startOfStructureHeap) + MarkedBlock::blockSize);
            size_t size = g_jscConfig.sizeOfStructureHeap - MarkedBlock::blockSize;
            // The region is a fresh reservation, so it reads as zero once committed (is_zero). Without
            // that, mimalloc zeroes the arena's bookkeeping for all of the region's slices up front, which
            // for the 4 GB default is about 40 KB of pages touched before the first Structure exists.
            RELEASE_ASSERT(mi_manage_os_memory_ex(memory, size, false, false, true, -1, true, &structureArena));
            structureHeap = mi_heap_new_in_arena(structureArena);
#if OS(LINUX) && defined(MADV_DOFORK)
            // Undo tryReserveUncommittedAligned's MADV_DONTFORK: mimalloc stores
            // mi_arena_t and theaps inside this region and registers them in
            // process-wide lists that _mi_process_fork_child walks pre-exec.
            while (madvise(reinterpret_cast<void*>(g_jscConfig.startOfStructureHeap), g_jscConfig.sizeOfStructureHeap, MADV_DOFORK) == -1 && errno == EAGAIN) { }
#elif OS(DARWIN)
            // Undo tryReserveUncommittedAligned's mach_vm_map(..., VM_INHERIT_NONE);
            // same rationale as the Linux MADV_DOFORK branch above.
            vm_inherit(mach_task_self(), static_cast<vm_address_t>(g_jscConfig.startOfStructureHeap), static_cast<vm_size_t>(g_jscConfig.sizeOfStructureHeap), VM_INHERIT_COPY);
#endif
            return;
        }
        m_usedBlocks.set(0);
#else
        m_usedBlocks.set(0);
#endif
    }

    void* tryMallocStructureBlock()
    {
        void* block = tryMallocStructureBlockWithoutReserve();
        if (block && Options::runsCompiledImage())
            keepReserve();
        return block;
    }

    bool releaseReserve()
    {
        Vector<void*, reserveBlockCount> reserve;
        {
            Locker locker(m_reserveLock);
            reserve = std::exchange(m_reserve, { });
            m_hasReleasedReserve = true;
        }
        for (void* block : reserve)
            freeStructureBlock(block);
        return !reserve.isEmpty();
    }

    bool tryRestoreReleasedReserve()
    {
        {
            Locker locker(m_reserveLock);
            if (!m_hasReleasedReserve)
                return false;
        }
        Vector<void*, reserveBlockCount> reserve;
        while (reserve.size() < reserveBlockCount) {
            void* block = tryMallocStructureBlockWithoutReserve();
            if (!block)
                break;
            reserve.append(block);
        }
        if (reserve.size() == reserveBlockCount) {
            Locker locker(m_reserveLock);
            if (m_hasReleasedReserve) {
                m_reserve = WTF::move(reserve);
                m_hasReleasedReserve = false;
                return true;
            }
        }
        for (void* block : reserve)
            freeStructureBlock(block);
        return false;
    }

    void didThrowOutOfMemoryError()
    {
        Locker locker(m_reserveLock);
        m_hasReleasedReserve = false;
    }

    void keepReserve()
    {
        if (m_blockCount.load(std::memory_order_relaxed) * MarkedBlock::blockSize <= g_jscConfig.sizeOfStructureHeap / reserveThresholdDivisor)
            return;
        {
            Locker locker(m_reserveLock);
            if (m_hasReleasedReserve || m_reserve.size() == reserveBlockCount)
                return;
        }
        void* block = tryMallocStructureBlockWithoutReserve();
        if (!block)
            return;
        {
            Locker locker(m_reserveLock);
            if (!m_hasReleasedReserve && m_reserve.size() < reserveBlockCount) {
                m_reserve.append(block);
                return;
            }
        }
        freeStructureBlock(block);
    }

    void* tryMallocStructureBlockWithoutReserve()
    {
        void* block = tryMallocStructureBlockImpl();
        if (block)
            m_blockCount.fetch_add(1, std::memory_order_relaxed);
        return block;
    }

    void* tryMallocStructureBlockImpl()
    {
        if (!m_useSystemHeap) [[likely]] {
#if USE(LIBPAS)
            void* result = bmalloc_try_allocate_auxiliary_with_alignment_inline(&structureHeap, MarkedBlock::blockSize, MarkedBlock::blockSize);

#if PLATFORM(PLAYSTATION)
            // libpas isn't calling pas_page_malloc commit, so we've got to commit the region ourselves
            // https://bugs.webkit.org/show_bug.cgi?id=292771
            OSAllocator::commit(result, MarkedBlock::blockSize, true, false);
#endif
            return result;
#elif USE(MIMALLOC)
            return mi_heap_malloc_aligned(structureHeap, MarkedBlock::blockSize, MarkedBlock::blockSize);
#endif
        }

        size_t freeIndex;
        {
            Locker locker(m_lock);
            constexpr size_t startIndex = 0;
            freeIndex = m_usedBlocks.findBit(startIndex, 0);
            ASSERT(freeIndex <= m_usedBlocks.bitCount());
            RELEASE_ASSERT(g_jscConfig.sizeOfStructureHeap <= computePreferredStructureHeapReservationSize());
            if (freeIndex * MarkedBlock::blockSize >= g_jscConfig.sizeOfStructureHeap)
                return nullptr;
            // If we can't find a free block then `freeIndex == m_usedBlocks.bitCount()` and this set will grow the bit vector.
            m_usedBlocks.set(freeIndex);
        }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        auto* block = reinterpret_cast<uint8_t*>(g_jscConfig.startOfStructureHeap) + freeIndex * MarkedBlock::blockSize;
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        commitBlock(block);
        return block;
    }

    void freeStructureBlock(void* blockPtr)
    {
        m_blockCount.fetch_sub(1, std::memory_order_relaxed);
        if (!m_useSystemHeap) [[likely]] {
#if USE(LIBPAS)
            bmalloc_deallocate_inline(blockPtr);
            return;
#elif USE(MIMALLOC)
            mi_free(blockPtr);
            return;
#endif
        }

        decommitBlock(blockPtr);
        uintptr_t block = reinterpret_cast<uintptr_t>(blockPtr);
        RELEASE_ASSERT(g_jscConfig.startOfStructureHeap <= block && block < g_jscConfig.startOfStructureHeap + g_jscConfig.sizeOfStructureHeap);
        RELEASE_ASSERT(roundUpToMultipleOf<MarkedBlock::blockSize>(block) == block);

        Locker locker(m_lock);
        m_usedBlocks.quickClear((block - g_jscConfig.startOfStructureHeap) / MarkedBlock::blockSize);
    }

    static void commitBlock(void* block)
    {
#if OS(UNIX) && !PLATFORM(PLAYSTATION) && ASSERT_ENABLED
        constexpr bool readable = true;
        constexpr bool writable = true;
        OSAllocator::protect(block, MarkedBlock::blockSize, readable, writable);
#else
        constexpr bool writable = true;
        constexpr bool executable = false;
        OSAllocator::commit(block, MarkedBlock::blockSize, writable, executable);
#endif
    }

    static void decommitBlock(void* block)
    {
#if OS(UNIX) && !PLATFORM(PLAYSTATION) && ASSERT_ENABLED
        constexpr bool readable = false;
        constexpr bool writable = false;
        OSAllocator::protect(block, MarkedBlock::blockSize, readable, writable);
#else
        OSAllocator::decommit(block, MarkedBlock::blockSize);
#endif
    }

private:
    static size_t computePreferredStructureHeapReservationSize();

    // The actual preferred size will be the next power-of-two below this value
    // This value results in a 512MiB reservation on 3GiB devices, 1GiB on 4GiB
    static constexpr size_t maximumPercentageOfPhysicalMemoryToReserveWhenVAConstrained = 20;
    static constexpr size_t reserveBlockCount = 16;
    static constexpr size_t reserveThresholdDivisor = 4;
    Lock m_lock;
    bool m_useSystemHeap { true };
    BitVector m_usedBlocks;
    std::atomic<size_t> m_blockCount { 0 };
    Lock m_reserveLock;
    Vector<void*, reserveBlockCount> m_reserve WTF_GUARDED_BY_LOCK(m_reserveLock);
    bool m_hasReleasedReserve WTF_GUARDED_BY_LOCK(m_reserveLock) { false };
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

bool StructureAlignedMemoryAllocator::releaseReserve()
{
    return s_structureMemoryManager->releaseReserve();
}

bool StructureAlignedMemoryAllocator::tryRestoreReleasedReserve()
{
    return s_structureMemoryManager->tryRestoreReleasedReserve();
}

void StructureAlignedMemoryAllocator::didThrowOutOfMemoryError()
{
    s_structureMemoryManager->didThrowOutOfMemoryError();
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

bool StructureAlignedMemoryAllocator::releaseReserve()
{
    return false;
}

bool StructureAlignedMemoryAllocator::tryRestoreReleasedReserve()
{
    return false;
}

void StructureAlignedMemoryAllocator::didThrowOutOfMemoryError()
{
}

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
