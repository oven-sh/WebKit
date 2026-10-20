/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include "AOTEffectsOfOperands.h"
#include <optional>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static Witness witnessOf(const FlowSolver& solver, const FlowCallRequest& request)
{
    return solver.witnessAt(request.site, request.origin);
}

FlowLinkedRequest::FlowLinkedRequest(uint32_t index, const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowContext context, FlowInstancePolicy policy)
    : index(index)
    , origin(request.origin)
    , kind(request.kind)
    , times(request.times)
    , isInCycle(request.isInCycle)
    , context(context)
    , policy(policy)
    , passesExactReceiver(request.operands.thisObject)
    , thisValue(request.operands.thisValue)
    , newTarget(request.operands.newTarget)
    , newTargetObject(request.operands.newTargetObject)
    , result(request.operands.result)
    , thrown(request.operands.thrown)
    , arguments(arguments)
{
}

bool FlowLinkedRequest::isFor(const FlowCallRequest& request, const FlowPassedArguments& passed, FlowContext requestedContext, FlowInstancePolicy requestedPolicy) const
{
    const FlowCallOperands& operands = request.operands;
    return origin == request.origin
        && kind == request.kind
        && times == request.times
        && isInCycle == request.isInCycle
        && context == requestedContext
        && policy == requestedPolicy
        && passesExactReceiver == !!operands.thisObject
        && thisValue == operands.thisValue
        && newTarget == operands.newTarget
        && newTargetObject == operands.newTargetObject
        && result == operands.result
        && thrown == operands.thrown
        && arguments == passed;
}

bool FlowLinkedRequest::hasCallee(const FlowFunctionCall& call) const
{
    return call.closure ? closures.contains(call.closure) : functionsWithoutClosure.contains(call.function);
}

bool FlowLinkedRequest::addCallee(const FlowFunctionCall& call)
{
    if (call.closure)
        return closures.add(call.closure);
    if (functionsWithoutClosure.contains(call.function))
        return false;
    functionsWithoutClosure.append(call.function);
    return true;
}

FlowLinkedRequest* findLinkedRequest(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowContext context, FlowInstancePolicy policy)
{
    FlowCallLinkState& state = solver.callState().links;
    auto last = state.lastLinkedRequestOfSite.find(request.site);
    if (last == state.lastLinkedRequestOfSite.end())
        return nullptr;

    std::optional<uint32_t> index = last->value;
    while (index) {
        FlowLinkedRequest& candidate = state.linkedRequests[*index];
        if (candidate.isFor(request, arguments, context, policy))
            return &candidate;
        index = candidate.previousOfSite;
    }
    return nullptr;
}

FlowLinkedRequest& linkedRequestOf(FlowSolver& solver, const FlowCallRequest& request, const FlowPassedArguments& arguments, FlowContext context, FlowInstancePolicy policy)
{
    if (FlowLinkedRequest* found = findLinkedRequest(solver, request, arguments, context, policy))
        return *found;

    FlowCallLinkState& state = solver.callState().links;
    uint32_t index = static_cast<uint32_t>(state.linkedRequests.size());
    FlowLinkedRequest& created = state.linkedRequests.alloc(index, request, arguments, context, policy);
    auto last = state.lastLinkedRequestOfSite.add(request.site, index);
    if (!last.isNewEntry)
        created.previousOfSite = std::exchange(last.iterator->value, index);
    return created;
}

static FlowNodeID entryParameterNode(const FlowSolver& solver, const FlowLinkedCall& linked, uint32_t entryParameter)
{
    return solver.node(linked.callee, solver.graph().summaryOf(linked.callee).entryParameter(entryParameter));
}

static bool recordOperandFlow(FlowSolver& solver, const FlowLinkedCall& linked, FlowNodeID from, FlowNodeID target)
{
    FlowOperandFlowState& state = solver.callState().operandFlows;
    if (!state.recordedFlows.add(std::tuple { linked.link, from, target }).isNewEntry)
        return false;

    FlowNodeID operand = state.operandOfReceiverHub.get(from);
    solver.records().addOperandFlow({ operand ? operand : from, target, linked.link });
    return true;
}

