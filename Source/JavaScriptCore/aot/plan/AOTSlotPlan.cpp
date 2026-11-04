/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Mechanism::TypedSlots: a field of an object of which Link has proven that it holds numbers alone, with every writer known, lies unboxed in its slot, with the markers for absent, undefined and null.
// And an object of which Link has proven that it gets no property beside its fields has no word for such properties.
// Whatever Link has proven of a field, its slot is HELD to the type that the program writes of it in checked code: facts/AOTEnforcedFields.h. Every store is held to it, so a filled slot holds a value of it. What is only guessed is never held.
// At a suspension and at a throw: nothing. The form of a slot is decided once, in front of Compile, and holds for every cell for its whole life.
// Withheld: every slot holds a JSValue and takes any value.

#include "config.h"
#include "AOTSlotPlan.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTEnvironmentsHandedBack.h"
#include "AOTEnforcedFields.h"
#include "AOTFacts.h"
#include "AOTFamilies.h"
#include "AOTFieldsStorage.h"
#include "AOTIRProgram.h"
#include "AOTKeyOrdersOfLayouts.h"
#include "AOTLayoutData.h"
#include "AOTLinkTables.h"
#include "AOTLinkValuesLaidAbove.h"
#include "AOTMechanisms.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTTaggedLayouts.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTUnionPlan.h"
#include "AOTUnionTaggedMakers.h"
#include "AOTUnitedFamilies.h"
#include "JSCell.h"
#include "MarkedSpace.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t bytesPerSlotInSlotPlan = widthOf(SlotRepresentation::JSValue);
static constexpr TypeID typeOfIntegerSlotInSlotPlan = typeOfPrimitives(Primitive::Integer | Primitive::UndefinedOrNull);
static constexpr TypeID typeOfNumberSlotInSlotPlan = typeOfPrimitives(Primitive::Number | Primitive::UndefinedOrNull);

static_assert(maxFieldsInOrder <= std::numeric_limits<uint16_t>::max());
static_assert(offsetOfFirstEnvironmentVariable == sizeof(JSCell));
static_assert(!(offsetOfFirstField % bytesPerSlotInSlotPlan) && !(offsetOfFirstEnvironmentVariable % bytesPerSlotInSlotPlan));
static_assert(widthOf(SlotRepresentation::OptionalInt64) == bytesPerSlotInSlotPlan && widthOf(SlotRepresentation::OptionalDouble) == bytesPerSlotInSlotPlan);
static_assert(canLeaveSlot(SlotRepresentation::OptionalInt64) && canLeaveSlot(SlotRepresentation::OptionalDouble));
static_assert(widthOf(hiddenReferenceRepresentation(true)) <= bytesPerSlotInSlotPlan && widthOf(hiddenReferenceRepresentation(false)) <= bytesPerSlotInSlotPlan);
static_assert(defaultLimitValue(LimitID::MaxFieldOffset, hostArchitecture()) <= SlotData::maxOffset + 1);
static_assert(representsAbsentAsZero(SlotRepresentation::JSValue) && traitsOf(SlotRepresentation::JSValue).undefinedBits);

// ---- What is asked of the plan

std::optional<FieldIndex> SlotPlan::findField(const Program& program, LayoutID id, PropertyKeyID key) const
{
    if (!id || !key)
        return std::nullopt;
    const Layout& declaring = program.at(id);
    if (std::optional<uint32_t> declared = declaring.findField(key)) {
        const PlannedLayout& planned = layout(id);
        bool isSpelledByTypeAlone = planned.storage == LayoutStorage::CompiledObject && *declared < planned.numberOfFields() && !planned.slots[*declared].indexInLayoutRecord;
        return isSpelledByTypeAlone ? std::nullopt : declared;
    }
    size_t place = layout(id).findAppendedKey(key);
    if (place == notFound)
        return std::nullopt;
    return static_cast<FieldIndex>(declaring.fields.size() + place);
}

PropertyKeyID SlotPlan::keyOf(const Program& program, FieldReference field) const
{
    if (!field.layout)
        return { };
    const Layout& declaring = program.at(field.layout);
    if (field.field < declaring.fields.size())
        return declaring.fields[field.field].key;
    size_t place = field.field - declaring.fields.size();
    const Vector<PropertyKeyID>& appendedKeys = layout(field.layout).appendedKeys;
    return place < appendedKeys.size() ? appendedKeys[place] : PropertyKeyID();
}

static bool keepsBytesZeroInSlotPlan(const PlannedLayout& layout, uint32_t offset, uint32_t width)
{
    return layout.bytesKeptZero.containsIf([&](const ReservedBytes& reserved) {
        return reserved.offset <= offset && offset + width <= reserved.offset + reserved.width;
    });
}

// Whatever the merged slot claims holds of the slot of every member. A type that the members do not share is none, which claims nothing.
static void meetWithSlotOfMemberInSlotPlan(PlacedSlot& merged, const PlacedSlot& placed, bool isStore)
{
    // A read asks the key order where some member may list the field without storage. A store of undefined asks for no storage only where every member may.
    if (isStore)
        merged.mayExistWithoutStorage &= placed.mayExistWithoutStorage;
    else
        merged.mayExistWithoutStorage |= placed.mayExistWithoutStorage;
    merged.holdsAccessor |= placed.holdsAccessor;
    merged.isFilledAtBirth &= placed.isFilledAtBirth;
    merged.deferredClosureCapturesObject |= placed.deferredClosureCapturesObject;
    merged.deferredClosureCapturesEnvironment |= placed.deferredClosureCapturesEnvironment;
    if (merged.type != placed.type)
        merged.type = { };
    if (merged.typeOfRawValue != placed.typeOfRawValue)
        merged.typeOfRawValue = { };
}

FieldAccess SlotPlan::fieldAccess(const Program& program, PropertyKeyID key, LayoutSetID set, std::span<const LayoutID> members, bool isStore) const
{
    if (!m_fieldAccessesOfSets || !set || holdsOneMember(set) || liesInThread(key))
        return fieldAccess(program, key, members, isStore);
    // `location.locals`, behind a test for the sixty kinds of nodes that have the name: the walk, its rules of forms and each emitter ask about the sixty, at every such read, and the set dies with each function.
    if (liesInThread(set)) {
        static constexpr size_t fewestMembersWhoseAnswerIsKept = 8;
        if (members.size() < fewestMembersWhoseAnswerIsKept)
            return fieldAccess(program, key, members, isStore);
        uint64_t hash = key.bits();
        for (LayoutID member : members)
            hash = (hash ^ member.index()) * 0x9e3779b97f4a7c15ull;
        // Neither the empty key of the table nor the deleted one.
        hash = (hash | 1) & ~(1ull << 63);
        FieldAccessesOfSets::Part& part = m_fieldAccessesOfSets->parts[(hash >> 32) % m_fieldAccessesOfSets->parts.size()];
        auto findIn = [&](const Vector<std::unique_ptr<FieldAccessesOfSets::Part::OfMembers>, 1>& kept) -> const FieldAccess* {
            for (const auto& entry : kept) {
                if (entry->key == key && std::ranges::equal(entry->members, members))
                    return &entry->access;
            }
            return nullptr;
        };
        {
            Locker locker { part.lock };
            if (auto known = part.accessesOfMembers[isStore].find(hash); known != part.accessesOfMembers[isStore].end()) {
                if (const FieldAccess* access = findIn(known->value))
                    return *access;
            }
        }
        auto entry = makeUniqueWithoutFastMallocCheck<FieldAccessesOfSets::Part::OfMembers>();
        entry->key = key;
        entry->members.append(members);
        entry->access = fieldAccess(program, key, members, isStore);
        FieldAccess access = entry->access;
        Locker locker { part.lock };
        auto& kept = part.accessesOfMembers[isStore].add(hash, Vector<std::unique_ptr<FieldAccessesOfSets::Part::OfMembers>, 1>()).iterator->value;
        if (!findIn(kept))
            kept.append(WTF::move(entry));
        return access;
    }

    uint64_t asked = static_cast<uint64_t>(set.bits()) << 32 | key.bits();
    FieldAccessesOfSets::Part& part = m_fieldAccessesOfSets->parts[WTF::intHash(asked) % m_fieldAccessesOfSets->parts.size()];
    {
        Locker locker { part.lock };
        if (auto known = part.accesses[isStore].find(asked); known != part.accesses[isStore].end())
            return *known->value;
    }
    FieldAccess access = fieldAccess(program, key, members, isStore);
    Locker locker { part.lock };
    part.accesses[isStore].add(asked, makeUniqueWithoutFastMallocCheck<FieldAccess>(access));
    return access;
}

FieldAccess SlotPlan::fieldAccessOfOneLayout(const Program& program, PropertyKeyID key, LayoutID receiver) const
{
    FieldAccess access;
    access.key = key;
    if (isNeverAllocated(receiver) || !holdsCompiledObjects(receiver))
        return access;
    std::optional<FieldIndex> field = findField(program, receiver, key);
    if (!field)
        return access;
    const PlacedSlot& placed = slot(receiver, *field);
    if (!placed.hasStorage())
        return access;

    access.hasLocation = true;
    access.coverage = FieldAccess::Coverage::EveryMember;
    access.mayBeFilledWithoutStorage = placed.mayExistWithoutStorage;
    access.slot = placed;
    if (placed.needsExtension())
        access.extensionWord = extensionWord(receiver);
    access.membersWithField.append(receiver);
    return access;
}

