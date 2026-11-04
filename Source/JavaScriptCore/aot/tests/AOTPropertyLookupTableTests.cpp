/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTPropertyLookupTableTestSupport.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include <array>
#include <bit>
#include <memory>
#include <wtf/HashSet.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC::AOT {

namespace {

using Kind = Slot::Kind;

constexpr uint32_t typeOffsetOfStoredField = 8;

void ownValueRoundTrip()
{
    constexpr uint32_t inlineColumn = 1;
    constexpr uint32_t outOfLineColumn = 2;
    constexpr uint32_t farColumn = 3;
    constexpr PropertyOffset inlineOffset = 2;
    constexpr PropertyOffset outOfLineOffset = firstOutOfLineOffset + 3;
    constexpr PropertyOffset farOffset = firstOutOfLineOffset + (1 << 16);
    TableFixture fixture;
    auto& front = fixture.front();
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();

    Slot inlineValue = ownValueSlot(structure, inlineOffset);
    for (SlotAccess access : allSlotAccesses) {
        AOT_TEST_CHECK(!front.find(id, inlineColumn, access));
        AOT_TEST_CHECK(front.trySet(structure, inlineColumn, access, inlineValue));
        AOT_TEST_CHECK(isSameSlot(front.find(id, inlineColumn, access), inlineValue));
        const PackedSlot& cell = fixture.cellAt(structure, inlineColumn, access);
        AOT_TEST_CHECK(cell.payload == static_cast<uint32_t>(Slot::wordIndexFor(inlineOffset)));
        AOT_TEST_CHECK(std::bit_cast<uint64_t>(cell) >> 32 == PackedSlot::upperHalfForOwnInlineValue(inlineColumn));
    }

    Slot outOfLineValue = ownValueSlot(structure, outOfLineOffset);
    AOT_TEST_CHECK(outOfLineValue.kind == Kind::Value && outOfLineValue.has(Slot::isOutOfLine) && outOfLineValue.offsetInStorage() == outOfLineOffset);
    Slot farValue = ownValueSlot(structure, farOffset);
    AOT_TEST_CHECK(farValue.kind == Kind::FarValue && farValue.offsetInStorage() == farOffset);
    for (SlotAccess access : allSlotAccesses) {
        AOT_TEST_CHECK(front.trySet(structure, outOfLineColumn, access, outOfLineValue));
        AOT_TEST_CHECK(front.trySet(structure, farColumn, access, farValue));
        AOT_TEST_CHECK(isSameSlot(front.find(id, outOfLineColumn, access), outOfLineValue));
        AOT_TEST_CHECK(isSameSlot(front.find(id, farColumn, access), farValue));
        AOT_TEST_CHECK(std::bit_cast<uint64_t>(fixture.cellAt(structure, outOfLineColumn, access)) >> 32 != PackedSlot::upperHalfForOwnInlineValue(outOfLineColumn));
    }
}

void rowsDoNotOverlap()
{
    constexpr unsigned numberOfStructures = 200;
    constexpr unsigned columnsPerStructure = 40;
    TableFixture fixture;
    auto& front = fixture.front();
    Vector<Structure*> structures;
    auto offsetFor = [](unsigned structure, unsigned column) {
        return static_cast<PropertyOffset>((structure + column) % 50);
    };
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        structures.append(fixture.createStructure());
        for (unsigned column = 1; column <= columnsPerStructure; ++column)
            AOT_TEST_CHECK(front.trySet(structures[i], column, SlotAccess::Read, ownValueSlot(structures[i], offsetFor(i, column))));
    }
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        for (unsigned column = 1; column <= columnsPerStructure; ++column) {
            AOT_TEST_CHECK(fixture.cellAt(structures[i], column, SlotAccess::Read).name == column);
            AOT_TEST_CHECK(isSameSlot(front.find(structures[i]->id(), column, SlotAccess::Read), ownValueSlot(structures[i], offsetFor(i, column))));
        }
        AOT_TEST_CHECK(!front.find(structures[i]->id(), columnsPerStructure + 1, SlotAccess::Read));
    }
}

void rowMovePreservesPlanesAndChainBits()
{
    TableFixture fixture;
    auto& front = fixture.front();
    Structure* low = fixture.createStructure();
    Structure* high = fixture.createStructure();
    uint32_t lowColumn = 1;
    uint32_t highColumn = 2;
    AOT_TEST_CHECK(front.trySet(low, lowColumn, SlotAccess::Read, absentSlot(low)));
    AOT_TEST_CHECK(front.trySet(high, highColumn, SlotAccess::Read, absentSlot(high)));
    if (rowOf(low) > rowOf(high)) {
        std::swap(low, high);
        std::swap(lowColumn, highColumn);
    }
    PropertyOffset offset = invalidOffset;
    Structure* target = fixture.addProperty(high, highColumn, offset);
    Slot storeSlot = transitionSlot(high, target, offset, Slot::dependsOnChain);
    Slot defineSlot = transitionSlot(high, target, offset);
    AOT_TEST_CHECK(front.trySet(high, highColumn, SlotAccess::Store, storeSlot));
    AOT_TEST_CHECK(front.trySet(high, highColumn, SlotAccess::Define, defineSlot));
    uint32_t rowBeforeMove = rowOf(high);
    uint32_t collidingColumn = rowBeforeMove - rowOf(low) + highColumn;
    AOT_TEST_CHECK(RowDisplacementTable::hasColumn(collidingColumn) && collidingColumn != lowColumn);

    AOT_TEST_CHECK(front.trySet(low, collidingColumn, SlotAccess::Read, ownValueSlot(low, 1)));

    AOT_TEST_CHECK(rowOf(high) && rowOf(high) != rowBeforeMove);
    AOT_TEST_CHECK(isSameSlot(front.find(high->id(), highColumn, SlotAccess::Read), absentSlot(high)));
    AOT_TEST_CHECK(isSameSlot(front.find(high->id(), highColumn, SlotAccess::Store), storeSlot));
    AOT_TEST_CHECK(isSameSlot(front.find(high->id(), highColumn, SlotAccess::Define), defineSlot));
    AOT_TEST_CHECK(isSameSlot(front.find(low->id(), lowColumn, SlotAccess::Read), absentSlot(low)));
    AOT_TEST_CHECK(isSameSlot(front.find(low->id(), collidingColumn, SlotAccess::Read), ownValueSlot(low, 1)));
    AOT_TEST_CHECK(!front.find(low->id(), collidingColumn, SlotAccess::Store) && !front.find(low->id(), collidingColumn, SlotAccess::Define));
    AOT_TEST_CHECK(!front.hasChainDependentCell(low->id(), collidingColumn));

    AOT_TEST_CHECK(front.hasChainDependentCell(high->id(), highColumn));
    AOT_TEST_CHECK(front.removeChainDependentCells(high->id(), highColumn) == 2);
    AOT_TEST_CHECK(!front.hasChainDependentCell(high->id(), highColumn));
    AOT_TEST_CHECK(!front.find(high->id(), highColumn, SlotAccess::Read) && !front.find(high->id(), highColumn, SlotAccess::Store));
    AOT_TEST_CHECK(isSameSlot(front.find(high->id(), highColumn, SlotAccess::Define), defineSlot));
    AOT_TEST_CHECK(front.hasChainDependentCell(low->id(), lowColumn));
}

