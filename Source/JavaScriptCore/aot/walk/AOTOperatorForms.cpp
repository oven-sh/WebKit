/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorForms.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTRepresentation.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <span>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr OpcodeSet arithmeticInIntegersForm {
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Mod, Opcode::Negate, Opcode::Increment, Opcode::Decrement
};
constexpr OpcodeSet arithmeticInCheckedIntegersForm = arithmeticInIntegersForm | OpcodeSet { Opcode::Div };
constexpr OpcodeSet numericOperatorsWithForm = arithmeticInCheckedIntegersForm | OpcodeSet { Opcode::Pow };
constexpr OpcodeSet bitwiseInBigIntsForm { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::BitNot };
constexpr OpcodeSet numberConversionsWithForm { Opcode::ToNumber, Opcode::ToNumeric };

constexpr OpcodeSet arithmeticOperatorsWithForm = numericOperatorsWithForm | allBitwiseOperators;
constexpr OpcodeSet comparisonsWithForm = allRelations | allEqualities;
constexpr OpcodeSet conversionsWithForm = numberConversionsWithForm | OpcodeSet { Opcode::ToBoolean, Opcode::ToString };
constexpr OpcodeSet operatorsWithForm = arithmeticOperatorsWithForm | comparisonsWithForm | conversionsWithForm;

static_assert(!arithmeticOperatorsWithForm.intersects(comparisonsWithForm));
static_assert(!arithmeticOperatorsWithForm.intersects(conversionsWithForm));
static_assert(!comparisonsWithForm.intersects(conversionsWithForm));

static_assert(std::ranges::all_of(allMembersOf<Opcode>, [](Opcode opcode) {
    return operatorsWithForm.contains(opcode) == PayloadWord::form(opcode).has_value();
}));

static_assert(std::ranges::all_of(allMembersOf<Opcode>, [](Opcode opcode) {
    return !operatorsWithForm.contains(opcode) || effectRuleOf(opcode) == EffectRule::Operator || declaredBoundOf(opcode).isPure();
}));

constexpr OpcodeSet operatorsInIntegersForm = arithmeticInIntegersForm | allBitwiseOperators | comparisonsWithForm | numberConversionsWithForm | OpcodeSet { Opcode::ToBoolean };
constexpr OpcodeSet operatorsInNumbersForm = arithmeticOperatorsWithForm | comparisonsWithForm | numberConversionsWithForm | OpcodeSet { Opcode::ToBoolean };
constexpr OpcodeSet operatorsInNumberAndValueForm = OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div } | comparisonsWithForm;
constexpr OpcodeSet operatorsInBitsForm = allEqualities | OpcodeSet { Opcode::ToBoolean };
constexpr OpcodeSet operatorsInBooleansForm = operatorsInBitsForm | numberConversionsWithForm;
constexpr OpcodeSet operatorsInBigIntsForm = numericOperatorsWithForm | bitwiseInBigIntsForm | comparisonsWithForm;
constexpr OpcodeSet operatorsInStringsForm = OpcodeSet { Opcode::Add } | comparisonsWithForm;
constexpr OpcodeSet operatorsInNullableNumberForm = OpcodeSet { Opcode::ToString } | numberConversionsWithForm;
constexpr OpcodeSet operatorsThatCallInNumbersForm = allBitwiseOperators | OpcodeSet { Opcode::Mod, Opcode::Pow };

struct OperandOfForm : Type {
    explicit OperandOfForm(const Type& type)
        : Type(type)
    {
    }

    bool isNumeric() const { return !isNever() && isNumberLike(); }
    bool isBigInt() const { return !isNever() && hasOnlyPrimitives(Primitive::BigInt); }
    bool isNullableNumber() const { return isNumberOrNullish() || isUndefinedOrNull(); }
    bool isObjectOrNullish() const { return !(primitives & ~Primitive::UndefinedOrNull) && isJavaScriptValue(); }
    bool isBooleanOrNullish() const { return hasOnlyPrimitives(Primitive::Boolean | Primitive::UndefinedOrNull); }
    bool isSymbolOrNullish() const { return hasOnlyPrimitives(Primitive::AnySymbol | Primitive::UndefinedOrNull); }
    bool isSymbol() const { return primitives & Primitive::AnySymbol && hasOnlyPrimitives(Primitive::AnySymbol); }
    bool isPrimitiveValue() const { return hasOnlyPrimitives(Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString | Primitive::AnySymbol | Primitive::BigInt); }
    bool isStringOrUndefined() const { return primitives & Primitive::AnyString && hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined); }
    bool mayBeString() const { return primitives & Primitive::AnyString; }
    bool mayBeMarker() const { return primitives & (Primitive::Absent | Primitive::Unspecified); }
};

