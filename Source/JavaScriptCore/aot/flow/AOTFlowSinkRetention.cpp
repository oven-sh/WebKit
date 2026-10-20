/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowSinkRules.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTGrouping.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OperandUses retainingUses { OperandUse::Retains, OperandUse::RetainsContents };
// A caller has a container for what is stored in its own operand or in the result, and none for an object of the callee that is stored in either.
static constexpr OperandUses untrackedContainerUses { OperandUse::Retains, OperandUse::RetainsContents, OperandUse::PassesToUserCode };

namespace {

struct Retention {
    enum class Target : uint8_t { None, Result, Operand, UnknownMemory };

    static constexpr Retention inResult() { return { Target::Result, 0 }; }
    static constexpr Retention inOperand(uint32_t operand) { return { Target::Operand, operand }; }
    static constexpr Retention inUnknownMemory() { return { Target::UnknownMemory, 0 }; }

    Target target { Target::None };
    uint32_t operand { 0 };

    friend bool operator==(const Retention&, const Retention&) = default;
};

struct Groups {
    Vector<uint32_t> first;
    Vector<uint32_t> members;

    uint32_t size() const { return static_cast<uint32_t>(first.size() - 1); }
    std::span<const uint32_t> of(uint32_t group) const LIFETIME_BOUND { return members.subspan(first[group], first[group + 1] - first[group]); }
};

struct VersionOrder {
    Vector<uint32_t> componentOfVersion;
    Groups versionsByLevel;
};

struct RetentionState {
    const SinkRetentionInputs& inputs;
    Vector<uint32_t> componentOfVersion;
    Groups flowsByCaller;
    Vector<Vector<SinkRetention>> retentionByVersion;
};

struct ActivationCalls {
    Vector<FlowOperandFlow> flowsBySource;
    Vector<FlowOperandFlow> flowsByLink;
};

enum class StoreTargets : uint8_t { Unknown, Listed };

} // anonymous namespace

static Retention join(Retention a, Retention b)
{
    if (a.target == Retention::Target::None)
        return b;
    if (b.target == Retention::Target::None || a == b)
        return a;
    return Retention::inUnknownMemory();
}

