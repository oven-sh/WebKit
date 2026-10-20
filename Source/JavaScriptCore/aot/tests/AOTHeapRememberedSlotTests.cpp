/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "HeapInlines.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "RememberedSlot.h"
#include "SimpleMarkingConstraint.h"
#include "StoredCellLog.h"
#include "VM.h"
#include <array>
#include <memory>
#include <optional>
#include <type_traits>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

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
        m_realm.clear();
        m_vm = nullptr;
        m_lock.reset();
    }

    VM& vm() { return *m_vm; }

    JSArray* createArray(IndexingType indexingType, unsigned length)
    {
        if (!m_realm)
            m_realm.set(*m_vm, JSGlobalObject::create(*m_vm, JSGlobalObject::createStructure(*m_vm, jsNull())));
        JSArray* array = JSArray::tryCreate(*m_vm, m_realm->arrayStructureForIndexingTypeDuringAllocation(indexingType), length);
        AOT_TEST_CHECK(array);
        return array;
    }

private:
    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
    Strong<JSGlobalObject> m_realm;
};

struct Region {
    size_t begin;
    size_t end;

    friend bool operator==(const Region&, const Region&) = default;
};

using Regions = Vector<Region>;

struct OtherVisitor { };

constexpr unsigned regionShift = 6;
constexpr size_t regionSize = 1 << regionShift;

template<size_t tableSize>
struct ByteTable {
    using Slot = RememberedSlot<uint32_t>;
    static_assert(!(tableSize % sizeof(Slot)));

    std::span<const uint8_t> bytes() const { return asByteSpan(std::span<const Slot>(slots)); }
    Slot& slotAt(size_t offset) { return slots[offset / sizeof(Slot)]; }

    alignas(regionSize) std::array<Slot, tableSize / sizeof(Slot)> slots { };
};

} // anonymous namespace

static auto appendTo(Regions& regions)
{
    return [&regions](size_t begin, size_t end) {
        regions.append({ begin, end });
    };
}

static Regions regionsToSweep(RememberedSlotSet& set, CollectionScope scope)
{
    Regions regions;
    set.forEachRegionToSweep(scope, appendTo(regions));
    return regions;
}

static Regions regionsToClear(RememberedSlotSet& set)
{
    Regions regions;
    set.forEachRegionToClear(appendTo(regions));
    return regions;
}

static Regions regionsSeenByOtherVisitor(RememberedSlotSet& set)
{
    OtherVisitor visitor;
    Regions regions;
    set.forEachRegionToVisit(visitor, appendTo(regions));
    return regions;
}

static Vector<uint8_t> cardsOf(RememberedSlotSet& set, size_t count)
{
    return Vector<uint8_t>(std::span<const uint8_t>(set.cards(), count));
}

static void testSlotIsAsWideAsItsValue()
{
    static_assert(sizeof(RememberedSlot<uint8_t>) == 1 && sizeof(RememberedSlot<uint16_t>) == 2 && sizeof(RememberedSlot<uint32_t>) == 4 && sizeof(RememberedSlot<uint64_t>) == 8);
    static_assert(sizeof(RememberedSlot<JSCell*>) == sizeof(JSCell*) && alignof(RememberedSlot<JSCell*>) == alignof(JSCell*) && alignof(RememberedSlot<uint32_t>) == alignof(uint32_t));
    static_assert(std::is_trivially_default_constructible_v<RememberedSlot<uint32_t>> && std::is_trivially_copyable_v<RememberedSlot<JSCell*>>);
    static_assert(!RememberedSlot<uint32_t>::offsetOfValue());

    uint64_t zeroedMemory = 0;
    auto& slot = *reinterpret_cast<RememberedSlot<uint64_t>*>(&zeroedMemory);
    AOT_TEST_CHECK(!slot && !slot.get());
    slot.setWithoutRecordingForTesting(0x1122334455667788ull);
    AOT_TEST_CHECK(slot && zeroedMemory == 0x1122334455667788ull);
}

