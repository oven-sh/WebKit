/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineFolding.h"

#if ENABLE(AOT)

#include <cmath>
#include <limits>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Machine {

enum class ComparisonFoldOrdering : uint8_t {
    Less = 1 << 0,
    Equal = 1 << 1,
    Greater = 1 << 2,
    // Floating point only: an operand is NaN.
    Unordered = 1 << 3,
};

using ComparisonFoldOrderings = OptionSet<ComparisonFoldOrdering>;

static ComparisonFoldOrderings comparisonFoldOrderingsAcceptedBy(Opcode opcode)
{
    using Ordering = ComparisonFoldOrdering;
    switch (opcode) {
    case Equal:
        return { Ordering::Equal };
    case NotEqual:
        return { Ordering::Less, Ordering::Greater, Ordering::Unordered };
    case LessThan:
    case Below:
        return { Ordering::Less };
    case GreaterThan:
    case Above:
        return { Ordering::Greater };
    case LessEqual:
    case BelowEqual:
        return { Ordering::Less, Ordering::Equal };
    case GreaterEqual:
    case AboveEqual:
        return { Ordering::Greater, Ordering::Equal };
    case EqualOrUnordered:
        return { Ordering::Equal, Ordering::Unordered };
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
    case Add:
    case Sub:
    case Mul:
    case MulHigh:
    case UMulHigh:
    case Div:
    case UDiv:
    case Mod:
    case UMod:
    case Neg:
    case BitAnd:
    case BitOr:
    case BitXor:
    case Shl:
    case SShr:
    case ZShr:
    case Clz:
    case PopCount:
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
    case BitwiseCast:
    case SExt8:
    case SExt16:
    case SExt32:
    case ZExt32:
    case Trunc:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
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

static bool comparisonFoldIsUnsignedOrder(Opcode opcode)
{
    return opcode == Above || opcode == Below || opcode == AboveEqual || opcode == BelowEqual;
}

static Opcode comparisonFoldUnsignedOrderFor(Opcode opcode)
{
    if (opcode == LessThan)
        return Below;
    if (opcode == GreaterThan)
        return Above;
    if (opcode == LessEqual)
        return BelowEqual;
    if (opcode == GreaterEqual)
        return AboveEqual;
    return opcode;
}

// The constant result if the comparison answers the same for every possible ordering, otherwise null.
static Operation* comparisonFoldAnswerFor(Builder& builder, Opcode opcode, ComparisonFoldOrderings possible)
{
    ASSERT(!possible.isEmpty());
    ComparisonFoldOrderings accepted = comparisonFoldOrderingsAcceptedBy(opcode);
    if (accepted.containsAll(possible))
        return builder.const32(1);
    if (!accepted.containsAny(possible))
        return builder.const32(0);
    return nullptr;
}

static ALWAYS_INLINE IntegerRange comparisonFoldRangeOf(const Operation* operand)
{
    if (!hasFoldTrait(operand->opcode(), FoldTrait::BoundsIntegerRange))
        return widestIntegerRangeOf(operand->type());
    if (operand->hasInt())
        return { operand->asInt(), operand->asInt() };
    // rangeOf() reads an Int32 as its sign extension.
    if (operand->opcode() == SExt32)
        return rangeOf(operand->child(0));
    return rangeBoundedByOpcode(operand);
}

struct ComparisonFoldUnsignedRange {
    uint64_t min;
    uint64_t max;
};

// Sign extension preserves unsigned order, so an Int32 is compared through its sign-extended 64-bit range.
static ALWAYS_INLINE ComparisonFoldUnsignedRange comparisonFoldUnsignedRangeOf(const Operation* operand)
{
    IntegerRange range = comparisonFoldRangeOf(operand);
    if (range.min < 0 && range.max >= 0)
        return { 0, std::numeric_limits<uint64_t>::max() };
    return { static_cast<uint64_t>(range.min), static_cast<uint64_t>(range.max) };
}

template<typename Integer>
static ComparisonFoldOrderings comparisonFoldOrderingsBetween(Integer leftMin, Integer leftMax, Integer rightMin, Integer rightMax)
{
    ComparisonFoldOrderings result;
    if (leftMin < rightMax)
        result.add(ComparisonFoldOrdering::Less);
    if (leftMin <= rightMax && rightMin <= leftMax)
        result.add(ComparisonFoldOrdering::Equal);
    if (leftMax > rightMin)
        result.add(ComparisonFoldOrdering::Greater);
    return result;
}

static ComparisonFoldOrderings comparisonFoldPossibleOrderingsOf(Opcode opcode, const Operation* left, const Operation* right)
{
    if (left == right)
        return ComparisonFoldOrdering::Equal;

    if (comparisonFoldIsUnsignedOrder(opcode)) {
        ComparisonFoldUnsignedRange leftRange = comparisonFoldUnsignedRangeOf(left);
        ComparisonFoldUnsignedRange rightRange = comparisonFoldUnsignedRangeOf(right);
        return comparisonFoldOrderingsBetween(leftRange.min, leftRange.max, rightRange.min, rightRange.max);
    }

    IntegerRange leftRange = comparisonFoldRangeOf(left);
    IntegerRange rightRange = comparisonFoldRangeOf(right);
    return comparisonFoldOrderingsBetween(leftRange.min, leftRange.max, rightRange.min, rightRange.max);
}

// A floating-point comparison and its opposite are both false when an operand is NaN.
static bool comparisonFoldHasInvertedComparison(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    return isComparison(opcode) && opcode != EqualOrUnordered && operation->child(0)->type().isInt();
}

// The logical negation of a boolean, in the same type, or null if only a new BitXor could express it.
static Operation* comparisonFoldInverseOf(Builder& builder, Operation* boolean)
{
    if (comparisonFoldHasInvertedComparison(boolean))
        return builder.binary(invertedComparison(boolean->opcode()), boolean->child(0), boolean->child(1));
    if (boolean->opcode() == BitXor && boolean->child(1)->isInt(1) && isBoolean(boolean->child(0)))
        return boolean->child(0);
    return nullptr;
}

static Operation* comparisonFoldBooleanOfType(Builder& builder, Operation* boolean, Type type)
{
    ASSERT(type.isInt() && boolean->type().isInt());
    if (boolean->type() == type)
        return boolean;
    return builder.unary(type == Int32 ? Trunc : ZExt32, boolean);
}

static Operation* foldMachineComparisonWithZeroOrOne(Builder& builder, Opcode opcode, Operation* left, Operation* right)
{
    bool isZero = right->isInt(0);
    bool isOne = right->isInt(1);
    if (!isZero && !isOne)
        return nullptr;

    // PrimitiveEmitter::narrowWordBehind() matches Select(AboveEqual(word, minCell32), _, ZExt32(word)). minCell32 is above 1, so these four rewrites leave that shape alone.
    if ((opcode == BelowEqual && isZero) || (opcode == Below && isOne))
        return builder.binary(Equal, left, builder.constantOfType(left->type(), 0));
    if ((opcode == Above && isZero) || (opcode == AboveEqual && isOne))
        return builder.binary(NotEqual, left, builder.constantOfType(left->type(), 0));

    if (opcode != Equal && opcode != NotEqual)
        return nullptr;

    if (isBoolean(left)) {
        if ((opcode == Equal) == isOne)
            return comparisonFoldBooleanOfType(builder, left, Int32);
        if (Operation* inverse = comparisonFoldInverseOf(builder, left))
            return comparisonFoldBooleanOfType(builder, inverse, Int32);
    }

    // The right side is an integer constant, so these are integers. For floating point this would be wrong: Infinity - Infinity is NaN.
    if (isZero && left->opcode() == Sub)
        return builder.binary(opcode, left->child(0), left->child(1));

    return nullptr;
}

// How an Int64 was extended from 32 bits, if it was.
enum class ComparisonFoldExtension : uint8_t {
    None,
    Sign,
    Zero,
    // The 32-bit value is not negative, so both extensions give the same Int64.
    Either,
};

static ComparisonFoldExtension comparisonFoldExtensionOf(const Operation* operand)
{
    if (operand->hasInt64()) {
        int64_t value = operand->asInt64();
        if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<uint32_t>::max())
            return ComparisonFoldExtension::None;
        if (value < 0)
            return ComparisonFoldExtension::Sign;
        return value > std::numeric_limits<int32_t>::max() ? ComparisonFoldExtension::Zero : ComparisonFoldExtension::Either;
    }

    Opcode opcode = operand->opcode();
    if (opcode != SExt32 && opcode != ZExt32)
        return ComparisonFoldExtension::None;
    if (rangeOf(operand->child(0)).min >= 0)
        return ComparisonFoldExtension::Either;
    return opcode == SExt32 ? ComparisonFoldExtension::Sign : ComparisonFoldExtension::Zero;
}

static Operation* comparisonFoldBitsBeforeExtension(Builder& builder, Operation* operand)
{
    if (operand->hasInt64())
        return builder.const32(static_cast<int32_t>(operand->asInt64()));
    return operand->child(0);
}

// Sign extension preserves both signed and unsigned order. After zero extension, the signed and the unsigned order of the wide values are both the unsigned order of the 32-bit values.
static Operation* foldMachineComparisonOfExtensions(Builder& builder, Opcode opcode, Operation* left, Operation* right)
{
    if (left->type() != Int64)
        return nullptr;
    ComparisonFoldExtension leftExtension = comparisonFoldExtensionOf(left);
    if (leftExtension == ComparisonFoldExtension::None)
        return nullptr;
    ComparisonFoldExtension rightExtension = comparisonFoldExtensionOf(right);
    if (rightExtension == ComparisonFoldExtension::None)
        return nullptr;

    ComparisonFoldExtension extension = leftExtension == ComparisonFoldExtension::Either ? rightExtension : leftExtension;
    if (rightExtension != ComparisonFoldExtension::Either && rightExtension != extension)
        return nullptr;
    if (extension == ComparisonFoldExtension::Zero)
        opcode = comparisonFoldUnsignedOrderFor(opcode);
    return builder.binary(opcode, comparisonFoldBitsBeforeExtension(builder, left), comparisonFoldBitsBeforeExtension(builder, right));
}

static ALWAYS_INLINE bool comparisonFoldEveryOrderingIsPossible(Opcode opcode, const Operation* left, const Operation* right)
{
    if (hasFoldTrait(left->opcode(), FoldTrait::BoundsIntegerRange) || !right->hasInt())
        return false;
    int64_t constant = right->asInt();
    if (comparisonFoldIsUnsignedOrder(opcode))
        return constant && constant != -1;
    IntegerRange widest = widestIntegerRangeOf(left->type());
    return constant != widest.min && constant != widest.max;
}

static Operation* foldMachineComparisonOfIntegers(Builder& builder, Opcode opcode, Operation* left, Operation* right)
{
    if (!comparisonFoldEveryOrderingIsPossible(opcode, left, right)) {
        if (Operation* answer = comparisonFoldAnswerFor(builder, opcode, comparisonFoldPossibleOrderingsOf(opcode, left, right)))
            return answer;
    }
    if (Operation* folded = foldMachineComparisonWithZeroOrOne(builder, opcode, left, right))
        return folded;
    return foldMachineComparisonOfExtensions(builder, opcode, left, right);
}

// Every Float is exactly representable as a Double, and the conversion preserves order and NaN.
static double comparisonFoldNumberOf(const Operation* constant)
{
    return constant->hasDouble() ? constant->asDouble() : constant->asFloat();
}

static ComparisonFoldOrdering comparisonFoldOrderingOfNumbers(double left, double right)
{
    if (left < right)
        return ComparisonFoldOrdering::Less;
    if (left > right)
        return ComparisonFoldOrdering::Greater;
    if (left == right)
        return ComparisonFoldOrdering::Equal;
    return ComparisonFoldOrdering::Unordered;
}

// NaN != NaN, so left == right decides nothing.
static Operation* foldMachineComparisonOfNumbers(Builder& builder, Opcode opcode, Operation* left, Operation* right)
{
    ASSERT(!comparisonFoldIsUnsignedOrder(opcode));
    if (comparisonFoldIsUnsignedOrder(opcode) || !right->isConstant())
        return nullptr;

    double rightNumber = comparisonFoldNumberOf(right);
    if (std::isnan(rightNumber))
        return comparisonFoldAnswerFor(builder, opcode, ComparisonFoldOrdering::Unordered);
    if (!left->isConstant())
        return nullptr;
    return comparisonFoldAnswerFor(builder, opcode, comparisonFoldOrderingOfNumbers(comparisonFoldNumberOf(left), rightNumber));
}

Operation* foldComparison(Builder& builder, Opcode opcode, Operation* left, Operation* right)
{
    ASSERT(isComparison(opcode));
    ASSERT(left->type() == right->type());

    if (left->isConstant() && !right->isConstant())
        return builder.binary(flippedComparison(opcode), right, left);

    if (left->type().isFloat())
        return foldMachineComparisonOfNumbers(builder, opcode, left, right);
    if (!left->type().isInt() || opcode == EqualOrUnordered)
        return nullptr;
    return foldMachineComparisonOfIntegers(builder, opcode, left, right);
}

Operation* foldSelect(Builder& builder, Operation* predicate, Operation* whenNotZero, Operation* whenZero)
{
    ASSERT(whenNotZero->type() == whenZero->type());

    if (whenNotZero == whenZero)
        return whenNotZero;

    SimplifiedPredicate simplified = simplifyPredicate(predicate);
    if (simplified.predicate->hasInt()) {
        bool isNotZero = simplified.predicate->asInt();
        return isNotZero != simplified.isInverted ? whenNotZero : whenZero;
    }

    Type type = whenNotZero->type();
    if (!type.isInt() || !isBoolean(predicate))
        return nullptr;
    if (whenNotZero->isInt(1) && whenZero->isInt(0))
        return comparisonFoldBooleanOfType(builder, predicate, type);
    if (whenNotZero->isInt(0) && whenZero->isInt(1)) {
        if (Operation* inverse = comparisonFoldInverseOf(builder, predicate))
            return comparisonFoldBooleanOfType(builder, inverse, type);
    }
    return nullptr;
}

// A peeled operation that nothing else reads has counted no use of its child, so the scan can still fuse the child into the test.
static_assert(countsChildrenAtFirstUse(Equal) && countsChildrenAtFirstUse(NotEqual) && countsChildrenAtFirstUse(BitXor) && countsChildrenAtFirstUse(ZExt32) && countsChildrenAtFirstUse(SExt32) && countsChildrenAtFirstUse(Trunc));

SimplifiedPredicate peelPredicate(Operation* predicate)
{
    SimplifiedPredicate result { predicate };
    for (;;) {
        Operation* tested = result.predicate;
        Opcode opcode = tested->opcode();

        if ((opcode == Equal || opcode == NotEqual) && tested->child(1)->isInt(0)) {
            if (opcode == Equal)
                result.isInverted = !result.isInverted;
            result.predicate = tested->child(0);
            continue;
        }

        if (opcode == BitXor && tested->child(1)->isInt(1) && isBoolean(tested->child(0))) {
            result.isInverted = !result.isInverted;
            result.predicate = tested->child(0);
            continue;
        }

        // Truncating any other Int64 could drop its only nonzero bits.
        if (opcode == ZExt32 || opcode == SExt32 || (opcode == Trunc && isBoolean(tested->child(0)))) {
            result.predicate = tested->child(0);
            continue;
        }

        return result;
    }
}

// Bounded so that building a long BitOr chain, which asks at every link, stays linear.
static constexpr unsigned levelsSearchedByIsBoolean = 3;

static bool isBooleanWithinLevels(const Operation* operation, unsigned levels)
{
    Opcode opcode = operation->opcode();
    if (!hasFoldTrait(opcode, FoldTrait::MayBeBoolean))
        return false;
    if (isComparison(opcode))
        return true;
    if (operation->hasInt())
        return !operation->asInt() || operation->asInt() == 1;
    if (!levels)
        return false;

    auto childIsBoolean = [&](unsigned index) {
        return isBooleanWithinLevels(operation->child(index), levels - 1);
    };
    if (opcode == BitAnd)
        return childIsBoolean(1) || childIsBoolean(0);
    if (opcode == BitOr || opcode == BitXor)
        return childIsBoolean(0) && childIsBoolean(1);
    if (opcode == Select)
        return childIsBoolean(1) && childIsBoolean(2);
    if (opcode == ZExt32 || opcode == SExt32 || opcode == Trunc)
        return childIsBoolean(0);
    return false;
}

bool isBooleanByItsChildren(const Operation* operation)
{
    return isBooleanWithinLevels(operation, levelsSearchedByIsBoolean);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
