/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <atomic>
#include <bit>
#include <limits>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class ReservedRangeHeap {
    WTF_MAKE_NONCOPYABLE(ReservedRangeHeap);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ReservedRangeHeap);
public:
    enum class Placement : uint8_t { Anywhere, RandomWindow, AtStart };
    enum class InitialAccess : uint8_t { ReadWrite, None };

    struct Request {
        ASCIILiteral name;
        size_t size { 0 };
        size_t minSize { 0 };
        // When nonzero and smaller than size, only this much is reserved up front and the rest of the range is reserved on demand.
        size_t initialSegmentSize { 0 };
        size_t alignment { 0 };
        size_t unusedPrefixSize { 0 };
        // This much of the unused prefix, from the start of the range, is not reserved.
        size_t unreservedPrefixSize { 0 };
        // This much below the start of the range is reserved with it, without access. The alignment is that of its lower end.
        size_t reservedSizeBelowStart { 0 };
        uintptr_t start { 0 };
        size_t addressSpaceBudget { std::numeric_limits<size_t>::max() };
        OSAllocator::Usage usage { OSAllocator::UnknownUsage };
        Placement placement { Placement::Anywhere };
        InitialAccess initialAccess { InitialAccess::ReadWrite };
    };

    static ReservedRangeHeap* tryCreate(const Request&);
    // Null where none of the multiples of the alignment that were drawn is free.
    static void* tryReserveRandomWindow(size_t, size_t alignment, OSAllocator::Usage, bool isWritable);

    uintptr_t start() const { return m_start; }
    size_t size() const { return m_size; }
    size_t initialSegmentSize() const { return m_initialSegmentSize; }
    size_t reservedSize() const { return m_reservedSize.load(std::memory_order_relaxed); }
    bool contains(const void* pointer) const { return std::bit_cast<uintptr_t>(pointer) - m_start < m_size; }

    void* tryAllocateAligned(size_t alignment, size_t);
    void* tryAllocate(size_t);
    void* tryReallocate(void*, size_t);
    void free(void*);
    size_t sizeOf(const void*);
    void releaseFreeMemory();

    struct Pieces {
        size_t size;
        unsigned count;
    };
    // Whether each of so many threads that have taken nothing from the range yet can take all these. Takes address space if that is what it needs.
    bool tryEnsureRoomForNewThreads(unsigned numberOfThreads, std::span<const Pieces>);
    // The least address space that must be left for a full range to take more.
    JS_EXPORT_PRIVATE static size_t addressSpaceToGrow();

private:
    enum class BackEnd : uint8_t { Malloc, Granules };

    struct Backing {
        BackEnd backEnd { BackEnd::Granules };
        void* mallocHeap { nullptr };
        size_t size { 0 };
        void* mallocArena { nullptr };
    };

    ReservedRangeHeap(uintptr_t start, size_t, size_t reservedSize, const Backing&, const Request&);
    ~ReservedRangeHeap() = delete;

    static uintptr_t tryReserve(size_t, const Request&);
    static void releaseReservation(uintptr_t start, size_t, const Request&);
    static std::optional<Backing> tryCreateBacking(uintptr_t start, size_t, const Request&);

    void* tryAllocateSlow(size_t alignment, size_t);
    bool tryAddSegment(size_t allocationSize) WTF_REQUIRES_LOCK(m_lock);

    void* tryAllocateGranules(size_t);
    void freeGranules(void*);
    size_t granuleCountOfRun(size_t firstGranule) WTF_REQUIRES_LOCK(m_lock);
    void setAccess(size_t firstGranule, size_t count, bool isAccessible) WTF_REQUIRES_LOCK(m_lock);
    static void commitGranules(void*, size_t);
    static void decommitGranules(void*, size_t, bool removesAccess);

    const uintptr_t m_start;
    const size_t m_size;
    const size_t m_initialSegmentSize;
    const BackEnd m_backEnd;
    const bool m_isGrowable;
    // All of the range is reserved, and a new segment only gets access.
    const bool m_isReservedWhole;
    const OSAllocator::Usage m_usage;
    const ASCIILiteral m_name;
    std::atomic<void*> m_mallocHeap;
    std::atomic<size_t> m_reservedSize;

    Lock m_lock;
    Vector<void*, 4> m_segmentHeaps WTF_GUARDED_BY_LOCK(m_lock);
    Vector<void*, 4> m_segmentArenas WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_segmentsEnd WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    BitVector m_usedGranules WTF_GUARDED_BY_LOCK(m_lock);
    BitVector m_firstGranulesOfRuns WTF_GUARDED_BY_LOCK(m_lock);
    BitVector m_accessibleGranules WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_firstGranuleToSearch WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

} // namespace JSC
