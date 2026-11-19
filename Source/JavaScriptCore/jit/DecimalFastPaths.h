/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Decimal128.h"
#include <wtf/StdLibExtras.h>

namespace JSC {

// The usual cases of the operations on Decimals, for the compilers that build SSA: coefficients of 64 bits. Each goes on in the slow case
// with anything else.
//
// The generator has the types Value and Block, and
//     out()                        the builder of operations
//     int64Type()
//     newBlock(), appendTo(block)
//     continueIf(condition, otherwise), which is rare
//     branch(condition, taken, notTaken), of which neither is
//     addressInTables(offset, index), the sum of these and the address of Decimal128::tables()
//     loadFromTables(address, offset)
//     countLeadingZeros64(value)
template<typename Generator>
class DecimalFastPaths {
public:
    using Value = typename Generator::Value;
    using Block = typename Generator::Block;
    using RoundingMode = Decimal128::RoundingMode;

    struct Words {
        Value low;
        Value upper;
    };

    // Whether a result has zeros at its end depends on the data, so no processor can guess it. What only goes into another operation keeps
    // them. See Decimal128::withoutTrailingZeros().
    enum class TrailingZeros : bool { None, Some };

    explicit DecimalFastPaths(Generator& generator)
        : m_generator(generator)
    {
    }

    Words add(Words x, Words y, TrailingZeros result, Block slowCase) { return addOrSubtract(x, y, false, result, slowCase); }
    Words subtract(Words x, Words y, TrailingZeros result, Block slowCase) { return addOrSubtract(x, y, true, result, slowCase); }

    // A total that keeps its trailing zeros soon has the least exponent, and it seldom changes its sign. What comes next then waits for
    // one addition only.
    Words addToTotal(Words total, Words x, bool subtracts, Block slowCase)
    {
        Block otherCase = m_generator.newBlock();
        Block continuation = m_generator.newBlock();

        Aligned aligned = align(total, x, slowCase);
        Value count = countOfOther(total, x, aligned);
        continueIf(out().lessThanOrEqual(unitOf(total.upper), unitOf(x.upper)), otherCase);
        Value sum = subtracts ? out().sub(total.low, count) : out().add(total.low, count);
        continueIf(out().greaterThan(sum, constant(0)), otherCase);
        auto usualLow = out().anchor(sum);
        auto usualUpper = out().anchor(total.upper);
        out().jump(continuation);

        m_generator.appendTo(otherCase);
        Words other = addOrSubtract(total, x, aligned, count, subtracts, TrailingZeros::Some, slowCase);
        auto otherLow = out().anchor(other.low);
        auto otherUpper = out().anchor(other.upper);
        out().jump(continuation);

        m_generator.appendTo(continuation);
        return { phi(usualLow, otherLow), phi(usualUpper, otherUpper) };
    }

    Words multiply(Words x, Words y, TrailingZeros result, Block slowCase)
    {
        if (x.low->hasInt64())
            std::swap(x, y);
        if (y.low->hasInt64() && y.low->asInt64()) {
            continueIf(out().isZero64(highOfEither(x, y)), slowCase);
            continueIf(out().belowOrEqual(x.low, constant(std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(y.low->asInt64()))), slowCase);
        } else
            continueIf(out().isZero64(out().bitOr(highOfEither(x, y), out().uMulHigh(x.low, y.low))), slowCase);
        Value exponent = out().add(exponentOf(x.upper), exponentOf(y.upper));
        continueIf(isQuantumExponent(exponent), slowCase);
        // The coefficient of NaN and infinity is 0.
        return finish(out().mul(x.low, y.low), shl(exponent, Decimal128::shiftOfUnit), signOfProduct(x, y), result, slowCase);
    }

    // What to multiply by in place of a division by a constant.
    std::optional<Words> reciprocalOf(Words x)
    {
        if (!x.low->hasInt64() || !x.upper->hasInt64())
            return std::nullopt;
        auto reciprocal = Decimal128::fromWords(x.low->asInt64(), x.upper->asInt64()).exactReciprocal();
        if (!reciprocal)
            return std::nullopt;
        return Words { constant(reciprocal->words()[0]), constant(reciprocal->words()[1]) };
    }

