/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyCaches.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTClassPrototype.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTCompiledStructure.h"
#include "AOTEmptyPages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPendingStars.h"
#include "AOTProgramStatics.h"
#include "AOTProgramIdentifiers.h"
#include "AOTPropertyCachesAnswers.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTRuntimeRecords.h"
#include "AOTRuntimeReporter.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSiteAnswersOfGetters.h"
#include "AOTStaleAnswers.h"
#include "AOTState.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "ErrorInstance.h"
#include "FunctionPrototype.h"
#include "GetterSetter.h"
#include "JSCConfig.h"
#include "JSCInlines.h"
#include "PropertyOffset.h"
#include "RegExpObject.h"
#include "SlotVisitorInlines.h"
#include <array>
#include <bit>
#include <mutex>
#include <type_traits>
#include <wtf/Atomics.h>
#include <wtf/PageBlock.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/UnalignedAccess.h>
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
WTF_MAKE_TZONE_ALLOCATED_IMPL(HashedSlotTable);
WTF_MAKE_TZONE_ALLOCATED_IMPL(RowDisplacementTable);

struct PagesOfPlanes {
    explicit constexpr PagesOfPlanes(uint32_t numberOfBases)
        : sizeOfCellsInPlane(roundUpToMultipleOf<64>(static_cast<size_t>(numberOfBases) + RowDisplacementTable::numberOfColumns) * sizeof(PackedSlot))
        , sizeOfPlane(roundUpToMultipleOf<16 * KB>(sizeOfCellsInPlane))
        , sizeOfOccupied(roundUpToMultipleOf<16 * KB>(sizeOfCellsInPlane / sizeof(PackedSlot) / 8 + sizeof(uint64_t)))
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
static_assert(RowDisplacementTable::maxColumn < RowDisplacementTable::numberOfColumns);

static void* reservePagesOfTable(VM& vm, size_t size)
{
    void* result = vm.aotState().pages->tryAllocate(size);
    RELEASE_ASSERT(result, size);
    return result;
}

#if OS(LINUX) && !ENABLE(AOT_GUARDED_PAGES)
static constexpr bool pagesOfTablesCanGrow = true;

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

static constexpr uint32_t limitOfBasesInFrontTable = 2097152;
static_assert(limitOfBasesInFrontTable <= RowDisplacementTable::maxNumberOfBases);

static uint32_t numberOfBasesAtStart(uint32_t limitOfBases)
{
    return pagesOfTablesCanGrow ? std::min(RowDisplacementTable::initialNumberOfBases, limitOfBases) : limitOfBases;
}

StructureRows& StructureRows::singleton()
{
    static StructureRows* rows;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        rows = new StructureRows;
    });
    return *rows;
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
    m_rowsForStructureIDZero = static_cast<uint32_t*>(words) - (static_cast<uint32_t>(start) >> StructureRowWords::granuleShift);
}

size_t RowDisplacementTable::sizeOfPagesAtStart(uint64_t valueOfLimitOfBases)
{
    return PagesOfPlanes(numberOfBasesAtStart(static_cast<uint32_t>(std::clamp<uint64_t>(valueOfLimitOfBases, firstBase + 1, maxNumberOfBases)))).size;
}

RowDisplacementTable::RowDisplacementTable(VM& vm, CacheRegistry& registry, const CustomAccessorTable& customAccessors)
    : m_vm(vm)
    , m_customAccessors(customAccessors)
    , m_structureRows(StructureRows::singleton())
    , m_cageBase(vm.heap.cage().base())
{
    static_assert(limitOfBasesInFrontTable > firstBase);
    m_limitOfBases = limitOfBasesInFrontTable;
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
    Bits& bits = m_dependsOnChain[plane];
    if (!dependsOnChain) {
        bits.clear(index);
        return;
    }
    bits.set(index);
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
    if (!index || (slot.has(Slot::dependsOnChain) && !m_dependsOnChain[static_cast<unsigned>(access)].tryMakeRoomFor(index)))
        return false;
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
        m_rowsWrittenSinceLastSweep.forEachSetBit([&](size_t rowNumber) {
            numberRemoved += sweepRow(vm, static_cast<uint32_t>(rowNumber));
        });
    } else {
        for (unsigned rowNumber = 0; rowNumber < m_rows.size(); ++rowNumber)
            numberRemoved += sweepRow(vm, rowNumber);
    }
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
    static constexpr unsigned maxProbesBetweenRows = 4;
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

bool RowDisplacementTable::RowColumns::tryMakeRoomForOneMore()
{
    if (m_size < m_capacity)
        return true;
    uint16_t newCapacity = static_cast<uint16_t>(std::min<unsigned>(m_capacity * 2, UINT16_MAX));
    RELEASE_ASSERT(newCapacity > m_capacity);
    void* taken = nullptr;
    if (!tryFastMalloc(newCapacity * sizeof(uint16_t)).getValue(taken))
        return false;
    auto* newStorage = static_cast<uint16_t*>(taken);
    memcpy(newStorage, storage(), m_size * sizeof(uint16_t));
    if (m_capacity != inlineCapacity)
        fastFree(storage());
    memcpy(m_storage, &newStorage, sizeof(newStorage));
    m_capacity = newCapacity;
    return true;
}

bool RowDisplacementTable::RowColumns::tryAppend(uint16_t column)
{
    if (!tryMakeRoomForOneMore())
        return false;
    storage()[m_size++] = column;
    return true;
}

RowDisplacementTable::Rows::~Rows()
{
    for (uint32_t number = 0; number < m_size; ++number)
        (*this)[number].~Row();
    for (Row* segment : m_segments)
        fastFree(segment);
}

bool RowDisplacementTable::Rows::tryAlloc()
{
    if (!(m_size % rowsInSegment)) {
        void* segment = nullptr;
        if (!m_segments.tryReserveCapacity(std::max<size_t>(m_segments.size() + 1, m_segments.size() == m_segments.capacity() ? m_segments.capacity() * 2 : 0)) || !tryFastMalloc(rowsInSegment * sizeof(Row)).getValue(segment))
            return false;
        m_segments.append(static_cast<Row*>(segment));
    }
    new (NotNull, &m_segments[m_size / rowsInSegment][m_size % rowsInSegment]) Row();
    ++m_size;
    return true;
}

bool RowDisplacementTable::Bits::tryMakeRoomFor(size_t number)
{
    size_t had = m_words.size();
    if (number / 64 < had)
        return true;
    if (!m_words.tryGrow(std::max(number / 64 + 1, had * 2)))
        return false;
    zeroSpan(m_words.mutableSpan().subspan(had));
    return true;
}

bool RowDisplacementTable::tryAddRow()
{
    uint32_t number = m_rows.size();
    if (m_freeRows.capacity() <= number && !m_freeRows.tryReserveCapacity(std::max<size_t>(number + 1, m_freeRows.capacity() * 2)))
        return false;
    if (!m_rowsThatCanOutliveWhatTheyName.tryMakeRoomFor(number) || !m_rowsWrittenSinceLastSweep.tryMakeRoomFor(number) || !m_rows.tryAlloc())
        return false;
    m_freeRows.append(number);
    return true;
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
    if (m_freeRows.isEmpty() && !tryAddRow())
        return 0;
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
    uint32_t number = m_freeRows.takeLast();
    m_rows[number].owner = structure->id();
    m_rowsWrittenSinceLastSweep.set(number);
    last = { column, number };
    bool hadRoom = m_rows[number].columns.tryAppend(column);
    ASSERT_UNUSED(hadRoom, hadRoom);
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
    if (!columns.tryReserveCapacity(m_rows[rowNumber].columns.size() + 1) || (columnToAdd && !m_rows[rowNumber].columns.tryMakeRoomForOneMore()))
        return 0;
    columns.append(m_rows[rowNumber].columns.span());
    unsigned numberOfMovedColumns = columns.size();
    if (columnToAdd)
        columns.append(*columnToAdd);
    uint32_t newBase = findBase(columns.span());
    if (!newBase && columnToAdd)
        newBase = findBaseEvictingRows(columns.span(), owner);
    if (!newBase)
        return 0;
    for (SlotAccess access : allSlotAccesses) {
        if (!m_dependsOnChain[static_cast<unsigned>(access)].tryMakeRoomFor(newBase + std::ranges::max(columns)))
            return 0;
    }
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
        bool hadRoom = m_rows[rowNumber].columns.tryAppend(*columnToAdd);
        ASSERT_UNUSED(hadRoom, hadRoom);
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
        if (!rowAt(base).columns.tryAppend(column))
            return 0;
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
static constexpr uint32_t limitOfEntriesInHashedTable = 131072;
static constexpr uint32_t limitOfCustomAccessorRecords = 4096;
static_assert(hasOneBitSet(HashedSlotTable::initialNumberOfEntries) && HashedSlotTable::initialNumberOfEntries >= HashedSlot::ways);
static_assert(hasOneBitSet(limitOfEntriesInHashedTable) && limitOfEntriesInHashedTable >= HashedSlotTable::initialNumberOfEntries && limitOfEntriesInHashedTable <= maxAddressableEntries);
static_assert(!((1u << HashedSlotTable::regionShift) % (HashedSlot::ways * sizeof(HashedSlot))));

static size_t sizeOfRecords(uint32_t capacity)
{
    return roundUpToMultipleOf(pageSize(), std::max<size_t>(capacity, 1) * sizeof(CustomAccessorRecord));
}

CustomAccessorTable::CustomAccessorTable(VM& vm, CacheRegistry& registry)
    : m_vm(vm)
    , m_capacity(limitOfCustomAccessorRecords)
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
    else
        return std::nullopt;
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
    , m_maxNumberOfEntries(limitOfEntriesInHashedTable)
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

NEVER_INLINE void HashedSlotTable::forgetPlacesCopiedFromSlotForStores(const Slot& slot)
{
    if (slot.kind != Slot::Kind::Transition && slot.kind != Slot::Kind::Field && slot.kind != Slot::Kind::FieldCreation)
        return;
    for (Instance* instance : m_vm.aotState().instances) {
        PropertyCacheWords& words = instance->propertyCaches;
        if (std::exchange(words.hasCachedSlotValues, 0))
            zeroSpan(instance->keyedValuePlaces().last<KeyedValuePlace::numberOfPlacesPerAccess>());
    }
}

NEVER_INLINE void HashedSlotTable::forgetOrdinaryConstructorsWith(StructureID structureID)
{
    for (Instance* instance : m_vm.aotState().instances)
        instance->propertyCaches.headersOfPrototypeSlotsOfOrdinaryConstructors[PlaceOfOrdinaryConstructor::indexFor(structureID.bits())] = 0;
}

ALWAYS_INLINE_INTO_OTHER_FILES void HashedSlotTable::forgetPlacesCopiedFrom(const HashedSlot& filed)
{
    if (filed.access() == SlotAccess::Store) [[unlikely]]
        forgetPlacesCopiedFromSlotForStores(filed.slot);
    else if (filed.key == HashedSlot::keyFor(m_vm.propertyNames->hasInstanceSymbol.impl(), SlotAccess::Read) || filed.key == HashedSlot::keyFor(m_vm.propertyNames->prototype.impl(), SlotAccess::Read)) [[unlikely]]
        forgetOrdinaryConstructorsWith(filed.slot.structureID);
}

void HashedSlotTable::erase(Entry& entry)
{
    forgetPlacesCopiedFrom(entry.get());
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
        }
        uid->ref();
        ++m_numberOfFilledEntries;
    } else
        forgetPlacesCopiedFrom(entry->get());
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

static std::optional<uint32_t> frontColumnFor(VM& vm, const CachedName& name)
{
    std::optional<uint32_t> column = columnForNumber(name.identifierNumber);
    if (column && name.uid->isSymbol() && vm.aotState().identifiers->isSymbolOfRealm(*column))
        return std::nullopt;
    return column;
}

static void setBitOfName(auto& bits, uint32_t identifierNumber)
{
    bits[identifierNumber / 64] |= uint64_t { 1 } << identifierNumber % 64;
}

VMPropertyCaches::VMPropertyCaches(VM& vm)
    : m_vm(vm)
    , m_customAccessors(vm, *vm.aotState().caches)
    , m_front(vm, *vm.aotState().caches, m_customAccessors)
    , m_hashedTable(vm, *vm.aotState().caches, m_customAccessors)
    , m_tables(*static_cast<Tables*>(vm.aotState().pages->allocate(sizeof(Tables))))
{
    vm.aotState().caches->add(CacheKind::WeakByKey, "absence filters"_s, *this);
}

void VMPropertyCaches::rememberFirstElement(StructureID before, Structure* after)
{
    IndexingType shape = after->indexingType() & IndexingShapeMask;
    m_tables.firstElementTransitions[FirstElementTransition::indexFor(before, shape)] = { before.bits(), shape, after };
}

size_t VMPropertyCaches::removeDeadEntries(VM& vm, CollectionScope)
{
    size_t numberRemoved = 0;
    for (FirstElementTransition& entry : m_tables.firstElementTransitions) {
        if (!entry.after || (vm.heap.isMarked(entry.after) && vm.heap.isMarked(std::bit_cast<StructureID>(entry.structureIDBefore).decode())))
            continue;
        entry = { };
        ++numberRemoved;
    }
    for (AbsenceFilter& filter : m_absenceFilters) {
        if (!filter.structureID || vm.heap.isMarked(std::bit_cast<StructureID>(filter.structureID).decode()))
            continue;
        filter = { };
        ++numberRemoved;
    }
    for (NameShapesOfStructure& shapes : m_tables.nameShapes) {
        if (!shapes.structureID || vm.heap.isMarked(std::bit_cast<StructureID>(shapes.structureID).decode()))
            continue;
        shapes = { };
        ++numberRemoved;
    }
    forgetCreations();
    return numberRemoved;
}

void VMPropertyCaches::forgetCreations()
{
    bool hasWrapped = !++m_creationGeneration;
    m_creationGeneration += hasWrapped;
    for (Instance* instance : m_vm.aotState().instances) {
        instance->propertyCaches.creationGeneration = m_creationGeneration;
        if (hasWrapped && instance->objects.keyOrderRuns) [[unlikely]]
            zeroSpan(std::span { instance->objects.keyOrderRuns, sizeOfKeyOrderRunTable });
    }
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
    rememberNameOfText({ first, last, length, uid });
}

void VMPropertyCaches::rememberNameOfText(const NameOfText& name)
{
    size_t indexOfPair = NameOfText::hashOf(name.first, name.last, name.length) >> NameOfText::pairShift;
    NameOfText* pair = &m_namesOfTexts[2 * indexOfPair];
    if ((pair[0].uid == name.uid && pair[0].length == name.length) || (pair[1].uid == name.uid && pair[1].length == name.length))
        return;
    bool isFourth = !(++m_tables.numbersOfNamesRemembered[indexOfPair] % 4);
    NameOfText& entry = pair[pair[0].uid && !isFourth];
    name.uid->ref();
    if (entry.uid)
        entry.uid->deref();
    entry = name;
}

void VMPropertyCaches::rememberNameOfWideText(UniquedStringImpl* uid)
{
    uint32_t length = uid->length();
    if (uid->isSymbol() || !uid->is8Bit() || length < NameOfText::minLength || length > NameOfText::maxLength)
        return;
    std::array<char16_t, NameOfText::maxLength> units;
    StringImpl::copyCharacters(std::span { units }.first(length), uid->span8());
    const char16_t* end = units.data() + length;
    uint32_t lengthOfEntry = length + NameOfText::lengthOfWideText;
    if (length < 4) {
        rememberNameOfText({ WTF::unalignedLoad<uint32_t>(units.data()), WTF::unalignedLoad<uint32_t>(end - 2), lengthOfEntry, uid });
        return;
    }
    uint64_t first = WTF::unalignedLoad<uint64_t>(units.data());
    uint64_t last = WTF::unalignedLoad<uint64_t>(end - 4);
    if (length >= 8) {
        uint64_t second = WTF::unalignedLoad<uint64_t>(units.data() + 4);
        uint64_t third = WTF::unalignedLoad<uint64_t>(end - 8);
        first |= second << 8;
        last = third | last << 8;
    }
    rememberNameOfText({ first, last, lengthOfEntry, uid });
}

static bool listsEveryNameInItsTable(Structure* structure)
{
    return (!structure->isDictionary() || structure->mayBePrototype()) && !structure->hasPolyProto() && !hasIndexedProperties(structure->indexingType())
        && structure->propertyAccessesAreCacheable() && structure->propertyAccessesAreCacheableForAbsence()
        && !structure->typeInfo().overridesGetOwnPropertySlot() && !structure->hasNonReifiedStaticProperties();
}

