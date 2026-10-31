/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCaches.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTCompiledFrame.h"
#include "AOTCompiledStructure.h"
#include "AOTEmptyPages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPendingStars.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeCounters.h"
#include "AOTRuntimeRecords.h"
#include "AOTRuntimeValidation.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTThreshold.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "GetterSetter.h"
#include "JSCConfig.h"
#include "JSCInlines.h"
#include "PropertyOffset.h"
#include "SlotVisitorInlines.h"
#include <array>
#include <bit>
#include <mutex>
#include <type_traits>
#include <wtf/AlignedStorage.h>
#include <wtf/Atomics.h>
#include <wtf/PageBlock.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#if OS(LINUX)
#include <sys/mman.h>
#if !defined(MADV_POPULATE_WRITE)
#define MADV_POPULATE_WRITE 23
#endif
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMPropertyCaches);
WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyCaches);
WTF_MAKE_TZONE_ALLOCATED_IMPL(StringKeyCache);
WTF_MAKE_TZONE_ALLOCATED_IMPL(HashedSlotTable);
WTF_MAKE_TZONE_ALLOCATED_IMPL(RowDisplacementTable);

// The planes, and a bit for each cell of a plane. A cell is the base of a row plus a column.
struct PagesOfPlanes {
    explicit constexpr PagesOfPlanes(uint32_t numberOfBases)
        : sizeOfCellsInPlane((static_cast<size_t>(numberOfBases) + RowDisplacementTable::numberOfColumns) * sizeof(PackedSlot))
        , sizeOfPlane(roundUpToMultipleOf<16 * KB>(sizeOfCellsInPlane))
        , sizeOfOccupied(roundUpToMultipleOf<16 * KB>(roundUpToMultipleOf<64>(sizeOfCellsInPlane / sizeof(PackedSlot)) / 8 + sizeof(uint64_t)))
        , offsetOfOccupied(numberOfSlotAccesses * sizeOfPlane)
        , size(offsetOfOccupied + sizeOfOccupied)
    {
    }

    size_t sizeOfCellsInPlane;
    size_t sizeOfPlane;
    size_t sizeOfOccupied;
    size_t offsetOfOccupied;
    size_t size;
};
static_assert(!(RowDisplacementTable::numberOfColumns % 64) && !(RowDisplacementTable::initialNumberOfBases % 64));

static void* reservePagesOfTable(VM& vm, size_t size)
{
    void* result = vm.aotState().pages->tryAllocate(size);
    RELEASE_ASSERT(result, size);
    return result;
}

#if OS(LINUX) && !ENABLE(AOT_GUARDED_PAGES)
static constexpr bool pagesOfTablesCanGrow = true;

// What the pages held stays, what is added is zero. A refusal changes nothing.
static void* tryGrowPagesOfTable(VM& vm, void* pages, size_t size, size_t newSize)
{
    void* result = mremap(pages, size, newSize, MREMAP_MAYMOVE);
    if (result == MAP_FAILED)
        return nullptr;
    if (result != pages)
        vm.aotState().pages->didMoveAway(pages, size);
    return result;
}
#else
static constexpr bool pagesOfTablesCanGrow = false;
static void* tryGrowPagesOfTable(VM&, void*, size_t, size_t) { return nullptr; }
#endif

static uint32_t numberOfBasesAtStart(uint32_t limitOfBases)
{
    return pagesOfTablesCanGrow ? std::min(RowDisplacementTable::initialNumberOfBases, limitOfBases) : limitOfBases;
}

StructureRows& StructureRows::ofImageAt(unsigned position)
{
    static Lock lock;
    static NeverDestroyed<Vector<std::unique_ptr<StructureRows>>> rows;
    Locker locker { lock };
    while (rows->size() <= position)
        rows->append(std::unique_ptr<StructureRows>(new StructureRows));
    return *rows.get()[position];
}

StructureRows::StructureRows()
{
    uintptr_t start = g_jscConfig.startOfStructureHeap;
    size_t size = g_jscConfig.sizeOfStructureHeap;
    RELEASE_ASSERT(start && size);
    RELEASE_ASSERT((start & StructureID::structureIDMask) + (size - 1) <= StructureID::structureIDMask);
    size_t sizeOfWords = (size >> StructureRowWords::granuleShift) << StructureRowWords::wordShift;
    void* words = reserveGuardedPages(sizeOfWords);
#if OS(LINUX)
    while (madvise(words, sizeOfWords, MADV_NOHUGEPAGE) == -1 && errno == EAGAIN) { }
#endif
    // SAFETY: The pointer lies outside the allocation. It is only indexed with the bits of a StructureID shifted by granuleShift: the low 32 bits of an address in the structure heap, which does not cross a 4 GB boundary.
    m_rowsForStructureIDZero = static_cast<uint32_t*>(words) - (static_cast<uint32_t>(start) >> StructureRowWords::granuleShift);
}

size_t RowDisplacementTable::sizeOfPagesAtStart(uint64_t valueOfLimitOfBases)
{
    return PagesOfPlanes(numberOfBasesAtStart(static_cast<uint32_t>(std::clamp<uint64_t>(valueOfLimitOfBases, firstBase + 1, maxNumberOfBases)))).size;
}

RowDisplacementTable::RowDisplacementTable(VM& vm, CacheRegistry& registry, const CustomAccessorTable& customAccessors)
    : m_vm(vm)
    , m_customAccessors(customAccessors)
    , m_structureRows(StructureRows::ofImageAt(vm.aotState().image ? vm.aotState().image->position() : 0))
    , m_cageBase(vm.heap.cage().base())
{
    m_limitOfBases = static_cast<uint32_t>(std::clamp<uint64_t>(runtimeLimitValue(vm, LimitID::LookupTableBases), firstBase + 1, maxNumberOfBases));
    m_capacityOfBases = numberOfBasesAtStart(m_limitOfBases);
    PagesOfPlanes layout(m_capacityOfBases);
    auto* pages = static_cast<uint8_t*>(reservePagesOfTable(vm, layout.size));
    for (unsigned i = 0; i < numberOfSlotAccesses; ++i) {
        uint8_t* plane = pages + i * layout.sizeOfPlane;
        m_planes[i] = reinterpret_cast<RememberedSlot<PackedSlot>*>(plane);
        m_writtenRegions[i] = makeUniqueWithoutFastMallocCheck<RememberedSlotSet>(std::span<const uint8_t> { plane, layout.sizeOfCellsInPlane }, regionShift);
    }
    m_occupied = reinterpret_cast<uint64_t*>(pages + layout.offsetOfOccupied);
    m_numbersOfBasesWithPages.fill(m_capacityOfBases);
    registry.add(CacheKind::WeakByKey, "RowDisplacementTable"_s, *this);
}

RowDisplacementTable::~RowDisplacementTable()
{
    clear();
    PagesOfVM& pages = *m_vm.aotState().pages;
    if (!pagesOfTablesCanGrow) {
        pages.release(m_planes[0], PagesOfPlanes(m_capacityOfBases).size);
        return;
    }
    for (unsigned i = 0; i < numberOfSlotAccesses; ++i)
        pages.release(m_planes[i], PagesOfPlanes(m_numbersOfBasesWithPages[i]).sizeOfPlane);
    pages.release(m_occupied, PagesOfPlanes(m_numbersOfBasesWithPages[numberOfSlotAccesses]).sizeOfOccupied);
}

// Where there is no room for more pages the table stays as large as it is, and rows give way to rows as they do at the limit.
bool RowDisplacementTable::tryGrow()
{
    if (m_capacityOfBases >= m_limitOfBases)
        return false;
    uint32_t numberOfBases = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(m_capacityOfBases) * 2, m_limitOfBases));
    PagesOfPlanes to(numberOfBases);
    auto tryGrowPart = [&](unsigned part, void* pages, size_t PagesOfPlanes::* size) -> void* {
        if (m_numbersOfBasesWithPages[part] == numberOfBases)
            return pages;
        void* result = tryGrowPagesOfTable(m_vm, pages, PagesOfPlanes(m_numbersOfBasesWithPages[part]).*size, to.*size);
        if (result)
            m_numbersOfBasesWithPages[part] = numberOfBases;
        return result;
    };
    for (unsigned i = 0; i < numberOfSlotAccesses; ++i) {
        void* plane = tryGrowPart(i, m_planes[i], &PagesOfPlanes::sizeOfPlane);
        if (!plane) {
            m_limitOfBases = m_capacityOfBases;
            return false;
        }
        m_planes[i] = static_cast<RememberedSlot<PackedSlot>*>(plane);
        m_writtenRegions[i]->didMoveAndGrowTable({ static_cast<const uint8_t*>(plane), to.sizeOfCellsInPlane });
        m_didMovePlanes = true;
    }
    void* occupied = tryGrowPart(numberOfSlotAccesses, m_occupied, &PagesOfPlanes::sizeOfOccupied);
    if (!occupied) {
        m_limitOfBases = m_capacityOfBases;
        return false;
    }
    m_occupied = static_cast<uint64_t*>(occupied);
    m_capacityOfBases = numberOfBases;
    return true;
}

void RowDisplacementTable::writeCell(SlotAccess access, uint32_t index, PackedSlot cell, bool dependsOnChain)
{
    unsigned plane = static_cast<unsigned>(access);
    m_planes[plane][index].set(*m_writtenRegions[plane], cell);
    BitVector& bits = m_dependsOnChain[plane];
    if (!dependsOnChain) {
        bits.clear(index);
        return;
    }
    if (index >= bits.size())
        bits.ensureSize(std::max<size_t>(index + 1, bits.size() * 2));
    bits.quickSet(index);
}

void RowDisplacementTable::clearCell(SlotAccess access, uint32_t index)
{
    ASSERT(reservationAt(index).name != rowMarkerTag);
    writeCell(access, index, access == SlotAccess::Read ? emptyCellNamed(reservationAt(index).name) : PackedSlot { }, false);
}

uint32_t RowDisplacementTable::indexOfReservedCell(StructureID id, uint32_t column) const
{
    ASSERT(hasColumn(column));
    uint32_t base = baseOf(id);
    return base && reservationAt(base + column).name == column ? base + column : 0;
}

bool RowDisplacementTable::trySet(Structure* structure, uint32_t column, SlotAccess access, const Slot& slot)
{
    ASSERT(hasColumn(column) && slot.structureID == structure->id() && !structure->isDictionary());
    std::optional<PackedSlot> cell = PackedSlot::tryPack(slot, static_cast<uint16_t>(column), m_cageBase);
    if (!cell)
        return false;
    uint32_t index = ensureIndex(structure, static_cast<uint16_t>(column));
    writeCell(access, index, *cell, slot.has(Slot::dependsOnChain));
    if (cell->canOutliveWhatItNames()) {
        uint32_t rowNumber = reservationAt(index - column).payload;
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
    const PackedSlot& cell = cellAt(access, base + column);
    if (cell.name != column || cell.kind() == Slot::Kind::Empty)
        return std::nullopt;
    return cell.unpack(id, m_cageBase, dependsOnChain(access, base + column));
}

void RowDisplacementTable::remove(StructureID id, uint32_t column, SlotAccess access)
{
    ASSERT(hasColumn(column));
    uint32_t base = baseOf(id);
    if (base && cellAt(access, base + column).name == column)
        clearCell(access, base + column);
}

bool RowDisplacementTable::hasChainDependentCell(StructureID id, uint32_t column) const
{
    uint32_t index = indexOfReservedCell(id, column);
    return index && std::ranges::any_of(allSlotAccesses, [&](SlotAccess access) {
        return dependsOnChain(access, index);
    });
}

size_t RowDisplacementTable::removeChainDependentCells(StructureID id, uint32_t column)
{
    uint32_t index = indexOfReservedCell(id, column);
    size_t numberRemoved = 0;
    for (SlotAccess access : allSlotAccesses) {
        if (index && dependsOnChain(access, index)) {
            clearCell(access, index);
            ++numberRemoved;
        }
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
    size_t numberRemoved = 0;
    if (scope == CollectionScope::Eden) {
        for (size_t rowNumber : m_rowsWrittenSinceLastSweep)
            numberRemoved += sweepRow(vm, static_cast<uint32_t>(rowNumber));
    } else {
        for (unsigned rowNumber = 0; rowNumber < m_rows.size(); ++rowNumber)
            numberRemoved += sweepRow(vm, rowNumber);
    }
    if (!m_rowsWrittenSinceLastSweep.isEmpty())
        m_rowsWrittenSinceLastSweep.clearAll();
    if (scope == CollectionScope::Full && vm.heap.collectionScope() != CollectionScope::Eden) {
        compactBases();
        for (unsigned i = 0; i < numberOfSlotAccesses; ++i)
            decommitEmptyPages({ reinterpret_cast<uint8_t*>(m_planes[i]), PagesOfPlanes(m_capacityOfBases).sizeOfCellsInPlane }, *m_writtenRegions[i]);
    }
    return numberRemoved;
}

size_t RowDisplacementTable::sweepRow(VM& vm, uint32_t rowNumber)
{
    Row& row = m_rows[rowNumber];
    if (!row.owner)
        return 0;
    uint32_t base = baseOf(row.owner);
    if (!vm.heap.isMarked(row.owner.decode())) {
        size_t numberRemoved = row.columns.size();
        release(base);
        return numberRemoved;
    }
    if (!m_rowsThatCanOutliveWhatTheyName.get(rowNumber))
        return 0;
    size_t numberRemoved = 0;
    bool canStillOutliveWhatItNames = false;
    for (uint16_t column : row.columns) {
        for (SlotAccess access : allSlotAccesses) {
            if (!canHaveCellAt(access, base + column))
                continue;
            const PackedSlot& cell = cellAt(access, base + column);
            if (cell.isEmpty() || !cell.canOutliveWhatItNames())
                continue;
            ASSERT(cell.name == column);
            if (m_customAccessors.namesOnlyWhatIsLive(vm, cell.unpack(StructureID(), m_cageBase, false))) {
                canStillOutliveWhatItNames = true;
                continue;
            }
            clearCell(access, base + column);
            ++numberRemoved;
        }
    }
    if (!canStillOutliveWhatItNames)
        m_rowsThatCanOutliveWhatTheyName.clear(rowNumber);
    return numberRemoved;
}

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
        fitting &= freeCellsFrom(from + column);
    }
    if (!fitting)
        return 0;
    uint32_t base = from + WTF::ctz(fitting);
    return base < m_capacityOfBases ? base : 0;
}

uint32_t RowDisplacementTable::findBase(std::span<const uint16_t> columns, uint32_t preferredBase)
{
    constexpr unsigned basesPerBatch = 64;
    AOT_THRESHOLD(unsigned, maxProbesBetweenRows, 4);
    if (uint32_t base = preferredBase ? firstFittingBase(preferredBase, columns) : 0) {
        m_endOfBases = std::max(m_endOfBases, base + 1);
        return base;
    }
    uint32_t from = m_nextBase;
    for (unsigned probes = 0; probes < maxProbesBetweenRows && from < m_endOfBases; ++probes, from += basesPerBatch) {
        if (uint32_t base = firstFittingBase(from, columns)) {
            m_nextBase = base + 1;
            m_endOfBases = std::max(m_endOfBases, base + 1);
            return base;
        }
    }
    m_nextBase = from < m_endOfBases ? from : firstBase;
    from = m_endOfBases;
    do {
        for (; from < m_capacityOfBases; from += basesPerBatch) {
            if (uint32_t base = firstFittingBase(from, columns)) {
                m_endOfBases = base + 1;
                return base;
            }
        }
    } while (tryGrow());
    return 0;
}

uint32_t RowDisplacementTable::findBaseEvictingRows(std::span<const uint16_t> columns, StructureID keep)
{
    countLimitHit(m_vm, LimitID::LookupTableBases);
    Vector<uint32_t, 33> occupiedIndexes;
    for (uint32_t basesLeft = m_capacityOfBases - firstBase; basesLeft; --basesLeft) {
        uint32_t base = m_evictionHand;
        m_evictionHand = base + 1 < m_capacityOfBases ? base + 1 : firstBase;
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
            collect(base + column);
        if (overlapsKeep)
            continue;
        for (uint32_t index : occupiedIndexes) {
            if (!(freeCellsFrom(index) & 1))
                release(baseOfOccupant(index));
        }
        m_endOfBases = std::max(m_endOfBases, base + 1);
        return base;
    }
    return 0;
}

void RowDisplacementTable::compactBases()
{
    m_endOfBases = firstBase;
    for (unsigned number = 0; number < m_rows.size(); ++number) {
        if (StructureID owner = m_rows[number].owner)
            m_endOfBases = std::max(m_endOfBases, baseOf(owner) + 1);
    }
    m_nextBase = firstBase;
}

void RowDisplacementTable::reserve(uint32_t index, PackedSlot reservation)
{
    ASSERT(reservationAt(index).isEmpty());
    m_occupied[index / 64] |= 1ull << (index % 64);
    writeCell(SlotAccess::Read, index, reservation, false);
}

void RowDisplacementTable::vacate(uint32_t index)
{
    m_occupied[index / 64] &= ~(1ull << (index % 64));
    for (SlotAccess access : allSlotAccesses) {
        if (!canHaveCellAt(access, index))
            continue;
        m_planes[static_cast<unsigned>(access)][index].clear();
        m_dependsOnChain[static_cast<unsigned>(access)].clear(index);
    }
}

