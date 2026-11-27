/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

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

// What structureIDPlaceShift is chosen for. A size of the cells of Structure that leaves a kept bit the same in every StructureID fails here.
void structuresMadeInRowTakeMoreThanHalfOfThePlacesOfTablesThatShiftTheID()
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
}

} // namespace

std::span<const NativeTest> hashedSlotTableTests()
{
    static constexpr NativeTest tests[] = {
        { "hashed-slot-table/growth-keeps-the-address-and-every-entry"_s, growthKeepsTheAddressAndEveryEntry },
        { "hashed-slot-table/structures-made-in-a-row-fill-half-of-a-table-of-every-size"_s, structuresMadeInRowFillHalfOfTableOfEverySize },
        { "hashed-slot-table/structures-made-in-a-row-take-more-than-half-of-the-places-of-tables-that-shift-the-id"_s, structuresMadeInRowTakeMoreThanHalfOfThePlacesOfTablesThatShiftTheID },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
