/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowConstruction.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include "AOTFlowBuiltinCalls.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTFlowPropertyState.h"
#include "AOTFlowPrototypes.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

auto FlowConstructionLinks::groupEntries(uint32_t numberOfGroups, uint32_t numberOfEntries, NOESCAPE const ScopedLambda<uint32_t(uint32_t entry)>& groupOf) -> Groups
{
    Groups groups;
    groups.firstEntry.fill(0, numberOfGroups + 1);
    for (uint32_t entry = 0; entry < numberOfEntries; ++entry) {
        uint32_t group = groupOf(entry);
        if (group < numberOfGroups)
            ++groups.firstEntry[group + 1];
    }
    for (uint32_t group = 0; group < numberOfGroups; ++group)
        groups.firstEntry[group + 1] += groups.firstEntry[group];

    groups.entries.grow(groups.firstEntry.last());
    Vector<uint32_t> nextEntry = groups.firstEntry;
    for (uint32_t entry = 0; entry < numberOfEntries; ++entry) {
        uint32_t group = groupOf(entry);
        if (group < numberOfGroups)
            groups.entries[nextEntry[group]++] = entry;
    }
    return groups;
}

FlowConstructionLinks::FlowConstructionLinks(const FlowGraph& graph, const FlowRecords& records)
    : m_records(records)
{
    uint32_t numberOfInstances = graph.numberOfInstances();
    std::span<const FlowCallLink> links = records.callLinks();
    std::span<const FlowOperandFlow> operandFlows = records.operandFlows();
    std::span<const FlowBuiltinCallLink> builtinLinks = records.builtinCallLinks();
    std::span<const FlowUnknownCallee> unknownCallees = records.unknownCallees();

    m_linksByCaller = groupEntries(numberOfInstances, static_cast<uint32_t>(links.size()), [&](uint32_t entry) {
        return links[entry].callerInstance.index();
    });
    m_operandFlowsByLink = groupEntries(static_cast<uint32_t>(links.size()), static_cast<uint32_t>(operandFlows.size()), [&](uint32_t entry) {
        return operandFlows[entry].link.index();
    });
    m_builtinLinksByCaller = groupEntries(numberOfInstances, static_cast<uint32_t>(builtinLinks.size()), [&](uint32_t entry) {
        return builtinLinks[entry].callerInstance.index();
    });
    m_unknownCalleesByCaller = groupEntries(numberOfInstances, static_cast<uint32_t>(unknownCallees.size()), [&](uint32_t entry) {
        return unknownCallees[entry].callerInstance.index();
    });
}

bool FlowConstructionLinks::hasUnknownCallee(FlowInstanceID caller, ValueID site) const
{
    return std::ranges::any_of(m_unknownCalleesByCaller.of(caller.index()), [&](uint32_t index) {
        return m_records.unknownCallees()[index].site == site;
    });
}

static std::optional<BuiltinOperand> builtinOperandOf(const Value& operation, const FlowConstructionEvent& event, BuiltinCallMode mode)
{
    switch (mode) {
    case BuiltinCallMode::Get:
    case BuiltinCallMode::Set:
        return BuiltinOperand::receiver();
    case BuiltinCallMode::Call:
    case BuiltinCallMode::Construct:
        break;
    }
    if (std::optional<uint32_t> argument = operation.variadicOperandIndex(event.operandIndex)) {
        if (!VariadicOperandPosition::arguments(operation.opcode) || *argument > BuiltinOperand::maxArgumentIndex)
            return std::nullopt;
        return BuiltinOperand::argument(*argument);
    }
    if (OperandPosition::thisValue(operation.opcode) == event.operandIndex)
        return BuiltinOperand::receiver();
    return std::nullopt;
}

