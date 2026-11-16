/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSavedCodeAddresses.h"

#if ENABLE(AOT)

#include "AOTSavedFrame.h"
#include "CallFrame.h"

namespace JSC { namespace AOT {

#if ENABLE(RETURN_ADDRESS_SIGNING)

static constexpr GPRReg modifierOfSavedAddressGPR = CCallHelpers::dataTempRegister;
static constexpr GPRReg savedAddressGPR = CCallHelpers::memoryTempRegister;

static void emitModifierOfReturnAddress(CCallHelpers& jit, GPRReg record)
{
    jit.addPtr(CCallHelpers::TrustedImm32(sizeof(CallerFrameAndPC)), record, modifierOfSavedAddressGPR);
}

static void emitModifierOfResumeAddress(CCallHelpers& jit, GPRReg savedFrame)
{
    jit.addPtr(CCallHelpers::TrustedImm32(SavedFrame::offsetOfResumeAddress()), savedFrame, modifierOfSavedAddressGPR);
    jit.m_assembler.movk<64>(modifierOfSavedAddressGPR, SavedFrame::discriminatorOfResumeAddress, SavedFrame::discriminatorShift);
}

static void emitLoadSavedAddress(CCallHelpers& jit, CCallHelpers::Address word, GPRReg result)
{
    RELEASE_ASSERT(noOverlap(word.base, modifierOfSavedAddressGPR, savedAddressGPR));
    jit.loadPtr(word, savedAddressGPR);
    jit.untagPtr(modifierOfSavedAddressGPR, savedAddressGPR);
    jit.validateUntaggedPtr(savedAddressGPR);
    jit.move(savedAddressGPR, result);
}

static void emitSignSavedAddress(CCallHelpers& jit, GPRReg address)
{
    RELEASE_ASSERT(address != modifierOfSavedAddressGPR);
    jit.move(address, savedAddressGPR);
    jit.tagPtr(modifierOfSavedAddressGPR, savedAddressGPR);
}

static void emitStoreSavedAddress(CCallHelpers& jit, GPRReg address, CCallHelpers::Address word)
{
    RELEASE_ASSERT(noOverlap(word.base, modifierOfSavedAddressGPR, savedAddressGPR));
    emitSignSavedAddress(jit, address);
    jit.storePtr(savedAddressGPR, word);
}

void emitSignResumeAddress(CCallHelpers& jit, GPRReg resumeAddress, GPRReg savedFrame, GPRReg result)
{
    emitModifierOfResumeAddress(jit, savedFrame);
    emitSignSavedAddress(jit, resumeAddress);
    jit.move(savedAddressGPR, result);
}

#else

static void emitModifierOfReturnAddress(CCallHelpers&, GPRReg) { }
static void emitModifierOfResumeAddress(CCallHelpers&, GPRReg) { }

static void emitLoadSavedAddress(CCallHelpers& jit, CCallHelpers::Address word, GPRReg result)
{
    jit.loadPtr(word, result);
}

static void emitStoreSavedAddress(CCallHelpers& jit, GPRReg address, CCallHelpers::Address word)
{
    jit.storePtr(address, word);
}

#endif

void emitLoadReturnAddressOfFrameRecord(CCallHelpers& jit, GPRReg record, GPRReg result)
{
    emitModifierOfReturnAddress(jit, record);
    emitLoadSavedAddress(jit, CCallHelpers::Address(record, CallFrame::returnPCOffset()), result);
}

void emitStoreReturnAddressInFrameRecord(CCallHelpers& jit, GPRReg returnAddress, GPRReg record)
{
    emitModifierOfReturnAddress(jit, record);
    emitStoreSavedAddress(jit, returnAddress, CCallHelpers::Address(record, CallFrame::returnPCOffset()));
}

void emitLoadResumeAddress(CCallHelpers& jit, GPRReg savedFrame, GPRReg result)
{
    emitModifierOfResumeAddress(jit, savedFrame);
    emitLoadSavedAddress(jit, CCallHelpers::Address(savedFrame, SavedFrame::offsetOfResumeAddress()), result);
}

void emitStoreResumeAddress(CCallHelpers& jit, GPRReg resumeAddress, GPRReg savedFrame)
{
    emitModifierOfResumeAddress(jit, savedFrame);
    emitStoreSavedAddress(jit, resumeAddress, CCallHelpers::Address(savedFrame, SavedFrame::offsetOfResumeAddress()));
}

#if CPU(ARM64)
void emitPushLinkRegister(CCallHelpers& jit)
{
    jit.tagReturnAddress();
    jit.pushToSave(CCallHelpers::linkRegister);
}

void emitPopLinkRegister(CCallHelpers& jit, LoadedLinkRegister loadedLinkRegister)
{
    jit.popToRestore(CCallHelpers::linkRegister);
    jit.untagAfterLoadingReturnAddress(loadedLinkRegister);
}
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
