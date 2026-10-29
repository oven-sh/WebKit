/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTVector.h"
#include "Collector.h"
#include "DeferGCInlines.h"
#include "FastMarkAppender.h"
#include "HeapHelperPool.h"
#include "HeapInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "MarkedSpace.h"
#include "ObjectConstructor.h"
#include "RememberedSlot.h"
#include "SimpleMarkingConstraint.h"
#include "VM.h"
#include "Weak.h"
#include "WeakInlines.h"
#include <optional>
#include <wtf/Condition.h>
#include <wtf/DataLog.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/ParallelHelperPool.h>
#include <wtf/threads/BinarySemaphore.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> collectorTests();

namespace {

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
    Heap& heap() { return m_vm->heap; }
    JSGlobalObject* createRealm() { return JSGlobalObject::create(*m_vm, JSGlobalObject::createStructure(*m_vm, jsNull())); }

private:
    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
};

} // anonymous namespace

template<void (*body)()>
static void runInProcessWithCompiledCodeOptions()
{
    ASCIILiteral name;
    for (const NativeTest& test : collectorTests()) {
        if (test.run == &runInProcessWithCompiledCodeOptions<body>)
            name = test.name;
    }
    AOT_TEST_CHECK(!name.isNull());

    if (Options::useAOT()) {
        body();
        return;
    }

    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(name), "--useAOT=true"_s, "--numberOfGCMarkers=4"_s };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process with the options of compiled code ", result.description());
        failNativeTest(__FILE__, __LINE__, name.characters());
    }
}

static size_t sizeClassOf(size_t size)
{
    return MarkedSpace::s_sizeClassForSizeStep[MarkedSpace::sizeClassToIndex(size)];
}

static size_t sizeBetweenPowersOfTwo(size_t lowerPower)
{
    for (size_t size = lowerPower + MarkedSpace::sizeStep; size < 2 * lowerPower; size += MarkedSpace::sizeStep) {
        if (sizeClassOf(size) == size && size != sizeClassOf(lowerPower) && size != sizeClassOf(2 * lowerPower))
            return size;
    }
    return 0;
}

static HeapCell* allocateAuxiliary(TestVM& test, size_t size)
{
    return static_cast<HeapCell*>(test.heap().auxiliarySpace.allocate(test.vm(), size, nullptr, AllocationFailureMode::Assert));
}

static void checkFirstCellsOfSizeClassAreBorrowed()
{
    TestVM test;
    size_t size = sizeBetweenPowersOfTwo(1024);
    AOT_TEST_CHECK(size);
    size_t largerSize = sizeClassOf(2048);

    unsigned numberOfBorrowedCells = 0;
    HeapCell* cell = allocateAuxiliary(test, size);
    for (; cell->cellSize() == largerSize; cell = allocateAuxiliary(test, size))
        ++numberOfBorrowedCells;
    AOT_TEST_CHECK(numberOfBorrowedCells == 4 * KB / size);
    AOT_TEST_CHECK(cell->cellSize() == size);
    for (unsigned count = 0; count < 100; ++count)
        AOT_TEST_CHECK(allocateAuxiliary(test, size)->cellSize() == size);
}

static void checkAtMostSixteenCellsAreBorrowed()
{
    TestVM test;
    size_t size = sizeBetweenPowersOfTwo(128);
    AOT_TEST_CHECK(size);
    size_t largerSize = sizeClassOf(256);

    for (unsigned count = 0; count < 16; ++count)
        AOT_TEST_CHECK(allocateAuxiliary(test, size)->cellSize() == largerSize);
    AOT_TEST_CHECK(allocateAuxiliary(test, size)->cellSize() == size);
}

static void checkPowersOfTwoAndLargeCellsAreNotBorrowed()
{
    TestVM test;
    for (size_t size : { 64, 256, 1024, 2048 })
        AOT_TEST_CHECK(allocateAuxiliary(test, size)->cellSize() == sizeClassOf(size));
    size_t large = sizeBetweenPowersOfTwo(2048);
    AOT_TEST_CHECK(large && allocateAuxiliary(test, large)->cellSize() == large);
}

