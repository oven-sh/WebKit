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
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <tuple>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

inline constexpr SiteWeight minAccessWeightPerEmptySlot = SiteWeight { 1 } << siteWeightFractionBits;

void ensureSizeReaches(PackedLayouts& packed, LayoutID layout, uint32_t end)
{
    uint32_t& size = packed.sizes[layout.index()];
    size = std::max(size, WTF::roundUpToMultipleOf(16, end));
    RELEASE_ASSERT(size <= packed.inlineLimits[layout.index()]);
}

struct Unit {
    std::span<const SlotRow> rows;
    SiteWeight weight { 0 };
};

class PaddingWidener {
public:
    PaddingWidener(const SlotPlanContext&, const SlotUses&, std::span<const SlotClaim>, WidenedRepresentations, PackedLayouts&);

    void run();

private:
    static constexpr uint32_t narrow = widthOf(SlotRepresentation::OptionalInt32);
    static constexpr uint32_t wide = widthOf(SlotRepresentation::OptionalInt64);
    static_assert(widthOf(SlotRepresentation::Cell32) == narrow && widthOf(SlotRepresentation::OptionalCell32) == narrow);
    static_assert(widthOf(SlotRepresentation::Cell) == wide && widthOf(SlotRepresentation::JSValue) == wide);
    static_assert(!(offsetOfFirstField % wide) && !(offsetOfFirstEnvironmentVariable % wide));

    bool hasSlots(LayoutID id) const { return m_context.isCompiledObject(id) || m_context.isEnvironment(id); }
    uint32_t firstSlotOffsetOf(LayoutID id) const { return m_context.isEnvironment(id) ? offsetOfFirstEnvironmentVariable : offsetOfFirstField; }
    LayoutID layoutOf(SlotRow row) const { return m_layoutOfRow[row]; }
    uint32_t offsetOf(const Unit& unit) const { return m_packed.slots[unit.rows[0]].offset; }

    const Type& contentsOf(SlotRow) const;
    bool holdsEnvironmentOfDeferredClosures(SlotRow) const;
    bool isNarrow(SlotRepresentation) const;
    bool holdsUndefinedOrNull(const Unit&) const;
    std::optional<SlotRepresentation> widenedRepresentationOf(const Unit&) const;
    void createUnits();
    bool isCandidate(const Unit&) const;
    void findCandidates();
    void recordOccupiedBytes();
    void orderCandidates();
    std::optional<uint32_t> roomFor(const Unit&);
    void move(const Unit&, uint32_t offset);
    bool widen(uint32_t unit);

    const SlotPlanContext& m_context;
    const SlotUses& m_uses;
    std::span<const SlotClaim> m_claims;
    WidenedRepresentations m_kind;
    PackedLayouts& m_packed;

    Vector<LayoutID> m_layoutOfRow;
    Vector<SlotRow> m_unsharedRows;
    Vector<Unit> m_units;
    Vector<uint32_t> m_candidates;
    Vector<OccupiedBytes> m_occupied;
    Vector<uint32_t> m_firstPossiblyFreeOffset;
    BitVector m_isWidened;
};

PaddingWidener::PaddingWidener(const SlotPlanContext& context, const SlotUses& uses, std::span<const SlotClaim> claims, WidenedRepresentations kind, PackedLayouts& packed)
    : m_context(context)
    , m_uses(uses)
    , m_claims(claims)
    , m_kind(kind)
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
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool PaddingWidener::holdsUndefinedOrNull(const Unit& unit) const
{
    return std::ranges::any_of(unit.rows, [&](SlotRow row) {
        return contentsOf(row).mayBeUndefinedOrNull();
    });
}

std::optional<SlotRepresentation> PaddingWidener::widenedRepresentationOf(const Unit& unit) const
{
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
            if (slot.storage != SlotStorage::Inline || slot.width() != narrow)
                continue;
            m_layoutOfRow[row] = layout;
            if (m_packed.sharedLocationOfRow[row] == noSharedLocation)
                m_unsharedRows.append(row);
        }
    }
    m_units.reserveInitialCapacity(sharedLocations.size() + m_unsharedRows.size());
    for (const SharedLocation& location : sharedLocations)
        m_units.append(Unit { location.rows.span(), location.weight });
    for (size_t i = 0; i < m_unsharedRows.size(); ++i) {
        const SlotRowUse& use = m_uses.rows[m_unsharedRows[i]];
        m_units.append(Unit { m_unsharedRows.subspan(i, 1), saturatedSum(use.accessWeight, use.environmentUses) });
    }
}