    // The count of digits has 32 bits.
    Words round(Words x, TrailingZeros operand, Value digits, RoundingMode mode, TrailingZeros result, Block slowCase)
    {
        Block hasMoreDigits = m_generator.newBlock();
        Block hasNoMoreDigits = m_generator.newBlock();
        Block continuation = m_generator.newBlock();

        bool mayBeExact = operand == TrailingZeros::Some;
        if (mayBeExact)
            continueIf(out().isZero64(highOf(x.upper)), slowCase);
        // NaN and infinity have none. 0 seems to have some if the count is negative.
        Value exponent = out().neg(out().signExt32To64(digits));
        Value power = out().sub(exponent, exponentOf(x.upper));
        m_generator.branch(out().greaterThan(power, constant(0)), hasMoreDigits, hasNoMoreDigits);

        m_generator.appendTo(hasNoMoreDigits);
        bool finishesBoth = mayBeExact && result == TrailingZeros::None;
        auto sameLow = out().anchor(x.low);
        auto sameUpperOrUnit = out().anchor(finishesBoth ? unitOf(x.upper) : x.upper);
        out().jump(continuation);

        m_generator.appendTo(hasMoreDigits);
        continueIf(out().lessThanOrEqual(exponent, constant(Decimal128::maximumQuantumExponent)), slowCase);
        if (!mayBeExact)
            continueIf(out().isZero64(highOf(x.upper)), slowCase);
        continueIf(out().lessThanOrEqual(power, constant(Decimal128::greatestPowerOfDivisors)), slowCase);
        using Divisor = Decimal128::Divisor;
        static_assert(sizeof(Divisor) == 1 << 5);
        Value divisor = m_generator.addressInTables(OBJECT_OFFSETOF(Decimal128::Tables, divisors), shl(power, 5));
        auto load = [&](ptrdiff_t offset) { return m_generator.loadFromTables(divisor, offset); };
        Value quotient = out().lShr(out().uMulHigh(out().lShr(x.low, out().castToInt32(power)), load(OBJECT_OFFSETOF(Divisor, reciprocal))), out().castToInt32(load(OBJECT_OFFSETOF(Divisor, shift))));
        auto whole = [&] { return out().mul(quotient, load(OBJECT_OFFSETOF(Divisor, value))); };
        auto ifInexact = [&](Value increment) {
            if (mayBeExact)
                return out().bitAnd(increment, widen(out().notEqual(x.low, whole())));
            // The last digit is not 0, so the remainder is not.
            if (!digits->hasInt32() || digits->asInt32() < 0)
                continueIf(out().notZero64(x.low), slowCase);
            return increment;
        };

        Value count = quotient;
        switch (mode) {
        case RoundingMode::Ceil:
            count = out().add(quotient, ifInexact(out().bitXor(lShr(x.upper, 63), constant(1))));
            break;
        case RoundingMode::Floor:
            count = out().add(quotient, ifInexact(lShr(x.upper, 63)));
            break;
        case RoundingMode::Trunc:
            break;
        case RoundingMode::HalfExpand:
            count = out().add(quotient, widen(out().aboveOrEqual(out().sub(x.low, whole()), load(OBJECT_OFFSETOF(Divisor, half)))));
            break;
        case RoundingMode::HalfEven:
            count = out().add(quotient, widen(out().above(out().add(out().sub(x.low, whole()), out().bitAnd(quotient, constant(1))), load(OBJECT_OFFSETOF(Divisor, half)))));
            break;
        }
        Value unit = shl(exponent, Decimal128::shiftOfUnit);
        Words rounded = finishesBoth ? Words { count, unit } : finish(count, unit, x.upper, result, slowCase);
        auto roundedLow = out().anchor(rounded.low);
        auto roundedUpperOrUnit = out().anchor(rounded.upper);
        out().jump(continuation);

        m_generator.appendTo(continuation);
        Words joined { phi(sameLow, roundedLow), phi(sameUpperOrUnit, roundedUpperOrUnit) };
        return finishesBoth ? finish(joined.low, joined.upper, x.upper, result, slowCase) : joined;
    }

    // The power has 32 bits.
    Words scale10(Words x, Value power, Block slowCase)
    {
        // The coefficient of NaN and infinity is 0.
        continueIf(out().notZero64(out().bitOr(x.low, highOf(x.upper))), slowCase);
        Value exponent = out().add(exponentOf(x.upper), out().signExt32To64(power));
        continueIf(isQuantumExponent(exponent), slowCase);
        constexpr uint64_t field = ((1ull << Decimal128::bitsOfExponent) - 1) << Decimal128::shiftOfExponent;
        return { x.low, out().bitOr(out().bitAnd(x.upper, constant(~field)), lShr(shl(exponent, Decimal128::shiftOfUnit), 1)) };
    }

