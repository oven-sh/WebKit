/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRulesInternal.h"

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class NegativeZero : uint8_t { Impossible, Possible };

struct NaturalOperand {
    uint16_t primitives { 0 };
    Range integers;
    bool isMadeOfPrimitives { true };
    Range boundsOfFractions;

    bool hasOnlyPrimitives(uint16_t allowed) const { return isMadeOfPrimitives && !(primitives & ~allowed); }
    bool isNever() const { return isMadeOfPrimitives && !primitives; }
    bool isInteger() const { return !isNever() && hasOnlyPrimitives(Primitive::Integer); }
    bool isInt32() const { return isInteger() && integers.isSubsetOf(Range::int32()); }
    bool isBoolean() const { return !isNever() && hasOnlyPrimitives(Primitive::Boolean); }
    bool becomesNumberWithoutFraction() const { return !isNever() && hasOnlyPrimitives(Primitive::NumberLike & ~Primitive::FractionalNumber); }
    bool isNumberWithinBounds() const
    {
        bool holdsFractionsWithoutBounds = primitives & Primitive::FractionalNumber && boundsOfFractions.isEmpty();
        return !isNever() && hasOnlyPrimitives(Primitive::Integer | Primitive::FractionalNumber) && !holdsFractionsWithoutBounds;
    }
    Range boundsOfNumbers() const { return integers.unionWith(boundsOfFractions); }
};

using NaturalOperands = std::span<const NaturalOperand>;

constexpr OpcodeSet fractionFreeOperators {
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Mod, Opcode::Negate, Opcode::Increment, Opcode::Decrement, Opcode::ToNumber, Opcode::ToNumeric
};

constexpr OpcodeSet shiftOperators { Opcode::LShift, Opcode::RShift, Opcode::URShift };

struct NaturalPrimitives {
    uint16_t primitives { 0 };
    Range integers;
};

} // anonymous namespace

static NaturalPrimitives primitivesOf(const NaturalType& natural)
{
    NaturalPrimitives written;
    written.primitives = fromWire(natural.primitives);
    if (written.primitives & Primitive::Integer)
        written.integers = Range::full();
    if (natural.integers) {
        written.primitives |= Primitive::Integer;
        written.integers = written.integers.unionWith({ natural.integers->minimum, natural.integers->maximum });
    }
    return written;
}

static TypeParts primitivePartsOf(const NaturalType& natural)
{
    NaturalPrimitives written = primitivesOf(natural);
    TypeParts parts;
    parts.primitives = written.primitives;
    parts.integers = written.integers;
    return parts;
}

static bool describesPrimitivesAlone(const NaturalType& natural)
{
    return !natural.isObject && !natural.isFunction && !natural.opaque && !natural.builtinClass;
}

ALWAYS_INLINE_INTO_OTHER_FILES TypeID naturalTypeFrom(TypeArena& types, const NaturalType& natural)
{
    if (describesPrimitivesAlone(natural)) {
        NaturalPrimitives written = primitivesOf(natural);
        if (std::optional<TypeID> type = tryMakeTypeOfPrimitives(written.primitives, written.integers))
            return *type;
    }
    TypeParts parts = primitivePartsOf(natural);
    parts.anyObject = natural.isObject;
    parts.anyFunction = natural.isObject || natural.isFunction;
    if (parts.anyFunction)
        parts.seeds.add(UnknownSeed::ObjectWithoutLayout);
    parts.opaque = natural.opaque;
    if (natural.builtinClass)
        parts.unnamedClasses = TypeArena::partsOf(natural.builtinClass);
    return types.intern(parts);
}

