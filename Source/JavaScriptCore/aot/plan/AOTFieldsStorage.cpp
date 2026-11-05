/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFieldsStorage.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTUnionPlan.h"
#include "AOTUnitedFamilies.h"
#include "MarkedSpace.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t bytesPerSlotInFieldsStorage = widthOf(SlotRepresentation::JSValue);

static_assert(!(offsetOfFirstField % bytesPerSlotInFieldsStorage));

bool hasTypedSlotsForFields(const Facts& facts)
{
    return withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedSlots);
}

GroupsOfLayoutsForFields groupLayoutsForFields(const Program& program, const Families& families)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    auto isObject = [&](uint32_t index) {
        return isObjectLayout(program.layouts[index].kind);
    };
    UnitedFamilies united(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (LayoutID base = isObject(index) ? writtenBaseOf(program.layouts[index]) : LayoutID())
            united.unite(index, base.index());
    }
    for (uint32_t number = 0; number < families.size(); ++number) {
        if (families.nearestFamilyAbove(FamilyID(number)))
            continue;
        LayoutID first;
        program.at(families.layoutsOf(FamilyID(number))).forEach([&](LayoutID member) {
            if (!first)
                first = member;
            united.unite(first.index(), member.index());
        });
    }

    GroupsOfLayoutsForFields groups;
    groups.groupOf.fill(GroupsOfLayoutsForFields::noGroup, numberOfLayouts);
    Vector<uint32_t> lowestLayoutOf;
    lowestLayoutOf.fill(GroupsOfLayoutsForFields::noGroup, numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!isObject(index))
            continue;
        uint32_t& lowest = lowestLayoutOf[united.representativeOf(index)];
        if (lowest == GroupsOfLayoutsForFields::noGroup)
            lowest = index;
        groups.groupOf[index] = lowest;
        groups.layouts.append(index);
    }
    AOT::sort(groups.layouts, { }, [&](uint32_t index) {
        return std::pair { groups.groupOf[index], index };
    });
    return groups;
}

UnitsOfFields collectUnitsOfFields(const Program& program, const SlotPlanParts& parts, const GroupsOfLayoutsForFields& groups)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    UnitsOfFields result;
    result.firstRowOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    uint64_t numberOfRows = 0;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        result.firstRowOfLayout.append(static_cast<uint32_t>(numberOfRows));
        numberOfRows += program.layouts[index].fields.size() + parts.layouts[index].appendedKeys.size();
    }
    RELEASE_ASSERT(numberOfRows < UnitsOfFields::noUnit);
    result.firstRowOfLayout.append(static_cast<uint32_t>(numberOfRows));
    result.unitOfRow.fill(UnitsOfFields::noUnit, numberOfRows);

    uint32_t numberOfKeys = program.types.numberOfPropertyKeys();
    Vector<uint32_t> lastUnitUnderKey;
    lastUnitUnderKey.fill(UnitsOfFields::noUnit, numberOfKeys);
    uint32_t groupAtHand = GroupsOfLayoutsForFields::noGroup;
    uint32_t firstUnitOfGroup = 0;
    auto noteRow = [&](FieldReference row, PropertyKeyID key) {
        if (!key || key.index() >= numberOfKeys)
            return;
        uint32_t& unit = lastUnitUnderKey[key.index()];
        if (unit == UnitsOfFields::noUnit || unit < firstUnitOfGroup) {
            unit = static_cast<uint32_t>(result.units.size());
            result.units.append(UnitsOfFields::Unit { groupAtHand, 0, 0 });
        }
        ++result.units[unit].numberOfRows;
        result.unitOfRow[result.rowOf(row)] = unit;
    };
    for (uint32_t index : groups.layouts) {
        if (groups.groupOf[index] != groupAtHand) {
            groupAtHand = groups.groupOf[index];
            firstUnitOfGroup = static_cast<uint32_t>(result.units.size());
        }
        LayoutID id(index);
        const Layout& layout = program.layouts[index];
        uint32_t numberListed = static_cast<uint32_t>(layout.fields.size());
        for (uint32_t field = 0; field < numberListed; ++field) {
            PropertyKeyID key = layout.fields[field].key;
            if (key && layout.findField(key) == field)
                noteRow({ id, field }, key);
        }
        const Vector<PropertyKeyID>& appendedKeys = parts.layouts[index].appendedKeys;
        for (uint32_t place = 0; place < appendedKeys.size(); ++place)
            noteRow({ id, numberListed + place }, appendedKeys[place]);
    }

    uint32_t numberOfRowsInUnits = 0;
    for (UnitsOfFields::Unit& unit : result.units) {
        unit.firstRow = numberOfRowsInUnits;
        numberOfRowsInUnits += std::exchange(unit.numberOfRows, 0);
    }
    result.rows.grow(numberOfRowsInUnits);
    for (uint32_t index : groups.layouts) {
        for (uint32_t row = result.firstRowOfLayout[index]; row < result.firstRowOfLayout[index + 1]; ++row) {
            if (result.unitOfRow[row] == UnitsOfFields::noUnit)
                continue;
            UnitsOfFields::Unit& unit = result.units[result.unitOfRow[row]];
            result.rows[unit.firstRow + unit.numberOfRows++] = { LayoutID(index), row - result.firstRowOfLayout[index] };
        }
    }
    return result;
}

