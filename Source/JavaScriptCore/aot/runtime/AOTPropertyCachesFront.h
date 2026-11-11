/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTPropertyCachesWords.h"
#include "CollectionScope.h"
#include "RememberedSlot.h"
#include "Structure.h"
#include <algorithm>
#include <bit>
#include <memory>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/TZoneMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class CacheRegistry;
class CustomAccessorTable;

class StructureRows {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(StructureRows);
    WTF_MAKE_NONCOPYABLE(StructureRows);
public:
    static StructureRows& singleton();

    uint32_t rowOf(StructureID id) const { return *wordFor(id); }
    void setRowOf(StructureID id, uint32_t row) { *wordFor(id) = row; }
    const uint32_t* rowsForStructureIDZero() const { return m_rowsForStructureIDZero; }

private:
    StructureRows();

    uint32_t* wordFor(StructureID id) const
    {
        ASSERT(id && !id.isNuked());
        return m_rowsForStructureIDZero + (id.bits() >> StructureRowWords::granuleShift);
    }

    uint32_t* m_rowsForStructureIDZero { nullptr };
};

static_assert(sizeof(Structure) >= 1u << StructureRowWords::granuleShift);

class RowDisplacementTable {
    WTF_MAKE_NONCOPYABLE(RowDisplacementTable);
    WTF_MAKE_TZONE_ALLOCATED(RowDisplacementTable);
public:
    static constexpr uint32_t numberOfColumns = UINT16_MAX;
    static constexpr uint32_t maxColumn = numberOfColumns - 1;
    static constexpr uint32_t maxNumberOfBases = 1u << 21;
    static constexpr uint32_t initialNumberOfBases = 1u << 15;
    static constexpr uint32_t rowMarkerColumn = 0;
    static constexpr uint16_t rowMarkerTag = UINT16_MAX;
    static constexpr unsigned regionShift = 12;
    static constexpr uint32_t numberOfHotNames = 1u << 11;
    static constexpr uint32_t hotNameMultiplier = 1779;
    static size_t sizeOfPagesAtStart(uint64_t valueOfLimitOfBases);

    static constexpr bool hasColumn(uint32_t identifierNumber) { return identifierNumber != rowMarkerColumn && identifierNumber <= maxColumn; }
    static constexpr uint32_t numberForRank(uint32_t rankByUse, uint32_t numberOfNames)
    {
        uint32_t numberOfScatteredNames = std::min(std::bit_floor(numberOfNames + 1), numberOfHotNames);
        return rankByUse < numberOfScatteredNames ? rankByUse * hotNameMultiplier % numberOfScatteredNames : rankByUse;
    }

    RowDisplacementTable(VM&, CacheRegistry&, const CustomAccessorTable&);
    ~RowDisplacementTable();

    const PackedSlot* plane(SlotAccess access) const { return &m_planes[static_cast<unsigned>(access)]->get(); }
    bool takeDidMovePlanes() { return std::exchange(m_didMovePlanes, false); }

    bool trySet(Structure*, uint32_t column, SlotAccess, const Slot&);
    std::optional<Slot> find(StructureID, uint32_t column, SlotAccess) const;
    void remove(StructureID, uint32_t column, SlotAccess);

    bool hasChainDependentCell(StructureID, uint32_t column) const;
    size_t removeChainDependentCells(StructureID, uint32_t column);

    bool hasRow(StructureID id) const { return !!baseOf(id); }
    void releaseRowOf(StructureID);
    void clear();

    size_t removeDeadEntries(VM&, CollectionScope);

    template<typename Functor> void forEachSlot(const Functor&) const;

private:
    class RowColumns {
        WTF_MAKE_NONCOPYABLE(RowColumns);
    public:
        RowColumns() = default;
        ~RowColumns() { clear(); }
        unsigned size() const { return m_size; }
        std::span<const uint16_t> span() const { return { const_cast<RowColumns*>(this)->storage(), m_size }; }
        const uint16_t* begin() const { return span().data(); }
        const uint16_t* end() const { return span().data() + m_size; }
        bool tryMakeRoomForOneMore();
        bool tryAppend(uint16_t);
        void clear();

