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
#include "AOTLayoutData.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPhaseTimes.h"
#include "AOTPlacement.h"
#include "AOTPlacementSteps.h"
#include "AOTSiteWeight.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::none_of(allMembersOf<TerminatorKind>, [](TerminatorKind kind) {
    return siteCachesOf(kind) > 0;
}));
static_assert(std::ranges::all_of(allMembersOf<Opcode>, [](Opcode opcode) {
    return !siteCachesOf(opcode) || (siteCachesOf(opcode) == 1 && Value::opcodesWithSiteCache.contains(opcode));
}));

namespace {

using CacheRequest = SiteCacheRequest;

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
    RequestsOfStoreRuns requestsOfStoreRuns { RequestsOfStoreRuns::AreWritten };
};

class CacheRequestCollector {
public:
    CacheRequestCollector(const CachePlacementInputs& inputs, FunctionID id, bool needsWeights)
        : m_inputs(inputs)
        , m_id(id)
        , m_function(inputs.program.at(id))
        , m_needsWeights(needsWeights)
    {
    }

    void collect(Vector<CacheRequest>&);

private:
    FunctionAnalyses& analyses();
    SiteWeight weightOf(BlockID);

    const CachePlacementInputs& m_inputs;
    FunctionID m_id;
    const Function& m_function;
    bool m_needsWeights;
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
    return !context.program.at(context.id).isModuleBody() || analyses.isInCycle(block);
}

// FieldRecordIndex::isStoredWithoutTypeTest: the stubs by name store into such a slot in fewer instructions than PutKnownField, and file nothing either.
static bool isStoredThroughTableOfFields(const Program& program, const Field& declaration, SlotRepresentation representation)
{
    const Type& held = program.at(slotTypeOf(program, declaration));
    switch (representation) {
    case SlotRepresentation::JSValue:
        return held.isAnything();
    case SlotRepresentation::Int32:
    case SlotRepresentation::OptionalInt32:
        return held.primitives & Primitive::Integer && held.integers.minimum <= minInlineInt32 && held.integers.maximum >= INT32_MAX;
    case SlotRepresentation::Cell:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
    case SlotRepresentation::Uint32:
    case SlotRepresentation::Int64:
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::Double:
    case SlotRepresentation::OptionalDouble:
    case SlotRepresentation::Float:
    case SlotRepresentation::Boolean:
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::Uint8:
    case SlotRepresentation::Int8:
    case SlotRepresentation::Uint16:
    case SlotRepresentation::Int16:
    case SlotRepresentation::LiteralString:
    case SlotRepresentation::LiteralString32:
    case SlotRepresentation::DeferredClosure:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool storedFieldTakesAnyValue(const Program& program, const Placement& placement, const Function& function, const Value& store)
{
    return std::ranges::all_of(membersOf(program, program.typeOf(function, store.objectOperand())), [&](LayoutID member) {
        uint32_t field = *program.at(member).findField(store.key());
        const PlacedSlot& slot = placement.layout(member).slots[field];
        return slot.representation == SlotRepresentation::JSValue && !slot.isOutOfLine() && isStoredThroughTableOfFields(program, fieldAt(program, declarationOf(program, { member, field })), slot.representation);
    });
}

std::optional<uint32_t> findRecordOfStoredField(const Program& program, const Placement& placement, const Function& function, const Value& store, NameHasColumn nameHasColumn)
{
    if ((store.opcode != Opcode::PropSet || store.isFreeName()) && store.opcode != Opcode::PropDefine)
        return std::nullopt;
    const Type& receiver = program.typeOf(function, store.objectOperand());
    if (!receiver.isObject() || !receiver.layouts || !receiver.hasOnlyLayouts(program))
        return std::nullopt;
    std::optional<uint16_t> index;
    for (LayoutID member : membersOf(program, receiver)) {
        const PlannedLayout& planned = placement.layout(member);
        std::optional<uint32_t> field = program.at(member).findField(store.key());
        if (planned.storage != LayoutStorage::CompiledObject || !field)
            return std::nullopt;
        const Field& declaration = fieldAt(program, declarationOf(program, { member, *field }));
        const PlacedSlot& slot = planned.slots[*field];
        if (!declaration.form.isEmpty() || declaration.flags.contains(FieldFlags::IsPrivate) || !slot.hasStorage() || slot.representation == SlotRepresentation::DeferredClosure || !slot.indexInLayoutRecord)
            return std::nullopt;
        // A definition mostly creates the field, asks nothing of what is above the object, and has no site: told the record, a stub does in half the instructions what the way by name does.
        bool isDefinitionOfBoxedValue = store.opcode == Opcode::PropDefine && slot.representation == SlotRepresentation::JSValue && !slot.isOutOfLine();
        if ((nameHasColumn == NameHasColumn::Yes && !isDefinitionOfBoxedValue && isStoredThroughTableOfFields(program, declaration, slot.representation)) || (index && *index != *slot.indexInLayoutRecord))
            return std::nullopt;
        index = slot.indexInLayoutRecord;
    }
    if (!index)
        return std::nullopt;
    return static_cast<uint32_t>(sizeof(LayoutData) + *index * sizeof(FieldData));
}

unsigned numberOfSiteCachesUsedBy(const Program& program, const Placement& placement, const Function& function, const Value& value)
{
    if (value.opcode == Opcode::TryPutByIds && !value.request())
        return 0;
    return numberOfSiteCachesUsedByOperationWithRequest(program, placement, function, value);
}

static unsigned numberOfSiteCachesUsedBy(const CachePlacementInputs& inputs, const Function& function, const Value& value)
{
    if (inputs.requestsOfStoreRuns == RequestsOfStoreRuns::AreWritten)
        return numberOfSiteCachesUsedBy(inputs.program, inputs.placement, function, value);
    if (value.opcode == Opcode::TryPutByIds && !hasPropertyRun(inputs.program, inputs.build, value))
        return 0;
    return numberOfSiteCachesUsedByOperationWithRequest(inputs.program, inputs.placement, function, value);
}

unsigned numberOfSiteCachesUsedByOperationWithRequest(const Program& program, const Placement& placement, const Function& function, const Value& value)
{
    if (findRecordOfStoredField(program, placement, function, value, NameHasColumn::Yes))
        return 0;
    // Flow has seen compiled objects come to this read. The table of fields answers for all their layouts alike, and the stub without a site asks it first: a site in front of it would stay empty.
    if (value.opcode == Opcode::PropGet && !value.isFreeName() && value.receivers() && program.nameClassOf(value.key()) != NameClass::Length)
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
            if (!numberOfSiteCachesUsedBy(m_inputs, m_function, m_function.at(id)))
                continue;
            if (mayReadSiteCachesAgain(analyses(), block))
                requests.append(CacheRequest { m_needsWeights ? weightOf(block) : SiteWeight { 0 }, m_id, id });
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
        Function& function = editor.atForPlacedWords(cachesOfFunction.front().function);
        for (const PlacedCache& placed : cachesOfFunction)
            function.edit(placed.value, ValueEdit::Definition).setSiteCache(placed.cache);
    });
}

