/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayChains.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectsOfRules.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIRProgram.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTRuleSupport.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

std::span<const ChainMethod> chainMethods()
{
    static constexpr std::array<ChainMethod, 12> methods { {
        { Builtins::ArrayPrototypeSlice, ChainMethodRole::Range, ChainLinkKind::None, ChainEndKind::Array, false, false },
        { Builtins::ArrayPrototypeMap, ChainMethodRole::Link, ChainLinkKind::Map, ChainEndKind::Array, true, false },
        { Builtins::ArrayPrototypeFilter, ChainMethodRole::Link, ChainLinkKind::Filter, ChainEndKind::Array, true, false },
        { Builtins::ArrayPrototypeFlatMap, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::FlatMap, true, false },
        { Builtins::ArrayPrototypeForEach, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::ForEach, true, false },
        { Builtins::ArrayPrototypeReduce, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Reduce, true, false },
        { Builtins::ArrayPrototypeSome, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Some, true, true },
        { Builtins::ArrayPrototypeEvery, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Every, true, true },
        { Builtins::ArrayPrototypeFind, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Find, true, true },
        { Builtins::ArrayPrototypeFindIndex, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::FindIndex, true, true },
        { Builtins::ArrayPrototypeJoin, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Join, false, false },
        { Builtins::ArrayPrototypeLength, ChainMethodRole::End, ChainLinkKind::None, ChainEndKind::Length, false, false },
    } };
    return methods;
}

const ChainMethod* findChainMethod(BuiltinID id)
{
    std::span<const ChainMethod> methods = chainMethods();
    auto method = std::ranges::find(methods, id, &ChainMethod::builtin);
    return method == methods.end() ? nullptr : &*method;
}

static std::optional<ArrayMethodCall> libraryMethodCallOf(const Program& program, const Function& function, ValueID id)
{
    const BuiltinTable& table = program.builtins();
    FunctionID callee = function.at(id).function();
    const Function& target = program.at(callee);
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, target.enclosingRuntimeFunction);
    if (!entry || entry->function != callee)
        return std::nullopt;
    const ChainMethod* method = nullptr;
    for (BuiltinID builtin : table.at(target.enclosingRuntimeFunction).implements)
        method = method ? method : findChainMethod(table.canonicalID(builtin));
    if (!method)
        return std::nullopt;

    std::span<const ValueID> operands = function.at(id).argumentsOperands();
    RELEASE_ASSERT(operands.size() == target.numberOfEntryParameters());
    std::optional<unsigned> thisParameter = target.thisParameterIndex();
    if (!thisParameter || target.has(FunctionFlags::HasRestParameter))
        return std::nullopt;
    Vector<ValueID, 2> arguments(operands.subspan(target.firstParameterIndex(), target.numberOfPositionalParameters()));
    if (std::optional<unsigned> hiddenRest = target.hiddenRestParameterIndex()) {
        const Value& rest = function.at(sameObjectThroughCasts(function, operands[*hiddenRest]));
        if (rest.opcode != Opcode::NewArray || !rest.holes().empty() || rest.capacityOperand())
            return std::nullopt;
        arguments.append(rest.elementsOperands());
    }
    if (std::optional<unsigned> argumentCount = target.argumentCountParameterIndex()) {
        std::optional<int64_t> count = integerConstantThroughCasts(program, function, operands[*argumentCount]);
        if (!count || *count < 0 || static_cast<uint64_t>(*count) > arguments.size())
            return std::nullopt;
        arguments.shrink(static_cast<size_t>(*count));
    }
    return ArrayMethodCall { method, id, operands[*thisParameter], WTF::move(arguments) };
}

std::optional<ArrayMethodCall> arrayMethodCallOf(const Program& program, const Function& function, ValueID id)
{
    const Value& value = function.at(id);
    if (value.opcode == Opcode::GetArrayLength)
        return ArrayMethodCall { findChainMethod(Builtins::ArrayPrototypeLength), id, value.arrayOperand(), { } };
    if (!arrayMethodCallOpcodes.contains(value.opcode))
        return std::nullopt;
    if (value.opcode == Opcode::CallDirect)
        return libraryMethodCallOf(program, function, id);

    BuiltinCall call = builtinCallOf(program, value);
    if (call.form != BuiltinCallForm::Whole || !call.receiver)
        return std::nullopt;
    const BuiltinOverload& overload = program.builtins().at(call.overload);
    bool isCallOrRead = overload.mode == BuiltinCallMode::Call || overload.mode == BuiltinCallMode::Get;
    const ChainMethod* method = isCallOrRead ? findChainMethod(program.builtins().canonicalID(overload.builtin)) : nullptr;
    if (!method)
        return std::nullopt;
    return ArrayMethodCall { method, id, call.receiver, Vector<ValueID, 2>(call.arguments) };
}

