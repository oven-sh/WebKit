/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinCallEmitter.h"
#include "AOTCallPlan.h"
#include "AOTClosureWord.h"
#include "AOTClosuresLent.h"
#include "AOTConvention.h"
#include "AOTExceptionEmitter.h"
#include "AOTFrameMapEmitter.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubBodyConvention.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCalls.h"
#include "AOTStubsCoroutines.h"
#include "AOTThrowEmitter.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "StackAlignment.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isPassedInCall(const ArgumentLocation& location)
{
    return location.kind != ArgumentLocation::Kind::None;
}

static LType machineTypeOfResults(Machine::Procedure& proc, std::span<const ArgumentLocation> results)
{
    if (results.size() == 1) {
        if (!isPassedInCall(results[0]))
            return Machine::Void;
        return machineTypeOf(results[0].representation);
    }
    Vector<LType, 4> types;
    for (const ArgumentLocation& result : results) {
        if (isPassedInCall(result))
            types.append(machineTypeOf(result.representation));
    }
    if (types.isEmpty())
        return Machine::Void;
    if (types.size() == 1)
        return types[0];
    return proc.addTuple(types.span());
}

static Machine::ValueRep callStackWordRep(size_t stackIndex)
{
    return Machine::ValueRep::stackArgument(safeCast<Machine::ValueRep::OffsetType>(stackIndex * sizeOfStackWord));
}

bool CallEmitter::isUndefinedConstant(LValue value)
{
    return value->hasInt64() && value->asInt64() == static_cast<int64_t>(JSValue::ValueUndefined);
}

Boxed CallEmitter::thisOrUndefined(ValueID id)
{
    return id ? in<Boxed>(id) : undefined();
}

static bool readsCalleeFromReceiver(const ValueContext& call)
{
    if (PayloadWord::calleeKey(call->opcode))
        return !!call->calleeKey();
    return false;
}

bool CallEmitter::isProvenReceiverOf(BuiltinBoxedStub method, const Type& receiver)
{
    std::optional<JSTypeRange> accepted = cellTypesOfReceiversOf(method);
    if (!accepted || !receiver.isJavaScriptValue() || receiver.isNever())
        return false;
    if (receiver.hasOnlyPrimitives(Primitive::AnyString))
        return accepted->contains(StringType);
    const ClassPart* onlyClass = receiver.isObject() ? singleBuiltinClassOf(types(), receiver) : nullptr;
    if (!onlyClass)
        return false;
    const BuiltinClass& builtinClass = m_program.builtins().at(onlyClass->builtinClass);
    return builtinClass.instanceTest == BuiltinInstanceTest::JSTypeEquals && accepted->contains(builtinClass.type);
}

std::optional<ThrowSite> CallEmitter::siteThatNamesCallee(const ValueContext& value)
{
    StringID name = value->calleeName();
    if (!name)
        return std::nullopt;
    return ThrowSite {
        .kind = ThrowKind::NotAFunction,
        .a = m_mentions.add(NumberSpace::ShownString, name),
        .b = std::nullopt,
        .calleeIsReadFromReceiver = readsCalleeFromReceiver(value),
        .aIsMention = true,
    };
}

Vector<Boxed, 8> CallEmitter::boxedOperands(std::span<const ValueID> ids)
{
    Vector<Boxed, 8> operands;
    for (ValueID id : ids)
        operands.append(in<Boxed>(id));
    return operands;
}

Vector<Operand, 8> CallEmitter::operandsOf(std::span<const ValueID> ids, const ArgumentLocations& locations)
{
    RELEASE_ASSERT(ids.size() == locations.parameters.size());
    Vector<Operand, 8> operands;
    for (size_t i = 0; i < ids.size(); ++i)
        operands.append(isPassedInCall(locations.parameters[i]) ? generator().getIn(ids[i], locations.parameters[i].representation) : Operand { });
    return operands;
}

CallEmitter::PlacedArguments CallEmitter::placeConventionArguments(const ConventionCall& call)
{
    return placeConventionArguments(call, inputs().stubCatalog.summaryOf(call.stub).signature);
}

