/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCoroutines.h"
#include "AOTEffectsCheck.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct AwaitedCall {
    ValueID call;
    ValueID await;
    BlockID block;
    uint32_t callIndex { 0 };
    uint32_t awaitIndex { 0 };
    FunctionID entry;
    Vector<ValueID, 1> casts;
};

struct SuccessorParts {
    FunctionID entry;
    ValueID promise;
    Vector<ValueID, 8> arguments;
    Origin callOrigin;
    AccessSiteID accessSite;
    TypeID prediction;
    bool isDemotedTailCall;
};

} // anonymous namespace

static FunctionID findEntryWithoutPromise(const Program& program, const Value& call)
{
    const Function& callee = program.at(call.function());
    const CreatorRecord::Coroutine* record = coroutineOf(callee);
    if (!record || callee.role != FunctionRole::CoroutineRamp)
        return { };
    return record->entryWithoutPromise;
}

static bool hasOnlyPureValues(FunctionPassContext& context, std::span<const ValueID> values)
{
    return std::ranges::all_of(values, [&](ValueID value) {
        return context.analyses.effectsOf(value).onAnyEdge().isPure();
    });
}

static std::optional<AwaitedCall> findAwaitedCall(FunctionPassContext& context, ValueID await)
{
    const Function& function = context.function;

    Vector<ValueID, 1> casts;
    ValueID awaited = function.at(await).awaitedOperand();
    for (; isCastWithoutTest(function.at(awaited).opcode); awaited = function.at(awaited).operand(0))
        casts.append(awaited);

    const Value& call = function.at(awaited);
    if (call.opcode != Opcode::CallDirect)
        return std::nullopt;
    FunctionID entry = findEntryWithoutPromise(context.program, call);
    if (!entry)
        return std::nullopt;

    const UseLists& useLists = context.analyses.useLists();
    auto hasSingleUse = [&](ValueID value) {
        return useLists.singleUse(value).has_value();
    };
    if (!hasSingleUse(awaited) || !std::ranges::all_of(casts, hasSingleUse))
        return std::nullopt;

    ValueLocation callLocation = useLists.locationOf(awaited);
    ValueLocation awaitLocation = useLists.locationOf(await);
    RELEASE_ASSERT(awaitLocation.placement == ValuePlacement::InBlock);
    if (callLocation.placement != ValuePlacement::InBlock || callLocation.block != awaitLocation.block || callLocation.index >= awaitLocation.index)
        return std::nullopt;

    const BlockValues& values = function.at(awaitLocation.block).values;
    if (!hasOnlyPureValues(context, values.subspan(callLocation.index + 1, awaitLocation.index - callLocation.index - 1)))
        return std::nullopt;
    return AwaitedCall { awaited, await, awaitLocation.block, callLocation.index, awaitLocation.index, entry, WTF::move(casts) };
}

static Vector<AwaitedCall> findAwaitedCalls(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<AwaitedCall> sites;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (function.at(value).opcode != Opcode::AwaitBySavingFrame)
                continue;
            if (std::optional<AwaitedCall> site = findAwaitedCall(context, value))
                sites.append(WTF::move(*site));
        }
    }
    return sites;
}

static SuccessorParts copySuccessorParts(const Function& function, const AwaitedCall& site)
{
    const Value& call = function.at(site.call);
    const Value& await = function.at(site.await);
    return {
        .entry = site.entry,
        .promise = await.promiseOperand(),
        .arguments = Vector<ValueID, 8>(call.argumentsOperands()),
        .callOrigin = call.origin(),
        .accessSite = call.accessSite(),
        .prediction = await.prediction(),
        .isDemotedTailCall = call.isDemotedTailCall(),
    };
}

static void becomeAwaitOfCall(Value& value, const SuccessorParts& parts)
{
    value.become(Opcode::AwaitCallBySavingFrame);
    value.setFunction(parts.entry);
    value.setPromiseOperand(parts.promise);
    value.setArgumentsOperands(parts.arguments.span());
    value.setOrigin(parts.callOrigin);
    value.setAccessSite(parts.accessSite);
    value.setPrediction(parts.prediction);
    value.setIsDemotedTailCall(parts.isDemotedTailCall);
}

static void markCallAndCasts(const Function& function, const AwaitedCall& site, BitVector& erasedIndexes)
{
    const BlockValues& values = function.at(site.block).values;
    for (uint32_t index = site.callIndex; index < site.awaitIndex; ++index) {
        if (values[index] == site.call || site.casts.contains(values[index]))
            erasedIndexes.set(index);
    }
}

static void eraseMarkedValues(IRBuilder& builder, BlockID block, BitVector& erasedIndexes)
{
    if (erasedIndexes.isEmpty())
        return;
    builder.eraseValues(block, erasedIndexes);
    erasedIndexes.clearAll();
}

static void expandTypedAwait(FunctionPassContext& context, IRBuilder& builder, const AwaitedCall& site, Value&& successor, const ReplacedEffects& replaced)
{
    Function& function = context.function;
    TypeID type = function.at(site.await).type;
    Origin callOrigin = successor.origin();

    IRBuilder::Expansion expansion = builder.beginExpansion(ProgramPoint { site.block, site.awaitIndex });
    Origin awaitOrigin = builder.origin();
    builder.setOrigin(callOrigin);
    ValueID result = builder.append(WTF::move(successor));
    checkReplacement(context.analyses.effectContext(), function, { context.id, site.block, result }, function.at(result), replaced, context.build.diagnostics());

    builder.setOrigin(awaitOrigin);
    builder.endExpansion(expansion, convertTo(builder, result, DeclaredType { .type = type }));
}

// Sites of one block are rewritten last position first: what is marked lies after this site, so this site's positions still hold.
static void rewrite(FunctionPassContext& context, IRBuilder& builder, const AwaitedCall& site, BitVector& erasedIndexes)
{
    Function& function = context.function;
    const EffectContext& effects = context.analyses.effectContext();

    ReplacedEffects replaced;
    replaced.add(effects, function, function.at(site.call));
    replaced.add(effects, function, function.at(site.await));

    SuccessorParts parts = copySuccessorParts(function, site);
    Value successor;
    becomeAwaitOfCall(successor, parts);
    const Type& successorType = context.types.at(resultTypeOf(context.program, function, successor, context.types));
    if (successorType.isSubsetOf(context.types.at(function.at(site.await).type), context.program)) {
        becomeAwaitOfCall(function.at(site.await), parts);
        checkReplacement(effects, function, { context.id, site.block, site.await }, function.at(site.await), replaced, context.build.diagnostics());
    } else {
        eraseMarkedValues(builder, site.block, erasedIndexes);
        expandTypedAwait(context, builder, site, WTF::move(successor), replaced);
    }
    markCallAndCasts(function, site, erasedIndexes);
}

PassOutcome resolveAwaitedCalls(FunctionPassContext& context)
{
    if (context.function.role != FunctionRole::CoroutineBody || coroutineOfBody(context.function).kind != CoroutineKind::AsyncFunction)
        return PassOutcome::Unchanged;

    Vector<AwaitedCall> sites = findAwaitedCalls(context);
    if (sites.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    BitVector erasedIndexes;
    for (size_t index = sites.size(); index--;) {
        const AwaitedCall& site = sites[index];
        rewrite(context, builder, site, erasedIndexes);
        if (!index || sites[index - 1].block != site.block)
            eraseMarkedValues(builder, site.block, erasedIndexes);
    }
    context.analyses.didChangeValues();

    context.build.statistics().count(StatisticID::AwaitedCallsResolved, sites.size());
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
