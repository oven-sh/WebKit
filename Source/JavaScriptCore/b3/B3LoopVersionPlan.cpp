/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3LoopVersionPlan.h"

#if ENABLE(B3_JIT)

#include "B3Dominators.h"
#include "B3LoopUtilities.h"
#include "B3NaturalLoops.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

static unsigned valuesLeftInLoop(const LoopVersionPlan& plan, bool creditsRemovedValues)
{
    return plan.numberOfClonedValues - plan.hoisted.numberOfHoistedValues + plan.hoisted.numberOfHoistsBeyondRegisters - (creditsRemovedValues ? plan.numberOfRemovedValues : 0);
}

static bool shouldPreferResolvedPlan(const LoopVersionPlan& resolved, const LoopVersionPlan* plain)
{
    bool creditsRemovedValues = !resolved.mayExitEveryIteration;
    unsigned left = valuesLeftInLoop(resolved, creditsRemovedValues) + (creditsRemovedValues ? 0 : resolved.hoisted.numberOfSpeculativeHoists);
    unsigned leftOtherwise = plain ? valuesLeftInLoop(*plain, creditsRemovedValues) : resolved.numberOfClonedValues;
    return left < leftOtherwise;
}

static LoopVersionPlan createPlan(const HotRegion& region)
{
    LoopVersionPlan plan;
    plan.region = region;
    return plan;
}

struct LoopVersionPlanner::Attempt {
    Attempt(LoopVersionPlanner& planner, const HotRegion& region, PhiResolution resolution, std::optional<UnswitchedEdge> unswitched, UnswitchCandidate* candidate)
        : plan(createPlan(region))
        , invariance(planner.m_proc, planner.m_loops, plan.region, planner.m_phiChildren, resolution)
        , resolution(resolution)
        , unswitched(unswitched)
        , candidate(candidate)
    {
    }

    const NaturalLoop& loop() const { return *plan.region.loop; }
    bool isUnswitchedBlock(BasicBlock* block) const { return unswitched && unswitched->block == block; }

    LoopVersionPlan plan;
    LoopInvariance invariance;
    PhiResolution resolution;
    std::optional<UnswitchedEdge> unswitched;
    UnswitchCandidate* candidate;
    BasicBlock* topTest { nullptr };
    Vector<Value*, 8> variantGuards;
    IndexSet<BasicBlock*> blocksBeforeTopTest;
    std::optional<IndexSet<BasicBlock*>> rejoiningBlocks;
};

LoopVersionPlanner::LoopVersionPlanner(Procedure& proc)
    : m_proc(proc)
    , m_loops(proc.naturalLoops())
    , m_phiChildren(proc)
    , m_useCounts(proc)
{
}

Vector<LoopVersionPlan, 2> LoopVersionPlanner::plan(const NaturalLoop& loop)
{
    Origin origin = loop.header()->at(0)->origin();
    auto plans = findPlans(loop);
    if (!plans) {
        m_proc.addRemark({ RemarkKind::LoopNotVersioned, plans.error().reason, origin, plans.error().limit });
        return { };
    }
    m_proc.addRemark({ RemarkKind::LoopVersioned, GaveUpReason::None, origin, std::nullopt });
    unsigned numberOfDecidedComparisons = 0;
    for (const LoopVersionPlan& plan : *plans)
        numberOfDecidedComparisons += plan.rangeChecks.decidedComparisons.size();
    if (numberOfDecidedComparisons)
        m_proc.addRemark({ RemarkKind::RangeChecksRemoved, GaveUpReason::None, origin, std::nullopt }, numberOfDecidedComparisons);
    for (const LoopVersionPlan& plan : *plans) {
        if (plan.boundNotClampedReason != GaveUpReason::None)
            m_proc.addRemark({ RemarkKind::LoopBoundNotClamped, plan.boundNotClampedReason, origin, std::nullopt });
        if (plan.rangeChecks.clampsBound())
            m_proc.count(Counter::LoopBoundsClamped);
    }
    return WTF::move(*plans);
}

