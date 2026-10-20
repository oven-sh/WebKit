/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTFlowOrder.h"
#include "AOTFlowParts.h"
#include "AOTFlowPropertyState.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using LocationEvents = OptionSet<FlowLocationEvent>;

struct LayoutFieldTask {
    const FlowFamilyInputs& inputs;
    const BitVector& functionsCalledByHost;
    LayoutID layout;
    std::optional<BitVector> fieldsSetAtCreation;
    std::optional<bool> constructorIsOnlyConstructed;
};

struct EventCause {
    FlowLocationEvent event;
    UnprovenCause cause;
};

} // anonymous namespace

static FlowLocationOrder scopeOrderOf(const FlowSolution& solution, const FlowFieldSubject& subject)
{
    if (!subject.declaredIndex)
        return { };
    return locationOrderOf(solution, subject.layout, *subject.declaredIndex);
}

static BitVector findFieldsSetAtCreation(const FlowFamilyInputs& inputs, LayoutID layout)
{
    bool hasAllocatedSubclass = std::ranges::any_of(inputs.hierarchy.layoutsBelow(layout), [&](LayoutID member) {
        return member != layout && !inputs.program().at(member).isDerived() && !inputs.hierarchy.objectsOf(member).empty();
    });
    if (hasAllocatedSubclass)
        return { };
    return constructionOf(inputs.solution, layout).fieldsSetAtCreation;
}

// One operation that both constructs and calls an instance has one link, with the kind of whichever request came first. The uses of a closure list both.
static bool isEnteredOnlyByConstruction(const LayoutFieldTask& task, FunctionID function)
{
    const FlowFamilyInputs& inputs = task.inputs;
    if (findCallerLoss(inputs, task.functionsCalledByHost, function).cause != UnprovenCause::Unknown)
        return false;
    bool isCalled = std::ranges::any_of(inputs.objects().closuresOf(function), [&](FlowObjectID closure) {
        return inputs.factsOf(closure).uses.contains(FlowObjectUse::Called);
    });
    if (isCalled)
        return false;
    return std::ranges::all_of(inputs.calls.linksTo(function), [&](FlowCallLinkID link) {
        return inputs.records().at(link).kind == FlowCallLinkKind::Construct;
    });
}

static bool isConstructorOnlyConstructed(LayoutFieldTask& task)
{
    if (!task.constructorIsOnlyConstructed)
        task.constructorIsOnlyConstructed = isEnteredOnlyByConstruction(task, task.inputs.program().at(task.layout).constructor);
    return *task.constructorIsOnlyConstructed;
}