FieldAccess SlotPlan::fieldAccess(const Program& program, PropertyKeyID key, std::span<const LayoutID> receivers, bool isStore) const
{
    if (receivers.size() == 1)
        return fieldAccessOfOneLayout(program, key, receivers[0]);

    Vector<LayoutID, 8> ascendingReceivers;
    if (std::ranges::adjacent_find(receivers, std::ranges::greater_equal { }) != receivers.end()) {
        ascendingReceivers.append(receivers);
        AOT::sort(ascendingReceivers);
        ascendingReceivers.shrink(ascendingReceivers.size() - std::ranges::unique(ascendingReceivers).size());
        receivers = ascendingReceivers.span();
    }

    FieldAccess access;
    access.key = key;
    Vector<const PlacedSlot*, 4> slotsOfMembers;
    Vector<LayoutID, 4> membersWithoutSlot;
    // Such a field may lie among the extra properties, whatever the bytes of the cell hold.
    bool someMemberHasFieldWithoutStorage = false;
    for (LayoutID member : receivers) {
        if (isNeverAllocated(member))
            continue;
        const PlacedSlot* placed = nullptr;
        if (holdsCompiledObjects(member)) {
            if (std::optional<FieldIndex> field = findField(program, member, key))
                placed = &slot(member, *field);
        }
        if (!placed || !placed->hasStorage()) {
            someMemberHasFieldWithoutStorage |= !!placed;
            membersWithoutSlot.append(member);
            continue;
        }
        access.membersWithField.append(member);
        slotsOfMembers.append(placed);
    }
    if (slotsOfMembers.isEmpty())
        return access;

    const PlacedSlot noExtensionWord { };
    auto extensionWordOfMember = [&](size_t index) -> const PlacedSlot& {
        return slotsOfMembers[index]->needsExtension() ? extensionWord(access.membersWithField[index]) : noExtensionWord;
    };
    const PlacedSlot& first = *slotsOfMembers[0];
    size_t lowest = 0;
    bool hasOneLocation = true;
    // A deferring slot is read by operations of its own, which know one place.
    bool everySlotLiesInCellInOneForm = first.representation != SlotRepresentation::DeferredClosure;
    for (size_t index = 0; index < slotsOfMembers.size(); ++index) {
        const PlacedSlot& placed = *slotsOfMembers[index];
        hasOneLocation &= placed.hasLocationOf(first) && extensionWordOfMember(index).hasLocationOf(extensionWordOfMember(0));
        everySlotLiesInCellInOneForm &= placed.storage == SlotStorage::Inline && placed.representation == first.representation;
        if (placed.offset < slotsOfMembers[lowest]->offset)
            lowest = index;
    }

    bool coversEveryMember = membersWithoutSlot.isEmpty();
    access.coverage = coversEveryMember ? FieldAccess::Coverage::EveryMember : FieldAccess::Coverage::SomeMembers;
    if (!isStore && hasOneLocation && !coversEveryMember && !someMemberHasFieldWithoutStorage && first.storage == SlotStorage::Inline && representsAbsentAsZero(first.representation)) {
        bool membersWithoutSlotKeepBytesZero = std::ranges::all_of(membersWithoutSlot, [&](LayoutID member) {
            return holdsCompiledObjects(member) && keepsBytesZeroInSlotPlan(layout(member), first.offset, first.width());
        });
        if (membersWithoutSlotKeepBytesZero)
            access.coverage = FieldAccess::Coverage::EveryMemberHasSlot;
    }
    // A store writes without asking whether the object has the slot.
    bool everyObjectAskedHasSlot = !isStore || coversEveryMember;
    access.findsOffsetByShape = !hasOneLocation && everySlotLiesInCellInOneForm && everyObjectAskedHasSlot;
    access.hasLocation = access.findsOffsetByShape || (hasOneLocation && everyObjectAskedHasSlot);

    // Without a location the slot stays one without storage.
    if (access.hasLocation) {
        access.slot = *slotsOfMembers[lowest];
        access.extensionWord = extensionWordOfMember(lowest);
    }
    for (const PlacedSlot* placed : slotsOfMembers) {
        access.mayBeFilledWithoutStorage |= placed->mayExistWithoutStorage;
        if (access.hasLocation)
            meetWithSlotOfMemberInSlotPlan(access.slot, *placed, isStore);
    }
    // The objects of the members without the field have the slot for ever empty.
    if (access.coverage == FieldAccess::Coverage::EveryMemberHasSlot)
        access.slot.isFilledAtBirth = false;
    if (access.membersWithField.size() > 1) {
        access.slot.deferredFunction = { };
        access.slot.indexInLayoutRecord = std::nullopt;
    }
    return access;
}

static Type typeHeldInSlotOfSlotPlan(const Program& program, const PlacedSlot& placed)
{
    return program.at(placed.type ? placed.type : TypeTable::anything);
}

std::optional<uint32_t> SlotPlan::recordOfStoredField(const Program& program, PropertyKeyID key, std::span<const LayoutID> receivers) const
{
    constexpr OptionSet<FieldFlags> flagsOfPrivateNames { FieldFlags::IsPrivate, FieldFlags::IsBrand };
    std::optional<uint16_t> index;
    for (LayoutID member : receivers) {
        if (isNeverAllocated(member))
            continue;
        if (!holdsCompiledObjects(member))
            return std::nullopt;
        std::optional<FieldIndex> field = findField(program, member, key);
        if (!field)
            return std::nullopt;
        // An appended field is a plain property under a name.
        if (*field < program.at(member).fields.size()) {
            const Field& declaration = fieldAt(program, declarationOf(program, { member, *field }));
            if (!declaration.form.isEmpty() || declaration.flags.containsAny(flagsOfPrivateNames))
                return std::nullopt;
        }
        const PlacedSlot& placed = slot(member, *field);
        if (!placed.hasStorage() || placed.holdsAccessor || placed.representation == SlotRepresentation::DeferredClosure || !placed.indexInLayoutRecord)
            return std::nullopt;
        if (index && *index != *placed.indexInLayoutRecord)
            return std::nullopt;
        index = placed.indexInLayoutRecord;
    }
    if (!index)
        return std::nullopt;
    return static_cast<uint32_t>(sizeof(LayoutData) + *index * sizeof(FieldData));
}

// A stub that goes by the form stores without asking FieldData::type, which is `anything` for a slot without a type.
static StoredFieldForm formOfStoredFieldInSlotOfSlotPlan(const Program& program, const PlacedSlot& placed)
{
    if (!placed.hasStorage() || placed.isOutOfLine())
        return StoredFieldForm::Any;
    Type held = typeHeldInSlotOfSlotPlan(program, placed);
    bool holdsValuesAsTheyAre = placed.representation == SlotRepresentation::JSValue;
    if (holdsValuesAsTheyAre && held.isAnything())
        return StoredFieldForm::TakesAnyValue;
    bool takesEveryNumber = (held.primitives & Primitive::Number) == Primitive::Number && held.integers == Range::full() && held.boundsOfFractions.isEmpty();
    if (!takesEveryNumber || !held.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull))
        return StoredFieldForm::Any;
    if (holdsValuesAsTheyAre)
        return StoredFieldForm::KeepsNumbersAsValues;
    bool holdsDoubles = placed.representation == SlotRepresentation::Double || placed.representation == SlotRepresentation::OptionalDouble;
    return holdsDoubles ? StoredFieldForm::AcceptsAnyNumber : StoredFieldForm::Any;
}

StoredFieldForm SlotPlan::formOfStoredField(const Program& program, PropertyKeyID key, std::span<const LayoutID> receivers) const
{
    std::optional<StoredFieldForm> form;
    for (LayoutID member : receivers) {
        if (isNeverAllocated(member))
            continue;
        std::optional<FieldIndex> field = findField(program, member, key);
        if (!field)
            return StoredFieldForm::Any;
        StoredFieldForm formInMember = formOfStoredFieldInSlotOfSlotPlan(program, slot(member, *field));
        if (form && *form != formInMember)
            return StoredFieldForm::Any;
        form = formInMember;
    }
    return form.value_or(StoredFieldForm::Any);
}

// ---- How the plan is made

namespace {

// In ascending order each holds what the one in front holds.
enum class FormOfPlaceInSlotPlan : uint8_t {
    Unclaimed,
    Integer,
    Number,
    AnyValue,
};

// Where the fields of every layout lie, whether or not cells of the layout are made: what is written below a layout goes by it.
struct PlacesOfFieldsInSlotPlan {
    static constexpr uint32_t noPlace = UINT32_MAX;

    uint32_t rowOf(FieldReference field) const { return firstRowOfLayout[field.layout.index()] + field.field; }
    // Of an object. The hidden words lie in front of the fields.
    uint64_t offsetOfFirstSlotOf(LayoutID id) const { return offsetOfFirstField + (layoutsWithHiddenWords.quickGet(id.index()) ? bytesPerSlotInSlotPlan : 0); }

    // One more entry than there are layouts.
    Vector<uint32_t> firstRowOfLayout;
    // By row.
    Vector<uint64_t> offsets;
    // By row. The rows with one number are one field as a class and the classes below it list it. They lie at one offset in one form.
    Vector<uint32_t> places;
    // By place.
    Vector<FormOfPlaceInSlotPlan> formsOfPlaces;
    // By row: typeHeldOfFieldForSlotPlan(). Null: the slot is held to no more than its form says.
    Vector<TypeID> typesHeld;
    // By layout, for each name of LinkTables::appendedKeysOf(): the same.
    Vector<Vector<TypeID>> typesHeldOfAppendedFields;
    // By layout: behind its last field.
    Vector<uint64_t> ends;
    // By layout, for each name of LinkTables::appendedKeysOf(): the granule of the Extension that its slot takes. inCell: the slot lies in the cell.
    static constexpr uint8_t inCell = UINT8_MAX;
    // By layout, then by the same names: the granule is that of the name in every member of a family, so that a read through the family finds it in one place.
    Vector<BitVector> appendedFieldsThatShareGranule;
    Vector<Vector<uint8_t>> granulesOfAppendedFields;
    // By row. inCell: nearly all. `this.exports = undefined`, and no maker goes on to fill it: the granule that the slot of a listed field takes. granuleNotChosen: while the fields are placed, one that will get a granule.
    static constexpr uint8_t granuleNotChosen = inCell - 1;
    // The field has no slot, and no place in the record of the layout: what is stored under its name is an extra property.
    static constexpr uint8_t withoutSlot = inCell - 2;
    Vector<uint8_t> granulesOfListedFields;
    // By row: LinkedCell::isGivenByNoMaker of a row that was chosen. Its key is in no order of a birth.
    BitVector rowsGivenByNoMaker;
    bool liesOutOfLine(uint32_t row) const { return granulesOfListedFields[row] != inCell; }
    // By layout: some row of it does.
    BitVector layoutsWithListedFieldsOutOfLine;
    // By layout: findLayoutsWithHiddenWordsForSlotPlan().
    BitVector layoutsWithHiddenWords;
    // By layout: PlannedLayout::bytesKeptZero. placeFieldsOfUnions() fills it. Empty: no layout keeps any.
    Vector<Vector<ReservedBytes>> bytesKeptZero;
};

struct StorageOfLayoutInSlotPlan {
    bool hasCells() const { return (storage == LayoutStorage::CompiledObject && !isRecordAlone) || storage == LayoutStorage::Environment; }

