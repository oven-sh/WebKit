/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <bit>
#include <limits>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class ReservedRangeHeap {
    WTF_MAKE_NONCOPYABLE(ReservedRangeHeap);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ReservedRangeHeap);
public:
    enum class Placement : uint8_t { Anywhere, RandomWindow };
    enum class InitialAccess : uint8_t { ReadWrite, None };

    struct Request {
        ASCIILiteral name;
        size_t size { 0 };
        size_t minSize { 0 };
        size_t alignment { 0 };
        size_t unusedPrefixSize { 0 };
        size_t addressSpaceBudget { std::numeric_limits<size_t>::max() };
        OSAllocator::Usage usage { OSAllocator::UnknownUsage };
        Placement placement { Placement::Anywhere };
        InitialAccess initialAccess { InitialAccess::ReadWrite };
    };

    static ReservedRangeHeap* tryCreate(const Request&);

    uintptr_t start() const { return m_start; }
    size_t size() const { return m_size; }
    bool contains(const void* pointer) const { return std::bit_cast<uintptr_t>(pointer) - m_start < m_size; }

    void* tryAllocateAligned(size_t alignment, size_t);
    void* tryAllocate(size_t);
    void* tryReallocate(void*, size_t);
    void free(void*);
    void releaseFreeMemory();

private:
    enum class BackEnd : uint8_t { Malloc, Granules };

    struct Backing {
        BackEnd backEnd { BackEnd::Granules };
        void* mallocHeap { nullptr };
        size_t size { 0 };
    };

    ReservedRangeHeap(uintptr_t start, const Backing&, const Request&);
    ~ReservedRangeHeap() = delete;

    static uintptr_t tryReserve(size_t, const Request&);
    static void releaseReservation(uintptr_t start, size_t);
    static std::optional<Backing> tryCreateBacking(uintptr_t start, size_t, const Request&);

    void* tryAllocateGranules(size_t);
    void freeGranules(void*);
    size_t granuleCountOfRun(size_t firstGranule) WTF_REQUIRES_LOCK(m_lock);
    static void commitGranules(void*, size_t);
    static void decommitGranules(void*, size_t);

    const uintptr_t m_start;
    const size_t m_size;
    const BackEnd m_backEnd;
    void* const m_mallocHeap;

    Lock m_lock;
    BitVector m_usedGranules WTF_GUARDED_BY_LOCK(m_lock);
    BitVector m_firstGranulesOfRuns WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_firstGranuleToSearch WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

} // namespace JSC