void RowDisplacementTable::RowColumns::append(uint16_t column)
{
    if (m_size == m_capacity) {
        uint16_t newCapacity = static_cast<uint16_t>(std::min<unsigned>(m_capacity * 2, UINT16_MAX));
        RELEASE_ASSERT(newCapacity > m_capacity);
        auto* newStorage = static_cast<uint16_t*>(fastMalloc(newCapacity * sizeof(uint16_t)));
        memcpy(newStorage, storage(), m_size * sizeof(uint16_t));
        if (m_capacity != inlineCapacity)
            fastFree(storage());
        memcpy(m_storage, &newStorage, sizeof(newStorage));
        m_capacity = newCapacity;
    }
    storage()[m_size++] = column;
}

void RowDisplacementTable::RowColumns::clear()
{
    if (m_capacity != inlineCapacity)
        fastFree(storage());
    m_size = 0;
    m_capacity = inlineCapacity;
}

static PackedSlot markerOfRow(uint32_t rowNumber)
{
    return { rowNumber, 0, PackedSlot::ownInlineValueBits, RowDisplacementTable::rowMarkerTag };
}

uint32_t RowDisplacementTable::createRow(Structure* structure, uint16_t column)
{
    constexpr unsigned maxExpectedColumns = 32;
    Vector<uint16_t, maxExpectedColumns> expectedColumns;
    expectedColumns.append(column);
    RecentRow& last = m_recentRows[column % m_recentRows.size()];
    uint32_t preferredBase = 0;
    if (last.column == column && last.row < m_rows.size() && m_rows[last.row].owner) {
        preferredBase = baseOf(m_rows[last.row].owner) + 1;
        for (uint16_t other : m_rows[last.row].columns) {
            if (other != column && expectedColumns.size() < maxExpectedColumns)
                expectedColumns.append(other);
        }
    }
    uint32_t base = findBase(expectedColumns.span(), preferredBase);
    if (!base)
        base = findBaseEvictingRows(expectedColumns.span(), structure->id());
    ASSERT(base);
    uint32_t number = m_freeRows.isEmpty() ? m_rows.size() : m_freeRows.takeLast();
    if (number == m_rows.size())
        m_rows.alloc();
    m_rows[number].owner = structure->id();
    m_rowsWrittenSinceLastSweep.set(number);
    last = { column, number };
    m_rows[number].columns.append(column);
    reserve(base, markerOfRow(number));
    reserve(base + column, emptyCellNamed(column));
    m_structureRows.setRowOf(structure->id(), base);
    return base + column;
}

uint32_t RowDisplacementTable::moveRow(uint32_t base, std::optional<uint16_t> columnToAdd)
{
    uint32_t rowNumber = reservationAt(base).payload;
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
    reserve(newBase, markerOfRow(rowNumber));
    for (unsigned i = 0; i < numberOfMovedColumns; ++i) {
        uint32_t from = base + columns[i];
        reserve(newBase + columns[i], emptyCellNamed(columns[i]));
        for (SlotAccess access : allSlotAccesses) {
            if (canHaveCellAt(access, from) && !cellAt(access, from).isEmpty())
                writeCell(access, newBase + columns[i], cellAt(access, from), dependsOnChain(access, from));
        }
        vacate(from);
    }
    if (columnToAdd) {
        reserve(newBase + *columnToAdd, emptyCellNamed(*columnToAdd));
        m_rows[rowNumber].columns.append(*columnToAdd);
    }
    m_structureRows.setRowOf(owner, newBase);
    return newBase;
}

uint32_t RowDisplacementTable::ensureIndex(Structure* structure, uint16_t column)
{
    uint32_t base = baseOf(structure->id());
    if (!base)
        return createRow(structure, column);
    uint32_t index = base + column;
    if (reservationAt(index).name == column)
        return index;
    bool isOccupied = !reservationAt(index).isEmpty();
    if (isOccupied) {
        uint32_t occupantBase = baseOfOccupant(index);
        if (rowAt(occupantBase).columns.size() <= rowAt(base).columns.size() && moveRow(occupantBase, std::nullopt))
            isOccupied = false;
    }
    if (!isOccupied) {
        rowAt(base).columns.append(column);
        reserve(index, emptyCellNamed(column));
        return index;
    }
    if (uint32_t newBase = moveRow(base, column))
        return newBase + column;
    release(base);
    return createRow(structure, column);
}

void RowDisplacementTable::release(uint32_t base)
{
    ASSERT(base && reservationAt(base).name == rowMarkerTag);
    uint32_t rowNumber = reservationAt(base).payload;
    Row& row = m_rows[rowNumber];
    for (uint16_t column : row.columns)
        vacate(base + column);
    vacate(base);
    m_structureRows.setRowOf(row.owner, 0);
    row.owner = StructureID();
    row.columns.clear();
    m_rowsThatCanOutliveWhatTheyName.clear(rowNumber);
    m_freeRows.append(rowNumber);
}

static constexpr uint64_t maxAddressableEntries = 1ull << (32 - HashedSlot::sizeShift);
static_assert(hasOneBitSet(HashedSlotTable::initialNumberOfEntries) && HashedSlotTable::initialNumberOfEntries >= HashedSlot::ways);
static_assert(!((1u << HashedSlotTable::regionShift) % (HashedSlot::ways * sizeof(HashedSlot))));

static size_t sizeOfRecords(uint32_t capacity)
{
    return roundUpToMultipleOf(pageSize(), std::max<size_t>(capacity, 1) * sizeof(CustomAccessorRecord));
}

CustomAccessorTable::CustomAccessorTable(VM& vm, CacheRegistry& registry)
    : m_vm(vm)
    , m_capacity(safeCast<uint32_t>(runtimeLimitValue(vm, LimitID::CustomAccessorRecords)))
{
    m_records = static_cast<CustomAccessorRecord*>(vm.aotState().pages->allocate(sizeOfRecords(m_capacity)));
    registry.add(CacheKind::WeakByKey, "CustomAccessorTable"_s, *this);
}

CustomAccessorTable::~CustomAccessorTable()
{
    m_vm.aotState().pages->release(m_records, sizeOfRecords(m_capacity));
}

std::optional<uint32_t> CustomAccessorTable::find(const CustomAccessorRecord& record) const
{
    return m_indexByKey.getOptional(keyOf(record));
}

std::optional<uint32_t> CustomAccessorTable::add(const CustomAccessorRecord& record)
{
    RELEASE_ASSERT(record.function && record.globalObject);
    if (auto index = find(record))
        return index;
    uint32_t index;
    if (!m_freeIndexes.isEmpty())
        index = m_freeIndexes.takeLast();
    else if (m_numberOfRecordsEverUsed < m_capacity)
        index = m_numberOfRecordsEverUsed++;
    else {
        countLimitHit(m_vm, LimitID::CustomAccessorRecords);
        return std::nullopt;
    }
    m_records[index] = record;
    m_records[index].unused = 0;
    m_indexByKey.add(keyOf(record), index);
    m_writtenSinceCollection.recordAll();
    return index;
}

size_t CustomAccessorTable::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t numberOfRecordsFreed = 0;
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        for (uint32_t index = 0; index < m_numberOfRecordsEverUsed; ++index) {
            CustomAccessorRecord& record = m_records[index];
            if (!record.function || ((!record.slotBase || vm.heap.isMarked(record.slotBase)) && vm.heap.isMarked(record.globalObject)))
                continue;
            m_indexByKey.remove(keyOf(record));
            record = { };
            m_freeIndexes.append(index);
            ++numberOfRecordsFreed;
        }
    });
    return numberOfRecordsFreed;
}

bool CustomAccessorTable::namesOnlyWhatIsLive(VM& vm, const Slot& slot) const
{
    bool namesOnlyLiveCells = slot.namesOnlyLiveCells([&](JSCell* cell) {
        return vm.heap.isMarked(cell);
    }, vm.heap.cage().base());
    return namesOnlyLiveCells && (Slot::payloadOf(slot.kind) != SlotPayload::CustomAccessor || isInUse(slot.customAccessor.index));
}

static size_t sizeOfReservation(uint32_t maxNumberOfEntries)
{
    return roundUpToMultipleOf(pageSize(), static_cast<size_t>(maxNumberOfEntries) * sizeof(HashedSlot));
}

HashedSlotTable::HashedSlotTable(VM& vm, CacheRegistry& registry, const CustomAccessorTable& customAccessors)
    : m_vm(vm)
    , m_customAccessors(customAccessors)
    , m_maxNumberOfEntries(static_cast<uint32_t>(std::bit_floor(std::clamp<uint64_t>(runtimeLimitValue(vm, LimitID::HashedTableEntries), initialNumberOfEntries, maxAddressableEntries))))
    , m_numberOfEntries(initialNumberOfEntries)
    , m_entries(static_cast<Entry*>(reservePagesOfTable(vm, sizeOfReservation(m_maxNumberOfEntries))))
    , m_writtenRegions(asByteSpan(allEntries()), regionShift)
{
    registry.add(CacheKind::WeakByKey, "HashedSlotTable"_s, *this);
}

HashedSlotTable::~HashedSlotTable()
{
    clear();
    m_vm.aotState().pages->release(m_entries, sizeOfReservation(m_maxNumberOfEntries));
}

auto HashedSlotTable::findEntry(StructureID structureID, UniquedStringImpl* uid, uintptr_t key) const -> Entry*
{
    uint32_t offsetOfFirstWay = HashedSlot::offsetOfFirstWay(structureID, uid, offsetMask());
    for (unsigned way = 0; way < HashedSlot::ways; ++way) {
        auto* entry = reinterpret_cast<Entry*>(reinterpret_cast<uint8_t*>(m_entries) + HashedSlot::offsetOfWay(offsetOfFirstWay, way));
        if (key ? entry->get().matches(structureID, key) : entry->get().isEmpty())
            return entry;
    }
    return nullptr;
}

auto HashedSlotTable::findChainDependentEntry(StructureID structureID, UniquedStringImpl* uid, SlotAccess access) const -> Entry*
{
    Entry* entry = findEntry(structureID, uid, HashedSlot::keyFor(uid, access));
    return entry && entry->get().slot.has(Slot::dependsOnChain) ? entry : nullptr;
}

void HashedSlotTable::erase(Entry& entry)
{
    UniquedStringImpl* uid = entry.get().uid();
    entry.clear();
    --m_numberOfFilledEntries;
    uid->deref();
}

void HashedSlotTable::releaseNamesErasedDuringCollection()
{
    for (UniquedStringImpl* uid : std::exchange(m_namesErasedDuringCollection, { }))
        uid->deref();
}

HashedSlotTable::SetResult HashedSlotTable::set(UniquedStringImpl* uid, SlotAccess access, const Slot& slot)
{
    releaseNamesErasedDuringCollection();
    ASSERT(uid && slot.structureID && !(std::bit_cast<uintptr_t>(uid) & HashedSlot::accessMask));
    uintptr_t key = HashedSlot::keyFor(uid, access);
    Entry* entry = findEntry(slot.structureID, uid, key);
    if (!entry) {
        entry = findEntry(slot.structureID, uid, 0);
        if (!entry) {
            entry = m_entries + (HashedSlot::offsetOfWay(HashedSlot::offsetOfFirstWay(slot.structureID, uid, offsetMask()), m_nextVictimWay++ % HashedSlot::ways) >> HashedSlot::sizeShift);
            ++m_evictionsSinceCollection;
            if (m_numberOfFilledEntries >= m_numberOfEntries / 2)
                ++m_evictionsWhileHalfFull;
            erase(*entry);
            count(m_vm, RuntimeCounterID::HashedTableEvictions);
            if (m_numberOfEntries == m_maxNumberOfEntries)
                countLimitHit(m_vm, LimitID::HashedTableEntries);
        }
        uid->ref();
        ++m_numberOfFilledEntries;
    }
    entry->set(m_writtenRegions, HashedSlot { slot, key });
    if (m_numberOfEntries >= m_maxNumberOfEntries || (m_evictionsWhileHalfFull <= m_numberOfEntries / 64 && m_evictionsSinceCollection <= m_numberOfEntries / 4))
        return SetResult::Stored;
    grow();
    return SetResult::StoredAndGrew;
}

void HashedSlotTable::grow()
{
    std::span<Entry> lowerHalf = allEntries();
    [[maybe_unused]] bool fillsEveryPage = m_numberOfFilledEntries >= m_numberOfEntries / 2;
    m_numberOfEntries *= 2;
    m_writtenRegions.didMoveTable(asByteSpan(allEntries()));
#if OS(LINUX)
    if (fillsEveryPage)
        madvise(lowerHalf.data() + lowerHalf.size(), lowerHalf.size_bytes(), MADV_POPULATE_WRITE);
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
    Entry* entry = findEntry(structureID, uid, HashedSlot::keyFor(uid, access));
    return entry ? &entry->get().slot : nullptr;
}

void HashedSlotTable::remove(StructureID structureID, UniquedStringImpl* uid, SlotAccess access)
{
    releaseNamesErasedDuringCollection();
    if (Entry* entry = findEntry(structureID, uid, HashedSlot::keyFor(uid, access)))
        erase(*entry);
}

bool HashedSlotTable::hasChainDependentEntry(StructureID structureID, UniquedStringImpl* uid) const
{
    return std::ranges::any_of(allSlotAccesses, [&](SlotAccess access) {
        return findChainDependentEntry(structureID, uid, access);
    });
}

