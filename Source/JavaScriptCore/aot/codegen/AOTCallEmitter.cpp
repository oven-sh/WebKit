/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTBuiltinCalls.h"
#include "AOTConvention.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTExceptionEmitter.h"
#include "AOTFlowResults.h"
#include "AOTFlowSinkEquations.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFrameMapEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTRuntimeValidationEmitter.h"
#include "AOTStubBodyConvention.h"
#include "AOTStubCatalog.h"
#include "AOTStubRegisters.h"
#include "AOTStubsCalls.h"
#include "AOTThreshold.h"
#include "AOTThrowEmitter.h"
#include "AOTVector.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "StackAlignment.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isPassed(const ArgumentLocation& location)
{
    return location.kind != ArgumentLocation::Kind::None;
}

static LType b3TypeOfResults(B3::Procedure& proc, std::span<const ArgumentLocation> results)
{
    Vector<B3::Type, 4> types;
    for (const ArgumentLocation& result : results) {
        if (isPassed(result))
            types.append(b3TypeOf(result.representation));
    }
    if (types.isEmpty())
        return B3::Void;
    if (types.size() == 1)
        return types[0];
    return proc.addTuple(Vector<B3::Type>(types));
}

static B3::ValueRep stackWordRep(size_t stackIndex)
{
    return B3::ValueRep::stackArgument(safeCast<B3::ValueRep::OffsetType>(stackIndex * sizeOfStackWord));
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
    return call->opcode == Opcode::CallMethod;
}

std::optional<ThrowSite> CallEmitter::siteThatNamesCallee(const ValueContext& value)
{
    if (!value->calleeName())
        return std::nullopt;
    return ThrowSite { ThrowKind::NotAFunction, value->calleeName().index(), std::nullopt, readsCalleeFromReceiver(value) };
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
    for (size_t i = 0; i < ids.size(); ++i) {
        RELEASE_ASSERT(isPassedAs(representationOf(ids[i]), locations.parameters[i].representation));
        operands.append(get(ids[i]));
    }
    return operands;
}

CallEmitter::PlacedArguments CallEmitter::placeConventionArguments(const ConventionCall& call)
{
    const StubSignature& signature = inputs().stubCatalog.summaryOf(call.stub).signature;
    RELEASE_ASSERT(signature.usesCallConvention && call.stubOperands.size() == signature.numberOfOperands);

    PlacedArguments placed;
    for (size_t i = 0; i < call.stubOperands.size(); ++i)
        placed.inRegisters.append(B3::ConstrainedValue(call.stubOperands[i], B3::ValueRep::reg(signature.operands[i])));
    if (call.callee)
        placed.inRegisters.append(B3::ConstrainedValue(call.callee, B3::ValueRep::reg(calleeGPR)));
    if (call.thisValue)
        placed.inRegisters.append(B3::ConstrainedValue(call.thisValue, B3::ValueRep::reg(thisGPR)));
    if (call.count)
        placed.inRegisters.append(B3::ConstrainedValue(call.count, B3::ValueRep::reg(countGPR)));
    for (size_t i = 0; i < call.arguments.size(); ++i) {
        if (i < numberOfArgumentGPRs)
            placed.inRegisters.append(B3::ConstrainedValue(call.arguments[i], B3::ValueRep::reg(argumentGPR(static_cast<unsigned>(i)))));
        else
            placed.onStack.append(call.arguments[i]);
    }
    placed.stackWordsToReserve = static_cast<uint32_t>(placed.onStack.size());
    return placed;
}

void CallEmitter::markArgumentsLetGoByCallee(B3::PatchpointValue* call, const PlacedArguments& placed)
{
    if (placed.letGoByCallee.isEmpty())
        return;
    BitVector children;
    for (unsigned index = 0; index < call->numChildren(); ++index) {
        if (placed.letGoByCallee.contains(call->child(index)))
            children.set(index);
    }
    call->setChildrenLetGoByCallee(WTF::move(children));
    m_proc.setReleasesDeadValues(true);
}

// StackmapValue::childrenLetGoByCallee().
void CallEmitter::emitClearOfReleasedRegisters(CCallHelpers& jit, const B3::StackmapGenerationParams& params, const RegisterSet& among)
{
    RegisterSet released = params.value()->registersToRelease();
    released.filter(among);
    released.exclude(params.usedRegisters());
    released.forEach([&](Reg reg) {
        jit.move(CCallHelpers::TrustedImm32(0), reg.gpr());
    });
}

