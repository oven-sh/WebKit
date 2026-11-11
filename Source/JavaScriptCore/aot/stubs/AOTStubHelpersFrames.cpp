/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubHelpers.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTStubRegisters.h"
#include "AOTStubsCallLists.h"
#include "VM.h"

namespace JSC { namespace AOT {

static_assert(hostCallFrameSize(0) == 48 && hostCallFrameSize(1) == 64 && hostCallFrameSize(2) == 64 && hostCallFrameSize(8) == 112);

Address hostCallFrameSlot(CallFrameSlot slot, ptrdiff_t offset)
{
    return Address(sp, static_cast<int>(slot) * static_cast<int>(sizeof(Register)) + offset);
}

Address outgoingFrameSlot(CallFrameSlot slot, ptrdiff_t offset)
{
    return Address(sp, (static_cast<int>(slot) - CallerFrameAndPC::sizeInRegisters) * static_cast<int>(sizeof(Register)) + offset);
}

Address incomingFrameSlot(CallFrameSlot slot, ptrdiff_t offset)
{
    return Address(sp, static_cast<int>(slot) * static_cast<int>(sizeof(Register)) - static_cast<int>(prologueStackPointerDelta()) + offset);
}

static Address hostCallFrameArgument(unsigned index)
{
    return hostCallFrameSlot(CallFrameSlot::firstArgument, index * sizeof(Register));
}

static void emitFrameHeader(StubAssembler& jit, const HostCallFrame& frame, GPRReg scratch)
{
    jit.storePtr(fp, Address(sp, CallFrame::callerFrameOffset()));
    Label returnPC = jit.bindReturnLabel(StubFrameKind::HostCall);
    jit.loadLabelAddress(returnPC, scratch);
    jit.storePtr(scratch, Address(sp, CallFrame::returnPCOffset()));
    jit.store64(TrustedImm32(0), hostCallFrameSlot(CallFrameSlot::codeBlock));
    jit.store64(frame.callee, hostCallFrameSlot(CallFrameSlot::callee));
}

static void emitConstantCountFrame(StubAssembler& jit, const HostCallFrame& frame, unsigned count, GPRReg scratch)
{
    jit.subPtr(TrustedImm32(hostCallFrameSize(count)), sp);
    emitFrameHeader(jit, frame, scratch);
    jit.store64(TrustedImm32(count + 1), hostCallFrameSlot(CallFrameSlot::argumentCountIncludingThis));
    jit.store64(frame.thisValue, hostCallFrameSlot(CallFrameSlot::thisArgument));

    if (frame.arguments) {
        for (unsigned i = 0; i < count; ++i) {
            jit.load64(Address(*frame.arguments, i * sizeof(Register)), scratch);
            jit.store64(scratch, hostCallFrameArgument(i));
        }
        return;
    }
    RELEASE_ASSERT(count <= numberOfArgumentGPRs);
    for (unsigned i = 0; i < count; ++i) {
        RELEASE_ASSERT(argumentGPR(i) != scratch);
        jit.store64(argumentGPR(i), hostCallFrameArgument(i));
    }
}

static void emitRegisterCountFrame(StubAssembler& jit, const HostCallFrame& frame, GPRReg count, GPRReg arguments, GPRReg scratch)
{
    const GPRReg copied = jit.reuseScratch(StubRegister::A0);
    RELEASE_ASSERT(noOverlap(scratch, copied, count, arguments, frame.callee) && noOverlap(copied, count, frame.thisValue));

    static_assert(stackAlignmentRegisters() == 2);
    jit.add32(TrustedImm32(CallFrame::headerSizeInRegisters + 1 + 1), count, scratch);
    jit.and32(TrustedImm32(~1), scratch);
    jit.lshiftPtr(TrustedImm32(3), scratch);
    jit.subPtr(scratch, sp);
    emitFrameHeader(jit, frame, scratch);
    jit.add32(TrustedImm32(1), count, scratch);
    jit.store64(scratch, hostCallFrameSlot(CallFrameSlot::argumentCountIncludingThis));
    jit.store64(frame.thisValue, hostCallFrameSlot(CallFrameSlot::thisArgument));

    Jump hasNoArguments = jit.branchTest32(CCallHelpers::Zero, count);
    jit.zeroExtend32ToWord(count, count);
    jit.addPtr(TrustedImm32(hostCallFrameArgument(0).offset), sp, scratch);
    Label copyLoop = jit.label();
    jit.sub32(TrustedImm32(1), count);
    jit.load64(BaseIndex(arguments, count, CCallHelpers::TimesEight), copied);
    jit.store64(copied, BaseIndex(scratch, count, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, count).linkTo(copyLoop, &jit);
    hasNoArguments.link(&jit);
}

void emitHostCallFrame(StubAssembler& jit, const HostCallFrame& frame)
{
    RELEASE_ASSERT(frame.arguments != InvalidGPRReg);
    const GPRReg scratch = jit.reuseScratch(StubRegister::T11);
    RELEASE_ASSERT(noOverlap(scratch, frame.callee, frame.arguments.value_or(InvalidGPRReg), sp) && scratch != frame.thisValue);

    if (const GPRReg* count = std::get_if<GPRReg>(&frame.argumentCount)) {
        RELEASE_ASSERT(frame.arguments);
        emitRegisterCountFrame(jit, frame, *count, *frame.arguments, scratch);
        return;
    }
    emitConstantCountFrame(jit, frame, std::get<unsigned>(frame.argumentCount), scratch);
}

void emitHostCallFrameOfCallWithCount(StubAssembler& jit, GPRReg callee, GPRReg thisValue, GPRReg count, JumpList& doesNotFitOnStack)
{
    const GPRReg scratch = jit.reuseScratch(StubRegister::T11);
    const GPRReg copied = jit.reuseScratch(StubRegister::A0);
    RELEASE_ASSERT(noOverlap(scratch, copied, count, callee, sp, fp) && noOverlap(scratch, copied, count, thisValue) && copied == argumentGPR(0));

    emitAlignedSizeOfWords(jit, count, CallFrame::headerSizeInRegisters + 1 + numberOfArgumentGPRs, scratch);
    jit.subPtr(sp, scratch, scratch);
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Above, softStackLimit(), scratch));
    doesNotFitOnStack.append(jit.branchPtr(CCallHelpers::Above, scratch, fp));
    jit.move(scratch, sp);
    emitFrameHeader(jit, { callee, thisValue, count, std::nullopt }, scratch);
    jit.add32(TrustedImm32(1), count, scratch);
    jit.store64(scratch, hostCallFrameSlot(CallFrameSlot::argumentCountIncludingThis));
    jit.store64(thisValue, hostCallFrameSlot(CallFrameSlot::thisArgument));
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
        jit.store64(argumentGPR(i), hostCallFrameArgument(i));
    Jump hasNoStackWords = jit.branch32(CCallHelpers::BelowOrEqual, count, TrustedImm32(numberOfArgumentGPRs));
    jit.sub32(TrustedImm32(numberOfArgumentGPRs), count);
    jit.zeroExtend32ToWord(count, count);
    jit.addPtr(TrustedImm32(hostCallFrameArgument(numberOfArgumentGPRs).offset), sp, scratch);
    emitCopyFromLastWord(jit, Address(fp, static_cast<int32_t>(offsetOfStackWordsFromFrame)), Address(scratch), count, copied);
    hasNoStackWords.link(&jit);
}

void callNativeFunction(StubAssembler& jit, CodeSpecializationKind, Address function, GPRReg)
{
    jit.call(function, HostFunctionPtrTag);
}

void callNativeFunctionAndReturn(StubAssembler& jit, CodeSpecializationKind kind, GPRReg scratch, Address realm, Address function)
{
    const GPRReg realmArgument = jit.reuseScratch(StubRegister::A0);
    const GPRReg callFrameArgument = jit.reuseScratch(StubRegister::A1);
    RELEASE_ASSERT(noOverlap(realm.base, realmArgument, callFrameArgument, scratch) && noOverlap(function.base, realmArgument, callFrameArgument, scratch));
    RELEASE_ASSERT(scratch != GPRInfo::returnValueGPR);

    jit.move(sp, callFrameArgument);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
    jit.storePtr(callFrameArgument, Address(scratch, VM::offsetOfTopCallFrame()));
    jit.loadPtr(realm, realmArgument);
    callNativeFunction(jit, kind, function, scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), scratch);
    Jump exception = jit.branchTestPtr(CCallHelpers::NonZero, Address(scratch, VM::exceptionOffset()));
    jit.emitFunctionEpilogue();
    jit.pollBeforeReturningFromNativeCode();
    jit.returnToCaller();

    exception.link(&jit);
    jit.emitFunctionEpilogue();
    jit.jumpToExceptionHandler();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
