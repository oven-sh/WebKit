/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTCollectionTestSupport.h"
#include "AOTEmptyPages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTHeapHooks.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "MarkingConstraint.h"
#include "VM.h"
#include <array>
#include <optional>
#include <type_traits>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>

#if OS(LINUX)
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

namespace {

using CallLog = Vector<ASCIILiteral>;
using ScopeLog = Vector<CollectionScope>;

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
    SlotVisitor& collectorVisitor() { return m_vm->heap.collectorSlotVisitor(); }
    AbstractSlotVisitor& verifierVisitor() { return collectorVisitor(); }

private:
    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
};

class CollectingVM : public TestVM {
public:
    CollectingVM()
    {
        vm().aotState().caches = makeUnique<CacheRegistry>();
        for (auto& constraint : createMarkingConstraints(vm()))
            vm().heap.addMarkingConstraint(WTF::move(constraint));
    }

    CacheRegistry& registry() { return *vm().aotState().caches; }
    void collect(CollectionScope scope) { collectWithCollectorHooks(vm(), scope); }
};

struct RootsOwner {
    template<typename Visitor> void visitRoots(Visitor&) { log.append(name); }

    ASCIILiteral name;
    CallLog& log;
};

struct RecordedRootsOwner {
    template<typename Visitor> void visitRoots(Visitor& visitor)
    {
        ++visitCount;
        unsigned& regionCount = std::is_same_v<Visitor, SlotVisitor> ? regionsSeenByCollector : regionsSeenByVerifier;
        writtenRegions.forEachRegionToVisit(visitor, [&](size_t, size_t) {
            ++regionCount;
        });
    }

    RememberedSlotSet writtenRegions;
    unsigned visitCount { 0 };
    unsigned regionsSeenByCollector { 0 };
    unsigned regionsSeenByVerifier { 0 };
};

struct ConditionalRootsOwner {
    template<typename Visitor> void visitRoots(Visitor& visitor) { sourceMarks.append(visitor.isMarked(source)); }

    JSCell* source;
    Vector<bool>& sourceMarks;
};

struct WeakOwner {
    size_t removeDeadEntries(VM&, CollectionScope scope)
    {
        log.append(name);
        scopes.append(scope);
        return 0;
    }

    ASCIILiteral name;
    CallLog& log;
    ScopeLog scopes { };
};

struct TableOfDeadEntries {
    using Slot = RememberedSlot<uint32_t>;

    static constexpr unsigned regionShift = 6;
    static constexpr size_t regionCount = 4;
    static constexpr size_t slotsPerRegion = (1 << regionShift) / sizeof(Slot);

    TableOfDeadEntries()
        : writtenRegions(asByteSpan(std::span<Slot>(slots)), regionShift)
    {
    }

    Slot& firstSlotOfRegion(size_t region) { return slots[region * slotsPerRegion]; }
    void add(size_t region) { firstSlotOfRegion(region).set(writtenRegions, 1); }
    void addWithoutRecord(size_t region) { firstSlotOfRegion(region).setWithoutRecordingForTesting(1); }

    size_t removeDeadEntries(VM&, CollectionScope scope)
    {
        ++callCount;
        size_t removedCount = 0;
        writtenRegions.forEachRegionToSweep(scope, [&](size_t begin, size_t end) {
            ++sweptRegionCount;
            RememberedSlotSet::forEachEntryInRegion(std::span<Slot>(slots), begin, end, [&](Slot& slot) {
                if (!slot)
                    return;
                slot.clear();
                ++removedCount;
            });
        });
        return removedCount;
    }

    std::array<Slot, regionCount * slotsPerRegion> slots { };
    RememberedSlotSet writtenRegions;
    unsigned callCount { 0 };
    unsigned sweptRegionCount { 0 };
};

struct ClearedOwner {
    void clear() { log.append(name); }

    ASCIILiteral name;
    CallLog& log;
};

struct AnalyzedClearedOwner {
    void clear() { }
    void willAnalyzeHeap(VM&) { log.append(name); }

    ASCIILiteral name;
    CallLog& log;
};

struct Observer {
    void didFinishCollection(VM&, CollectionScope scope)
    {
        log.append(name);
        scopes.append(scope);
    }

    ASCIILiteral name;
    CallLog& log;
    ScopeLog scopes { };
};

struct AnalyzedObserver {
    void didFinishCollection(VM&, CollectionScope) { }
    void willAnalyzeHeap(VM&) { log.append(name); }

    ASCIILiteral name;
    CallLog& log;
};

