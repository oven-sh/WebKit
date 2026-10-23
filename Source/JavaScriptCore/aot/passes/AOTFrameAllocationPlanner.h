/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLosses.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Typed {

class Heat;

struct FrameDecision {
    ValueID allocation;
    AllocationStorage storage { AllocationStorage::Heap };
    AllocationHeapCause cause { AllocationHeapCause::None };
    // AOTFrameAllocationPointers.h, isLentTo().
    bool isLent { false };
    // Uses that let a closure out, each of them the last thing that happens to it. The closure of the frame serves the others, and a closure of the heap is made for each of these where it stands.
    Vector<Use, 1> madeAgainAt;
    // A closure of the heap that need not be made: the closure that the closure table holds stands for it.
    bool isHeldByClosureTable { false };
    std::optional<Use> forcedBy;
    EscapeReasons reasons;
    LimitID limit { LimitID::None };
};

struct FramePlan {
    Vector<FrameDecision> decisions;
    Vector<ValueID, 2> emptyArgumentLists;
    // The frame has to outlive the call, which takes a cell that lies in it.
    Vector<BlockID, 1> demotedReturnCalls;
    // They lie in the frame and hold no pointer into it: an environment is materialized in front of them.
    Vector<ValueID, 1> frameClosuresOverHeapCells;
    FrameAllocation allocation;
    unsigned closuresInFrames { 0 };
    unsigned closuresLent { 0 };
    unsigned environmentsInFrames { 0 };
    unsigned environmentsOnHeap { 0 };
    unsigned environmentsMaterializedLate { 0 };
    unsigned objectsInFrames { 0 };
    unsigned arraysInFrames { 0 };
    unsigned restArraysInFrames { 0 };

    bool changesFunction() const;
};

bool hasStorageChoice(const Value&);
bool isEmptyArrayLiteral(const Value&);

// Locked stack slots are never shared, so the arrays of a function take up to 32 * (24 + 8 * MaxFrameArrayLength) bytes of its frame.
inline constexpr unsigned maxFollowedArrays = 32;
// What the planner finds out about the uses of arrays, without the analyses that it needs for everything else: most functions that make an array store it or return it.
bool mayPutSomeArrayInFrame(const Program&, const FlowResults&, const BuildContext&, const Function&);

class FramePlanner {
    WTF_MAKE_NONCOPYABLE(FramePlanner);
public:
    FramePlanner(FunctionAnalyses&, const Heat&);

    FramePlan run();

    // Of a closure or an object: run() leaves it on the heap, whatever the analyses say of its uses, and records no more than that and whether the closure table holds it.
    static bool staysOnHeapByForm(const Program&, const FlowResults&, const BuildContext&, const Value& allocation);
    static bool isLikeEveryClosureOfItsFunction(const Program&, const FlowResults*, const Function& maker, const Value& closure);

private:
    const Value& definitionOf(ObjectID object) const LIFETIME_BOUND { return m_function.at(m_escape.at(object).definition); }
    bool isCandidate(ObjectID object) const { return m_isCandidate.get(object.index()); }
    bool isArray(ObjectID object) const { return arrayAllocationsWithStorage.contains(definitionOf(object).opcode); }
    bool isInFrame(ObjectID object) const { return m_decisions[object.index()].storage == AllocationStorage::Frame; }
    bool mayBeInFrame(ObjectID object) const { return m_decisions[object.index()].storage != AllocationStorage::Heap; }
    void moveToHeap(ObjectID, AllocationHeapCause, std::optional<Use> = std::nullopt, LimitID = LimitID::None);

    void shareEmptyArgumentLists();
    bool isOnlyReadAsHiddenRest(ObjectID list) const;
    bool isHiddenRestOfEveryCallee(const Use&) const;

    void collectCandidates();

    void applyEscapeAnalysis();
    void decideByEscapeState(ObjectID);
    void decideArrayByLiveInstances(ObjectID);
    bool hasUntrackedAlias(ObjectID) const;
    bool escapesOnlyIntoClosures(ObjectID) const;
    bool keepClosureUntilItLeaves(ObjectID closure);
    bool willHaveLongFormsInLine(BlockID);
    void takeClosuresOfTable();
    bool isOnlyCalled(ObjectID closure) const;
    bool isLikeEveryClosureOfItsFunction(const Value& closure) const { return isLikeEveryClosureOfItsFunction(m_program, m_flow, m_function, closure); }
    bool mayFindBorrowingCallee(const Value& call, uint32_t operandIndex) const;
    bool isLent(ObjectID object) const { return m_decisions[object.index()].isLent; }
    bool isKeptUntilItLeaves(ObjectID object) const { return isLent(object) || !m_decisions[object.index()].madeAgainAt.isEmpty(); }

    void checkCellForms();
    static AllocationHeapCause checkClosureForm(const Program&, const Value& closure);
    static AllocationHeapCause checkObjectForm(const Program&, const Value& object);
    static bool fitsInFrame(const Program&, const BuildContext&, const Value& allocation);

    void checkUses();
    AllocationHeapCause checkUse(ObjectID, const ObjectUse&) const;
    bool mayLeakContentsThrough(const ObjectUse&) const;
    bool leaksContents(ObjectID cell) const;

    void checkUsesOfArrays();
    bool calleesEndTheirTailCalls(const Value& call) const;
    bool passesCellOfFrame(const Value& call) const;

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
    const Heat& m_heat;
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
