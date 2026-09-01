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
#include <algorithm>
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

thread_local Heap* Heap::s_currentThreadClient { nullptr };
// H-TLS-TABLE (SHAREDHEAP-ALLOC-EVIDENCE.md §41 T1): direct snapshot of the
// current thread's GCThreadLocalCache {m_table, m_tableBound} so the C++
// non-iso allocate fast path (CompleteSubspaceInlines.h) skips both the
// allocationClientForCurrentThread resolver and the client.threadLocalCache()
// member chase. Restamped at the two owner-thread mutation points only:
// setCurrentThreadClient (attach/detach/A36C swap) and growTable. Zero-init
// → bound==0 → fast path falls through, so unstamped readers (compilation
// threads, pre-attach bootstrap) preserve today's resolver semantics.
thread_local Allocator* Heap::s_currentThreadTLCTable { nullptr };
thread_local unsigned Heap::s_currentThreadTLCBound { 0 };

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
    // H-VMLITE-TLCPTR attach stamp: every {attach, A36C carrier swap, main-
    // client adoption} re-stamps the §10A.1 slot here, AFTER the carrier's
    // VMLite::setCurrent (JSLock.cpp install ordering), so the lite mirror
    // tracks the now-current client's already-grown table without waiting for
    // the next growTable. Detach (null client) clears the mirror so a stale
    // table pointer is never read past the client's lifetime.
    // H-TLS-TABLE: restamp the IE-TLS {table, bound} snapshot at the same
    // sites — the C++ fast path's correctness depends on exactly the same
    // "current client's table" invariant the lite mirror does. Detach clears
    // to {nullptr, 0} so a post-detach allocate() reads bound==0 and takes
    // the resolver slow path (which is where the I2 ASSERTs live).
    if (client) {
        Allocator* table = client->threadLocalCache().table();
        unsigned bound = client->threadLocalCache().tableBound();
        setCurrentThreadTLCSnapshot(table, bound);
        stampTLCMirrorOnCurrentLite(table, bound);
        // H-GCCLIENT-COMPLETESUBSPACE-WRAPPER bind: idempotent; computes each
        // wrapper's slotBase = table + server.tlcIndexBase() now that (a) the
        // fixed-capacity table exists (TLC ctor above) and (b) every server
        // CompleteSubspace's tlcIndexBase is reserved (also TLC ctor). Gated
        // on the option so a GIL-on / non-shared client never touches the
        // wrappers (they stay default-constructed and have no caller). Runs
        // on every {attach, A36C carrier swap, main-client adoption} — same
        // sites as the TLS/lite restamps; first attach is the only one that
        // changes state, the rest re-store identical pointers.
        if (Options::useSharedGCHeap())
            client->bindCompleteSubspaceClients();
    } else {
        setCurrentThreadTLCSnapshot(nullptr, 0);
        stampTLCMirrorOnCurrentLite(nullptr, 0);
    }
}

// --- End §10A.1 current-client TLS ---

