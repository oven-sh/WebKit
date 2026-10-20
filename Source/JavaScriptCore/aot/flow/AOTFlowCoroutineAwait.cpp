/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCoroutines.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static Witness witnessOf(const FlowSolver& solver, const FlowAwaitRequest& request)
{
    return solver.witnessAt(request.site, request.origin);
}

static void settleWithUnknownValue(FlowSolver& solver, const FlowAwaitRequest& request)
{
    Witness witness = witnessOf(solver, request);
    solver.joinUnknown(request.result, witness);
    throwUnknownValue(solver, request.thrown, witness);
}

static void awaitPrimitivesOf(FlowSolver& solver, const FlowAwaitRequest& request, FlowNodeID value)
{
    solver.join(request.result, solver.valueOf(value).withoutObjects());
}

static bool mayBeOtherConstructor(FlowSolver& solver, const FlowAwaitRequest& request, const FlowPropertySource& source)
{
    switch (source.kind) {
    case FlowPropertySourceKind::BuiltinMember:
        return solver.builtins().canonicalID(source.builtin) != Builtins::Promise;
    case FlowPropertySourceKind::Value:
    case FlowPropertySourceKind::Getter:
        solver.waitOn(source.node, request.site, FlowWakeUp::Revisit);
        return !solver.valueOf(source.node).isEmpty();
    case FlowPropertySourceKind::Unknown:
    case FlowPropertySourceKind::BuiltinGetter:
        return true;
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// PromiseResolve reads the property before anything is awaited. A promise with another constructor than Promise is settled by Promise.prototype.then, which constructs the species of that constructor.
static void readPromiseConstructor(FlowSolver& solver, const FlowAwaitRequest& request, FlowObjectID promise)
{
    const Program& program = solver.program();
    FlowKeySet keys = keySetOf(program, program.keyOf(BuiltinNames::constructor));

    bool mayHaveOtherConstructor = false;
    forEachPropertySource(solver, promise, keys, { FlowWalkOperation::Get, false, request.site }, [&](const FlowPropertySource& source) {
        mayHaveOtherConstructor |= mayBeOtherConstructor(solver, request, source);
    });
    if (!mayHaveOtherConstructor) {
        FlowAccess read { .site = request.site, .origin = request.origin, .kind = FlowAccessKind::Load, .flags = FlowAccessFlag::IsInCycle, .thrown = request.thrown };
        accessProperties(solver, read, FlowObjectSet(promise), keys);
        return;
    }

    FlowNodeID promises = solver.temporaryFor(callTemporaryKey(request.site, FlowCallTemporaryRole::PromisesSettledByThen));
    solver.joinObject(promises, promise);
    runProtocolsInOwnStatement(solver, { request.site, request.origin, promises, Protocol::Species, { }, solver.graph().nativeCode(), request.thrown });
}

static void awaitPromise(FlowSolver& solver, const FlowAwaitRequest& request, FlowObjectID promise)
{
    readPromiseConstructor(solver, request, promise);

    const FlowGraph& graph = solver.graph();
    FlowNodeID valueSlot = graph.slot(promise, BuiltinSlots::PromiseValue);
    if (!valueSlot) {
        settleWithUnknownValue(solver, request);
        return;
    }

    Witness witness = witnessOf(solver, request);
    solver.connect(valueSlot, request.result, { }, witness);
    if (request.thrown)
        connectThrown(solver, graph.slot(promise, BuiltinSlots::PromiseReason), request.thrown, witness);
}

static void callThen(FlowSolver& solver, const FlowAwaitRequest& request, FlowObjectID thenable, const FlowPropertySource& source)
{
    std::array<FlowNodeID, 2> resolvingFunctions { unknownValueNode(solver), unknownValueNode(solver) };
    FlowCallRequest call {
        request.site,
        request.origin,
        FlowCallLinkKind::Protocol,
        BuiltinCallbackTimes::Later,
        true,
        { .arguments = resolvingFunctions, .thrown = request.thrown },
    };
    callMethodSource(solver, call, thenable, 0, source);
    settleWithUnknownValue(solver, request);
    solver.joinObject(request.result, thenable);
}

static void awaitWithThenNode(FlowSolver& solver, const FlowAwaitRequest& request, FlowObjectID object, const FlowPropertySource& source)
{
    solver.waitOn(source.node, request.site, FlowWakeUp::Revisit);

    const FlowValue& then = solver.valueOf(source.node);
    if (then.mayBeObject())
        callThen(solver, request, object, source);
    else if (!then.isEmpty())
        solver.joinObject(request.result, object);
}

static void awaitWithThenSource(FlowSolver& solver, const FlowAwaitRequest& request, FlowObjectID object, const FlowPropertySource& source)
{
    switch (source.kind) {
    case FlowPropertySourceKind::Absent:
        solver.joinObject(request.result, object);
        return;
    case FlowPropertySourceKind::BuiltinMember:
        if (solver.builtins().canonicalID(source.builtin) == Builtins::PromisePrototypeThen)
            awaitPromise(solver, request, object);
        else
            callThen(solver, request, object, source);
        return;
    case FlowPropertySourceKind::Value:
    case FlowPropertySourceKind::Getter:
        awaitWithThenNode(solver, request, object, source);
        return;
    case FlowPropertySourceKind::Unknown:
    case FlowPropertySourceKind::BuiltinGetter:
        callThen(solver, request, object, source);
        return;
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void runAwait(FlowSolver& solver, FlowStatementSite site, const FlowAwait& await)
{
    FlowAwaitRequest request { site, await.origin, solver.node(site, await.result), solver.node(site, await.thrown) };
    FlowNodeID value = solver.node(site, await.value);
    if (!value) {
        solver.joinPrimitives(request.result, Primitive::Undefined);
        return;
    }

    awaitObjects(solver, request, solver.takeNewObjects(site, 0, value));
    awaitPrimitivesOf(solver, request, value);
}

void awaitValuesOf(FlowSolver& solver, const FlowAwaitRequest& request, FlowNodeID value)
{
    if (!value)
        return;

    // Every run of the statement has to come here: the cursor hands out what has arrived since the last one.
    // An object that is no thenable is joined into the result one by one. No value is inside never: the edge only says, to whoever follows edges, that the value is used as the result is.
    solver.connect(value, request.result, TypeTable::never);
    awaitObjects(solver, request, solver.takeNewObjects(callTemporaryKey(request.site, FlowCallTemporaryRole::AwaitedObjects, value.index()), value));
    awaitPrimitivesOf(solver, request, value);
}

void awaitObjects(FlowSolver& solver, const FlowAwaitRequest& request, const FlowNewObjects& objects)
{
    solver.forEachMember(objects, [&](FlowObjectID object) {
        if (object == unknownFlowObject) {
            settleWithUnknownValue(solver, request);
            return;
        }
        solver.noteUse(object, FlowObjectUse::Awaited);
        awaitThenable(solver, request, object);
    });
}

void awaitThenable(FlowSolver& solver, const FlowAwaitRequest& request, FlowObjectID object)
{
    const Program& program = solver.program();
    FlowKeySet keys = keySetOf(program, program.keyOf(BuiltinNames::then));
    forEachPropertySource(solver, object, keys, { FlowWalkOperation::Get, false, request.site }, [&](const FlowPropertySource& source) {
        awaitWithThenSource(solver, request, object, source);
    });
}

void resolvePromiseWith(FlowSolver& solver, FlowStatementSite site, ValueID origin, FlowObjectID promise, FlowNodeID value)
{
    if (!value)
        return;

    FlowNodeID resolved = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::ResolvedValue, value.index()));
    FlowNodeID rejected = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::RejectedValue, value.index()));

    // The caller may be inside a loop over the members of a cursor, so a statement of its own awaits what arrives in the value.
    // No value is inside never: the edge only says, to whoever follows edges, that the value is used as the resolved value is.
    solver.connect(value, resolved, TypeTable::never);
    runProtocolsInOwnStatement(solver, { site, origin, value, Protocol::Thenable, { }, resolved, rejected });

    const FlowGraph& graph = solver.graph();
    Witness witness = solver.witnessAt(site, origin);
    FlowNodeID valueSlot = graph.slot(promise, BuiltinSlots::PromiseValue);
    if (valueSlot)
        solver.connect(resolved, valueSlot, { }, witness);
    else
        handToUnknownRecipient(solver, resolved, witness);
    rejectPromiseWith(solver, promise, rejected, witness);
}

void rejectPromiseWith(FlowSolver& solver, FlowObjectID promise, FlowNodeID reason, const Witness& witness)
{
    const FlowGraph& graph = solver.graph();
    solver.connect(reason, graph.slot(promise, BuiltinSlots::PromiseReason), { }, witness);
    // The host reports a rejection that no handler takes.
    solver.connect(reason, graph.nativeCode(), { }, witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