    private:
        static constexpr uint16_t inlineCapacity = sizeof(uint16_t*) / sizeof(uint16_t);
        uint16_t* storage()
        {
            if (m_capacity == inlineCapacity)
                return m_storage;
            uint16_t* result;
            memcpy(&result, m_storage, sizeof(result));
            return result;
        }

        uint16_t m_size { 0 };
        uint16_t m_capacity { inlineCapacity };
        uint16_t m_storage[inlineCapacity] { };
    };

    struct Row {
        StructureID owner;
        RowColumns columns;
    };
    static_assert(sizeof(Row) == 16);

    class Rows {
        WTF_MAKE_NONCOPYABLE(Rows);
    public:
        Rows() = default;
        ~Rows();
        uint32_t size() const { return m_size; }
        Row& operator[](uint32_t number) { ASSERT(number < m_size); return m_segments[number / rowsInSegment][number % rowsInSegment]; }
        const Row& operator[](uint32_t number) const { ASSERT(number < m_size); return m_segments[number / rowsInSegment][number % rowsInSegment]; }
        bool tryAlloc();

    private:
        static constexpr uint32_t rowsInSegment = 128;
        Vector<Row*> m_segments;
        uint32_t m_size { 0 };
    };

    class Bits {
    public:
        size_t size() const { return m_words.size() * 64; }
        bool tryMakeRoomFor(size_t number);
        bool get(size_t number) const { return number < size() && (m_words[number / 64] >> (number % 64) & 1); }
        void set(size_t number) { ASSERT(number < size()); m_words[number / 64] |= 1ull << (number % 64); }
        void clear(size_t number)
        {
            if (number < size())
                m_words[number / 64] &= ~(1ull << (number % 64));
        }
        void clearAll() { zeroSpan(m_words.mutableSpan()); }
        template<typename Functor> void forEachSetBit(const Functor& functor) const
        {
            for (size_t word = 0; word < m_words.size(); ++word) {
                for (uint64_t left = m_words[word]; left; left &= left - 1)
                    functor(word * 64 + std::countr_zero(left));
            }
        }

    private:
        Vector<uint64_t> m_words;
    };

    struct RecentRow {
        uint16_t column { 0 };
        uint32_t row { 0 };
    };

    const PackedSlot& cellAt(SlotAccess access, uint32_t index) const { return m_planes[static_cast<unsigned>(access)][index].get(); }
    const PackedSlot& reservationAt(uint32_t index) const { return cellAt(SlotAccess::Read, index); }
    PackedSlot emptyCellNamed(uint16_t column) const { return *PackedSlot::tryPack(Slot::createWithoutPayload(StructureID(), Slot::Kind::Empty, 0), column, m_cageBase); }
    void writeCell(SlotAccess, uint32_t index, PackedSlot, bool dependsOnChain);
    bool dependsOnChain(SlotAccess access, uint32_t index) const { return m_dependsOnChain[static_cast<unsigned>(access)].get(index); }

    uint32_t baseOf(StructureID id) const { return m_structureRows.rowOf(id); }
    uint32_t indexOfReservedCell(StructureID, uint32_t column) const;
    Row& rowAt(uint32_t base) { return m_rows[reservationAt(base).payload]; }
    uint32_t baseOfOccupant(uint32_t index) const { return reservationAt(index).name == rowMarkerTag ? index : index - reservationAt(index).name; }

