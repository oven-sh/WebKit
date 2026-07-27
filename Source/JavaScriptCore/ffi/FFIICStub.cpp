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

constexpr int32_t numberTagSaveOffset = -static_cast<int32_t>(sizeof(CPURegister));
constexpr int32_t notCellMaskSaveOffset = -static_cast<int32_t>(2 * sizeof(CPURegister));
constexpr size_t tagSaveAreaBytes = 2 * sizeof(CPURegister); // 16, keeps the frame 16-byte aligned.
static_assert(!(tagSaveAreaBytes % stackAlignmentBytes()));
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
        jit.compare32(CCallHelpers::NotEqual, valueGPR, CCallHelpers::TrustedImm32(0), scratchGPR);
        jit.store64(scratchGPR, slot);
        auto done = jit.jump();
        notInt32.link(&jit);
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
            jit.store32(CCallHelpers::TrustedImm32(0), slot.withOffset(4));
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
            auto notInt32 = jit.branchIfNotInt32(valueGPR);
            jit.signExtend32ToPtr(valueGPR, valueGPR);
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
        slowPath.append(jit.branchIfNotCell(valueGPR));
        slowPath.append(jit.branchIfNotType(valueGPR, JSTypeRange { static_cast<JSType>(FirstTypedArrayType), static_cast<JSType>(LastTypedArrayType) }));
        jit.loadPtr(CCallHelpers::Address(valueGPR, JSArrayBufferView::offsetOfVector()), scratchGPR);
        slowPath.append(jit.branchTestPtr(CCallHelpers::Zero, scratchGPR)); // null / detached vector: keep those semantics in C++.
        jit.load64(CCallHelpers::Address(valueGPR, JSArrayBufferView::offsetOfLength()), scratch2GPR);
        jit.cageConditionally(Gigacage::Primitive, scratchGPR, scratch2GPR, scratch3GPR);
        jit.store64(scratchGPR, slot);
        stored.link(&jit);
        return;
    }

    case Type::BufferLength:
        slowPath.append(jit.jump());
        return;

    case Type::JSValue:
        jit.load64(argument, valueGPR);
        jit.store64(valueGPR, slot);
        return;

    case Type::RESERVED_WasNapiEnv:
    case Type::Void:
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

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
        jit.load32(returnSlot, GPRInfo::returnValueGPR);
        jit.boxInt32(GPRInfo::returnValueGPR, resultRegs);
        return;

    case Type::Uint32: {
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

    case Type::JSValue:
        jit.load64(returnSlot, GPRInfo::returnValueGPR);
        return;

    case Type::Int64:
    case Type::Uint64:
    case Type::Int64Fast:
    case Type::Uint64Fast:
    case Type::Pointer:
    case Type::CString:
    case Type::Function:
        jit.load64(returnSlot, GPRInfo::argumentGPR2);
        jit.move(CCallHelpers::TrustedImm32(static_cast<uint32_t>(type)), GPRInfo::argumentGPR1);
        jit.move(CCallHelpers::TrustedImmPtr(globalObject), GPRInfo::argumentGPR0);
        jit.move(CCallHelpers::TrustedImm32(0), GPRInfo::argumentGPR3);
        jit.move(CCallHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(operationFFIBoxSlot)), callTargetGPR);
        jit.call(callTargetGPR, OperationPtrTag);
        exceptionChecks.append(jit.emitExceptionCheck(vm));
        return;

    case Type::Buffer:
    case Type::BufferLength:
    case Type::RESERVED_WasNapiEnv:
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

    CodePtr<JITThunkPtrTag> invokeThunk = signature.invokeThunk();
    if (!invokeThunk)
        return nullptr;

    const unsigned argumentCount = signature.argumentCount();
    const Type returnType = signature.returnType();

    for (unsigned i = 0; i < argumentCount; ++i) {
        if (signature.argumentType(i) == Type::BufferLength)
            return nullptr;
    }

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

    jit.emitFunctionPrologue();
    jit.subPtr(CCallHelpers::TrustedImm32(static_cast<int32_t>(frameBytes)), CCallHelpers::stackPointerRegister);
    emitSaveTagRegisters(jit);
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));

    jit.storePtr(GPRInfo::callFrameRegister, &vm.topCallFrame);

    if (argumentCount) {
        JIT_COMMENT(jit, "arity check");
        slowPath.append(jit.branch32(CCallHelpers::Below, CCallHelpers::payloadFor(CallFrameSlot::argumentCountIncludingThis), CCallHelpers::TrustedImm32(argumentCount + 1)));
    }

    for (unsigned i = 0; i < argumentCount; ++i) {
        Type type = signature.argumentType(i);
        JIT_COMMENT(jit, "argument ", i, " : ", name(type));
        CCallHelpers::Address argument = CCallHelpers::addressFor(virtualRegisterForArgumentIncludingThis(static_cast<int>(i) + 1));
        emitConvertArgument(jit, type, argument, slotAddress(i), slowPath);
    }

    JIT_COMMENT(jit, "call invoke thunk");
    jit.addPtr(CCallHelpers::TrustedImm32(slotsOffsetFromFP), GPRInfo::callFrameRegister, GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImmPtr(target), GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImmPtr(invokeThunk.taggedPtr()), callTargetGPR);
    jit.call(callTargetGPR, OperationPtrTag);

    exceptionChecks.append(jit.emitExceptionCheck(vm));

    JIT_COMMENT(jit, "box return value : ", name(returnType));
    emitBoxReturnValue(jit, vm, globalObject, returnType, returnSlotAddress, exceptionChecks);

    emitRestoreTagRegisters(jit);
    jit.emitFunctionEpilogue();
    jit.ret();

    slowPath.link(&jit);
    JIT_COMMENT(jit, "slow path");
    jit.move(GPRInfo::callFrameRegister, GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImmPtr(globalObject), GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(operationFFICallSlowPath)), callTargetGPR);
    jit.call(callTargetGPR, OperationPtrTag);
    exceptionChecks.append(jit.emitExceptionCheck(vm));
    emitRestoreTagRegisters(jit);
    jit.emitFunctionEpilogue();
    jit.ret();

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


#endif // USE(JSVALUE64) && !ENABLE(JIT_CAGE)

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)
