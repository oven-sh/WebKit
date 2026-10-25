/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocationPlanner.h"
#include "AOTRegionCloner.h"
#include "AOTSort.h"
#include "AOTStatistics.h"
#include "AOTThreshold.h"
#include "AOTUnspecifiedValuePaths.h"
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

namespace {

struct DuplicationPlan {
    VirtualObjectID object;
    BlockID merge;
    Vector<Edge, 2> cellEdges;
};

} // anonymous namespace

static bool allocatesOnEveryPath(const Candidate& candidate)
{
    return candidate.isAbandoned || (candidate.numberOfBuilds && !candidate.diesVirtual);
}

static void resetAnalysis(AllocationSinking& sinking)
{
    for (uint32_t index = 0; index < sinking.numberOfCandidates(); ++index) {
        Candidate& candidate = sinking.at(VirtualObjectID(index));
        if (candidate.abandonedBy == LimitID::None && !candidate.isLeftOut)
            candidate.isAbandoned = false;
        candidate.isBuiltBeforeBranches = false;
    }
    sinking.decisions() = SinkingDecisions { };
}

static Vector<VirtualObjectID, 4> collectReachableObjects(const SinkingState& state, VirtualObjectID root)
{
    Vector<VirtualObjectID, 4> objects;
    if (!state.find(root))
        return objects;

    objects.append(root);
    for (size_t next = 0; next < objects.size(); ++next) {
        for (const HeldObject& entry : state.find(objects[next])->heldObjects) {
            if (state.find(entry.object))
                objects.appendIfNotContains(entry.object);
        }
    }
    return objects;
}

static Vector<BlockID, 4> collectBlocksEnteredFromOutside(const Function& function, const CFG& cfg, std::span<const BlockID> region)
{
    BitVector isInRegion(function.blocks.size());
    for (BlockID block : region)
        isInRegion.quickSet(block.index());
    Vector<BlockID, 4> entries;
    for (BlockID block : region) {
        bool isEnteredFromOutside = std::ranges::any_of(cfg.predecessors(block), [&](BlockID predecessor) {
            return !isInRegion.quickGet(predecessor.index());
        });
        if (isEnteredFromOutside)
            entries.append(block);
    }
    return entries;
}

Vector<Edge, 2> AllocationSinking::collectCellEdges(VirtualObjectID object, BlockID merge)
{
    std::span<const Edge> incomingEdges = m_analyses.cfg().incomingEdges(merge);
    if (std::ranges::any_of(incomingEdges, &Edge::isHandlerEdge))
        return { };

    Vector<Edge, 2> cellEdges;
    bool hasOtherShape = false;
    for (Edge edge : incomingEdges) {
        const SinkingState* head = headOf(edge.from);
        if (!head)
            continue;

        const Block& source = m_function.at(edge.from);
        prepare(edge.from);
        SinkingState state = *head;
        ObjectInterpreter interpreter(*this, SinkingMode::Analyze, state);
        for (ValueID id : source.values)
            interpreter.execute(id);
        interpreter.executeTerminator(source.terminator);
        OutgoingEdge outgoing = outgoingEdgeOf(m_function, edge);
        interpreter.enterEdge(outgoing, &source.terminator, nullptr);

        const ObjectState* entering = state.isReachable ? state.find(object) : nullptr;
        if (!entering)
            continue;
        // What may be a cell on a way that seldom runs is built there.
        BlockFrequency frequency = m_function.at(edge.from).frequency;
        bool isRarer = frequency != m_function.at(merge).frequency && rarerOf(frequency, m_function.at(merge).frequency) == frequency;
        if (entering->isCell() || (isRarer && entering->hasCell()))
            cellEdges.append(edge);
        else
            hasOtherShape = true;
    }
    if (!hasOtherShape)
        return { };
    return cellEdges;
}

