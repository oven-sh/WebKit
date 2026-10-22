/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGlobalBindingPlacement.h"

#if ENABLE(AOT)

#include "AOTBindGlobalNames.h"
#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTIRPrinter.h"
#include "AOTPlacementSteps.h"
#include "AOTProgramTableEncoder.h"
#include "AOTThreads.h"
#include <algorithm>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxIdentifiers, hostArchitecture()) <= GlobalBindingTable::identifierMask);

namespace {

using BindingSite = GlobalBindingSite;

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

void collectGlobalBindingSites(const Program& program, FunctionID id, Vector<GlobalBindingSite>& sites)
{
    const Function& function = program.at(id);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        auto& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            ProgramPoint point { BlockID(blockIndex), index };
            switch (value.opcode) {
            case Opcode::GetGlobalBinding:
                sites.append(BindingSite { { value.key(), value.seesScriptBindings(), false, value.isForTypeOf() }, id, point, values[index] });
                break;
            case Opcode::SetGlobalBinding:
                sites.append(BindingSite { { value.key(), value.seesScriptBindings(), true }, id, point, values[index] });
                break;
            case Opcode::PropSet:
                if (value.isFreeName())
                    sites.append(BindingSite { { value.key(), true, true }, id, point, values[index], true });
                break;
            default:
                break;
            }
        }
    }
}

static Vector<uint32_t> rankFunctionsByHeat(const Program& program, const Heat& heat)
{
    Vector<uint32_t> ranks(FillWith { }, program.functions.size(), UINT32_MAX);
    std::span<const FunctionID> hottestFirst = heat.hottestFirst();
    for (uint32_t rank = 0; rank < hottestFirst.size(); ++rank) {
        if (hottestFirst[rank].index() < ranks.size())
            ranks[hottestFirst[rank].index()] = rank;
    }
    return ranks;
}

static Vector<FirstUse> findFirstUses(const PlacementContext& context, std::span<const BindingSite> sites)
{
    Vector<uint32_t> ranks = rankFunctionsByHeat(context.program, context.inputs.heat);
    Vector<FirstUse> uses(sites.size(), [&](size_t index) {
        return FirstUse { sites[index].key, ranks[sites[index].function.index()], static_cast<uint32_t>(index) };
    });
    parallelSort(context.build.parallelLoops(), uses.mutableSpan(), [](const FirstUse& a, const FirstUse& b) {
        return std::tie(a.key, a.functionRank, a.siteIndex) < std::tie(b.key, b.functionRank, b.siteIndex);
    });
    removeRepeatedElements(uses, [](const FirstUse& a, const FirstUse& b) {
        return a.key == b.key;
    });
    std::ranges::sort(uses, [](const FirstUse& a, const FirstUse& b) {
        return std::tie(a.functionRank, a.siteIndex) < std::tie(b.functionRank, b.siteIndex);
    });
    return uses;
}

static GlobalBindingID findBinding(std::span<const NumberedBinding> ascendingBindings, const GlobalBindingKey& key)
{
    auto found = std::ranges::lower_bound(ascendingBindings, key, { }, &NumberedBinding::key);
    return found != ascendingBindings.end() && found->key == key ? found->id : GlobalBindingID();
}

static Vector<uint32_t> findFunctionBoundaries(std::span<const BindingSite> sites)
{
    Vector<uint32_t> boundaries;
    for (uint32_t index = 0; index < sites.size(); ++index) {
        if (!index || sites[index].function != sites[index - 1].function)
            boundaries.append(index);
    }
    boundaries.append(static_cast<uint32_t>(sites.size()));
    return boundaries;
}

static void writeBindings(ProgramEditor& editor, const ParallelLoops& loops, std::span<const BindingSite> sites, std::span<const NumberedBinding> ascendingBindings)
{
    Vector<uint32_t> boundaries = findFunctionBoundaries(sites);
    forEachIndexInParallel(loops, boundaries.size() - 1, [&](unsigned group) {
        auto sitesOfFunction = sites.subspan(boundaries[group], boundaries[group + 1] - boundaries[group]);
        Function& function = editor.atForPlacedWords(sitesOfFunction.front().function);
        for (const BindingSite& site : sitesOfFunction)
            function.edit(site.value, ValueEdit::Definition).setBinding(findBinding(ascendingBindings, site.key));
    });
}

static void convertToNamedAccesses(ProgramEditor& editor, const PlacementContext& context, TypeArena& types, std::span<const BindingSite> sites)
{
    Vector<uint32_t> boundaries = findFunctionBoundaries(sites);
    for (size_t group = 0; group + 1 < boundaries.size(); ++group) {
        auto sitesOfFunction = sites.subspan(boundaries[group], boundaries[group + 1] - boundaries[group]);
        FunctionID id = sitesOfFunction.front().function;
        IRBuilder builder(AnalysisContext { context.build, context.program, &context.inputs.flow, nullptr, id }, editor.at(id), types);
        for (const BindingSite& site : sitesOfFunction) {
            convertGlobalBindingToNamedAccess(builder, site.point, site.value);
            context.build.gaveUp(LimitID::MaxGlobalBindings, { .function = id, .value = site.value });
        }
        builder.applyInsertions();
    }
}

GlobalBindingTable placeGlobalBindings(ProgramEditor& editor, const PlacementContext& context, TypeArena& types, const GlobalBindingSitesOfBodies* sitesOfBodies, Vector<FunctionID>* functionsWithOtherOperations)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();

    auto sites = collectInParallel<BindingSite>(loops, program.functions.size(), [&](unsigned index, Vector<BindingSite>& found) {
        if (!program.functions[index].hasBody())
            return;
        if (sitesOfBodies)
            found.appendVector((*sitesOfBodies)(FunctionID(index)));
        else
            collectGlobalBindingSites(program, FunctionID(index), found);
    });

    BitVector keysStoredByName(program.types.numberOfPropertyKeys());
    sites.removeAllMatching([&](const BindingSite& site) {
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
    std::ranges::sort(ascendingBindings, { }, &NumberedBinding::key);

    if (exceedsLimit) {
        Vector<BindingSite> sitesWithoutBinding;
        sites.removeAllMatching([&](const BindingSite& site) {
            if (findBinding(ascendingBindings.span(), site.key))
                return false;
            if (site.key.isStore && site.key.seesScriptBindings)
                keysStoredByName.quickSet(site.key.key.index());
            sitesWithoutBinding.append(site);
            return true;
        });
        convertToNamedAccesses(editor, context, types, sitesWithoutBinding.span());
        if (functionsWithOtherOperations) {
            for (const BindingSite& site : sitesWithoutBinding) {
                if (functionsWithOtherOperations->isEmpty() || functionsWithOtherOperations->last() != site.function)
                    functionsWithOtherOperations->append(site.function);
            }
        }
    }
    writeBindings(editor, loops, sites.span(), ascendingBindings.span());

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

void GlobalBindingTable::dump(PrintStream& out, const Program& program) const
{
    IRPrinter printer(out, program);
    for (unsigned index = 0; index < bindings.size(); ++index) {
        const PlacedGlobalBinding& binding = bindings[index];
        out.print("globalBinding "_s, index, " "_s);
        printer.print(binding.key.key);
        out.print(binding.key.isStore ? " store"_s : " read"_s);
        if (binding.key.seesScriptBindings)
            out.print(" seesScriptBindings"_s);
        if (binding.key.isForTypeOf)
            out.print(" isForTypeOf"_s);
        if (binding.isStoredByName)
            out.print(" isStoredByName"_s);
        out.print("\n"_s);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
