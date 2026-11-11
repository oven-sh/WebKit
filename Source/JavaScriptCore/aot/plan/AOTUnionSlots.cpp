/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUnionPlan.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTSort.h"
#include "MarkedSpace.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned numberOfWidthsInUnionSlots = 4;
static constexpr uint32_t widestSlotInUnionSlots = 1u << (numberOfWidthsInUnionSlots - 1);
static_assert(widestSlotInUnionSlots == widthOf(SlotRepresentation::JSValue) && !(offsetOfFirstField % widestSlotInUnionSlots));

static constexpr uint32_t roundUpToWidestSlotInUnionSlots(uint32_t bytes)
{
    return (bytes + widestSlotInUnionSlots - 1) & ~(widestSlotInUnionSlots - 1);
}

namespace {

using WidthCountsInUnionSlots = std::array<uint32_t, numberOfWidthsInUnionSlots>;

bool remainingRowsFitInUnionSlots(const WidthCountsInUnionSlots& room, const WidthCountsInUnionSlots& unplaced)
{
    uint64_t taken = 0;
    for (unsigned logSize = numberOfWidthsInUnionSlots; logSize--;) {
        taken = 2 * taken + unplaced[logSize];
        if (room[logSize] < taken)
            return false;
    }
    return true;
}

class CountsInUnionSlots {
public:
    explicit CountsInUnionSlots(size_t numberOfWords)
        : m_numberOfWords(numberOfWords)
    {
    }

    void add(std::span<const uint64_t> positions, uint32_t amount)
    {
        for (unsigned lowest = 0; amount >> lowest; ++lowest) {
            if (!(amount >> lowest & 1))
                continue;
            for (size_t word = 0; word < positions.size(); ++word) {
                uint64_t carry = positions[word];
                for (unsigned plane = lowest; carry; ++plane) {
                    while (m_planes.size() <= plane * m_numberOfWords)
                        m_planes.insertFill(m_planes.size(), 0, m_numberOfWords);
                    uint64_t& bits = m_planes[plane * m_numberOfWords + word];
                    carry = std::exchange(bits, bits ^ carry) & carry;
                }
            }
        }
    }

    template<typename Functor>
    void forEachCount(const Functor& functor) const
    {
        for (size_t index = 0; index < m_planes.size(); ++index) {
            for (uint64_t bits = m_planes[index]; bits; bits &= bits - 1)
                functor(index % m_numberOfWords * 64 + std::countr_zero(bits), 1u << (index / m_numberOfWords));
        }
    }

private:
    size_t m_numberOfWords;
    Vector<uint64_t, 64> m_planes;
};

class CellInUnionSlots {
public:
    bool isInitialized() const { return m_isInitialized; }

    void initialize(uint32_t firstByte, uint32_t limit, const WidthCountsInUnionSlots& unplaced)
    {
        m_isInitialized = true;
        m_firstByte = firstByte;
        m_limit = limit;
        m_end = firstByte;
        m_unplaced = unplaced;
        m_occupied.fill(0, limit / 64 + 1);
        RELEASE_ASSERT(firstByte < 64);
        m_occupied[0] = (1ull << firstByte) - 1;
        for (uint32_t offset = limit; offset < m_occupied.size() * 64; offset += widestSlotInUnionSlots)
            m_occupied[offset / 64] |= maskOf(offset, widestSlotInUnionSlots);
        for (unsigned logSize = 0; logSize < numberOfWidthsInUnionSlots; ++logSize)
            m_room[logSize] = (limit - firstByte) >> logSize;
        m_freeWidestSlots.fill(0, limit / widestSlotInUnionSlots / 64 + 1);
        for (uint32_t offset = roundUpToWidestSlotInUnionSlots(firstByte); offset + widestSlotInUnionSlots <= limit; offset += widestSlotInUnionSlots)
            m_freeWidestSlots[offset / widestSlotInUnionSlots / 64] |= 1ull << (offset / widestSlotInUnionSlots % 64);
    }

    uint32_t limit() const { return m_limit; }
    uint32_t end() const { return m_end; }

    bool isFree(uint32_t offset, uint32_t size) const
    {
        return offset + size <= m_limit && !(m_occupied[offset / 64] & maskOf(offset, size));
    }

    bool canPlace(uint32_t offset, unsigned logSize) const
    {
        if (!isAlignedAndFree(offset, logSize) || !m_unplaced[logSize])
            return false;
        WidthCountsInUnionSlots unplaced = m_unplaced;
        --unplaced[logSize];
        return remainingRowsFitInUnionSlots(roomAfterTaking(offset, logSize), unplaced);
    }

    void keepPlaceableOffsets(std::span<uint64_t> words, unsigned logSize) const
    {
        std::array<bool, numberOfWidthsInUnionSlots> fits = fitsInLargerBlocks(logSize);
        for (size_t word = 0; word < words.size(); ++word) {
            uint64_t placeable = 0;
            if (logSize == numberOfWidthsInUnionSlots - 1) {
                if (fits[0] && word < m_freeWidestSlots.size())
                    placeable = m_freeWidestSlots[word];
            } else {
                for (unsigned part = 0; part < 1u << logSize; ++part) {
                    size_t occupied = (word << logSize) + part;
                    if (occupied < m_occupied.size())
                        placeable |= joinFirstBytes(placeableFirstBytes(m_occupied[occupied], logSize, fits), logSize) << (part * (64 >> logSize));
                }
            }
            words[word] &= placeable;
        }
    }

    void place(uint32_t offset, unsigned logSize)
    {
        RELEASE_ASSERT(canPlace(offset, logSize));
        --m_unplaced[logSize];
        take(offset, logSize);
    }

    void occupy(uint32_t offset, unsigned logSize)
    {
        if (isAlignedAndFree(offset, logSize))
            take(offset, logSize);
    }

    std::optional<uint32_t> findOffset(unsigned logSize, uint32_t from = 0) const
    {
        uint32_t size = 1u << logSize;
        uint32_t offset = (std::max(from, m_firstByte) + size - 1) & ~(size - 1);
        while (offset + size <= m_limit) {
            if (m_occupied[offset / 64] == UINT64_MAX) {
                offset = (offset / 64 + 1) * 64;
                continue;
            }
            if (canPlace(offset, logSize))
                return offset;
            offset += size;
        }
        return std::nullopt;
    }

private:
    static uint64_t maskOf(uint32_t offset, uint32_t size) { return ((1ull << size) - 1) << (offset % 64); }

    bool isAlignedAndFree(uint32_t offset, unsigned logSize) const
    {
        uint32_t size = 1u << logSize;
        return !(offset & (size - 1)) && offset >= m_firstByte && isFree(offset, size);
    }

