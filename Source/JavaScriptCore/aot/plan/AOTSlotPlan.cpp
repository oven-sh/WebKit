/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
#include "AOTLayoutData.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTTaggedLayouts.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
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
#include <wtf/HashMap.h>
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

static void meetWithSlotOfMemberInSlotPlan(PlacedSlot& merged, const PlacedSlot& placed, bool isStore)
{
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
    if (merged.provenType != placed.provenType)
        merged.provenType = { };
    merged.everyStoreFits &= placed.everyStoreFits;
    if (merged.typeOfRawValue != placed.typeOfRawValue)
        merged.typeOfRawValue = { };
}

FieldAccess SlotPlan::fieldAccess(const Program& program, PropertyKeyID key, LayoutSetID set, std::span<const LayoutID> members, bool isStore) const
{
    if (!m_fieldAccessesOfSets || !set || holdsOneMember(set) || liesInThread(key))
        return fieldAccess(program, key, members, isStore);
    if (liesInThread(set)) {
        static constexpr size_t fewestMembersWhoseAnswerIsKept = 8;
        if (members.size() < fewestMembersWhoseAnswerIsKept)
            return fieldAccess(program, key, members, isStore);
        uint64_t hash = key.bits();
        for (LayoutID member : members)
            hash = (hash ^ member.index()) * 0x9e3779b97f4a7c15ull;
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

FieldAccess SlotPlan::fieldAccessOfLayouts(const Program& program, PropertyKeyID key, std::span<const LayoutID> receivers, bool isStore) const
{
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
    bool coversEveryMember = true;
    for (LayoutID member : receivers) {
        if (isNeverAllocated(member))
            continue;
        const PlacedSlot* placed = nullptr;
        if (holdsCompiledObjects(member)) {
            if (std::optional<FieldIndex> field = findField(program, member, key))
                placed = &slot(member, *field);
        }
        if (!placed || !placed->hasStorage()) {
            coversEveryMember = false;
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
    bool everySlotLiesInCellInOneForm = first.representation != SlotRepresentation::DeferredClosure;
    for (size_t index = 0; index < slotsOfMembers.size(); ++index) {
        const PlacedSlot& placed = *slotsOfMembers[index];
        hasOneLocation &= placed.hasLocationOf(first) && extensionWordOfMember(index).hasLocationOf(extensionWordOfMember(0));
        everySlotLiesInCellInOneForm &= placed.storage == SlotStorage::Inline && placed.representation == first.representation;
        if (placed.offset < slotsOfMembers[lowest]->offset)
            lowest = index;
    }

    access.coverage = coversEveryMember ? FieldAccess::Coverage::EveryMember : FieldAccess::Coverage::SomeMembers;
    bool everyObjectAskedHasSlot = !isStore || coversEveryMember;
    access.findsOffsetByShape = !hasOneLocation && everySlotLiesInCellInOneForm && everyObjectAskedHasSlot;
    access.hasLocation = access.findsOffsetByShape || (hasOneLocation && everyObjectAskedHasSlot);

    if (access.hasLocation) {
        access.slot = *slotsOfMembers[lowest];
        access.extensionWord = extensionWordOfMember(lowest);
    }
    for (const PlacedSlot* placed : slotsOfMembers) {
        access.mayBeFilledWithoutStorage |= placed->mayExistWithoutStorage;
        if (access.hasLocation)
            meetWithSlotOfMemberInSlotPlan(access.slot, *placed, isStore);
    }
    if (access.membersWithField.size() > 1) {
        access.slot.deferredFunction = { };
        access.slot.indexInLayoutRecord = std::nullopt;
    }
    return access;
}

static Type typeHeldInSlotOfSlotPlan(const Program& program, const PlacedSlot& placed)
{
    return program.at(placed.typeInLayoutRecord() ? placed.typeInLayoutRecord() : TypeID(TypeTable::anything));
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

namespace {

enum class FormOfPlaceInSlotPlan : uint8_t {
    Unclaimed,
    Integer,
    Number,
    AnyValue,
};

struct PlacesOfFieldsInSlotPlan {
    static constexpr uint32_t noPlace = UINT32_MAX;

    uint32_t rowOf(FieldReference field) const { return firstRowOfLayout[field.layout.index()] + field.field; }
    uint64_t offsetOfFirstSlotOf(LayoutID id) const { return offsetOfFirstField + (layoutsWithHiddenWords.quickGet(id.index()) ? bytesPerSlotInSlotPlan : 0); }

    Vector<uint32_t> firstRowOfLayout;
    Vector<uint64_t> offsets;
    Vector<uint32_t> places;
    Vector<FormOfPlaceInSlotPlan> formsOfPlaces;
    Vector<TypeID> typesHeld;
    Vector<Vector<TypeID>> typesHeldOfAppendedFields;
    Vector<uint64_t> ends;
    static constexpr uint8_t inCell = UINT8_MAX;
    Vector<BitVector> appendedFieldsThatShareGranule;
    Vector<Vector<uint8_t>> granulesOfAppendedFields;
    static constexpr uint8_t granuleNotChosen = inCell - 1;
    static constexpr uint8_t withoutSlot = inCell - 2;
    Vector<uint8_t> granulesOfListedFields;
    BitVector rowsGivenByNoMaker;
    bool liesOutOfLine(uint32_t row) const { return granulesOfListedFields[row] != inCell; }
    BitVector layoutsWithListedFieldsOutOfLine;
    BitVector layoutsWithHiddenWords;
};

struct StorageOfLayoutInSlotPlan {
    bool hasCells() const { return (storage == LayoutStorage::CompiledObject && !isRecordAlone) || storage == LayoutStorage::Environment; }

    LayoutStorage storage { LayoutStorage::None };
    bool isRecordAlone { false };
};

} // anonymous namespace

static BitVector findLayoutsNeverAllocatedForSlotPlan(const Program& program, const LinkTables& linked)
{
    BitVector layoutsNeverAllocated(program.layouts.size());
    for (size_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].has(LayoutFlags::IsNeverAllocated) || (index < linked.layouts.size() && !linked.layouts[index].isAllocated))
            layoutsNeverAllocated.quickSet(index);
    }
    for (const TaggedFamily& family : program.taggedFamilies) {
        for (const TaggedLayout& member : family.members)
            layoutsNeverAllocated.quickSet(member.layout.index());
    }
    return layoutsNeverAllocated;
}

static bool declaresPrivateNameForSlotPlan(const Layout& layout)
{
    constexpr OptionSet<FieldFlags> flagsOfPrivateNames { FieldFlags::IsPrivate, FieldFlags::IsBrand };
    return std::ranges::any_of(layout.fields, [&](const Field& field) {
        return field.flags.containsAny(flagsOfPrivateNames) && !field.flags.contains(FieldFlags::IsInherited);
    });
}

static StorageOfLayoutInSlotPlan chooseStorageOfLayoutForSlotPlan(const Layout& layout, bool isNeverAllocated)
{
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
    if (above.layout == reference.layout || !isPlaced.quickGet(above.layout.index()) || !isObjectLayout(program.at(above.layout).kind))
        return std::nullopt;
    return above;
}

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
    if (top != reference.layout)
        return false;
    bool isWritten = false;
    for (LayoutID written : layout.supertypes) {
        std::optional<uint32_t> listed = program.at(written).findField(field.key);
        if (!listed)
            continue;
        const Field& asWritten = program.at(written).fields[*listed];
        if (asWritten.flags.contains(FieldFlags::IsRequired) && !(asWritten.declared.written() && program.at(asWritten.declared.written()).primitives & Primitive::Undefined && !asWritten.declared.has(PositionFlags::OmitsUndefined)))
            return false;
        isWritten = true;
    }
    return isWritten || !field.flags.contains(FieldFlags::IsRequired);
}

static bool needsHiddenWordInSlotPlan(const Program& program, const LinkTables& link, const BitVector& layoutsWithSlotsThatStoresMayEmpty, LayoutID id)
{
    constexpr OptionSet<LayoutChange> changesThatMakeExtraProperties { LayoutChange::GainsProperty, LayoutChange::GetsAccessor };
    LayoutKind kind = program.at(id).kind;
    if (kind != LayoutKind::Record && kind != LayoutKind::Instance)
        return true;
    if (id.index() >= link.layouts.size() || link.layout(id).possibleChanges.containsAny(changesThatMakeExtraProperties) || layoutsWithSlotsThatStoresMayEmpty.get(id.index()))
        return true;
    return link.numberOfAppendedKeysOfPlan(id);
}

static BitVector findLayoutsWithHiddenWordsForSlotPlan(const Program& program, const LinkTables* link, const BitVector& layoutsWithSlotsThatStoresMayEmpty, const Families* families)
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
        if (isObject(index) && (!link || needsHiddenWordInSlotPlan(program, *link, layoutsWithSlotsThatStoresMayEmpty, LayoutID(index))))
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
        if (storage[index].storage != LayoutStorage::CompiledObject || !storage[index].hasCells() || index >= held.layouts.size() || !result.layoutsWithHiddenWords.quickGet(index))
            continue;
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

static bool mayGainPropertiesInSlotPlan(const LinkTables& link, LayoutID id)
{
    return id.index() < link.layouts.size() && link.layout(id).possibleChanges.contains(LayoutChange::GainsProperty);
}

static bool mayBeNoFieldInSlotPlan(const LinkTables& link, FieldReference reference)
{
    if (link.layout(reference.layout).possibleChanges.contains(LayoutChange::GainsProperty))
        return true;
    const LinkedCell& cell = link.field(reference);
    return cell.everyWriterIsKnown && !cell.isWrittenAfterConstruction;
}

static void placeAppendedFieldsForSlotPlan(const Program& program, const LinkTables& link, const Families* families, std::span<const StorageOfLayoutInSlotPlan> storage, PlacesOfFieldsInSlotPlan& result)
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
        uint32_t numberListed = static_cast<uint32_t>(program.layouts[index].fields.size());
        for (uint32_t place = 0; place < link.numberOfAppendedKeysOfPlan(LayoutID(index)); ++place)
            result.granulesOfAppendedFields[index].append(link.isAppendedFieldRequiredByTypeOfThis({ LayoutID(index), numberListed + place }) ? PlacesOfFieldsInSlotPlan::inCell : noGranule);
    }

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
        rowsWaiting.shrink(0);
        for (uint32_t row = result.firstRowOfLayout[index]; hasListedFieldsOutOfLine && row < result.firstRowOfLayout[index + 1]; ++row) {
            if (result.granulesOfListedFields[row] == PlacesOfFieldsInSlotPlan::granuleNotChosen)
                rowsWaiting.append(row);
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
            if (granule != noGranule)
                continue;
            if (numbersOfGranulesTaken[index] < maxExtensionGranules)
                granule = numbersOfGranulesTaken[index]++;
            else
                granule = mayGainPropertiesInSlotPlan(link, LayoutID(index)) ? PlacesOfFieldsInSlotPlan::withoutSlot : PlacesOfFieldsInSlotPlan::inCell;
        }
    };
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        numberGranulesOf(numberGranulesOf, index);
}

