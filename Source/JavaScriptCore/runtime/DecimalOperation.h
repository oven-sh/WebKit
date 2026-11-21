/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/NativeFunction.h>
#include <array>
#include <compare>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

// The methods of Decimal.prototype that compilers know, by the nodes of the DFG.
#define FOR_EACH_DECIMAL_OPERATION(macro) \
    /* DecimalArith */ \
    macro(Add, add) \
    macro(Subtract, subtract) \
    macro(Multiply, multiply) \
    macro(Divide, divide) \
    macro(Remainder, remainder) \
    macro(Abs, abs) \
    macro(Negate, negate) \
    macro(Mantissa, mantissa) \
    /* DecimalScale */ \
    macro(Round, round) \
    macro(Scale10, scale10) \
    /* DecimalTest */ \
    macro(Equals, equals) \
    macro(NotEquals, notEquals) \
    macro(LessThan, lessThan) \
    macro(LessThanOrEqual, lessThanOrEqual) \
    macro(GreaterThan, greaterThan) \
    macro(GreaterThanOrEqual, greaterThanOrEqual) \
    macro(IsNaN, isNaN) \
    macro(IsFinite, isFinite) \
    /* DecimalToNumber */ \
    macro(Compare, compare) \
    macro(Exponent, exponent) \
    /* DecimalToString */ \
    macro(ToString, toString) \
    macro(ToExponential, toExponential) \
    /* DecimalToFixed */ \
    macro(ToFixed, toFixed)

enum class DecimalOperation : uint8_t {
#define DECLARE_DECIMAL_OPERATION(name, method) name,
    FOR_EACH_DECIMAL_OPERATION(DECLARE_DECIMAL_OPERATION)
#undef DECLARE_DECIMAL_OPERATION
};
static constexpr unsigned numberOfDecimalOperations = static_cast<unsigned>(DecimalOperation::ToFixed) + 1;

static constexpr std::array<ASCIILiteral, numberOfDecimalOperations> namesOfDecimalOperations {
#define NAME_DECIMAL_OPERATION(name, method) #method ## _s,
    FOR_EACH_DECIMAL_OPERATION(NAME_DECIMAL_OPERATION)
#undef NAME_DECIMAL_OPERATION
};

// Of a function of Decimal.prototype.
std::optional<DecimalOperation> decimalOperationOf(TaggedNativeFunction);

// See Decimal128::withoutTrailingZeros().
constexpr bool takesTrailingZeros(DecimalOperation operation)
{
    return operation <= DecimalOperation::Remainder || operation == DecimalOperation::Round || (operation >= DecimalOperation::LessThan && operation <= DecimalOperation::Exponent);
}

// The order of two values is -1, 0, 1 or this.
static constexpr int32_t unorderedDecimals = 2;
constexpr int32_t orderOfDecimals(std::partial_ordering order)
{
    return order == std::partial_ordering::unordered ? unorderedDecimals : (order > 0) - (order < 0);
}

} // namespace JSC