CallEmitter::PlacedArguments CallEmitter::placeConventionArguments(const ConventionCall& call, const StubSignature& signature)
{
    RELEASE_ASSERT(signature.usesCallConvention && call.stubOperands.size() == signature.numberOfOperands);

    PlacedArguments placed;
    for (size_t i = 0; i < call.stubOperands.size(); ++i)
        placed.inRegisters.append(Machine::ConstrainedValue(call.stubOperands[i], Machine::ValueRep::reg(signature.operands[i])));
    if (call.callee)
        placed.inRegisters.append(Machine::ConstrainedValue(call.callee, Machine::ValueRep::reg(calleeGPR)));
    if (call.thisValue)
        placed.inRegisters.append(Machine::ConstrainedValue(call.thisValue, Machine::ValueRep::reg(thisGPR)));
    for (size_t i = 0; i < call.arguments.size(); ++i) {
        if (i < numberOfArgumentGPRs)
            placed.inRegisters.append(Machine::ConstrainedValue(call.arguments[i], Machine::ValueRep::reg(argumentGPR(static_cast<unsigned>(i)))));
        else
            placed.onStack.append(call.arguments[i]);
    }
    placed.stackWordsToReserve = static_cast<uint32_t>(placed.onStack.size());
    return placed;
}

static void emitDelegatedCallMark(CCallHelpers& jit)
{
#if CPU(X86_64)
    for (uint8_t byte : headOfDelegationMark)
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(byte));
#endif
    jit.m_assembler.buffer().putInt(delegationMark);
}

