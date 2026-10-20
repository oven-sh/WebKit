/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCachesFront.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTPropertyCachesCustomAccessors.h"
#include "AOTRuntimeCounters.h"
#include "JSCInlines.h"
#include <wtf/OSAllocator.h>
#include <wtf/TZoneMallocInlines.h>
#if OS(LINUX)
#include <sys/mman.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(RowDisplacementTable);

static constexpr size_t sizeOfCellsInPlane = RowDisplacementTable::cellsPerPlane * sizeof(PackedSlot);
static constexpr size_t sizeOfPlane = roundUpToMultipleOf<16 * KB>(sizeOfCellsInPlane);
static constexpr size_t sizeOfOccupied = RowDisplacementTable::cellsPerPlane / 8 + sizeof(uint64_t);
static constexpr size_t offsetOfOccupied = numberOfSlotAccesses * sizeOfPlane;
static constexpr size_t sizeOfTables = offsetOfOccupied + roundUpToMultipleOf<16 * KB>(sizeOfOccupied);
static_assert(!(RowDisplacementTable::cellsPerPlane % 64));

static void* reserveForTable(size_t size)
{
    void* result = OSAllocator::reserveAndCommit(size, OSAllocator::FastMallocPages);
#if OS(LINUX)
    while (madvise(result, size, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#endif
    return result;
}

RowDisplacementTable::RowDisplacementTable(VM& vm, CacheRegistry& registry, const CustomAccessorTable& customAccessors)
    : m_vm(vm)
    , m_customAccessors(customAccessors)
    , m_structureRows(StructureRows::singleton())
    , m_cageBase(vm.heap.cage().base())
{
    auto* tables = static_cast<uint8_t*>(reserveForTable(sizeOfTables));
    for (unsigned i = 0; i < numberOfSlotAccesses; ++i) {
        uint8_t* plane = tables + i * sizeOfPlane;
        m_planes[i] = reinterpret_cast<RememberedSlot<PackedSlot>*>(plane);
        m_writtenRegions[i] = makeUniqueWithoutFastMallocCheck<RememberedSlotSet>(std::span<const uint8_t> { plane, sizeOfCellsInPlane }, regionShift);
    }
    m_occupied = reinterpret_cast<uint64_t*>(tables + offsetOfOccupied);
    m_bases.limit = static_cast<uint32_t>(std::clamp<uint64_t>(runtimeLimitValue(vm, LimitID::LookupTableBases), m_bases.first + 1, maxNumberOfBases));
    registry.add(CacheKind::WeakByKey, "RowDisplacementTable"_s, *this);
}

RowDisplacementTable::~RowDisplacementTable()
{
    clear();
    OSAllocator::decommitAndRelease(m_planes[0], sizeOfTables);
}

void RowDisplacementTable::writeCell(SlotAccess access, uint32_t index, PackedSlot cell)
{
    unsigned plane = static_cast<unsigned>(access);
    m_planes[plane][index].set(*m_writtenRegions[plane], cell);
}

void RowDisplacementTable::setDependsOnChain(SlotAccess access, uint32_t index, bool value)
{
    BitVector& bits = dependsOnChain(access);
    if (!value) {
        if (bits.get(index))
            bits.quickClear(index);
        return;
    }
    if (index >= bits.size())
        bits.ensureSize(std::max<size_t>(index + 1, bits.size() * 2));
    bits.quickSet(index);
}

void RowDisplacementTable::clearCell(SlotAccess access, uint32_t index)
{
    PackedSlot cell { };
    if (access == SlotAccess::Read) {
        ASSERT(!isRowMarker(reservationAt(index)));
        cell = *PackedSlot::tryPack(Slot::createWithoutPayload(StructureID(), Slot::Kind::Empty, 0), reservationAt(index).name, m_cageBase);
    }
    writeCell(access, index, cell);
    setDependsOnChain(access, index, false);
}

bool RowDisplacementTable::trySet(Structure* structure, uint32_t column, SlotAccess access, const Slot& slot)
{
    ASSERT(hasColumn(column) && slot.structureID == structure->id() && !structure->isDictionary());
    std::optional<PackedSlot> cell = PackedSlot::tryPack(slot, static_cast<uint16_t>(column), m_cageBase);
    if (!cell)
        return false;
    uint32_t index = ensureIndex(structure, column);
    writeCell(access, index, *cell);
    setDependsOnChain(access, index, slot.has(Slot::dependsOnChain));
    if (cell->canOutliveWhatItNames()) {
        uint32_t rowNumber = rowNumberAt(index - column);
        m_rowsThatCanOutliveWhatTheyName.set(rowNumber);
        m_rowsWrittenSinceLastSweep.set(rowNumber);
    }
    return true;
}

std::optional<Slot> RowDisplacementTable::find(StructureID id, uint32_t column, SlotAccess access) const
{
    ASSERT(hasColumn(column));
    uint32_t base = baseOf(id);
    if (!base)
        return std::nullopt;
    uint32_t index = indexOf(base, column);
    const PackedSlot& cell = cellAt(access, index);
    if (cell.name != column || cell.kind() == Slot::Kind::Empty)
        return std::nullopt;
    return cell.unpack(id, m_cageBase, dependsOnChain(access).get(index));
}

void RowDisplacementTable::remove(StructureID id, uint32_t column, SlotAccess access)
{
    ASSERT(hasColumn(column));
    uint32_t base = baseOf(id);
    if (!base)
        return;
    uint32_t index = indexOf(base, column);
    if (cellAt(access, index).name == column)
        clearCell(access, index);
}

bool RowDisplacementTable::hasChainDependentCell(StructureID id, uint32_t column) const
{
    ASSERT(hasColumn(column));
    uint32_t base = baseOf(id);
    if (!base)
        return false;
    uint32_t index = indexOf(base, column);
    if (!isReservedFor(index, column))
        return false;
    for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define }) {
        if (dependsOnChain(access).get(index))
            return true;
    }
    return false;
}