namespace {

// A callback that takes the operand has a link with the flow. resolveBuiltinLinks leaks where a link shows none.
constexpr OperandUses confinedUses {
    OperandUse::Inspects,
    OperandUse::ReadsContents,
    OperandUse::WritesContents,
    OperandUse::MovesElements,
    OperandUse::PassesToUserCode,
};

constexpr OperandUses confinedWritingUses { OperandUse::WritesContents, OperandUse::MovesElements };

enum class BuiltinEffectOnObject : uint8_t {
    MayLeak,
    HeldByResult,
    Confined,
    NoBuiltin,
};

struct BuiltinLinksOfEvent {
    BuiltinEffectOnObject effect { BuiltinEffectOnObject::MayLeak };
    bool mayWriteObject { true };
};

struct LinkedFunctions {
    bool hasLinkWithoutObject { false };
    bool hasDirectCallee { false };
    Vector<FunctionID, 2> takingObject;
};

class ConstructionGraphBuilder {
public:
    ConstructionGraphBuilder(FlowSolver& solver, const FlowConstructionLinks& links)
        : m_solver(solver)
        , m_links(links)
        , m_program(solver.program())
        , m_flowGraph(solver.graph())
        , m_state(solver.propertyState().constructions)
    {
    }

    FlowConstructionGraph build()
    {
        addConstructorNodes();
        uint32_t next = 0;
        while (next < m_graph.nodes.size()) {
            buildMissingFunctions(next);
            for (uint32_t end = m_graph.nodes.size(); next < end; ++next)
                resolveNode(next);
        }
        for (FlowConstructionNode& node : m_graph.nodes)
            collectCallees(node);
        return WTF::move(m_graph);
    }

private:
    uint32_t addNode(FlowInstanceID instance, FlowConstructionRoot root)
    {
        return m_graph.nodeIndexes.ensure(std::tuple { instance, root.definition }, [&] {
            uint32_t index = m_graph.nodes.size();
            m_graph.nodes.append(FlowConstructionNode { instance, root });
            return index;
        }).iterator->value;
    }

    void addConstructorNodes()
    {
        for (uint32_t index = 0; index < m_flowGraph.numberOfInstances(); ++index) {
            FlowInstanceID instance(index);
            if (m_flowGraph.isHost(instance))
                continue;
            for (const FlowConstructionRoot& root : constructorRootsOf(m_program, m_program.at(m_flowGraph.at(instance).function)))
                m_graph.constructorNodes.append(addNode(instance, root));
        }
    }

    void buildMissingFunctions(uint32_t firstNode)
    {
        Vector<FunctionID> functions;
        for (uint32_t index = firstNode; index < m_graph.nodes.size(); ++index) {
            FunctionID function = m_flowGraph.at(m_graph.nodes[index].instance).function;
            if (!m_state.functions[function.index()])
                functions.append(function);
        }
        SortedSet<FunctionID> missing(functions.span());
        forEachIndexInParallel(m_solver.build().parallelLoops(), static_cast<unsigned>(missing.size()), [&](unsigned index) {
            m_state.functions[missing[index].index()] = buildFunctionConstruction(m_program, m_solver.build(), missing[index]);
        });
    }

    void resolveNode(uint32_t index)
    {
        FlowInstanceID instance = m_graph.nodes[index].instance;
        FlowConstructionRoot root = m_graph.nodes[index].root;
        const FlowInstance& record = m_flowGraph.at(instance);
        FlowFunctionConstruction* function = m_state.functions[record.function.index()].get();
        const FlowConstructionSummary* summary = function->find(root.definition);
        m_graph.nodes[index].function = function;
        if (!summary || summary->tracking == FlowConstructionTracking::Unknown || record.state == FlowInstanceState::PastWorkLimit) {
            m_graph.nodes[index].solved.leakWitness = m_flowGraph.witnessOf(instance, root.definition);
            return;
        }

        Vector<FlowResolvedConstructionEvent> resolvedEvents;
        resolvedEvents.reserveInitialCapacity(summary->events.size());
        for (const FlowConstructionEvent& event : summary->events)
            resolvedEvents.append(resolveEvent(instance, root, event));
        resolveHolders(summary->events, resolvedEvents);

        FlowConstructionNode& node = m_graph.nodes[index];
        node.summary = summary;
        node.resolvedEvents = WTF::move(resolvedEvents);
    }

    static void resolveHolders(std::span<const FlowConstructionEvent> events, Vector<FlowResolvedConstructionEvent>& resolvedEvents)
    {
        Vector<ValueID, 2> holders;
        for (size_t index = 0; index < events.size(); ++index) {
            if (resolvedEvents[index].resultHoldsObject)
                holders.append(events[index].origin);
        }
        for (size_t index = 0; index < events.size(); ++index) {
            if (!events[index].holder || holders.contains(events[index].holder))
                continue;
            resolvedEvents[index].holderIsEmpty = true;
            if (events[index].kind == FlowConstructionEventKind::Leak)
                resolvedEvents[index].doesNotLeak = true;
        }
    }

