/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowSolver.h"

#if ENABLE(AOT)

#include "AOTFlowCallOperations.h"
#include "AOTFlowFixpointCheck.h"
#include "AOTFlowNames.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowPropertyOperations.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>
#include <wtf/MathExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static Subject literalLimitSubject(const FlowGraph& graph, FlowNodeID target, const FlowValue& source)
{
    if (!source.strings && !source.symbols)
        return { };
    FunctionID function = graph.functionChargedFor(target);
    if (graph.isHost(function))
        return { };
    return { .function = function };
}

FlowSolver::FlowSolver(const Program& program, FlowObjectTable& objects, FlowGraph& graph, BuildContext& build)
    : m_program(program)
    , m_objects(objects)
    , m_graph(graph)
    , m_build(build)
    , m_evaluator(program, graph.values(), graph.values().types())
    , m_workByFunction(FillWith { }, program.functions.size() + 1, uint64_t { 0 })
    , m_maxWorkPerFunction(build.limit(LimitID::MaxFlowWorkPerFunction))
    , m_maxEscapedObjectsBesideUnknown(build.limit(LimitID::MaxEscapedObjectsBesideUnknown))
    , m_losesWakeUps(build.recordedOptions().has(TestingSwitch::LoseFlowWakeUps))
{
    m_states.objects = createFlowObjectState(*this);
    m_states.properties = createFlowPropertyState(*this);
    m_states.calls = createFlowCallState(*this);

    if (uint32_t seed = build.options().flowOrderSeed)
        m_shuffle.emplace(seed);

    // A solve that is stopped from outside has said by then where its steps went.
    if (build.options().logging.logsPhaseTimes)
        m_counters.stepsAtNextLog = 1 << 20;
}

FlowSolver::~FlowSolver() = default;

void FlowSolver::drainQueuesAndRefine()
{
    while (true) {
        drainQueues();
        if (releaseStepsRelyingOnAbsence())
            continue;
        FlowProgress constructions = refineConstructions(*this);
        FlowProgress order = refineExecutionOrder(*this);
        FlowProgress inheritors = refineEscapedInheritors(*this);
        if (constructions == FlowProgress::Finished && order == FlowProgress::Finished && inheritors == FlowProgress::Finished && !hasQueuedWork())
            return;
    }
}

bool FlowSolver::mayRelyOn(FlowAbsence absence, FlowStatementSite site)
{
    // Running every rule again on the final sets decides as the last release did. The last run of an abandoned instance has no run after it.
    if (!site.instance || m_fixpointCheck || m_isInLastRunOfInstance)
        return true;

    // While the steps of a kind are released none of the kinds before it waits and no node changes its value, so whatever a released statement meets is judged on the quiet state.
    if (m_quietSteps.isReleasing && absence <= m_quietSteps.releasedAbsence)
        return true;

    Member member = memberWaitingFor(site, FlowWakeUp::Revisit);
    Waiter& waiter = m_quietSteps.waiting[static_cast<size_t>(absence)].ensure(std::pair { member.site, member.cursor }, [&] {
        return Waiter { member.site, member.cursor, { }, { } };
    }).iterator->value;
    if (member.object)
        waiter.members.add(member.object);
    return false;
}

bool FlowSolver::releaseStepsRelyingOnAbsence()
{
    auto waiting = std::ranges::find_if(m_quietSteps.waiting, [](const StepsRelyingOnAbsence& steps) {
        return !steps.isEmpty();
    });
    if (waiting == m_quietSteps.waiting.end())
        return false;

    RELEASE_ASSERT(m_worklist.statementsToRun.isEmpty());
    m_quietSteps.releasedAbsence = static_cast<FlowAbsence>(waiting - m_quietSteps.waiting.begin());
    for (const Waiter& waiter : std::exchange(*waiting, { }).values())
        bringBack(waiter);

    Vector<FlowStatementSite> statements;
    statements.reserveInitialCapacity(m_worklist.statementsToRun.size());
    while (!m_worklist.statementsToRun.isEmpty())
        statements.append(m_worklist.statementsToRun.takeFirst());
    if (m_shuffle) [[unlikely]] {
        for (size_t index = statements.size(); index > 1; --index)
            std::swap(statements[index - 1], statements[m_shuffle->getUint32(static_cast<uint32_t>(index))]);
    }

    m_quietSteps.isReleasing = true;
    m_quietSteps.numberOfNodesAtRelease = m_graph.numberOfNodes();
    for (FlowStatementSite site : statements)
        runQueuedStatement(site);
    m_quietSteps.isReleasing = false;
    ++m_quietSteps.numberOfReleases;

    for (const HeldBackValue& held : std::exchange(m_quietSteps.heldBackValues, { })) {
        if (held.recordsOrigin)
            join(held.target, held.value, held.witness);
        else
            propagate(held.target, held.value, held.witness);
    }
    return true;
}

void FlowSolver::solve()
{
    addRoots(*this);
    drainQueuesAndRefine();
    // A set that is too small is a false proof, and nothing after the solve can tell.
    if (m_build.options().checksFlowFixpoint) [[unlikely]]
        m_fixpointChanges = FixpointCheck(*this).run();
    keepWhatProvenLookupsRelyOn(*this);
    m_graph.sortPropertiesOfUnknownObject();

    Statistics& statistics = m_build.statistics();
    statistics.count(StatisticID::FlowWorklistSteps, m_counters.nodeSteps + m_counters.statementRuns);
    statistics.count(StatisticID::FlowStatementRuns, m_counters.statementRuns);
    statistics.count(StatisticID::FlowStatementRunsWithoutChange, m_counters.statementRunsWithoutChange);
    statistics.count(StatisticID::FlowStatementWakeUps, m_counters.wakeUps);
    statistics.count(StatisticID::FlowStatementsStartedOver, m_counters.statementsStartedOver);
    statistics.count(StatisticID::FlowMembersRepeated, m_counters.membersRepeated);
    statistics.count(StatisticID::FlowCursorsNotReached, m_counters.cursorsNotReached);
    statistics.count(StatisticID::FlowNodes, m_graph.numberOfNodes());
    statistics.count(StatisticID::FlowEdges, m_graph.numberOfEdges());
    statistics.count(StatisticID::FlowInstances, m_graph.numberOfInstances());
    statistics.count(StatisticID::FlowAllocationSites, m_objects.numberOfSites());
    statistics.count(StatisticID::FlowAllocationClasses, m_objects.numberOfAllocationClasses());
    values().addTo(statistics);
    if (m_build.options().logging.logsPhaseTimes) [[unlikely]]
        logCounters();
}

