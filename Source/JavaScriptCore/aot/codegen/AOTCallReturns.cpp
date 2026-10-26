/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include "AOTClosureTestEmitter.h"
#include "AOTCodeSites.h"
#include "AOTConstantEmitter.h"
#include "AOTControlEmitter.h"
#include "AOTConvention.h"
#include "AOTElideStackChecks.h"
#include "AOTHintEmitter.h"
#include "AOTPromisedResults.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsClosures.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsCalls.h"
#include "AOTStubsValues.h"
#include "AOTTypeTestEmitter.h"
#include "AOTVector.h"
#include "AirCode.h"
#include "AirFrameEmission.h"
#include "AirGenerationContext.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3UpsilonValue.h"
#include "JSCJSValueInlines.h"
#include "StackAlignment.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool returnsNothing(const ArgumentLocations& locations)
{
    return std::ranges::all_of(locations.results, [](const ArgumentLocation& result) {
        return result.kind == ArgumentLocation::Kind::None;
    });
}

static bool hasCompatibleResults(const ArgumentLocations& callee, const ArgumentLocations& caller)
{
    return std::ranges::equal(callee.results, caller.results, [](const ArgumentLocation& returned, const ArgumentLocation& expected) {
        return returned.kind == expected.kind && returned.reg == expected.reg && returned.stackIndex == expected.stackIndex
            && haveSameRegisterRepresentation(returned.representation, expected.representation);
    });
}

static unsigned spilledBytesOf(const CodeGenerationInputs& inputs, FunctionID id)
{
    return inputs.conventions.of(inputs.program, id).spillsArgumentRegisters ? sizeOfSpilledArgumentRegisters : 0;
}

static void emitStoreToStackWord(CCallHelpers& jit, const B3::Air::Code& code, const B3::ValueRep& source, GPRReg scratch, uint32_t stackIndex)
{
    CCallHelpers::Address word = code.addressInFrame(offsetOfStackWordFromFrame(stackIndex));
    if (source.isGPR()) {
        jit.store64(source.gpr(), word);
        return;
    }
    if (source.isFPR()) {
        jit.storeDouble(source.fpr(), word);
        return;
    }
    if (source.isStack())
        jit.load64(code.addressInFrame(source.offsetFromFP()), scratch);
    else
        source.emitRestore(jit, scratch);
    jit.store64(scratch, word);
}

