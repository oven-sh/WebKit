/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinFamilyState.h"
#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTObjectWords.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyCachesKeys.h"
#include "AOTState.h"
#include "DeferGC.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "ObjectConstructor.h"
#include "ObjectPrototype.h"
#include <array>

namespace JSC { namespace AOT {

using Typed::OwnKeysKind;

static BuiltinID engineFunctionFor(OwnKeysKind kind)
{
    switch (kind) {
    case OwnKeysKind::Keys:
        return Builtins::ObjectKeys;
    case OwnKeysKind::Names:
        return Builtins::ObjectGetOwnPropertyNames;
    case OwnKeysKind::Symbols:
        return Builtins::ObjectGetOwnPropertySymbols;
    case OwnKeysKind::All:
        return Builtins::ReflectOwnKeys;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void cacheKeyListByStructure(VM& vm, Object* compiled)
{
    DeferGC deferGC(vm);
    compiled->ensureStructureForExtraProperties(vm);
    if (!compiled->keysDependOnlyOnStructureAndOrderWord())
        return;
    JSCellButterfly* list = compiled->keysWithExtraProperties(vm);
    if (!list)
        return;
    uint32_t structureWord = compiled->structureID().bits() >> 1;
    unsafeMakeSpan(compiled->instance().objects.keyLists, numberOfKeyLists)[indexOfKeyList(structureWord, compiled->orderWord())] = { compiled->orderWord(), structureWord, list };
}

JSC_DEFINE_JIT_OPERATION(operationAOTOwnPropertyKeys, JSCell*, (Instance* instance, EncodedJSValue encodedValue, uint32_t encodedKind))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTOwnPropertyKeys);
    JSValue value = JSValue::decode(encodedValue);
    OwnKeysKind kind = static_cast<OwnKeysKind>(encodedKind);
    JSObject* object;
    if (value.isObject()) [[likely]] {
        object = asObject(value);
        markPristineIfOriginal(*instance, object);
    } else {
        if (kind == OwnKeysKind::All || value.isUndefinedOrNull()) {
            JSValue list = callInEngine(instance, engineFunctionFor(kind), value);
            OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
            OPERATION_RETURN(scope, list.asCell());
        }
        object = value.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (object->structure()->isDictionary()) [[unlikely]]
        flattenOnce(vm, object);
    Object* compiled = instance->state->objects->layouts.compiledObject(object);
    if (compiled) {
        if (!compiled->hasExtraProperties()) {
            if (kind != OwnKeysKind::Symbols)
                compiled->keys(vm);
        } else if (kind == OwnKeysKind::Keys) {
            cacheKeyListByStructure(vm, compiled);
            if (JSArray* keys = globalObject->isHavingABadTime() ? nullptr : compiled->tryCreateKeysArrayWithExtraProperties(globalObject))
                OPERATION_RETURN(scope, keys);
        }
    }
    switch (kind) {
    case OwnKeysKind::Keys:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Strings, DontEnumPropertiesMode::Exclude));
    case OwnKeysKind::Names:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Strings, DontEnumPropertiesMode::Include));
    case OwnKeysKind::Symbols:
        if (compiled && !compiled->hasExtraProperties() && !compiled->layout().hasFieldsWithSymbolKeys)
            OPERATION_RETURN(scope, constructEmptyArray(globalObject, nullptr));
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::Symbols, DontEnumPropertiesMode::Include));
    case OwnKeysKind::All:
        OPERATION_RETURN(scope, ownPropertyKeys(globalObject, object, PropertyNameMode::StringsAndSymbols, DontEnumPropertiesMode::Include));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTHasOwnProperty, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTHasOwnProperty);
    if (auto name = findNameEqualToLongString(JSValue::decode(encodedBase), JSValue::decode(encodedKey), false)) [[unlikely]]
        OPERATION_RETURN(scope, !!*name);
    JSObject* object = JSValue::decode(encodedBase).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, objectPrototypeHasOwnProperty(globalObject, object, name));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPropertyIsEnumerable, size_t, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTPropertyIsEnumerable);
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    JSObject* object = JSValue::decode(encodedBase).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    PropertyDescriptor descriptor;
    bool isOwn = object->getOwnPropertyDescriptor(globalObject, name, descriptor);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, isOwn && descriptor.enumerable());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSetLiteralPrototype, void, (Instance* instance, JSCell* cell, EncodedJSValue encodedPrototype))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSetLiteralPrototype);
    JSObject* object = cellArgument<JSObject>(cell, Entry::operationAOTSetLiteralPrototype);
    JSValue prototype = JSValue::decode(encodedPrototype);
    if (!prototype.isObject() && !prototype.isNull())
        return;
    if (Object* compiled = instance->state->objects->layouts.compiledObject(object)) {
        if (compiled->getPrototypeDirect() != prototype)
            compiled->setPrototypeAtCreation(vm, prototype);
        return;
    }
    object->setPrototypeDirect(vm, prototype);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTObjectCreate, JSCell*, (Instance* instance, JSCell* prototypeOrNull))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTObjectCreate);
    if (!prototypeOrNull) {
        Structure* structure = globalObject->nullPrototypeObjectStructure();
        prepareFinalObjectAllocation(instance, InlineAllocation::NullPrototypeObject, structure);
        OPERATION_RETURN(scope, constructEmptyObject(vm, structure));
    }
    JSObject* prototype = cellArgument<JSObject>(prototypeOrNull, Entry::operationAOTObjectCreate);
    prepareFinalObjectAllocation(instance, InlineAllocation::DefaultFinalObject, globalObject->objectStructureForObjectConstructor());
    JSFinalObject* object = constructEmptyObject(globalObject, prototype);
    instance->state->builtinFamilies->cacheObjectCreate(prototype, object->structure());
    OPERATION_RETURN(scope, object);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetOwnPropertyDescriptor, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetOwnPropertyDescriptor);
    JSValue base = JSValue::decode(encodedBase);
    JSObject* object;
    if (base.isObject()) [[likely]]
        object = asObject(base);
    else {
        if (base.isUndefinedOrNull())
            OPERATION_RETURN(scope, JSValue::encode(callInEngine(instance, Builtins::ObjectGetOwnPropertyDescriptor, base, JSValue::decode(encodedKey))));
        object = base.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(objectConstructorGetOwnPropertyDescriptor(globalObject, object, name)));
}