    FlowResolvedConstructionEvent resolveEvent(FlowInstanceID instance, const FlowConstructionRoot& root, const FlowConstructionEvent& event)
    {
        FlowResolvedConstructionEvent resolved;
        resolved.witness = m_flowGraph.witnessOf(instance, event.origin);
        switch (event.kind) {
        case FlowConstructionEventKind::Leak:
            return resolved;
        case FlowConstructionEventKind::Exit:
            resolved.doesNotLeak = true;
            return resolved;
        case FlowConstructionEventKind::Store:
        case FlowConstructionEventKind::Read:
        case FlowConstructionEventKind::Call:
            break;
        }

        if (root.kind == FlowConstructionRootKind::SuperConstruct && event.origin == root.definition && event.operand == root.definition)
            resolveParentConstruction(instance, event, resolved);
        else
            resolveObjectUse(instance, event, resolved);
        FunctionID function = m_flowGraph.at(instance).function;
        resolved.startsAtPrototype = isReadOfCreatedObject(m_program.at(function), root, event) && lookupStartOf(m_state, function, event.origin) != FlowLookupStart::Object;
        std::ranges::sort(resolved.callees);
        removeRepeatedElements(resolved.callees);
        return resolved;
    }

    void resolveParentConstruction(FlowInstanceID instance, const FlowConstructionEvent& event, FlowResolvedConstructionEvent& resolved)
    {
        bool leaks = m_links.hasUnknownCallee(instance, event.origin);
        m_links.forEachLinkAt(instance, event.origin, [&](FlowCallLinkID, const FlowCallLink& link) {
            if (link.kind != FlowCallLinkKind::Construct)
                return;
            Vector<FlowConstructionRoot, 2> roots = constructorRootsOf(m_program, m_program.at(link.callee));
            // super() binds whatever object the parent returns. One that existed before has leaked, and has its fields already.
            const FlowFunctionConstruction* parent = m_state.functions[link.callee.index()].get();
            if (roots.isEmpty() || !parent || !parent->returnsOnlyConstructedObject)
                leaks = true;
            for (const FlowConstructionRoot& root : roots)
                resolved.callees.append(addNode(link.calleeInstance, root));
        });
        bool hasBuiltinParent = false;
        m_links.forEachBuiltinLinkAt(instance, event.origin, [&](const FlowBuiltinCallLink&) {
            hasBuiltinParent = true;
            if (!resolved.callees.isEmpty())
                leaks = true;
        });
        resolved.doesNotLeak = !leaks;
        resolved.runsOneCallee = true;
        // The constructor of a built-in class defines own properties that no event lists.
        resolved.storesOnlyThroughCallees = !leaks && !hasBuiltinParent;
    }

    void resolveObjectUse(FlowInstanceID instance, const FlowConstructionEvent& event, FlowResolvedConstructionEvent& resolved)
    {
        FlowNodeID operandNode = m_flowGraph.nodeOf(instance, event.operand);
        bool leaks = !operandNode || m_links.hasUnknownCallee(instance, event.origin);
        LinkedFunctions functions;
        m_links.forEachLinkAt(instance, event.origin, [&](FlowCallLinkID id, const FlowCallLink& link) {
            bool receivesObject = false;
            m_links.forEachOperandFlowOf(id, [&](const FlowOperandFlow& flow) {
                if (flow.from != operandNode)
                    return;
                receivesObject = true;
                if (std::optional<uint32_t> callee = addParameterNode(flow.parameter))
                    resolved.callees.append(*callee);
                else
                    leaks = true;
            });
            if (!receivesObject) {
                functions.hasLinkWithoutObject = true;
                return;
            }
            functions.takingObject.append(link.callee);
            if (link.times != BuiltinCallbackTimes::Once)
                leaks = true;
            if (link.kind != FlowCallLinkKind::BuiltinCallback)
                functions.hasDirectCallee = true;
        });

        BuiltinLinksOfEvent builtinLinks = resolveBuiltinLinks(instance, event, functions);
        BuiltinEffectOnObject builtinEffect = builtinLinks.effect;
        if (builtinEffect == BuiltinEffectOnObject::MayLeak)
            leaks = true;
        resolved.resultHoldsObject = builtinEffect == BuiltinEffectOnObject::HeldByResult;

        bool hasCallees = !resolved.callees.isEmpty();
        bool builtinRunsCallees = hasCallees && !functions.hasDirectCallee;
        bool otherCodeMayRun = functions.hasLinkWithoutObject || (builtinEffect != BuiltinEffectOnObject::NoBuiltin && !builtinRunsCallees);
        resolved.runsOneCallee = !otherCodeMayRun && !mayBeStoppedAboveObject(instance, event, operandNode);
        resolved.doesNotLeak = !leaks;
        resolved.storesOnlyThroughCallees = !leaks && !builtinLinks.mayWriteObject;
    }