static void placeFieldsOfLayoutForSlotPlan(const Program& program, const BitVector& isPlaced, LayoutID id, PlacesOfFieldsInSlotPlan& result)
{
    const Layout& layout = program.at(id);
    bool isObject = isObjectLayout(layout.kind);
    LayoutID base = isObject ? writtenBaseOf(layout) : LayoutID();
    if (base && (!isPlaced.quickGet(base.index()) || !isObjectLayout(program.at(base).kind)))
        base = { };

    uint64_t end = isObject ? std::max(result.ends[id.index()], result.offsetOfFirstSlotOf(id)) : offsetOfFirstEnvironmentVariable;
    if (base)
        end = result.ends[base.index()];
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

static PlacesOfFieldsInSlotPlan placeFieldsForSlotPlan(const Program& program, const LinkTables& link, const LinkTables& held, const BitVector& layoutsWithSlotsThatStoresMayEmpty, const Families* families, std::span<const StorageOfLayoutInSlotPlan> storage, bool hasTypedSlots)
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
    result.layoutsWithHiddenWords = findLayoutsWithHiddenWordsForSlotPlan(program, hasTypedSlots ? &link : nullptr, layoutsWithSlotsThatStoresMayEmpty, families);
    if (families && hasTypedSlots)
        chooseListedFieldsOutOfLineForSlotPlan(program, link, held, *families, storage, result);
    placeAppendedFieldsForSlotPlan(program, link, families, storage, result);
    if (families) {
        placeFieldsSharedByFamiliesForSlotPlan(program, *families, storage, result);
        auto hasCells = [&](LayoutID id) {
            return storage[id.index()].storage == LayoutStorage::CompiledObject && storage[id.index()].hasCells();
        };
        auto addPlace = [&] {
            result.formsOfPlaces.append(FormOfPlaceInSlotPlan::Unclaimed);
            return static_cast<uint32_t>(result.formsOfPlaces.size() - 1);
        };
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
        placeFieldsOfUnions(program, link, *families, RowsOfSlotPlan { result.firstRowOfLayout.span(), result.offsets.mutableSpan(), result.places.mutableSpan(), result.ends.mutableSpan(), result.layoutsWithHiddenWords, widthsInCell.span() }, hasCells, addPlace);
    }

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

static bool slotMayGoByTypeOfFieldForSlotPlan(const Program& program, const Facts& facts, FieldReference reference)
{
    constexpr OptionSet<FieldFlags> filledByAllocationAlone { FieldFlags::IsBrand, FieldFlags::IsHidden };
    const Field* declaration = reference.field < program.at(reference.layout).fields.size() ? &fieldAt(program, declarationOf(program, reference)) : nullptr;
    if (declaration && (!declaration->form.isEmpty() || declaration->flags.containsAny(filledByAllocationAlone)))
        return false;
    FieldFacts fieldFacts = facts.field(reference);
    if (!fieldFacts.formOfValue.isEmpty() || fieldFacts.facts.contains(FieldFact::IsAccessor))
        return false;
    return !declaration || !declaration->flags.contains(FieldFlags::IsPrivate) || (facts.hasOneInstance() && facts.isEvaluatedAtMostOnce(declaredLayoutOf(program, declarationOf(program, reference).layout)));
}

static FormOfPlaceInSlotPlan formThatHoldsTypeForSlotPlan(const Program& program, TypeID type)
{
    constexpr uint16_t markers = Primitive::UndefinedOrNull | Primitive::Absent;
    if (isNeverType(type))
        return FormOfPlaceInSlotPlan::Unclaimed;
    Type held = program.at(type);
    if (!(held.primitives & Primitive::Number) || !held.hasOnlyPrimitives(Primitive::Number | markers))
        return FormOfPlaceInSlotPlan::AnyValue;
    return held.primitives & Primitive::Number & ~Primitive::Integer ? FormOfPlaceInSlotPlan::Number : FormOfPlaceInSlotPlan::Integer;
}

static FormOfPlaceInSlotPlan formProvenOfFieldForSlotPlan(const Program& program, const Facts& facts, const LinkTables& link, FieldReference reference)
{
    TypeID proven = link.provenOfField(reference);
    if (!proven || !slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference))
        return FormOfPlaceInSlotPlan::AnyValue;
    return formThatHoldsTypeForSlotPlan(program, proven);
}



