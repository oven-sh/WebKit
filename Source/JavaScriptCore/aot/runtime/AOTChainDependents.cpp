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
#include "AOTGenerator.h"
#include "AOTGlobalBindings.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCaches.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTRuntimeHooks.h"
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

JSGlobalObject* ChainDependents::realm() const
{
    return m_state.instance.globalObject;
}

ChainDependentOwnerID ChainDependents::addOwner(ChainDependentOwner& owner)
{
    RELEASE_ASSERT(m_owners.size() < static_cast<size_t>(ChainDependentOwnerID::NoOwner));
    m_owners.append(&owner);
    return static_cast<ChainDependentOwnerID>(m_owners.size() - 1);
}

size_t ChainDependents::ObjectDependents::size() const
{
    size_t size = onAnyName.size();
    for (const DependentSet& set : byName.values())
        size += set.size();
    return size;
}

void ChainDependents::ObjectDependents::forEach(const Invocable<void(const ChainDependent&)> auto& functor) const
{
    for (const ChainDependent& dependent : onAnyName)
        functor(dependent);
    for (const DependentSet& set : byName.values()) {
        for (const ChainDependent& dependent : set)
            functor(dependent);
    }
}

void ChainDependents::ObjectDependents::removeIf(const Invocable<bool(const ChainDependent&)> auto& functor)
{
    onAnyName.removeIf(functor);
    byName.removeIf([&](auto& entry) {
        entry.value.removeIf(functor);
        return entry.value.isEmpty();
    });
}

void startReportingReplacements(VM& vm, JSObject* object)
{
    Object* compiled = compiledObjectOrNull(object);
    if (!compiled || compiled->compiledStructure().reportsReplacements())
        return;
    compiled->JSCell::setStructure(vm, compiled->state().objects->structures.withReportedReplacements(vm, compiled->compiledStructure()));
}

static bool holdsExtraProperty(VM& vm, JSObject* object, UniquedStringImpl* uidOrNull)
{
    Object* compiled = compiledObjectOrNull(object);
    JSObject* extra = compiled ? extraPropertiesIfCreated(*compiled) : nullptr;
    return extra && (!uidOrNull || isValidOffset(extra->structure()->get(vm, uidOrNull)));
}

void ChainDependents::add(std::span<JSObject* const> chainObjects, const ChainDependent& dependent)
{
    if (chainObjects.empty())
        return;
    VM& vm = *m_state.instance.vm;
    const bool readsValuesWhereTheyAre = ownerOf(dependent).readsValuesWhereTheyAre();
    for (JSObject* object : chainObjects) {
        ASSERT(object->mayBePrototype());
        if (!readsValuesWhereTheyAre)
            m_state.instance.propertyCaches.forgetReplacedMembersOf(object);
        if (holdsExtraProperty(vm, object, dependent.uidOrNull))
            startReportingReplacements(vm, object);
        m_objectsWithDependents.add(object);
        ObjectDependents& dependents = m_dependentsByObject.ensure(object, [] {
            return ObjectDependents();
        }).iterator->value;
        DependentSet& set = dependent.uidOrNull ? dependents.byName.ensure(dependent.uidOrNull, [] {
            return DependentSet();
        }).iterator->value : dependents.onAnyName;
        if (!set.add(dependent).isNewEntry)
            continue;
        ++m_numberOfDependentsAddedSinceOwnersWereAsked;
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
    };

    if (!uidOrNull) {
        ObjectDependents dependents = m_dependentsByObject.take(iterator);
        dependents.forEach(drop);
        return;
    }

    ObjectDependents& all = iterator->value;
    DependentSet onName = all.byName.take(uidOrNull);
    DependentSet onAnyName = std::exchange(all.onAnyName, { });
    if (all.isEmpty())
        m_dependentsByObject.remove(iterator);
    for (const ChainDependent& dependent : onName)
        drop(dependent);
    for (const ChainDependent& dependent : onAnyName)
        drop(dependent);
}