void accessesAreIndependent()
{
    constexpr uint32_t column = 1;
    TableFixture fixture;
    auto& front = fixture.front();
    Structure* before = fixture.createStructure();
    StructureID id = before->id();
    PropertyOffset offset = invalidOffset;
    Structure* after = fixture.addProperty(before, column, offset);
    Slot storeSlot = transitionSlot(before, after, offset, Slot::dependsOnChain);
    Slot defineSlot = transitionSlot(before, after, offset);

    AOT_TEST_CHECK(front.trySet(before, column, SlotAccess::Read, absentSlot(before)));
    AOT_TEST_CHECK(!front.find(id, column, SlotAccess::Store) && !front.find(id, column, SlotAccess::Define));
    AOT_TEST_CHECK(front.trySet(before, column, SlotAccess::Store, storeSlot));
    AOT_TEST_CHECK(!front.find(id, column, SlotAccess::Define));
    AOT_TEST_CHECK(front.trySet(before, column, SlotAccess::Define, defineSlot));

    AOT_TEST_CHECK(isSameSlot(front.find(id, column, SlotAccess::Read), absentSlot(before)));
    std::optional<Slot> found = front.find(id, column, SlotAccess::Store);
    AOT_TEST_CHECK(isSameSlot(found, storeSlot));
    AOT_TEST_CHECK(found->kind == Kind::Transition && found->transition.newStructureID == after->id() && found->offsetInStorage() == offset);
    AOT_TEST_CHECK(isSameSlot(front.find(id, column, SlotAccess::Define), defineSlot));

    front.remove(id, column, SlotAccess::Store);
    AOT_TEST_CHECK(!front.find(id, column, SlotAccess::Store));
    AOT_TEST_CHECK(isSameSlot(front.find(id, column, SlotAccess::Read), absentSlot(before)));
    AOT_TEST_CHECK(isSameSlot(front.find(id, column, SlotAccess::Define), defineSlot));
}

void removeKeepsRowAndColumn()
{
    constexpr uint32_t column = 1;
    constexpr uint32_t unusedColumn = 2;
    TableFixture fixture;
    auto& front = fixture.front();
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    Slot slot = ownValueSlot(structure, 0);
    front.remove(id, column, SlotAccess::Read);
    AOT_TEST_CHECK(!front.hasRow(id));
    AOT_TEST_CHECK(front.trySet(structure, column, SlotAccess::Read, slot));
    AOT_TEST_CHECK(front.trySet(structure, column, SlotAccess::Store, slot));
    uint32_t row = rowOf(structure);
    AOT_TEST_CHECK(row && front.hasRow(id));
    AOT_TEST_CHECK(front.plane(SlotAccess::Read)[row].name == RowDisplacementTable::rowMarkerTag);

    for (unsigned i = 0; i < 3; ++i)
        front.remove(id, column, SlotAccess::Read);
    front.remove(id, unusedColumn, SlotAccess::Read);

    AOT_TEST_CHECK(!front.find(id, column, SlotAccess::Read));
    AOT_TEST_CHECK(front.hasRow(id) && rowOf(structure) == row);
    const PackedSlot& reservation = fixture.cellAt(structure, column, SlotAccess::Read);
    AOT_TEST_CHECK(reservation.name == column && reservation.kind() == Kind::Empty);
    AOT_TEST_CHECK(isSameSlot(front.find(id, column, SlotAccess::Store), slot));

    front.remove(id, column, SlotAccess::Store);
    AOT_TEST_CHECK(fixture.cellAt(structure, column, SlotAccess::Store).isEmpty());
    AOT_TEST_CHECK(front.trySet(structure, column, SlotAccess::Read, slot) && rowOf(structure) == row);

    for (unsigned i = 0; i < 2; ++i) {
        front.releaseRowOf(id);
        AOT_TEST_CHECK(!front.hasRow(id) && !rowOf(structure) && !front.find(id, column, SlotAccess::Read));
    }
    AOT_TEST_CHECK(front.plane(SlotAccess::Read)[row].isEmpty() && front.plane(SlotAccess::Read)[row + column].isEmpty());
}

void unpackableSlotMakesNoRow()
{
    constexpr uint32_t column = 1;
    constexpr PropertyOffset offsetPastPackedWordIndex = firstOutOfLineOffset + 200;
    constexpr uint16_t hiddenWordOffset = 8;
    TableFixture fixture;
    auto& front = fixture.front();
    Structure* structure = fixture.createStructure();
    Structure* other = fixture.createStructure();
    StructureID id = structure->id();
    JSObject* holder = fixture.createPrototype();

    Slot unpackableSlots[] = {
        Slot::createConstant(id, 0, jsNumber(1)),
        fieldSlot(structure, SlotRepresentation::JSValue, typeOffsetOfStoredField),
        transitionSlot(structure, other, offsetPastPackedWordIndex),
        Slot::createExtraProperty(id, Kind::ExtraProperty, Slot::isOwn, other->id(), 0, hiddenWordOffset),
        Slot::createExtraPropertyAddition(id, Slot::isOwn, other->id(), 0, hiddenWordOffset),
        inheritedValueSlot(structure, holder, offsetPastPackedWordIndex),
    };
    for (const Slot& slot : unpackableSlots) {
        for (SlotAccess access : allSlotAccesses)
            AOT_TEST_CHECK(!front.trySet(structure, column, access, slot));
        AOT_TEST_CHECK(!front.hasRow(id));
    }

    Slot extraProperty = Slot::createExtraProperty(id, Kind::ExtraProperty, Slot::isOwn, StructureID(), 0, hiddenWordOffset);
    AOT_TEST_CHECK(front.trySet(structure, column, SlotAccess::Read, extraProperty));
    AOT_TEST_CHECK(isSameSlot(front.find(id, column, SlotAccess::Read), extraProperty));
}