struct OperandsOfForm {
    using Question = bool (OperandOfForm::*)() const;

    bool areBoth(Question question) const { return (left.*question)() && (right.*question)(); }
    bool isEither(Question question) const { return (left.*question)() || (right.*question)(); }
    bool mayBothBe(uint16_t primitives) const { return left.primitives & primitives && right.primitives & primitives; }

    bool canCompareBits() const
    {
        return areBoth(&Type::isJavaScriptValue) && !mayBothBe(Primitive::Number) && !mayBothBe(Primitive::AnyString) && !mayBothBe(Primitive::BigInt);
    }

    OperandOfForm left;
    OperandOfForm right;
};

} // anonymous namespace

static void checkOperandsOfForm(Opcode opcode, OperatorTypes operands)
{
    RELEASE_ASSERT(operands.left);
    RELEASE_ASSERT(!!operands.right == (numberOfNamedOperandsOf(opcode) == 2));
}

static bool hasNeverAmongOperandsOfForm(OperatorTypes operands)
{
    return isNeverType(operands.left) || isNeverType(operands.right);
}

static OperandsOfForm operandsOfForm(const TypeArena& types, OperatorTypes operands)
{
    Type left = types.at(operands.left);
    return { OperandOfForm(left), OperandOfForm(operands.right ? types.at(operands.right) : left) };
}

bool hasOperatorForm(Opcode opcode)
{
    return operatorsWithForm.contains(opcode);
}

bool canCompareBits(const TypeArena& types, TypeID left, TypeID right)
{
    RELEASE_ASSERT(left && right);
    return operandsOfForm(types, { left, right }).canCompareBits();
}

static std::optional<Range> rangeInIntegersForm(CheckedInt64 minimum, CheckedInt64 maximum)
{
    if (minimum.hasOverflowed() || maximum.hasOverflowed())
        return std::nullopt;
    return Range { minimum.value(), maximum.value() };
}

static std::optional<Range> productsInIntegersForm(Range left, Range right)
{
    bool mayBeNegativeZero = (left.minimum < 0 && right.contains(0)) || (right.minimum < 0 && left.contains(0));
    if (mayBeNegativeZero)
        return std::nullopt;
    Range products = Range::empty();
    for (int64_t multiplicand : { left.minimum, left.maximum }) {
        for (int64_t multiplier : { right.minimum, right.maximum }) {
            CheckedInt64 product = CheckedInt64(multiplicand) * multiplier;
            if (product.hasOverflowed())
                return std::nullopt;
            products = products.unionWith(Range::one(product.value()));
        }
    }
    return products;
}

static std::optional<Range> remaindersInIntegersForm(Range dividends, Range divisors)
{
    if (dividends.minimum < 0 || divisors.contains(0))
        return std::nullopt;
    int64_t largestDivisor = std::max(std::abs(divisors.minimum), std::abs(divisors.maximum));
    return Range { 0, std::min(dividends.maximum, largestDivisor - 1) };
}

static std::optional<Range> resultsInIntegersForm(Opcode opcode, Range left, Range right)
{
    if (opcode == Opcode::Add)
        return rangeInIntegersForm(CheckedInt64(left.minimum) + right.minimum, CheckedInt64(left.maximum) + right.maximum);
    if (opcode == Opcode::Sub)
        return rangeInIntegersForm(CheckedInt64(left.minimum) - right.maximum, CheckedInt64(left.maximum) - right.minimum);
    if (opcode == Opcode::Increment)
        return rangeInIntegersForm(CheckedInt64(left.minimum) + 1, CheckedInt64(left.maximum) + 1);
    if (opcode == Opcode::Decrement)
        return rangeInIntegersForm(CheckedInt64(left.minimum) - 1, CheckedInt64(left.maximum) - 1);
    if (opcode == Opcode::Negate) {
        if (left.contains(0))
            return std::nullopt;
        return rangeInIntegersForm(CheckedInt64(0) - left.maximum, CheckedInt64(0) - left.minimum);
    }
    if (opcode == Opcode::Mul)
        return productsInIntegersForm(left, right);
    RELEASE_ASSERT(opcode == Opcode::Mod);
    return remaindersInIntegersForm(left, right);
}

