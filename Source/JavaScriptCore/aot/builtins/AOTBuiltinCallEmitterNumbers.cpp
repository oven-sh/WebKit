/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

Integer BuiltinCallEmitter::divide(Integer dividend, Range dividends, Integer divisor, Range divisors)
{
    if (dividends.minimum >= 0 && divisors.minimum > 0 && dividends.isSubsetOf(Range::uint32()) && divisors.isSubsetOf(Range::uint32())) {
        Word32 narrowDividend = low32(dividend);
        return zeroExtended(Word32(m_out.m_block->appendNew<B3::Value>(m_proc, B3::UDiv, m_out.origin(), narrowDividend.value(), low32(divisor).value())));
    }
    if (divisors.isSingleValue() && hasOneBitSet(divisors.minimum)) {
        unsigned shift = WTF::fastLog2(static_cast<uint64_t>(divisors.minimum));
        if (!shift)
            return dividend;
        if (dividends.minimum < 0)
            dividend = add(dividend, Integer(m_out.lShr(m_out.aShr(dividend.value(), 63), 64 - shift)));
        return Integer(m_out.aShr(dividend.value(), m_out.constInt32(shift)));
    }
    if (dividends.isSubsetOf(Range::int32()) && divisors.isSubsetOf(Range::int32()) && (dividends.minimum > INT32_MIN || divisors.minimum > 0)) {
        Word32 narrowDividend = low32(dividend);
        return signExtended(Word32(m_out.div(narrowDividend.value(), low32(divisor).value())));
    }
    return Integer(m_out.div(dividend.value(), divisor.value()));
}

Integer BuiltinCallEmitter::roundedQuotient(QuotientRounding rounding, Integer dividend, Range dividends, Integer divisor, Range divisors)
{
    bool isDividendConstant = dividends.isSingleValue();
    bool isDivisorConstant = divisors.isSingleValue();
    bool isNeverNegative = dividends.minimum >= 0 && divisors.minimum > 0;
    if (rounding == QuotientRounding::Round) {
        dividend = add(isDividendConstant ? integer(2 * dividends.minimum) : add(dividend, dividend), divisor);
        divisor = isDivisorConstant ? integer(2 * divisors.minimum) : add(divisor, divisor);
        dividends = { 2 * dividends.minimum + divisors.minimum, 2 * dividends.maximum + divisors.maximum };
        divisors = { 2 * divisors.minimum, 2 * divisors.maximum };
        rounding = QuotientRounding::Floor;
    }
    bool roundsUp = rounding == QuotientRounding::Ceil;
    if (isDivisorConstant && hasOneBitSet(divisors.minimum) && rounding != QuotientRounding::Truncate) {
        if (roundsUp)
            dividend = add(dividend, integer(divisors.minimum - 1));
        return Integer(m_out.aShr(dividend.value(), m_out.constInt32(WTF::fastLog2(static_cast<uint64_t>(divisors.minimum)))));
    }
    if (roundsUp && isNeverNegative) {
        if (isDivisorConstant)
            dividend = add(dividend, integer(divisors.minimum - 1));
        else if (isDividendConstant)
            dividend = add(divisor, integer(dividends.minimum - 1));
        else
            dividend = add(dividend, sub(divisor, integer(1)));
        dividends.maximum += divisors.maximum - 1;
    }
    Integer truncated = divide(dividend, dividends, divisor, divisors);
    if (rounding == QuotientRounding::Truncate || isNeverNegative)
        return truncated;
    Integer remainder = sub(dividend, mul(truncated, divisor));
    Condition isNegative = lessThan(bitXor(remainder, divisor), 0);
    Condition isInexact = isNotZero(remainder);
    if (roundsUp)
        return add(truncated, zeroExtended(logicalAnd(isInexact, logicalNot(isNegative))));
    return sub(truncated, zeroExtended(logicalAnd(isInexact, isNegative)));
}

void BuiltinCallEmitter::generateDivideIntegers(const ValueContext& value)
{
    Range dividends = typeOf(value->dividendOperand()).integers;
    Range divisors = typeOf(value->divisorOperand()).integers;
    QuotientRounding rounding = value->rounding();
    bool convertsToInt32 = rounding == QuotientRounding::ToInt32;
    bool hasExactOperands = dividends.isSubsetOf(Range::exact()) && divisors.isSubsetOf(Range::exact());
    if (!hasExactOperands || (divisors.contains(0) && !convertsToInt32)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    Integer dividend = integerOperand(value->dividendOperand());
    Integer divisor = integerOperand(value->divisorOperand());
    const Type& type = typeOf(value.id());
    if (!convertsToInt32) {
        defineAs(value, roundedQuotient(rounding, dividend, dividends, divisor, divisors), type);
        return;
    }
    if (!divisors.contains(0)) {
        defineAs(value, low32(divide(dividend, dividends, divisor, divisors)), type);
        return;
    }
    if (dividends.isSubsetOf(Range::int32()) && divisors.isSubsetOf(Range::int32())) {
        Word32 narrowDividend = low32(dividend);
        defineAs(value, Word32(m_out.chillDiv(narrowDividend.value(), low32(divisor).value())), type);
        return;
    }
    defineAs(value, low32(Integer(m_out.chillDiv(dividend.value(), divisor.value()))), type);
}

void BuiltinCallEmitter::generateToInt32(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    Word32 bits = typeOf(operand).isInteger() ? low32(integerOperand(operand)) : emitter<ConversionEmitter>().toInt32(numberOperand(operand));
    defineAs(value, bits, Type::integer(Range::int32()));
}

void BuiltinCallEmitter::generateToUint32(const ValueContext& value)
{
    ValueID operand = value->valueOperand();
    Word32 bits = typeOf(operand).isInteger() ? low32(integerOperand(operand)) : emitter<ConversionEmitter>().toInt32(numberOperand(operand));
    defineAs(value, zeroExtended(bits), Type::integer(Range::uint32()));
}

void BuiltinCallEmitter::generateToIntegerOrInfinity(const ValueContext& value)
{
    Number operand = numberOperand(value->valueOperand());
    Number zero = number(0);
    Condition isNotNaN = equal(operand, operand);
    defineAs(value, select(isNotNaN, add(trunc(operand), zero), zero), typeOf(value.id()));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
