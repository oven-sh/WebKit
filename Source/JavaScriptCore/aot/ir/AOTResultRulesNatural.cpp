/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRulesInternal.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class NegativeZero : uint8_t { Impossible, Possible };

using OperandTypes = std::span<const Type* const>;

constexpr OpcodeSet fractionFreeOperators {
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Mod, Opcode::Negate, Opcode::Increment, Opcode::Decrement, Opcode::ToNumber, Opcode::ToNumeric
};

} // anonymous namespace

Type naturalTypeFrom(const NaturalType& natural)
{
    Type type = Type::fromPrimitives(fromWire(natural.primitives));
    if (natural.integers) {
        type.primitives |= Primitive::Integer;
        type.integers = type.integers.unionWith({ natural.integers->minimum, natural.integers->maximum });
    }
    type.anyObject = natural.isObject;
    type.anyFunction = natural.isObject || natural.isFunction;
    type.opaque = natural.opaque;
    if (natural.builtinClass)
        type.classes = Type::builtinClass(natural.builtinClass).classes;
    type.canonicalize();
    return type;
}

static OperandClass classOf(const Type& type)
{
    if (type.isNever() || type.isNumberLike())
        return OperandClass::NumberLike;
    if (type.hasOnlyPrimitives(Primitive::BigInt))
        return OperandClass::BigInt;
    if (type.isAnyString())
        return OperandClass::String;
    return OperandClass::Any;
}

static Vector<const Type*, 2> operandTypesOf(const TypingContext& context, const Value& value, uint32_t numberOfOperands)
{
    Vector<const Type*, 2> types;
    for (uint32_t index = 0; index < numberOfOperands; ++index)
        types.append(&context.typeOf(value.operand(index)));
    return types;
}

static bool hasNever(OperandTypes operands)
{
    return std::ranges::any_of(operands, [](const Type* type) {
        return type->isNever();
    });
}

static Type exactIntegerType(CheckedInt64 minimum, CheckedInt64 maximum, NegativeZero negativeZero = NegativeZero::Impossible)
{
    if (minimum.hasOverflowed() || maximum.hasOverflowed())
        return Type::fromPrimitives(Primitive::NumberWithoutFraction);
    Range range { minimum.value(), maximum.value() };
    if (!range.isSubsetOf(Range::exact()))
        return Type::fromPrimitives(Primitive::NumberWithoutFraction);
    Type type = Type::integer(range);
    return negativeZero == NegativeZero::Possible ? type.withPrimitives(Primitive::WholeNumber) : type;
}

static Type integerProductType(Range a, Range b)
{
    int64_t minimum = std::numeric_limits<int64_t>::max();
    int64_t maximum = std::numeric_limits<int64_t>::min();
    for (int64_t left : { a.minimum, a.maximum }) {
        for (int64_t right : { b.minimum, b.maximum }) {
            CheckedInt64 product = CheckedInt64(left) * right;
            if (product.hasOverflowed())
                return Type::fromPrimitives(Primitive::NumberWithoutFraction);
            minimum = std::min(minimum, product.value());
            maximum = std::max(maximum, product.value());
        }
    }
    bool mayBeNegativeZero = (a.minimum < 0 && b.contains(0)) || (b.minimum < 0 && a.contains(0));
    return exactIntegerType(minimum, maximum, mayBeNegativeZero ? NegativeZero::Possible : NegativeZero::Impossible);
}

static Type integerRemainderType(Range dividends, Range divisors)
{
    int64_t largestDivisor = std::max(std::abs(divisors.minimum), std::abs(divisors.maximum));
    Range remainders {
        std::max(std::min<int64_t>(dividends.minimum, 0), -(largestDivisor - 1)),
        std::min(std::max<int64_t>(dividends.maximum, 0), largestDivisor - 1)
    };
    bool mayBeNegativeZero = dividends.minimum < 0;
    bool mayBeNaN = divisors.contains(0);
    Type type = Type::integer(remainders);
    return mayBeNegativeZero || mayBeNaN ? type.withPrimitives(Primitive::WholeNumber) : type;
}

