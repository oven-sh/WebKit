/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
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
#include "PropertyTable.h"

#include "HeapInlines.h"
#include "JSCJSValueInlines.h"
#include <wtf/MathExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(PropertyTable);

WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyTable::DeletedOffsets);

const ClassInfo PropertyTable::s_info = { "PropertyTable"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(PropertyTable) };

PropertyTable* PropertyTable::create(VM& vm, unsigned initialCapacity)
{
    PropertyTable* table = new (NotNull, allocateCell<PropertyTable>(vm)) PropertyTable(vm, initialCapacity);
    table->finishCreation(vm);
    return table;
}

PropertyTable* PropertyTable::clone(VM& vm, const PropertyTable& other)
{
    PropertyTable* table = new (NotNull, allocateCell<PropertyTable>(vm)) PropertyTable(vm, other);
    table->finishCreation(vm);
    return table;
}

PropertyTable* PropertyTable::clone(VM& vm, unsigned initialCapacity, const PropertyTable& other)
{
    PropertyTable* table = new (NotNull, allocateCell<PropertyTable>(vm)) PropertyTable(vm, initialCapacity, other);
    table->finishCreation(vm);
    return table;
}

PropertyTable::PropertyTable(VM& vm, unsigned initialCapacity)
    : JSCell(vm, vm.propertyTableStructure.get())
{
    // TSAN: header words are stored with the relaxed accessors even during
    // construction. The table is private until its Structure publishes it,
    // and a cell is never recycled while a lock-free probe still holds its
    // pointer (see ~PropertyTable), so this is not a memory-safety measure:
    // the relaxed stores exist so TSAN pairs them with a previous occupant's
    // relaxed probes of this cell address, whose happens-before edge through
    // the GC's sweep and reallocation TSAN cannot see.
    concurrentRelaxedStore(m_indexSize, sizeForCapacity(initialCapacity));
    concurrentRelaxedStore(m_indexMask, indexSize() - 1);
    concurrentRelaxedStore(m_keyCount, 0u);
    concurrentRelaxedStore(m_deletedCount, 0u);
    ASSERT(isPowerOfTwo(indexSize()));
    bool isCompact = tableCapacity() < UINT8_MAX;
    publishIndexVector(allocateZeroedIndexVector(isCompact, indexSize()));
    ASSERT(isCompact == this->isCompact());
}

// TSAN: the clone source `other` can be a published table (its in-place
// mutators hold the owning Structure's m_lock, which the clone path also
// holds, or the table is private) - read its header words via the relaxed
// accessors to pair correctly with its relaxed-store writers.
PropertyTable::PropertyTable(VM& vm, const PropertyTable& other)
    : JSCell(vm, vm.propertyTableStructure.get())
{
    // TSAN: see the first constructor — relaxed stores even during construction.
    concurrentRelaxedStore(m_indexSize, other.indexSize());
    concurrentRelaxedStore(m_indexMask, other.indexMask());
    publishIndexVector(allocateIndexVector(other.isCompact(), other.indexSize()));
    concurrentRelaxedStore(m_keyCount, other.keyCount());
    concurrentRelaxedStore(m_deletedCount, other.deletedCount());
    ASSERT(isPowerOfTwo(m_indexSize));
    ASSERT(isCompact() == other.isCompact());
    memcpy(std::bit_cast<void*>(m_indexVector & indexVectorMask), std::bit_cast<void*>(other.indexVector() & indexVectorMask), dataSize(isCompact()));

    forEachProperty([&](auto& entry) {
        entry.key()->ref();
        return IterationStatus::Continue;
    });

    copyDeletedOffsetsFrom(other);
}

