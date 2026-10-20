/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCallState.h"

#if ENABLE(AOT)

#include <algorithm>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static Subject subjectOf(const FlowSolver& solver, const FlowCallRequest& request)
{
    const FlowGraph& graph = solver.graph();
    if (graph.isHost(request.site.instance))
        return { };
    return { .function = graph.at(request.site.instance).function, .value = request.origin };
}

static bool isRestOperand(const BuiltinPlace& place)
{
    return place.kind == BuiltinPlaceKind::Operand && place.operand->kind() == BuiltinOperand::Kind::ArgumentsFrom;
}

static FlowNodeID restNodeOf(const FlowGraph& graph, FlowObjectID list)
{
    FlowNodeID rest = contentsNode(graph, list, ContentsSlot::Element);
    return rest ? rest : graph.nativeCode();
}

static FlowObjectID allocateArgumentList(FlowSolver& solver, const FlowOverloadCall& overloadCall, uint64_t numberOfElements, uint32_t ordinalInOverload, FlowNodeID holder)
{
    const FlowCallRequest& request = overloadCall.call.request;
    Witness witness = witnessOf(solver, overloadCall.call);
    uint64_t maxPositions = solver.callState().limits.maxPositionalParts;

    FlowObjectID list = allocateAtCallSite(solver, { request.site, {
        .kind = FlowSiteKind::Instance,
        .origin = FlowSiteOrigin::BuiltinElement,
        .numberOfPositions = static_cast<uint16_t>(std::min(numberOfElements, maxPositions)),
        .isCreatedDense = true,
        .value = request.origin,
        .builtinClass = BuiltinClasses::Array,
        .ordinal = overloadCall.id.index() << 16 | ordinalInOverload,
    } });

    bool isFirstRun = !solver.valueOf(holder).objects.contains(list);
    if (isFirstRun && numberOfElements > maxPositions)
        solver.build().gaveUp(LimitID::MaxPositionalParts, subjectOf(solver, request));
    if (list != unknownFlowObject && mayRunRepeatedly(request))
        solver.noteEvent(list, FlowObjectEvent::AllocatedRepeatedly, witness);

    solver.joinObject(holder, list, witness);
    return list;
}

static void storeArgumentsByPosition(FlowSolver& solver, const FlowOverloadCall& overloadCall, uint32_t firstArgument, FlowObjectID list)
{
    const FlowPassedArguments& arguments = overloadCall.call.arguments;
    const FlowGraph& graph = solver.graph();
    Witness witness = witnessOf(solver, overloadCall.call);
    uint32_t numberOfPositions = solver.objects().at(list).numberOfPositions;
    FlowNodeID rest = restNodeOf(graph, list);

    for (uint32_t index = firstArgument; index < arguments.positions.size(); ++index) {
        uint32_t position = index - firstArgument;
        FlowNodeID target = rest;
        if (position < numberOfPositions) {
            target = graph.position(list, position);
            solver.noteLocationEvent(target, FlowLocationEvent::InitializedAtAllocation);
        }
        if (FlowNodeID argument = arguments.positions[index])
            solver.connect(argument, target, { }, witness);
        else
            solver.joinPrimitives(target, Primitive::Undefined, witness);
    }
    solver.connect(arguments.rest, rest, { }, witness);
}

static void createArgumentList(FlowSolver& solver, const FlowOverloadCall& overloadCall, uint32_t firstArgument, uint32_t ordinalInOverload, FlowNodeID holder)
{
    uint64_t numberOfArguments = overloadCall.call.arguments.positions.size();
    uint64_t numberOfElements = numberOfArguments > firstArgument ? numberOfArguments - firstArgument : 0;
    FlowObjectID list = allocateArgumentList(solver, overloadCall, numberOfElements, ordinalInOverload, holder);
    storeArgumentsByPosition(solver, overloadCall, firstArgument, list);
}

static void storeBoundArguments(FlowSolver& solver, const FlowOverloadCall& overloadCall, uint32_t firstArgument)
{
    BuiltinSlotID slot = BuiltinSlots::BoundFunctionBoundArguments;
    forEachResultObject(solver, overloadCall, [&](FlowObjectID boundFunction) {
        if (FlowNodeID holder = solver.graph().slot(boundFunction, slot)) {
            createArgumentList(solver, overloadCall, firstArgument, slot.index(), holder);
            return;
        }
        forEachOperandNode(overloadCall.call, BuiltinOperand::argumentsFrom(firstArgument), [&](FlowNodeID argument) {
            connectIntoSlot(solver, overloadCall, argument, boundFunction, slot);
        });
    });
}