static void passToNode(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked, FlowNodeID from, FlowNodeID target, TypeID filter)
{
    if (!from || !target)
        return;
    if (!recordOperandFlow(solver, linked, from, target))
        return;

    FlowNodeID source = linked.passesThroughGates ? provenOperandNode(solver, call.request.site, from) : from;
    solver.connect(source, target, filter, witnessOf(solver, call.request));
}

static void passCalleeObject(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked, uint32_t entryParameter)
{
    if (call.closure)
        passOperand(solver, call, linked, { { }, call.closure, entryParameter });
    else
        solver.joinUnknownObject(entryParameterNode(solver, linked, entryParameter), witnessOf(solver, call.request));
}

static FlowNodeID loadPrototypeProperty(FlowSolver& solver, const FlowCallRequest& request, FlowObjectID constructor)
{
    const Program& program = solver.program();
    FlowNodeID prototype = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::PrototypeOfNewTarget, constructor.index()));
    FlowAccess load { .site = request.site, .origin = request.origin, .kind = FlowAccessKind::Load, .value = prototype, .thrown = request.operands.thrown };
    if (mayRunRepeatedly(request))
        load.flags.add(FlowAccessFlag::IsInCycle);
    accessProperties(solver, load, FlowObjectSet(constructor), keySetOf(program, program.keyOf(BuiltinNames::prototype)));
    return prototype;
}

void connectPrototypeFromConstructor(FlowSolver& solver, const FlowCallRequest& request, FlowObjectID created, FlowObjectID constructor, BuiltinID defaultPrototype)
{
    FlowNodeID prototype = loadPrototypeProperty(solver, request, constructor);
    solver.waitOn(prototype, request.site, FlowWakeUp::Revisit);
    bool mayTakeDefault = !!solver.valueOf(prototype).primitives;

    if (created == unknownFlowObject) {
        Witness witness = witnessOf(solver, request);
        connectCreationPrototypeOfUnknownObject(solver, prototype, witness);
        // The members of a class that trimming can remove are found through the unknown object once the prototype of the class has reached native code.
        if (mayTakeDefault && defaultPrototype)
            solver.joinObject(solver.graph().nativeCode(), ensureBuiltinObject(solver, defaultPrototype), witness);
        return;
    }

    connectCreationPrototype(solver, created, prototype);
    if (!mayTakeDefault)
        return;

    FlowNodeID link = solver.ensureOpenPart(created, FlowOpenPart::PrototypeLink);
    if (defaultPrototype)
        solver.joinObject(link, ensureBuiltinObject(solver, defaultPrototype));
    else
        solver.joinUnknownObject(link, witnessOf(solver, request));
}

static FlowObjectID allocateForNewTarget(FlowSolver& solver, const FlowCallRequest& request, FlowObjectID newTarget)
{
    if (!newTarget)
        newTarget = unknownFlowObject;

    FlowObjectID created = unknownFlowObject;
    if (solver.objects().kindOf(newTarget) == FlowObjectKind::Closure) {
        LayoutID layout = solver.program().at(solver.objects().at(newTarget).function).constructedClass;
        FlowSiteDescription description {
            .kind = layout ? FlowSiteKind::Layout : FlowSiteKind::Opaque,
            .origin = FlowSiteOrigin::Operation,
            .value = request.origin,
            .layout = layout,
        };
        created = allocateAtCallSite(solver, { request.site, description });
    }

    // Whatever stands for the new object, it inherits from what new.target holds as its prototype: a bound function or a proxy is given one by the program.
    connectPrototypeFromConstructor(solver, request, created, newTarget, Builtins::ObjectPrototype);
    if (created != unknownFlowObject && mayRunRepeatedly(request))
        solver.noteEvent(created, FlowObjectEvent::AllocatedRepeatedly, witnessOf(solver, request));
    return created;
}

static bool takesAllocatedThis(const Function& function)
{
    return function.thisParameterIndex() || function.kind == FunctionKind::Plain;
}

