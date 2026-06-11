/*
 * Copyright (C) 2012, 2016 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "CellContainerInlines.h"
#include "Heap.h"
#include "MarkedBlock.h"
#include <JavaScriptCore/JSCell.h>

namespace JSC {

inline WeakImpl* WeakSet::allocate(JSValue jsValue, WeakHandleOwner* weakHandleOwner, void* context)
{
    CellContainer container = jsValue.asCell()->cellContainer();
    // SharedGC (T9): main-VM-only assert — container.vm() is the main VM
    // (server-owned container) and the predicate names its API lock.
    // GIL-phase sound (JSLock migration, I2); standalone (§12.1) clients
    // never allocate Weaks. Post-GIL this becomes an access-held predicate
    // (currentThreadClient()). The didAllocate() below feeds the relaxed
    // atomic counters (§5.4/F3) — any-client OK.
    ASSERT(container.vm().currentThreadIsHoldingAPILock());
    JSC::Heap& heap = container.vm().heap;
    WeakImpl* weakImpl;
    {
        // SharedGC (review round 4): once the server is shared, ALL WeakSet
        // mutation runs under MSPL or world-stopped (the weak-mutation
        // protocol; asserted in WeakSet::sweep/shrink). Without this lock,
        // the freelist pop / findAllocator walk / m_blocks append below race
        // another client's MSPL-held in-lock block sweep of the same
        // container's WeakSet (LocalAllocator::tryAllocateIn, the steal
        // path, Heap::sweepSynchronously), which rewrites the very sweep
        // results m_allocator points into and re-frees a popped-but-not-yet-
        // constructed cell (state still Deallocated) — lost/aliased
        // WeakImpls. The WeakImpl construction (state -> Live) must also be
        // inside the section for that reason. Option off / !ISS: no-op
        // locker, today's code (I10). Lock-order: callers hold no rank >= 7
        // lock (in-lock block sweeps run destructors, which never CREATE
        // Weaks — they only deallocate, which is lock-free; see
        // WeakSet::deallocate). L2 holds: no collection request or stop
        // inside the section (didAllocate is outside; addAllocator's
        // didAllocate(blockSize) only feeds counters/activity timer, the
        // same call registerPreciseAllocation already makes under MSPL).
        MutatorSlowPathLocker mutatorSlowPathLocker(heap);
        WeakSet& weakSet = container.weakSet();
        WeakBlock::FreeCell* allocator = weakSet.m_allocator;
        if (!allocator) [[unlikely]]
            allocator = weakSet.findAllocator(container);
        weakSet.m_allocator = allocator->next;
        weakImpl = new (NotNull, WeakBlock::asWeakImpl(allocator)) WeakImpl(jsValue, weakHandleOwner, context);
    }
    heap.didAllocate(sizeof(WeakImpl));
    return weakImpl;
}

inline void WeakBlock::finalize(WeakImpl* weakImpl)
{
    ASSERT(weakImpl->state() == WeakImpl::Dead);
    weakImpl->setState(WeakImpl::Finalized);
    WeakHandleOwner* weakHandleOwner = weakImpl->weakHandleOwner();
    if (!weakHandleOwner)
        return;
    weakHandleOwner->finalize(Handle<Unknown>::wrapSlot(&const_cast<JSValue&>(weakImpl->jsValue())), weakImpl->context());
}

inline void WeakSet::reap()
{
    forEachBlock([](WeakBlock& block) {
        block.reap();
    });
}

} // namespace JSC
