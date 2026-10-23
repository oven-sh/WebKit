/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCConfig.h"
#include <atomic>
#include <bit>
#include <expected>
#include <limits>
#include <wtf/Assertions.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class HeapCell;
class JSCell;
class ReservedRangeHeap;
class VM;

class MarkedBlockCage {
    WTF_MAKE_NONCOPYABLE(MarkedBlockCage);
public:
    enum class Error : uint8_t { Disabled, InvalidSize, ReservationFailed, BlocksOutsideCage };
    [[nodiscard]] JS_EXPORT_PRIVATE static std::expected<void, Error> initialize();
    [[nodiscard]] JS_EXPORT_PRIVATE static std::expected<void, Error> require();

    MarkedBlockCage();
    ~MarkedBlockCage();

    static bool isEnabled() { return g_jscConfig.markedBlockCageSize; }
    static bool isRequired() { return s_isRequired.load(std::memory_order_relaxed); }
    static void didAllocateOutside() { s_hasAllocationsOutside.store(true, std::memory_order_relaxed); }

    static constexpr unsigned shift = 3;
    static constexpr uint64_t maxSize = 1ull << (32 + shift);
    static constexpr uint32_t minIndex = 1 << 21;
    static uintptr_t baseOf(const void* pointerInCage) { return std::bit_cast<uintptr_t>(pointerInCage) & ~static_cast<uintptr_t>(maxSize - 1); }
    static uint32_t encode(uintptr_t base, const void* cell)
    {
        uintptr_t bits = std::bit_cast<uintptr_t>(cell);
        RELEASE_ASSERT(bits - base < maxSize && !(bits & ((1 << shift) - 1)), bits, base);
        return static_cast<uint32_t>(bits >> shift);
    }
    static JSCell* decode(uintptr_t base, uint32_t index) { return std::bit_cast<JSCell*>(base + (static_cast<uintptr_t>(index) << shift)); }

    uintptr_t base() const { return m_base; }
    size_t size() const { return m_size; }
    bool contains(const void* pointer) const { return std::bit_cast<uintptr_t>(pointer) - m_base < m_size; }

    void* tryAllocateAligned(size_t alignment, size_t);
    void* tryAllocate(size_t);
    void* tryReallocate(void*, size_t);
    void free(void*);

    struct Indices {
        uintptr_t base { 0 };
        uint32_t first { 0 };
        uint32_t count { 0 };
    };
    Indices usedIndices() const;

    void keepReserveOnceHalfFull();
    bool releaseReserve();
    bool releaseLastReserve();
    JS_EXPORT_PRIVATE void didThrowOutOfMemoryError();
    size_t reserveSize() const;

    struct ReservationFailure {
        size_t sizeWithinBudget { 0 };
        size_t smallestSize { 0 };
    };
    JS_EXPORT_PRIVATE static ReservationFailure reservationFailure();

    enum class ExhaustedHeap : bool { Cells, Structures };
    static void logExhaustion(ExhaustedHeap);
    void setExhaustedHeap(ExhaustedHeap exhaustedHeap) { m_exhaustedHeap = exhaustedHeap; }
    JS_EXPORT_PRIVATE String describeExhaustedLimit() const;
    using ExhaustionHandler = void (*)();
    JS_EXPORT_PRIVATE static void setExhaustionHandler(ExhaustionHandler);

    // Called on the thread of the VM, inside an allocation, when the error that is owed has found no place to be thrown and the last of the reserve is given out.
    // A host that can end this thread alone asks for its termination here.
    static void willUseLastReserve(VM&);
    using LastReserveHandler = void (*)(VM&);
    JS_EXPORT_PRIVATE static void setLastReserveHandler(LastReserveHandler);

private:
    void didAllocate(const void*, size_t);

    ReservedRangeHeap* m_range { nullptr };
    uintptr_t m_base { 0 };
    size_t m_size { 0 };
    bool m_sharesRange { false };
    bool m_hasReleasedReserve { false };
    ExhaustedHeap m_exhaustedHeap { ExhaustedHeap::Cells };
    Vector<void*> m_reserve;
    std::atomic<uintptr_t> m_lowestAllocation { std::numeric_limits<uintptr_t>::max() };
    std::atomic<uintptr_t> m_endOfHighestAllocation { 0 };

    static std::atomic<bool> s_isRequired;
    static std::atomic<bool> s_hasAllocationsOutside;
    static std::atomic<ExhaustionHandler> s_exhaustionHandler;
    static std::atomic<LastReserveHandler> s_lastReserveHandler;
};

ALWAYS_INLINE uintptr_t cageBaseOf(const HeapCell* holder)
{
    return MarkedBlockCage::baseOf(holder);
}

template<typename Visitor>
ALWAYS_INLINE void visitNarrowReference(Visitor& visitor, const HeapCell* holder, uint32_t index)
{
    if (index >= MarkedBlockCage::minIndex)
        visitor.appendUnbarriered(MarkedBlockCage::decode(cageBaseOf(holder), index));
}

} // namespace JSC
