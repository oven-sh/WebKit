/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "Options.h"
#include "VM.h"
#include <array>
#include <tuple>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class LocalAllocator;

namespace AOT {

struct Instance;

// Which places of the program allocate, in a run with --aotCountsPath.
class AllocationCounts {
    WTF_MAKE_TZONE_ALLOCATED(AllocationCounts);
    WTF_MAKE_NONCOPYABLE(AllocationCounts);
public:
    explicit AllocationCounts(Instance&);
    ~AllocationCounts();

    static bool isEnabled() { return !!Options::aotCountsPath(); }

    // For an operation that compiled code calls where it found no room, before the operation allocates.
    static void willAllocate(Instance&, uint32_t allocation);
    static void willAllocateClosure(Instance&, uint64_t request);
    static void willAllocateMap(Instance&);
    static void willAllocateSet(Instance&);
    // For the engine, where an allocator has used up its free list, whoever took the cells: compiled code, a stub or C++.
    static void willRefill(VM&, const LocalAllocator&);
    // For the engine, where it makes something that is dearer than its bytes: a Structure, a bound function. Every one is counted, under the innermost compiled frame.
    JS_EXPORT_PRIVATE static void didMake(VM&, ASCIILiteral what, const char* detailOrNull = nullptr);

    // "allocation <function> <file>:<line> <layout> <cells> <bytes>": six words with one space between them, the most bytes first.
    void dump(PrintStream&) const;

private:
    // The first kind is not zero because a key of zeros stands for an empty place in the table.
    enum class Kind : uint32_t { Allocation = 1, Closure, Map, Set };
    struct Allocated {
        Kind kind;
        uint32_t id { 0 };
    };
    struct Line {
        String words;
        uint64_t cells { 0 };
        uint64_t bytes { 0 };
    };
    // The function of the image and the call site in it, then what is allocated there.
    using Key = std::tuple<uint64_t, uint32_t, uint32_t>;

    enum class ReasonForNoRoom : uint8_t { FirstAllocationAtSite, NoList, ListOfOneCell, ListOfAtMost4Cells, ListOfAtMost16Cells, ListOfAtMost64Cells, LongerList };
    static constexpr unsigned numberOfReasonsForNoRoom = 7;

    static AllocationCounts& ensure(Instance&);
    static void countFreeList(Instance&, const void* allocator, Allocated);
    void add(Allocated, uint64_t cells, uint64_t bytes);
    void addRefill(const LocalAllocator&, uint64_t cells, uint64_t bytes);
    String describeLayout(Allocated) const;
    // The copies of one place, in the functions it was inlined into and in the versions of its function, share a line.
    uint32_t numberOfLine(const String& words);

    Instance& m_instance;
    UncheckedKeyHashMap<Key, uint32_t> m_linesByCallSite;
    UncheckedKeyHashMap<String, uint32_t> m_linesByWords;
    Vector<Line> m_lines;
    UncheckedKeyHashMap<String, uint32_t> m_refillsByWords;
    Vector<Line> m_refills;
    UncheckedKeyHashMap<String, uint32_t> m_madeByWords;
    Vector<Line> m_made;
    std::array<uint64_t, numberOfReasonsForNoRoom> m_slowAllocations { };
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

    // "allocation - - <layout> <cells> <bytes>" for a cell with an allocation, "allocation - - stub:<family> <cells> <bytes>" for any other, "allocation - - code <cells> <bytes>" for what compiled code allocates in line.
    static void dump(const Instance&, PrintStream&);
};

// For an operation that allocates and that a stub calls without recording a frame: vm.topCallFrame is what an earlier operation left behind, and that frame is gone.
class LeafOperationScope {
    WTF_MAKE_NONCOPYABLE(LeafOperationScope);
public:
    explicit LeafOperationScope(VM& vm)
        : m_vm(vm)
        , m_topCallFrame(vm.topCallFrame)
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
};

} // namespace AOT

} // namespace JSC

#endif // ENABLE(AOT)
