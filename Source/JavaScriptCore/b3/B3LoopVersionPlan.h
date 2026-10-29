/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3CountedLoops.h"
#include "B3LoopInvariance.h"
#include "B3LoopRangeChecks.h"
#include "B3LoopRegions.h"
#include "B3PhiChildren.h"
#include "B3UseCounts.h"
#include <array>

namespace JSC { namespace B3 {

class PatchpointValue;

struct UnswitchedBranch {
    Value* terminal { nullptr };
    BasicBlock* otherSuccessor { nullptr };
};

struct LoopVersionPlan {
    HotRegion region;
    HoistPlan hoisted;
    Excursion excursion;
    RangeCheckPlan rangeChecks;
    std::optional<CountedLoop> countedLoop;
    GaveUpReason boundNotClampedReason { GaveUpReason::None };
    std::optional<UnswitchedBranch> unswitched;
    bool isEnteredFromPreHeader { true };
    BasicBlock* firstIterationSlowPath { nullptr };
    bool mayExitEveryIteration { false };
    unsigned numberOfClonedValues { 0 };
    unsigned numberOfRemovedValues { 0 };

    bool rewritesLoopTest() const { return rangeChecks.clampsBound(); }
};

class LoopVersionPlanner {
    WTF_MAKE_NONCOPYABLE(LoopVersionPlanner);
public:
    JS_EXPORT_PRIVATE explicit LoopVersionPlanner(Procedure&);

    JS_EXPORT_PRIVATE Vector<LoopVersionPlan, 2> plan(const NaturalLoop&);

private:
    struct UnswitchCandidate {
        BasicBlock* block { nullptr };
        std::array<BasicBlock*, 2> successors { };
    };
    struct Attempt;

    std::expected<Vector<LoopVersionPlan, 2>, GaveUp> findPlans(const NaturalLoop&);
    Vector<LoopVersionPlan, 2> tryUnswitch(const NaturalLoop&, const UnswitchCandidate&);
    std::optional<GaveUp> finishPlan(LoopVersionPlan&);

    std::expected<LoopVersionPlan, GaveUp> tryPlan(const HotRegion&, PhiResolution, std::optional<UnswitchedEdge>, UnswitchCandidate*);
    std::optional<GaveUp> walkLikelyPath(Attempt&);
    static void findUnswitchCandidate(NaturalLoops&, const NaturalLoop&, BasicBlock*, UnswitchCandidate&);
    bool shouldKeepGuardInLoop(Attempt&, BasicBlock*, BasicBlock* likelySuccessor);
    void visitLoopGuard(Attempt&, BasicBlock*, BasicBlock* likelySuccessor);
    bool mayVaryAcrossIterations(const LoopInvariance&, Value* terminal) const;
    BasicBlock* findFirstIterationSlowPath(const NaturalLoop&, BasicBlock*, BasicBlock* likelySuccessor);
    std::optional<int64_t> valueOnFirstIteration(const NaturalLoop&, Value*, uint64_t timeToLive);
    bool isNeverNegative(Value*);
    static bool hoistsEnough(const Attempt&);
    std::optional<GaveUp> checkHoistThresholds(const Attempt&) const;
    void findCountedLoop(Attempt&);
    bool canRewriteLoopTest(const Attempt&) const;
    unsigned countRemovedComparisons(const Attempt&) const;
    std::optional<GaveUp> checkSpeculativeHoistBudget(const Attempt&) const;

    Procedure& m_proc;
    NaturalLoops& m_loops;
    PhiChildren m_phiChildren;
    UseCounts m_useCounts;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
