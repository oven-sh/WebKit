/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTable.h"
#include "AOTStringArena.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned containersPerTask = 256;

static constexpr OptionSet<ObjectMutation> unprovenContentsMutations {
    ObjectMutation::ReachNativeCode,
    ObjectMutation::ReachUnknownJavaScript,
    ObjectMutation::RedefineProperty,
    ObjectMutation::ChangePrototype,
};

namespace {

struct Container {
    FlowObjectID object;
    BuiltinClassID builtinClass;
    uint32_t subject { 0 };
};

using DependencyList = Vector<uint32_t, 2>;

struct ConversionSchedule {
    unsigned numberOfLevels() const { return static_cast<unsigned>(firstTaskOfLevel.size()) - 1; }
    unsigned firstTaskOf(unsigned level) const { return firstTaskOfLevel[level]; }
    unsigned numberOfTasksOf(unsigned level) const { return firstTaskOfLevel[level + 1] - firstTaskOfLevel[level]; }

    std::span<const uint32_t> containersOf(unsigned task) const LIFETIME_BOUND
    {
        uint32_t first = firstPositionOfTask[task];
        return order.subspan(first, firstPositionOfTask[task + 1] - first);
    }

    Vector<uint32_t> order;
    Vector<uint32_t> firstPositionOfTask;
    Vector<uint32_t> firstTaskOfLevel;
};

struct ContentsConversion {
    FlowContentsResult& resultOf(uint32_t index) const { return results[containers[index].object.index()]; }

    const FlowFamilyInputs& inputs;
    const FlowTypeConverter& converter;
    std::span<const Container> containers;
    std::span<FlowContentsResult> results;
};

} // anonymous namespace

static Vector<Container> collectContainers(const FlowFamilyInputs& inputs)
{
    const FlowObjectTable& objects = inputs.objects();
    const BuiltinTable& builtins = inputs.solution.builtins();
    return collectInParallel<Container>(inputs.loops(), objects.size(), [&](unsigned index, Vector<Container>& containers) {
        FlowObjectID object(index);
        const FlowObject& record = objects.at(object);
        if (record.kind == FlowObjectKind::Instance && record.builtinClass && builtins.numberOfSlotsInType(record.builtinClass))
            containers.append(Container { object, record.builtinClass, record.allocationClass.index() });
    });
}

static void forEachContentsSlot(const BuiltinTable& builtins, BuiltinClassID builtinClass, NOESCAPE const Invocable<void(const BuiltinSlot&)> auto& functor)
{
    for (BuiltinSlotID id : builtins.at(builtinClass).slots) {
        const BuiltinSlot& slot = builtins.at(id);
        if (slot.indexInType)
            functor(slot);
    }
}

static Vector<FlowNodeID, 4> contentsNodesOf(const FlowGraph& graph, FlowObjectID object, ContentsSlot contentsSlot)
{
    Vector<FlowNodeID, 4> nodes;
    if (contentsSlot == ContentsSlot::Element) {
        forEachElementPart(graph, object, arrayIndexRange, [&](FlowNodeID node) {
            if (node)
                nodes.append(node);
        });
    }
    if (nodes.isEmpty()) {
        if (FlowNodeID node = contentsNode(graph, object, contentsSlot))
            nodes.append(node);
    }
    return nodes;
}

static bool hasProvenContents(const FlowFamilyInputs& inputs, const Container& container)
{
    return inputs.isEnabled(FlowResultKind::ProvenContents, container.subject)
        && !inputs.factsOf(container.object).mutations.containsAny(unprovenContentsMutations);
}

static FlowObjectSet objectsInContentsOf(const FlowFamilyInputs& inputs, const Container& container)
{
    const FlowGraph& graph = inputs.graph();
    FlowObjectSet objects;
    forEachContentsSlot(inputs.solution.builtins(), container.builtinClass, [&](const BuiltinSlot& slot) {
        for (FlowNodeID node : contentsNodesOf(graph, container.object, *slot.contentsSlot))
            objects.unite(graph.valueOf(node).objects);
    });
    return objects;
}

