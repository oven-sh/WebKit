/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef __cplusplus

#include "BPlatform.h"

#if BUSE(MIMALLOC)

#include "BExport.h"
#include "BInline.h"
#include "Sizes.h"
#include "mimalloc.h"
#include <atomic>
#include <cstddef>

namespace bmalloc {

// Where HeapKind::PrimitiveGigacage allocations come from on mimalloc: heaps of their own, in address ranges that
// nothing else is allocated from. What is allocated with that kind holds no pointers, and scripts and input choose
// its content: the bytes of ArrayBuffers and typed arrays, Wasm memories. Unlike in a Gigacage, pointers are not masked.
//
// A request that no range can serve goes to the default heap, so nothing fails that would succeed there.
class MimallocPrimitiveHeap {
public:
    BINLINE static void* tryMalloc(size_t size)
    {
        if (mi_heap_t* heap = heapFor(size, 0)) [[likely]] {
            if (void* result = mi_heap_malloc(heap, size)) [[likely]]
                return result;
        }
        return allocateSlow(size, 0, false, nullptr);
    }

    BINLINE static void* tryZeroedMalloc(size_t size)
    {
        if (mi_heap_t* heap = heapFor(size, 0)) [[likely]] {
            if (void* result = mi_heap_zalloc(heap, size)) [[likely]]
                return result;
        }
        return allocateSlow(size, 0, true, nullptr);
    }

    BINLINE static void* tryMemalign(size_t alignment, size_t size)
    {
        if (mi_heap_t* heap = heapFor(size, alignment)) [[likely]] {
            if (void* result = mi_heap_malloc_aligned(heap, size, alignment)) [[likely]]
                return result;
        }
        return allocateSlow(size, alignment, false, nullptr);
    }

    BINLINE static void* tryZeroedMemalign(size_t alignment, size_t size)
    {
        if (mi_heap_t* heap = heapFor(size, alignment)) [[likely]] {
            if (void* result = mi_heap_zalloc_aligned(heap, size, alignment)) [[likely]]
                return result;
        }
        return allocateSlow(size, alignment, true, nullptr);
    }

    // mi_realloc would move a block that has to move to the default heap.
    BINLINE static void* tryRealloc(void* block, size_t size)
    {
        if (mi_heap_t* heap = heapFor(size, 0)) [[likely]] {
            if (void* result = mi_heap_realloc(heap, block, size)) [[likely]]
                return result;
        }
        return allocateSlow(size, 0, false, block);
    }

    // Settles whether the process has ranges, from its environment as it is now. Otherwise the first allocation does.
    BEXPORT static void decideWhetherToHaveRanges();

    // mi_collect and mi_theap_collect(mi_theap_get_default()) leave what the calling thread holds of these heaps.
    BEXPORT static void collectThisThread(bool force);

    BEXPORT static bool isInRange(const void*);
    // How many allocations went to the default heap in a process that has ranges.
    BEXPORT static size_t allocationsOutsideRanges();

private:
    struct Request {
        size_t size;
        size_t alignment;
        bool isZeroed;
        void* blockToReallocate;
    };

    // Beyond these mimalloc puts a block in no arena, whichever heap is asked: it maps it by itself. A heap that is
    // bound to an arena can only refuse such a block, and it collects before it refuses.
    static constexpr size_t largestBlockSize = 2 * Sizes::GB;
    static constexpr size_t largestAlignment = 64 * Sizes::kB;

    BINLINE static mi_heap_t* heapFor(size_t size, size_t alignment)
    {
        if (size > largestBlockSize || alignment > largestAlignment) [[unlikely]]
            return nullptr;
        return s_heap.load(std::memory_order_acquire);
    }

    BEXPORT static void* allocateSlow(size_t, size_t alignment, bool isZeroed, void* blockToReallocate);
    static void* allocateIn(mi_heap_t*, const Request&);
    static void* allocateInSomeRange(const Request&);
    static mi_heap_t* tryAddRange();
    static void decide();
    static void doWithoutRanges();

    // The heap of the range in use, or the default heap in a process without ranges. Null before the first allocation
    // and while all ranges are full.
    BEXPORT static std::atomic<mi_heap_t*> s_heap;
};

} // namespace bmalloc

#endif // BUSE(MIMALLOC)

#endif // __cplusplus
