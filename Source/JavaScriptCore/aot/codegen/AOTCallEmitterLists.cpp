/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTArgumentsEmitter.h"
#include "AOTConvention.h"
#include "AOTConversionEmitter.h"
#include "AOTElementEmitterAllocation.h"
#include "AOTEntryEmitter.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

void CallEmitter::generateCallList(const ValueContext& value)
{
    Boxed callee = in<Boxed>(value->calleeOperand());
    Boxed thisValue = thisOrUndefined(value->thisValueOperand());
    ArgumentsEmitter& ownArguments = emitter<ArgumentsEmitter>();
    if (std::optional<uint32_t> firstArgument = ownArguments.findForwardedRest(value->listOperand())) {
        RELEASE_ASSERT(!value.returnCall() && positionOf(value) == CallPosition::Ordinary);
        build().statistics().count(StatisticID::RestArraysForwarded);
        if (mayForwardCallToReceiver(value.value()))
            requireForwardedCallCanBeMade(value, callee, thisValue, false);
        LValue operands[] = { ownArguments.restList(*firstArgument).value(), m_out.zeroExtPtr(ownArguments.restLength(*firstArgument).value()) };
        ConventionCall forwarded { .stub = callWithList(), .stubOperands = operands, .callee = callee.value(), .thisValue = thisValue.value() };
        defineResultOfCallByValue(value, callByConvention(value, forwarded, { .throwSite = siteThatNamesCallee(value) }));
        return;
    }
    if (const Operation& literal = operationAt(value->listOperand()); literal.opcode == Opcode::NewArray && ArrayAllocationEmitter(generator()).isLeftToListCall(value.block(), value->listOperand(), literal)) {
        callWithLiteralAndList(value, callee, thisValue, literal);
        return;
    }
    LValue list = in<CellPointer>(value->listOperand()).value();
    ConventionCall call {
        .stub = callWithArray(positionOf(value)),
        .stubOperands = singleElementSpan(list),
        .callee = callee.value(),
        .thisValue = thisValue.value(),
    };
    if (!value.returnCall()) {
        if (mayForwardCallToReceiver(value.value()))
            requireForwardedCallCanBeMade(value, callee, thisValue, value->isTailCall);
        defineResultOfCallByValue(value, callByConvention(value, call, { .throwSite = siteThatNamesCallee(value) }));
        return;
    }

    requireCallableBeforeTailCall(value, callee, thisValue, boxCell(in<CellPointer>(value->listOperand())));
    tailCallByConvention(value, call);
}

void CallEmitter::callWithLiteralAndList(const ValueContext& value, Boxed callee, Boxed thisValue, const Operation& literal)
{
    ValueID append = value->previous;
    while (operationAt(append).opcode != Opcode::AppendElements && operationAt(append).opcode != Opcode::AppendIterable)
        append = operationAt(append).previous;
    Vector<LValue, 4> words;
    for (ValueID element : literal.elementsOperands())
        words.append(emitter<ConversionEmitter>().box(value, get(element), typeOf(element)).value());
    LValue list = operationAt(append).opcode == Opcode::AppendElements ? in<CellPointer>(operationAt(append).sourceOperand()).value() : generator().wordOf(value->listOperand());
    ConventionCall call {
        .stub = callWithWordsAndArray(words.size(), positionOf(value)),
        .stubOperands = singleElementSpan(list),
        .callee = callee.value(),
        .thisValue = thisValue.value(),
        .arguments = words.span(),
    };
    if (mayForwardCallToReceiver(value.value()))
        requireForwardedCallCanBeMade(value, callee, thisValue, value->isTailCall);
    build().statistics().count(StatisticID::ListCallsWithoutLists);
    defineResultOfCallByValue(value, callByConvention(value, call, { .throwSite = siteThatNamesCallee(value) }));
}

