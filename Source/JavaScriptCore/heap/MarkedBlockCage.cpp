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
#include "ResourceExhaustion.h"
#include "StructureAlignedMemoryAllocator.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <optional>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/DataLog.h>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/StackPointer.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX) || OS(FREEBSD)
#include <link.h>
#endif
#if OS(UNIX)
#include <sys/mman.h>
#endif

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
// Kept beside the reserve, in one piece, for one cell that is larger than a block.
static constexpr size_t largeCellReserveSize = MB;
static constexpr size_t lastReserveDivisor = 4;
// A VM that has taken no more blocks than this has only just been created.
static constexpr unsigned numberOfBlocksOfNewVM = 24;
// The most pieces of the size of a block that a VM of this process has taken until the first statement of the program ran: its blocks, and the chunks it keeps beside them.
static std::atomic<unsigned> s_numberOfBlocksOfStartedVM { numberOfBlocksOfNewVM };
// Any of the VMs that share a range can fill it, however few cells another has and wherever they lie. Then all of them find no room at about the same time.
// So each keeps, from its first blocks on, what a thread usually takes until it has ended alone. They lie in the pages that its first blocks have taken from the range anyway.
static constexpr unsigned numberOfChunksOfNewVM = 3;
// What a thread takes beyond that is kept once, for all of them. It is as large as the last of the smallest reserve of a VM.
static constexpr size_t sharedReserveSize = smallestReserveSize / lastReserveDivisor;
// A full collection is barren if the program can allocate less than 1/64 of the heap before there is no room again.
static constexpr size_t barrenCollectionDivisor = 64;
// After this many barren collections in a row, with no out-of-memory error in between, the error is owed.
static constexpr unsigned barrenCollectionsUntilError = 4;
// While the error finds no place to be thrown: after this many a thread that can end alone is asked to, and after this many the process ends.
static constexpr unsigned barrenCollectionsUntilThreadEnds = 8;
static constexpr unsigned barrenCollectionsUntilProcessEnds = 16;
// Creating the error, collections and the frame of a signal reach this far below where the error is thrown, at most.
static constexpr size_t stackBelowErrorToClear = 64 * KB;

static_assert(smallestUsableSize == smallestCellRangeSize);
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
static Lock s_sharedReserveLock;
static LazyNeverDestroyed<Vector<void*>> s_sharedReserve WTF_GUARDED_BY_LOCK(s_sharedReserveLock);

// Beside the shared reserve, for cells that are larger than a block: one piece of the range, given out in runs of chunks, so that it serves several VMs at once.
// What is freed goes back to the piece and not to the range: the piece is whole again when the cells are gone.
class SharedLargeCellReserve {
public:
    bool tryKeep(ReservedRangeHeap& range)
    {
        Locker locker { m_lock };
        if (m_start.load(std::memory_order_relaxed))
            return true;
        void* piece = range.tryAllocateAligned(MarkedBlock::blockSize, largeCellReserveSize);
        m_start.store(std::bit_cast<uintptr_t>(piece), std::memory_order_relaxed);
        return piece;
    }

    bool contains(const void* pointer) const { return std::bit_cast<uintptr_t>(pointer) - m_start.load(std::memory_order_relaxed) < largeCellReserveSize; }

    bool hasRoom()
    {
        Locker locker { m_lock };
        return m_start.load(std::memory_order_relaxed) && std::ranges::find(m_runs, 0) != m_runs.end();
    }

    void* tryAllocate(size_t size)
    {
        size_t count = (size + reserveChunkSize - 1) / reserveChunkSize;
        Locker locker { m_lock };
        uintptr_t start = m_start.load(std::memory_order_relaxed);
        if (!start || !count || count > numberOfChunks)
            return nullptr;
        size_t numberOfFreeChunks = 0;
        for (size_t index = 0; index < numberOfChunks; ++index) {
            numberOfFreeChunks = m_runs[index] ? 0 : numberOfFreeChunks + 1;
            if (numberOfFreeChunks < count)
                continue;
            size_t first = index + 1 - count;
            std::fill_n(m_runs.begin() + first, count, insideRun);
            m_runs[first] = static_cast<uint8_t>(count);
            return std::bit_cast<void*>(start + first * reserveChunkSize);
        }
        return nullptr;
    }

