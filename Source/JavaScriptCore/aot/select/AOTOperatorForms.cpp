/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorForms.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfRules.h"
#include "AOTInductionVariables.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr OpcodeSet integerArithmeticOperators {
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Mod, Opcode::Negate, Opcode::Increment, Opcode::Decrement
};
constexpr OpcodeSet checkedIntegerOperators = integerArithmeticOperators | OpcodeSet { Opcode::Div };
constexpr OpcodeSet numberArithmeticOperators = checkedIntegerOperators | OpcodeSet { Opcode::Pow };
constexpr OpcodeSet bigIntBitwiseOperators { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::BitNot };
constexpr OpcodeSet bitwiseOperators = bigIntBitwiseOperators | OpcodeSet { Opcode::URShift };
constexpr OpcodeSet equalityOperators { Opcode::Eq, Opcode::StrictEq };
constexpr OpcodeSet numberConversionOperators { Opcode::ToNumber, Opcode::ToNumeric };

constexpr OpcodeSet arithmeticOperators = numberArithmeticOperators | bitwiseOperators;
constexpr OpcodeSet comparisonOperators = relationalOperators | equalityOperators;
constexpr OpcodeSet conversionOperators = numberConversionOperators | OpcodeSet { Opcode::ToBoolean, Opcode::ToString };

static_assert((arithmeticOperators | comparisonOperators | conversionOperators) == Value::opcodesWithForm);
static_assert(!arithmeticOperators.intersects(comparisonOperators));
static_assert(!arithmeticOperators.intersects(conversionOperators));
static_assert(!comparisonOperators.intersects(conversionOperators));

constexpr OpcodeSet operatorsOnIntegers = integerArithmeticOperators | bitwiseOperators | comparisonOperators | numberConversionOperators | OpcodeSet { Opcode::ToBoolean };
constexpr OpcodeSet operatorsOnNumbers = arithmeticOperators | comparisonOperators | numberConversionOperators | OpcodeSet { Opcode::ToBoolean };
constexpr OpcodeSet operatorsOnNumberAndValue = OpcodeSet { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div } | comparisonOperators;
constexpr OpcodeSet operatorsOnBits = equalityOperators | OpcodeSet { Opcode::ToBoolean };
constexpr OpcodeSet operatorsOnBigInts = numberArithmeticOperators | bigIntBitwiseOperators | comparisonOperators;
constexpr OpcodeSet operatorsOnStrings = OpcodeSet { Opcode::Add } | comparisonOperators;
constexpr OpcodeSet operatorsOnNullableStrings = OpcodeSet { Opcode::Add } | equalityOperators;
constexpr OpcodeSet operatorsOnNullableNumber = OpcodeSet { Opcode::ToString } | numberConversionOperators;

constexpr OpcodeSet operatorsCallingOnNumbers = bitwiseOperators | OpcodeSet { Opcode::Mod, Opcode::Pow };

static_assert(std::ranges::all_of(allMembersOf<Opcode>, [](Opcode opcode) {
    return !hasOperatorForm(opcode) || effectRuleOf(opcode) == EffectRule::Operator || declaredBoundOf(opcode).isPure();
}));

} // anonymous namespace

static bool mayBothBe(uint16_t primitives, const Type& left, const Type& right)
{
    return left.primitives & primitives && right.primitives & primitives;
}

bool isNumericOperand(const Type& type)
{
    return !type.isNever() && type.isNumberLike();
}

bool canCompareBits(const Type& left, const Type& right)
{
    return !mayBothBe(Primitive::Number, left, right) && !mayBothBe(Primitive::AnyString, left, right) && !mayBothBe(Primitive::BigInt, left, right);
}

static OperatorTypes typesOf(const OperatorSite& site, const Value& value, const Type& result)
{
    const Type& left = site.typing.typeOf(value.operand(0));
    const Type& right = value.numberOfOperands() > 1 ? site.typing.typeOf(value.operand(1)) : left;
    return { left, right, result };
}

static OperatorForm chooseFormByTypes(const OperatorSite& site, Opcode opcode, const OperatorTypes& types)
{
    if (types.left.isNever() || types.right.isNever())
        return OperatorForm::Generic;
    if (arithmeticOperators.contains(opcode))
        return chooseArithmeticForm(site, opcode, types);
    if (comparisonOperators.contains(opcode))
        return chooseComparisonForm(site, opcode, types);
    RELEASE_ASSERT(conversionOperators.contains(opcode));
    return chooseConversionForm(site, opcode, types);
}