void FlowSolver::logCounters() const
{
    dataLogLn("AOT flow solve: ", m_counters.nodeSteps, " node steps | ", m_counters.valuesPushedAlongEdges, " values pushed along edges, ", m_counters.valuesPushedWithoutChange, " without change | ", m_counters.statementRuns, " statement runs, ", m_counters.statementRunsWithoutChange, " without change | ",
        m_counters.wakeUps, " wake-ups | ", m_counters.statementsStartedOver, " statements started over | ", m_counters.membersRepeated, " members repeated | ",
        m_counters.cursorsNotReached, " cursors not reached | ", m_quietSteps.numberOfReleases, " releases of steps that rely on an absence | ", m_graph.numberOfNodes(), " nodes | ", m_graph.numberOfEdges(), " edges | ", m_cursors.all.size(), " cursors");
    for (size_t kind = 0; kind < Counters::numberOfStatementKinds; ++kind) {
        if (m_counters.runsOfKind[kind])
            dataLogLn("AOT flow solve:   ", nameOf(static_cast<FlowStatementKind>(kind)), " | ", m_counters.runsOfKind[kind], " runs, ", m_counters.runsWithoutChangeOfKind[kind], " without change");
    }

    for (size_t kind = 0; kind < Counters::numberOfNodeKinds; ++kind) {
        if (m_counters.nodeStepsOfKind[kind])
            dataLogLn("AOT flow solve:   ", nameOf(static_cast<FlowNodeKind>(kind)), " | ", m_counters.nodeStepsOfKind[kind], " node steps");
    }

    constexpr size_t numberOfNodesToLog = 8;
    HashMap<FlowNodeID, size_t> numberOfWaitersOn;
    for (const auto& waiter : m_waiters.onNodes.waiters)
        ++numberOfWaitersOn.add(waiter.watched, 0).iterator->value;
    Vector<std::pair<size_t, FlowNodeID>> nodesByWaiters;
    for (auto& [node, numberOfWaiters] : numberOfWaitersOn)
        nodesByWaiters.append({ numberOfWaiters, node });
    std::ranges::sort(nodesByWaiters, std::greater { });
    for (auto [numberOfWaiters, node] : nodesByWaiters.span().first(std::min(numberOfNodesToLog, nodesByWaiters.size()))) {
        dataLog("AOT flow solve:   n", node.index(), " ", nameOf(m_graph.kindOf(node)), " | ", numberOfWaiters, " waiters | ", valueOf(node).objects.size(), " objects");
        for (auto& [key, temporary] : m_temporaries.all) {
            if (temporary == node)
                dataLog(" | owner ", static_cast<unsigned>(key.owner), " role ", key.role, " index ", key.index, " of ", nameOf(key.site.statement.kind), " ", key.site.statement.index, " in i", key.site.instance.index());
        }
        dataLogLn();
    }
}

FlowRecords FlowSolver::takeRecords()
{
    m_records.finish(m_graph);
    return WTF::move(m_records);
}

FlowSolverStates FlowSolver::takeStates()
{
    return WTF::move(m_states);
}

void FlowSolver::drainQueues()
{
    while (true) {
        if (m_counters.nodeSteps + m_counters.statementRuns >= m_counters.stepsAtNextLog) [[unlikely]] {
            m_counters.stepsAtNextLog *= 2;
            logCounters();
        }

        if (!m_worklist.functionsToAbandon.isEmpty())
            abandon(m_worklist.functionsToAbandon.takeFirst());
        else if (!m_worklist.hostCallsToAbandon.isEmpty())
            abandonHostCall(m_worklist.hostCallsToAbandon.takeFirst());
        else if (m_shuffle) [[unlikely]] {
            if (!takeShuffledStep())
                return;
        } else if (!m_worklist.instancesToInitialize.isEmpty())
            initializeInstance(m_worklist.instancesToInitialize.takeFirst());
        else if (!m_worklist.objectsReachingNativeCode.isEmpty())
            didReachNativeCode(*this, m_worklist.objectsReachingNativeCode.takeFirst());
        else if (!m_worklist.objectsReachingUnknownJavaScript.isEmpty())
            didReachUnknownJavaScript(*this, m_worklist.objectsReachingUnknownJavaScript.takeFirst());
        else if (!m_worklist.nodesToProcess.isEmpty())
            processNode(FlowNodeID(m_worklist.nodesToProcess.takeNext()));
        else if (!m_worklist.statementsToRun.isEmpty())
            runQueuedStatement(m_worklist.statementsToRun.takeFirst());
        else if (!m_worklist.widelyReadNodesToProcess.isEmpty())
            processNode(FlowNodeID(m_worklist.widelyReadNodesToProcess.takeNext()));
        else
            return;
    }
}

