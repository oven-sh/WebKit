/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3ConstantReplacements.h"

#if ENABLE(B3_JIT)

#include "B3IntRange.h"
#include "B3ValueInlines.h"
#include <bit>
#include <cmath>
#include <type_traits>

namespace JSC { namespace B3 {

std::optional<uint64_t> bitsOfConstant(const Value* value)
{
    if (value->hasInt32())
        return static_cast<uint32_t>(value->asInt32());
    if (value->hasInt64())
        return static_cast<uint64_t>(value->asInt64());
    if (value->hasDouble())
        return std::bit_cast<uint64_t>(value->asDouble());
    if (value->hasFloat())
        return std::bit_cast<uint32_t>(value->asFloat());
    return std::nullopt;
}

std::optional<ConstantReplacement> findConstantReplacement(Value* select)
{
    if (select->opcode() != Select)
        return std::nullopt;

    Value* tested = select->child(0);
    uint64_t replaced = 0;
    bool holdsForReplaced = false;
    if ((tested->opcode() == Equal || tested->opcode() == NotEqual) && tested->child(1)->hasInt()) {
        holdsForReplaced = tested->opcode() == Equal;
        replaced = *bitsOfConstant(tested->child(1));
        tested = tested->child(0);
    }

    Value* original = select->child(holdsForReplaced ? 2 : 1);
    bool testsBitsOfOriginal = tested == original
        || (tested->opcode() == BitwiseCast && tested->child(0) == original)
        || (original->opcode() == BitwiseCast && original->child(0) == tested);
    if (!testsBitsOfOriginal)
        return std::nullopt;

    std::optional<uint64_t> replacement = bitsOfConstant(select->child(holdsForReplaced ? 1 : 2));
    if (!replacement)
        return std::nullopt;
    return ConstantReplacement { original, tested, replaced, *replacement };
}

template<typename Signed>
static std::optional<bool> compareIntegers(Opcode opcode, uint64_t left, uint64_t right)
{
    auto intervalOf = [&](uint64_t bits) -> IntegerInterval {
        Int128 integer = isUnsignedRelation(opcode) ? Int128 { static_cast<std::make_unsigned_t<Signed>>(bits) } : Int128 { static_cast<Signed>(bits) };
        return { integer, integer };
    };

    switch (evaluateComparison(opcode, intervalOf(left), intervalOf(right))) {
    case TriState::False:
        return false;
    case TriState::True:
        return true;
    case TriState::Indeterminate:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename FloatingPoint>
static std::optional<bool> compareFloatingPoint(Opcode opcode, FloatingPoint left, FloatingPoint right)
{
    switch (opcode) {
    case Equal:
        return left == right;
    case NotEqual:
        return left != right;
    case LessThan:
        return left < right;
    case GreaterThan:
        return left > right;
    case LessEqual:
        return left <= right;
    case GreaterEqual:
        return left >= right;
    case EqualOrUnordered:
        return std::isunordered(left, right) || left == right;
    default:
        return std::nullopt;
    }
}

std::optional<bool> evaluateComparisonOfBits(Opcode opcode, Type type, uint64_t left, uint64_t right)
{
    switch (type.kind()) {
    case Int32:
        return compareIntegers<int32_t>(opcode, left, right);
    case Int64:
        return compareIntegers<int64_t>(opcode, left, right);
    case Float:
        return compareFloatingPoint(opcode, std::bit_cast<float>(static_cast<uint32_t>(left)), std::bit_cast<float>(static_cast<uint32_t>(right)));
    case Double:
        return compareFloatingPoint(opcode, std::bit_cast<double>(left), std::bit_cast<double>(right));
    default:
        return std::nullopt;
    }
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
