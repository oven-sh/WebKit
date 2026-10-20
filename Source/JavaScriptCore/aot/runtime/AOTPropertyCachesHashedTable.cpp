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
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(HashedSlotTable);

static constexpr std::array slotAccesses { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define };
static_assert(slotAccesses.size() == numberOfSlotAccesses);

static constexpr size_t bytesPerSet = HashedSlot::ways * sizeof(HashedSlot);
static constexpr uint64_t maxAddressableEntries = 1ull << (32 - HashedSlot::sizeShift);
static_assert(hasOneBitSet(HashedSlotTable::initialNumberOfEntries) && HashedSlotTable::initialNumberOfEntries >= HashedSlot::ways);
static_assert(!((1u << HashedSlotTable::regionShift) % bytesPerSet));

static uint32_t maxNumberOfEntriesFor(VM& vm)
{
    uint64_t limit = std::clamp<uint64_t>(runtimeLimitValue(vm, LimitID::HashedTableEntries), HashedSlotTable::initialNumberOfEntries, maxAddressableEntries);
    return static_cast<uint32_t>(std::bit_floor(limit));
}

HashedSlotTable::HashedSlotTable(VM& vm, CacheRegistry& registry, const CustomAccessorTable& customAccessors)
    : m_vm(vm)
    , m_customAccessors(customAccessors)
    , m_entries(allocateEntries(initialNumberOfEntries))
    , m_numberOfEntries(initialNumberOfEntries)
    , m_maxNumberOfEntries(maxNumberOfEntriesFor(vm))
    , m_writtenRegions(asByteSpan(allEntries()), regionShift)
{
    registry.add(CacheKind::WeakByKey, "HashedSlotTable"_s, *this);
}

HashedSlotTable::~HashedSlotTable()
{
    clear();
    freeEntries(m_entries);
}

auto HashedSlotTable::allocateEntries(uint32_t numberOfEntries) -> Entry*
{
    auto* entries = static_cast<Entry*>(fastAlignedMalloc(bytesPerSet, static_cast<size_t>(numberOfEntries) * sizeof(Entry)));
    zeroSpan(std::span { entries, numberOfEntries });
    return entries;
}

void HashedSlotTable::freeEntries(Entry* entries)
{
    fastFree(entries);
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
    }
    entry.set(m_writtenRegions, HashedSlot { slot, key });
}

void HashedSlotTable::erase(Entry& entry)
{
    UniquedStringImpl* uid = entry.get().uid();
    entry.clear();
    uid->deref();
}

void HashedSlotTable::evict(Entry& entry)
{
    erase(entry);
    count(m_vm, RuntimeCounterID::HashedTableEvictions);
    ++m_evictionsSinceGrowth;
    if (m_numberOfEntries == m_maxNumberOfEntries)
        countLimitHit(m_vm, LimitID::HashedTableEntries);
}

void HashedSlotTable::eraseDuringCollection(Entry& entry)
{
    // Heap::pruneDeadReferences clears the thread's atom table, so the last reference of an atom cannot be dropped here.
    m_namesErasedDuringCollection.append(entry.get().uid());
    entry.clear();
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
    return m_numberOfEntries < m_maxNumberOfEntries && m_evictionsSinceGrowth > m_numberOfEntries / 4;
}

void HashedSlotTable::grow()
{
    std::span<Entry> previousEntries = allEntries();
    m_numberOfEntries *= 2;
    m_entries = allocateEntries(m_numberOfEntries);
    m_writtenRegions.didMoveTable(asByteSpan(allEntries()));
    for (Entry& entry : previousEntries) {
        if (entry.get().isEmpty())
            continue;
        if (Entry* destination = findEmptyEntry(entry.get().slot.structureID, entry.get().uid()))
            destination->set(m_writtenRegions, entry.get());
        else
            evict(entry);
    }
    freeEntries(previousEntries.data());
    m_evictionsSinceGrowth = 0;
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
    for (Entry& entry : allEntries()) {
        if (!entry.get().isEmpty())
            erase(entry);
    }
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