static bool fitsIntegersForm(Opcode opcode, const OperandsOfForm& operands, const Type& result)
{
    if (!result.isInteger() || !arithmeticInIntegersForm.contains(opcode))
        return false;
    std::optional<Range> results = resultsInIntegersForm(opcode, operands.left.integers, operands.right.integers);
    return results && results->isSubsetOf(result.integers);
}

template<typename ResultOfOperator>
static OperatorForm chooseNumericForm(Opcode opcode, const OperandsOfForm& operands, const ResultOfOperator& result)
{
    if (operands.areBoth(&Type::isInteger)) {
        if (fitsIntegersForm(opcode, operands, result()))
            return OperatorForm::Integers;
        if (isLegalOperatorForm(opcode, OperatorForm::CheckedIntegers))
            return OperatorForm::CheckedIntegers;
    }
    if (operands.areBoth(&OperandOfForm::isNumeric))
        return OperatorForm::Numbers;
    if (operands.areBoth(&OperandOfForm::isBigInt))
        return OperatorForm::BigInts;
    if (operands.isEither(&Type::isNumber) && result().isNumber() && isLegalOperatorForm(opcode, OperatorForm::NumberAndValue))
        return OperatorForm::NumberAndValue;
    return OperatorForm::Generic;
}

static bool isEmptyStringPlusPossibleString(const Program& program, const OperandOfForm& text, const OperandOfForm& other)
{
    if (!text.hasOnlyPrimitives(Primitive::LiteralString))
        return false;
    StringID literal = singleStringLiteralOf(text);
    return literal && literal == program.strings.emptyString() && other.mayBeString() && !other.mayBeMarker();
}

static OperatorForm chooseConcatenationForm(const Program& program, const OperandsOfForm& operands)
{
    if (operands.isEither(&Type::hasObjects)) {
        if (isEmptyStringPlusPossibleString(program, operands.left, operands.right) || isEmptyStringPlusPossibleString(program, operands.right, operands.left))
            return OperatorForm::MayBeString;
        return OperatorForm::Generic;
    }
    if (operands.areBoth(&Type::isAnyString))
        return OperatorForm::Strings;
    if (operands.isEither(&Type::isAnyString))
        return OperatorForm::StringAndPrimitive;
    if (operands.areBoth(&Type::isAnyStringOrNullish))
        return OperatorForm::NullableStrings;
    if (operands.isEither(&Type::isAnyStringOrNullish))
        return OperatorForm::NullableStringAndPrimitive;
    return OperatorForm::Generic;
}

template<typename ResultOfOperator>
static OperatorForm chooseAdditionForm(const Program& program, const OperandsOfForm& operands, const ResultOfOperator& result)
{
    OperatorForm numericForm = chooseNumericForm(Opcode::Add, operands, result);
    if (numericForm != OperatorForm::Generic)
        return numericForm;
    return chooseConcatenationForm(program, operands);
}

static ALWAYS_INLINE OperatorForm chooseBitwiseForm(Opcode opcode, const OperandsOfForm& operands)
{
    if (operands.areBoth(&Type::isInt32))
        return OperatorForm::Integers;
    if (operands.areBoth(&OperandOfForm::isNumeric))
        return OperatorForm::Numbers;
    if (operands.areBoth(&OperandOfForm::isBigInt) && isLegalOperatorForm(opcode, OperatorForm::BigInts))
        return OperatorForm::BigInts;
    return OperatorForm::Generic;
}

template<typename ResultOfOperator>
static OperatorForm chooseArithmeticFormBy(const Program& program, const TypeArena& types, Opcode opcode, OperatorTypes operandTypes, const ResultOfOperator& result)
{
    checkOperandsOfForm(opcode, operandTypes);
    if (hasNeverAmongOperandsOfForm(operandTypes))
        return OperatorForm::Generic;

    OperandsOfForm operands = operandsOfForm(types, operandTypes);
    if (allBitwiseOperators.contains(opcode))
        return chooseBitwiseForm(opcode, operands);
    if (opcode == Opcode::Add)
        return chooseAdditionForm(program, operands, result);
    return chooseNumericForm(opcode, operands, result);
}

