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

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

#include "JSCPtrTag.h"
#include "JSExportMacros.h"
#include "MacroAssemblerCodeRef.h"

namespace JSC { namespace FFI {

class Signature;

#if ENABLE(JIT)

// Generates the per-signature invoke thunk: the ONLY code that emits
// native-callee-ABI marshaling (SysV64 / AAPCS64 incl. Apple sub-word
// stack packing / Win64 shadow space + positional pairing). The entry ABI is
// the JSC operation convention (SYSV_ABI on Windows x64) and the prototype is
// FFI::InvokeThunkFunction (FFISignature.h):
//
//     void JIT_OPERATION_ATTRIBUTES thunk(void* target, uint64_t* slots);
//
// where `slots` is the canonical slot buffer of FFISignature.h: argument i
// at slots[i], the return value at slots[argumentCount()], every slot in
// its normalized encoding. The thunk is signature-pure (target and slots are
// runtime parameters), never touches JS state and never allocates, so
// Signature::invokeThunk() caches the result process-wide.
//
// Returns a null code ref when executable memory allocation fails or on an
// unsupported architecture; callers turn that into an out-of-memory /
// "not supported" error.
//
// JS_EXPORT_PRIVATE: the testFFI executable (SPEC section 11.3) links the
// JavaScriptCore library (built with hidden default visibility) and calls
// this directly for its native-vs-thunk ABI differential; precedent
// FFICallingConvention.h's exported layout entry points.
JS_EXPORT_PRIVATE MacroAssemblerCodeRef<JITThunkPtrTag> generateInvokeThunk(const Signature&);

#endif // ENABLE(JIT)

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
