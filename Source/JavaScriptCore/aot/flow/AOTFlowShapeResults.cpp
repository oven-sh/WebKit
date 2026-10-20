/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowShapeResults.h"

#if ENABLE(AOT)

#include "AOTFlowObjectState.h"
#include "AOTFlowParts.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTFlowPrototypes.h"
#include "AOTFlowResultFamilies.h"
#include <algorithm>
#include <initializer_list>
#include <span>
#include <wtf/IterationStatus.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<FlowObjectEvent> allocationEvents {
    FlowObjectEvent::Allocated,
    FlowObjectEvent::AllocatedByConstructor,
    FlowObjectEvent::AllocatedRepeatedly,
};

static constexpr OptionSet<FlowObjectEvent> eventsWithoutKey {
    FlowObjectEvent::StoredByUnknownKey,
    FlowObjectEvent::DeletedByUnknownKey,
    FlowObjectEvent::ElementsMoved,
    FlowObjectEvent::ContentsStored,
    FlowObjectEvent::StoredUnderNumericName,
    FlowObjectEvent::HoleMade,
    FlowObjectEvent::StoredWithinLength,
};

static constexpr OptionSet<ObjectMutation> mutationsWithoutKey {
    ObjectMutation::ChangeIntegrity,
    ObjectMutation::ChangePrototype,
    ObjectMutation::BecomePrototype,
};

static constexpr OptionSet<FlowObjectUse> usesByNativeCode {
    FlowObjectUse::ReadByName,
    FlowObjectUse::ReadByUnknownKey,
    FlowObjectUse::Enumerated,
    FlowObjectUse::Called,
    FlowObjectUse::CalledRepeatedly,
    FlowObjectUse::Constructed,
};

static constexpr OptionSet<FlowObjectUse> keyListingUses {
    FlowObjectUse::Enumerated,
    FlowObjectUse::Serialized,
};

static constexpr OptionSet<FlowLocationEvent> storeEvents {
    FlowLocationEvent::StoredByOperation,
    FlowLocationEvent::StoredByManyOperations,
};

static constexpr OptionSet<FlowLocationEvent> propertyCreatingEvents = storeEvents | OptionSet<FlowLocationEvent> {
    FlowLocationEvent::WrittenByUnknownCode,
    FlowLocationEvent::Redefined,
};

static constexpr OptionSet<ObjectMutation> elementStorageMutations {
    ObjectMutation::ReachUnknownJavaScript,
    ObjectMutation::RedefineProperty,
    ObjectMutation::ChangePrototype,
};

static constexpr OptionSet<ObjectMutation> mutationsInvisibleToInheritors {
    ObjectMutation::ReachNativeCode,
    ObjectMutation::BecomePrototype,
};

namespace {

struct UnknownObjectRecords {
    OptionSet<ObjectMutation> mutations;
    OptionSet<FlowObjectEvent> events;
    OptionSet<FlowObjectUse> uses;
    unsigned numberOfWrittenKeys { 0 };
    bool mayGetIndexedAccessor { false };
};

struct AllocationClassTask {
    const FlowFamilyInputs& inputs;
    std::span<const ClassFlow> classesByLayout;
    const FlowAppendedKeys& appendedKeys;
    const FlowBuiltinTables& builtinLocations;
    std::span<const PropertyKeyID> keysOfUnknownObject;
    const FlowObjectSet& objectsWithReplacedContents;
    const FlowObjectSet& denseArrays;
    AllocationClassID id;
    const FlowAllocationClassMembers& members;
    FlowObjectFacts joined;
};

struct OtherNewTarget {
    bool isPossible { false };
    Witness witness { noWitness };
};

struct ConstructedClass {
    LayoutID layout;
    BuiltinClassID builtinClass;
};

} // anonymous namespace

static bool hasIndexedAccessor(const FlowSolution& solution, FlowObjectID object)
{
    const FlowGraph& graph = solution.graph;
    for (const FlowProperty& property : graph.propertiesOf(object)) {
        if ((property.getter || property.setter) && classOf(solution.program, property.key) == FlowKeyClass::Index)
            return true;
    }
    for (FlowOpenPart part : { FlowOpenPart::IndexedProperties, FlowOpenPart::StringProperties }) {
        FlowNodeID node = graph.find(object, part);
        if (node && solution.records.recordOf(node).events.contains(FlowLocationEvent::Redefined))
            return true;
    }
    return false;
}

static bool hasIndexKeyStores(const FlowSolution& solution, FlowObjectID object)
{
    if (indexedElementSlotOf(solution.builtins(), solution.objects.at(object).builtinClass))
        return false;
    return hasUnnamedKeyStores(FlowWalkInputs::of(solution), object, FlowOpenPart::IndexedProperties);
}

static bool mayBecomeOwnProperty(const FlowSolution& solution, FlowObjectID object, const FlowProperty& property)
{
    if (property.getter || property.setter)
        return true;
    if (!property.value || solution.graph.kindOf(property.value) != FlowNodeKind::Property)
        return false;
    OptionSet<FlowLocationEvent> events = solution.records.recordOf(property.value).events | aliasedEventsOf(solution, object, property.key);
    return events.containsAny(propertyCreatingEvents);
}

static UnknownObjectRecords collectUnknownObjectRecords(const FlowSolution& solution)
{
    const FlowRecords& records = solution.records;
    UnknownObjectRecords unknown;
    unknown.mutations = records.possibleMutations(unknownFlowObject);
    unknown.events = records.eventsOf(unknownFlowObject);
    unknown.uses = records.usesOf(unknownFlowObject);
    for (const FlowProperty& property : solution.graph.propertiesOf(unknownFlowObject)) {
        if (mayBecomeOwnProperty(solution, unknownFlowObject, property))
            ++unknown.numberOfWrittenKeys;
    }
    unknown.mayGetIndexedAccessor = hasIndexedAccessor(solution, unknownFlowObject);
    return unknown;
}