FunctionID findChainTraversal(const Program& program)
{
    std::span<const BuiltinOverload> overloads = program.builtins().overloadsOf(Builtins::CompilerHelpersTraverseArray);
    RELEASE_ASSERT(!overloads.empty());
    const BuiltinImplementation& implementation = overloads.front().code.implementation;
    RELEASE_ASSERT(implementation.kind == BuiltinImplementationKind::Compiled);
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, implementation.function);
    return entry && entry->function && program.at(entry->function).hasBody() ? entry->function : FunctionID();
}

Function buildChainStep(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    static constexpr unsigned endCallbackSlot = 2 * ChainShape::maxNumberOfLinks;
    ChainShape shape = ChainShape::decode(key.parameter);
    const Function& traversal = inputs.program.at(key.target);

    Function step;
    step.name = traversal.name;
    step.module = traversal.module;
    step.position = traversal.position;
    step.text = traversal.text;
    step.kind = traversal.kind;
    step.role = FunctionRole::ChainStep;
    step.flags = traversal.flags & FunctionFlags::IsHiddenInStackTraces;
    step.thisType = DeclaredType { .type = TypeTable::anything };
    step.results.append(DeclaredType { .type = TypeTable::anything });
    step.capabilities = initialCapabilitiesOf(step);
    step.creator = CreatorRecord::ChainStep { };

    IRBuilder builder({ inputs.build, inputs.program, &inputs.flow, nullptr }, step, inputs.types);
    builder.setOrigin({ traversal.position, 0 });
    BlockID block = builder.addBlock();
    auto addParameter = [&] {
        step.parameters.append(DeclaredType { .type = TypeTable::anything });
        return builder.addParameter(block, TypeTable::anything);
    };
    ValueID array = addParameter();
    ValueID start = addParameter();
    ValueID end = addParameter();
    std::array<ValueID, endCallbackSlot + 2> slots { };
    for (unsigned slot = 0; slot < 2 * shape.numberOfLinks(); ++slot)
        slots[slot] = addParameter();
    if (endTakesCallback(shape.end))
        slots[endCallbackSlot] = addParameter();
    if (endTakesThisArgument(shape.end))
        slots[endCallbackSlot + 1] = addParameter();
    ValueID operand = endTakesOperand(shape) ? addParameter() : ValueID();

    builder.moveToEndOf(block);
    ValueID undefined = builder.appendUndefined();
    std::ranges::replace(slots, ValueID(), undefined);
    Value operandList = IRBuilder::make(Opcode::NewArray);
    operandList.setContents(TypeTable::anything);
    operandList.setElementsOperands(slots);
    ValueID operands = builder.append(WTF::move(operandList));
    ValueID shapeWord = builder.appendInteger(key.parameter);
    std::array<ValueID, 6> arguments { array, start, end, shapeWord, operands, operand ? operand : undefined };
    ValueID call = appendDirectCall(builder, { .callee = key.target, .arguments = arguments });
    step.edit(call, ValueEdit::Definition).setIsAlwaysInlined(true);
    emitReturnOfCall(builder, call);
    return step;
}

namespace {

struct ArrayChainFinder {
    using Links = Vector<ArrayMethodCall, ChainShape::maxNumberOfLinks>;

    FunctionPassContext& context;
    const BitVector* callsTakenAlone;
    const Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    BitVector isInChain { };
    Vector<ArrayChain> chains { };

    const Type& typeOf(ValueID id) const { return context.program.typeOf(function, id); }

    FunctionID calleeOf(ValueID callback) const
    {
        CalleeCandidates candidates = calleeCandidates(context.program, function, callback);
        return candidates.isProven() && candidates.proven.size() == 1 && !candidates.hasOtherCallees && !candidates.mayBeNullish ? candidates.proven[0] : FunctionID();
    }

