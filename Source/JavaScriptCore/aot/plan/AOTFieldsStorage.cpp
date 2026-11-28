/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFieldsStorage.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
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
#include <wtf/HashMap.h>
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

UnitsOfFields collectUnitsOfFields(const Program& program, const LinkTables& linked, const StorageOfLayouts& storage, const GroupsOfLayoutsForFields& groups)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    UnitsOfFields result;
    result.firstRowOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    uint64_t numberOfRows = 0;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        result.firstRowOfLayout.append(static_cast<uint32_t>(numberOfRows));
        numberOfRows += program.layouts[index].fields.size() + (storage.layouts[index].holdsCompiledObjects() ? linked.numberOfAppendedKeysOfPlan(LayoutID(index)) : 0);
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
        uint32_t numberNotRepeated = numberOfFieldsNotRepeated(program, storage.layoutsThatRepeatTheirBases, id);
        for (uint32_t field = 0; field < numberNotRepeated; ++field) {
            PropertyKeyID key = layout.fields[field].key;
            if (key && layout.findField(key) == field)
                noteRow({ id, field }, key);
        }
        if (!storage.layouts[index].holdsCompiledObjects())
            continue;
        uint32_t fieldOfNextKey = numberListed;
        for (PropertyKeyID key : linked.appendedKeysOf(id))
            noteRow({ id, fieldOfNextKey++ }, key);
        for (PropertyKeyID key : linked.keysAppendedByHeldAloneOf(id))
            noteRow({ id, fieldOfNextKey++ }, key);
    }

    uint32_t numberOfRowsInUnits = 0;
    for (UnitsOfFields::Unit& unit : result.units) {
        unit.firstRow = numberOfRowsInUnits;
        numberOfRowsInUnits += std::exchange(unit.numberOfRows, 0);
    }
    result.rows.grow(numberOfRowsInUnits);
    for (uint32_t index : groups.layouts) {
        uint32_t firstRow = result.firstRowOfLayout[index];
        auto listRows = [&](uint32_t row, uint32_t end) {
            for (; row < end; ++row) {
                if (result.unitOfRow[row] == UnitsOfFields::noUnit)
                    continue;
                UnitsOfFields::Unit& unit = result.units[result.unitOfRow[row]];
                result.rows[unit.firstRow + unit.numberOfRows++] = { LayoutID(index), row - firstRow };
            }
        };
        listRows(firstRow, firstRow + numberOfFieldsNotRepeated(program, storage.layoutsThatRepeatTheirBases, LayoutID(index)));
        listRows(firstRow + static_cast<uint32_t>(program.layouts[index].fields.size()), result.firstRowOfLayout[index + 1]);
    }
    return result;
}

