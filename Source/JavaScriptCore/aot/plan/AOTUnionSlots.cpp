/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Mechanism::TypedOperations: where in the cell a field lies, so that a name which is read through a family lies at one offset in its members, or at few, and a member that lacks it keeps those bytes zero.
// At a suspension and at a throw: nothing. A slot has its offset for the whole build.
// Withheld: nobody calls, and every layout has its fields in the order of declaration. ANY PLACE IS RIGHT: SlotPlan::fieldAccess() answers by the offsets that it finds. No cell grows for any of it.

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

// How often reached code, as written, names the field through ONE layout. fields-and-objects-2 counts that (usesOfField()): whoever brings the two packages together forwards it. It only ranks.
static uint32_t mentionsOfFieldForUnionSlots(const LinkTables&, FieldReference)
{
    return 0;
}

static uint32_t sumOfMentionsInUnionSlots(uint32_t first, uint32_t second)
{
    return first > UINT32_MAX - second ? UINT32_MAX : first + second;
}

static constexpr uint32_t roundUpToWidestSlotInUnionSlots(uint32_t bytes)
{
    return (bytes + widestSlotInUnionSlots - 1) & ~(widestSlotInUnionSlots - 1);
}

namespace {

using WidthCountsInUnionSlots = std::array<uint32_t, numberOfWidthsInUnionSlots>;

// Taking any free aligned block takes exactly two free blocks of half its width, so counts decide what packing widest first would find.
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

// The bytes of a cell while rows are given their offsets. It refuses an offset that would leave no room for the rows still to come.
class CellInUnionSlots {
public:
    bool isInitialized() const { return m_isInitialized; }

    // `unplaced`: by the logarithm of the width, how many rows will be placed. Both bounds are multiples of the widest slot.
    void initialize(uint32_t firstByte, uint32_t limit, const WidthCountsInUnionSlots& unplaced)
    {
        m_isInitialized = true;
        m_firstByte = firstByte;
        m_limit = limit;
        m_end = firstByte;
        m_unplaced = unplaced;
        m_occupied.fill(0, limit / 64 + 1);
        for (uint32_t offset = 0; offset < firstByte; offset += widestSlotInUnionSlots)
            m_occupied[offset / 64] |= maskOf(offset, widestSlotInUnionSlots);
        for (uint32_t offset = limit; offset < m_occupied.size() * 64; offset += widestSlotInUnionSlots)
            m_occupied[offset / 64] |= maskOf(offset, widestSlotInUnionSlots);
        for (unsigned logSize = 0; logSize < numberOfWidthsInUnionSlots; ++logSize)
            m_room[logSize] = (limit - firstByte) >> logSize;
    }

    uint32_t firstByte() const { return m_firstByte; }
    uint32_t limit() const { return m_limit; }
    // One byte past the last that is taken.
    uint32_t end() const { return m_end; }

    // Of an aligned block.
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

    void place(uint32_t offset, unsigned logSize)
    {
        RELEASE_ASSERT(canPlace(offset, logSize));
        --m_unplaced[logSize];
        take(offset, logSize);
    }

    // The same for bytes that none of the rows still to come takes: they are kept zero.
    bool canReserve(uint32_t offset, unsigned logSize) const
    {
        return isAlignedAndFree(offset, logSize) && remainingRowsFitInUnionSlots(roomAfterTaking(offset, logSize), m_unplaced);
    }

    void reserve(uint32_t offset, unsigned logSize)
    {
        RELEASE_ASSERT(canReserve(offset, logSize));
        take(offset, logSize);
    }

    // A row that somebody else has placed, wherever that is.
    void occupy(uint32_t offset, unsigned logSize)
    {
        if (isAlignedAndFree(offset, logSize))
            take(offset, logSize);
    }

    // The lowest offset at or past `from` that canPlace() takes.
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

    void take(uint32_t offset, unsigned logSize)
    {
        m_room = roomAfterTaking(offset, logSize);
        m_occupied[offset / 64] |= maskOf(offset, 1u << logSize);
        m_end = std::max(m_end, offset + (1u << logSize));
    }

