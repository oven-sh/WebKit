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

// SharedGC (T9): main-VM-only — the sweeper is a timer on the main VM's run
// loop. T4(d): once ISS the sweeper keeps running on that run loop (main
// client's thread) in the restricted shared mode — see doWorkUntil().
IncrementalSweeper::IncrementalSweeper(JSC::Heap* heap)
    : Base(heap->vm())
    , m_currentDirectory(nullptr)
{
}

void IncrementalSweeper::doWorkUntil(VM& vm, ApproximateTime deadline)
{
    // T4(d): RE-ENABLED when shared (was the T8/I5b deviation-4 stand-down,
    // which — with the weak-bearing carve-out leaving only in-lock and
    // conductor-side sweeps — made committed capacity monotone once ISS).
    // The original objections are answered structurally, not by disabling:
    //  - "block->sweep()'s lock-free BlockDirectoryBits reads race another
    //    client's addBlock m_bits resize": the shared path holds the server
    //    MSPL across each per-block operation (sweepNextBlockShared) — the
    //    same §5.2 in-lock license every allocation slow path's sweep uses;
    //    addBlock runs under MSPL, so the resize is serialized out.
    //  - "freeBlock() mutates the precise/block registries": the shared path
    //    NEVER frees or shrinks a block (MC-SAFE S4: physical reclamation is
    //    world-stopped-only — Heap::reclaimSharedGCMemoryAtCycleEnd). Empty
    //    sweep results stay on the directories' empty lists, reusable by
    //    every client's allocator; the OS-return happens at the next
    //    conducted cycle's world-stopped shrink.
    //  - weak-bearing blocks are skipped, mirroring BlockDirectory::sweep's
    //    mutator-concurrent carve-out (WeakSet::head() is stable under MSPL).
    // Option off / pre-sticky: byte-for-byte today's behavior (I10) — the
    // shared branch in sweepNextBlock is reached only once ISS.
    if (!m_currentDirectory) {
        m_currentDirectory = vm.heap.objectSpace().firstDirectory();
        m_sharedUnsweptCursor = 0;
    }

    if (m_currentDirectory)
        doSweep(vm, deadline, SweepTrigger::OpportunisticTask);
}

void IncrementalSweeper::doWork(VM& vm)
{
    // T4(d): shared mode no longer stands down — see doWorkUntil().
    if (m_lastOpportunisticTaskDidFinishSweeping) {
        m_lastOpportunisticTaskDidFinishSweeping = false;
        scheduleTimer();
        return;
    }
    doSweep(vm, ApproximateTime::now() + sweepTimeSlice, SweepTrigger::Timer);
}

void IncrementalSweeper::doSweep(VM& vm, ApproximateTime deadline, SweepTrigger trigger)
{
    std::optional<TraceScope> traceScope;
    if (Options::useTracePoints()) [[unlikely]]
        traceScope.emplace(IncrementalSweepStart, IncrementalSweepEnd, vm.heap.size(), vm.heap.capacity());

    vm.heap.clearConcurrentRetainedDataIfPossible();

    while (sweepNextBlock(vm, trigger)) {
        if (ApproximateTime::now() < deadline)
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

    // T4(d): the shared server takes the restricted path. Checked per block
    // (after the stop poll, before any directory state is touched) so a
    // mid-doSweep ISS flip — the §10B.4 sticky switch can land while this
    // run-loop callback is between blocks — moves us onto the MSPL'd path
    // before the next directory read.
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
    // T4(d) shared-server sweep step. Invariants (see doWorkUntil's banner):
    //  - MSPL held for the whole step: serializes the directory-bit reads
    //    and the block sweep against every other client's allocation slow
    //    path (addBlock resize, in-lock sweeps) — the §5.2 license. MSPL
    //    sections run with heap access held and never park, so the §10.4
    //    barrier excludes a stop window from opening mid-step (the caller's
    //    stopIfNecessary() poll above the dispatch is the park point).
    //  - NO freeBlock, NO block->shrink(): physical reclamation is
    //    world-stopped-only when shared (MC-SAFE S4); empty results stay on
    //    the empty lists for Heap::reclaimSharedGCMemoryAtCycleEnd.
    //  - Weak-bearing blocks are skipped (same predicate as
    //    BlockDirectory::sweep's carve-out: head() is stable under MSPL);
    //    the sweeper-owned cursor advances past them so the scan cannot
    //    livelock on a skipped block. They stay unswept for the next
    //    world-stopped sweep — unchanged from the pre-T4 steady state.
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

    // MSPL is held, satisfying the shared-mode precondition asserted in
    // Heap::sweepNextLogicallyEmptyWeakBlock (logically-empty WeakBlock
    // destruction under MSPL is the already-licensed drain path —
    // sweepAllLogicallyEmptyWeakBlocks does exactly this).
    return vm.heap.sweepNextLogicallyEmptyWeakBlock();
}

void IncrementalSweeper::startSweeping(JSC::Heap& heap)
{
    // T4(d): also called by the shared conductor inside the stop window
    // (Heap::notifyIncrementalSweeper) — the owning run-loop thread is
    // parked there, so these plain writes are ordered by the stop protocol's
    // resume edge; setTimeUntilFire locks internally.
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