void ChainDependents::didCreateMembers(const CreatedMembers& created)
{
    if (!m_objectsWithDependents.mayContain(created.prototype))
        return;
    auto iterator = m_dependentsByObject.find(created.prototype);
    if (iterator == m_dependentsByObject.end())
        return;
    VM& vm = *m_state.instance.vm;
    DeferGC deferGC(vm);
    ObjectDependents& all = iterator->value;
    Vector<ChainDependent, 8> dropped;
    all.removeIf([&](const ChainDependent& dependent) {
        if (ownerOf(dependent).keepChainDependent(vm, dependent.subject, dependent.uidOrNull, created))
            return false;
        dropped.append(dependent);
        return true;
    });
    if (all.isEmpty())
        m_dependentsByObject.remove(iterator);
    for (const ChainDependent& dependent : dropped)
        ownerOf(dependent).dropChainDependent(vm, dependent.subject, dependent.uidOrNull);
}

size_t ChainDependents::numberOfDependents(JSObject* object) const
{
    auto iterator = m_dependentsByObject.find(object);
    return iterator == m_dependentsByObject.end() ? 0 : iterator->value.size();
}

size_t ChainDependents::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t numberOfDependentsOfDeadObjects = 0;
    bool dropsEvictedDependents = scope == CollectionScope::Full && m_numberOfDependentsAddedSinceOwnersWereAsked >= m_numberOfDependentsLeftWhenOwnersWereAsked;
    size_t numberOfDependentsLeft = 0;
    m_structuresWithState.removeIf([&](StructureID id) {
        return !vm.heap.isMarked(id.decode());
    });
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        m_dependentsByObject.removeIf([&](auto& entry) {
            if (!vm.heap.isMarked(entry.key)) {
                numberOfDependentsOfDeadObjects += entry.value.size();
                return true;
            }
            if (!dropsEvictedDependents)
                return false;
            entry.value.removeIf([&](const ChainDependent& dependent) {
                return !ownerOf(dependent).hasChainDependent(vm, dependent.subject, dependent.uidOrNull);
            });
            numberOfDependentsLeft += entry.value.size();
            return entry.value.isEmpty();
        });
    });
    if (dropsEvictedDependents) {
        m_numberOfDependentsAddedSinceOwnersWereAsked = 0;
        m_numberOfDependentsLeftWhenOwnersWereAsked = numberOfDependentsLeft;
    }
    return numberOfDependentsOfDeadObjects;
}

static const LayoutData* classWhoseHolderChanged(const ClassTable& classes, JSObject* changed, ClassSide& side)
{
    if (const LayoutData* layout = classes.classHeldBy(changed, side))
        return layout;
    if (changed->classInfo() != ClassPrototype::info())
        return nullptr;
    const LayoutData* layout = classes.classOf(*uncheckedDowncast<ClassPrototype>(changed));
    if (!layout || classes.classPrototypeIfExists(*layout) != changed)
        return nullptr;
    side = ClassSide::Prototype;
    return layout;
}

static bool seesAccessesUnderName(VM& vm, JSObject* holder, UniquedStringImpl* uid)
{
    unsigned attributes = 0;
    Object* compiled = compiledObjectOrNull(holder);
    JSValue held = compiled ? compiled->getDirectFromNativeCode(vm, uid, &attributes) : holder->getDirect(vm, uid, attributes);
    return held && ClassTable::seesAccesses(vm, uid, attributes);
}

