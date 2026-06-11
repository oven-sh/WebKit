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
// BlockDirectory::m_tlcIndex (non-iso); iso allocators enter m_perDirectory
// only. Layout and indexing are FROZEN; the JIT addressing contract
// (offsetOfTable()/offsetOfTableBound() and the vm-relative chain) is
// PROVISIONAL (deviation 6) — offsets exported, layout-stable.
class GCThreadLocalCache {
    WTF_MAKE_NONCOPYABLE(GCThreadLocalCache);
public:
    explicit GCThreadLocalCache(JSC::Heap& server);
    ~GCThreadLocalCache(); // §5.3 teardown: runs stopAllocatingForGood() (idempotent), then destroys owned allocators.

    // Fast path: bounds check + indexed load; null slot => slow path which
    // materializes a LocalAllocator (dedup via m_perDirectory; I3). Owner
    // thread only (I2); for iso directories (tlcIndex == invalid) this is a
    // lookup-only m_perDirectory query (§5.3).
    Allocator allocatorFor(BlockDirectory&); // by directory->tlcIndex()
    // "Ensure" semantics: creates the directory (directoryLock only) and this
    // client's LocalAllocator on first use; null only when the size step has
    // no size class (callers take the precise path).
    Allocator allocatorForSizeStep(CompleteSubspace&, size_t sizeClassIndex);

    // §5.3 (T4): GCClient::IsoSubspace LocalAllocators enter m_perDirectory
    // at materialization, lookup-only — NOT owned (their IsoSubspace owns
    // them by value), never in m_table. Covers iso for the §10A.1 ownership
    // predicate and for the stop/teardown loops below. Called from
    // GCClient::Heap::registerIsoSubspaceLocalAllocators() and the dynamic
    // iso-subspace Slow paths (owner thread only, pre-publication).
    void registerExternalAllocator(LocalAllocator*);

    // §10A.1 ownership predicate: true iff this cache holds the given
    // LocalAllocator — owned non-iso allocators and the registered
    // GCClient::IsoSubspace allocators (lookup-only entries, §5.3).
    bool ownsLocalAllocator(const LocalAllocator*) const;

    // Conductor-side (world-stopped, I2 exception) or owner-thread entry
    // points; mirror MarkedSpace's per-allocator stop/resume/prepare over
    // every allocator of this client (owned non-iso + registered iso).
    // LocalAllocator's assertSharedAllocatorMutationIsSafe checks the I5b
    // conditions per slot.
    void stopAllocating();
    void resumeAllocating();
    void prepareForAllocation();
    // §5.3 teardown (I9), world running: per-slot stopAllocatingForGood()
    // under MSPL (directory-bit flips are I5b writes), then unlink each
    // allocator under its directory's m_localAllocatorsLock (rank 7 -> 8).
    // Idempotent; also runs from the dtor for stragglers.
    void stopAllocatingForGood();

    JSC::Heap& server() { return m_server; }

    // PROVISIONAL (SPEC-heap.md §5.3 Status): slot = tlcIndexBase + sizeClassIndex;
    //   slot < *offsetOfTableBound() ? table[slot] : null
    static constexpr ptrdiff_t offsetOfTable() { return OBJECT_OFFSETOF(GCThreadLocalCache, m_table); }
    static constexpr ptrdiff_t offsetOfTableBound() { return OBJECT_OFFSETOF(GCThreadLocalCache, m_tableBound); }

private:
    Allocator materializeAllocator(BlockDirectory&); // slow path; I3 dedup.
    void growTable(unsigned neededBound); // grow-only (§5.3); owner thread.

    JSC::Heap& m_server;
    Allocator* m_table { nullptr }; // flat; LocalAllocator* or null
    unsigned m_tableBound { 0 }; // grow-only
    Vector<std::unique_ptr<LocalAllocator>> m_ownedAllocators;
    HashMap<BlockDirectory*, LocalAllocator*> m_perDirectory; // cold; I3
};

} // namespace GCClient
} // namespace JSC