static SinkRetention publishedRetentionOf(Retention retention)
{
    switch (retention.target) {
    case Retention::Target::None:
    case Retention::Target::UnknownMemory:
        return { };
    case Retention::Target::Result:
        return { RetainedIn::Result, 0 };
    case Retention::Target::Operand:
        return { RetainedIn::Operand, retention.operand };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Groups sortIntoGroups(uint32_t numberOfGroups, uint32_t count, NOESCAPE const Invocable<uint32_t(uint32_t)> auto& groupAt, NOESCAPE const Invocable<uint32_t(uint32_t)> auto& memberAt)
{
    Groups groups;
    groupByIndex(numberOfGroups, groups.first, groups.members, [&](const auto& visit) {
        for (uint32_t index = 0; index < count; ++index)
            visit(groupAt(index), memberAt(index));
    });
    return groups;
}

static bool storesValue(FlowAccessKind kind)
{
    return isWrite(kind) && kind != FlowAccessKind::Delete;
}

static bool hasFlowForEveryReference(EntryParameter kind)
{
    switch (kind) {
    case EntryParameter::Capture:
    case EntryParameter::This:
    case EntryParameter::Parameter:
        return true;
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowLocalID findRestParameter(const FlowSummary& summary)
{
    FlowLocalID parameter;
    if (!summary.restSite)
        return parameter;
    for (const FlowSeed& seed : summary.seeds) {
        if (seed.kind() == FlowSeedKind::Site && seed.site() == *summary.restSite)
            parameter = seed.node();
    }
    return parameter;
}

StoreContainer findStoreContainer(const Program& program, const FlowSummary& summary, FlowLocalID receiver)
{
    ValueID value = receiver && !summary.isExternal(receiver) ? summary.originOf(receiver) : ValueID();
    if (!value)
        return { };

    const Function& function = program.at(summary.function);
    ValueID definition = sameObjectThroughCasts(function, value);
    FlowLocalID root = summary.localOfValue[definition.index()];
    if (!root)
        return { };
    if (root.index() < summary.numberOfEntryParameters)
        return { StoreContainerKind::EntryParameter, root };

    EffectSummaries unknownSummaries;
    switch (freshnessOf(EffectContext { program, unknownSummaries }, function, function.at(definition))) {
    case ResultFreshness::Unknown:
        return { };
    case ResultFreshness::Fresh:
        return { StoreContainerKind::OwnAllocation, root };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

class ActivationWalk {
    WTF_MAKE_NONCOPYABLE(ActivationWalk);
public:
    ActivationWalk(const RetentionState& state, FunctionID version, FlowInstanceID home)
        : m_state(state)
        , m_families(state.inputs.families)
        , m_graph(m_families.graph())
        , m_records(m_families.records())
        , m_numbering(state.inputs.numbering)
        , m_equations(state.inputs.equations)
        , m_summary(m_graph.summaryOf(home))
        , m_version(version)
        , m_home(home)
        , m_restParameter(findRestParameter(m_summary))
    {
    }

    Retention retentionOf(FlowNodeID parameter)
    {
        Retention result;
        uint32_t node = m_numbering.graphNode(parameter);
        if (!m_equations.sinks[node].contains(OperandUse::Retains))
            return result;

        m_visited.clear();
        m_worklist.shrink(0);
        visit(node);
        while (!m_worklist.isEmpty() && result != Retention::inUnknownMemory())
            result = join(result, retentionAt(m_worklist.takeLast()));
        return result;
    }

private:
    void visit(uint32_t node)
    {
        if (m_visited.add(node).isNewEntry)
            m_worklist.append(node);
    }

    bool isInActivation(FlowNodeID node) const { return m_graph.instanceOf(node) == m_home; }

    FlowLocalID localOf(ValueID value) const { return value ? m_summary.localOfValue[value.index()] : FlowLocalID(); }

    bool mayPointTo(FlowLocalID local, FlowObjectID object) const
    {
        return m_summary.isExternal(local) || m_graph.valueOf(m_graph.nodeOf(m_home, local)).objects.contains(object);
    }

    bool carriesRetains(const SinkDependency& dependency) const
    {
        if (dependency.transform == SinkTransform::IfReturned && !m_equations.sinks[dependency.condition].contains(OperandUse::Returns))
            return false;
        return applyTransform(dependency.transform, m_equations.sinks[dependency.node]).contains(OperandUse::Retains);
    }

    Retention retentionAt(uint32_t node)
    {
        if (m_equations.base[node].containsAny(retainingUses))
            return Retention::inUnknownMemory();

        Retention result = retentionInCalleeObjects(node);
        for (const SinkDependency& dependency : m_equations.dependenciesOf(node)) {
            if (carriesRetains(dependency))
                result = join(result, retentionThrough(node, dependency));
        }
        return result;
    }

    Retention retentionThrough(uint32_t subject, const SinkDependency& dependency)
    {
        switch (dependency.transform) {
        case SinkTransform::Same:
        case SinkTransform::IfReturned:
            return retentionOfCopy(dependency.node);
        case SinkTransform::Lifetime:
            return retentionThroughObject(subject, dependency.node);
        case SinkTransform::Argument:
            return retentionThroughCallee(subject, dependency.node);
        case SinkTransform::Leaves:
        case SinkTransform::Contents:
            return Retention::inUnknownMemory();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Retention retentionOfCopy(uint32_t node)
    {
        bool isFollowed = false;
        switch (m_numbering.kindOf(node)) {
        case SinkNodeKind::GraphNode:
            isFollowed = isInActivation(m_numbering.graphNodeAt(node));
            break;
        case SinkNodeKind::ResultHub:
            isFollowed = m_records.at(m_numbering.callLinkAt(node)).callerInstance == m_home;
            break;
        case SinkNodeKind::ObjectLifetime:
        case SinkNodeKind::OuterMemory:
            break;
        }
        if (!isFollowed)
            return Retention::inUnknownMemory();
        visit(node);
        return { };
    }

    Retention retentionThroughValue(FlowNodeID node)
    {
        if (!isInActivation(node))
            return Retention::inUnknownMemory();

        OperandUses uses = m_equations.sinks[m_numbering.graphNode(node)];
        if (uses.containsAny(untrackedContainerUses))
            return Retention::inUnknownMemory();
        return uses.contains(OperandUse::Returns) ? Retention::inResult() : Retention();
    }

    Retention retentionThroughContainer(FlowLocalID container)
    {
        StoreContainer found = findStoreContainer(m_families.program(), m_summary, container);
        switch (found.kind) {
        case StoreContainerKind::Unknown:
            return Retention::inUnknownMemory();
        case StoreContainerKind::EntryParameter:
            return found.root == m_restParameter ? Retention::inUnknownMemory() : Retention::inOperand(found.root.index());
        case StoreContainerKind::OwnAllocation:
            return retentionThroughValue(m_graph.nodeOf(m_home, found.root));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    StoreTargets listedUnlessBuiltinRuns(ValueID origin) const
    {
        return m_families.calls.builtinLinksAt(m_home, origin).empty() ? StoreTargets::Listed : StoreTargets::Unknown;
    }

    StoreTargets forEachStoreTarget(FlowStatementReference reference, FlowLocalID stored, NOESCAPE const Invocable<void(FlowLocalID)> auto& functor) const
    {
        const FlowStatements& statements = m_summary.statements;
        switch (reference.kind) {
        case FlowStatementKind::Allocation: {
            const FlowAllocation& allocation = statements.allocations[reference.index];
            if (allocation.prototype == stored)
                functor(localOf(allocation.origin));
            return StoreTargets::Listed;
        }
        case FlowStatementKind::FieldAccess: {
            const FlowFieldAccess& access = statements.fieldAccesses[reference.index];
            if (storesValue(access.kind) && access.value == stored)
                functor(access.receiver);
            return StoreTargets::Listed;
        }
        case FlowStatementKind::PropertyAccess: {
            const FlowPropertyAccess& access = statements.propertyAccesses[reference.index];
            if (!storesValue(access.kind) || access.value != stored)
                return StoreTargets::Listed;
            functor(access.receiver);
            functor(access.thisValue);
            return listedUnlessBuiltinRuns(access.origin);
        }
        case FlowStatementKind::ElementAccess: {
            const FlowElementAccess& access = statements.elementAccesses[reference.index];
            if (!storesValue(access.kind) || access.value != stored)
                return StoreTargets::Listed;
            functor(access.receiver);
            functor(access.thisValue);
            return listedUnlessBuiltinRuns(access.origin);
        }
        case FlowStatementKind::SlotAccess: {
            const FlowSlotAccess& access = statements.slotAccesses[reference.index];
            if (storesValue(access.kind) && access.value == stored)
                functor(access.receiver);
            return StoreTargets::Listed;
        }
        case FlowStatementKind::PrototypeAccess: {
            const FlowPrototypeAccess& access = statements.prototypeAccesses[reference.index];
            if (storesValue(access.kind) && access.value == stored)
                functor(access.object);
            return StoreTargets::Listed;
        }
        case FlowStatementKind::Iteration: {
            const FlowIteration& iteration = statements.iterations[reference.index];
            if (iteration.iterable == stored || iteration.sent == stored)
                functor(iteration.state);
            return StoreTargets::Listed;
        }
        case FlowStatementKind::OperandUse: {
            const FlowOperandUse& use = statements.operandUses[reference.index];
            if (use.operand != stored)
                return StoreTargets::Listed;
            functor(use.retainer);
            return listedUnlessBuiltinRuns(use.origin);
        }
        case FlowStatementKind::Call:
            return listedUnlessBuiltinRuns(statements.calls[reference.index].origin);
        case FlowStatementKind::ClassDefinition:
        case FlowStatementKind::MethodDefinition:
        case FlowStatementKind::Await:
        case FlowStatementKind::BuiltinCall:
        case FlowStatementKind::Lookup:
            return StoreTargets::Unknown;
        case FlowStatementKind::Filter:
        case FlowStatementKind::ConditionalSeed:
        case FlowStatementKind::NaturalOperation:
        case FlowStatementKind::LocationAccess:
        case FlowStatementKind::PropertyCopy:
        case FlowStatementKind::DirectCall:
        case FlowStatementKind::ModuleOperation:
        case FlowStatementKind::UnseenCodeEvent:
            return StoreTargets::Listed;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Retention retentionThroughObject(uint32_t subject, uint32_t holder)
    {
        FlowObjectID object;
        switch (m_numbering.kindOf(holder)) {
        case SinkNodeKind::ObjectLifetime:
            object = m_numbering.objectAt(holder);
            break;
        case SinkNodeKind::OuterMemory:
            break;
        case SinkNodeKind::GraphNode:
        case SinkNodeKind::ResultHub:
            return Retention::inUnknownMemory();
        }
        if (m_numbering.kindOf(subject) != SinkNodeKind::GraphNode)
            return Retention::inUnknownMemory();
        FlowNodeID node = m_numbering.graphNodeAt(subject);
        if (m_graph.kindOf(node) != FlowNodeKind::Local)
            return Retention::inUnknownMemory();

        FlowLocalID stored = m_graph.localOf(node);
        Retention result;
        bool hasContainer = false;
        auto addContainer = [&](FlowLocalID container) {
            hasContainer = true;
            result = join(result, retentionThroughContainer(container));
        };
        auto addStoreTarget = [&](FlowLocalID target) {
            if (target && (!object || mayPointTo(target, object)))
                addContainer(target);
        };

        for (FlowStatementReference statement : m_summary.waiters.of(stored)) {
            switch (forEachStoreTarget(statement, stored, addStoreTarget)) {
            case StoreTargets::Unknown:
                result = Retention::inUnknownMemory();
                break;
            case StoreTargets::Listed:
                break;
            }
        }
        for (FlowLocalID target : m_summary.successors.of(stored)) {
            if (!m_summary.isExternal(target) || !m_summary.externalAt(target).isPartOfSite())
                continue;
            uint32_t site = m_summary.externalAt(target).site();
            if (!object || m_graph.objectOf(m_home, site) == object)
                addContainer(localOf(m_summary.sites[site].value));
        }
        return hasContainer ? result : Retention::inUnknownMemory();
    }

    const ActivationCalls& calls()
    {
        if (m_calls)
            return *m_calls;

        ActivationCalls sorted;
        for (uint32_t index : m_state.flowsByCaller.of(m_home.index()))
            sorted.flowsBySource.append(m_records.operandFlows()[index]);
        sorted.flowsByLink = sorted.flowsBySource;
        std::ranges::sort(sorted.flowsBySource, { }, [](const FlowOperandFlow& flow) {
            return std::tuple { flow.from, flow.parameter, flow.link };
        });
        std::ranges::sort(sorted.flowsByLink, { }, [](const FlowOperandFlow& flow) {
            return std::tuple { flow.link, flow.parameter, flow.from };
        });
        m_calls = WTF::move(sorted);
        return *m_calls;
    }

    std::span<const FlowOperandFlow> flowsFrom(FlowNodeID from)
    {
        return recordsWith(calls().flowsBySource.span(), from, &FlowOperandFlow::from);
    }

    std::span<const FlowOperandFlow> flowsBetween(FlowNodeID from, FlowNodeID parameter)
    {
        return recordsWith(calls().flowsBySource.span(), std::pair { from, parameter }, [](const FlowOperandFlow& flow) {
            return std::pair { flow.from, flow.parameter };
        });
    }

    std::span<const FlowOperandFlow> flowsUnder(FlowCallLinkID link, FlowNodeID parameter)
    {
        return recordsWith(calls().flowsByLink.span(), std::pair { link, parameter }, [](const FlowOperandFlow& flow) {
            return std::pair { flow.link, flow.parameter };
        });
    }

    Retention retentionInCalleeObjects(uint32_t subject)
    {
        Retention result;
        if (m_numbering.kindOf(subject) != SinkNodeKind::GraphNode)
            return result;
        for (const FlowOperandFlow& flow : flowsFrom(m_numbering.graphNodeAt(subject))) {
            FlowObjectID owner = m_graph.objectOf(flow.parameter);
            if (owner && m_equations.sinks[m_numbering.lifetime(owner)].containsAny(escapingUses))
                result = Retention::inUnknownMemory();
        }
        return result;
    }

    Retention retentionThroughOperand(FlowCallLinkID link, FlowNodeID parameter)
    {
        std::span<const FlowOperandFlow> flows = flowsUnder(link, parameter);
        if (flows.empty())
            return Retention::inUnknownMemory();

        Retention result;
        for (const FlowOperandFlow& flow : flows) {
            bool isLocal = m_graph.kindOf(flow.from) == FlowNodeKind::Local && isInActivation(flow.from);
            result = join(result, isLocal ? retentionThroughContainer(m_graph.localOf(flow.from)) : Retention::inUnknownMemory());
        }
        return result;
    }

    Retention retentionInCaller(FlowCallLinkID link, SinkRetention retentionInCallee)
    {
        const FlowCallLink& call = m_records.at(link);
        switch (retentionInCallee.retainedIn) {
        case RetainedIn::UnknownMemory:
            return Retention::inUnknownMemory();
        case RetainedIn::Result:
            return call.result ? retentionThroughValue(call.result) : Retention();
        case RetainedIn::Operand:
            return retentionThroughOperand(link, m_graph.nodeOf(call.calleeInstance, m_graph.summaryOf(call.calleeInstance).entryParameter(retentionInCallee.retainingOperand)));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Retention retentionThroughCallee(uint32_t subject, uint32_t parameter)
    {
        if (m_numbering.kindOf(subject) != SinkNodeKind::GraphNode || m_numbering.kindOf(parameter) != SinkNodeKind::GraphNode)
            return Retention::inUnknownMemory();
        FlowNodeID node = m_numbering.graphNodeAt(parameter);
        if (m_graph.kindOf(node) != FlowNodeKind::Local)
            return Retention::inUnknownMemory();

        FlowInstanceID callee = m_graph.instanceOf(node);
        uint32_t index = m_graph.localOf(node).index();
        std::span<const FlowOperandFlow> flows = flowsBetween(m_numbering.graphNodeAt(subject), node);
        if (index >= m_graph.summaryOf(callee).numberOfEntryParameters || flows.empty())
            return Retention::inUnknownMemory();
        if (!hasFlowForEveryReference(m_families.program().at(m_graph.at(callee).function).entryParameterKind(index)))
            return Retention::inUnknownMemory();

        FunctionID version = versionRunning(m_graph, m_state.inputs.versions, callee);
        if (m_state.componentOfVersion[version.index()] == m_state.componentOfVersion[m_version.index()])
            return Retention::inUnknownMemory();

        Retention result;
        for (const FlowOperandFlow& flow : flows)
            result = join(result, retentionInCaller(flow.link, m_state.retentionByVersion[version.index()][index]));
        return result;
    }

    const RetentionState& m_state;
    const FlowFamilyInputs& m_families;
    const FlowGraph& m_graph;
    const FlowRecords& m_records;
    const SinkNodeNumbering& m_numbering;
    const SinkEquations& m_equations;
    const FlowSummary& m_summary;
    FunctionID m_version;
    FlowInstanceID m_home;
    FlowLocalID m_restParameter;
    std::optional<ActivationCalls> m_calls;
    HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_visited;
    Vector<uint32_t, 16> m_worklist;
};

} // anonymous namespace

static VersionOrder computeVersionOrder(const SinkRetentionInputs& inputs)
{
    const FlowGraph& graph = inputs.families.graph();
    std::span<const FlowCallLink> links = inputs.families.records().callLinks();
    uint32_t numberOfVersions = inputs.versions.functionCount();

    Vector<std::pair<uint32_t, uint32_t>> calls;
    calls.reserveInitialCapacity(links.size());
    for (const FlowCallLink& link : links) {
        if (graph.isHost(link.callerInstance))
            continue;
        uint32_t callee = versionRunning(graph, inputs.versions, link.calleeInstance).index();
        forEachVersionRunning(graph, inputs.versions, link.callerInstance, [&](FunctionID caller) {
            calls.append({ caller.index(), callee });
        });
    }
    Groups callees = sortIntoGroups(numberOfVersions, static_cast<uint32_t>(calls.size()), [&](uint32_t index) {
        return calls[index].first;
    }, [&](uint32_t index) {
        return calls[index].second;
    });

    StronglyConnectedComponents components = computeStronglyConnectedComponents(numberOfVersions, [&](unsigned version) {
        return callees.of(version);
    });
    auto versionAt = [](uint32_t version) {
        return version;
    };
    Groups versionsByComponent = sortIntoGroups(components.numberOfComponents, numberOfVersions, [&](uint32_t version) {
        return components.componentOf[version];
    }, versionAt);

    Vector<uint32_t> levelOfComponent;
    levelOfComponent.fill(0, components.numberOfComponents);
    uint32_t numberOfLevels = 0;
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        uint32_t level = 0;
        for (uint32_t version : versionsByComponent.of(component)) {
            for (uint32_t callee : callees.of(version)) {
                uint32_t calleeComponent = components.componentOf[callee];
                if (calleeComponent != component)
                    level = std::max(level, levelOfComponent[calleeComponent] + 1);
            }
        }
        levelOfComponent[component] = level;
        numberOfLevels = std::max(numberOfLevels, level + 1);
    }

    Groups versionsByLevel = sortIntoGroups(numberOfLevels, numberOfVersions, [&](uint32_t version) {
        return levelOfComponent[components.componentOf[version]];
    }, versionAt);
    return { WTF::move(components.componentOf), WTF::move(versionsByLevel) };
}

static Groups groupFlowsByCaller(const FlowGraph& graph, const FlowRecords& records)
{
    std::span<const FlowOperandFlow> flows = records.operandFlows();
    return sortIntoGroups(graph.numberOfInstances(), static_cast<uint32_t>(flows.size()), [&](uint32_t index) {
        return records.at(flows[index].link).callerInstance.index();
    }, [](uint32_t index) {
        return index;
    });
}

static void computeRetentionOf(RetentionState& state, FunctionID id)
{
    const FlowFamilyInputs& families = state.inputs.families;
    const FlowGraph& graph = families.graph();
    FlowFunctionVersion version = findFunctionVersion(graph, state.inputs.versions, id);
    const FlowSummary& summary = graph.summaryOf(version.body);
    if (!version.isReached() || !summary.hasBody() || families.solution.isPastWorkLimit(version.body))
        return;

    Vector<uint32_t, 8> retainedParameters;
    for (uint32_t index = 0; index < summary.numberOfEntryParameters; ++index) {
        OperandUses uses;
        for (FlowNodeID node : nodesOf(graph, version, summary.entryParameter(index)))
            uses.add(state.inputs.equations.sinks[state.inputs.numbering.graphNode(node)]);
        if (uses.contains(OperandUse::Retains) && !uses.contains(OperandUse::PassesToUserCode))
            retainedParameters.append(index);
    }
    if (retainedParameters.isEmpty())
        return;

    Vector<Retention, 8> retentions;
    retentions.fill(Retention(), retainedParameters.size());
    for (FlowInstanceID home : version.instances) {
        ActivationWalk walk(state, id, home);
        for (size_t position = 0; position < retainedParameters.size(); ++position)
            retentions[position] = join(retentions[position], walk.retentionOf(graph.nodeOf(home, summary.entryParameter(retainedParameters[position]))));
    }
    for (size_t position = 0; position < retainedParameters.size(); ++position)
        state.retentionByVersion[id.index()][retainedParameters[position]] = publishedRetentionOf(retentions[position]);
}

Vector<Vector<SinkRetention>> computeSinkRetention(const SinkRetentionInputs& inputs)
{
    const FlowGraph& graph = inputs.families.graph();
    const ParallelLoops& loops = inputs.families.loops();
    VersionOrder order;
    Groups flowsByCaller;
    runTasksInParallel(loops, {
        [&] { order = computeVersionOrder(inputs); },
        [&] { flowsByCaller = groupFlowsByCaller(graph, inputs.families.records()); },
    });
    RetentionState state { inputs, WTF::move(order.componentOfVersion), WTF::move(flowsByCaller), { } };

    state.retentionByVersion.grow(inputs.versions.functionCount());
    forEachIndexInParallel(loops, state.retentionByVersion.size(), [&](unsigned index) {
        const FlowSummary& summary = graph.summaryOf(inputs.versions.bodyOf(FunctionID(index)));
        state.retentionByVersion[index].fill(SinkRetention { }, summary.numberOfEntryParameters);
    });

    for (uint32_t level = 0; level < order.versionsByLevel.size(); ++level) {
        std::span<const uint32_t> versions = order.versionsByLevel.of(level);
        forEachIndexInParallel(loops, versions.size(), [&](unsigned index) {
            computeRetentionOf(state, FunctionID(versions[index]));
        });
    }
    return WTF::move(state.retentionByVersion);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