static void didChangeClassHolder(VM& vm, State& state, JSObject* changed, UniquedStringImpl* uidOrNull)
{
    ClassTable& classes = state.objects->classes;
    ClassSide side;
    const LayoutData* layout = classWhoseHolderChanged(classes, changed, side);
    if (!layout)
        return;
    classes.reportReplacementsOnHolder(vm, *layout, side, changed, uidOrNull);
    if (uidOrNull) {
        classes.holdMemberToProof(vm, *layout, side, changed, uidOrNull);
        if (!classes.hasNoGuardLeft(*layout, side) && seesAccessesUnderName(vm, changed, uidOrNull) && !classes.holdsMemberOfRecordUnder(vm, *layout, side, changed, uidOrNull))
            classes.didLayWhatSeesAccesses(vm, *layout, side);
        classes.didChangeMember(*layout, side, uidOrNull);
        classes.noteReplacedMember(vm, *layout, side, changed, uidOrNull);
        return;
    }
    classes.holdLinkToProof(vm, *layout, side, changed);
    if (changed->isFrozen(vm)) {
        classes.didLayWhatSeesAccesses(vm, *layout, side);
        classes.didFreezeHolder(vm, *layout, side, changed);
        return;
    }
    classes.didChangeEveryMember(*layout, side);
    classes.didChangePrototypeLink(*layout, side);
}

enum class ChangeOfChainObject : uint8_t { IsOfAnyKind, ReplacesValue };

static void tellOfChangeOfChainObject(VM& vm, JSObject* changed, UniquedStringImpl* uidOrNull, ChangeOfChainObject change)
{
    if (VMPropertyCaches* caches = vm.aotState().propertyCaches.get()) {
        if (change == ChangeOfChainObject::ReplacesValue)
            caches->didReplaceValueOnObjectOfSomeChain(changed, uidOrNull);
        else
            caches->didChangeObjectOnSomeChain(changed, uidOrNull);
        JSGlobalObject* realm = changed->structure()->realm();
        if (realm && changed == realm->objectPrototype() && realm->aotInstance())
            caches->didChangeObjectPrototype(changed, uidOrNull);
    }
    for (Instance* instance : vm.aotState().instances) {
        State* state = instance->state;
        if (!state)
            continue;
        instance->propertyCaches.forgetReplacedMembersOf(changed);
        if (!uidOrNull)
            instance->propertyCaches.forgetPrototypeChainFilters();
        if (state->chainDependents)
            state->chainDependents->didChange(changed, uidOrNull);
        if (state->objects)
            didChangeClassHolder(vm, *state, changed, uidOrNull);
    }
}

void didChangeChainObject(VM& vm, JSObject* changed, UniquedStringImpl* uidOrNull)
{
    if (changed && changed == vm.aotState().unobservedClassHolder)
        return;
    tellOfChangeOfChainObject(vm, changed, uidOrNull, ChangeOfChainObject::IsOfAnyKind);
}

void didReplaceValueOnChainObject(VM& vm, JSObject* changed, UniquedStringImpl* uid)
{
    tellOfChangeOfChainObject(vm, changed, uid, ChangeOfChainObject::ReplacesValue);
}

void RuntimeHooks::didStartWatchingReplacements(VM& vm)
{
    if (VMPropertyCaches* caches = vm.aotState().propertyCaches.get())
        caches->didStartWatchingReplacements();
    for (Instance* instance : vm.aotState().instances) {
        State* state = instance->state;
        instance->propertyCaches.forgetReplacedMembers();
        if (state && state->objects)
            state->objects->classes.forgetStructuresOfHeirs();
    }
}

void RuntimeHooks::didChangeGlobalLexicalBindings(JSGlobalObject* realm)
{
    didChangeChainObject(realm->vm(), realm, nullptr);

    Instance* instance = Instance::of(realm);
    State* state = instance ? instance->state : nullptr;
    if (!state || !state->globalBindings)
        return;
    state->globalBindings->dropConstants();
    if (realm->globalLexicalBindingEpoch() + 1 == Options::thresholdForGlobalLexicalBindingEpoch())
        state->globalBindings->dropLocations();
}

static State* stateOrNull(Structure* structure)
{
    JSGlobalObject* realm = structure->realm();
    Instance* instance = realm ? Instance::of(realm) : nullptr;
    return instance ? instance->state : nullptr;
}

