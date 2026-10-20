/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLosses.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

struct FrameDecision {
    ValueID allocation;
    AllocationStorage storage { AllocationStorage::Heap };
    AllocationHeapCause cause { AllocationHeapCause::None };
    std::optional<Use> forcedBy;
    EscapeReasons reasons;
    LimitID limit { LimitID::None };
};

struct FramePlan {
    Vector<FrameDecision> decisions;
    FrameAllocation allocation;
    unsigned closuresInFrames { 0 };
    unsigned environmentsInFrames { 0 };
    unsigned environmentsOnHeap { 0 };
    unsigned environmentsMaterializedLate { 0 };
    unsigned objectsInFrames { 0 };

    bool changesFunction() const;
};

bool hasStorageChoice(const Value&);

class FramePlanner {
    WTF_MAKE_NONCOPYABLE(FramePlanner);
public:
    explicit FramePlanner(FunctionAnalyses&);

    FramePlan run();

private:
    const Value& definitionOf(ObjectID object) const LIFETIME_BOUND { return m_function.at(m_escape.at(object).definition); }
    bool isCandidate(ObjectID object) const { return m_isCandidate.get(object.index()); }
    bool isInFrame(ObjectID object) const { return m_decisions[object.index()].storage == AllocationStorage::Frame; }
    bool mayBeInFrame(ObjectID object) const { return m_decisions[object.index()].storage != AllocationStorage::Heap; }
    void moveToHeap(ObjectID, AllocationHeapCause, std::optional<Use> = std::nullopt, LimitID = LimitID::None);

    void collectCandidates();

    void applyEscapeAnalysis();
    void decideByEscapeState(ObjectID);
    bool hasUntrackedAlias(ObjectID) const;
    bool escapesOnlyIntoClosures(ObjectID) const;

    void checkCellForms();
    AllocationHeapCause checkClosureForm(const Value& closure) const;
    AllocationHeapCause checkObjectForm(const Value& object) const;
    bool fitsInFrame(const Value& allocation);

    void checkUses();
    AllocationHeapCause checkUse(ObjectID, const ObjectUse&) const;
    bool mayLeakContentsThrough(const ObjectUse&) const;

    void checkSuspensions();
    void checkReturnCalls();

    void checkHolders();
    std::optional<ObjectID> findHolder(const Use&) const;
    bool dieTogether(ObjectID holder, ObjectID held) const;
    void addHolder(ObjectID holder, const Use&);

    void checkMaterialization();
    BitVector findFrameClosures() const;
    bool canMaterializeLate(ObjectID environment, const BitVector& frameClosures);

    void finishPlan();

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    const FlowResults* m_flow;
    const EscapeAnalysis& m_escape;
    BitVector m_isCandidate;
    Vector<FrameDecision> m_decisions;
    FramePlan m_plan;
};

void applyFramePlan(const AnalysisContext&, Function&, TypeArena&, const FramePlan&);
AllocationRemark remarkOf(const Function&, const FrameDecision&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
