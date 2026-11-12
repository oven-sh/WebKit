/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlotPlan.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTClosuresOfLiterals.h"
#include "AOTEnvironmentsHandedBack.h"
#include "AOTEnforcedFields.h"
#include "AOTFacts.h"
#include "AOTFamilies.h"
#include "AOTFieldsStorage.h"
#include "AOTIRProgram.h"
#include "AOTKeyOrders.h"
#include "AOTLayoutData.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTPhaseTimes.h"
#include "AOTPromotedEnvironments.h"
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
#include <atomic>
#include <functional>
#include <limits>
#include <optional>
#include <span>
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
    merged.isHeldFilled &= placed.isHeldFilled;
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
                    return &entry->access.withMembersLent();
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
        auto entry = makeUniqueWithoutFastMallocCheck<FieldAccessesOfSets::Part::OfMembers>(key, members, fieldAccess(program, key, members, isStore));
        Locker locker { part.lock };
        auto& kept = part.accessesOfMembers[isStore].add(hash, Vector<std::unique_ptr<FieldAccessesOfSets::Part::OfMembers>, 1>()).iterator->value;
        if (const FieldAccess* access = findIn(kept))
            return *access;
        kept.append(WTF::move(entry));
        return kept.last()->access.withMembersLent();
    }

    uint64_t asked = static_cast<uint64_t>(set.bits()) << 32 | key.bits();
    FieldAccessesOfSets::Part& part = m_fieldAccessesOfSets->parts[WTF::intHash(asked) % m_fieldAccessesOfSets->parts.size()];
    {
        Locker locker { part.lock };
        if (auto known = part.accesses[isStore].find(asked); known != part.accesses[isStore].end())
            return known->value->withMembersLent();
    }
    Vector<LayoutID, 16> membersAsked;
    uint32_t numberInUse = 0;
    FieldAccess access = listMembersAskedAbout(program, key, set, WhatIsWrittenAbove::IsNotAsked, membersAsked, numberInUse) ? fieldAccessOfLayouts(program, key, membersAsked.span(), isStore, numberInUse) : fieldAccess(program, key, members, isStore);
    Locker locker { part.lock };
    return part.accesses[isStore].ensure(asked, [&] {
        return makeUniqueWithoutFastMallocCheck<FieldAccessKeptForBuild>(WTF::move(access));
    }).iterator->value->withMembersLent();
}

bool SlotPlan::listMembersAskedAbout(const Program& program, PropertyKeyID key, LayoutSetID set, WhatIsWrittenAbove whatIsWrittenAbove, Vector<LayoutID, 16>& asked, uint32_t& numberInUse) const
{
    static constexpr size_t mostMembersAskedOneByOne = 64;
    if (!m_fieldAccessesOfSets || !set || !key || holdsOneMember(set) || liesInThread(set) || liesInThread(key))
        return false;
    std::span<const LayoutID> members = membersOf(program.types, set);
    if (members.size() <= mostMembersAskedOneByOne)
        return false;

    auto sortWithoutRepeats = [](auto& list) {
        AOT::sort(list);
        list.shrink(list.size() - std::ranges::unique(list).size());
        list.shrinkToFit();
    };
    Vector<uint64_t>& layoutsByName = m_fieldAccessesOfSets->layoutsByName;
    std::call_once(m_fieldAccessesOfSets->layoutsByNameAreListed, [&] {
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            auto list = [&](PropertyKeyID name) {
                if (name)
                    layoutsByName.append(static_cast<uint64_t>(name.bits()) << 32 | index);
            };
            for (const Field& field : program.layouts[index].fields)
                list(field.key);
            for (PropertyKeyID appended : layout(LayoutID(index)).appendedKeys)
                list(appended);
        }
        sortWithoutRepeats(layoutsByName);
        BitVector isView(program.layouts.size());
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            for (LayoutID view : program.viewsAbove(LayoutID(index)))
                isView.quickSet(view.index());
        }
        for (size_t view : isView) {
            for (const Field& field : program.layouts[view].fields) {
                if (field.key)
                    m_fieldAccessesOfSets->namesWrittenInViews.append(field.key.bits());
            }
        }
        sortWithoutRepeats(m_fieldAccessesOfSets->namesWrittenInViews);
    });
    if (whatIsWrittenAbove == WhatIsWrittenAbove::IsAsked && std::ranges::binary_search(m_fieldAccessesOfSets->namesWrittenInViews, key.bits()))
        return false;
    uint64_t first = static_cast<uint64_t>(key.bits()) << 32;
    auto firstWithName = std::ranges::lower_bound(layoutsByName, first);
    auto endWithName = std::ranges::lower_bound(firstWithName, layoutsByName.end(), first + (1ull << 32));
    if (static_cast<size_t>(endWithName - firstWithName) >= members.size())
        return false;

    using MembersByName = FieldAccessesOfSets::Part::MembersByName;
    FieldAccessesOfSets::Part& part = m_fieldAccessesOfSets->parts[WTF::intHash(set.bits()) % m_fieldAccessesOfSets->parts.size()];
    Locker locker { part.lock };
    MembersByName& listed = *part.membersByName.ensure(set.bits(), [] {
        return makeUniqueWithoutFastMallocCheck<MembersByName>();
    }).iterator->value;
    if (!listed.areListed) {
        listed.areListed = true;
        for (LayoutID member : members) {
            const Layout& layout = program.at(member);
            listed.numberInUse += hasDirectInstances(member);
            if (writtenBaseOf(layout) && layout.inheritedFields != InheritedFields::AllOfRow)
                listed.askedAboutEveryName.append(member);
        }
    }

    numberInUse = listed.numberInUse;
    auto everyName = listed.askedAboutEveryName.begin();
    for (auto withName = firstWithName; withName != endWithName; ++withName) {
        LayoutID member(static_cast<uint32_t>(*withName));
        if (!std::ranges::binary_search(members, member))
            continue;
        while (everyName != listed.askedAboutEveryName.end() && *everyName < member)
            asked.append(*everyName++);
        if (everyName != listed.askedAboutEveryName.end() && *everyName == member)
            ++everyName;
        asked.append(member);
    }
    while (everyName != listed.askedAboutEveryName.end())
        asked.append(*everyName++);
    return true;
}

FieldAccess SlotPlan::fieldAccessOfOneLayout(const Program& program, PropertyKeyID key, LayoutID receiver) const
{
    FieldAccess access;
    access.key = key;
    if (!hasDirectInstances(receiver) || !holdsCompiledObjects(receiver))
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

FieldAccess SlotPlan::fieldAccessOfLayouts(const Program& program, PropertyKeyID key, std::span<const LayoutID> receivers, bool isStore, std::optional<uint32_t> numberInUseAmongAllReceivers) const
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
    uint32_t numberInUse = 0;
    for (LayoutID member : receivers) {
        if (!hasDirectInstances(member))
            continue;
        ++numberInUse;
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
    if (numberInUseAmongAllReceivers && numberInUse < *numberInUseAmongAllReceivers)
        coversEveryMember = false;
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
        if (!hasDirectInstances(member))
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
        if (!hasDirectInstances(member))
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
    Vector<FormOfPlaceInSlotPlan> formsProven;
    Vector<TypeID> typesHeld;
    Vector<Vector<TypeID>> typesHeldOfAppendedFields;
    Vector<uint64_t> ends;
    static constexpr uint8_t inCell = UINT8_MAX;
    Vector<BitVector> appendedFieldsThatShareGranule;
    Vector<Vector<uint8_t>> granulesOfAppendedFields;
    Vector<uint32_t> cellFieldNumbersFromBase;
    static constexpr uint8_t granuleNotChosen = inCell - 1;
    static constexpr uint8_t withoutSlot = inCell - 2;
    Vector<uint8_t> granulesOfListedFields;
    BitVector rowsGivenByNoMaker;
    bool liesOutOfLine(uint32_t row) const { return granulesOfListedFields[row] != inCell; }
    BitVector layoutsWithListedFieldsOutOfLine;
    BitVector layoutsWithHiddenWords;
    BitVector familiesThatMayAgreeOnSlots;
};

using StorageOfLayoutInSlotPlan = StorageOfLayout;

} // anonymous namespace