std::expected<Vector<LoopVersionPlan, 2>, GaveUp> LoopVersionPlanner::findPlans(const NaturalLoop& loop)
{
    auto region = findHotRegion(m_loops, loop, std::nullopt);
    if (!region)
        return std::unexpected(region.error());

    auto resolved = tryPlan(*region, PhiResolution::Resolve, std::nullopt, nullptr);
    UnswitchCandidate candidate;
    auto plain = tryPlan(*region, PhiResolution::Keep, std::nullopt, &candidate);

    Vector<LoopVersionPlan, 2> plans;
    if (resolved && shouldPreferResolvedPlan(*resolved, plain ? &*plain : nullptr))
        plans.append(WTF::move(*resolved));
    else
        plans = tryUnswitch(loop, candidate);
    if (plans.isEmpty()) {
        if (!plain)
            return std::unexpected(plain.error());
        plans.append(WTF::move(*plain));
    }
    for (LoopVersionPlan& plan : plans) {
        if (auto gaveUp = finishPlan(plan))
            return std::unexpected(*gaveUp);
    }
    return WTF::move(plans);
}

Vector<LoopVersionPlan, 2> LoopVersionPlanner::tryUnswitch(const NaturalLoop& loop, const UnswitchCandidate& candidate)
{
    if (!candidate.block)
        return { };
    auto tryEdge = [&](BasicBlock* successor) -> std::expected<LoopVersionPlan, GaveUp> {
        UnswitchedEdge edge { candidate.block, successor };
        auto region = findHotRegion(m_loops, loop, edge);
        if (!region)
            return std::unexpected(region.error());
        return tryPlan(*region, PhiResolution::Keep, edge, nullptr);
    };
    auto first = tryEdge(candidate.successors[0]);
    if (!first)
        return { };
    auto second = tryEdge(candidate.successors[1]);
    if (!second)
        return { };
    if (first->numberOfClonedValues + second->numberOfClonedValues > m_proc.limit(Limit::MaxClonedValuesPerUnswitchedLoop)) {
        m_proc.didReachLimit(Limit::MaxClonedValuesPerUnswitchedLoop, candidate.block->last()->origin());
        return { };
    }
    first->unswitched = UnswitchedBranch { candidate.block->last(), candidate.successors[1] };
    second->isEnteredFromPreHeader = false;
    Vector<LoopVersionPlan, 2> plans;
    plans.append(WTF::move(*first));
    plans.append(WTF::move(*second));
    return plans;
}

static bool isLoopTestSameOnRarePaths(const LoopVersionPlan& plan)
{
    const CountedLoop& loop = *plan.countedLoop;
    if (loop.test->last()->child(0)->opcode() == Phi)
        return false;
    return !plan.hoisted.isHoisted(loop.bound) || plan.hoisted.valuesValidInExcursion.contains(loop.bound);
}

// unrollLoops leaves a loop with a call alone. It unrolls other loops only if each side path finishes its iteration on its own.
static bool callsInLikelyBlocks(const HotRegion& region)
{
    for (BasicBlock* block : region.blocks) {
        for (Value* value : *block) {
            if (isCall(value) && !isLoopPoll(value))
                return true;
        }
    }
    return false;
}

std::optional<GaveUp> LoopVersionPlanner::finishPlan(LoopVersionPlan& plan)
{
    bool mayBeUnrolled = !callsInLikelyBlocks(plan.region);
    plan.excursion = findExcursion(m_proc, m_loops, plan.region, plan.hoisted, [&](Value* value) {
        return mayBeUnrolled || plan.rangeChecks.strided || plan.rangeChecks.decidedComparisons.contains(value);
    });
    if (plan.rangeChecks.dependsOnLoopTest() && plan.excursion.passesThrough(plan.countedLoop->test) && !isLoopTestSameOnRarePaths(plan))
        plan.excursion = { };
    if (plan.firstIterationSlowPath && !plan.excursion.contains(plan.firstIterationSlowPath) && !plan.unswitched && plan.isEnteredFromPreHeader)
        return GaveUp(GaveUpReason::NoSingleLikelyPath);
    return std::nullopt;
}

