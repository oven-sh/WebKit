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
#include "AOTConvention.h"
#include "AOTExceptionEmitter.h"
#include "AOTFrameMapEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubBodyConvention.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCalls.h"
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

// A constant has no form of its own: it is made in the form in which the callee takes the parameter.
// A word that says nothing is handed over all the same, as zero: ControlEmitter::generateReturn().
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
    const StubSignature& signature = inputs().stubCatalog.summaryOf(call.stub).signature;
    RELEASE_ASSERT(signature.usesCallConvention && call.stubOperands.size() == signature.numberOfOperands);

    PlacedArguments placed;
    for (size_t i = 0; i < call.stubOperands.size(); ++i)
        placed.inRegisters.append(Machine::ConstrainedValue(call.stubOperands[i], Machine::ValueRep::reg(signature.operands[i])));
    if (call.callee)
        placed.inRegisters.append(Machine::ConstrainedValue(call.callee, Machine::ValueRep::reg(calleeGPR)));
    if (call.thisValue)
        placed.inRegisters.append(Machine::ConstrainedValue(call.thisValue, Machine::ValueRep::reg(thisGPR)));
    if (call.count)
        placed.inRegisters.append(Machine::ConstrainedValue(call.count, Machine::ValueRep::reg(countGPR)));
    for (size_t i = 0; i < call.arguments.size(); ++i) {
        if (i < numberOfArgumentGPRs)
            placed.inRegisters.append(Machine::ConstrainedValue(call.arguments[i], Machine::ValueRep::reg(argumentGPR(static_cast<unsigned>(i)))));
        else
            placed.onStack.append(call.arguments[i]);
    }
    placed.stackWordsToReserve = static_cast<uint32_t>(placed.onStack.size());
    return placed;
}

void CallEmitter::emitClearOfReleasedRegisters(CCallHelpers& jit, const Machine::PatchParameters& params, const RegisterSet& among)
{
    RegisterSet released = params.value()->registersToRelease();
    released.filter(among);
    released.exclude(params.usedRegisters());
    released.forEach([&](Reg reg) {
        jit.move(CCallHelpers::TrustedImm32(0), reg.gpr());
    });
}