// A helper that the constructor calls, and a function that somebody calls without new, can run again on an object that exists already.
static bool isConstructionStore(LayoutFieldTask& task, const Witness& store)
{
    if (!hasFunction(store))
        return false;

    const Program& program = task.inputs.program();
    FunctionID function(store.function);
    switch (program.at(function).kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return program.at(task.layout).constructor == function;
    case FunctionKind::Initializer:
        return true;
    case FunctionKind::Plain:
        return program.at(task.layout).constructor == function && isConstructorOnlyConstructed(task);
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool storeRunsOncePerObject(LayoutFieldTask& task, const FlowFieldSubject& subject)
{
    const FlowFamilyInputs& inputs = task.inputs;
    switch (inputs.program().at(subject.layout).kind) {
    case LayoutKind::Instance:
        break;
    case LayoutKind::Record:
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    if (!subject.declaredIndex)
        return false;
    if (!task.fieldsSetAtCreation)
        task.fieldsSetAtCreation = findFieldsSetAtCreation(inputs, task.layout);
    if (!task.fieldsSetAtCreation->get(*subject.declaredIndex))
        return false;

    bool hasOnlyConstructionStores = true;
    forEachFieldNode(inputs, subject, [&](FlowObjectID, FlowNodeID node) {
        hasOnlyConstructionStores &= isConstructionStore(task, inputs.records().recordOf(node).firstStore);
    });
    return hasOnlyConstructionStores;
}

static StoreCount computeFieldStores(LayoutFieldTask& task, const FlowFieldSubject& subject, LocationEvents events)
{
    constexpr LocationEvents uncountedChanges { FlowLocationEvent::WrittenByUnknownCode, FlowLocationEvent::Deleted, FlowLocationEvent::Redefined };
    if (events.containsAny(uncountedChanges))
        return StoreCount::Unknown;
    if (task.inputs.program().at(subject.layout).isEnvironment())
        return scopeOrderOf(task.inputs.solution, subject).stores;

    bool isInitialized = events.contains(FlowLocationEvent::InitializedAtAllocation);
    if (!isInitialized && !isStoredByOperation(events))
        return StoreCount::Never;
    if (events.contains(FlowLocationEvent::StoredByManyOperations))
        return StoreCount::Unknown;
    if (isInitialized)
        return events.contains(FlowLocationEvent::StoredByOperation) ? StoreCount::Unknown : StoreCount::AtMostOnce;
    return storeRunsOncePerObject(task, subject) ? StoreCount::AtMostOnce : StoreCount::Unknown;
}

// A function without receiver sets has no stamps, so none of its stores says where it stores.
static StoreNaming computeStoreNaming(const FlowFamilyInputs& inputs, LocationEvents events)
{
    constexpr LocationEvents unnamedStores { FlowLocationEvent::WrittenByUnknownCode, FlowLocationEvent::StoredWithoutNamingLocation, FlowLocationEvent::Redefined };
    if (events.containsAny(unnamedStores) || !inputs.build().isEnabledEverywhere(FlowResultKind::ReceiverSets))
        return StoreNaming::Unknown;
    return StoreNaming::EveryStoreNamesLocation;
}

static bool isUnchangedDuringActivation(StoreCount stores, LocationEvents events)
{
    switch (stores) {
    case StoreCount::Unknown:
        return false;
    case StoreCount::Never:
        return true;
    case StoreCount::AtMostOnce:
        return !isStoredByOperation(events);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Only the refinement of the constructions records where a location outside the declared fields is missing.
static bool isStoredIntoEveryObjectByConstructor(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject)
{
    for (LayoutID layout : inputs.hierarchy.layoutsBelow(subject.layout)) {
        for (FlowObjectID object : inputs.hierarchy.objectsOf(layout)) {
            OptionSet<FlowObjectEvent> events = inputs.records().eventsOf(object);
            if (!events.contains(FlowObjectEvent::Allocated))
                continue;
            if (!events.contains(FlowObjectEvent::AllocatedByConstructor) || !fieldNodeOn(inputs.graph(), object, subject))
                return false;
        }
    }
    return true;
}

static InitializationState computeFieldInitialization(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject, LocationEvents events)
{
    if (!subject.declaredIndex && !isStoredIntoEveryObjectByConstructor(inputs, subject))
        return InitializationState::Unknown;
    if (inputs.program().at(subject.layout).isEnvironment())
        return scopeOrderOf(inputs.solution, subject).initialization;

    constexpr LocationEvents storesAtCreation { FlowLocationEvent::InitializedAtAllocation, FlowLocationEvent::StoredByOperation };
    if (!events.contains(FlowLocationEvent::MissingAtAllocation) && events.containsAny(storesAtCreation))
        return InitializationState::SetAtCreation;
    // Only the objects of a constructor have a record of a read that comes before the store of an operation.
    bool isStoredOnlyAtAllocation = events.contains(FlowLocationEvent::InitializedAtAllocation) && !isStoredByOperation(events);
    if (!events.contains(FlowLocationEvent::ReadBeforeInitialization) && isStoredOnlyAtAllocation)
        return InitializationState::StoredBeforeEveryRead;
    return InitializationState::Unknown;
}

static OptionSet<FieldStability> computeStability(LocationEvents events)
{
    constexpr LocationEvents storesOfOtherTypes { FlowLocationEvent::StoredOutsideType, FlowLocationEvent::WrittenByUnknownCode };
    OptionSet<FieldStability> stability;
    if (!events.contains(FlowLocationEvent::Deleted))
        stability.add(FieldStability::IsNeverDeleted);
    if (!events.contains(FlowLocationEvent::Redefined))
        stability.add(FieldStability::IsNeverRedefined);
    if (!events.containsAny(storesOfOtherTypes))
        stability.add(FieldStability::IsNeverStoredWithOtherType);
    return stability;
}

static FlowSiteLoss findLoss(LocationEvents events, const Witness& firstLoss)
{
    constexpr std::array causes {
        EventCause { FlowLocationEvent::WrittenByUnknownCode, UnprovenCause::UnknownWriter },
        EventCause { FlowLocationEvent::Deleted, UnprovenCause::FieldDeleted },
        EventCause { FlowLocationEvent::Redefined, UnprovenCause::PropertyRedefined },
        EventCause { FlowLocationEvent::StoredOutsideType, UnprovenCause::StoreOfOtherType },
    };
    FlowSiteLoss loss;
    for (const EventCause& entry : causes) {
        if (events.contains(entry.event))
            keepFirstLoss(loss, FlowSiteLoss { .cause = entry.cause, .witness = firstLoss });
    }
    return loss;
}

static bool hasSamePrototypes(const FlowFamilyInputs& inputs, FlowObjectID object, FlowObjectID other)
{
    const FlowGraph& graph = inputs.graph();
    FlowNodeID link = graph.find(object, FlowOpenPart::PrototypeLink);
    FlowNodeID otherLink = graph.find(other, FlowOpenPart::PrototypeLink);
    if (!link || !otherLink)
        return !link && !otherLink && inputs.objects().at(object).layout == inputs.objects().at(other).layout;
    return graph.valueOf(link) == graph.valueOf(otherLink);
}

static bool isAbsentOnPrototypesOf(const FlowSolution& solution, FlowObjectID object, const FlowKeySet& keys)
{
    FlowPropertyWalk walk { .operation = FlowWalkOperation::Get, .startsAtPrototype = true, .waiter = { } };
    bool isProvenAbsent = false;
    bool hasOtherSource = false;
    forEachPropertySource(solution, object, keys, walk, [&](const FlowPropertySource& source) {
        if (source.kind == FlowPropertySourceKind::Absent && source.certainty == FlowCertainty::Proven)
            isProvenAbsent = true;
        else
            hasOtherSource = true;
    });
    if (!isProvenAbsent || hasOtherSource)
        return false;

    // A read answers an accessor without a getter like an absent name. A store runs its setter, and a test finds the name.
    walk.operation = FlowWalkOperation::Set;
    bool hasSetter = false;
    forEachPropertySource(solution, object, keys, walk, [&](const FlowPropertySource&) {
        hasSetter = true;
    });
    return !hasSetter;
}

// An object without a node for the field has the slot too, and every read of it goes on to the prototypes.
static bool isAbsentOnPrototypes(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject)
{
    FlowKeySet keys = keySetOf(inputs.program(), subject.key);
    FlowObjectID lastObject;
    for (LayoutID layout : inputs.hierarchy.layoutsBelow(subject.layout)) {
        for (FlowObjectID object : inputs.hierarchy.objectsOf(layout)) {
            if (lastObject && hasSamePrototypes(inputs, object, lastObject))
                continue;
            if (!isAbsentOnPrototypesOf(inputs.solution, object, keys))
                return false;
            lastObject = object;
        }
    }
    return true;
}

// A store through an object that inherits from this one ends its walk at the field. It is refused where the field is read-only, as the fields of a frozen object are,
// and it reaches no setter and no read-only property of the name further up. Without the field the walk would go on.
static bool isObservedByStoresOfHeirs(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject)
{
    FlowKeySet keys = keySetOf(inputs.program(), subject.key);
    for (LayoutID layout : inputs.hierarchy.layoutsBelow(subject.layout)) {
        for (FlowObjectID object : inputs.hierarchy.objectsOf(layout)) {
            OptionSet<ObjectMutation> mutations = inputs.records().possibleMutations(object);
            if (!mutations.contains(ObjectMutation::BecomePrototype))
                continue;
            if (mutations.contains(ObjectMutation::ChangeIntegrity) || !isAbsentOnPrototypesOf(inputs.solution, object, keys))
                return true;
        }
    }
    return false;
}

// What the value of a field refers to only by way of the object that has the field, that object keeps alive without the field. The holder has to stand for one object and no other:
// a closure in a field of one object may have captured another object of the same allocation site.
static bool refersToObservedObjectsThroughHolderOnly(const FlowFamilyInputs& inputs, FlowObjectID holder, FlowNodeID field)
{
    constexpr size_t maxVisitedObjects = 1024;
    const FlowObservedLifetimes& lifetimes = inputs.observedLifetimes;
    if (!isCreatedOnce(inputs, holder))
        return false;

    FlowObjectSet visited(holder);
    Vector<FlowObjectID, 32> worklist;
    bool reachesObservedObject = false;
    size_t numberOfVisitedObjects = 0;
    auto visit = [&](FlowObjectID object) {
        if (!lifetimes.keepingObservedAlive.contains(object) || !visited.add(object))
            return;
        reachesObservedObject |= lifetimes.observed.contains(object) || ++numberOfVisitedObjects > maxVisitedObjects;
        worklist.append(object);
    };
    inputs.graph().valueOf(field).objects.forEach(visit);
    while (!worklist.isEmpty() && !reachesObservedObject)
        forEachObjectReferredTo(inputs.solution, worklist.takeLast(), visit);
    return !reachesObservedObject;
}

static bool mayKeepObservedObjectAlive(const FlowFamilyInputs& inputs, const FlowFieldSubject& subject)
{
    bool mayKeepAlive = false;
    forEachFieldNode(inputs, subject, [&](FlowObjectID holder, FlowNodeID node) {
        if (mayKeepAlive || !mayKeepObservedObjectAlive(inputs, node))
            return;
        bool mayBeObservedSymbol = inputs.observedLifetimes.includesSymbols && (inputs.graph().valueOf(node).primitives & Primitive::AnySymbol);
        mayKeepAlive = mayBeObservedSymbol || !refersToObservedObjectsThroughHolderOnly(inputs, holder, node);
    });
    return mayKeepAlive;
}

static FieldFlow computeFieldFlow(LayoutFieldTask& task, const FlowFieldSubject& subject)
{
    const FlowFamilyInputs& inputs = task.inputs;
    FlowLocationFacts facts = collectLocationFacts(inputs, subject);
    if (!facts.hasNode)
        return { };

    LocationEvents events = facts.events;
    FlowSiteLoss loss = findLoss(events, facts.firstLoss);
    auto isEnabled = [&](FlowResultKind kind) {
        if (inputs.isEnabled(kind, subject.layout.index()))
            return true;
        keepFirstLoss(loss, FlowSiteLoss { .cause = UnprovenCause::ResultDisabled });
        return false;
    };

    FieldFlow flow;
    LocationFlow& location = flow.location;
    if (isEnabled(FlowResultKind::StoreCounts)) {
        if (!events.contains(FlowLocationEvent::WrittenByUnknownCode))
            location.writers = WriterState::AllKnown;
        location.stores = computeFieldStores(task, subject, events);
        location.storeNaming = computeStoreNaming(inputs, events);
        location.isUnchangedDuringActivation = isUnchangedDuringActivation(location.stores, events);
    }
    location.mayKeepObservedObjectAlive = mayKeepObservedObjectAlive(inputs, subject);
    if (isEnabled(FlowResultKind::ObjectUses)) {
        location.reads = readStateOf(events);
        if (location.reads != ReadState::Unknown && subject.key && isObservedByStoresOfHeirs(inputs, subject))
            location.reads = ReadState::Unknown;
        if (location.mayKeepObservedObjectAlive)
            location.reads = ReadState::Unknown;
    }
    if (isEnabled(FlowResultKind::HolderMutations))
        flow.stability = computeStability(events);
    if (isEnabled(FlowResultKind::Initialization)) {
        location.initialization = computeFieldInitialization(inputs, subject, events);
        if (location.initialization == InitializationState::SetAtCreation && flow.stability == everyFlagOf<FieldStability>())
            flow.occupancy = Occupancy::AlwaysFilled;
    }
    if (flow.occupancy != Occupancy::AlwaysFilled && subject.key && isEnabled(FlowResultKind::OwnNames) && isAbsentOnPrototypes(inputs, subject))
        flow.prototypeName = PrototypeNameState::AbsentOnPrototypes;

    location.cause = loss.cause;
    location.witness = loss.witness;
    return flow;
}

static void computeFieldsOfLayout(const FlowFamilyInputs& inputs, const BitVector& functionsCalledByHost, LayoutID id, std::span<const PropertyKeyID> appendedKeys, FlowFieldResults& results)
{
    const Layout& layout = inputs.program().at(id);
    LayoutFieldTask task { inputs, functionsCalledByHost, id, std::nullopt, std::nullopt };

    Vector<FieldFlow>& declared = results.declared[id.index()];
    declared = Vector<FieldFlow>(layout.fields.size());
    for (uint32_t index = 0; index < declared.size(); ++index) {
        const Field& field = layout.fields[index];
        if (field.flags.contains(FieldFlags::IsInherited))
            continue;
        declared[index] = computeFieldFlow(task, FlowFieldSubject { id, index, field.key });
    }

    Vector<FieldFlow>& appended = results.appended[id.index()];
    appended.reserveInitialCapacity(appendedKeys.size());
    for (PropertyKeyID key : appendedKeys)
        appended.append(computeFieldFlow(task, FlowFieldSubject { id, std::nullopt, key }));
}

FlowFieldResults computeFieldResults(const FlowFamilyInputs& inputs, const FlowAppendedKeys& appendedKeys)
{
    size_t numberOfLayouts = inputs.program().layouts.size();
    RELEASE_ASSERT(appendedKeys.size() == numberOfLayouts);

    FlowFieldResults results;
    results.declared = Vector<Vector<FieldFlow>>(numberOfLayouts);
    results.appended = Vector<Vector<FieldFlow>>(numberOfLayouts);
    BitVector hostCalled = functionsCalledByHost(inputs.program(), inputs.loops());
    forEachIndexInParallel(inputs.loops(), numberOfLayouts, [&](unsigned index) {
        computeFieldsOfLayout(inputs, hostCalled, LayoutID(index), appendedKeys[index].span(), results);
    });
    return results;
}

std::span<const FlowReplacedDefinition> FlowFieldDefinitionPlan::replacedIn(FunctionID initializer) const
{
    auto found = std::ranges::equal_range(replaced, initializer, { }, &FlowReplacedDefinition::initializer);
    return replaced.subspan(found.begin() - replaced.begin(), found.size());
}

std::span<const FlowKeptDefinition> FlowFieldDefinitionPlan::keptIn(FlowNodeID location) const
{
    auto found = std::ranges::equal_range(kept, location, { }, &FlowKeptDefinition::location);
    return kept.subspan(found.begin() - kept.begin(), found.size());
}

static FlowNodeID locationDefinedBy(const FlowGraph& graph, const Value& definition, FlowObjectID object)
{
    if (definition.opcode == Opcode::PrivateDefine) {
        FlowFieldLocation location = findFieldLocation(graph, object, definition.layout(), definition.field());
        return location.lookup == FlowFieldLookup::Found ? location.node : FlowNodeID();
    }
    FlowFieldLocation declared = findFieldLocation(graph, object, definition.key());
    if (declared.lookup == FlowFieldLookup::Found)
        return declared.node;
    const FlowProperty* property = graph.findProperty(object, definition.key());
    return property ? property->value : FlowNodeID();
}

static std::optional<FieldReference> fieldDefinedBy(const FlowFamilyInputs& inputs, const Value& definition, FlowObjectID object)
{
    if (definition.opcode == Opcode::PrivateDefine)
        return declarationOf(inputs.program(), { definition.layout(), definition.field() });
    if (!inputs.objects().hasLayout(object))
        return std::nullopt;
    return inputs.hierarchy.findDeclaredField(inputs.objects().at(object).layout, definition.key());
}

static std::optional<Value> findValueHeldAnyway(const FlowFamilyInputs& inputs, FieldReference field)
{
    const FlowValueTable& values = inputs.solution.values();
    uint16_t primitives = 0;
    Range integers = Range::empty();
    StringID string;
    forEachFieldNode(inputs, { field.layout, field.field, fieldAt(inputs.program(), field).key }, [&](FlowObjectID, FlowNodeID node) {
        const FlowValue& value = inputs.graph().valueOf(node);
        primitives |= value.primitives;
        integers = integers.unionWith(values.rangeOf(value.integers));
        if (!string && !values.stringsOf(value.strings).empty())
            string = values.stringsOf(value.strings).front();
    });
    if (primitives & Primitive::String)
        string = inputs.program().strings.emptyString();

    if (primitives & Primitive::Integer && !integers.isEmpty()) {
        Value integer(Opcode::Integer);
        integer.setInteger(std::clamp<int64_t>(0, integers.minimum, integers.maximum));
        return integer;
    }
    if (primitives & Primitive::Boolean) {
        Value boolean(Opcode::Boolean);
        boolean.setBoolean(!(primitives & Primitive::False));
        return boolean;
    }
    if (primitives & Primitive::Null)
        return Value(Opcode::Null);
    if (string) {
        Value constant(Opcode::String);
        constant.setString(string);
        return constant;
    }
    return std::nullopt;
}

static void planFieldDefinition(const FlowFamilyInputs& inputs, FunctionID initializer, ValueID id, FlowFieldDefinitionPlan& plan)
{
    const FlowGraph& graph = inputs.graph();
    const Value& definition = inputs.program().at(initializer).at(id);

    Vector<FlowKeptDefinition, 4> kept;
    std::optional<FieldReference> field;
    bool definesOneField = true;
    graph.forEachInstanceOf(initializer, [&](FlowInstanceID instance) {
        FlowNodeID initialized = graph.nodeOf(instance, definition.objectOperand());
        FlowNodeID value = graph.nodeOf(instance, definition.valueOperand());
        RELEASE_ASSERT(initialized && value);
        graph.valueOf(initialized).objects.forEach([&](FlowObjectID object) {
            if (FlowNodeID location = locationDefinedBy(graph, definition, object))
                kept.append({ location, value });
            std::optional<FieldReference> declared = fieldDefinedBy(inputs, definition, object);
            definesOneField = definesOneField && declared && (!field || field == declared);
            field = declared;
        });
    });

    std::optional<Value> value = field && definesOneField ? findValueHeldAnyway(inputs, *field) : std::nullopt;
    if (value)
        plan.replaced.append({ initializer, id, WTF::move(*value) });
    else
        plan.kept.appendVector(kept);
}

FlowFieldDefinitionPlan planFieldDefinitions(const FlowFamilyInputs& inputs)
{
    FlowFieldDefinitionPlan plan;
    const Vector<std::unique_ptr<FlowFunctionConstruction>>& functions = inputs.solution.propertyState.constructions.functions;
    for (uint32_t index = 0; index < functions.size(); ++index) {
        if (!functions[index] || !inputs.graph().isReached(FunctionID(index)))
            continue;
        for (const FlowFieldDefinition& definition : functions[index]->fieldDefinitions) {
            if (!definition.isObserved)
                planFieldDefinition(inputs, FunctionID(index), definition.definition, plan);
        }
    }
    std::ranges::sort(plan.kept);
    removeRepeatedElements(plan.kept);
    return plan;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
