/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFlowResults.h"
#include "AOTFlowSinkEquations.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubBodyConvention.h"
#include "AOTStubCatalog.h"
#include "AOTStubsCalls.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeTestEmitter.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "StackAlignment.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

static bool isPassed(const ArgumentLocation& location)
{
    return location.kind != ArgumentLocation::Kind::None;
}

static LType b3TypeOfResults(Procedure& proc, std::span<const ArgumentLocation> results)
{
    Vector<B3::Type, 4> types;
    for (const ArgumentLocation& result : results) {
        if (isPassed(result))
            types.append(b3TypeOf(result.representation));
    }
    if (types.isEmpty())
        return Void;
    if (types.size() == 1)
        return types[0];
    return proc.addTuple(Vector<B3::Type>(types));
}

static ValueRep stackWordRep(size_t stackIndex)
{
    return ValueRep::stackArgument(safeCast<ValueRep::OffsetType>(stackIndex * sizeOfStackWord));
}

CallEmitter::CallTarget CallEmitter::CallTarget::function(FunctionID calledFunction, uint16_t entry)
{
    CallTarget target;
    target.kind = TargetKind::Function;
    target.calledFunction = calledFunction;
    target.entry = entry;
    return target;
}

CallEmitter::CallTarget CallEmitter::CallTarget::address(LValue calledAddress)
{
    CallTarget target;
    target.kind = TargetKind::Address;
    target.calledAddress = calledAddress;
    return target;
}

CallEmitter::CallTarget CallEmitter::CallTarget::stub(StubKey calledStub, std::optional<uint32_t> immediate)
{
    CallTarget target;
    target.kind = TargetKind::Stub;
    target.calledStub = calledStub;
    target.immediate = immediate;
    return target;
}

CallEmitter::CallEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

bool CallEmitter::isUndefinedConstant(LValue value)
{
    return value->hasInt64() && value->asInt64() == static_cast<int64_t>(JSValue::ValueUndefined);
}

Boxed CallEmitter::thisOrUndefined(ValueID id)
{
    return id ? in<Boxed>(id) : undefined();
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
        RELEASE_ASSERT(representationOf(ids[i]) == locations.parameters[i].representation);
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
        placed.inRegisters.append(ConstrainedValue(call.stubOperands[i], ValueRep::reg(signature.operands[i])));
    if (call.callee)
        placed.inRegisters.append(ConstrainedValue(call.callee, ValueRep::reg(calleeGPR)));
    if (call.thisValue)
        placed.inRegisters.append(ConstrainedValue(call.thisValue, ValueRep::reg(thisGPR)));
    if (call.count)
        placed.inRegisters.append(ConstrainedValue(call.count, ValueRep::reg(countGPR)));
    for (size_t i = 0; i < call.arguments.size(); ++i) {
        if (i < numberOfArgumentGPRs)
            placed.inRegisters.append(ConstrainedValue(call.arguments[i], ValueRep::reg(argumentGPR(static_cast<unsigned>(i)))));
        else
            placed.onStack.append(call.arguments[i]);
    }
    placed.stackWordsToReserve = static_cast<uint32_t>(placed.onStack.size());
    return placed;
}