B3::PatchpointValue* CallEmitter::emitCall(const OperationContext& context, const CallTarget& target, const PlacedArguments& placed, std::span<const ArgumentLocation> results, const CallOptions& options)
{
    RuntimeCallEmitter& calls = emitter<RuntimeCallEmitter>();
    const StubCatalog& catalog = inputs().stubCatalog;
    CallPatchpointRequest request {
        .resultType = b3TypeOfResults(m_proc, results),
        .earlyClobbered = RegisterSet::macroClobberedGPRs(),
        .lateClobbered = calls.registersClobberedByCalls(),
        .effects = Effects::unknownCode(),
    };
    switch (target.kind) {
    case TargetKind::Function:
        if (m_program.at(target.calledFunction).role == FunctionRole::StubBody)
            request.lateClobbered = StubBodyConvention::registersClobberedByCalls();
        break;
    case TargetKind::Address:
        break;
    case TargetKind::Stub:
        request.lateClobbered.merge(catalog.summaryOf(target.calledStub).clobbered);
        request.effects = catalog.effectsOf(target.calledStub);
        request.callsStub = true;
        break;
    }
    if (options.callerSuspendsAtReturn)
        request.lateClobbered.merge(calleeSavesClobberedAtSuspensions());

    std::optional<Integer> stopsBeforeCall;
    if (generator().validates() && request.effects.mayCollect && !calls.effectsOfCall(context, request.effects).mayCollect)
        stopsBeforeCall = emitter<RuntimeValidationEmitter>().countStopsForCollector();

    B3::PatchpointValue* patchpoint = calls.createCallPatchpoint(context, request);
    if (options.callerSuspendsAtReturn)
        patchpoint->setNeedsFrameRecord(true);
    for (const B3::ConstrainedValue& argument : placed.inRegisters)
        patchpoint->append(argument);
    for (size_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex])
            patchpoint->append(B3::ConstrainedValue(argument, stackWordRep(stackIndex)));
    }
    if (target.kind == TargetKind::Address)
        patchpoint->append(B3::ConstrainedValue(target.calledAddress, B3::ValueRep::reg(valueEntryGPR)));
    markArgumentsLetGoByCallee(patchpoint, placed);

    uint32_t stackWordsToReserve = placed.stackWordsToReserve;
    WTF::Vector<B3::ValueRep, 1> resultConstraints;
    for (const ArgumentLocation& result : results) {
        switch (result.kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
        case ArgumentLocation::Kind::FPR:
            resultConstraints.append(B3::ValueRep::reg(result.reg));
            break;
        case ArgumentLocation::Kind::Stack:
            resultConstraints.append(stackWordRep(result.stackIndex));
            stackWordsToReserve = std::max(stackWordsToReserve, result.stackIndex + 1);
            break;
        }
    }
    if (!resultConstraints.isEmpty())
        patchpoint->resultConstraints = WTF::move(resultConstraints);
    if (stackWordsToReserve)
        m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(stackWordsToReserve * sizeOfStackWord));

    RELEASE_ASSERT(!options.readBeforeCall || (target.kind == TargetKind::Stub && target.immediate));
    OptionalIndex firstReadSite;
    if (options.readBeforeCall)
        firstReadSite = { calls.addSite(context, { .origin = options.readBeforeCall }) };
    uint32_t site = calls.addSite(context, options);
    B3::Air::StackSlot* keptRegisterStorage = generator().checksKeptRegisters() ? calls.keptRegisterStorage() : nullptr;
    RegisterSet clobbered = request.lateClobbered;
    clobbered.merge(request.earlyClobbered);
    RELEASE_ASSERT(!options.throwSite || target.kind == TargetKind::Stub);
    Vector<uint32_t, 3> wordsOfThrowSite;
    if (options.throwSite)
        wordsOfThrowSite = throwSiteWords(*options.throwSite, Representation::Void, 0);
    patchpoint->setGenerator([records = &m_records, wordsOfThrowSite = WTF::move(wordsOfThrowSite), stubCalls = &m_records.stubCalls, kind = target.kind, calledFunction = target.calledFunction, entry = target.entry, calledStub = target.calledStub, immediate = target.immediate, expectation = target.expectation, passesCallee = target.passesCallee, site = OptionalIndex { site }, firstReadSite, keptRegisterStorage, clobbered, caller = m_id, value = context.valueID().index(), callerSuspendsAtReturn = options.callerSuspendsAtReturn](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        emitClearOfReleasedRegisters(jit, params, RegisterSet::allGPRs());
        if (callerSuspendsAtReturn)
            FrameMapEmitter::releaseSavedRegisters(jit, params);
        std::optional<KeptRegisterCheck> check;
        if (keptRegisterStorage)
            check = KeptRegisterCheck { clobbered, params.code().addressInFrame(keptRegisterStorage->offsetFromFP()), caller, value, true };
        const KeptRegisterCheck* keptRegisters = check ? &*check : nullptr;
        if (expectation) {
            // A stub that finds the callee stores it.
            CCallHelpers::Address callee(instanceGPR, AOT_INSTANCE_OFFSET(scratch.expectedCall.callee));
            if (passesCallee)
                jit.storePtr(calleeGPR, callee);
            else
                jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), callee);
            // In halves: a wide immediate in the code of an image may look like an address of the compiling process.
            auto storeWord = [&](uint64_t word, int32_t offset) {
                static_assert(std::endian::native == std::endian::little);
                jit.store32(CCallHelpers::TrustedImm32(static_cast<int32_t>(word)), CCallHelpers::Address(instanceGPR, offset));
                jit.store32(CCallHelpers::TrustedImm32(static_cast<int32_t>(word >> 32)), CCallHelpers::Address(instanceGPR, offset + static_cast<int32_t>(sizeof(uint32_t))));
            };
            storeWord(expectation->encodedEffects(), AOT_INSTANCE_OFFSET(scratch.expectedCall.effects));
            storeWord(expectation->encodedSite(), AOT_INSTANCE_OFFSET(scratch.expectedCall.site));
            storeWord(expectation->encodedBuiltins(), AOT_INSTANCE_OFFSET(scratch.expectedCall.builtins));
        }
        switch (kind) {
        case TargetKind::Function:
            stubCalls->callFunction(jit, calledFunction, entry, site, keptRegisters);
            return;
        case TargetKind::Address:
            stubCalls->callRegister(jit, valueEntryGPR, site, keptRegisters);
            return;
        case TargetKind::Stub:
            if (wordsOfThrowSite.isEmpty()) {
                if (immediate)
                    stubCalls->call(jit, calledStub, *immediate, site, keptRegisters, firstReadSite);
                else
                    stubCalls->call(jit, calledStub, site, keptRegisters);
            } else {
                CCallHelpers::Label begin = jit.label();
                uint32_t returnSite = immediate ? stubCalls->call(jit, calledStub, *immediate, site, keptRegisters, firstReadSite) : stubCalls->call(jit, calledStub, site, keptRegisters);
                records->units.pin(begin, jit.label());
                records->traps.callThrowSites.append({ returnSite, wordsOfThrowSite });
            }
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    calls.finishCallPatchpoint(context, patchpoint, site, request);
    if (options.hasLandingOfMisfit) {
        m_landingOfLastCall = emitter<ExceptionEmitter>().attachLandingOfMisfit(patchpoint, site);
        // Whoever comes to the landing has left there what it would have returned. No summary of a callee says so: only the landing reads it.
        m_heaps.decoratePatchpointWrite(&m_heaps.AOTInstance_misfit, patchpoint);
    }
    if (stopsBeforeCall) {
        RuntimeValidationEmitter& validation = emitter<RuntimeValidationEmitter>();
        switch (target.kind) {
        case TargetKind::Function:
            validation.checkCollectorDidNotStop(context, *stopsBeforeCall, CalledCode::Kind::Function, target.calledFunction.index());
            break;
        case TargetKind::Address:
            validation.checkCollectorDidNotStop(context, *stopsBeforeCall, CalledCode::Kind::Address, 0);
            break;
        case TargetKind::Stub:
            validation.checkCollectorDidNotStop(context, *stopsBeforeCall, CalledCode::Kind::Stub, static_cast<uint32_t>(target.calledStub.family()));
            break;
        }
    }
    return patchpoint;
}

