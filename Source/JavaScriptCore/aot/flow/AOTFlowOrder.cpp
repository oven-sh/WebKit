/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrder.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"
#include "AOTFlowObjectState.h"
#include "AOTFlowPropertyState.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

FlowMemberHolder memberHolderKindOf(const FlowObjectTable& objects, FlowObjectID object)
{
    switch (objects.kindOf(object)) {
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
        return FlowMemberHolder::Always;
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return FlowMemberHolder::WhenPrototype;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return FlowMemberHolder::No;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool followsMembersOf(const FlowOrderInputs& inputs, FlowObjectID holder)
{
    switch (memberHolderKindOf(inputs.objects(), holder)) {
    case FlowMemberHolder::No:
        return false;
    case FlowMemberHolder::WhenPrototype:
        return inputs.isPrototype(holder);
    case FlowMemberHolder::Always:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowOrderFingerprint FlowSolverOrderInputs::fingerprint() const
{
    return {
        .numberOfInstances = m_graph.numberOfInstances(),
        .numberOfObjects = m_graph.objects().size(),
        .numberOfAllocatedObjects = m_records.numberOfAllocatedObjects(),
        .numberOfEvaluatedModules = static_cast<unsigned>(m_records.evaluatedModules().bitCount()),
        .numberOfModuleLoads = static_cast<unsigned>(m_records.moduleLoads().size()),
        .numberOfUnseenCodeRecords = static_cast<unsigned>(m_records.unseenCode().size()),
        .numberOfFunctionsPastWorkLimit = static_cast<unsigned>(m_records.functionsPastWorkLimit().bitCount()),
        .numberOfCallLinks = static_cast<unsigned>(m_records.callLinks().size()),
        .numberOfBuiltinCallLinks = static_cast<unsigned>(m_records.builtinCallLinks().size()),
        .numberOfUnknownCallees = static_cast<unsigned>(m_records.unknownCallees().size()),
        .numberOfStaticFieldAccesses = static_cast<unsigned>(m_records.staticFieldAccesses().size()),
        .numberOfMemberAccesses = static_cast<unsigned>(m_records.memberAccesses().size()),
        .numberOfUncertainMemberStores = static_cast<unsigned>(m_records.uncertainMemberStores().size()),
        .numberOfPrototypes = m_records.numberOfObjectsWith(ObjectMutation::BecomePrototype),
        .numberOfObjectsChangingIntegrity = m_records.numberOfObjectsWith(ObjectMutation::ChangeIntegrity),
        .numberOfObjectsInNativeCode = m_records.numberOfObjectsWith(ObjectMutation::ReachNativeCode),
        .numberOfObjectsInUnknownJavaScript = m_records.numberOfObjectsWith(ObjectMutation::ReachUnknownJavaScript),
    };
}

bool FlowSolverOrderInputs::hasClassObjectEscaped(FunctionID constructor) const
{
    return std::ranges::any_of(m_graph.objects().closuresOf(constructor), [&](FlowObjectID classObject) {
        return hasEscaped(classObject);
    });
}

bool FlowSolverOrderInputs::mayChangeIntegrity(FlowObjectID object) const
{
    OptionSet<ObjectMutation> mutations = m_records.possibleMutations(object);
    if (mutations.containsAny({ ObjectMutation::ChangeIntegrity, ObjectMutation::ReachUnknownJavaScript }))
        return true;
    return mutations.contains(ObjectMutation::ReachNativeCode) && m_records.possibleMutations(unknownFlowObject).contains(ObjectMutation::ChangeIntegrity);
}

template<typename Statement>
static ValueID originOfStatement(const Statement& statement)
{
    if constexpr (requires { statement.origin; })
        return statement.origin;
    else
        return { };
}

static ValueID originOf(FlowSolver& solver, FlowStatementSite site)
{
    const FlowStatements& statements = solver.summaryOf(site).statements;
    switch (site.statement.kind) {
#define ORIGIN_OF_FLOW_STATEMENT(kind, type, member, cursors) \
    case FlowStatementKind::kind: \
        return originOfStatement(statements.member[site.statement.index]);
        FOR_EACH_FLOW_STATEMENT_KIND(ORIGIN_OF_FLOW_STATEMENT)
#undef ORIGIN_OF_FLOW_STATEMENT
    case FlowStatementKind::BuiltinCall:
        return solver.callState().builtins.statements[site.statement.index].key.origin;
    case FlowStatementKind::Lookup:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void noteStaticFieldAccess(FlowSolver& solver, FlowStatementSite site, VariableID variable, FlowStaticFieldAccessKind kind)
{
    FlowStaticFieldAccess access { variable, { }, { }, kind == FlowStaticFieldAccessKind::Store };
    if (site.instance && !solver.graph().isHost(site.instance)) {
        if (ValueID origin = originOf(solver, site)) {
            access.function = solver.graph().at(site.instance).function;
            access.origin = origin;
        }
    }
    solver.records().addStaticFieldAccess(access);
}

static FlowMemberOperation memberOperationAt(FlowSolver& solver, FlowStatementSite site)
{
    if (!site.instance || solver.graph().isHost(site.instance))
        return { };
    ValueID origin = originOf(solver, site);
    if (!origin)
        return { };
    const FlowInstance& instance = solver.graph().at(site.instance);
    return { instance.function, origin, instance.isCallSiteInstance() ? site.instance : FlowInstanceID() };
}

void noteMemberRead(FlowSolver& solver, FlowStatementSite site, FlowObjectID holder, PropertyKeyID key)
{
    solver.records().addMemberAccess({ holder, key, memberOperationAt(solver, site), false });
}

void noteMemberStore(FlowSolver& solver, FlowStatementSite site, FlowObjectID holder, PropertyKeyID key)
{
    solver.records().addMemberAccess({ holder, key, memberOperationAt(solver, site), true });
}

void noteUncertainMemberStore(FlowSolver& solver, FlowStatementSite site)
{
    FlowMemberOperation operation = memberOperationAt(solver, site);
    if (operation.function)
        solver.records().addUncertainMemberStore(operation);
}

static constexpr uint16_t absentOrUndefined = Primitive::Absent | Primitive::Undefined;

uint16_t valueBeforeInitialization(const Program&, const Variable& variable)
{
    bool isStaticField = !!variable.owner;
    switch (variable.initialization) {
    case VariableInitialization::Uninitialized:
        // A property read on the class object answers undefined for a static field that is not defined yet.
        return isStaticField ? absentOrUndefined : Primitive::Absent;
    case VariableInitialization::Undefined:
        return Primitive::Undefined;
    case VariableInitialization::Function:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint16_t valueBeforeInitialization(const Program& program, const Field& field)
{
    bool hasDeadZone = program.at(field.declared.type).primitives & Primitive::Absent;
    return hasDeadZone ? Primitive::Absent : Primitive::Undefined;
}

static void joinValueBeforeInitialization(FlowSolver& solver, FlowNodeID location, uint16_t value)
{
    solver.noteLocationEvent(location, FlowLocationEvent::ReadBeforeInitialization);
    solver.joinPrimitives(location, value);
}

void noteReadBeforeInitialization(FlowSolver& solver, VariableID variable)
{
    joinValueBeforeInitialization(solver, solver.graph().variable(variable), valueBeforeInitialization(solver.program(), solver.program().at(variable)));
}

void noteReadBeforeInitialization(FlowSolver& solver, LayoutID environment, uint32_t fieldIndex)
{
    Vector<FlowObjectID, 8> objects;
    for (AllocationClassID allocationClass : solver.objects().allocationClassesOf(environment)) {
        forEachObjectOfClass(solver.objects(), allocationClass, [&](FlowObjectID object) {
            objects.append(object);
        });
    }
    uint16_t value = valueBeforeInitialization(solver.program(), solver.program().at(environment).fields[fieldIndex]);
    for (FlowObjectID object : objects)
        joinValueBeforeInitialization(solver, solver.graph().field(object, fieldIndex), value);
}

static bool isAllocated(const FlowOrderInputs& inputs, LayoutID environment)
{
    bool found = false;
    for (AllocationClassID allocationClass : inputs.objects().allocationClassesOf(environment)) {
        forEachObjectOfClass(inputs.objects(), allocationClass, [&](FlowObjectID object) {
            found |= inputs.isAllocated(object);
        });
    }
    return found;
}

static void markUnprovenLocations(const FlowOrderInputs& inputs, FlowOrderState& order)
{
    const Program& program = inputs.program();
    const FlowScopeLocationNumbering& numbering = order.scopes.numbering;
    auto markIfUnproven = [&](uint32_t location) {
        if (order.scopes.orders[location].initialization == InitializationState::Unknown)
            order.locationsReadBeforeInitialization.set(location);
    };

    const BitVector& evaluatedModules = inputs.evaluatedModules();
    for (uint32_t variable = 0; variable < numbering.numberOfVariables; ++variable) {
        if (evaluatedModules.get(program.variables[variable].module.index()))
            markIfUnproven(variable);
    }
    for (uint32_t layout = 0; layout < program.layouts.size(); ++layout) {
        uint32_t first = numbering.firstField[layout];
        uint32_t end = numbering.firstField[layout + 1];
        if (first == end || !isAllocated(inputs, LayoutID(layout)))
            continue;
        for (uint32_t location = first; location < end; ++location)
            markIfUnproven(location);
    }
}

static FieldReference environmentFieldAt(const FlowScopeLocationNumbering& numbering, uint32_t location)
{
    auto next = std::ranges::upper_bound(numbering.firstField, location);
    RELEASE_ASSERT(next != numbering.firstField.begin() && next != numbering.firstField.end());
    uint32_t layout = static_cast<uint32_t>(next - numbering.firstField.begin()) - 1;
    return { LayoutID(layout), location - numbering.firstField[layout] };
}

static void noteUnprovenLocations(FlowSolver& solver, const FlowOrderState& order)
{
    const FlowScopeLocationNumbering& numbering = order.scopes.numbering;
    for (size_t bit : order.locationsReadBeforeInitialization) {
        uint32_t location = static_cast<uint32_t>(bit);
        if (location < numbering.numberOfVariables) {
            noteReadBeforeInitialization(solver, VariableID(location));
            continue;
        }
        FieldReference field = environmentFieldAt(numbering, location);
        noteReadBeforeInitialization(solver, field.layout, field.field);
    }
}

static bool isDefinedBeforeEveryLookup(const FlowSolver& solver, const FlowOrderState& order, const FlowProperty& accessor, FlowObjectID holder)
{
    FlowLocationOrder proven = order.scopes.orderOf(holder, accessor.key);
    if (proven.initialization != InitializationState::StoredBeforeEveryRead || proven.stores != StoreCount::AtMostOnce)
        return false;
    return std::ranges::none_of(std::array { accessor.getter, accessor.setter }, [&](FlowNodeID half) {
        return half && solver.records().recordOf(half).events.containsAny(eventsChangingDefinedAccessor);
    });
}

// A store that has run the setter alone comes back and defines a property of its receiver as well.
static void takeBackUnconfirmedAccessors(FlowSolver& solver, const FlowOrderState& order)
{
    for (auto [holder, key] : solver.propertyState().accessorsAssumedPresent.accessors) {
        const FlowProperty* entry = solver.graph().findProperty(holder, key);
        RELEASE_ASSERT(entry);
        FlowProperty accessor = *entry;
        if (isDefinedBeforeEveryLookup(solver, order, accessor, holder))
            continue;

        bool isTakenBackAlready = true;
        for (FlowNodeID half : { accessor.getter, accessor.setter }) {
            if (!half || solver.records().recordOf(half).events.contains(FlowLocationEvent::ReadBeforeInitialization))
                continue;
            solver.noteLocationEvent(half, FlowLocationEvent::ReadBeforeInitialization);
            isTakenBackAlready = false;
        }
        if (!isTakenBackAlready)
            solver.revisitPropertyWaiters(holder, key);
    }
}

FlowProgress refineExecutionOrder(FlowSolver& solver)
{
    FlowSolverOrderInputs inputs { solver.program(), solver.graph(), solver.records(), solver.build() };
    FlowOrderState& order = solver.callState().order;
    FlowOrderFingerprint fingerprint = inputs.fingerprint();
    if (order.computedFor != fingerprint) {
        order.computedFor = fingerprint;
        order.modules = computeModuleOrder(inputs);
        order.scopes = computeScopeOrders(inputs, order.modules, order.accessesByFunction);
        markUnprovenLocations(inputs, order);
        noteUnprovenLocations(solver, order);
    }
    takeBackUnconfirmedAccessors(solver, order);
    return solver.hasQueuedWork() ? FlowProgress::QueuedWork : FlowProgress::Finished;
}

FlowModuleOrdering moduleOrderingOf(const FlowSolution& solution, ModuleID first, ModuleID second)
{
    return solution.callState.order.modules.orderingOf(first, second);
}

static FlowLocationOrder withoutFailedProof(const FlowOrderState& order, FlowLocationOrder proven, std::optional<uint32_t> location)
{
    if (location && order.locationsReadBeforeInitialization.get(*location))
        proven.initialization = InitializationState::Unknown;
    return proven;
}

FlowLocationOrder locationOrderOf(const FlowSolution& solution, VariableID variable)
{
    const FlowOrderState& order = solution.callState.order;
    return withoutFailedProof(order, order.scopes.orderOf(variable), order.scopes.numbering.locationOf(variable));
}

FlowLocationOrder locationOrderOf(const FlowSolution& solution, LayoutID environment, uint32_t fieldIndex)
{
    const FlowOrderState& order = solution.callState.order;
    return withoutFailedProof(order, order.scopes.orderOf(environment, fieldIndex), order.scopes.numbering.locationOf(environment, fieldIndex));
}

FlowLocationOrder locationOrderOf(const FlowSolution& solution, FlowObjectID holder, PropertyKeyID key)
{
    return solution.callState.order.scopes.orderOf(holder, key);
}

static bool isRead(const FlowRecords& records, BuiltinID location)
{
    return !(records.builtinUse(location).uses - BuiltinUse::Tested).isEmpty();
}

static bool mayChangeCacheEntry(const FlowRecords& records, const FlowProperty& entry)
{
    constexpr OptionSet<FlowLocationEvent> changes {
        FlowLocationEvent::Deleted,
        FlowLocationEvent::Redefined,
        FlowLocationEvent::StoredByOperation,
        FlowLocationEvent::StoredByManyOperations,
    };
    if (entry.getter || entry.setter)
        return true;
    return entry.value && records.recordOf(entry.value).events.containsAny(changes);
}

FlowCacheEntryState cacheEntryStateOf(const FlowSolution& solution, ModuleID module)
{
    constexpr OptionSet<FlowObjectEvent> changesUnderUnknownKeys {
        FlowObjectEvent::StoredByUnknownKey,
        FlowObjectEvent::DeletedByUnknownKey,
    };
    const Program& program = solution.program;
    const FlowRecords& records = solution.records;
    if (!module || module.index() >= program.modules.size() || records.mayRunUnknownJavaScript())
        return FlowCacheEntryState::Unknown;
    if (!isRead(records, Builtins::ModuleRequireCache) && !isRead(records, Builtins::NodeModule_cache))
        return FlowCacheEntryState::NeverRemoved;

    FlowObjectID cache = solution.objects.findBuiltin(Builtins::ModuleCache);
    if (!cache || solution.objectsInNativeCode().contains(cache) || records.eventsOf(cache).containsAny(changesUnderUnknownKeys))
        return FlowCacheEntryState::Unknown;

    StringID path = program.at(module).path;
    bool mayChange = std::ranges::any_of(solution.graph.propertiesOf(cache), [&](const FlowProperty& entry) {
        const PropertyKey& key = program.at(entry.key);
        return key.kind == PropertyKeyKind::Text && key.text == path && mayChangeCacheEntry(records, entry);
    });
    return mayChange ? FlowCacheEntryState::Unknown : FlowCacheEntryState::NeverRemoved;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
