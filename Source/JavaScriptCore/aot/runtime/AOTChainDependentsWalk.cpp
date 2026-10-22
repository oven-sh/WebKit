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
    JSType type = typeInfo.type();
    bool answersOnlyForLength = type == ArrayType || type == DerivedStringObjectType || object == start.globalObject->arrayPrototype();
    if (!canUseMegamorphicGetByIdExcludingIndex(vm, uid)) {
        // Those caches leave out what a function answers for by itself. An array and a String object look `name` and `prototype` up like any other name.
        return answersOnlyForLength && (uid == vm.propertyNames->name || uid == vm.propertyNames->prototype);
    }
    // A slim closure that has properties has all of them in its table, its length, its name and its prototype too.
    return answersOnlyForLength || type == JSFunctionType || type == SlimClosureWithPropertiesType;
}

static bool holdsBoxedValueInCell(const FieldData& field)
{
    if (field.isOutOfLine())
        return false;
    SlotRepresentation representation = field.slot.representation();
    return representation == SlotRepresentation::JSValue || representation == SlotRepresentation::Cell;
}

// Only the key order says that such a field exists where its slot holds nothing. Its value is undefined then.
static bool existsWithoutStorage(const Object& object, const FieldData& field)
{
    return field.flags & fieldMayExistWithoutStorage && !object.layout().keysAreNeverEnumerated && object.isInKeyOrder(object.state(), field);
}

static StepResult stopAtField(Object& object, const FieldData& field, PropertyOffset offset, ChainWalk& walk)
{
    // Whoever finds an accessor takes its getter and setter from the word that the offset names.
    if (field.isAccessor() && !isValidOffset(offset))
        return stopAt(&object, walk);
    walk.changeableObjects.append(&object);
    walk.result = ChainWalkResult::Found;
    walk.holder = &object;
    walk.owner = &object;
    walk.offset = offset;
    walk.attributes = object.attributesOf(field);
    return StepResult::Stop;
}

// Compiled code leaves the creation of a field to the run time, which reports it, where the object has extra properties and its layout lists its keys.
static bool reportsCreationOf(const Object& object, const FieldData& field)
{
    return object.hasExtraProperties() && !object.layout().keysAreNeverEnumerated && !(field.flags & fieldMayExistWithoutStorage);
}

// A slot that gets filled holds a property of the form that the layout gives the field. Where that is a writable data property, a store below it creates a property of the receiver,
// as it does where the name is absent, and what a read of it answers does not depend on the receiver.
static StepResult passEmptySlot(const ChainWalkStart& start, Object& object, const FieldData& field, ChainWalk& walk)
{
    if (field.slot.representation() == SlotRepresentation::DeferredClosure || field.flags & (fieldIsReadOnly | fieldIsAccessor))
        return stopAt(&object, walk);
    if (!reportsCreationOf(object, field)) {
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
            walk.result = ChainWalkResult::Found;
            walk.holder = extra;
            walk.owner = &object;
            walk.offset = *offset;
            walk.attributes = attributes;
            return StepResult::Stop;
        }
    }
    if (!field) {
        walk.changeableObjects.append(&object);
        return StepResult::Continue;
    }
    if (existsWithoutStorage(object, *field))
        return stopAtField(object, *field, invalidOffset, walk);
    return passEmptySlot(start, object, *field, walk);
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
    PropertyOffset offset = holdsBoxedValueInCell(*field) ? Slot::offsetFor(static_cast<int32_t>(field->slot.offset / sizeof(EncodedJSValue))) : invalidOffset;
    return stopAtField(object, *field, offset, walk);
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
        // An immutable property keeps its value and its place. It may still be made read-only, as Object.freeze() makes it, and from then on it stops the stores of whoever inherits it.
        bool mayStopStoresLater = start.use == ChainWalkUse::KeptForStores && !(attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue);
        if (!structure->isImmutablePropertyForReader(uid) || mayStopStoresLater)
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
    bool canGainProperty = structure->isStructureExtensible() && !structure->isImmutablePropertyForReader(uid);
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
    // The object that is asked answers with itself as the receiver, which is right for a data property and for none.
    if (walk.firstObjectWithEmptySlot && walk.result == ChainWalkResult::Found && walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
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
