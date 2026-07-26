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
#include "FFICallbackThunk.h"

#if USE(BUN_JSC_ADDITIONS)

#include "ArgList.h"
#include "CCallHelpers.h"
#include "CallData.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "FFICallingConvention.h"
#include "FFIContext.h"
#include "FFIConversions.h"
#include "FFISignature.h"
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

// Scratch GPR usable while incoming native arguments are still live in their
// argument registers: it must not be an integer argument register of ANY
// NativeCC on this CPU, must not be the MacroAssembler implicit scratch
// (r11 on x86-64, x16/x17 on arm64), and it is only ever a volatile register
// in every native CC (rax / x9), so nothing has to preserve it. It also holds
// the ffiCallbackDispatch target for the operation call.
#if CPU(X86_64)
constexpr GPRReg thunkScratchGPR = X86Registers::eax;
#else
constexpr GPRReg thunkScratchGPR = ARM64Registers::x9;
#endif

// Win64 nonvolatile save area (SPEC section 9.2 step 1). The full 128 bits
// of xmm6-xmm15 plus rsi and rdi are callee-saved in the Microsoft x64 ABI but
// caller-saved in the SysV ABI that ffiCallbackDispatch (JIT_OPERATION_ATTRIBUTES
// = SYSV_ABI on Windows) is compiled with, so a Win64-entered thunk that calls
// SysV C++ must preserve them explicitly. These saves must not be "optimized"
// away: step 4's call targets rdi/rsi (argumentGPR0/1) directly and the
// dispatch may clobber xmm6-xmm15 freely.
#if OS(WINDOWS) && CPU(X86_64)
constexpr unsigned win64XMMSaveCount = 10; // xmm6 .. xmm15
constexpr unsigned win64SaveAreaBytes = win64XMMSaveCount * 16 + 2 * 8; // 176, a multiple of 16
// xmm4 is volatile in the Win64 ABI and is never an argument register (Win64
// passes only four FP arguments, in xmm0-xmm3), so it can be clobbered before
// the incoming arguments have been spilled. It is deliberately NOT
// fpTempRegister (xmm15), which is one of the registers being saved.
constexpr FPRReg win64SaveTempFPR = X86Registers::xmm4;
#else
constexpr unsigned win64SaveAreaBytes = 0;
#endif
static_assert(!(win64SaveAreaBytes % 16), "the Win64 save area keeps rsp 16-byte aligned");

#if OS(WINDOWS) && CPU(X86_64)
// FFI-SPEC-GAP: SPEC section 9.2 step 1 asks for storeVector/loadVector
// (movups) here, but MacroAssemblerX86_64's storeVector/loadVector emit the
// AVX vmovups encoding and ASSERT(supportsAVX() && Options::useWasmSIMD()) --
// the Windows x64 build targets the nehalem (SSE4.2, no AVX) floor. Each
// 128-bit register is therefore saved as two 64-bit halves with SSE2-level
// instructions (movsd + movhlps / unpcklpd via the vectorExtractLane /
// vectorReplaceLane helpers, which take their non-AVX paths when AVX is
// absent), still preserving the full 128 bits the ABI requires.
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
    // The native return value already sits in rax / xmm0; this touches only
    // xmm6-xmm15, xmm4, rsi and rdi.
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

// Writes an incoming integer-class argument that arrived in an argument GPR
// into its canonical slot (SPEC section 4). Only the bits the native caller
// was required to fill are trusted; sub-word values are re-extended from
// their width and Type::Bool is normalized to {0, 1} from its low byte.
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
        // Only the low byte of a bool argument is ABI-defined (SysV, Win64 and
        // AAPCS64 alike); the slot MUST end up exactly 0 or 1.
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

// Writes an incoming argument that arrived on the stack into its canonical
// slot. Loads are sized to the type's native width for BOTH stack packings:
// the padding bits above a sub-8-byte value are unspecified in every ABI
// (and do not even exist under the Apple arm64 natural packing, SPEC section
// 7.1.1), so a blind load64 would trust caller garbage.
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
        // Bits [31:0] carry the float and bits [63:32] must be zero (SPEC
        // section 4): a zero-extending 32-bit load produces exactly that.
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