    size_t sizeOf(const void* pointer)
    {
        Locker locker { m_lock };
        return numberOfChunksOfRunAt(pointer) * reserveChunkSize;
    }

    void free(void* pointer)
    {
        Locker locker { m_lock };
        std::fill_n(m_runs.begin() + indexOf(pointer), numberOfChunksOfRunAt(pointer), 0);
    }

private:
    static constexpr size_t numberOfChunks = largeCellReserveSize / reserveChunkSize;
    static constexpr uint8_t insideRun = std::numeric_limits<uint8_t>::max();
    static_assert(numberOfChunks < insideRun);

    size_t indexOf(const void* pointer) const
    {
        uintptr_t offset = std::bit_cast<uintptr_t>(pointer) - m_start.load(std::memory_order_relaxed);
        RELEASE_ASSERT(offset < largeCellReserveSize && !(offset % reserveChunkSize), offset);
        return offset / reserveChunkSize;
    }

    size_t numberOfChunksOfRunAt(const void* pointer) WTF_REQUIRES_LOCK(m_lock)
    {
        uint8_t count = m_runs[indexOf(pointer)];
        RELEASE_ASSERT(count && count != insideRun, count);
        return count;
    }

    Lock m_lock;
    std::atomic<uintptr_t> m_start { 0 };
    // For the first chunk of a run the number of its chunks, for a free chunk 0.
    std::array<uint8_t, numberOfChunks> m_runs WTF_GUARDED_BY_LOCK(m_lock) { };
};
static constinit SharedLargeCellReserve s_sharedLargeCellReserve;

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

// 0 where no address in the image fits in 32 bits.
static uintptr_t endOfImageBelow4GB()
{
    uintptr_t start = std::numeric_limits<uintptr_t>::max();
    uintptr_t end = 0;
#if OS(LINUX) || OS(FREEBSD)
    struct Bounds {
        uintptr_t& start;
        uintptr_t& end;
    } bounds { start, end };
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) {
        auto& bounds = *static_cast<Bounds*>(data);
        for (unsigned index = 0; index < info->dlpi_phnum; ++index) {
            const auto& header = info->dlpi_phdr[index];
            if (header.p_type != PT_LOAD)
                continue;
            uintptr_t address = info->dlpi_addr + header.p_vaddr;
            bounds.start = std::min(bounds.start, address);
            bounds.end = std::max<uintptr_t>(bounds.end, address + header.p_memsz);
        }
        // The executable comes first.
        return 1;
    }, &bounds);
#endif
    return start <= std::numeric_limits<uint32_t>::max() ? end : 0;
}

size_t MarkedBlockCage::distanceOfStartFromBase()
{
    static const size_t result = [] {
        constexpr size_t granule = 32 * MB;
        uint64_t endOfImage = endOfImageBelow4GB();
        return static_cast<size_t>(std::min<uint64_t>(roundUpToMultipleOf<granule>(endOfImage << shift), maxSize / 2));
    }();
    return result;
}

static size_t remainingAddressSpace() WTF_REQUIRES_LOCK(s_poolLock)
{
    size_t budget = addressSpaceForMarkedBlockCages();
    return budget > s_totalReservedSize ? budget - s_totalReservedSize : 0;
}