    WidthCountsInUnionSlots roomAfterTaking(uint32_t offset, unsigned logSize) const
    {
        WidthCountsInUnionSlots room = m_room;
        for (unsigned other = 0; other < numberOfWidthsInUnionSlots; ++other) {
            if (other <= logSize)
                room[other] -= 1u << (logSize - other);
            else if (isFree(offset & ~((1u << other) - 1), 1u << other))
                --room[other];
        }
        return room;
    }

    std::array<bool, numberOfWidthsInUnionSlots> fitsInLargerBlocks(unsigned logSize) const
    {
        std::array<bool, numberOfWidthsInUnionSlots> fits { };
        if (!m_unplaced[logSize])
            return fits;
        WidthCountsInUnionSlots unplaced = m_unplaced;
        --unplaced[logSize];
        WidthCountsInUnionSlots room = m_room;
        for (unsigned other = 0; other <= logSize; ++other)
            room[other] -= 1u << (logSize - other);
        for (unsigned larger = 0; logSize + larger < numberOfWidthsInUnionSlots; ++larger) {
            if (larger)
                --room[logSize + larger];
            fits[larger] = remainingRowsFitInUnionSlots(room, unplaced);
        }
        return fits;
    }

    static uint64_t placeableFirstBytes(uint64_t occupied, unsigned logSize, const std::array<bool, numberOfWidthsInUnionSlots>& fits)
    {
        constexpr std::array<uint64_t, numberOfWidthsInUnionSlots> firstBytesOfBlocks { UINT64_MAX, 0x5555555555555555, 0x1111111111111111, 0x0101010101010101 };
        std::array<uint64_t, numberOfWidthsInUnionSlots> freeBlocks;
        freeBlocks[0] = ~occupied;
        for (unsigned log = 1; log < numberOfWidthsInUnionSlots; ++log)
            freeBlocks[log] = freeBlocks[log - 1] & (freeBlocks[log - 1] >> (1u << (log - 1))) & firstBytesOfBlocks[log];
        uint64_t placeable = 0;
        uint64_t inLargerBlocks = 0;
        for (unsigned larger = numberOfWidthsInUnionSlots - logSize; larger--;) {
            uint64_t blocks = freeBlocks[logSize + larger];
            for (unsigned log = logSize + larger; log > logSize; --log)
                blocks |= blocks << (1u << (log - 1));
            if (fits[larger])
                placeable |= blocks & ~inLargerBlocks;
            inLargerBlocks |= blocks;
        }
        return placeable;
    }

    static uint64_t joinFirstBytes(uint64_t bits, unsigned logSize)
    {
        for (; logSize; --logSize) {
            for (unsigned shift = 1; shift < 32; shift *= 2)
                bits = (bits | bits >> shift) & masksOfEveryOtherRun[std::countr_zero(shift)];
        }
        return bits;
    }
    static constexpr std::array<uint64_t, 5> masksOfEveryOtherRun { 0x3333333333333333, 0x0f0f0f0f0f0f0f0f, 0x00ff00ff00ff00ff, 0x0000ffff0000ffff, 0x00000000ffffffff };

    void take(uint32_t offset, unsigned logSize)
    {
        m_room = roomAfterTaking(offset, logSize);
        m_occupied[offset / 64] |= maskOf(offset, 1u << logSize);
        m_freeWidestSlots[offset / widestSlotInUnionSlots / 64] &= ~(1ull << (offset / widestSlotInUnionSlots % 64));
        m_end = std::max(m_end, offset + (1u << logSize));
    }

    Vector<uint64_t, 4> m_occupied;
    Vector<uint64_t, 1> m_freeWidestSlots;
    bool m_isInitialized { false };
    uint32_t m_firstByte { 0 };
    uint32_t m_limit { 0 };
    uint32_t m_end { 0 };
    WidthCountsInUnionSlots m_unplaced { };
    WidthCountsInUnionSlots m_room { };
};

enum class FormInUnionSlots : uint8_t {
    Unknown,
    Unclaimed,
    Integer,
    Number,
    AnyValue,
};

struct OffsetUseInUnionSlots {
    uint32_t offset { 0 };
    uint32_t numberOfRows { 0 };
};

Vector<OffsetUseInUnionSlots, 8> countOffsetUsesInUnionSlots(Vector<OffsetUseInUnionSlots, 8>&& offsets)
{
    AOT::sort(offsets, [](const OffsetUseInUnionSlots& a, const OffsetUseInUnionSlots& b) {
        return a.offset < b.offset;
    });
    Vector<OffsetUseInUnionSlots, 8> uses;
    for (OffsetUseInUnionSlots use : offsets) {
        if (uses.isEmpty() || uses.last().offset != use.offset)
            uses.append(OffsetUseInUnionSlots { use.offset, 0 });
        uses.last().numberOfRows += use.numberOfRows;
    }
    AOT::sort(uses, [](const OffsetUseInUnionSlots& a, const OffsetUseInUnionSlots& b) {
        return a.numberOfRows != b.numberOfRows ? a.numberOfRows > b.numberOfRows : a.offset < b.offset;
    });
    return uses;
}

class PlacerOfUnionSlots {
    WTF_MAKE_NONCOPYABLE(PlacerOfUnionSlots);
public:
    PlacerOfUnionSlots(const Program& program, const LinkTables& link, const Families& families, const LayoutsBelowTheirBases& layoutsBelowTheirBases, const RowsOfSlotPlan& rows, const ScopedLambda<bool(LayoutID)>& hasCells, const ScopedLambda<uint32_t()>& addPlace)
        : m_program(program)
        , m_link(link)
        , m_families(families)
        , m_rows(rows)
        , m_hasCells(hasCells)
        , m_addPlace(addPlace)
        , m_numberOfLayouts(static_cast<uint32_t>(program.layouts.size()))
        , m_depths(layoutsBelowTheirBases.depths)
        , m_layoutsBelowTheirBases(layoutsBelowTheirBases.layouts)
        , m_layoutsPassedOver(rows.layoutsThatRepeatTheirBases && !rows.layoutsThatRepeatTheirBases->isEmpty() ? rows.layoutsThatRepeatTheirBases : nullptr)
        , m_isTouched(m_numberOfLayouts)
        , m_allAboveIsPlaced(m_numberOfLayouts)
    {
        RELEASE_ASSERT(rows.firstRowOfLayout.size() == m_numberOfLayouts + 1 && rows.ends.size() == m_numberOfLayouts && rows.offsetsOfFirstSlots.size() == m_numberOfLayouts && m_depths.size() == m_numberOfLayouts);
        RELEASE_ASSERT(rows.offsets.size() == rows.firstRowOfLayout.back() && rows.places.size() == rows.offsets.size() && (rows.widths.empty() || rows.widths.size() == rows.offsets.size()));
    }

