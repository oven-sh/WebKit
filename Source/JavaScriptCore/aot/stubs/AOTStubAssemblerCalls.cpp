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

RegisterSet floatingPointRegistersLostToAccessors(StubCallee callee)
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
    if (!preservingCall.registersThatCalleeLoses.empty()) {
        RELEASE_ASSERT(!preservingCall.cellsToKeep[0]);
        preserving(preservingCall.registersThatCalleeLoses, [&] {
            emitCall();
        });
    } else {
        RegisterSet lostAnyway = m_recording.clobbered();
        lostAnyway.merge(RegisterSet::macroClobberedGPRs());
        lostAnyway.merge(floatingPointRegistersLostToAccessors(preservingCall.callee));
        SavedRegisterArea area = storeRegistersClobberedBy(preservingCall.callee, preservingCall.returnsValue, lostAnyway, preservingCall.cellsToKeep, isX86_64() ? sizeof(CPURegister) : 0);
        withSavedRegisters(RegisterSet::fromIterable(area.registers), [&] {
            emitCall();
        });
        loadSavedRegisters(area, StackPointerAfterLoads::IsLeftToEpilogue);
    }
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
    if (preservingCall.callee == StubCallee::HostFunction)
        pollBeforeReturningFromNativeCode();
    ret();
    threw.link(this);
    if (key().callerFrame() == CallerFrame::StubFrame)
        emitFunctionEpilogue();
    jumpToStub(handleException(true));
}

void StubAssembler::exitOnMiss(Entry cachingOperation, const OperationCall& operationCall)
{
    callOperationAndReturn(cachingOperation, operationCall);
}

void StubAssembler::exitOnCannot(Entry finisher, const OperationCall& operationCall)
{
    callOperationAndReturn(finisher, operationCall);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