    // Of 64 bits, and not the least.
    Words fromInteger(Value integer, bool mayBeNegative, TrailingZeros result, Block slowCase)
    {
        Value magnitude = mayBeNegative ? magnitudeOf(integer) : integer;
        Value sign = mayBeNegative ? integer : constant(0);
        if (result == TrailingZeros::Some)
            return { magnitude, shl(lShr(sign, 63), 63) };
        return finish(magnitude, constant(0), sign, result, slowCase);
    }

    Words withoutTrailingZeros(Words x)
    {
        Block isSame = m_generator.newBlock();
        Block continuation = m_generator.newBlock();

        continueIf(out().isZero64(highOf(x.upper)), isSame);
        Words stripped = finish(x.low, unitOf(x.upper), x.upper, TrailingZeros::None, isSame);
        auto strippedLow = out().anchor(stripped.low);
        auto strippedUpper = out().anchor(stripped.upper);
        out().jump(continuation);

        m_generator.appendTo(isSame);
        auto sameLow = out().anchor(x.low);
        auto sameUpper = out().anchor(x.upper);
        out().jump(continuation);

        m_generator.appendTo(continuation);
        return { phi(strippedLow, sameLow), phi(strippedUpper, sameUpper) };
    }

    Words abs(Words x) { return { x.low, out().bitAnd(x.upper, constant(~Decimal128::signBit)) }; }
    Words negate(Words x) { return { x.low, out().select(isNaN(x), x.upper, out().bitXor(x.upper, constant(Decimal128::signBit))) }; }

    Value isNaN(Words x) { return out().equal(x.upper, constant(Decimal128::upperOfNaN)); }
    Value isFinite(Words x)
    {
        // The exponents of NaN and infinity differ in the last bit only.
        static_assert((Decimal128::exponentOfNaN ^ Decimal128::exponentOfInfinity) == 1);
        return out().notEqual(lShr(shl(x.upper, 1), Decimal128::shiftOfUnit + 1), constant(Decimal128::exponentOfNaN >> 1));
    }

    // Both of operands without trailing zeros.
    Value equals(Words x, Words y) { return out().bitAnd(areEqualOrBothNaN(x, y), out().logicalNot(isNaN(x))); }
    Value notEquals(Words x, Words y) { return out().logicalNot(out().bitOr(areEqualOrBothNaN(x, y), out().bitOr(isNaN(x), isNaN(y)))); }

    // Calls the functor with two integers that are in the order of the operands, once for each way to them. It has to end the block.
    void compare(Words x, Words y, Block slowCase, NOESCAPE const Invocable<void(Value, Value)> auto& functor)
    {
        Block notShortCase = m_generator.newBlock();
        Aligned aligned = align(x, y, notShortCase);
        // The signs first: they do not wait for the factors.
        functor(out().mul(aligned.factor, withSignOf(x.low, x.upper)), out().mul(aligned.otherFactor, withSignOf(y.low, y.upper)));

        // Of one sign and one exponent: the upper word of the difference, with a bit behind it for the lower word.
        m_generator.appendTo(notShortCase);
        continueIf(out().isZero64(lShr(out().bitXor(x.upper, y.upper), Decimal128::shiftOfExponent)), slowCase);
        continueIf(out().logicalNot(isNaN(x)), slowCase);
        Value high = out().sub(out().sub(x.upper, y.upper), widen(out().below(x.low, y.low)));
        Value difference = out().bitOr(shl(high, 1), widen(out().notEqual(x.low, y.low)));
        functor(withSignOf(difference, x.upper), constant(0));
    }

    // Of 32 bits.
    Value exponent(Words x, Block slowCase)
    {
        Block isLong = m_generator.newBlock();
        Block continuation = m_generator.newBlock();

        constexpr int shift = Decimal128::shiftOfDigitSteps;
        continueIf(out().isZero64(highOf(x.upper)), isLong);
        Value index = out().sub(m_generator.countLeadingZeros64(x.low), constant(64 - shift));
        continueIf(out().below(index, constant(shift)), slowCase);
        Value step = m_generator.loadFromTables(m_generator.addressInTables(OBJECT_OFFSETOF(Decimal128::Tables, digitSteps), shl(index, 3)), 0);
        auto ofShort = out().anchor(out().add(exponentOf(x.upper), lShr(out().add(x.low, step), shift)));
        out().jump(continuation);

        m_generator.appendTo(isLong);
        Value longStep = m_generator.addressInTables(OBJECT_OFFSETOF(Decimal128::Tables, digitStepsOfLongValues), shl(m_generator.countLeadingZeros64(highOf(x.upper)), 4));
        Value carry = widen(out().below(out().add(x.low, m_generator.loadFromTables(longStep, 0)), x.low));
        auto ofLong = out().anchor(exponentOf(out().add(out().add(x.upper, m_generator.loadFromTables(longStep, sizeof(uint64_t))), carry)));
        out().jump(continuation);

        m_generator.appendTo(continuation);
        return out().castToInt32(phi(ofShort, ofLong));
    }

private:
    using Alignment = Decimal128::Alignment;