size_t HashedSlotTable::removeChainDependentEntries(StructureID structureID, UniquedStringImpl* uid)
{
    releaseNamesErasedDuringCollection();
    size_t numberOfRemovedEntries = 0;
    for (SlotAccess access : allSlotAccesses) {
        if (Entry* entry = findChainDependentEntry(structureID, uid, access)) {
            erase(*entry);
            ++numberOfRemovedEntries;
        }
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
    m_evictionsSinceCollection = 0;
    size_t numberOfRemovedEntries = 0;
    m_writtenRegions.forEachRegionToSweep(scope, [&](size_t beginOffset, size_t endOffset) {
        RememberedSlotSet::forEachEntryInRegion(allEntries(), beginOffset, endOffset, [&](Entry& entry) {
            if (entry.get().isEmpty() || m_customAccessors.namesOnlyWhatIsLive(vm, entry.get().slot))
                return;
            m_namesErasedDuringCollection.append(entry.get().uid());
            entry.clear();
            --m_numberOfFilledEntries;
            ++numberOfRemovedEntries;
        });
    });
    return numberOfRemovedEntries;
}

static std::optional<uint32_t> columnForNumber(std::optional<uint32_t> identifierNumber)
{
    return identifierNumber && RowDisplacementTable::hasColumn(*identifierNumber) ? identifierNumber : std::nullopt;
}

static void setBitOfName(auto& bits, uint32_t identifierNumber)
{
    bits[identifierNumber / 64] |= uint64_t { 1 } << identifierNumber % 64;
}

VMPropertyCaches::VMPropertyCaches(VM& vm)
    : m_vm(vm)
    , m_image(vm.aotState().image)
    , m_customAccessors(vm, *vm.aotState().caches)
    , m_front(vm, *vm.aotState().caches, m_customAccessors)
    , m_hashedTable(vm, *vm.aotState().caches, m_customAccessors)
    , m_tables(*static_cast<Tables*>(vm.aotState().pages->allocate(sizeof(Tables))))
{
    vm.aotState().caches->add(CacheKind::WeakByKey, "absence filters"_s, *this);
}

// A filter names its Structure by its number, and the number of one that has ended goes to another.
size_t VMPropertyCaches::removeDeadEntries(VM& vm, CollectionScope)
{
    size_t numberRemoved = 0;
    for (AbsenceFilter& filter : m_absenceFilters) {
        if (!filter.structureID || vm.heap.isMarked(std::bit_cast<StructureID>(filter.structureID).decode()))
            continue;
        filter = { };
        ++numberRemoved;
    }
    return numberRemoved;
}

VMPropertyCaches::~VMPropertyCaches()
{
    for (UniquedStringImpl* uid : m_columnsByNameAddress.keys())
        uid->deref();
    for (NameOfText& entry : m_namesOfTexts) {
        if (entry.uid)
            entry.uid->deref();
    }
    m_vm.aotState().pages->release(&m_tables, sizeof(Tables));
}

void VMPropertyCaches::rememberNameOfText(UniquedStringImpl* uid)
{
    uint32_t length = uid->length();
    if (uid->isSymbol() || !uid->is8Bit() || length < NameOfText::minLength || length > NameOfText::maxLength)
        return;
    const Latin1Character* characters = uid->span8().data();
    unsigned width = NameOfText::widthOfWordFor(length);
    uint64_t first = 0;
    uint64_t last = 0;
    static_assert(std::endian::native == std::endian::little);
    memcpy(&first, characters, width);
    memcpy(&last, characters + length - width, width);
    NameOfText* pair = &m_namesOfTexts[NameOfText::firstIndexFor(first, last, length)];
    if (pair[0].uid == uid || pair[1].uid == uid)
        return;
    uid->ref();
    if (pair[1].uid)
        pair[1].uid->deref();
    pair[1] = pair[0];
    pair[0] = { first, last, length, uid };
}

static bool listsEveryNameInItsTable(Structure* structure)
{
    return (!structure->isDictionary() || structure->mayBePrototype()) && !structure->hasPolyProto() && !hasIndexedProperties(structure->indexingType())
        && structure->propertyAccessesAreCacheable() && structure->propertyAccessesAreCacheableForAbsence()
        && !structure->typeInfo().overridesGetOwnPropertySlot() && !structure->hasNonReifiedStaticProperties();
}

bool VMPropertyCaches::serves(const Instance& instance) const
{
    return instance.program == m_image;
}

void VMPropertyCaches::didCreateInstance(Instance& instance)
{
    if (m_hasInstance && !m_isSharedByInstances) {
        // The instances that a realm has for other images have tables of their own.
        m_isSharedByInstances = std::ranges::count_if(m_vm.aotState().instances, [&](const Instance* each) {
            return serves(*each) && (!each->globalObject || each->globalObject->microtaskRunnability() != QueuedTaskResult::Discard);
        }) > 1;
        m_front.clear();
        m_hashedTable.clear();
        for (Instance* earlier : m_vm.aotState().instances) {
            if (!serves(*earlier))
                continue;
            zeroSpan(earlier->keyedValuePlaces());
            if (StringKeyCache* keys = earlier->state ? earlier->state->stringKeys.getIfCreated() : nullptr)
                keys->forgetPlacesOfPairs();
        }
    }
    m_hasInstance = true;
    if (instance.globalObject) {
        m_filteredObjects.add(instance.globalObject->objectPrototype());
        didChangeObjectPrototype(instance.globalObject->objectPrototype(), nullptr);
    }
    publishWords(instance);
}

void VMPropertyCaches::didChangeObjectPrototype(JSObject* prototype, UniquedStringImpl* uidOrNull)
{
    Structure* structure = prototype->structure();
    const ProgramIdentifiers* identifiers = m_vm.aotState().identifiers.get();
    if (!identifiers || !listsEveryNameInItsTable(structure) || !structure->storedPrototype().isNull()) {
        m_namesOfObjectPrototypes.fill(UINT64_MAX);
        return;
    }
    auto add = [&](UniquedStringImpl* uid) {
        std::optional<uint32_t> number = identifiers->numberOf(uid);
        if (number && FieldTableEntry::hasColumn(*number))
            setBitOfName(m_namesOfObjectPrototypes, *number);
    };
    if (uidOrNull) {
        add(uidOrNull);
        return;
    }
    structure->forEachProperty(m_vm, [&](const auto& entry) {
        add(entry.key());
        return true;
    });
}

size_t VMPropertyCaches::maxSizeOfPagesAtStart(const Image& image)
{
    return roundUpToMultipleOf(pageSize(), sizeof(Tables))
        + sizeOfRecords(safeCast<uint32_t>(runtimeLimitValue(&image, LimitID::CustomAccessorRecords)))
        + RowDisplacementTable::sizeOfPagesAtStart(runtimeLimitValue(&image, LimitID::LookupTableBases))
        + sizeOfReservation(static_cast<uint32_t>(std::bit_floor(std::clamp<uint64_t>(runtimeLimitValue(&image, LimitID::HashedTableEntries), HashedSlotTable::initialNumberOfEntries, maxAddressableEntries))));
}

void VMPropertyCaches::publishWords(Instance& instance) const
{
    PropertyCacheWords& words = instance.propertyCaches;
    const uint32_t* rowsForStructureIDZero = StructureRows::ofImageAt(instance.program ? instance.program->position() : 0).rowsForStructureIDZero();
    for (SlotAccess access : allSlotAccesses)
        words.front[static_cast<unsigned>(access)] = { rowsForStructureIDZero, m_front.plane(access) };
    words.hashedSlots = m_hashedTable.entries();
    words.hashedSlotOffsetMask = m_hashedTable.offsetMask();
    words.customAccessors = m_customAccessors.records();
    words.absenceFilters = const_cast<AbsenceFilter*>(m_absenceFilters.data());
    words.absenceFilterGenerationForStores = m_absenceFilterGenerationForStores;
    words.changesUnderNamesOnChains = m_changesUnderNamesOnChains;
    words.namesChangedOnChains = m_namesChangedOnChains.data();
    words.namesOfObjectPrototypes = m_namesOfObjectPrototypes.data();
    words.missesOfPairs = const_cast<uint8_t*>(m_missesOfPairs.data());
    words.namesOfTexts = m_namesOfTexts.data();
    words.infoOfClassPrototypes = ClassPrototype::info();
}

static bool addOwnNamesThatStopStores(VM& vm, const ProgramIdentifiers& identifiers, JSObject* object, AbsenceFilter& filter)
{
    auto addNamesInTable = [&](Structure* structure) {
        if (!listsEveryNameInItsTable(structure))
            return false;
        if (structure->typeInfo().overridesPut() || structure->typeInfo().overridesGetPrototype())
            filter.generationForStores = 0;
        structure->forEachProperty(vm, [&](const auto& entry) {
            std::optional<uint32_t> number = entry.attributes() & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue ? identifiers.numberOf(entry.key()) : std::nullopt;
            if (number)
                filter.addNameThatStopsStores(*number);
            return true;
        });
        return true;
    };
    Structure* structure = object->structure();
    if (structure->isCompiledStructure()) {
        auto* compiled = uncheckedDowncast<CompiledStructure>(structure);
        if (!compiled->describesObject() || (compiled->hasExtraProperties() && !compiled->listsEveryExtraProperty()))
            return false;
        bool isFrozen = uncheckedDowncast<Object>(object)->orderWord() & orderWordIsFrozen;
        for (const FieldData& field : compiled->layout().fields()) {
            if (isFrozen || field.flags & (fieldIsReadOnly | fieldIsAccessor) || field.slot.representation() == SlotRepresentation::DeferredClosure)
                filter.addNameThatStopsStores(field.name);
        }
        return !compiled->hasExtraProperties() || addNamesInTable(compiled->extraPropertiesStructure());
    }
    if (object->classInfo() == ClassPrototype::info()) {
        // A class may extend a class of another image.
        Instance* instance = Instance::owning(object);
        State* state = instance ? instance->state : nullptr;
        if (!state || object->butterfly())
            return false;
        const LayoutData* classLayout = state->objects->classes.classOf(*uncheckedDowncast<ClassPrototype>(object));
        for (const MethodData& method : classLayout->methods()) {
            if (method.flags & methodHasComputedName)
                return false;
            if (method.kind() != MethodKind::Method)
                filter.addNameThatStopsStores(method.name);
        }
        return true;
    }
    return addNamesInTable(structure);
}

void VMPropertyCaches::fileAbsenceFilterForStores(JSGlobalObject* globalObject, JSObject* object)
{
    Structure* structure = object->structure();
    if (structure->hasPolyProto() || structure->typeInfo().overridesGetPrototype())
        return;
    AOT_THRESHOLD(unsigned, maxObjectsAbove, 8);
    AbsenceFilter& filter = m_absenceFilters[AbsenceFilter::indexFor(structure->id().bits())];
    if (filter.structureID == structure->id().bits() && filter.generationForStores == m_absenceFilterGenerationForStores && filter.changesUnderNames == m_changesUnderNamesOnChains)
        return;
    for (JSObject* holder = structure->storedPrototypeObject(); holder; holder = holder->getPrototypeDirect().getObject())
        Object::ensureStructureForExtraPropertiesIfNeeded(m_vm, holder);
    const ProgramIdentifiers* identifiers = m_vm.aotState().identifiers.get();
    if (!identifiers || structure->realm() != globalObject)
        return;
    AbsenceFilter filled { structure->id().bits(), m_absenceFilterGenerationForStores, { }, m_changesUnderNamesOnChains, 0 };
    unsigned numberOfObjects = 0;
    for (JSObject* holder = object->getPrototypeDirect().getObject(); holder; holder = holder->getPrototypeDirect().getObject()) {
        if (++numberOfObjects > maxObjectsAbove || !addOwnNamesThatStopStores(m_vm, *identifiers, holder, filled))
            return;
        m_filteredObjects.add(holder);
    }
    // A compiled object's filter is only queried for the names of its own fields, and many names share a bit. A field whose bit is set is looked up directly, so that it does not pay for another name: the accessor
    // `__proto__` of Object.prototype alone is above nearly every object.
    if (const Object* receiver = compiledObjectOrNull(object)) {
        AbsenceFilter ofAllNames = std::exchange(filled, AbsenceFilter { filled.structureID, filled.generationForStores, { }, filled.changesUnderNames, 0 });
        for (const FieldData& field : receiver->layout().fields()) {
            if (ofAllNames.letsStoresPass(field.name) || !FieldTableEntry::hasColumn(field.name))
                continue;
            ChainWalk walk = walkChain({ globalObject, structure, object, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, m_vm.aotState().identifiers->identifier(field.name).impl());
            if (storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
                filled.addNameThatStopsStores(field.name);
        }
    }
    filter = filled;
}

bool VMPropertyCaches::storeCreatesOwnField(const Object& object, uint32_t name) const
{
    if (!FieldTableEntry::hasColumn(name))
        return false;
    if (object.compiledStructure().flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))
        return !mayBeNameOfObjectPrototype(name);
    const AbsenceFilter& filter = absenceFilterFor(object.structureID());
    return filter.structureID == object.structureID().bits() && filter.generationForStores == m_absenceFilterGenerationForStores && filter.letsStoresPass(name)
        && (filter.changesUnderNames == m_changesUnderNamesOnChains || !wasChangedOnSomeChain(name));
}

bool VMPropertyCaches::addFilterAndCheckStoreCreatesOwnField(JSGlobalObject* globalObject, Object& object, uint32_t name)
{
    if (!FieldTableEntry::hasColumn(name))
        return false;
    if (!object.compiledStructure().flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))
        fileAbsenceFilterForStores(globalObject, &object);
    return storeCreatesOwnField(object, name);
}

// A filter lets a field's name pass where a plain property lower on the chain hides an accessor, so a name that is gone from `changed` may stop stores as well.
static bool mayStopStores(JSObject* changed, UniquedStringImpl* uid)
{
    Structure* structure = changed->structure();
    bool isField = false;
    if (Object* object = compiledObjectOrNull(changed)) {
        const FieldData* field = object->fieldNamed(object->state(), uid);
        if (field && object->isInSlot(*field))
            return object->attributesOf(*field) & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue || field->slot.representation() == SlotRepresentation::DeferredClosure;
        isField = field;
        JSObject* extra = extraPropertiesIfMade(*object);
        if (!extra)
            return true;
        structure = extra->structure();
        if (structure->isUncacheableDictionary())
            return true;
    } else if (structure->isCompiledStructure() || !listsEveryNameInItsTable(structure))
        return true;
    unsigned attributes = 0;
    std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(uid, attributes);
    if (!offset)
        return true;
    return !isValidOffset(*offset) || isField || attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue;
}

void VMPropertyCaches::didChangeFilteredObject(JSObject* changed, UniquedStringImpl* uidOrNull)
{
    const ProgramIdentifiers* identifiers = m_vm.aotState().identifiers.get();
    if (uidOrNull && identifiers) {
        std::optional<uint32_t> number = identifiers->numberOf(uidOrNull);
        if (!number || !FieldTableEntry::hasColumn(*number) || !mayStopStores(changed, uidOrNull))
            return;
        setBitOfName(m_namesChangedOnChains, *number);
        if (!++m_changesUnderNamesOnChains) [[unlikely]]
            m_absenceFilters.fill(AbsenceFilter { });
    } else if (!++m_absenceFilterGenerationForStores) [[unlikely]] {
        m_absenceFilters.fill(AbsenceFilter { });
        m_absenceFilterGenerationForStores = 1;
    }
    for (Instance* instance : m_vm.aotState().instances) {
        if (!serves(*instance))
            continue;
        instance->propertyCaches.absenceFilterGenerationForStores = m_absenceFilterGenerationForStores;
        instance->propertyCaches.changesUnderNamesOnChains = m_changesUnderNamesOnChains;
    }
}

void VMPropertyCaches::didChangeObjectOnSomeChain(JSObject* changed, UniquedStringImpl* uidOrNull)
{
    // Most objects that report a change are still being filled, and no filter has been built from them yet.
    if (!changed || m_filteredObjects.mayContain(changed))
        didChangeFilteredObject(changed, uidOrNull);
    for (Instance* instance : m_vm.aotState().instances) {
        if (!serves(*instance))
            continue;
        if (std::exchange(instance->propertyCaches.placesSayAbsence, 0)) {
            zeroSpan(instance->keyedValuePlaces().first<KeyedValuePlace::numberOfPlacesPerAccess>());
            ++instance->propertyCaches.timesAbsencesWereForgotten;
        }
    }
}

void VMPropertyCaches::didStartWatchingReplacements()
{
    for (Instance* instance : m_vm.aotState().instances) {
        if (!serves(*instance))
            continue;
        if (std::exchange(instance->propertyCaches.placesTakeReplacementsAsUnwatched, 0))
            zeroSpan(instance->keyedValuePlaces().last<KeyedValuePlace::numberOfPlacesPerAccess>());
    }
}

void VMPropertyCaches::set(Structure* structure, const CachedName& name, SlotAccess access, const Slot& slot)
{
    StructureID id = structure->id();
    ASSERT(slot.structureID == id);
    if (std::optional<uint32_t> column = hasFrontCells(access) ? columnForNumber(name.identifierNumber) : std::nullopt) {
        bool isSet = m_front.trySet(structure, *column, access, slot);
        if (m_front.takeDidMovePlanes()) {
            for (Instance* instance : m_vm.aotState().instances) {
                if (serves(*instance))
                    publishWords(*instance);
            }
        }
        if (isSet) {
            if (slot.has(Slot::dependsOnChain)) {
                auto result = m_columnsByNameAddress.add(name.uid, static_cast<uint16_t>(*column));
                ASSERT(result.iterator->value == *column);
                if (result.isNewEntry)
                    name.uid->ref();
            }
            m_hashedTable.remove(id, name.uid, access);
            return;
        }
        m_front.remove(id, *column, access);
    }
    // Evaluated text reads by value what this table holds. The cells in front are for accesses by name alone. No store by value comes here under such a symbol: loadNameOfKeyCell().
    if (slot.kind != Slot::Kind::FieldCreation && isPreparedBeforeLookups(m_vm, name.uid))
        return;
    if (m_hashedTable.set(name.uid, access, slot) == HashedSlotTable::SetResult::Stored)
        return;
    for (Instance* instance : m_vm.aotState().instances) {
        if (serves(*instance))
            publishWords(*instance);
    }
}

std::optional<Slot> VMPropertyCaches::find(StructureID id, const CachedName& name, SlotAccess access) const
{
    if (std::optional<uint32_t> column = hasFrontCells(access) ? columnForNumber(name.identifierNumber) : std::nullopt) {
        if (std::optional<Slot> slot = m_front.find(id, *column, access))
            return slot;
    }
    if (const Slot* slot = m_hashedTable.find(id, name.uid, access))
        return *slot;
    return std::nullopt;
}

void VMPropertyCaches::remove(StructureID id, const CachedName& name, SlotAccess access)
{
    const ProgramIdentifiers* identifiers = m_vm.aotState().identifiers.get();
    if (std::optional<uint32_t> column = hasFrontCells(access) ? columnForNumber(name.identifierNumber || !identifiers ? name.identifierNumber : identifiers->numberOf(name.uid)) : std::nullopt)
        m_front.remove(id, *column, access);
    m_hashedTable.remove(id, name.uid, access);
}

void VMPropertyCaches::dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uid)
{
    ASSERT(uid);
    StructureID id = std::bit_cast<StructureID>(structureIDBits);
    if (std::optional<uint16_t> column = m_columnsByNameAddress.getOptional(uid))
        m_front.removeChainDependentCells(id, *column);
    m_hashedTable.removeChainDependentEntries(id, uid);
}

bool VMPropertyCaches::hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl* uid) const
{
    ASSERT(uid);
    StructureID id = std::bit_cast<StructureID>(structureIDBits);
    std::optional<uint16_t> column = m_columnsByNameAddress.getOptional(uid);
    return (column && m_front.hasChainDependentCell(id, *column)) || m_hashedTable.hasChainDependentEntry(id, uid);
}

namespace {

struct CreatedMember {
    static std::optional<CreatedMember> find(VM& vm, const CreatedMembers& created, UniquedStringImpl* uid)
    {
        unsigned attributes = 0;
        PropertyOffset offset = created.prototype->structure()->get(vm, uid, attributes);
        if (!isValidOffset(offset))
            return std::nullopt;
        return CreatedMember { uid, offset, attributes };
    }

    UniquedStringImpl* uid;
    PropertyOffset offset;
    unsigned attributes;
};

}

