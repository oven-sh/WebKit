/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTFlowObjectState.h"
#include "AOTFlowOrder.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTFlowPropertyState.h"
#include "AOTFlowPrototypes.h"
#include "AOTGrouping.h"
#include "AOTSortedSet.h"
#include "AOTThreads.h"
#include <algorithm>
#include <span>
#include <tuple>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<FlowObjectEvent> unnamedKeyChanges {
    FlowObjectEvent::StoredByUnknownKey,
    FlowObjectEvent::DeletedByUnknownKey,
};

static constexpr OptionSet<FlowLocationEvent> storeEvents {
    FlowLocationEvent::StoredByOperation,
    FlowLocationEvent::StoredByManyOperations,
    FlowLocationEvent::WrittenByUnknownCode,
};

static constexpr OptionSet<FlowLocationEvent> mutableLocationEvents = storeEvents | OptionSet<FlowLocationEvent> {
    FlowLocationEvent::MissingAtAllocation,
    FlowLocationEvent::Deleted,
    FlowLocationEvent::Redefined,
};

static constexpr OptionSet<FlowLocationEvent> changesOfAssignedLocation {
    FlowLocationEvent::StoredByManyOperations,
    FlowLocationEvent::WrittenByUnknownCode,
    FlowLocationEvent::Deleted,
    FlowLocationEvent::Redefined,
};

static constexpr OptionSet<FlowLocationEvent> readsOtherThanCalls {
    FlowLocationEvent::ReadAsValue,
    FlowLocationEvent::PresenceObserved,
    FlowLocationEvent::ReadByUnknownCode,
};

static constexpr OptionSet<FlowObjectUse> callUses {
    FlowObjectUse::Called,
    FlowObjectUse::CalledRepeatedly,
};

namespace {

using HolderSet = SortedSet<FlowObjectID, 2>;

enum class AssignedMembers : bool { MayChange, AreFollowed };

template<typename Record>
struct RecordsByLayout {
    std::span<const Record> of(LayoutID layout) const LIFETIME_BOUND { return entriesOfGroup(firstRecordOfLayout, records, layout.index()); }

