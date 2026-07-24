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

// WHY THIS STUB EXISTS (measured, not assumed): once a call site tiers up, the DFG/FTL emit a
// CallFFI node (the FTL calling the target directly), and this stub is bypassed. Its job is the
// UNOPTIMIZED path -- LLInt/baseline callers and any generic (non-devirtualized) call -- and there
// it is decisive: with the JIT tiers disabled, noop() is 4.9ns via this stub vs 7.4ns through the
// generic C++ host marshaller, and add(i32,i32) is 5.6ns vs 13.4ns (the C++ path re-walks the
// signature per call; the stub's per-type conversions are compiled). Cold code and every call
// before tier-up run through here, so do not delete it on the strength of a JIT-tiers-on benchmark.
#include "config.h"
#include "FFIICStub.h"

#if USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)

#include "CCallHelpers.h"
#include "CallFrame.h"
#include "FFICallHost.h"
#include "FFIContext.h"
#include "FFIConversions.h"
#include "FFISignature.h"
#include "FFIType.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include "JITCode.h"
#include "JITOperations.h"
#include "JSCPtrTag.h"
#include "JSArrayBufferView.h"
#include "JSBigInt.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSType.h"
#include "LinkBuffer.h"
#include "Options.h"
#include "StackAlignment.h"
#include "ThunkGenerator.h"
#include "VirtualRegister.h"
#include <limits>
#include <wtf/CompilationThread.h>
#include <wtf/DataLog.h>
#include <wtf/MathExtras.h>
#include <wtf/RawPointer.h>
#include <wtf/StdIntExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>

namespace JSC { namespace FFI {

#if USE(JSVALUE64) && !ENABLE(JIT_CAGE)

// The stub is the callee's own function entry code (installed as the call
// code of the JSFFIFunction's diversified NativeExecutable, SPEC section
// 8.1), entered in the JS calling convention from JS call sites (LLInt call
// slow path, call ICs, DFG/FTL direct calls). C++-initiated calls
// (Interpreter::executeCall on CallData::Type::Native) invoke the
// executable's native function, FFI::ffiHostCall, and never enter here.
// Register plan: every register used below is caller-saved in the JS calling
// convention on x86-64 and arm64 (rax/rsi/rdx/rcx/r8/r10 and x0-x5), none is
// the macro-assembler's implicit scratch (r11 / x16 / x17), and nothing is
// kept live in a register across the native call: everything needed
// afterwards is re-derived from the frame pointer.
namespace {

constexpr GPRReg valueGPR = GPRInfo::regT0; // rax / x0: the JSValue being converted; the boxed return value.
constexpr GPRReg scratchGPR = GPRInfo::regT2; // rdx / x2
constexpr GPRReg scratch2GPR = GPRInfo::regT3; // rcx / x3
constexpr GPRReg scratch3GPR = GPRInfo::regT4; // r8 / x4
constexpr GPRReg callTargetGPR = GPRInfo::nonArgGPR0; // r10 / x8: volatile, not an argument register.
constexpr FPRReg valueFPR = FPRInfo::fpRegT0; // xmm0 / q0

static_assert(valueGPR != callTargetGPR);
static_assert(scratchGPR != callTargetGPR && scratch2GPR != callTargetGPR && scratch3GPR != callTargetGPR);
static_assert(valueGPR != scratchGPR && valueGPR != scratch2GPR && valueGPR != scratch3GPR);
static_assert(scratchGPR != scratch2GPR && scratchGPR != scratch3GPR && scratch2GPR != scratch3GPR);
static_assert(callTargetGPR != GPRInfo::argumentGPR0 && callTargetGPR != GPRInfo::argumentGPR1 && callTargetGPR != GPRInfo::argumentGPR2);
static_assert(callTargetGPR != GPRInfo::returnValueGPR);

// FFI-SPEC-GAP: SPEC section 8.3 does not say how the runtime tag registers
// (numberTag / notCellMask) are handled. They are JS-calling-convention
// callee-saves whose contents are not guaranteed canonical at a function
// entry (the baseline JIT and SpecializedThunkJIT both save then materialize
// them in the prologue), so the stub follows that protocol: it saves the
// caller's values FP-relative, materializes the canonical tags for its own
// use, and restores the saved values on every exit path (fast return,
// slow-path return, and before the callee-save copy on exception unwind).
constexpr int32_t numberTagSaveOffset = -static_cast<int32_t>(sizeof(CPURegister));
constexpr int32_t notCellMaskSaveOffset = -static_cast<int32_t>(2 * sizeof(CPURegister));
constexpr size_t tagSaveAreaBytes = 2 * sizeof(CPURegister); // 16, keeps the frame 16-byte aligned.
static_assert(!(tagSaveAreaBytes % stackAlignmentBytes()));
// On 64-bit targets sizeof(CallerFrameAndPC) is a stack-alignment multiple,
// so the frame pointer left by emitFunctionPrologue() is itself 16-byte
// aligned and subtracting a 16-byte-multiple frame keeps sp aligned at both
// the invoke-thunk call and the operation calls.
static_assert(!stackAdjustmentForAlignment());

void emitSaveTagRegisters(CCallHelpers& jit)
{
    jit.store64(GPRInfo::numberTagRegister, CCallHelpers::Address(GPRInfo::callFrameRegister, numberTagSaveOffset));
    jit.store64(GPRInfo::notCellMaskRegister, CCallHelpers::Address(GPRInfo::callFrameRegister, notCellMaskSaveOffset));
    jit.emitMaterializeTagCheckRegisters();
}

void emitRestoreTagRegisters(CCallHelpers& jit)
{
    jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, numberTagSaveOffset), GPRInfo::numberTagRegister);
    jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, notCellMaskSaveOffset), GPRInfo::notCellMaskRegister);
}