void fieldSlotsPackByTypeAndHiddenWord()
{
    constexpr uint16_t lastPackedHiddenWordOffset = UINT8_MAX << PackedSlot::hiddenWordOffsetShift;
    constexpr uint32_t column = 1;
    constexpr uint32_t refusedColumn = 2;
    TableFixture fixture;
    auto& front = fixture.front();
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    auto packs = [&](const Slot& slot) {
        if (!front.trySet(structure, column, SlotAccess::Read, slot))
            return false;
        AOT_TEST_CHECK(isSameSlot(front.find(id, column, SlotAccess::Read), slot));
        return true;
    };
    auto fieldInExtension = [&](uint16_t extensionWordOffset) {
        return Slot::createField(id, Kind::Field, Slot::isOwn | Slot::isOutOfLine, SlotData::create(24, SlotRepresentation::JSValue), 0, extensionWordOffset);
    };

    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        AOT_TEST_CHECK(packs(fieldSlot(structure, representation)));
        AOT_TEST_CHECK(!front.trySet(structure, refusedColumn, SlotAccess::Store, fieldSlot(structure, representation, typeOffsetOfStoredField)));
    }
    AOT_TEST_CHECK(packs(fieldSlot(structure, SlotRepresentation::JSValue, 0, Slot::isOwn | Slot::absentIsUndefined)));
    AOT_TEST_CHECK(packs(fieldSlot(structure, SlotRepresentation::JSValue, 0, Slot::isOwn | Slot::absentIsUndefined | Slot::dependsOnChain)));
    AOT_TEST_CHECK(packs(fieldInExtension(lastPackedHiddenWordOffset)));
    AOT_TEST_CHECK(packs(Slot::createField(id, Kind::FieldCreation, Slot::isOwn, SlotData::create(16, SlotRepresentation::OptionalCell32), 0, 0)));
    AOT_TEST_CHECK(packs(Slot::createField(id, Kind::Variable, Slot::isOwn, SlotData::create(SlotData::maxOffset, SlotRepresentation::JSValue), 0, 0)));

    AOT_TEST_CHECK(!front.trySet(structure, refusedColumn, SlotAccess::Read, fieldInExtension(lastPackedHiddenWordOffset + (1 << PackedSlot::hiddenWordOffsetShift))));
    AOT_TEST_CHECK(!front.trySet(structure, refusedColumn, SlotAccess::Read, fieldInExtension(2)));
    AOT_TEST_CHECK(!front.trySet(structure, refusedColumn, SlotAccess::Read, Slot::createField(id, Kind::FieldCreation, Slot::isOwn | Slot::absentIsUndefined, SlotData::create(16, SlotRepresentation::JSValue), 0, 0)));
    for (SlotAccess access : allSlotAccesses)
        AOT_TEST_CHECK(!front.find(id, refusedColumn, access));
}

void numberForRankIsOneToOne()
{
    for (uint32_t numberOfNames : { 0u, 1u, 2u, 6u, 2046u, 2047u, 2048u, 5000u, 40000u }) {
        Vector<bool> isTaken(FillWith { }, numberOfNames + 1, false);
        AOT_TEST_CHECK(RowDisplacementTable::numberForRank(0, numberOfNames) == RowDisplacementTable::rowMarkerColumn);
        for (uint32_t rank = 0; rank <= numberOfNames; ++rank) {
            uint32_t number = RowDisplacementTable::numberForRank(rank, numberOfNames);
            AOT_TEST_CHECK(number <= numberOfNames && !isTaken[number]);
            AOT_TEST_CHECK(rank < RowDisplacementTable::numberOfHotNames || number == rank);
            isTaken[number] = true;
        }
    }
}

void structureWithoutRowMisses()
{
    constexpr unsigned numberOfStructures = 8;
    constexpr uint32_t columnsPerStructure = 16;
    constexpr uint32_t numberOfProbedColumns = 256;
    TableFixture fixture;
    auto& front = fixture.front();
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        Structure* structure = fixture.createStructure();
        for (uint32_t column = 1; column <= columnsPerStructure; ++column) {
            for (SlotAccess access : allSlotAccesses)
                AOT_TEST_CHECK(front.trySet(structure, column, access, ownValueSlot(structure, 0)));
        }
    }

    Structure* structureWithoutRow = fixture.createStructure();
    AOT_TEST_CHECK(!rowOf(structureWithoutRow) && !front.hasRow(structureWithoutRow->id()));
    for (uint32_t column = 1; column <= numberOfProbedColumns; ++column) {
        for (SlotAccess access : allSlotAccesses) {
            AOT_TEST_CHECK(fixture.cellAt(structureWithoutRow, column, access).name != column);
            AOT_TEST_CHECK(!front.find(structureWithoutRow->id(), column, access));
        }
    }
}