    bool prependCallback(ArrayChain& chain, const ArrayMethodCall& call, ValueID thisArgument)
    {
        ValueID callback = call.argumentOrNull(0);
        if (callback)
            chain.callbacks.insert(0, ChainCallback { call.call, callback, thisArgument, calleeOf(callback) });
        return !!callback;
    }

    void tryChain(BlockID block, const ArrayMethodCall& last)
    {
        ArrayChain chain;
        chain.block = block;
        chain.lastCall = last.call;
        Links linksFromLast;
        switch (last.method->role) {
        case ChainMethodRole::Range:
            return;
        case ChainMethodRole::Link:
            chain.shape.end = ChainEndKind::Array;
            linksFromLast.append(last);
            break;
        case ChainMethodRole::End:
            chain.shape.end = last.method->end;
            if (!takeEnd(chain, last))
                return;
            break;
        }
        chain.source = collectLinks(chain, last, linksFromLast);

        size_t numberOfLinks = linksFromLast.size();
        if (numberOfLinks + (chain.rangeCall ? 1 : 0) + (chain.shape.end != ChainEndKind::Array ? 1 : 0) < 2 && !paysAlone(last))
            return;
        for (size_t i = 0; i < numberOfLinks; ++i) {
            const ArrayMethodCall& link = linksFromLast[i];
            if (!prependCallback(chain, link, link.argumentOrNull(1)))
                return;
            chain.shape.links[numberOfLinks - 1 - i] = link.method->link;
        }
        if (!isWorthFusingIn(chain))
            return;

        isInChain.set(chain.lastCall.index());
        if (chain.rangeCall)
            isInChain.set(chain.rangeCall.index());
        for (const ArrayMethodCall& link : linksFromLast)
            isInChain.set(link.call.index());
        chains.append(WTF::move(chain));
    }

    // The loop of the traversal runs once for each element, however seldom the chain itself is reached. In cold code it pays where each callback goes into the loop.
    bool isWorthFusingIn(const ArrayChain& chain)
    {
        if (analyses.blockFrequencies().classOf(chain.block) == BlockFrequency::Rare || context.runsAtMostOnce(chain.block))
            return false;
        if (!isCold(heatClassOf(context, chain.block)))
            return true;
        // Outside the loop the text is put together by calls, which cost what `join` costs and more.
        if (context.inputs.heat.isCompact(context.program, context.id) || chain.shape.end == ChainEndKind::Join)
            return false;
        return !chain.callbacks.isEmpty() && std::ranges::all_of(chain.callbacks, [](const ChainCallback& callback) {
            return !!callback.callee;
        });
    }

    // The traversal knows how long the array of a filter or of a flatMap can get, and gives the array of a map storage for the numbers that the callback returns.
    bool paysAlone(const ArrayMethodCall& call) const
    {
        const Program& program = context.program;
        if (!callsTakenAlone || !callsTakenAlone->get(call.call.index()))
            return false;
        if (call.method->role == ChainMethodRole::End)
            return call.method->end == ChainEndKind::FlatMap;
        switch (call.method->link) {
        case ChainLinkKind::None:
            return false;
        case ChainLinkKind::Filter:
            return true;
        case ChainLinkKind::Map:
            break;
        }
        FunctionID callee = call.arguments.isEmpty() ? FunctionID() : calleeOf(call.arguments[0]);
        return callee && program.at(callee).results.size() == 1 && arrayStorageAtAllocation(program.at(program.at(callee).results[0].type)) != ArrayStorage::Contiguous;
    }

