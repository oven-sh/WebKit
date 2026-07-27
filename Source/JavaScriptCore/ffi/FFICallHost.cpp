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
#include "FFICallHost.h"

#if USE(BUN_JSC_ADDITIONS)

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

static ALWAYS_INLINE EncodedJSValue ffiCall(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* function = uncheckedDowncast<JSFFIFunction>(callFrame->jsCallee());
    Signature& signature = function->signature();
    FFIContext& context = globalObject->ffiContext();
    StringArena::Scope arenaScope(context);

    unsigned argumentCount = signature.argumentCount();
    ASSERT(argumentCount <= Signature::maxArguments);
    uint64_t slots[Signature::maxArguments + 1];

    for (unsigned i = 0; i < argumentCount; ++i) {
        Type type = signature.argumentType(i);
        writeSlotFromJSValue(globalObject, context, type, callFrame->argument(i), slots[i], &context.stringArena());
        RETURN_IF_EXCEPTION(scope, { });
    }
    slots[argumentCount] = 0;

    CodePtr<JITThunkPtrTag> thunk = signature.invokeThunk();
    if (!thunk) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope, "bun:ffi failed to allocate executable memory for the invoke thunk"_s);
        return { };
    }

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
                if (pending)
                    hookScope.clearException();
                else {
                    pending = hookScope.exception();
                    hookScope.clearException();
                }
            }
        }
        if (pending) [[unlikely]] {
            throwException(globalObject, scope, pending);
            return { };
        }
    } else
        thunk.taggedPtr<InvokeThunkFunction>()(function->target(), slots);
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
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    OPERATION_RETURN(scope, FFI::ffiCall(globalObject, callFrame));
}

} // namespace JSC

#endif // USE(JSVALUE64)

#endif // USE(BUN_JSC_ADDITIONS)
