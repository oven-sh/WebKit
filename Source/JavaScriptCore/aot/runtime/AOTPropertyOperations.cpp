/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTErrorMessages.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPendingStars.h"
#include "AOTPropertyCachesKeys.h"
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
    auto found = base.isObject() ? instance->state->objects->classes.findThroughLazyPrototypes(vm, asObject(base), identifier.impl()) : std::nullopt;
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
    ++instance->propertyCaches.slowPathReadCount;
    JSValue value = JSValue::decode(base);
    if (value.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createNotAnObjectError(globalObject, value, identifierAt(instance, name)));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    LookByName look(vm, value, identifierAt(instance, name).impl());
    OPERATION_RETURN(scope, JSValue::encode(getWithoutCaches(instance, value, identifierAt(instance, name))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCallCustomGetter, EncodedJSValue, (Instance* instance, EncodedJSValue thisValue, uint32_t, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCallCustomGetter);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(thisValue).get(globalObject, identifierAt(instance, key))));
}

bool tryReplaceGlobalProperty(Instance* instance, VM& vm, JSValue base, UniquedStringImpl* uid, JSValue value)
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
        if (!isValidOffset(offset) || attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue || structure->isImmutableProperty(uid) || isWriteJudgedByWriter(structure, uid))
            return false;
        if (structure->propertyAccessesAreCacheable()) {
            structure->didCachePropertyReplacement(vm, offset);
            caches.cache({ structure, { uid, std::nullopt }, SlotAccess::Store }, PropertyAnswer::own(PropertyAnswerKind::Value, SlotAccess::Store, offset));
        } else
            structure->didReplaceProperty(offset);
    }
    target->putDirectOffset(vm, offset, value);
    return true;
}

static bool tryReplaceValueThatIsNoCellOnObjectOfSomeChain(VM& vm, JSValue base, UniquedStringImpl* uid, JSValue value)
{
    if (value.isCell() || !base.isObject())
        return false;
    JSObject* object = asObject(base);
    Structure* structure = object->structure();
    if (!structure->mayBePrototype() || structure->isCompiledStructure())
        return false;
    auto put = structure->classInfoForCells()->methodTable.put;
    if (put != JSObject::info()->methodTable.put && put != InternalFunction::info()->methodTable.put)
        return false;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, uid, attributes);
    if (!isValidOffset(offset) || attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue || structure->isImmutableProperty(uid) || isWriteJudgedByWriter(structure, uid))
        return false;
    JSValue held = object->getDirect(offset);
    if (held == value)
        return true;
    if (held.isCell())
        return false;
    structure->didReplaceProperty(offset);
    object->putDirectOffset(vm, offset, value);
    didReplaceValueOnChainObject(vm, object, uid);
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
    if (base.isCell() && base.asCell()->type() == SlimClosureType)
        RELEASE_AND_RETURN(scope, putAndCache({ instance, identifier, name }, base.asCell(), value, mode));
    PutPropertySlot slot(base, mode == PutMode::Strict, PutPropertySlot::PutById);
    if (Object* object = compiledObjectOrNull(base)) {
        if (tryRepeatFieldExit(instance, base, name, value, mode))
            return;
        const FieldData* field = object->fieldNamed(*instance->state, identifier);
        if (field && object->hasExtraProperties() && !object->read(*field))
            object->ensureStructureForExtraProperties(vm);
        bool chainLetsStoresPass = field && !object->isInSlot(*field) && vm.aotState().propertyCaches->addFilterAndCheckStoreCreatesOwnField(globalObject, *object, field->name);
        scope.release();
        Object::putWithKnownField(object, globalObject, identifier, value, slot, field, chainLetsStoresPass);
        return;
    }
    if (tryReplaceGlobalProperty(instance, vm, base, identifier.impl(), value))
        return;
    if (tryReplaceValueThatIsNoCellOnObjectOfSomeChain(vm, base, identifier.impl(), value))
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

