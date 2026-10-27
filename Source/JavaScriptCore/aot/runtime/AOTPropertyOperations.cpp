/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTErrorMessages.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeHooks.h"
#include "AOTSchemaEnums.h"
#include "AOTUnwinding.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "ExceptionHelpers.h"
#include "GetterSetter.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <array>
#include <bit>
#include <cmath>

namespace JSC { namespace AOT {

JSValue getWithoutCaches(Instance* instance, JSValue base, const Identifier& identifier, JSValue receiver)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto found = base.isObject() ? instance->state->objects->classes.findThroughLazyPrototypes(vm, asObject(base), identifier.impl(), false) : std::nullopt;
    PropertySlot slot(receiver, PropertySlot::InternalMethodType::Get);
    if (!found)
        RELEASE_AND_RETURN(scope, base.get(globalObject, identifier, slot));
    if (found->member) {
        JSValue member = found->member->holder->getDirect(found->member->offset);
        if (!found->member->isAccessor)
            return member;
        RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(member.asCell())->callGetter(globalObject, receiver));
    }
    if (!found->firstObjectAbove)
        return jsUndefined();
    RELEASE_AND_RETURN(scope, JSValue(found->firstObjectAbove).get(globalObject, identifier, slot));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetById, EncodedJSValue, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetById);
    JSValue value = JSValue::decode(base);
    if (value.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, value, identifierAt(instance, name)));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    if (Options::aotCountsPath()) [[unlikely]]
        instance->state->propertyCaches->countMiss("read without caches"_s, false, value, identifierAt(instance, name).impl());
    LookByName look(vm, value, identifierAt(instance, name).impl());
    OPERATION_RETURN(scope, JSValue::encode(getWithoutCaches(instance, value, identifierAt(instance, name))));
}

static bool tryReplaceGlobalProperty(Instance* instance, VM& vm, JSValue base, UniquedStringImpl* uid, JSValue value)
{
    JSGlobalObject* target = instance->globalObject;
    if (base != JSValue(target->globalThis()) || base.asCell()->type() != GlobalProxyType)
        return false;
    Structure* structure = target->structure();
    PropertyCaches& caches = *instance->state->propertyCaches;
    const Slot* filed = caches.findByKey(structure, uid, SlotAccess::Store);
    if (filed && (!filed->has(Slot::isOwn) || (filed->kind != Slot::Kind::Value && filed->kind != Slot::Kind::FarValue)))
        return false;
    PropertyOffset offset = filed ? filed->offsetInStorage() : invalidOffset;
    if (!filed) {
        unsigned attributes = 0;
        offset = structure->get(vm, uid, attributes);
        if (!isValidOffset(offset) || attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue || structure->isImmutableProperty(uid))
            return false;
        if (structure->propertyAccessesAreCacheable()) {
            structure->didCachePropertyReplacement(vm, offset);
            caches.cache({ structure, { uid, std::nullopt }, SlotAccess::Store }, PropertyAnswer::own(PropertyAnswerKind::Value, SlotAccess::Store, offset));
        }
    }
    structure->didReplaceProperty(offset);
    target->putDirectOffset(vm, offset, value);
    return true;
}

static void putById(Instance* instance, JSValue base, uint32_t name, JSValue value, PutMode mode)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& identifier = identifierAt(instance, name);
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, base, identifier));
        return;
    }
    if (Options::aotCountsPath()) [[unlikely]]
        instance->state->propertyCaches->countMiss("store without caches"_s, false, base, identifier.impl());
    if (base.isCell() && base.asCell()->type() == SlimClosureType)
        RELEASE_AND_RETURN(scope, putAndCache({ instance, identifier, name, nullptr }, base.asCell(), value, mode));
    PutPropertySlot slot(base, mode == PutMode::Strict, PutPropertySlot::PutById);
    if (Object* object = compiledObjectOrNull(base)) {
        const FieldData* field = object->fieldNamed(*instance->state, identifier);
        if (field && object->hasExtraProperties() && !object->read(*field))
            object->ensureStructureForExtraProperties(vm);
        Object::putWithKnownField(object, globalObject, identifier, value, slot, field);
        RETURN_IF_EXCEPTION(scope, void());
        object->state().objects->classes.didStoreByName(vm, object->layout(), identifier.impl());
        return;
    }
    if (tryReplaceGlobalProperty(instance, vm, base, identifier.impl(), value))
        return;
    scope.release();
    LookByName look(vm, base, identifier.impl());
    base.put(globalObject, identifier, value, slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutById, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutById);
    putById(instance, JSValue::decode(base), name, JSValue::decode(value), PutMode::Strict);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByIdSloppy, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPutByIdSloppy);
    putById(instance, JSValue::decode(base), name, JSValue::decode(value), PutMode::Sloppy);
    OPERATION_RETURN(scope);
}

