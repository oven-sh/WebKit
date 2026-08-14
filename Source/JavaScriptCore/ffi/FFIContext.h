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

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

#include "FFISignature.h"
#include "HeapObserver.h"
#include "JSExportMacros.h"
#include "MacroAssemblerCodeRef.h"
#include "WriteBarrier.h"
#include <span>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringImpl.h>

namespace JSC {
class JSFFICallback;
class VM;
} // namespace JSC

namespace JSC { namespace FFI {

// What the entry thunk of a `threadsafe` JSFFICallback targets in place of the cell. Native code may
// call that thunk from any thread and at any time — including after the callback's global object and VM
// are gone (a terminated worker), which the embedder cannot always sequence against `close()`. So
// nothing the foreign thread touches lives in the GC cell: the thunk code, the signature, the
// closed/pending state and the embedder's routing token live here, and once an entrypoint has been handed
// out the handle (and so the thunk) is never freed: a call arriving after close() or after the cell and
// VM are gone counts itself out and returns zero rather than jumping into freed code. Only the owning
// thread reads `callback()`.
class ThreadsafeCallbackHandle final : public ThreadSafeRefCounted<ThreadsafeCallbackHandle> {
    WTF_MAKE_TZONE_ALLOCATED(ThreadsafeCallbackHandle);
public:
    static Ref<ThreadsafeCallbackHandle> create(JSFFICallback* callback, Ref<Signature>&& signature, void* embedderContext)
    {
        return adoptRef(*new ThreadsafeCallbackHandle(callback, WTF::move(signature), embedderContext));
    }

    Signature& signature() const { return m_signature.get(); }
    void* embedderContext() const { return m_embedderContext; }
    JSFFICallback* callback() const { return m_callback; } // owning thread; null once the cell is destroyed

    void setEntryCode(MacroAssemblerCodeRef<JITThunkPtrTag>&& code) { m_entryCode = WTF::move(code); }
    const MacroAssemblerCodeRef<JITThunkPtrTag>& entryCode() const { return m_entryCode; }

    static constexpr unsigned closedBit = 0x80000000u;
    static constexpr unsigned countMask = 0x7fffffffu;
    // Any thread: count an invocation in unless closed.
    bool tryBeginInvocation()
    {
        unsigned state = m_state.load(std::memory_order_acquire);
        do {
            if (state & closedBit)
                return false;
        } while (!m_state.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire));
        return true;
    }
    // Owning thread: whether this was the last pending invocation of a closed callback.
    bool endInvocation()
    {
        unsigned state = m_state.fetch_sub(1, std::memory_order_acq_rel) - 1;
        return (state & closedBit) && !(state & countMask);
    }
    // Owning thread: whether invocations are still pending.
    bool markClosedAndReportPending() { return m_state.fetch_or(closedBit, std::memory_order_acq_rel) & countMask; }
    void cellDestroyed()
    {
        m_callback = nullptr;
        m_state.fetch_or(closedBit, std::memory_order_acq_rel);
    }

private:
    ThreadsafeCallbackHandle(JSFFICallback* callback, Ref<Signature>&& signature, void* embedderContext)
        : m_signature(WTF::move(signature))
        , m_embedderContext(embedderContext)
        , m_callback(callback)
    {
    }

    const Ref<Signature> m_signature;
    void* const m_embedderContext;
    JSFFICallback* m_callback;
    MacroAssemblerCodeRef<JITThunkPtrTag> m_entryCode;
    std::atomic<unsigned> m_state { 0 }; // closedBit | pending-invocation count
};

class ThreadsafeInvocation final : public ThreadSafeRefCounted<ThreadsafeInvocation> {
    WTF_MAKE_TZONE_ALLOCATED(ThreadsafeInvocation);
public:
    static Ref<ThreadsafeInvocation> create(ThreadsafeCallbackHandle& handle, std::span<const uint64_t> slots)
    {
        return adoptRef(*new ThreadsafeInvocation(handle, slots));
    }

