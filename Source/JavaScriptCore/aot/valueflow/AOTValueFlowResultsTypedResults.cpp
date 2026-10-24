/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTCFG.h"
#include "AOTFlowRecords.h"
#include "AOTFlowSiteData.h"
#include "AOTFlowStamps.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTUseLists.h"
#include "AOTValueFlowPositions.h"
#include "AOTValueFlowTypedResults.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static BitVector findFilledArraysOfTypedResults(const ValueFlowResultInputs& inputs)
{
    const ValueFlowUniverse& universe = inputs.universe();
    RELEASE_ASSERT(inputs.heapIndex);
    Vector<ValueFlowHeapIndex::Stretch> stretches = inputs.heapIndex->stretchesOfNodesWrittenUnder([](ValueFlowName name) {
        return name.kind == ValueFlowNameKind::Elements;
    });
    Vector<uint32_t> filled = collectInParallel<uint32_t>(inputs.loops(), stretches.size(), [&](unsigned stretch, Vector<uint32_t>& found) {
        for (const ValueFlowHeapIndex::WrittenNode& node : stretches[stretch].nodes) {
            if (ValueSets::isComplement(node.objects))
                continue;
            ValueFlowObjectID object = inputs.sets().onlyMember(node.objects);
            const ValueFlowSite* site = object ? universe.siteOf(object) : nullptr;
            if (site && site->origin == ValueFlowSiteOrigin::TypedResult)
                found.append(object.index());
        }
    });
    BitVector isFilled;
    for (uint32_t object : filled)
        isFilled.set(object);
    return isFilled;
}

void computeTypedResultResults(const ValueFlowResultInputs& inputs, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    FlowApplication& application = *tables.application;

    inputs.sets().forEachMember(inputs.solver().valueOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::RecordsMadeByParsers)), [&](ValueFlowObjectID record) {
        application.layouts[universe.layoutOf(record).index()].flow.observedUses.add(ObservedUse::ReachesTypedParse);
    });

    auto parsesTypedResult = [&](const ValueFlowStatement& statement) {
        return statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::Call && universe.siteObject(statement, ValueFlowTypedResult::firstOrdinal);
    };
    Vector<const ValueFlowStatement*> calls;
    for (uint32_t index : findIndicesInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        return inputs.isReached(FunctionID(index)) && std::ranges::any_of(inputs.bodies().statementsOf(index), parsesTypedResult);
    })) {
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (parsesTypedResult(statement))
                calls.append(&statement);
        }
    }
    if (calls.isEmpty())
        return;

    BitVector isFilled = findFilledArraysOfTypedResults(inputs);
    size_t numberOfOtherSites = tables.sites.allocationSites.size();
    tables.sites.typedResultSites.fill(AllocationSiteID(), program.layouts.size());
    FunctionID functionOfAccessSites;
    Vector<AccessSiteID> accessSites;
    FunctionID functionOfUses;
    std::optional<CFG> cfg;
    std::optional<UseLists> uses;
    for (const ValueFlowStatement* call : calls) {
        const Function& function = program.at(call->function);
        if (functionOfUses != call->function) {
            functionOfUses = call->function;
            cfg.emplace(function);
            uses.emplace(function, *cfg);
        }
        ValueFlowTypedResult result = findTypedResult(program, universe.builtinTables(), function, *uses, call->site);
        RELEASE_ASSERT(result);
        if (!isFilled.get(universe.siteObject(*call, result.ordinalOfPlainArray()).index()))
            continue;

        if (functionOfAccessSites != call->function) {
            functionOfAccessSites = call->function;
            accessSites = accessSitesByValue(function, application.accessSites, call->function);
        }
        if (AccessSiteID accessSite = accessSites[call->site.index()])
            tables.sites.typedResultCalls.append({ accessSite, result.type, result.layouts });

        for (LayoutID layout : result.layouts) {
            application.layouts[layout.index()].flow.observedUses.add({ ObservedUse::ReachesTypedParse, ObservedUse::IsParsedByType });
            const Vector<AllocationClassID>& classes = application.layouts[layout.index()].flow.allocationClasses;
            AllocationSiteID& site = tables.sites.typedResultSites[layout.index()];
            if (site || classes.isEmpty())
                continue;
            site = AllocationSiteID(tables.sites.allocationSites.size());
            tables.sites.allocationSites.append(AllocationSite { call->function, layout, { }, classes[0], function.locationOf(call->site) });
        }
    }
    AOT::sort(tables.sites.typedResultCalls, { }, &FlowTypedResultCall::call);
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size() - numberOfOtherSites);
}

static constexpr OptionSet<ObjectMutation> mutationsOfArraysWithoutMark {
    ObjectMutation::ChangePrototype,
    ObjectMutation::BecomePrototype,
    ObjectMutation::StoreOfOtherType,
};