    void run()
    {

        collectUnits();
        collectGroups();
        if (m_groups.isEmpty())
            return;
        if (m_layoutsPassedOver)
            addRowsPassedOver();
        m_cells.grow(m_numberOfLayouts);
        m_depthsReached.fill(0, m_numberOfLayouts);
        for (uint32_t group : m_groupsInOrder)
            placeAtSharedOffsets(m_groups[group]);
        fillLayoutsThatWereTouched();
        finishLayoutsThatWereTouched();
    }

private:
    using UnitIndex = uint32_t;
    using UnitList = Vector<UnitIndex, 8>;

    static constexpr uint32_t none = UINT32_MAX;

    struct Unit {
        uint32_t firstRow { 0 };
        uint32_t numberOfRows { 0 };
        LayoutID declaringLayout;
        uint32_t depth { 0 };
        uint32_t offset { 0 };
        uint32_t place { RowsOfSlotPlan::noPlace };
        uint8_t logWidth { 0 };
        FormInUnionSlots form { FormInUnionSlots::Unknown };
        bool isPlaced { false };
        bool isRefused { false };
        bool isQueued { false };
    };

    struct PartOfGroup {
        uint32_t index { 0 };
        uint32_t numberOfRows { 0 };
        bool isGroup { false };
    };

    struct Group {
        uint32_t firstPart { 0 };
        uint32_t numberOfParts { 0 };
        uint32_t depthOfDeepestDeclaration { 0 };
        uint32_t numberOfRows { 0 };
        uint32_t numberOfRowsInWidestFamily { 0 };
        uint32_t firstUnitLeft { 0 };
        uint32_t numberOfUnitsLeft { 0 };
        uint8_t logWidth { 0 };
        FormInUnionSlots form { FormInUnionSlots::Unknown };
    };

    struct NameInFamily {
        uint32_t key { 0 };
        PartOfGroup part;
    };

    struct RowsOfUnit {
        UnitIndex unit { 0 };
        uint32_t numberOfRows { 0 };
    };

    uint32_t rowOf(FieldReference field) const { return m_rows.firstRowOfLayout[field.layout.index()] + field.field; }
    uint32_t widthOfRow(uint32_t row) const { return m_rows.widths.empty() ? widestSlotInUnionSlots : m_rows.widths[row]; }
    bool isTouchable(LayoutID id) const { return m_depths[id.index()] != none; }
    std::span<const uint32_t> rowsOf(UnitIndex unit) const LIFETIME_BOUND { return m_rowsOfUnits.span().subspan(m_units[unit].firstRow, m_units[unit].numberOfRows); }
    uint32_t offsetOfFirstSlotOf(LayoutID id) const { return m_rows.offsetsOfFirstSlots[id.index()]; }

    bool standsByItself(LayoutID id) const
    {
        return !writtenBaseOf(m_program.at(id)) && m_program.layoutsExpectedBelow(id).empty() && m_link.appendedKeysOf(id).empty() && m_hasCells(id);
    }

    std::optional<FieldReference> findFieldAbove(FieldReference reference) const
    {
        LayoutID base = writtenBaseOf(m_program.at(reference.layout));
        if (!base)
            return std::nullopt;
        const Field& listed = fieldAt(m_program, reference);
        FieldReference above;
        if (std::optional<uint32_t> listedAbove = m_program.at(base).findField(listed.key))
            above = { base, *listedAbove };
        else if (listed.flags.contains(FieldFlags::IsInherited))
            above = declarationOf(m_program, reference);
        else
            return std::nullopt;
        if (above.layout == reference.layout || !isTouchable(above.layout))
            return std::nullopt;
        return m_layoutsPassedOver ? fieldNotRepeated(m_program, *m_layoutsPassedOver, m_rows.nearestLayoutsAboveThatDoNotRepeat, above) : above;
    }

    void collectUnits()
    {
        uint32_t numberOfRows = static_cast<uint32_t>(m_rows.offsets.size());
        m_unitOfRow.fill(none, numberOfRows);
        m_layoutOfRow.fill(LayoutID(), numberOfRows);
        BitVector layoutsWithLayoutsBelow(m_numberOfLayouts);
        for (LayoutID id : m_layoutsBelowTheirBases) {
            if (LayoutID base = writtenBaseOf(m_program.at(id)))
                layoutsWithLayoutsBelow.quickSet(base.index());
        }
        Vector<uint32_t> rowsWithUnits;
        for (LayoutID id : m_layoutsBelowTheirBases) {
            const Layout& layout = m_program.at(id);
            uint32_t numberOfFields = m_layoutsPassedOver ? numberOfFieldsNotRepeated(m_program, *m_layoutsPassedOver, id) : static_cast<uint32_t>(layout.fields.size());
            uint32_t firstRow = m_rows.firstRowOfLayout[id.index()];
            uint32_t depth = m_depths[id.index()];
            bool hasBase = !!writtenBaseOf(layout);
            if (!hasBase && !layoutsWithLayoutsBelow.quickGet(id.index()) && !m_families.nearestFamilyOf(id))
                continue;
            for (uint32_t field = 0; field < numberOfFields; ++field) {
                uint32_t row = firstRow + field;
                uint32_t width = widthOfRow(row);
                m_layoutOfRow[row] = id;
                if (!width)
                    continue;
                RELEASE_ASSERT(std::has_single_bit(width) && width <= widestSlotInUnionSlots);
                uint8_t logWidth = static_cast<uint8_t>(std::countr_zero(width));
                UnitIndex unit = none;
                if (hasBase) {
                    if (std::optional<FieldReference> above = findFieldAbove({ id, field }))
                        unit = m_unitOfRow[rowOf(*above)];
                }
                if (unit == none) {
                    unit = static_cast<UnitIndex>(m_units.size());
                    Unit declared;
                    declared.declaringLayout = id;
                    declared.depth = depth;
                    declared.logWidth = logWidth;
                    if (m_rows.places[row] != RowsOfSlotPlan::noPlace) {
                        declared.isPlaced = true;
                        declared.offset = static_cast<uint32_t>(m_rows.offsets[row]);
                        declared.place = m_rows.places[row];
                    }
                    m_units.append(declared);
                }
                Unit& unitOfRow = m_units[unit];
                unitOfRow.isRefused |= unitOfRow.logWidth != logWidth;
                ++unitOfRow.numberOfRows;
                m_unitOfRow[row] = unit;
                rowsWithUnits.append(row);
            }
        }
        listRowsOfUnits(rowsWithUnits.span());
    }