static void forEachKeyedLocation(const FlowSolution& solution, FlowObjectID object, NOESCAPE const Invocable<void(PropertyKeyID, FlowNodeID)> auto& functor)
{
    const FlowGraph& graph = solution.graph;
    if (solution.objects.hasLayout(object)) {
        const FlowObject& record = solution.objects.at(object);
        const Layout& layout = solution.program.at(record.layout);
        for (uint32_t index = 0; index < record.numberOfDeclaredParts; ++index) {
            if (PropertyKeyID key = layout.fields[index].key)
                functor(key, graph.field(object, index));
        }
    }
    for (const FlowProperty& property : graph.propertiesOf(object)) {
        if (!property.value || graph.kindOf(property.value) != FlowNodeKind::DeclaredPart)
            functor(property.key, property.value);
    }
}

// The slots of an instance hold the fields of the classes above its own, also where no constructor above has stored them into this object: a constructor nobody sees made it.
template<typename Functor>
static void forEachKeyOfClassesAbove(const Program& program, LayoutID layout, const Functor& functor)
{
    size_t steps = 0;
    for (LayoutID above = predictedHeritageOf(program.at(layout)); above && steps++ < program.layouts.size(); above = predictedHeritageOf(program.at(above))) {
        for (const Field& field : program.at(above).fields) {
            if (field.key)
                functor(field.key);
        }
    }
}

static void addRecordsOfUnknownObject(const FlowSolution& solution, const UnknownObjectRecords& unknown, FlowObjectID object, FlowObjectFacts& facts)
{
    bool isStoredByUnknownKey = unknown.events.contains(FlowObjectEvent::StoredByUnknownKey);

    facts.mutations.add(ObjectMutation::ReachNativeCode);
    facts.mutations.add(unknown.mutations & mutationsWithoutKey);
    facts.events.add(unknown.events & eventsWithoutKey);
    facts.uses.add(usesByNativeCode | unknown.uses);
    if (isStoredByUnknownKey)
        facts.mutations.add({ ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation });
    if (unknown.events.contains(FlowObjectEvent::ContentsStored))
        facts.mutations.add(ObjectMutation::StoreAfterCreation);
    if (unknown.events.contains(FlowObjectEvent::StoredUnderNumericName))
        facts.mutations.add(ObjectMutation::AddProperty);
    if (unknown.events.contains(FlowObjectEvent::DeletedByUnknownKey))
        facts.mutations.add(ObjectMutation::DeleteField);
    forEachKeyCreatedWithObject(solution.program, solution.objects, object, [&](PropertyKeyID key) {
        if (aliasedEventsOf(solution, object, key).contains(FlowLocationEvent::Deleted))
            facts.mutations.add(ObjectMutation::DeleteField);
    });
    if (solution.objects.hasLayout(object)) {
        forEachKeyOfClassesAbove(solution.program, solution.objects.at(object).layout, [&](PropertyKeyID key) {
            OptionSet<FlowLocationEvent> events = aliasedEventsOf(solution, object, key);
            if (events.contains(FlowLocationEvent::Deleted))
                facts.mutations.add(ObjectMutation::DeleteField);
            if (events.contains(FlowLocationEvent::Redefined))
                facts.mutations.add(ObjectMutation::RedefineProperty);
        });
    }

    unsigned numberOfMatchedKeys = 0;
    forEachKeyedLocation(solution, object, [&](PropertyKeyID key, FlowNodeID location) {
        OptionSet<FlowLocationEvent> events = aliasedEventsOf(solution, object, key);
        if (events.contains(FlowLocationEvent::Deleted))
            facts.mutations.add(ObjectMutation::DeleteField);
        if (events.contains(FlowLocationEvent::Redefined))
            facts.mutations.add(ObjectMutation::RedefineProperty);
        const FlowProperty* written = solution.graph.findProperty(unknownFlowObject, key);
        if (written && mayBecomeOwnProperty(solution, unknownFlowObject, *written))
            ++numberOfMatchedKeys;

        bool isStoredByName = events.containsAny(storeEvents);
        if (!isStoredByName && !(isStoredByUnknownKey && unnamedKeyPartOf(classOf(solution.program, key))))
            return;
        facts.mutations.add(ObjectMutation::StoreAfterCreation);
        if (isTypedLocation(solution.graph, location))
            facts.mutations.add(ObjectMutation::StoreOfOtherType);
    });

    if (numberOfMatchedKeys < unknown.numberOfWrittenKeys) {
        facts.mutations.add(ObjectMutation::AddProperty);
        facts.mayGetKeysOfUnknownObject = true;
    }
    if (unknown.mayGetIndexedAccessor) {
        facts.mutations.add({ ObjectMutation::RedefineProperty, ObjectMutation::AddProperty });
        facts.mayGetIndexedAccessor = true;
        facts.mayGetUnlistedKey = true;
    }
}

static FlowObjectFacts computeFactsOf(const FlowSolution& solution, const UnknownObjectRecords& unknown, FlowObjectID object)
{
    const FlowRecords& records = solution.records;
    FlowObjectFacts facts;
    facts.events = records.eventsOf(object);
    if (solution.objectsInUnknownJavaScript().contains(object)) {
        facts.isStoredUnderUnnamedKeys = facts.events.contains(FlowObjectEvent::StoredByUnknownKey) || hasIndexKeyStores(solution, object);
        facts.mutations = allObjectMutations;
        facts.events.add(OptionSet<FlowObjectEvent>::all() - allocationEvents);
        facts.uses = OptionSet<FlowObjectUse>::all();
        facts.mayGetUnlistedKey = true;
        facts.mayGetIndexKey = true;
        facts.mayGetIndexedAccessor = true;
        return facts;
    }

    facts.mutations = records.possibleMutations(object);
    facts.uses = records.usesOf(object);
    facts.mayGetIndexedAccessor = hasIndexedAccessor(solution, object);
    if (sharesLocationsWithUnknownObject(FlowWalkInputs::of(solution), object))
        addRecordsOfUnknownObject(solution, unknown, object, facts);
    if (facts.events.contains(FlowObjectEvent::StoredByUnknownKey))
        facts.mayGetUnlistedKey = true;
    if (hasIndexKeyStores(solution, object)) {
        facts.mutations.add(ObjectMutation::AddProperty);
        facts.mayGetIndexKey = true;
    }
    facts.isStoredUnderUnnamedKeys = facts.mayGetUnlistedKey || facts.mayGetIndexKey;
    return facts;
}

