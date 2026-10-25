/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorForms.h"

#if ENABLE(AOT)

#include "AOTRepresentation.h"
#include <functional>

namespace JSC { namespace AOT { namespace Typed {

static bool isObjectOrNullish(const Type& type)
{
    return !(type.primitives & ~Primitive::UndefinedOrNull) && type.isJavaScriptValue();
}

static bool isBooleanOrNullish(const Type& type)
{
    return type.hasOnlyPrimitives(Primitive::Boolean | Primitive::UndefinedOrNull);
}

static bool isInt32AndBoxedValue(const Type& integer, const Type& other)
{
    return integer.isInt32() && defaultRepresentationOf(other) == Representation::JSValue;
}

static bool isStringOrUndefined(const Type& type)
{
    return type.primitives & Primitive::AnyString && type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined);
}

static bool isLiteralStringWord(const Type& type)
{
    return defaultRepresentationOf(type) == Representation::LiteralString;
}

static bool needsGenericTest(const Type& type)
{
    return type.primitives & Primitive::Number && type.primitives & Primitive::BigInt;
}

static bool areBoth(const OperatorTypes& types, auto predicate)
{
    return std::invoke(predicate, types.left) && std::invoke(predicate, types.right);
}

static bool isEither(const OperatorTypes& types, auto predicate)
{
    return std::invoke(predicate, types.left) || std::invoke(predicate, types.right);
}

static OperatorForm chooseRelationForm(const OperatorTypes& types, CodeShape shape)
{
    if (areBoth(types, &Type::isInteger))
        return OperatorForm::Integers;
    if (areBoth(types, isNumericOperand))
        return OperatorForm::Numbers;
    if (areBoth(types, isBigInt))
        return OperatorForm::BigInts;
    if (isEither(types, &Type::isNever))
        return OperatorForm::Generic;
    if (isEither(types, &Type::isNumber))
        return OperatorForm::NumberAndValue;
    if (shape == CodeShape::Fast && areBoth(types, &Type::isAnyString))
        return OperatorForm::Strings;
    // A character read beyond the end of a text. undefined is compared as NaN, so no relation holds. (null is compared as 0, which the text of a string may say.)
    if (shape == CodeShape::Fast && areBoth(types, isStringOrUndefined))
        return OperatorForm::NullableStrings;
    return OperatorForm::Generic;
}

static OperatorForm chooseStrictEqualityForm(const OperatorTypes& types, CodeShape shape)
{
    if (areBoth(types, &Type::isInteger))
        return OperatorForm::Integers;
    bool mayBeNumber = (types.left.primitives | types.right.primitives) & Primitive::Number;
    if (mayBeNumber && areBoth(types, isNullableNumber))
        return OperatorForm::Numbers;
    if (areBoth(types, &Type::isBoolean))
        return OperatorForm::Booleans;
    if (canCompareBits(types.left, types.right))
        return OperatorForm::Bits;
    if (areBoth(types, isBigInt))
        return OperatorForm::BigInts;
    // Two words are compared in fewer bytes than the call takes.
    if (shape != CodeShape::Fast && !areBoth(types, isLiteralStringWord))
        return OperatorForm::Generic;

    if (areBoth(types, &Type::isAnyString))
        return OperatorForm::Strings;
    if (areBoth(types, &Type::isAnyStringOrNullish))
        return OperatorForm::NullableStrings;
    if (isEither(types, &Type::isAnyString))
        return OperatorForm::StringAndValue;
    if (isInt32AndBoxedValue(types.left, types.right) || isInt32AndBoxedValue(types.right, types.left))
        return OperatorForm::Int32AndValue;
    if (isEither(types, &Type::isNumber))
        return OperatorForm::NumberAndValue;
    return OperatorForm::Generic;
}

static OperatorForm chooseLooseEqualityForm(const OperatorTypes& types, CodeShape shape)
{
    bool comparesWithoutConversion = areBoth(types, &Type::isNumber) || areBoth(types, &Type::isAnyString) || areBoth(types, &Type::isBoolean)
        || areBoth(types, &Type::isObject) || areBoth(types, isBigInt);
    if (comparesWithoutConversion)
        return chooseStrictEqualityForm(types, shape);
    if (areBoth(types, isNullableNumber))
        return OperatorForm::Numbers;
    if (shape == CodeShape::Fast && areBoth(types, &Type::isAnyStringOrNullish))
        return OperatorForm::NullableStrings;
    if (areBoth(types, isObjectOrNullish) || areBoth(types, isBooleanOrNullish))
        return OperatorForm::Bits;
    return OperatorForm::Generic;
}

OperatorForm chooseComparisonForm(const OperatorSite& site, Opcode opcode, const OperatorTypes& types)
{
    switch (opcode) {
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        return chooseRelationForm(types, site.shape);
    case Opcode::StrictEq:
        return chooseStrictEqualityForm(types, site.shape);
    case Opcode::Eq:
        return chooseLooseEqualityForm(types, site.shape);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static OperatorForm chooseToNumberForm(const Type& type)
{
    if (type.isBoolean())
        return OperatorForm::Booleans;
    if (type.isInteger())
        return OperatorForm::Integers;
    if (isNullableNumber(type))
        return OperatorForm::Numbers;
    if (isNumericOperand(type))
        return OperatorForm::NullableNumber;
    return OperatorForm::Generic;
}

static OperatorForm chooseToBooleanForm(const Type& type)
{
    if (type.isBoolean())
        return OperatorForm::Booleans;
    if (type.isInteger())
        return OperatorForm::Integers;
    if (type.isNumberOrNullish())
        return OperatorForm::Numbers;
    if (!needsGenericTest(type))
        return OperatorForm::Bits;
    return OperatorForm::Generic;
}

static OperatorForm chooseToStringForm(const Type& type, CodeShape shape)
{
    if (type.isAnyString())
        return OperatorForm::String;
    if (type.isNumber())
        return shape == CodeShape::Fast ? OperatorForm::Number : OperatorForm::NumberByOperation;
    if (type.isAnyStringOrNullish())
        return OperatorForm::NullableString;
    if (type.primitives & Primitive::AnyString && !(type.primitives & Primitive::Absent)) {
        bool mayBeNumber = !type.hasObjects() && type.primitives & Primitive::Number;
        return mayBeNumber ? OperatorForm::MayBeStringOrNumber : OperatorForm::MayBeString;
    }
    if (type.isNumberOrNullish() && shape == CodeShape::Fast)
        return OperatorForm::NullableNumber;
    return OperatorForm::Generic;
}

OperatorForm chooseConversionForm(const OperatorSite& site, Opcode opcode, const OperatorTypes& types)
{
    switch (opcode) {
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        return chooseToNumberForm(types.left);
    case Opcode::ToBoolean:
        return chooseToBooleanForm(types.left);
    case Opcode::ToString:
        return chooseToStringForm(types.left, site.shape);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