    // A bit for each byte.
    Vector<uint64_t, 4> m_occupied;
    bool m_isInitialized { false };
    uint32_t m_firstByte { 0 };
    uint32_t m_limit { 0 };
    uint32_t m_end { 0 };
    WidthCountsInUnionSlots m_unplaced { };
    // By the logarithm of the width: how many aligned blocks are free.
    WidthCountsInUnionSlots m_room { };
};

// In ascending order each holds what the one in front holds: FormOfPlaceInSlotPlan.
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

Vector<OffsetUseInUnionSlots, 8> countOffsetUsesInUnionSlots(Vector<uint32_t, 8>&& offsets)
{
    AOT::sort(offsets);
    Vector<OffsetUseInUnionSlots, 8> uses;
    for (uint32_t offset : offsets) {
        if (uses.isEmpty() || uses.last().offset != offset)
            uses.append(OffsetUseInUnionSlots { offset, 0 });
        ++uses.last().numberOfRows;
    }
    AOT::sort(uses, [](const OffsetUseInUnionSlots& a, const OffsetUseInUnionSlots& b) {
        return a.numberOfRows != b.numberOfRows ? a.numberOfRows > b.numberOfRows : a.offset < b.offset;
    });
    return uses;
}

class PlacerOfUnionSlots {
    WTF_MAKE_NONCOPYABLE(PlacerOfUnionSlots);
public:
    PlacerOfUnionSlots(const Program& program, const LinkTables& link, const Families& families, const RowsOfSlotPlan& rows, const ScopedLambda<bool(LayoutID)>& hasCells, const ScopedLambda<uint32_t()>& addPlace, Vector<Vector<ReservedBytes>>& bytesKeptZero)
        : m_program(program)
        , m_link(link)
        , m_families(families)
        , m_rows(rows)
        , m_hasCells(hasCells)
        , m_addPlace(addPlace)
        , m_bytesKeptZero(bytesKeptZero)
        , m_numberOfLayouts(static_cast<uint32_t>(program.layouts.size()))
        , m_isTouched(m_numberOfLayouts)
        , m_allAboveIsPlaced(m_numberOfLayouts)
    {
        RELEASE_ASSERT(rows.firstRowOfLayout.size() == m_numberOfLayouts + 1 && rows.ends.size() == m_numberOfLayouts);
        RELEASE_ASSERT(rows.offsets.size() == rows.firstRowOfLayout.back() && rows.places.size() == rows.offsets.size() && (rows.widths.empty() || rows.widths.size() == rows.offsets.size()));
    }

    void run()
    {
        orderLayoutsBelowTheirBases();
        collectUnits();
        collectGroups();
        findReadsThatEmptySlotsMayServe();
        if (m_groups.isEmpty() && m_readsThatEmptySlotsMayServe.isEmpty())
            return;
        m_cells.grow(m_numberOfLayouts);
        m_depthsReached.fill(0, m_numberOfLayouts);
        for (uint32_t group : m_groupsByTurn)
            placeAtSharedOffsets(group);
        // Beside the rows that several layouts share, while the rows of one layout can still make way. Then beside those, in what is left of the cells.
        reserveEmptySlots();
        fillLayoutsThatWereTouched();
        reserveEmptySlots();
        fillLayoutsThatWereTouched();
        finishLayoutsThatWereTouched();
    }

private:
    using UnitIndex = uint32_t;
    using UnitList = Vector<UnitIndex, 8>;

    static constexpr uint32_t none = UINT32_MAX;

    // The rows that lie at one offset whatever else is decided: a declaration with the rows that the layouts written below it have from it, or hide it with.
    struct Unit {
        // In m_rowsOfUnits. The first is the declaration.
        uint32_t firstRow { 0 };
        uint32_t numberOfRows { 0 };
        LayoutID declaringLayout;
        // Of the layout that declares the field.
        uint32_t depth { 0 };
        // The first group, by turn, that holds one of the rows.
        uint32_t group { none };
        uint32_t offset { 0 };
        uint32_t place { RowsOfSlotPlan::noPlace };
        uint8_t logWidth { 0 };
        FormInUnionSlots form { FormInUnionSlots::Unknown };
        bool isPlaced { false };
        // Its rows differ in their widths. Nobody here places it.
        bool isRefused { false };
        bool isQueued { false };
    };

    // The declarations of one name in the members of one family that have one width and come to one form.
    struct Group {
        // In m_unitsOfGroups: ascending, a unit once for each member that has a row of it.
        uint32_t firstUnit { 0 };
        uint32_t numberOfUnits { 0 };
        uint32_t mentions { 0 };
    };

    struct RowInMember {
        uint32_t family { 0 };
        uint32_t key { 0 };
        uint8_t logWidth { 0 };
        FormInUnionSlots form { FormInUnionSlots::Unknown };
        UnitIndex unit { 0 };
    };

    uint32_t rowOf(FieldReference field) const { return m_rows.firstRowOfLayout[field.layout.index()] + field.field; }
    uint32_t widthOfRow(uint32_t row) const { return m_rows.widths.empty() ? widestSlotInUnionSlots : m_rows.widths[row]; }
    bool isTouchable(LayoutID id) const { return m_depths[id.index()] != none; }
    std::span<const uint32_t> rowsOf(UnitIndex unit) const LIFETIME_BOUND { return m_rowsOfUnits.span().subspan(m_units[unit].firstRow, m_units[unit].numberOfRows); }
    std::span<const UnitIndex> unitsOf(const Group& group) const LIFETIME_BOUND { return m_unitsOfGroups.span().subspan(group.firstUnit, group.numberOfUnits); }
    uint32_t offsetOfFirstSlotOf(LayoutID id) const { return offsetOfFirstField + (m_rows.layoutsWithHiddenWords.get(id.index()) ? widestSlotInUnionSlots : 0); }

    // ---- The layouts

