/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowSolver.h"

#if ENABLE(AOT)

#include "AOTFlowCallOperations.h"
#include "AOTFlowCallState.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static Subject subjectOf(const Program& program, FunctionID function)
{
    return { .function = function, .location = program.at(function).declarationLocation() };
}

static FlowObjectSet collectObjects(const FlowGraph& graph, FlowInstanceID instance)
{
    const FlowSummary& summary = graph.summaryOf(instance);
    FlowObjectSet objects;
    for (uint32_t site = 0; site < summary.sites.size(); ++site)
        objects.add(graph.objectOf(instance, site));
    for (uint32_t index = 0; index < summary.numberOfLocals; ++index)
        objects.unite(graph.valueOf(graph.nodeOf(instance, FlowLocalID(index))).objects);
    return objects;
}

void FlowSolver::charge(FlowStatementSite site, uint64_t steps)
{
    FunctionID function = m_graph.at(site.instance).function;
    if (m_graph.isHost(function))
        chargeHostCall(site, steps);
    else
        charge(function, steps);
}

// The host allocates the hoisted closures of every module, so abandoning it would widen every function of the program. Each call that native code makes gives up alone.
void FlowSolver::chargeHostCall(FlowStatementSite site, uint64_t steps)
{
    if (site.statement.kind != FlowStatementKind::BuiltinCall || m_fixpointCheck)
        return;

    uint64_t& work = m_workByHostCall.add(site, 0).iterator->value;
    bool wasWithinLimit = work <= m_maxWorkPerFunction;
    work += steps;
    if (!wasWithinLimit || work <= m_maxWorkPerFunction)
        return;

    m_build.gaveUp(LimitID::MaxFlowWorkPerFunction, Subject { });
    m_worklist.hostCallsToAbandon.append(site);
}

bool FlowSolver::isHostCallPastWorkLimit(FlowStatementSite site) const
{
    if (site.statement.kind != FlowStatementKind::BuiltinCall || !m_graph.isHost(site.instance))
        return false;
    auto work = m_workByHostCall.find(site);
    return work != m_workByHostCall.end() && work->value > m_maxWorkPerFunction;
}

bool FlowSolver::isAboutToBeAbandoned(FlowStatementSite site) const
{
    if (m_worklist.functionsToAbandon.isEmpty() && m_worklist.hostCallsToAbandon.isEmpty())
        return false;

    FunctionID function = m_graph.at(site.instance).function;
    for (FunctionID queued : m_worklist.functionsToAbandon) {
        if (queued == function)
            return true;
    }
    for (FlowStatementSite queued : m_worklist.hostCallsToAbandon) {
        if (queued == site)
            return true;
    }
    return false;
}

void FlowSolver::charge(FunctionID function, uint64_t steps)
{
    if (!function || m_graph.isHost(function) || m_fixpointCheck)
        return;

    uint64_t& work = m_workByFunction[function.index()];
    work += steps;
    if (work <= m_maxWorkPerFunction)
        return;
    if (!m_records.addFunctionPastWorkLimit(function))
        return;

    m_build.gaveUp(LimitID::MaxFlowWorkPerFunction, subjectOf(m_program, function));
    m_worklist.functionsToAbandon.append(function);
}

void FlowSolver::abandon(FunctionID function)
{
    Witness witness = witnessOf(subjectOf(m_program, function));
    raiseUnseenCode(*this, UnseenCodeCause::LimitReached, BuiltinID(), witness);

    FlowObjectSet sharedObjects;
    for (uint32_t index = 0, count = m_objects.numberOfSitesOf(function); index < count; ++index)
        sharedObjects.add(m_objects.at(m_objects.siteOf(function, index)).sharedObject);
    reach(m_graph.unknownJavaScript(), sharedObjects, witness);

    Vector<FlowInstanceID, 8> instances;
    m_graph.forEachInstanceOf(function, [&](FlowInstanceID instance) {
        instances.append(instance);
    });
    for (FlowInstanceID instance : instances) {
        if (m_graph.at(instance).state == FlowInstanceState::Initialized)
            abandon(instance);
    }
    reachObjectsWaitedOnBy(function, 0, witness);
}