bool RuntimeHooks::shouldUseDictionaryForAddition(VM& vm, JSObject* object, Structure* structure, UniquedStringImpl* uid)
{
    JSType type = structure->typeInfo().type();
    if (type != FinalObjectType && type != CompiledClosureType && type != SlimClosureWithPropertiesType)
        return false;
    if (structure->hasBeenDictionary() || isCopyOnWrite(structure->indexingMode()) || structure->isCompiledStructure() || structure->typeInfo().prohibitsPropertyCaching())
        return false;
    if (structure->transitionKind() == TransitionKind::PropertyDeletion)
        return false;
    if (structure->inlineCapacity() == extraPropertiesInlineCapacity && structure->hasMonoProto() && structure->storedPrototype().isNull())
        return false;
    State* state = stateOrNull(structure);
    if (!state || !state->caches)
        return false;
    FirstAdditions& additions = firstAdditionsOf(*state);
    if (additions.shouldMakeStructure(structure, uid) || !hasFewEnoughNamesToBeFilled(structure) || !additions.mayBeginToFill(object))
        return false;
    additions.didBeginToFill(vm, object, structure);
    return true;
}

void RuntimeHooks::finishFillingObjects(VM& vm)
{
    vm.aotState().hasObjectsBeingFilled = false;
    DeferGC deferGC(vm);
    for (Instance* instance : vm.aotState().instances) {
        if (State* state = instance->state; state && state->firstAdditions)
            state->firstAdditions->finishFilling(vm);
    }
}

void RuntimeHooks::didCreateAdditionTransition(VM&, Structure* made)
{
    State* state = stateOrNull(made);
    if (state && state->firstAdditions)
        state->firstAdditions->didMakeStructure(made);
}

