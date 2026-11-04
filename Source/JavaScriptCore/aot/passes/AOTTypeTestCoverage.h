/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTGrouping.h"
#include "AOTTypeTestSite.h"
#include "AOTVector.h"
#include <optional>
#include <ranges>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// TakesProof: the site is rewritten where all that it asks for is proven already. Where something is missing no test is placed, and the site stays what it is.
enum class RequirementKind : uint8_t { PlacesTest, AsksOnly, TakesProof };

struct TestRequirement {
    uint32_t site { 0 };
    uint32_t test { 0 };
    TestConditionID condition;
    ProgramPoint point;
    RequirementKind kind { RequirementKind::PlacesTest };
};

struct TestPrerequisite {
    TestConditionID condition;
    std::optional<uint32_t> narrowedOperand;
};

// A test names earlier proofs of its site: for each operand the nearest permanent proof about it, and as its guard every stretch proof about its subject.
void forEachTestPrerequisite(std::span<const TestRequirement> requirements, const TestConditions& conditions, uint32_t requirement, NOESCAPE const Invocable<void(const TestPrerequisite&)> auto& functor)
{
    const TestCondition& tested = conditions.at(requirements[requirement].condition);

    uint32_t firstOfSite = requirement;
    while (firstOfSite && requirements[firstOfSite - 1].site == requirements[requirement].site)
        --firstOfSite;
    std::span<const TestRequirement> earlierRequirements = requirements.subspan(firstOfSite, requirement - firstOfSite);

    std::optional<unsigned> guardIndex = OperandPosition::guard(tested.test.opcode);
    for (uint32_t operandIndex = 0; operandIndex < tested.test.numberOfOperands(); ++operandIndex) {
        if (guardIndex == operandIndex)
            continue;
        for (const TestRequirement& earlier : earlierRequirements | std::views::reverse) {
            const TestCondition& condition = conditions.at(earlier.condition);
            if (condition.lastsForever() && condition.subject == tested.test.operand(operandIndex)) {
                functor(TestPrerequisite { earlier.condition, operandIndex });
                break;
            }
        }
    }

    if (!guardIndex || !tested.subject)
        return;
    for (const TestRequirement& earlier : earlierRequirements) {
        const TestCondition& condition = conditions.at(earlier.condition);
        if (condition.lastsForStretch() && condition.subject == tested.subject)
            functor(TestPrerequisite { earlier.condition, std::nullopt });
    }
}

enum class ProofSource : uint8_t { TakenEdge, SwitchCase, GuardValue, Allocation };

