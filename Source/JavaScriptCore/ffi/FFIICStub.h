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

#if USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)

#include <wtf/Forward.h>
#include <wtf/RefPtr.h>

namespace JSC {

class JITCode;
class JSGlobalObject;
class VM;

namespace FFI {

class Signature;

// Generates the per-JSFFIFunction IC entry stub (SPEC section 8.3): JS
// calling-convention code that fast-converts the JS arguments into the
// canonical slot buffer (SPEC section 4), calls the signature's invoke thunk
// with the baked `target`, and boxes the return slot. Any type miss branches
// to a shared slow path that performs the whole call in C++ via
// operationFFICallSlowPath (SPEC section 8.2).
//
// Only `target` and the signature are baked as immediates; the callee cell is
// read from CallFrameSlot::callee at call time, so no JSFFIFunction cell needs to
// exist when this stub is generated. JSFFIFunction::create() (SPEC section 8.1)
// calls this BEFORE
// allocating the cell and installs the result as the call code of a
// diversified NativeExecutable (its generatedJITCodeForCall()), with no
// Repatch.cpp special case. JS call sites -- the LLInt call slow path,
// baseline/DFG/FTL call ICs and DFG/FTL direct calls -- enter the stub through
// that executable code. C++-initiated calls (Interpreter::executeCall on
// CallData::Type::Native, which includes JSC::call and
// Function.prototype.call/apply reached from C++) invoke the executable's
// TaggedNativeFunction, FFI::ffiHostCall, directly and never enter the stub;
// that path is semantically identical, just unaccelerated.
//
// Returns nullptr when the JIT / executable allocator is unavailable, when
// the signature's invoke thunk cannot be generated, when the stub itself fails
// to allocate, or on configurations where FFI JIT code is compiled out
// (32-bit, JIT operation validation). The caller then falls back to the
// plain host-function executable (FFI::ffiHostCall), which is behaviorally
// identical apart from speed.
RefPtr<JITCode> generateICStubCode(VM&, JSGlobalObject*, Signature&, void* target);

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && ENABLE(JIT)
