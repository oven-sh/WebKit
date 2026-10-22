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

ProofEnding endingBetweenTestAndBranch(FunctionAnalyses&, const TestCondition&, ValueID writer, const EffectsOnEdges& effectsOfWriter);

class EndingsAtWriter {
public:
    EndingsAtWriter(FunctionAnalyses&, ValueID writer, const EffectsOnEdges& effectsOfWriter);

    ProofEnding endingOf(const TestCondition&, const std::optional<GuardDependencies>& dependenciesOfTest);

    // endingOf() in two steps, for many conditions that differ in their subjects alone.
    GuardWriter::WrittenDependencies writtenOf(const TestCondition&, const GuardDependencies&);
    ProofEnding endingOf(ValueID subject, bool subjectComesFromOutside, const GuardWriter::WrittenDependencies&);

private:
    FunctionAnalyses& m_analyses;
    ValueID m_writer;
    const Effects& m_effects;
    bool m_isBoundedByWriter;
    std::optional<GuardWriter> m_guardWriter;
};

ProofEnding endingOfTerminator(FunctionAnalyses&, const TestCondition&, const Effects& effectsOfTerminator);

// A value of any other operation suspends nothing and writes nothing.
const OpcodeSet& opcodesThatMayEndProofs();

struct TestEventInputs {
    FunctionAnalyses& analyses;
    const TestConditions& conditions;
    std::span<const TestSite> sites;
    std::span<const TestRequirement> requirements;
    std::span<const ExistingProof> proofs;
    std::span<const HoistedTest> hoistedTests;
    bool measuresCodeBetweenEvents { true };
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

    std::span<const TestEvent> eventsOf(BlockID) const LIFETIME_BOUND;
    // Whether an event of its block has the index of the value. Most values have none.
    bool hasEventAt(ValueID value) const { return m_hasEvent.get(value.index()); }
    uint32_t bytesAfterLastEventOf(BlockID block) const { return m_bytesAfterLastEvent[block.index()]; }

    const BitVector& endedBy(const TestEvent& event) const LIFETIME_BOUND
    {
        ASSERT(event.kind == TestEvent::Kind::Ending);
        return m_endedSets[event.payload];
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

    // A slow path comes back on such an edge: without the edges of this kind its target is still reached.
    bool isRejoiningEdge(Edge) const;

private:
    struct EdgeItem {
        Edge edge;
        uint32_t index { 0 };
    };

    struct PointItem {
        ProgramPoint point;
        uint32_t index { 0 };
    };

    void findBlocksEnteredOnlyFromRarerCode();
    void collectEdgeItems();
    void collectPointItems();
    void findResultReads();
    void collectEvents();
    void findWhereConditionsMatter();
    void groupConditionsByDependencies();
    void collectEventsOf(BlockID);
    void appendEvent(TestEvent::Kind, uint32_t index, uint32_t payload);
    void appendEnding(uint32_t index, BitVector&& endedConditions);
    std::span<const uint32_t> conditionsThatMatterIn(BlockID);
    BitVector findConditionsEndedBy(ValueID writer, const EffectsOnEdges&, std::span<const uint32_t> conditionsThatMatter) const;
    BitVector findConditionsEndedByTerminator(const Effects&, std::span<const uint32_t> conditionsThatMatter) const;
    uint32_t internEndedSet(BitVector&& endedConditions);

    static std::span<const EdgeItem> itemsFrom(BlockID, const Vector<EdgeItem>&, const Vector<uint32_t>& firstItem);

    TestEventInputs m_inputs;
    const Function& m_function;
    const CFG& m_cfg;
    Vector<TestEvent> m_events;
    Vector<uint32_t> m_firstEvent;
    BitVector m_hasEvent;
    Vector<uint32_t> m_bytesAfterLastEvent;
    uint32_t m_pendingBytes { 0 };
    Vector<BitVector, 4> m_endedSets;
    Vector<std::optional<GuardDependencies>> m_guardDependencies;
    // Conditions of one group differ in their subjects alone.
    struct WrittenByWriter {
        ValueID writer;
        GuardWriter::WrittenDependencies written;
    };
    Vector<uint32_t> m_dependencyGroups;
    BitVector m_subjectsFromOutside;
    mutable Vector<WrittenByWriter> m_writtenOfDependencyGroups;
    struct BlocksWhereConditionMatters {
        uint32_t condition { 0 };
        uint32_t first { 0 };
        uint32_t last { 0 };
    };
    Vector<BlocksWhereConditionMatters> m_whereConditionsMatter;
    // By position in the reverse postorder: the first position that holds a block that the block may reach.
    Vector<uint32_t> m_firstBlockReachedFrom;
    Vector<uint32_t, 16> m_conditionsThatMatter;
    BlockID m_blockOfConditionsThatMatter;
    Vector<EdgeItem> m_edgeProofs;
    Vector<uint32_t> m_firstEdgeProof;
    Vector<EdgeItem> m_edgeTests;
    Vector<uint32_t> m_firstEdgeTest;
    Vector<PointItem> m_requirementsByPoint;
    Vector<PointItem> m_valueProofsByPoint;
    Vector<PointItem> m_parameterProofsByBlock;
    BitVector m_isBarrier;
    BitVector m_isEnteredOnlyFromRarerCode;
    Vector<uint32_t> m_lastResultRead;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