static void testSetWritesTheCardOfTheSlot()
{
    ByteTable<4 * regionSize> table;
    RememberedSlotSet set(table.bytes(), regionShift);
    AOT_TEST_CHECK(set.regionShift() == regionShift && !set.hasWrittenRegions());
    AOT_TEST_CHECK((cardsOf(set, 8) == Vector<uint8_t> { 0, 0, 0, 0, 0, 0, 0, 0 }));

    table.slotAt(2 * regionSize).set(set, 7);
    AOT_TEST_CHECK(table.slotAt(2 * regionSize).get() == 7 && set.hasWrittenRegions());
    AOT_TEST_CHECK((cardsOf(set, 8) == Vector<uint8_t> { 0, 0, 1, 0, 0, 0, 0, 0 }));

    table.slotAt(regionSize - sizeof(uint32_t)).set(set, 8);
    table.slotAt(3 * regionSize + 8).set(set, 9);
    table.slotAt(3 * regionSize + 12).set(set, 10);
    AOT_TEST_CHECK((cardsOf(set, 8) == Vector<uint8_t> { 1, 0, 1, 1, 0, 0, 0, 0 }));
    AOT_TEST_CHECK(RememberedSlotSet::neverWrittenCard == 0 && RememberedSlotSet::writtenCard == 1 && RememberedSlotSet::scannedCard == 2);
}

static void testOtherStoresWriteNoCard()
{
    ByteTable<4 * regionSize> table;
    RememberedSlotSet set(table.bytes(), regionShift);

    table.slotAt(regionSize).setWithoutRecordingForTesting(5);
    AOT_TEST_CHECK(table.slotAt(regionSize).get() == 5 && !set.hasWrittenRegions());
    table.slotAt(regionSize).clear();
    AOT_TEST_CHECK(!table.slotAt(regionSize) && !set.hasWrittenRegions());
    AOT_TEST_CHECK((cardsOf(set, 8) == Vector<uint8_t> { 0, 0, 0, 0, 0, 0, 0, 0 }));

    table.slotAt(0).set(set, 1);
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden).size() == 1);
    table.slotAt(0).clear();
    table.slotAt(0).setWithoutRecordingForTesting(2);
    AOT_TEST_CHECK((cardsOf(set, 8) == Vector<uint8_t> { 2, 0, 0, 0, 0, 0, 0, 0 }) && !set.hasWrittenRegions());
}

static void testNumberOfCards()
{
    for (size_t tableSize : { 0, 1, 255, 256, 257, 8 * 256 })
        AOT_TEST_CHECK(RememberedSlotSet::numberOfCardsFor(tableSize, 8) == 8);
    AOT_TEST_CHECK(RememberedSlotSet::numberOfCardsFor(8 * 256 + 1, 8) == 16 && RememberedSlotSet::numberOfCardsFor(16 * 256, 8) == 16 && RememberedSlotSet::numberOfCardsFor(16 * 256 + 1, 8) == 24);
    AOT_TEST_CHECK(RememberedSlotSet::numberOfCardsFor(257, 0) == 264 && RememberedSlotSet::numberOfCardsFor(1 << 20, 12) == 256);
    static_assert(RememberedSlotSet::cardsPerWord == 8);
}

static void testOtherVisitorSeesEveryRegionEverWritten()
{
    ByteTable<12 * regionSize> table;
    RememberedSlotSet set(table.bytes(), regionShift);
    AOT_TEST_CHECK(regionsSeenByOtherVisitor(set).isEmpty());

    table.slotAt(regionSize).set(set, 1);
    table.slotAt(9 * regionSize).set(set, 1);
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden).size() == 2);
    table.slotAt(4 * regionSize).set(set, 1);

    Vector<uint8_t> cardsBefore = cardsOf(set, 16);
    AOT_TEST_CHECK((cardsBefore == Vector<uint8_t> { 0, 2, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0 }));
    Regions expected { { regionSize, 2 * regionSize }, { 4 * regionSize, 5 * regionSize }, { 9 * regionSize, 10 * regionSize } };
    AOT_TEST_CHECK(regionsSeenByOtherVisitor(set) == expected);
    AOT_TEST_CHECK(regionsSeenByOtherVisitor(set) == expected);
    AOT_TEST_CHECK(cardsOf(set, 16) == cardsBefore);
}