Machine::PatchOperation* CallEmitter::emitCall(const OperationContext& context, const CallTarget& target, const PlacedArguments& placed, std::span<const ArgumentLocation> results, const CallOptions& options)
{
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    const StubCatalog& catalog = inputs().stubCatalog;
    CallPatchpointRequest request {
        .resultType = machineTypeOfResults(m_proc, results),
        .earlyClobbered = RegisterSet::macroClobberedGPRs(),
        .lateClobbered = calls.registersClobberedByCalls(),
        .effects = Effects::unknownCode(),
    };
    switch (target.kind) {
    case TargetKind::Function:
        if (conventions().savesRegistersOfCallers(target.calledUnit))
            request.lateClobbered = StubBodyConvention::registersClobberedByCalls();
        break;
    case TargetKind::Stub:
        request.lateClobbered.merge(catalog.summaryOf(target.calledStub).clobbered);
        request.effects = catalog.effectsOf(target.calledStub);
        break;
    case TargetKind::Address:
        break;
    }
    if (options.callerSuspendsAtReturn)
        request.lateClobbered.merge(calleeSavesClobberedAtSuspensions());

    Machine::PatchOperation* patchpoint = calls.createCallPatchpoint(context, request);
    if (options.callerSuspendsAtReturn)
        patchpoint->setNeedsFrameRecord(true);
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

    RELEASE_ASSERT(!options.throwSite || target.kind == TargetKind::Stub);
    std::span<const uint32_t> wordsOfThrowSite;
    uint8_t wordsThatAreMentions = 0;
    if (options.throwSite) {
        Vector<uint32_t, 3> words = throwSiteWords(*options.throwSite, Representation::Void, 0);
        wordsThatAreMentions = mentionsAmongThrowSiteWords(*options.throwSite, words.size());
        wordsOfThrowSite = m_proc.arena().copySpan<uint32_t>(words.span());
    }
    patchpoint->setGenerator([records = &m_records, wordsOfThrowSite, wordsThatAreMentions, kind = target.kind, calledUnit = target.calledUnit, entry = target.entry, calledStub = target.calledStub, immediate = target.immediate, numberedImmediate = options.numberedImmediate, site = OptionalIndex { site }, firstReadSite, callerSuspendsAtReturn = options.callerSuspendsAtReturn](CCallHelpers& jit, const Machine::PatchParameters& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        emitClearOfReleasedRegisters(jit, params, RegisterSet::allGPRs());
        if (callerSuspendsAtReturn)
            FrameMapEmitter::releaseSavedRegisters(jit, params);
        StubCalls& stubCalls = records->stubCalls;
        switch (kind) {
        case TargetKind::Function:
            stubCalls.callFunction(jit, calledUnit, entry, site);
            return;
        case TargetKind::Stub: {
            uint32_t returnSite = numberedImmediate ? stubCalls.call(jit, calledStub, *numberedImmediate, site, firstReadSite)
                : immediate ? stubCalls.call(jit, calledStub, *immediate, site, firstReadSite)
                : stubCalls.call(jit, calledStub, site);
            if (!wordsOfThrowSite.empty())
                records->traps.callThrowSites.append(CallThrowSite { returnSite, Vector<uint32_t, 3>(wordsOfThrowSite), wordsThatAreMentions });
            return;
        }
        case TargetKind::Address:
            stubCalls.callRegister(jit, valueEntryGPR, site);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    calls.finishCallPatchpoint(context, patchpoint, site, request);
    return patchpoint;
}

Operand CallEmitter::resultOfCall(const ArgumentLocation& location, LValue value)
{
    if (!isPassedInCall(location)) {
        // The callee does not hand the result back, and no ResultAt asks for it. Whoever returns the results of the call as they came has no place for it either.
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
    // A callee that hands back one field in place of its object has one result too. The record of its call holds none of its results: ResultAt reads the field.
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
    return Boxed(emitCall(context, CallTarget::stub(call.stub, options.immediate), placeConventionArguments(call), { &boxedResult, 1 }, options));
}

// countOperand lives as long as the call does.
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
    if (call.lentArguments) {
        RELEASE_ASSERT(values.size() <= numberOfArgumentGPRs && !(call.lentArguments >> values.size()));
        conventionCall.stub = callValueLending(static_cast<unsigned>(values.size()), call.lentArguments, positionOf(context, options));
    } else if (call.methodOfBuiltinClass && positionOf(context, options) == CallPosition::Ordinary && servesCallsByName(*call.methodOfBuiltinClass, static_cast<unsigned>(values.size()))) {
        conventionCall.stub = callMethodOfBuiltinClassByValue(*call.methodOfBuiltinClass, static_cast<unsigned>(values.size()));
    } else {
        chooseStubThatCallsValue(conventionCall, countOperand, call.calleeIsCell);
        conventionCall.stub = conventionCall.stub.withForm(static_cast<uint8_t>(positionOf(context, options)));
    }
    return callByConvention(context, conventionCall, options);
}

// A cell is boxed as it is, so the record between the closure and the call moves nothing.
static uint8_t argumentsLentBy(const Code& code, const Operation& call)
{
    uint8_t lentArguments = 0;
    std::span<const ValueID> arguments = call.argumentsOperands();
    for (unsigned index = 0; index < arguments.size() && index < ClosureWord::numberOfBorrowedArguments; ++index) {
        const Operation* made = &code.at(arguments[index]);
        if (made->opcode == Opcode::Box)
            made = &code.at(made->operand(0));
        if (made->opcode == Opcode::NewClosure && made->isLent())
            lentArguments |= 1u << index;
    }
    return lentArguments;
}

void CallEmitter::defineResultOfCallByValue(const ValueContext& value, Boxed result)
{
    Type callee = typeOf(value->calleeOperand());
    if (BuiltinID called = callee.isObject() ? singleBuiltinOf(types(), callee) : BuiltinID())
        emitter<BuiltinCallEmitter>().stopUnlessNativeResultIsAsWritten(value, result, called, constructionsByValue.contains(value->opcode));
    define(value, result);
}

void CallEmitter::generateCall(const ValueContext& value)
{
    if (emitter<AccessEmitter>().tryCallMethodByName(value))
        return;
    ValueID callee = value->calleeOperand();
    // The stub that calls a value does not know what the program calls it. What is no object is never called, so the place throws by itself and says the name.
    if (const Type& type = typeOf(callee); value->calleeName() && !type.hasObjects() && type.isJavaScriptValue() && !type.isNever()) {
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
    call.calleeIsCell = typeOf(callee).isObject();
    call.lentArguments = argumentsLentBy(m_code, value.value());
    if (value->methodOfReceiverAsMade() && value->thisValueOperand())
        call.methodOfBuiltinClass = m_program.builtins().boxedCallStubOf(value->methodOfReceiverAsMade());
    if (value.returnCall()) {
        tailCallValue(value, call);
        return;
    }
    // The tail call of an inlined function. A stack trace leaves that function out once the callee is entered, as it does after a real tail call.
    if (value->isTailCall && mayForwardCallToReceiver(value))
        requireCallableBeforeTailCall(value, call.callee, call.thisValue);
    defineResultOfCallByValue(value, callValue(value, call, { .throwSite = siteThatNamesCallee(value), .callerSuspendsAtReturn = value.isAwaitedAtOnce() }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