    void listRowsOfUnits(std::span<const uint32_t> rowsWithUnits)
    {
        uint32_t firstRow = 0;
        for (Unit& unit : m_units) {
            unit.firstRow = firstRow;
            firstRow += std::exchange(unit.numberOfRows, 0);
        }
        m_rowsOfUnits.fill(0, firstRow);
        for (uint32_t row : rowsWithUnits) {
            Unit& unit = m_units[m_unitOfRow[row]];
            m_rowsOfUnits[unit.firstRow + unit.numberOfRows++] = row;
        }
    }

    void addRowsPassedOver()
    {
        struct RowAdded {
            uint32_t row { 0 };
            uint32_t lastAddedToUnitInFront { none };
        };
        const BitVector& layoutsPassedOver = *std::exchange(m_layoutsPassedOver, nullptr);
        Vector<RowAdded> rowsAdded;
        Vector<uint32_t> lastRowsAddedToUnits;
        lastRowsAddedToUnits.fill(none, m_units.size());
        Vector<uint32_t> placesInWalk;
        placesInWalk.fill(0, m_numberOfLayouts);
        uint32_t placeInWalk = 0;
        bool declaresOrRefusesUnit = false;
        for (LayoutID id : m_layoutsBelowTheirBases) {
            placesInWalk[id.index()] = placeInWalk++;
            if (!layoutsPassedOver.get(id.index()))
                continue;
            const Layout& layout = m_program.at(id);
            uint32_t firstRow = m_rows.firstRowOfLayout[id.index()];
            for (uint32_t field = layout.ownFieldCount; field < layout.fields.size(); ++field) {
                uint32_t width = widthOfRow(firstRow + field);
                m_layoutOfRow[firstRow + field] = id;
                if (!width)
                    continue;
                std::optional<FieldReference> above = findFieldAbove({ id, field });
                UnitIndex unit = above ? m_unitOfRow[rowOf(*above)] : none;
                if (unit == none || m_units[unit].logWidth != std::countr_zero(width)) {
                    declaresOrRefusesUnit = true;
                    break;
                }
                m_unitOfRow[firstRow + field] = unit;
                rowsAdded.append({ firstRow + field, std::exchange(lastRowsAddedToUnits[unit], static_cast<uint32_t>(rowsAdded.size())) });
            }
        }
        if (declaresOrRefusesUnit) {
            m_units.shrink(0);
            m_groups.shrink(0);
            m_partsOfGroups.shrink(0);
            collectUnits();
            collectGroups();
            return;
        }

        Vector<uint32_t, 16> rowsListed;
        Vector<uint32_t, 16> rowsOfUnitAdded;
        for (uint32_t last = static_cast<uint32_t>(rowsAdded.size()); last--;) {
            UnitIndex index = m_unitOfRow[rowsAdded[last].row];
            if (lastRowsAddedToUnits[index] != last)
                continue;
            rowsOfUnitAdded.shrink(0);
            for (uint32_t added = last; added != none; added = rowsAdded[added].lastAddedToUnitInFront)
                rowsOfUnitAdded.append(rowsAdded[added].row);
            rowsListed.shrink(0);
            rowsListed.append(rowsOf(index));
            Unit& unit = m_units[index];
            unit.firstRow = static_cast<uint32_t>(m_rowsOfUnits.size());
            unit.numberOfRows = static_cast<uint32_t>(rowsListed.size() + rowsOfUnitAdded.size());
            auto placeOf = [&](uint32_t row) {
                return placesInWalk[m_layoutOfRow[row].index()];
            };
            for (uint32_t row : rowsListed) {
                while (!rowsOfUnitAdded.isEmpty() && placeOf(rowsOfUnitAdded.last()) < placeOf(row))
                    m_rowsOfUnits.append(rowsOfUnitAdded.takeLast());
                m_rowsOfUnits.append(row);
            }
            while (!rowsOfUnitAdded.isEmpty())
                m_rowsOfUnits.append(rowsOfUnitAdded.takeLast());
        }
    }

    FormInUnionSlots formProvenOfRow(uint32_t row) const
    {
        constexpr uint16_t markers = Primitive::UndefinedOrNull | Primitive::Absent;
        LayoutID id = m_layoutOfRow[row];
        const LinkedCell* cell = m_link.findCellOfField({ id, row - m_rows.firstRowOfLayout[id.index()] });
        TypeID proven = cell && cell->everyWriterIsKnown ? cell->proven : TypeID();
        if (!proven)
            return FormInUnionSlots::AnyValue;
        if (isNeverType(proven))
            return FormInUnionSlots::Unclaimed;
        Type held = m_program.at(proven);
        if (!(held.primitives & Primitive::Number) || !held.hasOnlyPrimitives(Primitive::Number | markers))
            return FormInUnionSlots::AnyValue;
        return held.primitives & Primitive::Number & ~Primitive::Integer ? FormInUnionSlots::Number : FormInUnionSlots::Integer;
    }

    FormInUnionSlots formOf(UnitIndex index)
    {
        Unit& unit = m_units[index];
        if (unit.form != FormInUnionSlots::Unknown)
            return unit.form;
        FormInUnionSlots form = FormInUnionSlots::Unclaimed;
        for (uint32_t row : rowsOf(index))
            form = std::max(form, formProvenOfRow(row));
        unit.form = form == FormInUnionSlots::Unclaimed ? FormInUnionSlots::AnyValue : form;
        return unit.form;
    }

    void collectGroups()
    {
        const TypeTable& types = m_program.types;
        uint32_t numberOfFamilies = m_families.size();
        auto forEachNameOf = [&](LayoutID member, NOESCAPE const Invocable<void(uint32_t key, UnitIndex)> auto& functor) {
            if (!isTouchable(member) || !m_hasCells(member))
                return;
            const Layout& layout = m_program.at(member);
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                PropertyKeyID key = layout.fields[field].key;
                UnitIndex unit = m_unitOfRow[rowOf({ member, field })];
                if (!key || !types.hasStableNumber(key) || unit == none || m_units[unit].isRefused || layout.findField(key) != field)
                    continue;
                functor(key.index(), unit);
            }
        };