static void testSweepSeesWrittenRegionsOnce()
{
    ByteTable<4 * regionSize> table;
    RememberedSlotSet set(table.bytes(), regionShift);
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden).isEmpty() && regionsToSweep(set, CollectionScope::Full).isEmpty());

    table.slotAt(0).set(set, 1);
    table.slotAt(3 * regionSize).set(set, 1);
    Regions written { { 0, regionSize }, { 3 * regionSize, 4 * regionSize } };
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden) == written);
    AOT_TEST_CHECK(!set.hasWrittenRegions() && (cardsOf(set, 8) == Vector<uint8_t> { 2, 0, 0, 2, 0, 0, 0, 0 }));
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden).isEmpty());

    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Full) == written);
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Full) == written);
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden).isEmpty());

    table.slotAt(regionSize).set(set, 1);
    AOT_TEST_CHECK((regionsToSweep(set, CollectionScope::Eden) == Regions { { regionSize, 2 * regionSize } }));
    table.slotAt(2 * regionSize).set(set, 1);
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Full).size() == 4);
}

static void testClearResetsTheCards()
{
    ByteTable<4 * regionSize> table;
    RememberedSlotSet set(table.bytes(), regionShift);
    AOT_TEST_CHECK(regionsToClear(set).isEmpty());

    table.slotAt(0).set(set, 1);
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden).size() == 1);
    table.slotAt(2 * regionSize).set(set, 1);
    AOT_TEST_CHECK((regionsToClear(set) == Regions { { 0, regionSize }, { 2 * regionSize, 3 * regionSize } }));
    AOT_TEST_CHECK((cardsOf(set, 8) == Vector<uint8_t> { 0, 0, 0, 0, 0, 0, 0, 0 }));
    AOT_TEST_CHECK(regionsToClear(set).isEmpty() && regionsToSweep(set, CollectionScope::Full).isEmpty() && regionsSeenByOtherVisitor(set).isEmpty());
}

static void testRecordAllLeavesPaddingCardsAlone()
{
    ByteTable<10 * regionSize + 4> table;
    RememberedSlotSet set(table.bytes(), regionShift);
    set.recordAll();
    AOT_TEST_CHECK((cardsOf(set, 16) == Vector<uint8_t> { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 }));
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden).size() == 11);
}

static void testLastRegionIsClippedToTheTable()
{
    ByteTable<2 * regionSize + 12> table;
    RememberedSlotSet set(table.bytes(), regionShift);
    table.slotAt(2 * regionSize + 8).set(set, 1);
    Regions clipped { { 2 * regionSize, 2 * regionSize + 12 } };
    AOT_TEST_CHECK(regionsSeenByOtherVisitor(set) == clipped && regionsToSweep(set, CollectionScope::Eden) == clipped && regionsToClear(set) == clipped);

    table.slotAt(regionSize).set(set, 1);
    AOT_TEST_CHECK((regionsToSweep(set, CollectionScope::Eden) == Regions { { regionSize, 2 * regionSize } }));
}

static void testSetWithOneRegion()
{
    RememberedSlotSet set;
    AOT_TEST_CHECK(!set.hasWrittenRegions() && regionsToSweep(set, CollectionScope::Full).isEmpty() && regionsSeenByOtherVisitor(set).isEmpty());

    set.recordAll();
    Regions oneRegion { { 0, 0 } };
    AOT_TEST_CHECK(set.hasWrittenRegions() && (cardsOf(set, 8) == Vector<uint8_t> { 1, 0, 0, 0, 0, 0, 0, 0 }));
    AOT_TEST_CHECK(regionsSeenByOtherVisitor(set) == oneRegion);
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden) == oneRegion && regionsToSweep(set, CollectionScope::Eden).isEmpty());
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Full) == oneRegion);
    AOT_TEST_CHECK(regionsToClear(set) == oneRegion && regionsToClear(set).isEmpty());
}

