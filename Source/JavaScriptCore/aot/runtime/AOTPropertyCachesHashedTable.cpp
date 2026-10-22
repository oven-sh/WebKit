/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesHashedTable.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTPropertyCachesCustomAccessors.h"
#include "AOTRuntimeCounters.h"
#include "JSCInlines.h"
#include <algorithm>
#include <array>
#include <bit>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#if OS(LINUX)
#include <errno.h>
#include <sys/mman.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(HashedSlotTable);

static constexpr std::array slotAccesses { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define };
static_assert(slotAccesses.size() == numberOfSlotAccesses);

static constexpr size_t bytesPerSet = HashedSlot::ways * sizeof(HashedSlot);
static constexpr uint64_t maxAddressableEntries = 1ull << (32 - HashedSlot::sizeShift);
static_assert(hasOneBitSet(HashedSlotTable::initialNumberOfEntries) && HashedSlotTable::initialNumberOfEntries >= HashedSlot::ways);
static_assert(!((1u << HashedSlotTable::regionShift) % bytesPerSet));

#if OS(LINUX)
#if defined(MADV_POPULATE_WRITE)
static constexpr int populateForWriting = MADV_POPULATE_WRITE;
#else
static constexpr int populateForWriting = 23;
#endif
#endif

static uint32_t maxNumberOfEntriesFor(VM& vm)
{
    uint64_t limit = std::clamp<uint64_t>(runtimeLimitValue(vm, LimitID::HashedTableEntries), HashedSlotTable::initialNumberOfEntries, maxAddressableEntries);
    return static_cast<uint32_t>(std::bit_floor(limit));
}

static size_t sizeOfReservation(uint32_t maxNumberOfEntries)
{
    return roundUpToMultipleOf(pageSize(), static_cast<size_t>(maxNumberOfEntries) * sizeof(HashedSlot));
}

HashedSlotTable::HashedSlotTable(VM& vm, CacheRegistry& registry, const CustomAccessorTable& customAccessors)
    : m_vm(vm)
    , m_customAccessors(customAccessors)
    , m_maxNumberOfEntries(maxNumberOfEntriesFor(vm))
    , m_numberOfEntries(initialNumberOfEntries)
    , m_entries(reserveEntries(m_maxNumberOfEntries))
    , m_writtenRegions(asByteSpan(allEntries()), regionShift)
{
    registry.add(CacheKind::WeakByKey, "HashedSlotTable"_s, *this);
}

HashedSlotTable::~HashedSlotTable()
{
    clear();
    OSAllocator::decommitAndRelease(m_entries, sizeOfReservation(m_maxNumberOfEntries));
}

