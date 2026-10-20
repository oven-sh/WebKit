/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowAppendedFields.h"

#if ENABLE(AOT)

#include "AOTFlowObjectState.h"
#include "AOTGrouping.h"
#include "AOTResolveCalls.h"
#include <algorithm>
#include <cstdlib>
#include <tuple>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct NameSet {
    Vector<PropertyKeyID> names;
    Vector<AllocationClassID, 1> allocationClasses;
    uint64_t numberOfSites { 0 };
    bool allocatesRepeatedly { false };
};

} // anonymous namespace

// Whether the operation of a site says what it allocates, so that it can name a derived layout.
enum class SiteNaming : uint8_t {
    NamesNoLayout,
    // The run time allocates for a construction that stays by value, and it knows the declared layout of a class alone. No such construction enters a call-site version.
    NamesLayoutInCallSiteVersions,
    NamesLayout,
};

static bool canHaveDerivedLayouts(const FlowFamilyInputs& inputs, LayoutID id)
{
    if (!canAppendFieldsTo(inputs, id) || !inputs.isEnabled(FlowResultKind::DerivedLayouts, id.index()))
        return false;
    // FIXME: Off until placement and the run time know derived layouts.
    static const bool isSwitchedOn = !!getenv("BUN_AOT_DERIVED_LAYOUTS");
    if (!isSwitchedOn)
        return false;
    const Layout& layout = inputs.program().at(id);
    return layout.kind == LayoutKind::Record || (layout.constructor && !inputs.hierarchy.findBuiltinBase(id));
}

// ResolveCalls replaces such an operation by an allocation and a direct call. Any other construction allocates in the entrypoint of the constructor, which knows the declared layout alone.
static bool isConstructedDirectly(const FlowFamilyInputs& inputs, const FlowSite& site, const Value& construct)
{
    FunctionID constructor = inputs.program().at(site.description.layout).constructor;
    if (construct.newTargetOperand() || !constructor || !canConstructDirectly(inputs.program(), constructor))
        return false;
    if (!inputs.build().isEnabled(PassID::ResolveCalls, site.function) || !inputs.isEnabled(FlowResultKind::CalleeSets, site.function.index()))
        return false;

    bool hasOnlyConstructor = true;
    inputs.graph().forEachInstanceOf(site.function, [&](FlowInstanceID instance) {
        ValueID value = site.description.value;
        if (inputs.calls.hasUnknownCalleeAt(instance, value) || !inputs.calls.builtinLinksAt(instance, value).empty())
            hasOnlyConstructor = false;
        for (FlowCallLinkID link : inputs.calls.linksAt(instance, value))
            hasOnlyConstructor &= inputs.records().at(link).callee == constructor;
    });
    return hasOnlyConstructor;
}

static SiteNaming namingOf(const FlowFamilyInputs& inputs, AllocationSiteID id)
{
    const FlowSite& site = inputs.objects().at(id);
    const FlowSiteDescription& description = site.description;
    if (description.kind != FlowSiteKind::Layout || description.origin != FlowSiteOrigin::Operation || !description.value)
        return SiteNaming::NamesNoLayout;
    if (!site.function || inputs.graph().isHost(site.function) || inputs.solution.isPastWorkLimit(site.function))
        return SiteNaming::NamesNoLayout;

    const Value& operation = inputs.program().at(site.function).at(description.value);
    switch (operation.opcode) {
    case Opcode::NewObject:
        // The prototype object of a class below selects that class, and the object is then no object of the layout that the operation names.
        if (operation.layout() != description.layout)
            return SiteNaming::NamesNoLayout;
        return operation.prototypeOperand() ? SiteNaming::NamesLayoutInCallSiteVersions : SiteNaming::NamesLayout;
    case Opcode::Construct:
        return isConstructedDirectly(inputs, site, operation) ? SiteNaming::NamesLayout : SiteNaming::NamesNoLayout;
    default:
        return SiteNaming::NamesNoLayout;
    }
}