static std::optional<Slot> slotBehindCreatedMember(const Slot& slot, const CreatedMember& member, const CreatedMembers& created)
{
    if (!slot.structureID || slot.kind == Slot::Kind::Uncacheable)
        return std::nullopt;
    Structure* structure = slot.structureID.decode();
    if (!structure->isObject() || !structure->hasMonoProto())
        return std::nullopt;
    JSObject* prototype = created.prototype;
    auto [uid, offset, attributes] = member;
    for (JSObject* object = structure->storedPrototypeObject(); object != prototype; object = object->getPrototypeDirect().getObject()) {
        if (!object)
            return std::nullopt;
        if (object->classInfo() == ClassPrototype::info() && ClassPrototype::hasMember(object, uid))
            return slot;
    }
    bool isValue = slot.kind == Slot::Kind::Value;
    if ((!isValue && slot.kind != Slot::Kind::Getter && slot.kind != Slot::Kind::Setter) || slot.has(Slot::isOwn) || slot.holder->structure() != created.structureOfDeclaredMemberHolders)
        return std::nullopt;
    JSValue kept = valueAtOffset(asObject(slot.holder), slot.offsetInStorage());
    JSValue made = prototype->getDirect(offset);
    if (isValue) {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue || kept != made)
            return std::nullopt;
    } else {
        if (!(attributes & PropertyAttribute::Accessor))
            return std::nullopt;
        auto* keptAccessor = uncheckedDowncast<GetterSetter>(kept.asCell());
        auto* madeAccessor = uncheckedDowncast<GetterSetter>(made.asCell());
        if (keptAccessor->getter() != madeAccessor->getter() || keptAccessor->setter() != madeAccessor->setter())
            return std::nullopt;
    }
    Slot moved = Slot::createInStorage(slot.structureID, slot.kind, slot.flags & ~Slot::isOutOfLine, prototype, offset);
    if (moved.kind != slot.kind)
        return std::nullopt;
    return moved;
}

static bool isSameSlot(const Slot& a, const Slot& b)
{
    return a.headerBits() == b.headerBits() && a.payloadBits() == b.payloadBits();
}

static bool belongsToOneObject(Structure*);

bool VMPropertyCaches::keepChainDependent(VM& vm, uint32_t structureIDBits, UniquedStringImpl* uid, const CreatedMembers& created)
{
    ASSERT(uid);
    ASSERT(vm.heap.isDeferred());
    std::optional<CreatedMember> member = CreatedMember::find(vm, created, uid);
    if (!member)
        return true;
    StructureID id = std::bit_cast<StructureID>(structureIDBits);
    std::optional<uint16_t> column = m_columnsByNameAddress.getOptional(uid);
    CachedName name { uid, column ? std::optional<uint32_t>(*column) : std::nullopt };
    bool keepsSome = false;
    for (SlotAccess access : { SlotAccess::Read, SlotAccess::Store, SlotAccess::Define, SlotAccess::Delete }) {
        std::optional<Slot> slot = find(id, name, access);
        if (!slot || !slot->has(Slot::dependsOnChain))
            continue;
        std::optional<Slot> kept = slotBehindCreatedMember(*slot, *member, created);
        if (!kept) {
            if (column && hasFrontCells(access))
                m_front.remove(id, *column, access);
            m_hashedTable.remove(id, uid, access);
            continue;
        }
        keepsSome = true;
        if (isSameSlot(*kept, *slot))
            continue;
        Structure* structure = id.decode();
        set(structure, belongsToOneObject(structure) ? CachedName { uid, std::nullopt } : name, access, *kept);
    }
    return keepsSome;
}

void initializePropertyCacheWords(Instance& instance)
{
    VM& vm = *instance.vm;
    JSGlobalObject* globalObject = instance.globalObject;
    PropertyCacheWords& words = instance.propertyCaches;
    Structure* numberStructure = globalObject->numberObjectStructureConcurrently();
    Structure* booleanStructure = globalObject->booleanObjectStructureConcurrently();
    Structure* structureOfNoReceiver = vm.structureStructure.get();
    words.numberStructureID = (numberStructure ? numberStructure : structureOfNoReceiver)->id().bits();
    words.booleanStructureID = (booleanStructure ? booleanStructure : structureOfNoReceiver)->id().bits();
    words.hasInstanceUID = vm.propertyNames->hasInstanceSymbol.impl();
    words.prototypeUID = vm.propertyNames->prototype.impl();
    words.lengthUID = vm.propertyNames->length.impl();
    words.nameUID = vm.propertyNames->name.impl();
    words.lookupOfOrdinaryObjects = std::bit_cast<const void*>(JSFinalObject::info()->methodTable.getOwnPropertySlot);
    words.lookupOfArrays = std::bit_cast<const void*>(JSArray::info()->methodTable.getOwnPropertySlot);
    words.lookupOfFunctions = std::bit_cast<const void*>(JSFunction::info()->methodTable.getOwnPropertySlot);
    auto setNumberOfName = [&](TrapOfProxy trap, const Identifier& name) {
        words.numbersOfNamesOfTraps[static_cast<unsigned>(trap)] = vm.aotState().identifiers->numberOf(name.impl()).value_or(0);
    };
    setNumberOfName(TrapOfProxy::Get, vm.propertyNames->get);
    setNumberOfName(TrapOfProxy::Has, vm.propertyNames->has);
    setNumberOfName(TrapOfProxy::Set, vm.propertyNames->set);
    words.defaultHasInstanceFunction = globalObject->functionProtoHasInstanceSymbolFunction();
    const Image& image = *vm.aotState().image;
    if (image.sharedCodeCopies().empty())
        return;
    uint8_t* sharedCode = const_cast<uint8_t*>(image.sharedCodeCopy(0));
    std::span offsets = image.slotHandlerOffsets();
    std::span handlers(&instance.slotHandlers()[0][0], numberOfSlotHandlerTables * numberOfSlotKindsWithHandlers);
    RELEASE_ASSERT(offsets.size() == handlers.size());
    for (size_t index = 0; index < handlers.size(); ++index)
        handlers[index] = offsets[index] == offsetOfAbsentStub ? nullptr : sharedCode + offsets[index];
}

template<typename T>
CreatedAtFirstUse<T>::CreatedAtFirstUse(State& state)
    : m_state(state)
    , m_value(state.tableArena)
{
}

template<typename T>
CreatedAtFirstUse<T>::~CreatedAtFirstUse() = default;

template<typename T>
NEVER_INLINE T& CreatedAtFirstUse<T>::create()
{
    return m_value.make(m_state);
}

// The first use may come while another image of the realm runs.
static VMPropertyCaches& tablesOfImageOf(const State& state)
{
    VMState& vmState = state.instance.vm->aotState();
    if (!vmState.hasSeveralImages) [[likely]]
        return *vmState.propertyCaches;
    return *vmState.parts[state.instance.program->position()].propertyCaches;
}

template class CreatedAtFirstUse<PropertyCaches>;
template class CreatedAtFirstUse<StringKeyCache>;

PropertyCaches::PropertyCaches(State& state)
    : m_state(state)
    , m_vm(*state.instance.vm)
    , m_tables(tablesOfImageOf(state))
    , m_tablesOwnerID(state.chainDependents->addOwner(m_tables))
    , m_siteCaches(*state.siteCaches)
    , m_watchedValues(state)
{
    state.caches->add(CacheKind::StrongRoots, "Structures of additions in an Eden collection"_s, *this);
    state.caches->addObserver("PropertyCaches"_s, *this);
    state.caches->addClearedEachCollection("exits of fields"_s, asMutableByteSpan(std::span { m_fieldExits }));
    state.instance.propertyCaches.fieldExits = m_fieldExits.data();
    if (Options::aotCountsPath())
        m_countedFilings = makeUnique<UncheckedKeyHashSet<uint64_t>>();
}

PropertyCaches::~PropertyCaches() = default;

void PropertyCaches::didAddProperty(Structure* before, Structure* after)
{
    if (before == after || after->isDictionary())
        return;
    if (const CompiledStructure* compiled = compiledObjectStructureOrNull(after); compiled && compiled->hasOutOfLineKeyOrder())
        return;
    Locker locker { m_state.caches->lock() };
    m_additionsSinceCollection.append({ before->id(), after->id() });
}

JSCell* PropertyCaches::keyForTraps(UniquedStringImpl* uid)
{
    bool isKept = !uid->isSymbol() && m_vm.aotState().identifiers->numberOf(uid);
    if (isKept) {
        if (JSCell* key = m_keysForTraps.get(uid))
            return key;
    }
    JSCell* key = identifierToSafePublicJSValue(m_vm, Identifier::fromUid(m_vm, uid)).asCell();
    if (isKept) {
        Locker locker { m_state.caches->lock() };
        m_keysForTraps.add(uid, key);
    }
    return key;
}

void PropertyCaches::prepareHoldersBeforeLookupsUnder(Symbol* symbol, JSObject* function)
{
    Locker locker { m_state.caches->lock() };
    m_preparationsBeforeLookups.set(&symbol->uid(), PreparationBeforeLookups { symbol, function });
    symbol->setPerCellBit(true);
}

template<typename Visitor>
void PropertyCaches::visitRoots(Visitor& visitor)
{
    for (JSCell* key : m_keysForTraps.values())
        visitor.appendUnbarriered(key);
    for (const PreparationBeforeLookups& preparation : m_preparationsBeforeLookups.values()) {
        visitor.appendUnbarriered(preparation.symbol);
        visitor.appendUnbarriered(preparation.function);
    }
    if (visitor.collectionScope() != CollectionScope::Eden)
        return;
    auto keepsStructure = [&](const Addition& addition) {
        if (!visitor.isMarked(addition.before.decode()))
            return false;
        visitor.appendUnbarriered(addition.after.decode());
        return true;
    };
    if constexpr (std::is_same_v<Visitor, SlotVisitor>)
        m_additionsSinceCollection.removeAllMatching(keepsStructure);
    else {
        for (const Addition& addition : m_additionsSinceCollection)
            keepsStructure(addition);
    }
}

template void PropertyCaches::visitRoots(AbstractSlotVisitor&);
template void PropertyCaches::visitRoots(SlotVisitor&);

void PropertyCaches::ensureChangesAreReported(std::span<JSObject* const> objects)
{
    for (JSObject* object : objects) {
        if (object->mayBePrototype()) [[likely]]
            continue;
        DeferGCForAWhile deferGC(m_vm);
        object->didBecomePrototype(m_vm);
    }
}

CachedName PropertyCaches::nameOf(uint32_t identifierNumber) const
{
    return { m_vm.aotState().identifiers->identifier(identifierNumber).impl(), identifierNumber };
}

CachedName PropertyCaches::nameOf(UniquedStringImpl* uid) const
{
    return { uid, m_vm.aotState().identifiers->numberOf(uid) };
}

bool PropertyCaches::canCache(Structure* structure, const PropertyAnswer& answer) const
{
    if (structure->isDictionary() && (structure->isUncacheableDictionary() || answer.kind != PropertyAnswerKind::Value || !answer.isOwn || answer.dependsOnChain()))
        return false;
    bool isVariable = answer.kind == PropertyAnswerKind::Variable;
    if (answer.dependsOnChain() || isVariable) {
        JSGlobalObject* realm = structure->realm();
        if (realm && Instance::of(realm) != &m_state.instance)
            return false;
    }
    return !m_tables.isSharedByInstances() || (!isVariable && structure->typeInfo().isObject());
}

static bool belongsToOneObject(Structure* structure)
{
    if (structure->isDictionary() || structure->typeInfo().type() == GlobalProxyType)
        return true;
    const CompiledStructure* compiled = compiledObjectStructureOrNull(structure);
    return compiled && ((compiled->extraPropertiesStructure() && !compiled->listsEveryExtraProperty()) || compiled->hasKeyOrderOfOneObject());
}

void PropertyCaches::cache(const CachedAccess& access, const PropertyAnswer& answer)
{
    if (answer.didFlatten) {
        didFileNothing();
        return;
    }
    ASSERT(answer.access == access.access);
    Structure* structure = access.structure;
    if (!canCache(structure, answer)) {
        count(m_state.instance, RuntimeCounterID::AnswersNotFiled);
        return;
    }
    if (m_countedFilings) [[unlikely]]
        countFiling(access);
    if (structure == m_state.instance.globalObject->numberObjectStructureConcurrently())
        m_state.instance.propertyCaches.numberStructureID = structure->id().bits();
    else if (structure == m_state.instance.globalObject->booleanObjectStructureConcurrently())
        m_state.instance.propertyCaches.booleanStructureID = structure->id().bits();
    ensureChangesAreReported(answer.changeableObjects.span());
    Slot slot = slotFor(answer, structure);
    if (validatesRuntime(*m_state.instance.vm)) [[unlikely]]
        m_tables.validateImageOfSlot(*m_state.instance.vm, slot, access.access, access.name.uid);
    m_tables.set(structure, belongsToOneObject(structure) ? CachedName { access.name.uid, std::nullopt } : access.name, access.access, slot);
    if (slot.has(Slot::dependsOnChain))
        m_state.chainDependents->add(answer.changeableObjects.span(), { access.name.uid, structure->id().bits(), m_tablesOwnerID });
    if (access.site)
        m_siteCaches.fill(access.site, slot, { access.name.uid, access.access, answer.changeableObjects.span() });
    if (answer.newStructure)
        didAddProperty(structure, answer.newStructure);
}

void PropertyCaches::countFiling(const CachedAccess& access)
{
    StructureID id = access.structure->id();
    uint64_t pair = (static_cast<uint64_t>(id.bits()) << 32 | WTF::PtrHash<UniquedStringImpl*>::hash(access.name.uid)) + static_cast<uint64_t>(access.access) + 1;
    if (m_countedFilings->add(pair).isNewEntry)
        return;
    count(m_state.instance, RuntimeCounterID::PairsFiledAgain);
    static constexpr std::array kindNames {
#define AOT_NAME_SLOT_KIND(name, payload, location, presence, kindFlag) #name ""_s,
        FOR_EACH_AOT_SLOT_KIND(AOT_NAME_SLOT_KIND)
#undef AOT_NAME_SLOT_KIND
    };
    std::optional<Slot> present = m_tables.find(id, nameOf(access.name.uid), access.access);
    if (!present) {
        countNamed(access.access == SlotAccess::Read ? "read filed again"_s : "other access filed again"_s, access.name.uid);
        return;
    }
    count(m_state.instance, RuntimeCounterID::PairsFiledAgainWhilePresent);
    countNamed(kindNames[static_cast<size_t>(present->kind)], access.name.uid);
}

void PropertyCaches::countNamed(ASCIILiteral what, UniquedStringImpl* name)
{
    if (!m_countedFilings)
        return;
    ++m_namedCounts.add(makeString(what, ':', StringView(name)), 0).iterator->value;
}

static ASCIILiteral describeReceiverOfMiss(JSValue receiver)
{
    if (!receiver.isCell())
        return "primitive"_s;
    JSCell* cell = receiver.asCell();
    if (PropertyWalk::isCompiledObject(cell)) {
        auto& structure = *uncheckedDowncast<CompiledStructure>(cell->structure());
        if (structure.hasExtraProperties())
            return structure.hasOtherPrototype() ? "compiled object with extra properties and another prototype"_s : "compiled object with extra properties"_s;
        return structure.hasOtherPrototype() ? "compiled object with another prototype"_s : "compiled object as its layout says"_s;
    }
    if (cell->type() == FinalObjectType)
        return cell->structure()->isDictionary() ? "ordinary object, dictionary"_s : "ordinary object"_s;
    if (cell->type() == SlimClosureType) {
        auto* closure = uncheckedDowncast<SlimClosure>(cell);
        if (closure->isBound())
            return closure->perCellBit() ? "slim closure, bound, name read before"_s : "slim closure, bound"_s;
        return closure->perCellBit() ? "slim closure, name read before"_s : "slim closure"_s;
    }
    return cell->classInfo()->className;
}

static ASCIILiteral describeFieldOfMiss(JSValue receiver, UniquedStringImpl* name)
{
    Object* object = compiledObjectOrNull(receiver);
    if (!object)
        return ""_s;
    const FieldData* field = object->fieldNamed(object->state(), name);
    if (!field)
        return ", no such field"_s;
    if (field->isAccessor())
        return ", an accessor field"_s;
    return object->isInSlot(*field) ? ", a field in its slot"_s : ", a field outside its slot"_s;
}

