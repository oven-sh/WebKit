/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFieldsStorage.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t bytesPerSlotInSlotForms = widthOf(SlotRepresentation::JSValue);
static constexpr TypeID typeOfIntegerSlotInSlotForms = typeOfPrimitives(Primitive::Integer | Primitive::UndefinedOrNull);
static constexpr TypeID typeOfNumberSlotInSlotForms = typeOfPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
static constexpr Range integersOfNarrowSlotInSlotForms { minInlineInt32, INT32_MAX };

#if USE(BIGINT32)
static constexpr uint16_t primitivesThatAreCellsInSlotForms = cellPrimitives & ~Primitive::BigInt;
#else
static constexpr uint16_t primitivesThatAreCellsInSlotForms = cellPrimitives;
#endif

static_assert(canLeaveSlot(SlotRepresentation::OptionalBoolean) && canLeaveSlot(SlotRepresentation::OptionalInt32) && canLeaveSlot(SlotRepresentation::Cell32) && canLeaveSlot(SlotRepresentation::OptionalCell32));
static_assert(canLeaveSlot(SlotRepresentation::OptionalInt64) && canLeaveSlot(SlotRepresentation::OptionalDouble));

namespace {

enum class ContentsInSlotForms : uint8_t {
    Booleans = 1 << 0,
    NarrowIntegers = 1 << 1,
    Integers = 1 << 2,
    Fractions = 1 << 3,
    Cells = 1 << 4,
    UndefinedOrNull = 1 << 5,
    AnyValue = 1 << 6,
};

} // anonymous namespace

enum class ClaimInSlotForms : bool { IsProven, IsHeld };

static OptionSet<ContentsInSlotForms> contentsOfTypeForSlotForms(const Program& program, TypeID type, ClaimInSlotForms claim)
{
    using Contents = ContentsInSlotForms;
    if (isNeverType(type))
        return { };
    Type proven = program.at(type);
    if (!proven.isJavaScriptValue() || proven.primitives & Primitive::Unspecified)
        return Contents::AnyValue;
    if (singleFunctionOf(TypeReader(program), proven))
        return Contents::AnyValue;
    OptionSet<Contents> contents;
    uint16_t primitives = proven.primitives & ~Primitive::Absent;
    auto takeOut = [&](uint16_t some, Contents brought) {
        if (primitives & some)
            contents.add(brought);
        primitives &= ~some;
    };
    if (primitives & Primitive::Integer)
        contents.add(proven.integers.isSubsetOf(claim == ClaimInSlotForms::IsHeld ? Range::int32() : integersOfNarrowSlotInSlotForms) ? Contents::NarrowIntegers : Contents::Integers);
    primitives &= ~Primitive::Integer;
    takeOut(Primitive::Number, Contents::Fractions);
    takeOut(Primitive::Boolean, Contents::Booleans);
    takeOut(Primitive::UndefinedOrNull, Contents::UndefinedOrNull);
    takeOut(primitivesThatAreCellsInSlotForms, Contents::Cells);
    if (proven.hasObjects())
        contents.add(Contents::Cells);
    if (primitives)
        contents.add(Contents::AnyValue);
    return contents;
}

static SlotRepresentation representationOfContentsForSlotForms(OptionSet<ContentsInSlotForms> contents, bool mayBeNarrow, bool usesWideReferences, bool isHeld, bool isReadForArithmetic)
{
    using Contents = ContentsInSlotForms;
    OptionSet<Contents> values = contents - Contents::UndefinedOrNull;
    if (values.isEmpty() || values.contains(Contents::AnyValue))
        return SlotRepresentation::JSValue;
    if (values == Contents::Booleans && mayBeNarrow)
        return SlotRepresentation::OptionalBoolean;
    if (values == Contents::NarrowIntegers && (mayBeNarrow || isHeld))
        return SlotRepresentation::OptionalInt32;
    constexpr OptionSet<Contents> integers { Contents::NarrowIntegers, Contents::Integers };
    if ((values - integers).isEmpty() && !isHeld)
        return SlotRepresentation::OptionalInt64;
    if ((values - integers - Contents::Fractions).isEmpty())
        return isHeld && !isReadForArithmetic ? SlotRepresentation::JSValue : SlotRepresentation::OptionalDouble;
    if (values == Contents::Cells && mayBeNarrow && !usesWideReferences)
        return contents.contains(Contents::UndefinedOrNull) ? SlotRepresentation::OptionalCell32 : SlotRepresentation::Cell32;
    return SlotRepresentation::JSValue;
}

