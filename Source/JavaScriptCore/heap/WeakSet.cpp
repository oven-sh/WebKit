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

#include "config.h"
#include "WeakSet.h"

#include "Heap.h"
#include "VM.h"

namespace JSC {

// UNGIL §A.3 (AB-10) cross-TU seams — defined in runtime/VMManager.cpp;
// declaration pattern matches heap/Heap.cpp:151, heap/LocalAllocator.cpp:45,
// heap/BlockDirectory.cpp:45 and heap/MarkedSpace.cpp. Signatures must stay
// byte-identical.
bool jsThreadsThreadGranularWorldIsStopped(); // §A.3.2 post-quiescence depth.
bool jsThreadsCurrentThreadIsStopConductor(); // §A.3.3 tenure check.

WeakSet::~WeakSet()
{
    if (isOnList())
        remove();

    // Sweeping a container hands back every block in its WeakSet, and a container is always swept
    // before it is freed - at VM teardown through lastChanceToFinalize as much as in a collection.
    // Destroying a block here instead would dangle the Weak<>s still pointing into it.
    RELEASE_ASSERT(m_blocks.isEmpty());
}

void WeakSet::lastChanceToFinalize()
{
    for (WeakBlock* block = m_blocks.head(); block; block = block->next()) {
        WeakBlock::IterationScope iterationScope(*block);
        block->lastChanceToFinalize();
    }
}

void WeakSet::reap()
{
    forEachBlock([](WeakBlock& block) {
        block.reap();
    });
}

void WeakSet::didBecomeEmpty(WeakBlock* block)
{
    // SharedGC (review round 4) — the weak-mutation protocol: once the
    // server is shared, every WeakSet mutation (this sweep, shrink,
    // resetAllocator, and WeakSet::allocate's freelist/m_blocks writes)
    // runs under MSPL or while the world is stopped for all clients.
    // Contexts: conducted-collection sweeps and reap/visit are
    // world-stopped (deviation 4); mutator-concurrent block sweeps hold
    // MSPL (LocalAllocator::allocateSlowCase, Heap::sweepSynchronously) —
    // and additionally SKIP blocks whose WeakSet has any WeakBlocks (the
    // weak-bearing carve-out at LocalAllocator::tryAllocateIn, the steal
    // path, and BlockDirectory::sweep), because MSPL alone does not exclude
    // the lock-free WeakSet::deallocate or the finalizer-vs-Weak-owner
    // lifetime race; teardown (lastChanceToFinalize) holds MSPL with no
    // other mutator left. So a mutator-concurrent arrival here only ever
    // sees an empty m_blocks list.
    // UNGIL §K.5 class-4 (AB-10): a §A.3 thread-granular window's CONDUCTOR
    // is also licensed — every other entered mutator is parked at a poll
    // site (so the lock-free WeakSet::deallocate cannot be in flight) and
    // the window's GCL bracket excludes any shared GC (so no concurrent
    // finalizer). Reached from the conductor's in-window allocation slow
    // path (the class-4 allocating body, ANNEX HBT2.1).
    ASSERT(!heap()->isSharedServer() || heap()->worldIsStoppedForAllClients() || heap()->mutatorSlowPathLock().isHeld() || (jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor()));

    for (WeakBlock* block = m_blocks.head(); block;) {
        heap()->sweepNextLogicallyEmptyWeakBlock();

        WeakBlock* nextBlock = block->next();
        block->sweep();
        if (block->isLogicallyEmptyButNotFree()) {
            // If this WeakBlock is logically empty, but still has Weaks pointing into it,
            // we can't destroy it just yet. Detach it from the WeakSet and hand ownership
            // to the Heap so we don't pin down the entire MarkedBlock or PreciseAllocation.
            m_blocks.remove(block);
            heap()->addLogicallyEmptyWeakBlock(block);
            block->disconnectContainer();
        }
        block = nextBlock;
    }

    resetAllocator();
}

void WeakSet::tryReleaseBlock(WeakBlock* block)
{
    // SharedGC (review round 4): weak-mutation protocol — see sweep(),
    // including the §A.3 conductor disjunct (AB-10).
    ASSERT(!heap()->isSharedServer() || heap()->worldIsStoppedForAllClients() || heap()->mutatorSlowPathLock().isHeld() || (jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor()));

    WeakBlock* next;
    for (WeakBlock* block = m_blocks.head(); block; block = next) {
        next = block->next();

    bool isEmpty = block->isEmpty();
    if (!isEmpty && !block->hasOnlyFinalizedHandles())
        return;

    if (block == m_nextAllocator)
        m_nextAllocator = block->next();

    m_blocks.remove(block);
    if (isEmpty)
        heap()->returnWeakBlockToPool(block);
    else
        heap()->addDetachedWeakBlock(block);

    if (m_blocks.isEmpty() && isOnList())
        remove();
}

void WeakSet::sweep()
{
    // WeakBlock::sweep calls finalizer and it can allocate/deallocate WeakImpls. This means,
    //
    // 1. New WeakBlock can be allocated and chained to m_blocks during iteration.
    // 2. WeakBlocks get empty and removed from m_blocks.
    //
    // So our approach is,
    //
    // 1. We are iterating doubly-linked list, so it is fine when a new WeakBlock is appended to m_blocks.
    // 2. During sweeping, we mark WeakBlock via WeakBlock::IterationScope. This defers empty / logically-empty chaining.
    //    Thus we do not unchain the currently swept block from m_blocks.
    // 3. Once the scope is closed the block is no longer held, so it can be released right away. Its
    //    successor has to be read first, because releasing pools the block and may free it outright.
    WeakBlock* next;
    for (WeakBlock* block = m_blocks.head(); block; block = next) {
        {
            WeakBlock::IterationScope iterationScope(*block);
            block->sweep();
        }
        next = block->next();
        tryReleaseBlock(block);
    }

    detachAllocator();

    // A finalizer above can clear the last live handle in a block the first walk already released
    // its hold on. Nothing revisits a block when its live count reaches zero, so classify whatever
    // survived now that every finalizer has run.
    for (WeakBlock* block = m_blocks.head(); block; block = next) {
        next = block->next();
        tryReleaseBlock(block);
    }

    resetAllocator();

    if (m_blocks.isEmpty() && isOnList())
        remove();
}

void WeakSet::shrink()
{
    detachAllocator();

    WeakBlock* next;
    for (WeakBlock* block = m_blocks.head(); block; block = next) {
        next = block->next();

        if (block->isEmpty()) {
            m_blocks.remove(block);
            heap()->returnWeakBlockToPool(block);
        }
    }

    resetAllocator();

    if (m_blocks.isEmpty() && isOnList())
        remove();
}

WeakBlock* WeakSet::findAllocator(CellContainer container)
{
    if (WeakBlock* block = tryFindAllocator())
        return block;

    return addAllocator(container);
}

WeakBlock* WeakSet::tryFindAllocator()
{
    while (m_nextAllocator) {
        WeakBlock* block = m_nextAllocator;
        m_nextAllocator = m_nextAllocator->next();

        if (block->hasFreeCell())
            return m_currentBlock = block;
    }

    return nullptr;
}

WeakBlock* WeakSet::addAllocator(CellContainer container)
{
    if (!isOnList())
        heap()->objectSpace().addActiveWeakSet(this);

    WeakBlock* block = heap()->takeWeakBlockFromPool();
    if (block)
        block->reattach(container);
    else {
        block = WeakBlock::create(*heap(), container);
        // Only a block that is new memory counts against the eden trigger. One taken from the
        // pool adds no footprint.
        heap()->didAllocate(WeakBlock::blockSize);
    }

    m_blocks.append(block);
    ASSERT(block->hasFreeCell());
    return m_currentBlock = block;
}

} // namespace JSC
