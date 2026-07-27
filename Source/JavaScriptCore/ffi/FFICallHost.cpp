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
#include "FFICallHost.h"

#if USE(BUN_JSC_ADDITIONS)

// See FFICallHost.h: the host path is compiled out on 32-bit exactly like
// the FFIConversions API it consumes (SPEC section 14); nothing can reach it
// there because JSFFIFunction::create always throws on such builds.
#if USE(JSVALUE64)

#include "CallFrame.h"
#include "ExceptionHelpers.h"
#include "FFIContext.h"
#include "FFIConversions.h"
#include "FFISignature.h"
#include "FFIType.h"
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "JSFFIFunction.h"
#include "JSGlobalObject.h"
#include "TopExceptionScope.h"

namespace JSC {

namespace FFI {

// The single implementation shared by the C++ host path (ffiHostCall) and
// the IC stub's whole-call slow path (operationFFICallSlowPath). callFrame is
// the JS call frame whose callee is the JSFFIFunction being invoked.
static ALWAYS_INLINE EncodedJSValue ffiCall(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* function = uncheckedDowncast<JSFFIFunction>(callFrame->jsCallee());
    Signature& signature = function->signature();
    FFIContext& context = globalObject->ffiContext();
    // FFI-SPEC-GAP: the spec (section 8.2) writes `StringArena::Scope arenaScope(ctx)` and passes
    // `&arena` to writeSlotFromJSValue without naming the accessor; we assume A3 exposes the
    // call-scoped arena as FFIContext::stringArena() (returning FFI::StringArena&) with the RAII
    // bracket type FFI::StringArena::Scope constructible from FFIContext&.
    StringArena::Scope arenaScope(context);

    unsigned argumentCount = signature.argumentCount();
    ASSERT(argumentCount <= Signature::maxArguments);
    uint64_t slots[Signature::maxArguments + 1];

    for (unsigned i = 0; i < argumentCount; ++i) {
        Type type = signature.argumentType(i);
        // Missing JS arguments are jsUndefined() and take each type's undefined rule (Bun parity).
        writeSlotFromJSValue(globalObject, context, type, callFrame->argument(i), slots[i], &context.stringArena());
        RETURN_IF_EXCEPTION(scope, { });
    }
    slots[argumentCount] = 0;

    CodePtr<JITThunkPtrTag> thunk = signature.invokeThunk();
    if (!thunk) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope, "bun:ffi failed to allocate executable memory for the invoke thunk"_s);
        return { };
    }

    // Embedder call hooks (hooked functions are host-path-only, so this is the single place they
    // run). `before` immediately precedes the native call; `after` immediately follows it and runs
    // UNCONDITIONALLY -- even when the native call left a JS exception pending (a callback threw)
    // -- so a scope opened in before (e.g. an N-API handle scope) is always closed.
    //
    // Exception discipline: a callback that throws during the native call raises its exception
    // INSIDE `hookScope` below, which is therefore the owning scope entitled to clear it. That is
    // the CallData.cpp stash-and-rethrow idiom: the exception is stashed and cleared through the
    // inner TopExceptionScope (so the after-hook -- ordinary embedder code, not exception-handling
    // code -- observes a clean VM), the inner scope closes, and only then is the exception
    // re-thrown against this function's own throw scope, propagating exactly as it would have.
    // A before-hook that throws aborts the native call (its exception flows the same way).
    if (const CallHooks* hooks = function->hooks()) [[unlikely]] {
        Exception* pending = nullptr;
        {
            auto hookScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
            void* hookToken = hooks->before ? hooks->before(globalObject, callFrame) : nullptr;
            if (!hookScope.exception()) [[likely]]
                thunk.taggedPtr<InvokeThunkFunction>()(function->target(), slots);
            pending = hookScope.exception(); // from a throwing before-hook or an in-call callback
            if (pending) [[unlikely]]
                hookScope.clearException();
            if (hooks->after)
                hooks->after(globalObject, callFrame, hookToken); // runs on a clean VM
            if (hookScope.exception()) [[unlikely]] {
                // The after-hook itself threw. If we were already carrying an exception it wins
                // (the after-hook's is dropped); otherwise the after-hook's exception propagates.
                if (pending)
                    hookScope.clearException();
                else {
                    pending = hookScope.exception();
                    hookScope.clearException();
                }
            }
        }
        if (pending) [[unlikely]] {
            // Re-throw the ORIGINAL Exception* cell (not pending->value()): the JSValue overload
            // would mint a NEW Exception, discarding identity/stack, notifying the debugger twice,
            // and -- for a TerminationException -- re-installing it as an ordinary catchable cell.
            // Re-throwing the same cell keeps caught===thrown and keeps m_terminationException the
            // pending exception, so termination stays uncatchable.
            throwException(globalObject, scope, pending);
            return { };
        }
    } else
        thunk.taggedPtr<InvokeThunkFunction>()(function->target(), slots);
    // A JS callback that ran inside the native call may have left an exception pending on the VM.
    RETURN_IF_EXCEPTION(scope, { });

    RELEASE_AND_RETURN(scope, JSValue::encode(jsValueFromSlot(globalObject, context, signature.returnType(), slots[argumentCount])));
}

JSC_DEFINE_HOST_FUNCTION(ffiHostCall, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return ffiCall(globalObject, callFrame);
}

} // namespace FFI

JSC_DEFINE_JIT_OPERATION(operationFFICallSlowPath, EncodedJSValue, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    // The IC stub stores vm.topCallFrame = callFrameRegister before calling any operation
    // (SPEC section 8.3 step 3) and passes that same frame here; the operation-prologue
    // tracer (rather than NativeCallFrameTracer) additionally ASSERTs, on
    // USE(BUILTIN_FRAME_ADDRESS) debug builds, that the stub really performed that store,
    // which callbacks re-entering the VM during the native call depend on.
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    OPERATION_RETURN(scope, FFI::ffiCall(globalObject, callFrame));
}

} // namespace JSC

#endif // USE(JSVALUE64)

#endif // USE(BUN_JSC_ADDITIONS)
