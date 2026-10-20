/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowUnseenCode.h"

#if ENABLE(AOT)

#include "AOTFlowCallState.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTFlowPrototypes.h"
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FlowNativeCodeEntry {
    const FlowCallRequest& request;
    const Function& function;
    FlowInstanceID instance;
    FlowObjectID closure;
    const Witness& witness;
};

} // namespace

static FlowNodeID entryParameterNode(const FlowSolver& solver, FlowInstanceID instance, unsigned index)
{
    return solver.node(instance, solver.graph().summaryOf(instance).entryParameter(index));
}

static FlowLocalID findRestArrayLocal(const FlowSummary& summary)
{
    if (!summary.restSite)
        return { };
    for (const FlowSeed& seed : summary.seeds) {
        if (seed.kind() == FlowSeedKind::Site && seed.site() == *summary.restSite)
            return seed.node();
    }
    return { };
}

// No load: its waits would run every call of native code again and never this one. What is stored into the property later follows the edge.
static void connectPrototypeProperty(FlowSolver& solver, FlowObjectID closure, FlowObjectID created)
{
    if (created == unknownFlowObject)
        return;

    const Program& program = solver.program();
    connectCreationPrototype(solver, created, ensureOwnValue(solver, closure, program.keyOf(BuiltinNames::prototype)));
    noteMemberRead(solver, { }, closure, program.keyOf(BuiltinNames::prototype));

    // Until something is stored there, the object inherits from what the kind of the function gives.
    const Function& function = program.at(solver.objects().at(closure).function);
    if (!hasPrototypeProperty(program, function))
        solver.joinObject(solver.ensureOpenPart(created, FlowOpenPart::PrototypeLink), ensureBuiltinObject(solver, prototypeOfPrototypeProperty(function)));
}

static void passCallee(FlowSolver& solver, const FlowNativeCodeEntry& entry)
{
    std::optional<unsigned> callee = entry.function.calleeParameterIndex();
    if (!callee)
        return;

    FlowNodeID parameter = entryParameterNode(solver, entry.instance, *callee);
    if (entry.closure)
        solver.joinObject(parameter, entry.closure, entry.witness);
    else
        solver.joinUnknownObject(parameter, entry.witness);
}

static void passConstructionOperands(FlowSolver& solver, const FlowNativeCodeEntry& entry)
{
    const Function& function = entry.function;
    if (function.constructorForm() == ConstructorForm::NotConstructor)
        return;

    if (std::optional<unsigned> newTarget = function.newTargetParameterIndex())
        solver.joinObject(entryParameterNode(solver, entry.instance, *newTarget), entry.closure, entry.witness);

    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    if (!thisParameter || !function.constructedClass)
        return;

    const FlowCallRequest& request = entry.request;
    FlowObjectID constructed = allocateAtCallSite(solver, { request.site, { .kind = FlowSiteKind::Layout, .origin = FlowSiteOrigin::Operation, .value = request.origin, .layout = function.constructedClass } });
    connectPrototypeProperty(solver, entry.closure, constructed);
    solver.noteEvent(constructed, FlowObjectEvent::AllocatedRepeatedly, entry.witness);
    solver.joinObject(entryParameterNode(solver, entry.instance, *thisParameter), constructed, entry.witness);
    solver.joinObject(request.operands.result, constructed, entry.witness);
}

static void connectResultsToNativeCode(FlowSolver& solver, const FlowNativeCodeEntry& entry)
{
    const FlowSummary& summary = solver.graph().summaryOf(entry.instance);
    FlowNodeID nativeCode = entry.request.operands.result;

    if (summary.coroutine.resultSite) {
        FlowObjectID result = linkCoroutineResult(solver, entry.request, entry.instance);
        if (entry.closure && summary.coroutine.yielded)
            connectPrototypeProperty(solver, entry.closure, result);
        solver.noteEvent(result, FlowObjectEvent::AllocatedRepeatedly, entry.witness);
        solver.joinObject(nativeCode, result, entry.witness);
    } else {
        for (uint32_t index = 0; index < summary.numberOfResults; ++index)
            solver.connect(solver.node(entry.instance, summary.result(index)), nativeCode, { }, entry.witness);
    }
    solver.connect(solver.node(entry.instance, summary.thrown()), nativeCode, { }, entry.witness);
}

void enterFromNativeCode(FlowSolver& solver, FunctionID function, FlowObjectID closure, const Witness& witness)
{
    FlowInstanceID instance = solver.enter(function, FlowContext::Generic);
    if (!instance)
        return;

    FlowCallRequest request {
        nativeCodeSite(solver),
        ValueID(),
        FlowCallLinkKind::Host,
        BuiltinCallbackTimes::Repeatedly,
        true,
        { .result = solver.graph().nativeCode() },
    };
    FlowCallLinkID link = ensureCallLink(solver, request, instance);
    FlowNativeCodeEntry entry { request, solver.program().at(function), instance, closure, witness };

    passDeclaredTypes(solver, instance, witness);
    passCallee(solver, entry);
    if (closure) {
        FlowPassedArguments noArguments;
        passCaptures(solver, { request, noArguments, function, closure, FlowContext::Generic, FlowInstancePolicy::BySummary }, { link, instance, false });
        passConstructionOperands(solver, entry);
        if (solver.callState().links.constructsUnknownCalleeForOtherNewTarget)
            solver.noteEvent(closure, FlowObjectEvent::OtherNewTarget, witness);
    }
    connectResultsToNativeCode(solver, entry);
}