    ThreadsafeCallbackHandle& handle() const { return m_handle.get(); }
    void* embedderContext() const { return m_handle->embedderContext(); }
    std::span<uint64_t> slots() { return m_slots.mutableSpan(); }
    std::span<const uint64_t> slots() const { return m_slots.span(); }

private:
    ThreadsafeInvocation(ThreadsafeCallbackHandle& handle, std::span<const uint64_t> slots)
        : m_handle(handle)
        , m_slots(slots)
    {
    }

    const Ref<ThreadsafeCallbackHandle> m_handle;
    Vector<uint64_t, 8> m_slots;
};

class FFIContext;

class StringArena {
    WTF_MAKE_NONCOPYABLE(StringArena);
public:
    class Scope {
        WTF_MAKE_NONCOPYABLE(Scope);
        WTF_FORBID_HEAP_ALLOCATION;
    public:
        explicit Scope(StringArena& arena)
            : m_arena(arena)
        {
            m_arena.enter();
        }

        explicit Scope(FFIContext&);

        ~Scope()
        {
            m_arena.exit();
        }

    private:
        StringArena& m_arena;
    };

    StringArena() = default;
    ~StringArena() = default;

    JS_EXPORT_PRIVATE void enter();
    void exit()
    {
        ASSERT(m_depth);
        if (!--m_depth)
            reset();
    }
    unsigned depth() const { return m_depth; }

    JS_EXPORT_PRIVATE std::span<char> allocate(size_t bytes);
    void shrinkWhenIdle()
    {
        if (m_depth)
            return;
        m_offsetInLastChunk = 0;
        m_chunks.clear();
    }

private:
    void reset();

    static constexpr size_t defaultChunkBytes = 4096;
    static constexpr size_t maximumRetainedChunkBytes = 64 * 1024;

    Vector<MallocSpan<char>, 4> m_chunks;
    size_t m_offsetInLastChunk { 0 };
    unsigned m_depth { 0 };
};

class FFIContext final : public HeapObserver {
    WTF_MAKE_TZONE_ALLOCATED(FFIContext);
    WTF_MAKE_NONCOPYABLE(FFIContext);
public:
    JS_EXPORT_PRIVATE explicit FFIContext(VM&);
    JS_EXPORT_PRIVATE ~FFIContext();

    void willGarbageCollect() final { }
    void didGarbageCollect(CollectionScope) final { m_arena.shrinkWhenIdle(); }

    StringArena& stringArena() { return m_arena; }
    StringArena& arena() { return m_arena; }

    const CString* cachedUTF8(StringImpl&);
    const CString& cacheUTF8(StringImpl&, CString&&);

    static constexpr unsigned utf8CacheCapacity = 64;

    void addLiveCallback(VM&, JSGlobalObject& owner, JSFFICallback*);
    void removeLiveCallback(JSGlobalObject& owner, JSFFICallback*);
    template<typename Visitor> void visitLiveCallbacks(JSGlobalObject& owner, Visitor&);

    using ThreadsafeDispatchFunction = void (*)(ThreadsafeInvocation&);
    JS_EXPORT_PRIVATE static void setThreadsafeDispatch(ThreadsafeDispatchFunction);
    static ThreadsafeDispatchFunction threadsafeDispatch() { return s_threadsafeDispatch; }

private:
    Vector<WriteBarrier<JSFFICallback>> m_liveCallbacks;
    JS_EXPORT_PRIVATE static ThreadsafeDispatchFunction s_threadsafeDispatch;
    struct UTF8CacheEntry {
        RefPtr<StringImpl> key;
        CString utf8;
        uint64_t lastUse { 0 };
    };

    VM& m_vm;
    StringArena m_arena;
    Vector<UTF8CacheEntry, utf8CacheCapacity> m_utf8Cache;
    uint64_t m_utf8CacheClock { 0 };
};

inline StringArena::Scope::Scope(FFIContext& context)
    : Scope(context.arena())
{
}

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