using TestBytes = std::array<uint8_t, 5000>;
constexpr unsigned byteRegionShift = 12;
constexpr size_t byteIndexInFirstRegion = 100;
constexpr size_t byteIndexInSecondRegion = 4500;
static_assert(byteIndexInFirstRegion >> byteRegionShift != byteIndexInSecondRegion >> byteRegionShift);

} // namespace

static void testCallsInRegistrationOrder()
{
    TestVM test;
    CacheRegistry registry;
    CallLog log;
    WeakOwner first { "first"_s, log };
    ClearedOwner second { "second"_s, log };
    WeakOwner third { "third"_s, log };
    registry.add(CacheKind::WeakByKey, first.name, first);
    registry.add(CacheKind::ClearedEachCollection, second.name, second);
    registry.add(CacheKind::WeakByKey, third.name, third);

    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(log == CallLog({ "first"_s, "second"_s, "third"_s }));
}

static void testSpanIsClearedEveryTime()
{
    TestVM test;
    CacheRegistry registry;
    TestBytes bytes { };
    registry.addClearedEachCollection("bytes"_s, bytes);

    bytes[byteIndexInSecondRegion] = 1;
    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(!bytes[byteIndexInSecondRegion]);

    bytes[byteIndexInSecondRegion] = 1;
    registry.finalize(test.vm(), CollectionScope::Full);
    AOT_TEST_CHECK(!bytes[byteIndexInSecondRegion]);
}

static void testWholePagesOfSpanAreGivenBackAndWhatLiesBesideItStays()
{
    TestVM test;
    CacheRegistry registry;
    constexpr size_t sizeOfBorder = 8;
    size_t size = 3 * pageSize();
    std::span<uint8_t> pages { static_cast<uint8_t*>(OSAllocator::reserveAndCommit(size, OSAllocator::FastMallocPages)), size };
    std::span<uint8_t> cleared = pages.subspan(sizeOfBorder, size - 2 * sizeOfBorder);
    registry.addClearedAndGivenBackEachCollection("pages"_s, cleared);
    auto holdsOnlyOnes = [](std::span<const uint8_t> bytes) {
        return std::ranges::all_of(bytes, [](uint8_t byte) { return byte == 1; });
    };
    auto onlyMiddlePageIsAway = [&] {
#if OS(LINUX)
        std::array<unsigned char, 3> isThere { };
        return !mincore(pages.data(), size, isThere.data()) && (isThere[0] & 1) && !(isThere[1] & 1) && (isThere[2] & 1);
#else
        return true;
#endif
    };

    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        memsetSpan(pages, 1);
        AOT_TEST_CHECK(holdsOnlyOnes(pages));
        registry.finalize(test.vm(), scope);
        AOT_TEST_CHECK(onlyMiddlePageIsAway());
        AOT_TEST_CHECK(holdsOnlyOnes(pages.first(sizeOfBorder)) && holdsOnlyOnes(pages.last(sizeOfBorder)));
        AOT_TEST_CHECK(holdsOnlyZeros(cleared));
    }
    OSAllocator::decommitAndRelease(pages.data(), size);
}

static void testIdleSpanPagesAreReleasedAtNextCollection()
{
    TestVM test;
    CacheRegistry registry;
    constexpr size_t sizeOfBorder = 8;
    size_t size = 4 * pageSize();
    std::span<uint8_t> pages { static_cast<uint8_t*>(OSAllocator::reserveAndCommit(size, OSAllocator::FastMallocPages)), size };
    std::span<uint8_t> cleared = pages.subspan(sizeOfBorder, size - 2 * sizeOfBorder);
    registry.addClearedEachCollection("pages"_s, cleared);
#if OS(LINUX)
    auto isAway = [&](size_t page) {
        std::array<unsigned char, 4> isThere { };
        return !mincore(pages.data(), size, isThere.data()) && !(isThere[page] & 1);
    };
    constexpr bool idlePagesAreReleased = true;
#else
    auto isAway = [](size_t) {
        return false;
    };
    constexpr bool idlePagesAreReleased = false;
#endif

    for (CollectionScope scope : { CollectionScope::Eden, CollectionScope::Full }) {
        memsetSpan(pages, 1);
        registry.finalize(test.vm(), scope);
        AOT_TEST_CHECK(!isAway(0) && !isAway(1) && !isAway(2) && !isAway(3));

        pages[2 * pageSize() + sizeOfBorder] = 1;
        registry.finalize(test.vm(), scope);
        AOT_TEST_CHECK(isAway(1) == idlePagesAreReleased && !isAway(0) && !isAway(2) && !isAway(3));

        registry.finalize(test.vm(), scope);
        AOT_TEST_CHECK(isAway(2) == idlePagesAreReleased && !isAway(0) && !isAway(3));
        AOT_TEST_CHECK(holdsOnlyZeros(cleared) && pages.front() == 1 && pages.back() == 1);
    }
    OSAllocator::decommitAndRelease(pages.data(), size);
}