Vector<BlockID> AllocationSinking::collectLiveRange(VirtualObjectID object, BlockID merge)
{
    const SinkingState* headOfMerge = headOf(merge);
    if (!headOfMerge)
        return { };
    Vector<VirtualObjectID, 4> objects = collectReachableObjects(*headOfMerge, object);
    if (objects.isEmpty())
        return { };

    const CFG& cfg = m_analyses.cfg();
    Vector<BlockID> region { merge };
    BitVector isInRegion(m_function.blocks.size());
    isInRegion.quickSet(merge.index());
    uint64_t numberOfValues = 0;
    for (size_t next = 0; next < region.size(); ++next) {
        BlockID block = region[next];
        numberOfValues += m_function.at(block).values.size() + 1;
        if (exceeds(LimitID::MaxDuplicatedTailValues, numberOfValues, at(object).definition))
            return { };

        for (BlockID successor : cfg.successors(block)) {
            if (successor == merge)
                return { };
            const SinkingState* head = headOf(successor);
            bool isLive = head && std::ranges::any_of(objects, [&](VirtualObjectID member) {
                return !!head->find(member);
            });
            if (isLive && !isInRegion.quickSet(successor.index()))
                region.append(successor);
        }
    }

    AOT::sort(region, { }, [&](BlockID block) {
        return cfg.reversePostOrderIndex(block);
    });
    return region;
}

bool AllocationSinking::paysWithoutEdges(VirtualObjectID object, std::span<const Edge> edges)
{
    BitVector allocatedOnEveryPath(m_candidates.size());
    for (size_t index = 0; index < m_candidates.size(); ++index) {
        if (allocatesOnEveryPath(m_candidates[index]))
            allocatedOnEveryPath.quickSet(index);
    }

    resetAnalysis(*this);
    m_ignoredEdges.clear();
    m_ignoredEdges.append(edges);
    analyze();
    m_ignoredEdges.clear();

    const SinkingState* head = headOf(targetOf(m_function, edges.front()).block);
    if (!head)
        return false;
    Vector<VirtualObjectID, 4> objects = collectReachableObjects(*head, object);
    return std::ranges::any_of(objects, [&](VirtualObjectID member) {
        return allocatedOnEveryPath.quickGet(member.index()) && !allocatesOnEveryPath(at(member));
    });
}

void AllocationSinking::duplicate(IRBuilder& builder, std::span<const BlockID> region, std::span<const Edge> cellEdges)
{
    RegionCloner cloner(builder, m_function);
    cloner.cloneBlocks(region, [](ValueID, Value&) { });
    for (Edge edge : cellEdges) {
        const Target& target = targetOf(m_function, edge);
        BlockID clone = cloner.cloneOf(target.block);
        builder.redirectEdge(edge, clone, Arguments(target.arguments));
    }
    cloner.repairSSA(m_analyses);
}

// A callee that hands over the parts of its result hands over the object itself on some of its ways. The caller puts the parts together only to have one value where the two meet.
// A way that seldom runs and makes the object counts the same, where it is a read or a test behind the meeting that wants the cell.
bool AllocationSinking::meetsObjectMadeOnAnotherWay(const Candidate& join) const
{
    if (join.isLeftOut || m_function.at(join.block).frequency != BlockFrequency::Normal)
        return false;
    bool isMadeOnEveryPath = std::ranges::any_of(m_candidates, [&](const Candidate& candidate) {
        return candidate.family == join.family && allocatesOnEveryPath(candidate);
    });
    if (!isMadeOnEveryPath)
        return false;
    std::optional<Opcode> cause = join.firstCause ? join.firstCause->opcode : std::nullopt;
    bool isBuiltForRead = cause && (interpretedOf(*cause) == Interpretation::Read || interpretedOf(*cause) == Interpretation::Test);
    size_t parameterIndex = m_function.at(join.block).parameters.find(join.definition);
    return std::ranges::any_of(m_analyses.cfg().incomingEdges(join.block), [&](Edge edge) {
        ValueID incoming = valuePassedOn(m_function, edge, parameterIndex);
        if (!incoming)
            return false;
        return (isBuiltForRead && m_function.at(edge.from).frequency != BlockFrequency::Normal) || m_function.at(sameObjectThroughCasts(m_function, incoming)).opcode == Opcode::ResultAt;
    });
}

