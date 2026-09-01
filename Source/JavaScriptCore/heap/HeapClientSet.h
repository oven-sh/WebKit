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

#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/SentinelLinkedList.h>

namespace JSC {

class Heap;

namespace GCClient {
class Heap;
}

// The registry of GCClient::Heaps attached to one server JSC::Heap
// (SPEC-heap.md §5.1). The set is consulted by the shared-mode stop protocol
// (§10), the conductor's per-client cache flush/resume, and the epoch
// reclamation minimum (§11).
//
// Locking: HeapClientSet::m_lock is rank 6 in the master lock order
// (SPEC-heap.md §6). forEach may be called either under m_lock (which it takes
// itself) or by the conductor while the world is stopped for all clients.
class HeapClientSet {
    WTF_MAKE_NONCOPYABLE(HeapClientSet);
public:
    HeapClientSet() = default;
    ~HeapClientSet();

    // Registers a client (called from the GCClient::Heap ctor). When this
    // makes size() > 1 with Options::useSharedGCHeap() set, the server first
    // becomes a sticky shared server (ISS, I13): there may be only one such
    // server per process (RELEASE_ASSERTed), and the switch runs the §10B.4
    // attach-quiescence loop (timed waits on the GC election condition under
    // *m_threadLock until the legacy protocol is ticket-quiescent AND no
    // foreign legacy mutator can be mid-heap-operation) before the client is
    // inserted. Like remove(), an add() on an already-shared server cannot
    // complete between stop and resume (I13 add-side): the insert runs under
    // the server's GC barrier lock with !worldIsStoppedForAllClients(), so
    // the registry is frozen inside a stop window on both the add and the
    // remove side (relied on by GCSafepointEpoch::bumpAndReclaim and the §10
    // step-8 resume pass). May block.
    void add(GCClient::Heap&);

    // Unregisters a client (called from the GCClient::Heap dtor). Must not
    // complete between stop and resume (I13): when shared, the unlink runs
    // under the server's GC barrier lock with !worldIsStoppedForAllClients(),
    // so removal of a stopped client defers to resume. A remove() leaving
    // size() == 1 with the main client surviving arms the §10D ISS-reversion
    // flag (consumed by the main client's thread at a later poll; T5).
    // Pre (shared): the client holds no heap access.
    void remove(GCClient::Heap&);

    // Pre: caller holds no lock that ranks <= 6, or the caller is the
    // conductor while worldIsStoppedForAllClients() (§5.1).
    template<typename Functor>
    void forEach(const Functor&);

    unsigned size() const;

    // §10D (review round 1): runs functor(size) with the registry lock
    // (rank 6) HELD. Heap::pollIssRevertIfNeeded uses this to make its final
    // size() == 1 re-check and the ISS clear one atomic step against add()'s
    // isSharedServer() re-check, which runs under this same lock — without
    // it, a concurrent add() could insert a second client between a
    // released-lock size() sample and the ISS store, yielding two registered
    // mutator clients with isSharedServer() == false (no MSPL, no stop
    // protocol). The functor must not take any lock of rank <= 6.
    template<typename Functor>
    void withSizeUnderRegistryLock(const Functor&);

    // SPEC-congc §9.3(1) (CG-3c): the mid-cycle ATTACH fence-init handshake.
    // MUST be called inside the GBL/!WSAC section of add()'s already-shared
    // insert, strictly BEFORE the registry append publishes the client
    // (asserted). Copies the server master fence/threshold pair into the
    // client's §5.3(2) copies and stamps m_fenceEpochSeen = FEP.
    // Happens-before: the master mutates only in-window (WSAC under GBL) and
    // this runs under GBL with !WSAC, so the snapshot is untorn and never
    // stale; a live-marking attachee starts RAISED; CG-I3's close assert
    // holds; the §5.3(3) pin subsumes the values, the FEP stamp stays.
    // !C1R: no-op — the copies are unrouted, unread state (F33/CGD4.4;
    // CG-I0 byte-for-byte). Defined in Heap.cpp (needs both heaps' privates
    // via this class's friendship). The add()-side call is a chartered-out
    // HeapClientSet.cpp hunk — see the INTEGRATE-congc.md manifest row.
    static void snapshotBarrierFenceStateForAttach(GCClient::Heap&);

    // SPEC-congc §9.2(1) (CG-3c): the EXIT1/teardown CMS final flush. Called
    // by GCClient::Heap::detachCurrentThread() (between the permanent access
    // drop and the epoch=MAX park) and by ~GCClient::Heap()'s
    // non-attached-thread branch — in BOTH cases strictly after the client's
    // last possible barrier (access permanently dropped => the CMS is
    // frozen) and strictly before HCS remove. Drains the client's CMS,
    // under m_markingMutex then the CMS leaf lock, into the SERVER legacy
    // m_mutatorMarkStack via its multi-producer append() (F44) — NOT into
    // m_sharedMutatorMarkStack: a between-cycles flush there would pre-load
    // the shared accounting before runBeginPhase's didReachTermination()
    // precondition (the CG-T8 Arm-1 RED root cause), and §9.2's F34 rule
    // forbids the phase read that could discriminate. See the CG-3c AMEND
    // record in INTEGRATE-congc.md (normative content; extends the CG-3b
    // §5.2(i) open-kind narrowing to the exit flush). !C1R: no-op — the CMS
    // is null (F33/CGD4.4; CG-I0). Idempotent; safe on a re-attaching
    // harness client (the flush is just an early total donation).
    //
    // DRAIN SHAPE (NORMATIVE; T7 congc-teardown-crash root cause): the CMS
    // is a GCSegmentedArray, and GCSegmentedArray::removeLast() does NOT
    // refill across segment boundaries — it is a bare data()[preDecTop()],
    // while isEmpty() stays false whenever a next segment exists. A bare
    //   while (!cms->isEmpty()) target->append(cms->removeLast());
    // loop therefore underflows m_top (0 -> SIZE_MAX) the moment the head
    // segment empties with further segments still chained — a wild read and
    // the observed teardown SIGSEGV (GCSegmentedArray<JSCell const*>::
    // removeLast <- this helper <- GCClient::Heap::detachCurrentThread <-
    // tearDownSpawnedThreadForExit), reachable whenever an exiting thread
    // accumulated more than one segment (~500 cells) of barrier appends
    // since the last WND-open drain. The implementation MUST use the same
    // segment-boundary pattern as every other MarkStackArray drain
    // (MarkStackArray::transferToImpl):
    //   while (!cms->isEmpty()) {
    //       cms->refill();
    //       while (cms->canRemoveLast())
    //           target->append(cms->removeLast());
    //   }
    // refill()'s head-segment destroy is licensed by the held CMS lock; the
    // per-cell server append keeps taking the target's multi-producer
    // m_appendLock (F44). Locking and §9.2(1) ordering are otherwise
    // unchanged.
    static void flushClientMutatorMarkStackForExit(GCClient::Heap&);

private:
    mutable Lock m_lock; // rank 6 (SPEC-heap.md §6)
    unsigned m_size WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    SentinelLinkedList<GCClient::Heap, BasicRawSentinelNode<GCClient::Heap>> m_clients WTF_GUARDED_BY_LOCK(m_lock);
};

template<typename Functor>
void HeapClientSet::forEach(const Functor& functor)
{
    Locker locker { m_lock };
    m_clients.forEach([&](GCClient::Heap* client) {
        functor(*client);
    });
}

template<typename Functor>
void HeapClientSet::withSizeUnderRegistryLock(const Functor& functor)
{
    Locker locker { m_lock };
    functor(m_size);
}

} // namespace JSC