    uint64_t freeCellsFrom(uint32_t index) const;
    uint32_t firstFittingBase(uint32_t from, std::span<const uint16_t> columns) const;
    uint32_t findBase(std::span<const uint16_t> columns, uint32_t preferredBase = 0);
    uint32_t findBaseEvictingRows(std::span<const uint16_t> columns, StructureID keep);
    void compactBases();
    bool tryGrow();
    void reserve(uint32_t index, PackedSlot);
    void vacate(uint32_t index);
    bool tryAddRow();
    uint32_t createRow(Structure*, uint16_t column);
    uint32_t moveRow(uint32_t base, std::optional<uint16_t> columnToAdd);
    uint32_t ensureIndex(Structure*, uint16_t column);
    void release(uint32_t base);
    void clearCell(SlotAccess, uint32_t index);
    bool canHaveCellAt(SlotAccess access, uint32_t index) const { return m_writtenRegions[static_cast<unsigned>(access)]->cards()[index * sizeof(PackedSlot) >> regionShift] != RememberedSlotSet::neverWrittenCard; }
    size_t sweepRow(VM&, uint32_t rowNumber);

    static constexpr uint32_t firstBase = 1;

    VM& m_vm;
    const CustomAccessorTable& m_customAccessors;
    StructureRows& m_structureRows;
    uintptr_t m_cageBase { 0 };
    std::array<RememberedSlot<PackedSlot>*, numberOfSlotAccesses> m_planes { };
    uint64_t* m_occupied { nullptr };
    uint32_t m_limitOfBases { 0 };
    uint32_t m_capacityOfBases { 0 };
    std::array<uint32_t, numberOfSlotAccesses + 1> m_numbersOfBasesWithPages { };
    bool m_didMovePlanes { false };
    uint32_t m_nextBase { firstBase };
    uint32_t m_endOfBases { firstBase };
    Rows m_rows;
    Vector<uint32_t> m_freeRows;
    Bits m_rowsThatCanOutliveWhatTheyName;
    Bits m_rowsWrittenSinceLastSweep;
    std::array<RecentRow, 256> m_recentRows { };
    std::array<Bits, numberOfSlotAccesses> m_dependsOnChain;
    std::array<std::unique_ptr<RememberedSlotSet>, numberOfSlotAccesses> m_writtenRegions;
    uint32_t m_evictionHand { firstBase };
};

static_assert(RowDisplacementTable::rowMarkerColumn == PackedSlot::noName && RowDisplacementTable::maxColumn < RowDisplacementTable::rowMarkerTag && RowDisplacementTable::rowMarkerTag <= PackedSlot::maxName);
static_assert(defaultLimitValue(LimitID::LookupTableNames, Architecture::ARM64) == RowDisplacementTable::maxColumn && defaultLimitValue(LimitID::LookupTableNames, Architecture::X86_64) == RowDisplacementTable::maxColumn);
static_assert(hasOneBitSet(RowDisplacementTable::initialNumberOfBases) && RowDisplacementTable::initialNumberOfBases <= RowDisplacementTable::maxNumberOfBases);
static_assert(RowDisplacementTable::hotNameMultiplier % 2 && !RowDisplacementTable::numberForRank(0, 5000));
static_assert(RowDisplacementTable::numberForRank(1, 5000) == RowDisplacementTable::hotNameMultiplier && RowDisplacementTable::numberForRank(2048, 5000) == 2048);
static_assert(RowDisplacementTable::numberForRank(3, 6) == 1 && RowDisplacementTable::numberForRank(4, 6) == 4);

template<typename Functor>
void RowDisplacementTable::forEachSlot(const Functor& functor) const
{
    for (unsigned number = 0; number < m_rows.size(); ++number) {
        const Row& row = m_rows[number];
        if (!row.owner)
            continue;
        for (uint16_t column : row.columns) {
            uint32_t index = baseOf(row.owner) + column;
            for (SlotAccess access : allSlotAccesses) {
                const PackedSlot& cell = cellAt(access, index);
                if (!cell.isEmpty() && cell.kind() != Slot::Kind::Empty)
                    functor(cell.unpack(row.owner, m_cageBase, dependsOnChain(access, index)), static_cast<uint32_t>(column), access);
            }
        }
    }
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
