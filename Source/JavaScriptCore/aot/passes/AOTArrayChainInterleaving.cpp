/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayChains.h"

#if ENABLE(AOT)

#include "AOTBuiltinPristine.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsOfRules.h"
#include "AOTFunctionAnalyses.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

using StepEffects = Vector<Effects, ChainShape::maxNumberOfLinks + 2>;

static constexpr unsigned arrayArgumentIndex = 2;
static constexpr unsigned arrayArgumentIndexOfReducer = 3;
static constexpr AbstractHeapSet arrayHeaps { AbstractHeap::Elements, AbstractHeap::ArrayStorage };
static constexpr uint16_t primitivesJoinedWithoutUserCode = Primitive::AnyString | Primitive::Number | Primitive::Boolean | Primitive::BigInt | Primitive::UndefinedOrNull | Primitive::Absent;

static bool canBeObserved(const Effects& effects)
{
    return effects.writes.containsAny(observableHeaps) || effects.mayRunOtherCode();
}

static bool mayStop(const Effects& effects)
{
    return effects.mayThrow || !effects.alwaysCompletes();
}

static bool mayAccessSameMemory(const Effects& a, const Effects& b)
{
    LocationMasks orderedWritesOfA = a.writes.restrictedTo(orderedHeaps);
    LocationMasks orderedWritesOfB = b.writes.restrictedTo(orderedHeaps);
    if (orderedWritesOfA.overlaps(b.writes) || orderedWritesOfA.overlaps(b.reads) || orderedWritesOfB.overlaps(a.reads))
        return true;
    return (a.mayRunOtherCode() && !b.reads.restrictedTo(orderedHeaps).isEmpty())
        || (b.mayRunOtherCode() && !a.reads.restrictedTo(orderedHeaps).isEmpty());
}

static bool isFirstStepOnSource(const ArrayChain& chain, size_t step)
{
    return !step && !chain.rangeCall;
}

static bool isCallOfChain(const ArrayChain& chain, ValueID id)
{
    if (id == chain.rangeCall || id == chain.lastCall)
        return true;
    return chain.callbacks.containsIf([&](const ChainCallback& callback) {
        return callback.call == id;
    });
}

static bool isSourcePristine(FunctionPassContext& context, const ArrayChain& chain)
{
    PristineQuery query { context.program, context.function, &context.analyses };
    PristineRequirement requirement { BuiltinClasses::Array, { PristineAspects::Names, PristineAspects::Storage } };
    ProgramPoint point { chain.block, context.analyses.useLists().locationOf(chain.firstCall()).index };
    return pristineProofOf(query, chain.source, requirement, point) == PristineProof::Proven;
}

static bool isFunction(FunctionPassContext& context, ValueID value)
{
    const Type& type = context.program.typeOf(context.function, value);
    return type.isObject() && type.isSubsetOf(Type::function(), context.program);
}

static std::optional<unsigned> findParameterReceiving(const Function& callee, unsigned argumentIndex)
{
    unsigned numberOfPositionalParameters = callee.numberOfPositionalParameters();
    if (argumentIndex < numberOfPositionalParameters)
        return callee.firstParameterIndex() + argumentIndex;
    if (callee.has(FunctionFlags::HasRestParameter))
        return callee.firstParameterIndex() + numberOfPositionalParameters;
    return std::nullopt;
}