static SiteNaming namingOf(const FlowFamilyInputs& inputs, AllocationClassID allocationClass)
{
    SiteNaming weakest = SiteNaming::NamesLayout;
    for (AllocationSiteID site : inputs.objects().at(allocationClass).sites)
        weakest = std::min(weakest, namingOf(inputs, site));
    return weakest;
}

namespace {

// Numbers sequences from one. The empty sequence is zero.
class SequenceNumbering {
public:
    uint32_t numberOf(Vector<uint32_t>&& sequence)
    {
        if (sequence.isEmpty())
            return 0;
        Vector<uint32_t, 1>& candidates = m_numbersByHash.add(static_cast<uint64_t>(computeHash(sequence)) + 1, Vector<uint32_t, 1>()).iterator->value;
        for (uint32_t candidate : candidates) {
            if (m_sequences[candidate - 1] == sequence)
                return candidate;
        }
        m_sequences.append(WTF::move(sequence));
        candidates.append(static_cast<uint32_t>(m_sequences.size()));
        return candidates.last();
    }

    uint32_t size() const { return static_cast<uint32_t>(m_sequences.size()); }

private:
    Vector<Vector<uint32_t>> m_sequences;
    HashMap<uint64_t, Vector<uint32_t, 1>> m_numbersByHash;
};

struct OwnedObject {
    FlowInstanceID owner;
    AllocationSiteID site;
    FlowObjectID object;
    uint32_t nameSet { 0 };
};

// An operation in one part of its function: the function, the group of the instances, the operation.
using CallSitePart = std::tuple<FunctionID, uint32_t, ValueID>;

// An instance is in group zero, the part of its function that the version as read and the generic version stand for, or in the group of the call-site instances of its function
// that allocate as it does. A group becomes a version.
class CallSiteVersionPlanner {
public:
    CallSiteVersionPlanner(const FlowFamilyInputs& inputs, std::span<const PropertyKeyID> symbolsCreatedOnce, FlowObjectTable& objects)
        : m_inputs(inputs)
        , m_symbolsCreatedOnce(symbolsCreatedOnce)
        , m_objects(objects)
        , m_isBarred(inputs.graph().numberOfInstances())
    {
    }

    FlowCallSiteVersions run()
    {
        collectOwnedObjects();
        if (m_ownedObjects.isEmpty())
            return { };
        collectLinksToInstances();
        for (const OwnedObject& owned : m_ownedObjects)
            spreadToCallers(owned.owner);

        // The group of a caller names the groups of its callees: callers that differ only in a group that is given up are alike again.
        do
            computeGroups();
        while (barLoneGroups() || barGroupsPastBudget() || barGroupsEnteredAmbiguously());
        splitAllocationClasses();
        return createVersions();
    }

private:
    enum class GroupState : uint8_t { Unknown, InProgress, Known };

    const FlowGraph& graph() const { return m_inputs.graph(); }
    const Program& program() const { return m_inputs.program(); }

    bool canHaveVersions(FunctionID id) const
    {
        const Function& function = program().at(id);
        return function.hasBody()
            && function.mayBeCloned()
            && !function.isModuleBody()
            && !function.isUnsplitCoroutine()
            && (mayBeCalledDirectly(program(), id) || canConstructDirectly(program(), id))
            && m_inputs.isEnabled(FlowResultKind::CallVersions, id.index())
            && !m_inputs.solution.isPastWorkLimit(id);
    }

    // The passes make a direct call of such an operation whenever its callee is one function, and nothing but an operation of the program enters the instance.
    bool isEnteredByDirectCall(const FlowCallLink& link) const
    {
        if (link.kind != FlowCallLinkKind::Call && link.kind != FlowCallLinkKind::Construct)
            return false;
        if (!link.site || graph().isHost(link.caller) || m_inputs.solution.isPastWorkLimit(link.caller))
            return false;
        if (!m_inputs.build().isEnabled(PassID::ResolveCalls, link.caller) || !m_inputs.isEnabled(FlowResultKind::CalleeSets, link.caller.index()))
            return false;
        const Value& operation = program().at(link.caller).at(link.site);
        switch (operation.opcode) {
        case Opcode::Call:
        case Opcode::CallDirect:
            return link.kind == FlowCallLinkKind::Call && mayBeCalledDirectly(program(), link.callee);
        case Opcode::Construct:
            return link.kind == FlowCallLinkKind::Construct && !operation.newTargetOperand() && canConstructDirectly(program(), link.callee);
        default:
            return false;
        }
    }