// CallTarget is private to CallEmitter, so its type is deduced.
template<typename Target>
static void emitJumpTo(CCallHelpers& jit, CodeRecords& records, const Target& target)
{
    using TargetKind = decltype(Target::kind);
    switch (target.kind) {
    case TargetKind::Function:
        records.stubCalls.jumpToFunction(jit, target.calledFunction, target.entry);
        return;
    case TargetKind::Address:
        jit.farJump(valueEntryGPR, JSEntryPtrTag);
        return;
    case TargetKind::Stub:
        if (target.immediate)
            records.stubCalls.tailCall(jit, target.calledStub, *target.immediate);
        else
            records.stubCalls.tailCall(jit, target.calledStub);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A function whose check its callers cover is entered behind a check. Every other target checks the stack for itself where it sets up a frame.
template<typename Target>
static bool pollsWithoutFrame(const CodeGenerationInputs& inputs, FunctionID id, const Target& target)
{
    using TargetKind = decltype(Target::kind);
    if (!hasFaultingLoopPolls || spilledBytesOf(inputs, id))
        return false;
    return target.kind != TargetKind::Function || inputs.stackChecks.of(target.calledFunction) != StackCheck::CoveredByCallers;
}

bool CallEmitter::hasStackWordsOfCaller(uint32_t numberOfWords) const
{
    return !numberOfWords || (hasStackArgumentAreaOf(m_function.role) && numberOfWords <= inputs().conventions.stackWordsReservedByCallersOf(m_program, m_id));
}

// The check is the poll of a cycle of tail calls.
void CallEmitter::didEmitTailCall()
{
    if (inputs().stackChecks.of(m_id) == StackCheck::Required)
        m_records.frame.needsStackCheckForTailCalls = true;
}

void CallEmitter::emitTailCall(const OperationContext& context, const CallTarget& target, const PlacedArguments& placed)
{
    RELEASE_ASSERT(placed.onStack.size() <= placed.stackWordsToReserve);
    if (target.kind == TargetKind::Function) {
        const CallingConventions& conventions = inputs().conventions;
        ArgumentLocations own = conventions.locationsOf(m_program, m_id);
        if (!returnsNothing(own) && !hasCompatibleResults(conventions.locationsOf(m_program, target.calledFunction), own)) {
            fail(context, InternalErrorKind::InvalidReturnCall);
            m_out.unreachable();
            return;
        }
    }

    bool fitsInIncomingArea = hasStackWordsOfCaller(placed.stackWordsToReserve);
    // An argument list frame needs every slot below the frame record, and that layout is requested before code generation.
    if (!fitsInIncomingArea && !m_records.frame.readsCallerFrame) {
        fail(context, InternalErrorKind::InvalidReturnCall);
        m_out.unreachable();
        return;
    }

    didEmitTailCall();
    if (fitsInIncomingArea) {
        if (m_records.frame.needsStackCheckForTailCalls && pollsWithoutFrame(inputs(), m_id, target))
            emitter<HintEmitter>().pollWhereNoFrameIsSetUp(context);
        jumpAfterFrameTeardown(target, placed);
    } else
        callFromArgumentListFrame(target, placed);
}

B3::PatchpointValue* CallEmitter::leaveAfterFrameTeardown(std::span<const B3::ConstrainedValue> inRegisters, std::span<const StackWord> stackWords, const Invocable<void(CCallHelpers&)> auto& leave)
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    for (const B3::ConstrainedValue& value : inRegisters)
        patchpoint->append(value);
    Vector<uint32_t, 4> stackIndices;
    for (const StackWord& word : stackWords) {
        patchpoint->append(B3::ConstrainedValue(word.value, B3::ValueRep::ColdAny));
        stackIndices.append(word.index);
    }
    if (!stackIndices.isEmpty())
        patchpoint->numGPScratchRegisters = 1;
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    patchpoint->setRunsWithoutFrame(stackIndices.isEmpty());
    patchpoint->setGenerator([numberInRegisters = static_cast<unsigned>(inRegisters.size()), stackIndices, leave](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        for (unsigned i = 0; i < stackIndices.size(); ++i)
            emitStoreToStackWord(jit, params.code(), params[numberInRegisters + i], params.gpScratch(0), stackIndices[i]);
        if (params.context().frameIsSetUp)
            B3::Air::emitFrameTeardown(jit, params.code(), jit.scratchRegister());
        leave(jit);
    });
    return patchpoint;
}

void CallEmitter::jumpAfterFrameTeardown(const CallTarget& target, const PlacedArguments& placed)
{
    Vector<B3::ConstrainedValue, 8> inRegisters = placed.inRegisters;
    if (target.kind == TargetKind::Address)
        inRegisters.append(B3::ConstrainedValue(target.calledAddress, B3::ValueRep::reg(valueEntryGPR)));
    Vector<StackWord, 4> stackWords;
    for (uint32_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex])
            stackWords.append({ stackIndex, argument });
    }
    unsigned spilledBytes = spilledBytesOf(inputs(), m_id);
    RELEASE_ASSERT(stackWords.isEmpty() || !spilledBytes);

    CallTarget destination = target;
    destination.calledAddress = nullptr;
    B3::PatchpointValue* patchpoint = leaveAfterFrameTeardown(inRegisters.span(), stackWords.span(), [records = &m_records, destination, spilledBytes](CCallHelpers& jit) {
        if (spilledBytes)
            emitDropSpilledArgumentRegisters(jit);
        emitJumpTo(jit, *records, destination);
    });
    // The stack check is part of the frame setup.
    patchpoint->setFollowsFrameSetup(m_records.frame.needsStackCheckForTailCalls && !pollsWithoutFrame(inputs(), m_id, target));
}

void CallEmitter::returnMisfit(Boxed value)
{
    PlacedArguments placed;
    placed.inRegisters.append(B3::ConstrainedValue(value.value(), B3::ValueRep::reg(GPRInfo::returnValueGPR)));
    jumpAfterFrameTeardown(CallTarget::stub(AOT::returnMisfit(), std::nullopt), placed);
}

