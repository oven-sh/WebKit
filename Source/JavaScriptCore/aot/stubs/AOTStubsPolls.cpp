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

static constexpr GPRReg A0 = gprOf(StubRegister::A0);
static constexpr GPRReg A1 = gprOf(StubRegister::A1);
static constexpr GPRReg T11 = gprOf(StubRegister::T11);
static constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
static constexpr Address softStackLimit { instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit() };

static void callPollSlow(StubAssembler& jit)
{
    jit.preservingCallerSavedRegisters(false, [&](unsigned) {
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTPollSlow, T11);
    });
}

StubSignature signatureOfPollAtPrologue(StubKey) { return { }; }
StubSignature signatureOfPollAtLoop(StubKey) { return { }; }
StubSignature signatureOfPollBeforeEntering(StubKey) { return { }; }

void generatePollAtPrologue(StubAssembler& jit)
{
#if CPU(X86_64)
    // Frame setup clobbers no register here, so every one of them may hold a value of the caller, the macro assembler's scratch register too.
    constexpr GPRReg stackPointerAtSite = X86Registers::eax;
    jit.push(stackPointerAtSite);
    jit.addPtr(TrustedImm32(2 * sizeof(void*)), sp, stackPointerAtSite);
    jit.m_assembler.cmpq_mr(softStackLimit.offset, softStackLimit.base, stackPointerAtSite);
    jit.pop(stackPointerAtSite);
    jit.linkToStub(Jump(jit.m_assembler.jCC(X86Assembler::ConditionB)), throwStackOverflow(true));
#else
    jit.linkToStub(jit.branchPtr(CCallHelpers::Below, sp, softStackLimit), throwStackOverflow(true));
#endif
    jit.preservingCallerSavedRegisters(false, [&](unsigned bytes) {
        jit.loadCallerReturnAddress(bytes, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTPollAtPrologue, T11);
#if CPU(X86_64)
        jit.storePtr(GPRInfo::returnValueGPR, Address(sp, bytes));
#else
        jit.storePtr(GPRInfo::returnValueGPR, Address(sp));
#endif
    });
#if CPU(X86_64)
    jit.linkToStub(jit.branchTestPtr(CCallHelpers::Zero, Address(sp)), handleExceptionAtPrologue());
#else
    jit.linkToStub(jit.branchTestPtr(CCallHelpers::Zero, CCallHelpers::linkRegister), handleExceptionAtPrologue());
#endif
    jit.ret();
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
    Jump doesNotFit = jit.branchPtr(CCallHelpers::Below, scratch, softStackLimit);
    jit.subPtr(TrustedImm32(sizeof(void*)), sp);
    callPollSlow(jit);
    jit.addPtr(TrustedImm32(sizeof(void*)), sp);
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
    Jump doesNotFit = jit.branchPtr(CCallHelpers::Below, sp, softStackLimit);
    jit.pushPair(siteReturnAddress, CCallHelpers::linkRegister);
    callPollSlow(jit);
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
