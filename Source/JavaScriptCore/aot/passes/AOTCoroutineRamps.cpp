/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutinePlan.h"

#if ENABLE(AOT)

#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// A ramp takes over the id and the callers of its source, so what is known about those callers carries over.
static constexpr OptionSet<FunctionCapabilities> inheritedCapabilities {
    FunctionCapabilities::MayChangeSignature,
    FunctionCapabilities::HasOnlyDirectCallers,
    FunctionCapabilities::IsNeverCalledByHost,
};

static Function copySignature(const Function& unsplit)
{
    Function ramp;
    ramp.name = unsplit.name;
    ramp.module = unsplit.module;
    ramp.position = unsplit.position;
    ramp.text = unsplit.text;
    ramp.kind = unsplit.kind;
    ramp.flags = unsplit.flags;
    ramp.length = unsplit.length;
    ramp.enclosingRuntimeFunction = unsplit.enclosingRuntimeFunction;
    ramp.captures = unsplit.captures;
    ramp.thisType = unsplit.thisType;
    ramp.parameters = unsplit.parameters;
    ramp.results = unsplit.results;
    ramp.derivation = unsplit.derivation;
    ramp.constructedClass = unsplit.constructedClass;
    return ramp;
}

static Function createHeader(const Function& unsplit, const CoroutinePlan& plan)
{
    Function ramp = copySignature(unsplit);
    ramp.role = FunctionRole::CoroutineRamp;
    if (unsplit.runsOnlyBehindFailedTest())
        ramp.flags.add(FunctionFlags::IsSplitOffGenericVersion);
    ramp.creator = plan.record;
    ramp.capabilities = (initialCapabilitiesOf(ramp) - inheritedCapabilities) | (unsplit.capabilities & inheritedCapabilities);
    return ramp;
}

static Vector<ValueID, 8> addEntryParameters(IRBuilder& builder, BlockID entry, const Function& unsplit)
{
    TypingContext typing { builder.context().program, unsplit, builder.types() };
    unsigned numberOfEntryParameters = unsplit.numberOfEntryParameters();
    Vector<ValueID, 8> entryParameters;
    entryParameters.reserveInitialCapacity(numberOfEntryParameters);
    for (unsigned index = 0; index < numberOfEntryParameters; ++index)
        entryParameters.append(builder.addParameter(entry, declaredEntryTypeOf(typing, unsplit, index)));
    return entryParameters;
}

// A ramp is built before its body exists, so the result type of this call cannot come from the callee. A worker takes no driver.
static ValueID appendStart(IRBuilder& builder, FunctionID body, std::span<const ValueID> entryParameters, ValueID driver)
{
    Value call(Opcode::CallDirect);
    call.setFunction(body);
    call.setArgumentsOperands(entryParameters);
    if (driver)
        call.appendVariadicOperand(driver);
    return builder.appendWithType(WTF::move(call), TypeTable::anything);
}

