/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTPropertyLookupTableTestSupport.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <array>
#include <bit>
#include <memory>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

namespace {

void evictionKeepsFilling()
{
    constexpr uint32_t numberOfWideRows = RowDisplacementTable::maxNumberOfBases / RowDisplacementTable::numberOfColumns;
    constexpr unsigned numberOfNarrowRows = RowDisplacementTable::numberOfColumns;
    TableFixture fixture;
    auto& front = fixture.front();
    Vector<Structure*> structures;
    for (uint32_t i = 0; i < numberOfWideRows; ++i) {
        structures.append(fixture.createStructure());
        Slot slot = ownValueSlot(structures.last(), 0);
        for (uint32_t column = 1; column <= RowDisplacementTable::maxColumn; ++column)
            AOT_TEST_CHECK(front.trySet(structures.last(), column, SlotAccess::Read, slot));
    }
    for (Structure* structure : structures)
        AOT_TEST_CHECK(front.find(structure->id(), RowDisplacementTable::maxColumn, SlotAccess::Read));

    for (unsigned i = 0; i < numberOfNarrowRows; ++i) {
        structures.append(fixture.createStructure());
        Slot slot = ownValueSlot(structures.last(), 1);
        AOT_TEST_CHECK(front.trySet(structures.last(), 1, SlotAccess::Read, slot));
        AOT_TEST_CHECK(isSameSlot(front.find(structures.last()->id(), 1, SlotAccess::Read), slot));
    }

    unsigned numberOfEvictedRows = 0;
    for (Structure* structure : structures) {
        AOT_TEST_CHECK(front.hasRow(structure->id()) == !!rowOf(structure));
        AOT_TEST_CHECK(front.hasRow(structure->id()) == !!front.find(structure->id(), 1, SlotAccess::Read));
        numberOfEvictedRows += !front.hasRow(structure->id());
    }
    AOT_TEST_CHECK(numberOfEvictedRows && !front.hasRow(structures.first()->id()));
}

static constexpr size_t bytesInFrontOfZeroedInstance = static_cast<size_t>(-InstanceLayout::offsetOfKeyedValuePlaces);

struct ZeroedInstanceDeleter {
    void operator()(Instance* instance) const
    {
        instance->~Instance();
        fastFree(reinterpret_cast<uint8_t*>(instance) - bytesInFrontOfZeroedInstance);
    }
};
using ZeroedInstance = std::unique_ptr<Instance, ZeroedInstanceDeleter>;

ZeroedInstance createZeroedInstance()
{
    constexpr size_t size = bytesInFrontOfZeroedInstance + sizeof(Instance);
    auto* memory = static_cast<uint8_t*>(fastAlignedMalloc(alignof(Instance), size));
    ZeroedInstance instance { new (NotNull, memory + bytesInFrontOfZeroedInstance) Instance };
    zeroSpan(unsafeMakeSpan(memory, size));
    return instance;
}

bool hasPublishedWords(TableFixture& fixture, const Instance& instance)
{
    const PropertyCacheWords& words = instance.propertyCaches;
    for (SlotAccess access : allSlotAccesses) {
        const FrontWords& front = words.front[static_cast<unsigned>(access)];
        if (front.plane != fixture.front().plane(access) || front.rowsForStructureIDZero != StructureRows::singleton().rowsForStructureIDZero())
            return false;
    }
    return words.hashedSlots == fixture.hashedTable().entries() && words.hashedSlotOffsetMask == fixture.hashedTable().offsetMask()
        && words.customAccessors == fixture.caches().customAccessors().records();
}

void growthKeepsEntriesAndPublishesWords()
{
    constexpr uint32_t initialNumberOfEntries = HashedSlotTable::initialNumberOfEntries;
    constexpr unsigned maxNumberOfFills = 16 * initialNumberOfEntries;
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    ZeroedInstance instance = createZeroedInstance();
    fixture.vm().aotState().instances.append(instance.get());
    fixture.caches().didCreateInstance(*instance);
    AOT_TEST_CHECK(table.numberOfEntries() == initialNumberOfEntries && hasPublishedWords(fixture, *instance));

    Structure* lastStructure = nullptr;
    for (unsigned i = 0; table.numberOfEntries() == initialNumberOfEntries; ++i) {
        AOT_TEST_CHECK(i < maxNumberOfFills);
        lastStructure = fixture.createStructure();
        fixture.caches().set(lastStructure, fixture.nameWithoutNumber(1), SlotAccess::Read, absentSlot(lastStructure, 0));
    }

    AOT_TEST_CHECK(table.numberOfEntries() == 2 * initialNumberOfEntries);
    AOT_TEST_CHECK(table.offsetMask() == HashedSlot::offsetMaskFor(2 * initialNumberOfEntries));
    AOT_TEST_CHECK(hasPublishedWords(fixture, *instance));
    AOT_TEST_CHECK(isSameSlot(table.find(lastStructure->id(), fixture.uid(1), SlotAccess::Read), absentSlot(lastStructure, 0)));
    size_t numberOfKeptEntries = 0;
    table.forEachEntry([&](const HashedSlot& entry) {
        AOT_TEST_CHECK(table.find(entry.slot.structureID, entry.uid(), entry.access()) == &entry.slot);
        ++numberOfKeptEntries;
    });
    AOT_TEST_CHECK(numberOfKeptEntries > initialNumberOfEntries / 2);
    fixture.vm().aotState().instances.clear();
}

void frontGrowthKeepsRowsAndPublishesWords()
{
    constexpr unsigned numberOfStructures = 3 * RowDisplacementTable::initialNumberOfBases;
    constexpr uint32_t columns[] = { 1, 977, RowDisplacementTable::maxColumn };
    TableFixture fixture;
    auto& front = fixture.front();
    ZeroedInstance instance = createZeroedInstance();
    fixture.vm().aotState().instances.append(instance.get());
    fixture.caches().didCreateInstance(*instance);
    AOT_TEST_CHECK(hasPublishedWords(fixture, *instance));

    auto offsetFor = [](unsigned structure, uint32_t column) {
        return static_cast<PropertyOffset>((structure + column) % 50);
    };
    Vector<Structure*> structures;
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        structures.append(fixture.createStructure());
        for (uint32_t column : columns)
            fixture.caches().set(structures[i], fixture.name(column), SlotAccess::Read, ownValueSlot(structures[i], offsetFor(i, column)));
        fixture.caches().set(structures[i], fixture.name(columns[0]), SlotAccess::Define, absentSlot(structures[i], 0));
        AOT_TEST_CHECK(hasPublishedWords(fixture, *instance));
    }
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        for (uint32_t column : columns) {
            AOT_TEST_CHECK(fixture.cellAt(structures[i], column, SlotAccess::Read).name == column);
            AOT_TEST_CHECK(isSameSlot(front.find(structures[i]->id(), column, SlotAccess::Read), ownValueSlot(structures[i], offsetFor(i, column))));
            AOT_TEST_CHECK(!front.find(structures[i]->id(), column, SlotAccess::Store));
        }
        AOT_TEST_CHECK(isSameSlot(front.find(structures[i]->id(), columns[0], SlotAccess::Define), absentSlot(structures[i], 0)));
    }
    fixture.vm().aotState().instances.clear();
}

