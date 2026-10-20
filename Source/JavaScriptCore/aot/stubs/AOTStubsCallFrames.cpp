/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCallFrames.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTStubFrames.h"
#include "CallFrame.h"
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

using Address = CCallHelpers::Address;
using Jump = CCallHelpers::Jump;
using Label = CCallHelpers::Label;
using TrustedImm32 = CCallHelpers::TrustedImm32;

static constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
static constexpr GPRReg fp = GPRInfo::callFrameRegister;

#if CPU(X86_64)
static constexpr GPRReg returnAddressGPR = CCallHelpers::s_scratchRegister;

static constexpr GPRReg frameRecordGPR = CCallHelpers::s_scratchRegister;
static constexpr GPRReg stackWordGPR = X86Registers::eax;
static constexpr GPRReg listReturnAddressGPR = X86Registers::r10;
static constexpr FPRReg parkedStackWordFPR = X86Registers::xmm8;
static constexpr FPRReg parkedListReturnAddressFPR = X86Registers::xmm9;

static_assert([] {
    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index) {
        if (directArgumentGPR(index) == frameRecordGPR)
            return false;
    }
    for (unsigned index = 0; index < numberOfDirectArgumentFPRs; ++index) {
        if (directArgumentFPR(index) == parkedStackWordFPR || directArgumentFPR(index) == parkedListReturnAddressFPR)
            return false;
    }
    return noOverlap(frameRecordGPR, stackWordGPR, listReturnAddressGPR, calleeGPR, valueEntryGPR, instanceGPR, cellCageBaseGPR);
}());
#else
static constexpr GPRReg frameRecordGPR = ARM64Registers::x12;
static constexpr GPRReg stackWordGPR = ARM64Registers::x13;
static constexpr GPRReg listReturnAddressGPR = ARM64Registers::x14;

static_assert([] {
    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index) {
        if (!noOverlap(directArgumentGPR(index), frameRecordGPR, stackWordGPR, listReturnAddressGPR))
            return false;
    }
    return noOverlap(frameRecordGPR, stackWordGPR, listReturnAddressGPR, thisGPR, countGPR, calleeGPR, valueEntryGPR, instanceGPR, cellCageBaseGPR);
}());
#endif

static Address argumentListReturnAddress()
{
    return Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(StubFrameKind::ArgumentList));
}

void emitSpillArgumentRegisters(CCallHelpers& jit)
{
#if CPU(X86_64)
    jit.pop(returnAddressGPR);
    for (unsigned i = numberOfArgumentGPRs; i--;)
        jit.push(argumentGPR(i));
    jit.push(returnAddressGPR);
#else
    static_assert(!(numberOfArgumentGPRs % 2));
    jit.subPtr(TrustedImm32(sizeOfSpilledArgumentRegisters), sp);
    for (unsigned i = 0; i < numberOfArgumentGPRs; i += 2)
        jit.storePair64(argumentGPR(i), argumentGPR(i + 1), sp, TrustedImm32(i * sizeOfStackWord));
#endif
}

void emitDropSpilledArgumentRegisters(CCallHelpers& jit)
{
#if CPU(X86_64)
    jit.pop(returnAddressGPR);
    jit.addPtr(TrustedImm32(sizeOfSpilledArgumentRegisters), sp);
    jit.push(returnAddressGPR);
#else
    jit.addPtr(TrustedImm32(sizeOfSpilledArgumentRegisters), sp);
#endif
}

void emitReturnDroppingSpilledArgumentRegisters(CCallHelpers& jit)
{
    emitDropSpilledArgumentRegisters(jit);
    jit.ret();
}

void emitCallFromArgumentListFrame(CCallHelpers& jit, const ScopedLambda<void()>& emitJumpToCallee)
{
#if CPU(ARM64)
    jit.loadPtr(argumentListReturnAddress(), CCallHelpers::linkRegister);
#else
    jit.push(argumentListReturnAddress());
#endif
    emitJumpToCallee();
}