Machine::PatchOperation* CallEmitter::emitCall(const OperationContext& context, const CallTarget& target, const PlacedArguments& placed, std::span<const ArgumentLocation> results, const CallOptions& options, const AnswersOfCalledStub* answersOfCalledStub)
{
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    static const RegisterSet registersClobberedByCalls = calls.registersClobberedByCalls();
    CallPatchpointRequest request {
        .resultType = machineTypeOfResults(m_proc, results),
        .earlyClobbered = RegisterSet::macroClobberedGPRs(),
        .lateClobbered = registersClobberedByCalls,
        .effects = Effects::unknownCode(),
    };
    const StubSummary* summaryOfCalledStub = nullptr;
    switch (target.kind) {
    case TargetKind::Function:
        if (conventions().savesRegistersOfCallers(target.calledUnit))
            request.lateClobbered = StubBodyConvention::registersClobberedByCalls();
        break;
    case TargetKind::Stub:
        if (answersOfCalledStub) {
            summaryOfCalledStub = answersOfCalledStub->summary;
            request.effects = answersOfCalledStub->effects;
        } else
            summaryOfCalledStub = &inputs().stubCatalog.summaryAndEffectsOf(target.calledStub, request.effects);
        request.lateClobbered.merge(summaryOfCalledStub->clobbered);
        break;
    case TargetKind::Address:
        break;
    }
    if (options.callerSuspendsAtReturn)
        request.lateClobbered.merge(calleeSavesClobberedAtSuspensions());

    Machine::PatchOperation* patchpoint = calls.createCallPatchpoint(context, request);
    if (options.callerSuspendsAtReturn)
        patchpoint->setNeedsFrameRecord(true);
    else if (summaryOfCalledStub && summaryOfCalledStub->signature.filledOutOfResultInFront && !target.immediate && !options.numberedImmediate) {
        patchpoint->setRegisterThatCalleeFillsOutOfResult(summaryOfCalledStub->signature.filledOutOfResultInFront);
        patchpoint->setRegisterThatCalleeMovesIntoResult(summaryOfCalledStub->signature.movedIntoResultInFront);
    }
    for (const Machine::ConstrainedValue& argument : placed.inRegisters)
        patchpoint->append(argument);
    if (target.kind == TargetKind::Address)
        patchpoint->append(Machine::ConstrainedValue(target.calledAddress, Machine::ValueRep::reg(valueEntryGPR)));
    for (size_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex])
            patchpoint->append(Machine::ConstrainedValue(argument, callStackWordRep(stackIndex)));
    }

    uint32_t stackWordsToReserve = placed.stackWordsToReserve;
    Vector<Machine::ValueRep, 2> resultConstraints;
    for (const ArgumentLocation& result : results) {
        switch (result.kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
        case ArgumentLocation::Kind::FPR:
            resultConstraints.append(Machine::ValueRep::reg(result.reg));
            break;
        case ArgumentLocation::Kind::Stack:
            resultConstraints.append(callStackWordRep(result.stackIndex));
            stackWordsToReserve = std::max(stackWordsToReserve, result.stackIndex + 1);
            break;
        }
    }
    if (!resultConstraints.isEmpty())
        patchpoint->resultConstraints = resultConstraints;
    if (stackWordsToReserve)
        m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(stackWordsToReserve * sizeOfStackWord));

    RELEASE_ASSERT(!target.immediate || !options.numberedImmediate);
    RELEASE_ASSERT(!options.numberedImmediate || target.kind == TargetKind::Stub);
    RELEASE_ASSERT(!options.readBeforeCall || (target.kind == TargetKind::Stub && (target.immediate || options.numberedImmediate)));
    OptionalIndex firstReadSite;
    if (options.readBeforeCall)
        firstReadSite = { calls.addSite(context, { .origin = options.readBeforeCall }) };
    uint32_t site = calls.addSite(context, options);

    RELEASE_ASSERT(!options.throwSite || target.kind != TargetKind::Function);
    std::span<const uint32_t> wordsOfThrowSite;
    uint8_t wordsThatAreMentions = 0;
    if (options.throwSite) {
        Vector<uint32_t, 3> words = throwSiteWords(*options.throwSite, Representation::Void, 0);
        wordsThatAreMentions = mentionsAmongThrowSiteWords(*options.throwSite, words.size());
        wordsOfThrowSite = m_proc.arena().copySpan<uint32_t>(words.span());
    }
    patchpoint->setGenerator([records = &m_records, wordsOfThrowSite, wordsThatAreMentions, kind = target.kind, calledUnit = target.calledUnit, entry = target.entry, calledStub = target.calledStub, immediate = target.immediate, numberedImmediate = options.numberedImmediate, site = OptionalIndex { site }, firstReadSite, callerSuspendsAtReturn = options.callerSuspendsAtReturn, callerDelegates = options.callerDelegates](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (callerSuspendsAtReturn)
            FrameMapEmitter::releaseSavedRegisters(jit, params);
        StubCalls& stubCalls = records->stubCalls;
        uint32_t returnSite = 0;
        switch (kind) {
        case TargetKind::Function:
            stubCalls.callFunction(jit, calledUnit, entry, site);
            return;
        case TargetKind::Stub:
            if (params.value()->leavesMoveOutOfResultToCallee()) [[unlikely]]
                stubCalls.enterNextStubInFront(params.value()->registerThatCalleeFillsOutOfResult(), params.value()->registerThatCalleeMovesIntoResult());
            returnSite = numberedImmediate ? stubCalls.call(jit, calledStub, *numberedImmediate, site, firstReadSite)
                : immediate ? stubCalls.call(jit, calledStub, *immediate, site, firstReadSite)
                : stubCalls.call(jit, calledStub, site);
            break;
        case TargetKind::Address:
            if (immediate)
                jit.move(CCallHelpers::TrustedImm32(*immediate), countGPR);
            returnSite = stubCalls.callRegister(jit, valueEntryGPR, site);
            break;
        }
        if (callerDelegates)
            emitDelegatedCallMark(jit);
        if (!wordsOfThrowSite.empty())
            records->traps.callThrowSites.append(CallThrowSite { returnSite, Vector<uint32_t, 3>(wordsOfThrowSite), wordsThatAreMentions });
    });
    calls.finishCallPatchpoint(context, patchpoint, site, request);
    return patchpoint;
}

Operand CallEmitter::resultOfCall(const ArgumentLocation& location, LValue value)
{
    if (!isPassedInCall(location)) {
        if (location.representation == Representation::Void)
            return { };
        RELEASE_ASSERT(location.representation == Representation::JSValue);
        return undefined();
    }
    return Operand(location.representation, value);
}