static void checkBorrowedCellSurvivesCollection()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    VM& vm = test.vm();

    unsigned inlineCapacity = 0;
    for (unsigned candidate = 17; candidate < 32 && !inlineCapacity; ++candidate) {
        size_t size = JSFinalObject::allocationSize(candidate);
        if (sizeClassOf(size) == size && size != sizeClassOf(128) && size != sizeClassOf(256) && size < 256)
            inlineCapacity = candidate;
    }
    AOT_TEST_CHECK(inlineCapacity);

    Structure* structure = JSFinalObject::createStructure(vm, realm, realm->objectPrototype(), inlineCapacity);
    JSFinalObject* object = JSFinalObject::create(vm, structure);
    if (object->cellSize() != sizeClassOf(256))
        return;
    JSObject* value = constructEmptyObject(realm);
    object->putDirect(vm, Identifier::fromString(vm, "value"_s), value);
    Weak<JSObject> weakValue(value);
    value = nullptr;

    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(weakValue.get() && object->getDirect(vm, Identifier::fromString(vm, "value"_s)) == JSValue(weakValue.get()));
    ensureStillAliveHere(object);
}

static void testNothingIsBorrowedWithoutCompiledCode()
{
    if (Options::useAOT())
        return;
    TestVM test;
    for (size_t lowerPower : { 128, 512, 1024 }) {
        size_t size = sizeBetweenPowersOfTwo(lowerPower);
        if (size)
            AOT_TEST_CHECK(allocateAuxiliary(test, size)->cellSize() == size);
    }
}

static void checkSmallHeapStartsNoHelper()
{
    AOT_TEST_CHECK(!heapHelperPool().numberOfThreads());
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    test.heap().collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(heapHelperPool().numberOfThreads() == 1);

    JSObject* holder = constructEmptyObject(realm);
    for (unsigned index = 0; index < 10; ++index)
        holder->putDirectIndex(realm, index, constructEmptyObject(realm));
    test.heap().collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(!test.heap().didSkipMarkingInLastCollection() && !test.heap().collector().hasStartedHelpers());
    AOT_TEST_CHECK(heapHelperPool().numberOfThreads() == 1);
    ensureStillAliveHere(holder);
}

static void checkWideHeapStartsHelpers()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    VM& vm = test.vm();
    constexpr unsigned length = 200000;
    JSArray* array = JSArray::tryCreate(vm, realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), length);
    AOT_TEST_CHECK(array);
    for (unsigned index = 0; index < length; ++index)
        array->putDirectIndex(realm, index, constructEmptyObject(realm));
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(vm.heap.collector().hasStartedHelpers());
    for (unsigned index = 0; index < length; index += 997)
        AOT_TEST_CHECK(array->getDirectIndex(realm, index).isObject());
    ensureStillAliveHere(array);
}

static void testWaitingForStartedThreadsDoesNotWaitForTheirWork()
{
    // Never destroyed, like the collector's own pool: a thread that is leaving still refers to its pool.
    ParallelHelperPool& pool = ParallelHelperPool::create("Test Helper"_s).leakRef();
    pool.ensureThreads(2);
    BinarySemaphore hasTakenWork;
    Lock lock;
    Condition mayFinishCondition;
    bool mayFinish = false;
    {
        ParallelHelperClient client(&pool);
        client.setFunction([&] {
            hasTakenWork.signal();
            Locker locker { lock };
            while (!mayFinish)
                mayFinishCondition.wait(lock);
        });
        hasTakenWork.wait();
        pool.waitUntilThreadsAreUp();
        {
            Locker locker { lock };
            mayFinish = true;
            mayFinishCondition.notifyAll();
        }
        client.finish();
    }
}

