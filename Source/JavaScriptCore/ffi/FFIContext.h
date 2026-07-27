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

// FFI-SPEC-GAP: SPEC section 14 asks for `#if USE(JSVALUE64)` around every FFI
// file, but A6's JSGlobalObject hooks (the `std::unique_ptr<FFI::FFIContext>`
// member and JSGlobalObject::ffiContext()) are guarded by USE(BUN_JSC_ADDITIONS)
// alone, so FFIContext must be a complete type on 32-bit builds too (nothing in
// this file has a 64-bit dependence). FFIConversions and the JIT rows keep the
// USE(JSVALUE64) guard.

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

// A threadsafe callback's foreign-thread invocation, captured as plain data. Created on the
// FOREIGN thread inside ffiCallbackDispatch (no JS values, no VM access -- only these raw slot
// words and pointers, all copyable off-thread), handed to the embedder's dispatch function, and
// consumed on the JS thread by FFI::runThreadsafeInvocation(). ThreadSafeRefCounted so the record
// can safely outlive the foreign thread's stack frame while queued.
class ThreadsafeInvocation final : public ThreadSafeRefCounted<ThreadsafeInvocation> {
    WTF_MAKE_TZONE_ALLOCATED(ThreadsafeInvocation);
public:
    static Ref<ThreadsafeInvocation> create(JSFFICallback* callback, void* embedderContext, std::span<const uint64_t> slots)
    {
        return adoptRef(*new ThreadsafeInvocation(callback, embedderContext, slots));
    }

    // The callback cell. LIFETIME: this record was COUNTED by the dispatch (see
    // JSFFICallback::tryBeginThreadsafeInvocation), and a nonzero pending count keeps the cell in
    // the live-callback root set -- close() with pending records defers its unroot until the last
    // one drains (endThreadsafeInvocation). So the raw pointer is valid until runThreadsafeInvocation
    // retires it, and, through the cell's m_callable barrier, so is the JS callable it invokes.
    JSFFICallback* callback() const { return m_callback; }
    // Opaque pointer the embedder supplied at creation (e.g. an event-loop / context handle):
    // captured on the JS thread, read on the foreign thread, never dereferenced by the engine.
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

// Call-scoped bump allocator that owns the NUL-terminated UTF-8 copies made
// for Type::CString JS-string arguments and callback return values (SPEC
// section 5 / 9.3 step 6). Brackets nest: enter()/exit() bump a depth counter
// and nested (re-entrant) FFI calls share the same storage.
//
// FFI-SPEC-GAP: SPEC sections 5/6 say the storage is "reset when depth returns
// to 0", but a `cstring`-returning callback's UTF-8 copy is written by
// callbackDispatch inside ITS OWN Scope and is read by native code only after
// the callback thunk returns -- i.e. after that Scope's exit(). When the
// enclosing FFI call did not bracket the arena (the IC stub never does, and the
// DFG/FTL typed path brackets only for UntypedUse cstring/pointer arguments),
// the dispatch's Scope is the outermost bracket, so freeing at exit() would hand
// native code a dangling char*. Reclamation is therefore deferred: exit() only
// decrements the depth, and the storage handed out during any bracket is
// recycled at the NEXT outermost enter() on this arena (the start of the next
// FFI activity on this context). Pointers thus stay valid until the next FFI
// call begins, which is what a callback's cstring return needs to survive back
// into native code, and holds regardless of whether the outer tier bracketed.
// The bracketed tiers (host path, IC-stub slow path, callbackDispatch, the
// DFG/FTL cstring/pointer path) keep the original guarantee unchanged: while
// their depth is non-zero nothing is recycled, so every string transcoded
// during the outermost bracketed call survives until that call returns.
// Residual gap for the spec review: under an UN-bracketed outer tier a second
// callback invocation is the "next FFI activity", so a native caller that
// retains an earlier callback-returned cstring across a later callback call on
// the same context reads recycled storage; closing that requires the IC stub /
// typed DFG path to bracket calls that can re-enter (rows A7/A9/A10).
class StringArena {
    WTF_MAKE_NONCOPYABLE(StringArena);
public:
    // RAII bracket used by the C++ host path, operationFFICallSlowPath and
    // FFI::callbackDispatch. The DFG/FTL CallFFI paths bracket explicitly with
    // operationFFIArenaEnter / operationFFIArenaExit instead.
    class Scope {
        WTF_MAKE_NONCOPYABLE(Scope);
        WTF_FORBID_HEAP_ALLOCATION;
    public:
        explicit Scope(StringArena& arena)
            : m_arena(arena)
        {
            m_arena.enter();
        }

        // FFI-SPEC-GAP: the spec spells the RAII both as `StringArena::Scope arenaScope(ctx)`
        // (section 8.2) and as an `ArenaScope` (section 5); accept an FFIContext& as
        // well as a StringArena& and provide the ArenaScope alias below so every
        // caller compiles. Defined after FFIContext.
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