bool FlowSolver::takeShuffledStep()
{
    enum class Queue : uint8_t { Instances, NativeCode, UnknownJavaScript, Nodes, Statements, WidelyReadNodes };
    Vector<Queue, 6> queues;
    auto addUnlessEmpty = [&](Queue queue, const auto& work) {
        if (!work.isEmpty())
            queues.append(queue);
    };
    addUnlessEmpty(Queue::Instances, m_worklist.instancesToInitialize);
    addUnlessEmpty(Queue::NativeCode, m_worklist.objectsReachingNativeCode);
    addUnlessEmpty(Queue::UnknownJavaScript, m_worklist.objectsReachingUnknownJavaScript);
    addUnlessEmpty(Queue::Nodes, m_worklist.nodesToProcess);
    addUnlessEmpty(Queue::Statements, m_worklist.statementsToRun);
    addUnlessEmpty(Queue::WidelyReadNodes, m_worklist.widelyReadNodesToProcess);
    if (queues.isEmpty())
        return false;

    auto takeFrom = [&](auto& deque) {
        return m_shuffle->getUint32(2) ? deque.takeFirst() : deque.takeLast();
    };
    auto takeNodeFrom = [&](FlowSweepQueue& queue) {
        queue.continueAt(m_shuffle->getUint32(m_graph.numberOfNodes()));
        return FlowNodeID(queue.takeNext());
    };
    switch (queues[m_shuffle->getUint32(queues.size())]) {
    case Queue::Instances:
        initializeInstance(takeFrom(m_worklist.instancesToInitialize));
        return true;
    case Queue::NativeCode:
        didReachNativeCode(*this, takeFrom(m_worklist.objectsReachingNativeCode));
        return true;
    case Queue::UnknownJavaScript:
        didReachUnknownJavaScript(*this, takeFrom(m_worklist.objectsReachingUnknownJavaScript));
        return true;
    case Queue::Nodes:
        processNode(takeNodeFrom(m_worklist.nodesToProcess));
        return true;
    case Queue::Statements:
        runQueuedStatement(takeFrom(m_worklist.statementsToRun));
        return true;
    case Queue::WidelyReadNodes:
        processNode(takeNodeFrom(m_worklist.widelyReadNodesToProcess));
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FlowSolver::hasQueuedWork() const
{
    return !m_worklist.functionsToAbandon.isEmpty()
        || !m_worklist.hostCallsToAbandon.isEmpty()
        || !m_worklist.instancesToInitialize.isEmpty()
        || !m_worklist.objectsReachingNativeCode.isEmpty()
        || !m_worklist.objectsReachingUnknownJavaScript.isEmpty()
        || !m_worklist.nodesToProcess.isEmpty()
        || !m_worklist.statementsToRun.isEmpty()
        || !m_worklist.widelyReadNodesToProcess.isEmpty()
        || std::ranges::any_of(m_quietSteps.waiting, [](const StepsRelyingOnAbsence& steps) { return !steps.isEmpty(); });
}

uint64_t FlowSolver::numberOfChanges() const
{
    return m_counters.changes + m_graph.numberOfNodes() + m_graph.numberOfEdges() + m_graph.numberOfInstances();
}

void FlowSolver::join(FlowNodeID target, const FlowValue& value, const Witness& witness)
{
    if (holdsBackValuesOf(target)) {
        if (!value.isEmpty())
            m_quietSteps.heldBackValues.append({ target, value, witness, true });
        return;
    }
    if (propagate(target, value, witness) == UnknownObjectArrival::Arrived)
        m_records.noteUnknownObjectOrigin(target, witness);
}

FlowSolver::UnknownObjectArrival FlowSolver::propagate(FlowNodeID target, const FlowValue& value, const Witness& witness)
{
    if (!target || value.isEmpty())
        return UnknownObjectArrival::None;
    if (FlowNodeID unseenCode = unseenCodeInPlaceOf(target)) {
        reach(unseenCode, value.objects, witness);
        return UnknownObjectArrival::None;
    }
    return addTo(target, value);
}

FlowNodeID FlowSolver::unseenCodeInPlaceOf(FlowNodeID target) const
{
    if (m_graph.isUnseenCode(target))
        return target;
    if (!m_graph.hasInstancesPastWorkLimit())
        return { };
    if (m_graph.kindOf(target) == FlowNodeKind::Local && m_graph.at(m_graph.instanceOf(target)).state == FlowInstanceState::PastWorkLimit)
        return m_graph.unknownJavaScript();
    return { };
}

FlowSolver::UnknownObjectArrival FlowSolver::addTo(FlowNodeID target, const FlowValue& value)
{
    if (value.isEmpty())
        return UnknownObjectArrival::None;

    UnknownObjectArrival arrival = value.hasUnknownObject() ? UnknownObjectArrival::Arrived : UnknownObjectArrival::None;
    FlowValue& held = m_graph.at(target).value;
    // Whatever the node holds has been let in, so none of the objects is one that the node keeps out.
    if (value.objects.isSubsetOf(held.objects) && bringsNoPrimitives(value, held))
        return arrival;

    if (m_nodesWithoutObjectsLettingInUnseenCode.get(target.index()) && value.objects.overlaps(m_objectsLettingInUnseenCode)) {
        FlowValue kept = value;
        kept.objects.subtract(m_objectsLettingInUnseenCode);
        return addTo(target, kept);
    }
    // The kinds of primitives that come with an unknown value would predict every kind.
    if (value.primitives && m_predictions.get(target.index())) {
        FlowValue objects;
        objects.objects = value.objects;
        return addTo(target, objects);
    }
    if (value.hasUnknownObject() && m_gates.get(target.index())) {
        // processNode() offers a gate what it offers any other node, so an edge gives one answer whoever pushes along it.
        FlowValue tracked = value;
        if (!m_predictions.get(target.index()) && listsNoMoreEscapedObjects(target, value))
            tracked.objects = withoutEscapedObjects(value.objects);
        tracked.objects.remove(unknownFlowObject);
        return addTo(target, tracked);
    }

    FlowObjectSet added;
    Subject subject = literalLimitSubject(m_graph, target, value);
    FlowJoinOutcome outcome = listsNoMoreEscapedObjects(target, value)
        ? values().joinExcluding(held, value, m_escapedObjects, subject, added)
        : values().join(held, value, subject, added);
    if (!outcome)
        return arrival;
    queueNode(target, WTF::move(added), outcome.didChangePrimitives);

    // A Map and a Set keep -0 as +0.
    if (outcome.didChangePrimitives && (value.primitives & Primitive::WholeNumber) && m_graph.holdsCollectionKeys(target))
        addTo(target, values().valueOfIntegers(Range::one(0)));
    return arrival;
}

bool FlowSolver::listsNoMoreEscapedObjects(FlowNodeID target, const FlowValue& arriving) const
{
    const FlowObjectSet& held = valueOf(target).objects;
    if (arriving.objects.isEmpty() || arriving.objects.onlyObject() == unknownFlowObject)
        return false;
    if (!held.containsUnknownObject() && !arriving.objects.containsUnknownObject())
        return false;
    // Which objects have reached unseen code is a fact about each of them: one that native code has may still reach unknown JavaScript.
    if (unseenCodeInPlaceOf(target))
        return false;
    // Beside the unknown object an escaped object is a prediction, and nobody waits for one. Running every rule again adds none.
    if (m_fixpointCheck) [[unlikely]]
        return true;
    return held.hasMoreObjectsThan(m_maxEscapedObjectsBesideUnknown) || arriving.objects.hasMoreObjectsThan(m_maxEscapedObjectsBesideUnknown);
}

FlowObjectSet FlowSolver::withoutEscapedObjects(const FlowObjectSet& objects) const
{
    FlowObjectSet remainingObjects = objects;
    remainingObjects.subtract(m_escapedObjects);
    if (objects.containsUnknownObject())
        remainingObjects.add(unknownFlowObject);
    return remainingObjects;
}

void FlowSolver::joinObject(FlowNodeID target, FlowObjectID object, const Witness& witness)
{
    join(target, values().valueOfObject(object), witness);
}

void FlowSolver::joinObjects(FlowNodeID target, const FlowObjectSet& objects, const Witness& witness)
{
    if (!target || objects.isEmpty())
        return;
    FlowValue value;
    value.objects = objects;
    join(target, value, witness);
}

void FlowSolver::joinPrimitives(FlowNodeID target, uint16_t primitives, const Witness& witness)
{
    join(target, values().valueOfPrimitives(primitives), witness);
}

void FlowSolver::joinUnknown(FlowNodeID target, const Witness& witness)
{
    join(target, values().unknownValue(), witness);
}

void FlowSolver::joinUnknownObject(FlowNodeID target, const Witness& witness)
{
    join(target, values().unknownObjectValue(), witness);
}

void FlowSolver::reach(FlowNodeID sink, const FlowObjectSet& objects, const Witness& witness)
{
    FlowObjectSet added;
    if (!m_graph.at(sink).value.objects.uniteAndCollect(objects, added))
        return;

    bool isJavaScript = sink == m_graph.unknownJavaScript();
    if (!isJavaScript)
        m_escapedObjects.add(added);
    ObjectMutation mutation = isJavaScript ? ObjectMutation::ReachUnknownJavaScript : ObjectMutation::ReachNativeCode;
    Deque<FlowObjectID>& reachingObjects = isJavaScript ? m_worklist.objectsReachingUnknownJavaScript : m_worklist.objectsReachingNativeCode;
    bool arrivesFromNativeCode = isJavaScript && !hasFunction(witness);
    added.forEach([&](FlowObjectID object) {
        if (object == unknownFlowObject)
            return;
        noteMutation(object, mutation, arrivesFromNativeCode ? m_records.witnessOf(object, ObjectMutation::ReachNativeCode) : witness);
        reachingObjects.append(object);
    });

    if (isJavaScript) {
        reach(m_graph.nativeCode(), added, witness);
        // The unknown object stands for whatever has reached native code.
        if (m_records.unseenCodeCanRun() && added.containsUnknownObject())
            connect(m_graph.nativeCode(), sink, { }, witness);
    }
    queueNode(sink, WTF::move(added), false);
}

void FlowSolver::connect(FlowNodeID from, FlowNodeID to, TypeID filter, const Witness& witness)
{
    if (!from || !to)
        return;
    // Whatever comes into an exposed location reaches native code, and the unknown value stands for all of that: the edge only predicts.
    if (m_fixpointCheck && m_graph.hasEdgeToUnseenCode(from) && values().isSubsetOf(values().unknownValue(), valueOf(to))) [[unlikely]]
        return;
    if (!m_graph.addEdge(from, to, filter))
        return;
    if (m_fixpointCheck) [[unlikely]]
        m_fixpointCheck->didAddEdge(from, to);

    const FlowValue& value = valueOf(from);
    if (holdsBackValuesOf(to)) {
        if (!value.isEmpty())
            m_quietSteps.heldBackValues.append({ to, filter ? values().filter(value, m_program.at(filter)) : value, witness, false });
        return;
    }
    if (filter)
        propagate(to, values().filter(value, m_program.at(filter)), witness);
    else
        propagate(to, value, witness);
}

auto FlowSolver::memberWaitingFor(FlowStatementSite site, FlowWakeUp wakeUp) const -> Member
{
    if (wakeUp == FlowWakeUp::Revisit && m_memberInProgress.cursor && m_memberInProgress.site == site)
        return m_memberInProgress;
    return { site, { }, { } };
}

void FlowSolver::waitOn(FlowNodeID node, FlowStatementSite site, FlowWakeUp wakeUp)
{
    if (!node || !site.instance)
        return;
    if (site.statement.kind == FlowStatementKind::Lookup) {
        RELEASE_ASSERT(wakeUp == FlowWakeUp::Revisit && m_graph.kindOf(node) == FlowNodeKind::PrototypeLink);
        m_waitsOfLookup->append(FlowLookupWait { node, m_graph.objectOf(node), { }, { } });
        return;
    }

    NodeWaiter& waiter = m_waiters.onNodes.ensure(node, memberWaitingFor(site, wakeUp), m_counters.changes);
    if (waiter.wakeUp >= wakeUp)
        return;
    waiter.wakeUp = wakeUp;
    ++m_counters.changes;
}

void FlowSolver::waitOnMutations(FlowObjectID object, FlowStatementSite site, OptionSet<ObjectMutation> relevant, FlowWakeUp wakeUp)
{
    if (!object || !site.instance || relevant.isEmpty())
        return;
    if (site.statement.kind == FlowStatementKind::Lookup) {
        RELEASE_ASSERT(wakeUp == FlowWakeUp::Revisit);
        m_waitsOfLookup->append(FlowLookupWait { { }, object, { }, relevant });
        return;
    }

    OptionSet<ObjectMutation>& known = m_waiters.onMutations.ensure(object, memberWaitingFor(site, wakeUp), m_counters.changes).mutationsFor(wakeUp);
    if (known.containsAll(relevant))
        return;
    ++m_counters.changes;
    known.add(relevant);
}

void FlowSolver::waitOnProperty(FlowObjectID object, PropertyKeyID key, FlowStatementSite site)
{
    if (!object || !key || !site.instance)
        return;
    if (site.statement.kind == FlowStatementKind::Lookup) {
        m_waitsOfLookup->append(FlowLookupWait { { }, object, key, { } });
        return;
    }

    m_waiters.onNamedProperties.ensure(std::pair { object, key }, memberWaitingFor(site, FlowWakeUp::Revisit), m_counters.changes);
}

void FlowSolver::waitOnProperties(FlowObjectID object, FlowStatementSite site)
{
    if (!object || !site.instance)
        return;
    if (site.statement.kind == FlowStatementKind::Lookup) {
        m_waitsOfLookup->append(FlowLookupWait { { }, object, { }, { } });
        return;
    }

    m_waiters.onProperties.ensure(object, memberWaitingFor(site, FlowWakeUp::Revisit), m_counters.changes);
}

void FlowSolver::listWaitsOfLookup(FlowLookupWaits* waits)
{
    RELEASE_ASSERT(!waits || !m_waitsOfLookup);
    m_waitsOfLookup = waits;
}

void FlowSolver::waitOn(const FlowLookupWait& wait, FlowStatementSite site)
{
    if (wait.node)
        waitOn(wait.node, site, FlowWakeUp::Revisit);
    else if (!wait.mutations.isEmpty())
        waitOnMutations(wait.object, site, wait.mutations);
    else if (wait.key)
        waitOnProperty(wait.object, wait.key, site);
    else
        waitOnProperties(wait.object, site);
}

void FlowSolver::growNodeTablesFor(FlowNodeID node)
{
    size_t size = m_worklist.pendingObjectsOfNode.size();
    if (node.index() < size)
        return;

    size_t grownSize = std::max<size_t>({ node.index() + 1, m_graph.numberOfNodes(), size * 2 });
    m_worklist.pendingObjectsOfNode.grow(grownSize);
    m_worklist.widelyReadNodes.ensureSize(grownSize);
    m_worklist.nodesWithNewPrimitives.ensureSize(grownSize);
    m_cursors.arrivalListOfNode.grow(grownSize);
    m_gates.ensureSize(grownSize);
    m_predictions.ensureSize(grownSize);
}

void FlowSolver::queueNode(FlowNodeID node, FlowObjectSet&& addedObjects, bool hasNewPrimitives)
{
    ++m_counters.changes;
    growNodeTablesFor(node);
    if (m_graph.kindOf(node) == FlowNodeKind::PrototypeLink)
        forgetAnswersOfLookupsThrough(*this, m_graph.objectOf(node));
    if (m_fixpointCheck) [[unlikely]]
        m_fixpointCheck->didChangeValue(node, addedObjects, hasNewPrimitives);

    size_t index = node.index();
    if (!addedObjects.isEmpty()) {
        if (ArrivalListID list = m_cursors.arrivalListOfNode[index]) {
            auto& arrivals = m_cursors.arrivalLists[list.index()].arrivals;
            addedObjects.forEach([&](FlowObjectID object) {
                arrivals.append(object);
            });
        }

        FlowObjectSet& pendingObjects = m_worklist.pendingObjectsOfNode[index];
        if (pendingObjects.isEmpty())
            pendingObjects = WTF::move(addedObjects);
        else
            pendingObjects.unite(addedObjects);
    }
    if (hasNewPrimitives)
        m_worklist.nodesWithNewPrimitives.quickSet(index);
    if (m_worklist.widelyReadNodes.quickGet(index))
        m_worklist.widelyReadNodesToProcess.add(index);
    else
        m_worklist.nodesToProcess.add(index);
}

uint32_t FlowSolver::firstStatementOf(FlowInstanceID instance)
{
    constexpr uint32_t notNumbered = UINT32_MAX;
    Vector<uint32_t>& firstStatementOfInstance = m_worklist.firstStatementOfInstance;
    size_t index = instance.index();
    if (index >= firstStatementOfInstance.size())
        firstStatementOfInstance.insertFill(firstStatementOfInstance.size(), notNumbered, std::max<size_t>({ index + 1, m_graph.numberOfInstances(), firstStatementOfInstance.size() * 2 }) - firstStatementOfInstance.size());

    uint32_t& first = firstStatementOfInstance[index];
    if (first == notNumbered) {
        first = m_worklist.numberOfSummaryStatements;
        m_worklist.numberOfSummaryStatements += m_graph.summaryOf(instance).numbering.numberOfStatements;
    }
    return first;
}

std::pair<BitVector&, size_t> FlowSolver::queuedBitOf(FlowStatementSite site)
{
    RELEASE_ASSERT(site.statement.kind != FlowStatementKind::Lookup);
    if (site.statement.kind == FlowStatementKind::BuiltinCall)
        return { m_worklist.queuedBuiltinCalls, site.statement.index };
    return { m_worklist.queuedSummaryStatements, firstStatementOf(site.instance) + summaryOf(site).numbering.numberOf(site.statement) };
}

void FlowSolver::queueStatement(FlowStatementSite site, BitVector& queuedStatements, size_t bit)
{
    ++m_counters.wakeUps;
    if (bit >= queuedStatements.size())
        queuedStatements.ensureSize(std::max<size_t>(bit + 1, queuedStatements.size() * 2));
    if (!queuedStatements.quickSet(bit))
        m_worklist.statementsToRun.append(site);
}

void FlowSolver::queueStatement(FlowStatementSite site)
{
    auto [queuedStatements, bit] = queuedBitOf(site);
    queueStatement(site, queuedStatements, bit);
}

void FlowSolver::runQueuedStatement(FlowStatementSite site)
{
    auto [queuedStatements, bit] = queuedBitOf(site);
    queuedStatements.quickClear(bit);

    Vector<FlowCursorID, 2> cursorsWithMembersToRepeat;
    if (!m_worklist.cursorsWithMembersToRepeat.isEmpty())
        cursorsWithMembersToRepeat = m_worklist.cursorsWithMembersToRepeat.take(site);
    run(site);

    if (m_graph.at(site.instance).state == FlowInstanceState::PastWorkLimit)
        return;

    // A cursor inside a plain loop over what another cursor handed out is reached only when that one hands out again.
    bool hasReachedEveryCursor = std::ranges::all_of(cursorsWithMembersToRepeat, [&](FlowCursorID cursor) {
        return m_cursors.all[cursor.index()].lastRun == m_runNumber;
    });
    if (hasReachedEveryCursor)
        return;
    ++m_counters.cursorsNotReached;
    revisit(site);
}

void FlowSolver::revisit(FlowStatementSite site)
{
    if (isHostCallPastWorkLimit(site))
        return;

    queueStatement(site);
    if (startOverCursorsOf(site))
        ++m_counters.statementsStartedOver;
}

void FlowSolver::revisit(const Waiter& waiter)
{
    if (m_losesWakeUps) [[unlikely]]
        return;
    bringBack(waiter);
}

void FlowSolver::bringBack(const Waiter& waiter)
{
    if (!waiter.cursor) {
        revisit(waiter.site);
        return;
    }
    if (isHostCallPastWorkLimit(waiter.site))
        return;

    queueStatement(waiter.site);
    Cursor& outer = m_cursors.all[waiter.cursor.index()];
    bool hasStartedOver = !outer.numberOfArrivalsTaken;
    if (hasStartedOver)
        return;

    FlowObjectSet membersToRepeat;
    if (!outer.membersToRepeat.uniteAndCollect(waiter.members, membersToRepeat))
        return;
    m_counters.membersRepeated += membersToRepeat.size();
    Vector<FlowCursorID, 2>& cursorsWithMembersToRepeat = m_worklist.cursorsWithMembersToRepeat.add(waiter.site, Vector<FlowCursorID, 2> { }).iterator->value;
    if (!cursorsWithMembersToRepeat.contains(waiter.cursor))
        cursorsWithMembersToRepeat.append(waiter.cursor);

    if (!outer.hasCursorsInsideMembers)
        return;
    membersToRepeat.forEach([&](FlowObjectID member) {
        auto inner = m_cursors.takenInsideMember.find(std::pair { waiter.cursor, member });
        if (inner == m_cursors.takenInsideMember.end())
            return;
        for (FlowCursorID cursor : inner->value)
            startOver(cursor);
    });
}

bool FlowSolver::startOver(FlowCursorID id)
{
    Cursor& cursor = m_cursors.all[id.index()];
    bool hasStartedOver = cursor.primitivesTaken || cursor.numberOfArrivalsTaken;
    cursor.primitivesTaken = 0;
    cursor.numberOfArrivalsTaken = 0;
    cursor.membersToRepeat.clear();
    return hasStartedOver;
}

bool FlowSolver::startOverCursorsOf(FlowStatementSite site)
{
    bool hasStartedOver = false;
    size_t instance = site.instance.index();
    bool hasNumberedCursors = instance < m_cursors.firstOfInstance.size() && m_cursors.firstOfInstance[instance];
    if (hasNumberedCursors) {
        for (uint32_t cursor = 0; cursor < numberOfCursorsOf(site.statement.kind); ++cursor)
            hasStartedOver |= startOver(numberedCursor(site, cursor));
    }

    forEachKeyedCursorOf(site, [&](FlowCursorID cursor) {
        hasStartedOver |= startOver(cursor);
    });
    return hasStartedOver;
}

void FlowSolver::processNode(FlowNodeID current)
{
    size_t index = current.index();
    FlowValue pushed;
    bool hasNewPrimitives = m_worklist.nodesWithNewPrimitives.quickClear(index);
    if (hasNewPrimitives)
        pushed = valueOf(current).withoutObjects();
    pushed.objects = std::exchange(m_worklist.pendingObjectsOfNode[index], FlowObjectSet());

    // A node that came to be widely read while it was queued is in both queues.
    if (pushed.isEmpty())
        return;

    ++m_counters.nodeSteps;
    charge(m_graph.functionChargedFor(current), 1);
    if (m_fixpointCheck) [[unlikely]]
        m_fixpointCheck->willProcess(current);

    constexpr unsigned fewestReadersOfWidelyReadNode = 32;
    unsigned numberOfReaders = 0;

    std::optional<Witness> witness;
    auto push = [&](FlowNodeID target, const FlowValue& value) {
        FlowNodeID unseenCode = unseenCodeInPlaceOf(target);
        if (!unseenCode) {
            addTo(target, value);
            return;
        }
        if (!witness)
            witness = witnessOfNode(current);
        reach(unseenCode, value.objects, *witness);
    };

    // What reaches native code from here does so before the other successors are offered it.
    if (m_graph.hasEdgeToUnseenCode(current)) {
        m_graph.forEachEdge(current, [&](const FlowEdge& edge) {
            if (edge.target == m_graph.nativeCode() || edge.target == m_graph.unknownJavaScript())
                push(edge.target, edge.filter ? values().filter(pushed, m_program.at(edge.filter)) : pushed);
        });
    }

    std::optional<FlowValue> pushedWithoutEscapedObjects;
    auto valueFor = [&](FlowNodeID target) -> const FlowValue& {
        if (!listsNoMoreEscapedObjects(target, pushed))
            return pushed;
        if (!pushedWithoutEscapedObjects) {
            pushedWithoutEscapedObjects = pushed.withoutObjects();
            pushedWithoutEscapedObjects->objects = withoutEscapedObjects(pushed.objects);
        }
        return *pushedWithoutEscapedObjects;
    };

    m_graph.forEachSuccessor(current, [&](FlowNodeID target, TypeID filter) {
        ++numberOfReaders;
        uint64_t changesBefore = m_counters.changes;
        if (filter)
            push(target, values().filter(valueFor(target), m_program.at(filter)));
        else if (m_graph.isUnseenCode(target) || m_graph.hasInstancesPastWorkLimit() || !pushed.objects.isSubsetOf(valueOf(target).objects) || !bringsNoPrimitives(pushed, valueOf(target)))
            push(target, valueFor(target));
        ++m_counters.valuesPushedAlongEdges;
        m_counters.valuesPushedWithoutChange += m_counters.changes == changesBefore;
    });

    FlowNodeKind kind = m_graph.kindOf(current);
    ++m_counters.nodeStepsOfKind[static_cast<size_t>(kind)];
    if (kind == FlowNodeKind::Local) {
        FlowInstanceID instance = m_graph.instanceOf(current);
        const FlowSummary& summary = m_graph.summaryOf(instance);
        std::span<const FlowStatementReference> statementsToWake = summary.statementsToWake.of(m_graph.localOf(current));
        if (!statementsToWake.empty()) {
            uint32_t firstStatement = firstStatementOf(instance);
            for (FlowStatementReference statement : statementsToWake)
                queueStatement(FlowStatementSite { instance, statement }, m_worklist.queuedSummaryStatements, firstStatement + summary.numbering.numberOf(statement));
        }
    }

    // Only the run of a statement adds a waiter, and none runs here.
    bool isUnseenCode = kind == FlowNodeKind::NativeCode || kind == FlowNodeKind::UnknownJavaScript;
    auto wake = [&](const NodeWaiter& waiter) {
        ++numberOfReaders;
        FlowStatementSite site = waiter.site;
        // The statements of an abandoned instance no longer run, and its code does to a late arrival whatever JavaScript can do.
        if (m_graph.at(site.instance).state == FlowInstanceState::PastWorkLimit) {
            if (!isUnseenCode)
                reach(m_graph.unknownJavaScript(), pushed.objects, witnessAt(site.instance, ValueID()));
            return;
        }
        switch (waiter.wakeUp) {
        case FlowWakeUp::ContinueAtUnknownObject:
            if (pushed.objects.containsUnknownObject())
                queueStatement(site);
            return;
        case FlowWakeUp::ContinueAtObject:
            if (!pushed.objects.isEmpty())
                queueStatement(site);
            return;
        case FlowWakeUp::Continue:
            if (!waiter.cursor || hasNewPrimitives || hasObjectsToTake(waiter.cursor))
                queueStatement(site);
            return;
        case FlowWakeUp::Revisit:
            revisit(waiter);
            return;
        }
    };
    m_waiters.onNodes.forEachWaiterOn(current, wake);

    if (numberOfReaders >= fewestReadersOfWidelyReadNode)
        m_worklist.widelyReadNodes.quickSet(index);
}

FlowCursorID FlowSolver::numberedCursor(FlowStatementSite site, uint32_t cursorInStatement)
{
    const FlowStatementNumbering& numbering = summaryOf(site).numbering;
    size_t instance = site.instance.index();
    if (instance >= m_cursors.firstOfInstance.size())
        m_cursors.firstOfInstance.grow(std::max<size_t>({ instance + 1, m_graph.numberOfInstances(), m_cursors.firstOfInstance.size() * 2 }));

    FlowCursorID& first = m_cursors.firstOfInstance[instance];
    if (!first) {
        first = FlowCursorID(m_cursors.all.size());
        m_cursors.all.grow(m_cursors.all.size() + numbering.numberOfCursors);
    }
    return FlowCursorID(first.index() + numbering.cursorOf(site.statement, cursorInStatement));
}

FlowCursorID FlowSolver::cursorFor(FlowStatementSite site, uint32_t cursorInStatement, FlowNodeID node)
{
    FlowCursorID cursor = numberedCursor(site, cursorInStatement);
    FlowNodeID nodeOfCursor = m_cursors.all[cursor.index()].node;
    if (nodeOfCursor && nodeOfCursor != node)
        return cursorFor(FlowTemporaryKey { site, FlowTemporaryOwner::Core, static_cast<uint16_t>(cursorInStatement), 0 }, node);
    return cursor;
}

FlowCursorID FlowSolver::cursorFor(const FlowTemporaryKey& key, FlowNodeID node)
{
    RELEASE_ASSERT(key.site.instance);
    Vector<KeyedCursor>& keyed = m_cursors.keyed;
    uint32_t hash = hashOfNumbers(hashOf(key), node.index());
    std::optional<uint32_t> existing = m_cursors.keyedByKeyAndNode.find(hash, [&](uint32_t candidate) {
        return keyed[candidate].node == node && keyed[candidate].key == key;
    });
    if (existing)
        return keyed[*existing].cursor;

    FlowCursorID cursor(m_cursors.all.size());
    m_cursors.all.grow(m_cursors.all.size() + 1);

    uint32_t position = keyed.size();
    m_cursors.keyedByKeyAndNode.add(hash, position);
    keyed.append(KeyedCursor { key, node, cursor, 0 });
    uint32_t hashOfSite = hashOf(key.site);
    std::optional<uint32_t> first = m_cursors.firstKeyedOfStatement.find(hashOfSite, [&](uint32_t candidate) {
        return keyed[candidate].key.site == key.site;
    });
    if (first)
        keyed[position].nextOfStatement = std::exchange(keyed[*first].nextOfStatement, position);
    else
        m_cursors.firstKeyedOfStatement.add(hashOfSite, position);

    m_waiters.onNodes.ensure(node, Member { key.site, cursor, { } }, m_counters.changes).wakeUp = FlowWakeUp::Continue;
    return cursor;
}

bool FlowSolver::hasObjectsToTake(FlowCursorID id) const
{
    const Cursor& cursor = m_cursors.all[id.index()];
    if (!cursor.node)
        return true;
    return cursor.numberOfArrivalsTaken != m_cursors.arrivalLists[m_cursors.arrivalListOfNode[cursor.node.index()].index()].numberOfArrivals();
}

void FlowSolver::noteTakeInsideMember(FlowCursorID cursor, FlowStatementSite site)
{
    if (!m_memberInProgress.cursor || m_memberInProgress.cursor == cursor || m_memberInProgress.site != site)
        return;
    m_cursors.all[m_memberInProgress.cursor.index()].hasCursorsInsideMembers = true;
    Vector<FlowCursorID, 1>& inner = m_cursors.takenInsideMember.add(std::pair { m_memberInProgress.cursor, m_memberInProgress.object }, Vector<FlowCursorID, 1> { }).iterator->value;
    if (!inner.contains(cursor))
        inner.append(cursor);
}

FlowNewObjects FlowSolver::takeObjects(FlowCursorID id, FlowStatementSite site, FlowNodeID node)
{
    noteTakeInsideMember(id, site);

    const ArrivalList& arrivals = ensureArrivalList(node);
    Cursor& cursor = m_cursors.all[id.index()];
    cursor.node = node;
    cursor.lastRun = m_runNumber;

    uint32_t numberOfArrivalsTaken = std::exchange(cursor.numberOfArrivalsTaken, arrivals.numberOfArrivals());
    if (!numberOfArrivalsTaken)
        return { FlowObjectSet(valueOf(node).objects), site, id };

    FlowObjectSet objects = std::exchange(cursor.membersToRepeat, FlowObjectSet());
    objects.add(arrivals.arrivalsAfter(numberOfArrivalsTaken));
    return { WTF::move(objects), site, id };
}

auto FlowSolver::ensureArrivalList(FlowNodeID node) -> const ArrivalList&
{
    growNodeTablesFor(node);
    ArrivalListID& list = m_cursors.arrivalListOfNode[node.index()];
    if (list)
        return m_cursors.arrivalLists[list.index()];

    list = ArrivalListID(m_cursors.arrivalLists.size());
    m_cursors.arrivalLists.append(ArrivalList { { }, static_cast<uint32_t>(valueOf(node).objects.size()) });
    return m_cursors.arrivalLists.last();
}

bool FlowSolver::takePrimitives(FlowCursorID id, FlowStatementSite site, FlowNodeID node)
{
    noteTakeInsideMember(id, site);

    Cursor& cursor = m_cursors.all[id.index()];
    uint16_t primitives = valueOf(node).primitives;
    if (!(primitives & ~cursor.primitivesTaken))
        return false;
    cursor.primitivesTaken |= primitives;
    return true;
}

FlowNewObjects FlowSolver::takeNewObjects(FlowStatementSite site, uint32_t cursorInStatement, FlowNodeID node)
{
    if (!node)
        return { };
    return takeObjects(cursorFor(site, cursorInStatement, node), site, node);
}

FlowNewObjects FlowSolver::takeNewObjects(const FlowTemporaryKey& key, FlowNodeID node)
{
    if (!node)
        return { };
    return takeObjects(cursorFor(key, node), key.site, node);
}

bool FlowSolver::takeNewPrimitives(FlowStatementSite site, uint32_t cursorInStatement, FlowNodeID node)
{
    return node && takePrimitives(cursorFor(site, cursorInStatement, node), site, node);
}

bool FlowSolver::takeNewPrimitives(const FlowTemporaryKey& key, FlowNodeID node)
{
    return node && takePrimitives(cursorFor(key, node), key.site, node);
}

FlowNodeID FlowSolver::temporaryFor(const FlowTemporaryKey& key)
{
    RELEASE_ASSERT(key.site.instance);
    uint32_t hash = hashOf(key);
    std::optional<uint32_t> existing = m_temporaries.byKey.find(hash, [&](uint32_t candidate) {
        return m_temporaries.all[candidate].key == key;
    });
    if (existing)
        return m_temporaries.all[*existing].node;

    m_temporaries.byKey.add(hash, m_temporaries.all.size());
    m_temporaries.all.append(Temporary { key, m_graph.addTemporary(key.site.instance) });
    return m_temporaries.all.last().node;
}

FlowNodeID FlowSolver::gateFor(const FlowTemporaryKey& key)
{
    FlowNodeID gate = temporaryFor(key);
    growNodeTablesFor(gate);
    m_gates.quickSet(gate.index());
    return gate;
}

FlowNodeID FlowSolver::predictionFor(const FlowTemporaryKey& key)
{
    FlowNodeID prediction = gateFor(key);
    m_predictions.quickSet(prediction.index());
    return prediction;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
