/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCreateBoundFunctions.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <compare>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct BindCall {
    FunctionID function;
    ValueID call;
    BuiltinOverloadID overload;

    friend auto operator<=>(const BindCall&, const BindCall&) = default;
};

struct BindResolution {
    const FlowFamilyInputs& inputs;
    std::span<const AllocationClass> allocationClasses;
    const FlowTypeConverter& converter;
    const FlowVersionPlan& versions;
    TypeArena& types;
    FlowObjectID bind;
};

struct ResolvedBindCall {
    FunctionID target;
    Vector<FlowObjectID, 2> results;
};

} // anonymous namespace

static Vector<BindCall> collectBindCalls(const FlowFamilyInputs& inputs)
{
    const BuiltinTable& builtins = inputs.solution.builtins();
    const FlowGraph& graph = inputs.graph();

    Vector<BindCall> calls;
    for (const FlowBuiltinCallLink& link : inputs.records().builtinCallLinks()) {
        if (link.kind != FlowCallLinkKind::Call || link.mode != BuiltinCallMode::Call || builtins.canonicalID(link.builtin) != Builtins::FunctionPrototypeBind)
            continue;
        if (link.callerInstance && link.site && link.overload && !graph.isHost(link.callerInstance))
            calls.append({ graph.at(link.callerInstance).function, link.site, link.overload });
    }
    sortAndRemoveDuplicates(inputs.loops(), calls);
    return calls;
}

static const FlowCall* findCallStatement(const FlowFamilyInputs& inputs, const BindCall& bindCall)
{
    const Vector<FlowCall>& calls = inputs.graph().summaryOf(bindCall.function).statements.calls;
    auto found = std::ranges::find(calls, bindCall.call, &FlowCall::origin);
    return found == calls.end() ? nullptr : &*found;
}

// The application replaces the call by the creation of a closure that takes the operands of the call as they stand.
static bool canBecomeClosureCreation(const FlowFamilyInputs& inputs, const BindCall& bindCall, const FlowCall& call)
{
    if (inputs.program().at(bindCall.function).at(bindCall.call).opcode != Opcode::Call)
        return false;
    if (call.mode != FlowCallMode::Call || call.argumentList || call.flags.contains(FlowCallFlag::IsTailCall))
        return false;
    size_t numberOfArguments = inputs.graph().summaryOf(bindCall.function).statements.list(call.arguments).size();
    return numberOfArguments <= 1;
}

static bool areClosuresAsCreated(const BindResolution& resolution, FunctionID target, const BuiltinOverload& overload)
{
    const Program& program = resolution.inputs.program();
    Vector<PropertyKeyID, 2> keys(overload.receiver->namedMembers.size(), [&](size_t index) {
        return program.keyOf(overload.receiver->namedMembers[index]);
    });

    bool hasClosures = false;
    for (const AllocationClass& allocationClass : resolution.allocationClasses) {
        if (allocationClass.closureFunction != target)
            continue;
        hasClosures = true;
        if (!keepsCreatedMembers(allocationClass, keys.span()))
            return false;
    }
    return hasClosures;
}

// The bound function hands the value on as the this value of the version it enters. A version without known callers declares the type as read.
// The type of any other version is what flows into the parameter in some instance of the target.
static bool fitsThisParameter(const BindResolution& resolution, FunctionID targetID, FlowNodeID boundThis)
{
    const FlowGraph& graph = resolution.inputs.graph();
    const Function& target = resolution.inputs.program().at(targetID);
    std::optional<unsigned> parameter = target.thisParameterIndex();
    if (!parameter)
        return true;

    if (resolution.versions.genericVersionOf(targetID)) {
        if (!boundThis)
            return resolution.inputs.program().at(target.thisType.type).primitives & Primitive::Undefined;
        return resolution.converter.isInside(singleElementSpan(boundThis), resolution.inputs.program().at(target.thisType.type), resolution.types) == TypeInclusion::Inside;
    }

    uint16_t boundPrimitives = boundThis ? graph.valueOf(boundThis).primitives : static_cast<uint16_t>(Primitive::Undefined);
    if (boundPrimitives & ~Primitive::UndefinedOrNull)
        return false;

    uint16_t primitives = 0;
    FlowObjectSet objects;
    graph.forEachInstanceOf(targetID, [&](FlowInstanceID instance) {
        const FlowValue& value = graph.valueOf(graph.nodeOf(instance, graph.summaryOf(instance).entryParameter(*parameter)));
        primitives |= value.primitives;
        objects.unite(value.objects);
    });
    return !(boundPrimitives & ~primitives) && (!boundThis || graph.valueOf(boundThis).objects.isSubsetOf(objects));
}

static FunctionID findOnlyTarget(const FlowFamilyInputs& inputs, const FlowValue& targets)
{
    if (!targets.hasOnlyObjects())
        return { };

    FunctionID target;
    bool hasOneTarget = targets.objects.allOf([&](FlowObjectID object) {
        const FlowObject& record = inputs.objects().at(object);
        if (record.kind != FlowObjectKind::Closure || (target && target != record.function))
            return false;
        target = record.function;
        return true;
    });
    return hasOneTarget ? target : FunctionID();
}

