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
#include "FFIInvokeThunk.h"

#if USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)

#include "CCallHelpers.h"
#include "FFICallingConvention.h"
#include "FFISignature.h"
#include "FFIType.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include "LinkBuffer.h"
#include "Options.h"
#include "ThunkGenerator.h"
#include <wtf/DataLog.h>

namespace JSC { namespace FFI {

#if !ENABLE(JIT_CAGE) && (CPU(X86_64) || CPU(ARM64))

namespace {

#if CPU(X86_64)
static constexpr GPRReg shuffleGPR = X86Registers::eax;
#elif CPU(ARM64)
static constexpr GPRReg shuffleGPR = ARM64Registers::x8;
#endif

inline CCallHelpers::Address slotAddress(GPRReg slotsReg, unsigned slotIndex)
{
    return CCallHelpers::Address(slotsReg, static_cast<int32_t>(slotIndex * slotSize));
}

void emitStackArgument(CCallHelpers& jit, const CallLayout& layout, const ArgLocation& loc, GPRReg slotsReg, unsigned slotIndex)
{
    ASSERT(loc.kind == ArgLocation::Kind::Stack);
    CCallHelpers::Address src = slotAddress(slotsReg, slotIndex);
    CCallHelpers::Address dst(MacroAssembler::stackPointerRegister, static_cast<int32_t>(loc.stackOffset));

    jit.load64(src, shuffleGPR);
    if (layout.packing == StackPacking::Natural) {
        switch (nativeSizeInBytes(loc.type)) {
        case 1:
            jit.store8(shuffleGPR, dst);
            break;
        case 2:
            jit.store16(shuffleGPR, dst);
            break;
        case 4:
            jit.store32(shuffleGPR, dst);
            break;
        case 8:
            jit.store64(shuffleGPR, dst);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        return;
    }

    ASSERT(!(loc.stackOffset % 8));
    ASSERT(loc.stackOffset + 8 <= layout.stackBytes);
    jit.store64(shuffleGPR, dst);
}

void emitReturnNormalization(CCallHelpers& jit, const Signature& signature, GPRReg slotsReg)
{
    Type returnType = signature.returnType();
    unsigned returnSlotIndex = signature.argumentCount();
    CCallHelpers::Address returnSlot = slotAddress(slotsReg, returnSlotIndex);

    switch (argClass(returnType)) {
    case ArgClass::Void:
        break;

    case ArgClass::Float: {
        jit.storeFloat(FPRInfo::returnValueFPR, returnSlot);
        jit.store32(CCallHelpers::TrustedImm32(0), returnSlot.withOffset(4));
        break;
    }

    case ArgClass::Double:
        jit.storeDouble(FPRInfo::returnValueFPR, returnSlot);
        break;

    case ArgClass::Int: {
        GPRReg returnGPR = GPRInfo::returnValueGPR;
        if (returnType == Type::Bool) {
            jit.and32(CCallHelpers::TrustedImm32(0xff), returnGPR);
            jit.compare32(CCallHelpers::NotEqual, returnGPR, CCallHelpers::TrustedImm32(0), returnGPR);
        } else {
            switch (nativeSizeInBytes(returnType)) {
            case 1:
                if (isSigned(returnType)) {
                    jit.signExtend8To32(returnGPR, returnGPR);
                    jit.signExtend32To64(returnGPR, returnGPR);
                } else {
                    jit.zeroExtend8To32(returnGPR, returnGPR);
                    jit.zeroExtend32ToWord(returnGPR, returnGPR);
                }
                break;
            case 2:
                if (isSigned(returnType)) {
                    jit.signExtend16To32(returnGPR, returnGPR);
                    jit.signExtend32To64(returnGPR, returnGPR);
                } else {
                    jit.zeroExtend16To32(returnGPR, returnGPR);
                    jit.zeroExtend32ToWord(returnGPR, returnGPR);
                }
                break;
            case 4:
                if (isSigned(returnType))
                    jit.signExtend32To64(returnGPR, returnGPR);
                else
                    jit.zeroExtend32ToWord(returnGPR, returnGPR);
                break;
            case 8:
                break;
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
        jit.store64(returnGPR, returnSlot);
        break;
    }
    }
}

} // anonymous namespace

MacroAssemblerCodeRef<JITThunkPtrTag> generateInvokeThunk(const Signature& signature)
{
    const NativeCC cc = hostNativeCC();
    CallLayout layout = computeCallLayout(cc, signature, Direction::Outgoing);
    ASSERT(layout.arguments.size() == signature.argumentCount());
    RELEASE_ASSERT(!(layout.stackBytes % 16));

    auto [slotsReg, targetReg] = scratchGPRsForInvoke(cc);
    ASSERT(slotsReg != shuffleGPR && targetReg != shuffleGPR);
    ASSERT(slotsReg != GPRInfo::returnValueGPR && targetReg != GPRInfo::returnValueGPR);

    std::span<const GPRReg> integerArgumentRegs = integerArgumentRegisters(cc);
    std::span<const FPRReg> floatArgumentRegs = floatArgumentRegisters(cc);

    CCallHelpers jit(nullptr);

    jit.emitFunctionPrologue();
#if CPU(X86_64)
    ASSERT(slotsReg == X86Registers::ebx);
    jit.pushToSave(X86Registers::ebx);
    jit.subPtr(CCallHelpers::TrustedImm32(8), MacroAssembler::stackPointerRegister);
#elif CPU(ARM64)
    ASSERT(slotsReg == ARM64Registers::x19);
    jit.pushPair(ARM64Registers::x19, ARM64Registers::x20);
#endif

    if (layout.stackBytes)
        jit.subPtr(CCallHelpers::TrustedImm32(layout.stackBytes), MacroAssembler::stackPointerRegister);

    jit.move(GPRInfo::argumentGPR1, slotsReg);
    jit.move(GPRInfo::argumentGPR0, targetReg);

    for (unsigned i = 0; i < layout.arguments.size(); ++i) {
        const ArgLocation& loc = layout.arguments[i];
        if (loc.kind == ArgLocation::Kind::Stack)
            emitStackArgument(jit, layout, loc, slotsReg, i);
    }
    for (unsigned i = 0; i < layout.arguments.size(); ++i) {
        const ArgLocation& loc = layout.arguments[i];
        if (loc.kind != ArgLocation::Kind::FPR)
            continue;
        FPRReg fpr = floatArgumentRegs[loc.regIndex];
        switch (argClass(loc.type)) {
        case ArgClass::Float:
            jit.loadFloat(slotAddress(slotsReg, i), fpr);
            break;
        case ArgClass::Double:
            jit.loadDouble(slotAddress(slotsReg, i), fpr);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    for (unsigned i = 0; i < layout.arguments.size(); ++i) {
        const ArgLocation& loc = layout.arguments[i];
        if (loc.kind != ArgLocation::Kind::GPR)
            continue;
        ASSERT(argClass(loc.type) == ArgClass::Int);
        GPRReg gpr = integerArgumentRegs[loc.regIndex];
        ASSERT(gpr != slotsReg && gpr != targetReg);
        jit.load64(slotAddress(slotsReg, i), gpr);
    }

#if CPU(X86_64)
    if (cc == NativeCC::SysV64) {
        unsigned floatArgumentRegisterUseCount = 0;
        for (const ArgLocation& loc : layout.arguments) {
            if (loc.kind == ArgLocation::Kind::FPR)
                ++floatArgumentRegisterUseCount;
        }
        ASSERT(floatArgumentRegisterUseCount <= floatArgumentRegs.size());
        jit.move(CCallHelpers::TrustedImm32(floatArgumentRegisterUseCount), X86Registers::eax);
    }
#endif

    jit.call(targetReg, CFunctionPtrTag);

    emitReturnNormalization(jit, signature, slotsReg);

    if (layout.stackBytes)
        jit.addPtr(CCallHelpers::TrustedImm32(layout.stackBytes), MacroAssembler::stackPointerRegister);
#if CPU(X86_64)
    jit.addPtr(CCallHelpers::TrustedImm32(8), MacroAssembler::stackPointerRegister);
    jit.popToRestore(X86Registers::ebx);
#elif CPU(ARM64)
    jit.popPair(ARM64Registers::x19, ARM64Registers::x20);
#endif
    jit.emitFunctionEpilogue();
    jit.ret();

    LinkBuffer linkBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk, JITCompilationCanFail);
    if (linkBuffer.didFailToAllocate()) [[unlikely]]
        return { };

    linkBuffer.setIsThunk();
    auto code = FINALIZE_CODE_IF(Options::dumpDisassembly() || Options::dumpFFIDisassembly(), linkBuffer, JITThunkPtrTag, "FFI invoke"_s, "FFI invoke %s", signature.toString().utf8().data());
    dataLogLnIf(Options::verboseFFI(), "[FFI] generated invoke thunk for ", signature.toString());
    return code;
}

#else // ENABLE(JIT_CAGE) || !(CPU(X86_64) || CPU(ARM64))

MacroAssemblerCodeRef<JITThunkPtrTag> generateInvokeThunk(const Signature&)
{
    return { };
}

#endif

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)