Operand CallEmitter::resultOfCall(const ArgumentLocation& location, LValue value)
{
    if (!isPassed(location)) {
        RELEASE_ASSERT(location.representation == Representation::JSValue);
        return undefined();
    }
    return Operand(location.representation, value);
}

void CallEmitter::defineResultsOfCall(const ValueContext& value, std::span<const ArgumentLocation> results, B3::PatchpointValue* call)
{
    if (results.empty())
        return;
    if (results.size() == 1) {
        define(value, resultOfCall(results[0], call));
        return;
    }
    size_t numberPassed = std::ranges::count_if(results, isPassed);
    unsigned indexInTuple = 0;
    Vector<Operand, 4> operands;
    for (const ArgumentLocation& result : results) {
        LValue passed = nullptr;
        if (isPassed(result))
            passed = numberPassed == 1 ? call : m_out.extract(call, indexInTuple++);
        operands.append(resultOfCall(result, passed));
    }
    defineResults(value, operands.span());
}

Boxed CallEmitter::callByConvention(const OperationContext& context, const ConventionCall& call, const CallOptions& options)
{
    RELEASE_ASSERT(!options.pureFunction);
    const ArgumentLocation boxedResult { ArgumentLocation::Kind::GPR, GPRInfo::returnValueGPR, 0, Representation::JSValue };
    CallTarget target = CallTarget::stub(call.stub, options.immediate);
    if (generator().validates()) {
        target.expectation = expectationOf(context, call.builtin);
        target.passesCallee = call.callee;
    }
    return Boxed(emitCall(context, target, placeConventionArguments(call), { &boxedResult, 1 }, options));
}