        Vector<uint32_t> firstMemberOutsideSmallerFamilies;
        firstMemberOutsideSmallerFamilies.fill(0, numberOfFamilies + 2);
        for (uint32_t layout = 0; layout < m_numberOfLayouts; ++layout) {
            if (FamilyID family = m_families.nearestFamilyOf(LayoutID(layout)))
                ++firstMemberOutsideSmallerFamilies[family.index() + 2];
        }
        for (uint32_t family = 0; family < numberOfFamilies; ++family)
            firstMemberOutsideSmallerFamilies[family + 2] += firstMemberOutsideSmallerFamilies[family + 1];
        Vector<LayoutID> membersOutsideSmallerFamilies;
        membersOutsideSmallerFamilies.grow(firstMemberOutsideSmallerFamilies.last());
        for (uint32_t layout = 0; layout < m_numberOfLayouts; ++layout) {
            if (FamilyID family = m_families.nearestFamilyOf(LayoutID(layout)))
                membersOutsideSmallerFamilies[firstMemberOutsideSmallerFamilies[family.index() + 1]++] = LayoutID(layout);
        }

        Vector<Vector<NameInFamily>> namesOfFamiliesInside(numberOfFamilies);
        BitVector familiesThatOneFamilyInsideLeftNamesWithOneUnitEach(numberOfFamilies);
        for (uint32_t family = 0; family < numberOfFamilies; ++family) {
            Vector<NameInFamily> names = std::exchange(namesOfFamiliesInside[family], { });
            size_t numberFromInside = names.size();
            for (LayoutID member : membersOutsideSmallerFamilies.span().subspan(firstMemberOutsideSmallerFamilies[family], firstMemberOutsideSmallerFamilies[family + 1] - firstMemberOutsideSmallerFamilies[family])) {
                forEachNameOf(member, [&](uint32_t key, UnitIndex unit) {
                    names.append(NameInFamily { key, { unit, 1, false } });
                });
            }
            bool isAsFamilyInsideLeftIt = names.size() == numberFromInside && familiesThatOneFamilyInsideLeftNamesWithOneUnitEach.quickGet(family);
            if (!isAsFamilyInsideLeftIt) {
                AOT::sort(names, [](const NameInFamily& a, const NameInFamily& b) {
                    return std::tuple(a.key, a.part.isGroup, a.part.index) < std::tuple(b.key, b.part.isGroup, b.part.index);
                });
            }

            bool someNameHasGroup = false;
            size_t numberKept = isAsFamilyInsideLeftIt ? names.size() : 0;
            for (size_t begin = numberKept; begin < names.size();) {
                size_t end = begin;
                size_t endOfNamed = begin;
                for (; end < names.size() && names[end].key == names[begin].key; ++end) {
                    NameInFamily name = names[end];
                    if (endOfNamed > begin && !name.part.isGroup && names[endOfNamed - 1].part.index == name.part.index)
                        names[endOfNamed - 1].part.numberOfRows += name.part.numberOfRows;
                    else
                        names[endOfNamed++] = name;
                }
                std::span<NameInFamily> named = names.mutableSpan().subspan(begin, endOfNamed - begin);
                begin = end;
                if (named.size() == 1 && !named[0].part.isGroup) {
                    names[numberKept++] = named[0];
                    continue;
                }
                auto classOf = [&](const PartOfGroup& part) {
                    return part.isGroup ? std::tuple(m_groups[part.index].logWidth, m_groups[part.index].form) : std::tuple(m_units[part.index].logWidth, formOf(part.index));
                };
                AOT::sort(named, [&](const NameInFamily& a, const NameInFamily& b) {
                    return std::tuple_cat(classOf(a.part), std::tuple(a.part.isGroup, a.part.index)) < std::tuple_cat(classOf(b.part), std::tuple(b.part.isGroup, b.part.index));
                });
                for (std::span<const NameInFamily> rest = named; !rest.empty();) {
                    size_t size = std::ranges::find_if(rest, [&](const NameInFamily& other) {
                        return classOf(other.part) != classOf(rest[0].part);
                    }) - rest.begin();
                    NameInFamily kept = rest[0];
                    if (size > 1 || kept.part.isGroup) {
                        Group group;
                        group.firstPart = static_cast<uint32_t>(m_partsOfGroups.size());
                        group.numberOfParts = static_cast<uint32_t>(size);
                        std::tie(group.logWidth, group.form) = classOf(kept.part);
                        for (const NameInFamily& name : rest.first(size)) {
                            m_partsOfGroups.append(name.part);
                            group.depthOfDeepestDeclaration = std::max(group.depthOfDeepestDeclaration, name.part.isGroup ? m_groups[name.part.index].depthOfDeepestDeclaration : m_units[name.part.index].depth);
                            group.numberOfRows += name.part.isGroup ? m_groups[name.part.index].numberOfRows : name.part.numberOfRows;
                        }
                        kept.part = PartOfGroup { static_cast<uint32_t>(m_groups.size()), 0, true };
                        m_groups.append(group);
                        someNameHasGroup = true;
                    }
                    rest = rest.subspan(size);
                    names[numberKept++] = kept;
                }
            }
            names.shrink(numberKept);

            if (FamilyID above = m_families.nearestFamilyAbove(FamilyID(family))) {
                Vector<NameInFamily>& namesAbove = namesOfFamiliesInside[above.index()];
                familiesThatOneFamilyInsideLeftNamesWithOneUnitEach.quickSet(above.index(), namesAbove.isEmpty() && !someNameHasGroup);
                if (namesAbove.isEmpty())
                    namesAbove = WTF::move(names);
                else
                    namesAbove.appendVector(names);
            }
        }
        for (size_t index = m_groups.size(); index--;) {
            Group& group = m_groups[index];
            group.numberOfRowsInWidestFamily = std::max(group.numberOfRowsInWidestFamily, group.numberOfRows);
            for (const PartOfGroup& part : m_partsOfGroups.span().subspan(group.firstPart, group.numberOfParts)) {
                if (part.isGroup)
                    m_groups[part.index].numberOfRowsInWidestFamily = std::max(m_groups[part.index].numberOfRowsInWidestFamily, group.numberOfRowsInWidestFamily);
            }
        }
        m_groupsInOrder.shrink(0);
        for (uint32_t group = 0; group < m_groups.size(); ++group)
            m_groupsInOrder.append(group);
        AOT::stableSort(m_groupsInOrder, [&](uint32_t a, uint32_t b) {
            if (m_groups[a].depthOfDeepestDeclaration != m_groups[b].depthOfDeepestDeclaration)
                return m_groups[a].depthOfDeepestDeclaration < m_groups[b].depthOfDeepestDeclaration;
            return m_rows.haveFinalWidths && m_groups[a].numberOfRowsInWidestFamily > m_groups[b].numberOfRowsInWidestFamily;
        });
    }