static std::optional<Type> integerQuotientType(Range dividends, Range divisors, QuotientRounding rounding)
{
    bool convertsToInt32 = rounding == QuotientRounding::ToInt32;
    if (divisors.contains(0) && !convertsToInt32)
        return std::nullopt;
    int64_t magnitude = std::max(std::abs(dividends.minimum), std::abs(dividends.maximum));
    Range quotients { dividends.minimum >= 0 && divisors.minimum > 0 ? 0 : -magnitude, magnitude };
    if (convertsToInt32 && !quotients.isSubsetOf(Range::int32()))
        return Type::integer(Range::int32());
    if (!quotients.isSubsetOf(Range::exact()))
        return std::nullopt;
    return Type::integer(quotients);
}

static std::optional<Type> bitwiseResultType(Opcode opcode, Range a, Range b)
{
    if (!a.isSubsetOf(Range::int32()) || !b.isSubsetOf(Range::int32()))
        return std::nullopt;
    switch (opcode) {
    case Opcode::BitAnd:
        if (a.minimum >= 0 && b.minimum >= 0)
            return Type::integer({ 0, std::min(a.maximum, b.maximum) });
        if (a.minimum >= 0)
            return Type::integer({ 0, a.maximum });
        if (b.minimum >= 0)
            return Type::integer({ 0, b.maximum });
        return Type::integer(Range::int32());
    case Opcode::BitOr:
    case Opcode::BitXor: {
        bool mayBeNegative = a.minimum < 0 || b.minimum < 0;
        if (mayBeNegative && opcode == Opcode::BitXor)
            return Type::integer(Range::int32());
        if (a.maximum < 0 || b.maximum < 0)
            return Type::integer({ std::min(a.minimum, b.minimum), -1 });
        return Type::integer({ mayBeNegative ? std::min(a.minimum, b.minimum) : 0, (int64_t { 1 } << std::bit_width(static_cast<uint64_t>(std::max(a.maximum, b.maximum)))) - 1 });
    }
    case Opcode::LShift:
        return Type::integer(Range::int32());
    case Opcode::RShift: {
        if (b.minimum != b.maximum)
            return Type::integer({ std::min<int64_t>(a.minimum, 0), std::max<int64_t>(a.maximum, -1) });
        int64_t shift = b.minimum & 31;
        return Type::integer({ a.minimum >> shift, a.maximum >> shift });
    }
    case Opcode::URShift: {
        int64_t shift = b.isSingleValue() ? b.minimum & 31 : 0;
        return Type::integer({ 0, (a.minimum >= 0 ? a.maximum : Range::uint32().maximum) >> shift });
    }
    default:
        return std::nullopt;
    }
}

static std::optional<Type> bitwiseResultBoundedByOneOperand(Opcode opcode, OperandTypes operands)
{
    switch (opcode) {
    case Opcode::BitAnd: {
        std::optional<int64_t> largest;
        for (const Type* operand : operands) {
            if (operand->isInteger() && operand->integers.isSubsetOf(Range::length()))
                largest = std::min(largest.value_or(operand->integers.maximum), operand->integers.maximum);
        }
        if (!largest)
            return std::nullopt;
        return Type::integer({ 0, *largest });
    }
    case Opcode::URShift: {
        const Type& count = *operands[1];
        if (!count.isInteger() || !count.integers.isSingleValue())
            return std::nullopt;
        return Type::integer({ 0, Range::uint32().maximum >> (count.integers.minimum & 31) });
    }
    default:
        return std::nullopt;
    }
}

static std::optional<Type> unaryIntegerResult(Opcode opcode, Range a)
{
    switch (opcode) {
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        return Type::integer(a);
    case Opcode::Increment:
        return exactIntegerType(CheckedInt64(a.minimum) + 1, CheckedInt64(a.maximum) + 1);
    case Opcode::Decrement:
        return exactIntegerType(CheckedInt64(a.minimum) - 1, CheckedInt64(a.maximum) - 1);
    case Opcode::Negate:
        return exactIntegerType(-a.maximum, -a.minimum, a.contains(0) ? NegativeZero::Possible : NegativeZero::Impossible);
    case Opcode::BitNot:
        if (!a.isSubsetOf(Range::int32()))
            return std::nullopt;
        return Type::integer({ -a.maximum - 1, -a.minimum - 1 });
    default:
        return std::nullopt;
    }
}

