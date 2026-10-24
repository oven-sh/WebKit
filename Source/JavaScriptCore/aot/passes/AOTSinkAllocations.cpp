/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static SunkAllocation outcomeOf(ValueID allocation, const Candidate& candidate)
{
    if (candidate.isAbandoned)
        return { .allocation = allocation, .outcome = SunkAllocationOutcome::Abandoned, .limit = candidate.abandonedBy };
    if (!candidate.numberOfBuilds)
        return { .allocation = allocation, .outcome = SunkAllocationOutcome::Removed };
    return { .allocation = allocation, .outcome = SunkAllocationOutcome::Sunk, .numberOfBuilds = candidate.numberOfBuilds };
}

AllocationSinking::AllocationSinking(FunctionAnalyses& analyses, const SinkingPolicy& policy)
    : m_analyses(analyses)
    , m_program(analyses.program())
    , m_function(analyses.function())
    , m_policy(policy)
    , m_objectContext { analyses.program(), analyses.function(), analyses.context().flow }
    , m_candidateOfValue(analyses.function().values.size())
    , m_checksShortcuts(analyses.context().build.options().verifiesAfterEveryPass)
    , m_maxWork(limit(LimitID::MaxSinkingWork))
{
}

AllocationSinking::~AllocationSinking() = default;

void AllocationSinking::analyze()
{
    m_joinsSetAside.shrink(0);
    m_setsAsideJoins = true;
    setAsideJoinsOfAllocationsBuiltInTheirBlocks();
    analyzeUntilNothingChanges();
    m_setsAsideJoins = false;
    if (m_joinsSetAside.isEmpty() || !hasCandidates())
        return;

    Vector<Candidate> candidatesWithoutJoins = m_candidates;
    SinkingDecisions decisionsWithoutJoins = m_decisions;
    uint64_t workWithoutJoins = m_work;
    takeBackJoinsSetAside();
    analyzeUntilNothingChanges();
    if (hasCandidates() || !hasSpentItsWork())
        return;

    m_candidates = WTF::move(candidatesWithoutJoins);
    m_decisions = WTF::move(decisionsWithoutJoins);
    m_work = 0;
    analyzeUntilNothingChanges();
    m_work += workWithoutJoins;
}

bool AllocationSinking::analyzeStartingFrom(SinkingFindings&& findings)
{
    auto isSameCandidate = [](const Candidate& a, const Candidate& b) {
        return a.definition == b.definition && a.block == b.block && a.isJoin == b.isJoin && a.family == b.family && a.descriptionIndex == b.descriptionIndex && a.tagBitWhenAbsent == b.tagBitWhenAbsent && a.isLeftOut == b.isLeftOut;
    };
    if (!std::ranges::equal(m_candidates, findings.candidates, isSameCandidate))
        return false;

    m_candidates = WTF::move(findings.candidates);
    m_decisions = WTF::move(findings.decisions);
    m_decisions.takeDidChange();
    analyzeUntilNothingChanges();
    return true;
}

void AllocationSinking::analyzeUntilNothingChanges()
{
    while (hasCandidates()) {
        solve();
        if (hasSpentItsWork())
            break;
        count();
        if (!abandonUnprofitableCandidates() && !m_mustSolveAgain)
            return;
    }
    invalidateResults();
}

void AllocationSinking::invalidateResults()
{
    m_heads.fill(std::nullopt, m_function.blocks.size());
    m_blocksEnteredWithNothing.clearAll();
    resetCounters();
}

bool AllocationSinking::hasCandidates() const
{
    return std::ranges::any_of(m_candidates, [](const Candidate& candidate) {
        return !candidate.isJoin && !candidate.isAbandoned;
    });
}

Vector<SunkAllocation> AllocationSinking::outcomes() const
{
    Vector<SunkAllocation> result;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            if (interpretedOf(m_function.at(id).opcode) != Interpretation::Allocation)
                continue;
            if (VirtualObjectID object = m_candidateOfValue[id.index()])
                result.append(outcomeOf(id, at(object)));
            else
                result.append(SunkAllocation { .allocation = id, .outcome = SunkAllocationOutcome::NotACandidate });
        }
    }
    return result;
}

ShapeContext AllocationSinking::shapeContext(VirtualObjectID object) const
{
    return { m_objectContext, familyOf(object), m_orders };
}

const VirtualObjectDescription& AllocationSinking::descriptionOf(VirtualObjectID object) const
{
    const Candidate& candidate = at(object);
    RELEASE_ASSERT(!candidate.isJoin);
    return familyOf(object).descriptionAt(candidate.descriptionIndex);
}

VirtualObjectID AllocationSinking::candidateDefinedBy(ValueID value) const
{
    if (value.index() >= m_candidateOfValue.size())
        return { };
    VirtualObjectID object = m_candidateOfValue[value.index()];
    if (!object || at(object).isAbandoned)
        return { };
    return object;
}

const SinkingState* AllocationSinking::headOf(BlockID block) const
{
    if (block.index() >= m_heads.size())
        return nullptr;
    if (m_blocksEnteredWithNothing.get(block.index())) {
        m_blocksEnteredWithNothing.quickClear(block.index());
        m_heads[block.index()] = SinkingState::nothingAt(block);
    }
    const std::optional<SinkingState>& head = m_heads[block.index()];
    if (!head || !head->isReachable)
        return nullptr;
    return &*head;
}