static bool isResultOf(const FlowFamilyInputs& inputs, const BindCall& bindCall, FlowObjectID object)
{
    const FlowObjectTable& objects = inputs.objects();
    if (objects.kindOf(object) != FlowObjectKind::Instance || !objects.isInstanceOf(object, BuiltinClasses::BoundFunction))
        return false;
    const FlowSite& site = objects.at(objects.at(object).site);
    return site.function == bindCall.function && site.description.value == bindCall.call && site.description.origin == FlowSiteOrigin::BuiltinResult;
}

static bool resolveInInstance(const BindResolution& resolution, const BindCall& bindCall, const FlowCall& call, FlowInstanceID instance, ResolvedBindCall& resolved)
{
    const FlowFamilyInputs& inputs = resolution.inputs;
    const FlowGraph& graph = inputs.graph();
    auto nodeOf = [&](FlowLocalID local) {
        return local ? graph.nodeOf(instance, local) : FlowNodeID();
    };

    FlowNodeID callees = nodeOf(call.callee);
    FlowNodeID targets = nodeOf(call.thisValue);
    if (!callees || !targets)
        return false;
    bool isReached = !graph.valueOf(callees).isEmpty() && !graph.valueOf(targets).isEmpty();
    if (!isReached)
        return true;
    if (!graph.valueOf(callees).hasOnlyObjects() || graph.valueOf(callees).objects.onlyObject() != resolution.bind || inputs.calls.hasUnknownCalleeAt(instance, bindCall.call))
        return false;

    FunctionID target = findOnlyTarget(inputs, graph.valueOf(targets));
    if (!target || (resolved.target && resolved.target != target))
        return false;
    resolved.target = target;

    std::span<const FlowLocalID> arguments = graph.summaryOf(instance).statements.list(call.arguments);
    if (!fitsThisParameter(resolution, target, arguments.empty() ? FlowNodeID() : nodeOf(arguments[0])))
        return false;

    return graph.valueOf(nodeOf(call.result)).objects.allOf([&](FlowObjectID object) {
        resolved.results.append(object);
        return isResultOf(inputs, bindCall, object);
    });
}

static std::optional<ResolvedBindCall> resolveBindCall(const BindResolution& resolution, const BindCall& bindCall)
{
    const FlowFamilyInputs& inputs = resolution.inputs;
    const Program& program = inputs.program();
    if (inputs.solution.isPastWorkLimit(bindCall.function) || !inputs.isEnabled(FlowResultKind::ExactFunctions, bindCall.function.index()))
        return std::nullopt;

    const FlowCall* call = findCallStatement(inputs, bindCall);
    if (!call || !canBecomeClosureCreation(inputs, bindCall, *call))
        return std::nullopt;

    ResolvedBindCall resolved;
    bool isResolvedEverywhere = true;
    inputs.graph().forEachInstanceOf(bindCall.function, [&](FlowInstanceID instance) {
        isResolvedEverywhere = isResolvedEverywhere && resolveInInstance(resolution, bindCall, *call, instance, resolved);
    });
    if (!isResolvedEverywhere || !resolved.target)
        return std::nullopt;

    // The body of a function that flow never enters is removed, and the bound function calls it.
    if (!inputs.graph().isReached(resolved.target) || !canCreateBoundFunction(program, resolved.target) || !program.at(resolved.target).captures.isEmpty())
        return std::nullopt;
    if (!areClosuresAsCreated(resolution, resolved.target, inputs.solution.builtins().at(bindCall.overload)))
        return std::nullopt;
    if (inputs.build().exceeds(LimitID::MaxCaptures, numberOfBoundThisAndTargetCaptures, Subject { .function = bindCall.function, .value = bindCall.call }))
        return std::nullopt;
    return resolved;
}

FlowBoundFunctionResults computeBoundFunctionResults(const FlowFamilyInputs& inputs, std::span<const AllocationClass> allocationClasses, const FlowTypeConverter& converter, const FlowVersionPlan& versions, TypeTable& table)
{
    FlowBoundFunctionResults results;
    results.plan.firstFunction = versions.functionCount();

    FlowObjectID bind = inputs.objects().findBuiltin(Builtins::FunctionPrototypeBind);
    if (!bind)
        return results;

    Vector<BindCall> calls = collectBindCalls(inputs);
    Vector<std::optional<ResolvedBindCall>> resolvedCalls;
    {
        TypeInterningPhase phase(table, 0, TypeInterningPhase::Scratch);
        TypeArena types(phase, TypeInterningPhase::Scratch);
        BindResolution resolution { inputs, allocationClasses, converter, versions, types, bind };
        resolvedCalls = calls.map([&](const BindCall& call) {
            return resolveBindCall(resolution, call);
        });
        phase.discard();
    }

    Vector<FunctionID> targets;
    for (const std::optional<ResolvedBindCall>& resolved : resolvedCalls) {
        if (resolved)
            targets.append(resolved->target);
    }
    if (targets.isEmpty())
        return results;
    sortAndRemoveDuplicates(inputs.loops(), targets);
    results.plan.targets = WTF::move(targets);

    results.functionByObject.grow(inputs.objects().size());
    for (size_t index = 0; index < calls.size(); ++index) {
        if (!resolvedCalls[index])
            continue;
        FunctionID function = results.plan.functionBoundTo(resolvedCalls[index]->target);
        results.plan.sites.append({ calls[index].function, calls[index].call, function });
        for (FlowObjectID object : resolvedCalls[index]->results)
            results.functionByObject[object.index()] = function;
    }
    return results;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