struct OwnPropertyFields {
    JSValue isPresent { jsBoolean(false) };
    JSValue value { jsUndefined() };
    JSValue writable { jsUndefined() };
    JSValue getter { jsUndefined() };
    JSValue setter { jsUndefined() };
    JSValue isEnumerable { jsBoolean(false) };
    JSValue isConfigurable { jsBoolean(false) };
};

static OwnPropertyFields ownPropertyFieldsOf(const PropertyDescriptor& descriptor)
{
    auto orUndefined = [](JSValue value) {
        return value ? value : jsUndefined();
    };
    return {
        .isPresent = jsBoolean(true),
        .value = orUndefined(descriptor.value()),
        .writable = descriptor.writablePresent() ? jsBoolean(descriptor.writable()) : jsUndefined(),
        .getter = descriptor.getterPresent() ? orUndefined(descriptor.getter()) : jsUndefined(),
        .setter = descriptor.setterPresent() ? orUndefined(descriptor.setter()) : jsUndefined(),
        .isEnumerable = jsBoolean(descriptor.enumerable()),
        .isConfigurable = jsBoolean(descriptor.configurable()),
    };
}

static void storeOwnPropertyFields(EncodedJSValue* results, const OwnPropertyFields& fields)
{
    std::array values { fields.isPresent, fields.value, fields.writable, fields.getter, fields.setter, fields.isEnumerable, fields.isConfigurable };
    auto words = unsafeMakeSpan(results, values.size());
    for (size_t i = 0; i < values.size(); ++i)
        words[i] = JSValue::encode(values[i]);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetOwnPropertyFields, void, (Instance* instance, EncodedJSValue encodedBase, EncodedJSValue encodedKey, EncodedJSValue* results))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetOwnPropertyFields);
    JSValue base = JSValue::decode(encodedBase);
    JSObject* object;
    if (base.isObject()) [[likely]]
        object = asObject(base);
    else {
        if (base.isUndefinedOrNull()) {
            callInEngine(instance, Builtins::ObjectGetOwnPropertyDescriptor, base, JSValue::decode(encodedKey));
            OPERATION_RETURN_IF_EXCEPTION(scope);
            storeOwnPropertyFields(results, { });
            OPERATION_RETURN(scope);
        }
        object = base.toObject(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope);
    }
    auto name = JSValue::decode(encodedKey).toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PropertyDescriptor descriptor;
    bool isOwn = object->getOwnPropertyDescriptor(globalObject, name, descriptor);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    storeOwnPropertyFields(results, isOwn ? ownPropertyFieldsOf(descriptor) : OwnPropertyFields { });
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