// SPEC-objectmodel §6 (Task 9): clones inherit the Reusable list, the
// Quarantined list with its stamps verbatim (the slots they describe are
// copied along with the table) and the cached epoch slot - clones live in the
// same server heap (Structures never migrate across heaps), so the slot stays
// correct. Replaced index vectors are NOT inherited: the clone allocated its
// own vector, and the source's quarantine frees them when the source dies.
void PropertyTable::copyDeletedOffsetsFrom(const PropertyTable& other)
{
    unsigned deletedOffsetCount = 0;
    const DeletedOffsets* otherDeletedOffsets = other.m_deletedOffsets.get();
    if (otherDeletedOffsets && !(otherDeletedOffsets->reusable.isEmpty() && otherDeletedOffsets->quarantined.isEmpty())) {
        DeletedOffsets& deletedOffsets = ensureDeletedOffsets();
        deletedOffsets.reusable = otherDeletedOffsets->reusable;
        deletedOffsets.quarantined = otherDeletedOffsets->quarantined;
        concurrentRelaxedStore(deletedOffsets.epochSlot, concurrentRelaxedLoad(otherDeletedOffsets->epochSlot));
        deletedOffsetCount = deletedOffsets.reusable.size() + deletedOffsets.quarantined.size();
    }
    // TSAN: relaxed store even during construction (see the first constructor).
    concurrentRelaxedStore(m_deletedOffsetCount, deletedOffsetCount);
}

PropertyTable::PropertyTable(VM& vm, unsigned initialCapacity, const PropertyTable& other)
    : JSCell(vm, vm.propertyTableStructure.get())
{
    // TSAN: see the first constructor — relaxed stores even during construction.
    concurrentRelaxedStore(m_indexSize, sizeForCapacity(initialCapacity));
    concurrentRelaxedStore(m_indexMask, indexSize() - 1);
    concurrentRelaxedStore(m_keyCount, 0u);
    concurrentRelaxedStore(m_deletedCount, 0u);
    ASSERT(isPowerOfTwo(indexSize()));
    ASSERT(initialCapacity >= other.keyCount());
    bool isCompact = other.isCompact() && tableCapacity() < UINT8_MAX;
    publishIndexVector(allocateZeroedIndexVector(isCompact, indexSize()));
    ASSERT(this->isCompact() == isCompact);

    withIndexVector([&](auto* vector) {
        auto* table = tableFromIndexVector(vector);
        other.forEachProperty([&](auto& entry) {
            ASSERT(canInsert(entry));
            reinsert(vector, table, entry);
            entry.key()->ref();
            return IterationStatus::Continue;
        });
    });

    copyDeletedOffsetsFrom(other);
}

void PropertyTable::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    vm.heap.reportExtraMemoryAllocated(this, dataSize(isCompact()));
}

template<typename Visitor>
void PropertyTable::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PropertyTable>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(cell, visitor);
    visitor.reportExtraMemoryVisited(thisObject->dataSize(thisObject->isCompact()));
}

DEFINE_VISIT_CHILDREN(PropertyTable);

void PropertyTable::destroy(JSCell* cell)
{
    static_cast<PropertyTable*>(cell)->PropertyTable::~PropertyTable();
}

PropertyTable::~PropertyTable()
{
    forEachProperty([&](auto& entry) {
        entry.key()->deref();
        return IterationStatus::Continue;
    });
    destroyIndexVector(indexVector());
    // T3 (flag-on): replaced vectors still in quarantine die with the table.
    // Safe even against lock-free probes: the cell is only swept once
    // unreachable, and any probing mutator holds the table pointer in a
    // register/stack slot the conservative scan roots.
    if (m_deletedOffsets) {
        for (const QuarantinedIndexVector& quarantined : m_deletedOffsets->quarantinedIndexVectors)
            destroyIndexVector(quarantined.indexVector);
    }
}

void PropertyTable::seal()
{
    // T3: wholesale in-place attribute edit. The only caller
    // (Structure::nonPropertyTransitionSlow) runs it on a freshly cloned or
    // materialized table pinned to a transition that is not yet published
    // (it enters the source's transition table, or stays unreachable for a
    // dictionary source, only afterwards), so no lock-free probe can see the
    // plain stores below. The seqlock bracket is kept so every wholesale
    // editor stays bracketed (see renumberPropertyOffsets); it is cheap.
    beginConcurrentEdit();
    forEachPropertyMutable([&](auto& entry) {
        if (!PropertyName(entry.key()).isPrivateName())
            entry.setAttributes(entry.attributes() | static_cast<unsigned>(PropertyAttribute::DontDelete));
        return IterationStatus::Continue;
    });
    bumpConcurrentEditCount();
}