// For the counts alone: why answerFromObjectsAndReturn(), the walk of the stubs over the receiver and the objects above it, left a name to the run time. The tests are those of the stub, in its order.
static ASCIILiteral whyObjectsDidNotAnswer(VM& vm, JSValue receiver, UniquedStringImpl* uid)
{
    constexpr unsigned flagsOfOwnLookup = OverridesGetOwnPropertySlot | OverridesGetPrototype;
    if (!receiver.isCell())
        return "the receiver is no cell"_s;
    if (!receiver.isObject())
        return "the receiver is a cell and no object"_s;
    if (!uid->length())
        return "the empty name"_s;
    bool isReceiver = true;
    auto at = [&](ASCIILiteral ofReceiver, ASCIILiteral ofObjectAbove) { return isReceiver ? ofReceiver : ofObjectAbove; };
    enum class InTable : uint8_t { Absent, Value, Stops };
    ASCIILiteral stop;
    bool hasElementsThatDoNotMatter = false;
    auto askPropertyTable = [&](JSObject* object, ASCIILiteral hasElements, ASCIILiteral hasNoTable, ASCIILiteral isCustom, ASCIILiteral isAccessor) {
        if (object->indexingType() & IndexingShapeMask && !hasElementsThatDoNotMatter) {
            stop = hasElements;
            return InTable::Stops;
        }
        Structure* structure = object->structure();
        if (structure->seenProperties().ruleOut(CompactPtr<UniquedStringImpl>::encode(uid)))
            return InTable::Absent;
        PropertyTable* table = *std::bit_cast<PropertyTable* const*>(std::bit_cast<const uint8_t*>(structure) + Structure::propertyTableUnsafeOffset());
        if (!table) {
            stop = hasNoTable;
            return InTable::Stops;
        }
        auto [offset, attributes] = table->get(uid);
        if (!isValidOffset(offset))
            return InTable::Absent;
        if (attributes & PropertyAttribute::CustomAccessorOrValue) {
            stop = isCustom;
            return InTable::Stops;
        }
        if (attributes & PropertyAttribute::Accessor) {
            stop = isAccessor;
            return InTable::Stops;
        }
        return InTable::Value;
    };
    for (JSObject* object = asObject(receiver);; isReceiver = false) {
        bool asksPropertyTable = !(object->inlineTypeFlags() & flagsOfOwnLookup);
        hasElementsThatDoNotMatter = false;
        JSType type = object->type();
        if (!asksPropertyTable) {
            if (uid->isSymbol())
                return at("a symbol, and the receiver has a lookup of its own"_s, "a symbol, and an object above has a lookup of its own"_s);
            if (type == CompiledObjectType) {
                Object& compiled = *uncheckedDowncast<Object>(object);
                std::optional<uint32_t> number = vm.aotState().identifiers->numberOf(uid);
                if (number && !compiled.state().tables.hasFieldTableColumn(*number))
                    return at("the name has no column, and the receiver is a compiled object"_s, "the name has no column, and an object above is a compiled object"_s);
                if (const FieldData* field = number ? compiled.fieldNamed(compiled.state(), uid) : nullptr) {
                    if (field->isPrivate())
                        return "a private field"_s;
                    if (compiled.isInSlot(*field)) {
                        if (field->isAccessor())
                            return at("an accessor field of the receiver, in its slot"_s, "an accessor field of an object above, in its slot"_s);
                        if (field->slot.representation() == SlotRepresentation::DeferredClosure)
                            return at("a closure of the receiver that is not made yet"_s, "a closure of an object above that is not made yet"_s);
                        return at("a field of the receiver in its slot, in a form that the stub does not read"_s, "a field of an object above in its slot, in a form that the stub does not read"_s);
                    }
                    if (field->flags & fieldMayExistWithoutStorage)
                        return at("a field of the receiver that may exist without storage"_s, "a field of an object above that may exist without storage"_s);
                }
                if (compiled.hasExtraProperties()) {
                    JSObject* extra = compiled.compiledStructure().extraPropertiesSlot() ? extraPropertiesIfMade(compiled) : nullptr;
                    if (!extra || extra->type() != FinalObjectType)
                        return at("the extra properties of the receiver are not where the stub looks"_s, "the extra properties of an object above are not where the stub looks"_s);
                    switch (askPropertyTable(extra, "the extra properties have elements"_s, at("the extra properties of the receiver have no property table"_s, "the extra properties of an object above have no property table"_s), "an extra property is a custom accessor or value"_s, "an extra property is an accessor: the stub could not call the getter"_s)) {
                    case InTable::Stops:
                        return stop;
                    case InTable::Value:
                        return "an extra property that is a plain value: NOTHING STOOD IN THE WAY"_s;
                    case InTable::Absent:
                        break;
                    }
                }
            } else if (type == CompiledClosureType || isSlimClosureType(type)) {
                if (uid == vm.propertyNames->name.impl() || uid == vm.propertyNames->length.impl() || uid == vm.propertyNames->prototype.impl())
                    return at("name, length or prototype of a function object"_s, "name, length or prototype, and a function object is above"_s);
                if (type == CompiledClosureType && uncheckedDowncast<Closure>(object)->kind().isConstructor)
                    return at("the receiver is a class object"_s, "a class object is above"_s);
                asksPropertyTable = type != SlimClosureType;
            } else if (object->classInfo() == ClassPrototype::info()) {
                if (object->butterfly())
                    return "a prototype object of a class that has been given something"_s;
                if (ClassPrototype::hasMember(object, uid))
                    return "a member that a class declares: its closure is not made yet, or the stub could not call the getter"_s;
            } else if (!(object->inlineTypeFlags() & OverridesGetPrototype) && object->methodTable()->getOwnPropertySlot == JSFinalObject::info()->methodTable.getOwnPropertySlot)
                asksPropertyTable = true;
            else if (!(object->inlineTypeFlags() & OverridesGetPrototype) && object->methodTable()->getOwnPropertySlot == JSArray::info()->methodTable.getOwnPropertySlot) {
                if (uid == vm.propertyNames->length.impl() || isASCIIDigit((*uid)[0]))
                    return at("the length of an array, or a name that begins with a digit"_s, "the length, or a name that begins with a digit, and an array is above"_s);
                asksPropertyTable = true;
                hasElementsThatDoNotMatter = true;
            } else if (!(object->inlineTypeFlags() & OverridesGetPrototype) && object->methodTable()->getOwnPropertySlot == JSFunction::info()->methodTable.getOwnPropertySlot) {
                if (uid == vm.propertyNames->name.impl() || uid == vm.propertyNames->length.impl() || uid == vm.propertyNames->prototype.impl())
                    return at("name, length or prototype of a function of the engine or the host"_s, "name, length or prototype, and a function of the engine or the host is above"_s);
                asksPropertyTable = true;
            } else if (type == ProxyObjectType)
                return at("the receiver is a proxy"_s, "a proxy is above"_s);
            else
                return at("the receiver is of another class with a lookup of its own"_s, "an object of another class with a lookup of its own is above"_s);
        }
        if (asksPropertyTable) {
            switch (askPropertyTable(object, at("the receiver has elements"_s, "an object above has elements"_s), at("the receiver has no property table"_s, "an object above has no property table"_s), at("a custom accessor or value of the receiver"_s, "a custom accessor or value of an object above"_s), "an accessor: the stub could not call the getter"_s)) {
            case InTable::Stops:
                return stop;
            case InTable::Value:
                return "a plain value: NOTHING STOOD IN THE WAY"_s;
            case InTable::Absent:
                break;
            }
            if (TypeInfo::hasStaticPropertyTable(object->inlineTypeFlags()) && !object->structure()->staticPropertiesReified())
                return at("the receiver has a static table that is not all in its property table"_s, "an object above has a static table that is not all in its property table"_s);
        }
        JSValue above = object->structure()->storedPrototype();
        if (!above)
            return "an object that has its prototype in a slot of its own"_s;
        if (!above.isObject())
            return "nothing has the name: NOTHING STOOD IN THE WAY"_s;
        object = asObject(above);
    }
}

NEVER_INLINE void PropertyCaches::countMiss(ASCIILiteral access, bool shouldFile, JSValue receiver, UniquedStringImpl* name)
{
    if (!m_countedFilings)
        return;
    if (access == "read"_s)
        ++m_namedCounts.add(makeString("miss by reason:read:"_s, shouldFile && !m_counterOfPairToFile ? "one miss in sixteen that the stub sends to be filed"_s : whyObjectsDidNotAnswer(m_vm, receiver, name)), 0).iterator->value;
    ASCIILiteral which = shouldFile ? "later"_s : "first"_s;
    ASCIILiteral ofReceiver = describeReceiverOfMiss(receiver);
    ASCIILiteral ofField = describeFieldOfMiss(receiver, name);
    ++m_namedCounts.add(makeString("miss by receiver:"_s, access, ':', which, ':', ofReceiver, ofField), 0).iterator->value;
    ++m_namedCounts.add(makeString("miss by name:"_s, access, ':', which, ':', StringView(name)), 0).iterator->value;
    auto frames = CompiledFrameIterator::tryCreateAtTop(m_vm);
    if (!frames || frames->isAtEnd())
        return;
    uintptr_t offset = std::bit_cast<uintptr_t>(frames->callSite().address) - std::bit_cast<uintptr_t>(m_vm.aotState().image->code().data());
    ++m_namedCounts.add(makeString("miss by site:"_s, access, ':', offset, ':', StringView(name), ':', ofReceiver, ofField), 0).iterator->value;
}

void PropertyCaches::dumpNamedCounts(PrintStream& out) const
{
    for (auto& [name, times] : m_namedCounts)
        out.println(times, "\tnamed:", name);
}

bool PropertyCaches::shouldFile(StructureID structureID, UniquedStringImpl* uid, SlotAccess access)
{
    m_counterOfPairToFile = nullptr;
    if (std::exchange(m_state.instance.propertyCaches.missIsDueForFiling, 0))
        return true;
    if (m_shouldForgetMisses.load(std::memory_order_relaxed)) [[unlikely]] {
        m_shouldForgetMisses.store(false, std::memory_order_relaxed);
        m_missCounters.fill(0);
    }
    constexpr uint8_t maskOfCount = (1u << bitsOfMissCount) - 1;
    constexpr uint8_t wasFiled = maskOfCount;
    constexpr uint32_t countersOfPair = 4;
    static_assert(hasOneBitSet(numberOfMissCounters) && numberOfMissCounters <= 1u << 24 && missesBeforeFiling < wasFiled && storeMissesBeforeFiling < wasFiled);
    static_assert(hasOneBitSet(countersOfPair) && countersOfPair <= numberOfMissCounters);
    uint32_t hash = WTF::pairIntHash(structureID.bits(), WTF::PtrHash<UniquedStringImpl*>::hash(uid) + static_cast<uint32_t>(access));
    uint32_t first = hash % numberOfMissCounters;
    uint8_t bitsOfPair = static_cast<uint8_t>(hash >> 24) & ~maskOfCount;
    uint8_t* ofPair = nullptr;
    uint8_t* unused = nullptr;
    for (uint32_t step = 0; step < countersOfPair && !ofPair; ++step) {
        uint8_t& candidate = m_missCounters[first ^ step];
        uint8_t misses = candidate & maskOfCount;
        if (misses && (candidate & ~maskOfCount) == bitsOfPair)
            ofPair = &candidate;
        else if (!unused && (!misses || misses == wasFiled))
            unused = &candidate;
    }
    if (!ofPair) {
        ofPair = unused ? unused : &m_missCounters[first];
        *ofPair = bitsOfPair;
    }
    uint8_t& counter = *ofPair;
    if ((counter & maskOfCount) >= (access == SlotAccess::Read ? missesBeforeFiling : storeMissesBeforeFiling)) {
        counter |= wasFiled;
        m_counterOfPairToFile = &counter;
        return true;
    }
    ++counter;
    count(m_state.instance, RuntimeCounterID::FirstMissesNotFiled);
    return false;
}

void PropertyCaches::didFileNothing()
{
    if (uint8_t* counter = std::exchange(m_counterOfPairToFile, nullptr))
        *counter = 0;
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(FirstAdditions);

FirstAdditions::FirstAdditions(State& state)
    : m_pairs(static_cast<uint32_t*>(state.pages.allocate(numberOfPairs * sizeof(uint32_t))), numberOfPairs)
{
    state.caches->add(CacheKind::WeakByKey, "objects being filled"_s, *this);
}

FirstAdditions& firstAdditionsOf(State& state)
{
    if (!state.firstAdditions)
        state.firstAdditions = makeUnique<FirstAdditions>(state);
    return *state.firstAdditions;
}

bool FirstAdditions::shouldMakeStructure(Structure* structure, UniquedStringImpl* uid)
{
    if (structure->id() == m_structureMade)
        return true;
    // A pair that came once has the highest bit set. One that came again has it clear: it is still found once nothing holds its Structure any more, until a newcomer takes its place. Zero stands for no pair.
    constexpr uint32_t cameOnce = 1u << 31;
    uint32_t hash = WTF::pairIntHash(structure->id().bits(), uid->existingSymbolAwareHash()) | cameOnce;
    constexpr uint32_t numberOfSets = numberOfPairs / pairsPerSet;
    static_assert(hasOneBitSet(numberOfSets) && !(pairsPerSet % SIMD::stride<uint32_t>));
    uint32_t* set = &m_pairs[hash % numberOfSets * pairsPerSet];
    auto findInSet = [&](auto matches) -> uint32_t* {
        for (uint32_t* places = set; places < set + pairsPerSet; places += SIMD::stride<uint32_t>) {
            if (std::optional<uint8_t> index = SIMD::findFirstNonZeroIndex(matches(SIMD::load(places))))
                return places + *index;
        }
        return nullptr;
    };
    auto holdsPair = [&](auto words) {
        return SIMD::equal(SIMD::bitOr(words, SIMD::splat32(cameOnce)), SIMD::splat32(hash));
    };
    if (uint32_t* found = findInSet(holdsPair)) {
        // Newcomers take the lowest place that they may, so it moves above the free ones.
        *found = 0;
        uint32_t* highestFree = set + pairsPerSet - 1;
        while (*highestFree)
            --highestFree;
        *highestFree = hash & ~cameOnce;
        return true;
    }
    auto isForNewcomers = [&](auto words) {
        return SIMD::lessThan(words, SIMD::splat32(cameOnce));
    };
    uint32_t* place = findInSet(isForNewcomers);
    if (!place) {
        // Each place of a full set is replaced half as often as the one before it, so a pair is remembered until it returns whether few or many others arrive in between.
        m_evictionSeed = m_evictionSeed * 1664525 + 1013904223;
        place = set + std::min<uint32_t>(std::countl_zero(m_evictionSeed), pairsPerSet - 1);
    }
    *place = hash;
    return false;
}

// What was a dictionary only while it was given its names has not grown into one: its Structure is like any other. Other objects may share it, what is added to them is remembered in it,
// and it may still grow into a dictionary, which is then made flat once.
void didFinishFilling(Structure* structure)
{
    ASSERT(!structure->isDictionary());
    structure->setHasBeenFlattenedBefore(false);
    structure->setHasBeenDictionary(false);
}

bool hasFewEnoughNamesToBeFilled(Structure* structure)
{
    AOT_THRESHOLD(unsigned, maxNamesCopiedIntoTable, 64);
    return numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity()) <= maxNamesCopiedIntoTable;
}

// Nothing of a cacheable dictionary moves when it is made flat, and no cell is allocated.
static void makeFlatWhatWasFilled(VM& vm, JSObject* object)
{
    if (!object)
        return;
    Structure* structure = object->structure();
    if (!structure->isDictionary() || structure->isUncacheableDictionary() || structure->hasBeenFlattenedBefore())
        return;
    structure->flattenDictionaryStructure(vm, object);
    didFinishFilling(structure);
}

bool FirstAdditions::forget(JSObject* object)
{
    for (JSObject*& filled : m_objectsBeingFilled) {
        if (filled == object) {
            filled = nullptr;
            return true;
        }
    }
    return false;
}

bool FirstAdditions::mayBeginToFill(JSObject* object) const
{
    AOT_THRESHOLD(uintptr_t, maxTimesOneObjectIsFilled, 4);
    static_assert(maxTimesOneObjectIsFilled < timesMask);
    uintptr_t times = m_timesObjectsBeganToFill[placeOfTimes(object)] ^ std::bit_cast<uintptr_t>(object);
    return times > timesMask || times < maxTimesOneObjectIsFilled;
}

void FirstAdditions::didBeginToFill(VM& vm, JSObject* object)
{
    uintptr_t& times = m_timesObjectsBeganToFill[placeOfTimes(object)];
    times = (times ^ std::bit_cast<uintptr_t>(object)) > timesMask ? std::bit_cast<uintptr_t>(object) | 1 : times + 1;
    makeFlatWhatWasFilled(vm, std::exchange(m_objectsBeingFilled[m_numberOfObjectsFilled++ % numberOfObjectsBeingFilled], object));
    vm.aotState().hasObjectsBeingFilled = true;
}

void FirstAdditions::finishFilling(VM& vm)
{
    for (JSObject*& object : m_objectsBeingFilled)
        makeFlatWhatWasFilled(vm, std::exchange(object, nullptr));
}

size_t FirstAdditions::removeDeadEntries(VM& vm, CollectionScope)
{
    m_timesObjectsBeganToFill.fill(0);
    size_t numberOfDeadObjects = 0;
    for (JSObject*& object : m_objectsBeingFilled) {
        if (object && !vm.heap.isMarked(object)) {
            object = nullptr;
            ++numberOfDeadObjects;
        }
    }
    return numberOfDeadObjects;
}

const Slot* PropertyCaches::findByKey(Structure* structure, UniquedStringImpl* uid, SlotAccess access)
{
    StructureID id = structure->id();
    if (const Slot* slot = m_tables.hashedTable().find(id, uid, access)) [[likely]]
        return slot;
    std::optional<Slot> slot = m_tables.find(id, nameOf(uid), access);
    if (!slot)
        return nullptr;
    m_tables.set(structure, { uid, std::nullopt }, access, *slot);
    return m_tables.hashedTable().find(id, uid, access);
}

