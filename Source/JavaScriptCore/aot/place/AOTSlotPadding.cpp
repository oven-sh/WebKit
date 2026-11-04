/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlanSteps.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

inline constexpr SiteWeight minAccessWeightPerEmptySlot = uint64_t { 1 } << siteWeightFractionBits;

void ensureSizeReaches(PackedLayouts& packed, LayoutID layout, uint32_t end)
{
    uint32_t& size = packed.sizes[layout.index()];
    size = std::max(size, WTF::roundUpToMultipleOf(16, end));
    RELEASE_ASSERT(size <= packed.inlineLimits[layout.index()]);
}

struct PaddingUnit {
    std::span<const SlotRow> rows;
    SiteWeight weight { 0 };
};

class PaddingWidener {
public:
    PaddingWidener(const SlotPlanContext&, const SlotUses&, const JoinedRows&, std::span<const SlotClaim>, WidenedRepresentations, PackedLayouts&);

    void run();

private:
    static constexpr uint32_t narrow = widthOf(SlotRepresentation::OptionalInt32);
    static constexpr uint32_t wide = widthOf(SlotRepresentation::OptionalInt64);
    static_assert(widthOf(SlotRepresentation::Cell32) == narrow && widthOf(SlotRepresentation::OptionalCell32) == narrow);
    static_assert(widthOf(SlotRepresentation::Cell) == wide && widthOf(SlotRepresentation::JSValue) == wide);
    static_assert(!(offsetOfFirstField % wide) && !(offsetOfFirstEnvironmentVariable % wide));
    static_assert(widthOf(SlotRepresentation::Int32) == narrow);

    // Nobody reads the variable of an environment by name.
    bool hasSlots(LayoutID id) const { return m_context.isCompiledObject(id) || (m_context.isEnvironment(id) && m_kind != WidenedRepresentations::SmallIntegers); }
    bool hasWidthToWiden(const PlacedSlot& slot) const { return m_kind == WidenedRepresentations::SmallIntegers ? slot.width() < narrow : slot.width() == narrow; }
    // A small integer takes only bytes that the cell has as it is.
    uint32_t limitOf(LayoutID id) const { return m_kind == WidenedRepresentations::SmallIntegers ? m_packed.sizes[id.index()] : m_packed.inlineLimits[id.index()]; }
    uint32_t firstSlotOffsetOf(LayoutID id) const { return m_context.isEnvironment(id) ? offsetOfFirstEnvironmentVariable : offsetOfFirstField; }
    LayoutID layoutOf(SlotRow row) const { return m_layoutOfRow[row]; }
    uint32_t offsetOf(const PaddingUnit& unit) const { return m_packed.slots[unit.rows[0]].offset; }
    uint32_t currentWidthOf(const PaddingUnit& unit) const { return m_packed.slots[unit.rows[0]].width(); }

    const Type& contentsOf(SlotRow) const;
    bool holdsEnvironmentOfDeferredClosures(SlotRow) const;
    bool isNarrow(SlotRepresentation) const;
    bool holdsUndefinedOrNull(const PaddingUnit&) const;
    SiteWeight weightOfAccessesByName(SlotRow) const;
    std::optional<SlotRepresentation> widenedRepresentationOf(const PaddingUnit&) const;
    void createUnits();
    bool isCandidate(const PaddingUnit&) const;
    void findCandidates();
    void weighCandidatesByName();
    void findUnitGroups();
    void recordOccupiedBytes();
    void orderCandidates();
    std::optional<uint32_t> roomFor(const PaddingUnit&);
    std::optional<SlotRow> findRowThatMayMove(LayoutID, uint32_t offset) const;
    std::optional<uint32_t> findFreeHalfOutside(LayoutID, uint32_t word) const;
    std::optional<uint32_t> makeRoomInOwnWord(const PaddingUnit&);
    void move(const PaddingUnit&, uint32_t offset);
    std::optional<uint32_t> findRoomFor(const PaddingUnit&, bool mayMoveOtherSlots);
    void widenAt(uint32_t unit, uint32_t offset, SlotRepresentation);
    bool widen(uint32_t unit, bool mayMoveOtherSlots);

