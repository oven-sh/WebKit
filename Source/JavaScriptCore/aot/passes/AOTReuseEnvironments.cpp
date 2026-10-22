/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReuseEnvironments.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBorrowedParameters.h"
#include "AOTBuildContext.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTLargeVectors.h"
#include "AOTLiveness.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class HolderAction : uint8_t { Disqualify, Ignore, Mark };

constexpr OperandUses usesLeavingFunction { OperandUse::Retains, OperandUse::Returns };

struct EscapeMark {
    ProgramPoint point;
    ValueID environment;
    LayoutID layout;
};

struct ReleasePoint {
    std::optional<Edge> edge;
    ProgramPoint point;
    ValueID environment;
    LayoutID layout;
};

struct EnvironmentSite {
    LayoutID layout;
    BlockID block;

    friend auto operator<=>(const EnvironmentSite&, const EnvironmentSite&) = default;
};

// All that the pass asks the escape analysis of a function.
struct OwnedValues {
    // See isOwned().
    BitVector owned;
    // Those of them that may be an object that the function hands back among several results.
    BitVector handedBack;
};

struct FunctionPlan {
    Vector<EscapeMark> marks;
    Vector<ReleasePoint> releases;
    Vector<LayoutID> disqualifiedLayouts;
    Vector<EnvironmentSite> sites;
    // Which of its several results are environments that the function has made itself, whichever way it returns.
    BitVector resultsMadeHere;
    // No body and no summary changes until the functions are rewritten. So the first step that needs the analysis leaves what it says for the steps behind it.
    std::optional<OwnedValues> ownedValues;
};

class CandidateTypes {
public:
    CandidateTypes(const Program& program, const BitVector& candidateLayouts)
        : m_environments(program.types.size())
    {
        for (unsigned index = 0; index < m_environments.size(); ++index) {
            LayoutID environment = program.at(TypeID(index)).environment;
            if (environment && candidateLayouts.get(environment.index()))
                m_environments[index] = environment;
        }
    }

    LayoutID environmentOf(TypeID type) const { return type ? m_environments[type.index()] : LayoutID(); }
    LayoutID environmentOf(const Function& function, ValueID id) const { return environmentOf(function.at(id).type); }

    bool hasCandidateValue(const Program& program, FunctionID id) const
    {
        BodyIndex* index = program.bodyIndex();
        std::optional<NamesInBody> names = index ? index->namesIn(program, id) : std::nullopt;
        bool mayHaveOne = !names || std::ranges::any_of(names->types, [&](TypeID type) {
            return !!environmentOf(type);
        });
        return mayHaveOne && program.at(id).values.containsIf([&](const Value& value) {
            return !!environmentOf(value.type);
        });
    }

private:
    Vector<LayoutID> m_environments;
};

} // anonymous namespace

static AnalysisContext contextOf(const AnalysisContext& programContext, FunctionID id)
{
    return { programContext.build, programContext.program, programContext.flow, programContext.effects, id };
}

static bool isAlwaysCell(const Program& program, const Function& function, ValueID id)
{
    return !program.typeOf(function, id).primitives;
}

// No operation reads the hidden field that holds the environment. The closures that the object makes on demand do: they are cells of the heap wherever the object lies.
static bool isEnvironmentOfDeferredClosures(const Program& program, const Value& user, uint32_t operandIndex)
{
    if (user.opcode != Opcode::NewObject)
        return false;
    std::optional<uint32_t> field = program.at(user.layout()).flow.closureEnvironmentField;
    std::optional<uint32_t> position = user.variadicOperandIndex(operandIndex);
    return field && position && *position < user.fields().size() && user.fields()[*position] == *field;
}

static bool isOwned(const Function& function, const EscapeAnalysis& escape, ValueID id)
{
    if (escape.gaveUp())
        return false;
    PointsToSet pointsTo = escape.pointsTo(id);
    if (pointsTo.mayBeUntracked || pointsTo.objects.isEmpty())
        return false;
    for (ObjectID object : pointsTo.objects) {
        const TrackedObject& tracked = escape.at(object);
        if (tracked.kind != ObjectKind::Allocation || tracked.state == EscapeState::Unknown)
            return false;
        const Value& definition = function.at(tracked.definition);
        if (definition.opcode != Opcode::NewObject || definition.storage() != AllocationStorage::Heap)
            return false;
    }
    return true;
}

