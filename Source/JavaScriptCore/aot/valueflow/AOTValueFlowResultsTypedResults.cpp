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
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTUseLists.h"
#include "AOTValueFlowPositions.h"
#include "AOTValueFlowTypedResults.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// The rule of a built-in fills the array that stands for what the generic parser makes whenever it allocates a typed result, and nothing else stores into an array that no set holds.
static BitVector findFilledArraysOfTypedResults(const ValueFlowResultInputs& inputs)
{
    const ValueFlowSolver& solver = inputs.solver();
    const ValueFlowUniverse& universe = inputs.universe();
    constexpr uint32_t nodesPerShare = 1 << 14;
    uint32_t numberOfNodes = solver.graph().numberOfNodes();
    Vector<Vector<uint32_t>> filledOfShare((numberOfNodes + nodesPerShare - 1) / nodesPerShare);
    forEachIndexInParallel(inputs.loops(), filledOfShare.size(), [&](unsigned share) {
        for (uint32_t index = share * nodesPerShare; index < std::min(numberOfNodes, (share + 1) * nodesPerShare); ++index) {
            const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(index));
            if (!key || key->kind != ValueFlowMadeNodeKind::Written || key->name().kind != ValueFlowNameKind::Elements || ValueSets::isComplement(key->set))
                continue;
            if (solver.valueOf(ValueFlowNodeID(index)) == ValueSets::nothing)
                continue;
            ValueFlowObjectID object = inputs.sets().onlyMember(key->set);
            const ValueFlowSite* site = object ? universe.siteOf(object) : nullptr;
            if (site && site->origin == ValueFlowSiteOrigin::TypedResult)
                filledOfShare[share].append(object.index());
        }
    });
    BitVector isFilled;
    for (const Vector<uint32_t>& filled : filledOfShare) {
        for (uint32_t object : filled)
            isFilled.set(object);
    }
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

        // One site for each layout: whatever parser or validator allocates the layout, for whichever call.
        for (LayoutID layout : result.layouts) {
            application.layouts[layout.index()].flow.observedUses.add(ObservedUse::ReachesTypedParse);
            const Vector<AllocationClassID>& classes = application.layouts[layout.index()].flow.allocationClasses;
            AllocationSiteID& site = tables.sites.typedResultSites[layout.index()];
            if (site || classes.isEmpty())
                continue;
            site = AllocationSiteID(tables.sites.allocationSites.size());
            tables.sites.allocationSites.append(AllocationSite { call->function, layout, { }, classes[0], function.locationOf(call->site) });
        }
    }
    std::ranges::sort(tables.sites.typedResultCalls, { }, &FlowTypedResultCall::call);
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size() - numberOfOtherSites);
}

static unsigned indexOfArrayElementsInType(const BuiltinTable& builtins)
{
    std::optional<unsigned> index;
    for (BuiltinSlotID id : builtins.at(BuiltinClasses::Array).slots) {
        const BuiltinSlot& slot = builtins.at(id);
        if (slot.indexInType && slot.contentsSlot == ContentsSlot::Element)
            index = *slot.indexInType;
    }
    return index.value();
}

// No mark says which prototype an array is created with, what becomes a prototype by a construction, or what is stored into a slot of native code.
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
    // What holds for a class holds for every array of it in the tree of a Data value. The arrays that no store reaches have a class of their own.
    std::array<AllocationClassID, 2> classesOfArrays;
    size_t numberOfOtherNodes = tables.sites.dataNodes.size();
    TypeInterningPhase phase(types, 1);
    std::optional<TypeArena> arena;
    arena.emplace(phase, 0);
    ValueFlowPositionCounts counts(nullptr);

    for (uint32_t index : findIndicesInParallel(inputs.loops(), program.functions.size(), [&](unsigned index) {
        return inputs.isReached(FunctionID(index)) && std::ranges::any_of(inputs.bodies().statementsOf(index), [](const ValueFlowStatement& statement) {
            return statement.kind == ValueFlowStatementKind::Operation && statement.opcode == Opcode::Data;
        });
    })) {
        const Function& function = program.functions[index];
        std::optional<ValueFlowPositions> positions;
        Vector<FlowAllocationStamp>& stamps = application.bodies[index].allocations;
        size_t numberOfOtherStamps = stamps.size();
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Data)
                continue;

            AllocationSiteID rootSite;
            forEachDataNodeInPreorder(program.at(function.at(statement.site).data()).root, [&](uint32_t ordinal, const DataNode& node) {
                if (!node.is<DataNode::Array>() && !node.is<DataNode::Object>())
                    return;
                ValueFlowObjectID object = universe.siteObject(statement, ordinal);
                RELEASE_ASSERT(object);
                const ValueFlowSite& site = *universe.siteOf(object);

                FlowDataNodeStamp stamp { AllocationSiteID(tables.sites.allocationSites.size()), { }, { }, { } };
                if (site.layout) {
                    const Vector<AllocationClassID>& classes = application.layouts[site.layout.index()].flow.allocationClasses;
                    if (!classes.isEmpty())
                        stamp.allocationClass = classes[0];
                } else {
                    AllocationClassID& classOfArrays = classesOfArrays[markedMutationsOf(inputs, object).contains(ObjectMutation::StoreAfterCreation)];
                    if (!classOfArrays) {
                        classOfArrays = AllocationClassID(tables.sites.allocationClasses.size());
                        AllocationClass arrays;
                        arrays.builtinClass = BuiltinClasses::Array;
                        arrays.impossibleMutations = allObjectMutations - mutationsOfArraysWithoutMark;
                        tables.sites.allocationClasses.append(WTF::move(arrays));
                    }
                    const ValueFlowContainerFacts* facts = containers.find(object);
                    AllocationClass& arrays = tables.sites.allocationClasses[classOfArrays.index()];
                    arrays.impossibleMutations.remove(markedMutationsOf(inputs, object));
                    stamp.allocationClass = classOfArrays;
                    // Where nothing is ever stored, the record says best which storage to begin with.
                    if (facts && indexOfElements < facts->contents.size() && facts->contents[indexOfElements] != TypeTable::never)
                        stamp.elements = facts->contents[indexOfElements];
                    // The value of the operation gets a cast to what is proven of it. An array further down in the tree has no value until the tree is expanded.
                    if (facts && ordinal && (facts->contentsAreProven || !facts->pristine.isEmpty())) {
                        if (!positions)
                            positions.emplace(inputs, *arena, counts, index);
                        stamp.type = arena->intern(positions->typeOfObject(object));
                    }
                }
                tables.sites.allocationSites.append(AllocationSite { FunctionID(index), site.layout, site.builtinClass, stamp.allocationClass, function.locationOf(statement.site) });
                if (!site.layout && neverChangesOnceMade(inputs, object) && inputs.build.isEnabled(FlowResultKind::HolderMutations, site.builtinClass.index()))
                    tables.sites.sitesOfObjectsThatNeverChange.set(stamp.site.index());
                if (!ordinal) {
                    rootSite = stamp.site;
                    stamps.append(FlowAllocationStamp { .value = statement.site, .site = rootSite, .allocationClass = stamp.allocationClass });
                }
                tables.sites.dataNodes.append({ rootSite, ordinal, stamp });
            });
        }
        if (stamps.size() != numberOfOtherStamps)
            std::ranges::sort(stamps, { }, &FlowAllocationStamp::value);
    }

    arena.reset();
    TypeRemap remap = phase.finish(StringRemap { });
    for (FlowDataNode& node : tables.sites.dataNodes.mutableSpan().subspan(numberOfOtherNodes))
        remap.apply(node.stamp.type);

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