static void testHelpersStartEagerlyWithoutCompiledCode()
{
    if (Options::useAOT())
        return;
    TestVM test;
    test.createRealm();
    AOT_TEST_CHECK(heapHelperPool().numberOfThreads() == Options::numberOfGCMarkers() - 1);
    test.heap().collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(test.heap().collector().hasStartedHelpers());
}

static void checkEdenCollectionWithEmptyEdenSkipsMarking()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    VM& vm = test.vm();
    JSObject* kept = constructEmptyObject(realm);
    Weak<JSObject> weakKept(kept);

    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(!vm.heap.didSkipMarkingInLastCollection());
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(vm.heap.didSkipMarkingInLastCollection() && vm.heap.lastCollectionScope() == CollectionScope::Eden);
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(vm.heap.didSkipMarkingInLastCollection());
    AOT_TEST_CHECK(weakKept.get() == kept);
    ensureStillAliveHere(kept);
}

static void checkAllocationEndsTheSkips()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    VM& vm = test.vm();

    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(vm.heap.didSkipMarkingInLastCollection());

    JSObject* holder = constructEmptyObject(realm);
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(!vm.heap.didSkipMarkingInLastCollection());

    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(vm.heap.didSkipMarkingInLastCollection());

    JSObject* young = constructEmptyObject(realm);
    holder->putDirect(vm, Identifier::fromString(vm, "young"_s), young);
    Weak<JSObject> weakYoung(young);
    young = nullptr;
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(!vm.heap.didSkipMarkingInLastCollection() && weakYoung.get());
    AOT_TEST_CHECK(holder->getDirect(vm, Identifier::fromString(vm, "young"_s)) == JSValue(weakYoung.get()));

    AOT_TEST_CHECK(JSArray::tryCreate(vm, realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 1000000));
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(!vm.heap.didSkipMarkingInLastCollection());
    ensureStillAliveHere(holder);
}

static void checkFullCollectionIsNeverSkipped()
{
    TestVM test;
    test.createRealm();
    VM& vm = test.vm();
    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(!vm.heap.didSkipMarkingInLastCollection());
}

NEVER_INLINE static void storeOldObjectIntoOldObject(VM& vm, JSGlobalObject* realm, Weak<JSObject>& weakTarget, JSObject* holder)
{
    JSObject* target = constructEmptyObject(realm);
    JSObject* firstHolder = constructEmptyObject(realm);
    firstHolder->putDirect(vm, Identifier::fromString(vm, "target"_s), target);
    weakTarget = Weak<JSObject>(target);
    vm.heap.collectSync(CollectionScope::Full);
    holder->putDirect(vm, Identifier::fromString(vm, "target"_s), target);
    ensureStillAliveHere(firstHolder);
}

static void checkStoreBeforeSkippedCollectionIsSeenLater()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    VM& vm = test.vm();
    JSObject* holder = constructEmptyObject(realm);
    holder->putDirect(vm, Identifier::fromString(vm, "target"_s), jsNull());
    Weak<JSObject> weakTarget;
    storeOldObjectIntoOldObject(vm, realm, weakTarget, holder);

    vm.heap.collectSync(CollectionScope::Eden);
    vm.heap.collectSync(CollectionScope::Eden);
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(weakTarget.get() && holder->getDirect(vm, Identifier::fromString(vm, "target"_s)) == JSValue(weakTarget.get()));
    ensureStillAliveHere(holder);
}

static void testNoCollectionIsSkippedWithoutCompiledCode()
{
    if (Options::useAOT())
        return;
    TestVM test;
    test.createRealm();
    VM& vm = test.vm();
    vm.heap.collectSync(CollectionScope::Full);
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(!vm.heap.didSkipMarkingInLastCollection());
}

namespace {

struct TableOfRoots {
    using Slot = RememberedSlot<JSCell*>;
    static constexpr unsigned regionShift = 10;
    static constexpr size_t numberOfSlots = 20000;

