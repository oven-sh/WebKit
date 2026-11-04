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

// Every rule produces the exact bits that the operation would, except for the payload of a NaN.
// Of two NaNs with different signs an instruction keeps the one in its first operand, and the allocator decides which operand comes first.
// None of these hold: x + 0 = x (fails for -0), x * 0 = 0 (NaN, infinity, negative x), x - x = 0 (NaN, infinity).
// Nothing is reassociated, because every operation rounds.

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

// An invalid operation makes a NaN with the sign set on x86-64 and clear on ARM64, and a NaN that an operand brings keeps its sign. A program can read the sign out of a Float64Array.
// The builder and the encoders are compiled for one CPU, that of the compiler itself, so the host's arithmetic gives the target's sign.
// The payload goes: without it a NaN is a number when it is boxed.
static Operation* numberFoldConstant(Builder& builder, double value)
{
    return builder.constDouble(std::isnan(value) ? std::bit_cast<double>((std::bit_cast<uint64_t>(value) >> 51 | 1) << 51) : value);
}

static Operation* numberFoldConstant(Builder& builder, float value)
{
    return builder.constFloat(std::isnan(value) ? std::bit_cast<float>((std::bit_cast<uint32_t>(value) >> 22 | 1) << 22) : value);
}

// Like Math.min and Math.max: NaN if either operand is NaN, and -0 is less than 0.
// The NaN is that of the sum: fmin and fmax choose it as fadd does on ARM64, and the encoder adds the operands on x86-64.
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

// The operation only produces whole numbers, infinities and NaN. Rounding leaves each of those unchanged, -0 included.
static bool numberFoldIsRounded(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    return opcode == Ceil || opcode == Floor || opcode == FTrunc || opcode == IToD;
}

// Only a power of two has one. Dividing by it and multiplying by its reciprocal compute the same real number, and each rounds it once.
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

// At most 2^52 in magnitude, so IToD rounded nothing away.
static bool numberFoldIsExactInteger(const Operation* number)
{
    if (number->opcode() != IToD)
        return false;
    const Operation* integer = number->child(0);
    constexpr int64_t limit = int64_t { 1 } << 52;
    return integer->type() == Int32 || IntegerRange { -limit, limit }.contains(rangeOf(integer));
}

static Operation* numberFoldIntegerBehind(Builder& builder, Operation* number)
{
    Operation* integer = number->child(0);
    return integer->type() == Int32 ? builder.unary(SExt32, integer) : integer;
}

// Add(IToD a, IToD b) is IToD(Add(a, b)), and so for Sub. The result is at most 2^53 in magnitude, which a double holds. Neither form gives -0.
static Operation* numberFoldSumOfExactIntegers(Builder& builder, Opcode opcode, Operation* left, Operation* right)
{
    ASSERT(opcode == Add || opcode == Sub);
    if (!numberFoldIsExactInteger(left) || !numberFoldIsExactInteger(right))
        return nullptr;
    // A statement each: as two arguments of one call they would be built in an order that the language leaves open, and the index of an operation follows that order.
    Operation* wideLeft = numberFoldIntegerBehind(builder, left);
    Operation* wideRight = numberFoldIntegerBehind(builder, right);
    return builder.unary(IToD, builder.binary(opcode, wideLeft, wideRight));
}

// An operation on two constants must always fold: the encoder has only one scratch register to materialize a constant in.
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
            return numberFoldConstant(builder, *leftConstant + *rightConstant);
        // -0 + 0 is 0, so the identity of addition is -0, not 0.
        if (!*rightConstant && std::signbit(*rightConstant))
            return left;
        return nullptr;

    case Sub:
        if (Operation* difference = numberFoldSumOfExactIntegers(builder, opcode, left, rightOrNull))
            return difference;
        if (!rightConstant)
            return nullptr;
        if (leftConstant)
            return numberFoldConstant(builder, *leftConstant - *rightConstant);
        // x - -0 is x + 0, which turns -0 into 0.
        if (!*rightConstant && !std::signbit(*rightConstant))
            return left;
        return nullptr;

    case Mul:
        if (!rightConstant)
            return nullptr;
        if (leftConstant)
            return numberFoldConstant(builder, *leftConstant * *rightConstant);
        if (*rightConstant == 1)
            return left;
        return nullptr;

    case Div:
        if (!rightConstant)
            return nullptr;
        if (leftConstant)
            return numberFoldConstant(builder, *leftConstant / *rightConstant);
        if (*rightConstant == 1)
            return left;
        if (std::optional<T> reciprocal = numberFoldExactReciprocalOf(*rightConstant))
            return builder.binary(Mul, left, numberFoldConstant(builder, *reciprocal));
        return nullptr;

    case Neg:
        if (leftConstant)
            return numberFoldConstant(builder, -*leftConstant);
        if (left->opcode() == Neg)
            return left->child(0);
        return nullptr;

    case Abs:
        if (leftConstant)
            return numberFoldConstant(builder, std::abs(*leftConstant));
        if (left->opcode() == Abs)
            return left;
        if (left->opcode() == Neg)
            return builder.unary(Abs, left->child(0));
        return nullptr;

    case Ceil:
        if (leftConstant)
            return numberFoldConstant(builder, std::ceil(*leftConstant));
        return numberFoldIsRounded(left) ? left : nullptr;

    case Floor:
        if (leftConstant)
            return numberFoldConstant(builder, std::floor(*leftConstant));
        return numberFoldIsRounded(left) ? left : nullptr;

    case FTrunc:
        if (leftConstant)
            return numberFoldConstant(builder, std::trunc(*leftConstant));
        return numberFoldIsRounded(left) ? left : nullptr;

    case Sqrt:
        if (leftConstant)
            return numberFoldConstant(builder, std::sqrt(*leftConstant));
        return nullptr;

    case FMin:
        if (leftConstant && rightConstant)
            return numberFoldConstant(builder, numberFoldMin(*leftConstant, *rightConstant));
        return nullptr;

    case FMax:
        if (leftConstant && rightConstant)
            return numberFoldConstant(builder, numberFoldMax(*leftConstant, *rightConstant));
        return nullptr;

    case Nop:
    case Opaque:
    case Const32:
    case Const64:
    case ConstFloat:
    case ConstDouble:
    case FramePointer:
    case SlotBase:
    case AddressOf:
    case NumberOf:
    case InstanceAddressOf:
    case ArgumentReg:
    case MulHigh:
    case UMulHigh:
    case UDiv:
    case Mod:
    case UMod:
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case Clz:
    case PopCount:
    case BitwiseCast:
    case SExt8:
    case SExt16:
    case SExt32:
    case ZExt32:
    case Trunc:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
    case Select:
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
    case Store8:
    case Store16:
    case Store:
    case Fence:
    case Phi:
    case Upsilon:
    case Extract:
    case Patch:
    case Check:
    case Jump:
    case Branch:
    case Switch:
    case Return:
    case Oops:
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
