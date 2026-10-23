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
#include "FFICallbackThunk.h"

#if ENABLE(FFI)

#include "ArgList.h"
#include "CCallHelpers.h"
#include "CallData.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "FFICallbackEntryPool.h"
#include "FFICallingConvention.h"
#include "FFIContext.h"
#include "FFIConversions.h"
#include "FFISignature.h"
#include "FFIStubEmitters.h"
#include "FFIType.h"
#include "JSCJSValueInlines.h"
#include "JSFFICallback.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "JSObjectInlines.h"
#include "LinkBuffer.h"
#include "MarkedVector.h"
#include "Options.h"
#include <wtf/MathExtras.h>

namespace JSC {

#if FFI_CALLBACK_THUNK_SUPPORTED

namespace FFI {

namespace {

#if CPU(X86_64)
constexpr GPRReg thunkScratchGPR = X86Registers::eax;
#else
constexpr GPRReg thunkScratchGPR = ARM64Registers::x9;
#endif

#if OS(WINDOWS) && CPU(X86_64)
constexpr unsigned win64XMMSaveCount = 10; // xmm6 .. xmm15
constexpr unsigned win64SaveAreaBytes = win64XMMSaveCount * 16 + 2 * 8; // 176, a multiple of 16
constexpr FPRReg win64SaveTempFPR = X86Registers::xmm4;
#else
constexpr unsigned win64SaveAreaBytes = 0;
#endif
static_assert(!(win64SaveAreaBytes % 16), "the Win64 save area keeps rsp 16-byte aligned");

#if OS(WINDOWS) && CPU(X86_64)
static void emitSaveWin64Nonvolatiles(CCallHelpers& jit, int saveAreaOffsetFromFP)
{
    for (unsigned i = 0; i < win64XMMSaveCount; ++i) {
        FPRReg reg = static_cast<FPRReg>(X86Registers::xmm6 + i);
        CCallHelpers::Address low(GPRInfo::callFrameRegister, saveAreaOffsetFromFP + static_cast<int>(i * 16));
        jit.storeDouble(reg, low);
        jit.vectorExtractLaneFloat64(CCallHelpers::TrustedImm32(1), reg, win64SaveTempFPR);
        jit.storeDouble(win64SaveTempFPR, low.withOffset(8));
    }
    jit.storePtr(X86Registers::esi, CCallHelpers::Address(GPRInfo::callFrameRegister, saveAreaOffsetFromFP + static_cast<int>(win64XMMSaveCount * 16)));
    jit.storePtr(X86Registers::edi, CCallHelpers::Address(GPRInfo::callFrameRegister, saveAreaOffsetFromFP + static_cast<int>(win64XMMSaveCount * 16 + 8)));
}

static void emitRestoreWin64Nonvolatiles(CCallHelpers& jit, int saveAreaOffsetFromFP)
{
    for (unsigned i = 0; i < win64XMMSaveCount; ++i) {
        FPRReg reg = static_cast<FPRReg>(X86Registers::xmm6 + i);
        CCallHelpers::Address low(GPRInfo::callFrameRegister, saveAreaOffsetFromFP + static_cast<int>(i * 16));
        jit.loadDouble(low, reg);
        jit.loadDouble(low.withOffset(8), win64SaveTempFPR);
        jit.vectorReplaceLaneFloat64(CCallHelpers::TrustedImm32(1), win64SaveTempFPR, reg);
    }
    jit.loadPtr(CCallHelpers::Address(GPRInfo::callFrameRegister, saveAreaOffsetFromFP + static_cast<int>(win64XMMSaveCount * 16)), X86Registers::esi);
    jit.loadPtr(CCallHelpers::Address(GPRInfo::callFrameRegister, saveAreaOffsetFromFP + static_cast<int>(win64XMMSaveCount * 16 + 8)), X86Registers::edi);
}
#endif // OS(WINDOWS) && CPU(X86_64)

static void emitStoreIncomingArgumentGPR(CCallHelpers& jit, Type type, GPRReg source, CCallHelpers::Address slot)
{
    GPRReg scratchGPR = thunkScratchGPR;
    switch (type) {
    case Type::Char:
    case Type::Int8:
        jit.signExtend8To64(source, scratchGPR);
        break;
    case Type::Uint8:
        jit.zeroExtend8To64(source, scratchGPR);
        break;
    case Type::Int16:
        jit.signExtend16To64(source, scratchGPR);
        break;
    case Type::Uint16:
        jit.zeroExtend16To64(source, scratchGPR);
        break;
    case Type::Int32:
        jit.signExtend32To64(source, scratchGPR);
        break;
    case Type::Uint32:
        jit.zeroExtend32ToWord(source, scratchGPR);
        break;
    case Type::Bool:
        jit.and32(CCallHelpers::TrustedImm32(0xff), source, scratchGPR);
        jit.compare32(CCallHelpers::NotEqual, scratchGPR, CCallHelpers::TrustedImm32(0), scratchGPR);
        break;
    case Type::Int64:
    case Type::Uint64:
    case Type::Int64Fast:
    case Type::Uint64Fast:
    case Type::Pointer:
    case Type::CString:
    case Type::Function:
    case Type::Buffer:
    case Type::BufferLength: // a callback parameter is a plain unsigned 64-bit length (like Uint64)
    case Type::JSValue:
        jit.store64(source, slot);
        return;
    case Type::Float:
    case Type::Double:
    case Type::Void:
    case Type::RESERVED_WasNapiEnv:
        RELEASE_ASSERT_NOT_REACHED(); // Never GPR-classed / never a valid argument.
    }
    jit.store64(scratchGPR, slot);
}

static void emitStoreIncomingArgumentStack(CCallHelpers& jit, Type type, CCallHelpers::Address source, CCallHelpers::Address slot)
{
    GPRReg scratchGPR = thunkScratchGPR;
    switch (type) {
    case Type::Char:
    case Type::Int8:
        jit.load8SignedExtendTo32(source, scratchGPR);
        jit.signExtend32To64(scratchGPR, scratchGPR);
        break;
    case Type::Uint8:
        jit.load8(source, scratchGPR);
        break;
    case Type::Int16:
        jit.load16SignedExtendTo32(source, scratchGPR);
        jit.signExtend32To64(scratchGPR, scratchGPR);
        break;
    case Type::Uint16:
        jit.load16(source, scratchGPR);
        break;
    case Type::Int32:
        jit.load32(source, scratchGPR);
        jit.signExtend32To64(scratchGPR, scratchGPR);
        break;
    case Type::Uint32:
        jit.load32(source, scratchGPR); // A 32-bit load zero-extends the full register.
        break;
    case Type::Bool:
        jit.load8(source, scratchGPR);
        jit.compare32(CCallHelpers::NotEqual, scratchGPR, CCallHelpers::TrustedImm32(0), scratchGPR);
        break;
    case Type::Float:
        jit.load32(source, scratchGPR);
        break;
    case Type::Int64:
    case Type::Uint64:
    case Type::Int64Fast:
    case Type::Uint64Fast:
    case Type::Double:
    case Type::Pointer:
    case Type::CString:
    case Type::Function:
    case Type::Buffer:
    case Type::BufferLength: // a callback parameter is a plain unsigned 64-bit length (like Uint64)
    case Type::JSValue:
        jit.load64(source, scratchGPR);
        break;
    case Type::Void:
    case Type::RESERVED_WasNapiEnv:
        RELEASE_ASSERT_NOT_REACHED(); // Never an argument.
    }
    jit.store64(scratchGPR, slot);
}

static void emitStoreIncomingArgumentFPR(CCallHelpers& jit, Type type, FPRReg source, CCallHelpers::Address slot)
{
    switch (type) {
    case Type::Float:
        jit.storeFloat(source, slot);
        jit.store32(CCallHelpers::TrustedImm32(0), slot.withOffset(4));
        break;
    case Type::Double:
        jit.storeDouble(source, slot);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED(); // Only Float / Double are FPR-classed.
    }
}

} // anonymous namespace

MacroAssemblerCodeRef<JITThunkPtrTag> generateCallbackThunk(VM&, JSFFICallback& callback)
{
    Signature& signature = callback.signature();
    const NativeCC cc = hostNativeCC();
    const CallLayout layout = computeCallLayout(cc, signature, Direction::Incoming);
    const auto integerArgumentGPRs = integerArgumentRegisters(cc);
    const auto floatArgumentFPRs = floatArgumentRegisters(cc);

#if ASSERT_ENABLED
    for (GPRReg argumentGPR : integerArgumentGPRs)
        ASSERT(argumentGPR != thunkScratchGPR);
#endif

    const unsigned argumentCount = signature.argumentCount();
    const unsigned slotBufferBytes = static_cast<unsigned>(signature.slotBufferBytes());
    ASSERT(slotBufferBytes == signature.slotCount() * slotSize);

    const unsigned frameBytes = win64SaveAreaBytes + WTF::roundUpToMultipleOf<16>(slotBufferBytes);
    const int slotsOffsetFromFP = -static_cast<int>(frameBytes);
#if OS(WINDOWS) && CPU(X86_64)
    const int saveAreaOffsetFromFP = -static_cast<int>(win64SaveAreaBytes);
#endif

    CCallHelpers jit;

    jit.emitFunctionPrologue();

    jit.subPtr(CCallHelpers::TrustedImm32(frameBytes), CCallHelpers::stackPointerRegister);

#if OS(WINDOWS) && CPU(X86_64)
    emitSaveWin64Nonvolatiles(jit, saveAreaOffsetFromFP);
#endif

    for (unsigned i = 0; i < argumentCount; ++i) {
        const ArgLocation& location = layout.arguments[i];
        const Type type = signature.argumentType(i);
        ASSERT(location.type == type);
        const CCallHelpers::Address slot(GPRInfo::callFrameRegister, slotsOffsetFromFP + static_cast<int>(i * slotSize));

        switch (location.kind) {
        case ArgLocation::Kind::GPR:
            RELEASE_ASSERT(location.regIndex < integerArgumentGPRs.size());
            emitStoreIncomingArgumentGPR(jit, type, integerArgumentGPRs[location.regIndex], slot);
            break;
        case ArgLocation::Kind::FPR:
            RELEASE_ASSERT(location.regIndex < floatArgumentFPRs.size());
            emitStoreIncomingArgumentFPR(jit, type, floatArgumentFPRs[location.regIndex], slot);
            break;
        case ArgLocation::Kind::Stack: {
            const CCallHelpers::Address source(GPRInfo::callFrameRegister, static_cast<int>(incomingStackOffset(layout, i)));
            emitStoreIncomingArgumentStack(jit, type, source, slot);
            break;
        }
        }
    }

    jit.addPtr(CCallHelpers::TrustedImm32(slotsOffsetFromFP), GPRInfo::callFrameRegister, GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImmPtr(&callback), GPRInfo::argumentGPR0);
    auto dispatchOperation = callback.isThreadsafe() ? tagCFunction<OperationPtrTag>(ffiCallbackDispatchThreadsafe) : tagCFunction<OperationPtrTag>(ffiCallbackDispatch);
    jit.move(CCallHelpers::TrustedImmPtr(dispatchOperation), thunkScratchGPR);
    jit.call(thunkScratchGPR, OperationPtrTag);

    const CCallHelpers::Address returnSlot(GPRInfo::callFrameRegister, slotsOffsetFromFP + static_cast<int>(argumentCount * slotSize));
    switch (layout.returnClass) {
    case ArgClass::Void:
        break;
    case ArgClass::Int:
        jit.load64(returnSlot, GPRInfo::returnValueGPR);
        break;
    case ArgClass::Float:
        jit.loadFloat(returnSlot, FPRInfo::returnValueFPR);
        break;
    case ArgClass::Double:
        jit.loadDouble(returnSlot, FPRInfo::returnValueFPR);
        break;
    }

#if OS(WINDOWS) && CPU(X86_64)
    emitRestoreWin64Nonvolatiles(jit, saveAreaOffsetFromFP);
#endif
    jit.emitFunctionEpilogue();
    jit.ret();

    LinkBuffer patchBuffer(jit, GLOBAL_THUNK_ID, LinkBuffer::Profile::Thunk, JITCompilationCanFail);
    if (patchBuffer.didFailToAllocate()) [[unlikely]]
        return { };

    patchBuffer.setIsThunk();
    return FINALIZE_CODE_IF(Options::dumpDisassembly() || Options::dumpFFIDisassembly(), patchBuffer, JITThunkPtrTag, "FFICallbackThunk"_s, "FFI callback %s", signature.toString().ascii().data());
}

#if FFI_STUBS_SUPPORTED

Vector<CCallHelpers::Label> emitCallbackEntries(CCallHelpers& jit, unsigned numberOfEntries, const ContextAddressEmitter& moveContextAddress)
{
    constexpr NativeCC cc = hostNativeCC();
    const auto integerArgumentGPRs = integerArgumentRegisters(cc);
    const auto floatArgumentFPRs = floatArgumentRegisters(cc);

    constexpr unsigned frameBytes = win64SaveAreaBytes + static_cast<unsigned>(WTF::roundUpToMultipleOf<16>(sizeof(CallbackRegisters)));
    constexpr int registersOffsetFromFP = -static_cast<int>(frameBytes);
#if OS(WINDOWS) && CPU(X86_64)
    constexpr int saveAreaOffsetFromFP = -static_cast<int>(win64SaveAreaBytes);
#endif

    Vector<CCallHelpers::Label> entries;
    entries.reserveInitialCapacity(numberOfEntries);
    CCallHelpers::JumpList toBody;
    for (unsigned i = 0; i < numberOfEntries; ++i) {
        entries.append(jit.label());
        jit.move(CCallHelpers::TrustedImm32(i), thunkScratchGPR);
        toBody.append(jit.jump());
    }

    toBody.link(&jit);
    jit.emitFunctionPrologue();

    jit.subPtr(CCallHelpers::TrustedImm32(frameBytes), CCallHelpers::stackPointerRegister);

#if OS(WINDOWS) && CPU(X86_64)
    emitSaveWin64Nonvolatiles(jit, saveAreaOffsetFromFP);
#endif

    for (unsigned i = 0; i < integerArgumentGPRs.size(); ++i)
        jit.store64(integerArgumentGPRs[i], CCallHelpers::Address(GPRInfo::callFrameRegister, registersOffsetFromFP + static_cast<int>(CallbackRegisters::offsetOfGPR(i))));
    for (unsigned i = 0; i < floatArgumentFPRs.size(); ++i)
        jit.storeDouble(floatArgumentFPRs[i], CCallHelpers::Address(GPRInfo::callFrameRegister, registersOffsetFromFP + static_cast<int>(CallbackRegisters::offsetOfFPR(i))));

    jit.move(thunkScratchGPR, GPRInfo::argumentGPR0);
    jit.addPtr(CCallHelpers::TrustedImm32(registersOffsetFromFP), GPRInfo::callFrameRegister, GPRInfo::argumentGPR1);
    jit.move(GPRInfo::callFrameRegister, GPRInfo::argumentGPR2);
    moveContextAddress(jit, thunkScratchGPR);
    jit.loadPtr(CCallHelpers::Address(thunkScratchGPR, CallbackEntryContext::offsetOfEnter()), thunkScratchGPR);
    jit.call(thunkScratchGPR, OperationPtrTag);

    // The body serves every signature: the caller reads the return register its signature names.
    const CCallHelpers::Address returnValue(GPRInfo::callFrameRegister, registersOffsetFromFP + static_cast<int>(CallbackRegisters::offsetOfReturnValue()));
    jit.load64(returnValue, GPRInfo::returnValueGPR);
    jit.loadDouble(returnValue, FPRInfo::returnValueFPR);

#if OS(WINDOWS) && CPU(X86_64)
    emitRestoreWin64Nonvolatiles(jit, saveAreaOffsetFromFP);
#endif
    jit.emitFunctionEpilogue();
    jit.ret();

    return entries;
}

#endif // FFI_STUBS_SUPPORTED

} // namespace FFI

namespace FFI {

class CallbackEntryScope {
    WTF_MAKE_NONCOPYABLE(CallbackEntryScope);
public:
    explicit CallbackEntryScope(VM& vm)
        : m_vm(vm)
    {
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
        m_savedNeedExceptionCheck = m_vm.m_needExceptionCheck;
        if (m_savedNeedExceptionCheck) {
            m_savedThrowPointRecursionDepth = m_vm.m_simulatedThrowPointRecursionDepth;
            m_savedThrowPointLocation = m_vm.m_simulatedThrowPointLocation;
            m_savedNativeStackTraceOfLastSimulatedThrow = WTF::move(m_vm.m_nativeStackTraceOfLastSimulatedThrow);
            m_vm.m_needExceptionCheck = false;
        }
#endif
    }

