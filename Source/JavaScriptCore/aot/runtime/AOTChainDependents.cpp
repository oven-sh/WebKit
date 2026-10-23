/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChainDependents.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTGlobalBindings.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCaches.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "StructureRareDataInlines.h"
#include <bit>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ChainDependents);

void ChainDependents::NameIndex::add(const ChainDependent& dependent)
{
    if (!dependent.uidOrNull) {
        unnamed.append(dependent);
        return;
    }
    named.ensure(dependent.uidOrNull, [] {
        return Vector<ChainDependent, 1>();
    }).iterator->value.append(dependent);
}

class ChainDependents::StructureStateOwner final : public ChainDependentOwner {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(StructureStateOwner);
public:
    void add(StructureID id) { m_structures.add(id); }

    void removeDeadStructures(VM& vm)
    {
        m_structures.removeIf([&](StructureID id) {
            return !vm.heap.isMarked(id.decode());
        });
    }

    void dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl*) final
    {
        StructureID id = std::bit_cast<StructureID>(structureIDBits);
        if (!m_structures.contains(id))
            return;
        Structure* structure = id.decode();
        structure->setDefinitelyNonThenableState(Structure::DefinitelyNonThenableState::NotComputed);
        StructureRareData* rareData = structure->tryRareData();
        if (!rareData)
            return;
        for (unsigned index = 0; index < numberOfCachedSpecialPropertyKeys; ++index)
            rareData->clearCachedSpecialProperty(static_cast<CachedSpecialPropertyKey>(index));
    }

    bool hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl*) const final
    {
        return m_structures.contains(std::bit_cast<StructureID>(structureIDBits));
    }

private:
    UncheckedKeyHashSet<StructureID> m_structures;
};

ChainDependents::ChainDependents(State& state)
    : m_state(state)
    , m_structureState(makeUnique<StructureStateOwner>())
    , m_structureStateOwner(addOwner(*m_structureState))
{
    state.caches->add(CacheKind::WeakByKey, "ChainDependents"_s, *this);
}

ChainDependents::~ChainDependents() = default;

ChainDependentOwnerID ChainDependents::addOwner(ChainDependentOwner& owner)
{
    RELEASE_ASSERT(m_owners.size() < maxNumberOfOwners);
    m_owners.append(&owner);
    return ChainDependentOwnerID(static_cast<uint8_t>(m_owners.size() - 1));
}

void ChainDependents::add(std::span<JSObject* const> chainObjects, const ChainDependent& dependent)
{
    if (chainObjects.empty())
        return;
    if (dependent.owner == m_structureStateOwner)
        m_structureState->add(std::bit_cast<StructureID>(dependent.subject));
    for (JSObject* object : chainObjects) {
        ASSERT(object->mayBePrototype());
        ObjectDependents& dependents = m_dependentsByObject.ensure(object, [] {
            return ObjectDependents();
        }).iterator->value;
        if (!dependents.set.add(dependent).isNewEntry)
            continue;
        ++m_numberOfDependentsAddedSinceOwnersWereAsked;
        if (dependents.index)
            dependents.index->add(dependent);
    }
    m_writtenSinceCollection.recordAll();
}

void ChainDependents::didChange(JSObject* changed, UniquedStringImpl* uidOrNull)
{
    ASSERT(changed);
    auto iterator = m_dependentsByObject.find(changed);
    if (iterator == m_dependentsByObject.end())
        return;

    VM& vm = *m_state.instance.vm;
    auto drop = [&](const ChainDependent& dependent) {
        ownerOf(dependent).dropChainDependent(vm, dependent.subject, dependent.uidOrNull);
        count(m_state.instance, RuntimeCounterID::ChainDependentsDropped);
    };

    if (!uidOrNull) {
        ObjectDependents dependents = m_dependentsByObject.take(iterator);
        for (const ChainDependent& dependent : dependents.set)
            drop(dependent);
        return;
    }

    ObjectDependents& all = iterator->value;
    Vector<ChainDependent, 8> dependents;
    if (!all.index && all.set.size() <= maxSizeWithoutIndex) {
        all.set.removeIf([&](const ChainDependent& dependent) {
            if (dependent.uidOrNull && dependent.uidOrNull != uidOrNull)
                return false;
            dependents.append(dependent);
            return true;
        });
    } else {
        if (!all.index) {
            all.index = makeUnique<NameIndex>();
            for (const ChainDependent& dependent : all.set)
                all.index->add(dependent);
        }
        dependents.appendVector(std::exchange(all.index->unnamed, { }));
        dependents.appendVector(all.index->named.take(uidOrNull));
        for (const ChainDependent& dependent : dependents)
            all.set.remove(dependent);
    }
    if (all.set.isEmpty())
        m_dependentsByObject.remove(iterator);
    for (const ChainDependent& dependent : dependents)
        drop(dependent);
}