static ArrayChainDeclineReason findSeenIntermediateArray(FunctionPassContext& context, const ArrayChain& chain)
{
    for (size_t step = 0; step < chain.callbacks.size(); ++step) {
        if (isFirstStepOnSource(chain, step))
            continue;

        const ChainCallback& callback = chain.callbacks[step];
        if (!callback.callee)
            return ArrayChainDeclineReason::CalleeIsUnknown;

        const Function& callee = context.program.at(callback.callee);
        if (callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
            return ArrayChainDeclineReason::CallbackSeesIntermediateArray;

        bool isReducer = chain.shape.end == ChainEndKind::Reduce && callback.call == chain.lastCall;
        std::optional<unsigned> parameter = findParameterReceiving(callee, isReducer ? arrayArgumentIndexOfReducer : arrayArgumentIndex);
        if (parameter && !context.inputs.effects.of(callback.callee).parameter(*parameter).operand.uses.isEmpty())
            return ArrayChainDeclineReason::CallbackSeesIntermediateArray;
    }
    return ArrayChainDeclineReason::None;
}

static Effects effectsOfJoining(FunctionPassContext& context, ValueID joinedArray)
{
    const Program& program = context.program;
    const ClassPart* part = program.typeOf(context.function, joinedArray).findClassPart(BuiltinClasses::Array);
    if (!part || !part->contentsAreProven)
        return context.inputs.effects.unknownCode();
    if (!program.at(part->contentsAt(program.builtins(), ContentsSlot::Element)).hasOnlyPrimitives(primitivesJoinedWithoutUserCode))
        return context.inputs.effects.unknownCode();
    return Effects::none();
}

static Effects effectsOfFlattening(FunctionPassContext& context, FunctionID mapper)
{
    const Program& program = context.program;
    if (!mapper || program.at(mapper).results.size() != 1)
        return context.inputs.effects.unknownCode();

    const Type& parts = program.at(program.at(mapper).result().type);
    if (parts.hasObjects() && !pristineAspectsProvenBy(parts, BuiltinClasses::Array).contains(PristineAspects::Storage))
        return context.inputs.effects.unknownCode();
    return Effects::none();
}

static TypeID elementTypeOf(FunctionPassContext& context, ValueID array)
{
    const Program& program = context.program;
    const ClassPart* part = program.typeOf(context.function, array).findClassPart(BuiltinClasses::Array);
    return part && part->contentsAreProven ? part->contentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything;
}

static Effects effectsOfCallback(FunctionPassContext& context, const ChainCallback& callback, bool isReducer)
{
    EffectContext effects { context.program, context.inputs.effects };
    if (!callback.callee)
        return effectsOfCallees(effects, CalleeList::unknown(), BuiltinCallMode::Call).onAnyEdge();

    CalleeList callees = CalleeList::none();
    callees.add(callback.callee);
    std::optional<ArrayMethodCall> call = arrayMethodCallOf(context.program, context.function, callback.call);
    if (!call)
        return effectsOfCallees(effects, callees, BuiltinCallMode::Call).onAnyEdge();

    Vector<TypeID, 4> arguments;
    if (isReducer)
        arguments.append(TypeTable::anything);
    arguments.appendList({ elementTypeOf(context, call->receiver), TypeTable::length, context.function.at(call->receiver).type });
    TypeID thisValue = callback.thisArgument ? context.function.at(callback.thisArgument).type : TypeTable::undefined;
    return effectsOfCallees(effects, callees, BuiltinCallMode::Call, { thisValue, arguments.span() }).onAnyEdge();
}

static StepEffects collectStepEffects(FunctionPassContext& context, const ArrayChain& chain, const ArrayMethodCall& last)
{
    StepEffects effects;
    for (const ChainCallback& callback : chain.callbacks)
        effects.append(effectsOfCallback(context, callback, chain.shape.end == ChainEndKind::Reduce && callback.call == chain.lastCall));
    if (chain.shape.end == ChainEndKind::Join)
        effects.append(effectsOfJoining(context, last.receiver));
    if (chain.shape.end == ChainEndKind::FlatMap)
        effects.last().merge(effectsOfFlattening(context, chain.callbacks.last().callee));
    return effects;
}

static ArrayChainDeclineReason findInterference(std::span<const Effects> effects)
{
    for (size_t earlier = 0; earlier < effects.size(); ++earlier) {
        for (size_t later = earlier + 1; later < effects.size(); ++later) {
            const Effects& a = effects[earlier];
            const Effects& b = effects[later];
            if (mayAccessSameMemory(a, b))
                return ArrayChainDeclineReason::CallbacksInterfere;
            if (mayStop(a) && (canBeObserved(b) || mayStop(b)))
                return ArrayChainDeclineReason::CallbacksInterfere;
            if (mayStop(b) && canBeObserved(a))
                return ArrayChainDeclineReason::SkippedCallIsObservable;
        }
    }
    return ArrayChainDeclineReason::None;
}

static bool canSkipCalls(std::span<const Effects> effects)
{
    return std::ranges::none_of(effects, [](const Effects& step) {
        return canBeObserved(step) || mayStop(step);
    });
}

static bool mayWriteSource(FunctionPassContext& context, const ArrayChain& chain, std::span<const Effects> effects)
{
    bool mayWriteArrays = false;
    for (size_t step = 0; step < effects.size(); ++step)
        mayWriteArrays |= !isFirstStepOnSource(chain, step) && effects[step].writes.containsAny(arrayHeaps);
    if (!mayWriteArrays)
        return false;

    // The caller of this function holds an entry parameter, so other code reaches it whatever happens to it here.
    const EscapeAnalysis& escape = context.analyses.escape();
    std::optional<ObjectID> source = escape.singleObject(chain.source);
    if (!source)
        return true;
    const TrackedObject& tracked = escape.at(*source);
    return tracked.kind != ObjectKind::Allocation || tracked.state != EscapeState::DoesNotEscape;
}

static bool hasInterferingValueBetweenCalls(FunctionPassContext& context, const ArrayChain& chain, std::span<const Effects> effects)
{
    Effects moved = listReadEffects;
    for (const Effects& step : effects)
        moved.merge(step);

    FunctionAnalyses& analyses = context.analyses;
    const UseLists& useLists = analyses.useLists();
    const BlockValues& values = context.function.at(chain.block).values;
    uint32_t end = useLists.locationOf(chain.lastCall).index;
    for (uint32_t index = useLists.locationOf(chain.firstCall()).index + 1; index < end; ++index) {
        ValueID id = values[index];
        if (isCallOfChain(chain, analyses.sameObjectThroughCasts(id)))
            continue;
        if (analyses.effectsOf(id).onAnyEdge().interferesWith(moved))
            return true;
    }
    return false;
}

ArrayChainDeclineReason checkInterleaving(FunctionPassContext& context, const ArrayChain& chain)
{
    std::optional<ArrayMethodCall> last = arrayMethodCallOf(context.program, context.function, chain.lastCall);
    RELEASE_ASSERT(last);

    if (!isSourcePristine(context, chain))
        return ArrayChainDeclineReason::SourceIsNotPristine;

    bool hasOnlyFunctions = std::ranges::all_of(chain.callbacks, [&](const ChainCallback& callback) {
        return isFunction(context, callback.function);
    });
    if (!hasOnlyFunctions)
        return ArrayChainDeclineReason::CallbackIsNotAFunction;

    if (ArrayChainDeclineReason obstacle = findSeenIntermediateArray(context, chain); obstacle != ArrayChainDeclineReason::None)
        return obstacle;

    StepEffects effects = collectStepEffects(context, chain, *last);
    if (ArrayChainDeclineReason obstacle = findInterference(effects.span()); obstacle != ArrayChainDeclineReason::None)
        return obstacle;
    if (last->method->stopsEarly && !canSkipCalls(effects.span().first(chain.shape.numberOfLinks())))
        return ArrayChainDeclineReason::SkippedCallIsObservable;
    if (mayWriteSource(context, chain, effects.span()))
        return ArrayChainDeclineReason::CallbackWritesSource;
    if (hasInterferingValueBetweenCalls(context, chain, effects.span()))
        return ArrayChainDeclineReason::ValueBetweenLinksInterferes;
    return ArrayChainDeclineReason::None;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