void VMPropertyCaches::didCreateInstance(Instance& instance)
{
    if (m_hasInstance && !m_isSharedByInstances) {
        m_isSharedByInstances = std::ranges::count_if(m_vm.aotState().instances, [&](const Instance* each) {
            return !each->globalObject || each->globalObject->microtaskRunnability() != QueuedTaskResult::Discard;
        }) > 1;
        m_front.clear();
        m_hashedTable.clear();
        for (Instance* earlier : m_vm.aotState().instances) {
            if (!earlier->propertyCaches.hasRunSinceClear)
                continue;
            zeroSpan(earlier->keyedValuePlaces());
            earlier->propertyCaches.forwardingProxyHandlers = { };
            zeroSpan(earlier->acceptedKindsOfValues());
            earlier->propertyCaches.hasCachedSlotValues = 0;
            earlier->propertyCaches.accessTable.forgetEverything();
            if (StringKeyCache* keys = earlier->state ? earlier->state->stringKeys.getIfCreated() : nullptr)
                keys->forgetPlacesOfPairs();
            bool isRetired = earlier->globalObject && earlier->globalObject->microtaskRunnability() == QueuedTaskResult::Discard;
            earlier->propertyCaches.hasRunSinceClear = !isRetired || m_vm.entryScope || m_vm.topEntryFrame;
        }
    }
    m_hasInstance = true;
    instance.propertyCaches.hasRunSinceClear = true;
    if (instance.globalObject) {
        m_filteredObjects.add(instance.globalObject->objectPrototype());
        didChangeObjectPrototype(instance.globalObject->objectPrototype(), nullptr);
    }
    publishWords(instance);
}

void VMPropertyCaches::didChangeObjectPrototype(JSObject* prototype, UniquedStringImpl* uidOrNull)
{
    Structure* structure = WTF::opaque(prototype->structure());
    const ProgramIdentifiers* identifiers = identifiersOfRealm(m_vm, structure->realm());
    forgetCreations();
    if (!identifiers || !listsEveryNameInItsTable(structure) || !structure->storedPrototype().isNull()) {
        m_namesOfObjectPrototypes.fill(UINT64_MAX);
        m_objectPrototypeStoreStoppers.fill(UINT64_MAX);
        m_tables.nameShapesOfObjectPrototypes = { UINT64_MAX, UINT64_MAX };
        return;
    }
    auto add = [&](UniquedStringImpl* uid, bool stopsStores) {
        if (!uid->isSymbol())
            m_tables.nameShapesOfObjectPrototypes.add(*uid);
        if (stopsStores && uid != m_vm.propertyNames->underscoreProto.impl())
            setBitOfName(m_objectPrototypeStoreStoppers, FieldTableEntry::noName);
        std::optional<uint32_t> number = identifiers->numberOf(uid);
        if (!number || !FieldTableEntry::hasColumn(*number))
            return;
        setBitOfName(m_namesOfObjectPrototypes, *number);
        if (stopsStores)
            setBitOfName(m_objectPrototypeStoreStoppers, *number);
    };
    auto stopsStores = [&](unsigned attributes) {
        return structure->typeInfo().overridesPut() || attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue;
    };
    if (uidOrNull) {
        unsigned attributes = 0;
        add(uidOrNull, isValidOffset(structure->get(m_vm, uidOrNull, attributes)) && stopsStores(attributes));
        return;
    }
    structure->forEachProperty(m_vm, [&](const auto& entry) {
        add(entry.key(), stopsStores(entry.attributes()));
        return true;
    });
}

static Structure* structureOfListedNames(Structure* structure)
{
    return structure->isCompiledStructure() ? uncheckedDowncast<CompiledStructure>(structure)->extraPropertiesStructure() : structure;
}

NameShapesOfStructure VMPropertyCaches::shapesBeforeListing(JSGlobalObject* globalObject, Structure* structure) const
{
    NameShapesOfStructure neverListed { structure->id().bits(), 0, 0, 0, NameShapes::ofAnyName() };
    JSValue prototype = structure->hasPolyProto() || structure->typeInfo().overridesGetPrototype() ? JSValue() : structure->storedPrototype();
    if (!prototype || (!prototype.isNull() && prototype != JSValue(globalObject->objectPrototype())))
        return neverListed;
    size_t numberOfNames = 0;
    if (structure->isCompiledStructure()) {
        auto* compiled = uncheckedDowncast<CompiledStructure>(structure);
        if (!compiled->describesObject() || !identifiersOfRealm(m_vm, structure->realm()) || compiled->layout().kind() != LayoutKind::Record || (compiled->hasExtraProperties() && !compiled->listsEveryExtraProperty()))
            return neverListed;
        numberOfNames = compiled->layout().fields().size();
    } else if (structure->typeInfo().type() != FinalObjectType)
        return neverListed;
    if (Structure* listed = structureOfListedNames(structure)) {
        if (listed->isDictionary() || !listsEveryNameInItsTable(listed))
            return neverListed;
        numberOfNames += numberOfSlotsForMaxOffset(listed->maxOffset(), listed->inlineCapacity());
    }
    return { structure->id().bits(), static_cast<uint32_t>(1 + numberOfNames / 2), !prototype.isNull(), 0, NameShapes::ofAnyName() };
}

NameShapes VMPropertyCaches::listOwnNames(Structure* structure) const
{
    NameShapes names;
    if (structure->isCompiledStructure()) {
        ProgramIdentifiers* identifiers = identifiersOfRealm(m_vm, structure->realm());
        for (const FieldData& field : uncheckedDowncast<CompiledStructure>(structure)->layout().fields()) {
            UniquedStringImpl* name = identifiers->identifier(field.name).impl();
            if (name && !name->isSymbol())
                names.add(*name);
        }
    }
    if (Structure* listed = structureOfListedNames(structure)) {
        listed->forEachPropertyConcurrently([&](const auto& entry) {
            if (!entry.key()->isSymbol())
                names.add(*entry.key());
            return true;
        });
    }
    return names;
}

bool VMPropertyCaches::mayHaveNameEqualTo(JSGlobalObject* globalObject, JSObject* object, const JSString& key)
{
    constexpr unsigned maxIndexLength = NameShapesOfStructure::maxIndexLength;
    unsigned length = key.length();
    Structure* structure = object->structure();
    NameShapesOfStructure& shapes = m_tables.nameShapes[NameShapesOfStructure::indexFor(structure->id())];
    if (!length)
        return true;
    if (shapes.structureID != structure->id().bits())
        shapes = shapesBeforeListing(globalObject, structure);
    if (shapes.ownNames.areOfAnyName()) {
        if (!shapes.questionsBeforeListing || --shapes.questionsBeforeListing)
            return true;
        shapes.ownNames = listOwnNames(structure);
    }
    if (Object* compiled = compiledObjectOrNull(object); compiled && hasPendingStars(*compiled))
        return true;
    NameShapes above;
    if (shapes.isBelowObjectPrototype)
        above = m_tables.nameShapesOfObjectPrototypes;
    bool isLengthOfName = (shapes.ownNames.lengths | above.lengths) & NameShapes::bitOfLength(length);
    if (!isLengthOfName && length > maxIndexLength)
        return false;
    constexpr int mostUnitsThatTellNamesApart = 32;
    if (isLengthOfName && std::popcount(shapes.ownNames.firstUnits | above.firstUnits) > mostUnitsThatTellNamesApart)
        return true;
    JSRopeString::LeafIterator pieces(&key);
    StringView piece = pieces.next();
    while (piece.isEmpty() && !pieces.isAtEnd())
        piece = pieces.next();
    if (piece.isEmpty() || (length <= maxIndexLength && isASCIIDigit(piece[0])))
        return true;
    return isLengthOfName && (shapes.ownNames.firstUnits | above.firstUnits) & NameShapes::bitOfUnit(piece[0]);
}

size_t VMPropertyCaches::maxSizeOfPagesAtStart(const Image&)
{
    return roundUpToMultipleOf(pageSize(), sizeof(Tables))
        + sizeOfRecords(limitOfCustomAccessorRecords)
        + RowDisplacementTable::sizeOfPagesAtStart(limitOfBasesInFrontTable)
        + sizeOfReservation(limitOfEntriesInHashedTable);
}

void VMPropertyCaches::publishWords(Instance& instance) const
{
    PropertyCacheWords& words = instance.propertyCaches;
    const uint32_t* rowsForStructureIDZero = StructureRows::singleton().rowsForStructureIDZero();
    for (SlotAccess access : allSlotAccesses)
        words.front[static_cast<unsigned>(access)] = { rowsForStructureIDZero, m_front.plane(access) };
    words.hashedSlots = m_hashedTable.entries();
    words.hashedSlotOffsetMask = m_hashedTable.offsetMask();
    words.customAccessors = m_customAccessors.records();
    words.absenceFilters = const_cast<AbsenceFilter*>(m_absenceFilters.data());
    words.absenceFilterGenerationForStores = m_absenceFilterGenerationForStores;
    words.changesUnderNamesOnChains = m_changesUnderNamesOnChains;
    words.creationGeneration = m_creationGeneration;
    words.namesChangedOnChains = m_namesChangedOnChains.data();
    words.namesOfObjectPrototypes = m_namesOfObjectPrototypes.data();
    words.objectPrototypeStoreStoppers = m_objectPrototypeStoreStoppers.data();
    words.nameShapes = m_tables.nameShapes.data();
    words.nameShapesOfObjectPrototypes = &m_tables.nameShapesOfObjectPrototypes;
    words.missesOfPairs = const_cast<uint8_t*>(m_missesOfPairs.data());
    words.namesOfTexts = m_namesOfTexts.data();
    words.infoOfClassPrototypes = ClassPrototype::info();
}

static bool listsOwnNames(JSGlobalObject* globalObject, JSObject* object)
{
    Structure* structure = object->structure();
    if (structure->isCompiledStructure()) {
        auto* compiled = uncheckedDowncast<CompiledStructure>(structure);
        return compiled->describesObject() && (!compiled->hasExtraProperties() || (compiled->listsEveryExtraProperty() && listsEveryNameInItsTable(compiled->extraPropertiesStructure())));
    }
    return object->classInfo() == ClassPrototype::info() || listsEveryNameInItsTable(structure) || isArrayPrototypeWithWritableLength(globalObject, object);
}

static bool addOwnNamesThatStopStores(VM& vm, const ProgramIdentifiers& identifiers, JSObject* object, bool listsNames, AbsenceFilter& filter)
{
    auto addNamesInTable = [&](Structure* structure) {
        bool isArrayPrototype = isArrayPrototypeWithWritableLength(structure->realm(), object);
        if (!isArrayPrototype && !listsEveryNameInItsTable(structure))
            return false;
        if (!isArrayPrototype && (structure->typeInfo().overridesPut() || structure->typeInfo().overridesGetPrototype()))
            filter.generationForStores = 0;
        if (!listsNames)
            return true;
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
            if (!listsNames)
                break;
            if (isFrozen || field.flags & (fieldIsReadOnly | fieldIsAccessor) || field.slot.representation() == SlotRepresentation::DeferredClosure)
                filter.addNameThatStopsStores(field.name);
        }
        return !compiled->hasExtraProperties() || addNamesInTable(compiled->extraPropertiesStructure());
    }
    if (object->classInfo() == ClassPrototype::info()) {
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
    static constexpr unsigned maxObjectsAbove = 8;
    static constexpr unsigned mostFieldsAskedOneByOne = 8;
    AbsenceFilter& filter = m_absenceFilters[AbsenceFilter::indexFor(structure->id().bits())];
    if (filter.structureID == structure->id().bits() && filter.generationForStores == m_absenceFilterGenerationForStores && filter.changesUnderNames == m_changesUnderNamesOnChains)
        return;
    for (JSObject* holder = structure->storedPrototypeObject(); holder; holder = holder->getPrototypeDirect().getObject())
        Object::ensureStructureForExtraPropertiesIfNeeded(m_vm, holder);
    const ProgramIdentifiers* identifiers = identifiersOfRealm(m_vm, globalObject);
    if (!identifiers || structure->realm() != globalObject)
        return;
    for (JSObject* holder = object->getPrototypeDirect().getObject(); holder; holder = holder->getPrototypeDirect().getObject()) {
        if (!listsOwnNames(globalObject, holder))
            return;
    }
    const Object* receiver = compiledObjectOrNull(object);
    const bool listsNamesAbove = !receiver || receiver->layout().fields().size() > mostFieldsAskedOneByOne;
    AbsenceFilter filled { structure->id().bits(), m_absenceFilterGenerationForStores, { }, m_changesUnderNamesOnChains, 0 };
    unsigned numberOfObjects = 0;
    for (JSObject* holder = object->getPrototypeDirect().getObject(); holder; holder = holder->getPrototypeDirect().getObject()) {
        if (++numberOfObjects > maxObjectsAbove || !addOwnNamesThatStopStores(m_vm, *identifiers, holder, listsNamesAbove, filled))
            return;
        m_filteredObjects.add(holder);
    }
    if (receiver) {
        AbsenceFilter ofAllNames = std::exchange(filled, AbsenceFilter { filled.structureID, filled.generationForStores, { }, filled.changesUnderNames, 0 });
        for (const FieldData& field : receiver->layout().fields()) {
            if ((listsNamesAbove && ofAllNames.letsStoresPass(field.name)) || !FieldTableEntry::hasColumn(field.name))
                continue;
            ChainWalk walk = walkChain({ globalObject, structure, object, ChainWalkMode::AllocatesNothing, ChainWalkUse::UsedAtOnce }, identifiersOfRealm(m_vm, structure->realm())->identifier(field.name).impl());
            if (storeThroughChain(walk) != StoreThroughChain::CreatesOwnProperty)
                filled.addNameThatStopsStores(field.name);
        }
    }
    filter = filled;
    if (listsNamesAbove)
        countEvent(m_vm, RuntimeCounterID::AbsenceFiltersFilled);
}

bool VMPropertyCaches::storeCreatesOwnField(const Object& object, uint32_t name) const
{
    if (!FieldTableEntry::hasColumn(name))
        return false;
    if (object.compiledStructure().flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype))
        return !mayObjectPrototypeStopStore(name);
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