static OwnedValues findOwnedValues(const Function& function, const EscapeAnalysis& escape)
{
    OwnedValues values;
    if (escape.gaveUp())
        return values;

    BitVector objectsHandedBack;
    for (const Value& value : function.values) {
        if (value.opcode != Opcode::Results)
            continue;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            objectsHandedBack.merge(escape.pointsTo(operand).objects.bits());
        });
    }
    for (unsigned index = 0; index < function.values.size(); ++index) {
        ValueID id(index);
        if (!isOwned(function, escape, id))
            continue;
        values.owned.set(index);
        for (ObjectID object : escape.pointsTo(id).objects) {
            if (objectsHandedBack.get(object.index()))
                values.handedBack.set(index);
        }
    }
    return values;
}

static const OwnedValues& ownedValuesOf(FunctionPlan& plan, FunctionAnalyses& analyses)
{
    if (plan.ownedValues)
        return *plan.ownedValues;
    // A value is owned only if it is an object that the function has allocated on the heap.
    bool allocatesOnHeap = analyses.function().values.containsIf([](const Value& value) {
        return value.opcode == Opcode::NewObject && value.storage() == AllocationStorage::Heap;
    });
    plan.ownedValues = allocatesOnHeap ? findOwnedValues(analyses.function(), analyses.escape()) : OwnedValues { };
    return *plan.ownedValues;
}

static void forEachLayoutWithSites(const FunctionPlan& plan, NOESCAPE const Invocable<void(LayoutID, std::span<const EnvironmentSite>)> auto& functor)
{
    std::span<const EnvironmentSite> sites = plan.sites.span();
    while (!sites.empty()) {
        LayoutID layout = sites.front().layout;
        size_t count = std::ranges::find_if(sites, [&](const EnvironmentSite& site) {
            return site.layout != layout;
        }) - sites.begin();
        functor(layout, sites.first(count));
        sites = sites.subspan(count);
    }
}

static bool isReusableAllocation(const Program& program, const FrameAllocation& frameAllocation, const Value& value)
{
    if (value.opcode != Opcode::NewObject || value.storage() != AllocationStorage::Heap)
        return false;
    LayoutID layout = value.layout();
    return program.at(layout).kind == LayoutKind::Environment && !frameAllocation.mayBeInFrame(layout);
}

// The next turn of a loop can take the cell that this turn has done with, and so can the next call of the function.
static void findSites(const Program& program, const FrameAllocation& frameAllocation, const BitVector& environmentLayouts, FunctionID id, FunctionPlan& plan)
{
    Vector<EnvironmentSite>& sites = plan.sites;
    auto isEnvironmentLayout = [&](NameKind kind, uint32_t index) {
        return kind == NameKind::Layout && environmentLayouts.get(index);
    };
    forEachListedValueNaming(program, id, { Opcode::NewObject }, isEnvironmentLayout, [&](BlockID block, ValueID, const Value& value) {
        if (isReusableAllocation(program, frameAllocation, value))
            sites.append({ value.layout(), block });
    });
    if (sites.isEmpty())
        return;

    CFG cfg(program.functions[id.index()]);
    sites.removeAllMatching([&](const EnvironmentSite& site) {
        return !cfg.isReachable(site.block);
    });
    std::ranges::sort(sites);
    removeRepeatedElements(sites);
}

static BitVector findCandidateLayouts(const Program& program, const FrameAllocation& frameAllocation, const ParallelLoops& loops, Vector<FunctionPlan>& plans)
{
    BitVector environmentLayouts(program.layouts.size());
    for (uint32_t index : findIndicesInParallel(loops, program.layouts.size(), [&](unsigned index) {
        return program.layouts[index].kind == LayoutKind::Environment;
    }))
        environmentLayouts.quickSet(index);
    forEachIndexInParallel(loops, plans.size(), [&](unsigned index) {
        findSites(program, frameAllocation, environmentLayouts, FunctionID(index), plans[index]);
    });

    BitVector candidateLayouts;
    for (const FunctionPlan& plan : plans) {
        for (const EnvironmentSite& site : plan.sites)
            candidateLayouts.set(site.layout.index());
    }
    return candidateLayouts;
}

namespace {

class HolderClassifier {
public:
    HolderClassifier(const AnalysisContext& context, const EffectContext& effects, const CandidateTypes& types, FunctionPlan& plan)
        : m_context(context)
        , m_effects(effects)
        , m_types(types)
        , m_function(context.program.at(context.id))
        , m_mayChange(context.build.isEnabled(PassID::ReuseEnvironments, context.id))
        , m_plan(plan)
    {
    }

    void run();

private:
    LayoutID environmentOf(ValueID id) const { return m_types.environmentOf(m_function, id); }

