/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "JSArrayInlines.h"
#include "JSCInlines.h"
#include <cmath>

namespace JSC { namespace AOT {

static EncodedJSValue encodeIndexKey(int64_t index)
{
    return JSValue::encode(jsNumber(static_cast<double>(index)));
}

static bool isLengthOfVector(JSArray* array, uint32_t index)
{
    IndexingType shape = array->indexingType() & IndexingShapeMask;
    return (shape == Int32Shape || shape == DoubleShape || shape == ContiguousShape) && index == array->butterfly()->publicLength();
}

static void putToArray(JSGlobalObject* globalObject, JSArray* array, EncodedJSValue encodedKey, EncodedJSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue key = JSValue::decode(encodedKey);
    if (std::optional<uint32_t> index = key.tryGetAsUint32Index()) {
        // A store at the length is a push.
        if (isLengthOfVector(array, *index) && pushToFullVector(vm, array, JSValue::decode(value)))
            return;
        scope.release();
        array->methodTable()->putByIndex(array, globalObject, *index, JSValue::decode(value), true);
        return;
    }
    auto name = key.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    PutPropertySlot slot(array, true);
    scope.release();
    array->methodTable()->put(array, globalObject, name, JSValue::decode(value), slot);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPutAtIndex, void, (Instance* instance, JSCell* array, EncodedJSValue value, uint64_t position))
{
    constexpr Entry entry = Entry::operationAOTArrayPutAtIndex;
    AOT_OPERATION_BEGIN(instance, entry);
    putToArray(globalObject, cellArgument<JSArray>(array, entry), encodeIndexKey(static_cast<int64_t>(position)), value);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPutNumberAtIndex, void, (Instance* instance, JSCell* cell, EncodedJSValue value, uint64_t position))
{
    constexpr Entry entry = Entry::operationAOTArrayPutNumberAtIndex;
    AOT_OPERATION_BEGIN(instance, entry);
    JSArray* array = cellArgument<JSArray>(cell, entry);
    if (JSValue number = JSValue::decode(value); number.isNumber() && !std::isnan(number.asNumber()) && hasInt32(array->indexingType()))
        array->tryMakeWritableDouble(vm);
    putToArray(globalObject, array, encodeIndexKey(static_cast<int64_t>(position)), value);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTArrayPutAtNumber, void, (Instance* instance, JSCell* array, EncodedJSValue value, double position))
{
    constexpr Entry entry = Entry::operationAOTArrayPutAtNumber;
    AOT_OPERATION_BEGIN(instance, entry);
    putToArray(globalObject, cellArgument<JSArray>(array, entry), JSValue::encode(jsNumber(purifyNaN(position))), value);
    OPERATION_RETURN(scope);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