auto HashedSlotTable::reserveEntries(uint32_t maxNumberOfEntries) -> Entry*
{
    size_t size = sizeOfReservation(maxNumberOfEntries);
    void* entries = OSAllocator::reserveAndCommit(size, OSAllocator::FastMallocPages);
#if OS(LINUX)
    while (madvise(entries, size, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#endif
    return static_cast<Entry*>(entries);
}

auto HashedSlotTable::findEntry(StructureID structureID, UniquedStringImpl* uid, SlotAccess access) const -> Entry*
{
    uint32_t offsetOfFirstWay = HashedSlot::offsetOfFirstWay(structureID, uid, offsetMask());
    uintptr_t key = HashedSlot::keyFor(uid, access);
    for (unsigned way = 0; way < HashedSlot::ways; ++way) {
        Entry* entry = entryAt(HashedSlot::offsetOfWay(offsetOfFirstWay, way));
        if (entry->get().matches(structureID, key))
            return entry;
    }
    return nullptr;
}

auto HashedSlotTable::findEmptyEntry(StructureID structureID, UniquedStringImpl* uid) const -> Entry*
{
    uint32_t offsetOfFirstWay = HashedSlot::offsetOfFirstWay(structureID, uid, offsetMask());
    for (unsigned way = 0; way < HashedSlot::ways; ++way) {
        Entry* entry = entryAt(HashedSlot::offsetOfWay(offsetOfFirstWay, way));
        if (entry->get().isEmpty())
            return entry;
    }
    return nullptr;
}

auto HashedSlotTable::chooseEntry(StructureID structureID, UniquedStringImpl* uid, SlotAccess access) -> Entry&
{
    if (Entry* entry = findEntry(structureID, uid, access))
        return *entry;
    if (Entry* entry = findEmptyEntry(structureID, uid))
        return *entry;
    uint32_t offsetOfFirstWay = HashedSlot::offsetOfFirstWay(structureID, uid, offsetMask());
    Entry& victim = *entryAt(HashedSlot::offsetOfWay(offsetOfFirstWay, m_nextVictimWay++ % HashedSlot::ways));
    evict(victim);
    return victim;
}

void HashedSlotTable::write(Entry& entry, UniquedStringImpl* uid, SlotAccess access, const Slot& slot)
{
    ASSERT(!(std::bit_cast<uintptr_t>(uid) & HashedSlot::accessMask));
    uintptr_t key = HashedSlot::keyFor(uid, access);
    if (entry.get().key != key) {
        ASSERT(entry.get().isEmpty());
        uid->ref();
        ++m_numberOfFilledEntries;
    }
    entry.set(m_writtenRegions, HashedSlot { slot, key });
}

void HashedSlotTable::erase(Entry& entry)
{
    UniquedStringImpl* uid = entry.get().uid();
    entry.clear();
    --m_numberOfFilledEntries;
    uid->deref();
}

void HashedSlotTable::evict(Entry& entry)
{
    // A set that overflows now and then while most of the table is empty is bad luck, and twice the memory is too much to pay for it: over the life of the table only what it evicts while it is at least half full counts toward its growth.
    ++m_evictionsSinceCollection;
    if (isAtLeastHalfFull())
        ++m_evictionsWhileHalfFull;
    erase(entry);
    count(m_vm, RuntimeCounterID::HashedTableEvictions);
    if (m_numberOfEntries == m_maxNumberOfEntries)
        countLimitHit(m_vm, LimitID::HashedTableEntries);
}

void HashedSlotTable::eraseDuringCollection(Entry& entry)
{
    // Heap::pruneDeadReferences clears the thread's atom table, so the last reference of an atom cannot be dropped here.
    m_namesErasedDuringCollection.append(entry.get().uid());
    entry.clear();
    --m_numberOfFilledEntries;
}

void HashedSlotTable::releaseNamesErasedDuringCollection()
{
    for (UniquedStringImpl* uid : m_namesErasedDuringCollection)
        uid->deref();
    m_namesErasedDuringCollection.clear();
}

HashedSlotTable::SetResult HashedSlotTable::set(UniquedStringImpl* uid, SlotAccess access, const Slot& slot)
{
    releaseNamesErasedDuringCollection();
    ASSERT(uid);
    ASSERT(slot.structureID);
    write(chooseEntry(slot.structureID, uid, access), uid, access, slot);
    if (!shouldGrow())
        return SetResult::Stored;
    grow();
    return SetResult::StoredAndGrew;
}

bool HashedSlotTable::shouldGrow() const
{
    return m_numberOfEntries < m_maxNumberOfEntries && (m_evictionsWhileHalfFull > m_numberOfEntries / 64 || m_evictionsSinceCollection > m_numberOfEntries / 4);
}

// The mask of a table of twice the size has one more bit.
void HashedSlotTable::grow()
{
    std::span<Entry> lowerHalf = allEntries();
    [[maybe_unused]] bool fillsEveryPage = isAtLeastHalfFull();
    m_numberOfEntries *= 2;
    m_writtenRegions.didMoveTable(asByteSpan(allEntries()));
#if OS(LINUX)
    // Of a table that is at least half full every page of the upper half gets entries now: one call into the kernel takes the place of a fault per page.
    if (fillsEveryPage)
        madvise(lowerHalf.data() + lowerHalf.size(), lowerHalf.size_bytes(), populateForWriting);
#endif
    uint32_t bitOfUpperHalf = static_cast<uint32_t>(lowerHalf.size_bytes());
    for (Entry& entry : lowerHalf) {
        const HashedSlot& filed = entry.get();
        if (filed.isEmpty() || !(HashedSlot::offsetOfFirstWay(filed.slot.structureID, filed.uid(), offsetMask()) & bitOfUpperHalf))
            continue;
        Entry& destination = *(&entry + lowerHalf.size());
        ASSERT(destination.get().isEmpty());
        destination.set(m_writtenRegions, filed);
        entry.clear();
    }
    m_evictionsWhileHalfFull = 0;
    m_evictionsSinceCollection = 0;
    count(m_vm, RuntimeCounterID::HashedTableGrowths);
}

const Slot* HashedSlotTable::find(StructureID structureID, UniquedStringImpl* uid, SlotAccess access) const
{
    Entry* entry = findEntry(structureID, uid, access);
    return entry ? &entry->get().slot : nullptr;
}

void HashedSlotTable::remove(StructureID structureID, UniquedStringImpl* uid, SlotAccess access)
{
    releaseNamesErasedDuringCollection();
    if (Entry* entry = findEntry(structureID, uid, access))
        erase(*entry);
}

bool HashedSlotTable::hasChainDependentEntry(StructureID structureID, UniquedStringImpl* uid) const
{
    for (SlotAccess access : slotAccesses) {
        Entry* entry = findEntry(structureID, uid, access);
        if (entry && entry->get().slot.has(Slot::dependsOnChain))
            return true;
    }
    return false;
}

size_t HashedSlotTable::removeChainDependentEntries(StructureID structureID, UniquedStringImpl* uid)
{
    releaseNamesErasedDuringCollection();
    size_t numberOfRemovedEntries = 0;
    for (SlotAccess access : slotAccesses) {
        Entry* entry = findEntry(structureID, uid, access);
        if (!entry || !entry->get().slot.has(Slot::dependsOnChain))
            continue;
        erase(*entry);
        ++numberOfRemovedEntries;
    }
    return numberOfRemovedEntries;
}

void HashedSlotTable::clear()
{
    releaseNamesErasedDuringCollection();
    m_writtenRegions.forEachRegionToClear([&](size_t beginOffset, size_t endOffset) {
        RememberedSlotSet::forEachEntryInRegion(allEntries(), beginOffset, endOffset, [&](Entry& entry) {
            if (!entry.get().isEmpty())
                erase(entry);
        });
    });
    ASSERT(!m_numberOfFilledEntries);
}

size_t HashedSlotTable::removeDeadEntries(VM& vm, CollectionScope scope)
{
    auto isMarked = [&](JSCell* cell) {
        return vm.heap.isMarked(cell);
    };
    uintptr_t cageBase = vm.heap.cage().base();
    auto isDead = [&](const Slot& slot) {
        if (!slot.namesOnlyLiveCells(isMarked, cageBase))
            return true;
        return Slot::payloadOf(slot.kind) == SlotPayload::CustomAccessor && !m_customAccessors.isInUse(slot.customAccessor.index);
    };
    m_evictionsSinceCollection = 0;
    size_t numberOfRemovedEntries = 0;
    m_writtenRegions.forEachRegionToSweep(scope, [&](size_t beginOffset, size_t endOffset) {
        RememberedSlotSet::forEachEntryInRegion(allEntries(), beginOffset, endOffset, [&](Entry& entry) {
            if (entry.get().isEmpty() || !isDead(entry.get().slot))
                return;
            eraseDuringCollection(entry);
            ++numberOfRemovedEntries;
        });
    });
    return numberOfRemovedEntries;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
