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
#include "GCSafepointEpoch.h"

#include "Heap.h"
#include "HeapClientSet.h"
#include "RaceAmplifier.h"
#include <limits>
#include <utility>
#include <wtf/Threading.h>

namespace JSC {

GCSafepointEpoch::~GCSafepointEpoch()
{
    // Server teardown. Every client has detached (HeapClientSet's dtor
    // asserts the registry is empty) and this heap will never reach another
    // safepoint, so safepoint-deferred destruction degenerates to immediate
    // destruction: drain whatever is still retired. Destroy outside
    // m_retireLock — destroy thunks may themselves retire(), and those
    // late-retired items must drain too.
    RELEASE_ASSERT(m_reclaimerBracketHolder.load() == nullptr);
    while (true) {
        Vector<RetiredItem> toDestroy;
        {
            Locker locker { m_retireLock };
            toDestroy = std::exchange(m_retired, Vector<RetiredItem>());
        }
        if (toDestroy.isEmpty())
            break;
        for (const RetiredItem& item : toDestroy)
            item.destroy(item.pointer);
    }
}

void GCSafepointEpoch::setServer(JSC::Heap& server)
{
    RELEASE_ASSERT(!m_server);
    m_server = &server;
}

void GCSafepointEpoch::retire(void* pointer, void (*destroy)(void*))
{
    ASSERT(pointer);
    ASSERT(destroy);
    // Leaf lock (§6/§13.10f): callable under the per-cell lock (10a) or
    // Structure::m_lock (10b); never while holding ranks 7-9b; not
    // async-signal-safe (allocates).
    uint64_t epoch = m_epoch.load(std::memory_order_acquire); // F4.
    // T10 amplifier hook (AMPLIFIER.md): widen the stale-epoch window — the
    // epoch is sampled but the item is not yet on the list; a concurrent
    // reclaim sequence bumping right now must still defer this item past the
    // NEXT safepoint (it lands stamped with the old epoch, which the bump's
    // min(localEpoch) cannot exceed by 2).
    RaceAmplifier::perturb();
    Locker locker { m_retireLock };
    m_retired.append(RetiredItem { pointer, destroy, epoch });
}

void GCSafepointEpoch::beginReclaimerBracket()
{
    // I11(c): the caller (Heap::runSafepointHooksAndReclaim, the sole reclaim
    // sequence) owns a compiler-thread suspension at this point — a fresh
    // suspendCompilerThreads() pair, the stop window's suspension that this
    // same thread holds across the bracket, or vacuously (no compiler
    // threads). The bracket is what licenses bumpAndReclaim(); a conducted
    // cycle's periphery suspension alone never does.
    RELEASE_ASSERT(!m_reclaimerBracketHolder.load());
    m_reclaimerBracketHolder.store(&Thread::currentSingleton());
}

void GCSafepointEpoch::endReclaimerBracket()
{
    RELEASE_ASSERT(m_reclaimerBracketHolder.load() == &Thread::currentSingleton());
    m_reclaimerBracketHolder.store(nullptr);
}

bool GCSafepointEpoch::reclaimLicensed() const
{
    if (!m_server)
        return false;
    bool worldStopped = m_server->isSharedServer()
        ? m_server->worldIsStoppedForAllClients() // I11(b), shared (§10 step 7).
        : m_server->worldIsStopped(); // I11(b), legacy runEndPhase (§9 note).
    return worldStopped && m_reclaimerBracketHolder.load() == &Thread::currentSingleton();
}

void GCSafepointEpoch::bumpAndReclaim()
{
    // I11: legal contexts are §10 step 7 (shared) and the legacy runEndPhase
    // reclamation site (!isSharedServer(), including option-off — the I10
    // exemption), both via Heap::runSafepointHooksAndReclaim(); never a
    // JSThreads stop (jit R4/CS4 refused — such stops enqueue a GC request
    // instead, §13.10a).
    RELEASE_ASSERT(m_server);
    RELEASE_ASSERT(reclaimLicensed()); // I11(b)+(c): world stopped, bracket held by this thread.

    uint64_t oldEpoch = m_epoch.load(std::memory_order_acquire);

    // §11: no-op when nothing is retired — no bump, no client iteration (the
    // sole option-off behavior delta stays a cheap empty check, I10).
    {
        Locker locker { m_retireLock };
        if (m_retired.isEmpty())
            return;
    }

    // T10 amplifier hook (AMPLIFIER.md): widen the bump window between the
    // empty-check and the min(localEpoch) scan — in-stop participants
    // (parallel marker helpers run only inside the stop window, I11) may
    // retire() concurrently; their items must survive this bump.
    RaceAmplifier::perturb();

    // I11(a): minimum over every registered client's published local epoch.
    // The reclaim sequence stamped each one to oldEpoch exactly while the
    // world was stopped; a detached client published UINT64_MAX (DCT) before
    // leaving the set; the registry cannot change inside a stop (I13). An
    // empty set (e.g. a server with no live clients at a forced collection)
    // yields an unbounded minimum. forEach takes the rank-6 registry lock —
    // fine for the stopped conductor (L3), and m_retireLock (leaf) is not
    // held here.
    uint64_t minLocalEpoch = std::numeric_limits<uint64_t>::max();
    m_server->clientSet().forEach([&](GCClient::Heap& client) {
        minLocalEpoch = std::min(minLocalEpoch, client.m_localEpoch.load(std::memory_order_seq_cst));
    });
    // Every client crossed THIS safepoint: a stale (un-stamped) client would
    // sit below oldEpoch and indicate the caller skipped the stamping loop.
    RELEASE_ASSERT(minLocalEpoch >= oldEpoch);

    // Items retired at epoch E are destroyed only once every client has
    // published a local epoch > E (I11(a)): with minLocalEpoch == oldEpoch,
    // items retired during this very stop window (epoch == oldEpoch) survive
    // to the next safepoint — the conducted cycle that retired them can
    // never be the one that frees them.
    Vector<RetiredItem> toDestroy;
    {
        Locker locker { m_retireLock };
        m_retired.removeAllMatching([&](const RetiredItem& item) {
            if (item.epoch < minLocalEpoch) {
                toDestroy.append(item);
                return true;
            }
            return false;
        });
    }

    // Destroy outside m_retireLock: destroy thunks may themselves retire()
    // (their items land in the next epoch window).
    for (const RetiredItem& item : toDestroy)
        item.destroy(item.pointer);

    m_epoch.store(oldEpoch + 1, std::memory_order_release); // F4.
}

} // namespace JSC