// Files what the stubs can repeat for the next object of this Structure, or that they cannot.
static NEVER_INLINE bool deleteAndFile(Instance* instance, JSObject* object, PropertyName property)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr SlotAccess access = SlotAccess::Delete;
    PropertyCaches& caches = *instance->state->propertyCaches;
    Structure* structureBefore = object->structure();
    Object* compiled = compiledObjectOrNull(object);
    JSObject* extra = compiled ? compiled->extraProperties() : nullptr;
    bool belongsToOneObject = extra && !compiled->compiledStructure().extraPropertiesSlot();
    if (belongsToOneObject || !caches.shouldFile(structureBefore->id(), property.uid(), access))
        RELEASE_AND_RETURN(scope, JSCell::deleteProperty(object, globalObject, property));
    CachedAccess filed { structureBefore, { property.uid(), std::nullopt }, access };
    // The next object arrives as this one did, so nothing is gained by a Structure that says more.
    bool listsExtraProperties = extra && compiled->compiledStructure().listsEveryExtraProperty();
    if (extra && !listsExtraProperties && !compiled->fieldNamed(compiled->state(), property)) {
        caches.cache(filed, PropertyAnswer::uncacheable(access));
        RELEASE_AND_RETURN(scope, JSCell::deleteProperty(object, globalObject, property));
    }
    uint32_t orderWordBefore = compiled ? compiled->orderWord() : 0;
    DeletePropertySlot slot;
    bool deleted = object->methodTable()->deleteProperty(object, globalObject, property, slot);
    RETURN_IF_EXCEPTION(scope, false);
    PropertyAnswer answer = PropertyAnswer::uncacheable(access);
    if (deleted) {
        if (listsExtraProperties)
            compiled->ensureStructureForExtraProperties(vm);
        answer = classifyDeletion({ globalObject, object, structureBefore, orderWordBefore, property.uid(), slot });
    }
    // Whoever arrives with this Structure again finds a slot, and does not ask for one to be filed.
    caches.cache(filed, answer.didFlatten ? PropertyAnswer::uncacheable(access) : answer);
    return deleted;
}

static ALWAYS_INLINE size_t deleteNamedProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName property, bool isStrict, Instance* instanceThatFiles)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (vm.aotState().hasObjectsBeingFilled) [[unlikely]]
        RuntimeHooks::finishFillingObjects(vm);
    if (Structure* structure = object->structure(); object->type() == FinalObjectType && structure->isDictionary() && !structure->isUncacheableDictionary()) {
        // Every name that a cacheable dictionary gives up makes a Structure and a copy of its table.
        unsigned attributes;
        if (isValidOffset(structure->get(vm, property, attributes)) && !(attributes & PropertyAttribute::DontDelete))
            object->convertToUncacheableDictionary(vm);
    }
    bool deleted = instanceThatFiles ? deleteAndFile(instanceThatFiles, object, property) : JSCell::deleteProperty(object, globalObject, property);
    RETURN_IF_EXCEPTION(scope, 0);
    if (!deleted && isStrict)
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
    return deleted;
}

