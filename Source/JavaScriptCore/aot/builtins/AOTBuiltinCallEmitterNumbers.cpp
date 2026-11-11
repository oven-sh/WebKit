/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinCallEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTOperatorEmitter.h"
#include "AOTIRProgram.h"
#include "AOTThrowEmitter.h"
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
            unsigned leadingZeros = std::countl_zero(std::max(static_cast<uint32_t>(dividends.maximum), constantDivisor));
            B3::DivisionMagic<uint32_t> magic = B3::computeUnsignedDivisionMagic(constantDivisor, leadingZeros);
            if (magic.add) {
                uint64_t multiplier = ((1ull << 32) | magic.magicMultiplier) << (31 - magic.shift);
                return Integer(m_out.uMulHigh(dividend.value(), m_out.constInt64(static_cast<int64_t>(multiplier))));
            }
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

static constexpr Range dividendsWithSignFolded { -(1ll << 55), 1ll << 55 };

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
    if (isDivisorConstant && divisors.minimum > 1 && !isNeverNegative && rounding != QuotientRounding::Truncate && dividends.isSubsetOf(dividendsWithSignFolded)) {
        if (roundsUp) {
            dividend = add(dividend, integer(divisors.minimum - 1));
            dividends = { dividends.minimum + divisors.minimum - 1, dividends.maximum + divisors.minimum - 1 };
        }
        Integer sign(m_out.aShr(dividend.value(), m_out.constInt32(63)));
        Range magnitudes { 0, std::max(dividends.maximum, -(dividends.minimum + 1)) };
        return bitXor(divide(bitXor(dividend, sign), magnitudes, divisor, divisors), sign);
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

void BuiltinCallEmitter::holdOperandsOfDivisionToToldRanges(const ValueContext& value, Integer dividend, Range dividends, Integer divisor, Range divisors)
{
    if (!emittersCheckProofs())
        return;
    auto liesIn = [&](Integer operand, Range told) {
        return belowOrEqual(sub(operand, integer(told.minimum)), integer(told.maximum - told.minimum));
    };
    emitter<ThrowEmitter>().requireOrCrash(value, logicalAnd(liesIn(dividend, dividends), liesIn(divisor, divisors)), std::nullopt, ProofSource::OtherOperation);
}

static ValueID integerBehindForms(const Code& code, ValueID id)
{
    static constexpr OpcodeSet formsOfInteger { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
    while (!code.isConstant(id) && formsOfInteger.contains(code.at(id).opcode))
        id = code.at(id).operand(0);
    return id;
}

BuiltinCallEmitter::OperandsOfDivision BuiltinCallEmitter::operandsOfDivision(ValueID dividend, ValueID divisor)
{
    const Code& code = generator().code();
    divisor = integerBehindForms(code, divisor);
    bool isConstant = code.at(divisor).opcode == Opcode::Integer;
    uint32_t first = (integerBehindForms(code, dividend).index() + 1) << 1 | isConstant;
    return { first, isConstant ? static_cast<uint64_t>(code.at(divisor).integer()) : divisor.index() };
}

void BuiltinCallEmitter::findDivisions()
{
    m_hasFoundDivisions = true;
    const Code& code = generator().code();
    for (uint32_t block = 0; block < code.numberOfBlocks(); ++block) {
        uint32_t place = 0;
        for (ValueID id : code.valuesOf(BlockID(block))) {
            ++place;
            const Operation& operation = code.at(id);
            if (operation.opcode == Opcode::Mod)
                m_placesOfRemainders.add(id.index(), place);
            if (operation.opcode != Opcode::DivideIntegers)
                continue;
            ValueID dividend = operation.dividendOperand();
            ValueID divisor = operation.divisorOperand();
            bool truncates = operation.rounding() == QuotientRounding::ToInt32 || operation.rounding() == QuotientRounding::Truncate
                || (operation.rounding() == QuotientRounding::Floor && typeOf(dividend).integers.minimum >= 0 && typeOf(divisor).integers.minimum > 0);
            if (truncates)
                m_divisionsByOperands.add(operandsOfDivision(dividend, divisor), Vector<PlacedDivision, 1>()).iterator->value.append({ id, block, place });
        }
    }
}

bool BuiltinCallEmitter::isEmittedWithBlock(BlockID block, BlockID later)
{
    const Code& code = generator().code();
    const Block& first = code.at(block);
    const Block& second = code.at(later);
    if (first.frequency != second.frequency || first.isInLoop != second.isInLoop || !generator().blockDominates(block, later))
        return false;
    if (!first.isInLoop)
        return true;
    BlockID head = second.terminator.kind == TerminatorKind::Jump ? code.at(second.terminator.next()).to : BlockID();
    if (!head || !code.at(head).isLoopHead)
        return false;
    static constexpr unsigned maxBlocksAfterHead = 8;
    BlockID above = block;
    for (unsigned step = 0; above && step < maxBlocksAfterHead; ++step, above = generator().nearestDominatorOf(above)) {
        if (code.at(above).isLoopHead)
            return above == head && above != block;
    }
    return false;
}

std::optional<BuiltinCallEmitter::DivisionOfSameOperands> BuiltinCallEmitter::findDivisionOfSameOperands(const ValueContext& remainder)
{
    if (!m_hasFoundDivisions)
        findDivisions();
    if (m_divisionsByOperands.isEmpty() || generator().keepsValuesInHeapFrame())
        return std::nullopt;
    auto found = m_divisionsByOperands.find(operandsOfDivision(remainder->leftOperand(), remainder->rightOperand()));
    if (found == m_divisionsByOperands.end())
        return std::nullopt;
    std::span<const PlacedDivision> divisions = found->value.span();
    uint32_t block = remainder.block().index();
    uint32_t place = m_placesOfRemainders.get(remainder.id().index());
    auto next = std::ranges::upper_bound(divisions, std::pair { block, place }, { }, [](const PlacedDivision& division) {
        return std::pair { division.block, division.place };
    });
    if (next != divisions.begin() && std::prev(next)->block == block) {
        ValueID ahead = std::prev(next)->division;
        const Operation& division = generator().code().at(ahead);
        bool isWholeQuotient = (division.representation == Representation::Int32 || division.representation == Representation::Int64) && (division.rounding() != QuotientRounding::ToInt32 || typeOf(remainder->leftOperand()).isInt32());
        if (isWholeQuotient && generator().isGenerated(ahead))
            return DivisionOfSameOperands { ahead, true };
    }
    if (next != divisions.end() && (next->block == block || isEmittedWithBlock(remainder.block(), BlockID(next->block))))
        return DivisionOfSameOperands { next->division, false };
    return std::nullopt;
}

void BuiltinCallEmitter::generateDivideIntegers(const ValueContext& value)
{
    Range dividends = typeOf(value->dividendOperand()).integers;
    Range divisors = typeOf(value->divisorOperand()).integers;
    QuotientRounding rounding = value->rounding();
    bool convertsToInt32 = rounding == QuotientRounding::ToInt32;
    bool isShift = rounding == QuotientRounding::Floor && divisors.isSingleValue() && hasOneBitSet(divisors.minimum);
    bool hasExactOperands = (isShift || dividends.isSubsetOf(Range::exact())) && divisors.isSubsetOf(Range::exact());
    if (!hasExactOperands || (divisors.contains(0) && !convertsToInt32)) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    Integer dividend = integerOperand(value->dividendOperand());
    Integer divisor = integerOperand(value->divisorOperand());
    holdOperandsOfDivisionToToldRanges(value, dividend, dividends, divisor, divisors);
    const Type& type = typeOf(value.id());
    if (LValue quotient = m_quotientsOfDivisions.take(value.id().index())) {
        if (convertsToInt32)
            defineAs(value, low32(Integer(quotient)), type);
        else
            defineAs(value, Integer(quotient), type);
        return;
    }
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

Integer BuiltinCallEmitter::remainderByConstant(Integer dividend, Range dividends, int64_t magnitudeOfDivisor)
{
    if (hasOneBitSet(magnitudeOfDivisor)) {
        if (dividends.minimum >= 0)
            return bitAnd(dividend, integer(magnitudeOfDivisor - 1));
        unsigned shift = WTF::fastLog2(static_cast<uint64_t>(magnitudeOfDivisor));
        Integer raised = add(dividend, Integer(m_out.lShr(m_out.aShr(dividend.value(), 63), 64 - shift)));
        return sub(dividend, bitAnd(raised, integer(-magnitudeOfDivisor)));
    }
    Integer divisor = integer(magnitudeOfDivisor);
    return sub(dividend, mul(divide(dividend, dividends, divisor, Range::one(magnitudeOfDivisor)), divisor));
}

void BuiltinCallEmitter::generateReciprocalOfDivisor(const ValueContext& value)
{
    Integer divisor = integerOperand(value->divisorOperand());
    if (AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime))
        emitter<ThrowEmitter>().requireOrCrash(value, logicalAnd(isNotZero(divisor), belowOrEqual(divisor, integer(INT32_MAX))), std::nullopt, ProofSource::OtherOperation);
    define(value, Pointer(m_out.add(m_out.uDiv(m_out.constInt64(-1), divisor.value()), m_out.constInt64(1))));
}

LValue BuiltinCallEmitter::lowHalfOfProductWithReciprocal(LValue reciprocal, LValue dividend)
{
    bool isSum = dividend->opcode() == Machine::Add && dividend->child(1)->hasInt64();
    LValue base = isSum ? dividend->child(0) : dividend;
    uint64_t offset = isSum ? dividend->child(1)->asInt64() : 0;
    if (base->opcode() == Machine::ZExt32)
        base = base->child(0);

    LowHalfOfRemainder& last = m_lowHalfOfLastRemainder;
    LValue lowHalf = last.lowHalf && last.reciprocal == reciprocal && last.base == base && m_out.findsAgain(last.lowHalf)
        ? m_out.add(last.lowHalf, m_out.mul(reciprocal, m_out.constInt64(offset - last.offset)))
        : m_out.mul(reciprocal, dividend);
    last = { reciprocal, base, offset, lowHalf };
    return lowHalf;
}

void BuiltinCallEmitter::generateRemainderByReciprocal(const ValueContext& value)
{
    ValueID dividendOperand = value->dividendOperand();
    Integer dividend = representationOf(dividendOperand) == Representation::Int32 ? emitter<ConversionEmitter>().widen(in<Word32>(dividendOperand), typeOf(dividendOperand).integers) : integerOperand(dividendOperand);
    Integer divisor = integerOperand(value->divisorOperand());
    bool checksProofs = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime);
    if (checksProofs)
        emitter<ThrowEmitter>().requireOrCrash(value, logicalAnd(belowOrEqual(dividend, integer(UINT32_MAX)), logicalAnd(isNotZero(divisor), belowOrEqual(divisor, integer(INT32_MAX)))), std::nullopt, ProofSource::OtherOperation);
    LValue lowHalf = lowHalfOfProductWithReciprocal(in<Pointer>(value->reciprocalOperand()).value(), dividend.value());
    Integer remainder(m_out.uMulHigh(lowHalf, divisor.value()));
    if (checksProofs)
        emitter<ThrowEmitter>().requireOrCrash(value, equal(remainder, Integer(m_out.mod(dividend.value(), divisor.value()))), std::nullopt, ProofSource::OtherOperation);
    defineAs(value, remainder, typeOf(value.id()));
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
    holdOperandsOfDivisionToToldRanges(value, dividend, dividends, divisor, divisors);
    const Type& type = typeOf(value.id());

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

void BuiltinCallEmitter::generateMultiplyAddIntegers(const ValueContext& value)
{
    const Type& type = typeOf(value.id());
    if (!type.isInteger() || type.integers == Range::full()) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    bool checksProofs = emittersCheckProofs();
    auto term = [&](ValueID operand) {
        Range told = typeOf(operand).integers;
        Integer integer = representationOf(operand) == Representation::Int32 ? emitter<ConversionEmitter>().widen(in<Word32>(operand), told) : integerOperand(operand);
        if (checksProofs)
            emitter<ThrowEmitter>().requireOrCrash(value, belowOrEqual(sub(integer, this->integer(told.minimum)), this->integer(told.maximum - told.minimum)), std::nullopt, ProofSource::OtherOperation);
        return integer;
    };
    Integer sum = mul(term(value->multiplicandOperand()), term(value->multiplierOperand()));
    sum = add(sum, term(value->addendOperand()));
    if (ValueID carry = value->carryOperand())
        sum = add(sum, term(carry));
    defineAs(value, sum, type);
}

void BuiltinCallEmitter::generateMultiplyIntegers(const ValueContext& value)
{
    const Type& type = typeOf(value.id());
    if (!type.isInteger() || !type.integers.isSubsetOf(Range::exact())) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }
    Integer multiplicand = integerOperand(value->multiplicandOperand());
    Integer multiplier = integerOperand(value->multiplierOperand());
    bool checksProofs = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::CheckProofsAtRunTime);
    auto checkProduct = [&](Integer product) {
        if (checksProofs)
            emitter<ThrowEmitter>().requireOrCrash(value, equal(mul(toNumber(multiplicand), toNumber(multiplier)), toNumber(product)), std::nullopt, ProofSource::OtherOperation);
    };
    if (type.isInt32()) {
        Word32 product = mul(low32(multiplicand), low32(multiplier));
        checkProduct(emitter<ConversionEmitter>().widen(product, Range::int32()));
        defineAs(value, product, type);
        return;
    }
    Integer product = mul(multiplicand, multiplier);
    checkProduct(product);
    defineAs(value, product, type);
}