static bool mayStopStores(JSObject* changed, UniquedStringImpl* uid)
{
    Structure* structure = changed->structure();
    bool isField = false;
    if (Object* object = compiledObjectOrNull(changed)) {
        const FieldData* field = object->fieldNamed(object->state(), uid);
        if (field && object->isInSlot(*field))
            return object->attributesOf(*field) & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue || field->slot.representation() == SlotRepresentation::DeferredClosure;
        isField = field;
        JSObject* extra = extraPropertiesIfCreated(*object);
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
    const ProgramIdentifiers* identifiers = identifiersOfRealm(m_vm, changed ? changed->structure()->realm() : nullptr);
    if (uidOrNull && identifiers) {
        std::optional<uint32_t> number = identifiers->numberOf(uidOrNull);
        if (!number && uidOrNull->isSymbol()) {
            for (Instance* instance : m_vm.aotState().instances) {
                number = identifiersOf(*instance).numberOf(uidOrNull);
                if (number)
                    break;
            }
        }
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
        instance->propertyCaches.absenceFilterGenerationForStores = m_absenceFilterGenerationForStores;
        instance->propertyCaches.changesUnderNamesOnChains = m_changesUnderNamesOnChains;
    }
    forgetCreations();
}

void VMPropertyCaches::didChangeObjectOnSomeChain(JSObject* changed, UniquedStringImpl* uidOrNull)
{
    if (!changed || m_filteredObjects.mayContain(changed))
        didChangeFilteredObject(changed, uidOrNull);
    for (Instance* instance : m_vm.aotState().instances) {
        instance->propertyCaches.accessTable.didChangeObjectOnSomeChain();
        if (std::exchange(instance->propertyCaches.readGroupsSayAbsence, 0))
            instance->state->statics->clearCaches();
        if (std::exchange(instance->propertyCaches.placesSayAbsence, 0)) {
            zeroSpan(instance->keyedValuePlaces().first<KeyedValuePlace::numberOfPlacesPerAccess>());
            instance->propertyCaches.forwardingProxyHandlers = { };
            if (!(++instance->propertyCaches.timesAbsencesWereForgotten & StringKeyCache::KeyOfPair::maskBelowOtherRead)) [[unlikely]]
                StringKeyCache::forgetPlacesOfPairs(*instance);
        }
    }
}

void VMPropertyCaches::didReplaceValueOnObjectOfSomeChain(JSObject* changed, UniquedStringImpl* uid)
{
    if (m_filteredObjects.mayContain(changed))
        didChangeFilteredObject(changed, uid);
}

void VMPropertyCaches::didStartWatchingReplacements()
{
    for (Instance* instance : m_vm.aotState().instances) {
        if (std::exchange(instance->propertyCaches.placesTakeReplacementsAsUnwatched, 0))
            zeroSpan(instance->keyedValuePlaces().last<KeyedValuePlace::numberOfPlacesPerAccess>());
    }
}

void VMPropertyCaches::set(Structure* structure, const CachedName& name, SlotAccess access, const Slot& slot)
{
    StructureID id = structure->id();
    ASSERT(slot.structureID == id);
    const bool isExcludedFromHashedTable = access == SlotAccess::Read && isPreparedBeforeLookups(m_vm, name.uid);
    if (std::optional<uint32_t> column = hasFrontCells(access) ? frontColumnFor(m_vm, name) : std::nullopt) {
        bool isSet = m_front.trySet(structure, *column, access, slot);
        if (m_front.takeDidMovePlanes()) {
            for (Instance* instance : m_vm.aotState().instances)
                publishWords(*instance);
        }
        if (isSet && !slot.isForProxies()) {
            if (slot.has(Slot::dependsOnChain)) {
                auto result = m_columnsByNameAddress.add(name.uid, static_cast<uint16_t>(*column));
                ASSERT(result.iterator->value == *column);
                if (result.isNewEntry)
                    name.uid->ref();
            }
            m_hashedTable.remove(id, name.uid, access);
            return;
        }
        if (!isSet)
            m_front.remove(id, *column, access);
    }
    if (isExcludedFromHashedTable)
        return;
    if (m_hashedTable.set(name.uid, access, slot) == HashedSlotTable::SetResult::Stored)
        return;
    for (Instance* instance : m_vm.aotState().instances)
        publishWords(*instance);
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

bool VMPropertyCaches::holdsInFrontRows(StructureID id, const CachedName& name, SlotAccess access) const
{
    std::optional<uint32_t> column = hasFrontCells(access) ? columnForNumber(name.identifierNumber) : std::nullopt;
    return column && m_front.find(id, *column, access);
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

static std::optional<Slot> slotBehindCreatedMember(const Slot& slot, const CreatedMembers& created, UniquedStringImpl* uid, PropertyOffset offset, unsigned attributes)
{
    if (!slot.structureID || slot.kind == Slot::Kind::Uncacheable)
        return std::nullopt;
    Structure* structure = slot.structureID.decode();
    if (!structure->isObject() || !structure->hasMonoProto())
        return std::nullopt;
    JSObject* prototype = created.prototype;
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

static bool belongsToOneObject(Structure*);

bool VMPropertyCaches::keepChainDependent(VM& vm, uint32_t structureIDBits, UniquedStringImpl* uid, const CreatedMembers& created)
{
    ASSERT(uid);
    ASSERT(vm.heap.isDeferred());
    unsigned attributes = 0;
    PropertyOffset offset = created.prototype->structure()->get(vm, uid, attributes);
    if (!isValidOffset(offset))
        return true;
    StructureID id = std::bit_cast<StructureID>(structureIDBits);
    std::optional<uint16_t> column = m_columnsByNameAddress.getOptional(uid);
    CachedName name { uid, column ? std::optional<uint32_t>(*column) : std::nullopt };
    bool keepsSome = false;
    for (SlotAccess access : allSlotAccesses) {
        std::optional<Slot> slot = find(id, name, access);
        if (!slot || !slot->has(Slot::dependsOnChain))
            continue;
        std::optional<Slot> kept = slotBehindCreatedMember(*slot, created, uid, offset, attributes);
        if (!kept) {
            if (column && hasFrontCells(access))
                m_front.remove(id, *column, access);
            m_hashedTable.remove(id, uid, access);
            continue;
        }
        keepsSome = true;
        if (kept->headerBits() == slot->headerBits() && kept->payloadBits() == slot->payloadBits())
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
    uintptr_t wordsToUndefined = (std::bit_cast<uintptr_t>(instance.state->objectThatHoldsUndefined->inlineStorage()) - vm.heap.cage().base()) / sizeof(EncodedJSValue);
    words.wordOfCageThatHoldsUndefined = wordsToUndefined <= UINT32_MAX ? static_cast<uint32_t>(wordsToUndefined) : 0;
    words.hasInstanceUID = vm.propertyNames->hasInstanceSymbol.impl();
    words.prototypeUID = vm.propertyNames->prototype.impl();
    words.lengthUID = vm.propertyNames->length.impl();
    words.nameUID = vm.propertyNames->name.impl();
    words.lookupOfOrdinaryObjects = std::bit_cast<const void*>(JSFinalObject::info()->methodTable.getOwnPropertySlot);
    words.lookupOfArrays = std::bit_cast<const void*>(JSArray::info()->methodTable.getOwnPropertySlot);
    words.lookupOfFunctions = std::bit_cast<const void*>(JSFunction::info()->methodTable.getOwnPropertySlot);
    const ProgramIdentifiers* identifiers = vm.aotState().identifiers.get();
    auto setNumberOfNameOfTrap = [&](TrapOfProxy trap, const Identifier& name) {
        std::optional<uint32_t> number = identifiers ? identifiers->numberOf(name.impl()) : std::nullopt;
        words.numbersOfNamesOfTraps[static_cast<unsigned>(trap)] = number && RowDisplacementTable::hasColumn(*number) ? *number : 0;
    };
    setNumberOfNameOfTrap(TrapOfProxy::Get, vm.propertyNames->get);
    setNumberOfNameOfTrap(TrapOfProxy::Has, vm.propertyNames->has);
    setNumberOfNameOfTrap(TrapOfProxy::Set, vm.propertyNames->set);
    words.defaultHasInstanceFunction = globalObject->functionProtoHasInstanceSymbolFunction();
    words.holderOfDefaultHasInstanceFunction = globalObject->functionPrototype();
    const Image& image = *vm.aotState().image;
    if (image.sharedCodeCopies().empty())
        return;
    uint8_t* sharedCode = const_cast<uint8_t*>(image.sharedCodeCopy(0));
    std::span offsets = image.slotHandlerOffsets();
    std::span handlers(&instance.slotHandlers()[0][0], sizeof(SlotHandlers) / sizeof(void*));
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

template class CreatedAtFirstUse<PropertyCaches>;
template class CreatedAtFirstUse<StringKeyCache>;

PropertyCaches::PropertyCaches(State& state)
    : m_state(state)
    , m_vm(*state.instance.vm)
    , m_tables(*state.instance.vm->aotState().propertyCaches)
    , m_tablesOwnerID(state.chainDependents->addOwner(m_tables))
    , m_siteCaches(state, m_tables.customAccessors())
    , m_answersAtSites(state, m_siteCaches)
    , m_structuresBelowBuiltinClasses(state)
    , m_errorsWithMessage(state)
    , m_missCounters(state.tableArena.allocate<uint8_t>(numberOfMissCounters), numberOfMissCounters)
    , m_prototypeChainFilters(state.tableArena.allocate<PrototypeChainFilter>(PrototypeChainFilter::numberOfFilters), PrototypeChainFilter::numberOfFilters)
    , m_switchTargets(state.tableArena.allocate<StringSwitchTarget>(StringSwitchTarget::numberOfTargets), StringSwitchTarget::numberOfTargets)
    , m_functionAllocationEntries(state.tableArena.allocate<FunctionAllocationEntry>(FunctionAllocationEntry::numberOfBirths), FunctionAllocationEntry::numberOfBirths)
    , m_closureAdditions(state.tableArena.allocate<ClosureAddition>(ClosureAddition::numberOfPlaces), ClosureAddition::numberOfPlaces)
{
    state.caches->add(CacheKind::StrongRoots, "Structures of additions in an Eden collection"_s, *this);
    state.caches->addObserver("PropertyCaches"_s, *this);
    state.caches->addClearedEachCollection("exits of fields"_s, asMutableByteSpan(std::span { m_fieldExits }));
    state.caches->addClearedEachCollection("returns of fields"_s, asMutableByteSpan(std::span { m_fieldReturns }));
    state.caches->addClearedEachCollection("traps of proxy handlers"_s, asMutableByteSpan(std::span { m_proxyHandlerTraps }));
    state.caches->addClearedEachCollection("filters of prototype chains"_s, asMutableByteSpan(std::span { m_prototypeChainFilters }), &state.instance.propertyCaches.prototypeChainFiltersMayBeFilled);
    state.caches->addClearedEachCollection("targets of switches"_s, asMutableByteSpan(std::span { m_switchTargets }), &m_switchTargetsMayBeFilled);
    state.caches->addClearedEachCollection("births of functions"_s, asMutableByteSpan(std::span { m_functionAllocationEntries }), &m_functionAllocationsMayBeFilled);
    state.caches->addClearedEachCollection("additions to closures"_s, asMutableByteSpan(std::span { m_closureAdditions }), &m_closureAdditionsMayBeFilled);
    state.caches->addClearedEachCollection("extra properties added"_s, asMutableByteSpan(std::span { m_addedExtraProperties }));
    state.caches->addClearedEachCollection("ordinary constructors"_s, asMutableByteSpan(std::span { state.instance.propertyCaches.headersOfPrototypeSlotsOfOrdinaryConstructors }));
    state.caches->add(CacheKind::ClearedEachCollection, "exits of fields beside their table"_s, m_fieldExitsBesideTable);
    state.caches->add(CacheKind::ClearedEachCollection, "extra properties added beside their table"_s, m_addedExtraPropertiesBesideTable);
    state.instance.propertyCaches.fieldExits = m_fieldExits.data();
    state.instance.propertyCaches.fieldReturns = m_fieldReturns.data();
    state.instance.propertyCaches.prototypeChainFilters = m_prototypeChainFilters.data();
    state.instance.propertyCaches.switchTargets = m_switchTargets.data();
    state.instance.propertyCaches.functionAllocationEntries = m_functionAllocationEntries.data();
    state.instance.propertyCaches.closureAdditions = m_closureAdditions.data();
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
    return { identifiersOf(m_state.instance).uid(identifierNumber), identifierNumber };
}

CachedName PropertyCaches::nameOf(UniquedStringImpl* uid) const
{
    return { uid, identifiersOf(m_state.instance).numberOf(uid) };
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
    return compiled && ((compiled->extraPropertiesStructure() && !compiled->listsEveryExtraProperty()) || compiled->hasKeyOrderOfOneObject() || compiled->heldExtraProperties());
}

void PropertyCaches::cache(const CachedAccess& access, const PropertyAnswer& answer)
{
    if (answer.didFlatten) {
        didFileNothing();
        return;
    }
    ASSERT(answer.access == access.access);
    Structure* structure = access.structure;
    if (!canCache(structure, answer))
        return;
    if (structure == m_state.instance.globalObject->numberObjectStructureConcurrently())
        m_state.instance.propertyCaches.numberStructureID = structure->id().bits();
    else if (structure == m_state.instance.globalObject->booleanObjectStructureConcurrently())
        m_state.instance.propertyCaches.booleanStructureID = structure->id().bits();
    ensureChangesAreReported(answer.changeableObjects.span());
    Slot slot = slotFor(answer, structure);
    m_tables.set(structure, belongsToOneObject(structure) ? CachedName { access.name.uid, std::nullopt } : access.name, access.access, slot);
    if (slot.has(Slot::dependsOnChain))
        m_state.chainDependents->add(answer.changeableObjects.span(), { access.name.uid, structure->id().bits(), m_tablesOwnerID });
    if (answer.newStructure)
        didAddProperty(structure, answer.newStructure);
    if (access.access == SlotAccess::Read)
        m_errorsWithMessage.keepIfRead(structure, access.name.uid);
    if (access.access == SlotAccess::Read)
        keepIfBelowBuiltinClass(structure);
}

static unsigned hashOfName(UniquedStringImpl* uidOrNull)
{
    return uidOrNull ? uidOrNull->existingSymbolAwareHash() : 0;
}

bool PropertyCaches::shouldFile(StructureID structureID, UniquedStringImpl* uid, SlotAccess access)
{
    m_counterOfPairToFile = nullptr;
    if (std::exchange(m_state.instance.propertyCaches.missIsDueForFiling, 0))
        return true;
    if (m_shouldForgetMisses.load(std::memory_order_relaxed)) [[unlikely]] {
        m_shouldForgetMisses.store(false, std::memory_order_relaxed);
        zeroSpan(m_missCounters);
    }
    constexpr uint8_t maskOfCount = (1u << bitsOfMissCount) - 1;
    constexpr uint8_t wasFiled = maskOfCount;
    constexpr uint32_t countersOfPair = 4;
    static_assert(hasOneBitSet(numberOfMissCounters) && numberOfMissCounters <= 1u << 24 && missesBeforeFiling < wasFiled && storeMissesBeforeFiling < wasFiled);
    static_assert(hasOneBitSet(countersOfPair) && countersOfPair <= numberOfMissCounters);
    uint32_t hash = WTF::pairIntHash(structureID.bits(), hashOfName(uid) + static_cast<uint32_t>(access));
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
    return false;
}

void PropertyCaches::remember(const FieldExit& exit)
{
    FieldExit& place = placeOf(exit.structureBefore, exit.name);
    putAtPlace(place, m_fieldExitsBesideTable, exit);
    if (exit.widthOfSlot)
        m_state.instance.propertyCaches.placesOfFieldExitsBySlot[((exit.structureBefore.bits() >> StructureRowWords::granuleShift) + (exit.offsetOfSlot >> 2)) % FieldExit::numberOfPlacesBySlot] = static_cast<uint8_t>(&place - m_fieldExits.data());
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
    return structure->id() == m_structureMade || recordAddition(structure, uid);
}

bool FirstAdditions::recordAddition(Structure* structure, UniquedStringImpl* uid)
{
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
        m_evictionSeed = m_evictionSeed * 1664525 + 1013904223;
        place = set + std::min<uint32_t>(std::countl_zero(m_evictionSeed), pairsPerSet - 1);
    }
    *place = hash;
    return false;
}

void didFinishFilling(Structure* structure)
{
    ASSERT(!structure->isDictionary());
    structure->setHasBeenFlattenedBefore(false);
    structure->setHasBeenDictionary(false);
}

bool hasFewEnoughNamesToBeFilled(Structure* structure)
{
    static constexpr unsigned maxNamesCopiedIntoTable = 64;
    return numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity()) <= maxNamesCopiedIntoTable;
}

static bool isBeingFilled(JSObject* object)
{
    Structure* structure = object->structure();
    return structure->isDictionary() && !structure->isUncacheableDictionary() && !structure->hasBeenFlattenedBefore();
}

bool FirstAdditions::adoptSharedStructure(VM& vm, ObjectBeingFilled& filled)
{
    Structure* structure = filled.object->structure();
    if (!filled.sharedStructure || filled.object->type() != FinalObjectType || structure->mayBePrototype() || !structure->isStructureExtensible())
        return false;
    Structure* shared = filled.sharedStructure.decode();
    bool isShared = true;
    structure->forEachProperty(vm, [&](const auto& entry) {
        if (entry.offset() > shared->maxOffset()) {
            PropertyOffset offset = invalidOffset;
            Structure* next = Structure::addPropertyTransitionToExistingStructure(shared, entry.key(), entry.attributes(), offset);
            isShared = next && !next->isDictionary() && offset == entry.offset();
            if (isShared)
                shared = next;
        }
        return isShared;
    });
    filled.sharedStructure = shared->id();
    if (!isShared || shared->maxOffset() != structure->maxOffset() || shared->inlineCapacity() != structure->inlineCapacity() || shared->indexingModeIncludingHistory() != structure->indexingModeIncludingHistory() || shared->realm() != structure->realm() || shared->hasMonoProto() != structure->hasMonoProto() || shared->hasPolyProto() || shared->storedPrototype() != structure->storedPrototype() || shared->mayBePrototype() || shared->isWatchingReplacement() || !shared->isStructureExtensible() || shared->hasImmutableProperties() != structure->hasImmutableProperties())
        return false;
    uint32_t numberOfNames = 0;
    structure->forEachProperty(vm, [&](const auto& entry) {
        ++numberOfNames;
        unsigned attributes;
        isShared = !entry.attributes() && shared->getWithoutMaterializing(entry.key(), attributes) == entry.offset() && !attributes;
        return isShared;
    });
    if (!isShared || numberOfNames != numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity()))
        return false;
    filled.object->setStructure(vm, shared);
    return true;
}

void FirstAdditions::adoptSharedStructure(VM& vm, JSObject* object)
{
    for (ObjectBeingFilled& filled : m_objectsBeingFilled) {
        if (filled.object == object && isBeingFilled(object) && adoptSharedStructure(vm, filled))
            filled = { };
    }
}

void FirstAdditions::finishFilling(VM& vm, ObjectBeingFilled filled)
{
    if (!filled.object || !isBeingFilled(filled.object) || adoptSharedStructure(vm, filled))
        return;
    Structure* structure = filled.object->structure();
    structure->flattenDictionaryStructure(vm, filled.object);
    didFinishFilling(structure);
}

bool FirstAdditions::forget(JSObject* object)
{
    for (ObjectBeingFilled& filled : m_objectsBeingFilled) {
        if (filled.object == object) {
            filled = { };
            return true;
        }
    }
    return false;
}

bool FirstAdditions::mayBeginToFill(JSObject* object) const
{
    static constexpr uintptr_t maxTimesOneObjectIsFilled = 4;
    static_assert(maxTimesOneObjectIsFilled < timesMask);
    uintptr_t times = m_timesObjectsBeganToFill[placeOfTimes(object)] ^ std::bit_cast<uintptr_t>(object);
    return times > timesMask || times < maxTimesOneObjectIsFilled;
}

void FirstAdditions::didBeginToFill(VM& vm, JSObject* object, Structure* structureBefore)
{
    uintptr_t& times = m_timesObjectsBeganToFill[placeOfTimes(object)];
    times = (times ^ std::bit_cast<uintptr_t>(object)) > timesMask ? std::bit_cast<uintptr_t>(object) | 1 : times + 1;
    finishFilling(vm, std::exchange(m_objectsBeingFilled[m_numberOfObjectsFilled++ % numberOfObjectsBeingFilled], ObjectBeingFilled { object, structureBefore->id() }));
    vm.aotState().hasObjectsBeingFilled = true;
}

void FirstAdditions::finishFilling(VM& vm)
{
    for (ObjectBeingFilled& filled : m_objectsBeingFilled)
        finishFilling(vm, std::exchange(filled, ObjectBeingFilled { }));
}

size_t FirstAdditions::removeDeadEntries(VM& vm, CollectionScope)
{
    m_timesObjectsBeganToFill.fill(0);
    size_t numberOfDeadObjects = 0;
    for (ObjectBeingFilled& filled : m_objectsBeingFilled) {
        if (filled.object && !vm.heap.isMarked(filled.object)) {
            filled = { };
            ++numberOfDeadObjects;
        }
        if (filled.sharedStructure && !vm.heap.isMarked(filled.sharedStructure.decode()))
            filled.sharedStructure = { };
    }
    return numberOfDeadObjects;
}

static void askAgainWhatSlotAnswers(Instance& instance, JSCell* base, UniquedStringImpl* name, const Slot& slot)
{
    constexpr ASCIILiteral table = "the slot table"_s;
    switch (slot.kind) {
    case Slot::Kind::Value: {
        JSCell* holder = slot.has(Slot::isOwn) ? base : slot.holder;
        if (holder && holder->isObject())
            stopAtStaleAnswer(instance, base, name, KindOfAnswer::Value, valueAtOffset(asObject(holder), Slot::offsetFor(slot.wordIndex())), table);
        return;
    }
    case Slot::Kind::Getter:
    case Slot::Kind::ExtraGetter:
        stopAtStaleAnswer(instance, base, name, KindOfAnswer::Accessor, { }, table);
        return;
    case Slot::Kind::Absent:
        stopAtStaleAnswer(instance, base, name, KindOfAnswer::Absent, { }, table);
        return;
    default:
        if (Slot::presenceOf(slot.kind) == SlotPresence::Present)
            stopAtStaleAnswer(instance, base, name, KindOfAnswer::Present, { }, table);
        return;
    }
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
    if (access == SlotAccess::Read && m_vm.aotState().verifiesFastPathAnswers) [[unlikely]]
        askAgainWhatSlotAnswers(m_state.instance, base, name.uid, *slot);
    if (!belongsToOneObject(structure) && RowDisplacementTable::hasColumn(identifierNumber) && !m_tables.front().find(id, identifierNumber, access))
        m_tables.set(structure, name, access, *slot);
    if (!slot->has(Slot::dependsOnChain)) {
        m_siteCaches.fill(site, *slot, name.uid, { });
        return;
    }
    JSGlobalObject* globalObject = m_state.instance.globalObject;
    if (JSGlobalObject* realm = structure->realm(); realm && Instance::of(realm) != &m_state.instance) {
        m_siteCaches.didFailToFill(site);
        return;
    }
    ChainWalkUse use = access == SlotAccess::Read ? ChainWalkUse::KeptForReadsThatAskObjects : ChainWalkUse::KeptForStores;
    ChainWalk walk = structure->typeInfo().type() == GlobalProxyType
        ? walkChainThroughGlobalProxy(globalObject, *slot, name.uid)
        : walkChain({ globalObject, structure, base->isObject() ? asObject(base) : nullptr, ChainWalkMode::AllocatesNothing, use }, name.uid);
    if (walk.result == ChainWalkResult::Unknown) {
        m_siteCaches.didFailToFill(site);
        return;
    }
    ensureChangesAreReported(walk.changeableObjects.span());
    m_siteCaches.fill(site, *slot, name.uid, walk.changeableObjects.span());
}

void PropertyCaches::fillStoreSiteCache(SiteCache site, Structure* structure, UniquedStringImpl* uid, PropertyOffset offset)
{
    structure->didCachePropertyReplacement(m_vm, offset);
    m_siteCaches.fill(site, Slot::createInStorage(structure->id(), Slot::Kind::Value, Slot::isOwn, nullptr, offset), uid, { });
}

static_assert(!((1u << SiteCaches::regionShift) % sizeof(Slot)));
static_assert(std::is_same_v<decltype(SiteCacheState::missesLeft), uint16_t>, "the stub's countSiteCacheMiss wraps from 0 to 65535");

static std::span<uint8_t> allocateCardsOfSiteCaches(State& state, size_t sizeOfSlots)
{
    size_t numberOfCards = RememberedSlotSet::numberOfCardsFor(sizeOfSlots, SiteCaches::regionShift);
    return { state.tableArena.allocate<uint8_t>(numberOfCards), numberOfCards };
}

SiteCaches::SiteCaches(State& state, const CustomAccessorTable& customAccessors)
    : m_state(state)
    , m_customAccessors(customAccessors)
    , m_slots(state.instance.siteCaches())
    , m_states(state.tableArena.allocate<SiteCacheState>(SiteCacheState::numberOfStates))
    , m_refillCounts(state.tableArena.allocate<uint8_t>(SiteCacheState::numberOfStates))
    , m_sitesThatMissed(state.tableArena.allocate<uint64_t>((std::min<size_t>(m_slots.size(), numberOfMissBits) + 63) / 64))
    , m_ownerID(state.chainDependents->addOwner(*this))
    , m_writtenRegions(asByteSpan(m_slots), regionShift, allocateCardsOfSiteCaches(state, m_slots.size_bytes()))
{
    state.instance.propertyCaches.siteCacheStates = m_states;
    state.caches->add(CacheKind::WeakByKey, "SiteCaches"_s, *this);
    state.caches->add(CacheKind::StrongRoots, "Structures that sites name"_s, *this);
}

bool SiteCaches::didMissBefore(SiteCache site)
{
    uint32_t missBit = indexOf(site) % numberOfMissBits;
    uint64_t& word = m_sitesThatMissed[missBit / 64];
    uint64_t bit = 1ull << (missBit % 64);
    return std::exchange(word, word | bit) & bit;
}

bool SiteCaches::mayBeFilledAfterMiss(SiteCache site)
{
    if (!didMissBefore(site))
        return false;
    uint16_t& missesLeft = stateOf(site).missesLeft;
    return !missesLeft || !missesLeft--;
}

void SiteCaches::waitLongerForNextFill(const void* site)
{
    uint8_t& refillCount = refillCountOf(site);
    stateOf(site).missesLeft = SiteCacheState::missesBeforeRefill(refillCount);
    if (refillCount < SiteCacheState::maxRefillCount)
        ++refillCount;
}

unsigned SiteCaches::takeSamplesToLeaveOutBeforeNextFill(const void* site)
{
    uint8_t& refillCount = refillCountOf(site);
    unsigned samples = SiteCacheState::missesBeforeRefill(refillCount) / SiteCacheState::missesOfEmptySitesPerSample;
    if (refillCount < SiteCacheState::maxRefillCount)
        ++refillCount;
    return samples;
}

bool SiteCaches::didWaitForSamples(const void* site)
{
    uint16_t& missesLeft = stateOf(site).missesLeft;
    if (missesLeft < SiteCacheState::missesOfEmptySitesPerSample)
        return true;
    missesLeft -= SiteCacheState::missesOfEmptySitesPerSample;
    return false;
}

void SiteCaches::forgetHowOftenItWasFilled(const void* site)
{
    refillCountOf(site) = 0;
    stateOf(site).missesLeft = 0;
}

void SiteCaches::didFailToFill(SiteCache site)
{
    if (site->get().isEmpty()) {
        site->set(m_writtenRegions, Slot::createWithoutPayload(StructureID(), Slot::Kind::Uncacheable, 0));
        m_wasWrittenSinceLastSweep = true;
    }
    waitLongerForNextFill(site);
}

static void countIfRefill(State& state, SiteCache site)
{
    if (!site->get().isEmpty() && site->get().structureID)
        countEvent(*state.instance.vm, RuntimeCounterID::SiteCacheRefills);
}

void SiteCaches::fill(SiteCache site, const Slot& slot, UniquedStringImpl* uid, std::span<JSObject* const> changeableObjects)
{
    RELEASE_ASSERT(slot.has(Slot::dependsOnChain) || changeableObjects.empty());
    countIfRefill(m_state, site);
    site->set(m_writtenRegions, slot);
    m_wasWrittenSinceLastSweep = true;
    waitLongerForNextFill(site);
    if (slot.has(Slot::dependsOnChain))
        m_state.chainDependents->add(changeableObjects, { uid, indexOf(site), m_ownerID });
}

void SiteCaches::clear(SiteCache site)
{
    site->clear();
    forgetHowOftenItWasFilled(site);
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
    unsigned attributes = 0;
    PropertyOffset offset = created.prototype->structure()->get(vm, uid, attributes);
    if (!isValidOffset(offset))
        return true;
    Slot slot = m_slots[siteIndex].get();
    if (!slot.has(Slot::dependsOnChain))
        return false;
    std::optional<Slot> kept = slotBehindCreatedMember(slot, created, uid, offset, attributes);
    if (!kept)
        return false;
    if (kept->headerBits() != slot.headerBits() || kept->payloadBits() != slot.payloadBits()) {
        m_slots[siteIndex].set(m_writtenRegions, *kept);
        m_wasWrittenSinceLastSweep = true;
    }
    return true;
}

bool SiteCaches::hasChainDependent(VM&, uint32_t siteIndex, UniquedStringImpl*) const
{
    return m_slots[siteIndex].get().has(Slot::dependsOnChain);
}

static void forEachStructureNamedBySlotOfSite(const Slot& slot, const Invocable<void(StructureID)> auto& functor)
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
        if (belongsToOneObject(structure))
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
            forEachStructureNamedBySlotOfSite(site.get(), [&](StructureID id) {
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
            site.clear();
            ++numberOfClearedSites;
        });
    });
    if (isFullCollection)
        decommitEmptyPages(asWritableBytes(m_slots), m_writtenRegions);
    return numberOfClearedSites;
}