    // A layout comes behind the layout above it. Left out, and never touched: what is no object, a ring of written bases with all that hangs below it, a layout that does not list what the row above it declares with all below it.
    // The fields of such a layout begin behind the end of the layout above, wherever the fields of that one lie.
    void orderLayoutsBelowTheirBases()
    {
        constexpr uint32_t unseen = UINT32_MAX - 1;
        constexpr uint32_t onTheWay = UINT32_MAX - 2;
        m_depths.fill(unseen, m_numberOfLayouts);
        Vector<uint32_t, 16> row;
        for (uint32_t index = 0; index < m_numberOfLayouts; ++index) {
            row.shrink(0);
            LayoutID above(index);
            for (; above && m_depths[above.index()] == unseen; above = writtenBaseOf(m_program.at(above))) {
                m_depths[above.index()] = onTheWay;
                row.append(above.index());
            }
            // Null: the row ends. On the way: it has come back to where it has been.
            uint32_t depthAbove = above ? m_depths[above.index()] : 0;
            bool mayBeTouched = !above || (depthAbove != onTheWay && depthAbove != none);
            uint32_t depth = above && mayBeTouched ? depthAbove + 1 : 0;
            while (!row.isEmpty()) {
                uint32_t layoutIndex = row.takeLast();
                const Layout& layout = m_program.layouts[layoutIndex];
                mayBeTouched &= isObjectLayout(layout.kind) && (!writtenBaseOf(layout) || layout.inheritedFields == InheritedFields::AllOfRow);
                m_depths[layoutIndex] = mayBeTouched ? depth++ : none;
                if (mayBeTouched)
                    m_layoutsBelowTheirBases.append(LayoutID(layoutIndex));
            }
        }
    }

    // Nothing lies above it, nothing below it, and nothing is laid behind its fields, so the bytes up to the size that the collector gives out are its own.
    bool standsByItself(LayoutID id) const
    {
        return !writtenBaseOf(m_program.at(id)) && m_program.layoutsExpectedBelow(id).empty() && m_link.appendedKeysOf(id).empty() && m_hasCells(id);
    }

    // ---- The units

    // The field of a layout above whose place the field takes: findFieldAboveForSlotPlan().
    std::optional<FieldReference> findFieldAbove(FieldReference reference) const
    {
        LayoutID base = writtenBaseOf(m_program.at(reference.layout));
        if (!base)
            return std::nullopt;
        const Field& listed = fieldAt(m_program, reference);
        FieldReference above;
        if (listed.flags.contains(FieldFlags::IsInherited))
            above = declarationOf(m_program, reference);
        else if (std::optional<uint32_t> hidden = m_program.at(base).findField(listed.key))
            above = { base, *hidden };
        else
            return std::nullopt;
        if (above.layout == reference.layout || !isTouchable(above.layout))
            return std::nullopt;
        return above;
    }

    void collectUnits()
    {
        uint32_t numberOfRows = static_cast<uint32_t>(m_rows.offsets.size());
        m_unitOfRow.fill(none, numberOfRows);
        m_layoutOfRow.fill(LayoutID(), numberOfRows);
        Vector<uint32_t> rowsWithUnits;
        for (LayoutID id : m_layoutsBelowTheirBases) {
            uint32_t numberOfFields = static_cast<uint32_t>(m_program.at(id).fields.size());
            for (uint32_t field = 0; field < numberOfFields; ++field) {
                uint32_t row = rowOf({ id, field });
                uint32_t width = widthOfRow(row);
                m_layoutOfRow[row] = id;
                if (!width)
                    continue;
                RELEASE_ASSERT(std::has_single_bit(width) && width <= widestSlotInUnionSlots);
                uint8_t logWidth = static_cast<uint8_t>(std::countr_zero(width));
                UnitIndex unit = none;
                if (std::optional<FieldReference> above = findFieldAbove({ id, field }))
                    unit = m_unitOfRow[rowOf(*above)];
                if (unit == none) {
                    unit = static_cast<UnitIndex>(m_units.size());
                    m_units.append(Unit { });
                    m_units[unit].declaringLayout = id;
                    m_units[unit].depth = m_depths[id.index()];
                    m_units[unit].logWidth = logWidth;
                    if (m_rows.places[row] != RowsOfSlotPlan::noPlace) {
                        m_units[unit].isPlaced = true;
                        m_units[unit].offset = static_cast<uint32_t>(m_rows.offsets[row]);
                        m_units[unit].place = m_rows.places[row];
                    }
                }
                m_units[unit].isRefused |= m_units[unit].logWidth != logWidth;
                ++m_units[unit].numberOfRows;
                m_unitOfRow[row] = unit;
                rowsWithUnits.append(row);
            }
        }

        uint32_t firstRow = 0;
        for (Unit& unit : m_units) {
            unit.firstRow = firstRow;
            firstRow += std::exchange(unit.numberOfRows, 0);
        }
        m_rowsOfUnits.fill(0, firstRow);
        // In the order of the layouts, so the declaration comes first.
        for (uint32_t row : rowsWithUnits) {
            Unit& unit = m_units[m_unitOfRow[row]];
            m_rowsOfUnits[unit.firstRow + unit.numberOfRows++] = row;
        }
    }

    // As formProvenOfFieldForSlotPlan() goes by Link's word. Rows that share a place share a form, the widest that one of them asks for.
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