static void testExternalCards()
{
    ByteTable<9 * regionSize> table;
    alignas(RememberedSlotSet::cardsPerWord) std::array<uint8_t, 24> cards { };
    static_assert(RememberedSlotSet::numberOfCardsFor(9 * regionSize, regionShift) == 16);
    RememberedSlotSet set(table.bytes(), regionShift, std::span<uint8_t>(cards));
    AOT_TEST_CHECK(set.cards() == cards.data());

    table.slotAt(8 * regionSize).set(set, 1);
    AOT_TEST_CHECK(cards[8] == RememberedSlotSet::writtenCard && set.hasWrittenRegions());
    cards[1] = RememberedSlotSet::writtenCard;
    AOT_TEST_CHECK((regionsToSweep(set, CollectionScope::Eden) == Regions { { regionSize, 2 * regionSize }, { 8 * regionSize, 9 * regionSize } }));
    AOT_TEST_CHECK(cards[1] == RememberedSlotSet::scannedCard && cards[8] == RememberedSlotSet::scannedCard);

    cards[16] = RememberedSlotSet::writtenCard;
    AOT_TEST_CHECK(!set.hasWrittenRegions() && regionsToSweep(set, CollectionScope::Eden).isEmpty() && cards[16] == RememberedSlotSet::writtenCard);
}

static void testMovedTable()
{
    ByteTable<2 * regionSize> smallTable;
    ByteTable<20 * regionSize> largeTable;
    RememberedSlotSet set(smallTable.bytes(), regionShift);
    smallTable.slotAt(0).set(set, 1);
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden).size() == 1);

    set.didMoveTable(largeTable.bytes());
    Regions afterMove = regionsToSweep(set, CollectionScope::Eden);
    AOT_TEST_CHECK(afterMove.size() == 20 && afterMove[0] == (Region { 0, regionSize }) && afterMove[19] == (Region { 19 * regionSize, 20 * regionSize }));
    AOT_TEST_CHECK(regionsToSweep(set, CollectionScope::Eden).isEmpty());

    largeTable.slotAt(17 * regionSize).set(set, 1);
    AOT_TEST_CHECK((regionsToSweep(set, CollectionScope::Eden) == Regions { { 17 * regionSize, 18 * regionSize } }));

    set.didMoveTable(smallTable.bytes());
    AOT_TEST_CHECK((regionsToSweep(set, CollectionScope::Full) == Regions { { 0, regionSize }, { regionSize, 2 * regionSize } }));
}

static void testEntryAcrossABorderIsSeenFromBothRegions()
{
    struct Entry {
        uint64_t words[3];
    };
    static_assert(sizeof(Entry) == 24);
    std::array<Entry, 8> entries { };
    auto indexesIn = [&](size_t begin, size_t end) {
        Vector<size_t> indexes;
        RememberedSlotSet::forEachEntryInRegion(std::span<Entry>(entries), begin, end, [&](Entry& entry) {
            indexes.append(&entry - entries.data());
        });
        return indexes;
    };

    AOT_TEST_CHECK((indexesIn(0, 64) == Vector<size_t> { 0, 1, 2 }));
    AOT_TEST_CHECK((indexesIn(64, 128) == Vector<size_t> { 2, 3, 4, 5 }));
    AOT_TEST_CHECK((indexesIn(128, 192) == Vector<size_t> { 5, 6, 7 }));
    AOT_TEST_CHECK((indexesIn(0, 48) == Vector<size_t> { 0, 1 }));
    AOT_TEST_CHECK((indexesIn(0, 256) == Vector<size_t> { 0, 1, 2, 3, 4, 5, 6, 7 }));
    AOT_TEST_CHECK(indexesIn(192, 256).isEmpty() && indexesIn(0, 0).isEmpty());
}

namespace {

struct RootTable {
    using Slot = RememberedSlot<JSCell*>;
    static constexpr size_t slotsPerRegion = regionSize / sizeof(Slot);
    static constexpr size_t numberOfRegions = 8;