// An object that arrives later in a node that a statement waits on goes to the sink. Whether an object is early or late depends on the order of the worklist, so the early ones go there too.
void FlowSolver::reachObjectsWaitedOnBy(FunctionID function, size_t firstWaiter, const Witness& witness)
{
    FlowObjectSet objectsRead;
    for (const auto& waiter : m_waiters.onNodes.waiters.span().subspan(firstWaiter)) {
        FlowNodeID node = waiter.watched;
        if (node == m_graph.nativeCode() || node == m_graph.unknownJavaScript())
            continue;
        if (waiter.site.instance && m_graph.at(waiter.site.instance).function == function)
            objectsRead.unite(valueOf(node).objects);
    }
    reach(m_graph.unknownJavaScript(), objectsRead, witness);
}

void FlowSolver::abandon(FlowInstanceID instance)
{
    const FlowSummary& summary = m_graph.summaryOf(instance);
    Witness witness = witnessAt(instance, ValueID());

    reach(m_graph.unknownJavaScript(), collectObjects(m_graph, instance), witness);

    for (uint32_t index = 0; index < summary.numberOfLocals; ++index) {
        FlowNodeID local = m_graph.nodeOf(instance, FlowLocalID(index));
        TypeID type = m_graph.typeAsRead(local);
        join(local, type ? values().valueOfType(type) : values().unknownValue(), witness);
    }

    // What an operation of the function has produced may still be on its way to the operations that use it. The definition of a class and of a member is recorded
    // once the closure of the class has reached it, and whoever reads the records takes them for the whole definition.
    for (bool hasPushedValues = true; hasPushedValues;) {
        hasPushedValues = false;
        for (uint32_t index = 0; index < summary.numberOfLocals; ++index) {
            FlowNodeID local = m_graph.nodeOf(instance, FlowLocalID(index));
            growNodeTablesFor(local);
            if (m_worklist.pendingObjectsOfNode[local.index()].isEmpty() && !m_worklist.nodesWithNewPrimitives.quickGet(local.index()))
                continue;
            processNode(local);
            hasPushedValues = true;
        }
    }

    // The last run sees every member of every operand again. A loop that stopped when the limit was passed had taken members from its cursor without handling them,
    // the unknown object among them, and what was linked through gates is linked again with plain edges, into the generic instance.
    size_t firstWaiterOfLastRun = m_waiters.onNodes.waiters.size();
    m_isInLastRunOfInstance = true;
    summary.statements.forEach([&](FlowStatementReference statement) {
        FlowStatementSite site { instance, statement };
        startOverCursorsOf(site);
        run(site);
    });
    abandonBuiltinCallsOf(*this, instance);
    m_isInLastRunOfInstance = false;

    // Until the state changes, join() adds to a local without reaching the sink: this covers what the last run added.
    reach(m_graph.unknownJavaScript(), collectObjects(m_graph, instance), witness);

    // The same for what a statement reads through a cursor of its own, which needs no waiter on a local.
    reachObjectsWaitedOnBy(summary.function, firstWaiterOfLastRun, witness);
    FlowObjectSet objectsRead;
    auto addObjectsReadThrough = [&](FlowCursorID cursor) {
        FlowNodeID node = m_cursors.all[cursor.index()].node;
        if (node && node != m_graph.nativeCode() && node != m_graph.unknownJavaScript())
            objectsRead.unite(valueOf(node).objects);
    };
    auto addObjectsReadBy = [&](FlowStatementSite site) {
        bool hasNumberedCursors = instance.index() < m_cursors.firstOfInstance.size() && m_cursors.firstOfInstance[instance.index()];
        if (hasNumberedCursors) {
            for (uint32_t cursor = 0; cursor < numberOfCursorsOf(site.statement.kind); ++cursor)
                addObjectsReadThrough(numberedCursor(site, cursor));
        }
        forEachKeyedCursorOf(site, addObjectsReadThrough);
    };
    summary.statements.forEach([&](FlowStatementReference statement) {
        addObjectsReadBy({ instance, statement });
    });
    const auto& builtinCalls = callState().builtins.statementsByInstance;
    if (auto found = builtinCalls.find(instance); found != builtinCalls.end()) {
        for (uint32_t index : found->value)
            addObjectsReadBy({ instance, { FlowStatementKind::BuiltinCall, index } });
    }
    reach(m_graph.unknownJavaScript(), objectsRead, witness);

    m_graph.setState(instance, FlowInstanceState::PastWorkLimit);
}

void FlowSolver::abandonHostCall(FlowStatementSite site)
{
    raiseUnseenCode(*this, UnseenCodeCause::LimitReached, builtinCalledAt(*this, site), noWitness);
    abandonBuiltinCall(*this, site);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
