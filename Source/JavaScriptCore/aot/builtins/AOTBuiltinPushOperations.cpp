/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include <array>
#include <span>

namespace JSC { namespace AOT {

static EncodedJSValue pushSeveralInPushOperations(Instance* instance, JSArray* array, std::span<const EncodedJSValue> values)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasAnyArrayStorage(array->indexingType()) || (isCopyOnWrite(array->indexingMode()) && array->hasFrozenElementsInPlace())) [[unlikely]] {
        MarkedArgumentBuffer arguments;
        for (EncodedJSValue value : values)
            arguments.append(JSValue::decode(value));
        if (arguments.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return encodedJSValue();
        }
        RELEASE_AND_RETURN(scope, JSValue::encode(callMethodInEngine(instance, Builtins::ArrayPrototypePush, array, arguments)));
    }
    for (EncodedJSValue value : values) {
        array->push(globalObject, JSValue::decode(value));
        RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    return JSValue::encode(jsNumber(array->length()));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPushTwo, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue first, EncodedJSValue second))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayPushTwo);
    std::array values { first, second };
    OPERATION_RETURN(scope, pushSeveralInPushOperations(instance, cellArgument<JSArray>(cell, Entry::operationAOTArrayPushTwo), values));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPushThree, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue first, EncodedJSValue second, EncodedJSValue third))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayPushThree);
    std::array values { first, second, third };
    OPERATION_RETURN(scope, pushSeveralInPushOperations(instance, cellArgument<JSArray>(cell, Entry::operationAOTArrayPushThree), values));
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPushFour, EncodedJSValue, (Instance* instance, JSCell* cell, EncodedJSValue first, EncodedJSValue second, EncodedJSValue third, EncodedJSValue fourth))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTArrayPushFour);
    std::array values { first, second, third, fourth };
    OPERATION_RETURN(scope, pushSeveralInPushOperations(instance, cellArgument<JSArray>(cell, Entry::operationAOTArrayPushFour), values));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
