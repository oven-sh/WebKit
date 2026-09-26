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
        // SharedGC: once the server is shared, a Weak<> is created under
        // MSPL and the heap's weak handle lock (the weak-mutation protocol;
        // see WeakSet::sweep). MSPL keeps a WeakSet without blocks that way
        // while another client's MSPL-held block sweep of the same container
        // is going on (LocalAllocator::tryAllocateIn, the steal path,
        // Heap::sweepSynchronously: they skip weak-bearing blocks). The weak
        // handle lock excludes WeakImpl::clear on any other thread, which
        // pushes onto the free lists popped here and unlinks emptied blocks
        // from the lists walked here. The WeakImpl construction (state ->
        // Live) is inside the section too: a slot taken but not yet Live
        // would read as Deallocated to a sweep. Option off / !ISS: no-op
        // lockers, main's code (I10). Lock-order: callers hold no rank >= 7
        // lock (in-lock block sweeps run destructors, which never CREATE
        // Weaks — they only clear them, which takes the weak handle lock
        // alone). L2 holds: no collection request or stop inside the section
        // (didAllocate is outside; addAllocator's didAllocate(blockSize) only
        // feeds counters/activity timer, the same call
        // registerPreciseAllocation already makes under MSPL).
        MutatorSlowPathLocker mutatorSlowPathLocker(heap);
        std::optional<Locker<Lock>> weakHandleLocker;
        if (heap.isSharedServer()) [[unlikely]]
            weakHandleLocker.emplace(heap.weakHandleLock());
        WeakSet& weakSet = container.weakSet();
        WeakBlock* block = weakSet.m_currentBlock;
        if (!block || !block->hasFreeCell()) [[unlikely]]
            block = weakSet.findAllocator(container);
        weakImpl = new (NotNull, WeakBlock::asWeakImpl(block->takeFreeCell())) WeakImpl(jsValue, weakHandleOwner, context);
    }
    heap.didAllocate(sizeof(WeakImpl));
    return weakImpl;
}

inline void WeakBlock::finalize(WeakImpl* weakImpl)
{
    ASSERT(weakImpl->state() == WeakImpl::Dead);
    weakImpl->setState(WeakImpl::Finalized);
    ASSERT(m_deadCount);
    --m_deadCount;
    WeakHandleOwner* weakHandleOwner = weakImpl->weakHandleOwner();
    if (!weakHandleOwner)
        return;
    weakHandleOwner->finalize(Handle<Unknown>::wrapSlot(&const_cast<JSValue&>(weakImpl->jsValue())), weakImpl->context());
}

} // namespace JSC