static std::optional<Type> binaryIntegerResult(Opcode opcode, Range a, Range b)
{
    switch (opcode) {
    case Opcode::Add:
        return exactIntegerType(CheckedInt64(a.minimum) + b.minimum, CheckedInt64(a.maximum) + b.maximum);
    case Opcode::Sub:
        return exactIntegerType(CheckedInt64(a.minimum) - b.maximum, CheckedInt64(a.maximum) - b.minimum);
    case Opcode::Mul:
        return integerProductType(a, b);
    case Opcode::Mod:
        return integerRemainderType(a, b);
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
        return bitwiseResultType(opcode, a, b);
    default:
        return std::nullopt;
    }
}

static std::optional<Type> integerResultOf(Opcode opcode, OperandTypes operands)
{
    if (operands.size() == 1)
        return unaryIntegerResult(opcode, operands[0]->integers);
    if (operands.size() == 2)
        return binaryIntegerResult(opcode, operands[0]->integers, operands[1]->integers);
    return std::nullopt;
}

static std::optional<Type> numberResultOf(Opcode opcode, OperandTypes operands)
{
    // false is 0 and true is 1.
    if ((opcode == Opcode::ToNumber || opcode == Opcode::ToNumeric) && operands[0]->isBoolean())
        return Type::integer({ operands[0]->primitives & Primitive::False ? 0 : 1, operands[0]->primitives & Primitive::True ? 1 : 0 });
    bool areAllIntegers = std::ranges::all_of(operands, [](const Type* type) {
        return type->isInteger();
    });
    if (areAllIntegers) {
        if (std::optional<Type> result = integerResultOf(opcode, operands))
            return result;
    }
    if (!fractionFreeOperators.contains(opcode))
        return bitwiseResultBoundedByOneOperand(opcode, operands);
    bool areAllWithoutFraction = std::ranges::all_of(operands, [](const Type* type) {
        return type->isNumberWithoutFraction();
    });
    if (areAllWithoutFraction)
        return Type::fromPrimitives(Primitive::NumberWithoutFraction);
    return std::nullopt;
}

static Type naturalTypeOfOperands(const TypingContext& context, Opcode opcode, NaturalTable table, OperandTypes operands)
{
    RELEASE_ASSERT(operands.size() == numberOfOperandsOf(table));
    if (hasNever(operands))
        return Type::never();
    Vector<OperandClass, 2> classes;
    for (const Type* type : operands)
        classes.append(classOf(*type));
    Type upperBound = naturalTypeFrom(naturalTypeOf(table, classes.span()));
    if (!upperBound.isNumber())
        return upperBound;
    std::optional<Type> refined = numberResultOf(opcode, operands);
    if (!refined)
        return upperBound;
    ASSERT_UNUSED(context, refined->isSubsetOf(upperBound, context.program));
    return WTF::move(*refined);
}

Type naturalTypeOfValue(const TypingContext& context, const Value& value, NaturalTable table)
{
    Vector<const Type*, 2> operands = operandTypesOf(context, value, numberOfOperandsOf(table));
    return naturalTypeOfOperands(context, value.opcode, table, operands.span());
}

static Type toBooleanResultType(const TypingContext& context, const Type& operand, const Type& upperBound)
{
    if (truthyPartOf(operand, context.types).isNever())
        return Type::fromPrimitives(Primitive::False);
    if (falsyPartOf(operand, context.types).isNever())
        return Type::fromPrimitives(Primitive::True);
    return upperBound;
}

static bool mayBeZero(const Type& type)
{
    return type.primitives & Primitive::WholeNumber || type.integers.contains(0);
}

static Type strictEqualityResultType(const TypingContext& context, const Type& left, const Type& right, const Type& upperBound)
{
    bool mayBeEqual = left.overlaps(right, context.program) || (mayBeZero(left) && mayBeZero(right));
    if (!mayBeEqual)
        return Type::fromPrimitives(Primitive::False);
    if (left == right && left.isSingleValue() && !left.isNumber())
        return Type::fromPrimitives(Primitive::True);
    return upperBound;
}