    bool canBeInGroup(FlowInstanceID instance) const
    {
        const FlowInstance& record = graph().at(instance);
        if (!record.isCallSiteInstance() || !canHaveVersions(record.function))
            return false;
        return std::ranges::all_of(linksTo(instance), [&](FlowCallLinkID link) {
            return isEnteredByDirectCall(m_inputs.records().at(link));
        });
    }

    std::span<const FlowCallLinkID> linksTo(FlowInstanceID instance) const
    {
        return entriesOfGroup(m_firstLinkToInstance, m_linksToInstance, instance.index());
    }

    void collectLinksToInstances()
    {
        std::span<const FlowCallLink> links = m_inputs.records().callLinks();
        groupByIndex(graph().numberOfInstances(), m_firstLinkToInstance, m_linksToInstance, [&](const auto& visit) {
            for (size_t index = 0; index < links.size(); ++index)
                visit(links[index].calleeInstance.index(), FlowCallLinkID(index));
        });
    }

    bool canBeSplit(AllocationClassID allocationClass) const
    {
        const FlowAllocationClassMembers& members = m_objects.at(allocationClass);
        if (!members.layout || members.copies.isEmpty() || !canHaveDerivedLayouts(m_inputs, members.layout) || m_inputs.hierarchy.hasOnlyOrdinaryObjects(allocationClass))
            return false;
        return namingOf(m_inputs, allocationClass) != SiteNaming::NamesNoLayout;
    }

    void collectOwnedObjects()
    {
        // The first object of a site that the solve created has no owner in its record.
        HashMap<FlowObjectID, FlowInstanceID> ownersOfFirstObjects;
        for (const auto& entry : m_inputs.solution.objectState.solveObjects) {
            if (!m_objects.at(entry.value).instance)
                ownersOfFirstObjects.add(entry.value, entry.key.second);
        }

        for (unsigned index = 0; index < m_objects.numberOfAllocationClasses(); ++index) {
            AllocationClassID allocationClass(index);
            if (!canBeSplit(allocationClass)) {
                // FIXME: Goes with the switch.
                static const bool traces = !!getenv("BUN_AOT_DERIVED_LAYOUTS_TRACE");
                const FlowAllocationClassMembers& members = m_objects.at(allocationClass);
                if (traces && members.layout && !members.copies.isEmpty())
                    dataLogLn("class C", index, " of L", members.layout.index(), " with ", members.copies.size(), " copies is not split: naming ", static_cast<unsigned>(namingOf(m_inputs, allocationClass)), " ordinary ", m_inputs.hierarchy.hasOnlyOrdinaryObjects(allocationClass));
                continue;
            }
            m_splitClasses.append(allocationClass);
            LayoutID layout = m_objects.at(allocationClass).layout;
            forEachObjectOfClass(m_objects, allocationClass, [&](FlowObjectID object) {
                if (!m_inputs.records().eventsOf(object).contains(FlowObjectEvent::Allocated))
                    return;
                const FlowObject& record = m_objects.at(object);
                FlowInstanceID owner = record.instance ? record.instance : ownersOfFirstObjects.get(object);
                if (!owner || !graph().at(owner).isCallSiteInstance())
                    return;
                Vector<PropertyKeyID> names;
                appendStoredKeys(m_inputs, object, names);
                keepCandidateKeys(m_inputs, m_symbolsCreatedOnce, layout, names);
                // An object that gets no name says nothing about its caller: it stays with the objects of the common part.
                Vector<uint32_t> sequence;
                for (PropertyKeyID name : names)
                    sequence.append(name.index());
                m_ownedObjects.append({ owner, record.site, object, m_nameSets.numberOf(WTF::move(sequence)) });
                m_ownerOfObject.add(object, owner);
            });
        }
        std::ranges::sort(m_ownedObjects, { }, [](const OwnedObject& owned) {
            return std::tuple { owned.owner, owned.site, owned.object };
        });
    }

