/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeTestCoverage.h"

#if ENABLE(AOT)

#include <algorithm>
#include <utility>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

// No budget bridges a gap wider than the largest one, so one width stands for all of them. That bounds the solve.
static uint32_t widenedGap(uint32_t gap, uint32_t bytes, uint32_t largestBudget)
{
    return std::min(saturatingSum<uint32_t>(gap, bytes), saturatingSum<uint32_t>(largestBudget, 1u));
}

static void restartGap(TestState& state, TestConditionID condition)
{
    if (!state.bytesSinceUse.isEmpty())
        state.bytesSinceUse[condition.index()] = 0;
}

static void markAsPassed(TestState& state, TestConditionID condition)
{
    state.passed.set(condition.index());
    restartGap(state, condition);
}

static bool isIgnoredFailingEdge(const TestCoverageInputs& inputs, Edge edge)
{
    if (!inputs.blocksWithIgnoredFailingEdge || !inputs.blocksWithIgnoredFailingEdge->get(edge.from.index()))
        return false;
    const Function& function = inputs.events.function();
    return &targetOf(function, edge) == &function.at(edge.from).terminator.notTaken();
}

TestCoverageProblem::TestCoverageProblem(const TestCoverageInputs& inputs)
    : m_inputs(inputs)
    , m_cycles(inputs.events.analyses().blockCycles())
{
    const GapBudget& budget = inputs.budget;
    RELEASE_ASSERT(budget.outsideCycles.has_value() == budget.insideCycles.has_value());
    if (budget.outsideCycles)
        m_largestBudget = std::max(*budget.outsideCycles, *budget.insideCycles);
}

TestState TestCoverageProblem::stateAtEntry() const
{
    TestState state;
    if (m_largestBudget)
        state.bytesSinceUse.fill(0, m_inputs.events.conditions().size());
    return state;
}

bool TestCoverageProblem::merge(State& into, const State& from) const
{
    if (!from.isReachable)
        return false;
    if (!into.isReachable) {
        into = from;
        settlePendingBytes(into);
        return true;
    }

    settlePendingBytes(into);
    bool changed = false;
    BitVector passedBefore = into.passed;
    for (size_t condition : passedBefore) {
        if (!from.passed.get(condition)) {
            into.passed.clear(condition);
            if (m_largestBudget)
                into.bytesSinceUse[condition] = 0;
            changed = true;
            continue;
        }
        if (!m_largestBudget)
            continue;
        uint32_t gap = widenedGap(from.bytesSinceUse[condition], from.pendingBytes, *m_largestBudget);
        if (gap > into.bytesSinceUse[condition]) {
            into.bytesSinceUse[condition] = gap;
            changed = true;
        }
    }
    return changed;
}

void TestCoverageProblem::enterEdge(State& state, Edge edge) const
{
    if (!state.isReachable)
        return;
    if (edge.isHandlerEdge()) {
        endAll(state);
        return;
    }
    if (isIgnoredFailingEdge(m_inputs, edge) || leavesOut(edge)) {
        state.isReachable = false;
        endAll(state);
        return;
    }

    const TestEvents& events = m_inputs.events;
    settlePendingBytes(state);
    events.forEachProofOn(edge, [&](uint32_t proof) {
        establish(state, events.proofs()[proof].condition);
    });
    events.forEachHoistedTestOn(edge, [&](uint32_t hoistedTest) {
        establish(state, events.hoistedTests()[hoistedTest].condition);
    });
}

bool TestCoverageProblem::leavesOut(Edge edge) const
{
    return m_inputs.slowPaths == SlowPaths::MayLeave && m_inputs.events.isRejoiningEdge(edge);
}

void TestCoverageProblem::execute(State& state, const DataflowStep& step) const
{
    if (!state.isReachable || !m_inputs.events.hasEventAt(step.id))
        return;
    for (const TestEvent& event : eventsAt(step.block, step.index))
        applyEvent(state, step.block, event);
}

void TestCoverageProblem::executeValuesOf(State& state, BlockID block) const
{
    uint32_t terminatorIndex = m_inputs.events.function().at(block).values.size();
    std::optional<uint32_t> index;
    for (const TestEvent& event : m_inputs.events.eventsOf(block)) {
        if (event.index >= terminatorIndex)
            break;
        if (index != event.index) {
            if (!state.isReachable)
                break;
            index = event.index;
        }
        applyEvent(state, block, event);
    }
}