// Whether the list fits the storage that its readers expect is asked where it is made, of each element that may be no such number. Behind that there are two lists, and of each the answer is known:
// the one that is made of numbers, and one for every value, on a way that seldom runs.
static void testElementsOfList(IRBuilder& builder, const Program& program, Function& function, ProgramPoint point)
{
    Value list = function.at(function.at(point.block).values[point.index]);
    TypeID typeOfList = list.type;
    Type numbers = typeOfNumbersIn(arrayStorageAtAllocation(program, program.at(typeOfList)));
    IRBuilder::Expansion expansion = builder.beginExpansion(point);
    BlockID holdsOtherValue;

    Vector<ValueID, 4> elements;
    for (ValueID element : list.elementsOperands()) {
        if (program.typeOf(function, element).isSubsetOf(numbers, program)) {
            elements.append(element);
            continue;
        }
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(element, numbers), OperandIndex::TypeTest::value, BlockFrequency::Normal, BlockFrequency::Rare);
        elements.append(branch.provenOnTaken);
        if (!holdsOtherValue)
            holdsOtherValue = builder.addBlock(BlockFrequency::Rare);
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(holdsOtherValue);
        builder.moveToEndOf(branch.taken);
    }
    Value listOfNumbers = list;
    listOfNumbers.setContents(builder.types().intern(program.at(list.contents()).intersectWith(numbers, builder.types())));
    listOfNumbers.setElementsOperands(elements.span());
    builder.endExpansion(expansion, builder.append(makeAssumeType(builder.append(WTF::move(listOfNumbers)), typeOfList)));

    if (!holdsOtherValue)
        return;
    builder.moveToEndOf(holdsOtherValue);
    list.setContents(TypeTable::anything);
    builder.emitJump(expansion.continuation, { builder.append(makeAssumeType(builder.append(WTF::move(list)), typeOfList)) });
}

bool AllocationSinking::testElementsOfLists(IRBuilder& builder)
{
    AOT_THRESHOLD(size_t, maxBlockVisitsToFindUnspecifiedElements, 65536);
    Vector<ProgramPoint, 2> points;
    Vector<ValueID, 2> lists;
    BitVector isListToTest;
    size_t numberOfElements = 0;
    for (ValueID literal : m_testedListsThatMayNotFit) {
        const Value& list = m_function.at(literal);
        ValueLocation location = m_analyses.useLists().locationOf(literal);
        if (list.opcode != Opcode::NewArray || location.placement != ValuePlacement::InBlock)
            continue;
        if (list.elementsOperands().empty() || !list.holes().empty() || list.capacityOperand())
            continue;
        // Only an allocation is a guard of what it makes.
        bool isGuard = std::ranges::any_of(m_analyses.useLists().usesOf(literal), [&](const Use& use) {
            return use.kind == Use::Kind::Operand && m_function.at(use.user).operandKind(use.operandIndex) == OperandKind::Guard;
        });
        if (isGuard)
            continue;
        lists.append(literal);
        isListToTest.set(literal.index());
        numberOfElements += list.elementsOperands().size();
    }
    if (lists.isEmpty())
        return false;
    // A list may hold an Unspecified value on a way that reaches it, and a test would look at that value.
    UsesOfUnspecifiedValues found = findUsesOfUnspecifiedValues(m_analyses, numberOfElements + 1, OtherFunctions::AreAtRest, maxBlockVisitsToFindUnspecifiedElements, &isListToTest);
    for (ValueID literal : lists) {
        bool holdsUnspecifiedValue = !found.wasEveryPathFollowed || std::ranges::any_of(found.uses, [&](const UseOfUnspecifiedValue& use) {
            return use.user == literal;
        });
        if (holdsUnspecifiedValue)
            continue;
        ValueLocation location = m_analyses.useLists().locationOf(literal);
        points.append(ProgramPoint { location.block, location.index });
    }
    AOT::sort(points, [](const ProgramPoint& a, const ProgramPoint& b) {
        return a.block != b.block ? a.block > b.block : a.index > b.index;
    });
    for (ProgramPoint point : points)
        testElementsOfList(builder, m_program, builder.function(), point);
    return !points.isEmpty();
}

