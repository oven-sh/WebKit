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

#include "FFISignature.h"
#include "JSFunction.h"
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>

namespace JSC {

class JITCode;
class JSGlobalObject;

// A JSFunction whose call runs a native (dlopen'd) function through the FFI
// machinery. Callable like any function: JS-initiated call sites (LLInt call
// slow path, call ICs, DFG/FTL direct calls) enter the NativeExecutable's call
// code -- the per-instance JIT'd IC entry stub when Options::useFFIICStub()
// produced one (this cell keeps it alive), otherwise the C++ host path
// (FFI::ffiHostCall) -- and C++-initiated calls (JSC::call,
// Function.prototype.call reached from C++) always run FFI::ffiHostCall
// directly. Both paths share the same body, so behavior is identical.
class JSFFIFunction final : public JSFunction {
public:
    using Base = JSFunction;

    static constexpr unsigned StructureFlags = Base::StructureFlags;

    static constexpr DestructionMode needsDestruction = NeedsDestruction; // Holds Ref<Signature> + RefPtr<JITCode>.
    static void destroy(JSCell*);
    ~JSFFIFunction(); // Out-of-line: RefPtr<JITCode> needs the complete type only in the .cpp.

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.ffiFunctionSpace<mode>();
    }

    DECLARE_EXPORT_INFO;

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    // Throws a TypeError on the global object's scope and returns nullptr when
    // bun:ffi cannot be used (JIT disabled / executable memory unavailable /
    // unsupported architecture); returns nullptr with an exception pending in
    // that case, never a partially-built object.
    JS_EXPORT_PRIVATE static JSFFIFunction* create(VM&, JSGlobalObject*, Structure*, Ref<FFI::Signature>&&, void* target, const String& name);

    FFI::Signature& signature() const { return m_signature.get(); }
    void* target() const { return m_target; }
    // The IC entry stub installed as this function's executable call code, or
    // null when the plain host-function path is in use.
    JITCode* icCode() const { return m_icCode.get(); }

    static constexpr ptrdiff_t offsetOfSignature() { return OBJECT_OFFSETOF(JSFFIFunction, m_signature); }
    static constexpr ptrdiff_t offsetOfTarget() { return OBJECT_OFFSETOF(JSFFIFunction, m_target); }

private:
    JSFFIFunction(VM&, NativeExecutable*, JSGlobalObject*, Structure*, Ref<FFI::Signature>&&, void* target, RefPtr<JITCode>&& icCode);

    Ref<FFI::Signature> m_signature;
    void* m_target;
    RefPtr<JITCode> m_icCode; // Keeps the IC entry stub alive; null when no stub.
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