void CallEmitter::defineResultsOfCall(const ValueContext& value, std::span<const ArgumentLocation> results, Machine::PatchOperation* call)
{
    if (results.empty())
        return;
    if (results.size() == 1 && hasRegister(value.representation())) {
        define(value, resultOfCall(results[0], call));
        return;
    }
    size_t numberPassed = std::ranges::count_if(results, isPassedInCall);
    unsigned indexInTuple = 0;
    Vector<Operand, 4> operands;
    for (const ArgumentLocation& result : results) {
        LValue passed = nullptr;
        if (isPassedInCall(result))
            passed = numberPassed == 1 ? call : m_out.extract(call, indexInTuple++);
        operands.append(resultOfCall(result, passed));
    }
    defineResults(value, operands.span());
}

Boxed CallEmitter::callByConvention(const OperationContext& context, const ConventionCall& call, const CallOptions& options)
{
    RELEASE_ASSERT(!options.pureFunction);
    const ArgumentLocation boxedResult { ArgumentLocation::Kind::GPR, GPRInfo::returnValueGPR, 0, Representation::JSValue };
    AnswersOfCalledStub answers;
    answers.summary = &inputs().stubCatalog.summaryAndEffectsOf(call.stub, answers.effects);
    return Boxed(emitCall(context, CallTarget::stub(call.stub, options.immediate), placeConventionArguments(call, answers.summary->signature), { &boxedResult, 1 }, options, &answers));
}

Boxed CallEmitter::callAddressByConvention(const OperationContext& context, Pointer address, const ConventionCall& call, const CallOptions& options)
{
    const ArgumentLocation boxedResult { ArgumentLocation::Kind::GPR, GPRInfo::returnValueGPR, 0, Representation::JSValue };
    CallTarget target = CallTarget::address(address.value(), { });
    target.immediate = static_cast<uint32_t>(call.arguments.size());
    return Boxed(emitCall(context, target, placeConventionArguments(call, callConventionSignature({ })), { &boxedResult, 1 }, options));
}

void CallEmitter::chooseStubThatCallsValue(ConventionCall& call, LValue& countOperand, bool calleeIsCell)
{
    unsigned count = static_cast<unsigned>(call.arguments.size());
    if (count > numberOfArgumentGPRs) {
        countOperand = m_out.constInt64(count);
        call.stub = calleeIsCell ? callCellWithCount() : callValueWithCount();
        call.stubOperands = singleElementSpan(countOperand);
    } else if (isUndefinedConstant(call.thisValue)) {
        call.stub = calleeIsCell ? callValueCellWithoutThis(count) : callValueWithoutThis(count);
        call.thisValue = nullptr;
    } else
        call.stub = calleeIsCell ? callValueCell(count) : AOT::callValue(count);
}

Boxed CallEmitter::callValue(const OperationContext& context, const ValueCall& call, const CallOptions& options)
{
    Vector<LValue, 8> values;
    for (Boxed argument : call.arguments)
        values.append(argument.value());

    ConventionCall conventionCall { .callee = call.callee.value(), .thisValue = call.thisValue.value(), .arguments = values.span() };
    LValue countOperand = nullptr;
    const bool comesBack = positionOf(context) == CallPosition::Ordinary || context.origin().inlineFrame;
    if (call.methodMayBeUnread) {
        RELEASE_ASSERT(call.methodOfBuiltinClass && !call.lentArguments && comesBack);
        conventionCall.stub = call.receiverIsProvenOfBuiltinClass ? callMethodThatMayBeUnread(*call.methodOfBuiltinClass, static_cast<unsigned>(values.size())) : callMethodOfAnyReceiverThatMayBeUnread(*call.methodOfBuiltinClass, static_cast<unsigned>(values.size()));
    } else if (call.lentArguments) {
        RELEASE_ASSERT(values.size() <= numberOfArgumentGPRs && !(call.lentArguments >> values.size()));
        conventionCall.stub = callValueLending(static_cast<unsigned>(values.size()), call.lentArguments, positionOf(context));
    } else if (call.methodOfBuiltinClass && comesBack && servesCallsByName(*call.methodOfBuiltinClass, static_cast<unsigned>(values.size()))) {
        conventionCall.stub = call.receiverIsProvenOfBuiltinClass ? callMethodOfProvenBuiltinClassByValue(*call.methodOfBuiltinClass, static_cast<unsigned>(values.size())) : callMethodOfBuiltinClassByValue(*call.methodOfBuiltinClass, static_cast<unsigned>(values.size()));
    } else {
        chooseStubThatCallsValue(conventionCall, countOperand, call.calleeIsCell);
        conventionCall.stub = conventionCall.stub.withForm(static_cast<uint8_t>(positionOf(context)));
        if (isX86_64() && hasOneInstance() && positionOf(context) == CallPosition::Ordinary && values.size() <= numberOfArgumentGPRs && context.valueID() && context.block() && generator().usesLoopForms(context.block()))
            conventionCall.stub = callValueCopy(conventionCall.stub, (generator().code().numberOfOperationsInOrder() + m_numberOfCallsOfCopies++) % numberOfCallValueCopies);
    }
    return callByConvention(context, conventionCall, options);
}

