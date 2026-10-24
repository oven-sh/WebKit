/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPlanner.h"
#include "AOTSort.h"
#include "AOTStatistics.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<ObjectMutation> shapeChangingMutations {
    ObjectMutation::DeleteField,
    ObjectMutation::RedefineProperty,
    ObjectMutation::AddProperty,
    ObjectMutation::ChangeIntegrity,
    ObjectMutation::ChangePrototype,
    ObjectMutation::StoreOfOtherType,
};

static bool isFrameAllocatableClosure(const Program& program, const Value& closure)
{
    const Function& callee = program.at(closure.function());
    return !closure.isMadeOnDemand() && !callee.has(FunctionFlags::TakesCallee) && callee.hasOrdinaryFrame();
}

static bool isFrameAllocatableObject(const AllocationSinking& sinking, const Value& object)
{
    const Program& program = sinking.program();
    LayoutID id = object.layout();
    const Layout& layout = program.at(id);
    if (layout.fields.size() > sinking.limit(LimitID::MaxFrameEnvironmentFields))
        return false;

    switch (layout.kind) {
    case LayoutKind::Environment:
        return true;
    case LayoutKind::CoroutineFrame:
    case LayoutKind::Closure:
    case LayoutKind::ModuleNamespace:
        return false;
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        break;
    }

    if (object.prototypeOperand() || layout.has(LayoutFlags::IsOfCreatedObject) || layout.flow.fieldCreation != FieldCreation::OnlyAtConstruction)
        return false;
    // The verifier allows an on-demand closure exactly one use, so the allocation that lists it cannot be duplicated.
    for (uint32_t field = 0; field < layout.fields.size(); ++field) {
        if (flowOf(program, FieldReference { id, field }).location.deferral != ClosureDeferral::Unknown)
            return false;
    }

    const FlowResults* flow = sinking.objectContext().flow;
    AllocationClassID allocationClass = object.allocationClass();
    OptionSet<ObjectMutation> impossibleMutations = flow && allocationClass ? flow->at(allocationClass).impossibleMutations : layout.flow.impossibleMutations;
    return impossibleMutations.containsAll(shapeChangingMutations);
}

// Mirrors the checks FramePlanner makes on the allocation itself. FramePlanner still decides, from the uses, whether a copy goes in the frame.
bool isFrameAllocatable(const AllocationSinking& sinking, const Candidate& candidate)
{
    if (candidate.isJoin || !sinking.context().build.isEnabled(PassID::AllocateInFrames, sinking.analyses().subject().function))
        return false;
    const Value& definition = sinking.function().at(candidate.definition);
    if (!hasStorageChoice(definition) || arrayAllocationsWithStorage.contains(definition.opcode))
        return false;
    if (definition.opcode == Opcode::NewClosure)
        return isFrameAllocatableClosure(sinking.program(), definition);
    return isFrameAllocatableObject(sinking, definition);
}

void AllocationSinking::resetCounters()
{
    for (Candidate& candidate : m_candidates) {
        // Count no longer tracks an abandoned candidate: recordDecline() and duplicateLiveRange() read what its last count found.
        if (candidate.isAbandoned)
            continue;
        candidate.needsGuard = false;
        candidate.diesVirtual = false;
        candidate.diesOutsideJoins = false;
        candidate.holdersThatStoreItLate.shrink(0);
        candidate.numberOfBuilds = 0;
        candidate.numberOfRarerBuilds = 0;
        candidate.numberOfBorrowingBuilds = 0;
        candidate.numberOfFoldedStores = 0;
        candidate.numberOfRepeatedStores = 0;
        candidate.mergeBlock = BlockID();
        candidate.firstCause = std::nullopt;
    }
    m_returns.clear();
    m_edgesNeedingCode.clear();
    m_entriesIntoJoins.shrink(0);
}

void AllocationSinking::count()
{
    resetCounters();
    findWhetherObjectsAreBuiltBeforeBranches();
    ++m_numberOfSweeps;
    for (BlockID block : m_analyses.cfg().reversePostOrder())
        countBlock(block);
}

