/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTTypeRelations.h"
#include <optional>
#include <span>
#include <utility>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static size_t callObjectsWith(FlowSolver&, const FlowCallRequest&, const FlowPassedArguments&, const FlowNewObjects& callees);

FlowCallLinkKind linkKindOf(FlowCallMode mode)
{
    switch (mode) {
    case FlowCallMode::Call:
        return FlowCallLinkKind::Call;
    case FlowCallMode::Construct:
    case FlowCallMode::SuperConstruct:
        return FlowCallLinkKind::Construct;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinCallMode builtinCallModeOf(FlowCallLinkKind kind)
{
    switch (kind) {
    case FlowCallLinkKind::Construct:
        return BuiltinCallMode::Construct;
    case FlowCallLinkKind::Call:
    case FlowCallLinkKind::Accessor:
    case FlowCallLinkKind::BuiltinCallback:
    case FlowCallLinkKind::Protocol:
    case FlowCallLinkKind::Host:
        return BuiltinCallMode::Call;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowCallRequest requestOf(FlowSolver& solver, FlowStatementSite site, const FlowCall& call, std::span<const FlowNodeID> arguments)
{
    return {
        site,
        call.origin,
        linkKindOf(call.mode),
        BuiltinCallbackTimes::Once,
        call.flags.contains(FlowCallFlag::IsInCycle),
        {
            .thisValue = solver.node(site, call.thisValue),
            .newTarget = solver.node(site, call.newTarget),
            .arguments = arguments,
            .argumentList = solver.node(site, call.argumentList),
            .result = solver.node(site, call.result),
            .thrown = solver.node(site, call.thrown),
        },
    };
}

static bool isSuperConstruct(const FlowSolver& solver, const FlowCallRequest& request)
{
    if (!request.origin)
        return false;
    Opcode opcode = solver.operationAt(request.site, request.origin).opcode;
    return opcode == Opcode::SuperConstruct || opcode == Opcode::SuperConstructList;
}

static void noteOtherNewTarget(FlowSolver& solver, const FlowCallRequest& request, FlowObjectID constructor)
{
    solver.noteEvent(constructor, FlowObjectEvent::OtherNewTarget, solver.witnessAt(request.site, request.origin));
}

// A super call passes the new.target of a derived class on, so there a closure is never another new.target.
static void noteNewTargetsOf(FlowSolver& solver, const FlowCallRequest& request, const FlowLinkedRequest& linkedRequest, FlowObjectID constructor)
{
    bool hasOtherNewTarget = linkedRequest.everyConstructorHasOtherNewTarget || (linkedRequest.onlyNewTarget && linkedRequest.onlyNewTarget != constructor);
    if (FlowObjectID newTarget = request.operands.newTargetObject)
        hasOtherNewTarget = solver.objects().kindOf(newTarget) != FlowObjectKind::Closure || (newTarget != constructor && !isSuperConstruct(solver, request));
    if (hasOtherNewTarget)
        noteOtherNewTarget(solver, request, constructor);
}

// One closure is another new.target for every constructor but itself. Two closures, or anything that is no closure, are one for every constructor.
static void noteNewTarget(FlowSolver& solver, const FlowCallRequest& request, FlowLinkedRequest& linkedRequest, FlowObjectID newTarget)
{
    if (linkedRequest.everyConstructorHasOtherNewTarget)
        return;

    bool isClosure = solver.objects().kindOf(newTarget) == FlowObjectKind::Closure;
    if (isClosure && (newTarget == linkedRequest.onlyNewTarget || isSuperConstruct(solver, request)))
        return;

    if (isClosure && !linkedRequest.onlyNewTarget)
        linkedRequest.onlyNewTarget = newTarget;
    else
        linkedRequest.everyConstructorHasOtherNewTarget = true;

    linkedRequest.closures.forEach([&](FlowObjectID constructor) {
        if (linkedRequest.everyConstructorHasOtherNewTarget || constructor != newTarget)
            noteOtherNewTarget(solver, request, constructor);
    });
}

// A class with a heritage has no allocating operation of its own. The constructor at the top of its chain allocates, and one nobody sees may also keep the object.
// A layout without a heritage cannot stand for an instance of a built-in class, which is what such a constructor may create.
static void allocateInstanceFromUnknownConstructor(FlowSolver& solver, const FlowCallRequest& request, FlowObjectID newTarget)
{
    const Program& program = solver.program();
    if (solver.objects().kindOf(newTarget) != FlowObjectKind::Closure)
        return;
    LayoutID layout = program.at(solver.objects().at(newTarget).function).constructedClass;
    if (!layout || !program.at(layout).has(LayoutFlags::HasHeritage))
        return;

    FlowSiteDescription description {
        .kind = FlowSiteKind::Layout,
        .origin = FlowSiteOrigin::Operation,
        .value = request.origin,
        .layout = layout,
    };
    FlowObjectID instance = allocateAtCallSite(solver, { request.site, description });
    if (instance == unknownFlowObject)
        return;

    Witness witness = solver.witnessAt(request.site, request.origin);
    connectPrototypeFromConstructor(solver, request, instance, newTarget, BuiltinID());
    if (mayRunRepeatedly(request))
        solver.noteEvent(instance, FlowObjectEvent::AllocatedRepeatedly, witness);
    handToUnknownRecipient(solver, instance, witness);
    solver.joinObject(request.operands.result, instance, witness);
}

static void allocateInstanceForExactNewTarget(FlowSolver& solver, const FlowCallRequest& request)
{
    if (request.kind == FlowCallLinkKind::Construct && request.operands.newTargetObject)
        allocateInstanceFromUnknownConstructor(solver, request, request.operands.newTargetObject);
}

static bool hasExpectedResult(const FlowSolver& solver, const FlowCallRequest& request)
{
    if (!request.origin || request.kind != FlowCallLinkKind::Call)
        return false;
    const Value& operation = solver.operationAt(request.site, request.origin);
    return operation.opcode == Opcode::Call && operation.expectedResult();
}

static bool isFollowedAtEveryRun(const FlowSolver& solver, const FlowCallRequest& request)
{
    return constructsForNewTargetNode(request) || hasExpectedResult(solver, request);
}

void followLinkedRequest(FlowSolver& solver, const FlowCallRequest& request, FlowLinkedRequest& linkedRequest)
{
    if (linkedRequest.hasUnknownCallee)
        allocateExpectedResult(solver, request);
    if (!constructsForNewTargetNode(request))
        return;

    auto takeNewTargets = [&](FlowCallTemporaryRole role) {
        return solver.takeNewObjects(callTemporaryKey(request.site, role, linkedRequest.index), request.operands.newTarget);
    };

    takeNewTargets(FlowCallTemporaryRole::NewTargetsOfConstructors).forEach([&](FlowObjectID newTarget) {
        noteNewTarget(solver, request, linkedRequest, newTarget);
    });
    if (linkedRequest.allocatesThis)
        allocateThisForNewTargets(solver, request, linkedRequest);
    if (!linkedRequest.hasUnknownCallee)
        return;
    solver.forEachMember(takeNewTargets(FlowCallTemporaryRole::NewTargetsOfUnknownConstructor), [&](FlowObjectID newTarget) {
        allocateInstanceFromUnknownConstructor(solver, request, newTarget);
    });
}

void noteUnknownCallee(FlowSolver& solver, FlowStatementSite site, ValueID origin, FlowCallLinkKind kind)
{
    if (solver.callState().links.recordedUnknownCallees.add(std::tuple { site.instance, origin, static_cast<uint8_t>(kind) }).isNewEntry)
        solver.records().addUnknownCallee({ site.instance, origin, kind });
}

static bool passesOperandsOfNativeCode(FlowSolver& solver, const FlowCallRequest& request)
{
    return passesOperandsOfHost(solver, request.site, request) && isInSeenCode(solver, request.site);
}

void callUnknownCallee(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowLinkedRequest& linkedRequest)
{
    const FlowCallOperands& operands = request.operands;
    Witness witness = solver.witnessAt(request.site, request.origin);
    if (passesOperandsOfNativeCode(solver, request))
        exposeStartupModules(solver, witness);
    if (operands.thisObject)
        handToUnknownRecipient(solver, operands.thisObject, witness);

    // The load of the prototype property of new.target is the one other part that the statement comes back for.
    if (std::exchange(linkedRequest.hasUnknownCallee, true)) {
        allocateInstanceForExactNewTarget(solver, request);
        return;
    }

    noteUnknownCallee(solver, request.site, request.origin, request.kind);
    if (request.kind == FlowCallLinkKind::Construct && (operands.newTarget || operands.newTargetObject))
        constructEscapedFunctionsForOtherNewTarget(solver, witness);

    if (!operands.thisObject)
        handToUnknownRecipient(solver, operands.thisValue, witness);
    if (operands.newTargetObject)
        handToUnknownRecipient(solver, operands.newTargetObject, witness);
    else
        handToUnknownRecipient(solver, operands.newTarget, witness);
    for (FlowNodeID position : arguments.positions)
        handToUnknownRecipient(solver, position, witness);
    handToUnknownRecipient(solver, arguments.rest, witness);

    solver.joinUnknown(operands.result, witness);
    allocateInstanceForExactNewTarget(solver, request);
    throwUnknownValue(solver, operands.thrown, witness);
}

static void callClosure(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowObjectID closure, FlowLinkedRequest& linkedRequest)
{
    FunctionID function = solver.objects().at(closure).function;
    const Function& callee = solver.program().at(function);

    if (request.kind == FlowCallLinkKind::Construct) {
        if (callee.constructorForm() == ConstructorForm::NotConstructor)
            return;
        noteNewTargetsOf(solver, request, linkedRequest, closure);
    } else if (callee.kind == FunctionKind::BaseConstructor || callee.kind == FunctionKind::DerivedConstructor)
        return;
    if (passesOperandsOfNativeCode(solver, request))
        exposeStartupModules(solver, solver.witnessAt(request.site, request.origin));

    linkFunctionCall(solver, { request, arguments, function, closure, linkedRequest.context, linkedRequest.policy }, linkedRequest);
}

void runCallOfValues(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowNodeID callees)
{
    if (!callees)
        return;
    FlowNewObjects objects = solver.takeNewObjects(callTemporaryKey(request.site, FlowCallTemporaryRole::CalledValues, callees.index()), callees);
    solver.charge(request.site, callObjectsWith(solver, request, arguments, objects));
}

// What a bound function passes on reaches the target over edges. The statement looks at it again only to turn generic.
static void waitOnBoundOperand(FlowSolver& solver, FlowStatementSite site, FlowNodeID operand)
{
    solver.waitOn(operand, site, FlowWakeUp::ContinueAtUnknownObject);
}

static FlowNodeID mixBoundArguments(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& passed, FlowObjectID boundFunction)
{
    const FlowGraph& graph = solver.graph();
    FlowStatementSite site = request.site;
    FlowNodeID boundArguments = graph.slot(boundFunction, BuiltinSlots::BoundFunctionBoundArguments);
    FlowNodeID mixed = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::MixedBoundArguments, boundFunction.index()));

    solver.takeNewObjects(callTemporaryKey(site, FlowCallTemporaryRole::ArgumentLists, boundArguments.index()), boundArguments).forEach([&](FlowObjectID list) {
        if (!contentsNode(graph, list, ContentsSlot::Element)) {
            solver.joinUnknown(mixed, solver.witnessAt(site, request.origin));
            return;
        }
        forEachElementPart(graph, list, arrayIndexRange, [&](FlowNodeID part) {
            solver.connect(part, mixed);
        });
    });
    for (FlowNodeID position : passed.positions)
        solver.connect(position, mixed);
    solver.connect(passed.rest, mixed);
    solver.joinPrimitives(mixed, Primitive::Undefined);
    waitOnBoundOperand(solver, site, mixed);
    return mixed;
}

static FlowPassedArguments prependBoundArguments(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& passed, FlowObjectID boundFunction)
{
    const FlowGraph& graph = solver.graph();
    FlowNodeID boundArguments = graph.slot(boundFunction, BuiltinSlots::BoundFunctionBoundArguments);
    solver.waitOn(boundArguments, request.site, FlowWakeUp::Revisit);

    const FlowObjectSet& lists = solver.valueOf(boundArguments).objects;
    if (lists.isEmpty())
        return passed;

    FlowObjectID list = lists.onlyObject();
    FlowNodeID rest = list ? contentsNode(graph, list, ContentsSlot::Element) : FlowNodeID();
    solver.waitOn(rest, request.site, FlowWakeUp::Revisit);
    if (!rest || !solver.valueOf(rest).isEmpty())
        return { { }, mixBoundArguments(solver, request, passed, boundFunction), { 0, Range::length().maximum } };

    FlowPassedArguments all;
    uint32_t numberOfPositions = solver.objects().at(list).numberOfPositions;
    for (uint32_t position = 0; position < numberOfPositions; ++position) {
        FlowNodeID boundArgument = graph.position(list, position);
        waitOnBoundOperand(solver, request.site, boundArgument);
        all.positions.append(boundArgument);
    }
    all.positions.appendVector(passed.positions);
    all.rest = passed.rest;
    all.count = { passed.count.minimum + numberOfPositions, passed.count.maximum + numberOfPositions };
    return all;
}

static bool mayConstructTarget(FlowSolver& solver, FlowStatementSite site, FlowObjectID boundFunction, Vector<FlowObjectID, 2>& visited)
{
    if (visited.contains(boundFunction))
        return false;
    visited.append(boundFunction);

    FlowNodeID targets = solver.graph().slot(boundFunction, BuiltinSlots::BoundFunctionTarget);
    solver.waitOn(targets, site);
    return solver.valueOf(targets).objects.anyOf([&](FlowObjectID target) {
        const FlowObject& record = solver.objects().at(target);
        if (record.kind == FlowObjectKind::Closure)
            return solver.program().at(record.function).constructorForm() != ConstructorForm::NotConstructor;
        return !solver.objects().isInstanceOf(target, BuiltinClasses::BoundFunction) || mayConstructTarget(solver, site, target, visited);
    });
}

// Native code constructs what it holds, and a construction passes other operands than a call. It gets what such a bound function holds and enters the target itself.
// No code, seen or unseen, writes what a bound function holds, so nothing is opened, whatever the function has reached.
static void exposeCapturesToConstruction(FlowSolver& solver, const FlowCallRequest& request, FlowObjectID boundFunction)
{
    const FlowGraph& graph = solver.graph();
    Vector<FlowObjectID, 2> visited;
    if (!mayConstructTarget(solver, request.site, boundFunction, visited))
        return;
    Witness witness = reachWitnessOf(solver, boundFunction, ObjectMutation::ReachNativeCode);
    for (uint32_t index = 0; index < solver.objects().at(boundFunction).numberOfDeclaredParts; ++index) {
        FlowNodeID part = graph.declaredPart(boundFunction, index);
        solver.connect(part, graph.nativeCode(), { }, witness);
        solver.noteLocationEvent(part, FlowLocationEvent::ReadByUnknownCode);
    }
}

void runBoundFunctionCall(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& passed, FlowObjectID boundFunction)
{
    const FlowGraph& graph = solver.graph();
    if (request.kind == FlowCallLinkKind::Host)
        exposeCapturesToConstruction(solver, request, boundFunction);

    FlowCallRequest inner = request;
    inner.operands.thisObject = { };
    inner.operands.thisValue = request.kind == FlowCallLinkKind::Construct ? FlowNodeID() : graph.slot(boundFunction, BuiltinSlots::BoundFunctionBoundThis);
    waitOnBoundOperand(solver, request.site, inner.operands.thisValue);

    FlowPassedArguments all = prependBoundArguments(solver, request, passed, boundFunction);
    runCallOfValues(solver, inner, all, graph.slot(boundFunction, BuiltinSlots::BoundFunctionTarget));
}

static void noteCalleeUses(FlowSolver& solver, const FlowCallRequest& request, FlowObjectID callee)
{
    solver.noteUse(callee, request.kind == FlowCallLinkKind::Construct ? FlowObjectUse::Constructed : FlowObjectUse::Called);
    if (mayRunRepeatedly(request) || request.kind == FlowCallLinkKind::Host)
        solver.noteUse(callee, FlowObjectUse::CalledRepeatedly);
}

static bool isCalledAsUnknownCallee(const FlowSolver& solver, FlowObjectID callee)
{
    switch (solver.objects().kindOf(callee)) {
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Opaque:
        return true;
    case FlowObjectKind::Instance:
        return !solver.objects().isInstanceOf(callee, BuiltinClasses::BoundFunction) && isFunctionClass(solver.builtins(), solver.objects().at(callee).builtinClass);
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An unknown callee hands every operand to native code and answers with the unknown value, and native code calls whatever has reached it with unknown operands.
// So beside an unknown callee, a callee that has reached native code needs nothing from the statement: calling it there only predicts it,
// and a call has an arm for each predicted callee or for none. One that the request has linked before stays followed.
// Two things native code does not do until unknown JavaScript can run: it constructs a function with no other new.target than the function, so a construction is linked all the same,
// and what a built-in stores into the slots of an unknown receiver reaches no escaped object, so a built-in is called all the same.
static std::optional<FlowNewObjects> withoutUnpredictedCallees(FlowSolver& solver, const FlowNewObjects& callees, FlowLinkedRequest& linkedRequest)
{
    bool mayRunUnknownJavaScript = solver.records().mayRunUnknownJavaScript();
    if (linkedRequest.kind == FlowCallLinkKind::Construct && !mayRunUnknownJavaScript)
        return std::nullopt;

    FlowObjectSet calleesOfNativeCode = callees;
    calleesOfNativeCode.intersect(solver.escapedObjects());
    calleesOfNativeCode.subtract(linkedRequest.closures);
    calleesOfNativeCode.subtract(linkedRequest.predictedCallees);
    FlowObjectSet calledAllTheSame;
    calleesOfNativeCode.forEach([&](FlowObjectID callee) {
        FlowObjectKind kind = solver.objects().kindOf(callee);
        bool isBuiltin = kind == FlowObjectKind::Builtin || kind == FlowObjectKind::ModuleBuiltin;
        if (isCalledAsUnknownCallee(solver, callee) || (isBuiltin && !mayRunUnknownJavaScript))
            calledAllTheSame.add(callee);
    });
    calleesOfNativeCode.subtract(calledAllTheSame);
    if (calleesOfNativeCode.isEmpty())
        return std::nullopt;

    // Nobody waits for a prediction, so running every rule again makes none.
    if (!solver.isCheckingFixpoint()) {
        if (linkedRequest.predictedCallees.size() + calleesOfNativeCode.size() > solver.callState().limits.maxDispatchCases)
            linkedRequest.hasTooManyCalleesToPredict = true;
        if (!linkedRequest.hasTooManyCalleesToPredict) {
            linkedRequest.predictedCallees.unite(calleesOfNativeCode);
            return std::nullopt;
        }
    }

    FlowObjectSet remaining = callees;
    remaining.subtract(calleesOfNativeCode);
    return callees.withObjects(WTF::move(remaining));
}

// Answers how many callees it has called.
static size_t callObjectsWith(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, const FlowNewObjects& callees)
{
    FlowContext context = contextOf(solver, request, arguments, callees);
    bool isFollowed = isFollowedAtEveryRun(solver, request);
    if (callees.isEmpty() && !isFollowed)
        return 0;

    FlowLinkedRequest* found = findLinkedRequest(solver, request, arguments, context, FlowInstancePolicy::BySummary);
    auto linkedRequest = [&]() -> FlowLinkedRequest& {
        if (!found)
            found = &linkedRequestOf(solver, request, arguments, context, FlowInstancePolicy::BySummary);
        return *found;
    };

    std::optional<FlowNewObjects> remainingCallees;
    // Whether the callees that come with an unknown one are predictions does not depend on which of them is looked at first.
    bool hasUnknownCallee = found && found->hasUnknownCallee;
    if (!hasUnknownCallee) {
        callees.forEach([&](FlowObjectID callee) {
            hasUnknownCallee = hasUnknownCallee || isCalledAsUnknownCallee(solver, callee);
        });
    }
    if (hasUnknownCallee)
        remainingCallees = withoutUnpredictedCallees(solver, callees, linkedRequest());

    const FlowNewObjects& calledObjects = remainingCallees ? *remainingCallees : callees;
    solver.forEachMember(calledObjects, [&](FlowObjectID callee) {
        if (solver.isAboutToBeAbandoned(request.site))
            return;
        FlowObjectKind kind = solver.objects().kindOf(callee);
        if (kind != FlowObjectKind::Unknown)
            noteCalleeUses(solver, request, callee);

        switch (kind) {
        case FlowObjectKind::Unknown:
        case FlowObjectKind::Opaque:
            callUnknownCallee(solver, request, arguments, linkedRequest());
            return;
        case FlowObjectKind::Allocation:
        case FlowObjectKind::Prototype:
        case FlowObjectKind::ModuleNamespace:
            return;
        case FlowObjectKind::Closure:
            callClosure(solver, request, arguments, callee, linkedRequest());
            return;
        case FlowObjectKind::Instance:
            if (solver.objects().isInstanceOf(callee, BuiltinClasses::BoundFunction))
                callBoundFunction(solver, request, arguments, callee);
            else if (isFunctionClass(solver.builtins(), solver.objects().at(callee).builtinClass))
                callUnknownCallee(solver, request, arguments, linkedRequest());
            return;
        case FlowObjectKind::Builtin:
        case FlowObjectKind::ModuleBuiltin:
            callBuiltinObject(solver, request, arguments, callee);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });

    if (isFollowed && found)
        followLinkedRequest(solver, request, *found);
    return calledObjects.size();
}

void callObjects(FlowSolver& solver, const FlowCallRequest& request, const FlowNewObjects& callees)
{
    callObjectsWith(solver, request, expandArguments(solver, request), callees);
}

static void callMethodsByKey(FlowSolver& solver, const FlowCallRequest& request, PropertyKeyID key)
{
    FlowNodeID receiver = request.operands.thisValue;
    FlowKeySet keys = keySetOf(solver.program(), key);
    callMethods(solver, request, solver.takeNewObjects(request.site, 1, receiver), keys);
    if (!solver.takeNewPrimitives(request.site, 1, receiver))
        return;

    uint16_t primitives = static_cast<uint16_t>(solver.valueOf(receiver).primitives & ~(Primitive::UndefinedOrNull | Primitive::Absent));
    if (primitives)
        callMethodsOfPrimitives(solver, request, { primitives }, keys);
}

void runCall(FlowSolver& solver, FlowStatementSite site, const FlowCall& call)
{
    Vector<FlowNodeID, 8> arguments = solver.nodesOf(site, call.arguments);
    FlowCallRequest request = requestOf(solver, site, call, arguments.span());

    if (call.calleeKey) {
        callMethodsByKey(solver, request, call.calleeKey);
        return;
    }
    callObjects(solver, request, solver.takeNewObjects(site, 0, solver.node(site, call.callee)));
}

void runDirectCall(FlowSolver& solver, FlowStatementSite site, const FlowDirectCall& call)
{
    Vector<FlowNodeID, 8> arguments = solver.nodesOf(site, call.arguments);
    Vector<FlowNodeID, 8> results = solver.nodesOf(site, call.results);
    Witness witness = solver.witnessAt(site, call.origin);

    FlowCallRequest request {
        site,
        call.origin,
        FlowCallLinkKind::Call,
        BuiltinCallbackTimes::Once,
        call.flags.contains(FlowCallFlag::IsInCycle),
        {
            .arguments = arguments.span(),
            .result = results.isEmpty() ? FlowNodeID() : results[0],
            .thrown = solver.node(site, call.thrown),
        },
    };
    FlowPassedArguments passed = expandArguments(solver, request);
    FlowContext context = contextOf(solver, request, passed, FlowObjectSet());
    FlowFunctionCall direct { request, passed, call.function, { }, context, FlowInstancePolicy::BySummary };

    // The statement comes back only for what an async function returns.
    FlowLinkedRequest& linkedRequest = linkedRequestOf(solver, request, passed, context, direct.policy);
    if (!mayLinkWithTrackedOperands(solver, direct, linkedRequest))
        return;
    if (!linkedRequest.addCallee(direct) && !solver.graph().summaryOf(call.function).coroutine.resultSite)
        return;

    FlowInstanceID callee = chooseInstance(solver, direct);
    if (!callee) {
        callUnknownCallee(solver, request, passed, linkedRequest);
        for (FlowNodeID result : results)
            solver.joinUnknown(result, witness);
        return;
    }

    FlowLinkedCall linked { ensureCallLink(solver, request, callee), callee, hasTrackedOperands(solver.graph().at(callee).context) };
    for (uint32_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index])
            passOperand(solver, direct, linked, { arguments[index], { }, index });
    }

    const FlowSummary& summary = solver.graph().summaryOf(callee);
    if (summary.coroutine.resultSite || results.size() == 1) {
        connectCallResults(solver, direct, linked);
        return;
    }
    for (uint32_t index = 0; index < results.size(); ++index)
        solver.connect(solver.node(callee, summary.result(index)), results[index], { }, witness);
    connectThrown(solver, solver.node(callee, summary.thrown()), request.operands.thrown, witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
