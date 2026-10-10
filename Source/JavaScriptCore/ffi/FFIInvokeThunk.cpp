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
    auto code = FINALIZE_CODE_IF(Options::dumpDisassembly() || Options::dumpFFIDisassembly(), linkBuffer, JITThunkPtrTag, "FFI invoke"_s, "FFI invoke %s", signature.toString().utf8().legacyCStringPointer());
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

#if USE(BUN_JSC_ADDITIONS) && FFI_INVOKE_WITHOUT_JIT_SUPPORTED

#include "FFICallingConvention.h"
#include "FFISignature.h"
#include "FFIType.h"
#include <wtf/InlineASM.h>

namespace JSC { namespace FFI {

struct InvokeArguments {
    static constexpr unsigned maxStackBytes = (shadowStackBytes(NativeCC::Win64) + Signature::maxArguments * 8 + 15) & ~15u;

    uint64_t gprs[8];
    uint64_t fprs[8];
    uint64_t returnGPR;
    uint64_t returnFPR;
    uint32_t stackBytes;
    uint32_t fprCount;
    alignas(16) uint8_t stack[maxStackBytes];
};

static_assert(!OBJECT_OFFSETOF(InvokeArguments, gprs));
static_assert(OBJECT_OFFSETOF(InvokeArguments, fprs) == 64);
static_assert(OBJECT_OFFSETOF(InvokeArguments, returnGPR) == 128);
static_assert(OBJECT_OFFSETOF(InvokeArguments, returnFPR) == 136);
static_assert(OBJECT_OFFSETOF(InvokeArguments, stackBytes) == 144);
static_assert(OBJECT_OFFSETOF(InvokeArguments, fprCount) == 148);
static_assert(OBJECT_OFFSETOF(InvokeArguments, stack) == 160);

extern "C" void ffiInvokeTrampoline(void* target, InvokeArguments*);

#if CPU(ARM64) && OS(WINDOWS)

__asm__(
    ".text" "\n"
    ".balign 16" "\n"
    ".globl " SYMBOL_STRING(ffiInvokeTrampoline) "\n"
    ".def " SYMBOL_STRING(ffiInvokeTrampoline) "; .scl 2; .type 32; .endef" "\n"
    ".seh_proc " SYMBOL_STRING(ffiInvokeTrampoline) "\n"
    SYMBOL_STRING(ffiInvokeTrampoline) ":" "\n"

    "stp x19, x20, [sp, #-32]!" "\n"
    ".seh_save_regp_x x19, 32" "\n"
    "stp x29, x30, [sp, #16]" "\n"
    ".seh_save_fplr 16" "\n"
    "add x29, sp, #16" "\n"
    ".seh_add_fp 16" "\n"
    ".seh_endprologue" "\n"
    "mov x19, x1" "\n"
    "mov x9, x0" "\n"

    "ldr w10, [x19, #144]" "\n"
    "sub sp, sp, x10" "\n"
    "add x11, x19, #160" "\n"
    "mov x12, sp" "\n"
    "cbz x10, " LOCAL_LABEL_STRING(ffiInvokeTrampolineRegisters) "\n"
    LOCAL_LABEL_STRING(ffiInvokeTrampolineStack) ":" "\n"
    "ldp x13, x14, [x11], #16" "\n"
    "stp x13, x14, [x12], #16" "\n"
    "subs x10, x10, #16" "\n"
    "b.ne " LOCAL_LABEL_STRING(ffiInvokeTrampolineStack) "\n"

    LOCAL_LABEL_STRING(ffiInvokeTrampolineRegisters) ":" "\n"
    "ldp d0, d1, [x19, #64]" "\n"
    "ldp d2, d3, [x19, #80]" "\n"
    "ldp d4, d5, [x19, #96]" "\n"
    "ldp d6, d7, [x19, #112]" "\n"
    "ldp x0, x1, [x19]" "\n"
    "ldp x2, x3, [x19, #16]" "\n"
    "ldp x4, x5, [x19, #32]" "\n"
    "ldp x6, x7, [x19, #48]" "\n"
    "blr x9" "\n"
    "str x0, [x19, #128]" "\n"
    "str d0, [x19, #136]" "\n"

    ".seh_startepilogue" "\n"
    "sub sp, x29, #16" "\n"
    ".seh_add_fp 16" "\n"
    "ldp x29, x30, [sp, #16]" "\n"
    ".seh_save_fplr 16" "\n"
    "ldp x19, x20, [sp], #32" "\n"
    ".seh_save_regp_x x19, 32" "\n"
    ".seh_endepilogue" "\n"
    "ret" "\n"
    ".seh_endproc" "\n"
);

#elif CPU(ARM64)

__asm__(
    ".text" "\n"
    ".balign 16" "\n"
    ".globl " SYMBOL_STRING(ffiInvokeTrampoline) "\n"
    HIDE_SYMBOL(ffiInvokeTrampoline) "\n"
    SYMBOL_STRING(ffiInvokeTrampoline) ":" "\n"

    "stp x20, x19, [sp, #-32]!" "\n"
    "stp x29, x30, [sp, #16]" "\n"
    "add x29, sp, #16" "\n"
    "mov x19, x1" "\n"
    "mov x9, x0" "\n"

    "ldr w10, [x19, #144]" "\n"
    "sub sp, sp, x10" "\n"
    "add x11, x19, #160" "\n"
    "mov x12, sp" "\n"
    "cbz x10, " LOCAL_LABEL_STRING(ffiInvokeTrampolineRegisters) "\n"
    LOCAL_LABEL_STRING(ffiInvokeTrampolineStack) ":" "\n"
    "ldp x13, x14, [x11], #16" "\n"
    "stp x13, x14, [x12], #16" "\n"
    "subs x10, x10, #16" "\n"
    "b.ne " LOCAL_LABEL_STRING(ffiInvokeTrampolineStack) "\n"

    LOCAL_LABEL_STRING(ffiInvokeTrampolineRegisters) ":" "\n"
    "ldp d0, d1, [x19, #64]" "\n"
    "ldp d2, d3, [x19, #80]" "\n"
    "ldp d4, d5, [x19, #96]" "\n"
    "ldp d6, d7, [x19, #112]" "\n"
    "ldp x0, x1, [x19]" "\n"
    "ldp x2, x3, [x19, #16]" "\n"
    "ldp x4, x5, [x19, #32]" "\n"
    "ldp x6, x7, [x19, #48]" "\n"
    "blr x9" "\n"
    "str x0, [x19, #128]" "\n"
    "str d0, [x19, #136]" "\n"

    "sub sp, x29, #16" "\n"
    "ldp x29, x30, [sp, #16]" "\n"
    "ldp x20, x19, [sp], #32" "\n"
    "ret" "\n"
    ".previous" "\n"
);

#elif CPU(X86_64) && OS(WINDOWS)

__asm__(
    ".text" "\n"
    ".balign 16" "\n"
    ".globl " SYMBOL_STRING(ffiInvokeTrampoline) "\n"
    ".def " SYMBOL_STRING(ffiInvokeTrampoline) "; .scl 2; .type 32; .endef" "\n"
    ".seh_proc " SYMBOL_STRING(ffiInvokeTrampoline) "\n"
    SYMBOL_STRING(ffiInvokeTrampoline) ":" "\n"

    "pushq %rbp" "\n"
    ".seh_pushreg %rbp" "\n"
    "pushq %rbx" "\n"
    ".seh_pushreg %rbx" "\n"
    "pushq %rsi" "\n"
    ".seh_pushreg %rsi" "\n"
    "pushq %rdi" "\n"
    ".seh_pushreg %rdi" "\n"
    "subq $8, %rsp" "\n"
    ".seh_stackalloc 8" "\n"
    "movq %rsp, %rbp" "\n"
    ".seh_setframe %rbp, 0" "\n"
    ".seh_endprologue" "\n"
    "movq %rdx, %rbx" "\n"
    "movq %rcx, %r10" "\n"

    "movl 144(%rbx), %ecx" "\n"
    "subq %rcx, %rsp" "\n"
    "leaq 160(%rbx), %rsi" "\n"
    "movq %rsp, %rdi" "\n"
    "shrl $3, %ecx" "\n"
    "rep movsq" "\n"

    "movsd 64(%rbx), %xmm0" "\n"
    "movsd 72(%rbx), %xmm1" "\n"
    "movsd 80(%rbx), %xmm2" "\n"
    "movsd 88(%rbx), %xmm3" "\n"
    "movq (%rbx), %rcx" "\n"
    "movq 8(%rbx), %rdx" "\n"
    "movq 16(%rbx), %r8" "\n"
    "movq 24(%rbx), %r9" "\n"
    "callq *%r10" "\n"
    "movq %rax, 128(%rbx)" "\n"
    "movsd %xmm0, 136(%rbx)" "\n"

    "leaq 8(%rbp), %rsp" "\n"
    "popq %rdi" "\n"
    "popq %rsi" "\n"
    "popq %rbx" "\n"
    "popq %rbp" "\n"
    "ret" "\n"
    ".seh_endproc" "\n"
);

#elif CPU(X86_64)

__asm__(
    ".text" "\n"
    ".balign 16" "\n"
    ".globl " SYMBOL_STRING(ffiInvokeTrampoline) "\n"
    HIDE_SYMBOL(ffiInvokeTrampoline) "\n"
    SYMBOL_STRING(ffiInvokeTrampoline) ":" "\n"

    "pushq %rbp" "\n"
    "movq %rsp, %rbp" "\n"
    "pushq %rbx" "\n"
    "subq $8, %rsp" "\n"
    "movq %rsi, %rbx" "\n"
    "movq %rdi, %r10" "\n"

    "movl 144(%rbx), %ecx" "\n"
    "subq %rcx, %rsp" "\n"
    "leaq 160(%rbx), %rsi" "\n"
    "movq %rsp, %rdi" "\n"
    "shrl $3, %ecx" "\n"
    "rep movsq" "\n"

    "movsd 64(%rbx), %xmm0" "\n"
    "movsd 72(%rbx), %xmm1" "\n"
    "movsd 80(%rbx), %xmm2" "\n"
    "movsd 88(%rbx), %xmm3" "\n"
    "movsd 96(%rbx), %xmm4" "\n"
    "movsd 104(%rbx), %xmm5" "\n"
    "movsd 112(%rbx), %xmm6" "\n"
    "movsd 120(%rbx), %xmm7" "\n"
    "movq (%rbx), %rdi" "\n"
    "movq 8(%rbx), %rsi" "\n"
    "movq 16(%rbx), %rdx" "\n"
    "movq 24(%rbx), %rcx" "\n"
    "movq 32(%rbx), %r8" "\n"
    "movq 40(%rbx), %r9" "\n"
    "movl 148(%rbx), %eax" "\n"
    "callq *%r10" "\n"
    "movq %rax, 128(%rbx)" "\n"
    "movsd %xmm0, 136(%rbx)" "\n"

    "leaq -8(%rbp), %rsp" "\n"
    "popq %rbx" "\n"
    "popq %rbp" "\n"
    "ret" "\n"
    ".previous" "\n"
);

#endif

static uint64_t normalizedIntegerReturn(Type type, uint64_t value)
{
    if (type == Type::Bool)
        return !!(value & 0xff);
    switch (nativeSizeInBytes(type)) {
    case 1:
        return isSigned(type) ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(value))) : static_cast<uint8_t>(value);
    case 2:
        return isSigned(type) ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(value))) : static_cast<uint16_t>(value);
    case 4:
        return isSigned(type) ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value))) : static_cast<uint32_t>(value);
    case 8:
        return value;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void invokeWithoutJIT(const Signature& signature, void* target, uint64_t* slots)
{
    CallLayout layout = computeCallLayout(hostNativeCC(), signature, Direction::Outgoing);
    RELEASE_ASSERT(layout.stackBytes <= InvokeArguments::maxStackBytes && !(layout.stackBytes % 16));

    InvokeArguments arguments;
    zeroBytes(arguments.gprs);
    zeroBytes(arguments.fprs);
    arguments.stackBytes = layout.stackBytes;
    arguments.fprCount = 0;
    memset(arguments.stack, 0, layout.stackBytes);

    for (unsigned i = 0; i < layout.arguments.size(); ++i) {
        const ArgLocation& location = layout.arguments[i];
        switch (location.kind) {
        case ArgLocation::Kind::GPR:
            arguments.gprs[location.regIndex] = slots[i];
            break;
        case ArgLocation::Kind::FPR:
            arguments.fprs[location.regIndex] = slots[i];
            ++arguments.fprCount;
            break;
        case ArgLocation::Kind::Stack:
            memcpy(arguments.stack + location.stackOffset, &slots[i], layout.packing == StackPacking::Natural ? nativeSizeInBytes(location.type) : 8);
            break;
        }
    }

    ffiInvokeTrampoline(target, &arguments);

    uint64_t& returnSlot = slots[signature.argumentCount()];
    switch (layout.returnClass) {
    case ArgClass::Void:
        break;
    case ArgClass::Float:
        returnSlot = static_cast<uint32_t>(arguments.returnFPR);
        break;
    case ArgClass::Double:
        returnSlot = arguments.returnFPR;
        break;
    case ArgClass::Int:
        returnSlot = normalizedIntegerReturn(signature.returnType(), arguments.returnGPR);
        break;
    }
}

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && FFI_INVOKE_WITHOUT_JIT_SUPPORTED