void structuresHaveTheirOwnRowWords()
{
    constexpr unsigned numberOfStructures = 256;
    TableFixture fixture;
    auto& front = fixture.front();
    Vector<Structure*> structures;
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        structures.append(fixture.createStructure());
        AOT_TEST_CHECK(!rowOf(structures.last()));
    }
    for (Structure* structure : structures)
        AOT_TEST_CHECK(front.trySet(structure, 1, SlotAccess::Read, ownValueSlot(structure, 0)));

    HashSet<uint32_t> rows;
    for (Structure* structure : structures) {
        AOT_TEST_CHECK(rowOf(structure) && rows.add(rowOf(structure)).isNewEntry);
        AOT_TEST_CHECK(front.plane(SlotAccess::Read)[rowOf(structure)].name == RowDisplacementTable::rowMarkerTag);
    }
    size_t numberOfVisitedSlots = 0;
    front.forEachSlot([&](const Slot& slot, uint32_t column, SlotAccess access) {
        AOT_TEST_CHECK(column == 1 && access == SlotAccess::Read && isSameSlot(slot, ownValueSlot(slot.structureID.decode(), 0)));
        ++numberOfVisitedSlots;
    });
    AOT_TEST_CHECK(numberOfVisitedSlots == numberOfStructures);

    front.clear();
    for (Structure* structure : structures)
        AOT_TEST_CHECK(!rowOf(structure) && !front.find(structure->id(), 1, SlotAccess::Read));
}

void evictionKeepsFilling()
{
    constexpr uint32_t numberOfWideRows = RowDisplacementTable::maxNumberOfBases / RowDisplacementTable::numberOfColumns;
    constexpr unsigned numberOfNarrowRows = RowDisplacementTable::numberOfColumns;
    TableFixture fixture;
    auto& front = fixture.front();
    Vector<Structure*> structures;
    for (uint32_t i = 0; i < numberOfWideRows; ++i) {
        structures.append(fixture.createStructure());
        Slot slot = ownValueSlot(structures.last(), 0);
        for (uint32_t column = 1; column <= RowDisplacementTable::maxColumn; ++column)
            AOT_TEST_CHECK(front.trySet(structures.last(), column, SlotAccess::Read, slot));
    }
    for (Structure* structure : structures)
        AOT_TEST_CHECK(front.find(structure->id(), RowDisplacementTable::maxColumn, SlotAccess::Read));

    for (unsigned i = 0; i < numberOfNarrowRows; ++i) {
        structures.append(fixture.createStructure());
        Slot slot = ownValueSlot(structures.last(), 1);
        AOT_TEST_CHECK(front.trySet(structures.last(), 1, SlotAccess::Read, slot));
        AOT_TEST_CHECK(isSameSlot(front.find(structures.last()->id(), 1, SlotAccess::Read), slot));
    }

    unsigned numberOfEvictedRows = 0;
    for (Structure* structure : structures) {
        AOT_TEST_CHECK(front.hasRow(structure->id()) == !!rowOf(structure));
        AOT_TEST_CHECK(front.hasRow(structure->id()) == !!front.find(structure->id(), 1, SlotAccess::Read));
        numberOfEvictedRows += !front.hasRow(structure->id());
    }
    AOT_TEST_CHECK(numberOfEvictedRows && !front.hasRow(structures.first()->id()));
}

Vector<StructureID> createRows(TableFixture& fixture)
{
    constexpr unsigned numberOfStructures = 32;
    Vector<StructureID> ids;
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        Structure* structure = fixture.createStructure();
        AOT_TEST_CHECK(fixture.front().trySet(structure, 1 + i % 4, SlotAccess::Read, ownValueSlot(structure, 0)));
        AOT_TEST_CHECK(rowOf(structure));
        ids.append(structure->id());
    }
    return ids;
}

void vmTeardownReleasesRows()
{
    auto haveNoRows = [](const Vector<StructureID>& ids) {
        for (StructureID id : ids) {
            if (StructureRows::singleton().rowOf(id))
                return false;
        }
        return true;
    };
    {
        TableFixture fixture;
        Vector<StructureID> ids = createRows(fixture);
        fixture.vm().aotState().willShutDownHeap();
        AOT_TEST_CHECK(haveNoRows(ids));
    }
    Vector<StructureID> idsOfDestroyedVM;
    {
        TableFixture fixture;
        idsOfDestroyedVM = createRows(fixture);
    }
    AOT_TEST_CHECK(haveNoRows(idsOfDestroyedVM));
}

void nameWithoutColumnGoesToHashedTable()
{
    TableFixture fixture;
    auto& caches = fixture.caches();
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    Slot slot = ownValueSlot(structure, 0);

    for (uint32_t number : { RowDisplacementTable::rowMarkerColumn, RowDisplacementTable::maxColumn + 1 }) {
        AOT_TEST_CHECK(!RowDisplacementTable::hasColumn(number));
        caches.set(structure, fixture.name(number), SlotAccess::Read, slot);
        AOT_TEST_CHECK(isSameSlot(fixture.hashedTable().find(id, fixture.uid(number), SlotAccess::Read), slot));
        AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(number), SlotAccess::Read), slot));
    }
    caches.set(structure, fixture.nameWithoutNumber(1), SlotAccess::Read, slot);
    AOT_TEST_CHECK(isSameSlot(fixture.hashedTable().find(id, fixture.uid(1), SlotAccess::Read), slot));
    AOT_TEST_CHECK(!fixture.front().hasRow(id));

    AOT_TEST_CHECK(RowDisplacementTable::hasColumn(RowDisplacementTable::maxColumn));
    caches.set(structure, fixture.name(RowDisplacementTable::maxColumn), SlotAccess::Read, slot);
    AOT_TEST_CHECK(isSameSlot(fixture.front().find(id, RowDisplacementTable::maxColumn, SlotAccess::Read), slot));
    AOT_TEST_CHECK(!fixture.hashedTable().find(id, fixture.uid(RowDisplacementTable::maxColumn), SlotAccess::Read));
}