static BitVector findLayoutsNeverAllocatedForSlotPlan(const Program& program, const LinkTables& linked, const ParallelLoops& loops)
{
    static_assert(!(indicesInRangeOfLoop % 64));
    BitVector layoutsNeverAllocated(program.layouts.size());
    forEachRangeOfIndicesInParallel(loops, program.layouts.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            if (program.layouts[index].has(LayoutFlags::IsNeverAllocated) || (index < linked.layouts.size() && !linked.layouts[index].isAllocated))
                layoutsNeverAllocated.quickSet(index);
        }
    });
    for (const TaggedFamily& family : program.taggedFamilies) {
        for (const TaggedLayout& member : family.members)
            layoutsNeverAllocated.quickSet(member.layout.index());
    }
    return layoutsNeverAllocated;
}

static bool declaresPrivateNameForSlotPlan(const Layout& layout)
{
    constexpr OptionSet<FieldFlags> flagsOfPrivateNames { FieldFlags::IsPrivate, FieldFlags::IsBrand };
    return std::ranges::any_of(layout.fields.span().first(layout.copiesWholeFieldListOfBase ? layout.ownFieldCount : layout.fields.size()), [&](const Field& field) {
        return field.flags.containsAny(flagsOfPrivateNames) && !field.flags.contains(FieldFlags::IsInherited);
    });
}

static StorageOfLayoutInSlotPlan chooseStorageOfLayoutForSlotPlan(const Layout& layout, bool isNeverAllocated)
{
    bool hasCellsOfItsOwn = !isNeverAllocated && !builtinClassOfInstances(layout);
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
    if (std::optional<uint32_t> listedAbove = program.at(base).findField(listed.key))
        above = { base, *listedAbove };
    else if (listed.flags.contains(FieldFlags::IsInherited))
        above = declarationOf(program, reference);
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

enum class HiddenWordUse : uint8_t { Never, Unseen, Seen };

static HiddenWordUse hiddenWordUseInSlotPlan(const Program& program, const LinkTables& link, const LinkTables& held, const BitVector& layoutsWithSlotsThatStoresMayEmpty, const BitVector& layoutsThatRepeatTheirBases, LayoutID id)
{
    constexpr OptionSet<LayoutChange> changesThatMakeExtraProperties { LayoutChange::GainsProperty, LayoutChange::GetsAccessor };
    LayoutKind kind = program.at(id).kind;
    if (kind != LayoutKind::Record && kind != LayoutKind::Instance)
        return HiddenWordUse::Seen;
    if (id.index() >= link.layouts.size() || layoutsWithSlotsThatStoresMayEmpty.get(id.index()))
        return HiddenWordUse::Seen;
    uint32_t numberDeclared = numberOfFieldsNotRepeated(program, layoutsThatRepeatTheirBases, id);
    for (uint32_t field = 0; field < numberDeclared; ++field) {
        if (!fieldAt(program, declarationOf(program, { id, field })).form.isEmpty())
            return HiddenWordUse::Seen;
    }
    if (link.numberOfAppendedKeysOfPlan(id))
        return HiddenWordUse::Seen;
    if (!link.layout(id).possibleChanges.containsAny(changesThatMakeExtraProperties))
        return HiddenWordUse::Never;
    bool isExtendedAlone = link.layout(id).isExtendedAlone || id.index() >= held.layouts.size() || held.layout(id).isExtendedAlone;
    FunctionID constructor = program.at(id).constructor;
    bool isConstructedByFewCalls = !constructor || constructor.index() >= link.functions.size() || link.function(constructor).numberOfCallSites <= 2;
    return kind == LayoutKind::Instance && !isExtendedAlone && !isConstructedByFewCalls && !program.at(id).isDerived() ? HiddenWordUse::Unseen : HiddenWordUse::Seen;
}

static BitVector findLayoutsWithHiddenWordsForSlotPlan(const Program& program, const LinkTables* link, const LinkTables& held, const BitVector& layoutsWithSlotsThatStoresMayEmpty, const BitVector& layoutsThatRepeatTheirBases, const Families* families, std::span<const StorageOfLayoutInSlotPlan> storage, const CellSizesForFields& cellSizes)
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
    BitVector groupsWithCells(numberOfLayouts);
    BitVector groupsOfSeveral(numberOfLayouts);
    auto hasCells = [&](uint32_t index) {
        return storage[index].storage == LayoutStorage::CompiledObject && storage[index].hasCells();
    };
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (hasCells(index) && groupsWithCells.quickSet(groups.representativeOf(index)))
            groupsOfSeveral.quickSet(groups.representativeOf(index));
    }
    BitVector layoutsWithoutSublayouts(numberOfLayouts);
    auto isSmallerWithoutHiddenWord = [&](uint32_t index) {
        uint64_t numberOfFields = program.layouts[index].fields.size();
        for (uint32_t field = 0; field < numberOfFields; ++field) {
            if (mayLieOutOfLineThoughListedInSlotPlan(program, held, layoutsWithoutSublayouts, { LayoutID(index), field }))
                return false;
        }
        return numberOfFields && cellSizes.cellSizeFor(offsetOfFirstField + numberOfFields * bytesPerSlotInSlotPlan) < cellSizes.cellSizeFor(offsetOfFirstField + (numberOfFields + 1) * bytesPerSlotInSlotPlan);
    };
    BitVector groupsWithWords(numberOfLayouts);
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        if (!isObject(index) || (link && !hasCells(index)))
            continue;
        HiddenWordUse use = link ? hiddenWordUseInSlotPlan(program, *link, held, layoutsWithSlotsThatStoresMayEmpty, layoutsThatRepeatTheirBases, LayoutID(index)) : HiddenWordUse::Seen;
        bool canOmitHiddenWord = use == HiddenWordUse::Unseen && !groupsOfSeveral.quickGet(groups.representativeOf(index)) && isSmallerWithoutHiddenWord(index);
        if (use == HiddenWordUse::Seen || (use == HiddenWordUse::Unseen && !canOmitHiddenWord))
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

static void chooseListedFieldsOutOfLineForSlotPlan(const Program& program, const LinkTables& held, const Families& families, std::span<const StorageOfLayoutInSlotPlan> storage, PlacesOfFieldsInSlotPlan& result)
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
        if (FunctionID maker = held.layouts[index].onlyFunctionThatMakesObjects; maker && program.at(maker).isModuleBody())
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
    const BitVector& familiesThatMayAgree = result.familiesThatMayAgreeOnSlots;
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