std::expected<LoopVersionPlan, GaveUp> LoopVersionPlanner::tryPlan(const HotRegion& region, PhiResolution resolution, std::optional<UnswitchedEdge> unswitched, UnswitchCandidate* candidate)
{
    Attempt attempt(*this, region, resolution, unswitched, candidate);
    LoopVersionPlan& plan = attempt.plan;
    if (auto gaveUp = attempt.invariance.scanRegion())
        return std::unexpected(*gaveUp);
    plan.numberOfClonedValues = attempt.invariance.numberOfClonedValues();

    if (auto gaveUp = walkLikelyPath(attempt))
        return std::unexpected(*gaveUp);
    if (auto gaveUp = checkHoistThresholds(attempt))
        return std::unexpected(*gaveUp);
    findCountedLoop(attempt);
    bool canRewriteTest = plan.countedLoop && canRewriteLoopTest(attempt);
    if (plan.countedLoop && !canRewriteTest)
        plan.boundNotClampedReason = GaveUpReason::TestNotOnLikelyPath;
    bool clampsLoopBounds = m_proc.policy().loops.clampsLoopBounds;
    if (!clampsLoopBounds)
        plan.boundNotClampedReason = GaveUpReason::None;
    if (plan.countedLoop) {
        plan.rangeChecks = planRangeChecks({ m_proc, m_phiChildren, plan.region, attempt.invariance, *plan.countedLoop, attempt.blocksBeforeTopTest, canRewriteTest && clampsLoopBounds });
        plan.numberOfRemovedValues += countRemovedComparisons(attempt);
    } else if (clampsLoopBounds) {
        plan.rangeChecks = planRangeChecksOfMeetingCounters({ m_proc, m_loops, m_phiChildren, plan.region, attempt.invariance, mayBeEnteredOnAnyTurn(attempt) });
        plan.numberOfRemovedValues += countRemovedComparisons(attempt);
    }
    if (plan.rangeChecks.decidedComparisons.isEmpty() && !hoistsEnough(attempt))
        return std::unexpected(GaveUp(GaveUpReason::NothingToHoist));
    if (auto gaveUp = checkSpeculativeHoistBudget(attempt))
        return std::unexpected(*gaveUp);

    plan.hoisted = attempt.invariance.takePlan();
    return WTF::move(plan);
}

std::optional<GaveUp> LoopVersionPlanner::walkLikelyPath(Attempt& attempt)
{
    const HotRegion& region = attempt.plan.region;
    LoopInvariance& invariance = attempt.invariance;
    IndexSet<BasicBlock*> visited;
    for (BasicBlock* block = region.header; block && region.contains(block) && visited.add(block);) {
        for (Value* value : *block) {
            if (value != block->last())
                invariance.visit(value);
        }
        if (block->last()->opcode() == Jump) {
            block = block->successorBlock(0);
            continue;
        }
        BasicBlock* likelySuccessor = attempt.isUnswitchedBlock(block) ? attempt.unswitched->successor : likelySuccessorOfGuard(m_loops, attempt.loop(), block);
        if (!likelySuccessor) {
            if (attempt.candidate && invariance.hasInvariantChildren(block->last()))
                findUnswitchCandidate(m_loops, attempt.loop(), block, *attempt.candidate);
            break;
        }
        bool canHoistGuard = isHoist(invariance.canHoist(block->last()));
        if (attempt.isUnswitchedBlock(block) && !canHoistGuard)
            return GaveUp(GaveUpReason::NothingToHoist);
        if (canHoistGuard && !shouldKeepGuardInLoop(attempt, block, likelySuccessor))
            invariance.hoistGuard(block->last(), likelySuccessor);
        else
            visitLoopGuard(attempt, block, likelySuccessor);
        block = likelySuccessor;
    }
    return std::nullopt;
}