static ValueID appendDriverAndStart(IRBuilder& builder, const CreatorRecord::Coroutine& record, const Function& unsplit, std::span<const ValueID> entryParameters)
{
    switch (record.kind) {
    case CoroutineKind::AsyncFunction: {
        // A body that throws before its first suspension returns a new rejected promise, not this one.
        ValueID promise = builder.append(Opcode::NewPromise, { });
        return appendStart(builder, record.body, entryParameters, promise);
    }
    case CoroutineKind::Generator:
    case CoroutineKind::AsyncGenerator: {
        std::optional<unsigned> calleeParameter = unsplit.calleeParameterIndex();
        RELEASE_ASSERT(calleeParameter);
        Opcode opcode = record.kind == CoroutineKind::Generator ? Opcode::NewGenerator : Opcode::NewAsyncGenerator;
        Value allocation = IRBuilder::make(opcode, { entryParameters[*calleeParameter] });
        allocation.setFunction(record.ramp);
        ValueID generator = builder.append(WTF::move(allocation));
        appendStart(builder, record.body, entryParameters, generator);
        return generator;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A ramp returns what it allocates, whatever is said of its result. What is said of the contents stays in the type, where contents are no proof.
static bool narrowDeclaredResult(const Program& program, TypeArena& types, DeclaredType& declared, TypeID allocatedType)
{
    bool isExpected = declared.flags.contains(PositionFlags::IsExpected);
    if (!isExpected && types.at(declared.type).isSubsetOf(types.at(allocatedType), program))
        return false;

    TypeID met = types.intersectionOf(allocatedType, declared.type);
    declared.type = met == TypeTable::never ? allocatedType : met;
    declared.flags.remove(PositionFlags::IsExpected);
    return true;
}

static void narrowResult(IRBuilder& builder, Function& ramp, TypeID allocatedType, CoroutinePlan& plan)
{
    RELEASE_ASSERT(ramp.results.size() == 1);
    plan.didNarrowResult = narrowDeclaredResult(builder.context().program, builder.types(), ramp.results[0], allocatedType);
}

static void emitReturnOfDriver(IRBuilder& builder, const Function& ramp, ValueID returned)
{
    emitReturnOfCall(builder, convertTo(builder, returned, DeclaredType { .type = ramp.results[0].type }));
}

Function buildCoroutineRamp(const AnalysisContext& context, TypeArena& types, const CoroutineTypes& coroutineTypes, const Function& unsplit, CoroutinePlan& plan)
{
    Function ramp = createHeader(unsplit, plan);

    IRBuilder builder(context, ramp, types);
    builder.setOrigin(Origin { ramp.position, 0 });
    BlockID entry = builder.addBlock();
    builder.moveToEndOf(entry);
    Vector<ValueID, 8> entryParameters = addEntryParameters(builder, entry, unsplit);
    narrowResult(builder, ramp, coroutineTypes.allocatedBy(plan.record.kind), plan);

    emitReturnOfDriver(builder, ramp, appendDriverAndStart(builder, plan.record, unsplit, entryParameters.span()));
    return ramp;
}

// The worker returns and throws as any function does. Here its result becomes the resolution of a new promise, and what it throws the reason of a rejected one.
Function buildRampOfWorker(const AnalysisContext& context, TypeArena& types, const CoroutineTypes& coroutineTypes, const Function& unsplit, CoroutinePlan& plan)
{
    RELEASE_ASSERT(plan.neverSuspends && plan.record.kind == CoroutineKind::AsyncFunction && plan.record.worker);
    Function ramp = createHeader(unsplit, plan);

    IRBuilder builder(context, ramp, types);
    builder.setOrigin(Origin { ramp.position, 0 });
    BlockID entry = builder.addBlock();
    Vector<ValueID, 8> entryParameters = addEntryParameters(builder, entry, unsplit);
    narrowResult(builder, ramp, coroutineTypes.allocatedBy(plan.record.kind), plan);

    BlockID rejection = builder.addHandlerBlock(BlockRole::CatchHandler, ValueID());
    builder.setHandler(Target { rejection, { }, { } });
    BlockID calling = builder.addBlock();
    builder.setHandler(std::nullopt);
    BlockID resolving = builder.addBlock();

    builder.moveToEndOf(entry);
    builder.emitJump(calling);

    builder.moveToEndOf(calling);
    ValueID result = appendStart(builder, plan.record.worker, entryParameters.span(), { });
    builder.emitJump(resolving);

    builder.moveToEndOf(resolving);
    ValueID promise = builder.append(Opcode::NewPromise, { });
    builder.append(Opcode::ResolvePromise, { promise, result });
    emitReturnOfDriver(builder, ramp, promise);

    builder.moveToEndOf(rejection);
    ValueID error = builder.append(Opcode::CatchValue, { });
    ValueID rejected = builder.append(Opcode::NewPromise, { });
    builder.append(Opcode::RejectPromise, { rejected, error });
    emitReturnOfDriver(builder, ramp, rejected);
    return ramp;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