void hashedAccessesCoexist()
{
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    Structure* structure = fixture.createStructure();
    Structure* other = fixture.createStructure();
    StructureID id = structure->id();
    UniquedStringImpl* uid = fixture.uid(1);
    PropertyOffset offset = invalidOffset;
    Structure* target = fixture.addProperty(structure, 1, offset);
    std::array<Slot, numberOfSlotAccesses> slots { absentSlot(structure), transitionSlot(structure, target, offset, Slot::dependsOnChain), transitionSlot(structure, target, offset) };

    for (SlotAccess access : allSlotAccesses) {
        AOT_TEST_CHECK(!table.find(id, uid, access));
        AOT_TEST_CHECK(table.set(uid, access, slots[static_cast<unsigned>(access)]) == HashedSlotTable::SetResult::Stored);
    }
    for (SlotAccess access : allSlotAccesses) {
        AOT_TEST_CHECK(isSameSlot(table.find(id, uid, access), slots[static_cast<unsigned>(access)]));
        AOT_TEST_CHECK(!table.find(other->id(), uid, access) && !table.find(id, fixture.uid(2), access));
    }

    AOT_TEST_CHECK(table.set(uid, SlotAccess::Read, ownValueSlot(structure, 3)) == HashedSlotTable::SetResult::Stored);
    AOT_TEST_CHECK(isSameSlot(table.find(id, uid, SlotAccess::Read), ownValueSlot(structure, 3)));
    AOT_TEST_CHECK(fixture.numberOfSlots() == numberOfSlotAccesses);

    table.remove(id, uid, SlotAccess::Store);
    AOT_TEST_CHECK(!table.find(id, uid, SlotAccess::Store));
    AOT_TEST_CHECK(table.find(id, uid, SlotAccess::Read) && table.find(id, uid, SlotAccess::Define));
    table.clear();
    AOT_TEST_CHECK(!fixture.numberOfSlots());
}

uint32_t setOf(HashedSlotTable& table, Structure* structure, UniquedStringImpl* uid)
{
    constexpr uint32_t wayBits = (HashedSlot::ways - 1) << HashedSlot::sizeShift;
    return HashedSlot::offsetOfFirstWay(structure->id(), uid, table.offsetMask()) & ~wayBits;
}

void fifthKeyOfSetEvicts()
{
    constexpr unsigned maxNumberOfCandidates = 1 << 16;
    static_assert(HashedSlot::ways == numberOfSlotAccesses + 1);
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    UniquedStringImpl* uid = fixture.uid(1);
    Structure* first = fixture.createStructure();
    Structure* second = nullptr;
    for (unsigned i = 0; i < maxNumberOfCandidates && !second; ++i) {
        Structure* candidate = fixture.createStructure();
        if (setOf(table, candidate, uid) == setOf(table, first, uid))
            second = candidate;
    }
    AOT_TEST_CHECK(second);

    for (SlotAccess access : allSlotAccesses)
        table.set(uid, access, ownValueSlot(first, 0));
    table.set(uid, SlotAccess::Read, ownValueSlot(second, 0));
    AOT_TEST_CHECK(fixture.numberOfSlots() == HashedSlot::ways);

    AOT_TEST_CHECK(table.set(uid, SlotAccess::Store, ownValueSlot(second, 1)) == HashedSlotTable::SetResult::Stored);

    AOT_TEST_CHECK(fixture.numberOfSlots() == HashedSlot::ways);
    AOT_TEST_CHECK(isSameSlot(table.find(second->id(), uid, SlotAccess::Store), ownValueSlot(second, 1)));
}

// The caches clear Instance::keyedValuePlaces(), which lie in front of the instance.
static constexpr size_t bytesInFrontOfZeroedInstance = static_cast<size_t>(-InstanceLayout::offsetOfKeyedValuePlaces);
static_assert(InstanceLayout::offsetOfKeyedValuePlaces < 0 && !(bytesInFrontOfZeroedInstance % alignof(Instance)));

struct ZeroedInstanceDeleter {
    void operator()(Instance* instance) const
    {
        instance->~Instance();
        fastFree(reinterpret_cast<uint8_t*>(instance) - bytesInFrontOfZeroedInstance);
    }
};
using ZeroedInstance = std::unique_ptr<Instance, ZeroedInstanceDeleter>;

ZeroedInstance createZeroedInstance()
{
    constexpr size_t size = bytesInFrontOfZeroedInstance + sizeof(Instance);
    auto* memory = static_cast<uint8_t*>(fastAlignedMalloc(alignof(Instance), size));
    ZeroedInstance instance { new (NotNull, memory + bytesInFrontOfZeroedInstance) Instance };
    zeroSpan(unsafeMakeSpan(memory, size));
    return instance;
}

bool hasPublishedWords(TableFixture& fixture, const Instance& instance)
{
    const PropertyCacheWords& words = instance.propertyCaches;
    for (SlotAccess access : allSlotAccesses) {
        const FrontWords& front = words.front[static_cast<unsigned>(access)];
        if (front.plane != fixture.front().plane(access) || front.rowsForStructureIDZero != StructureRows::singleton().rowsForStructureIDZero())
            return false;
    }
    return words.hashedSlots == fixture.hashedTable().entries() && words.hashedSlotOffsetMask == fixture.hashedTable().offsetMask()
        && words.customAccessors == fixture.caches().customAccessors().records();
}

void growthKeepsEntriesAndPublishesWords()
{
    constexpr uint32_t initialNumberOfEntries = HashedSlotTable::initialNumberOfEntries;
    constexpr unsigned maxNumberOfFills = 16 * initialNumberOfEntries;
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    ZeroedInstance instance = createZeroedInstance();
    fixture.vm().aotState().instances.append(instance.get());
    fixture.caches().didCreateInstance(*instance);
    AOT_TEST_CHECK(table.numberOfEntries() == initialNumberOfEntries && hasPublishedWords(fixture, *instance));

    Structure* lastStructure = nullptr;
    for (unsigned i = 0; table.numberOfEntries() == initialNumberOfEntries; ++i) {
        AOT_TEST_CHECK(i < maxNumberOfFills);
        lastStructure = fixture.createStructure();
        fixture.caches().set(lastStructure, fixture.nameWithoutNumber(1), SlotAccess::Read, absentSlot(lastStructure, 0));
    }

    AOT_TEST_CHECK(table.numberOfEntries() == 2 * initialNumberOfEntries);
    AOT_TEST_CHECK(table.offsetMask() == HashedSlot::offsetMaskFor(2 * initialNumberOfEntries));
    AOT_TEST_CHECK(hasPublishedWords(fixture, *instance));
    AOT_TEST_CHECK(isSameSlot(table.find(lastStructure->id(), fixture.uid(1), SlotAccess::Read), absentSlot(lastStructure, 0)));
    size_t numberOfKeptEntries = 0;
    table.forEachEntry([&](const HashedSlot& entry) {
        AOT_TEST_CHECK(table.find(entry.slot.structureID, entry.uid(), entry.access()) == &entry.slot);
        ++numberOfKeptEntries;
    });
    AOT_TEST_CHECK(numberOfKeptEntries > initialNumberOfEntries / 2);
    fixture.vm().aotState().instances.clear();
}