static size_t deleteFromValue(JSGlobalObject* globalObject, JSValue base, const Identifier* name, JSValue key, bool isStrict)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = base.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    if (uint32_t index; !name && key.getUInt32(index)) {
        bool deleted = object->methodTable()->deletePropertyByIndex(object, globalObject, index);
        RETURN_IF_EXCEPTION(scope, 0);
        if (!deleted && isStrict)
            throwTypeError(globalObject, scope, UnableToDeletePropertyError);
        return deleted;
    }
    Identifier nameOfKey;
    if (auto longKeyName = name ? std::nullopt : findNameEqualToLongString(object, key, false)) [[unlikely]] {
        if (!*longKeyName)
            return 1;
        nameOfKey = Identifier::fromUid(vm, *longKeyName);
    } else if (!name) {
        nameOfKey = key.toPropertyKey(globalObject);
        RETURN_IF_EXCEPTION(scope, 0);
    }
    RELEASE_AND_RETURN(scope, deleteNamedProperty(globalObject, object, name ? *name : nameOfKey, isStrict, nullptr));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteById, size_t, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteById);
    Identifier identifier = identifierAt(instance, name);
    OPERATION_RETURN(scope, deleteFromValue(globalObject, JSValue::decode(base), &identifier, { }, true));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByIdSloppy, size_t, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteByIdSloppy);
    Identifier identifier = identifierAt(instance, name);
    OPERATION_RETURN(scope, deleteFromValue(globalObject, JSValue::decode(base), &identifier, { }, false));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByVal, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteByVal);
    OPERATION_RETURN(scope, deleteFromValue(globalObject, JSValue::decode(base), nullptr, JSValue::decode(key), true));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByValSloppy, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteByValSloppy);
    OPERATION_RETURN(scope, deleteFromValue(globalObject, JSValue::decode(base), nullptr, JSValue::decode(key), false));
}

static ALWAYS_INLINE size_t deleteByName(Instance* instance, JSCell* base, UniquedStringImpl* uid, bool isStrict)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The stub hands over the name alone, without the string or the symbol that it read the name from.
    Identifier name = Identifier::fromUid(vm, uid);
    JSObject* object = JSValue(base).toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    Instance* instanceThatFiles = nullptr;
    // The stub found the Structure and the name in no table, and not for the first time.
    if (instance->propertyCaches.missIsDueForFiling) [[unlikely]] {
        instance->propertyCaches.missIsDueForFiling = 0;
        if (object == base)
            instanceThatFiles = instance;
    }
    RELEASE_AND_RETURN(scope, deleteNamedProperty(globalObject, object, name, isStrict, instanceThatFiles));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByName, size_t, (Instance* instance, JSCell* base, UniquedStringImpl* uid))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteByName);
    OPERATION_RETURN(scope, deleteByName(instance, base, uid, true));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDeleteByNameSloppy, size_t, (Instance* instance, JSCell* base, UniquedStringImpl* uid))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDeleteByNameSloppy);
    OPERATION_RETURN(scope, deleteByName(instance, base, uid, false));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetSuperBase, EncodedJSValue, (Instance* instance, EncodedJSValue home, uint32_t homeKind))
{
    constexpr Entry entry = Entry::operationAOTGetSuperBase;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGCForAWhile deferGC(vm);
    JSObject* homeObject = objectArgument(home, entry);
    if (static_cast<Typed::HomeObjectKind>(homeKind) == Typed::HomeObjectKind::ClassPrototype) {
        Closure* classObject = cellArgument<Closure>(homeObject, entry);
        homeObject = objectArgument(JSValue::encode(classObject->prototypeProperty(vm)), entry);
        instance->state->objects->classes.rememberPlaceOfPrototypeProperty(vm, classObject, homeObject);
    }
    if (homeObject->structure()->typeInfo().overridesGetPrototype()) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, JSValue::encode(homeObject->getPrototypeDirect()));
}