    std::span<const OwnedObject> objectsOwnedBy(FlowInstanceID instance) const
    {
        return std::ranges::equal_range(m_ownedObjects, instance, { }, &OwnedObject::owner);
    }

    // What an instance allocates tells its callers apart too, as far up as the callers are call-site instances.
    void spreadToCallers(FlowInstanceID instance)
    {
        Vector<FlowInstanceID, 8> worklist { instance };
        while (!worklist.isEmpty()) {
            FlowInstanceID current = worklist.takeLast();
            if (!m_isToBeGrouped.add(current).isNewEntry)
                continue;
            m_instancesToGroup.append(current);
            for (FlowCallLinkID link : linksTo(current)) {
                FlowInstanceID caller = m_inputs.records().at(link).callerInstance;
                if (caller && graph().at(caller).isCallSiteInstance())
                    worklist.append(caller);
            }
        }
    }

    void computeGroups()
    {
        m_groups = { };
        m_groupOfInstance.fill(0u, graph().numberOfInstances());
        m_state.fill(GroupState::Unknown, graph().numberOfInstances());
        for (FlowInstanceID instance : m_instancesToGroup)
            groupOf(instance);
    }

    uint32_t groupOf(FlowInstanceID instance)
    {
        switch (m_state[instance.index()]) {
        case GroupState::Known:
            return m_groupOfInstance[instance.index()];
        case GroupState::InProgress:
            return 0;
        case GroupState::Unknown:
            break;
        }
        if (!m_isToBeGrouped.contains(instance) || m_isBarred.quickGet(instance.index()) || !canBeInGroup(instance)) {
            // FIXME: Goes with the switch.
            static const bool traces = !!getenv("BUN_AOT_DERIVED_LAYOUTS_TRACE");
            if (traces && m_isToBeGrouped.contains(instance) && !m_isBarred.quickGet(instance.index()))
                dataLogLn("no group for i", instance.index(), " of f", graph().at(instance).function.index(), canHaveVersions(graph().at(instance).function) ? ": a link is no direct call" : ": the function can have no versions");
            m_state[instance.index()] = GroupState::Known;
            return 0;
        }

        m_state[instance.index()] = GroupState::InProgress;
        Vector<uint32_t> signature;
        for (const OwnedObject& owned : objectsOwnedBy(instance)) {
            if (!owned.nameSet)
                continue;
            signature.append(owned.site.index());
            signature.append(owned.nameSet);
        }
        size_t numberOfAllocationWords = signature.size();
        for (FlowCallLinkID link : m_inputs.calls.linksFrom(instance)) {
            const FlowCallLink& record = m_inputs.records().at(link);
            if (uint32_t group = groupOf(record.calleeInstance)) {
                signature.append(record.site.index());
                signature.append(group);
            }
        }
        uint32_t group = 0;
        if (!signature.isEmpty()) {
            signature.append(static_cast<uint32_t>(numberOfAllocationWords));
            signature.append(graph().at(instance).function.index());
            group = m_groups.numberOf(WTF::move(signature));
        }
        m_groupOfInstance[instance.index()] = group;
        m_state[instance.index()] = GroupState::Known;
        return group;
    }

    Vector<Vector<FlowInstanceID, 1>> collectInstancesByGroup() const
    {
        Vector<Vector<FlowInstanceID, 1>> instances(m_groups.size() + 1);
        for (unsigned index = 0; index < m_groupOfInstance.size(); ++index) {
            if (uint32_t group = m_groupOfInstance[index])
                instances[group].append(FlowInstanceID(index));
        }
        return instances;
    }