static Type refineOperator(const TypingContext& context, Opcode opcode, OperandTypes operands, const Type& upperBound)
{
    if (hasNever(operands))
        return Type::never();
    switch (opcode) {
    case Opcode::URShift:
    case Opcode::ToNumber:
        return numberResultOf(opcode, operands).value_or(upperBound);
    case Opcode::ToBoolean:
        return toBooleanResultType(context, *operands[0], upperBound);
    case Opcode::StrictEq:
        return strictEqualityResultType(context, *operands[0], *operands[1], upperBound);
    default:
        return upperBound;
    }
}

static Type typeTestResultType(const TypingContext& context, const Type& operand, const Type& tested, const Type& upperBound)
{
    if (operand.isNever())
        return Type::never();
    if (operand.isSubsetOf(tested, context.program))
        return Type::fromPrimitives(Primitive::True);
    if (!operand.overlaps(tested, context.program))
        return Type::fromPrimitives(Primitive::False);
    return upperBound;
}

static Type quotientResultType(const Type& dividend, const Type& divisor, QuotientRounding rounding, const Type& upperBound)
{
    if (dividend.isNever() || divisor.isNever())
        return Type::never();
    if (!dividend.isInteger() || !divisor.isInteger())
        return upperBound;
    return integerQuotientType(dividend.integers, divisor.integers, rounding).value_or(upperBound);
}

static Type refineConversion(Opcode opcode, const Type& operand, const Type& upperBound)
{
    if (operand.isNever())
        return Type::never();
    switch (opcode) {
    case Opcode::ToInt32:
        return operand.isInt32() ? operand : upperBound;
    case Opcode::ToUint32:
        return operand.isInteger() && operand.integers.isSubsetOf(Range::uint32()) ? operand : upperBound;
    case Opcode::ToIntegerOrInfinity:
        return operand.isInteger() ? operand : Type::fromPrimitives(Primitive::NumberWithoutFraction);
    case Opcode::ToLength: {
        constexpr Range lengths { 0, maxExactInteger - 1 };
        if (!operand.isInteger())
            return Type::integer(lengths);
        return Type::integer({ std::clamp(operand.integers.minimum, lengths.minimum, lengths.maximum), std::clamp(operand.integers.maximum, lengths.minimum, lengths.maximum) });
    }
    case Opcode::LogicalNot:
        if (operand.hasOnlyPrimitives(Primitive::True))
            return Type::fromPrimitives(Primitive::False);
        if (operand.hasOnlyPrimitives(Primitive::False))
            return Type::fromPrimitives(Primitive::True);
        return upperBound;
    case Opcode::ToObject:
        return operand.primitives & ~Primitive::UndefinedOrNull ? upperBound : operand.withOnlyObjects();
    case Opcode::ToThis:
        return operand.isObject() ? operand : upperBound;
    default:
        return upperBound;
    }
}

static Type refinedTypeOf(const TypingContext& context, const Value& value, const Type& upperBound)
{
    if (!opcodesWithNaturalTypeRefinedByValue.contains(value.opcode))
        return upperBound;
    if (Value::opcodesWithForm.contains(value.opcode)) {
        Vector<const Type*, 2> operands = operandTypesOf(context, value, value.numberOfOperands());
        return refineOperator(context, value.opcode, operands.span(), upperBound);
    }

    switch (value.opcode) {
    case Opcode::GetTypedArrayElement: {
        if (!isTypedView(value.typedArrayType()))
            return upperBound;
        Type element = elementTypeOf(value.typedArrayType());
        return value.isInsideView() ? element : element.withPrimitives(Primitive::Undefined);
    }
    case Opcode::DivideIntegers:
        return quotientResultType(context.typeOf(value.dividendOperand()), context.typeOf(value.divisorOperand()), value.rounding(), upperBound);
    case Opcode::TypeTest:
        return typeTestResultType(context, context.typeOf(value.valueOperand()), context.types.at(value.testedType()), upperBound);
    case Opcode::ToInt32:
    case Opcode::ToUint32:
    case Opcode::ToIntegerOrInfinity:
    case Opcode::ToLength:
    case Opcode::LogicalNot:
    case Opcode::ToObject:
    case Opcode::ToThis:
        return refineConversion(value.opcode, context.typeOf(value.valueOperand()), upperBound);
    default:
        return upperBound;
    }
}

