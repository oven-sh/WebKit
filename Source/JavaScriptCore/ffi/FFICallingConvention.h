/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

#include "FFIType.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include "JSExportMacros.h"
#include <array>
#include <span>
#include <wtf/Vector.h>

namespace JSC::FFI {

class Signature;

// The native calling conventions bun:ffi knows how to lay out. JSC's own
// JIT <-> C++ boundary is always SysV (SYSV_ABI on Windows x86-64), so a
// NativeCC only ever describes the FFI callee (invoke thunk) or the callback
// caller (callback entry thunk) boundary.
enum class NativeCC : uint8_t { SysV64, AAPCS64, Win64 };

// How arguments that overflow the argument registers are packed on the
// stack. Every supported ABI uses one 8-byte slot per argument except
// Apple arm64, which packs stack arguments with their natural size and
// alignment (SPEC section 7.1.1).
// FFI-SPEC-GAP: the spec only exposes the packing mode implicitly through
// hostNativeCC() + OS(DARWIN); it is surfaced here as an explicit enum plus
// a computeCallLayout overload so row T's golden tables can exercise both
// AAPCS64 packings from a single host, while the spec's three-argument
// computeCallLayout keeps its exact contract (host packing).
enum class StackPacking : uint8_t { EightByteSlots, Natural };

constexpr NativeCC hostNativeCC()
{
#if OS(WINDOWS) && CPU(X86_64)
    return NativeCC::Win64;
#elif CPU(ARM64)
    return NativeCC::AAPCS64;
#else
    // FFI-SPEC-GAP: the spec defines hostNativeCC() only for X86_64 and
    // ARM64. Every other CPU is unsupported (SPEC section 14: creation throws
    // "bun:ffi is not supported on this architecture"), but hostNativeCC()
    // must stay total, so SysV64 is the fallback for the layout math.
    return NativeCC::SysV64;
#endif
}

constexpr StackPacking stackPackingForNativeCC(NativeCC cc)
{
#if OS(DARWIN) && CPU(ARM64)
    if (cc == NativeCC::AAPCS64)
        return StackPacking::Natural;
#else
    UNUSED_PARAM(cc);
#endif
    return StackPacking::EightByteSlots;
}

struct ArgLocation {
    enum class Kind : uint8_t { GPR, FPR, Stack } kind { Kind::GPR };
    uint8_t regIndex { 0 };      // index into the NativeCC's arg register list when GPR/FPR
    unsigned stackOffset { 0 };  // byte offset from SP-at-call when Stack (includes Win64 shadow space)
    Type type { Type::Void };
};

struct CallLayout {
    NativeCC cc { hostNativeCC() };
    Vector<ArgLocation, 8> arguments; // parallel to Signature arguments
    unsigned stackBytes { 0 };         // bytes of outgoing stack (shadow + stack args), rounded up to 16; ASSERT(!(stackBytes % 16))
    ArgClass returnClass { ArgClass::Void };
    // FFI-SPEC-GAP: the packing mode is recorded so the invoke/callback
    // thunks can select store64/load64 vs sized stores/loads from the layout
    // itself rather than re-deriving it from OS()/CPU() macros.
    StackPacking packing { StackPacking::EightByteSlots };
};

// The physical argument locations of a native call are identical whether we
// are the caller (invoke thunk, Direction::Outgoing) or the callee (callback
// entry thunk, Direction::Incoming): register indices name the same registers
// and stack offsets are relative to the stack pointer at the call instruction.
// The direction is carried through so callers document intent and so the
// incoming FP-relative conversion (incomingStackOffset) reads naturally.
enum class Direction : uint8_t { Outgoing, Incoming };

// The out-of-line entry points are JS_EXPORT_PRIVATE so the testFFI
// executable (SPEC section 11.3), which links the JavaScriptCore library like
// testmasm, can build its golden layout tables against them (the library is
// compiled with hidden default visibility). Precedent: b3/air's
// computeCCallArguments.
JS_EXPORT_PRIVATE CallLayout computeCallLayout(NativeCC, const Signature&, Direction = Direction::Outgoing);
// Explicit-packing variant; the overload above forwards with
// stackPackingForNativeCC(cc). Only NativeCC::AAPCS64 has more than one
// meaningful packing; SysV64 and Win64 always use eight-byte slots.
JS_EXPORT_PRIVATE CallLayout computeCallLayout(NativeCC, StackPacking, const Signature&, Direction = Direction::Outgoing);

// For Direction::Incoming (callback thunk): the byte offset of stack argument
// `argIndex` relative to the callee's frame pointer AFTER its prologue
// (loc.stackOffset + 16 on x86-64/arm64: saved fp + return address).
JS_EXPORT_PRIVATE unsigned incomingStackOffset(const CallLayout&, unsigned argIndex);

// {calleeSavedSlotsReg, volatileTargetReg} for the invoke thunk (SPEC section
// 7.2 step 3): x86-64: {X86Registers::ebx (rbx), X86Registers::r10}; arm64:
// {ARM64Registers::x19, ARM64Registers::x9}. Neither register is in
// integerArgumentRegisters(cc) for any cc, neither is the ABI return
// register, and neither is the MacroAssembler implicit scratch
// (X86_64 s_scratchRegister = r11; arm64 ip0/ip1 = x16/x17) -- static_asserts
// live in FFICallingConvention.cpp.
JS_EXPORT_PRIVATE std::array<GPRReg, 2> scratchGPRsForInvoke(NativeCC);

constexpr unsigned shadowStackBytes(NativeCC cc)
{
    // Win64 requires the caller to reserve a 32-byte home ("shadow") area for
    // the four register parameters even when fewer are used.
    return cc == NativeCC::Win64 ? 32 : 0;
}

// Positional argument-register capacities, independent of the host CPU so the
// layout math is total (row T computes golden tables for every NativeCC on a
// single host). The concrete register lists below exist only when the host
// CPU has those registers.
constexpr unsigned integerArgumentRegisterCount(NativeCC cc)
{
    switch (cc) {
    case NativeCC::SysV64:
        return 6;
    case NativeCC::AAPCS64:
        return 8;
    case NativeCC::Win64:
        return 4;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr unsigned floatArgumentRegisterCount(NativeCC cc)
{
    switch (cc) {
    case NativeCC::SysV64:
        return 8;
    case NativeCC::AAPCS64:
        return 8;
    case NativeCC::Win64:
        return 4;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// Register lists, ordered by argument position. On a host CPU that does not
// have a given ABI's registers (e.g. SysV64/Win64 when built for arm64) the
// list is empty; nothing on such a host ever emits code for that ABI.
#if ENABLE(ASSEMBLER) && CPU(X86_64)
inline constexpr std::array<GPRReg, 6> s_sysV64IntegerArgumentRegisters {
    X86Registers::edi, X86Registers::esi, X86Registers::edx, X86Registers::ecx, X86Registers::r8, X86Registers::r9,
};
inline constexpr std::array<FPRReg, 8> s_sysV64FloatArgumentRegisters {
    X86Registers::xmm0, X86Registers::xmm1, X86Registers::xmm2, X86Registers::xmm3,
    X86Registers::xmm4, X86Registers::xmm5, X86Registers::xmm6, X86Registers::xmm7,
};
inline constexpr std::array<GPRReg, 4> s_win64IntegerArgumentRegisters {
    X86Registers::ecx, X86Registers::edx, X86Registers::r8, X86Registers::r9,
};
// Win64 assigns rcx/rdx/r8/r9 and xmm0-xmm3 by argument *position*; xmm4 and
// xmm5 are never argument registers.
inline constexpr std::array<FPRReg, 4> s_win64FloatArgumentRegisters {
    X86Registers::xmm0, X86Registers::xmm1, X86Registers::xmm2, X86Registers::xmm3,
};
#endif // ENABLE(ASSEMBLER) && CPU(X86_64)

#if ENABLE(ASSEMBLER) && CPU(ARM64)
inline constexpr std::array<GPRReg, 8> s_aapcs64IntegerArgumentRegisters {
    ARM64Registers::x0, ARM64Registers::x1, ARM64Registers::x2, ARM64Registers::x3,
    ARM64Registers::x4, ARM64Registers::x5, ARM64Registers::x6, ARM64Registers::x7,
};
inline constexpr std::array<FPRReg, 8> s_aapcs64FloatArgumentRegisters {
    ARM64Registers::q0, ARM64Registers::q1, ARM64Registers::q2, ARM64Registers::q3,
    ARM64Registers::q4, ARM64Registers::q5, ARM64Registers::q6, ARM64Registers::q7,
};
#endif // ENABLE(ASSEMBLER) && CPU(ARM64)

constexpr std::span<const GPRReg> integerArgumentRegisters(NativeCC cc)
{
    switch (cc) {
    case NativeCC::SysV64:
#if ENABLE(ASSEMBLER) && CPU(X86_64)
        return std::span<const GPRReg> { s_sysV64IntegerArgumentRegisters };
#else
        return { };
#endif
    case NativeCC::Win64:
#if ENABLE(ASSEMBLER) && CPU(X86_64)
        return std::span<const GPRReg> { s_win64IntegerArgumentRegisters };
#else
        return { };
#endif
    case NativeCC::AAPCS64:
#if ENABLE(ASSEMBLER) && CPU(ARM64)
        return std::span<const GPRReg> { s_aapcs64IntegerArgumentRegisters };
#else
        return { };
#endif
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr std::span<const FPRReg> floatArgumentRegisters(NativeCC cc)
{
    switch (cc) {
    case NativeCC::SysV64:
#if ENABLE(ASSEMBLER) && CPU(X86_64)
        return std::span<const FPRReg> { s_sysV64FloatArgumentRegisters };
#else
        return { };
#endif
    case NativeCC::Win64:
#if ENABLE(ASSEMBLER) && CPU(X86_64)
        return std::span<const FPRReg> { s_win64FloatArgumentRegisters };
#else
        return { };
#endif
    case NativeCC::AAPCS64:
#if ENABLE(ASSEMBLER) && CPU(ARM64)
        return std::span<const FPRReg> { s_aapcs64FloatArgumentRegisters };
#else
        return { };
#endif
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

} // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