    // Units that some access reaches together. The access uses one load as long as they share a representation, so they are widened together or not at all.
    struct UnitGroup {
        Vector<uint32_t, 4> units;
        SlotRepresentation widened { SlotRepresentation::JSValue };
        bool staysNarrow { false };
    };
    static constexpr uint32_t noUnitGroup = UINT32_MAX;

    const SlotPlanContext& m_context;
    const SlotUses& m_uses;
    const JoinedRows& m_joinedRows;
    std::span<const SlotClaim> m_claims;
    WidenedRepresentations m_kind;
    uint32_t m_widenedWidth;
    PackedLayouts& m_packed;

    Vector<LayoutID> m_layoutOfRow;
    Vector<SlotRow> m_unsharedRows;
    Vector<PaddingUnit> m_units;
    Vector<uint32_t> m_candidates;
    Vector<UnitGroup> m_unitGroups;
    Vector<uint32_t> m_groupOfUnit;
    Vector<OccupiedBytes> m_occupied;
    Vector<uint32_t> m_firstPossiblyFreeOffset;
    BitVector m_isWidened;
};

PaddingWidener::PaddingWidener(const SlotPlanContext& context, const SlotUses& uses, const JoinedRows& joinedRows, std::span<const SlotClaim> claims, WidenedRepresentations kind, PackedLayouts& packed)
    : m_context(context)
    , m_uses(uses)
    , m_joinedRows(joinedRows)
    , m_claims(claims)
    , m_kind(kind)
    , m_widenedWidth(kind == WidenedRepresentations::SmallIntegers ? narrow : wide)
    , m_packed(packed)
{
}

const Type& PaddingWidener::contentsOf(SlotRow row) const
{
    const Program& program = m_context.program;
    FieldReference declaration = declarationOf(program, { layoutOf(row), m_context.rows.fieldOf(row) });
    return program.at(slotTypeOf(program, fieldAt(program, declaration)));
}

// The layout table hands the slot to the run time as a narrow reference in the cell, beside each field whose closure is made over it.
bool PaddingWidener::holdsEnvironmentOfDeferredClosures(SlotRow row) const
{
    const Program& program = m_context.program;
    if (!m_context.isCompiledObject(layoutOf(row)))
        return false;
    FieldReference declaration = declarationOf(program, { layoutOf(row), m_context.rows.fieldOf(row) });
    return fieldAt(program, declaration).flags.contains(FieldFlags::IsHidden) && contentsOf(row).environment;
}

