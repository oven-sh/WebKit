/*
 * Copyright (C) 2009-2023 Apple Inc. All rights reserved.
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

#include "ArgList.h"
#include "CallLinkInfoBase.h"
#include "ExceptionHelpers.h"
#include "Interpreter.h"
#include "JSFunction.h"
#include "JSFunctionInlines.h"
#include "ProtoCallFrameInlines.h"
#include "VMEntryScope.h"
#include "VMEntryScopeInlines.h"
#include "VMInlines.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Scope.h>
#include <wtf/Threading.h>

namespace JSC {

class CachedCall : public CallLinkInfoBase {
    WTF_MAKE_NONCOPYABLE(CachedCall);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    JS_EXPORT_PRIVATE CachedCall(JSGlobalObject*, JSFunction*, int argumentCount);

    ~CachedCall()
    {
        // AB17e F4 (object-lifetime closure): delist FIRST, before the member
        // teardown below. A locked drain
        // (CodeBlock::unlinkOrUpgradeIncomingCalls) can be
        // mid-unlinkOrUpgradeImpl on this node (reading m_addressForCall /
        // m_protoCallFrame) when this stack object dies; removeOnDestruction
        // acquires the link lock unconditionally gilOff, so we either delist
        // before any drain observes the node or block until the drain loop
        // ends. ~CallLinkInfoBase's own gilOff delist would run only AFTER
        // this store — too late.
        if (g_jscConfig.gilOffProcess) [[unlikely]]
            removeOnDestruction();
        WTF::atomicStore(&m_addressForCall, static_cast<void*>(nullptr), std::memory_order_relaxed); // THREADS: see unlinkOrUpgradeImpl.
    }

    ALWAYS_INLINE JSValue call()
    {
        ASSERT(m_valid);
        ASSERT(m_arguments.size() == static_cast<size_t>(m_protoCallFrame.argumentCount()));
        return m_vm.interpreter.executeCachedCall(*this);
    }

    JSFunction* function()
    {
        ASSERT(m_valid);
        return uncheckedDowncast<JSFunction>(m_protoCallFrame.calleeValue.unboxedCell());
    }
    FunctionExecutable* functionExecutable() { return m_functionExecutable; }
    JSScope* scope() { return m_scope; }

    void setThis(JSValue v) { m_protoCallFrame.setThisValue(v); }

    void clearArguments() { m_arguments.clear(); }
    void appendArgument(JSValue v) { m_arguments.append(v); }
    bool hasOverflowedArguments() { return m_arguments.hasOverflowed(); }

    void unlinkOrUpgradeImpl(VM&, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock)
    {
        // THREADS (TSAN wave 2, cachedcall-protoframe-crossthread; SPEC-jit
        // §4.x/§5.8 NOT blessed): a CachedCall is a STACK-resident object on
        // its constructing thread. The locked install drain
        // (CodeBlock::unlinkOrUpgradeIncomingCalls ← ScriptExecutable::
        // installCode) walks the incoming-call list on whatever thread tiers
        // the function up; reaching here on a FOREIGN thread and writing
        // m_protoCallFrame / m_addressForCall is a cross-thread write into
        // another thread's stack while that thread struct-copies the proto
        // frame in executeCachedCall (TSAN: setCodeBlock vs __tsan_memcpy).
        // Foreign drains therefore touch ONLY two things: the sentinel-list
        // links (under s_callLinkSerializationLock, which the gilOff drain
        // holds across the whole loop) and the dedicated m_staleGeneration
        // signalling word. The owning thread's next call() acquires the bumped
        // generation, nulls its own m_addressForCall, and lazily relink()s —
        // upgrade happens on the OWNING thread, never here. m_ownerThread was
        // member-initialized strictly before the locked linkIncomingCall push
        // that made this node reachable, so the lock orders that read.
        if (g_jscConfig.gilOffProcess && m_ownerThread != &Thread::currentSingleton()) [[unlikely]] {
            if (isOnList())
                removeOnDestruction();
            WTF::atomicStore(&m_staleGeneration, WTF::atomicLoad(&m_staleGeneration, std::memory_order_relaxed) + 1, std::memory_order_release);
            return;
        }

        // Same-thread (or gilOn) drain: m_protoCallFrame / m_addressForCall are
        // owner-thread-only words after the foreign-skip above, so the rewrite
        // below is sequential w.r.t. every reader. The list removal still goes
        // through the locked helper gilOff (precondition 11; the relink push
        // goes through CodeBlock::linkIncomingCall which locks its push).
        if (isOnList())
            removeOnDestruction();

        if (newCodeBlock && m_protoCallFrame.codeBlock() == oldCodeBlock) {
            newCodeBlock->m_shouldAlwaysBeInlined = false;
            m_protoCallFrame.setCodeBlock(newCodeBlock);
            WTF::atomicStore(&m_addressForCall, newCodeBlock->jitCode()->addressForCall(), std::memory_order_release);
            newCodeBlock->linkIncomingCall(nullptr, this);
            return;
        }
        WTF::atomicStore(&m_addressForCall, static_cast<void*>(nullptr), std::memory_order_relaxed);
    }

    void relink();


    template<typename... Args> requires (std::is_convertible_v<Args, JSValue> && ...)
    JSValue callWithArguments(JSGlobalObject*, JSValue thisValue, Args...);

private:
    VM& m_vm;
    VMEntryScope m_entryScope;
    ProtoCallFrame m_protoCallFrame;
    MarkedArgumentBuffer m_arguments;

    // THREADS (cachedcall-protoframe-crossthread): owner-side half of the
    // foreign-skip in unlinkOrUpgradeImpl. Called on the OWNING thread before
    // each m_addressForCall load; if a foreign-thread drain bumped the
    // generation we null our entry so the existing !entry path lazily
    // relink()s on this thread. A missed bump (relaxed propagation lag) just
    // executes one more iteration on the stale-but-matched (codeBlock, entry)
    // pair the gilOff snapshot path already tolerates; the next call observes
    // it. Single-writer (foreign drains hold s_callLinkSerializationLock),
    // single-reader (owner), so the relaxed RMW in the writer and the
    // acquire/observed pair here are sufficient.
    ALWAYS_INLINE void absorbForeignStaleGeneration()
    {
        unsigned gen = WTF::atomicLoad(&m_staleGeneration, std::memory_order_acquire);
        if (gen != m_observedGeneration) [[unlikely]] {
            m_observedGeneration = gen;
            WTF::atomicStore(&m_addressForCall, static_cast<void*>(nullptr), std::memory_order_relaxed);
        }
    }

    FunctionExecutable* m_functionExecutable;
    JSScope* m_scope;
    void* m_addressForCall { nullptr };
    unsigned m_numParameters { 0 };
    // THREADS: identity of the constructing (= stack-owning) thread. Written
    // once during member-init (gated on the read-only Config-page byte so
    // flag-off pays only a predicted-not-taken test, no TLS read), read by
    // foreign-thread drains under the link lock that ordered the push.
    Thread* m_ownerThread { g_jscConfig.gilOffProcess ? &Thread::currentSingleton() : nullptr };
    // THREADS: dedicated cross-thread signalling word; the ONLY non-list word
    // a foreign-thread drain writes on this stack object. Release-stored under
    // the link lock, acquire-loaded by absorbForeignStaleGeneration().
    unsigned m_staleGeneration { 0 };
    unsigned m_observedGeneration { 0 }; // owner-thread only.
#if ASSERT_ENABLED
    bool m_valid { false };
#endif

    friend class Interpreter;
};

} // namespace JSC