OperatorForm chooseArithmeticForm(const Program& program, const TypeArena& types, Opcode opcode, OperatorTypes operandTypes, TypeID result)
{
    RELEASE_ASSERT(arithmeticOperatorsWithForm.contains(opcode) && result);
    return chooseArithmeticFormBy(program, types, opcode, operandTypes, [&] {
        return types.at(result);
    });
}

static OperatorForm chooseRelationForm(const OperandsOfForm& operands)
{
    if (operands.areBoth(&Type::isInteger))
        return OperatorForm::Integers;
    if (operands.areBoth(&OperandOfForm::isNumeric))
        return OperatorForm::Numbers;
    if (operands.areBoth(&OperandOfForm::isBigInt))
        return OperatorForm::BigInts;
    if (operands.isEither(&Type::isNumber))
        return OperatorForm::NumberAndValue;
    if (operands.areBoth(&Type::isAnyString))
        return OperatorForm::Strings;
    if (operands.areBoth(&OperandOfForm::isStringOrUndefined))
        return OperatorForm::NullableStrings;
    return OperatorForm::Generic;
}

static ALWAYS_INLINE bool isInt32AndBoxedValue(const OperandOfForm& integer, const OperandOfForm& other)
{
    return integer.isInt32() && representationOf(other) == Representation::JSValue;
}

static ALWAYS_INLINE OperatorForm chooseStrictEqualityForm(const OperandsOfForm& operands)
{
    if (operands.areBoth(&Type::isInteger))
        return OperatorForm::Integers;
    bool mayBeNumber = (operands.left.primitives | operands.right.primitives) & Primitive::Number;
    if (mayBeNumber && operands.areBoth(&OperandOfForm::isNullableNumber))
        return OperatorForm::Numbers;
    if (operands.areBoth(&Type::isBoolean))
        return OperatorForm::Booleans;
    if (operands.canCompareBits())
        return OperatorForm::Bits;
    if (operands.areBoth(&OperandOfForm::isBigInt))
        return OperatorForm::BigInts;
    if (operands.areBoth(&Type::isAnyString))
        return OperatorForm::Strings;
    if (operands.areBoth(&Type::isAnyStringOrNullish))
        return OperatorForm::NullableStrings;
    if (operands.isEither(&Type::isAnyString))
        return OperatorForm::StringAndValue;
    if (isInt32AndBoxedValue(operands.left, operands.right) || isInt32AndBoxedValue(operands.right, operands.left))
        return OperatorForm::Int32AndValue;
    if (operands.isEither(&Type::isNumber))
        return OperatorForm::NumberAndValue;
    return OperatorForm::Generic;
}

static OperatorForm chooseLooseEqualityForm(const OperandsOfForm& operands)
{
    bool comparesWithoutConversion = operands.areBoth(&Type::isNumber) || operands.areBoth(&Type::isAnyString) || operands.areBoth(&Type::isBoolean)
        || operands.areBoth(&Type::isObject) || operands.areBoth(&OperandOfForm::isBigInt);
    if (comparesWithoutConversion)
        return chooseStrictEqualityForm(operands);
    if (operands.areBoth(&OperandOfForm::isNullableNumber))
        return OperatorForm::Numbers;
    if (operands.areBoth(&Type::isAnyStringOrNullish))
        return OperatorForm::NullableStrings;
    if (operands.areBoth(&OperandOfForm::isObjectOrNullish) || operands.areBoth(&OperandOfForm::isBooleanOrNullish) || operands.areBoth(&OperandOfForm::isSymbolOrNullish))
        return OperatorForm::Bits;
    if (operands.isEither(&Type::isUndefinedOrNull) && operands.areBoth(&Type::isJavaScriptValue) && !operands.isEither(&OperandOfForm::mayBeMarker))
        return OperatorForm::Bits;
    if (operands.isEither(&OperandOfForm::isSymbol) && operands.areBoth(&OperandOfForm::isPrimitiveValue) && operands.canCompareBits())
        return OperatorForm::Bits;
    return OperatorForm::Generic;
}

ALWAYS_INLINE_INTO_OTHER_FILES OperatorForm chooseComparisonForm(const Program&, const TypeArena& types, Opcode opcode, OperatorTypes operandTypes)
{
    RELEASE_ASSERT(comparisonsWithForm.contains(opcode));
    checkOperandsOfForm(opcode, operandTypes);
    if (hasNeverAmongOperandsOfForm(operandTypes))
        return OperatorForm::Generic;

    OperandsOfForm operands = operandsOfForm(types, operandTypes);
    if (allRelations.contains(opcode))
        return chooseRelationForm(operands);
    return opcode == Opcode::StrictEq ? chooseStrictEqualityForm(operands) : chooseLooseEqualityForm(operands);
}

