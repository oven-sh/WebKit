/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
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
{
}

AllocationSinking::~AllocationSinking() = default;

void AllocationSinking::analyze()
{
    while (hasCandidates()) {
        solve();
        count();
        if (!abandonUnprofitableCandidates() && !m_mustSolveAgain)
            return;
    }
    invalidateResults();
}

void AllocationSinking::invalidateResults()
{
    m_heads.fill(std::nullopt, m_function.blocks.size());
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
    const std::optional<SinkingState>& head = m_heads[block.index()];
    if (!head || !head->isReachable)
        return nullptr;
    return &*head;
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

void AllocationSinking::didBuildAfterMerge(VirtualObjectID object, BlockID mergeBlock)
{
    Candidate& candidate = at(object);
    if (!candidate.mergeBlock)
        candidate.mergeBlock = mergeBlock;
}

PassOutcome sinkAllocations(FunctionPassContext& context)
{
    constexpr unsigned maxEdgeSplittingRounds = 3;

    PassOutcome outcome = PassOutcome::Unchanged;
    for (unsigned round = 0; round < maxEdgeSplittingRounds; ++round) {
        AllocationSinking sinking(context.analyses, SinkingPolicy { });
        if (!sinking.collectCandidates())
            return outcome;
        sinking.analyze();

        IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
        if (sinking.duplicateLiveRange(builder))
            return PassOutcome::Changed;
        if (!sinking.hasCandidates()) {
            sinking.recordLosses();
            return outcome;
        }
        if (sinking.splitEdgesNeedingCode(builder)) {
            outcome = PassOutcome::Changed;
            continue;
        }

        sinking.rewrite(builder);
        sinking.recordLosses();
        sinking.countStatistics();
        return PassOutcome::Changed;
    }

    ASSERT_NOT_REACHED();
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