    LayoutStorage storage { LayoutStorage::None };
    bool isRecordAlone { false };
};

} // anonymous namespace

// What reached code does not allocate has no shape, no record and no name in the image. Link's tables at their defaults say of every layout that it may be allocated.
static BitVector findLayoutsNeverAllocatedForSlotPlan(const Program& program, const LinkTables& linked)
{
    BitVector layoutsNeverAllocated(program.layouts.size());
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].has(LayoutFlags::IsNeverAllocated) || (index < linked.layouts.size() && !linked.layouts[index].isAllocated))
            layoutsNeverAllocated.quickSet(index);
    }
    // No body names a member of a tagged family.
    for (const TaggedFamily& family : program.taggedFamilies) {
        for (const TaggedLayout& member : family.members)
            layoutsNeverAllocated.quickSet(member.layout.index());
    }
    return layoutsNeverAllocated;
}

// The generic operations on a private name find it in the record of the class that declares it, whatever object they are given.
static bool declaresPrivateNameForSlotPlan(const Layout& layout)
{
    constexpr OptionSet<FieldFlags> flagsOfPrivateNames { FieldFlags::IsPrivate, FieldFlags::IsBrand };
    return std::ranges::any_of(layout.fields, [&](const Field& field) {
        return field.flags.containsAny(flagsOfPrivateNames) && !field.flags.contains(FieldFlags::IsInherited);
    });
}

static StorageOfLayoutInSlotPlan chooseStorageOfLayoutForSlotPlan(const Layout& layout, bool isNeverAllocated)
{
    // The constructor of a built-in class makes the instances of the classes below it: cells of that class, which have no slots.
    bool hasCellsOfItsOwn = !isNeverAllocated && !layout.writtenBuiltinBase;
    switch (layout.kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        if (hasCellsOfItsOwn)
            return { layout.kind == LayoutKind::Record && layout.holdsOrdinaryObjects ? LayoutStorage::OrdinaryObject : LayoutStorage::CompiledObject, false };
        if (declaresPrivateNameForSlotPlan(layout))
            return { LayoutStorage::CompiledObject, true };
        return { };
    case LayoutKind::Environment:
    case LayoutKind::CoroutineFrame:
        return { hasCellsOfItsOwn ? LayoutStorage::Environment : LayoutStorage::None, false };
    case LayoutKind::Closure:
    case LayoutKind::ModuleNamespace:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The field of a layout above whose place the field takes: the declaration of what the layout has taken from above, or what its own field under the same name hides.
static std::optional<FieldReference> findFieldAboveForSlotPlan(const Program& program, const BitVector& isPlaced, FieldReference reference, LayoutID base)
{
    const Field& listed = fieldAt(program, reference);
    FieldReference above;
    if (listed.flags.contains(FieldFlags::IsInherited))
        above = declarationOf(program, reference);
    else if (std::optional<uint32_t> hidden = program.at(base).findField(listed.key))
        above = { base, *hidden };
    else
        return std::nullopt;
    // In a ring of written bases the declaration may stand in a layout that comes later.
    if (above.layout == reference.layout || !isPlaced.quickGet(above.layout.index()) || !isObjectLayout(program.at(above.layout).kind))
        return std::nullopt;
    return above;
}

// A FIELD THAT ITS MAKERS ONLY GIVE `undefined` CLAIMS NO SLOT IN THE CELL.
// `function Symbol(flags, name) { this.flags = flags; .. this.exports = undefined; this.exportSymbol = undefined; }`: the store gives every object the key, so that all have one shape, and fills nothing. One object in ten goes on to hold anything there.
// The key stays in the order of the birth. The slot is a granule of the Extension, which an object gets where something is stored: PlacedSlot::mayExistWithoutStorage.
// A guess, which decides where the slot lies and nothing else. It is wrong where code that is no maker fills the field of most objects: each of those pays an Extension.
// So it is asked of a field that the written type lets be left out, and of a layout whose slots nothing else decides: nothing lies above it or below it,
// or it is the layout of a tagged maker, and the members below have the field where the maker's layout has it.
// SO DOES A FIELD OF A RECORD THAT NO MAKER IN SIGHT GIVES ANYTHING. `interface Message { text: string; isMeta?: boolean; .. a hundred more .. }   { text, uuid }`: the type lists all that some message may hold, and a message holds a dozen of them.
// It is what a name is that Link appended: somebody may store it later, so it has a granule.
// `held`: Facts::held(), which follows the makers where an object of the layout has been in unseen hands.
static bool mayLieOutOfLineThoughListedInSlotPlan(const Program& program, const LinkTables& held, const BitVector& layoutsWithLayoutsBelow, FieldReference reference)
{
    if (!held.field(reference).isLeftUndefinedByMakers && !held.field(reference).isGivenByNoMaker)
        return false;
    const Layout& layout = program.at(reference.layout);
    if ((layout.kind != LayoutKind::Instance && layout.kind != LayoutKind::Record) || !isFoundByShapeTest(layout))
        return false;
    LayoutID top = writtenBaseOf(layout) ? writtenBaseOf(layout) : reference.layout;
    if (writtenBaseOf(program.at(top)) || (layoutsWithLayoutsBelow.quickGet(top.index()) && !onlyTaggedMembersLieBelow(program, top)))
        return false;
    const Field& field = layout.fields[reference.field];
    constexpr OptionSet<FieldFlags> flagsOfFieldsInCell { FieldFlags::IsMethod, FieldFlags::IsPrivate, FieldFlags::IsImmutable, FieldFlags::IsBrand, FieldFlags::IsHidden, FieldFlags::IsAlsoInPrototype, FieldFlags::IsBrandOfMethods };
    if (!field.key || field.flags.containsAny(flagsOfFieldsInCell) || !field.form.isEmpty() || layout.findField(field.key) != reference.field || declarationOf(program, reference) != reference)
        return false;
    // What a kind of its own lists is filled by that kind's maker or, like `locals` and `flowNode`, by code that is no maker: nobody says which.
    if (top != reference.layout)
        return false;
    // `interface Symbol { flags: number; exports?: SymbolTable }   function Symbol(this: Symbol, flags) { .. }`: the layout of a constructor requires all that the constructor stores. The types that are written of the objects say what may be left out.
    bool isWritten = false;
    for (LayoutID written : layout.supertypes) {
        std::optional<uint32_t> listed = program.at(written).findField(field.key);
        if (!listed)
            continue;
        // `constEnumOnlyModule: boolean | undefined` has to be written out where an object is made, and may be written `undefined`.
        const Field& asWritten = program.at(written).fields[*listed];
        if (asWritten.flags.contains(FieldFlags::IsRequired) && !(asWritten.declared.written() && program.at(asWritten.declared.written()).primitives & Primitive::Undefined && !asWritten.declared.has(PositionFlags::OmitsUndefined)))
            return false;
        isWritten = true;
    }
    return isWritten || !field.flags.contains(FieldFlags::IsRequired);
}

static bool needsHiddenWordInSlotPlan(const Program& program, const LinkTables& link, LayoutID id)
{
    constexpr OptionSet<LayoutChange> changesThatMakeExtraProperties { LayoutChange::GainsProperty, LayoutChange::GetsAccessor };
    LayoutKind kind = program.at(id).kind;
    // A container keeps its elements among the extra properties.
    if (kind != LayoutKind::Record && kind != LayoutKind::Instance)
        return true;
    if (id.index() >= link.layouts.size() || link.layout(id).possibleChanges.containsAny(changesThatMakeExtraProperties))
        return true;
    // They may lie in the Extension.
    return link.numberOfAppendedKeysOfPlan(id);
}

// A property that the cell and the Extension have no room for lies among the extra properties of the object: one that its layout has no field for, or a field that a descriptor has defined anew. A hidden word names them.
// THAT WORD IS A MATTER OF COST ALONE. An object without it gets such properties all the same and pays with a Structure of its own: StructureTable::holdingExtraProperties().
// With an even number of fields the hidden words make the cell 16 bytes larger: a pair is 48 bytes in place of 32.
// What a layout has from the layout above lies where it lies there, and what the members of a family share lies at one slot in all of them. So a row of classes has the words or lacks them as one, and so does a family.
// `link` null: every object has them.
static BitVector findLayoutsWithHiddenWordsForSlotPlan(const Program& program, const LinkTables* link, const Families* families)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    BitVector layoutsWithWords(numberOfLayouts);
    auto isObject = [&](uint32_t index) {
        return isObjectLayout(program.layouts[index].kind);
    };
    UnitedFamilies groups(numberOfLayouts);
    for (uint32_t index = 0; link && index < numberOfLayouts; ++index) {
        if (LayoutID base = isObject(index) ? writtenBaseOf(program.layouts[index]) : LayoutID())
            groups.unite(index, base.index());
    }
    for (uint32_t number = 0; link && families && number < families->size(); ++number) {
        // Any two families are nested or apart: what another holds adds nothing to what that one unites.
        if (families->nearestFamilyAbove(FamilyID(number)))
            continue;
        LayoutID first;
        program.at(families->layoutsOf(FamilyID(number))).forEach([&](LayoutID member) {
            if (!first)
                first = member;
            groups.unite(first.index(), member.index());
        });
    }
    BitVector groupsWithWords(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (isObject(index) && (!link || needsHiddenWordInSlotPlan(program, *link, LayoutID(index))))
            groupsWithWords.quickSet(groups.representativeOf(index));
    }
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (isObject(index) && groupsWithWords.quickGet(groups.representativeOf(index)))
            layoutsWithWords.quickSet(index);
    }
    return layoutsWithWords;
}

template<typename Functor>
static void forEachFieldInDeclaredOrderForSlotPlan(const Layout& layout, const Functor& functor)
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