    TableOfRoots()
        : slots(numberOfSlots, [](size_t) { return Slot { }; })
        , writtenRegions(asByteSpan(std::span<const Slot>(slots.span())), regionShift)
    {
        test.vm().heap.addMarkingConstraint(makeUnique<SimpleMarkingConstraint>(
            "Tr"_s, "Table of Roots of a Test"_s,
            MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
                visitRoots(visitor);
            })),
            ConstraintVolatility::GreyedByExecution,
            ConstraintConcurrency::Concurrent,
            ConstraintParallelism::Sequential));
    }

    template<typename Appender>
    void visitRegion(Appender& appender, size_t begin, size_t end)
    {
        RememberedSlotSet::forEachEntryInRegion(slots.mutableSpan(), begin, end, [&](Slot& slot) {
            if (JSCell* cell = slot.get())
                appender.appendUnbarriered(cell);
        });
    }

    template<typename Visitor>
    void visitRoots(Visitor& visitor)
    {
        bool isDeferred = writtenRegions.tryDeferFullVisit(visitor, [](SlotVisitor& visitor, void* owner, size_t begin, size_t end) {
            auto& table = *static_cast<TableOfRoots*>(owner);
            table.numberOfDeferredVisits.fetch_add(1);
            appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
                table.visitRegion(appender, begin, end);
            });
        }, this);
        if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
            if (isDeferred) {
                ++numberOfDeferrals;
                stackSizeAfterDeferral = visitor.collectorMarkStack().size();
                if (cellToStoreAfterDeferral)
                    slots[0].set(writtenRegions, std::exchange(cellToStoreAfterDeferral, nullptr));
            }
        }
        if (isDeferred)
            return;
        appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
            writtenRegions.forEachRegionToVisit(visitor, [&](size_t begin, size_t end) ALWAYS_INLINE_LAMBDA {
                visitRegion(appender, begin, end);
            });
        });
    }

    NEVER_INLINE void fill(JSGlobalObject* realm)
    {
        DeferGCForAWhile deferGC(realm->vm());
        for (Slot& slot : slots) {
            JSObject* object = constructEmptyObject(realm);
            object->putDirectIndex(realm, 0, constructEmptyObject(realm));
            slot.set(writtenRegions, object);
        }
    }

    void checkEverythingIsMarked(JSGlobalObject* realm)
    {
        for (Slot& slot : slots) {
            AOT_TEST_CHECK(test.heap().isMarked(slot.get()));
            JSValue element = asObject(slot.get())->getDirectIndex(realm, 0);
            AOT_TEST_CHECK(element.isObject() && test.heap().isMarked(element.asCell()));
        }
    }

    Vector<Slot> slots;
    RememberedSlotSet writtenRegions;
    std::atomic<unsigned> numberOfDeferredVisits { 0 };
    unsigned numberOfDeferrals { 0 };
    size_t stackSizeAfterDeferral { 0 };
    JSCell* cellToStoreAfterDeferral { nullptr };
    TestVM test;
};

} // anonymous namespace

static void checkFullCollectionDefersRegionsOfRoots()
{
    auto table = makeUniqueWithoutFastMallocCheck<TableOfRoots>();
    JSGlobalObject* realm = table->test.createRealm();
    table->fill(realm);
    table->numberOfDeferrals = 0;
    table->numberOfDeferredVisits = 0;

    table->test.heap().collectSync(CollectionScope::Full);

    constexpr size_t bytesPerDeferredRegion = 4096;
    AOT_TEST_CHECK(table->numberOfDeferrals == 1);
    AOT_TEST_CHECK(table->numberOfDeferredVisits == (TableOfRoots::numberOfSlots * sizeof(TableOfRoots::Slot) + bytesPerDeferredRegion - 1) / bytesPerDeferredRegion);
    AOT_TEST_CHECK(table->stackSizeAfterDeferral < TableOfRoots::numberOfSlots / 4);
    table->checkEverythingIsMarked(realm);
}