void frontGrowthKeepsRowsAndPublishesWords()
{
    constexpr unsigned numberOfStructures = 3 * RowDisplacementTable::initialNumberOfBases;
    constexpr uint32_t columns[] = { 1, 977, RowDisplacementTable::maxColumn };
    TableFixture fixture;
    auto& front = fixture.front();
    ZeroedInstance instance = createZeroedInstance();
    fixture.vm().aotState().instances.append(instance.get());
    fixture.caches().didCreateInstance(*instance);
    AOT_TEST_CHECK(hasPublishedWords(fixture, *instance));

    auto offsetFor = [](unsigned structure, uint32_t column) {
        return static_cast<PropertyOffset>((structure + column) % 50);
    };
    Vector<Structure*> structures;
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        structures.append(fixture.createStructure());
        for (uint32_t column : columns)
            fixture.caches().set(structures[i], fixture.name(column), SlotAccess::Read, ownValueSlot(structures[i], offsetFor(i, column)));
        fixture.caches().set(structures[i], fixture.name(columns[0]), SlotAccess::Define, absentSlot(structures[i], 0));
        AOT_TEST_CHECK(hasPublishedWords(fixture, *instance));
    }
    for (unsigned i = 0; i < numberOfStructures; ++i) {
        for (uint32_t column : columns) {
            AOT_TEST_CHECK(fixture.cellAt(structures[i], column, SlotAccess::Read).name == column);
            AOT_TEST_CHECK(isSameSlot(front.find(structures[i]->id(), column, SlotAccess::Read), ownValueSlot(structures[i], offsetFor(i, column))));
            AOT_TEST_CHECK(!front.find(structures[i]->id(), column, SlotAccess::Store));
        }
        AOT_TEST_CHECK(isSameSlot(front.find(structures[i]->id(), columns[0], SlotAccess::Define), absentSlot(structures[i], 0)));
    }
    fixture.vm().aotState().instances.clear();
}

void hashedEntryHoldsItsName()
{
    TableFixture fixture;
    auto& table = fixture.hashedTable();
    Structure* structure = fixture.createStructure();
    UniquedStringImpl* uid = fixture.uid(1);
    unsigned initialRefCount = uid->refCount();

    table.set(uid, SlotAccess::Read, absentSlot(structure));
    AOT_TEST_CHECK(uid->refCount() == initialRefCount + 1);
    table.set(uid, SlotAccess::Read, ownValueSlot(structure, 0));
    AOT_TEST_CHECK(uid->refCount() == initialRefCount + 1);
    table.set(uid, SlotAccess::Store, ownValueSlot(structure, 0));
    AOT_TEST_CHECK(uid->refCount() == initialRefCount + 2);
    table.remove(structure->id(), uid, SlotAccess::Read);
    table.remove(structure->id(), uid, SlotAccess::Read);
    AOT_TEST_CHECK(uid->refCount() == initialRefCount + 1);
    table.clear();
    AOT_TEST_CHECK(uid->refCount() == initialRefCount);
}

void fillByNumberReplacesHashedCopy()
{
    TableFixture fixture;
    auto& caches = fixture.caches();
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    Slot packable = ownValueSlot(structure, 0);
    Slot unpackable = fieldSlot(structure, SlotRepresentation::JSValue, typeOffsetOfStoredField);

    caches.set(structure, fixture.nameWithoutNumber(1), SlotAccess::Store, packable);
    AOT_TEST_CHECK(!fixture.front().hasRow(id) && fixture.hashedTable().find(id, fixture.uid(1), SlotAccess::Store));
    AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(1), SlotAccess::Store), packable));

    caches.set(structure, fixture.name(1), SlotAccess::Store, packable);
    AOT_TEST_CHECK(isSameSlot(fixture.front().find(id, 1, SlotAccess::Store), packable));
    AOT_TEST_CHECK(!fixture.hashedTable().find(id, fixture.uid(1), SlotAccess::Store));

    caches.set(structure, fixture.name(1), SlotAccess::Store, unpackable);
    AOT_TEST_CHECK(!fixture.front().find(id, 1, SlotAccess::Store));
    AOT_TEST_CHECK(isSameSlot(fixture.hashedTable().find(id, fixture.uid(1), SlotAccess::Store), unpackable));
    AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(1), SlotAccess::Store), unpackable));
    AOT_TEST_CHECK(!caches.find(id, fixture.name(1), SlotAccess::Read) && !caches.find(id, fixture.name(1), SlotAccess::Define));
}

void removeClearsBothLevels()
{
    TableFixture fixture;
    auto& caches = fixture.caches();
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    Slot slot = ownValueSlot(structure, 0);
    caches.set(structure, fixture.name(1), SlotAccess::Read, slot);
    fixture.hashedTable().set(fixture.uid(1), SlotAccess::Read, slot);
    caches.set(structure, fixture.name(1), SlotAccess::Store, slot);
    caches.set(structure, fixture.name(2), SlotAccess::Read, slot);

    caches.remove(id, fixture.name(1), SlotAccess::Read);

    AOT_TEST_CHECK(!fixture.front().find(id, 1, SlotAccess::Read) && !fixture.hashedTable().find(id, fixture.uid(1), SlotAccess::Read));
    AOT_TEST_CHECK(!caches.find(id, fixture.name(1), SlotAccess::Read));
    AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(1), SlotAccess::Store), slot));
    AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(2), SlotAccess::Read), slot));
}