    bool takeEnd(ArrayChain& chain, const ArrayMethodCall& last)
    {
        switch (last.method->end) {
        case ChainEndKind::Array:
        case ChainEndKind::Length:
            return true;
        case ChainEndKind::ForEach:
        case ChainEndKind::Some:
        case ChainEndKind::Every:
        case ChainEndKind::Find:
        case ChainEndKind::FindIndex:
        case ChainEndKind::FlatMap:
            return prependCallback(chain, last, last.argumentOrNull(1));
        case ChainEndKind::Reduce:
            if (last.arguments.size() >= 2) {
                chain.shape.hasInitialValue = true;
                chain.endOperand = last.arguments[1];
            }
            return prependCallback(chain, last, { });
        case ChainEndKind::Join:
            if (last.arguments.isEmpty())
                return true;
            if (last.arguments.size() > 1 || !typeOf(last.arguments[0]).isAnyString())
                return false;
            chain.endOperand = last.arguments[0];
            return true;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    ValueID collectLinks(ArrayChain& chain, const ArrayMethodCall& last, Links& linksFromLast)
    {
        ArrayMethodCall consumer = last;
        while (true) {
            ValueID receiver = consumer.receiver;
            ValueID producer = analyses.sameObjectThroughCasts(receiver);
            std::optional<ArrayMethodCall> link = arrayMethodCallOf(context.program, function, producer);
            if (!link || analyses.useLists().blockOf(producer) != chain.block || !isUsedOnlyBy(receiver, consumer))
                return receiver;
            switch (link->method->role) {
            case ChainMethodRole::End:
                return receiver;
            case ChainMethodRole::Range: {
                ValueID from = link->argumentOrNull(0);
                ValueID to = link->argumentOrNull(1);
                if ((from && !typeOf(from).isInteger()) || (to && !typeOf(to).isInteger()))
                    return receiver;
                chain.rangeCall = link->call;
                chain.from = from;
                chain.to = to;
                return link->receiver;
            }
            case ChainMethodRole::Link:
                if (linksFromLast.size() == ChainShape::maxNumberOfLinks)
                    return receiver;
                linksFromLast.append(*link);
                consumer = WTF::move(*link);
                break;
            }
        }
    }

    bool isUsedOnlyBy(ValueID receiver, const ArrayMethodCall& consumer)
    {
        CastChain casts(function, receiver);
        for (ValueID argument : consumer.arguments) {
            if (analyses.sameObjectThroughCasts(argument) == casts.last())
                return false;
        }
        auto isPassingCast = [&](ValueID cast) {
            const ClassPart* part = typeOf(cast).singleBuiltinClass();
            return !analyses.mayThrow(cast) || (part && part->builtinClass == BuiltinClasses::Array);
        };
        const UseLists& useLists = analyses.useLists();
        ValueID user = consumer.call;
        for (ValueID value : casts) {
            if (value != casts.last() && !isPassingCast(value))
                return false;
            for (const Use& use : useLists.usesOf(value)) {
                // A validating build looks at each array of the chain, and goes without the look where there is no array.
                bool isValidation = use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ValidateType && use.block == useLists.blockOf(value);
                if (use.kind != Use::Kind::Operand || (!isValidation && use.user != user && use.user != consumer.call))
                    return false;
            }
            user = value;
        }
        return true;
    }
};

} // anonymous namespace

Vector<ArrayChain> findArrayChains(FunctionPassContext& context, const BitVector* callsTakenAlone)
{
    ArrayChainFinder finder { context, callsTakenAlone };
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = context.function.at(block).values;
        for (size_t index = values.size(); index--;) {
            if (finder.isInChain.get(values[index].index()))
                continue;
            if (std::optional<ArrayMethodCall> call = arrayMethodCallOf(context.program, context.function, values[index]))
                finder.tryChain(block, *call);
        }
    }
    return WTF::move(finder.chains);
}

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

static ArrayChainDeclineReason findSeenIntermediateArray(FunctionPassContext& context, const ArrayChain& chain)
{
    for (size_t step = chain.rangeCall ? 0 : 1; step < chain.callbacks.size(); ++step) {
        const ChainCallback& callback = chain.callbacks[step];
        if (!callback.callee)
            return ArrayChainDeclineReason::CalleeIsUnknown;
        const Function& callee = context.program.at(callback.callee);
        if (callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
            return ArrayChainDeclineReason::CallbackSeesIntermediateArray;

        bool isReducer = chain.shape.end == ChainEndKind::Reduce && callback.call == chain.lastCall;
        unsigned argumentIndex = isReducer ? arrayArgumentIndexOfReducer : arrayArgumentIndex;
        if (argumentIndex >= callee.numberOfPositionalParameters() && !callee.has(FunctionFlags::HasRestParameter))
            continue;
        unsigned parameter = callee.firstParameterIndex() + std::min(argumentIndex, callee.numberOfPositionalParameters());
        if (!context.inputs.effects.of(callback.callee).operand(parameter).uses.isEmpty())
            return ArrayChainDeclineReason::CallbackSeesIntermediateArray;
    }
    return ArrayChainDeclineReason::None;
}

static StepEffects collectStepEffects(FunctionPassContext& context, const ArrayChain& chain, ValueID joinedArray)
{
    const Program& program = context.program;
    EffectContext effectContext { program, context.inputs.effects };
    StepEffects effects;
    for (const ChainCallback& callback : chain.callbacks) {
        CalleeList callees = callback.callee ? CalleeList::none() : CalleeList::unknown();
        if (callback.callee)
            callees.add(callback.callee);
        effects.append(effectsOfCallees(effectContext, callees, BuiltinCallMode::Call).onAnyEdge());
    }
    if (chain.shape.end == ChainEndKind::Join) {
        const ClassPart* part = program.typeOf(context.function, joinedArray).findClassPart(BuiltinClasses::Array);
        bool runsNoUserCode = part && part->contentsAreProven && program.at(part->contentsAt(program.builtins(), ContentsSlot::Element)).hasOnlyPrimitives(primitivesJoinedWithoutUserCode);
        effects.append(runsNoUserCode ? Effects::none() : context.inputs.effects.unknownCode());
    }
    if (chain.shape.end == ChainEndKind::FlatMap) {
        FunctionID mapper = chain.callbacks.last().callee;
        bool hasProvenResult = mapper && program.at(mapper).results.size() == 1 && !program.at(mapper).result().flags.contains(PositionFlags::IsExpected);
        const Type* parts = hasProvenResult ? &program.at(program.at(mapper).result().type) : nullptr;
        if (!parts || (parts->hasObjects() && !pristineAspectsProvenBy(*parts, BuiltinClasses::Array).contains(PristineAspects::Storage)))
            effects.last().merge(context.inputs.effects.unknownCode());
    }
    return effects;
}

static ArrayChainDeclineReason findInterference(std::span<const Effects> effects)
{
    for (size_t earlier = 0; earlier < effects.size(); ++earlier) {
        for (size_t later = earlier + 1; later < effects.size(); ++later) {
            const Effects& a = effects[earlier];
            const Effects& b = effects[later];
            if (mayAccessSameMemory(a, b) || (mayStop(a) && (canBeObserved(b) || mayStop(b))))
                return ArrayChainDeclineReason::CallbacksInterfere;
            if (mayStop(b) && canBeObserved(a))
                return ArrayChainDeclineReason::SkippedCallIsObservable;
        }
    }
    return ArrayChainDeclineReason::None;
}

static bool mayWriteSource(FunctionPassContext& context, const ArrayChain& chain, std::span<const Effects> effects)
{
    bool mayWriteArrays = std::ranges::any_of(effects.subspan(std::min<size_t>(chain.rangeCall ? 0 : 1, effects.size())), [](const Effects& step) {
        return step.writes.containsAny(arrayHeaps);
    });
    if (!mayWriteArrays)
        return false;
    // The caller of this function holds an entry parameter, so other code reaches it whatever happens to it here.
    const EscapeAnalysis& escape = context.analyses.escape();
    std::optional<ObjectID> source = escape.singleObject(chain.source);
    return !source || escape.at(*source).kind != ObjectKind::Allocation || escape.at(*source).state != EscapeState::DoesNotEscape || !escape.containersOf(*source).empty();
}

static bool hasInterferingValueBetweenCalls(FunctionPassContext& context, const ArrayChain& chain, std::span<const Effects> effects)
{
    Effects moved = listReadEffects;
    for (const Effects& step : effects)
        moved.merge(step);
    auto isCallOfChain = [&](ValueID id) {
        return id == chain.rangeCall || id == chain.lastCall || chain.callbacks.containsIf([&](const ChainCallback& callback) {
            return callback.call == id;
        });
    };
    FunctionAnalyses& analyses = context.analyses;
    const BlockValues& values = context.function.at(chain.block).values;
    uint32_t end = analyses.useLists().locationOf(chain.lastCall).index;
    for (uint32_t index = analyses.useLists().locationOf(chain.firstCall()).index + 1; index < end; ++index) {
        if (!isCallOfChain(analyses.sameObjectThroughCasts(values[index])) && analyses.effectsOf(values[index]).onAnyEdge().interferesWith(moved))
            return true;
    }
    return false;
}

ArrayChainDeclineReason checkInterleaving(FunctionPassContext& context, const ArrayChain& chain)
{
    const Program& program = context.program;
    std::optional<ArrayMethodCall> last = arrayMethodCallOf(program, context.function, chain.lastCall);
    RELEASE_ASSERT(last);

    PristineQuery query { program, context.function, &context.analyses };
    PristineRequirement requirement { BuiltinClasses::Array, { PristineAspects::Names, PristineAspects::Storage } };
    ProgramPoint point { chain.block, context.analyses.useLists().locationOf(chain.firstCall()).index };
    if (pristineProofOf(query, chain.source, requirement, point) != PristineProof::Proven)
        return ArrayChainDeclineReason::SourceIsNotPristine;
    for (const ChainCallback& callback : chain.callbacks) {
        const Type& type = program.typeOf(context.function, callback.function);
        if (!type.isObject() || !type.isSubsetOf(Type::function(), program))
            return ArrayChainDeclineReason::CallbackIsNotAFunction;
    }
    if (ArrayChainDeclineReason obstacle = findSeenIntermediateArray(context, chain); obstacle != ArrayChainDeclineReason::None)
        return obstacle;

    StepEffects effects = collectStepEffects(context, chain, last->receiver);
    if (ArrayChainDeclineReason obstacle = findInterference(effects.span()); obstacle != ArrayChainDeclineReason::None)
        return obstacle;
    bool skipsCallThatShows = last->method->stopsEarly && std::ranges::any_of(effects.span().first(chain.shape.numberOfLinks()), [](const Effects& step) {
        return canBeObserved(step) || mayStop(step);
    });
    if (skipsCallThatShows)
        return ArrayChainDeclineReason::SkippedCallIsObservable;
    if (mayWriteSource(context, chain, effects.span()))
        return ArrayChainDeclineReason::CallbackWritesSource;
    if (hasInterferingValueBetweenCalls(context, chain, effects.span()))
        return ArrayChainDeclineReason::ValueBetweenLinksInterferes;
    return ArrayChainDeclineReason::None;
}

static uint32_t indexInBlock(const UseLists& useLists, BlockID block, ValueID value)
{
    ValueLocation location = useLists.locationOf(value);
    RELEASE_ASSERT(location.placement == ValuePlacement::InBlock && location.block == block);
    return location.index;
}

static void markUnsharedHiddenRest(FunctionPassContext& context, BlockID block, ValueID call, BitVector& indexes)
{
    const Value& value = context.function.at(call);
    std::optional<unsigned> hiddenRest = value.opcode == Opcode::CallDirect ? context.program.at(value.function()).hiddenRestParameterIndex() : std::nullopt;
    if (!hiddenRest)
        return;
    const UseLists& useLists = context.analyses.useLists();
    Vector<uint32_t, 2> unsharedIndexes;
    ValueID user = call;
    for (ValueID id : CastChain(context.function, value.argumentsOperands()[*hiddenRest])) {
        std::optional<Use> use = useLists.singleUse(id);
        ValueLocation location = useLists.locationOf(id);
        bool isUnshared = use && use->kind == Use::Kind::Operand && use->user == user;
        if (!isUnshared || location.placement != ValuePlacement::InBlock || location.block != block)
            return;
        unsharedIndexes.append(location.index);
        user = id;
    }
    for (uint32_t index : unsharedIndexes)
        indexes.set(index);
}

// All that goes with the chain but its last call.
static BitVector findErasedIndexes(FunctionPassContext& context, const ArrayChain& chain)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    Vector<ValueID, ChainShape::maxNumberOfLinks + 2> calls;
    if (chain.rangeCall)
        calls.append(chain.rangeCall);
    for (const ChainCallback& callback : chain.callbacks) {
        if (callback.call != chain.lastCall)
            calls.append(callback.call);
    }
    calls.append(chain.lastCall);

    BitVector erased;
    ValueID producer;
    for (ValueID call : calls) {
        if (producer) {
            std::optional<ArrayMethodCall> consumer = arrayMethodCallOf(context.program, function, call);
            RELEASE_ASSERT(consumer);
            CastChain receiver(function, consumer->receiver);
            RELEASE_ASSERT(receiver.last() == producer);
            for (ValueID value : receiver) {
                erased.set(indexInBlock(useLists, chain.block, value));
                for (const Use& use : useLists.usesOf(value)) {
                    if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ValidateType)
                        erased.set(indexInBlock(useLists, chain.block, use.user));
                }
            }
        }
        markUnsharedHiddenRest(context, chain.block, call, erased);
        producer = call;
    }
    return erased;
}