void LoopVersionPlanner::findUnswitchCandidate(NaturalLoops& loops, const NaturalLoop& loop, BasicBlock* block, UnswitchCandidate& result)
{
    if (block->last()->opcode() != Branch && block->last()->opcode() != Switch)
        return;
    unsigned numberOfLikelySuccessors = 0;
    FrequencyClass likeliestClass = likeliestSuccessorClass(block);
    for (const FrequentedBlock& successor : block->successors()) {
        if (!isLikelySuccessor(likeliestClass, successor) || !loops.belongsTo(successor.block(), loop))
            continue;
        if (successor.block() == loop.header())
            return;
        if (numberOfLikelySuccessors && (result.successors[0] == successor.block() || (numberOfLikelySuccessors == 2 && result.successors[1] == successor.block())))
            continue;
        if (numberOfLikelySuccessors == 2)
            return;
        result.successors[numberOfLikelySuccessors++] = successor.block();
    }
    if (numberOfLikelySuccessors == 2)
        result.block = block;
}

bool LoopVersionPlanner::shouldKeepGuardInLoop(Attempt& attempt, BasicBlock* block, BasicBlock* likelySuccessor)
{
    bool usesSpeculativeHoist = false;
    for (Value* child : block->last()->children())
        usesSpeculativeHoist |= attempt.invariance.isSpeculativelyHoisted(child);
    if (!usesSpeculativeHoist)
        return false;
    if (!attempt.rejoiningBlocks)
        attempt.rejoiningBlocks = findRejoiningBlocks(m_loops, attempt.plan.region, InvalidationSet { });
    for (BasicBlock* successor : block->successorBlocks()) {
        if (successor != likelySuccessor && attempt.rejoiningBlocks->contains(successor))
            return true;
    }
    return false;
}

void LoopVersionPlanner::visitLoopGuard(Attempt& attempt, BasicBlock* block, BasicBlock* likelySuccessor)
{
    LoopVersionPlan& plan = attempt.plan;
    const NaturalLoop& loop = attempt.loop();
    if (!plan.firstIterationSlowPath && block->last()->opcode() == Branch)
        plan.firstIterationSlowPath = findFirstIterationSlowPath(loop, block, likelySuccessor);
    if (attempt.invariance.addLoopGuard(block->last(), likelySuccessor) == LoopGuardKind::Redundant)
        return;

    bool slowPathStaysInLoop = false;
    for (BasicBlock* successor : block->successorBlocks())
        slowPathStaysInLoop |= successor != likelySuccessor && m_loops.belongsTo(successor, loop);
    if (slowPathStaysInLoop) {
        bool variesAcrossIterations = mayVaryAcrossIterations(attempt.invariance, block->last());
        plan.mayExitEveryIteration |= !variesAcrossIterations;
        if (variesAcrossIterations)
            attempt.variantGuards.append(block->last());
    }
    if (!attempt.topTest && !attempt.isUnswitchedBlock(block) && block->last()->opcode() == Branch) {
        const FrequentedBlock& other = block->successor(block->successorBlock(0) == likelySuccessor ? 1 : 0);
        if (!m_loops.belongsTo(other.block(), loop) && !other.isRare())
            attempt.topTest = block;
    }
}

bool LoopVersionPlanner::mayVaryAcrossIterations(const LoopInvariance& invariance, Value* terminal) const
{
    IndexSet<Value*> seen;
    Vector<Value*, 16> worklist { terminal };
    for (unsigned count = 0; !worklist.isEmpty(); ++count) {
        Value* value = worklist.takeLast();
        if (invariance.isInvariant(value) || !seen.add(value))
            continue;
        if (value->opcode() == Phi)
            return true;
        if (count >= m_proc.limit(Limit::MaxSourcesOfLoopTest)) {
            m_proc.didReachLimit(Limit::MaxSourcesOfLoopTest, terminal->origin());
            return true;
        }
        for (Value* child : value->children())
            worklist.append(child);
    }
    return false;
}

BasicBlock* LoopVersionPlanner::findFirstIterationSlowPath(const NaturalLoop& loop, BasicBlock* block, BasicBlock* likelySuccessor)
{
    auto rangeCheck = rangeCheckFor(block->last()->child(0));
    if (!rangeCheck)
        return nullptr;
    BasicBlock* inBoundsSuccessor = block->successorBlock(rangeCheck->isTrueInBounds ? 0 : 1);
    auto index = valueOnFirstIteration(loop, rangeCheck->index, m_proc.limit(Limit::MaxRangeAnalysisDepth));
    if (index && *index < 0 && isNeverNegative(rangeCheck->limit) && inBoundsSuccessor == likelySuccessor)
        return block->successorBlock(rangeCheck->isTrueInBounds ? 1 : 0);
    return nullptr;
}