    ALWAYS_INLINE CellInUnionSlots& cellOf(LayoutID id)
    {
        CellInUnionSlots& cell = m_cells[id.index()];
        if (!cell.isInitialized()) [[unlikely]]
            initializeCellOf(id, cell);
        return cell;
    }

    NEVER_INLINE void initializeCellOf(LayoutID id, CellInUnionSlots& cell)
    {
        uint32_t firstRow = m_rows.firstRowOfLayout[id.index()];
        uint32_t endOfRows = m_rows.firstRowOfLayout[id.index() + 1];
        UnitList units;
        for (uint32_t row = firstRow; row < endOfRows; ++row) {
            if (m_unitOfRow[row] != none)
                units.append(m_unitOfRow[row]);
        }
        AOT::sort(units);
        units.shrink(units.size() - std::ranges::unique(units).size());

        WidthCountsInUnionSlots unplaced { };
        uint32_t bytes = 0;
        for (UnitIndex unit : units) {
            bytes += 1u << m_units[unit].logWidth;
            if (!m_units[unit].isPlaced)
                ++unplaced[m_units[unit].logWidth];
        }
        uint32_t firstByte = offsetOfFirstSlotOf(id);
        uint32_t limit = roundUpToWidestSlotInUnionSlots(firstByte + bytes);
        for (UnitIndex unit : units) {
            if (m_units[unit].isPlaced)
                limit = std::max(limit, roundUpToWidestSlotInUnionSlots(m_units[unit].offset + (1u << m_units[unit].logWidth)));
        }
        if (standsByItself(id))
            limit = static_cast<uint32_t>(WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(limit));
        cell.initialize(firstByte, limit, unplaced);
        for (UnitIndex unit : units) {
            if (m_units[unit].isPlaced)
                cell.occupy(m_units[unit].offset, m_units[unit].logWidth);
        }

    }

    CellInUnionSlots& cellOfRow(uint32_t row) { return cellOf(m_layoutOfRow[row]); }

    bool fitsEveryRow(UnitIndex unit, uint32_t offset)
    {
        unsigned logWidth = m_units[unit].logWidth;
        return std::ranges::all_of(rowsOf(unit), [&](uint32_t row) {
            return cellOfRow(row).canPlace(offset, logWidth);
        });
    }

    void place(UnitIndex index, uint32_t offset, uint32_t placeSharedWith)
    {
        Unit& unit = m_units[index];
        RELEASE_ASSERT(!unit.isPlaced && !unit.isRefused);
        unit.offset = offset;
        unit.place = placeSharedWith != RowsOfSlotPlan::noPlace ? placeSharedWith : m_addPlace();
        LayoutID lastLayout;
        for (uint32_t row : rowsOf(index)) {
            if (std::exchange(lastLayout, m_layoutOfRow[row]) != m_layoutOfRow[row])
                cellOfRow(row).place(offset, unit.logWidth);
            RELEASE_ASSERT(m_rows.places[row] == RowsOfSlotPlan::noPlace);
            m_rows.offsets[row] = offset;
            m_rows.places[row] = unit.place;
        }
        unit.isPlaced = true;
        m_isTouched.quickSet(unit.declaringLayout.index());
    }

    uint32_t placeInUseAt(std::span<const UnitIndex> unitsOfGroup, uint32_t offset) const
    {
        for (UnitIndex unit : unitsOfGroup) {
            if (m_units[unit].isPlaced && m_units[unit].offset == offset)
                return m_units[unit].place;
        }
        return RowsOfSlotPlan::noPlace;
    }

    bool tryPlace(UnitIndex unit, uint32_t offset, uint32_t& placeAtOffset)
    {
        if (!fitsEveryRow(unit, offset))
            return false;
        place(unit, offset, placeAtOffset);
        placeAtOffset = m_units[unit].place;
        return true;
    }

    void placeUnitsThatFitAt(uint32_t offset, UnitList& unplaced, std::span<const UnitIndex> unitsOfGroup)
    {
        uint32_t placeAtOffset = placeInUseAt(unitsOfGroup, offset);
        unplaced.removeAllMatching([&](UnitIndex unit) {
            return tryPlace(unit, offset, placeAtOffset);
        });
    }

    std::optional<uint32_t> findLowestOffsetThatFitsAll(std::span<const uint32_t> rows, unsigned logSize)
    {
        uint32_t offset = 0;
        size_t numberOfRowsInAgreement = 0;
        for (size_t index = 0; numberOfRowsInAgreement < rows.size(); index = index + 1 == rows.size() ? 0 : index + 1) {
            std::optional<uint32_t> next = cellOfRow(rows[index]).findOffset(logSize, offset);
            if (!next)
                return std::nullopt;
            numberOfRowsInAgreement = *next == offset ? numberOfRowsInAgreement + 1 : 1;
            offset = *next;
        }
        return offset;
    }

    bool placeByItself(UnitIndex unit)
    {
        if (waitsForUnitsDeclaredAbove(unit))
            return false;
        std::optional<uint32_t> offset = findLowestOffsetThatFitsAll(rowsOf(unit), m_units[unit].logWidth);
        if (!offset)
            return false;
        place(unit, *offset, RowsOfSlotPlan::noPlace);
        return true;
    }

    void sortIntoPlacementOrder(UnitList& units) const
    {
        AOT::sort(units, [&](UnitIndex a, UnitIndex b) {
            return std::tuple(m_units[a].depth, a) < std::tuple(m_units[b].depth, b);
        });
    }

    void placeUnitsDeclaredAbove(std::span<const UnitIndex> units)
    {
        UnitList above;
        auto collectAbove = [&](UnitIndex unit) {
            uint32_t depth = m_units[unit].depth;
            LayoutID layout = m_units[unit].declaringLayout;
            if (!depth || m_depthsReached[layout.index()] >= depth)
                return;
            m_depthsReached[layout.index()] = depth;
            for (uint32_t row = m_rows.firstRowOfLayout[layout.index()]; row < m_rows.firstRowOfLayout[layout.index() + 1]; ++row) {
                UnitIndex other = m_unitOfRow[row];
                if (other == none)
                    continue;
                Unit& candidate = m_units[other];
                if (candidate.isPlaced || candidate.isRefused || candidate.isQueued || candidate.depth >= depth)
                    continue;
                candidate.isQueued = true;
                above.append(other);
            }
        };
        for (UnitIndex unit : units)
            collectAbove(unit);
        sortIntoPlacementOrder(above);
        for (UnitIndex unit : above)
            placeByItself(unit);
    }

