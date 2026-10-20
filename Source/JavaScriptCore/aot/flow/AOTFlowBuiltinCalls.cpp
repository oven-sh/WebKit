/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include <algorithm>
#include <initializer_list>

namespace JSC { namespace AOT { namespace Typed {

static BuiltinUse builtinUseOf(BuiltinCallMode mode)
{
    switch (mode) {
    case BuiltinCallMode::Call:
        return BuiltinUse::Called;
    case BuiltinCallMode::Construct:
        return BuiltinUse::Constructed;
    case BuiltinCallMode::Get:
        return BuiltinUse::Read;
    case BuiltinCallMode::Set:
        return BuiltinUse::Written;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID typedResultTypeOf(const FlowCallState& state, FlowInstanceID caller, ValueID call)
{
    return state.builtins.typedResultTypes.get({ caller, call });
}

// A member that the table does not describe takes the built-in object it was found on as this and leaves it alone.
FlowNodeID escapingReceiverNodeOf(FlowSolver& solver, const FlowBuiltinCall& call)
{
    if (!call.receiver)
        return { };

    FlowTemporaryKey key = callTemporaryKey(call.request.site, FlowCallTemporaryRole::EscapingReceivers, call.receiver.index());
    FlowNodeID escapingReceivers = solver.temporaryFor(key);
    FlowObjectSet newReceivers = solver.takeNewObjects(key, call.receiver);
    FlowObjectSet newEscapingReceivers = newReceivers;
    newReceivers.forEach([&](FlowObjectID receiver) {
        if (!canEscapeAsReceiver(solver.objects().kindOf(receiver)))
            newEscapingReceivers.remove(receiver);
    });
    solver.joinObjects(escapingReceivers, newEscapingReceivers);
    // No value is inside never. The edge only says that the receiver is used as the node that holds part of it.
    solver.connect(call.receiver, escapingReceivers, TypeTable::never);
    return escapingReceivers;
}

bool isInApplication(FlowSolver& solver, FlowStatementSite site)
{
    return isInSeenCode(solver, site) && solver.functionOf(site).role == FunctionRole::Source;
}

// A member that a protocol finds under one of its names (valueOf, toJSON, then, next) is called by native code of its own accord: seen code has not named it as a callee or as an operand.
bool isAppliedBySeenCode(FlowSolver& solver, const FlowBuiltinCall& call)
{
    return call.request.kind != FlowCallLinkKind::Protocol && isInSeenCode(solver, call.request.site);
}

Witness witnessOf(FlowSolver& solver, const FlowBuiltinCall& call)
{
    return solver.witnessAt(call.request.site, call.request.origin);
}

FlowNodeID operandNodeOf(const FlowBuiltinCall& call, BuiltinOperand operand)
{
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
        return call.receiver;
    case BuiltinOperand::Kind::Argument:
        return call.arguments.at(operand.argumentIndex());
    case BuiltinOperand::Kind::ArgumentsFrom:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void forEachOperandNode(const FlowBuiltinCall& call, BuiltinOperand operand, NOESCAPE const ScopedLambda<void(FlowNodeID)>& functor)
{
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        if (FlowNodeID node = operandNodeOf(call, operand))
            functor(node);
        return;
    case BuiltinOperand::Kind::ArgumentsFrom:
        for (size_t position = operand.argumentIndex(); position < call.arguments.positions.size(); ++position) {
            if (FlowNodeID node = call.arguments.positions[position])
                functor(node);
        }
        if (call.arguments.rest)
            functor(call.arguments.rest);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool mayBePassed(const FlowBuiltinCall& call, BuiltinOperand operand)
{
    if (operand.isReceiver())
        return !!call.receiver;
    return call.arguments.mayPass(operand.argumentIndex());
}

bool isAlwaysPassed(const FlowBuiltinCall& call, BuiltinOperand operand)
{
    return operand.isReceiver() || call.arguments.alwaysPasses(operand.argumentIndex());
}

static bool acceptsOnlyObjects(const BuiltinType& type)
{
    constexpr OptionSet<BuiltinPrimitive> absentValues { BuiltinPrimitive::NotPassed, BuiltinPrimitive::Undefined, BuiltinPrimitive::Null };
    return !type.isAnything() && (type.primitives - absentValues).isEmpty() && type.stringLiterals.empty();
}

// A built-in that wants an object or a function throws for anything else. One that wants a number or a string may convert without the entry saying so.
bool takesOnlyAcceptedObjects(const BuiltinTable& builtins, const BuiltinParameter& parameter)
{
    return parameter.conversion == BuiltinConversion::None && acceptsOnlyObjects(builtins.at(parameter.accepted));
}

// The last overload of a dispatch takes whatever is left, and the only overload of a built-in takes everything.
static bool refusesOperand(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    auto refuses = [&](const BuiltinParameter& parameter) {
        return takesOnlyAcceptedObjects(solver.builtins(), parameter)
            && matchOperand(solver, overloadCall.call, parameter.operand, parameter.accepted) == FlowTypeMatch::Never;
    };
    const BuiltinOverload& overload = overloadCall.overload;
    return (overload.receiver && refuses(*overload.receiver)) || std::ranges::any_of(overload.parameters, refuses);
}

static void applyOverload(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowCallOperands& operands = overloadCall.call.request.operands;
    const BuiltinOverload& overload = overloadCall.overload;

    recordOverload(solver, overloadCall);
    enterFunctionCalledByValue(solver, overloadCall);
    applyOverloadAttachments(solver, overloadCall);
    if (overload.isUndeclared()) {
        noteUndescribedNativeCode(solver, overloadCall);
        applyUndeclaredOverload(solver, overloadCall);
        return;
    }

    if (enterImplementation(solver, overloadCall) == FlowEnteredImplementation::Entered) {
        if (overload.receiver)
            applyOperandRoles(solver, overloadCall, *overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            applyOperandRoles(solver, overloadCall, parameter);
        return;
    }
    noteUndescribedNativeCode(solver, overloadCall);

    // A built-in tests the kinds of its operands before it stores, calls back or returns. What it reads from them before that test still runs.
    bool isRefused = refusesOperand(solver, overloadCall);
    if (isRefused)
        runProtocolsOfParameters(solver, overloadCall);
    else {
        applyParameters(solver, overloadCall);
        applyTransfers(solver, overloadCall);
        applyCallbacks(solver, overloadCall);
        applyHandlerCalls(solver, overloadCall);
        applyPropertyActions(solver, overloadCall);
        applyResult(solver, overloadCall);
    }

    // Code that is no operand runs too, and what it throws comes out of the call. The engine throws instances of Error; the host also throws objects of its own classes, such as a DOMException.
    bool runsOtherCode = overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
    bool throwsObjectsOfHost = (isRefused || overload.throws()) && solver.builtins().at(overloadCall.call.builtin).origin == BuiltinOrigin::Host;
    if (runsOtherCode || throwsObjectsOfHost)
        throwUnknownValue(solver, operands.thrown, witnessOf(solver, overloadCall.call));
    else if (isRefused || overload.throws())
        throwRuntimeError(solver, operands.thrown, witnessOf(solver, overloadCall.call));
}

// Nothing says what the function does to its operands, so everything can happen to those objects, and to those alone.
static void callUndescribedMember(FlowSolver& solver, const FlowBuiltinCall& call)
{
    const FlowCallOperands& operands = call.request.operands;
    Witness witness = witnessOf(solver, call);
    FlowNodeID unknownJavaScript = solver.graph().unknownJavaScript();

    didCallUndeclaredNativeCode(solver, call.request.site, witness);
    solver.connect(call.receiver, unknownJavaScript, { }, witness);
    solver.connect(operands.newTarget, unknownJavaScript, { }, witness);
    if (operands.newTargetObject)
        solver.joinObject(unknownJavaScript, operands.newTargetObject, witness);
    for (FlowNodeID position : call.arguments.positions)
        solver.connect(position, unknownJavaScript, { }, witness);
    solver.connect(call.arguments.rest, unknownJavaScript, { }, witness);

    solver.joinUnknown(operands.result, witness);
    solver.joinObject(operands.result, ensureBuiltinObject(solver, call.builtin), witness);
    throwUnknownValue(solver, operands.thrown, witness);
}

static void applyBuiltin(FlowSolver& solver, const FlowBuiltinCall& call)
{
    noteBuiltinUse(solver, call.builtin, builtinUseOf(call.mode), witnessOf(solver, call));
    applyCallAttachments(solver, call);
    if (standsForWhatItHolds(solver.builtins(), call.builtin)) {
        // Reading such a property starts nothing. A call of what was read does.
        if (call.mode == BuiltinCallMode::Get) {
            solver.joinUnknown(call.request.operands.result, witnessOf(solver, call));
            solver.joinObject(call.request.operands.result, ensureBuiltinObject(solver, call.builtin), witnessOf(solver, call));
            return;
        }
        if (!solver.builtins().dispatchOf(call.builtin, call.mode)) {
            callUndescribedMember(solver, call);
            return;
        }
        solver.joinObject(call.request.operands.result, ensureBuiltinObject(solver, call.builtin), witnessOf(solver, call));
    }
    forEachSelectedOverload(solver, call, [&](BuiltinOverloadID id) {
        applyOverload(solver, { call, id, solver.builtins().at(id) });
    });
}

void runBuiltinCall(FlowSolver& solver, FlowStatementSite site)
{
    const FlowBuiltinCallStatement& statement = solver.callState().builtins.statements[site.statement.index];
    if (statement.isAbandoned)
        return;

    // A call that leads back to its own statement passes it more operands during the run.
    FlowBuiltinCallKey key = statement.key;
    FlowBuiltinCallOperands operands = statement.operands;
    FlowCallRequest request {
        site,
        key.origin,
        key.kind,
        key.times,
        operands.isInCycle,
        {
            .thisValue = operands.receiver,
            .newTarget = operands.newTarget,
            .newTargetObject = operands.newTargetObject,
            .arguments = operands.arguments.positions.span(),
            .result = operands.result,
            .thrown = operands.thrown,
        },
    };

    if (key.callee.boundFunction) {
        runBoundFunctionCall(solver, request, operands.arguments, key.callee.boundFunction);
        return;
    }
    if (!key.callee.protocols.isEmpty()) {
        std::span<const BuiltinName> namedMembers = std::span(&key.callee.namedMember, key.callee.namedMember ? 1 : 0);
        runProtocols(solver, { site, key.origin, key.callee.values, key.callee.protocols, namedMembers, operands.result, operands.thrown });
        return;
    }
    if (key.callee.values) {
        runCallOfValues(solver, request, operands.arguments, key.callee.values);
        return;
    }
    applyBuiltin(solver, { request, operands.arguments, key.callee.builtin, key.callee.mode, key.callee.module, operands.receiver });
}

void recordOverload(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowBuiltinCall& call = overloadCall.call;
    const FlowCallRequest& request = call.request;
    FlowBuiltinCallStatement& statement = statementAt(solver.callState().builtins, request.site);
    if (!statement.recordedOverloads.contains(overloadCall.id)) {
        statement.recordedOverloads.append(overloadCall.id);
        solver.records().addBuiltinCallLink({ request.site.instance, request.origin, call.builtin, overloadCall.id, call.mode, request.kind });
    }

    // Each bound function that a site calls passes operands of its own under the site and the origin of that call.
    if (statement.overloadsWithRecordedOperands.contains(overloadCall.id))
        return;
    statement.overloadsWithRecordedOperands.append(overloadCall.id);
    recordOperandValues(solver, overloadCall);
}

static void recordOperandValue(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinOperand operand, OptionSet<BuiltinOperandRole> roles)
{
    const FlowCallRequest& request = overloadCall.call.request;
    FlowNodeID node;
    if (mayBePassed(overloadCall.call, operand))
        node = sourceNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::Operand, .operand = operand });
    if (solver.callState().builtins.recordedOperandValues.add(std::tuple { request.site, overloadCall.id, packedOperand(operand), node }).isNewEntry)
        solver.records().addBuiltinOperandValue({ request.site.instance, request.origin, overloadCall.id, operand, roles, node });
}

void recordOperandValues(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const BuiltinTable& builtins = solver.builtins();
    const BuiltinOverload& overload = overloadCall.overload;
    BuiltinID builtin = overloadCall.call.builtin;

    auto recordParameter = [&](const BuiltinParameter& parameter) {
        if (!parameter.roles.isEmpty())
            recordOperandValue(solver, overloadCall, parameter.operand, parameter.roles);
    };
    auto recordCondition = [&](const BuiltinCondition* condition) {
        if (condition && !condition->buildAdmitsRuntimeCode && !condition->callRunsOtherCode)
            recordOperandValue(solver, overloadCall, condition->operand, { });
    };

    if (overload.receiver)
        recordParameter(*overload.receiver);
    for (const BuiltinParameter& parameter : overload.parameters)
        recordParameter(parameter);

    for (const BuiltinFeatureUse& use : builtins.featureUsesOf(builtin)) {
        if (!use.overload || use.overload == overloadCall.id)
            recordCondition(use.condition);
    }
    for (const BuiltinModuleLoad& load : builtins.moduleLoadsOf(builtin))
        recordCondition(load.condition);
    if (const BuiltinUnseenCode* unseenCode = builtins.unseenCodeOf(builtin))
        recordCondition(unseenCode->condition);
}

void applyUndeclaredOverload(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowBuiltinCall& call = overloadCall.call;
    const FlowCallRequest& request = call.request;
    const FlowCallOperands& operands = request.operands;
    Witness witness = witnessOf(solver, call);

    for (const BuiltinParameter& parameter : overloadCall.overload.parameters)
        applyParameter(solver, overloadCall, parameter);

    // Nothing says what such native code does to an operand that seen code passes to it, so everything can happen to that object, and to that object alone.
    // A statement of native code's own stands for a call of seen code whose callee nobody knows, and the operands of such a call reach native code and keep their shape.
    bool claimsEveryUse = isInSeenCode(solver, request.site);
    auto pass = [&](FlowNodeID operand) {
        if (claimsEveryUse)
            applyOperandBehavior(solver, { request.site, request.origin, operand, { }, operands.result, operands.thrown }, OperandBehavior());
        else
            handToUnknownRecipient(solver, operand, witness);
    };
    pass(escapingReceiverNodeOf(solver, call));
    pass(operands.newTarget);
    if (operands.newTargetObject) {
        if (claimsEveryUse)
            solver.joinObject(solver.graph().unknownJavaScript(), operands.newTargetObject, witness);
        else
            handToUnknownRecipient(solver, operands.newTargetObject, witness);
    }

    solver.joinUnknown(operands.result, witness);
    throwUnknownValue(solver, operands.thrown, witness);
}

// The call stands for each call of seen code whose callee nobody knows, and for what native code without a declaration does with a function among its operands.
// So the operands are what was handed to such a callee or to such native code, not whatever has reached native code.
void callBuiltinFromNativeCode(FlowSolver& solver, FlowObjectID builtinObject)
{
    BuiltinID builtin = solver.objects().at(builtinObject).builtin;
    FlowNodeID handed = valuesHandedToUnknownRecipients(solver);
    FlowPassedArguments arguments { { }, handed, { 0, Range::length().maximum } };

    for (FlowCallLinkKind kind : { FlowCallLinkKind::Host, FlowCallLinkKind::Construct }) {
        if (!solver.builtins().dispatchOf(builtin, builtinCallModeOf(kind)))
            continue;
        FlowCallRequest request {
            nativeCodeSite(solver),
            ValueID(),
            kind,
            BuiltinCallbackTimes::Repeatedly,
            true,
            { .thisValue = handed, .result = solver.graph().nativeCode() },
        };
        callBuiltinObject(solver, request, arguments, builtinObject);
    }
}

void callBuiltinAccessorFromNativeCode(FlowSolver& solver, BuiltinID accessor, BuiltinCallMode mode)
{
    FlowNodeID unknown = unknownValueNode(solver);
    FlowCallRequest request {
        nativeCodeSite(solver),
        ValueID(),
        FlowCallLinkKind::Host,
        BuiltinCallbackTimes::Repeatedly,
        true,
        { .thisValue = unknown, .argumentList = unknown, .result = solver.graph().nativeCode() },
    };
    callBuiltin(solver, request, accessor, mode);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