// Writes an incoming floating-point argument that arrived in an argument FPR
// into its canonical slot.
static void emitStoreIncomingArgumentFPR(CCallHelpers& jit, Type type, FPRReg source, CCallHelpers::Address slot)
{
    switch (type) {
    case Type::Float:
        // f32 slots must have bits [63:32] zeroed (SPEC section 4 / 9.2 step 3).
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

    // Frame (all offsets are relative to the frame pointer set up by the
    // native-ABI prologue):
    //   [fp - win64SaveAreaBytes, fp)                    Win64 nonvolatile save area (Windows x64 only)
    //   [fp - frameBytes, fp - frameBytes + slotBufferBytes)  canonical slot buffer
    // frameBytes is a multiple of 16, so the stack pointer stays 16-byte aligned
    // at the ffiCallbackDispatch call on every ABI (x86-64: the caller's call
    // pushed 8 bytes and emitFunctionPrologue pushes rbp; arm64: sp is always a
    // multiple of 16 and pushPair keeps it so).
    const unsigned frameBytes = win64SaveAreaBytes + WTF::roundUpToMultipleOf<16>(slotBufferBytes);
    const int slotsOffsetFromFP = -static_cast<int>(frameBytes);
#if OS(WINDOWS) && CPU(X86_64)
    const int saveAreaOffsetFromFP = -static_cast<int>(win64SaveAreaBytes);
#endif

    CCallHelpers jit;

    // Step 1: native-ABI prologue.
    jit.emitFunctionPrologue();

    // Step 2: allocate the Win64 save area (if any) and the slot buffer.
    jit.subPtr(CCallHelpers::TrustedImm32(frameBytes), CCallHelpers::stackPointerRegister);

#if OS(WINDOWS) && CPU(X86_64)
    emitSaveWin64Nonvolatiles(jit, saveAreaOffsetFromFP);
#endif

    // Step 3: spill every incoming native argument into its canonical slot.
    // The slot buffer is otherwise left uninitialized (the void return slot
    // is never read), except that f32 slots zero their upper halves.
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

    // Step 4: call SYSV EncodedJSValue ffiCallbackDispatch(JSFFICallback*,
    // uint64_t* slots) through the JSC operation calling convention. slotsBase
    // is re-derived FP-relative into argumentGPR1 (never carried in a volatile
    // register across the call); on Windows x64 this is a SysV call issued
    // from a Win64-entered frame -- hence step 2's saves. topCallFrame is NOT
    // touched: the outer FFI call site established it and the vmEntry inside
    // profiledCall saves/restores it (SPEC section 9.3).
    jit.addPtr(CCallHelpers::TrustedImm32(slotsOffsetFromFP), GPRInfo::callFrameRegister, GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImmPtr(&callback), GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImmPtr(tagCFunction<OperationPtrTag>(ffiCallbackDispatch)), thunkScratchGPR);
    jit.call(thunkScratchGPR, OperationPtrTag);

    // Step 5: load the (fully normalized, SPEC section 4) return slot into
    // the native return register for the signature's return class. Any JS
    // exception raised by the callback stays pending on the VM; the outer FFI
    // call's exception check surfaces it once native code returns.
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

    // Step 6: restore the Win64 nonvolatiles, native epilogue, return.
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

} // namespace FFI

namespace FFI {

// A callback dispatch is entered from FOREIGN C frames that stand between
// an outstanding exception-check obligation and its designated checker: the
// STILL-LIVE scope of the outer FFI call site (FFI::ffiCall / the JIT'd
// stubs), which performs its exception check only once native code returns.
// A previous callback dispatch on this same native call may have armed
// that obligation (a real exception left pending for the outer site, or --
// under Options::validateExceptionChecks() -- a simulated throw): the C
// caller can never satisfy it, and the exception-check verifier does not
// model "checker resumes after foreign frames", so constructing any scope
// here would trip verifyExceptionCheckNeedIsSatisfied.
//
// This RAII suspends the verifier's outstanding obligation across the nested
// entry (analogous to how ~ThrowScope's willBeHandleByLLIntOrJIT models a
// non-scope checker) and restores it on exit, unioned with any obligation
// this dispatch itself armed for that same checker. It manages verifier
// bookkeeping ONLY; a REAL pending exception is handled separately by the
// no-op early return in ffiCallbackDispatch, before any scope exists.
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
        // If this dispatch's own scopes armed a fresh obligation (for the very
        // same outer checker), the newer one wins; otherwise reinstate the
        // suspended one so the outer FFI call site's check is still verified.
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

// SPEC section 9.3. Entered from the native-ABI callback thunk, i.e.
// ultimately from FOREIGN C code: there is no JSC CallFrame at
// __builtin_frame_address(1), so this body deliberately contains NO
// DECLARE_CALL_FRAME, JITOperationPrologueCallFrameTracer,
// SlowPathFrameTracer or write to vm.topCallFrame -- the outer FFI call site
// (host path / IC stub / DFG-FTL) established topCallFrame before entering
// native code, and the vmEntry inside profiledCall saves and restores it.
JSC_DEFINE_JIT_OPERATION(ffiCallbackDispatch, EncodedJSValue, (JSFFICallback* callback, uint64_t* slots))
{
    // THREADSAFE branch, FIRST, before touching the global object / VM / lock: a threadsafe
    // callback may be entered from a FOREIGN thread, so nothing here may assume the JS thread.
    // We only read plain immutable-after-creation cell fields and copy the raw C argument
    // words (no JS values are created off-thread -- that is the whole point: BigInt/heap boxing
    // for i64/u64/large pointers happens later, on the JS thread, in runThreadsafeInvocation).
    if (callback->isThreadsafe()) [[unlikely]] {
        FFI::Signature& signature = callback->signature();
        const unsigned argumentCount = signature.argumentCount();
        auto dispatch = FFI::FFIContext::threadsafeDispatch();
        RELEASE_ASSERT(dispatch); // creation refused a threadsafe callback with no dispatch registered
        // Count the invocation BEFORE handing it off (a single CAS): the cell then stays rooted
        // until it drains, and the increment cannot race close() -- if close() already ran, the
        // closed bit is visible and we do NOT dispatch (no record referencing an unrooted cell).
        if (callback->tryBeginThreadsafeInvocation()) [[likely]] {
            auto invocation = FFI::ThreadsafeInvocation::create(callback, callback->embedderContext(), std::span<const uint64_t>(slots, argumentCount));
            dispatch(invocation.get()); // embedder queues to its JS thread; keeps its own ref
        }
        slots[argumentCount] = 0; // a threadsafe return value is undefined by nature: zero it
        return { encodedJSUndefined(), nullptr };
    }

    JSGlobalObject* globalObject = callback->globalObject();
    VM& vm = globalObject->vm();
    // The JS thread that made the outer FFI call already holds the lock; this
    // is the cheap re-entrant path (calling a callback from a foreign thread
    // is undefined behaviour in v1, exactly like Bun's non-threadsafe
    // callbacks).
    JSLockHolder locker(vm);

    FFI::Signature& signature = callback->signature();
    const unsigned argumentCount = signature.argumentCount();
    uint64_t& returnSlot = slots[argumentCount];

    // Native code may call the callback again while an exception thrown by
    // an earlier invocation is still pending (the outer FFI call has not
    // returned yet, e.g. a native loop like ffi_call_cb_reentrant). Calling
    // into JS with a pending exception is forbidden, so such re-invocations
    // are no-ops: the pending exception stays put for the outer FFI call
    // site's exception check to surface, and the native caller reads a zero
    // return slot. This check runs before ANY scope is constructed and uses
    // exceptionForInspection(), which touches no verifier state.
    if (Exception* pendingException = vm.exceptionForInspection()) [[unlikely]] {
        returnSlot = 0;
        return { encodedJSUndefined(), pendingException };
    }

    // Suspend the outer FFI call site's outstanding (real or simulated)
    // exception-check obligation across this foreign-code re-entry; see
    // FFI::CallbackEntryScope. Declared before `scope` so it destructs after
    // it, unioning any obligation this dispatch's own scopes arm.
    FFI::CallbackEntryScope entryScope(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);

    FFI::FFIContext& context = globalObject->ffiContext();
    // FFI-SPEC-GAP: the spec names the RAII bracket "StringArena::Scope"
    // (section 9.3 step 2 / section 8.2 step 1) constructed from the
    // FFIContext, and passes "&arena" to writeSlotFromJSValue without naming
    // the accessor; FFIContext::stringArena() is the assumed accessor for the
    // context's call-scoped arena.
    FFI::StringArena::Scope arenaScope(context);

    // The dispatch's own failures (bad callable, OOM building the argument
    // list, a throwing conversion, or an exception thrown by the JS call) all
    // take the same exit: the exception is left PENDING on the VM for the outer
    // FFI call site's exception check to surface, and the native caller sees a
    // zero return slot (SPEC section 9.3 step 5).
    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < argumentCount; ++i) {
        FFI::Type type = signature.argumentType(i);
        // jsValueFromSlot only throws on out-of-memory (heap BigInt
        // allocation); check per iteration so nothing else allocates or
        // converts under a pending exception.
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
    // FFI-SPEC-GAP: the spec calls profiledCall unconditionally; a non-callable
    // target (impossible from Bun's glue, which validates the function) is
    // reported as a TypeError instead of tripping the ASSERT inside call().
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
        FFI::writeSlotFromJSValue(globalObject, context, signature.returnType(), result, returnSlot, &context.stringArena());
        if (scope.exception()) [[unlikely]] {
            returnSlot = 0;
            OPERATION_RETURN(scope, encodedJSUndefined());
        }
    }

    // The thunk ignores this value (it reads the return slot); it is returned
    // only so the JS result is visible in a debugger.
    OPERATION_RETURN(scope, JSValue::encode(result));
}

namespace FFI {

// JS-thread half of a threadsafe callback. The embedder's dispatch function queued
// `invocation`; its event loop calls this on the JS thread. Converts the copied raw slots to
// JS values via the SAME jsValueFromSlot the synchronous path uses (so BigInt boxing for
// i64/u64/large pointers is done here, on the JS thread, never off-thread) and invokes the
// callable. The C caller received 0 long ago, so the JS return value is discarded. Delivery is
// UNCONDITIONAL, even if close() ran after this record was accepted: close() only refuses NEW
// foreign-thread calls; an invocation accepted while open is a commitment. Any exception is
// left pending for the embedder's task machinery to report as it does for any
// posted task -- there is no outer FFI call site to surface it to.
void runThreadsafeInvocation(ThreadsafeInvocation& invocation)
{
    JSFFICallback* callback = invocation.callback();
    // This invocation was counted by ffiCallbackDispatch, so the cell is still rooted here even
    // if close() already ran. Retire the count on every exit; if this was the last pending
    // invocation of a CLOSED callback, unroot now (the deferred half of close()).
    struct RetireInvocation {
        JSFFICallback* callback;
        ~RetireInvocation()
        {
            if (callback->endThreadsafeInvocation())
                callback->unroot();
        }
    } retire { callback };

    // Deliver even if close() has since run. close() only stops NEW foreign-thread calls from
    // being accepted (tryBeginThreadsafeInvocation() refuses once closed); an invocation that
    // was accepted while the callback was open is a commitment and must still reach the JS
    // callable (Bun parity: cc()/ffi threadsafe callbacks drain their queue after close()). The
    // count keeps the cell rooted until this drains, so the callable is guaranteed alive here.
    JSGlobalObject* globalObject = callback->globalObject();
    VM& vm = globalObject->vm();
    JSLockHolder locker(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Signature& signature = callback->signature();
    FFIContext& context = globalObject->ffiContext();
    StringArena::Scope arenaScope(context);

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

// The callback thunk machinery is compiled out on this configuration (no JIT / 32-bit /
// JIT-caged / unsupported CPU), so no threadsafe callback can ever be created and no
// ThreadsafeInvocation can exist. runThreadsafeInvocation is still declared unconditionally in
// BunFFI.h (embedders and $vm link against it), so provide the degraded definition here rather
// than leave an undefined symbol -- matching how the creation entry points degrade to a
// runtime error instead of a link error.
namespace FFI {
void runThreadsafeInvocation(ThreadsafeInvocation&)
{
    RELEASE_ASSERT_NOT_REACHED(); // unreachable: no threadsafe callback exists to have queued this
}
} // namespace FFI

#endif // FFI_CALLBACK_THUNK_SUPPORTED

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
