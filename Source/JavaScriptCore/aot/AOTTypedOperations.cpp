/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedOperations.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTInlineCaches.h"
#include "AOTModuleEvaluation.h"
#include "AOTOperationHelpers.h"
#include "AOTOperationsObjects.h"
#include "AOTRuntime.h"
#include "AOTSlotWatchpoint.h"
#include "AOTTypedBuiltins.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedObject.h"
#include "AOTTypedType.h"
#include "AbstractModuleRecord.h"
#include "AsyncContextSwapScope.h"
#include "BuiltinNames.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "JSModuleRecord.h"
#include "ClonedArguments.h"
#include "Completion.h"
#include "DirectEvalExecutable.h"
#include "ErrorConstructor.h"
#include "Interpreter.h"
#include "DeferTermination.h"
#include "DirectArguments.h"
#include "IteratorOperations.h"
#include "JSAsyncFromSyncIterator.h"
#include "CustomGetterSetter.h"
#include "GetterSetter.h"
#include "JSBoundFunction.h"
#include "JSCellButterfly.h"
#include "JSONObject.h"
#include "MarkedBlockCage.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSMapInlines.h"
#include "JSPromise.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"
#include "ModuleRegistryEntry.h"
#include "JSTypedArrays.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "ObjectAllocationProfileInlines.h"
#include "ObjectConstructor.h"
#include "JSGlobalObjectFunctions.h"
#include "ObjectConstructorInlines.h"
#include "ObjectPropertyConditionSet.h"
#include "ObjectPrototype.h"
#include "ProxyObject.h"
#include "Operations.h"
#include "TopExceptionScope.h"
#include "TypeofType.h"
#include "VMTrapsInlines.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT {

using Typed::State;

static ALWAYS_INLINE const Identifier& nameAt(Instance* instance, uint32_t index)
{
    return instance->typed->name(*instance->vm, index);
}

static void fill(MarkedArgumentBuffer& buffer, uint32_t count, const EncodedJSValue* values)
{
    for (uint32_t i = 0; i < count; ++i)
        buffer.append(JSValue::decode(values[i]));
}

// A value: the name has it for good.
static void fillGlobalPropertyCache(VM& vm, Instance* instance, JSGlobalObject* globalObject, uint32_t cacheNumber, int64_t location, JSValue value = { })
{
    Structure* structure = globalObject->structure();
    if (value) {
        instance->typed->globalPropertyCaches()[cacheNumber - 1] = { structure->id().bits(), globalObject->globalLexicalBindingEpoch(), 0, JSValue::encode(value) };
        instance->didStoreRootInStatics(instance->typed->data.globalPropertyCachesOffset + (cacheNumber - 1) * sizeof(Typed::GlobalPropertyCache) + OBJECT_OFFSETOF(Typed::GlobalPropertyCache, value));
        return;
    }
    if (structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore()) {
        globalObject->flattenDictionaryObject(vm);
        return;
    }
    if (!structure->propertyAccessesAreCacheable())
        return;
    Typed::GlobalPropertyCache& cache = instance->typed->globalPropertyCaches()[cacheNumber - 1];
    cache = { structure->id().bits(), globalObject->globalLexicalBindingEpoch(), location, JSValue::encode(JSValue()) };
}

static int64_t locationInButterfly(PropertyOffset offset)
{
    return offsetInButterfly(offset) * static_cast<int64_t>(sizeof(EncodedJSValue));
}

enum class FreeName : uint8_t { Read, TypeOf, PropertyOfGlobalThis };

static JSValue getFree(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint32_t nameIndex, uint32_t cacheNumber, FreeName how)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (cacheNumber) {
        auto& cache = instance->typed->globalPropertyCaches()[cacheNumber - 1];
        if (cache.location && cache.structureID == globalObject->structureID().bits() && cache.globalLexicalBindingEpoch == globalObject->globalLexicalBindingEpoch())
            return JSValue::decode(*std::bit_cast<EncodedJSValue*>((cache.location < 0 ? std::bit_cast<int64_t>(globalObject->butterfly()) : 0) + cache.location));
    }
    const Identifier& name = nameAt(instance, nameIndex);
    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    PropertySlot lexicalSlot(lexical, PropertySlot::InternalMethodType::Get);
    if (how != FreeName::PropertyOfGlobalThis && JSGlobalLexicalEnvironment::getOwnPropertySlot(lexical, globalObject, name, lexicalSlot)) {
        JSValue value = lexicalSlot.getValue(globalObject, name);
        if (value == jsTDZValue())
            throwException(globalObject, scope, createTDZError(globalObject, name));
        else if (auto entry = lexical->symbolTable()->get(name.impl()); cacheNumber && !entry.isNull())
            fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, std::bit_cast<int64_t>(lexical->variableAt(entry.scopeOffset()).slot()), entry.isReadOnly() ? value : JSValue());
        return value;
    }
    PropertySlot slot(how == FreeName::PropertyOfGlobalThis ? globalObject->globalThis() : globalObject, PropertySlot::InternalMethodType::Get);
    bool found = globalObject->getPropertySlot(globalObject, name, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found) {
        if (how == FreeName::Read)
            throwException(globalObject, scope, createUndefinedVariableError(globalObject, name));
        return jsUndefined();
    }
    JSValue value = slot.getValue(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (!cacheNumber || slot.slotBase() != globalObject)
        return value;
    constexpr unsigned fixed = PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete;
    bool isEnginePrivateName = instance->typed->data.stringFacts(nameIndex) & Typed::Data::stringIsEnginePrivateName;
    bool isFixed = (slot.attributes() & fixed) == fixed || isEnginePrivateName;
    if (slot.isCacheableValue()) {
        if (isFixed || instance->typed->trustGlobalProperty(vm, globalObject, cacheNumber, name.impl(), value))
            fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, 0, value);
        else if (isOutOfLineOffset(slot.cachedOffset()))
            fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, locationInButterfly(slot.cachedOffset()));
    } else if (slot.isValue()) {
        if (auto entry = globalObject->symbolTable()->get(name.impl()); !entry.isNull())
            fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, std::bit_cast<int64_t>(globalObject->variableAt(entry.scopeOffset()).slot()), entry.isReadOnly() || isEnginePrivateName ? value : JSValue());
    } else if (slot.isCustom() && isFixed && slot.attributes() & PropertyAttribute::CustomValue)
        fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, 0, value);
    return value;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetFree, EncodedJSValue, (Instance* instance, uint32_t name, uint32_t cacheNumber))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(getFree(instance, globalObject, vm, name, cacheNumber, FreeName::Read)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedValidateGlobalForGood, void, (Instance* instance, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue current = getFree(instance, globalObject, vm, name, 0, FreeName::Read);
    RELEASE_ASSERT_WITH_MESSAGE(!scope.exception() && current == JSValue::decode(value), "AOT: a free name that was taken to have one value for good has another");
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetFreeForTypeOf, EncodedJSValue, (Instance* instance, uint32_t name, uint32_t cacheNumber))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(getFree(instance, globalObject, vm, name, cacheNumber, FreeName::TypeOf)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetPropertyOfGlobalThis, EncodedJSValue, (Instance* instance, uint32_t name, uint32_t cacheNumber))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(getFree(instance, globalObject, vm, name, cacheNumber, FreeName::PropertyOfGlobalThis)));
}

