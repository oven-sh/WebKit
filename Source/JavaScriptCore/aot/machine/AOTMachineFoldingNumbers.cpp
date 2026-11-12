/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineFolding.h"

#if ENABLE(AOT)

#include <bit>
#include <cmath>
#include <optional>
#include <type_traits>

namespace JSC { namespace AOT { namespace Machine {

template<typename T>
static std::optional<T> numberFoldConstantOf(const Operation* operation)
{
    if constexpr (std::is_same_v<T, double>) {
        if (operation->hasDouble())
            return operation->asDouble();
    } else {
        if (operation->hasFloat())
            return operation->asFloat();
    }
    return std::nullopt;
}

static Operation* numberFoldConstant(Builder& builder, double value)
{
    return builder.constDouble(value);
}

static Operation* numberFoldConstant(Builder& builder, float value)
{
    return builder.constFloat(value);
}

template<typename T>
static T numberFoldNaN(bool isNegative)
{
    if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<double>(isNegative ? 0xfff8000000000000ull : 0x7ff8000000000000ull);
    else
        return std::bit_cast<float>(isNegative ? 0xffc00000u : 0x7fc00000u);
}

template<typename T>
static Operation* numberFoldResult(Builder& builder, T result, T left, T right)
{
    if (!std::isnan(result))
        return numberFoldConstant(builder, result);
    if (std::isnan(left))
        return numberFoldConstant(builder, numberFoldNaN<T>(std::signbit(left)));
    if (std::isnan(right))
        return numberFoldConstant(builder, numberFoldNaN<T>(std::signbit(right)));
    return numberFoldConstant(builder, numberFoldNaN<T>(isX86_64()));
}

template<typename T>
static Operation* numberFoldResult(Builder& builder, T result, T operand)
{
    return numberFoldResult(builder, result, operand, operand);
}

template<typename T>
static T numberFoldMin(T left, T right)
{
    if (std::isnan(left) || std::isnan(right))
        return left + right;
    if (left == right)
        return std::signbit(left) ? left : right;
    return left < right ? left : right;
}

template<typename T>
static T numberFoldMax(T left, T right)
{
    if (std::isnan(left) || std::isnan(right))
        return left + right;
    if (left == right)
        return std::signbit(left) ? right : left;
    return left > right ? left : right;
}

static bool numberFoldIsRounded(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    return opcode == Ceil || opcode == Floor || opcode == FTrunc || opcode == IToD;
}

template<typename T>
static std::optional<T> numberFoldExactReciprocalOf(T divisor)
{
    if (!std::isnormal(divisor))
        return std::nullopt;
    int exponent = 0;
    if (std::abs(std::frexp(divisor, &exponent)) != static_cast<T>(0.5))
        return std::nullopt;
    T reciprocal = 1 / divisor;
    if (!std::isnormal(reciprocal))
        return std::nullopt;
    return reciprocal;
}

bool isExactConvertedInteger(const Operation* number)
{
    if (number->opcode() != IToD)
        return false;
    const Operation* integer = number->child(0);
    constexpr int64_t limit = int64_t { 1 } << 52;
    return integer->type() == Int32 || IntegerRange { -limit, limit }.contains(rangeOf(integer));
}

Operation* wideConvertedInteger(Builder& builder, Operation* number)
{
    Operation* integer = number->child(0);
    return integer->type() == Int32 ? builder.unary(SExt32, integer) : integer;
}

static Operation* numberFoldSumOfExactIntegers(Builder& builder, Opcode opcode, Operation* left, Operation* right)
{
    ASSERT(opcode == Add || opcode == Sub);
    if (!isExactConvertedInteger(left) || !isExactConvertedInteger(right))
        return nullptr;
    Operation* wideLeft = wideConvertedInteger(builder, left);
    Operation* wideRight = wideConvertedInteger(builder, right);
    return builder.unary(IToD, builder.binary(opcode, wideLeft, wideRight));
}

template<typename T>
static Operation* foldMachineNumber(Builder& builder, Opcode opcode, Operation* left, Operation* rightOrNull)
{
    ASSERT(!!rightOrNull == (descriptionOf(opcode).numberOfChildren == 2));

    std::optional<T> leftConstant = numberFoldConstantOf<T>(left);
    std::optional<T> rightConstant = rightOrNull ? numberFoldConstantOf<T>(rightOrNull) : std::nullopt;

    switch (opcode) {
    case Add:
        if (Operation* sum = numberFoldSumOfExactIntegers(builder, opcode, left, rightOrNull))
            return sum;
        if (!rightConstant)
            return nullptr;
        if (leftConstant)
            return numberFoldResult(builder, *leftConstant + *rightConstant, *leftConstant, *rightConstant);
        if (!*rightConstant && std::signbit(*rightConstant))
            return left;
        return nullptr;

    case Sub:
        if (Operation* difference = numberFoldSumOfExactIntegers(builder, opcode, left, rightOrNull))
            return difference;
        if (!rightConstant)
            return nullptr;
        if (leftConstant)
            return numberFoldResult(builder, *leftConstant - *rightConstant, *leftConstant, *rightConstant);
        if (!*rightConstant && !std::signbit(*rightConstant))
            return left;
        return nullptr;

    case Mul:
        if (!rightConstant)
            return nullptr;
        if (leftConstant)
            return numberFoldResult(builder, *leftConstant * *rightConstant, *leftConstant, *rightConstant);
        if (*rightConstant == 1)
            return left;
        return nullptr;

    case Div:
        if (!rightConstant)
            return nullptr;
        if (leftConstant)
            return numberFoldResult(builder, *leftConstant / *rightConstant, *leftConstant, *rightConstant);
        if (*rightConstant == 1)
            return left;
        if (std::optional<T> reciprocal = numberFoldExactReciprocalOf(*rightConstant))
            return builder.binary(Mul, left, numberFoldConstant(builder, *reciprocal));
        return nullptr;

    case Neg:
        if (leftConstant)
            return numberFoldConstant(builder, std::isnan(*leftConstant) ? numberFoldNaN<T>(!std::signbit(*leftConstant)) : -*leftConstant);
        if (left->opcode() == Neg)
            return left->child(0);
        return nullptr;

    case Abs:
        if (leftConstant)
            return numberFoldConstant(builder, std::isnan(*leftConstant) ? numberFoldNaN<T>(false) : std::abs(*leftConstant));
        if (left->opcode() == Abs)
            return left;
        if (left->opcode() == Neg)
            return builder.unary(Abs, left->child(0));
        return nullptr;

    case Ceil:
        if (leftConstant)
            return numberFoldResult(builder, std::ceil(*leftConstant), *leftConstant);
        return numberFoldIsRounded(left) ? left : nullptr;

    case Floor:
        if (leftConstant)
            return numberFoldResult(builder, std::floor(*leftConstant), *leftConstant);
        return numberFoldIsRounded(left) ? left : nullptr;

    case FTrunc:
        if (leftConstant)
            return numberFoldResult(builder, std::trunc(*leftConstant), *leftConstant);
        return numberFoldIsRounded(left) ? left : nullptr;

    case Sqrt:
        if (leftConstant)
            return numberFoldResult(builder, std::sqrt(*leftConstant), *leftConstant);
        return nullptr;

    case FMin:
        if (leftConstant && rightConstant)
            return numberFoldResult(builder, numberFoldMin(*leftConstant, *rightConstant), *leftConstant, *rightConstant);
        return nullptr;

    case FMax:
        if (leftConstant && rightConstant)
            return numberFoldResult(builder, numberFoldMax(*leftConstant, *rightConstant), *leftConstant, *rightConstant);
        return nullptr;

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operation* foldFloatingPoint(Builder& builder, Opcode opcode, Type type, Operation* left, Operation* rightOrNull)
{
    ASSERT(left->type() == type && (!rightOrNull || rightOrNull->type() == type));

    switch (type.kind()) {
    case Float:
        return foldMachineNumber<float>(builder, opcode, left, rightOrNull);
    case Double:
        return foldMachineNumber<double>(builder, opcode, left, rightOrNull);
    case Void:
    case Int32:
    case Int64:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