void TestCoverageProblem::executeTerminator(State& state, const DataflowTerminatorStep& step) const
{
    if (!state.isReachable)
        return;
    const TestEvents& events = m_inputs.events;
    uint32_t terminatorIndex = events.function().at(step.block).values.size();
    for (const TestEvent& event : eventsAt(step.block, terminatorIndex))
        applyEvent(state, step.block, event);
    state.pendingBytes = saturatingSum<uint32_t>(state.pendingBytes, events.bytesAfterLastEventOf(step.block));
}

RequirementCoverage TestCoverageProblem::classify(const State& state, const TestRequirement& requirement) const
{
    return classify(state, requirement.condition, requirement.point.block);
}

RequirementCoverage TestCoverageProblem::classify(const State& state, TestConditionID condition, BlockID block) const
{
    ASSERT(!state.pendingBytes);
    if (!state.passed.get(condition.index()))
        return RequirementCoverage::NeedsTest;
    if (std::optional<uint32_t> budget = budgetIn(block); budget && state.bytesSinceUse[condition.index()] > *budget)
        return RequirementCoverage::NeedsTest;
    return RequirementCoverage::Covered;
}

void TestCoverageProblem::applyEvent(State& state, BlockID block, const TestEvent& event) const
{
    const TestEvents& events = m_inputs.events;
    state.pendingBytes = saturatingSum<uint32_t>(state.pendingBytes, event.bytesBefore);
    switch (event.kind) {
    case TestEvent::Kind::ParameterProof:
    case TestEvent::Kind::ValueProof:
        settlePendingBytes(state);
        establish(state, events.proofs()[event.payload].condition);
        return;
    case TestEvent::Kind::Barrier:
    case TestEvent::Kind::Suspension:
        endAll(state);
        return;
    case TestEvent::Kind::Requirement: {
        const TestRequirement& requirement = events.requirements()[event.payload];
        ASSERT_UNUSED(block, requirement.point.block == block);
        applyRequirement(state, requirement);
        return;
    }
    case TestEvent::Kind::Ending:
        end(state, events.endedBy(event));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TestCoverageProblem::settlePendingBytes(State& state) const
{
    uint32_t bytes = std::exchange(state.pendingBytes, 0u);
    if (!m_largestBudget || !bytes)
        return;
    for (size_t condition : state.passed)
        state.bytesSinceUse[condition] = widenedGap(state.bytesSinceUse[condition], bytes, *m_largestBudget);
}

std::optional<uint32_t> TestCoverageProblem::budgetIn(BlockID block) const
{
    return m_cycles.isInCycle(block) ? m_inputs.budget.insideCycles : m_inputs.budget.outsideCycles;
}

std::span<const TestEvent> TestCoverageProblem::eventsAt(BlockID block, uint32_t index) const
{
    return std::span<const TestEvent>(std::ranges::equal_range(m_inputs.events.eventsOf(block), index, { }, &TestEvent::index));
}

void TestCoverageProblem::applyRequirement(State& state, const TestRequirement& requirement) const
{
    switch (requirement.kind) {
    case RequirementKind::AsksOnly:
        return;
    case RequirementKind::PlacesTest:
        break;
    }

    settlePendingBytes(state);
    switch (classify(state, requirement)) {
    case RequirementCoverage::Covered:
        restartGap(state, requirement.condition);
        return;
    case RequirementCoverage::NeedsTest:
        establish(state, requirement.condition);
        return;
    case RequirementCoverage::Unknown:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void TestCoverageProblem::establish(State& state, TestConditionID condition) const
{
    ASSERT(!state.pendingBytes);
    markAsPassed(state, condition);
}

void TestCoverageProblem::end(State& state, const BitVector& conditions) const
{
    state.passed.exclude(conditions);
    if (!m_largestBudget)
        return;
    for (size_t condition : conditions)
        state.bytesSinceUse[condition] = 0;
}

void TestCoverageProblem::endAll(State& state) const
{
    state.passed.clearAll();
    state.bytesSinceUse.fill(0);
    state.pendingBytes = 0;
}

TestCoverage::TestCoverage(const TestCoverageInputs& inputs)
    : m_events(inputs.events)
    , m_requirements(FillWith { }, inputs.events.requirements().size(), RequirementCoverage::Unknown)
    , m_hoistedTests(FillWith { }, inputs.events.hoistedTests().size(), RequirementCoverage::Unknown)
{
    FunctionAnalyses& analyses = m_events.analyses();
    TestCoverageProblem problem(inputs);
    StatesAtHeads heads = analyses.solveForward(problem);
    for (BlockID block : m_events.cfg().reversePostOrder()) {
        std::optional<TestState>& head = heads[block.index()];
        if (head && head->isReachable)
            classifyBlock(problem, block, TestState(*head), heads);
    }
}

void TestCoverage::classifyBlock(const TestCoverageProblem& problem, BlockID block, TestState&& state, const StatesAtHeads& heads)
{
    for (const TestEvent& event : m_events.eventsOf(block)) {
        if (event.kind != TestEvent::Kind::Requirement) {
            problem.applyEvent(state, block, event);
            continue;
        }

        state.pendingBytes = saturatingSum<uint32_t>(state.pendingBytes, event.bytesBefore);
        problem.settlePendingBytes(state);
        const TestRequirement& requirement = m_events.requirements()[event.payload];
        RequirementCoverage coverage = problem.classify(state, requirement);
        m_requirements[event.payload] = coverage;
        if (coverage == RequirementCoverage::NeedsTest && state.passed.get(requirement.condition.index()))
            m_exceededBudget.set(event.payload);

        TestEvent eventAfterSettling = event;
        eventAfterSettling.bytesBefore = 0;
        problem.applyEvent(state, block, eventAfterSettling);
    }
    state.pendingBytes = saturatingSum<uint32_t>(state.pendingBytes, m_events.bytesAfterLastEventOf(block));
    classifyOutgoingEdges(problem, block, state);
    classifyRejoiningEdges(problem, block, state, heads);
}

static BitVector findMissingConditions(const TestState& atHead, const TestState& onEdge)
{
    BitVector missing;
    for (size_t condition : atHead.passed) {
        if (!onEdge.passed.get(condition))
            missing.set(condition);
    }
    return missing;
}

const BitVector* TestCoverage::missingOn(Edge edge) const
{
    if (m_missingOnEdges.isEmpty())
        return nullptr;
    for (uint32_t index = m_firstMissingOnEdgeFrom[edge.from.index()]; index < m_missingOnEdges.size() && m_missingOnEdges[index].first.from == edge.from; ++index) {
        if (m_missingOnEdges[index].first == edge)
            return &m_missingOnEdges[index].second;
    }
    return nullptr;
}

void TestCoverage::classifyRejoiningEdges(const TestCoverageProblem& problem, BlockID block, const TestState& atTail, const StatesAtHeads& heads)
{
    const Function& function = m_events.function();
    uint32_t numberOfTargets = function.at(block).terminator.targets.size();
    for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
        Edge edge { block, targetIndex };
        if (!problem.leavesOut(edge))
            continue;
        const std::optional<TestState>& atHead = heads[targetOf(function, edge).block.index()];
        if (!atHead || !atHead->isReachable)
            continue;

        TestState onEdge = atTail;
        m_events.forEachProofOn(edge, [&](uint32_t proof) {
            markAsPassed(onEdge, m_events.proofs()[proof].condition);
        });
        BitVector missing = findMissingConditions(*atHead, onEdge);
        if (missing.isEmpty())
            continue;
        if (m_firstMissingOnEdgeFrom.isEmpty())
            m_firstMissingOnEdgeFrom.fill(UINT32_MAX, function.blocks.size());
        if (m_firstMissingOnEdgeFrom[block.index()] == UINT32_MAX)
            m_firstMissingOnEdgeFrom[block.index()] = m_missingOnEdges.size();
        m_missingOnEdges.append({ edge, WTF::move(missing) });
    }
}

void TestCoverage::classifyOutgoingEdges(const TestCoverageProblem& problem, BlockID block, const TestState& atTail)
{
    const Function& function = m_events.function();
    uint32_t numberOfTargets = function.at(block).terminator.targets.size();
    for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
        Edge edge { block, targetIndex };
        Vector<uint32_t, 4> hoistedTests;
        m_events.forEachHoistedTestOn(edge, [&](uint32_t hoistedTest) {
            hoistedTests.append(hoistedTest);
        });
        if (hoistedTests.isEmpty())
            continue;

        TestState state = atTail;
        problem.settlePendingBytes(state);
        m_events.forEachProofOn(edge, [&](uint32_t proof) {
            markAsPassed(state, m_events.proofs()[proof].condition);
        });

        BlockID target = targetOf(function, edge).block;
        for (uint32_t index : hoistedTests) {
            const HoistedTest& hoistedTest = m_events.hoistedTests()[index];
            RequirementCoverage coverage = problem.classify(state, hoistedTest.condition, target);
            m_hoistedTests[index] = coverage;
            if (coverage == RequirementCoverage::NeedsTest && state.passed.get(hoistedTest.condition.index()))
                m_exceededBudget.set(hoistedTest.requirement);
            markAsPassed(state, hoistedTest.condition);
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
