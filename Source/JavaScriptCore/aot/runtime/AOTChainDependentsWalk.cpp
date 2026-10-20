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
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTState.h"
#include "ArrayPrototype.h"
#include "InlineCacheCompiler.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

namespace {

enum class StepResult : uint8_t { Continue, Stop };

}

static bool isLazyClassPrototype(JSObject* object)
{
    return object->classInfo() == ClassPrototype::info();
}

static bool canWalkThrough(Structure* structure)
{
    return !isProxyType(structure->typeInfo().type()) && !structure->needImpurePropertyWatchpoint() && !structure->isDictionary() && (structure->isCompiledStructure() || structure->propertyAccessesAreCacheable());
}

// A slot filed for an Unknown walk is dropped when this object changes. An object with immutable properties still reifies its static ones.
static StepResult stopAt(JSObject* object, ChainWalk& walk)
{
    walk.changeableObjects.append(object);
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
    walk.result = ChainWalkResult::Found;
    walk.holder = found->member->holder;
    walk.owner = found->member->holder;
    walk.offset = found->member->offset;
    walk.attributes = found->member->isAccessor ? PropertyAttribute::Accessor | PropertyAttribute::DontEnum : static_cast<unsigned>(PropertyAttribute::DontEnum);
}

static StepResult passLazyClassPrototypes(const ChainWalkStart& start, UniquedStringImpl* uid, JSObject* objectBelow, JSObject*& object, ChainWalk& walk)
{
    Instance* instance = Instance::of(start.globalObject);
    for (; object && isLazyClassPrototype(object); objectBelow = object, object = object->getPrototypeDirect().getObject()) {
        walk.changeableObjects.append(object);
        // ClassPrototype::createMembers() reports to the ChainDependents of its own Instance only.
        if (Instance::of(object->structure()->realm()) != instance || object->butterfly())
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
    if (!canUseMegamorphicGetByIdExcludingIndex(vm, uid))
        return false;
    JSType type = typeInfo.type();
    return type == ArrayType || type == JSFunctionType || type == DerivedStringObjectType || object == start.globalObject->arrayPrototype();
}

static bool holdsBoxedValueInCell(const FieldData& field)
{
    if (field.isOutOfLine())
        return false;
    SlotRepresentation representation = field.slot.representation();
    return representation == SlotRepresentation::JSValue || representation == SlotRepresentation::Cell;
}

// Compiled code leaves the creation of a field to the run time, which reports it, where the object has extra properties and its layout lists its keys.
static bool reportsCreationOf(const Object& object, const FieldData& field)
{
    return object.hasExtraProperties() && !object.layout().keysAreNeverEnumerated && !(field.flags & fieldMayExistWithoutStorage);
}

// A slot that gets filled holds a writable data property, and a store below one creates a property of the receiver, as it does where the name is absent.
static StepResult passEmptySlot(const ChainWalkStart& start, Object& object, const FieldData& field, ChainWalk& walk)
{
    if (field.slot.representation() == SlotRepresentation::DeferredClosure)
        return stopAt(&object, walk);
    if (start.use != ChainWalkUse::UsedAtOnce && !reportsCreationOf(object, field)) {
        if (start.use == ChainWalkUse::KeptForReads)
            return stopAt(&object, walk);
        walk.passedSlotFilledWithoutReport = true;
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
        if (!offset)
            return stopAt(&object, walk);
        if (isValidOffset(*offset)) {
            walk.changeableObjects.append(&object);
            walk.result = ChainWalkResult::Found;
            walk.holder = extra;
            walk.owner = &object;
            walk.offset = *offset;
            walk.attributes = attributes;
            return StepResult::Stop;
        }
    }
    if (field)
        return passEmptySlot(start, object, *field, walk);
    walk.changeableObjects.append(&object);
    return StepResult::Continue;
}

// Compiled code stores to a filled slot without a report, so the walk names the word of the slot and never its value. Only the run time empties a slot, moves its property
// elsewhere, changes its attributes or changes the extra properties, and it reports each of these for an object that may be a prototype.
static StepResult lookUpInCompiledObject(const ChainWalkStart& start, UniquedStringImpl* uid, Object& object, ChainWalk& walk)
{
    if (object.isInFrame() || PropertyName(uid).isPrivateName())
        return stopAt(&object, walk);
    const FieldData* field = object.fieldNamed(object.state(), uid);
    if (field && field->isPrivate())
        return stopAt(&object, walk);
    if (!field || !object.isInSlot(*field))
        return lookUpExtraPropertyOfCompiledObject(start, uid, object, field, walk);
    walk.changeableObjects.append(&object);
    walk.result = ChainWalkResult::Found;
    walk.holder = &object;
    walk.owner = &object;
    walk.attributes = object.attributesOf(*field);
    if (holdsBoxedValueInCell(*field))
        walk.offset = Slot::offsetFor(static_cast<int32_t>(field->slot.offset / sizeof(EncodedJSValue)));
    return StepResult::Stop;
}

static StepResult lookUpInObject(const ChainWalkStart& start, UniquedStringImpl* uid, JSObject* object, ChainWalk& walk)
{
    VM& vm = start.globalObject->vm();
    Structure* structure = object->structure();
    if (!canWalkThrough(structure)) {
        if (structure->isDictionary() && start.mode == ChainWalkMode::MayAllocate)
            walk.didFlatten = flattenOnce(vm, object);
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
        if (!structure->isImmutableProperty(uid))
            walk.changeableObjects.append(object);
        walk.result = ChainWalkResult::Found;
        walk.holder = object;
        walk.owner = object;
        walk.offset = *offset;
        walk.attributes = attributes;
        return StepResult::Stop;
    }
    if (structure->hasNonReifiedStaticProperties() && structure->findPropertyHashEntry(uid))
        return stopAt(object, walk);
    bool canGainProperty = structure->isStructureExtensible() && !structure->isImmutableProperty(uid);
    if (canGainProperty || !structure->hasImmutablePrototype())
        walk.changeableObjects.append(object);
    return StepResult::Continue;
}

static void walkObjectsAbove(const ChainWalkStart& start, UniquedStringImpl* uid, ChainWalk& walk)
{
    if (parseIndex(PropertyName(uid)))
        return;

    Structure* below = start.structure;
    JSObject* objectBelow = start.receiverOrNull;
    while (true) {
        if (below->typeInfo().overridesGetPrototype() || (below->isObject() && below->hasPolyProto()))
            return;
        JSObject* object = below->prototypeForLookup(start.globalObject).getObject();
        if (passLazyClassPrototypes(start, uid, objectBelow, object, walk) == StepResult::Stop)
            return;
        if (!object) {
            walk.result = ChainWalkResult::Absent;
            return;
        }
        if (lookUpInObject(start, uid, object, walk) == StepResult::Stop)
            return;
        below = object->structure();
        objectBelow = object;
    }
}

ChainWalk walkChain(const ChainWalkStart& start, UniquedStringImpl* uid)
{
    ChainWalk walk;
    walkObjectsAbove(start, uid, walk);
    if (walk.passedSlotFilledWithoutReport && storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
        walk.result = ChainWalkResult::Unknown;
    return walk;
}

StoreThroughChain storeThroughChain(const ChainWalk& walk)
{
    switch (walk.result) {
    case ChainWalkResult::Unknown:
        return StoreThroughChain::Unknown;
    case ChainWalkResult::Absent:
        return StoreThroughChain::CreatesOwnProperty;
    case ChainWalkResult::Found:
        break;
    }
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
