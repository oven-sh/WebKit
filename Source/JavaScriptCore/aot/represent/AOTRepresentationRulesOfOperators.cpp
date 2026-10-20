/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationRules.h"

#if ENABLE(AOT)

#include "AOTOperatorForms.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet bitwiseOperators {
    Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift, Opcode::BitNot
};
static constexpr OpcodeSet testOperators {
    Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::Eq, Opcode::StrictEq, Opcode::ToBoolean
};
static_assert((bitwiseOperators | testOperators | Value::opcodesWithForm) == Value::opcodesWithForm);
static_assert(!bitwiseOperators.intersects(testOperators));

namespace {

struct OperatorContext {
    const Program& program;
    const Function& function;
    const Value& value;

    const Type& operandType(uint32_t operandIndex) const { return program.typeOf(function, value.operand(operandIndex)); }
    const Type& resultType() const { return program.at(value.type); }
    bool isBitwise() const { return bitwiseOperators.contains(value.opcode); }
};

} // anonymous namespace

static Representation integerWidthOf(const OperatorContext& operation)
{
    const Type& result = operation.resultType();
    bool fitsInt32 = !result.isInteger() || result.isInt32();
    operation.value.forEachChild([&](ValueID operand) {
        fitsInt32 = fitsInt32 && operation.program.typeOf(operation.function, operand).isInt32();
    });
    return fitsInt32 ? Representation::Int32 : Representation::Int64;
}

static Representation numberOrOptionalOf(const Type& type)
{
    if (type.isNumber())
        return Representation::Double;
    RELEASE_ASSERT(isLegalRepresentation(type, Representation::OptionalDouble));
    return Representation::OptionalDouble;
}

static Representation wantedByMixedOperands(const OperatorContext& operation, uint32_t operandIndex, bool (Type::*hasUnboxedKind)() const, Representation unboxed)
{
    uint32_t left = declaredPosition(OperandPosition::left(operation.value.opcode));
    uint32_t right = declaredPosition(OperandPosition::right(operation.value.opcode));
    uint32_t unboxedIndex = (operation.operandType(left).*hasUnboxedKind)() ? left : right;
    return operandIndex == unboxedIndex ? unboxed : Representation::JSValue;
}

static Representation wantedByBits(const OperatorContext& operation, uint32_t operandIndex)
{
    if (operation.value.opcode == Opcode::ToBoolean) {
        // Whether a reference is a cell or a marker shows in every form of it.
        const Type& type = operation.operandType(operandIndex);
        return isLegalRepresentation(type, Representation::Cell32) ? Representation::Unchosen : defaultRepresentationOf(type);
    }

    bool comparesCells = true;
    operation.value.forEachChild([&](ValueID operand) {
        comparesCells = comparesCells && defaultRepresentationOf(operation.program.typeOf(operation.function, operand)) == Representation::Cell;
    });
    return comparesCells ? Representation::Cell : Representation::JSValue;
}

static bool comparesLiteralStringWords(const OperatorContext& operation)
{
    if (operation.value.opcode != Opcode::StrictEq && operation.value.opcode != Opcode::Eq)
        return false;
    bool areWords = true;
    operation.value.forEachChild([&](ValueID operand) {
        areWords = areWords && defaultRepresentationOf(operation.program.typeOf(operation.function, operand)) == Representation::LiteralString;
    });
    return areWords;
}

static Representation unboxedNumberRepresentationOf(const Type& type)
{
    return type.isInteger() ? integerRepresentationOf(type) : Representation::Double;
}

Representation wantedByConcatenation(const Type& part)
{
    if (part.isAnyString())
        return Representation::Cell;
    return part.isNumber() ? unboxedNumberRepresentationOf(part) : Representation::JSValue;
}