constexpr bool isOnEdge(ProofSource source)
{
    switch (source) {
    case ProofSource::TakenEdge:
    case ProofSource::SwitchCase:
        return true;
    case ProofSource::GuardValue:
    case ProofSource::Allocation:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct ExistingProof {
    ProofSource source { ProofSource::GuardValue };
    TestConditionID condition;
    ValueID value;
    Edge edge;
};

struct HoistedTest {
    Edge edge;
    uint32_t requirement { 0 };
    TestConditionID condition;
    Vector<ValueID, 2> operands;
};

enum class ProofEnding : uint8_t { MayEnd, DoesNotEnd };

ProofEnding endingBetweenTestAndBranch(FunctionAnalyses&, const TestCondition&, ValueID writer, const EffectsOnEdges& effectsOfWriter);

// What the proofs of the stretch conditions depend on. Conditions of one group differ in their subjects alone, so a writer is looked at once for all of them.
struct ConditionGroups {
    ConditionGroups(FunctionAnalyses&, const TestConditions&);

    // Those of the listed stretch conditions whose proofs may end there.
    BitVector findConditionsEndedBy(ValueID writer, const EffectsOnEdges&, std::span<const uint32_t> stretchConditions);
    BitVector findConditionsEndedByTerminator(const Effects&, std::span<const uint32_t> stretchConditions) const;

    struct WrittenByWriter {
        uint32_t writer { 0 };
        GuardWriter::WrittenDependencies written;
        bool hasAskedForAllocations { false };
        std::optional<BitVector> allocationsWhoseProofsEnd;
    };

    FunctionAnalyses& analyses;
    const TestConditions& conditions;
    Vector<std::optional<GuardDependencies>> dependencies;
    Vector<uint32_t> groupOf;
    BitVector subjectsFromOutside;
    Vector<WrittenByWriter> writtenOfGroups;
    uint32_t numberOfWriters { 0 };

private:
    void listConditionsOfObjects();
    bool findConditionsWithinReachOf(GuardWriter&);

    // Made when the first writer asks. A condition on allocations of the function stands under each of them, and the last writer that reaches one of them stands with the condition.
    bool hasConditionsOfObjects { false };
    BitVector isAboutAllocationsAlone;
    HashMap<uint32_t, Vector<uint32_t, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> conditionsOfObjects;
    Vector<uint32_t> lastWriterWithinReach;
    // What the subject itself may be, for those conditions.
    Vector<Vector<ObjectID, 1>> allocationsOfSubjects;
};

// A value of any other operation suspends nothing and writes nothing.
const OpcodeSet& opcodesThatMayEndProofs();

bool readsAsDefinition(const Program&, const Function&, const Value&);

struct TestEventInputs {
    FunctionAnalyses& analyses;
    const TestConditions& conditions;
    std::span<const TestSite> sites;
    std::span<const TestRequirement> requirements;
    std::span<const ExistingProof> proofs;
    std::span<const HoistedTest> hoistedTests;
    bool measuresCodeBetweenEvents { true };
    // No proof is carried past the front of these values.
    const BitVector* valuesThatStartNewStretch { nullptr };
};

struct TestEvent {
    // EndOfRequirements: behind the requirements of a site that takes proofs.
    enum class Kind : uint8_t { ParameterProof, Barrier, Requirement, EndOfRequirements, Ending, Suspension, ValueProof };

    Kind kind { Kind::Barrier };
    uint32_t index { 0 };
    uint32_t payload { 0 };
    uint32_t bytesBefore { 0 };
};

class TestEvents {
    WTF_MAKE_NONCOPYABLE(TestEvents);
public:
    explicit TestEvents(const TestEventInputs&);

    FunctionAnalyses& analyses() const { return m_inputs.analyses; }
    const Function& function() const { return m_function; }
    const CFG& cfg() const { return m_cfg; }
    const TestConditions& conditions() const { return m_inputs.conditions; }
    std::span<const TestRequirement> requirements() const { return m_inputs.requirements; }
    std::span<const ExistingProof> proofs() const { return m_inputs.proofs; }
    std::span<const HoistedTest> hoistedTests() const { return m_inputs.hoistedTests; }

    std::span<const TestEvent> eventsOf(BlockID) const LIFETIME_BOUND;
    // Whether an event of its block has the index of the value. Most values have none.
    bool hasEventAt(ValueID value) const { return m_hasEvent.get(value.index()); }
    bool startsNewStretch(ValueID value) const { return m_inputs.valuesThatStartNewStretch && m_inputs.valuesThatStartNewStretch->get(value.index()); }
    uint32_t bytesAfterLastEventOf(BlockID block) const { return m_bytesAfterLastEvent[block.index()]; }

    const BitVector& endedBy(const TestEvent& event) const LIFETIME_BOUND
    {
        ASSERT(event.kind == TestEvent::Kind::Ending);
        return m_endedSets[event.payload];
    }

    void forEachProofOn(Edge edge, NOESCAPE const Invocable<void(uint32_t proof)> auto& functor) const { forEachItemOn(edge, m_edgeProofs, m_firstEdgeProof, functor); }
    void forEachHoistedTestOn(Edge edge, NOESCAPE const Invocable<void(uint32_t hoistedTest)> auto& functor) const { forEachItemOn(edge, m_edgeTests, m_firstEdgeTest, functor); }

    uint32_t endOfResultReadsAfter(ProgramPoint) const;

    // A slow path comes back on such an edge: without the edges of this kind its target is still reached.
    Expected<bool> isRejoiningEdge(Edge) const;

private:
    struct Collector;
    struct EdgeItem {
        Edge edge;
        uint32_t index { 0 };
    };

    static void forEachItemOn(Edge edge, const Vector<EdgeItem>& items, const Vector<uint32_t>& firstItem, const auto& functor)
    {
        for (const EdgeItem& item : entriesOfGroup(firstItem, items, edge.from.index())) {
            if (item.edge == edge)
                functor(item.index);
        }
    }

    TestEventInputs m_inputs;
    const Function& m_function;
    const CFG& m_cfg;
    Vector<TestEvent> m_events;
    Vector<uint32_t> m_firstEvent;
    BitVector m_hasEvent;
    Vector<uint32_t> m_bytesAfterLastEvent;
    Vector<BitVector, 4> m_endedSets;
    Vector<EdgeItem> m_edgeProofs;
    Vector<uint32_t> m_firstEdgeProof;
    Vector<EdgeItem> m_edgeTests;
    Vector<uint32_t> m_firstEdgeTest;
    BitVector m_isEnteredOnlyFromRarerCode;
    Vector<uint32_t> m_lastResultRead;
};

struct GapBudget {
    std::optional<uint32_t> outsideCycles;
    std::optional<uint32_t> insideCycles;

    static constexpr GapBudget unlimited() { return { }; }
};

enum class RequirementCoverage : uint8_t { Unknown, NeedsTest, Covered };

// Whoever lets slow paths leave has to send an edge somewhere else where something of TestCoverage::missingOn() is used behind it.
enum class SlowPaths : uint8_t { ComeBack, MayLeave };

struct TestCoverageInputs {
    const TestEvents& events;
    GapBudget budget;
    const BitVector* blocksWithIgnoredFailingEdge { nullptr };
    SlowPaths slowPaths { SlowPaths::ComeBack };
};

class TestCoverage {
    WTF_MAKE_NONCOPYABLE(TestCoverage);
public:
    explicit TestCoverage(const TestCoverageInputs&);

    RequirementCoverage of(uint32_t requirement) const { return m_requirements[requirement]; }
    RequirementCoverage ofHoistedTest(uint32_t hoistedTest) const { return m_hoistedTests[hoistedTest]; }
    bool exceededBudget(uint32_t requirement) const { return m_exceededBudget.get(requirement); }

    // What holds at the head of the target and not on this rejoining edge, which the state at that head leaves out.
    const BitVector* missingOn(Edge) const LIFETIME_BOUND;

private:
    Vector<RequirementCoverage> m_requirements;
    Vector<RequirementCoverage> m_hoistedTests;
    BitVector m_exceededBudget;
    // In the order of their sources, which are classified one after another.
    Vector<std::pair<Edge, BitVector>, 4> m_missingOnEdges;
    Vector<uint32_t> m_firstMissingOnEdgeFrom;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
