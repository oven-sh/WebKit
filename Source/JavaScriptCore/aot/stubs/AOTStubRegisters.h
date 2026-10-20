/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubLeafConvention.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include "MacroAssembler.h"
#include "RegisterSet.h"
#include <array>
#include <span>

namespace JSC { namespace AOT {

static constexpr GPRReg instanceGPR = GPRInfo::jitDataRegister;
#if CPU(ARM64)
static constexpr GPRReg cellCageBaseGPR = ARM64Registers::x25;
static constexpr GPRReg throwWordGPR = ARM64Registers::x16;
static constexpr GPRReg faultCallerLinkGPR = ARM64Registers::x16;
#elif CPU(X86_64)
static constexpr GPRReg cellCageBaseGPR = X86Registers::r14;
static constexpr GPRReg throwWordGPR = X86Registers::r11;
static constexpr GPRReg faultCallerLinkGPR = InvalidGPRReg;
#endif

#if CPU(ARM64)
static constexpr unsigned numberOfOperationArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
constexpr GPRReg operationArgumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
#elif CPU(X86_64)
static constexpr GPRReg seventhOperationArgumentGPR = X86Registers::r12;
static constexpr GPRReg eighthOperationArgumentGPR = X86Registers::ebx;
static constexpr unsigned numberOfOperationArgumentGPRs = GPRInfo::numberOfArgumentRegisters + 2;
constexpr GPRReg operationArgumentGPR(unsigned index)
{
    if (index < GPRInfo::numberOfArgumentRegisters)
        return GPRInfo::toArgumentRegister(index);
    return index == GPRInfo::numberOfArgumentRegisters ? seventhOperationArgumentGPR : eighthOperationArgumentGPR;
}
#endif

static constexpr unsigned numberOfLeafArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
constexpr GPRReg leafArgumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }

#if CPU(X86_64)
inline constexpr std::array<GPRReg, 1> unsavedCalleeSaveGPRs { X86Registers::r12 };
inline constexpr std::array<GPRReg, 1> suspensionClobberedGPRs { X86Registers::r15 };
inline constexpr std::array<FPRReg, 0> pinnedFPRsInFrameSavingFunctions { };
#elif CPU(ARM64)
inline constexpr std::array<GPRReg, 0> unsavedCalleeSaveGPRs { };
inline constexpr std::array<GPRReg, 6> suspensionClobberedGPRs { ARM64Registers::x19, ARM64Registers::x20, ARM64Registers::x21, ARM64Registers::x22, ARM64Registers::x23, ARM64Registers::x24 };
inline constexpr std::array<FPRReg, 8> pinnedFPRsInFrameSavingFunctions { ARM64Registers::q8, ARM64Registers::q9, ARM64Registers::q10, ARM64Registers::q11, ARM64Registers::q12, ARM64Registers::q13, ARM64Registers::q14, ARM64Registers::q15 };
static_assert(!(suspensionClobberedGPRs.size() % 2));
#endif

inline RegisterSet unsavedCalleeSaves() { return RegisterSet::fromIterable(unsavedCalleeSaveGPRs); }
inline RegisterSet calleeSavesClobberedAtSuspensions() { return RegisterSet::fromIterable(suspensionClobberedGPRs); }
inline RegisterSet registersClobberedByOperations() { return RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()); }
inline RegisterSet registersClobberedByCompiledCalls() { return registersClobberedByOperations().merge(unsavedCalleeSaves()); }

inline RegisterSet registersClobberedByPreserveMostLeaves()
{
    RegisterSet clobbered = registersClobberedByOperations();
#if AOT_LEAVES_PRESERVE_MOST && CPU(X86_64)
    // preserve_most also saves rax and rdx in a function that returns nothing in them. No stub relies on that.
    clobbered.exclude(RegisterSet(X86Registers::ecx, X86Registers::esi, X86Registers::edi, X86Registers::r8, X86Registers::r9, X86Registers::r10));
#elif AOT_LEAVES_PRESERVE_MOST && CPU(ARM64)
    clobbered.exclude(RegisterSet(ARM64Registers::x9, ARM64Registers::x10, ARM64Registers::x11, ARM64Registers::x12, ARM64Registers::x13, ARM64Registers::x14, ARM64Registers::x15));
#endif
    return clobbered;
}

enum class StubRegister : uint8_t { R0, A0, A1, A2, A3, A4, A5, A6, A7, T9, T10, T11, T12, T13, T14, T15, EntryT12, EntryT13, Operation };

