/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPrimitives.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeFunctions.h"
#include "AOTRuntimeTable.h"
#include "AOTState.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "Operations.h"
#include "TypeofType.h"

namespace JSC { namespace AOT {

#define AOT_DEFINE_BINARY_OPERATION(name, type, result) \
    JSC_DEFINE_JIT_OPERATION(name, type, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        OPERATION_RETURN(scope, result); \
    }

#define AOT_DEFINE_NUMERIC_BINARY_OPERATION(name, function, bigIntFunction) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        if (left.isHeapBigInt() && right.isHeapBigInt()) \
            OPERATION_RETURN(scope, JSValue::encode(JSBigInt::bigIntFunction(globalObject, left.asHeapBigInt(), right.asHeapBigInt()))); \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, left, right))); \
    }

#define AOT_DEFINE_COMPARISON_OPERATION(name, ofHeapBigInts, result) \
    JSC_DEFINE_JIT_OPERATION(name, size_t, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue left = JSValue::decode(encodedLeft); \
        JSValue right = JSValue::decode(encodedRight); \
        if (left.isHeapBigInt() && right.isHeapBigInt()) \
            OPERATION_RETURN(scope, JSBigInt::compare(left.asHeapBigInt(), right.asHeapBigInt()) ofHeapBigInts); \
        OPERATION_RETURN(scope, result); \
    }

