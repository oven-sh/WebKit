/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutinePlan.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
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

// A ramp is built before its body exists, so the result type of this call cannot come from the callee.
static ValueID appendStart(IRBuilder& builder, FunctionID body, std::span<const ValueID> entryParameters, ValueID driver)
{
    Value call(Opcode::CallDirect);
    call.setFunction(body);
    call.setArgumentsOperands(entryParameters);
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

static bool narrowDeclaredResult(const Program& program, const TypeArena& types, DeclaredType& declared, TypeID allocatedType)
{
    const Type& allocated = types.at(allocatedType);
    const Type& result = types.at(declared.type);
    if (!allocated.isSubsetOf(result, program) || result.isSubsetOf(allocated, program))
        return false;

    applyPosition(declared, FlowPositionUpdate { .type = allocatedType });
    if (declared.predicted && !types.at(declared.predicted).overlaps(allocated, program))
        declared.predicted = { };
    return true;
}

static void emitReturnOfDriver(IRBuilder& builder, Function& ramp, ValueID returned, TypeID allocatedType, CoroutinePlan& plan)
{
    RELEASE_ASSERT(ramp.results.size() == 1);
    DeclaredType& declared = ramp.results[0];
    plan.didNarrowResult = narrowDeclaredResult(builder.context().program, builder.types(), declared, allocatedType);
    emitReturnOfCall(builder, convertTo(builder, returned, DeclaredType { .type = declared.type }));
}

Function buildCoroutineRamp(const AnalysisContext& context, TypeArena& types, const CoroutineTypes& coroutineTypes, const Function& unsplit, CoroutinePlan& plan)
{
    Function ramp = copySignature(unsplit);
    ramp.role = FunctionRole::CoroutineRamp;
    ramp.creator = plan.record;
    ramp.capabilities = (initialCapabilitiesOf(ramp) - inheritedCapabilities) | (unsplit.capabilities & inheritedCapabilities);

    IRBuilder builder(context, ramp, types);
    builder.setOrigin(Origin { ramp.position, 0 });
    BlockID entry = builder.addBlock();
    builder.moveToEndOf(entry);

    TypingContext typing { context.program, unsplit, types };
    unsigned numberOfEntryParameters = unsplit.numberOfEntryParameters();
    Vector<ValueID, 8> entryParameters;
    entryParameters.reserveInitialCapacity(numberOfEntryParameters);
    for (unsigned index = 0; index < numberOfEntryParameters; ++index)
        entryParameters.append(builder.addParameter(entry, declaredEntryTypeOf(typing, unsplit, index)));

    ValueID returned = appendDriverAndStart(builder, plan.record, unsplit, entryParameters.span());
    emitReturnOfDriver(builder, ramp, returned, coroutineTypes.allocatedBy(plan.record.kind), plan);
    return ramp;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
