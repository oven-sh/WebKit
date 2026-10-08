/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedOperations.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedBuiltins.h"
#include "AOTTypedObject.h"
#include "AOTTypedType.h"
#include "AbstractModuleRecord.h"
#include "BuiltinNames.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "JSModuleRecord.h"
#include "ClonedArguments.h"
#include "IteratorOperations.h"
#include "JSAsyncFromSyncIterator.h"
#include "GetterSetter.h"
#include "JSBoundFunction.h"
#include "JSONObject.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSMapInlines.h"
#include "JSPromise.h"
#include "Microtask.h"
#include "JSTypedArrays.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "ObjectConstructor.h"
#include "ObjectPrototype.h"
#include "Operations.h"
#include "TopExceptionScope.h"
#include "TypeofType.h"
#include "VMTrapsInlines.h"

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

static JSValue getFree(JSGlobalObject* globalObject, VM& vm, const Identifier& name, bool throwsIfMissing)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSGlobalLexicalEnvironment* lexical = globalObject->globalLexicalEnvironment();
    PropertySlot lexicalSlot(lexical, PropertySlot::InternalMethodType::Get);
    if (JSGlobalLexicalEnvironment::getOwnPropertySlot(lexical, globalObject, name, lexicalSlot))
        return lexicalSlot.getValue(globalObject, name);
    PropertySlot slot(globalObject, PropertySlot::InternalMethodType::Get);
    bool found = globalObject->getPropertySlot(globalObject, name, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!found) {
        if (throwsIfMissing)
            throwException(globalObject, scope, createUndefinedVariableError(globalObject, name));
        return jsUndefined();
    }
    RELEASE_AND_RETURN(scope, slot.getValue(globalObject, name));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetFree, EncodedJSValue, (Instance* instance, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(getFree(globalObject, vm, nameAt(instance, name), true)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetFreeForTypeOf, EncodedJSValue, (Instance* instance, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(getFree(globalObject, vm, nameAt(instance, name), false)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetFree, void, (Instance* instance, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    // All code is strict: an assignment does not create a global variable.
    bool exists = globalObject->hasProperty(globalObject, identifier);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!exists) {
        throwException(globalObject, scope, createUndefinedVariableError(globalObject, identifier));
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(globalObject, true);
    globalObject->methodTable()->put(globalObject, globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetByName, EncodedJSValue, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (identifier.isPrivateName()) [[unlikely]]
        OPERATION_RETURN(scope, JSValue::encode(privateGet(globalObject, vm, JSValue::decode(base), identifier)));
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(base).get(globalObject, identifier)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSetByName, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    if (identifier.isPrivateName()) [[unlikely]] {
        privateSet(globalObject, vm, JSValue::decode(base), identifier, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(JSValue::decode(base), true);
    JSValue::decode(base).put(globalObject, identifier, JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

// CreateDataPropertyOrThrow. On an ordinary object nothing can be in its way where the program does this.
static void createDataProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName name, JSValue value)
{
    if (object->structure()->compiledLayout())
        object->createDataProperty(globalObject, name, value, true);
    else
        object->putDirectMayBeIndex(globalObject, name, value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDefineByName, void, (Instance* instance, EncodedJSValue base, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const Identifier& identifier = nameAt(instance, name);
    // A field that is `#name` is there from when the object is made.
    if (identifier.isPrivateName()) [[unlikely]] {
        privateSet(globalObject, vm, JSValue::decode(base), identifier, JSValue::decode(value));
        OPERATION_RETURN(scope);
    }
    asObject(JSValue::decode(base))->createDataProperty(globalObject, identifier, JSValue::decode(value), true);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedHasName, size_t, (Instance* instance, EncodedJSValue base, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(base);
    const Identifier& identifier = nameAt(instance, name);
    if (identifier.isPrivateName()) [[unlikely]]
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
    if (!deleted)
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedDeleteByKey, size_t, (Instance* instance, EncodedJSValue base, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = JSValue::decode(base).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto property = JSValue::decode(key).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    bool deleted = JSCell::deleteProperty(object, globalObject, property);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (!deleted)
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
    OPERATION_RETURN(scope, deleted);
}

static JSValue callValue(JSGlobalObject* globalObject, VM& vm, JSValue callee, JSValue thisValue, uint32_t count, const EncodedJSValue* values)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto callData = JSC::getCallData(callee);
    if (callData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, makeString(callee.toWTFString(globalObject), " is not a function"_s));
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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCallMethod, EncodedJSValue, (Instance* instance, EncodedJSValue base, uint32_t name, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue baseValue = JSValue::decode(base);
    const Identifier& identifier = nameAt(instance, name);
    JSValue callee = identifier.isPrivateName() ? privateGet(globalObject, vm, baseValue, identifier) : baseValue.get(globalObject, identifier);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!callee.isCallable()) {
        throwTypeError(globalObject, scope, makeString(identifier.string(), " is not a function"_s));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(callValue(globalObject, vm, callee, baseValue, count, values)));
}

static JSValue requireFunctionOfHost(JSGlobalObject* globalObject, Instance* instance)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = *instance->typed;
    if (!state.importMeta) {
        auto* importer = uncheckedDowncast<JSModuleRecord>(state.moduleRecord);
        state.importMeta = globalObject->moduleLoader()->createImportMetaProperties(globalObject, identifierToJSValue(vm, importer->moduleKey()), importer, nullptr);
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
        throwTypeError(globalObject, scope, makeString("Cannot find module '"_s, specifier.toWTFString(globalObject), "': the host has no require()"_s));
        return { };
    }
    MarkedArgumentBuffer arguments;
    arguments.append(specifier);
    RELEASE_AND_RETURN(scope, call(globalObject, require, callData, jsUndefined(), arguments));
}

// What an ES module sees of `module.exports`, as in a bundle: `default`, and a getter for each own property.
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
    }
    bool hasDefault = isNodeMode || !source;
    if (!hasDefault) {
        JSValue marker = source->get(globalObject, Identifier::fromString(vm, "__esModule"_s));
        RETURN_IF_EXCEPTION(scope, nullptr);
        hasDefault = !marker.toBoolean(globalObject);
    }
    if (hasDefault)
        result->putDirect(vm, vm.propertyNames->defaultKeyword, exported);
    if (!source)
        return result;
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    source->methodTable()->getOwnPropertyNames(source, globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (auto& name : names) {
        if (result->hasOwnProperty(globalObject, name))
            continue;
        // A live view: the property is read from `module.exports` on each use.
        MarkedArgumentBuffer boundArguments;
        boundArguments.append(identifierToJSValue(vm, name));
        JSObject* getter = JSBoundFunction::create(vm, globalObject, instance->typed->liveBindingGetter(vm), source, boundArguments, 0, nullptr, SourceTaintedOrigin::Untainted);
        RETURN_IF_EXCEPTION(scope, nullptr);
        result->putDirectAccessor(globalObject, name, GetterSetter::create(vm, globalObject, getter, nullptr), PropertyAttribute::Accessor | 0);
    }
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetExternalModule, EncodedJSValue, (Instance* instance, uint32_t specifier))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    AbstractModuleRecord* importer = instance->typed->moduleRecord;
    RELEASE_ASSERT(importer);
    const Identifier& name = nameAt(instance, specifier);
    AbstractModuleRecord* imported = importer->hostResolveImportedModule(globalObject, name, ScriptFetchParameters::Type::JavaScript);
    if (!imported) {
        // A bundler may have written a built-in module the other way.
        String other = name.string().startsWith("node:"_s) ? name.string().string().substring(5) : makeString("node:"_s, name.string());
        imported = importer->hostResolveImportedModule(globalObject, Identifier::fromString(vm, other), ScriptFetchParameters::Type::JavaScript);
    }
    if (imported)
        OPERATION_RETURN(scope, JSValue::encode(imported->getModuleNamespace(globalObject)));
    JSValue exported = requireFromHost(globalObject, instance, jsString(vm, name.string()));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(toESM(globalObject, instance, exported, true)));
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

static void defineAccessor(JSGlobalObject* globalObject, JSValue object, JSValue key, JSValue function, bool isEnumerable, bool isGetter)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto property = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    PropertyDescriptor descriptor;
    if (isGetter)
        descriptor.setGetter(function);
    else
        descriptor.setSetter(function);
    descriptor.setEnumerable(isEnumerable);
    descriptor.setConfigurable(true);
    scope.release();
    asObject(object)->methodTable()->defineOwnProperty(asObject(object), globalObject, property, descriptor, true);
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
    // Only an ordinary function of the program takes the object that was made for it.
    auto* closure = dynamicDowncast<Typed::Closure>(callee);
    if (!closure || closure->kind() != Typed::Format::FunctionKind::Plain || closure->isDeclaredAs(Typed::functionIsAsync | Typed::functionIsGenerator)) {
        auto constructData = JSC::getConstructData(callee);
        if (constructData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, "The value is not a constructor"_s);
            return { };
        }
        RELEASE_AND_RETURN(scope, construct(globalObject, callee, constructData, arguments));
    }
    // OrdinaryCreateFromConstructor(newTarget, "%Object.prototype%").
    JSValue prototype = closure->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    object->setPrototypeAtBirth(vm, prototype.isObject() ? asObject(prototype) : closure->realm()->objectPrototype());
    JSValue result = closure->callAsConstructor(globalObject, object, closure, arguments);
    RETURN_IF_EXCEPTION(scope, { });
    return result.isObject() ? result : JSValue(object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConstructWith, EncodedJSValue, (Instance* instance, EncodedJSValue callee, JSCell* object, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, JSValue::encode(constructWith(globalObject, vm, JSValue::decode(callee), object, count, values)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedLanguageOperation, EncodedJSValue, (Instance* instance, uint32_t which, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto argument = [&](unsigned index) {
        return index < count ? JSValue::decode(values[index]) : jsUndefined();
    };
    auto copyDataProperties = [&](JSObject* target, JSValue source, const UncheckedKeyHashSet<RefPtr<UniquedStringImpl>>& excluded) {
        if (source.isUndefinedOrNull())
            return;
        JSObject* from = source.toObject(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
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
    case Typed::LanguageOperation::DefineField: {
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        createDataProperty(globalObject, asObject(argument(0)), key, argument(2));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::DefineGetter:
    case Typed::LanguageOperation::DefineSetter:
        defineAccessor(globalObject, argument(0), argument(1), argument(2), argument(3).isTrue(), static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::DefineGetter);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::ForInKeys: {
        JSArray* keys = constructEmptyArray(globalObject, nullptr);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (argument(0).isUndefinedOrNull())
            OPERATION_RETURN(scope, JSValue::encode(keys));
        JSObject* object = argument(0).toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        object->getPropertyNames(globalObject, names, DontEnumPropertiesMode::Exclude);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        for (auto& name : names) {
            keys->push(globalObject, jsString(vm, name.string()));
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(keys));
    }
    case Typed::LanguageOperation::AppendSpread: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
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
    case Typed::LanguageOperation::ConstructWithArray: {
        bool isCall = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::Apply;
        JSArray* array = uncheckedDowncast<JSArray>(argument(isCall ? 2 : 1));
        MarkedArgumentBuffer arguments;
        for (unsigned i = 0; i < array->length(); ++i) {
            arguments.append(array->getIndex(globalObject, i));
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        if (arguments.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        if (isCall) {
            auto callData = JSC::getCallData(argument(0));
            if (callData.type == CallData::Type::None) {
                throwTypeError(globalObject, scope, "The value is not a function"_s);
                OPERATION_RETURN(scope, encodedJSValue());
            }
            OPERATION_RETURN(scope, JSValue::encode(call(globalObject, argument(0), callData, argument(1), arguments)));
        }
        auto constructData = JSC::getConstructData(argument(0));
        if (constructData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, "The value is not a constructor"_s);
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
        instance->typed->makeClass(globalObject, classObject, argument(1));
        OPERATION_RETURN(scope, values[0]);
    }
    case Typed::LanguageOperation::NewObjectOfClass: {
        auto* classObject = dynamicDowncast<Typed::Closure>(argument(0));
        RELEASE_ASSERT(classObject && classObject->classData());
        OPERATION_RETURN(scope, JSValue::encode(instance->typed->createObjectOfClass(vm, *classObject->classData(), asObject(classObject->getDirect(vm, vm.propertyNames->prototype)))));
    }
    case Typed::LanguageOperation::DefineMethod: {
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        // `#name() {}` cannot be stored to.
        PropertyDescriptor descriptor(argument(2), key.isPrivateName() ? PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly : static_cast<unsigned>(PropertyAttribute::DontEnum));
        asObject(argument(0))->methodTable()->defineOwnProperty(asObject(argument(0)), globalObject, key, descriptor, true);
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::SuperGet:
    case Typed::LanguageOperation::SuperSet: {
        bool isGet = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::SuperGet;
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
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
        JSValue parent = asObject(argument(0))->getPrototype(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        auto constructData = JSC::getConstructData(parent);
        if (constructData.type == CallData::Type::None) {
            throwTypeError(globalObject, scope, "The superclass is not a constructor."_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(construct(globalObject, parent, constructData, arguments, argument(1))));
    }
    case Typed::LanguageOperation::ConstructorResult:
        if (argument(0).isObject())
            OPERATION_RETURN(scope, values[0]);
        if (!argument(0).isUndefined() && !argument(1).isObject()) {
            throwTypeError(globalObject, scope, "Cannot return a non-object type in the constructor of a derived class."_s);
            OPERATION_RETURN(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, values[1]);
    case Typed::LanguageOperation::NewPromise:
        OPERATION_RETURN(scope, JSValue::encode(JSPromise::create(vm, globalObject->promiseStructure())));
    case Typed::LanguageOperation::ResolvePromise:
        uncheckedDowncast<JSPromise>(argument(0))->resolve(globalObject, vm, argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::RejectPromise:
        uncheckedDowncast<JSPromise>(argument(0))->reject(vm, argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::Await: {
        // The driver of an async generator has to tell an await from a yield.
        auto* frame = uncheckedDowncast<Typed::Object>(uncheckedDowncast<Typed::Closure>(argument(0))->environment());
        if (auto* generator = dynamicDowncast<Typed::AsyncGenerator>(frame->read(frame->layout().fields()[Typed::CoroutineFrame::promiseField])))
            generator->willSuspendAtAwait();
        JSPromise::resolveWithInternalMicrotaskForAsyncAwait(globalObject, vm, argument(1), InternalMicrotask::AOTAsyncFunctionResume, argument(0));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::NewGenerator: {
        auto* resumeClosure = uncheckedDowncast<Typed::Closure>(argument(0));
        OPERATION_RETURN(scope, JSValue::encode(Typed::Generator::create(vm, instance->typed->generatorStructure(vm, resumeClosure->functionIndex(), false), resumeClosure)));
    }
    case Typed::LanguageOperation::NewAsyncGenerator: {
        auto* resumeClosure = uncheckedDowncast<Typed::Closure>(argument(0));
        OPERATION_RETURN(scope, JSValue::encode(Typed::AsyncGenerator::create(vm, globalObject, instance->typed->generatorStructure(vm, resumeClosure->functionIndex(), true), resumeClosure)));
    }
    case Typed::LanguageOperation::CreateArguments: {
        JSArray* array = uncheckedDowncast<JSArray>(argument(0));
        unsigned length = array->length();
        ClonedArguments* result = ClonedArguments::createEmpty(vm, globalObject, globalObject->clonedArgumentsStructure(), nullptr, length, nullptr);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        for (unsigned i = 0; i < length; ++i) {
            result->putDirectIndex(globalObject, i, array->getIndex(globalObject, i));
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        // Without a callee to take them from, it counts as having its special properties already.
        result->putDirectAccessor(globalObject, vm.propertyNames->callee, globalObject->throwTypeErrorArgumentsCalleeGetterSetter(), PropertyAttribute::DontDelete | PropertyAttribute::DontEnum | PropertyAttribute::Accessor);
        result->putDirect(vm, vm.propertyNames->iteratorSymbol, globalObject->arrayProtoValuesFunction(), static_cast<unsigned>(PropertyAttribute::DontEnum));
        OPERATION_RETURN(scope, JSValue::encode(result));
    }
    case Typed::LanguageOperation::GetAsyncIterator: {
        JSValue method = argument(0).get(globalObject, vm.propertyNames->asyncIteratorSymbol);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        bool isSync = method.isUndefinedOrNull();
        if (isSync) {
            method = argument(0).get(globalObject, vm.propertyNames->iteratorSymbol);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
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
        if (!isSync)
            OPERATION_RETURN(scope, JSValue::encode(iterator));
        JSValue nextMethod = iterator.get(globalObject, vm.propertyNames->next);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(JSAsyncFromSyncIterator::create(vm, globalObject->asyncFromSyncIteratorStructure(), asObject(iterator), nextMethod, IterationMode::Generic)));
    }
    case Typed::LanguageOperation::GetIterator: {
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
        stack->push(globalObject, jsBoolean(isAsync));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    }
    case Typed::LanguageOperation::RecordDisposeError:
        recordDisposeError(globalObject, uncheckedDowncast<JSArray>(argument(0)), argument(1));
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    case Typed::LanguageOperation::DisposeNextResource:
    case Typed::LanguageOperation::DisposeResources: {
        JSArray* stack = uncheckedDowncast<JSArray>(argument(0));
        bool disposesAll = static_cast<Typed::LanguageOperation>(which) == Typed::LanguageOperation::DisposeResources;
        if (JSValue hasError = stack->getDirect(vm, Identifier::fromString(vm, "hasError"_s)); disposesAll && argument(1).isTrue() && !hasError)
            recordDisposeError(globalObject, stack, argument(2));
        while (stack->length()) {
            bool isAsync = stack->pop(globalObject).isTrue();
            JSValue method = stack->pop(globalObject);
            JSValue resource = stack->pop(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
            JSValue result = jsUndefined();
            if (!method.isUndefined()) {
                auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
                result = call(globalObject, method, JSC::getCallData(method), resource, ArgList());
                if (Exception* exception = catchScope.exception()) [[unlikely]] {
                    JSValue error = exception->value();
                    if (!catchScope.clearExceptionExceptTermination())
                        OPERATION_RETURN(scope, encodedJSValue());
                    recordDisposeError(globalObject, stack, error);
                    result = jsUndefined();
                }
            }
            if (!disposesAll)
                OPERATION_RETURN(scope, JSValue::encode(isAsync ? result : jsNull()));
        }
        if (!disposesAll)
            OPERATION_RETURN(scope, values[0]);
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
    case Typed::LanguageOperation::ParseJSON: {
        auto text = asString(argument(0))->value(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        JSValue parsed = JSONParseWithException(globalObject, text);
        OPERATION_RETURN(scope, JSValue::encode(parsed));
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
        if (!state.importMeta) {
            auto* importer = uncheckedDowncast<JSModuleRecord>(state.moduleRecord);
            state.importMeta = globalObject->moduleLoader()->createImportMetaProperties(globalObject, identifierToJSValue(vm, importer->moduleKey()), importer, nullptr);
            OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        }
        OPERATION_RETURN(scope, JSValue::encode(state.importMeta));
    }
    case Typed::LanguageOperation::ImportExternalModule: {
        JSString* specifier = argument(0).toString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        auto* importer = uncheckedDowncast<JSModuleRecord>(instance->typed->moduleRecord);
        OPERATION_RETURN(scope, JSValue::encode(globalObject->moduleLoader()->importModule(globalObject, specifier, jsUndefined(), SourceOrigin { importer->sourceCode().provider()->sourceOrigin() })));
    }
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
    case Typed::LanguageOperation::HasEnumerableProperty: {
        JSObject* object = argument(0).toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        auto key = argument(1).toPropertyKey(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(object->hasEnumerableProperty(globalObject, key))));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConstruct, EncodedJSValue, (Instance* instance, EncodedJSValue callee, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue constructor = JSValue::decode(callee);
    auto constructData = JSC::getConstructData(constructor);
    if (constructData.type == CallData::Type::None) {
        throwTypeError(globalObject, scope, "The value is not a constructor"_s);
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIsCallable, size_t, (Instance*, JSCell* cell))
{
    return cell->isCallable();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIsClosureOf, size_t, (Instance*, JSCell* cell, uint32_t function))
{
    auto* closure = dynamicDowncast<Typed::Closure>(cell);
    return closure && closure->functionIndex() == function;
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

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCoerce, EncodedJSValue, (Instance* instance, EncodedJSValue value, uint32_t type, uint32_t reason, uint32_t function, uint32_t position))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    OPERATION_RETURN(scope, JSValue::encode(state.coerce(globalObject, JSValue::decode(value), state.type(type), static_cast<Typed::Format::CastReason>(reason), function, position)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDidCreateField, void, (Instance* instance, JSCell* cell, uint32_t name))
{
    State& state = *instance->typed;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    const Typed::FieldData* field = object->fieldNamed(state, nameAt(instance, name));
    RELEASE_ASSERT(field);
    object->didCreateField(state, *field);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetExtra, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    JSObject* extra = object->extraProperties();
    unsigned attributes;
    JSValue value = extra ? extra->getDirect(vm, nameAt(instance, name), attributes) : JSValue();
    if (!value)
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue) {
        throwTypeError(globalObject, scope, makeString("Cannot read property '"_s, nameAt(instance, name).string(), "' of an object of type "_s, instance->typed->nameOf(object->layout()), ": it is an accessor"_s));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedHasExtra, size_t, (Instance* instance, JSCell* cell, uint32_t name))
{
    JSObject* extra = uncheckedDowncast<Typed::Object>(cell)->extraProperties();
    return extra && extra->getDirectOffset(*instance->vm, nameAt(instance, name)) != invalidOffset;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPutExtra, void, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
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
    return instance->typed->string(*instance->vm, index);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedBarrierSlow, void, (Instance* instance, JSCell* cell))
{
    instance->vm->writeBarrierSlowPath(cell);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedPollSlow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN(scope);
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
    JSValue value = exception->value();
    scope.clearException();
    return JSValue::encode(value);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowTypeMismatch, void, (Instance* instance, EncodedJSValue value, uint32_t kinds, uint32_t name, uint32_t function, uint32_t position))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    throwTypeError(globalObject, scope, makeString(State::describe(JSValue::decode(value)), " is not of type "_s, state.describeKinds(kinds, name), state.describePosition(function, position)));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNonNullFailed, void, (Instance* instance, EncodedJSValue encodedValue, uint32_t type, uint32_t goesOn, uint32_t function, uint32_t position))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    State& state = *instance->typed;
    JSValue value = JSValue::decode(encodedValue);
    String what = value.isUndefinedOrNull() ? makeString("`!` on "_s, State::describe(value)) : makeString(State::describe(value), " is not of type "_s, state.describe(state.type(type)));
    if (Options::reportAOTTypedCoercions()) [[unlikely]]
        state.reportCoercion(function, position, what);
    if (!goesOn)
        throwTypeError(globalObject, scope, makeString(what, state.describePosition(function, position)));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowNullish, void, (Instance* instance, EncodedJSValue value, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwTypeError(globalObject, scope, makeString(JSValue::decode(value).isNull() ? "null"_s : "undefined"_s, " is not an object (evaluating its property '"_s, nameAt(instance, name).string(), "')"_s));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFactDoesNotHold, void, (Instance*, EncodedJSValue value, uint32_t function, uint32_t number))
{
    dataLogLn("AOT: a fact does not hold: f", function, ": v", number, " is ", State::describe(JSValue::decode(value)));
    CRASH();
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowStackOverflow, void, (Instance* instance))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwStackOverflowError(globalObject, scope);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedThrowTDZError, void, (Instance* instance, uint32_t))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    throwException(globalObject, scope, createReferenceError(globalObject, "Cannot access a variable before its initialization"_s));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewObject, JSCell*, (Instance* instance))
{
    return constructEmptyObject(instance->globalObject);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArray, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    Structure* structure = globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous);
    JSArray* array = JSArray::tryCreate(vm, structure, count);
    if (!array) {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    for (uint32_t i = 0; i < count; ++i)
        array->putDirectIndex(globalObject, i, JSValue::decode(values[i]));
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPush, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    if (array->type() != ArrayType) [[unlikely]] {
        JSValue callee = array->get(globalObject, vm.propertyNames->builtinNames().pushPublicName());
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(callValue(globalObject, vm, callee, array, 1, &value)));
    }
    array->push(globalObject, JSValue::decode(value));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(array->length())));
}

static void throwWouldRunCode(JSGlobalObject* globalObject, ThrowScope& scope)
{
    throwTypeError(globalObject, scope, "An array was changed after it was taken for a plain array, and reading from it would run code"_s);
}

// [[Get]] as far as that runs no code of the program. Nothing: it would, and that has been thrown.
static std::optional<JSValue> getWithoutRunningCode(JSGlobalObject* globalObject, JSArray* array, PropertyName name, std::optional<uint32_t> index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(array, PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool isFound = index ? array->getPropertySlot(globalObject, *index, slot) : array->getPropertySlot(globalObject, name, slot);
    scope.assertNoException();
    if (slot.isTaintedByOpaqueObject() || (isFound && !slot.isValue())) {
        throwWouldRunCode(globalObject, scope);
        return std::nullopt;
    }
    if (!isFound)
        return jsUndefined();
    return slot.getValue(globalObject, name);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayGet, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue key))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    std::optional<uint32_t> index = JSValue::decode(key).tryGetAsUint32Index();
    Identifier name;
    if (!index)
        name = Identifier::from(vm, JSValue::decode(key).asNumber());
    auto value = getWithoutRunningCode(globalObject, array, name, index);
    OPERATION_RETURN(scope, value ? JSValue::encode(*value) : encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedArrayPut, void, (Instance* instance, JSCell* cell, EncodedJSValue key, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    if (std::optional<uint32_t> index = JSValue::decode(key).tryGetAsUint32Index()) {
        array->methodTable()->putByIndex(array, globalObject, *index, JSValue::decode(value), true);
        OPERATION_RETURN(scope);
    }
    PutPropertySlot slot(array, true);
    array->methodTable()->put(array, globalObject, Identifier::from(vm, JSValue::decode(key).asNumber()), JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedCallReaderOfArray, EncodedJSValue, (Instance* instance, JSCell* cell, uint32_t builtin, uint32_t count, const EncodedJSValue* values))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSArray* array = uncheckedDowncast<JSArray>(cell);
    auto& builtins = Typed::Builtins::singleton();
    JSCell*& callee = instance->typedBuiltins[builtins.cellOf(Typed::BuiltinID(builtin))];
    if (!callee)
        callee = builtins.findCell(globalObject, builtins.cellOf(Typed::BuiltinID(builtin)));
    if (array->type() != ArrayType) [[unlikely]] {
        auto& entry = builtins.at(Typed::BuiltinID(builtin));
        auto found = getWithoutRunningCode(globalObject, array, Identifier::fromString(vm, entry.name), std::nullopt);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        // The function of the engine asks the array for its elements, and the prototype chain where there is a hole.
        if (*found != JSValue(callee) || (entry.effects & Typed::Effect::ReadsThis && array->holesMustForwardToPrototype())) {
            throwWouldRunCode(globalObject, scope);
            OPERATION_RETURN(scope, encodedJSValue());
        }
    }
    OPERATION_RETURN(scope, JSValue::encode(callValue(globalObject, vm, callee, array, count, values)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedOpenChangedArray, void, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto found = getWithoutRunningCode(globalObject, uncheckedDowncast<JSArray>(cell), vm.propertyNames->iteratorSymbol, std::nullopt);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (*found != JSValue(globalObject->linkTimeConstant(LinkTimeConstant::arrayProtoValues)))
        throwWouldRunCode(globalObject, scope);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedBuiltinCell, JSCell*, (Instance* instance, uint32_t index))
{
    JSCell* cell = Typed::Builtins::singleton().findCell(instance->globalObject, index);
    instance->typedBuiltins[index] = cell;
    return cell;
}

template<typename View>
static JSCell* newView(JSGlobalObject* globalObject, size_t length)
{
    return View::create(globalObject, globalObject->typedArrayStructure(View::TypedArrayStorageType, false), length);
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
        OPERATION_RETURN(scope, newView<JS ## name ## Array>(globalObject, length));
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFunctionEntry, void*, (Instance* instance, uint32_t function))
{
    return std::bit_cast<void*>(instance->typed->entry(function));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedClosureConstant, JSCell*, (Instance* instance, uint32_t function))
{
    return instance->typed->closure(*instance->vm, function);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNewArrayWithLength, JSCell*, (Instance* instance, uint64_t bits))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    int64_t length = static_cast<int64_t>(bits);
    if (length < 0 || length > UINT32_MAX) {
        throwException(globalObject, scope, createRangeError(globalObject, "Array size is not a small enough positive integer."_s));
        OPERATION_RETURN(scope, nullptr);
    }
    JSArray* array = length < MIN_ARRAY_STORAGE_CONSTRUCTION_LENGTH ? JSArray::tryCreate(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), length) : nullptr;
    if (!array)
        array = constructEmptyArray(globalObject, nullptr, static_cast<uint32_t>(length));
    OPERATION_RETURN(scope, array);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedConcat, JSCell*, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, jsString(globalObject, uncheckedDowncast<JSString>(left), uncheckedDowncast<JSString>(right)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedSubstring, JSCell*, (Instance* instance, JSCell* string, int32_t from, int32_t to))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    OPERATION_RETURN(scope, jsSubstring(globalObject, uncheckedDowncast<JSString>(string), from, to - from));
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
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto characters = uncheckedDowncast<JSString>(string)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto otherCharacters = uncheckedDowncast<JSString>(other)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    size_t found = characters->find(otherCharacters.data, place);
    OPERATION_RETURN(scope, found == notFound ? static_cast<size_t>(-1) : found);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIntToString, JSCell*, (Instance* instance, uint64_t bits))
{
    VM& vm = *instance->vm;
    int64_t value = static_cast<int64_t>(bits);
    if (value == static_cast<int32_t>(value))
        return int32ToString(vm, static_cast<int32_t>(value), 10);
    return jsString(vm, String::number(static_cast<double>(value)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedDoubleToString, JSCell*, (Instance* instance, double value))
{
    VM& vm = *instance->vm;
    return jsString(vm, vm.numericStrings.add(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCharToString, JSCell*, (Instance* instance, uint32_t character))
{
    return jsSingleCharacterString(*instance->vm, static_cast<char16_t>(character));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedNewMap, JSCell*, (Instance* instance))
{
    return JSMap::create(*instance->vm, instance->globalObject->mapStructure());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterOpen, EncodedJSValue, (Instance* instance, EncodedJSValue iterable))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    // An array whose iteration nothing can observe stands for its own iterator: compiled code steps through it by index. Anything else
    // gets a record, which is no array.
    // (Not an object of a class of the host that extends JSArray: it may keep its length anywhere.)
    if (auto* array = dynamicDowncast<JSArray>(JSValue::decode(iterable)); array && array->classInfo() == JSArray::info() && array->isIteratorProtocolFastAndNonObservable())
        OPERATION_RETURN(scope, iterable);
    IterationRecord record = iteratorForIterable(globalObject, JSValue::decode(iterable));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    JSCellButterfly* state = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, 3);
    state->setIndex(vm, 0, record.iterator);
    state->setIndex(vm, 1, record.nextMethod);
    state->setIndex(vm, 2, jsUndefined());
    OPERATION_RETURN(scope, JSValue::encode(state));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterHas, size_t, (Instance* instance, EncodedJSValue encodedState))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSCellButterfly* state = uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell());
    IterationRecord record { state->get(0), state->get(1) };
    JSValue next = iteratorStep(globalObject, record);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (next.isFalse())
        OPERATION_RETURN(scope, 0);
    JSValue value = iteratorValue(globalObject, next);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    state->setIndex(vm, 2, value);
    OPERATION_RETURN(scope, 1);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedIterGet, EncodedJSValue, (Instance*, EncodedJSValue encodedState))
{
    return JSValue::encode(uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell())->get(2));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterClose, void, (Instance* instance, EncodedJSValue encodedState))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSCellButterfly* state = uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell());
    iteratorClose(globalObject, state->get(0));
    OPERATION_RETURN(scope);
}

// IteratorClose with a throw completion: that completion wins over whatever `return` does.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedIterCloseForThrow, void, (Instance* instance, EncodedJSValue encodedState))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSCellButterfly* state = uncheckedDowncast<JSCellButterfly>(JSValue::decode(encodedState).asCell());
    {
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        JSValue iterator = state->get(0);
        JSValue method = iterator.get(globalObject, vm.propertyNames->returnKeyword);
        if (!catchScope.exception() && !method.isUndefinedOrNull()) {
            if (auto callData = JSC::getCallData(method); callData.type != CallData::Type::None)
                call(globalObject, method, callData, iterator, ArgList());
        }
        catchScope.clearExceptionExceptTermination();
    }
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
