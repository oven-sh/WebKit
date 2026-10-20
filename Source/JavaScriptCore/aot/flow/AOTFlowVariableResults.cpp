/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTFlowOrder.h"
#include "AOTValueIdentity.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using LocationEvents = OptionSet<FlowLocationEvent>;

// At run time ResolveExport reads the export entries of these modules to decide which names exist, also an entry whose variable no code reads.
static BitVector findModulesResolvedAtRunTime(const Program& program)
{
    BitVector modules(program.modules.size());
    for (size_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        for (const ModuleRequest& request : module.requests) {
            if (!request.isDynamicStarExport)
                continue;
            modules.quickSet(index);
            modules.quickSet(request.target.index());
        }
        for (const RequiredName& requiredName : module.requiredNames)
            modules.quickSet(requiredName.module.index());
    }
    return modules;
}

static bool isReadByUnnamedKeys(const FlowFamilyInputs& inputs, ModuleID module, const ModuleFlow& flow)
{
    if (flow.namespaceUse != NamespaceUse::Unknown)
        return false;
    FlowObjectID namespaceObject = inputs.objects().findNamespace(module);
    return namespaceObject && inputs.factsOf(namespaceObject).uses.containsAny(unnamedReadUses);
}

static void addEventToExports(const Module& module, FlowLocationEvent event, Vector<LocationEvents>& eventsByVariable)
{
    for (const ModuleExport& moduleExport : module.exports) {
        switch (moduleExport.kind) {
        case ModuleExportKind::Variable:
            if (moduleExport.variable)
                eventsByVariable[moduleExport.variable.index()].add(event);
            break;
        case ModuleExportKind::Namespace:
        case ModuleExportKind::Indirect:
            break;
        }
    }
    for (const SyntheticExport& syntheticExport : module.syntheticExports) {
        if (syntheticExport.variable)
            eventsByVariable[syntheticExport.variable.index()].add(event);
    }
}

static void addEventsOnNamespaces(const FlowFamilyInputs& inputs, std::span<const ModuleFlow> modules, Vector<LocationEvents>& eventsByVariable)
{
    const Program& program = inputs.program();
    BitVector modulesResolvedAtRunTime = findModulesResolvedAtRunTime(program);
    for (size_t index = 0; index < program.modules.size(); ++index) {
        if (modulesResolvedAtRunTime.quickGet(index))
            addEventToExports(program.modules[index], FlowLocationEvent::PresenceObserved, eventsByVariable);
        if (isReadByUnnamedKeys(inputs, ModuleID(index), modules[index]))
            addEventToExports(program.modules[index], FlowLocationEvent::ReadAsValue, eventsByVariable);
    }
}

static LocationEvents eventsOnClassObject(const FlowFamilyInputs& inputs, FlowObjectID classObject, PropertyKeyID key)
{
    LocationEvents events = aliasedEventsOf(inputs.solution, classObject, key);
    bool mayBeNamedByUnknownKey = !key || classOf(inputs.program(), key) != FlowKeyClass::PrivateName;
    if (!mayBeNamedByUnknownKey)
        return events;

    const FlowObjectFacts& facts = inputs.factsOf(classObject);
    if (facts.uses.containsAny(unnamedReadUses))
        events.add(FlowLocationEvent::ReadAsValue);
    if (facts.events.contains(FlowObjectEvent::DeletedByUnknownKey))
        events.add(FlowLocationEvent::Deleted);
    // Sealing and freezing change the attributes of every property of the class object.
    if (facts.mutations.contains(ObjectMutation::ChangeIntegrity))
        events.add(FlowLocationEvent::Redefined);
    return events;
}

static void addEventsOnClassObjects(const FlowFamilyInputs& inputs, Vector<LocationEvents>& eventsByVariable)
{
    for (const Layout& layout : inputs.program().layouts) {
        if (layout.staticFields.isEmpty())
            continue;
        for (FlowObjectID classObject : inputs.objects().closuresOf(layout.constructor)) {
            for (const StaticField& field : layout.staticFields)
                eventsByVariable[field.variable.index()].add(eventsOnClassObject(inputs, classObject, field.key));
        }
    }
}