static JSObject* checkSuperBase(JSGlobalObject* globalObject, VM& vm, JSValue base, const Identifier* member = nullptr)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!base.isObject())
        throwException(globalObject, scope, createSuperBaseNotAnObjectError(globalObject, base, member));
    return base.getObject();
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetSuperPropertyById, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue receiver, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetSuperPropertyById);
    const Identifier& identifier = identifierAt(instance, name);
    JSObject* parent = checkSuperBase(globalObject, vm, JSValue::decode(base), &identifier);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    // The stub answers for what is filed unless that is an accessor or lies in no cell of the front table.
    if (instance->state->propertyCaches->findByKey(parent->structure(), identifier.impl(), SlotAccess::Read)) {
        if (JSValue found = JSValue::decode(operationAOTGetGeneric(instance, parent, identifier.impl())))
            OPERATION_RETURN(scope, JSValue::encode(found));
        OPERATION_RETURN(scope, JSValue::encode(getWithoutCaches(instance, parent, identifier, JSValue::decode(receiver))));
    }
    OPERATION_RETURN(scope, JSValue::encode(getAndCache({ instance, identifier, name, nullptr }, parent, JSValue::decode(receiver))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSuperGet, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue key, EncodedJSValue receiver))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSuperGet);
    JSObject* parent = checkSuperBase(globalObject, vm, JSValue::decode(base));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    // The answer is filed for the object above, where the stub looks the next time.
    if (!parseIndex(property))
        OPERATION_RETURN(scope, JSValue::encode(getUnderKeyForReceiver(instance, parent, property, JSValue::decode(receiver))));
    PropertySlot slot(JSValue::decode(receiver), PropertySlot::InternalMethodType::Get);
    OPERATION_RETURN(scope, JSValue::encode(JSValue(parent).get(globalObject, property, slot)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSuperSet, void, (Instance* instance, EncodedJSValue base, EncodedJSValue key, EncodedJSValue value, EncodedJSValue receiver, uint32_t mode))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSuperSet);
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    JSObject* parent = checkSuperBase(globalObject, vm, JSValue::decode(base));
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PutPropertySlot slot(JSValue::decode(receiver), static_cast<Typed::ECMAMode>(mode) == Typed::ECMAMode::Strict);
    parent->methodTable()->put(parent, globalObject, property, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTScopeHasBinding, size_t, (Instance* instance, EncodedJSValue encodedScope, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTScopeHasBinding);
    JSObject* object = objectArgument(encodedScope, Entry::operationAOTScopeHasBinding);
    const Identifier& identifier = identifierAt(instance, name);
    bool hasBinding = object->hasProperty(globalObject, identifier);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (!hasBinding)
        OPERATION_RETURN(scope, false);
    JSValue unscopables = object->get(globalObject, vm.propertyNames->unscopablesSymbol);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (!unscopables.isObject())
        OPERATION_RETURN(scope, true);
    JSValue blocked = asObject(unscopables)->get(globalObject, identifier);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, !blocked.toBoolean(globalObject));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewDictionary, JSCell*, (Instance* instance, uint32_t inlineCapacity))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewDictionary);
    DeferTerminationForAWhile deferTermination(vm);
    ThrowsOwedOutOfMemoryErrorAtReturn owedError(instance, frameRecord);
    Structure* structure = globalObject->objectStructureForObjectConstructor();
    if (inlineCapacity != JSFinalObject::defaultInlineCapacity)
        structure = globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), inlineCapacity);
    prepareFinalObjectAllocation(instance, static_cast<InlineAllocation>(static_cast<unsigned>(InlineAllocation::DefaultFinalObject) + (inlineCapacity - JSFinalObject::defaultInlineCapacity) / 2), structure);
    OPERATION_RETURN(scope, constructEmptyObject(vm, structure));
}

