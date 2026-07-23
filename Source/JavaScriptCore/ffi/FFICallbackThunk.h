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

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "JITOperationValidation.h"
#include "JSCJSValue.h"
#include "JSCPtrTag.h"
#include "MacroAssemblerCodeRef.h"
#include "OperationResult.h"

// The callback entry thunk is JIT-emitted 64-bit code; JIT-less, 32-bit,
// JIT-operation-validation and non-{x86-64, arm64} builds compile the entry
// thunk AND ffiCallbackDispatch out, and JSFFICallback creation throws (SPEC
// section 14) -- the same predicate the invoke thunk, IC stub and
// JSFFIFunction::create use, so both halves of bun:ffi agree on whether the
// feature exists on a given build.
#if ENABLE(JIT) && USE(JSVALUE64) && !ENABLE(JIT_CAGE) && (CPU(X86_64) || CPU(ARM64))
#define FFI_CALLBACK_THUNK_SUPPORTED 1
#else
#define FFI_CALLBACK_THUNK_SUPPORTED 0
#endif

namespace JSC {

class JSFFICallback;
class VM;

namespace FFI {

#if FFI_CALLBACK_THUNK_SUPPORTED
// Generates the native-ABI (hostNativeCC()) entry thunk for `callback`
// (SPEC section 9.2). The JSFFICallback* is baked as an immediate; the thunk
// spills the incoming native arguments into the canonical slot buffer, calls
// ffiCallbackDispatch through the JSC operation calling convention, and
// returns the slot-encoded result per the signature's return type. Returns
// a null MacroAssemblerCodeRef on executable-memory allocation failure.
MacroAssemblerCodeRef<JITThunkPtrTag> generateCallbackThunk(VM&, JSFFICallback&);
#endif // FFI_CALLBACK_THUNK_SUPPORTED

} // namespace FFI

#if FFI_CALLBACK_THUNK_SUPPORTED
// The C++ half of a callback invocation (SPEC section 9.3), entered from the
// native-ABI entry thunk -- i.e. ultimately from FOREIGN C code. It converts
// the slot buffer to JSValues, calls the JS callable, and writes the return
// slot. The EncodedJSValue result (the JS call's result) is only for
// debuggability; the thunk reads the return slot, not the return register.
// Declared through JSC_DECLARE_JIT_OPERATION solely to get the
// JIT_OPERATION_ATTRIBUTES (SysV on Windows x86-64) calling convention. It is
// compiled under the same predicate as the entry thunk (its only caller) --
// the FFIConversions / FFIContext machinery it uses is USE(JSVALUE64)-only.
JSC_DECLARE_JIT_OPERATION(ffiCallbackDispatch, EncodedJSValue, (JSFFICallback*, uint64_t*));
#endif // FFI_CALLBACK_THUNK_SUPPORTED

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
