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

#if USE(BUN_JSC_ADDITIONS)

#include "DFGSpeculativeJIT.h"

#if ENABLE(DFG_JIT) && USE(JSVALUE64)

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#include "DFGSlowPathGenerator.h"
#include "FFIContext.h"
#include "FFIConversions.h"
#include "FFISignature.h"
#include "FFIType.h"
#include "JSArrayBufferView.h"
#include "JSCast.h"
#include "JSFFIFunction.h"
#include "JSGlobalObject.h"

namespace JSC { namespace DFG {

// SPEC docs/ffi/SPEC.md section 10.4. There is no in-tree DFG-tier precedent for a
// var-args native call (CallWasm DFG_CRASHes in this tier), so the structure below is
// the whole contract: canonical slot buffer in the SP-relative outgoing area, unboxed
// stores per FFI::Type, a direct call to the per-signature invoke thunk with
// vm.topCallFrame stored explicitly, an exception check, and a per-return-type box.

// A JS -> slot conversion of one of these types may transcode a JS string (cstring) or
// otherwise reach the C++ conversion path that allocates from the call-scoped StringArena
// (spec section 5), so any UntypedUse argument of these types brackets the whole call with
// operationFFIArenaEnter / operationFFIArenaExit.
static bool ffiUntypedConversionMayUseStringArena(FFI::Type type)
{
    switch (type) {
    case FFI::Type::CString:
    case FFI::Type::Pointer:
    case FFI::Type::Function:
    case FFI::Type::Buffer:
        return true;
    default:
        return false;
    }
}

void SpeculativeJIT::compileCallFFI(Node* node)
{
    JSFFIFunction* function = node->ffiFunction();
    FFI::Signature& signature = node->ffiSignature();
    void* target = function->target();
    JSGlobalObject* globalObject = function->globalObject();

    // FFI-SPEC-GAP: the spec (10.4 step 4) hands `&ctx` and `ctx.addressOfNapiEnv()` to the
    // emitted code as immediates, which requires the FFIContext to already exist while we
    // compile on the DFG compiler thread. It always does: a JSFFIFunction can only feed a
    // CallFFI node after having been called (the ByteCodeParser feed reads the callee from a
    // CallLinkStatus, spec 10.2), and every non-DFG entry into the function (the C++ host path
    // and the IC stub's slow path, spec 8.2/8.3) begins with `globalObject->ffiContext()`,
    // creating the context on the mutator thread first. See also CROSS-ROW-REQUESTS.md,
    // which asks JSFFIFunction::create to touch the context eagerly for both codegen tiers.
    FFI::FFIContext* ffiContext = &globalObject->ffiContext();

    CodePtr<JITThunkPtrTag> invokeThunk = signature.invokeThunk();
    if (!invokeThunk) [[unlikely]] {
        // FFI-SPEC-GAP: spec 7.2 lets invoke-thunk generation return null on executable-memory
        // exhaustion (LinkBuffer::didFailToAllocate with JITCompilationCanFail), but spec 10.4
        // is silent on what the DFG does then. Every other tier degrades gracefully (the C++ host
        // path throws an OutOfMemoryError, spec 8.2 step 3; the FTL fails its own allocation flag),
        // so codegen must not crash the process from the compiler thread. Emit a call that throws
        // an OutOfMemoryError at runtime for this node (host-path parity) followed by the
        // node's exception check; the boxed result below is never reached. CROSS-ROW-REQUESTS.md
        // additionally asks A8 not to convert to CallFFI when the thunk cannot be generated.
        flushRegisters();
        callOperationWithoutExceptionCheck(operationThrowOutOfMemoryError, TrustedImmPtr(&vm()));
        exceptionCheck();
        GPRTemporary result(this);
        move(TrustedImm64(JSValue::encode(jsUndefined())), result.gpr());
        jsValueResult(result.gpr(), node);
        return;
    }

    const unsigned nativeArgumentCount = signature.argumentCount();
    const unsigned returnSlotIndex = nativeArgumentCount;

    // Strength reduction (spec 10.2) established exact arity and reserved the outgoing frame
    // space for the slot buffer via the m_parameterSlots bump; sanity-check both here.
    DFG_ASSERT(m_graph, node, node->numChildren() == 2 + signature.jsArgumentCount(), node->numChildren(), signature.jsArgumentCount());
    DFG_ASSERT(m_graph, node, m_graph.m_parameterSlots * sizeof(Register) >= signature.slotBufferBytes(), m_graph.m_parameterSlots, signature.slotCount());

    // The callee's global object is the realm every tier uses for conversions and errors
    // (the C++ host path receives the callee's global object, spec 8.2). Freeze it so the
    // graph tracks the cell we bake into operation calls.
    FrozenValue* frozenGlobalObject = m_graph.freeze(globalObject);

    auto slotAddressFor = [&](unsigned slotIndex) -> Address {
        return Address(stackPointerRegister, static_cast<int32_t>(slotIndex * FFI::slotSize));
    };

    // Determine whether any UntypedUse conversion may allocate from the call-scoped string
    // arena (spec section 5); if so the whole call is bracketed with arena enter/exit.
    bool needsArenaBracket = false;
    {
        unsigned jsArgumentIndex = 0;
        for (unsigned i = 0; i < nativeArgumentCount; ++i) {
            FFI::Type type = signature.argumentType(i);
            if (FFI::isSyntheticArgument(type))
                continue;
            Edge edge = m_graph.varArgChild(node, 2 + jsArgumentIndex++);
            if (edge.useKind() == UntypedUse && ffiUntypedConversionMayUseStringArena(type))
                needsArenaBracket = true;
        }
    }

    if (needsArenaBracket) {
        // Every live value must be on the stack before an operation call. The arena-enter
        // operation cannot throw (it only bumps the arena depth), so it is emitted as a plain
        // call with no exception check -- exactly like the arena-exit call after the native
        // call. (callOperationWithoutExceptionCheck cannot be used here: it requires a NOEXCEPT
        // operation, and operationFFIArenaEnter is declared with the ordinary
        // ExceptionOperationResult<void> convention, spec section 5.)
        flushRegisters();
        setupArguments<decltype(operationFFIArenaEnter)>(TrustedImmPtr(frozenGlobalObject));
        appendCall(operationFFIArenaEnter);
    }

    // The VM has a pending exception at this point in the emitted code (the caller checked).
    // Leave the string arena before the exception propagates so the arena depth returns to
    // zero (spec section 5). The exception itself stays pending on the VM; nothing here
    // clears it, so the caller's subsequent exception check still fires.
    auto emitArenaExitIfExceptionPending = [this, frozenGlobalObject] {
        Jump noException = emitExceptionCheck(vm(), AssemblyHelpers::InvertedExceptionCheck);
        setupArguments<decltype(operationFFIArenaExit)>(TrustedImmPtr(frozenGlobalObject));
        appendCall(operationFFIArenaExit);
        // The consumer of the write-slot exception (tryHandleOrGetExceptionUnderSilentSpill /
        // exceptionCheck) reads it from operationFFIWriteSlot's exception register --
        // returnValueGPR for an ExceptionOperationResult<void> operation -- and the arena-exit
        // call above clobbers that register (argumentGPR0 == returnValueGPR on arm64, and any
        // call overwrites rax/x0). Re-establish the register convention from the VM here instead
        // of relying on operationFFIArenaExit's own return convention.
        loadPtr(vm().addressOfException(), GPRInfo::returnValueGPR);
        noException.link(this);
    };

    // Stores a JS int32 (register low 32 bits) into a slot in the canonical encoding of
    // the given integer-class FFI type (spec section 4): signed types sign-extend to 64
    // bits, unsigned types zero-extend, bool becomes exactly 0 or 1 (toBoolean semantics,
    // never and32(1)).
    auto storeInt32AsIntegerType = [&](FFI::Type type, GPRReg valueGPR, GPRReg scratchGPR, Address slot) {
        switch (type) {
        case FFI::Type::Char:
        case FFI::Type::Int8:
            signExtend8To32(valueGPR, scratchGPR);
            signExtend32ToPtr(scratchGPR, scratchGPR);
            break;
        case FFI::Type::Uint8:
            zeroExtend8To32(valueGPR, scratchGPR);
            zeroExtend32ToWord(scratchGPR, scratchGPR);
            break;
        case FFI::Type::Int16:
            signExtend16To32(valueGPR, scratchGPR);
            signExtend32ToPtr(scratchGPR, scratchGPR);
            break;
        case FFI::Type::Uint16:
            zeroExtend16To32(valueGPR, scratchGPR);
            zeroExtend32ToWord(scratchGPR, scratchGPR);
            break;
        case FFI::Type::Int32:
            signExtend32ToPtr(valueGPR, scratchGPR);
            break;
        case FFI::Type::Uint32:
            zeroExtend32ToWord(valueGPR, scratchGPR);
            break;
        case FFI::Type::Bool:
            compare32(NotEqual, valueGPR, TrustedImm32(0), scratchGPR);
            zeroExtend32ToWord(scratchGPR, scratchGPR);
            break;
        default:
            DFG_CRASH(m_graph, node, "CallFFI: not an integer-class FFI type");
            return;
        }
        store64(scratchGPR, slot);
    };

    // Argument marshaling into the canonical slot buffer, which is the SP-relative outgoing
    // area [sp, sp + slotBufferBytes) reserved by the strength-reduction conversion.
    //
    // FFI-SPEC-GAP: the spec (10.4 steps 2-4) evaluates all argument children into typed
    // operands first, flushes, then stores. With up to Signature::maxArguments (32) native
    // parameters that would hold more locked registers than any target has, so each argument
    // is instead evaluated and stored into its slot one at a time, releasing its operand
    // registers before the next (the outgoing area is untouched by spilling and by the calls
    // emitted below, so early stores are safe). This mirrors how emitCall() streams var-args
    // into the callee frame. No argument is `use()`d early: the operands' registers merely
    // unlock, and the children stay referenced until the result is set (spec 10.4 step 8), so
    // cell arguments (typed-array views whose vector pointers we store) remain live in the
    // frame across the native call after the flushRegisters() below.
    unsigned jsArgumentIndex = 0;
    for (unsigned i = 0; i < nativeArgumentCount; ++i) {
        FFI::Type type = signature.argumentType(i);
        Address slotAddress = slotAddressFor(i);

        if (FFI::isSyntheticArgument(type)) {
            // Synthetic napi_env: read live from the FFIContext at call time, never baked as
            // an immediate (spec section 6).
            ASSERT(type == FFI::Type::NapiEnv);
            GPRTemporary scratch(this);
            loadPtr(ffiContext->addressOfNapiEnv(), scratch.gpr());
            store64(scratch.gpr(), slotAddress);
            continue;
        }

        Edge& edge = m_graph.varArgChild(node, 2 + jsArgumentIndex);
        ++jsArgumentIndex;

        switch (edge.useKind()) {
        case KnownInt32Use: {
            // Conversion (spec 10.2) already inserted the Int32 check; no speculation here.
            SpeculateInt32Operand value(this, edge);
            GPRTemporary scratch(this);
            storeInt32AsIntegerType(type, value.gpr(), scratch.gpr(), slotAddress);
            break;
        }

        case KnownBooleanUse: {
            DFG_ASSERT(m_graph, node, type == FFI::Type::Bool, static_cast<unsigned>(type));
            SpeculateBooleanOperand value(this, edge);
            GPRTemporary scratch(this);
            GPRReg scratchGPR = scratch.gpr();
            // The operand holds the boxed boolean (ValueFalse = 0x06 / ValueTrue = 0x07); its
            // low bit is the 0/1 payload.
            move(value.gpr(), scratchGPR);
            and64(TrustedImm32(1), scratchGPR);
            store64(scratchGPR, slotAddress);
            break;
        }

        case DoubleRepUse: {
            SpeculateDoubleOperand value(this, edge);
            FPRReg valueFPR = value.fpr();
            switch (type) {
            case FFI::Type::Double:
                storeDouble(valueFPR, slotAddress);
                break;
            case FFI::Type::Float: {
                FPRTemporary floatScratch(this);
                FPRReg floatFPR = floatScratch.fpr();
                convertDoubleToFloat(valueFPR, floatFPR);
                store32(TrustedImm32(0), slotAddress.withOffset(4));
                storeFloat(floatFPR, slotAddress);
                break;
            }
            default:
                DFG_CRASH(m_graph, node, "CallFFI: DoubleRepUse edge for a non-floating-point FFI type");
                break;
            }
            break;
        }

        case UntypedUse: {
            JSValueOperand value(this, edge);
            GPRTemporary scratch(this);
            GPRTemporary slotAddressTemp(this);
            FPRTemporary fpScratchTemp(this);
            GPRReg valueGPR = value.gpr();
            GPRReg scratchGPR = scratch.gpr();
            GPRReg slotAddressGPR = slotAddressTemp.gpr();
            FPRReg scratchFPR = fpScratchTemp.fpr();

            // Materialize the slot address for the out-of-line slow path before any branch, so
            // the register allocation state is identical along the fast and slow paths.
            addPtr(TrustedImm32(static_cast<int32_t>(i * FFI::slotSize)), stackPointerRegister, slotAddressGPR);

            // Fast conversions inline the same branchy shape as the IC stub (spec 8.3 step 4);
            // any miss lands in operationFFIWriteSlot, which owns the full spec section 5
            // semantics (and any TypeError). No speculation check / OSR exit is ever emitted:
            // MayExit for CallFFI is ExitsForExceptions only (spec 10.3).
            JumpList slowCases;
            JumpList stored;

            auto storeTypedArrayViewVector = [&] {
                slowCases.append(branchIfNotType(valueGPR, JSTypeRange { static_cast<JSType>(FirstTypedArrayType), static_cast<JSType>(LastTypedArrayType) }));
                // SHARED / RESIZABLE views are marked in the mode byte; their semantics (and any
                // future policy for them) stay in C++, so they take the slow path -- exactly like
                // the FTL twin, keeping the tiers behaviorally identical (SPEC section 5).
                slowCases.append(branchTest8(NonZero, Address(valueGPR, JSArrayBufferView::offsetOfMode()), TrustedImm32(isResizableOrGrowableSharedMode)));
                loadPtr(Address(valueGPR, JSArrayBufferView::offsetOfVector()), scratchGPR);
                // A null vector (DETACHED view, or a wasteful view with no storage) keeps its
                // semantics in C++ (slow path).
                slowCases.append(branchTestPtr(Zero, scratchGPR));
                cageTypedArrayStorage(valueGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
            };

            switch (type) {
            case FFI::Type::Char:
            case FFI::Type::Int8:
            case FFI::Type::Uint8:
            case FFI::Type::Int16:
            case FFI::Type::Uint16:
            case FFI::Type::Int32:
            case FFI::Type::Uint32:
                slowCases.append(branchIfNotInt32(valueGPR));
                storeInt32AsIntegerType(type, valueGPR, scratchGPR, slotAddress);
                break;

            case FFI::Type::Bool: {
                Jump notInt32 = branchIfNotInt32(valueGPR);
                // Any non-zero int32 is true (toBoolean); never and32(1) here.
                compare32(NotEqual, valueGPR, TrustedImm32(0), scratchGPR);
                zeroExtend32ToWord(scratchGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                stored.append(jump());

                notInt32.link(this);
                slowCases.append(branchIfNotBoolean(valueGPR, scratchGPR));
                move(valueGPR, scratchGPR);
                and64(TrustedImm32(1), scratchGPR);
                store64(scratchGPR, slotAddress);
                break;
            }

            case FFI::Type::Int64:
            case FFI::Type::Uint64:
            case FFI::Type::Int64Fast:
            case FFI::Type::Uint64Fast: {
                Jump notInt32 = branchIfNotInt32(valueGPR);
                signExtend32ToPtr(valueGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                stored.append(jump());

                notInt32.link(this);
                // Non-int32 numbers (doubles, BigInt32) take the C++ path in v1, which applies
                // FFI::doubleToInt64 / the modular BigInt rules (spec section 5).
                slowCases.append(branchIfNotCell(valueGPR));
                slowCases.append(branchIfNotHeapBigInt(valueGPR));
                // Digit 0 with sign == mod-2^64 truncation, identical to JSBigInt::toBigInt64
                // (AssemblyHelpers::toBigInt64, the JSToWasm precedent).
                toBigInt64(valueGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                break;
            }

            case FFI::Type::Double:
            case FFI::Type::Float: {
                slowCases.append(branchIfNotNumber(valueGPR));
                Jump notInt32 = branchIfNotInt32(valueGPR);
                convertInt32ToDouble(valueGPR, scratchFPR);
                Jump converted = jump();
                notInt32.link(this);
                unboxDouble(valueGPR, scratchGPR, scratchFPR);
                converted.link(this);
                if (type == FFI::Type::Double)
                    storeDouble(scratchFPR, slotAddress);
                else {
                    convertDoubleToFloat(scratchFPR, scratchFPR);
                    store32(TrustedImm32(0), slotAddress.withOffset(4));
                    storeFloat(scratchFPR, slotAddress);
                }
                break;
            }

            case FFI::Type::Pointer:
            case FFI::Type::CString:
            case FFI::Type::Function: {
                Jump notInt32 = branchIfNotInt32(valueGPR);
                // int32 pointers are sign-extended (spec section 5, Bun parity).
                signExtend32ToPtr(valueGPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                stored.append(jump());

                notInt32.link(this);
                Jump notNumber = branchIfNotNumber(valueGPR);
                unboxDouble(valueGPR, scratchGPR, scratchFPR);
                // MacroAssembler::truncateDoubleToInt64 is FFI::doubleToInt64 by definition
                // (spec section 5).
                truncateDoubleToInt64(scratchFPR, scratchGPR);
                store64(scratchGPR, slotAddress);
                stored.append(jump());

                notNumber.link(this);
                // Cells: only TypedArray / DataView views convert inline. ArrayBuffers, JS
                // strings (cstring transcoding), JSFFICallbacks, null/undefined and everything
                // else keep their exact semantics in C++ via the slow path.
                slowCases.append(branchIfNotCell(valueGPR));
                storeTypedArrayViewVector();
                break;
            }

            case FFI::Type::Buffer:
                // buffer requires a view; every non-view (including numbers) throws in C++.
                slowCases.append(branchIfNotCell(valueGPR));
                storeTypedArrayViewVector();
                break;

            case FFI::Type::NapiValue:
                // Raw EncodedJSValue pass-through; no conversion, no slow path.
                store64(valueGPR, slotAddress);
                break;

            case FFI::Type::Void:
            case FFI::Type::NapiEnv:
                DFG_CRASH(m_graph, node, "CallFFI: unexpected JS argument type");
                break;
            }

            stored.link(this);

            if (!slowCases.empty()) {
                Label doneLabel = label();
                Vector<SilentRegisterSavePlan> savePlans;
                silentSpillAllRegistersImpl(false, savePlans, NoResult);
                uint32_t typeTag = static_cast<uint32_t>(type);
                bool exitArenaOnException = needsArenaBracket;
                addSlowPathGeneratorLambda([=, this, savePlans = WTF::move(savePlans), slowCases = WTF::move(slowCases)]() mutable {
                    slowCases.link(this);
                    silentSpill(savePlans);
                    setupArguments<decltype(operationFFIWriteSlot)>(TrustedImmPtr(frozenGlobalObject), TrustedImmPtr(ffiContext), TrustedImm32(static_cast<int32_t>(typeTag)), JSValueRegs(valueGPR), slotAddressGPR);
                    appendCall(operationFFIWriteSlot);
                    // Exception path: leave the arena before the exception propagates
                    // (spec section 5). We are still under the silent spill, so the extra call
                    // may clobber any register; silentFill() below restores the live set.
                    if (exitArenaOnException)
                        emitArenaExitIfExceptionPending();
                    std::optional<GPRReg> exceptionReg = tryHandleOrGetExceptionUnderSilentSpill<decltype(operationFFIWriteSlot)>(savePlans, NoResult);
                    silentFill(savePlans);
                    if (exceptionReg)
                        exceptionCheck(*exceptionReg);
                    jump().linkTo(doneLabel, this);
                });
            }
            break;
        }

        default:
            DFG_CRASH(m_graph, node, "CallFFI: unexpected use kind on an argument edge");
            break;
        }
    }

    // All arguments are in the slot buffer. Spill everything so callbacks re-entering the
    // VM, exception unwinding and the conservative scan see a coherent frame, and so that
    // no live value sits in a caller-saved register across the native call.
    flushRegisters();

    // Call the invoke thunk directly. NOT via appendCall()/callOperation(): their
    // prepareForExternalCall() deliberately trashes vm.topCallFrame in debug builds
    // expecting the callee operation to re-store it, and the invoke thunk never does. Our
    // explicit store below is therefore the LAST instruction touching vm.topCallFrame
    // before the call (spec 10.4 step 5).
    emitStoreCodeOrigin(node->origin.semantic);
    storePtr(GPRInfo::callFrameRegister, &vm().topCallFrame);
    move(TrustedImmPtr(target), GPRInfo::argumentGPR0);
    move(stackPointerRegister, GPRInfo::argumentGPR1);
    move(TrustedImmPtr(invokeThunk.taggedPtr()), GPRInfo::nonArgGPR0);
    call(GPRInfo::nonArgGPR0, OperationPtrTag);

    // FFI-SPEC-GAP: the spec (section 5 / 10.4 step 4) exits the arena "after the return value
    // has been boxed AND on the exception path". A boxed result held in a temporary register
    // could not survive the arena-exit operation call, and the return boxing never reads the
    // arena (it only owns argument copies, which are dead once the native call returns). So the
    // arena is left once, unconditionally, immediately after the native call: this covers both
    // the normal path and the exception path (a pending exception raised by a re-entering
    // callback survives the arena-exit call and is checked right after). The argument
    // conversion slow paths above exit the arena on their own exception edges.
    if (needsArenaBracket) {
        setupArguments<decltype(operationFFIArenaExit)>(TrustedImmPtr(frozenGlobalObject));
        appendCall(operationFFIArenaExit);
    }

    // Exceptions raised by a JS callback that ran inside the native call are pending on the
    // VM (spec 8.2 step 5).
    exceptionCheck();

    // Box the return slot per FFI::Type (spec section 5, native -> JS table). The node is
    // NodeResultJS.
    Address returnSlot = slotAddressFor(returnSlotIndex);
    switch (signature.returnType()) {
    case FFI::Type::Void: {
        GPRTemporary result(this);
        move(TrustedImm64(JSValue::encode(jsUndefined())), result.gpr());
        jsValueResult(result.gpr(), node);
        break;
    }

    case FFI::Type::Char:
    case FFI::Type::Int8:
    case FFI::Type::Uint8:
    case FFI::Type::Int16:
    case FFI::Type::Uint16:
    case FFI::Type::Int32: {
        // The slot is already sign/zero-extended (spec section 4); load32 leaves the register in
        // canonical DataFormatInt32 shape (upper 32 bits zero).
        GPRTemporary result(this);
        load32(returnSlot, result.gpr());
        strictInt32Result(result.gpr(), node);
        break;
    }

    case FFI::Type::Uint32: {
        // int32 if it fits, else double (the compileUInt32ToNumber shape, boxed since the node
        // produces a JSValue).
        GPRTemporary result(this);
        GPRTemporary value(this);
        FPRTemporary fpValue(this);
        GPRReg resultGPR = result.gpr();
        GPRReg valueGPR = value.gpr();
        FPRReg valueFPR = fpValue.fpr();
        load64(returnSlot, valueGPR);
        Jump doesNotFitInt32 = branch32(LessThan, valueGPR, TrustedImm32(0));
        boxInt32(valueGPR, JSValueRegs(resultGPR));
        Jump done = jump();
        doesNotFitInt32.link(this);
        convertUInt32ToDouble(valueGPR, valueFPR);
        boxDouble(valueFPR, resultGPR);
        done.link(this);
        jsValueResult(resultGPR, node);
        break;
    }

    case FFI::Type::Bool: {
        // The producer normalized the slot to exactly 0 or 1 (spec section 4), so blessing it
        // is a single or32.
        GPRTemporary result(this);
        GPRReg resultGPR = result.gpr();
        load32(returnSlot, resultGPR);
#if ASSERT_ENABLED
        Jump canonical = branch32(BelowOrEqual, resultGPR, TrustedImm32(1));
        breakpoint();
        canonical.link(this);
#endif
        or32(TrustedImm32(JSValue::ValueFalse), resultGPR);
        jsValueResult(resultGPR, node, DataFormatJSBoolean);
        break;
    }

    case FFI::Type::Double:
    case FFI::Type::Float: {
        // FFI-SPEC-GAP: the spec says doubleResult(), but CallFFI is NodeResultJS and a
        // NodeResultJS value in DataFormatDouble is unfillable (SpeculativeJIT::fillJSValue
        // DFG_CRASHes on DataFormatDouble), so the double is purified and boxed instead --
        // matching the FTL lowering (spec 10.5, "always setJSValue") and compileCallDOM.
        FPRTemporary fpValue(this);
        GPRTemporary result(this);
        FPRReg valueFPR = fpValue.fpr();
        GPRReg resultGPR = result.gpr();
        if (signature.returnType() == FFI::Type::Double)
            loadDouble(returnSlot, valueFPR);
        else {
            loadFloat(returnSlot, valueFPR);
            convertFloatToDouble(valueFPR, valueFPR);
        }
        // Every native -> JS floating-point value is purified exactly once at the load
        // (spec section 5), so a non-canonical NaN can never be NaN-boxed into a JSValue.
        purifyNaN(valueFPR, valueFPR);
        boxDouble(valueFPR, resultGPR);
        jsValueResult(resultGPR, node);
        break;
    }

    case FFI::Type::NapiValue: {
        // Raw EncodedJSValue bits, no conversion (spec section 5).
        GPRTemporary result(this);
        load64(returnSlot, result.gpr());
        jsValueResult(result.gpr(), node);
        break;
    }

    case FFI::Type::Int64:
    case FFI::Type::Uint64:
    case FFI::Type::Int64Fast:
    case FFI::Type::Uint64Fast:
    case FFI::Type::Pointer:
    case FFI::Type::CString:
    case FFI::Type::Function:
    case FFI::Type::Buffer: {
        // Exotic boxing (BigInt / MAX_INT52 fast paths / pointer null -> jsNull()) stays
        // out of line in v1 (spec 10.4 step 7): operationFFIBoxSlot implements the whole
        // native -> JS table for these types and may throw only on OOM.
        GPRTemporary slotValue(this);
        GPRTemporary result(this);
        GPRReg slotValueGPR = slotValue.gpr();
        GPRReg resultGPR = result.gpr();
        load64(returnSlot, slotValueGPR);
        callOperation(operationFFIBoxSlot, resultGPR, TrustedImmPtr(frozenGlobalObject), TrustedImm32(static_cast<int32_t>(static_cast<uint32_t>(signature.returnType()))), slotValueGPR);
        // operationFFIBoxSlot allocates (heap BigInts) and can throw OOM; the FTL and IC-stub twins
        // of this path check, so must the DFG.
        exceptionCheck();
        jsValueResult(resultGPR, node);
        break;
    }

    case FFI::Type::NapiEnv:
        DFG_CRASH(m_graph, node, "CallFFI: napi_env is never a return type");
        break;
    }

    // Proves to the test harness that the DFG tier actually compiled a CallFFI (spec 11.2).
    FFI::g_ffiCompileCounts.dfgCallFFI++;
}


} } // namespace JSC::DFG

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(DFG_JIT) && USE(JSVALUE64)

#endif // USE(BUN_JSC_ADDITIONS)
