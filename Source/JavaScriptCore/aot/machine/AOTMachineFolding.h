/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineBuilder.h"

namespace JSC { namespace AOT { namespace Machine {

// Each answers with a value that stands for the requested operation, or with null where it knows none. The operands are final: they have been folded themselves.
// A rule may look at an operand's opcode, children and constant. It may not ask how often an operand is used: use counts are not final yet.
// What a rule builds it builds through the Builder, so it is folded and numbered in turn. Each rule builds operations that are smaller than the request, so the chain ends.
// A commutative request arrives with a constant operand, if it has one, on the right.

// Add, Sub, Mul, Neg, Div, UDiv, Mod, UMod, MulHigh, UMulHigh of integers.
Operation* foldIntegerArithmetic(Builder&, Opcode, Type, Operation* left, Operation* rightOrNull, bool isChill);
// BitAnd, BitOr, BitXor, Shl, SShr, ZShr, Clz, PopCount.
Operation* foldBitwise(Builder&, Opcode, Type, Operation* left, Operation* rightOrNull);
// SExt8, SExt16, SExt32, ZExt32, Trunc, IToD, FloatToDouble, DoubleToFloat, BitwiseCast.
Operation* foldConversion(Builder&, Opcode, Type result, Operation*);
// Builder::lowWord(). The low word of an Int64, made of the low words of what the Int64 is made of, where that takes fewer operations than the Int64 and a Trunc of it.
// Add, Sub, Mul, Neg, BitAnd, BitOr, BitXor and Shl by a constant below 32, however many of them stand on one another.
Operation* foldLowWord(Builder&, Operation* wide);
// Add, Sub, Mul, Div, Neg of numbers, and Abs, Ceil, Floor, FTrunc, Sqrt, FMin, FMax.
Operation* foldFloatingPoint(Builder&, Opcode, Type, Operation* left, Operation* rightOrNull);
// The eleven comparisons, of integers and of numbers.
Operation* foldComparison(Builder&, Opcode, Operation* left, Operation* right);
Operation* foldSelect(Builder&, Operation* predicate, Operation* whenNotZero, Operation* whenZero);

namespace FoldTrait {
enum : uint8_t {
    IsPeeledOffPredicates = 1 << 0,
    MayBeBoolean = 1 << 1,
    BoundsIntegerRange = 1 << 2,
};
}

constexpr std::array<uint8_t, numberOfOpcodes> listFoldTraitsOfOpcodes()
{
    std::array<uint8_t, numberOfOpcodes> traits { };
    for (Opcode opcode : { Equal, NotEqual, BitXor, ZExt32, SExt32, Trunc })
        traits[opcode] |= FoldTrait::IsPeeledOffPredicates;
    for (Opcode opcode : { Const32, Const64, Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual, EqualOrUnordered, BitAnd, BitOr, BitXor, Select, ZExt32, SExt32, Trunc })
        traits[opcode] |= FoldTrait::MayBeBoolean;
    for (Opcode opcode : { Const32, Const64, Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual, EqualOrUnordered, Load8Z, Load8S, Load16Z, Load16S, Clz, PopCount, SExt8, SExt16, SExt32, Trunc, ZExt32, Add, Sub, Neg, Mul, BitAnd, BitOr, BitXor, Shl, SShr, ZShr, Select })
        traits[opcode] |= FoldTrait::BoundsIntegerRange;
    return traits;
}
inline constexpr std::array<uint8_t, numberOfOpcodes> foldTraitsOfOpcodes = listFoldTraitsOfOpcodes();

constexpr bool hasFoldTrait(Opcode opcode, uint8_t trait) { return foldTraitsOfOpcodes[opcode] & trait; }

// What a Branch, a Select or a Check may test in place of its predicate.
struct SimplifiedPredicate {
    Operation* predicate;
    // The two ways out change places.
    bool isInverted { false };
};
SimplifiedPredicate peelPredicate(Operation* predicate);
inline SimplifiedPredicate simplifyPredicate(Operation* predicate)
{
    if (!hasFoldTrait(predicate->opcode(), FoldTrait::IsPeeledOffPredicates))
        return { predicate };
    return peelPredicate(predicate);
}

// Whether every value of the operation is 0 or 1.
bool isBooleanByItsChildren(const Operation*);
inline bool isBoolean(const Operation* operation)
{
    Opcode opcode = operation->opcode();
    return hasFoldTrait(opcode, FoldTrait::MayBeBoolean) && (isComparison(opcode) || isBooleanByItsChildren(operation));
}

// What the opcode, the constants and the children down to the third level say of an integer, read as signed 64 bits. An Int32 is read as its sign extension.
// Load8Z, Load16Z, ZExt32, a comparison, BitAnd with a constant that is not negative, ZShr by a constant, Clz, PopCount, Select and Add, Sub, Shl of what is known, where nothing wraps.
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

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