    ~CallbackEntryScope()
    {
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
        if (m_vm.m_needExceptionCheck || !m_savedNeedExceptionCheck)
            return;
        m_vm.m_needExceptionCheck = true;
        m_vm.m_simulatedThrowPointRecursionDepth = m_savedThrowPointRecursionDepth;
        m_vm.m_simulatedThrowPointLocation = m_savedThrowPointLocation;
        m_vm.m_nativeStackTraceOfLastSimulatedThrow = WTF::move(m_savedNativeStackTraceOfLastSimulatedThrow);
#endif
    }

private:
    VM& m_vm;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    bool m_savedNeedExceptionCheck { false };
    unsigned m_savedThrowPointRecursionDepth { 0 };
    ExceptionEventLocation m_savedThrowPointLocation;
    std::unique_ptr<StackTrace> m_savedNativeStackTraceOfLastSimulatedThrow;
#endif
};

} // namespace FFI

JSC_DEFINE_JIT_OPERATION(ffiCallbackDispatchThreadsafe, EncodedJSValue, (JSFFICallback* callback, uint64_t* slots))
{
    ASSERT(callback->isThreadsafe());
    FFI::Signature& signature = callback->signature();
    const unsigned argumentCount = signature.argumentCount();
    auto dispatch = FFI::FFIContext::threadsafeDispatch();
    RELEASE_ASSERT(dispatch);
    if (callback->tryBeginThreadsafeInvocation()) [[likely]] {
        auto invocation = FFI::ThreadsafeInvocation::create(callback, callback->embedderContext(), std::span<const uint64_t>(slots, argumentCount));
        dispatch(invocation.get());
    }
    slots[argumentCount] = 0;
    return { encodedJSUndefined(), nullptr };
}

JSC_DEFINE_JIT_OPERATION(ffiCallbackDispatch, EncodedJSValue, (JSFFICallback* callback, uint64_t* slots))
{
    ASSERT(!callback->isThreadsafe());
    JSGlobalObject* globalObject = callback->globalObject();
    VM& vm = globalObject->vm();
    JSLockHolder locker(vm);

    FFI::Signature& signature = callback->signature();
    const unsigned argumentCount = signature.argumentCount();
    uint64_t& returnSlot = slots[argumentCount];

    if (Exception* pendingException = vm.exceptionForInspection()) [[unlikely]] {
        returnSlot = 0;
        return { encodedJSUndefined(), pendingException };
    }

    FFI::CallbackEntryScope entryScope(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);

    FFI::FFIContext& context = globalObject->ffiContext();

    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < argumentCount; ++i) {
        FFI::Type type = signature.argumentType(i);
        arguments.append(FFI::jsValueFromSlot(globalObject, context, type, slots[i]));
        if (scope.exception()) [[unlikely]] {
            returnSlot = 0;
            OPERATION_RETURN(scope, encodedJSUndefined());
        }
    }
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        returnSlot = 0;
        OPERATION_RETURN(scope, encodedJSUndefined());
    }

    JSObject* callable = callback->callable();
    CallData callData = JSC::getCallData(callable);
    if (callData.type == CallData::Type::None) [[unlikely]] {
        throwTypeError(globalObject, scope, "FFI callback target is not callable"_s);
        returnSlot = 0;
        OPERATION_RETURN(scope, encodedJSUndefined());
    }

    JSValue result = profiledCall(globalObject, ProfilingReason::API, callable, callData, jsUndefined(), arguments);
    if (scope.exception()) [[unlikely]] {
        returnSlot = 0;
        OPERATION_RETURN(scope, encodedJSUndefined());
    }

    if (signature.returnType() != FFI::Type::Void) {
        if (signature.returnType() == FFI::Type::CString && result.isString()) {
            String string = result.toWTFString(globalObject);
            if (scope.exception()) [[unlikely]] {
                returnSlot = 0;
                OPERATION_RETURN(scope, encodedJSUndefined());
            }
            returnSlot = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(callback->setReturnCString(string.utf8())));
        } else {
            FFI::writeSlotFromJSValue(globalObject, context, signature.returnType(), result, returnSlot, nullptr);
            if (scope.exception()) [[unlikely]] {
                returnSlot = 0;
                OPERATION_RETURN(scope, encodedJSUndefined());
            }
        }
    }

    OPERATION_RETURN(scope, JSValue::encode(result));
}