Vector<FlowObjectFacts> computeObjectFacts(const FlowSolution& solution)
{
    UnknownObjectRecords unknown = collectUnknownObjectRecords(solution);
    unsigned numberOfObjects = solution.objects.size();
    Vector<FlowObjectFacts> factsByObject(numberOfObjects);
    forEachIndexInParallel(solution.build.parallelLoops(), numberOfObjects, [&](unsigned index) {
        factsByObject[index] = computeFactsOf(solution, unknown, FlowObjectID(index));
    });
    return factsByObject;
}

static void forEachAllocatedObject(const FlowFamilyInputs& inputs, AllocationClassID allocationClass, NOESCAPE const Invocable<void(FlowObjectID)> auto& functor)
{
    forEachObjectOfClass(inputs.objects(), allocationClass, [&](FlowObjectID object) {
        if (inputs.factsOf(object).events.contains(FlowObjectEvent::Allocated))
            functor(object);
    });
}

static void forEachLayoutInProvenChain(const FlowHierarchy& hierarchy, LayoutID layout, NOESCAPE const Invocable<IterationStatus(LayoutID)> auto& functor)
{
    LayoutID current = layout;
    while (functor(current) == IterationStatus::Continue) {
        const FlowBaseResult& base = hierarchy.baseOf(current);
        if (base.state != BaseState::Layout)
            return;
        current = base.base;
    }
}

static FlowObjectFacts joinFacts(const FlowFamilyInputs& inputs, AllocationClassID allocationClass)
{
    FlowObjectFacts joined;
    forEachAllocatedObject(inputs, allocationClass, [&](FlowObjectID object) {
        const FlowObjectFacts& facts = inputs.factsOf(object);
        joined.mutations.add(facts.mutations);
        joined.events.add(facts.events);
        joined.uses.add(facts.uses);
        joined.mayGetUnlistedKey |= facts.mayGetUnlistedKey;
        joined.mayGetIndexKey |= facts.mayGetIndexKey;
        joined.mayGetKeysOfUnknownObject |= facts.mayGetKeysOfUnknownObject;
        joined.mayGetIndexedAccessor |= facts.mayGetIndexedAccessor;
        joined.isStoredUnderUnnamedKeys |= facts.isStoredUnderUnnamedKeys;
    });
    return joined;
}

static bool areMutationsEnabled(const AllocationClassTask& task)
{
    FlowResultKind kind = task.members.builtinClass ? FlowResultKind::PristineInstances : FlowResultKind::HolderMutations;
    return task.inputs.isEnabled(kind, task.id.index());
}

static Witness findLowestWitness(const AllocationClassTask& task, ObjectMutation mutation)
{
    const FlowRecords& records = task.inputs.records();
    Witness witness = noWitness;
    forEachAllocatedObject(task.inputs, task.id, [&](FlowObjectID object) {
        keepLowerWitness(witness, records.witnessOf(object, mutation));
    });
    return hasFunction(witness) ? witness : records.witnessOf(unknownFlowObject, mutation);
}

static void keepWitnessOfIndexedAccessor(const FlowSolution& solution, FlowObjectID object, Witness& witness)
{
    const FlowGraph& graph = solution.graph;
    for (const FlowProperty& property : graph.propertiesOf(object)) {
        if (classOf(solution.program, property.key) != FlowKeyClass::Index)
            continue;
        for (FlowNodeID accessor : { property.getter, property.setter }) {
            if (accessor)
                keepLowerWitness(witness, solution.records.recordOf(accessor).firstStore);
        }
    }
    for (FlowOpenPart part : { FlowOpenPart::IndexedProperties, FlowOpenPart::StringProperties }) {
        FlowNodeID node = graph.find(object, part);
        if (node && solution.records.recordOf(node).events.contains(FlowLocationEvent::Redefined))
            keepLowerWitness(witness, solution.records.recordOf(node).firstLoss);
    }
}

static Witness findWitnessOfIndexedAccessor(const AllocationClassTask& task)
{
    const FlowSolution& solution = task.inputs.solution;
    Witness witness = noWitness;
    forEachAllocatedObject(task.inputs, task.id, [&](FlowObjectID object) {
        keepWitnessOfIndexedAccessor(solution, object, witness);
    });
    if (!hasFunction(witness))
        keepWitnessOfIndexedAccessor(solution, unknownFlowObject, witness);
    return hasFunction(witness) ? witness : findLowestWitness(task, ObjectMutation::RedefineProperty);
}

static OtherNewTarget findOtherNewTarget(const FlowFamilyInputs& inputs, LayoutID layout)
{
    OtherNewTarget result;
    FunctionID constructor = layout ? inputs.program().at(layout).constructor : FunctionID();
    if (!constructor)
        return result;
    for (FlowObjectID closure : inputs.objects().closuresOf(constructor)) {
        if (!inputs.factsOf(closure).events.contains(FlowObjectEvent::OtherNewTarget))
            continue;
        result.isPossible = true;
        keepLowerWitness(result.witness, inputs.records().witnessOf(closure, FlowObjectEvent::OtherNewTarget));
    }
    return result;
}

static bool holdsOnlyBuiltin(const FlowFamilyInputs& inputs, FlowNodeID prototypeLink, BuiltinID builtin)
{
    const FlowValue& prototypes = inputs.graph().valueOf(prototypeLink);
    FlowObjectID expected = inputs.objects().findBuiltin(builtin);
    return !(prototypes.primitives & Primitive::Null) && prototypes.objects.allOf([&](FlowObjectID prototype) {
        return prototype == expected;
    });
}

static bool isUnmodifiedGeneratorPrototype(const FlowFamilyInputs& inputs, FlowObjectID object, BuiltinID parent)
{
    const FlowObject& record = inputs.objects().at(object);
    if (record.kind != FlowObjectKind::Prototype || !inputs.program().at(record.function).has(FunctionFlags::IsGenerator))
        return false;
    const FlowObjectFacts& facts = inputs.factsOf(object);
    if (!facts.mutations.containsOnly(mutationsInvisibleToInheritors) || facts.mayGetUnlistedKey)
        return false;
    bool mayGetOwnProperty = std::ranges::any_of(inputs.graph().propertiesOf(object), [&](const FlowProperty& property) {
        return mayBecomeOwnProperty(inputs.solution, object, property);
    });
    if (mayGetOwnProperty)
        return false;
    if (defaultPrototypeOf(inputs.program(), inputs.objects(), object) != FlowDefaultPrototype { FlowDefaultPrototypeKind::Builtin, parent })
        return false;
    FlowNodeID link = inputs.graph().find(object, FlowOpenPart::PrototypeLink);
    return !link || holdsOnlyBuiltin(inputs, link, parent);
}