static ReservedRangeHeap* tryCreateRange(size_t size, size_t minSize, size_t randomPrefixRange, size_t initialSegmentSize = 0) WTF_REQUIRES_LOCK(s_poolLock)
{
    size_t distanceFromBase = MarkedBlockCage::distanceOfStartFromBase();
    size = std::min(size, static_cast<size_t>(MarkedBlockCage::maxSize) - distanceFromBase);
    minSize = std::min(minSize, size);
    RELEASE_ASSERT(minSize >= smallestUsableSize && minSize <= size, minSize, size);
    RELEASE_ASSERT(randomPrefixRange >= randomPrefixGranule && unusedPrefixSize + randomPrefixRange < minSize, randomPrefixRange, minSize);

    // The first range gets the smallest size whatever its share is: without it nothing runs.
    size_t remaining = s_firstRange ? remainingAddressSpace() : std::max(remainingAddressSpace(), minSize);
    if (minSize > remaining)
        return nullptr;

    uint64_t random = Options::seedOfVMRandomForFuzzer() ? Options::seedOfVMRandomForFuzzer() : cryptographicallyRandomNumber<uint64_t>();
    size_t prefixSize = unusedPrefixSize + random % (randomPrefixRange / randomPrefixGranule) * randomPrefixGranule;

    ReservedRangeHeap::Request request {
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
    };
    ReservedRangeHeap* range = nullptr;
    bool reservesAllAboveBase = !distanceFromBase || !hasAddressSpaceLimit();
    if (reservesAllAboveBase) {
        request.reservedSizeBelowStart = distanceFromBase;
        range = ReservedRangeHeap::tryCreate(request);
    }
#if OS(UNIX)
    // Under a limit on address space nothing is reserved between what lies at the base and the range.
    constexpr unsigned maxPlacements = 4;
    for (unsigned attempt = 0; !reservesAllAboveBase && !range && attempt < maxPlacements; ++attempt) {
        void* base = ReservedRangeHeap::tryReserveRandomWindow(unusedPrefixSize, MarkedBlockCage::maxSize, request.usage, false);
        if (!base)
            base = OSAllocator::tryReserveUncommittedAligned(unusedPrefixSize, MarkedBlockCage::maxSize, request.usage, nullptr, false);
        if (!base)
            return nullptr;
        RELEASE_ASSERT(!mprotect(base, unusedPrefixSize, PROT_NONE));
        request.placement = ReservedRangeHeap::Placement::AtStart;
        request.start = std::bit_cast<uintptr_t>(base) + distanceFromBase;
        request.unreservedPrefixSize = unusedPrefixSize;
        range = ReservedRangeHeap::tryCreate(request);
        if (!range)
            OSAllocator::releaseDecommitted(base, unusedPrefixSize, 0);
    }
#endif
    if (!range)
        return nullptr;

    RELEASE_ASSERT(!((range->start() - distanceFromBase) & (MarkedBlockCage::maxSize - 1)), range->start(), range->size());
    s_totalReservedSize += range->reservedSize();
#if ENABLE(AOT)
    makeEmptyExtensionReadable(range->start() - distanceFromBase);
#endif
    return range;
}