GCThreadLocalCache::GCThreadLocalCache(JSC::Heap& server)
    : m_server(server)
{
    // H-TLC-FIXEDTABLE-NOREALLOC + H-GCCLIENT-COMPLETESUBSPACE-WRAPPER
    // (SHAREDHEAP-ALLOC-EVIDENCE.md §41 T1): eagerly reserve every server
    // CompleteSubspace's tlcIndexBase (write-once under directoryLock,
    // idempotent across clients; NO BlockDirectory is created — see
    // CompleteSubspace::ensureTlcIndexBaseReserved) and pre-grow m_table to
    // its lifetime maximum so it NEVER reallocs. This makes every per-client
    // slotBase pointer (= m_table + tlcIndexBase) and the H-TLS-TABLE /
    // H-VMLITE-TLCPTR {table, bound} snapshots valid for the client's
    // lifetime with no restamp-on-grow hazard. Flag-off: today's lazy
    // {nullptr, 0} table — the branch is the only delta. RSS: 5 ×
    // numSizeClasses × sizeof(Allocator) ≈ a few KB per client (well inside
    // the +10% gate at W=16). Runs during GCClient::Heap construction, which
    // for the first client (vm.clientHeap) is after JSC::Heap is fully
    // constructed (VM member declaration order), so the server subspaces
    // exist and directoryLock is takeable.
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
        // Direct allocate (NOT growTable): growTable would restamp the
        // s_currentThreadTLCTable/Bound TLS pair and the lite mirror, but at
        // ctor time this thread's stamped client (if any) is a DIFFERENT
        // GCClient::Heap — the restamp would install the wrong table. The
        // correct stamp for THIS client lands at setCurrentThreadClient
        // (attach), which now also binds the per-client CompleteSubspace
        // wrappers.
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

Allocator GCThreadLocalCache::allocatorFor(BlockDirectory& directory)
{
    unsigned index = directory.tlcIndex();
    if (index == BlockDirectory::invalidTlcIndex) {
        // Iso directories: lookup-only (§5.3) — the GCClient::IsoSubspace
        // LocalAllocator registered at materialization; null if this client
        // never materialized one for this directory.
        return Allocator(m_perDirectory.get(&directory));
    }
    if (index < m_tableBound) {
        if (Allocator allocator = m_table[index])
            return allocator;
    }
    return materializeAllocator(directory);
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

    growTable(tlcIndex + 1);

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

void GCThreadLocalCache::growTable(unsigned neededBound)
{
    if (neededBound <= m_tableBound)
        return;
    // H-TLC-FIXEDTABLE-NOREALLOC (§41 T1): with the fixed-capacity pre-grow
    // in the ctor, the early-return above must always fire under
    // Options::useSharedGCHeap() — m_table never reallocs, so the per-client
    // slotBase pointers (GCClient::CompleteSubspace::m_slotBase) and the
    // H-TLS-TABLE / H-VMLITE-TLCPTR snapshots never dangle. Reaching here
    // means a tlcIndex exceeded numCompleteSubspaces × numSizeClasses, i.e. a
    // 6th CompleteSubspace was added without bumping
    // JSC::Heap::numCompleteSubspaces. The realloc/free leg below is retained
    // verbatim for !useSharedGCHeap (where this whole class is inert today)
    // and as the documented fallback shape; it is unreachable flag-on.
    RELEASE_ASSERT_WITH_MESSAGE(!Options::useSharedGCHeap(), "GCThreadLocalCache fixed-capacity table overflow; bump JSC::Heap::numCompleteSubspaces");
    static constexpr unsigned minimumBound = 32;
    unsigned newBound = std::max(neededBound, std::max(m_tableBound * 2, minimumBound));
    Allocator* newTable = static_cast<Allocator*>(fastMalloc(static_cast<size_t>(newBound) * sizeof(Allocator)));
    for (unsigned i = 0; i < m_tableBound; ++i)
        newTable[i] = m_table[i];
    for (unsigned i = m_tableBound; i < newBound; ++i)
        newTable[i] = Allocator();
    Allocator* oldTable = m_table;
    // PROVISIONAL JIT addressing contract (§5.3 Status / deviation 6):
    // contents are published before the pointer, the pointer before the
    // bound, so a same-thread (or future TLS-relative) reader doing
    // bound-check + indexed load never sees uninitialized slots.
    WTF::storeStoreFence();
    m_table = newTable;
    WTF::storeStoreFence();
    m_tableBound = newBound; // Grow-only (§5.3).
    // H-VMLITE-TLCPTR + H-TLS-TABLE: re-stamp both the lite mirror and the
    // IE-TLS snapshot BEFORE freeing the old table — the readers (JIT inline
    // emitter via lite, CompleteSubspace::allocate via TLS) are this same
    // thread (I2/I11) so there is no concurrent dereference of oldTable, but
    // keeping the snapshots coherent with {m_table, m_tableBound} across the
    // free preserves the bound-first/table-second invariant unconditionally.
    // growTable is reachable only from materializeAllocator (owner thread,
    // under Options::useSharedGCHeap()), so the restamp installs THIS
    // thread's allocating client's table — exactly what
    // allocationClientForCurrentThread would have resolved to on the call
    // that landed here.
    Heap::setCurrentThreadTLCSnapshot(newTable, newBound);
    stampTLCMirrorOnCurrentLite(newTable, newBound);
    if (oldTable)
        fastFree(oldTable);
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

void GCThreadLocalCache::stopAllocating()
{
    // §10 step 5 flush: conductor-side while the world is stopped for all
    // clients (I2 exception), or the owner thread itself.
    if (Options::validateFreeListStructure()) [[unlikely]]
        FreeList::setStructureValidationContext("tlcflush"); // Per-client step-5 flush provenance.
    for (LocalAllocator* allocator : m_perDirectory.values())
        allocator->stopAllocating();
    if (Options::validateFreeListStructure()) [[unlikely]]
        FreeList::setStructureValidationContext("other");
}

void GCThreadLocalCache::resumeAllocating()
{
    // §10 step 8: strictly precedes the VMM resume (§10.9).
    for (LocalAllocator* allocator : m_perDirectory.values())
        allocator->resumeAllocating();
}

void GCThreadLocalCache::prepareForAllocation()
{
    // Mirrors MarkedSpace::prepareForAllocation per slot (conductor-side).
    for (LocalAllocator* allocator : m_perDirectory.values())
        allocator->prepareForAllocation();
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
        entry.value->stopAllocatingForGood();
        entry.key->detachLocalAllocator(*entry.value);
    }
    if (Options::validateFreeListStructure()) [[unlikely]]
        FreeList::setStructureValidationContext("other");
}

} // namespace GCClient
} // namespace JSC
