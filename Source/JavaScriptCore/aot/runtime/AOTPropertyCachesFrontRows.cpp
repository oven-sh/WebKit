/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesFront.h"

#if ENABLE(AOT)

#include "AOTRuntimeCounters.h"
#include "JSCInlines.h"
#include <wtf/FastMalloc.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

uint64_t RowDisplacementTable::freeCellsFrom(uint32_t index) const
{
    uint32_t word = index / 64;
    uint32_t shift = index % 64;
    uint64_t occupied = m_occupied[word] >> shift;
    if (shift)
        occupied |= m_occupied[word + 1] << (64 - shift);
    return ~occupied;
}

uint32_t RowDisplacementTable::firstFittingBase(uint32_t from, std::span<const uint16_t> columns) const
{
    uint64_t fitting = freeCellsFrom(from);
    for (uint16_t column : columns) {
        if (!fitting)
            return 0;
        fitting &= freeCellsFrom(indexOf(from, column));
    }
    if (!fitting)
        return 0;
    uint32_t base = from + WTF::ctz(fitting);
    return base < m_bases.limit ? base : 0;
}

uint32_t RowDisplacementTable::findBase(std::span<const uint16_t> columns, uint32_t preferredBase)
{
    constexpr unsigned basesPerBatch = 64;
    constexpr unsigned maxProbesBetweenRows = 4;
    if (uint32_t base = preferredBase ? firstFittingBase(preferredBase, columns) : 0) {
        m_bases.end = std::max(m_bases.end, base + 1);
        return base;
    }
    uint32_t from = m_bases.next;
    for (unsigned probes = 0; probes < maxProbesBetweenRows && from < m_bases.end; ++probes, from += basesPerBatch) {
        if (uint32_t base = firstFittingBase(from, columns)) {
            m_bases.next = base + 1;
            m_bases.end = std::max(m_bases.end, base + 1);
            return base;
        }
    }
    m_bases.next = from < m_bases.end ? from : m_bases.first;
    for (from = m_bases.end; from < m_bases.limit; from += basesPerBatch) {
        if (uint32_t base = firstFittingBase(from, columns)) {
            m_bases.end = base + 1;
            return base;
        }
    }
    return 0;
}

uint32_t RowDisplacementTable::findBaseEvictingRows(std::span<const uint16_t> columns, StructureID keep)
{
    countLimitHit(m_vm, LimitID::LookupTableBases);
    Vector<uint32_t, 33> occupiedIndexes;
    // With a lowered limit a wide row can overlap itself at every base, so the hand goes around once.
    for (uint32_t basesLeft = m_bases.limit - m_bases.first; basesLeft; --basesLeft) {
        uint32_t base = m_evictionHand;
        m_evictionHand = base + 1 < m_bases.limit ? base + 1 : m_bases.first;
        occupiedIndexes.shrink(0);
        bool overlapsKeep = false;
        auto collect = [&](uint32_t index) {
            if (freeCellsFrom(index) & 1)
                return;
            occupiedIndexes.append(index);
            overlapsKeep |= rowAt(baseOfOccupant(index)).owner == keep;
        };
        collect(base);
        for (uint16_t column : columns)
            collect(indexOf(base, column));
        if (overlapsKeep)
            continue;
        for (uint32_t index : occupiedIndexes) {
            if (!(freeCellsFrom(index) & 1))
                release(baseOfOccupant(index));
        }
        m_bases.end = std::max(m_bases.end, base + 1);
        return base;
    }
    return 0;
}

void RowDisplacementTable::compactBases()
{
    uint32_t end = m_bases.first;
    for (unsigned number = 0; number < m_rows.size(); ++number) {
        if (StructureID owner = m_rows[number].owner)
            end = std::max(end, baseOf(owner) + 1);
    }
    m_bases.end = end;
    m_bases.next = m_bases.first;
}

void RowDisplacementTable::reserve(uint32_t index, uint16_t tag, uint32_t payload)
{
    ASSERT(reservationAt(index).isEmpty());
    m_occupied[index / 64] |= 1ull << (index % 64);
    if (tag == rowMarkerTag) {
        writeCell(SlotAccess::Read, index, PackedSlot { payload, 0, PackedSlot::ownInlineValueBits, rowMarkerTag });
        return;
    }
    ASSERT(hasColumn(tag) && !payload);
    writeCell(SlotAccess::Read, index, *PackedSlot::tryPack(Slot::createWithoutPayload(StructureID(), Slot::Kind::Empty, 0), tag, m_cageBase));
}

void RowDisplacementTable::vacate(uint32_t index)
{
    m_occupied[index / 64] &= ~(1ull << (index % 64));
    for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define }) {
        if (!canHaveCellAt(access, index))
            continue;
        m_planes[static_cast<unsigned>(access)][index].clear();
        setDependsOnChain(access, index, false);
    }
}

uint32_t RowDisplacementTable::baseOfOccupant(uint32_t index) const
{
    const PackedSlot& cell = reservationAt(index);
    ASSERT(!cell.isEmpty());
    return isRowMarker(cell) ? index : index - cell.name;
}

