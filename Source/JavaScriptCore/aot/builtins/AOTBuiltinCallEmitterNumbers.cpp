/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTIRProgram.h"
#include "AOTTypeTable.h"
#include "B3ComputeDivisionMagic.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

Integer BuiltinCallEmitter::divide(Integer dividend, Range dividends, Integer divisor, Range divisors)
{
    if (dividends.minimum >= 0 && divisors.minimum > 0 && dividends.isSubsetOf(Range::uint32()) && divisors.isSubsetOf(Range::uint32())) {
        if (divisors.isSingleValue() && !hasOneBitSet(divisors.minimum)) {
            uint32_t constantDivisor = static_cast<uint32_t>(divisors.minimum);
            // computeUnsignedDivisionMagic() takes no more leading zeros than the divisor has.
            unsigned leadingZeros = std::countl_zero(std::max(static_cast<uint32_t>(dividends.maximum), constantDivisor));
            B3::DivisionMagic<uint32_t> magic = B3::computeUnsignedDivisionMagic(constantDivisor, leadingZeros);
            if (magic.add) {
                // The multiplier has 33 bits. With it moved to the top of the word, the high half of the product is the quotient.
                uint64_t multiplier = ((1ull << 32) | magic.magicMultiplier) << (31 - magic.shift);
                return Integer(m_out.uMulHigh(dividend.value(), m_out.constInt64(static_cast<int64_t>(multiplier))));
            }
            // Both factors are below 2^32, so the product fits 64 bits.
            Integer shifted = magic.preShift ? Integer(m_out.lShr(dividend.value(), magic.preShift)) : dividend;
            return Integer(m_out.lShr(mul(shifted, integer(magic.magicMultiplier)).value(), 32 + magic.shift));
        }
        Word32 narrowDividend = low32(dividend);
        return zeroExtended(Word32(m_out.uDiv(narrowDividend.value(), low32(divisor).value())));
    }
    if (divisors.isSingleValue() && hasOneBitSet(divisors.minimum)) {
        unsigned shift = WTF::fastLog2(static_cast<uint64_t>(divisors.minimum));
        if (!shift)
            return dividend;
        if (dividends.minimum < 0 && dividends.isSubsetOf(Range::int32()) && shift < 32) {
            Word32 narrowDividend = low32(dividend);
            Word32 roundedTowardZero = isX86()
                ? select(lessThan(narrowDividend, 0), add(narrowDividend, word32(static_cast<int32_t>(divisors.minimum - 1))), narrowDividend)
                : add(narrowDividend, Word32(m_out.lShr(m_out.aShr(narrowDividend.value(), 31), 32 - shift)));
            return signExtended(Word32(m_out.aShr(roundedTowardZero.value(), m_out.constInt32(shift))));
        }
        if (dividends.minimum < 0)
            dividend = add(dividend, Integer(m_out.lShr(m_out.aShr(dividend.value(), 63), 64 - shift)));
        return Integer(m_out.aShr(dividend.value(), m_out.constInt32(shift)));
    }
    if (dividends.isSubsetOf(Range::int32()) && divisors.isSubsetOf(Range::int32()) && (dividends.minimum > INT32_MIN || divisors.minimum > 0)) {
        if (divisors.isSingleValue() && (divisors.minimum > 1 || (divisors.minimum < -1 && divisors.minimum > INT32_MIN))) {
            B3::DivisionMagic<int32_t> magic = B3::computeSignedDivisionMagic(static_cast<int32_t>(divisors.minimum));
            // The multiplier is a signed number of 33 bits, handed over cut to 32. Whole, it needs none of the corrections of the 32-bit sequence, and its product with an int32 fits 64 bits.
            int64_t multiplier = static_cast<uint32_t>(magic.magicMultiplier);
            if (divisors.minimum < 0)
                multiplier -= 1ll << 32;
            Integer product = mul(dividend, integer(multiplier));
            Integer roundedDown(m_out.aShr(product.value(), 32 + magic.shift));
            return add(roundedDown, Integer(m_out.lShr(product.value(), 63)));
        }
        Word32 narrowDividend = low32(dividend);
        return signExtended(Word32(m_out.div(narrowDividend.value(), low32(divisor).value())));
    }
    if (divisors.isSingleValue() && (divisors.minimum > 1 || divisors.minimum < -1)) {
        // Here the multiplier is a signed number of 65 bits cut to 64. Adding or subtracting the dividend makes up for the bit that was cut.
        B3::DivisionMagic<int64_t> magic = B3::computeSignedDivisionMagic(divisors.minimum);
        Integer quotient(m_out.mulHigh(dividend.value(), m_out.constInt64(magic.magicMultiplier)));
        if (divisors.minimum > 0 && magic.magicMultiplier < 0)
            quotient = add(quotient, dividend);
        if (divisors.minimum < 0 && magic.magicMultiplier > 0)
            quotient = sub(quotient, dividend);
        if (magic.shift)
            quotient = Integer(m_out.aShr(quotient.value(), m_out.constInt32(magic.shift)));
        if (dividends.minimum >= 0 && divisors.minimum > 0)
            return quotient;
        return add(quotient, Integer(m_out.lShr(quotient.value(), 63)));
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
        Word32 narrowDivisor = low32(divisor);
        if (!isX86()) {
            defineAs(value, Word32(m_out.chillDiv(narrowDividend.value(), narrowDivisor.value())), type);
            return;
        }
        bool mayOverflow = dividends.minimum == INT32_MIN && divisors.contains(-1);
        Condition isOrdinary = mayOverflow ? above(add(narrowDivisor, word32(1)), word32(1)) : isNotZero(narrowDivisor);
        defineAs(value, either<Word32>(isOrdinary, [&] {
            return Word32(m_out.div(narrowDividend.value(), narrowDivisor.value()));
        }, [&] {
            return mayOverflow ? select(isNotZero(narrowDivisor), neg(narrowDividend), word32(0)) : word32(0);
        }), type);
        return;
    }
    defineAs(value, low32(Integer(m_out.chillDiv(dividend.value(), divisor.value()))), type);
}