bool storeCreatesOwnProperty(JSGlobalObject* globalObject, Structure* receiverStructure, JSObject* receiverOrNull, UniquedStringImpl* uid, ChainDependents& ofOwner, const ChainDependent& dependent)
{
    ChainWalk walk = walkChain({ globalObject, receiverStructure, receiverOrNull, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, uid);
    if (storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
        return false;
    if (!walk.dependsOnChain())
        return true;

    if (receiverStructure->realm() != globalObject || ofOwner.realm() != globalObject)
        return false;
    ofOwner.add(walk.changeableObjects.span(), dependent);
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

static bool lazyPrototypeDeclaresMember(Structure* structure, UniquedStringImpl* uid)
{
    if (!structure->hasMonoProto())
        return false;
    for (JSObject* object = structure->storedPrototypeObject(); object && object->classInfo() == ClassPrototype::info() && !object->butterfly(); object = object->getPrototypeDirect().getObject()) {
        if (ClassPrototype::hasMember(object, uid))
            return true;
    }
    return false;
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
        return lazyPrototypeDeclaresMember(structure, uid) ? Presence::Present : Presence::Unknown;
    state->chainDependents->addStateOfStructure(walk.changeableObjects.span(), structure, uid);
    return walk.result == ChainWalkResult::Found ? Presence::Present : Presence::Absent;
}

namespace {

enum class StepResult : uint8_t { Continue, Stop };

}

static bool canWalkThrough(Structure* structure)
{
    if (isProxyType(structure->typeInfo().type()))
        return false;
    return structure->isCompiledStructure() || (structure->prototypeQueriesAreCacheable() && !structure->typeInfo().getOwnPropertySlotIsImpure());
}

static bool findsNothingAbove(JSGlobalObject* globalObject, Structure* receiverStructure, UniquedStringImpl* uid, StoredName name)
{
    bool mayBeDeclared = name == StoredName::MayBeSpelledByProgram;
    if (mayBeDeclared && parseIndex(PropertyName(uid)))
        return false;
    for (Structure* below = receiverStructure; !hidesObjectsAbove(below) && !(below->isObject() && below->hasPolyProto());) {
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
    ASSERT(name == StoredName::MayBeSpelledByProgram || (!parseIndex(PropertyName(uid)) && !identifiersOfRealm(globalObject->vm(), globalObject)->numberOf(uid)));
    if (findsNothingAbove(globalObject, receiverStructure, uid, name)) [[likely]]
        return true;
    return storeThroughChain(walkChain({ globalObject, receiverStructure, receiverOrNull, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, uid)) == StoreThroughChain::CreatesOwnProperty;
}

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

static bool findMemberDeclaredByLazyPrototype(const ChainWalkStart& start, UniquedStringImpl* uid, JSObject* classPrototype, ChainWalk& walk)
{
    auto creation = start.mode == ChainWalkMode::MayAllocate ? ClassTable::MemberCreation::Allowed : ClassTable::MemberCreation::Forbidden;
    auto found = Instance::owning(classPrototype)->state->objects->classes.findFromLazyPrototype(start.globalObject->vm(), classPrototype, uid, creation);
    if (!found || !found->member)
        return false;
    stopAtProperty(walk, found->member->holder, found->member->holder, found->member->offset, found->member->isAccessor ? PropertyAttribute::Accessor | PropertyAttribute::DontEnum : static_cast<unsigned>(PropertyAttribute::DontEnum));
    return true;
}

static StepResult passLazyClassPrototypes(const ChainWalkStart& start, UniquedStringImpl* uid, JSObject*& object, ChainWalk& walk)
{
    for (; object && object->classInfo() == ClassPrototype::info(); object = object->getPrototypeDirect().getObject()) {
        if (object->butterfly())
            return stopAt(object, walk);
        if (!ClassPrototype::hasMember(object, uid)) {
            walk.changeableObjects.append(object);
            continue;
        }
        if (findMemberDeclaredByLazyPrototype(start, uid, object, walk) || start.mode == ChainWalkMode::AllocatesNothing)
            return stopAt(object, walk);
        ClassPrototype::ensureMembers(object);
        break;
    }
    return StepResult::Continue;
}

static bool isCompleteClassObject(VM& vm, JSObject* object, Structure* structure)
{
    auto* closure = uncheckedDowncast<Closure>(object);
    if (!closure->classData())
        return true;
    unsigned attributes = 0;
    std::optional<PropertyOffset> prototype = structure->getWithoutMaterializing(vm.propertyNames->prototype.impl(), attributes);
    return prototype && isValidOffset(*prototype) && !closure->hasStaticFieldsLeft();
}

static bool hasOrdinaryLookupForName(const ChainWalkStart& start, JSObject* object, Structure* structure, UniquedStringImpl* uid)
{
    VM& vm = start.globalObject->vm();
    TypeInfo typeInfo = structure->typeInfo();
    if (start.mode == ChainWalkMode::AllocatesNothing && typeInfo.type() == CompiledClosureType) {
        bool isNameOfEveryFunction = uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype || uid == vm.propertyNames->underscoreProto;
        return !isNameOfEveryFunction && isCompleteClassObject(vm, object, structure);
    }
    if (!structure->propertyAccessesAreCacheableForAbsence()) {
        return start.mode == ChainWalkMode::MayAllocate && !mayHaveLazyProperty(vm, object, uid, LookupPosition::PrototypeChain);
    }
    if (!typeInfo.overridesGetOwnPropertySlot() && !typeInfo.overridesPut())
        return true;

    JSType type = typeInfo.type();
    bool answersOnlyForLength = type == ArrayType || type == DerivedStringObjectType || object == start.globalObject->arrayPrototype();
    if (uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype || uid == vm.propertyNames->underscoreProto) {
        return answersOnlyForLength && (uid == vm.propertyNames->name || uid == vm.propertyNames->prototype);
    }
    if (type == InternalFunctionType && !typeInfo.overridesGetOwnPropertySlot() && structure->classInfoForCells()->methodTable.put == InternalFunction::info()->methodTable.put)
        return true;
    return answersOnlyForLength || type == JSFunctionType || type == SlimClosureWithPropertiesType;
}

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
    if (field.isAccessor() && !isValidOffset(offset))
        return stopAt(&object, walk);
    walk.changeableObjects.append(&object);
    return stopAtProperty(walk, &object, &object, offset, object.attributesOf(field));
}

static StepResult passEmptySlot(const ChainWalkStart& start, Object& object, const FieldData& field, ChainWalk& walk)
{
    if (field.slot.representation() == SlotRepresentation::DeferredClosure || field.flags & (fieldIsReadOnly | fieldIsAccessor))
        return stopAt(&object, walk);
    if (!object.hasExtraProperties() || field.flags & fieldMayExistWithoutStorage) {
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
    walk.passedEmptySlot = true;
    walk.changeableObjects.append(&object);
    return StepResult::Continue;
}

static StepResult lookUpExtraPropertyOfCompiledObject(const ChainWalkStart& start, UniquedStringImpl* uid, Object& object, const FieldData* field, ChainWalk& walk)
{
    if (JSObject* extra = object.extraProperties()) {
        Structure* structure = extra->structure();
        unsigned attributes = 0;
        if (structure->isUncacheableDictionary() && start.use != ChainWalkUse::UsedAtOnce)
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
    if (field->flags & fieldMayExistWithoutStorage && object.isInKeyOrder(object.state(), *field))
        return stopAtField(object, *field, invalidOffset, walk);
    return passEmptySlot(start, object, *field, walk);
}

static StepResult lookUpInCompiledObject(const ChainWalkStart& start, UniquedStringImpl* uid, Object& object, ChainWalk& walk)
{
    if (object.isInFrame())
        return StepResult::Stop;
    if (PropertyName(uid).isPrivateName() && !PropertyName(uid).passesThroughProxies())
        return stopAt(&object, walk);
    const FieldData* field = object.fieldNamed(object.state(), uid);
    if (field && field->isPrivate())
        return stopAt(&object, walk);
    if (!field || !object.isInSlot(*field))
        return lookUpExtraPropertyOfCompiledObject(start, uid, object, field, walk);
    return stopAtField(object, *field, offsetOfBoxedValueInCell(*field), walk);
}

bool isArrayPrototypeWithWritableLength(const JSGlobalObject* globalObject, const JSObject* object)
{
    Structure* structure = object->structure();
    return object == globalObject->arrayPrototype() && !hasIndexedProperties(structure->indexingType()) && !structure->hasPolyProto() && !structure->hasNonReifiedStaticProperties();
}

static StepResult lookUpInObject(const ChainWalkStart& start, UniquedStringImpl* uid, JSObject* object, ChainWalk& walk)
{
    VM& vm = start.globalObject->vm();
    Structure* structure = object->structure();
    if (!canWalkThrough(structure))
        return stopAt(object, walk);
    if (structure->isCompiledStructure())
        return isCompiledObject(object) ? lookUpInCompiledObject(start, uid, *uncheckedDowncast<Object>(object), walk) : stopAt(object, walk);
    if (start.use == ChainWalkUse::KeptForStores && uid == vm.propertyNames->length && isArrayPrototypeWithWritableLength(start.globalObject, object)) {
        walk.changeableObjects.append(object);
        return stopAtProperty(walk, object, object, invalidOffset, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    }
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
        bool mayStopStoresLater = start.use == ChainWalkUse::KeptForStores && !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue);
        if (!structure->isImmutablePropertyForReader(uid) || mayStopStoresLater)
            walk.changeableObjects.append(object);
        return stopAtProperty(walk, object, object, *offset, attributes);
    }
    if (structure->hasNonReifiedStaticProperties()) {
        if (auto entry = structure->findPropertyHashEntry(uid)) {
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
    while (!hidesObjectsAbove(below) && !(below->isObject() && below->hasPolyProto())) {
        JSObject* object = below->prototypeForLookup(start.globalObject).getObject();
        if (passLazyClassPrototypes(start, uid, object, walk) == StepResult::Stop)
            break;
        if (!object) {
            walk.result = ChainWalkResult::Absent;
            break;
        }
        if (lookUpInObject(start, uid, object, walk) == StepResult::Stop)
            break;
        below = object->structure();
    }
    if (walk.passedSlotFilledWithoutReport && storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
        walk.result = ChainWalkResult::Unknown;
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
