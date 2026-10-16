/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCConfig.h"
#include <atomic>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSCell;
class MarkedBlockCageManager;

// With Options::useAOT(), the MarkedBlocks of a FastMallocAlignedMemoryAllocator come from a reservation of its own. Each heap has one, all of one size; a heap for which there is no room shares the first. What is static here is about the heap that the current thread runs or collects.
// PreciseAllocations do not.
class MarkedBlockCage {
    WTF_MAKE_NONCOPYABLE(MarkedBlockCage);
public:
    static void initialize();

    MarkedBlockCage() = default;
    ~MarkedBlockCage();

    static bool isEnabled() { return g_jscConfig.sizeOfMarkedBlockCage; }
    static bool contains(const void* pointer) { return std::bit_cast<uintptr_t>(pointer) - base() < g_jscConfig.sizeOfMarkedBlockCage; }

    // A cell of a block in 32 bits. The cage is aligned to its size, which is at most maxSize, so it does not cross a multiple of that.
    static constexpr unsigned shift = 3;
    static constexpr uint64_t maxSize = 1ull << (32 + shift);
    static constexpr uint32_t minIndex = 1 << 21;
    static uintptr_t base()
    {
#if OS(DARWIN)
        if (uintptr_t base = s_baseOfOnlyCage.load(std::memory_order_relaxed)) [[likely]]
            return base;
        return baseOfCurrentOutOfLine();
#else
        return s_baseOfCurrent;
#endif
    }
    static uintptr_t baseOfCurrent() { return s_baseOfCurrent; }
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

    static MarkedBlockCage* current() { return s_current; }
    static void setCurrent(MarkedBlockCage* cage, uintptr_t base)
    {
        s_current = cage;
        s_baseOfCurrent = base;
    }
    void makeCurrent();

    void* tryAllocateBlock();
    void freeBlock(void*);
    bool owns(const void*) const;
    bool isReserved() const { return m_manager.load(std::memory_order_acquire); }
    // A free block keeps its pages until then.
    static void decommitFreeBlocks();
    static void logExhaustion();
    using ExhaustionHandler = void (*)();
    JS_EXPORT_PRIVATE static void setExhaustionHandler(ExhaustionHandler);

private:
    std::atomic<MarkedBlockCageManager*> m_manager { nullptr };
    bool m_hasTakenFirstManager { false };

    static constinit thread_local MarkedBlockCage* s_current;
    static constinit thread_local uintptr_t s_baseOfCurrent;
#if OS(DARWIN)
#if CPU(ARM64)
    JS_EXPORT_PRIVATE static uintptr_t baseOfCurrentOutOfLine() __attribute__((preserve_most));
#else
    JS_EXPORT_PRIVATE static uintptr_t baseOfCurrentOutOfLine();
#endif
    JS_EXPORT_PRIVATE static std::atomic<uintptr_t> s_baseOfOnlyCage;
#endif
    static std::atomic<bool> s_isRequired;
    static std::atomic<bool> s_hasBlocksOutside;
    static std::atomic<ExhaustionHandler> s_exhaustionHandler;
};

} // namespace JSC