static TypeID typeInWordsOfTestsForSlotPlan(const Facts& facts, TypeArena& types, TypeID type, bool isProven = false)
{
    if (!type || isNeverType(type) || !types.at(type).isJavaScriptValue() || types.at(type).primitives & Primitive::Unspecified)
        return { };
    TypeID inFamilies = widenedToFamily(types, facts.families(), types.withoutPrimitives(type, Primitive::Absent));
    TypeID tested = types.testablePartOf(isProven ? inFamilies : types.withLiteralStringsWidened(inFamilies));
    if (isNeverType(tested) || types.at(tested).hasUnboundedUnknown())
        return { };
    if (!isProven && types.at(tested).objects && !singleFunctionOf(types, types.at(types.withOnlyObjects(tested))))
        return { };
    return tested;
}

static TypeID closuresDeclaredInContainerForSlotPlan(const Program& program, const Facts& facts, TypeArena& types, FieldReference reference)
{
    const Layout& layout = program.at(reference.layout);
    if (layout.kind != LayoutKind::Container || reference.field >= layout.fields.size() || !slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference))
        return { };
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

static TypeID typeHeldOfFieldForSlotPlan(const Program& program, const Facts& facts, TypeArena& types, FieldReference reference)
{
    if (TypeID closures = closuresDeclaredInContainerForSlotPlan(program, facts, types, reference))
        return closures;
    const Layout& layout = program.at(reference.layout);
    if (layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance)
        return { };
    if (!slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference))
        return { };
    FieldFacts enforced = facts.field(reference);
    return contentsOfEnforcedSlot(types, enforced.slotType, enforced.facts.contains(FieldFact::SlotHoldsUndefinedAndNull));
}

