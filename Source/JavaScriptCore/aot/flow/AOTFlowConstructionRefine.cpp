/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowConstruction.h"

#if ENABLE(AOT)

#include "AOTFlowObjectOperations.h"
#include "AOTFlowObjectState.h"
#include "AOTFlowPropertyState.h"
#include "AOTFlowPrototypes.h"
#include "AOTLayout.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <compare>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ConstructionRefinement {
    FlowSolver& solver;
    FlowConstructionGraph& graph;
    FlowConstructionState& state;
};

struct ConstructionMaker {
    FlowObjectID object;
    uint32_t node { 0 };

    friend auto operator<=>(const ConstructionMaker&, const ConstructionMaker&) = default;
};

struct ConstructionMakers {
    Vector<ConstructionMaker> makers;
    BitVector objectsWithUnknownMaker;
};

struct ConstructedNames {
    FlowConstructionNames stored;
    FlowConstructionNames readEarly;
};

struct OwnFieldOrder {
    FieldOrder order;
    bool isComplete { false };
};

struct ClassFieldOrder {
    const FieldOrder* order { nullptr };
    AllocationClassID allocationClass;
};

struct CountedFieldOrder {
    const FieldOrder* order { nullptr };
    size_t numberOfSites { 0 };
};

} // namespace

FlowConstructionState createFlowConstructionState(FlowSolver& solver)
{
    const Program& program = solver.program();
    BuildContext& build = solver.build();

    FlowConstructionState state;
    state.functions.grow(program.functions.size());
    Vector<uint32_t> functions = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.at(FunctionID(index));
        if (!function.hasBody())
            return false;
        return function.kind == FunctionKind::Initializer || !constructorRootsOf(program, function).isEmpty();
    });
    forEachIndexInParallel(build.parallelLoops(), functions.size(), [&](unsigned index) {
        state.functions[functions[index]] = buildFunctionConstruction(program, build, FunctionID(functions[index]));
    });
    return state;
}

const FlowObjectConstruction* findObjectConstruction(const FlowConstructionState& state, FlowObjectID object)
{
    auto found = state.objectIndexes.find(object);
    if (found == state.objectIndexes.end())
        return nullptr;
    return &state.objects[found->value];
}

static FlowFunctionConstruction* findFunctionConstruction(const FlowConstructionState& state, FunctionID function)
{
    return function.index() < state.functions.size() ? state.functions[function.index()].get() : nullptr;
}

static FlowReadOfCreatedObject* findReadOfCreatedObject(const FlowConstructionState& state, FunctionID function, ValueID read)
{
    FlowFunctionConstruction* construction = findFunctionConstruction(state, function);
    return construction ? construction->findReadOfCreatedObject(read) : nullptr;
}

FlowLookupStart lookupStartOf(const FlowConstructionState& state, FunctionID function, ValueID read)
{
    const FlowReadOfCreatedObject* found = findReadOfCreatedObject(state, function, read);
    return found ? found->start : FlowLookupStart::Object;
}

FlowLookupStart lookupStartOf(FlowSolver& solver, FlowStatementSite site, ValueID read)
{
    FlowReadOfCreatedObject* found = findReadOfCreatedObject(solver.propertyState().constructions, solver.graph().at(site.instance).function, read);
    if (!found || found->start == FlowLookupStart::Object)
        return FlowLookupStart::Object;
    if (!found->sites.contains(site))
        found->sites.append(site);
    return found->start;
}

bool definesUnobservedValue(FlowSolver& solver, FlowStatementSite site, ValueID definition)
{
    FlowFunctionConstruction* construction = findFunctionConstruction(solver.propertyState().constructions, solver.graph().at(site.instance).function);
    FlowFieldDefinition* found = construction ? construction->findFieldDefinition(definition) : nullptr;
    if (!found || found->isObserved)
        return false;
    if (!found->sites.contains(site))
        found->sites.append(site);
    return true;
}