BitVector findGroupsLaidForGoodForFields(const SlotPlanParts& parts, const GroupsOfLayoutsForFields& groups)
{
    BitVector groupsLaidForGood(parts.layouts.size());
    for (uint32_t index : groups.layouts) {
        if (hasCellsForFields(parts, LayoutID(index)) && parts.layouts[index].cellsAreHandedBack)
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
    return fieldFacts.formOfValue.isEmpty() && !fieldFacts.facts.contains(FieldFact::IsAccessor);
}

bool isPlainSlotOfField(const PlannedLayout& planned, FieldIndex field)
{
    const PlacedSlot& slot = planned.slots[field];
    return !slot.holdsAccessor && !slot.isFilledAtBirth && !slot.deferredFunction && slot.representation != SlotRepresentation::DeferredClosure
        && planned.environmentOfDeferredClosures != field && !planned.holdsRawValueFromBirth(field);
}

namespace {

struct FreeBytesInFieldsStorage {
    static FreeBytesInFieldsStorage behind(uint64_t offsetOfFirstSlot)
    {
        FreeBytesInFieldsStorage free;
        for (unsigned logarithm = 0; logarithm < free.runs.size(); ++logarithm) {
            if (!(offsetOfFirstSlot & 1ull << logarithm))
                continue;
            free.runs[logarithm] = offsetOfFirstSlot;
            offsetOfFirstSlot += 1ull << logarithm;
        }
        free.end = offsetOfFirstSlot;
        return free;
    }

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
        occupy(0, offsetOfFirstSlot);
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

    std::optional<uint64_t> tryTakeBelow(uint32_t width, uint64_t limit)
    {
        for (uint64_t offset = 0; offset + width <= limit; offset += width) {
            if (!isFree(offset, width))
                continue;
            occupy(offset, width);
            return offset;
        }
        return std::nullopt;
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
    ChooserOfFieldsStorage(Program&, const Facts&, BuildContext&, SlotPlanParts&, const LayoutsBelowTheirBases&, GroupsOfLayoutsForFields&&, UnitsOfFields&&, std::span<const LayoutSetID> fewLayoutsWithoutFamily);

    void run();

private:
    static constexpr uint32_t noPlaceInCell = RowsOfSlotPlan::noPlace;

    uint32_t numberOfLayouts() const { return static_cast<uint32_t>(m_program.layouts.size()); }
    bool hasCells(LayoutID id) const { return hasCellsForFields(m_parts, id); }
    const PlacedSlot& slotOf(FieldReference row) const { return m_parts.layouts[row.layout.index()].slots[row.field]; }
    uint64_t offsetOfFirstSlotOf(LayoutID id) const { return m_offsetsOfFirstSlots[id.index()]; }
    bool isLaidAgain(uint32_t layout) const { return m_groups.groupOf[layout] != GroupsOfLayoutsForFields::noGroup && m_groupsLaidAgain.quickGet(m_groups.groupOf[layout]); }
    void layGroupAgain(LayoutID member) { m_groupsLaidAgain.quickSet(m_groups.groupOf[member.index()]); }

    void forEachLayoutWithCells(const Invocable<void(LayoutID, PlannedLayout&)> auto& functor)
    {
        for (uint32_t index : m_groups.layouts) {
            if (hasCells(LayoutID(index)))
                functor(LayoutID(index), m_parts.layouts[index]);
        }
    }

    void findOffsetsOfFirstSlots();
    void layFirstLateNamesInCellsOnTrial();
    bool mayLieInCellAsNarrowReference(FieldReference) const;
    BitVector findLayoutsThatLateNamesMayMoveInto();
    void chooseForms();

    void prepareRows();
    BitVector findFamiliesThatMayAgreeOnSlots() const;
    void findMembersThatAgreeOnSlots(FamilyID, Vector<LayoutID, 16>&) const;
    void layFieldsSharedByFamilies();
    void layFieldsOfUnions();
    std::optional<FieldReference> findFieldAbove(const BitVector& isLaid, FieldReference, LayoutID base) const;
    const PlacedSlot* findBaseAppendedSlot(LayoutID, PropertyKeyID, const PlacedSlot&) const;
    void placeAppendedFieldsAsInBase(LayoutID, const BitVector& isLaid);
    void layFieldsOf(LayoutID, const BitVector& isLaid);
    void handOffsetsToSlotsOf(LayoutID);
    void fillCellThatStaysAsLaid(LayoutID);
    void layFieldOnTrialOutOfLineAgain(LayoutID, PlacedSlot&);
    Vector<FieldIndex, 8> chooseInitialNames(LayoutID, const PlannedLayout&, uint64_t endOfCell, NOESCAPE const Invocable<uint32_t(FieldIndex)> auto& placeInWrittenTypes) const;
    void fillRoomLeftInCell(LayoutID, PlannedLayout&);
    void finishCell(LayoutID);
    std::optional<uint32_t> findOffsetOfNameInLayoutsBelow(LayoutID, PropertyKeyID, SlotRepresentation) const;
    void moveLateNamesToWhereLayoutsBelowListThem(LayoutID);
    void layCells();

    Program& m_program;
    const Facts& m_facts;
    const LinkTables& m_link;
    BuildContext& m_build;
    SlotPlanParts& m_parts;
    const LayoutsBelowTheirBases& m_layoutsBelowTheirBases;
    CellSizesForFields m_cellSizes;
    GroupsOfLayoutsForFields m_groups;
    UnitsOfFields m_units;
    const std::span<const LayoutSetID> m_fewLayoutsWithoutFamily;

    BitVector m_groupsLaidAgain;

    Vector<uint8_t> m_offsetsOfFirstSlots;
    HashMap<uint32_t, PlacedSlot, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_slotsOutOfLineOfFieldsOnTrial;

    Vector<uint64_t> m_offsets;
    Vector<uint32_t> m_placesInCells;
    Vector<uint8_t> m_widths;
    Vector<uint64_t> m_ends;
    uint32_t m_numberOfPlacesInCells { 0 };
    BytesOfCellInFieldsStorage m_cell;
    BitVector m_slotsAsInBase;
    HashMap<uint32_t, Vector<LayoutID, 4>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_layoutsBelow;
};

ChooserOfFieldsStorage::ChooserOfFieldsStorage(Program& program, const Facts& facts, BuildContext& build, SlotPlanParts& parts, const LayoutsBelowTheirBases& layoutsBelowTheirBases, GroupsOfLayoutsForFields&& groups, UnitsOfFields&& units, std::span<const LayoutSetID> fewLayoutsWithoutFamily)
    : m_program(program)
    , m_facts(facts)
    , m_link(facts.linked())
    , m_build(build)
    , m_parts(parts)
    , m_layoutsBelowTheirBases(layoutsBelowTheirBases)
    , m_cellSizes(build)
    , m_groups(WTF::move(groups))
    , m_units(WTF::move(units))
    , m_fewLayoutsWithoutFamily(fewLayoutsWithoutFamily)
    , m_groupsLaidAgain(program.layouts.size())
{
}

void ChooserOfFieldsStorage::run()
{
    findOffsetsOfFirstSlots();
    layFirstLateNamesInCellsOnTrial();
    chooseForms();
    layCells();
}

void ChooserOfFieldsStorage::findOffsetsOfFirstSlots()
{
    BitVector groupsWithWords(numberOfLayouts());
    BitVector groupsWithExtensions(numberOfLayouts());
    forEachLayoutWithCells([&](LayoutID id, PlannedLayout& planned) {
        bool hasSlotOutOfLine = planned.slots.containsIf([](const PlacedSlot& slot) {
            return slot.isOutOfLine();
        });
        if (hasSlotOutOfLine)
            groupsWithExtensions.quickSet(m_groups.groupOf[id.index()]);
        if (hasSlotOutOfLine || planned.hasExtraPropertiesWord())
            groupsWithWords.quickSet(m_groups.groupOf[id.index()]);
    });
    forEachLayoutWithCells([&](LayoutID id, PlannedLayout& planned) {
        if (planned.environmentOfDeferredClosures || (groupsWithWords.quickGet(m_groups.groupOf[id.index()]) && !planned.hasExtraPropertiesWord()))
            layGroupAgain(id);
    });
    BitVector groupsLaidForGood = findGroupsLaidForGoodForFields(m_parts, m_groups);
    forEachLayoutWithCells([&](LayoutID id, PlannedLayout& planned) {
        uint32_t numberListed = static_cast<uint32_t>(m_program.at(id).fields.size());
        for (uint32_t place = 0; place < planned.appendedKeys.size() && !isLaidAgain(id.index()) && !groupsLaidForGood.quickGet(m_groups.groupOf[id.index()]); ++place) {
            const PlacedSlot& slot = planned.slots[numberListed + place];
            const PlacedSlot* slotOfBase = findBaseAppendedSlot(id, planned.appendedKeys[place], slot);
            if (slotOfBase && slotOfBase->offset != slot.offset)
                layGroupAgain(id);
        }
    });

    m_offsetsOfFirstSlots.fill(static_cast<uint8_t>(offsetOfFirstField), numberOfLayouts());
    for (uint32_t index : m_groups.layouts) {
        if (!groupsWithWords.quickGet(m_groups.groupOf[index]))
            continue;
        uint32_t widthOfWord = groupsWithExtensions.quickGet(m_groups.groupOf[index]) ? bytesPerSlotInFieldsStorage : widthOf(hiddenReferenceRepresentation(m_build.recordedOptions().useWideReferences()));
        m_offsetsOfFirstSlots[index] = static_cast<uint8_t>(offsetOfFirstField + widthOfWord);
    }
}

void ChooserOfFieldsStorage::layFirstLateNamesInCellsOnTrial()
{
    forEachLayoutWithCells([&](LayoutID id, PlannedLayout& planned) {
        for (FieldIndex field = static_cast<uint32_t>(m_program.at(id).fields.size()); field < planned.numberOfFields(); ++field) {
            PlacedSlot& slot = planned.slots[field];
            if (!slot.mayTakeWordLeftInCell)
                continue;
            m_slotsOutOfLineOfFieldsOnTrial.add(id.index(), slot);
            layAppendedFieldInCellForFields(m_program, m_facts, { id, field }, 0, slot);
            break;
        }
    });
}

bool ChooserOfFieldsStorage::mayLieInCellAsNarrowReference(FieldReference field) const
{
    LayoutKind kind = m_program.at(field.layout).kind;
    const PlannedLayout& planned = m_parts.layouts[field.layout.index()];
    const PlacedSlot& slot = planned.slots[field.field];
    if ((kind != LayoutKind::Record && kind != LayoutKind::Instance) || !slot.isOutOfLine() || slot.representation != SlotRepresentation::JSValue || !slot.type)
        return false;
    if (!isPlainSlotOfField(planned, field.field) || !isPlainFieldByRecords(m_program, m_facts, field) || narrowFormOfSlotHeldToTypeForFields(m_program, slot.type, false) != SlotRepresentation::OptionalCell32)
        return false;
    uint32_t unit = m_units.unitOfRow[m_units.rowOf(field)];
    return unit != UnitsOfFields::noUnit && std::ranges::none_of(m_units.rowsOf(m_units.units[unit]), [&](FieldReference other) {
        return other.layout != field.layout && hasCells(other.layout) && slotOf(other).hasLocationOf(slot);
    });
}

BitVector ChooserOfFieldsStorage::findLayoutsThatLateNamesMayMoveInto()
{
    BitVector result(numberOfLayouts());
    if (m_build.recordedOptions().useWideReferences())
        return result;
    forEachLayoutWithCells([&](LayoutID id, PlannedLayout& planned) {
        for (FieldIndex field = static_cast<uint32_t>(m_program.at(id).fields.size()); field < planned.numberOfFields(); ++field) {
            if (!planned.slots[field].liesInCellOnTrial() && !mayLieInCellAsNarrowReference({ id, field }))
                continue;
            result.quickSet(id.index());
            break;
        }
    });
    return result;
}

void ChooserOfFieldsStorage::chooseForms()
{
    BitVector layoutsWithSlotsOfOtherWidths(numberOfLayouts());
    chooseFormsOfSlots(m_program, m_facts, m_build, m_parts, m_groups, m_units, m_fewLayoutsWithoutFamily, m_offsetsOfFirstSlots.span(), findLayoutsThatLateNamesMayMoveInto(), layoutsWithSlotsOfOtherWidths);
    for (size_t index : layoutsWithSlotsOfOtherWidths)
        layGroupAgain(LayoutID(static_cast<uint32_t>(index)));
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
        const FreeBytesInFieldsStorage& free = freeBytes[id.index()];
        return free.end ? free : FreeBytesInFieldsStorage::behind(offsetOfFirstSlotOf(id));
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
    RowsOfSlotPlan rows { m_units.firstRowOfLayout.span(), m_offsets.mutableSpan(), m_placesInCells.mutableSpan(), m_ends.mutableSpan(), m_offsetsOfFirstSlots.span(), m_widths.span() };
    rows.haveFinalWidths = true;
    placeFieldsOfUnions(m_program, m_link, m_facts.families(), m_layoutsBelowTheirBases, rows, scopedLambda<bool(LayoutID)>(hasCellsLaidAgain), scopedLambda<uint32_t()>(addPlace));
}

std::optional<FieldReference> ChooserOfFieldsStorage::findFieldAbove(const BitVector& isLaid, FieldReference reference, LayoutID base) const
{
    const Field& listed = fieldAt(m_program, reference);
    FieldReference above;
    if (std::optional<uint32_t> listedAbove = m_program.at(base).findField(listed.key))
        above = { base, *listedAbove };
    else if (listed.flags.contains(FieldFlags::IsInherited))
        above = declarationOf(m_program, reference);
    else
        return std::nullopt;
    if (above.layout == reference.layout || !isLaid.quickGet(above.layout.index()) || !isObjectLayout(m_program.at(above.layout).kind))
        return std::nullopt;
    return above;
}

const PlacedSlot* ChooserOfFieldsStorage::findBaseAppendedSlot(LayoutID id, PropertyKeyID key, const PlacedSlot& slot) const
{
    LayoutID base = isObjectLayout(m_program.at(id).kind) ? writtenBaseOf(m_program.at(id)) : LayoutID();
    if (!base || !hasCells(base) || slot.storage != SlotStorage::Inline || slot.liesInCellOnTrial())
        return nullptr;
    const PlannedLayout& planned = m_parts.layouts[base.index()];
    size_t place = std::ranges::find(planned.appendedKeys, key) - planned.appendedKeys.begin();
    if (place == planned.appendedKeys.size())
        return nullptr;
    const PlacedSlot& slotOfBase = planned.slots[m_program.at(base).fields.size() + place];
    return slotOfBase.storage == SlotStorage::Inline && !slotOfBase.liesInCellOnTrial() && slotOfBase.representation == slot.representation ? &slotOfBase : nullptr;
}

void ChooserOfFieldsStorage::placeAppendedFieldsAsInBase(LayoutID id, const BitVector& isLaid)
{
    m_slotsAsInBase.clearAll();
    if (!hasCells(id))
        return;
    LayoutID base = writtenBaseOf(m_program.at(id));
    if (!base || !isLaid.quickGet(base.index()))
        return;
    PlannedLayout& planned = m_parts.layouts[id.index()];
    uint32_t numberListed = static_cast<uint32_t>(m_program.at(id).fields.size());
    for (uint32_t place = 0; place < planned.appendedKeys.size(); ++place) {
        PlacedSlot& slot = planned.slots[numberListed + place];
        const PlacedSlot* slotOfBase = findBaseAppendedSlot(id, planned.appendedKeys[place], slot);
        if (!slotOfBase || !m_cell.isFree(slotOfBase->offset, slot.width()))
            continue;
        m_cell.occupy(slotOfBase->offset, slot.width());
        slot.offset = slotOfBase->offset;
        m_slotsAsInBase.set(numberListed + place);
    }
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
    placeAppendedFieldsAsInBase(id, isLaid);
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
    if (!hasCells(id))
        return;
    handOffsetsToSlotsOf(id);
    finishCell(id);
}

void ChooserOfFieldsStorage::handOffsetsToSlotsOf(LayoutID id)
{
    PlannedLayout& planned = m_parts.layouts[id.index()];
    uint32_t numberListed = static_cast<uint32_t>(m_program.at(id).fields.size());
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        PlacedSlot& slot = planned.slots[field];
        if (slot.storage == SlotStorage::Inline && !slot.liesInCellOnTrial() && !m_slotsAsInBase.get(field))
            slot.offset = static_cast<uint32_t>(field < numberListed ? m_offsets[m_units.rowOf({ id, field })] : m_cell.take(slot.width()));
    }
}

void ChooserOfFieldsStorage::fillCellThatStaysAsLaid(LayoutID id)
{
    if (m_build.recordedOptions().useWideReferences())
        return;
    const PlannedLayout& planned = m_parts.layouts[id.index()];
    std::span<const PlacedSlot> slotsOfFields = planned.slots.span().first(planned.numberOfFields());
    bool hasLateName = std::ranges::any_of(slotsOfFields.last(planned.appendedKeys.size()), [](const PlacedSlot& slot) {
        return slot.isOutOfLine() || slot.liesInCellOnTrial();
    });
    if (!hasLateName)
        return;
    m_cell.reset(offsetOfFirstSlotOf(id));
    for (const PlacedSlot& slot : slotsOfFields) {
        if (slot.storage == SlotStorage::Inline && !slot.liesInCellOnTrial())
            m_cell.occupy(slot.offset, slot.width());
    }
    finishCell(id);
}

void ChooserOfFieldsStorage::layFieldOnTrialOutOfLineAgain(LayoutID id, PlacedSlot& slot)
{
    PlacedSlot asFound = m_slotsOutOfLineOfFieldsOnTrial.get(id.index());
    slot.representation = asFound.representation;
    slot.storage = asFound.storage;
    slot.mayExistWithoutStorage = asFound.mayExistWithoutStorage;
    slot.offset = asFound.offset;
    slot.type = asFound.type;
}

Vector<FieldIndex, 8> ChooserOfFieldsStorage::chooseInitialNames(LayoutID id, const PlannedLayout& planned, uint64_t endOfCell, NOESCAPE const Invocable<uint32_t(FieldIndex)> auto& placeInWrittenTypes) const
{
    Vector<FieldIndex, 8> chosen;
    std::span<const LinkedInitialNames> candidates = m_link.initialNamesOf(id);
    if (candidates.empty())
        return chosen;
    constexpr uint32_t width = widthOf(SlotRepresentation::OptionalCell32);
    size_t room = 0;
    for (uint64_t offset = offsetOfFirstSlotOf(id); offset + width <= endOfCell; offset += width)
        room += m_cell.isFree(offset, width);
    uint32_t numberListed = static_cast<uint32_t>(m_program.at(id).fields.size());
    Vector<std::pair<uint32_t, FieldIndex>, 8> lacking;
    Vector<std::pair<uint32_t, FieldIndex>, 8> fewest;
    while (room) {
        fewest.shrink(0);
        for (const LinkedInitialNames& names : candidates) {
            lacking.shrink(0);
            bool fits = true;
            for (PropertyKeyID key : names.keys) {
                size_t place = std::ranges::find(planned.appendedKeys, key) - planned.appendedKeys.begin();
                if (place == planned.appendedKeys.size()) {
                    fits = false;
                    break;
                }
                FieldIndex field = numberListed + static_cast<uint32_t>(place);
                const PlacedSlot& slot = planned.slots[field];
                if ((slot.storage == SlotStorage::Inline && !slot.liesInCellOnTrial()) || chosen.contains(field))
                    continue;
                fits &= slot.liesInCellOnTrial() ? slot.width() <= width || (slot.type && narrowFormOfSlotHeldToTypeForFields(m_program, slot.type, false) == SlotRepresentation::OptionalCell32) : mayLieInCellAsNarrowReference({ id, field });
                lacking.append({ placeInWrittenTypes(field), field });
            }
            if (!fits || lacking.isEmpty() || lacking.size() > room)
                continue;
            std::ranges::sort(lacking);
            if (fewest.isEmpty() || std::pair { lacking.size(), lacking[0] } < std::pair { fewest.size(), fewest[0] })
                fewest.swap(lacking);
        }
        if (fewest.isEmpty())
            break;
        for (auto [place, field] : fewest)
            chosen.append(field);
        room -= fewest.size();
    }
    return chosen;
}

void ChooserOfFieldsStorage::fillRoomLeftInCell(LayoutID id, PlannedLayout& planned)
{
    if (m_build.recordedOptions().useWideReferences())
        return;
    bool hasRoomForReferences = true;
    uint64_t endOfCell = m_cellSizes.cellSizeFor(m_cell.end());
    const Layout& layout = m_program.at(id);
    uint32_t numberListed = static_cast<uint32_t>(layout.fields.size());
    auto placeInWrittenTypes = [&](FieldIndex field) {
        uint32_t numberDeclaredInFront = 0;
        for (LayoutID written : layout.supertypes) {
            const Layout& type = m_program.at(written);
            if (std::optional<uint32_t> listed = type.findField(planned.appendedKeys[field - numberListed])) {
                auto place = std::ranges::find(type.declaredOrder, *listed);
                return numberDeclaredInFront + static_cast<uint32_t>(place - type.declaredOrder.begin());
            }
            numberDeclaredInFront += static_cast<uint32_t>(type.fields.size());
        }
        return UINT32_MAX;
    };
    Vector<FieldIndex, 8> initialNames = chooseInitialNames(id, planned, endOfCell, placeInWrittenTypes);
    Vector<std::tuple<size_t, bool, uint32_t, FieldIndex>, 32> lateNames;
    for (FieldIndex field = numberListed; field < planned.numberOfFields(); ++field)
        lateNames.append({ std::ranges::find(initialNames, field) - initialNames.begin(), !m_link.isStoredByFollowerOfMaker({ id, field }), placeInWrittenTypes(field), field });
    std::ranges::sort(lateNames);
    for (auto [rank, isStoredIntoObjectsThatArriveAlone, place, field] : lateNames) {
        PlacedSlot& slot = planned.slots[field];
        if (slot.liesInCellOnTrial()) {
            bool isWide = slot.width() > widthOf(SlotRepresentation::OptionalCell32);
            if (std::optional<uint64_t> offset = rank < initialNames.size() && isWide ? std::nullopt : m_cell.tryTakeBelow(slot.width(), endOfCell))
                slot.offset = static_cast<uint32_t>(*offset);
            else
                layFieldOnTrialOutOfLineAgain(id, slot);
            slot.mayTakeWordLeftInCell = false;
        }
        if (!hasRoomForReferences || !mayLieInCellAsNarrowReference({ id, field }))
            continue;
        std::optional<uint64_t> offset = m_cell.tryTakeBelow(widthOf(SlotRepresentation::OptionalCell32), endOfCell);
        if (!offset) {
            hasRoomForReferences = false;
            continue;
        }
        slot.storage = SlotStorage::Inline;
        slot.representation = SlotRepresentation::OptionalCell32;
        slot.offset = static_cast<uint32_t>(*offset);
        slot.mayExistWithoutStorage = false;
    }
}

void ChooserOfFieldsStorage::finishCell(LayoutID id)
{
    PlannedLayout& planned = m_parts.layouts[id.index()];
    fillRoomLeftInCell(id, planned);
    bool hasSlotOutOfLine = planned.slots.containsIf([](const PlacedSlot& slot) {
        return slot.isOutOfLine();
    });
    if (!hasSlotOutOfLine) {
        planned.extensionSize = 0;
        planned.granulesOfFirstExtension = 0;
    }

    PlacedSlot& extension = planned.slots[planned.extensionWordIndex()];
    PlacedSlot& extraProperties = planned.slots[planned.extraPropertiesWordIndex()];
    extension = PlacedSlot { };
    extraProperties = PlacedSlot { };
    RELEASE_ASSERT(!hasSlotOutOfLine || offsetOfFirstSlotOf(id) == offsetOfFirstField + bytesPerSlotInFieldsStorage);
    if (offsetOfFirstSlotOf(id) > offsetOfFirstField) {
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

        return;
    }
    planned.size = static_cast<uint32_t>(size);
    moveLateNamesToWhereLayoutsBelowListThem(id);
}

std::optional<uint32_t> ChooserOfFieldsStorage::findOffsetOfNameInLayoutsBelow(LayoutID id, PropertyKeyID key, SlotRepresentation representation) const
{
    auto below = m_layoutsBelow.find(id.index());
    if (below == m_layoutsBelow.end())
        return std::nullopt;
    std::optional<uint32_t> found;
    for (LayoutID heir : below->value) {
        std::optional<uint32_t> listed = hasCells(heir) ? m_program.at(heir).findField(key) : std::nullopt;
        if (!listed)
            continue;
        const PlacedSlot& slot = slotOf({ heir, *listed });
        uint32_t row = m_units.rowOf({ heir, *listed });
        bool isLaid = isLaidAgain(heir.index()) ? m_placesInCells[row] != noPlaceInCell && m_widths[row] : slot.storage == SlotStorage::Inline;
        uint32_t offset = isLaidAgain(heir.index()) ? static_cast<uint32_t>(m_offsets[row]) : slot.offset;
        if (!isLaid || slot.representation != representation || found.value_or(offset) != offset)
            return std::nullopt;
        found = offset;
    }
    return found;
}

void ChooserOfFieldsStorage::moveLateNamesToWhereLayoutsBelowListThem(LayoutID id)
{
    if (!m_layoutsBelow.contains(id.index()))
        return;
    PlannedLayout& planned = m_parts.layouts[id.index()];
    uint32_t numberListed = static_cast<uint32_t>(m_program.at(id).fields.size());
    BitVector liesWhereLayoutsBelowListIt(planned.numberOfFields());
    for (FieldIndex field = numberListed; field < planned.numberOfFields(); ++field) {
        PlacedSlot& slot = planned.slots[field];
        if (slot.storage != SlotStorage::Inline)
            continue;
        std::optional<uint32_t> offset = findOffsetOfNameInLayoutsBelow(id, planned.appendedKeys[field - numberListed], slot.representation);
        if (!offset || *offset + slot.width() > planned.size)
            continue;
        if (*offset != slot.offset && m_cell.isFree(*offset, slot.width())) {
            m_cell.occupy(*offset, slot.width());
            slot.offset = *offset;
        }
        for (FieldIndex other = numberListed; *offset != slot.offset && other < planned.numberOfFields(); ++other) {
            PlacedSlot& slotOfOther = planned.slots[other];
            if (slotOfOther.storage == SlotStorage::Inline && slotOfOther.offset == *offset && slotOfOther.width() == slot.width() && !liesWhereLayoutsBelowListIt.quickGet(other))
                std::swap(slotOfOther.offset, slot.offset);
        }
        if (*offset == slot.offset)
            liesWhereLayoutsBelowListIt.quickSet(field);
    }
}

void ChooserOfFieldsStorage::layCells()
{
    for (uint32_t index : m_groups.layouts) {
        if (LayoutID base = isObjectLayout(m_program.layouts[index].kind) ? writtenBaseOf(m_program.layouts[index]) : LayoutID())
            m_layoutsBelow.add(base.index(), Vector<LayoutID, 4> { }).iterator->value.append(LayoutID(index));
    }
    if (!m_groupsLaidAgain.isEmpty()) {
        prepareRows();
        layFieldsSharedByFamilies();
        layFieldsOfUnions();
    }

    BitVector isLaid(numberOfLayouts());
    BitVector isWaiting(numberOfLayouts());
    Vector<LayoutID, 16> waiting;
    for (uint32_t index : m_groups.layouts) {
        if (!isLaidAgain(index)) {
            if (hasCells(LayoutID(index)))
                fillCellThatStaysAsLaid(LayoutID(index));
            continue;
        }
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

void chooseStorageOfFields(Program& program, const Facts& facts, BuildContext& build, SlotPlanParts& parts, const LayoutsBelowTheirBases& layoutsBelowTheirBases, GroupsOfLayoutsForFields&& groups, UnitsOfFields&& units, std::span<const LayoutSetID> fewLayoutsWithoutFamily)
{
    ChooserOfFieldsStorage(program, facts, build, parts, layoutsBelowTheirBases, WTF::move(groups), WTF::move(units), fewLayoutsWithoutFamily).run();
}

static bool isNarrowFormForFieldsStorage(SlotRepresentation representation)
{
    switch (representation) {
    case SlotRepresentation::OptionalBoolean:
    case SlotRepresentation::Int32:
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
        return plan.isAllocatable(id) && plan.layout(id).holdsCompiledObjects() && !plan.layout(id).slots.isEmpty();
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