    template<typename Visitor>
    void visitRoots(Visitor& visitor)
    {
        ++(std::is_same_v<Visitor, SlotVisitor> ? visitsByCollector : visitsByOtherVisitors);
        writtenRegions.forEachRegionToVisit(visitor, [&](size_t begin, size_t end) {
            if constexpr (std::is_same_v<Visitor, SlotVisitor>)
                regionsSeenByCollector.append({ begin, end });
            RememberedSlotSet::forEachEntryInRegion(std::span<Slot>(slots), begin, end, [&](Slot& slot) {
                if (slot)
                    visitor.appendUnbarriered(slot.get());
            });
        });
    }

    std::span<Slot> region(size_t index) { return std::span<Slot>(slots).subspan(index * slotsPerRegion, slotsPerRegion); }

    alignas(regionSize) std::array<Slot, numberOfRegions * slotsPerRegion> slots { };
    RememberedSlotSet writtenRegions { asByteSpan(std::span<const Slot>(slots)), regionShift };
    Regions regionsSeenByCollector;
    unsigned visitsByCollector { 0 };
    unsigned visitsByOtherVisitors { 0 };
};

struct CollectedRoots {
    CollectedRoots()
    {
        registry.add(CacheKind::StrongRoots, "table of a test"_s, table);
        test.vm().heap.addMarkingConstraint(makeUnique<SimpleMarkingConstraint>(
            "Rs"_s, "Remembered Slots of a Test"_s,
            MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
                registry.visitRoots(visitor, RootVolatility::GreyedByMarking);
            })),
            ConstraintVolatility::GreyedByMarking,
            ConstraintConcurrency::Concurrent,
            ConstraintParallelism::Sequential));
    }

    Regions collect(CollectionScope scope)
    {
        table.regionsSeenByCollector.clear();
        test.vm().heap.collectSync(scope);
        AOT_TEST_CHECK(test.vm().heap.lastCollectionScope() == scope);
        return table.regionsSeenByCollector;
    }

    unsigned numberOfMarkedCellsIn(size_t region)
    {
        unsigned count = 0;
        for (RootTable::Slot& slot : table.region(region))
            count += test.vm().heap.isMarked(slot.get());
        return count;
    }

    RootTable table;
    CacheRegistry registry;
    TestVM test;
};

} // anonymous namespace

static NEVER_INLINE void fillRegionWithYoungCells(CollectedRoots& roots, size_t region, bool recordsStores)
{
    for (RootTable::Slot& slot : roots.table.region(region)) {
        JSCell* cell = jsNontrivialString(roots.test.vm(), "a young cell"_s);
        if (recordsStores)
            slot.set(roots.table.writtenRegions, cell);
        else
            slot.setWithoutRecordingForTesting(cell);
    }
}

static Region regionAt(size_t index)
{
    return { index * regionSize, (index + 1) * regionSize };
}

static void testYoungCellInRecordedSlotSurvivesEdenCollection()
{
    std::unique_ptr<CollectedRoots> rootsOutsideTheStack = std::make_unique<CollectedRoots>();
    CollectedRoots& roots = *rootsOutsideTheStack;
    AOT_TEST_CHECK(roots.collect(CollectionScope::Full).isEmpty() && roots.table.visitsByCollector);

    fillRegionWithYoungCells(roots, 2, true);
    fillRegionWithYoungCells(roots, 5, false);
    AOT_TEST_CHECK((roots.collect(CollectionScope::Eden) == Regions { regionAt(2) }));
    AOT_TEST_CHECK(roots.numberOfMarkedCellsIn(2) == RootTable::slotsPerRegion);
    AOT_TEST_CHECK(roots.numberOfMarkedCellsIn(5) < RootTable::slotsPerRegion);
    for (RootTable::Slot& slot : roots.table.region(5))
        slot.clear();

    AOT_TEST_CHECK(roots.collect(CollectionScope::Eden).isEmpty());
    AOT_TEST_CHECK(roots.numberOfMarkedCellsIn(2) == RootTable::slotsPerRegion);
}