// Fast JS -> canonical-slot conversion for one native parameter (SPEC
// section 8.3 step 4). `argument` is the FP-relative address of the boxed JS
// argument, `slot` the FP-relative address of the parameter's 8-byte slot.
// Every type miss appends to `slowPath`, which redoes the whole call in C++.
void emitConvertArgument(CCallHelpers& jit, Type type, CCallHelpers::Address argument, CCallHelpers::Address slot, CCallHelpers::JumpList& slowPath)
{
    switch (type) {
    case Type::Char:
    case Type::Int8:
        jit.load64(argument, valueGPR);
        slowPath.append(jit.branchIfNotInt32(valueGPR));
        jit.signExtend8To32(valueGPR, valueGPR);
        jit.signExtend32ToPtr(valueGPR, valueGPR);
        jit.store64(valueGPR, slot);
        return;

    case Type::Uint8:
        jit.load64(argument, valueGPR);
        slowPath.append(jit.branchIfNotInt32(valueGPR));
        jit.zeroExtend8To32(valueGPR, valueGPR); // 32-bit result clears bits [63:32] on both targets.
        jit.store64(valueGPR, slot);
        return;

    case Type::Int16:
        jit.load64(argument, valueGPR);
        slowPath.append(jit.branchIfNotInt32(valueGPR));
        jit.signExtend16To32(valueGPR, valueGPR);
        jit.signExtend32ToPtr(valueGPR, valueGPR);
        jit.store64(valueGPR, slot);
        return;

    case Type::Uint16:
        jit.load64(argument, valueGPR);
        slowPath.append(jit.branchIfNotInt32(valueGPR));
        jit.zeroExtend16To32(valueGPR, valueGPR);
        jit.store64(valueGPR, slot);
        return;

    case Type::Int32:
        jit.load64(argument, valueGPR);
        slowPath.append(jit.branchIfNotInt32(valueGPR));
        jit.signExtend32ToPtr(valueGPR, valueGPR);
        jit.store64(valueGPR, slot);
        return;

    case Type::Uint32:
        jit.load64(argument, valueGPR);
        slowPath.append(jit.branchIfNotInt32(valueGPR));
        jit.zeroExtend32ToWord(valueGPR, valueGPR);
        jit.store64(valueGPR, slot);
        return;

    case Type::Bool: {
        jit.load64(argument, valueGPR);
        auto notInt32 = jit.branchIfNotInt32(valueGPR);
        // Any non-zero int32 is true (toBoolean semantics) -- never an and32(1),
        // which would mis-convert even non-zero values.
        jit.compare32(CCallHelpers::NotEqual, valueGPR, CCallHelpers::TrustedImm32(0), scratchGPR);
        jit.store64(scratchGPR, slot);
        auto done = jit.jump();
        notInt32.link(&jit);
        // Booleans are the common case (Bun's `!!val` glue): ValueFalse = 0x06,
        // ValueTrue = 0x07, so the low bit of the unboxed bits is the payload.
        slowPath.append(jit.branchIfNotBoolean(valueGPR, scratchGPR));
        jit.and32(CCallHelpers::TrustedImm32(1), valueGPR, scratchGPR);
        jit.store64(scratchGPR, slot);
        done.link(&jit);
        return;
    }

    case Type::Int64:
    case Type::Uint64:
    case Type::Int64Fast:
    case Type::Uint64Fast: {
        jit.load64(argument, valueGPR);
        auto notInt32 = jit.branchIfNotInt32(valueGPR);
        jit.signExtend32ToPtr(valueGPR, valueGPR); // int32 -> sign-extend (then reinterpret for the unsigned types).
        jit.store64(valueGPR, slot);
        auto done = jit.jump();
        notInt32.link(&jit);
        // Non-int32 numbers take the slow path in v1 (which applies
        // FFI::doubleToInt64); a HeapBigInt of length <= 1 is fast-pathed with
        // the same digit-0-with-sign truncation as JSBigInt::toBigInt64.
        slowPath.append(jit.branchIfNotCell(valueGPR));
        slowPath.append(jit.branchIfNotHeapBigInt(valueGPR));
        slowPath.append(jit.branch32(CCallHelpers::Above, CCallHelpers::Address(valueGPR, JSBigInt::offsetOfLength()), CCallHelpers::TrustedImm32(1)));
        jit.toBigInt64(valueGPR, scratchGPR);
        jit.store64(scratchGPR, slot);
        done.link(&jit);
        return;
    }

    case Type::Double:
    case Type::Float: {
        jit.load64(argument, valueGPR);
        slowPath.append(jit.branchIfNotNumber(valueGPR));
        auto isInt32 = jit.branchIfInt32(valueGPR);
        jit.unboxDouble(valueGPR, scratchGPR, valueFPR);
        auto haveDouble = jit.jump();
        isInt32.link(&jit);
        jit.convertInt32ToDouble(valueGPR, valueFPR);
        haveDouble.link(&jit);
        if (type == Type::Float) {
            jit.convertDoubleToFloat(valueFPR, valueFPR);
            jit.store32(CCallHelpers::TrustedImm32(0), slot.withOffset(4)); // f32 slots have bits [63:32] zero (SPEC section 4).
            jit.storeFloat(valueFPR, slot);
        } else
            jit.storeDouble(valueFPR, slot);
        return;
    }

    case Type::Pointer:
    case Type::CString:
    case Type::Function:
    case Type::Buffer: {
        jit.load64(argument, valueGPR);
        CCallHelpers::JumpList stored;
        if (type != Type::Buffer) {
            // Numbers are only accepted for the pointer-shaped types; `buffer`
            // requires a view and throws for anything else (in C++).
            auto notInt32 = jit.branchIfNotInt32(valueGPR);
            jit.signExtend32ToPtr(valueGPR, valueGPR); // int32 pointer arguments are sign-extended (SPEC section 5).
            jit.store64(valueGPR, slot);
            stored.append(jit.jump());
            notInt32.link(&jit);
            auto notNumber = jit.branchIfNotNumber(valueGPR);
            jit.unboxDouble(valueGPR, scratchGPR, valueFPR);
            jit.truncateDoubleToInt64(valueFPR, scratchGPR); // by definition identical to FFI::doubleToInt64.
            jit.store64(scratchGPR, slot);
            stored.append(jit.jump());
            notNumber.link(&jit);
        }
        // JSArrayBufferView fast path (Int8Array .. DataView is a contiguous
        // JSType range). JSArrayBuffer, JS strings (cstring transcoding),
        // JSFFICallback cells and everything else are handled by the C++ slow
        // path in v1.
        slowPath.append(jit.branchIfNotCell(valueGPR));
        slowPath.append(jit.branchIfNotType(valueGPR, JSTypeRange { static_cast<JSType>(FirstTypedArrayType), static_cast<JSType>(LastTypedArrayType) }));
        jit.loadPtr(CCallHelpers::Address(valueGPR, JSArrayBufferView::offsetOfVector()), scratchGPR);
        slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, scratchGPR)); // null / detached vector: keep those semantics in C++.
        // FFI-SPEC-GAP: SPEC section 8.3 step 4 spells this load32, but
        // JSArrayBufferView::m_length is a size_t, so it is loaded at its
        // real 64-bit width (matching the in-tree AssemblyHelpers precedents
        // that read offsetOfLength() with load64).
        jit.load64(CCallHelpers::Address(valueGPR, JSArrayBufferView::offsetOfLength()), scratch2GPR);
        jit.cageConditionally(Gigacage::Primitive, scratchGPR, scratch2GPR, scratch3GPR);
        jit.store64(scratchGPR, slot);
        stored.link(&jit);
        return;
    }

    case Type::NapiValue:
        // napi_value is a raw EncodedJSValue pass-through: no conversion.
        jit.load64(argument, valueGPR);
        jit.store64(valueGPR, slot);
        return;

    case Type::NapiEnv:
    case Type::Void:
        // NapiEnv is synthetic (never read from JS) and is emitted by the
        // caller; Void is not a valid argument type (rejected by
        // Signature::tryCreate).
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Boxes the return slot into the JS return-value register per the
// native->JS table (SPEC section 5), keeping the exotic (BigInt / pointer)
// boxing out of line in operationFFIBoxSlot. `exceptionChecks` collects the
// jump to the shared exception epilogue for the operation-calling cases.
void emitBoxReturnValue(CCallHelpers& jit, VM& vm, JSGlobalObject* globalObject, Type type, CCallHelpers::Address returnSlot, CCallHelpers::JumpList& exceptionChecks)
{
    JSValueRegs resultRegs { GPRInfo::returnValueGPR };
    switch (type) {
    case Type::Void:
        jit.moveTrustedValue(jsUndefined(), resultRegs);
        return;

    case Type::Char:
    case Type::Int8:
    case Type::Uint8:
    case Type::Int16:
    case Type::Uint16:
    case Type::Int32:
        // The slot is sign/zero extended to 64 bits; its low 32 bits are the
        // int32 payload. load32 zero-extends the destination on both targets.
        jit.load32(returnSlot, GPRInfo::returnValueGPR);
        jit.boxInt32(GPRInfo::returnValueGPR, resultRegs);
        return;

    case Type::Uint32: {
        // The zero-extended slot is a non-negative int64; box as int32 when it
        // fits (top bit of the low word clear), else as a double.
        jit.load64(returnSlot, GPRInfo::returnValueGPR);
        auto fitsInInt32 = jit.branch32(CCallHelpers::GreaterThanOrEqual, GPRInfo::returnValueGPR, CCallHelpers::TrustedImm32(0));
        jit.convertInt64ToDouble(GPRInfo::returnValueGPR, valueFPR);
        jit.boxDouble(valueFPR, resultRegs);
        auto done = jit.jump();
        fitsInInt32.link(&jit);
        jit.boxInt32(GPRInfo::returnValueGPR, resultRegs);
        done.link(&jit);
        return;
    }

    case Type::Bool:
        // The producers normalize the slot to exactly 0 or 1 (SPEC section
        // 4), so or-ing in ValueFalse (0x06) yields ValueFalse / ValueTrue.
        jit.load32(returnSlot, GPRInfo::returnValueGPR);
        jit.or32(CCallHelpers::TrustedImm32(JSValue::ValueFalse), GPRInfo::returnValueGPR);
        return;

    case Type::Double:
        jit.loadDouble(returnSlot, valueFPR);
        jit.purifyNaN(valueFPR, valueFPR); // a native NaN payload must never forge a boxed JSValue.
        jit.boxDouble(valueFPR, resultRegs);
        return;

    case Type::Float:
        jit.loadFloat(returnSlot, valueFPR);
        jit.convertFloatToDouble(valueFPR, valueFPR);
        jit.purifyNaN(valueFPR, valueFPR);
        jit.boxDouble(valueFPR, resultRegs);
        return;

    case Type::NapiValue:
        // FFI-SPEC-GAP: section 8.3 step 7 lists only the numeric/void inline
        // cases and the operationFFIBoxSlot cases; napi_value is by definition
        // the raw EncodedJSValue bits (section 5, "napi_value -> JSValue::decode(bits)"),
        // so it is returned as-is without an operation call.
        jit.load64(returnSlot, GPRInfo::returnValueGPR);
        return;

    case Type::Int64:
    case Type::Uint64:
    case Type::Int64Fast:
    case Type::Uint64Fast:
    case Type::Pointer:
    case Type::CString:
    case Type::Function:
        // Exotic boxing (BigInt allocation, Number-vs-BigInt cutoffs, pointer
        // null -> jsNull()) stays out of line: EncodedJSValue
        // operationFFIBoxSlot(JSGlobalObject*, uint32_t typeTag, uint64_t slot).
        jit.load64(returnSlot, GPRInfo::argumentGPR2);
        jit.move(CCallHelpers::TrustedImm32(static_cast<uint32_t>(type)), GPRInfo::argumentGPR1);
        jit.move(CCallHelpers::TrustedImmPtr(globalObject), GPRInfo::argumentGPR0);
        jit.move(CCallHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(operationFFIBoxSlot)), callTargetGPR);
        jit.call(callTargetGPR, OperationPtrTag);
        exceptionChecks.append(jit.emitExceptionCheck(vm));
        return;

    case Type::Buffer:
    case Type::NapiEnv:
        // Neither is a valid return type (rejected by Signature::tryCreate).
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // anonymous namespace

RefPtr<JITCode> generateICStubCode(VM& vm, JSGlobalObject* globalObject, Signature& signature, void* target)
{
    ASSERT(!isCompilationThread());

    if (!Options::useFFIICStub() || !Options::useJIT() || Options::forceICFailure())
        return nullptr;

    // The stub calls the (signature-pure, process-shared) invoke thunk;
    // without it there is nothing to enter.
    CodePtr<JITThunkPtrTag> invokeThunk = signature.invokeThunk();
    if (!invokeThunk)
        return nullptr;

    const unsigned argumentCount = signature.argumentCount();
    const unsigned jsArgumentCount = signature.jsArgumentCount();
    const Type returnType = signature.returnType();

    // Frame layout (SPEC section 8.3 step 1). After emitFunctionPrologue() the
    // frame pointer equals the entry stack pointer and is 16-byte aligned; we
    // reserve, below fp:
    //     [fp -  8]  caller's numberTagRegister
    //     [fp - 16]  caller's notCellMaskRegister
    //     [fp - frameBytes, +slotBufferBytes)  the canonical slot buffer
    // frameBytes is a multiple of 16 so sp stays aligned at both calls. The
    // slot buffer is addressed FP-relative and never carried in a register
    // across the native call.
    const size_t slotBufferBytes = signature.slotBufferBytes();
    const size_t slotAreaBytes = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(slotBufferBytes);
    const size_t frameBytes = tagSaveAreaBytes + slotAreaBytes;
    RELEASE_ASSERT(!(frameBytes % stackAlignmentBytes()));
    RELEASE_ASSERT(frameBytes <= static_cast<size_t>(std::numeric_limits<int32_t>::max()));
    const int32_t slotsOffsetFromFP = -static_cast<int32_t>(frameBytes);
    auto slotAddress = [&](unsigned slotIndex) {
        return CCallHelpers::Address(GPRInfo::callFrameRegister, slotsOffsetFromFP + static_cast<int32_t>(argumentSlotOffset(slotIndex)));
    };
    const CCallHelpers::Address returnSlotAddress = slotAddress(argumentCount);

    CCallHelpers jit;
    JIT_COMMENT(jit, "FFI IC stub for ", signature.toString());

    CCallHelpers::JumpList slowPath;
    CCallHelpers::JumpList exceptionChecks;

    // 1. Prologue and frame.
    jit.emitFunctionPrologue();
    jit.subPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(frameBytes)), CCallHelpers::stackPointerRegister);
    emitSaveTagRegisters(jit);
    // This IS the callee's entry, so CallFrameSlot::callee already holds the
    // JSFFIFunction; host frames carry a null CodeBlock.
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));

    // 3. topCallFrame before any operation call and before the native call
    //    (callbacks and exceptions re-enter the VM from inside it).
    jit.storePtr(GPRInfo::callFrameRegister, &vm.topCallFrame);

    // 2. Arity: provided < expected takes the slow path (missing arguments
    //    become undefined there); extra arguments are simply ignored.
    if (jsArgumentCount) {
        JIT_COMMENT(jit, "arity check");
        slowPath.append(jit.branch32(CCallHelpers::Below, CCallHelpers::payloadFor(CallFrameSlot::argumentCountIncludingThis), CCallHelpers::TrustedImm32(jsArgumentCount + 1)));
    }

    // 4. Fast-convert each native parameter into its canonical slot.
    void* const* napiEnvAddress = nullptr;
    unsigned jsIndex = 0;
    for (unsigned i = 0; i < argumentCount; ++i) {
        Type type = signature.argumentType(i);
        JIT_COMMENT(jit, "argument ", i, " : ", name(type));
        if (isSyntheticArgument(type)) {
            // Synthetic napi_env: read the embedder's env LIVE from the
            // FFIContext (never baked as an immediate, SPEC section 6).
            if (!napiEnvAddress)
                napiEnvAddress = globalObject->ffiContext().addressOfNapiEnv();
            jit.move(CCallHelpers::TrustedImmPtr(napiEnvAddress), scratchGPR);
            jit.loadPtr(CCallHelpers::Address(scratchGPR), scratchGPR);
            jit.store64(scratchGPR, slotAddress(i));
            continue;
        }
        CCallHelpers::Address argument = CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(jsIndex) + 1));
        emitConvertArgument(jit, type, argument, slotAddress(i), slowPath);
        ++jsIndex;
    }
    ASSERT(jsIndex == jsArgumentCount);

    // 5. Call the invoke thunk: void SYSV thunk(void* target, uint64_t* slots).
    JIT_COMMENT(jit, "call invoke thunk");
    jit.addPtr(CCallHelpers::TrustedImm32(slotsOffsetFromFP), GPRInfo::callFrameRegister, GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImmPtr(target), GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImmPtr(invokeThunk.taggedPtr()), callTargetGPR);
    jit.call(callTargetGPR, OperationPtrTag);

    // 6. A JS callback that ran inside the native call may have left an
    //    exception pending.
    exceptionChecks.append(jit.emitExceptionCheck(vm));

    // 7. Box the return slot.
    JIT_COMMENT(jit, "box return value : ", name(returnType));
    emitBoxReturnValue(jit, vm, globalObject, returnType, returnSlotAddress, exceptionChecks);

    // 8. Epilogue.
    emitRestoreTagRegisters(jit);
    jit.emitFunctionEpilogue();
    jit.ret();

    // 9. Slow path: operationFFICallSlowPath performs the entire call itself
    //    (argument conversion from this CallFrame, arena bracketing, boxing).
    slowPath.link(&jit);
    JIT_COMMENT(jit, "slow path");
    jit.move(GPRInfo::callFrameRegister, GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImmPtr(globalObject), GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(operationFFICallSlowPath)), callTargetGPR);
    jit.call(callTargetGPR, OperationPtrTag);
    exceptionChecks.append(jit.emitExceptionCheck(vm));
    // Result is already in returnValueGPR.
    emitRestoreTagRegisters(jit);
    jit.emitFunctionEpilogue();
    jit.ret();

    // Exception epilogue: exactly the nativeForGenerator sequence, after
    // restoring the caller's tag registers so the callee-save copy captures
    // their original contents.
    exceptionChecks.link(&jit);
    JIT_COMMENT(jit, "exception handler");
    emitRestoreTagRegisters(jit);
    jit.copyCalleeSavesToEntryFrameCalleeSavesBuffer(vm.topEntryFrame, GPRInfo::argumentGPR0);
    jit.storePtr(GPRInfo::callFrameRegister, &vm.topCallFrame);
    jit.move(CCallHelpers::TrustedImmPtr(&vm), GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(operationVMHandleException)), GPRInfo::regT3);
    jit.call(GPRInfo::regT3, OperationPtrTag);
    jit.jumpToExceptionHandler(vm);

    LinkBuffer linkBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk, JITCompilationCanFail);
    if (linkBuffer.didFailToAllocate()) [[unlikely]]
        return nullptr;
    linkBuffer.setIsThunk();
    auto codeRef = FINALIZE_CODE_IF(Options::dumpDisassembly() || Options::dumpFFIDisassembly(), linkBuffer, JSEntryPtrTag, "FFI ic"_s, "FFI ic %s", signature.toString().utf8().data());

    g_ffiCompileCounts.icStub++;
    dataLogLnIf(Options::verboseFFI(), "FFI: generated IC stub ", signature.toString(), " target ", RawPointer(target), " code ", RawPointer(codeRef.code().taggedPtr()));

    return adoptRef(new DirectJITCode(codeRef, codeRef.code(), JITType::HostCallThunk, NoIntrinsic));
}

#else // !(USE(JSVALUE64) && !ENABLE(JIT_CAGE))

RefPtr<JITCode> generateICStubCode(VM&, JSGlobalObject*, Signature&, void*)
{
    // bun:ffi is 64-bit only, and JIT-operation-validation builds require the
    // native target to be called untagged, so the FFI JIT surface is compiled
    // out there; JSFFIFunction::create() falls back to the host-function
    // executable (SPEC sections 0.1 / 14).
    return nullptr;
}

#endif // USE(JSVALUE64) && !ENABLE(JIT_CAGE)

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)