    auto& out() { return m_generator.out(); }
    void continueIf(Value condition, Block otherwise) { m_generator.continueIf(condition, otherwise); }
    Value constant(uint64_t value) { return out().constInt64(static_cast<int64_t>(value)); }
    Value shl(Value value, int amount) { return out().shl(value, out().constInt32(amount)); }
    Value lShr(Value value, int amount) { return out().lShr(value, out().constInt32(amount)); }
    Value aShr(Value value, int amount) { return out().aShr(value, out().constInt32(amount)); }
    Value rotateRightByOne(Value value) { return out().bitOr(lShr(value, 1), shl(value, 63)); }
    Value widen(Value condition) { return out().zeroExt(condition, m_generator.int64Type()); }
    Value phi(auto... incoming) { return out().phi(m_generator.int64Type(), incoming...); }

    // What the coefficient has above 64 bits, at the top of the word.
    Value highOf(Value upper) { return shl(upper, 64 - Decimal128::shiftOfExponent); }
    Value highOfEither(Words x, Words y)
    {
        if (x.upper->hasInt64() || y.upper->hasInt64())
            return out().bitOr(highOf(x.upper), highOf(y.upper));
        return highOf(out().bitOr(x.upper, y.upper));
    }
    Value unitOf(Value upper) { return shl(upper, 1); }
    Value exponentOf(Value upper) { return aShr(unitOf(upper), Decimal128::shiftOfUnit); }
    // Whether any coefficient can have it.
    Value isQuantumExponent(Value exponent) { return out().belowOrEqual(out().sub(exponent, constant(Decimal128::minimumQuantumExponent)), constant(Decimal128::maximumQuantumExponent - Decimal128::minimumQuantumExponent)); }

    // In the upper bit.
    Value signOfProduct(Words x, Words y)
    {
        if (x.upper->hasInt64() && x.upper->asInt64() >= 0)
            return y.upper;
        if (y.upper->hasInt64() && y.upper->asInt64() >= 0)
            return x.upper;
        return out().bitXor(x.upper, y.upper);
    }

    Value withSignOf(Value magnitude, Value sign)
    {
        Value mask = aShr(sign, 63);
        return out().sub(out().bitXor(magnitude, mask), mask);
    }

    Value magnitudeOf(Value integer) { return out().select(out().lessThan(integer, constant(0)), out().neg(integer), integer); }

    // What makes the coefficients of finite operands counts of one unit, below 2^62.
    struct Aligned {
        Value factor;
        Value otherFactor;
        Value alignment;
    };
    Aligned align(Words x, Words y, Block slowCase)
    {
        continueIf(out().isZero64(highOfEither(x, y)), slowCase);

        // The difference wraps around, but no two exponents are far enough apart to seem near by that.
        constexpr int reach = Decimal128::reachOfAlignments;
        static_assert(Decimal128::exponentOfNaN - Decimal128::minimumQuantumExponent < (1 << Decimal128::bitsOfExponent) - reach);
        constexpr int shift = 6;
        static_assert(sizeof(Alignment) == 1 << shift);
        Value index = out().add(aShr(out().sub(unitOf(x.upper), unitOf(y.upper)), Decimal128::shiftOfUnit - shift), constant(reach << shift));
        continueIf(out().belowOrEqual(index, constant(2 * reach << shift)), slowCase);
        Value alignment = m_generator.addressInTables(OBJECT_OFFSETOF(Decimal128::Tables, alignments), index);
        auto load = [&](ptrdiff_t offset) { return m_generator.loadFromTables(alignment, offset); };

        // NaN and infinity are far from any finite value, and their exponents are above the greatest with which no sum is infinite.
        auto isKnownToBeLow = [](Value upper) {
            return upper->hasInt64() && (upper->asInt64() << 1 >> Decimal128::shiftOfUnit) <= Decimal128::maximumQuantumExponent;
        };
        if (!isKnownToBeLow(x.upper) && !isKnownToBeLow(y.upper))
            continueIf(out().lessThanOrEqual(unitOf(x.upper), load(OBJECT_OFFSETOF(Alignment, greatestUnit))), slowCase);
        continueIf(out().belowOrEqual(x.low, load(OBJECT_OFFSETOF(Alignment, limit))), slowCase);
        continueIf(out().belowOrEqual(y.low, load(OBJECT_OFFSETOF(Alignment, otherLimit))), slowCase);
        return { load(OBJECT_OFFSETOF(Alignment, factor)), load(OBJECT_OFFSETOF(Alignment, otherFactor)), alignment };
    }

