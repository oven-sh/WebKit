/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsExceptions.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTRuntimeTable.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTVector.h"
#include "CallFrame.h"
#include "JSCJSValue.h"
#include "RegisterSet.h"
#include "StackAlignment.h"
#include "VM.h"
#include <wtf/MathExtras.h>

#if ENABLE(WEBASSEMBLY)
#include "JSWebAssemblyInstance.h"
#endif

namespace JSC { namespace AOT {

static constexpr GPRReg A0 = gprOf(StubRegister::A0);
static constexpr GPRReg A1 = gprOf(StubRegister::A1);
static constexpr GPRReg A2 = gprOf(StubRegister::A2);
static constexpr GPRReg T9 = gprOf(StubRegister::T9);
static constexpr GPRReg T10 = gprOf(StubRegister::T10);
static constexpr GPRReg T11 = gprOf(StubRegister::T11);
static constexpr GPRReg savedVM = GPRInfo::regCS0;

static_assert(noOverlap(T10, T11, A0, GPRInfo::regT1) && noOverlap(T11, A1));
static_assert(noOverlap(GPRInfo::returnValueGPR, A2));
static_assert(exists(handleException(false)) && exists(handleException(true)) && exists(handleExceptionAtPrologue()) && exists(catchException()) && exists(rejectInFrame()));
static_assert(exists(throwStackOverflow(false)) && exists(throwStackOverflow(true)));
static_assert(descriptorOf(StubFamily::ThrowStackOverflow).numberOfForms == static_cast<unsigned>(StackOverflowForm::AtPrologue) + 1);

// The VM is in T10, the function in T11. Afterwards the VM is in savedVM.
static void callLookupExceptionHandler(StubAssembler& jit)
{
#if ASSERT_ENABLED
    jit.storePtr(GPRInfo::callFrameRegister, Address(T10, VM::offsetOfTopCallFrame()));
#endif
    jit.move(T10, GPRInfo::regT1);
    jit.copyCalleeSavesToVMEntryFrameCalleeSavesBuffer(GPRInfo::regT1);
    jit.move(T10, savedVM);
    jit.move(T10, A0);
    jit.move(instanceGPR, A1);
    jit.call(T11, OperationPtrTag);
}

static void jumpToGenericUnwindTarget(StubAssembler& jit)
{
#if ENABLE(WEBASSEMBLY)
    static_assert(AOT_INSTANCE_OFFSET(vm) == JSWebAssemblyInstance::offsetOfVM());
#endif
    constexpr ptrdiff_t offsetOfInstance = 4 * sizeof(Register);
    static_assert(static_cast<int>(CallFrameSlot::callee) < 4);
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(offsetOfInstance + AOT_INSTANCE_OFFSET(vm) + sizeof(void*))), CCallHelpers::stackPointerRegister);
    jit.move(CCallHelpers::stackPointerRegister, GPRInfo::callFrameRegister);
    jit.addPtr(TrustedImm32(offsetOfInstance), GPRInfo::callFrameRegister, GPRInfo::regT1);
    jit.storePtr(GPRInfo::regT1, CCallHelpers::addressFor(CallFrameSlot::codeBlock));
    jit.storePtr(savedVM, Address(GPRInfo::regT1, AOT_INSTANCE_OFFSET(vm)));
    jit.move(TrustedImm64(JSValue::NativeCalleeTag), GPRInfo::regT1);
    jit.store64(GPRInfo::regT1, CCallHelpers::addressFor(CallFrameSlot::callee));
    jit.loadPtr(Address(savedVM, OBJECT_OFFSETOF(VM, targetMachinePCForThrow)), GPRInfo::regT1);
    jit.farJump(GPRInfo::regT1, ExceptionHandlerPtrTag);
}

// The callee-saved registers are those of the frame of the handler, so the instance is the one that unwinding has left the word in.
static void loadStackPointerOfHandler(StubAssembler& jit)
{
#if CPU(X86_64)
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.exceptions.stackPointerOfHandler)), CCallHelpers::stackPointerRegister);
#else
    // A landing pad takes the stack pointer from the frame pointer.
    UNUSED_PARAM(jit);
#endif
}

void emitUnwind(StubAssembler& jit)
{
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), T10);
    jit.loadOperation(Entry::operationAOTLookupExceptionHandler, T11);
    jit.emitFunctionPrologue();
    callLookupExceptionHandler(jit);
    Jump isNotInCompiledCallers = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR);
    jit.move(GPRInfo::returnValueGPR2, GPRInfo::callFrameRegister);
    jit.restoreCalleeSavesFromVMEntryFrameCalleeSavesBuffer(savedVM, A2);
    loadStackPointerOfHandler(jit);
    jit.farJump(GPRInfo::returnValueGPR, NoPtrTag);
    isNotInCompiledCallers.link(&jit);
    jumpToGenericUnwindTarget(jit);
}

void emitThrowStackOverflow(StubAssembler& jit)
{
    jit.emitFunctionPrologue();
    jit.move(instanceGPR, A0);
    jit.callOperation(Entry::operationAOTThrowStackOverflowError, T9);
    jit.emitFunctionEpilogue();
    jit.jumpToStub(handleException(false));
}