void CallEmitter::callFromArgumentListFrame(const CallTarget& target, const PlacedArguments& placed)
{
    RELEASE_ASSERT(m_records.frame.readsCallerFrame);
    m_records.frame.dropsArgumentListFrames = true;

    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    for (const B3::ConstrainedValue& argument : placed.inRegisters)
        patchpoint->append(argument);
    if (target.kind == TargetKind::Address)
        patchpoint->append(B3::ConstrainedValue(target.calledAddress, B3::ValueRep::reg(valueEntryGPR)));
    for (size_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex])
            patchpoint->append(B3::ConstrainedValue(argument, B3::ValueRep::stackArgument(safeCast<B3::ValueRep::OffsetType>(stackIndex * sizeOfStackWord))));
    }
    m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(placed.stackWordsToReserve * sizeOfStackWord));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobber(registersClobberedByCompiledCalls());
    patchpoint->effects.terminal = true;
    patchpoint->setNeedsFrameRecord(true);

    CallTarget destination = target;
    destination.calledAddress = nullptr;
    patchpoint->setGenerator([records = &m_records, destination, numberOfWords = placed.stackWordsToReserve, spilledBytes = spilledBytesOf(inputs(), m_id)](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        RELEASE_ASSERT(params.context().frameIsSetUp);
        B3::Air::emitCalleeSaveRestore(jit, params.code());
        emitMoveStackWordsToArgumentListFrame(jit, numberOfWords, spilledBytes);
        emitCallFromArgumentListFrame(jit, [&] {
            emitJumpTo(jit, *records, destination);
        });
    });
}

void CallEmitter::tailCallByConvention(const OperationContext& context, const ConventionCall& call)
{
    emitTailCall(context, CallTarget::stub(call.stub, std::nullopt), placeConventionArguments(call));
}

