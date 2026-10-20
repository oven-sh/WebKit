/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCallEmitter.h"

#if ENABLE(AOT)

#include "AOTChooseCallingConventions.h"
#include "AOTClosureTestEmitter.h"
#include "AOTConvention.h"
#include "AOTElideStackChecks.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsCalls.h"
#include "AOTTypeTestEmitter.h"
#include "AirCode.h"
#include "AirFrameEmission.h"
#include "AirGenerationContext.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "B3PatchpointValue.h"
#include "B3StackmapGenerationParams.h"
#include "StackAlignment.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

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

static void emitStoreToStackWord(CCallHelpers& jit, const ValueRep& source, GPRReg scratch, uint32_t stackIndex)
{
    CCallHelpers::Address word(GPRInfo::callFrameRegister, static_cast<int32_t>(offsetOfStackWordFromFrame(stackIndex)));
    if (source.isGPR()) {
        jit.store64(source.gpr(), word);
        return;
    }
    if (source.isFPR()) {
        jit.storeDouble(source.fpr(), word);
        return;
    }
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

bool CallEmitter::hasStackWordsOfCaller(uint32_t numberOfWords) const
{
    return !numberOfWords || (hasStackArgumentAreaOf(m_function.role) && numberOfWords <= inputs().conventions.stackWordsReservedByCallersOf(m_program, m_id));
}

// The check is the poll of a cycle of tail calls. Callers cover only what is in no cycle. An entrypoint that leaves its check to its callees enters functions
// with a check of their own, and a cycle goes on from there through one of their tail calls.
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
    if (fitsInIncomingArea)
        jumpAfterFrameTeardown(target, placed);
    else
        callFromArgumentListFrame(target, placed);
}

void CallEmitter::jumpAfterFrameTeardown(const CallTarget& target, const PlacedArguments& placed)
{
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    for (const ConstrainedValue& argument : placed.inRegisters)
        patchpoint->append(argument);
    if (target.kind == TargetKind::Address)
        patchpoint->append(ConstrainedValue(target.calledAddress, ValueRep::reg(valueEntryGPR)));
    unsigned numberInRegisters = patchpoint->numChildren();
    Vector<uint32_t, 4> stackIndices;
    for (uint32_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex]) {
            patchpoint->append(ConstrainedValue(argument, ValueRep::WarmAny));
            stackIndices.append(stackIndex);
        }
    }

    unsigned spilledBytes = spilledBytesOf(inputs(), m_id);
    RELEASE_ASSERT(stackIndices.isEmpty() || !spilledBytes);
    if (!stackIndices.isEmpty())
        patchpoint->numGPScratchRegisters = 1;
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->effects.terminal = true;
    // The stack check is part of the frame setup.
    patchpoint->setRunsWithoutFrame(stackIndices.isEmpty() && !m_records.frame.needsStackCheckForTailCalls);

    CallTarget destination = target;
    destination.calledAddress = nullptr;
    patchpoint->setGenerator([records = &m_records, destination, numberInRegisters, stackIndices, spilledBytes](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        for (unsigned i = 0; i < stackIndices.size(); ++i)
            emitStoreToStackWord(jit, params[numberInRegisters + i], params.gpScratch(0), stackIndices[i]);
        if (params.context().frameIsSetUp)
            Air::emitFrameTeardown(jit, params.code());
        if (spilledBytes)
            emitDropSpilledArgumentRegisters(jit);
        emitJumpTo(jit, *records, destination);
    });
}

void CallEmitter::callFromArgumentListFrame(const CallTarget& target, const PlacedArguments& placed)
{
    RELEASE_ASSERT(m_records.frame.readsCallerFrame);
    m_records.frame.dropsArgumentListFrames = true;

    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    for (const ConstrainedValue& argument : placed.inRegisters)
        patchpoint->append(argument);
    if (target.kind == TargetKind::Address)
        patchpoint->append(ConstrainedValue(target.calledAddress, ValueRep::reg(valueEntryGPR)));
    for (size_t stackIndex = 0; stackIndex < placed.onStack.size(); ++stackIndex) {
        if (LValue argument = placed.onStack[stackIndex])
            patchpoint->append(ConstrainedValue(argument, ValueRep::stackArgument(safeCast<ValueRep::OffsetType>(stackIndex * sizeOfStackWord))));
    }
    m_proc.requestCallArgAreaSizeInBytes(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(placed.stackWordsToReserve * sizeOfStackWord));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobber(registersClobberedByCompiledCalls());
    patchpoint->effects.terminal = true;

    CallTarget destination = target;
    destination.calledAddress = nullptr;
    patchpoint->setGenerator([records = &m_records, destination, numberOfWords = placed.stackWordsToReserve, spilledBytes = spilledBytesOf(inputs(), m_id)](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        RELEASE_ASSERT(params.context().frameIsSetUp);
        Air::emitCalleeSaveRestore(jit, params.code());
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

void CallEmitter::requireCallableBeforeTailCall(const OperationContext& context, Boxed callee, std::optional<StringID> name)
{
    if (name) {
        requireOrThrow(context, isCell(callee), ThrowKind::NotAFunction, Operand(callee), name->index());
        requireOrThrow(context, emitter<TypeTestEmitter>().isCallable(unboxCell(callee)), ThrowKind::NotAFunction, Operand(callee), name->index());
        return;
    }

    LBasicBlock mayNotBeCallable = newBlock();
    LBasicBlock continuation = newBlock();
    branchUnless(isCell(callee), mayNotBeCallable);
    m_out.branch(emitter<ClosureTestEmitter>().hasFunctionCellType(unboxCell(callee)).value(), normalEdge(continuation), rareEdge(mayNotBeCallable));

    m_out.appendTo(mayNotBeCallable);
    callVoidOperation(context, RuntimeOperation::ThrowUnlessCallable, { callee }, { .isOnRarePath = true });
    m_out.jump(continuation);

    m_out.appendTo(continuation);
}

void CallEmitter::tailCallValue(const ValueContext& value, const ValueCall& call)
{
    if (!typeOf(value->calleeOperand()).isSubsetOf(m_program.at(TypeTable::function), m_program))
        requireCallableBeforeTailCall(value, call.callee, call.calleeName);

    unsigned count = static_cast<unsigned>(call.arguments.size());
    Vector<LValue, 8> values;
    for (Boxed argument : call.arguments)
        values.append(argument.value());

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
