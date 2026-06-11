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
// loop; once ISS doWorkUntil() early-returns (T8/I5b below), so the main-VM
// binding is inert when shared.
IncrementalSweeper::IncrementalSweeper(JSC::Heap* heap)
    : Base(heap->vm())
    , m_currentDirectory(nullptr)
{
}

void IncrementalSweeper::doWorkUntil(VM& vm, MonotonicTime deadline)
{
    // SharedGC (T8/I5b, deviation 4): mutator-concurrent sweeping is disabled
    // once the server is shared. The sweeper runs on one client's run loop
    // while other clients allocate: its block->sweep() path performs
    // lock-free BlockDirectoryBits reads (isDestructible/isEmpty under
    // assertIsMutatorOrMutatorIsStopped) that race another client's
    // addBlock m_bits resize, and its freeBlock() mutates the precise/block
    // registries. Once isSharedServer(), unswept blocks are swept in-lock by
    // allocation slow paths (§5.2, MSPL) or synchronously by the conductor;
    // option off / pre-sticky this is byte-for-byte today's behavior (I10).
    if (vm.heap.isSharedServer()) [[unlikely]]
        return;

    if (!m_currentDirectory)
        m_currentDirectory = vm.heap.objectSpace().firstDirectory();

    if (m_currentDirectory)
        doSweep(vm, deadline, SweepTrigger::OpportunisticTask);
}

void IncrementalSweeper::doWork(VM& vm)
{
    // SharedGC (T8/I5b, deviation 4): see doWorkUntil(). The timer may still
    // be armed from a pre-sticky schedule; cancel it and stand down.
    if (vm.heap.isSharedServer()) [[unlikely]] {
        m_currentDirectory = nullptr;
        cancelTimer();
        return;
    }

    if (m_lastOpportunisticTaskDidFinishSweeping) {
        m_lastOpportunisticTaskDidFinishSweeping = false;
        scheduleTimer();
        return;
    }
    doSweep(vm, MonotonicTime::now() + sweepTimeSlice, SweepTrigger::Timer);
}

void IncrementalSweeper::doSweep(VM& vm, MonotonicTime deadline, SweepTrigger trigger)
{
    ASSERT(!vm.heap.isSharedServer()); // SharedGC (T8): gated in doWork/doWorkUntil.

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

void IncrementalSweeper::startSweeping(JSC::Heap& heap)
{
    ASSERT(!heap.isSharedServer()); // SharedGC (T8): gated in Heap::notifyIncrementalSweeper.
    scheduleTimer();
    m_currentDirectory = heap.objectSpace().firstDirectory();
}

void IncrementalSweeper::stopSweeping()
{
    m_currentDirectory = nullptr;
    cancelTimer();
}

} // namespace JSC
