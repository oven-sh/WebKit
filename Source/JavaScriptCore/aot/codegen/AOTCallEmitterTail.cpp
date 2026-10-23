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
#include "AOTConvention.h"
#include "AOTElideStackChecks.h"
#include "AOTHintEmitter.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStubsClosures.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsCalls.h"
#include "AOTStubsValues.h"
#include "AOTVector.h"
#include "AirCode.h"
#include "AirFrameEmission.h"
#include "AirGenerationContext.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
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

void CallEmitter::jumpAfterFrameTeardown(const CallTarget& target, const PlacedArguments& placed)
{
    B3::PatchpointValue* patchpoint = m_out.patchpoint(B3::Void);
    for (const B3::ConstrainedValue& argument : placed.inRegisters)
        patchpoint->append(argument);
    if (target.kind == TargetKind::Address)
        patchpoint->append(B3::ConstrainedValue(target.calledAddress, B3::ValueRep::reg(valueEntryGPR)));
    unsigned numberInRegisters = patchpoint->numChildren();
    Vector<uint32_t, 4> stackIndices;
    for (uint32_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex]) {
            patchpoint->append(B3::ConstrainedValue(argument, B3::ValueRep::WarmAny));
            stackIndices.append(stackIndex);
        }
    }

    unsigned spilledBytes = spilledBytesOf(inputs(), m_id);
    RELEASE_ASSERT(stackIndices.isEmpty() || !spilledBytes);
    if (!stackIndices.isEmpty())
        patchpoint->numGPScratchRegisters = 1;
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    patchpoint->setRunsWithoutFrame(stackIndices.isEmpty());
    // The stack check is part of the frame setup.
    patchpoint->setFollowsFrameSetup(m_records.frame.needsStackCheckForTailCalls && !pollsWithoutFrame(inputs(), m_id, target));

    CallTarget destination = target;
    destination.calledAddress = nullptr;
    patchpoint->setGenerator([records = &m_records, destination, numberInRegisters, stackIndices, spilledBytes](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        for (unsigned i = 0; i < stackIndices.size(); ++i)
            emitStoreToStackWord(jit, params.code(), params[numberInRegisters + i], params.gpScratch(0), stackIndices[i]);
        if (params.context().frameIsSetUp)
            B3::Air::emitFrameTeardown(jit, params.code(), jit.scratchRegister());
        if (spilledBytes)
            emitDropSpilledArgumentRegisters(jit);
        emitJumpTo(jit, *records, destination);
    });
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
    if (!typeOf(value->calleeOperand()).isSubsetOf(m_program.at(TypeTable::function), m_program)) {
        std::optional<ThrowSite> siteThatNamesCallee;
        if (value->calleeName())
            siteThatNamesCallee = ThrowSite { ThrowKind::NotAFunction, value->calleeName().index(), std::nullopt };
        requireCallableBeforeTailCall(value, call.callee, siteThatNamesCallee);
    }

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

    ConventionCall conventionCall;
    conventionCall.callee = call.callee.value();
    conventionCall.thisValue = call.thisValue.value();
    conventionCall.arguments = values.span();

    if (count > numberOfArgumentGPRs) {
        LValue countOperand = m_out.constInt64(count);
        conventionCall.stub = callValueWithCount();
        conventionCall.stubOperands = singleElementSpan(countOperand);
        tailCallByConvention(value, conventionCall);
        return;
    }
    if (isUndefinedConstant(conventionCall.thisValue)) {
        conventionCall.stub = callValueWithoutThis(count);
        conventionCall.thisValue = nullptr;
    } else
        conventionCall.stub = AOT::callValue(count);
    tailCallByConvention(value, conventionCall);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
