/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTBindGlobalNames.h"
#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTProgramTableEncoder.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxIdentifiers, hostArchitecture()) <= GlobalBindingTable::identifierMask);

namespace {

struct FirstUse {
    GlobalBindingKey key;
    uint32_t functionRank { 0 };
    uint32_t siteIndex { 0 };
};

struct NumberedBinding {
    GlobalBindingKey key;
    GlobalBindingID id;
};

} // anonymous namespace

static Vector<FirstUse> findFirstUses(const PlacementContext& context, std::span<const GlobalBindingSite> sites)
{
    Vector<uint32_t> ranks(FillWith { }, context.program.functions.size(), UINT32_MAX);
    std::span<const FunctionID> hottestFirst = context.inputs.heat.hottestFirst();
    for (uint32_t rank = 0; rank < hottestFirst.size(); ++rank) {
        if (hottestFirst[rank].index() < ranks.size())
            ranks[hottestFirst[rank].index()] = rank;
    }
    Vector<FirstUse> uses(sites.size(), [&](size_t index) {
        return FirstUse { sites[index].key, ranks[sites[index].function.index()], static_cast<uint32_t>(index) };
    });
    parallelSort(context.build.parallelLoops(), uses.mutableSpan(), [](const FirstUse& a, const FirstUse& b) {
        return std::tie(a.key, a.functionRank, a.siteIndex) < std::tie(b.key, b.functionRank, b.siteIndex);
    });
    removeRepeatedElements(uses, [](const FirstUse& a, const FirstUse& b) {
        return a.key == b.key;
    });
    AOT::sort(uses, [](const FirstUse& a, const FirstUse& b) {
        return std::tie(a.functionRank, a.siteIndex) < std::tie(b.functionRank, b.siteIndex);
    });
    return uses;
}

static GlobalBindingID findBinding(std::span<const NumberedBinding> ascendingBindings, const GlobalBindingKey& key)
{
    auto found = std::ranges::lower_bound(ascendingBindings, key, { }, &NumberedBinding::key);
    return found != ascendingBindings.end() && found->key == key ? found->id : GlobalBindingID();
}

GlobalBindingTable placeGlobalBindings(ProgramEditor& editor, const PlacementContext& context, TypeArena& types, BodyScansForPlacement& scans)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();

    auto sites = collectInParallel<GlobalBindingSite>(loops, program.functions.size(), [&](unsigned index, Vector<GlobalBindingSite>& found) {
        if (program.functions[index].hasBody())
            found.appendVector(scans.globalBindingSitesOf(FunctionID(index)));
    });

    BitVector keysStoredByName(program.types.numberOfPropertyKeys());
    sites.removeAllMatching([&](const GlobalBindingSite& site) {
        if (site.isNamedStore)
            keysStoredByName.quickSet(site.key.key.index());
        return site.isNamedStore;
    });

    Vector<FirstUse> firstUses = findFirstUses(context, sites.span());
    auto count = static_cast<uint32_t>(std::min<uint64_t>(firstUses.size(), context.build.limit(LimitID::MaxGlobalBindings)));
    bool exceedsLimit = count < firstUses.size();
    firstUses.shrink(count);

    Vector<NumberedBinding> ascendingBindings(count, [&](size_t index) {
        return NumberedBinding { firstUses[index].key, GlobalBindingID(static_cast<uint32_t>(index)) };
    });
    AOT::sort(ascendingBindings, { }, &NumberedBinding::key);

    if (exceedsLimit) {
        Vector<GlobalBindingSite> sitesWithoutBinding;
        sites.removeAllMatching([&](const GlobalBindingSite& site) {
            if (findBinding(ascendingBindings.span(), site.key))
                return false;
            if (site.key.isStore && site.key.seesScriptBindings)
                keysStoredByName.quickSet(site.key.key.index());
            sitesWithoutBinding.append(site);
            return true;
        });
        auto groupsWithoutBinding = groupSitesByFunction(sitesWithoutBinding.span());
        for (std::span<const GlobalBindingSite> sitesOfFunction : groupsWithoutBinding) {
            FunctionID id = sitesOfFunction.front().function;
            IRBuilder builder(AnalysisContext { context.build, program, &context.inputs.flow, nullptr, id }, editor.at(id), types);
            for (const GlobalBindingSite& site : sitesOfFunction) {
                convertGlobalBindingToNamedAccess(builder, site.point, site.value);
                context.build.gaveUp(LimitID::MaxGlobalBindings, { .function = id, .value = site.value });
            }
            builder.applyInsertions();
        }
        for (std::span<const GlobalBindingSite> sitesOfFunction : groupsWithoutBinding)
            scans.scan(sitesOfFunction.front().function);
    }
    auto groups = groupSitesByFunction(sites.span());
    forEachIndexInParallel(loops, groups.size(), [&](unsigned group) {
        Function& function = editor.atForPlacedWords(groups[group].front().function);
        for (const GlobalBindingSite& site : groups[group])
            function.edit(site.value, ValueEdit::Definition).setBinding(findBinding(ascendingBindings.span(), site.key));
    });

    GlobalBindingTable table;
    table.bindings = Vector<PlacedGlobalBinding>(count, [&](size_t index) {
        const GlobalBindingKey& key = firstUses[index].key;
        return PlacedGlobalBinding { key, keysStoredByName.quickGet(key.key.index()) };
    });
    context.build.statistics().count(StatisticID::GlobalBindingsPlaced, count);
    return table;
}

void encodeGlobalBindingTable(TableEncoder& encoder)
{
    auto& bindings = encoder.placement.globalBindings().bindings;
    encoder.setTable(ProgramTableIndex::globalBindings, Vector<uint32_t>(bindings.size(), [&](size_t index) {
        const PlacedGlobalBinding& binding = bindings[index];
        uint32_t word = encoder.identifierNumber(binding.key.key);
        if (binding.key.seesScriptBindings)
            word |= GlobalBindingTable::seesScriptBindingsFlag;
        if (binding.isStoredByName)
            word |= GlobalBindingTable::isStoredByNameFlag;
        if (binding.key.isForTypeOf)
            word |= GlobalBindingTable::isForTypeOfFlag;
        return word;
    }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