    // A declaration under which nothing is ever stored asks nothing of the place that it shares. It goes with those that hold any value.
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

    // ---- The groups

    uint32_t mentionsThroughFamily(uint32_t family, uint32_t key) const
    {
        std::span<const LinkedUseThroughFamily> uses = m_link.usesThroughFamilies.span();
        auto found = std::ranges::lower_bound(uses, std::tuple(family, key), { }, [](const LinkedUseThroughFamily& use) {
            return std::tuple(use.family.index(), use.key.index());
        });
        if (found == uses.end() || found->family.index() != family || found->key.index() != key)
            return 0;
        return sumOfMentionsInUnionSlots(found->reads, found->stores);
    }

    // `function kindOf(n: Decl | Expr) { return n.kind }`: for each family and each name that several declarations give its members. The most mentioned first, then in the order of the families, the smallest first, and of the names.
    // A family has its number by its size and its members, and a name that counts has its number by the program alone.
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

        // By family: the members that lie in no family inside it.
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

        // A class with what extends it is a family, and so is each class below with what extends that: a row of classes is a row of families that hold one another, and whoever goes through the members of each
        // goes through each pair of classes. One declaration lies at one offset anyway, so the members count only where several declarations give a name. Whether they do, a family knows from the families inside it.
        struct DeclarationOfName {
            uint32_t key { 0 };
            UnitIndex unit { 0 };
            auto operator<=>(const DeclarationOfName&) const = default;
        };
        // Ascending, of each name at most two. A family has a lower number than every family that holds it, so each has heard from all inside it when its turn comes.
        Vector<Vector<DeclarationOfName>> declarationsInFamilies(numberOfFamilies);
        // What came from the first family inside, which is in order.
        Vector<uint32_t> numbersOfDeclarationsInOrder;
        numbersOfDeclarationsInOrder.fill(0, numberOfFamilies);
        // By key: one more than the last family where several declarations give the name.
        Vector<uint32_t> lastFamiliesWithSeveralDeclarations;
        lastFamiliesWithSeveralDeclarations.fill(0, types.numberOfPropertyKeys());

        Vector<RowInMember> rowsInMembers;
        for (uint32_t family = 0; family < numberOfFamilies; ++family) {
            Vector<DeclarationOfName> declarations = std::exchange(declarationsInFamilies[family], { });
            for (LayoutID member : membersOutsideSmallerFamilies.span().subspan(firstMemberOutsideSmallerFamilies[family], firstMemberOutsideSmallerFamilies[family + 1] - firstMemberOutsideSmallerFamilies[family])) {
                forEachNameOf(member, [&](uint32_t key, UnitIndex unit) {
                    declarations.append(DeclarationOfName { key, unit });
                });
            }
            auto endOfThoseInOrder = declarations.begin() + numbersOfDeclarationsInOrder[family];
            AOT::sort(std::span(endOfThoseInOrder, declarations.end()));
            std::inplace_merge(declarations.begin(), endOfThoseInOrder, declarations.end());

            bool hasNameWithSeveralDeclarations = false;
            size_t numberKept = 0;
            for (size_t index = 0; index < declarations.size(); ++index) {
                DeclarationOfName declaration = declarations[index];
                size_t numberKeptOfName = numberKept && declarations[numberKept - 1].key == declaration.key ? 1 + (numberKept > 1 && declarations[numberKept - 2].key == declaration.key) : 0;
                if (numberKeptOfName == 2 || (numberKeptOfName && declarations[numberKept - 1].unit == declaration.unit))
                    continue;
                if (numberKeptOfName) {
                    hasNameWithSeveralDeclarations = true;
                    lastFamiliesWithSeveralDeclarations[declaration.key] = family + 1;
                }
                declarations[numberKept++] = declaration;
            }
            declarations.shrink(numberKept);

            if (hasNameWithSeveralDeclarations) {
                m_program.at(m_families.layoutsOf(FamilyID(family))).forEach([&](LayoutID member) {
                    forEachNameOf(member, [&](uint32_t key, UnitIndex unit) {
                        if (lastFamiliesWithSeveralDeclarations[key] == family + 1)
                            rowsInMembers.append(RowInMember { family, key, m_units[unit].logWidth, FormInUnionSlots::Unknown, unit });
                    });
                });
            }

            if (FamilyID above = m_families.nearestFamilyAbove(FamilyID(family))) {
                Vector<DeclarationOfName>& declarationsAbove = declarationsInFamilies[above.index()];
                if (declarationsAbove.isEmpty()) {
                    numbersOfDeclarationsInOrder[above.index()] = declarations.size();
                    declarationsAbove = WTF::move(declarations);
                } else
                    declarationsAbove.appendVector(declarations);
            }
        }
        auto nameInFamily = [](const RowInMember& row) {
            return std::tuple(row.family, row.key);
        };
        AOT::sort(rowsInMembers, [&](const RowInMember& a, const RowInMember& b) {
            return std::tuple(nameInFamily(a), a.unit) < std::tuple(nameInFamily(b), b.unit);
        });