BitVector findGroupsLaidForGoodForFields(const SlotPlanParts& parts, const GroupsOfLayoutsForFields& groups)
{
    BitVector groupsLaidForGood(parts.layouts.size());
    for (uint32_t index : groups.layouts) {
        if (hasCellsForFields(parts, LayoutID(index)) && parts.layouts[index].isEscapedOffset)
            groupsLaidForGood.quickSet(groups.groupOf[index]);
    }
    return groupsLaidForGood;
}

CellSizesForFields::CellSizesForFields(const BuildContext& build)
    : m_sizes(MarkedSpace::sizeClassesFor(build.options().sizeClassProgression))
{
}

uint64_t CellSizesForFields::cellSizeFor(uint64_t bytes) const
{
    auto sizeClass = std::ranges::lower_bound(m_sizes, bytes);
    return sizeClass == m_sizes.end() ? WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(bytes) : static_cast<uint64_t>(*sizeClass);
}

bool isPlainFieldByRecords(const Program& program, const Facts& facts, FieldReference reference)
{
    constexpr OptionSet<FieldFlags> flagsOfFieldsOfOtherKinds { FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods, FieldFlags::IsHidden };
    const Layout& layout = program.at(reference.layout);
    PropertyKeyID key;
    if (reference.field < layout.fields.size()) {
        const Field& declaration = fieldAt(program, declarationOf(program, reference));
        if (!declaration.form.isEmpty() || declaration.flags.containsAny(flagsOfFieldsOfOtherKinds))
            return false;
        key = declaration.key;
    } else {
        std::span<const PropertyKeyID> appendedKeys = facts.linked().appendedKeysOf(reference.layout);
        size_t place = reference.field - layout.fields.size();
        if (place < appendedKeys.size())
            key = appendedKeys[place];
    }
    if (!key || program.at(key).kind != PropertyKeyKind::Text)
        return false;
    FieldFacts fieldFacts = facts.field(reference);
    return fieldFacts.formOfValue.isEmpty() && !fieldFacts.facts.contains(FieldFact::IsAccessor) && fieldFacts.location.deferral == ClosureDeferral::Unknown;
}

bool isPlainSlotOfField(const PlannedLayout& planned, FieldIndex field)
{
    const PlacedSlot& slot = planned.slots[field];
    return !slot.holdsAccessor && !slot.isFilledAtBirth && !slot.deferredFunction && slot.representation != SlotRepresentation::DeferredClosure
        && planned.environmentOfDeferredClosures != field && !planned.holdsRawValueFromBirth(field);
}

namespace {

struct FreeBytesInFieldsStorage {
    uint64_t take(uint32_t width)
    {
        unsigned wanted = WTF::fastLog2(width);
        RELEASE_ASSERT(width == 1u << wanted && wanted <= runs.size());
        unsigned found = wanted;
        while (found < runs.size() && !runs[found])
            ++found;
        uint64_t offset = found < runs.size() ? std::exchange(runs[found], 0) : std::exchange(end, end + bytesPerSlotInFieldsStorage);
        while (found-- > wanted)
            runs[found] = offset + (1ull << found);
        return offset;
    }
    friend bool operator==(const FreeBytesInFieldsStorage&, const FreeBytesInFieldsStorage&) = default;