static TypeID widestTypeOfPrimitivesIn(const NaturalType& natural)
{
    if (!describesPrimitivesAlone(natural))
        return { };
    TypeParts parts = primitivePartsOf(natural);
    RELEASE_ASSERT(!(parts.primitives & primitivesWithLists));
    if (!parts.primitives)
        return { };
    if (std::optional<TypeID> type = tryMakeTypeOfPrimitives(parts.primitives, parts.integers))
        return *type;
    std::optional<TypeID> widened = tryMakeTypeOfPrimitives(parts.primitives, { widenedMinimum(parts.integers.minimum), widenedMaximum(parts.integers.maximum) });
    RELEASE_ASSERT(widened);
    return *widened;
}

namespace {

struct WidestPrimitiveResultRules {
    using Result = TypeID;

    Result natural(const Draft&, const NaturalType& written) { return widestTypeOfPrimitivesIn(written); }
    Result natural(const Draft&, NaturalTable table)
    {
        Vector<OperandClass, 2> classes;
        classes.fill(OperandClass::Any, numberOfOperandsOf(table));
        return widestTypeOfPrimitivesIn(naturalTypeOf(table, classes.span()));
    }
    Result none(auto&&...) { return { }; }
    Result constant(auto&&...) { return { }; }
    Result sameAs(auto&&...) { return { }; }
    Result allocation(auto&&...) { return { }; }
    Result declared(auto&&...) { return { }; }
    Result declaredContents(auto&&...) { return { }; }
    Result declaredResult(auto&&...) { return { }; }
    Result declaredParameter(auto&&...) { return { }; }
    Result unknown(auto&&...) { return { }; }
    Result cast(auto&&...) { return { }; }
    Result resultOperand(auto&&...) { return { }; }
};

} // anonymous namespace

static TypeID widestPrimitiveResultByRuleOf(Opcode opcode)
{
    if (resultRuleOf(opcode) != ResultRule::Natural)
        return { };
    std::optional<Opcode> generic = genericOperationOf(opcode);
    if (generic && resultRuleOf(*generic) == ResultRule::Unknown)
        return { };
    WidestPrimitiveResultRules rules;
    return applyResultRule(rules, Draft(opcode), 0, ResultSide::Unproven);
}

ALWAYS_INLINE_INTO_OTHER_FILES TypeID widestPrimitiveResultOf(Opcode opcode)
{
    static const std::array<TypeID, numberOfEnumerators<Opcode>> results = [] {
        std::array<TypeID, numberOfEnumerators<Opcode>> byRule;
        for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index)
            byRule[index] = widestPrimitiveResultByRuleOf(static_cast<Opcode>(index));
        return byRule;
    }();
    return results[static_cast<unsigned>(opcode)];
}

namespace {

struct WidestPrimitiveResultRulesWithArena : WidestPrimitiveResultRules {
    explicit WidestPrimitiveResultRulesWithArena(TypeArena& types)
        : types(types)
    {
    }

    Result natural(const Draft&, const NaturalType& written)
    {
        if (!describesPrimitivesAlone(written) || !primitivePartsOf(written).primitives)
            return { };
        return naturalTypeFrom(types, written);
    }
    Result natural(const Draft& draft, NaturalTable table) { return WidestPrimitiveResultRules::natural(draft, table); }

    TypeArena& types;
};

} // anonymous namespace

TypeID widestPrimitiveResultOf(TypeArena& types, Opcode producer, uint32_t resultIndex)
{
    if (resultRuleOf(producer) != ResultRule::Results)
        return { };
    std::optional<uint32_t> numberOfResults = numberOfDeclaredResultsOf(producer);
    if (!numberOfResults || resultIndex >= *numberOfResults)
        return { };
    WidestPrimitiveResultRulesWithArena rules(types);
    return applyResultRule(rules, Draft(producer), resultIndex, ResultSide::Unproven);
}

static ALWAYS_INLINE NaturalOperand naturalOperandOf(const TypeArena& types, TypeID id)
{
    RELEASE_ASSERT(id);
    if (holdsOnlyPrimitives(id))
        return { primitivesInID(id), integersInID(id), true, Range::empty() };
    const Type& type = types.recordOf(id);
    return { type.primitives, type.integers, !type.hasObjects() && type.isJavaScriptValue(), type.boundsOfFractions };
}

