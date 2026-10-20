/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// What a cursor has handed out to a request has to come out again to be linked with plain edges. A call that its statement makes at every run enters the generic instance by itself,
// and a built-in, a bound function or the values of a node are statements with a context of their own.
static bool hasCalleesFromCursors(const FlowCallLinkState& state, FlowStatementSite site)
{
    auto last = state.lastLinkedRequestOfSite.find(site);
    if (last == state.lastLinkedRequestOfSite.end())
        return false;

    std::optional<uint32_t> index = last->value;
    while (index) {
        const FlowLinkedRequest& linkedRequest = state.linkedRequests[*index];
        if (!linkedRequest.closures.isEmpty() || linkedRequest.hasUnknownCallee)
            return true;
        index = linkedRequest.previousOfSite;
    }
    return false;
}

FlowContext contextOf(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, const FlowObjectSet& callees)
{
    HashSet<FlowStatementSite>& genericSites = solver.callState().links.genericSites;
    if (genericSites.contains(request.site))
        return FlowContext::Generic;

    auto isTracked = [&](FlowNodeID node) {
        return !node || !solver.valueOf(node).hasUnknownObject();
    };
    auto isTrackedOperand = [&](FlowObjectID exactObject, FlowNodeID node) {
        return exactObject ? exactObject != unknownFlowObject : isTracked(node);
    };

    const FlowCallOperands& operands = request.operands;
    bool isProven = !callees.containsUnknownObject()
        && isTrackedOperand(operands.thisObject, operands.thisValue)
        && isTrackedOperand(operands.newTargetObject, operands.newTarget)
        && std::ranges::all_of(arguments.positions, isTracked)
        && isTracked(arguments.rest);
    if (isProven) {
        // An operand that no parameter takes goes through no gate, and the answer must not depend on whether the unknown object or the callee comes first.
        auto waitForUnknownObject = [&](FlowNodeID node) {
            solver.waitOn(node, request.site, FlowWakeUp::ContinueAtUnknownObject);
        };
        waitForUnknownObject(operands.thisValue);
        waitForUnknownObject(operands.newTarget);
        for (FlowNodeID position : arguments.positions)
            waitForUnknownObject(position);
        waitForUnknownObject(arguments.rest);
        return solver.graph().at(request.site.instance).context == FlowContext::Proven ? FlowContext::Proven : FlowContext::ProvenUnderGeneric;
    }

    genericSites.add(request.site);
    if (hasCalleesFromCursors(solver.callState().links, request.site))
        solver.revisit(request.site);
    return FlowContext::Generic;
}

bool repeatsOnContextChain(const FlowGraph& graph, FlowInstanceID caller, FunctionID callee)
{
    FlowInstanceID instance = caller;
    while (instance) {
        const FlowInstance& current = graph.at(instance);
        if (current.function == callee)
            return true;
        instance = current.callSite ? current.callSite->outer : FlowInstanceID();
    }
    return false;
}

// The flags of the operation that calls the function itself. What a built-in calls for the operation, an accessor that its lookup runs, has none.
static OptionSet<FlowCallFlag> flagsOfOperation(FlowSolver& solver, const FlowCallRequest& request)
{
    if (!request.site.instance || (request.kind != FlowCallLinkKind::Call && request.kind != FlowCallLinkKind::Construct))
        return { };

    // The call of a member waits on the member in a statement of the solver.
    FlowStatementSite site = request.site;
    if (site.statement.kind == FlowStatementKind::BuiltinCall)
        site = statementAt(solver.callState().builtins, site).key.summaryStatement;

    const FlowStatements& statements = solver.graph().summaryOf(site.instance).statements;
    switch (site.statement.kind) {
    case FlowStatementKind::Call: {
        const FlowCall& call = statements.calls[site.statement.index];
        return call.origin == request.origin ? call.flags : OptionSet<FlowCallFlag>();
    }
    case FlowStatementKind::DirectCall: {
        const FlowDirectCall& call = statements.directCalls[site.statement.index];
        return call.origin == request.origin ? call.flags : OptionSet<FlowCallFlag>();
    }
    default:
        return { };
    }
}

