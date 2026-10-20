/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorForms.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

static bool isBigInt(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::BigInt);
}

static OperatorForm chooseNumericForm(Opcode opcode, const OperatorTypes& types)
{
    const Type& left = types.left;
    const Type& right = types.right;
    if (left.isInteger() && right.isInteger()) {
        if (types.result.isInteger() && isLegalOperatorForm(opcode, OperatorForm::Integers))
            return OperatorForm::Integers;
        if (isLegalOperatorForm(opcode, OperatorForm::CheckedIntegers))
            return OperatorForm::CheckedIntegers;
    }
    if (isNumericOperand(left) && isNumericOperand(right))
        return OperatorForm::Numbers;
    if (isBigInt(left) && isBigInt(right))
        return OperatorForm::BigInts;
    bool hasNumberOperand = left.isNumber() || right.isNumber();
    if (hasNumberOperand && types.result.isNumber() && isLegalOperatorForm(opcode, OperatorForm::NumberAndValue))
        return OperatorForm::NumberAndValue;
    return OperatorForm::Generic;
}

static OperatorForm chooseConcatenationForm(CodeShape shape, const OperatorTypes& types)
{
    const Type& left = types.left;
    const Type& right = types.right;
    if (left.hasObjects() || right.hasObjects())
        return OperatorForm::Generic;
    if (left.isAnyString() && right.isAnyString())
        return OperatorForm::Strings;
    if (left.isAnyString() || right.isAnyString())
        return OperatorForm::StringAndPrimitive;
    if (shape != CodeShape::Fast)
        return OperatorForm::Generic;

    bool leftIsString = left.isAnyStringOrNullish();
    bool rightIsString = right.isAnyStringOrNullish();
    if (leftIsString && rightIsString)
        return OperatorForm::NullableStrings;
    if (leftIsString || rightIsString)
        return OperatorForm::NullableStringAndPrimitive;
    return OperatorForm::Generic;
}

static OperatorForm chooseAdditionForm(CodeShape shape, const OperatorTypes& types)
{
    OperatorForm numericForm = chooseNumericForm(Opcode::Add, types);
    if (numericForm != OperatorForm::Generic)
        return numericForm;
    return chooseConcatenationForm(shape, types);
}

static OperatorForm chooseBitwiseForm(Opcode opcode, const OperatorTypes& types)
{
    const Type& left = types.left;
    const Type& right = types.right;
    if (left.isInt32() && right.isInt32())
        return OperatorForm::Integers;
    if (isNumericOperand(left) && isNumericOperand(right))
        return OperatorForm::Numbers;
    if (isBigInt(left) && isBigInt(right) && isLegalOperatorForm(opcode, OperatorForm::BigInts))
        return OperatorForm::BigInts;
    return OperatorForm::Generic;
}

OperatorForm chooseArithmeticForm(const OperatorSite& site, Opcode opcode, const OperatorTypes& types)
{
    switch (opcode) {
    case Opcode::Add:
        return chooseAdditionForm(site.shape, types);
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::Negate:
    case Opcode::Increment:
    case Opcode::Decrement:
        return chooseNumericForm(opcode, types);
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
    case Opcode::BitNot:
        return chooseBitwiseForm(opcode, types);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