    void bar(const Vector<FlowInstanceID, 1>& instancesOfGroup)
    {
        for (FlowInstanceID instance : instancesOfGroup)
            m_isBarred.quickSet(instance.index());
    }

    // A function that always allocates alike needs one version, and it has one.
    bool barLoneGroups()
    {
        bool hasBarred = false;
        Vector<Vector<FlowInstanceID, 1>> instancesByGroup = collectInstancesByGroup();
        for (const Vector<FlowInstanceID, 1>& instances : instancesByGroup) {
            if (instances.isEmpty())
                continue;
            uint32_t group = m_groupOfInstance[instances[0].index()];
            bool isOnlyPart = true;
            graph().forEachInstanceOf(graph().at(instances[0]).function, [&](FlowInstanceID instance) {
                isOnlyPart &= m_groupOfInstance[instance.index()] == group;
            });
            if (!isOnlyPart)
                continue;
            bar(instances);
            hasBarred = true;
        }
        return hasBarred;
    }

    bool barGroupsPastBudget()
    {
        bool hasBarred = false;
        uint64_t budget = m_inputs.build().limit(LimitID::MaxCallSiteVersionValues);
        Vector<Vector<FlowInstanceID, 1>> instancesByGroup = collectInstancesByGroup();
        Vector<uint32_t> groups;
        for (uint32_t group = 1; group < instancesByGroup.size(); ++group) {
            if (!instancesByGroup[group].isEmpty())
                groups.append(group);
        }
        auto sizeOf = [&](uint32_t group) {
            return program().at(graph().at(instancesByGroup[group][0]).function).values.size();
        };
        std::ranges::sort(groups, { }, [&](uint32_t group) {
            return std::pair { sizeOf(group), group };
        });
        uint64_t growth = 0;
        for (uint32_t group : groups) {
            if (sizeOf(group) <= budget - growth) {
                growth += sizeOf(group);
                continue;
            }
            m_inputs.build().gaveUp(LimitID::MaxCallSiteVersionValues, Subject { .function = graph().at(instancesByGroup[group][0]).function });
            bar(instancesByGroup[group]);
            hasBarred = true;
        }
        return hasBarred;
    }

