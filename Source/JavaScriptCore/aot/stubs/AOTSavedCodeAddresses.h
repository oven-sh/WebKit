/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CCallHelpers.h"

namespace JSC { namespace AOT {

// With ENABLE(RETURN_ADDRESS_SIGNING) an address that code returns to or resumes at is signed where it lies in memory, and never in
// a register. These take it from one to the other, and lose the scratch registers of the assembler.
void emitLoadReturnAddressOfFrameRecord(CCallHelpers&, GPRReg record, GPRReg result);
void emitStoreReturnAddressInFrameRecord(CCallHelpers&, GPRReg returnAddress, GPRReg record);
void emitLoadResumeAddress(CCallHelpers&, GPRReg savedFrame, GPRReg result);
void emitStoreResumeAddress(CCallHelpers&, GPRReg resumeAddress, GPRReg savedFrame);
#if ENABLE(RETURN_ADDRESS_SIGNING)
// For an operation that stores it.
void emitSignResumeAddress(CCallHelpers&, GPRReg resumeAddress, GPRReg savedFrame, GPRReg result);
#else
ptrdiff_t offsetOfResumeAddressWithoutSignature();
#endif

// These lose no register.
void emitStoreResumeAddressThatCallerHasSigned(CCallHelpers&, GPRReg signedOrNull, GPRReg savedFrame);
void emitClearResumeAddressAndStorePromiseOrWaiter(CCallHelpers&, GPRReg promiseOrWaiter, GPRReg savedFrame);
// At a label whose address goes to memory without a signature. aot/README.md lists them.
void emitTargetOfUnsignedJump(CCallHelpers&);
static constexpr unsigned hintAtTargetOfUnsignedJump = 38; // BTI JC
#if CPU(ARM64)
// The link register is dead in between.
void emitPushLinkRegister(CCallHelpers&);
void emitPopLinkRegister(CCallHelpers&, LoadedLinkRegister = LoadedLinkRegister::MayBeSavedAgain);
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