static void checkEdenCollectionDefersNothing()
{
    auto table = makeUniqueWithoutFastMallocCheck<TableOfRoots>();
    JSGlobalObject* realm = table->test.createRealm();
    table->test.heap().collectSync(CollectionScope::Full);
    table->fill(realm);
    table->numberOfDeferrals = 0;
    table->numberOfDeferredVisits = 0;

    table->test.heap().collectSync(CollectionScope::Eden);

    AOT_TEST_CHECK(table->test.heap().lastCollectionScope() == CollectionScope::Eden);
    AOT_TEST_CHECK(!table->numberOfDeferrals);
    AOT_TEST_CHECK(!table->numberOfDeferredVisits);
    table->checkEverythingIsMarked(realm);
}

static NEVER_INLINE void prepareCellToStoreAfterDeferral(TableOfRoots& table, JSGlobalObject* realm)
{
    JSObject* object = constructEmptyObject(realm);
    object->putDirectIndex(realm, 0, constructEmptyObject(realm));
    table.cellToStoreAfterDeferral = object;
}

static void checkStoreAfterDeferralIsSeen()
{
    auto table = makeUniqueWithoutFastMallocCheck<TableOfRoots>();
    JSGlobalObject* realm = table->test.createRealm();
    table->fill(realm);
    prepareCellToStoreAfterDeferral(*table, realm);
    JSCell* replaced = table->slots[0].get();

    table->test.heap().collectSync(CollectionScope::Full);

    AOT_TEST_CHECK(!table->cellToStoreAfterDeferral);
    AOT_TEST_CHECK(table->slots[0].get() != replaced);
    table->checkEverythingIsMarked(realm);
}

static void testNothingIsDeferredWithoutCompiledCode()
{
    AOT_TEST_CHECK(!Options::useAOT());
    auto table = makeUniqueWithoutFastMallocCheck<TableOfRoots>();
    JSGlobalObject* realm = table->test.createRealm();
    table->fill(realm);
    table->numberOfDeferrals = 0;

    table->test.heap().collectSync(CollectionScope::Full);

    AOT_TEST_CHECK(!table->numberOfDeferrals);
    AOT_TEST_CHECK(!table->numberOfDeferredVisits);
    table->checkEverythingIsMarked(realm);
}

static WTF::Vector<Identifier> namesOfChain(VM& vm)
{
    return WTF::Vector<Identifier>(6, [&](size_t index) {
        return Identifier::fromString(vm, makeString("chained"_s, index));
    });
}

static Structure* addNames(VM& vm, Structure* structure, std::span<const Identifier> names)
{
    PropertyOffset offset;
    for (auto& name : names)
        structure = Structure::addPropertyTransition(vm, structure, name, 0, offset);
    return structure;
}

static void checkFullCollectionDropsTransitionChain()
{
    TestVM test;
    VM& vm = test.vm();
    Structure* empty = test.createRealm()->objectStructureForObjectConstructor();
    WTF::Vector<Identifier> names = namesOfChain(vm);
    JSObject* object = constructEmptyObject(vm, empty);
    for (auto& name : names)
        object->putDirect(vm, name, jsNumber(1));
    Structure* last = object->structure();
    AOT_TEST_CHECK(last->previousID() && !last->isPinnedPropertyTable());

    test.heap().collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(!last->previousID() && last->isPinnedPropertyTable());
    AOT_TEST_CHECK(last->transitionKind() == TransitionKind::PropertyAddition && last->transitionPropertyName() == names.last().impl());

    test.heap().collectNow(Sync, CollectionScope::Full);
    Structure* previous = addNames(vm, empty, names.span().first(5));
    AOT_TEST_CHECK(addNames(vm, previous, names.span().last(1)) == last);
    AOT_TEST_CHECK(last->isCacheableTransitionFrom(previous) && last->previousID() == previous);
    test.heap().collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(last->previousID() == previous);
    ensureStillAliveHere(object);
}