    // One operation in one version of its function enters one version of each function it may call. Where the links of the instances that share the operation disagree,
    // or the passes may leave a call by value for some callee, the callees are entered as if they had no versions.
    bool barGroupsEnteredAmbiguously()
    {
        std::span<const FlowCallLink> links = m_inputs.records().callLinks();
        HashMap<CallSitePart, Vector<std::pair<FunctionID, uint32_t>, 1>> calleesAt;
        HashSet<uint32_t, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> ambiguousGroups;
        auto partOf = [&](const FlowCallLink& link) {
            return CallSitePart { link.caller, m_groupOfInstance[link.callerInstance.index()], link.site };
        };

        for (const FlowCallLink& link : links) {
            if (m_groupOfInstance[link.calleeInstance.index()])
                calleesAt.add(partOf(link), Vector<std::pair<FunctionID, uint32_t>, 1>());
        }
        if (calleesAt.isEmpty())
            return false;
        for (const FlowCallLink& link : links) {
            if (!link.callerInstance)
                continue;
            auto callees = calleesAt.find(partOf(link));
            if (callees == calleesAt.end())
                continue;
            std::pair callee { link.callee, m_groupOfInstance[link.calleeInstance.index()] };
            if (!callees->value.contains(callee))
                callees->value.append(callee);
        }

        uint64_t maxCallees = m_inputs.build().limit(LimitID::MaxDispatchCases);
        for (const auto& entry : calleesAt) {
            auto [caller, groupOfCaller, site] = entry.key;
            const auto& callees = entry.value;
            bool isAmbiguous = callees.size() > maxCallees;
            bool constructs = program().at(caller).at(site).opcode == Opcode::Construct;
            // FIXME: Goes with the switch.
            static const bool traces = !!getenv("BUN_AOT_DERIVED_LAYOUTS_TRACE");
            if (traces && isAmbiguous)
                dataLogLn("versions given up at f", caller.index(), ":v", site.index(), " group ", groupOfCaller, ": ", callees.size(), " callees");
            for (auto [function, group] : callees) {
                bool hasSeveralGroups = std::ranges::count(callees, function, &std::pair<FunctionID, uint32_t>::first) > 1;
                bool cannotBeEntered = !(constructs ? canConstructDirectly(program(), function) : mayBeCalledDirectly(program(), function));
                if (traces && (hasSeveralGroups || cannotBeEntered))
                    dataLogLn("versions given up at f", caller.index(), ":v", site.index(), " group ", groupOfCaller, ": callee f", function.index(), " group ", group, hasSeveralGroups ? " has several groups" : "", cannotBeEntered ? " cannot be entered directly" : "");
                isAmbiguous |= hasSeveralGroups || cannotBeEntered;
            }
            graph().forEachInstanceOf(caller, [&](FlowInstanceID instance) {
                if (m_groupOfInstance[instance.index()] != groupOfCaller)
                    return;
                bool hasOtherCallee = m_inputs.calls.hasUnknownCalleeAt(instance, site) || !m_inputs.calls.builtinLinksAt(instance, site).empty();
                if (traces && hasOtherCallee)
                    dataLogLn("versions given up at f", caller.index(), ":v", site.index(), " group ", groupOfCaller, ": unknown or built-in callee in i", instance.index());
                isAmbiguous |= hasOtherCallee;
            });
            if (!isAmbiguous)
                continue;
            for (auto [function, group] : callees) {
                if (group)
                    ambiguousGroups.add(group);
            }
        }
        if (ambiguousGroups.isEmpty())
            return false;
        for (unsigned index = 0; index < m_groupOfInstance.size(); ++index) {
            if (uint32_t group = m_groupOfInstance[index]; group && ambiguousGroups.contains(group))
                m_isBarred.quickSet(index);
        }
        return true;
    }

    void splitAllocationClasses()
    {
        for (AllocationClassID allocationClass : m_splitClasses) {
            Vector<std::pair<uint32_t, FlowObjectID>> objectsByGroup;
            forEachObjectOfClass(m_objects, allocationClass, [&](FlowObjectID object) {
                FlowInstanceID owner = m_ownerOfObject.get(object);
                objectsByGroup.append({ owner ? m_groupOfInstance[owner.index()] : 0, object });
            });
            std::ranges::sort(objectsByGroup);
            if (objectsByGroup.first().first == objectsByGroup.last().first)
                continue;
            m_objects.splitAllocationClass(allocationClass, objectsByGroup.span());
        }
    }

    FlowCallSiteVersions createVersions() const
    {
        FlowCallSiteVersions result;
        for (const Vector<FlowInstanceID, 1>& instances : collectInstancesByGroup()) {
            if (!instances.isEmpty())
                result.versions.append({ graph().at(instances[0]).function, instances });
        }
        if (result.versions.isEmpty())
            return result;
        std::ranges::sort(result.versions, { }, [](const FlowCallSiteVersion& version) {
            return std::pair { version.body, version.instances[0] };
        });
        result.versionOfInstance.fill(0u, graph().numberOfInstances());
        for (uint32_t index = 0; index < result.versions.size(); ++index) {
            for (FlowInstanceID instance : result.versions[index].instances)
                result.versionOfInstance[instance.index()] = index + 1;
        }
        return result;
    }

    const FlowFamilyInputs& m_inputs;
    std::span<const PropertyKeyID> m_symbolsCreatedOnce;
    FlowObjectTable& m_objects;
    Vector<AllocationClassID> m_splitClasses;
    Vector<OwnedObject> m_ownedObjects;
    HashMap<FlowObjectID, FlowInstanceID> m_ownerOfObject;
    SequenceNumbering m_nameSets;
    SequenceNumbering m_groups;
    Vector<uint32_t> m_firstLinkToInstance;
    Vector<FlowCallLinkID> m_linksToInstance;
    HashSet<FlowInstanceID> m_isToBeGrouped;
    Vector<FlowInstanceID> m_instancesToGroup;
    Vector<uint32_t> m_groupOfInstance;
    Vector<GroupState> m_state;
    BitVector m_isBarred;
};

} // anonymous namespace