static void testWeakOwnerIsAskedAtEveryCollection()
{
    TestVM test;
    CacheRegistry registry;
    CallLog log;
    WeakOwner owner { "owner"_s, log };
    registry.add(CacheKind::WeakByKey, owner.name, owner);

    registry.finalize(test.vm(), CollectionScope::Eden);
    registry.finalize(test.vm(), CollectionScope::Eden);
    registry.finalize(test.vm(), CollectionScope::Full);
    AOT_TEST_CHECK(owner.scopes == ScopeLog({ CollectionScope::Eden, CollectionScope::Eden, CollectionScope::Full }));
}

static void testWeakOwnerSweepsNoRegionInEdenUntilRecordAll()
{
    TestVM test;
    CacheRegistry registry;
    TableOfDeadEntries table;
    registry.add(CacheKind::WeakByKey, "table"_s, table);

    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(table.callCount == 1 && !table.sweptRegionCount);

    table.writtenRegions.recordAll();
    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(table.callCount == 2 && table.sweptRegionCount == TableOfDeadEntries::regionCount);

    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(table.callCount == 3 && table.sweptRegionCount == TableOfDeadEntries::regionCount);
}

static void testWeakOwnerSweepsScannedRegionsInFull()
{
    TestVM test;
    CacheRegistry registry;
    TableOfDeadEntries table;
    registry.add(CacheKind::WeakByKey, "table"_s, table);

    registry.finalize(test.vm(), CollectionScope::Full);
    AOT_TEST_CHECK(!table.sweptRegionCount);

    table.writtenRegions.recordAll();
    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(table.sweptRegionCount == TableOfDeadEntries::regionCount);

    registry.finalize(test.vm(), CollectionScope::Full);
    AOT_TEST_CHECK(table.sweptRegionCount == 2 * TableOfDeadEntries::regionCount);
}

static void testWeakOwnerSweepsOnlyWrittenRegions()
{
    TestVM test;
    CacheRegistry registry;
    TableOfDeadEntries table;
    registry.add(CacheKind::WeakByKey, "table"_s, table);

    table.addWithoutRecord(0);
    table.add(2);
    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(table.sweptRegionCount == 1);
    AOT_TEST_CHECK(!table.firstSlotOfRegion(2));
    AOT_TEST_CHECK(table.firstSlotOfRegion(0));
}

static void testRootsAreVisitedOnEveryRun()
{
    TestVM test;
    CacheRegistry registry;
    CallLog log;
    RootsOwner owner { "owner"_s, log };
    registry.add(CacheKind::StrongRoots, owner.name, owner);

    registry.finalize(test.vm(), CollectionScope::Eden);
    registry.visitRoots(test.collectorVisitor(), RootVolatility::GreyedByMarking);
    registry.visitRoots(test.collectorVisitor(), RootVolatility::GreyedByMarking);
    AOT_TEST_CHECK(log.size() == 2);
}

static void testRootsSeeCellsMarkedBetweenRuns()
{
    TestVM test;
    VM& vm = test.vm();
    CacheRegistry registry;
    Vector<bool> sourceMarks;
    Strong<JSString> source(vm, jsNontrivialString(vm, "source"_s));
    ConditionalRootsOwner owner { source.get(), sourceMarks };
    registry.add(CacheKind::StrongRoots, "owner"_s, owner);

    registry.visitRoots(test.collectorVisitor(), RootVolatility::GreyedByMarking);
    vm.heap.collectSync(CollectionScope::Full);
    registry.visitRoots(test.collectorVisitor(), RootVolatility::GreyedByMarking);
    AOT_TEST_CHECK(sourceMarks == Vector<bool>({ false, true }));
}

static void testRecordedRootsAreSkippedInEdenUntilRecordAll()
{
    CollectingVM test;
    RecordedRootsOwner owner;
    test.registry().add(CacheKind::StrongRoots, "owner"_s, owner);

    test.collect(CollectionScope::Eden);
    AOT_TEST_CHECK(owner.visitCount && !owner.regionsSeenByCollector);

    owner.writtenRegions.recordAll();
    test.collect(CollectionScope::Eden);
    AOT_TEST_CHECK(owner.regionsSeenByCollector == 1);

    test.collect(CollectionScope::Eden);
    AOT_TEST_CHECK(owner.regionsSeenByCollector == 1);

    owner.writtenRegions.recordAll();
    test.collect(CollectionScope::Eden);
    AOT_TEST_CHECK(owner.regionsSeenByCollector == 2);
}