void allocateThisForNewTargets(FlowSolver& solver, const FlowCallRequest& request, const FlowLinkedRequest& linkedRequest)
{
    FlowTemporaryKey key = callTemporaryKey(request.site, FlowCallTemporaryRole::ObjectsForNewTargets, linkedRequest.index);
    FlowNodeID allocated = solver.temporaryFor(key);
    Witness witness = witnessOf(solver, request);
    solver.forEachMember(solver.takeNewObjects(key, request.operands.newTarget), [&](FlowObjectID newTarget) {
        solver.joinObject(allocated, allocateForNewTarget(solver, request, newTarget), witness);
    });
}

// What is allocated depends on new.target alone. The objects for the values of a node are collected in one node per request, and each constructor takes them over an edge.
static void allocateThis(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked, FlowLinkedRequest& linkedRequest)
{
    const FlowCallRequest& request = call.request;
    const Function& function = solver.program().at(call.function);
    if (request.kind != FlowCallLinkKind::Construct || !takesAllocatedThis(function))
        return;

    const FlowCallOperands& operands = request.operands;
    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    FlowNodeID parameter = thisParameter ? entryParameterNode(solver, linked, *thisParameter) : FlowNodeID();
    Witness witness = witnessOf(solver, request);

    if (constructsForNewTargetNode(request)) {
        FlowNodeID allocated = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::ObjectsForNewTargets, linkedRequest.index));
        solver.connect(allocated, parameter, { }, witness);
        solver.connect(allocated, operands.result, { }, witness);
        linkedRequest.allocatesThis = true;
        return;
    }

    FlowObjectID created = allocateForNewTarget(solver, request, operands.newTargetObject ? operands.newTargetObject : call.closure);
    solver.joinObject(parameter, created, witness);
    solver.joinObject(operands.result, created, witness);
}

static TypeID resultFilterOf(const FlowSolver& solver, const FlowCallRequest& request)
{
    if (request.kind != FlowCallLinkKind::Construct)
        return { };
    if (!request.origin)
        return TypeTable::object;

    const Value& operation = solver.operationAt(request.site, request.origin);
    if (operation.opcode == Opcode::SuperConstruct || operation.opcode == Opcode::SuperConstructList)
        return operation.type;
    return TypeTable::object;
}

static void connectCoroutineResult(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked)
{
    const FlowCallRequest& request = call.request;
    const FlowSummary& summary = solver.graph().summaryOf(linked.callee);
    Witness witness = witnessOf(solver, request);
    FlowObjectID result = linkCoroutineResult(solver, request, linked.callee);

    bool isGenerator = !!summary.coroutine.yielded;
    if (isGenerator && call.closure && result != unknownFlowObject) {
        BuiltinID defaultPrototype = solver.builtins().at(solver.objects().at(result).builtinClass).prototype;
        connectPrototypeFromConstructor(solver, request, result, call.closure, defaultPrototype);
    }
    solver.joinObject(request.operands.result, result, witness);

    // A generator runs up to its initial yield inside the call, so what its parameter initializers throw reaches the caller.
    if (isGenerator)
        connectThrown(solver, solver.node(linked.callee, summary.thrown()), request.operands.thrown, witness);
}

// Operands and results follow edges. A statement comes back to a linked callee only for what it reads on behalf of that callee alone:
// what an async function returns, and the prototype property of the function that an object is allocated for.
static bool isFollowedAfterLinking(const FlowSolver& solver, const FlowFunctionCall& call)
{
    const FlowSummary& summary = solver.graph().summaryOf(call.function);
    if (summary.coroutine.resultSite)
        return true;
    if (call.request.kind != FlowCallLinkKind::Construct || constructsForNewTargetNode(call.request))
        return false;
    return summary.hasBody() ? takesAllocatedThis(solver.program().at(call.function)) : !!call.request.operands.newTargetObject;
}

void linkFunctionCall(FlowSolver& solver, const FlowFunctionCall& call)
{
    FlowLinkedRequest& linkedRequest = linkedRequestOf(solver, call.request, call.arguments, call.context, call.policy);
    linkFunctionCall(solver, call, linkedRequest);
    followLinkedRequest(solver, call.request, linkedRequest);
}

