/*
 * Copyright (C) 2026 Codeblog Corp. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
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

#include "InternalFieldTuple.h"
#include "JSCast.h"
#include "JSGlobalObject.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>

namespace JSC {

// RAII helper for Bun's AsyncLocalStorage. JSGlobalObject::m_asyncContextData holds
// what async code continues with: field 0 is the async context (AsyncLocalStorage's
// data), field 1 the script execution owner the running script belongs to, for an
// embedder that has more than one per global object (undefined: the global object's
// own). A job (microtask, timer, ...) captures both when it is scheduled, with
// current(); constructing this scope with the captured value installs both for the
// lifetime of the scope and restores the previous values on destruction.
//
// A captured value is one JSValue, because one slot is what reactions and microtasks
// have for it: field 0's value while there is no owner, otherwise an InternalFieldTuple
// [async context, owner] (asyncContextOf() / scriptExecutionOwnerOf() take it apart).
//
// A job that captured "no context" (undefined, or an empty JSValue for callers
// that never capture) runs with no context and no owner: whatever an earlier job
// left in the slots via AsyncLocalStorage.enterWith() is not inherited, and whatever
// the job itself leaves there does not outlive it. Until the VM has enabled tracking
// (VM::isAsyncContextTrackingEnabled) nothing can have been captured, so every
// entry point here reduces to that flag test.
class AsyncContextSwapScope {
    WTF_MAKE_NONCOPYABLE(AsyncContextSwapScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE AsyncContextSwapScope(VM& vm, JSGlobalObject* globalObject, JSValue asyncContext)
        : m_vm(vm)
    {
        if (!vm.isAsyncContextTrackingEnabled())
            return;
        enter(globalObject, asyncContext);
    }

    // For internal microtasks: the captured context is in the dedicated argument
    // when the fast paths filled it, otherwise in an InternalFieldTuple
    // [context, asyncContext] that contextArg is unwrapped from (see wrap()).
    ALWAYS_INLINE AsyncContextSwapScope(VM& vm, JSGlobalObject* globalObject, JSValue asyncContextArg, JSValue& contextArg)
        : m_vm(vm)
    {
        if (!vm.isAsyncContextTrackingEnabled()) {
            ASSERT(!isContextTuple(contextArg));
            return;
        }
        JSValue asyncContext = asyncContextArg;
        if (asyncContext.isEmpty() || asyncContext.isUndefined()) {
            if (isContextTuple(contextArg)) [[unlikely]]
                asyncContext = unwrapContextTuple(contextArg);
        }
        enter(globalObject, asyncContext);
    }

    ALWAYS_INLINE ~AsyncContextSwapScope()
    {
        restoreEarly();
    }

    // Restore the previous async context before the scope's natural end. Later
    // destruction becomes a no-op. Use this when the tail of a case must run
    // with the caller's context restored (e.g. resolving a promise whose
    // resolution may itself capture the current async context).
    ALWAYS_INLINE void restoreEarly()
    {
        if (m_asyncContextData) {
            m_asyncContextData->putInternalField(m_vm, 0, m_restoreAsyncContext);
            if (m_asyncContextData->getInternalField(1) != m_restoreScriptExecutionOwner) [[unlikely]]
                m_asyncContextData->putInternalField(m_vm, 1, m_restoreScriptExecutionOwner);
            m_asyncContextData = nullptr;
        }
    }

    static ALWAYS_INLINE bool isContextTuple(JSValue contextArg)
    {
        // JSType test first: rejects the usual non-tuple cells (generators,
        // iterators, module records) without the ClassInfo walk.
        return !contextArg.isEmpty() && contextArg.isCell() && contextArg.asCell()->type() == InternalFieldTupleType && contextArg.asCell()->inherits<InternalFieldTuple>();
    }

    // If contextArg is an InternalFieldTuple [userContext, asyncContext],
    // overwrite contextArg with field 0 and return field 1. Otherwise leave
    // contextArg untouched and return jsUndefined(). Empty contextArg is
    // tolerated.
    static ALWAYS_INLINE JSValue unwrapContextTuple(JSValue& contextArg)
    {
        if (!isContextTuple(contextArg))
            return jsUndefined();
        auto* tuple = uncheckedDowncast<InternalFieldTuple>(contextArg.asCell());
        contextArg = tuple->getInternalField(0);
        return tuple->getInternalField(1);
    }

    // What to capture for a job being scheduled now: the async context and the script
    // execution owner in m_asyncContextData, or jsUndefined() when there is neither.
    static ALWAYS_INLINE JSValue current(VM& vm, JSGlobalObject* globalObject)
    {
        if (!vm.isAsyncContextTrackingEnabled())
            return jsUndefined();
        ASSERT(globalObject->m_asyncContextData);
        if (globalObject->m_asyncContextData->getInternalField(1).isUndefined()) [[likely]]
            return globalObject->m_asyncContextData->getInternalField(0);
        return globalObject->currentAsyncContextWithScriptExecutionOwner(vm);
    }

    // The captured value of an async context and a script execution owner that are not
    // (or not yet) the current ones.
    static ALWAYS_INLINE JSValue captured(VM& vm, JSGlobalObject* globalObject, JSValue asyncContext, JSValue scriptExecutionOwner)
    {
        if (scriptExecutionOwner.isUndefined())
            return asyncContext;
        return InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure(), asyncContext, scriptExecutionOwner);
    }

    // The two parts of a captured value.
    static ALWAYS_INLINE JSValue asyncContextOf(JSValue captured)
    {
        return isContextTuple(captured) ? uncheckedDowncast<InternalFieldTuple>(captured.asCell())->getInternalField(0) : captured;
    }

    static ALWAYS_INLINE JSValue scriptExecutionOwnerOf(JSValue captured)
    {
        return isContextTuple(captured) ? uncheckedDowncast<InternalFieldTuple>(captured.asCell())->getInternalField(1) : jsUndefined();
    }

    // Pair userContext with asyncContext in an InternalFieldTuple
    // [userContext, asyncContext] for the paths that only have one slot to
    // carry both. When asyncContext is none, returns userContext unchanged.
    static ALWAYS_INLINE JSValue wrap(VM& vm, JSGlobalObject* globalObject, JSValue userContext, JSValue asyncContext)
    {
        if (asyncContext.isEmpty() || asyncContext.isUndefined())
            return userContext;
        ASSERT(vm.isAsyncContextTrackingEnabled());
        return InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure(), userContext, asyncContext);
    }

    static ALWAYS_INLINE JSValue wrapWithCurrent(VM& vm, JSGlobalObject* globalObject, JSValue userContext)
    {
        return wrap(vm, globalObject, userContext, current(vm, globalObject));
    }

private:
    // The previous value is put back on exit even when nothing had to be
    // installed, so whatever the job itself leaves in the slot (enterWith())
    // ends with the job.
    ALWAYS_INLINE void enter(JSGlobalObject* globalObject, JSValue captured)
    {
        ASSERT(m_vm.isAsyncContextTrackingEnabled());
        ASSERT(globalObject->m_asyncContextData);
        JSValue asyncContext = captured.isEmpty() ? jsUndefined() : captured;
        JSValue scriptExecutionOwner = jsUndefined();
        if (isContextTuple(captured)) [[unlikely]] {
            auto* tuple = uncheckedDowncast<InternalFieldTuple>(captured.asCell());
            asyncContext = tuple->getInternalField(0);
            scriptExecutionOwner = tuple->getInternalField(1);
        }
        m_asyncContextData = globalObject->m_asyncContextData.get();
        m_restoreAsyncContext = m_asyncContextData->getInternalField(0);
        m_restoreScriptExecutionOwner = m_asyncContextData->getInternalField(1);
        if (m_restoreAsyncContext != asyncContext)
            m_asyncContextData->putInternalField(m_vm, 0, asyncContext);
        if (m_restoreScriptExecutionOwner != scriptExecutionOwner) [[unlikely]]
            m_asyncContextData->putInternalField(m_vm, 1, scriptExecutionOwner);
    }

    VM& m_vm;
    InternalFieldTuple* m_asyncContextData { nullptr };
    JSValue m_restoreAsyncContext;
    JSValue m_restoreScriptExecutionOwner;
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