    bool inheritsPropertyStoppingStores(FlowObjectID object, PropertyKeyID name) const
    {
        FlowWalkInputs inputs = FlowWalkInputs::of(m_solver);
        FlowObjectSet visited;
        Vector<FlowHolder, 4> pending { holderOf(m_solver.objects(), object) };
        while (!pending.isEmpty()) {
            FlowPrototypeSet prototypes = prototypesOf(inputs, pending.takeLast(), FlowStatementSite());
            if (!prototypes.isNeverUnknown)
                return true;
            for (const FlowHolder& holder : prototypes.holders) {
                if (holder.object && !visited.add(holder.object))
                    continue;
                if (stopsStoresFromBelow(inputs, holder, name))
                    return true;
                pending.append(holder);
            }
        }
        return false;
    }

    // An accessor or a read-only property that the object inherits keeps a store from creating a property of the object: the store runs the setter, throws, or does nothing.
    // The links of the store show a setter and nothing else. Each refinement looks at the chain again, so it sees what is defined there later.
    bool mayBeStoppedAboveObject(FlowInstanceID instance, const FlowConstructionEvent& event, FlowNodeID operandNode) const
    {
        if (event.kind != FlowConstructionEventKind::Store || !event.isExact || !operandNode)
            return false;
        const Value& store = m_program.at(m_flowGraph.at(instance).function).at(event.origin);
        if (store.opcode != Opcode::PropSet)
            return false;
        PropertyKeyID name = store.key();
        if (classOf(m_program, name) == FlowKeyClass::PrivateName)
            return false;

        bool mayBeStopped = false;
        m_flowGraph.valueOf(operandNode).objects.forEach([&](FlowObjectID object) {
            mayBeStopped = mayBeStopped || inheritsPropertyStoppingStores(object, name);
        });
        return mayBeStopped;
    }

    BuiltinLinksOfEvent resolveBuiltinLinks(FlowInstanceID instance, const FlowConstructionEvent& event, const LinkedFunctions& functions) const
    {
        const Value& operation = m_program.at(m_flowGraph.at(instance).function).at(event.origin);
        bool operationCallsBuiltin = false;
        m_links.forEachBuiltinLinkAt(instance, event.origin, [&](const FlowBuiltinCallLink& link) {
            operationCallsBuiltin |= isCalledByOperation(link, event);
        });

        BuiltinID builtin;
        bool hasOtherBuiltin = false;
        bool hasNestedBuiltin = false;
        bool isHeldByResult = false;
        OperandUses uses;
        m_links.forEachBuiltinLinkAt(instance, event.origin, [&](const FlowBuiltinCallLink& link) {
            if (functions.takingObject.contains(enteredBodyOf(link.overload)))
                return;
            if (operationCallsBuiltin && !isCalledByOperation(link, event)) {
                hasNestedBuiltin = true;
                return;
            }
            if (builtin && builtin != link.builtin)
                hasOtherBuiltin = true;
            builtin = link.builtin;
            OperandUses declaredUses = declaredUsesOf(link, operation, event);
            if (declaredUses.contains(OperandUse::Retains) && retainsOnlyInNewResult(link, operation, event)) {
                declaredUses.remove(OperandUse::Retains);
                isHeldByResult = true;
            }
            uses.add(declaredUses);
        });

        if (!builtin)
            return { BuiltinEffectOnObject::NoBuiltin, false };
        if (hasOtherBuiltin || !uses.containsOnly(confinedUses) || mayCallBoundFunction(instance, operation))
            return { };
        if (uses.contains(OperandUse::PassesToUserCode) && (functions.hasLinkWithoutObject || hasNestedBuiltin))
            return { };
        return { isHeldByResult ? BuiltinEffectOnObject::HeldByResult : BuiltinEffectOnObject::Confined, uses.containsAny(confinedWritingUses) };
    }

