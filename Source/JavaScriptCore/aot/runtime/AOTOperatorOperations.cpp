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
#include "AOTRuntimeFunctions.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "MathCommon.h"
#include "Operations.h"
#include "TypeofType.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTValuePow, EncodedJSValue, (Instance* instance, EncodedJSValue left, EncodedJSValue right))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValuePow);
    OPERATION_RETURN(scope, JSValue::encode(jsPow(globalObject, JSValue::decode(left), JSValue::decode(right))));
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

JSC_DEFINE_JIT_OPERATION(operationAOTValueBitNot, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTValueBitNot);
    OPERATION_RETURN(scope, JSValue::encode(jsBitwiseNot(globalObject, JSValue::decode(operand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToNumber, double, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTToNumber);
    OPERATION_RETURN(scope, JSValue::decode(operand).toNumber(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToNumeric, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTToNumeric);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(operand).toNumeric(globalObject)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypeOf, JSCell*, (Instance* instance, EncodedJSValue operand))
{
    return jsTypeStringForValue(instance->globalObject, JSValue::decode(operand));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsCallable, size_t, (Instance*, JSCell* cell))
{
    return cell->isCallable();
}

JSC_DEFINE_JIT_OPERATION(operationAOTToString, JSCell*, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTToString);
    OPERATION_RETURN(scope, JSValue::decode(operand).toString(globalObject));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToKey, EncodedJSValue, (Instance* instance, EncodedJSValue operand))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTToKey);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(operand).toPropertyKeyValue(globalObject)));
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
        JSValue boxed = jsNumber(purifyNaN(number)); \
        JSValue other = JSValue::decode(value); \
        JSValue result = bits.numberIsOnRight ? function(globalObject, other, boxed) : function(globalObject, boxed, other); \
        OPERATION_RETURN_IF_EXCEPTION(scope, 0); \
        if (!result.isNumber()) [[unlikely]] { \
            throwTypeMismatch(instance, globalObject, scope, result, bits.type); \
            OPERATION_RETURN(scope, 0); \
        } \
        OPERATION_RETURN(scope, result.asNumber()); \
    }
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTAddNumberAndValueAsNumber, jsAdd)
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTSubNumberAndValueAsNumber, jsSub)
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTMulNumberAndValueAsNumber, jsMul)
AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE(operationAOTDivNumberAndValueAsNumber, jsDiv)
#undef AOT_DEFINE_BINARY_ON_NUMBER_AND_VALUE

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