void emitDropArgumentListFrames(CCallHelpers& jit, GPRReg scratch)
{
    ASSERT(scratch != instanceGPR);
    Label again = jit.label();
#if CPU(ARM64)
    jit.loadPtr(argumentListReturnAddress(), scratch);
    Jump isNotArgumentListFrame = jit.branchPtr(CCallHelpers::NotEqual, CCallHelpers::linkRegister, scratch);
#else
    jit.loadPtr(Address(sp), scratch);
    Jump isNotArgumentListFrame = jit.branchPtr(CCallHelpers::NotEqual, scratch, argumentListReturnAddress());
#endif
    jit.emitFunctionEpilogue();
    jit.jump().linkTo(again, &jit);
    isNotArgumentListFrame.link(&jit);
}

static void emitFindListFrameRecord(CCallHelpers& jit, unsigned bytesAboveFrameRecord)
{
    jit.move(fp, frameRecordGPR);
    jit.loadPtr(argumentListReturnAddress(), listReturnAddressGPR);
    jit.loadPtr(Address(frameRecordGPR, CallFrame::returnPCOffset()), stackWordGPR);
    Jump isNotOnListFrame = jit.branchPtr(CCallHelpers::NotEqual, stackWordGPR, listReturnAddressGPR);
    Label nextFrame = jit.label();
    jit.loadPtr(Address(frameRecordGPR, CallFrame::callerFrameOffset()), frameRecordGPR);
    jit.loadPtr(Address(frameRecordGPR, CallFrame::returnPCOffset()), stackWordGPR);
    jit.branchPtr(CCallHelpers::Equal, stackWordGPR, listReturnAddressGPR).linkTo(nextFrame, &jit);
    if (!bytesAboveFrameRecord) {
        isNotOnListFrame.link(&jit);
        return;
    }
    Jump reusesListFrame = jit.jump();

    isNotOnListFrame.link(&jit);
    const GPRReg callerFrame = listReturnAddressGPR;
    jit.loadPtr(Address(frameRecordGPR, CallFrame::callerFrameOffset()), callerFrame);
    jit.addPtr(TrustedImm32(bytesAboveFrameRecord), frameRecordGPR);
    jit.storePtr(callerFrame, Address(frameRecordGPR, CallFrame::callerFrameOffset()));
    jit.storePtr(stackWordGPR, Address(frameRecordGPR, CallFrame::returnPCOffset()));
    reusesListFrame.link(&jit);
}

void emitMoveStackWordsToArgumentListFrame(CCallHelpers& jit, unsigned numberOfWords, unsigned bytesAboveFrameRecord)
{
    ASSERT(!(bytesAboveFrameRecord % stackAlignmentBytes()));
#if CPU(X86_64)
    jit.move64ToDouble(stackWordGPR, parkedStackWordFPR);
    jit.move64ToDouble(listReturnAddressGPR, parkedListReturnAddressFPR);
#endif
    emitFindListFrameRecord(jit, bytesAboveFrameRecord);
    const int32_t size = static_cast<int32_t>(WTF::roundUpToMultipleOf<stackAlignmentBytes()>(numberOfWords * sizeOfStackWord));
    for (unsigned k = numberOfWords; k--;) {
        const int32_t offset = static_cast<int32_t>(k * sizeOfStackWord);
        jit.load64(Address(sp, offset), stackWordGPR);
        jit.store64(stackWordGPR, Address(frameRecordGPR, offset - size));
    }
    jit.move(frameRecordGPR, fp);
    jit.addPtr(TrustedImm32(-size), frameRecordGPR, sp);
#if CPU(X86_64)
    jit.moveDoubleTo64(parkedStackWordFPR, stackWordGPR);
    jit.moveDoubleTo64(parkedListReturnAddressFPR, listReturnAddressGPR);
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
