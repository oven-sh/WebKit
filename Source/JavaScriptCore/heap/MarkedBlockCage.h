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
#include <wtf/OptionSet.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

#if USE(MIMALLOC)
#include <bmalloc/bmalloc.h>
#endif

namespace JSC {

class HeapCell;
class JSCell;
class JSGlobalObject;
class ReservedRangeHeap;
class ThrowScope;
class VM;

// An allocation that may not fail and has found no room. It lives until the allocation has what it asked for.
class RequestInExhaustedCage {
    WTF_MAKE_NONCOPYABLE(RequestInExhaustedCage);
public:
    RequestInExhaustedCage() = default;

private:
    friend class Heap;
#if USE(MIMALLOC)
    bmalloc::api::RequestWithBallast m_requestWithBallast;
    bool m_hasAskedWithBallast { false };
#endif
};

class MarkedBlockCage {
    WTF_MAKE_NONCOPYABLE(MarkedBlockCage);
public:
    enum class Error : uint8_t { Disabled, InvalidSize, ReservationFailed, BlocksOutsideCage };
    [[nodiscard]] JS_EXPORT_PRIVATE static std::expected<void, Error> initialize();
    // Where every VM shares a range that takes address space as it fills: makes sure that it has room for the cells that this many VMs make when they are created.
    JS_EXPORT_PRIVATE static bool tryEnsureRoomForNewVMs(unsigned count);
    // On the thread of the VM, before the first statement of the program runs. The next VMs are given room for as much as this one has taken.
    JS_EXPORT_PRIVATE void willRunProgram();
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
    uintptr_t start() const { return m_start; }
    size_t size() const { return m_size; }
    bool contains(const void* pointer) const { return std::bit_cast<uintptr_t>(pointer) - m_start < m_size; }
    // Large enough that no address in the image of the executable that fits in 32 bits is the index of a cell or its distance from the base.
    JS_EXPORT_PRIVATE static size_t distanceOfStartFromBase();

    void* tryAllocateAligned(size_t alignment, size_t);
    // On the thread of the VM, where tryAllocateAligned() found no room and releaseLastReserve() has said that there is a reserve beside the range.
    void* tryAllocateAlignedFromSharedReserve(size_t alignment, size_t);
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
    // The last of the reserve of this VM. From here on the VM also takes from the reserve that is kept for all VMs of its range.
    bool releaseLastReserve();
    // The range was full where no collection may run. The collection runs where collections are allowed again. If it makes room for all of the reserve that releaseReserve() gave out, no error is owed any more.
    void didFindNoRoomWhereNothingCollects() { m_owesCollection = true; }
    bool owesCollection() const { return m_owesCollection; }
    JS_EXPORT_PRIVATE bool tryTakeBackOutOfMemoryError(VM&);
    // At a poll the frames of the loop's callees are gone. What they left on the stack is cleared and one more full collection runs. If it makes room, no error is owed any more.
    JS_EXPORT_PRIVATE bool tryTakeBackOutOfMemoryErrorAtPoll(VM&);
    // An error that barren collections owe is offered to a poll first.
    bool errorWaitsForPoll() const { return m_errorWaitsForPoll; }
    JS_EXPORT_PRIVATE void didThrowOutOfMemoryError();
    // A program that catches the error lets go of objects. Words of frames that are gone still name them, and a later frame is scanned with every word under it that it does not write.
    bool hasDeadStackToClear() const { return m_stackPointerAtError; }
    JS_EXPORT_PRIVATE void clearDeadStack(VM&);
    JS_EXPORT_PRIVATE void* takeLowestAddressOfDeadStack(VM&);
    size_t reserveSize() const;

    // Makes fallible allocations use the reserve. For callers that can neither collect nor throw.
    class InfallibleAllocationScope {
        WTF_MAKE_NONCOPYABLE(InfallibleAllocationScope);
    public:
        explicit InfallibleAllocationScope(MarkedBlockCage& cage)
            : m_scope(cage.m_allocationsAreInfallible, true)
        {
        }

    private:
        SetForScope<bool> m_scope;
    };
    bool allocationsAreInfallible() const { return m_allocationsAreInfallible; }

