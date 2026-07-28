/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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

enum class NativeCC : uint8_t { SysV64, AAPCS64, Win64 };

enum class StackPacking : uint8_t { EightByteSlots, Natural };

constexpr NativeCC hostNativeCC()
{
#if OS(WINDOWS) && CPU(X86_64)
    return NativeCC::Win64;
#elif CPU(ARM64)
    return NativeCC::AAPCS64;
#else
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
    uint8_t regIndex { 0 };
    unsigned stackOffset { 0 };
    Type type { Type::Void };
};

struct CallLayout {
    NativeCC cc { hostNativeCC() };
    Vector<ArgLocation, 8> arguments;
    unsigned stackBytes { 0 };
    ArgClass returnClass { ArgClass::Void };
    StackPacking packing { StackPacking::EightByteSlots };
};

enum class Direction : uint8_t { Outgoing, Incoming };

JS_EXPORT_PRIVATE CallLayout computeCallLayout(NativeCC, const Signature&, Direction = Direction::Outgoing);
JS_EXPORT_PRIVATE CallLayout computeCallLayout(NativeCC, StackPacking, const Signature&, Direction = Direction::Outgoing);

JS_EXPORT_PRIVATE unsigned incomingStackOffset(const CallLayout&, unsigned argIndex);

JS_EXPORT_PRIVATE std::array<GPRReg, 2> scratchGPRsForInvoke(NativeCC);

constexpr unsigned shadowStackBytes(NativeCC cc)
{
    return cc == NativeCC::Win64 ? 32 : 0;
}

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