bool PaddingWidener::isCandidate(const Unit& unit) const
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
    return widened && widthOf(*widened) == wide;
}

void PaddingWidener::findCandidates()
{
    m_candidates = findIndicesInParallel(m_context.build.parallelLoops(), m_units.size(), [&](unsigned unit) {
        return isCandidate(m_units[unit]);
    });
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
        return std::tuple(m_units[b].weight, m_units[a].rows[0]) < std::tuple(m_units[a].weight, m_units[b].rows[0]);
    });
}

void PaddingWidener::move(const Unit& unit, uint32_t offset)
{
    for (SlotRow row : unit.rows) {
        PlacedSlot& slot = m_packed.slots[row];
        LayoutID layout = layoutOf(row);
        OccupiedBytes& occupied = m_occupied[layout.index()];
        RELEASE_ASSERT((occupied[slot.offset / 64] & bytesAt(slot.offset, narrow)) == bytesAt(slot.offset, narrow));
        occupied[slot.offset / 64] &= ~bytesAt(slot.offset, narrow);
        RELEASE_ASSERT(!(occupied[offset / 64] & bytesAt(offset, wide)));
        occupied[offset / 64] |= bytesAt(offset, wide);
        uint32_t& firstPossiblyFree = m_firstPossiblyFreeOffset[layout.index()];
        firstPossiblyFree = std::min(firstPossiblyFree, slot.offset / wide * wide);
        slot.offset = offset;
        ensureSizeReaches(m_packed, layout, offset + wide);
    }
}

std::optional<uint32_t> PaddingWidener::roomFor(const Unit& unit)
{
    uint32_t current = offsetOf(unit);
    uint32_t start = 0;
    uint32_t end = UINT32_MAX;
    for (SlotRow row : unit.rows) {
        uint32_t layout = layoutOf(row).index();
        uint32_t limit = m_packed.inlineLimits[layout];
        uint32_t& first = m_firstPossiblyFreeOffset[layout];
        while (first + wide <= limit && m_occupied[layout][first / 64] & bytesAt(first, wide))
            first += wide;
        start = std::max(start, first);
        end = std::min(end, limit);
    }
    auto isFree = [&](uint32_t offset) {
        return std::ranges::all_of(unit.rows, [&](SlotRow row) {
            uint64_t word = m_occupied[layoutOf(row).index()][offset / 64];
            if (current / 64 == offset / 64)
                word &= ~bytesAt(current, narrow);
            return !(word & bytesAt(offset, wide));
        });
    };
    if (uint32_t own = current / wide * wide; own < start && own + wide <= end && isFree(own))
        return own;
    for (uint32_t offset = start; offset + wide <= end; offset += wide) {
        if (isFree(offset))
            return offset;
    }
    return std::nullopt;
}

bool PaddingWidener::widen(uint32_t index)
{
    const Unit& unit = m_units[index];
    auto offset = roomFor(unit);
    if (!offset)
        return false;
    move(unit, *offset);
    SlotRepresentation widened = *widenedRepresentationOf(unit);
    for (SlotRow row : unit.rows) {
        PlacedSlot& slot = m_packed.slots[row];
        slot.representation = widened;
        RELEASE_ASSERT(slot.width() == wide);
    }
    m_isWidened.set(index);
    return true;
}

void PaddingWidener::run()
{
    createUnits();
    findCandidates();
    if (m_candidates.isEmpty())
        return;
    recordOccupiedBytes();
    orderCandidates();
    for (bool didWiden = true; didWiden;) {
        didWiden = false;
        for (uint32_t unit : m_candidates) {
            if (!m_isWidened.get(unit) && widen(unit))
                didWiden = true;
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
    for (LayoutID member : m_context.program.at(access.receivers).members) {
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
    return coverage.rowsWithSlot.size() >= withoutSlot && use.weight >= saturatedProduct(minAccessWeightPerEmptySlot, withoutSlot);
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
    std::ranges::sort(candidates, [&](uint32_t a, uint32_t b) {
        return std::tuple(accesses[b].weight, a) < std::tuple(accesses[a].weight, b);
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

void widenSlotsIntoPadding(const SlotPlanContext& context, const SlotUses& uses, std::span<const SlotClaim> claims, WidenedRepresentations kind, PackedLayouts& packed)
{
    if (kind == WidenedRepresentations::References && context.usesWideReferences)
        return;
    PaddingWidener(context, uses, claims, kind, packed).run();
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
        std::ranges::sort(reserved);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