static bool mayHaveOtherPrototype(const FlowFamilyInputs& inputs, FlowObjectID object)
{
    FlowNodeID link = inputs.graph().find(object, FlowOpenPart::PrototypeLink);
    if (!link)
        return false;

    const FlowValue& prototypes = inputs.graph().valueOf(link);
    FlowDefaultPrototype original = defaultPrototypeOf(inputs.program(), inputs.objects(), object);
    switch (original.kind) {
    case FlowDefaultPrototypeKind::Unknown:
        return true;
    case FlowDefaultPrototypeKind::Null:
        return prototypes.mayBeObject();
    case FlowDefaultPrototypeKind::Builtin:
        break;
    }
    if (prototypes.primitives & Primitive::Null)
        return true;
    FlowObjectID originalObject = inputs.objects().findBuiltin(original.builtin);
    return prototypes.objects.anyOf([&](FlowObjectID prototype) {
        return prototype != originalObject && !isUnmodifiedGeneratorPrototype(inputs, prototype, original.builtin);
    });
}

static bool mayBeCreatedWithOtherPrototype(const AllocationClassTask& task)
{
    LayoutID layout = declaredLayoutOf(task.inputs.program(), task.members.layout);
    // An object keeps the layout of its constructor whatever the prototype property of the constructor holds when it is allocated.
    if (layout && task.inputs.program().at(layout).kind == LayoutKind::Instance) {
        bool hasConstructor = !!task.inputs.program().at(layout).constructor;
        const ClassFlow& classFlow = task.classesByLayout[layout.index()];
        bool hasOnePrototypeObject = classFlow.prototypeProperty == MemberMutability::Fixed || classFlow.hasAssignedPrototype;
        return hasConstructor && (findOtherNewTarget(task.inputs, layout).isPossible || !hasOnePrototypeObject);
    }

    bool result = false;
    forEachAllocatedObject(task.inputs, task.id, [&](FlowObjectID object) {
        result |= mayHaveOtherPrototype(task.inputs, object);
    });
    return result;
}

static bool isDeclaredOrAppendedField(const AllocationClassTask& task, PropertyKeyID key)
{
    const FlowHierarchy& hierarchy = task.inputs.hierarchy;
    if (hierarchy.findDeclaredField(task.members.layout, key))
        return true;

    bool isAppended = false;
    forEachLayoutInProvenChain(hierarchy, task.members.layout, [&](LayoutID layout) {
        isAppended = std::ranges::binary_search(task.appendedKeys[layout.index()], key);
        return isAppended ? IterationStatus::Done : IterationStatus::Continue;
    });
    return isAppended;
}

static Vector<PropertyKeyID> collectKeysOfUnknownObject(const FlowSolution& solution)
{
    Vector<PropertyKeyID> keys;
    for (const FlowProperty& property : solution.graph.propertiesOf(unknownFlowObject)) {
        if (mayBecomeOwnProperty(solution, unknownFlowObject, property))
            keys.append(property.key);
    }
    return keys;
}

static Vector<PropertyKeyID> collectAddedKeys(const AllocationClassTask& task)
{
    const FlowSolution& solution = task.inputs.solution;
    Vector<PropertyKeyID> keys;
    auto addUnlessField = [&](PropertyKeyID key) {
        if (!task.members.layout || !isDeclaredOrAppendedField(task, key))
            keys.append(key);
    };

    forEachAllocatedObject(task.inputs, task.id, [&](FlowObjectID object) {
        for (const FlowProperty& property : solution.graph.propertiesOf(object)) {
            if (mayBecomeOwnProperty(solution, object, property))
                addUnlessField(property.key);
        }
    });
    if (task.joined.mayGetKeysOfUnknownObject) {
        for (PropertyKeyID key : task.keysOfUnknownObject)
            addUnlessField(key);
    }
    std::ranges::sort(keys);
    removeRepeatedElements(keys);
    return keys;
}

static bool mayGetAccessor(const AllocationClassTask& task)
{
    bool result = task.joined.mayGetIndexedAccessor;
    forEachAllocatedObject(task.inputs, task.id, [&](FlowObjectID object) {
        result |= std::ranges::any_of(task.inputs.graph().propertiesOf(object), [](const FlowProperty& property) {
            return property.getter || property.setter;
        });
    });
    return result;
}

// The solver saw an appended field as an added property, whose deletion it records on the location alone.
static bool mayDeleteAppendedField(const AllocationClassTask& task)
{
    const FlowSolution& solution = task.inputs.solution;
    bool result = false;
    forEachAllocatedObject(task.inputs, task.id, [&](FlowObjectID object) {
        for (const FlowProperty& property : solution.graph.propertiesOf(object)) {
            if (!property.value || solution.graph.kindOf(property.value) != FlowNodeKind::Property)
                continue;
            OptionSet<FlowLocationEvent> events = solution.records.recordOf(property.value).events | aliasedEventsOf(solution, object, property.key);
            result |= events.contains(FlowLocationEvent::Deleted) && isDeclaredOrAppendedField(task, property.key);
        }
    });
    return result;
}

static void forEachFieldInProvenChain(const AllocationClassTask& task, NOESCAPE const Invocable<void(const FlowFieldSubject&)> auto& functor)
{
    forEachLayoutInProvenChain(task.inputs.hierarchy, task.members.layout, [&](LayoutID layout) {
        const Vector<Field>& fields = task.inputs.program().at(layout).fields;
        for (uint32_t index = 0; index < fields.size(); ++index) {
            if (fields[index].key)
                functor(FlowFieldSubject { layout, index, fields[index].key });
        }
        for (PropertyKeyID key : task.appendedKeys[layout.index()])
            functor(FlowFieldSubject { layout, std::nullopt, key });
        return IterationStatus::Continue;
    });
}