static Vector<ValueID, 3 + 2 * (ChainShape::maxNumberOfLinks + 1) + 1> createStepArguments(FunctionPassContext& context, const ArrayChain& chain, const InsertValue& insert)
{
    const ChainShape& shape = chain.shape;
    unsigned numberOfLinks = shape.numberOfLinks();
    RELEASE_ASSERT(chain.callbacks.size() == numberOfLinks + endTakesCallback(shape.end));
    RELEASE_ASSERT(chain.endOperand || !endTakesOperand(shape) || shape.end == ChainEndKind::Join);

    ValueID undefined;
    auto valueOrUndefined = [&](ValueID value) {
        if (!value && !undefined)
            undefined = insert(IRBuilder::make(Opcode::Undefined));
        return value ? value : undefined;
    };
    Vector<ValueID, 3 + 2 * (ChainShape::maxNumberOfLinks + 1) + 1> arguments;
    arguments.append(chain.source);
    arguments.append(chain.from ? chain.from : insert(makeInteger(0)));
    arguments.append(valueOrUndefined(chain.to));
    for (unsigned index = 0; index < chain.callbacks.size(); ++index) {
        arguments.append(chain.callbacks[index].function);
        if (index < numberOfLinks || endTakesThisArgument(shape.end))
            arguments.append(valueOrUndefined(chain.callbacks[index].thisArgument));
    }
    if (endTakesOperand(shape) && chain.endOperand)
        arguments.append(chain.endOperand);
    else if (endTakesOperand(shape)) {
        Value separator = IRBuilder::make(Opcode::String);
        separator.setString(context.strings.intern(StringView(","_s)));
        arguments.append(insert(WTF::move(separator)));
    }
    return arguments;
}

