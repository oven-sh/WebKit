/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowApply.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct VersionCandidate {
    FunctionID function;
    uint64_t narrowedUses { 0 };
    uint64_t size { 0 };
};

} // anonymous namespace

static uint32_t functionCountOf(const FlowFamilyInputs& inputs)
{
    return static_cast<uint32_t>(inputs.program().functions.size());
}

static bool isVersionCandidate(const FlowFamilyInputs& inputs, FunctionID id)
{
    const Function& function = inputs.program().at(id);
    const FlowGraph& graph = inputs.graph();
    return function.hasBody()
        && function.mayBeCloned()
        && function.mayChangeSignature()
        && !function.isModuleBody()
        && inputs.isEnabled(FlowResultKind::CallVersions, id.index())
        && inputs.isEnabled(FlowResultKind::NarrowedTypes, id.index())
        && !inputs.solution.isPastWorkLimit(id)
        && graph.findInstance(id, FlowContext::Generic)
        && graph.isEnteredWithTrackedOperands(id);
}

static uint64_t countNarrowedUses(const FlowFamilyInputs& inputs, const FlowTypeConverter& converter, FunctionID id, TypeArena& arena)
{
    const Program& program = inputs.program();
    const FlowGraph& graph = inputs.graph();
    const Function& function = program.at(id);
    const FlowSummary& summary = graph.summaryOf(id);

    FlowVersionPlan planWithoutVersion { .provenVersions = { }, .functionCountAsRead = functionCountOf(inputs) };
    FlowVersionPlan planWithVersion { .provenVersions = { id }, .functionCountAsRead = functionCountOf(inputs) };
    FlowFunctionVersion sharedVersion = findFunctionVersion(graph, planWithoutVersion, id);
    FlowFunctionVersion provenVersion = findFunctionVersion(graph, planWithVersion, id);

    FlowConversionSwitches switches = FlowConversionSwitches::of(inputs.build(), id.index());
    switches.predictions = false;

    uint64_t narrowedUses = 0;
    for (unsigned index = 0, count = function.numberOfEntryParameters(); index < count; ++index) {
        const DeclaredType* declared = function.declaredEntryParameter(index);
        // The type of a capture joins what every closure of the function stores, so a version of its own narrows it no further.
        if (!declared || function.entryParameterKind(index) == EntryParameter::Capture)
            continue;
        FlowLocalID parameter = summary.entryParameter(index);
        uint64_t uses = summary.successors.of(parameter).size() + summary.waiters.of(parameter).size();
        if (!uses)
            continue;
        const Type& asRead = program.at(declared->type);
        Type joined = converter.convertWith(nodesOf(graph, sharedVersion, parameter).span(), asRead, switches, arena).type;
        Type proven = converter.convertWith(nodesOf(graph, provenVersion, parameter).span(), asRead, switches, arena).type;
        if (isWorthNarrowing(program, joined, proven))
            narrowedUses += uses;
    }
    return narrowedUses;
}

static void collectVersionCandidate(const FlowFamilyInputs& inputs, const FlowTypeConverter& converter, FunctionID id, TypeInterningPhase& phase, Vector<VersionCandidate>& candidates)
{
    if (!isVersionCandidate(inputs, id))
        return;

    TypeArena arena(phase, TypeInterningPhase::Scratch);
    uint64_t narrowedUses = countNarrowedUses(inputs, converter, id, arena);
    if (!narrowedUses)
        return;

    if (narrowedUses < inputs.build().limit(LimitID::MinNarrowedUsesForGenericVersion)) {
        inputs.build().gaveUp(LimitID::MinNarrowedUsesForGenericVersion, Subject { .function = id });
        return;
    }
    candidates.append(VersionCandidate { id, narrowedUses, inputs.program().at(id).values.size() });
}

static Vector<VersionCandidate> collectVersionCandidates(const FlowFamilyInputs& inputs, const FlowTypeConverter& converter, TypeTable& types)
{
    TypeInterningPhase phase(types, 0, TypeInterningPhase::Scratch);
    Vector<VersionCandidate> candidates = collectInParallel<VersionCandidate>(inputs.loops(), functionCountOf(inputs), [&](unsigned index, Vector<VersionCandidate>& found) {
        collectVersionCandidate(inputs, converter, FunctionID(index), phase, found);
    });
    phase.discard();
    return candidates;
}

static uint64_t computeGrowthBudget(const FlowFamilyInputs& inputs)
{
    uint64_t reachedSize = 0;
    for (uint32_t index = 0, count = functionCountOf(inputs); index < count; ++index) {
        FunctionID id(index);
        if (inputs.graph().isReached(id))
            reachedSize += inputs.program().at(id).values.size();
    }
    return inputs.build().limit(LimitID::MaxGenericVersionGrowthPercent) * reachedSize / 100;
}

static Vector<FunctionID> selectProvenVersions(const FlowFamilyInputs& inputs, Vector<VersionCandidate>&& candidates)
{
    parallelSort(inputs.loops(), candidates.mutableSpan(), [](const VersionCandidate& a, const VersionCandidate& b) {
        uint64_t benefitOfA = a.narrowedUses * b.size;
        uint64_t benefitOfB = b.narrowedUses * a.size;
        if (benefitOfA != benefitOfB)
            return benefitOfA > benefitOfB;
        return a.function < b.function;
    });

    uint64_t budget = computeGrowthBudget(inputs);
    uint64_t growth = 0;
    Vector<FunctionID> provenVersions;
    for (const VersionCandidate& candidate : candidates) {
        if (candidate.size > budget - growth) {
            inputs.build().gaveUp(LimitID::MaxGenericVersionGrowthPercent, Subject { .function = candidate.function });
            continue;
        }
        growth += candidate.size;
        provenVersions.append(candidate.function);
    }
    std::ranges::sort(provenVersions);
    return provenVersions;
}

FlowVersionPlan computeVersionResults(const FlowFamilyInputs& inputs, const FlowTypeConverter& converter, TypeTable& types)
{
    FlowVersionPlan plan;
    plan.functionCountAsRead = functionCountOf(inputs);
    plan.provenVersions = selectProvenVersions(inputs, collectVersionCandidates(inputs, converter, types));
    return plan;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
