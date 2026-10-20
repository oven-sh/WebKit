/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypeTestConditions.h"
#include "AOTTypeTestSite.h"
#include <optional>
#include <ranges>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class RequirementKind : uint8_t { PlacesTest, AsksOnly };

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

ProofEnding endingOf(FunctionAnalyses&, const TestCondition&, ValueID writer, const EffectsOnEdges& effectsOfWriter);

// endingOf() for one writer and many conditions.
class EndingsAtWriter {
public:
    EndingsAtWriter(FunctionAnalyses&, ValueID writer, const EffectsOnEdges& effectsOfWriter);

    ProofEnding endingOf(const TestCondition&, const std::optional<GuardDependencies>& dependenciesOfTest);

private:
    FunctionAnalyses& m_analyses;
    ValueID m_writer;
    const Effects& m_effects;
    bool m_isBoundedByWriter;
    std::optional<GuardWriter> m_guardWriter;
};

ProofEnding endingOfTerminator(FunctionAnalyses&, const TestCondition&, const Effects& effectsOfTerminator);

enum class RenewalPolicy : uint8_t { Never, ByShapeEpoch };

struct TestEventInputs {
    FunctionAnalyses& analyses;
    const TestConditions& conditions;
    std::span<const TestSite> sites;
    std::span<const TestRequirement> requirements;
    std::span<const ExistingProof> proofs;
    std::span<const HoistedTest> hoistedTests;
    RenewalPolicy renewal { RenewalPolicy::Never };
};

struct TestEvent {
    enum class Kind : uint8_t { ParameterProof, Barrier, Requirement, Ending, Suspension, ValueProof };

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
    std::span<const TestSite> sites() const { return m_inputs.sites; }
    std::span<const TestRequirement> requirements() const { return m_inputs.requirements; }
    std::span<const ExistingProof> proofs() const { return m_inputs.proofs; }
    std::span<const HoistedTest> hoistedTests() const { return m_inputs.hoistedTests; }
    RenewalPolicy renewal() const { return m_inputs.renewal; }

    std::span<const TestEvent> eventsOf(BlockID) const LIFETIME_BOUND;
    uint32_t bytesAfterLastEventOf(BlockID block) const { return m_bytesAfterLastEvent[block.index()]; }

    const BitVector& endedBy(const TestEvent& event) const LIFETIME_BOUND
    {
        ASSERT(event.kind == TestEvent::Kind::Ending);
        return m_endedSets[event.payload].ended;
    }

    const BitVector& markedStaleBy(const TestEvent& event) const LIFETIME_BOUND
    {
        ASSERT(event.kind == TestEvent::Kind::Ending);
        return m_endedSets[event.payload].markedStale;
    }

    void forEachProofOn(Edge edge, NOESCAPE const Invocable<void(uint32_t proof)> auto& functor) const
    {
        for (const EdgeItem& item : itemsFrom(edge.from, m_edgeProofs, m_firstEdgeProof)) {
            if (item.edge == edge)
                functor(item.index);
        }
    }

    void forEachHoistedTestOn(Edge edge, NOESCAPE const Invocable<void(uint32_t hoistedTest)> auto& functor) const
    {
        for (const EdgeItem& item : itemsFrom(edge.from, m_edgeTests, m_firstEdgeTest)) {
            if (item.edge == edge)
                functor(item.index);
        }
    }

    uint32_t endOfResultReadsAfter(ProgramPoint) const;

    // A slow path comes back on such an edge: its target is also entered from a block that runs as often as the target.
    bool isRejoiningEdge(Edge) const;

private:
    struct EdgeItem {
        Edge edge;
        uint32_t index { 0 };
    };

    struct EndedSet {
        BitVector ended;
        BitVector markedStale;

        friend bool operator==(const EndedSet&, const EndedSet&) = default;
    };

    struct PointItem {
        ProgramPoint point;
        uint32_t index { 0 };
    };

    void collectEdgeItems();
    void collectPointItems();
    void findResultReads();
    void collectEvents();
    void collectEventsOf(BlockID);
    void appendEvent(TestEvent::Kind, uint32_t index, uint32_t payload);
    void appendEnding(uint32_t index, EndedSet&&);
    EndedSet computeEndedSet(ValueID writer, const EffectsOnEdges&) const;
    EndedSet computeEndedSetOfTerminator(const Effects&) const;
    void splitByRenewal(EndedSet&) const;
    uint32_t internEndedSet(EndedSet&&);

    static std::span<const EdgeItem> itemsFrom(BlockID, const Vector<EdgeItem>&, const Vector<uint32_t>& firstItem);

    TestEventInputs m_inputs;
    const Function& m_function;
    const CFG& m_cfg;
    Vector<TestEvent> m_events;
    Vector<uint32_t> m_firstEvent;
    Vector<uint32_t> m_bytesAfterLastEvent;
    uint32_t m_pendingBytes { 0 };
    Vector<EndedSet, 4> m_endedSets;
    Vector<std::optional<GuardDependencies>> m_guardDependencies;
    Vector<EdgeItem> m_edgeProofs;
    Vector<uint32_t> m_firstEdgeProof;
    Vector<EdgeItem> m_edgeTests;
    Vector<uint32_t> m_firstEdgeTest;
    Vector<PointItem> m_requirementsByPoint;
    Vector<PointItem> m_valueProofsByPoint;
    Vector<PointItem> m_parameterProofsByBlock;
    BitVector m_isBarrier;
    Vector<uint32_t> m_lastResultRead;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
