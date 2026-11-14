/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(DFG_JIT)

#include "Decimal128.h"
#include "JITOperations.h"
#include "NativeFunction.h"
#include "UGPRPair.h"

namespace JSC {

// Optimized code computes with the two words of a Decimal128 in registers, and makes a DecimalObject of them only where one is seen.
enum class DecimalOperation : uint8_t {
    // DecimalArith
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Abs,
    Negate,
    Mantissa,
    // DecimalScale
    Round,
    Scale10,
    // DecimalTest
    Equals,
    NotEquals,
    LessThan,
    LessThanOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
    IsNaN,
    IsFinite,
    // DecimalToNumber
    Compare,
    Exponent,
    // DecimalToString
    ToString,
    ToExponential,
    // DecimalToFixed
    ToFixed,
};

// Of a function of Decimal.prototype.
std::optional<DecimalOperation> decimalOperationOf(TaggedNativeFunction);

// What a DecimalArith answers. A unary operation ignores y.
Decimal128 evaluate(DecimalOperation, Decimal128 x, Decimal128 y);

using BinaryDecimalOperation = UGPRPair(JIT_OPERATION_ATTRIBUTES*)(uint64_t, uint64_t, uint64_t, uint64_t);
// From Add to Remainder.
BinaryDecimalOperation binaryDecimalOperation(DecimalOperation);

// operationDecimalCompare answers -1, 0, 1 or this.
static constexpr int32_t unorderedDecimals = 2;

// These have no effects.
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationDecimalMantissa, UGPRPair, (uint64_t, uint64_t));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationDecimalRound, UGPRPair, (uint64_t, uint64_t, int32_t digits, uint32_t mode));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationDecimalScale10, UGPRPair, (uint64_t, uint64_t, int32_t power, uint32_t));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationDecimalCompare, UCPUStrictInt32, (uint64_t, uint64_t, uint64_t, uint64_t));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationDecimalExponent, EncodedJSValue, (uint64_t, uint64_t));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationInt32ToDecimal, UGPRPair, (int32_t));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationDoubleToDecimal, UGPRPair, (double));

JSC_DECLARE_JIT_OPERATION(operationNewDecimal, JSCell*, (VM*, Structure*, uint64_t, uint64_t));
JSC_DECLARE_JIT_OPERATION(operationDecimalToString, JSString*, (VM*, uint64_t, uint64_t));
JSC_DECLARE_JIT_OPERATION(operationDecimalToExponential, JSString*, (VM*, uint64_t, uint64_t));

// Where the function has an error to throw these answer the following upper word, which is that of no value, or null. Optimized code is
// left then, so that an error is the same in every tier.
static constexpr int32_t upperOfNoDecimal = -1;
JSC_DECLARE_JIT_OPERATION(operationToDecimal, UGPRPair, (JSGlobalObject*, EncodedJSValue));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationDecimalRoundByValue, UGPRPair, (uint64_t, uint64_t, EncodedJSValue digits, uint32_t mode));
JSC_DECLARE_NOEXCEPT_JIT_OPERATION(operationDecimalScale10ByValue, UGPRPair, (uint64_t, uint64_t, EncodedJSValue power, uint32_t));
JSC_DECLARE_JIT_OPERATION(operationDecimalToFixed, JSString*, (JSGlobalObject*, uint64_t, uint64_t, EncodedJSValue digits, EncodedJSValue roundingMode));

} // namespace JSC

namespace WTF {

void printInternal(PrintStream&, JSC::DecimalOperation);

} // namespace WTF

#endif // ENABLE(DFG_JIT)