template<bool isStrict>
static ALWAYS_INLINE OperationReturnType<void> setFree(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint32_t name, uint32_t cacheNumber, EncodedJSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    const Identifier& identifier = nameAt(instance, name);
    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    PropertySlot lexicalSlot(lexical, PropertySlot::InternalMethodType::Get);
    if (JSGlobalLexicalEnvironment::getOwnPropertySlot(lexical, globalObject, identifier, lexicalSlot)) {
        if (lexicalSlot.getValue(globalObject, identifier) == jsTDZValue()) {
            throwException(globalObject, scope, createTDZError(globalObject, identifier));
            OPERATION_RETURN(scope);
        }
        PutPropertySlot slot(lexical, true);
        JSGlobalLexicalEnvironment::put(lexical, globalObject, identifier, JSValue::decode(value), slot);
        OPERATION_RETURN(scope);
    }
    // In strict code an assignment does not create a global variable.
    bool exists = !isStrict || globalObject->hasProperty(globalObject, identifier);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!exists) {
        throwException(globalObject, scope, createUndefinedVariableError(globalObject, identifier));
        OPERATION_RETURN(scope);
    }
    Structure* structure = globalObject->structure();
    PutPropertySlot slot(globalObject, isStrict);
    globalObject->methodTable()->put(globalObject, globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (cacheNumber && slot.isCacheablePut() && slot.type() == PutPropertySlot::ExistingProperty && slot.base() == globalObject && globalObject->structure() == structure && isOutOfLineOffset(slot.cachedOffset())) {
        if (structure->propertyAccessesAreCacheable())
            structure->didCachePropertyReplacement(vm, slot.cachedOffset());
        fillGlobalPropertyCache(vm, instance, globalObject, cacheNumber, locationInButterfly(slot.cachedOffset()));
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetFree, void, (Instance* instance, uint32_t name, uint32_t cacheNumber, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    scope.release();
    return setFree<true>(instance, globalObject, vm, name, cacheNumber, value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetFreeSloppy, void, (Instance* instance, uint32_t name, uint32_t cacheNumber, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    scope.release();
    return setFree<false>(instance, globalObject, vm, name, cacheNumber, value);
}

using PrivateElement = Typed::Object::PrivateElement;

// PrivateGet.
static JSValue privateGet(JSGlobalObject* globalObject, VM& vm, JSValue base, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = base.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    PrivateElement element = Typed::Object::findPrivateElement(vm, object, name);
    switch (element.kind) {
    case PrivateElement::Kind::None:
        throwException(globalObject, scope, createInvalidPrivateNameError(globalObject));
        return { };
    case PrivateElement::Kind::Field:
    case PrivateElement::Kind::Method:
        return element.value;
    case PrivateElement::Kind::Accessor:
        if (!element.getter) {
            throwTypeError(globalObject, scope, "Trying to access an undefined private getter"_s);
            return { };
        }
        RELEASE_AND_RETURN(scope, call(globalObject, element.getter, JSC::getCallData(element.getter), base, ArgList()));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// PrivateSet.
static void privateSet(JSGlobalObject* globalObject, VM& vm, JSValue base, PropertyName name, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = base.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    PrivateElement element = Typed::Object::findPrivateElement(vm, object, name);
    switch (element.kind) {
    case PrivateElement::Kind::None:
        throwException(globalObject, scope, createInvalidPrivateNameError(globalObject));
        return;
    case PrivateElement::Kind::Field: {
        PutPropertySlot slot(object, true);
        scope.release();
        if (dynamicDowncast<Typed::Object>(object))
            Typed::Object::put(object, globalObject, name, value, slot);
        else
            object->setPrivateField(globalObject, name, value, slot);
        return;
    }
    case PrivateElement::Kind::Method:
        throwTypeError(globalObject, scope, "Trying to assign to a private method"_s);
        return;
    case PrivateElement::Kind::Accessor: {
        if (!element.setter) {
            throwTypeError(globalObject, scope, "Trying to access an undefined private setter"_s);
            return;
        }
        MarkedArgumentBuffer arguments;
        arguments.append(value);
        scope.release();
        call(globalObject, element.setter, JSC::getCallData(element.setter), base, arguments);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// `#name in object`
static bool privateHas(JSGlobalObject* globalObject, VM& vm, JSValue base, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!base.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, base));
        return false;
    }
    return Typed::Object::findPrivateElement(vm, asObject(base), name).kind != PrivateElement::Kind::None;
}

// PrivateFieldAdd, and with attributes PrivateMethodOrAccessorAdd, where the value of an accessor is a GetterSetter.
static void privateAdd(JSGlobalObject* globalObject, VM& vm, JSObject* object, PropertyName name, JSValue value, unsigned attributes)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isCompiled = !!dynamicDowncast<Typed::Object>(object);
    if (!isCompiled && !attributes) {
        PutPropertySlot slot(object, true);
        RELEASE_AND_RETURN(scope, object->definePrivateField(globalObject, name, value, slot));
    }
    if (Typed::Object::findPrivateElement(vm, object, name).kind != PrivateElement::Kind::None) {
        throwException(globalObject, scope, attributes ? createReinstallPrivateMethodError(globalObject) : createRedefinedPrivateNameError(globalObject));
        return;
    }
    if (isCompiled) {
        PropertyDescriptor descriptor;
        descriptor.setDescriptor(value, attributes);
        scope.release();
        Typed::Object::defineOwnProperty(object, globalObject, name, descriptor, true);
        return;
    }
    if (attributes & PropertyAttribute::Accessor)
        object->putDirectNonIndexAccessor(vm, name, uncheckedDowncast<GetterSetter>(value), attributes);
    else
        object->putDirect(vm, name, value, attributes);
}

// `#name` of a class, which is no property. A private name of the engine's is a private symbol as well, and a property key like any other.
static ALWAYS_INLINE bool isPrivateNameOfClass(Instance* instance, const Identifier& identifier, uint32_t name)
{
    return identifier.isPrivateName() && instance->typed->data.stringFacts(name) & Typed::Data::stringIsPrivateName;
}

// What the tables of keyed accesses may say of the object's structure and the name. See Instance::keyedReads.
static void cachePrivateElement(Instance* instance, VM& vm, JSValue base, Symbol* name)
{
    if (!base.isObject())
        return;
    Structure* structure = asObject(base)->structure();
    Slot& read = instance->keyedReadFor(structure->id(), name).slot;
    Slot& store = instance->keyedStoreFor(structure->id(), name);
    unsigned attributes = 0;
    PropertyOffset offset = structure->compiledLayout() ? invalidOffset : structure->get(vm, PropertyName(name->privateName()), attributes);
    bool isValue = isValidOffset(offset) && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue);
    cacheKeyedAccess(read, structure, name, isValue ? std::optional(offset) : std::nullopt);
    if (!isValidOffset(offset) && !structure->compiledLayout() && structure->propertyAccessesAreCacheable() && !structure->isDictionary())
        read.offset = Instance::keyIsAbsent;
    bool isField = isValue && !attributes;
    cacheKeyedAccess(store, structure, name, isField ? std::optional(offset) : std::nullopt);
    if (isField && !(store.offset & Slot::isGetter))
        structure->didCachePropertyReplacement(vm, offset);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateGet, EncodedJSValue, (Instance* instance, EncodedJSValue object, EncodedJSValue name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    cachePrivateElement(instance, vm, JSValue::decode(object), symbol);
    OPERATION_RETURN(scope, JSValue::encode(privateGet(globalObject, vm, JSValue::decode(object), symbol->privateName())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateSet, void, (Instance* instance, EncodedJSValue object, EncodedJSValue name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    cachePrivateElement(instance, vm, JSValue::decode(object), symbol);
    privateSet(globalObject, vm, JSValue::decode(object), symbol->privateName(), JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateHas, EncodedJSValue, (Instance* instance, EncodedJSValue object, EncodedJSValue name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    cachePrivateElement(instance, vm, JSValue::decode(object), symbol);
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(privateHas(globalObject, vm, JSValue::decode(object), symbol->privateName()))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPrivateFieldAdd, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Symbol* symbol = asSymbol(JSValue::decode(name));
    JSObject* object = asObject(JSValue::decode(encodedObject));
    Structure* before = object->structure();
    privateAdd(globalObject, vm, object, symbol->privateName(), JSValue::decode(value), 0);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    Structure* after = object->structure();
    auto isPlain = [](Structure* structure) {
        return !structure->compiledLayout() && structure->propertyAccessesAreCacheable() && !structure->isDictionary() && !structure->mayBePrototype();
    };
    if (!isPlain(before) || !isPlain(after) || after->previousID() != before || before->outOfLineCapacity() != after->outOfLineCapacity() || before->typeInfoBlob() != after->typeInfoBlob())
        OPERATION_RETURN(scope);
    unsigned attributes;
    auto& entry = instance->keyedAdditionFor(before->id(), symbol);
    cacheKeyedAccess(entry.slot, before, symbol, after->get(vm, PropertyName(symbol->privateName()), attributes));
    entry.newStructureID = after->id();
    entry.epoch = 0;
    entry.allocatesInitialStorage = 0;
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByName, EncodedJSValue, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (isPrivateNameOfClass(instance, identifier, name)) [[unlikely]]
        OPERATION_RETURN(scope, JSValue::encode(privateGet(globalObject, vm, JSValue::decode(base), identifier)));
    // In the words of every other way to read it.
    if (JSValue::decode(base).isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, State::nullishMessage(JSValue::decode(base), identifier.string(), false));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(base).get(globalObject, identifier)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPutLazyClosure, void, (Instance* instance, EncodedJSValue encodedBase, uint32_t function, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    instance->typed->putLazyClosure(globalObject, JSValue::decode(encodedBase), nameAt(instance, name), function);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetByName, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (isPrivateNameOfClass(instance, identifier, name)) [[unlikely]] {
        privateSet(globalObject, vm, JSValue::decode(base), identifier, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    if (JSValue::decode(base).isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, State::nullishMessage(JSValue::decode(base), identifier.string(), true));
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(JSValue::decode(base), true);
    JSValue::decode(base).put(globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetByNameSloppy, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (JSValue::decode(base).isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, State::nullishMessage(JSValue::decode(base), identifier.string(), true));
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(JSValue::decode(base), false);
    JSValue::decode(base).put(globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetStaticField, EncodedJSValue, (Instance* instance, uint32_t ofClass, uint32_t field))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    const Typed::LayoutData& layout = *state.classWithIndex(ofClass);
    JSValue classObject = state.closure(vm, layout.classData().classConstructor);
    OPERATION_RETURN(scope, JSValue::encode(classObject.get(globalObject, state.name(vm, layout.staticFields()[field].name))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetStaticField, void, (Instance* instance, uint32_t ofClass, uint32_t field, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    const Typed::LayoutData& layout = *state.classWithIndex(ofClass);
    JSValue classObject = state.closure(vm, layout.classData().classConstructor);
    PutPropertySlot slot(classObject, true);
    classObject.put(globalObject, state.name(vm, layout.staticFields()[field].name), JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetByNameInRowOfThis, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (tryStoreIntoRowOfThis(globalObject, nullptr, nullptr, JSValue::decode(base), identifier, JSValue::decode(value)))
        OPERATION_RETURN(scope);
    if (JSValue::decode(base).isUndefinedOrNull()) [[unlikely]] {
        throwTypeError(globalObject, scope, State::nullishMessage(JSValue::decode(base), identifier.string(), true));
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(JSValue::decode(base), true);
    JSValue::decode(base).put(globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

// CreateDataPropertyOrThrow. On an ordinary object nothing can be in its way where the program does this.
static void createDataProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue value)
{
    // A put only where nobody can tell it from a definition.
    Structure* structure = object->structure();
    unsigned attributes = 0;
    if (object->type() == FinalObjectType && !structure->compiledLayout() && structure->isStructureExtensible() && !parseIndex(name) && (structure->get(globalObject->vm(), name, attributes) == invalidOffset || !attributes))
        object->putDirect(globalObject->vm(), name, value);
    else
        object->createDataProperty(globalObject, name, value, true);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineByName, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    // A field that is `#name` is there from when the object is made.
    if (isPrivateNameOfClass(instance, identifier, name)) [[unlikely]] {
        privateSet(globalObject, vm, JSValue::decode(base), identifier, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    asObject(JSValue::decode(base))->createDataProperty(globalObject, identifier, JSValue::decode(value), true);
    OPERATION_RETURN(scope);
}

// As objectConstructorDefineProperty(), behind the key. False: thrown.
static bool defineOwnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue valueOrGetter, JSValue setter, uint32_t literal)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyDescriptor descriptor;
    if (globalObject->propertyDescriptorFastPathWatchpointSet().state() == ClearWatchpoint)
        globalObject->tryInstallPropertyDescriptorFastPathWatchpoint();
    if (globalObject->propertyDescriptorFastPathWatchpointSet().isStillValid() && globalObject->objectPrototypeChainIsSane()) [[likely]] {
        if (literal & DescriptorLiteral::hasEnumerable)
            descriptor.setEnumerable(literal & DescriptorLiteral::isEnumerable);
        if (literal & DescriptorLiteral::hasConfigurable)
            descriptor.setConfigurable(literal & DescriptorLiteral::isConfigurable);
        if (literal & DescriptorLiteral::hasValue)
            descriptor.setValue(valueOrGetter);
        if (literal & DescriptorLiteral::hasWritable)
            descriptor.setWritable(literal & DescriptorLiteral::isWritable);
        if (literal & DescriptorLiteral::hasGet) {
            if (!valueOrGetter.isUndefined() && !valueOrGetter.isCallable()) {
                throwTypeError(globalObject, scope, "Getter must be a function."_s);
                return false;
            }
            descriptor.setGetter(valueOrGetter);
        }
        if (literal & DescriptorLiteral::hasSet) {
            if (!setter.isUndefined() && !setter.isCallable()) {
                throwTypeError(globalObject, scope, "Setter must be a function."_s);
                return false;
            }
            descriptor.setSetter(setter);
        }
    } else {
        // Object.prototype may have what the literal has not: the literal is made after all, and asked.
        JSObject* description = constructEmptyObject(globalObject);
        if (literal & DescriptorLiteral::hasValue)
            description->putDirect(vm, vm.propertyNames->value, valueOrGetter);
        if (literal & DescriptorLiteral::hasWritable)
            description->putDirect(vm, vm.propertyNames->writable, jsBoolean(literal & DescriptorLiteral::isWritable));
        if (literal & DescriptorLiteral::hasEnumerable)
            description->putDirect(vm, vm.propertyNames->enumerable, jsBoolean(literal & DescriptorLiteral::isEnumerable));
        if (literal & DescriptorLiteral::hasConfigurable)
            description->putDirect(vm, vm.propertyNames->configurable, jsBoolean(literal & DescriptorLiteral::isConfigurable));
        if (literal & DescriptorLiteral::hasGet)
            description->putDirect(vm, vm.propertyNames->get, valueOrGetter);
        if (literal & DescriptorLiteral::hasSet)
            description->putDirect(vm, vm.propertyNames->set, setter);
        bool isDescriptor = toPropertyDescriptor(globalObject, description, descriptor);
        EXCEPTION_ASSERT(!scope.exception() == isDescriptor);
        if (!isDescriptor)
            return false;
    }
    object->methodTable()->defineOwnProperty(object, globalObject, name, descriptor, true);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineOwnProperty, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue key, EncodedJSValue valueOrGetter, EncodedJSValue setter, uint32_t literal))
{
    AOT_OPERATION_BEGIN(instance);
    JSValue object = JSValue::decode(encodedObject);
    if (!object.isObject()) {
        throwTypeError(globalObject, scope, "Properties can only be defined on Objects."_s);
        OPERATION_RETURN(scope);
    }
    auto name = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    defineOwnProperty(globalObject, asObject(object), name, JSValue::decode(valueOrGetter), JSValue::decode(setter), literal);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineOwnPropertyAtSite, void, (Instance* instance, EncodedJSValue encodedObject, EncodedJSValue value, uint32_t identifier, Slot* cache, uint32_t literal))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    JSValue base = JSValue::decode(encodedObject);
    if (!base.isObject()) {
        throwTypeError(globalObject, scope, "Properties can only be defined on Objects."_s);
        OPERATION_RETURN(scope);
    }
    JSObject* object = asObject(base);
    const Identifier& name = identifierAt(instance, callFrame, identifier);
    Structure* before = object->structure();
    bool isDefined = defineOwnProperty(globalObject, object, name, JSValue::decode(value), jsUndefined(), literal);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isDefined || !globalObject->propertyDescriptorFastPathWatchpointSet().isStillValid())
        OPERATION_RETURN(scope);
    Vector<UniquedStringImpl*, 6> absent;
    auto isAbsentUnless = [&](uint32_t has, const Identifier& nameOfDescriptor) {
        if (!(literal & has))
            absent.append(nameOfDescriptor.impl());
    };
    isAbsentUnless(DescriptorLiteral::hasValue, vm.propertyNames->value);
    isAbsentUnless(DescriptorLiteral::hasWritable, vm.propertyNames->writable);
    isAbsentUnless(DescriptorLiteral::hasEnumerable, vm.propertyNames->enumerable);
    isAbsentUnless(DescriptorLiteral::hasConfigurable, vm.propertyNames->configurable);
    isAbsentUnless(DescriptorLiteral::hasGet, vm.propertyNames->get);
    isAbsentUnless(DescriptorLiteral::hasSet, vm.propertyNames->set);
    cacheDefinedProperty(globalObject, callerData(instance, callFrame), cache, object, before, name.impl(), absent.span());
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasName, size_t, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(base);
    const Identifier& identifier = nameAt(instance, name);
    if (isPrivateNameOfClass(instance, identifier, name)) [[unlikely]]
        OPERATION_RETURN(scope, privateHas(globalObject, vm, value, identifier));
    if (!value.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, value));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, asObject(value)->hasProperty(globalObject, identifier));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDeleteByName, size_t, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    bool deleted = JSCell::deleteProperty(object, globalObject, nameAt(instance, name));
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!deleted) {
        if (object->isLockedBuiltin())
            object->noteRefusalByLock(vm, nameAt(instance, name).impl());
        JSValue held;
        throwTypeError(globalObject, scope, object->isLockedBuiltin() && object->isRefusedByLockAlone(vm, nameAt(instance, name).impl(), held) ? withAOTErrorNote(UnableToDeletePropertyError) : String(UnableToDeletePropertyError));
    }
    OPERATION_RETURN(scope, deleted);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByKey, EncodedJSValue, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue baseValue = JSValue::decode(base);
    JSValue keyValue = JSValue::decode(key);
    if (std::optional<uint32_t> index = keyValue.tryGetAsUint32Index())
        OPERATION_RETURN(scope, JSValue::encode(baseValue.get(globalObject, *index)));
    baseValue.requireObjectCoercible(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto property = keyValue.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(baseValue.get(globalObject, property)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetByKey, void, (Instance* instance, EncodedJSValue base, EncodedJSValue key, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue baseValue = JSValue::decode(base);
    JSValue keyValue = JSValue::decode(key);
    if (std::optional<uint32_t> index = keyValue.tryGetAsUint32Index()) {
        if (baseValue.isCell() && baseValue.asCell()->type() == ArrayType)
            putIndexOfArray(globalObject, uncheckedDowncast<JSArray>(baseValue.asCell()), *index, JSValue::decode(value), true);
        else
            baseValue.putByIndex(globalObject, *index, JSValue::decode(value), true);
        OPERATION_RETURN(scope);
    }
    baseValue.requireObjectCoercible(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    auto property = keyValue.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PutPropertySlot slot(baseValue, true);
    baseValue.put(globalObject, property, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasKey, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(base);
    if (!value.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, value));
        OPERATION_RETURN(scope, 0);
    }
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, asObject(value)->hasProperty(globalObject, property));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedElementOrNothing, EncodedJSValue, (Instance* instance, EncodedJSValue base, int64_t index, EncodedJSValue ifAbsent))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(base);
    bool asksFirst = !JSValue::decode(ifAbsent);
    if (index >= 0 && index <= static_cast<int64_t>(MAX_ARRAY_INDEX)) {
        if (!asksFirst)
            OPERATION_RETURN(scope, JSValue::encode(value.get(globalObject, static_cast<unsigned>(index))));
        if (!value.isObject()) {
            throwException(globalObject, scope, createInvalidInParameterError(globalObject, value));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        bool has = asObject(value)->hasProperty(globalObject, static_cast<unsigned>(index));
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!has)
            OPERATION_RETURN(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(asObject(value)->get(globalObject, static_cast<unsigned>(index))));
    }
    Identifier name = Identifier::from(vm, static_cast<double>(index));
    if (!asksFirst)
        OPERATION_RETURN(scope, JSValue::encode(value.get(globalObject, name)));
    if (!value.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, value));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    bool has = asObject(value)->hasProperty(globalObject, name);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!has)
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(asObject(value)->get(globalObject, name)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDeleteByKey, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    bool deleted = JSCell::deleteProperty(object, globalObject, property);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!deleted) {
        if (object->isLockedBuiltin())
            object->noteRefusalByLock(vm, property.impl());
        JSValue held;
        throwTypeError(globalObject, scope, object->isLockedBuiltin() && object->isRefusedByLockAlone(vm, property.impl(), held) ? withAOTErrorNote(UnableToDeletePropertyError) : String(UnableToDeletePropertyError));
    }
    OPERATION_RETURN(scope, deleted);
}

static JSValue callValue(JSGlobalObject* globalObject, VM& vm, JSValue callee, JSValue thisValue, uint32_t count, const EncodedJSValue* values)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto callData = JSC::getCallData(callee);
    if (callData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, makeString(errorDescriptionForValue(globalObject, callee), " is not a function"_s));
        return { };
    }
    MarkedArgumentBuffer arguments;
    fill(arguments, count, values);
    RELEASE_AND_RETURN(scope, call(globalObject, callee, callData, thisValue, arguments));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCall, EncodedJSValue, (Instance* instance, EncodedJSValue callee, EncodedJSValue thisValue, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(callValue(globalObject, vm, JSValue::decode(callee), JSValue::decode(thisValue), count, values)));
}

static void throwNotAFunction(JSGlobalObject* globalObject, ThrowScope& scope, const Identifier& name, JSValue callee)
{
    throwTypeError(globalObject, scope, makeString(name.string(), " is not a function, it is "_s, State::describe(callee)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowNotAFunction, void, (Instance* instance, EncodedJSValue callee, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwNotAFunction(globalObject, scope, nameAt(instance, name), JSValue::decode(callee));
    OPERATION_RETURN(scope);
}

// From Stub::CallMethodRead: the name is the one the slot's site has.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowMethodIsNotAFunction, void, (Instance* instance, EncodedJSValue callee, uint32_t identifierOfSite))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwNotAFunction(globalObject, scope, identifierAt(instance, callFrame, identifierOfSite), JSValue::decode(callee));
    OPERATION_RETURN(scope);
}

static JSValue requireFunctionOfHost(JSGlobalObject* globalObject, Instance* instance)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = *instance->typed;
    if (!state.importMeta) {
        auto* importer = uncheckedDowncast<JSModuleRecord>(state.moduleRecord);
        state.importMeta = globalObject->moduleLoader()->createImportMetaProperties(globalObject, identifierToJSValue(vm, state.keyOfImporter(vm)), importer, nullptr);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, state.importMeta->get(globalObject, Identifier::fromString(vm, "require"_s)));
}

static JSValue requireFromHost(JSGlobalObject* globalObject, Instance* instance, JSValue specifier)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue require = requireFunctionOfHost(globalObject, instance);
    RETURN_IF_EXCEPTION(scope, { });
    auto callData = JSC::getCallData(require);
    if (callData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, withAOTErrorNote(makeString("Cannot find module '"_s, specifier.toWTFString(globalObject), "': require() is not available here"_s)));
        return { };
    }
    MarkedArgumentBuffer arguments;
    arguments.append(specifier);
    RELEASE_AND_RETURN(scope, call(globalObject, require, callData, jsUndefined(), arguments));
}

// What an ES module sees of `module.exports`, as in a bundle: `default`, and a live view of each own property.
static JSObject* toESM(JSGlobalObject* globalObject, Instance* instance, JSValue exported, bool isNodeMode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* source = exported.getObject();
    JSObject* result = constructEmptyObject(globalObject);
    if (source) {
        JSValue prototype = source->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        result = prototype.isObject() ? constructEmptyObject(globalObject, asObject(prototype)) : constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
        // One Structure, not one for each name. What the properties are read from is the first of them: see State::liveBindingAccessor().
        result->convertToDictionary(vm);
        result->putDirect(vm, vm.propertyNames->builtinNames().starNamespacePrivateName(), source, PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete | PropertyAttribute::DontEnum);
        RELEASE_ASSERT(!result->getDirectOffset(vm, vm.propertyNames->builtinNames().starNamespacePrivateName()));
    } else if (!exported.isUndefinedOrNull())
        result = constructEmptyObject(globalObject, exported.synthesizePrototype(globalObject));
    bool hasDefault = isNodeMode || !source;
    if (!hasDefault) {
        JSValue marker = source->get(globalObject, Identifier::fromString(vm, "__esModule"_s));
        RETURN_IF_EXCEPTION(scope, nullptr);
        hasDefault = !marker.toBoolean(globalObject);
        if (!hasDefault) {
            hasDefault = !source->hasOwnProperty(globalObject, vm.propertyNames->defaultKeyword);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    if (hasDefault)
        result->putDirect(vm, vm.propertyNames->defaultKeyword, exported);
    if (!source)
        return result;
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    source->methodTable()->getOwnPropertyNames(source, globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, nullptr);
    // The property is read from `module.exports` on each use.
    for (auto& name : names) {
        if (result->hasOwnProperty(globalObject, name))
            continue;
        if (!parseIndex(name)) {
            result->putDirectCustomAccessor(vm, name, instance->typed->liveBindingAccessor(vm), PropertyAttribute::CustomValue | PropertyAttribute::ReadOnly | PropertyAttribute::DontDelete);
            continue;
        }
        MarkedArgumentBuffer boundArguments;
        boundArguments.append(identifierToJSValue(vm, name));
        JSObject* getter = JSBoundFunction::create(vm, globalObject, instance->typed->liveBindingGetter(vm), source, boundArguments, 0, nullptr, SourceTaintedOrigin::Untainted);
        RETURN_IF_EXCEPTION(scope, nullptr);
        result->putDirectAccessor(globalObject, name, GetterSetter::create(vm, globalObject, getter, nullptr), PropertyAttribute::Accessor | PropertyAttribute::DontDelete);
    }
    return result;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFailModuleCycle, void, (Instance* instance, uint64_t failed, uint32_t numberOfRows, EncodedJSValue error))
{
    if (instance->moduleEvaluationDepth > 1)
        return;
    auto& data = instance->typed->data;
    const uint32_t* words = data.at<uint32_t>(data.moduleEvaluationsOffset);
    constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
    auto forEachRowCalledBy = [&](uint32_t row, auto&& functor) {
        uint32_t start = words[numberOfRows + row];
        for (uint32_t cursor = start + 2; cursor != words[start + 1]; cursor += 2) {
            if (words[cursor] != none)
                functor(words[cursor + 1]);
        }
    };
    auto rowsReachedBy = [&](auto&& forEachNext) {
        BitVector reached(numberOfRows);
        Vector<uint32_t, 16> worklist;
        reached.quickSet(failed);
        worklist.append(static_cast<uint32_t>(failed));
        while (!worklist.isEmpty()) {
            forEachNext(worklist.takeLast(), [&](uint32_t next) {
                if (!reached.quickSet(next))
                    worklist.append(next);
            });
        }
        return reached;
    };
    Vector<Vector<uint32_t, 2>> callers(numberOfRows);
    for (size_t row : rowsReachedBy(forEachRowCalledBy)) {
        forEachRowCalledBy(row, [&](uint32_t callee) {
            callers[callee].append(static_cast<uint32_t>(row));
        });
    }
    BitVector cycle = rowsReachedBy([&](uint32_t row, auto&& functor) {
        for (uint32_t caller : callers[row])
            functor(caller);
    });
    auto put = [&](uint32_t offset, EncodedJSValue value) {
        *reinterpret_cast<EncodedJSValue*>(instance->typedStatics + offset) = value;
        instance->didStoreRootInStatics(offset);
    };
    for (size_t row : cycle) {
        uint32_t errorAt = words[words[numberOfRows + row]];
        if (errorAt == none || *reinterpret_cast<EncodedJSValue*>(instance->typedStatics + words[row]) != JSValue::encode(jsBoolean(true)))
            continue;
        put(words[row], JSValue::encode(jsBoolean(false)));
        put(errorAt, error);
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetExternalModule, EncodedJSValue, (Instance* instance, uint32_t specifier))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    AbstractModuleRecord* importer = instance->typed->moduleRecord;
    const Identifier& name = nameAt(instance, specifier);
    AbstractModuleRecord* imported = importer ? importer->hostResolveImportedModule(globalObject, name, ScriptFetchParameters::Type::JavaScript) : nullptr;
    if (importer && !imported) {
        // A bundler may have written a built-in module the other way.
        String other = name.string().startsWith("node:"_s) ? name.string().string().substring(5) : makeString("node:"_s, name.string());
        imported = importer->hostResolveImportedModule(globalObject, Identifier::fromString(vm, other), ScriptFetchParameters::Type::JavaScript);
    }
    if (imported)
        OPERATION_RETURN(scope, JSValue::encode(imported->getModuleNamespace(globalObject)));
    JSModuleLoader* loader = importer ? importer->moduleLoader() : globalObject->moduleLoader();
    Identifier key = loader->resolve(globalObject, name, instance->typed->keyOfImporter(vm), nullptr, true);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    bool isEvaluating = false;
    auto recordThatHasRun = [&]() -> AbstractModuleRecord* {
        ModuleRegistryEntry* entry = loader->registryEntry(key);
        AbstractModuleRecord* record = entry ? entry->record() : nullptr;
        if (!record)
            return nullptr;
        if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record)) {
            isEvaluating = cyclic->status() == CyclicModuleRecord::Status::Evaluating;
            bool hasRun = cyclic->status() == CyclicModuleRecord::Status::Evaluated || (isEvaluating && !cyclic->hasTLA());
            return hasRun && !cyclic->evaluationError() ? record : nullptr;
        }
        return record->moduleEnvironmentMayBeNull() ? record : nullptr;
    };
    imported = recordThatHasRun();
    if (!imported && !isEvaluating) {
        JSPromise* promise = loader->loadModuleSync(globalObject, key, nullptr, nullptr);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        promise->markAsHandled();
        if (promise->status() == JSPromise::Status::Rejected) {
            throwException(globalObject, scope, promise->result());
            OPERATION_RETURN(scope, encodedJSValue());
        }
        imported = recordThatHasRun();
    }
    if (!imported) {
        throwTypeError(globalObject, scope, withAOTErrorNote(makeString("The module \""_s, key.string(), "\" is not compiled with the program and cannot be loaded synchronously: use \"await import()\" instead"_s)));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(imported->getModuleNamespace(globalObject)));
}

static constexpr unsigned attributesOfCalleeOfArguments = PropertyAttribute::DontDelete | PropertyAttribute::DontEnum | PropertyAttribute::Accessor;

static ClonedArguments* tryCreateArgumentsAtOnce(VM& vm, JSGlobalObject* globalObject, JSArray* list, unsigned length)
{
    IndexingType type = list->indexingType();
    if ((!hasContiguous(type) && !hasInt32(type)) || containsHole(list->butterfly()->contiguous().data(), length))
        return nullptr;
    PropertyOffset offsetOfCallee;
    PropertyOffset offsetOfIterator;
    Structure* structure = Structure::addPropertyTransitionToExistingStructure(globalObject->clonedArgumentsStructure(), vm.propertyNames->callee, attributesOfCalleeOfArguments, offsetOfCallee);
    if (structure)
        structure = Structure::addPropertyTransitionToExistingStructure(structure, vm.propertyNames->iteratorSymbol, static_cast<unsigned>(PropertyAttribute::DontEnum), offsetOfIterator);
    if (!structure || !hasContiguous(structure->indexingType()) || structure->mayInterceptIndexedAccesses() || structure->storedPrototypeObject()->needsSlowPutIndexing())
        return nullptr;
    JSValue callee = globalObject->throwTypeErrorArgumentsCalleeGetterSetter();
    JSValue iterator = globalObject->arrayProtoValuesFunction();
    ClonedArguments* result = ClonedArguments::createEmpty(vm, nullptr, structure, nullptr, length, nullptr);
    if (!result)
        return nullptr;
    gcSafeMemcpy(result->butterfly()->contiguous().data(), list->butterfly()->contiguous().data(), length * sizeof(JSValue));
    result->putDirectOffset(vm, offsetOfCallee, callee);
    result->putDirectOffset(vm, offsetOfIterator, iterator);
    return result;
}

// The pending error of a disposable stack. A later error suppresses the earlier one.
static void recordDisposeError(JSGlobalObject* globalObject, JSArray* stack, JSValue error)
{
    VM& vm = globalObject->vm();
    Identifier hasErrorName = Identifier::fromString(vm, "hasError"_s);
    Identifier errorName = Identifier::fromString(vm, "error"_s);
    JSValue hasError = stack->getDirect(vm, hasErrorName);
    if (hasError && hasError.isTrue()) {
        JSValue suppressed = stack->getDirect(vm, errorName);
        JSObject* wrapped = ErrorInstance::create(vm, globalObject->errorStructure(ErrorType::SuppressedError), String(), JSValue(), nullptr, TypeNothing, ErrorType::SuppressedError, false);
        wrapped->putDirect(vm, vm.propertyNames->error, error, static_cast<unsigned>(PropertyAttribute::DontEnum));
        wrapped->putDirect(vm, vm.propertyNames->suppressed, suppressed, static_cast<unsigned>(PropertyAttribute::DontEnum));
        error = wrapped;
    }
    stack->putDirect(vm, hasErrorName, jsBoolean(true));
    stack->putDirect(vm, errorName, error);
}

static NEVER_INLINE JSValue disposeNextResource(JSGlobalObject* globalObject, JSArray* stack)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr int32_t needsAwait = 1;
    constexpr int32_t hasAwaited = 2;
    unsigned length = stack->length();
    int32_t awaits = 0;
    if (length % 3) {
        awaits = stack->pop(globalObject).asInt32();
        RETURN_IF_EXCEPTION(scope, { });
        --length;
    }
    auto leaveWith = [&](int32_t noted, JSValue result) {
        if (noted)
            stack->push(globalObject, jsNumber(noted));
        return result;
    };
    if (awaits == needsAwait) {
        JSValue kindOfNext = length ? stack->getIndex(globalObject, length - 1) : jsBoolean(false);
        RETURN_IF_EXCEPTION(scope, { });
        if (kindOfNext.isFalse())
            return jsUndefined();
    }
    if (!length)
        return stack;
    JSValue kind = stack->pop(globalObject);
    JSValue method = stack->pop(globalObject);
    JSValue resource = stack->pop(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (method.isUndefined())
        return leaveWith(awaits | needsAwait, jsNull());
    bool isPlainMethodUnderAwait = kind.isNumber();
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue result = call(globalObject, method, JSC::getCallData(method), resource, ArgList());
    if (Exception* exception = catchScope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        if (!catchScope.clearExceptionExceptTermination())
            return { };
        recordDisposeError(globalObject, stack, error);
        if (!isPlainMethodUnderAwait)
            return leaveWith(awaits, jsNull());
    }
    if (kind.isFalse())
        return leaveWith(awaits, jsNull());
    return leaveWith(awaits | hasAwaited, isPlainMethodUnderAwait || result.isNull() ? jsUndefined() : result);
}

static void defineAccessor(JSGlobalObject* globalObject, JSObject* object, PropertyName property, JSValue function, bool isEnumerable, bool isGetter)
{
    PropertyDescriptor descriptor;
    if (isGetter)
        descriptor.setGetter(function);
    else
        descriptor.setSetter(function);
    descriptor.setEnumerable(isEnumerable);
    descriptor.setConfigurable(true);
    object->methodTable()->defineOwnProperty(object, globalObject, property, descriptor, true);
}

static void defineAccessor(JSGlobalObject* globalObject, JSValue object, JSValue key, JSValue function, bool isEnumerable, bool isGetter, JSValue position)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto property = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    defineAccessor(globalObject, asObject(object), property, function, isEnumerable, isGetter);
    RETURN_IF_EXCEPTION(scope, void());
    if (auto* compiled = dynamicDowncast<Typed::Object>(object); compiled && position.isUInt32())
        compiled->placeExtraKey(compiled->state(), property, position.asUInt32());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineAccessors, void, (Instance* instance, JSCell* object, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Vector<std::pair<UniquedStringImpl*, uint32_t>, 16> namesAndPositions;
    for (uint32_t i = 0; i < count; ++i) {
        auto value = [&](unsigned which) {
            return JSValue::decode(values[4 * i + which]);
        };
        const Identifier& name = nameAt(instance, value(0).asUInt32());
        defineAccessor(globalObject, asObject(object), name, value(1), value(2).asInt32() & 2, value(2).asInt32() & 1);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        namesAndPositions.append({ name.impl(), value(3).asUInt32() });
    }
    if (auto* compiled = dynamicDowncast<Typed::Object>(object))
        compiled->placeExtraKeys(compiled->state(), namesAndPositions.span());
    OPERATION_RETURN(scope);
}

static JSValue constructWith(JSGlobalObject* globalObject, VM& vm, JSValue callee, JSCell* cell, uint32_t count, const EncodedJSValue* values)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer arguments;
    fill(arguments, count, values);
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    // Only an ordinary function of the program that declares what `this` is takes the object that was made for it. The object inherits from
    // its `prototype`: a Typed::PrototypeObject, or the layouts that are of its `this` type say that they may inherit from anything. An
    // object of any other layout its generic entrypoint turns away. One that it would leave with a field that cannot say "absent" it makes itself.
    auto* closure = dynamicDowncast<Typed::Closure>(callee);
    if (!closure || !closure->makesLaidOutObjects() || closure->structure()->aotInstance()->typed->refusesFreshObject(closure->functionIndex(), uncheckedDowncast<Typed::Object>(cell)->shape())) {
        auto constructData = JSC::getConstructData(callee);
        if (constructData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, makeString(State::describe(callee), " is not a constructor"_s));
            return { };
        }
        RELEASE_AND_RETURN(scope, construct(globalObject, callee, constructData, arguments));
    }
    closure->structure()->aotInstance()->typed->makeTheOneThatConstructs(closure);
    // OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%").
    JSValue prototype = closure->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    object->setPrototypeAtBirth(vm, prototype.isObject() ? asObject(prototype) : closure->realm()->objectPrototype());
    JSValue result = closure->callAsConstructor(globalObject, object, closure, arguments);
    RETURN_IF_EXCEPTION(scope, { });
    return result.isObject() ? result : JSValue(object);
}

static NEVER_INLINE bool appendBoxedDoubles(JSGlobalObject* globalObject, VM& vm, JSArray* target, const double* numbers, unsigned length)
{
    Vector<EncodedJSValue, 16> boxed(length, [&](size_t i) {
        return JSValue::encode(JSValue(JSValue::EncodeAsDouble, numbers[i]));
    });
    return target->appendMemcpy(globalObject, vm, target->length(), ArrayWithContiguous, boxed.span());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConstructWith, EncodedJSValue, (Instance* instance, EncodedJSValue callee, JSCell* object, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(constructWith(globalObject, vm, JSValue::decode(callee), object, count, values)));
}

static const char* nameOfElementsForCounters(JSArray* array)
{
    if (array->type() == DerivedArrayType)
        return "an heir of Array";
    if (array->classInfo() != JSArray::info())
        return array->classInfo()->className.characters();
    IndexingType type = array->indexingType();
    bool isShared = isCopyOnWrite(array->indexingMode());
    if (hasInt32(type))
        return isShared ? "int32, copy on write" : "int32";
    if (hasDouble(type))
        return isShared ? "doubles, copy on write" : "doubles";
    if (hasContiguous(type))
        return isShared ? "contiguous, copy on write" : "contiguous";
    return hasUndecided(type) ? "undecided" : hasAnyArrayStorage(type) ? "array storage" : "no elements";
}

static const char* whyArrayIsIteratedForCounters(JSArray* source)
{
    if (source->type() == DerivedArrayType)
        return "an heir of Array";
    if (source->classInfo() != JSArray::info())
        return "another class";
    if (!source->isIteratorProtocolFastAndNonObservable())
        return "the iteration can be observed";
    IndexingType type = source->indexingType();
    unsigned length = source->length();
    if (hasDouble(type))
        return containsHole(source->butterfly()->contiguousDouble().data(), length) ? "a hole or a NaN" : "the copy was declined";
    if (hasInt32(type) || hasContiguous(type))
        return containsHole(source->butterfly()->contiguous().data(), length) ? "a hole" : "the copy was declined";
    return "elements of another kind";
}

static NEVER_INLINE void countSpreadForCounters(Instance* instance, JSArray* target, JSValue source)
{
    auto* list = dynamicDowncast<JSArray>(source);
    countOperationNamed(instance, "AppendSpread:source", list ? nameOfElementsForCounters(list) : source.isCell() ? source.asCell()->classInfo()->className.characters() : "primitive");
    countOperationNamed(instance, "AppendSpread:target", nameOfElementsForCounters(target));
    if (!list)
        return;
    unsigned length = list->length();
    countOperationNamed(instance, "AppendSpread:length", !length ? "0" : length == 1 ? "1" : length <= 4 ? "2 to 4" : length <= 16 ? "5 to 16" : length <= 64 ? "17 to 64" : "over 64");
}

// False: nothing has been appended, or an exception was thrown.
static bool tryAppendArray(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSValue iterable)
{
    auto* source = dynamicDowncast<JSArray>(iterable);
    if (!source || source->classInfo() != JSArray::info() || !source->isIteratorProtocolFastAndNonObservable())
        return false;
    // The iteration yields `undefined` for a hole.
    IndexingType type = source->indexingType();
    unsigned length = source->length();
    if (hasDouble(type)) {
        if (containsHole(source->butterfly()->contiguousDouble().data(), length))
            return false;
        if (hasContiguous(target->indexingType()) && length < MIN_SPARSE_ARRAY_INDEX && target->length() < MIN_SPARSE_ARRAY_INDEX - length)
            return appendBoxedDoubles(globalObject, vm, target, source->butterfly()->contiguousDouble().data(), length);
    } else if (hasInt32(type) || hasContiguous(type)) {
        if (containsHole(source->butterfly()->contiguous().data(), length))
            return false;
    } else
        return false;
    return target->appendMemcpy(globalObject, vm, target->length(), source);
}

static bool tryAppendTypedArray(JSGlobalObject* globalObject, VM& vm, JSArray* target, JSArrayBufferView* source)
{
    if (!source->isIteratorProtocolFastAndNonObservable())
        return false;
    size_t count = source->length();
    if (!count)
        return !source->isDetached() && !source->isOutOfBounds();
    if (count > MAX_STORAGE_VECTOR_LENGTH || !source->canGetIndexQuicklyForTypedArray(count - 1))
        return false;
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (size_t i = 0; i < count; ++i) {
        target->push(globalObject, source->getIndexQuicklyForTypedArray(i));
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

static Structure* emptyStructureOfLiteralForCopy(JSGlobalObject* globalObject, VM& vm, unsigned inlineCapacity)
{
    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacity);
    return profile.structure();
}

// `{ ...source }`. `keepsLayout`: it is all of the literal, and the copy of a record of the program is of its layout. Otherwise the copy is an ordinary object, for more to be defined on.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedCloneObject, JSCell*, (Instance* instance, EncodedJSValue source, uint32_t keepsLayout))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (Typed::Object* record = keepsLayout ? instance->typed->compiledObject(JSValue::decode(source)) : nullptr; record && record->layout().kind == Typed::LayoutKind::Record)
        OPERATION_RETURN(scope, record->clone(globalObject));
    OPERATION_RETURN(scope, cloneObjectForSpread(globalObject, JSValue::decode(source)));
}

static Instance::CopyForMore planCopyForMore(Instance* instance, JSGlobalObject* globalObject, VM& vm, Structure* source, uint64_t more)
{
    if (!source->canPerformFastPropertyEnumerationCommon() || !checkStructureForClone(source) || !source->didTransition())
        return { };
    if (source->realm() != globalObject || source->storedPrototype() != JSValue(globalObject->objectPrototype()))
        return { };
    std::array<UniquedStringImpl*, 2> defined { nullptr, nullptr };
    unsigned needed = more >> 48;
    for (unsigned i = 0; i < 2; ++i) {
        if (uint32_t number = (more >> (24 * i)) & 0xffffff) {
            defined[i] = nameAt(instance, number - 1).impl();
            needed += !isValidOffset(source->get(vm, defined[i])) && defined[i] != defined[0];
        }
    }
    needed += defined[0] && !isValidOffset(source->get(vm, defined[0]));
    Vector<UniquedStringImpl*, 16> names;
    bool isInOrder = !source->outOfLineCapacity();
    source->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        isInOrder &= !entry.attributes() && static_cast<size_t>(entry.offset()) == names.size();
        names.append(entry.key());
        return isInOrder;
    });
    needed += names.size();
    Instance::CopyForMore result { source, { } };
    if (isInOrder && needed > source->inlineCapacity() && needed <= JSFinalObject::maxInlineCapacity) {
        size_t size = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(needed));
        unsigned inlineCapacity = std::min<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
        if (Structure* wider = instance->literalStructure(emptyStructureOfLiteralForCopy(globalObject, vm, inlineCapacity), names.span()))
            result.structure = wider;
    }
    result.offsets = { invalidOffset, invalidOffset };
    for (unsigned i = 0; i < 2 && defined[i]; ++i) {
        PropertyOffset offset = result.structure->get(vm, defined[i]);
        if (!isValidOffset(offset)) {
            Structure* next = Structure::addPropertyTransitionToExistingStructure(result.structure, defined[i], 0, offset);
            if (!next) {
                DeferredStructureTransitionWatchpointFire deferred(vm, result.structure);
                next = Structure::addNewPropertyTransition(vm, result.structure, defined[i], 0, offset, PutPropertySlot::UnknownContext, &deferred);
            }
            if (next->isDictionary() || next->outOfLineCapacity() != result.structure->outOfLineCapacity())
                return { };
            result.structure = next;
        }
        result.offsets[i] = offset;
    }
    return result;
}

static JSObject* tryCopyExcluding(Instance* instance, JSGlobalObject* globalObject, VM& vm, JSObject* source, std::span<const EncodedJSValue> keys)
{
    Structure* structure = source->structure();
    if (structure->isDictionary())
        return nullptr;
    Vector<UniquedStringImpl*, 8> excluded;
    for (EncodedJSValue encodedKey : keys) {
        JSValue key = JSValue::decode(encodedKey);
        const StringImpl* impl = key.isString() ? asString(key)->tryGetValueImpl() : nullptr;
        if (!impl || !impl->isAtom())
            return nullptr;
        excluded.append(static_cast<UniquedStringImpl*>(const_cast<StringImpl*>(impl)));
    }
    DeferGC deferGC(vm);
    auto* plan = instance->copyExcluding(structure, excluded.span());
    if (!plan) {
        Instance::CopyExcluding made;
        made.excluded.appendRange(excluded.begin(), excluded.end());
        bool isEligible = structure->canPerformFastPropertyEnumerationCommon() && checkStructureForClone(structure)
            && structure->realm() == globalObject && structure->storedPrototype() == JSValue(globalObject->objectPrototype());
        if (isEligible) {
            Vector<UniquedStringImpl*, 16> names;
            structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
                if (!excluded.contains(entry.key())) {
                    names.append(entry.key());
                    made.offsets.append(entry.offset());
                }
                return true;
            });
            if (names.size() <= JSFinalObject::maxInlineCapacity) {
                Structure* empty = emptyStructureOfLiteralForCopy(globalObject, vm, std::max<unsigned>(names.size(), JSFinalObject::defaultInlineCapacity));
                made.structure = names.isEmpty() ? empty : instance->literalStructure(empty, names.span());
            }
        }
        plan = instance->setCopyExcluding(structure, WTF::move(made));
    }
    if (!plan->structure)
        return nullptr;
    JSFinalObject* rest = JSFinalObject::create(vm, plan->structure);
    auto* to = rest->inlineStorageUnsafe();
    for (unsigned i = 0; i < plan->offsets.size(); ++i)
        to[i].setWithoutWriteBarrier(source->getDirect(plan->offsets[i]));
    return rest;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCloneObjectForMore, JSCell*, (Instance* instance, EncodedJSValue encodedSource, uint64_t more, EncodedJSValue first, EncodedJSValue second))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue source = JSValue::decode(encodedSource);
    std::array<JSValue, 2> values { JSValue::decode(first), JSValue::decode(second) };
    if (source.isCell() && source.asCell()->type() == FinalObjectType) {
        JSObject* object = asObject(source);
        Structure* structure = object->structure();
        Instance::CopyForMore plan;
        if (auto* known = instance->copyForMore(structure, more))
            plan = *known;
        else {
            DeferGC deferGC(vm);
            plan = *instance->setCopyForMore(structure, more, planCopyForMore(instance, globalObject, vm, structure, more));
        }
        if (Structure* ofCopy = plan.structure) {
            auto offsets = plan.offsets;
            JSFinalObject* copy;
            if (ofCopy->inlineCapacity() != structure->inlineCapacity()) {
                copy = JSFinalObject::create(vm, ofCopy);
                auto* to = copy->inlineStorageUnsafe();
                auto* from = object->inlineStorageUnsafe();
                for (PropertyOffset offset = 0; offset <= structure->maxOffset(); ++offset)
                    to[offset].setWithoutWriteBarrier(from[offset].get());
            } else if (unsigned propertyCapacity = object->butterfly() ? structure->outOfLineCapacity() : 0) {
                DeferGC deferGC(vm);
                Butterfly* butterfly = Butterfly::createUninitialized(vm, nullptr, 0, propertyCapacity, false, 0);
                memcpy(butterfly->propertyStorage() - propertyCapacity, object->butterfly()->propertyStorage() - propertyCapacity, propertyCapacity * sizeof(EncodedJSValue));
                copy = JSFinalObject::createWithButterflyCopyingInlineStorage(vm, ofCopy, butterfly, object->inlineStorage());
            } else
                copy = JSFinalObject::createWithButterflyCopyingInlineStorage(vm, ofCopy, nullptr, object->inlineStorage());
            for (unsigned i = 0; i < 2 && isValidOffset(offsets[i]); ++i)
                copy->locationForOffset(offsets[i])->setWithoutWriteBarrier(values[i]);
            OPERATION_RETURN(scope, copy);
        }
    }
    JSObject* copy = cloneObjectForSpread(globalObject, source);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    for (unsigned i = 0; i < 2; ++i) {
        if (uint32_t number = (more >> (24 * i)) & 0xffffff) {
            createDataProperty(globalObject, copy, nameAt(instance, number - 1), values[i]);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTryCopyRecordAs, JSCell*, (Instance* instance, uint32_t shape, uint32_t namesAre, uint32_t count, const EncodedJSValue* values))
{
    Typed::State& state = *instance->typed;
    Typed::Object* record = state.compiledObject(JSValue::decode(values[0]));
    return record ? record->tryCopyAs(instance->globalObject, state.layoutOfShape(shape), static_cast<Typed::NamesOfCopy>(namesAre), std::span { values + 1, count - 1 }) : nullptr;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTryMergeRecordsAs, JSCell*, (Instance* instance, uint32_t shape, EncodedJSValue first, EncodedJSValue second))
{
    Typed::State& state = *instance->typed;
    Typed::Object* one = state.compiledObject(JSValue::decode(first));
    Typed::Object* other = state.compiledObject(JSValue::decode(second));
    return one && other ? Typed::Object::tryMergeAs(instance->globalObject, state.layoutOfShape(shape), *one, *other) : nullptr;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTryCopyRecordAsAt, JSCell*, (Instance* instance, Slot* cache, uint32_t firstShape, uint32_t secondShape, uint32_t shape, uint32_t namesAre, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN(instance);
    Typed::State& state = *instance->typed;
    Typed::Object* record = state.compiledObject(JSValue::decode(values[0]));
    if (!record && !JSValue::decode(values[0]).isCell() && static_cast<Typed::NamesOfCopy>(namesAre) == Typed::NamesOfCopy::StoredNext) {
        Typed::Object* made = Typed::Object::tryMakeAs(globalObject, state, state.layoutOfShape(shape), std::span { values + 1, count - 1 });
        if (made && !SharedData::contains(cache) && !(made->orderWord() & ~Typed::orderAndCountMask) && Typed::orderOf(made->orderWord()) != Typed::keysAreInLayoutOrder) {
            Slot& slot = cache[!!firstShape + !!secondShape + 1];
            slot.offset = made->orderWord();
            slot.pointer = std::bit_cast<void*>(static_cast<uintptr_t>(1));
        }
        OPERATION_RETURN(scope, made);
    }
    if (!record)
        OPERATION_RETURN(scope, nullptr);
    uint64_t words = record->orderAndShapeWords();
    uint32_t shapeOfRecord = record->layout().shape;
    bool isNeverCopied = false;
    Typed::Object* copy = record->tryCopyAs(globalObject, state.layoutOfShape(shape), static_cast<Typed::NamesOfCopy>(namesAre), std::span { values + 1, count - 1 }, &isNeverCopied);
    bool isKnown = shapeOfRecord == firstShape || shapeOfRecord == secondShape;
    if (copy && isKnown && !SharedData::contains(cache) && !((static_cast<uint32_t>(words) | copy->orderWord()) & ~Typed::orderAndCountMask) && Typed::orderOf(static_cast<uint32_t>(words)) != Typed::keysAreInLayoutOrder && Typed::orderOf(copy->orderWord()) != Typed::keysAreInLayoutOrder) {
        Slot& slot = cache[shapeOfRecord != firstShape];
        slot.offset = copy->orderWord();
        slot.pointer = std::bit_cast<void*>(words);
    } else if (isNeverCopied && !SharedData::contains(cache))
        cache[!!firstShape + !!secondShape].pointer = std::bit_cast<void*>(words);
    OPERATION_RETURN(scope, copy);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedLanguageOperation, EncodedJSValue, (Instance* instance, uint32_t which, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (Options::useAOTOperationCounters()) [[unlikely]] {
        static constexpr char names[][40] = {
#define AOT_TYPED_NAME_LANGUAGE_OPERATION(name) #name,
            FOR_EACH_AOT_TYPED_LANGUAGE_OPERATION(AOT_TYPED_NAME_LANGUAGE_OPERATION)
#undef AOT_TYPED_NAME_LANGUAGE_OPERATION
        };
        countOperationNamed(instance, "language-operation", which < std::size(names) ? names[which] : "unknown");
        if (which == static_cast<uint32_t>(Typed::LanguageOperation::AppendSpread) && count > 1)
            countSpreadForCounters(instance, uncheckedDowncast<JSArray>(JSValue::decode(values[0])), JSValue::decode(values[1]));
    }
    auto argument = [&](unsigned index) {
        return index < count ? JSValue::decode(values[index]) : jsUndefined();
    };
    auto copyDataProperties = [&](JSObject* target, JSValue source, const UncheckedKeyHashSet<RefPtr<UniquedStringImpl>>& excluded) {
        if (source.isUndefinedOrNull())
            return;
        JSObject* from = source.toObject(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (excluded.isEmpty() && target->type() == FinalObjectType && from->type() == FinalObjectType && tryCopyDataProperties(instance, vm, target, from, nullptr, "operationAOTTypedLanguageOperation"))
            return;
        // What Object.assign() does where nobody can tell a store from a definition. False: it has done nothing.
        if (auto* ordinary = dynamicDowncast<JSFinalObject>(target); ordinary && excluded.isEmpty() && ordinary->canPerformFastPutInlineExcludingProto() && ordinary->isStructureExtensible()) {
            Vector<UniquedStringImpl*, 8> properties;
            MarkedArgumentBuffer valuesOfProperties;
            bool isDone = objectAssignFast(globalObject, ordinary, from, properties, valuesOfProperties);
            RETURN_IF_EXCEPTION(scope, void());
            if (isDone)
                return;
        }
        if (auto* record = dynamicDowncast<Typed::Object>(target); record && excluded.isEmpty()) {
            if (auto* other = dynamicDowncast<Typed::Object>(from); other && record->tryCopyFieldsOf(vm, *other))
                return;
        }
        PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Exclude);
        from->methodTable()->getOwnPropertyNames(from, globalObject, names, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, void());
        for (auto& name : names) {
            if (excluded.contains(name.impl()))
                continue;
            PropertySlot slot(from, PropertySlot::InternalMethodType::GetOwnProperty);
            bool has = from->methodTable()->getOwnPropertySlot(from, globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, void());
            if (!has || slot.attributes() & PropertyAttribute::DontEnum)
                continue;
            JSValue value = slot.isTaintedByOpaqueObject() ? from->get(globalObject, name) : slot.getValue(globalObject, name);
            RETURN_IF_EXCEPTION(scope, void());
            createDataProperty(globalObject, target, name, value);
            RETURN_IF_EXCEPTION(scope, void());
        }
    };
    switch (static_cast<Typed::LanguageOperation>(which)) {
    case Typed::LanguageOperation::CopyDataProperties:
        copyDataProperties(asObject(argument(0)), argument(1), { });
        OPERATION_RETURN(scope, values[0]);
    case Typed::LanguageOperation::CopyDataPropertiesExcluding: {
        if (argument(0).isUndefinedOrNull()) {
            throwTypeError(globalObject, scope, "Right side of assignment cannot be destructured"_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        if (Typed::Object* record = instance->typed->compiledObject(argument(0))) {
            if (JSObject* rest = record->tryCopyAsPlainObject(globalObject, std::span { values + 1, count - 1 }))
                OPERATION_RETURN(scope, JSValue::encode(rest));
        }
        if (JSValue source = argument(0); count > 1 && source.isCell() && source.asCell()->type() == FinalObjectType) {
            if (JSObject* rest = tryCopyExcluding(instance, globalObject, vm, asObject(source), std::span { values + 1, count - 1 }))
                OPERATION_RETURN(scope, JSValue::encode(rest));
        }
        UncheckedKeyHashSet<RefPtr<UniquedStringImpl>> excluded;
        for (unsigned i = 1; i < count; ++i) {
            auto key = argument(i).toPropertyKey(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            excluded.add(key.impl());
        }
        JSObject* target = constructEmptyObject(globalObject);
        copyDataProperties(target, argument(0), excluded);
        OPERATION_RETURN(scope, JSValue::encode(target));
    }
    // See operationAOTTypedCloneObject.
    case Typed::LanguageOperation::CloneObject:
        break;
    case Typed::LanguageOperation::CloneObjectWith:
        OPERATION_RETURN(scope, JSValue::encode(cloneObjectForSpread(globalObject, argument(0))));
    case Typed::LanguageOperation::CloneObjectAfter: {
        JSObject* target = constructEmptyObject(globalObject);
        for (unsigned i = 1; i + 1 < count; i += 2) {
            auto key = argument(i).toPropertyKey(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            createDataProperty(globalObject, target, key, argument(i + 1));
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        copyDataProperties(target, argument(0), { });
        OPERATION_RETURN(scope, JSValue::encode(target));
    }
    case Typed::LanguageOperation::DefineField: {
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        createDataProperty(globalObject, asObject(argument(0)), key, argument(2));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::DefineGetter:
    case Typed::LanguageOperation::DefineSetter:
        defineAccessor(globalObject, argument(0), argument(1), argument(2), argument(3).isTrue(), static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::DefineGetter, argument(4));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::AppendSpread: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
        if (tryAppendArray(globalObject, vm, array, argument(1)))
            OPERATION_RETURN(scope, values[0]);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (Typed::CollectionIteration::tryAppendTo(globalObject, array, argument(1))) {
            countOperationNamed(instance, "AppendSpread", "collection");
            OPERATION_RETURN(scope, values[0]);
        }
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (JSValue iterable = argument(1); iterable.isCell() && isTypedArrayType(iterable.asCell()->type())) {
            auto* view = uncheckedDowncast<JSArrayBufferView>(iterable.asCell());
            if (globalObject->isOriginalTypedArrayStructure(view->structure(), view->isResizableOrGrowableShared())) {
                if (tryAppendTypedArray(globalObject, vm, array, view)) {
                    countOperationNamed(instance, "AppendSpread", "typed array");
                    OPERATION_RETURN(scope, values[0]);
                }
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
        }
        if (Options::useAOTOperationCounters()) [[unlikely]] {
            countOperationNamed(instance, "AppendSpread", "iterated");
            if (auto* list = dynamicDowncast<JSArray>(argument(1)))
                countOperationNamed(instance, "AppendSpread:an array is iterated", whyArrayIsIteratedForCounters(list));
        }
        forEachInIterable(globalObject, argument(1), [&](VM&, JSGlobalObject* globalObject, JSValue value) {
            array->push(globalObject, value);
        });
        OPERATION_RETURN(scope, values[0]);
    }
    case Typed::LanguageOperation::AppendHole: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
        array->setLength(globalObject, array->length() + 1, true);
        OPERATION_RETURN(scope, values[0]);
    }
    case Typed::LanguageOperation::Apply:
    case Typed::LanguageOperation::CallWithSpread:
    case Typed::LanguageOperation::ConstructWithArray: {
        bool isCall = static_cast<Typed::LanguageOperation>(which) != Typed::LanguageOperation::ConstructWithArray;
        bool isSpread = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::CallWithSpread;
        CallData callData = isCall ? JSC::getCallData(argument(0)) : CallData();
        // Function.prototype.apply looks at the function first. A call evaluates its arguments first.
        if (isCall && !isSpread && callData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, makeString(errorDescriptionForValue(globalObject, argument(0)), " is not a function"_s));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        // A spread has made an array. Function.prototype.apply takes `undefined`, `null` and any array-like object as well.
        JSValue list = argument(isCall ? 2 : 1);
        MarkedArgumentBuffer arguments;
        if (isSpread) {
            forEachInIterable(globalObject, list, [&](VM&, JSGlobalObject*, JSValue value) {
                arguments.append(value);
            });
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (callData.type == CallData::Type::None) {
                throwTypeError(globalObject, scope, makeString(errorDescriptionForValue(globalObject, argument(0)), " is not a function"_s));
                OPERATION_RETURN(scope, encodedJSValue());
            }
        } else if (auto* array = dynamicDowncast<JSArray>(list)) [[likely]] {
            for (unsigned i = 0; i < array->length(); ++i) {
                arguments.append(array->getIndex(globalObject, i));
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
        } else if (!list.isUndefinedOrNull()) {
            if (!list.isObject()) {
                throwTypeError(globalObject, scope, "second argument to Function.prototype.apply must be an Array-like object"_s);
                OPERATION_RETURN(scope, encodedJSValue());
            }
            forEachInArrayLike(globalObject, asObject(list), [&](JSValue value) -> bool {
                arguments.append(value);
                return true;
            });
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        if (arguments.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        if (isCall)
            OPERATION_RETURN(scope, JSValue::encode(call(globalObject, argument(0), callData, argument(1), arguments)));
        auto constructData = JSC::getConstructData(argument(0));
        if (constructData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, makeString(State::describe(argument(0)), " is not a constructor"_s));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(construct(globalObject, argument(0), constructData, arguments)));
    }
    case Typed::LanguageOperation::ConstructWith:
        RELEASE_ASSERT(count >= 2);
        OPERATION_RETURN(scope, JSValue::encode(constructWith(globalObject, vm, argument(0), argument(1).asCell(), count - 2, values + 2)));
    case Typed::LanguageOperation::ConstructWithSpread: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(2));
        Vector<EncodedJSValue, 8> elements;
        MarkedArgumentBuffer keptAlive;
        for (unsigned i = 0; i < array->length(); ++i) {
            JSValue element = array->getIndex(globalObject, i);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            keptAlive.append(element);
            elements.append(JSValue::encode(element));
        }
        if (keptAlive.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(constructWith(globalObject, vm, argument(0), argument(1).asCell(), elements.size(), elements.span().data())));
    }
    case Typed::LanguageOperation::MakeClass: {
        auto* classObject = dynamicDowncast<Typed::Closure>(argument(0));
        RELEASE_ASSERT(classObject && classObject->classData());
        instance->typed->makeClass(globalObject, classObject, argument(1), Typed::State::PartOfClass::Everything, count > 2 ? argument(2) : JSValue());
        OPERATION_RETURN(scope, values[0]);
    }
    case Typed::LanguageOperation::NewObjectOfClass: {
        auto* classObject = dynamicDowncast<Typed::Closure>(argument(0));
        RELEASE_ASSERT(classObject && classObject->classData());
        OPERATION_RETURN(scope, JSValue::encode(instance->typed->createObjectOfClass(vm, *classObject->classData(), asObject(classObject->prototypeProperty(vm)))));
    }
    case Typed::LanguageOperation::DefineMethod: {
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        // `#name() {}` cannot be stored to.
        PropertyDescriptor descriptor(argument(2), key.isPrivateName() ? PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly : static_cast<unsigned>(PropertyAttribute::DontEnum));
        asObject(argument(0))->methodTable()->defineOwnProperty(asObject(argument(0)), globalObject, key, descriptor, true);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::SetFunctionName: {
        if (auto* slim = dynamicDowncast<Typed::SlimClosure>(argument(0))) {
            RELEASE_ASSERT(instance->typed->closureIfMade(slim->functionIndex()) != slim && (argument(1).isString() || argument(1).isSymbol()));
            auto key = argument(1).toPropertyKey(globalObject);
            scope.assertNoException();
            slim->setComputedName(vm, key, static_cast<Typed::Format::MethodKind>(argument(2).asInt32()));
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
        auto* closure = dynamicDowncast<Typed::Closure>(argument(0));
        // Its own cell: the one closure of a function that captures nothing has one name.
        RELEASE_ASSERT(closure && instance->typed->closureIfMade(closure->functionIndex()) != closure && (argument(1).isString() || argument(1).isSymbol()));
        if (JSValue own = closure->classData() ? closure->getDirect(vm, vm.propertyNames->name) : JSValue(); own && !own.isString())
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        auto key = argument(1).toPropertyKey(globalObject);
        scope.assertNoException();
        closure->setComputedName(vm, key, static_cast<Typed::Format::MethodKind>(argument(2).asInt32()));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::SuperGet:
    case Typed::LanguageOperation::SuperSet: {
        bool isGet = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::SuperGet;
        // What the method was defined on: the class object, or its `prototype`, which stays what it is.
        JSValue home = argument(0);
        if (!argument(isGet ? 3 : 4).isTrue()) {
            home = home.get(globalObject, vm.propertyNames->prototype);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        RELEASE_ASSERT(home.isObject());
        JSValue parent = asObject(home)->getPrototype(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!parent.isObject()) {
            throwTypeError(globalObject, scope, "The prototype of the home object is not an object"_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (isGet) {
            PropertySlot slot(argument(2), PropertySlot::InternalMethodType::Get);
            bool has = asObject(parent)->getPropertySlot(globalObject, key, slot);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            OPERATION_RETURN(scope, JSValue::encode(has ? slot.getValue(globalObject, key) : jsUndefined()));
        }
        PutPropertySlot slot(argument(3), true);
        asObject(parent)->methodTable()->put(asObject(parent), globalObject, key, argument(2), slot);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::SuperConstruct:
    case Typed::LanguageOperation::ConstructOn: {
        bool isGivenThis = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::ConstructOn;
        JSArray* array = uncheckedDowncast<JSArray>(argument(isGivenThis ? 3 : 2));
        MarkedArgumentBuffer arguments;
        for (unsigned i = 0; i < array->length(); ++i) {
            arguments.append(array->getIndex(globalObject, i));
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        if (arguments.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        if (isGivenThis) {
            auto* classObject = dynamicDowncast<Typed::Closure>(argument(0));
            RELEASE_ASSERT(classObject && classObject->classData());
            OPERATION_RETURN(scope, JSValue::encode(classObject->callAsConstructor(globalObject, argument(1), argument(2), arguments)));
        }
        JSValue parent = count > 3 ? argument(3) : asObject(argument(0))->getPrototype(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        auto constructData = JSC::getConstructData(parent);
        if (constructData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, "The superclass is not a constructor."_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        if (auto* errorConstructor = dynamicDowncast<ErrorConstructor>(parent); errorConstructor && errorConstructor->realm() == globalObject && argument(1).isObject() && argument(1) != parent) {
            JSObject* newTarget = asObject(argument(1));
            Structure* structure = JSC_GET_DERIVED_STRUCTURE(vm, errorStructure, newTarget, errorConstructor);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            OPERATION_RETURN(scope, JSValue::encode(ErrorInstance::create(globalObject, structure, arguments.at(0), arguments.at(1), nullptr, TypeNothing, ErrorType::Error, true, newTarget)));
        }
        OPERATION_RETURN(scope, JSValue::encode(construct(globalObject, parent, constructData, arguments, argument(1))));
    }
    case Typed::LanguageOperation::ConstructorResult:
        OPERATION_RETURN(scope, values[argument(0).isObject() ? 0 : 1]);
    case Typed::LanguageOperation::DerivedConstructorResult:
        if (argument(0).isObject())
            OPERATION_RETURN(scope, values[0]);
        if (!argument(0).isUndefined()) {
            throwTypeError(globalObject, scope, "Cannot return a non-object type in the constructor of a derived class."_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, values[1]);
    case Typed::LanguageOperation::RejectPromise:
        uncheckedDowncast<JSPromise>(argument(0))->reject(vm, argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    // These have operations of their own, below.
    case Typed::LanguageOperation::NewPromise:
    case Typed::LanguageOperation::ResolvePromise:
    case Typed::LanguageOperation::Await:
    // In place.
    case Typed::LanguageOperation::GetNewTarget:
        break;
    case Typed::LanguageOperation::NewGenerator: {
        auto* resumeClosure = uncheckedDowncast<Typed::Closure>(argument(0));
        Structure* structure = argument(1).isCell() ? instance->typed->generatorStructure(vm, uncheckedDowncast<Typed::Closure>(argument(1)), false) : instance->typed->generatorStructure(vm, resumeClosure->functionIndex(), false);
        OPERATION_RETURN(scope, JSValue::encode(Typed::Generator::create(vm, structure, resumeClosure)));
    }
    case Typed::LanguageOperation::NewAsyncGenerator: {
        auto* resumeClosure = uncheckedDowncast<Typed::Closure>(argument(0));
        Structure* structure = argument(1).isCell() ? instance->typed->generatorStructure(vm, uncheckedDowncast<Typed::Closure>(argument(1)), true) : instance->typed->generatorStructure(vm, resumeClosure->functionIndex(), true);
        OPERATION_RETURN(scope, JSValue::encode(Typed::AsyncGenerator::create(vm, globalObject, structure, resumeClosure)));
    }
    case Typed::LanguageOperation::CreateArguments:
    case Typed::LanguageOperation::GetFromArguments: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
        unsigned length = array->length();
        bool isRead = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::GetFromArguments;
        // Under an index the object has what the array has, and inherits nothing.
        if (auto index = isRead ? argument(1).tryGetAsUint32Index() : std::nullopt)
            OPERATION_RETURN(scope, JSValue::encode(*index < length ? array->getIndex(globalObject, *index) : jsUndefined()));
        ClonedArguments* result = tryCreateArgumentsAtOnce(vm, globalObject, array, length);
        countOperationNamed(instance, "CreateArguments", result ? "made at once" : "made step by step");
        if (!result) {
            result = ClonedArguments::createEmpty(vm, globalObject, globalObject->clonedArgumentsStructure(), nullptr, length, nullptr);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            for (unsigned i = 0; i < length; ++i) {
                result->putDirectIndex(globalObject, i, array->getIndex(globalObject, i));
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
            // Without a callee to take them from, it counts as having its special properties already.
            result->putDirectAccessor(globalObject, vm.propertyNames->callee, globalObject->throwTypeErrorArgumentsCalleeGetterSetter(), attributesOfCalleeOfArguments);
            result->putDirect(vm, vm.propertyNames->iteratorSymbol, globalObject->arrayProtoValuesFunction(), static_cast<unsigned>(PropertyAttribute::DontEnum));
        }
        if (!isRead)
            OPERATION_RETURN(scope, JSValue::encode(result));
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(result->get(globalObject, key)));
    }
    // Of sloppy code with simple parameters: the array of all arguments, the closure that runs, the number of parameters. A parameter lives in the slot of its number for good.
    case Typed::LanguageOperation::CreateMappedArguments: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
        unsigned length = array->length();
        unsigned capacity = std::max(length, static_cast<unsigned>(argument(2).asInt32()));
        DirectArguments* result = DirectArguments::create(vm, globalObject->directArgumentsStructure(), length, capacity);
        result->setCallee(vm, uncheckedDowncast<JSFunction>(argument(1)));
        for (unsigned i = 0; i < length; ++i) {
            JSValue value = array->getIndex(globalObject, i);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            result->argument(DirectArgumentsOffset(i)).set(vm, result, value);
        }
        OPERATION_RETURN(scope, JSValue::encode(result));
    }
    case Typed::LanguageOperation::GetMappedParameter:
        OPERATION_RETURN(scope, JSValue::encode(uncheckedDowncast<DirectArguments>(argument(0))->argument(DirectArgumentsOffset(argument(1).asInt32())).get()));
    case Typed::LanguageOperation::PutMappedParameter: {
        DirectArguments* object = uncheckedDowncast<DirectArguments>(argument(0));
        object->argument(DirectArgumentsOffset(argument(1).asInt32())).set(vm, object, argument(2));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::GetAsyncIterator: {
        if (auto* generator = dynamicDowncast<Typed::AsyncGenerator>(argument(0)); generator && generator->iteratesAsMade(globalObject))
            OPERATION_RETURN(scope, JSValue::encode(generator));
        JSValue method = argument(0).get(globalObject, vm.propertyNames->asyncIteratorSymbol);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (method.isUndefinedOrNull())
            OPERATION_RETURN(scope, JSValue::encode(createAsyncFromSyncIteratorForIterable(globalObject, argument(0))));
        auto callData = JSC::getCallData(method);
        if (callData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, "The value is not async iterable"_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        JSValue iterator = call(globalObject, method, callData, argument(0), ArgList());
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!iterator.isObject()) {
            throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(iterator));
    }
    case Typed::LanguageOperation::GetIterator: {
        if (auto* generator = dynamicDowncast<Typed::Generator>(argument(0)); generator && generator->iteratesAsMade(globalObject))
            OPERATION_RETURN(scope, JSValue::encode(generator));
        JSValue method = argument(0).get(globalObject, vm.propertyNames->iteratorSymbol);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        auto callData = JSC::getCallData(method);
        if (callData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, "Spread syntax requires ...iterable[Symbol.iterator] to be a function"_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        JSValue iterator = call(globalObject, method, callData, argument(0), ArgList());
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!iterator.isObject()) {
            throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(iterator));
    }
    case Typed::LanguageOperation::RequireIteratorResultObject:
        if (!argument(0).isObject())
            throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::IteratorCloseAndThrowMissingThrowMethod: {
        JSValue method = argument(0).get(globalObject, vm.propertyNames->returnKeyword);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!method.isUndefinedOrNull()) {
            auto callData = JSC::getCallData(method);
            if (callData.type != CallData::Type::None) {
                call(globalObject, method, callData, argument(0), ArgList());
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
        }
        throwTypeError(globalObject, scope, "Delegated generator does not have a 'throw' method."_s);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    case Typed::LanguageOperation::NewDisposableStack:
        OPERATION_RETURN(scope, JSValue::encode(constructEmptyArray(globalObject, nullptr)));
    case Typed::LanguageOperation::AddDisposableResource: {
        JSArray* stack = uncheckedDowncast<JSArray>(argument(0));
        JSValue resource = argument(1);
        bool isAsync = argument(2).isTrue();
        JSValue kind = jsBoolean(isAsync);
        JSValue method = jsUndefined();
        if (!resource.isUndefinedOrNull()) {
            if (!resource.isObject()) {
                throwTypeError(globalObject, scope, "The value of a using declaration must be an object, null or undefined"_s);
                OPERATION_RETURN(scope, encodedJSValue());
            }
            if (isAsync) {
                method = resource.get(globalObject, vm.propertyNames->asyncDisposeSymbol);
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            }
            if (method.isUndefinedOrNull()) {
                method = resource.get(globalObject, vm.propertyNames->disposeSymbol);
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
                if (isAsync)
                    kind = jsNumber(0);
            }
            if (!method.isCallable()) {
                throwTypeError(globalObject, scope, "The value of a using declaration has no callable dispose method"_s);
                OPERATION_RETURN(scope, encodedJSValue());
            }
        } else if (!isAsync)
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        stack->push(globalObject, resource);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        stack->push(globalObject, method);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        stack->push(globalObject, kind);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::RecordDisposeError:
        recordDisposeError(globalObject, uncheckedDowncast<JSArray>(argument(0)), argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::DisposeNextResource:
        OPERATION_RETURN(scope, JSValue::encode(disposeNextResource(globalObject, uncheckedDowncast<JSArray>(argument(0)))));
    case Typed::LanguageOperation::DisposeResources: {
        JSArray* stack = uncheckedDowncast<JSArray>(argument(0));
        if (JSValue hasError = stack->getDirect(vm, Identifier::fromString(vm, "hasError"_s)); argument(1).isTrue() && !hasError)
            recordDisposeError(globalObject, stack, argument(2));
        while (stack->length()) {
            stack->pop(globalObject);
            JSValue method = stack->pop(globalObject);
            JSValue resource = stack->pop(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            if (!method.isUndefined()) {
                auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
                call(globalObject, method, JSC::getCallData(method), resource, ArgList());
                if (Exception* exception = catchScope.exception()) [[unlikely]] {
                    JSValue error = exception->value();
                    if (!catchScope.clearExceptionExceptTermination())
                        OPERATION_RETURN(scope, encodedJSValue());
                    recordDisposeError(globalObject, stack, error);
                }
            }
        }
        JSValue hasError = stack->getDirect(vm, Identifier::fromString(vm, "hasError"_s));
        // The completion of the block is rethrown by the caller unless disposal has replaced it.
        if (hasError && hasError.isTrue()) {
            JSValue error = stack->getDirect(vm, Identifier::fromString(vm, "error"_s));
            if (!argument(1).isTrue() || error != argument(2))
                throwException(globalObject, scope, error);
        }
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::ToESM:
        OPERATION_RETURN(scope, JSValue::encode(toESM(globalObject, instance, argument(0), argument(1).isTrue())));
    case Typed::LanguageOperation::MakeModuleNamespace: {
        bool hasFlag = argument(count - 1).isBoolean();
        bool hasPrototypeOfHost = hasFlag && argument(count - 1).isTrue();
        unsigned end = count - hasFlag;
        JSObject* star = end > 1 ? argument(end - 1).getObject() : nullptr;
        end -= !!star;
        auto* names = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, end - 1);
        for (unsigned i = 1; i < end; ++i) {
            // It is looked for by its address first.
            asString(argument(i))->toIdentifier(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            // The numbers are the front end's, and so is the order.
            RELEASE_ASSERT(i == 1 || codePointCompare(StringView(*asString(argument(i - 1))->tryGetValueImpl()), StringView(*asString(argument(i))->tryGetValueImpl())) < 0);
            names->setIndex(vm, i - 1, argument(i));
        }
        JSCellButterfly* namesOfStar = nullptr;
        if (star) {
            PropertyNameArrayBuilder found(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            star->methodTable()->getOwnPropertyNames(star, globalObject, found, DontEnumPropertiesMode::Include);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            Vector<Identifier> brought;
            for (auto& name : found) {
                bool isOwn = name == vm.propertyNames->defaultKeyword;
                for (unsigned i = 1; i < end && !isOwn; ++i)
                    isOwn = asString(argument(i))->tryGetValueImpl() == name.impl();
                if (!isOwn)
                    brought.append(name);
            }
            std::sort(brought.begin(), brought.end(), [](const Identifier& a, const Identifier& b) {
                return codePointCompare(StringView(*a.impl()), StringView(*b.impl())) < 0;
            });
            namesOfStar = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, brought.size());
            for (unsigned i = 0; i < brought.size(); ++i) {
                JSString* text = jsString(vm, brought[i].string());
                text->toIdentifier(globalObject);
                OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
                namesOfStar->setIndex(vm, i, text);
            }
        }
        OPERATION_RETURN(scope, JSValue::encode(Typed::ModuleNamespace::create(vm, instance->typed->moduleNamespaceStructure(vm, hasPrototypeOfHost), asObject(argument(0)), names, star, namesOfStar)));
    }
    case Typed::LanguageOperation::ParseJSON: {
        auto text = asString(argument(0))->value(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        JSValue parsed = JSONParseWithException(globalObject, text);
        OPERATION_RETURN(scope, JSValue::encode(parsed));
    }
    case Typed::LanguageOperation::DirectEval: {
        if (!argument(0).isString())
            OPERATION_RETURN(scope, values[0]);
        if (!globalObject->evalEnabled()) [[unlikely]] {
            throwException(globalObject, scope, createEvalError(globalObject, globalObject->evalDisabledErrorMessage()));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        auto text = asString(argument(0))->value(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        JSObject* names = asObject(argument(1));
        static constexpr Latin1Character keyOfThis[] = { 0, 'e', 'v', 'a', 'l' };
        JSValue thisValue = names->getDirect(vm, Identifier::fromString(vm, String(std::span<const Latin1Character> { keyOfThis })));
        bool isInFunction = !!names->getDirect(vm, Typed::EvalScope::keyOfNewTarget(vm));
        static constexpr Latin1Character keyOfSloppy[] = { 0, 's', 'l', 'o', 'p', 'p', 'y' };
        bool isSloppy = !!names->getDirect(vm, Identifier::fromString(vm, String(std::span<const Latin1Character> { keyOfSloppy })));
        SourceOrigin origin;
        if (argument(2).isString()) {
            origin = SourceOrigin { URL::fileURLWithFileSystemPath(String { asString(argument(2))->value(globalObject) }) };
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        if (argument(2).isTrue() || argument(2).isString()) {
            for (ASCIILiteral name : { "require"_s, "__filename"_s, "__dirname"_s }) {
                if (JSValue accessor = names->getDirect(vm, Identifier::fromString(vm, makeString('\0', name))))
                    names->putDirectAccessor(globalObject, Identifier::fromString(vm, name), uncheckedDowncast<GetterSetter>(accessor.asCell()), static_cast<unsigned>(PropertyAttribute::Accessor));
            }
        }
        TDZEnvironment variablesUnderTDZ;
        PrivateNameEnvironment privateNames;
        auto* executable = DirectEvalExecutable::create(globalObject, makeSource(text.data, origin, SourceTaintedOrigin::Untainted), isSloppy ? NoLexicallyScopedFeatures : StrictModeLexicallyScopedFeature, DerivedContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None, false, isInFunction, isInFunction ? EvalContextType::FunctionEvalContext : EvalContextType::None, &variablesUnderTDZ, &privateNames);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (!executable->isInStrictContext() && (!executable->variables().empty() || !executable->topLevelFunctionDecls().empty() || !executable->functionHoistingCandidates().empty())) {
            throwSyntaxError(globalObject, scope, withAOTErrorNote("A direct eval in sloppy-mode code declares a variable or a function in the enclosing function, which a program compiled ahead of time does not support"_s));
            OPERATION_RETURN(scope, encodedJSValue());
        }
        JSScope* around = Typed::EvalScope::create(vm, instance->typed->evalScopeStructure(vm), globalObject->globalScope(), names);
        OPERATION_RETURN(scope, JSValue::encode(vm.interpreter.executeEval(executable, thisValue, around)));
    }
    case Typed::LanguageOperation::GetRequireFunction:
        OPERATION_RETURN(scope, JSValue::encode(requireFunctionOfHost(globalObject, instance)));
    case Typed::LanguageOperation::RequireExternalModule:
        OPERATION_RETURN(scope, JSValue::encode(requireFromHost(globalObject, instance, argument(0))));
    case Typed::LanguageOperation::GetTemplateObject: {
        JSObject* cooked = asObject(argument(0));
        JSObject* raw = asObject(argument(1));
        objectConstructorFreeze(globalObject, raw);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        cooked->putDirect(vm, vm.propertyNames->raw, raw, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
        objectConstructorFreeze(globalObject, cooked);
        OPERATION_RETURN(scope, values[0]);
    }
    case Typed::LanguageOperation::GetImportMeta: {
        auto& state = *instance->typed;
        if (count)
            OPERATION_RETURN(scope, JSValue::encode(state.importMetaOfModule(globalObject, asString(argument(0)))));
        if (!state.importMeta) {
            auto* importer = uncheckedDowncast<JSModuleRecord>(state.moduleRecord);
            state.importMeta = globalObject->moduleLoader()->createImportMetaProperties(globalObject, identifierToJSValue(vm, state.keyOfImporter(vm)), importer, nullptr);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(state.importMeta));
    }
    case Typed::LanguageOperation::ImportExternalModule: {
        auto rejectWithCaughtException = [&] {
            return JSValue::encode(JSPromise::create(vm, globalObject->promiseStructure())->rejectWithCaughtException(vm, scope));
        };
        JSString* specifier = argument(0).toString(globalObject);
        if (scope.exception()) [[unlikely]]
            OPERATION_RETURN(scope, rejectWithCaughtException());
        auto* importer = uncheckedDowncast<JSModuleRecord>(instance->typed->moduleRecord);
        SourceOrigin referrer = importer ? importer->sourceCode().provider()->sourceOrigin() : SourceOrigin { URL::fileURLWithFileSystemPath(instance->typed->pathOfFileAskedFor) };
        if (count > 2)
            referrer = SourceOrigin { URL::fileURLWithFileSystemPath(String { asString(argument(2))->value(globalObject) }) };
        JSPromise* promise = globalObject->moduleLoader()->importModule(globalObject, specifier, argument(1), referrer);
        if (scope.exception()) [[unlikely]]
            OPERATION_RETURN(scope, rejectWithCaughtException());
        OPERATION_RETURN(scope, JSValue::encode(promise));
    }
    case Typed::LanguageOperation::ImportWithOptions: {
        auto attributes = retrieveImportAttributesFromDynamicImportOptions(globalObject, argument(0), { vm.propertyNames->type.impl() });
        if (!scope.exception()) [[likely]]
            retrieveTypeImportAttribute(globalObject, attributes);
        if (scope.exception()) [[unlikely]]
            OPERATION_RETURN(scope, JSValue::encode(JSPromise::create(vm, globalObject->promiseStructure())->rejectWithCaughtException(vm, scope)));
        OPERATION_RETURN(scope, JSValue::encode(call(globalObject, argument(1), JSC::getCallData(argument(1)), jsUndefined(), ArgList())));
    }
    case Typed::LanguageOperation::NewModuleRecord:
        OPERATION_RETURN(scope, JSValue::encode(newModuleRecord(*instance, argument(0).isTrue(), argument(1))));
    case Typed::LanguageOperation::EnterModule:
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(enterModule(*instance, uncheckedDowncast<JSArray>(argument(0).asCell())))));
    case Typed::LanguageOperation::DidEvaluateRequiredModule:
        didEvaluateRequiredModule(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), uncheckedDowncast<JSArray>(argument(1).asCell()));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::LeaveModule:
        leaveModule(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::DidStartModuleBody:
        didStartModuleBody(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::AsyncModuleExecutionFulfilled:
        asyncModuleExecutionFulfilled(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::AsyncModuleExecutionRejected:
        asyncModuleExecutionRejected(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::NewImportTicket:
        OPERATION_RETURN(scope, JSValue::encode(newImportTicket(*instance, argument(0))));
    case Typed::LanguageOperation::SetImportPromise:
        setImportPromise(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::EvaluateModule:
        OPERATION_RETURN(scope, JSValue::encode(evaluateModule(*instance, uncheckedDowncast<JSArray>(argument(0).asCell()), argument(1), argument(2))));
    case Typed::LanguageOperation::NewPrivateName: {
        String description = argument(0).toWTFString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(Symbol::create(vm, PrivateSymbolImpl::create(*description.impl()).get())));
    }
    case Typed::LanguageOperation::PrivateGet:
        OPERATION_RETURN(scope, JSValue::encode(privateGet(globalObject, vm, argument(0), asSymbol(argument(1))->privateName())));
    case Typed::LanguageOperation::PrivateSet:
        privateSet(globalObject, vm, argument(0), asSymbol(argument(1))->privateName(), argument(2));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::PrivateHas:
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(privateHas(globalObject, vm, argument(0), asSymbol(argument(1))->privateName()))));
    case Typed::LanguageOperation::PrivateFieldAdd:
        privateAdd(globalObject, vm, asObject(argument(0)), asSymbol(argument(1))->privateName(), argument(2), 0);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::PrivateMethodsAdd: {
        JSObject* holder = asObject(argument(1));
        Vector<RefPtr<UniquedStringImpl>, 4> names;
        holder->structure()->forEachProperty(vm, [&](const auto& entry) {
            names.append(entry.key());
            return true;
        });
        for (auto& name : names) {
            unsigned attributes;
            JSValue value = holder->getDirect(vm, name.get(), attributes);
            privateAdd(globalObject, vm, asObject(argument(0)), name.get(), value, attributes);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    // It is expanded into stores before anything else is done to the program: see prepare().
    case Typed::LanguageOperation::Initialize:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewPromise, JSCell*, (Instance* instance))
{
    return JSPromise::create(*instance->vm, instance->globalObject->promiseStructure());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedResolvePromise, void, (Instance* instance, JSCell* promise, EncodedJSValue resolution))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    uncheckedDowncast<JSPromise>(promise)->resolve(globalObject, vm, JSValue::decode(resolution));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedAwait, void, (Instance* instance, JSCell* resumeClosure, EncodedJSValue awaited, uint32_t isAsyncFunction))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    InternalMicrotask task = InternalMicrotask::AOTAsyncFunctionResume;
    if (!isAsyncFunction) {
        uncheckedDowncast<Typed::AsyncGenerator>(resumeClosure)->willSuspendAtAwait();
        task = InternalMicrotask::AOTAsyncGeneratorResume;
    }
    JSValue value = JSValue::decode(awaited);
    if (!value.isObject()) {
        globalObject->queueMicrotask(vm, task, static_cast<uint8_t>(JSPromise::Status::Fulfilled), jsUndefined(), value, resumeClosure, AsyncContextSwapScope::current(vm, globalObject));
        OPERATION_RETURN(scope);
    }
    if (value.asCell()->structureID() == globalObject->promiseStructure()->id()) {
        uncheckedDowncast<JSPromise>(value)->performPromiseThenWithInternalMicrotask(vm, task, nullptr, resumeClosure, AsyncContextSwapScope::current(vm, globalObject));
        OPERATION_RETURN(scope);
    }
    if (Typed::driveAsyncIterator(globalObject, vm, value.asCell(), asObject(resumeClosure)))
        OPERATION_RETURN(scope);
    JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, value, task, resumeClosure);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConstruct, EncodedJSValue, (Instance* instance, EncodedJSValue callee, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue constructor = JSValue::decode(callee);
    auto constructData = JSC::getConstructData(constructor);
    if (constructData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, makeString(State::describe(constructor), " is not a constructor"_s));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    MarkedArgumentBuffer arguments;
    fill(arguments, count, values);
    OPERATION_RETURN(scope, JSValue::encode(construct(globalObject, constructor, constructData, arguments)));
}

#define AOT_DEFINE_TYPED_BINARY(name, function) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue left, EncodedJSValue right)) \
    { \
        AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance); \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, JSValue::decode(left), JSValue::decode(right)))); \
    }
AOT_DEFINE_TYPED_BINARY(operationAOTTypedAdd, jsAdd)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedSub, jsSub)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedMul, jsMul)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedDiv, jsDiv)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedMod, jsRemainder)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedExp, jsPow)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedBitAnd, jsBitwiseAnd)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedBitOr, jsBitwiseOr)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedBitXor, jsBitwiseXor)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedShl, jsLShift)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedShr, jsRShift)
AOT_DEFINE_TYPED_BINARY(operationAOTTypedUshr, jsURShift)
#undef AOT_DEFINE_TYPED_BINARY

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNeg, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue numeric = JSValue::decode(operand).toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::unaryMinus(globalObject, numeric.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(-numeric.asNumber())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedBitNot, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(jsBitwiseNot(globalObject, JSValue::decode(operand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToNumber, double, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::decode(operand).toNumber(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToNumeric, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(operand).toNumeric(globalObject)));
}

#define AOT_DEFINE_TYPED_COMPARISON(name, expression) \
    JSC_DEFINE_JIT_OPERATION(name, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        OPERATION_RETURN(scope, expression); \
    }
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedLess, jsLess<true>(globalObject, left, right))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedLessEq, jsLessEq<true>(globalObject, left, right))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedGreater, jsLess<false>(globalObject, right, left))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedGreaterEq, jsLessEq<false>(globalObject, right, left))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedStrictEq, JSValue::strictEqual(globalObject, left, right))
AOT_DEFINE_TYPED_COMPARISON(operationAOTTypedLooseEq, JSValue::equal(globalObject, left, right))
#undef AOT_DEFINE_TYPED_COMPARISON

JSC_DEFINE_JIT_OPERATION(operationAOTTypedInstanceOf, size_t, (Instance* instance, EncodedJSValue value, EncodedJSValue constructor))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue right = JSValue::decode(constructor);
    if (!right.isObject()) {
        throwException(globalObject, scope, createInvalidInstanceofParameterErrorNotFunction(globalObject, right));
        OPERATION_RETURN(scope, 0);
    }
    OPERATION_RETURN(scope, asObject(right)->hasInstance(globalObject, JSValue::decode(value)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTruthy, size_t, (Instance* instance, EncodedJSValue operand))
{
    return JSValue::decode(operand).toBoolean(instance->globalObject);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTypeOf, JSCell*, (Instance* instance, EncodedJSValue operand))
{
    return jsTypeStringForValue(instance->globalObject, JSValue::decode(operand));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTypeOfObject, size_t, (Instance* instance, JSCell* object))
{
    if (object->structure()->masqueradesAsUndefined(instance->globalObject))
        return static_cast<size_t>(TypeofType::Undefined);
    return static_cast<size_t>(object->isCallable() ? TypeofType::Function : TypeofType::Object);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIsCallable, size_t, (Instance*, JSCell* cell))
{
    return cell->isCallable();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToString, JSCell*, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::decode(operand).toString(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToKey, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(operand).toPropertyKeyValue(globalObject)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFmod, double, (double a, double b))
{
    return Math::fmodDouble(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedPow, double, (double a, double b))
{
    return operationMathPow(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedToInt32, size_t, (double value))
{
    return static_cast<uint32_t>(toInt32(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedAllocSlow, JSCell*, (Instance* instance, uint32_t allocation))
{
    return instance->typed->allocate(*instance->vm, allocation);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedMaterializeEnvironment, JSCell*, (Instance* instance, uint32_t allocation, const EncodedJSValue* source))
{
    JSCell* cell = instance->typed->allocate(*instance->vm, allocation);
    size_t size = instance->typed->layoutOfShape(instance->typed->allocation(allocation).shape).size;
    memcpy(reinterpret_cast<uint8_t*>(cell) + sizeof(JSCell), reinterpret_cast<const uint8_t*>(source) + sizeof(JSCell), size - sizeof(JSCell));
    instance->vm->mutatorFence();
    return cell;
}

// What Stub::NewTypedClosure0 and the like do, where the allocator has nothing at hand. The stub has what is captured in memory, where the collector finds it.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewClosureSlow, JSCell*, (Instance* instance, uint64_t request, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth))
{
    uint64_t captures[] = { first, second, third, fourth };
    RELEASE_ASSERT(ClosureRequest::countOf(request) <= (ClosureRequest::isSlim(request) ? ClosureRequest::maxNumberOfCapturesOfSlimClosure : ClosureRequest::maxNumberOfCaptures));
    JSCell* cell;
    auto word = [&](ptrdiff_t offset) -> uint64_t& {
        return *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(cell) + offset);
    };
    auto made = [&] {
        if (ClosureRequest::isMadeOnce(request)) {
            if (ClosureRequest::variableIsCell32(request))
                *std::bit_cast<uint32_t*>(second) = static_cast<uint32_t>(std::bit_cast<uint64_t>(cell) >> Typed::cell32Shift);
            else
                *std::bit_cast<uint64_t*>(second) = std::bit_cast<uint64_t>(cell);
            instance->vm->writeBarrier(std::bit_cast<JSCell*>(first));
        }
        return cell;
    };
    if (ClosureRequest::isSlim(request)) {
        cell = Typed::SlimClosure::createForCompiledCode(*instance->vm, instance->typed->slimClosureStructure(*instance->vm, ClosureRequest::functionOf(request)), Typed::SlimClosure::allocationSize(ClosureRequest::countOf(request)));
        for (unsigned slot = 0; slot < ClosureRequest::countOf(request); ++slot)
            word(Typed::SlimClosure::offsetOfCaptures() + slot * sizeof(uint64_t)) = captures[slot];
        WTF::storeStoreFence();
        word(Typed::SlimClosure::offsetOfWord()) = instance->typedFunctions[ClosureRequest::functionOf(request)];
        WTF::storeStoreFence();
        return made();
    }
    cell = instance->typed->allocate(*instance->vm, ClosureRequest::allocationOf(request));
    word(JSObject::butterflyOffset()) = 0;
    word(JSCallee::offsetOfScopeChain()) = std::bit_cast<uint64_t>(instance->globalObject);
    word(JSFunction::offsetOfExecutableOrRareData()) = std::bit_cast<uint64_t>(instance->typedExecutables[ClosureRequest::executableOf(request)]);
    for (unsigned slot = 0; slot < ClosureRequest::countOf(request); ++slot)
        word(Typed::Closure::offsetOfCaptures() + slot * sizeof(uint64_t)) = captures[slot];
    WTF::storeStoreFence();
    word(Typed::Closure::offsetOfEntry()) = instance->typedFunctions[ClosureRequest::functionOf(request)];
    WTF::storeStoreFence();
    return made();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerce, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t typeAndReason))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    OPERATION_RETURN(scope, JSValue::encode(state.coerce(globalObject, JSValue::decode(value), state.type(typeAndReason >> 4), static_cast<Typed::Format::CastReason>(typeAndReason & 15))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerceInPlace, void, (Instance* instance, EncodedJSValue value, uint32_t typeAndReason))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSValue result = state.coerce(globalObject, JSValue::decode(value), state.type(typeAndReason >> 4), static_cast<Typed::Format::CastReason>(typeAndReason & 15), { }, State::HandsBack::TheObjectItself);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    RELEASE_ASSERT(JSValue::encode(result) == value);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerceElement, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t index, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    JSValue element = array->getIndexQuickly(index);
    // It got into the array behind the type: nobody has cast it.
    JSValue converted = state.hold(globalObject, element, state.type(type));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (converted != element) {
        array->putDirectIndex(globalObject, index, converted);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(converted));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerceProperty, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue encodedValue, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSValue value = JSValue::decode(encodedValue);
    JSValue converted = state.hold(globalObject, value, state.type(type));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (converted != value && cell->type() == FinalObjectType) {
        JSObject* holder = asObject(cell);
        unsigned attributes = 0;
        if (holder->getDirect(vm, nameAt(instance, name), attributes) == value && !attributes)
            holder->putDirect(vm, nameAt(instance, name), converted);
    }
    OPERATION_RETURN(scope, JSValue::encode(converted));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedAuditCoerce, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t reason, uint32_t function, uint32_t lineAndColumn))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    OPERATION_RETURN(scope, JSValue::encode(state.coerce(globalObject, JSValue::decode(value), state.type(type), static_cast<Typed::Format::CastReason>(reason), { function, lineAndColumn })));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCountClosure, void, (Instance* instance, uint32_t function, uint32_t numberOfCaptures))
{
    instance->typed->countClosure(function, numberOfCaptures);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCountPlainAllocation, void, (Instance* instance, JSCell* cell, uint32_t function, uint32_t kindAndValue))
{
    instance->typed->countPlainAllocation(cell, function, kindAndValue);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDidCreateField, void, (Instance* instance, JSCell* cell, uint32_t name))
{
    State& state = *instance->typed;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(state, nameAt(instance, name & ~Typed::nameOfFieldCreatedWithoutStorage));
    RELEASE_ASSERT(field);
    object->didCreateField(state, *field, !(name & Typed::nameOfFieldCreatedWithoutStorage));
}

static void didCreateFieldAtSite(State& state, Typed::Object* object, const Typed::FieldData& field, Slot* cache)
{
    uint32_t orderWord = object->orderWord();
    object->didCreateField(state, field);
    // As the stub leaves it. What becomes of an object with extra properties goes by those too.
    if (!cache || SharedData::contains(cache) || orderWord & ~Typed::orderAndCountMask || Typed::orderOf(object->orderWord()) == Typed::keysAreInLayoutOrder)
        return;
    cache->offset = orderWord ^ object->orderWord();
    cache->pointer = std::bit_cast<void*>(static_cast<uint64_t>(Typed::shapeWordOf(object->layout().shape)) << 32 | orderWord);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDidCreateFieldAt, void, (Instance* instance, JSCell* cell, uint32_t offset, Slot* cache))
{
    State& state = *instance->typed;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    didCreateFieldAtSite(state, object, state.fieldAt(object->layout(), offset), cache);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedEnsureStorage, void, (Instance* instance, JSCell* cell, uint32_t name))
{
    State& state = *instance->typed;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(state, nameAt(instance, name));
    RELEASE_ASSERT(field);
    object->ensureStorageFor(*instance->vm, state, *field);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedPutNumberElsewhere, void, (Instance* instance, JSCell* cell, uint32_t name, double value))
{
    operationAOTTypedPutElsewhere(instance, cell, name, JSValue::encode(jsNumber(purifyNaN(value))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedPutElsewhere, void, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue value))
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(*instance->typed, nameAt(instance, name));
    RELEASE_ASSERT(field);
    object->storeElsewhere(*instance->vm, *field, JSValue::decode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetExtra, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    bool mayHaveFieldOfItsOwn = name & Typed::nameMayBeOfOwnField;
    name &= ~Typed::nameMayBeOfOwnField;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    if (instance->typed->isCounting()) [[unlikely]]
        instance->typed->countAccess(State::AccessKind::ReadExtra, removeCodePtrTag(callFrame->rawReturnPC()), nameAt(instance, name), object);
    if (mayHaveFieldOfItsOwn) [[unlikely]] {
        if (const Typed::FieldData* field = object->fieldNamed(*instance->typed, nameAt(instance, name))) {
            JSValue held = object->read(*field);
            OPERATION_RETURN(scope, JSValue::encode(held ? held : jsUndefined()));
        }
    }
    JSObject* extra = object->extraProperties();
    unsigned attributes;
    JSValue value = extra ? extra->getDirect(vm, nameAt(instance, name), attributes) : JSValue();
    if (!value)
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) {
        bool goesOn = instance->typed->reportOrThrow(globalObject, { .kind = "runtime-accessor"_s, .on = &object->layout(), .member = nameAt(instance, name).impl() }, makeString("Cannot read property '"_s, nameAt(instance, name).string(), "' of "_s, instance->typed->nameOf(object->layout()), ": TypeScript code of the program cannot call a getter that was added at run time"_s), State::InAudit::Ignored);
        OPERATION_RETURN(scope, goesOn ? JSValue::encode(jsUndefined()) : encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedFieldHasText, size_t, (Instance* instance, JSCell* cell, uint32_t name, uint32_t text))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    JSValue held = object->read(*object->fieldNamed(*instance->typed, nameAt(instance, name)));
    if (!held || !held.isString())
        OPERATION_RETURN(scope, false);
    // As operationAOTTypedStringEq.
    OPERATION_RETURN(scope, asString(held)->equal(globalObject, instance->typed->string(vm, text)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedHasExtra, size_t, (Instance* instance, JSCell* cell, uint32_t name))
{
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    if (name & Typed::nameMayBeOfOwnField) [[unlikely]] {
        name &= ~Typed::nameMayBeOfOwnField;
        if (const Typed::FieldData* field = object->fieldNamed(*instance->typed, nameAt(instance, name)))
            return !!object->read(*field);
    }
    JSObject* extra = object->extraProperties();
    return extra && extra->getDirectOffset(*instance->vm, nameAt(instance, name)) != invalidOffset;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPutExtra, void, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (instance->typed->isCounting()) [[unlikely]]
        instance->typed->countAccess(State::AccessKind::WriteExtra, removeCodePtrTag(callFrame->rawReturnPC()), nameAt(instance, name), cell);
    uncheckedDowncast<Typed::Object>(cell)->putExtraWithoutRunningCode(globalObject, nameAt(instance, name), JSValue::decode(value));
    OPERATION_RETURN(scope);
}

// OrdinaryHasInstance, for the prototype object of a class with a compiled layout.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedInheritsFromClass, size_t, (Instance* instance, JSCell* cell, uint32_t classIndex))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* prototypeOfClass = instance->typed->prototypeOfClassIfExists(classIndex);
    if (!prototypeOfClass || !cell->isObject())
        OPERATION_RETURN(scope, false);
    JSObject* object = asObject(cell);
    while (true) {
        JSValue prototype = object->getPrototype(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, false);
        if (!prototype.isObject())
            OPERATION_RETURN(scope, false);
        object = asObject(prototype);
        if (object == prototypeOfClass)
            OPERATION_RETURN(scope, true);
    }
}

// The literal as it is written, with or without its `n`.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedBigIntConstant, EncodedJSValue, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    String text = nameAt(instance, index).string();
    if (text.endsWith('n'))
        text = text.left(text.length() - 1);
    JSValue result = JSBigInt::parseInt(globalObject, text, JSBigInt::ErrorParseMode::ThrowExceptions);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringConstant, JSCell*, (Instance* instance, uint32_t index))
{
    return instance->typed->makeAndKeepStringConstant(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringMadeOnce, JSCell*, (Instance* instance, uint32_t index))
{
    return instance->typed->makeStringConstant(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedBarrierSlow, void, (Instance* instance, JSCell* cell))
{
    instance->vm->writeBarrierSlowPath(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedBarrierSlowKeepingRegisters, void, (Instance* instance, JSCell* cell))
{
    instance->vm->writeBarrierSlowPath(cell);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedRememberElement, void, (Instance* instance, JSCell* array, EncodedJSValue value))
{
    instance->typed->rememberElement(*instance->vm, asObject(array), JSValue::decode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPollSlow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN(scope);
}

static uint8_t* behindStackCheck(uint8_t* returnAddress)
{
    ImageAddressInfo function = classifyAddress(returnAddress - 1);
    RELEASE_ASSERT(function.kind == ImageAddressInfo::Function);
    uint8_t* start = returnAddress - 1 - function.offset;
#if CPU(ARM64)
    auto* call = reinterpret_cast<uint32_t*>(returnAddress) - 1;
    auto* instruction = reinterpret_cast<uint32_t*>(start);
    auto targetOfBranch = [](uint32_t* branch) { return branch + (static_cast<int32_t>(*branch << 6) >> 6); };
    auto isBranch = [](uint32_t* candidate) { return (*candidate & 0xfc000000) == 0x14000000; };
    if (isBranch(instruction))
        instruction = targetOfBranch(instruction);
    constexpr unsigned maxInstructionsInStackCheck = 8;
    for (unsigned i = 0; i < maxInstructionsInStackCheck; ++i, ++instruction) {
        bool isConditionalBranch = (*instruction & 0xff000010) == 0x54000000;
        if (isConditionalBranch && instruction + (static_cast<int32_t>(*instruction << 8) >> 13) == call)
            return reinterpret_cast<uint8_t*>(instruction + 1);
        if (isBranch(instruction) && targetOfBranch(instruction) == call)
            return reinterpret_cast<uint8_t*>(instruction + 1);
    }
#elif CPU(X86_64)
    constexpr unsigned sizeOfNearCall = 5;
    constexpr unsigned sizeOfConditionalJump = 6;
    constexpr unsigned sizeOfShortJump = 2;
    uint8_t* call = returnAddress - sizeOfNearCall;
    auto relative = [](uint8_t* at) { int32_t value; memcpy(&value, at, sizeof(value)); return value; };
    uint8_t* instruction = start;
    if (*instruction == 0xe9)
        instruction += sizeOfNearCall + relative(instruction + 1);
    else if (*instruction == 0xeb)
        instruction += sizeOfShortJump + static_cast<int8_t>(instruction[1]);
    constexpr unsigned maxBytesInStackCheck = 40;
    for (unsigned i = 0; i < maxBytesInStackCheck; ++i, ++instruction) {
        if (instruction[0] == 0x0f && instruction[1] == 0x82 && instruction + sizeOfConditionalJump + relative(instruction + 2) == call)
            return instruction + sizeOfConditionalJump;
        if (instruction[0] == 0x72 && instruction + sizeOfShortJump + static_cast<int8_t>(instruction[1]) == call)
            return instruction + sizeOfShortJump;
    }
#endif
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPollAtPrologue, void*, (Instance* instance, uint8_t* returnAddress))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, behindStackCheck(returnAddress));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrow, void, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwException(globalObject, scope, JSValue::decode(value));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCatch, EncodedJSValue, (Instance* instance))
{
    VM& vm = *instance->vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception);
    if (vm.isTerminationException(exception)) [[unlikely]]
        return JSValue::encode(JSValue());
    JSValue value = exception->value();
    scope.clearException();
    return JSValue::encode(value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCatchException, EncodedJSValue, (Instance* instance))
{
    VM& vm = *instance->vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception);
    if (vm.isTerminationException(exception)) [[unlikely]]
        return JSValue::encode(JSValue());
    scope.clearException();
    return JSValue::encode(exception);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedValueOfException, EncodedJSValue, (Instance*, EncodedJSValue thrown))
{
    auto* exception = dynamicDowncast<Exception>(JSValue::decode(thrown));
    return exception ? JSValue::encode(exception->value()) : thrown;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedRejectWithException, JSCell*, (Instance* instance))
{
    VM& vm = *instance->vm;
    // What the unwinder left there is a frame that is gone. To reject may call the host.
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception);
    JSValue value = exception->value();
    scope.clearException();
    JSPromise* promise = JSPromise::create(vm, instance->globalObject->promiseStructure());
    promise->reject(vm, value);
    return promise;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedPrepareCallOfProxy, uintptr_t, (Instance* instance, EncodedJSValue* hostFrame, uint32_t count))
{
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto decline = [&](const char* why) -> uintptr_t {
        countOperationNamed(instance, "call of a proxy", why);
        return 0;
    };
    const EncodedJSValue* thisAndArguments = hostFrame + static_cast<int>(CallFrameSlot::thisArgument);
    EncodedJSValue* results = hostFrame + CallFrame::headerSizeInRegisters + 1 + count;
    auto* proxy = uncheckedDowncast<ProxyObject>(JSValue::decode(hostFrame[static_cast<int>(CallFrameSlot::callee)]).asCell());
    if (!proxy->isCallable())
        return decline("it is not callable");
    JSValue handlerValue = proxy->handler();
    if (!handlerValue.isObject())
        return decline("it is revoked");
    JSObject* handler = asObject(handlerValue);
    if (handler->type() != FinalObjectType)
        return decline("the handler is no plain object");
    unsigned attributes;
    PropertyOffset offset = handler->structure()->get(vm, vm.propertyNames->builtinNames().applyPublicName(), attributes);
    if (!isValidOffset(offset) || attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessorOrValue))
        return decline("the handler has no `apply` of its own that is a value");
    JSValue trap = handler->getDirect(offset);
    if (!trap.isCallable())
        return decline("the trap is not callable");
    JSGlobalObject* globalObject = proxy->globalObject();
    if (globalObject->isHavingABadTime())
        return decline("arrays have indexed accessors to look at");
    JSArray* list;
    {
        ObjectInitializationScope initializationScope(vm);
        list = JSArray::tryCreateUninitializedRestricted(initializationScope, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), count);
        if (!list)
            return decline("there is no memory for the list");
        for (unsigned i = 0; i < count; ++i)
            list->initializeIndex(initializationScope, i, JSValue::decode(thisAndArguments[i + 1]));
    }
    countOperationNamed(instance, "call of a proxy", "the trap is called from the stub");
    results[0] = JSValue::encode(trap);
    results[1] = JSValue::encode(handler);
    results[2] = JSValue::encode(proxy->target());
    results[3] = JSValue::encode(JSValue::decode(thisAndArguments[0]).toThis(globalObject, ECMAMode::strict()));
    results[4] = JSValue::encode(list);
    return 1;
}

// In an audit build only tests that compiled code depends on still throw: see Lowering::ensure().
static ASCIILiteral auditPrefix(State& state)
{
    return state.data.isCompiledForAudit ? "audit stopped (compiled code depends on this check): "_s : ""_s;
}

static void throwTypeMismatch(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t type)
{
    State& state = *instance->typed;
    state.noteViolation(globalObject->vm(), { .kind = State::kindOfUnnamedViolation, .expected = &state.type(type), .got = value });
    throwTypeError(globalObject, scope, withAOTErrorNote(makeString(auditPrefix(state), state.mismatchMessage(globalObject->vm(), value, state.type(type)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHold, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue given = JSValue::decode(value);
    if (!given || !instance->typed->fits(instance->typed->type(type), given)) [[unlikely]]
        throwTypeMismatch(instance, globalObject, scope, given ? given : jsUndefined(), type);
    OPERATION_RETURN(scope, value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByNameHeld, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t name, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = asObject(cell)->get(globalObject, nameAt(instance, name));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!instance->typed->fits(instance->typed->type(type), value)) [[unlikely]]
        throwTypeMismatch(instance, globalObject, scope, value, type);
    OPERATION_RETURN(scope, JSValue::encode(value));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByNameHeldAsNumber, double, (Instance* instance, JSCell* cell, uint32_t name, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = asObject(cell)->get(globalObject, nameAt(instance, name));
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!instance->typed->fits(instance->typed->type(type), value) || !(value.isNumber() || value.isUndefinedOrNull())) [[unlikely]] {
        throwTypeMismatch(instance, globalObject, scope, value, type);
        OPERATION_RETURN(scope, 0);
    }
    if (value.isNumber())
        OPERATION_RETURN(scope, purifyNaN(value.asNumber()));
    OPERATION_RETURN(scope, std::bit_cast<double>(value.isUndefined() ? Typed::undefinedDouble : Typed::nullDouble));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNoteMisfit, void, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t parameter, uint32_t isAtCaller))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSValue given = JSValue::decode(value);
    state.noteViolation(vm, { .kind = isAtCaller ? "enter-argument"_s : "enter-function"_s, .what = isAtCaller ? "As"_s : "Claim"_s, .expected = &state.type(type), .got = given ? given : jsUndefined(), .parameter = parameter == UINT32_MAX ? std::nullopt : std::optional<unsigned>(parameter), .isAtCaller = !!isAtCaller, .action = State::ActionOnViolation::Continued });
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHoldOrBecome, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSValue given = JSValue::decode(value);
    if (given && state.fits(state.type(type), given))
        OPERATION_RETURN(scope, value);
    bool hasBecomeIt = given && given.isObject() && state.becomeWhereItIs(globalObject, asObject(given), state.type(type));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!hasBecomeIt)
        throwTypeMismatch(instance, globalObject, scope, given ? given : jsUndefined(), type);
    OPERATION_RETURN(scope, value);
}

static void throwKindMismatch(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t kinds, uint32_t name)
{
    State& state = *instance->typed;
    state.noteViolation(globalObject->vm(), { .kind = State::kindOfUnnamedViolation, .expectedKinds = kinds, .expectedName = name, .got = value });
    throwTypeError(globalObject, scope, withAOTErrorNote(makeString(auditPrefix(state), "expected "_s, state.describeKinds(kinds, name), ", got "_s, State::describe(value))));
}

static void throwNonNullFailed(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t type, uint32_t text)
{
    State& state = *instance->typed;
    state.noteViolation(globalObject->vm(), { .kind = "non-null"_s, .what = "NonNull"_s, .expected = &state.type(type), .got = value });
    throwTypeError(globalObject, scope, withAOTErrorNote(value.isUndefinedOrNull() ? State::nonNullMessage(value, text == UINT32_MAX ? String() : state.text(text)) : state.mismatchMessage(globalObject->vm(), value, state.type(type))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedMethodOfTable, EncodedJSValue, (Instance* instance, JSCell* table, uint32_t which))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    TableMethod method = static_cast<TableMethod>(which);
    bool isOfSet = method == TableMethod::SetHas || method == TableMethod::SetAdd;
    const Identifier* name = &vm.propertyNames->has;
    Intrinsic own = isOfSet ? JSSetHasIntrinsic : JSMapHasIntrinsic;
    if (method == TableMethod::MapGet) {
        name = &vm.propertyNames->get;
        own = JSMapGetIntrinsic;
    } else if (method == TableMethod::MapSet) {
        name = &vm.propertyNames->set;
        own = JSMapSetIntrinsic;
    } else if (method == TableMethod::SetAdd) {
        name = &vm.propertyNames->add;
        own = JSSetAddIntrinsic;
    }
    JSValue found;
    if (MegamorphicCache* cache = vm.megamorphicCache()) {
        if (auto* known = cache->findLoad(table->structureID(), name->impl()))
            found = !known->m_holder ? jsUndefined() : asObject(known->m_holder == JSCell::seenMultipleCalleeObjects() ? table : known->m_holder)->getDirect(known->m_offset);
    }
    if (!found) {
        PropertySlot slot(table, PropertySlot::InternalMethodType::Get);
        found = getByIdAndFillMegamorphicCache(globalObject, table, *name, slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    if (!found || !found.isCell()) {
        throwNotAFunction(globalObject, scope, *name, found ? found : jsUndefined());
        OPERATION_RETURN(scope, encodedJSValue());
    }
    auto* function = dynamicDowncast<JSFunction>(found.asCell());
    if (function && function->isHostFunction() && function->intrinsic() == own && table->type() == (isOfSet ? JSSetType : JSMapType))
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(found));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasByName, EncodedJSValue, (Instance* instance, JSCell* table, EncodedJSValue key, EncodedJSValue method))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue answer = callValue(globalObject, vm, JSValue::decode(method), table, 1, &key);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!answer.isBoolean()) {
        throwKindMismatch(instance, globalObject, scope, answer, Typed::Primitive::Boolean, UINT32_MAX);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(answer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedAssertionFailed, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t what, uint32_t function, uint32_t lineAndColumn))
{
    State& state = *instance->typed;
    return JSValue::encode(state.assertionFailed(JSValue::decode(value), state.type(type), what, { function, lineAndColumn }));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStoreAssertionFailed, EncodedJSValue, (Instance* instance, EncodedJSValue value, EncodedJSValue storedTo, uint32_t type, uint32_t function, uint32_t lineAndColumn))
{
    State& state = *instance->typed;
    return JSValue::encode(state.assertionFailed(JSValue::decode(value), state.type(type), static_cast<uint32_t>(Typed::Format::CastReason::Store), { function, lineAndColumn }, { }, JSValue::decode(storedTo)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowReachedUnreachable, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwTypeError(globalObject, scope, "audit stopped: reached code the types call unreachable (a function declared to return 'never' returned)"_s);
    OPERATION_RETURN(scope);
}

static void throwNullish(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, JSValue value, uint32_t name, Typed::NullishAccess access)
{
    if (access == Typed::NullishAccess::Iterate)
        throwTypeError(globalObject, scope, makeString(value.isNull() ? "null"_s : "undefined"_s, " is not iterable"_s));
    else
        throwTypeError(globalObject, scope, State::nullishMessage(value, name == UINT32_MAX ? String() : nameAt(instance, name).string(), access == Typed::NullishAccess::Write));
}

static uint8_t learnPlaceOfRun(VM& vm, Instance* instance, Slot* header, Structure* structure, unsigned index, bool isStore, uint32_t name)
{
    header->offset |= 1u << index;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, nameAt(instance, name).impl(), attributes);
    bool hasPlace = isValidOffset(offset) && isInlineOffset(offset) && !(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::CustomValue)) && !(isStore && (attributes & PropertyAttribute::ReadOnly || structure->mayBePrototype()));
    if (Options::useAOTOperationCounters()) [[unlikely]]
        runtimeTable(vm).countOperation("operationAOTTypedLearnPlace", hasPlace ? "learned" : "refused");
    if (!hasPlace)
        return 0;
    if (isStore)
        structure->didCachePropertyReplacement(vm, offset);
    uint8_t place = static_cast<uint8_t>(offset + JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue));
    std::bit_cast<uint8_t*>(&header[index / 8].pointer)[index % 8] = place;
    return place;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedLearnRun, size_t, (Instance* instance, Slot* header, EncodedJSValue encodedBase, uint32_t slotsAndStored, uint64_t firstNames, uint64_t nextNames))
{
    unsigned slots = slotsAndStored >> 24;
    if (SharedData::contains(header) || (header->offset & Slot::attemptsMask) == Slot::attemptsMask)
        return 0;
    auto counted = [&](bool hasLearned) -> size_t {
        if (Options::useAOTOperationCounters()) [[unlikely]]
            runtimeTable(*instance->vm).countOperation("operationAOTTypedLearnRun", hasLearned ? "learned" : "refused");
        return hasLearned;
    };
    header->offset = (header->offset & ~0xffffffu) + (1u << Slot::attemptsShift);
    if ((header->offset & Slot::attemptsMask) >> Slot::attemptsShift >= 4)
        header->offset |= Slot::attemptsMask;
    header->structureID = StructureID();
    for (unsigned i = 0; i < slots; ++i)
        header[i].pointer = nullptr;
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isCell() || base.asCell()->type() != FinalObjectType)
        return counted(false);
    Structure* structure = base.asCell()->structure();
    if (structure->compiledLayout() || structure->isDictionary() || !structure->propertyAccessesAreCacheable() || structure->needImpurePropertyWatchpoint())
        return counted(false);
    header->structureID = structure->id();
    VM& vm = *instance->vm;
    if (Data* data = instance->dataWithSlot(header, nullptr))
        didFillSlot(vm, data);
    DeferGCForAWhile deferGC(vm);
    for (unsigned index = 0; index < 6; ++index) {
        uint32_t name = static_cast<uint32_t>((index < 3 ? firstNames : nextNames) >> (index % 3 * 21)) & 0x1fffff;
        if (name == 0x1fffff)
            continue;
        learnPlaceOfRun(vm, instance, header, structure, index, slotsAndStored >> index & 1, name);
    }
    return counted(true);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedLearnPlace, size_t, (Instance* instance, Slot* header, EncodedJSValue encodedBase, uint32_t indexAndIsStore, uint32_t name))
{
    VM& vm = *instance->vm;
    unsigned index = indexAndIsStore & 0xff;
    JSValue base = JSValue::decode(encodedBase);
    if (SharedData::contains(header) || !base.isCell() || base.asCell()->structureID() != header->structureID)
        return 0;
    DeferGCForAWhile deferGC(vm);
    return learnPlaceOfRun(vm, instance, header, base.asCell()->structure(), index, indexAndIsStore >> 8, name);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFactDoesNotHold, void, (Instance* instance, EncodedJSValue value, uint32_t function, uint32_t number))
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashSet<uint64_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> reported;
    {
        Locker locker { lock };
        if (!reported->add(static_cast<uint64_t>(function) << 32 | number).isNewEntry)
            return;
    }
    DeferGCForAWhile deferGC(*instance->vm);
    JSValue found = JSValue::decode(value);
    if (!found) {
        dataLogLn("AOT: type fact violated: f", function, " v", number, " is the empty value");
        return;
    }
    bool isDescribedByItsPrototype = found.isObject() && !dynamicDowncast<Typed::Object>(found) && !asObject(found)->isCallable() && !dynamicDowncast<JSArray>(found);
    dataLogLn("AOT: type fact violated: f", function, " v", number, " is ", isDescribedByItsPrototype ? makeString("object "_s, asObject(found)->classInfo()->className) : State::describe(found));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCheckKnownPlace, size_t, (Instance* instance, EncodedJSValue encodedBase, uint32_t name, uint32_t placeAndFlags, uint32_t function, uint32_t number))
{
    constexpr uint32_t isNeverNil = 0x100;
    constexpr uint32_t isStore = 0x200;
    constexpr uint32_t isFirstStore = 0x400;
    VM& vm = *instance->vm;
    DeferGCForAWhile deferGC(vm);
    JSValue base = JSValue::decode(encodedBase);
    if (base.isUndefinedOrNull() && !(placeAndFlags & isNeverNil))
        return 0;
    if (base.isCell() && base.asCell()->type() == FinalObjectType) {
        Structure* structure = base.asCell()->structure();
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, nameAt(instance, name).impl(), attributes);
        unsigned refused = static_cast<unsigned>(placeAndFlags & isStore ? PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue : PropertyAttribute::AccessorOrCustomAccessorOrValue);
        if (offset == static_cast<PropertyOffset>(placeAndFlags & 0xff) && static_cast<unsigned>(offset) < structure->inlineCapacity() && !(attributes & refused) && !(placeAndFlags & isFirstStore && attributes) && !(placeAndFlags & isStore && structure->mayBePrototype()))
            return 1;
    }
    operationAOTTypedFactDoesNotHold(instance, encodedBase, function, number);
    return 0;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowStackOverflow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwStackOverflowError(globalObject, scope);
    OPERATION_RETURN(scope);
}

static void throwTDZError(Instance* instance, JSGlobalObject* globalObject, ThrowScope& scope, uint32_t name)
{
    if (name != UINT32_MAX && instance->typed->data.stringFacts(name) & Typed::Data::stringIsPrivateName) {
        throwException(globalObject, scope, createInvalidPrivateNameError(globalObject));
        return;
    }
    String variable = name == UINT32_MAX ? String() : nameAt(instance, name).string();
    throwException(globalObject, scope, createReferenceError(globalObject, variable.isEmpty() ? "Cannot access variable before initialization"_str : makeString("Cannot access '"_s, variable, "' before initialization."_s)));
}

// One operation, so that the frame it takes for its caller's is that of compiled code: DECLARE_CALL_FRAME() is the frame above, whoever that is.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowAtSite, void, (Instance* instance, const uint64_t* registers, const uint32_t* site, uint32_t isTrap))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    // From ThrowAtFault the return address is behind the access, which no call is.
    auto faultingAccess = isTrap ? std::nullopt : faultingAccessAt(reinterpret_cast<const uint8_t*>(site) - sizeOfNearCall);
    if (faultingAccess)
        site = faultingAccess->throwSite.data();
    uint32_t wordInRegister = static_cast<uint32_t>(registers[static_cast<unsigned>(throwWordGPR)]);
    if (isTrap == ThrowSite::wordIsInRegister)
        site = &wordInRegister;
    else if (isTrap)
        site = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(site) - offsetOfReturnAddressInTrap + 1);
    uint32_t word = *site++;
    uint32_t a = word & ThrowSite::hasWideA ? *site++ : (word & INT32_MAX) >> ThrowSite::aShift;
    if (a == ThrowSite::none)
        a = UINT32_MAX;
    uint32_t b = word & ThrowSite::hasB ? *site++ : UINT32_MAX;
    unsigned number = word >> ThrowSite::registerShift & 31;
    if (static_cast<Typed::Rep>(word >> ThrowSite::repShift & 7) == Typed::Rep::Void && number) {
        ErrorType errorType = static_cast<ErrorType>(number - 1);
        constexpr bool useCurrentFrame = true;
        auto* error = ErrorInstance::create(globalObject, globalObject->errorStructure(errorType), instance->typed->string(vm, a), jsUndefined(), nullptr, TypeNothing, errorType, useCurrentFrame);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        throwException(globalObject, scope, error);
        OPERATION_RETURN(scope);
    }
    uint64_t bits = registers[number];
    // The base holds nil as a pointer: the bits of the boxed value, or what its 32-bit form is decoded to.
    if (faultingAccess && bits >= FaultingAccessInImage::reach)
        bits = (bits - MarkedBlockCage::base()) >> MarkedBlockCage::shift;
    JSValue value;
    switch (static_cast<Typed::Rep>(word >> ThrowSite::repShift & 7)) {
    case Typed::Rep::Void:
        value = jsUndefined();
        break;
    case Typed::Rep::Int32:
        value = jsBoolean(bits & 1);
        break;
    case Typed::Rep::Int64:
        value = jsNumber(static_cast<double>(static_cast<int64_t>(bits)));
        break;
    case Typed::Rep::Double:
        value = jsNumber(purifyNaN(std::bit_cast<double>(registers[ThrowSite::numberOfRegistersPerBank + number])));
        break;
    case Typed::Rep::Cell:
    case Typed::Rep::JSValue:
        value = JSValue::decode(bits);
        // The word of a slot that holds nothing: see Typed::Lowering::thrown().
        if (!value)
            value = jsUndefined();
        break;
    // What stands for nothing reads `undefined`.
    case Typed::Rep::OptionalInt64:
        if (static_cast<int64_t>(bits) == Typed::nullInteger)
            value = jsNull();
        else if (static_cast<int64_t>(bits) == Typed::undefinedInteger || static_cast<int64_t>(bits) == Typed::absentInteger)
            value = jsUndefined();
        else
            value = jsNumber(static_cast<double>(static_cast<int64_t>(bits)));
        break;
    case Typed::Rep::OptionalDouble:
        bits = registers[ThrowSite::numberOfRegistersPerBank + number];
        if (bits == Typed::nullDouble)
            value = jsNull();
        else if (bits == Typed::undefinedDouble || bits == Typed::absentDouble)
            value = jsUndefined();
        else
            value = jsNumber(purifyNaN(std::bit_cast<double>(bits)));
        break;
    }
    switch (static_cast<ThrowKind>(word & 7)) {
    case ThrowKind::TypeMismatch:
        throwTypeMismatch(instance, globalObject, scope, value, a);
        break;
    case ThrowKind::KindMismatch:
        throwKindMismatch(instance, globalObject, scope, value, a, b);
        break;
    case ThrowKind::NullishRead:
        throwNullish(instance, globalObject, scope, value, a, Typed::NullishAccess::Read);
        break;
    case ThrowKind::NullishWrite:
        throwNullish(instance, globalObject, scope, value, a, Typed::NullishAccess::Write);
        break;
    case ThrowKind::NullishIterate:
        throwNullish(instance, globalObject, scope, value, a, Typed::NullishAccess::Iterate);
        break;
    case ThrowKind::NonNullFailed:
        throwNonNullFailed(instance, globalObject, scope, value, a, b);
        break;
    case ThrowKind::TDZ:
        throwTDZError(instance, globalObject, scope, a);
        break;
    case ThrowKind::NotAFunction:
        // Without a name: as callValue() says it.
        if (a == UINT32_MAX)
            throwTypeError(globalObject, scope, makeString(errorDescriptionForValue(globalObject, value), " is not a function"_s));
        else
            throwNotAFunction(globalObject, scope, nameAt(instance, a), value);
        break;
    }
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewObject, JSCell*, (Instance* instance))
{
    return constructEmptyObject(instance->globalObject);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewObjectForKeys, JSCell*, (Instance* instance, uint32_t hasNullPrototype))
{
    VM& vm = *instance->vm;
    JSGlobalObject* globalObject = instance->globalObject;
    JSObject* object = hasNullPrototype ? constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure()) : constructEmptyObject(globalObject);
    object->convertToDictionary(vm);
    return object;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewObjectWithRoom, JSCell*, (Instance* instance, uint32_t inlineCapacity))
{
    JSGlobalObject* globalObject = instance->globalObject;
    return constructEmptyObject(*instance->vm, globalObject->structureCache().emptyObjectStructureForPrototype(globalObject, globalObject->objectPrototype(), inlineCapacity));
}

// What an object of the structure has for a structure once properties of the names have been added to it, in that order, each behind the last. Null: something on the prototype chain may take or refuse a store of one of them, or the structure does not take them that way.
// No structure is made for the steps between. Every row of the same names comes to the same one, whoever asks.
static Structure* structureBehindRun(Instance* instance, VM& vm, Structure* structure, std::span<UniquedStringImpl* const> names)
{
    auto& target = instance->propertyRunTarget(structure, instance->canonicalPropertyRun(names), [&](Vector<UniquedStringImpl*, 16>& result) {
        result.append(names);
    });
    JSValue prototype = structure->storedPrototype();
    for (StructureID expected : target.prototypeStructures) {
        if (!prototype.isObject() || asObject(prototype)->structureID() != expected)
            return nullptr;
        prototype = expected.decode()->storedPrototype();
    }
    Structure* last = target.last;
    if (prototype.isObject() || !last)
        return nullptr;
    // See Instance::canonicalPropertyRun(). The names of the program stay; a symbol may have gone and another be at its address.
    PropertyOffset offset = structure->maxOffset();
    for (UniquedStringImpl* name : names) {
        offset = Instance::offsetAfter(offset, structure->inlineCapacity());
        if (name->isSymbol() && last->get(vm, name) != offset)
            return nullptr;
    }
    return last;
}

// Ahead of a fill of the cache of a CreateThis. Each counts: closures of one function may take turns at a site.
static bool mayFillForThis(Instance* instance, Data* data, Slot* cache)
{
    if (SharedData::contains(cache) || (cache[0].offset & Slot::attemptsMask) == Slot::attemptsMask)
        return false;
    cache[0].offset += 1u << Slot::attemptsShift;
    cache[0].clear();
    stopWatching(data, cache);
    // The epoch first: a slot that is filled is believed.
    cache[2].pointer = std::bit_cast<void*>(static_cast<uintptr_t>(instance->typedPrototypeEpoch));
    return true;
}

static uint32_t placesForStubToClear(Structure* structure, uint32_t numberInRegisters)
{
    uint32_t places = WTF::roundUpToMultipleOf<2>(structure->inlineCapacity());
    return places > WTF::roundUpToMultipleOf<2>(numberInRegisters) ? places : 0;
}

// An object of the structure, which has no properties, with those defined on it. `constructor`: whose `this` it is, if it is that: see Lowering::lowerObjectLiteral().
UniquedStringImpl* nameInRow(VM& vm, unsigned number, unsigned index)
{
    Image* image = Image::withCode();
    auto& data = *image->at<Typed::Data>(image->header().typedDataOffset);
    uint32_t facts = data.stringFacts(data.row(number)[1 + index]);
    RELEASE_ASSERT(facts & Typed::Data::stringHasIdentifierNumber);
    const Identifier* name = VMProgram::of(vm)->identifierIfMade(facts & Typed::Data::stringHashMask);
    RELEASE_ASSERT(name);
    return name->impl();
}

static JSObject* newObjectWithFields(Instance* instance, JSGlobalObject* globalObject, VM& vm, Data* data, Slot* cache, Structure* empty, JSCell* constructor, uint32_t count, const EncodedJSValue* values, const uint32_t* numbersOfNames, uint32_t numberInRegisters, uint32_t numberOfRowPlusOne = 0)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (globalObject != instance->globalObject)
        numberOfRowPlusOne = 0;
    // One structure for the row of names, with none made on the way.
    if (count > 1 || (count && numberOfRowPlusOne)) {
        bool isOrdinaryLiteral = !constructor && empty->storedPrototype() == JSValue(globalObject->objectPrototype());
        Vector<UniquedStringImpl*, 16> names;
        bool hasIndex = false;
        for (unsigned i = 0; i < count; ++i) {
            const Identifier& name = nameAt(instance, numbersOfNames[i]);
            hasIndex |= !!parseIndex(name);
            names.append(name.impl());
        }
        // Null: a name comes twice.
        if (Structure* structure = hasIndex ? nullptr : isOrdinaryLiteral ? instance->literalStructure(empty, names.span(), numberOfRowPlusOne) : structureBehindRun(instance, vm, empty, names.span())) {
            unsigned inlineCapacity = structure->inlineCapacity();
            if (count > inlineCapacity) {
                JSObject* object = JSFinalObject::createWithButterfly(vm, structure, Butterfly::create(vm, nullptr, 0, structure->outOfLineCapacity(), false, IndexingHeader(), 0));
                for (unsigned i = 0; i < count; ++i)
                    object->putDirectOffset(vm, i < inlineCapacity ? i : firstOutOfLineOffset + (i - inlineCapacity), JSValue::decode(values[i]));
                return object;
            }
            JSObject* object = constructEmptyObject(vm, structure);
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, i, JSValue::decode(values[i]));
            if (!constructor || mayFillForThis(instance, data, cache))
                fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), placesForStubToClear(structure, numberInRegisters), constructor);
            return object;
        }
    }
    JSObject* object = constructEmptyObject(vm, empty);
    bool areInOrder = true;
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& name = nameAt(instance, numbersOfNames[i]);
        JSValue value = JSValue::decode(values[i]);
        if (auto index = parseIndex(name)) {
            object->putDirectIndex(globalObject, *index, value);
            RETURN_IF_EXCEPTION(scope, nullptr);
            areInOrder = false;
            continue;
        }
        PutPropertySlot slot(object, true, PutPropertySlot::PutById);
        object->putDirect(vm, name, value, slot);
        areInOrder = areInOrder && slot.isCacheablePut() && slot.type() == PutPropertySlot::NewProperty && slot.cachedOffset() == static_cast<PropertyOffset>(i);
    }
    Structure* structure = object->structure();
    if (areInOrder && !structure->isDictionary() && count <= structure->inlineCapacity() && !object->butterfly()) {
        if (constructor && !mayFillForThis(instance, data, cache))
            return object;
        fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), placesForStubToClear(structure, numberInRegisters), constructor);
    }
    return object;
}

static Structure* emptyStructureOfLiteral(Instance* instance, JSGlobalObject* globalObject, VM& vm, bool hasNullPrototype, unsigned inlineCapacity)
{
    if (hasNullPrototype)
        return instance->emptyStructureWithoutPrototype(inlineCapacity);
    bool isRemembered = inlineCapacity < std::size(instance->emptyStructuresOfLiterals) && globalObject == instance->globalObject;
    if (isRemembered && instance->emptyStructuresOfLiterals[inlineCapacity])
        return instance->emptyStructuresOfLiterals[inlineCapacity];
    ObjectAllocationProfile profile;
    profile.initializeProfile(vm, globalObject, globalObject, globalObject->objectPrototype(), inlineCapacity);
    if (isRemembered)
        instance->emptyStructuresOfLiterals[inlineCapacity] = profile.structure();
    return profile.structure();
}

// From Stub::NewObjectLiteral<n>, by operationAOTNewObjectLiteral: the slot is empty, and the literal is of typed code. The values are those the stub takes in registers: the code stores the others into what comes back.
JSObject* newObjectLiteralAtSite(Instance* instance, JSGlobalObject* globalObject, VM& vm, FunctionRef function, Slot* cache, const EncodedJSValue* values, uint32_t numberOfValues)
{
    const uint32_t* row = function.info().function()->plans() + function.siteConstantOf(cache + 1) - 1;
    uint32_t count = *row & 0xffff;
    RELEASE_ASSERT(numberOfValues <= std::min(count, maxLiteralPropertiesInRegisters) && count <= maxPropertiesNamedBySite);
    std::array<EncodedJSValue, maxPropertiesNamedBySite> all;
    for (uint32_t i = 0; i < count; ++i)
        all[i] = i < numberOfValues ? values[i] : JSValue::encode(jsUndefined());
    Data* data = function.instance->dataIfExists(function.index);
    JSObject* object = newObjectWithFields(instance, globalObject, vm, data ? data : function.instance->sharedData, cache, emptyStructureOfLiteral(instance, globalObject, vm, *row >> 31, std::max(count, *row >> 16 & 0x7fff)), nullptr, count, all.data(), row + 1, numberOfValues, row[1 + count]);
    // What the stores behind the stub's call rest on.
    RELEASE_ASSERT(object && !object->butterfly() && count <= object->structure()->inlineCapacity());
    return object;
}

JSObject* newObjectOfRow(Instance* instance, JSGlobalObject* globalObject, VM& vm, uint32_t number, const EncodedJSValue* values, uint32_t numberOfValues)
{
    std::span<const uint32_t> row = reinterpret_cast<const Typed::Data*>(instance->typedData)->row(number);
    uint32_t count = row[0] & 0xff;
    uint32_t roomInLine = row[0] >> 8 & 0xff;
    bool hasNullPrototype = row[0] >> 16 & 1;
    RELEASE_ASSERT(row.size() == count + 1 && numberOfValues == std::min(count, maxLiteralPropertiesInRegisters) && count <= maxPropertiesNamedBySite);
    std::array<EncodedJSValue, maxPropertiesNamedBySite> all;
    for (uint32_t i = 0; i < count; ++i)
        all[i] = i < numberOfValues ? values[i] : JSValue::encode(jsUndefined());
    JSObject* object = newObjectWithFields(instance, globalObject, vm, instance->sharedData, instance->sharedData->slots, emptyStructureOfLiteral(instance, globalObject, vm, hasNullPrototype, std::max(count, roomInLine)), nullptr, count, all.data(), row.data() + 1, numberOfValues, number + 1);
    RELEASE_ASSERT(object && !object->butterfly() && count <= object->structure()->inlineCapacity());
    Structure* structure = object->structure();
    Allocator allocator = subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator);
    if (allocator && !structure->isDictionary() && structure->typeInfoBlob() == Instance::typeInfoBlobOfRows && structure->maxOffset() == static_cast<PropertyOffset>(count) - 1) {
        instance->fillRow(number, structure, placesForStubToClear(structure, numberOfValues), allocator.localAllocator());
        Vector<UniquedStringImpl*, 16> names;
        for (uint32_t i = 0; i < count; ++i)
            names.append(nameAt(instance, row[1 + i]).impl());
        tellNamesOfRow(vm, structure, names.span());
    }
    return object;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewObjectLiteral, JSCell*, (Instance* instance, Slot* cache, uint32_t count, uint32_t hasNullPrototypeAndInlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames))
{
    bool hasNullPrototype = hasNullPrototypeAndInlineCapacity & 1;
    AOT_OPERATION_BEGIN_AT(instance, cache);
    if (StructureID structureID = cache[0].structureID) {
        JSObject* object = constructEmptyObject(vm, structureID.decode());
        for (unsigned i = 0; i < count; ++i)
            object->putDirectOffset(vm, i, JSValue::decode(values[i]));
        OPERATION_RETURN(scope, object);
    }
    Structure* empty = emptyStructureOfLiteral(instance, globalObject, vm, hasNullPrototype, std::max(count, hasNullPrototypeAndInlineCapacity >> 1));
    OPERATION_RETURN(scope, newObjectWithFields(instance, globalObject, vm, callerData(instance, callFrame), cache, empty, nullptr, count, values, caller(instance, callFrame).info().function()->plans() + rowOfNames, std::min(count, maxLiteralPropertiesInRegisters)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCreateThisWithFields, JSCell*, (Instance* instance, Slot* cache, JSCell* constructorCell, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    const uint32_t* numbersOfNames = caller(instance, callFrame).info().function()->plans() + rowOfNames;
    if (Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity))
        OPERATION_RETURN(scope, newObjectWithFields(instance, globalObject, vm, callerData(instance, callFrame), cache, empty, newTarget, count, values, numbersOfNames, std::min(count, maxLiteralPropertiesInRegisters)));
    JSValue made = instance->typed->createThis(globalObject, constructor, asObject(newTarget));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    SetForScope onlyCopies(instance->typed->storesMakeOverPlainData, false);
    for (unsigned i = 0; i < count; ++i) {
        createDataProperty(globalObject, asObject(made), nameAt(instance, numbersOfNames[i]), JSValue::decode(values[i]));
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    OPERATION_RETURN(scope, made.asCell());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCreateThisOfCapacity, JSCell*, (Instance* instance, JSCell* constructorCell, JSCell* newTarget, uint32_t inlineCapacity))
{
    AOT_OPERATION_BEGIN(instance);
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    if (Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity))
        OPERATION_RETURN(scope, constructEmptyObject(vm, empty));
    OPERATION_RETURN(scope, instance->typed->createThis(globalObject, constructor, asObject(newTarget)).asCell());
}

// See Lowering::createThis(). Both are closures of the program.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedCreateThisAt, JSCell*, (Instance* instance, Slot* cache, JSCell* constructor, JSCell* newTarget))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    callFrame = frameOfSite(instance, callFrame);
    countMissAt(instance, callFrame, cache);
    Structure* empty;
    if (isSlimClosureType(constructor->type()) || isSlimClosureType(newTarget->type())) [[unlikely]] {
        empty = instance->typed->structureForThisWithSlimClosure(globalObject, asObject(constructor), asObject(newTarget), JSFinalObject::defaultInlineCapacity);
        if (!empty)
            OPERATION_RETURN(scope, Typed::ordinaryCreateFromConstructor(globalObject, asObject(newTarget)));
    } else {
        auto* closure = uncheckedDowncast<Typed::Closure>(constructor);
        empty = instance->typed->structureForThis(globalObject, closure, asObject(newTarget), JSFinalObject::defaultInlineCapacity);
        if (!empty)
            OPERATION_RETURN(scope, instance->typed->createThis(globalObject, closure, asObject(newTarget)).asCell());
    }
    Data* data = callerData(instance, callFrame);
    Allocator allocator = subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(empty->inlineCapacity()), AllocatorForMode::EnsureAllocator);
    if (mayFillForThis(instance, data, cache))
        fillAllocationCache(vm, data, cache, empty, allocator, empty->inlineCapacity(), newTarget);
    if (allocator) {
        Slot* shared = instance->thisAllocationFor(newTarget);
        shared[0].structureID = StructureID();
        shared[0].offset = empty->inlineCapacity() | Slot::pointerIsCell;
        shared[0].pointer = newTarget;
        shared[1].offset = empty->typeInfoBlob();
        shared[1].pointer = allocator.localAllocator();
        shared[2].pointer = std::bit_cast<void*>(static_cast<uintptr_t>(instance->typedPrototypeEpoch));
        shared[3].pointer = constructor;
        shared[0].structureID = empty->id();
        instance->hasThisAllocations = true;
    }
    OPERATION_RETURN(scope, constructEmptyObject(vm, empty));
}

// `this` with what the constructor is seen to store to it or define on it first, all at once: see Lowering::newObjectWithProperties(). A name has bit 32 set where it is stored to. Null: not that way: something on the prototype chain may have a say, or the object has no room for them in it.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedTryCreateThisWithStores, JSCell*, (Instance* instance, Slot* cache, JSCell* constructorCell, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* valuesThenNames))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity);
    if (!empty || count > empty->inlineCapacity())
        OPERATION_RETURN(scope, nullptr);
    Vector<UniquedStringImpl*, 16> names;
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& name = nameAt(instance, static_cast<uint32_t>(valuesThenNames[count + i]));
        if (parseIndex(name))
            OPERATION_RETURN(scope, nullptr);
        names.append(name.impl());
    }
    JSObject* object = constructEmptyObject(vm, empty);
    // Ahead of what remembers the structures of the chain.
    makePrototypeChainWatchable(vm, object);
    Structure* structure = structureBehindRun(instance, vm, empty, names.span());
    if (!structure)
        OPERATION_RETURN(scope, nullptr);
    // The collector goes by the structure, which comes last.
    for (unsigned i = 0; i < count; ++i)
        object->putDirectOffset(vm, i, JSValue::decode(valuesThenNames[i]));
    object->setStructure(vm, structure);
    Data* data = callerData(instance, callFrame);
    if (!mayFillForThis(instance, data, cache))
        OPERATION_RETURN(scope, object);
    Vector<UniquedStringImpl*, 16> stored;
    for (unsigned i = 0; i < count; ++i) {
        if (valuesThenNames[count + i] >> 32)
            stored.append(names[i]);
    }
    if (watchStores(globalObject, data, cache, object, structure, stored.span()))
        fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity(), newTarget);
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCreateThisWithStores, JSCell*, (Instance* instance, Slot* cache, JSCell* constructorCell, JSCell* newTarget, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* values, uint32_t rowOfNames))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    constexpr uint32_t isStoredTo = 1u << 31;
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    const uint32_t* numbersOfNames = caller(instance, callFrame).info().function()->plans() + rowOfNames;
    Data* data = callerData(instance, callFrame);
    Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity);
    bool mayBeAtOnce = empty && count <= empty->inlineCapacity();
    Vector<UniquedStringImpl*, 16> names;
    Vector<UniquedStringImpl*, 16> stored;
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& name = nameAt(instance, numbersOfNames[i] & ~isStoredTo);
        mayBeAtOnce = mayBeAtOnce && !parseIndex(name);
        names.append(name.impl());
        if (numbersOfNames[i] & isStoredTo)
            stored.append(name.impl());
    }
    auto fill = [&](JSObject* object, Structure* structure) {
        if (watchStores(globalObject, data, cache, object, structure, stored.span()))
            fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity(), newTarget);
    };
    if (mayBeAtOnce) {
        JSObject* object = constructEmptyObject(vm, empty);
        makePrototypeChainWatchable(vm, object);
        if (Structure* structure = structureBehindRun(instance, vm, empty, names.span())) {
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, i, JSValue::decode(values[i]));
            object->setStructure(vm, structure);
            if (mayFillForThis(instance, data, cache))
                fill(object, structure);
            OPERATION_RETURN(scope, object);
        }
    }
    JSValue made = empty ? JSValue(constructEmptyObject(vm, empty)) : instance->typed->createThis(globalObject, constructor, asObject(newTarget));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSObject* object = asObject(made);
    {
        CallSiteOverride where(*instance, callFrame->callerFrame());
        for (unsigned i = 0; i < count; ++i) {
            where.setItem(i);
            uint32_t number = numbersOfNames[i] & ~isStoredTo;
            const Identifier& name = nameAt(instance, number);
            if (isPrivateNameOfClass(instance, name, number)) [[unlikely]]
                privateSet(globalObject, vm, object, name, JSValue::decode(values[i]));
            else if (numbersOfNames[i] & isStoredTo) {
                PutPropertySlot slot(object, true);
                JSValue(object).put(globalObject, name, JSValue::decode(values[i]), slot);
            } else
                object->createDataProperty(globalObject, name, JSValue::decode(values[i]), true);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    if (!mayFillForThis(instance, data, cache))
        OPERATION_RETURN(scope, object);
    empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity);
    Structure* structure = object->structure();
    if (!empty || structure->isDictionary() || count > structure->inlineCapacity() || object->butterfly())
        OPERATION_RETURN(scope, object);
    Structure* first = structure;
    for (unsigned i = count; i-- && first;) {
        unsigned attributes = 0;
        if (structure->get(vm, names[i], attributes) != static_cast<PropertyOffset>(i) || attributes)
            OPERATION_RETURN(scope, object);
        first = first->previousID();
    }
    if (first != empty)
        OPERATION_RETURN(scope, object);
    makePrototypeChainWatchable(vm, object);
    fill(object, structure);
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGiveThisItsRow, void, (Instance* instance, Slot* cache, EncodedJSValue self, uint32_t count, uint32_t rowOfNames, uint32_t numberOfRowPlusOne))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    JSValue value = JSValue::decode(self);
    if (!value.isCell() || value.asCell()->type() != FinalObjectType)
        OPERATION_RETURN(scope);
    JSObject* object = asObject(value);
    Structure* empty = object->structure();
    if (empty->previousID() || isValidOffset(empty->maxOffset()) || empty->isDictionary() || empty->hasPolyProto() || !empty->isStructureExtensible() || empty->mayBePrototype() || empty->compiledLayout() || !empty->propertyAccessesAreCacheable() || object->butterfly() || count > empty->inlineCapacity())
        OPERATION_RETURN(scope);
    const uint32_t* numbersOfNames = caller(instance, callFrame).info().function()->plans() + rowOfNames;
    Vector<UniquedStringImpl*, 16> names;
    for (unsigned i = 0; i < count; ++i) {
        const Identifier& name = nameAt(instance, numbersOfNames[i]);
        if (parseIndex(name))
            OPERATION_RETURN(scope);
        names.append(name.impl());
    }
    makePrototypeChainWatchable(vm, object);
    Structure* whole = structureBehindRun(instance, vm, empty, names.span());
    if (!whole || whole->inlineCapacity() != empty->inlineCapacity() || whole->outOfLineCapacity())
        OPERATION_RETURN(scope);
    if (numberOfRowPlusOne && numberOfRowPlusOne <= UINT16_MAX && globalObject == instance->globalObject && !whole->numberOfRowPlusOne())
        whole->setNumberOfRowPlusOne(numberOfRowPlusOne);
    object->setStructure(vm, whole);
    if (cache)
        cacheRowGivenToThis(globalObject, callerData(instance, callFrame), cache, object, empty, whole, names.span());
    OPERATION_RETURN(scope);
}