SiteCachePosition siteCachePositionOf(const Program& program, FunctionID function, ValueID value)
{
    FunctionID source = firstSourceOf(program, function);
    ModuleID module = program.at(source).module;
    return { module ? module.index() : UINT32_MAX, source.index(), function.index(), value.index() };
}

void collectSiteCacheRequests(const Program& program, const Placement& placement, const FlowResults& flow, const Heat& heat, BuildContext& build, FunctionID id, bool needsWeights, Vector<SiteCacheRequest>& requests)
{
    CachePlacementInputs inputs { program, placement, flow, heat, build, RequestsOfStoreRuns::AreNotWrittenYet };
    CacheRequestCollector(inputs, id, needsWeights).collect(requests);
}

SiteCaches placeSiteCaches(ProgramEditor& editor, const Placement& placement, const FlowResults& flow, const Heat& heat, BuildContext& build, std::span<const Vector<SiteCacheRequest>> requestsOfFunctions)
{
    PhaseTimes times(build, PassID::PlaceSiteCaches);
    const Program& program = editor.program();
    const ParallelLoops& loops = build.parallelLoops();
    CachePlacementInputs inputs { program, placement, flow, heat, build };

    uint32_t numberOfSitesWithoutCache = 0;
    auto collectRequests = [&](bool needsWeights) {
        auto requests = collectInParallel<CacheRequest>(loops, program.functions.size(), [&](unsigned index, Vector<CacheRequest>& found) {
            if (!program.functions[index].hasBody())
                return;
            if (requestsOfFunctions.empty())
                CacheRequestCollector(inputs, FunctionID(index), needsWeights).collect(found);
            else
                found.appendVector(requestsOfFunctions[index]);
        });
        numberOfSitesWithoutCache = static_cast<uint32_t>(requests.removeAllMatching([](const CacheRequest& request) {
            return request.runsAtMostOnce;
        }));
        return requests;
    };
    // Where every request is served, the order by position below is the only one that anybody sees.
    auto requests = collectRequests(false);
    if (requests.size() > build.limit(LimitID::MaxSiteCaches)) {
        requests = collectRequests(true);
        sortHottestFirst(loops, requests);
    }

    times.didFinish("collect requests"_s);
    auto count = static_cast<uint32_t>(std::min<uint64_t>(requests.size(), build.limit(LimitID::MaxSiteCaches)));
    sortByPosition(program, loops, requests.mutableSpan().first(count));
    times.didFinish("sortByPosition"_s);
    writeSiteCaches(editor, loops, requests.span().first(count));
    times.didFinish("writeSiteCaches"_s);

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