static void testRecordedRootsAreVisitedOnceInFull()
{
    CollectingVM test;
    RecordedRootsOwner owner;
    test.registry().add(CacheKind::StrongRoots, "owner"_s, owner);

    test.collect(CollectionScope::Full);
    AOT_TEST_CHECK(owner.visitCount && !owner.regionsSeenByCollector);

    owner.writtenRegions.recordAll();
    test.collect(CollectionScope::Eden);
    AOT_TEST_CHECK(owner.regionsSeenByCollector == 1);

    test.collect(CollectionScope::Full);
    AOT_TEST_CHECK(owner.regionsSeenByCollector == 2);

    test.collect(CollectionScope::Full);
    AOT_TEST_CHECK(owner.regionsSeenByCollector == 3);
}

static void testExecutionRootsAreAlwaysVisited()
{
    TestVM test;
    CacheRegistry registry;
    CallLog log;
    RootsOwner owner { "owner"_s, log };
    registry.add(CacheKind::StrongRoots, owner.name, owner, RootVolatility::GreyedByExecution);

    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(!registry.hasVisitedExecutionRoots());

    registry.visitRoots(test.collectorVisitor(), RootVolatility::GreyedByExecution);
    AOT_TEST_CHECK(log.size() == 1);
    AOT_TEST_CHECK(registry.hasVisitedExecutionRoots());

    registry.visitRoots(test.collectorVisitor(), RootVolatility::GreyedByExecution);
    AOT_TEST_CHECK(log.size() == 2);

    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(!registry.hasVisitedExecutionRoots());
}

static void testVerifierAlwaysVisits()
{
    TestVM test;
    CacheRegistry registry;
    CallLog log;
    RootsOwner markingOwner { "marking"_s, log };
    RootsOwner executionOwner { "execution"_s, log };
    registry.add(CacheKind::StrongRoots, markingOwner.name, markingOwner);
    registry.add(CacheKind::StrongRoots, executionOwner.name, executionOwner, RootVolatility::GreyedByExecution);

    registry.finalize(test.vm(), CollectionScope::Eden);

    registry.visitRoots(test.verifierVisitor(), RootVolatility::GreyedByMarking);
    registry.visitRoots(test.verifierVisitor(), RootVolatility::GreyedByMarking);
    AOT_TEST_CHECK(log == CallLog({ "marking"_s, "marking"_s }));

    registry.visitRoots(test.verifierVisitor(), RootVolatility::GreyedByExecution);
    AOT_TEST_CHECK(log == CallLog({ "marking"_s, "marking"_s, "execution"_s }));
    AOT_TEST_CHECK(!registry.hasVisitedExecutionRoots());

    registry.visitRoots(test.collectorVisitor(), RootVolatility::GreyedByMarking);
    AOT_TEST_CHECK(log == CallLog({ "marking"_s, "marking"_s, "execution"_s, "marking"_s }));
}

static void testVerifierSeesWrittenRegionsAndChangesNoCard()
{
    TestVM test;
    CacheRegistry registry;
    RecordedRootsOwner owner;
    registry.add(CacheKind::StrongRoots, "owner"_s, owner);

    registry.visitRoots(test.verifierVisitor(), RootVolatility::GreyedByMarking);
    AOT_TEST_CHECK(owner.visitCount == 1 && !owner.regionsSeenByVerifier);

    owner.writtenRegions.recordAll();
    registry.visitRoots(test.verifierVisitor(), RootVolatility::GreyedByMarking);
    registry.visitRoots(test.verifierVisitor(), RootVolatility::GreyedByMarking);
    AOT_TEST_CHECK(owner.regionsSeenByVerifier == 2);
    AOT_TEST_CHECK(owner.writtenRegions.hasWrittenRegions());
    AOT_TEST_CHECK(!owner.regionsSeenByCollector);
}

static void testVolatilitySelectsEntries()
{
    TestVM test;
    CacheRegistry registry;
    CallLog log;
    RootsOwner markingOwner { "marking"_s, log };
    RootsOwner executionOwner { "execution"_s, log };
    registry.add(CacheKind::StrongRoots, markingOwner.name, markingOwner);
    registry.add(CacheKind::StrongRoots, executionOwner.name, executionOwner, RootVolatility::GreyedByExecution);

    registry.visitRoots(test.collectorVisitor(), RootVolatility::GreyedByMarking);
    AOT_TEST_CHECK(log == CallLog({ "marking"_s }));
    AOT_TEST_CHECK(!registry.hasVisitedExecutionRoots());

    registry.visitRoots(test.collectorVisitor(), RootVolatility::GreyedByExecution);
    AOT_TEST_CHECK(log == CallLog({ "marking"_s, "execution"_s }));
}

