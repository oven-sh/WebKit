/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "DeferTermination.h"
#include "Error.h"
#include "JSBigInt.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTIncrement, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTIncrement);
    JSValue numeric = JSValue::decode(encodedValue).toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::inc(globalObject, numeric.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(numeric.asNumber() + 1)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTDecrement, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDecrement);
    JSValue numeric = JSValue::decode(encodedValue).toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::dec(globalObject, numeric.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(numeric.asNumber() - 1)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToObject, JSCell*, (Instance* instance, EncodedJSValue encodedValue, uint32_t message))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTToObject);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull()) [[unlikely]] {
        if (String text = ProgramStrings::text(instance->tables(), Typed::StringID(message)); !text.isEmpty()) {
            throwException(globalObject, scope, createTypeError(globalObject, text));
            OPERATION_RETURN(scope, nullptr);
        }
    }
    OPERATION_RETURN(scope, value.toObject(globalObject));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToThis, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTToThis);
    DeferTerminationForAWhile deferTermination(vm);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull())
        OPERATION_RETURN(scope, JSValue::encode(globalObject->globalThis()));
    OPERATION_RETURN(scope, JSValue::encode(value.toObject(globalObject)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
