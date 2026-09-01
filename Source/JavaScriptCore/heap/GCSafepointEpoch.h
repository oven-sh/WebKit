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

#include <memory>
#include <wtf/Atomics.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace WTF {
class Thread;
}

namespace JSC {

class Heap;
class SharedHeapTestHarness;

// Epoch/handshake primitive for safepoint-deferred destruction
// (SPEC-heap.md §9/§11). Consumers (e.g. CodeBlock reclamation in the JIT
// workstream) retire() pointers whose destruction must be deferred until
// every registered mutator thread has crossed a stop-the-world safepoint.
//
// An item retired at epoch E is destroyed only after (I11):
//   (a) every client has published localEpoch > E (or detached), AND
//   (b) the world is stopped for all clients, AND
//   (c) compiler threads are suspended by the reclaimer's OWN suspend/resume
//       pair.
// bumpAndReclaim() runs only at §10 step 7 (shared mode) or at the legacy
// runEndPhase site (!isSharedServer(); SPEC-heap.md §9 contract notes) —
// never from a JSThreads stop (jit R4/CS4 refused; such stops enqueue a GC
// request instead).
//
// Lock ordering: m_retireLock is a leaf (SPEC-heap.md §6) — retire() may be
// called under the per-cell lock (10a) or Structure::m_lock (10b), but never
// while holding ranks 7-9b. retire() is not async-signal-safe.
class GCSafepointEpoch {
    WTF_MAKE_NONCOPYABLE(GCSafepointEpoch);
public:
    GCSafepointEpoch() = default;
    ~GCSafepointEpoch();

    uint64_t current() const { return m_epoch.load(std::memory_order_acquire); } // F4

    // Defer destruction of `pointer` (via `destroy`) past the next
    // all-client safepoint. Callable from any thread.
    JS_EXPORT_PRIVATE void retire(void* pointer, void (*destroy)(void*));

    template<typename T>
    void retire(std::unique_ptr<T> pointer)
    {
        retire(pointer.release(), [](void* p) {
            delete static_cast<T*>(p);
        });
    }

    // Conductor ONLY, while the world is stopped, after the reclaimer's own
    // compiler-thread suspension (I11) — i.e. only inside the reclaimer
    // bracket opened by Heap::runSafepointHooksAndReclaim(); RELEASE_ASSERTs
    // I11(b)/(c) and that every registered client's published local epoch
    // covers the current epoch (I11(a) stamping happened). No-op (no bump,
    // nothing destroyed) when nothing is retired (§11; the I10 option-off
    // exemption). Destroys items with epoch < min(client localEpochs), then
    // publishes epoch + 1.
    JS_EXPORT_PRIVATE void bumpAndReclaim();

    // True iff retire() has enqueued an item not yet reclaimed. Used by the
    // reclaim sequence (Heap::runSafepointHooksAndReclaim) to skip the
    // reclaimer suspension bracket when bumpAndReclaim() would no-op anyway
    // (§11 empty-check; the I10 option-off exemption) — see the comment at
    // that call site for the soundness argument.
    bool hasRetiredItems()
    {
        Locker locker { m_retireLock };
        return !m_retired.isEmpty();
    }

private:
    friend class JSC::Heap; // setServer + the reclaimer bracket (§11).
    friend class JSC::SharedHeapTestHarness; // reclaimLicensed() for the I11 unit test (T7).

    struct RetiredItem {
        void* pointer;
        void (*destroy)(void*);
        uint64_t epoch;
    };

    // Wired once from the owning server Heap's constructor (the member is
    // by-value in Heap, so this cannot be a constructor argument without
    // perturbing Heap's giant init list).
    void setServer(JSC::Heap&);

    // I11(c) bracket. Opened by the reclaimer (the conducted-cycle mutator at
    // the legacy runEndPhase site, or the §10 conductor at step 7) strictly
    // AFTER it owns a compiler-thread suspension: a fresh
    // suspendCompilerThreads() pair, an explicit adoption of the stop
    // window's suspension that this same thread holds and will release only
    // after the bracket closes, or vacuous (no compiler threads exist to
    // suspend). A conducted cycle's periphery suspension does NOT by itself
    // license a bump (T7 unit test): the license is the bracket, which only
    // the reclaim sequence opens.
    void beginReclaimerBracket();
    void endReclaimerBracket();

    // True iff the calling thread may bumpAndReclaim() right now: the world
    // is stopped (WSAC when shared, worldIsStopped() legacy) AND this thread
    // holds the reclaimer bracket. Validation/testing seam for I11.
    bool reclaimLicensed() const;

    Atomic<uint64_t> m_epoch { 1 };
    Lock m_retireLock; // leaf rank (SPEC-heap.md §6)
    Vector<RetiredItem> m_retired WTF_GUARDED_BY_LOCK(m_retireLock);
    JSC::Heap* m_server { nullptr }; // §11; set once at server construction.
    Atomic<WTF::Thread*> m_reclaimerBracketHolder { nullptr }; // I11(c); written only by the reclaimer while the world is stopped.
};

} // namespace JSC