static_assert(sizeof(SiteAnswers) == InstanceLayout::sizeOfSiteAnswers && sizeof(uint16_t) == InstanceLayout::sizeOfIndexOfSiteAnswers);
static_assert(!(InstanceLayout::bytesReservedForSiteAnswers % sizeof(WideSiteAnswers)));

AnswersAtSites::AnswersAtSites(State& state, SiteCaches& states)
    : m_state(state)
    , m_states(states)
    , m_indices(state.instance.indicesOfSiteAnswers())
    , m_answers(state.instance.siteAnswers())
    , m_endOfWideAnswers(reinterpret_cast<WideSiteAnswers*>(state.instance.siteAnswers().data() + state.instance.siteAnswers().size()))
    , m_placesInPageShift(WTF::fastLog2(static_cast<unsigned>(pageSize() / sizeof(uint16_t))))
    , m_requestsOfPages(state.tableArena.allocate<uint8_t>((m_indices.size() >> m_placesInPageShift) + 1))
    , m_ownerID(state.chainDependents->addOwner(*this))
{
    m_places.append(UINT32_MAX);
    m_places.append(UINT32_MAX);
    m_answers[numberOfAnswersThatHoldNothing] = SiteAnswers::forPlacesWithout();
    state.caches->add(CacheKind::WeakByKey, "AnswersAtSites"_s, *this);
}

uint32_t AnswersAtSites::placeAtDistanceFromInstance(uint32_t distance) const
{
    size_t offset = distance - InstanceLayout::offsetOfIndicesOfSiteAnswers(m_state.instance.tables().instanceSizes);
    RELEASE_ASSERT(!(offset % sizeof(uint16_t)) && offset / sizeof(uint16_t) < m_indices.size());
    return static_cast<uint32_t>(offset / sizeof(uint16_t));
}

uint8_t* AnswersAtSites::missesAndSamplesLeftOutAt(uint32_t place)
{
    uint8_t* instance = reinterpret_cast<uint8_t*>(&m_state.instance);
    size_t distance = reinterpret_cast<uint8_t*>(&m_indices[place]) - instance;
    return instance + InstanceLayout::offsetOfMissesOfPlaces + distance % InstanceLayout::bytesReservedForMissesOfPlaces;
}

void AnswersAtSites::beginPeriodOfAnotherLength(uint32_t place)
{
    static constexpr std::array<uint8_t, 8> missesLeftOut { 0, 5, 2, 7, 1, 4, 6, 3 };
    uint8_t& misses = *missesAndSamplesLeftOutAt(place);
    uint8_t turn = (misses + 1) & 7;
    misses = static_cast<uint8_t>(SiteCacheState::sampleStep * missesLeftOut[turn] | turn);
    samplesLeftOutAt(place) = 0;
}

void AnswersAtSites::waitLongerForNextFill(uint32_t place)
{
    static_assert(SiteCacheState::missesBeforeRefill(SiteCacheState::maxRefillCount) / SiteCacheState::missesOfEmptySitesPerSample <= UINT8_MAX);
    samplesLeftOutAt(place) = static_cast<uint8_t>(m_states.takeSamplesToLeaveOutBeforeNextFill(keyOfState(place)));
}

bool AnswersAtSites::isDueForFilling(uint32_t place)
{
    if (answersOf(place))
        return true;
    uint8_t& requests = m_requestsOfPages[place >> m_placesInPageShift];
    if (requests >= requestsThatOpenPage)
        return true;
    ++requests;
    return false;
}

SiteAnswers* AnswersAtSites::ensureAnswersOf(uint32_t place, SiteUse use)
{
    if (SiteAnswers* answers = answersOf(place))
        return answers;
    uint32_t number;
    if (!m_numbersGivenBack.isEmpty())
        number = m_numbersGivenBack.takeLast();
    else if ((m_places.size() + 1) * wordsInAnswers <= m_state.instance.propertyCaches.firstWordOfWideAnswers) {
        number = m_places.size();
        m_places.append(UINT32_MAX);
    } else
        return nullptr;
    ASSERT(m_answers[number].isEmpty());
    countEvent(*m_state.instance.vm, RuntimeCounterID::SiteCacheEntriesAllocated);
    m_answers[number].first = SiteAnswers::withoutKey(use, true);
    m_places[number] = place;
    m_indices[place] = static_cast<uint16_t>(number * wordsInAnswers);
    return &m_answers[number];
}

void AnswersAtSites::giveBackIfEmpty(uint32_t number)
{
    SiteAnswers& answers = m_answers[number];
    if (!answers.isEmpty() || m_places[number] == UINT32_MAX)
        return;
    answers = { };
    m_indices[m_places[number]] = 0;
    m_places[number] = UINT32_MAX;
    m_numbersGivenBack.append(static_cast<uint16_t>(number));
}

WideSiteAnswers* AnswersAtSites::wideAnswersOf(uint32_t place) const
{
    uint32_t word = m_indices[place];
    if (word < m_state.instance.propertyCaches.firstWordOfWideAnswers)
        return nullptr;
    return &wideAnswersAt(static_cast<uint32_t>((m_answers.size() * wordsInAnswers - word) / wordsInWideAnswers - 1));
}

WideSiteAnswers* AnswersAtSites::ensureWideAnswersOf(uint32_t place)
{
    if (WideSiteAnswers* wide = wideAnswersOf(place))
        return wide;
    if (m_placesWithoutFurtherAnswers.contains(place))
        return nullptr;
    uint32_t number;
    if (!m_numbersOfWideAnswersGivenBack.isEmpty())
        number = m_numbersOfWideAnswersGivenBack.takeLast();
    else if (m_places.size() * wordsInAnswers + (m_placesOfWideAnswers.size() + 1) * wordsInWideAnswers <= m_answers.size() * wordsInAnswers) {
        number = m_placesOfWideAnswers.size();
        m_placesOfWideAnswers.append(UINT32_MAX);
        m_state.instance.propertyCaches.firstWordOfWideAnswers = firstWordOfWideAnswersAt(number);
    } else
        return nullptr;
    WideSiteAnswers& wide = wideAnswersAt(number);
    ASSERT(wide.namesNoStructure() && !wide.answers.first.key && !wide.answers.second.key);
    if (SiteAnswers* narrow = answersOf(place)) {
        uint32_t numberOfNarrow = static_cast<uint32_t>(narrow - m_answers.data());
        wide.answers = std::exchange(*narrow, { });
        m_places[numberOfNarrow] = UINT32_MAX;
        m_numbersGivenBack.append(static_cast<uint16_t>(numberOfNarrow));
    } else
        wide.answers.first = SiteAnswers::withoutKey(SiteUse::Read, true);
    m_placesOfWideAnswers[number] = place;
    m_indices[place] = static_cast<uint16_t>(firstWordOfWideAnswersAt(number));
    return &wide;
}

void AnswersAtSites::giveBackWideAnswersIfEmpty(uint32_t number)
{
    WideSiteAnswers& wide = wideAnswersAt(number);
    if (!wide.answers.isEmpty() || !wide.namesNoStructure() || m_placesOfWideAnswers[number] == UINT32_MAX)
        return;
    wide = { };
    m_indices[m_placesOfWideAnswers[number]] = 0;
    m_placesOfWideAnswers[number] = UINT32_MAX;
    m_numbersOfWideAnswersGivenBack.append(static_cast<uint16_t>(number));
}

