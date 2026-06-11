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

    // SharedGC (§5.3; T4): with the option on, route through the calling
    // thread's client TLC (the server allocator table is never populated,
    // §5.5). SPEC-ungil §B / I4: GIL-off, the CURRENT thread's client — not
    // unconditionally vm.clientHeap — owns the LocalAllocators this thread
    // may pop from (Heap::allocationClientForCurrentThread is identity
    // GIL-on/flag-off). Option off: today's code (I10; the branch is the
    // only delta).
    if (Options::useSharedGCHeap()) [[unlikely]]
        return allocateForClient(Heap::allocationClientForCurrentThread(vm, vm.clientHeap), cellSize, deferralContext, failureMode);

    if (Allocator allocator = allocatorFor(cellSize, AllocatorForMode::AllocatorIfExists))
        return allocator.allocate(vm.heap, allocator.cellSize(), deferralContext, failureMode);
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