void AllocationSinking::countBlock(BlockID block)
{
    const Block& source = m_function.at(block);
    const Terminator& terminator = source.terminator;
    // Most blocks have nothing to do with any object.
    if (holdsNothingAtHead(block) && !m_checksShortcuts && !m_inputs.blocksWithCandidates.get(block.index()) && !mentionsName(terminator, block)) {
        bool entersBlockWithObjects = std::ranges::any_of(terminator.targets, [&](const Target& target) {
            return hasObjectsAtHead(target.block);
        });
        if (!entersBlockWithObjects) {
            didWork(1);
            return;
        }
    }

    const SinkingState* head = headOf(block);
    if (!head)
        return;
    SinkingState state = *head;
    didWork(1 + state.objects.size());
    ObjectInterpreter interpreter(*this, SinkingMode::Count, state);
    if (!state.objects.isEmpty() || m_inputs.blocksWithCandidates.get(block.index())) {
        prepare(block);
        for (uint32_t entry : relevantValuesOf(block))
            interpreter.executeRelevantValue(source.values, entry);
        if (m_checksShortcuts) [[unlikely]]
            interpreter.checkThatOtherValuesChangeNothing(block);
    }
    interpreter.executeTerminatorOf(block);

    if (terminator.targets.isEmpty()) {
        for (const ObjectEntry& entry : state.objects) {
            if (entry.state.isCell())
                continue;
            at(entry.object).diesVirtual = true;
            at(entry.object).diesOutsideJoins = true;
        }
        return;
    }

    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        const SinkingState* headOfTarget = headOf(terminator.targets[targetIndex].block);
        if (!headOfTarget || interpreter.isDeadEdge(terminator, targetIndex))
            continue;
        if (state.objects.isEmpty() && headOfTarget->objects.isEmpty())
            continue;
        Edge edge { block, targetIndex };
        BlockID target = terminator.targets[targetIndex].block;
        OutgoingEdge outgoing = isPlain(edge, target) && !m_checksShortcuts ? OutgoingEdge { edge, target, { } } : outgoingEdgeOf(m_function, edge);
        bool isLastTarget = targetIndex + 1 == terminator.targets.size();
        SinkingState stateOnEdge = isLastTarget ? WTF::move(state) : SinkingState(state);
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
    // The object escapes on rarer paths only. With one copy per path, the copy on the frequent paths does not escape and can be allocated in the frame.
    unsigned numberOfFrequentBuilds = candidate.numberOfBuilds - candidate.numberOfRarerBuilds;
    if (candidate.numberOfRarerBuilds && candidate.numberOfBorrowingBuilds && numberOfFrequentBuilds == candidate.numberOfBorrowingBuilds && isFrameAllocatable(*this, candidate))
        return true;
    if (candidate.numberOfBuilds != 1)
        return false;
    auto foldsStores = [](const Candidate& object) {
        return object.numberOfFoldedStores > object.numberOfRepeatedStores;
    };
    // An object that refers to its holder is stored late. As a cell it would have its holder built where it is allocated, in front of the stores that follow.
    return foldsStores(candidate) || std::ranges::any_of(candidate.holdersThatStoreItLate, [&](VirtualObjectID holder) {
        return foldsStores(at(holder));
    });
}

// An object that several paths would build is not given up: it is built once, in front of the first branch it reaches, and keeps the stores that were folded until there.
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
    if (changed)
        setAsideJoinsThatCarryOnlyCells();
    return changed;
}

void AllocationSinking::abandonJoinsThatOnlyFeedAbandonedJoins()
{
    auto indexOfJoin = [](const EntryIntoJoin& entry) { return entry.join.index(); };
    auto indexOfSource = [](const EntryIntoJoin& entry) { return entry.source.index(); };
    AOT::sort(m_entriesIntoJoins, { }, [](const EntryIntoJoin& entry) {
        return std::pair { entry.join.index(), entry.source.index() };
    });
    removeRepeatedElements(m_entriesIntoJoins);
    Vector<EntryIntoJoin> entriesBySource = m_entriesIntoJoins;
    AOT::stableSort(entriesBySource, { }, indexOfSource);

    auto goesOnlyIntoAbandonedJoins = [&](VirtualObjectID object) {
        const Candidate& candidate = at(object);
        if (!candidate.isJoin || candidate.isAbandoned || candidate.diesOutsideJoins || !candidate.holdersThatStoreItLate.isEmpty() || candidate.numberOfFoldedStores)
            return false;
        bool hasAbandonedSource = std::ranges::any_of(std::ranges::equal_range(m_entriesIntoJoins, object.index(), { }, indexOfJoin), [&](const EntryIntoJoin& entry) {
            return at(entry.source).isAbandoned;
        });
        return !hasAbandonedSource && std::ranges::all_of(std::ranges::equal_range(entriesBySource, object.index(), { }, indexOfSource), [&](const EntryIntoJoin& entry) {
            return at(entry.join).isAbandoned;
        });
    };

    Vector<VirtualObjectID, 16> abandonedJoins;
    for (size_t index = 0; index < m_entriesIntoJoins.size(); ++index) {
        VirtualObjectID join = m_entriesIntoJoins[index].join;
        if ((!index || m_entriesIntoJoins[index - 1].join != join) && at(join).isAbandoned)
            abandonedJoins.append(join);
    }
    while (!abandonedJoins.isEmpty()) {
        VirtualObjectID join = abandonedJoins.takeLast();
        for (const EntryIntoJoin& entry : std::ranges::equal_range(m_entriesIntoJoins, join.index(), { }, indexOfJoin)) {
            if (!goesOnlyIntoAbandonedJoins(entry.source))
                continue;
            abandon(entry.source);
            abandonedJoins.append(entry.source);
        }
    }
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
    if (m_mustSolveAgain)
        abandonJoinsThatOnlyFeedAbandonedJoins();
    if (m_mustSolveAgain)
        setAsideJoinsThatCarryOnlyCells();
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
            if (!candidate.wasFruitlessBefore)
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
            const ObjectState* state = head ? head->find(object) : nullptr;
            if (state && !state->isCell())
                statistics.count(StatisticID::AllocationsMergedAtJoins);
        } else {
            statistics.count(candidate.numberOfBuilds ? StatisticID::AllocationsSunk : StatisticID::AllocationsRemoved);
            if (candidate.numberOfRarerBuilds && candidate.numberOfBorrowingBuilds)
                statistics.count(StatisticID::AllocationsBuiltApartOnRarerPaths);
        }
        if (candidate.numberOfBuilds)
            statistics.count(StatisticID::StoresFoldedIntoAllocations, candidate.numberOfFoldedStores);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