    uint64_t end { 0 };
    std::array<uint64_t, 3> runs { };
};

class BytesOfCellInFieldsStorage {
public:
    void reset(uint64_t offsetOfFirstSlot)
    {
        m_taken.shrink(0);
        m_firstWordWithRoom.fill(0);
        m_end = 0;
        for (uint64_t offset = 0; offset < offsetOfFirstSlot; offset += bytesPerSlotInFieldsStorage)
            occupy(offset, bytesPerSlotInFieldsStorage);
    }

    bool isFree(uint64_t offset, uint64_t width) const
    {
        for (uint64_t byte = offset; byte < offset + width; ++byte) {
            if (byte / bytesPerWord < m_taken.size() && m_taken[byte / bytesPerWord] & 1ull << byte % bytesPerWord)
                return false;
        }
        return true;
    }

    void occupy(uint64_t offset, uint64_t width)
    {
        for (uint64_t byte = offset; byte < offset + width; ++byte) {
            while (byte / bytesPerWord >= m_taken.size())
                m_taken.append(0);
            m_taken[byte / bytesPerWord] |= 1ull << byte % bytesPerWord;
        }
        m_end = std::max(m_end, offset + width);
    }

    uint64_t take(uint32_t width)
    {
        unsigned logarithm = WTF::fastLog2(width);
        RELEASE_ASSERT(width == 1u << logarithm && logarithm < m_firstWordWithRoom.size());
        uint64_t bitsOfSlot = (1ull << width) - 1;
        for (size_t word = m_firstWordWithRoom[logarithm];; ++word) {
            if (word == m_taken.size())
                m_taken.append(0);
            for (unsigned bit = 0; m_taken[word] != UINT64_MAX && bit < bytesPerWord; bit += width) {
                if (m_taken[word] & bitsOfSlot << bit)
                    continue;
                m_taken[word] |= bitsOfSlot << bit;
                m_firstWordWithRoom[logarithm] = word;
                uint64_t offset = word * bytesPerWord + bit;
                m_end = std::max(m_end, offset + width);
                return offset;
            }
        }
    }

    uint64_t end() const { return m_end; }

private:
    static constexpr uint64_t bytesPerWord = 64;

    Vector<uint64_t, 16> m_taken;
    std::array<size_t, 4> m_firstWordWithRoom { };
    uint64_t m_end { 0 };
};

class ChooserOfFieldsStorage {
    WTF_MAKE_NONCOPYABLE(ChooserOfFieldsStorage);
public:
    ChooserOfFieldsStorage(Program&, const Facts&, BuildContext&, SlotPlanParts&);

    void run();

private:
    static constexpr uint32_t noPlaceInCell = RowsOfSlotPlan::noPlace;

    uint32_t numberOfLayouts() const { return static_cast<uint32_t>(m_program.layouts.size()); }
    bool hasCells(LayoutID id) const { return hasCellsForFields(m_parts, id); }
    const PlacedSlot& slotOf(FieldReference row) const { return m_parts.layouts[row.layout.index()].slots[row.field]; }
    uint64_t offsetOfFirstSlotOf(LayoutID id) const { return offsetOfFirstField + (m_layoutsWithHiddenWords.quickGet(id.index()) ? bytesPerSlotInFieldsStorage : 0); }
    bool isLaidAgain(uint32_t layout) const { return m_groups.groupOf[layout] != GroupsOfLayoutsForFields::noGroup && m_groupsLaidAgain.quickGet(m_groups.groupOf[layout]); }
    void layGroupAgain(LayoutID member) { m_groupsLaidAgain.quickSet(m_groups.groupOf[member.index()]); }

    void forEachLayoutWithCells(const Invocable<void(LayoutID, PlannedLayout&)> auto& functor)
    {
        for (uint32_t index : m_groups.layouts) {
            if (hasCells(LayoutID(index)))
                functor(LayoutID(index), m_parts.layouts[index]);
        }
    }

    void chooseForms();

    void findGroupsWithHiddenWords();
    void prepareRows();
    BitVector findFamiliesThatMayAgreeOnSlots() const;
    void findMembersThatAgreeOnSlots(FamilyID, Vector<LayoutID, 16>&) const;
    void layFieldsSharedByFamilies();
    void layFieldsOfUnions();
    std::optional<FieldReference> findFieldAbove(const BitVector& isLaid, FieldReference, LayoutID base) const;
    void layFieldsOf(LayoutID, const BitVector& isLaid);
    void finishCell(LayoutID);
    void layCellsAgain();

    Program& m_program;
    const Facts& m_facts;
    const LinkTables& m_link;
    BuildContext& m_build;
    SlotPlanParts& m_parts;
    GroupsOfLayoutsForFields m_groups;
    UnitsOfFields m_units;