    Vector<uint32_t> firstRecordOfLayout;
    Vector<Record> records;
};

struct ClassHolders {
    const HolderSet& on(ClassSide side) const LIFETIME_BOUND
    {
        switch (side) {
        case ClassSide::Prototype:
            return prototypeObjects;
        case ClassSide::Static:
            return classObjects;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    HolderSet classObjects;
    HolderSet prototypeObjects;
    FlowObjectID replacedPrototypeObject;
    FlowNodeID parentGivenBeforeLookups;
};

struct PrototypeObjects {
    HashMap<FlowObjectID, LayoutID> classOf;
    FlowObjectSet replaced;
};

struct SideNames {
    bool mayHaveUnknownName { false };
};

struct ClassMemberList {
    SideNames& namesOn(ClassSide side) LIFETIME_BOUND
    {
        switch (side) {
        case ClassSide::Prototype:
            return prototypeNames;
        case ClassSide::Static:
            return staticNames;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Vector<ClassMember> members;
    SideNames prototypeNames;
    SideNames staticNames;
};

struct IndexedMember {
    uint32_t index { 0 };
    ClassMember member;
};

} // anonymous namespace

static MemberMutability fixedIf(bool isFixed)
{
    return isFixed ? MemberMutability::Fixed : MemberMutability::Unknown;
}

template<typename Record>
static RecordsByLayout<Record> groupByLayout(std::span<const Record> records, size_t layoutCount)
{
    RecordsByLayout<Record> grouped;
    groupByIndex(layoutCount, grouped.firstRecordOfLayout, grouped.records, [&](const auto& visit) {
        for (const Record& record : records)
            visit(record.layout.index(), record);
    });
    return grouped;
}

static bool runsAtMostOnce(const FlowFamilyInputs& inputs, FunctionID function)
{
    if (!function)
        return false;
    return inputs.graph().isHost(function) || inputs.executionsByFunction[function.index()] == ExecutionCount::AtMostOnce;
}

static ExecutionCount evaluationsAtOneSiteOf(const FlowFamilyInputs& inputs, FunctionID function)
{
    return runsAtMostOnce(inputs, function) ? ExecutionCount::AtMostOnce : ExecutionCount::Unknown;
}

static ExecutionCount countDefinitionEvaluations(const FlowFamilyInputs& inputs, std::span<const FlowClassEvaluation> evaluations)
{
    if (evaluations.empty())
        return ExecutionCount::AtMostOnce;

    const FlowGraph& graph = inputs.graph();
    FunctionID function = graph.at(evaluations.front().instance).function;
    ValueID origin = evaluations.front().origin;
    bool hasOneSite = std::ranges::all_of(evaluations, [&](const FlowClassEvaluation& evaluation) {
        return graph.at(evaluation.instance).function == function && evaluation.origin == origin && !evaluation.isInCycle;
    });
    return hasOneSite ? evaluationsAtOneSiteOf(inputs, function) : ExecutionCount::Unknown;
}

static ExecutionCount countClosureCreations(const FlowFamilyInputs& inputs, const HolderSet& closures)
{
    if (closures.isEmpty())
        return ExecutionCount::AtMostOnce;

    const FlowObjectTable& objects = inputs.objects();
    AllocationSiteID site = objects.at(closures[0]).site;
    bool hasOneSite = std::ranges::all_of(closures, [&](FlowObjectID closure) {
        return objects.at(closure).site == site && !inputs.factsOf(closure).events.contains(FlowObjectEvent::AllocatedRepeatedly);
    });
    return hasOneSite ? evaluationsAtOneSiteOf(inputs, objects.allocatingFunctionOf(closures[0])) : ExecutionCount::Unknown;
}

static ClassHolders collectHolders(const FlowFamilyInputs& inputs, const Layout& layout, std::span<const FlowClassEvaluation> evaluations)
{
    ClassHolders holders;
    if (!layout.has(LayoutFlags::IsConstructorFunction)) {
        for (const FlowClassEvaluation& evaluation : evaluations) {
            holders.classObjects.add(evaluation.classObject);
            holders.prototypeObjects.add(evaluation.prototypeObject);
        }
        return holders;
    }

    const FlowObjectTable& objects = inputs.objects();
    for (FlowObjectID closure : objects.closuresOf(layout.constructor)) {
        if (!inputs.factsOf(closure).events.contains(FlowObjectEvent::Allocated))
            continue;
        holders.classObjects.add(closure);
        if (FlowObjectID prototypeObject = objects.findPrototype(closure))
            holders.prototypeObjects.add(prototypeObject);
    }
    return holders;
}

static FlowOwnProperty ownPropertyOf(const FlowFamilyInputs& inputs, FlowObjectID holder, PropertyKeyID key)
{
    return findOwnProperty(FlowWalkInputs::of(inputs.solution), holderOf(inputs.objects(), holder), key);
}

static OptionSet<FlowLocationEvent> locationEventsOf(const FlowFamilyInputs& inputs, FlowObjectID holder, PropertyKeyID key)
{
    OptionSet<FlowLocationEvent> events = aliasedEventsOf(inputs.solution, holder, key);
    FlowOwnProperty property = ownPropertyOf(inputs, holder, key);
    for (FlowNodeID node : { property.value, property.getter, property.setter }) {
        if (node)
            events.add(inputs.records().recordOf(node).events);
    }
    return events;
}

static bool holdsOnlyClosures(const FlowFamilyInputs& inputs, FlowNodeID node)
{
    const FlowValue& value = inputs.graph().valueOf(node);
    if (value.primitives || value.hasUnknownObject() || value.objects.isEmpty())
        return false;
    bool holdsOtherObject = false;
    value.objects.forEach([&](FlowObjectID object) {
        holdsOtherObject |= inputs.objects().kindOf(object) != FlowObjectKind::Closure;
    });
    return !holdsOtherObject;
}

static bool isStoredByOneOperation(const FlowFamilyInputs& inputs, FlowNodeID location)
{
    return !inputs.records().recordOf(location).events.containsAny(changesOfAssignedLocation) && holdsOnlyClosures(inputs, location);
}

static bool isNeverFilled(const FlowFamilyInputs& inputs, FlowNodeID location)
{
    return !location || (inputs.graph().valueOf(location).isEmpty() && !inputs.records().recordOf(location).events.containsAny(mutableLocationEvents));
}

static bool areHalvesOfOneDefinition(const FlowFamilyInputs& inputs, const FlowOwnProperty& property)
{
    for (FlowNodeID half : { property.getter, property.setter }) {
        if (half && !isStoredByOneOperation(inputs, half))
            return false;
    }
    if (!property.getter || !property.setter)
        return true;
    const Witness& getterStore = inputs.records().recordOf(property.getter).firstStore;
    const Witness& setterStore = inputs.records().recordOf(property.setter).firstStore;
    return getterStore.function == setterStore.function && getterStore.value == setterStore.value;
}

// One operation stores a function under the name or defines an accessor there, on a holder that is created once, and has completed before anything looks the name up there.
static bool isAssignedBeforeEveryRead(const FlowFamilyInputs& inputs, FlowObjectID holder, PropertyKeyID key, const FlowOwnProperty& property)
{
    bool isAccessor = property.getter || property.setter;
    if (isAccessor ? !isNeverFilled(inputs, property.value) || !areHalvesOfOneDefinition(inputs, property) : !property.value || !isStoredByOneOperation(inputs, property.value))
        return false;
    return locationOrderOf(inputs.solution, holder, key).initialization == InitializationState::StoredBeforeEveryRead;
}

static bool isFixedLocation(const FlowFamilyInputs& inputs, FlowObjectID holder, PropertyKeyID key, AssignedMembers assignedMembers)
{
    const FlowObjectFacts& facts = inputs.factsOf(holder);
    if (facts.mutations.contains(ObjectMutation::ReachUnknownJavaScript) || facts.events.containsAny(unnamedKeyChanges))
        return false;
    if (aliasedEventsOf(inputs.solution, holder, key).containsAny(mutableLocationEvents))
        return false;

    FlowOwnProperty property = ownPropertyOf(inputs, holder, key);
    if (assignedMembers == AssignedMembers::AreFollowed && isAssignedBeforeEveryRead(inputs, holder, key, property))
        return true;
    for (FlowNodeID node : { property.value, property.getter, property.setter }) {
        if (!node)
            continue;
        OptionSet<FlowLocationEvent> events = inputs.records().recordOf(node).events;
        if (!events.contains(FlowLocationEvent::InitializedAtAllocation) || events.containsAny(mutableLocationEvents))
            return false;
    }
    return true;
}

static bool isEnteredOnce(const FlowFamilyInputs& inputs, FlowInstanceID instance)
{
    return std::ranges::all_of(inputs.calls.linksTo(inputs.graph().at(instance).function), [&](FlowCallLinkID id) {
        const FlowCallLink& link = inputs.records().at(id);
        return link.calleeInstance != instance || (link.kind == FlowCallLinkKind::Call && !link.isInCycle);
    });
}

// The first instance that allocates at the site of a built-in call takes the shared object of the site, which names no instance.
static FlowInstanceID allocatingInstanceOf(const FlowFamilyInputs& inputs, FlowObjectID object)
{
    const FlowObject& record = inputs.objects().at(object);
    if (record.instance || !record.site)
        return record.instance;

    FlowInstanceID allocatingInstance;
    inputs.graph().forEachInstanceOf(inputs.objects().at(record.site).function, [&](FlowInstanceID instance) {
        if (inputs.solution.objectState.solveObjects.get(std::pair { record.site, instance }) == object)
            allocatingInstance = instance;
    });
    return allocatingInstance;
}

// An object that is allocated in an instance for the calls of one site is created as often as that call runs, in any other instance as often as the function.
bool isCreatedOnce(const FlowFamilyInputs& inputs, FlowObjectID object)
{
    if (inputs.factsOf(object).events.contains(FlowObjectEvent::AllocatedRepeatedly))
        return false;
    FunctionID function = inputs.objects().allocatingFunctionOf(object);
    for (FlowInstanceID instance = allocatingInstanceOf(inputs, object); instance;) {
        const std::optional<FlowCallSiteKey>& callSite = inputs.graph().at(instance).callSite;
        if (!callSite)
            break;
        if (!isEnteredOnce(inputs, instance))
            return false;
        function = callSite->caller;
        instance = callSite->outer;
    }
    return runsAtMostOnce(inputs, function);
}

// The object that the prototype property of the function holds whenever anything reads it: one operation stores it there before the first read.
static FlowObjectID findAssignedPrototype(const FlowFamilyInputs& inputs, FlowObjectID closure, PropertyKeyID prototypeKey)
{
    const FlowObjectFacts& facts = inputs.factsOf(closure);
    if (facts.mutations.contains(ObjectMutation::ReachUnknownJavaScript) || facts.events.containsAny(unnamedKeyChanges))
        return { };
    if (aliasedEventsOf(inputs.solution, closure, prototypeKey).containsAny(mutableLocationEvents))
        return { };

    FlowOwnProperty property = ownPropertyOf(inputs, closure, prototypeKey);
    if (!property.value)
        return { };
    OptionSet<FlowLocationEvent> events = inputs.records().recordOf(property.value).events;
    if (!events.contains(FlowLocationEvent::StoredByOperation) || events.containsAny(changesOfAssignedLocation))
        return { };
    if (locationOrderOf(inputs.solution, closure, prototypeKey).initialization != InitializationState::StoredBeforeEveryRead)
        return { };

    const FlowValue& value = inputs.graph().valueOf(property.value);
    if (value.primitives || value.hasUnknownObject())
        return { };
    FlowObjectID original = inputs.objects().findPrototype(closure);
    FlowObjectID assigned;
    bool hasSeveral = false;
    value.objects.forEach([&](FlowObjectID object) {
        if (object == original)
            return;
        hasSeveral |= !!assigned;
        assigned = object;
    });
    if (!assigned || hasSeveral || !isCreatedOnce(inputs, assigned))
        return { };
    return assigned;
}

static void followAssignedPrototype(const FlowFamilyInputs& inputs, LayoutID id, ClassHolders& holders)
{
    if (!inputs.program().at(id).has(LayoutFlags::IsConstructorFunction) || holders.classObjects.size() != 1)
        return;
    if (!inputs.isEnabled(FlowResultKind::Initialization, id.index()) || !inputs.isEnabled(FlowResultKind::HolderMutations, id.index()))
        return;
    if (countClosureCreations(inputs, holders.classObjects) != ExecutionCount::AtMostOnce)
        return;

    FlowObjectID assigned = findAssignedPrototype(inputs, holders.classObjects[0], inputs.program().keyOf(BuiltinNames::prototype));
    if (!assigned)
        return;
    holders.replacedPrototypeObject = inputs.objects().findPrototype(holders.classObjects[0]);
    holders.prototypeObjects = { };
    holders.prototypeObjects.add(assigned);
}

// The node with the object that one call of a built-in makes the prototype of the prototype object the function is created with, before anybody holds that object or an heir of it.
static FlowNodeID findParentGivenBeforeLookups(const FlowFamilyInputs& inputs, FlowObjectID closure, FlowObjectID prototypeObject)
{
    constexpr OptionSet<ObjectMutation> excludedMutations = reachMutations | OptionSet<ObjectMutation> { ObjectMutation::ChangeIntegrity };
    if (inputs.factsOf(closure).mutations.containsAny(reachMutations) || inputs.factsOf(prototypeObject).mutations.containsAny(excludedMutations))
        return { };

    PropertyKeyID link = inputs.program().keyOf(BuiltinNames::__proto__);
    FlowOwnProperty ownProperty = ownPropertyOf(inputs, prototypeObject, link);
    if (ownProperty.value || ownProperty.getter || ownProperty.setter)
        return { };
    if (locationOrderOf(inputs.solution, prototypeObject, link).initialization != InitializationState::StoredBeforeEveryRead)
        return { };

    return FlowWalkInputs::of(inputs.solution).state.prototypesGivenByBuiltin.get(prototypeObject);
}

static void followParentOfPrototypeObject(const FlowFamilyInputs& inputs, LayoutID id, ClassHolders& holders)
{
    if (!inputs.program().at(id).has(LayoutFlags::IsConstructorFunction) || holders.classObjects.size() != 1 || holders.prototypeObjects.size() != 1 || holders.replacedPrototypeObject)
        return;
    if (!inputs.isEnabled(FlowResultKind::Initialization, id.index()) || !inputs.isEnabled(FlowResultKind::HolderMutations, id.index()))
        return;
    if (!inputs.factsOf(holders.prototypeObjects[0]).mutations.contains(ObjectMutation::ChangePrototype))
        return;
    if (countClosureCreations(inputs, holders.classObjects) != ExecutionCount::AtMostOnce)
        return;
    holders.parentGivenBeforeLookups = findParentGivenBeforeLookups(inputs, holders.classObjects[0], holders.prototypeObjects[0]);
}

enum class PrototypeParent : uint8_t { Unknown, ObjectPrototype, ClassPrototype };

struct PrototypeParentAnswer {
    PrototypeParent kind { PrototypeParent::Unknown };
    LayoutID classLayout;
};

static PrototypeParentAnswer findPrototypeParent(const FlowFamilyInputs& inputs, FlowObjectID prototypeObject, const PrototypeObjects& prototypeObjects)
{
    FlowPrototypeSet parents = prototypesOf(FlowWalkInputs::of(inputs.solution), holderOf(inputs.objects(), prototypeObject), { }, &prototypeObjects.replaced);
    if (!parents.isNeverNull || !parents.isNeverUnknown || parents.holders.size() != 1)
        return { };

    const FlowHolder& parent = parents.holders[0];
    const BuiltinTable& builtins = inputs.program().builtins();
    if (parent.builtin) {
        bool isObjectPrototype = builtins.canonicalID(parent.builtin) == builtins.canonicalID(builtins.at(BuiltinClasses::Object).prototype);
        return { isObjectPrototype ? PrototypeParent::ObjectPrototype : PrototypeParent::Unknown, { } };
    }
    LayoutID classLayout = prototypeObjects.classOf.get(parent.object);
    return { classLayout ? PrototypeParent::ClassPrototype : PrototypeParent::Unknown, classLayout };
}

static LayoutID classOfGivenParent(const FlowFamilyInputs& inputs, FlowNodeID given, const PrototypeObjects& prototypeObjects)
{
    if (!given)
        return { };
    const FlowValue& value = inputs.graph().valueOf(given);
    if (value.primitives || value.hasUnknownObject())
        return { };
    FlowObjectID parent;
    bool hasSeveral = false;
    value.objects.forEachNotIn(prototypeObjects.replaced, [&](FlowObjectID object) {
        hasSeveral |= !!parent;
        parent = object;
    });
    return parent && !hasSeveral ? prototypeObjects.classOf.get(parent) : LayoutID();
}

// Where nothing is proven about a constructor function, its class is expected to be what seen code makes it: the instances inherit from the one object that seen code stores as the prototype,
// or from the object the function is created with, and that object from the prototype object of one other class. The link between the two is tested for.
static LayoutID predictClassOfParent(const FlowFamilyInputs& inputs, const ClassHolders& holders, const PrototypeObjects& prototypeObjects)
{
    if (holders.classObjects.size() != 1)
        return { };
    FlowObjectID closure = holders.classObjects[0];
    FlowObjectID expected = inputs.objects().findPrototype(closure);
    FlowObjectID original = expected;
    bool hasSeveral = false;
    if (FlowNodeID stored = ownPropertyOf(inputs, closure, inputs.program().keyOf(BuiltinNames::prototype)).value) {
        inputs.graph().valueOf(stored).objects.forEach([&](FlowObjectID object) {
            if (object == original || object == unknownFlowObject)
                return;
            hasSeveral |= expected != original;
            expected = object;
        });
    }
    if (!expected || hasSeveral)
        return { };

    LayoutID classOfParent;
    FlowPrototypeSet parents = prototypesOf(FlowWalkInputs::of(inputs.solution), holderOf(inputs.objects(), expected), { }, &prototypeObjects.replaced);
    for (const FlowHolder& parent : parents.holders) {
        if (parent.builtin || !parent.object || parent.object == unknownFlowObject)
            continue;
        LayoutID classLayout = prototypeObjects.classOf.get(parent.object);
        if (!classLayout || (classOfParent && classOfParent != classLayout))
            return { };
        classOfParent = classLayout;
    }
    return classOfParent;
}

static bool isFixedOnHolders(const FlowFamilyInputs& inputs, const HolderSet& holders, PropertyKeyID key, AssignedMembers assignedMembers = AssignedMembers::MayChange)
{
    return std::ranges::all_of(holders, [&](FlowObjectID holder) {
        return isFixedLocation(inputs, holder, key, assignedMembers);
    });
}

static bool mayChangePrototype(const FlowFamilyInputs& inputs, const HolderSet& holders)
{
    return std::ranges::any_of(holders, [&](FlowObjectID holder) {
        return inputs.factsOf(holder).mutations.contains(ObjectMutation::ChangePrototype);
    });
}

static bool mayGetUnlistedKey(const FlowFamilyInputs& inputs, const HolderSet& holders)
{
    return std::ranges::any_of(holders, [&](FlowObjectID holder) {
        const FlowObjectFacts& facts = inputs.factsOf(holder);
        return facts.mayGetUnlistedKey || facts.mayGetIndexKey || facts.mayGetKeysOfUnknownObject;
    });
}

static bool hasUnkeyedStaticField(const Layout& layout)
{
    return std::ranges::any_of(layout.staticFields, [](const StaticField& field) {
        return !field.key;
    });
}

static bool definesMember(std::span<const FlowMemberDefinition> definitions, ClassSide side, PropertyKeyID key)
{
    return std::ranges::any_of(definitions, [&](const FlowMemberDefinition& definition) {
        return definition.side == side && definition.key == key;
    });
}

static void addDefinedMembers(std::span<const FlowMemberDefinition> definitions, ClassMemberList& list)
{
    for (const FlowMemberDefinition& definition : definitions) {
        if (!definition.key) {
            list.namesOn(definition.side).mayHaveUnknownName = true;
            continue;
        }
        list.members.append(ClassMember { .key = definition.key, .side = definition.side, .kind = definition.kind, .function = definition.function });
    }
}

// A name that may hold something else than a function is listed without one as well: a lookup of that name learns nothing, and a lookup of any other name passes the holder.
static void addFunctionsIn(const FlowFamilyInputs& inputs, FlowNodeID node, const ClassMember& member, ClassMemberList& list)
{
    const FlowValue& value = inputs.graph().valueOf(node);
    bool mayHoldOtherValue = value.primitives || value.hasUnknownObject();
    value.objects.forEach([&](FlowObjectID object) {
        const FlowObject& record = inputs.objects().at(object);
        if (record.kind != FlowObjectKind::Closure) {
            mayHoldOtherValue = true;
            return;
        }
        ClassMember listed = member;
        listed.function = record.function;
        list.members.append(listed);
    });
    if (mayHoldOtherValue)
        list.members.append(member);
}

// An object with a layout that was stored into the prototype property has members in its fields.
static void addMembersInFields(const FlowFamilyInputs& inputs, FlowObjectID holder, ClassSide side, PropertyKeyID creationKey, ClassMemberList& list)
{
    const FlowObject& record = inputs.objects().at(holder);
    if (record.kind != FlowObjectKind::Allocation || !record.layout)
        return;
    const Vector<Field>& fields = inputs.program().at(record.layout).fields;
    for (uint32_t index = 0; index < record.numberOfDeclaredParts && index < fields.size(); ++index) {
        if (fields[index].key && fields[index].key != creationKey)
            addFunctionsIn(inputs, inputs.graph().field(holder, index), ClassMember { .key = fields[index].key, .side = side, .kind = MethodKind::Method }, list);
    }
}

static void addAssignedMembers(const FlowFamilyInputs& inputs, const HolderSet& holders, ClassSide side, PropertyKeyID creationKey, ClassMemberList& list)
{
    for (FlowObjectID holder : holders) {
        addMembersInFields(inputs, holder, side, creationKey, list);
        for (const FlowProperty& property : inputs.graph().propertiesOf(holder)) {
            if (property.key == creationKey)
                continue;
            if (property.value) {
                OptionSet<FlowLocationEvent> events = inputs.records().recordOf(property.value).events | aliasedEventsOf(inputs.solution, holder, property.key);
                if (events.containsAny(storeEvents))
                    addFunctionsIn(inputs, property.value, ClassMember { .key = property.key, .side = side, .kind = MethodKind::Method }, list);
            }
            if (property.getter)
                addFunctionsIn(inputs, property.getter, ClassMember { .key = property.key, .side = side, .kind = MethodKind::Getter }, list);
            if (property.setter)
                addFunctionsIn(inputs, property.setter, ClassMember { .key = property.key, .side = side, .kind = MethodKind::Setter }, list);
        }
    }
}

static auto memberOrderOf(const ClassMember& member)
{
    return std::tuple { member.side, member.key, member.kind, member.function };
}

static bool hasOneDefinition(std::span<const ClassMember> membersUnderOneKey)
{
    bool hasMethod = membersUnderOneKey.front().kind == MethodKind::Method;
    bool hasAccessor = membersUnderOneKey.back().kind != MethodKind::Method;
    if (hasMethod && hasAccessor)
        return false;
    return std::ranges::adjacent_find(membersUnderOneKey, { }, &ClassMember::kind) == membersUnderOneKey.end();
}

static bool hasOnlyCalledClosures(const FlowFamilyInputs& inputs, FunctionID function)
{
    return std::ranges::all_of(inputs.objects().closuresOf(function), [&](FlowObjectID closure) {
        const FlowObjectFacts& facts = inputs.factsOf(closure);
        return facts.uses.containsOnly(callUses) && facts.mutations.isEmpty();
    });
}

static void decideMemberGroup(const FlowFamilyInputs& inputs, LayoutID layout, const HolderSet& holders, bool mayHaveUnknownName, AssignedMembers assignedMembers, std::span<ClassMember> members)
{
    PropertyKeyID key = members.front().key;
    bool holdsOnlyFunctions = std::ranges::all_of(members, [](const ClassMember& member) {
        return !!member.function;
    });
    bool isFixed = inputs.isEnabled(FlowResultKind::HolderMutations, layout.index())
        && !mayHaveUnknownName
        && holdsOnlyFunctions
        && hasOneDefinition(members)
        && isFixedOnHolders(inputs, holders, key, assignedMembers);

    bool isOnlyReadAsCallee = inputs.isEnabled(FlowResultKind::ObjectUses, layout.index())
        && std::ranges::none_of(holders, [&](FlowObjectID holder) {
            return locationEventsOf(inputs, holder, key).containsAny(readsOtherThanCalls);
        });

    FlowWalkInputs walkInputs = FlowWalkInputs::of(inputs.solution);
    bool isWritableData = key
        && inputs.isEnabled(FlowResultKind::HolderMutations, layout.index())
        && !mayHaveUnknownName
        && std::ranges::none_of(holders, [&](FlowObjectID holder) {
            return stopsStoresFromBelow(walkInputs, holderOf(inputs.objects(), holder), key);
        });

    for (ClassMember& member : members) {
        member.mutability = fixedIf(isFixed);
        member.isWritableData = isWritableData;
        if (isOnlyReadAsCallee && member.function && hasOnlyCalledClosures(inputs, member.function))
            member.closureUse = ClosureUse::CalledOnly;
    }
}

static void decideMembers(const FlowFamilyInputs& inputs, LayoutID layout, const ClassHolders& holders, AssignedMembers assignedMembers, ClassMemberList& list)
{
    std::ranges::sort(list.members, { }, memberOrderOf);
    removeRepeatedElements(list.members);

    std::span<ClassMember> members = list.members.mutableSpan();
    size_t first = 0;
    while (first < members.size()) {
        ClassSide side = members[first].side;
        size_t end = first + 1;
        while (end < members.size() && members[end].side == side && members[end].key == members[first].key)
            ++end;
        decideMemberGroup(inputs, layout, holders.on(side), list.namesOn(side).mayHaveUnknownName, assignedMembers, members.subspan(first, end - first));
        first = end;
    }
}

static auto definitionOrderOf(const IndexedMember& entry)
{
    return std::tuple { entry.index, memberOrderOf(entry.member) };
}

static Vector<IndexedMember> collectIndexedMembers(std::span<const FlowMemberDefinition> definitions)
{
    Vector<IndexedMember> entries;
    for (const FlowMemberDefinition& definition : definitions)
        entries.append({ definition.index, ClassMember { .key = definition.constantKey, .side = definition.side, .kind = definition.kind, .function = definition.function } });
    std::ranges::sort(entries, { }, definitionOrderOf);
    removeRepeatedElements(entries, [](const IndexedMember& a, const IndexedMember& b) {
        return definitionOrderOf(a) == definitionOrderOf(b);
    });
    return entries;
}

static Vector<ClassMember> listMembersInDefinitionOrder(std::span<const FlowMemberDefinition> definitions, std::span<const ClassMember> namedMembers)
{
    Vector<IndexedMember> entries = collectIndexedMembers(definitions);
    Vector<ClassMember> members;
    members.reserveInitialCapacity(entries.size());
    for (size_t position = 0; position < entries.size(); ++position) {
        ClassMember member = entries[position].member;
        // Copies of a definition that disagree share an index. Only the copy that runs knows what it defines.
        bool sharesIndex = (position && entries[position - 1].index == entries[position].index)
            || (position + 1 < entries.size() && entries[position + 1].index == entries[position].index);
        if (sharesIndex)
            member.key = { };
        if (member.key) {
            auto named = std::ranges::lower_bound(namedMembers, memberOrderOf(member), { }, memberOrderOf);
            if (named != namedMembers.end() && memberOrderOf(*named) == memberOrderOf(member))
                member = *named;
        }
        members.append(member);
    }
    return members;
}

// No operation defines the members of a constructor function. A name of its prototype object under which seen code stores one function stands for a definition:
// where the member is not fixed, a test compares the holder with it.
static void appendMembersAssignedOnce(std::span<const ClassMember> namedMembers, Vector<ClassMember>& members)
{
    size_t first = 0;
    while (first < namedMembers.size()) {
        size_t end = first + 1;
        while (end < namedMembers.size() && namedMembers[end].side == namedMembers[first].side && namedMembers[end].key == namedMembers[first].key)
            ++end;
        std::span<const ClassMember> group = namedMembers.subspan(first, end - first);
        first = end;

        bool hasOnlyValues = std::ranges::all_of(group, [](const ClassMember& member) {
            return member.kind == MethodKind::Method;
        });
        auto holdsFunction = [](const ClassMember& member) {
            return !!member.function;
        };
        if (group.front().side == ClassSide::Prototype && hasOnlyValues && std::ranges::count_if(group, holdsFunction) == 1)
            members.append(*std::ranges::find_if(group, holdsFunction));
    }
}

static bool listsEveryName(const FlowFamilyInputs& inputs, const HolderSet& holders, const SideNames& names)
{
    return !names.mayHaveUnknownName && !mayGetUnlistedKey(inputs, holders);
}

static ClassFlow computeClassFlow(const FlowFamilyInputs& inputs, LayoutID id, const ClassHolders& holders, const PrototypeObjects& prototypeObjects, std::span<const FlowClassEvaluation> evaluations, std::span<const FlowMemberDefinition> definitions)
{
    const Program& program = inputs.program();
    const Layout& layout = program.at(id);
    PropertyKeyID constructorKey = program.keyOf(BuiltinNames::constructor);
    PropertyKeyID prototypeKey = program.keyOf(BuiltinNames::prototype);
    bool isConstructorFunction = layout.has(LayoutFlags::IsConstructorFunction);

    // A constructor function that is created without the property has no prototype object of its own for the results below to describe.
    bool hasFixedPrototypeProperty = !isConstructorFunction
        || (hasPrototypeProperty(program, program.at(layout.constructor)) && isFixedOnHolders(inputs, holders.classObjects, prototypeKey));
    bool hasAssignedPrototype = !!holders.replacedPrototypeObject;
    bool hasOnePrototypeObject = hasFixedPrototypeProperty || hasAssignedPrototype;

    ClassFlow flow;
    if (inputs.isEnabled(FlowResultKind::ClassEvaluations, id.index()))
        flow.evaluations = isConstructorFunction ? countClosureCreations(inputs, holders.classObjects) : countDefinitionEvaluations(inputs, evaluations);

    if (inputs.isEnabled(FlowResultKind::HolderMutations, id.index())) {
        LayoutID givenBase = classOfGivenParent(inputs, holders.parentGivenBeforeLookups, prototypeObjects);
        flow.prototypeLink = fixedIf(hasOnePrototypeObject && (givenBase || !mayChangePrototype(inputs, holders.prototypeObjects)));
        flow.staticLink = fixedIf(!mayChangePrototype(inputs, holders.classObjects));
        flow.constructorProperty = fixedIf(hasFixedPrototypeProperty
            && !definesMember(definitions, ClassSide::Prototype, constructorKey)
            && isFixedOnHolders(inputs, holders.prototypeObjects, constructorKey));
        flow.prototypeProperty = fixedIf(hasFixedPrototypeProperty);
        flow.hasAssignedPrototype = hasAssignedPrototype;
        if (hasAssignedPrototype)
            flow.prototypeBase = findPrototypeParent(inputs, holders.prototypeObjects[0], prototypeObjects).classLayout;
        else if (hasFixedPrototypeProperty)
            flow.prototypeBase = givenBase;
        if (isConstructorFunction && flow.prototypeLink != MemberMutability::Fixed && inputs.isEnabled(FlowResultKind::Predictions, id.index())) {
            LayoutID predicted = predictClassOfParent(inputs, holders, prototypeObjects);
            flow.prototypeBase = predicted != id ? predicted : LayoutID();
        }
    }
    // Where the parent of the prototype object of a constructor function is not proven, the class is expected to be what the program's types say, and the link is tested for.
    if (isConstructorFunction && flow.prototypeLink != MemberMutability::Fixed && layout.predictedBase)
        flow.prototypeBase = layout.predictedBase;

    ClassMemberList list;
    list.prototypeNames.mayHaveUnknownName = !hasOnePrototypeObject;
    list.staticNames.mayHaveUnknownName = hasUnkeyedStaticField(layout);
    addDefinedMembers(definitions, list);
    addAssignedMembers(inputs, holders.prototypeObjects, ClassSide::Prototype, constructorKey, list);
    addAssignedMembers(inputs, holders.classObjects, ClassSide::Static, prototypeKey, list);
    bool followsAssignedMembers = flow.evaluations == ExecutionCount::AtMostOnce && inputs.isEnabled(FlowResultKind::Initialization, id.index());
    decideMembers(inputs, id, holders, followsAssignedMembers ? AssignedMembers::AreFollowed : AssignedMembers::MayChange, list);

    if (inputs.isEnabled(FlowResultKind::OwnNames, id.index())) {
        flow.prototypeNamesAreComplete = listsEveryName(inputs, holders.prototypeObjects, list.prototypeNames);
        flow.staticNamesAreComplete = listsEveryName(inputs, holders.classObjects, list.staticNames);
    }
    flow.members = listMembersInDefinitionOrder(definitions, list.members.span());
    if (isConstructorFunction)
        appendMembersAssignedOnce(list.members.span(), flow.members);
    flow.namedMembers = WTF::move(list.members);
    return flow;
}

Vector<ClassFlow> computeClassResults(const FlowFamilyInputs& inputs)
{
    const Program& program = inputs.program();
    unsigned layoutCount = static_cast<unsigned>(program.layouts.size());
    RecordsByLayout<FlowClassEvaluation> evaluations = groupByLayout(inputs.records().classEvaluations(), layoutCount);
    RecordsByLayout<FlowMemberDefinition> definitions = groupByLayout(inputs.records().memberDefinitions(), layoutCount);

    auto isClass = [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        return layout.kind == LayoutKind::Instance && layout.constructor;
    };

    Vector<ClassHolders> holders(layoutCount);
    forEachIndexInParallel(inputs.loops(), layoutCount, [&](unsigned index) {
        if (!isClass(index))
            return;
        holders[index] = collectHolders(inputs, program.layouts[index], evaluations.of(LayoutID(index)));
        followAssignedPrototype(inputs, LayoutID(index), holders[index]);
        followParentOfPrototypeObject(inputs, LayoutID(index), holders[index]);
    });

    auto stopFollowingAssignedPrototype = [](ClassHolders& own) {
        own.prototypeObjects = { };
        own.prototypeObjects.add(std::exchange(own.replacedPrototypeObject, FlowObjectID()));
    };

    // The passes tell the instances of a class by their layout, so a prototype object stands for one class: `Alias.prototype = Class.prototype` makes an instance of either an instance of both.
    HashMap<FlowObjectID, unsigned> numberOfClassesWithPrototypeObject;
    for (const ClassHolders& own : holders) {
        if (own.prototypeObjects.size() == 1)
            ++numberOfClassesWithPrototypeObject.add(own.prototypeObjects[0], 0).iterator->value;
    }
    for (ClassHolders& own : holders) {
        if (own.replacedPrototypeObject && numberOfClassesWithPrototypeObject.get(own.prototypeObjects[0]) > 1)
            stopFollowingAssignedPrototype(own);
    }

    PrototypeObjects prototypeObjects;
    for (unsigned index = 0; index < layoutCount; ++index) {
        if (holders[index].prototypeObjects.size() == 1)
            prototypeObjects.classOf.add(holders[index].prototypeObjects[0], LayoutID(index));
        if (holders[index].replacedPrototypeObject)
            prototypeObjects.replaced.add(holders[index].replacedPrototypeObject);
    }
    // A lookup that leaves an assigned prototype object goes on in an object the passes know, or the class is as if its prototype property could hold anything.
    for (unsigned index = 0; index < layoutCount; ++index) {
        ClassHolders& own = holders[index];
        if (!own.replacedPrototypeObject || findPrototypeParent(inputs, own.prototypeObjects[0], prototypeObjects).kind != PrototypeParent::Unknown)
            continue;
        stopFollowingAssignedPrototype(own);
    }

    Vector<ClassFlow> classes(layoutCount);
    forEachIndexInParallel(inputs.loops(), layoutCount, [&](unsigned index) {
        LayoutID id(index);
        if (isClass(index))
            classes[index] = computeClassFlow(inputs, id, holders[index], prototypeObjects, evaluations.of(id), definitions.of(id));
    });
    return classes;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
