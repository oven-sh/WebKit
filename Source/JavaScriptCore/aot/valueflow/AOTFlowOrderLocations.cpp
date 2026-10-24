/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrderInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
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
    LayoutID owningClass = m_index.program().variables[location].owner;
    if (!m_classesEvaluatedOnce.get(owningClass.index()))
        return false;

    bool isDefinedWhenHeld = anyPrecedes(stores, entryByUseOf(m_index.program().at(owningClass).constructor));
    if (m_index.inputs.staticFieldsOfEscapedClasses.get(location) && !isDefinedWhenHeld)
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

InitializationState FlowScopeOwnerAnalysis::initializationOf(FlowScopeLocation location, bool isSetAtCreation, bool startsAbsent)
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
    if (isEveryReadCovered)
        return InitializationState::StoredBeforeEveryRead;

    if (startsAbsent && !isStaticField && !stores.isEmpty()) {
        for (const FlowScopeLocationAccess& read : accessesAt(m_index.reads.span(), location)) {
            if (anyPrecedes(stores, entryOf(read)))
                m_readsAfterInitialization.append({ read.function, read.value });
        }
    }
    return InitializationState::Unknown;
}

bool FlowScopeOwnerAnalysis::anyPrecedesCallersOf(const InitializingStores& stores, FunctionID reader)
{
    AOT_THRESHOLD(size_t, mostCallersToFollow, 64);
    Vector<FunctionID, 8> followed { reader };
    for (size_t next = 0; next < followed.size(); ++next) {
        FunctionID function = followed[next];
        if (followed.size() > mostCallersToFollow || m_index.calls.functionsEnteredByHost.get(function.index()) || !anyPrecedes(stores, entryOf(function)))
            return false;
        for (const FlowScopeCall& call : m_index.calls.callsOf(function)) {
            if (FlowScopeFrame* frame = findFrame(call.caller)) {
                if (!anyPrecedes(stores, entryAt(*frame, call.site)))
                    return false;
            } else if (!followed.contains(call.caller))
                followed.append(call.caller);
        }
    }
    return true;
}

bool FlowScopeOwnerAnalysis::isStoredBeforeCallersOfReaders(FlowScopeLocation location)
{
    InitializingStores stores = initializingStoresOf(location);
    bool result = true;
    forEachReader(location, [&](const FlowScopeLocationAccess& read) {
        result = result && (findFrame(read.function) || anyPrecedesCallersOf(stores, read.function));
    });
    return result;
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

bool FlowScopeOwnerAnalysis::isListedWithUndefinedAlone(std::span<const FlowEnvironmentAllocation> sites, uint32_t field)
{
    FlowScopeFrame& frame = ownerFrame();
    return std::ranges::all_of(sites, [&](const FlowEnvironmentAllocation& site) {
        const Value& allocation = frame.body.at(site.value);
        std::span<const uint32_t> fields = allocation.fields();
        for (size_t index = 0; index < fields.size(); ++index) {
            if (fields[index] == field && !listsStartOfVar(frame.body, allocation, index))
                return false;
        }
        return true;
    });
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
        if (isSetAtCreation && isListedWithUndefinedAlone(sites, field) && initializationOf(location, false, false) == InitializationState::StoredBeforeEveryRead) {
            orders[location] = {
                InitializationState::StoredBeforeEveryRead,
                storeCountOf(location, false, [&](const FlowScopeLocationAccess& store) {
                    return canRunTwicePerInstance(store, *allocationBlock);
                }),
                true,
            };
            continue;
        }
        bool startsAbsent = m_index.program().at(m_index.program().at(layout).fields[field].declared.type).primitives & Primitive::Absent;
        orders[location] = {
            initializationOf(location, isSetAtCreation, startsAbsent),
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
        bool startsAbsent = m_index.program().at(m_index.program().variables[location].declared.type).primitives & Primitive::Absent;
        orders[location] = {
            initializationOf(location, isSetAtCreation, startsAbsent),
            storeCountOf(location, isSetAtCreation, [&](const FlowScopeLocationAccess& store) {
                return canRunTwice(store);
            }),
        };
        if (orders[location].initialization == InitializationState::StoredBeforeEveryRead && orders[location].stores == StoreCount::AtMostOnce)
            orders[location].isStoredBeforeCallersOfReaders = isStoredBeforeCallersOfReaders(location);
    }
    for (LayoutID layout : m_owner.environments)
        computeEnvironmentOrders(layout, orders);
}

static bool runsAfterCompletion(const FlowScopeIndex& index, FunctionID function, ModuleID completed)
{
    ModuleID module = index.program().at(function).module;
    return module && index.moduleOrder.orderingOf(completed, module) == FlowModuleOrdering::CompletesBefore;
}

static bool canObserveUnfinishedModule(const Program& program, const FlowModuleValueRead& read)
{
    Opcode opcode = program.at(read.function).at(read.value).opcode;
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
    for (size_t module : index.modulesReadByHost)
        visit(ModuleID(static_cast<uint32_t>(module)));
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
    const Program& program = index.program();
    BitVector variablesReadEarly = findVariablesReadEarly(index);
    for (size_t synthetic : index.syntheticVariables) {
        FlowScopeLocation variable = static_cast<FlowScopeLocation>(synthetic);
        if (index.opaqueLocations.get(variable))
            continue;
        ModuleID module = program.variables[variable].module;
        if (index.inputs.modulesLoadedByName.get(module.index()))
            continue;
        bool isReadAfterCompletion = !variablesReadEarly.get(variable) && std::ranges::all_of(accessesAt(index.reads.span(), variable), [&](const FlowScopeLocationAccess& read) {
            return runsAfterCompletion(index, read.function, module);
        });
        orders[variable] = {
            isReadAfterCompletion ? InitializationState::StoredBeforeEveryRead : InitializationState::Unknown,
            accessesAt(index.stores.span(), variable).empty() ? StoreCount::AtMostOnce : StoreCount::Unknown,
        };
    }
}

FlowScopeOrders computeScopeOrders(const FlowOrderInputs& inputs, const FlowModuleOrder& moduleOrder)
{
    FlowScopeIndex index = createScopeIndex(inputs, moduleOrder);
    Vector<FlowLocationOrder> orders(index.numbering.numberOfLocations());
    Vector<Vector<FlowReadOfLocation>> readsAfterInitializationOfOwners(index.owners.size());
    forEachIndexInParallel(inputs.build.parallelLoops(), index.owners.size(), [&](unsigned owner) {
        FlowScopeOwnerAnalysis analysis(index, index.owners[owner]);
        analysis.computeEntries();
        analysis.computeOrders(orders.mutableSpan());
        readsAfterInitializationOfOwners[owner] = analysis.takeReadsAfterInitialization();
    });
    Vector<FlowReadOfLocation> readsAfterInitialization;
    for (Vector<FlowReadOfLocation>& reads : readsAfterInitializationOfOwners)
        readsAfterInitialization.appendVector(reads);
    AOT::sort(readsAfterInitialization);
    computeSyntheticExportOrders(index, orders.mutableSpan());

    return {
        WTF::move(index.numbering),
        WTF::move(orders),
        WTF::move(readsAfterInitialization),
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
