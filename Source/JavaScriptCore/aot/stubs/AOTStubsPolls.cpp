/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPolls.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsExceptions.h"
#include "StackManager.h"
#include "VM.h"

namespace JSC { namespace AOT {

static constexpr Address softStackLimitWord { instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit() };

// In a frame with a record, with the stack pointer aligned as at the entry of a stub.
static void callPollSlow(StubAssembler& jit)
{
    jit.preservingRegistersClobberedBy(StubCallee::Operation, false, [&](unsigned) {
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTPollSlow, T11);
    });
}

StubSignature signatureOfPollAtPrologue(StubKey) { return { }; }
StubSignature signatureOfPollAtLoop(StubKey) { return { }; }
StubSignature signatureOfPollBeforeEntering(StubKey) { return { }; }

#if CPU(X86_64)
// The return address of the stub is on top of the stack.
static void leaveFunctionAtPrologue(StubAssembler& jit, StubKey throwingStubAtPrologue, StubKey throwingStub)
{
    jit.loadPtr(Address(sp), A1);
    jit.move(instanceGPR, A0);
    jit.subPtr(TrustedImm32(sizeof(void*)), sp);
    // This path does not return to the function, so it clobbers nothing for it.
    jit.withSavedRegisters(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()), [&] {
        jit.callOperation(Entry::operationAOTBytesAllocatedAtPrologueCheck, T11);
    });
    jit.addPtr(TrustedImm32(sizeof(void*)), sp);
    jit.linkToStub(jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR), throwingStubAtPrologue);
    jit.getEffectiveAddress(CCallHelpers::BaseIndex(sp, GPRInfo::returnValueGPR, CCallHelpers::TimesOne, sizeof(void*)), sp);
    jit.jumpToStub(throwingStub);
}
#endif

void generatePollAtPrologue(StubAssembler& jit)
{
#if CPU(X86_64)
    // Frame setup clobbers no register here, so every one of them may hold a value of the caller, the macro assembler's scratch register too.
    constexpr GPRReg stackPointerAtSite = X86Registers::eax;
    jit.push(stackPointerAtSite);
    jit.addPtr(TrustedImm32(2 * sizeof(void*)), sp, stackPointerAtSite);
    jit.m_assembler.cmpq_mr(softStackLimitWord.offset, softStackLimitWord.base, stackPointerAtSite);
    jit.pop(stackPointerAtSite);
    Jump doesNotFit(jit.m_assembler.jCC(X86Assembler::ConditionB));
    Jump hasThrown;
    Jump mustThrow;
    jit.preservingCallerSavedRegisters(false, [&](unsigned bytes) {
        jit.loadCallerReturnAddress(bytes, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTPollAtPrologue, T11);
        hasThrown = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR);
        mustThrow = jit.branch64(CCallHelpers::Equal, GPRInfo::returnValueGPR, TrustedImm32(pollAtPrologueMustThrow));
        jit.storePtr(GPRInfo::returnValueGPR, Address(sp, bytes));
    });
    jit.ret();

    hasThrown.link(&jit);
    jit.emitFunctionEpilogue();
    leaveFunctionAtPrologue(jit, handleExceptionAtPrologue(), handleException(false));

    mustThrow.link(&jit);
    jit.emitFunctionEpilogue();
    doesNotFit.link(&jit);
    leaveFunctionAtPrologue(jit, throwStackOverflow(true), throwStackOverflow(false));
#else
    jit.linkToStub(jit.branchPtr(CCallHelpers::Below, sp, softStackLimitWord), throwStackOverflow(true));
    jit.preservingCallerSavedRegisters(false, [&](unsigned bytes) {
        jit.loadCallerReturnAddress(bytes, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTPollAtPrologue, T11);
        jit.storePtr(GPRInfo::returnValueGPR, Address(sp));
    });
    jit.linkToStub(jit.branchTestPtr(CCallHelpers::Zero, CCallHelpers::linkRegister), handleExceptionAtPrologue());
    jit.linkToStub(jit.branch64(CCallHelpers::Equal, CCallHelpers::linkRegister, TrustedImm32(pollAtPrologueMustThrow)), throwStackOverflow(true));
    jit.ret();
#endif
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
    UNUSED_PARAM(jit);
    RELEASE_ASSERT_NOT_REACHED();
#endif
}

void generatePollBeforeEntering(StubAssembler& jit)
{
#if CPU(X86_64)
    constexpr GPRReg scratch = CCallHelpers::s_scratchRegister;
    jit.addPtr(TrustedImm32(sizeof(void*)), sp, scratch);
    Jump doesNotFit = jit.branchPtr(CCallHelpers::Below, scratch, softStackLimitWord);
    jit.emitFunctionPrologue();
    callPollSlow(jit);
    jit.emitFunctionEpilogue();
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
    Jump hasThrown = jit.branchTestPtr(CCallHelpers::NonZero, Address(scratch, VM::exceptionOffset()));
    Jump mustThrow = jit.branchTest8(CCallHelpers::NonZero, Address(scratch, VM::offsetOfTraps() + VMTraps::offsetOfStackManager() + StackManager::offsetOfStopsAtFunctionEntries()));
    jit.ret();

    hasThrown.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(void*)), sp);
    jit.jumpToStub(handleException(false));

    mustThrow.link(&jit);
    doesNotFit.link(&jit);
    jit.addPtr(TrustedImm32(sizeof(void*)), sp);
#else
    constexpr GPRReg siteReturnAddress = ARM64Registers::x16;
    constexpr GPRReg scratch = ARM64Registers::x17;
    Jump doesNotFit = jit.branchPtr(CCallHelpers::Below, sp, softStackLimitWord);
    jit.pushPair(siteReturnAddress, CCallHelpers::linkRegister);
    callPollSlow(jit);
    constexpr GPRReg exception = siteReturnAddress;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), exception);
    // Both scratch registers are taken below. The loads, the pop and the test of the exception leave the flags alone.
    jit.load8(Address(exception, VM::offsetOfTraps() + VMTraps::offsetOfStackManager() + StackManager::offsetOfStopsAtFunctionEntries()), scratch);
    jit.compareOnFlags32(scratch, TrustedImm32(0));
    jit.loadPtr(Address(exception, VM::exceptionOffset()), exception);
    jit.popPair(CCallHelpers::linkRegister, scratch);
    Jump hasThrown = jit.branchTestPtr(CCallHelpers::NonZero, exception);
    jit.linkToStub(jit.branchOnFlags(CCallHelpers::NotEqual), throwStackOverflow());
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
