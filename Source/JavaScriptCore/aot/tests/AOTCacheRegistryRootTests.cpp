/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTVector.h"
#include "AbstractSlotVisitorInlines.h"
#include "HeapInlines.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "VM.h"
#include <array>
#include <optional>
#include <wtf/ConcurrentPtrHashSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

namespace {

using CellList = Vector<JSCell*>;

class TestVM {
    WTF_MAKE_NONCOPYABLE(TestVM);
public:
    TestVM()
        : m_vm(VM::create())
    {
        m_lock.emplace(*m_vm);
    }

    ~TestVM()
    {
        m_vm = nullptr;
        m_lock.reset();
    }

    VM& vm() { return *m_vm; }
    JSCell* firstCell() { return m_vm->smallStrings.emptyString(); }
    JSCell* secondCell() { return m_vm->structureStructure.get(); }

private:
    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
};

class RecordingVisitor final : public AbstractSlotVisitor {
public:
    explicit RecordingVisitor(VM& vm)
        : AbstractSlotVisitor(vm.heap.collector(), "Recording"_s, m_opaqueRootStorage)
    {
    }

    const CellList& cells() const { return m_cells; }

    void appendUnbarriered(JSCell* cell) final { m_cells.append(cell); }

    void append(const ConservativeRoots&) final { RELEASE_ASSERT_NOT_REACHED(); }
    void appendHiddenUnbarriered(JSCell*) final { RELEASE_ASSERT_NOT_REACHED(); }
    void markAuxiliary(const void*) final { RELEASE_ASSERT_NOT_REACHED(); }
    void visitAsConstraint(const JSCell*) final { RELEASE_ASSERT_NOT_REACHED(); }
    void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>) final { RELEASE_ASSERT_NOT_REACHED(); }
    void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { RELEASE_ASSERT_NOT_REACHED(); }

    bool isFirstVisit() const final { return true; }
    bool isMarked(const void*) const final { return false; }
    bool isMarked(MarkedBlock&, HeapCell*) const final { return false; }
    bool isMarked(PreciseAllocation&, HeapCell*) const final { return false; }
    bool mutatorsAreStopped() const final { return true; }
    void reportExtraMemoryVisited(size_t) final { }
#if ENABLE(RESOURCE_USAGE)
    void reportExternalMemoryVisited(size_t) final { }
#endif
    void didRace(const VisitRaceKey&) final { }

private:
    ConcurrentPtrHashSet m_opaqueRootStorage;
    CellList m_cells;
};

} // namespace

static CellList visitedCells(TestVM& test, CacheRegistry& registry, RootVolatility volatility)
{
    RecordingVisitor visitor(test.vm());
    AbstractSlotVisitor& abstractVisitor = visitor;
    registry.visitRoots(abstractVisitor, volatility);
    return visitor.cells();
}

static void testStrongRootsAreAppendedInOrder()
{
    TestVM test;
    CacheRegistry registry;
    std::array<JSCell*, 2> cells { test.firstCell(), test.secondCell() };
    registry.addStrongRoots("cells"_s, cells);

    AOT_TEST_CHECK(visitedCells(test, registry, RootVolatility::GreyedByMarking) == CellList({ test.firstCell(), test.secondCell() }));
}

static void testNullStrongRootsAreSkipped()
{
    TestVM test;
    CacheRegistry registry;
    std::array<JSCell*, 4> cells { nullptr, test.firstCell(), nullptr, test.secondCell() };
    std::array<JSCell*, 2> emptyCells { };
    registry.addStrongRoots("cells"_s, cells);
    registry.addStrongRoots("empty cells"_s, emptyCells);
    registry.addStrongRoots("no cells"_s, { });

    AOT_TEST_CHECK(visitedCells(test, registry, RootVolatility::GreyedByMarking) == CellList({ test.firstCell(), test.secondCell() }));
}

static void testStrongRootsAreReadAtEachVisit()
{
    TestVM test;
    CacheRegistry registry;
    std::array<JSCell*, 2> cells { };
    registry.addStrongRoots("cells"_s, cells);
    AOT_TEST_CHECK(visitedCells(test, registry, RootVolatility::GreyedByMarking).isEmpty());

    cells[1] = test.secondCell();
    AOT_TEST_CHECK(visitedCells(test, registry, RootVolatility::GreyedByMarking) == CellList({ test.secondCell() }));

    cells = { test.firstCell(), nullptr };
    AOT_TEST_CHECK(visitedCells(test, registry, RootVolatility::GreyedByMarking) == CellList({ test.firstCell() }));
}

static void testStrongRootsAreNotExecutionRoots()
{
    TestVM test;
    CacheRegistry registry;
    std::array<JSCell*, 1> cells { test.firstCell() };
    registry.addStrongRoots("cells"_s, cells);

    AOT_TEST_CHECK(visitedCells(test, registry, RootVolatility::GreyedByExecution).isEmpty());
}

static void testExecutionStrongRootsAreNotMarkingRoots()
{
    TestVM test;
    CacheRegistry registry;
    std::array<JSCell*, 1> markingCells { test.firstCell() };
    std::array<JSCell*, 1> executionCells { test.secondCell() };
    registry.addStrongRoots("marking cells"_s, markingCells);
    registry.addStrongRoots("execution cells"_s, executionCells, RootVolatility::GreyedByExecution);

    AOT_TEST_CHECK(visitedCells(test, registry, RootVolatility::GreyedByMarking) == CellList({ test.firstCell() }));
    AOT_TEST_CHECK(visitedCells(test, registry, RootVolatility::GreyedByExecution) == CellList({ test.secondCell() }));
}

std::span<const NativeTest> cacheRegistryRootTests()
{
    static constexpr NativeTest tests[] = {
        { "strongRootsAreAppendedInOrder"_s, testStrongRootsAreAppendedInOrder },
        { "nullStrongRootsAreSkipped"_s, testNullStrongRootsAreSkipped },
        { "strongRootsAreReadAtEachVisit"_s, testStrongRootsAreReadAtEachVisit },
        { "strongRootsAreNotExecutionRoots"_s, testStrongRootsAreNotExecutionRoots },
        { "executionStrongRootsAreNotMarkingRoots"_s, testExecutionStrongRootsAreNotMarkingRoots },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