FlowCallSiteVersions planCallSiteVersions(const FlowFamilyInputs& inputs, std::span<const PropertyKeyID> symbolsCreatedOnce, FlowObjectTable& objects)
{
    RELEASE_ASSERT(&objects == &inputs.objects());
    return CallSiteVersionPlanner(inputs, symbolsCreatedOnce, objects).run();
}

static NameSet collectNamesOf(const FlowFamilyInputs& inputs, std::span<const PropertyKeyID> symbolsCreatedOnce, LayoutID layout, AllocationClassID allocationClass)
{
    NameSet set;
    set.allocationClasses.append(allocationClass);
    set.numberOfSites = inputs.objects().at(allocationClass).sites.size();
    forEachObjectOfClass(inputs.objects(), allocationClass, [&](FlowObjectID object) {
        OptionSet<FlowObjectEvent> events = inputs.records().eventsOf(object);
        if (!events.contains(FlowObjectEvent::Allocated))
            return;
        set.allocatesRepeatedly |= events.contains(FlowObjectEvent::AllocatedRepeatedly);
        appendStoredKeys(inputs, object, set.names);
    });
    keepCandidateKeys(inputs, symbolsCreatedOnce, layout, set.names);
    return set;
}

static void removeNames(Vector<PropertyKeyID>& names, const Vector<PropertyKeyID>& ascendingNamesToRemove)
{
    names.removeAllMatching([&](PropertyKeyID name) {
        return std::ranges::binary_search(ascendingNamesToRemove, name);
    });
}

static void mergeInto(NameSet& set, NameSet&& other)
{
    set.names.appendVector(other.names);
    std::ranges::sort(set.names);
    removeRepeatedElements(set.names);
    set.allocationClasses.appendVector(other.allocationClasses);
    set.numberOfSites += other.numberOfSites;
    set.allocatesRepeatedly |= other.allocatesRepeatedly;
}

static Vector<NameSet> uniteEqualSets(Vector<NameSet>&& sets)
{
    std::ranges::sort(sets, [](const NameSet& a, const NameSet& b) {
        if (a.names != b.names)
            return std::ranges::lexicographical_compare(a.names, b.names);
        return a.allocationClasses[0] < b.allocationClasses[0];
    });
    Vector<NameSet> united;
    for (NameSet& set : sets) {
        if (!united.isEmpty() && united.last().names == set.names)
            mergeInto(united.last(), WTF::move(set));
        else
            united.append(WTF::move(set));
    }
    return united;
}

// Past the limit the sets whose classes allocate least share one layout.
static void foldLightestSets(const FlowFamilyInputs& inputs, LayoutID layout, Vector<NameSet>& sets)
{
    if (!inputs.build().exceeds(LimitID::MaxDerivedLayoutsPerFamily, sets.size(), Subject { .layout = layout }))
        return;
    std::ranges::sort(sets, [](const NameSet& a, const NameSet& b) {
        return std::tuple { a.allocatesRepeatedly, a.numberOfSites, b.allocationClasses[0] } > std::tuple { b.allocatesRepeatedly, b.numberOfSites, a.allocationClasses[0] };
    });
    size_t kept = static_cast<size_t>(inputs.build().limit(LimitID::MaxDerivedLayoutsPerFamily));
    RELEASE_ASSERT(kept);
    for (size_t index = kept; index < sets.size(); ++index)
        mergeInto(sets[kept - 1], WTF::move(sets[index]));
    sets.shrink(kept);
}