size_t RowDisplacementTable::removeChainDependentCells(StructureID id, uint32_t column)
{
    ASSERT(hasColumn(column));
    uint32_t base = baseOf(id);
    if (!base)
        return 0;
    uint32_t index = indexOf(base, column);
    if (!isReservedFor(index, column))
        return 0;
    size_t numberRemoved = 0;
    for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define }) {
        if (!dependsOnChain(access).get(index))
            continue;
        clearCell(access, index);
        ++numberRemoved;
    }
    return numberRemoved;
}

void RowDisplacementTable::releaseRowOf(StructureID id)
{
    if (uint32_t base = baseOf(id))
        release(base);
}

void RowDisplacementTable::clear()
{
    for (unsigned number = 0; number < m_rows.size(); ++number) {
        if (StructureID owner = m_rows[number].owner)
            release(baseOf(owner));
    }
    compactBases();
}

size_t RowDisplacementTable::removeDeadEntries(VM& vm, CollectionScope scope)
{
    // An Eden collection kills young cells only. A row that names one, or belongs to one, was written since the last sweep.
    size_t numberRemoved = 0;
    if (scope == CollectionScope::Eden) {
        for (size_t rowNumber : m_rowsWrittenSinceLastSweep)
            numberRemoved += sweepRow(vm, static_cast<uint32_t>(rowNumber));
    } else {
        for (unsigned rowNumber = 0; rowNumber < m_rows.size(); ++rowNumber)
            numberRemoved += sweepRow(vm, rowNumber);
    }
    m_rowsWrittenSinceLastSweep.clearAll();
    // Validation sweeps everything after an Eden collection too, and may not change where the next row goes.
    if (scope == CollectionScope::Full && vm.heap.collectionScope() != CollectionScope::Eden)
        compactBases();
    return numberRemoved;
}

size_t RowDisplacementTable::sweepRow(VM& vm, uint32_t rowNumber)
{
    auto isMarked = [&](JSCell* cell) {
        return vm.heap.isMarked(cell);
    };
    Row& row = m_rows[rowNumber];
    if (!row.owner)
        return 0;
    uint32_t base = baseOf(row.owner);
    if (!isStructureIDMarked(row.owner, isMarked)) {
        size_t numberRemoved = row.columns.size();
        release(base);
        return numberRemoved;
    }
    if (!m_rowsThatCanOutliveWhatTheyName.get(rowNumber))
        return 0;

    size_t numberRemoved = 0;
    bool canStillOutliveWhatItNames = false;
    for (uint16_t column : row.columns) {
        uint32_t index = indexOf(base, column);
        for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define }) {
            if (!canHaveCellAt(access, index))
                continue;
            const PackedSlot& cell = cellAt(access, index);
            if (cell.isEmpty() || !cell.canOutliveWhatItNames())
                continue;
            ASSERT(cell.name == column);
            Slot unpacked = cell.unpack(StructureID(), m_cageBase, false);
            bool namesUnusedAccessor = Slot::payloadOf(unpacked.kind) == SlotPayload::CustomAccessor && !m_customAccessors.isInUse(unpacked.customAccessor.index);
            if (unpacked.namesOnlyLiveCells(isMarked, m_cageBase) && !namesUnusedAccessor) {
                canStillOutliveWhatItNames = true;
                continue;
            }
            clearCell(access, index);
            ++numberRemoved;
        }
    }
    if (!canStillOutliveWhatItNames)
        m_rowsThatCanOutliveWhatTheyName.clear(rowNumber);
    return numberRemoved;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
