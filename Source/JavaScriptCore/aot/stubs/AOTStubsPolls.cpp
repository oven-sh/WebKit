/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPolls.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsExceptions.h"
#include "StackManager.h"
#include "VM.h"

namespace JSC { namespace AOT {

// In a frame with a record, with the stack pointer aligned as at the entry of a stub.
static void callPollSlow(StubAssembler& jit)
{
    jit.preservingRegistersClobberedBy(StubCallee::Operation, false, [&](unsigned) {
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTPollSlow, T11);
    });
}

// bytesAboveSavedRegisters: from the registers that are saved for the operation up to where the stack pointer is once the call of the function object has returned.
static void callPollBeforeEntering(StubAssembler& jit, unsigned bytesAboveSavedRegisters)
{
    // A call by value passes nothing in a floating-point register and keeps nothing in one.
    bool entersFunctionObject = static_cast<PollBeforeEnteringForm>(jit.key().form()) == PollBeforeEnteringForm::FunctionObject;
    jit.preservingRegistersClobberedBy(StubCallee::Operation, false, entersFunctionObject ? RegisterSet::allFPRs() : RegisterSet(), [&](unsigned bytes) {
        jit.addPtr(TrustedImm32(bytes + bytesAboveSavedRegisters), sp, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTPollBeforeEntering, T11);
    });
}

StubSignature signatureOfPollAtPrologue(StubKey) { return { }; }
StubSignature signatureOfPollAtPrologueWithoutCollecting(StubKey) { return { }; }
StubSignature signatureOfPollAtLoop(StubKey) { return { }; }
StubSignature signatureOfPollBeforeEntering(StubKey) { return { }; }

#if CPU(X86_64)
// The return address of the stub is on top of the stack. The throwing stub finds the stack as the caller of the function left it: a caller without a frame record is found by the stack pointer alone.
static void leaveFunctionAtPrologue(StubAssembler& jit, StubKey throwingStub)
{
    constexpr GPRReg returnAddress = CCallHelpers::s_scratchRegister;
    static_assert(noOverlap(returnAddress, GPRInfo::returnValueGPR, GPRInfo::returnValueGPR2, instanceGPR));
    jit.loadPtr(Address(sp), A1);
    jit.move(instanceGPR, A0);
    jit.subPtr(TrustedImm32(sizeof(void*)), sp);
    // This path does not return to the function, so it clobbers nothing for it.
    jit.withSavedRegisters(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()), [&] {
        jit.callOperation(Entry::operationAOTBytesAllocatedAtPrologueCheck, T11);
    });
    jit.addPtr(TrustedImm32(sizeof(void*)), sp);
    Jump hasNoFrameRecord = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR);
    jit.emitFunctionEpilogue();
    Jump isAtReturnAddress = jit.jump();
    hasNoFrameRecord.link(&jit);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(sp, GPRInfo::returnValueGPR, CCallHelpers::TimesOne, sizeof(void*)), sp);
    isAtReturnAddress.link(&jit);
    jit.pop(returnAddress);
    jit.addPtr(GPRInfo::returnValueGPR2, sp);
    jit.push(returnAddress);
    jit.jumpToStub(throwingStub);
}
#endif

