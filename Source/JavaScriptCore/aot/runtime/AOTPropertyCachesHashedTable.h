/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyCachesWords.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "RememberedSlot.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class VM;

namespace AOT {

class CacheRegistry;
class CustomAccessorTable;

// Addresses for the largest table are reserved once, and the table grows in place: it never moves, no earlier table is left behind, and a page takes memory from the first entry that is written to it.
class HashedSlotTable {
    WTF_MAKE_NONCOPYABLE(HashedSlotTable);
    WTF_MAKE_TZONE_ALLOCATED(HashedSlotTable);
public:
    static constexpr uint32_t initialNumberOfEntries = 1u << 10;
    // One set. The entries that are written between two collections lie all over the table, and a young collection looks at every entry of a region that was written to.
    static constexpr unsigned regionShift = 7;

    HashedSlotTable(VM&, CacheRegistry&, const CustomAccessorTable&);
    ~HashedSlotTable();

    const HashedSlot* entries() const { return &m_entries->get(); }
    uint32_t numberOfEntries() const { return m_numberOfEntries; }
    uint32_t numberOfFilledEntries() const { return m_numberOfFilledEntries; }
    uint64_t offsetMask() const { return HashedSlot::offsetMaskFor(m_numberOfEntries); }

    enum class SetResult : uint8_t { Stored, StoredAndGrew };
    SetResult set(UniquedStringImpl*, SlotAccess, const Slot&);
    const Slot* find(StructureID, UniquedStringImpl*, SlotAccess) const;
    void remove(StructureID, UniquedStringImpl*, SlotAccess);

    bool hasChainDependentEntry(StructureID, UniquedStringImpl*) const;
    size_t removeChainDependentEntries(StructureID, UniquedStringImpl*);
    void clear();

    size_t removeDeadEntries(VM&, CollectionScope);

    template<typename Functor>
    void forEachEntry(const Functor& functor) const
    {
        for (const Entry& entry : allEntries()) {
            if (!entry.get().isEmpty())
                functor(entry.get());
        }
    }

private:
    using Entry = RememberedSlot<HashedSlot>;

    std::span<Entry> allEntries() const { return { m_entries, m_numberOfEntries }; }
    Entry* entryAt(uint32_t offset) const { return reinterpret_cast<Entry*>(reinterpret_cast<uint8_t*>(m_entries) + offset); }
    Entry* findEntry(StructureID, UniquedStringImpl*, SlotAccess) const;
    Entry* findEmptyEntry(StructureID, UniquedStringImpl*) const;
    Entry& chooseEntry(StructureID, UniquedStringImpl*, SlotAccess);
    void write(Entry&, UniquedStringImpl*, SlotAccess, const Slot&);
    void erase(Entry&);
    void evict(Entry&);
    void eraseDuringCollection(Entry&);
    void releaseNamesErasedDuringCollection();
    bool isAtLeastHalfFull() const { return m_numberOfFilledEntries >= m_numberOfEntries / 2; }
    bool shouldGrow() const;
    void grow();
    static Entry* reserveEntries(uint32_t maxNumberOfEntries);

    VM& m_vm;
    const CustomAccessorTable& m_customAccessors;
    uint32_t m_maxNumberOfEntries { 0 };
    uint32_t m_numberOfEntries { 0 };
    Entry* m_entries { nullptr };
    uint32_t m_numberOfFilledEntries { 0 };
    uint32_t m_evictionsWhileHalfFull { 0 };
    uint32_t m_evictionsSinceCollection { 0 };
    uint32_t m_nextVictimWay { 0 };
    Vector<UniquedStringImpl*> m_namesErasedDuringCollection;
    RememberedSlotSet m_writtenRegions;
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
