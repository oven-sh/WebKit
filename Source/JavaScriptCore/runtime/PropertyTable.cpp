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

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

DEFINE_ALLOCATOR_WITH_HEAP_IDENTIFIER(PropertyTable);

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
    // construction — the cell address may be GC-recycled while stale readers
    // still probe it with concurrentRelaxedLoad (the table itself stays
    // private until its Structure publishes it, L6).
    concurrentRelaxedStore(m_indexSize, sizeForCapacity(initialCapacity));
    concurrentRelaxedStore(m_indexMask, indexSize() - 1);
    concurrentRelaxedStore(m_keyCount, 0u);
    concurrentRelaxedStore(m_deletedCount, 0u);
    ASSERT(isPowerOfTwo(indexSize()));
    bool isCompact = tableCapacity() < UINT8_MAX;
    concurrentRelaxedStore(m_indexVector, allocateZeroedIndexVector(isCompact, indexSize()));
    ASSERT(isCompact == this->isCompact());
}

// TSAN family 25 (§3.25): the clone source `other` can be a published table
// (its in-place mutators hold the owning Structure's m_lock, which the clone
// path also holds or the table is private, L6) - read its header words via
// the relaxed accessors to pair correctly with its relaxed-store writers.
// The new table itself is private until its Structure publishes it (L6), so
// plain init-list stores into *this* are fine.
PropertyTable::PropertyTable(VM& vm, const PropertyTable& other)
    : JSCell(vm, vm.propertyTableStructure.get())
{
    // TSAN: see the first constructor — relaxed stores even during construction.
    concurrentRelaxedStore(m_indexSize, other.indexSize());
    concurrentRelaxedStore(m_indexMask, other.indexMask());
    concurrentRelaxedStore(m_indexVector, allocateIndexVector(other.isCompact(), other.indexSize()));
    concurrentRelaxedStore(m_keyCount, other.keyCount());
    concurrentRelaxedStore(m_deletedCount, other.deletedCount());
    ASSERT(isPowerOfTwo(m_indexSize));
    ASSERT(isCompact() == other.isCompact());
    memcpy(std::bit_cast<void*>(m_indexVector & indexVectorMask), std::bit_cast<void*>(other.indexVector() & indexVectorMask), dataSize(isCompact()));

    forEachProperty([&](auto& entry) {
        entry.key()->ref();
        return IterationStatus::Continue;
    });

    // Copy the m_deletedOffsets vector.
    Vector<PropertyOffset>* otherDeletedOffsets = other.m_deletedOffsets.get();
    if (otherDeletedOffsets)
        m_deletedOffsets = makeUnique<Vector<PropertyOffset>>(*otherDeletedOffsets);
    concurrentRelaxedStore(m_deletedOffsetCount, m_deletedOffsets ? unsigned(m_deletedOffsets->size()) : 0u);

    // SPEC-objectmodel §6 (Task 9): clones inherit the Quarantined list with
    // its stamps verbatim (the slots they describe are copied along with the
    // table) and the cached epoch slot - clones live in the same server heap
    // (Structures never migrate across heaps), so the slot stays correct.
    if (Vector<QuarantinedDeletedOffset>* otherQuarantined = other.m_quarantinedDeletedOffsets.get())
        m_quarantinedDeletedOffsets = makeUnique<Vector<QuarantinedDeletedOffset>>(*otherQuarantined);
    concurrentRelaxedStore(m_quarantinedDeletedOffsetCount, m_quarantinedDeletedOffsets ? unsigned(m_quarantinedDeletedOffsets->size()) : 0u);
    m_quarantineEpochSlot = concurrentRelaxedLoad(other.m_quarantineEpochSlot);
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
    concurrentRelaxedStore(m_indexVector, allocateZeroedIndexVector(isCompact, indexSize()));
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

    // Copy the m_deletedOffsets vector.
    Vector<PropertyOffset>* otherDeletedOffsets = other.m_deletedOffsets.get();
    if (otherDeletedOffsets)
        m_deletedOffsets = makeUnique<Vector<PropertyOffset>>(*otherDeletedOffsets);
    concurrentRelaxedStore(m_deletedOffsetCount, m_deletedOffsets ? unsigned(m_deletedOffsets->size()) : 0u);

    // SPEC-objectmodel §6 (Task 9): see the copy constructor above.
    if (Vector<QuarantinedDeletedOffset>* otherQuarantined = other.m_quarantinedDeletedOffsets.get())
        m_quarantinedDeletedOffsets = makeUnique<Vector<QuarantinedDeletedOffset>>(*otherQuarantined);
    concurrentRelaxedStore(m_quarantinedDeletedOffsetCount, m_quarantinedDeletedOffsets ? unsigned(m_quarantinedDeletedOffsets->size()) : 0u);
    m_quarantineEpochSlot = concurrentRelaxedLoad(other.m_quarantineEpochSlot);
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
}