void RowDisplacementTable::RowColumns::append(uint16_t column)
{
    if (m_size == m_capacity) {
        uint16_t newCapacity = static_cast<uint16_t>(std::min<unsigned>(m_capacity * 2, UINT16_MAX));
        RELEASE_ASSERT(newCapacity > m_capacity);
        auto* storage = static_cast<uint16_t*>(fastMalloc(newCapacity * sizeof(uint16_t)));
        memcpy(storage, span().data(), m_size * sizeof(uint16_t));
        if (!isInline())
            fastFree(outOfLineStorage());
        memcpy(m_storage, &storage, sizeof(storage));
        m_capacity = newCapacity;
    }
    (isInline() ? m_storage : outOfLineStorage())[m_size++] = column;
}

void RowDisplacementTable::RowColumns::clear()
{
    if (!isInline())
        fastFree(outOfLineStorage());
    m_size = 0;
    m_capacity = inlineCapacity;
}

uint32_t RowDisplacementTable::allocateRowNumberFor(StructureID owner)
{
    uint32_t number;
    if (m_freeRows.isEmpty()) {
        number = m_rows.size();
        m_rows.alloc();
    } else
        number = m_freeRows.takeLast();
    m_rows[number].owner = owner;
    m_rowsWrittenSinceLastSweep.set(number);
    return number;
}

uint32_t RowDisplacementTable::createRow(Structure* structure, uint32_t column)
{
    uint16_t rowColumn = static_cast<uint16_t>(column);
    constexpr unsigned maxExpectedColumns = 32;
    Vector<uint16_t, maxExpectedColumns> expectedColumns;
    expectedColumns.append(rowColumn);
    RecentRow& last = m_recentRows[column % m_recentRows.size()];
    uint32_t preferredBase = 0;
    if (last.column == rowColumn && last.row < m_rows.size() && m_rows[last.row].owner) {
        preferredBase = baseOf(m_rows[last.row].owner) + 1;
        for (uint16_t other : m_rows[last.row].columns) {
            if (other != rowColumn && expectedColumns.size() < maxExpectedColumns)
                expectedColumns.append(other);
        }
    }
    uint32_t base = findBase(expectedColumns.span(), preferredBase);
    if (!base)
        base = findBaseEvictingRows(expectedColumns.span(), structure->id());
    ASSERT(base);
    uint32_t number = allocateRowNumberFor(structure->id());
    last = { rowColumn, number };
    m_rows[number].columns.append(rowColumn);
    reserve(base, rowMarkerTag, number);
    reserve(indexOf(base, column), rowColumn, 0);
    m_structureRows.setRowOf(structure->id(), base);
    return indexOf(base, column);
}

uint32_t RowDisplacementTable::moveRow(uint32_t base, std::optional<uint16_t> columnToAdd)
{
    uint32_t rowNumber = rowNumberAt(base);
    StructureID owner = m_rows[rowNumber].owner;
    Vector<uint16_t, 32> columns;
    columns.append(m_rows[rowNumber].columns.span());
    unsigned numberOfMovedColumns = columns.size();
    if (columnToAdd)
        columns.append(*columnToAdd);
    uint32_t newBase = findBase(columns.span());
    if (!newBase && columnToAdd)
        newBase = findBaseEvictingRows(columns.span(), owner);
    if (!newBase)
        return 0;
    vacate(base);
    reserve(newBase, rowMarkerTag, rowNumber);
    for (unsigned i = 0; i < numberOfMovedColumns; ++i) {
        uint32_t from = indexOf(base, columns[i]);
        uint32_t to = indexOf(newBase, columns[i]);
        reserve(to, columns[i], 0);
        for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define }) {
            if (!canHaveCellAt(access, from) || cellAt(access, from).isEmpty())
                continue;
            writeCell(access, to, cellAt(access, from));
            setDependsOnChain(access, to, dependsOnChain(access).get(from));
        }
        vacate(from);
    }
    if (columnToAdd) {
        reserve(indexOf(newBase, *columnToAdd), *columnToAdd, 0);
        m_rows[rowNumber].columns.append(*columnToAdd);
    }
    m_structureRows.setRowOf(owner, newBase);
    return newBase;
}

uint32_t RowDisplacementTable::ensureIndex(Structure* structure, uint32_t column)
{
    uint32_t base = baseOf(structure->id());
    if (!base)
        return createRow(structure, column);
    uint16_t rowColumn = static_cast<uint16_t>(column);
    uint32_t index = indexOf(base, column);
    if (isReservedFor(index, column))
        return index;
    bool isOccupied = !reservationAt(index).isEmpty();
    if (isOccupied) {
        uint32_t occupantBase = baseOfOccupant(index);
        if (rowAt(occupantBase).columns.size() <= rowAt(base).columns.size() && moveRow(occupantBase, std::nullopt))
            isOccupied = false;
    }
    if (!isOccupied) {
        rowAt(base).columns.append(rowColumn);
        reserve(index, rowColumn, 0);
        return index;
    }
    if (uint32_t newBase = moveRow(base, rowColumn))
        return indexOf(newBase, column);
    release(base);
    return createRow(structure, column);
}

void RowDisplacementTable::release(uint32_t base)
{
    ASSERT(base && isRowMarker(reservationAt(base)));
    uint32_t rowNumber = rowNumberAt(base);
    Row& row = m_rows[rowNumber];
    for (uint16_t column : row.columns)
        vacate(indexOf(base, column));
    vacate(base);
    m_structureRows.setRowOf(row.owner, 0);
    row.owner = StructureID();
    row.columns.clear();
    m_rowsThatCanOutliveWhatTheyName.clear(rowNumber);
    m_freeRows.append(rowNumber);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
