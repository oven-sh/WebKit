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
#include "ReservedRangeHeap.h"
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

namespace JSC {

static_assert(1 << MarkedBlockCage::shift <= MarkedBlock::atomSize);
static_assert(hasOneBitSet(MarkedBlockCage::maxSize));
static_assert(!((static_cast<uint64_t>(MarkedBlockCage::minIndex) << MarkedBlockCage::shift) % MarkedBlock::blockSize));

static constexpr size_t unusedPrefixSize = static_cast<size_t>(MarkedBlockCage::minIndex) << MarkedBlockCage::shift;
static constexpr size_t smallestUsableSize = 4 * unusedPrefixSize;
static constexpr size_t randomPrefixDivisor = 32;
static constexpr size_t randomPrefixGranule = 64 * KB;
static constexpr unsigned maxHalvingsOfFirstRange = 7;

static_assert(!(randomPrefixGranule % MarkedBlock::blockSize));
static_assert(smallestUsableSize / randomPrefixDivisor >= randomPrefixGranule);
static_assert(unusedPrefixSize + smallestUsableSize / randomPrefixDivisor < smallestUsableSize);

std::atomic<bool> MarkedBlockCage::s_isRequired { false };
std::atomic<bool> MarkedBlockCage::s_hasAllocationsOutside { false };
std::atomic<MarkedBlockCage::ExhaustionHandler> MarkedBlockCage::s_exhaustionHandler { nullptr };
static Lock s_poolLock;
static LazyNeverDestroyed<Vector<ReservedRangeHeap*>> s_freeRanges WTF_GUARDED_BY_LOCK(s_poolLock);
static ReservedRangeHeap* s_firstRange WTF_GUARDED_BY_LOCK(s_poolLock);
static size_t s_totalReservedSize WTF_GUARDED_BY_LOCK(s_poolLock);
static std::once_flag s_initializeOnce;
static std::optional<MarkedBlockCage::Error> s_initializationError;

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

static ReservedRangeHeap* tryCreateRange(size_t size, size_t minSize) WTF_REQUIRES_LOCK(s_poolLock)
{
    RELEASE_ASSERT(minSize >= smallestUsableSize && minSize <= size, minSize, size);

    size_t budget = addressSpaceToReserve() / addressSpaceDivisorForMarkedBlockCages;
    size_t remaining = budget > s_totalReservedSize ? budget - s_totalReservedSize : 0;
    if (minSize > remaining)
        return nullptr;

    uint64_t random = Options::seedOfVMRandomForFuzzer() ? Options::seedOfVMRandomForFuzzer() : cryptographicallyRandomNumber<uint64_t>();
    size_t prefixSize = unusedPrefixSize + random % (minSize / randomPrefixDivisor / randomPrefixGranule) * randomPrefixGranule;

    ReservedRangeHeap* range = ReservedRangeHeap::tryCreate({
        .name = "Cell Cage"_s,
        .size = size,
        .minSize = minSize,
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
    s_totalReservedSize += range->size();
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
        s_firstRange = tryCreateRange(size, std::max(size >> maxHalvingsOfFirstRange, smallestUsableSize));
        if (!s_firstRange) {
            s_initializationError = Error::ReservationFailed;
            return;
        }
        s_freeRanges->append(s_firstRange);
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
    else
        m_range = tryCreateRange(g_jscConfig.markedBlockCageSize, g_jscConfig.markedBlockCageSize);
    if (!m_range) {
        m_range = s_firstRange;
        m_sharesRange = true;
    }
    m_base = m_range->start();
    m_size = m_range->size();
}

MarkedBlockCage::~MarkedBlockCage()
{
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

void MarkedBlockCage::logExhaustion()
{
    if (!isRequired())
        return;
    bool isInGB = !(g_jscConfig.markedBlockCageSize % GB);
    dataLogLn("FATAL: out of memory. Each thread of a compiled executable can use ", g_jscConfig.markedBlockCageSize / (isInGB ? GB : MB), isInGB ? " GB" : " MB", " for JavaScript objects. Build with --aot-huge-heap to remove the limit. The executable then uses more memory and runs slower.");
    if (ExhaustionHandler handler = s_exhaustionHandler.load(std::memory_order_relaxed))
        handler();
}

void MarkedBlockCage::setExhaustionHandler(ExhaustionHandler handler)
{
    s_exhaustionHandler.store(handler, std::memory_order_relaxed);
}

} // namespace JSC