        for (size_t begin = 0; begin < rowsInMembers.size();) {
            size_t end = begin;
            while (end < rowsInMembers.size() && nameInFamily(rowsInMembers[end]) == nameInFamily(rowsInMembers[begin]))
                ++end;
            std::span<RowInMember> named = rowsInMembers.mutableSpan().subspan(begin, end - begin);
            begin = end;
            // One declaration lies at one offset anyway.
            if (named.front().unit == named.back().unit)
                continue;
            for (RowInMember& row : named)
                row.form = formOf(row.unit);
            AOT::sort(named, [](const RowInMember& a, const RowInMember& b) {
                return std::tuple(a.logWidth, a.form, a.unit) < std::tuple(b.logWidth, b.form, b.unit);
            });
            uint32_t mentions = mentionsThroughFamily(named[0].family, named[0].key);
            for (std::span<const RowInMember> rest = named; !rest.empty();) {
                size_t size = std::ranges::find_if(rest, [&](const RowInMember& row) {
                    return row.logWidth != rest[0].logWidth || row.form != rest[0].form;
                }) - rest.begin();
                if (rest[0].unit != rest[size - 1].unit) {
                    m_groups.append(Group { static_cast<uint32_t>(m_unitsOfGroups.size()), static_cast<uint32_t>(size), mentions });
                    for (const RowInMember& row : rest.first(size))
                        m_unitsOfGroups.append(row.unit);
                }
                rest = rest.subspan(size);
            }
        }

        m_groupsByTurn = Vector<uint32_t>(m_groups.size(), [](size_t group) {
            return static_cast<uint32_t>(group);
        });
        AOT::sort(m_groupsByTurn, [&](uint32_t a, uint32_t b) {
            return m_groups[a].mentions != m_groups[b].mentions ? m_groups[a].mentions > m_groups[b].mentions : a < b;
        });
        for (uint32_t group : m_groupsByTurn) {
            for (UnitIndex unit : unitsOf(m_groups[group])) {
                if (m_units[unit].group == none)
                    m_units[unit].group = group;
            }
        }
    }

    // ---- The cells

    // A cell is as large as its rows need in the order of declaration, and no larger: whatever is placed here changes places with another row.
    CellInUnionSlots& cellOf(LayoutID id)
    {
        CellInUnionSlots& cell = m_cells[id.index()];
        if (cell.isInitialized())
            return cell;

        uint32_t firstRow = m_rows.firstRowOfLayout[id.index()];
        uint32_t endOfRows = m_rows.firstRowOfLayout[id.index() + 1];
        // Two fields of a layout under one name hide one field above: they are one slot.
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
        return cell;
    }

    CellInUnionSlots& cellOfRow(uint32_t row) { return cellOf(m_layoutOfRow[row]); }

