/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include <algorithm>
#include <compare>
#include <functional>
#include <span>
#include <tuple>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class AccessorListing : uint8_t { Unknown, Complete };

struct LayoutSetMember {
    LayoutID set;
    LayoutID member;

    auto operator<=>(const LayoutSetMember&) const = default;
};

struct InitialFieldList {
    const Vector<uint32_t>* fields { nullptr };
    AllocationClassID allocationClass;
};

struct CountedFieldOrder {
    const Vector<uint32_t>* fields { nullptr };
    size_t numberOfSites { 0 };
};

} // anonymous namespace

static constexpr OptionSet<FlowLocationEvent> storeEvents {
    FlowLocationEvent::WrittenByUnknownCode,
    FlowLocationEvent::StoredByOperation,
    FlowLocationEvent::StoredByManyOperations,
};

static FlowObjectFacts joinObjectFacts(const FlowFamilyInputs& inputs, LayoutID id)
{
    FlowObjectFacts joined;
    for (FlowObjectID object : inputs.hierarchy.objectsOf(id)) {
        const FlowObjectFacts& facts = inputs.factsOf(object);
        joined.mutations.add(facts.mutations);
        joined.events.add(facts.events);
        joined.uses.add(facts.uses);
        joined.mayGetUnlistedKey |= facts.mayGetUnlistedKey;
        joined.mayGetIndexKey |= facts.mayGetIndexKey;
        joined.mayGetKeysOfUnknownObject |= facts.mayGetKeysOfUnknownObject;
        joined.mayGetIndexedAccessor |= facts.mayGetIndexedAccessor;
    }
    return joined;
}