static EffectsOnEdges effectsForOperandTypes(const OperatorSite& site, Opcode opcode, std::span<const Type> operandTypes, const Type& result)
{
    if (effectRuleOf(opcode) != EffectRule::Operator)
        return EffectsOnEdges::none();
    return effectsOfOperator(site.effects, opcode, operandTypes, result);
}

static void checkChoice(Opcode opcode, const OperatorChoice& choice)
{
    RELEASE_ASSERT(isLegalOperatorForm(opcode, choice.form));
    RELEASE_ASSERT(choice.form == OperatorForm::Generic || effectsBoundOf(opcode, choice.form).isSubsetOf(choice.effects.onAnyEdge()));
}

OperatorChoice chooseOperatorForm(const OperatorSite& site, const Value& value)
{
    RELEASE_ASSERT(hasOperatorForm(value.opcode));

    OperatorChoice choice;
    choice.result = computeResultType(site.typing, value);
    choice.effects = effectsOf(site.effects, site.typing.function, value);
    choice.form = chooseFormByTypes(site, value.opcode, typesOf(site, value, choice.result));
    checkChoice(value.opcode, choice);
    return choice;
}

OperatorChoice chooseOperatorFormFor(const OperatorSite& site, Opcode opcode, std::span<const Type> operandTypes)
{
    RELEASE_ASSERT(hasOperatorForm(opcode));
    RELEASE_ASSERT(operandTypes.size() == 1 || operandTypes.size() == 2);

    OperatorChoice choice;
    choice.result = computeOperatorResultType(site.typing, opcode, operandTypes);
    choice.effects = effectsForOperandTypes(site, opcode, operandTypes, choice.result);
    choice.form = chooseFormByTypes(site, opcode, { operandTypes.front(), operandTypes.back(), choice.result });
    checkChoice(opcode, choice);
    return choice;
}

OperatorForm chooseArithmeticForm(const OperatorSite& site, const Value& value)
{
    Type result = computeResultType(site.typing, value);
    return chooseArithmeticForm(site, value.opcode, typesOf(site, value, result));
}

OperatorForm chooseComparisonForm(const OperatorSite& site, const Value& value)
{
    Type result = computeResultType(site.typing, value);
    return chooseComparisonForm(site, value.opcode, typesOf(site, value, result));
}

OperatorForm chooseConversionForm(const OperatorSite& site, const Value& value)
{
    Type result = computeResultType(site.typing, value);
    return chooseConversionForm(site, value.opcode, typesOf(site, value, result));
}

bool isLegalOperatorForm(Opcode opcode, OperatorForm form)
{
    switch (form) {
    case OperatorForm::Unselected:
        return false;
    case OperatorForm::Generic:
        return hasOperatorForm(opcode);
    case OperatorForm::Integers:
        return operatorsOnIntegers.contains(opcode);
    case OperatorForm::CheckedIntegers:
        return checkedIntegerOperators.contains(opcode);
    case OperatorForm::Numbers:
        return operatorsOnNumbers.contains(opcode);
    case OperatorForm::NumberAndValue:
        return operatorsOnNumberAndValue.contains(opcode);
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
        return operatorsOnBits.contains(opcode);
    case OperatorForm::BigInts:
        return operatorsOnBigInts.contains(opcode);
    case OperatorForm::Strings:
        return operatorsOnStrings.contains(opcode);
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
        return equalityOperators.contains(opcode);
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStringAndPrimitive:
        return opcode == Opcode::Add;
    case OperatorForm::NullableStrings:
        return operatorsOnNullableStrings.contains(opcode);
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        return opcode == Opcode::ToString;
    case OperatorForm::NullableNumber:
        return operatorsOnNullableNumber.contains(opcode);
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
        return operatorsCallingOnNumbers.contains(opcode);
    case OperatorForm::NullableNumber:
        return !numberConversionOperators.contains(opcode);
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

Effects effectsBoundOf(Opcode opcode, OperatorForm form)
{
    if (form == OperatorForm::Unselected || form == OperatorForm::Generic)
        return Effects::unknownCode();
    if (!mayCallRuntime(opcode, form) || form == OperatorForm::Numbers)
        return Effects::none();
    if (opcode == Opcode::StrictEq && (form == OperatorForm::BigInts || form == OperatorForm::NumberAndValue))
        return Effects::none();
    return declaredBoundOf(opcode);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