static void solveConstructions(const FlowGraph& flowGraph, FlowConstructionGraph& graph)
{
    StronglyConnectedComponents components = computeStronglyConnectedComponents(graph.nodes.size(), [&](unsigned node) -> const Vector<uint32_t>& {
        return graph.nodes[node].callees;
    });

    Vector<uint32_t> calleesFirst(graph.nodes.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    std::ranges::sort(calleesFirst, { }, [&](uint32_t node) {
        return std::pair(components.componentOf[node], node);
    });

    for (uint32_t index : calleesFirst) {
        FlowConstructionNode& node = graph.nodes[index];
        if (components.isInCycle.get(index)) {
            node.summary = nullptr;
            node.solved.leakWitness = flowGraph.witnessOf(node.instance, node.root.definition);
        }
        solveStoredNames(graph, index);
        computeInitialOrder(graph, index);
    }
}

static bool isAllocatedByConstructor(const FlowObjectTable& objects, const FlowRecords& records, FlowObjectID object)
{
    return objects.kindOf(object) == FlowObjectKind::Allocation && records.eventsOf(object).contains(FlowObjectEvent::AllocatedByConstructor);
}

static bool mayHaveOtherNewTarget(const FlowObjectTable& objects, const FlowRecords& records, FunctionID constructor)
{
    return std::ranges::any_of(objects.closuresOf(constructor), [&](FlowObjectID closure) {
        return records.eventsOf(closure).contains(FlowObjectEvent::OtherNewTarget);
    });
}

static ConstructionMakers collectMakers(const ConstructionRefinement& refinement)
{
    const Program& program = refinement.solver.program();
    const FlowGraph& flowGraph = refinement.solver.graph();
    const FlowObjectTable& objects = refinement.solver.objects();
    const FlowRecords& records = refinement.solver.records();

    ConstructionMakers result;
    for (uint32_t index : refinement.graph.constructorNodes) {
        const FlowConstructionNode& node = refinement.graph.nodes[index];
        FlowNodeID root = flowGraph.nodeOf(node.instance, node.root.definition);
        if (!root)
            continue;
        FunctionID function = flowGraph.at(node.instance).function;
        bool constructsOtherLayouts = mayHaveOtherNewTarget(objects, records, function);
        flowGraph.valueOf(root).objects.forEach([&](FlowObjectID object) {
            if (!isAllocatedByConstructor(objects, records, object))
                return;
            // Reflect.construct(Base, [], Derived) creates an object of Derived that the constructor of Derived never stores to.
            if (program.at(objects.at(object).layout).constructor == function)
                result.makers.append({ object, index });
            else if (constructsOtherLayouts)
                result.objectsWithUnknownMaker.set(object.index());
        });
    }
    std::ranges::sort(result.makers);
    return result;
}

static FlowConstructionName nameOfField(const Program& program, LayoutID layout, uint32_t index)
{
    const Field& field = program.at(layout).fields[index];
    if (field.key)
        return { field.key, { } };
    return { { }, declarationOf(program, { layout, index }) };
}

static std::optional<uint32_t> findDeclaredField(const FlowGraph& flowGraph, FlowObjectID object, FlowConstructionName name)
{
    FlowFieldLocation location = name.key
        ? findFieldLocation(flowGraph, object, name.key)
        : findFieldLocation(flowGraph, object, name.field.layout, name.field.field);
    if (location.lookup != FlowFieldLookup::Found || flowGraph.kindOf(location.node) != FlowNodeKind::DeclaredPart)
        return std::nullopt;
    return location.fieldIndex;
}

static BitVector declaredFieldsAmong(const FlowGraph& flowGraph, FlowObjectID object, const FlowConstructionNames& names)
{
    BitVector fields(flowGraph.objects().at(object).numberOfDeclaredParts);
    for (FlowConstructionName name : names) {
        if (std::optional<uint32_t> field = findDeclaredField(flowGraph, object, name))
            fields.set(*field);
    }
    return fields;
}

static size_t parentOrderLength(const FlowConstructionGraph& graph, const FlowConstructionNode& node)
{
    if (node.root.kind != FlowConstructionRootKind::SuperConstruct || !node.summary)
        return 0;
    std::span<const FlowConstructionEvent> events = node.summary->events.span();
    auto rootEvent = std::ranges::find_if(events, [&](const FlowConstructionEvent& event) {
        return event.origin == node.root.definition && event.operand == node.root.definition;
    });
    if (rootEvent == events.end())
        return 0;
    const FlowResolvedConstructionEvent& resolved = node.resolvedEvents[rootEvent - events.begin()];
    if (resolved.callees.isEmpty())
        return 0;
    return std::min(graph.nodes[resolved.callees[0]].solved.initialOrder.size(), node.solved.initialOrder.size());
}

static OwnFieldOrder ownOrderOf(const ConstructionRefinement& refinement, uint32_t maker, FlowObjectID object)
{
    const FlowGraph& flowGraph = refinement.solver.graph();
    const FlowConstructionNode& node = refinement.graph.nodes[maker];

    OwnFieldOrder result;
    for (FlowConstructionName name : node.solved.initialOrder.span().subspan(parentOrderLength(refinement.graph, node))) {
        std::optional<uint32_t> field = findDeclaredField(flowGraph, object, name);
        if (!field)
            return result;
        result.order.fields.append(*field);
    }
    result.isComplete = true;
    return result;
}

static ConstructedNames meetNames(const FlowConstructionGraph& graph, std::span<const ConstructionMaker> makers)
{
    ConstructedNames names;
    if (makers.empty())
        return names;

    names.stored = graph.nodes[makers[0].node].solved.storedBeforeLeak;
    for (const ConstructionMaker& maker : makers) {
        const FlowSolvedConstruction& solved = graph.nodes[maker.node].solved;
        names.stored.intersect(solved.storedBeforeLeak);
        names.readEarly.unite(solved.readBeforeStore);
    }
    return names;
}

static void appendObjectConstruction(const ConstructionRefinement& refinement, FlowObjectID object, std::span<const ConstructionMaker> makers, const ConstructedNames& names)
{
    const FlowGraph& flowGraph = refinement.solver.graph();

    FlowObjectConstruction construction;
    construction.leak = ThisLeak::DoesNotLeak;
    construction.fieldCreation = FieldCreation::OnlyAtConstruction;
    construction.fieldsSetAtCreation = declaredFieldsAmong(flowGraph, object, names.stored);
    construction.fieldsReadBeforeStore = declaredFieldsAmong(flowGraph, object, names.readEarly);
    for (const ConstructionMaker& maker : makers) {
        const FlowSolvedConstruction& solved = refinement.graph.nodes[maker.node].solved;
        OwnFieldOrder own = ownOrderOf(refinement, maker.node, object);
        if (solved.leak == ThisLeak::Unknown)
            construction.leak = ThisLeak::Unknown;
        keepLowerWitness(construction.leakWitness, solved.leakWitness);
        if (solved.leak == ThisLeak::Unknown || solved.stores == FlowConstructionStores::Unknown || !own.isComplete)
            construction.fieldCreation = FieldCreation::Unknown;
        if (!construction.orders.contains(own.order))
            construction.orders.append(WTF::move(own.order));
    }

    refinement.state.objectIndexes.add(object, static_cast<uint32_t>(refinement.state.objects.size()));
    refinement.state.objects.append(WTF::move(construction));
}

enum class LocationWhenMissing : uint8_t { HoldsUndefined, IsPassedByLookups };

static void withdrawUnlessStored(const ConstructionRefinement& refinement, const FlowConstructionNames& stored, FlowNodeID location, FlowConstructionName name, LocationWhenMissing whenMissing)
{
    BitVector& withdrawnFields = refinement.state.withdrawnFields;
    if (withdrawnFields.quickGet(location.index()) || stored.contains(name))
        return;
    OptionSet<FlowLocationEvent> events = refinement.solver.records().recordOf(location).events;
    if (events.contains(FlowLocationEvent::InitializedAtAllocation) && !events.contains(FlowLocationEvent::MissingAtAllocation))
        return;

    withdrawnFields.quickSet(location.index());
    // Whoever gets the object at the leak or at the exit may read the field before any store.
    refinement.solver.noteLocationEvent(location, { FlowLocationEvent::MissingAtAllocation, FlowLocationEvent::ReadBeforeInitialization });
    if (whenMissing == LocationWhenMissing::HoldsUndefined)
        refinement.solver.joinPrimitives(location, Primitive::Undefined);
}

static void withdrawFields(const ConstructionRefinement& refinement, FlowObjectID object, const FlowConstructionNames& stored)
{
    const Program& program = refinement.solver.program();
    const FlowGraph& flowGraph = refinement.solver.graph();
    LayoutID layout = flowGraph.objects().at(object).layout;
    uint32_t numberOfDeclaredParts = flowGraph.objects().at(object).numberOfDeclaredParts;

    for (uint32_t index = 0; index < numberOfDeclaredParts; ++index)
        withdrawUnlessStored(refinement, stored, flowGraph.field(object, index), nameOfField(program, layout, index), LocationWhenMissing::HoldsUndefined);

    for (size_t index = 0; index < flowGraph.propertiesOf(object).size(); ++index) {
        FlowProperty property = flowGraph.propertiesOf(object)[index];
        if (!property.value || flowGraph.kindOf(property.value) != FlowNodeKind::Property)
            continue;
        bool isField = findField(program, layout, property.key, BaseChain::Predicted).has_value();
        withdrawUnlessStored(refinement, stored, property.value, { property.key, { } }, isField ? LocationWhenMissing::HoldsUndefined : LocationWhenMissing::IsPassedByLookups);
    }
}

static void meetAndWithdraw(const ConstructionRefinement& refinement, const ConstructionMakers& collected)
{
    const FlowObjectTable& objects = refinement.solver.objects();
    const FlowRecords& records = refinement.solver.records();
    std::span<const ConstructionMaker> makers = collected.makers.span();

    refinement.state.objects.clear();
    refinement.state.objectIndexes.clear();
    refinement.state.withdrawnFields.ensureSize(refinement.solver.graph().numberOfNodes());

    size_t cursor = 0;
    for (uint32_t index = 1; index < objects.size(); ++index) {
        FlowObjectID object(index);
        if (!isAllocatedByConstructor(objects, records, object))
            continue;
        size_t first = cursor;
        while (cursor < makers.size() && makers[cursor].object == object)
            ++cursor;
        std::span<const ConstructionMaker> makersOfObject = makers.subspan(first, cursor - first);
        if (collected.objectsWithUnknownMaker.get(index))
            makersOfObject = { };

        ConstructedNames names = meetNames(refinement.graph, makersOfObject);
        if (!makersOfObject.empty())
            appendObjectConstruction(refinement, object, makersOfObject, names);
        withdrawFields(refinement, object, names.stored);
    }
    RELEASE_ASSERT(cursor == makers.size());
}

static FlowProgress setLookupStart(FlowSolver& solver, FlowReadOfCreatedObject& read, FlowLookupStart start)
{
    if (read.start == start)
        return FlowProgress::Finished;
    read.start = start;
    for (FlowStatementSite site : read.sites)
        solver.revisit(site);
    if (start == FlowLookupStart::Object)
        read.sites.clear();
    return FlowProgress::QueuedWork;
}

// The other instances of a function were solved with the read as a lookup from the prototype, so a read that moves to the object asks for another refinement.
static FlowProgress decideLookupStarts(const ConstructionRefinement& refinement)
{
    const Program& program = refinement.solver.program();
    FlowProgress progress = FlowProgress::Finished;
    auto decide = [&](FlowReadOfCreatedObject& read, FlowLookupStart start) {
        if (setLookupStart(refinement.solver, read, start) == FlowProgress::QueuedWork)
            progress = FlowProgress::QueuedWork;
    };

    for (uint32_t index : refinement.graph.constructorNodes) {
        const FlowConstructionNode& node = refinement.graph.nodes[index];
        const FlowConstructionSummary* summary = node.function->find(node.root.definition);
        if (!summary)
            continue;
        const Function& function = program.at(node.function->function);
        for (size_t position = 0; position < summary->events.size(); ++position) {
            const FlowConstructionEvent& event = summary->events[position];
            if (!isReadOfCreatedObject(function, node.root, event))
                continue;
            if (!node.summary || !node.resolvedEvents[position].startsAtPrototype)
                decide(*node.function->findReadOfCreatedObject(event.origin), FlowLookupStart::Object);
        }
    }
    for (uint32_t index : refinement.graph.constructorNodes) {
        for (FlowReadOfCreatedObject& read : refinement.graph.nodes[index].function->readsOfCreatedObject) {
            if (read.start == FlowLookupStart::Undecided)
                decide(read, FlowLookupStart::Prototype);
        }
    }
    return progress;
}

static const FlowConstructionNode* findTrackedNode(const FlowConstructionGraph& graph, FlowInstanceID instance, ValueID root)
{
    auto found = graph.nodeIndexes.find(std::tuple { instance, root });
    if (found == graph.nodeIndexes.end() || !graph.nodes[found->value].summary)
        return nullptr;
    return &graph.nodes[found->value];
}

// super() in an arrow function runs the initializer too, on an object that no construction follows.
static const FlowConstructionNode* findConstructorRunning(const ConstructionRefinement& refinement, const FlowCallLink& link, const FlowConstructionNode& initializer)
{
    const Program& program = refinement.solver.program();
    const Function& caller = program.at(link.caller);
    ValueID initialized = caller.at(link.site).argumentsOperands()[*program.at(link.callee).thisParameterIndex()];
    const FlowConstructionNode* constructor = findTrackedNode(refinement.graph, link.callerInstance, sameObjectThroughCasts(caller, initialized));
    uint32_t initializerIndex = static_cast<uint32_t>(&initializer - refinement.graph.nodes.span().data());
    if (!constructor || !std::ranges::binary_search(constructor->callees, initializerIndex))
        return nullptr;
    return constructor;
}

static void observe(FlowSolver& solver, FlowFieldDefinition& definition)
{
    definition.isObserved = true;
    for (FlowStatementSite site : definition.sites)
        solver.revisit(site);
    definition.sites.clear();
}

static void observeFieldDefinitions(const ConstructionRefinement& refinement)
{
    const Program& program = refinement.solver.program();
    for (const FlowCallLink& link : refinement.solver.records().callLinks()) {
        FlowFunctionConstruction* construction = findFunctionConstruction(refinement.state, link.callee);
        if (!construction || construction->fieldDefinitions.isEmpty())
            continue;

        const Function& function = program.at(link.callee);
        const FlowConstructionNode* initializer = findTrackedNode(refinement.graph, link.calleeInstance, function.at(entryBlock).parameters[*function.thisParameterIndex()]);
        const FlowConstructionNode* constructor = initializer ? findConstructorRunning(refinement, link, *initializer) : nullptr;
        for (FlowFieldDefinition& definition : construction->fieldDefinitions) {
            if (definition.isObserved)
                continue;
            FlowConstructionName name = constructionNameOf(program, function.at(definition.definition));
            if (!constructor || constructor->solved.observedDefinitions.contains(name))
                observe(refinement.solver, definition);
        }
    }
}

static void takeSolvedConstructors(const ConstructionRefinement& refinement)
{
    // The length reads the order of the parent's node, which is a constructor node too and is emptied below.
    Vector<size_t> parentOrderLengths = refinement.graph.constructorNodes.map([&](uint32_t index) {
        return parentOrderLength(refinement.graph, refinement.graph.nodes[index]);
    });

    refinement.state.solvedConstructors.clear();
    for (size_t position = 0; position < parentOrderLengths.size(); ++position) {
        FlowConstructionNode& node = refinement.graph.nodes[refinement.graph.constructorNodes[position]];
        refinement.state.solvedConstructors.append({ node.instance, node.root, WTF::move(node.solved), parentOrderLengths[position] });
    }
}

static BitVector findFunctionsSeeingOnlyFinishedObjects(const ConstructionRefinement& refinement)
{
    const Program& program = refinement.solver.program();
    const FlowGraph& flowGraph = refinement.solver.graph();
    const Vector<FlowConstructionNode>& nodes = refinement.graph.nodes;

    BitVector isReached(nodes.size());
    Vector<uint32_t> nodesToVisit;
    auto reach = [&](uint32_t node) {
        if (isReached.add(node))
            nodesToVisit.append(node);
    };
    for (uint32_t node : refinement.graph.constructorNodes) {
        if (program.at(flowGraph.at(nodes[node].instance).function).constructedClass)
            reach(node);
    }

    BitVector holdsUnfinishedObject(program.functions.size());
    while (!nodesToVisit.isEmpty()) {
        const FlowConstructionNode& node = nodes[nodesToVisit.takeLast()];
        holdsUnfinishedObject.quickSet(flowGraph.at(node.instance).function.index());
        for (uint32_t callee : node.callees)
            reach(callee);
    }

    BitVector functions(program.functions.size());
    for (size_t index = 0; index < program.functions.size(); ++index)
        functions.quickSet(index, !holdsUnfinishedObject.quickGet(index));
    return functions;
}

FlowProgress refineConstructions(FlowSolver& solver)
{
    FlowProgress prototypes = refinePrototypeUses(solver);

    FlowConstructionLinks links(solver.graph(), solver.records());
    FlowConstructionGraph graph = buildConstructionGraph(solver, links);
    solveConstructions(solver.graph(), graph);

    ConstructionRefinement refinement { solver, graph, solver.propertyState().constructions };
    observeFieldDefinitions(refinement);
    // What a read that has just got its start finds is not linked: a method that the constructor calls on the object has stored nothing so far, and a field that is withdrawn stays withdrawn.
    if (decideLookupStarts(refinement) == FlowProgress::QueuedWork)
        return FlowProgress::QueuedWork;

    meetAndWithdraw(refinement, collectMakers(refinement));
    refinement.state.functionsSeeingOnlyFinishedObjects = findFunctionsSeeingOnlyFinishedObjects(refinement);
    takeSolvedConstructors(refinement);

    if (prototypes == FlowProgress::QueuedWork || solver.hasQueuedWork())
        return FlowProgress::QueuedWork;
    return FlowProgress::Finished;
}

static void meetObjectConstruction(FlowConstructionResult& result, const FlowObjectConstruction& construction, bool isFirstObject)
{
    if (construction.leak == ThisLeak::Unknown)
        result.thisLeak = ThisLeak::Unknown;
    if (construction.fieldCreation == FieldCreation::Unknown)
        result.fieldCreation = FieldCreation::Unknown;
    keepLowerWitness(result.leakWitness, construction.leakWitness);
    if (isFirstObject)
        result.fieldsSetAtCreation = construction.fieldsSetAtCreation;
    else
        result.fieldsSetAtCreation.filter(construction.fieldsSetAtCreation);
}

static std::strong_ordering compareFields(const FieldOrder& a, const FieldOrder& b)
{
    return std::lexicographical_compare_three_way(a.fields.begin(), a.fields.end(), b.fields.begin(), b.fields.end());
}

static Vector<FieldOrder> sortBySiteCount(const FlowObjectTable& objects, Vector<ClassFieldOrder>&& orders)
{
    std::ranges::sort(orders, [](const ClassFieldOrder& a, const ClassFieldOrder& b) {
        std::strong_ordering fields = compareFields(*a.order, *b.order);
        return fields != 0 ? fields < 0 : a.allocationClass < b.allocationClass;
    });

    Vector<CountedFieldOrder> countedOrders;
    for (size_t index = 0; index < orders.size(); ++index) {
        const ClassFieldOrder& entry = orders[index];
        bool isNewOrder = !index || *orders[index - 1].order != *entry.order;
        if (isNewOrder)
            countedOrders.append({ entry.order, 0 });
        if (isNewOrder || orders[index - 1].allocationClass != entry.allocationClass)
            countedOrders.last().numberOfSites += objects.at(entry.allocationClass).sites.size();
    }
    std::ranges::stable_sort(countedOrders, std::ranges::greater { }, &CountedFieldOrder::numberOfSites);

    return countedOrders.map([](const CountedFieldOrder& counted) {
        return *counted.order;
    });
}

static std::optional<uint32_t> findFieldOfLayout(const Program& program, LayoutID layout, FlowConstructionName name)
{
    if (name.key)
        return program.at(layout).findField(name.key);
    if (name.field.layout == layout)
        return name.field.field;
    return std::nullopt;
}

// The constructor of a class that is allocated only through subclasses still creates its own fields, in front of theirs.
static Vector<FieldOrder> ordersCreatedForSubclasses(const FlowSolution& solution, LayoutID layout)
{
    const Program& program = solution.program;
    FunctionID constructor = program.at(layout).constructor;
    Vector<FieldOrder> orders;
    if (!constructor)
        return orders;

    for (const FlowSolvedConstructionEntry& entry : solution.propertyState.constructions.solvedConstructors) {
        if (solution.graph.at(entry.instance).function != constructor)
            continue;

        FieldOrder order;
        for (FlowConstructionName name : entry.solved.initialOrder.span().subspan(entry.parentOrderLength)) {
            std::optional<uint32_t> field = findFieldOfLayout(program, layout, name);
            if (!field)
                break;
            order.fields.append(*field);
        }
        if (!orders.contains(order))
            orders.append(WTF::move(order));
    }
    std::ranges::sort(orders, [](const FieldOrder& a, const FieldOrder& b) {
        return compareFields(a, b) < 0;
    });
    return orders;
}

FlowConstructionResult constructionOf(const FlowSolution& solution, LayoutID layout)
{
    const FlowConstructionState& state = solution.propertyState.constructions;

    FlowConstructionResult result;
    result.thisLeak = ThisLeak::DoesNotLeak;
    result.fieldCreation = FieldCreation::OnlyAtConstruction;
    Vector<ClassFieldOrder> orders;
    bool hasAllocatedObject = false;
    bool hasUnknownConstruction = false;
    // The constructor of a layout constructs the objects of the layouts derived from it, and a derived layout has no constructor of its own.
    Vector<AllocationClassID, 8> allocationClasses(solution.objects.allocationClassesOf(layout));
    for (LayoutID derived : solution.objects.derivedLayoutsOf(layout))
        allocationClasses.append(solution.objects.allocationClassesOf(derived));
    for (AllocationClassID allocationClass : allocationClasses) {
        forEachObjectOfClass(solution.objects, allocationClass, [&](FlowObjectID object) {
            OptionSet<FlowObjectEvent> events = solution.records.eventsOf(object);
            if (!events.contains(FlowObjectEvent::Allocated))
                return;
            const FlowObjectConstruction* construction = events.contains(FlowObjectEvent::AllocatedByConstructor) ? findObjectConstruction(state, object) : nullptr;
            if (!construction) {
                hasUnknownConstruction = true;
                return;
            }
            meetObjectConstruction(result, *construction, !hasAllocatedObject);
            hasAllocatedObject = true;
            for (const FieldOrder& order : construction->orders)
                orders.append({ &order, allocationClass });
        });
    }
    if (hasUnknownConstruction)
        return { };
    if (!hasAllocatedObject) {
        FlowConstructionResult unallocated;
        unallocated.constructorOrders = ordersCreatedForSubclasses(solution, layout);
        return unallocated;
    }

    if (!solution.program.at(layout).isDerived())
        result.constructorOrders = sortBySiteCount(solution.objects, WTF::move(orders));
    return result;
}

bool seesOnlyFinishedObjects(const FlowSolution& solution, FunctionID function)
{
    return solution.propertyState.constructions.functionsSeeingOnlyFinishedObjects.get(function.index());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
