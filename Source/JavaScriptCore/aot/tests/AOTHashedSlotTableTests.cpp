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

constexpr uint32_t mostEvictionsWithoutGrowth = initialNumberOfEntries / 4;

uint32_t offsetOfSet(Structure* structure, UniquedStringImpl* uid, uint32_t numberOfEntries)
{
    constexpr uint32_t wayBits = (HashedSlot::ways - 1) << HashedSlot::sizeShift;
    return HashedSlot::offsetOfFirstWay(structure->id(), uid, HashedSlot::offsetMaskFor(numberOfEntries)) & ~wayBits;
}

// With each kind of access the two make more keys than a set has ways. They lie in one set of a table of the initial size and in two sets of a table of twice that size.
struct OwnersSharingSet {
    Structure* first { nullptr };
    Structure* second { nullptr };
};

OwnersSharingSet createOwnersSharingSet(TableFixture& fixture, UniquedStringImpl* uid)
{
    constexpr unsigned maxNumberOfCandidates = 1 << 16;
    AOT_TEST_CHECK(fixture.hashedTable().numberOfEntries() == initialNumberOfEntries);
    Structure* first = fixture.createStructure();
    for (unsigned i = 0; i < maxNumberOfCandidates; ++i) {
        Structure* candidate = fixture.createStructure();
        bool sharesSet = offsetOfSet(candidate, uid, initialNumberOfEntries) == offsetOfSet(first, uid, initialNumberOfEntries);
        if (sharesSet && offsetOfSet(candidate, uid, 2 * initialNumberOfEntries) != offsetOfSet(first, uid, 2 * initialNumberOfEntries))
            return { first, candidate };
    }
    AOT_TEST_CHECK(false);
    return { };
}

uint64_t numberOfEvictions(TableFixture& fixture)
{
    return valueOf(fixture.vm(), RuntimeCounterID::HashedTableEvictions);
}

// The keys are filed again and again. While they share a set, all but the first few of the stores evict.
void fileInTurns(TableFixture& fixture, const OwnersSharingSet& owners, UniquedStringImpl* uid, uint64_t maxNumberOfEvictions)
{
    constexpr unsigned maxNumberOfRounds = 4 * initialNumberOfEntries;
    auto& table = fixture.hashedTable();
    uint64_t numberBefore = numberOfEvictions(fixture);
    for (unsigned round = 0; round < maxNumberOfRounds; ++round) {
        for (Structure* structure : { owners.first, owners.second }) {
            for (SlotAccess access : allSlotAccesses) {
                if (numberOfEvictions(fixture) - numberBefore == maxNumberOfEvictions)
                    return;
                table.set(uid, access, ownValueSlot(structure, 0));
            }
        }
    }
}