void computeDataNodeResults(const ValueFlowResultInputs& inputs, const ValueFlowContainerResults& containers, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    FlowApplication& application = *tables.application;
    unsigned indexOfElements = indexOfArrayElementsInType(program.builtins());
    size_t numberOfOtherSites = tables.sites.allocationSites.size();
    std::array<AllocationClassID, 2> classesOfArrays;
    size_t numberOfOtherNodes = tables.sites.dataNodes.size();
    ValueFlowPositionCounts counts(nullptr);

    struct Node {
        ValueID operation;
        uint32_t ordinal;
        LayoutID layout;
        BuiltinClassID builtinClass;
        AllocationClassID classOfLayout;
        OptionSet<ObjectMutation> markedMutations;
        bool neverChanges;
        TypeID elements;
        TypeID type;
        ArrayStorage arrayStorage { ArrayStorage::Other };
    };
    const ParallelLoops& loops = inputs.loops();
    Vector<uint32_t> functions = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return inputs.isReached(FunctionID(index)) && std::ranges::any_of(inputs.bodies().statementsOf(index), [](const ValueFlowStatement& statement) {
            return statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::Data;
        });
    });
    TypeInterningPhase phase(types, functions.size());
    Vector<Vector<Node>> nodesOfFunction(functions.size());
    forEachIndexInParallel(loops, functions.size(), [&](unsigned task) {
        uint32_t index = functions[task];
        const Function& function = program.functions[index];
        TypeArena arena(phase, task);
        std::optional<ValueFlowPositions> positions;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Data)
                continue;
            forEachDataNodeInPreorder(program.at(function.at(statement.site).data()).root, [&](uint32_t ordinal, const DataNode& dataNode) {
                if (!dataNode.is<DataNode::Array>() && !dataNode.is<DataNode::Object>())
                    return;
                ValueFlowObjectID object = universe.siteObject(statement, ordinal);
                RELEASE_ASSERT(object);
                const ValueFlowSite& site = *universe.siteOf(object);
                Node node { statement.site, ordinal, site.layout, site.builtinClass, { }, { }, false, { }, { } };
                if (site.layout) {
                    const Vector<AllocationClassID>& classes = application.layouts[site.layout.index()].flow.allocationClasses;
                    if (!classes.isEmpty())
                        node.classOfLayout = classes[0];
                } else {
                    node.markedMutations = markedMutationsOf(inputs, object);
                    node.neverChanges = neverChangesOnceMade(inputs, object);
                    const ValueFlowContainerFacts* facts = containers.find(object);
                    if (facts && indexOfElements < facts->contents.size() && facts->contents[indexOfElements] != TypeTable::never)
                        node.elements = facts->contents[indexOfElements];
                    if (facts)
                        node.arrayStorage = facts->arrayStorageToldToCreator;
                    if (facts && ordinal && (facts->contentsAreProven || !facts->pristine.isEmpty())) {
                        if (!positions)
                            positions.emplace(inputs, arena, counts, index);
                        node.type = arena.intern(positions->typeOfObject(object));
                    }
                }
                nodesOfFunction[task].append(node);
            });
        }
    });

    auto classOfArray = [&](const Node& node) -> AllocationClassID& {
        return classesOfArrays[node.markedMutations.contains(ObjectMutation::StoreAfterCreation)];
    };
    Vector<uint32_t> firstSiteOfFunction;
    firstSiteOfFunction.reserveInitialCapacity(functions.size());
    uint32_t numberOfSites = static_cast<uint32_t>(numberOfOtherSites);
    for (const Vector<Node>& nodes : nodesOfFunction) {
        firstSiteOfFunction.append(numberOfSites);
        for (const Node& node : nodes) {
            uint32_t site = numberOfSites++;
            if (node.layout)
                continue;
            AllocationClassID& classOfArrays = classOfArray(node);
            if (!classOfArrays) {
                classOfArrays = AllocationClassID(tables.sites.allocationClasses.size());
                AllocationClass arrays;
                arrays.builtinClass = BuiltinClasses::Array;
                arrays.impossibleMutations = allObjectMutations - mutationsOfArraysWithoutMark;
                tables.sites.allocationClasses.append(WTF::move(arrays));
            }
            tables.sites.allocationClasses[classOfArrays.index()].impossibleMutations.remove(node.markedMutations);
            if (node.neverChanges && inputs.build.isEnabled(FlowResultKind::HolderMutations, node.builtinClass.index()))
                tables.sites.sitesOfObjectsThatNeverChange.set(site);
        }
    }
    tables.sites.allocationSites.grow(numberOfSites);
    tables.sites.dataNodes.grow(numberOfOtherNodes + (numberOfSites - numberOfOtherSites));
    TypeRemap remap = phase.finish(StringRemap { }, &loops);
    forEachIndexInParallel(loops, functions.size(), [&](unsigned task) {
        uint32_t index = functions[task];
        const Function& function = program.functions[index];
        Vector<FlowAllocationStamp>& stamps = application.bodies[index].allocations;
        size_t numberOfOtherStamps = stamps.size();
        uint32_t nextSite = firstSiteOfFunction[task];
        ValueID operation;
        AllocationSiteID rootSite;
        for (const Node& node : nodesOfFunction[task]) {
            if (node.operation != operation) {
                operation = node.operation;
                rootSite = { };
            }
            FlowDataNodeStamp stamp { AllocationSiteID(nextSite++), node.layout ? node.classOfLayout : classOfArray(node), node.elements, node.arrayStorage, node.type };
            remap.apply(stamp.type);
            tables.sites.allocationSites[stamp.site.index()] = AllocationSite { FunctionID(index), node.layout, node.builtinClass, stamp.allocationClass, function.locationOf(node.operation) };
            if (!node.ordinal) {
                rootSite = stamp.site;
                stamps.append(FlowAllocationStamp { .value = node.operation, .site = rootSite, .allocationClass = stamp.allocationClass });
            }
            tables.sites.dataNodes[numberOfOtherNodes + (stamp.site.index() - numberOfOtherSites)] = { rootSite, node.ordinal, stamp };
        }
        if (stamps.size() != numberOfOtherStamps)
            AOT::sort(stamps, { }, &FlowAllocationStamp::value);
    });

    for (AllocationClassID classOfArrays : classesOfArrays) {
        if (!classOfArrays)
            continue;
        AllocationClass& arrays = tables.sites.allocationClasses[classOfArrays.index()];
        if (!inputs.build.isEnabled(FlowResultKind::HolderMutations, BuiltinClasses::Array.index()))
            arrays.impossibleMutations = { };
        inputs.build.statistics().count(StatisticID::FlowAllocationClasses, 1);
    }
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size() - numberOfOtherSites);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
