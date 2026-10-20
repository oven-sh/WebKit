/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowCoroutines.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"

namespace JSC { namespace AOT { namespace Typed {

std::optional<CoroutineKind> coroutineKindOf(const Function& function)
{
    bool isAsync = function.has(FunctionFlags::IsAsync);
    bool isGenerator = function.has(FunctionFlags::IsGenerator);
    if (isAsync && isGenerator)
        return CoroutineKind::AsyncGenerator;
    if (isAsync)
        return CoroutineKind::AsyncFunction;
    if (isGenerator)
        return CoroutineKind::Generator;
    return std::nullopt;
}

BuiltinClassID resultClassOf(CoroutineKind kind)
{
    switch (kind) {
    case CoroutineKind::Generator:
        return BuiltinClasses::Generator;
    case CoroutineKind::AsyncFunction:
        return BuiltinClasses::Promise;
    case CoroutineKind::AsyncGenerator:
        return BuiltinClasses::AsyncGenerator;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void passSentValue(FlowSolver& solver, FlowNodeID sent, FlowNodeID target, const Witness& witness)
{
    if (sent)
        solver.connect(sent, target, { }, witness);
    else
        solver.joinPrimitives(target, Primitive::Undefined, witness);
}

static void resumeBody(FlowSolver& solver, const FlowGeneratorResumption& resumption, FlowInstanceID body)
{
    const FlowSummary& summary = solver.graph().summaryOf(body);
    Witness witness = solver.witnessAt(resumption.site, resumption.origin);

    FlowCallRequest request {
        resumption.site,
        resumption.origin,
        FlowCallLinkKind::Protocol,
        BuiltinCallbackTimes::Repeatedly,
        true,
        { .result = resumption.value, .thrown = resumption.thrown },
    };
    ensureCallLink(solver, request, body);

    passSentValue(solver, resumption.sent, solver.node(body, summary.coroutine.sent), witness);
    solver.connect(solver.node(body, summary.coroutine.yielded), resumption.value, { }, witness);
    solver.connect(solver.node(body, summary.result(0)), resumption.value, { }, witness);
    connectThrown(solver, solver.node(body, summary.thrown()), resumption.thrown, witness);

    switch (resumption.mode) {
    case ResumeMode::Next:
        return;
    case ResumeMode::Throw:
        passSentValue(solver, resumption.sent, thrownNodeOrNativeCode(solver, resumption.thrown), witness);
        return;
    case ResumeMode::Return:
        passSentValue(solver, resumption.sent, resumption.value, witness);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void linkPromise(FlowSolver& solver, const FlowCallRequest& request, FlowInstanceID body, FlowObjectID promise)
{
    const FlowSummary& summary = solver.graph().summaryOf(body);
    Witness witness = solver.witnessAt(request.site, request.origin);
    FlowNodeID returned = solver.node(body, summary.result(0));

    noteInstanceOf(solver, BuiltinClasses::Promise, witness);
    // No statement runs again for a call by native code, and nothing else can reach its promise.
    if (request.kind == FlowCallLinkKind::Host)
        solver.connect(returned, solver.graph().nativeCode(), { }, witness);
    else
        resolvePromiseWith(solver, request.site, request.origin, promise, returned);
    rejectPromiseWith(solver, promise, solver.node(body, summary.thrown()), witness);
}

static void linkGenerator(FlowSolver& solver, const FlowCallRequest& request, FlowInstanceID body, FlowObjectID generator, BuiltinSlotID elementSlot)
{
    FlowGraph& graph = solver.graph();
    if (generator == unknownFlowObject) {
        resumeBody(solver, { nativeCodeSite(solver), ValueID(), ResumeMode::Next, unknownValueNode(solver), graph.nativeCode(), FlowNodeID() }, body);
        return;
    }
    solver.connect(solver.node(body, graph.summaryOf(body).coroutine.yielded), graph.slot(generator, elementSlot), { }, solver.witnessAt(request.site, request.origin));
}

FlowObjectID linkCoroutineResult(FlowSolver& solver, const FlowCallRequest& request, FlowInstanceID callee)
{
    FlowStatementSite caller = request.kind == FlowCallLinkKind::Host ? FlowStatementSite() : request.site;
    FlowObjectID result = allocateCoroutineResult(solver, caller, request.origin, callee);
    if (result != unknownFlowObject) {
        solver.callState().coroutines.bodyOfResult.add(result, callee);
        if (mayRunRepeatedly(request))
            solver.noteEvent(result, FlowObjectEvent::AllocatedRepeatedly, solver.witnessAt(request.site, request.origin));
    }

    std::optional<CoroutineKind> kind = coroutineKindOf(solver.program().at(solver.graph().at(callee).function));
    RELEASE_ASSERT(kind);
    switch (*kind) {
    case CoroutineKind::Generator:
        linkGenerator(solver, request, callee, result, BuiltinSlots::GeneratorElement);
        return result;
    case CoroutineKind::AsyncFunction:
        linkPromise(solver, request, callee, result);
        return result;
    case CoroutineKind::AsyncGenerator:
        linkGenerator(solver, request, callee, result, BuiltinSlots::AsyncGeneratorElement);
        return result;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowInstanceID coroutineBodyOf(const FlowCallState& state, FlowObjectID result)
{
    return state.coroutines.bodyOfResult.get(result);
}

FlowGeneratorBody resumeGenerator(FlowSolver& solver, const FlowGeneratorResumption& resumption, FlowObjectID generator)
{
    FlowInstanceID body = coroutineBodyOf(solver.callState(), generator);
    if (!body || !solver.graph().summaryOf(body).coroutine.yielded)
        return FlowGeneratorBody::Unknown;

    resumeBody(solver, resumption, body);
    return FlowGeneratorBody::Resumed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
