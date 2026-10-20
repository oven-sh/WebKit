/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include <algorithm>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// Flow enters the body of a module where something reached loads the module, and a function of the library where something reached uses its built-in.
// Where the image has no compiled function of the library, the host creates its ordinary one.
static bool isEnteredOnDemand(const Function& function)
{
    if (function.role == FunctionRole::Library || function.role == FunctionRole::Model)
        return true;
    return function.kind == FunctionKind::Module || function.kind == FunctionKind::CommonJsModule;
}

static FunctionReachability reachabilityOf(const FlowFamilyInputs& inputs, const BitVector& hostCalled, FunctionID function)
{
    if (!inputs.isEnabled(FlowResultKind::ReachedFunctions, function.index()))
        return FunctionReachability::Unknown;
    if (inputs.graph().isReached(function))
        return FunctionReachability::Unknown;
    if (hostCalled.get(function.index()) && !isEnteredOnDemand(inputs.program().at(function)))
        return FunctionReachability::Unknown;
    return FunctionReachability::Unreached;
}

static bool isEnteredByHost(const FlowFamilyInputs& inputs, const BitVector& hostCalled, FunctionID function)
{
    if (hostCalled.get(function.index()))
        return true;
    return std::ranges::any_of(inputs.calls.linksTo(function), [&](FlowCallLinkID link) {
        const FlowCallLink& record = inputs.records().at(link);
        return record.kind == FlowCallLinkKind::Host || inputs.graph().isHost(record.caller);
    });
}

// A function whose callers are all known is listed in the stamp of every call that reaches it, and a caller with the result switched off gets no stamps.
static bool hasCallerWithoutStamps(const FlowFamilyInputs& inputs, FunctionID function)
{
    return std::ranges::any_of(inputs.calls.linksTo(function), [&](FlowCallLinkID link) {
        FunctionID caller = inputs.records().at(link).caller;
        return !inputs.graph().isHost(caller) && !inputs.isEnabled(FlowResultKind::CalleeSets, caller.index());
    });
}

FlowSiteLoss findCallerLoss(const FlowFamilyInputs& inputs, const BitVector& hostCalled, FunctionID function)
{
    FlowSiteLoss loss;
    if (!inputs.isEnabled(FlowResultKind::CalleeSets, function.index()) || hasCallerWithoutStamps(inputs, function))
        keepFirstLoss(loss, FlowSiteLoss { .cause = UnprovenCause::ResultDisabled });
    if (isEnteredByHost(inputs, hostCalled, function))
        keepFirstLoss(loss, FlowSiteLoss { .cause = UnprovenCause::ReachesNativeCode });
    for (FlowObjectID closure : inputs.objects().closuresOf(function)) {
        for (ObjectMutation mutation : inputs.factsOf(closure).mutations & reachMutations)
            keepFirstLoss(loss, FlowSiteLoss { .cause = causeOf(mutation), .witness = inputs.records().witnessOf(closure, mutation) });
    }
    return loss;
}

static bool isOnlyCalled(const FlowFamilyInputs& inputs, FlowObjectID closure)
{
    constexpr OptionSet<FlowObjectUse> callUses { FlowObjectUse::Called, FlowObjectUse::CalledRepeatedly, FlowObjectUse::Constructed };
    constexpr OptionSet<FlowObjectEvent> allocationEvents { FlowObjectEvent::Allocated, FlowObjectEvent::AllocatedByConstructor, FlowObjectEvent::AllocatedRepeatedly };

    const FlowObjectFacts& facts = inputs.factsOf(closure);
    if (!facts.uses.containsOnly(callUses) || !facts.mutations.isEmpty() || !facts.events.containsOnly(allocationEvents))
        return false;
    FlowObjectID prototype = inputs.objects().findPrototype(closure);
    return !prototype || inputs.factsOf(prototype).uses.isEmpty();
}

static ClosureUse closureUseOf(const FlowFamilyInputs& inputs, FunctionID function)
{
    if (!inputs.isEnabled(FlowResultKind::ObjectUses, function.index()))
        return ClosureUse::Unknown;

    std::span<const FlowObjectID> closures = inputs.objects().closuresOf(function);
    bool isCalledOnly = !closures.empty() && std::ranges::all_of(closures, [&](FlowObjectID closure) {
        return isOnlyCalled(inputs, closure);
    });
    return isCalledOnly ? ClosureUse::CalledOnly : ClosureUse::Unknown;
}