void dropRemovesOnlyDependentsOfTheName()
{
    constexpr uint32_t numberWithoutColumn = RowDisplacementTable::maxColumn + 1;
    TableFixture fixture;
    VM& vm = fixture.vm();
    auto& caches = fixture.caches();
    Structure* structure = fixture.createStructure();
    Structure* other = fixture.createStructure();
    StructureID id = structure->id();
    for (uint32_t droppedNumber : { 1u, numberWithoutColumn }) {
        uint32_t keptNumber = droppedNumber + 1;
        uint32_t independentNumber = droppedNumber + 2;
        caches.set(structure, fixture.name(droppedNumber), SlotAccess::Read, absentSlot(structure));
        caches.set(structure, fixture.name(keptNumber), SlotAccess::Read, absentSlot(structure));
        caches.set(structure, fixture.name(independentNumber), SlotAccess::Read, absentSlot(structure, 0));
        caches.set(other, fixture.name(droppedNumber), SlotAccess::Read, absentSlot(other));
        AOT_TEST_CHECK(caches.hasChainDependent(vm, id.bits(), fixture.uid(droppedNumber)) && caches.hasChainDependent(vm, id.bits(), fixture.uid(keptNumber)));
        AOT_TEST_CHECK(!caches.hasChainDependent(vm, id.bits(), fixture.uid(independentNumber)));

        caches.dropChainDependent(vm, id.bits(), fixture.uid(droppedNumber));
        caches.dropChainDependent(vm, id.bits(), fixture.uid(independentNumber));

        AOT_TEST_CHECK(!caches.find(id, fixture.name(droppedNumber), SlotAccess::Read));
        AOT_TEST_CHECK(!caches.hasChainDependent(vm, id.bits(), fixture.uid(droppedNumber)));
        AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(keptNumber), SlotAccess::Read), absentSlot(structure)));
        AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(independentNumber), SlotAccess::Read), absentSlot(structure, 0)));
        AOT_TEST_CHECK(isSameSlot(caches.find(other->id(), fixture.name(droppedNumber), SlotAccess::Read), absentSlot(other)));

        caches.dropChainDependent(vm, id.bits(), fixture.uid(droppedNumber));
        AOT_TEST_CHECK(caches.hasChainDependent(vm, other->id().bits(), fixture.uid(droppedNumber)));
    }
    Structure* structureWithoutEntries = fixture.createStructure();
    caches.dropChainDependent(vm, structureWithoutEntries->id().bits(), fixture.uid(1));
    AOT_TEST_CHECK(!caches.hasChainDependent(vm, structureWithoutEntries->id().bits(), fixture.uid(1)));
}

void dropKeepsDefinitionThatAdds()
{
    TableFixture fixture;
    auto& caches = fixture.caches();
    Structure* before = fixture.createStructure();
    StructureID id = before->id();
    PropertyOffset offset = invalidOffset;
    Structure* after = fixture.addProperty(before, 1, offset);
    Slot defineSlot = transitionSlot(before, after, offset);
    caches.set(before, fixture.name(1), SlotAccess::Store, transitionSlot(before, after, offset, Slot::dependsOnChain));
    caches.set(before, fixture.name(1), SlotAccess::Define, defineSlot);

    caches.dropChainDependent(fixture.vm(), id.bits(), fixture.uid(1));

    AOT_TEST_CHECK(!caches.find(id, fixture.name(1), SlotAccess::Store));
    AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(1), SlotAccess::Define), defineSlot));
}

void hostSetter() { }

void dropRemovesInheritedSetterAndKeepsField()
{
    TableFixture fixture;
    auto& caches = fixture.caches();
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    JSObject* holder = fixture.createPrototype();
    std::optional<uint32_t> record = caches.customAccessors().add({ reinterpret_cast<void*>(hostSetter), holder, fixture.globalObject(), 0, 0 });
    AOT_TEST_CHECK(record && caches.customAccessors().isInUse(*record));
    Slot setterSlot = Slot::createCustomAccessor(id, Kind::CustomSetter, Slot::dependsOnChain, *record);
    Slot readSlot = fieldSlot(structure, SlotRepresentation::JSValue);
    caches.set(structure, fixture.name(1), SlotAccess::Store, setterSlot);
    caches.set(structure, fixture.name(1), SlotAccess::Read, readSlot);
    AOT_TEST_CHECK(isSameSlot(fixture.front().find(id, 1, SlotAccess::Store), setterSlot));

    caches.dropChainDependent(fixture.vm(), id.bits(), fixture.uid(1));

    AOT_TEST_CHECK(!caches.find(id, fixture.name(1), SlotAccess::Store));
    AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(1), SlotAccess::Read), readSlot));
}

void storeSlotOfFieldStays()
{
    TableFixture fixture;
    auto& caches = fixture.caches();
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    Slot storeSlot = fieldSlot(structure, SlotRepresentation::JSValue, typeOffsetOfStoredField);
    Slot readSlot = fieldSlot(structure, SlotRepresentation::JSValue, 0, Slot::isOwn | Slot::absentIsUndefined | Slot::dependsOnChain);
    caches.set(structure, fixture.name(1), SlotAccess::Store, storeSlot);
    caches.set(structure, fixture.name(1), SlotAccess::Read, readSlot);
    AOT_TEST_CHECK(isSameSlot(fixture.hashedTable().find(id, fixture.uid(1), SlotAccess::Store), storeSlot));

    for (unsigned event = 0; event < 2; ++event) {
        caches.dropChainDependent(fixture.vm(), id.bits(), fixture.uid(1));
        AOT_TEST_CHECK(isSameSlot(caches.find(id, fixture.name(1), SlotAccess::Store), storeSlot));
        AOT_TEST_CHECK(!caches.find(id, fixture.name(1), SlotAccess::Read));
    }
}

void secondInstanceClearsOnce()
{
    TableFixture fixture;
    auto& caches = fixture.caches();
    auto& instances = fixture.vm().aotState().instances;
    Structure* structure = fixture.createStructure();
    StructureID id = structure->id();
    Slot slot = ownValueSlot(structure, 0);
    std::array<ZeroedInstance, 3> storage { createZeroedInstance(), createZeroedInstance(), createZeroedInstance() };
    auto fill = [&] {
        caches.set(structure, fixture.name(1), SlotAccess::Read, slot);
        caches.set(structure, fixture.nameWithoutNumber(2), SlotAccess::Read, slot);
    };
    auto isFilled = [&] {
        return !!caches.find(id, fixture.name(1), SlotAccess::Read) && !!caches.find(id, fixture.nameWithoutNumber(2), SlotAccess::Read);
    };
    auto createInstance = [&](unsigned index) {
        instances.append(storage[index].get());
        caches.didCreateInstance(*storage[index]);
        AOT_TEST_CHECK(hasPublishedWords(fixture, *storage[index]));
    };

    createInstance(0);
    fill();
    AOT_TEST_CHECK(!caches.isSharedByInstances() && isFilled());

    createInstance(1);
    AOT_TEST_CHECK(caches.isSharedByInstances());
    AOT_TEST_CHECK(!fixture.numberOfSlots() && !fixture.front().hasRow(id) && !rowOf(structure));

    fill();
    createInstance(2);
    AOT_TEST_CHECK(caches.isSharedByInstances() && isFilled());
    instances.clear();
}

