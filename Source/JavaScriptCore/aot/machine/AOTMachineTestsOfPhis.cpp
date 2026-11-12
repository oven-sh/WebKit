/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineTestsOfPhis.h"

#if ENABLE(AOT)

#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Machine {

static constexpr unsigned levelsOfTestOfPhi = 4;

static int64_t wordOfTestOfPhi(Type type, uint64_t bits)
{
    return type == Int32 ? static_cast<int32_t>(static_cast<uint32_t>(bits)) : static_cast<int64_t>(bits);
}

static std::optional<int64_t> valueOfTestOfPhiWithinLevels(const Operation* operation, const Operation* phi, int64_t brought, unsigned levels)
{
    if (operation == phi)
        return brought;
    if (operation->hasInt())
        return operation->asInt();
    if (!levels || operation->hasNoPosition() || !operation->type().isInt())
        return std::nullopt;

    Opcode opcode = operation->opcode();
    bool isConversion = opcode == Trunc || opcode == ZExt32 || opcode == SExt32;
    bool isArithmetic = opcode == Add || opcode == Sub || opcode == BitAnd || opcode == BitOr || opcode == BitXor;
    bool isShift = opcode == Shl || opcode == SShr || opcode == ZShr;
    bool comparesIntegers = isComparison(opcode) && opcode != EqualOrUnordered && operation->child(0)->type().isInt();
    if (!isConversion && !isArithmetic && !isShift && !comparesIntegers)
        return std::nullopt;

    std::optional<int64_t> left = valueOfTestOfPhiWithinLevels(operation->child(0), phi, brought, levels - 1);
    if (!left)
        return std::nullopt;
    if (opcode == SExt32)
        return left;
    if (opcode == ZExt32)
        return static_cast<int64_t>(static_cast<uint32_t>(*left));
    if (opcode == Trunc)
        return wordOfTestOfPhi(Int32, static_cast<uint64_t>(*left));

    std::optional<int64_t> right = valueOfTestOfPhiWithinLevels(operation->child(1), phi, brought, levels - 1);
    if (!right)
        return std::nullopt;
    Type type = operation->type();
    uint64_t a = static_cast<uint64_t>(*left);
    uint64_t b = static_cast<uint64_t>(*right);
    if (isShift) {
        bool is32 = type == Int32;
        unsigned amount = static_cast<unsigned>(b) & (is32 ? 31 : 63);
        if (opcode == Shl)
            return wordOfTestOfPhi(type, a << amount);
        if (opcode == SShr)
            return *left >> amount;
        return wordOfTestOfPhi(type, (is32 ? static_cast<uint32_t>(a) : a) >> amount);
    }
    if (opcode == Add)
        return wordOfTestOfPhi(type, a + b);
    if (opcode == Sub)
        return wordOfTestOfPhi(type, a - b);
    if (opcode == BitAnd)
        return wordOfTestOfPhi(type, a & b);
    if (opcode == BitOr)
        return wordOfTestOfPhi(type, a | b);
    if (opcode == BitXor)
        return wordOfTestOfPhi(type, a ^ b);

    if (opcode == Equal)
        return *left == *right;
    if (opcode == NotEqual)
        return *left != *right;
    if (opcode == LessThan)
        return *left < *right;
    if (opcode == GreaterThan)
        return *left > *right;
    if (opcode == LessEqual)
        return *left <= *right;
    if (opcode == GreaterEqual)
        return *left >= *right;
    if (opcode == Below)
        return a < b;
    if (opcode == Above)
        return a > b;
    if (opcode == BelowEqual)
        return a <= b;
    RELEASE_ASSERT(opcode == AboveEqual);
    return a >= b;
}

std::optional<int64_t> valueOfTestWherePhiHolds(const Operation* tested, const Operation* phi, int64_t brought)
{
    return valueOfTestOfPhiWithinLevels(tested, phi, brought, levelsOfTestOfPhi);
}

bool holdsNothingButTestOfItsPhi(const Block* join, const Operation* phi)
{
    ASSERT(phi->owner() == join);
    return testsOnlyPhi(join, phi);
}

bool testsOnlyPhi(const Block* join, const Operation* phi)
{
    const Operation* branch = join->last();
    ASSERT(branch->opcode() == Branch && phi->useCount() <= 2);
    if (branch->child(0) == phi || !valueOfTestWherePhiHolds(branch->child(0), phi, 0))
        return false;

    uint32_t numberThatStay = 1;
    for (const Operation* operation : join->operations()) {
        if (operation == branch || (!operation->useCount() && !operation->mustStay()))
            continue;
        if (operation->useCount() != 1 || operation->mustStay())
            return false;
        ++numberThatStay;
    }
    uint32_t numberBelowBranch = 0;
    bool readsPhi = false;
    Vector<const Operation*, 8> below { branch };
    while (!below.isEmpty()) {
        const Operation* operation = below.takeLast();
        ++numberBelowBranch;
        for (const Operation* child : operation->children()) {
            readsPhi |= child == phi;
            if (child->owner() == join && child->opcode() != Phi)
                below.append(child);
        }
    }
    return readsPhi && numberBelowBranch == numberThatStay;
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