static NEVER_INLINE bool deleteAndFile(Instance* instance, JSObject* object, PropertyName property)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr SlotAccess access = SlotAccess::Delete;
    PropertyCaches& caches = *instance->state->propertyCaches;
    Structure* structureBefore = object->structure();
    Object* compiled = compiledObjectOrNull(object);
    bool starsWait = compiled && hasPendingStars(*compiled);
    JSObject* extra = compiled && !starsWait ? compiled->extraProperties() : nullptr;
    bool belongsToOneObject = extra && !compiled->compiledStructure().extraPropertiesSlot();
    if (belongsToOneObject || starsWait || !caches.shouldFile(structureBefore->id(), property.uid(), access))
        RELEASE_AND_RETURN(scope, JSCell::deleteProperty(object, globalObject, property));
    CachedAccess filed { structureBefore, { property.uid(), std::nullopt }, access };
    bool listsExtraProperties = extra && compiled->compiledStructure().listsEveryExtraProperty();
    if (extra && !listsExtraProperties && !compiled->fieldNamed(compiled->state(), property.uid())) {
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
    caches.cache(filed, answer.didFlatten ? PropertyAnswer::uncacheable(access) : answer);
    return deleted;
}

static uint64_t orderAndShapeWordsOf(const Object& object)
{
    return WTF::unalignedLoad<uint64_t>(reinterpret_cast<const uint8_t*>(&object) + offsetOfOrderWord);
}

static void recordOrderAfterRemoval(Instance& instance, Object& object, UniquedStringImpl* uid, StructureID structureBefore, uint64_t wordsBefore)
{
    uint32_t orderWordBefore = static_cast<uint32_t>(wordsBefore);
    if (object.structureID() != structureBefore || orderWordBefore & ~orderAndCountMask || object.orderWord() == orderWordBefore || object.compiledStructure().hasOutOfLineKeyOrder())
        return;
    const FieldData* field = object.fieldNamed(object.state(), uid);
    if (!field || field->isOutOfLine() || field->isPrivate() || !absentPatternOf(field->slot.representation()) || field->slot.offset > UINT16_MAX)
        return;
    uint64_t key = removalRunKey(static_cast<uint16_t>(field->slot.offset));
    instance.objects.keyOrderRuns[indexOfKeyOrderRun(wordsBefore, key)] = { wordsBefore, key, object.orderWord(), 0 };
}

static ALWAYS_INLINE size_t deleteNamedProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName property, bool isStrict, Instance* instanceThatFiles)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (vm.aotState().hasObjectsBeingFilled) [[unlikely]]
        RuntimeHooks::finishFillingObjects(vm);
    if (Structure* structure = object->structure(); object->type() == FinalObjectType && structure->isDictionary() && !structure->isUncacheableDictionary()) {
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
    if (base.isUndefinedOrNull()) [[unlikely]] {
        throwException(globalObject, scope, createDeletionFromNullishError(globalObject, base));
        return 0;
    }
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
    Identifier name = Identifier::fromUid(vm, uid);
    JSObject* object = JSValue(base).toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, 0);
    Instance* instanceThatFiles = nullptr;
    if (instance->propertyCaches.missIsDueForFiling) [[unlikely]] {
        instance->propertyCaches.missIsDueForFiling = 0;
        if (object == base)
            instanceThatFiles = instance;
    }
    Object* compiled = compiledObjectOrNull(object);
    if (!compiled)
        RELEASE_AND_RETURN(scope, deleteNamedProperty(globalObject, object, name, isStrict, instanceThatFiles));
    StructureID structureBefore = compiled->structureID();
    uint64_t wordsBefore = orderAndShapeWordsOf(*compiled);
    size_t deleted = deleteNamedProperty(globalObject, object, name, isStrict, instanceThatFiles);
    RETURN_IF_EXCEPTION(scope, 0);
    if (deleted)
        recordOrderAfterRemoval(*instance, *compiled, uid, structureBefore, wordsBefore);
    return deleted;
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

static ALWAYS_INLINE EncodedJSValue baseOfSuper(Instance* instance, VM& vm, EncodedJSValue home, Typed::HomeObjectKind homeKind, Entry entry)
{
    JSObject* homeObject = objectArgument(home, entry);
    if (homeKind == Typed::HomeObjectKind::ClassPrototype) {
        Closure* classObject = cellArgument<Closure>(homeObject, entry);
        homeObject = objectArgument(JSValue::encode(classObject->prototypeProperty(vm)), entry);
        instance->state->objects->classes.rememberPlaceOfPrototypeProperty(vm, classObject, homeObject);
    }
    if (homeObject->structure()->typeInfo().overridesGetPrototype()) [[unlikely]]
        crashOnWrongOperand(entry);
    return JSValue::encode(homeObject->getPrototypeDirect());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTGetSuperBase, EncodedJSValue, (Instance* instance, EncodedJSValue home, uint32_t homeKind))
{
    constexpr Entry entry = Entry::operationAOTGetSuperBase;
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry);
    OPERATION_RETURN(scope, baseOfSuper(instance, vm, home, static_cast<Typed::HomeObjectKind>(homeKind), entry));
}

