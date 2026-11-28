/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTPropertyListCache.h"
#include "AOTPropertyLookupTableTestSupport.h"
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

namespace {

constexpr uint32_t initialNumberOfEntries = HashedSlotTable::initialNumberOfEntries;
constexpr unsigned numberOfKeysTakingTurns = 2 * numberOfSlotAccesses;
static_assert(numberOfKeysTakingTurns > HashedSlot::ways);

void growthKeepsTheAddressAndEveryEntry()
{
    constexpr unsigned maxNumberOfFills = 16 * initialNumberOfEntries;
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    UniquedStringImpl* uid = fixture.uid(1);
    const HashedSlot* entries = table.entries();
    AOT_TEST_CHECK(table.numberOfEntries() == initialNumberOfEntries && !table.numberOfFilledEntries());

    unsigned numberOfFills = 0;
    HashedSlotTable::SetResult result = HashedSlotTable::SetResult::Stored;
    while (result == HashedSlotTable::SetResult::Stored) {
        AOT_TEST_CHECK(numberOfFills < maxNumberOfFills);
        result = table.set(uid, SlotAccess::Read, absentSlot(fixture.createStructure(), 0));
        ++numberOfFills;
    }

    AOT_TEST_CHECK(table.numberOfEntries() == 2 * initialNumberOfEntries && table.entries() == entries);
    AOT_TEST_CHECK(table.numberOfFilledEntries() >= initialNumberOfEntries / 2);
    size_t numberOfEntriesFound = 0;
    size_t numberInUpperHalf = 0;
    table.forEachEntry([&](const HashedSlot& entry) {
        AOT_TEST_CHECK(table.find(entry.slot.structureID, entry.uid(), entry.access()) == &entry.slot);
        ++numberOfEntriesFound;
        numberInUpperHalf += &entry >= entries + initialNumberOfEntries;
    });
    AOT_TEST_CHECK(numberOfEntriesFound == table.numberOfFilledEntries());
    AOT_TEST_CHECK(numberInUpperHalf && numberInUpperHalf < numberOfEntriesFound);

    table.clear();
    AOT_TEST_CHECK(!table.numberOfFilledEntries() && !fixture.numberOfSlots());
    AOT_TEST_CHECK(table.numberOfEntries() == 2 * initialNumberOfEntries && table.entries() == entries);
}

// What HashedSlot::structureMultiplier is chosen for. A size of the cells of Structure that it spreads badly fails here.
void structuresMadeInRowFillHalfOfTableOfEverySize()
{
    constexpr uint32_t lastNumberOfEntries = 64 * initialNumberOfEntries;
    constexpr unsigned maxNumberOfFills = 2 * lastNumberOfEntries;
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    UniquedStringImpl* uid = fixture.uid(1);

    for (unsigned numberOfFills = 0; table.numberOfEntries() < lastNumberOfEntries; ++numberOfFills) {
        AOT_TEST_CHECK(numberOfFills < maxNumberOfFills);
        uint32_t numberOfEntries = table.numberOfEntries();
        if (table.set(uid, SlotAccess::Read, absentSlot(fixture.createStructure(), 0)) == HashedSlotTable::SetResult::StoredAndGrew)
            AOT_TEST_CHECK(table.numberOfEntries() == 2 * numberOfEntries && table.numberOfFilledEntries() >= numberOfEntries / 2);
    }
    table.clear();
}

// What structureIDPlaceShift is chosen for, and the product of AbsenceFilter. A size of the cells of Structure that leaves a kept bit the same in every StructureID fails here.
// The structure heap of a release build has its blocks 32768 bytes apart and that of a build with ASAN 16384: the three tables hold for both.
void structuresMadeInRowTakeMoreThanHalfOfThePlacesOfTablesKeyedByTheID()
{
    constexpr unsigned numberOfStructures = 4 * AbsenceFilter::numberOfFilters;
    TableFixture fixture;
    Vector<StructureID> structureIDs;
    for (unsigned index = 0; index < numberOfStructures; ++index)
        structureIDs.append(fixture.createStructure()->id());

    auto numberOfPlacesTaken = [&](unsigned numberOfPlaces, const auto& placeOf) {
        BitVector taken(numberOfPlaces);
        for (StructureID structureID : structureIDs)
            taken.set(placeOf(structureID));
        return taken.bitCount();
    };
    AOT_TEST_CHECK(numberOfPlacesTaken(AbsenceFilter::numberOfFilters, [](StructureID structureID) { return AbsenceFilter::indexFor(structureID.bits()); }) > AbsenceFilter::numberOfFilters / 2);
    AOT_TEST_CHECK(numberOfPlacesTaken(NameShapesOfStructure::numberOfEntries, [](StructureID structureID) { return NameShapesOfStructure::indexFor(structureID); }) > NameShapesOfStructure::numberOfEntries / 2);
    AOT_TEST_CHECK(numberOfPlacesTaken(FirstElementTransition::numberOfEntries, [](StructureID structureID) { return FirstElementTransition::indexFor(structureID, 0); }) > FirstElementTransition::numberOfEntries / 2);
    AOT_TEST_CHECK(numberOfPlacesTaken(PropertyListCache::numberOfEntries, [](StructureID structureID) { return PropertyListCache::placeOf(structureID, 0, 0); }) > PropertyListCache::numberOfEntries / 2);
    AOT_TEST_CHECK(numberOfPlacesTaken(PropertyCacheWords::numberOfErrorsWithMessage, [](StructureID structureID) { return structureID.bits() >> PropertyCacheWords::shiftOfErrorWithMessage & (PropertyCacheWords::numberOfErrorsWithMessage - 1); }) > PropertyCacheWords::numberOfErrorsWithMessage / 2);
    AOT_TEST_CHECK(numberOfPlacesTaken(PropertyCacheWords::numberOfStructuresBelowBuiltinClasses, [](StructureID structureID) { return structureID.bits() >> PropertyCacheWords::shiftOfPlaceOfStructureBelowBuiltinClass & (PropertyCacheWords::numberOfStructuresBelowBuiltinClasses - 1); }) > PropertyCacheWords::numberOfStructuresBelowBuiltinClasses / 2);
    AOT_TEST_CHECK(numberOfPlacesTaken(ForwardingProxyHandlers::size, [](StructureID structureID) { return ForwardingProxyHandlers::offsetFor(structureID.bits()) >> ForwardingProxyHandlers::sizeShift; }) > ForwardingProxyHandlers::size / 2);
    // The values under one key. This table has four times the places that a block of the structure heap has cells, and the blocks of a release build lie twice as far apart
    // as it has cells: a quarter is all that a sum with the ID can be asked for there.
    AOT_TEST_CHECK(numberOfPlacesTaken(KeyedValuePlace::numberOfPlacesPerAccess, [](StructureID structureID) { return KeyedValuePlace::offsetFor(nullptr, structureID) >> KeyedValuePlace::sizeShift; }) > KeyedValuePlace::numberOfPlacesPerAccess / 4);
}

// PropertyListCache::isSecondWalk() forgets the walks it has seen after as many as the cache has entries. Of that many structures made in a row, and of every second
// of twice as many, few fall on a place that another has taken: sixteen where the places are as good as random, and eighty where WTF::intHash() took the StructureIDs
// themselves. The two words are those of a compiled object with one name of its own beside its fields.
void structuresMadeInRowSeldomShareAPlaceOfTheWalksRemembered()
{
    constexpr unsigned numberOfWalks = PropertyListCache::numberOfEntries;
    constexpr uint32_t shapeWord = 0xffffffeb;
    constexpr uint32_t orderAndCount = 0x10000;
    TableFixture fixture;
    Vector<StructureID> structureIDs;
    for (unsigned index = 0; index < 4 * numberOfWalks; ++index)
        structureIDs.append(fixture.createStructure()->id());

    for (unsigned step : { 1u, 2u }) {
        for (unsigned start = 0; start + step * numberOfWalks <= structureIDs.size(); start += step * numberOfWalks) {
            BitVector taken(PropertyListCache::numberOfWalksRemembered);
            unsigned numberThatShare = 0;
            for (unsigned index = 0; index < numberOfWalks; ++index) {
                unsigned place = PropertyListCache::placeOfWalk(structureIDs[start + index * step], shapeWord, orderAndCount);
                numberThatShare += taken.get(place);
                taken.set(place);
            }
            AOT_TEST_CHECK(numberThatShare <= 40);
        }
    }
}

} // namespace

std::span<const NativeTest> hashedSlotTableTests()
{
    static constexpr NativeTest tests[] = {
        { "hashed-slot-table/growth-keeps-the-address-and-every-entry"_s, growthKeepsTheAddressAndEveryEntry },
        { "hashed-slot-table/structures-made-in-a-row-fill-half-of-a-table-of-every-size"_s, structuresMadeInRowFillHalfOfTableOfEverySize },
        { "hashed-slot-table/structures-made-in-a-row-take-more-than-half-of-the-places-of-tables-keyed-by-the-id"_s, structuresMadeInRowTakeMoreThanHalfOfThePlacesOfTablesKeyedByTheID },
        { "hashed-slot-table/structures-made-in-a-row-seldom-share-a-place-of-the-walks-remembered"_s, structuresMadeInRowSeldomShareAPlaceOfTheWalksRemembered },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