static void testFullCollectionVisitsScannedRegionsOnce()
{
    std::unique_ptr<CollectedRoots> rootsOutsideTheStack = std::make_unique<CollectedRoots>();
    CollectedRoots& roots = *rootsOutsideTheStack;
    fillRegionWithYoungCells(roots, 1, true);
    fillRegionWithYoungCells(roots, 6, true);
    AOT_TEST_CHECK((roots.collect(CollectionScope::Eden) == Regions { regionAt(1), regionAt(6) }));

    for (unsigned round = 0; round < 2; ++round) {
        AOT_TEST_CHECK((roots.collect(CollectionScope::Full) == Regions { regionAt(1), regionAt(6) }));
        AOT_TEST_CHECK(roots.numberOfMarkedCellsIn(1) == RootTable::slotsPerRegion && roots.numberOfMarkedCellsIn(6) == RootTable::slotsPerRegion);
    }

    fillRegionWithYoungCells(roots, 3, true);
    AOT_TEST_CHECK((roots.collect(CollectionScope::Full) == Regions { regionAt(1), regionAt(3), regionAt(6) }));
    AOT_TEST_CHECK(roots.numberOfMarkedCellsIn(3) == RootTable::slotsPerRegion);
    AOT_TEST_CHECK(roots.collect(CollectionScope::Eden).isEmpty());
}

static void testLogRefusesSmallOwners()
{
    TestVM test;
    StoredCellLog log;
    JSCell* young = jsNontrivialString(test.vm(), "a young cell"_s);

    JSArray* smallArray = test.createArray(ArrayWithContiguous, 100);
    AOT_TEST_CHECK(smallArray->butterfly()->vectorLength() < StoredCellLog::minVectorLengthOfOwner);
    AOT_TEST_CHECK(!log.tryRemember(smallArray, young));
    AOT_TEST_CHECK(!log.tryRemember(young, young));
    AOT_TEST_CHECK(!log.tryRemember(test.createArray(ArrayWithUndecided, 0), young));
    AOT_TEST_CHECK(!log.tryRemember(test.vm().structureStructure.get(), young));
    AOT_TEST_CHECK(!log.size() && !log.capacity());

    JSArray* arrayWithShortVector = test.createArray(ArrayWithArrayStorage, StoredCellLog::minVectorLengthOfOwner);
    AOT_TEST_CHECK(arrayWithShortVector->length() == StoredCellLog::minVectorLengthOfOwner && arrayWithShortVector->butterfly()->vectorLength() < StoredCellLog::minVectorLengthOfOwner);
    AOT_TEST_CHECK(!log.tryRemember(arrayWithShortVector, young) && !log.size() && !log.capacity());

    for (IndexingType indexingType : { ArrayWithContiguous, ArrayWithInt32, ArrayWithDouble }) {
        JSArray* largeArray = test.createArray(indexingType, StoredCellLog::minVectorLengthOfOwner);
        AOT_TEST_CHECK(largeArray->butterfly()->vectorLength() >= StoredCellLog::minVectorLengthOfOwner);
        size_t sizeBefore = log.size();
        AOT_TEST_CHECK(log.tryRemember(largeArray, young) && log.size() == sizeBefore + 1);
    }
    AOT_TEST_CHECK(log.capacity() == StoredCellLog::minCapacity);
    static_assert(StoredCellLog::minVectorLengthOfOwner == 1024 && StoredCellLog::minCapacity == 4096);
}

static void testLogSkipsOldCells()
{
    TestVM test;
    StoredCellLog log;
    JSArray* owner = test.createArray(ArrayWithContiguous, 2000);
    Strong<JSString> old(test.vm(), jsNontrivialString(test.vm(), "an old cell"_s));
    test.vm().heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(old->cellState() == CellState::PossiblyBlack);

    AOT_TEST_CHECK(log.tryRemember(owner, old.get()) && !log.size() && !log.capacity());

    JSCell* young = jsNontrivialString(test.vm(), "a young cell"_s);
    AOT_TEST_CHECK(young->cellState() != CellState::PossiblyBlack);
    AOT_TEST_CHECK(log.tryRemember(owner, young) && log.size() == 1);
    AOT_TEST_CHECK(log.tryRemember(owner, old.get()) && log.size() == 1);
}