bool mayBeMissing(const FlowBuiltinCall& call, const BuiltinPlace& place)
{
    if (place.kind != BuiltinPlaceKind::Operand)
        return false;

    BuiltinOperand operand = *place.operand;
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
    case BuiltinOperand::Kind::Argument:
        return !isAlwaysPassed(call, operand) || !operandNodeOf(call, operand);
    case BuiltinOperand::Kind::ArgumentsFrom: {
        std::span<const FlowNodeID> positions = call.arguments.positions.span();
        return std::ranges::contains(positions.subspan(std::min<size_t>(operand.argumentIndex(), positions.size())), FlowNodeID());
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool holdsPartsOfOperand(const BuiltinPlace& place)
{
    switch (place.kind) {
    case BuiltinPlaceKind::Slot:
    case BuiltinPlaceKind::IteratedValues:
    case BuiltinPlaceKind::OwnValues:
    case BuiltinPlaceKind::Prototype:
    case BuiltinPlaceKind::Member:
        return true;
    case BuiltinPlaceKind::None:
    case BuiltinPlaceKind::Operand:
    case BuiltinPlaceKind::Result:
    case BuiltinPlaceKind::ResultSlot:
    case BuiltinPlaceKind::ResultField:
    case BuiltinPlaceKind::NewValue:
    case BuiltinPlaceKind::NativeMemory:
    case BuiltinPlaceKind::NativeCell:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void applyTransfers(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    Witness witness = witnessOf(solver, overloadCall.call);
    for (const BuiltinTransfer& transfer : overloadCall.overload.flow.transfers) {
        bool storesRestArguments = isRestOperand(transfer.from) && transfer.to.kind == BuiltinPlaceKind::ResultSlot;
        if (storesRestArguments && transfer.to.slot == BuiltinSlots::BoundFunctionBoundArguments) {
            storeBoundArguments(solver, overloadCall, transfer.from.operand->argumentIndex());
            continue;
        }

        FlowNodeID source = sourceNodeOf(solver, overloadCall, transfer.from);
        FlowNodeID target = targetNodeOf(solver, overloadCall, transfer.to);
        solver.connect(source, target, { }, witness);
        if (holdsPartsOfOperand(transfer.from)) {
            forEachOperandNode(overloadCall.call, *transfer.from.operand, [&](FlowNodeID operand) {
                solver.records().addContentsRead(operand, source);
            });
        }
        if (mayBeMissing(overloadCall.call, transfer.from))
            solver.joinPrimitives(target, Primitive::Undefined, witness);
    }
}

void applyCallbacks(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    for (const BuiltinCallback& callback : overloadCall.overload.flow.callbacks)
        applyCallback(solver, overloadCall, callback);
}

static FlowNodeID argumentListsNode(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place)
{
    return solver.temporaryFor(callTemporaryKey(overloadCall.call.request.site, FlowCallTemporaryRole::ArgumentLists, packedPlace(place)));
}

static FlowNodeID spreadRestArguments(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinCallback& callback, const BuiltinPlace& place, Vector<FlowNodeID, 8>& arguments)
{
    const FlowPassedArguments& passed = overloadCall.call.arguments;
    uint32_t index = place.operand->argumentIndex();
    for (; index < passed.positions.size() && passed.alwaysPasses(index); ++index)
        arguments.append(passed.positions[index]);
    if (!passed.mayPass(index))
        return { };

    FlowNodeID lists = argumentListsNode(solver, overloadCall, place);
    createArgumentList(solver, overloadCall, index, packedOperand(callback.callee), lists);
    return lists;
}

static FlowNodeID spreadValues(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinCallback& callback, const BuiltinPlace& place)
{
    FlowNodeID lists = argumentListsNode(solver, overloadCall, place);
    FlowObjectID list = allocateArgumentList(solver, overloadCall, 0, packedOperand(callback.callee), lists);
    solver.connect(sourceNodeOf(solver, overloadCall, place), restNodeOf(solver.graph(), list), { }, witnessOf(solver, overloadCall.call));
    return lists;
}

static FlowNodeID spreadArguments(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinCallback& callback, const BuiltinPlace& place, Vector<FlowNodeID, 8>& arguments)
{
    switch (place.kind) {
    case BuiltinPlaceKind::None:
        return { };
    case BuiltinPlaceKind::Operand:
        if (isRestOperand(place))
            return spreadRestArguments(solver, overloadCall, callback, place, arguments);
        return operandNodeOf(overloadCall.call, *place.operand);
    case BuiltinPlaceKind::OwnValues:
        if (place.operand->kind() == BuiltinOperand::Kind::ArgumentsFrom)
            return spreadValues(solver, overloadCall, callback, place);
        return operandNodeOf(overloadCall.call, *place.operand);
    case BuiltinPlaceKind::Slot:
    case BuiltinPlaceKind::IteratedValues:
    case BuiltinPlaceKind::Result:
    case BuiltinPlaceKind::ResultSlot:
    case BuiltinPlaceKind::ResultField:
    case BuiltinPlaceKind::NewValue:
    case BuiltinPlaceKind::NativeMemory:
    case BuiltinPlaceKind::Prototype:
    case BuiltinPlaceKind::Member:
    case BuiltinPlaceKind::NativeCell:
        return spreadValues(solver, overloadCall, callback, place);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FlowNodeID callbackResultNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinCallback& callback)
{
    switch (callback.resultUse) {
    case BuiltinCallbackResultUse::Escapes:
        return solver.graph().nativeCode();
    case BuiltinCallbackResultUse::Ignored:
    case BuiltinCallbackResultUse::Tested:
        return { };
    case BuiltinCallbackResultUse::Returned:
        return targetNodeOf(solver, overloadCall, { .kind = BuiltinPlaceKind::Result });
    case BuiltinCallbackResultUse::Stored:
        return targetNodeOf(solver, overloadCall, callback.resultTo);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasCallbackResultPart(const BuiltinTable& builtins, BuiltinTypeID type, BuiltinOperand callee, FlowBuiltinTypePath& path)
{
    if (path.contains(type))
        return false;
    path.append(type);
    bool found = std::ranges::any_of(builtins.at(type).parts, [&](const BuiltinTypePart& part) {
        if (part.kind == BuiltinTypePartKind::CallbackResult)
            return part.operand == callee;
        return std::ranges::any_of(part.contents, [&](const BuiltinSlotContents& contents) {
            return hasCallbackResultPart(builtins, contents.type, callee, path);
        });
    });
    path.removeLast();
    return found;
}

static bool passesOnResultsOf(const BuiltinTable& builtins, const BuiltinFlow& flow, BuiltinOperand callee)
{
    auto holdsResults = [&](const BuiltinPlace& place) {
        FlowBuiltinTypePath path;
        return place.kind == BuiltinPlaceKind::NewValue && hasCallbackResultPart(builtins, place.newValueType, callee, path);
    };
    return std::ranges::any_of(flow.transfers, holdsResults, &BuiltinTransfer::from)
        || std::ranges::any_of(flow.propertyActions, holdsResults, &BuiltinPropertyAction::value)
        || std::ranges::any_of(flow.callbacks, [&](const BuiltinCallback& callback) {
            return holdsResults(callback.thisValue) || holdsResults(callback.spreadArguments) || std::ranges::any_of(callback.arguments, holdsResults);
        });
}

FlowNodeID callbackResultsNode(FlowSolver& solver, const FlowOverloadCall& overloadCall, BuiltinOperand callee)
{
    return solver.temporaryFor(callTemporaryKey(overloadCall.call.request.site, FlowCallTemporaryRole::CallbackResults, overloadCall.id.index() << 16 | packedOperand(callee)));
}

static BuiltinPlace placeOfOperandPart(const BuiltinTypePart& part)
{
    switch (part.kind) {
    case BuiltinTypePartKind::Operand:
        return { .kind = BuiltinPlaceKind::Operand, .operand = part.operand };
    case BuiltinTypePartKind::Slot:
        return { .kind = BuiltinPlaceKind::Slot, .operand = part.operand, .slot = part.slot };
    case BuiltinTypePartKind::IteratedValues:
        return { .kind = BuiltinPlaceKind::IteratedValues, .operand = part.operand, .isSettled = part.isSettled };
    case BuiltinTypePartKind::Prototype:
        return { .kind = BuiltinPlaceKind::Prototype, .operand = part.operand };
    case BuiltinTypePartKind::Instance:
    case BuiltinTypePartKind::InstanceOfReceiverClass:
    case BuiltinTypePartKind::TypedArrayOfReceiverElementType:
    case BuiltinTypePartKind::Layout:
    case BuiltinTypePartKind::Builtin:
    case BuiltinTypePartKind::CallbackResult:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The values reach user code from a node that the parts of the object feed, so nothing at the operand says what becomes of its contents.
static void notePassedContents(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinPlace& place)
{
    if (!holdsPartsOfOperand(place))
        return;
    forEachOperandNode(overloadCall.call, *place.operand, [&](FlowNodeID node) {
        solver.noteDirectUses(node, { OperandUse::RetainsContents, OperandUse::AppliesTransitively });
    });
}

void connectOperandPart(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinTypePart& part, FlowNodeID target)
{
    if (!target)
        return;

    Witness witness = witnessOf(solver, overloadCall.call);
    if (part.kind == BuiltinTypePartKind::CallbackResult) {
        solver.connect(callbackResultsNode(solver, overloadCall, *part.operand), target, { }, witness);
        return;
    }

    BuiltinPlace place = placeOfOperandPart(part);
    solver.connect(sourceNodeOf(solver, overloadCall, place), target, { }, witness);
    notePassedContents(solver, overloadCall, place);
    if (mayBeMissing(overloadCall.call, place))
        solver.joinPrimitives(target, Primitive::Undefined, witness);
}

static BuiltinCallbackTimes timesUnder(BuiltinCallbackTimes outer, BuiltinCallbackTimes inner)
{
    if (outer == BuiltinCallbackTimes::Once)
        return inner;
    if (inner == BuiltinCallbackTimes::Once)
        return outer;
    if (outer == BuiltinCallbackTimes::PerEvent || inner == BuiltinCallbackTimes::PerEvent)
        return BuiltinCallbackTimes::PerEvent;
    if (outer == BuiltinCallbackTimes::Later || inner == BuiltinCallbackTimes::Later)
        return BuiltinCallbackTimes::Later;
    return outer == inner ? outer : BuiltinCallbackTimes::Repeatedly;
}

void applyCallback(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinCallback& callback)
{
    const FlowCallRequest& request = overloadCall.call.request;

    std::span<const BuiltinPlace> places = callback.arguments;
    const BuiltinPlace* spread = &callback.spreadArguments;
    if (spread->kind == BuiltinPlaceKind::None && !places.empty() && isRestOperand(places.back())) {
        spread = &places.back();
        places = places.first(places.size() - 1);
    }

    Vector<FlowNodeID, 8> arguments;
    for (const BuiltinPlace& place : places)
        arguments.append(sourceNodeOf(solver, overloadCall, place));
    FlowNodeID argumentList = spreadArguments(solver, overloadCall, callback, *spread, arguments);
    FlowNodeID result = callbackResultNodeOf(solver, overloadCall, callback);
    FlowNodeID thisValue = sourceNodeOf(solver, overloadCall, callback.thisValue);
    FlowNodeID newTarget = sourceNodeOf(solver, overloadCall, callback.newTarget);
    FlowNodeID thrown = thrownNodeOf(solver, overloadCall, callback);

    if (passesOnResultsOf(solver.builtins(), overloadCall.overload.flow, callback.callee)) {
        FlowNodeID results = callbackResultsNode(solver, overloadCall, callback.callee);
        solver.connect(results, result, { }, witnessOf(solver, overloadCall.call));
        result = results;
    }

    notePassedContents(solver, overloadCall, callback.newTarget);
    notePassedContents(solver, overloadCall, callback.thisValue);
    notePassedContents(solver, overloadCall, *spread);
    for (const BuiltinPlace& place : places)
        notePassedContents(solver, overloadCall, place);

    // The built-in keeps what it passes until it calls, after the frame of its caller is gone.
    if (isCalledAfterReturn(callback.times)) {
        solver.noteDirectUses(newTarget, OperandUse::Retains);
        solver.noteDirectUses(thisValue, OperandUse::Retains);
        solver.noteDirectUses(argumentList, OperandUse::Retains);
        for (FlowNodeID argument : arguments)
            solver.noteDirectUses(argument, OperandUse::Retains);
    }

    FlowCallRequest inner {
        .site = request.site,
        .origin = request.origin,
        .kind = callback.constructs ? FlowCallLinkKind::Construct : FlowCallLinkKind::BuiltinCallback,
        .times = timesUnder(request.times, callback.times),
        .isInCycle = request.isInCycle,
        .operands = {
            .thisValue = thisValue,
            .newTarget = newTarget,
            .arguments = arguments.span(),
            .argumentList = argumentList,
            .result = result,
            .thrown = thrown,
        },
    };
    forEachOperandNode(overloadCall.call, callback.callee, [&](FlowNodeID callees) {
        callValuesOf(solver, inner, callees);
    });
}

static void applyHandlerCall(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinHandlerCall& handler)
{
    const FlowCallRequest& request = overloadCall.call.request;
    FlowNodeID callees = sourceNodeOf(solver, overloadCall, handler.callee);
    if (!callees)
        return;
    if (holdsPartsOfOperand(handler.callee)) {
        forEachOperandNode(overloadCall.call, *handler.callee.operand, [&](FlowNodeID operand) {
            solver.records().addContentsRead(operand, callees);
        });
    }

    Vector<FlowNodeID, 8> arguments;
    for (const BuiltinPlace& place : handler.arguments) {
        arguments.append(sourceNodeOf(solver, overloadCall, place));
        notePassedContents(solver, overloadCall, place);
    }
    FlowNodeID thisValue = sourceNodeOf(solver, overloadCall, handler.thisValue);
    notePassedContents(solver, overloadCall, handler.thisValue);

    bool isCalledLater = isCalledAfterReturn(handler.times);
    if (isCalledLater) {
        solver.noteDirectUses(callees, OperandUse::Retains);
        solver.noteDirectUses(thisValue, OperandUse::Retains);
        for (FlowNodeID argument : arguments)
            solver.noteDirectUses(argument, OperandUse::Retains);
    }

    // The host reports what a handler throws after the call has returned. It reaches no code of the program from here.
    FlowCallRequest inner {
        .site = request.site,
        .origin = request.origin,
        .kind = FlowCallLinkKind::BuiltinCallback,
        .times = timesUnder(request.times, handler.times),
        .isInCycle = request.isInCycle,
        .operands = {
            .thisValue = thisValue,
            .arguments = arguments.span(),
            .result = handler.resultUse == BuiltinCallbackResultUse::Escapes ? solver.graph().nativeCode() : FlowNodeID(),
            .thrown = isCalledLater ? FlowNodeID() : request.operands.thrown,
        },
    };
    callValuesOf(solver, inner, callees);
}

void applyHandlerCalls(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    for (const BuiltinHandlerCall& handler : overloadCall.overload.flow.handlerCalls)
        applyHandlerCall(solver, overloadCall, handler);
}

FlowNodeID thrownNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall, const BuiltinCallback& callback)
{
    if (!isCalledAfterReturn(callback.times))
        return operandCodeThrownNodeOf(solver, overloadCall);
    return rejectionNodeOf(solver, overloadCall);
}

// A built-in that answers with a new promise either throws what the code of an operand throws or rejects the promise with it.
FlowNodeID operandCodeThrownNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    const FlowCallRequest& request = overloadCall.call.request;
    FlowNodeID rejected = rejectionNodeOf(solver, overloadCall);
    if (!rejected)
        return request.operands.thrown;

    FlowNodeID thrown = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::ThrownOrRejectedValue, overloadCall.id.index()));
    connectThrown(solver, thrown, request.operands.thrown, witnessOf(solver, overloadCall.call));
    solver.connect(thrown, rejected);
    return thrown;
}

static bool answersWithNewPromise(const BuiltinTable& builtins, const BuiltinOverload& overload)
{
    const BuiltinType& type = builtins.at(overload.result.type);
    return type.freshness != BuiltinFreshness::No && std::ranges::any_of(type.parts, [&](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Instance && builtins.isSameOrSubclassOf(part.builtinClass, BuiltinClasses::Promise);
    });
}

FlowNodeID rejectionNodeOf(FlowSolver& solver, const FlowOverloadCall& overloadCall)
{
    if (!answersWithNewPromise(solver.builtins(), overloadCall.overload))
        return { };

    const FlowCallRequest& request = overloadCall.call.request;
    Witness witness = witnessOf(solver, overloadCall.call);
    FlowNodeID rejected;
    forEachResultObject(solver, overloadCall, [&](FlowObjectID object) {
        if (!solver.objects().isInstanceOf(object, BuiltinClasses::Promise))
            return;
        if (!rejected)
            rejected = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::RejectedValue, overloadCall.id.index()));
        rejectPromiseWith(solver, object, rejected, witness);
    });
    return rejected;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