static bool someStoreMayEmptySlotInSlotPlan(const Program& program, const Facts& facts, FieldReference reference)
{
    TypeID held = facts.field(reference).slotType;
    if (!held || !slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference))
        return false;
    TypeID proven = facts.linked().provenOfField(reference);
    if (!proven)
        return true;
    bool placeGetsFormThatHoldsAllThatIsProven = formThatHoldsTypeForSlotPlan(program, proven) != FormOfPlaceInSlotPlan::AnyValue;
    if (placeGetsFormThatHoldsAllThatIsProven)
        return false;
    constexpr uint16_t markersThatEveryHeldSlotTakes = Primitive::UndefinedOrNull | Primitive::Absent;
    Type stored = program.at(proven);
    stored.primitives &= ~markersThatEveryHeldSlotTakes;
    return !isSubset(TypeReader(program), stored, program.at(held));
}

static BitVector findLayoutsWithSlotsThatStoresMayEmptyForSlotPlan(const Program& program, const Facts& facts)
{
    BitVector result(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance)
            continue;
        for (uint32_t field = 0; field < layout.fields.size() && !result.quickGet(index); ++field) {
            if (someStoreMayEmptySlotInSlotPlan(program, facts, { LayoutID(index), field }))
                result.quickSet(index);
        }
    }
    return result;
}