std::optional<int64_t> LoopVersionPlanner::valueOnFirstIteration(const NaturalLoop& loop, Value* value, uint64_t timeToLive)
{
    if (value->hasInt())
        return value->asInt();
    if (value->opcode() == Add && value->type().isInt()) {
        if (!timeToLive) {
            m_proc.didReachLimit(Limit::MaxRangeAnalysisDepth, value->origin());
            return std::nullopt;
        }
        auto left = valueOnFirstIteration(loop, value->child(0), timeToLive - 1);
        auto right = left ? valueOnFirstIteration(loop, value->child(1), timeToLive - 1) : std::nullopt;
        if (!right)
            return std::nullopt;
        uint64_t sum = static_cast<uint64_t>(*left) + static_cast<uint64_t>(*right);
        return value->type() == Int32 ? static_cast<int32_t>(sum) : static_cast<int64_t>(sum);
    }
    if (value->opcode() != Phi || value->owner != loop.header())
        return std::nullopt;
    std::optional<int64_t> result;
    for (UpsilonValue* upsilon : m_phiChildren[value]) {
        if (m_loops.belongsTo(upsilon->owner, loop))
            continue;
        if (result || !upsilon->child(0)->hasInt())
            return std::nullopt;
        result = upsilon->child(0)->asInt();
    }
    return result;
}

bool LoopVersionPlanner::isNeverNegative(Value* value)
{
    auto hasNonNegativeRange = [&](Value* candidate) {
        return intervalFor(m_proc, candidate, Signedness::Signed).min >= 0;
    };
    if (hasNonNegativeRange(value))
        return true;
    if (value->opcode() != Phi)
        return false;
    auto upsilons = m_phiChildren[value];
    for (UpsilonValue* upsilon : upsilons) {
        if (!hasNonNegativeRange(upsilon->child(0)))
            return false;
    }
    return !!upsilons.size();
}

bool LoopVersionPlanner::hoistsEnough(const Attempt& attempt)
{
    const HoistPlan& hoisted = attempt.invariance.plan();
    return hoisted.hoistsGuard || hoisted.numberOfHoistedValues >= Limits::minHoistedValuesPerLoopVersion;
}

std::optional<GaveUp> LoopVersionPlanner::checkHoistThresholds(const Attempt& attempt) const
{
    const HoistPlan& hoisted = attempt.invariance.plan();
    if (attempt.unswitched && !hoisted.isHoisted(attempt.unswitched->block->last()))
        return GaveUp(GaveUpReason::NothingToHoist);
    if (attempt.plan.numberOfClonedValues - hoisted.numberOfHoistedValues + hoisted.numberOfPeeledValues > m_proc.limit(Limit::MaxClonedValuesPerLoopVersion)) {
        m_proc.didReachLimit(Limit::MaxClonedValuesPerLoopVersion, attempt.plan.region.header->at(0)->origin());
        return GaveUp::atLimit(Limit::MaxClonedValuesPerLoopVersion);
    }
    return std::nullopt;
}

static std::optional<IndexSet<BasicBlock*>> findBlocksBeforeTopTest(const HotRegion& region, const CountedLoop& countedLoop)
{
    IndexSet<BasicBlock*> result;
    Vector<BasicBlock*, 8> worklist { region.header };
    result.add(region.header);
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        for (Value* value : *block) {
            if (isLoopPoll(value))
                continue;
            Effects effects = value->effects();
            auto* upsilon = value->as<UpsilonValue>();
            if (effects.writes || effects.writesPinned || effects.exitsSideways || effects.fence || value->opcode() == Set || (upsilon && upsilon->phi()->owner == region.header))
                return std::nullopt;
        }
        for (BasicBlock* successor : block->successorBlocks()) {
            if (!region.contains(successor) || (block == countedLoop.test && successor == countedLoop.latch))
                continue;
            if (result.add(successor))
                worklist.append(successor);
        }
    }
    return result;
}

