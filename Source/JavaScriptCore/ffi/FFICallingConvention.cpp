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

#include "config.h"
#include "FFICallingConvention.h"

#if USE(BUN_JSC_ADDITIONS)

#include "FFISignature.h"
#include <wtf/MathExtras.h>

namespace JSC::FFI {

// -----------------------------------------------------------------------
// Invoke-thunk scratch register contract (SPEC section 7.1 / 7.2 step 3).
// The callee-saved register carries the slot buffer across the native call;
// the volatile register carries the call target. Neither may alias an
// argument register of any callee CC, the ABI return register, or the
// MacroAssembler's implicit scratch registers.
//
// The helper and the static_asserts are compiled only for the two CPUs whose
// register lists exist; defining the helper unconditionally would leave an
// unused static function (-Wunused-function) on every other configuration.

#if ENABLE(ASSEMBLER) && (CPU(X86_64) || CPU(ARM64))
static constexpr bool listContains(std::span<const GPRReg> list, GPRReg reg)
{
    for (GPRReg candidate : list) {
        if (candidate == reg)
            return true;
    }
    return false;
}
#endif

#if ENABLE(ASSEMBLER) && CPU(X86_64)
static_assert(!listContains(integerArgumentRegisters(NativeCC::SysV64), X86Registers::ebx));
static_assert(!listContains(integerArgumentRegisters(NativeCC::SysV64), X86Registers::r10));
static_assert(!listContains(integerArgumentRegisters(NativeCC::Win64), X86Registers::ebx));
static_assert(!listContains(integerArgumentRegisters(NativeCC::Win64), X86Registers::r10));
static_assert(X86Registers::ebx != GPRInfo::returnValueGPR);
static_assert(X86Registers::r10 != GPRInfo::returnValueGPR);
static_assert(X86Registers::ebx != MacroAssembler::s_scratchRegister);
static_assert(X86Registers::r10 != MacroAssembler::s_scratchRegister);
static_assert(integerArgumentRegisters(NativeCC::SysV64).size() == integerArgumentRegisterCount(NativeCC::SysV64));
static_assert(floatArgumentRegisters(NativeCC::SysV64).size() == floatArgumentRegisterCount(NativeCC::SysV64));
static_assert(integerArgumentRegisters(NativeCC::Win64).size() == integerArgumentRegisterCount(NativeCC::Win64));
static_assert(floatArgumentRegisters(NativeCC::Win64).size() == floatArgumentRegisterCount(NativeCC::Win64));
#endif

#if ENABLE(ASSEMBLER) && CPU(ARM64)
static_assert(!listContains(integerArgumentRegisters(NativeCC::AAPCS64), ARM64Registers::x19));
static_assert(!listContains(integerArgumentRegisters(NativeCC::AAPCS64), ARM64Registers::x9));
static_assert(ARM64Registers::x19 != GPRInfo::returnValueGPR);
static_assert(ARM64Registers::x9 != GPRInfo::returnValueGPR);
static_assert(ARM64Registers::x19 != MacroAssembler::dataTempRegister);
static_assert(ARM64Registers::x19 != MacroAssembler::memoryTempRegister);
static_assert(ARM64Registers::x9 != MacroAssembler::dataTempRegister);
static_assert(ARM64Registers::x9 != MacroAssembler::memoryTempRegister);
static_assert(integerArgumentRegisters(NativeCC::AAPCS64).size() == integerArgumentRegisterCount(NativeCC::AAPCS64));
static_assert(floatArgumentRegisters(NativeCC::AAPCS64).size() == floatArgumentRegisterCount(NativeCC::AAPCS64));
#endif

std::array<GPRReg, 2> scratchGPRsForInvoke(NativeCC cc)
{
#if ENABLE(ASSEMBLER) && CPU(X86_64)
    switch (cc) {
    case NativeCC::SysV64:
    case NativeCC::Win64:
        // rbx is callee-saved in both SysV and Win64; r10 is volatile in both
        // and is never an argument register.
        return { X86Registers::ebx, X86Registers::r10 };
    case NativeCC::AAPCS64:
        break;
    }
#elif ENABLE(ASSEMBLER) && CPU(ARM64)
    if (cc == NativeCC::AAPCS64)
        return { ARM64Registers::x19, ARM64Registers::x9 };
#else
    UNUSED_PARAM(cc);
#endif
    // A NativeCC that this host cannot emit code for was requested.
    RELEASE_ASSERT_NOT_REACHED();
    return { InvalidGPRReg, InvalidGPRReg };
}

// -----------------------------------------------------------------------
// Layout computation.

CallLayout computeCallLayout(NativeCC cc, const Signature& signature, Direction direction)
{
    return computeCallLayout(cc, stackPackingForNativeCC(cc), signature, direction);
}

CallLayout computeCallLayout(NativeCC cc, StackPacking packing, const Signature& signature, Direction direction)
{
    // The physical locations are identical for both directions: register
    // indices name the same argument registers, and stack offsets are byte
    // offsets from the stack pointer at the call instruction (which is what
    // both the outgoing store address and, via incomingStackOffset(), the
    // callee's fp-relative load address are derived from).
    UNUSED_PARAM(direction);

    // Only AAPCS64 has two packings (Apple vs. standard); the other ABIs
    // always use one eight-byte slot per stack argument.
    if (cc != NativeCC::AAPCS64)
        packing = StackPacking::EightByteSlots;

    CallLayout layout;
    layout.cc = cc;
    layout.packing = packing;
    layout.returnClass = argClass(signature.returnType());

    const unsigned integerRegisterCount = integerArgumentRegisterCount(cc);
    const unsigned floatRegisterCount = floatArgumentRegisterCount(cc);
    unsigned gprIndex = 0;
    unsigned fprIndex = 0;
    // Byte offset from SP-at-call of the next stacked argument (the AAPCS64
    // "NSAA"). It starts past the Win64 shadow space, which is 0 elsewhere.
    unsigned nextStackOffset = shadowStackBytes(cc);

    unsigned argumentCount = signature.argumentCount();
    layout.arguments.reserveInitialCapacity(argumentCount);
    for (unsigned i = 0; i < argumentCount; ++i) {
        Type type = signature.argumentType(i);
        ASSERT(isValidArgumentType(type));
        ArgClass klass = argClass(type);
        bool isFloatingPoint = klass == ArgClass::Float || klass == ArgClass::Double;

        ArgLocation location;
        location.type = type;

        if (cc == NativeCC::Win64) {
            // Win64 has four positional argument slots: slot i uses the i'th
            // integer or floating-point argument register according to the
            // argument's class; arguments beyond the fourth go on the stack
            // above the 32-byte shadow space, one 8-byte slot each.
            constexpr unsigned win64RegisterSlots = 4;
            if (i < win64RegisterSlots) {
                location.kind = isFloatingPoint ? ArgLocation::Kind::FPR : ArgLocation::Kind::GPR;
                location.regIndex = static_cast<uint8_t>(i);
            } else {
                location.kind = ArgLocation::Kind::Stack;
                location.stackOffset = shadowStackBytes(NativeCC::Win64) + (i - win64RegisterSlots) * 8;
                nextStackOffset = location.stackOffset + 8;
            }
        } else if (isFloatingPoint && fprIndex < floatRegisterCount) {
            location.kind = ArgLocation::Kind::FPR;
            location.regIndex = static_cast<uint8_t>(fprIndex++);
        } else if (!isFloatingPoint && gprIndex < integerRegisterCount) {
            location.kind = ArgLocation::Kind::GPR;
            location.regIndex = static_cast<uint8_t>(gprIndex++);
        } else {
            location.kind = ArgLocation::Kind::Stack;
            switch (packing) {
            case StackPacking::EightByteSlots:
                // SysV64 and AAPCS64 (rule C.16): every stacked argument
                // occupies one 8-byte slot, value in the low bits.
                location.stackOffset = nextStackOffset;
                nextStackOffset += 8;
                break;
            case StackPacking::Natural: {
                // Apple arm64 (SPEC section 7.1.1): natural size and
                // alignment, so sub-8-byte arguments pack tightly.
                unsigned size = nativeSizeInBytes(type);
                location.stackOffset = roundUpToMultipleOf(size, nextStackOffset);
                nextStackOffset = location.stackOffset + size;
                break;
            }
            }
        }

        layout.arguments.append(location);
    }

    layout.stackBytes = static_cast<unsigned>(roundUpToMultipleOf<16>(static_cast<size_t>(nextStackOffset)));
    ASSERT(!(layout.stackBytes % 16));
    return layout;
}

unsigned incomingStackOffset(const CallLayout& layout, unsigned argIndex)
{
    RELEASE_ASSERT(argIndex < layout.arguments.size());
    const ArgLocation& location = layout.arguments[argIndex];
    RELEASE_ASSERT(location.kind == ArgLocation::Kind::Stack);
    // After the callee's prologue the frame pointer addresses the saved frame
    // pointer, with the return address (x86-64) / saved link register (arm64)
    // in the next word, so the caller's SP-at-call is fp + 16 on every
    // supported (64-bit) target.
    constexpr unsigned savedFrameAndReturnAddressBytes = 2 * sizeof(void*);
    return location.stackOffset + savedFrameAndReturnAddressBytes;
}

} // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
