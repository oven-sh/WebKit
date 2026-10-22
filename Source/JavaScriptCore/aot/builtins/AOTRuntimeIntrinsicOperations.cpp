/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTErrorMessages.h"
#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "DeferGCInlines.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "LinkTimeConstant.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTRequireBuiltinModule, EncodedJSValue, (Instance* instance, uint32_t id))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireBuiltinModule);
    auto* requireBuiltinModule = globalObject->globalObjectMethodTable()->requireBuiltinModule;
    if (!requireBuiltinModule) [[unlikely]] {
        throwException(globalObject, scope, createBuiltinModulesUnavailableError(globalObject));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    JSValue result = requireBuiltinModule(globalObject, id);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNativeBinding, EncodedJSValue, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNativeBinding);
    auto* nativeBinding = globalObject->globalObjectMethodTable()->nativeBinding;
    if (!nativeBinding) [[unlikely]] {
        throwException(globalObject, scope, createNativeBindingsUnavailableError(globalObject));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    JSValue result = nativeBinding(globalObject, index);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLinkTimeConstant, JSCell*, (Instance* instance, uint32_t constant))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTLinkTimeConstant);
    DeferGCForAWhile deferGC(vm);
    JSCell* cell = globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(constant));
    instance->realm.linkTimeConstants[constant] = JSValue::encode(cell);
    OPERATION_RETURN(scope, cell);
}

JSC_DEFINE_JIT_OPERATION(operationAOTToLength, EncodedJSValue, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTToLength);
    double length = JSValue::decode(value).toLength(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(length)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetOwnProperty, EncodedJSValue, (Instance* instance, EncodedJSValue object, uint32_t key))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetOwnProperty);
    JSValue base = JSValue::decode(object);
    const Identifier& name = identifierAt(instance, key);
    PropertySlot slot(base, PropertySlot::InternalMethodType::GetOwnProperty);
    bool found = base.getOwnPropertySlot(globalObject, name, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(found ? slot.getValue(globalObject, name) : jsUndefined()));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
