/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTExpectedTypesByUse.h"
#include "AOTGenericCopy.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class TestPlacer {
    WTF_MAKE_NONCOPYABLE(TestPlacer);
public:
    TestPlacer(FunctionPassContext&, const TestPlacementPlan&, const ExpectedTypesByUse&);

    PassOutcome run();

    unsigned numberOfTests() const { return m_emittedTests.size(); }
    // With the index of each test in the plan.
    void forEachEmittedTest(NOESCAPE const Invocable<void(uint32_t plannedTest)> auto& functor) const
    {
        for (const EmittedTest& emitted : m_emittedTests)
            functor(emitted.plannedTest);
    }
    unsigned numberOfGuardedStretches() const;
    unsigned numberOfUnclonableStretches() const { return m_unclonableStretches.bitCount(); }
    unsigned numberOfWaysBack() const { return m_copy ? m_copy->numberOfWaysBack() : 0; }

private:
    using PointKey = std::pair<BlockID, uint32_t>;

    struct Cut {
        PointKey key() const { return { point.block, point.index }; }

        ProgramPoint point;
        BlockID blockBefore;
        BlockID blockAfter;
        bool hasTest { false };
    };

    struct SplitEdge {
        Edge edge;
        BlockID block;
    };

    struct PreparedLoop {
        uint32_t plannedWayBack { 0 };
        BlockID blockOnEnteringEdge;
        Vector<BlockID, 2> blocksOnBackEdges;
        bool copyComesBack { false };
    };

    struct SubjectOperand {
        uint32_t use { 0 };
        uint32_t operandIndex { 0 };
        uint32_t requirement { 0 };
    };

    struct EmittedTest {
        uint32_t plannedTest { 0 };
        BlockID block;
        ValueID subject;
        BlockID passed;
        BlockID failed;
        ValueID provenWhenPassed;
        ValueID provenWhenFailed;
    };

    struct TestChain {
        BlockID block;
        Target passedTarget;
        Target failedTarget;
        BlockFrequency frequency { BlockFrequency::Normal };
        // Then that block takes the proof of the last test itself.
        bool isOnlyWayToPassedTarget { false };
    };

    const TestCondition& conditionOf(TestConditionID id) const LIFETIME_BOUND { return m_plan.conditions->at(id); }
    const TestRequirement& requirementAt(uint32_t index) const LIFETIME_BOUND { return m_plan.requirements[index]; }
    const PlannedWayBackIntoLoop& plannedWayBackOf(const PreparedLoop& loop) const LIFETIME_BOUND { return m_plan.waysBackIntoLoops[loop.plannedWayBack]; }
    bool isPlaced(unsigned stretch) const { return m_plan.isGuarded(stretch) && !m_unclonableStretches.get(stretch); }
    bool shouldClone(const StretchRange& range) const { return range.isCloned && isPlaced(range.stretch); }
    bool goesOnBehindStretch(const StretchRange&) const;

    void collectSubjectOperands();
    void collectCuts();
    void splitTestedEdges();
    void splitBlocks();
    void findRejoinBlocks();
    void findRejoinBlocks(std::span<const ProgramPoint>, BitVector& isRejoinBlock);
    void checkStretches();
    void cloneGenericCopies();
    void redirectSlowPaths();
    BlockID ensureSplit(Edge);
    const Cut* findCut(ProgramPoint) const;
    Cut* findCut(ProgramPoint);
    BlockID blockOf(const StretchRange&) const;
    BlockID lastPartOf(BlockID) const;
    ProgramPoint pointBehindCuts(ProgramPoint) const;
    BlockID guardedBlockOf(const PlannedTest&);

    void prepareLoops();
    void passArgumentsThrough(Edge, BlockID blockOnEdge);
    bool canComeBackInto(const PreparedLoop&) const;
    BitVector findBlocksBehindClonedRanges() const;
    Vector<BlockID, 8> collectCopiedBlocks() const;
    BitVector findStretchesThatGuessLayouts() const;
    BitVector findStretchesInFrontOfLoops() const;
    void appendSplitEdgesInside(Vector<BlockID, 8>& region) const;
    void extendCopiesBehindStretches(const RegionCloner::LivenessSource&);
    GenericCopy::Extent planGenericCopy(std::span<const BlockID> blocksOfStretches, const RegionCloner::LivenessSource&);
    void addWayBackIntoLoop(const PreparedLoop&);
    void emitTestsOnWayBack(const PlannedWayBackIntoLoop&, const Target& failedTarget);

    void adoptExistingProofs();
    ValueID adoptEdgeProof(const ExistingProof&);
    void defineAdoptedProofs();
    void emitTests();
    void leaveOutBlocksOfUnusedProofs(const BitVector& isUsed);
    void emitChain(TestChain&&, std::span<const uint32_t> plannedTests);
    enum class IsLastOfChain : bool { No, Yes };
    void emitTest(const TestChain&, uint32_t plannedTest, IsLastOfChain);
    EmittedTest& branchOn(const TestChain&, uint32_t plannedTest, ValueID test, IsLastOfChain);
    Value createTest(const PlannedTest&);
    void keepOnlyWhatOperandsProve(Value& test) const;
    std::optional<TypeID> findTypeOfOperand(const Value& test, uint32_t operandIndex, ValueID passed) const;
    void addDefinition(TestConditionID, ValueID proof);
    ValueID representativeOf(TestConditionID) const;
    ValueID wrapGuard(ValueID guard);

    void rewriteUses();
    void rewriteUse(const PlannedUse&, uint32_t useIndex);
    ValueID narrowToOperand(ValueID passed, ValueID operand, uint32_t useIndex);
    void repairProofs();
    void replaceDominatedUses();
    void retypeChangedValues();

    FunctionPassContext& m_context;
    Function& m_function;
    const TestPlacementPlan& m_plan;
    const ExpectedTypesByUse& m_expectations;
    IRBuilder m_builder;
    std::optional<GenericCopy> m_copy;
    GenericCopy::Extent m_extentOfCopy { GenericCopy::Extent::NotCopied };
    BitVector m_unclonableStretches;
    // Of a cold function, as it was before the placer changed it.
    std::optional<BitVector> m_stretchesInFrontOfLoops;
    BitVector m_stretchesThatGuessLayouts;
    Vector<Cut, 8> m_cuts;
    Vector<SplitEdge, 4> m_splitEdges;
    Vector<PreparedLoop, 2> m_loops;
    BitVector m_isOnWayBackIntoLoop;
    Vector<ProgramPoint> m_rejoinPoints;
    BitVector m_isRejoinBlock;
    Vector<ProgramPoint> m_rejoinPointsWithoutTests;
    BitVector m_isRejoinBlockWithoutTests;
    Vector<SubjectOperand> m_subjectOperands;
    Vector<Vector<ValueID, 2>> m_definitions;
    BitVector m_isDefinition;
    Vector<ValueID> m_adoptedProofs;
    Vector<EmittedTest> m_emittedTests;
    Vector<ProgramPoint> m_usePoints;
    Vector<ValueID> m_changedValues;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
