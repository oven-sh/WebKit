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

#include "config.h"
#include "HeapClientSet.h"

#include "Heap.h"
#include "Options.h"

namespace JSC {

HeapClientSet::~HeapClientSet()
{
    // Every client must have unregistered (GCClient::Heap dtor) before the
    // server heap dies.
    Locker locker { m_lock };
    ASSERT(!m_size);
    ASSERT(m_clients.isEmpty());
}

void HeapClientSet::add(GCClient::Heap& client) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    JSC::Heap& server = client.server();
    for (;;) {
        {
            Locker locker { m_lock };
            ASSERT(!m_clients.isOnList(&client));
            bool wouldShare = m_size && Options::useSharedGCHeap();
            if (!wouldShare) {
                // First client, or option off: no stop protocol can exist
                // yet, so the trivial insert needs only the registry lock
                // (I10).
                m_clients.append(&client);
                ++m_size;
                if (m_size == 1)
                    server.m_mainClient = &client; // §3.3/§10A: the first client is the owning VM's.
                return;
            }
        }

        if (!server.isSharedServer()) {
            // This add() makes size() > 1 with the option on: run the §10B.4
            // sticky switch FIRST, with no HCS lock held (it takes
            // *m_threadLock, rank 5, outer to our rank 6) — quiesce the
            // legacy GC protocol, set sticky ISS (I13/I15) — then loop to
            // insert via the shared branch below.
            server.noteSharedServerSticky();
            continue;
        }

        // Already-shared insert. I13 add-side (review round 1): like
        // remove(), an add() must not complete between stop and resume — the
        // reclaim sequence's two forEach passes and the step-8 resume pass
        // rely on a frozen registry inside the stop window
        // (GCSafepointEpoch::bumpAndReclaim). Hold GBL (rank 4) across the
        // insert: the conductor sets/clears WSAC under this lock (F7) and
        // broadcasts on resume (§10.8), so no stop window can open
        // mid-insertion. The attaching thread holds no heap access yet
        // (attachCurrentThread runs later), so waiting here cannot deadlock
        // the §10.4 barrier.
        Locker barrierLocker { server.m_gcBarrierLock }; // GBL, rank 4.
        while (server.worldIsStoppedForAllClients())
            server.m_gcBarrierCondition.wait(server.m_gcBarrierLock);

        Locker locker { m_lock }; // Rank 6, inside rank 4 — master order (§6).
        ASSERT(!m_clients.isOnList(&client));
        if (!server.isSharedServer()) {
            // Race note (§10D): a concurrent reversion cleared ISS — it does
            // so under THIS lock (Heap::pollIssRevertIfNeeded's
            // withSizeUnderRegistryLock step), so this re-check under m_lock
            // is race-free: either we see ISS true and may insert as a
            // shared client, or we see false and re-run the sticky switch.
            continue;
        }
        snapshotBarrierFenceStateForAttach(client); // SPEC-congc 9.3(1) (CG-3c): fence/FEP snapshot inside the publishing GBL/!WSAC section, BEFORE the insert.
        m_clients.append(&client);
        ++m_size;
        if (m_size == 1)
            server.m_mainClient = &client; // All prior clients removed with ISS still set.
        return;
    }
}

void HeapClientSet::remove(GCClient::Heap& client) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    JSC::Heap& server = client.server();

    if (!server.isSharedServer()) {
        // Single-client / option-off: today's trivial unlink (I10).
        Locker locker { m_lock };
        if (!m_clients.isOnList(&client))
            return;
        m_clients.remove(&client);
        ASSERT(m_size);
        --m_size;
        if (server.m_mainClient == &client)
            server.m_mainClient = nullptr;
        return;
    }

    // I13: remove() must not complete between stop and resume. The conductor
    // sets/clears WSAC under the barrier lock (F7) and broadcasts the barrier
    // condition on resume (§10.8), so holding GBL across the unlink
    // guarantees no stop window opens mid-removal. A client being removed
    // must not hold heap access (detachCurrentThread() first; otherwise this
    // wait could deadlock against the §10.4 barrier).
    ASSERT(!client.hasHeapAccess());
    bool armIssRevert = false;
    {
        Locker barrierLocker { server.m_gcBarrierLock }; // GBL, rank 4.
        while (server.worldIsStoppedForAllClients())
            server.m_gcBarrierCondition.wait(server.m_gcBarrierLock);

        Locker locker { m_lock }; // Rank 6, inside rank 4 — master order (§6).
        if (!m_clients.isOnList(&client))
            return;
        m_clients.remove(&client);
        ASSERT(m_size);
        --m_size;
        if (server.m_mainClient == &client)
            server.m_mainClient = nullptr;
        if (m_size == 1) {
            GCClient::Heap* survivor = nullptr;
            m_clients.forEach([&](GCClient::Heap* c) { survivor = c; });
            armIssRevert = survivor && survivor == server.m_mainClient;
        }
    }
    if (armIssRevert) {
        // §10D: arm the reversion; the main client's thread performs it at a
        // later collectIfNecessaryOrDefer/SINFAC poll
        // (Heap::pollIssRevertIfNeeded, T5 — never here).
        Locker locker { *server.m_threadLock }; // Rank 5; nothing else held.
        server.m_issRevertPending.store(true, std::memory_order_relaxed);
    }
}

unsigned HeapClientSet::size() const
{
    Locker locker { m_lock };
    return m_size;
}

} // namespace JSC
