/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "MarkedBlockCage.h"

#include "AOTCellFormat.h"
#include "AddressSpaceBudget.h"
#include "MarkedBlock.h"
#include "Options.h"
#include "OwedOutOfMemoryError.h"
#include "ReservedRangeHeap.h"
#include "StructureAlignedMemoryAllocator.h"
#include <mutex>
#include <optional>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/DataLog.h>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

namespace JSC {

static_assert(1 << MarkedBlockCage::shift <= MarkedBlock::atomSize);
static_assert(hasOneBitSet(MarkedBlockCage::maxSize));
static_assert(!((static_cast<uint64_t>(MarkedBlockCage::minIndex) << MarkedBlockCage::shift) % MarkedBlock::blockSize));

static constexpr size_t unusedPrefixSize = static_cast<size_t>(MarkedBlockCage::minIndex) << MarkedBlockCage::shift;
static constexpr size_t smallestUsableSize = 4 * unusedPrefixSize;
static constexpr size_t randomPrefixDivisor = 32;
static constexpr size_t randomPrefixGranule = 64 * KB;
static constexpr unsigned maxHalvingsOfFirstRange = 7;
static constexpr size_t reserveDivisor = 64;
static constexpr size_t smallestReserveSize = 4 * MB;
static constexpr size_t largestReserveSize = 32 * MB;
static constexpr size_t reserveChunkSize = MarkedBlock::blockSize;
static constexpr size_t lastReserveDivisor = 4;

static_assert(!(randomPrefixGranule % MarkedBlock::blockSize));
static_assert(smallestUsableSize / randomPrefixDivisor >= randomPrefixGranule);
static_assert(unusedPrefixSize + (MarkedBlockCage::maxSize >> maxHalvingsOfFirstRange) / randomPrefixDivisor < smallestUsableSize);

std::atomic<bool> MarkedBlockCage::s_isRequired { false };
std::atomic<bool> MarkedBlockCage::s_hasAllocationsOutside { false };
std::atomic<MarkedBlockCage::ExhaustionHandler> MarkedBlockCage::s_exhaustionHandler { nullptr };
std::atomic<MarkedBlockCage::LastReserveHandler> MarkedBlockCage::s_lastReserveHandler { nullptr };
static Lock s_poolLock;
static LazyNeverDestroyed<Vector<ReservedRangeHeap*>> s_freeRanges WTF_GUARDED_BY_LOCK(s_poolLock);
static ReservedRangeHeap* s_firstRange WTF_GUARDED_BY_LOCK(s_poolLock);
static size_t s_totalReservedSize WTF_GUARDED_BY_LOCK(s_poolLock);
static std::once_flag s_initializeOnce;
static std::optional<MarkedBlockCage::Error> s_initializationError;
static MarkedBlockCage::ReservationFailure s_reservationFailure;
static bool s_isGrowable;

#if ENABLE(AOT)
static void makeEmptyExtensionReadable(uintptr_t start)
{
    static constexpr size_t largestPageSize = 64 * KB;
    static_assert(!(AOT::offsetOfEmptyExtensionInCage % largestPageSize) && AOT::offsetOfEmptyExtensionInCage + largestPageSize <= unusedPrefixSize);
    RELEASE_ASSERT(pageSize() <= largestPageSize);
    void* page = std::bit_cast<void*>(start + AOT::offsetOfEmptyExtensionInCage);
#if OS(UNIX) && !PLATFORM(PLAYSTATION)
    OSAllocator::protect(page, pageSize(), true, false);
#endif
    OSAllocator::commit(page, pageSize(), false, false);
}
#endif

static size_t remainingAddressSpace() WTF_REQUIRES_LOCK(s_poolLock)
{
    size_t budget = addressSpaceForMarkedBlockCages();
    return budget > s_totalReservedSize ? budget - s_totalReservedSize : 0;
}

static ReservedRangeHeap* tryCreateRange(size_t size, size_t minSize, size_t randomPrefixRange, size_t initialSegmentSize = 0) WTF_REQUIRES_LOCK(s_poolLock)
{
    RELEASE_ASSERT(minSize >= smallestUsableSize && minSize <= size, minSize, size);
    RELEASE_ASSERT(randomPrefixRange >= randomPrefixGranule && unusedPrefixSize + randomPrefixRange < minSize, randomPrefixRange, minSize);

    size_t remaining = remainingAddressSpace();
    if (minSize > remaining)
        return nullptr;

    uint64_t random = Options::seedOfVMRandomForFuzzer() ? Options::seedOfVMRandomForFuzzer() : cryptographicallyRandomNumber<uint64_t>();
    size_t prefixSize = unusedPrefixSize + random % (randomPrefixRange / randomPrefixGranule) * randomPrefixGranule;

    ReservedRangeHeap* range = ReservedRangeHeap::tryCreate({
        .name = "Cell Cage"_s,
        .size = size,
        .minSize = minSize,
        .initialSegmentSize = initialSegmentSize,
        .alignment = MarkedBlockCage::maxSize,
        .unusedPrefixSize = prefixSize,
        .addressSpaceBudget = remaining,
        .usage = OSAllocator::FastMallocPages,
        .placement = ReservedRangeHeap::Placement::RandomWindow,
        .initialAccess = ReservedRangeHeap::InitialAccess::None,
    });
    if (!range)
        return nullptr;

    RELEASE_ASSERT(!(range->start() & (MarkedBlockCage::maxSize - 1)), range->start(), range->size());
    s_totalReservedSize += range->reservedSize();
#if ENABLE(AOT)
    makeEmptyExtensionReadable(range->start());
#endif
    return range;
}

std::expected<void, MarkedBlockCage::Error> MarkedBlockCage::initialize()
{
    std::call_once(s_initializeOnce, [] {
        RELEASE_ASSERT(!g_jscConfig.initializeHasBeenCalled);
        if (!Options::useAOT() || !Options::markedBlockCageSizeInMB())
            return;
        size_t size = static_cast<size_t>(Options::markedBlockCageSizeInMB()) * MB;
        if (!hasOneBitSet(size) || size > maxSize) {
            s_initializationError = Error::InvalidSize;
            return;
        }

        Locker locker { s_poolLock };
        s_freeRanges.construct();
        if (size < smallestUsableSize) {
            g_jscConfig.markedBlockCageSize = size;
            return;
        }
        size_t randomPrefixRange = std::max(size >> maxHalvingsOfFirstRange, smallestUsableSize) / randomPrefixDivisor;
        if (hasAddressSpaceLimit())
            s_firstRange = tryCreateRange(std::clamp(availableAddressSpace() / MB * MB, smallestUsableSize, size), smallestUsableSize, randomPrefixRange, initialCageSegmentSize(smallestUsableSize));
        else
            s_firstRange = tryCreateRange(size, smallestUsableSize, randomPrefixRange);
        if (!s_firstRange) {
            s_initializationError = Error::ReservationFailed;
            s_reservationFailure = { std::min(size, remainingAddressSpace()), smallestUsableSize };
            return;
        }
        s_freeRanges->append(s_firstRange);
        s_isGrowable = s_firstRange->size() > s_firstRange->initialSegmentSize();
        g_jscConfig.markedBlockCageSize = s_firstRange->size();
    });
    if (s_initializationError)
        return makeUnexpected(*s_initializationError);
    return { };
}

MarkedBlockCage::MarkedBlockCage()
{
    if (!isEnabled())
        return;

    Locker locker { s_poolLock };
    if (!s_firstRange)
        return;

    if (!s_freeRanges->isEmpty())
        m_range = s_freeRanges->takeLast();
    else if (s_isGrowable)
        m_range = tryCreateRange(g_jscConfig.markedBlockCageSize, smallestUsableSize, smallestUsableSize / randomPrefixDivisor, smallestUsableSize);
    else
        m_range = tryCreateRange(g_jscConfig.markedBlockCageSize, g_jscConfig.markedBlockCageSize, g_jscConfig.markedBlockCageSize / randomPrefixDivisor);
    if (!m_range) {
        m_range = s_firstRange;
        m_sharesRange = true;
    }
    m_base = m_range->start();
    m_size = m_range->size();
}

MarkedBlockCage::~MarkedBlockCage()
{
    releaseLastReserve();
    if (!m_range || m_sharesRange)
        return;

    m_range->releaseFreeMemory();
    Locker locker { s_poolLock };
    s_freeRanges->append(m_range);
}

void* MarkedBlockCage::tryAllocateAligned(size_t alignment, size_t size)
{
    if (!m_range)
        return nullptr;
    void* result = m_range->tryAllocateAligned(alignment, size);
    if (result)
        didAllocate(result, size);
    return result;
}

void* MarkedBlockCage::tryAllocate(size_t size)
{
    if (!m_range)
        return nullptr;
    void* result = m_range->tryAllocate(size);
    if (result)
        didAllocate(result, size);
    return result;
}

void* MarkedBlockCage::tryReallocate(void* pointer, size_t size)
{
    if (!m_range)
        return nullptr;
    void* result = m_range->tryReallocate(pointer, size);
    if (result)
        didAllocate(result, size);
    return result;
}

void MarkedBlockCage::free(void* pointer)
{
    m_range->free(pointer);
}

size_t MarkedBlockCage::reserveSize() const
{
    return m_reserve.size() * reserveChunkSize;
}

void MarkedBlockCage::keepReserveOnceHalfFull()
{
#if CPU(X86_64) || CPU(ARM64)
    if (!m_range || m_hasReleasedReserve)
        return;
    size_t size = std::clamp(m_range->reservedSize() / reserveDivisor, smallestReserveSize, largestReserveSize);
    if (reserveSize() >= size || m_endOfHighestAllocation.load(std::memory_order_relaxed) <= m_base + m_range->initialSegmentSize() / 2)
        return;
    if (void* chunk = m_range->tryAllocateAligned(MarkedBlock::blockSize, reserveChunkSize))
        m_reserve.append(chunk);
#endif
}

bool MarkedBlockCage::releaseReserve()
{
    size_t numberOfChunksKept = m_reserve.size() / lastReserveDivisor;
    if (m_hasReleasedReserve || m_reserve.size() == numberOfChunksKept)
        return false;
    while (m_reserve.size() > numberOfChunksKept)
        m_range->free(m_reserve.takeLast());
    m_hasReleasedReserve = true;
    return true;
}

bool MarkedBlockCage::releaseLastReserve()
{
    if (m_reserve.isEmpty())
        return false;
    for (void* chunk : std::exchange(m_reserve, { }))
        m_range->free(chunk);
    m_hasReleasedReserve = true;
    return true;
}

void MarkedBlockCage::didAllocate(const void* pointer, size_t size)
{
    uintptr_t begin = std::bit_cast<uintptr_t>(pointer);
    uintptr_t end = begin + size;

    uintptr_t lowest = m_lowestAllocation.load(std::memory_order_relaxed);
    while (begin < lowest && !m_lowestAllocation.compare_exchange_weak(lowest, begin, std::memory_order_relaxed)) { }

    uintptr_t endOfHighest = m_endOfHighestAllocation.load(std::memory_order_relaxed);
    while (end > endOfHighest && !m_endOfHighestAllocation.compare_exchange_weak(endOfHighest, end, std::memory_order_relaxed)) { }
}

MarkedBlockCage::Indices MarkedBlockCage::usedIndices() const
{
    constexpr uintptr_t bytesPerIndex = 1 << shift;
    uintptr_t lowest = m_lowestAllocation.load(std::memory_order_relaxed);
    uintptr_t end = m_endOfHighestAllocation.load(std::memory_order_relaxed);
    if (lowest >= end)
        return { };
    uint64_t count = (end - lowest + bytesPerIndex - 1) >> shift;
    return { m_base, static_cast<uint32_t>((lowest - m_base) >> shift), static_cast<uint32_t>(std::min<uint64_t>(count, std::numeric_limits<uint32_t>::max())) };
}

std::expected<void, MarkedBlockCage::Error> MarkedBlockCage::require()
{
    if (!isEnabled())
        return makeUnexpected(s_initializationError.value_or(Error::Disabled));
    s_isRequired.store(true, std::memory_order_relaxed);
    if (s_hasAllocationsOutside.load(std::memory_order_relaxed))
        return makeUnexpected(Error::BlocksOutsideCage);
    return { };
}

MarkedBlockCage::ReservationFailure MarkedBlockCage::reservationFailure()
{
    return s_reservationFailure;
}

String describeLimitOfMarkedBlockCage()
{
    if (!MarkedBlockCage::isRequired())
        return { };
    if (s_isGrowable)
        return "The limit on address space (ulimit -v) leaves no room for more JavaScript objects. Raise the limit."_s;
    size_t size = g_jscConfig.markedBlockCageSize;
    bool isInGB = !(size % GB);
    if (size < static_cast<size_t>(Options::markedBlockCageSizeInMB()) * MB)
        return makeString("The limit on address space (ulimit -v) leaves the threads of this executable "_s, size / MB, " MB for JavaScript objects. Raise the limit, or build with --aot-huge-heap."_s);
    return makeString("Each thread of a compiled executable can use "_s, size / (isInGB ? GB : MB), isInGB ? " GB"_s : " MB"_s, " for JavaScript objects. Build with --aot-huge-heap to remove the limit. The executable then uses more memory and runs slower."_s);
}

static String describeLimitOfStructureHeap()
{
    size_t size = g_jscConfig.sizeOfStructureHeap;
    if (hasAddressSpaceLimit() && !Options::structureHeapSizeInKB())
        return makeString("The limit on address space (ulimit -v) leaves this executable "_s, size / MB, " MB for the shapes of JavaScript objects (Structures). Raise the limit, or set BUN_JSC_structureHeapSizeInKB to a larger power of two."_s);
    return makeString("This executable can use "_s, size / MB, " MB for the shapes of JavaScript objects (Structures)."_s);
}

String MarkedBlockCage::describeExhaustedLimit() const
{
    return m_exhaustedHeap == ExhaustedHeap::Structures ? describeLimitOfStructureHeap() : describeLimitOfMarkedBlockCage();
}

void MarkedBlockCage::didThrowOutOfMemoryError()
{
    m_hasReleasedReserve = false;
    if (std::exchange(m_exhaustedHeap, ExhaustedHeap::Cells) == ExhaustedHeap::Structures)
        StructureAlignedMemoryAllocator::didThrowOutOfMemoryError();
}

void MarkedBlockCage::logExhaustion(ExhaustedHeap exhaustedHeap)
{
    if (!isRequired())
        return;
    dataLogLn("FATAL: out of memory. ", exhaustedHeap == ExhaustedHeap::Structures ? describeLimitOfStructureHeap() : describeLimitOfMarkedBlockCage());
    if (ExhaustionHandler handler = s_exhaustionHandler.load(std::memory_order_relaxed))
        handler();
}

void MarkedBlockCage::setExhaustionHandler(ExhaustionHandler handler)
{
    s_exhaustionHandler.store(handler, std::memory_order_relaxed);
}

void MarkedBlockCage::willUseLastReserve(VM& vm)
{
    if (LastReserveHandler handler = s_lastReserveHandler.load(std::memory_order_relaxed))
        handler(vm);
}

void MarkedBlockCage::setLastReserveHandler(LastReserveHandler handler)
{
    s_lastReserveHandler.store(handler, std::memory_order_relaxed);
}

} // namespace JSC