static void generatePollAtPrologue(StubAssembler& jit, Entry operation)
{
#if CPU(X86_64)
    // Frame setup clobbers no register here, so every one of them may hold a value of the caller, the macro assembler's scratch register too.
    constexpr GPRReg stackPointerAtSite = X86Registers::eax;
    jit.push(stackPointerAtSite);
    jit.addPtr(TrustedImm32(2 * sizeof(void*)), sp, stackPointerAtSite);
    Address limit = StubAssembler::softStackLimitWord();
    jit.m_assembler.cmpq_mr(limit.offset, limit.base, stackPointerAtSite);
    jit.pop(stackPointerAtSite);
    Jump doesNotFit(jit.m_assembler.jCC(X86Assembler::ConditionB));
    Jump hasThrown;
    Jump mustThrow;
    jit.preservingCallerSavedRegisters(false, [&](unsigned bytes) {
        jit.loadCallerReturnAddress(bytes, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(operation, T11);
        hasThrown = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR);
        mustThrow = jit.branch64(CCallHelpers::Equal, GPRInfo::returnValueGPR, TrustedImm32(pollAtPrologueMustThrow));
        jit.storePtr(GPRInfo::returnValueGPR, Address(sp, bytes));
    });
    jit.ret();

    hasThrown.link(&jit);
    jit.emitFunctionEpilogue();
    leaveFunctionAtPrologue(jit, handleException(false));

    mustThrow.link(&jit);
    jit.emitFunctionEpilogue();
    doesNotFit.link(&jit);
    leaveFunctionAtPrologue(jit, throwStackOverflow(false));
#else
    jit.linkToStub(jit.branchPtr(CCallHelpers::Below, sp, StubAssembler::softStackLimitWord()), throwStackOverflow(true));
    jit.preservingCallerSavedRegisters(false, [&](unsigned bytes) {
        jit.loadCallerReturnAddress(bytes, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(operation, T11);
        jit.storePtr(GPRInfo::returnValueGPR, Address(sp));
    });
    jit.linkToStub(jit.branchTestPtr(CCallHelpers::Zero, CCallHelpers::linkRegister), handleExceptionAtPrologue());
    jit.linkToStub(jit.branch64(CCallHelpers::Equal, CCallHelpers::linkRegister, TrustedImm32(pollAtPrologueMustThrow)), throwStackOverflow(true));
    jit.ret();
#endif
}

void generatePollAtPrologue(StubAssembler& jit)
{
    generatePollAtPrologue(jit, Entry::operationAOTPollAtPrologue);
}

void generatePollAtPrologueWithoutCollecting(StubAssembler& jit)
{
    generatePollAtPrologue(jit, Entry::operationAOTPollAtPrologueWithoutCollecting);
}

void generatePollAtLoop(StubAssembler& jit)
{
#if CPU(X86_64)
    constexpr GPRReg scratch = CCallHelpers::s_scratchRegister;
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(sizeof(void*)), sp);
    callPollSlow(jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
    Jump hasThrown = jit.branchTestPtr(CCallHelpers::NonZero, Address(scratch, VM::exceptionOffset()));
    jit.emitFunctionEpilogue();
    if (jit.key().callerFrame() == CallerFrame::Frameless)
        jit.emitFramelessCallerEpilogue();
    jit.returnToCaller();

    hasThrown.link(&jit);
    jit.emitFunctionEpilogue();
    jit.jumpToStub(handleException(true));
#else
    // A poll clobbers the scratch registers of the macro assembler and nothing else.
    constexpr GPRReg scratch = ARM64Registers::x17;
    jit.emitFunctionPrologue();
    callPollSlow(jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
    jit.loadPtr(Address(scratch, VM::exceptionOffset()), scratch);
    jit.emitFunctionEpilogue();
    jit.linkToStub(jit.branchTestPtr(CCallHelpers::NonZero, scratch), handleException(true));
    if (jit.key().callerFrame() == CallerFrame::Frameless)
        jit.emitFramelessCallerEpilogueAndReturn(scratch);
    else
        jit.returnToCaller();
#endif
}

void generatePollBeforeEntering(StubAssembler& jit)
{
#if CPU(X86_64)
    constexpr GPRReg scratch = CCallHelpers::s_scratchRegister;
    jit.addPtr(TrustedImm32(sizeof(void*)), sp, scratch);
    Jump doesNotFit = jit.branchPtr(CCallHelpers::Below, scratch, StubAssembler::softStackLimitWord());
    jit.emitFunctionPrologue();
    // The frame record, the return address of this stub and the return address of the call.
    callPollBeforeEntering(jit, 3 * sizeof(void*));
    jit.emitFunctionEpilogue();
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
    Jump hasThrown = jit.branchTestPtr(CCallHelpers::NonZero, Address(scratch, VM::exceptionOffset()));
    jit.ret();

    hasThrown.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(void*)), sp);
    jit.jumpToStub(handleException(false));

    doesNotFit.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(void*)), sp);
#else
    constexpr GPRReg siteReturnAddress = ARM64Registers::x16;
    constexpr GPRReg scratch = ARM64Registers::x17;
    Jump doesNotFit = jit.branchPtr(CCallHelpers::Below, sp, StubAssembler::softStackLimitWord());
    jit.pushPair(siteReturnAddress, CCallHelpers::linkRegister);
    callPollBeforeEntering(jit, 2 * sizeof(void*));
    constexpr GPRReg exception = siteReturnAddress;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), exception);
    jit.loadPtr(Address(exception, VM::exceptionOffset()), exception);
    jit.popPair(CCallHelpers::linkRegister, scratch);
    Jump hasThrown = jit.branchTestPtr(CCallHelpers::NonZero, exception);
    // A return, so that it pairs with the call in the return-address predictor.
    jit.m_assembler.ret(scratch);

    hasThrown.link(&jit);
    jit.jumpToStub(handleException(false));

    doesNotFit.link(&jit);
    jit.move(siteReturnAddress, CCallHelpers::linkRegister);
#endif
    jit.jumpToStub(throwStackOverflow());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