static FunctionFlow computeFunctionFlow(const FlowFamilyInputs& inputs, const BitVector& hostCalled, FunctionID function)
{
    FunctionFlow flow;
    flow.reachability = reachabilityOf(inputs, hostCalled, function);
    flow.executions = inputs.executionsByFunction[function.index()];
    flow.closureUse = closureUseOf(inputs, function);

    FlowSiteLoss loss = findCallerLoss(inputs, hostCalled, function);
    if (loss.cause == UnprovenCause::Unknown)
        flow.callers = CallerState::AllKnown;
    if (inputs.solution.isPastWorkLimit(function)) {
        Witness declaration { function.index(), Wire::none, inputs.program().at(function).declarationLocation() };
        keepFirstLoss(loss, FlowSiteLoss { UnprovenCause::LimitReached, LimitID::MaxFlowWorkPerFunction, declaration });
    }
    flow.cause = loss.cause;
    flow.witness = loss.witness;
    return flow;
}

static bool isDirectCall(const Program& program, const FlowCallLink& link)
{
    switch (link.kind) {
    case FlowCallLinkKind::Call:
    case FlowCallLinkKind::Construct:
        return accessClassOf(program.at(link.caller).at(link.site).opcode) == AccessClass::DirectCall;
    case FlowCallLinkKind::Accessor:
    case FlowCallLinkKind::BuiltinCallback:
    case FlowCallLinkKind::Protocol:
    case FlowCallLinkKind::Host:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static OptionSet<FunctionCapabilities> provenCapabilitiesOf(const FlowFamilyInputs& inputs, CallerState callers, FunctionID function)
{
    if (callers != CallerState::AllKnown)
        return { };

    OptionSet<FunctionCapabilities> capabilities { FunctionCapabilities::IsNeverCalledByHost };
    bool hasOnlyDirectCallers = std::ranges::all_of(inputs.calls.linksTo(function), [&](FlowCallLinkID link) {
        return isDirectCall(inputs.program(), inputs.records().at(link));
    });
    if (hasOnlyDirectCallers)
        capabilities.add(FunctionCapabilities::HasOnlyDirectCallers);
    return capabilities;
}

static ProgramInstances computeProgramInstances(const FlowFamilyInputs& inputs)
{
    constexpr OptionSet<BuiltinUse> possibleCallUses { BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Escaped };

    if (!inputs.build().isEnabledForProgram(FlowResultKind::ReachedFunctions) || inputs.records().mayRunUnknownJavaScript())
        return ProgramInstances::Unknown;

    const BuiltinTable& table = inputs.solution.builtins();
    for (unsigned index = 0; table.contains(BuiltinID(index)); ++index) {
        BuiltinID builtin(index);
        if (!table.at(builtin).flags.contains(BuiltinFlag::StartsInstance))
            continue;
        // Uses are recorded under the canonical id.
        if (inputs.records().builtinUse(table.canonicalID(builtin)).uses.containsAny(possibleCallUses))
            return ProgramInstances::Unknown;
    }
    return ProgramInstances::AtMostOne;
}

FlowReachabilityResults computeReachabilityResults(const FlowFamilyInputs& inputs)
{
    unsigned numberOfFunctions = static_cast<unsigned>(inputs.program().functions.size());
    RELEASE_ASSERT(inputs.executionsByFunction.size() == numberOfFunctions);
    BitVector hostCalled = functionsCalledByHost(inputs.program());

    FlowReachabilityResults results;
    results.functions = Vector<FunctionFlow>(numberOfFunctions);
    results.provenCapabilities = Vector<OptionSet<FunctionCapabilities>>(numberOfFunctions);
    forEachIndexInParallel(inputs.loops(), numberOfFunctions, [&](unsigned index) {
        FunctionID function(index);
        results.functions[index] = computeFunctionFlow(inputs, hostCalled, function);
        results.provenCapabilities[index] = provenCapabilitiesOf(inputs, results.functions[index].callers, function);
    });
    results.instances = computeProgramInstances(inputs);
    return results;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
