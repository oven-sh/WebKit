/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTPropertyCachesStructureRows.h"
#include "AOTPropertyCachesWords.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "RememberedSlot.h"
#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>
#include <wtf/TZoneMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class VM;

namespace AOT {

class CacheRegistry;
class CustomAccessorTable;

class RowDisplacementTable {
    WTF_MAKE_NONCOPYABLE(RowDisplacementTable);
    WTF_MAKE_TZONE_ALLOCATED(RowDisplacementTable);
public:
    static constexpr uint32_t numberOfColumns = 1u << 15;
    static constexpr uint32_t maxColumn = numberOfColumns - 1;
    static constexpr uint32_t maxNumberOfBases = 1u << 21;
    static constexpr uint32_t cellsPerPlane = maxNumberOfBases + numberOfColumns;
    static constexpr uint32_t rowMarkerColumn = 0;
    static constexpr uint16_t rowMarkerTag = UINT16_MAX;
    static constexpr unsigned regionShift = 12;
    static constexpr uint32_t numberOfHotNames = 1u << 11;
    static constexpr uint32_t hotNameMultiplier = 1779;

    static constexpr bool hasColumn(uint32_t identifierNumber) { return identifierNumber != rowMarkerColumn && identifierNumber <= maxColumn; }
    static constexpr uint32_t numberForRank(uint32_t rankByUse, uint32_t numberOfNames)
    {
        uint32_t numberOfScatteredNames = std::min(std::bit_floor(numberOfNames + 1), numberOfHotNames);
        return rankByUse < numberOfScatteredNames ? rankByUse * hotNameMultiplier % numberOfScatteredNames : rankByUse;
    }

    RowDisplacementTable(VM&, CacheRegistry&, const CustomAccessorTable&);
    ~RowDisplacementTable();

    const PackedSlot* plane(SlotAccess access) const { return &m_planes[static_cast<unsigned>(access)]->get(); }

    bool trySet(Structure*, uint32_t column, SlotAccess, const Slot&);
    std::optional<Slot> find(StructureID, uint32_t column, SlotAccess) const;
    void remove(StructureID, uint32_t column, SlotAccess);

    bool hasChainDependentCell(StructureID, uint32_t column) const;
    size_t removeChainDependentCells(StructureID, uint32_t column);

    bool hasRow(StructureID id) const { return !!m_structureRows.rowOf(id); }
    void releaseRowOf(StructureID);
    void clear();

    size_t removeDeadEntries(VM&, CollectionScope);

    template<typename Functor> void forEachSlot(const Functor&) const;

private:
    struct Bases {
        uint32_t first { 1 };
        uint32_t limit { 0 };
        uint32_t next { 1 };
        uint32_t end { 1 };
    };

    class RowColumns {
        WTF_MAKE_NONCOPYABLE(RowColumns);
    public:
        RowColumns() = default;
        ~RowColumns() { clear(); }
        unsigned size() const { return m_size; }
        std::span<const uint16_t> span() const { return { isInline() ? m_storage : outOfLineStorage(), m_size }; }
        const uint16_t* begin() const { return span().data(); }
        const uint16_t* end() const { return span().data() + m_size; }
        void append(uint16_t);
        void clear();