    // Opens a bracket. The outermost enter() (depth 0 -> 1) first recycles all
    // storage handed out by previously completed brackets (see the deferred
    // reclamation note above).
    JS_EXPORT_PRIVATE void enter();
    // Closes a bracket. Never frees: storage stays valid until the next
    // outermost enter(). Safe to call while a VM exception is pending (the
    // DFG/FTL CallFFI path pops the arena on its exception edge).
    void exit()
    {
        ASSERT(m_depth);
        --m_depth;
    }
    unsigned depth() const { return m_depth; }

    // Returns `bytes` bytes of storage valid until the next outermost
    // enter(), or an empty span on allocation failure. The caller must be
    // inside a bracket (StringArena::Scope or operationFFIArenaEnter/Exit).
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

// Per-JSGlobalObject FFI state (SPEC section 6), lazily created and owned by
// JSGlobalObject::ffiContext(). Nothing in here is GC-visited EXCEPT the live-callback set,
// which JSGlobalObject::visitChildren visits via visitLiveCallbacks(): that set is the single
// engine-side root that keeps every un-close()d JSFFICallback alive (native code may hold its
// entry pointer with no JS reference in existence), replacing any embedder-side Strong handles.
//
// Thread contract: lazy first-creation happens on the mutator (JS) thread only.
// DFG/FTL compiler threads bake `&globalObject->ffiContext()` at
// code-generation time and may therefore only OBSERVE an already-created
// context; JSFFIFunction::create (which always precedes any CallFFI
// compilation for that function, on the mutator thread) forces the eager
// creation so the compiler never races the makeUnique.
class FFIContext {
    WTF_MAKE_TZONE_ALLOCATED(FFIContext);
    WTF_MAKE_NONCOPYABLE(FFIContext);
public:
    JS_EXPORT_PRIVATE FFIContext();
    JS_EXPORT_PRIVATE ~FFIContext();

    StringArena& arena() { return m_arena; }
    // FFI-SPEC-GAP: the spec does not name this accessor; provide both spellings.
    StringArena& stringArena() { return m_arena; }

    // Small LRU (utf8CacheCapacity entries) mapping a resolved StringImpl to
    // its UTF-8 encoding, so a hot Type::CString argument is not re-transcoded
    // on every call. Correctness never depends on it: the returned bytes are
    // always copied into the call-scoped StringArena by the conversion code.
    const CString* cachedUTF8(StringImpl&);
    const CString& cacheUTF8(StringImpl&, CString&&);

    static constexpr unsigned utf8CacheCapacity = 64;

    // ---- live-callback rooting ----
    // A JSFFICallback whose entry pointer native code may hold must not be collected merely
    // because JS dropped its reference. From creation until close() every callback lives in this
    // set, which the global object visits => the GC treats it as reachable. close() removes it.
    // This is the ONLY mechanism keeping such cells alive; embedders need no Strong handles.
    // Concurrency: the marker iterates this set from JSGlobalObject::visitChildren on
    // concurrent/parallel marker threads while the mutator appends/removes, so ALL THREE take
    // the owning global object's cellLock() (the m_weakTickets pattern in JSGlobalObject.cpp).
    // The lock is passed in because it lives on the global object, not the context.
    void addLiveCallback(VM&, JSGlobalObject& owner, JSFFICallback*);
    void removeLiveCallback(JSGlobalObject& owner, JSFFICallback*);
    template<typename Visitor> void visitLiveCallbacks(JSGlobalObject& owner, Visitor&);
    unsigned liveCallbackCount() const { return m_liveCallbacks.size(); }

    // ---- threadsafe dispatch ----
    // The embedder registers ONE process-wide dispatch function. When a THREADSAFE callback is
    // entered from a foreign thread, the engine copies the raw argument slots into a
    // ThreadsafeInvocation record and hands it to this function (still on the foreign thread; the
    // record is plain refcounted C data, no JS values, no VM access). The embedder queues it to
    // its JS thread and there calls FFI::runThreadsafeInvocation(record), which converts the slots
    // to JS values and invokes the callable. All JS-value creation therefore happens on the JS
    // thread. Registration is static because the foreign thread must not read per-global state.
    using ThreadsafeDispatchFunction = void (*)(ThreadsafeInvocation&);
    JS_EXPORT_PRIVATE static void setThreadsafeDispatch(ThreadsafeDispatchFunction);
    static ThreadsafeDispatchFunction threadsafeDispatch() { return s_threadsafeDispatch; }

private:
    // WriteBarrier'd against the global object's own cell (the owner passed to addLiveCallback),
    // so mutation gets a proper write barrier and visiting is a plain append.
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