static ALWAYS_INLINE Vector<NaturalOperand, 2> naturalOperandsOf(const TypeArena& types, OperandTypes operandTypes, uint32_t numberOfOperands)
{
    RELEASE_ASSERT(numberOfOperands <= operandTypes.size());
    return Vector<NaturalOperand, 2>(numberOfOperands, [&](size_t index) {
        return naturalOperandOf(types, operandTypes.types[index]);
    });
}

static OperandClass classOf(const NaturalOperand& operand)
{
    if (operand.hasOnlyPrimitives(Primitive::NumberLike))
        return OperandClass::NumberLike;
    if (operand.hasOnlyPrimitives(Primitive::BigInt))
        return OperandClass::BigInt;
    if (operand.hasOnlyPrimitives(Primitive::AnyString))
        return OperandClass::String;
    return OperandClass::Any;
}

static ALWAYS_INLINE bool hasNever(NaturalOperands operands)
{
    for (const NaturalOperand& operand : operands) {
        if (operand.isNever())
            return true;
    }
    return false;
}

static TypeID naturalTypeOfIntegers(TypeArena& types, Range integers, uint16_t otherPrimitives = 0)
{
    TypeParts parts;
    parts.primitives = otherPrimitives;
    if (!integers.isEmpty()) {
        parts.primitives |= Primitive::Integer;
        parts.integers = integers;
    }
    if (std::optional<TypeID> type = tryMakeTypeOfPrimitives(parts.primitives, parts.integers))
        return *type;
    return types.intern(parts);
}

static TypeID exactIntegerType(TypeArena& types, CheckedInt64 minimum, CheckedInt64 maximum, NegativeZero negativeZero = NegativeZero::Impossible)
{
    if (minimum.hasOverflowed() || maximum.hasOverflowed())
        return TypeArena::fromPrimitives(Primitive::NumberWithoutFraction);
    Range range { minimum.value(), maximum.value() };
    if (!range.isSubsetOf(Range::exact()))
        return TypeArena::fromPrimitives(Primitive::NumberWithoutFraction);
    return naturalTypeOfIntegers(types, range, negativeZero == NegativeZero::Possible ? Primitive::WholeNumber : 0);
}

static TypeID integerProductType(TypeArena& types, Range a, Range b)
{
    int64_t minimum = std::numeric_limits<int64_t>::max();
    int64_t maximum = std::numeric_limits<int64_t>::min();
    for (int64_t left : { a.minimum, a.maximum }) {
        for (int64_t right : { b.minimum, b.maximum }) {
            CheckedInt64 product = CheckedInt64(left) * right;
            if (product.hasOverflowed())
                return TypeArena::fromPrimitives(Primitive::NumberWithoutFraction);
            minimum = std::min(minimum, product.value());
            maximum = std::max(maximum, product.value());
        }
    }
    bool mayBeNegativeZero = (a.minimum < 0 && b.contains(0)) || (b.minimum < 0 && a.contains(0));
    return exactIntegerType(types, minimum, maximum, mayBeNegativeZero ? NegativeZero::Possible : NegativeZero::Impossible);
}

static Range integerRemaindersOf(Range dividends, Range divisors)
{
    int64_t largestDivisor = std::max(std::abs(divisors.minimum), std::abs(divisors.maximum));
    return {
        std::max(std::min<int64_t>(dividends.minimum, 0), -(largestDivisor - 1)),
        std::min(std::max<int64_t>(dividends.maximum, 0), largestDivisor - 1)
    };
}

static TypeID integerRemainderType(TypeArena& types, Range dividends, Range divisors)
{
    bool mayBeNegativeZero = dividends.minimum < 0;
    bool mayBeNaN = divisors.contains(0);
    return naturalTypeOfIntegers(types, integerRemaindersOf(dividends, divisors), mayBeNegativeZero || mayBeNaN ? Primitive::WholeNumber : 0);
}