void growthKeepsTheAddressAndEveryEntry()
{
    constexpr unsigned maxNumberOfFills = 16 * initialNumberOfEntries;
    TableFixture fixture;
    VM& vm = fixture.vm();
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
    AOT_TEST_CHECK(table.numberOfFilledEntries() == numberOfFills - valueOf(vm, RuntimeCounterID::HashedTableEvictions));
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

void keysTakingTurnsInOneSetGrowMostlyEmptyTable()
{
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    UniquedStringImpl* uid = fixture.uid(1);
    OwnersSharingSet owners = createOwnersSharingSet(fixture, uid);

    fileInTurns(fixture, owners, uid, mostEvictionsWithoutGrowth);
    AOT_TEST_CHECK(table.numberOfEntries() == initialNumberOfEntries && table.numberOfFilledEntries() == HashedSlot::ways);

    fileInTurns(fixture, owners, uid, std::numeric_limits<uint64_t>::max());

    AOT_TEST_CHECK(table.numberOfEntries() == 2 * initialNumberOfEntries);
    AOT_TEST_CHECK(numberOfEvictions(fixture) == mostEvictionsWithoutGrowth + 1);
    AOT_TEST_CHECK(table.numberOfFilledEntries() == numberOfKeysTakingTurns && fixture.numberOfSlots() == numberOfKeysTakingTurns);
    for (Structure* structure : { owners.first, owners.second }) {
        for (SlotAccess access : allSlotAccesses)
            AOT_TEST_CHECK(isSameSlot(table.find(structure->id(), uid, access), ownValueSlot(structure, 0)));
    }
}

void fewEvictionsBetweenCollectionsDoNotGrowMostlyEmptyTable()
{
    constexpr unsigned numberOfCollections = 6;
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    UniquedStringImpl* uid = fixture.uid(1);
    OwnersSharingSet owners = createOwnersSharingSet(fixture, uid);

    for (unsigned i = 0; i < numberOfCollections; ++i) {
        fileInTurns(fixture, owners, uid, mostEvictionsWithoutGrowth);
        fixture.sweep(i % 2 ? CollectionScope::Full : CollectionScope::Eden);
    }

    AOT_TEST_CHECK(numberOfEvictions(fixture) == numberOfCollections * mostEvictionsWithoutGrowth);
    AOT_TEST_CHECK(table.numberOfEntries() == initialNumberOfEntries);
    AOT_TEST_CHECK(table.numberOfFilledEntries() == HashedSlot::ways);
}

// A table that is at least half full grows after it has evicted 1/64 of its size, so this stops where the table is half full.
NEVER_INLINE void fileAbsenceForUnreferencedOwnersUntilHalfFull(TableFixture& fixture, UniquedStringImpl* uid)
{
    while (fixture.hashedTable().numberOfFilledEntries() < initialNumberOfEntries / 2)
        fixture.hashedTable().set(uid, SlotAccess::Read, absentSlot(fixture.createStructure(), 0));
}

void sweptEntriesNoLongerCountAsFilled()
{
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    UniquedStringImpl* uid = fixture.uid(1);
    fileAbsenceForUnreferencedOwnersUntilHalfFull(fixture, uid);
    uint32_t numberFilled = table.numberOfFilledEntries();
    AOT_TEST_CHECK(numberFilled >= initialNumberOfEntries / 2 && numberFilled == fixture.numberOfSlots());
    AOT_TEST_CHECK(table.numberOfEntries() == initialNumberOfEntries);

    size_t numberDropped = fixture.sweep(CollectionScope::Full);

    AOT_TEST_CHECK(table.numberOfFilledEntries() == numberFilled - numberDropped);
    AOT_TEST_CHECK(table.numberOfFilledEntries() + numberOfKeysTakingTurns < initialNumberOfEntries / 2);

    // What a table that is at least half full evicts counts across collections.
    OwnersSharingSet owners = createOwnersSharingSet(fixture, uid);
    fileInTurns(fixture, owners, uid, mostEvictionsWithoutGrowth);
    fixture.sweep(CollectionScope::Eden);
    fileInTurns(fixture, owners, uid, mostEvictionsWithoutGrowth);
    AOT_TEST_CHECK(table.numberOfEntries() == initialNumberOfEntries);
}

} // namespace

std::span<const NativeTest> hashedSlotTableTests()
{
    static constexpr NativeTest tests[] = {
        { "hashed-slot-table/growth-keeps-the-address-and-every-entry"_s, growthKeepsTheAddressAndEveryEntry },
        { "hashed-slot-table/keys-taking-turns-in-one-set-grow-a-mostly-empty-table"_s, keysTakingTurnsInOneSetGrowMostlyEmptyTable },
        { "hashed-slot-table/few-evictions-between-collections-do-not-grow-a-mostly-empty-table"_s, fewEvictionsBetweenCollectionsDoNotGrowMostlyEmptyTable },
        { "hashed-slot-table/swept-entries-no-longer-count-as-filled"_s, sweptEntriesNoLongerCountAsFilled },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
