/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChainDependents.h"

#if ENABLE(AOT)

#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTGlobalBindings.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "ArrayPrototype.h"
#include "InlineCacheCompiler.h"
#include "JSCInlines.h"
#include "Lookup.h"
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

ChainDependents::ChainDependents(State& state)
    : m_state(state)
{
    ChainDependentOwnerID self = addOwner(*this);
    RELEASE_ASSERT(self == ChainDependentOwnerID { });
    state.caches->add(CacheKind::WeakByKey, "ChainDependents"_s, *this);
}

void ChainDependents::dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl*)
{
    StructureID id = std::bit_cast<StructureID>(structureIDBits);
    if (!m_structuresWithState.contains(id))
        return;
    Structure* structure = id.decode();
    structure->setDefinitelyNonThenableState(Structure::DefinitelyNonThenableState::NotComputed);
    StructureRareData* rareData = structure->tryRareData();
    if (!rareData)
        return;
    for (unsigned index = 0; index < numberOfCachedSpecialPropertyKeys; ++index)
        rareData->clearCachedSpecialProperty(static_cast<CachedSpecialPropertyKey>(index));
}

bool ChainDependents::hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl*) const
{
    return m_structuresWithState.contains(std::bit_cast<StructureID>(structureIDBits));
}

ChainDependentOwnerID ChainDependents::addOwner(ChainDependentOwner& owner)
{
    RELEASE_ASSERT(m_owners.size() < static_cast<size_t>(ChainDependentOwnerID::NoOwner));
    m_owners.append(&owner);
    return static_cast<ChainDependentOwnerID>(m_owners.size() - 1);
}