static JSObject* checkSuperBase(JSGlobalObject* globalObject, VM& vm, JSValue base, const Identifier* member = nullptr)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!base.isObject())
        throwException(globalObject, scope, createSuperBaseNotAnObjectError(globalObject, base, member));
    return base.getObject();
}

static ALWAYS_INLINE EncodedJSValue propertyThroughSuperById(Instance* instance, EncodedJSValue base, EncodedJSValue receiver, uint32_t name)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& identifier = identifierAt(instance, name);
    JSObject* parent = checkSuperBase(globalObject, vm, JSValue::decode(base), &identifier);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (instance->state->propertyCaches->findByKey(parent->structure(), identifier.impl(), SlotAccess::Read)) {
        if (JSValue found = JSValue::decode(operationAOTGetGeneric(instance, parent, identifier.impl())))
            return JSValue::encode(found);
        RELEASE_AND_RETURN(scope, JSValue::encode(getWithoutCaches(instance, parent, identifier, JSValue::decode(receiver))));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(getAndCache({ instance, identifier, name }, parent, JSValue::decode(receiver))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetSuperPropertyById, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue receiver, uint32_t name))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetSuperPropertyById);
    OPERATION_RETURN(scope, propertyThroughSuperById(instance, base, receiver, name));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetSuperPropertyOfClassById, EncodedJSValue, (Instance* instance, EncodedJSValue classObject, EncodedJSValue receiver, uint32_t name))
{
    constexpr Entry entry = Entry::operationAOTGetSuperPropertyOfClassById;
    AOT_OPERATION_BEGIN(instance, entry);
    EncodedJSValue base;
    {
        DeferGCForAWhile deferGC(vm);
        base = baseOfSuper(instance, vm, classObject, Typed::HomeObjectKind::ClassPrototype, entry);
    }
    OPERATION_RETURN(scope, propertyThroughSuperById(instance, base, receiver, name));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSuperGet, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue key, EncodedJSValue receiver))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSuperGet);
    JSObject* parent = checkSuperBase(globalObject, vm, JSValue::decode(base));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNewDictionary, JSCell*, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewDictionary);
    DeferTerminationForAWhile deferTermination(vm);
    Structure* structure = globalObject->objectStructureForObjectConstructor();
    prepareFinalObjectAllocation(instance, InlineAllocation::DefaultFinalObject, structure);
    OPERATION_RETURN(scope, constructEmptyObject(vm, structure));
}