    // As if the first operand were positive.
    Value countOfOther(Words x, Words y, const Aligned& aligned) { return withSignOf(out().mul(aligned.otherFactor, y.low), signOfProduct(x, y)); }

    Words addOrSubtract(Words x, Words y, bool subtracts, TrailingZeros result, Block slowCase)
    {
        Aligned aligned = align(x, y, slowCase);
        return addOrSubtract(x, y, aligned, countOfOther(x, y, aligned), subtracts, result, slowCase);
    }

    Words addOrSubtract(Words x, Words y, const Aligned& aligned, Value otherCount, bool subtracts, TrailingZeros result, Block slowCase)
    {
        Value count = out().mul(aligned.factor, x.low);
        Value unit = out().select(out().lessThan(unitOf(x.upper), unitOf(y.upper)), unitOf(x.upper), unitOf(y.upper));
        Value sum = subtracts ? out().sub(count, otherCount) : out().add(count, otherCount);
        return finish(magnitudeOf(sum), unit, out().bitXor(sum, x.upper), result, slowCase, aligned.alignment);
    }

    // The sign is the upper bit. 0 is left to the slow case.
    Words finish(Value magnitude, Value unit, Value sign, TrailingZeros result, Block slowCase, Value alignment = nullptr)
    {
        auto words = [&](Value low, Value unit) {
            return Words { low, rotateRightByOne(out().bitOr(unit, lShr(sign, 63))) };
        };
        if (result == TrailingZeros::Some) {
            continueIf(out().notZero64(magnitude), slowCase);
            return words(magnitude, unit);
        }

        Block endsInZero = m_generator.newBlock();
        Block loop = m_generator.newBlock();
        Block isStripped = m_generator.newBlock();
        Block continuation = m_generator.newBlock();

        Value inverseOf5 = alignment ? m_generator.loadFromTables(alignment, OBJECT_OFFSETOF(Alignment, inverseOf5)) : constant(Decimal128::inverseOf5);
        Value tenth = rotateRightByOne(out().mul(inverseOf5, magnitude));
        continueIf(out().above(tenth, alignment ? m_generator.loadFromTables(alignment, OBJECT_OFFSETOF(Alignment, greatestTenth)) : constant(Decimal128::greatestTenth)), endsInZero);
        auto sameMagnitude = out().anchor(magnitude);
        auto sameUnit = out().anchor(unit);
        out().jump(continuation);

        m_generator.appendTo(endsInZero);
        continueIf(out().notZero64(magnitude), slowCase);
        auto firstMagnitude = out().anchor(tenth);
        auto firstUnit = out().anchor(unit);
        out().jump(loop);

        m_generator.appendTo(loop);
        Value shorter = phi(firstMagnitude);
        Value unitBefore = phi(firstUnit);
        Value greaterUnit = out().add(unitBefore, constant(1ull << Decimal128::shiftOfUnit));
        Value nextTenth = rotateRightByOne(out().mul(shorter, constant(Decimal128::inverseOf5)));
        out().addIncomingToPhi(shorter, out().anchor(nextTenth));
        out().addIncomingToPhi(unitBefore, out().anchor(greaterUnit));
        m_generator.branch(out().above(nextTenth, constant(Decimal128::greatestTenth)), isStripped, loop);

        m_generator.appendTo(isStripped);
        auto strippedMagnitude = out().anchor(shorter);
        auto strippedUnit = out().anchor(greaterUnit);
        out().jump(continuation);

        m_generator.appendTo(continuation);
        Value low = phi(sameMagnitude, strippedMagnitude);
        return words(low, phi(sameUnit, strippedUnit));
    }

    Value areEqualOrBothNaN(Words x, Words y)
    {
        // The representation of a value is unique, but for the sign of 0.
        Value areSame = out().isZero64(out().bitOr(out().bitXor(x.low, y.low), out().bitXor(x.upper, y.upper)));
        Value areZero = out().isZero64(out().bitOr(out().bitOr(x.low, y.low), shl(out().bitOr(x.upper, y.upper), 1)));
        return out().bitOr(areSame, areZero);
    }

    Generator& m_generator;
};

} // namespace JSC