StubSignature signatureOfHandleException(StubKey) { return { }; }
StubSignature signatureOfHandleExceptionPreservingRegisters(StubKey) { return { }; }
StubSignature signatureOfHandleExceptionAtPrologue(StubKey) { return { }; }
StubSignature signatureOfThrowStackOverflow(StubKey) { return { }; }
StubSignature signatureOfCatch(StubKey) { return { }; }
StubSignature signatureOfRejectInFrame(StubKey) { return { }; }

void generateHandleException(StubAssembler& jit)
{
    emitUnwind(jit);
}

void generateHandleExceptionPreservingRegisters(StubAssembler& jit)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
#if CPU(X86_64)
    constexpr GPRReg landingPad = CCallHelpers::s_scratchRegister;
#else
    constexpr GPRReg landingPad = T9;
#endif
    static_assert(noOverlap(landingPad, A2, savedVM));
    Vector<Reg, 48> registers;
    RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()).forEach([&](Reg reg) {
        if (reg != Reg(landingPad))
            registers.append(reg);
    });
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(registers.size() * sizeof(CPURegister))), sp);
    for (unsigned i = 0; i < registers.size(); ++i) {
        if (registers[i].isGPR())
            jit.storePtr(registers[i].gpr(), Address(sp, i * sizeof(CPURegister)));
        else
            jit.storeDouble(registers[i].fpr(), Address(sp, i * sizeof(CPURegister)));
    }
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), T10);
    jit.loadOperation(Entry::operationAOTLookupExceptionHandler, T11);
    callLookupExceptionHandler(jit);
    Jump isNotInCompiledCallers = jit.branchTestPtr(CCallHelpers::Zero, GPRInfo::returnValueGPR);
    jit.move(GPRInfo::returnValueGPR, landingPad);
    jit.move(GPRInfo::returnValueGPR2, GPRInfo::callFrameRegister);
    jit.restoreCalleeSavesFromVMEntryFrameCalleeSavesBuffer(savedVM, A2);
    for (unsigned i = 0; i < registers.size(); ++i) {
        if (registers[i].isGPR())
            jit.loadPtr(Address(sp, i * sizeof(CPURegister)), registers[i].gpr());
        else
            jit.loadDouble(Address(sp, i * sizeof(CPURegister)), registers[i].fpr());
    }
    loadStackPointerOfHandler(jit);
    jit.farJump(landingPad, NoPtrTag);
    isNotInCompiledCallers.link(&jit);
    jumpToGenericUnwindTarget(jit);
}

void generateHandleExceptionAtPrologue(StubAssembler& jit)
{
    jit.emitFunctionEpilogue();
    jit.jumpToStub(handleException(false));
}

void generateThrowStackOverflow(StubAssembler& jit)
{
    if (static_cast<StackOverflowForm>(jit.key().form()) == StackOverflowForm::AtPrologue)
        jit.emitFunctionEpilogue();
    emitThrowStackOverflow(jit);
}

void generateCatch(StubAssembler& jit)
{
    constexpr GPRReg vm = GPRInfo::regT3;
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), vm);
#if ENABLE(WEBASSEMBLY)
    jit.and64(TrustedImm64(JSValue::NativeCalleeMask), vm, GPRInfo::regT0);
    Jump isJSCallee = jit.branch64(CCallHelpers::NotEqual, GPRInfo::regT0, TrustedImm64(JSValue::NativeCalleeTag));
    jit.loadPtr(CCallHelpers::addressFor(CallFrameSlot::codeBlock), vm);
    jit.loadPtr(Address(vm, JSWebAssemblyInstance::offsetOfVM()), vm);
    Jump vmReady = jit.jump();
    isJSCallee.link(&jit);
#endif
    loadCellVM(jit, vm);
#if ENABLE(WEBASSEMBLY)
    vmReady.link(&jit);
#endif

    jit.restoreCalleeSavesFromVMEntryFrameCalleeSavesBuffer(vm, GPRInfo::regT0);
    jit.loadPtr(Address(vm, VM::callFrameForCatchOffset()), GPRInfo::callFrameRegister);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), Address(vm, VM::callFrameForCatchOffset()));
    jit.loadPtr(Address(vm, OBJECT_OFFSETOF(VM, targetMachinePCAfterCatch)), GPRInfo::regT1);
    loadStackPointerOfHandler(jit);
    jit.farJump(GPRInfo::regT1, ExceptionHandlerPtrTag);
}

void generateRejectInFrame(StubAssembler& jit)
{
    jit.move(CCallHelpers::stackPointerRegister, T9);
    jit.andPtr(TrustedImm32(-static_cast<int32_t>(stackAlignmentBytes())), T9);
    jit.move(T9, CCallHelpers::stackPointerRegister);
    jit.move(instanceGPR, A0);
    jit.move(GPRInfo::callFrameRegister, A1);
    jit.callOperation(Entry::operationAOTRejectWithException, T9);
    jit.emitFunctionEpilogue();
    jit.ret();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