void PropertyTable::seal()
{
    forEachPropertyMutable([&](auto& entry) {
        if (!PropertyName(entry.key()).isPrivateName())
            entry.setAttributes(entry.attributes() | static_cast<unsigned>(PropertyAttribute::DontDelete));
        return IterationStatus::Continue;
    });
}

void PropertyTable::freeze()
{
    forEachPropertyMutable([&](auto& entry) {
        if (!PropertyName(entry.key()).isPrivateName()) {
            if (!(entry.attributes() & PropertyAttribute::Accessor))
                entry.setAttributes(entry.attributes() | static_cast<unsigned>(PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
            else
                entry.setAttributes(entry.attributes() | static_cast<unsigned>(PropertyAttribute::DontDelete));
        }
        return IterationStatus::Continue;
    });
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

// SPEC-objectmodel §6 (Task 9): quarantine a deleted out-of-line offset.
// Caller context: reached from Structure::remove (StructureInlines.h) /
// the materialize replay (Structure.cpp) via addDeletedOffset; the table
// mutation holds the Structure's m_lock or the table is still thread-private
// (L6). The registry lock inside butterflyQuarantineEpochSlot() is a leaf
// under it (heap §6 ranking); nothing here allocates in the GC heap (O1 -
// Vector growth is fastMalloc).
void PropertyTable::quarantineDeletedOffset(PropertyOffset offset)
{
    ASSERT(Options::useJSThreads());
    // Inline and out-of-line offsets are both quarantined (objectmodel
    // review round 2, INTEGRATE-objectmodel.md §54).

    // Cache the OWNING server heap's epoch slot at first quarantine (§6).
    // Heap::heap(this) is the server heap this PropertyTable cell lives in -
    // with a shared GC server, every client VM of that server maps to the
    // same slot, which is exactly the r13 per-server-heap keying.
    // The cached pointer is written under the table's serialization but read
    // lock-free elsewhere (stable address) - relaxed accesses (§3.25).
    WTF::Atomic<uint64_t>* epochSlot = concurrentRelaxedLoad(m_quarantineEpochSlot);
    if (!epochSlot) {
        epochSlot = &butterflyQuarantineEpochSlot(*Heap::heap(this));
        concurrentRelaxedStore(m_quarantineEpochSlot, epochSlot);
    }

    if (!m_quarantinedDeletedOffsets)
        m_quarantinedDeletedOffsets = makeUnique<Vector<QuarantinedDeletedOffset>>();
    // Stamp = the heap's epoch AT deletion. Promotion requires stamp <
    // current, i.e. at least one full world-stopped window (one epoch bump)
    // strictly after this point - which flushes every reader that could hold
    // a stale offset/slot pointer (I18, with I34's no-poll rule).
    m_quarantinedDeletedOffsets->append(QuarantinedDeletedOffset { offset, epochSlot->load(std::memory_order_seq_cst) });
    // TSAN family 25 quarantine counters: keep the relaxed mirror in sync
    // under the same serialization the list edit holds.
    concurrentRelaxedStore(m_quarantinedDeletedOffsetCount, unsigned(m_quarantinedDeletedOffsets->size()));
}

// SPEC-objectmodel §9.4 (frozen): promote quarantined offsets whose stamp
// predates the owning heap's current epoch onto the Reusable list (§6 "lazy
// promotion"; takeDeletedOffset draws only from Reusable). Runs under the
// caller's table serialization (m_lock or table-private, L6).
void PropertyTable::releaseQuarantinedSlots(uint64_t currentEpoch)
{
    ASSERT(Options::useJSThreads());
    if (!m_quarantinedDeletedOffsets)
        return;
    m_quarantinedDeletedOffsets->removeAllMatching([&](QuarantinedDeletedOffset& entry) {
        if (entry.epoch >= currentEpoch)
            return false; // No epoch bump since the deletion yet (I18).
        if (!m_deletedOffsets)
            m_deletedOffsets = makeUnique<Vector<PropertyOffset>>();
        ASSERT(!m_deletedOffsets->contains(entry.offset));
        m_deletedOffsets->append(entry.offset);
        return true;
    });
    // TSAN family 25 quarantine counters: resync both relaxed mirrors after
    // promotion (runs under the caller's table serialization).
    concurrentRelaxedStore(m_quarantinedDeletedOffsetCount, unsigned(m_quarantinedDeletedOffsets->size()));
    concurrentRelaxedStore(m_deletedOffsetCount, m_deletedOffsets ? unsigned(m_deletedOffsets->size()) : 0u);
}

PropertyOffset PropertyTable::renumberPropertyOffsets(JSObject* object, unsigned inlineCapacity, Vector<JSValue>& values)
{
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