static BitVector findMergingFamilies(const Program& program, const Facts& facts, const Families& families)
{
    const LinkTables& linked = facts.linked();
    TypeReader reader(program);
    size_t numberOfTypes = program.types.numberOfTypes();
    BitVector result(families.size());
    BitVector familiesNamedWhole(families.size());
    BitVector typesAsked(numberOfTypes);
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> setsAsked;
    Vector<TypeID, 32> typesToAsk;
    Vector<uint32_t> lookOfFamily(FillWith { }, families.size(), 0);
    Vector<uint32_t> metFrom(FillWith { }, families.size(), 0);
    uint32_t look = 0;
    auto meet = [&](LayoutSetID layouts) {
        if (!layouts || holdsOneMember(layouts) || !setsAsked.add(layouts.bits()).isNewEntry)
            return;
        if (FamilyID family = families.familyOf(layouts)) {
            familiesNamedWhole.quickSet(family.index());
            return;
        }
        ++look;
        reader.at(layouts).forEach([&](LayoutID member) {
            uint32_t inside = families.size() + member.index();
            for (FamilyID family = families.nearestFamilyOf(member); family; family = families.nearestFamilyAbove(family)) {
                uint32_t number = family.index();
                if (std::exchange(lookOfFamily[number], look) == look) {
                    if (metFrom[number] != inside)
                        result.quickSet(number);
                    break;
                }
                metFrom[number] = inside;
                inside = number;
            }
        });
    };
    auto ask = [&](TypeID type) {
        if (type && !holdsOnlyPrimitives(type) && liesInProgram(type) && positionOf(type) < numberOfTypes && !typesAsked.quickSet(positionOf(type)))
            typesToAsk.append(type);
        while (!typesToAsk.isEmpty()) {
            Type read = reader.at(typesToAsk.takeLast());
            meet(read.layouts);
            for (const ClassPart& part : classPartsOf(reader, read)) {
                for (TypeID contents : part.writtenContents) {
                    if (contents && !holdsOnlyPrimitives(contents) && liesInProgram(contents) && positionOf(contents) < numberOfTypes && !typesAsked.quickSet(positionOf(contents)))
                        typesToAsk.append(contents);
                }
            }
        }
    };
    auto queryCell = [&](const LinkedCell& cell, const DeclaredType& declared) {
        ask(cell.proven ? cell.proven : declared.written());
    };
    for (uint32_t index = 0; index < program.functions.size() && index < linked.functions.size(); ++index) {
        if (!linked.functions[index].isReached)
            continue;
        ask(linked.functions[index].result);
        for (const LinkedParameter& parameter : linked.parametersOf(FunctionID(index)))
            ask(parameter.proven ? parameter.proven : parameter.writtenOrSeenToArrive());
    }
    for (size_t index = 0; index < linked.variables.size(); ++index)
        queryCell(linked.variables[index], program.variables[index].declared);
    for (uint32_t index = 0; index < program.layouts.size() && index < linked.layouts.size(); ++index) {
        if (!linked.layouts[index].isAllocated)
            continue;
        const auto& fields = program.layouts[index].fields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            if (const LinkedCell* cell = linked.findCellOfField({ LayoutID(index), field }))
                queryCell(*cell, fields[field].declared);
        }
    }
    for (const LinkedCell& cell : linked.appendedFields)
        ask(cell.proven);
    for (uint32_t number = families.size(); number--;) {
        FamilyID above = families.nearestFamilyAbove(FamilyID(number));
        if (above && familiesNamedWhole.quickGet(above.index()))
            familiesNamedWhole.quickSet(number);
    }
    result.merge(familiesNamedWhole);
    return result;
}

static bool rowFormsDiffer(const PlacesOfFieldsInSlotPlan& places, std::span<const uint32_t> rows)
{
    bool hasAnyValueForm = false;
    bool someHoldNumbers = false;
    for (uint32_t row : rows) {
        hasAnyValueForm |= places.formsProven[row] == FormOfPlaceInSlotPlan::AnyValue;
        someHoldNumbers |= places.formsProven[row] == FormOfPlaceInSlotPlan::Integer || places.formsProven[row] == FormOfPlaceInSlotPlan::Number;
    }
    return hasAnyValueForm && someHoldNumbers;
}

static void placeFieldsSharedByFamiliesForSlotPlan(const Program& program, const Facts& facts, const Families& families, std::span<const StorageOfLayoutInSlotPlan> storage, PlacesOfFieldsInSlotPlan& result)
{
    Vector<LayoutID, 16> members;
    Vector<uint32_t, 16> rows;
    std::optional<BitVector> mergingFamilies;
    auto meets = [&](uint32_t family) {
        if (!mergingFamilies)
            mergingFamilies = findMergingFamilies(program, facts, families);
        return mergingFamilies->quickGet(family);
    };
    const BitVector& familiesThatMayAgree = result.familiesThatMayAgreeOnSlots;
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
            bool numbersStandApart = !result.formsProven.isEmpty() && rowFormsDiffer(result, rows.span()) && !meets(number);
            uint32_t place = static_cast<uint32_t>(result.formsOfPlaces.size());
            for (uint32_t row : rows) {
                result.offsets[row] = end;
                result.places[row] = place + (numbersStandApart && result.formsProven[row] != FormOfPlaceInSlotPlan::AnyValue);
            }
            result.formsOfPlaces.append(FormOfPlaceInSlotPlan::Unclaimed);
            if (numbersStandApart)
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
    result.cellFieldNumbersFromBase.fill(0, numberOfLayouts);
    result.appendedFieldsThatShareGranule.grow(numberOfLayouts);
    Vector<uint8_t> numbersOfGranulesTaken;
    numbersOfGranulesTaken.fill(0, numberOfLayouts);
    BitVector hasListedAppendedFields(numberOfLayouts);
    auto listAppendedFieldsOf = [&](auto& self, uint32_t index) -> void {
        if (hasListedAppendedFields.quickSet(index) || storage[index].storage != LayoutStorage::CompiledObject || !storage[index].hasCells())
            return;
        LayoutID base = isObjectLayout(program.layouts[index].kind) ? writtenBaseOf(program.layouts[index]) : LayoutID();
        if (base)
            self(self, base.index());
        uint32_t numberListed = static_cast<uint32_t>(program.layouts[index].fields.size());
        Vector<PropertyKeyID, 16> keys = link.appendedKeysOfPlan(LayoutID(index));
        Vector<PropertyKeyID, 16> keysOfBase = base && !result.granulesOfAppendedFields[base.index()].isEmpty() ? link.appendedKeysOfPlan(base) : Vector<PropertyKeyID, 16>();
        for (uint32_t place = 0; place < keys.size(); ++place) {
            size_t placeInBase = std::ranges::find(keysOfBase, keys[place]) - keysOfBase.begin();
            bool isInBaseCell = placeInBase < keysOfBase.size() && result.granulesOfAppendedFields[base.index()][placeInBase] == PlacesOfFieldsInSlotPlan::inCell;
            bool isRequired = link.isAppendedFieldRequiredByTypeOfThis({ LayoutID(index), numberListed + place });
            bool isOfComputedLiteral = link.computedLiterals.get(index) && numberListed + keys.size() <= maxComputedLiteralNames;
            result.cellFieldNumbersFromBase[index] += isInBaseCell && !isRequired;
            result.granulesOfAppendedFields[index].append(isInBaseCell || isRequired || isOfComputedLiteral ? PlacesOfFieldsInSlotPlan::inCell : noGranule);
        }
    };
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        listAppendedFieldsOf(listAppendedFieldsOf, index);

    auto addPlaceOutOfLine = [&] {
        result.formsOfPlaces.append(FormOfPlaceInSlotPlan::AnyValue);
        return static_cast<uint32_t>(result.formsOfPlaces.size() - 1);
    };

    Vector<LayoutID, 16> members;
    Vector<uint8_t*, 16> granules;
    Vector<size_t, 16> placesInMembers;
    Vector<uint32_t, 16> rows;
    const BitVector& familiesThatMayAgree = result.familiesThatMayAgreeOnSlots;
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
                granule = mayGainPropertiesInSlotPlan(link, LayoutID(index)) && (hasListedFieldsOutOfLine || program.layouts[index].fields.size() < fieldsOfLargeLayout) ? PlacesOfFieldsInSlotPlan::withoutSlot : PlacesOfFieldsInSlotPlan::inCell;
        }
    };
    for (uint32_t index = 0; index < numberOfLayouts; ++index)
        numberGranulesOf(numberGranulesOf, index);
}

