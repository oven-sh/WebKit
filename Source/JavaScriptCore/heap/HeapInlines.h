/*
 * Copyright (C) 2014-2022 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "GCDeferralContext.h"
#include "Heap.h"
#include "HeapCellInlines.h"
#include "IndexingHeader.h"
#include "JSCast.h"
#include "Structure.h"
#include <type_traits>
#include <wtf/Assertions.h>
#include <wtf/MainThread.h>

namespace JSC {

// SharedGC (T9) vm() AUDIT LEGEND (SPEC-heap.md §3 deviation 3 / T9).
// The server Heap is by-value inside the main VM (phase 1: the shared server
// IS the main VM's vm.heap), so Heap::vm() is plain pointer arithmetic that
// yields "the main mutator VM" from ANY thread, including VM-less standalone
// (§12.1) clients and a VM-less §10.2 conductor. Every vm() use in heap/** is
// classified by a "SharedGC (T9)" tag into one of:
//   - main-VM-only: assumes the calling thread is the main VM's mutator
//     (API lock, entryScope, legacy !ISS m_worldState protocol). Sound today
//     and under the GIL (JSLock migration, I2); once ISS these sites are
//     either unreachable (legacy protocol quiesced, I15) or re-audited (T5b).
//   - per-client iteration: touches per-CLIENT state and must use
//     clientSet().forEach() / currentThreadClient() when shared.
//   - conductor-context OK: sound when executed by the §10.2 conductor
//     (incl. VM-less) — uses only VM-global/server-coupled state of the one
//     main VM, or is self-guarded (e.g. sanitizeStackForVM returns unless the
//     CALLING thread holds the main VM's API lock).
// Untagged uses reached only through tagged helpers inherit the helper's tag.
ALWAYS_INLINE VM& Heap::vm() const
{
    return *std::bit_cast<VM*>(std::bit_cast<uintptr_t>(this) - OBJECT_OFFSETOF(VM, heap));
}

ALWAYS_INLINE JSC::Heap* Heap::heap(const HeapCell* cell)
{
    if (!cell)
        return nullptr;
    return cell->heap();
}

inline JSC::Heap* Heap::heap(const JSValue v)
{
    if (!v.isCell())
        return nullptr;
    return heap(v.asCell());
}

// Defined in Heap.cpp with NEVER_INLINE to prevent LTO from breaking compiler barriers
// ALWAYS_INLINE bool Heap::isMarked(const void* rawCell)
// {
//     ASSERT(!m_isMarkingForGCVerifier);
//     HeapCell* cell = std::bit_cast<HeapCell*>(rawCell);
//     if (cell->isPreciseAllocation())
//         return cell->preciseAllocation().isMarked();
//     MarkedBlock& block = cell->markedBlock();
//     return block.isMarked(m_objectSpace.markingVersion(), cell);
// }

ALWAYS_INLINE bool Heap::testAndSetMarked(HeapVersion markingVersion, const void* rawCell)
{
    HeapCell* cell = std::bit_cast<HeapCell*>(rawCell);
    if (cell->isPreciseAllocation())
        return cell->preciseAllocation().testAndSetMarked();
    MarkedBlock& block = cell->markedBlock();
    Dependency dependency = block.aboutToMark(markingVersion, cell);
    return block.testAndSetMarked(cell, dependency);
}

ALWAYS_INLINE size_t Heap::cellSize(const void* rawCell)
{
    return std::bit_cast<HeapCell*>(rawCell)->cellSize();
}

inline void Heap::writeBarrier(const JSCell* from, JSValue to)
{
#if ENABLE(WRITE_BARRIER_PROFILING)
    WriteBarrierCounters::countWriteBarrier();
#endif
    if (!to.isCell())
        return;
    writeBarrier(from, to.asCell());
}

inline void Heap::writeBarrier(const JSCell* from, JSCell* to)
{
#if ENABLE(WRITE_BARRIER_PROFILING)
    WriteBarrierCounters::countWriteBarrier();
#endif
    ASSERT_GC_OBJECT_LOOKS_VALID(const_cast<JSCell*>(from));
    // FIXME: above assert verifies from is never nullptr so should be unnecessary
    if (!from) [[unlikely]]
        return;
    if (!to) [[unlikely]]
        return;
    ASSERT_GC_OBJECT_LOOKS_VALID(to);
    if (isWithinThreshold(from->cellState(), barrierThreshold())) [[unlikely]]
        writeBarrierSlowPath(from);
}

inline void Heap::writeBarrier(const JSCell* from)
{
    ASSERT_GC_OBJECT_LOOKS_VALID(const_cast<JSCell*>(from));
    // FIXME: above assert verifies from is never nullptr so should be unnecessary
    if (!from) [[unlikely]]
        return;
    if (isWithinThreshold(from->cellState(), barrierThreshold())) [[unlikely]]
        writeBarrierSlowPath(from);
}

inline void Heap::mutatorFence()
{
    // We could push this condition in the lower `if` as on X86 a storeStoreFence is a compilerFence
    // but this condition makes the logic a bit more explicit.
    if constexpr (isX86()) {
        WTF::compilerFence();
        return;
    }

    if (mutatorShouldBeFenced()) [[unlikely]]
        WTF::storeStoreFence();
}

template<typename Functor> inline void Heap::forEachCodeBlock(NOESCAPE const Functor& func)
{
    forEachCodeBlockImpl(scopedLambdaRef<void(CodeBlock*)>(func));
}

template<typename Functor> inline void Heap::forEachCodeBlockIgnoringJITPlans(const AbstractLocker& codeBlockSetLocker, NOESCAPE const Functor& func)
{
    forEachCodeBlockIgnoringJITPlansImpl(codeBlockSetLocker, scopedLambdaRef<void(CodeBlock*)>(func));
}

template<typename Functor> inline void Heap::forEachProtectedCell(const Functor& functor)
{
    for (auto& pair : m_protectedValues)
        functor(pair.key);
    m_handleSet.forEachStrongHandle(functor, m_protectedValues);
}

#if USE(FOUNDATION)
template <typename T>
inline void Heap::releaseSoon(RetainPtr<T>&& object)
{
    m_delayedReleaseObjects.append(WTF::move(object));
}
#endif

#ifdef JSC_GLIB_API_ENABLED
inline void Heap::releaseSoon(std::unique_ptr<JSCGLibWrapperObject>&& object)
{
    m_delayedReleaseObjects.append(WTF::move(object));
}
#endif

// SharedGC (§5.4/I17): when the server is shared, DeferGC depth is
// per-client — deferralDepthSlot() routes to the calling thread's client
// counter (or the server counter pre-ISS / on client-less threads), so one
// client's decrement can never close another client's DeferGC scope.
inline void Heap::incrementDeferralDepth()
{
    ASSERT(!Thread::mayBeGCThread() || m_worldIsStopped || worldIsStoppedForAllClients());
    deferralDepthSlot()++;
}

inline void Heap::decrementDeferralDepth()
{
    ASSERT(!Thread::mayBeGCThread() || m_worldIsStopped || worldIsStoppedForAllClients());
    unsigned& depth = deferralDepthSlot();
    ASSERT(depth);
    depth--;
}

inline void Heap::decrementDeferralDepthAndGCIfNeeded()
{
    ASSERT(!Thread::mayBeGCThread() || m_worldIsStopped || worldIsStoppedForAllClients());
    unsigned& depth = deferralDepthSlot();
    ASSERT(depth);
    depth--;

    // SharedGC (review round 4): per-client hint once ISS (didDeferGCWorkSlot
    // routes like deferralDepthSlot) — one client closing its scope can
    // neither observe nor swallow another client's pending hint.
    if (didDeferGCWorkSlot() || Options::forceDidDeferGCWork()) [[unlikely]] {
        decrementDeferralDepthAndGCIfNeededSlow();
        
        // Here are the possible relationships between m_deferralDepth and m_didDeferGCWork.
        // Note that prior to the call to decrementDeferralDepthAndGCIfNeededSlow,
        // m_didDeferGCWork had to have been true. Now it can be either false or true. There is
        // nothing we can reliably assert.
        //
        // Possible arrangements of m_didDeferGCWork and !!m_deferralDepth:
        //
        // Both false: We popped out of all DeferGCs and we did whatever work was deferred.
        //
        // Only m_didDeferGCWork is true: We stopped for GC and the GC did DeferGC. This is
        // possible because of how we handle the baseline JIT's worklist. It's also perfectly
        // safe because it only protects reportExtraMemory. We can just ignore this.
        //
        // Only !!m_deferralDepth is true: m_didDeferGCWork had been set spuriously. It is only
        // cleared by decrementDeferralDepthAndGCIfNeededSlow(). So, if we had deferred work but
        // then decrementDeferralDepth()'d, then we might have the bit set even if we GC'd since
        // then.
        //
        // Both true: We're in a recursive ~DeferGC. We wanted to do something about the
        // deferred work, but were unable to.
    }
}

inline void Heap::acquireAccess()
{
    // SharedGC (§10A): once ISS, JSLock::didAcquireLock's call here forwards
    // to the main client's acquireHeapAccess() (AHA), re-stamping the §10A.1
    // TLS slot and the access owner first (JSLock migration). Must precede
    // the m_worldState CAS: the legacy bits are superseded when shared.
    if (isSharedServer()) [[unlikely]] {
        acquireAccessForwardedToMainClient();
        return;
    }

    // SharedGC (T9): main-VM-only — below the ISS forward, so !ISS legacy
    // path; the caller is the main VM's mutator (JSLock wiring).
    if constexpr (validateDFGDoesGC)
        vm().verifyCanGC();

    if (m_worldState.compareExchangeWeak(0, hasAccessBit))
        return;
    acquireAccessSlow();
}

inline void Heap::stopIfNecessary()
{
    // SharedGC (§10A): shared-mode stop polling goes through SINFAC; the
    // legacy m_worldState machinery is quiesced once ISS (I15).
    if (isSharedServer()) [[unlikely]] {
        stopIfNecessaryForAllClients();
        return;
    }

    // SharedGC (T9): main-VM-only — !ISS legacy path (see acquireAccess()).
    if constexpr (validateDFGDoesGC)
        vm().verifyCanGC();

    if (mayNeedToStop())
        stopIfNecessarySlow();
}

template<typename Func>
void Heap::forEachSlotVisitor(const Func& func)
{
    func(*m_collectorSlotVisitor);
    func(*m_mutatorSlotVisitor);
    for (auto& visitor : m_parallelSlotVisitors)
        func(*visitor);
}

namespace GCClient {

ALWAYS_INLINE VM& Heap::vm() const
{
    // SPEC-heap.md §12.1/T9: standalone clients (markStandalone()) are not
    // embedded in a VM, so the OBJECT_OFFSETOF arithmetic below would produce
    // garbage. markStandalone() arms this assert.
    //
    // SharedGC (T9) audit of GCClient::Heap::vm() callers in heap/**:
    //   - Heap.cpp runSharedGCElection() (follower trap poll): guarded by
    //     `!client->m_isStandalone` before calling client->vm() — OK.
    //   - GCClient::IsoSubspace: no vm() use at all (ctor takes only the
    //     server BlockDirectory; allocateForClient() routes through
    //     client.server()) — standalone-safe by construction.
    //   - VM-taking entry points (CompleteSubspace::allocate(VM&),
    //     GCClient::IsoSubspace::allocate(VM&), `vm.clientHeap` accesses in
    //     Heap.cpp attach/forwarding) go VM->client, never client->vm(), so
    //     they cannot run on a standalone client.
    // No unguarded vm() call on a possibly-standalone client remains.
    RELEASE_ASSERT(!m_isStandalone);
    return *std::bit_cast<VM*>(std::bit_cast<uintptr_t>(this) - OBJECT_OFFSETOF(VM, clientHeap));
}

} // namespace GCClient

} // namespace JSC