    void classifyValues(BlockID);
    void classifyTerminator(const Block&);
    HolderAction classifyOperand(ValueID userID, uint32_t operandIndex, LayoutID) const;
    HolderAction classifyDirectCallArgument(const Value& call, uint32_t operandIndex, LayoutID) const;
    void markAcrossSuspensions();
    void apply(HolderAction, ProgramPoint, ValueID environment, LayoutID);
    void removeRepeatedEntries();

    const AnalysisContext& m_context;
    const EffectContext& m_effects;
    const CandidateTypes& m_types;
    const Function& m_function;
    bool m_mayChange;
    FunctionPlan& m_plan;
    Vector<BlockID> m_blocksWithSuspensions;
};

void HolderClassifier::apply(HolderAction action, ProgramPoint point, ValueID environment, LayoutID layout)
{
    if (action == HolderAction::Mark && (!m_mayChange || !isAlwaysCell(m_context.program, m_function, environment)))
        action = HolderAction::Disqualify;

    switch (action) {
    case HolderAction::Disqualify:
        m_plan.disqualifiedLayouts.append(layout);
        return;
    case HolderAction::Ignore:
        return;
    case HolderAction::Mark:
        m_plan.marks.append({ point, environment, layout });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void HolderClassifier::run()
{
    for (unsigned blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        classifyValues(BlockID(blockIndex));
        classifyTerminator(m_function.blocks[blockIndex]);
    }
    markAcrossSuspensions();
    removeRepeatedEntries();
}

void HolderClassifier::classifyValues(BlockID blockID)
{
    const Block& block = m_function.at(blockID);
    bool hasSuspension = false;
    for (uint32_t index = 0; index < block.values.size(); ++index) {
        ValueID userID = block.values[index];
        const Value& user = m_function.at(userID);
        if (user.opcode == Opcode::Unspecified) {
            if (LayoutID layout = environmentOf(userID))
                m_plan.disqualifiedLayouts.append(layout);
        }
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (LayoutID layout = environmentOf(operand))
                apply(classifyOperand(userID, operandIndex, layout), { blockID, index }, operand, layout);
        });
        hasSuspension |= maySuspend(user.opcode);
    }
    if (hasSuspension)
        m_blocksWithSuspensions.append(blockID);
}

HolderAction HolderClassifier::classifyOperand(ValueID userID, uint32_t operandIndex, LayoutID layout) const
{
    const Value& user = m_function.at(userID);
    if (user.opcode == Opcode::CallDirect)
        return classifyDirectCallArgument(user, operandIndex, layout);
    // One of several results goes to the caller under its declared type, and the caller's uses of it are classified like any other. What holds the results in between is followed like the environment itself.
    if (user.opcode == Opcode::Results)
        return operandIndex < m_function.results.size() && m_types.environmentOf(m_function.results[operandIndex].type) == layout ? HolderAction::Ignore : HolderAction::Disqualify;
    if (isEnvironmentOfDeferredClosures(m_context.program, user, operandIndex))
        return HolderAction::Mark;

    OperandSummary summary = summarizeOperand(m_effects, m_function, user, operandIndex);
    if (summary.uses.contains(OperandUse::PassesToUserCode))
        return HolderAction::Mark;
    // A closure that is lent may be copied to the heap by the stub of its call, and the copy holds what the closure holds.
    if (summary.uses.contains(OperandUse::Retains))
        return summary.retainedIn == RetainedIn::Result && isFrameCell(m_function, userID) && !(user.opcode == Opcode::NewClosure && user.isLent()) ? HolderAction::Ignore : HolderAction::Mark;
    if (summary.uses.contains(OperandUse::Returns) && environmentOf(userID) != layout)
        return HolderAction::Disqualify;
    return HolderAction::Ignore;
}

HolderAction HolderClassifier::classifyDirectCallArgument(const Value& call, uint32_t operandIndex, LayoutID layout) const
{
    const Function& callee = m_context.program.at(call.function());
    std::optional<uint32_t> entryParameter = entryParameterOf(callee, call, operandIndex);
    if (!callee.hasBody() || !entryParameter)
        return HolderAction::Disqualify;
    const DeclaredType* declared = callee.declaredEntryParameter(*entryParameter);
    if (!declared)
        return HolderAction::Disqualify;
    return m_types.environmentOf(declared->type) == layout ? HolderAction::Ignore : HolderAction::Disqualify;
}

void HolderClassifier::classifyTerminator(const Block& block)
{
    const Terminator& terminator = block.terminator;
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        LayoutID layout = environmentOf(operand);
        if (layout && summarizeOperand(m_effects, m_function, terminator, operandIndex).uses.containsAny(usesLeavingFunction))
            m_plan.disqualifiedLayouts.append(layout);
    });
    if (block.subject) {
        if (LayoutID layout = environmentOf(block.subject))
            m_plan.disqualifiedLayouts.append(layout);
    }
    block.forEachTarget([&](const Target& target) {
        auto& parameters = m_function.at(target.block).parameters;
        for (unsigned argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
            LayoutID layout = environmentOf(target.arguments[argumentIndex]);
            if (layout && (argumentIndex >= parameters.size() || environmentOf(parameters[argumentIndex]) != layout))
                m_plan.disqualifiedLayouts.append(layout);
        }
    });
}

