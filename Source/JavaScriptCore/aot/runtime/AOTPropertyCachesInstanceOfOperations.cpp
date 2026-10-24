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
#include "AOTPropertyCachesNamedAccess.h"
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

bool instanceOfAndCache(Instance* instance, JSValue value, JSValue constructor)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!constructor.isObject()) {
        throwException(globalObject, scope, createInstanceofRightHandSideNotObjectError(globalObject));
        return false;
    }

    JSObject* constructorObject = asObject(constructor);
    createClassObjectsOnChain(vm, constructorObject, vm.propertyNames->hasInstanceSymbol.impl());
    Structure* structureBeforeHasInstance = WTF::opaque(constructorObject->structure());
    PropertySlot hasInstanceSlot(constructorObject, PropertySlot::InternalMethodType::Get);
    bool hasHasInstance = constructorObject->getPropertySlot(globalObject, vm.propertyNames->hasInstanceSymbol, hasInstanceSlot);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue hasInstance = hasHasInstance ? hasInstanceSlot.getValue(globalObject, vm.propertyNames->hasInstanceSymbol) : jsUndefined();
    RETURN_IF_EXCEPTION(scope, false);
    cacheRead(instance, constructorObject, structureBeforeHasInstance, vm.propertyNames->hasInstanceSymbol, hasInstanceSlot);
    if (hasInstance != globalObject->functionProtoHasInstanceSymbolFunction() || !constructorObject->structure()->typeInfo().implementsDefaultHasInstance())
        RELEASE_AND_RETURN(scope, constructorObject->hasInstance(globalObject, value, hasInstance));

    // OrdinaryHasInstance reads "prototype" only for an object, so for any other value the lookup that fills the cache may run no code.
    bool readsPrototype = value.isObject();
    Structure* structureBeforePrototype = WTF::opaque(constructorObject->structure());
    PropertySlot prototypeSlot(constructorObject, readsPrototype ? PropertySlot::InternalMethodType::Get : PropertySlot::InternalMethodType::VMInquiry, &vm);
    bool hasPrototype = constructorObject->getPropertySlot(globalObject, vm.propertyNames->prototype, prototypeSlot);
    prototypeSlot.disallowVMEntry.reset();
    RETURN_IF_EXCEPTION(scope, false);
    JSValue prototype = hasPrototype && readsPrototype ? prototypeSlot.getValue(globalObject, vm.propertyNames->prototype) : jsUndefined();
    RETURN_IF_EXCEPTION(scope, false);
    if (!prototypeSlot.isTaintedByOpaqueObject())
        cacheRead(instance, constructorObject, structureBeforePrototype, vm.propertyNames->prototype, prototypeSlot);
    RELEASE_AND_RETURN(scope, JSObject::defaultHasInstance(globalObject, value, prototype));
}

JSC_DEFINE_JIT_OPERATION(operationAOTInstanceOfAndCache, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedConstructor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInstanceOfAndCache);
    OPERATION_RETURN(scope, instanceOfAndCache(instance, JSValue::decode(encodedValue), JSValue::decode(encodedConstructor)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDefaultHasInstance, size_t, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedPrototype))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDefaultHasInstance);
    OPERATION_RETURN(scope, JSObject::defaultHasInstance(globalObject, JSValue::decode(encodedValue), JSValue::decode(encodedPrototype)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
