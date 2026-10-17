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

#include "config.h"
#include "FFICallingConvention.h"

#if USE(BUN_JSC_ADDITIONS)

#include "FFISignature.h"
#include <wtf/MathExtras.h>

namespace JSC::FFI {

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
    RELEASE_ASSERT_NOT_REACHED();
    return { InvalidGPRReg, InvalidGPRReg };
}

CallLayout computeCallLayout(NativeCC cc, const Signature& signature, Direction direction)
{
    return computeCallLayout(cc, stackPackingForNativeCC(cc), signature, direction);
}

CallLayout computeCallLayout(NativeCC cc, StackPacking packing, const Signature& signature, Direction direction)
{
    UNUSED_PARAM(direction);

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
                location.stackOffset = nextStackOffset;
                nextStackOffset += 8;
                break;
            case StackPacking::Natural: {
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
    constexpr unsigned savedFrameAndReturnAddressBytes = 2 * sizeof(void*);
    return location.stackOffset + savedFrameAndReturnAddressBytes;
}

} // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