static void placeFieldsOfLayoutForSlotPlan(const Program& program, const BitVector& isPlaced, const StorageOfLayouts& storageOfLayouts, LayoutID id, PlacesOfFieldsInSlotPlan& result)
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
    uint32_t numberOfFields = numberOfFieldsNotRepeated(program, storageOfLayouts.layoutsThatRepeatTheirBases, id);
    for (uint32_t field = 0; base && field < numberOfFields; ++field) {
        std::optional<FieldReference> above = findFieldAboveForSlotPlan(program, isPlaced, { id, field }, base);
        if (!above || result.liesOutOfLine(result.rowOf({ id, field })))
            continue;
        uint32_t row = result.rowOf({ id, field });
        uint32_t rowAbove = result.rowOf(fieldNotRepeated(program, storageOfLayouts.layoutsThatRepeatTheirBases, storageOfLayouts.nearestLayoutsAboveThatDoNotRepeat.span(), *above));
        result.offsets[row] = result.offsets[rowAbove];
        result.places[row] = result.places[rowAbove];
        end = std::max(end, result.offsets[row] + bytesPerSlotInSlotPlan);
    }
    uint32_t firstRow = result.firstRowOfLayout[id.index()];
    std::span<uint32_t> placesOfLayout = result.places.mutableSpan().subspan(firstRow, numberOfFields);
    std::span<uint64_t> offsetsOfLayout = result.offsets.mutableSpan().subspan(firstRow, numberOfFields);
    auto placeAtEnd = [&](uint32_t field) {
        if (field >= numberOfFields || placesOfLayout[field] != PlacesOfFieldsInSlotPlan::noPlace)
            return;
        offsetsOfLayout[field] = end;
        placesOfLayout[field] = static_cast<uint32_t>(result.formsOfPlaces.size());
        result.formsOfPlaces.append(FormOfPlaceInSlotPlan::Unclaimed);
        end += bytesPerSlotInSlotPlan;
    };
    for (uint32_t field : layout.declaredOrder)
        placeAtEnd(field);
    for (uint32_t field = 0; field < numberOfFields; ++field)
        placeAtEnd(field);
    result.ends[id.index()] = end;
}

static void numberRowsForSlotPlan(const Program& program, PlacesOfFieldsInSlotPlan& result)
{
    result.firstRowOfLayout.reserveInitialCapacity(program.layouts.size() + 1);
    uint64_t numberOfRows = 0;
    for (const Layout& layout : program.layouts) {
        result.firstRowOfLayout.append(static_cast<uint32_t>(numberOfRows));
        numberOfRows += layout.fields.size();
    }
    RELEASE_ASSERT(numberOfRows < PlacesOfFieldsInSlotPlan::noPlace);
    result.firstRowOfLayout.append(static_cast<uint32_t>(numberOfRows));
}

static void placeFieldsForSlotPlan(const Program& program, const Facts& facts, const BitVector& layoutsWithSlotsThatStoresMayEmpty, const Families* families, const LayoutsBelowTheirBases& layoutsBelowTheirBases, std::span<const StorageOfLayoutInSlotPlan> storage, const StorageOfLayouts& storageOfLayouts, bool hasTypedSlots, const CellSizesForFields& cellSizes, PlacesOfFieldsInSlotPlan& result)
{
    const LinkTables& link = facts.linked();
    const LinkTables& held = facts.held();
    const BitVector& layoutsThatRepeatTheirBases = storageOfLayouts.layoutsThatRepeatTheirBases;
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    uint32_t numberOfRows = result.firstRowOfLayout.last();
    result.offsets.fill(0, numberOfRows);
    result.places.fill(PlacesOfFieldsInSlotPlan::noPlace, numberOfRows);
    result.ends.fill(0, numberOfLayouts);
    result.granulesOfListedFields.fill(PlacesOfFieldsInSlotPlan::inCell, numberOfRows);
    result.layoutsWithListedFieldsOutOfLine.ensureSize(numberOfLayouts);
    result.layoutsWithHiddenWords = findLayoutsWithHiddenWordsForSlotPlan(program, hasTypedSlots ? &link : nullptr, held, layoutsWithSlotsThatStoresMayEmpty, layoutsThatRepeatTheirBases, families, storage, cellSizes);
    if (families)
        result.familiesThatMayAgreeOnSlots = findFamiliesThatMayAgreeOnSlotsForSlotPlan(program, *families, storage);
    if (families && hasTypedSlots)
        chooseListedFieldsOutOfLineForSlotPlan(program, held, *families, storage, result);
    placeAppendedFieldsForSlotPlan(program, link, families, storage, result);
    if (families) {
        placeFieldsSharedByFamiliesForSlotPlan(program, facts, *families, storage, result);
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
        Vector<uint8_t> offsetsOfFirstSlots(numberOfLayouts, [&](size_t index) {
            return static_cast<uint8_t>(result.offsetOfFirstSlotOf(LayoutID(index)));
        });
        placeFieldsOfUnions(program, link, *families, layoutsBelowTheirBases, RowsOfSlotPlan { result.firstRowOfLayout.span(), result.offsets.mutableSpan(), result.places.mutableSpan(), result.ends.mutableSpan(), offsetsOfFirstSlots.span(), widthsInCell.span(), &layoutsThatRepeatTheirBases, storageOfLayouts.nearestLayoutsAboveThatDoNotRepeat.span() }, hasCells, addPlace);
    }

    BitVector isPlaced(numberOfLayouts);
    BitVector isWaiting(numberOfLayouts);
    Vector<LayoutID, 16> waiting;
    for (uint32_t index = 0; index < numberOfLayouts; ++index) {
        for (LayoutID current(index); current && !isPlaced.quickGet(current.index()) && !isWaiting.quickSet(current.index()); current = writtenBaseOf(program.at(current)))
            waiting.append(current);
        while (!waiting.isEmpty()) {
            LayoutID id = waiting.takeLast();
            placeFieldsOfLayoutForSlotPlan(program, isPlaced, storageOfLayouts, id, result);
            isPlaced.quickSet(id.index());
        }
    }

}

enum class SlotGoesByType : bool { InWhatItIsHeldTo, InItsRawForm };

static bool slotMayGoByTypeOfFieldForSlotPlan(const Program& program, const Facts& facts, FieldReference reference, SlotGoesByType asked = SlotGoesByType::InItsRawForm)
{
    constexpr OptionSet<FieldFlags> filledByAllocationAlone { FieldFlags::IsBrand, FieldFlags::IsHidden };
    const Field* declaration = reference.field < program.at(reference.layout).fields.size() ? &fieldAt(program, declarationOf(program, reference)) : nullptr;
    auto formForbids = [&](OptionSet<FieldForm> form) {
        return asked == SlotGoesByType::InItsRawForm ? !form.isEmpty() : form.contains(FieldForm::IsAccessor);
    };
    if (declaration && (formForbids(declaration->form) || declaration->flags.containsAny(filledByAllocationAlone)))
        return false;
    FieldFacts fieldFacts = facts.field(reference);
    if ((declaration ? formForbids(fieldFacts.formOfValue) : !fieldFacts.formOfValue.isEmpty()) || fieldFacts.facts.contains(FieldFact::IsAccessor))
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
    if (!slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference, SlotGoesByType::InWhatItIsHeldTo))
        return { };
    FieldFacts enforced = facts.field(reference);
    bool namesOrdinaryObjects = enforced.slotType && std::ranges::any_of(membersOf(types, types.at(enforced.slotType)), [&](LayoutID member) {
        return program.at(member).holdsOrdinaryObjects;
    });
    TypeID held = namesOrdinaryObjects ? types.unionOf(types.withoutObjects(enforced.slotType), TypeTable::object) : enforced.slotType;
    return contentsOfEnforcedSlot(types, held, enforced.facts.contains(FieldFact::SlotHoldsUndefinedAndNull));
}