static void testTransitionChainStaysWithoutCompiledCode()
{
    if (Options::useAOT())
        return;
    TestVM test;
    VM& vm = test.vm();
    JSObject* object = constructEmptyObject(vm, test.createRealm()->objectStructureForObjectConstructor());
    for (auto& name : namesOfChain(vm))
        object->putDirect(vm, name, jsNumber(1));
    test.heap().collectNow(Sync, CollectionScope::Full);
    AOT_TEST_CHECK(object->structure()->previousID() && !object->structure()->isPinnedPropertyTable() && !object->structure()->materializedPropertyTable());
}

template<void (*body)()>
static constexpr void (*withCompiledCodeOptions)() = runInProcessWithCompiledCodeOptions<body>;

std::span<const NativeTest> collectorTests()
{
    static constexpr NativeTest tests[] = {
        { "collector/larger-cells/first-cells-are-borrowed"_s, withCompiledCodeOptions<checkFirstCellsOfSizeClassAreBorrowed> },
        { "collector/larger-cells/at-most-sixteen"_s, withCompiledCodeOptions<checkAtMostSixteenCellsAreBorrowed> },
        { "collector/larger-cells/powers-of-two-and-large-cells-are-not"_s, withCompiledCodeOptions<checkPowersOfTwoAndLargeCellsAreNotBorrowed> },
        { "collector/larger-cells/survives-collection"_s, withCompiledCodeOptions<checkBorrowedCellSurvivesCollection> },
        { "collector/larger-cells/off-without-compiled-code"_s, testNothingIsBorrowedWithoutCompiledCode },
        { "collector/helpers/small-heap-starts-none"_s, withCompiledCodeOptions<checkSmallHeapStartsNoHelper> },
        { "collector/helpers/wide-heap-starts-them"_s, withCompiledCodeOptions<checkWideHeapStartsHelpers> },
        { "collector/helpers/eager-without-compiled-code"_s, testHelpersStartEagerlyWithoutCompiledCode },
        { "collector/helpers/waiting-for-started-threads-does-not-wait-for-their-work"_s, testWaitingForStartedThreadsDoesNotWaitForTheirWork },
        { "collector/roots/full-collection-defers-regions"_s, withCompiledCodeOptions<checkFullCollectionDefersRegionsOfRoots> },
        { "collector/roots/store-after-deferral-is-seen"_s, withCompiledCodeOptions<checkStoreAfterDeferralIsSeen> },
        { "collector/roots/eden-collection-defers-nothing"_s, withCompiledCodeOptions<checkEdenCollectionDefersNothing> },
        { "collector/roots/nothing-deferred-without-compiled-code"_s, testNothingIsDeferredWithoutCompiledCode },
        { "collector/empty-eden/skips-marking"_s, withCompiledCodeOptions<checkEdenCollectionWithEmptyEdenSkipsMarking> },
        { "collector/empty-eden/allocation-ends-the-skips"_s, withCompiledCodeOptions<checkAllocationEndsTheSkips> },
        { "collector/empty-eden/full-collection-is-never-skipped"_s, withCompiledCodeOptions<checkFullCollectionIsNeverSkipped> },
        { "collector/empty-eden/store-before-a-skip-is-seen-later"_s, withCompiledCodeOptions<checkStoreBeforeSkippedCollectionIsSeenLater> },
        { "collector/empty-eden/off-without-compiled-code"_s, testNoCollectionIsSkippedWithoutCompiledCode },
        { "collector/structures/full-collection-drops-transition-chain"_s, withCompiledCodeOptions<checkFullCollectionDropsTransitionChain> },
        { "collector/structures/chain-stays-without-compiled-code"_s, testTransitionChainStaysWithoutCompiledCode },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
