/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTInductionVariables.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include <algorithm>
#include <array>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct StringComparison {
    ValueID value;
    // The operands through casts.
    ValueID left;
    ValueID right;
    ProgramPoint point;
};

struct SharedOrder {
    StringComparison first;
    Vector<StringComparison, 2> others;
};

struct Match {
    SelectionGroup group;
    uint32_t order;
    bool isMirrored;
};

} // anonymous namespace

static constexpr std::array relationalOpcodes { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };

static_assert([] {
    OpcodeSet opcodes;
    for (Opcode opcode : relationalOpcodes)
        opcodes.add(opcode);
    return opcodes == relationalOperators;
}());

static uint64_t orderedPair(ValueID a, ValueID b)
{
    return static_cast<uint64_t>(a.index()) << 32 | b.index();
}

static uint64_t operandPairOf(const StringComparison& comparison)
{
    return orderedPair(std::min(comparison.left, comparison.right), std::max(comparison.left, comparison.right));
}

static Vector<StringComparison> collectStringComparisons(SelectionContext& context)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();

    Vector<StringComparison> comparisons;
    for (Opcode opcode : relationalOpcodes) {
        for (ValueID id : context.candidates(opcode)) {
            const Value& value = function.at(id);
            if (!context.typeOf(value.leftOperand()).isAnyString() || !context.typeOf(value.rightOperand()).isAnyString())
                continue;
            if (context.isOnSlowPath(id) || context.isConditionWithProofs(id))
                continue;
            ValueID left = analyses.sameObjectThroughCasts(value.leftOperand());
            ValueID right = analyses.sameObjectThroughCasts(value.rightOperand());
            if (left != right)
                comparisons.append(StringComparison { id, left, right, analyses.useLists().pointOf(id) });
        }
    }
    return comparisons;
}

static Vector<SharedOrder> findSharedOrders(SelectionContext& context)
{
    Vector<StringComparison> comparisons = collectStringComparisons(context);
    if (comparisons.size() < 2)
        return { };

    FunctionAnalyses& analyses = context.analyses();
    const CFG& cfg = analyses.cfg();
    std::ranges::sort(comparisons, { }, [&](const StringComparison& comparison) {
        return std::tuple { operandPairOf(comparison), cfg.reversePostOrderIndex(comparison.point.block), comparison.point.index };
    });

    Vector<SharedOrder> orders;
    size_t firstOrderOfPair = 0;
    for (size_t index = 0; index < comparisons.size(); ++index) {
        const StringComparison& comparison = comparisons[index];
        if (index && operandPairOf(comparisons[index - 1]) != operandPairOf(comparison))
            firstOrderOfPair = orders.size();
        auto ordersOfPair = orders.mutableSpan().subspan(firstOrderOfPair);
        auto dominating = std::ranges::find_if(ordersOfPair, [&](const SharedOrder& order) {
            return analyses.dominates(order.first.value, comparison.point);
        });
        if (dominating == ordersOfPair.end())
            orders.append(SharedOrder { comparison, { } });
        else
            dominating->others.append(comparison);
    }
    orders.removeAllMatching([](const SharedOrder& order) {
        return order.others.isEmpty();
    });
    return orders;
}

static Vector<SelectionGroup, 4> groupsOf(const SharedOrder& order)
{
    Vector<SelectionGroup, 4> groups { groupOfOne(order.first.value) };
    for (const StringComparison& other : order.others)
        groups.append(groupOfOne(other.value));
    return groups;
}

static void replaceWithOrderTest(GroupRewriter& rewriter, const Match& match, ValueID order)
{
    Opcode opcode = rewriter.original(match.group.root).opcode;
    ValueID zero = rewriter.insert(makeInteger(0));
    Value& root = rewriter.replaceRoot(match.isMirrored ? mirroredComparison(opcode) : opcode);
    root.setLeftOperand(order);
    root.setRightOperand(zero);
}

void selectSharedStringOrder(SelectionContext& context)
{
    Vector<SharedOrder> sharedOrders = findSharedOrders(context);
    sharedOrders.removeAllMatching([&](const SharedOrder& order) {
        return context.tryClaimAll(SelectionRuleID::SharedStringOrder, groupsOf(order).span()) != ClaimResult::Claimed;
    });

    Vector<Match> firstComparisons;
    Vector<Match> laterComparisons;
    for (uint32_t index = 0; index < sharedOrders.size(); ++index) {
        const SharedOrder& shared = sharedOrders[index];
        firstComparisons.append(Match { groupOfOne(shared.first.value), index, false });
        for (const StringComparison& other : shared.others)
            laterComparisons.append(Match { groupOfOne(other.value), index, other.left != shared.first.left });
    }

    Vector<ValueID> orders(sharedOrders.size());
    context.rewriteGroups(SelectionRuleID::SharedStringOrder, firstComparisons, [&](GroupRewriter& rewriter, const Match& match) {
        const Value& original = rewriter.original(match.group.root);
        orders[match.order] = rewriter.insert(IRBuilder::make(Opcode::StringOrder, { original.leftOperand(), original.rightOperand() }));
        replaceWithOrderTest(rewriter, match, orders[match.order]);
    });
    context.rewriteGroups(SelectionRuleID::SharedStringOrder, laterComparisons, [&](GroupRewriter& rewriter, const Match& match) {
        replaceWithOrderTest(rewriter, match, orders[match.order]);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
