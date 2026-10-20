/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSiteCachePlacement.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionKinds.h"
#include "AOTHeat.h"
#include "AOTSiteWeight.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) {
    return siteCachesOf(kind) > 0;
}));
static_assert(std::ranges::all_of(allMembersOf<Opcode>, [](Opcode opcode) {
    return !siteCachesOf(opcode) || (siteCachesOf(opcode) == 1 && Value::opcodesWithSiteCache.contains(opcode));
}));

namespace {

struct CacheRequest {
    SiteWeight weight { 0 };
    FunctionID function;
    ValueID value;
    bool runsAtMostOnce { false };
};

struct PlacedCache {
    FunctionID function;
    ValueID value;
    SiteCacheID cache;
};

struct CachePlacementInputs {
    const Program& program;
    const Placement& placement;
    const FlowResults& flow;
    const Heat& heat;
    BuildContext& build;
};

class CacheRequestCollector {
public:
    CacheRequestCollector(const CachePlacementInputs& inputs, FunctionID id)
        : m_inputs(inputs)
        , m_id(id)
        , m_function(inputs.program.at(id))
    {
    }

    void collect(Vector<CacheRequest>&);

private:
    FunctionAnalyses& analyses();
    SiteWeight weightOf(BlockID);

    const CachePlacementInputs& m_inputs;
    FunctionID m_id;
    const Function& m_function;
    std::optional<FunctionAnalyses> m_analyses;
    std::optional<FunctionSiteWeights> m_weights;
};

} // anonymous namespace

// A module body runs again only after its entry left the cache of `require`.
bool mayReadSiteCachesAgain(FunctionAnalyses& analyses, BlockID block)
{
    if (analyses.executionCountOf(block) == ExecutionCount::AtMostOnce)
        return false;
    const AnalysisContext& context = analyses.context();
    return !context.program.at(context.id).isModuleBody() || analyses.blockCycles().isInCycle(block);
}

unsigned numberOfSiteCachesUsedBy(const Program&, const Placement&, const Value& value)
{
    if (value.opcode == Opcode::TryPutByIds && !value.request())
        return 0;
    return siteCachesOf(value.opcode);
}

FunctionAnalyses& CacheRequestCollector::analyses()
{
    if (!m_analyses)
        m_analyses.emplace(AnalysisContext { m_inputs.build, m_inputs.program, &m_inputs.flow, nullptr, m_id }, m_function);
    return *m_analyses;
}

SiteWeight CacheRequestCollector::weightOf(BlockID block)
{
    if (!m_weights)
        m_weights.emplace(m_inputs.program, m_inputs.heat, m_id, analyses());
    return m_weights->weightOf(block);
}

void CacheRequestCollector::collect(Vector<CacheRequest>& requests)
{
    for (uint32_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID id : m_function.blocks[blockIndex].values) {
            if (!numberOfSiteCachesUsedBy(m_inputs.program, m_inputs.placement, m_function.at(id)))
                continue;
            if (mayReadSiteCachesAgain(analyses(), block))
                requests.append(CacheRequest { weightOf(block), m_id, id });
            else
                requests.append(CacheRequest { 0, m_id, id, true });
        }
    }
}

static void sortHottestFirst(const ParallelLoops& loops, Vector<CacheRequest>& requests)
{
    parallelSort(loops, requests.mutableSpan(), [](const CacheRequest& a, const CacheRequest& b) {
        if (a.weight != b.weight)
            return a.weight > b.weight;
        return std::tie(a.function, a.value) < std::tie(b.function, b.value);
    });
}

static void sortByPosition(const Program& program, const ParallelLoops& loops, std::span<CacheRequest> requests)
{
    Vector<std::pair<SiteCachePosition, CacheRequest>> positioned(requests.size());
    forEachIndexInParallel(loops, requests.size(), [&](unsigned index) {
        positioned[index] = { siteCachePositionOf(program, requests[index].function, requests[index].value), requests[index] };
    });
    parallelSort(loops, positioned.mutableSpan(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    forEachIndexInParallel(loops, requests.size(), [&](unsigned index) {
        requests[index] = positioned[index].second;
    });
}

static void writeSiteCaches(ProgramEditor& editor, const ParallelLoops& loops, std::span<const CacheRequest> servedRequests)
{
    Vector<PlacedCache> caches(servedRequests.size(), [&](size_t index) {
        const CacheRequest& request = servedRequests[index];
        return PlacedCache { request.function, request.value, SiteCacheID(index) };
    });
    parallelSort(loops, caches.mutableSpan(), [](const PlacedCache& a, const PlacedCache& b) {
        return a.function < b.function;
    });

    Vector<uint32_t> firstCacheOfFunction;
    for (uint32_t index = 0; index < caches.size(); ++index) {
        if (!index || caches[index].function != caches[index - 1].function)
            firstCacheOfFunction.append(index);
    }
    firstCacheOfFunction.append(static_cast<uint32_t>(caches.size()));

    forEachIndexInParallel(loops, firstCacheOfFunction.size() - 1, [&](unsigned group) {
        uint32_t first = firstCacheOfFunction[group];
        auto cachesOfFunction = caches.span().subspan(first, firstCacheOfFunction[group + 1] - first);
        Function& function = editor.at(cachesOfFunction.front().function);
        for (const PlacedCache& placed : cachesOfFunction)
            function.at(placed.value).setSiteCache(placed.cache);
    });
}

SiteCachePosition siteCachePositionOf(const Program& program, FunctionID function, ValueID value)
{
    FunctionID source = firstSourceOf(program, function);
    ModuleID module = program.at(source).module;
    return { module ? module.index() : UINT32_MAX, source.index(), function.index(), value.index() };
}

SiteCaches placeSiteCaches(ProgramEditor& editor, const Placement& placement, const FlowResults& flow, const Heat& heat, BuildContext& build)
{
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    CachePlacementInputs inputs { program, placement, flow, heat, build };

    auto requests = collectInParallel<CacheRequest>(loops, program.functions.size(), [&](unsigned index, Vector<CacheRequest>& found) {
        if (program.functions[index].hasBody())
            CacheRequestCollector(inputs, FunctionID(index)).collect(found);
    });
    auto numberOfSitesWithoutCache = static_cast<uint32_t>(requests.removeAllMatching([](const CacheRequest& request) {
        return request.runsAtMostOnce;
    }));
    sortHottestFirst(loops, requests);

    auto count = static_cast<uint32_t>(std::min<uint64_t>(requests.size(), build.limit(LimitID::MaxSiteCaches)));
    sortByPosition(program, loops, requests.mutableSpan().first(count));
    writeSiteCaches(editor, loops, requests.span().first(count));

    auto unservedRequests = requests.span().subspan(count);
    forEachIndexInParallel(loops, unservedRequests.size(), [&](unsigned index) {
        build.gaveUp(LimitID::MaxSiteCaches, { .function = unservedRequests[index].function, .value = unservedRequests[index].value });
    });

    numberOfSitesWithoutCache += static_cast<uint32_t>(unservedRequests.size());
    build.statistics().count(StatisticID::SiteCachesPlaced, count);
    build.statistics().count(StatisticID::SitesWithoutSiteCache, numberOfSitesWithoutCache);
    return SiteCaches(count, numberOfSitesWithoutCache);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