std::expected<void, MarkedBlockCage::Error> MarkedBlockCage::initialize()
{
    std::call_once(s_initializeOnce, [] {
        RELEASE_ASSERT(!g_jscConfig.initializeHasBeenCalled);
        if (!Options::runsCompiledImage() || !Options::markedBlockCageSizeInMB())
            return;
        size_t size = static_cast<size_t>(Options::markedBlockCageSizeInMB()) * MB;
        if (!hasOneBitSet(size) || size > maxSize) {
            s_initializationError = Error::InvalidSize;
            return;
        }

        {
            Locker locker { s_sharedReserveLock };
            s_sharedReserve.construct();
        }
        Locker locker { s_poolLock };
        s_freeRanges.construct();
        if (size < smallestUsableSize) {
            g_jscConfig.markedBlockCageSize = size;
            return;
        }
        size_t randomPrefixRange = std::max(size >> maxHalvingsOfFirstRange, smallestUsableSize) / randomPrefixDivisor;
        if (hasAddressSpaceLimit())
            shareCLibraryHeapBetweenThreads();
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

// The pieces are handed to the VM that needs them as they are: a piece that a thread gives back lies in a page of the thread that took it, where no other thread finds it.
static bool tryKeepSharedReserve(ReservedRangeHeap& range, size_t maxNumberOfChunksToTake = std::numeric_limits<size_t>::max())
{
    if (!s_sharedLargeCellReserve.tryKeep(range))
        return false;
    Locker locker { s_sharedReserveLock };
    while (s_sharedReserve->size() < sharedReserveSize / reserveChunkSize) {
        void* chunk = maxNumberOfChunksToTake-- ? range.tryAllocateAligned(MarkedBlock::blockSize, reserveChunkSize) : nullptr;
        if (!chunk)
            return false;
        s_sharedReserve->append(chunk);
    }
    return true;
}

bool MarkedBlockCage::tryEnsureRoomForNewVMs(unsigned count)
{
    // The range keeps pieces of different sizes in pages of different sizes. These fill the pages that a VM takes until the first statement of a small program runs:
    // 12 of 64 KB for cells outside blocks, and those of its blocks. The large ones come last: by then the small ones have divided what is free.
    ReservedRangeHeap::Pieces piecesOfVM[] = { { 8 * KB, 84 }, { MarkedBlock::blockSize, s_numberOfBlocksOfStartedVM.load(std::memory_order_relaxed) } };
    ReservedRangeHeap* range;
    bool isTaken;
    {
        Locker locker { s_poolLock };
        if (!s_firstRange || !s_isGrowable)
            return true;
        range = s_firstRange;
        isTaken = s_freeRanges->isEmpty();
    }
    if (isTaken && !tryKeepSharedReserve(*range))
        return false;
    // One more: the threads that run take from the range meanwhile.
    return range->tryEnsureRoomForNewThreads(count + 1, piecesOfVM);
}

void MarkedBlockCage::willRunProgram()
{
    unsigned taken = m_numberOfBlocksTaken.load(std::memory_order_relaxed) + m_reserve.size();
    unsigned most = s_numberOfBlocksOfStartedVM.load(std::memory_order_relaxed);
    while (taken > most && !s_numberOfBlocksOfStartedVM.compare_exchange_weak(most, taken, std::memory_order_relaxed)) { }
}

MarkedBlockCage::MarkedBlockCage()
{
    if (!isEnabled())
        return;

    {
        Locker locker { s_poolLock };
        if (!s_firstRange)
            return;

        // A range that takes address space as it needs it serves every thread: what one of them gives back, another can take.
        if (!s_freeRanges->isEmpty())
            m_range = s_freeRanges->takeLast();
        else if (!s_isGrowable)
            m_range = tryCreateRange(g_jscConfig.markedBlockCageSize, g_jscConfig.markedBlockCageSize, g_jscConfig.markedBlockCageSize / randomPrefixDivisor);
        if (!m_range) {
            m_range = s_firstRange;
            m_sharesRange = true;
        }
        m_isInFirstRange = m_range == s_firstRange;
        m_start = m_range->start();
        m_base = m_start - distanceOfStartFromBase();
        m_size = m_range->size();
    }
    if (m_sharesRange)
        tryKeepSharedReserve(*m_range);
}

MarkedBlockCage::~MarkedBlockCage()
{
    releaseOwnLastReserve();
    // The blocks of this VM have gone back to the range.
    if (m_owesSharedReserve)
        tryKeepSharedReserve(*m_range);
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
    if (result) {
        didAllocate(result, size);
        m_numberOfBlocksTaken.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

void* MarkedBlockCage::tryAllocateAlignedFromSharedReserve(size_t alignment, size_t size)
{
    if (!m_usesSharedReserve || alignment > MarkedBlock::blockSize || size > reserveChunkSize)
        return nullptr;
    void* result;
    {
        Locker locker { s_sharedReserveLock };
        if (s_sharedReserve->isEmpty())
            return nullptr;
        result = s_sharedReserve->takeLast();
    }
    m_owesSharedReserve = true;
    didAllocate(result, size);
    return result;
}

void* MarkedBlockCage::tryAllocate(size_t size)
{
    if (!m_range)
        return nullptr;
    void* result = m_range->tryAllocate(size);
    if (!result && m_hasReleasedReserve && size <= largeCellReserveSize)
        result = std::exchange(m_largeCellReserve, nullptr);
    if (!result && m_usesSharedReserve)
        result = s_sharedLargeCellReserve.tryAllocate(size);
    if (result)
        didAllocate(result, size);
    return result;
}

void* MarkedBlockCage::tryReallocate(void* pointer, size_t size)
{
    if (!m_range)
        return nullptr;
    if (s_sharedLargeCellReserve.contains(pointer)) [[unlikely]] {
        size_t oldSize = s_sharedLargeCellReserve.sizeOf(pointer);
        if (size <= oldSize)
            return pointer;
        void* result = tryAllocate(size);
        if (result) {
            memcpy(result, pointer, oldSize);
            s_sharedLargeCellReserve.free(pointer);
        }
        return result;
    }
    void* result = m_range->tryReallocate(pointer, size);
    if (result) {
        didAllocate(result, size);
        return result;
    }
    // The range has no room. What a new cell of this size gets, this one gets too.
    result = tryAllocate(size);
    if (result) {
        memcpy(result, pointer, std::min(size, m_range->sizeOf(pointer)));
        m_range->free(pointer);
    }
    return result;
}

void MarkedBlockCage::free(void* pointer)
{
    if (s_sharedLargeCellReserve.contains(pointer)) [[unlikely]]
        s_sharedLargeCellReserve.free(pointer);
    else
        m_range->free(pointer);
}

size_t MarkedBlockCage::reserveSize() const
{
    return m_reserve.size() * reserveChunkSize;
}

void MarkedBlockCage::keepReserveOnceHalfFull()
{
#if CPU(X86_64) || CPU(ARM64)
    if (!m_range)
        return;
    // What this VM took from the shared reserve goes back once the range has room again, a chunk for each block that the VM takes.
    if (m_owesSharedReserve) [[unlikely]]
        m_owesSharedReserve = !tryKeepSharedReserve(*m_range, 1);
    if (m_hasReleasedReserve)
        return;
    size_t size = std::clamp(m_range->reservedSize() / reserveDivisor, smallestReserveSize, largestReserveSize);
    if (m_numberOfBlocksTaken.load(std::memory_order_relaxed) <= numberOfBlocksOfNewVM || m_endOfHighestAllocation.load(std::memory_order_relaxed) <= m_start + m_range->initialSegmentSize() / 2) {
        if (!s_isGrowable && !m_sharesRange)
            return;
        size = numberOfChunksOfNewVM * reserveChunkSize;
    } else if (!m_largeCellReserve && std::exchange(m_shouldKeepLargeCellReserve, false))
        m_largeCellReserve = m_range->tryAllocateAligned(MarkedBlock::blockSize, largeCellReserveSize);
    if (reserveSize() >= size)
        return;
    if (void* chunk = m_range->tryAllocateAligned(MarkedBlock::blockSize, reserveChunkSize))
        m_reserve.append(chunk);
#endif
}

bool MarkedBlockCage::releaseReserve()
{
    if (!m_range)
        return false;
    // A share of the whole reserve: a program that is told again and again, and lets go of nothing, does not get the last of it piece by piece.
    size_t numberOfChunksKept = std::clamp(m_range->reservedSize() / reserveDivisor, smallestReserveSize, largestReserveSize) / reserveChunkSize / lastReserveDivisor;
    if (m_hasReleasedReserve || m_reserve.size() <= numberOfChunksKept)
        return false;
    m_numberOfChunksReleased = m_reserve.size() - numberOfChunksKept;
    while (m_reserve.size() > numberOfChunksKept)
        m_range->free(m_reserve.takeLast());
    m_hasReleasedReserve = true;
    return true;
}

bool MarkedBlockCage::hasHeapThatReleasedReserve() const
{
    return m_hasReleasedReserve || m_exhaustedHeaps.contains(ExhaustedHeap::Structures);
}

bool MarkedBlockCage::tryKeepReleasedReservesAgain()
{
    if (!hasHeapThatReleasedReserve())
        return false;
    if (m_exhaustedHeaps.contains(ExhaustedHeap::Structures)) {
        if (!StructureAlignedMemoryAllocator::tryKeepReleasedReserveAgain())
            return false;
        m_exhaustedHeaps.remove(ExhaustedHeap::Structures);
    }
    if (m_hasReleasedReserve && !tryKeepOwnReleasedReserveAgain())
        return false;
    m_exhaustedHeaps = { };
    return true;
}

bool MarkedBlockCage::tryKeepOwnReleasedReserveAgain()
{
    if (!m_hasReleasedReserve || !m_numberOfChunksReleased)
        return false;
    size_t numberOfChunksKept = m_reserve.size();
    while (m_reserve.size() < numberOfChunksKept + m_numberOfChunksReleased) {
        void* chunk = m_range->tryAllocateAligned(MarkedBlock::blockSize, reserveChunkSize);
        if (!chunk) {
            while (m_reserve.size() > numberOfChunksKept)
                m_range->free(m_reserve.takeLast());
            return false;
        }
        m_reserve.append(chunk);
    }
    m_numberOfChunksReleased = 0;
    m_hasReleasedReserve = false;
    m_shouldKeepLargeCellReserve = true;
    return true;
}

bool MarkedBlockCage::tryTakeBackOutOfMemoryError(VM& vm)
{
    if (!m_owesCollection || vm.heap.isDeferred())
        return false;
    m_owesCollection = false;
    vm.heap.collectForExhaustedCage();
    if (!vm.traps().needsOutOfMemoryError())
        return false;
    if (!tryKeepReleasedReservesAgain())
        return false;
    vm.traps().setNeedsOutOfMemoryError(false);
    return true;
}

bool MarkedBlockCage::tryTakeBackOutOfMemoryErrorAtPoll(VM& vm)
{
    m_errorWaitsForPoll = false;
    if (vm.heap.isDeferred() || (m_hasCollectedAtPoll && !m_owesCollection))
        return false;
    m_owesCollection = false;
    m_hasCollectedAtPoll = true;
    clearDeadStack(vm);
    vm.heap.collectForExhaustedCage();
    if (!vm.traps().needsOutOfMemoryError())
        return false;
    if (hasHeapThatReleasedReserve()) {
        if (!tryKeepReleasedReservesAgain())
            return false;
    } else {
        // How much the program can allocate now says whether this collection was barren too.
        m_numberOfBarrenCollections = barrenCollectionsUntilError - 1;
        m_exhaustedHeaps = { };
    }
    vm.traps().setNeedsOutOfMemoryError(false);
    return true;
}

void* MarkedBlockCage::takeLowestAddressOfDeadStack(VM& vm)
{
    uintptr_t stackPointer = std::bit_cast<uintptr_t>(currentStackPointer());
    uintptr_t lowest = stackPointer;
    if (void* stackPointerAtError = std::exchange(m_stackPointerAtError, nullptr))
        lowest = std::min(lowest, std::bit_cast<uintptr_t>(stackPointerAtError));
    // A frame that has no stack check of its own may end below the soft limit.
    return std::bit_cast<void*>(std::min(stackPointer, std::max(lowest - stackBelowErrorToClear, std::bit_cast<uintptr_t>(vm.stackLimit()))));
}

void MarkedBlockCage::clearDeadStack(VM& vm)
{
    sanitizeStackForVMFrom(vm, takeLowestAddressOfDeadStack(vm));
}

bool MarkedBlockCage::releaseLastReserve()
{
    // Both at once: the chunks of this VM are no room for a cell that is larger than a block.
    bool hasReleased = releaseOwnLastReserve();
    return tryUseSharedReserve() || hasReleased;
}

bool MarkedBlockCage::tryUseSharedReserve()
{
    if (!m_isInFirstRange || m_usesSharedReserve)
        return false;
    {
        Locker locker { s_sharedReserveLock };
        if (s_sharedReserve->isEmpty() && !s_sharedLargeCellReserve.hasRoom())
            return false;
    }
    m_usesSharedReserve = true;
    m_hasReleasedReserve = true;
    return true;
}

bool MarkedBlockCage::releaseOwnLastReserve()
{
    void* largeCellReserve = std::exchange(m_largeCellReserve, nullptr);
    if (largeCellReserve)
        m_range->free(largeCellReserve);
    if (m_reserve.isEmpty())
        return largeCellReserve;
    for (void* chunk : std::exchange(m_reserve, { }))
        m_range->free(chunk);
    m_numberOfChunksReleased = 0;
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

static constexpr ASCIILiteral noRoomUnderLimitOnAddressSpace = "The limit on address space (ulimit -v) leaves no room for more JavaScript objects. Raise the limit."_s;

String describeLimitOfMarkedBlockCage()
{
    if (!MarkedBlockCage::isRequired())
        return { };
    if (s_isGrowable)
        return noRoomUnderLimitOnAddressSpace;
    size_t size = g_jscConfig.markedBlockCageSize;
    bool isInGB = !(size % GB);
    if (size < std::min(static_cast<size_t>(Options::markedBlockCageSizeInMB()) * MB, static_cast<size_t>(MarkedBlockCage::maxSize) - MarkedBlockCage::distanceOfStartFromBase()))
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
    return m_exhaustedHeaps.contains(ExhaustedHeap::Structures) ? describeLimitOfStructureHeap() : describeLimitOfMarkedBlockCage();
}

void MarkedBlockCage::didThrowOutOfMemoryError()
{
    m_hasReleasedReserve = false;
    m_usesSharedReserve = false;
    m_shouldKeepLargeCellReserve = true;
    m_errorWaitsForPoll = false;
    if (std::exchange(m_exhaustedHeaps, { }).contains(ExhaustedHeap::Structures))
        StructureAlignedMemoryAllocator::didThrowOutOfMemoryError();
}

void MarkedBlockCage::didCreateOutOfMemoryError()
{
    m_numberOfBarrenCollections = 0;
    m_hasCollectedAtPoll = false;
    m_stackPointerAtError = currentStackPointer();
}

void MarkedBlockCage::didCreateOutOfMemoryError(VM& vm)
{
    didCreateOutOfMemoryError();
    // A range that grows takes its room from where storage takes it.
    if (!s_isGrowable || !m_exhaustedHeaps.contains(ExhaustedHeap::Cells))
        return;
    didThrowOutOfMemoryError();
    vm.traps().setNeedsOutOfMemoryError(false);
}

void MarkedBlockCage::didFindNoRoom(VM& vm, ExhaustedHeap exhaustedHeap)
{
    ASSERT(isRequired());
    uint64_t bytesAllocated = vm.heap.totalBytesAllocated();
    uint64_t bytesAllocatedSinceLastFull = bytesAllocated - std::exchange(m_bytesAllocatedWhenLastFull, bytesAllocated);
    // The same request, trying again after its collection.
    if (!bytesAllocatedSinceLastFull)
        return;
    if (bytesAllocatedSinceLastFull >= vm.heap.objectSpace().capacity() / barrenCollectionDivisor) {
        m_numberOfBarrenCollections = 0;
        m_hasCollectedAtPoll = false;
        return;
    }
    if (++m_numberOfBarrenCollections < barrenCollectionsUntilError)
        return;
    m_errorWaitsForPoll = false;
    if (!vm.traps().needsOutOfMemoryError()) {
        addExhaustedHeap(exhaustedHeap);
        m_errorWaitsForPoll = !m_hasCollectedAtPoll;
        vm.traps().setNeedsOutOfMemoryError(true);
    }
    if (m_numberOfBarrenCollections == barrenCollectionsUntilThreadEnds)
        willUseLastReserve(vm);
    if (m_numberOfBarrenCollections < barrenCollectionsUntilProcessEnds)
        return;
    logExhaustion(exhaustedHeap);
    RELEASE_ASSERT_RESOURCE_AVAILABLE(false, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
}

void MarkedBlockCage::logExhaustion(ExhaustedHeap exhaustedHeap)
{
    if (!isRequired())
        return;
    // Under a limit on address space nothing may be left to make a String of.
    if (exhaustedHeap == ExhaustedHeap::Cells && s_isGrowable)
        dataLogLn("FATAL: out of memory. ", noRoomUnderLimitOnAddressSpace);
    else
        dataLogLn("FATAL: out of memory. ", exhaustedHeap == ExhaustedHeap::Structures ? describeLimitOfStructureHeap() : describeLimitOfMarkedBlockCage());
    if (ExhaustionHandler handler = s_exhaustionHandler.load(std::memory_order_relaxed))
        handler();
}

void logExhaustedAddressSpace()
{
    if (s_isGrowable)
        MarkedBlockCage::logExhaustion(MarkedBlockCage::ExhaustedHeap::Cells);
}

void MarkedBlockCage::setExhaustionHandler(ExhaustionHandler handler)
{
    s_exhaustionHandler.store(handler, std::memory_order_relaxed);
}

void MarkedBlockCage::willUseLastReserve(VM& vm)
{
    if (LastReserveHandler handler = s_lastReserveHandler.load(std::memory_order_relaxed))
        handler(vm);
    // Loops of the run time that make many cells look for the error that is owed, not for a termination. A thread that is to end leaves them at once.
    if (vm.hasPendingTermination())
        vm.traps().setNeedsOutOfMemoryError(true);
}

void MarkedBlockCage::setLastReserveHandler(LastReserveHandler handler)
{
    s_lastReserveHandler.store(handler, std::memory_order_relaxed);
}

} // namespace JSC
