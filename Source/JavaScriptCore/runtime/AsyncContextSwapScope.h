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
#include "JSAsyncFunctionGenerator.h"
#include "JSAsyncGenerator.h"
#include "JSCast.h"
#include "JSGlobalObject.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>

namespace JSC {

// RAII helper for Bun's AsyncLocalStorage: swaps an async context value into
// JSGlobalObject::m_asyncContextData field 0 for the lifetime of the scope and
// restores the previous value on destruction. A no-op when the supplied context
// is empty, or undefined while async context tracking has never been enabled on
// the global, so the common path (no async context in use) costs a branch. Also provides helpers for the snapshot side (capturing the
// current context and wrapping it into an InternalFieldTuple alongside a user
// context) and for unwrapping such a tuple on the restore side.
class AsyncContextSwapScope {
    WTF_MAKE_NONCOPYABLE(AsyncContextSwapScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    ALWAYS_INLINE AsyncContextSwapScope(VM& vm, JSGlobalObject* globalObject, JSValue asyncContext)
        : m_vm(vm)
    {
        if (asyncContext.isEmpty())
            return;
        // Once anything uses async context, every continuation installs the
        // context it captured -- including "none" -- so a context entered inside
        // one continuation (AsyncLocalStorage.enterWith, an activated span) is
        // scoped to that continuation instead of leaking into whichever job
        // happens to run next. Until then this stays a single branch.
        if (asyncContext.isUndefined() && !globalObject->isAsyncContextTrackingEnabled())
            return;
        m_asyncContextData = globalObject->m_asyncContextData.get();
        if (!m_asyncContextData)
            return;
        m_restoreAsyncContext = m_asyncContextData->getInternalField(0);
        if (m_restoreAsyncContext != asyncContext)
            m_asyncContextData->putInternalField(vm, 0, asyncContext);
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

    // If contextArg is an InternalFieldTuple [userContext, asyncContext],
    // overwrite contextArg with field 0 and return field 1. Otherwise leave
    // contextArg untouched and return jsUndefined(). Empty contextArg is
    // tolerated (dynamicDowncast<T>(JSValue) is not empty-safe on its own).
    static ALWAYS_INLINE JSValue unwrapContextTuple(JSValue& contextArg)
    {
        if (contextArg.isEmpty())
            return jsUndefined();
        if (auto* tuple = dynamicDowncast<InternalFieldTuple>(contextArg)) {
            contextArg = tuple->getInternalField(0);
            return tuple->getInternalField(1);
        }
        return jsUndefined();
    }

    // Read the current async context (field 0 of m_asyncContextData), or
    // jsUndefined() when tracking has not been enabled on this global.
    static ALWAYS_INLINE JSValue current(JSGlobalObject* globalObject)
    {
        if (auto* asyncContextData = globalObject->m_asyncContextData.get())
            return asyncContextData->getInternalField(0);
        return jsUndefined();
    }

    // Snapshot the current async context alongside userContext in an
    // InternalFieldTuple [userContext, asyncContext]. When no async context is
    // active, returns userContext unchanged so the caller keeps using the
    // allocation-free inline/slim reaction fast paths.
    static ALWAYS_INLINE JSValue wrapWithCurrent(VM& vm, JSGlobalObject* globalObject, JSValue userContext)
    {
        JSValue asyncContext = current(globalObject);
        if (asyncContext.isUndefined())
            return userContext;
        return InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure(), userContext, asyncContext);
    }

    // Await-side capture. When the suspending object is an async (generator)
    // function, record the current async context in its AsyncContext slot — a
    // suspended async function has exactly one outstanding await, so this is a
    // single barriered store instead of an InternalFieldTuple allocation per
    // await — and return the object itself. Other drivers (AsyncFromSyncIterator,
    // top-level-await module records) are rare and keep the tuple path.
    static ALWAYS_INLINE JSValue captureForAwait(VM& vm, JSGlobalObject* globalObject, JSValue driver)
    {
        auto* asyncContextData = globalObject->m_asyncContextData.get();
        if (!asyncContextData)
            return driver;
        JSValue asyncContext = asyncContextData->getInternalField(0);
        if (driver.isCell()) {
            JSCell* cell = driver.asCell();
            JSType type = cell->type();
            if (type == JSAsyncFunctionGeneratorType) {
                auto* generator = uncheckedDowncast<JSAsyncFunctionGenerator>(cell);
                if (generator->asyncContext() != asyncContext)
                    generator->setAsyncContext(vm, asyncContext);
                return driver;
            }
            if (type == JSAsyncGeneratorType) {
                auto* generator = uncheckedDowncast<JSAsyncGenerator>(cell);
                if (generator->asyncContext() != asyncContext)
                    generator->setAsyncContext(vm, asyncContext);
                return driver;
            }
        }
        if (asyncContext.isUndefined())
            return driver;
        return InternalFieldTuple::create(vm, globalObject->internalFieldTupleStructure(), driver, asyncContext);
    }

    // Resume-side counterpart of captureForAwait(): yields the async context to
    // restore and leaves contextArg pointing at the unwrapped driver.
    static ALWAYS_INLINE JSValue contextForResume(JSValue& contextArg)
    {
        if (contextArg.isCell()) {
            JSCell* cell = contextArg.asCell();
            JSType type = cell->type();
            if (type == JSAsyncFunctionGeneratorType)
                return uncheckedDowncast<JSAsyncFunctionGenerator>(cell)->asyncContext();
            if (type == JSAsyncGeneratorType)
                return uncheckedDowncast<JSAsyncGenerator>(cell)->asyncContext();
        }
        return unwrapContextTuple(contextArg);
    }

private:
    VM& m_vm;
    InternalFieldTuple* m_asyncContextData { nullptr };
    JSValue m_restoreAsyncContext;
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