// The families in which findMembersThatAgreeOnSlotsForSlotPlan() can find anybody: some member has cells, and each that has them is free.
// Who asks every family goes through the members of these alone: in a row of classes that extend one another each class has a family of all below it, and none of them is one of these.
static BitVector findFamiliesThatMayAgreeOnSlotsForSlotPlan(const Program& program, const Families& families, std::span<const StorageOfLayoutInSlotPlan> storage)
{
    auto hasCells = [&](LayoutID member) {
        return storage[member.index()].storage == LayoutStorage::CompiledObject && storage[member.index()].hasCells();
    };
    BitVector result = families.familiesWithSomeMemberThat(hasCells);
    result.exclude(families.familiesWithSomeMemberThat([&](LayoutID member) {
        return hasCells(member) && (!isFoundByShapeTest(program.at(member)) || writtenBaseOf(program.at(member)));
    }));
    return result;
}

// The members whose objects a test of the family lets through and whose slots nothing else decides. Empty: they are fewer than two, or one of them has its fields where the layout above it has them.
// A class and what extends it share what is inherited.
static void findMembersThatAgreeOnSlotsForSlotPlan(const Program& program, const Families& families, const BitVector& familiesThatMayAgree, FamilyID family, std::span<const StorageOfLayoutInSlotPlan> storage, Vector<LayoutID, 16>& members)
{
    members.shrink(0);
    if (!familiesThatMayAgree.quickGet(family.index()))
        return;
    bool everyMemberIsFree = true;
    program.at(families.layoutsOf(family)).forEach([&](LayoutID member) {
        if (storage[member.index()].storage != LayoutStorage::CompiledObject || !storage[member.index()].hasCells())
            return;
        members.append(member);
        everyMemberIsFree &= isFoundByShapeTest(program.at(member)) && !writtenBaseOf(program.at(member));
    });
    if (members.size() < 2 || !everyMemberIsFree)
        members.shrink(0);
}

// What every member of a family lists lies in one place in all of them: in the cell of all, or out of line in all.
static void chooseListedFieldsOutOfLineForSlotPlan(const Program& program, const LinkTables& link, const LinkTables& held, const Families& families, std::span<const StorageOfLayoutInSlotPlan> storage, PlacesOfFieldsInSlotPlan& result)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    BitVector keysLeftOut;
    BitVector layoutsWithLayoutsBelow(numberOfLayouts);
    for (const Layout& layout : program.layouts) {
        if (LayoutID base = isObjectLayout(layout.kind) ? writtenBaseOf(layout) : LayoutID())
            layoutsWithLayoutsBelow.quickSet(base.index());
    }
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        // The word that names the Extension lies beside the word that names the extra properties. A layout without them would pay for both, and so would all that share its slots.
        if (storage[index].storage != LayoutStorage::CompiledObject || !storage[index].hasCells() || index >= held.layouts.size() || !result.layoutsWithHiddenWords.quickGet(index))
            continue;
        // `const api = {} as Api;` in the body of a module, filled by whoever comes: one object is made, so there is nothing to save, and every read would pay.
        if (FunctionID maker = held.layouts[index].onlyFunctionThatMakesObjects; maker && (program.at(maker).isModuleBody() || link.function(maker).runsAtMostOnce))
            continue;
        for (uint32_t field = 0; field < program.layouts[index].fields.size(); ++field) {
            if (!mayLieOutOfLineThoughListedInSlotPlan(program, held, layoutsWithLayoutsBelow, { LayoutID(index), field }))
                continue;
            result.granulesOfListedFields[result.rowOf({ LayoutID(index), field })] = PlacesOfFieldsInSlotPlan::granuleNotChosen;
            if (held.field({ LayoutID(index), field }).isGivenByNoMaker)
                result.rowsGivenByNoMaker.set(result.rowOf({ LayoutID(index), field }));
            keysLeftOut.set(program.layouts[index].fields[field].key.index());
        }
    }
    if (keysLeftOut.isEmpty())
        return;

    Vector<LayoutID, 16> members;
    Vector<uint32_t, 16> rows;
    BitVector familiesThatMayAgree = findFamiliesThatMayAgreeOnSlotsForSlotPlan(program, families, storage);
    for (uint32_t number = families.size(); number--;) {
        findMembersThatAgreeOnSlotsForSlotPlan(program, families, familiesThatMayAgree, FamilyID(number), storage, members);
        if (members.isEmpty())
            continue;
        const Layout& first = program.at(members[0]);
        for (uint32_t field = 0; field < first.fields.size(); ++field) {
            PropertyKeyID key = first.fields[field].key;
            if (!key || !keysLeftOut.get(key.index()) || first.findField(key) != field)
                continue;
            rows.shrink(0);
            for (LayoutID member : members) {
                std::optional<uint32_t> listed = program.at(member).findField(key);
                if (!listed)
                    break;
                rows.append(result.rowOf({ member, *listed }));
            }
            bool everyMemberLeavesItOut = std::ranges::all_of(rows, [&](uint32_t row) {
                return result.liesOutOfLine(row);
            });
            if (rows.size() < members.size() || everyMemberLeavesItOut)
                continue;
            for (uint32_t row : rows)
                result.granulesOfListedFields[row] = PlacesOfFieldsInSlotPlan::inCell;
        }
    }

    // What a layout has from the layout above lies where it lies there.
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        LayoutID base = isObjectLayout(program.layouts[index].kind) ? writtenBaseOf(program.layouts[index]) : LayoutID();
        if (!base || storage[index].storage != LayoutStorage::CompiledObject || !storage[index].hasCells())
            continue;
        for (uint32_t field = 0; field < program.layouts[index].fields.size(); ++field) {
            PropertyKeyID key = program.layouts[index].fields[field].key;
            std::optional<uint32_t> above = key && keysLeftOut.get(key.index()) ? program.at(base).findField(key) : std::nullopt;
            if (above && result.liesOutOfLine(result.rowOf({ base, *above })))
                result.granulesOfListedFields[result.rowOf({ LayoutID(index), field })] = PlacesOfFieldsInSlotPlan::granuleNotChosen;
        }
    }

    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (storage[index].storage != LayoutStorage::CompiledObject)
            continue;
        for (uint32_t row = result.firstRowOfLayout[index]; row < result.firstRowOfLayout[index + 1]; ++row) {
            if (result.liesOutOfLine(row))
                result.layoutsWithListedFieldsOutOfLine.quickSet(index);
        }
    }
}

// FIELDS THAT ALL MEMBERS OF A DECLARED FAMILY SHARE LIE AT ONE SLOT IN ALL OF THEM, so a read through the family is one load behind one test of the family.
// `interface Node { kind; flags; parent }` above what `function Node() { .. }`, `function Token() { .. }` and `function Identifier() { .. }` make.
// By family, the largest first: the names that every member lists come in front of all else, in the order in which the first member declares them.
// Any two families are nested or have no layout in common. So what a larger family has laid down is the same in all members of a smaller one, which goes on behind it.
static void placeFieldsSharedByFamiliesForSlotPlan(const Program& program, const Families& families, std::span<const StorageOfLayoutInSlotPlan> storage, PlacesOfFieldsInSlotPlan& result)
{
    Vector<LayoutID, 16> members;
    Vector<uint32_t, 16> rows;
    BitVector familiesThatMayAgree = findFamiliesThatMayAgreeOnSlotsForSlotPlan(program, families, storage);
    for (uint32_t number = families.size(); number--;) {
        findMembersThatAgreeOnSlotsForSlotPlan(program, families, familiesThatMayAgree, FamilyID(number), storage, members);
        if (members.isEmpty())
            continue;
        uint64_t end = std::max(result.ends[members[0].index()], result.offsetOfFirstSlotOf(members[0]));
        for (LayoutID member : members)
            RELEASE_ASSERT(std::max(result.ends[member.index()], result.offsetOfFirstSlotOf(member)) == end);
        const Layout& first = program.at(members[0]);
        forEachFieldInDeclaredOrderForSlotPlan(first, [&](uint32_t field) {
            PropertyKeyID key = first.fields[field].key;
            if (!key || first.findField(key) != field)
                return;
            rows.shrink(0);
            for (LayoutID member : members) {
                std::optional<uint32_t> listed = program.at(member).findField(key);
                if (!listed || result.places[result.rowOf({ member, *listed })] != PlacesOfFieldsInSlotPlan::noPlace || result.liesOutOfLine(result.rowOf({ member, *listed })))
                    return;
                rows.append(result.rowOf({ member, *listed }));
            }
            for (uint32_t row : rows) {
                result.offsets[row] = end;
                result.places[row] = static_cast<uint32_t>(result.formsOfPlaces.size());
            }
            result.formsOfPlaces.append(FormOfPlaceInSlotPlan::Unclaimed);
            end += bytesPerSlotInSlotPlan;
        });
        for (LayoutID member : members)
            result.ends[member.index()] = end;
    }
}

// The same for the fields that Link appended and that lie out of line: a name that every member of a family has so takes one granule in all of them.
// What is stored under a name without a slot is an extra property. So nothing in sight stores under the name, or the layout may gain properties for all that Link has proven: then whoever leans on its objects looks,
// and the run time lets the property be added. Link finds the declared field at a store and notes no such change for it.
static bool mayBeNoFieldInSlotPlan(const LinkTables& link, FieldReference reference)
{
    if (link.layout(reference.layout).possibleChanges.contains(LayoutChange::GainsProperty))
        return true;
    const LinkedCell& cell = link.field(reference);
    return cell.everyWriterIsKnown && !cell.isWrittenAfterConstruction;
}

