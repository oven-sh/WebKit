/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#include "Allocator.h"
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC {

class BlockDirectory;
class CompleteSubspace;
class Heap;
class LocalAllocator;

namespace GCClient {

// Per-client (per-thread, post-GIL) allocator cache over the shared server
// BlockDirectories (SPEC-heap.md §5.3; design template: libpas
// pas_thread_local_cache). The flat m_table is indexed by
// BlockDirectory::m_tlcIndex (non-iso) plus the stamped static iso slots; the
// remaining iso allocators enter m_perDirectory only. Under
// Options::useSharedGCHeap() the table is allocated at its lifetime capacity
// in the ctor and never reallocated, so pointers into it (the per-thread
// {table, bound} snapshot, the lite mirror, per-client slot bases) stay valid
// for the client's lifetime. Layout and indexing are FROZEN; generated code
// never addresses this object directly — it reads the {table, bound} mirror
// stamped on the owning VMLite.
class GCThreadLocalCache {
    WTF_MAKE_NONCOPYABLE(GCThreadLocalCache);
public:
    explicit GCThreadLocalCache(JSC::Heap& server);
    ~GCThreadLocalCache(); // §5.3 teardown: runs stopAllocatingForGood() (idempotent), then destroys owned allocators.

    // Fast path: bounds check + indexed load (slot = tlcIndexBase +
    // sizeClassIndex); a null slot materializes this client's LocalAllocator
    // (dedup via m_perDirectory; I3), creating the directory first
    // (directoryLock only). Owner thread only (I2). Null only when the size
    // step has no size class (callers take the precise path).
    Allocator allocatorForSizeStep(CompleteSubspace&, size_t sizeClassIndex);

    // §5.3 (T4): GCClient::IsoSubspace LocalAllocators enter m_perDirectory
    // at materialization, lookup-only — NOT owned (their IsoSubspace owns
    // them by value); a static iso subspace with a stamped tlcSlot is also
    // published into m_table at that slot. Covers iso for the §10A.1 ownership
    // predicate and for the stop/teardown loops below. Called from
    // GCClient::Heap::registerIsoSubspaceLocalAllocators() and the dynamic
    // iso-subspace Slow paths (owner thread only, pre-publication).
    void registerExternalAllocator(LocalAllocator*);

    // §10A.1 ownership predicate: true iff this cache holds the given
    // LocalAllocator — owned non-iso allocators and the registered
    // GCClient::IsoSubspace allocators (lookup-only entries, §5.3).
    bool ownsLocalAllocator(const LocalAllocator*) const;

    // Conductor-side (world-stopped, I2 exception) or owner-thread entry
    // point: resumes every allocator of this client (owned non-iso +
    // registered iso) after a stop. The matching stop-side flush runs through
    // MarkedSpace::stopAllocating over each directory's m_localAllocators
    // list, which already covers every client's allocators.
    // LocalAllocator's assertSharedAllocatorMutationIsSafe checks the I5b
    // conditions per slot.
    void resumeAllocating();
    // SharedGC Wlr T2: visit every LocalAllocator this cache holds (owned
    // non-iso AND registered iso — m_perDirectory is the I3-authoritative
    // owner set). Conductor-side, world stopped for all clients (I2
    // exception): the map is owner-thread-mutated outside the stop window.
    template<typename Functor>
    void forEachLocalAllocator(const Functor& functor)
    {
        for (LocalAllocator* allocator : m_perDirectory.values())
            functor(allocator);
    }
    // §5.3 teardown (I9), world running: per-slot stopAllocatingForGood()
    // under MSPL (directory-bit flips are I5b writes), then unlink each
    // allocator under its directory's m_localAllocatorsLock (rank 7 -> 8).
    // Idempotent; also runs from the dtor for stragglers.
    void stopAllocatingForGood();

    // Read-only view of the pair the §10A.1 client-slot stamp site
    // (setCurrentThreadClient) publishes to the TLS snapshot and the lite
    // mirror (VMLite::tlcTable/tlcTableBound), which is what the inline-
    // allocate emitters index: slot = tlcIndexBase + sizeClassIndex;
    // slot < bound ? table[slot] : null. Owner thread only (I2).
    Allocator* table() const { return m_table; }
    unsigned tableBound() const { return m_tableBound; }

private:
    Allocator materializeAllocator(BlockDirectory&); // slow path; I3 dedup.

    JSC::Heap& m_server;
    Allocator* m_table { nullptr }; // flat; LocalAllocator* or null. Allocated once, in the ctor.
    unsigned m_tableBound { 0 };
    Vector<std::unique_ptr<LocalAllocator>> m_ownedAllocators;
    HashMap<BlockDirectory*, LocalAllocator*> m_perDirectory; // cold; I3
};

} // namespace GCClient

#if ASSERT_ENABLED
// Debug-only per-thread JSCellLock hold depth, maintained by the
// JSCellLock::lock/tryLock/unlock inlines (runtime/JSCellInlines.h). A
// JSCellLock holder must not release heap access, pass a stop poll, or
// conduct a stop: a holder parked inside a stop window would keep the
// concurrent marker's tryLock-and-revisit from ever succeeding on that cell,
// so Heap.cpp asserts depth == 0 at stopIfNecessaryForAllClients entry and
// on every acquireHeapAccess park leg, gated on
// Options::useConcurrentSharedGCMarking(). Release builds compile all of
// this away.
//
// Lives here rather than Heap.h so JSCellInlines.h reaches it through
// HeapInlines.h -> Heap.h.
class GCCellLockDepth {
public:
    static void increment() { ++t_depth; }
    static void decrement()
    {
        ASSERT(t_depth);
        --t_depth;
    }
    static unsigned current() { return t_depth; }

private:
    static inline thread_local unsigned t_depth { 0 };
};
#endif // ASSERT_ENABLED

} // namespace JSC