static bool hasAllocationClasses(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
        return true;
    case LayoutKind::Closure:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The operation stays in the program and placement gives it an allocation of the layout it names. The constructor of a class that only subclasses
// construct is such an operation, and the run time falls back to the named layout where a link of the chain of bases is not proven.
static bool isNamedByReachedOperation(const FlowFamilyInputs& inputs, LayoutID id)
{
    // A version that no call enters keeps its operations, and they name the declared layout.
    bool hasDerivedLayouts = std::ranges::any_of(inputs.hierarchy.layoutsBelow(id), [&](LayoutID below) {
        return inputs.program().at(below).isDerived();
    });
    if (hasDerivedLayouts)
        return true;

    const FlowObjectTable& objects = inputs.objects();
    return std::ranges::any_of(objects.allocationClassesOf(id), [&](AllocationClassID allocationClass) {
        if (inputs.hierarchy.hasOnlyOrdinaryObjects(allocationClass))
            return false;
        return std::ranges::any_of(objects.at(allocationClass).sites, [&](AllocationSiteID site) {
            const FlowSite& record = objects.at(site);
            return record.description.origin == FlowSiteOrigin::Operation && record.function && inputs.graph().isReached(record.function);
        });
    });
}

static void computeLayoutUses(const FlowFamilyInputs& inputs, LayoutID id, const FlowObjectFacts& facts, LayoutFlow& flow)
{
    // A closure or a namespace object has its layout without an allocation class, so no event says whether one is created.
    bool isNeverAllocated = hasAllocationClasses(inputs.program().at(id).kind) && !facts.events.contains(FlowObjectEvent::Allocated) && !isNamedByReachedOperation(inputs, id);
    if (isNeverAllocated && inputs.isEnabled(FlowResultKind::ProvenLayoutSets, id.index()))
        flow.allocation = AllocationState::NeverAllocated;

    if (!inputs.isEnabled(FlowResultKind::ObjectUses, id.index()))
        return;
    if (facts.uses.contains(FlowObjectUse::Serialized))
        flow.observedUses.add(ObservedUse::Serialized);
    if (facts.uses.contains(FlowObjectUse::ReachesTypedParse))
        flow.observedUses.add(ObservedUse::ReachesTypedParse);
}

static AccessorListing addAccessorFunctions(const FlowFamilyInputs& inputs, FlowNodeID node, const ClassMember& accessor, Vector<ClassMember>& accessors)
{
    if (!node)
        return AccessorListing::Complete;

    AccessorListing listing = AccessorListing::Complete;
    inputs.graph().valueOf(node).objects.forEach([&](FlowObjectID object) {
        const FlowObject& record = inputs.objects().at(object);
        if (record.kind != FlowObjectKind::Closure) {
            listing = AccessorListing::Unknown;
            return;
        }
        ClassMember listed = accessor;
        listed.function = record.function;
        accessors.append(listed);
    });
    return listing;
}

// An accessor under a key nobody can name makes its object reach unknown JavaScript or counts as an indexed accessor.
static void computeOwnAccessors(const FlowFamilyInputs& inputs, LayoutID id, const FlowObjectFacts& facts, LayoutFlow& flow)
{
    if (!hasAllocationClasses(inputs.program().at(id).kind) || !inputs.isEnabled(FlowResultKind::OwnNames, id.index()))
        return;

    bool isComplete = !facts.mutations.contains(ObjectMutation::ReachUnknownJavaScript) && !facts.mayGetIndexedAccessor;
    auto addAccessorsOf = [&](FlowObjectID object) {
        for (const FlowProperty& property : inputs.graph().propertiesOf(object)) {
            AccessorListing getters = addAccessorFunctions(inputs, property.getter, ClassMember { .key = property.key, .kind = MethodKind::Getter }, flow.ownAccessors);
            AccessorListing setters = addAccessorFunctions(inputs, property.setter, ClassMember { .key = property.key, .kind = MethodKind::Setter }, flow.ownAccessors);
            isComplete = isComplete && getters == AccessorListing::Complete && setters == AccessorListing::Complete;
        }
    };
    for (FlowObjectID object : inputs.hierarchy.objectsOf(id))
        addAccessorsOf(object);
    if (facts.mayGetKeysOfUnknownObject)
        addAccessorsOf(unknownFlowObject);
    std::ranges::sort(flow.ownAccessors, { }, [](const ClassMember& accessor) {
        return std::tuple { accessor.key, accessor.kind, accessor.function };
    });
    removeRepeatedElements(flow.ownAccessors);
    flow.ownAccessorsAreComplete = isComplete;
}

static Vector<InitialFieldList> collectInitialFieldLists(const FlowFamilyInputs& inputs, LayoutID id)
{
    // An allocation lists fields of the declared layout, whichever layout of the family it names.
    Vector<InitialFieldList> lists;
    if (inputs.program().at(id).isDerived())
        return lists;
    auto collectFrom = [&](LayoutID layout) {
        for (FlowObjectID object : inputs.hierarchy.objectsOf(layout)) {
            if (inputs.factsOf(object).events.contains(FlowObjectEvent::AllocatedByConstructor))
                continue;
            for (const Vector<uint32_t>& fields : inputs.records().initialFieldLists(object))
                lists.append({ &fields, inputs.objects().at(object).allocationClass });
        }
    };
    collectFrom(id);
    for (LayoutID derived : inputs.objects().derivedLayoutsOf(id))
        collectFrom(derived);

    std::ranges::sort(lists, [](const InitialFieldList& a, const InitialFieldList& b) {
        std::strong_ordering order = std::lexicographical_compare_three_way(a.fields->begin(), a.fields->end(), b.fields->begin(), b.fields->end());
        return order != 0 ? order < 0 : a.allocationClass < b.allocationClass;
    });
    return lists;
}

static Vector<FieldOrder> computeInitialFieldOrders(const FlowFamilyInputs& inputs, LayoutID id)
{
    Vector<InitialFieldList> lists = collectInitialFieldLists(inputs, id);

    Vector<CountedFieldOrder> orders;
    for (size_t index = 0; index < lists.size(); ++index) {
        const InitialFieldList& list = lists[index];
        bool isNewOrder = !index || *lists[index - 1].fields != *list.fields;
        if (isNewOrder)
            orders.append({ list.fields, 0 });
        if (isNewOrder || lists[index - 1].allocationClass != list.allocationClass)
            orders.last().numberOfSites += inputs.objects().at(list.allocationClass).sites.size();
    }
    std::ranges::stable_sort(orders, std::ranges::greater { }, &CountedFieldOrder::numberOfSites);

    return orders.map([](const CountedFieldOrder& order) {
        return FieldOrder { *order.fields };
    });
}

static FieldCreation fieldCreationOfRecord(const FlowFamilyInputs& inputs, LayoutID id, const FlowObjectFacts& facts)
{
    if (facts.mutations.contains(ObjectMutation::AddProperty))
        return FieldCreation::Unknown;

    const Layout& layout = inputs.program().at(id);
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        const Field& field = layout.fields[index];
        if (field.flags.contains(FieldFlags::IsInherited))
            continue;
        OptionSet<FlowLocationEvent> events = collectLocationFacts(inputs, FlowFieldSubject { id, index, field.key }).events;
        if (events.contains(FlowLocationEvent::MissingAtAllocation) && events.containsAny(storeEvents))
            return FieldCreation::Unknown;
    }
    return FieldCreation::OnlyAtConstruction;
}

