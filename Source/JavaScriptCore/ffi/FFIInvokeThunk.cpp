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

// A volatile, non-argument (for every supported callee ABI) GPR used to
// shuffle stack arguments. It is never the invoke thunk's own two scratch
// registers (§7.2 step 3 / scratchGPRsForInvoke), never an integer argument
// register of any NativeCC, and never a macro-assembler implicit scratch.
#if CPU(X86_64)
// rax: the return register; unused as such until after the native call, and
// on SysV64 additionally the %al vector-register count published right
// before the call (step 5), i.e. after every stack shuffle is done.
static constexpr GPRReg shuffleGPR = X86Registers::eax;
#elif CPU(ARM64)
// x8: the AAPCS64 indirect-result register; no struct returns in v1, so it
// is free. It is not x9 (targetReg), not x16/x17 (ip0/ip1 macro scratch) and
// not x0-x7.
static constexpr GPRReg shuffleGPR = ARM64Registers::x8;
#endif

// FFI-SPEC-GAP: §7.2 step 4 says "using registers not yet consumed as
// scratch" for the stack-argument shuffle. Rather than tracking which
// argument registers are still free, this uses a single fixed volatile
// non-argument register (shuffleGPR). Stack arguments are emitted before any
// argument register is written, so a fixed register is trivially safe.

inline CCallHelpers::Address slotAddress(GPRReg slotsReg, unsigned slotIndex)
{
    return CCallHelpers::Address(slotsReg, static_cast<int32_t>(slotIndex * slotSize));
}