static void uniteTypesHeldInSlotPlan(const Facts& facts, TypeArena& types, std::span<TypeID*> held)
{
    TypeID united = *held[0];
    for (TypeID* type : held.subspan(1))
        united = united && *type ? joinOf(types, facts.families(), united, *type) : TypeID();
    united = typeInWordsOfTestsForSlotPlan(facts, types, united);
    for (TypeID* type : held)
        *type = united;
}

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

bool fieldStaysInSlot(const Program& program, const PlacedSlot& slot)
{
    bool takesEveryValue = slot.representation == SlotRepresentation::JSValue && (!slot.type || program.at(slot.type).isAnything());
    return takesEveryValue || (slot.type && slot.everyStoreFits);
}

Vector<FieldIndex, 8> fieldsHeldFilledInSlots(const Program& program, const LinkTables& linked, const SlotPlan& slots, LayoutID id)
{
    Vector<FieldIndex, 8> fields;
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Record || !slots.layout(id).hasSlots())
        return fields;
    for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
        const LinkedCell* cell = linked.findCellOfField({ id, field });
        if (!cell || !cell->isFilled)
            continue;
        const PlacedSlot& slot = slots.slot(id, field);
        if (slot.hasStorage() && !slot.holdsAccessor && !(slot.isOutOfLine() && slot.mayExistWithoutStorage) && fieldStaysInSlot(program, slot))
            fields.append(field);
    }
    return fields;
}

