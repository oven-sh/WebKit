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
    
    JSC::Heap& heap = *this->heap();
    while (WeakBlock* block = m_blocks.removeHead())
        WeakBlock::destroy(heap, block);
    ASSERT(m_blocks.isEmpty());
}

void WeakSet::lastChanceToFinalize()
{
    forEachBlock([](WeakBlock& block) {
        block.lastChanceToFinalize();
    });
}

void WeakSet::sweep()
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

void WeakSet::shrink()
{
    // SharedGC (review round 4): weak-mutation protocol — see sweep(),
    // including the §A.3 conductor disjunct (AB-10).
    ASSERT(!heap()->isSharedServer() || heap()->worldIsStoppedForAllClients() || heap()->mutatorSlowPathLock().isHeld() || (jsThreadsThreadGranularWorldIsStopped() && jsThreadsCurrentThreadIsStopConductor()));

    WeakBlock* next;
    for (WeakBlock* block = m_blocks.head(); block; block = next) {
        next = block->next();

        if (block->isEmpty())
            removeAllocator(block);
    }

    resetAllocator();
    
    if (m_blocks.isEmpty() && isOnList())
        remove();
}

WeakBlock::FreeCell* WeakSet::findAllocator(CellContainer container)
{
    if (WeakBlock::FreeCell* allocator = tryFindAllocator())
        return allocator;

    return addAllocator(container);
}

WeakBlock::FreeCell* WeakSet::tryFindAllocator()
{
    while (m_nextAllocator) {
        WeakBlock* block = m_nextAllocator;
        m_nextAllocator = m_nextAllocator->next();

        WeakBlock::SweepResult sweepResult = block->takeSweepResult();
        if (sweepResult.freeList)
            return sweepResult.freeList;
    }

    return nullptr;
}

WeakBlock::FreeCell* WeakSet::addAllocator(CellContainer container)
{
    if (!isOnList())
        heap()->objectSpace().addActiveWeakSet(this);
    
    WeakBlock* block = WeakBlock::create(*heap(), container);
    heap()->didAllocate(WeakBlock::blockSize);
    m_blocks.append(block);
    WeakBlock::SweepResult sweepResult = block->takeSweepResult();
    ASSERT(!sweepResult.isNull() && sweepResult.freeList);
    return sweepResult.freeList;
}

void WeakSet::removeAllocator(WeakBlock* block)
{
    m_blocks.remove(block);
    WeakBlock::destroy(*heap(), block);
}

} // namespace JSC