// Every instance that native code has entered so far, with or without a closure: a function of the library that is called by value has none.
void passOtherNewTargetToEnteredInstances(FlowSolver& solver, const Witness& witness)
{
    FlowValueTable& values = solver.values();
    BitVector instances = std::exchange(solver.callState().links.instancesEnteredWithoutOtherNewTarget, { });
    for (size_t index : instances) {
        FlowInstanceID instance(static_cast<uint32_t>(index));
        const Function& function = solver.program().at(solver.graph().at(instance).function);
        unsigned newTarget = *function.newTargetParameterIndex();
        const DeclaredType* declared = function.declaredEntryParameter(newTarget);
        solver.join(entryParameterNode(solver, instance, newTarget), declared && declared->type ? values.valueOfType(declared->type) : values.unknownValue(), witness);
    }
}

void passOtherNewTarget(FlowSolver& solver, FlowObjectID closure, const Witness& witness)
{
    FunctionID functionID = solver.objects().at(closure).function;
    const Function& function = solver.program().at(functionID);
    std::optional<unsigned> newTarget = function.newTargetParameterIndex();
    if (!newTarget || function.constructorForm() == ConstructorForm::NotConstructor)
        return;

    FlowInstanceID instance = solver.enter(functionID, FlowContext::Generic);
    if (!instance)
        return;
    const DeclaredType* declared = function.declaredEntryParameter(*newTarget);
    FlowValueTable& values = solver.values();
    solver.join(entryParameterNode(solver, instance, *newTarget), declared && declared->type ? values.valueOfType(declared->type) : values.unknownValue(), witness);
}

void constructEscapedFunctionsForOtherNewTarget(FlowSolver& solver, const Witness& witness)
{
    if (std::exchange(solver.callState().links.constructsUnknownCalleeForOtherNewTarget, true))
        return;

    passOtherNewTargetToEnteredInstances(solver, witness);
    FlowObjectSet escapedObjects = solver.valueOf(solver.graph().nativeCode()).objects;
    escapedObjects.forEach([&](FlowObjectID object) {
        if (solver.objects().kindOf(object) == FlowObjectKind::Closure)
            solver.noteEvent(object, FlowObjectEvent::OtherNewTarget, witness);
    });
}

static bool mayBeConstructedForOtherNewTarget(FlowSolver& solver)
{
    return solver.records().unseenCodeCanRun() || solver.callState().links.constructsUnknownCalleeForOtherNewTarget;
}

void passDeclaredTypes(FlowSolver& solver, FlowInstanceID instance, const Witness& witness)
{
    const FlowGraph& graph = solver.graph();
    const Function& function = solver.program().at(graph.at(instance).function);
    const FlowSummary& summary = graph.summaryOf(instance);
    FlowValueTable& values = solver.values();
    FlowLocalID restArray = findRestArrayLocal(summary);

    auto passTo = [&](unsigned index, FlowNodeID parameter) {
        switch (function.entryParameterKind(index)) {
        case EntryParameter::Capture:
            break;
        case EntryParameter::Callee:
            // enterFromNativeCode() passes the closure it enters.
            return;
        case EntryParameter::ArgumentCount:
            solver.join(parameter, values.valueOfIntegers(Range::length()), witness);
            return;
        case EntryParameter::NewTarget:
            // Native code calls a function, or constructs it with itself: passConstructionOperands() passes that one. Another new.target comes from unknown JavaScript,
            // or from a construction of the program's own whose callee nobody knows.
            if (!mayBeConstructedForOtherNewTarget(solver)) {
                solver.join(parameter, values.valueOfPrimitives(Primitive::Undefined), witness);
                solver.callState().links.instancesEnteredWithoutOtherNewTarget.set(instance.index());
                return;
            }
            [[fallthrough]];
        case EntryParameter::This:
        case EntryParameter::Parameter:
        case EntryParameter::HiddenRest: {
            const DeclaredType* declared = function.declaredEntryParameter(index);
            solver.join(parameter, declared && declared->type ? values.valueOfType(declared->type) : values.unknownValue(), witness);
            return;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    for (unsigned index = function.captures.size(); index < summary.numberOfEntryParameters; ++index) {
        FlowLocalID local = summary.entryParameter(index);
        if (local != restArray)
            passTo(index, solver.node(instance, local));
    }

    if (!summary.restSite)
        return;
    forEachElementPart(graph, graph.objectOf(instance, *summary.restSite), arrayIndexRange, [&](FlowNodeID part) {
        solver.joinUnknown(part, witness);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