static void testObserversRunAfterCaches()
{
    TestVM test;
    CacheRegistry registry;
    CallLog log;
    Observer firstObserver { "first observer"_s, log };
    WeakOwner weakOwner { "weak"_s, log };
    Observer secondObserver { "second observer"_s, log };
    ClearedOwner clearedOwner { "cleared"_s, log };
    registry.addObserver(firstObserver.name, firstObserver);
    registry.add(CacheKind::WeakByKey, weakOwner.name, weakOwner);
    registry.addObserver(secondObserver.name, secondObserver);
    registry.add(CacheKind::ClearedEachCollection, clearedOwner.name, clearedOwner);

    registry.finalize(test.vm(), CollectionScope::Eden);
    AOT_TEST_CHECK(log == CallLog({ "weak"_s, "cleared"_s, "first observer"_s, "second observer"_s }));

    registry.finalize(test.vm(), CollectionScope::Full);
    AOT_TEST_CHECK(firstObserver.scopes == ScopeLog({ CollectionScope::Eden, CollectionScope::Full }));
    AOT_TEST_CHECK(secondObserver.scopes == firstObserver.scopes);
}

static void testHeapAnalysisReachesOwnersThatAskForIt()
{
    TestVM test;
    CacheRegistry registry;
    CallLog log;
    ClearedOwner plainOwner { "plain"_s, log };
    AnalyzedObserver observer { "observer"_s, log };
    AnalyzedClearedOwner cache { "cache"_s, log };
    registry.add(CacheKind::ClearedEachCollection, plainOwner.name, plainOwner);
    registry.addObserver(observer.name, observer);
    registry.add(CacheKind::ClearedEachCollection, cache.name, cache);

    registry.willAnalyzeHeap(test.vm());
    AOT_TEST_CHECK(log == CallLog({ "observer"_s, "cache"_s }));
}

std::span<const NativeTest> cacheRegistryTests()
{
    static constexpr NativeTest tests[] = {
        { "callsInRegistrationOrder"_s, testCallsInRegistrationOrder },
        { "spanIsClearedEveryTime"_s, testSpanIsClearedEveryTime },
        { "wholePagesOfSpanAreGivenBackAndWhatLiesBesideItStays"_s, testWholePagesOfSpanAreGivenBackAndWhatLiesBesideItStays },
        { "idleSpanPagesAreReleasedAtNextCollection"_s, testIdleSpanPagesAreReleasedAtNextCollection },
        { "weakOwnerIsAskedAtEveryCollection"_s, testWeakOwnerIsAskedAtEveryCollection },
        { "weakOwnerSweepsNoRegionInEdenUntilRecordAll"_s, testWeakOwnerSweepsNoRegionInEdenUntilRecordAll },
        { "weakOwnerSweepsScannedRegionsInFull"_s, testWeakOwnerSweepsScannedRegionsInFull },
        { "weakOwnerSweepsOnlyWrittenRegions"_s, testWeakOwnerSweepsOnlyWrittenRegions },
        { "rootsAreVisitedOnEveryRun"_s, testRootsAreVisitedOnEveryRun },
        { "rootsSeeCellsMarkedBetweenRuns"_s, testRootsSeeCellsMarkedBetweenRuns },
        { "recordedRootsAreSkippedInEdenUntilRecordAll"_s, testRecordedRootsAreSkippedInEdenUntilRecordAll },
        { "recordedRootsAreVisitedOnceInFull"_s, testRecordedRootsAreVisitedOnceInFull },
        { "executionRootsAreAlwaysVisited"_s, testExecutionRootsAreAlwaysVisited },
        { "verifierAlwaysVisits"_s, testVerifierAlwaysVisits },
        { "verifierSeesWrittenRegionsAndChangesNoCard"_s, testVerifierSeesWrittenRegionsAndChangesNoCard },
        { "volatilitySelectsEntries"_s, testVolatilitySelectsEntries },
        { "observersRunAfterCaches"_s, testObserversRunAfterCaches },
        { "heapAnalysisReachesOwnersThatAskForIt"_s, testHeapAnalysisReachesOwnersThatAskForIt },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