static bool mayEnterCompiledCode(const Program&, const EffectDescription&, unsigned depth);

// A model says what a native function does and is never entered itself.
static bool modelMayEnterCompiledCode(const Program& program, const Function& model, unsigned depth)
{
    AOT_THRESHOLD(unsigned, maxDepth, 4);
    if (depth > maxDepth || model.blocks.isEmpty())
        return true;
    for (const Block& block : model.blocks) {
        for (ValueID value : block.values) {
            if (mayEnterCompiledCode(program, describeEffects(program, model, model.at(value)), depth))
                return true;
        }
        if (mayEnterCompiledCode(program, describeEffects(program, model, block.terminator), depth))
            return true;
    }
    return false;
}

static bool mayEnterCompiledCode(const Program& program, const EffectDescription& description, unsigned depth)
{
    auto mayBeEntered = [&](FunctionID id) {
        const Function& function = program.at(id);
        return function.role != FunctionRole::Model || modelMayEnterCompiledCode(program, function, depth + 1);
    };
    return description.callsUnknownCode || std::ranges::any_of(description.functions, mayBeEntered) || std::ranges::any_of(description.slowPathFunctions, mayBeEntered);
}

static bool changesOperands(const BuiltinTable& table, BuiltinID builtin)
{
    return std::ranges::any_of(table.overloadsOf(builtin), [](const BuiltinOverload& overload) {
        auto changes = [](const BuiltinParameter& parameter) {
            return parameter.uses.containsAny(mutatingUses);
        };
        return overload.isUndeclared() || (overload.receiver && changes(*overload.receiver)) || std::ranges::any_of(overload.parameters, changes);
    });
}

// The analysis has applied a built-in that escaped to everything that native code holds.
std::span<const BuiltinID> CallEmitter::builtinsCalledByNativeCode()
{
    if (!m_builtinsCalledByNativeCode) {
        m_builtinsCalledByNativeCode.emplace();
        const BuiltinTable& table = m_program.builtins();
        for (const BuiltinUseRecord& record : inputs().flow.builtinUses()) {
            BuiltinID builtin = table.canonicalID(BuiltinID(record.builtin));
            if (inputs().flow.usesOf(BuiltinID(record.builtin)).contains(BuiltinUse::Escaped) && changesOperands(table, builtin))
                m_builtinsCalledByNativeCode->append(builtin);
        }
    }
    return m_builtinsCalledByNativeCode->span();
}