static void putToArray(JSGlobalObject* globalObject, JSArray* array, JSValue key, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (std::optional<uint32_t> index = key.tryGetAsUint32Index()) {
        IndexingType shape = array->indexingType() & IndexingShapeMask;
        bool isPush = (shape == Int32Shape || shape == DoubleShape || shape == ContiguousShape) && *index == array->butterfly()->publicLength();
        if (isPush && pushToFullVector(vm, array, value))
            return;
        scope.release();
        array->methodTable()->putByIndex(array, globalObject, *index, value, true);
        return;
    }
    auto name = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    PutPropertySlot slot(array, true);
    scope.release();
    array->methodTable()->put(array, globalObject, name, value, slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPutAtIndex, void, (Instance* instance, JSCell* array, EncodedJSValue value, uint64_t position))
{
    constexpr Entry entry = Entry::operationAOTArrayPutAtIndex;
    AOT_OPERATION_BEGIN(instance, entry);
    putToArray(globalObject, cellArgument<JSArray>(array, entry), jsNumber(static_cast<double>(static_cast<int64_t>(position))), JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPutNumberAtIndex, void, (Instance* instance, JSCell* cell, EncodedJSValue value, uint64_t position))
{
    constexpr Entry entry = Entry::operationAOTArrayPutNumberAtIndex;
    AOT_OPERATION_BEGIN(instance, entry);
    JSArray* array = cellArgument<JSArray>(cell, entry);
    if (JSValue number = JSValue::decode(value); number.isNumber() && !std::isnan(number.asNumber()) && hasInt32(array->indexingType()))
        array->tryMakeWritableDouble(vm);
    putToArray(globalObject, array, jsNumber(static_cast<double>(static_cast<int64_t>(position))), JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPutAtNumber, void, (Instance* instance, JSCell* array, EncodedJSValue value, double position))
{
    constexpr Entry entry = Entry::operationAOTArrayPutAtNumber;
    AOT_OPERATION_BEGIN(instance, entry);
    putToArray(globalObject, cellArgument<JSArray>(array, entry), jsNumber(purifyNaN(position)), JSValue::decode(value));
    OPERATION_RETURN(scope);
}

struct FieldLeftByStub {
    Object* object;
    const FieldData& field;
};

static FieldLeftByStub fieldLeftByStub(Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, uint32_t reason, Entry entry, ASCIILiteral what = { })
{
    auto counter = static_cast<RuntimeCounterID>(reason);
    Slot slot = std::bit_cast<Slot>(std::array<uint64_t, 2> { header, payload });
    if (!countsStubExit(counter)) [[unlikely]]
        crashOnWrongOperand(entry);
    count(*instance, counter);
    auto* object = cellArgument<Object>(cell, entry);
    if (Slot::payloadOf(slot.kind) != SlotPayload::Field && Slot::payloadOf(slot.kind) != SlotPayload::FieldAndHolder) [[unlikely]]
        crashOnWrongOperand(entry);
    for (const FieldData& field : object->layout().fields()) {
        if (field.slot != slot.fieldSlot() || field.isOutOfLine() != slot.has(Slot::isOutOfLine))
            continue;
        if (PropertyCaches& caches = *instance->state->propertyCaches; !what.isNull() && caches.countsNames()) [[unlikely]]
            caches.countNamed(what, instance->state->objects->layouts.nameOf(*instance->vm, field));
        return { object, field };
    }
    crashOnWrongOperand(entry);
}

static void storeInPresentField(VM& vm, Object* object, const FieldData& field, JSValue value)
{
    if (!object->tryStore(vm, field, value))
        object->moveFieldToExtraProperties(vm, field, { value, object->attributesOf(field) }, SlotExitReason::StoreOfOtherType);
}

static void ensureGranules(VM& vm, Object* object, uint64_t granuleBits)
{
    object->ensureExtensionGranules(vm, granuleBits | object->layout().granulesFilledByConstructor());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReadField, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTReadField;
    AOT_OPERATION_BEGIN(instance, entry);
    auto [object, field] = fieldLeftByStub(instance, cell, header, payload, reason, entry, "stub could not read field"_s);
    if (field.slot.representation() == SlotRepresentation::DeferredClosure) {
        if (!object->isInSlot(field)) [[unlikely]]
            crashOnWrongOperand(entry);
        return JSValue::encode(object->materializeDeferredClosure(vm, field));
    }
    JSValue value = object->read(field);
    return JSValue::encode(value ? value : jsUndefined());
}

JSC_DEFINE_JIT_OPERATION(operationAOTStoreField, void, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, EncodedJSValue value, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTStoreField;
    AOT_OPERATION_BEGIN(instance, entry);
    auto [object, field] = fieldLeftByStub(instance, cell, header, payload, reason, entry, "stub could not store field"_s);
    if (!findFieldProperty(vm, *object, field).isInSlot) [[unlikely]]
        crashOnWrongOperand(entry);
    storeInPresentField(vm, object, field, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateField, void, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, EncodedJSValue encodedValue, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTCreateField;
    AOT_OPERATION_BEGIN(instance, entry);
    auto [object, field] = fieldLeftByStub(instance, cell, header, payload, reason, entry);
    JSValue value = JSValue::decode(encodedValue);
    if (PropertyCaches& caches = *instance->state->propertyCaches; caches.countsNames()) [[unlikely]] {
        ASCIILiteral what = "stub could not create field though its slot admits the value"_s;
        if (field.slot.representation() == SlotRepresentation::DeferredClosure)
            what = "stub could not create field that defers its closure"_s;
        else if (!instance->state->objects->layouts.slotAdmits(field, value))
            what = "stub could not create field whose slot does not admit the value"_s;
        caches.countNamed(what, instance->state->objects->layouts.nameOf(vm, field));
    }
    FieldProperty current = findFieldProperty(vm, *object, field);
    if (current.isInSlot) {
        storeInPresentField(vm, object, field, value);
        OPERATION_RETURN(scope);
    }
    if (current || object->isNotExtensible()) [[unlikely]]
        crashOnWrongOperand(entry);
    if (field.attributes() || !tryCreateField(vm, *instance->state, *object, field, value))
        object->putDirectFromNativeCode(vm, identifierAt(instance, field.name), value, 0);
    OPERATION_RETURN(scope);
}

static const FieldData& namedField(Instance* instance, Object* object, uint32_t key)
{
    const FieldData* field = object->fieldNamed(*instance->state, identifierAt(instance, key));
    RELEASE_ASSERT(field);
    return *field;
}

static const FieldData& listCreatedField(State& state, Object* object, uint32_t slotKey)
{
    const FieldData* created = state.objects->keyOrders.fieldAt(object->layout(), slotKey);
    RELEASE_ASSERT(created);
    const FieldData& field = *created;
    if (object->structure()->mayBePrototype()) [[unlikely]] {
        VM& vm = *state.instance.vm;
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Add, state.objects->layouts.nameOf(vm, field));
    }
    if (!object->isInKeyOrder(state, field))
        object->didCreateField(state, field, false);
    else if (!object->hasOutOfLineKeyOrder()) {
        uint32_t orderAndCount = object->orderWord() & orderAndCountMask;
        if (std::optional<uint32_t> numberOfExtraKeys = object->numberOfExtraKeysByStructure())
            state.objects->keyOrders.publishTransition(object->layout(), orderAndCount, field, orderAndCount, *numberOfExtraKeys);
    }
    return field;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateField, void, (Instance* instance, JSCell* cell, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateField);
    DeferGCForAWhile deferGC(vm);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateField);
    const FieldData& field = namedField(instance, object, key);
    didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Add, identifierAt(instance, key).impl());
    object->didCreateField(*instance->state, field, !(field.flags & fieldMayExistWithoutStorage));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryFinishBirth, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryFinishBirth);
    // A field that lives among the extra properties is looked up by name, which may make a property table. The object is in the middle of its birth.
    DeferGCForAWhile deferGC(vm);
    if (isCompiledObject(cell))
        uncheckedDowncast<Object>(cell)->tryFinishBirth();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldAt, void, (Instance* instance, JSCell* cell, uint32_t offset))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateFieldAt);
    DeferGCForAWhile deferGC(vm);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldAt);
    vm.writeBarrier(object);
    listCreatedField(*instance->state, object, offset);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldOutOfLineAt, void, (Instance* instance, JSCell* cell, uint32_t offset))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateFieldOutOfLineAt);
    DeferGCForAWhile deferGC(vm);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldOutOfLineAt);
    vm.writeBarrier(object);
    ensureGranules(vm, object, granuleBitOf(listCreatedField(*instance->state, object, offset | outOfLineSlotKey).slot.offset));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldsAt, void, (Instance* instance, JSCell* cell, uint64_t offsets))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDidCreateFieldsAt);
    DeferGCForAWhile deferGC(vm);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldsAt);
    vm.writeBarrier(object);
    for (uint64_t rest = offsets; static_cast<uint32_t>(rest) & slotKeyMask; rest >>= bitsPerSlotKey)
        listCreatedField(*instance->state, object, static_cast<uint32_t>(rest) & slotKeyMask);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureExtensionGranules, void, (Instance* instance, JSCell* cell, uint64_t granuleBits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTEnsureExtensionGranules);
    DeferGCForAWhile deferGC(vm);
    ensureGranules(vm, cellArgument<Object>(cell, Entry::operationAOTEnsureExtensionGranules), granuleBits);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureExtensionGranulesWithoutCollecting, void, (Instance* instance, JSCell* cell, uint64_t granuleBits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTEnsureExtensionGranulesWithoutCollecting);
    DeferGCForAWhile deferGC(vm);
    ensureGranules(vm, cellArgument<Object>(cell, Entry::operationAOTEnsureExtensionGranulesWithoutCollecting), granuleBits);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMaterializeDeferredClosure, JSCell*, (Instance* instance, JSCell* cell, uint32_t key))
{
    constexpr Entry entry = Entry::operationAOTMaterializeDeferredClosure;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* object = cellArgument<Object>(cell, entry);
    const FieldData& field = namedField(instance, object, key);
    if (!object->isInSlot(field)) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, object->materializeDeferredClosure(vm, field).asCell());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetFieldOutsideSlot, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t key))
{
    constexpr Entry entry = Entry::operationAOTGetFieldOutsideSlot;
    AOT_OPERATION_BEGIN(instance, entry);
    DeferGCForAWhile deferGC(vm);
    JSObject* extra = cellArgument<Object>(cell, entry)->extraProperties();
    JSValue value = extra ? extra->getDirect(vm, identifierAt(instance, key)) : JSValue();
    if (value && (value.isGetterSetter() || value.isCustomGetterSetter())) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, JSValue::encode(value ? value : jsUndefined()));
}