    // Storage: what arrays and objects keep outside their cells. It comes from malloc.
    // Tables: those of the VM that hold a key for each of some kind of value that the program keeps. A table asks malloc for all of its next size at once.
    enum class ExhaustedHeap : uint8_t { Cells = 1 << 0, Structures = 1 << 1, Storage = 1 << 2, Tables = 1 << 3 };
    // Called on the thread of the VM, on the slow path of an allocation, each time there is no room for the request and a full collection is tried next.
    // Without it a heap that stays almost full of live objects runs a full collection for every few objects the program allocates, and the program barely moves.
    JS_EXPORT_PRIVATE void didFindNoRoom(VM&, ExhaustedHeap);
    JS_EXPORT_PRIVATE void didCreateOutOfMemoryError();
    // Where storage is among what the program is owed an error for, this error is that error. So it is for cells under a limit on address space.
    JS_EXPORT_PRIVATE void didCreateOutOfMemoryError(VM&);

    struct ReservationFailure {
        size_t sizeWithinBudget { 0 };
        size_t smallestSize { 0 };
    };
    JS_EXPORT_PRIVATE static ReservationFailure reservationFailure();

    static void logExhaustion(ExhaustedHeap);
    // One error may be owed for several heaps.
    void addExhaustedHeap(ExhaustedHeap exhaustedHeap) { m_exhaustedHeaps.add(exhaustedHeap); }
    JS_EXPORT_PRIVATE String describeExhaustedLimit() const;
    using ExhaustionHandler = void (*)();
    JS_EXPORT_PRIVATE static void setExhaustionHandler(ExhaustionHandler);

    // Called on the thread of the VM, inside an allocation, when the error that is owed has found no place to be thrown and the last of the reserve is given out, or full collections keep freeing next to nothing.
    // A host that can end this thread alone asks for its termination here.
    static void willUseLastReserve(VM&);
    // Called on the thread of the VM when a request of that thread to malloc that may not fail has used the ballast (bmalloc::api::keepAddressSpaceBallast()).
    JS_EXPORT_PRIVATE static void didUseAddressSpaceBallast(VM&);
    // Called on the thread of the VM while pieces of the ballast are missing, ahead of work whose requests to malloc may not fail. Throws if there is no room to keep them again, unless the thread is exempt.
    JS_EXPORT_PRIVATE static bool throwErrorForMissingAddressSpaceBallast(VM&, JSGlobalObject*, ThrowScope&);
    // Called on the thread of the VM ahead of each Symbol that is made. Under a limit on address space the table of Symbols grows ahead of need. Where it finds no memory it goes on with the room it has, and the program is owed an error.
    JS_EXPORT_PRIVATE static void willAddSymbol(VM&);
    using LastReserveHandler = void (*)(VM&);
    JS_EXPORT_PRIVATE static void setLastReserveHandler(LastReserveHandler);

private:
    void didAllocate(const void*, size_t);
    bool hasHeapThatReleasedReserve() const;
    bool tryKeepReleasedReservesAgain(VM&);
    bool tryKeepOwnReleasedReserveAgain();
    bool tryKeepBallastAgain();
    bool releaseOwnLastReserve();
    bool tryUseSharedReserve();

    ReservedRangeHeap* m_range { nullptr };
    uintptr_t m_base { 0 };
    uintptr_t m_start { 0 };
    size_t m_size { 0 };
    bool m_sharesRange { false };
    bool m_isInFirstRange { false };
    bool m_usesSharedReserve { false };
    bool m_owesSharedReserve { false };
    bool m_hasReleasedReserve { false };
    bool m_shouldKeepLargeCellReserve { true };
    bool m_owesCollection { false };
    bool m_errorWaitsForPoll { false };
    bool m_hasCollectedAtPoll { false };
    bool m_allocationsAreInfallible { false };
    void* m_stackPointerAtError { nullptr };
    size_t m_numberOfChunksReleased { 0 };
    std::atomic<size_t> m_numberOfBlocksTaken { 0 };
    OptionSet<ExhaustedHeap> m_exhaustedHeaps;
    unsigned m_numberOfBarrenCollections { 0 };
    uint64_t m_bytesAllocatedWhenLastFull { 0 };
    Vector<void*> m_reserve;
    void* m_largeCellReserve { nullptr };
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
