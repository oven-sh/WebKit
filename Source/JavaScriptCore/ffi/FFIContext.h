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

#include "JSExportMacros.h"
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
class JSCell;
class JSFFICallback;
class VM;
} // namespace JSC

namespace JSC { namespace FFI {

class ThreadsafeInvocation final : public ThreadSafeRefCounted<ThreadsafeInvocation> {
    WTF_MAKE_TZONE_ALLOCATED(ThreadsafeInvocation);
public:
    static Ref<ThreadsafeInvocation> create(JSFFICallback* callback, void* embedderContext, std::span<const uint64_t> slots)
    {
        return adoptRef(*new ThreadsafeInvocation(callback, embedderContext, slots));
    }

    JSFFICallback* callback() const { return m_callback; }
    void* embedderContext() const { return m_embedderContext; }
    std::span<uint64_t> slots() { return m_slots.mutableSpan(); }
    std::span<const uint64_t> slots() const { return m_slots.span(); }

private:
    ThreadsafeInvocation(JSFFICallback* callback, void* embedderContext, std::span<const uint64_t> slots)
        : m_callback(callback)
        , m_embedderContext(embedderContext)
        , m_slots(slots)
    {
    }

    JSFFICallback* m_callback;
    void* m_embedderContext;
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

        StringArena& arena() { return m_arena; }

    private:
        StringArena& m_arena;
    };

    StringArena() = default;
    ~StringArena() = default;

    JS_EXPORT_PRIVATE void enter();
    void exit()
    {
        ASSERT(m_depth);
        --m_depth;
    }
    unsigned depth() const { return m_depth; }

    JS_EXPORT_PRIVATE std::span<char> allocate(size_t bytes);

private:
    void reset();

    static constexpr size_t defaultChunkBytes = 4096;
    static constexpr size_t maximumRetainedChunkBytes = 64 * 1024;

    Vector<MallocSpan<char>, 4> m_chunks;
    size_t m_offsetInLastChunk { 0 };
    unsigned m_depth { 0 };
};

using ArenaScope = StringArena::Scope;

class FFIContext {
    WTF_MAKE_TZONE_ALLOCATED(FFIContext);
    WTF_MAKE_NONCOPYABLE(FFIContext);
public:
    JS_EXPORT_PRIVATE FFIContext();
    JS_EXPORT_PRIVATE ~FFIContext();

    StringArena& arena() { return m_arena; }
    StringArena& stringArena() { return m_arena; }

    const CString* cachedUTF8(StringImpl&);
    const CString& cacheUTF8(StringImpl&, CString&&);

    static constexpr unsigned utf8CacheCapacity = 64;

    void addLiveCallback(VM&, JSGlobalObject& owner, JSFFICallback*);
    void removeLiveCallback(JSGlobalObject& owner, JSFFICallback*);
    template<typename Visitor> void visitLiveCallbacks(JSGlobalObject& owner, Visitor&);
    unsigned liveCallbackCount() const { return m_liveCallbacks.size(); }

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