static void putToArray(JSGlobalObject* globalObject, JSArray* array, JSValue key, JSValue value, ECMAMode mode = ECMAMode::strict())
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (std::optional<uint32_t> index = key.tryGetAsUint32Index()) {
        IndexingType shape = array->indexingType() & IndexingShapeMask;
        bool isPush = (shape == Int32Shape || shape == DoubleShape || shape == ContiguousShape) && *index == array->butterfly()->publicLength();
        if (isPush && pushToFullVector(vm, array, value))
            return;
        scope.release();
        array->methodTable()->putByIndex(array, globalObject, *index, value, mode.isStrict());
        return;
    }
    auto name = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    PutPropertySlot slot(array, mode.isStrict());
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

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPutAtIndexInSloppyCode, void, (Instance* instance, JSCell* array, EncodedJSValue value, uint64_t position))
{
    constexpr Entry entry = Entry::operationAOTArrayPutAtIndexInSloppyCode;
    AOT_OPERATION_BEGIN(instance, entry);
    putToArray(globalObject, cellArgument<JSArray>(array, entry), jsNumber(static_cast<double>(static_cast<int64_t>(position))), JSValue::decode(value), ECMAMode::sloppy());
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

static FieldLeftByStub fieldLeftByStub(JSCell* cell, uint64_t header, uint64_t payload, Entry entry)
{
    Slot slot = std::bit_cast<Slot>(std::array<uint64_t, 2> { header, payload });
    auto* object = cellArgument<Object>(cell, entry);
    if (Slot::payloadOf(slot.kind) != SlotPayload::Field && Slot::payloadOf(slot.kind) != SlotPayload::FieldAndHolder) [[unlikely]]
        crashOnWrongOperand(entry);
    for (const FieldData& field : object->layout().fields()) {
        if (field.slot != slot.fieldSlot() || field.isOutOfLine() != slot.has(Slot::isOutOfLine))
            continue;
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
    object->ensureExtensionGranules(vm, granuleBits | object->layout().granulesOfFirstExtension());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReadField, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, uint32_t))
{
    constexpr Entry entry = Entry::operationAOTReadField;
    AOT_OPERATION_BEGIN(instance, entry);
    auto [object, field] = fieldLeftByStub(cell, header, payload, entry);
    if (field.slot.representation() == SlotRepresentation::DeferredClosure) {
        if (!object->isInSlot(field)) [[unlikely]]
            crashOnWrongOperand(entry);
        return JSValue::encode(object->materializeDeferredClosure(vm, field));
    }
    JSValue value = object->read(field);
    return JSValue::encode(value ? value : jsUndefined());
}

JSC_DEFINE_JIT_OPERATION(operationAOTStoreField, void, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, EncodedJSValue value, uint32_t))
{
    constexpr Entry entry = Entry::operationAOTStoreField;
    AOT_OPERATION_BEGIN(instance, entry);
    auto [object, field] = fieldLeftByStub(cell, header, payload, entry);
    if (!findFieldProperty(vm, *object, field).isInSlot) [[unlikely]]
        crashOnWrongOperand(entry);
    storeInPresentField(vm, object, field, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCreateField, void, (Instance* instance, JSCell* cell, uint64_t header, uint64_t payload, EncodedJSValue encodedValue, uint32_t))
{
    constexpr Entry entry = Entry::operationAOTCreateField;
    AOT_OPERATION_BEGIN(instance, entry);
    auto [object, field] = fieldLeftByStub(cell, header, payload, entry);
    JSValue value = JSValue::decode(encodedValue);
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

static const FieldData* tryStepToFieldInHeldKeyOrder(Object* object, uint32_t slotKey)
{
    if (object->hasFlagsInOrderWord() || !object->hasOutOfLineKeyOrder())
        return nullptr;
    const CompiledStructure& structure = object->compiledStructure();
    const LayoutData& layout = object->layout();
    uint32_t count = countOf(object->orderWord());
    if (structure.hasKeyOrderOfOneObject() || structure.mayBePrototype() || layout.isClass() || !count || count >= maxFieldsInOrder)
        return nullptr;
    std::span<const uint16_t> held = structure.outOfLineKeyOrder();
    if (count >= held.size() || held[count] >= layout.numberOfFields)
        return nullptr;
    const FieldData& field = layout.fields()[held[count]];
    if (slotKeyFor(field.slot.offset, field.isOutOfLine()) != slotKey || field.flags & fieldMayExistWithoutStorage)
        return nullptr;
    object->setOrderAndCount(outOfLineOrderWordFor(count + 1));
    return &field;
}

static void listCreatedField(State& state, Object* object, uint32_t slotKey)
{
    if (tryStepToFieldInHeldKeyOrder(object, slotKey))
        return;
    bool stepIsOfOrderAlone = !object->structure()->mayBePrototype() && !object->layout().isClass() && !object->hasFlagsInOrderWord() && !object->hasKeyOrderOfItsOwn();
    if (stepIsOfOrderAlone && object->tryRepeatCreationAtSlot(state, slotKey))
        return;
    StructureID structureBefore = object->structureID();
    uint32_t orderAndCountBefore = object->orderWord() & orderAndCountMask;
    const FieldData* created = state.objects->keyOrders.fieldAt(object->layout(), slotKey);
    RELEASE_ASSERT(created);
    const FieldData& field = *created;
    if (object->structure()->mayBePrototype()) [[unlikely]] {
        VM& vm = *state.instance.vm;
        didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Add, state.objects->layouts.nameOf(vm, field));
    }
    bool isKnownNotToBeListed = !(field.flags & fieldMayExistWithoutStorage) && !(object->orderWord() & orderWordHasFieldOutsideSlot);
    if (isKnownNotToBeListed || !object->isInKeyOrder(state, field))
        object->didCreateField(state, field, false);
    else if (std::optional<uint32_t> numberOfExtraKeys = object->numberOfExtraKeysByStructure(); numberOfExtraKeys && !object->hasOutOfLineKeyOrder()) {
        uint32_t orderAndCount = object->orderWord() & orderAndCountMask;
        state.objects->keyOrders.publishTransition(object->layout(), orderAndCount, field, orderAndCount, *numberOfExtraKeys);
    }
    if (stepIsOfOrderAlone)
        object->rememberCreationAtSlot(state, structureBefore, orderAndCountBefore, slotKey);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateField, void, (Instance* instance, JSCell* cell, uint32_t key))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTDidCreateField);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateField);
    const FieldData& field = namedField(instance, object, key);
    didChangeOwnProperties(vm, *object, VM::StructureChainIntegrityEvent::Add, identifierAt(instance, key).impl());
    object->didCreateField(*instance->state, field, !(field.flags & fieldMayExistWithoutStorage));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryFinishBirth, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTTryFinishBirth);
    if (isCompiledObject(cell))
        uncheckedDowncast<Object>(cell)->tryFinishBirth();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAreHeldFieldsFilled, size_t, (Instance*, JSCell* cell))
{
    return cellArgument<Object>(cell, Entry::operationAOTAreHeldFieldsFilled)->holdsEveryFieldHeldFilled();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDidCreateFieldAt, void, (Instance* instance, JSCell* cell, uint32_t offset))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTDidCreateFieldAt);
    auto* object = cellArgument<Object>(cell, Entry::operationAOTDidCreateFieldAt);
    vm.writeBarrier(object);
    listCreatedField(*instance->state, object, offset);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureExtensionGranules, void, (Instance* instance, JSCell* cell, uint64_t granuleBits))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTEnsureExtensionGranules);
    ensureGranules(vm, cellArgument<Object>(cell, Entry::operationAOTEnsureExtensionGranules), granuleBits);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTEnsureExtensionGranulesWithoutCollecting, void, (Instance* instance, JSCell* cell, uint64_t granuleBits))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTEnsureExtensionGranulesWithoutCollecting);
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
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry);
    JSObject* extra = cellArgument<Object>(cell, entry)->extraProperties();
    JSValue value = extra ? extra->getDirect(vm, identifierAt(instance, key)) : JSValue();
    if (value && (value.isGetterSetter() || value.isCustomGetterSetter())) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, JSValue::encode(value ? value : jsUndefined()));
}