static uint64_t readIncomingArgument(const FFI::CallLayout& layout, unsigned index, const FFI::CallbackRegisters& registers, const void* frame)
{
    const FFI::ArgLocation& location = layout.arguments[index];
    switch (location.kind) {
    case FFI::ArgLocation::Kind::GPR:
        return registers.gprs[location.regIndex];
    case FFI::ArgLocation::Kind::FPR:
        return registers.fprs[location.regIndex];
    case FFI::ArgLocation::Kind::Stack: {
        uint64_t raw = 0;
        memcpy(&raw, static_cast<const uint8_t*>(frame) + FFI::incomingStackOffset(layout, index), layout.packing == FFI::StackPacking::Natural ? FFI::nativeSizeInBytes(location.type) : 8);
        return raw;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(ffiCallbackEnter, void, (uint32_t entry, FFI::CallbackRegisters* registers, const void* frame))
{
    JSFFICallback* callback = FFI::CallbackEntryPool::singleton().callbackAt(entry);
    RELEASE_ASSERT(callback);

    FFI::Signature& signature = callback->signature();
    const FFI::CallLayout& layout = signature.callLayout();
    const unsigned argumentCount = signature.argumentCount();
    ASSERT(argumentCount <= FFI::Signature::maxArguments);
    uint64_t slots[FFI::Signature::maxArguments + 1];
    for (unsigned i = 0; i < argumentCount; ++i)
        slots[i] = FFI::normalizeSlot(signature.argumentType(i), readIncomingArgument(layout, i, *registers, frame));
    slots[argumentCount] = 0;

    if (callback->isThreadsafe())
        ffiCallbackDispatchThreadsafe(callback, slots);
    else
        ffiCallbackDispatch(callback, slots);

    registers->returnValue = slots[argumentCount];
}

namespace FFI {

void runThreadsafeInvocation(ThreadsafeInvocation& invocation)
{
    JSFFICallback* callback = invocation.callback();
    struct RetireInvocation {
        JSFFICallback* callback;
        ~RetireInvocation()
        {
            if (callback->endThreadsafeInvocation())
                callback->unroot();
        }
    } retire { callback };

    JSGlobalObject* globalObject = callback->globalObject();
    VM& vm = globalObject->vm();
    JSLockHolder locker(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Signature& signature = callback->signature();
    FFIContext& context = globalObject->ffiContext();

    std::span<const uint64_t> slots = invocation.slots();
    ASSERT(slots.size() == signature.argumentCount());
    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < signature.argumentCount(); ++i) {
        arguments.append(jsValueFromSlot(globalObject, context, signature.argumentType(i), slots[i]));
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return;
    }

    JSObject* callable = callback->callable();
    CallData callData = JSC::getCallData(callable);
    if (callData.type == CallData::Type::None) [[unlikely]] {
        throwTypeError(globalObject, scope, "FFI callback target is not callable"_s);
        return;
    }
    profiledCall(globalObject, ProfilingReason::API, callable, callData, jsUndefined(), arguments);
    RETURN_IF_EXCEPTION(scope, void());
}

} // namespace FFI

#else // !FFI_CALLBACK_THUNK_SUPPORTED

namespace FFI {
void runThreadsafeInvocation(ThreadsafeInvocation&)
{
    RELEASE_ASSERT_NOT_REACHED(); // unreachable: no threadsafe callback exists to have queued this
}
} // namespace FFI

#endif // FFI_CALLBACK_THUNK_SUPPORTED

} // namespace JSC

#endif // ENABLE(FFI)
