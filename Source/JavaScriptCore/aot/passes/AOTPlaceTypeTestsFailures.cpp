/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlaceTypeTestsPlan.h"

#if ENABLE(AOT)

#include "AOTForwardDataflow.h"
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

class FailedConditionsProblem {
    WTF_MAKE_NONCOPYABLE(FailedConditionsProblem);
public:
    using State = BitVector;

    FailedConditionsProblem(const Function& function, const BlockFrequencies& frequencies, std::span<const FailingEdge> failingEdges)
        : m_function(function)
        , m_frequencies(frequencies)
        , m_failingEdges(failingEdges)
    {
    }

    State stateAtEntry() const { return { }; }

    bool merge(State& into, const State& from) const
    {
        size_t numberOfConditions = into.bitCount();
        into.merge(from);
        return into.bitCount() != numberOfConditions;
    }

    void enterEdge(State& state, Edge edge) const
    {
        if (m_frequencies.classOf(targetOf(m_function, edge).block) == BlockFrequency::Normal) {
            state.clearAll();
            return;
        }
        if (edge.isHandlerEdge())
            return;
        for (const FailingEdge& failing : std::ranges::equal_range(m_failingEdges, EdgeKey { edge.from.index(), edge.targetIndex }, { }, &FailingEdge::key))
            state.set(failing.condition.index());
    }

    void execute(State&, const DataflowStep&) const { }
    void executeTerminator(State&, const DataflowTerminatorStep&) const { }

private:
    const Function& m_function;
    const BlockFrequencies& m_frequencies;
    std::span<const FailingEdge> m_failingEdges;
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

static Vector<FailingEdge> collectFailingEdges(const Function& function, const CFG& cfg, const BlockFrequencies& frequencies, TestConditions& conditions)
{
    auto leavesNormalCode = [&](const Target& failing) {
        return frequencies.classOf(failing.block) != BlockFrequency::Normal;
    };

    Vector<FailingEdge> failingEdges;
    for (BlockID block : cfg.reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.isConditionalBranch() && leavesNormalCode(terminator.notTaken()))
            appendFailingEdgeOfBranch(function, conditions, block, failingEdges);
        else if (terminator.kind == TerminatorKind::SwitchOnShape && leavesNormalCode(terminator.otherwise()))
            appendFailingEdgeOfSwitch(function, conditions, block, failingEdges);
    }
    std::ranges::sort(failingEdges, { }, &FailingEdge::key);
    return failingEdges;
}

FailedConditions::FailedConditions(FunctionAnalyses& analyses)
    : m_conditions(analyses)
{
    const BlockFrequencies& frequencies = analyses.blockFrequencies();
    Vector<FailingEdge> failingEdges = collectFailingEdges(analyses.function(), analyses.cfg(), frequencies, m_conditions);
    if (failingEdges.isEmpty())
        return;

    FailedConditionsProblem problem(analyses.function(), frequencies, failingEdges.span());
    m_failedAtHead = analyses.solveForward(problem);
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
