/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubFrames.h"
#include "CCallHelpers.h"

namespace JSC { namespace AOT {

// With ENABLE(RETURN_ADDRESS_SIGNING) an address that code returns to or resumes at is signed anywhere but in the link register.
// Unless it says otherwise, what follows loses the scratch registers of the assembler.

CCallHelpers::Jump emitBranchOnCallerReturnAddress(CCallHelpers&, CCallHelpers::RelationalCondition, StubFrameKind, GPRReg scratch);
CCallHelpers::Jump emitBranchOnReturnAddressOfFrameRecord(CCallHelpers&, CCallHelpers::RelationalCondition, GPRReg record, StubFrameKind, GPRReg scratch);

class HeldReturnAddress {
public:
    static HeldReturnAddress loadLosingLinkRegister(CCallHelpers&, GPRReg record, GPRReg scratch);
#if CPU(ARM64)
    static HeldReturnAddress inLinkRegister() { return HeldReturnAddress(CCallHelpers::linkRegister); }
#endif

    CCallHelpers::Jump branch(CCallHelpers&, CCallHelpers::RelationalCondition, StubFrameKind) const;
    void storeInFrameRecord(CCallHelpers&, GPRReg record) const;

private:
    explicit HeldReturnAddress(GPRReg gpr)
        : m_gpr(gpr)
    {
    }

    GPRReg m_gpr;
};

class ReturnAddressToLookAt {
public:
    static ReturnAddressToLookAt load(CCallHelpers&, GPRReg record, GPRReg result);
#if CPU(X86_64)
    static ReturnAddressToLookAt alreadyIn(GPRReg gpr) { return ReturnAddressToLookAt(gpr); }
#endif

    GPRReg gpr() const { return m_gpr; }
    CCallHelpers::Address at(int32_t offset = 0) const { return CCallHelpers::Address(m_gpr, offset + distanceFromRegister); }
#if ENABLE(RETURN_ADDRESS_SIGNING)
    static constexpr int32_t distanceFromRegister = ARM64Assembler::distanceInFrontOfAddressToLookAt;
#else
    static constexpr int32_t distanceFromRegister = 0;
#endif

private:
    explicit ReturnAddressToLookAt(GPRReg gpr)
        : m_gpr(gpr)
    {
    }

    GPRReg m_gpr;
};

// Answers with the register that holds the address.
GPRReg emitLoadResumeAddressLosingLinkRegister(CCallHelpers&, GPRReg savedFrame, GPRReg scratch);

#if ENABLE(RETURN_ADDRESS_SIGNING)
void emitJumpToResumeAddress(CCallHelpers&, GPRReg asStored, GPRReg savedFrame);
void emitStoreLinkRegisterAsResumeAddress(CCallHelpers&, GPRReg savedFrame);
// In front of MacroAssembler::moveSignedPCRelativeAddress().
void emitModifierOfResumeAddress(CCallHelpers&, GPRReg savedFrame);
#else
CCallHelpers::Address returnAddressOfStubWithoutSignature(StubFrameKind);
ptrdiff_t offsetOfResumeAddressWithoutSignature();
#endif

// These lose no register.
void emitLoadResumeAddressAsStored(CCallHelpers&, GPRReg savedFrame, GPRReg result);
void emitStoreResumeAddressThatCallerHasSigned(CCallHelpers&, GPRReg signedOrNull, GPRReg savedFrame);
void emitClearResumeAddressAndStorePromiseOrWaiter(CCallHelpers&, GPRReg promiseOrWaiter, GPRReg savedFrame);

#if CPU(ARM64)
// The upper word of a pair takes the fewest instructions to sign.
inline void emitPushLinkRegister(CCallHelpers& jit) { jit.pushPair(ARM64Registers::zr, CCallHelpers::linkRegister); }
inline void emitPopLinkRegister(CCallHelpers& jit) { jit.popPair(ARM64Registers::zr, CCallHelpers::linkRegister); }
void emitPopLinkRegisterAndReturnToOldOne(CCallHelpers&);
void emitPopFrameRecordAndReturnToOldLinkRegister(CCallHelpers&);
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