bool PaddingWidener::isNarrow(SlotRepresentation representation) const
{
    switch (m_kind) {
    case WidenedRepresentations::Numbers:
        return representation == SlotRepresentation::OptionalInt32;
    case WidenedRepresentations::References:
        return representation == SlotRepresentation::Cell32 || representation == SlotRepresentation::OptionalCell32;
    case WidenedRepresentations::SmallIntegers:
        return representation == SlotRepresentation::Uint8 || representation == SlotRepresentation::Int8 || representation == SlotRepresentation::Uint16 || representation == SlotRepresentation::Int16;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool PaddingWidener::holdsUndefinedOrNull(const PaddingUnit& unit) const
{
    return std::ranges::any_of(unit.rows, [&](SlotRow row) {
        return contentsOf(row).mayBeUndefinedOrNull();
    });
}

SiteWeight PaddingWidener::weightOfAccessesByName(SlotRow row) const
{
    const Program& program = m_context.program;
    PropertyKeyID key = fieldAt(program, declarationOf(program, { layoutOf(row), m_context.rows.fieldOf(row) })).key;
    return key ? m_uses.keyWeights[key.index()] : SiteWeight { 0 };
}

std::optional<SlotRepresentation> PaddingWidener::widenedRepresentationOf(const PaddingUnit& unit) const
{
    if (m_kind == WidenedRepresentations::SmallIntegers)
        return SlotRepresentation::Int32;
    RepresentationChoice choice { .prefersInt32 = false, .holdsWideReferences = m_kind == WidenedRepresentations::References };
    uint16_t absent = 0;
    for (SlotRow row : unit.rows) {
        choice.holdsWideReferences |= holdsWideReferences(m_context, layoutOf(row));
        if (m_claims[row].needsAbsentState)
            absent = Primitive::Absent;
    }
    const Type& first = contentsOf(unit.rows[0]);
    if (unit.rows.size() == 1)
        return representationFor(m_context, absent ? first.withPrimitives(absent) : first, choice);

    Type contents = Type::fromPrimitives(absent);
    for (SlotRow row : unit.rows) {
        const Type& type = contentsOf(row);
        if (!type.isJavaScriptValue())
            return std::nullopt;
        contents = contentsOfSharedSlot(contents, type);
    }
    return representationFor(m_context, contents, choice);
}

void PaddingWidener::createUnits()
{
    const SlotRows& rows = m_context.rows;
    auto& sharedLocations = m_packed.sharedLocations;
    m_layoutOfRow.fill(LayoutID { }, rows.count());
    for (uint32_t index = 0; index < m_context.program.layouts.size(); ++index) {
        LayoutID layout(index);
        if (!hasSlots(layout))
            continue;
        for (FieldIndex field = 0; field < rows.numberOfFieldsOf(layout); ++field) {
            SlotRow row = rows.rowOf(layout, field);
            const PlacedSlot& slot = m_packed.slots[row];
            if (slot.storage != SlotStorage::Inline || !hasWidthToWiden(slot))
                continue;
            m_layoutOfRow[row] = layout;
            if (m_packed.sharedLocationOfRow[row] == noSharedLocation)
                m_unsharedRows.append(row);
        }
    }
    m_units.reserveInitialCapacity(sharedLocations.size() + m_unsharedRows.size());
    for (const SharedLocation& location : sharedLocations)
        m_units.append(PaddingUnit { location.rows.span(), location.weight });
    for (size_t i = 0; i < m_unsharedRows.size(); ++i) {
        const SlotRowUse& use = m_uses.rows[m_unsharedRows[i]];
        m_units.append(PaddingUnit { m_unsharedRows.subspan(i, 1), saturatedSum(use.accessWeight, use.environmentUses) });
    }
}

// A typed access is as fast at either width.
void PaddingWidener::weighCandidatesByName()
{
    for (uint32_t index : m_candidates) {
        PaddingUnit& unit = m_units[index];
        unit.weight = 0;
        for (SlotRow row : unit.rows)
            unit.weight = greaterOf(unit.weight, weightOfAccessesByName(row));
    }
}

bool PaddingWidener::isCandidate(const PaddingUnit& unit) const
{
    const PlacedSlot& first = m_packed.slots[unit.rows[0]];
    bool everyRowIsNarrow = std::ranges::all_of(unit.rows, [&](SlotRow row) {
        const PlacedSlot& slot = m_packed.slots[row];
        RELEASE_ASSERT(slot.hasLocationOf(first));
        if (slot.storage != SlotStorage::Inline || !isNarrow(slot.representation) || m_packed.rowsWithEmptySlotElsewhere.get(row) || holdsEnvironmentOfDeferredClosures(row))
            return false;
        return m_kind != WidenedRepresentations::References || !(contentsOf(row).primitives & (Primitive::AnyString | Primitive::AnySymbol));
    });
    if (!everyRowIsNarrow)
        return false;
    // Where undefined or null is a value of the field, a read takes the marker along, and the markers of the narrow slot are turned into those of the wide form and back at each access.
    if (m_kind == WidenedRepresentations::Numbers && !holdsUndefinedOrNull(unit))
        return false;
    auto widened = widenedRepresentationOf(unit);
    return widened && widthOf(*widened) == m_widenedWidth;
}

void PaddingWidener::findCandidates()
{
    m_candidates = findIndicesInParallel(m_context.build.parallelLoops(), m_units.size(), [&](unsigned unit) {
        return isCandidate(m_units[unit]);
    });
}

void PaddingWidener::findUnitGroups()
{
    constexpr uint32_t noUnit = UINT32_MAX;
    Vector<uint32_t> candidateOfRow(FillWith { }, m_context.rows.count(), noUnit);
    for (uint32_t unit : m_candidates) {
        for (SlotRow row : m_units[unit].rows)
            candidateOfRow[row] = unit;
    }
    m_groupOfUnit.fill(noUnitGroup, m_units.size());
    Vector<SlotRow, 16> rows;
    for (const JoinedRows::Set& joined : m_joinedRows.sets()) {
        UnitGroup group;
        rows.shrink(0);
        for (SlotRow row : m_joinedRows.rowsOf(joined)) {
            const PlacedSlot& slot = m_packed.slots[row];
            if (uint32_t unit = candidateOfRow[row]; unit != noUnit) {
                rows.append(row);
                if (!group.units.contains(unit))
                    group.units.append(unit);
            } else if (slot.storage == SlotStorage::Inline && isNarrow(slot.representation))
                group.staysNarrow = true;
        }
        if (group.units.isEmpty() || (group.units.size() == 1 && !group.staysNarrow))
            continue;
        auto widened = widenedRepresentationOf({ rows.span(), 0 });
        if (widened && widthOf(*widened) == m_widenedWidth)
            group.widened = *widened;
        else
            group.staysNarrow = true;
        for (uint32_t unit : group.units)
            m_groupOfUnit[unit] = m_unitGroups.size();
        m_unitGroups.append(WTF::move(group));
    }
}

void PaddingWidener::recordOccupiedBytes()
{
    unsigned numberOfLayouts = m_context.program.layouts.size();
    m_occupied.grow(numberOfLayouts);
    m_firstPossiblyFreeOffset.fill(0, numberOfLayouts);
    forEachIndexInParallel(m_context.build.parallelLoops(), numberOfLayouts, [&](unsigned index) {
        LayoutID layout(index);
        if (!hasSlots(layout))
            return;
        m_occupied[index] = occupiedBytesOf(m_context, m_packed, layout);
        m_firstPossiblyFreeOffset[index] = firstSlotOffsetOf(layout);
    });
}

void PaddingWidener::orderCandidates()
{
    parallelSort(m_context.build.parallelLoops(), m_candidates.mutableSpan(), [&](uint32_t a, uint32_t b) {
        return std::tuple(m_units[b].weight.predicted(), m_units[a].rows[0]) < std::tuple(m_units[a].weight.predicted(), m_units[b].rows[0]);
    });
}

void PaddingWidener::move(const PaddingUnit& unit, uint32_t offset)
{
    for (SlotRow row : unit.rows) {
        PlacedSlot& slot = m_packed.slots[row];
        LayoutID layout = layoutOf(row);
        OccupiedBytes& occupied = m_occupied[layout.index()];
        RELEASE_ASSERT((occupied[slot.offset / 64] & bytesAt(slot.offset, slot.width())) == bytesAt(slot.offset, slot.width()));
        occupied[slot.offset / 64] &= ~bytesAt(slot.offset, slot.width());
        RELEASE_ASSERT(!(occupied[offset / 64] & bytesAt(offset, m_widenedWidth)));
        occupied[offset / 64] |= bytesAt(offset, m_widenedWidth);
        uint32_t& firstPossiblyFree = m_firstPossiblyFreeOffset[layout.index()];
        firstPossiblyFree = std::min(firstPossiblyFree, slot.offset / m_widenedWidth * m_widenedWidth);
        slot.offset = offset;
        ensureSizeReaches(m_packed, layout, offset + m_widenedWidth);
    }
}

std::optional<uint32_t> PaddingWidener::roomFor(const PaddingUnit& unit)
{
    uint32_t current = offsetOf(unit);
    uint32_t start = 0;
    uint32_t end = UINT32_MAX;
    for (SlotRow row : unit.rows) {
        uint32_t layout = layoutOf(row).index();
        uint32_t limit = limitOf(layoutOf(row));
        uint32_t& first = m_firstPossiblyFreeOffset[layout];
        while (first + m_widenedWidth <= limit && m_occupied[layout][first / 64] & bytesAt(first, m_widenedWidth))
            first += m_widenedWidth;
        start = std::max(start, first);
        end = std::min(end, limit);
    }
    auto isFree = [&](uint32_t offset) {
        return std::ranges::all_of(unit.rows, [&](SlotRow row) {
            uint64_t word = m_occupied[layoutOf(row).index()][offset / 64];
            if (current / 64 == offset / 64)
                word &= ~bytesAt(current, currentWidthOf(unit));
            return !(word & bytesAt(offset, m_widenedWidth));
        });
    };
    if (uint32_t own = current / m_widenedWidth * m_widenedWidth; own < start && own + m_widenedWidth <= end && isFree(own))
        return own;
    for (uint32_t offset = start; offset + m_widenedWidth <= end; offset += m_widenedWidth) {
        if (isFree(offset))
            return offset;
    }
    return std::nullopt;
}

std::optional<SlotRow> PaddingWidener::findRowThatMayMove(LayoutID layout, uint32_t offset) const
{
    const SlotRows& rows = m_context.rows;
    for (FieldIndex field = 0; field < rows.numberOfFieldsOf(layout); ++field) {
        SlotRow row = rows.rowOf(layout, field);
        const PlacedSlot& slot = m_packed.slots[row];
        if (slot.storage != SlotStorage::Inline || slot.offset != offset)
            continue;
        if (slot.width() != narrow || m_packed.sharedLocationOfRow[row] != noSharedLocation || m_packed.rowsWithEmptySlotElsewhere.get(row))
            return std::nullopt;
        return row;
    }
    return std::nullopt;
}

std::optional<uint32_t> PaddingWidener::findFreeHalfOutside(LayoutID layout, uint32_t word) const
{
    const OccupiedBytes& occupied = m_occupied[layout.index()];
    uint32_t limit = m_packed.inlineLimits[layout.index()];
    for (uint32_t offset = firstSlotOffsetOf(layout); offset + narrow <= limit; offset += narrow) {
        if (offset / wide * wide != word && !(occupied[offset / 64] & bytesAt(offset, narrow)))
            return offset;
    }
    return std::nullopt;
}

std::optional<uint32_t> PaddingWidener::makeRoomInOwnWord(const PaddingUnit& unit)
{
    struct Departure {
        SlotRow row;
        uint32_t offset;
    };
    uint32_t current = offsetOf(unit);
    uint32_t word = current / wide * wide;
    uint32_t otherHalf = current == word ? word + narrow : word;
    Vector<Departure, 4> departures;
    for (SlotRow row : unit.rows) {
        LayoutID layout = layoutOf(row);
        if (word + wide > m_packed.inlineLimits[layout.index()])
            return std::nullopt;
        if (!(m_occupied[layout.index()][otherHalf / 64] & bytesAt(otherHalf, narrow)))
            continue;
        auto leaving = findRowThatMayMove(layout, otherHalf);
        if (!leaving)
            return std::nullopt;
        auto freeHalf = findFreeHalfOutside(layout, word);
        if (!freeHalf)
            return std::nullopt;
        departures.append(Departure { *leaving, *freeHalf });
    }
    for (Departure departure : departures) {
        LayoutID layout = layoutOf(departure.row);
        OccupiedBytes& occupied = m_occupied[layout.index()];
        occupied[otherHalf / 64] &= ~bytesAt(otherHalf, narrow);
        RELEASE_ASSERT(!(occupied[departure.offset / 64] & bytesAt(departure.offset, narrow)));
        occupied[departure.offset / 64] |= bytesAt(departure.offset, narrow);
        m_packed.slots[departure.row].offset = departure.offset;
        ensureSizeReaches(m_packed, layout, departure.offset + narrow);
    }
    return word;
}

std::optional<uint32_t> PaddingWidener::findRoomFor(const PaddingUnit& unit, bool mayMoveOtherSlots)
{
    auto offset = roomFor(unit);
    if (!offset && mayMoveOtherSlots && m_widenedWidth == wide)
        offset = makeRoomInOwnWord(unit);
    return offset;
}

void PaddingWidener::widenAt(uint32_t index, uint32_t offset, SlotRepresentation widened)
{
    const PaddingUnit& unit = m_units[index];
    move(unit, offset);
    for (SlotRow row : unit.rows) {
        PlacedSlot& slot = m_packed.slots[row];
        slot.representation = widened;
        RELEASE_ASSERT(slot.width() == m_widenedWidth);
    }
    m_isWidened.set(index);
}

bool PaddingWidener::widen(uint32_t index, bool mayMoveOtherSlots)
{
    if (m_groupOfUnit[index] == noUnitGroup) {
        auto offset = findRoomFor(m_units[index], mayMoveOtherSlots);
        if (!offset)
            return false;
        widenAt(index, *offset, *widenedRepresentationOf(m_units[index]));
        return true;
    }

    const UnitGroup& group = m_unitGroups[m_groupOfUnit[index]];
    if (group.staysNarrow)
        return false;
    // No two units of a group are in the same layout, so the room found for one is still free when all have been placed.
    Vector<uint32_t, 4> offsets;
    for (uint32_t unit : group.units) {
        auto offset = findRoomFor(m_units[unit], mayMoveOtherSlots);
        if (!offset)
            return false;
        offsets.append(*offset);
    }
    for (size_t i = 0; i < group.units.size(); ++i)
        widenAt(group.units[i], offsets[i], group.widened);
    return true;
}

void PaddingWidener::run()
{
    createUnits();
    findCandidates();
    if (m_candidates.isEmpty())
        return;
    if (m_kind == WidenedRepresentations::SmallIntegers)
        weighCandidatesByName();
    findUnitGroups();
    recordOccupiedBytes();
    orderCandidates();
    for (bool mayMoveOtherSlots : { false, true }) {
        for (bool didWiden = true; didWiden;) {
            didWiden = false;
            for (uint32_t unit : m_candidates) {
                if (!m_isWidened.get(unit) && widen(unit, mayMoveOtherSlots))
                    didWiden = true;
            }
        }
    }
}

enum class SlotLocations : uint8_t {
    None,
    One,
    Several,
};

struct SlotCoverage {
    SlotLocations locations { SlotLocations::None };
    PlacedSlot slot;
    Vector<SlotRow, 8> rowsWithSlot;
    Vector<LayoutID, 8> membersWithoutSlot;
};

class EmptySlotReserver {
public:
    EmptySlotReserver(const SlotPlanContext&, const SlotUses&, PackedLayouts&, const EmptySlotRoom&);

    void run();

private:
    std::optional<SlotRow> findRowWithSlot(LayoutID member, PropertyKeyID) const;
    SlotCoverage coverageOf(const SlotAccessKey&) const;
    bool isCandidate(const SlotAccessUse&) const;
    bool isKeptZero(ReservedBytes, LayoutID) const;
    bool canReserve(ReservedBytes, LayoutID) const;
    void reserve(ReservedBytes, LayoutID);

    const SlotPlanContext& m_context;
    const SlotUses& m_uses;
    PackedLayouts& m_packed;
    const EmptySlotRoom& m_room;
};

EmptySlotReserver::EmptySlotReserver(const SlotPlanContext& context, const SlotUses& uses, PackedLayouts& packed, const EmptySlotRoom& room)
    : m_context(context)
    , m_uses(uses)
    , m_packed(packed)
    , m_room(room)
{
}

std::optional<SlotRow> EmptySlotReserver::findRowWithSlot(LayoutID member, PropertyKeyID key) const
{
    if (!m_context.isCompiledObject(member))
        return std::nullopt;
    auto field = m_context.program.at(member).findField(key);
    if (!field)
        return std::nullopt;
    SlotRow row = m_context.rows.rowOf(member, *field);
    return m_packed.slots[row].hasStorage() ? std::optional(row) : std::nullopt;
}

SlotCoverage EmptySlotReserver::coverageOf(const SlotAccessKey& access) const
{
    SlotCoverage coverage;
    for (LayoutID member : m_context.program.at(access.receivers).completeMembers()) {
        auto row = findRowWithSlot(member, access.key);
        if (!row) {
            coverage.membersWithoutSlot.append(member);
            continue;
        }
        const PlacedSlot& slot = m_packed.slots[*row];
        coverage.rowsWithSlot.append(*row);
        if (!m_room.hasLocation(*row))
            coverage.locations = SlotLocations::Several;
        switch (coverage.locations) {
        case SlotLocations::None:
            coverage.locations = SlotLocations::One;
            coverage.slot = slot;
            break;
        case SlotLocations::One:
            if (!slot.hasLocationOf(coverage.slot))
                coverage.locations = SlotLocations::Several;
            break;
        case SlotLocations::Several:
            break;
        }
    }
    return coverage;
}

bool EmptySlotReserver::isCandidate(const SlotAccessUse& use) const
{
    if (use.access.isStore || !use.access.receivers)
        return false;
    SlotCoverage coverage = coverageOf(use.access);
    if (coverage.locations != SlotLocations::One || coverage.membersWithoutSlot.isEmpty())
        return false;
    if (coverage.slot.storage != SlotStorage::Inline || !representsAbsentAsZero(coverage.slot.representation))
        return false;
    bool everyMemberIsCompiledObject = std::ranges::all_of(coverage.membersWithoutSlot, [&](LayoutID member) {
        return m_context.isCompiledObject(member);
    });
    if (!everyMemberIsCompiledObject)
        return false;
    uint64_t withoutSlot = coverage.membersWithoutSlot.size();
    return coverage.rowsWithSlot.size() >= withoutSlot && (use.weight >= saturatedProduct(minAccessWeightPerEmptySlot, withoutSlot)).predicted();
}

bool EmptySlotReserver::isKeptZero(ReservedBytes bytes, LayoutID layout) const
{
    return m_packed.bytesKeptZero[layout.index()].containsIf([&](const ReservedBytes& reserved) {
        return reserved.offset <= bytes.offset && bytes.offset + bytes.width <= reserved.offset + reserved.width;
    });
}

bool EmptySlotReserver::canReserve(ReservedBytes bytes, LayoutID layout) const
{
    return isKeptZero(bytes, layout) || m_room.isFree(bytes, layout);
}

void EmptySlotReserver::reserve(ReservedBytes bytes, LayoutID layout)
{
    if (isKeptZero(bytes, layout))
        return;
    m_packed.bytesKeptZero[layout.index()].append(bytes);
    m_room.take(bytes, layout);
    ensureSizeReaches(m_packed, layout, bytes.offset + bytes.width);
}

void EmptySlotReserver::run()
{
    auto& accesses = m_uses.accesses;
    Vector<uint32_t> candidates = findIndicesInParallel(m_context.build.parallelLoops(), accesses.size(), [&](unsigned index) {
        return isCandidate(accesses[index]);
    });
    if (candidates.isEmpty())
        return;
    AOT::sort(candidates, [&](uint32_t a, uint32_t b) {
        return std::tuple(accesses[b].weight.predicted(), a) < std::tuple(accesses[a].weight.predicted(), b);
    });

    for (uint32_t index : candidates) {
        SlotCoverage coverage = coverageOf(accesses[index].access);
        ReservedBytes bytes { coverage.slot.offset, coverage.slot.width() };
        bool hasRoom = std::ranges::all_of(coverage.membersWithoutSlot, [&](LayoutID member) {
            return canReserve(bytes, member);
        });
        if (!hasRoom)
            continue;
        for (LayoutID member : coverage.membersWithoutSlot)
            reserve(bytes, member);
        for (SlotRow row : coverage.rowsWithSlot)
            m_packed.rowsWithEmptySlotElsewhere.set(row);
    }
}

} // anonymous namespace

void widenSlotsIntoPadding(const SlotPlanContext& context, const SlotUses& uses, const JoinedRows& joinedRows, std::span<const SlotClaim> claims, WidenedRepresentations kind, PackedLayouts& packed)
{
    if (kind == WidenedRepresentations::References && context.usesWideReferences)
        return;
    PaddingWidener(context, uses, joinedRows, claims, kind, packed).run();
}

void reserveEmptySlots(const SlotPlanContext& context, const SlotUses& uses, PackedLayouts& packed, const EmptySlotRoom& room)
{
    EmptySlotReserver(context, uses, packed, room).run();
}

void reserveEmptySlotsInPadding(const SlotPlanContext& context, const SlotUses& uses, PackedLayouts& packed)
{
    Vector<OccupiedBytes> occupiedBytes(context.program.layouts.size());
    auto occupiedIn = [&](LayoutID layout) -> OccupiedBytes& {
        OccupiedBytes& occupied = occupiedBytes[layout.index()];
        if (occupied.isEmpty())
            occupied = occupiedBytesOf(context, packed, layout);
        return occupied;
    };
    auto hasLocation = [](SlotRow) {
        return true;
    };
    auto isFree = [&](ReservedBytes bytes, LayoutID layout) {
        if (bytes.offset + bytes.width > packed.inlineLimits[layout.index()])
            return false;
        return !(occupiedIn(layout)[bytes.offset / 64] & bytesAt(bytes.offset, bytes.width));
    };
    auto take = [&](ReservedBytes bytes, LayoutID layout) {
        occupiedIn(layout)[bytes.offset / 64] |= bytesAt(bytes.offset, bytes.width);
    };
    reserveEmptySlots(context, uses, packed, { hasLocation, isFree, take });
    for (auto& reserved : packed.bytesKeptZero)
        AOT::sort(reserved);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