bool ChainDependents::hasDependents(JSObject* object) const
{
    return m_dependentsByObject.contains(object);
}

size_t ChainDependents::numberOfDependents(JSObject* object) const
{
    auto iterator = m_dependentsByObject.find(object);
    return iterator == m_dependentsByObject.end() ? 0 : iterator->value.set.size();
}

size_t ChainDependents::removeDeadEntries(VM& vm, CollectionScope scope)
{
    // A dependent that its owner evicted is not counted: nothing records an eviction here, and validation reads a count from an unwritten region as a missing record.
    size_t numberOfDependentsOfDeadObjects = 0;
    // To ask the owner of each dependent costs more than all the rest of a full collection's sweep of the tables, and what it finds takes room and does no harm.
    bool dropsEvictedDependents = scope == CollectionScope::Full && m_numberOfDependentsAddedSinceOwnersWereAsked >= m_numberOfDependentsLeftWhenOwnersWereAsked;
    size_t numberOfDependentsLeft = 0;
    m_structureState->removeDeadStructures(vm);
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        m_dependentsByObject.removeIf([&](auto& entry) {
            if (!vm.heap.isMarked(entry.key)) {
                numberOfDependentsOfDeadObjects += entry.value.set.size();
                return true;
            }
            if (!dropsEvictedDependents)
                return false;
            bool removedAny = entry.value.set.removeIf([&](const ChainDependent& dependent) {
                return !ownerOf(dependent).hasChainDependent(vm, dependent.subject, dependent.uidOrNull);
            });
            if (removedAny)
                entry.value.index = nullptr;
            numberOfDependentsLeft += entry.value.set.size();
            return entry.value.set.isEmpty();
        });
    });
    if (dropsEvictedDependents) {
        m_numberOfDependentsAddedSinceOwnersWereAsked = 0;
        m_numberOfDependentsLeftWhenOwnersWereAsked = numberOfDependentsLeft;
    }
    return numberOfDependentsOfDeadObjects;
}

static void failIfPrototypeNameIsNotListed(VM& vm, State& state, const LayoutData& classLayout, UniquedStringImpl* name)
{
    std::span<const AddedNameData> listed = state.tables.classPrototypeNames();
    if (!std::ranges::binary_search(listed, AddedNameData { classLayout.index, AddedNameData::listIsComplete }))
        return;
    if (name == vm.propertyNames->constructor.impl() || parseIndex(*name) || PropertyName(name).isPrivateName())
        return;
    std::optional<uint32_t> number = vm.aotState().identifiers->numberOf(name);
    if (number && std::ranges::binary_search(listed, AddedNameData { classLayout.index, *number }))
        return;
    dataLogLn("AOT: the prototype object of the class with layout ", classLayout.index, " gets the property ", String(name), ", which is not among the names that it ever has");
    crashOnValidationFailure(vm, { ValidationFailureKind::ImpossibleMutation, { }, { }, JSValue() });
}

static void didChangeClassHolder(VM& vm, State& state, JSObject* changed, UniquedStringImpl* uidOrNull)
{
    ClassTable& classes = state.objects->classes;
    ClassSide side;
    const LayoutData* layout = classes.classHeldBy(changed, side);
    if (!layout)
        return;
    if (uidOrNull) {
        if (side == ClassSide::Prototype && !state.tables.classPrototypeNames().empty()) [[unlikely]]
            failIfPrototypeNameIsNotListed(vm, state, *layout, uidOrNull);
        classes.didChangeMember(*layout, side, uidOrNull);
        return;
    }
    // Freezing is reported without a name, behind the change. It changes neither what a name finds nor the prototype, and nothing of a frozen object changes ever again.
    if (changed->isFrozen(vm))
        return;
    classes.didChangeEveryMember(*layout, side);
    classes.didChangePrototypeLink(*layout, side);
}

void didChangeChainObject(VM& vm, JSObject* changed, UniquedStringImpl* uidOrNull)
{
    if (vm.aotState().propertyCaches)
        vm.aotState().propertyCaches->didChangeObjectOnSomeChain(changed, uidOrNull);
    JSGlobalObject* realm = changed->structure()->realm();
    // What a realm does to its Object.prototype before it has an Instance is seen when the Instance is created.
    if (realm && changed == realm->objectPrototype() && Instance::of(realm))
        vm.aotState().propertyCaches->didChangeObjectPrototype(changed, uidOrNull);
    for (Instance* instance : vm.aotState().instances) {
        State* state = instance->state;
        if (!state)
            continue;
        if (state->chainDependents)
            state->chainDependents->didChange(changed, uidOrNull);
        if (state->objects)
            didChangeClassHolder(vm, *state, changed, uidOrNull);
    }
}