// The traversal stands for one call of a built-in that is written in JavaScript: a trace that is taken in the callback names the built-in between the callback and the caller, as it does where the text of the built-in takes the place of the call.
// `flatMap` ends in a tail call of the helper that calls the callback, and a trace names neither.
static std::optional<Origin> enterFrameOfOnlyCall(const Program& program, Function& function, const ArrayChain& chain)
{
    const Value& call = function.at(chain.lastCall);
    if (chain.firstCall() != chain.lastCall || chain.shape.end != ChainEndKind::Array || call.opcode != Opcode::CallDirect)
        return std::nullopt;
    const Function& builtin = program.at(call.function());
    Vector<InlineFrame>& frames = function.inlineFrames;
    if (frames.isEmpty())
        frames.append(InlineFrame { });
    frames.append(InlineFrame { .callee = call.function(), .callPosition = call.position, .parent = call.inlineFrame, .isTailCall = call.isDemotedTailCall(), .module = builtin.module });
    return Origin { builtin.position, static_cast<uint32_t>(frames.size() - 1) };
}

static void rewriteArrayChain(FunctionPassContext& context, IRBuilder& builder, const ArrayChain& chain)
{
    Function& function = context.function;
    FunctionID step = context.derivedFunction(derivedFunctionKey(FunctionRole::ChainStep, findChainTraversal(context.program), chain.shape.encode()));

    BitVector erasedIndexes = findErasedIndexes(context, chain);
    ProgramPoint point { chain.block, indexInBlock(context.analyses.useLists(), chain.block, chain.lastCall) };
    std::optional<Origin> originInBuiltin = enterFrameOfOnlyCall(context.program, function, chain);
    Origin origin = originInBuiltin ? *originInBuiltin : function.at(chain.lastCall).origin();
    TypeID resultType = function.at(chain.lastCall).type;
    size_t numberOfValuesBeforeInsertions = function.at(chain.block).values.size();

    builder.setOrigin(origin);
    // A ScopedLambda hands its functor each argument as an lvalue.
    auto insertBeforeLastCall = [&](auto&& value) -> ValueID {
        return builder.insertBefore(point, WTF::move(value));
    };
    InsertValue insert(insertBeforeLastCall);
    ValueInsertion insertion { context.analyses.context(), function, context.types, origin, insert };

    auto arguments = createStepArguments(context, chain, insert);
    Value call = makeDirectCall(insertion, { .callee = step, .arguments = arguments.span() });
    const Value& lastCall = function.at(chain.lastCall);
    call.setIsDemotedTailCall(!originInBuiltin && FlagMask::isDemotedTailCall(lastCall.opcode) && lastCall.isDemotedTailCall());
    // A step that is called calls each callback by value, and costs more than the methods that it stands for.
    call.setIsAlwaysInlined(true);
    ValueID fused = insert(WTF::move(call));
    ValueID converted = convertTo(insertion, fused, DeclaredType { .type = resultType });
    builder.applyInsertions();

    ValueSubstitution substitution(function);
    substitution.replace(chain.lastCall, converted);
    substitution.apply(function, &context.analyses);

    const BlockValues& values = function.at(chain.block).values;
    size_t indexOfLastCall = point.index + (values.size() - numberOfValuesBeforeInsertions);
    RELEASE_ASSERT(values[indexOfLastCall] == chain.lastCall);
    erasedIndexes.set(indexOfLastCall);
    builder.eraseValues(chain.block, erasedIndexes);
}