static TypeID widestTypeOfRepresentationForSlotForms(TypeArena& types, SlotRepresentation representation)
{
    switch (representation) {
    case SlotRepresentation::OptionalBoolean:
        return typeOfPrimitives(Primitive::Boolean | Primitive::UndefinedOrNull);
    case SlotRepresentation::OptionalInt32:
        return types.withIntegers(typeOfIntegerSlotInSlotForms, integersOfNarrowSlotInSlotForms);
    case SlotRepresentation::OptionalInt64:
        return typeOfIntegerSlotInSlotForms;
    case SlotRepresentation::OptionalDouble:
        return typeOfNumberSlotInSlotForms;
    case SlotRepresentation::Cell32:
        return types.withPrimitives(TypeTable::object, primitivesThatAreCellsInSlotForms & ~primitivesWithLists);
    case SlotRepresentation::OptionalCell32:
        return types.withPrimitives(TypeTable::object, (primitivesThatAreCellsInSlotForms & ~primitivesWithLists) | Primitive::UndefinedOrNull);
    default:
        return { };
    }
}

static bool formMayBeReplacedInSlotForms(const PlacedSlot& slot)
{
    switch (slot.representation) {
    case SlotRepresentation::JSValue:
        return !slot.type;
    case SlotRepresentation::OptionalInt64:
    case SlotRepresentation::OptionalDouble:
        return true;
    default:
        return false;
    }
}

static PropertyKeyID keyOfRowInSlotForms(const Program& program, const SlotPlanParts& parts, FieldReference row)
{
    const Layout& layout = program.at(row.layout);
    if (row.field < layout.fields.size())
        return layout.fields[row.field].key;
    const PlannedLayout& planned = parts.layouts[row.layout.index()];
    size_t firstAppended = planned.numberOfFields() - planned.appendedKeys.size();
    return row.field >= firstAppended && row.field - firstAppended < planned.appendedKeys.size() ? planned.appendedKeys[row.field - firstAppended] : PropertyKeyID();
}