// What a caller stores on an object that a function has just made for it says what kind of object it is, and two callers make two kinds. The function may have the object
// from a function it calls: the question goes down the calls whose result is handed up, and nowhere else.
static bool isAskedForNewObject(FlowSolver& solver, const FlowFunctionCall& call)
{
    const FlowGraph& graph = solver.graph();
    // FIXME: Off until placement and the run time know derived layouts.
    static const bool isSwitchedOn = !!getenv("BUN_AOT_DERIVED_LAYOUTS");
    if (!isSwitchedOn)
        return false;

    OptionSet<FlowCallFlag> flags = flagsOfOperation(solver, call.request);
    // A constructor fills in the new object, and what it is handed is often what tells the kinds apart later.
    bool makesNewObject = call.request.kind == FlowCallLinkKind::Construct || graph.summaryOf(call.function).flags.contains(FlowSummaryFlag::HandsUpNewObject);
    if (flags.contains(FlowCallFlag::ResultGetsProperties) && makesNewObject)
        return true;
    // A function on the way may only count the object or mark it and give it back.
    const FlowInstance& caller = graph.at(call.request.site.instance);
    return flags.contains(FlowCallFlag::ResultIsHandedUp) && caller.callSite && caller.callSite->isAskedForNewObject;
}

bool deservesCallSiteInstance(FlowSolver& solver, const FlowFunctionCall& call)
{
    const FlowSummary& summary = solver.graph().summaryOf(call.function);
    if (!summary.hasBody())
        return false;

    bool isAlwaysAtCallSite = call.policy == FlowInstancePolicy::AlwaysAtCallSite;
    bool isAsked = isAskedForNewObject(solver, call);
    // What runs under a generic caller is kept apart from what proven callers pass, not from itself. What it allocates for this caller is the exception:
    // the operation enters one version of the callee whichever version of the caller runs.
    if (!isAlwaysAtCallSite && !isAsked && call.context == FlowContext::ProvenUnderGeneric)
        return false;
    if (!isAlwaysAtCallSite && !isAsked && !summary.flags.contains(FlowSummaryFlag::PassesParameterOn))
        return false;
    if (repeatsOnContextChain(solver.graph(), call.request.site.instance, call.function))
        return false;
    return isAlwaysAtCallSite || !solver.build().exceeds(LimitID::MaxFlowInstanceValues, summary.numberOfValues, Subject { .function = call.function });
}

// Helpers that call helpers multiply their instances with each level. Below the limit the count starts again: the callee has one instance per call site of the function that calls it.
// A chain of functions that each hand up what the next one made is as long as the program wrote it, and only its top says what the object is for.
static bool canHoldNestedInstances(FlowSolver& solver, FlowInstanceID instance, bool isAskedForNewObject)
{
    const FlowGraph& graph = solver.graph();
    unsigned depth = 0;
    while (instance && graph.at(instance).callSite) {
        ++depth;
        instance = graph.at(instance).callSite->outer;
    }
    return depth && depth < solver.build().limit(isAskedForNewObject ? LimitID::MaxNestedInstancesOfNewObjects : LimitID::MaxNestedCallSiteInstances);
}

FlowInstanceID chooseInstance(FlowSolver& solver, const FlowFunctionCall& call)
{
    if (call.context == FlowContext::Generic)
        return solver.enter(call.function, FlowContext::Generic);

    if (deservesCallSiteInstance(solver, call)) {
        FlowInstanceID callerInstance = call.request.site.instance;
        const FlowInstance& caller = solver.graph().at(callerInstance);
        bool isAsked = isAskedForNewObject(solver, call);
        bool isNested = canHoldNestedInstances(solver, callerInstance, isAsked);
        // An instance that all instances of the caller share is asked only what the caller itself asks.
        if (!isNested)
            isAsked = isAsked && flagsOfOperation(solver, call.request).contains(FlowCallFlag::ResultGetsProperties);
        FlowCallSiteKey key { caller.function, call.request.origin, isNested ? callerInstance : FlowInstanceID(), call.context, isAsked };
        if (FlowInstanceID instance = solver.enterAtCallSite(call.function, key))
            return instance;
    }
    return solver.enter(call.function, call.context);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