static OperatorForm chooseToNumberForm(const OperandOfForm& operand)
{
    if (operand.isBoolean())
        return OperatorForm::Booleans;
    if (operand.isInteger())
        return OperatorForm::Integers;
    if (operand.isNullableNumber())
        return OperatorForm::Numbers;
    if (operand.isNumeric())
        return OperatorForm::NullableNumber;
    return OperatorForm::Generic;
}

static OperatorForm chooseToBooleanForm(const OperandOfForm& operand)
{
    if (operand.isBoolean())
        return OperatorForm::Booleans;
    if (operand.isInteger())
        return OperatorForm::Integers;
    if (operand.isNumberOrNullish())
        return OperatorForm::Numbers;
    bool holdsNumbersAndBigInts = operand.primitives & Primitive::Number && operand.primitives & Primitive::BigInt;
    return holdsNumbersAndBigInts ? OperatorForm::Generic : OperatorForm::Bits;
}

static OperatorForm chooseToStringForm(const OperandOfForm& operand)
{
    if (operand.isAnyString())
        return OperatorForm::String;
    if (operand.isNumber())
        return OperatorForm::Number;
    if (operand.isAnyStringOrNullish())
        return OperatorForm::NullableString;
    if (operand.mayBeString() && !operand.mayBeMarker()) {
        bool mayBeNumber = !operand.hasObjects() && operand.primitives & Primitive::Number;
        return mayBeNumber ? OperatorForm::MayBeStringOrNumber : OperatorForm::MayBeString;
    }
    if (operand.primitives & Primitive::Number && operand.hasOnlyPrimitives(Primitive::Number | Primitive::BigInt))
        return OperatorForm::MayBeStringOrNumber;
    if (operand.isNumberOrNullish())
        return OperatorForm::NullableNumber;
    return OperatorForm::Generic;
}

OperatorForm chooseConversionForm(const Program&, const TypeArena& types, Opcode opcode, TypeID operandType)
{
    RELEASE_ASSERT(conversionsWithForm.contains(opcode) && operandType);
    if (isNeverType(operandType))
        return OperatorForm::Generic;

    OperandOfForm operand(types.at(operandType));
    if (numberConversionsWithForm.contains(opcode))
        return chooseToNumberForm(operand);
    return opcode == Opcode::ToBoolean ? chooseToBooleanForm(operand) : chooseToStringForm(operand);
}

OperatorForm chooseFormOfOperator(const Program& program, TypeArena& types, Opcode opcode, OperatorTypes operandTypes)
{
    RELEASE_ASSERT(hasOperatorForm(opcode));
    checkOperandsOfForm(opcode, operandTypes);
    if (arithmeticOperatorsWithForm.contains(opcode)) {
        std::array<TypeID, 2> leftAndRight { operandTypes.left, operandTypes.right };
        std::span<const TypeID> typesByOperandIndex = std::span<const TypeID>(leftAndRight).first(numberOfNamedOperandsOf(opcode));
        TypingContext typing { program, types, PositionTypes::knowingNothing() };
        OperatorForm form = chooseArithmeticFormBy(program, types, opcode, operandTypes, [&] {
            TypeID result = resultTypeOfOperator(typing, opcode, typesByOperandIndex);
            RELEASE_ASSERT(result);
            return types.at(result);
        });
        RELEASE_ASSERT(isLegalOperatorForm(opcode, form));
        return form;
    }
    OperatorForm form = comparisonsWithForm.contains(opcode) ? chooseComparisonForm(program, types, opcode, operandTypes) : chooseConversionForm(program, types, opcode, operandTypes.left);
    RELEASE_ASSERT(isLegalOperatorForm(opcode, form));
    return form;
}