NativeCallExpectation CallEmitter::expectationOf(const OperationContext& context, CalledBuiltin builtin)
{
    Effects effects = context.effects().onAnyEdge();
    NativeCallExpectation expectation;
    expectation.isKnown = true;
    expectation.mayThrow = effects.mayThrow;
    expectation.mayCollect = effects.mayCollect;
    EffectDescription description = context.valueID() ? describeEffects(m_program, m_function, m_function.at(context.valueID())) : describeEffects(m_program, m_function, m_function.at(context.block()).terminator);
    expectation.mayEnterCompiledCode = mayEnterCompiledCode(m_program, description, 0);
    expectation.matchesAnyOverload = builtin.matchesAnyOverload;
    expectation.writes = effects.writes.heaps();
    expectation.overload = builtin.overload;
    expectation.function = m_id;
    expectation.value = context.valueID();
    expectation.trustsNativeCode = !inputs().flow.mayRunUnseenCode();
    if (!builtin.overload) {
        if (context.valueID()) {
            const Value& call = m_function.at(context.valueID());
            const BuiltinTable& table = m_program.builtins();
            CalleeList callees = calleesOfCall(m_program, call);
            for (BuiltinID callee : callees.listed().builtins)
                expectation.name(table.canonicalID(callee));
            call.forEachOperand([&](ValueID operand, uint32_t) {
                for (BuiltinID handed : typeOf(operand).builtins) {
                    if (!table.overloadsOf(handed).empty())
                        expectation.name(table.canonicalID(handed));
                }
            });
            // The analysis never saw a call that a pass made out of an operation: it followed the operation by a rule of its own, and handed no operand to native code there.
            if (PayloadWord::accessSite(call.opcode) && !call.accessSite())
                expectation.namesMoreBuiltins = true;
            // The copy of a call that runs with what a check has turned away has lost the callees that the analysis found for it.
            if (Value::opcodesWithCallees.contains(call.opcode) && !call.callees())
                expectation.namesMoreBuiltins = true;
        }
        for (BuiltinID callee : builtinsCalledByNativeCode())
            expectation.name(callee);
    }
    return expectation;
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
    unsigned count = static_cast<unsigned>(call.arguments.size());
    Vector<LValue, 8> values;
    for (Boxed argument : call.arguments)
        values.append(argument.value());

    ConventionCall conventionCall { .callee = call.callee.value(), .thisValue = call.thisValue.value(), .arguments = values.span(), .builtin = call.builtin };
    LValue countOperand = nullptr;
    if (call.lentArguments) {
        RELEASE_ASSERT(count <= numberOfArgumentGPRs && !(call.lentArguments >> count));
        conventionCall.stub = callValueLending(count, call.lentArguments, positionOf(context, options));
    } else {
        chooseStubThatCallsValue(conventionCall, countOperand, call.calleeIsCell);
        conventionCall.stub = conventionCall.stub.withForm(static_cast<uint8_t>(positionOf(context, options)));
    }
    return callByConvention(context, conventionCall, options);
}

void CallEmitter::generateCall(const ValueContext& value)
{
    ValueID callee = value->calleeOperand();
    // The stub that calls a value does not know what the program calls it. What is no object is never called, so the place throws by itself and says the name.
    if (const Type& type = typeOf(callee); value->calleeName() && !type.hasObjects() && type.isJavaScriptValue() && !type.isNever()) {
        emitter<ThrowEmitter>().throwAt(value, *siteThatNamesCallee(value), get(callee));
        if (value.returnCall()) {
            m_out.unreachable();
            return;
        }
        define(value, undefined());
        return;
    }
    Vector<Boxed, 8> arguments = boxedOperands(value->argumentsOperands());
    emitter<AccessEmitter>().recordArrival(value, in<Boxed>(callee));
    ValueCall call { in<Boxed>(callee), thisOrUndefined(value->thisValueOperand()), arguments.span() };
    call.lentArguments = argumentsLentBy(m_function, value.value());
    call.calleeIsCell = typeOf(callee).isObject();
    if (value.returnCall()) {
        tailCallValue(value, call);
        return;
    }
    // The tail call of an inlined function. A stack trace leaves that function out once the callee is entered, as it does after a real tail call.
    if (value->isTailCall && mayForwardCallToReceiver(value))
        requireCallableBeforeTailCall(value, call.callee, call.thisValue);
    define(value, callValue(value, call, { .throwSite = siteThatNamesCallee(value), .callerSuspendsAtReturn = value.isAwaitedAtOnce() }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
