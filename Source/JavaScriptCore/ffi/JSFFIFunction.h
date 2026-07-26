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
namespace FFI {

// Embedder-supplied call bracketing for a JSFFIFunction. When present the function is bound
// PERMANENTLY to the C++ host path: it is never given an FFIICStub and the DFG never converts a
// call to it into a CallFFI node, so before/after run around every single call from exactly one
// place (FFI::ffiHostCall). `before` runs immediately before the native call and returns an
// opaque token; `after` receives that token and runs immediately after the native call returns,
// UNCONDITIONALLY -- including when the native call left a JS exception pending (e.g. a callback
// threw) -- so a scope opened in before is always closed. The engine attaches no meaning to the
// token or to the JSFFIFunction's owner object; both are the embedder's to interpret (e.g. Bun
// opens/closes an N-API handle scope and finds its environment via callee->owner()). The struct
// must have static lifetime; the cell holds a raw pointer to it.
struct CallHooks {
    void* (*before)(JSGlobalObject*, CallFrame*);
    void (*after)(JSGlobalObject*, CallFrame*, void* token);
};

} // namespace FFI

class JSFFIFunction final : public JSFunction {
public:
    using Base = JSFunction;

    // "ptr" (the resolved native target as a double-encoded pointer, Bun parity) is an
    // INTRINSIC property served from m_target below -- never a putDirect on the instance. An
    // own-property addition would transition this cell's Structure, and a JSFFIFunction with a
    // non-canonical structure loses the callee fast paths on POLYMORPHIC (non-devirtualized)
    // call sites: measured ~2.5x slower per call. Keeping the structure canonical keeps every
    // call site fast, not just the ones the DFG turns into CallFFI.
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames;

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
    // `owner` (optional): a JS object this function keeps alive via a write barrier -- e.g. the
    // object owning the dlopen'd library handle. The GC therefore cannot finalize the owner while
    // ANY function referencing it is reachable, which is what makes GC-driven library close safe:
    // the owner's finalizer (dlclose) can only run once its last function is unreachable. Set once
    // at creation and never cleared. `hooks` (optional, static lifetime): see FFI::CallHooks --
    // presence binds the function permanently to the host path.
    JS_EXPORT_PRIVATE static JSFFIFunction* create(VM&, JSGlobalObject*, Structure*, Ref<FFI::Signature>&&, void* target, const String& name, JSObject* owner = nullptr, const FFI::CallHooks* hooks = nullptr);

    DECLARE_VISIT_CHILDREN;

    FFI::Signature& signature() const { return m_signature.get(); }
    void* target() const { return m_target; }
    JSObject* owner() const { return m_owner.get(); }
    const FFI::CallHooks* hooks() const { return m_hooks; }
    // A hooked function must run every call through the C++ host path (the only place the
    // hooks are invoked): no FFIICStub, no DFG/FTL CallFFI node.
    bool isHostPathOnly() const { return !!m_hooks; }
    // The IC entry stub installed as this function's executable call code, or
    // null when the plain host-function path is in use.
    JITCode* icCode() const { return m_icCode.get(); }

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    // Keep enumeration coherent with the intrinsic slots above: `ptr` / `native` are real own
    // properties of the cell, so Object.getOwnPropertyNames / `"ptr" in fn` must agree with reads.
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);

    static constexpr ptrdiff_t offsetOfSignature() { return OBJECT_OFFSETOF(JSFFIFunction, m_signature); }
    static constexpr ptrdiff_t offsetOfTarget() { return OBJECT_OFFSETOF(JSFFIFunction, m_target); }

private:
    JSFFIFunction(VM&, NativeExecutable*, JSGlobalObject*, Structure*, Ref<FFI::Signature>&&, void* target, RefPtr<JITCode>&& icCode, const FFI::CallHooks* hooks);

    Ref<FFI::Signature> m_signature;
    void* m_target;
    RefPtr<JITCode> m_icCode; // Keeps the IC entry stub alive; null when no stub.
    WriteBarrier<JSObject> m_owner; // Optional; keeps the owner (e.g. library handle object) alive.
    const FFI::CallHooks* m_hooks; // Optional, static lifetime; non-null => host path only.
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