static size_t numberOfStoresRemembered(StoredCellLog& log, JSArray* owner, JSCell* stored)
{
    size_t count = 0;
    while (count < 100000 && log.tryRemember(owner, stored))
        ++count;
    return count;
}

static void testLogStopsGrowingAtTheLengthOfTheOwner()
{
    TestVM test;
    JSCell* young = jsNontrivialString(test.vm(), "a young cell"_s);

    StoredCellLog logOfShortOwner;
    JSArray* shortOwner = test.createArray(ArrayWithContiguous, 2000);
    AOT_TEST_CHECK(shortOwner->butterfly()->vectorLength() <= StoredCellLog::minCapacity);
    AOT_TEST_CHECK(numberOfStoresRemembered(logOfShortOwner, shortOwner, young) == StoredCellLog::minCapacity);
    AOT_TEST_CHECK(logOfShortOwner.size() == StoredCellLog::minCapacity && logOfShortOwner.capacity() == StoredCellLog::minCapacity);
    AOT_TEST_CHECK(!logOfShortOwner.tryRemember(shortOwner, young));

    StoredCellLog logOfLongOwner;
    JSArray* longOwner = test.createArray(ArrayWithContiguous, 5000);
    unsigned length = longOwner->butterfly()->vectorLength();
    AOT_TEST_CHECK(length > StoredCellLog::minCapacity && length <= 2 * StoredCellLog::minCapacity);
    AOT_TEST_CHECK(numberOfStoresRemembered(logOfLongOwner, longOwner, young) == 2 * StoredCellLog::minCapacity);
    AOT_TEST_CHECK(logOfLongOwner.capacity() == 2 * StoredCellLog::minCapacity);

    AOT_TEST_CHECK(!logOfLongOwner.tryRemember(shortOwner, young));
    AOT_TEST_CHECK(logOfShortOwner.tryRemember(longOwner, young) && logOfShortOwner.capacity() == 2 * StoredCellLog::minCapacity);
}

namespace {

struct CollectedLog {
    CollectedLog()
    {
        test.vm().heap.addMarkingConstraint(makeUnique<SimpleMarkingConstraint>(
            "Sl"_s, "Stored Cell Log of a Test"_s,
            MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([this] (auto& visitor) {
                log.visitAndReset(visitor);
            })),
            ConstraintVolatility::GreyedByExecution,
            ConstraintConcurrency::Concurrent,
            ConstraintParallelism::Sequential));
    }

    StoredCellLog log;
    TestVM test;
};

constexpr unsigned numberOfLoggedCells = 32;

} // anonymous namespace

static NEVER_INLINE Vector<JSCell*> rememberYoungCells(CollectedLog& collected, JSArray* owner)
{
    Vector<JSCell*> cells;
    for (unsigned count = 0; count < numberOfLoggedCells; ++count) {
        cells.append(jsNontrivialString(collected.test.vm(), "a young cell"_s));
        AOT_TEST_CHECK(collected.log.tryRemember(owner, cells.last()));
    }
    return cells;
}

static unsigned numberOfMarkedCells(VM& vm, const Vector<JSCell*>& cells)
{
    unsigned count = 0;
    for (JSCell* cell : cells)
        count += vm.heap.isMarked(cell);
    return count;
}