void secondInstanceClearsOnce()
{
    TableFixture fixture;
    auto& caches = fixture.caches();
    auto& instances = fixture.vm().aotState().instances;
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    Slot slot = ownValueSlot(structure, 0);
    std::array<ZeroedInstance, 3> storage { createZeroedInstance(), createZeroedInstance(), createZeroedInstance() };
    auto fill = [&] {
        caches.set(structure, fixture.name(1), SlotAccess::Read, slot);
        caches.set(structure, fixture.nameWithoutNumber(2), SlotAccess::Read, slot);
    };
    auto isFilled = [&] {
        return !!caches.find(id, fixture.name(1), SlotAccess::Read) && !!caches.find(id, fixture.nameWithoutNumber(2), SlotAccess::Read);
    };
    auto createInstance = [&](unsigned index) {
        instances.append(storage[index].get());
        caches.didCreateInstance(*storage[index]);
        AOT_TEST_CHECK(hasPublishedWords(fixture, *storage[index]));
    };

    createInstance(0);
    fill();
    AOT_TEST_CHECK(!caches.isSharedByInstances() && isFilled());

    createInstance(1);
    AOT_TEST_CHECK(caches.isSharedByInstances());
    AOT_TEST_CHECK(!fixture.numberOfSlots() && !fixture.front().hasRow(id) && !rowOf(structure));

    fill();
    createInstance(2);
    AOT_TEST_CHECK(caches.isSharedByInstances() && isFilled());
    instances.clear();
}