static bool isSingleStore(LocationEvents events)
{
    return events.contains(FlowLocationEvent::StoredByOperation) && !events.contains(FlowLocationEvent::StoredByManyOperations);
}

static bool storeRunsAtMostOnce(const FlowFamilyInputs& inputs, LocationEvents events, const Witness& store)
{
    if (!isSingleStore(events) || store.function >= inputs.executionsByFunction.size())
        return false;
    const Function& function = inputs.program().functions[store.function];
    if (store.value >= function.values.size() || function.values[store.value].opcode != Opcode::PutModuleVariable)
        return false;
    return inputs.executionsByFunction[store.function] == ExecutionCount::AtMostOnce;
}

static StoreCount computeStores(const FlowFamilyInputs& inputs, const Variable& variable, LocationEvents events, const Witness& firstStore, StoreCount storesFromOrder)
{
    switch (storesFromOrder) {
    case StoreCount::Unknown:
        break;
    case StoreCount::Never:
        return StoreCount::Never;
    case StoreCount::AtMostOnce:
        // The order of execution and the events count the function a variable starts with as a store. LocationFlow counts operations only.
        return variable.initialization == VariableInitialization::Function ? StoreCount::Never : StoreCount::AtMostOnce;
    }
    if (!isStoredByOperation(events))
        return StoreCount::Never;
    return storeRunsAtMostOnce(inputs, events, firstStore) ? StoreCount::AtMostOnce : StoreCount::Unknown;
}

static bool isAnnotatedPure(const Value& value)
{
    return (value.opcode == Opcode::Call || value.opcode == Opcode::Construct) && value.isAnnotatedPure();
}

// The annotation says that a call may go when nobody uses its answer, and a store that nothing reads is no use: RemoveUnreadExports takes the call away with the store,
// so there is no value that anybody could see let go. That holds where the store is the only use of the answer and the variable has no store of another kind.
// Casts that test nothing may stand between the call and the store.
static bool storesUnsharedPureAnswer(const Function& function, const Value& store, const Vector<uint32_t>& numberOfUses)
{
    for (ValueID id = store.valueOperand(); numberOfUses[id.index()] == 1; id = function.at(id).valueOperand()) {
        const Value& value = function.at(id);
        if (isAnnotatedPure(value))
            return true;
        if (!isCastWithoutTest(value.opcode))
            return false;
    }
    return false;
}

static BitVector findVariablesHoldingPureAnswersOnly(const Program& program)
{
    BitVector variables(program.variables.size());
    BitVector variablesWithOtherStores(program.variables.size());
    Vector<uint32_t> numberOfUses;
    Vector<ValueID, 8> storesOfPureAnswers;
    for (const Function& function : program.functions) {
        storesOfPureAnswers.shrink(0);
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                if (value.opcode != Opcode::PutModuleVariable)
                    continue;
                if (isAnnotatedPure(function.at(sameObjectThroughCasts(function, value.valueOperand()))))
                    storesOfPureAnswers.append(id);
                else
                    variablesWithOtherStores.quickSet(value.variable().index());
            }
        }
        if (storesOfPureAnswers.isEmpty())
            continue;

        numberOfUses.fill(0, function.values.size());
        auto countUse = [&](ValueID value) {
            ++numberOfUses[value.index()];
        };
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values)
                function.at(id).forEachChild(countUse);
            block.forEachChildOutsideValues(countUse);
        }
        for (ValueID id : storesOfPureAnswers) {
            const Value& store = function.at(id);
            BitVector& found = storesUnsharedPureAnswer(function, store, numberOfUses) ? variables : variablesWithOtherStores;
            found.quickSet(store.variable().index());
        }
    }
    variables.exclude(variablesWithOtherStores);
    return variables;
}