// The object was made by operationAOTTypedCreateThisOfCapacity, and the names were defined on it or stored to it one by one, in this order. If that did what it does as a rule, the next one is made with all of them: see
// Lowering::newObjectWithProperties().
JSC_DEFINE_JIT_OPERATION(operationAOTTypedDidStoreToThis, void, (Instance* instance, Slot* cache, JSCell* constructorCell, JSCell* newTarget, JSCell* cell, uint32_t count, uint32_t inlineCapacity, const EncodedJSValue* names))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    Data* data = callerData(instance, callFrame);
    if (!mayFillForThis(instance, data, cache))
        OPERATION_RETURN(scope);
    auto* constructor = uncheckedDowncast<Typed::Closure>(constructorCell);
    Structure* empty = instance->typed->structureForThis(globalObject, constructor, asObject(newTarget), inlineCapacity);
    JSObject* object = asObject(cell);
    Structure* structure = object->structure();
    if (!empty || structure->isDictionary() || count > structure->inlineCapacity() || object->butterfly())
        OPERATION_RETURN(scope);
    Structure* first = structure;
    for (unsigned i = count; i-- && first;) {
        unsigned attributes = 0;
        if (structure->get(vm, PropertyName(nameAt(instance, static_cast<uint32_t>(names[i]))), attributes) != static_cast<PropertyOffset>(i) || attributes)
            OPERATION_RETURN(scope);
        first = first->previousID();
    }
    if (first != empty)
        OPERATION_RETURN(scope);
    makePrototypeChainWatchable(vm, object);
    Vector<UniquedStringImpl*, 16> stored;
    for (unsigned i = 0; i < count; ++i) {
        if (names[i] >> 32)
            stored.append(nameAt(instance, static_cast<uint32_t>(names[i])).impl());
    }
    if (!watchStores(globalObject, data, cache, object, structure, stored.span()))
        OPERATION_RETURN(scope);
    fillAllocationCache(vm, data, cache, structure, subspaceFor<JSFinalObject>(vm)->allocatorFor(JSFinalObject::allocationSize(structure->inlineCapacity()), AllocatorForMode::EnsureAllocator), structure->inlineCapacity(), newTarget);
    OPERATION_RETURN(scope);
}