Word32 BuiltinCallEmitter::int32BitsOf(ValueID operand, bool givesOnlyLowestBits)
{
    if (typeOf(operand).isInteger())
        return low32InWords(integerOperand(operand));
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    if (representationOf(operand) == Representation::JSValue)
        return conversions.toInt32OfBoxedNumber(in<Boxed>(operand));
    Number number = generator().holdsAnyNaN(operand) ? generator().getWithAnyNaN(operand).as<Number>() : numberOperand(operand);
    return conversions.toInt32(number, givesOnlyLowestBits ? ConversionEmitter::BitsOfInt32::Lowest : ConversionEmitter::BitsOfInt32::All);
}

void BuiltinCallEmitter::generateToInt32(const ValueContext& value)
{
    defineAs(value, int32BitsOf(value->valueOperand(), value->givesOnlyLowestBits()), m_program.at(TypeTable::int32));
}

void BuiltinCallEmitter::generateWholeDivisor(const ValueContext& value)
{
    Number divisor = numberOperand(value->divisorOperand());
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Integer whole = conversions.truncateUnchecked(divisor);
    Integer wholeOrZero = select(equal(conversions.bitsOf(toNumber(whole)), conversions.bitsOf(divisor)), whole, integer(0));
    defineAs(value, select(below(wholeOrZero, integer(maxExactInteger + 1)), wholeOrZero, integer(0)), typeOf(value.id()));
}

void BuiltinCallEmitter::generateRemainderByWholeDivisor(const ValueContext& value)
{
    Number remainder = emitter<OperatorEmitter>().remainderByWholeDivisor(value, numberOperand(value->dividendOperand()), numberOperand(value->divisorOperand()), integerOperand(value->wholeDivisorOperand()));
    defineAs(value, remainder, typeOf(value.id()));
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
