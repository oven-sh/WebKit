/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTPropertyLookupTableTestSupport.h"

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

} // namespace

std::span<const NativeTest> hashedSlotTableTests()
{
    static constexpr NativeTest tests[] = {
        { "hashed-slot-table/growth-keeps-the-address-and-every-entry"_s, growthKeepsTheAddressAndEveryEntry },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
