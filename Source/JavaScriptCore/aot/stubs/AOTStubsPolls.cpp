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

static void callPollSlow(StubAssembler& jit)
{
    jit.preservingRegistersClobberedBy(StubCallee::Operation, false, [&](unsigned) {
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTPollSlow, T11);
    });
}

static void callPollBeforeEntering(StubAssembler& jit, unsigned bytesAboveSavedRegisters)
{
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
static void leaveFunctionAtPrologue(StubAssembler& jit, StubKey throwingStub)
{
    constexpr GPRReg returnAddress = CCallHelpers::s_scratchRegister;
    static_assert(noOverlap(returnAddress, GPRInfo::returnValueGPR, GPRInfo::returnValueGPR2, instanceGPR));
    jit.loadPtr(Address(sp), A1);
    jit.move(instanceGPR, A0);
    jit.subPtr(TrustedImm32(sizeof(void*)), sp);
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