bool constantListsFieldsHeldFilledInSlots(const Program& program, const LinkTables& linked, const SlotPlan& slots, DataID data)
{
    Vector<const DataNode*, 16> nodesToVisit { &program.at(data).root };
    while (!nodesToVisit.isEmpty()) {
        const DataNode& node = *nodesToVisit.takeLast();
        if (const auto* array = node.tryAs<DataNode::Array>()) {
            for (const DataNode& element : array->elements)
                nodesToVisit.append(&element);
            continue;
        }
        const auto* object = node.tryAs<DataNode::Object>();
        if (!object)
            continue;
        if (Vector<FieldIndex, 8> held = object->layout ? fieldsHeldFilledInSlots(program, linked, slots, object->layout) : Vector<FieldIndex, 8>(); !held.isEmpty()) {
            HashSet<uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> listed;
            for (const DataMember& member : object->members)
                listed.add(member.key.bits());
            for (FieldIndex field : held) {
                if (!listed.contains(program.at(object->layout).fields[field].key.bits()))
                    return false;
            }
        }
        for (const DataMember& member : object->members)
            nodesToVisit.append(&member.value);
    }
    return true;
}

static bool hasTypedSlotsInSlotPlan(const Facts& facts)
{
    return withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedSlots);
}

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
    uint64_t endOfSlotsInCell = end;
    for (FieldIndex field = numberOfDeclaredFields; field < numberOfFields; ++field)
        endOfSlotsInCell += places.granulesOfAppendedFields[id.index()][field - numberOfDeclaredFields] == PlacesOfFieldsInSlotPlan::inCell ? bytesPerSlotInSlotPlan : 0;
    bool hasWordLeftOver = isCompiledObject && storage.hasCells() && !build.recordedOptions().useWideReferences()
        && WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(endOfSlotsInCell) - endOfSlotsInCell >= bytesPerSlotInSlotPlan;
    for (FieldIndex field = numberOfDeclaredFields; field < numberOfFields; ++field) {
        PlacedSlot& slot = planned.slots[field];
        uint8_t granule = places.granulesOfAppendedFields[id.index()][field - numberOfDeclaredFields];
        if (granule == PlacesOfFieldsInSlotPlan::withoutSlot)
            continue;
        slot.indexInLayoutRecord = planned.numberOfRecordFields++;
        slot.holdsAccessor = facts.field({ id, field }).facts.contains(FieldFact::IsAccessor);
        if (hasWordLeftOver && facts.linked().takesWordLeftInCell({ id, field }) && !places.appendedFieldsThatShareGranule[id.index()].get(field - numberOfDeclaredFields))
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
        slot.mayExistWithoutStorage = !facts.linked().isAppendedFieldRequiredBySomeWrittenType({ id, field }) && facts.field({ id, field }).formOfValue.isEmpty();
        endOfExtension = std::max(endOfExtension, slot.end());
    }
    if (!storage.hasCells())
        return planned;

    RELEASE_ASSERT(endOfExtension == offsetOfFirstExtensionSlot || places.layoutsWithHiddenWords.quickGet(id.index()));
    if (isCompiledObject && places.layoutsWithHiddenWords.quickGet(id.index())) {
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

    uint64_t size = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(end);
    if (size > build.limit(LimitID::MaxFieldOffset)) {
        build.diagnostics().add(InternalError { .kind = InternalErrorKind::InvalidLayout, .layout = id, .limit = LimitID::MaxFieldOffset, .count = clampTo<uint32_t>(size) });
        return { };
    }
    planned.size = static_cast<uint32_t>(size);
    return planned;
}