Representation wantedByOperator(const Program& program, const Function& function, const Value& user, uint32_t operandIndex)
{
    OperatorForm form = user.form();
    RELEASE_ASSERT(isLegalOperatorForm(user.opcode, form));

    OperatorContext operation { program, function, user };
    const Type& type = operation.operandType(operandIndex);
    switch (form) {
    case OperatorForm::Generic:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        return Representation::JSValue;
    case OperatorForm::NullableStrings:
        return comparesLiteralStringWords(operation) ? Representation::LiteralString : Representation::JSValue;
    case OperatorForm::Integers:
        return operation.isBitwise() ? Representation::Int32 : integerWidthOf(operation);
    case OperatorForm::CheckedIntegers:
        return integerWidthOf(operation);
    case OperatorForm::Numbers:
        // ToInt32 of an integer is its low word, and so is ToInt32 of undefined and null in the form that holds them beside integers.
        if (operation.isBitwise() && user.opcode != Opcode::BitNot) {
            if (type.isInteger())
                return integerRepresentationOf(type);
            if (defaultRepresentationOf(type) == Representation::OptionalInt64)
                return Representation::OptionalInt64;
        }
        return numberOrOptionalOf(type);
    case OperatorForm::NumberAndValue:
        return wantedByMixedOperands(operation, operandIndex, &Type::isNumber, Representation::Double);
    case OperatorForm::Bits:
        return wantedByBits(operation, operandIndex);
    case OperatorForm::Booleans:
        return Representation::Boolean;
    case OperatorForm::Strings:
        return comparesLiteralStringWords(operation) ? Representation::LiteralString : Representation::Cell;
    case OperatorForm::BigInts:
    case OperatorForm::String:
        return Representation::Cell;
    case OperatorForm::StringAndValue:
        return wantedByMixedOperands(operation, operandIndex, &Type::isAnyString, Representation::Cell);
    case OperatorForm::Int32AndValue:
        return wantedByMixedOperands(operation, operandIndex, &Type::isInt32, Representation::Int32);
    case OperatorForm::StringAndPrimitive:
        return wantedByConcatenation(type);
    case OperatorForm::NullableStringAndPrimitive:
        return type.isAnyString() ? Representation::Cell : Representation::JSValue;
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
        return unboxedNumberRepresentationOf(type);
    case OperatorForm::NullableNumber:
        if (user.opcode == Opcode::ToString)
            return Representation::JSValue;
        return type.isBoolean() ? Representation::Boolean : Representation::JSValue;
    case OperatorForm::Unselected:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation resultOfOperator(const Program& program, const Function& function, const Value& value)
{
    OperatorForm form = value.form();
    RELEASE_ASSERT(isLegalOperatorForm(value.opcode, form));

    if (testOperators.contains(value.opcode))
        return Representation::Boolean;
    if (value.opcode == Opcode::ToString)
        return Representation::Cell;
    if (value.opcode == Opcode::URShift)
        return Representation::Int64;

    OperatorContext operation { program, function, value };
    switch (form) {
    case OperatorForm::Generic:
        return value.opcode == Opcode::ToNumber ? Representation::Double : Representation::JSValue;
    case OperatorForm::Integers:
        return operation.isBitwise() ? Representation::Int32 : integerWidthOf(operation);
    case OperatorForm::Numbers:
        return operation.isBitwise() ? Representation::Int32 : Representation::Double;
    case OperatorForm::CheckedIntegers:
    case OperatorForm::NumberAndValue:
    case OperatorForm::NullableNumber:
        return Representation::Double;
    case OperatorForm::BigInts:
    case OperatorForm::Strings:
    case OperatorForm::StringAndPrimitive:
        return Representation::Cell;
    case OperatorForm::NullableStrings:
    case OperatorForm::NullableStringAndPrimitive:
        return Representation::JSValue;
    case OperatorForm::Bits:
    case OperatorForm::Booleans:
    case OperatorForm::StringAndValue:
    case OperatorForm::Int32AndValue:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
    case OperatorForm::Unselected:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