    bool waitsForUnitsDeclaredAbove(UnitIndex unit)
    {
        const Unit& asked = m_units[unit];
        LayoutID layout = asked.declaringLayout;
        if (!asked.depth || m_allAboveIsPlaced.quickGet(layout.index()))
            return false;
        for (uint32_t row = m_rows.firstRowOfLayout[layout.index()]; row < m_rows.firstRowOfLayout[layout.index() + 1]; ++row) {
            UnitIndex other = m_unitOfRow[row];
            if (other != none && !m_units[other].isPlaced && m_units[other].depth < asked.depth)
                return true;
        }
        m_allAboveIsPlaced.quickSet(layout.index());
        return false;
    }

    void tryPlaceAtOneOffset(UnitList& unplaced, std::span<const OffsetUseInUnionSlots> offsetsInUse, std::span<const UnitIndex> unitsOfGroup)
    {
        if (unplaced.isEmpty())
            return;
        std::optional<uint32_t> best;
        if (!offsetsInUse.empty()) {
            bool fitsAll = std::ranges::all_of(unplaced, [&](UnitIndex unit) {
                return fitsEveryRow(unit, offsetsInUse[0].offset);
            });
            if (fitsAll)
                best = offsetsInUse[0].offset;
        } else {
            Vector<uint32_t, 16> rows;
            for (UnitIndex unit : unplaced)
                rows.append(rowsOf(unit));
            best = findLowestOffsetThatFitsAll(rows.span(), m_units[unplaced[0]].logWidth);
        }
        if (best)
            placeUnitsThatFitAt(*best, unplaced, unitsOfGroup);
    }

    void placeAtMostSharedOffsets(UnitList& unplaced, std::span<const UnitIndex> unitsOfGroup)
    {
        if (unplaced.size() < 2)
            return;
        unsigned logSize = m_units[unplaced[0]].logWidth;
        struct OffsetsOfUnit {
            UnitIndex unit { 0 };
            size_t firstWord { 0 };
            size_t numberOfWords { 0 };
        };
        Vector<OffsetsOfUnit, 8> unitsLeft;
        Vector<uint64_t, 64> fittingOffsets;
        size_t mostWords = 0;
        for (UnitIndex unit : unplaced) {
            uint32_t leastLimit = UINT32_MAX;
            for (uint32_t row : rowsOf(unit))
                leastLimit = std::min(leastLimit, cellOfRow(row).limit());
            size_t numberOfOffsets = std::max(offsetOfFirstField, leastLimit) >> logSize;
            size_t numberOfWords = (numberOfOffsets + 63) / 64;
            size_t firstWord = fittingOffsets.size();
            fittingOffsets.insertFill(firstWord, UINT64_MAX, numberOfWords);
            std::span<uint64_t> words = fittingOffsets.mutableSpan().subspan(firstWord);
            words[0] &= UINT64_MAX << (offsetOfFirstField >> logSize);
            if (numberOfOffsets % 64)
                words.back() &= (1ull << (numberOfOffsets % 64)) - 1;
            for (uint32_t row : rowsOf(unit))
                cellOfRow(row).keepPlaceableOffsets(words, logSize);
            unitsLeft.append(OffsetsOfUnit { unit, firstWord, numberOfWords });
            mostWords = std::max(mostWords, numberOfWords);
        }
        auto countRows = [&](std::span<const OffsetsOfUnit> units) {
            CountsInUnionSlots counts(mostWords);
            for (const OffsetsOfUnit& offsets : units)
                counts.add(fittingOffsets.subspan(offsets.firstWord, offsets.numberOfWords), m_units[offsets.unit].numberOfRows);
            return counts;
        };
        Vector<uint32_t, 64> rowsThatFit(FillWith { }, mostWords * 64, 0u);
        countRows(unitsLeft.span()).forEachCount([&](size_t position, uint32_t count) {
            rowsThatFit[position] += count;
        });

        Vector<OffsetsOfUnit, 8> unitsPlaced;
        while (true) {
            size_t best = std::ranges::max_element(rowsThatFit) - rowsThatFit.begin();
            if (rowsThatFit[best] < 2)
                break;
            uint32_t offset = static_cast<uint32_t>(best) << logSize;
            uint32_t placeAtOffset = placeInUseAt(unitsOfGroup, offset);
            unitsPlaced.shrink(0);
            unitsLeft.removeAllMatching([&](const OffsetsOfUnit& offsets) {
                if (best / 64 >= offsets.numberOfWords)
                    return false;
                uint64_t& word = fittingOffsets[offsets.firstWord + best / 64];
                uint64_t bit = 1ull << (best % 64);
                if (!(word & bit))
                    return false;
                if (!tryPlace(offsets.unit, offset, placeAtOffset)) {
                    word &= ~bit;
                    rowsThatFit[best] -= m_units[offsets.unit].numberOfRows;
                    return false;
                }
                unitsPlaced.append(offsets);
                return true;
            });
            if (unitsPlaced.size() < unitsLeft.size()) {
                countRows(unitsPlaced.span()).forEachCount([&](size_t position, uint32_t count) {
                    rowsThatFit[position] -= count;
                });
            } else {
                rowsThatFit.fill(0);
                countRows(unitsLeft.span()).forEachCount([&](size_t position, uint32_t count) {
                    rowsThatFit[position] += count;
                });
            }
        }
        unplaced.removeAllMatching([&](UnitIndex unit) {
            return m_units[unit].isPlaced;
        });
    }

    void placeAtSharedOffsets(Group& group)
    {
        Vector<RowsOfUnit, 16> rows;
        for (const PartOfGroup& part : m_partsOfGroups.span().subspan(group.firstPart, group.numberOfParts)) {
            if (part.isGroup)
                rows.append(m_unitsLeftByGroups.span().subspan(m_groups[part.index].firstUnitLeft, m_groups[part.index].numberOfUnitsLeft));
            else
                rows.append(RowsOfUnit { part.index, part.numberOfRows });
        }
        auto uniteRowsWhere = [&](NOESCAPE const Invocable<bool(UnitIndex, UnitIndex)> auto& standsFor) {
            size_t numberKept = 0;
            for (RowsOfUnit ofUnit : rows) {
                if (numberKept && standsFor(rows[numberKept - 1].unit, ofUnit.unit))
                    rows[numberKept - 1].numberOfRows += ofUnit.numberOfRows;
                else
                    rows[numberKept++] = ofUnit;
            }
            rows.shrink(numberKept);
        };
        AOT::sort(rows, [](const RowsOfUnit& a, const RowsOfUnit& b) {
            return a.unit < b.unit;
        });
        uniteRowsWhere([](UnitIndex kept, UnitIndex next) {
            return kept == next;
        });

        placeAtSharedOffsets(rows.span());

        auto offsetOrNone = [&](UnitIndex unit) {
            return m_units[unit].isPlaced ? static_cast<uint64_t>(m_units[unit].offset) : std::numeric_limits<uint64_t>::max();
        };
        AOT::sort(rows, [&](const RowsOfUnit& a, const RowsOfUnit& b) {
            return std::tuple(offsetOrNone(a.unit), a.unit) < std::tuple(offsetOrNone(b.unit), b.unit);
        });
        uniteRowsWhere([&](UnitIndex kept, UnitIndex next) {
            return m_units[next].isPlaced && offsetOrNone(kept) == offsetOrNone(next);
        });
        group.firstUnitLeft = static_cast<uint32_t>(m_unitsLeftByGroups.size());
        group.numberOfUnitsLeft = static_cast<uint32_t>(rows.size());
        m_unitsLeftByGroups.appendVector(rows);
    }

