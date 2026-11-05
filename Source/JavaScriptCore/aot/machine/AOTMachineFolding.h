/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineBuilder.h"

namespace JSC { namespace AOT { namespace Machine {

Operation* foldIntegerArithmetic(Builder&, Opcode, Type, Operation* left, Operation* rightOrNull, bool isChill);
Operation* foldBitwise(Builder&, Opcode, Type, Operation* left, Operation* rightOrNull);
Operation* foldConversion(Builder&, Opcode, Type result, Operation*);
Operation* foldLowWord(Builder&, Operation* wide);
Operation* foldFloatingPoint(Builder&, Opcode, Type, Operation* left, Operation* rightOrNull);
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