TypeID wholeRemainderType(TypeArena& types, Range dividends, Range divisors)
{
    Range remainders = integerRemaindersOf(dividends, divisors);
    return naturalTypeOfIntegers(types, remainders.isEmpty() ? Range::one(0) : remainders);
}

std::optional<Range> rangeOfProduct(Range multiplicands, Range multipliers)
{
    Range products = Range::empty();
    for (int64_t multiplicand : { multiplicands.minimum, multiplicands.maximum }) {
        for (int64_t multiplier : { multipliers.minimum, multipliers.maximum }) {
            CheckedInt64 product = CheckedInt64(multiplicand) * multiplier;
            if (product.hasOverflowed())
                return std::nullopt;
            products = products.unionWith(Range::one(product.value()));
        }
    }
    return products;
}

TypeID wholeProductType(TypeArena& types, Range multiplicands, Range multipliers)
{
    std::optional<Range> products = rangeOfProduct(multiplicands, multipliers);
    return products && products->isSubsetOf(Range::exact()) ? naturalTypeOfIntegers(types, *products) : TypeID();
}

static int64_t floorOfIntegerQuotient(int64_t dividend, int64_t divisor)
{
    int64_t quotient = dividend / divisor;
    bool isRoundedUp = dividend % divisor && (dividend < 0) != (divisor < 0);
    return isRoundedUp ? quotient - 1 : quotient;
}

Range quotientsRoundedEitherWay(Range dividends, Range divisors)
{
    RELEASE_ASSERT(!dividends.isEmpty() && !divisors.isEmpty() && !divisors.contains(0));
    Range quotients = Range::empty();
    for (int64_t dividend : { dividends.minimum, dividends.maximum }) {
        for (int64_t divisor : { divisors.minimum, divisors.maximum })
            quotients = quotients.unionWith({ floorOfIntegerQuotient(dividend, divisor), -floorOfIntegerQuotient(-dividend, divisor) });
    }
    return quotients;
}

static Range quotientsConvertedToInt32WhereDivisorMayBeZero(Range dividends, Range divisors)
{
    int64_t magnitude = std::max(std::abs(dividends.minimum), std::abs(dividends.maximum));
    return { dividends.minimum >= 0 && divisors.minimum >= 0 ? 0 : -magnitude, magnitude };
}

TypeID integerQuotientType(TypeArena& types, Range dividends, Range divisors, QuotientRounding rounding)
{
    bool convertsToInt32 = rounding == QuotientRounding::ToInt32;
    if (divisors.contains(0) && !convertsToInt32)
        return { };
    Range quotients = divisors.contains(0) ? quotientsConvertedToInt32WhereDivisorMayBeZero(dividends, divisors) : quotientsRoundedEitherWay(dividends, divisors);
    if (rounding == QuotientRounding::Floor && divisors.isSingleValue() && divisors.minimum > 0 && !dividends.isSubsetOf(Range::exact()))
        quotients = { floorOfIntegerQuotient(dividends.minimum, divisors.minimum), floorOfIntegerQuotient(dividends.maximum, divisors.minimum) };
    if (convertsToInt32 && !quotients.isSubsetOf(Range::int32()))
        return TypeTable::int32;
    if (!quotients.isSubsetOf(Range::exact()))
        return { };
    return naturalTypeOfIntegers(types, quotients);
}

