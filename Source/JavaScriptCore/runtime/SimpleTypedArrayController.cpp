/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
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

#include "config.h"
#include "SimpleTypedArrayController.h"

#include "ArrayBuffer.h"
#include "GCSafepointEpoch.h"
#include "HeapInlines.h"
#include "JSArrayBuffer.h"
#include "JSCast.h"
#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "JSGlobalObjectInlines.h"

namespace JSC {

SimpleTypedArrayController::SimpleTypedArrayController(bool allowAtomicsWait)
    : m_allowAtomicsWait(allowAtomicsWait)
{
}

SimpleTypedArrayController::~SimpleTypedArrayController() = default;

JSArrayBuffer* SimpleTypedArrayController::toJS(JSGlobalObject* lexicalGlobalObject, JSGlobalObject* globalObject, ArrayBuffer& native)
{
    UNUSED_PARAM(lexicalGlobalObject);
    // Concurrent read of the published wrapper slot (first-wins protocol; see
    // ArrayBuffer.h). This mirrors Weak<>::get(), but reads through the
    // relaxed Atomic<WeakImpl*> mirror so the load is not a data race against
    // a concurrent registerWrapper() on another thread.
    if (WeakImpl* impl = native.wrapperImplConcurrently()) {
        if (impl->state() == WeakImpl::Live)
            return uncheckedDowncast<JSArrayBuffer>(impl->jsValue().asCell());
    }

    // The JSArrayBuffer::create function will register the wrapper in finishCreation.
    return JSArrayBuffer::create(globalObject->vm(), globalObject->arrayBufferStructure(native.sharingMode()), &native);
}

void SimpleTypedArrayController::registerWrapper(JSGlobalObject*, ArrayBuffer& native, JSArrayBuffer& wrapper)
{
    // First-wins CAS publication (see ArrayBuffer.h). Under GIL-off, two
    // threads can each see a null wrapper in toJS() and both reach here with
    // their own freshly created JSArrayBuffer. Exactly one Weak is installed
    // per publication; a loser whose rival is still Live drops its Weak on
    // return (its JSArrayBuffer remains a valid, merely uncached, wrapper for
    // the same ArrayBuffer). With a single thread the fast-path CAS succeeds
    // whenever the slot is empty, matching the old unconditional store.
    Weak<JSArrayBuffer> weak(&wrapper, &m_owner);
    if (native.tryPublishWrapperImpl(weak.unsafeImpl())) {
        // We won the first publication: hand lifetime ownership of the
        // published WeakImpl to the ArrayBuffer.
        native.m_wrapper = WTF::move(weak);
        return;
    }

    // Lost the CAS: a wrapper impl is already published. If it is still Live,
    // another thread cached its wrapper first — drop ours. If it is dead (the
    // cached wrapper was GC'd while the native ArrayBuffer survived), replace
    // it: the old single-threaded code unconditionally re-registered here,
    // and wrapper identity caching must keep working after the first wrapper
    // dies in BOTH flag modes (TSAN-TRIAGE §6.35 amendment). Republication is
    // serialized by a leaf lock. Per SPEC-ungil §LK WS row (i), no Weak is
    // created or destroyed under the lock: `weak` was constructed above and
    // `displaced` is dealt with after the Locker scope.
    Weak<JSArrayBuffer> displaced;
    {
        Locker locker { native.m_wrapperRepublishLock };
        WeakImpl* current = native.wrapperImplConcurrently();
        ASSERT(current);
        if (current->state() == WeakImpl::Live)
            return;
        displaced = WTF::move(native.m_wrapper);
        native.m_wrapper = WTF::move(weak);
        native.publishReplacementWrapperImpl(native.m_wrapper.unsafeImpl());
    }
    // A concurrent toJS() may still hold the displaced WeakImpl* from a
    // pre-replacement wrapperImplConcurrently() snapshot and inspect its
    // state(), so the impl's memory must outlive any such reader. Readers
    // cannot span a safepoint between the snapshot and the state() check, so
    // deferring the Weak's deallocation past the next all-client safepoint
    // (GCSafepointEpoch) suffices. Flag-off this only delays reclamation of a
    // dead impl slot to the legacy runEndPhase reclaim site — not observable.
    if (displaced) {
        auto* deferred = new Weak<JSArrayBuffer>(WTF::move(displaced));
        Heap::heap(&wrapper)->safepointEpoch().retire(deferred, [](void* pointer) {
            delete static_cast<Weak<JSArrayBuffer>*>(pointer);
        });
    }
}

bool SimpleTypedArrayController::isAtomicsWaitAllowedOnCurrentThread()
{
    return m_allowAtomicsWait;
}

bool SimpleTypedArrayController::JSArrayBufferOwner::isReachableFromOpaqueRoots(JSC::Handle<JSC::Unknown> handle, void*, JSC::AbstractSlotVisitor& visitor, ASCIILiteral* reason)
{
    if (reason) [[unlikely]]
        *reason = "JSArrayBuffer is opaque root"_s;
    auto& wrapper = uncheckedDowncast<JSArrayBuffer>(*handle.slot()->asCell());
    return visitor.containsOpaqueRoot(wrapper.impl());
}

void SimpleTypedArrayController::JSArrayBufferOwner::finalize(JSC::Handle<JSC::Unknown>, void*) { }

} // namespace JSC