// After the application the object has every field of its proven chain, the appended ones too. The solver took an appended field, and a field of a base that nobody predicted, for an added property:
// a store to it counted as an addition, and no type was enforced. The type of a field is made from the locations under its key, so a store that reaches no location may bring a value outside it.
// What is done by name through the unknown object is added key by key for the locations of an escaped object, and an object without a location under the key has the field all the same.
static OptionSet<ObjectMutation> mutationsOfFieldsTakenForProperties(const AllocationClassTask& task)
{
    constexpr OptionSet<ObjectMutation> storesOfAnyValue { ObjectMutation::StoreAfterCreation, ObjectMutation::StoreOfOtherType };
    const FlowFamilyInputs& inputs = task.inputs;
    OptionSet<ObjectMutation> mutations;
    forEachFieldInProvenChain(task, [&](const FlowFieldSubject& field) {
        forEachAllocatedObject(inputs, task.id, [&](FlowObjectID object) {
            FlowNodeID location = fieldNodeOn(inputs.graph(), object, field);
            OptionSet<FlowLocationEvent> aliasedEvents = aliasedEventsOf(inputs.solution, object, field.key);
            if (!location) {
                bool mayBeStored = mayBeStoredWithoutLocation(inputs, object, field.key);
                bool mayBeRedefined = aliasedEvents.contains(FlowLocationEvent::Redefined);
                if (mayBeStored)
                    mutations.add(storesOfAnyValue);
                if (mayBeRedefined)
                    mutations.add(ObjectMutation::RedefineProperty);
                if ((mayBeStored || mayBeRedefined) && aliasedEvents.contains(FlowLocationEvent::Deleted))
                    mutations.add(ObjectMutation::DeleteField);
                return;
            }
            if (inputs.graph().kindOf(location) != FlowNodeKind::Property)
                return;
            if ((inputs.records().recordOf(location).events | aliasedEvents).containsAny(storeEvents))
                mutations.add(ObjectMutation::StoreAfterCreation);
            if (holdsValueOutsideFieldType(inputs, field, location))
                mutations.add(ObjectMutation::StoreOfOtherType);
        });
    });
    return mutations;
}

static OptionSet<ObjectMutation> possibleMutationsOf(const AllocationClassTask& task, bool hasAddedKeys)
{
    OptionSet<ObjectMutation> possible = task.joined.mutations;
    bool mayAddProperty = hasAddedKeys || task.joined.mayGetUnlistedKey || task.joined.mayGetIndexKey || task.joined.events.contains(FlowObjectEvent::StoredUnderNumericName);
    if (task.members.layout)
        possible.set(ObjectMutation::AddProperty, mayAddProperty);
    else if (mayAddProperty)
        possible.add(ObjectMutation::AddProperty);
    if (task.members.layout && mayDeleteAppendedField(task))
        possible.add(ObjectMutation::DeleteField);
    if (task.members.layout)
        possible.add(mutationsOfFieldsTakenForProperties(task));
    if (mayGetAccessor(task))
        possible.add(ObjectMutation::RedefineProperty);
    if (mayBeCreatedWithOtherPrototype(task))
        possible.add(ObjectMutation::ChangePrototype);
    return possible;
}

static bool isUnchangedHolder(const BuiltinTable& builtins, const FlowBuiltinTables& locations, BuiltinID holder)
{
    if (builtins.hasLockedPrototypeLink(holder))
        return true;
    if (!locations.intactHolders.get(holder.index()))
        return false;
    auto locationsOfHolder = std::ranges::equal_range(locations.locations, holder, { }, &BuiltinLocationFlow::holder);
    return std::ranges::all_of(locationsOfHolder, [](const BuiltinLocationFlow& location) {
        return location.state == BuiltinLocationState::Constant;
    });
}