static NEVER_INLINE size_t JIT_OPERATION_ATTRIBUTES hasExtraPropertyByLookup(Instance* instance, JSCell* cell, uint32_t key)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasExtraProperty);
    DeferGCForAWhile deferGC(vm);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTHasExtraProperty);
    const Identifier& identifier = identifierAt(instance, key);
    const FieldData* field = object->fieldNamed(*instance->state, identifier);
    if (!field) {
        JSObject* extra = object->extraProperties();
        OPERATION_RETURN(scope, extra && extra->getDirectOffset(vm, identifier) != invalidOffset);
    }
    // Of a field the question is whether its slot reads as a value. A field that lives among the extra properties has left its slot.
    if (field->slot.representation() == SlotRepresentation::DeferredClosure)
        OPERATION_RETURN(scope, object->isInSlot(*field));
    bool readsAsValue = !!object->read(*field);
    uint32_t orderWord = object->orderWord();
    bool dependsOnOrderAlone = !(orderWord & orderWordHasFieldOutsideSlot) && !isOrderStoredOutOfLine(orderWord) && !object->layout().keysAreNeverEnumerated;
    if (dependsOnOrderAlone && !validatesRuntime(vm) && object->isInKeyOrder(*instance->state, *field) == readsAsValue)
        instance->state->objects->keyOrders.rememberWhetherOrderLists(object->shape(), orderWord & orderAndCountMask, key, readsAsValue);
    OPERATION_RETURN(scope, readsAsValue);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTHasExtraProperty, size_t, (Instance* instance, JSCell* cell, uint32_t key))
{
    auto* object = cellArgument<Object>(cell, Entry::operationAOTHasExtraProperty);
    uint32_t orderWord = object->orderWord();
    if (!(orderWord & orderWordHasFieldOutsideSlot)) [[likely]] {
        if (std::optional<bool> lists = instance->state->objects->keyOrders.rememberedWhetherOrderLists(object->shape(), orderWord & orderAndCountMask, key)) [[likely]]
            return *lists;
    }
    [[clang::musttail]] return hasExtraPropertyByLookup(instance, cell, key);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