static TypeID bitwiseResultType(TypeArena& types, Opcode opcode, Range a, Range b)
{
    if (!a.isSubsetOf(Range::int32()) || !b.isSubsetOf(Range::int32()))
        return { };
    switch (opcode) {
    case Opcode::BitAnd:
        if (a.minimum >= 0 && b.minimum >= 0)
            return naturalTypeOfIntegers(types, { 0, std::min(a.maximum, b.maximum) });
        if (a.minimum >= 0)
            return naturalTypeOfIntegers(types, { 0, a.maximum });
        if (b.minimum >= 0)
            return naturalTypeOfIntegers(types, { 0, b.maximum });
        return TypeTable::int32;
    case Opcode::BitOr:
    case Opcode::BitXor: {
        bool mayBeNegative = a.minimum < 0 || b.minimum < 0;
        if (mayBeNegative && opcode == Opcode::BitXor)
            return TypeTable::int32;
        if (a.maximum < 0 || b.maximum < 0)
            return naturalTypeOfIntegers(types, { std::min(a.minimum, b.minimum), -1 });
        return naturalTypeOfIntegers(types, { mayBeNegative ? std::min(a.minimum, b.minimum) : 0, (int64_t { 1 } << std::bit_width(static_cast<uint64_t>(std::max(a.maximum, b.maximum)))) - 1 });
    }
    default:
        return { };
    }
}

static Range shiftCountsOf(const NaturalOperand& count)
{
    constexpr Range everyCount { 0, 31 };
    if (!count.isInteger() || count.integers.maximum - count.integers.minimum >= everyCount.maximum)
        return everyCount;
    Range counts { count.integers.minimum & everyCount.maximum, count.integers.maximum & everyCount.maximum };
    return counts.minimum <= counts.maximum ? counts : everyCount;
}

static Range rangeAsInt32(const NaturalOperand& operand)
{
    return operand.isInt32() ? operand.integers : Range::int32();
}

static Range rangeAsUint32(const NaturalOperand& operand)
{
    constexpr int64_t modulus = Range::uint32().maximum + 1;
    if (!operand.isInteger())
        return Range::uint32();
    if (operand.integers.isSubsetOf(Range::uint32()))
        return operand.integers;
    if (operand.integers.isSubsetOf({ -modulus, -1 }))
        return { operand.integers.minimum + modulus, operand.integers.maximum + modulus };
    return Range::uint32();
}

