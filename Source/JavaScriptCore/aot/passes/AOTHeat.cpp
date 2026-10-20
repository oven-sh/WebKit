/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeat.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeatGraph.h"
#include "AOTPass.h"
#include "AOTPhaseTimes.h"
#include "AOTThreads.h"
#include <algorithm>
#include <cmath>
#include <wtf/PrintStream.h>
#include <wtf/dtoa.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(HeatModel::coldPercentile < HeatModel::hotPercentile && HeatModel::hotPercentile < 100);

ASCIILiteral nameOf(HeatClass heatClass)
{
    switch (heatClass) {
    case HeatClass::Unknown:
        return "Unknown"_s;
    case HeatClass::RunsOnce:
        return "RunsOnce"_s;
    case HeatClass::Cold:
        return "Cold"_s;
    case HeatClass::Warm:
        return "Warm"_s;
    case HeatClass::Hot:
        return "Hot"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

double Heat::activationsOf(const Program& program, FunctionID id) const
{
    constexpr double activationsWhereEverythingIsHot = 1e12;

    if (m_classOfEverything)
        return isHot(*m_classOfEverything) ? activationsWhereEverythingIsHot : 0;
    return m_entries.of(program, id).activations;
}

HeatClass Heat::of(const Program& program, FunctionID id) const
{
    if (m_classOfEverything)
        return *m_classOfEverything;
    return m_entries.of(program, id).heatClass;
}

uint8_t Heat::percentileOf(const Program& program, FunctionID id) const
{
    return m_entries.of(program, id).percentile;
}

HeatClass Heat::classOf(double heat) const
{
    if (m_classOfEverything)
        return *m_classOfEverything;
    if (!m_thresholds)
        return HeatClass::Unknown;
    if (!(heat > 0))
        return HeatClass::Cold;
    if (heat >= m_thresholds->hot)
        return HeatClass::Hot;
    if (heat < m_thresholds->cold)
        return HeatClass::Cold;
    return HeatClass::Warm;
}

static void printWithThreeDigits(PrintStream& out, double number)
{
    NumberToStringBuffer buffer;
    out.print(numberToFixedPrecisionString(number, 3, buffer, true));
}

void Heat::dump(PrintStream& out, const Program& program) const
{
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Entry& entry = m_entries.of(program, id);
        if (entry.heatClass == HeatClass::Unknown)
            continue;
        out.print("f", index, " ", nameOf(of(program, id)), " ", entry.percentile, " ");
        printWithThreeDigits(out, entry.activations);
        out.print("\n");
    }
    out.print("hottest:");
    for (FunctionID id : m_hottestFirst)
        out.print(" f", id.index());
    out.print("\n");
    if (!m_thresholds)
        return;
    out.print("thresholds: cold=");
    printWithThreeDigits(out, m_thresholds->cold);
    out.print(" hot=");
    printWithThreeDigits(out, m_thresholds->hot);
    out.print("\n");
}

namespace {

struct FunctionDensity {
    double density { 0 };
    FunctionID function;
};

} // anonymous namespace

static Vector<FunctionDensity> sortByDensity(const ParallelLoops& loops, const HeatGraph& graph, const Vector<uint32_t>& functionsWithBody, const Vector<double>& activations)
{
    Vector<FunctionDensity> order(functionsWithBody.size());
    forEachIndexInParallel(loops, order.size(), [&](unsigned k) {
        FunctionID function(functionsWithBody[k]);
        uint32_t size = graph.sizeOf(function);
        order[k] = { activations[function.index()] * graph.massOf(function) / std::max(1u, size), function };
    });
    parallelSort(loops, order.mutableSpan(), [](const FunctionDensity& a, const FunctionDensity& b) {
        return a.density != b.density ? a.density > b.density : a.function < b.function;
    });
    return order;
}

static Vector<uint8_t> computePercentiles(const HeatGraph& graph, const Vector<FunctionDensity>& hottestFirst, uint64_t totalSize)
{
    Vector<uint8_t> result(FillWith { }, graph.numberOfFunctions(), static_cast<uint8_t>(0));
    uint64_t visitedSize = 0;
    uint64_t colderSize = 0;
    for (size_t k = hottestFirst.size(); k--;) {
        auto [density, function] = hottestFirst[k];
        if (k + 1 < hottestFirst.size() && density != hottestFirst[k + 1].density)
            colderSize = visitedSize;
        result[function.index()] = static_cast<uint8_t>(std::min<uint64_t>(99, colderSize * 100 / totalSize));
        visitedSize += graph.sizeOf(function);
    }
    return result;
}

namespace {

struct BlockHeat {
    double heat { 0 };
    uint32_t size { 0 };
};

} // anonymous namespace

// A threshold is compared with the heat of a block, so it comes from blocks: the hottest fifth of the application's code is hot and the coldest quarter is cold.
// Blocks that are equally hot have one class, so the hottest ones are hot however large they are. The library is in every executable and its share of a small one
// is large, so it has no say in which code of the application is hot. Code outside every loop, recursion and handler is never hot. Code that is expected to run less than
// once is cold however much of it there is, and code that is expected to run more than once is not cold however little there is below it.
// A fifth of a very large program is more than a processor keeps at hand, and most of it never runs: what is hot also fits MaxHotCodeValues.
static HeatThresholds computeThresholds(const Program& program, const HeatGraph& graph, const Vector<uint32_t>& functionsWithBody, const Vector<double>& activations, uint64_t maxHotSize)
{
    Vector<BlockHeat> hottestFirst;
    uint64_t totalSize = 0;
    for (uint32_t index : functionsWithBody) {
        if (isLibraryCode(program, program.functions[index]))
            continue;
        for (const HeatBlockGroup& group : graph.blockGroupsOf(FunctionID(index))) {
            hottestFirst.append(BlockHeat { activations[index] * group.frequency, group.size });
            totalSize += group.size;
        }
    }
    std::ranges::sort(hottestFirst, std::ranges::greater { }, &BlockHeat::heat);

    HeatThresholds thresholds;
    uint64_t hotterSize = 0;
    for (size_t first = 0; first < hottestFirst.size();) {
        double heat = hottestFirst[first].heat;
        uint64_t size = 0;
        for (; first < hottestFirst.size() && hottestFirst[first].heat == heat; ++first)
            size += hottestFirst[first].size;
        bool fitsHotCode = !hotterSize || hotterSize + size <= maxHotSize;
        if (hotterSize * 100 < totalSize * (100 - HeatModel::hotPercentile) && fitsHotCode && heat >= frequencyConstants.iterationsPerLoop)
            thresholds.hot = heat;
        uint64_t colderSize = totalSize - hotterSize - size;
        if (colderSize * 100 >= totalSize * HeatModel::coldPercentile)
            thresholds.cold = heat;
        hotterSize += size;
    }
    thresholds.cold = std::clamp(thresholds.cold, 1.0, std::nextafter(1.0, 2.0));
    return thresholds;
}

Heat estimateHeat(const Program& program, const CallGraph& callGraph, const FlowResults& flow, BuildContext& build)
{
    PhaseTimes times(build, PassID::EstimateHeat);
    EffectSummaries effects = createSummariesFromFlow(program, flow, build);
    times.didFinish("createSummariesFromFlow"_s);
    HeatGraph graph = buildHeatGraph(program, callGraph, flow, effects, build);
    times.didFinish("buildHeatGraph"_s);
    Vector<double> activations = estimateActivations(graph);
    times.didFinish("estimateActivations"_s);

    unsigned count = graph.numberOfFunctions();
    Vector<uint32_t> functionsWithBody = findIndicesInParallel(build.parallelLoops(), count, [&](unsigned i) {
        return graph.sizeOf(FunctionID(i)) > 0;
    });
    if (functionsWithBody.isEmpty())
        return Heat();
    uint64_t totalSize = 0;
    for (uint32_t i : functionsWithBody)
        totalSize += graph.sizeOf(FunctionID(i));

    Vector<FunctionDensity> order = sortByDensity(build.parallelLoops(), graph, functionsWithBody, activations);
    Vector<uint8_t> percentiles = computePercentiles(graph, order, totalSize);
    times.didFinish("sortByDensity"_s);

    Heat heat;
    heat.m_entries = PerFunction<Heat::Entry>(count);
    heat.m_hottestFirst = Vector<FunctionID>(order.size(), [&](size_t k) {
        return order[k].function;
    });
    heat.m_thresholds = computeThresholds(program, graph, functionsWithBody, activations, build.limit(LimitID::MaxHotCodeValues));
    times.didFinish("computeThresholds"_s);
    for (uint32_t i : functionsWithBody) {
        FunctionID id(i);
        HeatClass classOfEntryBlock = flow.runsAtMostOnce(program, id) ? HeatClass::RunsOnce : heat.classOf(activations[i]);
        heat.m_entries[id] = Heat::Entry { activations[i], classOfEntryBlock, percentiles[i] };
    }

    switch (build.recordedOptions().heat()) {
    case HeatOverride::None:
        break;
    case HeatOverride::OnlyLoopsAreHot:
        for (uint32_t i : functionsWithBody)
            heat.m_entries[FunctionID(i)] = Heat::Entry { 1, HeatClass::Warm, 50 };
        heat.m_thresholds = HeatThresholds { frequencyConstants.unlikelyEdgeShare, frequencyConstants.iterationsPerLoop };
        break;
    case HeatOverride::EverythingIsHot:
        heat.m_classOfEverything = HeatClass::Hot;
        break;
    case HeatOverride::EverythingIsCold:
        heat.m_classOfEverything = HeatClass::Cold;
        break;
    case HeatOverride::NothingIsHottest:
        heat.m_hottestFirst.clear();
        break;
    }
    return heat;
}

HeatClass heatClassOf(const Heat& heat, FunctionAnalyses& analyses, BlockID block)
{
    const AnalysisContext& context = analyses.context();
    HeatClass heatClass = heat.classOf(heat.activationsOf(context.program, context.id) * analyses.blockFrequencies().frequencyOf(block));
    switch (context.build.recordedOptions().heat()) {
    case HeatOverride::EverythingIsHot:
    case HeatOverride::EverythingIsCold:
        return heatClass;
    case HeatOverride::None:
    case HeatOverride::OnlyLoopsAreHot:
    case HeatOverride::NothingIsHottest:
        break;
    }
    return analyses.executionCountOf(block) == ExecutionCount::AtMostOnce ? HeatClass::RunsOnce : heatClass;
}

HeatClass heatClassOf(FunctionPassContext& context, BlockID block)
{
    return heatClassOf(context.inputs.heat, context.analyses, block);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