PatchpointValue* CallEmitter::emitCall(const OperationContext& context, const CallTarget& target, const PlacedArguments& placed, std::span<const ArgumentLocation> results, const CallOptions& options)
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
        break;
    }

    PatchpointValue* patchpoint = calls.createCallPatchpoint(context, request);
    for (const ConstrainedValue& argument : placed.inRegisters)
        patchpoint->append(argument);
    for (size_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex])
            patchpoint->append(ConstrainedValue(argument, stackWordRep(stackIndex)));
    }
    if (target.kind == TargetKind::Address)
        patchpoint->append(ConstrainedValue(target.calledAddress, ValueRep::reg(valueEntryGPR)));

    uint32_t stackWordsToReserve = placed.stackWordsToReserve;
    Vector<ValueRep, 1> resultConstraints;
    for (const ArgumentLocation& result : results) {
        switch (result.kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
        case ArgumentLocation::Kind::FPR:
            resultConstraints.append(ValueRep::reg(result.reg));
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

    RELEASE_ASSERT(!options.isFollowedByResultTest || target.kind == TargetKind::Function);
    uint32_t site = calls.addSite(context, options);
    B3::Air::StackSlot* keptRegisterStorage = generator().validates() ? calls.keptRegisterStorage() : nullptr;
    RegisterSet clobbered = request.lateClobbered;
    clobbered.merge(request.earlyClobbered);
    patchpoint->setGenerator([stubCalls = &m_records.stubCalls, kind = target.kind, calledFunction = target.calledFunction, entry = target.entry, calledStub = target.calledStub, immediate = target.immediate, expectation = target.expectation, passesCallee = target.passesCallee, site = OptionalIndex { site }, keptRegisterStorage, clobbered, caller = m_id, value = context.valueID().index(), mark = options.isFollowedByResultTest ? ReturnSiteMark::ResultTest : ReturnSiteMark::None](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        std::optional<KeptRegisterCheck> check;
        if (keptRegisterStorage)
            check = KeptRegisterCheck { clobbered, CCallHelpers::Address(GPRInfo::callFrameRegister, static_cast<int32_t>(keptRegisterStorage->offsetFromFP())), caller, value, true };
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
            stubCalls->callFunction(jit, calledFunction, entry, site, keptRegisters, mark);
            return;
        case TargetKind::Address:
            stubCalls->call(jit, callResolvedValue(), site, keptRegisters);
            return;
        case TargetKind::Stub:
            if (immediate)
                stubCalls->call(jit, calledStub, *immediate, site, keptRegisters);
            else
                stubCalls->call(jit, calledStub, site, keptRegisters);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
    calls.finishCallPatchpoint(context, patchpoint, site, request);
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

void CallEmitter::defineResultsOfCall(const ValueContext& value, std::span<const ArgumentLocation> results, PatchpointValue* call)
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
    RELEASE_ASSERT(!options.pureFunction && !options.throwSite);
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
    constexpr unsigned maxDepth = 4;
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
            for (BuiltinID callee : calleesOfCall(m_program, m_function.at(context.valueID())).builtins)
                expectation.name(m_program.builtins().canonicalID(callee));
        }
        for (BuiltinID callee : builtinsCalledByNativeCode())
            expectation.name(callee);
    }
    return expectation;
}

Boxed CallEmitter::callValue(const OperationContext& context, const ValueCall& call, const CallOptions& options)
{
    unsigned count = static_cast<unsigned>(call.arguments.size());
    bool passesStackWords = count > numberOfArgumentGPRs;
    Vector<LValue, 8> values;
    for (Boxed argument : call.arguments)
        values.append(argument.value());

    ConventionCall conventionCall;
    conventionCall.callee = call.callee.value();
    conventionCall.thisValue = call.thisValue.value();
    conventionCall.arguments = values.span();
    conventionCall.builtin = call.builtin;

    if (call.calleeName) {
        uint32_t name = call.calleeName->index();
        if (!passesStackWords && name <= ThrowSiteWord::maxNarrowA) {
            LValue throwWord = m_out.constInt64(throwSiteWords(ThrowSite { ThrowKind::NotAFunction, name, std::nullopt }, Representation::JSValue, static_cast<unsigned>(calleeGPR))[0]);
            conventionCall.stub = callValueNamed(count);
            conventionCall.stubOperands = { &throwWord, 1 };
            return callByConvention(context, conventionCall, options);
        }
        requireOrThrow(context, isCell(call.callee), ThrowKind::NotAFunction, Operand(call.callee), name);
        requireOrThrow(context, emitter<TypeTestEmitter>().isCallable(unboxCell(call.callee)), ThrowKind::NotAFunction, Operand(call.callee), name);
    }

    if (passesStackWords) {
        LValue countOperand = m_out.constInt64(count);
        conventionCall.stub = callValueWithCount();
        conventionCall.stubOperands = { &countOperand, 1 };
        return callByConvention(context, conventionCall, options);
    }
    if (isUndefinedConstant(conventionCall.thisValue)) {
        conventionCall.stub = callValueWithoutThis(count);
        conventionCall.thisValue = nullptr;
        return callByConvention(context, conventionCall, options);
    }
    conventionCall.stub = AOT::callValue(count);
    return callByConvention(context, conventionCall, options);
}

void CallEmitter::generateCall(const ValueContext& value)
{
    ValueID callee = value->calleeOperand();
    std::optional<StringID> name;
    if (value->calleeName() && !typeOf(callee).isSubsetOf(m_program.at(TypeTable::function), m_program))
        name = value->calleeName();
    Vector<Boxed, 8> arguments = boxedOperands(value->argumentsOperands());
    ValueCall call { in<Boxed>(callee), thisOrUndefined(value->thisValueOperand()), arguments.span(), name };
    if (value.returnCall()) {
        tailCallValue(value, call);
        return;
    }
    define(value, callValue(value, call, { .isTailCall = value->isDemotedTailCall() }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