static TypeID shiftResultType(TypeArena& types, Opcode opcode, const NaturalOperand& shifted, const NaturalOperand& count)
{
    Range counts = shiftCountsOf(count);
    switch (opcode) {
    case Opcode::URShift: {
        Range bits = rangeAsUint32(shifted);
        return naturalTypeOfIntegers(types, { bits.minimum >> counts.maximum, bits.maximum >> counts.minimum });
    }
    case Opcode::RShift: {
        Range bits = rangeAsInt32(shifted);
        return naturalTypeOfIntegers(types, { bits.minimum >> (bits.minimum < 0 ? counts.minimum : counts.maximum), bits.maximum >> (bits.maximum < 0 ? counts.maximum : counts.minimum) });
    }
    case Opcode::LShift: {
        Range bits = rangeAsInt32(shifted);
        Range products {
            bits.minimum * (int64_t { 1 } << (bits.minimum < 0 ? counts.maximum : counts.minimum)),
            bits.maximum * (int64_t { 1 } << (bits.maximum < 0 ? counts.minimum : counts.maximum))
        };
        return products.isSubsetOf(Range::int32()) ? naturalTypeOfIntegers(types, products) : TypeTable::int32;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static TypeID bitwiseResultBoundedByOneOperand(TypeArena& types, Opcode opcode, NaturalOperands operands)
{
    switch (opcode) {
    case Opcode::BitAnd: {
        std::optional<int64_t> largest;
        for (const NaturalOperand& operand : operands) {
            if (operand.isInteger() && operand.integers.isSubsetOf(Range::length()))
                largest = std::min(largest.value_or(operand.integers.maximum), operand.integers.maximum);
        }
        if (!largest)
            return { };
        return naturalTypeOfIntegers(types, { 0, *largest });
    }
    default:
        return { };
    }
}

static TypeID unaryIntegerResult(TypeArena& types, Opcode opcode, Range a)
{
    switch (opcode) {
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        return naturalTypeOfIntegers(types, a);
    case Opcode::Increment:
        return exactIntegerType(types, CheckedInt64(a.minimum) + 1, CheckedInt64(a.maximum) + 1);
    case Opcode::Decrement:
        return exactIntegerType(types, CheckedInt64(a.minimum) - 1, CheckedInt64(a.maximum) - 1);
    case Opcode::Negate:
        return exactIntegerType(types, -a.maximum, -a.minimum, a.contains(0) ? NegativeZero::Possible : NegativeZero::Impossible);
    case Opcode::BitNot:
        if (!a.isSubsetOf(Range::int32()))
            return { };
        return naturalTypeOfIntegers(types, { -a.maximum - 1, -a.minimum - 1 });
    default:
        return { };
    }
}

static TypeID binaryIntegerResult(TypeArena& types, Opcode opcode, Range a, Range b)
{
    switch (opcode) {
    case Opcode::Add:
        return exactIntegerType(types, CheckedInt64(a.minimum) + b.minimum, CheckedInt64(a.maximum) + b.maximum);
    case Opcode::Sub:
        return exactIntegerType(types, CheckedInt64(a.minimum) - b.maximum, CheckedInt64(a.maximum) - b.minimum);
    case Opcode::Mul:
        return integerProductType(types, a, b);
    case Opcode::Mod:
        return integerRemainderType(types, a, b);
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
        return bitwiseResultType(types, opcode, a, b);
    default:
        return { };
    }
}

static TypeID integerResultOf(TypeArena& types, Opcode opcode, NaturalOperands operands)
{
    if (operands.size() == 1)
        return unaryIntegerResult(types, opcode, operands[0].integers);
    if (operands.size() == 2)
        return binaryIntegerResult(types, opcode, operands[0].integers, operands[1].integers);
    return { };
}

static TypeID numbersWithinExactBounds(TypeArena& types, CheckedInt64 minimum, CheckedInt64 maximum)
{
    if (minimum.hasOverflowed() || maximum.hasOverflowed())
        return { };
    Range bounds { minimum.value(), maximum.value() };
    if (!bounds.isSubsetOf(Range::exact()))
        return { };
    return types.numbersWithin(bounds);
}

static TypeID quotientOfIntegersWithinBounds(TypeArena& types, const NaturalOperand& dividend, const NaturalOperand& divisor)
{
    if (!dividend.isInteger() || !divisor.isInteger())
        return { };
    Range dividends = dividend.integers;
    Range divisors = divisor.integers;
    if (divisors.contains(0) || (dividends.contains(0) && divisors.maximum < 0))
        return { };
    if (!dividends.isSubsetOf(Range::exact()) || !divisors.isSubsetOf(Range::exact()))
        return { };
    return types.numbersWithin(quotientsRoundedEitherWay(dividends, divisors));
}

static TypeID numberResultWithinBounds(TypeArena& types, Opcode opcode, NaturalOperands operands)
{
    Range a = operands[0].boundsOfNumbers();
    switch (opcode) {
    case Opcode::Div:
        return quotientOfIntegersWithinBounds(types, operands[0], operands[1]);
    case Opcode::Add: {
        Range b = operands[1].boundsOfNumbers();
        return numbersWithinExactBounds(types, CheckedInt64(a.minimum) + b.minimum, CheckedInt64(a.maximum) + b.maximum);
    }
    case Opcode::Sub: {
        Range b = operands[1].boundsOfNumbers();
        return numbersWithinExactBounds(types, CheckedInt64(a.minimum) - b.maximum, CheckedInt64(a.maximum) - b.minimum);
    }
    case Opcode::Increment:
        return numbersWithinExactBounds(types, CheckedInt64(a.minimum) + 1, CheckedInt64(a.maximum) + 1);
    case Opcode::Decrement:
        return numbersWithinExactBounds(types, CheckedInt64(a.minimum) - 1, CheckedInt64(a.maximum) - 1);
    case Opcode::Negate:
        if (a.contains(0))
            return { };
        return types.numbersWithin({ -a.maximum, -a.minimum });
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        return types.numbersWithin(a);
    default:
        return { };
    }
}

static bool isIntegerOrNullish(const NaturalOperand& operand)
{
    return operand.isMadeOfPrimitives && operand.primitives & Primitive::UndefinedOrNull && operand.hasOnlyPrimitives(Primitive::Integer | Primitive::UndefinedOrNull);
}

static TypeID numberResultOf(TypeArena& types, Opcode opcode, NaturalOperands operands)
{
    if (allBitwiseOperators.contains(opcode) && std::ranges::any_of(operands, isIntegerOrNullish)) {
        Vector<NaturalOperand, 2> converted;
        for (const NaturalOperand& operand : operands) {
            converted.append(operand);
            if (!isIntegerOrNullish(operand))
                continue;
            converted.last().primitives = Primitive::Integer;
            converted.last().integers = operand.integers.unionWith(Range::one(0));
        }
        return numberResultOf(types, opcode, converted.span());
    }
    if ((opcode == Opcode::ToNumber || opcode == Opcode::ToNumeric) && operands[0].isBoolean())
        return naturalTypeOfIntegers(types, { operands[0].primitives & Primitive::False ? 0 : 1, operands[0].primitives & Primitive::True ? 1 : 0 });
    if (shiftOperators.contains(opcode))
        return shiftResultType(types, opcode, operands[0], operands[1]);
    if (std::ranges::all_of(operands, &NaturalOperand::isInteger)) {
        if (TypeID result = integerResultOf(types, opcode, operands))
            return result;
    }
    if (std::ranges::all_of(operands, &NaturalOperand::isNumberWithinBounds)) {
        if (TypeID result = numberResultWithinBounds(types, opcode, operands))
            return result;
    }
    if (!fractionFreeOperators.contains(opcode))
        return bitwiseResultBoundedByOneOperand(types, opcode, operands);
    if (std::ranges::all_of(operands, &NaturalOperand::becomesNumberWithoutFraction))
        return TypeArena::fromPrimitives(Primitive::NumberWithoutFraction);
    return { };
}

TypeID numberResultOf(TypeArena& types, Opcode opcode, OperandTypes operandTypes)
{
    Vector<NaturalOperand, 2> operands = naturalOperandsOf(types, operandTypes, numberOfNamedOperandsOf(opcode));
    if (hasNever(operands.span()))
        return TypeTable::never;
    return numberResultOf(types, opcode, operands.span());
}

ALWAYS_INLINE_INTO_OTHER_FILES TypeID typeAfterStepOfOne(TypeArena& types, Opcode opcode, std::span<const TypeID> operandTypes)
{
    auto stepIn = [](TypeID type) -> int64_t {
        if (!isIntegerType(type))
            return 0;
        Range integers = integersInID(type);
        bool isStep = integers.isSingleValue() && std::abs(integers.minimum) == 1;
        return isStep ? integers.minimum : 0;
    };
    TypeID stepped = operandTypes[0];
    int64_t step = 0;
    if (opcode == Opcode::Increment)
        step = 1;
    if (opcode == Opcode::Decrement)
        step = -1;
    if (opcode == Opcode::Sub)
        step = -stepIn(operandTypes[1]);
    if (opcode == Opcode::Add) {
        step = stepIn(operandTypes[1]);
        if (!step) {
            step = stepIn(operandTypes[0]);
            stepped = operandTypes[1];
        }
    }
    if (!step)
        return { };
    NaturalOperand counter = naturalOperandOf(types, stepped);
    if (!counter.isInteger() || !counter.integers.isSubsetOf(Range::exact()))
        return { };
    auto afterStep = [&](int64_t integer) {
        return std::clamp(integer + step, -maxExactInteger, maxExactInteger);
    };
    return naturalTypeOfIntegers(types, { afterStep(counter.integers.minimum), afterStep(counter.integers.maximum) });
}

TypeID naturalTypeOfOperands(const TypingContext& context, Opcode opcode, NaturalTable table, OperandTypes operandTypes)
{
    Vector<NaturalOperand, 2> operands = naturalOperandsOf(context.types, operandTypes, numberOfOperandsOf(table));
    if (hasNever(operands.span()))
        return TypeTable::never;
    if (opcode == Opcode::ToNumeric && operands[0].hasOnlyPrimitives(Primitive::Number | Primitive::BigInt))
        return operandTypes[0];
    if (allSums.contains(opcode) && operandTypes.size() == numberOfNamedOperandsOf(opcode)) {
        if (TypeID stepped = typeAfterStepOfOne(context.types, opcode, operandTypes.types))
            return stepped;
    }
    for (size_t index = 0; operands.size() == 2 && index < 2; ++index) {
        NaturalOperand& other = operands[1 - index];
        if (classOf(operands[index]) == OperandClass::NumberLike && classOf(other) == OperandClass::Any && other.hasOnlyPrimitives(Primitive::NumberLike | Primitive::BigInt))
            other.primitives &= ~Primitive::BigInt;
    }
    Vector<OperandClass, 2> classes;
    for (const NaturalOperand& operand : operands)
        classes.append(classOf(operand));
    TypeID upperBound = naturalTypeFrom(context.types, naturalTypeOf(table, classes.span()));
    bool mayGiveText = std::ranges::any_of(operands, [](const NaturalOperand& operand) {
        return !operand.hasOnlyPrimitives(Primitive::NumberLike | Primitive::BigInt);
    });
    bool mayGiveBigInt = std::ranges::all_of(operands, [](const NaturalOperand& operand) {
        return !operand.isMadeOfPrimitives || operand.primitives & (Primitive::BigInt | Primitive::Unspecified | Primitive::Absent);
    });
    if (!mayGiveText || !mayGiveBigInt)
        upperBound = context.types.withoutPrimitives(upperBound, (mayGiveText ? 0 : Primitive::AnyString) | (mayGiveBigInt ? 0 : Primitive::BigInt));
    if (isNeverType(upperBound))
        return TypeTable::never;
    if (!isNumberType(upperBound))
        return upperBound;
    TypeID refined = numberResultOf(context.types, opcode, operands.span());
    if (!refined)
        return upperBound;
    ASSERT(context.types.isSubset(refined, upperBound));
    return refined;
}

TypeID resultTypeOfOperator(const TypingContext& context, Opcode opcode, std::span<const TypeID> operandTypes)
{
    RELEASE_ASSERT(PayloadWord::form(opcode) && resultRuleOf(opcode) == ResultRule::Natural);
    RELEASE_ASSERT(operandTypes.size() == numberOfNamedOperandsOf(opcode));
    return resultTypeOf(context, Draft(opcode), OperandTypes { operandTypes });
}

static consteval TypeID typeOfIntegerElements(Range elements)
{
    return *tryMakeTypeOfPrimitives(Primitive::Integer, elements);
}

static_assert(!tryMakeTypeOfPrimitives(Primitive::Integer, { INT16_MIN, INT16_MAX }));
static_assert(!tryMakeTypeOfPrimitives(Primitive::Integer, { 0, UINT16_MAX }));

TypeID elementTypeOf(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
        return typeOfIntegerElements({ INT8_MIN, INT8_MAX });
    case TypeUint8:
    case TypeUint8Clamped:
        return typeOfIntegerElements({ 0, UINT8_MAX });
    case TypeInt16:
        return TypeTable::int16;
    case TypeUint16:
        return TypeTable::uint16;
    case TypeInt32:
        return TypeTable::int32;
    case TypeUint32:
        return TypeTable::uint32;
    case TypeFloat16:
    case TypeFloat32:
    case TypeFloat64:
        return TypeTable::number;
    case TypeBigInt64:
    case TypeBigUint64:
        return TypeTable::bigInt;
    case NotTypedArray:
    case TypeDataView:
        return TypeTable::anything;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