void HolderClassifier::markAcrossSuspensions()
{
    if (m_blocksWithSuspensions.isEmpty())
        return;

    BitVector candidateValues(m_function.values.size());
    for (unsigned index = 0; index < m_function.values.size(); ++index) {
        if (environmentOf(ValueID(index)))
            candidateValues.quickSet(index);
    }
    FunctionAnalyses analyses(m_context, m_function);
    Liveness liveness(m_function, analyses.cfg(), analyses.useLists(), LivenessOptions { .trackedValues = &candidateValues });

    for (BlockID blockID : m_blocksWithSuspensions) {
        if (!analyses.cfg().isReachable(blockID))
            continue;
        auto& values = m_function.at(blockID).values;
        Liveness::LocalCalc calc(liveness, blockID);
        for (uint32_t index = values.size(); index--;) {
            ValueID suspension = values[index];
            if (maySuspend(m_function.at(suspension).opcode)) {
                calc.forEachLive([&](ValueID live) {
                    if (live != suspension && !ownedValuesOf(m_plan, analyses).owned.get(live.index()))
                        apply(HolderAction::Mark, { blockID, index }, live, environmentOf(live));
                });
            }
            calc.execute(index);
        }
    }
}

void HolderClassifier::removeRepeatedEntries()
{
    std::ranges::sort(m_plan.disqualifiedLayouts);
    removeRepeatedElements(m_plan.disqualifiedLayouts);

    std::ranges::sort(m_plan.marks, { }, [](const EscapeMark& mark) {
        return std::tuple { mark.point.block, mark.environment, mark.point.index };
    });
    removeRepeatedElements(m_plan.marks, [](const EscapeMark& a, const EscapeMark& b) {
        return a.point.block == b.point.block && a.environment == b.environment;
    });
}

} // anonymous namespace

static void classifyHolders(const AnalysisContext& context, const EffectContext& effects, const CandidateTypes& types, FunctionPlan& plan)
{
    if (hasBody(context.program, context.id) && types.hasCandidateValue(context.program, context.id))
        HolderClassifier(context, effects, types, plan).run();
}

// Several results are held by a value that has no type of its own: by Results until the function returns, and by the call until the last ResultAt.
static bool holdsAmongSeveralResults(const Program& program, const CandidateTypes& types, const Function& function, const Value& value, LayoutID layout)
{
    switch (value.opcode) {
    case Opcode::Results: {
        bool holds = false;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            holds |= types.environmentOf(function, operand) == layout;
        });
        return holds;
    }
    case Opcode::CallDirect:
    case Opcode::CallTable: {
        auto& results = program.at(value.function()).results;
        return results.size() > 1 && results.containsIf([&](const auto& result) {
            return types.environmentOf(result.type) == layout;
        });
    }
    default:
        return false;
    }
}

// An environment that a function has made and hands back among its results is the caller's from there on: nobody else has it, but for those who have marked it.
// A function that returns what a call of another function answered says nothing about who made it.
static void findResultsMadeHere(const AnalysisContext& context, const CandidateTypes& types, FunctionPlan& plan)
{
    RecordFacts facts = recordFactsOf(context.program, context.id);
    if (!facts.hasBody || facts.numberOfResults < 2)
        return;
    const Function& function = context.program.at(context.id);
    BitVector madeHere;
    for (unsigned index = 0; index < function.results.size(); ++index) {
        if (types.environmentOf(function.results[index].type))
            madeHere.set(index);
    }
    if (madeHere.isEmpty())
        return;

    FunctionAnalyses analyses(context, function);
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        if (!analyses.cfg().isReachable(BlockID(blockIndex)))
            continue;
        const Terminator& terminator = function.blocks[blockIndex].terminator;
        if (terminator.kind == TerminatorKind::ReturnCall)
            return;
        if (terminator.kind != TerminatorKind::Return)
            continue;
        ValueID returned = terminator.valueOperand();
        if (!returned || function.at(returned).opcode != Opcode::Results)
            return;
        const Value& results = function.at(returned);
        if (results.numberOfVariadicOperands() != function.results.size())
            return;
        results.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (madeHere.get(operandIndex) && !(isAlwaysCell(context.program, function, operand) && ownedValuesOf(plan, analyses).owned.get(operand.index())))
                madeHere.clear(operandIndex);
        });
    }
    plan.resultsMadeHere = WTF::move(madeHere);
}