void ChainDependents::add(std::span<JSObject* const> chainObjects, const ChainDependent& dependent)
{
    if (chainObjects.empty())
        return;
    for (JSObject* object : chainObjects) {
        ASSERT(object->mayBePrototype());
        m_objectsWithDependents.add(object);
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

void ChainDependents::addStateOfStructure(std::span<JSObject* const> chainObjects, Structure* structure, UniquedStringImpl* uid)
{
    if (chainObjects.empty())
        return;
    m_structuresWithState.add(structure->id());
    add(chainObjects, { uid, structure->id().bits(), ChainDependentOwnerID { } });
}

void ChainDependents::dropDependentsOn(JSObject* changed, UniquedStringImpl* uidOrNull)
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
    m_structuresWithState.removeIf([&](StructureID id) {
        return !vm.heap.isMarked(id.decode());
    });
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
    const LayoutData* layout = classes.classWithGuardsHeldBy(changed, side);
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
    JSGlobalObject* realm = changed->structure()->realm();
    for (auto& parts : vm.aotState().parts) {
        if (!parts.propertyCaches)
            continue;
        parts.propertyCaches->didChangeObjectOnSomeChain(changed, uidOrNull);
        // What a realm does to its Object.prototype before it has an Instance is seen when the Instance is created.
        if (realm && changed == realm->objectPrototype() && realm->aotInstance())
            parts.propertyCaches->didChangeObjectPrototype(changed, uidOrNull);
    }
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

void RuntimeHooks::didFlattenStructure(VM& vm, Structure* structure)
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

void RuntimeHooks::didStartWatchingReplacements(VM& vm)
{
    for (auto& parts : vm.aotState().parts) {
        if (parts.propertyCaches)
            parts.propertyCaches->didStartWatchingReplacements();
    }
}

void RuntimeHooks::didChangeGlobalLexicalBindings(JSGlobalObject* realm)
{
    didChangeChainObject(realm->vm(), realm, nullptr);

    for (Instance* instance = realm->aotInstance(); instance; instance = instance->above) {
        State* state = instance->state;
        if (!state || !state->globalBindings)
            continue;
        state->globalBindings->dropConstants();
        // The caller counts the epoch up next, and at this number it starts over.
        if (realm->globalLexicalBindingEpoch() + 1 == Options::thresholdForGlobalLexicalBindingEpoch())
            state->globalBindings->dropLocations();
    }
}

static State* stateOrNull(Structure* structure)
{
    JSGlobalObject* realm = structure->realm();
    Instance* instance = realm ? Instance::of(realm) : nullptr;
    return instance ? instance->state : nullptr;
}

bool RuntimeHooks::becomesDictionaryByAddition(VM& vm, JSObject* object, Structure* structure, UniquedStringImpl* uid)
{
    JSType type = structure->typeInfo().type();
    if (type != FinalObjectType && type != CompiledClosureType && type != SlimClosureWithPropertiesType)
        return false;
    if (structure->hasBeenDictionary() || isCopyOnWrite(structure->indexingMode()) || structure->isCompiledStructure() || structure->typeInfo().prohibitsPropertyCaching())
        return false;
    // The Structure of the extra properties of a compiled object is part of what the Structure of that object says.
    if (structure->inlineCapacity() == extraPropertiesInlineCapacity && structure->hasMonoProto() && structure->storedPrototype().isNull())
        return false;
    State* state = stateOrNull(structure);
    if (!state || !state->caches)
        return false;
    FirstAdditions& additions = firstAdditionsOf(*state);
    if (additions.shouldMakeStructure(structure, uid) || !hasFewEnoughNamesToBeFilled(structure) || !additions.mayBeginToFill(object))
        return false;
    additions.didBeginToFill(vm, object);
    return true;
}

void RuntimeHooks::finishFillingObjects(VM& vm)
{
    vm.aotState().hasObjectsBeingFilled = false;
    for (Instance* instance : vm.aotState().instances) {
        if (State* state = instance->state; state && state->firstAdditions)
            state->firstAdditions->finishFilling(vm);
    }
}

void RuntimeHooks::didMakeStructureForAddition(VM&, Structure* made)
{
    State* state = stateOrNull(made);
    if (state && state->firstAdditions)
        state->firstAdditions->didMakeStructure(made);
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
    state->chainDependents->addStateOfStructure(walk.changeableObjects.span(), structure, uid);
    return walk.result == ChainWalkResult::Found ? Presence::Present : Presence::Absent;
}

namespace {

enum class StepResult : uint8_t { Continue, Stop };

}

static bool canWalkThrough(Structure* structure)
{
    return !isProxyType(structure->typeInfo().type()) && !structure->needImpurePropertyWatchpoint() && !structure->isDictionary() && (structure->isCompiledStructure() || structure->propertyAccessesAreCacheable());
}

// What walkChain() answers where every object above is an ordinary one or a class prototype without members yet, and none has the name. Nothing of the walk is kept, so nothing is listed.
static bool findsNothingAbove(JSGlobalObject* globalObject, Structure* receiverStructure, UniquedStringImpl* uid, StoredName name)
{
    bool mayBeDeclared = name == StoredName::MayBeSpelledByProgram;
    if (mayBeDeclared && parseIndex(PropertyName(uid)))
        return false;
    for (Structure* below = receiverStructure; !below->typeInfo().overridesGetPrototype() && !(below->isObject() && below->hasPolyProto());) {
        JSObject* object = below->prototypeForLookup(globalObject).getObject();
        if (!object)
            return true;
        Structure* structure = object->structure();
        if (object->classInfo() == ClassPrototype::info()) {
            if (object->butterfly() || (mayBeDeclared && ClassPrototype::hasMember(object, uid)))
                return false;
        } else {
            TypeInfo typeInfo = structure->typeInfo();
            if (!canWalkThrough(structure) || structure->isCompiledStructure() || !structure->propertyAccessesAreCacheableForAbsence() || typeInfo.overridesGetOwnPropertySlot() || typeInfo.overridesPut())
                return false;
            unsigned attributes;
            std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(uid, attributes);
            if (!offset || isValidOffset(*offset) || (structure->hasNonReifiedStaticProperties() && structure->findPropertyHashEntry(uid)))
                return false;
        }
        below = structure;
    }
    return false;
}

bool storeCreatesOwnProperty(JSGlobalObject* globalObject, Structure* receiverStructure, JSObject* receiverOrNull, UniquedStringImpl* uid, StoredName name)
{
    ASSERT(name == StoredName::MayBeSpelledByProgram || (!parseIndex(PropertyName(uid)) && !globalObject->vm().aotState().identifiers->numberOf(uid)));
    if (findsNothingAbove(globalObject, receiverStructure, uid, name)) [[likely]]
        return true;
    return storeThroughChain(walkChain({ globalObject, receiverStructure, receiverOrNull, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, uid)) == StoreThroughChain::CreatesOwnProperty;
}

// A slot filed for an Unknown walk is dropped when this object changes. An object with immutable properties still reifies its static ones.
static StepResult stopAt(JSObject* object, ChainWalk& walk)
{
    walk.changeableObjects.append(object);
    return StepResult::Stop;
}

static StepResult stopAtProperty(ChainWalk& walk, JSObject* holder, JSObject* owner, PropertyOffset offset, unsigned attributes)
{
    walk.result = ChainWalkResult::Found;
    walk.holder = holder;
    walk.owner = owner;
    walk.offset = offset;
    walk.attributes = attributes;
    return StepResult::Stop;
}

static void findDeclaredMember(const ChainWalkStart& start, UniquedStringImpl* uid, JSObject* objectBelow, JSObject* classPrototype, ChainWalk& walk)
{
    if (!objectBelow || objectBelow->getPrototypeDirect() != JSValue(classPrototype))
        return;
    auto creation = start.mode == ChainWalkMode::MayAllocate ? ClassTable::MemberCreation::Allowed : ClassTable::MemberCreation::Forbidden;
    auto found = State::of(start.globalObject).objects->classes.findThroughLazyPrototypes(start.globalObject->vm(), objectBelow, uid, true, creation);
    if (!found || !found->member)
        return;
    stopAtProperty(walk, found->member->holder, found->member->holder, found->member->offset, found->member->isAccessor ? PropertyAttribute::Accessor | PropertyAttribute::DontEnum : static_cast<unsigned>(PropertyAttribute::DontEnum));
}

static StepResult passLazyClassPrototypes(const ChainWalkStart& start, UniquedStringImpl* uid, JSObject* objectBelow, JSObject*& object, ChainWalk& walk)
{
    for (; object && object->classInfo() == ClassPrototype::info(); objectBelow = object, object = object->getPrototypeDirect().getObject()) {
        walk.changeableObjects.append(object);
        if (object->butterfly())
            return StepResult::Stop;
        if (ClassPrototype::hasMember(object, uid)) {
            findDeclaredMember(start, uid, objectBelow, object, walk);
            return StepResult::Stop;
        }
    }
    return StepResult::Continue;
}

static bool hasOrdinaryLookupForName(const ChainWalkStart& start, JSObject* object, Structure* structure, UniquedStringImpl* uid)
{
    VM& vm = start.globalObject->vm();
    TypeInfo typeInfo = structure->typeInfo();
    if (!structure->propertyAccessesAreCacheableForAbsence()) {
        // mayHaveLazyProperty() reads a property of a class object, which may materialize its property table.
        return start.mode == ChainWalkMode::MayAllocate && !mayHaveLazyProperty(vm, object, uid, LookupPosition::PrototypeChain);
    }
    if (!typeInfo.overridesGetOwnPropertySlot() && !typeInfo.overridesPut())
        return true;

    // The names and classes that the engine's megamorphic caches look up in the property table alone.
    JSType type = typeInfo.type();
    bool answersOnlyForLength = type == ArrayType || type == DerivedStringObjectType || object == start.globalObject->arrayPrototype();
    if (uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype || uid == vm.propertyNames->underscoreProto) {
        // Those caches leave out what a function answers for by itself. An array and a String object look `name` and `prototype` up like any other name.
        return answersOnlyForLength && (uid == vm.propertyNames->name || uid == vm.propertyNames->prototype);
    }
    // The put of a native constructor refuses names on the constructor itself. For whoever inherits from it, it is the put of an ordinary object.
    if (type == InternalFunctionType && !typeInfo.overridesGetOwnPropertySlot() && structure->classInfoForCells()->methodTable.put == InternalFunction::info()->methodTable.put)
        return true;
    // A slim closure that has properties has all of them in its table, its length, its name and its prototype too.
    return answersOnlyForLength || type == JSFunctionType || type == SlimClosureWithPropertiesType;
}

// An offset names a word of the cell only below firstOutOfLineOffset, and a large cell has words behind that.
static PropertyOffset offsetOfBoxedValueInCell(const FieldData& field)
{
    SlotRepresentation representation = field.slot.representation();
    if (field.isOutOfLine() || (representation != SlotRepresentation::JSValue && representation != SlotRepresentation::Cell))
        return invalidOffset;
    PropertyOffset offset = Slot::offsetFor(static_cast<int32_t>(field.slot.offset / sizeof(EncodedJSValue)));
    return isInlineOffset(offset) ? offset : invalidOffset;
}

static StepResult stopAtField(Object& object, const FieldData& field, PropertyOffset offset, ChainWalk& walk)
{
    // Whoever finds an accessor takes its getter and setter from the word that the offset names.
    if (field.isAccessor() && !isValidOffset(offset))
        return stopAt(&object, walk);
    walk.changeableObjects.append(&object);
    return stopAtProperty(walk, &object, &object, offset, object.attributesOf(field));
}

// A slot that gets filled holds a property of the form that the layout gives the field.
static StepResult passEmptySlot(const ChainWalkStart& start, Object& object, const FieldData& field, ChainWalk& walk)
{
    if (field.slot.representation() == SlotRepresentation::DeferredClosure || field.flags & (fieldIsReadOnly | fieldIsAccessor))
        return stopAt(&object, walk);
    // Compiled code leaves the creation of a field to the run time, which reports it, where the object has extra properties and its layout lists its keys.
    if (!object.hasExtraProperties() || object.layout().keysAreNeverEnumerated || field.flags & fieldMayExistWithoutStorage) {
        switch (start.use) {
        case ChainWalkUse::KeptForReads:
            return stopAt(&object, walk);
        case ChainWalkUse::KeptForReadsThatAskObjects:
            if (!walk.firstObjectWithEmptySlot)
                walk.firstObjectWithEmptySlot = &object;
            break;
        case ChainWalkUse::KeptForStores:
            walk.passedSlotFilledWithoutReport = true;
            break;
        case ChainWalkUse::UsedAtOnce:
            break;
        }
    }
    walk.changeableObjects.append(&object);
    return StepResult::Continue;
}

static StepResult lookUpExtraPropertyOfCompiledObject(const ChainWalkStart& start, UniquedStringImpl* uid, Object& object, const FieldData* field, ChainWalk& walk)
{
    if (JSObject* extra = object.extraProperties()) {
        Structure* structure = extra->structure();
        unsigned attributes = 0;
        if (structure->isDictionary() && start.use != ChainWalkUse::UsedAtOnce)
            return stopAt(&object, walk);
        std::optional<PropertyOffset> offset = findOffset(start.globalObject->vm(), structure, uid, start.mode, attributes);
        if (!offset || attributes & PropertyAttribute::CustomAccessorOrValue)
            return stopAt(&object, walk);
        if (isValidOffset(*offset)) {
            walk.changeableObjects.append(&object);
            return stopAtProperty(walk, extra, &object, *offset, attributes);
        }
    }
    if (!field) {
        walk.changeableObjects.append(&object);
        return StepResult::Continue;
    }
    // Only the key order says that such a field exists where its slot holds nothing. Its value is undefined then.
    if (field->flags & fieldMayExistWithoutStorage && !object.layout().keysAreNeverEnumerated && object.isInKeyOrder(object.state(), *field))
        return stopAtField(object, *field, invalidOffset, walk);
    return passEmptySlot(start, object, *field, walk);
}

// Compiled code stores to a filled slot without a report, so the walk names the word of the slot and never its value.
static StepResult lookUpInCompiledObject(const ChainWalkStart& start, UniquedStringImpl* uid, Object& object, ChainWalk& walk)
{
    if (object.isInFrame() || (PropertyName(uid).isPrivateName() && !PropertyName(uid).passesThroughProxies()))
        return stopAt(&object, walk);
    const FieldData* field = object.fieldNamed(object.state(), uid);
    if (field && field->isPrivate())
        return stopAt(&object, walk);
    if (!field || !object.isInSlot(*field))
        return lookUpExtraPropertyOfCompiledObject(start, uid, object, field, walk);
    return stopAtField(object, *field, offsetOfBoxedValueInCell(*field), walk);
}

static StepResult lookUpInObject(const ChainWalkStart& start, UniquedStringImpl* uid, JSObject* object, ChainWalk& walk)
{
    VM& vm = start.globalObject->vm();
    Structure* structure = object->structure();
    if (!canWalkThrough(structure)) {
        // Nothing of a cacheable dictionary moves when it is made flat, and no cell is allocated: the walk goes on through it.
        bool staysInPlace = structure->isDictionary() && !structure->isUncacheableDictionary();
        if (staysInPlace)
            flattenOnce(vm, object);
        else if (structure->isDictionary() && start.mode == ChainWalkMode::MayAllocate)
            walk.didFlatten = flattenOnce(vm, object);
        if (!staysInPlace || !canWalkThrough(structure))
            return stopAt(object, walk);
    }
    if (structure->isCompiledStructure())
        return isCompiledObject(object) ? lookUpInCompiledObject(start, uid, *uncheckedDowncast<Object>(object), walk) : stopAt(object, walk);
    if (!hasOrdinaryLookupForName(start, object, structure, uid))
        return stopAt(object, walk);

    unsigned attributes = 0;
    std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(uid, attributes);
    if (!offset) {
        if (start.mode == ChainWalkMode::AllocatesNothing)
            return stopAt(object, walk);
        offset = structure->get(vm, uid, attributes);
    }
    if (isValidOffset(*offset)) {
        // An immutable property keeps its value and its place. It may still be made read-only, as Object.freeze() makes it, and from then on it stops the stores of whoever inherits it.
        bool mayStopStoresLater = start.use == ChainWalkUse::KeptForStores && !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue);
        if (!structure->isImmutablePropertyForReader(uid) || mayStopStoresLater)
            walk.changeableObjects.append(object);
        return stopAtProperty(walk, object, object, *offset, attributes);
    }
    if (structure->hasNonReifiedStaticProperties()) {
        if (auto entry = structure->findPropertyHashEntry(uid)) {
            // A read moves every other kind of entry into the object. A custom accessor or value stays in the table of its class until the object changes.
            unsigned attributes = entry->value->attributes();
            if (attributes & (PropertyAttribute::BuiltinOrFunctionOrAccessorOrLazyProperty | PropertyAttribute::ConstantInteger) || !(attributes & PropertyAttribute::CustomAccessorOrValue))
                return stopAt(object, walk);
            walk.changeableObjects.append(object);
            return stopAtProperty(walk, object, object, invalidOffset, attributesForStructure(attributes));
        }
    }
    bool canGainProperty = structure->isStructureExtensible() && !structure->isImmutablePropertyForReader(uid);
    if (canGainProperty || !structure->hasImmutablePrototype())
        walk.changeableObjects.append(object);
    return StepResult::Continue;
}

ChainWalk walkChain(const ChainWalkStart& start, UniquedStringImpl* uid)
{
    ChainWalk walk;
    if (parseIndex(PropertyName(uid)))
        return walk;

    Structure* below = start.structure;
    JSObject* objectBelow = start.receiverOrNull;
    while (!below->typeInfo().overridesGetPrototype() && !(below->isObject() && below->hasPolyProto())) {
        JSObject* object = below->prototypeForLookup(start.globalObject).getObject();
        if (passLazyClassPrototypes(start, uid, objectBelow, object, walk) == StepResult::Stop)
            break;
        // A class prototype without members answers by the tables of its own image, whichever runs.
        if (VM& vm = start.globalObject->vm(); object && vm.aotState().hasSeveralImages && belongsToOtherImageSlow(vm, object)) [[unlikely]]
            break;
        if (!object) {
            walk.result = ChainWalkResult::Absent;
            break;
        }
        if (lookUpInObject(start, uid, object, walk) == StepResult::Stop)
            break;
        below = object->structure();
        objectBelow = object;
    }
    if (walk.passedSlotFilledWithoutReport && storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
        walk.result = ChainWalkResult::Unknown;
    // The object that is asked answers with itself as the receiver, which is right for a data property and for none.
    if (walk.firstObjectWithEmptySlot && walk.result == ChainWalkResult::Found && walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        walk.result = ChainWalkResult::Unknown;
    return walk;
}

StoreThroughChain storeThroughChain(const ChainWalk& walk)
{
    if (walk.result == ChainWalkResult::Unknown)
        return StoreThroughChain::Unknown;
    if (walk.result == ChainWalkResult::Absent)
        return StoreThroughChain::CreatesOwnProperty;
    if (walk.attributes & PropertyAttribute::Accessor)
        return StoreThroughChain::CallsSetter;
    if (walk.attributes & PropertyAttribute::CustomAccessorOrValue)
        return StoreThroughChain::CallsCustomSetter;
    if (walk.attributes & PropertyAttribute::ReadOnly)
        return StoreThroughChain::IsRefused;
    return StoreThroughChain::CreatesOwnProperty;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
