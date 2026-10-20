/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTForwardDataflow.h"
#include "AOTTypeTestEvents.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct GapBudget {
    std::optional<uint32_t> outsideCycles;
    std::optional<uint32_t> insideCycles;

    static constexpr GapBudget unlimited() { return { }; }
};

enum class RequirementCoverage : uint8_t { Unknown, NeedsTest, NeedsRenewal, Covered };

struct TestState {
    bool isReachable { true };
    BitVector passed;
    BitVector stale;
    Vector<uint32_t> bytesSinceUse;
    uint32_t pendingBytes { 0 };

    friend bool operator==(const TestState&, const TestState&) = default;
};

// Whoever lets slow paths leave has to send an edge somewhere else where something of TestCoverage::missingOn() is used behind it.
enum class SlowPaths : uint8_t { ComeBack, MayLeave };

struct TestCoverageInputs {
    const TestEvents& events;
    GapBudget budget;
    const BitVector* blocksWithIgnoredFailingEdge { nullptr };
    SlowPaths slowPaths { SlowPaths::ComeBack };
};

class TestCoverageProblem {
    WTF_MAKE_NONCOPYABLE(TestCoverageProblem);
public:
    using State = TestState;

    explicit TestCoverageProblem(const TestCoverageInputs&);

    State stateAtEntry() const;
    bool merge(State& into, const State& from) const;
    void enterEdge(State&, Edge) const;
    void execute(State&, const DataflowStep&) const;
    void executeTerminator(State&, const DataflowTerminatorStep&) const;

    RequirementCoverage classify(const State&, const TestRequirement&) const;
    RequirementCoverage classify(const State&, TestConditionID, BlockID) const;
    void applyEvent(State&, BlockID, const TestEvent&) const;
    void settlePendingBytes(State&) const;
    bool leavesOut(Edge) const;

private:
    std::optional<uint32_t> budgetIn(BlockID) const;
    std::span<const TestEvent> eventsAt(BlockID, uint32_t index) const;
    void applyRequirement(State&, const TestRequirement&) const;
    void establish(State&, TestConditionID) const;
    void end(State&, const BitVector& conditions) const;
    void endAll(State&) const;

    TestCoverageInputs m_inputs;
    const BlockCycles& m_cycles;
    std::optional<uint32_t> m_largestBudget;
};

class TestCoverage {
    WTF_MAKE_NONCOPYABLE(TestCoverage);
public:
    explicit TestCoverage(const TestCoverageInputs&);

    RequirementCoverage of(uint32_t requirement) const { return m_requirements[requirement]; }
    RequirementCoverage ofHoistedTest(uint32_t hoistedTest) const { return m_hoistedTests[hoistedTest]; }
    const BitVector& staleBefore(uint32_t requirement) const LIFETIME_BOUND { return m_staleBefore[requirement]; }
    bool exceededBudget(uint32_t requirement) const { return m_exceededBudget.get(requirement); }

    // What holds at the head of the target and not on this rejoining edge, which the state at that head leaves out.
    const BitVector* missingOn(Edge) const LIFETIME_BOUND;

private:
    using StatesAtHeads = Vector<std::optional<TestState>>;

    void classifyBlock(const TestCoverageProblem&, BlockID, TestState&&, const StatesAtHeads&);
    void classifyOutgoingEdges(const TestCoverageProblem&, BlockID, const TestState& atTail);
    void classifyRejoiningEdges(const TestCoverageProblem&, BlockID, const TestState& atTail, const StatesAtHeads&);

    const TestEvents& m_events;
    Vector<RequirementCoverage> m_requirements;
    Vector<RequirementCoverage> m_hoistedTests;
    Vector<BitVector> m_staleBefore;
    BitVector m_exceededBudget;
    Vector<std::pair<Edge, BitVector>, 4> m_missingOnEdges;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
