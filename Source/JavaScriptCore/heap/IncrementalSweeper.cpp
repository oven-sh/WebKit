/*
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
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
#include "IncrementalSweeper.h"

#include "BlockDirectoryInlines.h"
#include "DeferGCInlines.h"
#include "HeapInlines.h"
#include "MarkedBlockInlines.h"
#include <wtf/SystemTracing.h>

namespace JSC {

static constexpr Seconds sweepTimeSlice = 10_ms;
static constexpr double sweepTimeTotal = .10;
static constexpr double sweepTimeMultiplier = 1.0 / sweepTimeTotal;

void IncrementalSweeper::scheduleTimer()
{
    setTimeUntilFire(sweepTimeSlice * sweepTimeMultiplier);
}

// The sweeper is a timer on the main VM's run loop. Once the server is shared
// it keeps running there, mutator-concurrently, and every step takes the
// shared path (sweepNextBlockShared).
IncrementalSweeper::IncrementalSweeper(JSC::Heap* heap)
    : Base(heap->vm())
    , m_currentDirectory(nullptr)
{
}

void IncrementalSweeper::doWorkUntil(VM& vm, MonotonicTime deadline)
{
    if (!m_currentDirectory) {
        m_currentDirectory = vm.heap.objectSpace().firstDirectory();
        m_sharedUnsweptCursor = 0;
    }

    if (m_currentDirectory)
        doSweep(vm, deadline, SweepTrigger::OpportunisticTask);
}

void IncrementalSweeper::doWork(VM& vm)
{
    if (m_lastOpportunisticTaskDidFinishSweeping) {
        m_lastOpportunisticTaskDidFinishSweeping = false;
        scheduleTimer();
        return;
    }
    doSweep(vm, MonotonicTime::now() + sweepTimeSlice, SweepTrigger::Timer);
}

void IncrementalSweeper::doSweep(VM& vm, MonotonicTime deadline, SweepTrigger trigger)
{
    std::optional<TraceScope> traceScope;
    if (Options::useTracePoints()) [[unlikely]]
        traceScope.emplace(IncrementalSweepStart, IncrementalSweepEnd, vm.heap.size(), vm.heap.capacity());

    while (sweepNextBlock(vm, trigger)) {
        if (MonotonicTime::now() < deadline)
            continue;

        if (trigger == SweepTrigger::Timer)
            scheduleTimer();
        else
            m_lastOpportunisticTaskDidFinishSweeping = false;
        return;
    }
    if (trigger == SweepTrigger::OpportunisticTask)
        m_lastOpportunisticTaskDidFinishSweeping = true;

    cancelTimer();
}

bool IncrementalSweeper::sweepNextBlock(VM& vm, SweepTrigger trigger)
{
    vm.heap.stopIfNecessary();

    // Checked per block, after the stop poll and before any directory state
    // is read: the server can become shared between two blocks of one
    // doSweep, and the next step must then already run under MSPL.
    if (vm.heap.isSharedServer()) [[unlikely]]
        return sweepNextBlockShared(vm);

    MarkedBlock::Handle* block = nullptr;

    for (; m_currentDirectory; m_currentDirectory = m_currentDirectory->nextDirectory()) {
        block = m_currentDirectory->findBlockToSweep();
        if (block)
            break;
    }
    
    if (block) {
        DeferGCForAWhile deferGC(vm);
        block->sweep(nullptr);

        bool blockIsFreed = false;
        if (trigger == SweepTrigger::Timer) {
            if (!block->isEmpty())
                block->shrink();
            else {
                vm.heap.objectSpace().freeBlock(block);
                blockIsFreed = true;
            }
        }

        if (!blockIsFreed)
            m_currentDirectory->didFinishUsingBlock(block);
        return true;
    }

    return vm.heap.sweepNextLogicallyEmptyWeakBlock();
}

bool IncrementalSweeper::sweepNextBlockShared(VM& vm)
{
    // Shared-server sweep step, run mutator-concurrently on the main VM's run
    // loop while other clients allocate. Invariants:
    //  - The exclusive MSPL is held for the whole step, which serializes the
    //    directory-bit reads and the block sweep against every client's
    //    allocation slow path (addBlock's m_bits resize, in-lock sweeps). The
    //    step holds heap access and never parks, so no stop window opens
    //    mid-step; the caller's stopIfNecessary() poll is the park point.
    //  - No freeBlock and no block->shrink(): physical reclamation is
    //    world-stopped-only when shared (Heap::reclaimSharedGCMemoryAtCycleEnd).
    //    Blocks swept empty stay on the directories' empty lists, reusable by
    //    every client's allocator.
    //  - Weak-bearing blocks are skipped, like the carve-out at the other
    //    MSPL sweep sites (WeakSet::head() is stable under MSPL); the
    //    sweeper-owned cursor steps past them so the scan cannot livelock,
    //    and they wait for the next world-stopped sweep.
    MutatorSlowPathLocker mutatorSlowPathLocker(vm.heap);

    while (m_currentDirectory) {
        MarkedBlock::Handle* block = m_currentDirectory->findBlockToSweep(m_sharedUnsweptCursor);
        if (!block) {
            m_currentDirectory = m_currentDirectory->nextDirectory();
            m_sharedUnsweptCursor = 0;
            continue;
        }

        if (block->weakSet().head()) [[unlikely]] {
            // Weak-bearing: leave unswept, release the inUse bit
            // findBlockToSweep took, and step the cursor past it.
            m_currentDirectory->didFinishUsingBlock(block);
            m_sharedUnsweptCursor++;
            return true;
        }

        DeferGCForAWhile deferGC(vm);
        block->sweep(nullptr);
        m_currentDirectory->didFinishUsingBlock(block);
        return true;
    }

    // MSPL is still held, which is the shared-mode precondition of
    // Heap::sweepNextLogicallyEmptyWeakBlock.
    return vm.heap.sweepNextLogicallyEmptyWeakBlock();
}

void IncrementalSweeper::startSweeping(JSC::Heap& heap)
{
    // Also called by the shared conductor inside the stop window
    // (Heap::notifyIncrementalSweeper) while the owning run-loop thread is
    // parked, so these plain writes are published by the resume edge;
    // setTimeUntilFire locks internally.
    scheduleTimer();
    m_currentDirectory = heap.objectSpace().firstDirectory();
    m_sharedUnsweptCursor = 0;
}

void IncrementalSweeper::stopSweeping()
{
    m_currentDirectory = nullptr;
    cancelTimer();
}

} // namespace JSC
