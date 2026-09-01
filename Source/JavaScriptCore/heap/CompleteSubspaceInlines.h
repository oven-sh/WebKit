/*
 * Copyright (C) 2018-2021 Apple Inc. All rights reserved.
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

#pragma once

#include "CompleteSubspace.h"
#include "VM.h"

namespace JSC {

ALWAYS_INLINE void* CompleteSubspace::allocate(VM& vm, size_t cellSize, GCDeferralContext* deferralContext, AllocationFailureMode failureMode)
{
    if constexpr (validateDFGDoesGC)
        vm.verifyCanGC();

    if (Allocator allocator = allocatorFor(cellSize, AllocatorForMode::AllocatorIfExists))
        return allocator.allocate(vm.heap, allocator.cellSize(), deferralContext, failureMode);

    // With the shared-heap option on, the server allocator table consulted
    // above is never populated (allocatorForSlow), so every allocation goes
    // through the calling thread's client TLC. The per-thread {table, bound}
    // snapshot, written only by GCClient::Heap::setCurrentThreadClient,
    // resolves table[tlcIndexBase + sizeClassIndex] without the
    // allocationClientForCurrentThread resolver. The snapshot is keyed by
    // thread, not by VM: it only ever holds a client of the process's sole
    // shared server (the gilOff VM's heap, when a VM is gilOff), and every
    // heap reserves tlcIndexBase from its own per-MarkedSpace counter, so a
    // hit is trusted only when vm itself is gilOff. Any other VM entered on
    // this thread would otherwise pop cells from the stamped heap's blocks. A
    // miss (GIL-on, an unstamped thread, a null slot before first
    // materialization, a large or precise size, or an unreserved base, which
    // is invalidTlcIndex and cannot satisfy the size_t bound check by
    // wraparound) takes the resolver, which carries the access and stamping
    // ASSERTs.
    if (Options::useSharedGCHeap()) [[unlikely]] {
        if (vm.gilOff() && cellSize <= MarkedSpace::largeCutoff) {
            unsigned bound = GCClient::Heap::currentThreadTLCBound();
            size_t slot = static_cast<size_t>(tlcIndexBase()) + MarkedSpace::sizeClassToIndex(cellSize);
            if (slot < bound) {
                if (Allocator allocator = GCClient::Heap::currentThreadTLCTable()[slot]) {
                    // bound > 0 implies a stamped client: setCurrentThreadClient
                    // stores the client and the snapshot together.
                    ASSERT(GCClient::Heap::currentThreadClient());
                    ASSERT(&GCClient::Heap::currentThreadClient()->server() == &vm.heap);
                    ASSERT(GCClient::Heap::currentThreadClient()->hasHeapAccess() || vm.heap.worldIsStoppedForAllClients());
                    return allocator.allocate(vm.heap, allocator.cellSize(), deferralContext, failureMode);
                }
            }
        }
        return allocateSlowForClient(Heap::allocationClientForCurrentThread(vm, vm.clientHeap), cellSize, deferralContext, failureMode);
    }

    return allocateSlow(vm, cellSize, deferralContext, failureMode);
}

ALWAYS_INLINE void* CompleteSubspace::allocateForClient(GCClient::Heap& client, size_t cellSize, GCDeferralContext* deferralContext, AllocationFailureMode failureMode)
{
    // SharedGC (§12.1 seam; T4): per-client TLC routing; no VM-coupled
    // preludes (standalone harness clients have no VM). §5.5 keeps this a
    // C++ FreeList pop: the per-(client, directory) LocalAllocator lives in
    // the caller's GCThreadLocalCache.
    ASSERT(Options::useSharedGCHeap());
    if (cellSize <= MarkedSpace::largeCutoff) {
        if (Allocator allocator = client.threadLocalCache().allocatorForSizeStep(*this, MarkedSpace::sizeClassToIndex(cellSize)))
            return allocator.allocate(client.server(), allocator.cellSize(), deferralContext, failureMode);
    }
    return allocateSlowForClient(client, cellSize, deferralContext, failureMode);
}

} // namespace JSC