static bool someStoreMayEmptySlotInSlotPlan(const Program& program, const Facts& facts, FieldReference reference)
{
    TypeID held = facts.field(reference).slotType;
    if (!held || !slotMayGoByTypeOfFieldForSlotPlan(program, facts, reference, SlotGoesByType::InWhatItIsHeldTo))
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

static FormOfPlaceInSlotPlan formOfSlotOfScopeForSlotPlan(const Program& program, const Facts& facts, const LinkTables& link, FieldReference reference)
{
    if (fieldAt(program, reference).declared.isEstablished())
        return FormOfPlaceInSlotPlan::AnyValue;
    return formProvenOfFieldForSlotPlan(program, facts, link, reference);
}

static BitVector lookAtFieldsAheadOfPlacesForSlotPlan(Program& program, const Facts& facts, const ParallelLoops& loops, const BitVector& layoutsThatRepeatTheirBases, bool hasTypedSlots, PlacesOfFieldsInSlotPlan& places)
{
    static constexpr uint32_t layoutsInRange = 256;
    static_assert(!(layoutsInRange % 64));
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    const LinkTables& link = facts.linked();
    BitVector layoutsWithSlotsThatStoresMayEmpty(numberOfLayouts);
    if (hasTypedSlots) {
        places.typesHeld.fill(TypeID(), places.firstRowOfLayout.last());
        places.formsProven.fill(FormOfPlaceInSlotPlan::Unclaimed, places.firstRowOfLayout.last());
        places.typesHeldOfAppendedFields.grow(numberOfLayouts);
    }
    ArenasOfWorkers arenas(program.types, loops);
    forEachIndexInParallel(loops, (numberOfLayouts + layoutsInRange - 1) / layoutsInRange, [&](unsigned range, unsigned worker) {
        uint32_t endOfRange = std::min(numberOfLayouts, (range + 1) * layoutsInRange);
        for (uint32_t index = range * layoutsInRange; index < endOfRange; ++index) {
            LayoutID id(index);
            const Layout& layout = program.layouts[index];
            uint32_t numberDeclared = static_cast<uint32_t>(layout.fields.size());
            uint32_t numberNotRepeated = numberOfFieldsNotRepeated(program, layoutsThatRepeatTheirBases, id);
            if (layout.kind == LayoutKind::Record || layout.kind == LayoutKind::Instance) {
                for (uint32_t field = 0; field < numberNotRepeated; ++field) {
                    if (!someStoreMayEmptySlotInSlotPlan(program, facts, { id, field }))
                        continue;
                    layoutsWithSlotsThatStoresMayEmpty.quickSet(index);
                    break;
                }
            }
            if (hasTypedSlots && layout.kind == LayoutKind::Environment) {
                for (uint32_t field = 0; field < layout.fields.size(); ++field)
                    places.formsProven[places.firstRowOfLayout[index] + field] = formOfSlotOfScopeForSlotPlan(program, facts, link, { id, field });
            }
            if (!hasTypedSlots || !isObjectLayout(layout.kind))
                continue;
            TypeArena& types = arenas.of(worker);
            uint32_t firstRow = places.firstRowOfLayout[index];
            for (uint32_t field = 0; field < numberNotRepeated; ++field) {
                places.typesHeld[firstRow + field] = typeHeldOfFieldForSlotPlan(program, facts, types, { id, field });
                places.formsProven[firstRow + field] = formProvenOfFieldForSlotPlan(program, facts, link, { id, field });
            }
            for (uint32_t place = 0; place < link.numberOfAppendedKeysOfPlan(id); ++place)
                places.typesHeldOfAppendedFields[index].append(typeHeldOfFieldForSlotPlan(program, facts, types, { id, numberDeclared + place }));
        }
    });
    return layoutsWithSlotsThatStoresMayEmpty;
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

static void chooseFormsOfPlacesForSlotPlan(Program& program, const Facts& facts, const LinkTables& link, const Families* families, std::span<const StorageOfLayoutInSlotPlan> storage, const BitVector& layoutsThatRepeatTheirBases, PlacesOfFieldsInSlotPlan& places)
{
    static_assert(FormOfPlaceInSlotPlan::AnyValue > FormOfPlaceInSlotPlan::Number && FormOfPlaceInSlotPlan::AnyValue > FormOfPlaceInSlotPlan::Integer && FormOfPlaceInSlotPlan::AnyValue > FormOfPlaceInSlotPlan::Unclaimed);
    TypeArena types(program.types, ArenaKeeps::InProgram);
    BitVector placesWithRowThatTakesEveryValue(places.formsOfPlaces.size());
    BitVector rowsFilledOnceByDefinition(places.typesHeld.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!isObjectLayout(program.layouts[index].kind))
            continue;
        uint32_t numberOfRows = numberOfFieldsNotRepeated(program, layoutsThatRepeatTheirBases, LayoutID(index));
        for (uint32_t field = 0; field < numberOfRows; ++field) {
            if (fieldAt(program, declarationOf(program, { LayoutID(index), field })).form.containsAll({ FieldForm::IsReadOnly, FieldForm::IsNotConfigurable }))
                rowsFilledOnceByDefinition.quickSet(places.firstRowOfLayout[index] + field);
        }
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (!isObjectLayout(program.layouts[index].kind) && program.layouts[index].kind != LayoutKind::Environment)
            continue;
        uint32_t endOfRows = places.firstRowOfLayout[index] + numberOfFieldsNotRepeated(program, layoutsThatRepeatTheirBases, LayoutID(index));
        for (uint32_t row = places.firstRowOfLayout[index]; row < endOfRows; ++row) {
            if (!places.typesHeld[row])
                placesWithRowThatTakesEveryValue.quickSet(places.places[row]);
            FormOfPlaceInSlotPlan& form = places.formsOfPlaces[places.places[row]];
            form = std::max(form, places.formsProven[row]);
        }
    }

    for (uint32_t row = 0; row < places.typesHeld.size(); ++row) {
        if (places.typesHeld[row] && placesWithRowThatTakesEveryValue.quickGet(places.places[row]) && !rowsFilledOnceByDefinition.quickGet(row))
            places.typesHeld[row] = { };
    }

    constexpr uint32_t isNotUnited = UINT32_MAX;
    Vector<uint32_t> nextPositionAtPlace;
    nextPositionAtPlace.fill(0, places.formsOfPlaces.size());
    for (uint32_t row = 0; row < places.places.size(); ++row) {
        if (places.typesHeld[row])
            ++nextPositionAtPlace[places.places[row]];
    }
    uint32_t numberOfRowsUnited = 0;
    for (uint32_t& position : nextPositionAtPlace) {
        uint32_t numberOfRowsHeld = position;
        position = numberOfRowsHeld > 1 ? numberOfRowsUnited : isNotUnited;
        numberOfRowsUnited += numberOfRowsHeld > 1 ? numberOfRowsHeld : 0;
    }
    Vector<uint32_t> rowsByPlace;
    rowsByPlace.fill(0, numberOfRowsUnited);
    for (uint32_t row = 0; row < places.places.size(); ++row) {
        if (!places.typesHeld[row])
            continue;
        uint32_t& position = nextPositionAtPlace[places.places[row]];
        if (position != isNotUnited)
            rowsByPlace[position++] = row;
    }
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
    const BitVector& familiesThatMayAgree = places.familiesThatMayAgreeOnSlots;
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

bool takesAccessorsOfLibraryAlone(const Program& program, const SlotPlan& slots, LayoutID id, PropertyKeyID name)
{
    auto isClassOfLibrary = [&](LayoutID classLayout) {
        const Layout& layout = program.at(classLayout);
        return layout.kind == LayoutKind::Instance && layout.constructor && isLibraryCode(program, program.at(layout.constructor));
    };
    LayoutID declared = declaredLayoutOf(program, id);
    if (isClassOfLibrary(declared))
        return true;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID above = program.at(declared).base; above && numberOfClassesLeft--; above = program.at(above).base) {
        std::optional<FieldIndex> field = slots.findField(program, above, name);
        if (field && slots.slot(above, *field).holdsAccessor && isClassOfLibrary(above))
            return true;
    }
    return false;
}

void markFieldsHeldFilledInSlotPlan(const Program& program, const LinkTables& linked, const ParallelLoops& loops, SlotPlanParts& slots)
{
    static constexpr uint32_t layoutsInRange = 256;
    uint32_t numberOfLayouts = static_cast<uint32_t>(slots.layouts.size());
    forEachIndexInParallel(loops, (numberOfLayouts + layoutsInRange - 1) / layoutsInRange, [&](unsigned range) {
        uint32_t endOfRange = std::min(numberOfLayouts, (range + 1) * layoutsInRange);
        for (uint32_t index = range * layoutsInRange; index < endOfRange; ++index) {
            LayoutID id(index);
            const Layout& layout = program.at(id);
            PlannedLayout& planned = slots.layouts[index];
            if ((layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance) || !planned.hasSlots())
                continue;
            for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
                const LinkedCell* cell = linked.findCellOfField({ id, field });
                PlacedSlot& slot = planned.slots[field];
                slot.isHeldFilled = cell && cell->isFilled && slot.hasStorage() && !slot.holdsAccessor && !(slot.isOutOfLine() && slot.mayExistWithoutStorage) && fieldStaysInSlot(program, slot);
                if (slot.isHeldFilled)
                    planned.fieldsHeldFilled.append(field);
            }
        }
    });
}