static void testVisitEmptiesTheLog()
{
    CollectedLog collected;
    VM& vm = collected.test.vm();
    Strong<JSArray> owner(vm, collected.test.createArray(ArrayWithContiguous, 5000));
    vm.heap.collectSync(CollectionScope::Full);

    Vector<JSCell*> cells = rememberYoungCells(collected, owner.get());
    AOT_TEST_CHECK(collected.log.size() == numberOfLoggedCells && collected.log.capacity() == StoredCellLog::minCapacity);
    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(numberOfMarkedCells(vm, cells) == numberOfLoggedCells);
    AOT_TEST_CHECK(!collected.log.size() && collected.log.capacity() == StoredCellLog::minCapacity);

    Vector<JSCell*> cellsBeforeFullCollection = rememberYoungCells(collected, owner.get());
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(numberOfMarkedCells(vm, cellsBeforeFullCollection) < numberOfLoggedCells);
    AOT_TEST_CHECK(!collected.log.size() && collected.log.capacity() == StoredCellLog::minCapacity);
}

static void testVisitFreesAGrownBuffer()
{
    CollectedLog collected;
    VM& vm = collected.test.vm();
    Strong<JSArray> owner(vm, collected.test.createArray(ArrayWithContiguous, 5000));
    Strong<JSString> young(vm, jsNontrivialString(vm, "a young cell"_s));
    AOT_TEST_CHECK(numberOfStoresRemembered(collected.log, owner.get(), young.get()) == 2 * StoredCellLog::minCapacity);
    AOT_TEST_CHECK(collected.log.capacity() > StoredCellLog::minCapacity);

    vm.heap.collectSync(CollectionScope::Eden);
    AOT_TEST_CHECK(!collected.log.size() && !collected.log.capacity());

    JSCell* next = jsNontrivialString(vm, "the next young cell"_s);
    AOT_TEST_CHECK(collected.log.tryRemember(owner.get(), next) && collected.log.size() == 1 && collected.log.capacity() == StoredCellLog::minCapacity);
}

std::span<const NativeTest> heapRememberedSlotTests()
{
    static constexpr NativeTest tests[] = {
        { "remembered-slot/slot/as-wide-as-its-value"_s, testSlotIsAsWideAsItsValue },
        { "remembered-slot/slot/set-writes-the-card-of-the-slot"_s, testSetWritesTheCardOfTheSlot },
        { "remembered-slot/slot/other-stores-write-no-card"_s, testOtherStoresWriteNoCard },
        { "remembered-slot/set/number-of-cards"_s, testNumberOfCards },
        { "remembered-slot/set/other-visitor-sees-every-region-ever-written"_s, testOtherVisitorSeesEveryRegionEverWritten },
        { "remembered-slot/set/sweep-sees-written-regions-once"_s, testSweepSeesWrittenRegionsOnce },
        { "remembered-slot/set/clear-resets-the-cards"_s, testClearResetsTheCards },
        { "remembered-slot/set/record-all-leaves-padding-cards-alone"_s, testRecordAllLeavesPaddingCardsAlone },
        { "remembered-slot/set/last-region-is-clipped-to-the-table"_s, testLastRegionIsClippedToTheTable },
        { "remembered-slot/set/one-region"_s, testSetWithOneRegion },
        { "remembered-slot/set/external-cards"_s, testExternalCards },
        { "remembered-slot/set/moved-table"_s, testMovedTable },
        { "remembered-slot/set/entry-across-a-border-is-seen-from-both-regions"_s, testEntryAcrossABorderIsSeenFromBothRegions },
        { "remembered-slot/set/young-cell-in-recorded-slot-survives-eden-collection"_s, testYoungCellInRecordedSlotSurvivesEdenCollection },
        { "remembered-slot/set/full-collection-visits-scanned-regions-once"_s, testFullCollectionVisitsScannedRegionsOnce },
        { "remembered-slot/log/refuses-small-owners"_s, testLogRefusesSmallOwners },
        { "remembered-slot/log/skips-old-cells"_s, testLogSkipsOldCells },
        { "remembered-slot/log/stops-growing-at-the-length-of-the-owner"_s, testLogStopsGrowingAtTheLengthOfTheOwner },
        { "remembered-slot/log/visit-empties-the-log"_s, testVisitEmptiesTheLog },
        { "remembered-slot/log/visit-frees-a-grown-buffer"_s, testVisitFreesAGrownBuffer },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
