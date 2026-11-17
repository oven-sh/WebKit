/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSavedCodeAddresses.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTSavedFrame.h"
#include "AOTStubWords.h"
#include "CallFrame.h"

namespace JSC { namespace AOT {

struct ResumeAddressWord {
    static constexpr ptrdiff_t offset = SavedFrame::offsetOfResumeAddress();
    static CCallHelpers::Address in(GPRReg savedFrame) { return CCallHelpers::Address(savedFrame, offset); }
};
static_assert(SavedFrame::offsetOfPromiseOrWaiter() == ResumeAddressWord::offset + sizeof(void*));

struct ReturnAddressWord {
    static CCallHelpers::Address in(GPRReg record) { return CCallHelpers::Address(record, CallFrame::returnPCOffset()); }
    static CCallHelpers::Address ofStub(StubFrameKind kind) { return CCallHelpers::Address(instanceGPR, AOT_INSTANCE_OFFSET(stubs) + StubWords::offsetOfReturnAddress(kind)); }
};

CCallHelpers::Jump emitBranchOnCallerReturnAddress(CCallHelpers& jit, CCallHelpers::RelationalCondition condition, StubFrameKind kind, GPRReg scratch)
{
#if CPU(ARM64)
    jit.loadPtr(ReturnAddressWord::ofStub(kind), scratch);
    return jit.branchPtr(condition, CCallHelpers::linkRegister, scratch);
#else
    jit.loadPtr(CCallHelpers::Address(CCallHelpers::stackPointerRegister), scratch);
    return jit.branchPtr(condition, scratch, ReturnAddressWord::ofStub(kind));
#endif
}

CCallHelpers::Jump HeldReturnAddress::branch(CCallHelpers& jit, CCallHelpers::RelationalCondition condition, StubFrameKind kind) const
{
    return jit.branchPtr(condition, m_gpr, ReturnAddressWord::ofStub(kind));
}

#if ENABLE(RETURN_ADDRESS_SIGNING)

static constexpr GPRReg modifierGPR = CCallHelpers::dataTempRegister;
static constexpr GPRReg pointerGPR = CCallHelpers::memoryTempRegister;

static void emitAddressInModifier(CCallHelpers& jit, GPRReg base, int32_t offset)
{
    (void)jit.getCachedMemoryTempRegisterIDAndInvalidate();
    jit.addPtr(CCallHelpers::TrustedImm32(offset), base, jit.getCachedDataTempRegisterIDAndInvalidate());
}

static CCallHelpers::Address emitModifierOfReturnAddress(CCallHelpers& jit, GPRReg record)
{
    emitAddressInModifier(jit, record, sizeof(CallerFrameAndPC));
    return CCallHelpers::Address(modifierGPR, -static_cast<int32_t>(sizeof(void*)));
}

static void emitDiscriminatorOfResumeAddress(CCallHelpers& jit)
{
    jit.m_assembler.movk<64>(modifierGPR, SavedFrame::discriminatorOfResumeAddress, SavedFrame::discriminatorShift);
}

void emitModifierOfResumeAddress(CCallHelpers& jit, GPRReg savedFrame)
{
    emitAddressInModifier(jit, savedFrame, ResumeAddressWord::offset);
    emitDiscriminatorOfResumeAddress(jit);
}

CCallHelpers::Jump emitBranchOnReturnAddressOfFrameRecord(CCallHelpers& jit, CCallHelpers::RelationalCondition condition, GPRReg record, StubFrameKind kind, GPRReg scratch)
{
    RELEASE_ASSERT(noOverlap(scratch, modifierGPR, pointerGPR));
    CCallHelpers::Address word = emitModifierOfReturnAddress(jit, record);
    jit.loadPtr(ReturnAddressWord::ofStub(kind), scratch);
    jit.loadPtr(word, pointerGPR);
    jit.m_assembler.authenticateX17AndCompare(scratch);
    return jit.branchOnFlags(condition);
}

HeldReturnAddress HeldReturnAddress::loadLosingLinkRegister(CCallHelpers& jit, GPRReg record, GPRReg)
{
    jit.loadPtr(emitModifierOfReturnAddress(jit, record), pointerGPR);
    jit.m_assembler.authenticateX17IntoLinkRegister();
    return inLinkRegister();
}

void HeldReturnAddress::storeInFrameRecord(CCallHelpers& jit, GPRReg record) const
{
    CCallHelpers::Address word = emitModifierOfReturnAddress(jit, record);
    jit.m_assembler.signLinkRegisterInX17();
    jit.storePtr(pointerGPR, word);
}

ReturnAddressToLookAt ReturnAddressToLookAt::load(CCallHelpers& jit, GPRReg record, GPRReg result)
{
    jit.loadPtr(emitModifierOfReturnAddress(jit, record), pointerGPR);
    jit.m_assembler.authenticateX17ToLookAt(result);
    return ReturnAddressToLookAt(result);
}

GPRReg emitLoadResumeAddressLosingLinkRegister(CCallHelpers& jit, GPRReg savedFrame, GPRReg)
{
    emitAddressInModifier(jit, savedFrame, ResumeAddressWord::offset);
    jit.loadPtr(CCallHelpers::Address(modifierGPR), pointerGPR);
    emitDiscriminatorOfResumeAddress(jit);
    jit.m_assembler.authenticateX17IntoLinkRegister();
    return CCallHelpers::linkRegister;
}

void emitJumpToResumeAddress(CCallHelpers& jit, GPRReg asStored, GPRReg savedFrame)
{
    RELEASE_ASSERT(asStored != modifierGPR);
    emitModifierOfResumeAddress(jit, savedFrame);
    jit.move(asStored, pointerGPR);
    jit.m_assembler.authenticateX17AndJump();
}

void emitStoreLinkRegisterAsResumeAddress(CCallHelpers& jit, GPRReg savedFrame)
{
    RELEASE_ASSERT(noOverlap(savedFrame, modifierGPR, pointerGPR));
    emitModifierOfResumeAddress(jit, savedFrame);
    jit.m_assembler.signLinkRegisterInX17();
    emitStoreResumeAddressThatCallerHasSigned(jit, pointerGPR, savedFrame);
}

void emitPopLinkRegisterAndReturnToOldOne(CCallHelpers& jit)
{
    (void)jit.getCachedMemoryTempRegisterIDAndInvalidate();
    jit.m_assembler.popLinkRegisterAndReturnToOldOne(ARM64Assembler::FirstOfPoppedPair::Nothing);
}

void emitPopFrameRecordAndReturnToOldLinkRegister(CCallHelpers& jit)
{
    (void)jit.getCachedMemoryTempRegisterIDAndInvalidate();
    jit.m_assembler.popLinkRegisterAndReturnToOldOne(ARM64Assembler::FirstOfPoppedPair::FramePointer);
}

#else

CCallHelpers::Jump emitBranchOnReturnAddressOfFrameRecord(CCallHelpers& jit, CCallHelpers::RelationalCondition condition, GPRReg record, StubFrameKind kind, GPRReg scratch)
{
    return HeldReturnAddress::loadLosingLinkRegister(jit, record, scratch).branch(jit, condition, kind);
}

HeldReturnAddress HeldReturnAddress::loadLosingLinkRegister(CCallHelpers& jit, GPRReg record, GPRReg scratch)
{
    jit.loadPtr(ReturnAddressWord::in(record), scratch);
    return HeldReturnAddress(scratch);
}

void HeldReturnAddress::storeInFrameRecord(CCallHelpers& jit, GPRReg record) const
{
    jit.storePtr(m_gpr, ReturnAddressWord::in(record));
}

ReturnAddressToLookAt ReturnAddressToLookAt::load(CCallHelpers& jit, GPRReg record, GPRReg result)
{
    jit.loadPtr(ReturnAddressWord::in(record), result);
    return ReturnAddressToLookAt(result);
}

GPRReg emitLoadResumeAddressLosingLinkRegister(CCallHelpers& jit, GPRReg savedFrame, GPRReg scratch)
{
    emitLoadResumeAddressAsStored(jit, savedFrame, scratch);
    return scratch;
}

CCallHelpers::Address returnAddressOfStubWithoutSignature(StubFrameKind kind)
{
    return ReturnAddressWord::ofStub(kind);
}

ptrdiff_t offsetOfResumeAddressWithoutSignature()
{
    return ResumeAddressWord::offset;
}

#if CPU(ARM64)
static void emitPopPairAndReturnToOldLinkRegister(CCallHelpers& jit, GPRReg first)
{
    GPRReg returnAddress = jit.getCachedMemoryTempRegisterIDAndInvalidate();
    jit.move(CCallHelpers::linkRegister, returnAddress);
    jit.popPair(first, CCallHelpers::linkRegister);
    jit.m_assembler.ret(returnAddress);
}

void emitPopLinkRegisterAndReturnToOldOne(CCallHelpers& jit)
{
    emitPopPairAndReturnToOldLinkRegister(jit, ARM64Registers::zr);
}

void emitPopFrameRecordAndReturnToOldLinkRegister(CCallHelpers& jit)
{
    emitPopPairAndReturnToOldLinkRegister(jit, GPRInfo::callFrameRegister);
}
#endif

#endif

void emitLoadResumeAddressAsStored(CCallHelpers& jit, GPRReg savedFrame, GPRReg result)
{
    jit.loadPtr(ResumeAddressWord::in(savedFrame), result);
}

void emitStoreResumeAddressThatCallerHasSigned(CCallHelpers& jit, GPRReg signedOrNull, GPRReg savedFrame)
{
    jit.store64(signedOrNull, ResumeAddressWord::in(savedFrame));
}

void emitClearResumeAddressAndStorePromiseOrWaiter(CCallHelpers& jit, GPRReg promiseOrWaiter, GPRReg savedFrame)
{
#if CPU(ARM64)
    jit.storePair64(ARM64Registers::zr, promiseOrWaiter, savedFrame, CCallHelpers::TrustedImm32(ResumeAddressWord::offset));
#else
    jit.store64(CCallHelpers::TrustedImm32(0), ResumeAddressWord::in(savedFrame));
    jit.store64(promiseOrWaiter, CCallHelpers::Address(savedFrame, SavedFrame::offsetOfPromiseOrWaiter()));
#endif
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