Vector<FieldIndex, 8> fieldsHeldFilledInSlots(const Program&, const LinkTables&, const SlotPlan& slots, LayoutID id)
{
    return Vector<FieldIndex, 8>(slots.layout(id).fieldsHeldFilled.span());
}

bool constantListsFieldsHeldFilledInSlots(const Program& program, const LinkTables& linked, const SlotPlan& slots, DataID data, RootOfConstant root)
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
        bool isAsked = object->layout && (root == RootOfConstant::IsAsked || &node != &program.at(data).root);
        if (Vector<FieldIndex, 8> held = isAsked ? fieldsHeldFilledInSlots(program, linked, slots, object->layout) : Vector<FieldIndex, 8>(); !held.isEmpty()) {
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

void layAppendedFieldInCellForFields(const Program& program, const Facts& facts, FieldReference reference, uint32_t offset, PlacedSlot& slot)
{
    slot.storage = SlotStorage::Inline;
    slot.offset = offset;
    slot.mayExistWithoutStorage = false;
    if (hasTypedSlotsInSlotPlan(facts))
        giveSlotFormOfPlaceInSlotPlan(slot, formProvenOfFieldForSlotPlan(program, facts, facts.linked(), reference), slot.type);
}

static constexpr uint64_t maxCellBytesWithLateNames = 128;

static bool lateNamesFitInCell(const PlacesOfFieldsInSlotPlan& places, const CellSizesForFields& cellSizes, LayoutID id, uint64_t endOfSlotsInCell, bool areFilledTogether)
{
    constexpr uint64_t mostBytesOfCell = maxCellBytesWithLateNames;
    constexpr uint64_t bytesOfLargeCell = 512;
    const uint64_t mostBytesAdded = areFilledTogether ? mostBytesOfCell : 2 * bytesPerSlotInSlotPlan;
    const Vector<uint8_t>& granules = places.granulesOfAppendedFields[id.index()];
    uint64_t end = endOfSlotsInCell;
    endOfSlotsInCell -= places.cellFieldNumbersFromBase[id.index()] * bytesPerSlotInSlotPlan;
    for (size_t place = 0; place < granules.size(); ++place) {
        if (granules[place] >= maxExtensionGranules)
            continue;
        if (places.appendedFieldsThatShareGranule[id.index()].get(place))
            return false;
        end += bytesPerSlotInSlotPlan;
    }
    if (endOfSlotsInCell >= bytesOfLargeCell && end <= endOfSlotsInCell + endOfSlotsInCell / 2)
        return true;
    uint64_t sizeWithMore = std::min(cellSizes.cellSizeFor(endOfSlotsInCell + mostBytesAdded), mostBytesOfCell);
    return cellSizes.cellSizeFor(end) <= std::max(cellSizes.cellSizeFor(endOfSlotsInCell), sizeWithMore);
}

static BitVector lateMethodNamesInCell(const LinkTables& link, const PlacesOfFieldsInSlotPlan& places, const CellSizesForFields& cellSizes, LayoutID id, std::span<const PropertyKeyID> appendedKeys, uint64_t endOfSlotsInCell)
{
    BitVector lieInCell;
    uint64_t end = endOfSlotsInCell;
    for (size_t place = 0; place < appendedKeys.size(); ++place) {
        if (places.granulesOfAppendedFields[id.index()][place] >= maxExtensionGranules || places.appendedFieldsThatShareGranule[id.index()].get(place) || !link.isLateFieldStoredThroughThis(id, appendedKeys[place]))
            continue;
        if (cellSizes.cellSizeFor(end + bytesPerSlotInSlotPlan) > maxCellBytesWithLateNames)
            break;
        lieInCell.set(place);
        end += bytesPerSlotInSlotPlan;
    }
    return lieInCell;
}

static bool mayBeFilledInSightForSlotPlan(const Program& program, const LinkTables& link, FieldReference field)
{
    const LinkedLayout& layout = link.layout(field.layout);
    if (layout.reachesUnseenCode || layout.possibleChanges.contains(LayoutChange::GainsProperty))
        return false;
    TypeID held = link.field(field).proven;
    return !held || program.at(held).hasObjects() || (program.at(held).primitives & ~Primitive::Absent);
}

static PlannedLayout planLayoutForSlotPlan(const Program& program, const Facts& facts, const MentionedKeys& mentioned, const PlacesOfFieldsInSlotPlan& places, const CellSizesForFields& cellSizes, StorageOfLayoutInSlotPlan storage, LayoutID id, BuildContext& build)
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
        bool isSpelledByTypeAlone = declared.key && declared.flags.contains(FieldFlags::IsNeverWritten) && !mentioned.isSpelled(declared.key) && !mayBeFilledInSightForSlotPlan(program, facts.linked(), { id, field });
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
    const bool canPlaceLateNamesInCell = isCompiledObject && !build.recordedOptions().useWideReferences() && !places.layoutsWithListedFieldsOutOfLine.quickGet(id.index());
    const bool lateNamesLieInCell = canPlaceLateNamesInCell && lateNamesFitInCell(places, cellSizes, id, endOfSlotsInCell, facts.linked().layoutsFillingLateFieldsTogether.get(id.index()));
    BitVector lateNamesInCell;
    if (canPlaceLateNamesInCell && !lateNamesLieInCell) {
        lateNamesInCell = lateMethodNamesInCell(facts.linked(), places, cellSizes, id, planned.appendedKeys.span(), endOfSlotsInCell);
        endOfSlotsInCell += lateNamesInCell.bitCount() * bytesPerSlotInSlotPlan;
    }
    bool hasWordLeftOver = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(endOfSlotsInCell) - endOfSlotsInCell >= bytesPerSlotInSlotPlan;
    for (FieldIndex field = numberOfDeclaredFields; field < numberOfFields; ++field) {
        PlacedSlot& slot = planned.slots[field];
        uint8_t granule = places.granulesOfAppendedFields[id.index()][field - numberOfDeclaredFields];
        if (granule == PlacesOfFieldsInSlotPlan::withoutSlot)
            continue;
        slot.indexInLayoutRecord = planned.numberOfRecordFields++;
        slot.holdsAccessor = facts.field({ id, field }).facts.contains(FieldFact::IsAccessor);
        slot.type = hasTypedSlots ? places.typesHeldOfAppendedFields[id.index()][field - numberOfDeclaredFields] : TypeID();
        bool isFirstLateName = !build.recordedOptions().useWideReferences() && facts.linked().takesWordLeftInCell({ id, field }) && !places.appendedFieldsThatShareGranule[id.index()].get(field - numberOfDeclaredFields);
        if ((hasWordLeftOver && isFirstLateName) || ((lateNamesLieInCell || lateNamesInCell.get(field - numberOfDeclaredFields)) && granule < maxExtensionGranules))
            granule = PlacesOfFieldsInSlotPlan::inCell;
        if (granule == PlacesOfFieldsInSlotPlan::inCell) {
            layAppendedFieldInCellForFields(program, facts, { id, field }, static_cast<uint32_t>(end), slot);
            end += bytesPerSlotInSlotPlan;
            continue;
        }
        RELEASE_ASSERT(granule < maxExtensionGranules);
        slot.storage = SlotStorage::OutOfLine;
        slot.offset = offsetOfFirstExtensionSlot + granule * extensionGranuleSize;
        slot.mayExistWithoutStorage = !facts.linked().isAppendedFieldRequiredBySomeWrittenType({ id, field }) && facts.field({ id, field }).formOfValue.isEmpty();
        slot.mayTakeWordLeftInCell = isFirstLateName;
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

MentionedKeys findMentionedKeys(const Program&, const Facts&, const ParallelLoops&);
void planNames(const Program&, const LayoutsByNames&, const MentionedKeys&, BuildContext&, SlotPlanParts&);
Vector<ClosureForm> chooseClosureForms(const Program&, const LinkTables&);
BitVector findFunctionsWhoseClosuresGetOnlyListedNames(const Program&, const LinkTables&);
BitVector findFunctionsWhosePrototypePropertyStaysAsMade(const Program&, const LinkTables&);

KeyOrders computeKeyOrders(const Program&, const LinkTables&, const ThinSummaries&, const BodyIndexTable&, Mechanisms, std::span<const PlannedLayout>, BuildContext&);

StorageOfLayouts chooseStorageOfLayouts(const Program&, const Facts&, const ParallelLoops&);
SlotPlanParts computeSlotPlanParts(Program&, const Facts&, BuildContext&, const StorageOfLayouts&, KeyOrders&, PromotedEnvironments&);
static bool holdsNumbersThatMayBeFractionsInSlotPlan(const Type& held)
{
    return held.isNumberOrNullish() && !held.isInt32OrNullish();
}

static void dropTypesOfFractionsLeftBoxedInSlotPlan(const Program& program, BuildContext& build, SlotPlanParts& parts)
{
    std::atomic<uint64_t> numberOfFieldsOutOfLine { 0 };
    forEachRangeOfIndicesInParallel(build.parallelLoops(), parts.layouts.size(), [&](size_t first, size_t end) {
        uint64_t numberOutOfLineInRange = 0;
        for (PlannedLayout& layout : parts.layouts.mutableSpan().subspan(first, end - first)) {
            for (PlacedSlot& slot : layout.slots) {
                if (slot.type && slot.representation == SlotRepresentation::JSValue && holdsNumbersThatMayBeFractionsInSlotPlan(program.types.at(slot.type)))
                    slot.type = { };
                numberOutOfLineInRange += slot.isOutOfLine();
            }
        }
        numberOfFieldsOutOfLine.fetch_add(numberOutOfLineInRange, std::memory_order_relaxed);
    });
    build.statistics().count(StatisticID::FieldsOutOfLine, numberOfFieldsOutOfLine.load(std::memory_order_relaxed));
}

void holdDoorsToProofsInSlotPlan(Program&, const Facts&, const ParallelLoops&, SlotPlanParts&);
void holdDoorsToProofsInSlotPlan(Program& program, const Facts& facts, const ParallelLoops& loops, SlotPlanParts& parts)
{
    static constexpr uint32_t layoutsInRange = 256;
    uint32_t numberOfLayouts = static_cast<uint32_t>(parts.layouts.size());
    Vector<Vector<FieldReference>> fieldsWithDoorsInRanges((numberOfLayouts + layoutsInRange - 1) / layoutsInRange);
    {
        ArenasOfWorkers arenas(program.types, loops);
        forEachIndexInParallel(loops, static_cast<unsigned>(fieldsWithDoorsInRanges.size()), [&](unsigned range, unsigned worker) {
            TypeArena& types = arenas.of(worker);
            uint32_t endOfRange = std::min(numberOfLayouts, (range + 1) * layoutsInRange);
            for (uint32_t index = range * layoutsInRange; index < endOfRange; ++index) {
                LayoutKind kind = program.layouts[index].kind;
                bool hasDoors = kind == LayoutKind::Record || kind == LayoutKind::Instance || kind == LayoutKind::Container;
                bool mayBeStable = (kind == LayoutKind::Record || kind == LayoutKind::Instance) && !program.layouts[index].has(LayoutFlags::IsConstructorFunction);
                PlannedLayout& planned = parts.layouts[index];
                for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
                    PlacedSlot& slot = planned.slots[field];
                    FieldReference reference { LayoutID(index), field };
                    if (const LinkedCell* cell = mayBeStable ? facts.linked().findCellOfField(reference) : nullptr)
                        slot.isStable = cell->everyWriterIsKnown && !cell->isWrittenAfterConstruction && cell->isFilledAtEveryRead;
                    TypeID proven = facts.linked().provenOfField(reference);
                    slot.isStable &= proven || !slot.type;
                    if (!proven)
                        continue;
                    slot.everyStoreFits = !slot.type || types.isSubset(proven, slot.type);
                    slot.isStable &= slot.everyStoreFits;
                    slot.isStableWithoutNullish = slot.isStable && !(types.at(proven).primitives & (Primitive::UndefinedOrNull | Primitive::Absent));
                    if (!slot.everyStoreFits || !hasDoors || !slot.indexInLayoutRecord || !slot.hasStorage() || slot.holdsAccessor || slot.deferredFunction)
                        continue;
                    fieldsWithDoorsInRanges[range].append(reference);
                }
            }
        });
    }

    TypeArena types(program.types, ArenaKeeps::InProgram);
    for (const LinkedAccessorsOfField& laid : facts.linked().accessorsExpectedInFields) {
        PlannedLayout& planned = parts.layouts[laid.field.layout.index()];
        if (!laid.getterIsProven || laid.field.field >= planned.numberOfFields())
            continue;
        if (PlacedSlot& slot = planned.slots[laid.field.field]; slot.holdsAccessor && slot.indexInLayoutRecord && slot.hasStorage())
            slot.type = types.forFunction(laid.getter);
    }
    std::array<HashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>, 2> askedOfProvenInSlot;
    for (const Vector<FieldReference>& fieldsWithDoors : fieldsWithDoorsInRanges) {
        for (FieldReference reference : fieldsWithDoors) {
            PlacedSlot& slot = parts.layouts[reference.layout.index()].slots[reference.field];
            TypeID proven = facts.linked().provenOfField(reference);
            uint64_t key = static_cast<uint64_t>(proven.bits()) << 32 | slot.type.bits();
            const LinkedCell* cell = facts.linked().findCellOfField(reference);
            bool keepsItsTag = cell && holdsTagThatNarrowSlotForgets(types, *cell);
            bool reachesUnseenCode = reference.layout.index() >= facts.linked().layouts.size() || facts.linked().layout(reference.layout).reachesUnseenCode;
            bool isStoredByNameWithoutTest = !keepsItsTag && (reachesUnseenCode || slot.representation != SlotRepresentation::OptionalInt32) && (slot.representation == SlotRepresentation::OptionalInt32 || slot.representation == SlotRepresentation::OptionalDouble || slot.representation == SlotRepresentation::OptionalBoolean);
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

static bool repeatsItsBaseInSlotPlan(const Program& program, const Facts& facts, LayoutID id)
{
    const Layout& layout = program.at(id);
    LayoutID base = baseWhoseFieldListIsCopiedWhole(program, id);
    if (!base || layout.kind != LayoutKind::Instance || program.at(base).kind != LayoutKind::Instance || layout.supertypes.size() != 1 || layout.supertypes[0] != base)
        return false;
    if (facts.saysOfInheritedFieldsWhatBaseSays(id))
        return true;
    uint32_t numberAbove = static_cast<uint32_t>(program.at(base).fields.size());
    for (const LinkTables* tables : { &facts.linked(), &facts.held() }) {
        if (id.index() + 1 >= tables->firstFieldOfLayout.size() || base.index() + 1 >= tables->firstFieldOfLayout.size())
            return false;
        if (tables->firstFieldOfLayout[id.index() + 1] - tables->firstFieldOfLayout[id.index()] != layout.fields.size() || tables->firstFieldOfLayout[base.index() + 1] - tables->firstFieldOfLayout[base.index()] != numberAbove)
            return false;
    }
    for (uint32_t field = 0; field < numberAbove; ++field) {
        FieldReference below { id, layout.ownFieldCount + field };
        FieldReference above { base, field };
        if (facts.field(below) != facts.field(above) || facts.linked().field(below) != facts.linked().field(above) || facts.held().field(below) != facts.held().field(above))
            return false;
    }
    return true;
}

FieldReference fieldAboveLayoutThatRepeats(const Program& program, std::span<const LayoutID> nearestLayoutsAboveThatDoNotRepeat, FieldReference field)
{
    const Layout& layout = program.at(field.layout);
    LayoutID above = nearestLayoutsAboveThatDoNotRepeat[field.layout.index()];
    uint32_t numberDeclaredBelow = static_cast<uint32_t>(layout.fields.size() - program.at(above).fields.size());
    return field.field >= numberDeclaredBelow ? FieldReference { above, field.field - numberDeclaredBelow } : declarationOf(program, field);
}

StorageOfLayouts chooseStorageOfLayouts(const Program& program, const Facts& facts, const ParallelLoops& loops)
{
    static_assert(!(indicesInRangeOfLoop % 64));
    const LinkTables& linked = facts.linked();
    StorageOfLayouts result;
    result.layoutsNeverAllocated = findLayoutsNeverAllocatedForSlotPlan(program, linked, loops);
    result.layoutsWithoutDirectInstances.ensureSize(program.layouts.size());
    for (size_t index = 0; index < std::min<size_t>(program.layouts.size(), linked.layouts.size()); ++index) {
        if (!linked.layouts[index].hasDirectInstances)
            result.layoutsWithoutDirectInstances.quickSet(index);
    }
    for (const TaggedMaker& maker : linked.taggedMakers) {
        for (const TaggedLayout& member : program.taggedFamilies[maker.family].members) {
            if (member.layout.index() < linked.layouts.size() && linked.layout(member.layout).isAllocated)
                result.layoutsNeverAllocated.quickClear(member.layout.index());
        }
    }
    result.layouts.fill(StorageOfLayout { }, program.layouts.size());
    forEachRangeOfIndicesInParallel(loops, program.layouts.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            result.layouts[index] = chooseStorageOfLayoutForSlotPlan(program.layouts[index], result.layoutsNeverAllocated.quickGet(index));
    });
    result.layoutsThatRepeatTheirBases.ensureSize(program.layouts.size());
    forEachRangeOfIndicesInParallel(loops, program.layouts.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            LayoutID base = writtenBaseOf(program.layouts[index]);
            if (base && result.layouts[index].storage == LayoutStorage::None && result.layouts[base.index()].storage == LayoutStorage::None && repeatsItsBaseInSlotPlan(program, facts, LayoutID(static_cast<uint32_t>(index))))
                result.layoutsThatRepeatTheirBases.quickSet(index);
        }
    });
    if (!result.layoutsThatRepeatTheirBases.isEmpty())
        result.nearestLayoutsAboveThatDoNotRepeat.fill(LayoutID(), program.layouts.size());
    Vector<uint32_t, 16> row;
    for (size_t below : result.layoutsThatRepeatTheirBases) {
        auto current = static_cast<uint32_t>(below);
        for (; result.layoutsThatRepeatTheirBases.quickGet(current) && !result.nearestLayoutsAboveThatDoNotRepeat[current]; current = writtenBaseOf(program.layouts[current]).index())
            row.append(current);
        LayoutID above = result.layoutsThatRepeatTheirBases.quickGet(current) ? result.nearestLayoutsAboveThatDoNotRepeat[current] : LayoutID(current);
        while (!row.isEmpty())
            result.nearestLayoutsAboveThatDoNotRepeat[row.takeLast()] = above;
    }
    return result;
}

SlotPlanParts computeSlotPlanParts(Program& program, const Facts& facts, BuildContext& build, const StorageOfLayouts& storageOfLayouts, KeyOrders& keyOrders, PromotedEnvironments& promotedEnvironments)
{
    unsigned numberOfLayouts = static_cast<unsigned>(program.layouts.size());
    SlotPlanParts parts;
    parts.layoutsNeverAllocated = storageOfLayouts.layoutsNeverAllocated;
    parts.layoutsWithoutDirectInstances = storageOfLayouts.layoutsWithoutDirectInstances;
    parts.layoutsWithoutDirectInstances.merge(parts.layoutsNeverAllocated);
    std::span<const StorageOfLayoutInSlotPlan> storage = storageOfLayouts.layouts.span();
    bool hasTypedSlots = hasTypedSlotsInSlotPlan(facts);
    const Families* families = withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::TypedOperations) ? &facts.families() : nullptr;
    MentionedKeys mentioned;
    PlacesOfFieldsInSlotPlan places;
    BitVector layoutsWithSlotsThatStoresMayEmpty;
    LayoutsBelowTheirBases layoutsBelowTheirBases;
    runLanes(build.parallelLoops(), {
        [&] {
            mentioned = findMentionedKeys(program, facts, build.parallelLoops());
            numberRowsForSlotPlan(program, places);
            layoutsWithSlotsThatStoresMayEmpty = lookAtFieldsAheadOfPlacesForSlotPlan(program, facts, build.parallelLoops(), storageOfLayouts.layoutsThatRepeatTheirBases, hasTypedSlots, places);
        },
        [&] { layoutsBelowTheirBases = orderLayoutsBelowTheirBases(program); },
    });

    parts.layouts.grow(numberOfLayouts);
    GroupsOfLayoutsForFields groupsForFields;
    UnitsOfFields unitsOfFields;
    EnvironmentsHandedBack environmentsHandedBack;
    runLanes(build.parallelLoops(), {
        [&] {
            const CellSizesForFields cellSizes(build);
            placeFieldsForSlotPlan(program, facts, layoutsWithSlotsThatStoresMayEmpty, families, layoutsBelowTheirBases, storage, storageOfLayouts, hasTypedSlots, cellSizes, places);
            if (hasTypedSlots)
                chooseFormsOfPlacesForSlotPlan(program, facts, facts.linked(), families, storage, storageOfLayouts.layoutsThatRepeatTheirBases, places);
            forEachIndexInParallel(build.parallelLoops(), numberOfLayouts, [&](unsigned index) {
                parts.layouts[index] = planLayoutForSlotPlan(program, facts, mentioned, places, cellSizes, storage[index], LayoutID(index), build);
            });
        },
        [&] {
            groupsForFields = groupLayoutsForFields(program, facts.families());
            unitsOfFields = collectUnitsOfFields(program, facts.linked(), storageOfLayouts, groupsForFields);
        },
        [&] { environmentsHandedBack = findEnvironmentsHandedBack(program, facts, build); },
        [&] {
            parts.closureForms = chooseClosureForms(program, facts.linked());
            parts.functionsWhoseClosuresGetOnlyListedNames = findFunctionsWhoseClosuresGetOnlyListedNames(program, facts.linked());
            parts.functionsWhosePrototypePropertyStaysAsMade = findFunctionsWhosePrototypePropertyStaysAsMade(program, facts.linked());
        },
    });
    if (build.diagnostics().hasErrors())
        return parts;

    {
        StepTimes times(build, nameOf(Stage::PlanSlots));
        promotedEnvironments = planPromotedEnvironments(program, facts, parts, environmentsHandedBack.takenByClosuresMadeForCallsAlone.span());
        leaveClosuresOfLiteralsUnmade(program, facts, promotedEnvironments, build.recordedOptions().useWideReferences(), parts);
        times.didFinish("plan: promoted environments, closures of literals"_s);
    }
    runLanes(build.parallelLoops(), {
        [&] {
            StepTimes times(build, nameOf(Stage::PlanSlots));
            environmentsHandedBack.all.removeAllMatching([&](LayoutID id) {
                return promotedEnvironments.isPromoted(id);
            });
            layEnvironmentsHandedBack(build, environmentsHandedBack.all.span(), parts);
            chooseStorageOfFields(program, facts, build, parts, layoutsBelowTheirBases, WTF::move(groupsForFields), WTF::move(unitsOfFields));
            dropTypesOfFractionsLeftBoxedInSlotPlan(program, build, parts);
            std::optional<LayoutsByNames> writersAlone;
            if (!facts.mechanisms().contains(Mechanism::Guesses))
                writersAlone = LayoutsByNames::thatListsWritersAlone(program, facts.summaries());
            planNames(program, writersAlone ? *writersAlone : facts.layoutsByNames(), mentioned, build, parts);
            times.didFinish("plan: lane of storage of fields, names"_s);
        },
        [&] {
            StepTimes times(build, nameOf(Stage::PlanSlots));
            keyOrders = computeKeyOrders(program, facts.linked(), facts.summaries(), facts.bodies(), facts.mechanisms(), parts.layouts.span(), build);
            times.didFinish("plan: lane of key orders"_s);
        },
    });
    return parts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
