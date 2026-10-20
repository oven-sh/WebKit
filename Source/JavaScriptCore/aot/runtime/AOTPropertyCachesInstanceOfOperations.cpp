/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "Error.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static void cacheRead(Instance* instance, JSObject* constructor, Structure* structureBefore, const Identifier& name, const PropertySlot& slot)
{
    UniquedStringImpl* uid = name.impl();
    PropertyAnswer answer = classifyRead({ instance->globalObject, constructor, structureBefore, uid, slot });
    instance->state->propertyCaches->cache({ structureBefore, { uid, std::nullopt }, SlotAccess::Read }, answer);
    ensureStillAliveHere(structureBefore);
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceOfAndCache, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedConstructor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInstanceOfAndCache);
    JSValue value = JSValue::decode(encodedValue);
    JSValue constructor = JSValue::decode(encodedConstructor);
    if (!constructor.isObject()) {
        throwException(globalObject, scope, createTypeError(globalObject, "Right hand side of instanceof is not an object"_s));
        OPERATION_RETURN(scope, false);
    }

    JSObject* constructorObject = asObject(constructor);
    Structure* structureBeforeHasInstance = constructorObject->structure();
    PropertySlot hasInstanceSlot(constructorObject, PropertySlot::InternalMethodType::Get);
    bool hasHasInstance = constructorObject->getPropertySlot(globalObject, vm.propertyNames->hasInstanceSymbol, hasInstanceSlot);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    JSValue hasInstance = hasHasInstance ? hasInstanceSlot.getValue(globalObject, vm.propertyNames->hasInstanceSymbol) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    cacheRead(instance, constructorObject, structureBeforeHasInstance, vm.propertyNames->hasInstanceSymbol, hasInstanceSlot);
    if (hasInstance != globalObject->functionProtoHasInstanceSymbolFunction() || !constructorObject->structure()->typeInfo().implementsDefaultHasInstance())
        OPERATION_RETURN(scope, constructorObject->hasInstance(globalObject, value, hasInstance));

    // OrdinaryHasInstance reads "prototype" only for an object, so for any other value the lookup that fills the cache may run no code.
    bool readsPrototype = value.isObject();
    Structure* structureBeforePrototype = constructorObject->structure();
    PropertySlot prototypeSlot(constructorObject, readsPrototype ? PropertySlot::InternalMethodType::Get : PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool hasPrototype = constructorObject->getPropertySlot(globalObject, vm.propertyNames->prototype, prototypeSlot);
    prototypeSlot.disallowVMEntry.reset();
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    JSValue prototype = hasPrototype && readsPrototype ? prototypeSlot.getValue(globalObject, vm.propertyNames->prototype) : jsUndefined();
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    if (!prototypeSlot.isTaintedByOpaqueObject())
        cacheRead(instance, constructorObject, structureBeforePrototype, vm.propertyNames->prototype, prototypeSlot);
    OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, value, prototype));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefaultHasInstance, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedPrototype))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDefaultHasInstance);
    OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, JSValue::decode(encodedValue), JSValue::decode(encodedPrototype)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