void onlyInstanceCountsAsRun()
{
    TableFixture fixture;
    VMState& state = fixture.vm().aotState();
    std::array<ZeroedInstance, 2> storage { createZeroedInstance(), createZeroedInstance() };
    Instance* first = storage[0].get();
    uint8_t& hasRun = first->propertyCaches.hasRunSinceClear;

    state.instances.append(first);
    state.didChangeInstances();
    AOT_TEST_CHECK(state.onlyInstance == first && hasRun);

    state.instances.append(storage[1].get());
    state.didChangeInstances();
    AOT_TEST_CHECK(!state.onlyInstance);
    hasRun = false;

    state.instances.removeLast();
    state.didChangeInstances();
    AOT_TEST_CHECK(state.onlyInstance == first && hasRun);
    state.instances.clear();
    state.didChangeInstances();
}

void absenceBesidePairDoesNotOutliveItsCount()
{
    using KeyOfPair = StringKeyCache::KeyOfPair;
    constexpr uint32_t mask = KeyOfPair::maskBelowOtherRead;
    TableFixture fixture;
    ZeroedInstance instance = createZeroedInstance();
    fixture.vm().aotState().instances.append(instance.get());
    fixture.caches().didCreateInstance(*instance);
    std::array<KeyOfPair, StringKeyCache::waysOfKeysOfPairs> pairs { };
    instance->realm.keysOfPairs = pairs.data();
    instance->realm.keyOfPairMask = StringKeyCache::keyOfPairMaskFor(pairs.size());
    PropertyCacheWords& words = instance->propertyCaches;
    auto fileAbsence = [&] {
        pairs[1].structureIDOfPlace = 64;
        pairs[1].slotOfPlace = KeyOfPair::markOf(KeyOfPair::OtherRead::Absence) | (words.timesAbsencesWereForgotten & mask);
        words.pairsHavePlaces = 1;
    };
    auto absenceHolds = [&] {
        return pairs[1].structureIDOfPlace && !((pairs[1].slotOfPlace ^ words.timesAbsencesWereForgotten) & mask);
    };
    auto forgetAbsences = [&] {
        words.placesSayAbsence = 1;
        fixture.caches().didChangeObjectOnSomeChain(nullptr, nullptr);
    };

    for (uint32_t countWhenFiled : { 0u, 5u, mask, mask + 1 }) {
        words.timesAbsencesWereForgotten = countWhenFiled;
        fileAbsence();
        AOT_TEST_CHECK(absenceHolds());
        forgetAbsences();
        AOT_TEST_CHECK(words.timesAbsencesWereForgotten == countWhenFiled + 1 && !absenceHolds());

        uint32_t countWhenLowBitsAreZero = (countWhenFiled + 1 + mask) & ~mask;
        if (words.timesAbsencesWereForgotten != countWhenLowBitsAreZero) {
            words.timesAbsencesWereForgotten = countWhenLowBitsAreZero - 1;
            forgetAbsences();
        }
        if (words.timesAbsencesWereForgotten != countWhenFiled + mask + 1) {
            words.timesAbsencesWereForgotten = countWhenFiled + mask;
            forgetAbsences();
        }
        AOT_TEST_CHECK(words.timesAbsencesWereForgotten == countWhenFiled + mask + 1);
        AOT_TEST_CHECK(!absenceHolds());
    }
    fixture.vm().aotState().instances.clear();
}

} // namespace

std::span<const NativeTest> propertyLookupTableTests()
{
    static constexpr NativeTest tests[] = {
        { "property-lookup-table/eviction-keeps-filling"_s, evictionKeepsFilling },
        { "property-lookup-table/growth-keeps-entries-and-publishes-words"_s, growthKeepsEntriesAndPublishesWords },
        { "property-lookup-table/front-growth-keeps-rows-and-publishes-words"_s, frontGrowthKeepsRowsAndPublishesWords },
        { "property-lookup-table/second-instance-clears-once"_s, secondInstanceClearsOnce },
        { "property-lookup-table/only-instance-counts-as-run"_s, onlyInstanceCountsAsRun },
        { "property-lookup-table/absence-beside-pair-does-not-outlive-its-count"_s, absenceBesidePairDoesNotOutliveItsCount },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
