/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTArrayChains.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectSummaries.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTStringArena.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using ChainCalls = Vector<ValueID, ChainShape::maxNumberOfLinks + 2>;
using StepArguments = Vector<ValueID, 3 + 2 * (ChainShape::maxNumberOfLinks + 1) + 1>;

struct ErasedIndexes {
    uint32_t lastCall { 0 };
    BitVector otherValues;
};

} // anonymous namespace

static ChainCalls callsInOrder(const ArrayChain& chain)
{
    ChainCalls calls;
    if (chain.rangeCall)
        calls.append(chain.rangeCall);
    for (const ChainCallback& callback : chain.callbacks) {
        if (callback.call != chain.lastCall)
            calls.append(callback.call);
    }
    calls.append(chain.lastCall);
    return calls;
}

static uint32_t indexInBlock(const UseLists& useLists, BlockID block, ValueID value)
{
    ValueLocation location = useLists.locationOf(value);
    RELEASE_ASSERT(location.placement == ValuePlacement::InBlock && location.block == block);
    return location.index;
}

static void markUnsharedHiddenRest(FunctionPassContext& context, BlockID block, ValueID call, BitVector& indexes)
{
    const Function& function = context.function;
    const Value& value = function.at(call);
    if (value.opcode != Opcode::CallDirect)
        return;
    std::optional<unsigned> hiddenRest = context.program.at(value.function()).hiddenRestParameterIndex();
    if (!hiddenRest)
        return;

    const UseLists& useLists = context.analyses.useLists();
    Vector<uint32_t, 2> unsharedIndexes;
    ValueID user = call;
    for (ValueID id : CastChain(function, value.argumentsOperands()[*hiddenRest])) {
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

static ErasedIndexes findErasedIndexes(FunctionPassContext& context, const ArrayChain& chain)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();

    ErasedIndexes erased;
    ValueID producer;
    for (ValueID call : callsInOrder(chain)) {
        if (producer) {
            std::optional<ArrayMethodCall> consumer = arrayMethodCallOf(context.program, function, call);
            RELEASE_ASSERT(consumer);
            CastChain receiver(function, consumer->receiver);
            RELEASE_ASSERT(receiver.last() == producer);
            for (ValueID value : receiver)
                erased.otherValues.set(indexInBlock(useLists, chain.block, value));
        }
        markUnsharedHiddenRest(context, chain.block, call, erased.otherValues);
        producer = call;
    }
    erased.lastCall = indexInBlock(useLists, chain.block, chain.lastCall);
    return erased;
}

static Value makeFirstIndex()
{
    Value zero = IRBuilder::make(Opcode::Integer);
    zero.setInteger(0);
    return zero;
}

static Value makeDefaultSeparator(StringArena& strings)
{
    Value separator = IRBuilder::make(Opcode::String);
    separator.setString(strings.intern(StringView(","_s)));
    return separator;
}

static StepArguments createStepArguments(FunctionPassContext& context, const ArrayChain& chain, const InsertValue& insert)
{
    const ChainShape& shape = chain.shape;
    unsigned numberOfLinks = shape.numberOfLinks();
    RELEASE_ASSERT(chain.callbacks.size() == numberOfLinks + endTakesCallback(shape.end));
    RELEASE_ASSERT(chain.endOperand || !endTakesOperand(shape) || shape.end == ChainEndKind::Join);

    ValueID undefined;
    auto valueOrUndefined = [&](ValueID value) {
        if (value)
            return value;
        if (!undefined)
            undefined = insert(IRBuilder::make(Opcode::Undefined));
        return undefined;
    };

    StepArguments arguments;
    arguments.append(chain.source);
    arguments.append(chain.from ? chain.from : insert(makeFirstIndex()));
    arguments.append(valueOrUndefined(chain.to));
    for (unsigned index = 0; index < chain.callbacks.size(); ++index) {
        const ChainCallback& callback = chain.callbacks[index];
        arguments.append(callback.function);
        if (index < numberOfLinks || endTakesThisArgument(shape.end))
            arguments.append(valueOrUndefined(callback.thisArgument));
    }
    if (endTakesOperand(shape))
        arguments.append(chain.endOperand ? chain.endOperand : insert(makeDefaultSeparator(context.strings)));
    return arguments;
}

void rewriteArrayChain(FunctionPassContext& context, IRBuilder& builder, const ArrayChain& chain)
{
    Function& function = context.function;
    FunctionID step = context.derivedFunction(derivedFunctionKey(FunctionRole::ChainStep, findChainTraversal(context.program), chain.shape.encode()));

    ErasedIndexes erased = findErasedIndexes(context, chain);
    ProgramPoint point { chain.block, erased.lastCall };
    Origin origin = function.at(chain.lastCall).origin();
    TypeID resultType = function.at(chain.lastCall).type;
    size_t numberOfValuesBeforeInsertions = function.at(chain.block).values.size();

    builder.setOrigin(origin);
    // A ScopedLambda hands its functor each argument as an lvalue.
    auto insertBeforeLastCall = [&](auto&& value) -> ValueID {
        return builder.insertBefore(point, WTF::move(value));
    };
    InsertValue insert(insertBeforeLastCall);
    ValueInsertion insertion { context.analyses.context(), function, context.types, origin, insert };

    StepArguments arguments = createStepArguments(context, chain, insert);
    ValueID fused = insert(makeDirectCall(insertion, { .callee = step, .arguments = arguments.span() }));
    ValueID converted = convertTo(insertion, fused, DeclaredType { .type = resultType });
    builder.applyInsertions();

    ValueSubstitution substitution(function);
    substitution.replace(chain.lastCall, converted);
    substitution.apply(function, &context.analyses);

    const BlockValues& values = function.at(chain.block).values;
    size_t indexOfLastCall = erased.lastCall + (values.size() - numberOfValuesBeforeInsertions);
    RELEASE_ASSERT(values[indexOfLastCall] == chain.lastCall);
    BitVector erasedIndexes = WTF::move(erased.otherValues);
    erasedIndexes.set(indexOfLastCall);
    builder.eraseValues(chain.block, erasedIndexes);
}

// An ArrayChain names its operands by id, so a chain that reads the result of another one is rewritten before that result is replaced.
static void sortConsumersFirst(FunctionPassContext& context, Vector<ArrayChain>& chains)
{
    const CFG& cfg = context.analyses.cfg();
    const UseLists& useLists = context.analyses.useLists();
    std::ranges::sort(chains, std::ranges::greater { }, [&](const ArrayChain& chain) {
        return std::pair { cfg.reversePostOrderIndex(chain.block), useLists.locationOf(chain.lastCall).index };
    });
}

// The traversal is library code.
static bool traversalStaysWithinSummary(const FunctionPassContext& context, FunctionID traversal)
{
    const EffectContext& effects = context.analyses.effectContext();
    return CalleeSummary(effects, traversal)->onCall().isSubsetOf(CalleeSummary(effects, context.id)->onCall());
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

static void declineAll(FunctionPassContext& context, const Vector<ArrayChain>& chains)
{
    for (const ArrayChain& chain : chains)
        context.addDecline(Wire::DeclineKind::ArrayChainNotFused, chain.lastCall, ArrayChainDeclineReason::Unknown);
}

PassOutcome fuseArrayChains(FunctionPassContext& context)
{
    if (context.function.role == FunctionRole::ChainStep || context.function.enclosingRuntimeFunction)
        return PassOutcome::Unchanged;
    FunctionID traversal = findChainTraversal(context.program);
    if (!traversal)
        return PassOutcome::Unchanged;

    Vector<ArrayChain> accepted;
    for (ArrayChain& chain : findArrayChains(context)) {
        ArrayChainDeclineReason obstacle = checkInterleaving(context, chain);
        if (obstacle == ArrayChainDeclineReason::None)
            accepted.append(WTF::move(chain));
        else
            context.addDecline(Wire::DeclineKind::ArrayChainNotFused, chain.lastCall, obstacle);
    }
    if (accepted.isEmpty())
        return PassOutcome::Unchanged;
    if (!traversalStaysWithinSummary(context, traversal)) {
        declineAll(context, accepted);
        return PassOutcome::Unchanged;
    }

    // A test in front of a chain may hold behind it only because the calls of the chain say how little they write. Whether it still holds shows on the fused body.
    sortConsumersFirst(context, accepted);
    Function fused = context.function;
    {
        FunctionAnalyses analyses(context.analyses.context(), fused);
        FunctionPassContext fusedContext { context.program, context.inputs, context.id, fused, analyses, context.types, context.strings, context.build };
        IRBuilder builder(analyses.context(), fused, context.types, &analyses);
        for (const ArrayChain& chain : accepted)
            rewriteArrayChain(fusedContext, builder, chain);
    }
    FunctionAnalyses analysesOfFused(context.analyses.context(), fused);
    if (endsGuardInUse(analysesOfFused)) {
        declineAll(context, accepted);
        return PassOutcome::Unchanged;
    }

    context.function = WTF::move(fused);
    context.build.statistics().count(StatisticID::ArrayChainsFused, accepted.size());
    // A chain is rewritten inside its block: the fused body has the blocks, the targets and the handlers of the body that it replaces.
    context.keptAnalyses = controlFlowAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