Type refineFixedNatural(const TypingContext& context, const Value& value, const Type& upperBound)
{
    Type refined = refinedTypeOf(context, value, upperBound);
    ASSERT(refined.isSubsetOf(upperBound, context.program));
    return refined;
}

namespace {

struct OperatorRules {
    using Result = Type;

    const TypingContext& context;
    OperandTypes operands;

    Type natural(const Value& value, NaturalTable table) { return naturalTypeOfOperands(context, value.opcode, table, operands); }
    Type natural(const Value& value, const NaturalType& upperBound) { return refineOperator(context, value.opcode, operands, naturalTypeFrom(upperBound)); }

    template<typename... Arguments> Type none(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type constant(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type sameAs(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type allocation(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type declared(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type declaredContents(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type declaredResult(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type declaredParameter(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type unknown(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type cast(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
    template<typename... Arguments> Type resultOperand(Arguments&&...) { RELEASE_ASSERT_NOT_REACHED(); }
};

} // anonymous namespace

Type computeOperatorResultType(const TypingContext& context, Opcode opcode, std::span<const Type> operandTypes)
{
    RELEASE_ASSERT(Value::opcodesWithForm.contains(opcode));
    RELEASE_ASSERT(operandTypes.size() == numberOfNamedOperandsOf(opcode));
    Vector<const Type*, 2> operands;
    for (const Type& type : operandTypes)
        operands.append(&type);
    OperatorRules rules { context, operands.span() };
    return applyResultRule(rules, Value(opcode), 0, ResultSide::Proven);
}

Type elementTypeOf(TypedArrayType type)
{
    switch (type) {
    case TypeInt8:
        return Type::integer({ -128, 127 });
    case TypeUint8:
    case TypeUint8Clamped:
        return Type::integer({ 0, 255 });
    case TypeInt16:
        return Type::integer({ -32768, 32767 });
    case TypeUint16:
        return Type::integer({ 0, 65535 });
    case TypeInt32:
        return Type::integer(Range::int32());
    case TypeUint32:
        return Type::integer(Range::uint32());
    case TypeFloat16:
    case TypeFloat32:
    case TypeFloat64:
        return Type::fromPrimitives(Primitive::Number);
    case TypeBigInt64:
    case TypeBigUint64:
        return Type::fromPrimitives(Primitive::BigInt);
    case NotTypedArray:
    case TypeDataView:
        return Type::anything();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct WidestNaturalRules {
    using Result = std::optional<Type>;

    Result natural(const Value&, const NaturalType& type) { return naturalTypeFrom(type); }
    Result natural(const Value&, NaturalTable table)
    {
        Vector<OperandClass, 2> operands;
        operands.fill(OperandClass::Any, numberOfOperandsOf(table));
        return naturalTypeFrom(naturalTypeOf(table, operands.span()));
    }
    Result none(auto&&...) { return std::nullopt; }
    Result constant(auto&&...) { return std::nullopt; }
    Result sameAs(auto&&...) { return std::nullopt; }
    Result allocation(auto&&...) { return std::nullopt; }
    Result declared(const Value&, LayoutID) { return Type::object(); }
    Result declared(auto&&...) { return std::nullopt; }
    Result declaredContents(auto&&...) { return std::nullopt; }
    Result declaredResult(auto&&...) { return std::nullopt; }
    Result declaredParameter(auto&&...) { return std::nullopt; }
    Result unknown(auto&&...) { return std::nullopt; }
    Result cast(auto&&...) { return std::nullopt; }
    Result resultOperand(auto&&...) { return std::nullopt; }
};

} // namespace

std::optional<Type> tryWidestNaturalTypeOf(const Value& value)
{
    WidestNaturalRules rules;
    return applyResultRule(rules, value, 0, ResultSide::Proven);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
