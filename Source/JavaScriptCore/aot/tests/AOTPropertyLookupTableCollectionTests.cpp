/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCollectionTestSupport.h"
#include "AOTPropertyLookupTableTestSupport.h"
#include "AOTVector.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

namespace {

using Kind = Slot::Kind;

constexpr unsigned numberOfCandidates = 64;

NEVER_INLINE Vector<Structure*> createRowsTwiceForUnreferencedOwners(TableFixture& fixture)
{
    auto& front = fixture.front();
    Vector<Structure*> owners;
    for (unsigned i = 0; i < numberOfCandidates; ++i) {
        Structure* owner = fixture.createStructure();
        AOT_TEST_CHECK(front.trySet(owner, 1, SlotAccess::Read, ownValueSlot(owner, 0)));
        front.releaseRowOf(owner->id());
        AOT_TEST_CHECK(!rowOf(owner));
        AOT_TEST_CHECK(front.trySet(owner, 1, SlotAccess::Read, ownValueSlot(owner, 0)));
        owners.append(owner);
    }
    return owners;
}

Vector<Structure*> findDeadStructures(VM& vm, const Vector<Structure*>& structures)
{
    Vector<Structure*> deadStructures;
    for (Structure* structure : structures) {
        if (!hasSurvivedCollection(vm, structure))
            deadStructures.append(structure);
    }
    return deadStructures;
}

void checkRowsAreReleased(TableFixture& fixture, const Vector<Structure*>& deadStructures)
{
    AOT_TEST_CHECK(!deadStructures.isEmpty());
    for (Structure* structure : deadStructures)
        AOT_TEST_CHECK(!rowOf(structure) && !fixture.front().hasRow(structure->id()));
}

void edenSweepReleasesRecreatedRowOnce()
{
    TableFixture fixture;
    VM& vm = fixture.vm();
    auto& front = fixture.front();
    Strong<Structure> liveOwner(vm, fixture.createStructure());
    AOT_TEST_CHECK(front.trySet(liveOwner.get(), 1, SlotAccess::Read, ownValueSlot(liveOwner.get(), 2)));
    Vector<Structure*> owners = createRowsTwiceForUnreferencedOwners(fixture);

    fixture.sweep(CollectionScope::Eden);

    checkRowsAreReleased(fixture, findDeadStructures(vm, owners));
    AOT_TEST_CHECK(isSameSlot(front.find(liveOwner->id(), 1, SlotAccess::Read), ownValueSlot(liveOwner.get(), 2)));
    AOT_TEST_CHECK(!front.removeDeadEntries(vm, CollectionScope::Full));
}

NEVER_INLINE void cacheValuesOfUnreferencedHolders(TableFixture& fixture, const Vector<Strong<Structure>>& owners)
{
    for (auto& owner : owners)
        fixture.caches().set(owner.get(), fixture.name(1), SlotAccess::Read, inheritedValueSlot(owner.get(), fixture.createPrototype(), 0));
}

void sweepDropsDeadHolders(CollectionScope scope)
{
    TableFixture fixture;
    VM& vm = fixture.vm();
    auto& caches = fixture.caches();
    Vector<Strong<Structure>> owners;
    for (unsigned i = 0; i < numberOfCandidates; ++i)
        owners.append(Strong<Structure>(vm, fixture.createStructure()));
    cacheValuesOfUnreferencedHolders(fixture, owners);
    Strong<JSObject> liveHolder(vm, fixture.createPrototype());
    Strong<Structure> liveOwner(vm, fixture.createStructure());
    Slot liveSlot = inheritedValueSlot(liveOwner.get(), liveHolder.get(), 0);
    caches.set(liveOwner.get(), fixture.name(1), SlotAccess::Read, liveSlot);
    constexpr unsigned numberOfCachedEntries = numberOfCandidates + 1;

    size_t numberDropped = fixture.sweep(scope);

    unsigned numberOfResetEntries = 0;
    for (auto& owner : owners) {
        std::optional<Slot> slot = caches.find(owner->id(), fixture.name(1), SlotAccess::Read);
        if (slot)
            AOT_TEST_CHECK(slot->kind == Kind::Value && vm.heap.isMarked(slot->holder));
        else
            ++numberOfResetEntries;
    }
    AOT_TEST_CHECK(numberDropped && fixture.numberOfSlots() == numberOfCachedEntries - numberOfResetEntries);
    AOT_TEST_CHECK(isSameSlot(caches.find(liveOwner->id(), fixture.name(1), SlotAccess::Read), liveSlot));
}

void edenSweepDropsDeadHolders()
{
    sweepDropsDeadHolders(CollectionScope::Eden);
}

void hostGetter() { }

NEVER_INLINE Vector<uint32_t> cacheGettersOfUnreferencedHolders(TableFixture& fixture, const Vector<Strong<Structure>>& owners)
{
    Vector<uint32_t> records;
    for (auto& owner : owners) {
        std::optional<uint32_t> record = fixture.caches().customAccessors().add({ reinterpret_cast<void*>(hostGetter), fixture.createPrototype(), fixture.globalObject(), 0, 0 });
        AOT_TEST_CHECK(record);
        Slot slot = Slot::createCustomAccessor(owner->id(), Kind::CustomGetter, Slot::dependsOnChain, *record);
        AOT_TEST_CHECK(fixture.front().trySet(owner.get(), 1, SlotAccess::Read, slot));
        fixture.hashedTable().set(fixture.uid(1), SlotAccess::Read, slot);
        records.append(*record);
    }
    return records;
}

void sweepDropsSlotsOfFreedAccessorRecords()
{
    TableFixture fixture;
    VM& vm = fixture.vm();
    Vector<Strong<Structure>> owners;
    for (unsigned i = 0; i < numberOfCandidates; ++i)
        owners.append(Strong<Structure>(vm, fixture.createStructure()));
    Vector<uint32_t> records = cacheGettersOfUnreferencedHolders(fixture, owners);

    fixture.sweep(CollectionScope::Eden);

    unsigned numberOfFreedRecords = 0;
    for (unsigned i = 0; i < numberOfCandidates; ++i) {
        bool isInUse = fixture.caches().customAccessors().isInUse(records[i]);
        AOT_TEST_CHECK(!!fixture.front().find(owners[i]->id(), 1, SlotAccess::Read) == isInUse);
        AOT_TEST_CHECK(isInUse || !fixture.hashedTable().find(owners[i]->id(), fixture.uid(1), SlotAccess::Read));
        numberOfFreedRecords += !isInUse;
    }
    AOT_TEST_CHECK(numberOfFreedRecords);
}

} // namespace

std::span<const NativeTest> propertyLookupTableCollectionTests()
{
    static constexpr NativeTest tests[] = {
        { "property-lookup-table-collection/eden-sweep-releases-recreated-row-once"_s, edenSweepReleasesRecreatedRowOnce },
        { "property-lookup-table-collection/eden-sweep-drops-dead-holders"_s, edenSweepDropsDeadHolders },
        { "property-lookup-table-collection/sweep-drops-slots-of-freed-accessor-records"_s, sweepDropsSlotsOfFreedAccessorRecords },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
