/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTSelectionContext.h"
#include "AOTSort.h"
#include "AOTStringRuleSupport.h"
#include "AOTVector.h"
#include <algorithm>
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

struct StringOrderMatch {
    SelectionGroup group;
    uint32_t order;
    bool isMirrored;
};

} // anonymous namespace

static uint64_t operandPairOf(const StringComparison& comparison)
{
    return static_cast<uint64_t>(std::min(comparison.left, comparison.right).index()) << 32 | std::max(comparison.left, comparison.right).index();
}

static Vector<SharedOrder> findSharedOrders(SelectionContext& context)
{
    const Function& function = context.function();
    FunctionAnalyses& analyses = context.analyses();

    Vector<StringComparison> comparisons;
    for (Opcode opcode : relationalOpcodesOfStrings) {
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
    if (comparisons.size() < 2)
        return { };

    const CFG& cfg = analyses.cfg();
    AOT::sort(comparisons, { }, [&](const StringComparison& comparison) {
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

void selectSharedStringOrder(SelectionContext& context)
{
    Vector<SharedOrder> sharedOrders = findSharedOrders(context);
    sharedOrders.removeAllMatching([&](const SharedOrder& order) {
        Vector<SelectionGroup, 4> groups { groupOfOne(order.first.value) };
        for (const StringComparison& other : order.others)
            groups.append(groupOfOne(other.value));
        return context.tryClaimAll(SelectionRuleID::SharedStringOrder, groups.span()) != ClaimResult::Claimed;
    });

    Vector<StringOrderMatch> firstComparisons;
    Vector<StringOrderMatch> laterComparisons;
    for (uint32_t index = 0; index < sharedOrders.size(); ++index) {
        const SharedOrder& shared = sharedOrders[index];
        firstComparisons.append(StringOrderMatch { groupOfOne(shared.first.value), index, false });
        for (const StringComparison& other : shared.others)
            laterComparisons.append(StringOrderMatch { groupOfOne(other.value), index, other.left != shared.first.left });
    }

    // The first comparison of an order computes it.
    Vector<ValueID> orders(sharedOrders.size());
    auto rewrite = [&](GroupRewriter& rewriter, const StringOrderMatch& match) {
        const Value& original = rewriter.original(match.group.root);
        Opcode opcode = original.opcode;
        if (!orders[match.order])
            orders[match.order] = rewriter.insert(IRBuilder::make(Opcode::StringOrder, { original.leftOperand(), original.rightOperand() }));
        ValueID zero = rewriter.insert(makeInteger(0));
        Value& root = rewriter.replaceRoot(match.isMirrored ? mirroredComparison(opcode) : opcode);
        root.setLeftOperand(orders[match.order]);
        root.setRightOperand(zero);
    };
    context.rewriteGroups(SelectionRuleID::SharedStringOrder, firstComparisons, rewrite);
    context.rewriteGroups(SelectionRuleID::SharedStringOrder, laterComparisons, rewrite);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