static Vector<DependencyList> computeDependencies(const FlowFamilyInputs& inputs, std::span<const Container> containers)
{
    Vector<DependencyList> dependencies(containers.size());
    forEachIndexInParallel(inputs.loops(), static_cast<unsigned>(containers.size()), [&](unsigned index) {
        objectsInContentsOf(inputs, containers[index]).forEach([&](FlowObjectID object) {
            auto dependency = std::ranges::lower_bound(containers, object, { }, &Container::object);
            if (dependency != containers.end() && dependency->object == object)
                dependencies[index].append(static_cast<uint32_t>(dependency - containers.begin()));
        });
    });
    return dependencies;
}

static Vector<uint32_t> computeComponentLevels(const StronglyConnectedComponents& components, std::span<const DependencyList> dependencies, std::span<const uint32_t> containersByComponent)
{
    Vector<uint32_t> levelOfComponent;
    levelOfComponent.fill(0, components.numberOfComponents);
    for (uint32_t index : containersByComponent) {
        uint32_t component = components.componentOf[index];
        for (uint32_t dependency : dependencies[index]) {
            uint32_t dependencyComponent = components.componentOf[dependency];
            if (dependencyComponent != component)
                levelOfComponent[component] = std::max(levelOfComponent[component], levelOfComponent[dependencyComponent] + 1);
        }
    }
    return levelOfComponent;
}

