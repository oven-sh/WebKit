/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReuseEnvironments.h"

#if ENABLE(AOT)

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
#include "AOTLiveness.h"
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

struct FunctionPlan {
    Vector<EscapeMark> marks;
    Vector<ReleasePoint> releases;
    Vector<LayoutID> disqualifiedLayouts;
    Vector<EnvironmentSite> sitesInCycles;
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

    bool hasCandidateValue(const Function& function) const
    {
        return function.values.containsIf([&](const Value& value) {
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

static void forEachLayoutWithSites(const FunctionPlan& plan, NOESCAPE const Invocable<void(LayoutID, std::span<const EnvironmentSite>)> auto& functor)
{
    std::span<const EnvironmentSite> sites = plan.sitesInCycles.span();
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

static void findSitesInCycles(const Program& program, const FrameAllocation& frameAllocation, const Function& function, FunctionPlan& plan)
{
    Vector<EnvironmentSite>& sites = plan.sitesInCycles;
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (ValueID id : function.blocks[blockIndex].values) {
            const Value& value = function.at(id);
            if (isReusableAllocation(program, frameAllocation, value))
                sites.append({ value.layout(), BlockID(blockIndex) });
        }
    }
    if (sites.isEmpty())
        return;

    CFG cfg(function);
    BlockCycles cycles(cfg);
    sites.removeAllMatching([&](const EnvironmentSite& site) {
        return !cfg.isReachable(site.block) || !cycles.isInCycle(site.block);
    });
    std::ranges::sort(sites);
    removeRepeatedElements(sites);
}

static BitVector findCandidateLayouts(const Program& program, const FrameAllocation& frameAllocation, const ParallelLoops& loops, Vector<FunctionPlan>& plans)
{
    forEachIndexInParallel(loops, plans.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (function.hasBody())
            findSitesInCycles(program, frameAllocation, function, plans[index]);
    });

    BitVector candidateLayouts;
    for (const FunctionPlan& plan : plans) {
        for (const EnvironmentSite& site : plan.sitesInCycles)
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

    OperandSummary summary = summarizeOperand(m_effects, m_function, user, operandIndex);
    if (summary.uses.contains(OperandUse::PassesToUserCode))
        return HolderAction::Mark;
    if (summary.uses.contains(OperandUse::Retains))
        return summary.retainedIn == RetainedIn::Result && isFrameCell(m_function, userID) ? HolderAction::Ignore : HolderAction::Mark;
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
                    if (live != suspension && !isOwned(m_function, analyses.escape(), live))
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
    const Function& function = context.program.at(context.id);
    if (function.hasBody() && types.hasCandidateValue(function))
        HolderClassifier(context, effects, types, plan).run();
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
    ReleaseFinder(const AnalysisContext& context, const CandidateTypes& types, const Function& function, FunctionPlan& plan)
        : m_types(types)
        , m_function(function)
        , m_plan(plan)
        , m_analyses(context, function)
        , m_framePointers(function)
    {
    }

    void findReleasePoints(LayoutID, std::span<const EnvironmentSite>);

private:
    bool isOwnedEnvironment(ValueID, LayoutID);
    TrackedValues computeTrackedValues(LayoutID);
    std::optional<ValueID> findReleasableEnvironment(const TrackedValues&, const Liveness::LocalCalc&, LayoutID);
    void findReleasesInBlock(BlockID, LayoutID, const TrackedValues&, const Liveness&);
    void findReleasesOnEdges(BlockID, LayoutID, const TrackedValues&, const Liveness&, const Liveness::LocalCalc&);

    const CandidateTypes& m_types;
    const Function& m_function;
    FunctionPlan& m_plan;
    FunctionAnalyses m_analyses;
    FramePointers m_framePointers;
};

bool ReleaseFinder::isOwnedEnvironment(ValueID id, LayoutID layout)
{
    if (m_types.environmentOf(m_function, id) != layout || !isAlwaysCell(m_analyses.program(), m_function, id))
        return false;
    return isOwned(m_function, m_analyses.escape(), id);
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
        if (m_types.environmentOf(m_function, id) != layout && !(tracksFramePointers && m_framePointers.contains(id)))
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
    const BlockCycles& cycles = m_analyses.blockCycles();
    auto& targets = m_function.at(blockID).terminator.targets;
    Vector<Edge, 2> edges;
    for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        BlockID successor = targets[targetIndex].block;
        if (!cycles.areInSameCycle(blockID, successor) || !liveness.liveAtHead(successor).empty())
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
        if (std::optional<ValueID> environment = findReleasableEnvironment(tracked, calc, layout))
            m_plan.releases.append({ std::nullopt, { blockID, index + 1 }, *environment, layout });
    }
}

void ReleaseFinder::findReleasePoints(LayoutID layout, std::span<const EnvironmentSite> sites)
{
    TrackedValues tracked = computeTrackedValues(layout);
    Liveness liveness(m_function, m_analyses.cfg(), m_analyses.useLists(), LivenessOptions { .trackedValues = &tracked.bits });
    const BlockCycles& cycles = m_analyses.blockCycles();
    for (unsigned blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        BlockID blockID(blockIndex);
        if (liveness.liveAtHead(blockID).empty() && !tracked.definingBlocks.quickGet(blockIndex))
            continue;
        bool isInCycleOfSite = std::ranges::any_of(sites, [&](const EnvironmentSite& site) {
            return cycles.areInSameCycle(blockID, site.block);
        });
        if (isInCycleOfSite)
            findReleasesInBlock(blockID, layout, tracked, liveness);
    }
}

} // anonymous namespace

static void findReleasePoints(const AnalysisContext& context, const CandidateTypes& types, const BitVector& candidateLayouts, FunctionPlan& plan)
{
    ReleaseFinder finder(context, types, context.program.at(context.id), plan);
    forEachLayoutWithSites(plan, [&](LayoutID layout, std::span<const EnvironmentSite> sites) {
        if (candidateLayouts.get(layout.index()))
            finder.findReleasePoints(layout, sites);
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
        bool hasSite = plans[index].sitesInCycles.containsIf([&](const EnvironmentSite& site) {
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

EnvironmentReuse reuseEnvironments(ProgramEditor& editor, const FlowResults& flow, const FrameAllocation& frameAllocation, BuildContext& build)
{
    const Program& program = editor.program();
    Vector<FunctionPlan> plans(program.functions.size());
    BitVector candidateLayouts = findCandidateLayouts(program, frameAllocation, build.parallelLoops(), plans);
    if (candidateLayouts.isEmpty())
        return { };

    EffectSummaries summaries = createSummariesFromFlow(program, flow, build);
    EffectContext effects { program, summaries };
    AnalysisContext programContext { build, program, &flow, &summaries };
    CandidateTypes types(program, candidateLayouts);
    forEachIndexInParallel(build.parallelLoops(), plans.size(), [&](unsigned index) {
        classifyHolders(contextOf(programContext, FunctionID(index)), effects, types, plans[index]);
    });
    for (const FunctionPlan& plan : plans) {
        for (LayoutID layout : plan.disqualifiedLayouts)
            candidateLayouts.clear(layout.index());
    }

    Vector<FunctionID> functionsWithSites = findFunctionsWithSites(build, plans, candidateLayouts);
    Vector<Vector<LayoutID>> profitableLayouts(functionsWithSites.size());
    forEachIndexInParallel(build.parallelLoops(), functionsWithSites.size(), [&](unsigned index) {
        FunctionID id = functionsWithSites[index];
        FunctionPlan& plan = plans[id.index()];
        findReleasePoints(contextOf(programContext, id), types, candidateLayouts, plan);
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
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