static LocationFlow computeVariableFlow(const FlowFamilyInputs& inputs, VariableID id, LocationEvents eventsOnHolders, bool holdsPureAnswersOnly)
{
    FlowNodeID node = inputs.graph().variable(id);
    if (!node)
        return { };

    const Variable& variable = inputs.program().at(id);
    const FlowLocationRecord& record = inputs.records().recordOf(node);
    LocationEvents events = record.events | eventsOnHolders;
    FlowLocationOrder order = locationOrderOf(inputs.solution, id);
    // A store under a key nobody names, or to the unknown object where the class object has escaped, reaches a static field, and the order of execution lists neither.
    bool isStaticField = !!variable.owner;
    bool hasUnlistedStore = isStaticField && (events.contains(FlowLocationEvent::StoredByManyOperations) || isStoredByOperation(eventsOnHolders));
    // The run time empties the variable of a static field that is deleted, redefined or stored with a value outside its type, and keeps the property elsewhere.
    bool hasUnknownWriter = events.containsAny({ FlowLocationEvent::WrittenByUnknownCode, FlowLocationEvent::Deleted, FlowLocationEvent::Redefined, FlowLocationEvent::StoredOutsideType });

    FlowSiteLoss loss;
    auto addLossOf = [&](FlowLocationEvent event, UnprovenCause cause) {
        if (events.contains(event))
            keepFirstLoss(loss, FlowSiteLoss { .cause = cause, .witness = record.firstLoss });
    };
    addLossOf(FlowLocationEvent::WrittenByUnknownCode, UnprovenCause::UnknownWriter);
    addLossOf(FlowLocationEvent::Deleted, UnprovenCause::FieldDeleted);
    addLossOf(FlowLocationEvent::Redefined, UnprovenCause::PropertyRedefined);
    addLossOf(FlowLocationEvent::StoredOutsideType, UnprovenCause::StoreOfOtherType);
    auto isEnabled = [&](FlowResultKind kind) {
        if (inputs.isEnabled(kind, id.index()))
            return true;
        keepFirstLoss(loss, FlowSiteLoss { .cause = UnprovenCause::ResultDisabled });
        return false;
    };

    LocationFlow flow;
    if (isEnabled(FlowResultKind::StoreCounts) && !hasUnknownWriter) {
        flow.writers = WriterState::AllKnown;
        flow.stores = hasUnlistedStore ? StoreCount::Unknown : computeStores(inputs, variable, events, record.firstStore, order.stores);
        flow.isUnchangedDuringActivation = flow.stores == StoreCount::Never;
    }
    if (isEnabled(FlowResultKind::Initialization))
        flow.initialization = order.initialization;
    flow.mayKeepObservedObjectAlive = !holdsPureAnswersOnly && mayKeepObservedObjectAlive(inputs, node);
    if (isEnabled(FlowResultKind::ObjectUses) && !flow.mayKeepObservedObjectAlive)
        flow.reads = readStateOf(events);
    flow.cause = loss.cause;
    flow.witness = loss.witness;
    return flow;
}

Vector<LocationFlow> computeVariableResults(const FlowFamilyInputs& inputs, std::span<const ModuleFlow> modules)
{
    const Program& program = inputs.program();
    RELEASE_ASSERT(modules.size() == program.modules.size());

    Vector<LocationEvents> eventsOnHolders(program.variables.size());
    addEventsOnNamespaces(inputs, modules, eventsOnHolders);
    addEventsOnClassObjects(inputs, eventsOnHolders);
    BitVector variablesHoldingPureAnswersOnly = findVariablesHoldingPureAnswersOnly(program);

    Vector<LocationFlow> results(program.variables.size());
    forEachIndexInParallel(inputs.loops(), results.size(), [&](unsigned index) {
        results[index] = computeVariableFlow(inputs, VariableID(index), eventsOnHolders[index], variablesHoldingPureAnswersOnly.quickGet(index));
    });
    return results;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
