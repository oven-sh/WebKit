/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlowPaths.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTState.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "NumberPrototype.h"
#include "Operations.h"

namespace JSC { namespace AOT {

#define AOT_BINARY_OPERATION(name, function) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)))); \
    }

#define AOT_NUMERIC_BINARY_OPERATION(name, function, bigIntFunction) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        if (left.isHeapBigInt() && right.isHeapBigInt()) \
            OPERATION_RETURN(scope, JSValue::encode(JSBigInt::bigIntFunction(globalObject, left.asHeapBigInt(), right.asHeapBigInt()))); \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, left, right))); \
    }

JSC_DEFINE_JIT_OPERATION(operationAOTValueAdd, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValueAdd);
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    if (left.isHeapBigInt() && right.isHeapBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::add(globalObject, left.asHeapBigInt(), right.asHeapBigInt())));
    if (left.isString() && right.isInt32())
        OPERATION_RETURN(scope, JSValue::encode(jsString(globalObject, asString(left), int32ToString(vm, right.asInt32(), 10))));
    if (left.isInt32() && right.isString())
        OPERATION_RETURN(scope, JSValue::encode(jsString(globalObject, int32ToString(vm, left.asInt32(), 10), asString(right))));
    OPERATION_RETURN(scope, JSValue::encode(jsAdd(globalObject, left, right)));
}

AOT_NUMERIC_BINARY_OPERATION(operationAOTValueSub, jsSub, sub)
AOT_NUMERIC_BINARY_OPERATION(operationAOTValueMul, jsMul, multiply)
AOT_NUMERIC_BINARY_OPERATION(operationAOTValueDiv, jsDiv, divide)
AOT_NUMERIC_BINARY_OPERATION(operationAOTValueMod, jsRemainder, remainder)
AOT_BINARY_OPERATION(operationAOTValueBitAnd, jsBitwiseAnd)
AOT_BINARY_OPERATION(operationAOTValueBitOr, jsBitwiseOr)
AOT_BINARY_OPERATION(operationAOTValueBitXor, jsBitwiseXor)
AOT_BINARY_OPERATION(operationAOTValueLShift, jsLShift)
AOT_BINARY_OPERATION(operationAOTValueRShift, jsRShift)

JSC_DEFINE_JIT_OPERATION(operationAOTValueURShift, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValueURShift);
    // jsURShift gives the 32 bits as an int32: bytecode has op_unsigned behind it.
    JSValue bits = jsURShift(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(static_cast<uint32_t>(bits.asInt32()))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (Instance* instance, EncodedJSValue encodedOperand))
{
    JSGlobalObject* globalObject = instance->globalObject;
    return JSValue::decode(encodedOperand).toBoolean(globalObject);
}

static ALWAYS_INLINE std::optional<JSBigInt::ComparisonResult> tryCompareHeapBigInts(EncodedJSValue encodedLeft, EncodedJSValue encodedRight)
{
    JSValue left = JSValue::decode(encodedLeft);
    JSValue right = JSValue::decode(encodedRight);
    if (!left.isHeapBigInt() || !right.isHeapBigInt())
        return std::nullopt;
    return JSBigInt::compare(left.asHeapBigInt(), right.asHeapBigInt());
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLess, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareLess);
    if (auto comparison = tryCompareHeapBigInts(encodedLeft, encodedRight))
        OPERATION_RETURN(scope, *comparison == JSBigInt::ComparisonResult::LessThan);
    OPERATION_RETURN(scope, jsLess<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLessEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareLessEq);
    if (auto comparison = tryCompareHeapBigInts(encodedLeft, encodedRight))
        OPERATION_RETURN(scope, *comparison != JSBigInt::ComparisonResult::GreaterThan);
    OPERATION_RETURN(scope, jsLessEq<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreater, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareGreater);
    if (auto comparison = tryCompareHeapBigInts(encodedLeft, encodedRight))
        OPERATION_RETURN(scope, *comparison == JSBigInt::ComparisonResult::GreaterThan);
    OPERATION_RETURN(scope, jsLess<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreaterEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareGreaterEq);
    if (auto comparison = tryCompareHeapBigInts(encodedLeft, encodedRight))
        OPERATION_RETURN(scope, *comparison != JSBigInt::ComparisonResult::LessThan);
    OPERATION_RETURN(scope, jsLessEq<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareEq);
    OPERATION_RETURN(scope, JSValue::equal(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareEqToString, size_t, (Instance* instance, EncodedJSValue encodedLeft, uint64_t text))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareEqToString);
    OPERATION_RETURN(scope, JSValue::equal(globalObject, JSValue::decode(encodedLeft), instance->state->strings->string(vm, StringID(static_cast<uint32_t>(text)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareStrictEq, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareStrictEq);
    OPERATION_RETURN(scope, JSValue::strictEqual(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