static bool isMadeByCallee(const Function& function, std::span<const FunctionPlan> plans, const Value& value)
{
    if (value.opcode != Opcode::ResultAt)
        return false;
    const Value& call = function.at(value.resultsOperand());
    return call.opcode == Opcode::CallDirect && plans[call.function().index()].resultsMadeHere.get(value.index());
}

// Where a caller takes an environment that the callee has made out of the results, it has one to let go of, as where it makes one.
static void findSitesOfResults(const Program& program, const CandidateTypes& types, const BitVector& candidateLayouts, FunctionID id, std::span<FunctionPlan> plans)
{
    const Function& function = program.at(id);
    Vector<EnvironmentSite>& sites = plans[id.index()].sites;
    size_t numberOfSitesBefore = sites.size();
    forEachListedValue(program, id, { Opcode::ResultAt }, [&](BlockID block, uint32_t, ValueID, const Value& value) {
        LayoutID layout = types.environmentOf(value.type);
        if (layout && candidateLayouts.get(layout.index()) && isMadeByCallee(function, plans, value))
            sites.append({ layout, block });
    });
    if (sites.size() == numberOfSitesBefore)
        return;

    CFG cfg(function);
    sites.removeAllMatching([&](const EnvironmentSite& site) {
        return !cfg.isReachable(site.block);
    });
    std::ranges::sort(sites);
    removeRepeatedElements(sites);
}

namespace {

struct TrackedValues {
    bool isAnyLive(const Liveness::LocalCalc& calc) const
    {
        return std::ranges::any_of(list, [&](ValueID id) {
            return calc.isLive(id);
        });
    }

    Vector<ValueID> list;
    BitVector bits;
    BitVector definingBlocks;
};

class ReleaseFinder {
public:
    ReleaseFinder(const AnalysisContext& context, const CandidateTypes& types, const Function& function, std::span<const FunctionPlan> plans, FunctionPlan& plan)
        : m_types(types)
        , m_function(function)
        , m_plans(plans)
        , m_plan(plan)
        , m_analyses(context, function)
        , m_framePointers(function)
    {
    }

    void findReleasePoints(LayoutID);

private:
    bool isOwnedEnvironment(ValueID, LayoutID);
    TrackedValues computeTrackedValues(LayoutID);
    std::optional<ValueID> findReleasableEnvironment(const TrackedValues&, const Liveness::LocalCalc&, LayoutID);
    void findReleasesInBlock(BlockID, LayoutID, const TrackedValues&, const Liveness&);
    void findReleasesOnEdges(BlockID, LayoutID, const TrackedValues&, const Liveness&, const Liveness::LocalCalc&);