    BitVector m_groupsLaidAgain;

    BitVector m_layoutsWithHiddenWords;

    Vector<uint64_t> m_offsets;
    Vector<uint32_t> m_placesInCells;
    Vector<uint8_t> m_widths;
    Vector<uint64_t> m_ends;
    uint32_t m_numberOfPlacesInCells { 0 };
    BytesOfCellInFieldsStorage m_cell;
};

ChooserOfFieldsStorage::ChooserOfFieldsStorage(Program& program, const Facts& facts, BuildContext& build, SlotPlanParts& parts)
    : m_program(program)
    , m_facts(facts)
    , m_link(facts.linked())
    , m_build(build)
    , m_parts(parts)
    , m_groups(groupLayoutsForFields(program, facts.families()))
    , m_units(collectUnitsOfFields(program, parts, m_groups))
    , m_groupsLaidAgain(program.layouts.size())
    , m_layoutsWithHiddenWords(program.layouts.size())
{
}

void ChooserOfFieldsStorage::run()
{
    chooseForms();
    findGroupsWithHiddenWords();
    layCellsAgain();
}

void ChooserOfFieldsStorage::chooseForms()
{
    Vector<uint8_t> widthsInFrontOfForms;
    widthsInFrontOfForms.fill(0, m_units.numberOfRows());
    forEachLayoutWithCells([&](LayoutID id, PlannedLayout& planned) {
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field)
            widthsInFrontOfForms[m_units.rowOf({ id, field })] = static_cast<uint8_t>(planned.slots[field].width());
    });
    chooseFormsOfSlots(m_program, m_facts, m_build, m_parts, m_groups, m_units);
    forEachLayoutWithCells([&](LayoutID id, PlannedLayout& planned) {
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            if (planned.slots[field].storage == SlotStorage::Inline && widthsInFrontOfForms[m_units.rowOf({ id, field })] != planned.slots[field].width())
                layGroupAgain(id);
        }
    });
}

void ChooserOfFieldsStorage::findGroupsWithHiddenWords()
{
    BitVector groupsWithWords(numberOfLayouts());
    forEachLayoutWithCells([&](LayoutID id, PlannedLayout& planned) {
        bool needsWords = planned.hasExtraPropertiesWord() || planned.slots.containsIf([](const PlacedSlot& slot) {
            return slot.isOutOfLine();
        });
        if (needsWords)
            groupsWithWords.quickSet(m_groups.groupOf[id.index()]);
    });
    forEachLayoutWithCells([&](LayoutID id, PlannedLayout& planned) {
        if (groupsWithWords.quickGet(m_groups.groupOf[id.index()]) && !planned.hasExtraPropertiesWord())
            layGroupAgain(id);
    });
    for (uint32_t index : m_groups.layouts) {
        if (groupsWithWords.quickGet(m_groups.groupOf[index]))
            m_layoutsWithHiddenWords.quickSet(index);
    }
}

void ChooserOfFieldsStorage::prepareRows()
{
    m_offsets.fill(0, m_units.numberOfRows());
    m_placesInCells.fill(noPlaceInCell, m_units.numberOfRows());
    m_widths.fill(0, m_units.numberOfRows());
    m_ends.fill(0, numberOfLayouts());

    auto forEachListedFieldLaidAgain = [&](bool ofLayoutsWithCells, const auto& functor) {
        for (uint32_t index : m_groups.layouts) {
            if (!isLaidAgain(index) || hasCells(LayoutID(index)) != ofLayoutsWithCells)
                continue;
            for (uint32_t field = 0; field < m_program.layouts[index].fields.size(); ++field)
                functor(FieldReference { LayoutID(index), field });
        }
    };
    forEachListedFieldLaidAgain(true, [&](FieldReference row) {
        if (slotOf(row).storage == SlotStorage::Inline)
            m_widths[m_units.rowOf(row)] = static_cast<uint8_t>(slotOf(row).width());
    });
    forEachListedFieldLaidAgain(false, [&](FieldReference row) {
        m_widths[m_units.rowOf(row)] = bytesPerSlotInFieldsStorage;
    });
    forEachListedFieldLaidAgain(true, [&](FieldReference row) {
        FieldReference declaration = declarationOf(m_program, row);
        if (declaration.layout != row.layout && !hasCells(declaration.layout))
            m_widths[m_units.rowOf(declaration)] = m_widths[m_units.rowOf(row)];
    });
    forEachListedFieldLaidAgain(false, [&](FieldReference row) {
        FieldReference declaration = declarationOf(m_program, row);
        if (declaration.layout != row.layout)
            m_widths[m_units.rowOf(row)] = m_widths[m_units.rowOf(declaration)];
    });
}