    private:
        static constexpr uint16_t inlineCapacity = sizeof(uint16_t*) / sizeof(uint16_t);
        bool isInline() const { return m_capacity == inlineCapacity; }
        uint16_t* outOfLineStorage() const
        {
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

    struct RecentRow {
        uint16_t column { 0 };
        uint32_t row { 0 };
    };

    static uint32_t indexOf(uint32_t base, uint32_t column) { return base + column; }
    static bool isRowMarker(const PackedSlot& cell) { return cell.name == rowMarkerTag; }

    const PackedSlot& cellAt(SlotAccess access, uint32_t index) const { return m_planes[static_cast<unsigned>(access)][index].get(); }
    const PackedSlot& reservationAt(uint32_t index) const { return cellAt(SlotAccess::Read, index); }
    void writeCell(SlotAccess, uint32_t index, PackedSlot);
    BitVector& dependsOnChain(SlotAccess access) { return m_dependsOnChain[static_cast<unsigned>(access)]; }
    const BitVector& dependsOnChain(SlotAccess access) const { return m_dependsOnChain[static_cast<unsigned>(access)]; }
    void setDependsOnChain(SlotAccess, uint32_t index, bool);

    uint32_t baseOf(StructureID id) const { return m_structureRows.rowOf(id); }
    uint32_t rowNumberAt(uint32_t base) const { return reservationAt(base).payload; }
    Row& rowAt(uint32_t base) { return m_rows[rowNumberAt(base)]; }
    const Row& rowAt(uint32_t base) const { return m_rows[rowNumberAt(base)]; }
    uint32_t baseOfOccupant(uint32_t index) const;
    bool isReservedFor(uint32_t index, uint32_t column) const { return reservationAt(index).name == column; }

    uint64_t freeCellsFrom(uint32_t index) const;
    uint32_t firstFittingBase(uint32_t from, std::span<const uint16_t> columns) const;
    uint32_t findBase(std::span<const uint16_t> columns, uint32_t preferredBase = 0);
    uint32_t findBaseEvictingRows(std::span<const uint16_t> columns, StructureID keep);
    void compactBases();
    void reserve(uint32_t index, uint16_t tag, uint32_t payload);
    void vacate(uint32_t index);
    uint32_t allocateRowNumberFor(StructureID);
    uint32_t createRow(Structure*, uint32_t column);
    uint32_t moveRow(uint32_t base, std::optional<uint16_t> columnToAdd);
    uint32_t ensureIndex(Structure*, uint32_t column);
    void release(uint32_t base);
    void clearCell(SlotAccess, uint32_t index);
    // The first access commits a page of a plane, so a region that was never written is not even read.
    bool canHaveCellAt(SlotAccess access, uint32_t index) const { return m_writtenRegions[static_cast<unsigned>(access)]->cards()[index * sizeof(PackedSlot) >> regionShift] != RememberedSlotSet::neverWrittenCard; }
    size_t sweepRow(VM&, uint32_t rowNumber);
    void decommitEmptyPagesOfPlanes();

    VM& m_vm;
    const CustomAccessorTable& m_customAccessors;
    StructureRows& m_structureRows;
    uintptr_t m_cageBase { 0 };
    std::array<RememberedSlot<PackedSlot>*, numberOfSlotAccesses> m_planes { };
    uint64_t* m_occupied { nullptr };
    Bases m_bases;
    SegmentedVector<Row, 128> m_rows;
    Vector<uint32_t> m_freeRows;
    BitVector m_rowsThatCanOutliveWhatTheyName;
    BitVector m_rowsWrittenSinceLastSweep;
    std::array<RecentRow, 256> m_recentRows { };
    std::array<BitVector, numberOfSlotAccesses> m_dependsOnChain;
    std::array<std::unique_ptr<RememberedSlotSet>, numberOfSlotAccesses> m_writtenRegions;
    uint32_t m_evictionHand { 1 };
};

static_assert(RowDisplacementTable::rowMarkerColumn == PackedSlot::noName && RowDisplacementTable::maxColumn < RowDisplacementTable::rowMarkerTag && RowDisplacementTable::rowMarkerTag <= PackedSlot::maxName);
static_assert(defaultLimitValue(LimitID::LookupTableNames, Architecture::ARM64) == RowDisplacementTable::maxColumn && defaultLimitValue(LimitID::LookupTableNames, Architecture::X86_64) == RowDisplacementTable::maxColumn);
static_assert(defaultLimitValue(LimitID::LookupTableBases, Architecture::ARM64) == RowDisplacementTable::maxNumberOfBases && defaultLimitValue(LimitID::LookupTableBases, Architecture::X86_64) == RowDisplacementTable::maxNumberOfBases);
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
        uint32_t base = baseOf(row.owner);
        for (uint16_t column : row.columns) {
            uint32_t index = indexOf(base, column);
            for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define }) {
                const PackedSlot& cell = cellAt(access, index);
                if (cell.isEmpty() || cell.kind() == Slot::Kind::Empty)
                    continue;
                functor(cell.unpack(row.owner, m_cageBase, dependsOnChain(access).get(index)), static_cast<uint32_t>(column), access);
            }
        }
    }
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