OperatorChoice chooseOperatorForm(const Program& program, TypeArena& types, Opcode opcode, OperatorTypes operandTypes)
{
    RELEASE_ASSERT(hasOperatorForm(opcode));
    checkOperandsOfForm(opcode, operandTypes);

    std::array<TypeID, 2> leftAndRight { operandTypes.left, operandTypes.right };
    std::span<const TypeID> typesByOperandIndex = std::span<const TypeID>(leftAndRight).first(numberOfNamedOperandsOf(opcode));
    TypingContext typing { program, types, PositionTypes::knowingNothing() };

    OperatorChoice choice;
    choice.result = resultTypeOfOperator(typing, opcode, typesByOperandIndex);
    if (arithmeticOperatorsWithForm.contains(opcode))
        choice.form = chooseArithmeticForm(program, types, opcode, operandTypes, choice.result);
    else if (comparisonsWithForm.contains(opcode))
        choice.form = chooseComparisonForm(program, types, opcode, operandTypes);
    else
        choice.form = chooseConversionForm(program, types, opcode, operandTypes.left);
    RELEASE_ASSERT(isLegalOperatorForm(opcode, choice.form));

    Effects effectsByTypes = effectRuleOf(opcode) == EffectRule::Operator ? effectsOfOperator(typing, opcode, typesByOperandIndex, choice.result) : declaredBoundOf(opcode);
    choice.effects = effectsByTypes.intersectedWith(effectsBoundOf(opcode, choice.form));
    return choice;
}

ALWAYS_INLINE_INTO_OTHER_FILES bool isLegalOperatorForm(Opcode opcode, OperatorForm form)
{
    switch (form) {
    case OperatorForm::Unselected:
    case OperatorForm::NumberByOperation:
        return false;
    case OperatorForm::Generic:
        return hasOperatorForm(opcode);
    case OperatorForm::Integers:
        return operatorsInIntegersForm.contains(opcode);
    case OperatorForm::CheckedIntegers:
        return arithmeticInCheckedIntegersForm.contains(opcode);
    case OperatorForm::Numbers:
        return operatorsInNumbersForm.contains(opcode);
    case OperatorForm::NumberAndValue:
        return operatorsInNumberAndValueForm.contains(opcode);
    case OperatorForm::Bits:
        return operatorsInBitsForm.contains(opcode);
    case OperatorForm::Booleans:
        return operatorsInBooleansForm.contains(opcode);
    case OperatorForm::BigInts:
        return operatorsInBigIntsForm.contains(opcode);
    case OperatorForm::Strings:
    case OperatorForm::NullableStrings:
        return operatorsInStringsForm.contains(opcode);
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
        return allEqualities.contains(opcode);
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStringAndPrimitive:
        return opcode == Opcode::Add;
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeStringOrNumber:
        return opcode == Opcode::ToString;
    case OperatorForm::MayBeString:
        return opcode == Opcode::ToString || opcode == Opcode::Add;
    case OperatorForm::NullableNumber:
        return operatorsInNullableNumberForm.contains(opcode);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool mayCallRuntime(Opcode opcode, OperatorForm form)
{
    switch (form) {
    case OperatorForm::Integers:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::String:
        return false;
    case OperatorForm::Int32AndValue:
        return opcode == Opcode::Eq;
    case OperatorForm::Numbers:
        return operatorsThatCallInNumbersForm.contains(opcode);
    case OperatorForm::NullableNumber:
        return !numberConversionsWithForm.contains(opcode);
    case OperatorForm::Unselected:
    case OperatorForm::Generic:
    case OperatorForm::NumberAndValue:
    case OperatorForm::BigInts:
    case OperatorForm::Strings:
    case OperatorForm::StringAndValue:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Effects effectsOfFormThatTakesAnyValue(Opcode opcode)
{
    Effects effects = declaredBoundOf(opcode);
    for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(opcode); ++operandIndex) {
        if (!protocolsOf(opcode, operandIndex).isEmpty())
            effects.merge(Effects::unknownCode());
    }
    return effects;
}

Effects effectsBoundOf(Opcode opcode, OperatorForm form)
{
    switch (form) {
    case OperatorForm::Integers:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::Numbers:
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::String:
        return Effects::none();
    case OperatorForm::NullableNumber:
        return numberConversionsWithForm.contains(opcode) ? Effects::none() : declaredBoundOf(opcode);
    case OperatorForm::BigInts:
        return opcode == Opcode::StrictEq ? Effects::none() : declaredBoundOf(opcode);
    case OperatorForm::NumberAndValue:
    case OperatorForm::Int32AndValue:
        return opcode == Opcode::StrictEq ? Effects::none() : effectsOfFormThatTakesAnyValue(opcode);
    case OperatorForm::Strings:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
        return declaredBoundOf(opcode);
    case OperatorForm::Unselected:
    case OperatorForm::Generic:
    case OperatorForm::StringAndValue:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        return effectsOfFormThatTakesAnyValue(opcode);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
