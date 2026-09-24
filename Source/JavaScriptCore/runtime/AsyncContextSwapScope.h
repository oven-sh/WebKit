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

// RAII helper for Bun's AsyncLocalStorage. JSGlobalObject::m_asyncContextData holds what
// async code continues with: field 0 is the async context (AsyncLocalStorage's data), and
// field 1 the owner of the running script, a number, for an embedder that runs the script
// of several owners in one global object (undefined: there is no owner). A job (microtask,
// timer, ...) captures both when it is scheduled, with current(); constructing this scope
// with the captured value installs both for the lifetime of the scope and restores the
// previous values on destruction.
//
// A captured value is one JSValue, because one slot is what reactions and microtasks have
// for it: undefined, the async context, the owner's number, or an InternalFieldTuple
// [async context, owner] when there are both. (An async context is never a number.)
//
// A job that captured "no context" (undefined, or an empty JSValue for callers
// that never capture) runs with no context and no owner: whatever an earlier job left in
// the slots via AsyncLocalStorage.enterWith() is not inherited, and whatever the job
// itself leaves there does not outlive it. Until the VM has enabled tracking
// (VM::isAsyncContextTrackingEnabled) nothing can have been captured, so every
// entry point here reduces to that flag test.
//
// There are three of them. AsyncContextSwapScope is the one to use: it looks for an owner once
// owners are tracked (VM::isAsyncContextOwnerTracked()). The other two are for what runs a VM's
// jobs, which is one of two functions (VM::internalMicrotaskRunner()):
// AsyncContextSwapScopeWithoutOwner knows nothing of owners, and is runInternalMicrotask()'s,
// which runs the jobs until owners are tracked, so that what runs the jobs of a program that has
// no owner is what ran them before there were owners; AsyncContextSwapScopeWithOwner is
// runInternalMicrotaskWithOwner()'s, which runs them from then on.
enum class AsyncContextOwner : uint8_t {
    Never,
    IfTracked,
    Tracked,
};

template<AsyncContextOwner ownerIs>
class AsyncContextSwapScopeFor {
    static constexpr bool mayHaveOwner = ownerIs != AsyncContextOwner::Never;
    static ALWAYS_INLINE bool isOwnerTracked(VM& vm)
    {
        if constexpr (ownerIs == AsyncContextOwner::Tracked)
            return true;
        return vm.isAsyncContextOwnerTracked();
    }

    WTF_MAKE_NONCOPYABLE(AsyncContextSwapScopeFor);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE AsyncContextSwapScopeFor(VM& vm, JSGlobalObject* globalObject, JSValue asyncContext)
        : m_vm(vm)
    {
        if (!vm.isAsyncContextTrackingEnabled())
            return;
        enter(globalObject, asyncContext);
    }

    // For internal microtasks: the captured context is in the dedicated argument
    // when the fast paths filled it, otherwise in an InternalFieldTuple
    // [context, asyncContext] that contextArg is unwrapped from (see wrap()).
    ALWAYS_INLINE AsyncContextSwapScopeFor(VM& vm, JSGlobalObject* globalObject, JSValue asyncContextArg, JSValue& contextArg)
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

    ALWAYS_INLINE ~AsyncContextSwapScopeFor()
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
            if constexpr (mayHaveOwner) {
                if (m_restoreOwner) [[unlikely]]
                    m_asyncContextData->internalField(InternalFieldTuple::Field::Slot1).setWithoutWriteBarrier(m_restoreOwner);
            }
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

    // What to capture for a job being scheduled now: the async context (and the owner) in
    // m_asyncContextData, or jsUndefined() when there is none.
    static ALWAYS_INLINE JSValue current(VM& vm, JSGlobalObject* globalObject)
    {
        if (!vm.isAsyncContextTrackingEnabled())
            return jsUndefined();
        ASSERT(globalObject->m_asyncContextData);
        JSValue asyncContext = globalObject->m_asyncContextData->getInternalField(0);
        if constexpr (mayHaveOwner) {
            if (isOwnerTracked(vm)) [[unlikely]] {
                JSValue owner = globalObject->m_asyncContextData->getInternalField(1);
                if (asyncContext.isUndefined())
                    return owner;
                if (!owner.isUndefined()) [[unlikely]]
                    return globalObject->capturedAsyncContextWithOwner(vm, asyncContext, owner);
            }
        }
        return asyncContext;
    }

    // The two parts of a captured value.
    static ALWAYS_INLINE JSValue asyncContextOf(JSValue captured)
    {
        if (captured.isEmpty() || captured.isInt32())
            return jsUndefined();
        return isContextTuple(captured) ? uncheckedDowncast<InternalFieldTuple>(captured.asCell())->getInternalField(0) : captured;
    }

    static ALWAYS_INLINE JSValue ownerOf(JSValue captured)
    {
        if (captured.isEmpty())
            return jsUndefined();
        if (captured.isInt32())
            return captured;
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
    ALWAYS_INLINE void enter(JSGlobalObject* globalObject, JSValue asyncContext)
    {
        ASSERT(m_vm.isAsyncContextTrackingEnabled());
        ASSERT(globalObject->m_asyncContextData);
        if (asyncContext.isEmpty())
            asyncContext = jsUndefined();
        m_asyncContextData = globalObject->m_asyncContextData.get();
        m_restoreAsyncContext = m_asyncContextData->getInternalField(0);
        if constexpr (mayHaveOwner) {
            if (isOwnerTracked(m_vm)) [[unlikely]] {
                // (Not empty: restoreEarly() takes it to say that there is an owner to put back.)
                m_restoreOwner = m_asyncContextData->getInternalField(1);
                JSValue owner = jsUndefined();
                if (asyncContext.isInt32()) {
                    owner = asyncContext;
                    asyncContext = jsUndefined();
                } else if (isContextTuple(asyncContext)) [[unlikely]] {
                    auto* tuple = uncheckedDowncast<InternalFieldTuple>(asyncContext.asCell());
                    asyncContext = tuple->getInternalField(0);
                    owner = tuple->getInternalField(1);
                }
                // (A number or undefined.)
                m_asyncContextData->internalField(InternalFieldTuple::Field::Slot1).setWithoutWriteBarrier(owner);
            }
        }
        if (m_restoreAsyncContext != asyncContext)
            m_asyncContextData->putInternalField(m_vm, 0, asyncContext);
    }

    struct Nothing { };

    VM& m_vm;
    InternalFieldTuple* m_asyncContextData { nullptr };
    JSValue m_restoreAsyncContext;
    NO_UNIQUE_ADDRESS std::conditional_t<mayHaveOwner, JSValue, Nothing> m_restoreOwner;
};

using AsyncContextSwapScope = AsyncContextSwapScopeFor<AsyncContextOwner::IfTracked>;
using AsyncContextSwapScopeWithoutOwner = AsyncContextSwapScopeFor<AsyncContextOwner::Never>;
using AsyncContextSwapScopeWithOwner = AsyncContextSwapScopeFor<AsyncContextOwner::Tracked>;

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
