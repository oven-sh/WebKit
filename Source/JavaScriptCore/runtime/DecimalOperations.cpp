/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "DecimalOperations.h"

#if ENABLE(DFG_JIT)

#include "DecimalConstructor.h"
#include "DecimalObject.h"
#include "DecimalPrototype.h"
#include "DecimalStringCache.h"
#include "FrameTracers.h"
#include "JSCInlines.h"

IGNORE_WARNINGS_BEGIN("frame-address")

namespace JSC {

static ALWAYS_INLINE UGPRPair inRegisters(Decimal128 value)
{
    auto [low, upper] = value.words();
    return makeUGPRPair(low, upper);
}

static constexpr UGPRPair noDecimal = makeUGPRPair(0, static_cast<int64_t>(upperOfNoDecimal));

#define FOR_EACH_BINARY_DECIMAL_OPERATION(macro) \
    macro(Add, add) \
    macro(Subtract, subtract) \
    macro(Multiply, multiply) \
    macro(Divide, divide) \
    macro(Remainder, remainder)

#define DEFINE_BINARY_DECIMAL_OPERATION(name, function) \
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationDecimal##name, UGPRPair, (uint64_t, uint64_t, uint64_t, uint64_t)); \
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationDecimal##name, UGPRPair, (uint64_t xLow, uint64_t xUpper, uint64_t yLow, uint64_t yUpper)) \
{ \
    return inRegisters(Decimal128::function(Decimal128::fromWords(xLow, xUpper), Decimal128::fromWords(yLow, yUpper))); \
}
FOR_EACH_BINARY_DECIMAL_OPERATION(DEFINE_BINARY_DECIMAL_OPERATION)
#undef DEFINE_BINARY_DECIMAL_OPERATION

BinaryDecimalOperation binaryDecimalOperation(DecimalOperation operation)
{
    switch (operation) {
#define CASE(name, function) \
    case DecimalOperation::name: \
        return operationDecimal##name;
    FOR_EACH_BINARY_DECIMAL_OPERATION(CASE)
#undef CASE
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Decimal128 evaluate(DecimalOperation operation, Decimal128 x, Decimal128 y)
{
    switch (operation) {
#define CASE(name, function) \
    case DecimalOperation::name: \
        return Decimal128::function(x, y);
    FOR_EACH_BINARY_DECIMAL_OPERATION(CASE)
#undef CASE
    case DecimalOperation::Abs:
        return x.abs();
    case DecimalOperation::Negate:
        return x.negate();
    case DecimalOperation::Mantissa:
        return x.mantissa();
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationDecimalMantissa, UGPRPair, (uint64_t low, uint64_t upper))
{
    return inRegisters(Decimal128::fromWords(low, upper).mantissa());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationDecimalRound, UGPRPair, (uint64_t low, uint64_t upper, int32_t digits, uint32_t mode))
{
    return inRegisters(Decimal128::fromWords(low, upper).round(digits, static_cast<Decimal128::RoundingMode>(mode)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationDecimalScale10, UGPRPair, (uint64_t low, uint64_t upper, int32_t power, uint32_t))
{
    return inRegisters(Decimal128::fromWords(low, upper).scale10(power));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationDecimalCompare, UCPUStrictInt32, (uint64_t xLow, uint64_t xUpper, uint64_t yLow, uint64_t yUpper))
{
    auto order = Decimal128::compare(Decimal128::fromWords(xLow, xUpper), Decimal128::fromWords(yLow, yUpper));
    return toUCPUStrictInt32(order == std::partial_ordering::unordered ? unorderedDecimals : (order > 0) - (order < 0));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationDecimalExponent, EncodedJSValue, (uint64_t low, uint64_t upper))
{
    auto value = Decimal128::fromWords(low, upper);
    return JSValue::encode(value.isFinite() && !value.isZero() ? jsNumber(value.scientificExponent()) : jsDoubleNumber(value.exponent()));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationInt32ToDecimal, UGPRPair, (int32_t value))
{
    return inRegisters(Decimal128::fromInteger(value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationDoubleToDecimal, UGPRPair, (double value))
{
    return inRegisters(Decimal128::fromDouble(value));
}

JSC_DEFINE_JIT_OPERATION(operationToDecimal, UGPRPair, (JSGlobalObject* globalObject, EncodedJSValue value))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto result = tryToDecimal128(globalObject, JSValue::decode(value));
    OPERATION_RETURN(scope, result ? inRegisters(*result) : noDecimal);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationDecimalRoundByValue, UGPRPair, (uint64_t low, uint64_t upper, EncodedJSValue encodedDigits, uint32_t mode))
{
    JSValue digits = JSValue::decode(encodedDigits);
    if (!digits.isNumber() || !isInteger(digits.asNumber())) [[unlikely]]
        return noDecimal;
    return inRegisters(Decimal128::fromWords(low, upper).round(digits.asNumber(), static_cast<Decimal128::RoundingMode>(mode)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationDecimalScale10ByValue, UGPRPair, (uint64_t low, uint64_t upper, EncodedJSValue encodedPower, uint32_t))
{
    JSValue power = JSValue::decode(encodedPower);
    if (!power.isNumber() || (std::isfinite(power.asNumber()) && !isInteger(power.asNumber()))) [[unlikely]]
        return noDecimal;
    return inRegisters(Decimal128::fromWords(low, upper).scale10(power.asNumber()));
}

JSC_DEFINE_JIT_OPERATION(operationNewDecimal, JSCell*, (VM* vmPointer, Structure* structure, uint64_t low, uint64_t upper))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    OPERATION_RETURN(scope, DecimalObject::create(vm, structure, Decimal128::fromWords(low, upper)));
}

JSC_DEFINE_JIT_OPERATION(operationDecimalToString, JSString*, (VM* vmPointer, uint64_t low, uint64_t upper))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    OPERATION_RETURN(scope, vm.decimalStringCache().toString(vm, Decimal128::fromWords(low, upper)));
}

JSC_DEFINE_JIT_OPERATION(operationDecimalToExponential, JSString*, (VM* vmPointer, uint64_t low, uint64_t upper))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    OPERATION_RETURN(scope, vm.decimalStringCache().toExponential(vm, Decimal128::fromWords(low, upper)));
}

JSC_DEFINE_JIT_OPERATION(operationDecimalToFixed, JSString*, (JSGlobalObject* globalObject, uint64_t low, uint64_t upper, EncodedJSValue digits, EncodedJSValue roundingMode))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    OPERATION_RETURN(scope, tryDecimalToFixed(globalObject, Decimal128::fromWords(low, upper), JSValue::decode(digits), JSValue::decode(roundingMode)));
}

} // namespace JSC

IGNORE_WARNINGS_END

namespace WTF {

void printInternal(PrintStream& out, JSC::DecimalOperation operation)
{
    out.print(enumName(operation));
}

} // namespace WTF

#endif // ENABLE(DFG_JIT)
