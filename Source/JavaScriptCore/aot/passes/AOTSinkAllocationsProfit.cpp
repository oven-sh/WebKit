/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTStatistics.h"

namespace JSC { namespace AOT { namespace Typed {

void AllocationSinking::resetCounters()
{
    for (Candidate& candidate : m_candidates) {
        // Count no longer tracks an abandoned candidate: recordDecline() and duplicateLiveRange() read what its last count found.
        if (candidate.isAbandoned)
            continue;
        candidate.needsGuard = false;
        candidate.diesVirtual = false;
        candidate.isStoredLate = false;
        candidate.numberOfBuilds = 0;
        candidate.numberOfRarerBuilds = 0;
        candidate.numberOfFoldedStores = 0;
        candidate.numberOfRepeatedStores = 0;
        candidate.mergeBlock = BlockID();
        candidate.firstCause = std::nullopt;
    }
    m_returns.clear();
    m_edgesNeedingCode.clear();
}

void AllocationSinking::count()
{
    resetCounters();
    for (BlockID block : m_analyses.cfg().reversePostOrder())
        countBlock(block);
}

void AllocationSinking::countBlock(BlockID block)
{
    const SinkingState* head = headOf(block);
    if (!head)
        return;

    const Block& source = m_function.at(block);
    const Terminator& terminator = source.terminator;
    SinkingState state = *head;
    ObjectInterpreter interpreter(*this, SinkingMode::Count, state);
    if (!state.objects.isEmpty() || m_inputs.blocksWithCandidates.get(block.index())) {
        for (ValueID id : source.values)
            interpreter.execute(id);
    }
    interpreter.executeTerminator(terminator);

    if (terminator.targets.isEmpty()) {
        for (const ObjectEntry& entry : state.objects) {
            if (!entry.state.isCell())
                at(entry.object).diesVirtual = true;
        }
        return;
    }

    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        const SinkingState* headOfTarget = headOf(terminator.targets[targetIndex].block);
        if (!headOfTarget || interpreter.isDeadEdge(terminator, targetIndex))
            continue;
        if (state.objects.isEmpty() && headOfTarget->objects.isEmpty())
            continue;
        OutgoingEdge outgoing = outgoingEdgeOf(m_function, Edge { block, targetIndex });
        SinkingState stateOnEdge = state;
        ObjectInterpreter(*this, SinkingMode::Count, stateOnEdge).enterEdge(outgoing, &terminator, headOfTarget);
    }
}

// The limit is on the copies that run as often as the allocation. A build on a rarer path takes the allocation out of the code that runs more often.
static bool exceedsMaxBuilds(const Candidate& candidate, uint64_t maxNumberOfBuilds)
{
    return candidate.numberOfBuilds - candidate.numberOfRarerBuilds > maxNumberOfBuilds;
}

bool AllocationSinking::paysToSink(const Candidate& candidate) const
{
    if (!candidate.numberOfBuilds)
        return true;
    if (candidate.diesVirtual)
        return true;
    // An object that refers to its holder is stored late. As a cell it would have its holder built where it is allocated, in front of the stores that follow.
    return candidate.numberOfBuilds == 1 && (candidate.numberOfFoldedStores > candidate.numberOfRepeatedStores || candidate.isStoredLate);
}

// An object that several paths would build is not given up: it is built once, in front of the first branch it reaches, and keeps the stores that were folded until there.
// What refers to it is judged again after that, because it is built as often as its holder.
bool AllocationSinking::buildBeforeBranchesInsteadOfAbandoning()
{
    uint64_t maxNumberOfBuilds = limit(LimitID::MaxMaterializations);
    bool changed = false;
    for (Candidate& candidate : m_candidates) {
        if (candidate.isAbandoned || candidate.isJoin || candidate.isBuiltBeforeBranches)
            continue;
        if (candidate.numberOfBuilds < 2 || candidate.numberOfFoldedStores <= candidate.numberOfRepeatedStores)
            continue;
        if (!exceedsMaxBuilds(candidate, maxNumberOfBuilds) && paysToSink(candidate))
            continue;
        candidate.isBuiltBeforeBranches = true;
        changed = true;
    }
    return changed;
}

bool AllocationSinking::abandonUnprofitableCandidates()
{
    if (buildBeforeBranchesInsteadOfAbandoning())
        return true;

    uint64_t maxNumberOfBuilds = limit(LimitID::MaxMaterializations);
    for (uint32_t index = 0; index < numberOfCandidates(); ++index) {
        VirtualObjectID object(index);
        const Candidate& candidate = at(object);
        if (candidate.isAbandoned)
            continue;
        if (!candidate.isJoin && exceedsMaxBuilds(candidate, maxNumberOfBuilds))
            abandon(object, LimitID::MaxMaterializations);
        else if (!paysToSink(candidate))
            abandon(object);
    }
    return m_mustSolveAgain;
}

void AllocationSinking::recordLosses()
{
    if (!m_policy.recordsLosses)
        return;

    for (const Candidate& candidate : m_candidates) {
        if (candidate.isJoin)
            continue;
        if (candidate.isAbandoned) {
            recordDecline(candidate);
            continue;
        }
        if (!candidate.numberOfBuilds && m_tables)
            recordRemovedAllocation(candidate);
    }
}

void AllocationSinking::recordRemovedAllocation(const Candidate& candidate)
{
    BuildContext& build = context().build;
    if (!build.options().collectsAllocationRemarks())
        return;

    const Value& value = m_function.at(candidate.definition);
    if (!Value::opcodesWithAllocationSite.contains(value.opcode))
        return;
    AllocationSiteID site = value.allocationSite();
    if (!site)
        return;
    build.addAllocationRemark({ .site = site, .location = m_function.locationOf(candidate.definition), .outcome = AllocationOutcome::Removed });
}

void AllocationSinking::recordDecline(const Candidate& candidate)
{
    BuildContext& build = context().build;
    if (!build.options().collectsDeclines())
        return;

    LimitID reachedLimit = candidate.abandonedBy;
    EscapeReasons reasons;
    if (candidate.firstCause) {
        reasons = candidate.firstCause->reasons;
        if (reachedLimit == LimitID::None)
            reachedLimit = candidate.firstCause->limit;
    }
    build.addDecline({
        .kind = DeclineKind::AllocationNotSunk,
        .limit = reachedLimit,
        .reason = encodeDeclineReason(DeclineKind::AllocationNotSunk, reasons),
        .witness = witnessOf(m_analyses.subject(candidate.definition)),
    });
}

void AllocationSinking::countStatistics()
{
    Statistics& statistics = context().build.statistics();
    for (uint32_t index = 0; index < numberOfCandidates(); ++index) {
        VirtualObjectID object(index);
        const Candidate& candidate = at(object);
        if (candidate.isAbandoned)
            continue;
        if (candidate.isJoin) {
            const SinkingState* head = headOf(candidate.block);
            if (head && head->find(object))
                statistics.count(StatisticID::AllocationsMergedAtJoins);
        } else
            statistics.count(candidate.numberOfBuilds ? StatisticID::AllocationsSunk : StatisticID::AllocationsRemoved);
        if (candidate.numberOfBuilds)
            statistics.count(StatisticID::StoresFoldedIntoAllocations, candidate.numberOfFoldedStores);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