void PropertyCaches::fillSiteCacheFromTables(SiteCache site, JSCell* base, uint32_t identifierNumber, SlotAccess access)
{
    Structure* structure = base->structure();
    StructureID id = structure->id();
    CachedName name = nameOf(identifierNumber);
    std::optional<Slot> slot = m_tables.find(id, name, access);
    if (!slot) {
        m_siteCaches.didFailToFill(site);
        return;
    }
    m_siteCaches.didMissRepeatedly(site);
    if (!belongsToOneObject(structure) && RowDisplacementTable::hasColumn(identifierNumber) && !m_tables.front().find(id, identifierNumber, access))
        m_tables.set(structure, name, access, *slot);
    if (!slot->has(Slot::dependsOnChain)) {
        m_siteCaches.fill(site, *slot, { name.uid, access, { } });
        return;
    }
    JSGlobalObject* globalObject = m_state.instance.globalObject;
    ChainWalkUse use = access == SlotAccess::Read ? ChainWalkUse::KeptForReadsThatAskObjects : ChainWalkUse::KeptForStores;
    ChainWalk walk = structure->typeInfo().type() == GlobalProxyType
        ? walkChainThroughGlobalProxy(globalObject, *slot, name.uid)
        : walkChain({ globalObject, structure, base->isObject() ? asObject(base) : nullptr, ChainWalkMode::AllocatesNothing, use }, name.uid);
    if (walk.result == ChainWalkResult::Unknown) {
        m_siteCaches.didFailToFill(site);
        return;
    }
    ensureChangesAreReported(walk.changeableObjects.span());
    m_siteCaches.fill(site, *slot, { name.uid, access, walk.changeableObjects.span() });
}

JSValue PropertyCaches::findGetterAndFillSiteCache(SiteCache site, JSCell* base, uint32_t identifierNumber)
{
    VM& vm = *m_state.instance.vm;
    DeferGCForAWhile deferGC(vm);
    Structure* structure = base->structure();
    CachedName name = nameOf(identifierNumber);
    auto find = [&]() -> JSValue {
        Object* object = compiledObjectOrNull(base);
        if (!object || !Object::structureGuaranteesAbsence(structure, name.uid) || hasPendingStars(*object))
            return { };
        JSGlobalObject* globalObject = m_state.instance.globalObject;
        ChainWalk walk = walkChain({ globalObject, structure, object, ChainWalkMode::AllocatesNothing }, name.uid);
        if (walk.result != ChainWalkResult::Found || !isValidOffset(walk.offset) || !(walk.attributes & PropertyAttribute::Accessor))
            return { };
        auto* accessor = uncheckedDowncast<GetterSetter>(valueAtOffset(walk.holder, walk.offset).asCell());
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        slot.setCacheableGetterSlot(walk.holder, walk.attributes, accessor, walk.offset);
        PropertyAnswer answer = classifyRead({ globalObject, object, structure, name.uid, slot, ChainWalkMode::AllocatesNothing });
        if (answer.kind != PropertyAnswerKind::Accessor || !canCache(structure, answer))
            return { };
        m_siteCaches.didMissRepeatedly(site);
        cache({ structure, name, SlotAccess::Read, site }, answer);
        return accessor->getter();
    };
    if (JSValue getter = find())
        return getter;
    m_siteCaches.didFindNothingToFill(site);
    return jsUndefined();
}