// A turn that has left the fast version by a slow path may end in front of the guards of the fast version.
bool LoopVersionPlanner::mayBeEnteredOnAnyTurn(const Attempt& attempt)
{
    return !attempt.unswitched && !attempt.plan.region.slowPathBlocks.isEmpty();
}

void LoopVersionPlanner::findCountedLoop(Attempt& attempt)
{
    LoopVersionPlan& plan = attempt.plan;
    const HotRegion& region = plan.region;
    const LoopInvariance& invariance = attempt.invariance;
    auto analyze = [&](std::optional<LoopTest> test) {
        return analyzeCountedLoop({ m_proc, m_loops, attempt.loop(), m_phiChildren, [&](Value* value) { return invariance.isInvariant(value); }, test });
    };
    auto countedLoop = analyze(std::nullopt);
    if (!countedLoop && attempt.topTest && attempt.resolution == PhiResolution::Resolve) {
        Value* condition = attempt.topTest->last()->child(0);
        if (condition->opcode() == Phi && condition->owner != region.header)
            condition = invariance.singleInputInFastVersion(condition);
        if (condition)
            countedLoop = analyze(LoopTest { attempt.topTest, condition });
    }
    if (!countedLoop) {
        plan.boundNotClampedReason = countedLoop.error();
        return;
    }
    if (!invariance.isAvailableInPreHeader(countedLoop->start) || !invariance.isAvailableInPreHeader(countedLoop->bound)) {
        plan.boundNotClampedReason = GaveUpReason::BoundNotInvariant;
        return;
    }
    if (countedLoop->isTopTested) {
        auto blocksBeforeTopTest = findBlocksBeforeTopTest(region, *countedLoop);
        if (!blocksBeforeTopTest) {
            plan.boundNotClampedReason = GaveUpReason::EffectsBeforeTopTest;
            return;
        }
        attempt.blocksBeforeTopTest = WTF::move(*blocksBeforeTopTest);
    }
    countedLoop->isEnteredOnAnyTurn = mayBeEnteredOnAnyTurn(attempt);
    plan.countedLoop = *countedLoop;
}

bool LoopVersionPlanner::canRewriteLoopTest(const Attempt& attempt) const
{
    const LoopVersionPlan& plan = attempt.plan;
    const HotRegion& region = plan.region;
    BasicBlock* test = plan.countedLoop->test;
    if (!region.contains(test) || !region.contains(plan.countedLoop->latch))
        return false;
    if (attempt.invariance.plan().isHoisted(test->last()))
        return false;
    Dominators& dominators = m_proc.dominators();
    for (BasicBlock* predecessor : region.header->predecessors()) {
        if (m_loops.belongsTo(predecessor, *region.loop) && !dominators.dominates(test, predecessor))
            return false;
    }
    return true;
}

unsigned LoopVersionPlanner::countRemovedComparisons(const Attempt& attempt) const
{
    unsigned result = 0;
    for (auto& entry : attempt.plan.rangeChecks.decidedComparisons) {
        Value* comparison = entry.key;
        if (attempt.invariance.plan().isHoisted(comparison))
            continue;
        Value* last = comparison->owner->last();
        result += emitsInstructions(comparison) + (last->opcode() == Branch && last->child(0) == comparison && m_useCounts.numUses(comparison) == 1);
    }
    return result;
}

std::optional<GaveUp> LoopVersionPlanner::checkSpeculativeHoistBudget(const Attempt& attempt) const
{
    const HoistPlan& hoisted = attempt.invariance.plan();
    if (attempt.plan.numberOfClonedValues - hoisted.numberOfHoistedValues + hoisted.numberOfSpeculativeHoists <= m_proc.limit(Limit::MaxClonedValuesPerLoopVersion))
        return std::nullopt;
    for (Value* guard : attempt.variantGuards) {
        if (guard->opcode() != Branch || !attempt.plan.rangeChecks.decidedComparisons.contains(guard->child(0))) {
            m_proc.didReachLimit(Limit::MaxClonedValuesPerLoopVersion, attempt.plan.region.header->at(0)->origin());
            return GaveUp::atLimit(Limit::MaxClonedValuesPerLoopVersion);
        }
    }
    return std::nullopt;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