// See Lowering::lowerPropertyRun(). The cache: a slot that is filled as by a store that adds a property, with the place of the first of them, then a slot for each key: see cachePropertyRun(). An empty slot with all attempts used and Slot::isGetter:
// compiled code does not ask, until Data::retryAbandonedSlots() gives it attempts again.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedTryPutProperties, size_t, (Instance* instance, Slot* cache, EncodedJSValue encodedBase, uint32_t count, uint64_t keyed, const EncodedJSValue* valuesThenNames))
{
    AOT_OPERATION_BEGIN_AT(instance, cache);
    auto refuse = [&]() -> size_t {
        if (SharedData::contains(cache))
            return 0;
        if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
            cache->offset += 1u << Slot::attemptsShift;
        else if (!cache->structureID)
            cache->offset |= Slot::isGetter;
        return 0;
    };
    bool hasElementStores = count >> 31;
    bool mayBePrototype = count >> 30 & 1;
    bool mayReplace = count >> 29 & 1;
    count &= INT32_MAX >> 2;
    JSValue base = JSValue::decode(encodedBase);
    if (!base.isCell() || base.asCell()->type() != FinalObjectType)
        OPERATION_RETURN(scope, refuse());
    JSObject* object = asObject(base);
    Structure* structure = object->structure();
    if (hasElementStores && (hasAnyArrayStorage(structure->indexingType()) || structure->holesMustForwardToPrototype(object)))
        OPERATION_RETURN(scope, refuse());
    if (structure->compiledLayout() || structure->isDictionary() || !structure->isStructureExtensible() || structure->hasImmutableShape() || structure->hasPolyProto() || (structure->mayBePrototype() && !mayBePrototype)
        || structure->transitionCountEstimate() > Structure::s_maxTransitionLength)
        OPERATION_RETURN(scope, refuse());
    if (hasElementStores) {
        object->tryMakeWritableContiguous(vm);
        structure = object->structure();
    }
    Vector<UniquedStringImpl*, 16> names;
    Vector<JSCell*, 8> keys;
    for (unsigned i = 0; i < count; ++i) {
        if (!(keyed >> i & 1)) {
            names.append(nameAt(instance, static_cast<uint32_t>(valuesThenNames[count + i])).impl());
            continue;
        }
        JSValue key = JSValue::decode(valuesThenNames[count + i]);
        if (!key.isSymbol())
            OPERATION_RETURN(scope, refuse());
        names.append(&asSymbol(key)->uid());
        keys.append(key.asCell());
    }
    FunctionRef function = caller(instance, callFrame);
    Data* owner = SharedData::contains(cache) ? function.instance->sharedData : function.instance->dataIfExists(function.index);
    if (mayReplace && !structure->mayBePrototype() && structure->propertyAccessesAreCacheable()) {
        Vector<PropertyOffset, 16> offsets;
        for (UniquedStringImpl* name : names) {
            unsigned attributes;
            PropertyOffset offset = structure->get(vm, name, attributes);
            if (!isValidOffset(offset) || attributes || !isInlineOffset(offset))
                break;
            offsets.append(offset);
        }
        if (offsets.size() == count) {
            for (unsigned i = 0; i < count; ++i)
                object->putDirectOffset(vm, offsets[i], JSValue::decode(valuesThenNames[i]));
            if (!cachePropertyRunOfReplacements(vm, owner, cache + 1, structure, offsets.span()))
                refuse();
            OPERATION_RETURN(scope, 1);
        }
    }
    // Ahead of what remembers the structures of the chain.
    makePrototypeChainWatchable(vm, object);
    Structure* last = structureBehindRun(instance, vm, structure, names.span());
    if (!last)
        OPERATION_RETURN(scope, refuse());
    PropertyOffset offset = structure->maxOffset();
    // As JSObject::putDirectInternal() does it: the collector goes by the Structure, which comes last.
    bool grows = structure->outOfLineCapacity() != last->outOfLineCapacity();
    // What Stub::PutPropertiesWithStorage can do again. All by name: such a run has a second slot. A butterfly with elements is not copied there.
    bool mayBeRememberedWithStorage = !keyed && (!grows || !hasIndexedProperties(structure->indexingType()));
    if (grows)
        object->nukeStructureAndSetButterfly(vm, structure->id(), object->allocateMoreOutOfLineStorage(vm, structure->outOfLineCapacity(), last->outOfLineCapacity()));
    for (unsigned i = 0; i < count; ++i) {
        offset = Instance::offsetAfter(offset, structure->inlineCapacity());
        object->putDirectOffset(vm, offset, JSValue::decode(valuesThenNames[i]));
    }
    object->setStructure(vm, last);
    if (structure->mayBePrototype()) {
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
        OPERATION_RETURN(scope, 1);
    }
    if (!grows && isInlineOffset(offset))
        cachePropertyRun(globalObject, owner, cache, object, structure, names.span(), keys.span());
    else if (mayBeRememberedWithStorage && cachePropertyRunWithStorage(globalObject, owner, cache + 1, object, structure, names.span(), grows)) {
    } else if (!SharedData::contains(cache)) {
        // Nothing to remember: compiled code would come here every time, where each store by itself has a cache that serves. So this counts like a refusal.
        if ((cache->offset & Slot::attemptsMask) != Slot::attemptsMask)
            cache->offset += 1u << Slot::attemptsShift;
        else {
            if (cache->structureID) {
                cache->clear();
                stopWatching(owner, cache);
            }
            cache->offset |= Slot::isGetter;
        }
    }
    OPERATION_RETURN(scope, 1);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArray, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    // With the room it is given in place, generateArrayLiteral(), so that the free list that has run out there is the one that is filled.
    unsigned capacityOfEmpty = count >> capacityShiftInArrayLiteralCount;
    count &= (1u << capacityShiftInArrayLiteralCount) - 1;
    unsigned vectorLength = capacityOfEmpty ? capacityOfEmpty : vectorLengthOfArrayLiteral(count);
    if (hasContiguous(structure->indexingType()) && !structure->outOfLineCapacity() && count <= maxArrayElementsInRegisters) {
        if (void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull)) {
            Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
            butterfly->setVectorLength(vectorLength);
            butterfly->setPublicLength(count);
            for (uint32_t i = 0; i < vectorLength; ++i)
                butterfly->contiguous().atUnsafe(i).setWithoutWriteBarrier(i < count ? JSValue::decode(values[i]) : JSValue());
            OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, structure, butterfly));
        }
    }
    JSArray* array = JSArray::tryCreate(vm, structure, count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    for (uint32_t i = 0; i < count; ++i)
        array->putDirectIndex(globalObject, i, JSValue::decode(values[i]));
    OPERATION_RETURN(scope, array);
}