static NEVER_INLINE size_t JIT_OPERATION_ATTRIBUTES hasExtraPropertyByLookup(Instance* instance, JSCell* cell, uint32_t key)
{
    constexpr Entry entry = Entry::operationAOTHasExtraProperty;
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, entry);
    auto* object = cellArgument<Object>(cell, entry);
    const Identifier& identifier = identifierAt(instance, key);
    const FieldData* field = object->fieldNamed(*instance->state, identifier);
    if (!field) {
        if (std::optional<Object*> source = sourceOfPendingBinding(vm, *object, identifier.impl()))
            OPERATION_RETURN(scope, !!*source);
        JSObject* extra = object->extraProperties();
        OPERATION_RETURN(scope, extra && extra->getDirectOffset(vm, identifier) != invalidOffset);
    }
    if (field->slot.representation() == SlotRepresentation::DeferredClosure)
        OPERATION_RETURN(scope, object->isInSlot(*field));
    bool readsAsValue = !!object->read(*field);
    uint32_t orderWord = object->orderWord();
    bool dependsOnOrderAlone = !(orderWord & orderWordHasFieldOutsideSlot) && !object->bearsMark() && !isOrderStoredOutOfLine(orderWord);
    if (dependsOnOrderAlone && object->isInKeyOrder(*instance->state, *field) == readsAsValue)
        instance->state->objects->keyOrders.rememberWhetherOrderLists(object->shape(), orderWord & orderAndCountMask, key, readsAsValue);
    OPERATION_RETURN(scope, readsAsValue);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTHasExtraProperty, size_t, (Instance* instance, JSCell* cell, uint32_t key))
{
    auto* object = cellArgument<Object>(cell, Entry::operationAOTHasExtraProperty);
    uint32_t orderWord = object->orderWord();
    if (!(orderWord & orderWordHasFieldOutsideSlot) && !object->bearsMark()) [[likely]] {
        if (std::optional<bool> lists = instance->state->objects->keyOrders.rememberedWhetherOrderLists(object->shape(), orderWord & orderAndCountMask, key)) [[likely]]
            return *lists;
    }
    [[clang::musttail]] return hasExtraPropertyByLookup(instance, cell, key);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