static bool endsGuardInUse(FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        GuardAvailability::Cursor cursor(analyses.guards(), block);
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            if (reliesOf(value.opcode).isEmpty())
                continue;
            cursor.advanceTo(index);
            for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
                ValueID guard = value.operand(operandIndex);
                if (guard && value.operandKind(operandIndex) == OperandKind::Guard && cursor.stateOf(guard, value) == GuardState::Unavailable)
                    return true;
            }
        }
    }
    return false;
}

PassOutcome fuseArrayMethodsTakenAlone(FunctionPassContext& context, const BitVector& calls)
{
    if (context.function.role == FunctionRole::ChainStep || context.function.enclosingRuntimeFunction)
        return PassOutcome::Unchanged;
    FunctionID traversal = findChainTraversal(context.program);
    if (!traversal)
        return PassOutcome::Unchanged;

    Vector<ArrayChain> accepted;
    for (ArrayChain& chain : findArrayChains(context, &calls)) {
        ArrayChainDeclineReason obstacle = checkInterleaving(context, chain);
        if (obstacle == ArrayChainDeclineReason::None)
            accepted.append(WTF::move(chain));
        else
            context.addDecline(Wire::DeclineKind::ArrayChainNotFused, chain.lastCall, obstacle);
    }
    if (accepted.isEmpty())
        return PassOutcome::Unchanged;
    auto declineAll = [&] {
        for (const ArrayChain& chain : accepted)
            context.addDecline(Wire::DeclineKind::ArrayChainNotFused, chain.lastCall, ArrayChainDeclineReason::Unknown);
        return PassOutcome::Unchanged;
    };
    // The traversal is library code.
    const EffectContext& effects = context.analyses.effectContext();
    if (!CalleeSummary(effects, traversal)->onCall().isSubsetOf(CalleeSummary(effects, context.id)->onCall()))
        return declineAll();

    // An ArrayChain names its operands by id, so a chain that reads the result of another one is rewritten before that result is replaced.
    AOT::sort(accepted, std::ranges::greater { }, [&, &cfg = context.analyses.cfg(), &useLists = context.analyses.useLists()](const ArrayChain& chain) {
        return std::pair { cfg.reversePostOrderIndex(chain.block), useLists.locationOf(chain.lastCall).index };
    });
    // A test in front of a chain may hold behind it only because the calls of the chain say how little they write. Whether it still holds shows on the fused body.
    Function fused = context.function;
    {
        FunctionAnalyses analyses(context.analyses.context(), fused);
        FunctionPassContext fusedContext { context.program, context.inputs, context.id, fused, analyses, context.types, context.strings, context.build };
        IRBuilder builder(analyses.context(), fused, context.types, &analyses);
        for (const ArrayChain& chain : accepted) {
            const Terminator& terminator = fused.at(chain.block).terminator;
            if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == chain.lastCall) {
                builder.demoteReturnCall(chain.block);
                context.build.statistics().count(StatisticID::ReturnCallsDemoted);
            }
            rewriteArrayChain(fusedContext, builder, chain);
        }
    }
    FunctionAnalyses analysesOfFused(context.analyses.context(), fused);
    if (endsGuardInUse(analysesOfFused))
        return declineAll();

    context.function = WTF::move(fused);
    context.build.statistics().count(StatisticID::ArrayChainsFused, accepted.size());
    // A chain is rewritten inside its block: the fused body has the blocks, the targets and the handlers of the body that it replaces.
    context.keptAnalyses = controlFlowAnalyses;
    return PassOutcome::Changed;
}

PassOutcome fuseArrayChains(FunctionPassContext& context)
{
    return fuseArrayMethodsTakenAlone(context, { });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
