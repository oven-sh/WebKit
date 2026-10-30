/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "DeferGC.h"
#include "Options.h"
#include "VM.h"
#include <array>
#include <tuple>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class LocalAllocator;

namespace AOT {

struct Instance;
struct VMState;

struct CountedCells {
    friend auto operator<=>(const CountedCells&, const CountedCells&) = default;

    uint64_t bytes { 0 };
    uint64_t cells { 0 };
};

// Which places of the program allocate, in a run with --aotCountsPath.
class AllocationCounts {
    WTF_MAKE_TZONE_ALLOCATED(AllocationCounts);
    WTF_MAKE_NONCOPYABLE(AllocationCounts);
public:
    using Lines = UncheckedKeyHashMap<String, CountedCells>;

    AllocationCounts() = default;

    static bool isEnabled() { return !!Options::aotCountsPath(); }

    // For an operation that compiled code calls where it found no room, before the operation allocates.
    static void willAllocate(Instance&, uint32_t allocation);
    static void willAllocateClosure(Instance&, uint64_t request);
    static void willAllocateMap(Instance&);
    static void willAllocateSet(Instance&);
    static void willRefill(VM&, const LocalAllocator&);
    // A collection takes the free list from its allocator, so the used part is counted first.
    static void willStopAllocating(VM&, const LocalAllocator&);
    // A VM's free lists are destroyed with its heap, so their used part is counted here and its allocators are dropped.
    static void willShutDownHeap(VMState&);
    JS_EXPORT_PRIVATE static void didMake(VM&, ASCIILiteral what, const char* detailOrNull = nullptr);

    // "allocation <function> <file>:<line> <layout> <cells> <bytes>", the most bytes first, then "refill", "made" and "slow-allocation" lines.
    static void dump(const Instance&, PrintStream&);

private:
    // What a line counts at its place. A number, an index or an address tells the things of one kind apart.
    enum class Counted : uint8_t { Layout = 1, Closure, Map, Set, CellsOfAllocator };

    static AllocationCounts& ensure(Instance&);
    static AllocationCounts* ensureForFirstInstance(VM&);
    template<typename Functor> const String& wordsAtAllocatingFrame(VM&, Typed::FunctionID constructor, Counted, uint64_t detail, const Functor& describeCounted);

    // A line is put into words once. From then on the numbers of the frame and of what is counted there find it: a loop that allocates comes here with every free list.
    UncheckedKeyHashMap<std::tuple<uint64_t, uint64_t, unsigned>, String> m_wordsOfLines;
    Lines m_allocations;
    Lines m_refills;
    Lines m_made;
    std::array<uint64_t, 7> m_slowAllocations { };
};

// In a program built to count branches every stub that asks an allocator for a cell counts the cell first, and so does compiled code that allocates in line.
struct ExactAllocationCounts {
    static constexpr size_t wordsOfKind = 2;
    static size_t numberOfKinds();
    static size_t kindOfCompiledCode() { return numberOfKinds() - 1; }
    static ptrdiff_t offsetOfCells(size_t kind) { return static_cast<ptrdiff_t>(kind * wordsOfKind * sizeof(uint64_t)); }
    static ptrdiff_t offsetOfBytes(size_t kind) { return offsetOfCells(kind) + static_cast<ptrdiff_t>(sizeof(uint64_t)); }
    static ptrdiff_t offsetOfCellsOfAllocations() { return offsetOfCells(numberOfKinds()); }
    static size_t sizeInBytes(size_t numberOfAllocations) { return offsetOfCellsOfAllocations() + numberOfAllocations * sizeof(uint64_t); }
};

// For an operation that allocates and that a stub calls without recording a frame: vm.topCallFrame is what an earlier operation left behind, and that frame is gone.
// The operation puts collections off while it runs.
class LeafOperationScope {
    WTF_MAKE_NONCOPYABLE(LeafOperationScope);
public:
    explicit LeafOperationScope(VM& vm)
        : m_vm(vm)
        , m_topCallFrame(vm.topCallFrame)
        , m_deferGC(vm)
    {
        if (AllocationCounts::isEnabled()) [[unlikely]]
            vm.topCallFrame = nullptr;
    }

    ~LeafOperationScope()
    {
        if (AllocationCounts::isEnabled()) [[unlikely]]
            m_vm.topCallFrame = m_topCallFrame;
    }

private:
    VM& m_vm;
    CallFrame* m_topCallFrame;
    DeferGCForAWhile m_deferGC;
#if ASSERT_ENABLED
    AssertNoGC m_assertNoGC;
#endif
};

} // namespace AOT

} // namespace JSC

#endif // ENABLE(AOT)
