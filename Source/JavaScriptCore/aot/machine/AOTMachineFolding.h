/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineBuilder.h"
#include "AOTMachineTarget.h"
#include "AOTSlotRepresentation.h"

namespace JSC { namespace AOT { namespace Machine {

Operation* foldIntegerArithmetic(Builder&, Opcode, Type, Operation* left, Operation* rightOrNull, bool isChill);
Operation* foldMachineBitAndOfBoundedValue(Builder&, Type, Operation* left, Operation* right);
Operation* foldMachineBitOrOfBoundedValue(Builder&, Type, Operation* left, Operation* right);
Operation* foldMachineShiftOrBitCount(Builder&, Opcode, Type, Operation* left, Operation* rightOrNull);
Operation* foldMachineRotationOutOfShifts(Builder&, Opcode, Type, Operation* left, Operation* right);
Operation* foldConversionOtherThanOfWords(Builder&, Opcode, Type result, Operation*);
Operation* foldLowWord(Builder&, Operation* wide);
Operation* foldFloatingPoint(Builder&, Opcode, Type, Operation* left, Operation* rightOrNull);
Operation* foldMachineComparisonOfNumbers(Builder&, Opcode, Operation* left, Operation* right);
bool isExactConvertedInteger(const Operation* number);
Operation* wideConvertedInteger(Builder&, Operation* number);
Operation* foldMachineOrderWithZeroOrOne(Builder&, Opcode, Operation* left, bool isZero, bool isOne);
Operation* foldMachineComparisonOfBoundedIntegers(Builder&, Opcode, Operation* left, Operation* right);
Operation* foldSelect(Builder&, Operation* predicate, Operation* whenNotZero, Operation* whenZero);

namespace FoldTrait {
enum : uint8_t {
    IsPeeledOffPredicates = 1 << 0,
    MayBeBoolean = 1 << 1,
    BoundsIntegerRange = 1 << 2,
};
}

constexpr std::array<uint8_t, sizeOfTableByOpcode> listFoldTraitsOfOpcodes()
{
    std::array<uint8_t, sizeOfTableByOpcode> traits { };
    for (Opcode opcode : { Equal, NotEqual, BitXor, ZExt32, SExt32, Trunc })
        traits[opcode] |= FoldTrait::IsPeeledOffPredicates;
    for (Opcode opcode : { Const32, Const64, Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual, EqualOrUnordered, BitAnd, BitOr, BitXor, Select, ZExt32, SExt32, Trunc })
        traits[opcode] |= FoldTrait::MayBeBoolean;
    for (Opcode opcode : { Const32, Const64, Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual, EqualOrUnordered, Load8Z, Load8S, Load16Z, Load16S, Clz, PopCount, SExt8, SExt16, SExt32, Trunc, ZExt32, Add, Sub, Neg, Mul, BitAnd, BitOr, BitXor, Shl, SShr, ZShr, Select })
        traits[opcode] |= FoldTrait::BoundsIntegerRange;
    return traits;
}
inline constexpr std::array<uint8_t, sizeOfTableByOpcode> foldTraitsOfOpcodes = listFoldTraitsOfOpcodes();

constexpr bool hasFoldTrait(Opcode opcode, uint8_t trait) { return foldTraitsOfOpcodes[opcode] & trait; }

inline bool isDecodedReference(const Operation* operation)
{
    if (operation->opcode() != Add || !operation->child(0)->isArgumentReg(cellCageBaseGPR))
        return false;
    const Operation* scaled = operation->child(1);
    return scaled->opcode() == Shl && scaled->child(1)->isInt(cell32Shift) && scaled->child(0)->opcode() == ZExt32;
}
inline constexpr uint64_t bitsOfDecodedReference = ((uint64_t { 1 } << 48) - 1) & ~((uint64_t { 1 } << cell32Shift) - 1);

struct SimplifiedPredicate {
    Operation* predicate;
    bool isInverted { false };
};
SimplifiedPredicate peelPredicate(Operation* predicate);
inline SimplifiedPredicate simplifyPredicate(Operation* predicate)
{
    if (!hasFoldTrait(predicate->opcode(), FoldTrait::IsPeeledOffPredicates))
        return { predicate };
    return peelPredicate(predicate);
}

bool isBooleanByItsChildren(const Operation*);
inline bool isBoolean(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    return hasFoldTrait(opcode, FoldTrait::MayBeBoolean) && (isComparison(opcode) || isBooleanByItsChildren(operation));
}

ALWAYS_INLINE SimplifiedPredicate peelPredicateInline(Operation* predicate)
{
    SimplifiedPredicate result { predicate };
    for (;;) {
        Operation* tested = result.predicate;
        Opcode opcode = tested->opcode();
        if (!hasFoldTrait(opcode, FoldTrait::IsPeeledOffPredicates))
            return result;

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

        if (opcode == ZExt32 || opcode == SExt32 || (opcode == Trunc && isBoolean(tested->child(0)))) {
            result.predicate = tested->child(0);
            continue;
        }

        return result;
    }
}

struct IntegerRange {
    bool contains(const IntegerRange& other) const { return min <= other.min && other.max <= max; }

