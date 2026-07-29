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
#include "JSObject.h"
#include "MacroAssemblerCodeRef.h"
#include <atomic>

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

    JS_EXPORT_PRIVATE void close();
    bool isClosed() const { return m_closed; }

    JSObject* callable() const { return m_callable.get(); }
    const char* setReturnCString(const CString&);
    FFI::Signature& signature() const { return m_signature.get(); }
    bool isThreadsafe() const { return m_threadsafe; }
    void* embedderContext() const { return m_embedderContext; }
    static constexpr unsigned closedBit = 0x80000000u;
    static constexpr unsigned countMask = 0x7fffffffu;
    bool tryBeginThreadsafeInvocation()
    {
        unsigned state = m_threadsafeState.load(std::memory_order_acquire);
        do {
            if (state & closedBit)
                return false;
        } while (!m_threadsafeState.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire));
        return true;
    }
    bool endThreadsafeInvocation()
    {
        unsigned state = m_threadsafeState.fetch_sub(1, std::memory_order_acq_rel) - 1;
        return (state & closedBit) && !(state & countMask);
    }
    bool markClosedAndReportPending() { return m_threadsafeState.fetch_or(closedBit, std::memory_order_acq_rel) & countMask; }
    void unroot();

private:
    JSFFICallback(VM&, Structure*, Ref<FFI::Signature>&&);
    ~JSFFICallback();

    void finishCreation(VM&, JSObject* callable);

    WriteBarrier<JSObject> m_callable;
    Ref<FFI::Signature> m_signature;
    Vector<char, 64> m_returnCString;
    MacroAssemblerCodeRef<JITThunkPtrTag> m_entryCode;
    void* m_embedderContext { nullptr }; // opaque, embedder-owned; passed to threadsafe dispatch
    std::atomic<unsigned> m_threadsafeState { 0 }; // closedBit | pending-invocation count (see above)
    bool m_threadsafe { false };
    bool m_closed { false };
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