// Places one native stack argument. `slot` holds the §4-encoded value at
// slots[slotIndex]; `stackOffset` is relative to SP-at-call.
void emitStackArgument(CCallHelpers& jit, const CallLayout& layout, const ArgLocation& loc, GPRReg slotsReg, unsigned slotIndex)
{
    ASSERT(loc.kind == ArgLocation::Kind::Stack);
    CCallHelpers::Address src = slotAddress(slotsReg, slotIndex);
    CCallHelpers::Address dst(MacroAssembler::stackPointerRegister, static_cast<int32_t>(loc.stackOffset));

    // FFI-SPEC-GAP: §7.1.1 selects Apple-arm64 packing by
    // `OS(DARWIN) && CPU(ARM64)`; FFICallingConvention.h additionally records
    // the mode on the layout (CallLayout::packing), so the thunk reads it from
    // there and cannot disagree with computeCallLayout's offsets.
    jit.load64(src, shuffleGPR);
    if (layout.packing == StackPacking::Natural) {
        // §7.1.1: Apple arm64 packs stack arguments at their natural size and
        // alignment, so store exactly nativeSizeInBytes(type) bytes; never a
        // full 8-byte store into a packed sub-8-byte slot. For Float this
        // stores the 32-bit float pattern (slot bits [31:0], §4); for Double
        // the full 64 bits. Integers were already extended into the slot per
        // §4, so their low bytes are the value.
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

    // SysV64, AAPCS64 (non-Darwin) and Win64 give every stack argument an
    // 8-byte slot; the canonical slot encoding (§4) is exactly what belongs
    // there (integers pre-extended, f32 in the low 4 bytes with zero upper).
    ASSERT(!(loc.stackOffset % 8));
    ASSERT(loc.stackOffset + 8 <= layout.stackBytes);
    jit.store64(shuffleGPR, dst);
}

// §7.2 step 6 / §4: store the native return value into
// slots[argumentCount()] in canonical (fully normalized) encoding.
void emitReturnNormalization(CCallHelpers& jit, const Signature& signature, GPRReg slotsReg)
{
    Type returnType = signature.returnType();
    unsigned returnSlotIndex = signature.argumentCount();
    CCallHelpers::Address returnSlot = slotAddress(slotsReg, returnSlotIndex);

    switch (argClass(returnType)) {
    case ArgClass::Void:
        // Untouched / ignored (§4).
        break;

    case ArgClass::Float: {
        // §4: bit_cast<uint32_t>(float) in bits [31:0], bits [63:32] zero.
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
            // Only the low byte of a native bool return is ABI-defined; the
            // slot MUST be exactly 0 or 1 (§4). This also defends against an
            // int-returning callee misdeclared as bool.
            jit.and32(CCallHelpers::TrustedImm32(0xff), returnGPR);
            jit.compare32(CCallHelpers::NotEqual, returnGPR, CCallHelpers::TrustedImm32(0), returnGPR);
        } else {
            // Sub-word integer returns arrive with unspecified upper bits;
            // extend to 64 bits per the type's signedness and width (§4).
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
                // i64/u64/*_fast, pointer family, napi_value: already 64-bit.
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
    // Kept 16-byte aligned by computeCallLayout so the native call in step
    // 5 is on an aligned stack on every ABI (§7.1 / §7.2 step 2).
    RELEASE_ASSERT(!(layout.stackBytes % 16));

    // Register plan (§7.2 step 3, contract from scratchGPRsForInvoke, §7.1):
    // [0] is a callee-saved GPR that survives the native call and holds
    // `slots`; [1] is a volatile non-argument GPR consumed by the call itself
    // and holds `target`. Neither is an integer argument register of any
    // NativeCC, the ABI return register, or a macro-assembler implicit
    // scratch (r11 / x16 / x17), so neither is disturbed by argument setup.
    auto [slotsReg, targetReg] = scratchGPRsForInvoke(cc);
    ASSERT(slotsReg != shuffleGPR && targetReg != shuffleGPR);
    ASSERT(slotsReg != GPRInfo::returnValueGPR && targetReg != GPRInfo::returnValueGPR);

    std::span<const GPRReg> integerArgumentRegs = integerArgumentRegisters(cc);
    std::span<const FPRReg> floatArgumentRegs = floatArgumentRegisters(cc);

    CCallHelpers jit(nullptr);

    // Step 1: prologue. Entry is a JSC operation-convention call (SysV64
    // on Windows x64 too), so on x86-64 rsp is 8 (mod 16) at entry and
    // emitFunctionPrologue()'s push of rbp restores rsp = 0 (mod 16); on
    // arm64 sp always moves in 16-byte units. We then save the callee-saved
    // slots register keeping the stack 16-byte aligned: x86-64 pushes rbx
    // plus one 8-byte pad; arm64 pushes the x19/x20 pair (x20 is padding).
    jit.emitFunctionPrologue();
#if CPU(X86_64)
    ASSERT(slotsReg == X86Registers::ebx);
    jit.pushToSave(X86Registers::ebx);
    jit.subPtr(CCallHelpers::TrustedImm32(8), MacroAssembler::stackPointerRegister);
#elif CPU(ARM64)
    ASSERT(slotsReg == ARM64Registers::x19);
    jit.pushPair(ARM64Registers::x19, ARM64Registers::x20);
#endif

    // Step 2: outgoing stack area (Win64 shadow space + stack arguments),
    // a multiple of 16 so the native call below sees an aligned stack.
    if (layout.stackBytes)
        jit.subPtr(CCallHelpers::TrustedImm32(layout.stackBytes), MacroAssembler::stackPointerRegister);

    // Step 3: capture the two entry parameters before any argument-register
    // write (the native argument registers alias the entry argument
    // registers). `slots` must survive the call, so it goes to the
    // callee-saved register saved above; `target` only needs to reach the
    // call instruction, so a volatile non-argument register is enough.
    jit.move(GPRInfo::argumentGPR1, slotsReg);
    jit.move(GPRInfo::argumentGPR0, targetReg);

    // Step 4: marshal each argument slot into its ABI location. Stack
    // arguments first (they need shuffleGPR while no argument register is
    // live), then FPRs, then GPRs.
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
            // §4: the f32 pattern lives in the slot's low 32 bits.
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
        // Slot values are already sign/zero-extended to 64 bits (§4), so a
        // straight 64-bit load satisfies every integer/pointer parameter.
        jit.load64(slotAddress(slotsReg, i), gpr);
    }

#if CPU(X86_64)
    // FFI-SPEC-GAP: variadic native callees are a §0.1 non-goal, but the
    // SysV psABI requires %al to hold an upper bound (0..8) on the vector
    // registers used whenever the callee is variadic. Bun's TinyCC path
    // always set it, so fixed-arity signatures bound to printf-style
    // functions worked; publishing the exact FPR count here keeps that
    // well-defined for one instruction. rax (shuffleGPR) is dead once the
    // stack shuffle is done, and eax is not an argument register of either
    // x86-64 CC. (Win64's unprototyped-callee rule — mirroring FPR arguments
    // into the positional integer registers — is NOT applied, §0.1.)
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

    // Step 5: the native call. Win64/SysV64 return integers in rax and
    // floating point in xmm0; AAPCS64 in x0 / q0 — these are
    // GPRInfo::returnValueGPR / FPRInfo::returnValueFPR on each target.
    jit.call(targetReg, CFunctionPtrTag);

    // Step 6: normalize the return value into slots[argumentCount()]. The
    // slots register is callee-saved, so it survived the call.
    emitReturnNormalization(jit, signature, slotsReg);

    // Step 7: unwind exactly the reverse of steps 1 and 2, then return.
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

    // Step 8: finalize. Executable-memory exhaustion is not fatal here; the
    // creation site turns a null CodeRef into an out-of-memory error.
    LinkBuffer linkBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk, JITCompilationCanFail);
    if (linkBuffer.didFailToAllocate()) [[unlikely]]
        return { };

    // FFI-SPEC-GAP: FINALIZE_THUNK only keys its disassembly dump on
    // Options::dumpDisassembly(); §7.2/§12 also want
    // Options::dumpFFIDisassembly() to disassemble this thunk, so the
    // FINALIZE_THUNK sequence (setIsThunk() + FINALIZE_CODE) is spelled out
    // with the extra condition instead of using the macro directly.
    linkBuffer.setIsThunk();
    auto code = FINALIZE_CODE_IF(Options::dumpDisassembly() || Options::dumpFFIDisassembly(), linkBuffer, JITThunkPtrTag, "FFI invoke"_s, "FFI invoke %s", signature.toString().utf8().data());
    dataLogLnIf(Options::verboseFFI(), "[FFI] generated invoke thunk for ", signature.toString());
    return code;
}

#else // ENABLE(JIT_CAGE) || !(CPU(X86_64) || CPU(ARM64))

// bun:ffi requires a raw untagged call into the native target and is only
// implemented for the SysV64 / AAPCS64 / Win64 native ABIs (§0, §14): on
// JIT-cage validation builds and on any other CPU the thunk cannot be
// generated and creation throws.
MacroAssemblerCodeRef<JITThunkPtrTag> generateInvokeThunk(const Signature&)
{
    return { };
}

#endif

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)