static FieldCreation ownFieldCreationOf(const FlowFamilyInputs& inputs, LayoutID id, const FlowObjectFacts& facts, const FlowConstructionResult& construction)
{
    switch (inputs.program().at(id).kind) {
    case LayoutKind::Record:
        return fieldCreationOfRecord(inputs, id, facts);
    case LayoutKind::Instance:
        // The construction speaks for the constructors and their helpers alone, and a method may create a field that they left out.
        if (construction.fieldCreation != FieldCreation::OnlyAtConstruction)
            return construction.fieldCreation;
        return fieldCreationOfRecord(inputs, id, facts);
    case LayoutKind::Container:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return construction.fieldCreation;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void computeOwnResults(const FlowFamilyInputs& inputs, std::span<const FlowShapeResult> shapesByAllocationClass, LayoutID id, LayoutFlow& flow)
{
    FlowObjectFacts facts = joinObjectFacts(inputs, id);
    computeLayoutUses(inputs, id, facts, flow);
    computeOwnAccessors(inputs, id, facts, flow);

    const FlowBaseResult& base = inputs.hierarchy.baseOf(id);
    flow.baseState = base.state;
    flow.base = base.base;
    flow.baseClass = base.baseClass;
    for (AllocationClassID allocationClass : inputs.objects().allocationClassesOf(id)) {
        if (!inputs.hierarchy.hasOnlyOrdinaryObjects(allocationClass))
            flow.allocationClasses.append(allocationClass);
    }

    FlowConstructionResult construction = constructionOf(inputs.solution, id);
    if (inputs.isEnabled(FlowResultKind::ConstructorFieldOrders, id.index())) {
        flow.fieldCreation = ownFieldCreationOf(inputs, id, facts, construction);
        // constructionOf() has orders only when a constructor allocates every object of the layout, so one of the two lists is empty.
        flow.constructorOrders = WTF::move(construction.constructorOrders);
        flow.constructorOrders.appendVector(computeInitialFieldOrders(inputs, id));
    }
    if (inputs.isEnabled(FlowResultKind::ThisLeaks, id.index()))
        flow.thisLeak = construction.thisLeak;

    FlowSiteLoss loss;
    for (AllocationClassID allocationClass : flow.allocationClasses)
        keepFirstLoss(loss, shapesByAllocationClass[allocationClass.index()].loss);
    flow.cause = loss.cause;
    flow.witness = loss.witness;
}

static LayoutID provenBaseOf(const FlowHierarchy& hierarchy, LayoutID id)
{
    const FlowBaseResult& base = hierarchy.baseOf(id);
    return base.state == BaseState::Layout ? base.base : LayoutID();
}

static FieldCreation fieldCreationOfChain(const FlowHierarchy& hierarchy, std::span<const FieldCreation> ownFieldCreation, LayoutID id)
{
    if (!hierarchy.hasProvenChain(id))
        return FieldCreation::Unknown;
    for (LayoutID current = id; current; current = provenBaseOf(hierarchy, current)) {
        if (ownFieldCreation[current.index()] != FieldCreation::OnlyAtConstruction)
            return FieldCreation::Unknown;
    }
    return FieldCreation::OnlyAtConstruction;
}

static bool canBeMember(const Program& program, std::span<const LayoutFlow> results, LayoutID id)
{
    return results[id.index()].allocation != AllocationState::NeverAllocated && !program.at(id).isEnvironment();
}

static Vector<LayoutSetMember> collectSetMembers(const FlowFamilyInputs& inputs, std::span<const LayoutFlow> results)
{
    Vector<LayoutSetMember> setMembers = collectInParallel<LayoutSetMember>(inputs.loops(), static_cast<unsigned>(results.size()), [&](unsigned index, Vector<LayoutSetMember>& found) {
        LayoutID member(index);
        if (!canBeMember(inputs.program(), results, member))
            return;
        for (LayoutID set = provenBaseOf(inputs.hierarchy, member); set; set = provenBaseOf(inputs.hierarchy, set))
            found.append(LayoutSetMember { set, member });
    });
    sortAndRemoveDuplicates(inputs.loops(), setMembers);
    return setMembers;
}

static Vector<LayoutID> membersOf(const Program& program, std::span<const LayoutFlow> results, std::span<const LayoutSetMember> setMembers, LayoutID set)
{
    // What is below a member by a proven chain of bases is a member: a class below a class, and a layout derived from a declared one.
    Vector<LayoutID> members;
    for (LayoutID member : program.at(program.declaredMembersOf(set)).members) {
        if (canBeMember(program, results, member))
            members.append(member);
        for (const LayoutSetMember& setMember : std::ranges::equal_range(setMembers, member, { }, &LayoutSetMember::set))
            members.append(setMember.member);
    }

    std::ranges::sort(members);
    removeRepeatedElements(members);
    return members;
}

Vector<LayoutFlow> computeLayoutResults(const FlowFamilyInputs& inputs, std::span<const FlowShapeResult> shapesByAllocationClass)
{
    unsigned numberOfLayouts = static_cast<unsigned>(inputs.program().layouts.size());
    Vector<LayoutFlow> results(numberOfLayouts);
    forEachIndexInParallel(inputs.loops(), numberOfLayouts, [&](unsigned index) {
        computeOwnResults(inputs, shapesByAllocationClass, LayoutID(index), results[index]);
    });

    Vector<FieldCreation> ownFieldCreation = WTF::map(results, [](const LayoutFlow& flow) {
        return flow.fieldCreation;
    });
    Vector<LayoutSetMember> setMembers = collectSetMembers(inputs, results.span());
    forEachIndexInParallel(inputs.loops(), numberOfLayouts, [&](unsigned index) {
        LayoutID id(index);
        results[index].fieldCreation = fieldCreationOfChain(inputs.hierarchy, ownFieldCreation.span(), id);
        if (inputs.isEnabled(FlowResultKind::ProvenLayoutSets, index))
            results[index].members = membersOf(inputs.program(), results.span(), setMembers.span(), id);
    });
    return results;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