static ConversionSchedule scheduleConversions(const ParallelLoops& loops, std::span<const DependencyList> dependencies)
{
    uint32_t count = static_cast<uint32_t>(dependencies.size());
    StronglyConnectedComponents components = computeStronglyConnectedComponents(count, [&](unsigned index) -> const DependencyList& {
        return dependencies[index];
    });
    auto componentOf = [&](uint32_t index) {
        return components.componentOf[index];
    };

    ConversionSchedule schedule;
    schedule.order = Vector<uint32_t>(count, [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    parallelSort(loops, schedule.order.mutableSpan(), [&](uint32_t a, uint32_t b) {
        return std::pair { componentOf(a), a } < std::pair { componentOf(b), b };
    });

    Vector<uint32_t> levelOfComponent = computeComponentLevels(components, dependencies, schedule.order.span());
    auto levelOf = [&](uint32_t index) {
        return levelOfComponent[componentOf(index)];
    };
    parallelSort(loops, schedule.order.mutableSpan(), [&](uint32_t a, uint32_t b) {
        return std::tuple { levelOf(a), componentOf(a), a } < std::tuple { levelOf(b), componentOf(b), b };
    });

    for (uint32_t position = 0; position < count; ++position) {
        uint32_t index = schedule.order[position];
        bool startsLevel = !position || levelOf(index) != levelOf(schedule.order[position - 1]);
        bool startsComponent = !position || componentOf(index) != componentOf(schedule.order[position - 1]);
        if (startsLevel)
            schedule.firstTaskOfLevel.append(static_cast<uint32_t>(schedule.firstPositionOfTask.size()));
        if (startsLevel || (startsComponent && position - schedule.firstPositionOfTask.last() >= containersPerTask))
            schedule.firstPositionOfTask.append(position);
    }
    schedule.firstTaskOfLevel.append(static_cast<uint32_t>(schedule.firstPositionOfTask.size()));
    schedule.firstPositionOfTask.append(count);
    return schedule;
}

static Type typeAsReadOf(const FlowFamilyInputs& inputs, std::span<const FlowNodeID> nodes, TypeArena& arena)
{
    Type asRead = Type::never();
    for (FlowNodeID node : nodes) {
        TypeID type = inputs.graph().typeAsRead(node);
        asRead = asRead.unionWith(type ? inputs.program().at(type) : Type::anything(), arena);
    }
    return asRead;
}

// The parser gives each array the storage of what that array holds, and flow has one type for the elements of all the arrays of a text.
static bool isCreatedByParser(const FlowObjectTable& objects, FlowObjectID array)
{
    AllocationSiteID site = objects.at(array).site;
    return site && objects.at(site).description.kind == FlowSiteKind::PlainArray;
}

static FlowContentsResult convertContents(const ContentsConversion& conversion, const Container& container, TypeArena& arena)
{
    const FlowFamilyInputs& inputs = conversion.inputs;
    const BuiltinTable& builtins = inputs.solution.builtins();
    bool predictsStorage = container.builtinClass == BuiltinClasses::Array && !isCreatedByParser(inputs.objects(), container.object)
        && inputs.isEnabled(FlowResultKind::ArrayStorageKinds, container.subject);

    FlowContentsResult result;
    result.isProven = hasProvenContents(inputs, container);
    result.contents.grow(builtins.numberOfSlotsInType(container.builtinClass));
    forEachContentsSlot(builtins, container.builtinClass, [&](const BuiltinSlot& slot) {
        Vector<FlowNodeID, 4> nodes = contentsNodesOf(inputs.graph(), container.object, *slot.contentsSlot);
        if (nodes.isEmpty())
            return;
        FlowPositionType converted = conversion.converter.convert(nodes.span(), typeAsReadOf(inputs, nodes.span(), arena), container.subject, arena);
        const Type& contents = result.isProven || !converted.prediction ? converted.type : *converted.prediction;
        result.contents[*slot.indexInType] = arena.intern(contents);
        if (result.isProven && converted.prediction) {
            result.predictedContents.grow(result.contents.size());
            result.predictedContents[*slot.indexInType] = arena.intern(*converted.prediction);
        }
        if (predictsStorage && slot.contentsSlot == ContentsSlot::Element)
            result.predictedStorage = predictedArrayStorageOf(contents);
    });
    return result;
}

// Converts every container of the level, or only the repeated ones, whose storage their sites have decided.
static void convertLevel(const ContentsConversion& conversion, const ConversionSchedule& schedule, unsigned level, const BitVector* repeated, TypeTable& types)
{
    const ParallelLoops& loops = conversion.inputs.loops();
    unsigned firstTask = schedule.firstTaskOf(level);
    unsigned numberOfTasks = schedule.numberOfTasksOf(level);
    auto forEachContainerOf = [&](unsigned task, NOESCAPE const Invocable<void(uint32_t, FlowContentsResult&)> auto& functor) {
        for (uint32_t index : schedule.containersOf(firstTask + task)) {
            if (!repeated || repeated->get(index))
                functor(index, conversion.resultOf(index));
        }
    };

    TypeInterningPhase phase(types, numberOfTasks);
    forEachIndexInParallel(loops, numberOfTasks, [&](unsigned task) {
        TypeArena arena(phase, task);
        forEachContainerOf(task, [&](uint32_t index, FlowContentsResult& result) {
            ArrayStorage storageOfSite = result.predictedStorage;
            result = convertContents(conversion, conversion.containers[index], arena);
            if (repeated)
                result.predictedStorage = storageOfSite;
        });
    });
    TypeRemap remap = phase.finish(StringRemap());
    if (remap.isIdentity())
        return;

    forEachIndexInParallel(loops, numberOfTasks, [&](unsigned task) {
        if (remap.isIdentityFor(task))
            return;
        forEachContainerOf(task, [&](uint32_t, FlowContentsResult& result) {
            for (TypeID& contents : result.contents)
                remap.apply(contents);
            for (TypeID& contents : result.predictedContents)
                remap.apply(contents);
        });
    });
}

static unsigned indexOfArrayElementsInType(const BuiltinTable& builtins)
{
    std::optional<unsigned> index;
    forEachContentsSlot(builtins, BuiltinClasses::Array, [&](const BuiltinSlot& slot) {
        if (slot.contentsSlot == ContentsSlot::Element)
            index = *slot.indexInType;
    });
    return index.value();
}

static std::optional<ArrayStorage> storageOfNewHolesAt(const FlowFamilyInputs& inputs, AllocationSiteID id)
{
    const FlowSite& site = inputs.objects().at(id);
    if (!site.description.value)
        return std::nullopt;
    switch (site.description.origin) {
    case FlowSiteOrigin::Operation:
        return arrayStorageOfNewHoles(inputs.program().at(site.function).at(site.description.value));
    case FlowSiteOrigin::BuiltinResult:
        // Only a part of the result type of an overload has an ordinal, and the overload is its upper half.
        if (!site.description.ordinal)
            return std::nullopt;
        return arrayStorageOfNewHoles(inputs.solution.builtins().at(BuiltinOverloadID(site.description.ordinal >> 16)));
    default:
        return std::nullopt;
    }
}

static void forEachArrayWithStorage(const ContentsConversion& conversion, NOESCAPE const Invocable<void(uint32_t, FlowContentsResult&, AllocationSiteID)> auto& functor)
{
    const FlowObjectTable& objects = conversion.inputs.objects();
    for (uint32_t index = 0; index < conversion.containers.size(); ++index) {
        FlowContentsResult& result = conversion.resultOf(index);
        AllocationSiteID site = objects.at(conversion.containers[index].object).site;
        if (site && result.predictedStorage != ArrayStorage::Other)
            functor(index, result, site);
    }
}

static Vector<TypeID> uniteElementsWithinSites(const ContentsConversion& conversion, TypeTable& types)
{
    unsigned elementsIndex = indexOfArrayElementsInType(conversion.inputs.solution.builtins());
    Vector<TypeID> elementsBySite(conversion.inputs.objects().numberOfSites());
    TypeInterningPhase phase(types, 1);
    {
        TypeArena arena(phase, 0);
        forEachArrayWithStorage(conversion, [&](uint32_t, FlowContentsResult& result, AllocationSiteID site) {
            TypeID& elementsOfSite = elementsBySite[site.index()];
            TypeID elements = result.contents[elementsIndex];
            elementsOfSite = elementsOfSite ? arena.unionOf(elementsOfSite, elements) : elements;
        });
    }
    TypeRemap remap = phase.finish(StringRemap());
    for (TypeID& elements : elementsBySite)
        remap.apply(elements);
    return elementsBySite;
}

// The code of a site allocates one storage for all the objects that stand for its arrays, and the readers predict the storage that was allocated.
// Answers the arrays whose prediction this changes.
static BitVector shareStorageWithinSites(const ContentsConversion& conversion, std::span<const TypeID> elementsBySite, const TypeTable& types)
{
    BitVector changedArrays(conversion.containers.size());
    forEachArrayWithStorage(conversion, [&](uint32_t index, FlowContentsResult& result, AllocationSiteID site) {
        ArrayStorage storage = storageOfNewHolesAt(conversion.inputs, site).value_or(predictedArrayStorageOf(types.at(elementsBySite[site.index()])));
        if (storage == result.predictedStorage)
            return;
        result.predictedStorage = storage;
        changedArrays.set(index);
    });
    return changedArrays;
}

static BitVector findHoldersOf(const BitVector& arrays, std::span<const DependencyList> dependencies, const ConversionSchedule& schedule)
{
    BitVector holders(dependencies.size());
    if (arrays.isEmpty())
        return holders;

    // The order goes from the held to the holder, so only a cycle takes a further round.
    bool hasGrown = true;
    while (hasGrown) {
        hasGrown = false;
        for (uint32_t index : schedule.order) {
            if (holders.get(index))
                continue;
            bool holdsOne = std::ranges::any_of(dependencies[index], [&](uint32_t dependency) {
                return arrays.get(dependency) || holders.get(dependency);
            });
            if (!holdsOne)
                continue;
            holders.set(index);
            hasGrown = true;
        }
    }
    return holders;
}

// The type of a container names the storage of the arrays it holds, at any depth.
static void convertAgain(const ContentsConversion& conversion, const ConversionSchedule& schedule, const BitVector& holders, TypeTable& types)
{
    holders.forEachSetBit([&](size_t index) {
        FlowContentsResult& result = conversion.resultOf(static_cast<uint32_t>(index));
        result = FlowContentsResult { .predictedStorage = result.predictedStorage };
    });
    for (unsigned level = 0; level < schedule.numberOfLevels(); ++level)
        convertLevel(conversion, schedule, level, &holders, types);
}

FlowContentsResults computeContentsResults(const FlowFamilyInputs& inputs, std::span<const FlowShapeResult> shapes, TypeTable& types)
{
    Vector<FlowContentsResult> results(inputs.objects().size());
    Vector<Container> containers = collectContainers(inputs);
    Vector<DependencyList> dependencies = computeDependencies(inputs, containers.span());
    ConversionSchedule schedule = scheduleConversions(inputs.loops(), dependencies.span());

    FlowTypeConverter converter(inputs.solution, FlowConversionTables { shapes, results.span(), { } });
    ContentsConversion conversion { inputs, converter, containers.span(), results.mutableSpan() };
    for (unsigned level = 0; level < schedule.numberOfLevels(); ++level)
        convertLevel(conversion, schedule, level, nullptr, types);
    Vector<TypeID> elementsBySite = uniteElementsWithinSites(conversion, types);
    BitVector holders = findHoldersOf(shareStorageWithinSites(conversion, elementsBySite.span(), types), dependencies.span(), schedule);
    if (!holders.isEmpty()) {
        convertAgain(conversion, schedule, holders, types);
        elementsBySite = uniteElementsWithinSites(conversion, types);
    }
    return { WTF::move(results), WTF::move(elementsBySite) };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