// Takes a divisor above one. A remainder has the sign of its dividend, whatever the sign of the divisor.
Integer BuiltinCallEmitter::remainderByConstant(Integer dividend, Range dividends, int64_t magnitudeOfDivisor)
{
    if (hasOneBitSet(magnitudeOfDivisor)) {
        if (dividends.minimum >= 0)
            return bitAnd(dividend, integer(magnitudeOfDivisor - 1));
        // The quotient is rounded toward zero: a dividend below zero is raised by the divisor less one before its low bits are dropped.
        unsigned shift = WTF::fastLog2(static_cast<uint64_t>(magnitudeOfDivisor));
        Integer raised = add(dividend, Integer(m_out.lShr(m_out.aShr(dividend.value(), 63), 64 - shift)));
        return sub(dividend, bitAnd(raised, integer(-magnitudeOfDivisor)));
    }
    Integer divisor = integer(magnitudeOfDivisor);
    return sub(dividend, mul(divide(dividend, dividends, divisor, Range::one(magnitudeOfDivisor)), divisor));
}

void BuiltinCallEmitter::generateRemainderOfIntegers(const ValueContext& value)
{
    Range dividends = typeOf(value->dividendOperand()).integers;
    Range divisors = typeOf(value->divisorOperand()).integers;
    if (!dividends.isSubsetOf(Range::exact()) || !divisors.isSubsetOf(Range::exact())) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    Integer dividend = integerOperand(value->dividendOperand());
    Integer divisor = integerOperand(value->divisorOperand());
    const Type& type = typeOf(value.id());

    // Output::mod() takes no divisor that is zero and no quotient that does not fit, and what is appended to a block that never runs is folded like anything else.
    std::optional<int64_t> constantDivisor;
    if (divisors.isSingleValue())
        constantDivisor = divisors.minimum;
    else if (divisor.value()->hasInt())
        constantDivisor = divisor.value()->asInt();
    if (constantDivisor) {
        int64_t magnitude = std::abs(*constantDivisor);
        defineAs(value, magnitude > 1 ? remainderByConstant(dividend, dividends, magnitude) : integer(0), type);
        return;
    }

    bool divisorMayBeZero = divisors.contains(0);
    if (dividends.isSubsetOf(Range::int32()) && divisors.isSubsetOf(Range::int32())) {
        Word32 narrowDividend = low32(dividend);
        Word32 narrowDivisor = low32(divisor);
        bool mayOverflow = dividends.minimum == INT32_MIN && divisors.contains(-1);
        auto remainder = [&] {
            return Word32(m_out.mod(narrowDividend.value(), narrowDivisor.value()));
        };
        if (!divisorMayBeZero && !mayOverflow) {
            defineAs(value, remainder(), type);
            return;
        }
        // What -1 divides leaves nothing.
        Condition isOrdinary = mayOverflow ? above(add(narrowDivisor, word32(1)), word32(1)) : isNotZero(narrowDivisor);
        defineAs(value, either<Word32>(isOrdinary, remainder, [&] {
            return word32(0);
        }), type);
        return;
    }

    auto remainder = [&] {
        return Integer(m_out.mod(dividend.value(), divisor.value()));
    };
    if (!divisorMayBeZero) {
        defineAs(value, remainder(), type);
        return;
    }
    defineAs(value, either<Integer>(isNotZero(divisor), remainder, [&] {
        return integer(0);
    }), type);
}

Word32 BuiltinCallEmitter::int32BitsOf(ValueID operand, bool givesOnlyLowestBits)
{
    if (typeOf(operand).isInteger())
        return low32InWords(integerOperand(operand));
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    if (representationOf(operand) == Representation::JSValue)
        return conversions.toInt32OfBoxedNumber(in<Boxed>(operand));
    // Every NaN is 0.
    Number number = generator().holdsAnyNaN(operand) ? generator().getWithAnyNaN(operand).as<Number>() : numberOperand(operand);
    return conversions.toInt32(number, givesOnlyLowestBits ? ConversionEmitter::BitsOfInt32::Lowest : ConversionEmitter::BitsOfInt32::All);
}

void BuiltinCallEmitter::generateToInt32(const ValueContext& value)
{
    defineAs(value, int32BitsOf(value->valueOperand(), value->givesOnlyLowestBits()), m_program.at(TypeTable::int32));
}

void BuiltinCallEmitter::generateToUint32(const ValueContext& value)
{
    defineAs(value, zeroExtended(int32BitsOf(value->valueOperand(), false)), m_program.at(TypeTable::uint32));
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
