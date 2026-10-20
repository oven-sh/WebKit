/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramAnalyses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectSummariesInternal.h"
#include "AOTHeat.h"
#include "AOTPassRunner.h"
#include "AOTSummarizeForInlining.h"
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

template<typename Result>
static void destroyResultLater(BuildContext& build, std::unique_ptr<Result>& result)
{
    if (!result)
        return;
    build.destroyLater(std::exchange(result, nullptr));
}

ProgramAnalyses::ProgramAnalyses(const Program& program, PassRunner& runner, DerivedFunctions& derivedFunctions)
    : m_program(program)
    , m_runner(runner)
    , m_derivedFunctions(derivedFunctions)
    , m_unknownCallGraph(makeUniqueWithoutFastMallocCheck<const CallGraph>())
    , m_unknownEffects(makeUniqueWithoutFastMallocCheck<const EffectSummaries>())
    , m_unknownHeat(makeUniqueWithoutFastMallocCheck<const Heat>())
    , m_unknownInlining(makeUniqueWithoutFastMallocCheck<const InliningSummaries>())
{
}

ProgramAnalyses::~ProgramAnalyses() = default;

const CallGraph& ProgramAnalyses::callGraph(const FlowResults& flow) const
{
    if (!m_callGraph) {
        m_callGraph = makeUniqueWithoutFastMallocCheck<CallGraph>(m_runner.runAnalysis(PassID::BuildCallGraph, m_program, [&] {
            return buildCallGraph(m_program, flow, m_runner.build());
        }));
    }
    return *m_callGraph;
}

const EffectSummaries& ProgramAnalyses::effects(const FlowResults& flow) const
{
    if (!m_effects) {
        if (!m_bodySummaries)
            m_bodySummaries = makeUnique<BodySummaries>();
        m_effects = makeUniqueWithoutFastMallocCheck<EffectSummaries>(m_runner.runAnalysis(PassID::ComputeEffectSummaries, m_program, [&] {
            return computeEffectSummaries(m_program, flow, m_runner.build(), m_bodySummaries.get());
        }));
    }
    return *m_effects;
}

const Heat& ProgramAnalyses::heat(const FlowResults& flow) const
{
    if (!m_heat) {
        const CallGraph& graph = callGraph(flow);
        m_heat = makeUniqueWithoutFastMallocCheck<Heat>(m_runner.runAnalysis(PassID::EstimateHeat, m_program, [&] {
            return estimateHeat(m_program, graph, flow, m_runner.build());
        }));
    }
    return *m_heat;
}

const InliningSummaries& ProgramAnalyses::inlining(const FlowResults& flow) const
{
    if (!m_inlining) {
        const CallGraph& graph = callGraph(flow);
        const Heat& estimatedHeat = heat(flow);
        m_inlining = makeUniqueWithoutFastMallocCheck<InliningSummaries>(m_runner.runAnalysis(PassID::SummarizeForInlining, m_program, [&] {
            return summarizeForInlining(m_program, graph, estimatedHeat, flow, m_runner.build());
        }));
    }
    return *m_inlining;
}

// The effect summaries ask nothing of the call graph, nor of what is computed from it, much of which runs on one thread.
void ProgramAnalyses::computeEffectsBesideOtherResults(const FlowResults& flow, OptionSet<ProgramResult> needed)
{
    if (m_effects || !needed.contains(ProgramResult::EffectSummaries) || !m_runner.mayRunAnalysesTogether())
        return;
    bool lacksAnotherResult = (needed.contains(ProgramResult::CallGraph) && !m_callGraph)
        || (needed.contains(ProgramResult::Heat) && !m_heat)
        || (needed.contains(ProgramResult::InliningSummaries) && !m_inlining);
    if (!lacksAnotherResult)
        return;

    if (!m_bodySummaries)
        m_bodySummaries = makeUnique<BodySummaries>();
    m_effects = makeUniqueWithoutFastMallocCheck<EffectSummaries>(m_runner.runAnalysisBeside(PassID::ComputeEffectSummaries, m_program, [&] {
        return computeEffectSummaries(m_program, flow, m_runner.build(), m_bodySummaries.get());
    }, [&] {
        if (needed.contains(ProgramResult::CallGraph))
            callGraph(flow);
        if (needed.contains(ProgramResult::Heat))
            heat(flow);
        if (needed.contains(ProgramResult::InliningSummaries))
            inlining(flow);
    }));
}

OptimizationInputs ProgramAnalyses::inputs(const FlowResults& flow, OptionSet<ProgramResult> needed)
{
    computeEffectsBesideOtherResults(flow, needed);
    return {
        flow,
        needed.contains(ProgramResult::CallGraph) ? callGraph(flow) : *m_unknownCallGraph,
        needed.contains(ProgramResult::Heat) ? heat(flow) : *m_unknownHeat,
        needed.contains(ProgramResult::InliningSummaries) ? inlining(flow) : *m_unknownInlining,
        needed.contains(ProgramResult::EffectSummaries) ? effects(flow) : *m_unknownEffects,
        &m_derivedFunctions,
    };
}

std::unique_ptr<EffectSummaries> ProgramAnalyses::takeEffects(const FlowResults& flow)
{
    effects(flow);
    m_runner.didInvalidate(ProgramResult::EffectSummaries);
    destroyResultLater(m_runner.build(), m_bodySummaries);
    return std::exchange(m_effects, nullptr);
}

std::unique_ptr<Heat> ProgramAnalyses::takeHeat(const FlowResults& flow)
{
    heat(flow);
    m_runner.didInvalidate(ProgramResult::Heat);
    return std::exchange(m_heat, nullptr);
}

void ProgramAnalyses::invalidate(OptionSet<ProgramResult> results)
{
    destroyResultLater(m_runner.build(), m_bodySummaries);
    destroyResults(results);
}

void ProgramAnalyses::invalidate(OptionSet<ProgramResult> results, const ProgramEditor& editor)
{
    if (editor.mayHaveChangedSharedRecords())
        destroyResultLater(m_runner.build(), m_bodySummaries);
    else if (m_bodySummaries)
        m_bodySummaries->didChange(editor.changedFunctions().span());
    destroyResults(results);
}

void ProgramAnalyses::destroyResults(OptionSet<ProgramResult> results)
{
    if (results.containsAny({ ProgramResult::CallGraph, ProgramResult::Heat }))
        results.add(ProgramResult::InliningSummaries);

    BuildContext& build = m_runner.build();
    if (results.contains(ProgramResult::InliningSummaries))
        destroyResultLater(build, m_inlining);
    if (results.contains(ProgramResult::Heat))
        destroyResultLater(build, m_heat);
    if (results.contains(ProgramResult::CallGraph))
        destroyResultLater(build, m_callGraph);
    if (results.contains(ProgramResult::EffectSummaries))
        destroyResultLater(build, m_effects);
    m_runner.didInvalidate(results & resultsComputedOnDemand);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
