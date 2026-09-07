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
#include "GCThreadLocalCache.h"

#include "BlockDirectory.h"
#include "CompleteSubspace.h"
#include "FreeList.h"
#include "Heap.h"
#include "IsoSubspace.h"
#include "LocalAllocator.h"
#include "Options.h"
#include "VMLite.h"
#include <wtf/FastMalloc.h>

namespace JSC {
namespace GCClient {

// --- §10A.1 current-client TLS (server -> client seam) ---
//
// One process-wide thread_local slot mapping each mutator thread to the
// GCClient::Heap it is currently operating as. Set by attachCurrentThread()
// and by the server's ISS access forwarding (JSLock migration re-stamps it);
// cleared by detachCurrentThread(); releaseHeapAccess() does NOT clear it.
// GC helper threads may evaluate predicates that read the slot (they simply
// see the zero-initialized null — the prior ThreadSpecific's
// CanBeGCThread::True served only to permit that null read, which a plain
// thread_local gives unconditionally).
//
// B1-alloc-client-tls-fastpath: replaced the LazyNeverDestroyed<
// ThreadSpecific<Heap*>> + std::call_once resolver with a plain C++
// thread_local (same storage model as t_currentVMLite, VMLite.cpp:67). The
// reader is now ALWAYS_INLINE in Heap.h so allocationClientForCurrentThread
// collapses to one predictable branch + one __thread load + the existing
// server-identity check on the per-allocateCell hot path; this was the
// dominant term in the GIL-off/GIL-on heap-BigInt gap (bigintcost:
// currentThreadClient + pthread_getspecific + pthread_once@plt). Flag-off
// the slot is never read (every reader is behind a useJSThreads / gilOff /
// isSharedServer gate) and zero-init matches "never constructed". The
// writer stays out-of-line so every existing stamp site (attach/detach,
// JSLock A36C carrier swap, Heap.cpp main-client adoption) is semantically
// unchanged.

constinit thread_local Heap* Heap::s_currentThreadClient { nullptr };
// Snapshot of the current thread's stamped client's GCThreadLocalCache
// {m_table, m_tableBound}, so the C++ non-iso allocate fast path
// (CompleteSubspaceInlines.h) skips both the allocationClientForCurrentThread
// resolver and the client.threadLocalCache() member chase. Written only by
// setCurrentThreadClient, together with s_currentThreadClient, so bound > 0
// implies a stamped client and the table is that client's. The table is
// allocated at its lifetime capacity in the GCThreadLocalCache ctor, so the
// pointer never dangles while the client lives. Zero-init gives bound == 0,
// so unstamped readers (compilation threads, pre-attach bootstrap) take the
// resolver.
constinit thread_local Allocator* Heap::s_currentThreadTLCTable { nullptr };
constinit thread_local unsigned Heap::s_currentThreadTLCBound { 0 };

void Heap::setCurrentThreadTLCSnapshot(Allocator* table, unsigned bound)
{
    // Same-thread reader (CompleteSubspace::allocate) does bound-check FIRST
    // then indexed load; publish table BEFORE bound so a bound>0 read always
    // sees the matching table pointer (program order on one thread; the fence
    // is documentation of intent, not a cross-thread requirement).
    s_currentThreadTLCTable = table;
    WTF::compilerFence();
    s_currentThreadTLCBound = bound;
}

// H-VMLITE-TLCPTR (§B.4 lite-mirror stamp): publish {table, bound} onto the
// CURRENT thread's installed VMLite so the per-tier inline-allocate emitters
// can resolve `tlcTable[tlcIndexBase + sizeClassIndex]` with one
// lite-relative loadPtr. Owner-thread only (I2) and the JIT reader is the
// owner thread (I11), so program order suffices; bound is published LAST so a
// bound-first reader (the emitted bound>slot guard) never indexes past the
// table just stored. Gated on the process-level gilOff bit so flag-off / a
// GIL-on second VM never touch the lite (the fields are never read either).
// `lite` may be null (pre-install bootstrap allocations, GC-helper threads):
// the next stamp site re-stamps once the lite is installed.
static ALWAYS_INLINE void stampTLCMirrorOnCurrentLite(Allocator* table, unsigned bound)
{
    if (!g_jscConfig.gilOffProcess) [[likely]]
        return;
    VMLite* lite = VMLite::currentIfExists();
    if (!lite || !lite->gilOff)
        return;
    lite->tlcTable = table;
    WTF::compilerFence(); // bound LAST (same-thread program-order guarantee for the bound-first JIT reader).
    lite->tlcTableBound = bound;
}

void Heap::setCurrentThreadClient(Heap* client)
{
    s_currentThreadClient = client;
    // Every {attach, A36C carrier swap, main-client adoption} re-stamps the
    // §10A.1 slot here, AFTER the carrier's VMLite::setCurrent (JSLock.cpp
    // install ordering), so the lite mirror and the TLS {table, bound}
    // snapshot always describe the now-current client's table; this is the
    // only writer of either. Detach (null client) clears both so a stale
    // table pointer is never read past the client's lifetime and a
    // post-detach allocate() reads bound == 0 and takes the resolver slow
    // path (which is where the I2 ASSERTs live).
    if (client) {
        Allocator* table = client->threadLocalCache().table();
        unsigned bound = client->threadLocalCache().tableBound();
        setCurrentThreadTLCSnapshot(table, bound);
        stampTLCMirrorOnCurrentLite(table, bound);
    } else {
        setCurrentThreadTLCSnapshot(nullptr, 0);
        stampTLCMirrorOnCurrentLite(nullptr, 0);
    }
}

// --- End §10A.1 current-client TLS ---

GCThreadLocalCache::GCThreadLocalCache(JSC::Heap& server)
    : m_server(server)
{
    // Eagerly reserve every server CompleteSubspace's tlcIndexBase (write-once
    // under directoryLock, idempotent across clients; no BlockDirectory is
    // created — see CompleteSubspace::ensureTlcIndexBaseReserved) and allocate
    // m_table at its lifetime maximum so it never reallocs. This keeps the
    // per-thread TLS and VMLite {table, bound} snapshots valid for the
    // client's lifetime with no restamp-on-grow hazard, and lets the JIT bake
    // every tlcIndexBase from the first compile. Flag-off: the lazy
    // {nullptr, 0} table — the branch is the only delta. RSS: 5 ×
    // numSizeClasses × sizeof(Allocator) ≈ a few KB per client. Runs during
    // GCClient::Heap construction, which for the first client (vm.clientHeap)
    // is after JSC::Heap is fully constructed (VM member declaration order),
    // so the server subspaces exist and directoryLock is takeable.
    if (Options::useSharedGCHeap()) [[unlikely]] {
        server.primitiveGigacageAuxiliarySpace.ensureTlcIndexBaseReserved();
        server.auxiliarySpace.ensureTlcIndexBaseReserved();
        server.immutableButterflyAuxiliarySpace.ensureTlcIndexBaseReserved();
        server.cellSpace.ensureTlcIndexBaseReserved();
        server.destructibleObjectSpace.ensureTlcIndexBaseReserved();
        constexpr unsigned nonIsoCapacity = JSC::Heap::numCompleteSubspaces * static_cast<unsigned>(MarkedSpace::numSizeClasses);
        // H-ISO-TLCSLOT (GILOFF-TAX §42 follow-on): one fixed slot per static
        // server iso subspace, contiguous after the non-iso region. The slot is
        // a per-type compile-time constant (nonIsoCapacity + macro ordinal) so
        // JIT inline-allocate emitters bake it exactly as they bake a
        // CompleteSubspace tlcIndexBase — closing the IT-9 hole for iso types
        // (MakeRope: 36.4M lazy-slow-path traversals on intcs W=1). Stamping is
        // idempotent: the first client (vm.clientHeap, constructed serially
        // during the VM ctor after every server subspace is complete) writes
        // each slot once; later per-thread clients re-observe the same value
        // (asserted in stampTlcSlot). The 4 SpaceAndSet-backed statics and all
        // dynamic iso subspaces are NOT enumerated — they keep
        // invalidTlcIndex and stay m_perDirectory lookup-only (none is on a JIT
        // inline-allocate path). The BlockDirectory's own m_tlcIndex stays
        // invalidTlcIndex so allocatorFor / materializeAllocator's "iso =
        // lookup-only" predicates are unchanged.
        constexpr unsigned numStaticIsoSlots = 0
#define THREADS_COUNT_STATIC_ISO(name, heapCellType, type) + 1
            FOR_EACH_JSC_ISO_SUBSPACE(THREADS_COUNT_STATIC_ISO)
#undef THREADS_COUNT_STATIC_ISO
            ;
        {
            unsigned slot = nonIsoCapacity;
#define THREADS_STAMP_ISO_TLC_SLOT(name, heapCellType, type) \
            server.name.stampTlcSlot(slot++);
            FOR_EACH_JSC_ISO_SUBSPACE(THREADS_STAMP_ISO_TLC_SLOT)
#undef THREADS_STAMP_ISO_TLC_SLOT
            ASSERT_UNUSED(slot, slot == nonIsoCapacity + numStaticIsoSlots);
        }
        constexpr unsigned fixedCapacity = nonIsoCapacity + numStaticIsoSlots;
        // No TLS or lite-mirror stamp here: at ctor time this thread's stamped
        // client (if any) is a different GCClient::Heap. The stamp for this
        // client lands at setCurrentThreadClient (attach).
        m_table = static_cast<Allocator*>(fastZeroedMalloc(static_cast<size_t>(fixedCapacity) * sizeof(Allocator)));
        m_tableBound = fixedCapacity;
    }
}

GCThreadLocalCache::~GCThreadLocalCache()
{
    // §5.3 teardown (I9): GCClient::Heap::~Heap() ALWAYS ran
    // lastChanceToFinalize() -> stopAllocatingForGood() in its body, while
    // the client still held heap access and was still registered (the
    // review-round-1 teardown order) — this dtor runs later, during member
    // destruction, with no access and the client unregistered. Re-running
    // stopAllocatingForGood() here is a belt-and-braces straggler pass that
    // is a no-op by construction: every FreeList is empty, every current
    // block is null (LocalAllocator::stopAllocating early-returns without
    // touching any shared directory state), and every allocator is already
    // off its directory's m_localAllocators list (detachLocalAllocator
    // skips unlinked nodes), so no WSAC-licensed conductor work can race it
    // even though this thread holds only MSPL and no access.
    stopAllocatingForGood();
    m_perDirectory.clear(); // External (iso) allocators are owned by their IsoSubspace.
    m_ownedAllocators.clear(); // Owned LocalAllocators: stopped + unlinked above.
    if (m_table)
        fastFree(m_table);
}

Allocator GCThreadLocalCache::allocatorForSizeStep(CompleteSubspace& subspace, size_t sizeClassIndex)
{
    ASSERT(sizeClassIndex < MarkedSpace::numSizeClasses);
    // PROVISIONAL fast path contract (§5.3 Status): slot = tlcIndexBase +
    // sizeClassIndex; slot < m_tableBound ? m_table[slot] : null => slow.
    unsigned base = subspace.tlcIndexBase();
    if (base != BlockDirectory::invalidTlcIndex) {
        size_t slot = base + sizeClassIndex;
        if (slot < m_tableBound) {
            if (Allocator allocator = m_table[slot])
                return allocator;
        }
    }
    BlockDirectory* directory = subspace.ensureDirectoryForSizeStep(sizeClassIndex);
    if (!directory)
        return Allocator(); // No size class for this step: precise path.
    return materializeAllocator(*directory);
}

Allocator GCThreadLocalCache::materializeAllocator(BlockDirectory& directory)
{
    ASSERT(Options::useSharedGCHeap());
    // I2/§10A.1: only the owning thread materializes into its own cache; the
    // LocalAllocator ctor links into the directory's allocator list under
    // m_localAllocatorsLock (rank 8) — no MSPL needed here.

    // I3 dedup: one allocator per (client, directory); aliased table slots
    // share the pointer.
    if (LocalAllocator* existing = m_perDirectory.get(&directory))
        return Allocator(existing);

    auto owned = makeUnique<LocalAllocator>(&directory);
    LocalAllocator* allocator = owned.get();
    m_ownedAllocators.append(WTF::move(owned));
    auto addResult = m_perDirectory.add(&directory, allocator);
    ASSERT_UNUSED(addResult, addResult.isNewEntry);

    unsigned tlcIndex = directory.tlcIndex();
    ASSERT(tlcIndex != BlockDirectory::invalidTlcIndex); // Iso never reaches here (registered, so deduped above).
    ASSERT(directory.subspace() && !directory.subspace()->isIsoSubspace());
    CompleteSubspace& subspace = *static_cast<CompleteSubspace*>(directory.subspace());
    unsigned base = subspace.tlcIndexBase();
    ASSERT(base != BlockDirectory::invalidTlcIndex);

    // m_table was allocated at its lifetime capacity in the ctor, so the
    // per-thread {table, bound} snapshots never dangle.
    // MarkedSpace::reserveThreadLocalCacheIndices
    // keeps every non-iso tlcIndex below numCompleteSubspaces * numSizeClasses
    // (the iso slots start there), so this is only a bounds check on the write.
    RELEASE_ASSERT_WITH_MESSAGE(tlcIndex < m_tableBound, "GCThreadLocalCache fixed-capacity table overflow; bump JSC::Heap::numCompleteSubspaces");

    // Fill every size step aliased to this directory's size class (§5.3:
    // aliased entries share the LocalAllocator*). The directory's own
    // tlcIndex is the canonical (largest) aliased slot.
    size_t sizeClass = directory.cellSize();
    size_t index = MarkedSpace::sizeClassToIndex(sizeClass);
    ASSERT(base + index == tlcIndex);
    for (;;) {
        if (MarkedSpace::s_sizeClassForSizeStep[index] != sizeClass)
            break;
        m_table[base + index] = Allocator(allocator);
        if (!index--)
            break;
    }

    return Allocator(allocator);
}

void GCThreadLocalCache::registerExternalAllocator(LocalAllocator* allocator)
{
    ASSERT(allocator);
    // Lookup-only (§5.3): NOT appended to m_ownedAllocators — the
    // GCClient::IsoSubspace owns it by value. m_perDirectory stays the
    // I3-authoritative owner set (forEachLocalAllocator / ownsLocalAllocator /
    // teardown).
    auto addResult = m_perDirectory.add(&allocator->directory(), allocator);
    ASSERT_UNUSED(addResult, addResult.isNewEntry);
    // H-ISO-TLCSLOT: ALSO publish into the flat table when the server iso
    // subspace carries a stamped slot (FOR_EACH_JSC_ISO_SUBSPACE statics). The
    // ctor stamped every such slot and pre-grew m_table to cover them BEFORE
    // this runs (GCClient::Heap declares m_threadLocalCache after the iso
    // members; registerIsoSubspaceLocalAllocators is called from the ctor
    // BODY). Owner thread only (I2). Iso subspaces with no stamped slot
    // (SpaceAndSet statics, dynamic iso) and non-iso external allocators (none
    // exist today) skip the table write — m_perDirectory remains the
    // §10A.1/§5.3 source of truth either way.
    Subspace* subspace = allocator->directory().subspace();
    if (subspace && subspace->isIsoSubspace()) {
        unsigned slot = static_cast<JSC::IsoSubspace*>(subspace)->tlcSlot();
        if (slot != BlockDirectory::invalidTlcIndex) {
            ASSERT(slot < m_tableBound);
            ASSERT(!m_table[slot]);
            m_table[slot] = Allocator(allocator);
        }
    }
}

bool GCThreadLocalCache::ownsLocalAllocator(const LocalAllocator* allocator) const
{
    if (!allocator)
        return false;
    // I3: one allocator per (client, directory); aliased table slots share
    // the pointer, so the per-directory map is the authoritative owner set.
    return m_perDirectory.get(&allocator->directory()) == allocator;
}

void GCThreadLocalCache::resumeAllocating()
{
    // §10 step 8: strictly precedes the VMM resume (§10.9).
    for (LocalAllocator* allocator : m_perDirectory.values())
        allocator->resumeAllocating();
}

void GCThreadLocalCache::stopAllocatingForGood()
{
    // §5.3 teardown (I9), world running: directory-bit flips (I5b writes)
    // under MSPL across all slots, then unlink each allocator under its
    // directory's m_localAllocatorsLock (lock order 7 -> 8). MSPL implies
    // access here (review round 1): the effective caller is
    // GCClient::Heap::~Heap, which holds the client's heap access across
    // this call — that, not MSPL, is what excludes a concurrent conducted
    // stop's WSAC-licensed flush from racing us (the dtor's straggler
    // re-run below is a structural no-op, see ~GCThreadLocalCache). Covers owned
    // (non-iso) allocators AND the registered GCClient::IsoSubspace
    // allocators — implementing the GlobalGC FIXME from GCClient::Heap
    // (relinquish iso LocalAllocator memory back to the server). After this
    // returns, no allocator of this client is on any directory's
    // m_localAllocators list and every previously-held block has
    // inUse == false (I9; LocalAllocator::stopAllocating returns the current
    // block via MarkedBlock::Handle::stopAllocating, clearing inUse).
    MutatorSlowPathLocker mutatorSlowPathLocker(m_server);
    if (Options::validateFreeListStructure()) [[unlikely]]
        FreeList::setStructureValidationContext("tlcSAFG"); // Teardown flush provenance.
    for (auto& entry : m_perDirectory) {
        entry.value->stopAllocatingForClientTeardown();
        entry.key->detachLocalAllocator(*entry.value);
    }
    if (Options::validateFreeListStructure()) [[unlikely]]
        FreeList::setStructureValidationContext("other");
}

} // namespace GCClient
} // namespace JSC
