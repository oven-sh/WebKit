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
#include "LocalAllocator.h"
#include "Options.h"
#include <algorithm>
#include <mutex>
#include <wtf/FastMalloc.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/ThreadSpecific.h>

namespace JSC {
namespace GCClient {

// --- §10A.1 current-client TLS (server -> client seam) ---
//
// One process-wide ThreadSpecific slot mapping each mutator thread to the
// GCClient::Heap it is currently operating as. Set by attachCurrentThread()
// and by the server's ISS access forwarding (JSLock migration re-stamps it);
// cleared by detachCurrentThread(); releaseHeapAccess() does NOT clear it.
// CanBeGCThread::True: GC helper threads may evaluate predicates that read
// the slot (they simply see null).

static LazyNeverDestroyed<ThreadSpecific<Heap*, WTF::CanBeGCThread::True>> s_currentThreadClient;
static std::once_flag s_currentThreadClientOnceFlag;

static ThreadSpecific<Heap*, WTF::CanBeGCThread::True>& currentThreadClientSlot()
{
    std::call_once(s_currentThreadClientOnceFlag, [] {
        s_currentThreadClient.construct();
    });
    return s_currentThreadClient.get();
}

Heap* Heap::currentThreadClient()
{
    return *currentThreadClientSlot();
}

void Heap::setCurrentThreadClient(Heap* client)
{
    *currentThreadClientSlot() = client;
}

// --- End §10A.1 current-client TLS ---

GCThreadLocalCache::GCThreadLocalCache(JSC::Heap& server)
    : m_server(server)
{
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
    if (oldTable)
        fastFree(oldTable);
}

void GCThreadLocalCache::registerExternalAllocator(LocalAllocator* allocator)
{
    ASSERT(allocator);
    // Lookup-only (§5.3): NOT appended to m_ownedAllocators — the
    // GCClient::IsoSubspace owns it by value; never entered in m_table.
    auto addResult = m_perDirectory.add(&allocator->directory(), allocator);
    ASSERT_UNUSED(addResult, addResult.isNewEntry);
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