// What generateArrayOfNumbers() makes in place, of the same sizes, so that the free list that has run out there is the one that is filled. Nothing but holes: the caller stores the elements.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayOfNumbers, JSCell*, (Instance* instance, uint32_t lengthAndCapacity, uint32_t indexingType))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    unsigned length = lengthAndCapacity & ((1u << capacityShiftInArrayOfNumbers) - 1);
    unsigned capacity = lengthAndCapacity >> capacityShiftInArrayOfNumbers;
    RELEASE_ASSERT(length <= capacity && !globalObject->isHavingABadTime() && (indexingType == ArrayWithDouble || indexingType == ArrayWithInt32 || indexingType == ArrayWithContiguous));
    void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, capacity * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!storage) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
    butterfly->setVectorLength(capacity);
    butterfly->setPublicLength(length);
    for (unsigned i = 0; i < capacity; ++i) {
        if (indexingType == ArrayWithDouble)
            butterfly->contiguousDouble().atUnsafe(i) = PNaN;
        else
            butterfly->contiguous().atUnsafe(i).clear();
    }
    OPERATION_RETURN(scope, JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(indexingType), butterfly));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPush, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue value, uint64_t expectedAndSite))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    uint32_t expected = expectedAndSite >> 32;
    if (uint32_t site = static_cast<uint32_t>(expectedAndSite) >> 1)
        operationAOTTypedUpdateVectorLengthHint(instance, cell, site - 1);
    if (array->type() != ArrayType) [[unlikely]] {
        bool hasReadMethod = expectedAndSite & 1;
        JSValue callee = (hasReadMethod ? globalObject->arrayPrototype() : array)->get(globalObject, vm.propertyNames->builtinNames().pushPublicName());
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(callValue(globalObject, vm, callee, array, 1, &value)));
    }
    if (unsigned newLength = pushToFullVector(vm, array, JSValue::decode(value), expected))
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(newLength)));
    array->push(globalObject, JSValue::decode(value));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(array->length())));
}

