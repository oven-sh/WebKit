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

#include "FFISignature.h"
#include "JSFunction.h"
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>

namespace JSC {

class JITCode;
class JSGlobalObject;

namespace FFI {

struct CallHooks {
    void* (*before)(JSGlobalObject*, CallFrame*);
    void (*after)(JSGlobalObject*, CallFrame*, void* token);
};

} // namespace FFI

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

    JS_EXPORT_PRIVATE static JSFFIFunction* create(VM&, JSGlobalObject*, Structure*, Ref<FFI::Signature>&&, void* target, const String& name, JSObject* owner = nullptr, const FFI::CallHooks* hooks = nullptr);

    DECLARE_VISIT_CHILDREN;

    FFI::Signature& signature() const { return m_signature.get(); }
    void* target() const { return m_target; }
    JSObject* owner() const { return m_owner.get(); }
    const FFI::CallHooks* hooks() const { return m_hooks; }
    bool isHostPathOnly() const { return !!m_hooks; }
    JITCode* icCode() const { return m_icCode.get(); }

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