constexpr GPRReg gprOf(StubRegister reg)
{
    switch (reg) {
#if CPU(X86_64)
    case StubRegister::R0:
    case StubRegister::Operation:
        return X86Registers::eax;
    case StubRegister::A0:
    case StubRegister::T12:
        return X86Registers::edi;
    case StubRegister::A1:
        return X86Registers::esi;
    case StubRegister::A2:
    case StubRegister::EntryT13:
        return X86Registers::edx;
    case StubRegister::A3:
    case StubRegister::T14:
    case StubRegister::EntryT12:
        return X86Registers::ecx;
    case StubRegister::A4:
    case StubRegister::T10:
        return X86Registers::r8;
    case StubRegister::A5:
    case StubRegister::T11:
        return X86Registers::r9;
    case StubRegister::A6:
    case StubRegister::A7:
        return InvalidGPRReg;
    case StubRegister::T9:
    case StubRegister::T15:
        return X86Registers::r10;
    case StubRegister::T13:
        return X86Registers::r12;
#elif CPU(ARM64)
    case StubRegister::R0:
    case StubRegister::A0:
        return ARM64Registers::x0;
    case StubRegister::A1:
        return ARM64Registers::x1;
    case StubRegister::A2:
        return ARM64Registers::x2;
    case StubRegister::A3:
        return ARM64Registers::x3;
    case StubRegister::A4:
        return ARM64Registers::x4;
    case StubRegister::A5:
        return ARM64Registers::x5;
    case StubRegister::A6:
        return ARM64Registers::x6;
    case StubRegister::A7:
        return ARM64Registers::x7;
    case StubRegister::T9:
        return ARM64Registers::x9;
    case StubRegister::T10:
        return ARM64Registers::x10;
    case StubRegister::T11:
    case StubRegister::Operation:
        return ARM64Registers::x11;
    case StubRegister::T12:
    case StubRegister::EntryT12:
        return ARM64Registers::x12;
    case StubRegister::T13:
    case StubRegister::EntryT13:
        return ARM64Registers::x13;
    case StubRegister::T14:
        return ARM64Registers::x14;
    case StubRegister::T15:
        return ARM64Registers::x15;
#endif
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

#if CPU(X86_64)
inline constexpr GPRReg assertionScratchGPR = MacroAssembler::s_scratchRegister;
inline constexpr GPRReg preservingCallImmediateGPR = MacroAssembler::s_scratchRegister;
inline constexpr GPRReg leafFunctionGPR = MacroAssembler::s_scratchRegister;
#elif CPU(ARM64)
inline constexpr GPRReg assertionScratchGPR = ARM64Registers::x12;
inline constexpr GPRReg preservingCallImmediateGPR = ARM64Registers::x9;
inline constexpr GPRReg leafFunctionGPR = ARM64Registers::x16;
#endif

constexpr std::span<const GPRReg> alternateReceiverGPRs()
{
#if CPU(X86_64)
    static constexpr GPRReg registers[] = { X86Registers::ecx, X86Registers::edx, X86Registers::ebx, X86Registers::eax, X86Registers::edi, X86Registers::r8, X86Registers::r15 };
    return registers;
#elif CPU(ARM64)
    return { };
#endif
}

inline constexpr StubRegister firstStubOperandRegister = StubRegister::R0;
inline constexpr StubRegister stubImmediateRegister = StubRegister::T9;
#if CPU(X86_64)
inline constexpr StubRegister copyEndOffsetRegister = StubRegister::T10;
#elif CPU(ARM64)
inline constexpr StubRegister copyEndOffsetRegister = StubRegister::T14;
#endif

static_assert(gprOf(StubRegister::R0) == GPRInfo::returnValueGPR);
static_assert(gprOf(StubRegister::A0) == GPRInfo::argumentGPR0 && gprOf(StubRegister::A1) == GPRInfo::argumentGPR1 && gprOf(StubRegister::A2) == GPRInfo::argumentGPR2);
static_assert(gprOf(StubRegister::A3) == GPRInfo::argumentGPR3 && gprOf(StubRegister::A4) == GPRInfo::argumentGPR4 && gprOf(StubRegister::A5) == GPRInfo::argumentGPR5);
#if CPU(ARM64)
static_assert(gprOf(StubRegister::A6) == GPRInfo::argumentGPR6 && gprOf(StubRegister::A7) == GPRInfo::argumentGPR7);
static_assert(assertionScratchGPR == gprOf(StubRegister::T12) && preservingCallImmediateGPR == gprOf(stubImmediateRegister));
static_assert(faultCallerLinkGPR == MacroAssembler::dataTempRegister && leafFunctionGPR == MacroAssembler::dataTempRegister);
#endif
static_assert([] {
    for (unsigned index = 0; index < numberOfOperationArgumentGPRs; ++index) {
        if (!noOverlap(operationArgumentGPR(index), gprOf(StubRegister::Operation), gprOf(stubImmediateRegister)))
            return false;
    }
    return true;
}());
static_assert([] {
    for (unsigned index = 0; index < numberOfLeafArgumentGPRs; ++index) {
        if (leafArgumentGPR(index) == leafFunctionGPR)
            return false;
    }
    return true;
}());

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
