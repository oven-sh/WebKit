/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTInductionVariables.h"
#include "AOTLoopEntry.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ConstantBound {
    SelectionGroup group;
    uint32_t numberOperand;
    int64_t bound;
};

struct BoundedComparison {
    ValueID value;
    uint32_t numberOperand;
};

struct LoopComparison {
    BoundedComparison comparison;
    ProgramPoint point;
    BlockID header;
    ValueID number;
};

struct HoistedBound {
    ValueID number;
    Opcode comparison;
    bool numberIsOnLeft;
    Vector<BoundedComparison, 2> comparisons;
};

struct LoopBounds {
    SelectionGroup group;
    BlockID header;
    Vector<HoistedBound, 2> bounds;
};

} // anonymous namespace

static constexpr std::array relationalOpcodes { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };

static_assert([] {
    OpcodeSet opcodes;
    for (Opcode opcode : relationalOpcodes)
        opcodes.add(opcode);
    return opcodes == relationalOperators;
}());

static std::optional<uint32_t> findNumberOperand(SelectionContext& context, const Value& value)
{
    bool integerIsFirst = context.typeOf(value.operand(0)).isInteger();
    ValueID integerOperand = value.operand(integerIsFirst ? 0 : 1);
    ValueID numberOperand = value.operand(integerIsFirst ? 1 : 0);
    const Type& numberType = context.typeOf(numberOperand);
    if (!context.typeOf(integerOperand).isInteger() || !numberType.isNumber() || numberType.isInteger())
        return std::nullopt;
    return integerIsFirst ? 1u : 0u;
}

static int64_t constantBoundOf(Opcode relation, double constant)
{
    bool roundsUp = relation == Opcode::Less || relation == Opcode::GreaterEq;
    bool isFalseAtGreatest = relation == Opcode::Greater || relation == Opcode::GreaterEq;
    constexpr int64_t least = -maxTrackedInteger;
    constexpr int64_t greatest = maxTrackedInteger;
    if (std::isnan(constant))
        return isFalseAtGreatest ? greatest : least;
    double rounded = roundsUp ? std::ceil(constant) : std::floor(constant);
    return rounded >= 0x1p62 ? greatest : rounded <= -0x1p62 ? least : static_cast<int64_t>(rounded);
}

static Vector<ConstantBound> findConstantBounds(SelectionContext& context)
{
    const Program& program = context.program();
    const Function& function = context.function();

    Vector<ConstantBound> matches;
    for (Opcode opcode : relationalOpcodes) {
        for (ValueID id : context.candidates(opcode)) {
            const Value& value = function.at(id);
            std::optional<uint32_t> numberOperand = findNumberOperand(context, value);
            if (!numberOperand)
                continue;
            std::optional<ValueID> constant = constantThroughCasts(program, function, value.operand(*numberOperand));
            if (!constant || function.at(*constant).opcode != Opcode::Double || context.isConditionWithProofs(id))
                continue;

            Opcode relation = *numberOperand ? opcode : mirroredComparison(opcode);
            ConstantBound match { groupOfOne(id), *numberOperand, constantBoundOf(relation, function.at(*constant).number()) };
            if (context.tryClaim(SelectionRuleID::IntegerBound, match.group) == ClaimResult::Claimed)
                matches.append(WTF::move(match));
        }
    }
    return matches;
}

static void rewriteConstantBound(GroupRewriter& rewriter, const ConstantBound& match)
{
    ValueID bound = rewriter.insert(makeInteger(match.bound));
    const Value& original = rewriter.original(match.group.root);
    Value& root = rewriter.replaceRoot(original.opcode);
    root.setLeftOperand(original.leftOperand());
    root.setRightOperand(original.rightOperand());
    root.setOperand(match.numberOperand, bound);
}

