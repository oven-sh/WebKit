/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRemoveUnusedParameters.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTParameterUses.h"
#include "AOTPassRunner.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTThreads.h"
#include "AOTValueSubstitution.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ParameterRemovalPlan {
    FunctionID reducedFunctionOf(FunctionID) const;

    Vector<BitVector> removed;
    Vector<FunctionID> candidates;
    Vector<FunctionID> reducedFunctions;
    uint64_t numberOfRemovedParameters { 0 };
};

struct EntryParameterPosition {
    unsigned index;
    EntryParameter kind;
};

FunctionID ParameterRemovalPlan::reducedFunctionOf(FunctionID function) const
{
    if (function.index() >= removed.size() || removed[function.index()].isEmpty())
        return { };
    size_t position = std::ranges::lower_bound(candidates, function) - candidates.begin();
    return reducedFunctions[position];
}

} // anonymous namespace

static bool countsEntryParameters(CreatorRecord::Kind kind)
{
    switch (kind) {
    case CreatorRecord::Kind::Entrypoint:
    case CreatorRecord::Kind::Coroutine:
    case CreatorRecord::Kind::BoundFunction:
    case CreatorRecord::Kind::ResultWrapper:
    case CreatorRecord::Kind::ResultWorker:
    case CreatorRecord::Kind::ParameterWrapper:
    case CreatorRecord::Kind::StubBody:
        return true;
    case CreatorRecord::Kind::None:
    case CreatorRecord::Kind::Version:
    case CreatorRecord::Kind::Continuation:
    case CreatorRecord::Kind::Serializer:
    case CreatorRecord::Kind::Parser:
    case CreatorRecord::Kind::Validator:
    case CreatorRecord::Kind::ChainStep:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isRemovable(const Function& function, EntryParameter kind)
{
    switch (kind) {
    case EntryParameter::Capture:
    case EntryParameter::This:
    case EntryParameter::ArgumentCount:
    case EntryParameter::Parameter:
    case EntryParameter::HiddenRest:
        return true;
    case EntryParameter::Callee:
        return !function.has(FunctionFlags::IsGenerator);
    case EntryParameter::NewTarget:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename List>
static void removeAtPositions(List& list, const BitVector& removed)
{
    size_t newSize = 0;
    for (size_t index = 0; index < list.size(); ++index) {
        if (!removed.get(index))
            list[newSize++] = list[index];
    }
    list.shrink(newSize);
}

ParameterEditing canEditParameters(const Program& program, const CallGraph& callGraph, FunctionID id)
{
    auto& function = program.at(id);
    if (!function.hasBody() || !function.mayChangeSignature() || !function.mayBeCloned() || !function.hasOrdinaryFrame())
        return ParameterEditing::Unknown;
    if (!callGraph.numberOfDirectCallSites(id))
        return ParameterEditing::Unknown;
    if (function.constructedClass || function.isUnsplitCoroutine() || function.isModuleBody())
        return ParameterEditing::Unknown;
    if (countsEntryParameters(function.creator.kind()))
        return ParameterEditing::Unknown;
    return ParameterEditing::Allowed;
}

static Vector<BitVector> findUnusedParameters(const Program& program, const CallGraph& callGraph, BuildContext& build)
{
    unsigned count = program.functions.size();
    Vector<BitVector> unused(count);
    Vector<Vector<ForwardedParameter>> forwarded(count);
    forEachIndexInParallel(build.parallelLoops(), count, [&](unsigned index) {
        FunctionID id(index);
        if (canEditParameters(program, callGraph, id) != ParameterEditing::Allowed || !build.isEnabled(PassID::RemoveUnusedParameters, id))
            return;
        auto& function = program.functions[index];
        unsigned numberOfEntryParameters = function.numberOfEntryParameters();
        BitVector removableEntryParameters(numberOfEntryParameters);
        for (unsigned i = 0; i < numberOfEntryParameters; ++i) {
            if (isRemovable(function, function.entryParameterKind(i)))
                removableEntryParameters.quickSet(i);
        }
        auto uses = classifyParameterUses(function, removableEntryParameters, &forwarded[index]);
        for (unsigned i = 0; i < uses.size(); ++i) {
            if (uses[i] != ParameterUse::Used)
                unused[index].set(i);
        }
    });
    Vector<unsigned> forwardingFunctions;
    for (unsigned index = 0; index < count; ++index) {
        if (!forwarded[index].isEmpty())
            forwardingFunctions.append(index);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned index : forwardingFunctions) {
            for (auto& forwardedParameter : forwarded[index]) {
                if (!unused[index].get(forwardedParameter.parameter) || unused[forwardedParameter.callee.index()].get(forwardedParameter.calleeParameter))
                    continue;
                unused[index].clear(forwardedParameter.parameter);
                changed = true;
            }
        }
    }
    return unused;
}

void removeEntryParameters(Function& function, const BitVector& removed)
{
    unsigned firstParameter = function.firstParameterIndex();
    Vector<EntryParameterPosition, 8> positions;
    for (size_t index : removed)
        positions.append({ static_cast<unsigned>(index), function.entryParameterKind(index) });

    for (size_t i = positions.size(); i--;) {
        auto [index, kind] = positions[i];
        switch (kind) {
        case EntryParameter::Capture:
            function.captures.removeAt(index);
            break;
        case EntryParameter::This:
            function.flags.remove(FunctionFlags::HasThis);
            function.thisType = DeclaredType { .type = TypeTable::anything };
            break;
        case EntryParameter::Parameter: {
            unsigned parameter = index - firstParameter;
            if (function.has(FunctionFlags::HasRestParameter) && parameter + 1 == function.parameters.size())
                function.flags.remove(FunctionFlags::HasRestParameter);
            function.parameters.removeAt(parameter);
            break;
        }
        case EntryParameter::Callee:
            function.flags.remove(FunctionFlags::TakesCallee);
            break;
        case EntryParameter::ArgumentCount:
            function.flags.remove(FunctionFlags::HasArgumentCount);
            break;
        case EntryParameter::HiddenRest:
            function.flags.remove(FunctionFlags::HasHiddenRest);
            break;
        case EntryParameter::NewTarget:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    if (function.hasBody()) {
        auto& entryParameters = function.at(entryBlock).parameters;
        for (size_t index : removed)
            becomeUndefined(function, entryParameters[index]);
        removeAtPositions(entryParameters, removed);
    }
    removeAtPositions(function.derivation.parameterMap, removed);
}

void removeCallArguments(Function& caller, ValueID call, const BitVector& removed)
{
    auto& value = caller.at(call);
    RELEASE_ASSERT(value.opcode == Opcode::CallDirect);
    for (uint32_t position = value.numberOfVariadicOperands(); position--;) {
        if (removed.get(position))
            value.removeVariadicOperand(position);
    }
}

static Function createReducedFunction(const Program& program, FunctionID original, const BitVector& removed)
{
    Function reduced = program.at(original);
    if (!reduced.derivation.source) {
        reduced.derivation.parameterMap = Vector<ParameterOrigin>(reduced.numberOfEntryParameters(), [](size_t index) {
            return ParameterOrigin { static_cast<uint32_t>(index) };
        });
    }
    removeEntryParameters(reduced, removed);
    reduced.capabilities.add({ FunctionCapabilities::HasOnlyDirectCallers, FunctionCapabilities::IsNeverCalledByHost });
    return reduced;
}

static void emitReturnOfForwardedCall(IRBuilder& builder, ValueID call, TailCallRequirement requirement)
{
    const Function& forwarder = builder.function();
    if (forwarder.at(call).type == TypeTable::never) {
        builder.emitUnreachable();
        return;
    }
    if (forwarder.results.size() != 1) {
        builder.emitReturn(call);
        return;
    }

    Terminator terminator = IRBuilder::makeTerminator(TerminatorKind::ReturnCall, { call });
    terminator.setRequirement(requirement);
    builder.emitTerminator(WTF::move(terminator));
}

static void becomeForwarder(const AnalysisContext& context, Function& original, FunctionID reduced, const BitVector& removed, TypeArena& types)
{
    Function forwarder;
    static_cast<FunctionRecord&>(forwarder) = original;
    forwarder.inlineFrames.clear();
    forwarder.capabilities.add(FunctionCapabilities::IsHiddenInStackTraces);
    forwarder.capabilities.remove({ FunctionCapabilities::MayChangeSignature, FunctionCapabilities::MayBeInlined });

    {
        IRBuilder builder(context, forwarder, types);
        builder.setOrigin({ original.position, 0 });
        BlockID entry = builder.addBlock();
        auto& entryParameters = original.at(entryBlock).parameters;
        Vector<ValueID, 8> arguments;
        for (size_t index = 0; index < entryParameters.size(); ++index) {
            auto& parameter = original.at(entryParameters[index]);
            ValueID copy = builder.addParameter(entry, parameter.type);
            forwarder.at(copy).setWritten(parameter.written());
            forwarder.at(copy).setPrediction(parameter.prediction());
            if (!removed.get(index))
                arguments.append(copy);
        }
        builder.moveToEndOf(entry);

        Value call = IRBuilder::make(Opcode::CallDirect);
        call.setFunction(reduced);
        call.setArgumentsOperands(arguments.span());
        call.setResultCheck(resultCheckOf(*context.flow, reduced));
        emitReturnOfForwardedCall(builder, builder.append(WTF::move(call)), tailCallRequirementOf(original));
    }
    original = WTF::move(forwarder);
}

static ParameterRemovalPlan planParameterRemoval(const Program& program, const CallGraph& callGraph, BuildContext& build)
{
    ParameterRemovalPlan plan;
    plan.removed = findUnusedParameters(program, callGraph, build);
    for (size_t index = 0; index < plan.removed.size(); ++index) {
        size_t numberOfRemovedParameters = plan.removed[index].bitCount();
        if (!numberOfRemovedParameters)
            continue;
        plan.candidates.append(FunctionID(index));
        plan.numberOfRemovedParameters += numberOfRemovedParameters;
    }
    return plan;
}

static void addReducedFunctions(ProgramEditor& editor, BuildContext& build, ParameterRemovalPlan& plan)
{
    const Program& program = editor.program();
    Vector<Function> reducedFunctions(plan.candidates.size());
    forEachIndexInParallel(build.parallelLoops(), plan.candidates.size(), [&](unsigned index) {
        FunctionID original = plan.candidates[index];
        reducedFunctions[index] = createReducedFunction(program, original, plan.removed[original.index()]);
    });

    plan.reducedFunctions.reserveInitialCapacity(plan.candidates.size());
    for (size_t index = 0; index < plan.candidates.size(); ++index)
        plan.reducedFunctions.append(editor.addFunctionLike(plan.candidates[index], WTF::move(reducedFunctions[index])));
}

static Vector<FunctionID> findCallers(const CallGraph& callGraph, const BuildContext& build, const ParameterRemovalPlan& plan)
{
    BitVector isCaller;
    for (FunctionID candidate : plan.candidates) {
        for (FunctionID caller : callGraph.callersOf(candidate))
            isCaller.set(caller.index());
    }
    for (FunctionID candidate : plan.candidates)
        isCaller.clear(candidate.index());

    Vector<FunctionID> callers;
    for (size_t index : isCaller) {
        if (build.isEnabled(PassID::RemoveUnusedParameters, FunctionID(index)))
            callers.append(FunctionID(index));
    }
    callers.appendVector(plan.reducedFunctions);
    return callers;
}

static bool retargetCalls(Function& caller, const ParameterRemovalPlan& plan)
{
    bool didRetarget = false;
    for (auto& block : caller.blocks) {
        for (ValueID id : block.values) {
            auto& call = caller.at(id);
            if (call.opcode != Opcode::CallDirect)
                continue;
            FunctionID callee = call.function();
            FunctionID reduced = plan.reducedFunctionOf(callee);
            if (!reduced)
                continue;
            removeCallArguments(caller, id, plan.removed[callee.index()]);
            call.setFunction(reduced);
            didRetarget = true;
        }
    }
    return didRetarget;
}

// An inline frame holds a copy of the body, which the reduced function has. Stack traces hide the forwarder.
static void moveInlineFramesToReducedFunctions(ProgramEditor& editor, const BuildContext& build, const ParameterRemovalPlan& plan)
{
    const Program& program = editor.program();
    auto isReduced = [&](const InlineFrame& frame) {
        return frame.callee && plan.reducedFunctionOf(frame.callee);
    };
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        if (std::ranges::none_of(program.functions[index].inlineFrames, isReduced))
            return;
        for (InlineFrame& frame : editor.at(FunctionID(index)).inlineFrames) {
            if (isReduced(frame))
                frame.callee = plan.reducedFunctionOf(frame.callee);
        }
    });
}

void removeUnusedParameters(ProgramEditor& editor, const OptimizationInputs& inputs, const PassRunner& runner)
{
    BuildContext& build = runner.build();
    const Program& program = editor.program();
    ParameterRemovalPlan plan = planParameterRemoval(program, inputs.callGraph, build);
    if (plan.candidates.isEmpty())
        return;

    addReducedFunctions(editor, build, plan);
    Vector<FunctionID> callers = findCallers(inputs.callGraph, build, plan);
    editFunctionsInParallel(editor, build, callers.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        if (retargetCalls(function, plan))
            simplifyEditedFunction(runner, program, inputs, id, function, arenas);
    });
    editFunctionsInParallel(editor, build, plan.candidates.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        AnalysisContext context { build, program, &inputs.flow, &inputs.effects, id };
        becomeForwarder(context, function, plan.reducedFunctions[index], plan.removed[id.index()], arenas.types);
    });
    moveInlineFramesToReducedFunctions(editor, build, plan);
    build.statistics().count(StatisticID::UnusedParametersRemoved, plan.numberOfRemovedParameters);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
