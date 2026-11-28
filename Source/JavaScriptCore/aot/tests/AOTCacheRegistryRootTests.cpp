/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTDroppedClosureTestSupport.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTVMState.h"
#include "AOTVector.h"
#include "AbstractSlotVisitorInlines.h"
#include "HeapInlines.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "VM.h"
#include <array>
#include <optional>
#include <stdlib.h>
#include <wtf/ConcurrentPtrHashSet.h>
#include <wtf/DataLog.h>
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

class VisitorBehindMarking final : public AbstractSlotVisitor {
public:
    VisitorBehindMarking(VM& vm, std::initializer_list<JSCell*> dropped)
        : AbstractSlotVisitor(vm.heap.collector(), "Behind marking"_s, m_opaqueRootStorage)
        , m_dropped(dropped)
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
    bool isMarked(const void* cell) const final { return !wasDropped(cell); }
    bool isMarked(MarkedBlock&, HeapCell* cell) const final { return !wasDropped(cell); }
    bool isMarked(PreciseAllocation&, HeapCell* cell) const final { return !wasDropped(cell); }
    bool mutatorsAreStopped() const final { return true; }
    void reportExtraMemoryVisited(size_t) final { }
#if ENABLE(RESOURCE_USAGE)
    void reportExternalMemoryVisited(size_t) final { }
#endif
    void didRace(const VisitRaceKey&) final { }

private:
    bool wasDropped(const void* cell) const
    {
        return std::ranges::any_of(m_dropped, [&](JSCell* dropped) {
            return dropped == cell;
        });
    }