static unsigned cellOfArraySlice()
{
    auto& builtins = Typed::Builtins::singleton();
    static const unsigned function = [&] {
        for (unsigned i = 0; i < builtins.size(); ++i) {
            auto& implementation = builtins.at(Typed::BuiltinID(i)).implementation;
            if (implementation.kind == Typed::BuiltinImplementation::Inline && implementation.inlined == Typed::InlineBuiltin::ArraySlice)
                return builtins.cellOf(Typed::BuiltinID(i));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }();
    return function;
}

static JSArray* sliceAsTheStubDoes(VM& vm, JSGlobalObject* globalObject, JSCell* cell, int64_t from, int64_t to)
{
    if (cell->type() != ArrayType || globalObject->isHavingABadTime())
        return nullptr;
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    IndexingType shape = array->indexingType() & IndexingShapeMask;
    if (shape != Int32Shape && shape != DoubleShape && shape != ContiguousShape)
        return nullptr;
    int64_t length = array->butterfly()->publicLength();
    from = from < 0 ? std::max<int64_t>(from + length, 0) : std::min(from, length);
    to = to < 0 ? std::max<int64_t>(to + length, 0) : std::min(to, length);
    int64_t count = std::max<int64_t>(to - from, 0);
    if (count > 64)
        return nullptr;
    unsigned vectorLength = static_cast<unsigned>(count) | 1;
    void* storage = vm.auxiliarySpace().allocate(vm, Butterfly::totalSize(0, 0, true, vectorLength * sizeof(EncodedJSValue)), nullptr, AllocationFailureMode::ReturnNull);
    if (!storage)
        return nullptr;
    Butterfly* butterfly = Butterfly::fromBase(storage, 0, 0);
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(static_cast<unsigned>(count));
    const uint64_t* source = array->butterfly()->indexingPayload<uint64_t>() + from;
    uint64_t* target = butterfly->indexingPayload<uint64_t>();
    for (unsigned i = 0; i < vectorLength; ++i)
        target[i] = i < count ? source[i] : shape == DoubleShape ? std::bit_cast<uint64_t>(PNaN) : JSValue::encode(JSValue());
    JSArray* result = JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(static_cast<IndexingType>(IsArray | shape)), butterfly);
    ensureStillAliveHere(array);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayCopy, EncodedJSValue, (Instance* instance, JSCell* array))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (JSArray* copy = sliceAsTheStubDoes(vm, globalObject, array, 0, static_cast<int64_t>(1) << 32))
        OPERATION_RETURN(scope, JSValue::encode(copy));
    MarkedArgumentBuffer arguments;
    OPERATION_RETURN(scope, JSValue::encode(callBuiltinInEngine(instance, cellOfArraySlice(), array, arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySlice, EncodedJSValue, (Instance* instance, JSCell* array, int64_t from, int64_t to))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (JSArray* part = sliceAsTheStubDoes(vm, globalObject, array, from, to))
        OPERATION_RETURN(scope, JSValue::encode(part));
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(static_cast<double>(from)));
    arguments.append(jsNumber(static_cast<double>(to)));
    OPERATION_RETURN(scope, JSValue::encode(callBuiltinInEngine(instance, cellOfArraySlice(), array, arguments)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArraySearchByCall, EncodedJSValue, (Instance* instance, JSCell* array, EncodedJSValue searched, EncodedJSValue methodRead, uint32_t isIndexOf))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Identifier name = isIndexOf ? Identifier::fromString(vm, "indexOf"_s) : vm.propertyNames->includes;
    JSValue method = JSValue::decode(methodRead);
    bool hasReadMethod = !!method;
    if (!hasReadMethod) {
        method = JSValue(array).get(globalObject, name);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    auto callData = JSC::getCallData(method);
    if (callData.type == CallData::Type::None) {
        if (hasReadMethod || method.isCell())
            throwTypeError(globalObject, scope, makeString(errorDescriptionForValue(globalObject, method), " is not a function"_s));
        else
            throwNotAFunction(globalObject, scope, name, method);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    MarkedArgumentBuffer arguments;
    arguments.append(JSValue::decode(searched));
    EncodedJSValue found = JSValue::encode(call(globalObject, method, callData, array, arguments));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!isIndexOf || static_cast<uint64_t>(found) >> 32)
        OPERATION_RETURN(scope, found);
    vm.encodedHostCallReturnValue = found;
    OPERATION_RETURN(scope, static_cast<EncodedJSValue>(Typed::arraySearchIsHandedBack));
}

static bool reportOrThrowWouldRunCode(JSGlobalObject* globalObject, State& state)
{
    return state.reportOrThrow(globalObject, { .kind = "array-not-plain"_s }, "Cannot read array element: the array or its prototype chain has accessors or a Proxy, which are not run for an array of a declared element type"_s, State::InAudit::Ignored);
}

// [[Get]] as far as that runs no code of the program. Nothing: it would, and that has been thrown.
static std::optional<JSValue> getWithoutRunningCode(JSGlobalObject* globalObject, State& state, JSArray* array, PropertyName name, std::optional<uint32_t> index)
{
    if (index && array->canGetIndexQuickly(*index))
        return array->getIndexQuickly(*index);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(array, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = index ? array->getPropertySlot(globalObject, *index, slot) : array->getPropertySlot(globalObject, name, slot);
    scope.assertNoException();
    if (slot.isTaintedByOpaqueObject() || (isFound && !slot.isValue())) {
        if (!reportOrThrowWouldRunCode(globalObject, state))
            return std::nullopt;
        return jsUndefined();
    }
    if (!isFound)
        return jsUndefined();
    return slot.getValue(globalObject, name);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayGet, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    instance->typed->countArrayGet(array, JSValue::decode(key));
    std::optional<uint32_t> index = JSValue::decode(key).tryGetAsUint32Index();
    Identifier name;
    if (!index)
        name = Identifier::from(vm, JSValue::decode(key).asNumber());
    auto value = getWithoutRunningCode(globalObject, *instance->typed, array, name, index);
    if (value && value->isDouble())
        value = jsNumber(value->asDouble());
    OPERATION_RETURN(scope, value ? JSValue::encode(*value) : encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayElementAs, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t key, uint32_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    JSValue boxedKey = jsNumber(static_cast<double>(static_cast<int64_t>(key)));
    state.countArrayGet(array, boxedKey);
    std::optional<uint32_t> index = boxedKey.tryGetAsUint32Index();
    Identifier name;
    if (!index)
        name = Identifier::from(vm, boxedKey.asNumber());
    auto value = getWithoutRunningCode(globalObject, state, array, name, index);
    if (!value)
        OPERATION_RETURN(scope, encodedJSValue());
    JSValue element = value->isDouble() ? jsNumber(value->asDouble()) : *value;
    JSValue converted = state.hold(globalObject, element, state.type(type));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (converted != element && index && array->canGetIndexQuickly(*index)) {
        array->putDirectIndex(globalObject, *index, converted);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(converted));
}

static bool mayGrowVector(unsigned vectorLength, uint32_t index)
{
    return index >= vectorLength && (index < MIN_BEYOND_LENGTH_SPARSE_INDEX || index / 2 <= vectorLength);
}

static SparseArrayValueMap* sparseMapOf(JSArray* array)
{
    return array->indexingType() == ArrayWithArrayStorage ? array->butterfly()->arrayStorage()->m_sparseMap.get() : nullptr;
}

static void convertArrayStorageToContiguous(VM& vm, JSArray* array)
{
    // ArrayStorage, Contiguous and ArrayStorage again between two reads of the Structure would look to a marker like no change at all.
    if (vm.heap.mutatorShouldBeFenced())
        return;
    Structure* structure = array->structure();
    JSGlobalObject* globalObject = structure->globalObject();
    if (globalObject->isHavingABadTime() || structure != globalObject->originalArrayStructureForIndexingType(ArrayWithArrayStorage))
        return;
    ArrayStorage* storage = array->butterfly()->arrayStorage();
    SparseArrayValueMap* map = storage->m_sparseMap.get();
    unsigned length = storage->length();
    if (length > MAX_STORAGE_VECTOR_LENGTH || (map && (map->sparseMode() || map->lengthIsReadOnly())))
        return;
    if (!isDenseEnoughForVector(length, storage->m_numValuesInVector + (map ? map->size() : 0)))
        return;
    DeferGC deferGC(vm);
    unsigned vectorLength = Butterfly::optimalContiguousVectorLength(static_cast<size_t>(0), length);
    Butterfly* butterfly = Butterfly::tryCreateUninitialized(vm, array, 0, 0, true, vectorLength * sizeof(EncodedJSValue));
    if (!butterfly)
        return;
    butterfly->setVectorLength(vectorLength);
    butterfly->setPublicLength(length);
    WriteBarrier<Unknown>* elements = butterfly->contiguous().data();
    unsigned numberInVector = std::min(storage->vectorLength(), length);
    for (unsigned i = 0; i < numberInVector; ++i)
        elements[i].setWithoutWriteBarrier(storage->m_vector[i].get());
    for (unsigned i = numberInVector; i < vectorLength; ++i)
        elements[i].clear();
    if (map) {
        for (auto& entry : *map)
            elements[entry.index()].setWithoutWriteBarrier(entry.getNonSparseMode());
    }
    {
        Locker locker { array->cellLock() };
        array->nukeStructureAndSetButterfly(vm, structure->id(), butterfly);
        array->setStructure(vm, globalObject->originalArrayStructureForIndexingType(ArrayWithContiguous));
    }
    vm.writeBarrier(array);
}

static ALWAYS_INLINE void putIndexOfArrayInline(JSGlobalObject* globalObject, VM& vm, JSArray* array, uint32_t index, JSValue value, bool isStrict)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned vectorLength = array->indexingType() & IndexingShapeMask ? array->butterfly()->vectorLength() : 0;
    if (array->type() == ArrayType && mayGrowVector(vectorLength, index) && array->tryPutIndexInPlace(vm, index, value))
        return;
    bool hadSparseMap = sparseMapOf(array);
    array->methodTable()->putByIndex(array, globalObject, index, value, isStrict);
    if (!scope.exception() && array->type() == ArrayType && array->indexingType() == ArrayWithArrayStorage && (hadSparseMap || sparseMapOf(array)))
        convertArrayStorageToContiguous(vm, array);
}

void putIndexOfArray(JSGlobalObject* globalObject, JSArray* array, uint32_t index, JSValue value, bool isStrict)
{
    putIndexOfArrayInline(globalObject, globalObject->vm(), array, index, value, isStrict);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPut, void, (Instance* instance, JSCell* cell, EncodedJSValue key, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    IndexingType shapeBefore = array->indexingType() & IndexingShapeMask;
    unsigned vectorLengthBefore = shapeBefore ? array->butterfly()->vectorLength() : 0;
    auto count = makeScopeExit([&] {
        instance->typed->countArrayPut(array, JSValue::decode(key), shapeBefore, vectorLengthBefore);
    });
    if (std::optional<uint32_t> index = JSValue::decode(key).tryGetAsUint32Index()) {
        // The engine gives up on keeping the elements in place at a store from index 1,000 on that is beyond the end of the vector, and
        // for good. Compiled code reads them in place. A table by a number that several tables share is stored to like that. So the
        // vector grows instead where it at most doubles, as it may by being appended to. Farther out the elements go to a map, until one
        // slot in eight is filled.
        scope.release();
        putIndexOfArrayInline(globalObject, vm, array, *index, JSValue::decode(value), true);
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(array, true);
    array->methodTable()->put(array, globalObject, Identifier::from(vm, JSValue::decode(key).asNumber()), JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStoreElement, void, (Instance* instance, JSCell* cell, int64_t index, EncodedJSValue value))
{
    return operationAOTTypedArrayPut(instance, cell, JSValue::encode(jsNumber(static_cast<double>(index))), value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayGetAtNumber, EncodedJSValue, (Instance* instance, JSCell* cell, double key))
{
    return operationAOTTypedArrayGet(instance, cell, JSValue::encode(jsNumber(purifyNaN(key))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayGetAtIndex, EncodedJSValue, (Instance* instance, JSCell* cell, uint64_t key))
{
    return operationAOTTypedArrayGet(instance, cell, JSValue::encode(jsNumber(static_cast<double>(static_cast<int64_t>(key)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPutAtIndex, void, (Instance* instance, JSCell* cell, EncodedJSValue value, uint64_t key))
{
    return operationAOTTypedArrayPut(instance, cell, JSValue::encode(jsNumber(static_cast<double>(static_cast<int64_t>(key)))), value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPutAtNumber, void, (Instance* instance, JSCell* cell, EncodedJSValue value, double key))
{
    return operationAOTTypedArrayPut(instance, cell, JSValue::encode(jsNumber(purifyNaN(key))), value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTryAddNamedProperties, size_t, (Instance* instance, JSCell* cell, uint32_t count, const EncodedJSValue* valuesThenNames))
{
    VM& vm = *instance->vm;
    countOperationNamed(instance, __func__);
    JSObject* array = asObject(cell);
    Structure* from = array->structure();
    auto* transition = instance->typed->namedPropertiesTransition(vm, from, { valuesThenNames + count, count });
    if (!transition || !transition->to)
        return 0;
    // As JSObject::putDirectInternal() does it: the collector goes by the Structure, which comes last.
    if (from->outOfLineCapacity() != transition->to->outOfLineCapacity())
        array->nukeStructureAndSetButterfly(vm, from->id(), array->allocateMoreOutOfLineStorage(vm, from->outOfLineCapacity(), transition->to->outOfLineCapacity()));
    for (uint32_t i = 0; i < count; ++i)
        array->putDirectOffset(vm, transition->offsets[i], JSValue::decode(valuesThenNames[i]));
    array->setStructure(vm, transition->to);
    return 1;
}

static bool isValuesOfAnotherRealmWhoseIterationIsUntouched(JSValue found)
{
    auto* function = dynamicDowncast<JSFunction>(found);
    if (!function)
        return false;
    JSGlobalObject* realm = function->realm();
    return function == realm->arrayProtoValuesFunction() && realm->arrayIteratorProtocolWatchpointSet().isStillValid();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedOpenChangedArray, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto found = getWithoutRunningCode(globalObject, *instance->typed, uncheckedDowncast<JSArray>(cell), vm.propertyNames->iteratorSymbol, std::nullopt);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (*found != JSValue(globalObject->linkTimeConstant(LinkTimeConstant::arrayProtoValues)) && !isValuesOfAnotherRealmWhoseIterationIsUntouched(*found))
        reportOrThrowWouldRunCode(globalObject, *instance->typed);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTypedArrayIteratorOpen, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
    if (!view->isIteratorProtocolFastAndNonObservable()) {
        JSObject* common = asObject(globalObject->typedArrayPrototype(typedArrayType(view->type()))->getPrototypeDirect());
        PropertySlot slot(view, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool isFound = view->getPropertySlot(globalObject, vm.propertyNames->iteratorSymbol, slot);
        scope.assertNoException();
        if (slot.isTaintedByOpaqueObject() || !isFound || !slot.isValue() || slot.getValue(globalObject, vm.propertyNames->iteratorSymbol) != common->getDirect(vm, vm.propertyNames->iteratorSymbol)) {
            reportOrThrowWouldRunCode(globalObject, *instance->typed);
            OPERATION_RETURN(scope);
        }
    }
    validateTypedArray(globalObject, view);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTypedArrayIteratorHasNext, size_t, (Instance* instance, JSCell* cell, uint64_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* view = uncheckedDowncast<JSArrayBufferView>(cell);
    validateTypedArray(globalObject, view);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, index < view->length());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHoldToType, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint64_t type))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue given = JSValue::decode(value);
    if (!given || !instance->typed->fits(instance->typed->type(static_cast<uint32_t>(type)), given)) [[unlikely]]
        throwTypeMismatch(instance, globalObject, scope, given ? given : jsUndefined(), static_cast<uint32_t>(type));
    OPERATION_RETURN(scope, value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedBuiltinCell, JSCell*, (Instance* instance, uint32_t index))
{
    JSCell* cell = Typed::Builtins::singleton().findCell(instance->globalObject, index);
    instance->typedBuiltins[index] = cell;
    return cell;
}

JSCell* typedBuiltinCell(Instance* instance, unsigned index)
{
    if (JSCell* cell = instance->typedBuiltins[index]) [[likely]]
        return cell;
    return operationAOTTypedBuiltinCell(instance, index);
}

JSValue callBuiltinInEngine(Instance* instance, unsigned function, JSValue thisValue, const ArgList& arguments)
{
    JSCell* cell = typedBuiltinCell(instance, function);
    return call(instance->globalObject, cell, JSC::getCallData(cell), thisValue, arguments);
}

template<typename View>
static JSCell* newView(Instance* instance, JSGlobalObject* globalObject, size_t length)
{
    static_assert(View::TypedArrayStorageType < Instance::numberOfTypedArrayAllocations && sizeof(View) == sizeof(JSArrayBufferView));
    Structure* structure = globalObject->typedArrayStructure(View::TypedArrayStorageType, false);
    if (auto& row = instance->typedArrayAllocations[View::TypedArrayStorageType]; !row.allocator) {
        VM& vm = *instance->vm;
        row.header = static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
        row.elementSize = View::elementSize;
        instance->primitiveAuxiliarySpace = &vm.primitiveGigacageAuxiliarySpace();
        row.allocator = subspaceFor<View>(vm)->allocatorFor(sizeof(View), AllocatorForMode::EnsureAllocator).localAllocator();
    }
    return View::create(globalObject, structure, length);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewTypedArray, JSCell*, (Instance* instance, uint32_t type, double number))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    // ToIndex.
    double index = std::isnan(number) ? 0 : std::trunc(number);
    if (index < 0 || index > MAX_ARRAY_BUFFER_SIZE) {
        throwRangeError(globalObject, scope, "Length out of range of buffer"_s);
        OPERATION_RETURN(scope, nullptr);
    }
    size_t length = static_cast<size_t>(index);
    switch (static_cast<TypedArrayType>(type)) {
#define AOT_TYPED_NEW_VIEW(name) \
    case Type ## name: \
        OPERATION_RETURN(scope, newView<JS ## name ## Array>(instance, globalObject, length));
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_TYPED_NEW_VIEW)
#undef AOT_TYPED_NEW_VIEW
    case NotTypedArray:
    case TypeDataView:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedTypedArrayPut, void, (Instance* instance, JSCell* view, uint64_t index, double number))
{
    DeferTerminationForAWhile deferTermination(*instance->vm);
    JSValue value(JSValue::EncodeAsDouble, purifyNaN(number));
    switch (view->type()) {
#define AOT_TYPED_PUT_ELEMENT(name) \
    case name ## ArrayType: \
        uncheckedDowncast<JS ## name ## Array>(view)->setIndex(instance->globalObject, index, value); \
        return;
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(AOT_TYPED_PUT_ELEMENT)
#undef AOT_TYPED_PUT_ELEMENT
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedClosureConstant, JSCell*, (Instance* instance, uint32_t function))
{
    countOperationNamed(instance, __func__);
    if (instance->typed->closuresHaveCaptures(function))
        return instance->typed->lazyClosureCell(*instance->vm, function);
    return instance->typed->closureObjectFor(*instance->vm, function);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCreateThis, JSCell*, (Instance* instance, JSCell* constructor, JSCell* newTarget))
{
    DeferTerminationForAWhile deferTermination(*instance->vm);
    // As the run time makes it where it constructs, so that the objects of one function have one structure.
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*instance->vm);
    JSValue made = isSlimClosureType(constructor->type()) || isSlimClosureType(newTarget->type()) ? JSValue(Typed::ordinaryCreateFromConstructor(instance->globalObject, asObject(newTarget))) : instance->typed->createThis(instance->globalObject, uncheckedDowncast<Typed::Closure>(constructor), asObject(newTarget));
    scope.assertNoException();
    return made.asCell();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConcat, JSCell*, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, jsString(globalObject, uncheckedDowncast<JSString>(left), uncheckedDowncast<JSString>(right)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConcatThree, JSCell*, (Instance* instance, JSCell* first, JSCell* second, JSCell* third))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, jsString(globalObject, uncheckedDowncast<JSString>(first), uncheckedDowncast<JSString>(second), uncheckedDowncast<JSString>(third)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCodeUnitOfRope, size_t, (Instance* instance, JSCell* string, int64_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t outside = static_cast<size_t>(-1);
    auto value = uncheckedDowncast<JSString>(string)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, outside);
    if (static_cast<uint64_t>(index) >= value->length())
        OPERATION_RETURN(scope, outside);
    OPERATION_RETURN(scope, static_cast<size_t>(value.data[static_cast<unsigned>(index)]));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSubstring, JSCell*, (Instance* instance, JSCell* string, int32_t from, int32_t to))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSString* part = jsSubstring(globalObject, uncheckedDowncast<JSString>(string), from, to - from);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (to - from == 2 && !part->isRope())
        jsString(globalObject, part, part);
    OPERATION_RETURN(scope, part);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedFlatten, void*, (Instance* instance, JSCell* string))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto value = uncheckedDowncast<JSString>(string)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, value->impl());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringEq, size_t, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, uncheckedDowncast<JSString>(left)->equal(globalObject, uncheckedDowncast<JSString>(right)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringHasAt, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t place))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    unsigned length = uncheckedDowncast<JSString>(string)->length();
    unsigned otherLength = uncheckedDowncast<JSString>(other)->length();
    if (place < 0 || otherLength > length || static_cast<unsigned>(place) > length - otherLength)
        OPERATION_RETURN(scope, false);
    auto characters = uncheckedDowncast<JSString>(string)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    auto otherCharacters = uncheckedDowncast<JSString>(other)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->substring(place, otherLength) == otherCharacters.data);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringIndexOf, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t place))
{
    // As operationStringIndexOfWithIndex(). The place is within the string.
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* base = uncheckedDowncast<JSString>(string);
    JSString* argument = uncheckedDowncast<JSString>(other);
    unsigned position = place;
    if (base->length() < static_cast<uint64_t>(argument->length()) + position)
        OPERATION_RETURN(scope, none);
    auto otherCharacters = argument->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (argument->length() == 1 && base->isRope() && base->length() >= JSString::minLengthForRopeWalk) {
        if (auto found = base->tryFindOneChar(globalObject, otherCharacters[0], position))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    size_t found = characters->find(vm.adaptiveStringSearcherTables(), otherCharacters, position);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIntToString, JSCell*, (Instance* instance, uint64_t bits))
{
    VM& vm = *instance->vm;
    int64_t value = static_cast<int64_t>(bits);
    // Hashed while it is at hand: as the key of a Map it is then looked up in place.
    if (value == static_cast<int32_t>(value)) {
        JSString* string = int32ToString(vm, static_cast<int32_t>(value), 10);
        string->tryGetValueImpl()->hash();
        return string;
    }
    return jsString(vm, String::number(static_cast<double>(value)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDoubleToString, JSCell*, (Instance* instance, double value))
{
    VM& vm = *instance->vm;
    // The cache is for strings of more than one character.
    if (!value)
        return vm.smallStrings.singleCharacterString('0');
    return vm.numericStrings.addJSString(vm, value);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCharToString, JSCell*, (Instance* instance, uint32_t character))
{
    return jsSingleCharacterString(*instance->vm, static_cast<char16_t>(character));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewMap, JSCell*, (Instance* instance))
{
    return JSMap::create(*instance->vm, instance->globalObject->mapStructure());
}

static ALWAYS_INLINE bool standsForItsOwnIterator(JSValue iterable)
{
    // (Not an object of a class of the host that extends JSArray: it may keep its length anywhere.)
    auto* array = dynamicDowncast<JSArray>(iterable);
    return array && array->classInfo() == JSArray::info() && array->isIteratorProtocolFastAndNonObservable();
}

template<typename Functor>
static ALWAYS_INLINE void openForeignIteration(JSGlobalObject* globalObject, JSValue iterable, const Functor& makeRecord)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // GetIterator, refusing in the words of the interpreter: see iterator_open in CommonSlowPaths.cpp.
    JSValue method = iterable.get(globalObject, vm.propertyNames->iteratorSymbol);
    RETURN_IF_EXCEPTION(scope, void());
    if (auto validation = validateIterable(vm, iterable, method); validation != IterableValidationResult::Valid) [[unlikely]] {
        throwTypeError(globalObject, scope, getIteratorErrorMessage(validation, iterable));
        return;
    }
    JSValue iterator = call(globalObject, method, JSC::getCallData(method), iterable, ArgList());
    RETURN_IF_EXCEPTION(scope, void());
    if (!iterator.isObject()) [[unlikely]] {
        throwTypeError(globalObject, scope, "Iterator result interface is not an object."_s);
        return;
    }
    JSValue next = asObject(iterator)->get(globalObject, vm.propertyNames->next);
    RETURN_IF_EXCEPTION(scope, void());
    auto* state = makeRecord();
    state->setIndex(vm, 0, iterator);
    // An int32 in this place marks a record of CollectionIteration. It cannot be called, no more than `undefined`.
    state->setIndex(vm, 1, next.isInt32() ? jsUndefined() : next);
    state->setIndex(vm, 2, jsUndefined());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterOpen, EncodedJSValue, (Instance* instance, EncodedJSValue iterable))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    // An array whose iteration nothing can observe stands for its own iterator: compiled code steps through it by index. Anything else
    // gets a record, which is no array.
    if (standsForItsOwnIterator(JSValue::decode(iterable)))
        OPERATION_RETURN(scope, iterable);
    if (JSCellButterfly* state = Typed::CollectionIteration::tryOpen(globalObject, JSValue::decode(iterable)))
        OPERATION_RETURN(scope, JSValue::encode(state));
    JSCellButterfly* state = nullptr;
    openForeignIteration(globalObject, JSValue::decode(iterable), [&] {
        return state = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 3);
    });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(state));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterOpenInFrame, EncodedJSValue, (Instance* instance, EncodedJSValue iterable, EncodedJSValue* words))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    if (standsForItsOwnIterator(JSValue::decode(iterable)))
        OPERATION_RETURN(scope, iterable);
    auto* state = Typed::CollectionIteration::InFrame::at(words);
    if (Typed::CollectionIteration::tryOpen(globalObject, JSValue::decode(iterable), state))
        OPERATION_RETURN(scope, JSValue::encode(globalObject));
    openForeignIteration(globalObject, JSValue::decode(iterable), [&] {
        return state;
    });
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(globalObject));
}

template<typename Record>
static ALWAYS_INLINE bool stepIteration(JSGlobalObject* globalObject, Record* state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (Typed::CollectionIteration::isOne(state))
        RELEASE_AND_RETURN(scope, Typed::CollectionIteration::step(globalObject, state));
    IterationRecord record { state->get(0), state->get(1) };
    JSValue result = iteratorNextExported(globalObject, record);
    RETURN_IF_EXCEPTION(scope, false);
    // As the engine makes them: `value` and `done` are its own data properties, in these places.
    if (JSObject* object = asObject(result); object->structure() == globalObject->iteratorResultObjectStructure()) {
        if (object->getDirect(iteratorResultObjectDonePropertyOffset).toBoolean(globalObject))
            return false;
        state->setIndex(vm, 2, object->getDirect(iteratorResultObjectValuePropertyOffset));
        return true;
    }
    bool isDone = iteratorCompleteExported(globalObject, result);
    RETURN_IF_EXCEPTION(scope, false);
    if (isDone)
        return false;
    JSValue value = iteratorValue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, false);
    state->setIndex(vm, 2, value);
    return true;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterHas, size_t, (Instance* instance, EncodedJSValue encodedState))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, stepIteration(globalObject, uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterHasInFrame, size_t, (Instance* instance, EncodedJSValue* words))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, stepIteration(globalObject, Typed::CollectionIteration::InFrame::at(words)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIterGet, EncodedJSValue, (Instance*, EncodedJSValue encodedState))
{
    return JSValue::encode(uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell())->get(2));
}

template<typename Record>
static ALWAYS_INLINE void closeIteration(JSGlobalObject* globalObject, Record* state)
{
    if (Typed::CollectionIteration::isOne(state))
        Typed::CollectionIteration::close(globalObject, state);
    else
        iteratorClose(globalObject, state->get(0));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterClose, void, (Instance* instance, EncodedJSValue encodedState))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    closeIteration(globalObject, uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell()));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterCloseInFrame, void, (Instance* instance, EncodedJSValue* words))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    closeIteration(globalObject, Typed::CollectionIteration::InFrame::at(words));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedTakeElements, void, (Instance* instance, EncodedJSValue encodedIterable, uint32_t count, EncodedJSValue* taken))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue iterable = JSValue::decode(encodedIterable);
    for (uint32_t i = 0; i < count; ++i)
        taken[i] = JSValue::encode(jsUndefined());
    if (standsForItsOwnIterator(iterable)) {
        JSArray* array = asArray(iterable);
        for (uint32_t i = 0; i < count && i < array->length(); ++i) {
            JSValue element = array->getIndex(globalObject, i);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            taken[i] = JSValue::encode(element);
        }
        OPERATION_RETURN(scope);
    }
    Typed::CollectionIteration::InFrame record;
    if (!Typed::CollectionIteration::tryOpen(globalObject, iterable, &record)) {
        openForeignIteration(globalObject, iterable, [&] {
            return &record;
        });
        OPERATION_RETURN_IF_EXCEPTION(scope);
    }
    for (uint32_t i = 0; i < count; ++i) {
        bool has = stepIteration(globalObject, &record);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        if (!has)
            OPERATION_RETURN(scope);
        taken[i] = JSValue::encode(record.get(2));
    }
    closeIteration(globalObject, &record);
    OPERATION_RETURN(scope);
}

// IteratorClose with a throw completion: that completion wins over whatever `return` does.
template<typename Record>
static ALWAYS_INLINE void closeIterationForThrow(JSGlobalObject* globalObject, Record* state)
{
    VM& vm = globalObject->vm();
    if (Typed::CollectionIteration::isOne(state)) {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        Typed::CollectionIteration::close(globalObject, state);
        catchScope.clearExceptionExceptTermination();
    } else {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        JSValue iterator = state->get(0);
        JSValue method = iterator.get(globalObject, vm.propertyNames->returnKeyword);
        if (!catchScope.exception() && !method.isUndefinedOrNull()) {
            if (auto callData = JSC::getCallData(method); callData.type != CallData::Type::None)
                call(globalObject, method, callData, iterator, ArgList());
        }
        catchScope.clearExceptionExceptTermination();
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterCloseForThrow, void, (Instance* instance, EncodedJSValue encodedState))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    closeIterationForThrow(globalObject, uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell()));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterCloseForThrowInFrame, void, (Instance* instance, EncodedJSValue* words))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    closeIterationForThrow(globalObject, Typed::CollectionIteration::InFrame::at(words));
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