// As the stub GetByValOfKeyOfPair files an absence beside a pair and asks whether it still holds.
void absenceBesidePairDoesNotOutliveItsCount()
{
    using KeyOfPair = StringKeyCache::KeyOfPair;
    constexpr uint32_t mask = KeyOfPair::maskBelowOtherRead;
    TableFixture fixture;
    ZeroedInstance instance = createZeroedInstance();
    fixture.vm().aotState().instances.append(instance.get());
    fixture.caches().didCreateInstance(*instance);
    std::array<KeyOfPair, StringKeyCache::waysOfKeysOfPairs> pairs { };
    instance->realm.keysOfPairs = pairs.data();
    instance->realm.keyOfPairMask = StringKeyCache::keyOfPairMaskFor(pairs.size());
    PropertyCacheWords& words = instance->propertyCaches;
    auto fileAbsence = [&] {
        pairs[1].structureIDOfPlace = 64;
        pairs[1].slotOfPlace = KeyOfPair::markOf(KeyOfPair::OtherRead::Absence) | (words.timesAbsencesWereForgotten & mask);
        words.pairsHavePlaces = 1;
    };
    auto absenceHolds = [&] {
        return pairs[1].structureIDOfPlace && !((pairs[1].slotOfPlace ^ words.timesAbsencesWereForgotten) & mask);
    };
    auto forgetAbsences = [&] {
        words.placesSayAbsence = 1;
        fixture.caches().didChangeObjectOnSomeChain(nullptr, nullptr);
    };

    for (uint32_t countWhenFiled : { 0u, 5u, mask, mask + 1 }) {
        words.timesAbsencesWereForgotten = countWhenFiled;
        fileAbsence();
        AOT_TEST_CHECK(absenceHolds());
        forgetAbsences();
        AOT_TEST_CHECK(words.timesAbsencesWereForgotten == countWhenFiled + 1 && !absenceHolds());

        // The count goes on, one at a time, until its low bits are those that the pair has kept. On its way they are zero once.
        uint32_t countWhenLowBitsAreZero = (countWhenFiled + 1 + mask) & ~mask;
        if (words.timesAbsencesWereForgotten != countWhenLowBitsAreZero) {
            words.timesAbsencesWereForgotten = countWhenLowBitsAreZero - 1;
            forgetAbsences();
        }
        if (words.timesAbsencesWereForgotten != countWhenFiled + mask + 1) {
            words.timesAbsencesWereForgotten = countWhenFiled + mask;
            forgetAbsences();
        }
        AOT_TEST_CHECK(words.timesAbsencesWereForgotten == countWhenFiled + mask + 1);
        AOT_TEST_CHECK(!absenceHolds());
    }
    fixture.vm().aotState().instances.clear();
}

} // namespace

std::span<const NativeTest> propertyLookupTableTests()
{
    static constexpr NativeTest tests[] = {
        { "property-lookup-table/own-value-round-trip"_s, ownValueRoundTrip },
        { "property-lookup-table/rows-do-not-overlap"_s, rowsDoNotOverlap },
        { "property-lookup-table/row-move-preserves-planes-and-chain-bits"_s, rowMovePreservesPlanesAndChainBits },
        { "property-lookup-table/accesses-are-independent"_s, accessesAreIndependent },
        { "property-lookup-table/remove-keeps-row-and-column"_s, removeKeepsRowAndColumn },
        { "property-lookup-table/unpackable-slot-makes-no-row"_s, unpackableSlotMakesNoRow },
        { "property-lookup-table/field-slots-pack-by-type-and-hidden-word"_s, fieldSlotsPackByTypeAndHiddenWord },
        { "property-lookup-table/number-for-rank-is-one-to-one"_s, numberForRankIsOneToOne },
        { "property-lookup-table/structure-without-row-misses"_s, structureWithoutRowMisses },
        { "property-lookup-table/structures-have-their-own-row-words"_s, structuresHaveTheirOwnRowWords },
        { "property-lookup-table/eviction-keeps-filling"_s, evictionKeepsFilling },
        { "property-lookup-table/vm-teardown-releases-rows"_s, vmTeardownReleasesRows },
        { "property-lookup-table/name-without-column-goes-to-hashed-table"_s, nameWithoutColumnGoesToHashedTable },
        { "property-lookup-table/hashed-accesses-coexist"_s, hashedAccessesCoexist },
        { "property-lookup-table/fifth-key-of-set-evicts"_s, fifthKeyOfSetEvicts },
        { "property-lookup-table/growth-keeps-entries-and-publishes-words"_s, growthKeepsEntriesAndPublishesWords },
        { "property-lookup-table/front-growth-keeps-rows-and-publishes-words"_s, frontGrowthKeepsRowsAndPublishesWords },
        { "property-lookup-table/hashed-entry-holds-its-name"_s, hashedEntryHoldsItsName },
        { "property-lookup-table/fill-by-number-replaces-hashed-copy"_s, fillByNumberReplacesHashedCopy },
        { "property-lookup-table/remove-clears-both-levels"_s, removeClearsBothLevels },
        { "property-lookup-table/drop-removes-only-dependents-of-the-name"_s, dropRemovesOnlyDependentsOfTheName },
        { "property-lookup-table/drop-keeps-definition-that-adds"_s, dropKeepsDefinitionThatAdds },
        { "property-lookup-table/drop-removes-inherited-setter-and-keeps-field"_s, dropRemovesInheritedSetterAndKeepsField },
        { "property-lookup-table/store-slot-of-field-stays"_s, storeSlotOfFieldStays },
        { "property-lookup-table/second-instance-clears-once"_s, secondInstanceClearsOnce },
        { "property-lookup-table/absence-beside-pair-does-not-outlive-its-count"_s, absenceBesidePairDoesNotOutliveItsCount },
    };
    return tests;
}

} // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