static void placeAppendedFieldsForSlotPlan(const Program& program, const LinkTables& link, const MentionsOfKeys& mentions, const Families* families, std::span<const StorageOfLayoutInSlotPlan> storage, PlacesOfFieldsInSlotPlan& result)
{
    constexpr uint8_t noGranule = PlacesOfFieldsInSlotPlan::inCell - 1;
    static_assert(maxAppendedFieldsOutOfLine <= noGranule && maxExtensionGranules <= PlacesOfFieldsInSlotPlan::withoutSlot);
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    result.granulesOfAppendedFields.grow(numberOfLayouts);
    result.appendedFieldsThatShareGranule.grow(numberOfLayouts);
    Vector<uint8_t> numbersOfGranulesTaken;
    numbersOfGranulesTaken.fill(0, numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (storage[index].storage != LayoutStorage::CompiledObject || !storage[index].hasCells())
            continue;
        // `interface Type { flags: number; id: number }   function Type(this: Type, flags) { this.flags = flags; }   const made = new Type(flags); made.id = next;`
        // Whoever has an object made fills what the written type of `this` requires, so every object gets it. A type that only a cast leads to is written of some of the objects. A guess, which decides where the slot lies and nothing else.
        uint32_t numberListed = static_cast<uint32_t>(program.layouts[index].fields.size());
        for (uint32_t place = 0; place < link.numberOfAppendedKeysOfPlan(LayoutID(index)); ++place)
            result.granulesOfAppendedFields[index].append(link.isAppendedFieldRequiredByTypeOfThis({ LayoutID(index), numberListed + place }) ? PlacesOfFieldsInSlotPlan::inCell : noGranule);
    }

    // A row has a place, which says with which rows it shares its form and what it is held to.
    auto addPlaceOutOfLine = [&] {
        result.formsOfPlaces.append(FormOfPlaceInSlotPlan::AnyValue);
        return static_cast<uint32_t>(result.formsOfPlaces.size() - 1);
    };

    Vector<LayoutID, 16> members;
    Vector<uint8_t*, 16> granules;
    Vector<size_t, 16> placesInMembers;
    Vector<uint32_t, 16> rows;
    BitVector familiesThatMayAgree = families ? findFamiliesThatMayAgreeOnSlotsForSlotPlan(program, *families, storage) : BitVector();
    for (uint32_t number = families ? families->size() : 0; number--;) {
        findMembersThatAgreeOnSlotsForSlotPlan(program, *families, familiesThatMayAgree, FamilyID(number), storage, members);
        if (members.isEmpty())
            continue;
        const Layout& first = program.at(members[0]);
        for (uint32_t field = 0; result.layoutsWithListedFieldsOutOfLine.quickGet(members[0].index()) && field < first.fields.size() && numbersOfGranulesTaken[members[0].index()] < maxExtensionGranules; ++field) {
            if (result.granulesOfListedFields[result.rowOf({ members[0], field })] != PlacesOfFieldsInSlotPlan::granuleNotChosen)
                continue;
            rows.shrink(0);
            for (LayoutID member : members) {
                std::optional<uint32_t> listed = first.fields[field].key ? program.at(member).findField(first.fields[field].key) : std::nullopt;
                if (!listed || result.granulesOfListedFields[result.rowOf({ member, *listed })] != PlacesOfFieldsInSlotPlan::granuleNotChosen)
                    break;
                rows.append(result.rowOf({ member, *listed }));
            }
            if (rows.size() < members.size())
                continue;
            uint8_t granule = numbersOfGranulesTaken[members[0].index()];
            uint32_t place = addPlaceOutOfLine();
            for (uint32_t row : rows) {
                result.granulesOfListedFields[row] = granule;
                result.places[row] = place;
            }
            for (LayoutID member : members)
                RELEASE_ASSERT(numbersOfGranulesTaken[member.index()]++ == granule);
        }
        Vector<PropertyKeyID, 16> keysOfFirst = link.appendedKeysOfPlan(members[0]);
        for (PropertyKeyID key : keysOfFirst) {
            granules.shrink(0);
            placesInMembers.shrink(0);
            for (LayoutID member : members) {
                Vector<PropertyKeyID, 16> keys = link.appendedKeysOfPlan(member);
                size_t place = std::ranges::find(keys, key) - keys.begin();
                if (place == keys.size() || result.granulesOfAppendedFields[member.index()][place] != noGranule)
                    break;
                granules.append(&result.granulesOfAppendedFields[member.index()][place]);
                placesInMembers.append(place);
            }
            if (granules.size() < members.size() || numbersOfGranulesTaken[members[0].index()] >= maxExtensionGranules)
                continue;
            uint8_t granule = numbersOfGranulesTaken[members[0].index()];
            for (uint8_t* place : granules)
                *place = granule;
            for (LayoutID member : members)
                RELEASE_ASSERT(numbersOfGranulesTaken[member.index()]++ == granule);
            for (size_t index = 0; index < members.size() && members.size() > 1; ++index)
                result.appendedFieldsThatShareGranule[members[index].index()].set(placesInMembers[index]);
        }
    }

    // An Extension has room for so many granules. What finds none lies in the cell. A layout comes behind the layout above it, and has what it has from there in the granule that it has there.
    BitVector isNumbered(numberOfLayouts);
    Vector<uint32_t, 64> rowsWaiting;
    auto numberGranulesOf = [&](auto& self, uint32_t index) -> void {
        if (isNumbered.quickSet(index))
            return;
        LayoutID base = isObjectLayout(program.layouts[index].kind) ? writtenBaseOf(program.layouts[index]) : LayoutID();
        if (base)
            self(self, base.index());
        bool hasListedFieldsOutOfLine = result.layoutsWithListedFieldsOutOfLine.quickGet(index);
        for (uint32_t field = 0; hasListedFieldsOutOfLine && field < program.layouts[index].fields.size(); ++field) {
            uint32_t row = result.rowOf({ LayoutID(index), field });
            std::optional<uint32_t> above = base && result.granulesOfListedFields[row] == PlacesOfFieldsInSlotPlan::granuleNotChosen ? program.at(base).findField(program.layouts[index].fields[field].key) : std::nullopt;
            if (!above)
                continue;
            uint32_t rowAbove = result.rowOf({ base, *above });
            result.granulesOfListedFields[row] = result.granulesOfListedFields[rowAbove];
            if (!result.liesOutOfLine(row) || result.granulesOfListedFields[row] == PlacesOfFieldsInSlotPlan::withoutSlot)
                continue;
            RELEASE_ASSERT(result.granulesOfListedFields[row] < maxExtensionGranules);
            result.places[row] = result.places[rowAbove];
            numbersOfGranulesTaken[index] = std::max<uint8_t>(numbersOfGranulesTaken[index], result.granulesOfListedFields[row] + 1);
        }
        // A record may list more than an Extension has room for: `interface Settings { .. two hundred optional members .. }`. The granules go to the names that the program mentions in most functions.
        // What finds none and is given by no maker is no field: a cell with a slot for each would be as wide as the type is long. What a birth lists as `undefined` has its key in the order of the birth, so it lies in the cell,
        // and so does what code in sight stores under, where Link has ruled out that the objects gain properties.
        rowsWaiting.shrink(0);
        for (uint32_t row = result.firstRowOfLayout[index]; hasListedFieldsOutOfLine && row < result.firstRowOfLayout[index + 1]; ++row) {
            if (result.granulesOfListedFields[row] == PlacesOfFieldsInSlotPlan::granuleNotChosen)
                rowsWaiting.append(row);
        }
        if (numbersOfGranulesTaken[index] + rowsWaiting.size() > maxExtensionGranules) {
            auto mentionsOf = [&](uint32_t row) -> uint32_t {
                size_t key = program.layouts[index].fields[row - result.firstRowOfLayout[index]].key.index();
                return key < mentions.all.size() ? mentions.all[key] : 0;
            };
            std::ranges::stable_sort(rowsWaiting, [&](uint32_t a, uint32_t b) {
                return mentionsOf(a) > mentionsOf(b);
            });
        }
        for (uint32_t row : rowsWaiting) {
            uint8_t& granule = result.granulesOfListedFields[row];
            if (numbersOfGranulesTaken[index] < maxExtensionGranules) {
                granule = numbersOfGranulesTaken[index]++;
                result.places[row] = addPlaceOutOfLine();
            } else if (result.rowsGivenByNoMaker.get(row) && mayBeNoFieldInSlotPlan(link, { LayoutID(index), row - result.firstRowOfLayout[index] })) {
                granule = PlacesOfFieldsInSlotPlan::withoutSlot;
                result.places[row] = addPlaceOutOfLine();
            } else
                granule = PlacesOfFieldsInSlotPlan::inCell;
        }
        for (uint8_t& granule : result.granulesOfAppendedFields[index]) {
            if (granule == noGranule)
                granule = numbersOfGranulesTaken[index] < maxExtensionGranules ? numbersOfGranulesTaken[index]++ : PlacesOfFieldsInSlotPlan::inCell;
        }
    };
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        numberGranulesOf(numberGranulesOf, index);
}

// What a layout has from the layout above lies where it lies there. Behind that its other fields as Layout::declaredOrder lists them, and last those that it does not list.
// That is the order in which a row of classes declares and creates its fields: addDeclaredOrderOf() of AOTKeyOrders.cpp.
static void placeFieldsOfLayoutForSlotPlan(const Program& program, const BitVector& isPlaced, LayoutID id, PlacesOfFieldsInSlotPlan& result)
{
    const Layout& layout = program.at(id);
    bool isObject = isObjectLayout(layout.kind);
    LayoutID base = isObject ? writtenBaseOf(layout) : LayoutID();
    // `class A extends B {}` beside `class B extends A {}`: the first layout of a ring to be placed has nothing above it.
    if (base && (!isPlaced.quickGet(base.index()) || !isObjectLayout(program.at(base).kind)))
        base = { };

    // What its families share lies there by now.
    uint64_t end = isObject ? std::max(result.ends[id.index()], result.offsetOfFirstSlotOf(id)) : offsetOfFirstEnvironmentVariable;
    if (base)
        end = result.ends[base.index()];
    // placeFieldsOfUnions() may have placed its own fields by now.
    end = std::max(end, result.ends[id.index()]);
    uint32_t numberOfFields = static_cast<uint32_t>(layout.fields.size());
    for (uint32_t field = 0; base && field < numberOfFields; ++field) {
        std::optional<FieldReference> above = findFieldAboveForSlotPlan(program, isPlaced, { id, field }, base);
        if (!above || result.liesOutOfLine(result.rowOf({ id, field })))
            continue;
        uint32_t row = result.rowOf({ id, field });
        result.offsets[row] = result.offsets[result.rowOf(*above)];
        result.places[row] = result.places[result.rowOf(*above)];
        end = std::max(end, result.offsets[row] + bytesPerSlotInSlotPlan);
    }
    auto placeAtEnd = [&](uint32_t field) {
        if (field >= numberOfFields)
            return;
        uint32_t row = result.rowOf({ id, field });
        if (result.places[row] != PlacesOfFieldsInSlotPlan::noPlace)
            return;
        result.offsets[row] = end;
        result.places[row] = static_cast<uint32_t>(result.formsOfPlaces.size());
        result.formsOfPlaces.append(FormOfPlaceInSlotPlan::Unclaimed);
        end += bytesPerSlotInSlotPlan;
    };
    for (uint32_t field : layout.declaredOrder)
        placeAtEnd(field);
    for (uint32_t field = 0; field < numberOfFields; ++field)
        placeAtEnd(field);
    result.ends[id.index()] = end;
}