BitVector ChooserOfFieldsStorage::findFamiliesThatMayAgreeOnSlots() const
{
    const Families& families = m_facts.families();
    BitVector result = families.familiesWithSomeMemberThat([&](LayoutID member) {
        return hasCells(member);
    });
    result.exclude(families.familiesWithSomeMemberThat([&](LayoutID member) {
        return hasCells(member) && (!isFoundByShapeTest(m_program.at(member)) || writtenBaseOf(m_program.at(member)));
    }));
    return result;
}

void ChooserOfFieldsStorage::findMembersThatAgreeOnSlots(FamilyID family, Vector<LayoutID, 16>& members) const
{
    members.shrink(0);
    bool everyMemberIsFree = true;
    m_program.at(m_facts.families().layoutsOf(family)).forEach([&](LayoutID member) {
        if (!hasCells(member))
            return;
        members.append(member);
        everyMemberIsFree &= isFoundByShapeTest(m_program.at(member)) && !writtenBaseOf(m_program.at(member));
    });
    if (members.size() < 2 || !everyMemberIsFree)
        members.shrink(0);
}

void ChooserOfFieldsStorage::layFieldsSharedByFamilies()
{
    const Families& families = m_facts.families();
    Vector<FreeBytesInFieldsStorage> freeBytes(numberOfLayouts());
    auto freeBytesOf = [&](LayoutID id) {
        FreeBytesInFieldsStorage free = freeBytes[id.index()];
        if (!free.end)
            free.end = offsetOfFirstSlotOf(id);
        return free;
    };
    Vector<LayoutID, 16> members;
    Vector<uint32_t, 16> rows;
    BitVector isListed;
    BitVector familiesThatMayAgree = findFamiliesThatMayAgreeOnSlots();
    for (uint32_t number = families.size(); number--;) {
        if (!familiesThatMayAgree.quickGet(number))
            continue;
        findMembersThatAgreeOnSlots(FamilyID(number), members);
        if (members.isEmpty() || !isLaidAgain(members[0].index()))
            continue;
        FreeBytesInFieldsStorage free = freeBytesOf(members[0]);
        bool membersStandAlike = std::ranges::all_of(members, [&](LayoutID member) {
            return freeBytesOf(member) == free;
        });
        if (!membersStandAlike)
            continue;

        const Layout& first = m_program.at(members[0]);
        auto layInAllMembers = [&](uint32_t field) {
            if (field >= first.fields.size() || isListed.set(field))
                return;
            PropertyKeyID key = first.fields[field].key;
            if (!key || first.findField(key) != field)
                return;
            uint8_t width = m_widths[m_units.rowOf({ members[0], field })];
            rows.shrink(0);
            for (LayoutID member : members) {
                std::optional<uint32_t> listed = m_program.at(member).findField(key);
                if (!listed)
                    return;
                uint32_t row = m_units.rowOf({ member, *listed });
                if (m_placesInCells[row] != noPlaceInCell || !width || m_widths[row] != width)
                    return;
                rows.append(row);
            }
            uint64_t offset = free.take(width);
            for (uint32_t row : rows) {
                m_offsets[row] = offset;
                m_placesInCells[row] = m_numberOfPlacesInCells;
            }
            ++m_numberOfPlacesInCells;
        };
        isListed.clearAll();
        for (uint32_t field : first.declaredOrder)
            layInAllMembers(field);
        for (uint32_t field = 0; field < first.fields.size(); ++field)
            layInAllMembers(field);
        for (LayoutID member : members) {
            freeBytes[member.index()] = free;
            m_ends[member.index()] = free.end;
        }
    }
}

void ChooserOfFieldsStorage::layFieldsOfUnions()
{
    auto hasCellsLaidAgain = [&](LayoutID id) {
        return isLaidAgain(id.index()) && hasCells(id);
    };
    auto addPlace = [&] {
        return m_numberOfPlacesInCells++;
    };
    RowsOfSlotPlan rows { m_units.firstRowOfLayout.span(), m_offsets.mutableSpan(), m_placesInCells.mutableSpan(), m_ends.mutableSpan(), m_layoutsWithHiddenWords, m_widths.span() };
    placeFieldsOfUnions(m_program, m_link, m_facts.families(), rows, scopedLambda<bool(LayoutID)>(hasCellsLaidAgain), scopedLambda<uint32_t()>(addPlace));
}