    // A built-in that the called built-in calls in turn takes the object only where the called one passes it on.
    static bool isCalledByOperation(const FlowBuiltinCallLink& link, const FlowConstructionEvent& event)
    {
        switch (link.kind) {
        case FlowCallLinkKind::Call:
        case FlowCallLinkKind::Construct:
            return event.kind == FlowConstructionEventKind::Call;
        case FlowCallLinkKind::Accessor:
            return event.kind != FlowConstructionEventKind::Call;
        case FlowCallLinkKind::BuiltinCallback:
        case FlowCallLinkKind::Protocol:
        case FlowCallLinkKind::Host:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool retainsOnlyInNewResult(const FlowBuiltinCallLink& link, const Value& operation, const FlowConstructionEvent& event) const
    {
        const BuiltinTable& builtins = m_program.builtins();
        const BuiltinOverload& overload = builtins.at(link.overload);
        std::optional<BuiltinOperand> position = builtinOperandOf(operation, event, link.mode);
        if (!position || declaredBehaviorOf(overload, *position).retainedIn != RetainedIn::Result)
            return false;
        switch (builtins.at(overload.result.type).freshness) {
        case BuiltinFreshness::Unaliased:
        case BuiltinFreshness::Yes:
            return true;
        case BuiltinFreshness::No:
        case BuiltinFreshness::UnlessSpecies:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    FunctionID enteredBodyOf(BuiltinOverloadID overload) const
    {
        const RuntimeFunctionEntry* entry = findRuntimeFunction(m_program, analyzedFunctionOf(m_program.builtins().at(overload)));
        return entry ? entry->function : FunctionID();
    }

    bool mayCallBoundFunction(FlowInstanceID instance, const Value& operation) const
    {
        bool mayCall = false;
        operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!operation.operandUses(operandIndex).containsAny(callingUses))
                return;
            FlowNodeID callees = m_flowGraph.nodeOf(instance, operand);
            mayCall |= !callees || m_flowGraph.valueOf(callees).objects.anyOf([&](FlowObjectID callee) {
                return m_flowGraph.objects().isInstanceOf(callee, BuiltinClasses::BoundFunction);
            });
        });
        return mayCall;
    }

    std::optional<uint32_t> addParameterNode(FlowNodeID parameter)
    {
        if (m_flowGraph.kindOf(parameter) != FlowNodeKind::Local)
            return std::nullopt;
        FlowInstanceID instance = m_flowGraph.instanceOf(parameter);
        if (m_flowGraph.isHost(instance))
            return std::nullopt;
        const Function& callee = m_program.at(m_flowGraph.at(instance).function);
        const CompactList<ValueID>& entryParameters = callee.at(entryBlock).parameters;
        uint32_t entryParameter = m_flowGraph.localOf(parameter).index();
        if (callee.isUnsplitCoroutine() || entryParameter >= entryParameters.size())
            return std::nullopt;
        return addNode(instance, { FlowConstructionRootKind::EntryParameter, entryParameters[entryParameter] });
    }

    OperandUses declaredUsesOf(const FlowBuiltinCallLink& link, const Value& operation, const FlowConstructionEvent& event) const
    {
        std::optional<BuiltinOperand> position = builtinOperandOf(operation, event, link.mode);
        if (!position)
            return OperandUses::all();
        OperandBehavior behavior = declaredBehaviorOf(m_program.builtins().at(link.overload), *position);
        return behavior.protocols.isEmpty() ? behavior.uses : OperandUses::all();
    }

    static void collectCallees(FlowConstructionNode& node)
    {
        for (const FlowResolvedConstructionEvent& event : node.resolvedEvents)
            node.callees.appendVector(event.callees);
        std::ranges::sort(node.callees);
        removeRepeatedElements(node.callees);
    }

    FlowSolver& m_solver;
    const FlowConstructionLinks& m_links;
    const Program& m_program;
    const FlowGraph& m_flowGraph;
    FlowConstructionState& m_state;
    FlowConstructionGraph m_graph;
};

} // anonymous namespace

FlowConstructionGraph buildConstructionGraph(FlowSolver& solver, const FlowConstructionLinks& links)
{
    return ConstructionGraphBuilder(solver, links).build();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