// `families`: null where typed operations are withheld. Then every layout has its fields in the order of declaration.
static PlacesOfFieldsInSlotPlan placeFieldsForSlotPlan(const Program& program, const LinkTables& link, const LinkTables& held, const MentionsOfKeys& mentions, const Families* families, std::span<const StorageOfLayoutInSlotPlan> storage, bool hasTypedSlots)
{
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    PlacesOfFieldsInSlotPlan result;
    result.firstRowOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    uint64_t numberOfRows = 0;
    for (const Layout& layout : program.layouts) {
        result.firstRowOfLayout.append(static_cast<uint32_t>(numberOfRows));
        numberOfRows += layout.fields.size();
    }
    RELEASE_ASSERT(numberOfRows < PlacesOfFieldsInSlotPlan::noPlace);
    result.firstRowOfLayout.append(static_cast<uint32_t>(numberOfRows));
    result.offsets.fill(0, numberOfRows);
    result.places.fill(PlacesOfFieldsInSlotPlan::noPlace, numberOfRows);
    result.ends.fill(0, numberOfLayouts);
    result.granulesOfListedFields.fill(PlacesOfFieldsInSlotPlan::inCell, numberOfRows);
    result.layoutsWithListedFieldsOutOfLine.ensureSize(numberOfLayouts);
    result.layoutsWithHiddenWords = findLayoutsWithHiddenWordsForSlotPlan(program, hasTypedSlots ? &link : nullptr, families);
    if (families && hasTypedSlots)
        chooseListedFieldsOutOfLineForSlotPlan(program, link, held, *families, storage, result);
    // A listed field that finds no granule lies in the cell after all. So this comes in front of whatever lays cells.
    placeAppendedFieldsForSlotPlan(program, link, mentions, families, storage, result);
    if (families) {
        placeFieldsSharedByFamiliesForSlotPlan(program, *families, storage, result);
        auto hasCells = [&](LayoutID id) {
            return storage[id.index()].storage == LayoutStorage::CompiledObject && storage[id.index()].hasCells();
        };
        auto addPlace = [&] {
            result.formsOfPlaces.append(FormOfPlaceInSlotPlan::Unclaimed);
            return static_cast<uint32_t>(result.formsOfPlaces.size() - 1);
        };
        // What lies out of line is passed over.
        Vector<uint8_t> widthsInCell;
        if (!result.layoutsWithListedFieldsOutOfLine.isEmpty()) {
            widthsInCell.fill(bytesPerSlotInSlotPlan, result.granulesOfListedFields.size());
            for (size_t index : result.layoutsWithListedFieldsOutOfLine) {
                for (uint32_t row = result.firstRowOfLayout[index]; row < result.firstRowOfLayout[index + 1]; ++row) {
                    if (result.liesOutOfLine(row))
                        widthsInCell[row] = 0;
                }
            }
        }
        placeFieldsOfUnions(program, link, *families, RowsOfSlotPlan { result.firstRowOfLayout.span(), result.offsets.mutableSpan(), result.places.mutableSpan(), result.ends.mutableSpan(), result.layoutsWithHiddenWords, widthsInCell.span() }, hasCells, addPlace, result.bytesKeptZero);
    }

    // A layout comes behind the layout above it.
    BitVector isPlaced(numberOfLayouts);
    BitVector isWaiting(numberOfLayouts);
    Vector<LayoutID, 16> waiting;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        for (LayoutID current(index); current && !isPlaced.quickGet(current.index()) && !isWaiting.quickSet(current.index()); current = writtenBaseOf(program.at(current)))
            waiting.append(current);
        while (!waiting.isEmpty()) {
            LayoutID id = waiting.takeLast();
            placeFieldsOfLayoutForSlotPlan(program, isPlaced, id, result);
            isPlaced.quickSet(id.index());
        }
    }
    return result;
}

// False: the slot holds every value, whatever is known of the field.
static bool slotMayGoByTypeOfFieldForSlotPlan(const Program& program, const Facts& facts, FieldReference reference)
{
    // Where the walk has not proven that a value fits its slot, the field is defined by name behind the allocation, and these have no such definition: keyDefinedBehindAllocationInChoices().
    // A private name is none of them: no allocation is written with one, its definition stands behind the allocation.
    constexpr OptionSet<FieldFlags> filledByAllocationAlone { FieldFlags::IsBrand, FieldFlags::IsHidden };
    // The slot of an accessor holds its functions. An appended field has no declaration: it is a plain property under a name.
    const Field* declaration = reference.field < program.at(reference.layout).fields.size() ? &fieldAt(program, declarationOf(program, reference)) : nullptr;
    if (declaration && (!declaration->form.isEmpty() || declaration->flags.containsAny(filledByAllocationAlone)))
        return false;
    FieldFacts fieldFacts = facts.field(reference);
    if (!fieldFacts.formOfValue.isEmpty() || fieldFacts.facts.contains(FieldFact::IsAccessor))
        return false;
    // Where the evaluations of the class share the layout, or the realms do, the emitter and the run time compare the brand and then look for a boxed value: PrivateAccessEmitter::findSlotsOfProvenReceiver().
    return !declaration || !declaration->flags.contains(FieldFlags::IsPrivate) || (facts.hasOneInstance() && facts.isEvaluatedAtMostOnce(declaredLayoutOf(program, declarationOf(program, reference).layout)));
}

static FormOfPlaceInSlotPlan formThatHoldsTypeForSlotPlan(const Program& program, TypeID type)
{
    constexpr uint16_t markers = Primitive::UndefinedOrNull | Primitive::Absent;
    // `abstract x: number`: nothing is ever stored under this declaration, so it asks nothing of the place that it shares.
    if (isNeverType(type))
        return FormOfPlaceInSlotPlan::Unclaimed;
    Type held = program.at(type);
    if (!(held.primitives & Primitive::Number) || !held.hasOnlyPrimitives(Primitive::Number | markers))
        return FormOfPlaceInSlotPlan::AnyValue;
    return held.primitives & Primitive::Number & ~Primitive::Integer ? FormOfPlaceInSlotPlan::Number : FormOfPlaceInSlotPlan::Integer;
}

// Null: Link has proven nothing of what the stores bring.
static TypeID typeProvenOfFieldForSlotPlan(const LinkTables& link, FieldReference reference)
{
    const LinkedCell* cell = link.findCellOfField(reference);
    return cell && cell->everyWriterIsKnown ? cell->proven : TypeID();
}

static FormOfPlaceInSlotPlan formProvenOfFieldForSlotPlan(const Program& program, const Facts& facts, const LinkTables& link, FieldReference reference)
{
    TypeID proven = typeProvenOfFieldForSlotPlan(link, reference);
    if (!proven || !slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference))
        return FormOfPlaceInSlotPlan::AnyValue;
    return formThatHoldsTypeForSlotPlan(program, proven);
}

// ---- What a slot is held to



// In the words of the run time's test, which is all that a door asks (appendTypeRecord() of image/AOTTypeTables.cpp), and of the middle: one layout or one family. Null: it says too little to be worth a test at each store.
static TypeID typeInWordsOfTestsForSlotPlan(const Facts& facts, TypeArena& types, TypeID type)
{
    if (!type || isNeverType(type) || !types.at(type).isJavaScriptValue() || types.at(type).primitives & Primitive::Unspecified)
        return { };
    TypeID tested = types.testablePartOf(types.withLiteralStringsWidened(widenedToFamily(types, facts.families(), types.withoutPrimitives(type, Primitive::Absent))));
    if (isNeverType(tested) || types.at(tested).hasUnboundedUnknown())
        return { };
    // Whoever reads a closure of one function out of a slot may mention the code of that function: LinkTables::referrers of Facts::linked() is the graph of both readings and leads there, and the held tables name only what has code.
    // Not so a built-in: what is in reach of the library is decided by what is proven alone.
    if (types.at(tested).objects && !singleFunctionOf(types, types.at(types.withOnlyObjects(tested))))
        return { };
    return tested;
}

// `Debug.assert(..)` at a thousand places and `(Debug as any)[name] = noop` at one. The block that makes the object of a namespace stores each function that is declared in it once, so hardly a store pays the test,
// and behind a filled slot each of the places enters the function.
static TypeID closuresDeclaredInContainerForSlotPlan(const Program& program, const Facts& facts, TypeArena& types, FieldReference reference)
{
    const Layout& layout = program.at(reference.layout);
    if (layout.kind != LayoutKind::Container || reference.field >= layout.fields.size() || !slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference))
        return { };
    // What stays with compiled code, with every writer known, is read without a look at its slot: no store may empty it.
    const LinkTables& linked = facts.linked();
    const LinkedCell* proven = linked.findCellOfField(reference);
    if (proven && proven->everyWriterIsKnown && (proven->proven || (reference.layout.index() < linked.layouts.size() && !linked.layout(reference.layout).reachesUnseenCode)))
        return { };
    const DeclaredType& declared = layout.fields[reference.field].declared;
    if (!declared.isExpected() || !declared.writtenFlags().contains(PositionFlags::IsFromCompiler) || !declared.written())
        return { };
    Type written = types.at(declared.written());
    if (!written.isObject() || !written.isJavaScriptValue() || !singleFunctionOf(types, written))
        return { };
    return typeInWordsOfTestsForSlotPlan(facts, types, declared.written());
}