    int64_t min;
    int64_t max;
};
inline IntegerRange widestIntegerRangeOf(Type type)
{
    ASSERT(type.isInt());
    if (type == Int32)
        return { std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() };
    return { std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max() };
}
IntegerRange rangeBoundedByOpcode(const Operation*);
inline IntegerRange rangeOf(const Operation* operation)
{
    if (!hasFoldTrait(operation->opcode(), FoldTrait::BoundsIntegerRange))
        return widestIntegerRangeOf(operation->type());
    return rangeBoundedByOpcode(operation);
}

constexpr bool comparisonFoldIsUnsignedOrder(Opcode opcode)
{
    return opcode == Above || opcode == Below || opcode == AboveEqual || opcode == BelowEqual;
}

ALWAYS_INLINE bool comparisonFoldEveryOrderingIsPossible(Opcode opcode, const Operation* left, const Operation* right)
{
    if (hasFoldTrait(left->opcode(), FoldTrait::BoundsIntegerRange) || !right->hasInt())
        return false;
    int64_t constant = right->asInt();
    if (comparisonFoldIsUnsignedOrder(opcode))
        return constant && constant != -1;
    IntegerRange widest = widestIntegerRangeOf(left->type());
    return constant != widest.min && constant != widest.max;
}

static_assert([] {
    for (unsigned opcode = 0; opcode < numberOfOpcodes; ++opcode) {
        if (hasFoldTrait(static_cast<Opcode>(opcode), FoldTrait::MayBeBoolean) && !hasFoldTrait(static_cast<Opcode>(opcode), FoldTrait::BoundsIntegerRange))
            return false;
    }
    return hasFoldTrait(Sub, FoldTrait::BoundsIntegerRange) && hasFoldTrait(Const64, FoldTrait::BoundsIntegerRange) && hasFoldTrait(SExt32, FoldTrait::BoundsIntegerRange) && hasFoldTrait(ZExt32, FoldTrait::BoundsIntegerRange);
}());

static_assert(hasFoldTrait(Const32, FoldTrait::BoundsIntegerRange));

ALWAYS_INLINE bool comparisonFoldNeitherSideIsBounded(const Operation* left, const Operation* right)
{
    return left != right && !hasFoldTrait(left->opcode(), FoldTrait::BoundsIntegerRange) && !hasFoldTrait(right->opcode(), FoldTrait::BoundsIntegerRange);
}

ALWAYS_INLINE bool comparisonFoldTestsBitsOfUnboundedValue(Opcode opcode, const Operation* left, const Operation* right)
{
    if ((opcode != Equal && opcode != NotEqual) || left->opcode() != BitAnd || !right->hasInt())
        return false;
    const Operation* mask = left->child(1);
    if (!mask->hasInt() || hasFoldTrait(left->child(0)->opcode(), FoldTrait::BoundsIntegerRange))
        return false;
    int64_t bits = mask->asInt();
    if (bits < 0)
        return true;
    int64_t constant = right->asInt();
    return bits > 1 && constant >= 0 && constant <= bits;
}

ALWAYS_INLINE Operation* foldComparison(Builder& builder, Opcode opcode, Operation* left, Operation* right)
{
    ASSERT(isComparison(opcode));
    ASSERT(left->type() == right->type());

    if (left->isConstant() && !right->isConstant())
        return builder.binary(flippedComparison(opcode), right, left);

    if (left->type().isFloat())
        return foldMachineComparisonOfNumbers(builder, opcode, left, right);
    if (!left->type().isInt() || opcode == EqualOrUnordered)
        return nullptr;
    if (comparisonFoldEveryOrderingIsPossible(opcode, left, right)) {
        ASSERT(!isBoolean(left) && left->opcode() != Sub);
        if (!comparisonFoldIsUnsignedOrder(opcode))
            return nullptr;
        return foldMachineOrderWithZeroOrOne(builder, opcode, left, right->isInt(0), right->isInt(1));
    }
    if (comparisonFoldTestsBitsOfUnboundedValue(opcode, left, right) || comparisonFoldNeitherSideIsBounded(left, right))
        return nullptr;
    return foldMachineComparisonOfBoundedIntegers(builder, opcode, left, right);
}

ALWAYS_INLINE Operation* foldMachineBitAnd(Builder& builder, Type type, Operation* left, Operation* right)
{
    if (left == right)
        return left;
    if (!right->hasInt())
        return nullptr;

    int64_t mask = right->asInt();
    if (left->hasInt())
        return builder.constantOfType(type, left->asInt() & mask);
    if (!mask)
        return right;
    if (mask == -1)
        return left;

    if (!hasFoldTrait(left->opcode(), FoldTrait::BoundsIntegerRange))
        return nullptr;
    return foldMachineBitAndOfBoundedValue(builder, type, left, right);
}

inline bool mayBeHalfOfRotation(const Operation* value)
{
    return value->opcode() == Shl || value->opcode() == ZShr;
}

ALWAYS_INLINE Operation* foldMachineBitOr(Builder& builder, Type type, Operation* left, Operation* right)
{
    if (left == right)
        return left;
    if (!right->hasInt())
        return mayBeHalfOfRotation(left) || mayBeHalfOfRotation(right) ? foldMachineRotationOutOfShifts(builder, BitOr, type, left, right) : nullptr;

    int64_t bits = right->asInt();
    if (left->hasInt())
        return builder.constantOfType(type, left->asInt() | bits);
    if (!bits)
        return left;
    if (bits == -1)
        return right;
    if (!hasFoldTrait(left->opcode(), FoldTrait::BoundsIntegerRange))
        return nullptr;
    return foldMachineBitOrOfBoundedValue(builder, type, left, right);
}

ALWAYS_INLINE Operation* foldMachineBitXor(Builder& builder, Type type, Operation* left, Operation* right)
{
    if (left == right)
        return builder.constantOfType(type, 0);
    if (!right->hasInt())
        return mayBeHalfOfRotation(left) || mayBeHalfOfRotation(right) ? foldMachineRotationOutOfShifts(builder, BitXor, type, left, right) : nullptr;

    int64_t bits = right->asInt();
    if (left->hasInt())
        return builder.constantOfType(type, left->asInt() ^ bits);
    if (!bits)
        return left;

    Opcode leftOpcode = left->opcode();
    if (bits == 1 && isComparison(leftOpcode) && leftOpcode != EqualOrUnordered && left->child(0)->type().isInt())
        return builder.binary(invertedComparison(leftOpcode), left->child(0), left->child(1));

    if (leftOpcode == BitXor && left->child(1)->hasInt())
        return builder.binary(BitXor, left->child(0), builder.constantOfType(type, left->child(1)->asInt() ^ bits));

    return nullptr;
}

ALWAYS_INLINE Operation* foldBitwise(Builder& builder, Opcode opcode, Type type, Operation* left, Operation* rightOrNull)
{
    if (!type.isInt())
        return nullptr;

    if (opcode == BitAnd)
        return foldMachineBitAnd(builder, type, left, rightOrNull);
    if (opcode == BitOr)
        return foldMachineBitOr(builder, type, left, rightOrNull);
    if (opcode == BitXor)
        return foldMachineBitXor(builder, type, left, rightOrNull);
    return foldMachineShiftOrBitCount(builder, opcode, type, left, rightOrNull);
}

inline constexpr int64_t bitFoldLowWord = 0xffffffffll;

ALWAYS_INLINE Operation* foldMachineSignExtension32(Builder& builder, Operation* operand)
{
    if (operand->hasInt32())
        return builder.const64(operand->asInt32());

    if (operand->opcode() == Trunc) {
        Operation* wide = operand->child(0);
        if (IntegerRange { std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max() }.contains(rangeOf(wide)))
            return wide;
    }

    if (rangeOf(operand).min >= 0)
        return builder.unary(ZExt32, operand);

    return nullptr;
}

ALWAYS_INLINE Operation* foldMachineZeroExtension32(Builder& builder, Operation* operand)
{
    if (operand->hasInt32())
        return builder.const64(static_cast<uint32_t>(operand->asInt32()));

    if (operand->opcode() == Trunc) {
        Operation* wide = operand->child(0);
        if (IntegerRange { 0, bitFoldLowWord }.contains(rangeOf(wide)))
            return wide;
    }

    return nullptr;
}

ALWAYS_INLINE Operation* foldMachineTruncation(Builder& builder, Operation* operand)
{
    if (operand->hasInt64())
        return builder.const32(static_cast<int32_t>(operand->asInt64()));

    Opcode opcode = operand->opcode();
    if (opcode == SExt32 || opcode == ZExt32)
        return operand->child(0);

    if ((opcode == Add || opcode == Sub || opcode == BitOr || opcode == BitXor) && operand->child(1)->hasInt64() && !(operand->child(1)->asInt64() & bitFoldLowWord))
        return builder.unary(Trunc, operand->child(0));

    if (opcode == BitAnd && operand->child(1)->hasInt64() && (operand->child(1)->asInt64() & bitFoldLowWord) == bitFoldLowWord)
        return builder.unary(Trunc, operand->child(0));

    return nullptr;
}

ALWAYS_INLINE Operation* foldConversion(Builder& builder, Opcode opcode, Type result, Operation* operand)
{
    if (opcode == SExt32)
        return foldMachineSignExtension32(builder, operand);
    if (opcode == ZExt32)
        return foldMachineZeroExtension32(builder, operand);
    if (opcode == Trunc)
        return foldMachineTruncation(builder, operand);
    return foldConversionOtherThanOfWords(builder, opcode, result, operand);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
