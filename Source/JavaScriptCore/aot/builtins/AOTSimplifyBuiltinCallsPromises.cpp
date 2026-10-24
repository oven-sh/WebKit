/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTSplitUnions.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PromiseAllOfLiteral {
    ValueID call;
    ValueID literal;
};

} // anonymous namespace

static bool isPromiseStaticCall(FunctionPassContext& context, const Value& value, BuiltinID builtin)
{
    if (value.opcode != Opcode::CallBuiltin || value.builtinCallForm() != BuiltinCallForm::Whole)
        return false;
    if (context.program.builtins().at(value.overload()).builtin != builtin)
        return false;
    ValueID receiver = value.receiverOperand();
    return receiver && singleBuiltinOf(context.program.typeOf(context.function, receiver)) == Builtins::Promise;
}

static std::optional<PromiseAllOfLiteral> matchPromiseAllOfLiteral(FunctionPassContext& context, ValueID callID)
{
    const Function& function = context.function;
    const Value& call = function.at(callID);
    if (!isPromiseStaticCall(context, call, Builtins::PromiseAll))
        return std::nullopt;

    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() != 1)
        return std::nullopt;

    std::optional<ValueID> literalID = findSingleUseDefinition(context, arguments[0], callID);
    if (!literalID)
        return std::nullopt;

    const Value& literal = function.at(*literalID);
    if (literal.opcode != Opcode::NewArray || !literal.holes().empty() || literal.capacityOperand())
        return std::nullopt;
    if (!context.analyses.useLists().singleUse(*literalID))
        return std::nullopt;
    return PromiseAllOfLiteral { callID, *literalID };
}

static TypeID fulfillmentTypeOf(const Program& program, const Type& promise)
{
    const ClassPart* part = promise.findClassPart(BuiltinClasses::Promise);
    return part ? part->contentsAt(program.builtins(), ContentsSlot::Value) : TypeTable::anything;
}

unsigned simplifyPromiseAllOfLiterals(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    const Function& function = context.function;
    Vector<PromiseAllOfLiteral> promiseCalls;
    for (const ValueSite& site : calls) {
        if (std::optional<PromiseAllOfLiteral> promiseCall = matchPromiseAllOfLiteral(context, site.value))
            promiseCalls.append(*promiseCall);
    }

    unsigned rewrites = 0;
    for (const PromiseAllOfLiteral& promiseCall : promiseCalls) {
        std::span<const ValueID> elements = function.at(promiseCall.literal).elementsOperands();
        Value rewritten = IRBuilder::make(Opcode::PromiseAllOfElements);
        rewritten.setContents(fulfillmentTypeOf(context.program, context.program.typeOf(function, promiseCall.call)));
        rewritten.setElementsOperands(elements);
        if (!keepsTypeOf(context, promiseCall.call, rewritten))
            continue;
        if (context.exceeds(LimitID::MaxArrayLiteralBatch, elements.size(), promiseCall.call))
            continue;
        replaceKeepingType(context, promiseCall.call, WTF::move(rewritten));
        ++rewrites;
    }
    if (rewrites)
        context.analyses.didChangeValues();
    return rewrites;
}

static Value makeNewPromise(const Value& call)
{
    Value promise = IRBuilder::make(Opcode::NewPromise);
    promise.setAllocationSite(call.allocationSite());
    return promise;
}

static bool canResolveNewPromise(FunctionPassContext& context, ValueID callID)
{
    const Value& call = context.function.at(callID);
    if (!isPromiseStaticCall(context, call, Builtins::PromiseResolve) || call.argumentsOperands().size() > 1)
        return false;
    if (freshnessOf({ context.program, context.inputs.effects }, context.function, call) != ResultFreshness::Fresh)
        return false;
    return keepsTypeOf(context, callID, makeNewPromise(call));
}

static ValueID insertResolvedPromise(FunctionPassContext& context, IRBuilder& builder, const ValueSite& site)
{
    const Value& call = context.function.at(site.value);
    ReplacedEffects replaced;
    replaced.add({ context.program, context.inputs.effects }, context.function, call);

    std::span<const ValueID> arguments = call.argumentsOperands();
    ValueID resolution = arguments.empty() ? ValueID() : arguments[0];
    Value newPromise = makeNewPromise(call);
    builder.setOrigin(call.origin());

    ValueID promise = builder.insertBefore(site.point(), WTF::move(newPromise));
    if (!resolution)
        resolution = builder.insertBefore(site.point(), IRBuilder::make(Opcode::Undefined));
    ValueID resolve = builder.insertBefore(site.point(), IRBuilder::make(Opcode::ResolvePromise, { promise, resolution }));

    checkRewrite(context, ValueSite { site.block, site.index, promise }, replaced);
    checkRewrite(context, ValueSite { site.block, site.index, resolve }, replaced);
    return promise;
}

unsigned simplifyPromiseResolve(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    Function& function = context.function;
    Vector<ValueSite> sites;
    for (const ValueSite& site : calls) {
        if (canResolveNewPromise(context, site.value))
            sites.append(site);
    }
    if (sites.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    Vector<ValueID> promises;
    promises.reserveInitialCapacity(sites.size());
    for (const ValueSite& site : sites)
        promises.append(insertResolvedPromise(context, builder, site));
    builder.applyInsertions();

    for (size_t index = 0; index < sites.size(); ++index) {
        ValueID call = sites[index].value;
        Value upcast = IRBuilder::make(Opcode::Upcast, { promises[index] });
        upcast.setCastType(function.at(call).type);
        replaceKeepingType(context, call, WTF::move(upcast));
    }
    context.analyses.didChangeValues();
    return static_cast<unsigned>(sites.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