// An instance with tracked operands is entered because no operand is unknown. What passes the gates of a link that exists follows edges, whatever the site becomes.
bool mayLinkWithTrackedOperands(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedRequest& linkedRequest)
{
    if (!hasTrackedOperands(call.context))
        return true;
    if (linkedRequest.hasCallee(call) && !linkedRequest.passesExactReceiver)
        return true;
    return solver.mayRelyOn(FlowAbsence::UnknownOperand, call.request.site);
}

void linkFunctionCall(FlowSolver& solver, const FlowFunctionCall& call, FlowLinkedRequest& linkedRequest)
{
    if (!mayLinkWithTrackedOperands(solver, call, linkedRequest))
        return;

    bool isFirstVisit = linkedRequest.addCallee(call) || linkedRequest.passesExactReceiver;
    if (!isFirstVisit && !isFollowedAfterLinking(solver, call))
        return;

    FlowInstanceID callee = chooseInstance(solver, call);
    if (!callee) {
        callUnknownCallee(solver, call.request, call.arguments, linkedRequest);
        return;
    }

    FlowLinkedCall linked { ensureCallLink(solver, call.request, callee), callee, hasTrackedOperands(solver.graph().at(callee).context) };
    if (isFirstVisit) {
        passCaptures(solver, call, linked);
        passThisAndNewTarget(solver, call, linked);
        passArguments(solver, call, linked);
    }
    allocateThis(solver, call, linked, linkedRequest);
    connectCallResults(solver, call, linked);
}

FlowCallLinkID ensureCallLink(FlowSolver& solver, const FlowCallRequest& request, FlowInstanceID callee)
{
    FlowInstanceID caller = request.site.instance;
    return solver.callState().links.links.ensure(std::tuple { caller, request.origin, callee }, [&] {
        const FlowGraph& graph = solver.graph();
        didLinkCall(solver, caller, callee);
        return solver.records().addCallLink({
            caller,
            request.origin,
            callee,
            graph.at(caller).function,
            graph.at(callee).function,
            request.kind,
            request.times,
            request.isInCycle,
            request.operands.result,
            request.operands.thrown,
        });
    }).iterator->value;
}

// The version for unknown operands takes anything. What the calls that flow follows pass to it is what the other calls are expected to pass.
// Beside the unknown object the parameter lists few objects, so a node of its own collects them. It has no reader in the solve and proves nothing.
static void predictEntryParameter(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked, const FlowPassedOperand& operand, TypeID filter)
{
    if (call.context != FlowContext::Generic || (!operand.object && !operand.from) || (filter && !mayBeReference(solver.program().at(filter))))
        return;

    FlowNodeID prediction = solver.predictionFor(callTemporaryKey({ linked.callee, { } }, FlowCallTemporaryRole::PredictedEntryParameter, operand.entryParameter));
    solver.records().setPredictedEntryParameter(call.function, operand.entryParameter, prediction);
    if (!operand.object)
        solver.connect(operand.from, prediction, filter, witnessOf(solver, call.request));
    else if (operand.object != unknownFlowObject)
        solver.joinObject(prediction, operand.object, witnessOf(solver, call.request));
}

void passOperand(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked, const FlowPassedOperand& operand)
{
    FlowNodeID parameter = entryParameterNode(solver, linked, operand.entryParameter);
    const DeclaredType* declared = solver.program().at(call.function).declaredEntryParameter(operand.entryParameter);
    TypeID filter = declared ? declared->type : TypeID();
    predictEntryParameter(solver, call, linked, operand, filter);

    if (!operand.object) {
        passToNode(solver, call, linked, operand.from, parameter, filter);
        return;
    }

    FlowValue value = solver.values().valueOfObject(operand.object);
    solver.join(parameter, filter ? solver.values().filter(value, solver.program().at(filter)) : value, witnessOf(solver, call.request));
    if (operand.from)
        recordOperandFlow(solver, linked, operand.from, parameter);
}

void passPrimitives(FlowSolver& solver, const FlowLinkedCall& linked, uint32_t entryParameter, uint16_t primitives)
{
    solver.joinPrimitives(entryParameterNode(solver, linked, entryParameter), primitives);
}