#define AOT_DEFINE_UNARY_OPERATION(name, type, result) \
    JSC_DEFINE_JIT_OPERATION(name, type, (Instance* instance, EncodedJSValue encodedOperand)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        JSValue operand = JSValue::decode(encodedOperand); \
        OPERATION_RETURN(scope, result); \
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

AOT_DEFINE_NUMERIC_BINARY_OPERATION(operationAOTValueSub, jsSub, sub)
AOT_DEFINE_NUMERIC_BINARY_OPERATION(operationAOTValueMul, jsMul, multiply)
AOT_DEFINE_NUMERIC_BINARY_OPERATION(operationAOTValueDiv, jsDiv, divide)
AOT_DEFINE_NUMERIC_BINARY_OPERATION(operationAOTValueMod, jsRemainder, remainder)
AOT_DEFINE_BINARY_OPERATION(operationAOTValuePow, EncodedJSValue, JSValue::encode(jsPow(globalObject, left, right)))
AOT_DEFINE_BINARY_OPERATION(operationAOTValueBitAnd, EncodedJSValue, JSValue::encode(jsBitwiseAnd(globalObject, left, right)))
AOT_DEFINE_BINARY_OPERATION(operationAOTValueBitOr, EncodedJSValue, JSValue::encode(jsBitwiseOr(globalObject, left, right)))
AOT_DEFINE_BINARY_OPERATION(operationAOTValueBitXor, EncodedJSValue, JSValue::encode(jsBitwiseXor(globalObject, left, right)))
AOT_DEFINE_BINARY_OPERATION(operationAOTValueLShift, EncodedJSValue, JSValue::encode(jsLShift(globalObject, left, right)))
AOT_DEFINE_BINARY_OPERATION(operationAOTValueRShift, EncodedJSValue, JSValue::encode(jsRShift(globalObject, left, right)))
AOT_DEFINE_COMPARISON_OPERATION(operationAOTCompareLess, == JSBigInt::ComparisonResult::LessThan, jsLess<true>(globalObject, left, right))
AOT_DEFINE_COMPARISON_OPERATION(operationAOTCompareLessEq, != JSBigInt::ComparisonResult::GreaterThan, jsLessEq<true>(globalObject, left, right))
AOT_DEFINE_COMPARISON_OPERATION(operationAOTCompareGreater, == JSBigInt::ComparisonResult::GreaterThan, jsLess<false>(globalObject, right, left))
AOT_DEFINE_COMPARISON_OPERATION(operationAOTCompareGreaterEq, != JSBigInt::ComparisonResult::LessThan, jsLessEq<false>(globalObject, right, left))
AOT_DEFINE_BINARY_OPERATION(operationAOTCompareEq, size_t, JSValue::equal(globalObject, left, right))
AOT_DEFINE_BINARY_OPERATION(operationAOTCompareStrictEq, size_t, JSValue::strictEqual(globalObject, left, right))
AOT_DEFINE_UNARY_OPERATION(operationAOTValueBitNot, EncodedJSValue, JSValue::encode(jsBitwiseNot(globalObject, operand)))
AOT_DEFINE_UNARY_OPERATION(operationAOTToNumber, double, operand.toNumber(globalObject))
AOT_DEFINE_UNARY_OPERATION(operationAOTToNumeric, EncodedJSValue, JSValue::encode(operand.toNumeric(globalObject)))
AOT_DEFINE_UNARY_OPERATION(operationAOTToString, JSCell*, operand.toString(globalObject))
AOT_DEFINE_UNARY_OPERATION(operationAOTToKey, EncodedJSValue, JSValue::encode(operand.toPropertyKeyValue(globalObject)))
#undef AOT_DEFINE_BINARY_OPERATION
#undef AOT_DEFINE_NUMERIC_BINARY_OPERATION
#undef AOT_DEFINE_COMPARISON_OPERATION
#undef AOT_DEFINE_UNARY_OPERATION

JSC_DEFINE_JIT_OPERATION(operationAOTValueURShift, EncodedJSValue, (Instance* instance, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValueURShift);
    // jsURShift gives the 32 bits as an int32: bytecode has op_unsigned behind it.
    JSValue bits = jsURShift(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight));
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(static_cast<uint32_t>(bits.asInt32()))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareEqToString, size_t, (Instance* instance, EncodedJSValue encodedLeft, uint64_t text))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareEqToString);
    OPERATION_RETURN(scope, JSValue::equal(globalObject, JSValue::decode(encodedLeft), instance->state->strings->string(vm, StringID(static_cast<uint32_t>(text)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueNegate, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValueNegate);
    JSValue numeric = JSValue::decode(operand).toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::unaryMinus(globalObject, numeric.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(-numeric.asNumber())));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (Instance* instance, EncodedJSValue encodedOperand))
{
    return JSValue::decode(encodedOperand).toBoolean(instance->globalObject);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypeOf, JSCell*, (Instance* instance, EncodedJSValue operand))
{
    return jsTypeStringForValue(instance->globalObject, JSValue::decode(operand));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsCallable, size_t, (Instance*, JSCell* cell))
{
    return cell->isCallable();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFmod, double, (double a, double b))
{
    constexpr double limit = 4611686018427387904.0;
    if (std::abs(a) < limit) [[likely]] {
        int64_t dividend = static_cast<int64_t>(a);
        if (static_cast<double>(dividend) == a && std::abs(b) < limit) [[likely]] {
            int64_t divisor = static_cast<int64_t>(b);
            if (static_cast<double>(divisor) == b && divisor) [[likely]] {
                int64_t remainder = dividend % divisor;
                return remainder ? static_cast<double>(remainder) : a * 0.0;
            }
        }
    }
    return Math::fmodDouble(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPow, double, (double a, double b))
{
    return operationMathPow(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToInt32, size_t, (double value))
{
    return static_cast<uint32_t>(toInt32(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTryConvertToInteger, int64_t, (EncodedJSValue encodedValue))
{
    JSValue value = JSValue::decode(encodedValue);
    if (value.isInt32())
        return value.asInt32();
    if (!value.isDouble())
        return std::numeric_limits<int64_t>::min();
    double number = value.asDouble();
    if (!(number >= -static_cast<double>(Typed::maxTrackedInteger) && number <= static_cast<double>(Typed::maxTrackedInteger)))
        return std::numeric_limits<int64_t>::min();
    int64_t integer = static_cast<int64_t>(number);
    return static_cast<double>(integer) == number ? integer : std::numeric_limits<int64_t>::min();
}

#define AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(name, function) \
    JSC_DEFINE_JIT_OPERATION(name, double, (Instance* instance, EncodedJSValue value, uint32_t typeAndNumberSide, double number)) \
    { \
        AOT_OPERATION_BEGIN(instance, Entry::name); \
        auto bits = NumberAndValueBits::decode(typeAndNumberSide); \
        JSValue boxed = jsNumber(withoutPayload(number)); \
        JSValue other = JSValue::decode(value); \
        JSValue result = bits.numberIsOnRight ? function(globalObject, other, boxed) : function(globalObject, boxed, other); \
        OPERATION_RETURN_IF_EXCEPTION(scope, 0); \
        RELEASE_ASSERT(result.isNumber()); \
        OPERATION_RETURN(scope, result.asNumber()); \
    }
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTAddNumberAndValueAsNumber, jsAdd)
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTSubNumberAndValueAsNumber, jsSub)
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTMulNumberAndValueAsNumber, jsMul)
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTDivNumberAndValueAsNumber, jsDiv)
#undef AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