// A read for whoever may not run code or change the object: nothing is made, and what needs that is left for a read by name.
void PropertyCaches::fileValueWithoutChangingObject(JSCell* base, uint32_t identifierNumber)
{
    VM& vm = *m_state.instance.vm;
    Structure* structure = base->structure();
    CachedName name = nameOf(identifierNumber);
    JSGlobalObject* globalObject = m_state.instance.globalObject;
    PropertySlot slot(base, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (Object* object = compiledObjectOrNull(base); object ? !Object::structureGuaranteesAbsence(structure, name.uid) || hasPendingStars(*object) : base->type() != FinalObjectType || structure->isDictionary())
        return;
    unsigned attributes = 0;
    PropertyOffset ownOffset = isCompiledObject(base) ? invalidOffset : structure->getConcurrently(name.uid, attributes);
    if (isValidOffset(ownOffset)) {
        if (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
            return;
        slot.setValue(asObject(base), attributes, jsUndefined(), ownOffset);
    } else {
        ChainWalk walk = walkChain({ globalObject, structure, asObject(base), ChainWalkMode::AllocatesNothing }, name.uid);
        if (walk.result == ChainWalkResult::Found) {
            if (walk.attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
                return;
            if (isValidOffset(walk.offset))
                slot.setValue(walk.holder, walk.attributes, jsUndefined(), walk.offset);
            else
                slot.setValue(walk.holder, walk.attributes, jsUndefined());
        } else if (walk.result != ChainWalkResult::Absent)
            return;
    }
    PropertyAnswer answer = classifyRead({ globalObject, base, structure, name.uid, slot, ChainWalkMode::AllocatesNothing });
    if (answer.kind == PropertyAnswerKind::Value || answer.kind == PropertyAnswerKind::InheritedField || answer.kind == PropertyAnswerKind::Absent)
        cache({ structure, name, SlotAccess::Read, nullptr }, answer);
}

void PropertyCaches::cacheFreeName(SiteCache site, UniquedStringImpl* uid, SlotAccess access, const PropertyAnswer& answer)
{
    if (answer.didFlatten)
        return;
    ASSERT(site);
    ASSERT(answer.access == access);
    Slot slot = slotFor(answer, nullptr);
    if (answer.kind == PropertyAnswerKind::Constant && !answer.dependsOnChain()) {
        m_siteCaches.fill(site, slot, { uid, access, { } });
        return;
    }
    slot.flags |= Slot::dependsOnChain;
    ChainObjects objects;
    objects.append(m_state.instance.globalObject);
    objects.appendVector(answer.changeableObjects);
    ensureChangesAreReported(objects.span());
    m_siteCaches.fill(site, slot, { uid, access, objects.span() });
}

namespace {

enum class StructureLookup : uint8_t { Unknown, Absent, Own, Inherited };

struct StructureLookupResult {
    StructureLookup outcome { StructureLookup::Unknown };
    JSObject* holder { nullptr };
    PropertyOffset offset { invalidOffset };
    unsigned attributes { 0 };
};

}

static StructureLookupResult lookUpByStructure(Structure* structure, UniquedStringImpl* uid)
{
    for (JSObject* holder = nullptr;; structure = holder->structure()) {
        TypeInfo info = structure->typeInfo();
        if (!info.isObject() || !structure->hasMonoProto() || info.overridesGetOwnPropertySlot() || info.overridesGetPrototype()
            || TypeInfo::hasStaticPropertyTable(info.inlineTypeFlags()) || structure->isCompiledStructure() || structure->isUncacheableDictionary())
            return { };
        unsigned attributes = 0;
        PropertyOffset offset = structure->getConcurrently(uid, attributes);
        if (isValidOffset(offset))
            return { holder ? StructureLookup::Inherited : StructureLookup::Own, holder, offset, attributes };
        holder = structure->storedPrototypeObject();
        if (!holder)
            return { StructureLookup::Absent };
    }
}

static bool slotInStorageAgrees(const Slot& slot, SlotAccess access, const StructureLookupResult& found)
{
    bool isOwn = slot.has(Slot::isOwn);
    if (found.outcome != (isOwn ? StructureLookup::Own : StructureLookup::Inherited) || (!isOwn && found.holder != slot.holder) || found.offset != slot.offsetInStorage())
        return false;
    if (slot.kind == Slot::Kind::Getter || slot.kind == Slot::Kind::Setter)
        return found.attributes & PropertyAttribute::Accessor;
    if (access == SlotAccess::Define)
        return !found.attributes;
    return !(found.attributes & (access == SlotAccess::Read ? PropertyAttribute::AccessorOrCustomAccessorOrValue : PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue));
}

// A Structure that has dropped its chain takes the first like chain that comes to it for its own: other objects built that one anew, name by name, while an object or a cache kept the end of the old one.
// It no longer names the Structure of the slot then. The store replays the addition by the offset and the StructureID alone, so all that these two rely on is compared.
static bool hasPropertiesOfAndOneMore(Structure* structure, Structure* newStructure, UniquedStringImpl* uid)
{
    if (structure->isDictionary() || newStructure->isDictionary() || newStructure->typeInfoBlob() != structure->typeInfoBlob() || newStructure->inlineCapacity() != structure->inlineCapacity())
        return false;
    if (newStructure->classInfoForCells() != structure->classInfoForCells() || newStructure->realm() != structure->realm() || !structure->hasMonoProto() || !newStructure->hasMonoProto() || newStructure->storedPrototype() != structure->storedPrototype())
        return false;
    if (newStructure->mayBePrototype() != structure->mayBePrototype() || newStructure->isStructureExtensible() != structure->isStructureExtensible() || newStructure->mayHaveImmutableProperties() != structure->mayHaveImmutableProperties())
        return false;
    // Each list is taken under the lock of the Structure that has the table, which may be one and the same for both.
    Vector<PropertyTableEntry> properties = structure->getPropertiesConcurrently();
    Vector<PropertyTableEntry> newProperties = newStructure->getPropertiesConcurrently();
    if (newProperties.size() != properties.size() + 1)
        return false;
    UncheckedKeyHashMap<UniquedStringImpl*, std::pair<PropertyOffset, unsigned>> placesInNewStructure;
    for (const PropertyTableEntry& entry : newProperties)
        placesInNewStructure.add(entry.key(), std::pair { entry.offset(), entry.attributes() });
    for (const PropertyTableEntry& entry : properties) {
        auto place = placesInNewStructure.find(entry.key());
        if (place == placesInNewStructure.end() || place->value != std::pair<PropertyOffset, unsigned> { entry.offset(), entry.attributes() })
            return false;
    }
    auto added = placesInNewStructure.find(uid);
    return added != placesInNewStructure.end() && !added->value.second;
}

static bool transitionSlotAgrees(const Slot& slot, SlotAccess access, const StructureLookupResult& found, UniquedStringImpl* uid)
{
    if (found.outcome == StructureLookup::Own || (access == SlotAccess::Store && found.outcome == StructureLookup::Inherited && found.attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue))
        return false;
    Structure* structure = slot.structureID.decode();
    Structure* newStructure = slot.transition.newStructureID.decode();
    if (newStructure->getConcurrently(uid) != slot.offsetInStorage())
        return false;
    return newStructure->isTransitionFrom(structure) || isLastAdditionOfRun(structure, newStructure, uid) || hasPropertiesOfAndOneMore(structure, newStructure, uid);
}

bool slotAgreesWithStructures(VM&, const Slot& slot, SlotAccess access, UniquedStringImpl* uid)
{
    if (!slot.structureID)
        return true;
    StructureLookupResult found = lookUpByStructure(slot.structureID.decode(), uid);
    if (found.outcome == StructureLookup::Unknown)
        return true;
    switch (slot.kind) {
    case Slot::Kind::Value:
    case Slot::Kind::FarValue:
    case Slot::Kind::Getter:
    case Slot::Kind::Setter:
        return slotInStorageAgrees(slot, access, found);
    case Slot::Kind::Absent:
        return found.outcome == StructureLookup::Absent;
    case Slot::Kind::Transition:
        return transitionSlotAgrees(slot, access, found, uid);
    case Slot::Kind::Removal:
        return found.outcome == StructureLookup::Own && found.offset == slot.offsetInStorage() && !(found.attributes & PropertyAttribute::DontDelete) && !isValidOffset(slot.removal.newStructureID.decode()->getConcurrently(uid));
    case Slot::Kind::NothingToRemove:
        return found.outcome != StructureLookup::Own;
    case Slot::Kind::Empty:
    case Slot::Kind::CustomGetter:
    case Slot::Kind::CustomSetter:
    case Slot::Kind::Field:
    case Slot::Kind::FieldGetter:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::FieldOrInherited:
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::InheritedField:
    case Slot::Kind::FieldOrInheritedField:
    case Slot::Kind::Variable:
    case Slot::Kind::ExtraProperty:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ExtraPropertyAddition:
    case Slot::Kind::ExtraPropertyRemoval:
    case Slot::Kind::FieldRemoval:
    case Slot::Kind::ArrayLength:
    case Slot::Kind::TypedArrayLength:
    case Slot::Kind::StringLength:
    case Slot::Kind::ClosureLength:
    case Slot::Kind::ClosureName:
    case Slot::Kind::Constant:
    case Slot::Kind::Uncacheable:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The stubs read a row in the numbers of their own image: orders of keys, types, extra properties. Under the Structure of a compiled object of another image
// a row says only what the slot of a field says by itself, where an extra property lies, or what stands above the object.
static bool mayStandUnderStructureOfOtherImage(const Slot& slot, SlotAccess access)
{
    switch (slot.kind) {
    case Slot::Kind::Field:
        return access == SlotAccess::Read || (access == SlotAccess::Store && (slot.field.type & Slot::typeIsGivenByBits));
    case Slot::Kind::FieldOrInherited:
        return access == SlotAccess::Read && !slot.has(Slot::inheritsGetter);
    case Slot::Kind::ExtraProperty:
        return access == SlotAccess::Read || access == SlotAccess::Store;
    case Slot::Kind::FieldGetter:
    case Slot::Kind::FieldCreation:
    case Slot::Kind::FieldRemoval:
    case Slot::Kind::FieldOrInheritedSetter:
    case Slot::Kind::FieldOrInheritedField:
    case Slot::Kind::ExtraGetter:
    case Slot::Kind::ExtraSetter:
    case Slot::Kind::ExtraPropertyAddition:
    case Slot::Kind::ExtraPropertyRemoval:
        return false;
    default:
        return true;
    }
}

bool VMPropertyCaches::describesObjectOfOtherImage(Structure* structure) const
{
    if (!structure->isCompiledStructure())
        return false;
    auto& compiled = *uncheckedDowncast<CompiledStructure>(structure);
    if (!compiled.describesObject())
        return false;
    std::span<const uint8_t> bytes = m_image->bytes();
    return std::bit_cast<uintptr_t>(&compiled.layout()) - std::bit_cast<uintptr_t>(bytes.data()) >= bytes.size();
}

void VMPropertyCaches::validateImageOfSlot(VM& vm, const Slot& slot, SlotAccess access, UniquedStringImpl* uid) const
{
    Structure* structure = slot.structureID.decode();
    if (!describesObjectOfOtherImage(structure) || mayStandUnderStructureOfOtherImage(slot, access))
        return;
    dataLogLn("AOT validation: a row of the property caches of one image is filed under the Structure of a compiled object of another in the numbers of its own: name ", uid ? String(uid) : "(none)"_str, ", kind of row ", static_cast<unsigned>(slot.kind), ", access ", static_cast<unsigned>(access));
    crashOnValidationFailure(vm, { ValidationFailureKind::CacheHitDiffers, { }, { }, structure });
}

static void validateSlotLiveness(VM& vm, const CustomAccessorTable& customAccessors, const Slot& slot)
{
    if (!customAccessors.namesOnlyWhatIsLive(vm, slot))
        crashOnValidationFailure(vm, { ValidationFailureKind::CacheEntryDropped, { }, { }, JSValue() });
}

static bool requiresFinishedWalk(Slot::Kind kind)
{
    return kind == Slot::Kind::Value || kind == Slot::Kind::Absent || kind == Slot::Kind::Getter || kind == Slot::Kind::Setter || kind == Slot::Kind::Transition || kind == Slot::Kind::TypedArrayLength;
}

static JSObject* findUnregisteredChainObject(const ChainDependents& dependents, Structure* receiverStructure, const StructureLookupResult& found, UniquedStringImpl* uid)
{
    if (found.outcome != StructureLookup::Absent && found.outcome != StructureLookup::Inherited)
        return nullptr;
    for (JSObject* object = receiverStructure->storedPrototypeObject(); object; object = object->structure()->storedPrototypeObject()) {
        Structure* structure = object->structure();
        bool isHolder = object == found.holder;
        bool canGainProperty = structure->isStructureExtensible() && !structure->isImmutablePropertyForReader(uid);
        bool canChange = isHolder ? !structure->isImmutablePropertyForReader(uid) : canGainProperty || !structure->hasImmutablePrototype();
        if (canChange && !dependents.hasDependents(object))
            return object;
        if (isHolder)
            break;
    }
    return nullptr;
}

bool VMPropertyCaches::isBelowUnreportedChange(Structure* structure) const
{
    if (m_objectsWithUnreportedChange.isEmpty()) [[likely]]
        return false;
    while (structure->isObject() && structure->hasMonoProto()) {
        JSObject* prototype = structure->storedPrototypeObject();
        if (!prototype)
            return false;
        if (m_objectsWithUnreportedChange.contains(prototype))
            return true;
        structure = prototype->structure();
    }
    return false;
}

static void validateSlotAnswer(VM& vm, const Slot& slot, SlotAccess access, UniquedStringImpl* uid, const ChainDependents* dependents)
{
    Structure* structure = slot.structureID.decode();
    if (!structure->isObject())
        return;
    vm.aotState().propertyCaches->validateImageOfSlot(vm, slot, access, uid);
    if (slot.has(Slot::dependsOnChain) && vm.aotState().propertyCaches->isBelowUnreportedChange(structure))
        return;
    if (!slotAgreesWithStructures(vm, slot, access, uid)) {
        dataLogLn("AOT validation: a row of the property caches does not agree with the Structure it is filed under: name ", uid ? String(uid) : "(none)"_str, ", kind of row ", static_cast<unsigned>(slot.kind), ", access ", static_cast<unsigned>(access), ", kind of dictionary ", static_cast<unsigned>(structure->dictionaryKind()), ", flattened before ", structure->hasBeenFlattenedBefore(), ", may be a prototype ", structure->mayBePrototype(), ", properties up to offset ", structure->maxOffset());
        if (slot.kind == Slot::Kind::Transition) {
            Structure* newStructure = slot.transition.newStructureID.decode();
            Structure* named = newStructure->previousID();
            PropertyOffset offsetByTable = invalidOffset;
            Structure* reached = structure->isDictionary() ? nullptr : Structure::addPropertyTransitionToExistingStructure(structure, uid, 0, offsetByTable);
            dataLogLn("AOT validation: the addition: offset in the row ", slot.offsetInStorage(), ", in the new Structure ", newStructure->getConcurrently(uid), ", in the old one ", structure->getConcurrently(uid), "; the new Structure names ", named == structure ? "the old one" : named ? "another" : "none", " as its predecessor, kind of its transition ", static_cast<unsigned>(newStructure->transitionKind()), ", by this name ", newStructure->transitionPropertyName() == uid, ", table pinned ", newStructure->isPinnedPropertyTable(), ", kind of dictionary ", static_cast<unsigned>(newStructure->dictionaryKind()), ", properties up to offset ", newStructure->maxOffset(), "; the old one's table pinned ", structure->isPinnedPropertyTable(), ", kind of its transition ", static_cast<unsigned>(structure->transitionKind()), ", it leads by this name to ", reached == newStructure ? "the new one" : reached ? "another" : "none");
        }
        crashOnValidationFailure(vm, { ValidationFailureKind::CacheHitDiffers, { }, { }, structure });
    }
    if (!dependents || !slot.has(Slot::dependsOnChain) || !requiresFinishedWalk(slot.kind))
        return;
    if (JSObject* object = findUnregisteredChainObject(*dependents, structure, lookUpByStructure(structure, uid), uid))
        crashOnValidationFailure(vm, { ValidationFailureKind::CacheEntryDropped, { }, { }, object });
}

void VMPropertyCaches::validate(VM& vm) const
{
    auto validateSlot = [&](const Slot& slot, SlotAccess access, UniquedStringImpl* uidOrNull) {
        validateSlotLiveness(vm, m_customAccessors, slot);
        if (!uidOrNull)
            return;
        JSGlobalObject* realm = slot.structureID.decode()->realm();
        Instance* instance = realm ? Instance::of(realm) : nullptr;
        validateSlotAnswer(vm, slot, access, uidOrNull, instance && instance->state ? instance->state->chainDependents.get() : nullptr);
    };
    const ProgramIdentifiers* identifiers = vm.aotState().identifiers.get();
    m_front.forEachSlot([&](const Slot& slot, uint32_t column, SlotAccess access) {
        validateSlot(slot, access, identifiers && column < identifiers->count() ? identifiers->tryGetIdentifierConcurrently(column) : nullptr);
    });
    m_hashedTable.forEachEntry([&](const HashedSlot& entry) {
        validateSlot(entry.slot, entry.access(), entry.uid());
    });
}

static bool isFirstInstanceOfItsImage(VM& vm, const Instance& instance)
{
    for (Instance* other : vm.aotState().instances) {
        if (other->program == instance.program)
            return other == &instance;
    }
    return false;
}

void PropertyCaches::didFinishCollection(VM& vm, CollectionScope)
{
    {
        Locker locker { m_state.caches->lock() };
        m_additionsSinceCollection.shrink(0);
    }
    m_shouldForgetMisses.store(true, std::memory_order_relaxed);
    if (!validatesRuntime(vm))
        return;
    // Whichever image ran when the collection began, what the checks ask answers for the image that runs: Instance::of(), the caches of the VM.
    RunningImageScope runsImageOfCaches(vm.aotState(), m_state.instance.program);
    if (isFirstInstanceOfItsImage(vm, m_state.instance))
        m_tables.validate(vm);
    m_siteCaches.forEachValidatedSite([&](const Slot& slot, UniquedStringImpl* uid, SlotAccess access) {
        validateSlotLiveness(vm, m_tables.customAccessors(), slot);
        if (slot.structureID)
            validateSlotAnswer(vm, slot, access, uid, m_state.chainDependents.get());
    });
}

static void countReason(Instance& instance, uint32_t reason)
{
    ASSERT(countsStubExit(static_cast<RuntimeCounterID>(reason)));
    count(instance, static_cast<RuntimeCounterID>(reason));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCallGetterOfSlot, EncodedJSValue, (Instance* instance, EncodedJSValue base, JSCell* getterSetter, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTCallGetterOfSlot;
    AOT_OPERATION_BEGIN(instance, entry);
    countReason(*instance, reason);
    OPERATION_RETURN(scope, JSValue::encode(cellArgument<GetterSetter>(getterSetter, entry)->callGetter(globalObject, JSValue::decode(base))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCallSetterOfSlot, void, (Instance* instance, EncodedJSValue base, JSCell* getterSetter, EncodedJSValue value, uint32_t putMode, uint32_t reason))
{
    constexpr Entry entry = Entry::operationAOTCallSetterOfSlot;
    AOT_OPERATION_BEGIN(instance, entry);
    countReason(*instance, reason);
    cellArgument<GetterSetter>(getterSetter, entry)->callSetter(globalObject, JSValue::decode(base), JSValue::decode(value), static_cast<PutMode>(putMode) == PutMode::Strict);
    OPERATION_RETURN(scope);
}

static void addPropertyByTransition(VM& vm, JSObject* target, Structure* newStructure, PropertyOffset offset, JSValue value)
{
    ASSERT(newStructure->isValidOffset(offset) && newStructure->inlineCapacity() == target->structure()->inlineCapacity() && newStructure->storedPrototype() == target->structure()->storedPrototype());
    bool hasRoomInOwnCell = !target->structure()->outOfLineCapacity() && newStructure->outOfLineCapacity() == initialOutOfLineCapacity
        && std::bit_cast<char*>(target->butterfly()) == std::bit_cast<char*>(target) + sizeOfArrayWithStorage(0, propertyRoomOfArray);
    if (!hasRoomInOwnCell)
        target->growOutOfLineStorageForTransition(vm, newStructure);
    target->putDirectOffset(vm, offset, value);
    target->setStructure(vm, newStructure);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAddPropertyBySlot, void, (Instance* instance, JSCell* object, uint64_t header, uint64_t payload, EncodedJSValue value, uint32_t reason))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAddPropertyBySlot);
    countReason(*instance, reason);
    Slot slot = std::bit_cast<Slot>(std::array<uint64_t, 2> { header, payload });
    RELEASE_ASSERT(object->structureID() == slot.structureID);
    if (slot.kind == Slot::Kind::Transition) {
        addPropertyByTransition(vm, asObject(object), WTF::opaque(slot.transition.newStructureID.decode()), slot.offsetInStorage(), JSValue::decode(value));
        return;
    }
    RELEASE_ASSERT(slot.kind == Slot::Kind::ExtraPropertyAddition);
    Object* receiver = uncheckedDowncast<Object>(object);
    // The slot is a copy out of a weak cache, and no object need have this Structure yet: this pointer alone keeps it alive while the extra properties are allocated.
    // The collector knows no StructureID on the stack, so the compiler must not make the pointer anew from the ID behind the call.
    Structure* newStructure = WTF::opaque(slot.extraPropertyAddition.newStructureID.decode());
    JSObject* extraProperties = receiver->ensureExtraProperties(vm);
    RELEASE_ASSERT(extraProperties);
    addPropertyByTransition(vm, extraProperties, compiledObjectStructureOrNull(newStructure)->extraPropertiesStructure(), Slot::offsetFor(slot.extraPropertyAddition.wordIndex), JSValue::decode(value));
    receiver->JSCell::setStructure(vm, newStructure);
}

WatchedValues::WatchedValues(State& state)
    : m_state(state)
{
    state.caches->add(CacheKind::WeakByKey, "WatchedValues"_s, *this);
}

bool WatchedValues::tryWatchConstant(VM& vm, JSObject* holder, UniquedStringImpl* uid, JSValue expected, SiteCache site)
{
    // A free-name site with a cached store compares no Structure and fires no watchpoint, and each later Structure of the holder starts with a valid replacement set.
    if (!site || m_namesWithCachedStore.contains(uid) || !holder->structure()->propertyAccessesAreCacheable())
        return false;
    SiteCaches& sites = m_state.propertyCaches->siteCaches();
    auto isExpected = [&](JSValue held) { return held == expected; };
    auto watch = PropertyValueWatchpoint::tryWatch(vm, holder, uid, isExpected, [](PropertyValueWatchpoint& fired) {
        auto& owner = fired.owner<WatchedValues>();
        owner.m_writtenSinceCollection.recordAll();
        SiteCaches& sites = owner.m_state.propertyCaches->siteCaches();
        sites.clear(sites.at(fired.number()));
    }, this, sites.indexOf(site));
    if (!watch)
        return false;

    // Clearing a site that holds a constant stops the watch of that site, which must not be the watch created here.
    if (site->get().kind == Slot::Kind::Constant)
        sites.clear(site);
    m_constantsBySite.set(watch->number(), WTF::move(watch));
    m_writtenSinceCollection.recordAll();
    return true;
}

void WatchedValues::stopWatchingConstant(SiteCache site)
{
    auto iterator = m_constantsBySite.find(m_state.propertyCaches->siteCaches().indexOf(site));
    if (iterator != m_constantsBySite.end() && !iterator->value->hasFired())
        m_constantsBySite.remove(iterator);
}

size_t WatchedValues::removeDeadEntries(VM& vm, CollectionScope scope)
{
    size_t numberOfRemovedWatches = 0;
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        m_constantsBySite.removeIf([&](auto& entry) {
            bool isDead = entry.value->hasFired() || !vm.heap.isMarked(entry.value->key().object());
            numberOfRemovedWatches += isDead;
            return isDead;
        });
    });
    return numberOfRemovedWatches;
}

static_assert(!((1u << SiteCaches::regionShift) % sizeof(Slot)));
static_assert(std::is_same_v<decltype(SiteCacheState::missesLeft), uint16_t>, "the stub's countSiteCacheMiss wraps from 0 to 65535");

static std::span<uint8_t> allocateCards(State& state, size_t sizeOfSlots)
{
    size_t numberOfCards = RememberedSlotSet::numberOfCardsFor(sizeOfSlots, SiteCaches::regionShift);
    return { state.tableArena.allocate<uint8_t>(numberOfCards), numberOfCards };
}

SiteCaches::SiteCaches(State& state)
    : m_state(state)
    , m_customAccessors(tablesOfImageOf(state).customAccessors())
    , m_slots(state.instance.siteCaches())
    , m_states(state.tableArena.allocate<SiteCacheState>(SiteCacheState::numberOfStates))
    , m_refillCounts(state.tableArena.allocate<uint8_t>(SiteCacheState::numberOfStates))
    , m_sitesThatMissed(state.tableArena.allocate<uint64_t>(numberOfMissWords(m_slots.size())))
    , m_ownerID(state.chainDependents->addOwner(*this))
    , m_writtenRegions(asByteSpan(m_slots), regionShift, allocateCards(state, m_slots.size_bytes()))
{
    state.instance.propertyCaches.siteCacheStates = m_states;
    state.caches->add(CacheKind::WeakByKey, "SiteCaches"_s, *this);
    state.caches->add(CacheKind::StrongRoots, "Structures that sites name"_s, *this);
}

bool SiteCaches::didMissBefore(SiteCache site)
{
    uint32_t missBit = indexOf(site) % maxNumberOfMissBits;
    uint64_t& word = m_sitesThatMissed[missBit / 64];
    uint64_t bit = 1ull << (missBit % 64);
    return std::exchange(word, word | bit) & bit;
}

void SiteCaches::waitLongerForNextFill(SiteCache site, unsigned mostRefillsThatCount)
{
    uint8_t& refillCount = refillCountOf(site);
    stateOf(site).missesLeft = SiteCacheState::missesBeforeRefill(refillCount, mostRefillsThatCount);
    if (refillCount < SiteCacheState::maxRefillCount)
        ++refillCount;
}

void SiteCaches::didFailToFill(SiteCache site)
{
    // The misses of all empty sites are counted together, and one in a few asks. With a slot that fits no object the site counts its own, and asks more and more seldom.
    if (site->get().isEmpty())
        site->set(m_writtenRegions, Slot::createWithoutPayload(StructureID(), Slot::Kind::Uncacheable, 0));
    waitLongerForNextFill(site);
}

void SiteCaches::didFailToFillWithRun(SiteCache site)
{
    if (site->get().isEmpty())
        stateOf(site).missesLeft = SiteCacheState::missesBeforeRefill(0);
    else
        waitLongerForNextFill(site, SiteCacheState::maxRefillCountOfRun);
}

// Whoever asks on behalf of an empty site looks the name up each time, so it asks more and more seldom.
void SiteCaches::didFindNothingToFill(SiteCache site)
{
    waitLongerForNextFill(site);
}

bool SiteCaches::tryWrite(SiteCache site, const Slot& slot, unsigned mostRefillsThatCount)
{
    if (!didMissBefore(site)) {
        stateOf(site).missesLeft = 0;
        return false;
    }
    if (site->get().kind == Slot::Kind::Constant)
        m_state.propertyCaches->watchedValues().stopWatchingConstant(site);
    // A slot without a Structure fits no object: didFailToFill() put it there.
    if (!site->get().isEmpty() && site->get().structureID)
        count(m_state.instance, RuntimeCounterID::SiteCacheRefills);
    site->set(m_writtenRegions, slot);
    m_wasWrittenSinceLastSweep = true;
    waitLongerForNextFill(site, mostRefillsThatCount);
    return true;
}

void SiteCaches::fill(SiteCache site, const Slot& slot)
{
    RELEASE_ASSERT(!slot.has(Slot::dependsOnChain));
    tryWrite(site, slot);
}

void SiteCaches::fill(SiteCache site, const Slot& slot, const NamedFill& named)
{
    if (!tryWrite(site, slot))
        return;
    if (slot.has(Slot::dependsOnChain))
        m_state.chainDependents->add(named.changeableObjects, { named.uid, indexOf(site), m_ownerID });
    if (validatesRuntime(*m_state.instance.vm)) [[unlikely]]
        m_validatedSites.set(indexOf(site), std::pair { named.uid, named.access });
}

void SiteCaches::fill(SiteCache site, const Slot& slot, std::span<const ChainDependence> dependences)
{
    if (!tryWrite(site, slot, SiteCacheState::maxRefillCountOfRun))
        return;
    for (const ChainDependence& dependence : dependences) {
        RELEASE_ASSERT(slot.has(Slot::dependsOnChain) || dependence.changeableObjects.empty());
        m_state.chainDependents->add(dependence.changeableObjects, { dependence.uid, indexOf(site), m_ownerID });
    }
}

void SiteCaches::clear(SiteCache site)
{
    if (site->get().kind == Slot::Kind::Constant)
        m_state.propertyCaches->watchedValues().stopWatchingConstant(site);
    site->clear();
    // What it held holds no more: that says nothing about how many Structures it meets.
    refillCountOf(site) = 0;
    stateOf(site).missesLeft = 0;
}

void SiteCaches::dropChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl*)
{
    if (m_slots[siteIndex].get().has(Slot::dependsOnChain))
        clear(at(siteIndex));
}

bool SiteCaches::keepChainDependent(VM& vm, uint32_t siteIndex, UniquedStringImpl* uid, const CreatedMembers& created)
{
    ASSERT(vm.heap.isDeferred());
    if (!uid)
        return false;
    std::optional<CreatedMember> member = CreatedMember::find(vm, created, uid);
    if (!member)
        return true;
    Slot slot = m_slots[siteIndex].get();
    if (!slot.has(Slot::dependsOnChain))
        return false;
    std::optional<Slot> kept = slotBehindCreatedMember(slot, *member, created);
    if (!kept)
        return false;
    if (!isSameSlot(*kept, slot)) {
        m_slots[siteIndex].set(m_writtenRegions, *kept);
        m_wasWrittenSinceLastSweep = true;
    }
    return true;
}

bool SiteCaches::hasChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl*) const
{
    return m_slots[siteIndex].get().has(Slot::dependsOnChain);
}

static void forEachStructureNamedBy(const Slot& slot, const Invocable<void(StructureID)> auto& functor)
{
    if (slot.structureID)
        functor(slot.structureID);
    switch (Slot::payloadOf(slot.kind)) {
    case SlotPayload::ExtraProperty:
        if (slot.extraProperty.structureID)
            functor(slot.extraProperty.structureID);
        return;
    case SlotPayload::ExtraPropertyAddition:
        return functor(slot.extraPropertyAddition.newStructureID);
    case SlotPayload::Transition:
        return functor(slot.transition.newStructureID);
    case SlotPayload::Removal:
        return functor(slot.removal.newStructureID);
    case SlotPayload::None:
    case SlotPayload::WordIndex:
    case SlotPayload::Field:
    case SlotPayload::OrderWords:
    case SlotPayload::CustomAccessor:
    case SlotPayload::Holder:
    case SlotPayload::FieldAndHolder:
    case SlotPayload::Constant:
    case SlotPayload::Key:
        return;
    }
}

template<typename Visitor>
void SiteCaches::visitRoots(Visitor& visitor)
{
    if (visitor.collectionScope() == CollectionScope::Eden)
        return;

    constexpr bool looksOnceAtEachSite = std::is_same_v<Visitor, SlotVisitor>;
    if constexpr (looksOnceAtEachSite) {
        if (std::exchange(m_versionOfStructuresThatWait, visitor.markingVersion()) != visitor.markingVersion())
            m_structuresThatWait.clear();
    }
    auto isSettled = [&](StructureID id) {
        Structure* structure = id.decode();
        if (visitor.isMarked(structure))
            return true;
        auto* compiled = dynamicDowncast<CompiledStructure>(structure);
        JSObject* prototype = structure->hasMonoProto() ? structure->storedPrototype().getObject() : nullptr;
        JSGlobalObject* realm = structure->globalObject();
        if ((compiled && compiled->heldExtraProperties()) || (prototype && !visitor.isMarked(prototype)) || (realm && !visitor.isMarked(realm)))
            return false;
        visitor.appendUnbarriered(structure);
        return true;
    };
    m_structuresThatWait.removeIf(isSettled);
    m_writtenRegions.forEachRegionToVisit(visitor, [&](size_t beginOffset, size_t endOffset) {
        RememberedSlotSet::forEachEntryInRegion(m_slots, beginOffset, endOffset, [&](RememberedSlot<Slot>& site) {
            if (site.get().isEmpty())
                return;
            forEachStructureNamedBy(site.get(), [&](StructureID id) {
                if (!isSettled(id) && looksOnceAtEachSite)
                    m_structuresThatWait.add(id);
            });
        });
    });
}

template void SiteCaches::visitRoots(AbstractSlotVisitor&);
template void SiteCaches::visitRoots(SlotVisitor&);

size_t SiteCaches::removeDeadEntries(VM& vm, CollectionScope scope)
{
    if (scope == CollectionScope::Eden && !m_wasWrittenSinceLastSweep)
        return 0;
    m_wasWrittenSinceLastSweep = false;

    auto isMarked = [&](JSCell* cell) {
        return vm.heap.isMarked(cell);
    };
    uintptr_t cageBase = vm.heap.cage().base();
    bool isFullCollection = scope == CollectionScope::Full && vm.heap.collectionScope() != CollectionScope::Eden;
    size_t numberOfClearedSites = 0;
    m_writtenRegions.forEachRegionToSweep(scope, [&](size_t beginOffset, size_t endOffset) {
        RememberedSlotSet::forEachEntryInRegion(m_slots, beginOffset, endOffset, [&](RememberedSlot<Slot>& site) {
            const Slot& slot = site.get();
            if (slot.isEmpty() || (slot.namesOnlyLiveCells(isMarked, cageBase) && (Slot::payloadOf(slot.kind) != SlotPayload::CustomAccessor || m_customAccessors.isInUse(slot.customAccessor.index))))
                return;
            // Not SiteCaches::clear(): a sweep unlinks no watchpoint. WatchedValues::removeDeadEntries drops the watch of a Constant slot.
            site.clear();
            ++numberOfClearedSites;
        });
    });
    if (isFullCollection)
        decommitEmptyPages(asWritableBytes(m_slots), m_writtenRegions);
    return numberOfClearedSites;
}

StringKeyCache::StringKeyCache(State& state)
    : m_instance(state.instance)
    , m_cageBase(state.instance.vm->heap.cage().base())
    , m_cageSize(state.instance.vm->heap.cage().start() - state.instance.vm->heap.cage().base() + state.instance.vm->heap.cage().size())
    , m_maxCachedKeyLength(runtimeLimitValue(*state.instance.vm, LimitID::MaxCachedKeyLength))
{
    state.caches->add(CacheKind::WeakByKey, "StringKeyCache"_s, *this);
    publishIntegerKeys();
    m_instance.realm.caseConversions = m_caseConversions.data();
}

void StringKeyCache::publishIntegerKeys()
{
    m_instance.realm.integerKeys = m_integerKeys.isEmpty() ? nullptr : m_integerKeys.span().data();
    m_instance.realm.integerKeyMask = m_integerKeys.isEmpty() ? 0 : m_integerKeys.size() - 1;
}

JSString* StringKeyCache::findOrAdd(VM& vm, std::span<const Latin1Character> characters)
{
    if (characters.size() < 2)
        return characters.empty() ? jsEmptyString(vm) : jsSingleCharacterString(vm, characters[0]);
    if (characters.size() > m_maxCachedKeyLength) {
        countLimitHit(vm, LimitID::MaxCachedKeyLength);
        return jsNontrivialString(vm, String(characters));
    }
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    if (JSString* key = tryFindCachedKey(characters, hash))
        return key;
    return makeKey(vm, characters, hash);
}

NEVER_INLINE JSString* StringKeyCache::findAmongEqualHashes(const uint64_t* pair, std::span<const Latin1Character> characters) const
{
    if (JSString* key = matchingKey(pair[0], characters))
        return key;
    return matchingKey(pair[1], characters);
}

JSString* StringKeyCache::caseConvertedString(VM& vm, std::span<const Latin1Character> characters)
{
    ASSERT(characters.size() >= 2);
    unsigned hash = StringHasher::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    uint64_t& entry = m_caseConvertedStrings[hash % m_caseConvertedStrings.size()];
    if (entry && entry >> 32 == hash) {
        if (JSString* key = matchingKey(entry, characters))
            return key;
    }
    JSString* newString = jsNontrivialString(vm, String(characters));
    if (vm.heap.cage().contains(newString)) {
        entry = static_cast<uint64_t>(hash) << 32 | MarkedBlockCage::encode(m_cageBase, newString);
        m_writtenSinceCollection.recordAll();
    }
    return newString;
}

void StringKeyCache::cacheCaseConversion(VM& vm, const JSString* source, LetterCase letterCase, JSString* converted)
{
    if (!vm.heap.cage().contains(source) || !vm.heap.cage().contains(converted))
        return;
    uint32_t encoded = MarkedBlockCage::encode(m_cageBase, source);
    size_t way = 0;
    while (way < waysOfCaseConversions - 1 && m_caseConversions[indexOfCaseConversion(encoded, letterCase, way)])
        ++way;
    for (; way; --way)
        m_caseConversions[indexOfCaseConversion(encoded, letterCase, way)] = m_caseConversions[indexOfCaseConversion(encoded, letterCase, way - 1)];
    m_caseConversions[indexOfCaseConversion(encoded, letterCase)] = static_cast<uint64_t>(encoded) << 32 | MarkedBlockCage::encode(m_cageBase, converted);
    m_hashesWithCaseConversions.set(indexOfCaseConversion(encoded, letterCase) >> caseConversionSetShift);
    m_writtenSinceCollection.recordAll();
}

JSString* StringKeyCache::makeKey(VM& vm, std::span<const Latin1Character> characters, unsigned hash)
{
    return makeKey(vm, characters, hash, { });
}

void StringKeyCache::growCachedKeys()
{
    m_cachedKeyEvictionCount = 0;
    std::optional former = tryResize(m_cachedKeys, m_cachedKeys.size() * 2);
    if (!former)
        return;
    for (uint64_t formerEntry : *former) {
        if (!formerEntry)
            continue;
        unsigned hash = formerEntry >> 32;
        size_t index = firstIndex(hash, m_cachedKeys.size());
        if (m_cachedKeys[index])
            index += m_cachedKeys[index + 1] ? hash >> 23 & 1 : 1;
        m_cachedKeys[index] = formerEntry;
    }
}

void StringKeyCache::cacheKeyByParts(const KeyByParts& parts, JSString* key)
{
    if (!canEncode(key))
        return;
    addKeyByParts(parts, key);
    m_writtenSinceCollection.recordAll();
}

void StringKeyCache::resizeIntegerKeys(size_t tableSize)
{
    m_integerKeyEvictionCount = 0;
    std::optional former = tryResize(m_integerKeys, tableSize);
    if (!former)
        return;
    for (uint64_t formerEntry : *former) {
        if (formerEntry)
            m_integerKeys[formerEntry >> 32 & (tableSize - 1)] = formerEntry;
    }
    publishIntegerKeys();
}

bool StringKeyCache::cacheIntegerKey(uint32_t integer, JSString* key)
{
    if (!canEncode(key))
        return false;
    if (m_integerKeys.isEmpty()) [[unlikely]] {
        if (!tryResize(m_integerKeys, minIntegerKeyTableSize))
            return false;
        publishIntegerKeys();
    }
    size_t tableSize = m_integerKeys.size();
    uint64_t& entry = m_integerKeys[integer & (tableSize - 1)];
    bool overwrites = entry && entry >> 32 != integer;
    entry = static_cast<uint64_t>(integer) << 32 | MarkedBlockCage::encode(m_cageBase, key);
    ++m_integerKeysEnteredSinceSweep;
    m_writtenSinceCollection.recordAll();
    if (overwrites && ++m_integerKeyEvictionCount > tableSize / 4 && tableSize < maxIntegerKeyTableSize) [[unlikely]]
        resizeIntegerKeys(tableSize * 2);
    return true;
}

JSString* StringKeyCache::makeIntegerKey(VM& vm, uint32_t integer, std::span<const Latin1Character> characters)
{
    ASSERT(characters.size() >= 2);
    unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
    JSString* key = jsNontrivialString(vm, String(StringImpl::create(characters, hash)));
    cacheIntegerKey(integer, key);
    return key;
}

const StringKeyCache::KeyOfPair* StringKeyCache::noKeysOfPairs()
{
    static constinit const std::array<KeyOfPair, waysOfKeysOfPairs> empty { };
    return empty.data();
}

void StringKeyCache::publishKeysOfPairs()
{
    m_instance.realm.keysOfPairs = m_keysOfPairs.span().data();
    m_instance.realm.keyOfPairMask = keyOfPairMaskFor(m_keysOfPairs.size());
}

// Whether it took the place of another.
bool StringKeyCache::addKeyOfPair(const KeyOfPair& added)
{
    KeyOfPair* set = m_keysOfPairs.mutableSpan().data() + (hashOfPair(added.first, added.second) & keyOfPairMaskFor(m_keysOfPairs.size())) / sizeof(KeyOfPair);
    bool evicts = set[0].key && set[1].key;
    // The one that came last stands in front.
    if (set[0].key)
        set[1] = set[0];
    set[0] = added;
    return evicts;
}

void StringKeyCache::cacheKeyOfPair(EncodedJSValue first, EncodedJSValue second, JSString* key)
{
    if (m_keysOfPairs.isEmpty()) [[unlikely]] {
        if (!tryResize(m_keysOfPairs, minTableSize))
            return;
        publishKeysOfPairs();
    }
    if (addKeyOfPair({ first, second, key, 0, 0 }) && ++m_keyOfPairEvictionCount > m_keysOfPairs.size() / 4) [[unlikely]]
        growKeysOfPairs();
    m_writtenSinceCollection.recordAll();
}

void StringKeyCache::forgetPlacesOfPairs()
{
    if (!std::exchange(m_instance.propertyCaches.pairsHavePlaces, 0))
        return;
    for (KeyOfPair& entry : m_keysOfPairs) {
        entry.structureIDOfPlace = 0;
        entry.slotOfPlace = 0;
    }
}

void StringKeyCache::growKeysOfPairs()
{
    m_keyOfPairEvictionCount = 0;
    if (m_keysOfPairs.size() >= maxKeysByPartsTableSize)
        return;
    std::optional former = tryResize(m_keysOfPairs, m_keysOfPairs.size() * 2);
    if (!former)
        return;
    publishKeysOfPairs();
    // From the back of each set, so that the one that came last stands in front again.
    for (const KeyOfPair& entry : *former | std::views::reverse) {
        if (entry.key)
            m_keyOfPairEvictionCount += addKeyOfPair(entry);
    }
}

void StringKeyCache::growKeysByParts()
{
    m_keysByPartsEvictionCount = 0;
    if (m_keysByParts.size() >= maxKeysByPartsTableSize)
        return;
    std::optional former = tryResize(m_keysByParts, m_keysByParts.size() * 2);
    if (!former)
        return;
    for (const KeyByParts& entry : *former) {
        if (!entry.countAndKinds)
            continue;
        uint32_t count = entry.countAndKinds & 0xff;
        uint64_t mixed = count;
        for (uint32_t i = 0; i < count; ++i)
            mixed = (mixed ^ entry.part(i)) * 0x9e3779b97f4a7c15ull;
        uint32_t hash = static_cast<uint32_t>((mixed ^ entry.countAndKinds >> 8) >> 32);
        size_t index = firstIndex(hash, m_keysByParts.size());
        if (m_keysByParts[index].countAndKinds && !m_keysByParts[index + 1].countAndKinds)
            ++index;
        else if (m_keysByParts[index].countAndKinds) {
            index += hash >> 31;
            ++m_keysByPartsEvictionCount;
        }
        m_keysByParts[index] = entry;
    }
}

size_t StringKeyCache::removeDeadKeysByParts(VM& vm)
{
    size_t removedCount = 0;
    for (KeyByParts& entry : m_keysByParts) {
        if (!entry.countAndKinds)
            continue;
        bool isAlive = vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, entry.key));
        for (uint32_t areCells = entry.countAndKinds >> 8, i = 0; areCells && isAlive; areCells >>= 1, ++i) {
            if (areCells & 1)
                isAlive = vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, entry.part(i)));
        }
        if (!isAlive) {
            entry = { };
            ++removedCount;
        }
    }
    return removedCount;
}