MentionsOfKeys countMentionsOfKeys(const Program&, const Facts&, const ParallelLoops&);
void planNames(const Program&, const MentionsOfKeys&, BuildContext&, SlotPlanParts&);

SlotPlanParts computeSlotPlanParts(Program&, const Facts&, BuildContext&);
static void dropTypesOfNumbersLeftBoxedInSlotPlan(const Program& program, SlotPlanParts& parts)
{
    for (PlannedLayout& layout : parts.layouts) {
        for (PlacedSlot& slot : layout.slots) {
            if (slot.type && slot.representation == SlotRepresentation::JSValue && program.types.at(slot.type).isNumberOrNullish())
                slot.type = { };
        }
    }
}

void holdDoorsToProofsInSlotPlan(Program&, const Facts&, SlotPlanParts&);
void holdDoorsToProofsInSlotPlan(Program& program, const Facts& facts, SlotPlanParts& parts)
{
    TypeArena types(program.types, ArenaKeeps::InProgram);
    HashMap<uint64_t, bool, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> provenLiesInSlot;
    std::array<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>, 2> askedOfProvenInSlot;
    for (uint32_t index = 0; index < parts.layouts.size(); ++index) {
        LayoutKind kind = program.layouts[index].kind;
        bool hasDoors = kind == LayoutKind::Record || kind == LayoutKind::Instance || kind == LayoutKind::Container;
        PlannedLayout& planned = parts.layouts[index];
        for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
            PlacedSlot& slot = planned.slots[field];
            FieldReference reference { LayoutID(index), field };
            TypeID proven = facts.linked().provenOfField(reference);
            if (!proven)
                continue;
            uint64_t key = static_cast<uint64_t>(proven.bits()) << 32 | slot.type.bits();
            slot.everyStoreFits = !slot.type || provenLiesInSlot.ensure(key, [&] { return types.isSubset(proven, slot.type); }).iterator->value;
            if (!slot.everyStoreFits || !hasDoors || !slot.indexInLayoutRecord || !slot.hasStorage() || slot.holdsAccessor || slot.deferredFunction)
                continue;
            bool isStoredByNameWithoutTest = slot.representation == SlotRepresentation::OptionalInt32 || slot.representation == SlotRepresentation::OptionalDouble || slot.representation == SlotRepresentation::OptionalBoolean;
            TypeID asked = TypeID::fromBits(askedOfProvenInSlot[isStoredByNameWithoutTest].ensure(key, [&] {
                TypeID asked = typeInWordsOfTestsForSlotPlan(facts, types, proven, true);
                if (asked && isStoredByNameWithoutTest && types.at(asked).primitives & Primitive::Number)
                    asked = types.unionOf(asked, TypeTable::number);
                if (asked && isStoredByNameWithoutTest && types.at(asked).primitives & Primitive::Boolean)
                    asked = types.withPrimitives(asked, Primitive::Boolean);
                if (asked && slot.type)
                    asked = types.intersectionOf(asked, slot.type);
                if (asked && !types.isSubset(types.withoutPrimitives(proven, Primitive::Absent), asked))
                    return TypeID().bits();
                return asked && asked != slot.type && !isNeverType(asked) && !types.at(asked).isAnything() ? asked.bits() : TypeID().bits();
            }).iterator->value);
            if (asked && slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference))
                slot.provenType = asked;
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
    PlacesOfFieldsInSlotPlan places = placeFieldsForSlotPlan(program, facts.linked(), facts.held(), findLayoutsWithSlotsThatStoresMayEmptyForSlotPlan(program, facts), hasTypedOperations ? &facts.families() : nullptr, storage.span(), hasTypedSlotsInSlotPlan(facts));
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