// The stub that calls finds out too late that there is nothing to call: the frame that the error names is gone by then.
void CallEmitter::requireCallableBeforeTailCall(const OperationContext& context, Boxed callee, std::optional<ThrowSite> siteThatNamesCallee)
{
    ClosureTestEmitter& closures = emitter<ClosureTestEmitter>();
    LBasicBlock mayNotBeCallable = newBlock();
    LBasicBlock continuation = newBlock();
    branchUnless(isCell(callee), mayNotBeCallable);
    Condition isKnownToBeCallable = context.shape() == CodeShape::Fast ? closures.hasFunctionCellType(unboxCell(callee)) : closures.isSlimClosureAsCreated(unboxCell(callee));
    m_out.branch(isKnownToBeCallable.value(), normalEdge(continuation), rareEdge(mayNotBeCallable));

    m_out.appendTo(mayNotBeCallable);
    callStub(context, throwUnlessCallable(), { callee.value() }, { .throwSite = siteThatNamesCallee, .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

// The frame holds closures that the call lends. It stays until a callee returns that cannot come round to this call again by tail calls alone. Goes on where the callee is any other.
void CallEmitter::callAndReturnIfCalleeEndsItsTailCalls(const ValueContext& value, const ValueCall& call)
{
    ArgumentLocations own = inputs().conventions.locationsOf(m_program, m_id);
    if (own.results.size() != 1 || own.results[0].representation != Representation::JSValue)
        return;

    LBasicBlock keepsFrame = newBlock();
    LBasicBlock givesUpFrame = newBlock();
    LValue operands[] = { call.callee.value() };
    emitter<RuntimeCallEmitter>().callStubWithoutSiteAndBranch(valueEndsItsTailCalls(), operands, { normalEdge(keepsFrame), normalEdge(givesUpFrame) });

    m_out.appendTo(keepsFrame);
    Operand result = callValue(value, call, { .isTailCall = true });
    returnValues(*value.returnCall(), singleElementSpan(result));

    m_out.appendTo(givesUpFrame);
}

void CallEmitter::tailCallValue(const ValueContext& value, const ValueCall& call)
{
    if (!typeOf(value->calleeOperand()).isSubsetOf(m_program.at(TypeTable::function), m_program))
        requireCallableBeforeTailCall(value, call.callee, siteThatNamesCallee(value));

    unsigned count = static_cast<unsigned>(call.arguments.size());
    Vector<LValue, 8> values;
    for (Boxed argument : call.arguments)
        values.append(argument.value());
    if (call.lentArguments) {
        callAndReturnIfCalleeEndsItsTailCalls(value, call);
        for (unsigned index = 0; index < count; ++index) {
            if (call.lentArguments >> index & 1)
                values[index] = boxCell(callOperation<CellPointer>(value, RuntimeOperation::MoveClosureToHeap, { unboxCell(call.arguments[index]) }, { .isOnRarePath = true })).value();
        }
    }

    ConventionCall conventionCall { .callee = call.callee.value(), .thisValue = call.thisValue.value(), .arguments = values.span() };
    LValue countOperand = nullptr;
    chooseStubThatCallsValue(conventionCall, countOperand, call.calleeIsCell);
    tailCallByConvention(value, conventionCall);
}

void ControlEmitter::generateReturn(const TerminatorContext& terminator)
{
    ValueID value = terminator->valueOperand();
    uint32_t numberOfResults = static_cast<uint32_t>(m_function.results.size());
    Vector<Operand, 4> operands;
    if (numberOfResults == 1) {
        if (value && leavesAnotherWayUnlessPromised(terminator, value))
            return;
        if (value)
            operands.append(get(value));
        else
            operands.append(emitter<ConstantEmitter>().constantOf(jsUndefined(), inputs().conventions.resultRepresentation(m_program, m_id)));
    } else {
        for (uint32_t index = 0; index < numberOfResults; ++index)
            operands.append(generator().resultAt(value, index));
    }
    emitter<CallEmitter>().returnValues(terminator, operands.span());
}

// True where the return has been generated. The returns that seldom run share one test.
bool ControlEmitter::leavesAnotherWayUnlessPromised(const TerminatorContext& terminator, ValueID value)
{
    TypeID promised = inputs().promisedResults.of(m_id);
    if (!promised || typeOf(value).isSubsetOf(m_program.at(promised), m_program))
        return false;

    auto testAndReturn = [&](Boxed result, TypeID from) {
        TypeTestEmitter& tests = emitter<TypeTestEmitter>();
        LBasicBlock isPromised = newBlock();
        LBasicBlock isMisfit = newBlock();
        tests.emitBranch(result, tests.planFor(from, Representation::JSValue, promised), { normalEdge(isPromised), rareEdge(isMisfit) }, terminator.shape());
        m_out.appendTo(isMisfit);
        emitter<CallEmitter>().returnMisfit(result);
        m_out.appendTo(isPromised);
        Operand operand = result;
        emitter<CallEmitter>().returnValues(terminator, { &operand, 1 });
    };
    if (m_function.at(terminator.block()).frequency == BlockFrequency::Normal) {
        testAndReturn(in<Boxed>(value), m_function.at(value).type);
        return true;
    }

    if (!m_testedReturn) {
        LBasicBlock here = m_out.m_block;
        m_testedReturn = newBlock();
        m_out.appendTo(m_testedReturn);
        m_resultOfTestedReturn = m_out.phi(B3::Int64);
        testAndReturn(Boxed(m_resultOfTestedReturn), TypeTable::anything);
        m_out.appendTo(here);
    }
    m_out.m_block->appendNew<B3::UpsilonValue>(m_proc, m_out.origin(), in<Boxed>(value).value(), m_resultOfTestedReturn);
    m_out.jump(m_testedReturn);
    return true;
}

void ControlEmitter::generateReturnCall(const TerminatorContext& terminator)
{
    generator().generateReturnedCall(terminator, terminator->callOperand());
}

void CallEmitter::returnValues(const TerminatorContext&, std::span<const Operand> operands)
{
    const CallingConventions& conventions = inputs().conventions;
    ArgumentLocations locations = conventions.locationsOf(m_program, m_id);
    std::span<const ArgumentLocation> results = locations.results.span();
    bool spillsArgumentRegisters = conventions.of(m_program, m_id).spillsArgumentRegisters;
    RELEASE_ASSERT(operands.size() == results.size());
    for (size_t i = 0; i < results.size(); ++i)
        RELEASE_ASSERT(operands[i].representation == results[i].representation);

    if (!spillsArgumentRegisters && results.size() <= 1) {
        if (results.empty() || results[0].kind == ArgumentLocation::Kind::None) {
            m_out.retVoid();
            return;
        }
        RELEASE_ASSERT(results[0].reg == (isPassedInFPR(results[0].representation) ? Reg(FPRInfo::returnValueFPR) : Reg(GPRInfo::returnValueGPR)));
        m_out.ret(operands[0].value);
        return;
    }

    Vector<B3::ConstrainedValue, 8> inRegisters;
    Vector<StackWord, 4> stackWords;
    for (size_t i = 0; i < results.size(); ++i) {
        LValue value = operands[i].value;
        switch (results[i].kind) {
        case ArgumentLocation::Kind::None:
            break;
        case ArgumentLocation::Kind::GPR:
        case ArgumentLocation::Kind::FPR:
            inRegisters.append(B3::ConstrainedValue(value, B3::ValueRep::reg(results[i].reg)));
            break;
        case ArgumentLocation::Kind::Stack:
            stackWords.append({ results[i].stackIndex, value->type() == B3::Int32 ? m_out.zeroExt(value, B3::Int64) : value });
            break;
        }
    }
    RELEASE_ASSERT(stackWords.isEmpty() || !spillsArgumentRegisters);
    leaveAfterFrameTeardown(inRegisters.span(), stackWords.span(), [spillsArgumentRegisters](CCallHelpers& jit) {
        if (spillsArgumentRegisters)
            emitReturnDroppingSpilledArgumentRegisters(jit);
        else
            jit.ret();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
