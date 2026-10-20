/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPlaceTypeTestsPlan.h"
#include "AOTRegionCloner.h"
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class TestPlacer {
    WTF_MAKE_NONCOPYABLE(TestPlacer);
public:
    TestPlacer(FunctionPassContext&, const TestPlacementPlan&);

    PassOutcome run();

    unsigned numberOfTests() const { return m_emittedTests.size() - m_numberOfRenewals; }
    unsigned numberOfRenewals() const { return m_numberOfRenewals; }
    unsigned numberOfGuardedStretches() const;
    unsigned numberOfUnclonableStretches() const { return m_unclonableStretches.bitCount(); }

private:
    using PointKey = std::pair<BlockID, uint32_t>;

    struct Cut {
        PointKey key() const { return { point.block, point.index }; }

        ProgramPoint point;
        BlockID blockBefore;
        BlockID blockAfter;
    };

    struct SplitEdge {
        Edge edge;
        BlockID block;
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
    };

    const TestCondition& conditionOf(TestConditionID id) const LIFETIME_BOUND { return m_plan.conditions->at(id); }
    const TestRequirement& requirementAt(uint32_t index) const LIFETIME_BOUND { return m_plan.requirements[index]; }
    bool isPlaced(unsigned stretch) const { return m_plan.isGuarded(stretch) && !m_unclonableStretches.get(stretch); }
    bool shouldClone(const StretchRange& range) const { return range.isCloned && isPlaced(range.stretch); }

    void collectSubjectOperands();
    void collectCuts();
    void splitTestedEdges();
    void splitBlocks();
    void checkStretches();
    void cloneGenericCopies();
    void redirectSlowPaths();
    BlockID ensureSplit(Edge);
    const Cut* findCut(ProgramPoint) const;
    BlockID blockOf(const StretchRange&) const;
    BlockID lastPartOf(BlockID) const;
    void appendSplitEdgesInside(Vector<BlockID, 8>& region) const;
    BlockID guardedBlockOf(const PlannedTest&);

    void adoptExistingProofs();
    ValueID adoptEdgeProof(const ExistingProof&);
    void defineAdoptedProofs();
    void emitTests();
    void emitChain(TestChain&&, std::span<const uint32_t> plannedTests);
    void emitTest(const TestChain&, uint32_t plannedTest);
    void emitRenewal(const TestChain&, uint32_t plannedTest);
    EmittedTest& branchOn(const TestChain&, uint32_t plannedTest, ValueID test);
    Value createTest(const PlannedTest&);
    void readShapeEpoch();
    void addDefinition(TestConditionID, ValueID proof);
    ValueID representativeOf(TestConditionID) const;
    ValueID wrapGuard(ValueID guard);

    void rewriteUses();
    void rewriteUse(const PlannedUse&, uint32_t useIndex);
    ValueID narrowToOperand(ValueID passed, ValueID operand, uint32_t useIndex);
    void repairProofs();
    void replaceDominatedUses();
    void replaceUsesDominatedBy(Edge, ValueID subject, ValueID proven);
    void retypeChangedValues();

    FunctionPassContext& m_context;
    Function& m_function;
    const TestPlacementPlan& m_plan;
    IRBuilder m_builder;
    std::optional<RegionCloner> m_cloner;
    BitVector m_unclonableStretches;
    Vector<Cut, 8> m_cuts;
    Vector<SplitEdge, 4> m_splitEdges;
    Vector<SubjectOperand> m_subjectOperands;
    Vector<Vector<ValueID, 2>> m_definitions;
    BitVector m_isDefinition;
    Vector<ValueID> m_adoptedProofs;
    Vector<ValueID, 4> m_epochReads;
    Vector<EmittedTest> m_emittedTests;
    Vector<ProgramPoint> m_usePoints;
    Vector<std::pair<ValueID, ValueID>> m_aliasesByIdentity;
    Vector<ValueID> m_changedValues;
    unsigned m_numberOfRenewals { 0 };
};

bool predictLayoutsByNames(FunctionPassContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