bool AnswersAtSites::tryDropWideAnswers(uint32_t place, WideSiteAnswers& wide)
{
    if (wide.structureThatLeavesFieldOut)
        return false;
    uint32_t number;
    if (!m_numbersGivenBack.isEmpty())
        number = m_numbersGivenBack.takeLast();
    else if ((m_places.size() + 1) * wordsInAnswers <= m_state.instance.propertyCaches.firstWordOfWideAnswers) {
        number = m_places.size();
        m_places.append(UINT32_MAX);
    } else
        return false;
    uint32_t numberOfWide = numberOf(wide);
    m_answers[number] = wide.answers;
    m_places[number] = place;
    m_indices[place] = static_cast<uint16_t>(number * wordsInAnswers);
    wide = { };
    m_placesOfWideAnswers[numberOfWide] = UINT32_MAX;
    m_numbersOfWideAnswersGivenBack.append(static_cast<uint16_t>(numberOfWide));
    m_placesWithoutFurtherAnswers.add(place);
    countEvent(*m_state.instance.vm, RuntimeCounterID::SitesWithoutFurtherAnswers);
    return true;
}

bool AnswersAtSites::beginTrialUnlessTried(uint32_t place, WideSiteAnswers& wide)
{
    if (!m_states.hasReachedLongestWait(keyOfState(place)))
        return false;
    uint8_t& timesWithoutRoom = m_timesWithoutRoom.add(place, 0).iterator->value;
    if (timesWithoutRoom == TrialOfFurtherAnswers::overflowsBeforeTrial || ++timesWithoutRoom != TrialOfFurtherAnswers::overflowsBeforeTrial)
        return false;
    m_trials.set(place, TrialOfFurtherAnswers { wide });
    wide.structuresOfFurtherAnswers.fill(0);
    wide.structuresThatLackName.fill(0);
    samplesLeftOutAt(place) = 0;
    return true;
}

bool AnswersAtSites::noteSampleOfTrial(uint32_t place, StructureID id)
{
    auto found = m_trials.isEmpty() ? m_trials.end() : m_trials.find(place);
    if (found == m_trials.end())
        return false;
    TrialOfFurtherAnswers& trial = found->value;
    trial.samplesServed += trial.serves(id);
    if (++trial.samples < TrialOfFurtherAnswers::numberOfSamples) {
        samplesLeftOutAt(place) = 0;
        return true;
    }
    bool servesMany = trial.samplesServed * 5 >= TrialOfFurtherAnswers::numberOfSamples * 2;
    if (WideSiteAnswers* wide = wideAnswersOf(place); wide && (servesMany || !tryDropWideAnswers(place, *wide))) {
        wide->structuresOfFurtherAnswers = trial.hidden.structuresOfFurtherAnswers;
        wide->operandsOfFurtherAnswers = trial.hidden.operandsOfFurtherAnswers;
        wide->formsOfFurtherAnswersHeld = trial.hidden.formsOfFurtherAnswersHeld;
        wide->structuresThatLackName = trial.hidden.structuresThatLackName;
        m_wasWrittenSinceLastSweep = true;
    }
    m_trials.remove(found);
    waitLongerForNextFill(place);
    return true;
}

void AnswersAtSites::didFailToFill(uint32_t place)
{
    waitLongerForNextFill(place);
}

static bool isRoad(SiteUse use, SiteAnswers::Answer answer)
{
    return isRead(use) && answer.isByStructure() && !answer.operand && SiteAnswers::formOf(use, answer.key) == SiteAnswers::Form::Nothing;
}

static bool borrowsOperandOfOtherAnswer(SiteUse use, SiteAnswers::Answer answer)
{
    if (!answer.isByStructure())
        return false;
    SiteAnswers::Form form = SiteAnswers::formOf(use, answer.key);
    return form == SiteAnswers::Form::ValueInStorageOfHolder || form == SiteAnswers::Form::GetterInStorageOfHolder;
}

static bool isValueInStorageOfHolderAtCall(SiteAnswers::Answer answer)
{
    return answer.isByStructure() && SiteAnswers::formOf(SiteUse::Call, answer.key) == SiteAnswers::Form::ValueInStorageOfHolder;
}

static SiteAnswers::Answer asNewestAtRead(SiteUse use, SiteAnswers::Answer answer)
{
    static_assert(Slot::wordIndexFor(0) > 0 && Slot::wordIndexFor(firstOutOfLineOffset) < 0);
    if (!answer.isByStructure() || SiteAnswers::formOf(use, answer.key) != SiteAnswers::Form::OwnValueInCell)
        return answer;
    uint32_t key = (answer.key & ~SiteAnswers::formMask) | SiteAnswers::bitsOf(use, SiteAnswers::mostCommonForm(use));
    return { key, use == SiteUse::Read ? static_cast<uint32_t>(answer.operand / sizeof(EncodedJSValue)) : answer.operand };
}

static bool secondRepeatsFirst(SiteUse use, const SiteAnswers& answers)
{
    using Form = SiteAnswers::Form;
    if (!answers.holdsShape() || !answers.second.isByStructure() || answers.second.operand != answers.first.operand)
        return false;
    Form form = SiteAnswers::formOf(use, answers.second.key);
    uint32_t bitTurned = form == Form::OwnNarrowReferenceInCell ? SiteAnswers::bitTurnedForNarrowReferenceInCell : form == Form::OwnInt32InCell ? SiteAnswers::bitTurnedForInt32InCell : form == Form::OwnFieldInExtension ? SiteAnswers::bitTurnedForFieldInExtension : 0;
    if (!bitTurned && form != Form::OwnFieldInCell)
        return false;
    const CompiledStructure* compiled = compiledObjectStructureOrNull(WTF::opaque(answers.second.structureID().decode()));
    return compiled && answers.first.key == (shapeWordOf(compiled->layout().shape) ^ bitTurned);
}

void AnswersAtSites::fill(uint32_t place, SiteUse use, const AnswerByStructure& filled, UniquedStringImpl* uid, std::span<JSObject* const> changeableObjects, bool stubFollowsFurtherAnswers)
{
    RELEASE_ASSERT(filled.answer.isByStructure());
    if (isRead(use) && holdsAnotherStructure(place, use, filled.answer.structureID())) {
        SiteAnswers::Form form = SiteAnswers::formOf(use, filled.answer.key);
        bool comesBack = stubFollowsFurtherAnswers && wasPutOut(place, filled.answer.structureID());
        if (std::ranges::contains(WideSiteAnswers::formsOfFurtherAnswers, form) || (comesBack && WideSiteAnswers::canBeFurtherAnswer(form, filled.indexInStorageOfHolder))) {
            addFurtherAnswer(place, filled, uid, changeableObjects);
            return;
        }
        if (stubFollowsFurtherAnswers)
            m_visitorsPutOut.add(static_cast<uint64_t>(place) << 32 | answersOf(place)->second.structureID().bits());
    }
    SiteAnswers* answers = ensureAnswersOf(place, use);
    if (!answers) {
        didFailToFill(place);
        return;
    }
    waitLongerForNextFill(place);
    SiteAnswers::Answer& newest = answers->newestByStructure(use);
    SiteAnswers::Answer& other = answers->besideNewestByStructure(use);
    const bool otherIsFirst = &other == &answers->first;
    const bool otherReadsOperandOfNewest = use == SiteUse::Call && isValueInStorageOfHolderAtCall(other);
    if (use == SiteUse::Call && isValueInStorageOfHolderAtCall(filled.answer) && (otherReadsOperandOfNewest || isValueInStorageOfHolderAtCall(newest)) && newest.operand == filled.answer.operand && other.operand == static_cast<uint32_t>(filled.indexInStorageOfHolder)) {
        if (newest.key && newest.key != filled.answer.key)
            other.key = newest.key;
        newest.key = filled.answer.key;
        m_wasWrittenSinceLastSweep = true;
        if (!changeableObjects.empty())
            m_state.chainDependents->add(changeableObjects, { uid, place, m_ownerID });
        return;
    }
    if (otherReadsOperandOfNewest)
        other = SiteAnswers::withoutKey(use, otherIsFirst);
    bool didBorrow = borrowsOperandOfOtherAnswer(use, newest);
    if (didBorrow)
        newest = { };
    if (borrowsOperandOfOtherAnswer(use, filled.answer))
        other = SiteAnswers::withoutKey(use, otherIsFirst, static_cast<uint32_t>(filled.indexInStorageOfHolder));
    else if (!isRead(use)) {
        if (newest.key)
            other = newest;
    } else if (didBorrow)
        other = SiteAnswers::withoutKey(use, otherIsFirst);
    newest = isRead(use) ? asNewestAtRead(use, filled.answer) : filled.answer;
    if (isRoad(use, filled.answer))
        countEvent(*m_state.instance.vm, RuntimeCounterID::SiteCacheRoutingEntriesStored);
    ASSERT(use != SiteUse::Call || !answers->second.isByStructure() || SiteAnswers::formOf(use, answers->second.key) != SiteAnswers::Form::GetterInStorageOfHolder);
    m_wasWrittenSinceLastSweep = true;
    if (!changeableObjects.empty())
        m_state.chainDependents->add(changeableObjects, { uid, place, m_ownerID });
}

void AnswersAtSites::fillByShape(uint32_t place, SiteUse use, SiteAnswers::Answer answer, std::optional<SiteAnswers::Answer> sameByStructure)
{
    RELEASE_ASSERT(answer.isByShape());
    SiteAnswers* answers = ensureAnswersOf(place, use);
    if (!answers) {
        didFailToFill(place);
        return;
    }
    waitLongerForNextFill(place);
    if (borrowsOperandOfOtherAnswer(use, answers->second))
        answers->second = { };
    answers->first = answer;
    if (sameByStructure && (!answers->second.key || isRoad(use, answers->second))) {
        answers->second = *sameByStructure;
        m_wasWrittenSinceLastSweep = true;
    }
}

bool AnswersAtSites::holdsAnotherStructure(uint32_t place, SiteUse use, StructureID id) const
{
    const SiteAnswers* answers = answersOf(place);
    return answers && answers->second.isByStructure() && answers->second.structureID() != id && !secondRepeatsFirst(use, *answers) && !isRoad(use, answers->second);
}

void AnswersAtSites::moveUpFurtherAnswer(uint32_t place, SiteUse use, StructureID id)
{
    using Form = SiteAnswers::Form;
    WideSiteAnswers* wide = wideAnswersOf(place);
    if (!wide)
        return;
    if (uint8_t& leftOut = samplesLeftOutAt(place)) {
        --leftOut;
        return;
    }
    auto found = std::ranges::find(wide->structuresOfFurtherAnswers, id.bits());
    if (found == wide->structuresOfFurtherAnswers.end())
        return;
    size_t index = found - wide->structuresOfFurtherAnswers.begin();
    SiteAnswers::Answer& first = wide->answers.first;
    SiteAnswers::Answer& second = wide->answers.second;
    const WideSiteAnswers::FormOfFurtherAnswer held = wide->formsOfFurtherAnswersHeld[index];
    const AnswerByStructure up { { id.bits() | held.bits, wide->operandsOfFurtherAnswers[index] }, held.indexInStorageOfHolder };
    const bool upBorrows = borrowsOperandOfOtherAnswer(use, up.answer);
    if (upBorrows && wide->answers.holdsShape())
        return;
    AnswerByStructure down { isRoad(use, second) ? SiteAnswers::Answer { } : second, 0 };
    if (down.answer.isByStructure()) {
        if (borrowsOperandOfOtherAnswer(use, second))
            down.indexInStorageOfHolder = static_cast<int32_t>(first.operand);
        else if (SiteAnswers::formOf(use, second.key) == Form::OwnValueInStorage && static_cast<int32_t>(second.operand) > 0)
            down.answer = { (second.key & ~SiteAnswers::formMask) | SiteAnswers::bitsOf(use, Form::OwnValueInCell), static_cast<uint32_t>(second.operand * sizeof(EncodedJSValue)) };
        if (!WideSiteAnswers::canBeFurtherAnswer(SiteAnswers::formOf(use, down.answer.key), down.indexInStorageOfHolder))
            return;
    }
    waitLongerForNextFill(place);
    if (upBorrows)
        first = SiteAnswers::withoutKey(use, true, static_cast<uint32_t>(up.indexInStorageOfHolder));
    else if (down.indexInStorageOfHolder)
        first = SiteAnswers::withoutKey(use, true);
    second = asNewestAtRead(use, up.answer);
    *found = down.answer.isByStructure() ? down.answer.structureID().bits() : 0;
    wide->operandsOfFurtherAnswers[index] = down.answer.operand;
    wide->formsOfFurtherAnswersHeld[index] = { static_cast<uint8_t>(down.answer.key & SiteAnswers::formMask), static_cast<int8_t>(down.indexInStorageOfHolder) };
    m_wasWrittenSinceLastSweep = true;
}

void AnswersAtSites::addFurtherAnswer(uint32_t place, const AnswerByStructure& filled, UniquedStringImpl* uid, std::span<JSObject* const> changeableObjects)
{
    const SiteAnswers::Answer answer = filled.answer;
    if (noteSampleOfTrial(place, answer.structureID()))
        return;
    WideSiteAnswers* wide = ensureWideAnswersOf(place);
    if (!wide) {
        waitLongerForNextFill(place);
        return;
    }
    auto& structures = wide->structuresOfFurtherAnswers;
    if (std::ranges::contains(structures, answer.structureID().bits())) {
        waitLongerForNextFill(place);
        return;
    }
    auto room = std::ranges::find(structures, 0u);
    if (room == structures.end()) {
        if (beginTrialUnlessTried(place, *wide))
            return;
        room = structures.begin() + m_nextStructureToForget++ % structures.size();
        waitLongerForNextFill(place);
    }
    size_t index = room - structures.begin();
    *room = answer.structureID().bits();
    wide->operandsOfFurtherAnswers[index] = answer.operand;
    wide->formsOfFurtherAnswersHeld[index] = { static_cast<uint8_t>(answer.key & SiteAnswers::formMask), static_cast<int8_t>(filled.indexInStorageOfHolder) };
    m_wasWrittenSinceLastSweep = true;
    if (!changeableObjects.empty())
        m_state.chainDependents->add(changeableObjects, { uid, place, m_ownerID });
}

void AnswersAtSites::addStructureThatLacksName(uint32_t place, StructureID id, UniquedStringImpl* uid, std::span<JSObject* const> changeableObjects, bool mayGiveUp)
{
    if (noteSampleOfTrial(place, id))
        return;
    WideSiteAnswers* wide = ensureWideAnswersOf(place);
    if (!wide) {
        waitLongerForNextFill(place);
        return;
    }
    auto& structures = wide->structuresThatLackName;
    if (std::ranges::contains(structures, id.bits())) {
        waitLongerForNextFill(place);
        return;
    }
    auto room = std::ranges::find(structures, 0u);
    if (room == structures.end()) {
        if (mayGiveUp && beginTrialUnlessTried(place, *wide))
            return;
        room = structures.begin() + m_nextStructureToForget++ % structures.size();
        waitLongerForNextFill(place);
    }
    *room = id.bits();
    m_wasWrittenSinceLastSweep = true;
    if (!changeableObjects.empty())
        m_state.chainDependents->add(changeableObjects, { uid, place, m_ownerID });
}

bool AnswersAtSites::findsSlotOf(uint32_t place, SiteUse use, const Object& object) const
{
    const SiteAnswers* answers = answersOf(place);
    if (!answers)
        return false;
    if (answers->holdsShape() && !((answers->first.key ^ object.shapeWordWithMark()) & ~SiteAnswers::bitsTurnedForAnswersByShape))
        return true;
    auto isFieldOfObject = [](SiteAnswers::Form form) {
        return form == SiteAnswers::Form::OwnFieldInCell || form == SiteAnswers::Form::OwnNarrowReferenceInCell || form == SiteAnswers::Form::OwnInt32InCell || form == SiteAnswers::Form::OwnFieldInExtension;
    };
    if (const WideSiteAnswers* wide = wideAnswersOf(place)) {
        auto found = std::ranges::find(wide->structuresOfFurtherAnswers, object.structureID().bits());
        if (found != wide->structuresOfFurtherAnswers.end() && isFieldOfObject(SiteAnswers::formOf(use, wide->formsOfFurtherAnswersHeld[found - wide->structuresOfFurtherAnswers.begin()].bits)))
            return true;
    }
    if (!answers->second.isByStructure() || answers->second.structureID() != object.structureID())
        return false;
    return isFieldOfObject(SiteAnswers::formOf(use, answers->second.key));
}

void AnswersAtSites::fillAboveFieldLeftOut(uint32_t place, StructureID id, WhatStandsAbove above, UniquedStringImpl* uid, std::span<JSObject* const> changeableObjects)
{
    waitLongerForNextFill(place);
    WideSiteAnswers* wide = ensureWideAnswersOf(place);
    if (!wide)
        return;
    wide->structureThatLeavesFieldOut = id.bits();
    wide->holderOrWordAbove = above.holderOrWord;
    wide->indexInStorageOfHolderAbove = above.indexInStorageOfHolder;
    m_wasWrittenSinceLastSweep = true;
    if (!changeableObjects.empty())
        m_state.chainDependents->add(changeableObjects, { uid, place, m_ownerID });
}

