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
#include "VM.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>

namespace JSC {

// RAII helper for Bun's AsyncLocalStorage. A job (microtask, timer, ...)
// captures the value of JSGlobalObject::m_asyncContextData field 0 when it is
// scheduled; constructing this scope with that captured value installs it for
// the lifetime of the scope and restores the previous value on destruction.
//
// A job that captured "no context" (undefined, or an empty JSValue for callers
// that never capture) runs with no context: whatever an earlier job left in the
// slot via AsyncLocalStorage.enterWith() is not inherited, and whatever the job
// itself leaves there does not outlive it. Until the VM has enabled tracking
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

    // The async context to capture for a job being scheduled now: field 0 of
    // m_asyncContextData, or jsUndefined() when there is none.
    static ALWAYS_INLINE JSValue current(VM& vm, JSGlobalObject* globalObject)
    {
        if (!vm.isAsyncContextTrackingEnabled())
            return jsUndefined();
        ASSERT(globalObject->m_asyncContextData);
        return globalObject->m_asyncContextData->getInternalField(0);
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

protected:
    // For a scope that decides by itself whether it enters: ModuleLoaderAsyncContextScope.
    enum DoNotEnterTag { DoNotEnter };
    ALWAYS_INLINE AsyncContextSwapScope(VM& vm, DoNotEnterTag)
        : m_vm(vm)
    {
    }

    bool didEnter() const { return m_asyncContextData; }

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
        if (m_restoreAsyncContext != asyncContext)
            m_asyncContextData->putInternalField(m_vm, 0, asyncContext);
    }

private:
    VM& m_vm;
    InternalFieldTuple* m_asyncContextData { nullptr };
    JSValue m_restoreAsyncContext;
};

// For a job of the module loader, and for what the loader calls on its way.
//
// A load keeps the async context of whoever started it: what was current where import() was called. It is in the cells of
// the load (ModuleLoadingContext, ModuleLoaderPayload, ModuleGraphLoadingState, and ModuleRegistryEntry until the record
// is made), and every request that the load makes takes it from there: JSModuleLoader::asyncContextOfLoad(). Where that
// context is a store, a job of the load is a continuation of the import(): it runs under the store, and what it leaves
// in the slot ends with it.
//
// A load that captured no store has no scope. Its jobs run as such jobs always did: under what is in the slot, and what
// they leave there stays. That is:
// - an import() outside any store, and the load of an entry point, whose module enters what the program goes on in;
// - a synchronous load (JSModuleLoader::loadModuleSync()), which captures nothing: what it runs inside the call of
//   require() runs in the context of the caller, and what its modules enter is the caller's, as for a CommonJS module.
//
// While a synchronous load drains its queue, no job of the loader enters the context of its load. The drain also takes
// the jobs of other loads that come due meanwhile: of an import() in flight, and of an import() that a module calls
// during the drain. It runs them inside the call of require(), so they run in the context of its caller, as the steps
// of the synchronous load do. (The job that resumes a module behind its own await is no such job: it has the context
// of that await, as any await has.)
class ModuleLoaderAsyncContextScope : public AsyncContextSwapScope {
public:
    ALWAYS_INLINE ModuleLoaderAsyncContextScope(VM& vm, JSGlobalObject* globalObject, JSValue asyncContext)
        : AsyncContextSwapScope(vm, DoNotEnter)
    {
        if (!wouldEnter(vm, asyncContext)) [[likely]]
            return;
        enter(globalObject, asyncContext);
    }

    static ALWAYS_INLINE bool wouldEnter(VM& vm, JSValue asyncContext)
    {
        if (asyncContext.isEmpty() || asyncContext.isUndefined()) [[likely]]
            return false;
        return !vm.synchronousModuleQueue();
    }
};

// For a job of the module loader that evaluates modules. A module with a top-level await leaves a job behind that runs
// the modules that wait for it (CyclicModuleRecord::executeAsync()). That job continues the job that began to evaluate
// the module: JSModuleLoader::asyncContextOfModulesThatWait() says what it gets.
// - Where the job entered the context of its load, the slot is the job's own: the job behind the await gets what is
//   current where the awaiting module begins, as any continuation does.
// - Where the job did not enter it (the drain of a synchronous load), it gets the context of the load.
// - Where the load keeps no context, it gets none, as before there was this scope. What the slot holds in a job that
//   entered nothing is not the job's to hand on: it can be the context of whoever runs the job.
class ModuleEvaluationAsyncContextScope final : public ModuleLoaderAsyncContextScope {
public:
    ALWAYS_INLINE ModuleEvaluationAsyncContextScope(VM& vm, JSGlobalObject* globalObject, JSValue asyncContext)
        : ModuleLoaderAsyncContextScope(vm, globalObject, asyncContext)
        , m_vm(vm)
        , m_asyncContextOfLoad(asyncContext)
        , m_outer(std::exchange(vm.m_moduleEvaluationAsyncContextScope, this))
    {
    }

    ALWAYS_INLINE ~ModuleEvaluationAsyncContextScope()
    {
        m_vm.m_moduleEvaluationAsyncContextScope = m_outer;
    }

    using ModuleLoaderAsyncContextScope::didEnter;
    JSValue asyncContextOfLoad() const { return m_asyncContextOfLoad; }

private:
    VM& m_vm;
    JSValue m_asyncContextOfLoad;
    ModuleEvaluationAsyncContextScope* m_outer;
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
