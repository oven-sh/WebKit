/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MimallocPrimitiveHeap.h"

#if BUSE(MIMALLOC_PRIMITIVE_HEAP)

#include "BAssert.h"
#include "Gigacage.h"
#include "Mutex.h"
#include <chrono>
#include <cstdint>

#if BOS(UNIX)
#include <sys/resource.h>
#endif

namespace bmalloc {

namespace {

struct Range {
    mi_heap_t* heap;
    uintptr_t start;
    size_t size;
};

// The most that one mimalloc arena holds. A heap is bound to one arena, and an arena does not grow.
constexpr uint64_t rangeSize = 16 * static_cast<uint64_t>(Sizes::GB);
// Ranges cost address space only, and only once they are needed. mimalloc has 160 arenas for the whole process.
constexpr size_t maxRangeCount = 64;

// That no range has room for a block above this size says little about smaller ones.
constexpr size_t largestSizeThatShowsRangesAreFull = Sizes::MB;
constexpr auto timeBetweenAttemptsWhileFull = std::chrono::seconds(1);

Mutex s_lock;
Range s_ranges[maxRangeCount];
std::atomic<size_t> s_rangeCount { 0 };
bool s_isDecided { false };
std::atomic<bool> s_hasNoRanges { false };
std::atomic<size_t> s_allocationsOutsideRanges { 0 };
std::chrono::steady_clock::time_point s_timeOfNextAttempt;

bool shouldHaveRanges()
{
#if BCPU(ADDRESS64)
#if BOS(UNIX) && defined(RLIMIT_AS)
    // A range would come out of what the rest of the process may map.
    struct rlimit limit;
    if (getrlimit(RLIMIT_AS, &limit) || limit.rlim_cur != RLIM_INFINITY)
        return false;
#endif
    return true;
#else
    return false;
#endif
}

} // anonymous namespace

std::atomic<mi_heap_t*> MimallocPrimitiveHeap::s_heap { nullptr };

void MimallocPrimitiveHeap::doWithoutRanges()
{
    s_hasNoRanges.store(true, std::memory_order_relaxed);
    s_heap.store(mi_heap_main(), std::memory_order_release);
}

void MimallocPrimitiveHeap::decide()
{
    if (s_isDecided)
        return;
    s_isDecided = true;
    if (mi_heap_t* heap = shouldHaveRanges() ? tryAddRange() : nullptr)
        s_heap.store(heap, std::memory_order_release);
    else
        doWithoutRanges();
}

void* MimallocPrimitiveHeap::allocateIn(mi_heap_t* heap, const Request& request)
{
    if (request.blockToReallocate)
        return heap ? mi_heap_realloc(heap, request.blockToReallocate, request.size) : mi_realloc(request.blockToReallocate, request.size);
    if (request.alignment) {
        if (request.isZeroed)
            return heap ? mi_heap_zalloc_aligned(heap, request.size, request.alignment) : mi_zalloc_aligned(request.size, request.alignment);
        return heap ? mi_heap_malloc_aligned(heap, request.size, request.alignment) : mi_malloc_aligned(request.size, request.alignment);
    }
    if (request.isZeroed)
        return heap ? mi_heap_zalloc(heap, request.size) : mi_zalloc(request.size);
    return heap ? mi_heap_malloc(heap, request.size) : mi_malloc(request.size);
}

mi_heap_t* MimallocPrimitiveHeap::tryAddRange()
{
    size_t count = s_rangeCount.load(std::memory_order_relaxed);
    if (count == maxRangeCount)
        return nullptr;

    // Exclusive: mimalloc gives the free pages of any other arena to whichever heap asks next.
    mi_arena_id_t arena { };
    if (mi_reserve_os_memory_ex(static_cast<size_t>(rangeSize), /* commit */ false, /* allow_large */ false, /* exclusive */ true, &arena))
        return nullptr;
    mi_heap_t* heap = mi_heap_new_in_arena(arena);
    RELEASE_BASSERT(heap);

    size_t size = 0;
    void* start = mi_arena_area(arena, &size);
    s_ranges[count] = { heap, reinterpret_cast<uintptr_t>(start), size };
    s_rangeCount.store(count + 1, std::memory_order_release);
    return heap;
}

void* MimallocPrimitiveHeap::allocateInSomeRange(const Request& request)
{
    bool areFull = !s_heap.load(std::memory_order_relaxed);
    if (areFull && std::chrono::steady_clock::now() < s_timeOfNextAttempt)
        return nullptr;

    for (size_t index = s_rangeCount.load(std::memory_order_relaxed); index--;) {
        mi_heap_t* heap = s_ranges[index].heap;
        if (void* result = allocateIn(heap, request)) {
            s_heap.store(heap, std::memory_order_release);
            return result;
        }
    }

    if (mi_heap_t* heap = tryAddRange()) {
        s_heap.store(heap, std::memory_order_release);
        return allocateIn(heap, request);
    }

    if (request.size <= largestSizeThatShowsRangesAreFull) {
        // A heap walks all its pages before it refuses, which is too much to pay for each allocation.
        s_heap.store(nullptr, std::memory_order_relaxed);
        s_timeOfNextAttempt = std::chrono::steady_clock::now() + timeBetweenAttemptsWhileFull;
    }
    return nullptr;
}

void* MimallocPrimitiveHeap::allocateSlow(size_t size, size_t alignment, bool isZeroed, void* blockToReallocate)
{
    Request request { size, alignment, isZeroed, blockToReallocate };
    if (!s_hasNoRanges.load(std::memory_order_relaxed)) {
        LockHolder locker(s_lock);
        decide();
        if (request.size <= largestBlockSize && request.alignment <= largestAlignment && !s_hasNoRanges.load(std::memory_order_relaxed))
            return allocateInSomeRange(request);
    }
    if (s_rangeCount.load(std::memory_order_relaxed))
        s_allocationsOutsideRanges.fetch_add(1, std::memory_order_relaxed);
    return allocateIn(nullptr, request);
}

void MimallocPrimitiveHeap::decideWhetherToHaveRanges()
{
    LockHolder locker(s_lock);
    decide();
}

size_t MimallocPrimitiveHeap::allocationsOutsideRanges()
{
    return s_allocationsOutsideRanges.load(std::memory_order_relaxed);
}

void MimallocPrimitiveHeap::collectThisThread(bool force)
{
    for (size_t index = 0, count = s_rangeCount.load(std::memory_order_acquire); index < count; ++index)
        mi_heap_collect(s_ranges[index].heap, force);
}

bool MimallocPrimitiveHeap::isInRange(const void* pointer)
{
    for (size_t index = 0, count = s_rangeCount.load(std::memory_order_acquire); index < count; ++index) {
        if (reinterpret_cast<uintptr_t>(pointer) - s_ranges[index].start < s_ranges[index].size)
            return true;
    }
    return false;
}

} // namespace bmalloc

#if !GIGACAGE_ENABLED
namespace Gigacage {

void ensureGigacage()
{
    bmalloc::MimallocPrimitiveHeap::decideWhetherToHaveRanges();
}

} // namespace Gigacage
#endif

#endif // BUSE(MIMALLOC_PRIMITIVE_HEAP)