    const CandidateTypes& m_types;
    const Function& m_function;
    std::span<const FunctionPlan> m_plans;
    FunctionPlan& m_plan;
    FunctionAnalyses m_analyses;
    FramePointers m_framePointers;
};

bool ReleaseFinder::isOwnedEnvironment(ValueID id, LayoutID layout)
{
    if (m_types.environmentOf(m_function, id) != layout || !isAlwaysCell(m_analyses.program(), m_function, id))
        return false;
    if (isMadeByCallee(m_function, m_plans, m_function.at(id)))
        return true;
    // This function has done with an environment that it hands back among its results, and the caller has not: it is the caller's to let go of.
    const OwnedValues& values = ownedValuesOf(m_plan, m_analyses);
    return values.owned.get(id.index()) && !values.handedBack.get(id.index());
}

TrackedValues ReleaseFinder::computeTrackedValues(LayoutID layout)
{
    bool tracksFramePointers = std::ranges::any_of(m_framePointers.allocations(), [&](ValueID frameCell) {
        bool refersToLayout = false;
        m_function.at(frameCell).forEachOperand([&](ValueID operand, uint32_t) {
            refersToLayout |= m_types.environmentOf(m_function, operand) == layout;
        });
        return refersToLayout;
    });

    const UseLists& useLists = m_analyses.useLists();
    TrackedValues tracked;
    tracked.bits.ensureSize(m_function.values.size());
    tracked.definingBlocks.ensureSize(m_function.blocks.size());
    for (unsigned index = 0; index < m_function.values.size(); ++index) {
        ValueID id(index);
        bool isFollowed = m_types.environmentOf(m_function, id) == layout
            || (tracksFramePointers && m_framePointers.contains(id))
            || holdsAmongSeveralResults(m_analyses.program(), m_types, m_function, m_function.at(id), layout);
        if (!isFollowed)
            continue;
        ValueLocation location = useLists.locationOf(id);
        if (location.placement == ValuePlacement::Detached)
            continue;
        tracked.list.append(id);
        tracked.bits.quickSet(index);
        tracked.definingBlocks.quickSet(location.block.index());
    }
    return tracked;
}

std::optional<ValueID> ReleaseFinder::findReleasableEnvironment(const TrackedValues& tracked, const Liveness::LocalCalc& calc, LayoutID layout)
{
    for (ValueID live : tracked.list) {
        if (!calc.isLive(live))
            continue;
        if (isOwnedEnvironment(live, layout))
            return live;
        if (!std::ranges::binary_search(m_framePointers.allocations(), live))
            continue;
        std::optional<ValueID> ownedOperand;
        m_function.at(live).forEachOperand([&](ValueID operand, uint32_t) {
            if (!ownedOperand && isOwnedEnvironment(operand, layout))
                ownedOperand = operand;
        });
        if (ownedOperand)
            return ownedOperand;
    }
    return std::nullopt;
}

void ReleaseFinder::findReleasesOnEdges(BlockID blockID, LayoutID layout, const TrackedValues& tracked, const Liveness& liveness, const Liveness::LocalCalc& calc)
{
    auto& targets = m_function.at(blockID).terminator.targets;
    Vector<Edge, 2> edges;
    for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        BlockID successor = targets[targetIndex].block;
        if (!liveness.liveAtHead(successor).empty())
            continue;
        bool hasTrackedParameter = std::ranges::any_of(m_function.at(successor).parameters, [&](ValueID parameter) {
            return tracked.bits.quickGet(parameter.index());
        });
        if (!hasTrackedParameter)
            edges.append(Edge { blockID, targetIndex });
    }
    if (edges.isEmpty())
        return;

    std::optional<ValueID> environment = findReleasableEnvironment(tracked, calc, layout);
    if (!environment)
        return;
    for (Edge edge : edges)
        m_plan.releases.append({ edge, { }, *environment, layout });
}

void ReleaseFinder::findReleasesInBlock(BlockID blockID, LayoutID layout, const TrackedValues& tracked, const Liveness& liveness)
{
    const Block& block = m_function.at(blockID);
    Liveness::LocalCalc calc(liveness, blockID);
    bool isLiveAfter = tracked.isAnyLive(calc);
    if (isLiveAfter)
        findReleasesOnEdges(blockID, layout, tracked, liveness, calc);

    for (uint32_t index = block.values.size(); index--;) {
        calc.execute(index);
        bool isLiveBefore = tracked.isAnyLive(calc);
        bool isLastUse = isLiveBefore && !isLiveAfter;
        isLiveAfter = isLiveBefore;
        if (!isLastUse)
            continue;
        // Nothing stands between a tail call and the end of the block, and the callee still has the environment.
        if (block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.callOperand() == block.values[index])
            continue;
        if (std::optional<ValueID> environment = findReleasableEnvironment(tracked, calc, layout))
            m_plan.releases.append({ std::nullopt, { blockID, index + 1 }, *environment, layout });
    }
}

void ReleaseFinder::findReleasePoints(LayoutID layout)
{
    TrackedValues tracked = computeTrackedValues(layout);
    Liveness liveness(m_function, m_analyses.cfg(), m_analyses.useLists(), LivenessOptions { .trackedValues = &tracked.bits });
    for (unsigned blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        BlockID blockID(blockIndex);
        if (!liveness.liveAtHead(blockID).empty() || tracked.definingBlocks.quickGet(blockIndex))
            findReleasesInBlock(blockID, layout, tracked, liveness);
    }
}

} // anonymous namespace

static void findReleasePoints(const AnalysisContext& context, const CandidateTypes& types, const BitVector& candidateLayouts, std::span<const FunctionPlan> plans, FunctionPlan& plan)
{
    ReleaseFinder finder(context, types, context.program.at(context.id), plans, plan);
    forEachLayoutWithSites(plan, [&](LayoutID layout, std::span<const EnvironmentSite>) {
        if (candidateLayouts.get(layout.index()))
            finder.findReleasePoints(layout);
    });
}

