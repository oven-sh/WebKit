/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTForwardDataflow.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using EdgeKey = std::pair<uint32_t, uint32_t>;

struct FailingEdge {
    EdgeKey key() const { return { edge.from.index(), edge.targetIndex }; }

    Edge edge;
    TestConditionID condition;
};

} // anonymous namespace

static Edge edgeTo(BlockID from, const Terminator& terminator, const Target& target)
{
    return Edge { from, static_cast<uint32_t>(&target - terminator.targets.span().data()) };
}

static void appendFailingEdgeOfBranch(const Function& function, TestConditions& conditions, BlockID block, Vector<FailingEdge>& failingEdges)
{
    const Terminator& terminator = function.at(block).terminator;
    const Value& test = function.at(terminator.conditionOperand());
    if (provesOf(test.opcode) != Proof::None)
        failingEdges.append(FailingEdge { edgeTo(block, terminator, terminator.notTaken()), conditions.add(test) });
}

static void appendFailingEdgeOfSwitch(const Function& function, TestConditions& conditions, BlockID block, Vector<FailingEdge>& failingEdges)
{
    const Terminator& terminator = function.at(block).terminator;
    Edge otherwise = edgeTo(block, terminator, terminator.otherwise());
    for (LayoutSetID layouts : terminator.layoutSets())
        failingEdges.append(FailingEdge { otherwise, conditions.add(makeShapeTest(terminator.subjectOperand(), layouts)) });
}

static Vector<FailingEdge> collectFailingEdges(const Function& function, const CFG& cfg, TestConditions& conditions)
{
    auto leavesNormalCode = [&](const Target& failing) {
        return function.at(failing.block).frequency != BlockFrequency::Normal;
    };

    Vector<FailingEdge> failingEdges;
    for (BlockID block : cfg.reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.isConditionalBranch() && leavesNormalCode(terminator.notTaken()))
            appendFailingEdgeOfBranch(function, conditions, block, failingEdges);
        else if (terminator.kind == TerminatorKind::SwitchOnShape && leavesNormalCode(terminator.otherwise()))
            appendFailingEdgeOfSwitch(function, conditions, block, failingEdges);
    }
    AOT::sort(failingEdges, { }, &FailingEdge::key);
    return failingEdges;
}

FailedConditions::FailedConditions(FunctionAnalyses& analyses)
    : m_conditions(analyses)
{
    const Function& function = analyses.function();
    const CFG& cfg = analyses.cfg();
    Vector<FailingEdge> failingEdges = collectFailingEdges(function, cfg, m_conditions);
    if (failingEdges.isEmpty())
        return;

    // What has failed goes on along every edge and is forgotten where normal code is entered: it is known in the marked blocks behind the failing edges and nowhere else.
    std::span<const BlockID> order = cfg.reversePostOrder();
    m_failedAtHead.grow(function.blocks.size());
    BitVector isPending(order.size());
    auto failedAtHeadOf = [&](BlockID block) -> BitVector& {
        std::optional<BitVector>& failed = m_failedAtHead[block.index()];
        if (!failed)
            failed.emplace();
        return *failed;
    };
    for (const FailingEdge& failing : failingEdges) {
        BlockID target = targetOf(function, failing.edge).block;
        if (failedAtHeadOf(target).add(failing.condition.index()))
            isPending.quickSet(cfg.reversePostOrderIndex(target));
    }
    for (bool shouldRepeat = true; shouldRepeat;) {
        shouldRepeat = false;
        for (size_t position : isPending) {
            isPending.quickClear(position);
            BlockID block = order[position];
            function.at(block).forEachTarget([&](const Target& target) {
                if (target.block == block || function.at(target.block).frequency == BlockFrequency::Normal)
                    return;
                BitVector& failed = failedAtHeadOf(target.block);
                size_t numberOfConditions = failed.bitCount();
                failed.merge(*m_failedAtHead[block.index()]);
                if (failed.bitCount() == numberOfConditions)
                    return;
                uint32_t positionOfTarget = cfg.reversePostOrderIndex(target.block);
                isPending.quickSet(positionOfTarget);
                shouldRepeat |= positionOfTarget <= position;
            });
        }
    }
}

bool FailedConditions::mayHaveFailed(const Value& test, BlockID at) const
{
    if (m_failedAtHead.isEmpty())
        return false;
    const std::optional<BitVector>& failed = m_failedAtHead[at.index()];
    if (!failed || failed->isEmpty())
        return false;
    std::optional<TestConditionID> condition = m_conditions.find(test);
    return condition && failed->get(condition->index());
}

bool FailedConditions::mayHaveFailedTestOf(ValueID subject, BlockID at) const
{
    if (m_failedAtHead.isEmpty())
        return false;
    const std::optional<BitVector>& failed = m_failedAtHead[at.index()];
    if (!failed)
        return false;
    for (size_t condition : *failed) {
        if (m_conditions.at(TestConditionID(condition)).subject == subject)
            return true;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
