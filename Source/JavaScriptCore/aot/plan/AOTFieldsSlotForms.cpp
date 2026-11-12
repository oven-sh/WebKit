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
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/HashSet.h>
#include <wtf/MathExtras.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

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
    bool isCount = proven.integers.minimum >= 0 && proven.integers.maximum == INT32_MAX;
    if (primitives & Primitive::Integer)
        contents.add(proven.integers.isSubsetOf(claim == ClaimInSlotForms::IsHeld ? Range::int32() : integersOfNarrowSlotInSlotForms) && (claim == ClaimInSlotForms::IsHeld || !isCount) ? Contents::NarrowIntegers : Contents::Integers);
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

SlotRepresentation narrowFormOfSlotHeldToTypeForFields(const Program& program, TypeID held, bool usesWideReferences)
{
    return representationOfContentsForSlotForms(contentsOfTypeForSlotForms(program, held, ClaimInSlotForms::IsHeld), true, usesWideReferences, true, false);
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

static constexpr size_t mostLayoutsOfSetForSlotForms = 4;

static void forEachFewLayoutsWithoutFamilyForSlotForms(const Program& program, const LinkTables& linked, const Families& families, NOESCAPE const Invocable<void(LayoutSetID)> auto& functor)
{
    size_t numberOfTypes = program.types.numberOfTypes();
    BitVector typesAsked(2 * numberOfTypes);
    HashSet<uint32_t> setsAsked;
    auto ask = [&](TypeID type, bool isWritten) {
        if (!type || holdsOnlyPrimitives(type) || !liesInProgram(type) || positionOf(type) >= numberOfTypes || typesAsked.quickSet(2 * positionOf(type) + isWritten))
            return;
        Type read = program.at(type);
        LayoutSetID layouts = isWritten ? read.unnamedLayouts : read.hasOnlyLayouts() ? read.layouts : LayoutSetID();
        if (!layouts || holdsOneMember(layouts) || !liesInProgram(layouts) || program.at(layouts).size() > mostLayoutsOfSetForSlotForms)
            return;
        if (setsAsked.add(layouts.bits()).isNewEntry && !families.familyOf(layouts))
            functor(layouts);
    };
    for (uint32_t index = 0; index < program.functions.size() && index < linked.functions.size(); ++index) {
        if (!linked.functions[index].isReached)
            continue;
        const Function& function = program.functions[index];
        ask(function.thisType.written(), true);
        for (const DeclaredType& parameter : function.parameters)
            ask(parameter.written(), true);
        for (const DeclaredType& result : function.results)
            ask(result.written(), true);
        ask(linked.functions[index].result, false);
        for (const LinkedParameter& parameter : linked.parametersOf(FunctionID(index))) {
            ask(parameter.proven, false);
            ask(parameter.writtenOrSeenToArrive(), false);
        }
    }
    for (size_t index = 0; index < linked.variables.size(); ++index)
        ask(linked.variables[index].proven, false);
    for (uint32_t index = 0; index < program.layouts.size() && index < linked.layouts.size(); ++index) {
        if (!linked.layouts[index].isAllocated)
            continue;
        const auto& fields = program.layouts[index].fields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            ask(fields[field].declared.written(), true);
            if (const LinkedCell* cell = linked.findCellOfField({ LayoutID(index), field }))
                ask(cell->proven, false);
        }
    }
}

