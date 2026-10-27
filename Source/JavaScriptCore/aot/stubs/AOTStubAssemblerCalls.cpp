/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubAssembler.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStubsExceptions.h"
#include "AOTVector.h"
#include "VM.h"
#include <algorithm>

namespace JSC { namespace AOT {

void StubAssembler::callLeaf(Entry entry, unsigned bytesPushed)
{
    std::optional<RuntimeOperation> operation = runtimeOperationOf(entry);
    RELEASE_ASSERT(operation && isLeaf(*operation));
    checkHasNoBorrowedFPRs();
    recordCall(entry);
    recordClobber(leafFunctionGPR);
    if (preservesMostRegisters(*operation))
        recordCallee(StubCallee::PreserveMostLeaf);
    else
        recordClobbers(registersClobberedByOperations());
#if CPU(X86_64)
    RELEASE_ASSERT(!(bytesPushed % sizeof(CPURegister)));
    const unsigned padding = (bytesPushed + sizeof(CPURegister)) % stackAlignmentBytes() ? sizeof(CPURegister) : 0;
    if (padding)
        subPtr(TrustedImm32(padding), stackPointerRegister);
    loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtimeTable)), leafFunctionGPR);
    emitStackAlignmentCheck();
    CCallHelpers::call(Address(leafFunctionGPR, offsetInRuntimeTable(entry)), OperationPtrTag);
    if (padding)
        addPtr(TrustedImm32(padding), stackPointerRegister);
#else
    RELEASE_ASSERT(!(bytesPushed % stackAlignmentBytes()));
    pushToSave(linkRegister);
    loadEntry(entry, leafFunctionGPR);
    CCallHelpers::call(leafFunctionGPR, OperationPtrTag);
    popToRestore(linkRegister);
#endif
}

// An access that calls an accessor keeps few floating-point registers: storing and loading all of them was a third of the way to a getter and back.
static RegisterSet floatingPointRegistersLostToAccessors(StubCallee callee)
{
    constexpr unsigned numberKept = 2;
    RegisterSet lost = RegisterSet::macroClobberedFPRs();
    Vector<Reg, 32> others;
    registersClobberedBy(callee).forEach([&](Reg reg) {
        if (reg.isFPR() && !lost.contains(reg, IgnoreVectors))
            others.append(reg);
    });
    for (unsigned index = 0; index + numberKept < others.size(); ++index)
        lost.add(others[index], IgnoreVectors);
    return lost;
}

void StubAssembler::callAndReturnPreservingRegisters(const PreservingCall& preservingCall, const ScopedLambda<void()>& emitCall)
{
#if CPU(X86_64)
    constexpr GPRReg scratch = s_scratchRegister;
#else
    constexpr GPRReg scratch = ARM64Registers::x16;
#endif
    RELEASE_ASSERT(m_savedRegisterStack.isEmpty() && key().callerFrame() != CallerFrame::Frameless);
    checkHasNoBorrowedFPRs();
    emitFunctionPrologue();
#if CPU(X86_64)
    // The size of the save area aligns the stack of a stub that has pushed nothing, and the prologue pushed one register.
    subPtr(TrustedImm32(sizeof(CPURegister)), stackPointerRegister);
#endif
    // The stub returns behind the call, so what it has written itself is lost to its caller anyway.
    RegisterSet lostAnyway = m_recording.clobbered();
    lostAnyway.merge(RegisterSet::macroClobberedGPRs());
    lostAnyway.merge(floatingPointRegistersLostToAccessors(preservingCall.callee));
    RELEASE_ASSERT(!preservingCall.cellToKeep || !lostAnyway.contains(*preservingCall.cellToKeep, IgnoreVectors));
    preservingRegistersClobberedBy(preservingCall.callee, preservingCall.returnsValue, lostAnyway, [&](unsigned) {
        emitCall();
    });
    // The epilogue takes the stack pointer from the frame pointer, so the padding is not taken off first.
    emitFunctionEpilogue();
    if (!preservingCall.checksException) {
        RELEASE_ASSERT(!preservingCall.emitBeforeReturn);
        ret();
        return;
    }
    loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
    Jump threw = branchTestPtr(NonZero, Address(scratch, VM::exceptionOffset()));
    if (preservingCall.emitBeforeReturn)
        (*preservingCall.emitBeforeReturn)();
    ret();
    threw.link(this);
    if (key().callerFrame() == CallerFrame::StubFrame)
        emitFunctionEpilogue();
    jumpToStub(handleException(true));
}

void StubAssembler::exitOnMiss(Entry cachingOperation, const OperationCall& operationCall)
{
    m_recording.exits.append(StubExitRecord { StubExit::Miss, cachingOperation, RuntimeCounterID::None });
    callOperationAndReturn(cachingOperation, operationCall);
}

void StubAssembler::exitOnCannot(Entry finisher, RuntimeCounterID reason, GPRReg reasonArgument, const OperationCall& operationCall)
{
    RELEASE_ASSERT(reason != RuntimeCounterID::None);
    m_recording.exits.append(StubExitRecord { StubExit::Cannot, finisher, reason });
    OperationCall callWithReason = operationCall;
    auto unused = std::ranges::find_if(callWithReason.constantArguments, [](const auto& constant) {
        return !constant;
    });
    RELEASE_ASSERT(unused != callWithReason.constantArguments.end());
    *unused = OperationCall::ConstantArgument { reasonArgument, static_cast<uint32_t>(reason) };
    callOperationAndReturn(finisher, callWithReason);
}

void StubAssembler::callEngineThunk(Entry pointer, GPRReg scratch)
{
    RELEASE_ASSERT(!isOperation(pointer) && canWrite(scratch));
    recordClobber(scratch);
    loadRuntimePointer(pointer, scratch);
    call(scratch, JSEntryPtrTag);
}

void StubAssembler::callCode(CodeSymbol symbol)
{
    RELEASE_ASSERT(symbol.space == SymbolSpace::RegExp);
    emitStackAlignmentCheck();
    RegisterSet lost = registersClobberedByOperations();
    if (!m_savedRegisterStack.isEmpty())
        lost.exclude(m_savedRegisterStack.last());
    m_recording.clobberedByCallees.merge(lost);
    m_recording.symbols.appendIfNotContains(symbol);
    m_relocations.call(symbol);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