void AnswersAtSites::dropChainDependent(VM&, uint32_t place, UniquedStringImpl*)
{
    SiteAnswers* answers = answersOf(place);
    if (!answers)
        return;
    for (SiteAnswers::Answer* answer : { &answers->first, &answers->second }) {
        if (!answer->isByShape())
            *answer = { };
    }
    m_trials.remove(place);
    m_timesWithoutRoom.remove(place);
    m_placesWithoutFurtherAnswers.remove(place);
    if (WideSiteAnswers* wide = wideAnswersOf(place)) {
        wide->forgetStructures();
        giveBackWideAnswersIfEmpty(numberOf(*wide));
    } else
        giveBackIfEmpty(static_cast<uint32_t>(answers - m_answers.data()));
    m_states.forgetHowOftenItWasFilled(keyOfState(place));
    samplesLeftOutAt(place) = 0;
}

bool AnswersAtSites::hasChainDependent(VM&, uint32_t place, UniquedStringImpl*) const
{
    const SiteAnswers* answers = answersOf(place);
    if (!answers)
        return false;
    const WideSiteAnswers* wide = wideAnswersOf(place);
    return answers->first.isByStructure() || answers->second.isByStructure() || (wide && !wide->namesNoStructure()) || m_trials.contains(place);
}

size_t AnswersAtSites::removeDeadEntries(VM& vm, CollectionScope scope)
{
    if (scope == CollectionScope::Full) {
        m_visitorsPutOut.clear();
        m_timesWithoutRoom.clear();
        m_placesWithoutFurtherAnswers.clear();
    }
    for (auto& trial : m_trials)
        m_timesWithoutRoom.remove(trial.key);
    m_trials.clear();
    if (scope == CollectionScope::Eden && !m_wasWrittenSinceLastSweep)
        return 0;
    m_wasWrittenSinceLastSweep = false;

    size_t numberOfClearedAnswers = 0;
    for (uint32_t number = 1; number < m_places.size(); ++number) {
        SiteAnswers& answers = m_answers[number];
        bool didClear = false;
        for (SiteAnswers::Answer* answer : { &answers.first, &answers.second }) {
            if (!answer->isByStructure() || vm.heap.isMarked(answer->structureID().decode()))
                continue;
            answer->key = 0;
            didClear = true;
            ++numberOfClearedAnswers;
        }
        if (didClear)
            giveBackIfEmpty(number);
    }
    auto isDead = [&](uint32_t bits) {
        return bits && !vm.heap.isMarked(std::bit_cast<StructureID>(bits).decode());
    };
    for (uint32_t number = 0; number < m_placesOfWideAnswers.size(); ++number) {
        WideSiteAnswers& wide = wideAnswersAt(number);
        size_t numberBefore = numberOfClearedAnswers;
        for (SiteAnswers::Answer* answer : { &wide.answers.first, &wide.answers.second }) {
            if (!answer->isByStructure() || vm.heap.isMarked(answer->structureID().decode()))
                continue;
            *answer = { };
            ++numberOfClearedAnswers;
        }
        for (std::span<uint32_t> structures : { std::span<uint32_t>(wide.structuresOfFurtherAnswers), std::span<uint32_t>(wide.structuresThatLackName) }) {
            for (uint32_t& bits : structures) {
                if (!isDead(bits))
                    continue;
                bits = 0;
                ++numberOfClearedAnswers;
            }
        }
        if (isDead(wide.structureThatLeavesFieldOut)) {
            wide.forgetWhatStandsAbove();
            ++numberOfClearedAnswers;
        }
        if (numberOfClearedAnswers != numberBefore)
            giveBackWideAnswersIfEmpty(number);
    }
    return numberOfClearedAnswers;
}

static std::optional<uint32_t> offsetOfPlainValueInCell(const FieldData& field)
{
    SlotRepresentation representation = field.slot.representation();
    if (field.isAccessor() || field.isOutOfLine() || (representation != SlotRepresentation::JSValue && representation != SlotRepresentation::Cell) || field.slot.offset % sizeof(EncodedJSValue))
        return std::nullopt;
    return field.slot.offset;
}

static bool isPlainValueInExtension(const Object& object, const FieldData& field)
{
    SlotRepresentation representation = field.slot.representation();
    if (field.isAccessor() || !field.isOutOfLine() || (representation != SlotRepresentation::JSValue && representation != SlotRepresentation::Cell))
        return false;
    return object.layout().extensionSlot() == SlotData::create(offsetOfFirstField, SlotRepresentation::Cell32);
}

static std::optional<SiteAnswers::Answer> answerForPlainValueInExtension(const Object& object, const FieldData& field)
{
    if (!isPlainValueInExtension(object, field))
        return std::nullopt;
    return SiteAnswers::tryByShapeInExtension(object.shapeWord(), field.slot.offset);
}

static std::optional<SiteAnswers::Answer> answerForTypedSlotInCell(const Object& object, const FieldData& field)
{
    if (field.isAccessor() || field.isOutOfLine())
        return std::nullopt;
    return SiteAnswers::tryByShapeInTypedSlot(object.shapeWord(), field.slot.representation(), field.slot.offset);
}

static std::optional<AnswersAtSites::AnswerByStructure> getterFieldAnswer(SiteUse use, const Slot& slot)
{
    if (slot.kind != Slot::fieldOfGetter || !isRead(use) || slot.has(Slot::isOutOfLine))
        return std::nullopt;
    SlotData data = slot.field.slot;
    bool holdsPlainValue = (data.representation() == SlotRepresentation::JSValue || data.representation() == SlotRepresentation::Cell) && !(data.offset % sizeof(EncodedJSValue));
    std::optional<SiteAnswers::Form> form = holdsPlainValue ? std::optional(SiteAnswers::Form::OwnFieldInCell) : SiteAnswers::formByStructureOfSlotInCell(data.representation());
    std::optional<SiteAnswers::Answer> answer = form ? SiteAnswers::tryByStructure(use, slot.structureID, *form, data.offset) : std::nullopt;
    return answer ? std::optional(AnswersAtSites::AnswerByStructure { *answer, 0 }) : std::nullopt;
}

static std::optional<AnswersAtSites::AnswerByStructure> answerOfSlot(SiteUse use, const Slot& slot, uintptr_t cageBase, bool usesWideReferences, JSObject* objectThatHoldsUndefined)
{
    using Form = SiteAnswers::Form;
    auto answer = [&](Form form, uint32_t operand, int32_t indexInStorageOfHolder = 0) -> std::optional<AnswersAtSites::AnswerByStructure> {
        std::optional<SiteAnswers::Answer> made = SiteAnswers::tryByStructure(use, slot.structureID, form, operand);
        if (!made)
            return std::nullopt;
        return AnswersAtSites::AnswerByStructure { *made, indexInStorageOfHolder };
    };
    switch (slot.kind) {
    case Slot::Kind::Value: {
        int32_t wordIndex = slot.wordIndex();
        if (slot.has(Slot::isOwn)) {
            if (slot.has(Slot::isOutOfLine))
                return answer(Form::OwnValueInStorage, static_cast<uint32_t>(wordIndex));
            return answer(isRead(use) ? Form::OwnValueInCell : Form::OwnFieldInCell, static_cast<uint32_t>(wordIndex) * sizeof(EncodedJSValue));
        }
        if (!slot.holder || usesWideReferences)
            return std::nullopt;
        uintptr_t wordsToHolder = (std::bit_cast<uintptr_t>(slot.holder) - cageBase) / sizeof(EncodedJSValue);
        if (wordsToHolder + static_cast<uint32_t>(std::max(wordIndex, 0)) > UINT32_MAX)
            return std::nullopt;
        if (slot.has(Slot::isOutOfLine))
            return answer(Form::ValueInStorageOfHolder, static_cast<uint32_t>(wordsToHolder), wordIndex);
        return answer(Form::ValueInWordOfCage, static_cast<uint32_t>(wordsToHolder) + static_cast<uint32_t>(wordIndex));
    }
    case Slot::Kind::Getter: {
        std::optional<WordOfGetterForSiteAnswers> word = findWordOfGetterForSiteAnswers(use, slot, cageBase, usesWideReferences);
        return word ? answer(word->form, word->operand, word->indexInStorageOfHolder) : std::nullopt;
    }
    case Slot::Kind::FieldGetter: {
        if ((use != SiteUse::Call && !isRead(use)) || slot.has(Slot::isOutOfLine))
            return std::nullopt;
        SlotRepresentation representation = slot.field.slot.representation();
        if ((representation != SlotRepresentation::JSValue && representation != SlotRepresentation::Cell) || slot.field.slot.offset % sizeof(EncodedJSValue))
            return std::nullopt;
        return answer(Form::OwnGetterInCell, slot.field.slot.offset);
    }
    case Slot::Kind::Absent: {
        if (!isRead(use) || usesWideReferences)
            return std::nullopt;
        uintptr_t words = (std::bit_cast<uintptr_t>(objectThatHoldsUndefined->inlineStorage()) - cageBase) / sizeof(EncodedJSValue);
        return words <= UINT32_MAX ? answer(Form::ValueInWordOfCage, static_cast<uint32_t>(words)) : std::nullopt;
    }
    case Slot::Kind::ArrayLength:
        return isRead(use) && slot.has(Slot::isLastIndexOfRegExp) ? answer(Form::OwnValueInCell, static_cast<uint32_t>(RegExpObject::offsetOfLastIndex())) : std::nullopt;
    case Slot::Kind::TypedArrayLength:
        return isRead(use) && !slot.has(Slot::readsByteOffset) ? answer(Form::LengthOfTypedArray, 0) : std::nullopt;
    case Slot::Kind::ExtraProperty:
    case Slot::Kind::ExtraGetter: {
        const bool isGetter = slot.kind == Slot::Kind::ExtraGetter;
        if (isGetter && use != SiteUse::Call)
            return std::nullopt;
        constexpr int32_t maxIndex = (1 << (31 - SiteAnswers::indexShiftInOperandOfExtraValue)) - 1;
        uint32_t hiddenWordOffset = slot.hiddenWordOffset();
        int32_t wordIndex = slot.extraProperty.wordIndex;
        if (usesWideReferences || hiddenWordOffset % sizeof(uint32_t) || hiddenWordOffset / sizeof(uint32_t) > UINT8_MAX || wordIndex > maxIndex || wordIndex < -maxIndex)
            return std::nullopt;
        uint32_t operand = static_cast<uint32_t>(wordIndex) << SiteAnswers::indexShiftInOperandOfExtraValue | hiddenWordOffset / sizeof(uint32_t);
        if (isGetter)
            return answer(slot.has(Slot::isOutOfLine) ? Form::ExtraGetterInStorageAtCall : Form::ExtraGetterInCellAtCall, operand);
        return answer(slot.has(Slot::isOutOfLine) ? Form::ExtraValueInStorage : Form::ExtraValueInCell, operand);
    }
    default:
        return std::nullopt;
    }
}

