/*
 * Copyright (C) 2017-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
#include "ReservedRangeHeap.h"

#if CPU(ADDRESS64)

#include "AddressSpaceBudget.h"
#include "MarkedBlock.h"
#include <algorithm>
#include <bmalloc/bmalloc.h>
#include <cstring>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/MathExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#if USE(LIBPAS)
#include <bmalloc/bmalloc_heap.h>
#include <bmalloc/bmalloc_heap_config.h>
#include <bmalloc/bmalloc_heap_inlines.h>
#include <bmalloc/bmalloc_heap_ref.h>
#include <bmalloc/pas_primitive_heap_ref.h>
#elif USE(MIMALLOC)
#include <bmalloc/mimalloc.h>
#endif
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#if OS(UNIX)
#include <errno.h>
#include <sys/mman.h>
#endif
#if OS(DARWIN)
#include <mach/mach.h>
#endif
#if ASAN_ENABLED && __has_include(<sanitizer/asan_interface.h>)
#include <sanitizer/asan_interface.h>
#endif
#if ASAN_ENABLED && OS(LINUX)
#include <sanitizer/lsan_interface.h>
#endif

#endif // CPU(ADDRESS64)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

#if CPU(ADDRESS64)

static constexpr size_t granuleSize = MarkedBlock::blockSize;
static constexpr size_t sizeStep = MB;
static constexpr unsigned maxReservationAttempts = 10;
static_assert(!(sizeStep % granuleSize));
// mimalloc divides a range into chunks of this size and refuses a range that holds none.
static constexpr size_t mallocChunkSize = 32 * MB;
#if OS(LINUX) && USE(MIMALLOC)
static constexpr size_t minSegmentSize = 2 * mallocChunkSize;
// malloc and the stacks of threads go on taking address space.
static constexpr size_t addressSpaceToLeaveBesideSegment = 32 * MB;
static constexpr unsigned maxSegmentPlacements = 8;
#endif
static constexpr unsigned maxRandomPlacements = 16;
// The system keeps one mapping for each stretch of granules with the same access.
static constexpr size_t maxAccessBorders = 16384;
static constexpr uintptr_t lowestRandomAddress = 64 * GB;
static constexpr uintptr_t highestRandomAddress = 124 * 1024 * GB;
#if OS(LINUX)
static constexpr bool refusalMeansNoRoomAnywhere = true;
#else
static constexpr bool refusalMeansNoRoomAnywhere = false;
#endif

static bool usesMalloc()
{
#if USE(LIBPAS) || USE(MIMALLOC)
    return bmalloc::api::isEnabled();
#else
    return false;
#endif
}

static bool canGrow(const ReservedRangeHeap::Request& request)
{
#if OS(LINUX) && USE(MIMALLOC)
    return usesMalloc() && request.initialAccess == ReservedRangeHeap::InitialAccess::None && request.initialSegmentSize && request.initialSegmentSize < request.size;
#else
    UNUSED_PARAM(request);
    return false;
#endif
}

static bool reservesWithoutAccess(const ReservedRangeHeap::Request& request)
{
#if OS(LINUX)
    if (request.initialAccess != ReservedRangeHeap::InitialAccess::None)
        return false;
#if USE(LIBPAS)
    // libpas never commits, so its memory has to be readable and writable from the start.
    if (usesMalloc())
        return false;
#endif
    return true;
#else
    UNUSED_PARAM(request);
    return false;
#endif
}

static size_t findFreeGranule(const BitVector& usedGranules, size_t position)
{
    return position < usedGranules.size() ? usedGranules.findBit(position, false) : position;
}

static size_t findUsedGranule(const BitVector& usedGranules, size_t position, size_t granuleCount)
{
    size_t result = position < usedGranules.size() ? usedGranules.findBit(position, true) : usedGranules.size();
    return result < usedGranules.size() ? result : granuleCount;
}

static void resizeGranuleBits(BitVector& bits, size_t size)
{
#if ASAN_ENABLED && OS(LINUX)
    // BitVector stores its pointer shifted, so LeakSanitizer cannot follow it. A range is never destroyed.
    __lsan::ScopedDisabler disabler;
#endif
    bits.ensureSize(size);
}

static void ensureGranuleBits(BitVector& bits, size_t end, size_t granuleCount)
{
    if (end > bits.size())
        resizeGranuleBits(bits, std::min(std::max(end, 2 * bits.size()), granuleCount));
}

static std::atomic<size_t> s_accessBorderCount { 0 };

static size_t countAccessBorders(const BitVector& accessibleGranules, size_t first, size_t count)
{
    size_t result = 0;
    for (size_t index = first ? first - 1 : 0; index < first + count; ++index)
        result += accessibleGranules.get(index) != accessibleGranules.get(index + 1);
    return result;
}

static size_t granuleIndexOf(uintptr_t start, size_t size, const void* pointer)
{
    uintptr_t offset = std::bit_cast<uintptr_t>(pointer) - start;
    RELEASE_ASSERT(offset < size && !(offset % granuleSize), offset, size);
    return offset / granuleSize;
}

ReservedRangeHeap* ReservedRangeHeap::tryCreate(const Request& request)
{
    RELEASE_ASSERT(request.minSize && !(request.size % sizeStep) && !(request.minSize % sizeStep) && request.minSize <= request.size, request.size, request.minSize);
    RELEASE_ASSERT(!request.alignment || hasOneBitSet(request.alignment), request.alignment);
    RELEASE_ASSERT(!(request.unusedPrefixSize % granuleSize) && request.unusedPrefixSize < request.minSize, request.unusedPrefixSize, request.minSize);

    bool isGrowable = canGrow(request);
    size_t size = std::max(std::min(isGrowable ? request.initialSegmentSize : request.size, request.addressSpaceBudget / sizeStep * sizeStep), request.minSize);
    for (unsigned attempt = 0; attempt < maxReservationAttempts; ++attempt) {
        size_t nextSize = std::max(size / 2 / sizeStep * sizeStep, request.minSize);
        bool hasAnotherAttempt = attempt + 1 < maxReservationAttempts && nextSize < size;
        if (uintptr_t start = tryReserve(size, request)) {
            std::optional<Backing> backing = tryCreateBacking(start, size, request);
            if (!backing && !(hasAnotherAttempt && nextSize >= request.unusedPrefixSize + mallocChunkSize))
                backing = Backing { BackEnd::Granules, nullptr, size };
            if (backing)
                return new ReservedRangeHeap(start, isGrowable && backing->backEnd == BackEnd::Malloc ? request.size : backing->size, *backing, request);
            releaseReservation(start, size);
        }
        if (!hasAnotherAttempt)
            break;
        size = nextSize;
    }
    return nullptr;
}

void* ReservedRangeHeap::tryReserveRandomWindow(size_t size, size_t alignment, OSAllocator::Usage usage, bool isWritable)
{
    uintptr_t firstWindow = (lowestRandomAddress + alignment - 1) / alignment;
    uintptr_t endWindow = highestRandomAddress / alignment;
    for (unsigned attempt = 0; attempt < maxRandomPlacements && firstWindow < endWindow; ++attempt) {
        void* requestedAddress = std::bit_cast<void*>((firstWindow + cryptographicallyRandomNumber<uint32_t>() % (endWindow - firstWindow)) * alignment);
#if OS(DARWIN)
        void* reserved = OSAllocator::tryReserveUncommittedAligned(size, alignment, usage, requestedAddress, isWritable);
#elif OS(FREEBSD)
        // Without these flags the system adds a distance of its own to the address it is asked for.
        UNUSED_PARAM(usage);
        void* reserved = mmap(requestedAddress, size, isWritable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_EXCL, -1, 0);
        if (reserved == MAP_FAILED)
            reserved = nullptr;
#else
        void* reserved = OSAllocator::tryReserveUncommitted(size, usage, requestedAddress, isWritable);
#endif
        if (!reserved) {
            if (refusalMeansNoRoomAnywhere)
                return nullptr;
            continue;
        }
        if (reserved == requestedAddress)
            return reserved;
        OSAllocator::releaseDecommitted(reserved, size, 0);
        // A system that fills its address space from the top answers below the wish where the wish lies beyond the end.
        if (reserved < requestedAddress)
            endWindow = std::min(endWindow, std::bit_cast<uintptr_t>(reserved) / alignment + 1);
    }
    return nullptr;
}

uintptr_t ReservedRangeHeap::tryReserve(size_t size, const Request& request)
{
    size_t alignment = std::max(request.alignment, roundUpToPowerOfTwo(size));
    bool isWritable = !reservesWithoutAccess(request);
    void* address = nullptr;
    if (request.placement == Placement::RandomWindow)
        address = tryReserveRandomWindow(size, alignment, request.usage, isWritable);
    if (!address)
        address = OSAllocator::tryReserveUncommittedAligned(size, alignment, request.usage, nullptr, isWritable);
    if (!address)
        return 0;
#if OS(UNIX)
    if (request.initialAccess == InitialAccess::None) {
        size_t inaccessibleSize = isWritable ? request.unusedPrefixSize : size;
        RELEASE_ASSERT(!inaccessibleSize || !mprotect(address, inaccessibleSize, PROT_NONE));
    }
#endif
    return std::bit_cast<uintptr_t>(address);
}

void ReservedRangeHeap::releaseReservation(uintptr_t start, size_t size)
{
#if OS(WINDOWS)
    // An aligned reservation cannot be released on Windows.
    UNUSED_PARAM(start);
    UNUSED_PARAM(size);
#else
    OSAllocator::releaseDecommitted(std::bit_cast<void*>(start), size, 0);
#endif
}

auto ReservedRangeHeap::tryCreateBacking(uintptr_t start, size_t size, const Request& request) -> std::optional<Backing>
{
    if (!usesMalloc())
        return Backing { BackEnd::Granules, nullptr, size };

#if USE(LIBPAS)
    auto* type = new (NotNull, fastMalloc(sizeof(bmalloc_type))) bmalloc_type(BMALLOC_TYPE_INITIALIZER(1, 1, request.name.characters()));
    auto* heapRef = new (NotNull, fastMalloc(sizeof(pas_primitive_heap_ref))) pas_primitive_heap_ref(BMALLOC_AUXILIARY_HEAP_REF_INITIALIZER(type));
#if PLATFORM(PLAYSTATION)
    // libpas isn't calling pas_page_malloc commit, so we've got to commit the region ourselves
    // https://bugs.webkit.org/show_bug.cgi?id=292771
    OSAllocator::commit(std::bit_cast<void*>(start), granuleSize, true, false);
#endif
    bmalloc_force_auxiliary_heap_into_reserved_memory(heapRef, start + request.unusedPrefixSize, start + size);
    return Backing { BackEnd::Malloc, heapRef, size };
#elif USE(MIMALLOC)
    // The region is a fresh reservation, so it reads as zero once committed (is_zero). Without
    // that, mimalloc zeroes the arena's bookkeeping for all of the region's slices up front, which
    // for a 4 GB region is about 40 KB of pages touched before the first allocation.
    mi_arena_id_t arena { };
    if (!mi_manage_os_memory_ex(std::bit_cast<void*>(start + request.unusedPrefixSize), size - request.unusedPrefixSize, false, false, true, -1, true, &arena))
        return std::nullopt;
    mi_heap_t* heap = mi_heap_new_in_arena(arena);
    RELEASE_ASSERT(heap);
#if OS(LINUX) && defined(MADV_DOFORK)
    // Undo tryReserveUncommittedAligned's MADV_DONTFORK: mimalloc stores
    // mi_arena_t and theaps inside this region and registers them in
    // process-wide lists that _mi_process_fork_child walks pre-exec.
    while (madvise(std::bit_cast<void*>(start), size, MADV_DOFORK) == -1 && errno == EAGAIN) { }
#elif OS(DARWIN)
    // Undo tryReserveUncommittedAligned's mach_vm_map(..., VM_INHERIT_NONE);
    // same rationale as the Linux MADV_DOFORK branch above.
    vm_inherit(mach_task_self(), static_cast<vm_address_t>(start), static_cast<vm_size_t>(size), VM_INHERIT_COPY);
#endif
    return Backing { BackEnd::Malloc, heap, size };
#else
    UNUSED_PARAM(start);
    UNUSED_PARAM(request);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

ReservedRangeHeap::ReservedRangeHeap(uintptr_t start, size_t size, const Backing& backing, const Request& request)
    : m_start(start)
    , m_size(size)
    , m_initialSegmentSize(backing.size)
    , m_backEnd(backing.backEnd)
    , m_isGrowable(size > backing.size)
    , m_usage(request.usage)
    , m_name(request.name)
    , m_mallocHeap(backing.mallocHeap)
    , m_reservedSize(backing.size)
{
    RELEASE_ASSERT((m_backEnd == BackEnd::Malloc) == !!backing.mallocHeap);
    RELEASE_ASSERT(!m_isGrowable || m_backEnd == BackEnd::Malloc);
    if (m_backEnd != BackEnd::Granules) {
        Locker locker { m_lock };
        m_segmentHeaps.append(backing.mallocHeap);
        m_segmentsEnd = backing.size;
        return;
    }

    size_t prefixGranuleCount = request.unusedPrefixSize / granuleSize;
    resizeGranuleBits(m_usedGranules, prefixGranuleCount);
    for (size_t index = 0; index < prefixGranuleCount; ++index)
        m_usedGranules.quickSet(index);
    m_firstGranuleToSearch = prefixGranuleCount;
}

void* ReservedRangeHeap::tryAllocateAligned(size_t alignment, size_t size)
{
#if USE(LIBPAS)
    if (m_backEnd == BackEnd::Malloc) [[likely]] {
        void* result = bmalloc_try_allocate_auxiliary_with_alignment_inline(static_cast<pas_primitive_heap_ref*>(m_mallocHeap.load(std::memory_order_relaxed)), size, alignment);
#if PLATFORM(PLAYSTATION)
        // libpas isn't calling pas_page_malloc commit, so we've got to commit the region ourselves
        // https://bugs.webkit.org/show_bug.cgi?id=292771
        if (result)
            OSAllocator::commit(result, size, true, false);
#endif
        return result;
    }
#elif USE(MIMALLOC)
    if (m_backEnd == BackEnd::Malloc) [[likely]] {
        void* result = mi_heap_malloc_aligned(static_cast<mi_heap_t*>(m_mallocHeap.load(std::memory_order_acquire)), size, alignment);
        if (!result && m_isGrowable) [[unlikely]]
            return tryAllocateSlow(alignment, size);
        return result;
    }
#endif
    RELEASE_ASSERT(alignment <= granuleSize, alignment);
    return tryAllocateGranules(size);
}

void* ReservedRangeHeap::tryAllocate(size_t size)
{
#if USE(LIBPAS)
    if (m_backEnd == BackEnd::Malloc) [[likely]]
        return bmalloc_try_allocate_auxiliary_inline(static_cast<pas_primitive_heap_ref*>(m_mallocHeap.load(std::memory_order_relaxed)), size);
#elif USE(MIMALLOC)
    if (m_backEnd == BackEnd::Malloc) [[likely]] {
        void* result = mi_heap_malloc(static_cast<mi_heap_t*>(m_mallocHeap.load(std::memory_order_acquire)), size);
        if (!result && m_isGrowable) [[unlikely]]
            return tryAllocateSlow(0, size);
        return result;
    }
#endif
    return tryAllocateGranules(size);
}

void* ReservedRangeHeap::tryReallocate(void* pointer, size_t size)
{
#if USE(LIBPAS)
    if (m_backEnd == BackEnd::Malloc) [[likely]]
        return bmalloc_try_reallocate_auxiliary_inline(pointer, static_cast<pas_primitive_heap_ref*>(m_mallocHeap.load(std::memory_order_relaxed)), size, pas_reallocate_free_if_successful);
#elif USE(MIMALLOC)
    if (m_backEnd == BackEnd::Malloc) [[likely]] {
        void* result = mi_heap_realloc(static_cast<mi_heap_t*>(m_mallocHeap.load(std::memory_order_acquire)), pointer, size);
        if (result || !m_isGrowable) [[likely]]
            return result;
        result = tryAllocateSlow(0, size);
        if (!result)
            return nullptr;
        memcpy(result, pointer, std::min(size, mi_usable_size(pointer)));
        mi_free(pointer);
        return result;
    }
#endif
    size_t first = granuleIndexOf(m_start, m_size, pointer);
    size_t oldSize;
    {
        Locker locker { m_lock };
        RELEASE_ASSERT(m_firstGranulesOfRuns.get(first), first);
        oldSize = granuleCountOfRun(first) * granuleSize;
    }
    if (size <= oldSize)
        return pointer;

    void* result = tryAllocateGranules(size);
    if (!result)
        return nullptr;
    memcpy(result, pointer, oldSize);
    freeGranules(pointer);
    return result;
}

void ReservedRangeHeap::free(void* pointer)
{
#if USE(LIBPAS)
    if (m_backEnd == BackEnd::Malloc) [[likely]] {
        bmalloc_deallocate_inline(pointer);
        return;
    }
#elif USE(MIMALLOC)
    if (m_backEnd == BackEnd::Malloc) [[likely]] {
        mi_free(pointer);
        return;
    }
#endif
    freeGranules(pointer);
}

void ReservedRangeHeap::releaseFreeMemory()
{
    // The scavenger of libpas does this by itself, and a freed run of granules is decommitted at once.
#if USE(MIMALLOC)
    if (m_backEnd == BackEnd::Malloc) {
        Locker locker { m_lock };
        for (void* heap : m_segmentHeaps)
            mi_heap_collect(static_cast<mi_heap_t*>(heap), true);
    }
#endif
}

void* ReservedRangeHeap::tryAllocateSlow(size_t alignment, size_t size)
{
#if OS(LINUX) && USE(MIMALLOC)
    auto tryAllocateIn = [&](void* heap) {
        return alignment ? mi_heap_malloc_aligned(static_cast<mi_heap_t*>(heap), size, alignment) : mi_heap_malloc(static_cast<mi_heap_t*>(heap), size);
    };

    Locker locker { m_lock };
    for (size_t index = m_segmentHeaps.size(); index--;) {
        if (void* result = tryAllocateIn(m_segmentHeaps[index])) {
            m_mallocHeap.store(m_segmentHeaps[index], std::memory_order_release);
            return result;
        }
    }
    if (!tryAddSegment(size + alignment))
        return nullptr;
    m_mallocHeap.store(m_segmentHeaps.last(), std::memory_order_release);
    return tryAllocateIn(m_segmentHeaps.last());
#else
    UNUSED_PARAM(alignment);
    UNUSED_PARAM(size);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

bool ReservedRangeHeap::tryAddSegment(size_t allocationSize)
{
#if OS(LINUX) && USE(MIMALLOC)
    size_t minSize = std::max(minSegmentSize, roundUpToMultipleOf<mallocChunkSize>(allocationSize) + mallocChunkSize);
    size_t size = std::max(minSize, roundUpToMultipleOf<mallocChunkSize>(reservedSize() / 2));
    size_t left = addressSpaceLeft();
    if (left < minSize + addressSpaceToLeaveBesideSegment)
        return false;
    size = std::min(size, (left - addressSpaceToLeaveBesideSegment) / mallocChunkSize * mallocChunkSize);
    Request request { .name = m_name, .usage = m_usage, .initialAccess = InitialAccess::None };
    unsigned placements = 0;
    while (true) {
        size_t offset = roundUpToMultipleOf<mallocChunkSize>(m_segmentsEnd);
        if (offset >= m_size || m_size - offset < minSize)
            return false;
        size = std::min(size, (m_size - offset) / mallocChunkSize * mallocChunkSize);

        void* address = std::bit_cast<void*>(m_start + offset);
        void* reserved = OSAllocator::tryReserveUncommitted(size, m_usage, address, false);
        if (!reserved) {
            size = size / 2 / mallocChunkSize * mallocChunkSize;
            if (size < minSize)
                return false;
            continue;
        }
        if (reserved != address) {
            OSAllocator::releaseDecommitted(reserved, size, 0);
            if (++placements == maxSegmentPlacements)
                return false;
            m_segmentsEnd = offset + size;
            continue;
        }
        RELEASE_ASSERT(!mprotect(address, size, PROT_NONE));

        std::optional<Backing> backing = tryCreateBacking(m_start + offset, size, request);
        if (!backing) {
            releaseReservation(m_start + offset, size);
            return false;
        }
        m_segmentHeaps.append(backing->mallocHeap);
        m_segmentsEnd = offset + size;
        m_reservedSize.fetch_add(size, std::memory_order_relaxed);
        return true;
    }
#else
    UNUSED_PARAM(allocationSize);
    UNUSED_PARAM(m_usage);
    UNUSED_PARAM(m_name);
    return false;
#endif
}

void* ReservedRangeHeap::tryAllocateGranules(size_t size)
{
    if (size > m_size)
        return nullptr;
    size_t count = std::max<size_t>((size + granuleSize - 1) / granuleSize, 1);
    size_t granuleCount = m_size / granuleSize;

    size_t first;
    {
        Locker locker { m_lock };
        m_firstGranuleToSearch = findFreeGranule(m_usedGranules, m_firstGranuleToSearch);
        first = m_firstGranuleToSearch;
        while (true) {
            if (first + count > granuleCount)
                return nullptr;
            size_t next = findUsedGranule(m_usedGranules, first, granuleCount);
            if (next - first >= count)
                break;
            first = findFreeGranule(m_usedGranules, next);
        }

        ensureGranuleBits(m_usedGranules, first + count, granuleCount);
        ensureGranuleBits(m_firstGranulesOfRuns, first + 1, granuleCount);
        for (size_t index = first; index < first + count; ++index)
            m_usedGranules.quickSet(index);
        m_firstGranulesOfRuns.quickSet(first);
        if (first == m_firstGranuleToSearch)
            m_firstGranuleToSearch += count;
        ensureGranuleBits(m_accessibleGranules, first + count, granuleCount);
        setAccess(first, count, true);
    }

    void* result = std::bit_cast<void*>(m_start + first * granuleSize);
    commitGranules(result, count * granuleSize);
#if ASAN_ENABLED
    __asan_unpoison_memory_region(result, count * granuleSize);
#endif
    return result;
}

void ReservedRangeHeap::freeGranules(void* pointer)
{
    size_t first = granuleIndexOf(m_start, m_size, pointer);
    size_t count;
    bool removesAccess;
    {
        Locker locker { m_lock };
        RELEASE_ASSERT(m_firstGranulesOfRuns.get(first), first);
        count = granuleCountOfRun(first);
        bool addsBorders = first && m_accessibleGranules.get(first - 1) && m_accessibleGranules.get(first + count);
        removesAccess = !addsBorders || s_accessBorderCount.load(std::memory_order_relaxed) < maxAccessBorders;
        if (removesAccess)
            setAccess(first, count, false);
    }

    // The run stays marked as used until it is decommitted, so that nobody is handed it meanwhile.
#if ASAN_ENABLED
    __asan_poison_memory_region(pointer, count * granuleSize);
#endif
    decommitGranules(pointer, count * granuleSize, removesAccess);

    Locker locker { m_lock };
    m_firstGranulesOfRuns.quickClear(first);
    for (size_t index = first; index < first + count; ++index)
        m_usedGranules.quickClear(index);
    m_firstGranuleToSearch = std::min(m_firstGranuleToSearch, first);
}

void ReservedRangeHeap::setAccess(size_t first, size_t count, bool isAccessible)
{
    size_t bordersBefore = countAccessBorders(m_accessibleGranules, first, count);
    for (size_t index = first; index < first + count; ++index)
        m_accessibleGranules.quickSet(index, isAccessible);
    s_accessBorderCount.fetch_add(countAccessBorders(m_accessibleGranules, first, count) - bordersBefore, std::memory_order_relaxed);
}

size_t ReservedRangeHeap::granuleCountOfRun(size_t firstGranule)
{
    size_t count = 1;
    while (m_usedGranules.get(firstGranule + count) && !m_firstGranulesOfRuns.get(firstGranule + count))
        ++count;
    return count;
}

void ReservedRangeHeap::commitGranules(void* address, size_t size)
{
#if OS(UNIX) && !PLATFORM(PLAYSTATION)
    OSAllocator::protect(address, size, true, true);
#endif
    OSAllocator::commit(address, size, true, false);
}

void ReservedRangeHeap::decommitGranules(void* address, size_t size, bool removesAccess)
{
    OSAllocator::decommit(address, size);
#if OS(UNIX) && !PLATFORM(PLAYSTATION)
    if (removesAccess)
        OSAllocator::protect(address, size, false, false);
#else
    UNUSED_PARAM(removesAccess);
#endif
}

#else // not CPU(ADDRESS64)

ReservedRangeHeap* ReservedRangeHeap::tryCreate(const Request&)
{
    return nullptr;
}

#endif // CPU(ADDRESS64)

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