void chooseFormsOfSlots(Program& program, const Facts& facts, BuildContext& build, SlotPlanParts& parts, const GroupsOfLayoutsForFields& groups, const UnitsOfFields& units)
{
    if (!hasTypedSlotsForFields(facts))
        return;
    const LinkTables& linked = facts.linked();
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    bool usesWideReferences = build.recordedOptions().useWideReferences();
    uint32_t numberOfUnits = static_cast<uint32_t>(units.units.size());

    BitVector layoutsAboveCells(numberOfLayouts);
    for (uint32_t index : groups.layouts) {
        if (!hasCellsForFields(parts, LayoutID(index)))
            continue;
        for (LayoutID above = writtenBaseOf(program.layouts[index]); above && !layoutsAboveCells.quickSet(above.index()); above = writtenBaseOf(program.at(above))) { }
    }

    BitVector groupsLaidForGood = findGroupsLaidForGoodForFields(parts, groups);
    Vector<OptionSet<ContentsInSlotForms>> contentsOfUnits(numberOfUnits);
    BitVector unitsWhoseFormMayChange(numberOfUnits);
    BitVector unitsWithSlotHeldToType(numberOfUnits);
    for (uint32_t unit = 0; unit < numberOfUnits; ++unit) {
        std::optional<SlotRepresentation> found;
        bool everyFieldIsClaimed = !groupsLaidForGood.quickGet(units.units[unit].group);
        for (FieldReference row : units.rowsOf(units.units[unit])) {
            bool hasCells = hasCellsForFields(parts, row.layout);
            if (!hasCells && !layoutsAboveCells.quickGet(row.layout.index()))
                continue;
            if (hasCells && parts.layouts[row.layout.index()].slots[row.field].isOutOfLine())
                continue;
            const LinkedCell* cell = linked.findCellOfField(row);
            bool isProven = cell && cell->everyWriterIsKnown && cell->proven;
            TypeID held;
            if (hasCells) {
                const PlannedLayout& planned = parts.layouts[row.layout.index()];
                const PlacedSlot& slot = planned.slots[row.field];
                LayoutKind kind = program.at(row.layout).kind;
                isProven &= row.layout.index() < linked.layouts.size() && !linked.layout(row.layout).reachesUnseenCode && formMayBeReplacedInSlotForms(slot);
                if (!isProven && slot.representation == SlotRepresentation::JSValue)
                    held = slot.type;
                everyFieldIsClaimed &= kind == LayoutKind::Record || kind == LayoutKind::Instance;
                everyFieldIsClaimed &= slot.storage == SlotStorage::Inline && found.value_or(slot.representation) == slot.representation && isPlainSlotOfField(planned, row.field);
                found = slot.representation;
            } else if (!isProven)
                held = facts.field(row).slotType;
            everyFieldIsClaimed &= (isProven || held) && isPlainFieldByRecords(program, facts, row);
            if (!everyFieldIsClaimed)
                break;
            contentsOfUnits[unit].add(isProven ? contentsOfTypeForSlotForms(program, cell->proven, ClaimInSlotForms::IsProven) : contentsOfTypeForSlotForms(program, held, ClaimInSlotForms::IsHeld));
            if (held)
                unitsWithSlotHeldToType.quickSet(unit);
        }
        if (found && everyFieldIsClaimed)
            unitsWhoseFormMayChange.quickSet(unit);
    }
    if (unitsWhoseFormMayChange.isEmpty())
        return;

    CellSizesForFields sizes(build);
    BitVector layoutsWithSmallerCells(numberOfLayouts);
    for (uint32_t index : groups.layouts) {
        LayoutID id(index);
        if (!hasCellsForFields(parts, id))
            continue;
        const PlannedLayout& planned = parts.layouts[index];
        uint64_t bytesAsFound = 0;
        uint64_t bytesAtNarrowest = 0;
        bool hasHiddenWords = planned.hasExtraPropertiesWord();
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            const PlacedSlot& slot = planned.slots[field];
            hasHiddenWords |= slot.isOutOfLine();
            if (slot.storage != SlotStorage::Inline)
                continue;
            uint32_t unit = units.unitOfRow[units.rowOf({ id, field })];
            bool mayChange = unit != UnitsOfFields::noUnit && unitsWhoseFormMayChange.quickGet(unit);
            bytesAsFound += slot.width();
            bytesAtNarrowest += mayChange ? widthOf(representationOfContentsForSlotForms(contentsOfUnits[unit], true, usesWideReferences, unitsWithSlotHeldToType.quickGet(unit), false)) : slot.width();
        }
        uint64_t offsetOfFirstSlot = offsetOfFirstField + (hasHiddenWords ? bytesPerSlotInSlotForms : 0);
        if (sizes.cellSizeFor(offsetOfFirstSlot + WTF::roundUpToMultipleOf<bytesPerSlotInSlotForms>(bytesAtNarrowest)) < sizes.cellSizeFor(offsetOfFirstSlot + WTF::roundUpToMultipleOf<bytesPerSlotInSlotForms>(bytesAsFound)))
            layoutsWithSmallerCells.quickSet(index);
    }

    TypeArena types(program.types, ArenaKeeps::InProgram);
    for (size_t unit : unitsWhoseFormMayChange) {
        std::span<const FieldReference> rows = units.rowsOf(units.units[unit]);
        bool mayBeNarrow = std::ranges::any_of(rows, [&](FieldReference row) {
            return layoutsWithSmallerCells.quickGet(row.layout.index());
        });
        bool isReadForArithmetic = unitsWithSlotHeldToType.quickGet(unit) && std::ranges::any_of(rows, [&](FieldReference row) {
            PropertyKeyID key = keyOfRowInSlotForms(program, parts, row);
            return key && linked.fieldsReadForArithmetic.names(program, row.layout, key);
        });
        SlotRepresentation representation = representationOfContentsForSlotForms(contentsOfUnits[unit], mayBeNarrow, usesWideReferences, unitsWithSlotHeldToType.quickGet(unit), isReadForArithmetic);
        if (representation == SlotRepresentation::JSValue)
            continue;
        bool holdsReferences = representation == SlotRepresentation::Cell32 || representation == SlotRepresentation::OptionalCell32;
        if (holdsReferences && unitsWithSlotHeldToType.quickGet(unit)) {
            std::optional<TypeID> heldOfAll;
            bool allAreHeldAlike = std::ranges::all_of(rows, [&](FieldReference row) {
                if (!hasCellsForFields(parts, row.layout) || parts.layouts[row.layout.index()].slots[row.field].isOutOfLine())
                    return true;
                TypeID held = parts.layouts[row.layout.index()].slots[row.field].type;
                bool isAlike = held && heldOfAll.value_or(held) == held;
                heldOfAll = held;
                return isAlike;
            });
            if (!allAreHeldAlike)
                continue;
        }
        TypeID type = widestTypeOfRepresentationForSlotForms(types, representation);
        for (FieldReference row : rows) {
            if (!hasCellsForFields(parts, row.layout) || parts.layouts[row.layout.index()].slots[row.field].isOutOfLine())
                continue;
            PlacedSlot& slot = parts.layouts[row.layout.index()].slots[row.field];
            if (!holdsReferences || slot.representation != SlotRepresentation::JSValue || !slot.type)
                slot.type = type;
            slot.representation = representation;
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