// Visits the holders from the first one up its prototype chain until the functor answers Done. False when a holder may have changed or the table does not name the next one.
static bool walkUnchangedChain(const AllocationClassTask& task, BuiltinID first, NOESCAPE const Invocable<IterationStatus(BuiltinID)> auto& functor)
{
    const BuiltinTable& builtins = task.inputs.solution.builtins();
    BuiltinID holder = builtins.canonicalID(first);
    while (true) {
        if (functor(holder) == IterationStatus::Done)
            return true;
        if (!isUnchangedHolder(builtins, task.builtinLocations, holder))
            return false;
        const Builtin& object = builtins.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = builtins.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

static bool inheritsUnchangedMembers(const AllocationClassTask& task)
{
    const BuiltinClass& instanceClass = task.inputs.solution.builtins().at(task.members.builtinClass);
    if (!instanceClass.prototype || instanceClass.isPrimitiveWrapper)
        return false;
    return walkUnchangedChain(task, instanceClass.prototype, [](BuiltinID) {
        return IterationStatus::Continue;
    });
}

static bool hasDefaultSpecies(const AllocationClassTask& task)
{
    const BuiltinTable& builtins = task.inputs.solution.builtins();
    BuiltinID constructor = builtins.at(task.members.builtinClass).constructor;
    if (!constructor)
        return true;

    bool isConstant = true;
    bool isChainUnchanged = walkUnchangedChain(task, constructor, [&](BuiltinID holder) {
        BuiltinID species = builtins.findOwnMember(holder, BuiltinNames::speciesSymbol);
        if (!species)
            return IterationStatus::Continue;
        isConstant = task.builtinLocations.constantMembers.get(species.index());
        return IterationStatus::Done;
    });
    return isChainUnchanged && isConstant;
}

static FlowObjectSet collectObjectsWithReplacedContents(const FlowFamilyInputs& inputs)
{
    FlowObjectSet objects;
    for (const FlowBuiltinOperandValue& value : inputs.records().builtinOperandValues()) {
        if (value.node && value.roles.contains(BuiltinOperandRole::ReplacesContents))
            objects.unite(inputs.graph().valueOf(value.node).objects);
    }
    return objects;
}

// An array has no holes when its creator fills every index below the length and nothing that reaches it makes a hole. A copy has the holes of its sources.
static FlowObjectSet collectDenseArrays(const FlowFamilyInputs& inputs)
{
    const FlowObjectTable& objects = inputs.objects();
    FlowObjectSet denseArrays;
    for (unsigned index = 0; index < objects.size(); ++index) {
        FlowObjectID object(index);
        const FlowObject& record = objects.at(object);
        if (record.kind != FlowObjectKind::Instance || record.builtinClass != BuiltinClasses::Array || !record.site)
            continue;
        // An index that was below the length is below it for good only in an array that never gets shorter.
        OptionSet<FlowObjectEvent> events = inputs.factsOf(object).events;
        bool mayGetHole = events.contains(FlowObjectEvent::HoleMade) || events.containsAll({ FlowObjectEvent::StoredWithinLength, FlowObjectEvent::ElementsMoved });
        if (objects.at(record.site).description.isCreatedDense && !mayGetHole)
            denseArrays.add(object);
    }

    for (bool changed = true; changed;) {
        changed = false;
        for (const FlowHoleCopy& copy : inputs.records().holeCopies()) {
            if (denseArrays.contains(copy.result) && !inputs.graph().valueOf(copy.source).objects.isSubsetOf(denseArrays))
                changed |= denseArrays.remove(copy.result);
        }
    }
    return denseArrays;
}

static bool hasOnlyDenseArrays(const AllocationClassTask& task)
{
    bool areDense = true;
    forEachObjectOfClass(task.inputs.objects(), task.id, [&](FlowObjectID object) {
        areDense &= task.denseArrays.contains(object);
    });
    return areDense;
}

// A match converts lastIndex to a length, and the conversion of anything but a number can run code of the program or throw.
static bool keepsPatternAndNumericLastIndex(const AllocationClassTask& task)
{
    const FlowGraph& graph = task.inputs.graph();
    bool isKept = true;
    forEachObjectOfClass(task.inputs.objects(), task.id, [&](FlowObjectID object) {
        FlowNodeID lastIndex = graph.slot(object, BuiltinSlots::RegExpLastIndex);
        if (!lastIndex || task.objectsWithReplacedContents.contains(object)) {
            isKept = false;
            return;
        }
        const FlowValue& stored = graph.valueOf(lastIndex);
        if (stored.mayBeObject() || (stored.primitives & ~Primitive::Number))
            isKept = false;
    });
    return isKept;
}

// Storage says that the elements lie as the storage kind says and that no access under a number meets an accessor. A data property under a name like "-1" leaves both true,
// so an array may have lost the mark of its structure and keep the proof. The prototype chain of the instance is covered, as it is by the test at run time.
static OptionSet<PristineAspects> provenPristineAspects(const AllocationClassTask& task, OptionSet<ObjectMutation> impossibleMutations)
{
    OptionSet<PristineAspects> aspects;
    if (!inheritsUnchangedMembers(task))
        return aspects;
    bool isArray = task.members.builtinClass == BuiltinClasses::Array;
    if (isArray && !impossibleMutations.contains(ObjectMutation::ChangeIntegrity))
        return aspects;
    if (impossibleMutations.containsAll(elementStorageMutations))
        aspects.add(PristineAspects::Storage);
    if (isArray && aspects.contains(PristineAspects::Storage) && hasOnlyDenseArrays(task))
        aspects.add(PristineAspects::Dense);
    if (impossibleMutations.containsAll(builtinBehaviorMutations) && hasDefaultSpecies(task))
        aspects.add(PristineAspects::Names);
    if (task.members.builtinClass != BuiltinClasses::RegExp || !aspects.contains(PristineAspects::Names))
        return aspects;
    if (impossibleMutations.containsAll(reachMutations) && keepsPatternAndNumericLastIndex(task))
        aspects.add(PristineAspects::AsCreated);
    return aspects;
}

static FlowSiteLoss findDisabledResult(const AllocationClassTask& task)
{
    if (task.inputs.isEnabled(FlowResultKind::ProvenLayoutSets, task.members.layout.index()) && areMutationsEnabled(task))
        return { };
    return { UnprovenCause::ResultDisabled, LimitID::None, noWitness };
}

static Witness witnessOfAllocation(const FlowFamilyInputs& inputs, FlowObjectID object, FunctionID function)
{
    const FlowObjectTable& objects = inputs.objects();
    Witness witness = noWitness;
    witness.location = objects.locationOf(object);
    if (inputs.graph().isHost(function))
        return witness;
    witness.function = function.index();
    AllocationSiteID site = objects.at(object).site;
    if (ValueID value = site ? objects.at(site).description.value : ValueID())
        witness.value = value.index();
    return witness;
}

static FlowSiteLoss findWorkLimitLoss(const AllocationClassTask& task)
{
    FlowSiteLoss loss;
    forEachAllocatedObject(task.inputs, task.id, [&](FlowObjectID object) {
        FunctionID function = task.inputs.objects().allocatingFunctionOf(object);
        if (!function || !task.inputs.solution.isPastWorkLimit(function))
            return;
        keepFirstLoss(loss, { UnprovenCause::LimitReached, LimitID::MaxFlowWorkPerFunction, witnessOfAllocation(task.inputs, object, function) });
    });
    return loss;
}

// The constructor of the built-in class makes the instances, whatever operation the solver saw allocate one.
static FlowSiteLoss findBuiltinBase(const AllocationClassTask& task)
{
    const FlowBaseResult* base = task.inputs.hierarchy.findBuiltinBase(task.members.layout);
    if (!base)
        return { };
    return { UnprovenCause::BuiltinBase, LimitID::None, base->witness };
}

static FlowSiteLoss findUnknownBase(const AllocationClassTask& task)
{
    const FlowHierarchy& hierarchy = task.inputs.hierarchy;
    if (hierarchy.hasProvenChain(task.members.layout))
        return { };

    LayoutID last = task.members.layout;
    forEachLayoutInProvenChain(hierarchy, task.members.layout, [&](LayoutID layout) {
        last = layout;
        return IterationStatus::Continue;
    });
    return { UnprovenCause::UnknownBase, LimitID::None, hierarchy.baseOf(last).witness };
}

static const Value* findAllocatingOperation(const FlowFamilyInputs& inputs LIFETIME_BOUND, const FlowSite& site)
{
    const FlowSiteDescription& description = site.description;
    if (description.kind != FlowSiteKind::Layout || description.origin != FlowSiteOrigin::Operation || !description.value)
        return nullptr;
    if (!site.function || inputs.graph().isHost(site.function))
        return nullptr;
    return &inputs.program().at(site.function).at(description.value);
}

static bool takesNewTargetFromOperand(const BuiltinOverload& overload)
{
    return std::ranges::any_of(overload.flow.callbacks, [](const BuiltinCallback& callback) {
        return callback.constructs && callback.newTarget.kind != BuiltinPlaceKind::None;
    });
}

// The link of an abandoned call names no overload.
static bool takesNewTargetFromOperand(const BuiltinTable& builtins, const FlowBuiltinCallLink& link)
{
    if (link.overload)
        return takesNewTargetFromOperand(builtins.at(link.overload));
    return std::ranges::any_of(builtins.overloadsOf(builtins.canonicalID(link.builtin)), [](const BuiltinOverload& overload) {
        return takesNewTargetFromOperand(overload);
    });
}

// Without a new.target of its own each callee is its own new.target, so the object has the layout of the constructor that runs.
// A built-in that constructs one of its operands may take new.target from another: the object is made at the call of the built-in.
static bool passesNewTarget(const FlowFamilyInputs& inputs, const FlowSite& site, const Value& operation)
{
    if (OperandPosition::newTarget(operation.opcode))
        return !!operation.newTargetOperand();

    const BuiltinTable& builtins = inputs.solution.builtins();
    bool passes = false;
    inputs.graph().forEachInstanceOf(site.function, [&](FlowInstanceID instance) {
        passes = passes || std::ranges::any_of(inputs.calls.builtinLinksAt(instance, site.description.value), [&](const FlowBuiltinCallLink& link) {
            return takesNewTargetFromOperand(builtins, link);
        });
    });
    return passes;
}

static void forEachConstructedClass(const FlowFamilyInputs& inputs, const FlowSite& site, NOESCAPE const Invocable<void(const ConstructedClass&)> auto& functor)
{
    const Value* operation = findAllocatingOperation(inputs, site);
    if (!operation)
        return;
    if (PayloadWord::layout(operation->opcode))
        functor(ConstructedClass { operation->layout(), { } });

    if (!passesNewTarget(inputs, site, *operation))
        return;

    const BuiltinTable& builtins = inputs.solution.builtins();
    ValueID call = site.description.value;
    inputs.graph().forEachInstanceOf(site.function, [&](FlowInstanceID instance) {
        for (FlowCallLinkID id : inputs.calls.linksAt(instance, call)) {
            const FlowCallLink& link = inputs.records().at(id);
            // The implementation of a built-in has no closure, and the link of the built-in names its class.
            if (link.kind == FlowCallLinkKind::Construct && !inputs.objects().closuresOf(link.callee).empty())
                functor(ConstructedClass { inputs.program().at(link.callee).constructedClass, { } });
        }
        for (const FlowBuiltinCallLink& link : inputs.calls.builtinLinksAt(instance, call)) {
            if (link.mode == BuiltinCallMode::Construct)
                functor(ConstructedClass { { }, builtins.at(builtins.canonicalID(link.builtin)).instanceClass });
        }
    });
}

static bool isProvenBelow(const FlowHierarchy& hierarchy, LayoutID layout, const ConstructedClass& ancestor)
{
    bool isBelow = false;
    forEachLayoutInProvenChain(hierarchy, layout, [&](LayoutID current) {
        const FlowBaseResult& base = hierarchy.baseOf(current);
        isBelow = current == ancestor.layout || (base.state == BaseState::BuiltinClass && base.baseClass == ancestor.builtinClass);
        return isBelow ? IterationStatus::Done : IterationStatus::Continue;
    });
    return isBelow;
}

static FlowSiteLoss findUnprovenDerivedAllocation(const AllocationClassTask& task)
{
    FlowSiteLoss loss;
    for (AllocationSiteID site : task.members.sites) {
        forEachConstructedClass(task.inputs, task.inputs.objects().at(site), [&](const ConstructedClass& constructed) {
            if (isProvenBelow(task.inputs.hierarchy, task.members.layout, constructed))
                return;
            keepFirstLoss(loss, { UnprovenCause::OtherNewTarget, LimitID::None, findOtherNewTarget(task.inputs, constructed.layout).witness });
        });
    }
    return loss;
}

// What a constructor nobody sees makes for new.target need not have the shape of the layout, whatever the chain of the class says.
static FlowSiteLoss findUnknownConstructor(const AllocationClassTask& task)
{
    const FlowFamilyInputs& inputs = task.inputs;
    FlowSiteLoss loss;
    for (AllocationSiteID id : task.members.sites) {
        const FlowSite& site = inputs.objects().at(id);
        const Value* operation = findAllocatingOperation(inputs, site);
        if (!operation || !passesNewTarget(inputs, site, *operation))
            continue;

        ValueID call = site.description.value;
        inputs.graph().forEachInstanceOf(site.function, [&](FlowInstanceID instance) {
            if (inputs.calls.hasUnknownCalleeAt(instance, call))
                keepFirstLoss(loss, { UnprovenCause::UnknownCallee, LimitID::None, inputs.graph().witnessOf(instance, call) });
        });
    }
    return loss;
}

static Witness witnessOfSite(const FlowFamilyInputs& inputs, const FlowSite& site)
{
    Witness witness = noWitness;
    ValueID value = site.description.value;
    if (!value || inputs.graph().isHost(site.function))
        return witness;
    witness.function = site.function.index();
    witness.value = value.index();
    witness.location = inputs.program().at(site.function).locationOf(value);
    return witness;
}

static FlowSiteLoss findOrdinaryObject(const AllocationClassTask& task)
{
    FlowSiteLoss loss;
    for (AllocationSiteID id : task.members.sites) {
        const FlowSite& site = task.inputs.objects().at(id);
        if (!allocatesOrdinaryObject(site.description) && !task.inputs.hierarchy.createsDictionaries(task.id))
            continue;
        UnprovenCause cause = site.description.origin == FlowSiteOrigin::BuiltinConstructor ? UnprovenCause::BuiltinBase : UnprovenCause::NonLayoutObject;
        keepFirstLoss(loss, { cause, LimitID::None, witnessOfSite(task.inputs, site) });
    }
    return loss;
}

static FlowSiteLoss findShapeLoss(const AllocationClassTask& task)
{
    FlowSiteLoss loss;
    keepFirstLoss(loss, findDisabledResult(task));
    keepFirstLoss(loss, findWorkLimitLoss(task));
    keepFirstLoss(loss, findOrdinaryObject(task));
    keepFirstLoss(loss, findBuiltinBase(task));
    keepFirstLoss(loss, findUnknownBase(task));
    keepFirstLoss(loss, findUnprovenDerivedAllocation(task));
    keepFirstLoss(loss, findUnknownConstructor(task));
    if (task.joined.mutations.contains(ObjectMutation::ReachUnknownJavaScript))
        keepFirstLoss(loss, { UnprovenCause::ReachesUnknownJavaScript, LimitID::None, findLowestWitness(task, ObjectMutation::ReachUnknownJavaScript) });
    if (task.joined.mayGetIndexedAccessor)
        keepFirstLoss(loss, { UnprovenCause::PropertyRedefined, LimitID::None, findWitnessOfIndexedAccessor(task) });
    return loss;
}

static FlowShapeResult computeShape(const AllocationClassTask& task)
{
    FlowShapeResult shape;
    if (!task.members.layout) {
        shape.loss.cause = UnprovenCause::NonLayoutObject;
        return shape;
    }
    shape.loss = task.inputs.program().at(task.members.layout).isEnvironment() ? findDisabledResult(task) : findShapeLoss(task);
    if (shape.loss.cause == UnprovenCause::Unknown)
        shape.state = FlowShapeState::KeepsShape;
    else if (task.inputs.hierarchy.hasOnlyOrdinaryObjects(task.id))
        shape.state = FlowShapeState::NeverHasShape;
    return shape;
}

static AllocationClass createRecord(const AllocationClassTask& task, const FlowShapeResult& shape)
{
    const FlowAllocationClassMembers& members = task.members;
    uint32_t subject = task.id.index();

    AllocationClass record;
    record.layout = members.layout;
    record.builtinClass = members.builtinClass;
    record.closureFunction = members.closureFunction;

    Vector<PropertyKeyID> addedKeys = collectAddedKeys(task);
    if (areMutationsEnabled(task)) {
        record.impossibleMutations = allObjectMutations - possibleMutationsOf(task, !addedKeys.isEmpty());
        record.impossibleMutations.set(ObjectMutation::LoseShape, shape.state == FlowShapeState::KeepsShape);
        record.isStoredUnderUnnamedKeys = task.joined.isStoredUnderUnnamedKeys;
        if (members.builtinClass)
            record.pristine = provenPristineAspects(task, record.impossibleMutations);
    }
    if (task.inputs.isEnabled(FlowResultKind::ObjectUses, subject) && !task.joined.uses.containsAny(keyListingUses) && !task.joined.mutations.containsAny(reachMutations))
        record.keys = KeyEnumeration::NeverEnumerated;
    if (task.inputs.isEnabled(FlowResultKind::OwnNames, subject)) {
        record.addedKeys = WTF::move(addedKeys);
        record.addedKeysAreComplete = !task.joined.mayGetUnlistedKey && !task.joined.events.contains(FlowObjectEvent::StoredUnderNumericName);
    }
    if (members.layout) {
        record.cause = shape.loss.cause;
        record.witness = shape.loss.witness;
    }
    return record;
}

FlowShapeResults computeShapeResults(const FlowFamilyInputs& inputs, std::span<const ClassFlow> classesByLayout, const FlowAppendedKeys& appendedKeys, const FlowBuiltinTables& builtinLocations)
{
    unsigned numberOfClasses = inputs.objects().numberOfAllocationClasses();
    FlowShapeResults results { Vector<FlowShapeResult>(numberOfClasses), Vector<AllocationClass>(numberOfClasses) };
    Vector<PropertyKeyID> keysOfUnknownObject = collectKeysOfUnknownObject(inputs.solution);
    FlowObjectSet objectsWithReplacedContents = collectObjectsWithReplacedContents(inputs);
    FlowObjectSet denseArrays = collectDenseArrays(inputs);
    forEachIndexInParallel(inputs.loops(), numberOfClasses, [&](unsigned index) {
        AllocationClassID id(index);
        AllocationClassTask task { inputs, classesByLayout, appendedKeys, builtinLocations, keysOfUnknownObject.span(), objectsWithReplacedContents, denseArrays, id, inputs.objects().at(id), joinFacts(inputs, id) };
        FlowShapeResult& shape = results.shapes[index];
        AllocationClass& record = results.allocationClasses[index];
        shape = computeShape(task);
        record = createRecord(task, shape);
        shape.pristine = record.pristine;
    });
    return results;
}

ShapePermanence shapePermanenceOf(const Program& program, const FlowResults&, LayoutID id)
{
    if (!id)
        return ShapePermanence::Unknown;
    // The language fixes the properties, the integrity level and the prototype of a namespace object.
    if (program.at(id).kind == LayoutKind::ModuleNamespace)
        return ShapePermanence::CannotBeLost;
    const LayoutFlow& flow = program.at(id).flow;
    if (flow.allocation == AllocationState::NeverAllocated)
        return ShapePermanence::CannotBeLost;
    if (!flow.allocationClasses.isEmpty() && flow.cause == UnprovenCause::Unknown)
        return ShapePermanence::CannotBeLost;
    return ShapePermanence::Unknown;
}

ShapePermanence shapePermanenceOf(const Program& program, const FlowResults& flow, const LayoutSet& set)
{
    if (!set.isComplete || set.members.isEmpty())
        return ShapePermanence::Unknown;
    bool cannotBeLost = std::ranges::all_of(set.members, [&](LayoutID member) {
        return shapePermanenceOf(program, flow, member) == ShapePermanence::CannotBeLost;
    });
    return cannotBeLost ? ShapePermanence::CannotBeLost : ShapePermanence::Unknown;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