void chooseFormsOfSlots(Program& program, const Facts& facts, BuildContext& build, SlotPlanParts& parts, const GroupsOfLayoutsForFields& groups, const UnitsOfFields& units, std::span<const uint8_t> offsetsOfFirstSlots, const BitVector& layoutsThatLateNamesMayMoveInto, BitVector& layoutsWithSlotsOfOtherWidths)
{
    static_assert(!(indicesInRangeOfLoop % 64));
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
    forEachRangeOfIndicesInParallel(build.parallelLoops(), numberOfUnits, [&](size_t firstUnit, size_t endOfUnits) {
        for (size_t unit = firstUnit; unit < endOfUnits; ++unit) {
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
    });
    if (unitsWhoseFormMayChange.isEmpty())
        return;

    CellSizesForFields sizes(build);
    BitVector layoutsThatGainFromNarrowForms = layoutsThatLateNamesMayMoveInto;
    forEachRangeOfIndicesInParallel(build.parallelLoops(), numberOfLayouts, [&](size_t firstLayout, size_t endOfLayouts) {
        for (size_t index = firstLayout; index < endOfLayouts; ++index) {
            LayoutID id(static_cast<uint32_t>(index));
            if (groups.groupOf[index] == GroupsOfLayoutsForFields::noGroup || !hasCellsForFields(parts, id))
                continue;
            const PlannedLayout& planned = parts.layouts[index];
            uint64_t endAsFound = offsetsOfFirstSlots[index];
            uint64_t endAtNarrowest = offsetsOfFirstSlots[index];
            for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
                const PlacedSlot& slot = planned.slots[field];
                if (slot.storage != SlotStorage::Inline || slot.liesInCellOnTrial())
                    continue;
                uint32_t unit = planned.environmentOfDeferredClosures == field ? UnitsOfFields::noUnit : units.unitOfRow[units.rowOf({ id, field })];
                bool mayChange = unit != UnitsOfFields::noUnit && unitsWhoseFormMayChange.quickGet(unit);
                endAsFound += slot.width();
                endAtNarrowest += mayChange ? widthOf(representationOfContentsForSlotForms(contentsOfUnits[unit], true, usesWideReferences, unitsWithSlotHeldToType.quickGet(unit), false)) : slot.width();
            }
            if (sizes.cellSizeFor(endAtNarrowest) < sizes.cellSizeFor(endAsFound))
                layoutsThatGainFromNarrowForms.quickSet(index);
        }
    });

    auto isInheritedRow = [&](FieldReference row) {
        const Layout& layout = program.at(row.layout);
        return row.field < layout.fields.size() && layout.fields[row.field].flags.contains(FieldFlags::IsInherited);
    };
    auto isNarrowByItself = [&](size_t unit) {
        return std::ranges::any_of(units.rowsOf(units.units[unit]), [&](FieldReference row) {
            return !isInheritedRow(row) && layoutsThatGainFromNarrowForms.quickGet(row.layout.index());
        });
    };
    auto formOf = [&](size_t unit, bool mayBeNarrow) {
        bool isHeld = unitsWithSlotHeldToType.quickGet(unit);
        SlotRepresentation representation = representationOfContentsForSlotForms(contentsOfUnits[unit], mayBeNarrow, usesWideReferences, isHeld, false);
        if (!isHeld)
            return representation;
        SlotRepresentation representationForArithmetic = representationOfContentsForSlotForms(contentsOfUnits[unit], mayBeNarrow, usesWideReferences, isHeld, true);
        bool isReadForArithmetic = representationForArithmetic != representation && std::ranges::any_of(units.rowsOf(units.units[unit]), [&](FieldReference row) {
            PropertyKeyID key = keyOfRowInSlotForms(program, parts, row);
            return key && linked.fieldsReadForArithmetic.names(program, row.layout, key);
        });
        return isReadForArithmetic ? representationForArithmetic : representation;
    };

    Vector<uint32_t> unitsAbove(numberOfUnits, [](size_t unit) {
        return static_cast<uint32_t>(unit);
    });
    auto lowestUnitedWith = [&](uint32_t unit) {
        while (unitsAbove[unit] != unit)
            unit = unitsAbove[unit] = unitsAbove[unitsAbove[unit]];
        return unit;
    };
    BitVector unitsUnitedWithOthers(numberOfUnits);
    forEachFewLayoutsWithoutFamilyForSlotForms(program, linked, facts.families(), [&](LayoutSetID layouts) {
        Vector<LayoutID, mostLayoutsOfSetForSlotForms> members;
        program.at(layouts).forEach([&](LayoutID member) {
            if (hasCellsForFields(parts, member))
                members.append(member);
        });
        if (members.size() < 2)
            return;
        Vector<uint32_t, mostLayoutsOfSetForSlotForms> unitsUnderName;
        for (const Field& field : program.at(members[0]).fields) {
            unitsUnderName.shrink(0);
            for (LayoutID member : members) {
                std::optional<uint32_t> index = field.key ? program.at(member).findField(field.key) : std::nullopt;
                if (!index || parts.layouts[member.index()].slots[*index].storage != SlotStorage::Inline)
                    break;
                uint32_t unit = units.unitOfRow[units.rowOf({ member, *index })];
                if (unit == UnitsOfFields::noUnit || !unitsWhoseFormMayChange.quickGet(unit))
                    break;
                unitsUnderName.append(unit);
            }
            if (unitsUnderName.size() != members.size())
                continue;
            uint32_t lowest = UINT32_MAX;
            for (uint32_t unit : unitsUnderName) {
                unitsUnitedWithOthers.quickSet(unit);
                lowest = std::min(lowest, lowestUnitedWith(unit));
            }
            for (uint32_t unit : unitsUnderName)
                unitsAbove[lowestUnitedWith(unit)] = lowest;
        }
    });
    BitVector unitsNarrowWithOthers(numberOfUnits);
    if (!unitsUnitedWithOthers.isEmpty()) {
        Vector<std::optional<SlotRepresentation>> narrowForms(numberOfUnits);
        BitVector haveSeveralNarrowForms(numberOfUnits);
        BitVector holdUnitThatIsNarrow(numberOfUnits);
        for (size_t unit : unitsUnitedWithOthers) {
            uint32_t lowest = lowestUnitedWith(static_cast<uint32_t>(unit));
            SlotRepresentation narrow = formOf(unit, true);
            if (narrowForms[lowest].value_or(narrow) != narrow)
                haveSeveralNarrowForms.quickSet(lowest);
            narrowForms[lowest] = narrow;
            if (isNarrowByItself(unit))
                holdUnitThatIsNarrow.quickSet(lowest);
        }
        for (size_t unit : unitsUnitedWithOthers) {
            uint32_t lowest = lowestUnitedWith(static_cast<uint32_t>(unit));
            if (holdUnitThatIsNarrow.quickGet(lowest) && !haveSeveralNarrowForms.quickGet(lowest))
                unitsNarrowWithOthers.quickSet(unit);
        }
    }

    TypeArena types(program.types, ArenaKeeps::InProgram);
    Vector<std::pair<SlotRepresentation, TypeID>, 8> widestTypesMade;
    auto widestTypeOf = [&](SlotRepresentation representation) {
        for (auto [made, type] : widestTypesMade) {
            if (made == representation)
                return type;
        }
        TypeID type = widestTypeOfRepresentationForSlotForms(types, representation);
        widestTypesMade.append({ representation, type });
        return type;
    };
    for (size_t unit : unitsWhoseFormMayChange) {
        std::span<const FieldReference> rows = units.rowsOf(units.units[unit]);
        SlotRepresentation representation = formOf(unit, unitsNarrowWithOthers.quickGet(unit) || isNarrowByItself(unit));
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
        TypeID type = widestTypeOf(representation);
        for (FieldReference row : rows) {
            if (!hasCellsForFields(parts, row.layout) || parts.layouts[row.layout.index()].slots[row.field].isOutOfLine())
                continue;
            PlacedSlot& slot = parts.layouts[row.layout.index()].slots[row.field];
            if (!holdsReferences || slot.representation != SlotRepresentation::JSValue || !slot.type)
                slot.type = type;
            if (slot.storage == SlotStorage::Inline && !slot.liesInCellOnTrial() && slot.width() != widthOf(representation))
                layoutsWithSlotsOfOtherWidths.quickSet(row.layout.index());
            slot.representation = representation;
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