Boxed CallEmitter::constructValue(const OperationContext& context, ConstructSyntax syntax, Boxed callee, Boxed newTarget, std::span<const ValueID> argumentsAfterNewTarget, std::optional<ThrowSite> siteThatNamesCallee)
{
    Vector<LValue, 8> arguments { newTarget.value() };
    for (ValueID argument : argumentsAfterNewTarget)
        arguments.append(in<Boxed>(argument).value());
    LValue count = m_out.constInt64(static_cast<int64_t>(arguments.size()));

    ConventionCall call {
        .stub = AOT::constructValue(syntax),
        .stubOperands = singleElementSpan(count),
        .callee = callee.value(),
        .thisValue = undefined().value(),
        .arguments = arguments.span(),
    };
    return callByConvention(context, call, { .throwSite = siteThatNamesCallee, .constructs = true });
}

Boxed CallEmitter::constructWithArray(const OperationContext& context, ConstructSyntax syntax, Boxed callee, Boxed newTarget, CellPointer list)
{
    return Boxed(callStub(context, AOT::constructWithArray(syntax), { callee.value(), newTarget.value(), list.value() }, { .constructs = true }).first);
}

void CallEmitter::generateConstruct(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    Boxed callee = in<Boxed>(value->calleeOperand());

    Type typeOfCallee = typeOf(value->calleeOperand());
    FunctionID constructor = typeOfCallee.isObject() && !value->newTargetOperand() ? singleFunctionOf(types(), typeOfCallee) : FunctionID();
    CodeUnit entry { constructor, CodeRole::AllocatingConstructEntry };
    bool unitTestsStackInOwnFrame = m_code.holdsCallThatPlanHasNotCounted() || conventions().stackCheckOf(m_unit) == StackCheck::Required;
    if (constructor && unitTestsStackInOwnFrame && conventions().exists(entry)) {
        Vector<LValue, 8> arguments { callee.value() };
        for (ValueID argument : value->argumentsOperands())
            arguments.append(in<Boxed>(argument).value());
        size_t numberOfArguments = arguments.size();
        size_t numberOfParameters = describeEntry(inputs(), entry).numberOfParameters;
        while (arguments.size() < numberOfParameters)
            arguments.append(undefined().value());

        PlacedArguments placed;
        placed.inRegisters.append(Machine::ConstrainedValue(callee.value(), Machine::ValueRep::reg(calleeGPR)));
        placed.inRegisters.append(Machine::ConstrainedValue(undefined().value(), Machine::ValueRep::reg(thisGPR)));
        placed.inRegisters.append(Machine::ConstrainedValue(m_out.constInt64(static_cast<int64_t>(numberOfArguments)), Machine::ValueRep::reg(countGPR)));
        for (size_t index = 0; index < arguments.size(); ++index) {
            if (index < numberOfArgumentGPRs)
                placed.inRegisters.append(Machine::ConstrainedValue(arguments[index], Machine::ValueRep::reg(argumentGPR(static_cast<unsigned>(index)))));
            else
                placed.onStack.append(arguments[index]);
        }
        placed.stackWordsToReserve = static_cast<uint32_t>(placed.onStack.size());
        const ArgumentLocation boxedResult { ArgumentLocation::Kind::GPR, GPRInfo::returnValueGPR, 0, Representation::JSValue };
        define(value, Boxed(emitCall(value, CallTarget::unit(entry, primaryEntry), placed, { &boxedResult, 1 }, { .constructs = true })));
        return;
    }

    Boxed newTarget = inOptional<Boxed>(value->newTargetOperand()).value_or(callee);
    defineResultOfCallByValue(value, constructValue(value, ConstructSyntax::NewExpression, callee, newTarget, value->argumentsOperands(), siteThatNamesCallee(value)));
}

void CallEmitter::generateConstructList(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    Boxed callee = in<Boxed>(value->calleeOperand());
    Boxed newTarget = inOptional<Boxed>(value->newTargetOperand()).value_or(callee);
    defineResultOfCallByValue(value, constructWithArray(value, ConstructSyntax::NewExpression, callee, newTarget, in<CellPointer>(value->listOperand())));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
