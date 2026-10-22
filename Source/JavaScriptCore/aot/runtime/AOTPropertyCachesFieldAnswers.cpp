/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesAnswers.h"

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTCompiledStructure.h"
#include "AOTModuleNamespace.h"
#include "AOTModules.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTProgramStatics.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

namespace {

enum class ExtraPropertyLookupResult : uint8_t { Unknown, Absent, Found };

struct ExtraPropertyLookup {
    ExtraPropertyLookupResult result { ExtraPropertyLookupResult::Unknown };
    PropertyOffset offset { invalidOffset };
    unsigned attributes { 0 };
};

}

static PropertyAnswer uncacheableOnce(SlotAccess access)
{
    return PropertyAnswer::uncacheable(access, true);
}

static PropertyAnswer uncacheableAfter(SlotAccess access, ChainWalk&& walk)
{
    PropertyAnswer answer = PropertyAnswer::uncacheable(access, walk.didFlatten);
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static PropertyAnswer fieldAnswer(PropertyAnswerKind kind, SlotAccess access, const FieldData& field)
{
    PropertyAnswer answer;
    answer.kind = kind;
    answer.access = access;
    answer.isOwn = true;
    answer.field = &field;
    return answer;
}

static PropertyAnswer extraPropertyAnswer(PropertyAnswerKind kind, SlotAccess access, const ExtraPropertyLookup& lookup)
{
    PropertyAnswer answer;
    answer.kind = kind;
    answer.access = access;
    answer.isOwn = true;
    answer.offset = lookup.offset;
    return answer;
}

// A slot serves every object of the receiver's Structure, so only the Structure it names for the extra properties counts, not the one this object has.
static ExtraPropertyLookup lookUpExtraProperty(VM& vm, const CompiledStructure& structure, UniquedStringImpl* uid, ChainWalkMode mode)
{
    ExtraPropertyLookup lookup;
    if (!structure.hasExtraProperties()) {
        lookup.result = ExtraPropertyLookupResult::Absent;
        return lookup;
    }
    Structure* extraPropertiesStructure = structure.extraPropertiesStructure();
    if (!extraPropertiesStructure)
        return lookup;
    std::optional<PropertyOffset> offset = findOffset(vm, extraPropertiesStructure, uid, mode, lookup.attributes);
    if (!offset)
        return lookup;
    if (isValidOffset(*offset))
        lookup.result = ExtraPropertyLookupResult::Found;
    else if (structure.listsEveryExtraProperty())
        lookup.result = ExtraPropertyLookupResult::Absent;
    lookup.offset = *offset;
    return lookup;
}

static bool mayHaveExtraPropertyNamed(VM& vm, const CompiledStructure& structure, UniquedStringImpl* uid, ChainWalkMode mode)
{
    return lookUpExtraProperty(vm, structure, uid, mode).result != ExtraPropertyLookupResult::Absent;
}

static bool canCacheAccess(VM& vm, Object& object, UniquedStringImpl* uid)
{
    return !isNeverCachedName(vm, uid) && isCompiledObject(&object);
}

static PropertyAnswer classifyExtraPropertyRead(const ReadQuery& query, Object& object)
{
    VM& vm = query.globalObject->vm();
    if (query.mode == ChainWalkMode::MayAllocate && Object::ensureStructureForExtraPropertiesIfNeeded(vm, &object))
        return uncacheableOnce(SlotAccess::Read);
    ExtraPropertyLookup lookup = lookUpExtraProperty(vm, object.compiledStructure(), query.uid, query.mode);
    if (lookup.result != ExtraPropertyLookupResult::Found)
        return uncacheableOnce(SlotAccess::Read);
    if (!(lookup.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return extraPropertyAnswer(PropertyAnswerKind::ExtraProperty, SlotAccess::Read, lookup);
    if (lookup.attributes & PropertyAttribute::Accessor)
        return extraPropertyAnswer(PropertyAnswerKind::ExtraAccessor, SlotAccess::Read, lookup);
    return PropertyAnswer::uncacheable(SlotAccess::Read);
}

// What an access finds where the slot of the field is empty.
static void nameInheritedProperty(PropertyAnswer& answer, const ChainWalk& walk)
{
    if (walk.result != ChainWalkResult::Found)
        return;
    bool isAccessor = walk.attributes & PropertyAttribute::Accessor;
    if (!isValidOffset(walk.offset)) {
        if (isAccessor || answer.access != SlotAccess::Read)
            return;
        answer.kind = PropertyAnswerKind::FieldOrInheritedField;
        answer.holder = walk.holder;
        return;
    }
    answer.kind = isAccessor ? PropertyAnswerKind::FieldOrInheritedAccessor : PropertyAnswerKind::FieldOrInheritedValue;
    answer.holder = walk.holder;
    answer.offset = walk.offset;
}

// The field is undefined where it exists and its slot is empty. A read needs the key order only where the objects above the receiver answer something else.
static bool nameAnswerOfSlotThatMayBeUndefined(PropertyAnswer& answer, const ChainWalk& walk)
{
    if (walk.firstObjectWithEmptySlot)
        return false;
    switch (walk.result) {
    case ChainWalkResult::Unknown:
        return false;
    case ChainWalkResult::Absent:
        break;
    case ChainWalkResult::Found:
        if (walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue || !isValidOffset(walk.offset))
            return false;
        nameInheritedProperty(answer, walk);
        break;
    }
    answer.emptySlotMayBeUndefined = true;
    return true;
}

static bool holdsGetterSetterAsReference(const FieldData& field)
{
    switch (field.slot.representation()) {
    case SlotRepresentation::JSValue:
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
        return true;
    default:
        return false;
    }
}

static PropertyAnswer classifyFieldRead(const ReadQuery& query, Object& object, const FieldData& field)
{
    // The slot of an accessor holds its getter and setter, not what a read answers. Where it is empty the read goes on as for a data field.
    bool isAccessor = field.isAccessor();
    if (isAccessor && !holdsGetterSetterAsReference(field))
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    PropertyAnswer answer = fieldAnswer(isAccessor ? PropertyAnswerKind::FieldAccessor : PropertyAnswerKind::Field, SlotAccess::Read, field);
    if (!field.canLeaveSlot())
        return answer;
    // `in` and hasOwn take absentIsUndefined to mean that an absent slot is an absent property. Only the key order says that of such a field.
    bool mayExistWithoutStorage = field.flags & fieldMayExistWithoutStorage;

    // A property is in its slot or among the extra properties, never in both.
    ExtraPropertyLookupResult amongExtraProperties = lookUpExtraProperty(query.globalObject->vm(), object.compiledStructure(), query.uid, query.mode).result;
    if (mayExistWithoutStorage && amongExtraProperties != ExtraPropertyLookupResult::Absent)
        return answer;
    switch (amongExtraProperties) {
    case ExtraPropertyLookupResult::Found:
        return classifyExtraPropertyRead(query, object);
    case ExtraPropertyLookupResult::Unknown:
        return object.isInSlot(field) ? answer : classifyExtraPropertyRead(query, object);
    case ExtraPropertyLookupResult::Absent:
        break;
    }

    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, &object, query.mode, ChainWalkUse::KeptForReadsThatAskObjects }, query.uid);
    if (walk.didFlatten)
        return uncacheableOnce(SlotAccess::Read);
    if (mayExistWithoutStorage) {
        if (!nameAnswerOfSlotThatMayBeUndefined(answer, walk))
            return answer;
    } else if (walk.firstObjectWithEmptySlot) {
        if (walk.result != ChainWalkResult::Unknown && !isAccessor) {
            answer.kind = PropertyAnswerKind::FieldOrInheritedField;
            answer.holder = walk.firstObjectWithEmptySlot;
        }
    } else {
        answer.absentIsUndefined = walk.result == ChainWalkResult::Absent;
        // No kind of slot names both a getter in a field and what is inherited in its place.
        if (!(walk.attributes & PropertyAttribute::CustomAccessorOrValue) && !isAccessor)
            nameInheritedProperty(answer, walk);
    }
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

PropertyAnswer classifyCompiledObjectRead(const ReadQuery& query, Object& object)
{
    if (!canCacheAccess(query.globalObject->vm(), object, query.uid) || object.structure() != query.structureBefore)
        return uncacheableOnce(SlotAccess::Read);
    if (const FieldData* field = object.fieldNamed(object.state(), query.uid))
        return classifyFieldRead(query, object, *field);
    object.state().objects->layouts.didReadAbsentField(query.globalObject->vm(), object.layout(), query.uid);
    return classifyExtraPropertyRead(query, object);
}

static PropertyAnswer classifyFieldStore(const StoreQuery& query, Object& object, const FieldData& field)
{
    SlotAccess access = accessFor(query.mode);
    // What a setter did to the receiver says nothing about the store that called it.
    const CompiledStructure* before = compiledObjectStructureOrNull(query.structureBefore);
    if (!before || &before->layout() != &object.layout() || (object.structure() != before && !query.slot.isCacheableSetter()))
        return uncacheableOnce(access);
    // A store to a field that the layout gives another form than a plain property's may be refused, call a setter or move the property out of its slot.
    if (field.flags & (fieldIsStoredOnce | fieldAttributeFlags) || field.slot.representation() == SlotRepresentation::DeferredClosure)
        return PropertyAnswer::uncacheable(access);

    PropertyAnswer answer = fieldAnswer(PropertyAnswerKind::Field, access, field);
    answer.undefinedNeedsNoStorage = field.flags & fieldMayExistWithoutStorage && object.state().objects->layouts.slotAdmits(field, jsUndefined());
    if (!field.canLeaveSlot() || before->mayBePrototype())
        return answer;
    if (mayHaveExtraPropertyNamed(query.globalObject->vm(), *before, query.uid, ChainWalkMode::MayAllocate))
        return answer;
    if (access == SlotAccess::Define) {
        answer.kind = PropertyAnswerKind::FieldCreation;
        return answer;
    }
    bool hasSamePrototype = object.getPrototypeDirect() == before->storedPrototype();
    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, hasSamePrototype ? &object : nullptr, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, query.uid);
    if (walk.didFlatten)
        return uncacheableOnce(access);
    switch (storeThroughChain(walk)) {
    case StoreThroughChain::CreatesOwnProperty:
        answer.kind = PropertyAnswerKind::FieldCreation;
        break;
    case StoreThroughChain::CallsSetter:
        // Such a field may exist and hold undefined where it has no storage, and then the store belongs to it. Only the key order says so.
        if (!(field.flags & fieldMayExistWithoutStorage))
            nameInheritedProperty(answer, walk);
        break;
    case StoreThroughChain::Unknown:
    case StoreThroughChain::CallsCustomSetter:
    case StoreThroughChain::IsRefused:
        break;
    }
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static bool addsOneExtraProperty(const CompiledStructure& before, const CompiledStructure& after)
{
    if (&after.layout() != &before.layout() || after.storedPrototype() != before.storedPrototype())
        return false;
    Structure* extraPropertiesBefore = before.extraPropertiesStructure();
    Structure* extraPropertiesAfter = after.extraPropertiesStructure();
    if (!extraPropertiesAfter || (before.hasExtraProperties() && !extraPropertiesBefore))
        return false;
    Structure* expectedPrevious = extraPropertiesBefore ? extraPropertiesBefore : Instance::of(before.realm())->realm.emptyExtraPropertiesStructure;
    return extraPropertiesAfter->previousID() == expectedPrevious;
}

// Unsupported where neither the stub nor its finisher adds the property.
static OutOfLineStorageChange storageChangeOfExtraPropertyAddition(const CompiledStructure& before, const CompiledStructure& after, PropertyOffset offset)
{
    Structure* extraPropertiesAfter = after.extraPropertiesStructure();
    if (!before.hasExtraProperties()) {
        if (offset || extraPropertiesAfter->typeInfoBlob() != JSFinalObject::defaultTypeInfoBlob())
            return OutOfLineStorageChange::Unsupported;
        return OutOfLineStorageChange::Stays;
    }
    if (isInlineOffset(offset))
        return OutOfLineStorageChange::Stays;
    return outOfLineStorageChange(before.extraPropertiesStructure(), extraPropertiesAfter, offset);
}

static PropertyAnswer classifyExtraPropertyReplacement(SlotAccess access, const ExtraPropertyLookup& lookup)
{
    unsigned attributes = lookup.attributes;
    bool canReplaceValue = access == SlotAccess::Define ? !attributes : !(attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::AccessorOrCustomAccessorOrValue));
    if (canReplaceValue)
        return extraPropertyAnswer(PropertyAnswerKind::ExtraProperty, access, lookup);
    if (access == SlotAccess::Store && (attributes & PropertyAttribute::Accessor))
        return extraPropertyAnswer(PropertyAnswerKind::ExtraAccessor, access, lookup);
    return PropertyAnswer::uncacheable(access);
}

static PropertyAnswer classifyExtraPropertyAddition(const StoreQuery& query, Object& object, const ExtraPropertyLookup& lookup)
{
    SlotAccess access = accessFor(query.mode);
    const CompiledStructure* before = compiledObjectStructureOrNull(query.structureBefore);
    const CompiledStructure& after = object.compiledStructure();
    if (!before || lookup.attributes || !Object::structureGuaranteesAbsence(query.structureBefore, query.uid) || !addsOneExtraProperty(*before, after))
        return uncacheableOnce(access);
    OutOfLineStorageChange storageChange = storageChangeOfExtraPropertyAddition(*before, after, lookup.offset);
    if (before->mayBePrototype() || storageChange == OutOfLineStorageChange::Unsupported)
        return PropertyAnswer::uncacheable(access);

    PropertyAnswer answer = extraPropertyAnswer(PropertyAnswerKind::ExtraPropertyAddition, access, lookup);
    answer.newStructure = object.structure();
    if (storageChange != OutOfLineStorageChange::Stays)
        answer.newStorageCapacity = after.extraPropertiesStructure()->outOfLineCapacity();
    answer.storageHasIndexingHeader = storageChange == OutOfLineStorageChange::GrowsBesideIndexingHeader;
    if (access == SlotAccess::Define)
        return answer;
    // JavaScript on the prototype chain may have added the property, so the object's state alone proves nothing about the next store.
    ChainWalk walk = walkChain({ query.globalObject, query.structureBefore, &object, ChainWalkMode::MayAllocate, ChainWalkUse::KeptForStores }, query.uid);
    if (storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
        return uncacheableAfter(access, WTF::move(walk));
    answer.changeableObjects = WTF::move(walk.changeableObjects);
    return answer;
}

static PropertyAnswer classifyExtraPropertyStore(const StoreQuery& query, Object& object)
{
    SlotAccess access = accessFor(query.mode);
    ExtraPropertyLookup lookup = lookUpExtraProperty(query.globalObject->vm(), object.compiledStructure(), query.uid, ChainWalkMode::MayAllocate);
    if (lookup.result != ExtraPropertyLookupResult::Found)
        return uncacheableOnce(access);
    if (object.structure() == query.structureBefore)
        return classifyExtraPropertyReplacement(access, lookup);
    return classifyExtraPropertyAddition(query, object, lookup);
}

PropertyAnswer classifyCompiledObjectStore(const StoreQuery& query, Object& object)
{
    if (!canCacheAccess(query.globalObject->vm(), object, query.uid))
        return uncacheableOnce(accessFor(query.mode));
    if (const FieldData* field = object.fieldNamed(object.state(), query.uid))
        return classifyFieldStore(query, object, *field);
    return classifyExtraPropertyStore(query, object);
}

PropertyAnswer classifyNamespaceRead(const ReadQuery& query)
{
    VM& vm = query.globalObject->vm();
    auto* moduleNamespace = uncheckedDowncast<ModuleNamespace>(query.base.asCell());
    if (moduleNamespace->structure() != query.structureBefore)
        return uncacheableOnce(SlotAccess::Read);

    // Namespaces share a Structure until one is read by name, and nothing about one namespace is filed under that Structure.
    if (!moduleNamespace->hasOwnStructure()) {
        if (query.mode == ChainWalkMode::MayAllocate)
            moduleNamespace->ensureOwnStructure(vm);
        return uncacheableOnce(SlotAccess::Read);
    }
    if (vm.aotState().propertyCaches->isSharedByInstances() || moduleNamespace->isDeferred())
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    std::optional<unsigned> index = moduleNamespace->find(query.uid);
    if (!index)
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    State& state = State::of(query.structureBefore->realm());
    const ModuleExportData& moduleExport = state.modules->table().exports(moduleNamespace->module())[*index];
    if (moduleExport.kind() != ModuleExportKind::Variable)
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    // A function that nothing is stored over has no place in the statics: its slot names a row of the closure table, and a cached variable is read from the statics.
    if (moduleExport.variable.representation() == SlotRepresentation::DeferredClosure)
        return PropertyAnswer::uncacheable(SlotAccess::Read);
    if (!ProgramStatics::readVariableWithoutAllocating(state.instance, moduleExport.variable))
        return uncacheableOnce(SlotAccess::Read);

    PropertyAnswer answer;
    answer.kind = PropertyAnswerKind::Variable;
    answer.access = SlotAccess::Read;
    answer.isOwn = true;
    answer.variable = moduleExport.variable;
    return answer;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