// WHAT A SLOT IS HELD TO: FieldFacts::slotType with the markers that lie beside it. facts/AOTEnforcedFields.h says where it comes from: what is written in checked code, and nothing else.
// It is no proof, and it is made true: the record of the layout has the type (FieldData::type), and whoever stores goes by it. A value that does not fit takes the field out of its slot. Null: every value.
static TypeID typeHeldOfFieldForSlotPlan(const Program& program, const Facts& facts, TypeArena& types, FieldReference reference)
{
    if (TypeID closures = closuresDeclaredInContainerForSlotPlan(program, facts, types, reference))
        return closures;
    // The engine fills the results of its built-ins and the elements of a container by itself.
    const Layout& layout = program.at(reference.layout);
    if (layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance)
        return { };
    if (!slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference))
        return { };
    FieldFacts enforced = facts.field(reference);
    return contentsOfEnforcedSlot(types, enforced.slotType, enforced.facts.contains(FieldFact::SlotHoldsUndefinedAndNull));
}

// A STORE THROUGH A FAMILY WRITES WITHOUT ASKING WHICH MEMBER IT HAS, so its value has to fit the slot of each. `interface Node { kind: SyntaxKind }` above `interface SourceFile { kind: SyntaxKind.SourceFile }`:
// held each to its own, the two would take nothing but the one number. So what lies at one slot is held to one type, which holds what each of them is held to.
static void uniteTypesHeldInSlotPlan(const Facts& facts, TypeArena& types, std::span<TypeID*> held)
{
    // A row that takes every value leaves the place no type that a form could go by: then no row of the place is held to anything.
    TypeID united = *held[0];
    for (TypeID* type : held.subspan(1))
        united = united && *type ? joinOf(types, facts.families(), united, *type) : TypeID();
    united = typeInWordsOfTestsForSlotPlan(facts, types, united);
    for (TypeID* type : held)
        *type = united;
}

// A place has the form that holds what is proven of the field in every layout that lists it, with cells of its own or without: the initializers of an abstract class store into the objects of the classes below.
// No store into an environment is tested, and a value that does not fit has nowhere to go: an environment holds every value as it is.
static void chooseFormsOfPlacesForSlotPlan(Program& program, const Facts& facts, const LinkTables& link, const Families* families, std::span<const StorageOfLayoutInSlotPlan> storage, PlacesOfFieldsInSlotPlan& places)
{
    TypeArena types(program.types, ArenaKeeps::InProgram);
    places.typesHeld.fill(TypeID(), places.offsets.size());
    places.typesHeldOfAppendedFields.grow(program.layouts.size());
    BitVector placesWithRowThatTakesEveryValue(places.formsOfPlaces.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!isObjectLayout(program.layouts[index].kind))
            continue;
        LayoutID id(index);
        uint32_t numberDeclared = static_cast<uint32_t>(program.layouts[index].fields.size());
        for (uint32_t field = 0; field < numberDeclared; ++field) {
            uint32_t row = places.rowOf({ id, field });
            places.typesHeld[row] = typeHeldOfFieldForSlotPlan(program, facts, types, { id, field });
            if (!places.typesHeld[row])
                placesWithRowThatTakesEveryValue.quickSet(places.places[row]);
            FormOfPlaceInSlotPlan& form = places.formsOfPlaces[places.places[row]];
            if (form != FormOfPlaceInSlotPlan::AnyValue)
                form = std::max(form, formProvenOfFieldForSlotPlan(program, facts, link, { id, field }));
        }
        for (uint32_t place = 0; place < link.numberOfAppendedKeysOfPlan(id); ++place)
            places.typesHeldOfAppendedFields[index].append(typeHeldOfFieldForSlotPlan(program, facts, types, { id, numberDeclared + place }));
    }

    for (uint32_t row = 0; row < places.typesHeld.size(); ++row) {
        if (places.typesHeld[row] && placesWithRowThatTakesEveryValue.quickGet(places.places[row]))
            places.typesHeld[row] = { };
    }

    // The rows of one place, side by side. Nearly every place has one row, and nothing is united there.
    Vector<uint32_t> numbersOfRowsHeldAtPlaces;
    numbersOfRowsHeldAtPlaces.fill(0, places.formsOfPlaces.size());
    for (uint32_t row = 0; row < places.places.size(); ++row) {
        if (places.typesHeld[row])
            ++numbersOfRowsHeldAtPlaces[places.places[row]];
    }
    Vector<uint32_t> rowsByPlace;
    for (uint32_t row = 0; row < places.places.size(); ++row) {
        if (places.typesHeld[row] && numbersOfRowsHeldAtPlaces[places.places[row]] > 1)
            rowsByPlace.append(row);
    }
    AOT::sort(rowsByPlace, [&](uint32_t a, uint32_t b) {
        return std::pair { places.places[a], a } < std::pair { places.places[b], b };
    });
    Vector<TypeID*, 16> held;
    for (size_t first = 0; first < rowsByPlace.size();) {
        held.shrink(0);
        size_t end = first;
        for (; end < rowsByPlace.size() && places.places[rowsByPlace[end]] == places.places[rowsByPlace[first]]; ++end)
            held.append(&places.typesHeld[rowsByPlace[end]]);
        if (held.size() > 1)
            uniteTypesHeldInSlotPlan(facts, types, held.mutableSpan());
        first = end;
    }

    // An appended field has no place. The smallest family first, so that what a larger one unites holds what a smaller one has united.
    Vector<LayoutID, 16> members;
    BitVector familiesThatMayAgree = families ? findFamiliesThatMayAgreeOnSlotsForSlotPlan(program, *families, storage) : BitVector();
    for (uint32_t number = 0; families && number < families->size(); ++number) {
        findMembersThatAgreeOnSlotsForSlotPlan(program, *families, familiesThatMayAgree, FamilyID(number), storage, members);
        if (members.isEmpty())
            continue;
        for (PropertyKeyID key : link.appendedKeysOfPlan(members[0])) {
            held.shrink(0);
            std::optional<uint8_t> granule;
            for (LayoutID member : members) {
                Vector<PropertyKeyID, 16> keys = link.appendedKeysOfPlan(member);
                size_t place = std::ranges::find(keys, key) - keys.begin();
                if (place == keys.size() || granule.value_or(places.granulesOfAppendedFields[member.index()][place]) != places.granulesOfAppendedFields[member.index()][place])
                    break;
                granule = places.granulesOfAppendedFields[member.index()][place];
                held.append(&places.typesHeldOfAppendedFields[member.index()][place]);
            }
            if (held.size() == members.size() && *granule != PlacesOfFieldsInSlotPlan::inCell)
                uniteTypesHeldInSlotPlan(facts, types, held.mutableSpan());
        }
    }
}

// The plan hears of Link and of what is withheld through Facts alone: FactsRequest::mechanisms.
static bool hasTypedSlotsInSlotPlan(const Facts& facts)
{
    return withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedSlots);
}

// `held`: typeHeldOfFieldForSlotPlan(). A place that linked() proves numbers may be written too: there the slot is held to all that its form has, which holds what is proven.
static void giveSlotFormOfPlaceInSlotPlan(PlacedSlot& slot, FormOfPlaceInSlotPlan form, TypeID held)
{
    switch (form) {
    case FormOfPlaceInSlotPlan::Unclaimed:
    case FormOfPlaceInSlotPlan::AnyValue:
        slot.type = held;
        break;
    case FormOfPlaceInSlotPlan::Integer:
        slot.representation = SlotRepresentation::OptionalInt64;
        slot.type = typeOfIntegerSlotInSlotPlan;
        break;
    case FormOfPlaceInSlotPlan::Number:
        slot.representation = SlotRepresentation::OptionalDouble;
        slot.type = typeOfNumberSlotInSlotPlan;
        break;
    }
}