std::optional<FieldReference> ChooserOfFieldsStorage::findFieldAbove(const BitVector& isLaid, FieldReference reference, LayoutID base) const
{
    const Field& listed = fieldAt(m_program, reference);
    FieldReference above;
    if (listed.flags.contains(FieldFlags::IsInherited))
        above = declarationOf(m_program, reference);
    else if (std::optional<uint32_t> hidden = m_program.at(base).findField(listed.key))
        above = { base, *hidden };
    else
        return std::nullopt;
    if (above.layout == reference.layout || !isLaid.quickGet(above.layout.index()) || !isObjectLayout(m_program.at(above.layout).kind))
        return std::nullopt;
    return above;
}

void ChooserOfFieldsStorage::layFieldsOf(LayoutID id, const BitVector& isLaid)
{
    const Layout& layout = m_program.at(id);
    if (!isObjectLayout(layout.kind))
        return;
    LayoutID base = writtenBaseOf(layout);
    if (base && (!isLaid.quickGet(base.index()) || !isObjectLayout(m_program.at(base).kind)))
        base = { };

    uint32_t numberListed = static_cast<uint32_t>(layout.fields.size());
    auto isYetToBeLaid = [&](uint32_t row) {
        return m_placesInCells[row] == noPlaceInCell && m_widths[row];
    };
    m_cell.reset(offsetOfFirstSlotOf(id));
    for (uint32_t field = 0; field < numberListed; ++field) {
        uint32_t row = m_units.rowOf({ id, field });
        if (m_placesInCells[row] != noPlaceInCell && m_widths[row])
            m_cell.occupy(m_offsets[row], m_widths[row]);
    }

    for (uint32_t field = 0; base && field < numberListed; ++field) {
        uint32_t row = m_units.rowOf({ id, field });
        std::optional<FieldReference> above = isYetToBeLaid(row) ? findFieldAbove(isLaid, { id, field }, base) : std::nullopt;
        if (!above)
            continue;
        uint32_t rowAbove = m_units.rowOf(*above);
        if (m_placesInCells[rowAbove] == noPlaceInCell || m_widths[rowAbove] != m_widths[row] || !m_cell.isFree(m_offsets[rowAbove], m_widths[row]))
            continue;
        m_cell.occupy(m_offsets[rowAbove], m_widths[row]);
        m_offsets[row] = m_offsets[rowAbove];
        m_placesInCells[row] = m_placesInCells[rowAbove];
    }
    auto layWhereRoomIs = [&](uint32_t field) {
        if (field >= numberListed)
            return;
        uint32_t row = m_units.rowOf({ id, field });
        if (!isYetToBeLaid(row))
            return;
        m_offsets[row] = m_cell.take(m_widths[row]);
        m_placesInCells[row] = m_numberOfPlacesInCells++;
    };
    for (uint32_t field : layout.declaredOrder)
        layWhereRoomIs(field);
    for (uint32_t field = 0; field < numberListed; ++field)
        layWhereRoomIs(field);
    m_ends[id.index()] = m_cell.end();
    if (hasCells(id))
        finishCell(id);
}

