/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrderInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreads.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

bool FlowScopeOwnerAnalysis::completesBeforeReturn(FlowScopeFrame& frame, ValueID value)
{
    FlowScopeEntry exit = exitOf(frame);
    switch (exit.state) {
    case FlowScopeEntryState::Unknown:
        return false;
    case FlowScopeEntryState::AfterPoint:
        return frame.analyses.dominates(value, exit.point);
    case FlowScopeEntryState::Never:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Seen from a caller, a store has run when the call returns, and a function that is entered during the call may be entered as soon as the call starts.
bool FlowScopeOwnerAnalysis::precedes(const FlowScopeLocationAccess& store, const FlowScopeEntry& entry)
{
    switch (entry.state) {
    case FlowScopeEntryState::Unknown:
        return false;
    case FlowScopeEntryState::AfterPoint:
        break;
    case FlowScopeEntryState::Never:
        return true;
    }

    FlowScopeFrame* frame = findFrame(store.function);
    RELEASE_ASSERT(frame);
    ValueID completed = store.value;
    FlowScopeEntry entered = entry;
    while (frameOf(entered).depth > frame->depth)
        entered = entryInCaller(entered);
    while (frame->function != entered.function) {
        if (!completesBeforeReturn(*frame, completed))
            return false;
        completed = frame->call;
        frame = frame->caller;
        if (frameOf(entered).depth > frame->depth)
            entered = entryInCaller(entered);
    }
    return frame->analyses.dominates(completed, entered.point);
}

bool FlowScopeOwnerAnalysis::anyPrecedes(const InitializingStores& stores, const FlowScopeEntry& entry)
{
    return std::ranges::any_of(stores, [&](const FlowScopeLocationAccess& store) {
        return precedes(store, entry);
    });
}

std::span<const FlowScopeLocationAccess> FlowScopeOwnerAnalysis::definitionsOf(FlowScopeLocation location) const
{
    return accessesAt(m_definitions.span(), location);
}

// An environment is followed in the body of its owner only.
auto FlowScopeOwnerAnalysis::initializingStoresOf(FlowScopeLocation location) -> InitializingStores
{
    bool isOneLocation = m_index.numbering.isOneLocation(location);
    InitializingStores stores;
    for (const FlowScopeLocationAccess& store : accessesAt(m_index.stores.span(), location)) {
        if (store.function == m_owner.function || (isOneLocation && findFrame(store.function)))
            stores.append(store);
    }
    stores.append(definitionsOf(location));
    return stores;
}

bool FlowScopeOwnerAnalysis::isDefinedAtEveryAccessThroughClassObject(FlowScopeLocation location, const InitializingStores& stores)
{
    // All evaluations of a class share the variables of its static fields, and a definition says nothing about the class object of another evaluation.
    LayoutID owningClass = m_index.program().variables[location].owner;
    if (!m_classesEvaluatedOnce.get(owningClass.index()))
        return false;

    // Outside the frames nobody holds the class object, or an object that inherits from it, before somebody can reach the constructor.
    bool isDefinedWhenHeld = anyPrecedes(stores, entryByUseOf(m_index.program().at(owningClass).constructor));
    if (m_index.staticFieldsOfEscapedClasses.get(location) && !isDefinedWhenHeld)
        return false;

    auto definitions = definitionsOf(location);
    return std::ranges::all_of(accessesAt(m_index.accessesThroughClassObjects.span(), location), [&](const FlowScopeLocationAccess& access) {
        if (std::ranges::binary_search(definitions, access))
            return true;
        if (isDefinedWhenHeld && !findFrame(access.function))
            return true;
        return anyPrecedes(stores, entryOf(access)) || anyPrecedes(stores, entryBySubjectOf(access.function, access.value));
    });
}

InitializationState FlowScopeOwnerAnalysis::initializationOf(FlowScopeLocation location, bool isSetAtCreation)
{
    if (isSetAtCreation)
        return InitializationState::SetAtCreation;
    InitializingStores stores = initializingStoresOf(location);
    bool isEveryReadCovered = true;
    forEachReader(location, [&](const FlowScopeLocationAccess& read) {
        isEveryReadCovered = isEveryReadCovered && anyPrecedes(stores, entryOf(read));
    });
    if (isExported(location))
        isEveryReadCovered = isEveryReadCovered && anyPrecedes(stores, m_hostEntry);
    bool isStaticField = location < m_index.numbering.numberOfVariables && m_index.program().variables[location].owner;
    if (isStaticField)
        isEveryReadCovered = isEveryReadCovered && isDefinedAtEveryAccessThroughClassObject(location, stores);
    return isEveryReadCovered ? InitializationState::StoredBeforeEveryRead : InitializationState::Unknown;
}

StoreCount FlowScopeOwnerAnalysis::storeCountOf(FlowScopeLocation location, bool isSetAtCreation, NOESCAPE const Invocable<bool(const FlowScopeLocationAccess&)> auto& canRunTwice)
{
    auto stores = accessesAt(m_index.stores.span(), location);
    auto storesThroughClassObjects = accessesAt(m_index.storesThroughClassObjects.span(), location);
    size_t numberOfStores = stores.size() + storesThroughClassObjects.size();
    if (!numberOfStores)
        return isSetAtCreation ? StoreCount::AtMostOnce : StoreCount::Never;
    if (isSetAtCreation || numberOfStores != 1)
        return StoreCount::Unknown;
    return canRunTwice(stores.empty() ? storesThroughClassObjects.front() : stores.front()) ? StoreCount::Unknown : StoreCount::AtMostOnce;
}

bool FlowScopeOwnerAnalysis::canRunTwice(const FlowScopeLocationAccess& store)
{
    ValueID value = store.value;
    FlowScopeFrame* frame = findFrame(store.function);
    if (!frame)
        return true;
    for (; frame; value = frame->call, frame = frame->caller) {
        auto point = pointOf(*frame, value);
        if (!point || frame->analyses.blockCycles().isInCycle(point->block))
            return true;
    }
    return false;
}

static bool isModuleBody(const Function& function)
{
    return function.kind == FunctionKind::Module || function.kind == FunctionKind::CommonJsModule;
}

// One listed store that runs at most once, into a holder that is created at most once: both in one evaluation of a module. An evaluation that follows one that threw creates its own holder.
StoreCount FlowScopeOwnerAnalysis::storeCountOfMember(FlowScopeLocation location)
{
    auto stores = accessesAt(m_index.stores.span(), location);
    if (!isModuleBody(m_index.program().at(m_owner.function)) || stores.size() != 1 || canRunTwice(stores.front()))
        return StoreCount::Unknown;

    const FlowObjectTable& objects = m_index.inputs.objects();
    const FlowObject* holder = &objects.at(m_index.numbering.members[location - m_index.numbering.firstMember()].holder);
    if (holder->kind == FlowObjectKind::Prototype)
        holder = &objects.at(holder->owner);
    if (!holder->site || holder->isCopy())
        return StoreCount::Unknown;
    const FlowSite& site = objects.at(holder->site);
    // The host creates the function object of a declaration once, with its module.
    if (m_index.inputs.isHost(site.function))
        return StoreCount::AtMostOnce;
    if (site.description.origin != FlowSiteOrigin::Operation || !site.description.value)
        return StoreCount::Unknown;
    return canRunTwice({ location, site.function, site.description.value }) ? StoreCount::Unknown : StoreCount::AtMostOnce;
}

bool FlowScopeOwnerAnalysis::canRunTwicePerInstance(const FlowScopeLocationAccess& store, BlockID allocation)
{
    if (store.function != m_owner.function)
        return true;
    auto point = pointOf(ownerFrame(), store.value);
    return !point || ownerFrame().analyses.cfg().isInCycleAvoiding(point->block, allocation);
}

std::optional<BlockID> FlowScopeOwnerAnalysis::trackedAllocationBlock(LayoutID layout, std::span<const FlowEnvironmentAllocation> sites)
{
    if (sites.size() != 1)
        return std::nullopt;
    FlowScopeFrame& frame = ownerFrame();
    auto point = pointOf(frame, sites.front().value);
    if (!point)
        return std::nullopt;
    auto isOwnAccessOfAllocation = [&](const FlowScopeLocationAccess& access) {
        return frame.analyses.sameObjectThroughCasts(frame.body.at(access.value).environmentOperand()) == sites.front().value;
    };
    uint32_t end = m_index.numbering.firstField[layout.index() + 1];
    for (FlowScopeLocation location = m_index.numbering.firstField[layout.index()]; location < end; ++location) {
        auto ownReads = recordsWith(accessesAt(m_index.reads.span(), location), m_owner.function, &FlowScopeLocationAccess::function);
        auto ownStores = recordsWith(accessesAt(m_index.stores.span(), location), m_owner.function, &FlowScopeLocationAccess::function);
        if (!std::ranges::all_of(ownReads, isOwnAccessOfAllocation) || !std::ranges::all_of(ownStores, isOwnAccessOfAllocation))
            return std::nullopt;
    }
    return point->block;
}

void FlowScopeOwnerAnalysis::computeEnvironmentOrders(LayoutID layout, std::span<FlowLocationOrder> orders)
{
    auto sites = allocationsOf(m_index, layout);
    auto allocationBlock = trackedAllocationBlock(layout, sites);
    uint32_t numberOfFields = m_index.program().at(layout).fields.size();
    Vector<uint32_t> allocationsListing(FillWith { }, numberOfFields, 0u);
    for (const FlowEnvironmentAllocation& site : sites) {
        for (uint32_t field : ownerFrame().body.at(site.value).fields())
            ++allocationsListing[field];
    }
    for (uint32_t field = 0; field < numberOfFields; ++field) {
        FlowScopeLocation location = *m_index.numbering.locationOf(layout, field);
        if (m_index.opaqueLocations.get(location))
            continue;
        bool isSetAtCreation = allocationsListing[field] == sites.size();
        if (!allocationBlock) {
            bool hasNoStore = accessesAt(m_index.stores.span(), location).empty();
            orders[location] = {
                isSetAtCreation ? InitializationState::SetAtCreation : InitializationState::Unknown,
                isSetAtCreation && hasNoStore ? StoreCount::AtMostOnce : StoreCount::Unknown,
            };
            continue;
        }
        orders[location] = {
            initializationOf(location, isSetAtCreation),
            storeCountOf(location, isSetAtCreation, [&](const FlowScopeLocationAccess& store) {
                return canRunTwicePerInstance(store, *allocationBlock);
            }),
        };
    }
}

void FlowScopeOwnerAnalysis::computeOrders(std::span<FlowLocationOrder> orders)
{
    for (VariableID id : m_owner.variables) {
        FlowScopeLocation location = id.index();
        if (m_index.opaqueLocations.get(location))
            continue;
        bool isSetAtCreation = m_index.program().variables[location].initialization == VariableInitialization::Function;
        orders[location] = {
            initializationOf(location, isSetAtCreation),
            storeCountOf(location, isSetAtCreation, [&](const FlowScopeLocationAccess& store) {
                return canRunTwice(store);
            }),
        };
    }
    for (LayoutID layout : m_owner.environments)
        computeEnvironmentOrders(layout, orders);
    for (FlowScopeLocation location : m_owner.members) {
        if (!m_index.opaqueLocations.get(location))
            orders[location] = { initializationOf(location, false), storeCountOfMember(location) };
    }
}

static bool runsAfterCompletion(const FlowScopeIndex& index, FunctionID function, ModuleID completed)
{
    ModuleID module = index.program().at(function).module;
    return module && index.moduleOrder.orderingOf(completed, module) == FlowModuleOrdering::CompletesBefore;
}

static bool canObserveUnfinishedModule(const Program& program, const FlowModuleValueRead& read)
{
    Opcode opcode = program.at(read.function).at(read.value).opcode;
    // The promise of an import is resolved with the namespace object after the evaluation has completed.
    if (opcode == Opcode::ImportModule)
        return false;
    if (opcode == Opcode::RequireModule)
        return program.at(read.module).requireResult != RequireResult::ModuleExports;
    return true;
}

static BitVector findVariablesReadEarly(const FlowScopeIndex& index)
{
    const Program& program = index.program();
    BitVector variables(program.variables.size());
    BitVector isVisited(program.modules.size());
    Vector<ModuleID> worklist;
    auto visit = [&](ModuleID module) {
        if (!isVisited.set(module.index()))
            worklist.append(module);
    };

    for (const FlowModuleValueRead& read : index.moduleValueReads) {
        if (canObserveUnfinishedModule(program, read) && !runsAfterCompletion(index, read.function, read.module))
            visit(read.module);
    }
    while (!worklist.isEmpty()) {
        const Module& module = program.at(worklist.takeLast());
        for (const SyntheticExport& entry : module.syntheticExports) {
            if (entry.variable)
                variables.set(entry.variable.index());
        }
        for (const ModuleExport& entry : module.exports) {
            switch (entry.kind) {
            case ModuleExportKind::Variable:
                if (entry.variable)
                    variables.set(entry.variable.index());
                break;
            case ModuleExportKind::Namespace:
                if (entry.module)
                    visit(entry.module);
                break;
            case ModuleExportKind::Indirect:
                break;
            }
        }
    }
    return variables;
}

void computeSyntheticExportOrders(const FlowScopeIndex& index, std::span<FlowLocationOrder> orders)
{
    if (index.anyModuleValueMayBeRead)
        return;
    const Program& program = index.program();
    BitVector variablesReadEarly = findVariablesReadEarly(index);
    for (size_t synthetic : index.syntheticVariables) {
        FlowScopeLocation variable = static_cast<FlowScopeLocation>(synthetic);
        if (index.opaqueLocations.get(variable))
            continue;
        ModuleID module = program.variables[variable].module;
        bool isReadAfterCompletion = !variablesReadEarly.get(variable) && std::ranges::all_of(accessesAt(index.reads.span(), variable), [&](const FlowScopeLocationAccess& read) {
            return runsAfterCompletion(index, read.function, module);
        });
        orders[variable] = {
            isReadAfterCompletion ? InitializationState::StoredBeforeEveryRead : InitializationState::Unknown,
            accessesAt(index.stores.span(), variable).empty() ? StoreCount::AtMostOnce : StoreCount::Unknown,
        };
    }
}

FlowScopeOrders computeScopeOrders(const FlowOrderInputs& inputs, const FlowModuleOrder& moduleOrder, FlowScopeAccessesByFunction& byFunction)
{
    FlowScopeLocationNumbering numbering = createScopeLocationNumbering(inputs.program());
    numberMemberLocations(inputs, numbering);
    ensureScopeAccesses(inputs, numbering, byFunction);
    FlowScopeIndex index = createScopeIndex(inputs, moduleOrder, byFunction, WTF::move(numbering));
    Vector<FlowLocationOrder> orders(index.numbering.numberOfLocations());
    Vector<FlowScopeTimeline> timelines(index.owners.size());
    forEachIndexInParallel(inputs.build().parallelLoops(), index.owners.size(), [&](unsigned owner) {
        FlowScopeOwnerAnalysis analysis(index, index.owners[owner]);
        analysis.computeEntries();
        analysis.computeOrders(orders.mutableSpan());
        timelines[owner] = analysis.createTimeline();
    });
    computeSyntheticExportOrders(index, orders.mutableSpan());
    for (auto [location, assumed] : index.assumedStores) {
        if (orders[assumed].initialization != InitializationState::StoredBeforeEveryRead)
            orders[location] = { };
    }

    return {
        WTF::move(index.numbering),
        WTF::move(orders),
        WTF::move(index.locationOwners),
        WTF::move(index.environmentOwners),
        WTF::move(timelines),
    };
}

std::optional<uint32_t> FlowScopeLocationNumbering::locationOf(VariableID variable) const
{
    if (variable.index() >= numberOfVariables)
        return std::nullopt;
    return variable.index();
}

std::optional<uint32_t> FlowScopeLocationNumbering::locationOf(LayoutID environment, uint32_t fieldIndex) const
{
    if (!environment || environment.index() + 1 >= firstField.size())
        return std::nullopt;
    uint32_t first = firstField[environment.index()];
    if (fieldIndex >= firstField[environment.index() + 1] - first)
        return std::nullopt;
    return first + fieldIndex;
}

std::optional<uint32_t> FlowScopeLocationNumbering::locationOf(FlowObjectID holder, PropertyKeyID key) const
{
    FlowMemberLocation member { holder, key };
    auto found = std::ranges::lower_bound(members, member);
    if (found == members.end() || *found != member)
        return std::nullopt;
    return firstMember() + static_cast<uint32_t>(found - members.begin());
}

FlowScopeEntry FlowScopeTimeline::entryOf(FunctionID function) const
{
    auto entry = std::ranges::lower_bound(entries, function, { }, &std::pair<FunctionID, FlowScopeEntry>::first);
    if (entry == entries.end() || entry->first != function)
        return unknownScopeEntry;
    return entry->second;
}

FlowLocationOrder FlowScopeOrders::orderOf(VariableID variable) const
{
    auto location = numbering.locationOf(variable);
    return location ? orders[*location] : FlowLocationOrder { };
}

FlowLocationOrder FlowScopeOrders::orderOf(LayoutID environment, uint32_t fieldIndex) const
{
    auto location = numbering.locationOf(environment, fieldIndex);
    return location ? orders[*location] : FlowLocationOrder { };
}

FlowLocationOrder FlowScopeOrders::orderOf(FlowObjectID holder, PropertyKeyID key) const
{
    auto location = numbering.locationOf(holder, key);
    return location ? orders[*location] : FlowLocationOrder { };
}

const FlowScopeTimeline* FlowScopeOrders::timelineOf(FunctionID owner) const
{
    auto timeline = std::ranges::lower_bound(timelines, owner, { }, &FlowScopeTimeline::owner);
    if (timeline == timelines.end() || timeline->owner != owner)
        return nullptr;
    return &*timeline;
}

static void dumpEntry(PrintStream& out, FunctionID owner, const FlowScopeEntry& entry)
{
    switch (entry.state) {
    case FlowScopeEntryState::Unknown:
        out.print("unknown");
        return;
    case FlowScopeEntryState::AfterPoint:
        out.print("after ");
        if (entry.function != owner)
            out.print("f", entry.function.index(), ":");
        out.print("b", entry.point.block.index(), "#", entry.point.index);
        return;
    case FlowScopeEntryState::Never:
        out.print("never");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpEntries(PrintStream& out, FunctionID owner, const Vector<std::pair<FunctionID, FlowScopeEntry>>& entries)
{
    CommaPrinter comma(", "_s, " "_s);
    for (auto& [function, entry] : entries) {
        out.print(comma, "f", function.index(), " ");
        dumpEntry(out, owner, entry);
    }
    out.println();
}

static void dumpOrder(PrintStream& out, const FlowLocationOrder& order)
{
    out.println(": ", nameOf(order.initialization), " ", nameOf(order.stores));
}

void FlowScopeOrders::dump(PrintStream& out) const
{
    for (const FlowScopeTimeline& timeline : timelines) {
        out.print("timeline f", timeline.owner.index(), ":");
        dumpEntries(out, timeline.owner, timeline.entries);
        if (timeline.entriesByUse == timeline.entries)
            continue;
        out.print("timeline f", timeline.owner.index(), " by use:");
        dumpEntries(out, timeline.owner, timeline.entriesByUse);
    }
    for (uint32_t variable = 0; variable < numbering.numberOfVariables; ++variable) {
        if (orders[variable] == FlowLocationOrder { })
            continue;
        out.print("g", variable);
        dumpOrder(out, orders[variable]);
    }
    for (uint32_t layout = 0; layout + 1 < numbering.firstField.size(); ++layout) {
        uint32_t first = numbering.firstField[layout];
        for (uint32_t location = first; location < numbering.firstField[layout + 1]; ++location) {
            if (orders[location] == FlowLocationOrder { })
                continue;
            out.print("L", layout, ".", location - first);
            dumpOrder(out, orders[location]);
        }
    }
    for (uint32_t member = 0; member < numbering.members.size(); ++member) {
        const FlowLocationOrder& order = orders[numbering.firstMember() + member];
        if (order == FlowLocationOrder { })
            continue;
        out.print("o", numbering.members[member].holder.index(), ".k", numbering.members[member].key.index());
        dumpOrder(out, order);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
