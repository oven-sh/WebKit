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

// FFI-SPEC-GAP: SPEC section 14 requires `#if USE(JSVALUE64)` around the FFI
// call machinery so 32-bit builds compile it out. The cell classes and the
// JSGlobalObject / heap hooks (SPEC section 13) stay compiled everywhere and
// JSFFIFunction::create throws "bun:ffi is not supported on this architecture"
// on 32-bit, so nothing on a 32-bit build can reach ffiHostCall or
// operationFFICallSlowPath; they are compiled out here exactly like the
// FFIConversions API they consume (writeSlotFromJSValue / jsValueFromSlot),
// which A3 also gates on USE(JSVALUE64).
#if USE(JSVALUE64)

#include "JSCJSValue.h"
#include "OperationResult.h"

namespace JSC {

class CallFrame;
class JSGlobalObject;

namespace FFI {

// The C++ host path for JSFFIFunction: marshals the JS arguments into the
// canonical slot buffer, invokes the per-signature invoke thunk, and boxes the
// native return value. This is the NativeFunction installed in every
// JSFFIFunction's NativeExecutable: JS call sites reach it when no IC entry
// stub was generated, and C++-initiated calls (JSC::call,
// Function.prototype.call/apply reached from C++) always run it directly.
JSC_DECLARE_HOST_FUNCTION(ffiHostCall);

} // namespace FFI

// Same body as FFI::ffiHostCall, but with the JIT operation calling convention;
// the per-function IC entry stub calls this on its whole-call slow path with
// its own CallFrame.
JSC_DECLARE_JIT_OPERATION(operationFFICallSlowPath, EncodedJSValue, (JSGlobalObject*, CallFrame*));

} // namespace JSC

#endif // USE(JSVALUE64)

#endif // USE(BUN_JSC_ADDITIONS)