size_t StringKeyCache::removeDeadKeysOfPairs(VM& vm)
{
    size_t removedCount = 0;
    auto isAlive = [&](EncodedJSValue part) {
        JSValue value = JSValue::decode(part);
        return !value.isCell() || vm.heap.isMarked(value.asCell());
    };
    for (KeyOfPair& entry : m_keysOfPairs) {
        if (!entry.key || (vm.heap.isMarked(entry.key) && isAlive(entry.first) && isAlive(entry.second)))
            continue;
        entry = { };
        ++removedCount;
    }
    return removedCount;
}

size_t StringKeyCache::removeDeadCaseConversions(VM& vm)
{
    size_t removedCount = 0;
    m_hashesWithCaseConversions.forEachSetBit([&](size_t hash) {
        bool hasEntries = false;
        for (uint64_t& entry : std::span { m_caseConversions }.subspan(hash << caseConversionSetShift, size_t { 1 } << caseConversionSetShift)) {
            if (!entry)
                continue;
            if (vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry >> 32))) && vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)))) {
                hasEntries = true;
                continue;
            }
            entry = 0;
            ++removedCount;
        }
        if (!hasEntries)
            m_hashesWithCaseConversions.clear(hash);
    });
    return removedCount;
}

size_t StringKeyCache::removeDeadEntries(VM& vm, std::span<uint64_t> entries)
{
    size_t removedCount = 0;
    for (uint64_t& entry : entries) {
        if (entry && !vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)))) {
            entry = 0;
            ++removedCount;
        }
    }
    return removedCount;
}

size_t StringKeyCache::removeDeadIntegerKeys(VM& vm)
{
    size_t removedCount = 0;
    size_t aliveCount = 0;
    for (uint64_t& entry : m_integerKeys) {
        if (!entry)
            continue;
        if (vm.heap.isMarked(MarkedBlockCage::decode(m_cageBase, static_cast<uint32_t>(entry)))) {
            ++aliveCount;
            continue;
        }
        entry = 0;
        ++removedCount;
    }
    // A text that died is found no more, so the table has to hold what is alive and what will be entered until the next sweep, which is taken to be as much as since the last one.
    size_t wantedSize = std::max(minIntegerKeyTableSize, roundUpToPowerOfTwo((aliveCount + m_integerKeysEnteredSinceSweep) * 2));
    m_integerKeysEnteredSinceSweep = 0;
    if (wantedSize < m_integerKeys.size())
        resizeIntegerKeys(wantedSize);
    return removedCount;
}

size_t StringKeyCache::removeDeadEntries(VM& vm, CollectionScope scope)
{
    // A Structure may have died, as for the places themselves, which are cleared at every collection.
    forgetPlacesOfPairs();
    size_t removedCount = 0;
    m_writtenSinceCollection.forEachRegionToSweep(scope, [&](size_t, size_t) {
        removedCount += removeDeadEntries(vm, m_cachedKeys.mutableSpan());
        removedCount += removeDeadEntries(vm, m_caseConvertedStrings);
        removedCount += removeDeadCaseConversions(vm);
        removedCount += removeDeadIntegerKeys(vm);
        removedCount += removeDeadKeysByParts(vm);
        removedCount += removeDeadKeysOfPairs(vm);
    });
    return removedCount;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