static bool reachesUnmarkedRelease(const Function& function, const FunctionPlan& plan, LayoutID layout, std::span<const EnvironmentSite> sites)
{
    BitVector blocksWithReleases(function.blocks.size());
    for (auto& release : plan.releases) {
        if (release.layout == layout)
            blocksWithReleases.quickSet((release.edge ? release.edge->from : release.point.block).index());
    }
    if (blocksWithReleases.isEmpty())
        return false;
    BitVector blocksWithMarks(function.blocks.size());
    for (auto& mark : plan.marks) {
        if (mark.layout == layout)
            blocksWithMarks.quickSet(mark.point.block.index());
    }

    BitVector seen(function.blocks.size());
    Vector<BlockID, 16> worklist;
    for (const EnvironmentSite& site : sites) {
        if (!seen.quickSet(site.block.index()))
            worklist.append(site.block);
    }
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        if (blocksWithMarks.quickGet(block.index()))
            continue;
        if (blocksWithReleases.quickGet(block.index()))
            return true;
        function.at(block).forEachTarget([&](const Target& target) {
            if (!seen.quickSet(target.block.index()))
                worklist.append(target.block);
        });
    }
    return false;
}

static Vector<LayoutID> findProfitableLayouts(const Function& function, const FunctionPlan& plan)
{
    Vector<LayoutID> profitableLayouts;
    forEachLayoutWithSites(plan, [&](LayoutID layout, std::span<const EnvironmentSite> sites) {
        if (reachesUnmarkedRelease(function, plan, layout, sites))
            profitableLayouts.append(layout);
    });
    return profitableLayouts;
}

static Origin originAt(const Function& function, ProgramPoint point)
{
    const Block& block = function.at(point.block);
    if (point.index < block.values.size())
        return function.at(block.values[point.index]).origin();
    if (!block.values.isEmpty())
        return function.at(block.values.last()).origin();
    return block.terminator.origin();
}

static void insertOperation(IRBuilder& builder, ProgramPoint point, Opcode opcode, ValueID environment)
{
    builder.setOrigin(originAt(builder.function(), point));
    builder.insertBefore(point, IRBuilder::make(opcode, { environment }));
}

static void resolveEdgeReleases(IRBuilder& builder, Vector<ReleasePoint>& releases)
{
    Vector<ReleasePoint*> edgeReleases;
    for (ReleasePoint& release : releases) {
        if (release.edge)
            edgeReleases.append(&release);
    }
    if (edgeReleases.isEmpty())
        return;
    std::ranges::sort(edgeReleases, { }, [](const ReleasePoint* release) {
        return std::pair { release->edge->from, release->edge->targetIndex };
    });

    CFG cfg(builder.function());
    std::optional<Edge> previousEdge;
    BlockID block;
    for (ReleasePoint* release : edgeReleases) {
        if (release->edge != previousEdge) {
            previousEdge = release->edge;
            BlockID target = targetOf(builder.function(), *release->edge).block;
            block = cfg.singleIncomingEdge(target) ? target : builder.splitEdge(*release->edge);
        }
        release->point = { block, 0 };
    }
}

static void rewriteFunction(const AnalysisContext& context, Function& function, TypeArena& types, const FunctionPlan& plan, const BitVector& reusedLayouts)
{
    Vector<ReleasePoint> releases;
    for (const ReleasePoint& release : plan.releases) {
        if (reusedLayouts.get(release.layout.index()))
            releases.append(release);
    }

    IRBuilder builder(context, function, types);
    resolveEdgeReleases(builder, releases);
    for (const ReleasePoint& release : releases)
        insertOperation(builder, release.point, Opcode::ReleaseEnvironment, release.environment);
    for (const EscapeMark& mark : plan.marks) {
        if (reusedLayouts.get(mark.layout.index()))
            insertOperation(builder, mark.point, Opcode::MarkEnvironmentEscaped, mark.environment);
    }
    builder.applyInsertions();
}

static bool needsRewrite(const FunctionPlan& plan, const BitVector& reusedLayouts)
{
    auto isReused = [&](const auto& operation) {
        return reusedLayouts.get(operation.layout.index());
    };
    return std::ranges::any_of(plan.marks, isReused) || std::ranges::any_of(plan.releases, isReused);
}

static void rewriteFunctions(ProgramEditor& editor, const AnalysisContext& programContext, const Vector<FunctionPlan>& plans, const BitVector& reusedLayouts)
{
    Vector<FunctionID> changedFunctions;
    for (unsigned index = 0; index < plans.size(); ++index) {
        if (needsRewrite(plans[index], reusedLayouts))
            changedFunctions.append(FunctionID(index));
    }

    editFunctionsInParallel(editor, programContext.build, changedFunctions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        rewriteFunction(contextOf(programContext, id), function, arenas.types, plans[id.index()], reusedLayouts);
    });
}

