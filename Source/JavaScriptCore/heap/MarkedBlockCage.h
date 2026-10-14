/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCConfig.h"
#include <atomic>

namespace JSC {

class JSCell;

// With Options::useAOT(), the MarkedBlocks of every FastMallocAlignedMemoryAllocator in the process come from one reservation.
// PreciseAllocations do not.
class MarkedBlockCage {
public:
    static void initialize();

    static bool isEnabled() { return g_jscConfig.sizeOfMarkedBlockCage; }
    static bool contains(const void* pointer) { return std::bit_cast<uintptr_t>(pointer) - g_jscConfig.startOfMarkedBlockCage < g_jscConfig.sizeOfMarkedBlockCage; }

    // A cell of a block in 32 bits. The cage is aligned to its size, which is at most maxSize, so it does not cross a multiple of that.
    static constexpr unsigned shift = 3;
    static constexpr uint64_t maxSize = 1ull << (32 + shift);
    static uintptr_t base() { return g_jscConfig.startOfMarkedBlockCage & ~static_cast<uintptr_t>(maxSize - 1); }
    static uint32_t encode(const JSCell* cell)
    {
        uintptr_t bits = std::bit_cast<uintptr_t>(cell);
        RELEASE_ASSERT(contains(cell) && !(bits & ((1 << shift) - 1)), bits);
        return static_cast<uint32_t>(bits >> shift);
    }
    static JSCell* decode(uint32_t index) { return std::bit_cast<JSCell*>(base() + (static_cast<uintptr_t>(index) << shift)); }

    // Until something requires the cage, blocks come from fastMalloc once it is full.
    static void require();
    static bool isRequired() { return s_isRequired.load(std::memory_order_relaxed); }
    static void didAllocateBlockOutside() { s_hasBlocksOutside.store(true, std::memory_order_relaxed); }

    static void* tryAllocateBlock();
    static void freeBlock(void*);
    // A free block keeps its pages until then.
    static void decommitFreeBlocks();
    static void logExhaustion();

private:
    static std::atomic<bool> s_isRequired;
    static std::atomic<bool> s_hasBlocksOutside;
};

} // namespace JSC