    bool fitsEveryRow(UnitIndex unit, uint32_t offset)
    {
        return std::ranges::all_of(rowsOf(unit), [&](uint32_t row) {
            return cellOfRow(row).canPlace(offset, m_units[unit].logWidth);
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
            // The rows of a unit stand in the order of the layouts.
            if (std::exchange(lastLayout, m_layoutOfRow[row]) != m_layoutOfRow[row])
                cellOfRow(row).place(offset, unit.logWidth);
            RELEASE_ASSERT(m_rows.places[row] == RowsOfSlotPlan::noPlace);
            m_rows.offsets[row] = offset;
            m_rows.places[row] = unit.place;
        }
        // A cell that is made counts the units that are placed as taken.
        unit.isPlaced = true;
        m_isTouched.quickSet(unit.declaringLayout.index());
    }

    // Of a unit among these that lies at the offset. Rows of one group at one offset have one place.
    uint32_t placeInUseAt(std::span<const UnitIndex> unitsOfGroup, uint32_t offset) const
    {
        for (UnitIndex unit : unitsOfGroup) {
            if (m_units[unit].isPlaced && m_units[unit].offset == offset)
                return m_units[unit].place;
        }
        return RowsOfSlotPlan::noPlace;
    }

    // `placeAtOffset`: placeInUseAt(), and from the first unit that is placed on the place of that one.
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

    // False: the rows that are left to place fit each cell by their number, which does not say that one offset suits every layout. The caller of placeFieldsOfUnions() lays the rows at the ends.
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

    uint32_t rankingMentionsOf(UnitIndex unit) const
    {
        uint32_t row = m_rowsOfUnits[m_units[unit].firstRow];
        LayoutID id = m_layoutOfRow[row];
        return mentionsOfFieldForUnionSlots(m_link, { id, row - m_rows.firstRowOfLayout[id.index()] });
    }

    void sortIntoPlacementOrder(UnitList& units) const
    {
        AOT::sort(units, [&](UnitIndex a, UnitIndex b) {
            return std::tuple(m_units[a].depth, rankingMentionsOf(b), a) < std::tuple(m_units[b].depth, rankingMentionsOf(a), b);
        });
    }

    void placeWithGroup(UnitIndex unit)
    {
        if (m_units[unit].isPlaced)
            return;
        if (m_units[unit].group != none && m_groups[m_units[unit].group].mentions)
            placeAtSharedOffsets(m_units[unit].group, m_units[unit].depth);
        if (!m_units[unit].isPlaced)
            placeByItself(unit);
    }

    // While the declarations of a class are placed before those of the classes below it, the layouts that hold a declaration have the same bytes free.
    // Then an offset that suits all of them is there, without a cell that grows.
    void placeUnitsDeclaredAbove(std::span<const UnitIndex> units)
    {
        UnitList above;
        auto collectAbove = [&](UnitIndex unit) {
            // What is declared above the layout that declares the unit has a row in that layout.
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
            placeWithGroup(unit);

        // The rest of their groups can only follow them. It does so before a group that can still lie anywhere takes those bytes.
        Vector<uint32_t, 8> groups;
        for (UnitIndex unit : above) {
            if (m_units[unit].group != none && m_groups[m_units[unit].group].mentions)
                groups.append(m_units[unit].group);
        }
        AOT::sort(groups);
        groups.shrink(groups.size() - std::ranges::unique(groups).size());
        for (uint32_t group : groups)
            placeAtOffsetsInUse(m_groups[group]);
    }

    // WHOEVER LAYS WHAT IS LEFT OF A CLASS LAYS IT BEHIND THE END OF THAT CLASS, which is where the fields of the classes below it begin. So no declaration is placed while one above it has no place: the two would come to lie on the same bytes.
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

    // 1.6: a declaration joins an offset that others of its group use already.
    void placeAtOffsetsInUse(const Group& group)
    {
        UnitList unplaced;
        Vector<uint32_t, 8> offsets;
        for (UnitIndex unit : unitsOf(group)) {
            if (m_units[unit].isPlaced)
                offsets.append(m_units[unit].offset);
            else if ((unplaced.isEmpty() || unplaced.last() != unit) && !waitsForUnitsDeclaredAbove(unit))
                unplaced.append(unit);
        }
        for (OffsetUseInUnionSlots use : countOffsetUsesInUnionSlots(WTF::move(offsets)))
            placeUnitsThatFitAt(use.offset, unplaced, unitsOf(group));
    }

    // 1.5: one load reads the field in every member, without a look at the shape.
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

    // 1.6: the offsets that most of the declarations can share. The units lie in different layouts, so which offsets fit one of them does not change when another is placed.
    void placeAtMostSharedOffsets(UnitList& unplaced, std::span<const UnitIndex> unitsOfGroup)
    {
        if (unplaced.size() < 2)
            return;
        unsigned logSize = m_units[unplaced[0]].logWidth;
        auto offsetAt = [&](size_t candidate) {
            return offsetOfFirstField + (static_cast<uint32_t>(candidate) << logSize);
        };

        // By index in `unplaced`: how many offsets lie within the limits of all cells of the unit.
        Vector<size_t, 8> candidatesWithinLimits;
        size_t numberOfCandidates = 0;
        for (UnitIndex unit : unplaced) {
            uint32_t leastLimit = UINT32_MAX;
            for (uint32_t row : rowsOf(unit))
                leastLimit = std::min(leastLimit, cellOfRow(row).limit());
            candidatesWithinLimits.append(static_cast<size_t>(std::max(offsetOfFirstField, leastLimit) - offsetOfFirstField) >> logSize);
            numberOfCandidates = std::max(numberOfCandidates, candidatesWithinLimits.last());
        }
        if (!numberOfCandidates)
            return;

        BitVector fits(unplaced.size() * numberOfCandidates);
        Vector<uint32_t, 64> rowsThatFit(FillWith { }, numberOfCandidates, 0u);
        for (size_t index = 0; index < unplaced.size(); ++index) {
            for (size_t candidate = 0; candidate < candidatesWithinLimits[index]; ++candidate) {
                if (!fitsEveryRow(unplaced[index], offsetAt(candidate)))
                    continue;
                fits.quickSet(index * numberOfCandidates + candidate);
                rowsThatFit[candidate] += m_units[unplaced[index]].numberOfRows;
            }
        }

        while (true) {
            size_t best = std::ranges::max_element(rowsThatFit) - rowsThatFit.begin();
            if (rowsThatFit[best] < 2)
                break;
            uint32_t placeAtOffset = placeInUseAt(unitsOfGroup, offsetAt(best));
            for (size_t index = 0; index < unplaced.size(); ++index) {
                if (!fits.quickGet(index * numberOfCandidates + best))
                    continue;
                uint32_t numberOfRows = m_units[unplaced[index]].numberOfRows;
                if (!tryPlace(unplaced[index], offsetAt(best), placeAtOffset)) {
                    fits.quickClear(index * numberOfCandidates + best);
                    rowsThatFit[best] -= numberOfRows;
                    continue;
                }
                for (size_t candidate = 0; candidate < numberOfCandidates; ++candidate) {
                    if (fits.quickClear(index * numberOfCandidates + candidate))
                        rowsThatFit[candidate] -= numberOfRows;
                }
            }
        }
        unplaced.removeAllMatching([&](UnitIndex unit) {
            return m_units[unit].isPlaced;
        });
    }

    // A unit that is declared deeper than `greatestDepth` waits for the turn of the group.
    void placeAtSharedOffsets(uint32_t groupIndex, uint32_t greatestDepth = UINT32_MAX)
    {
        std::span<const UnitIndex> unitsOfGroup = unitsOf(m_groups[groupIndex]);
        UnitList unplaced;
        for (UnitIndex unit : unitsOfGroup) {
            if (!m_units[unit].isPlaced && m_units[unit].depth <= greatestDepth && (unplaced.isEmpty() || unplaced.last() != unit))
                unplaced.append(unit);
        }
        if (unplaced.isEmpty())
            return;

        placeUnitsDeclaredAbove(unplaced.span());
        unplaced.removeAllMatching([&](UnitIndex unit) {
            return m_units[unit].isPlaced || waitsForUnitsDeclaredAbove(unit);
        });

        Vector<uint32_t, 8> offsets;
        for (UnitIndex unit : unitsOfGroup) {
            if (m_units[unit].isPlaced)
                offsets.append(m_units[unit].offset);
        }
        Vector<OffsetUseInUnionSlots, 8> offsetsInUse = countOffsetUsesInUnionSlots(WTF::move(offsets));
        if (offsetsInUse.size() <= 1)
            tryPlaceAtOneOffset(unplaced, offsetsInUse.span(), unitsOfGroup);
        for (OffsetUseInUnionSlots use : offsetsInUse)
            placeUnitsThatFitAt(use.offset, unplaced, unitsOfGroup);
        placeAtMostSharedOffsets(unplaced, unitsOfGroup);
    }

    // ---- 1.7: bytes that are kept zero

    template<typename Functor>
    bool everyMemberInUse(FamilyID family, const Functor& functor) const
    {
        return m_program.at(m_families.layoutsOf(family)).allOf([&](LayoutID member) {
            if (member.index() < m_link.layouts.size() && !m_link.layout(member).isAllocated)
                return true;
            return functor(member);
        });
    }

    // Null: the member does not list the name.
    UnitIndex findUnitOf(LayoutID member, PropertyKeyID key) const
    {
        std::optional<uint32_t> field = m_program.at(member).findField(key);
        return field ? m_unitOfRow[rowOf({ member, *field })] : none;
    }

    // All that does not hang on offsets. An empty slot costs each object of a layout that lacks the field a store, so there are no more such layouts than layouts that have the field, and no more than places that read it.
    bool emptySlotsMayServe(const LinkedUseThroughFamily& use)
    {
        uint32_t numberOfMembersWithSlot = 0;
        uint32_t numberOfMembersWithoutSlot = 0;
        bool mayServe = everyMemberInUse(use.family, [&](LayoutID member) {
            if (!isTouchable(member) || !m_hasCells(member))
                return false;
            if (m_program.at(member).findField(use.key)) {
                // Zero reads as absent in a slot that holds any value as it is.
                UnitIndex unit = findUnitOf(member, use.key);
                if (unit == none || m_units[unit].isRefused || (1u << m_units[unit].logWidth) != widestSlotInUnionSlots || formOf(unit) != FormInUnionSlots::AnyValue)
                    return false;
                ++numberOfMembersWithSlot;
                return true;
            }
            // It has no appended field either: one of that name may lie anywhere, or among the extra properties.
            return standsByItself(member) && ++numberOfMembersWithoutSlot <= use.reads;
        });
        return mayServe && numberOfMembersWithoutSlot && numberOfMembersWithSlot >= numberOfMembersWithoutSlot;
    }

    void findReadsThatEmptySlotsMayServe()
    {
        for (uint32_t index = 0; index < m_link.usesThroughFamilies.size(); ++index) {
            const LinkedUseThroughFamily& use = m_link.usesThroughFamilies[index];
            if (use.reads && use.family.index() < m_families.size() && emptySlotsMayServe(use))
                m_readsThatEmptySlotsMayServe.append(index);
        }
        AOT::sort(m_readsThatEmptySlotsMayServe, [&](uint32_t a, uint32_t b) {
            uint32_t readsOfA = m_link.usesThroughFamilies[a].reads;
            uint32_t readsOfB = m_link.usesThroughFamilies[b].reads;
            return readsOfA != readsOfB ? readsOfA > readsOfB : a < b;
        });
    }

    bool keepsZero(LayoutID layout, uint32_t offset) const
    {
        return m_bytesKeptZero[layout.index()].containsIf([&](const ReservedBytes& reserved) {
            return reserved.offset == offset;
        });
    }

    // A row that is placed stays where it is, so what is reserved beside it stays right.
    // `x.parent` over A and B where A alone has the field: no group has placed a declaration that stands alone, so it is placed here, where B has room.
    void reserveEmptySlots()
    {
        constexpr unsigned logSize = numberOfWidthsInUnionSlots - 1;
        Vector<LayoutID, 8> membersWithoutSlot;
        UnitList unitsWithSlot;
        UnitList unplaced;
        for (uint32_t index : m_readsThatEmptySlotsMayServe) {
            const LinkedUseThroughFamily& use = m_link.usesThroughFamilies[index];
            membersWithoutSlot.shrink(0);
            unitsWithSlot.shrink(0);
            everyMemberInUse(use.family, [&](LayoutID member) {
                if (UnitIndex unit = findUnitOf(member, use.key); unit != none)
                    unitsWithSlot.append(unit);
                else
                    membersWithoutSlot.append(member);
                return true;
            });
            AOT::sort(unitsWithSlot);
            unitsWithSlot.shrink(unitsWithSlot.size() - std::ranges::unique(unitsWithSlot).size());
            placeUnitsDeclaredAbove(unitsWithSlot.span());

            unplaced.shrink(0);
            std::optional<uint32_t> offset;
            bool hasOneLocation = true;
            for (UnitIndex unit : unitsWithSlot) {
                if (!m_units[unit].isPlaced) {
                    hasOneLocation &= !waitsForUnitsDeclaredAbove(unit);
                    unplaced.append(unit);
                    continue;
                }
                hasOneLocation &= !offset || *offset == m_units[unit].offset;
                offset = m_units[unit].offset;
            }
            if (!hasOneLocation)
                continue;

            auto suitsEveryMember = [&](uint32_t candidate) {
                bool fitsEveryUnit = std::ranges::all_of(unplaced, [&](UnitIndex unit) {
                    return fitsEveryRow(unit, candidate);
                });
                return fitsEveryUnit && std::ranges::all_of(membersWithoutSlot, [&](LayoutID member) {
                    return keepsZero(member, candidate) || cellOf(member).canReserve(candidate, logSize);
                });
            };
            if (offset && !suitsEveryMember(*offset))
                continue;
            if (!offset) {
                uint32_t leastLimit = UINT32_MAX;
                for (LayoutID member : membersWithoutSlot)
                    leastLimit = std::min(leastLimit, cellOf(member).limit());
                for (uint32_t candidate = offsetOfFirstField; !offset && candidate + widestSlotInUnionSlots <= leastLimit; candidate += widestSlotInUnionSlots) {
                    if (suitsEveryMember(candidate))
                        offset = candidate;
                }
            }
            if (!offset)
                continue;

            placeUnitsThatFitAt(*offset, unplaced, unitsWithSlot.span());
            for (LayoutID member : membersWithoutSlot) {
                if (keepsZero(member, *offset))
                    continue;
                cellOf(member).reserve(*offset, logSize);
                m_bytesKeptZero[member.index()].append(ReservedBytes { *offset, widestSlotInUnionSlots });
                m_isTouched.quickSet(member.index());
            }
        }
    }

    // ---- The rest of the layouts that were touched

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

    // Whoever lays the rest lays it at the end, so a layout of which one field lies where its group wants it is filled whole: its other own fields go into the bytes that are left, in the order of declaration.
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
        for (size_t index : m_isTouched) {
            m_rows.ends[index] = std::max<uint64_t>(m_rows.ends[index], cellOf(LayoutID(index)).end());
            AOT::sort(m_bytesKeptZero[index], { }, &ReservedBytes::offset);
        }
    }

    const Program& m_program;
    const LinkTables& m_link;
    const Families& m_families;
    const RowsOfSlotPlan& m_rows;
    const ScopedLambda<bool(LayoutID)>& m_hasCells;
    const ScopedLambda<uint32_t()>& m_addPlace;
    Vector<Vector<ReservedBytes>>& m_bytesKeptZero;
    uint32_t m_numberOfLayouts;
    // By layout: how many written bases stand above it. none: it is never touched.
    Vector<uint32_t> m_depths;
    Vector<LayoutID> m_layoutsBelowTheirBases;
    // By layout: it declares a row that was placed here, or keeps bytes zero.
    BitVector m_isTouched;
    // By layout. Made when first asked for: most layouts share no name with a stranger.
    Vector<CellInUnionSlots> m_cells;
    // By layout: every declaration above this depth with a row in the layout is placed or queued.
    Vector<uint32_t> m_depthsReached;
    // By layout: waitsForUnitsDeclaredAbove() has found every one of them placed.
    BitVector m_allAboveIsPlaced;
    // By row.
    Vector<UnitIndex> m_unitOfRow;
    Vector<LayoutID> m_layoutOfRow;
    Vector<Unit> m_units;
    Vector<uint32_t> m_rowsOfUnits;
    Vector<Group> m_groups;
    Vector<UnitIndex> m_unitsOfGroups;
    Vector<uint32_t> m_groupsByTurn;
    // Indexes into LinkTables::usesThroughFamilies, by falling reads.
    Vector<uint32_t> m_readsThatEmptySlotsMayServe;
};

} // anonymous namespace

void placeFieldsOfUnions(const Program& program, const LinkTables& link, const Families& families, RowsOfSlotPlan rows, NOESCAPE const ScopedLambda<bool(LayoutID)>& hasCells, NOESCAPE const ScopedLambda<uint32_t()>& addPlace, Vector<Vector<ReservedBytes>>& bytesKeptZero)
{
    bytesKeptZero.clear();
    bytesKeptZero.grow(program.layouts.size());
    PlacerOfUnionSlots(program, link, families, rows, hasCells, addPlace, bytesKeptZero).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
