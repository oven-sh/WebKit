/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTCollectionTestSupport.h"
#include "AOTPropertyLookupTableTestSupport.h"
#include <wtf/SetForScope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

namespace {

using Kind = Slot::Kind;

constexpr unsigned numberOfCandidates = 64;
constexpr uint32_t siblingNumber = 1;
constexpr uint32_t targetNumber = 2;

struct Owners {
    Vector<Strong<Structure>> structures;
    Vector<Strong<Structure>> siblings;
};

Owners createOwnersWithWeakTransitions(TableFixture& fixture, unsigned count)
{
    Owners owners;
    for (unsigned i = 0; i < count; ++i) {
        Structure* owner = fixture.createStructure();
        PropertyOffset offset = invalidOffset;
        owners.structures.append(Strong<Structure>(fixture.vm(), owner));
        owners.siblings.append(Strong<Structure>(fixture.vm(), fixture.addProperty(owner, siblingNumber, offset)));
    }
    return owners;
}

enum class Level : uint8_t { Front, HashedTable };

Structure* cacheAddition(TableFixture& fixture, Structure* owner, Level level)
{
    PropertyOffset offset = invalidOffset;
    Structure* target = fixture.addProperty(owner, targetNumber, offset);
    Slot slot = transitionSlot(owner, target, offset, Slot::dependsOnChain);
    if (level == Level::Front)
        AOT_TEST_CHECK(fixture.front().trySet(owner, targetNumber, SlotAccess::Store, slot));
    else
        fixture.hashedTable().set(fixture.uid(targetNumber), SlotAccess::Store, slot);
    return target;
}

std::optional<Slot> findAddition(TableFixture& fixture, Structure* owner, Level level)
{
    if (level == Level::Front)
        return fixture.front().find(owner->id(), targetNumber, SlotAccess::Store);
    const Slot* slot = fixture.hashedTable().find(owner->id(), fixture.uid(targetNumber), SlotAccess::Store);
    return slot ? std::optional<Slot>(*slot) : std::nullopt;
}

NEVER_INLINE Vector<Structure*> cacheAdditionsWithUnreferencedTargets(TableFixture& fixture, const Owners& owners, Level level)
{
    Vector<Structure*> targets;
    for (auto& owner : owners.structures)
        targets.append(cacheAddition(fixture, owner.get(), level));
    return targets;
}

NEVER_INLINE Vector<Structure*> createRowsForUnreferencedOwners(TableFixture& fixture)
{
    Vector<Structure*> owners;
    for (unsigned i = 0; i < numberOfCandidates; ++i) {
        owners.append(fixture.createStructure());
        AOT_TEST_CHECK(fixture.front().trySet(owners.last(), 1, SlotAccess::Read, ownValueSlot(owners.last(), 0)));
    }
    return owners;
}

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
        if (!vm.heap.isMarked(structure))
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

size_t sweepWithValidation(TableFixture& fixture, CollectionScope scope)
{
    SetForScope validation(fixture.vm().aotState().validatesRuntime, true);
    return fixture.sweep(scope);
}

void checkLivenessOfPayload(Slot slot, Structure* live, Structure* dead, const Invocable<bool(JSCell*)> auto& isMarked)
{
    uintptr_t cageBase = MarkedBlockCage::baseOf(live);
    switch (Slot::payloadOf(slot.kind)) {
    case SlotPayload::Holder:
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        slot.holder = live;
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        slot.holder = dead;
        AOT_TEST_CHECK(!slot.namesOnlyLiveCells(isMarked, cageBase));
        return;
    case SlotPayload::FieldAndHolder:
        AOT_TEST_CHECK(MarkedBlockCage::baseOf(dead) == cageBase);
        slot.fieldAndHolder.holder = MarkedBlockCage::encode(cageBase, live);
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        slot.fieldAndHolder.holder = MarkedBlockCage::encode(cageBase, dead);
        AOT_TEST_CHECK(!slot.namesOnlyLiveCells(isMarked, cageBase));
        return;
    case SlotPayload::ExtraProperty:
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        slot.extraProperty.structureID = live->id();
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        slot.extraProperty.structureID = dead->id();
        AOT_TEST_CHECK(!slot.namesOnlyLiveCells(isMarked, cageBase));
        return;
    case SlotPayload::ExtraPropertyAddition:
        slot.extraPropertyAddition.newStructureID = live->id();
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        slot.extraPropertyAddition.newStructureID = dead->id();
        AOT_TEST_CHECK(!slot.namesOnlyLiveCells(isMarked, cageBase));
        return;
    case SlotPayload::Transition:
        slot.transition.newStructureID = live->id();
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        slot.transition.newStructureID = dead->id();
        AOT_TEST_CHECK(!slot.namesOnlyLiveCells(isMarked, cageBase));
        return;
    case SlotPayload::Constant:
        slot.constant = JSValue::encode(jsNumber(1));
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        slot.constant = JSValue::encode(live);
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        slot.constant = JSValue::encode(dead);
        AOT_TEST_CHECK(!slot.namesOnlyLiveCells(isMarked, cageBase));
        return;
    case SlotPayload::None:
    case SlotPayload::WordIndex:
    case SlotPayload::Field:
    case SlotPayload::CustomAccessor:
        slot.holder = dead;
        AOT_TEST_CHECK(slot.namesOnlyLiveCells(isMarked, cageBase));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void slotLivenessByKind()
{
    TableFixture fixture;
    Structure* live = fixture.createStructure();
    Structure* dead = fixture.createStructure();
    auto isMarked = [&](JSCell* cell) {
        return cell != dead;
    };
    uintptr_t cageBase = MarkedBlockCage::baseOf(live);

    AOT_TEST_CHECK((Slot { }).namesOnlyLiveCells(isMarked, cageBase));
    for (Kind kind : allSlotKinds) {
        AOT_TEST_CHECK(Slot::create(dead->id(), kind, 0, 0).namesOnlyLiveCells(isMarked, cageBase) == (kind == Kind::Empty));
        checkLivenessOfPayload(Slot::create(live->id(), kind, 0, 0), live, dead, isMarked);
        checkLivenessOfPayload(Slot::create(StructureID(), kind, 0, 0), live, dead, isMarked);
    }
}

void sweepDropsDeadAdditionTargets(CollectionScope scope, Level level)
{
    TableFixture fixture;
    VM& vm = fixture.vm();
    Owners owners = createOwnersWithWeakTransitions(fixture, numberOfCandidates);
    cacheAdditionsWithUnreferencedTargets(fixture, owners, level);
    Owners liveOwners = createOwnersWithWeakTransitions(fixture, 1);
    Structure* liveOwner = liveOwners.structures[0].get();
    Strong<Structure> liveTarget(vm, cacheAddition(fixture, liveOwner, level));
    size_t numberEvicted = valueOf(vm, RuntimeCounterID::HashedTableEvictions);

    size_t numberDropped = sweepWithValidation(fixture, scope);

    unsigned numberOfResetEntries = 0;
    for (auto& owner : owners.structures) {
        std::optional<Slot> slot = findAddition(fixture, owner.get(), level);
        if (!slot) {
            AOT_TEST_CHECK(level != Level::Front || (fixture.front().hasRow(owner->id()) && fixture.cellAt(owner.get(), targetNumber, SlotAccess::Store).isEmpty()));
            AOT_TEST_CHECK(!fixture.caches().hasChainDependent(vm, owner->id().bits(), fixture.uid(targetNumber)));
            ++numberOfResetEntries;
            continue;
        }
        AOT_TEST_CHECK(slot->kind == Kind::Transition);
        AOT_TEST_CHECK(vm.heap.isMarked(slot->transition.newStructureID.decode()));
    }
    AOT_TEST_CHECK(numberDropped && numberOfResetEntries == numberDropped + numberEvicted);

    auto checkLiveEntry = [&] {
        std::optional<Slot> slot = findAddition(fixture, liveOwner, level);
        AOT_TEST_CHECK(slot && slot->kind == Kind::Transition && slot->transition.newStructureID == liveTarget->id());
    };
    checkLiveEntry();
    sweepWithValidation(fixture, scope);
    checkLiveEntry();
}

void edenSweepDropsDeadAdditionTargets()
{
    sweepDropsDeadAdditionTargets(CollectionScope::Eden, Level::Front);
}

void edenSweepDropsDeadAdditionTargetsOfHashedTable()
{
    sweepDropsDeadAdditionTargets(CollectionScope::Eden, Level::HashedTable);
}

void fullSweepDropsDeadAdditionTargets()
{
    sweepDropsDeadAdditionTargets(CollectionScope::Full, Level::Front);
    sweepDropsDeadAdditionTargets(CollectionScope::Full, Level::HashedTable);
}

void sweepReleasesRowsOfDeadOwners(CollectionScope scope)
{
    TableFixture fixture;
    VM& vm = fixture.vm();
    auto& front = fixture.front();
    Strong<Structure> liveOwner(vm, fixture.createStructure());
    AOT_TEST_CHECK(front.trySet(liveOwner.get(), 1, SlotAccess::Read, ownValueSlot(liveOwner.get(), 2)));
    Vector<Structure*> owners = createRowsForUnreferencedOwners(fixture);

    size_t numberDropped = sweepWithValidation(fixture, scope);

    Vector<Structure*> deadOwners = findDeadStructures(vm, owners);
    checkRowsAreReleased(fixture, deadOwners);
    AOT_TEST_CHECK(numberDropped == deadOwners.size());
    AOT_TEST_CHECK(!front.removeDeadEntries(vm, CollectionScope::Full));
    AOT_TEST_CHECK(isSameSlot(front.find(liveOwner->id(), 1, SlotAccess::Read), ownValueSlot(liveOwner.get(), 2)));
}

void edenSweepReleasesRowsOfDeadOwners()
{
    sweepReleasesRowsOfDeadOwners(CollectionScope::Eden);
}

void fullSweepReleasesRowsOfDeadOwners()
{
    sweepReleasesRowsOfDeadOwners(CollectionScope::Full);
}

void edenSweepReleasesRecreatedRowOnce()
{
    TableFixture fixture;
    VM& vm = fixture.vm();
    auto& front = fixture.front();
    Strong<Structure> liveOwner(vm, fixture.createStructure());
    AOT_TEST_CHECK(front.trySet(liveOwner.get(), 1, SlotAccess::Read, ownValueSlot(liveOwner.get(), 2)));
    Vector<Structure*> owners = createRowsTwiceForUnreferencedOwners(fixture);

    sweepWithValidation(fixture, CollectionScope::Eden);

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
    size_t numberEvicted = valueOf(vm, RuntimeCounterID::HashedTableEvictions);

    size_t numberDropped = sweepWithValidation(fixture, scope);

    unsigned numberOfResetEntries = 0;
    for (auto& owner : owners) {
        std::optional<Slot> slot = caches.find(owner->id(), fixture.name(1), SlotAccess::Read);
        if (slot)
            AOT_TEST_CHECK(slot->kind == Kind::Value && vm.heap.isMarked(slot->holder));
        else
            ++numberOfResetEntries;
    }
    AOT_TEST_CHECK(numberDropped && numberOfResetEntries == numberDropped + numberEvicted);
    AOT_TEST_CHECK(isSameSlot(caches.find(liveOwner->id(), fixture.name(1), SlotAccess::Read), liveSlot));
}

void edenSweepDropsDeadHolders()
{
    sweepDropsDeadHolders(CollectionScope::Eden);
}

void fullSweepDropsDeadHolders()
{
    sweepDropsDeadHolders(CollectionScope::Full);
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

    sweepWithValidation(fixture, CollectionScope::Eden);

    unsigned numberOfFreedRecords = 0;
    for (unsigned i = 0; i < numberOfCandidates; ++i) {
        bool isInUse = fixture.caches().customAccessors().isInUse(records[i]);
        AOT_TEST_CHECK(!!fixture.front().find(owners[i]->id(), 1, SlotAccess::Read) == isInUse);
        AOT_TEST_CHECK(isInUse || !fixture.hashedTable().find(owners[i]->id(), fixture.uid(1), SlotAccess::Read));
        numberOfFreedRecords += !isInUse;
    }
    AOT_TEST_CHECK(numberOfFreedRecords);
}

NEVER_INLINE void cacheAbsenceForUnreferencedOwners(TableFixture& fixture, UniquedStringImpl* uid)
{
    for (unsigned i = 0; i < numberOfCandidates; ++i)
        fixture.hashedTable().set(uid, SlotAccess::Read, absentSlot(fixture.createStructure(), 0));
}

void sweepKeepsNamesUntilNextWrite()
{
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    Strong<Structure> liveOwner(fixture.vm(), fixture.createStructure());
    UniquedStringImpl* uid = fixture.uid(1);
    unsigned initialRefCount = uid->refCount();
    cacheAbsenceForUnreferencedOwners(fixture, uid);
    size_t numberOfEntries = fixture.numberOfSlots();
    AOT_TEST_CHECK(numberOfEntries && uid->refCount() == initialRefCount + numberOfEntries);

    size_t numberDropped = sweepWithValidation(fixture, CollectionScope::Eden);

    AOT_TEST_CHECK(numberDropped && fixture.numberOfSlots() == numberOfEntries - numberDropped);
    AOT_TEST_CHECK(uid->refCount() == initialRefCount + numberOfEntries);
    table.remove(liveOwner->id(), uid, SlotAccess::Read);
    AOT_TEST_CHECK(uid->refCount() == initialRefCount + numberOfEntries - numberDropped);
}

void edenSweepLooksOnlyAtWhatWasWritten()
{
    TableFixture fixture;
    VM& vm = fixture.vm();
    auto& front = fixture.front();
    auto& table = fixture.hashedTable();
    Owners liveOwners = createOwnersWithWeakTransitions(fixture, 1);
    Structure* liveOwner = liveOwners.structures[0].get();
    Strong<Structure> liveTarget(vm, cacheAddition(fixture, liveOwner, Level::Front));
    Owners owners = createOwnersWithWeakTransitions(fixture, numberOfCandidates);
    Vector<Structure*> targets = cacheAdditionsWithUnreferencedTargets(fixture, owners, Level::Front);
    Vector<Strong<Structure>> retainedTargets;
    for (unsigned i = 0; i < numberOfCandidates; ++i) {
        retainedTargets.append(Strong<Structure>(vm, targets[i]));
        table.set(fixture.uid(targetNumber), SlotAccess::Store, *front.find(owners.structures[i]->id(), targetNumber, SlotAccess::Store));
    }
    AOT_TEST_CHECK(!sweepWithValidation(fixture, CollectionScope::Eden));

    retainedTargets.clear();
    vm.heap.collectSync(CollectionScope::Full);
    size_t numberOfDeadTargets = findDeadStructures(vm, targets).size();
    AOT_TEST_CHECK(numberOfDeadTargets > 1);

    size_t numberOfSlots = fixture.numberOfSlots();
    AOT_TEST_CHECK(!front.removeDeadEntries(vm, CollectionScope::Eden) && !table.removeDeadEntries(vm, CollectionScope::Eden));
    AOT_TEST_CHECK(fixture.numberOfSlots() == numberOfSlots);
    size_t numberOfDeadTargetsInHashedTable = 0;
    std::optional<unsigned> rewrittenOwner;
    for (unsigned i = 0; i < numberOfCandidates; ++i) {
        AOT_TEST_CHECK(findAddition(fixture, owners.structures[i].get(), Level::Front));
        numberOfDeadTargetsInHashedTable += !vm.heap.isMarked(targets[i]) && findAddition(fixture, owners.structures[i].get(), Level::HashedTable);
        if (!vm.heap.isMarked(targets[i]) && !rewrittenOwner)
            rewrittenOwner = i;
    }
    AOT_TEST_CHECK(numberOfDeadTargetsInHashedTable && rewrittenOwner);

    AOT_TEST_CHECK(front.trySet(liveOwner, targetNumber, SlotAccess::Store, *front.find(liveOwner->id(), targetNumber, SlotAccess::Store)));
    AOT_TEST_CHECK(!front.removeDeadEntries(vm, CollectionScope::Eden));

    Structure* owner = owners.structures[*rewrittenOwner].get();
    AOT_TEST_CHECK(front.trySet(owner, targetNumber, SlotAccess::Store, *front.find(owner->id(), targetNumber, SlotAccess::Store)));
    AOT_TEST_CHECK(front.removeDeadEntries(vm, CollectionScope::Eden) == 1);
    for (unsigned i = 0; i < numberOfCandidates; ++i)
        AOT_TEST_CHECK(!findAddition(fixture, owners.structures[i].get(), Level::Front) == (i == *rewrittenOwner));

    AOT_TEST_CHECK(front.removeDeadEntries(vm, CollectionScope::Full) == numberOfDeadTargets - 1);
    AOT_TEST_CHECK(table.removeDeadEntries(vm, CollectionScope::Full) == numberOfDeadTargetsInHashedTable);
    AOT_TEST_CHECK(findAddition(fixture, liveOwner, Level::Front));
    AOT_TEST_CHECK(!front.removeDeadEntries(vm, CollectionScope::Full));
}

void sweepKeepsRowsThatNameNothingElse()
{
    TableFixture fixture;
    VM& vm = fixture.vm();
    auto& front = fixture.front();
    Vector<Strong<Structure>> owners;
    for (unsigned i = 0; i < numberOfCandidates; ++i) {
        owners.append(Strong<Structure>(vm, fixture.createStructure()));
        AOT_TEST_CHECK(front.trySet(owners.last().get(), 1, SlotAccess::Read, ownValueSlot(owners.last().get(), 0)));
    }
    vm.heap.collectSync(CollectionScope::Full);
    AOT_TEST_CHECK(!front.removeDeadEntries(vm, CollectionScope::Eden) && !front.removeDeadEntries(vm, CollectionScope::Full));
    for (auto& owner : owners)
        AOT_TEST_CHECK(front.find(owner->id(), 1, SlotAccess::Read));
}

} // namespace

std::span<const NativeTest> propertyLookupTableCollectionTests()
{
    static constexpr NativeTest tests[] = {
        { "property-lookup-table-collection/slot-liveness-by-kind"_s, slotLivenessByKind },
        { "property-lookup-table-collection/eden-sweep-drops-dead-addition-targets"_s, edenSweepDropsDeadAdditionTargets },
        { "property-lookup-table-collection/eden-sweep-drops-dead-addition-targets-of-hashed-table"_s, edenSweepDropsDeadAdditionTargetsOfHashedTable },
        { "property-lookup-table-collection/full-sweep-drops-dead-addition-targets"_s, fullSweepDropsDeadAdditionTargets },
        { "property-lookup-table-collection/eden-sweep-releases-rows-of-dead-owners"_s, edenSweepReleasesRowsOfDeadOwners },
        { "property-lookup-table-collection/full-sweep-releases-rows-of-dead-owners"_s, fullSweepReleasesRowsOfDeadOwners },
        { "property-lookup-table-collection/eden-sweep-releases-recreated-row-once"_s, edenSweepReleasesRecreatedRowOnce },
        { "property-lookup-table-collection/eden-sweep-drops-dead-holders"_s, edenSweepDropsDeadHolders },
        { "property-lookup-table-collection/full-sweep-drops-dead-holders"_s, fullSweepDropsDeadHolders },
        { "property-lookup-table-collection/sweep-drops-slots-of-freed-accessor-records"_s, sweepDropsSlotsOfFreedAccessorRecords },
        { "property-lookup-table-collection/sweep-keeps-names-until-next-write"_s, sweepKeepsNamesUntilNextWrite },
        { "property-lookup-table-collection/eden-sweep-looks-only-at-what-was-written"_s, edenSweepLooksOnlyAtWhatWasWritten },
        { "property-lookup-table-collection/sweep-keeps-rows-that-name-nothing-else"_s, sweepKeepsRowsThatNameNothingElse },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