void passCaptures(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked)
{
    if (!call.closure)
        return;

    uint32_t numberOfCaptures = solver.program().at(call.function).captures.size();
    for (uint32_t index = 0; index < numberOfCaptures; ++index) {
        FlowNodeID from = solver.graph().capture(call.closure, index);
        FlowNodeID parameter = entryParameterNode(solver, linked, index);
        if (recordOperandFlow(solver, linked, from, parameter))
            solver.connect(from, parameter);
    }
}

void passThisAndNewTarget(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked)
{
    const Function& function = solver.program().at(call.function);
    const FlowCallOperands& operands = call.request.operands;
    bool constructs = call.request.kind == FlowCallLinkKind::Construct;

    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    if (thisParameter && !constructs) {
        if (operands.thisObject || operands.thisValue)
            passOperand(solver, call, linked, { operands.thisValue, operands.thisObject, *thisParameter });
        else
            passPrimitives(solver, linked, *thisParameter, Primitive::Undefined);
    }

    if (std::optional<unsigned> calleeParameter = function.calleeParameterIndex())
        passCalleeObject(solver, call, linked, *calleeParameter);

    if (std::optional<unsigned> newTargetParameter = function.newTargetParameterIndex()) {
        if (!constructs)
            passPrimitives(solver, linked, *newTargetParameter, Primitive::Undefined);
        else if (operands.newTargetObject || operands.newTarget)
            passOperand(solver, call, linked, { operands.newTarget, operands.newTargetObject, *newTargetParameter });
        else
            passCalleeObject(solver, call, linked, *newTargetParameter);
    }

    if (std::optional<unsigned> countParameter = function.argumentCountParameterIndex())
        solver.join(entryParameterNode(solver, linked, *countParameter), solver.values().valueOfIntegers(call.arguments.count));
}

void passArguments(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked)
{
    const Function& function = solver.program().at(call.function);
    uint32_t firstParameter = function.firstParameterIndex();
    uint32_t numberOfParameters = function.numberOfPositionalParameters();

    for (uint32_t position = 0; position < numberOfParameters; ++position) {
        if (FlowNodeID argument = call.arguments.at(position))
            passOperand(solver, call, linked, { argument, { }, firstParameter + position });
        else
            passPrimitives(solver, linked, firstParameter + position, Primitive::Undefined);
    }

    if (solver.graph().summaryOf(linked.callee).restSite)
        passRestArguments(solver, call, linked, numberOfParameters);
}

void passRestArguments(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked, uint32_t firstRestArgument)
{
    const FlowGraph& graph = solver.graph();
    const FlowPassedArguments& arguments = call.arguments;
    FlowObjectID rest = graph.objectOf(linked.callee, *graph.summaryOf(linked.callee).restSite);
    uint32_t numberOfPositions = solver.objects().at(rest).numberOfPositions;
    FlowNodeID elements = contentsNode(graph, rest, ContentsSlot::Element);

    auto passToPart = [&](FlowNodeID argument, FlowNodeID part) {
        if (argument)
            passToNode(solver, call, linked, argument, part, { });
        else
            solver.joinPrimitives(part, Primitive::Undefined);
    };

    for (uint32_t index = 0; index < numberOfPositions; ++index)
        passToPart(arguments.at(firstRestArgument + index), graph.position(rest, index));
    for (size_t position = firstRestArgument + numberOfPositions; position < arguments.positions.size(); ++position)
        passToPart(arguments.positions[position], elements);
    passToNode(solver, call, linked, arguments.rest, elements, { });
}

void connectCallResults(FlowSolver& solver, const FlowFunctionCall& call, const FlowLinkedCall& linked)
{
    const FlowCallRequest& request = call.request;
    const FlowSummary& summary = solver.graph().summaryOf(linked.callee);
    if (summary.coroutine.resultSite) {
        connectCoroutineResult(solver, call, linked);
        return;
    }

    Witness witness = witnessOf(solver, request);
    if (summary.numberOfResults)
        solver.connect(solver.node(linked.callee, summary.result(0)), request.operands.result, resultFilterOf(solver, request), witness);
    connectThrown(solver, solver.node(linked.callee, summary.thrown()), request.operands.thrown, witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