static PlannedLayout planLayoutForSlotPlan(const Program& program, const Facts& facts, const MentionsOfKeys& mentions, const PlacesOfFieldsInSlotPlan& places, StorageOfLayoutInSlotPlan storage, LayoutID id, BuildContext& build)
{
    const Layout& layout = program.at(id);
    PlannedLayout planned;
    planned.storage = storage.storage;
    planned.isRecordAlone = storage.isRecordAlone;
    if (!planned.hasSlots())
        return planned;

    bool isCompiledObject = planned.storage == LayoutStorage::CompiledObject;
    if (isCompiledObject && storage.hasCells())
        planned.appendedKeys = Vector<PropertyKeyID>(facts.linked().appendedKeysOfPlan(id).span());
    planned.indexAppendedKeys();
    uint32_t numberOfDeclaredFields = static_cast<uint32_t>(layout.fields.size());
    uint32_t numberOfFields = numberOfDeclaredFields + static_cast<uint32_t>(planned.appendedKeys.size());
    if (isCompiledObject && numberOfFields > maxFieldsInOrder) {
        build.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id, .limit = LimitID::FieldsInKeyOrder, .count = numberOfFields });
        return { };
    }

    bool hasTypedSlots = hasTypedSlotsInSlotPlan(facts);
    uint32_t endOfExtension = offsetOfFirstExtensionSlot;
    planned.slots.grow(numberOfFields + numberOfHiddenWords);
    for (FieldIndex field = 0; field < numberOfDeclaredFields; ++field) {
        PlacedSlot& slot = planned.slots[field];
        // `interface Settings { stored: number; neverStored?: string }`: nothing in the program stores under the name and no reached code spells it. The record would hold the name, and it could be read out of the executable.
        // Whoever brings the name at run time adds a property, as under any name that the layout does not declare. The slot stays where it is, for ever empty.
        // `type Seven = { a?: number; .. g?: number }  made[name] = 1`, with a name out of a list of the program: the store finds the slot by the record. So a name that the program spells as a text is spelled.
        const Field& declared = layout.fields[field];
        bool isSpelledByTypeAlone = declared.key && declared.flags.contains(FieldFlags::IsNeverWritten) && !mentions.isSpelled(declared.key);
        uint32_t row = places.rowOf({ id, field });
        bool hasNoSlot = places.granulesOfListedFields[row] == PlacesOfFieldsInSlotPlan::withoutSlot;
        if (isCompiledObject && !isSpelledByTypeAlone && !hasNoSlot)
            slot.indexInLayoutRecord = planned.numberOfRecordFields++;
        if (!storage.hasCells() || hasNoSlot)
            continue;
        slot.holdsAccessor = fieldAt(program, declarationOf(program, { id, field })).form.contains(FieldForm::IsAccessor) || facts.field({ id, field }).facts.contains(FieldFact::IsAccessor);
        if (places.liesOutOfLine(row)) {
            RELEASE_ASSERT(isCompiledObject && places.granulesOfListedFields[row] < maxExtensionGranules);
            slot.storage = SlotStorage::OutOfLine;
            slot.offset = offsetOfFirstExtensionSlot + places.granulesOfListedFields[row] * extensionGranuleSize;
            slot.type = hasTypedSlots ? places.typesHeld[row] : TypeID();
            // `this.exports = undefined`: the key is of the birth. What no maker gives has its key from the store that brings its granule.
            slot.mayExistWithoutStorage = !places.rowsGivenByNoMaker.get(row);
            slot.isLeftOutByMakers = true;
            endOfExtension = std::max(endOfExtension, slot.end());
            continue;
        }
        slot.storage = SlotStorage::Inline;
        slot.offset = static_cast<uint32_t>(places.offsets[row]);
        giveSlotFormOfPlaceInSlotPlan(slot, places.formsOfPlaces[places.places[row]], isCompiledObject && hasTypedSlots ? places.typesHeld[row] : TypeID());
    }

    uint64_t end = places.ends[id.index()];
    // A cell is as large as its size class. The word that is left over costs nothing, and a granule of the Extension costs its eight bytes, the Extension where it is the only one, and a read that counts bits.
    uint64_t endOfSlotsInCell = end;
    for (FieldIndex field = numberOfDeclaredFields; field < numberOfFields; ++field)
        endOfSlotsInCell += places.granulesOfAppendedFields[id.index()][field - numberOfDeclaredFields] == PlacesOfFieldsInSlotPlan::inCell ? bytesPerSlotInSlotPlan : 0;
    bool hasWordLeftOver = isCompiledObject && storage.hasCells() && !build.recordedOptions().useWideReferences()
        && WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(endOfSlotsInCell) - endOfSlotsInCell >= bytesPerSlotInSlotPlan;
    for (FieldIndex field = numberOfDeclaredFields; field < numberOfFields; ++field) {
        PlacedSlot& slot = planned.slots[field];
        slot.indexInLayoutRecord = planned.numberOfRecordFields++;
        slot.holdsAccessor = facts.field({ id, field }).facts.contains(FieldFact::IsAccessor);
        uint8_t granule = places.granulesOfAppendedFields[id.index()][field - numberOfDeclaredFields];
        if (hasWordLeftOver && facts.linked().isAppendedFieldNamedMostOften({ id, field }) && !places.appendedFieldsThatShareGranule[id.index()].get(field - numberOfDeclaredFields))
            granule = PlacesOfFieldsInSlotPlan::inCell;
        TypeID held = hasTypedSlots ? places.typesHeldOfAppendedFields[id.index()][field - numberOfDeclaredFields] : TypeID();
        if (granule == PlacesOfFieldsInSlotPlan::inCell) {
            slot.storage = SlotStorage::Inline;
            slot.offset = static_cast<uint32_t>(end);
            end += bytesPerSlotInSlotPlan;
            if (hasTypedSlots)
                giveSlotFormOfPlaceInSlotPlan(slot, formProvenOfFieldForSlotPlan(program, facts, facts.linked(), { id, field }), held);
            continue;
        }
        RELEASE_ASSERT(granule < maxExtensionGranules);
        slot.storage = SlotStorage::OutOfLine;
        slot.offset = offsetOfFirstExtensionSlot + granule * extensionGranuleSize;
        slot.type = held;
        // `node.jsDoc = undefined` in the maker of every node gives the objects one shape and fills nothing. The key order says that the property is there.
        // No store of compiled code creates a field with a form, and a definition asks whether the object has the property by a look at its slot.
        slot.mayExistWithoutStorage = !facts.linked().isAppendedFieldRequiredBySomeWrittenType({ id, field }) && facts.field({ id, field }).formOfValue.isEmpty();
        endOfExtension = std::max(endOfExtension, slot.end());
    }
    if (!storage.hasCells())
        return planned;

    RELEASE_ASSERT(endOfExtension == offsetOfFirstExtensionSlot || places.layoutsWithHiddenWords.quickGet(id.index()));
    if (isCompiledObject && places.layoutsWithHiddenWords.quickGet(id.index())) {
        // The stubs read a hidden word in the width that the options of the build say, whatever the layout.
        SlotRepresentation hiddenReference = hiddenReferenceRepresentation(build.recordedOptions().useWideReferences());
        PlacedSlot& extraProperties = planned.slots[planned.extraPropertiesWordIndex()];
        extraProperties.representation = hiddenReference;
        extraProperties.storage = SlotStorage::Inline;
        extraProperties.offset = offsetOfFirstField;
        if (endOfExtension > offsetOfFirstExtensionSlot) {
            planned.extensionSize = endOfExtension;
            PlacedSlot& extension = planned.slots[planned.extensionWordIndex()];
            extension.representation = hiddenReference;
            extension.storage = SlotStorage::Inline;
            extension.offset = offsetOfFirstField;
            if (2 * widthOf(hiddenReference) <= bytesPerSlotInSlotPlan)
                extraProperties.offset = offsetOfFirstField + widthOf(hiddenReference);
            else {
                extraProperties.offset = static_cast<uint32_t>(end);
                end += bytesPerSlotInSlotPlan;
            }
        }
    }

    if (id.index() < places.bytesKeptZero.size())
        planned.bytesKeptZero = places.bytesKeptZero[id.index()];
    uint64_t size = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(end);
    if (size > build.limit(LimitID::MaxFieldOffset)) {
        build.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id, .limit = LimitID::MaxFieldOffset, .count = clampTo<uint32_t>(size) });
        return { };
    }
    planned.size = static_cast<uint32_t>(size);
    return planned;
}

// AOTNamePlan.cpp.
MentionsOfKeys countMentionsOfKeys(const Program&, const Facts&, const ParallelLoops&);
void planNames(const Program&, const MentionsOfKeys&, BuildContext&, SlotPlanParts&);

// planProgram() of AOTPlan.cpp calls it, declares it for itself, and fills `variables`.
// An error is reported to the build, and the layout it is about is left without slots.
SlotPlanParts computeSlotPlanParts(Program&, const Facts&, BuildContext&);
// A NUMBER THAT IS LEFT IN A BOXED SLOT IS HELD TO NOTHING, WHATEVER FACTS SAY OF THE FIELD. Without a type a guess tests `i32` or `number`, one compare, and unboxes an int32 or a double. With undefined|null|i32 the two markers drag the
// operation into floating point: the value is unboxed into an optional double, whose markers each operation decodes: `value * this.step`: +6 instructions a turn for an enum, +7 for a number
// (whole/hoist/programs/q/p/p13-method.ts x1.189, p4-callback.ts x1.031). And every store by name pays the test of the type. One freeze (11-04 01:10:13), int32 held | not: the TypeScript-compiler benchmark 36.34 | 36.24 G.
// BEHIND WHOEVER CHOOSES RAW FORMS: he reads the type of a slot that is still boxed, and in a raw form the optional value is a plain load. Booleans, texts and references gain boxed too.
static void dropTypesOfNumbersLeftBoxedInSlotPlan(const Program& program, SlotPlanParts& parts)
{
    for (PlannedLayout& layout : parts.layouts) {
        for (PlacedSlot& slot : layout.slots) {
            if (slot.type && slot.representation == SlotRepresentation::JSValue && program.types.at(slot.type).isNumberOrNullish())
                slot.type = { };
        }
    }
}

SlotPlanParts computeSlotPlanParts(Program& program, const Facts& facts, BuildContext& build)
{
    unsigned numberOfLayouts = static_cast<unsigned>(program.layouts.size());
    SlotPlanParts parts;
    parts.layoutsNeverAllocated = findLayoutsNeverAllocatedForSlotPlan(program, facts.linked());
    for (const TaggedMaker& maker : facts.linked().taggedMakers) {
        for (const TaggedLayout& member : program.taggedFamilies[maker.family].members) {
            if (member.layout.index() < facts.linked().layouts.size() && facts.linked().layout(member.layout).isAllocated)
                parts.layoutsNeverAllocated.quickClear(member.layout.index());
        }
    }

    Vector<StorageOfLayoutInSlotPlan> storage(numberOfLayouts, [&](size_t index) {
        return chooseStorageOfLayoutForSlotPlan(program.layouts[index], parts.layoutsNeverAllocated.quickGet(index));
    });
    bool hasTypedOperations = withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedOperations);
    MentionsOfKeys mentions = countMentionsOfKeys(program, facts, build.parallelLoops());
    PlacesOfFieldsInSlotPlan places = placeFieldsForSlotPlan(program, facts.linked(), facts.held(), mentions, hasTypedOperations ? &facts.families() : nullptr, storage.span(), hasTypedSlotsInSlotPlan(facts));
    if (hasTypedSlotsInSlotPlan(facts))
        chooseFormsOfPlacesForSlotPlan(program, facts, facts.linked(), hasTypedOperations ? &facts.families() : nullptr, storage.span(), places);

    parts.layouts.grow(numberOfLayouts);
    forEachIndexInParallel(build.parallelLoops(), numberOfLayouts, [&](unsigned index) {
        parts.layouts[index] = planLayoutForSlotPlan(program, facts, mentions, places, storage[index], LayoutID(index), build);
    });
    planEnvironmentsHandedBack(program, facts, build, parts);
    chooseStorageOfFields(program, facts, build, parts);
    dropTypesOfNumbersLeftBoxedInSlotPlan(program, parts);
    uint64_t numberOfFieldsOutOfLine = 0;
    for (const PlannedLayout& planned : parts.layouts) {
        for (const PlacedSlot& slot : planned.slots)
            numberOfFieldsOutOfLine += slot.isOutOfLine();
    }
    build.statistics().count(StatisticID::FieldsOutOfLine, numberOfFieldsOutOfLine);
    planNames(program, mentions, build, parts);
    return parts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
