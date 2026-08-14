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

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "JITOperationValidation.h"
#include "JSCJSValue.h"
#include "JSCPtrTag.h"
#include "MacroAssemblerCodeRef.h"
#include "OperationResult.h"

#if ENABLE(JIT) && !ENABLE(JIT_CAGE) && (CPU(X86_64) || CPU(ARM64))
#define FFI_CALLBACK_THUNK_SUPPORTED 1
#else
#define FFI_CALLBACK_THUNK_SUPPORTED 0
#endif

namespace JSC {

class JSFFICallback;
class VM;

namespace FFI {

#if FFI_CALLBACK_THUNK_SUPPORTED
MacroAssemblerCodeRef<JITThunkPtrTag> generateCallbackThunk(VM&, JSFFICallback&);
#endif // FFI_CALLBACK_THUNK_SUPPORTED

} // namespace FFI

#if FFI_CALLBACK_THUNK_SUPPORTED
JSC_DECLARE_JIT_OPERATION(ffiCallbackDispatch, EncodedJSValue, (JSFFICallback*, uint64_t*));
JSC_DECLARE_JIT_OPERATION(ffiCallbackDispatchThreadsafe, EncodedJSValue, (FFI::ThreadsafeCallbackHandle*, uint64_t*));
#endif // FFI_CALLBACK_THUNK_SUPPORTED

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