static std::optional<AnswersAtSites::WhatStandsAbove> findWhatStandsAboveFieldLeftOut(const Slot& slot, const ChainWalk& walk, uintptr_t cageBase, JSObject* objectThatHoldsUndefined)
{
    if (walk.firstObjectWithEmptySlot)
        return std::nullopt;
    switch (slot.kind) {
    case Slot::Kind::Field: {
        if (walk.result != ChainWalkResult::Absent || (!slot.has(Slot::absentIsUndefined) && !slot.has(Slot::emptySlotMayBeUndefined)))
            return std::nullopt;
        uintptr_t words = (std::bit_cast<uintptr_t>(objectThatHoldsUndefined->inlineStorage()) - cageBase) / sizeof(EncodedJSValue);
        return words <= UINT32_MAX ? std::optional(AnswersAtSites::WhatStandsAbove { static_cast<uint32_t>(words) }) : std::nullopt;
    }
    case Slot::Kind::FieldOrInherited: {
        JSCell* holder = MarkedBlockCage::decode(cageBase, slot.fieldAndHolder.holder);
        if (walk.result != ChainWalkResult::Found || walk.holder != holder || slot.has(Slot::inheritsGetter) || slot.has(Slot::emptySlotMayBeUndefined))
            return std::nullopt;
        int32_t wordIndex = slot.wordIndex();
        if (slot.has(Slot::inheritedIsOutOfLine))
            return wordIndex < 0 ? std::optional(AnswersAtSites::WhatStandsAbove { slot.fieldAndHolder.holder, wordIndex }) : std::nullopt;
        uint64_t words = static_cast<uint64_t>(slot.fieldAndHolder.holder) + static_cast<uint32_t>(wordIndex);
        return wordIndex > 0 && words <= UINT32_MAX ? std::optional(AnswersAtSites::WhatStandsAbove { static_cast<uint32_t>(words) }) : std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

bool PropertyCaches::structureSaysFieldIsAbsent(Structure& structure, const CachedName& name)
{
    const CompiledStructure& compiled = *compiledObjectStructureOrNull(&structure);
    std::optional<Slot> slot = compiled.listsEveryExtraProperty() ? m_tables.find(structure.id(), name, SlotAccess::Read) : std::nullopt;
    return slot && slot->kind == Slot::Kind::Field && slot->has(Slot::absentIsUndefined);
}

bool PropertyCaches::fillAboveFieldLeftOut(uint32_t place, SiteUse use, Object& object, const CachedName& name)
{
    Structure* structure = object.structure();
    if (m_state.instance.options().useWideReferences() || object.compiledStructure().flags().contains(CompiledStructure::Flag::HasOnlyFieldsBelowObjectPrototype) || !m_answersAtSites.findsSlotOf(place, use, object))
        return false;
    std::optional<Slot> slot = m_tables.find(structure->id(), name, SlotAccess::Read);
    if (!slot || belongsToOneObject(structure))
        return false;
    if (m_vm.aotState().verifiesFastPathAnswers) [[unlikely]]
        askAgainWhatSlotAnswers(m_state.instance, &object, name.uid, *slot);
    if (JSGlobalObject* realm = structure->realm(); realm && Instance::of(realm) != &m_state.instance)
        return false;
    ChainWalk walk = walkChain({ m_state.instance.globalObject, structure, &object, ChainWalkMode::AllocatesNothing, ChainWalkUse::KeptForReadsThatAskObjects }, name.uid);
    std::optional<AnswersAtSites::WhatStandsAbove> above = findWhatStandsAboveFieldLeftOut(*slot, walk, m_vm.heap.cage().base(), m_state.objectThatHoldsUndefined);
    if (!above)
        return false;
    ensureChangesAreReported(walk.changeableObjects.span());
    m_answersAtSites.fillAboveFieldLeftOut(place, structure->id(), *above, name.uid, walk.changeableObjects.span());
    return true;
}

ErrorsWithMessage::ErrorsWithMessage(State& state)
    : m_state(state)
{
    state.caches->add(CacheKind::WeakByKey, "errors with a message"_s, *this);
}

size_t ErrorsWithMessage::removeDeadEntries(VM& vm, CollectionScope)
{
    size_t numberCleared = 0;
    for (uint32_t& place : m_state.instance.propertyCaches.errorsWithMessage) {
        if (!place || vm.heap.isMarked(std::bit_cast<StructureID>(place).decode()))
            continue;
        place = 0;
        ++numberCleared;
    }
    return numberCleared;
}

void ErrorsWithMessage::keepIfRead(Structure* structure, UniquedStringImpl* uid)
{
    VM& vm = *m_state.instance.vm;
    if (uid != vm.propertyNames->message.impl() || structure->classInfoForCells() != ErrorInstance::info() || structure->isDictionary())
        return;
    uint32_t& place = m_state.instance.propertyCaches.placeOfErrorWithMessage(structure->id().bits());
    if (place)
        return;
    unsigned attributes = 0;
    std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(uid, attributes);
    if (offset && *offset == firstOutOfLineOffset && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        place = structure->id().bits();
}

StructuresBelowBuiltinClasses::StructuresBelowBuiltinClasses(State& state)
    : m_state(state)
    , m_ownerID(state.chainDependents->addOwner(*this))
{
    const BuiltinTable& table = BuiltinTable::singleton();
    for (unsigned index = 0; index < table.classCount(); ++index) {
        if (const BuiltinClass& builtinClass = table.at(BuiltinClassID(index)); builtinClass.keepsStructuresOfClassesBelow)
            m_classes.append({ builtinClass.type, builtinClass.id });
    }
    state.caches->add(CacheKind::WeakByKey, "structures below built-in classes"_s, *this);
}

void StructuresBelowBuiltinClasses::dropChainDependent(VM&, uint32_t structureID, UniquedStringImpl*)
{
    uint32_t& place = m_state.instance.propertyCaches.placeOfStructureBelowBuiltinClass(structureID);
    if (place == structureID)
        place = 0;
}

bool StructuresBelowBuiltinClasses::hasChainDependent(VM&, uint32_t structureID, UniquedStringImpl*) const
{
    return m_state.instance.propertyCaches.placeOfStructureBelowBuiltinClass(structureID) == structureID;
}

size_t StructuresBelowBuiltinClasses::removeDeadEntries(VM& vm, CollectionScope)
{
    size_t numberCleared = 0;
    for (uint32_t& place : m_state.instance.propertyCaches.structuresBelowBuiltinClasses) {
        if (!place || vm.heap.isMarked(std::bit_cast<StructureID>(place).decode()))
            continue;
        place = 0;
        ++numberCleared;
    }
    return numberCleared;
}

BuiltinClassID StructuresBelowBuiltinClasses::classAbove(Structure* structure) const
{
    JSType type = structure->typeInfo().type();
    for (auto [typeOfInstances, builtinClass] : m_classes) {
        if (type == typeOfInstances)
            return builtinClass;
    }
    return { };
}

bool StructuresBelowBuiltinClasses::holds(Structure* structure) const
{
    return m_state.instance.propertyCaches.placeOfStructureBelowBuiltinClass(structure->id().bits()) == structure->id().bits();
}

void StructuresBelowBuiltinClasses::keep(Structure* structure, std::span<JSObject* const> changeableObjects)
{
    m_state.chainDependents->add(changeableObjects, { nullptr, structure->id().bits(), m_ownerID });
    m_state.instance.propertyCaches.placeOfStructureBelowBuiltinClass(structure->id().bits()) = structure->id().bits();
    countEvent(*m_state.instance.vm, RuntimeCounterID::StructuresInheritingBuiltinMethods);
}

void PropertyCaches::keepIfBelowBuiltinClass(Structure* structure)
{
    BuiltinClassID builtinClass = m_structuresBelowBuiltinClasses.classAbove(structure);
    if (!builtinClass) [[likely]]
        return;
    if (structure->isPristine() || structure->isDictionary() || structure->hasPolyProto() || structure->typeInfo().overridesGetOwnPropertySlot() || m_structuresBelowBuiltinClasses.holds(structure))
        return;
    JSGlobalObject* globalObject = m_state.instance.globalObject;
    const BuiltinTable& table = BuiltinTable::singleton();
    BuiltinID prototype = table.at(builtinClass).prototype;
    JSObject* prototypeOfClass = m_state.builtinCells ? m_state.builtinCells->lockedObjects().get(prototype.index()) : nullptr;
    if (structure->realm() != globalObject || !prototypeOfClass)
        return;
    ChainObjects changeableObjects;
    for (BuiltinID member : table.membersOf(prototype)) {
        if (!table.boxedCallStubOf(table.canonicalID(member)))
            continue;
        Identifier name = table.identifierOf(m_vm, table.at(member).name);
        unsigned attributes = 0;
        std::optional<PropertyOffset> own = structure->getWithoutMaterializing(name.impl(), attributes);
        if (!own || isValidOffset(*own))
            return;
        ChainWalk walk = walkChain({ globalObject, structure, nullptr, ChainWalkMode::AllocatesNothing, ChainWalkUse::KeptForReads }, name.impl());
        if (walk.result != ChainWalkResult::Found || walk.holder != prototypeOfClass)
            return;
        for (JSObject* object : walk.changeableObjects) {
            if (!changeableObjects.contains(object))
                changeableObjects.append(object);
        }
    }
    ensureChangesAreReported(changeableObjects.span());
    m_structuresBelowBuiltinClasses.keep(structure, changeableObjects.span());
}

static uint16_t wordOfExtraProperties(JSObject& base, bool usesWideReferences)
{
    const CompiledStructure* compiled = compiledObjectStructureOrNull(base.structure());
    if (!compiled || usesWideReferences || !compiled->extraPropertiesStructureID() || belongsToOneObject(WTF::opaque(base.structure())))
        return 0;
    JSObject* extraProperties = uncheckedDowncast<Object>(&base)->extraProperties();
    std::optional<SlotData> slot = compiled->extraPropertiesSlot();
    if (!slot || !extraProperties || extraProperties->structureID() != compiled->extraPropertiesStructureID() || extraProperties->structure()->isDictionary())
        return 0;
    return slot->offset % sizeof(uint32_t) || slot->offset / sizeof(uint32_t) > UINT16_MAX ? 0 : static_cast<uint16_t>(slot->offset / sizeof(uint32_t));
}

std::optional<uint16_t> PropertyCaches::holderOfReadGroup(JSObject& object, const ReadGroupCache& group, uint32_t numberOfReads)
{
    Structure* structure = WTF::opaque(object.structure());
    if (!structure->isCompiledStructure()) {
        if (structure->isDictionary() || structure->typeInfo().overridesGetOwnPropertySlot() || TypeInfo::hasStaticPropertyTable(object.inlineTypeFlags()))
            return std::nullopt;
        return 0;
    }
    uint32_t numberOfFields = 0;
    if (Object* compiled = structure->isDictionary() ? nullptr : compiledObjectOrNull(&object)) {
        for (uint32_t index = 0; index < numberOfReads; ++index) {
            const FieldData* field = compiled->fieldNamed(m_state, nameOf(group.keys[index]).uid);
            numberOfFields += field && offsetOfPlainValueInCell(*field);
        }
    }
    uint16_t wordOfExtension = numberOfFields == numberOfReads ? 0 : wordOfExtraProperties(object, m_state.instance.options().useWideReferences());
    if (wordOfExtension)
        return wordOfExtension;
    return numberOfFields ? std::optional<uint16_t>(0) : std::nullopt;
}

ReadGroupCache::Row* PropertyCaches::claimReadGroupRow(JSValue base, ReadGroupCache& group, uint32_t numberOfReads)
{
    JSObject* object = base.getObject();
    std::optional<uint16_t> holder = object ? holderOfReadGroup(*object, group, numberOfReads) : std::nullopt;
    const uint16_t wordOfExtension = holder.value_or(0);
    uint32_t id = holder ? object->structureID().bits() : 0;
    RELEASE_ASSERT(!(id & (ReadGroupCache::markOfHiddenRow | ReadGroupCache::markOfExtension | ReadGroupCache::markOfStorage)));

    if (group.samplesLeft) {
        ReadGroupCache::Row* hidden = nullptr;
        for (unsigned index = 0; index < ReadGroupCache::numberOfRows; ++index) {
            if (id && group.rows[index].structure == (id | ReadGroupCache::markOfHiddenRow)) {
                hidden = &group.rows[index];
                ++group.cameInLastSample[index];
            }
        }
        for (unsigned index = 0; id && !hidden && index < ReadGroupCache::numberOfRows; ++index) {
            if (group.rows[index].structure)
                continue;
            hidden = &group.rows[index];
            hidden->claim(id, wordOfExtension);
            hidden->hide();
            ++group.cameInLastSample[index];
        }
        group.samplesThatHadRow += !!hidden;
        if (!--group.samplesLeft) {
            size_t mostCommon = std::ranges::max_element(group.cameInLastSample) - std::begin(group.cameInLastSample);
            if (mostCommon) {
                if (hidden == &group.rows[0] || hidden == &group.rows[mostCommon])
                    hidden = &group.rows[hidden == &group.rows[0] ? mostCommon : 0];
                std::swap(group.rows[0], group.rows[mostCommon]);
                std::swap(group.cameInLastSample[0], group.cameInLastSample[mostCommon]);
            }
            for (ReadGroupCache::Row& row : group.rows)
                row.show();
            if (group.samplesThatHadRow * 3 < ReadGroupCache::numberOfSamples)
                group.giveUp();
            group.missesLeft = ReadGroupCache::firstWait << group.doublings;
            if (group.doublings < ReadGroupCache::maxDoublings)
                ++group.doublings;
        }
        return hidden;
    }

    static constexpr uint8_t waitsWhileNothingIsClaimed = 3;
    if (!id && std::ranges::none_of(group.rows, [](const ReadGroupCache::Row& row) { return !!row.structure; })) {
        if (group.doublings >= waitsWhileNothingIsClaimed)
            group.giveUp();
        else
            group.missesLeft = ReadGroupCache::firstWait << group.doublings++;
        return nullptr;
    }

    ReadGroupCache::Row* claimed = nullptr;
    if (id) {
        for (ReadGroupCache::Row& row : group.rows) {
            if (row.structure == id)
                return &row;
            if (!row.structure && !claimed)
                claimed = &row;
        }
    }
    bool putsOutRow = id && !claimed;
    if (putsOutRow) {
        unsigned fewest = ReadGroupCache::numberOfRows - 1;
        for (unsigned index = ReadGroupCache::numberOfRows - 1; index--;) {
            if (group.cameInLastSample[index] < group.cameInLastSample[fewest])
                fewest = index;
        }
        claimed = &group.rows[fewest];
    }
    if (claimed)
        claimed->claim(id, wordOfExtension);
    if (claimed && !putsOutRow && claimed != &group.rows[0]) {
        std::swap(*claimed, group.rows[0]);
        std::swap(group.cameInLastSample[claimed - group.rows], group.cameInLastSample[0]);
        claimed = &group.rows[0];
    }
    if (!id || putsOutRow) {
        for (ReadGroupCache::Row& row : group.rows) {
            if (row.structure)
                row.hide();
        }
        group.samplesLeft = ReadGroupCache::numberOfSamples;
        group.samplesThatHadRow = 0;
        std::ranges::fill(group.cameInLastSample, 0);
    }
    return claimed;
}

bool PropertyCaches::isAbsentFromPlainObject(JSObject* object, UniquedStringImpl* uid)
{
    Structure* structure = WTF::opaque(object->structure());
    if (structure->typeInfo().type() != FinalObjectType || structure->isCompiledStructure() || structure->isDictionary() || !structure->propertyAccessesAreCacheableForAbsence() || structure->needImpurePropertyWatchpoint())
        return false;
    if (PropertyName(uid).isPrivateName() || isNeverCachedName(m_vm, uid) || isPreparedBeforeLookups(m_vm, uid) || parseIndex(*uid))
        return false;
    if (JSGlobalObject* realm = structure->realm(); realm && Instance::of(realm) != &m_state.instance)
        return false;
    ChainWalk walk = walkChain({ m_state.instance.globalObject, structure, object, ChainWalkMode::AllocatesNothing, ChainWalkUse::KeptForReadsThatAskObjects }, uid);
    if (walk.result != ChainWalkResult::Absent || walk.firstObjectWithEmptySlot)
        return false;
    ensureChangesAreReported(walk.changeableObjects.span());
    return true;
}

void PropertyCaches::fillReadGroupPlace(JSCell* base, uint32_t identifierNumber, ReadGroupCache::Row& row, uint32_t index, bool mayBeAbsent)
{
    int16_t& place = row.places[index];
    if (place != ReadGroupCache::placeIsNotKnown)
        return;
    place = ReadGroupCache::placeIsAskedOfStub;
    JSObject* holder = asObject(base);
    if (!row.wordOfExtension && holder->structure()->isCompiledStructure()) {
        const FieldData* field = uncheckedDowncast<Object>(base)->fieldNamed(m_state, nameOf(identifierNumber).uid);
        std::optional<uint32_t> offset = field ? offsetOfPlainValueInCell(*field) : std::nullopt;
        if (offset && *offset / sizeof(EncodedJSValue) <= static_cast<size_t>(INT16_MAX))
            place = static_cast<int16_t>(*offset / sizeof(EncodedJSValue));
        return;
    }
    if (row.wordOfExtension) {
        auto* object = uncheckedDowncast<Object>(base);
        holder = object->extraProperties();
        if (wordOfExtraProperties(*object, m_state.instance.options().useWideReferences()) != row.wordOfExtension || object->fieldNamed(m_state, nameOf(identifierNumber).uid))
            return;
    }
    unsigned attributes;
    std::optional<PropertyOffset> offset = holder->structure()->getWithoutMaterializing(nameOf(identifierNumber).uid, attributes);
    if (mayBeAbsent && !row.wordOfExtension && offset && !isValidOffset(*offset) && isAbsentFromPlainObject(holder, nameOf(identifierNumber).uid)) {
        row.absentReads |= 1u << index;
        m_state.instance.propertyCaches.readGroupsSayAbsence = 1;
        return;
    }
    if (!offset || !isValidOffset(*offset) || (attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
        return;
    static_assert(!(JSObject::offsetOfInlineStorage() % sizeof(EncodedJSValue)));
    ptrdiff_t wordIndex = isInlineOffset(*offset) ? static_cast<ptrdiff_t>(JSObject::offsetOfInlineStorage() / sizeof(EncodedJSValue) + offsetInInlineStorage(*offset)) : static_cast<ptrdiff_t>(offsetInButterfly(*offset));
    if (wordIndex > INT16_MIN && wordIndex <= INT16_MAX && (wordIndex > 0) == isInlineOffset(*offset) && wordIndex)
        place = static_cast<int16_t>(wordIndex);
}

void PropertyCaches::claimAndFillReadGroupRow(JSValue base, ReadGroupCache& group, uint32_t numberOfReads, uint32_t optionalReads)
{
    RELEASE_ASSERT(numberOfReads && numberOfReads <= ReadGroupCache::numberOfPlaces);
    ReadGroupCache::Row* row = claimReadGroupRow(base, group, numberOfReads);
    if (!row)
        return;
    bool isHidden = row->structure & ReadGroupCache::markOfHiddenRow;
    RELEASE_ASSERT(base.isObject() && (row->structure & ~ReadGroupCache::markOfHiddenRow) == base.asCell()->structureID().bits());
    row->numberOfReads = static_cast<uint8_t>(numberOfReads);
    for (uint32_t index = 0; index < numberOfReads; ++index)
        fillReadGroupPlace(base.asCell(), group.keys[index], *row, index, optionalReads >> index & 1);
    if (!isHidden)
        row->show();
}

void PropertyCaches::fileOwnPlainValueUnlessFiled(JSObject* base, uint32_t identifierNumber)
{
    Structure* structure = WTF::opaque(base->structure());
    CachedName name = nameOf(identifierNumber);
    if (uint16_t word = wordOfExtraProperties(*base, m_state.instance.options().useWideReferences())) {
        unsigned attributes;
        auto* object = uncheckedDowncast<Object>(base);
        JSObject* extraProperties = object->extraProperties();
        std::optional<PropertyOffset> offset = object->fieldNamed(m_state, name.uid) || m_tables.find(structure->id(), name, SlotAccess::Read) ? std::nullopt : extraProperties->structure()->getWithoutMaterializing(name.uid, attributes);
        if (word && offset && isValidOffset(*offset) && !(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))
            cache({ structure, name, SlotAccess::Read }, PropertyAnswer::own(PropertyAnswerKind::ExtraProperty, SlotAccess::Read, *offset));
        return;
    }
    if (structure->isDictionary() || structure->typeInfo().overridesGetOwnPropertySlot() || TypeInfo::hasStaticPropertyTable(base->inlineTypeFlags()) || m_tables.find(structure->id(), name, SlotAccess::Read))
        return;
    unsigned attributes;
    std::optional<PropertyOffset> offset = structure->getWithoutMaterializing(name.uid, attributes);
    if (!offset || !isValidOffset(*offset) || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return;
    JSValue value = valueAtOffset(base, *offset);
    if (!value)
        return;
    PropertySlot slot(base, PropertySlot::InternalMethodType::VMInquiry, &m_vm);
    slot.setValue(base, attributes, value, *offset);
    PropertyAnswer answer = classifyRead({ m_state.instance.globalObject, base, structure, name.uid, slot, ChainWalkMode::AllocatesNothing });
    if (answer.isCacheable())
        cache({ structure, name, SlotAccess::Read }, answer);
}

bool PropertyCaches::mayReadDictionary(Structure* structure)
{
    static constexpr uint32_t maxReads = 16;
    DictionaryReads& reads = m_readsOfDictionaries[(structure->id().bits() >> 4) % m_readsOfDictionaries.size()];
    if (reads.structure != structure->id().bits())
        reads = { structure->id().bits(), 0 };
    return reads.count < maxReads && ++reads.count;
}

bool PropertyCaches::fillFromHeldValue(uint32_t place, SiteUse use, Object& object, UniquedStringImpl* uid, bool stubFollowsFurtherAnswers)
{
    if (PropertyName(uid).isPrivateName() || isNeverCachedName(m_vm, uid) || isPreparedBeforeLookups(m_vm, uid) || parseIndex(*uid) || !object.tryNameHeldStructure(m_vm))
        return false;
    JSObject* held = object.compiledStructure().heldExtraProperties();
    unsigned attributes = 0;
    std::optional<PropertyOffset> offset = held->structure()->getWithoutMaterializing(uid, attributes);
    if (!offset || !isValidOffset(*offset) || attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue)
        return false;
    Slot slot = Slot::createInStorage(object.structureID(), Slot::Kind::Value, 0, held, *offset);
    if (slot.kind != Slot::Kind::Value)
        return false;
    std::optional<AnswersAtSites::AnswerByStructure> answer = answerOfSlot(use, slot, m_vm.heap.cage().base(), m_state.instance.options().useWideReferences(), m_state.objectThatHoldsUndefined);
    if (!answer)
        return false;
    if (m_vm.aotState().verifiesFastPathAnswers) [[unlikely]]
        askAgainWhatSlotAnswers(m_state.instance, &object, uid, slot);
    m_answersAtSites.fill(place, use, *answer, uid, { }, stubFollowsFurtherAnswers);
    return true;
}

void PropertyCaches::fillSiteAnswers(uint32_t place, JSCell* base, uint32_t identifierNumber, SiteUse use, bool stubFollowsFurtherAnswers, bool stubJumpsToArmOfAnswer, bool testsForPresence, bool readsForOtherReceiver)
{
    Structure* structure = WTF::opaque(base->structure());
    StructureID id = structure->id();
    CachedName name = nameOf(identifierNumber);
    if (isRead(use))
        m_errorsWithMessage.keepIfRead(structure, name.uid);
    if (compiledObjectStructureOrNull(structure))
        holdExtraPropertiesToStructureThatIsNamed(m_state.instance.globalObject, *uncheckedDowncast<Object>(base), "caching a lookup at a site"_s);
    if (use == SiteUse::AbsenceAmongExtraProperties) {
        const CompiledStructure* compiled = compiledObjectStructureOrNull(structure);
        unsigned attributes = 0;
        std::optional<PropertyOffset> offset = compiled && compiled->listsEveryExtraProperty() && !belongsToOneObject(structure) ? compiled->extraPropertiesStructure()->getWithoutMaterializing(name.uid, attributes) : std::nullopt;
        std::optional<SiteAnswers::Answer> answer = offset && !isValidOffset(*offset) ? SiteAnswers::tryByStructure(use, id, SiteAnswers::Form::Nothing, 0) : std::nullopt;
        const SiteAnswers* answers = m_answersAtSites.answersOf(place);
        if (!answer)
            m_answersAtSites.didFailToFill(place);
        else if (answers && answers->first.isByStructure() && answers->second.isByStructure())
            m_answersAtSites.addStructureThatLacksName(place, id, name.uid, { }, false);
        else
            m_answersAtSites.fill(place, use, { *answer }, name.uid, { });
        return;
    }
    if (use == SiteUse::Call)
        keepIfBelowBuiltinClass(structure);
    if (isCompiledClosure(base) && uncheckedDowncast<Closure>(base)->canHoldStaticFieldInVariable(name.uid)) {
        m_tables.remove(id, name, SlotAccess::Read);
        return;
    }

    if (compiledObjectStructureOrNull(structure)) {
        auto* object = uncheckedDowncast<Object>(base);
        const FieldData* field = object->fieldNamed(m_state, name.uid);
        bool isLeftOut = field && isRead(use) && !object->isInSlot(*field) && (!(object->orderWord() & orderWordHasFieldOutsideSlot) || structureSaysFieldIsAbsent(*structure, name));
        if (isLeftOut && fillAboveFieldLeftOut(place, use, *object, name))
            return;
        if (std::optional<uint32_t> offset = field ? offsetOfPlainValueInCell(*field) : std::nullopt; offset && (object->isInSlot(*field) || isLeftOut)) {
            const SiteAnswers* answers = m_answersAtSites.answersOf(place);
            bool hasRoomOnlyByStructure = answers && answers->holdsShape() && answers->first.key != object->shapeWord() && (!answers->second.key || isRoad(use, answers->second) || stubFollowsFurtherAnswers);
            if (isRead(use) && !hasRoomOnlyByStructure && !object->bearsMark()) {
                m_answersAtSites.fillByShape(place, use, SiteAnswers::byShape(object->shapeWord(), *offset), stubJumpsToArmOfAnswer ? SiteAnswers::tryByStructure(use, id, SiteAnswers::Form::OwnFieldInCell, *offset) : std::nullopt);
                return;
            }
            if (std::optional<SiteAnswers::Answer> answer = SiteAnswers::tryByStructure(use, id, SiteAnswers::Form::OwnFieldInCell, *offset))
                m_answersAtSites.fill(place, use, { *answer }, name.uid, { });
            else
                m_answersAtSites.didFailToFill(place);
            return;
        }
        if (std::optional<SiteAnswers::Answer> answer = field && isRead(use) ? answerForTypedSlotInCell(*object, *field) : std::nullopt; answer && (object->isInSlot(*field) || isLeftOut)) {
            const SiteAnswers* answers = m_answersAtSites.answersOf(place);
            bool hasRoomOnlyByStructure = answers && answers->holdsShape() && answers->first.key != answer->key && (!answers->second.key || isRoad(use, answers->second));
            std::optional<SiteAnswers::Form> form = hasRoomOnlyByStructure || object->bearsMark() ? SiteAnswers::formByStructureOfSlotInCell(field->slot.representation()) : std::nullopt;
            if (std::optional<SiteAnswers::Answer> byStructure = form ? SiteAnswers::tryByStructure(use, id, *form, field->slot.offset) : std::nullopt) {
                m_answersAtSites.fill(place, use, { *byStructure }, name.uid, { });
                return;
            }
            if (object->bearsMark())
                m_answersAtSites.didFailToFill(place);
            else {
                std::optional<SiteAnswers::Form> sameForm = stubJumpsToArmOfAnswer ? SiteAnswers::formByStructureOfSlotInCell(field->slot.representation()) : std::nullopt;
                m_answersAtSites.fillByShape(place, use, *answer, sameForm ? SiteAnswers::tryByStructure(use, id, *sameForm, field->slot.offset) : std::nullopt);
            }
            return;
        }
        if (std::optional<SiteAnswers::Answer> answer = field && isRead(use) && !object->bearsMark() ? answerForPlainValueInExtension(*object, *field) : std::nullopt; answer && (object->isInSlot(*field) || isLeftOut)) {
            m_answersAtSites.fillByShape(place, use, *answer, stubJumpsToArmOfAnswer ? SiteAnswers::tryByStructure(use, id, SiteAnswers::Form::OwnFieldInExtension, answer->operand) : std::nullopt);
            return;
        }
        if (!field && !testsForPresence && object->compiledStructure().heldExtraProperties() && fillFromHeldValue(place, use, *object, name.uid, stubFollowsFurtherAnswers))
            return;
        if (std::optional<uint32_t> operand = field && (use == SiteUse::Call || stubJumpsToArmOfAnswer) && isPlainValueInExtension(*object, *field) && object->isInSlot(*field) ? SiteAnswers::operandOfFieldInExtension(field->slot.offset) : std::nullopt) {
            if (std::optional<SiteAnswers::Answer> answer = SiteAnswers::tryByStructure(use, id, SiteAnswers::Form::OwnFieldInExtension, *operand))
                m_answersAtSites.fill(place, use, { *answer }, name.uid, { });
            else
                m_answersAtSites.didFailToFill(place);
            return;
        }
    }

    std::optional<Slot> slot = m_tables.find(id, name, SlotAccess::Read);
    if (slot && m_vm.aotState().verifiesFastPathAnswers) [[unlikely]]
        askAgainWhatSlotAnswers(m_state.instance, base, name.uid, *slot);
    auto hasNoFormAtPlaces = [](Slot::Kind kind) {
        return (kind == Slot::Kind::ClosureName || kind == Slot::Kind::ClosureLength || kind == Slot::Kind::CustomGetter || kind == Slot::Kind::Getter || kind == Slot::Kind::ExtraGetter);
    };
    bool rowAnswers = slot && stubJumpsToArmOfAnswer && hasNoFormAtPlaces(slot->kind) && !belongsToOneObject(structure) && m_tables.holdsInFrontRows(id, name, SlotAccess::Read)
        && !(compiledObjectStructureOrNull(structure) && uncheckedDowncast<Object>(base)->fieldNamed(m_state, name.uid));
    if (slot && (slot->kind == Slot::Kind::ExtraProperty || slot->kind == Slot::Kind::ExtraGetter) && belongsToOneObject(structure))
        slot = std::nullopt;
    bool placeMayNotCallGetter = slot && slot->kind == Slot::Kind::Getter && slot->has(Slot::isOwn) && compiledObjectStructureOrNull(structure);
    std::optional<AnswersAtSites::AnswerByStructure> answer = slot && !testsForPresence && !readsForOtherReceiver ? getterFieldAnswer(use, *slot) : std::nullopt;
    if (!answer && slot && slot->kind == Slot::fieldOfGetter) {
        ChainWalk above = walkChain({ m_state.instance.globalObject, structure, asObject(base), ChainWalkMode::AllocatesNothing, ChainWalkUse::KeptForReadsThatAskObjects }, name.uid);
        if (above.result == ChainWalkResult::Found && above.holder && isValidOffset(above.offset) && (above.attributes & PropertyAttribute::Accessor))
            slot = Slot::createInStorage(id, Slot::Kind::Getter, Slot::dependsOnChain, above.holder, above.offset);
    }
    if (!answer)
        answer = slot && !placeMayNotCallGetter ? answerOfSlot(use, *slot, m_vm.heap.cage().base(), m_state.instance.options().useWideReferences(), m_state.objectThatHoldsUndefined) : std::nullopt;
    if (!answer && slot && slot->isForProxies() && isRead(use) && stubJumpsToArmOfAnswer && !m_state.instance.options().useWideReferences() && slot->location == identifierNumber && RowDisplacementTable::hasColumn(identifierNumber) && m_keysForTraps.get(name.uid) == slot->holder) {
        uintptr_t wordsToKey = (std::bit_cast<uintptr_t>(slot->holder) - m_vm.heap.cage().base()) / sizeof(EncodedJSValue);
        if (std::optional<SiteAnswers::Answer> throughProxy = wordsToKey <= UINT32_MAX ? SiteAnswers::tryByStructure(use, id, SiteAnswers::Form::Nothing, static_cast<uint32_t>(wordsToKey)) : std::nullopt) {
            answer = AnswersAtSites::AnswerByStructure { *throughProxy, 0 };
            countEvent(m_vm, RuntimeCounterID::SiteCacheProxyEntriesStored);
        }
    }
    if (!answer && rowAnswers && !m_answersAtSites.holdsAnotherStructure(place, use, id)) {
        if (std::optional<SiteAnswers::Answer> road = SiteAnswers::tryByStructure(use, id, SiteAnswers::Form::Nothing, 0))
            answer = AnswersAtSites::AnswerByStructure { *road, 0 };
    }
    if (!answer) {
        m_answersAtSites.didFailToFill(place);
        return;
    }
    bool isOneMoreStructureThatLacksName = slot->kind == Slot::Kind::Absent && m_answersAtSites.holdsAnotherStructure(place, use, id);
    bool mayGiveUp = isRead(use) && !testsForPresence;
    if (!slot->has(Slot::dependsOnChain)) {
        if (isOneMoreStructureThatLacksName)
            m_answersAtSites.addStructureThatLacksName(place, id, name.uid, { }, mayGiveUp);
        else
            m_answersAtSites.fill(place, use, *answer, name.uid, { }, stubFollowsFurtherAnswers);
        return;
    }
    JSGlobalObject* globalObject = m_state.instance.globalObject;
    if (JSGlobalObject* realm = structure->realm(); (realm && Instance::of(realm) != &m_state.instance) || structure->typeInfo().type() == GlobalProxyType) {
        m_answersAtSites.didFailToFill(place);
        return;
    }
    ChainWalk walk = walkChain({ globalObject, structure, base->isObject() ? asObject(base) : nullptr, ChainWalkMode::AllocatesNothing, ChainWalkUse::KeptForReadsThatAskObjects }, name.uid);
    if (walk.result == ChainWalkResult::Unknown || (slot->kind == Slot::Kind::Absent && (walk.result != ChainWalkResult::Absent || walk.firstObjectWithEmptySlot))) {
        m_answersAtSites.didFailToFill(place);
        return;
    }
    ensureChangesAreReported(walk.changeableObjects.span());
    if (isOneMoreStructureThatLacksName)
        m_answersAtSites.addStructureThatLacksName(place, id, name.uid, walk.changeableObjects.span(), mayGiveUp);
    else
        m_answersAtSites.fill(place, use, *answer, name.uid, walk.changeableObjects.span(), stubFollowsFurtherAnswers);
}

void PropertyCaches::didFinishCollection(VM&, CollectionScope)
{
    {
        Locker locker { m_state.caches->lock() };
        m_additionsSinceCollection.shrink(0);
    }
    m_shouldForgetMisses.store(true, std::memory_order_relaxed);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCallGetterOfSlot, EncodedJSValue, (Instance* instance, EncodedJSValue base, JSCell* getterSetter, uint32_t))
{
    constexpr Entry entry = Entry::operationAOTCallGetterOfSlot;
    AOT_OPERATION_BEGIN(instance, entry);
    if (vm.aotState().verifiesFastPathAnswers) [[unlikely]]
        stopAtStaleGetterOfPendingRead(*instance, JSValue::decode(base), getterSetter);
    ++instance->propertyCaches.slowPathReadCount;
    OPERATION_RETURN(scope, JSValue::encode(cellArgument<GetterSetter>(getterSetter, entry)->callGetter(globalObject, JSValue::decode(base))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCallSetterOfSlot, void, (Instance* instance, EncodedJSValue base, JSCell* getterSetter, EncodedJSValue value, uint32_t putMode, uint32_t))
{
    constexpr Entry entry = Entry::operationAOTCallSetterOfSlot;
    AOT_OPERATION_BEGIN(instance, entry);
    cellArgument<GetterSetter>(getterSetter, entry)->callSetter(globalObject, JSValue::decode(base), JSValue::decode(value), static_cast<PutMode>(putMode) == PutMode::Strict);
    OPERATION_RETURN(scope);
}

static void addPropertyByTransition(VM& vm, JSObject* target, Structure* newStructure, PropertyOffset offset, JSValue value)
{
    ASSERT(newStructure->isValidOffset(offset) && newStructure->inlineCapacity() == target->structure()->inlineCapacity() && newStructure->storedPrototype() == target->structure()->storedPrototype());
    bool hasRoomInOwnCell = !target->structure()->outOfLineCapacity() && !target->structure()->hasElementsStartOffset() && newStructure->outOfLineCapacity() == initialOutOfLineCapacity
        && std::bit_cast<char*>(target->butterfly()) == std::bit_cast<char*>(target) + sizeOfArrayWithStorage(0, propertyRoomOfArray);
    if (!hasRoomInOwnCell)
        target->growOutOfLineStorageForTransition(vm, newStructure);
    target->putDirectOffset(vm, offset, value);
    target->setStructure(vm, newStructure);
}

static bool isOrdinaryFunction(Instance& instance, JSFunction* function)
{
    if (function->rareData())
        return false;
    JSCell* executable = function->executable();
    auto isOfKind = [&](FunctionTables::ExecutableKind kind) {
        return executable == instance.closures.executables[static_cast<unsigned>(kind)];
    };
    return isOfKind(FunctionTables::ExecutableKind::Constructible) || isOfKind(FunctionTables::ExecutableKind::NotConstructible) || executable == instance.realm.boundFunctionExecutable;
}

bool tryStoreBySlot(Instance& instance, JSObject* target, UniquedStringImpl* uid, JSValue value)
{
    VM& vm = *instance.vm;
    Structure* structure = target->structure();
    if (structure->isDictionary())
        return false;
    const Slot* filed = instance.state->propertyCaches->findByKey(structure, uid, SlotAccess::Store);
    if (!filed)
        return false;
    Slot slot = *filed;
    switch (slot.kind) {
    case Slot::Kind::Value:
    case Slot::Kind::FarValue:
        target->putDirectOffset(vm, slot.offsetInStorage(), value);
        return true;
    case Slot::Kind::Transition:
        if (slot.transition.newStorageCapacity && !structure->outOfLineCapacity() && isJSFunctionType(target->type()) && !isOrdinaryFunction(instance, uncheckedDowncast<JSFunction>(target)))
            return false;
        addPropertyByTransition(vm, target, WTF::opaque(slot.transition.newStructureID.decode()), slot.offsetInStorage(), value);
        holdClosureToLackingName(vm, target, uid);
        return true;
    default:
        return false;
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAddPropertyBySlot, void, (Instance* instance, JSCell* object, uint64_t header, uint64_t payload, EncodedJSValue value, uint32_t))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAddPropertyBySlot);
    Slot slot = std::bit_cast<Slot>(std::array<uint64_t, 2> { header, payload });
    RELEASE_ASSERT(object->structureID() == slot.structureID);
    if (slot.kind == Slot::Kind::Transition) {
        addPropertyByTransition(vm, asObject(object), WTF::opaque(slot.transition.newStructureID.decode()), slot.offsetInStorage(), JSValue::decode(value));
        holdClosureToLackingName(vm, object, object->structure()->transitionPropertyName());
        return;
    }
    RELEASE_ASSERT(slot.kind == Slot::Kind::ExtraPropertyAddition);
    Object* receiver = uncheckedDowncast<Object>(object);
    // The slot is a copy out of a weak cache, and no object need have this Structure yet: this pointer alone keeps it alive while the extra properties are allocated.
    Structure* newStructure = WTF::opaque(slot.extraPropertyAddition.newStructureID.decode());
    JSObject* extraProperties = receiver->ensureExtraProperties(vm);
    RELEASE_ASSERT(extraProperties);
    addPropertyByTransition(vm, extraProperties, compiledObjectStructureOrNull(newStructure)->extraPropertiesStructure(), Slot::offsetFor(slot.extraPropertyAddition.wordIndex), JSValue::decode(value));
    receiver->setVariantOfStructure(vm, newStructure);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