static Vector<LoopComparison> collectLoopComparisons(SelectionContext& context)
{
    const Function& function = context.function();

    Vector<LoopComparison> comparisons;
    for (Opcode opcode : relationalOpcodes) {
        for (ValueID id : context.candidates(opcode)) {
            const Value& value = function.at(id);
            std::optional<uint32_t> numberOperand = findNumberOperand(context, value);
            if (!numberOperand || context.isConditionWithProofs(id))
                continue;
            ProgramPoint point = context.analyses().useLists().pointOf(id);
            std::optional<InvariantOperand> invariant = findOutermostInvariantLoop(context, point.block, value.operand(*numberOperand), [&](ValueID number) {
                return context.typeOf(number).isNumber();
            });
            if (invariant)
                comparisons.append(LoopComparison { { id, *numberOperand }, point, invariant->header, invariant->value });
        }
    }
    return comparisons;
}

static void addComparison(const Function& function, LoopBounds& loopBounds, const LoopComparison& candidate)
{
    Opcode opcode = function.at(candidate.comparison.value).opcode;
    bool numberIsOnLeft = !candidate.comparison.numberOperand;
    for (HoistedBound& bound : loopBounds.bounds) {
        if (bound.number == candidate.number && bound.comparison == opcode && bound.numberIsOnLeft == numberIsOnLeft) {
            bound.comparisons.append(candidate.comparison);
            return;
        }
    }
    loopBounds.bounds.append(HoistedBound { candidate.number, opcode, numberIsOnLeft, { candidate.comparison } });
}

static Vector<LoopBounds> findLoopBounds(SelectionContext& context)
{
    Vector<LoopComparison> candidates = collectLoopComparisons(context);
    if (candidates.isEmpty())
        return { };

    const Function& function = context.function();
    const CFG& cfg = context.analyses().cfg();
    std::ranges::sort(candidates, { }, [&](const LoopComparison& candidate) {
        return std::tuple { cfg.reversePostOrderIndex(candidate.header), cfg.reversePostOrderIndex(candidate.point.block), candidate.point.index };
    });

    Vector<LoopBounds> loops;
    BlockID header;
    bool canEmit = false;
    for (const LoopComparison& candidate : candidates) {
        if (candidate.header != header) {
            header = candidate.header;
            canEmit = canEmitOnLoopEntry(context, header);
        }
        if (!canEmit)
            continue;

        SelectionGroup group = groupOfOne(candidate.comparison.value);
        if (context.tryClaim(SelectionRuleID::IntegerBound, group) != ClaimResult::Claimed)
            continue;
        if (loops.isEmpty() || loops.last().header != header)
            loops.append(LoopBounds { WTF::move(group), header, { } });
        addComparison(function, loops.last(), candidate);
    }
    return loops;
}

static Value makeIntegerBound(const HoistedBound& bound)
{
    RELEASE_ASSERT(relationalOperators.contains(bound.comparison));
    Value value = IRBuilder::make(Opcode::IntegerBound, { bound.number });
    value.setComparison(bound.comparison);
    value.setNumberIsOnLeft(bound.numberIsOnLeft);
    return value;
}

static void rewriteLoopBounds(SelectionContext& context, GroupRewriter& rewriter, const LoopBounds& match)
{
    Vector<ValueID, 2> boundValues;
    LoopEntryResult result = emitOnLoopEntry(context, rewriter.builder(), match.header, [&](IRBuilder& builder) {
        for (const HoistedBound& bound : match.bounds)
            boundValues.append(builder.append(makeIntegerBound(bound)));
    });
    if (result != LoopEntryResult::Emitted)
        return;

    Function& function = rewriter.function();
    for (size_t index = 0; index < match.bounds.size(); ++index) {
        for (const BoundedComparison& comparison : match.bounds[index].comparisons)
            function.at(comparison.value).setOperand(comparison.numberOperand, boundValues[index]);
    }
    context.analyses().didChangeValues();
}

void selectIntegerBound(SelectionContext& context)
{
    Vector<ConstantBound> constantBounds = findConstantBounds(context);
    context.rewriteGroups(SelectionRuleID::IntegerBound, constantBounds, [](GroupRewriter& rewriter, const ConstantBound& match) {
        rewriteConstantBound(rewriter, match);
    });

    Vector<LoopBounds> loopBounds = findLoopBounds(context);
    context.rewriteGroups(SelectionRuleID::IntegerBound, loopBounds, [&](GroupRewriter& rewriter, const LoopBounds& match) {
        rewriteLoopBounds(context, rewriter, match);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