    void placeAtSharedOffsets(std::span<const RowsOfUnit> rows)
    {
        UnitList unplaced;
        for (const RowsOfUnit& ofUnit : rows) {
            if (!m_units[ofUnit.unit].isPlaced)
                unplaced.append(ofUnit.unit);
        }
        if (unplaced.isEmpty())
            return;
        UnitList unitsOfGroup;
        for (const RowsOfUnit& ofUnit : rows)
            unitsOfGroup.append(ofUnit.unit);

        placeUnitsDeclaredAbove(unplaced.span());
        unplaced.removeAllMatching([&](UnitIndex unit) {
            return m_units[unit].isPlaced || waitsForUnitsDeclaredAbove(unit);
        });

        Vector<OffsetUseInUnionSlots, 8> offsets;
        for (const RowsOfUnit& ofUnit : rows) {
            if (m_units[ofUnit.unit].isPlaced)
                offsets.append(OffsetUseInUnionSlots { m_units[ofUnit.unit].offset, ofUnit.numberOfRows });
        }
        Vector<OffsetUseInUnionSlots, 8> offsetsInUse = countOffsetUsesInUnionSlots(WTF::move(offsets));
        if (offsetsInUse.size() <= 1)
            tryPlaceAtOneOffset(unplaced, offsetsInUse.span(), unitsOfGroup);
        for (OffsetUseInUnionSlots use : offsetsInUse)
            placeUnitsThatFitAt(use.offset, unplaced, unitsOfGroup);
        placeAtMostSharedOffsets(unplaced, unitsOfGroup);
    }

    template<typename Functor>
    static void forEachFieldInDeclaredOrder(const Layout& layout, const Functor& functor)
    {
        uint32_t numberOfFields = static_cast<uint32_t>(layout.fields.size());
        BitVector isListed(numberOfFields);
        for (uint32_t field : layout.declaredOrder) {
            if (field < numberOfFields && !isListed.quickSet(field))
                functor(field);
        }
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            if (!isListed.quickGet(field))
                functor(field);
        }
    }

    void fillLayoutsThatWereTouched()
    {
        for (LayoutID id : m_layoutsBelowTheirBases) {
            if (!m_isTouched.quickGet(id.index()))
                continue;
            forEachFieldInDeclaredOrder(m_program.at(id), [&](uint32_t field) {
                UnitIndex unit = m_unitOfRow[rowOf({ id, field })];
                if (unit != none && m_units[unit].declaringLayout == id && !m_units[unit].isPlaced && !m_units[unit].isRefused)
                    placeByItself(unit);
            });
        }
    }

    void finishLayoutsThatWereTouched()
    {
        for (size_t index : m_isTouched)
            m_rows.ends[index] = std::max<uint64_t>(m_rows.ends[index], cellOf(LayoutID(index)).end());
    }

    const Program& m_program;
    const LinkTables& m_link;
    const Families& m_families;
    const RowsOfSlotPlan& m_rows;
    const ScopedLambda<bool(LayoutID)>& m_hasCells;
    const ScopedLambda<uint32_t()>& m_addPlace;
    uint32_t m_numberOfLayouts;
    const Vector<uint32_t>& m_depths;
    const Vector<LayoutID>& m_layoutsBelowTheirBases;
    const BitVector* m_layoutsPassedOver;
    BitVector m_isTouched;
    Vector<CellInUnionSlots> m_cells;
    Vector<uint32_t> m_depthsReached;
    BitVector m_allAboveIsPlaced;
    Vector<UnitIndex> m_unitOfRow;
    Vector<LayoutID> m_layoutOfRow;
    Vector<Unit> m_units;
    Vector<uint32_t> m_rowsOfUnits;
    Vector<Group> m_groups;
    Vector<uint32_t> m_groupsInOrder;
    Vector<PartOfGroup> m_partsOfGroups;
    Vector<RowsOfUnit> m_unitsLeftByGroups;
};

} // anonymous namespace

LayoutsBelowTheirBases orderLayoutsBelowTheirBases(const Program& program)
{
    constexpr uint32_t none = LayoutsBelowTheirBases::isNotTouched;
    constexpr uint32_t unseen = UINT32_MAX - 1;
    constexpr uint32_t onTheWay = UINT32_MAX - 2;
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    LayoutsBelowTheirBases result;
    result.depths.fill(unseen, numberOfLayouts);
    Vector<uint32_t, 16> row;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        row.shrink(0);
        LayoutID above(index);
        for (; above && result.depths[above.index()] == unseen; above = writtenBaseOf(program.at(above))) {
            result.depths[above.index()] = onTheWay;
            row.append(above.index());
        }
        uint32_t depthAbove = above ? result.depths[above.index()] : 0;
        bool mayBeTouched = !above || (depthAbove != onTheWay && depthAbove != none);
        uint32_t depth = above && mayBeTouched ? depthAbove + 1 : 0;
        while (!row.isEmpty()) {
            uint32_t layoutIndex = row.takeLast();
            const Layout& layout = program.layouts[layoutIndex];
            mayBeTouched &= isObjectLayout(layout.kind) && (!writtenBaseOf(layout) || layout.inheritedFields == InheritedFields::AllOfRow);
            result.depths[layoutIndex] = mayBeTouched ? depth++ : none;
            if (mayBeTouched)
                result.layouts.append(LayoutID(layoutIndex));
        }
    }
    return result;
}

void placeFieldsOfUnions(const Program& program, const LinkTables& link, const Families& families, const LayoutsBelowTheirBases& layoutsBelowTheirBases, RowsOfSlotPlan rows, NOESCAPE const ScopedLambda<bool(LayoutID)>& hasCells, NOESCAPE const ScopedLambda<uint32_t()>& addPlace)
{
    PlacerOfUnionSlots(program, link, families, layoutsBelowTheirBases, rows, hasCells, addPlace).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