void ChooserOfFieldsStorage::finishCell(LayoutID id)
{
    PlannedLayout& planned = m_parts.layouts[id.index()];
    uint32_t numberListed = static_cast<uint32_t>(m_program.at(id).fields.size());
    bool hasSlotOutOfLine = false;
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        PlacedSlot& slot = planned.slots[field];
        hasSlotOutOfLine |= slot.isOutOfLine();
        if (slot.storage != SlotStorage::Inline)
            continue;
        slot.offset = static_cast<uint32_t>(field < numberListed ? m_offsets[m_units.rowOf({ id, field })] : m_cell.take(slot.width()));
    }

    PlacedSlot& extension = planned.slots[planned.extensionWordIndex()];
    PlacedSlot& extraProperties = planned.slots[planned.extraPropertiesWordIndex()];
    extension = PlacedSlot { };
    extraProperties = PlacedSlot { };
    RELEASE_ASSERT(!hasSlotOutOfLine || m_layoutsWithHiddenWords.quickGet(id.index()));
    if (m_layoutsWithHiddenWords.quickGet(id.index())) {
        SlotRepresentation hiddenReference = hiddenReferenceRepresentation(m_build.recordedOptions().useWideReferences());
        extraProperties.representation = hiddenReference;
        extraProperties.storage = SlotStorage::Inline;
        extraProperties.offset = offsetOfFirstField;
        if (hasSlotOutOfLine) {
            extension.representation = hiddenReference;
            extension.storage = SlotStorage::Inline;
            extension.offset = offsetOfFirstField;
            if (2 * widthOf(hiddenReference) <= bytesPerSlotInFieldsStorage)
                extraProperties.offset = offsetOfFirstField + widthOf(hiddenReference);
            else
                extraProperties.offset = static_cast<uint32_t>(m_cell.take(bytesPerSlotInFieldsStorage));
        }
    }

    uint64_t size = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(m_cell.end());
    if (size > m_build.limit(LimitID::MaxFieldOffset)) {
        m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id, .limit = LimitID::MaxFieldOffset, .count = clampTo<uint32_t>(size) });
        planned = PlannedLayout { };
        return;
    }
    planned.size = static_cast<uint32_t>(size);
}

void ChooserOfFieldsStorage::layCellsAgain()
{
    if (m_groupsLaidAgain.isEmpty())
        return;
    prepareRows();
    layFieldsSharedByFamilies();
    layFieldsOfUnions();

    BitVector isLaid(numberOfLayouts());
    BitVector isWaiting(numberOfLayouts());
    Vector<LayoutID, 16> waiting;
    for (uint32_t index : m_groups.layouts) {
        if (!isLaidAgain(index))
            continue;
        for (LayoutID current(index); current && !isLaid.quickGet(current.index()) && !isWaiting.quickSet(current.index()); current = writtenBaseOf(m_program.at(current)))
            waiting.append(current);
        while (!waiting.isEmpty()) {
            LayoutID id = waiting.takeLast();
            layFieldsOf(id, isLaid);
            isLaid.quickSet(id.index());
        }
    }
}

} // anonymous namespace

void chooseStorageOfFields(Program& program, const Facts& facts, BuildContext& build, SlotPlanParts& parts)
{
    if (!hasTypedSlotsForFields(facts))
        return;
    ChooserOfFieldsStorage(program, facts, build, parts).run();
}

static bool isNarrowFormForFieldsStorage(SlotRepresentation representation)
{
    switch (representation) {
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::OptionalInt32:
    case SlotRepresentation::Cell32:
    case SlotRepresentation::OptionalCell32:
        return true;
    default:
        return false;
    }
}

void checkStorageOfFields(const Program& program, const Facts& facts, const Plan& plan, BuildContext& build)
{
    auto expect = [&](bool holds, LayoutID layout, ASCIILiteral words) {
        if (!holds)
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::PlacementCheckFailed, .layout = layout, .detail = String(words) });
    };
    auto hasCellsInUse = [&](LayoutID id) {
        return plan.isInUse(id) && plan.layout(id).holdsCompiledObjects() && !plan.layout(id).slots.isEmpty();
    };
    const LinkTables& linked = facts.linked();
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID id(index);
        if (!hasCellsInUse(id))
            continue;
        const Layout& layout = program.layouts[index];
        const PlannedLayout& planned = plan.layout(id);
        LayoutID base = writtenBaseOf(layout);
        if (base && !hasCellsInUse(base))
            base = { };

        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            const PlacedSlot& slot = planned.slots[field];
            if (slot.hasStorage() && isNarrowFormForFieldsStorage(slot.representation) && planned.environmentOfDeferredClosures != field) {
                const LinkedCell* cell = linked.findCellOfField({ id, field });
                expect((cell && cell->everyWriterIsKnown && cell->proven) || (slot.type && canLeaveSlot(slot.representation)), id, "a slot has a narrow form, Link does not know every writer of its field, and the slot is held to no type or its field cannot leave it"_s);
            }
            if (!base || field >= layout.fields.size() || !layout.fields[field].flags.contains(FieldFlags::IsInherited))
                continue;
            if (std::optional<uint32_t> above = program.at(base).findField(layout.fields[field].key)) {
                const PlacedSlot& slotAbove = plan.slot(base, *above);
                expect(slot.storage == slotAbove.storage && slot.representation == slotAbove.representation, id, "a field that the layout has from the layout above has another storage or another form there"_s);
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