bool AllocationSinking::isLiveAtHead(ValueID value, BlockID block) const
{
    if (!m_liveness) [[unlikely]] {
        if (!LivenessOfAskedValues::answersFor(m_analyses.opcodesOfValues()))
            return m_analyses.liveness().isLiveAtHead(value, block);
        m_liveness.emplace(m_function, m_analyses.cfg(), m_analyses.useLists());
    }
    bool isLive = m_liveness->isLiveAtHead(value, block);
    if (m_checksShortcuts) [[unlikely]]
        RELEASE_ASSERT(isLive == m_analyses.liveness().isLiveAtHead(value, block));
    return isLive;
}

const BitVector& AllocationSinking::findBlocksWithLiveDefinition(VirtualObjectID object)
{
    const Candidate& candidate = at(object);
    BitVector& blocks = m_blocksWithLiveDefinition[object.index()].emplace();
    // Makes the sets of single values, where those answer.
    isLiveAtHead(candidate.definition, m_analyses.cfg().root());
    if (m_liveness && !m_checksShortcuts)
        blocks = m_liveness->blocksEnteredWith(candidate.definition);
    blocks.ensureSize(m_function.blocks.size());
    if (!m_liveness || m_checksShortcuts) {
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (isLiveAtHead(candidate.definition, block))
                blocks.quickSet(block.index());
        }
    }
    if (candidate.isJoin)
        blocks.quickSet(candidate.block.index());
    return blocks;
}

uint64_t AllocationSinking::limit(LimitID id) const
{
    return context().build.limit(id);
}

bool AllocationSinking::exceeds(LimitID id, uint64_t amount, ValueID value)
{
    if (amount <= limit(id))
        return false;
    if (m_policy.recordsLosses)
        context().build.gaveUp(id, m_analyses.subject(value));
    return true;
}

void AllocationSinking::abandon(VirtualObjectID object, LimitID limit)
{
    Candidate& candidate = at(object);
    if (candidate.isAbandoned)
        return;

    candidate.isAbandoned = true;
    candidate.abandonedBy = limit;
    m_mustSolveAgain = true;
    if (limit != LimitID::None && m_policy.recordsLosses)
        context().build.gaveUp(limit, m_analyses.subject(candidate.definition));
}

bool AllocationSinking::isIgnored(Edge edge) const
{
    return m_ignoredEdges.contains(edge);
}

void AllocationSinking::addVirtualReturn(VirtualReturn&& virtualReturn)
{
    m_returns.append(WTF::move(virtualReturn));
}

void AllocationSinking::addEdgeNeedingCode(Edge edge)
{
    m_edgesNeedingCode.appendIfNotContains(edge);
}

bool AllocationSinking::mayBeListOfNumbers(VirtualObjectID object) const
{
    return std::ranges::any_of(familyOf(object).descriptions(), [&](const VirtualObjectDescription& description) {
        return classifyListOfNumbers(m_program, description) != ListOfNumbers::No;
    });
}

void AllocationSinking::didBuildAfterMerge(VirtualObjectID object, BlockID mergeBlock)
{
    Candidate& candidate = at(object);
    if (!candidate.mergeBlock)
        candidate.mergeBlock = mergeBlock;
}

PassOutcome sinkAllocations(FunctionPassContext& context)
{
    AOT_THRESHOLD(unsigned, maxEdgeSplittingRounds, 3);
    constexpr OpcodeSet allocations = opcodesInterpretedAs(Interpretation::Allocation);

    if (!context.analyses.opcodesOfValues().intersects(allocations))
        return PassOutcome::Unchanged;
    if (4 * context.analyses.cfg().reversePostOrder().size() > 3 * context.numberOfBlocksWhenSinkingGaveUp && context.numberOfBlocksWhenSinkingGaveUp)
        return PassOutcome::Unchanged;

    SinkingPolicy policy { .leavesWhenNoAllocationPays = true };
    if (!context.build.options().verifiesAfterEveryPass)
        policy.tracesOfFruitlessAllocations = &context.tracesOfAllocationsNotWorthSinking;
    PassOutcome outcome = PassOutcome::Unchanged;
    std::optional<SinkingFindings> findingsBeforeSplitting;
    bool hasGivenUp = false;
    for (unsigned round = 0; round < maxEdgeSplittingRounds; ++round) {
        AllocationSinking sinking(context.analyses, policy);
        if (!sinking.collectCandidates()) {
            sinking.recordLosses();
            sinking.addTracesOfFruitlessAllocations(context.tracesOfAllocationsNotWorthSinking);
            return outcome;
        }
        if (!findingsBeforeSplitting || !sinking.analyzeStartingFrom(WTF::move(*findingsBeforeSplitting)))
            sinking.analyze();
        hasGivenUp |= sinking.hasSpentItsWork() || sinking.hasStartedOverWithFewerCandidates();
        if (hasGivenUp)
            context.numberOfBlocksWhenSinkingGaveUp = context.analyses.cfg().reversePostOrder().size();

        IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
        if (sinking.testElementsOfLists(builder) || sinking.duplicateLiveRange(builder))
            return PassOutcome::Changed;
        if (!sinking.hasCandidates()) {
            sinking.recordLosses();
            sinking.addTracesOfFruitlessAllocations(context.tracesOfAllocationsNotWorthSinking);
            return outcome;
        }
        if (sinking.splitEdgesNeedingCode(builder)) {
            outcome = PassOutcome::Changed;
            findingsBeforeSplitting = sinking.takeFindings();
            continue;
        }

        sinking.addTracesOfFruitlessAllocations(context.tracesOfAllocationsNotWorthSinking);
        sinking.rewrite(builder);
        sinking.raiseFrequenciesBehindDecidedBranches();
        sinking.recordLosses();
        sinking.countStatistics();
        return PassOutcome::Changed;
    }

    ASSERT_NOT_REACHED();
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