void CallEmitter::defineResultOfCallByValue(const ValueContext& value, Boxed result)
{
    defineResultOfCallByValue(value, result, typeOf(value->calleeOperand()));
}

void CallEmitter::defineResultOfCallByValue(const ValueContext& value, Boxed result, const Type& callee)
{
    if (BuiltinID called = callee.isObject() ? singleBuiltinOf(types(), callee) : BuiltinID())
        emitter<BuiltinCallEmitter>().stopUnlessNativeResultIsAsWritten(value, result, called, constructionsByValue.contains(value->opcode));
    define(value, result);
}

Boxed CallEmitter::callDelegate(const ValueContext& value)
{
    ValueID callee = value->calleeOperand();
    Vector<Boxed, 8> arguments = boxedOperands(value->argumentsOperands());
    ValueCall call { in<Boxed>(callee), thisOrUndefined(value->thisValueOperand()), arguments.span() };
    call.calleeIsCell = typeOf(callee).isObject();
    return callValue(value, call, { .throwSite = siteThatNamesCallee(value), .callerDelegates = true });
}

void CallEmitter::generateCall(const ValueContext& value)
{
    if (emitter<AccessEmitter>().tryCallMethodByName(value))
        return;
    ValueID callee = value->calleeOperand();
    const Type type = typeOf(callee);
    if (value->calleeName() && !type.hasObjects() && type.isJavaScriptValue() && !type.isNever()) {
        emitter<ThrowEmitter>().throwAt(value, *siteThatNamesCallee(value), Operand(in<Boxed>(callee)));
        if (value.returnCall()) {
            m_out.unreachable();
            return;
        }
        define(value, undefined());
        return;
    }
    Vector<Boxed, 8> arguments = boxedOperands(value->argumentsOperands());
    ValueCall call { in<Boxed>(callee), thisOrUndefined(value->thisValueOperand()), arguments.span() };
    call.calleeIsCell = type.isObject();
    call.lentArguments = argumentsLentBy(m_code, value.value());
    if (value->methodOfReceiverAsMade() && value->thisValueOperand())
        call.methodOfBuiltinClass = m_program.builtins().boxedCallStubOf(value->methodOfReceiverAsMade());
    call.receiverIsProvenOfBuiltinClass = call.methodOfBuiltinClass && isProvenReceiverOf(*call.methodOfBuiltinClass, typeOf(value->thisValueOperand()));
    call.methodMayBeUnread = emitter<AccessEmitter>().methodMayBeUnread(callee);
    if (value.returnCall()) {
        tailCallValue(value, call);
        return;
    }
    if (mayForwardCallToReceiver(value.value()))
        requireForwardedCallCanBeMade(value, call.callee, call.thisValue, value->isTailCall);
    defineResultOfCallByValue(value, callValue(value, call, { .throwSite = siteThatNamesCallee(value), .callerSuspendsAtReturn = value.isAwaitedAtOnce() }), type);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
