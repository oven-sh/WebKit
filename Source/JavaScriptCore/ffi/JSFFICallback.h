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
#include "JSObject.h"
#include <atomic>
#include "MacroAssemblerCodeRef.h"

namespace JSC {

// A JSFFICallback wraps a JS callable object in a native-ABI entry point
// (SPEC section 9). Native code calls `nativeEntrypoint()` like any C function
// pointer of the callback's signature; the entry thunk marshals the native
// arguments into the canonical slot buffer and re-enters JS through
// ffiCallbackDispatch. An un-close()d callback is ROOTED by the engine (the
// live-callback set on FFIContext), so the cell -- and therefore its entry code --
// stays alive as long as native code might call it, even with no JS reference;
// close() unroots it (deferred until any queued threadsafe invocations drain).
class JSFFICallback final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    static constexpr unsigned StructureFlags = Base::StructureFlags;

    // Holds a Ref<FFI::Signature> and the MacroAssemblerCodeRef of the entry
    // thunk, so the cell must run its destructor.
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.ffiCallbackSpace<mode>();
    }

    DECLARE_EXPORT_INFO;

    DECLARE_VISIT_CHILDREN;

    // Creates the cell and generates its native-ABI entry thunk. On failure
    // (`--useJIT=false` / executable allocation disabled, an unsupported
    // architecture, or executable memory exhaustion) an exception is thrown on
    // the global object's throw scope and nullptr is returned, so every
    // creator (BunFFI.cpp, $vm.ffiCallback, testFFI) gets identical error
    // behavior. `callable` must be callable; the JS surface (SPEC section
    // 9.1) is installed by finishCreation.
    // `threadsafe`: the entry may be called from a FOREIGN thread; the invocation is then not
    // run inline but copied into a ThreadsafeInvocation and handed to the process-wide dispatch
    // function (FFIContext::setThreadsafeDispatch), which the embedder queues to its JS thread. The
    // C caller receives a zero return immediately (a threadsafe return value is undefined by
    // nature). `embedderContext` is an opaque pointer captured now, on the JS thread, and passed
    // through to that dispatch function (never dereferenced by the engine).
    JS_EXPORT_PRIVATE static JSFFICallback* create(VM&, JSGlobalObject*, Structure*, JSObject* callable, Ref<FFI::Signature>&&, bool threadsafe = false, void* embedderContext = nullptr);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    // Prototype object carrying the `close` method (deliberately not an own property of the cell).
    static JSObject* createPrototype(VM&, JSGlobalObject*);

    // The code pointer handed to C. It stays valid for the cell's lifetime;
    // close() does NOT clear it.
    JS_EXPORT_PRIVATE void* nativeEntrypoint() const;

    // The single close() rule (SPEC section 9.1): idempotent, drops nothing
    // native-side (the entry code lives as long as the cell), the JS-visible
    // "ptr" own property is set to null, and m_closed makes $vm / Bun glue
    // reject the object as an FFI argument.
    JS_EXPORT_PRIVATE void close();
    bool isClosed() const { return m_closed; }

    JSObject* callable() const { return m_callable.get(); }
    FFI::Signature& signature() const { return m_signature.get(); }
    // Read ON THE FOREIGN THREAD by ffiCallbackDispatch: plain immutable-after-creation fields,
    // safe to read without the JS lock (no barriers needed for a plain bool / raw pointer).
    bool isThreadsafe() const { return m_threadsafe; }
    void* embedderContext() const { return m_embedderContext; }
    // Threadsafe lifetime + close() race, both handled by ONE atomic word:
    //   bit 31        = "closed" (set once by close(), on the JS thread)
    //   bits 0..30    = number of queued-but-not-yet-run ThreadsafeInvocations
    // The cell must stay ROOTED (in the live-callback set) until every queued invocation has
    // drained, even if close() ran first -- a record holds only a raw pointer to this cell. And
    // the increment must never race close(): a foreign thread calls tryBeginThreadsafeInvocation()
    // which increments ONLY if the closed bit is clear (a single CAS on this word), so either the
    // increment happens-before close() observes the count (count>0 -> close defers the unroot to
    // the last invocation) or close() wins and the foreign thread sees "closed" and does NOT
    // dispatch (no record referencing an unrooted cell is ever created). Only this word is
    // touched off-thread.
    static constexpr unsigned closedBit = 0x80000000u;
    static constexpr unsigned countMask = 0x7fffffffu;
    // FOREIGN thread. Returns false (do not dispatch) if already closed.
    bool tryBeginThreadsafeInvocation()
    {
        unsigned state = m_threadsafeState.load(std::memory_order_acquire);
        do {
            if (state & closedBit)
                return false;
        } while (!m_threadsafeState.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire));
        return true;
    }
    // JS thread. Retires one invocation; returns true if the caller should now unroot the cell
    // (it was closed and this was the last pending invocation).
    bool endThreadsafeInvocation()
    {
        unsigned state = m_threadsafeState.fetch_sub(1, std::memory_order_acq_rel) - 1;
        return (state & closedBit) && !(state & countMask);
    }
    // JS thread, from close(): atomically set the closed bit and report whether any invocation
    // was in flight at that instant (if none, close() unroots immediately; otherwise the last
    // endThreadsafeInvocation() does).
    bool markClosedAndReportPending() { return m_threadsafeState.fetch_or(closedBit, std::memory_order_acq_rel) & countMask; }
    void unroot();

private:
    JSFFICallback(VM&, Structure*, Ref<FFI::Signature>&&);
    ~JSFFICallback();

    void finishCreation(VM&, JSObject* callable);

    WriteBarrier<JSObject> m_callable;
    Ref<FFI::Signature> m_signature;
    // Native-ABI entry thunk with this cell baked as an immediate; generated by
    // FFI::generateCallbackThunk() in finishCreation. Owned by the cell.
    MacroAssemblerCodeRef<JITThunkPtrTag> m_entryCode;
    void* m_embedderContext { nullptr }; // opaque, embedder-owned; passed to threadsafe dispatch
    std::atomic<unsigned> m_threadsafeState { 0 }; // closedBit | pending-invocation count (see above)
    bool m_threadsafe { false };
    bool m_closed { false };
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