void didFlattenStructure(VM& vm, Structure* structure)
{
    if (!validatesRuntime(vm) || !structure->isUncacheableDictionary())
        return;
    VMPropertyCaches& caches = *vm.aotState().propertyCaches;
    bool hasRowInFront = caches.front().hasRow(structure->id());
    unsigned numberOfHashedRows = 0;
    caches.hashedTable().forEachEntry([&](const HashedSlot& entry) {
        numberOfHashedRows += entry.slot.structureID == structure->id();
    });
    if (hasRowInFront || numberOfHashedRows) {
        dataLogLn("AOT validation: an uncacheable dictionary is flattened while the property caches hold rows under its Structure: in the front table ", hasRowInFront, ", in the hashed table ", numberOfHashedRows, ", flattened before ", structure->hasBeenFlattenedBefore(), ", may be a prototype ", structure->mayBePrototype(), ", properties up to offset ", structure->maxOffset());
        crashOnValidationFailure(vm, { ValidationFailureKind::CacheHitDiffers, { }, { }, structure });
    }
}

void didChangeGlobalLexicalBindings(JSGlobalObject* realm)
{
    didChangeChainObject(realm->vm(), realm, nullptr);

    Instance* instance = Instance::of(realm);
    State* state = instance ? instance->state : nullptr;
    if (!state || !state->globalBindings)
        return;
    state->globalBindings->dropConstants();
    // The caller counts the epoch up next, and at this number it starts over.
    if (realm->globalLexicalBindingEpoch() + 1 == Options::thresholdForGlobalLexicalBindingEpoch())
        state->globalBindings->dropLocations();
}

static State* stateOrNull(Structure* structure)
{
    JSGlobalObject* realm = structure->realm();
    Instance* instance = realm ? Instance::of(realm) : nullptr;
    return instance ? instance->state : nullptr;
}

bool storeCreatesOwnProperty(JSGlobalObject* globalObject, Structure* receiverStructure, JSObject* receiverOrNull, UniquedStringImpl* uid)
{
    return storeThroughChain(walkChain({ globalObject, receiverStructure, receiverOrNull, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, uid)) == StoreThroughChain::CreatesOwnProperty;
}

bool storeCreatesOwnProperty(JSGlobalObject* globalObject, Structure* receiverStructure, JSObject* receiverOrNull, UniquedStringImpl* uid, const ChainDependent& dependent)
{
    ChainWalk walk = walkChain({ globalObject, receiverStructure, receiverOrNull, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, uid);
    if (storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
        return false;
    if (!walk.dependsOnChain())
        return true;

    // The walk passes lazy class prototypes of globalObject's Instance only, and they report to its ChainDependents only.
    State* state = stateOrNull(receiverStructure);
    if (!state || &state->instance != Instance::of(globalObject))
        return false;
    state->chainDependents->add(walk.changeableObjects.span(), dependent);
    return true;
}

static PropertyWalk::Presence ownPresenceForStructure(Structure* structure, UniquedStringImpl* uid)
{
    using Presence = PropertyWalk::Presence;
    if (structure->isCompiledStructure())
        return Object::structureGuaranteesAbsence(structure, uid) ? Presence::Absent : Presence::Unknown;
    if (structure->isDictionary() || !structure->propertyAccessesAreCacheable() || !structure->propertyAccessesAreCacheableForAbsence())
        return Presence::Unknown;
    if (structure->typeInfo().overridesGetOwnPropertySlot() || structure->hasNonReifiedStaticProperties())
        return Presence::Unknown;
    unsigned attributes = 0;
    std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(uid, attributes);
    if (!offset)
        return Presence::Unknown;
    return isValidOffset(*offset) ? Presence::Present : Presence::Absent;
}

PropertyWalk::Presence presenceForStructure(VM&, Structure* structure, UniquedStringImpl* uid)
{
    using Presence = PropertyWalk::Presence;
    Presence ownPresence = ownPresenceForStructure(structure, uid);
    if (ownPresence != Presence::Absent)
        return ownPresence;
    State* state = stateOrNull(structure);
    if (!state)
        return Presence::Unknown;

    ChainWalk walk = walkChain({ structure->realm(), structure, nullptr, ChainWalkMode::AllocatesNothing }, uid);
    if (walk.result == ChainWalkResult::Unknown)
        return Presence::Unknown;
    ChainDependents& dependents = *state->chainDependents;
    dependents.add(walk.changeableObjects.span(), { uid, structure->id().bits(), dependents.structureStateOwner() });
    return walk.result == ChainWalkResult::Found ? Presence::Present : Presence::Absent;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
