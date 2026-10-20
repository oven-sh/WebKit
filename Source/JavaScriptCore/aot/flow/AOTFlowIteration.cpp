/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowIteration.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"

namespace JSC { namespace AOT { namespace Typed {

static FlowCallRequest protocolCall(const FlowIterationRequest& request, FlowNodeID receiver, FlowNodeID result, BuiltinCallbackTimes times)
{
    return { request.site, request.origin, FlowCallLinkKind::Protocol, times, request.isInCycle, { .thisValue = receiver, .result = result, .thrown = request.thrown } };
}

static FlowKeySet keysOf(const Program& program, BuiltinName name)
{
    return keySetOf(program, program.keyOf(name));
}

static void openIteratorsOfNode(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID iterable, const FlowNewObjects& newIterables, FlowNodeID state)
{
    if (!iterable)
        return;
    openIterators(solver, request, iterable, newIterables, state);
    if (uint16_t strings = solver.valueOf(iterable).primitives & Primitive::AnyString)
        openIteratorsOfPrimitives(solver, request, strings, state);
}

static bool isGeneratorResumption(const FlowSolver& solver, FlowStatementSite site, ValueID origin)
{
    if (!origin)
        return false;
    Opcode opcode = solver.operationAt(site, origin).opcode;
    return opcode == Opcode::ResumeGenerator || opcode == Opcode::StepGenerator;
}

static bool isArrayIteratorStep(const FlowSolver& solver, FlowStatementSite site, ValueID origin)
{
    return origin && solver.operationAt(site, origin).opcode == Opcode::StepArrayIterator;
}

static bool mayBeGenerator(const FlowObjectTable& objects, FlowObjectID object)
{
    return object == unknownFlowObject
        || objects.kindOf(object) == FlowObjectKind::Opaque
        || objects.isInstanceOf(object, BuiltinClasses::Generator)
        || objects.isInstanceOf(object, BuiltinClasses::AsyncGenerator);
}

// ResumeGenerator answers what the method of the prototype answers: an iterator result that the run time makes, an ordinary object. StepGenerator answers the value itself.
static FlowNodeID valueOfIteratorResult(FlowSolver& solver, const FlowIterationRequest& request, const FlowIteratorResumption& resumption)
{
    const Program& program = solver.program();
    const FlowGraph& graph = solver.graph();
    Witness witness = solver.witnessAt(request.site, request.origin);
    LayoutID layoutID = program.layoutOf(BuiltinLayouts::IteratorResult);

    FlowObjectID result = allocateAtCallSite(solver, {
        request.site,
        { .kind = FlowSiteKind::Layout, .origin = FlowSiteOrigin::BuiltinElement, .value = request.origin, .layout = layoutID },
    });
    if (result == unknownFlowObject) {
        solver.joinUnknownObject(resumption.value, witness);
        return graph.nativeCode();
    }
    if (request.isInCycle)
        solver.noteEvent(result, FlowObjectEvent::AllocatedRepeatedly, witness);
    solver.joinObject(resumption.value, result, witness);

    const Layout& layout = program.at(layoutID);
    std::optional<uint32_t> valueField = layout.findField(program.keyOf(BuiltinNames::value));
    std::optional<uint32_t> doneField = layout.findField(program.keyOf(BuiltinNames::done));
    RELEASE_ASSERT(valueField && doneField);
    solver.joinPrimitives(graph.field(result, *doneField), Primitive::Boolean, witness);

    // A generator that has completed answers undefined.
    FlowNodeID value = graph.field(result, *valueField);
    if (resumption.mode == ResumeMode::Next)
        solver.joinPrimitives(value, Primitive::Undefined, witness);
    return value;
}

static void resumeGenerators(FlowSolver& solver, const FlowIterationRequest& request, const FlowNewObjects& generators, const FlowIteratorResumption& resumption)
{
    Witness witness = solver.witnessAt(request.site, request.origin);
    bool answersIteratorResult = solver.operationAt(request.site, request.origin).opcode == Opcode::ResumeGenerator;
    solver.forEachMember(generators, [&](FlowObjectID generator) {
        bool hasBody = directIterationOf(solver, generator) == FlowDirectIteration::Generator;
        if (!hasBody && !mayBeGenerator(solver.objects(), generator))
            return;

        FlowNodeID value = answersIteratorResult ? valueOfIteratorResult(solver, request, resumption) : resumption.value;
        if (hasBody) {
            FlowGeneratorBody body = resumeGenerator(solver, { request.site, request.origin, resumption.mode, resumption.sent, value, request.thrown }, generator);
            RELEASE_ASSERT(body == FlowGeneratorBody::Resumed);
            return;
        }

        solver.joinUnknown(value, witness);
        throwUnknownValue(solver, request.thrown, witness);
        handToUnknownRecipient(solver, resumption.sent, witness);
    });
}

static void checkSentValue(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID sent)
{
    if (!sent)
        return;
    FlowNodeID arguments[1] = { sent };
    FlowCallRequest call = protocolCall(request, { }, { }, BuiltinCallbackTimes::Repeatedly);
    call.operands.arguments = arguments;
    contextOf(solver, call, expandArguments(solver, call), FlowObjectSet());
}

void runIteration(FlowSolver& solver, FlowStatementSite site, const FlowIteration& iteration)
{
    FlowIterationRequest request { site, iteration.origin, iteration.iteratorKind, true, solver.node(site, iteration.thrown) };
    FlowNodeID state = solver.node(site, iteration.state);

    switch (iteration.step) {
    case FlowIterationStep::Open: {
        FlowNodeID iterable = solver.node(site, iteration.iterable);
        openIteratorsOfNode(solver, request, iterable, solver.takeNewObjects(site, 0, iterable), state);
        return;
    }
    case FlowIterationStep::Resume: {
        FlowIteratorResumption resumption { iteration.resumeMode, solver.node(site, iteration.sent), solver.node(site, iteration.value) };
        FlowNewObjects iterators = solver.takeNewObjects(site, 0, state);
        if (isGeneratorResumption(solver, site, iteration.origin)) {
            resumeGenerators(solver, request, iterators, resumption);
            return;
        }
        if (isArrayIteratorStep(solver, site, iteration.origin)) {
            stepArrayIterators(solver, request, state, iterators, resumption);
            return;
        }
        checkSentValue(solver, request, resumption.sent);
        resumeIterators(solver, request, state, iterators, resumption);
        return;
    }
    case FlowIterationStep::Close:
        closeIterators(solver, request, state, solver.takeNewObjects(site, 0, state));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void openIterators(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID iterable, const FlowNewObjects& iterables, FlowNodeID state)
{
    solver.noteUseOf(iterables, FlowObjectUse::Iterated);
    switch (request.kind) {
    case IteratorKind::Sync:
        callMethods(solver, protocolCall(request, iterable, state, BuiltinCallbackTimes::Once), iterables, keysOf(solver.program(), BuiltinNames::iteratorSymbol));
        return;
    case IteratorKind::Async:
        solver.forEachMember(iterables, [&](FlowObjectID object) {
            openAsyncIterator(solver, request, iterable, object, state);
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void openAsyncIteratorFrom(FlowSolver& solver, const FlowCallRequest& call, FlowObjectID receiver, uint16_t primitiveReceiver, const FlowPropertySource& source)
{
    switch (source.kind) {
    case FlowPropertySourceKind::BuiltinMember:
        callMethodSource(solver, call, receiver, primitiveReceiver, source);
        return;
    case FlowPropertySourceKind::Unknown:
    case FlowPropertySourceKind::Value:
    case FlowPropertySourceKind::Getter:
    case FlowPropertySourceKind::BuiltinGetter:
        callMethodSource(solver, call, receiver, primitiveReceiver, source);
        break;
    case FlowPropertySourceKind::Absent:
    case FlowPropertySourceKind::Setter:
    case FlowPropertySourceKind::BuiltinSetter:
        break;
    }

    FlowKeySet keys = keysOf(solver.program(), BuiltinNames::iteratorSymbol);
    if (receiver)
        callMethods(solver, call, FlowObjectSet(receiver), keys);
    else
        callMethodsOfPrimitives(solver, call, FlowPrimitiveReceiver { primitiveReceiver }, keys);
}

void openIteratorsOfPrimitives(FlowSolver& solver, const FlowIterationRequest& request, uint16_t primitives, FlowNodeID state)
{
    const Program& program = solver.program();
    FlowCallRequest call = protocolCall(request, { }, state, BuiltinCallbackTimes::Once);
    FlowPrimitiveReceiver receiver { primitives };
    switch (request.kind) {
    case IteratorKind::Sync:
        callMethodsOfPrimitives(solver, call, receiver, keysOf(program, BuiltinNames::iteratorSymbol));
        return;
    case IteratorKind::Async:
        forEachPropertySource(solver, receiver, keysOf(program, BuiltinNames::asyncIteratorSymbol), FlowPropertyWalk { FlowWalkOperation::Get, false, request.site }, [&](const FlowPropertySource& source) {
            openAsyncIteratorFrom(solver, call, FlowObjectID(), primitives, source);
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void openAsyncIterator(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID iterable, FlowObjectID object, FlowNodeID state)
{
    FlowCallRequest call = protocolCall(request, iterable, state, BuiltinCallbackTimes::Once);
    forEachPropertySource(solver, object, keysOf(solver.program(), BuiltinNames::asyncIteratorSymbol), FlowPropertyWalk { FlowWalkOperation::Get, false, request.site }, [&](const FlowPropertySource& source) {
        openAsyncIteratorFrom(solver, call, object, 0, source);
    });
}

void closeIterators(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID state, const FlowNewObjects& iterators)
{
    const Program& program = solver.program();
    FlowKeySet keys = keySetOf(program, methodKeyOf(program, ResumeMode::Return));
    switch (request.kind) {
    case IteratorKind::Sync:
        callMethods(solver, protocolCall(request, state, { }, BuiltinCallbackTimes::Once), iterators, keys);
        return;
    case IteratorKind::Async: {
        FlowStatementSite site = request.site;
        FlowNodeID results = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::IteratorResults, state.index()));
        FlowNodeID awaitedResults = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::AwaitedIteratorResults, state.index()));
        FlowNodeID values = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::AwaitedElements, state.index()));

        // The method of an async generator is native code that keeps the generator. Flow resumes a body that it knows, as it does for a step.
        FlowCallRequest call = protocolCall(request, state, results, BuiltinCallbackTimes::Once);
        solver.forEachMember(iterators, [&](FlowObjectID iterator) {
            if (directIterationOf(solver, iterator) == FlowDirectIteration::Generator)
                resumeIterator(solver, request, state, iterator, { ResumeMode::Return, { }, { } });
            else
                callMethods(solver, call, FlowObjectSet(iterator), keys);
        });
        awaitValuesOf(solver, { site, request.origin, awaitedResults, request.thrown }, results);
        readIteratorResults(solver, request, awaitedResults, values);
        awaitValuesOf(solver, { site, request.origin, { }, request.thrown }, values);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void connectIteratedValues(FlowSolver& solver, const FlowIterationRequest& request, FlowNodeID iterable, FlowNodeID values)
{
    if (!iterable)
        return;

    FlowStatementSite site = request.site;
    FlowNodeID state = solver.temporaryFor(callTemporaryKey(site, FlowCallTemporaryRole::IteratorStates, iterable.index()));

    // Every run of the statement has to come here: the cursors hand out what has arrived since the last one.
    openIteratorsOfNode(solver, request, iterable, solver.takeNewObjects(callTemporaryKey(site, FlowCallTemporaryRole::Iterables, iterable.index()), iterable), state);

    FlowNewObjects iterators = solver.takeNewObjects(callTemporaryKey(site, FlowCallTemporaryRole::IteratorStateObjects, state.index()), state);
    resumeIterators(solver, request, state, iterators, { ResumeMode::Next, { }, values });
    closeIterators(solver, request, state, iterators);
}

void noteIteratorOf(FlowSolver& solver, FlowObjectID iterator, IterationKind kind)
{
    solver.callState().iteration.kindOfIterator.add(iterator, kind);
}

std::optional<IterationKind> iterationKindOf(const FlowCallState& state, FlowObjectID iterator)
{
    return state.iteration.kindOfIterator.getOptional(iterator);
}

PropertyKeyID methodKeyOf(const Program& program, ResumeMode mode)
{
    switch (mode) {
    case ResumeMode::Next:
        return program.keyOf(BuiltinNames::next);
    case ResumeMode::Throw:
        return program.keyOf(BuiltinNames::throwKeyword);
    case ResumeMode::Return:
        return program.keyOf(BuiltinNames::returnKeyword);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