static bool isAllocatedByCallSiteVersions(const FlowFamilyInputs& inputs, const FlowCallSiteVersions& versions, AllocationClassID allocationClass)
{
    bool isAllocatedByVersions = true;
    forEachObjectOfClass(inputs.objects(), allocationClass, [&](FlowObjectID object) {
        if (!inputs.records().eventsOf(object).contains(FlowObjectEvent::Allocated))
            return;
        FlowInstanceID owner = inputs.objects().at(object).instance;
        isAllocatedByVersions &= owner && versions.versionOf(owner);
    });
    return isAllocatedByVersions;
}

static bool canNameDerivedLayout(const FlowFamilyInputs& inputs, const FlowCallSiteVersions& versions, AllocationClassID allocationClass)
{
    switch (namingOf(inputs, allocationClass)) {
    case SiteNaming::NamesNoLayout:
        return false;
    case SiteNaming::NamesLayoutInCallSiteVersions:
        return isAllocatedByCallSiteVersions(inputs, versions, allocationClass);
    case SiteNaming::NamesLayout:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<NameSet> planDerivedLayouts(const FlowFamilyInputs& inputs, const FlowCallSiteVersions& versions, std::span<const PropertyKeyID> symbolsCreatedOnce, LayoutID layout)
{
    Vector<NameSet> sets;
    Vector<PropertyKeyID> namesOfDeclaredLayout;
    bool declaredLayoutIsAllocated = false;
    for (AllocationClassID allocationClass : inputs.objects().allocationClassesOf(layout)) {
        if (inputs.hierarchy.hasOnlyOrdinaryObjects(allocationClass))
            continue;
        NameSet set = collectNamesOf(inputs, symbolsCreatedOnce, layout, allocationClass);
        if (canNameDerivedLayout(inputs, versions, allocationClass)) {
            sets.append(WTF::move(set));
            continue;
        }
        declaredLayoutIsAllocated = true;
        namesOfDeclaredLayout.appendVector(set.names);
    }
    std::ranges::sort(namesOfDeclaredLayout);
    removeRepeatedElements(namesOfDeclaredLayout);

    sets.removeAllMatching([&](NameSet& set) {
        removeNames(set.names, namesOfDeclaredLayout);
        declaredLayoutIsAllocated |= set.names.isEmpty();
        return set.names.isEmpty();
    });
    sets = uniteEqualSets(WTF::move(sets));
    // One set that every object gets is appended to the declared layout.
    if (sets.size() + declaredLayoutIsAllocated < 2)
        return { };
    foldLightestSets(inputs, layout, sets);
    std::ranges::sort(sets, { }, [](const NameSet& set) {
        return set.allocationClasses[0];
    });
    return sets;
}

void deriveLayouts(const FlowFamilyInputs& inputs, const FlowCallSiteVersions& versions, std::span<const PropertyKeyID> symbolsCreatedOnce, Program& program, FlowObjectTable& objects)
{
    RELEASE_ASSERT(&program == &inputs.program() && &objects == &inputs.objects());
    unsigned numberOfDeclaredLayouts = program.layouts.size();
    Vector<Vector<NameSet>> plans(numberOfDeclaredLayouts);
    forEachIndexInParallel(inputs.loops(), numberOfDeclaredLayouts, [&](unsigned index) {
        LayoutID layout(index);
        if (canHaveDerivedLayouts(inputs, layout) && objects.allocationClassesOf(layout).size() > 1)
            plans[index] = planDerivedLayouts(inputs, versions, symbolsCreatedOnce, layout);
    });

    ProgramEditor editor(program);
    uint64_t numberOfDerivedLayouts = 0;
    for (unsigned index = 0; index < numberOfDeclaredLayouts; ++index) {
        for (const NameSet& set : plans[index]) {
            LayoutID derived = editor.addLayout(createDerivedLayout(program, LayoutID(index)));
            for (AllocationClassID allocationClass : set.allocationClasses)
                objects.moveToDerivedLayout(allocationClass, derived);
            ++numberOfDerivedLayouts;
        }
    }
    inputs.build().statistics().count(StatisticID::FlowDerivedLayouts, numberOfDerivedLayouts);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