    ConcurrentPtrHashSet m_opaqueRootStorage;
    CellList m_dropped;
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

static CellList rootsOfRunTime(LoadedProgram& program, std::initializer_list<JSCell*> dropped)
{
    VisitorBehindMarking visitor(program.vm(), dropped);
    AbstractSlotVisitor& abstractVisitor = visitor;
    for (CacheRegistry* registry : { program.state().caches.get(), program.vm().aotState().caches.get() }) {
        for (RootVolatility volatility : { RootVolatility::GreyedByMarking, RootVolatility::GreyedByExecution })
            registry->visitRoots(abstractVisitor, volatility);
    }
    return visitor.cells();
}

static void testNoTableRootsClosureGivenDisplayName(LoadedProgram& program)
{
    VM& vm = program.vm();
    bool hasSeenSlimClosures = false;
    bool hasSeenFullClosures = false;
    unsigned number = 0;
    for (ASCIILiteral name : namesOfFunctionsWithClosuresToDrop) {
        FunctionID function = findFunctionWithClosuresToDrop(program, name);
        (functionKindData(function).isSlim ? hasSeenSlimClosures : hasSeenFullClosures) = true;
        for (bool hasStructureOfItsOwn : { false, true }) {
            JSString* text = createTextToDrop(program, number++);
            JSObject* source = createSourceOfDisplayName(program, text);
            JSObject* closure = hasStructureOfItsOwn ? createClosureWithStructureOfItsOwn(program, function, source) : createClosureWithSharedStructure(program, function, source);
            AOT_TEST_CHECK(closure->get(program.realm(), vm.propertyNames->displayName) == JSValue(text));
            fileReadOfDisplayName(program, closure);
            fillSiteWithReadOfDisplayName(program, closure);

            CellList roots = rootsOfRunTime(program, { closure, source, text });
            AOT_TEST_CHECK(!roots.isEmpty());
            AOT_TEST_CHECK(!roots.contains(closure) && !roots.contains(source) && !roots.contains(text));
        }
    }
    AOT_TEST_CHECK(hasSeenSlimClosures && hasSeenFullClosures);
}

static void testSiteRootsStructureThatNextObjectGets(LoadedProgram& program)
{
    for (ASCIILiteral name : namesOfFunctionsWithClosuresToDrop) {
        FunctionID function = findFunctionWithClosuresToDrop(program, name);
        JSObject* closure = createClosureWithSharedStructure(program, function, createSourceOfDisplayName(program, createTextToDrop(program, 0)));
        Structure* structure = closure->structure();
        AOT_TEST_CHECK(!rootsOfRunTime(program, { closure, structure }).contains(structure));

        fileReadOfDisplayName(program, closure);
        AOT_TEST_CHECK(!rootsOfRunTime(program, { closure, structure }).contains(structure));

        fillSiteWithReadOfDisplayName(program, closure);
        AOT_TEST_CHECK(rootsOfRunTime(program, { closure, structure }).contains(structure));
        AOT_TEST_CHECK(!rootsOfRunTime(program, { closure, structure, structure->storedPrototypeObject() }).contains(structure));
        program.state().propertyCaches->siteCaches().clear(siteOfTestsOfDroppedClosures(program));
    }
}

static void testSitePastEndOfSiteCachesOnRequest(LoadedProgram& program)
{
    SiteCaches& sites = program.state().propertyCaches->siteCaches();
    SiteCache first = siteOfTestsOfDroppedClosures(program);
    AOT_TEST_CHECK(!sites.indexOf(first));
    if (getenv(crashRequestVariable))
        dataLogLn(sites.indexOf(std::bit_cast<SiteCache>(std::bit_cast<uintptr_t>(first) + program.instance().siteCaches().size_bytes())));
}

static void testSiteBetweenTwoSlotsOnRequest(LoadedProgram& program)
{
    SiteCaches& sites = program.state().propertyCaches->siteCaches();
    SiteCache first = siteOfTestsOfDroppedClosures(program);
    AOT_TEST_CHECK(!sites.indexOf(first));
    if (getenv(crashRequestVariable))
        dataLogLn(sites.indexOf(std::bit_cast<SiteCache>(std::bit_cast<uintptr_t>(first) + 1)));
}

static void testSiteDoesNotRootStructureOfOneObject(LoadedProgram& program)
{
    for (ASCIILiteral name : namesOfFunctionsWithClosuresToDrop) {
        FunctionID function = findFunctionWithClosuresToDrop(program, name);
        JSObject* closure = createClosureWithStructureOfItsOwn(program, function, createSourceOfDisplayName(program, createTextToDrop(program, 0)));
        Structure* structure = closure->structure();
        fileReadOfDisplayName(program, closure);
        fillSiteWithReadOfDisplayName(program, closure);
        AOT_TEST_CHECK(!rootsOfRunTime(program, { closure, structure }).contains(structure));
        program.state().propertyCaches->siteCaches().clear(siteOfTestsOfDroppedClosures(program));
    }
}

template<LoadedProgramTest body>
static void changeObjectsUnseen(LoadedProgram& program)
{
    program.vm().aotState().nativeTestChangesObjectsUnseen = true;
    body(program);
}

template<LoadedProgramTest body>
static constexpr void (*withClosuresToDrop)() = runNativeTestWithLoadedProgram<cacheRegistryRootTests, programWithClosuresToDrop, changeObjectsUnseen<body>>;

std::span<const NativeTest> cacheRegistryRootTests()
{
    static constexpr NativeTest tests[] = {
        { "cache-registry-roots/no-table-roots-closure-given-display-name"_s, withClosuresToDrop<testNoTableRootsClosureGivenDisplayName> },
        { "cache-registry-roots/site-roots-structure-that-next-object-gets"_s, withClosuresToDrop<testSiteRootsStructureThatNextObjectGets> },
        { "ends-on-request/site-past-the-end-of-the-site-caches"_s, withClosuresToDrop<testSitePastEndOfSiteCachesOnRequest> },
        { "ends-on-request/site-between-two-slots"_s, withClosuresToDrop<testSiteBetweenTwoSlotsOnRequest> },
        { "cache-registry-roots/site-does-not-root-structure-of-one-object"_s, withClosuresToDrop<testSiteDoesNotRootStructureOfOneObject> },
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