// A block that only hands its parameter on to the next block where values meet hides from that one which way brings the parts and which the whole.
bool AllocationSinking::handOnToNextJoin(IRBuilder& builder, const Candidate& join)
{
    const Block& block = m_function.at(join.block);
    if (!block.values.isEmpty() || block.parameters.size() != 1 || block.terminator.kind != TerminatorKind::Jump || block.terminator.next().block == join.block)
        return false;
    bool isOnlyHandedOn = std::ranges::all_of(m_analyses.useLists().usesOf(join.definition), [&](const Use& use) {
        return use.kind == Use::Kind::EdgeArgument && use.block == join.block;
    });
    Vector<Edge, 4> edges(m_analyses.cfg().incomingEdges(join.block));
    bool takesArguments = std::ranges::all_of(edges, [&](Edge edge) {
        return !edge.isHandlerEdge() && numberOfExplicitArguments(m_function, edge) == 1;
    });
    if (!isOnlyHandedOn || !takesArguments)
        return false;

    Target next = block.terminator.next();
    for (Edge edge : edges) {
        ValueID incoming = valuePassedOn(m_function, edge, 0);
        Vector<ValueID, 4> arguments(next.arguments.size(), [&](size_t index) {
            return next.arguments[index] == join.definition ? incoming : next.arguments[index];
        });
        builder.redirectEdge(edge, next.block, Arguments(arguments.span()));
    }
    builder.emptyUnreachableBlocks();
    return true;
}

bool AllocationSinking::duplicateLiveRange(IRBuilder& builder)
{
    Vector<DuplicationPlan, 2> plans;
    for (uint32_t index = 0; index < m_candidates.size(); ++index) {
        const Candidate& candidate = m_candidates[index];
        if (candidate.abandonedBy != LimitID::None)
            continue;
        if (candidate.isJoin ? meetsObjectMadeOnAnotherWay(candidate) : !!candidate.mergeBlock)
            plans.append(DuplicationPlan { VirtualObjectID(index), candidate.isJoin ? candidate.block : candidate.mergeBlock, { } });
    }
    if (plans.isEmpty() || hasSpentItsWork())
        return false;
    for (const DuplicationPlan& plan : plans) {
        if (at(plan.object).isJoin && handOnToNextJoin(builder, at(plan.object)))
            return true;
    }

    Vector<Candidate> savedCandidates = m_candidates;
    SinkingDecisions savedDecisions = m_decisions;
    auto restoreAnalysis = [&] {
        m_candidates = savedCandidates;
        m_decisions = savedDecisions;
    };
    auto collectClonableRange = [&](const DuplicationPlan& plan) {
        Vector<BlockID> region = collectLiveRange(plan.object, plan.merge);
        if (region.isEmpty())
            return region;
        // A way that seldom runs leaves the range where it builds the object, and may come back into the range further down.
        Vector<BlockID, 4> entries = collectBlocksEnteredFromOutside(m_function, m_analyses.cfg(), region.span());
        if (RegionCloner::checkCanDuplicate(m_analyses, region.span(), entries.span()) != CloneCheck::CanClone)
            region.clear();
        return region;
    };

    resetAnalysis(*this);
    startWorkOver();
    solve();
    for (DuplicationPlan& plan : plans) {
        plan.cellEdges = collectCellEdges(plan.object, plan.merge);
        if (!plan.cellEdges.isEmpty() && collectClonableRange(plan).isEmpty())
            plan.cellEdges.clear();
    }

    uint64_t numberOfTrials = 0;
    for (const DuplicationPlan& plan : plans) {
        if (plan.cellEdges.isEmpty())
            continue;
        if (exceeds(LimitID::MaxSinkingRounds, ++numberOfTrials, at(plan.object).definition))
            continue;
        restoreAnalysis();
        if (!paysWithoutEdges(plan.object, plan.cellEdges.span()))
            continue;
        Vector<BlockID> region = collectClonableRange(plan);
        if (region.isEmpty())
            continue;

        duplicate(builder, region.span(), plan.cellEdges.span());
        context().build.statistics().count(StatisticID::TailsDuplicatedForSinking);
        return true;
    }

    restoreAnalysis();
    startWorkOver();
    m_decisions.takeDidChange();
    analyzeUntilNothingChanges();
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