void PropertyTable::freeze()
{
    // T3: see seal() above.
    beginConcurrentEdit();
    forEachPropertyMutable([&](auto& entry) {
        if (!PropertyName(entry.key()).isPrivateName()) {
            if (!(entry.attributes() & PropertyAttribute::Accessor))
                entry.setAttributes(entry.attributes() | static_cast<unsigned>(PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
            else
                entry.setAttributes(entry.attributes() | static_cast<unsigned>(PropertyAttribute::DontDelete));
        }
        return IterationStatus::Continue;
    });
    bumpConcurrentEditCount();
}

bool PropertyTable::isSealed() const
{
    bool result = true;
    forEachProperty([&](const auto& entry) {
        if (!PropertyName(entry.key()).isPrivateName() && (entry.attributes() & PropertyAttribute::DontDelete) != static_cast<unsigned>(PropertyAttribute::DontDelete)) {
            result = false;
            return IterationStatus::Done;
        }
        return IterationStatus::Continue;
    });
    return result;
}

bool PropertyTable::isFrozen() const
{
    bool result = true;
    forEachProperty([&](const auto& entry) {
        if (!PropertyName(entry.key()).isPrivateName()) {
            if (!(entry.attributes() & PropertyAttribute::DontDelete)) {
                result = false;
                return IterationStatus::Done;
            }
            if (!(entry.attributes() & (PropertyAttribute::ReadOnly | PropertyAttribute::Accessor))) {
                result = false;
                return IterationStatus::Done;
            }
        }
        return IterationStatus::Continue;
    });
    return result;
}

// Cache the OWNING server heap's epoch slot at first use (§6). Heap::heap(this)
// is the server heap this PropertyTable cell lives in - with a shared GC
// server, every client VM of that server maps to the same slot, which is
// exactly the r13 per-server-heap keying. The cached pointer is written under
// the table's serialization but read lock-free elsewhere (stable address) -
// relaxed accesses (§3.25). The registry lock inside
// butterflyQuarantineEpochSlot() is a leaf under the caller's serialization.
WTF::Atomic<uint64_t>& PropertyTable::ensureQuarantineEpochSlot(DeletedOffsets& deletedOffsets)
{
    WTF::Atomic<uint64_t>* epochSlot = concurrentRelaxedLoad(deletedOffsets.epochSlot);
    if (!epochSlot) {
        epochSlot = &butterflyQuarantineEpochSlot(*Heap::heap(this));
        concurrentRelaxedStore(deletedOffsets.epochSlot, epochSlot);
    }
    return *epochSlot;
}

// SPEC-objectmodel §6 (Task 9): quarantine a deleted offset (inline or
// out-of-line). Caller context: reached from Structure::remove
// (StructureInlines.h) / the materialize replay (Structure.cpp) via
// addDeletedOffset; the table mutation holds the Structure's m_lock or the
// table is still thread-private (L6). Nothing here allocates in the GC heap
// (O1 - Vector growth is fastMalloc).
void PropertyTable::quarantineDeletedOffset(PropertyOffset offset)
{
    ASSERT(Options::useJSThreads());
    DeletedOffsets& deletedOffsets = ensureDeletedOffsets();
    WTF::Atomic<uint64_t>& epochSlot = ensureQuarantineEpochSlot(deletedOffsets);
    // Stamp = the heap's epoch AT deletion. Promotion requires stamp <
    // current, i.e. at least one full world-stopped window (one epoch bump)
    // strictly after this point - which flushes every reader that could hold
    // a stale offset/slot pointer (I18, with I34's no-poll rule). The epoch is
    // monotone and appends are serialized, so the list stays sorted by stamp.
    deletedOffsets.quarantined.append(QuarantinedDeletedOffset { offset, epochSlot.load(std::memory_order_seq_cst) });
    // TSAN family 25 quarantine counters: keep the relaxed mirror in sync
    // under the same serialization the list edit holds.
    concurrentRelaxedStore(m_deletedOffsetCount, deletedOffsetCount() + 1);
}

// T3 (flag-on): deferred free of a replaced index vector. A lock-free probe
// (Structure::getConcurrently fast path / PropertyTable::findConcurrently)
// may have loaded m_indexVector immediately before rehash swapped it and may
// still be walking the old allocation; probes never poll safepoints, so a
// crossed owning-heap epoch (bumped only while the world is stopped) proves
// no probe can still hold the pointer — the identical I18/I34 argument the
// deleted-offset quarantine above rests on. Runs under the caller's table
// serialization (m_lock or table-private, L6); the epoch-slot registry lock
// is a leaf under it; Vector growth is fastMalloc (O1-clean).
void PropertyTable::quarantineIndexVector(uintptr_t indexVector)
{
    ASSERT(Options::useJSThreads());
    DeletedOffsets& deletedOffsets = ensureDeletedOffsets();
    uint64_t currentEpoch = ensureQuarantineEpochSlot(deletedOffsets).load(std::memory_order_seq_cst);

    // Opportunistic sweep: anything stamped strictly before the current
    // epoch has had a full world-stopped window since it was unpublished.
    // This bounds the list to the vectors retired since the last epoch bump
    // (geometric sizes, so memory overhead stays within ~1x of the live
    // vector); the destructor frees whatever remains.
    deletedOffsets.quarantinedIndexVectors.removeAllMatching([&](const QuarantinedIndexVector& quarantined) {
        if (quarantined.epoch >= currentEpoch)
            return false; // No epoch bump since it was unpublished yet (I18).
        destroyIndexVector(quarantined.indexVector);
        return true;
    });
    deletedOffsets.quarantinedIndexVectors.append(QuarantinedIndexVector { indexVector, currentEpoch });
}

// SPEC-objectmodel §9.4 (frozen): promote quarantined offsets whose stamp
// predates the owning heap's current epoch onto the Reusable list (§6 "lazy
// promotion"; takeDeletedOffset draws only from Reusable). Runs under the
// caller's table serialization (m_lock or table-private, L6). The mirror
// m_deletedOffsetCount counts both lists, so promotion leaves it unchanged.
void PropertyTable::releaseQuarantinedSlots(uint64_t currentEpoch)
{
    ASSERT(Options::useJSThreads());
    if (!m_deletedOffsets)
        return;
    Vector<QuarantinedDeletedOffset>& quarantined = m_deletedOffsets->quarantined;
    // Stamps are appended in non-decreasing epoch order and removal preserves
    // order, so an unpromotable first entry means nothing is promotable; this
    // keeps an add after N deletes O(1) instead of rescanning all N stamps
    // until the next collection bumps the epoch.
    if (quarantined.isEmpty() || quarantined.first().epoch >= currentEpoch)
        return;
    Vector<PropertyOffset>& reusable = m_deletedOffsets->reusable;
    quarantined.removeAllMatching([&](QuarantinedDeletedOffset& entry) {
        if (entry.epoch >= currentEpoch)
            return false; // No epoch bump since the deletion yet (I18).
        ASSERT(!reusable.contains(entry.offset));
        reusable.append(entry.offset);
        return true;
    });
}

PropertyOffset PropertyTable::renumberPropertyOffsets(JSObject* object, unsigned inlineCapacity, Vector<JSValue>& values)
{
    // T3: flag-on this only runs under the §10.6 stop (flattenDictionary-
    // Structure bails otherwise), so no lock-free probe can be in flight —
    // but bracket the in-place offset rewrite anyway: it is cheap, keeps the
    // "every probe-visible mutation is bracketed" invariant auditable, and
    // protects any future caller that is not under a stop.
    beginConcurrentEdit();
    ASSERT(values.size() == size());
    unsigned i = 0;
    PropertyOffset offset = invalidOffset;
    forEachPropertyMutable([&](auto& entry) {
        values[i] = object->getDirect(entry.offset());
        offset = offsetForPropertyNumber(i, inlineCapacity);
        entry.setOffset(offset);
        ++i;
        return IterationStatus::Continue;
    });
    clearDeletedOffsets();
    bumpConcurrentEditCount(); // T3: close the bracket opened above.
    return offset;
}

template<typename Functor>
inline void PropertyTable::forEachPropertyMutable(const Functor& functor)
{
    withIndexVector([&](auto* vector) {
        auto* cursor = tableFromIndexVector(vector);
        auto* end = tableEndFromIndexVector(vector);
        for (; cursor != end; ++cursor) {
            if (cursor->key() == PROPERTY_MAP_DELETED_ENTRY_KEY)
                continue;
            if (functor(*cursor) == IterationStatus::Done)
                return;
        }
    });
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