static Vector<FunctionID> findFunctionsWithSites(const BuildContext& build, const Vector<FunctionPlan>& plans, const BitVector& candidateLayouts)
{
    Vector<FunctionID> functions;
    for (unsigned index = 0; index < plans.size(); ++index) {
        bool hasSite = plans[index].sites.containsIf([&](const EnvironmentSite& site) {
            return candidateLayouts.get(site.layout.index());
        });
        if (hasSite && build.isEnabled(PassID::ReuseEnvironments, FunctionID(index)))
            functions.append(FunctionID(index));
    }
    return functions;
}

void EnvironmentReuse::dump(PrintStream& out, const Program&) const
{
    out.print("reused environments: [");
    CommaPrinter separator(" "_s);
    for (size_t index : m_reusedLayouts)
        out.print(separator, "L", index);
    out.println("]");
}

EnvironmentReuse reuseEnvironments(ProgramEditor& editor, const FlowResults& flow, const FrameAllocation& frameAllocation, BuildContext& build, NOESCAPE const ScopedLambda<BorrowedParameters()>& computeBorrowedParameters)
{
    const Program& program = editor.program();
    Vector<FunctionPlan> plans = createVectorInParallel<FunctionPlan>(build.parallelLoops(), program.functions.size());
    BitVector candidateLayouts = findCandidateLayouts(program, frameAllocation, build.parallelLoops(), plans);
    if (candidateLayouts.isEmpty())
        return { };

    EffectSummaries summaries = createSummariesFromFlow(program, flow, computeBorrowedParameters(), build);
    EffectContext effects { program, summaries };
    AnalysisContext programContext { build, program, &flow, &summaries };
    CandidateTypes types(program, candidateLayouts);
    Vector<uint32_t> largestFunctionsFirst = largeFunctions(program, build.parallelLoops());
    forEachIndexInParallel(build.parallelLoops(), plans.size(), largestFunctionsFirst.span(), [&](unsigned index) {
        classifyHolders(contextOf(programContext, FunctionID(index)), effects, types, plans[index]);
        findResultsMadeHere(contextOf(programContext, FunctionID(index)), types, plans[index]);
    });
    for (const FunctionPlan& plan : plans) {
        for (LayoutID layout : plan.disqualifiedLayouts)
            candidateLayouts.clear(layout.index());
    }
    bool someResultIsMadeByItsFunction = plans.containsIf([](const FunctionPlan& plan) {
        return !plan.resultsMadeHere.isEmpty();
    });
    if (someResultIsMadeByItsFunction) {
        forEachIndexInParallel(build.parallelLoops(), plans.size(), largestFunctionsFirst.span(), [&](unsigned index) {
            findSitesOfResults(program, types, candidateLayouts, FunctionID(index), plans.mutableSpan());
        });
    }

    Vector<FunctionID> functionsWithSites = findFunctionsWithSites(build, plans, candidateLayouts);
    Vector<Vector<LayoutID>> profitableLayouts(functionsWithSites.size());
    Vector<uint32_t> largestWithSitesFirst;
    for (uint32_t large : largestFunctionsFirst) {
        auto found = std::ranges::lower_bound(functionsWithSites, FunctionID(large));
        if (found != functionsWithSites.end() && *found == FunctionID(large))
            largestWithSitesFirst.append(static_cast<uint32_t>(found - functionsWithSites.begin()));
    }
    forEachIndexInParallel(build.parallelLoops(), functionsWithSites.size(), largestWithSitesFirst.span(), [&](unsigned index) {
        FunctionID id = functionsWithSites[index];
        FunctionPlan& plan = plans[id.index()];
        findReleasePoints(contextOf(programContext, id), types, candidateLayouts, plans.span(), plan);
        profitableLayouts[index] = findProfitableLayouts(program.at(id), plan);
    });

    EnvironmentReuse result;
    BitVector& reusedLayouts = result.m_reusedLayouts;
    for (auto& profitableInFunction : profitableLayouts) {
        for (LayoutID layout : profitableInFunction)
            reusedLayouts.set(layout.index());
    }
    reusedLayouts.filter(candidateLayouts);
    if (!reusedLayouts.isEmpty()) {
        rewriteFunctions(editor, programContext, plans, reusedLayouts);
        build.statistics().count(StatisticID::EnvironmentsReused, reusedLayouts.bitCount());
    }
    build.destroyLater(WTF::move(summaries));
    build.destroyLater(WTF::move(plans));
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
