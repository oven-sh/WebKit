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

#include "FFIContext.h"
#include "FFISignature.h"
#include "JSObject.h"
#include "MacroAssemblerCodeRef.h"

namespace JSC {

class JSFFICallback final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    static constexpr unsigned StructureFlags = Base::StructureFlags;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.ffiCallbackSpace<mode>();
    }

    DECLARE_EXPORT_INFO;

    DECLARE_VISIT_CHILDREN;

    JS_EXPORT_PRIVATE static JSFFICallback* create(VM&, JSGlobalObject*, Structure*, JSObject* callable, Ref<FFI::Signature>&&, bool threadsafe = false, void* embedderContext = nullptr);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static JSObject* createPrototype(VM&, JSGlobalObject*);

    JS_EXPORT_PRIVATE void* nativeEntrypoint() const;
    const MacroAssemblerCodeRef<JITThunkPtrTag>& entryCode() const;

    JS_EXPORT_PRIVATE void close();
    bool isClosed() const { return m_closed; }

    JSObject* callable() const { return m_callable.get(); }
    const char* setReturnCString(const CString&);
    FFI::Signature& signature() const { return m_signature.get(); }
    bool isThreadsafe() const { return !!m_threadsafe; }
    FFI::ThreadsafeCallbackHandle* threadsafeHandle() const { return m_threadsafe.get(); }
    void unroot();

private:
    JSFFICallback(VM&, Structure*, Ref<FFI::Signature>&&);
    ~JSFFICallback();

    void finishCreation(VM&, JSObject* callable);

    WriteBarrier<JSObject> m_callable;
    Ref<FFI::Signature> m_signature;
    Vector<char, 64> m_returnCString;
    MacroAssemblerCodeRef<JITThunkPtrTag> m_entryCode; // a threadsafe callback's lives in its handle instead
    RefPtr<FFI::ThreadsafeCallbackHandle> m_threadsafe;
    bool m_closed { false };
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
