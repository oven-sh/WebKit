/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLayout.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTRepresentationRules.h"
#include "AOTSlotRepresentation.h"
#include "AOTSweep.h"
#include "AOTThinSummary.h"
#include "AOTTypeArena.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkNumberForms.h"
#include "AOTWalkKnownCalls.h"
#include "AOTWalkKnown.h"
#include "AOTWalkBirths.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet accessesByNameWithTypedForms { Opcode::PropGet, Opcode::PropSet, Opcode::PropHas, Opcode::PropDefine };
static constexpr OpcodeSet accessesUnderPrivateNames = opcodesOfFamily(OperationFamily::PrivateAccess);
static_assert(accessesUnderPrivateNames == OpcodeSet { Opcode::PrivateGet, Opcode::PrivateSet, Opcode::PrivateDefine, Opcode::PrivateHas });
static constexpr OpcodeSet operationsWithTypedAccesses = accessesByNameWithTypedForms | accessesUnderPrivateNames
    | OpcodeSet { Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::GetEnvironmentField, Opcode::PutEnvironmentField, Opcode::SuperGet, Opcode::SuperSet };

FamilyID familyAroundSetOfLayouts(const Walker&, LayoutSetID);

static constexpr size_t mostLayoutsWithoutFamilyAround = 4;

namespace {

struct ReceiversOfTypedAccess {
    LayoutSetID set;
    FamilyID family;
    FamilyID familyAround;
    LayoutsOwnedOrLent<8> members;
    OptionSet<LayoutChange> possibleChanges;
};

enum class CoverageOfTypedAccess : uint8_t { NoReceiver, SomeReceivers, EveryReceiver };

struct FieldOfTypedAccess {
    CoverageOfTypedAccess coverage { CoverageOfTypedAccess::NoReceiver };
    FieldReference place;
    bool liesInOneSlot { false };
    bool liesInOneSlotOfReceiversThatHaveIt { false };
    bool isMovedOutByNoStore { false };
    bool isNeverRedefined { true };
    bool isRequired { true };
    bool isStoredByProgram { true };
    bool takesStoredValue { true };
    bool isNamedAbove { false };
    bool isFilledByEveryConstruction { true };
    bool isFilledAtEveryRead { true };
    bool mayBeListedWithoutStorage { false };
    bool isAppended { false };
    bool isAppendedByConstructors { true };
    bool isLaidLate { false };
    bool isOfBirth { true };
    bool markAnswersForSlot { true };
    bool markAnswersForValue { true };
    bool liesInFormWithMarks { true };
    bool liesInFormOfNumbersOrBooleans { true };
    bool liesInOptionalBoolean { true };
    bool isWrittenWithoutUndefinedAndNull { true };
    LayoutID lastReceiverThatListsIt;
    bool severalReceiversListIt { false };
};

class WordsListedOnce {
public:
    template<size_t inlineCapacity>
    ALWAYS_INLINE void appendUnlessListed(Vector<MemberConditionRecord, inlineCapacity>& list, const MemberConditionRecord& condition)
    {
        appendUnlessListed(list, condition, [](const MemberConditionRecord& listed) {
            static_assert(numberOfEnumerators<MemberConditionKind> <= 8);
            return static_cast<uint64_t>(listed.key.bits()) << 32 | static_cast<uint64_t>(listed.layout.index()) << 4 | static_cast<uint64_t>(listed.kind) << 1 | static_cast<uint64_t>(listed.side);
        });
    }

    template<size_t inlineCapacity>
    ALWAYS_INLINE void appendUnlessListed(Vector<LayoutID, inlineCapacity>& list, LayoutID layout)
    {
        appendUnlessListed(list, layout, [](LayoutID listed) {
            return static_cast<uint64_t>(listed.index());
        });
    }

private:
    template<typename T, size_t inlineCapacity>
    ALWAYS_INLINE void appendUnlessListed(Vector<T, inlineCapacity>& list, const T& entry, const Invocable<uint64_t(const T&)> auto& wordOf)
    {
        constexpr size_t fewEnoughToCompare = 8;
        if (list.size() < fewEnoughToCompare) [[likely]] {
            if (!list.contains(entry))
                list.append(entry);
            return;
        }
        appendUnlessWordIsListed(list, entry, wordOf);
    }

    template<typename T, size_t inlineCapacity>
    NEVER_INLINE void appendUnlessWordIsListed(Vector<T, inlineCapacity>& list, const T& entry, const Invocable<uint64_t(const T&)> auto& wordOf)
    {
        for (; m_numberOfEntriesTaken < list.size(); ++m_numberOfEntriesTaken)
            m_words.add(wordOf(list[m_numberOfEntriesTaken]));
        if (!m_words.add(wordOf(entry)).isNewEntry)
            return;
        list.append(entry);
        ++m_numberOfEntriesTaken;
    }

    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_words;
    size_t m_numberOfEntriesTaken { 0 };
};

struct KeptByTypedAccesses final : Walker::KeptForBuild {
    Vector<std::optional<std::optional<ReceiversOfTypedAccess>>> receiversOfFamilies;
    HashMap<std::pair<uint64_t, uint32_t>, FieldOfTypedAccess> fieldsOfFamilies;
    struct MembersByKey {
        uint32_t numberOfKeysAsked { 0 };
        bool areListed { false };
        Vector<uint32_t> askedAboutEveryKey;
        HashMap<uint32_t, Vector<uint32_t, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> withKey;
    };
    Vector<MembersByKey> membersOfFamiliesByKey;
    struct AnswersAbove {
        bool somebodyAnswersForEveryMember { false };
        Vector<LayoutID, 2> classesWhoseGuardsAnswer;
    };
    HashMap<uint64_t, AnswersAbove> answersAboveFamilies;
    HashMap<std::pair<uint64_t, uint64_t>, FieldOfTypedAccess> fieldsOfSets;
    HashMap<std::pair<uint64_t, uint64_t>, AnswersAbove> answersAboveSets;
    HashMap<uint64_t, bool> whetherNothingAnswersAbove;
    struct RowsAboveReceivers {
        enum class Kind : uint8_t { NotLookedAtYet, EveryMemberListsWhatStandsAbove, SomeMemberSaysNoUnderEveryKey, AreAskedOneByOne };
        Kind kind { Kind::NotLookedAtYet };
        size_t membersAskedOneByOne { 0 };
        Vector<LinkTables::StretchOfRows> stretchesOfInstancesInRows;
    };
    HashMap<uint32_t, RowsAboveReceivers, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> rowsAboveReceivers;
    HashMap<uint64_t, bool> whetherEveryMemberMayBeAskedWhetherReadFindsNothing;
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> whetherEveryReceiverIsRecord;
    HashMap<uint64_t, bool> whetherNoMemberHasNameOrFindsItLaidAbove;
    struct NamesLaidOnPrototypes {
        BitVector keys;
        bool someLieUnderKeysNobodyKnows { false };
    };
    std::optional<NamesLaidOnPrototypes> namesLaidOnPrototypes;
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> whetherSomeRowOfBasesIsNotNamed;
    BitVector classesClimbedForQuestion;
    Vector<uint32_t> classesToForgetBehindQuestion;
    HashMap<uint64_t, MemberFound> membersFoundAbove;
    enum class RowAbove : uint8_t { NotAsked, IsAsked, Passes, Fails };
    Vector<RowAbove> rowsAboveClasses;
    std::optional<BitVector> keysOfMembersOfPrototypes;
    struct LayoutsThatSourcesFit {
        LayoutID onlyLayoutMade;
        Vector<LayoutID, 3> layoutsThatAllFit;
    };
    HashMap<uint32_t, LayoutsThatSourcesFit, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> layoutsThatSourcesOfCopiesFit;
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> whetherWordsOfCellAreItsProperties;
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> whetherSomeFieldMayBeLeftOut;
};

struct OperandsOfTypedAccess {
    Reference object;
    Reference stored;
    TypeID typeOfObject;
    TypeID typeOfStored;
    bool areOnlyTypes { false };
};

struct WhatChangeForbids {
    LayoutChange change;
    OptionSet<TypedAccess> forbids;
    OptionSet<TypedAccess> allowedWhereCellIsSeen;
    CellFact seenOfCell { CellFact::NotFrozen };
    OptionSet<TypedAccess> allowedWhereGuardOfReadsHolds;
};

} // anonymous namespace

static constexpr OptionSet<TypedAccess> storesOfTypedAccesses { TypedAccess::StoresIntoFilledSlot, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::DefinesSlotThatMayBeEmpty };
static constexpr OptionSet<TypedAccess> accessesToSlotsThatMayBeEmpty { TypedAccess::ReadsSlotThatMayBeEmpty, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::DefinesSlotThatMayBeEmpty };
static constexpr WhatChangeForbids whatChangesForbid[] {
    { LayoutChange::IsFrozen, storesOfTypedAccesses, storesOfTypedAccesses, CellFact::NotFrozen },
    { LayoutChange::GainsProperty, { TypedAccess::FindsNoProperty }, { TypedAccess::FindsNoProperty }, CellFact::NoExtraProperties },
    { LayoutChange::LosesProperty, { }, { } },
    { LayoutChange::GetsObjectAbove, { TypedAccess::ReadsSlotThatMayBeEmpty, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::FindsNoProperty }, { TypedAccess::ReadsSlotThatMayBeEmpty, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::FindsNoProperty }, CellFact::HasLayoutPrototype, { } },
    { LayoutChange::ChangesPrototype, { TypedAccess::FindsMethodAbove }, { TypedAccess::FindsMethodAbove }, CellFact::HasLayoutPrototype, { } },
    { LayoutChange::GetsAccessor, accessesToSlotsThatMayBeEmpty | OptionSet<TypedAccess> { TypedAccess::FindsMethodAbove }, accessesToSlotsThatMayBeEmpty, CellFact::NoFieldOutsideSlot, { TypedAccess::FindsMethodAbove } },
    { LayoutChange::GetsAccessor, { TypedAccess::FindsNoProperty }, { TypedAccess::FindsNoProperty }, CellFact::NoExtraProperties },
};
static constexpr CellFact factsThatLooksAtCellShow[] { CellFact::NotFrozen, CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype, CellFact::NoExtraProperties };
static constexpr OpcodeSet storesByNameIntoSlots { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet accessesThatMayStoreUnderNameForOrderWord { Opcode::PropSet, Opcode::PropDefine, Opcode::ElemSet, Opcode::ElemDefine };

bool possibleChangesForbid(OptionSet<LayoutChange> possibleChanges, TypedAccess access, OptionSet<CellFact> knownOfCell, bool guardOfReadsHolds)
{
    return std::ranges::any_of(whatChangesForbid, [&](const WhatChangeForbids& row) {
        if (!possibleChanges.contains(row.change) || !row.forbids.contains(access))
            return false;
        if (guardOfReadsHolds && row.allowedWhereGuardOfReadsHolds.contains(access))
            return false;
        return !row.allowedWhereCellIsSeen.contains(access) || !knownOfCell.contains(row.seenOfCell);
    });
}

static const LinkTables& linkTablesForTypedAccesses(const Walker& walker)
{
    return walker.facts().linked();
}

static const Families& familiesForTypedAccesses(const Walker& walker)
{
    return walker.facts().families();
}

static OptionSet<LayoutChange> possibleChangesForTypedAccess(const LinkTables& link, LayoutID layout)
{
    return layout.index() < link.layouts.size() ? link.layout(layout).possibleChanges : LinkedLayout { }.possibleChanges;
}

static ALWAYS_INLINE LinkedCell cellOfFieldForTypedAccess(const LinkTables& link, FieldReference field)
{
    const LinkedCell* cell = link.findCellOfField(field);
    return cell ? *cell : LinkedCell { };
}

static bool isLookedUpAsNameForTypedAccess(const Program& program, PropertyKeyID key)
{
    return program.types.isLookedUpAsName(key);
}

static bool findsMemberWithoutCodeForTypedAccess(const Program& program, const LinkTables& link, PropertyKeyID key)
{
    return isLookedUpAsNameForTypedAccess(program, key) && !link.lookupUnderKeyMayRunPreparation(program, key);
}

static bool objectsMayHoldNamesBesideFieldsForTypedAccess(const LinkTables& link, LayoutID layout)
{
    return layout.index() >= link.layouts.size() || link.layout(layout).isFilledUnderComputedKeys || link.layout(layout).possibleChanges.contains(LayoutChange::GainsProperty);
}

MemberFound findMemberAboveObjectsOf(const Program& program, const LinkTables& link, LayoutID layout, PropertyKeyID key)
{
    if (!findsMemberWithoutCodeForTypedAccess(program, link, key) || !isFoundByShapeTest(program.at(layout)) || findField(program, layout, key))
        return { };
    if (possibleChangesForbid(possibleChangesForTypedAccess(link, layout), TypedAccess::FindsMethodAbove) || std::ranges::contains(link.appendedKeysOf(layout), key))
        return { };
    MemberFound found = link.findMember(program, layout, ClassSide::Prototype, key);
    if (found.kind == MemberFound::Kind::Accessor && objectsMayHoldNamesBesideFieldsForTypedAccess(link, layout))
        return { };
    return found;
}

std::optional<FindsNothing> findsNothingAboveObjectsOf(const Program& program, const LinkTables& link, LayoutID layout, PropertyKeyID key)
{
    if (!findsMemberWithoutCodeForTypedAccess(program, link, key) || !isFoundByShapeTest(program.at(layout)) || possibleChangesForbid(possibleChangesForTypedAccess(link, layout), TypedAccess::FindsMethodAbove))
        return std::nullopt;
    return link.findsNothing(program, layout, ClassSide::Prototype, key, Of::HolderAndAbove);
}

LayoutID classOfClassObject(const Program& program, FunctionID function)
{
    const Function& record = program.at(function);
    if (!record.constructedClass || !isClassConstructorOf(record.kind) || program.at(record.constructedClass).constructor != function)
        return { };
    return record.constructedClass;
}

static bool classStaysAsWrittenForTypedAccess(const LinkTables& link, LayoutID classLayout)
{
    return !possibleChangesForbid(possibleChangesForTypedAccess(link, classLayout), TypedAccess::FindsMethodAbove);
}

LayoutID classProvenAbove(const Program& program, const LinkTables& link, LayoutID classLayout)
{
    return classStaysAsWrittenForTypedAccess(link, classLayout) ? program.at(classLayout).base : LayoutID();
}

MemberFound findMemberOfNamedObject(const Program& program, const LinkTables& link, const ObjectRecord& object, PropertyKeyID key)
{
    if (!findsMemberWithoutCodeForTypedAccess(program, link, key))
        return { };
    LayoutID classLayout;
    ClassSide side = ClassSide::Prototype;
    if (object.kind == ObjectKind::Prototype)
        classLayout = LayoutID(object.payload);
    else if (object.kind == ObjectKind::Closure) {
        classLayout = classOfClassObject(program, FunctionID(object.payload));
        side = ClassSide::Static;
    }
    if (!classLayout || !classStaysAsWrittenForTypedAccess(link, classLayout))
        return { };
    MemberFound found = link.findMember(program, classLayout, side, key);
    if (side == ClassSide::Prototype || found.kind == MemberFound::Kind::Unknown)
        return found;
    for (LayoutID current = classLayout; current; current = program.at(current).base) {
        if (program.at(current).findStaticField(key))
            return { };
    }
    return found;
}

static KeptByTypedAccesses& keptByTypedAccesses(Walker& walker)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByTypedAccesses();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<KeptByTypedAccesses>();
    return static_cast<KeptByTypedAccesses&>(*kept);
}

static bool addMembersInUseForTypedAccess(Walker&, ReceiversOfTypedAccess&);

static const std::optional<ReceiversOfTypedAccess>& receiversOfFamilyForTypedAccess(Walker& walker, FamilyID family)
{
    auto& receiversOfFamilies = keptByTypedAccesses(walker).receiversOfFamilies;
    if (receiversOfFamilies.isEmpty())
        receiversOfFamilies.grow(familiesForTypedAccesses(walker).size());
    std::optional<std::optional<ReceiversOfTypedAccess>>& known = receiversOfFamilies[family.index()];
    if (!known) {
        ReceiversOfTypedAccess receivers;
        receivers.set = familiesForTypedAccesses(walker).layoutsOf(family);
        receivers.family = family;
        bool someIsInUse = addMembersInUseForTypedAccess(walker, receivers);
        known = someIsInUse ? std::optional<ReceiversOfTypedAccess>(WTF::move(receivers)) : std::nullopt;
    }
    return *known;
}

static std::optional<ReceiversOfTypedAccess> findReceiversAmongLayoutsForTypedAccess(Walker& walker, LayoutSetID set)
{
    std::optional<ReceiversOfTypedAccess> found(std::in_place);
    ReceiversOfTypedAccess& receivers = *found;
    receivers.set = set;
    if (!holdsOneMember(receivers.set)) {
        receivers.family = familiesForTypedAccesses(walker).familyOf(receivers.set);
        if (receivers.family) {
            const std::optional<ReceiversOfTypedAccess>& kept = receiversOfFamilyForTypedAccess(walker, receivers.family);
            if (!kept) {
                found.reset();
                return found;
            }
            receivers.set = kept->set;
            receivers.family = kept->family;
            receivers.familyAround = kept->familyAround;
            receivers.members = LayoutsOwnedOrLent<8>::lentBy(kept->members);
            receivers.possibleChanges = kept->possibleChanges;
            return found;
        }
        receivers.familyAround = familyAroundSetOfLayouts(walker, receivers.set);
        if (!receivers.familyAround && walker.types().at(receivers.set).size() > mostLayoutsWithoutFamilyAround) {
            found.reset();
            return found;
        }
    }
    if (!addMembersInUseForTypedAccess(walker, receivers))
        found.reset();
    return found;
}

static std::optional<ReceiversOfTypedAccess> findReceiversOfTypedAccess(Walker& walker, TypeID proven)
{
    if (!proven || holdsOnlyPrimitives(proven))
        return std::nullopt;
    const Type& type = walker.types().recordOf(proven);
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return std::nullopt;
    return findReceiversAmongLayoutsForTypedAccess(walker, type.layouts);
}

static bool addMembersInUseForTypedAccess(Walker& walker, ReceiversOfTypedAccess& receivers)
{
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    if (holdsOneMember(receivers.set)) {
        LayoutID only(positionOf(receivers.set));
        if (!plan.isInUse(only) || !plan.slots().holdsCompiledObjects(only) || !isFoundByShapeTest(program.at(only)))
            return false;
        receivers.members.append(only);
        receivers.possibleChanges.add(possibleChangesForTypedAccess(link, only));
        return true;
    }
    static constexpr size_t mostMembersAskedWithoutFamily = 16;
    std::span<const LayoutID> membersOfSet = membersOf(types, receivers.set);
    if (receivers.familyAround && membersOfSet.size() > mostMembersAskedWithoutFamily) {
        if (const std::optional<ReceiversOfTypedAccess>& ofFamily = receiversOfFamilyForTypedAccess(walker, receivers.familyAround)) {
            receivers.members.reserveInitialCapacity(membersOfSet.size());
            bool someMemberMayChange = !ofFamily->possibleChanges.isEmpty();
            for (LayoutID member : membersOfSet) {
                if (!plan.isInUse(member))
                    continue;
                receivers.members.append(member);
                if (someMemberMayChange)
                    receivers.possibleChanges.add(possibleChangesForTypedAccess(link, member));
            }
            return !receivers.members.isEmpty();
        }
    }
    for (LayoutID member : membersOfSet) {
        if (!plan.isInUse(member))
            continue;
        if (!plan.slots().holdsCompiledObjects(member) || !isFoundByShapeTest(program.at(member)))
            return false;
        receivers.members.append(member);
        receivers.possibleChanges.add(possibleChangesForTypedAccess(link, member));
    }
    return !receivers.members.isEmpty();
}

static FieldOfTypedAccess describeFieldAnewForTypedAccess(Walker&, const ReceiversOfTypedAccess&, PropertyKeyID, TypeID stored, bool goesByKeyOrder);

static bool unitMayMentionClosuresMadeWhenReadForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    const SlotPlan& slots = walker.plan().slots();
    if (!slots.someSlotUnderKeyHoldsMark(key)) [[likely]]
        return true;
    return std::ranges::all_of(receivers.members, [&](LayoutID member) {
        std::optional<FieldIndex> field = slots.findField(walker.program(), member, key);
        FunctionID made = field ? slots.slot(member, *field).deferredFunction : FunctionID();
        return !made || walker.facts().linked().mayBeMentionedBy(walker.unit(), made);
    });
}

static FieldOfTypedAccess describeFieldOfTypedAccess(Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, TypeID stored, bool goesByKeyOrder = false)
{
    if (!unitMayMentionClosuresMadeWhenReadForTypedAccess(walker, receivers, key)) [[unlikely]] {
        FieldOfTypedAccess field;
        field.coverage = CoverageOfTypedAccess::SomeReceivers;
        return field;
    }
    if (liesInThread(stored) || liesInThread(key))
        return describeFieldAnewForTypedAccess(walker, receivers, key, stored, goesByKeyOrder);
    if (!receivers.family) {
        TypeArena::SetOfRound round = walker.types().setOfRound(receivers.set);
        if (!round.isKeptForBuild && liesInThread(receivers.set))
            return describeFieldAnewForTypedAccess(walker, receivers, key, stored, goesByKeyOrder);
        std::pair<uint64_t, uint64_t> asked { static_cast<uint64_t>(round.word) << 32 | key.bits(), static_cast<uint64_t>(stored.bits()) << 1 | goesByKeyOrder };
        auto& fieldsOfSets = keptByTypedAccesses(walker).fieldsOfSets;
        if (auto known = fieldsOfSets.find(asked); known != fieldsOfSets.end())
            return known->value;
        FieldOfTypedAccess field = describeFieldAnewForTypedAccess(walker, receivers, key, stored, goesByKeyOrder);
        fieldsOfSets.add(asked, field);
        return field;
    }
    std::pair<uint64_t, uint32_t> asked { static_cast<uint64_t>(receivers.family.index() + 1) << 33 | static_cast<uint64_t>(key.bits()) << 1 | goesByKeyOrder, stored.bits() };
    auto& fieldsOfFamilies = keptByTypedAccesses(walker).fieldsOfFamilies;
    if (auto known = fieldsOfFamilies.find(asked); known != fieldsOfFamilies.end())
        return known->value;
    FieldOfTypedAccess field = describeFieldAnewForTypedAccess(walker, receivers, key, stored, goesByKeyOrder);
    fieldsOfFamilies.add(asked, field);
    return field;
}

static ALWAYS_INLINE bool listMembersThatMayHaveKeyForTypedAccess(Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, Vector<uint32_t, 16>& places)
{
    static constexpr size_t mostMembersAskedOneByOne = 16;
    static constexpr uint32_t mostKeysAskedOfEveryMember = 8;
    FamilyID family = receivers.family ? receivers.family : receivers.familyAround;
    if (!family || receivers.members.size() <= mostMembersAskedOneByOne || liesInThread(key))
        return false;
    const std::optional<ReceiversOfTypedAccess>& ofFamily = receiversOfFamilyForTypedAccess(walker, family);
    if (!ofFamily)
        return false;
    std::span<const LayoutID> membersOfFamily = ofFamily->members.span();
    auto& membersOfFamiliesByKey = keptByTypedAccesses(walker).membersOfFamiliesByKey;
    if (membersOfFamiliesByKey.isEmpty())
        membersOfFamiliesByKey.grow(familiesForTypedAccesses(walker).size());
    KeptByTypedAccesses::MembersByKey& members = membersOfFamiliesByKey[family.index()];
    if (!members.areListed) {
        if (members.numberOfKeysAsked++ < mostKeysAskedOfEveryMember)
            return false;
        members.areListed = true;
        const Program& program = walker.program();
        const SlotPlan& slots = walker.plan().slots();
        for (uint32_t place = 0; place < membersOfFamily.size(); ++place) {
            const Layout& layout = program.at(membersOfFamily[place]);
            if (writtenBaseOf(layout) && layout.inheritedFields != InheritedFields::AllOfRow) {
                members.askedAboutEveryKey.append(place);
                continue;
            }
            auto list = [&](PropertyKeyID listed) {
                if (!listed)
                    return;
                Vector<uint32_t, 1>& withKey = members.withKey.add(listed.bits(), Vector<uint32_t, 1>()).iterator->value;
                if (withKey.isEmpty() || withKey.last() != place)
                    withKey.append(place);
            };
            for (const Field& declared : layout.fields)
                list(declared.key);
            for (PropertyKeyID appended : slots.layout(membersOfFamily[place]).appendedKeys)
                list(appended);
        }
    }
    auto withKey = members.withKey.find(key.bits());
    std::span<const uint32_t> found = withKey == members.withKey.end() ? std::span<const uint32_t>() : withKey->value.span();
    if (!receivers.family && members.askedAboutEveryKey.size() + found.size() >= receivers.members.size() / 4)
        return false;
    places.grow(members.askedAboutEveryKey.size() + found.size());
    std::ranges::merge(members.askedAboutEveryKey, found, places.begin());
    if (receivers.family)
        return true;
    size_t numberInSet = 0;
    for (uint32_t place : places) {
        auto inSet = std::ranges::lower_bound(receivers.members, membersOfFamily[place]);
        if (inSet != receivers.members.end() && *inSet == membersOfFamily[place])
            places[numberInSet++] = inSet - receivers.members.begin();
    }
    places.shrink(numberInSet);
    return true;
}

static FieldOfTypedAccess describeFieldAnewForTypedAccess(Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, TypeID stored, bool goesByKeyOrder)
{
    const Program& program = walker.program();
    const SlotPlan& slots = walker.plan().slots();
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    TypeArena& types = walker.types();

    FieldOfTypedAccess field;
    unsigned numberWithField = 0;
    bool isDeclaredWithoutSlot = false;
    bool holdsPlainValue = true;
    bool staysInSlot = true;
    Vector<uint32_t, 16> places;
    bool asksListedMembers = listMembersThatMayHaveKeyForTypedAccess(walker, receivers, key, places);
    size_t numberAsked = asksListedMembers ? places.size() : receivers.members.size();
    for (size_t asked = 0; asked < numberAsked; ++asked) {
        LayoutID member = receivers.members[asksListedMembers ? places[asked] : asked];
        std::optional<FieldIndex> index = slots.findField(program, member, key);
        bool isDeclared = !index && findField(program, member, key);
        if (index || isDeclared) {
            field.severalReceiversListIt |= !!field.lastReceiverThatListsIt;
            field.lastReceiverThatListsIt = member;
        }
        if (!index) {
            isDeclaredWithoutSlot |= isDeclared;
            continue;
        }
        FieldReference reference { member, *index };
        if (!numberWithField++)
            field.place = findField(program, member, key).value_or(reference);

        const Layout& layout = program.at(member);
        const Field* recordOrNull = *index < layout.fields.size() ? &layout.fields[*index] : nullptr;
        const PlacedSlot& slot = slots.slot(member, *index);

        bool mayBeListedWithoutStorage = slot.hasStorage() && slot.isOutOfLine() && slot.mayExistWithoutStorage;
        field.mayBeListedWithoutStorage |= mayBeListedWithoutStorage;
        bool mayLackStorage = !slot.hasStorage() || (mayBeListedWithoutStorage && !goesByKeyOrder);
        if (OptionSet<FieldForm> form = walker.facts().field(reference).formOfValue; !form.isEmpty())
            holdsPlainValue &= !stored && !form.contains(FieldForm::IsAccessor);
        holdsPlainValue &= (!recordOrNull || recordOrNull->form.isEmpty() || (!stored && !recordOrNull->form.contains(FieldForm::IsAccessor))) && !slot.holdsAccessor && !mayLackStorage && (slot.representation != SlotRepresentation::DeferredClosure || !stored || (slot.type && types.isSubset(stored, slot.type)));

        LinkedCell cell = cellOfFieldForTypedAccess(link, reference);
        bool takesEveryValue = slot.representation == SlotRepresentation::JSValue && (!slot.type || types.at(slot.type).isAnything());
        bool holdsBoxedNumbers = slot.representation == SlotRepresentation::JSValue && slot.type && types.at(slot.type).isNumberOrNullish();
        field.liesInFormWithMarks &= holdsBoxedNumbers || (slot.representation != SlotRepresentation::JSValue && slot.representation != SlotRepresentation::DeferredClosure && canLeaveSlot(slot.representation));
        field.liesInFormOfNumbersOrBooleans &= holdsBoxedNumbers || !traitsOf(slot.representation).holdsCell;
        field.liesInOptionalBoolean &= slot.representation == SlotRepresentation::OptionalBoolean;
        staysInSlot &= fieldStaysInSlot(program, slot) && (!recordOrNull || recordOrNull->form.isEmpty());
        field.isNeverRedefined &= walker.facts().field(reference).stability.contains(FieldStability::IsNeverRedefined);
        field.isFilledByEveryConstruction &= cell.isFilled;
        field.isFilledAtEveryRead &= cell.isFilledAtEveryRead;
        field.markAnswersForSlot &= slot.isFilledAtBirth;
        field.markAnswersForValue &= walker.plan().layout(member).holdsRawValueFromBirth(*index);
        field.isOfBirth &= cell.isOfBirth || (recordOrNull && recordOrNull->flags.contains(FieldFlags::IsInherited) && cellOfFieldForTypedAccess(link, declarationOf(program, reference)).isOfBirth);
        if (stored)
            field.takesStoredValue &= (takesEveryValue || (slot.type && types.isSubset(stored, slot.type))) && (!slot.provenType || types.isSubset(stored, slot.provenType));
        if (!recordOrNull) {
            field.isRequired &= link.isAppendedFieldRequiredBySomeWrittenType(reference);
            field.isAppended = true;
            bool isLaidLate = link.isAppendedFieldLaidLate(reference);
            field.isLaidLate |= isLaidLate;
            field.isAppendedByConstructors &= link.isAppendedFieldRequiredByTypeOfThis(reference) && !isLaidLate;
            field.isWrittenWithoutUndefinedAndNull = false;
            continue;
        }
        const Field& record = *recordOrNull;

        bool isMethodOfClass = layout.kind == LayoutKind::Instance && record.flags.contains(FieldFlags::IsMethod);
        field.isStoredByProgram &= !isMethodOfClass && !record.flags.contains(FieldFlags::IsNeverWritten);
        bool isRequiredByWrittenType = record.originLayout && record.originField && fieldAt(program, { record.originLayout, *record.originField }).flags.contains(FieldFlags::IsRequired);
        const auto& declared = (record.originLayout && record.originField ? fieldAt(program, { record.originLayout, *record.originField }) : record).declared;
        TypeID written = declared.written();
        uint16_t nullishAsWritten = written ? types.at(written).primitives & Primitive::UndefinedOrNull & ~(declared.has(PositionFlags::OmitsUndefined) ? Primitive::Undefined : 0) & ~(declared.has(PositionFlags::OmitsNull) ? Primitive::Null : 0) : 0;
        field.isWrittenWithoutUndefinedAndNull &= written && !types.at(written).isAnything() && !nullishAsWritten;
        field.isRequired &= record.flags.contains(FieldFlags::IsRequired) || isRequiredByWrittenType || layout.kind == LayoutKind::Container;
        field.isNamedAbove |= isMethodOfClass || record.flags.contains(FieldFlags::IsAlsoInPrototype);
    }

    if (!numberWithField) {
        field.coverage = isDeclaredWithoutSlot ? CoverageOfTypedAccess::SomeReceivers : CoverageOfTypedAccess::NoReceiver;
        return field;
    }
    if (numberWithField < receivers.members.size()) {
        field.coverage = CoverageOfTypedAccess::SomeReceivers;
        if (stored || isDeclaredWithoutSlot || !holdsPlainValue)
            return field;
        field.isMovedOutByNoStore = staysInSlot;
        FieldAccess access = walker.plan().fieldAccess(program, key, receivers.set, receivers.members.span(), false);
        field.liesInOneSlotOfReceiversThatHaveIt = access.coverage == FieldAccess::Coverage::SomeMembers && access.hasLocation && access.slot.hasStorage() && (!access.mayBeFilledWithoutStorage || goesByKeyOrder)
            && (!access.findsOffsetByShape || paysToFindOffsetByShape(walker, access.membersWithField.span(), key, false));
        return field;
    }
    field.coverage = CoverageOfTypedAccess::EveryReceiver;

    if (stored) {
        Type typeOfStored = types.at(stored);
        field.takesStoredValue &= typeOfStored.isJavaScriptValue() && !(typeOfStored.primitives & (Primitive::Absent | Primitive::Unspecified));
    }
    if (!holdsPlainValue)
        return field;
    field.isMovedOutByNoStore = staysInSlot;
    FieldAccess access = walker.plan().fieldAccess(program, key, receivers.set, receivers.members.span(), !!stored);
    field.liesInOneSlot = access.hasLocation && (!access.findsOffsetByShape || paysToFindOffsetByShape(walker, receivers.members.span(), key, !!stored)) && (!access.mayBeFilledWithoutStorage || goesByKeyOrder)
        && access.coverage == FieldAccess::Coverage::EveryMember && access.slot.hasStorage();
    return field;
}

static bool nothingAnswersBelowObjectPrototypeForTypedAccess(const Program& program, const LinkTables& link, LayoutID id, PropertyKeyID key)
{
    const Layout& layout = program.at(id);
    if (layout.has(LayoutFlags::IsOfCreatedObject))
        return false;
    if (layout.kind == LayoutKind::Record)
        return !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
    return link.findsNothing(program, id, ClassSide::Prototype, key, Of::HolderAndAbove).has_value();
}

static bool nothingAnswersAboveObjectsOfForTypedAccess(const Program& program, const LinkTables& link, LayoutID id, PropertyKeyID key)
{
    return !isOnObjectPrototype(program.nameClassOf(key)) && nothingAnswersBelowObjectPrototypeForTypedAccess(program, link, id, key);
}

static std::optional<uint64_t> questionKeptAboutReceiversForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    if (liesInThread(key))
        return std::nullopt;
    if (receivers.family)
        return static_cast<uint64_t>(receivers.family.index() + 1) << 32 | key.bits();
    TypeArena::SetOfRound round = walker.types().setOfRound(receivers.set);
    if (!round.isKeptForBuild)
        return std::nullopt;
    return static_cast<uint64_t>(round.word) << 32 | key.bits();
}

static bool nothingAnswersAboveReceiversForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    const Program& program = walker.program();
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    if (isOnObjectPrototype(program.nameClassOf(key)))
        return false;
    auto askEachMember = [&] {
        return std::ranges::all_of(receivers.members, [&](LayoutID member) {
            return nothingAnswersBelowObjectPrototypeForTypedAccess(program, link, member, key);
        });
    };
    std::optional<uint64_t> asked = questionKeptAboutReceiversForTypedAccess(walker, receivers, key);
    if (!asked)
        return askEachMember();
    auto& ofRecords = keptByTypedAccesses(const_cast<Walker&>(walker)).whetherEveryReceiverIsRecord;
    auto everyReceiverIsRecord = ofRecords.find(static_cast<uint32_t>(*asked >> 32));
    if (everyReceiverIsRecord == ofRecords.end()) {
        everyReceiverIsRecord = ofRecords.add(static_cast<uint32_t>(*asked >> 32), std::ranges::all_of(receivers.members, [&](LayoutID member) {
            return program.at(member).kind == LayoutKind::Record;
        })).iterator;
    }
    if (everyReceiverIsRecord->value)
        asked = *asked >> 32 << 32;
    auto& kept = keptByTypedAccesses(const_cast<Walker&>(walker)).whetherNothingAnswersAbove;
    auto known = kept.find(*asked);
    if (known != kept.end())
        return known->value;

    using RowsAboveReceivers = KeptByTypedAccesses::RowsAboveReceivers;
    RowsAboveReceivers& rows = keptByTypedAccesses(const_cast<Walker&>(walker)).rowsAboveReceivers.add(static_cast<uint32_t>(*asked >> 32), RowsAboveReceivers()).iterator->value;
    if (rows.kind == RowsAboveReceivers::Kind::NotLookedAtYet && rows.membersAskedOneByOne >= receivers.members.size()) {
        rows.kind = RowsAboveReceivers::Kind::EveryMemberListsWhatStandsAbove;
        Vector<std::pair<uint32_t, LayoutID>> places;
        for (LayoutID member : receivers.members) {
            const Layout& layout = program.at(member);
            if (layout.kind == LayoutKind::Record && !layout.has(LayoutFlags::IsOfCreatedObject) && !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction))
                continue;
            if (layout.kind == LayoutKind::Record || layout.has(LayoutFlags::IsOfCreatedObject) || member.index() >= link.layouts.size() || !link.layouts[member.index()].listsEveryMemberAbove || link.layouts[member.index()].namesAreDefinedPastLists[static_cast<size_t>(ClassSide::Prototype)]) {
                rows.kind = RowsAboveReceivers::Kind::SomeMemberSaysNoUnderEveryKey;
                break;
            }
            if (link.layouts[member.index()].hasConstructorFunctionInRow)
                rows.kind = RowsAboveReceivers::Kind::AreAskedOneByOne;
            places.append({ link.placesOfLayoutsInRows[member.index()], member });
        }
        if (rows.kind == RowsAboveReceivers::Kind::EveryMemberListsWhatStandsAbove) {
            std::ranges::sort(places);
            for (auto [place, member] : places) {
                if (!rows.stretchesOfInstancesInRows.isEmpty() && rows.stretchesOfInstancesInRows.last().end == place)
                    ++rows.stretchesOfInstancesInRows.last().end;
                else
                    rows.stretchesOfInstancesInRows.append({ place, place + 1, member });
            }
        }
    }
    bool nothingAnswers = false;
    switch (rows.kind) {
    case RowsAboveReceivers::Kind::SomeMemberSaysNoUnderEveryKey:
        break;
    case RowsAboveReceivers::Kind::NotLookedAtYet:
        nothingAnswers = std::ranges::all_of(receivers.members, [&](LayoutID member) {
            ++rows.membersAskedOneByOne;
            return nothingAnswersBelowObjectPrototypeForTypedAccess(program, link, member, key);
        });
        break;
    case RowsAboveReceivers::Kind::AreAskedOneByOne:
        nothingAnswers = askEachMember();
        break;
    case RowsAboveReceivers::Kind::EveryMemberListsWhatStandsAbove: {
        uint32_t classesAsked = 0;
        nothingAnswers = rows.stretchesOfInstancesInRows.isEmpty() || (!classHolderIsMadeWithName(program, ClassSide::Prototype, key) && !link.someClassAtOrAboveDeclares(program, rows.stretchesOfInstancesInRows.span(), ClassSide::Prototype, key, classesAsked));
        const_cast<Walker&>(walker).count(StatisticID::DeclaringClassesChecked, classesAsked);
        if (walker.checksWalk())
            RELEASE_ASSERT(nothingAnswers == askEachMember());
        break;
    }
    }
    return keptByTypedAccesses(const_cast<Walker&>(walker)).whetherNothingAnswersAbove.add(*asked, nothingAnswers).iterator->value;
}

static TypeID typeLaidAboveObjectsOfForTypedAccess(const Program& program, const LinkTables& link, TypeArena& types, LayoutID id, PropertyKeyID key)
{
    const Layout& layout = program.at(id);
    if (!findsMemberWithoutCodeForTypedAccess(program, link, key) || isOnObjectPrototype(program.nameClassOf(key)) || layout.kind != LayoutKind::Instance || layout.has(LayoutFlags::IsOfCreatedObject))
        return { };
    OptionSet<LayoutChange> possibleChanges = possibleChangesForTypedAccess(link, id);
    if (possibleChangesForbid(possibleChanges, TypedAccess::FindsNoProperty) || possibleChangesForbid(possibleChanges, TypedAccess::FindsMethodAbove) || std::ranges::find(link.appendedKeysOf(id), key) != link.appendedKeysOf(id).end())
        return { };
    Vector<FunctionID, 4> functions;
    if (!link.findFunctionsLaidAbove(program, id, key, functions) || functions.isEmpty())
        return { };
    TypeID found = TypeTable::undefined;
    for (FunctionID function : functions)
        found = types.unionOf(found, types.forFunction(function));
    return found;
}

TypeID typeReadByNameOffObjectsOf(const Program& program, const LinkTables& link, const Plan* plan, TypeArena& types, LayoutID layout, PropertyKeyID key)
{
    if (!isLookedUpAsNameForTypedAccess(program, key) || !isFoundByShapeTest(program.at(layout)))
        return { };
    std::optional<FieldReference> field = findField(program, layout, key);
    bool isAppended = false;
    if (!field) {
        std::span<const PropertyKeyID> appendedKeys = link.appendedKeysOf(layout);
        size_t place = std::ranges::find(appendedKeys, key) - appendedKeys.begin();
        isAppended = place < appendedKeys.size();
        if (isAppended)
            field = FieldReference { layout, static_cast<uint32_t>(program.at(layout).fields.size() + place) };
    }
    if (!field)
        return typeLaidAboveObjectsOfForTypedAccess(program, link, types, layout, key);
    if (field->layout != layout)
        return { };
    const Field* record = isAppended ? nullptr : &fieldAt(program, *field);
    LinkedCell cell = cellOfFieldForTypedAccess(link, *field);
    bool liesUnderAttributesThatCount = record && !record->form.isEmpty() && (plan || record->form.contains(FieldForm::IsAccessor));
    if (liesUnderAttributesThatCount || !cell.everyWriterIsKnown || !cell.proven)
        return { };
    TypeID atDoors = cell.proven;
    if (plan) {
        const PlacedSlot& slot = plan->slot(layout, field->field);
        atDoors = slot.provenType ? slot.provenType : slot.everyStoreFits ? slot.type : TypeID();
    }
    if (cell.holdsAccessorFromBirth) {
        const LinkedAccessorsOfField* laid = link.findAccessorsExpectedIn(*field);
        TypeID returned = !laid || !laid->getterIsProven ? TypeID() : plan ? plan->typeHeldOfResult(program, link, laid->getter) : program.at(laid->getter).isCoroutine() ? TypeID() : link.function(laid->getter).result;
        return returned || plan ? returned : cell.proven;
    }
    if (cell.keepsAccessorOnceLaid) {
        const LinkedAccessorsOfField* laid = link.findAccessorsExpectedIn(*field);
        TypeID returned = !laid || !laid->getterMayNotBeLaidYet || !plan ? TypeID() : plan->typeHeldOfResult(program, link, laid->getter);
        bool isNamedAbove = record && record->flags.containsAny({ FieldFlags::IsMethod, FieldFlags::IsAlsoInPrototype });
        bool emptySlotSaysThatNameIsMissing = !isNamedAbove && !possibleChangesForbid(possibleChangesForTypedAccess(link, layout) - LayoutChange::GetsAccessor, TypedAccess::ReadsSlotThatMayBeEmpty) && nothingAnswersAboveObjectsOfForTypedAccess(program, link, layout, key);
        return returned && emptySlotSaysThatNameIsMissing ? types.withPrimitives(returned, Primitive::Undefined) : TypeID();
    }
    if (!atDoors)
        return { };
    if (cell.isFilledAtEveryRead)
        return atDoors;

    if (!plan && record && !record->form.isEmpty())
        return types.withPrimitives(types.withoutPrimitives(atDoors, Primitive::Absent), Primitive::Undefined);
    if (!plan && isAppended && program.at(layout).has(LayoutFlags::IsOfCreatedObject))
        return types.withPrimitives(types.withoutPrimitives(atDoors, Primitive::Absent), Primitive::Undefined);
    bool isNamedAbove = record && record->flags.containsAny({ FieldFlags::IsMethod, FieldFlags::IsAlsoInPrototype });
    if (isNamedAbove || possibleChangesForbid(possibleChangesForTypedAccess(link, layout), TypedAccess::ReadsSlotThatMayBeEmpty) || !nothingAnswersAboveObjectsOfForTypedAccess(program, link, layout, key))
        return { };
    return types.withPrimitives(types.withoutPrimitives(atDoors, Primitive::Absent), Primitive::Undefined);
}

static OptionSet<LayoutChange> changesThatMayReachFieldForTypedAccess(const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field)
{
    return field.isNeverRedefined && field.coverage == CoverageOfTypedAccess::EveryReceiver ? receivers.possibleChanges - LayoutChange::GetsAccessor : receivers.possibleChanges;
}

static bool mayFindSlotEmptyInTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, TypedAccess access)
{
    if (!field.isMovedOutByNoStore || possibleChangesForbid(changesThatMayReachFieldForTypedAccess(receivers, field), access))
        return false;
    return access == TypedAccess::DefinesSlotThatMayBeEmpty || (!field.isNamedAbove && nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key));
}

static bool findsNothingForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    return field.coverage == CoverageOfTypedAccess::NoReceiver && !possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty)
        && nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key);
}

static BuiltinID findMemberOfObjectPrototypeForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (field.coverage != CoverageOfTypedAccess::NoReceiver || !isOnObjectPrototype(program.nameClassOf(key)) || !walker.has(Mechanism::BuiltinCalls))
        return { };
    const BuiltinTable& table = program.builtins();
    BuiltinID location = findConstantMemberOnceForBuild(walker, Builtins::ObjectPrototype, key);
    if (!location || !isObjectKind(table.at(table.canonicalID(location)).kind))
        return { };
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    const SlotPlan& slots = walker.plan().slots();
    bool everyReceiverInheritsIt = std::ranges::all_of(receivers.members, [&](LayoutID member) {
        return nothingAnswersBelowObjectPrototypeForTypedAccess(program, link, member, key) && !slots.findField(program, member, key);
    });
    return everyReceiverInheritsIt ? location : BuiltinID();
}

static bool changesForbidReadOffObjectPrototype(const ReceiversOfTypedAccess& receivers, OptionSet<CellFact> knownOfCell)
{
    return possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty, knownOfCell) || possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove, knownOfCell);
}

static bool paysToLookAtSlotForTypedAccess(const Walker& walker, const FieldOfTypedAccess& field, PropertyKeyID key, bool isRead = false, bool isLastResort = false)
{
    PropertyKeyKind kind = walker.program().at(key).kind;
    bool isLikelyThere = isLastResort || field.isRequired || (isRead && (field.isAppended ? field.isAppendedByConstructors : field.isOfBirth));
    bool isNameOrSymbolThatIsRead = kind == PropertyKeyKind::Text || kind == PropertyKeyKind::EnginePrivateName || kind == PropertyKeyKind::ModulePrivateName || (isRead && kind == PropertyKeyKind::CreatedSymbol);
    return isLikelyThere && !field.mayBeListedWithoutStorage && isNameOrSymbolThatIsRead;
}

static TypedForm formBehindFilledSlotForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, TypedAccess access, bool isLastResort = false)
{
    if (!paysToLookAtSlotForTypedAccess(walker, field, key, access == TypedAccess::ReadsFilledSlot, isLastResort) || possibleChangesForbid(receivers.possibleChanges, access))
        return TypedForm::None;
    return TypedForm::BuiltBehindFilledSlot;
}

static MemberFound findMemberAboveReceiversForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    auto askEachMember = [&]() -> MemberFound {
        MemberFound foundByAll;
        for (LayoutID member : receivers.members) {
            MemberFound found = findMemberAboveObjectsOf(walker.program(), linkTablesForTypedAccesses(walker), member, key);
            if (found.kind == MemberFound::Kind::Unknown && !found.method && !found.getter && !found.setter)
                return { };
            if (member != receivers.members[0] && (found.kind != foundByAll.kind || found.method != foundByAll.method || found.getter != foundByAll.getter || found.setter != foundByAll.setter))
                return { };
            foundByAll = found;
        }
        return foundByAll;
    };
    std::optional<uint64_t> asked = key ? questionKeptAboutReceiversForTypedAccess(walker, receivers, key) : std::nullopt;
    if (!asked)
        return askEachMember();
    auto& kept = keptByTypedAccesses(const_cast<Walker&>(walker)).membersFoundAbove;
    auto known = kept.find(*asked);
    if (known == kept.end())
        known = kept.add(*asked, askEachMember()).iterator;
    return known->value;
}

static MemberFound findMemberOfNamedValueForTypedAccess(const Walker& walker, TypeID proven, PropertyKeyID key)
{
    if (!proven)
        return { };
    Type type = walker.types().at(proven);
    ObjectID object = type.isObject() && type.isJavaScriptValue() ? singleObjectOf(type) : ObjectID();
    return object ? findMemberOfNamedObject(walker.program(), linkTablesForTypedAccesses(walker), walker.types().objects().at(object), key) : MemberFound();
}

static ValueID valueBehindOtherNamesForTypedAccess(const BodyIndex& index, ValueID value, std::span<const OtherNameOfObjectBeingMade> namesFollowedByScan = { })
{
    while (value) {
        ValueID named;
        if (!index.isParameter(value)) {
            if (inputPassThroughOpcodes.contains(index.opcodeOf(value)) || index.opcodeOf(value) == Opcode::ToThis)
                named = index.value(value).operand(0);
        } else if (std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(index.blockOf(value))) {
            uint32_t position = value.index() - index.block(index.blockOf(value)).parameter(0).index();
            std::span<const ValueID> arguments = edge->target.arguments();
            named = position < arguments.size() ? arguments[position] : index.valueNarrowedInto(value);
        } else if (auto followed = std::ranges::find(namesFollowedByScan, value, &OtherNameOfObjectBeingMade::parameter); followed != namesFollowedByScan.end()) {
            named = followed->object;
        }
        if (!named)
            return value;
        value = named;
    }
    return value;
}

static bool everyReceiverIsBornByLiteralInTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers)
{
    return !receivers.members.isEmpty() && std::ranges::all_of(receivers.members.span(), [&](LayoutID member) {
        const Layout& layout = walker.program().at(member);
        return layout.kind == LayoutKind::Record && !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction) && !layout.has(LayoutFlags::IsOfCreatedObject);
    });
}

template<typename Member, typename NumberOf>
static bool listOfWrittenObjectHoldsInTypedAccess(const Walker& walker, std::span<const Member> list, uint32_t asked, const NumberOf& numberOf)
{
    static constexpr size_t longestListSoughtThrough = 8;
    if (list.size() <= longestListSoughtThrough)
        return std::ranges::any_of(list, [&](const Member& member) { return numberOf(member) == asked; });
    return walker.numbersInListOfWrittenObject(list.data(), [&](BitVector& numbers) {
        for (const Member& member : list)
            numbers.set(numberOf(member));
    }).get(asked);
}

static ALWAYS_INLINE bool isFilledForBodyBeingReadInTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, ValueID object, PropertyKeyID key)
{
    if (!field.isFilledByEveryConstruction || !field.isMovedOutByNoStore)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID root = valueBehindOtherNamesForTypedAccess(index, object, walker.facts().summaries().at(walker.currentFunction()).otherNamesOfObjectsBeingMade.span());
    if (!root)
        return false;
    const Function& function = walker.program().at(walker.currentFunction());
    if (index.isParameter(root)) {
        std::optional<unsigned> thisParameter = function.thisParameterIndex();
        bool isHandedObjectBeingMade = function.kind == FunctionKind::Initializer || (function.takesThisFromConstruction() && function.constructedClass);
        return !isHandedObjectBeingMade || !thisParameter || index.blockOf(root) != entryBlock || root != index.block(entryBlock).parameter(*thisParameter) || (function.kind != FunctionKind::Initializer && everyReceiverIsBornByLiteralInTypedAccess(walker, receivers));
    }
    BodyValue made = index.value(root);
    if (made.opcode() == Opcode::SuperConstruct || made.opcode() == Opcode::SuperConstructList)
        return false;
    if (made.opcode() == Opcode::Data) {
        DataID data = made.as<Opcode::Data>().data();
        const DataNode::Object* constant = data ? walker.program().at(data).root.tryAs<DataNode::Object>() : nullptr;
        return constant && listOfWrittenObjectHoldsInTypedAccess(walker, std::span<const DataMember>(constant->members), key.index(), [](const DataMember& member) { return member.key.index(); });
    }
    if (made.opcode() != Opcode::NewObject)
        return true;
    auto allocation = made.as<Opcode::NewObject>();
    std::optional<uint32_t> listed = allocation.layout() ? walker.program().at(allocation.layout()).findField(key) : std::nullopt;
    return listed && listOfWrittenObjectHoldsInTypedAccess(walker, std::span<const uint32_t>(allocation.fields()), *listed, [](uint32_t laid) { return laid; });
}

static bool isBeingMadeByBodyBeingReadInTypedAccess(const Walker& walker, ValueID object)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID root = valueBehindOtherNamesForTypedAccess(index, object, walker.facts().summaries().at(walker.currentFunction()).otherNamesOfObjectsBeingMade.span());
    if (!root)
        return false;
    if (!index.isParameter(root)) {
        Opcode opcode = index.opcodeOf(root);
        return opcode == Opcode::SuperConstruct || opcode == Opcode::SuperConstructList || opcode == Opcode::NewObject;
    }
    const Function& function = walker.program().at(walker.currentFunction());
    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    bool isHandedObjectBeingMade = function.kind == FunctionKind::Initializer || (function.takesThisFromConstruction() && function.constructedClass);
    return isHandedObjectBeingMade && thisParameter && index.blockOf(root) == entryBlock && root == index.block(entryBlock).parameter(*thisParameter);
}

static Reference valueInSlotForTypedAccess(Walker& walker, Reference object, const FieldOfTypedAccess& field)
{
    Reference held = walker.state().slot(object, field.place);
    TypeID type = held && !isClosureLeftUnmade(walker, held) ? walker.typeOf(held) : TypeID();
    return type && !(walker.types().at(type).primitives & Primitive::Absent) ? held : Reference();
}

static bool slotIsKnownEmptyForTypedAccess(Walker& walker, Reference object, const FieldOfTypedAccess& field)
{
    Reference noted = walker.state().slot(object, field.place);
    return noted && noted.isRecord() && walker.code().at(noted.id()).opcode == Opcode::Absent;
}

static bool provesSlotFilledForTypedAccess(const Walker& walker, Reference held, PropertyKeyID key)
{
    if (!held)
        return false;
    if (!held.isValue())
        return true;
    const Operation& origin = walker.code().at(held.id());
    return (origin.opcode != Opcode::GetSlotOrUndefined && origin.opcode != Opcode::GetDeferredClosureSlotOrUndefined) || origin.key() != key;
}

static bool bindBooleanForTypedAccess(Walker& walker, const BodyValue& value, bool answer)
{
    Draft constant(Opcode::Boolean);
    constant.setBoolean(answer);
    walker.bind(value.id(), walker.constant(WTF::move(constant)));
    return true;
}

static void fillSlotOperationForTypedAccess(Draft& draft, Reference object, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    draft.setObjectOperand(object);
    draft.setKey(key);
    draft.setReceivers(receivers.set);
    draft.setLevel(KnowledgeLevel::Proven);
}

static bool slotHoldsMarkForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    const SlotPlan& slots = walker.plan().slots();
    if (!slots.someSlotUnderKeyHoldsMark(key)) [[likely]]
        return false;
    for (LayoutID member : receivers.members) {
        std::optional<FieldIndex> field = slots.findField(walker.program(), member, key);
        if (field && slots.slot(member, *field).representation == SlotRepresentation::DeferredClosure)
            return true;
    }
    return false;
}

static Opcode readOfFilledSlotForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    return slotHoldsMarkForTypedAccess(walker, receivers, key) ? Opcode::GetDeferredClosureSlot : Opcode::GetSlot;
}

static ALWAYS_INLINE void readScopeBesideSlotForTypedAccess(Walker& walker, Origin origin, Reference closure)
{
    if (!closure || !closure.isValue() || walker.isUnreachable() || walker.code().at(closure.id()).opcode != Opcode::GetDeferredClosureSlot)
        return;
    TypeID type = walker.typeOf(closure);
    FunctionID function = type ? singleFunctionOf(walker.types(), walker.types().at(type)) : FunctionID();
    if (!function || walker.program().at(function).captures.size() != 1)
        return;
    Draft capture(Opcode::GetCapture, origin);
    walker.chooseConservativeFields(capture);
    capture.setFunction(function);
    capture.setIndex(0);
    capture.setClosureOperand(closure);
    walker.build(WTF::move(capture));
}

static Draft draftOfSlotOperationForTypedAccess(Walker& walker, const BodyValue& value, Opcode opcode, Reference object, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    Draft draft = walker.draftOf(value);
    draft.become(opcode);
    fillSlotOperationForTypedAccess(draft, object, receivers, key);
    return draft;
}

static ALWAYS_INLINE TypedForm buildReadOfMemberForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const MemberFound& member)
{
    switch (member.kind) {
    case MemberFound::Kind::Unknown:
        return TypedForm::None;
    case MemberFound::Kind::Method: {
        if (operands.areOnlyTypes)
            return TypedForm::Built;
        Draft read = walker.draftOf(value);
        walker.chooseConservativeFields(read);
        Effects effects = Effects::ofReadByNameThatRunsNoCode();
        walker.bind(value.id(), walker.build(WTF::move(read), &effects));
        return TypedForm::Built;
    }
    case MemberFound::Kind::Accessor:
        break;
    }

    if (!member.getter) {
        if (!operands.areOnlyTypes)
            walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
        return TypedForm::Built;
    }
    if (!walker.has(Mechanism::DirectCalls))
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return TypedForm::BuiltAsCall;
    return callAccessor(walker, value, member.getter, operands.object, { }) ? TypedForm::Built : TypedForm::None;
}

static bool mayFindSlotEmptyBehindLooksInTypedRead(const Walker&, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID, OptionSet<CellFact> knownOfCell, const WalkState* stateAskedAboutGuards);
static std::optional<OptionSet<CellFact>> askAheadAboutReadOfSlotThatMayBeEmpty(const Walker&, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID);
static TypedForm buildReadOfMethodBehindLooksForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&, const ReceiversOfTypedAccess&, PropertyKeyID);
static TypedForm buildReadThatFindsNothingBehindLooksForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID);
static TypedForm buildReadOffObjectPrototypeForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID);
static TypedForm buildAccessToAccessorBehindLooksForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&, const ReceiversOfTypedAccess&, PropertyKeyID);
static TypedForm buildReadOffConstructorFunctionBehindGuardForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&);
static TypedForm buildReadOffHeirOfBuiltinForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&);
static TypedForm buildReadsOfMethodOffPrototypeOfClassForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&);
static bool orderWordShowsThatCellLacksFieldForTypedAccess(Walker&, Reference object, const ReceiversOfTypedAccess&, PropertyKeyID);
static bool isReadForCallAloneInTypedAccess(const Walker&, const BodyValue&);
static void askAgainGuardsAboveReceiversForTypedAccess(Walker&, const ReceiversOfTypedAccess&, PropertyKeyID, bool stores, Origin);
static void askAgainWhetherFieldHidesAccessorForTypedAccess(Walker&, Reference object, const ReceiversOfTypedAccess&, PropertyKeyID, Origin);

OptionSet<CellFact> knownCellFacts(const Walker& walker, Reference object)
{
    OptionSet<CellFact> facts = walker.state().cellFacts(object);
    if (walker.shouldAssumeGuardsAndFacts())
        facts.add({ CellFact::NotFrozen, CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype, CellFact::NoExtraProperties });
    return facts;
}

static TypedForm buildReadWhereSomeReceiversLackFieldForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    if (!field.liesInOneSlotOfReceiversThatHaveIt || value.opcode() != Opcode::PropGet || slotHoldsMarkForTypedAccess(walker, receivers, key))
        return TypedForm::None;
    OptionSet<CellFact> knownOfCell = knownCellFacts(walker, operands.object);
    if (possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty, knownOfCell))
        return TypedForm::None;
    if (!mayFindSlotEmptyBehindLooksInTypedRead(walker, receivers, field, key, knownOfCell, &walker.state()))
        return TypedForm::None;
    askAgainGuardsAboveReceiversForTypedAccess(walker, receivers, key, false, walker.originOf(value));
    if (walker.isUnreachable())
        return TypedForm::None;

    Reference object = operands.object;
    if (Reference held = valueInSlotForTypedAccess(walker, object, field); held && walker.has(Mechanism::Folding)) {
        walker.bind(value.id(), held);
        return TypedForm::Built;
    }
    Reference read = walker.build(draftOfSlotOperationForTypedAccess(walker, value, Opcode::GetSlotOrUndefined, object, receivers, key));
    walker.bind(value.id(), read);
    if (read)
        walker.state().noteSlot(object, field.place, read);
    return TypedForm::Built;
}

static bool lookAsksForValueOfWrittenTypeInTypedAccess(const FieldOfTypedAccess& field)
{
    if (!field.isRequired || !field.liesInFormWithMarks)
        return false;
    return field.isWrittenWithoutUndefinedAndNull || (field.liesInFormOfNumbersOrBooleans && !field.liesInOptionalBoolean);
}

static ALWAYS_INLINE bool markOfCellAnswersForFieldInTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, ValueID object)
{
    bool lookAsksForValue = lookAsksForValueOfWrittenTypeInTypedAccess(field) && !field.markAnswersForValue;
    return field.liesInOneSlot && field.coverage == CoverageOfTypedAccess::EveryReceiver && field.markAnswersForSlot && !lookAsksForValue && !field.isAppended && paysToLookAtSlotForTypedAccess(walker, field, key)
        && !possibleChangesForbid(receivers.possibleChanges, TypedAccess::ReadsFilledSlot) && !isBeingMadeByBodyBeingReadInTypedAccess(walker, object);
}

bool storeMaySetMarkOfCell(const Walker& walker, std::span<const LayoutID> receivers, PropertyKeyID key, TypeID typeOfStored)
{
    if (typeOfStored && !walker.types().at(typeOfStored).mayBeUndefinedOrNull())
        return false;
    return std::ranges::any_of(receivers, [&](LayoutID member) {
        std::optional<FieldIndex> field = walker.plan().slots().findField(walker.program(), member, key);
        return field && walker.plan().layout(member).holdsRawValueFromBirth(*field);
    });
}

bool typedStoreAheadMaySetMarkOfCell(Walker& walker, TypeID typeOfObject, PropertyKeyID key, TypeID typeOfStored)
{
    std::optional<ReceiversOfTypedAccess> receivers = typeOfObject ? findReceiversOfTypedAccess(walker, typeOfObject) : std::nullopt;
    return !receivers || storeMaySetMarkOfCell(walker, receivers->members.span(), key, typeOfStored);
}

bool markOfCellAnswersForRead(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    if (read.opcode() != Opcode::PropGet && read.opcode() != Opcode::PropSet)
        return false;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject);
    return receivers && markOfCellAnswersForFieldInTypedAccess(walker, *receivers, describeFieldOfTypedAccess(walker, *receivers, read.key(), { }, true), read.key(), read.objectOperand());
}

std::optional<FilledSlot> findFilledSlot(Walker& walker, TypeID typeOfObject, PropertyKeyID key)
{
    if (!walker.has(Mechanism::TypedOperations) || !typeOfObject || walker.types().at(typeOfObject).primitives || !walker.types().at(typeOfObject).isJavaScriptValue())
        return std::nullopt;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject);
    if (!receivers)
        return std::nullopt;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (findsNothingForTypedAccess(walker, *receivers, field, key) || field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || slotHoldsMarkForTypedAccess(walker, *receivers, key))
        return std::nullopt;
    if (!field.isFilledAtEveryRead || !field.isMovedOutByNoStore)
        return std::nullopt;
    return FilledSlot { field.place, receivers->set };
}

static TypedForm buildReadForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, PropertyKeyID key)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, operands.typeOfObject);
    if (!receivers) {
        if (TypedForm form = buildReadOfMemberForTypedAccess(walker, value, operands, findMemberOfNamedValueForTypedAccess(walker, operands.typeOfObject, key)); form != TypedForm::None)
            return form;
    }
    if (!receivers) {
        TypedForm form = operands.areOnlyTypes || operands.object ? buildReadOfBuiltinMember(walker, value, operands.typeOfObject, operands.object) : TypedForm::None;
        if (form == TypedForm::None)
            form = buildReadOffHeirOfBuiltinForTypedAccess(walker, value, operands);
        return form != TypedForm::None ? form : buildReadOffConstructorFunctionBehindGuardForTypedAccess(walker, value, operands);
    }
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (findsNothingForTypedAccess(walker, *receivers, field, key)) {
        if (!operands.areOnlyTypes)
            walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
        return TypedForm::Built;
    }
    if (std::optional<uint64_t> refused = !operands.areOnlyTypes && operands.object && field.lastReceiverThatListsIt && !field.severalReceiversListIt ? findKnown(walker, KnownNote::IsOfNoneOfLayouts, operands.object) : std::nullopt) {
        bool onlyOneThatListsItIsRefused = std::ranges::binary_search(walker.types().listOf(LayoutSetID::fromBits(static_cast<uint32_t>(*refused))), field.lastReceiverThatListsIt.index(), { }, [](LayoutID layout) { return layout.index(); });
        if (onlyOneThatListsItIsRefused && !possibleChangesForbid(receivers->possibleChanges, TypedAccess::FindsNoProperty) && nothingAnswersAboveReceiversForTypedAccess(walker, *receivers, key)) {
            walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
            return TypedForm::Built;
        }
    }
    if (field.coverage == CoverageOfTypedAccess::NoReceiver) {
        MemberFound above = findMemberAboveReceiversForTypedAccess(walker, *receivers, key);
        if (above.kind == MemberFound::Kind::Accessor && !operands.areOnlyTypes)
            askAgainWhetherFieldHidesAccessorForTypedAccess(walker, operands.object, *receivers, key, walker.originOf(value));
        TypedForm form = buildReadOfMemberForTypedAccess(walker, value, operands, above);
        if (form == TypedForm::None)
            form = buildReadOfMethodBehindLooksForTypedAccess(walker, value, operands, *receivers, key);
        if (form == TypedForm::None)
            form = buildAccessToAccessorBehindLooksForTypedAccess(walker, value, operands, *receivers, key);
        if (form == TypedForm::None)
            form = buildReadOffObjectPrototypeForTypedAccess(walker, value, operands, *receivers, field, key);
        return form != TypedForm::None ? form : buildReadThatFindsNothingBehindLooksForTypedAccess(walker, value, operands, *receivers, field, key);
    }
    if (!field.liesInOneSlot)
        return TypedForm::None;

    bool mayFindSlotEmpty = mayFindSlotEmptyInTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsSlotThatMayBeEmpty);
    bool isFilledByConstructions = (field.isFilledAtEveryRead && field.isMovedOutByNoStore) || isFilledForBodyBeingReadInTypedAccess(walker, *receivers, field, value.objectOperand(), key);
    auto readsFilledSlotForCallAlone = [&] {
        return isFilledByConstructions && walker.has(Mechanism::DirectCalls) && isReadForCallAloneInTypedAccess(walker, value);
    };
    if (operands.areOnlyTypes) {
        if (mayFindSlotEmpty || isFilledByConstructions) {
            if (slotHoldsMarkForTypedAccess(walker, *receivers, key) && !readsFilledSlotForCallAlone())
                walker.noteFormAskedAhead(Opcode::GetDeferredClosureSlotOrUndefined, KnowledgeLevel::Proven);
            return TypedForm::Built;
        }
        if (field.isAppended && isBeingMadeByBodyBeingReadInTypedAccess(walker, value.objectOperand()) && !nothingAnswersAboveReceiversForTypedAccess(walker, *receivers, key))
            return TypedForm::None;
        TypedForm behindFilledSlot = formBehindFilledSlotForTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsFilledSlot);
        bool paysToLookAtCell = walker.isInLoop() || walker.weighsWhatIsWritten() || walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) || field.isLaidLate;
        if (behindFilledSlot != TypedForm::None || !paysToLookAtCell)
            return behindFilledSlot;
        if (askAheadAboutReadOfSlotThatMayBeEmpty(walker, *receivers, field, key))
            return TypedForm::BuiltBehindLookAtCell;
        if (walker.isInLoop())
            return formBehindFilledSlotForTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsFilledSlot, true);
        if (isBeingMadeByBodyBeingReadInTypedAccess(walker, value.objectOperand()))
            return TypedForm::None;
        TypedForm lastResort = formBehindFilledSlotForTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsFilledSlot, true);
        if (lastResort != TypedForm::None)
            walker.noteFormAskedAhead(Opcode::IsSlotFilled, KnowledgeLevel::Proven);
        return lastResort;
    }

    Reference object = operands.object;
    Reference held = valueInSlotForTypedAccess(walker, object, field);
    bool isFilled = isFilledByConstructions || (provesSlotFilledForTypedAccess(walker, held, key) && !possibleChangesForbid(receivers->possibleChanges, TypedAccess::ReadsFilledSlot));
    bool markAnswers = walker.state().cellFacts(object).contains(CellFact::BearsNoMark) && markOfCellAnswersForFieldInTypedAccess(walker, *receivers, field, key, value.objectOperand());
    isFilled |= markAnswers;
    if (!isFilled && !mayFindSlotEmpty)
        mayFindSlotEmpty = mayFindSlotEmptyBehindLooksInTypedRead(walker, *receivers, field, key, knownCellFacts(walker, object), &walker.state());
    if (!isFilled && mayFindSlotEmpty)
        askAgainGuardsAboveReceiversForTypedAccess(walker, *receivers, key, false, walker.originOf(value));
    if (!isFilled && !mayFindSlotEmpty)
        return TypedForm::None;
    if (held && walker.has(Mechanism::Folding)) {
        walker.bind(value.id(), held);
        return TypedForm::Built;
    }

    if (!isFilled && walker.has(Mechanism::Folding) && (orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, key) || slotIsKnownEmptyForTypedAccess(walker, object, field))) {
        walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
        return TypedForm::Built;
    }

    std::optional<Walker::ProofsHoldEverywhere> filledWhereverObjectIs;
    if (isFilledByConstructions)
        filledWhereverObjectIs.emplace(walker);
    bool holdsMark = slotHoldsMarkForTypedAccess(walker, *receivers, key);
    Opcode opcodeOfRead = isFilled ? (holdsMark ? Opcode::GetDeferredClosureSlot : Opcode::GetSlot) : (holdsMark ? Opcode::GetDeferredClosureSlotOrUndefined : Opcode::GetSlotOrUndefined);
    if (markAnswers)
        walker.checkFactsRememberedOfCell(object, walker.originOf(value), CellFact::BearsNoMark);
    if (holdsMark && readsFilledSlotForCallAlone()) {
        Effects effects = Effects::ofReadByNameThatRunsNoCode();
        Reference read = walker.build(draftOfSlotOperationForTypedAccess(walker, value, opcodeOfRead, object, *receivers, key), &effects);
        readScopeBesideSlotForTypedAccess(walker, walker.originOf(value), read);
        walker.bind(value.id(), read);
        if (read && !isClosureLeftUnmade(walker, walker.state().slot(object, field.place)))
            walker.state().noteSlot(object, field.place, read);
        return TypedForm::Built;
    }
    Draft draft = draftOfSlotOperationForTypedAccess(walker, value, opcodeOfRead, object, *receivers, key);
    if (markAnswers && field.markAnswersForValue && opcodeOfRead == Opcode::GetSlot)
        draft.setHoldsDeclaredType(true);
    Reference read = walker.build(WTF::move(draft));
    readScopeBesideSlotForTypedAccess(walker, walker.originOf(value), read);
    walker.bind(value.id(), read);
    if (read)
        walker.state().noteSlot(object, field.place, read);
    if (read && opcodeOfRead == Opcode::GetSlot)
        walker.noteSlotRead(object, key, read);
    return TypedForm::Built;
}

FunctionID findMethodAboveProvenObject(Walker& walker, Reference object, PropertyKeyID key)
{
    return object && object.isValue() ? findMethodAboveProvenType(walker, walker.typeOf(object), key) : FunctionID();
}

FunctionID findMethodAboveProvenType(Walker& walker, TypeID proven, PropertyKeyID key)
{
    if (!walker.has(Mechanism::TypedOperations) || !proven || isNeverType(proven) || walker.isUnreachable())
        return { };
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, proven);
    if (!receivers || describeFieldOfTypedAccess(walker, *receivers, key, { }, true).coverage != CoverageOfTypedAccess::NoReceiver)
        return { };
    MemberFound above = findMemberAboveReceiversForTypedAccess(walker, *receivers, key);
    return above.kind == MemberFound::Kind::Method ? above.method : FunctionID();
}

Reference readMethodAboveProvenObject(Walker& walker, Reference object, PropertyKeyID key, Origin origin)
{
    if (!findMethodAboveProvenObject(walker, object, key))
        return { };
    Draft read(Opcode::PropGet, origin);
    read.setObjectOperand(object);
    read.setKey(key);
    walker.chooseConservativeFields(read);
    Effects effects = Effects::ofReadByNameThatRunsNoCode();
    return walker.build(WTF::move(read), &effects);
}

Reference readFilledFieldOfProvenObject(Walker& walker, Reference object, PropertyKeyID key, Origin origin)
{
    if (!walker.has(Mechanism::TypedOperations) || !object || !object.isValue() || walker.isUnreachable())
        return { };
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return { };
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || slotHoldsMarkForTypedAccess(walker, *receivers, key))
        return { };
    Reference held = valueInSlotForTypedAccess(walker, object, field);
    bool isFilled = (field.isFilledAtEveryRead && field.isMovedOutByNoStore) || (provesSlotFilledForTypedAccess(walker, held, key) && !possibleChangesForbid(receivers->possibleChanges, TypedAccess::ReadsFilledSlot));
    if (!isFilled)
        return { };
    if (held && walker.has(Mechanism::Folding))
        return held;
    Draft draft(Opcode::GetSlot, origin);
    walker.chooseConservativeFields(draft);
    fillSlotOperationForTypedAccess(draft, object, *receivers, key);
    Reference read = walker.build(WTF::move(draft));
    if (read)
        walker.state().noteSlot(object, field.place, read);
    return read;
}

static bool slotTellsWhoHasPrivateNameForTypedAccess(const Walker&, LayoutID declaring);

Reference readFilledFieldInFrontOfTurns(Walker& walker, const BodyValue& read, Reference object)
{
    if (!object || !object.isValue() || walker.isUnreachable())
        return { };
    PropertyKeyID key = keyOfAccessToSlot(walker.program(), read);
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers || (read.opcode() == Opcode::PrivateGet && !slotTellsWhoHasPrivateNameForTypedAccess(walker, read.layout())))
        return { };
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || slotHoldsMarkForTypedAccess(walker, *receivers, key))
        return { };
    if (!isFilledForBodyBeingReadInTypedAccess(walker, *receivers, field, read.objectOperand(), key))
        return readFilledFieldOfProvenObject(walker, object, key, walker.originOf(read));
    if (Reference held = valueInSlotForTypedAccess(walker, object, field))
        return held;
    Walker::ProofsHoldEverywhere filledWhereverObjectIs(walker);
    Draft draft(Opcode::GetSlot, walker.originOf(read));
    walker.chooseConservativeFields(draft);
    fillSlotOperationForTypedAccess(draft, object, *receivers, key);
    Reference value = walker.build(WTF::move(draft));
    if (value)
        walker.state().noteSlot(object, field.place, value);
    return value;
}

FunctionID findMethodInSightOfProvenType(Walker& walker, TypeID proven, PropertyKeyID key)
{
    if (FunctionID above = findMethodAboveProvenType(walker, proven, key))
        return above;
    if (!walker.has(Mechanism::TypedOperations) || !proven || isNeverType(proven) || walker.isUnreachable())
        return { };
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, proven);
    if (!receivers)
        return { };
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || !field.isFilledAtEveryRead || !field.isMovedOutByNoStore || slotHoldsMarkForTypedAccess(walker, *receivers, key))
        return { };
    TypeID held;
    for (LayoutID layout : receivers->members.span()) {
        TypeID read = typeReadByNameOffObjectsOf(walker.program(), linkTablesForTypedAccesses(walker), &linkTablesForTypedAccesses(walker) == &walker.facts().linked() ? &walker.plan() : nullptr, walker.types(), layout, key);
        if (!read)
            return { };
        held = held ? walker.types().unionOf(held, read) : read;
    }
    if (!held)
        return { };
    const Type& type = walker.types().at(held);
    return type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
}

Reference readMethodInSightOfProvenObject(Walker& walker, Reference object, PropertyKeyID key, Origin origin)
{
    if (Reference above = readMethodAboveProvenObject(walker, object, key, origin))
        return above;
    Reference own = readFilledFieldOfProvenObject(walker, object, key, origin);
    if (!own)
        return { };
    const Type& held = walker.types().at(walker.typeOf(own));
    return held.isObject() && held.isJavaScriptValue() && singleFunctionOf(walker.types(), held) ? own : Reference();
}

Reference readMethodForCallAlone(Walker& walker, Reference object, PropertyKeyID key, Origin origin)
{
    if (Reference inSight = readMethodInSightOfProvenObject(walker, object, key, origin))
        return inSight;
    if (!walker.has(Mechanism::TypedOperations) || !object || !object.isValue() || walker.isUnreachable())
        return { };
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers || !slotHoldsMarkForTypedAccess(walker, *receivers, key))
        return { };
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || !field.isFilledAtEveryRead || !field.isMovedOutByNoStore)
        return { };
    Draft draft(Opcode::GetDeferredClosureSlot, origin);
    walker.chooseConservativeFields(draft);
    fillSlotOperationForTypedAccess(draft, object, *receivers, key);
    Effects effects = Effects::ofReadByNameThatRunsNoCode();
    Reference read = walker.build(WTF::move(draft), &effects);
    readScopeBesideSlotForTypedAccess(walker, origin, read);
    return read;
}

bool findsNothingUnderNameOfProvenObject(Walker& walker, Reference object, PropertyKeyID key)
{
    if (!walker.has(Mechanism::TypedOperations) || !object || !object.isValue())
        return false;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    return receivers && findsNothingForTypedAccess(walker, *receivers, describeFieldOfTypedAccess(walker, *receivers, key, { }, true), key);
}

bool buildReadWhereSomeReceiversLackField(Walker& walker, const BodyValue& value)
{
    PropertyKeyID key = value.key();
    if (value.opcode() != Opcode::PropGet || value.isFreeName() || !isLookedUpAsNameForTypedAccess(walker.program(), key))
        return false;
    OperandsOfTypedAccess operands;
    operands.object = walker.referenceOf(value.objectOperand());
    operands.typeOfObject = operands.object ? walker.typeOf(operands.object) : TypeID();
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, operands.typeOfObject);
    if (!receivers)
        return false;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    return field.coverage == CoverageOfTypedAccess::SomeReceivers && buildReadWhereSomeReceiversLackFieldForTypedAccess(walker, value, operands, *receivers, field, key) == TypedForm::Built;
}

bool buildOwnPropertyTest(Walker& walker, const BodyValue& question, Reference object, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (!walker.has(Mechanism::TypedOperations) || !object || !object.isValue() || !isLookedUpAsNameForTypedAccess(program, key))
        return false;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return false;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage == CoverageOfTypedAccess::NoReceiver) {
        const LinkTables& link = linkTablesForTypedAccesses(walker);
        const SlotPlan& slots = walker.plan().slots();
        bool noReceiverHasIt = !possibleChangesForbid(receivers->possibleChanges - LayoutChange::GetsObjectAbove, TypedAccess::FindsNoProperty) && std::ranges::all_of(receivers->members, [&](LayoutID member) {
            return link.findsNothing(program, member, ClassSide::Prototype, key, Of::ObjectItself) && !slots.findField(program, member, key);
        });
        return noReceiverHasIt && bindBooleanForTypedAccess(walker, question, false);
    }
    if (!field.liesInOneSlot || !field.isMovedOutByNoStore || possibleChangesForbid(changesThatMayReachFieldForTypedAccess(*receivers, field) - LayoutChange::GetsObjectAbove, TypedAccess::ReadsSlotThatMayBeEmpty))
        return false;
    bool isFilled = provesSlotFilledForTypedAccess(walker, valueInSlotForTypedAccess(walker, object, field), key) || everyReceiverKeepsFieldInSlotThatIsNeverEmpty(walker, receivers->members.span(), key);
    if (walker.has(Mechanism::Folding) && isFilled)
        return bindBooleanForTypedAccess(walker, question, true);
    if (walker.has(Mechanism::Folding) && (orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, key) || slotIsKnownEmptyForTypedAccess(walker, object, field)))
        return bindBooleanForTypedAccess(walker, question, false);
    walker.bind(question.id(), walker.build(draftOfSlotOperationForTypedAccess(walker, question, Opcode::IsSlotFilled, object, *receivers, key)));
    return true;
}

static TypedForm buildTestForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, PropertyKeyID key)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, operands.typeOfObject);
    if (!receivers)
        return TypedForm::None;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (findsNothingForTypedAccess(walker, *receivers, field, key)) {
        if (!operands.areOnlyTypes)
            bindBooleanForTypedAccess(walker, value, false);
        return TypedForm::Built;
    }
    if (!field.liesInOneSlot)
        return TypedForm::None;

    bool mayFindSlotEmpty = mayFindSlotEmptyInTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsSlotThatMayBeEmpty);
    bool isFilledByConstructions = isFilledForBodyBeingReadInTypedAccess(walker, *receivers, field, value.objectOperand(), key);
    if (operands.areOnlyTypes)
        return mayFindSlotEmpty || isFilledByConstructions ? TypedForm::Built : formBehindFilledSlotForTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsFilledSlot);

    Reference object = operands.object;
    bool isFilled = isFilledByConstructions
        || (provesSlotFilledForTypedAccess(walker, valueInSlotForTypedAccess(walker, object, field), key) && !possibleChangesForbid(receivers->possibleChanges, TypedAccess::ReadsFilledSlot));
    if (!isFilled && !mayFindSlotEmpty)
        return TypedForm::None;
    isFilled |= everyReceiverKeepsFieldInSlotThatIsNeverEmpty(walker, receivers->members.span(), key);
    if (!isFilled && walker.has(Mechanism::Folding) && (orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, key) || slotIsKnownEmptyForTypedAccess(walker, object, field))) {
        bindBooleanForTypedAccess(walker, value, false);
        return TypedForm::Built;
    }
    if (isFilled && walker.has(Mechanism::Folding)) {
        bindBooleanForTypedAccess(walker, value, true);
        return TypedForm::Built;
    }
    walker.bind(value.id(), walker.build(draftOfSlotOperationForTypedAccess(walker, value, Opcode::IsSlotFilled, object, *receivers, key)));
    return TypedForm::Built;
}

bool cellAnswersForOwnProperty(Walker& walker, TypeID typeOfObject, PropertyKeyID key)
{
    if (!isLookedUpAsNameForTypedAccess(walker.program(), key))
        return false;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject);
    if (!receivers)
        return false;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage == CoverageOfTypedAccess::NoReceiver)
        return true;
    return field.coverage == CoverageOfTypedAccess::EveryReceiver && field.liesInOneSlot && !field.mayBeListedWithoutStorage;
}

static bool mayOverwriteInTypedStore(const ReceiversOfTypedAccess& receivers, OptionSet<CellFact> knownOfCell)
{
    return !possibleChangesForbid(receivers.possibleChanges, TypedAccess::StoresIntoFilledSlot, knownOfCell);
}

bool findParentNamedByClassRecordForTypedAccess(const Walker& walker, LayoutID classLayout, LayoutID& parent)
{
    const Layout& layout = walker.program().at(classLayout);
    ClassFacts facts = walker.facts().classOf(classLayout);
    parent = { };
    if (layout.isDerived())
        parent = writtenBaseOf(layout);
    else {
        switch (facts.baseState) {
        case BaseState::Layout:
            parent = facts.base;
            break;
        case BaseState::Unknown:
            parent = writtenBaseOf(layout);
            break;
        case BaseState::None:
            parent = facts.prototypeBase;
            break;
        }
    }
    return parent || !layout.has(LayoutFlags::HasHeritage);
}

namespace {

enum class LookupAbove : uint8_t { OfStore, OfStoreIntoCellMadeHere, OfRead };

enum class WhoAnswersForWhatStandsAbove : uint8_t {
    Nobody,
    NothingStandsThere,
    GuardOfClass,
};

} // anonymous namespace

bool objectPrototypeMayGainName(const Program& program)
{
    const BuiltinTable& builtins = program.builtins();
    BuiltinID objectPrototype = builtins.canonicalID(Builtins::ObjectPrototype);
    if (!builtins.at(objectPrototype).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) || !builtins.openNamesOf(objectPrototype).empty() || !builtins.hasLockedPrototypeLink(objectPrototype))
        return true;
    return program.isSomeNameOpenedOn(objectPrototype);
}

static bool objectPrototypeTakesListedNamesAlone(const Program& program)
{
    const BuiltinTable& builtins = program.builtins();
    BuiltinID objectPrototype = builtins.canonicalID(Builtins::ObjectPrototype);
    return builtins.at(objectPrototype).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) && builtins.openNamesOf(objectPrototype).empty() && builtins.hasLockedPrototypeLink(objectPrototype);
}

bool byteTellsOfNamesAddedToObjectPrototype(const Program& program)
{
    return program.admitsReplacedBuiltins && objectPrototypeTakesListedNamesAlone(program);
}

static bool objectPrototypeMayGainNameUnder(const Walker& walker, PropertyKeyID key)
{
    if (!walker.objectPrototypeMayGainName())
        return false;
    const Program& program = walker.program();
    bool takesListedNamesAlone = objectPrototypeTakesListedNamesAlone(program);
    const PropertyKey& record = program.at(key);
    return !takesListedNamesAlone || (record.kind == PropertyKeyKind::Text && program.isOpenedSomewhere(record.text));
}

static bool rowAboveClassPassesWhateverTheNameForTypedAccess(const Walker& walker, KeptByTypedAccesses& kept, LayoutID classLayout)
{
    using RowAbove = KeptByTypedAccesses::RowAbove;
    const Program& program = walker.program();
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    if (kept.rowsAboveClasses.isEmpty())
        kept.rowsAboveClasses.fill(RowAbove::NotAsked, program.layouts.size());
    auto hasMemberUnderComputedKey = [](const auto& declared) {
        return declared.side == ClassSide::Prototype && !declared.key;
    };
    Vector<LayoutID, 16> asked;
    RowAbove answer = RowAbove::Passes;
    for (LayoutID current = classLayout; current;) {
        if (RowAbove known = kept.rowsAboveClasses[current.index()]; known != RowAbove::NotAsked) {
            answer = known == RowAbove::Passes ? RowAbove::Passes : RowAbove::Fails;
            break;
        }
        kept.rowsAboveClasses[current.index()] = RowAbove::IsAsked;
        asked.append(current);
        const Layout& layout = program.at(current);
        LayoutID parent;
        if (layout.kind != LayoutKind::Instance || !layout.constructor || !walker.plan().hasClassRecord(current)
            || (layout.has(LayoutFlags::IsConstructorFunction) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
            || std::ranges::any_of(walker.facts().membersOf(current), hasMemberUnderComputedKey)
            || std::ranges::any_of(std::ranges::equal_range(link.members, current, { }, &LinkedMember::classLayout), hasMemberUnderComputedKey)
            || !findParentNamedByClassRecordForTypedAccess(walker, current, parent)) {
            answer = RowAbove::Fails;
            break;
        }
        current = parent;
    }
    for (LayoutID layout : asked)
        kept.rowsAboveClasses[layout.index()] = answer;
    return answer == RowAbove::Passes;
}

bool hasOnlyNamedMembersAbove(Walker& walker, LayoutID classLayout)
{
    return rowAboveClassPassesWhateverTheNameForTypedAccess(walker, keptByTypedAccesses(walker), classLayout);
}

static bool someClassHasMemberOfPrototypeUnderForTypedAccess(const Walker& walker, KeptByTypedAccesses& kept, PropertyKeyID key)
{
    if (liesInThread(key))
        return false;
    if (!kept.keysOfMembersOfPrototypes) {
        BitVector keys;
        auto note = [&](const auto& declared) {
            if (declared.side == ClassSide::Prototype && declared.key)
                keys.set(positionOf(declared.key));
        };
        for (uint32_t index = 0; index < walker.program().layouts.size(); ++index)
            std::ranges::for_each(walker.facts().membersOf(LayoutID(index)), note);
        std::ranges::for_each(linkTablesForTypedAccesses(walker).members, note);
        kept.keysOfMembersOfPrototypes = WTF::move(keys);
    }
    return kept.keysOfMembersOfPrototypes->get(positionOf(key));
}

static constexpr unsigned classesGoneThroughBeforeRowIsLookedUp = 8;

static bool firstFunctionObjectIsClassForTypedAccess(const Program& program, const LinkTables& link, LayoutID member, LayoutID classLayout)
{
    return program.at(classLayout).has(LayoutFlags::IsConstructorFunction) && possibleChangesForTypedAccess(link, member).contains(LayoutChange::ChangesPrototype);
}

static WhoAnswersForWhatStandsAbove whoAnswersForWhatStandsAboveObjectsOf(const Walker& walker, LayoutID member, PropertyKeyID key, LookupAbove lookup, LayoutID& classLayout, ASCIILiteral* whyNobody = nullptr)
{
    auto nobody = [&](ASCIILiteral why) {
        if (whyNobody)
            *whyNobody = why;
        return WhoAnswersForWhatStandsAbove::Nobody;
    };
    const Program& program = walker.program();
    if (objectPrototypeMayGainNameUnder(walker, key))
        return nobody("a test run may lay the name on Object.prototype"_s);
    if (program.at(member).kind == LayoutKind::Record)
        return nothingAnswersAboveObjectsOfForTypedAccess(program, linkTablesForTypedAccesses(walker), member, key) ? WhoAnswersForWhatStandsAbove::NothingStandsThere : nobody("something above a literal's object may answer"_s);

    const LinkTables& link = linkTablesForTypedAccesses(walker);
    classLayout = declaredLayoutOf(program, member);
    if (isOnObjectPrototype(program.nameClassOf(key)))
        return nobody("Object.prototype has the name"_s);
    if (program.at(member).has(LayoutFlags::IsOfCreatedObject)) {
        if (!walker.plan().hasClassRecord(classLayout))
            return nobody("the layout is of objects made by Object.create, and it has no record"_s);
        if (walker.facts().hasMemberUnder(classLayout, ClassSide::Prototype, key) || walker.facts().hasMemberUnder(classLayout, ClassSide::Static, key))
            return nobody("something that is known to stand above the created object has the name"_s);
        return WhoAnswersForWhatStandsAbove::GuardOfClass;
    }
    if (!walker.facts().isEvaluatedAtMostOnce(classLayout) && (lookup != LookupAbove::OfStoreIntoCellMadeHere || !firstFunctionObjectIsClassForTypedAccess(program, link, member, classLayout)))
        return nobody("the class may be evaluated again"_s);
    size_t numberOfClassesLeft = program.layouts.size();
    unsigned numberOfClassesGoneThrough = 0;
    for (LayoutID current = classLayout; current;) {
        if (!whyNobody && numberOfClassesGoneThrough++ == classesGoneThroughBeforeRowIsLookedUp) {
            KeptByTypedAccesses& kept = keptByTypedAccesses(const_cast<Walker&>(walker));
            if (!rowAboveClassPassesWhateverTheNameForTypedAccess(walker, kept, current))
                return WhoAnswersForWhatStandsAbove::Nobody;
            if (!someClassHasMemberOfPrototypeUnderForTypedAccess(walker, kept, key))
                return WhoAnswersForWhatStandsAbove::GuardOfClass;
        }
        const Layout& layout = program.at(current);
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || !layout.constructor || !walker.plan().hasClassRecord(current))
            return nobody("a class on the way up has no record"_s);
        if (layout.has(LayoutFlags::IsConstructorFunction) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
            return nobody("a constructor function on the way up holds captures"_s);
        if (walker.facts().hasMemberUnder(current, ClassSide::Prototype, { }) || walker.facts().hasMemberUnder(current, ClassSide::Prototype, key))
            return nobody("a record on the way up declares the name, or a member under a computed key"_s);
        bool linkHasSeenMember = !link.membersUnder(current, ClassSide::Prototype, { }).empty() || std::ranges::any_of(link.membersUnder(current, ClassSide::Prototype, key), [&](uint32_t place) {
            return lookup == LookupAbove::OfRead || link.members[place].kind != MethodKind::Method;
        });
        if (linkHasSeenMember)
            return nobody("Link has seen a member defined under the name, or under a computed key"_s);
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return nobody("the row of classes goes on above the program's own"_s);
        current = parent;
    }
    return WhoAnswersForWhatStandsAbove::GuardOfClass;
}

static bool lookupPassesAboveReceiversForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, LookupAbove lookup, const WalkState* stateAskedAboutGuards)
{
    if (field.isNamedAbove)
        return false;
    if (!receivers.possibleChanges.contains(LayoutChange::ChangesPrototype)) {
        if (nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key))
            return true;
        if (lookup == LookupAbove::OfRead)
            return false;
    }
    auto askEachMember = [&] {
        KeptByTypedAccesses::AnswersAbove answers;
        const LinkTables& link = linkTablesForTypedAccesses(walker);
        for (uint32_t place : link.membersUnderKeyAlongRows(ClassSide::Prototype, key)) {
            LayoutID classLayout;
            if (std::ranges::binary_search(receivers.members, link.members[place].classLayout) && whoAnswersForWhatStandsAboveObjectsOf(walker, link.members[place].classLayout, key, lookup, classLayout) == WhoAnswersForWhatStandsAbove::Nobody)
                return answers;
        }
        WordsListedOnce classesListed;
        answers.somebodyAnswersForEveryMember = std::ranges::all_of(receivers.members, [&](LayoutID member) {
            LayoutID classLayout;
            WhoAnswersForWhatStandsAbove who = whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, lookup, classLayout);
            if (who == WhoAnswersForWhatStandsAbove::GuardOfClass)
                classesListed.appendUnlessListed(answers.classesWhoseGuardsAnswer, classLayout);
            return who != WhoAnswersForWhatStandsAbove::Nobody;
        });
        return answers;
    };
    auto answerBy = [&](const KeptByTypedAccesses::AnswersAbove& answers) {
        if (!answers.somebodyAnswersForEveryMember || !stateAskedAboutGuards || walker.shouldAssumeGuardsAndFacts())
            return answers.somebodyAnswersForEveryMember;
        return std::ranges::all_of(answers.classesWhoseGuardsAnswer, [&](LayoutID classLayout) {
            return lookup == LookupAbove::OfRead ? stateAskedAboutGuards->guardOfReadsHolds(classLayout, key) : stateAskedAboutGuards->storesPassAbove(classLayout, key);
        });
    };
    bool answerIsKept = lookup != LookupAbove::OfStoreIntoCellMadeHere && !liesInThread(key);
    if (receivers.family && answerIsKept) {
        auto& answersAboveFamilies = keptByTypedAccesses(const_cast<Walker&>(walker)).answersAboveFamilies;
        uint64_t asked = static_cast<uint64_t>(receivers.family.index() + 1) << 33 | static_cast<uint64_t>(key.bits()) << 1 | (lookup == LookupAbove::OfRead);
        auto known = answersAboveFamilies.find(asked);
        if (known == answersAboveFamilies.end())
            known = answersAboveFamilies.add(asked, askEachMember()).iterator;
        return answerBy(known->value);
    }
    if (TypeArena::SetOfRound round = answerIsKept ? walker.types().setOfRound(receivers.set) : TypeArena::SetOfRound(); round.isKeptForBuild) {
        auto& answersAboveSets = keptByTypedAccesses(const_cast<Walker&>(walker)).answersAboveSets;
        std::pair<uint64_t, uint64_t> asked { static_cast<uint64_t>(round.word) << 32 | key.bits(), lookup == LookupAbove::OfRead };
        auto known = answersAboveSets.find(asked);
        if (known == answersAboveSets.end())
            known = answersAboveSets.add(asked, askEachMember()).iterator;
        return answerBy(known->value);
    }
    return answerBy(askEachMember());
}

static void askAgainGuardsAboveReceiversForTypedAccess(Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, bool stores, Origin origin)
{
    if ((!walker.checksProofs() && !walker.checksWalk()) || walker.isUnreachable())
        return;
    bool asksWhatReadLeansOn = !stores && walker.checksProofs() && receivers.possibleChanges.contains(LayoutChange::ChangesPrototype);
    Vector<MemberConditionRecord, 8> remembered;
    WordsListedOnce rememberedListed;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout;
        if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, stores ? LookupAbove::OfStore : LookupAbove::OfRead, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass)
            continue;
        if (!asksWhatReadLeansOn && !(stores ? walker.state().storesPassAbove(classLayout, key) : walker.state().guardOfReadsHolds(classLayout, key)))
            continue;
        MemberConditionRecord condition { stores ? MemberConditionKind::MemberPassedByStores : MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
        rememberedListed.appendUnlessListed(remembered, condition);
    }
    if (remembered.isEmpty())
        return;
    walker.askGuardsAgainWhereTheyAreLeanedOn(remembered.span(), origin);
}

void checkGuardsThatAreRemembered(Walker& walker, std::span<const MemberConditionRecord> remembered, Origin origin)
{
    if (!walker.checksProofs() || remembered.empty() || walker.isUnreachable())
        return;
    Draft test(Opcode::AreMembersUnchanged, origin);
    test.setConditions(remembered);
    Reference holds = walker.buildTestOfCheck(WTF::move(test));
    if (holds && holds.isValue() && !walker.isUnreachable())
        walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
}

static bool storeAsksGuardsWherePrototypeStaysForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    return !field.isNamedAbove && !receivers.possibleChanges.contains(LayoutChange::ChangesPrototype) && !nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key);
}

static bool mayCreateInTypedStore(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, bool isDefinition, OptionSet<CellFact> knownOfCell, const WalkState* stateAskedAboutGuards, LookupAbove lookup = LookupAbove::OfStore)
{
    bool emptySlotSaysThatPropertyIsMissing = field.isMovedOutByNoStore || knownOfCell.contains(CellFact::NoFieldOutsideSlot);
    if (!emptySlotSaysThatPropertyIsMissing || possibleChangesForbid(receivers.possibleChanges, isDefinition ? TypedAccess::DefinesSlotThatMayBeEmpty : TypedAccess::StoresIntoSlotThatMayBeEmpty, knownOfCell))
        return false;
    return isDefinition || lookupPassesAboveReceiversForTypedAccess(walker, receivers, field, key, lookup, stateAskedAboutGuards);
}

static bool mayFindSlotEmptyBehindLooksInTypedRead(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, OptionSet<CellFact> knownOfCell, const WalkState* stateAskedAboutGuards)
{
    bool emptySlotSaysThatPropertyIsMissing = field.isMovedOutByNoStore || knownOfCell.contains(CellFact::NoFieldOutsideSlot);
    if (!emptySlotSaysThatPropertyIsMissing || possibleChangesForbid(changesThatMayReachFieldForTypedAccess(receivers, field), TypedAccess::ReadsSlotThatMayBeEmpty, knownOfCell))
        return false;
    return lookupPassesAboveReceiversForTypedAccess(walker, receivers, field, key, LookupAbove::OfRead, stateAskedAboutGuards);
}

static bool readAsksGuardsForTypedAccess(const ReceiversOfTypedAccess& receivers, OptionSet<CellFact> askedOfCell)
{
    return askedOfCell.contains(CellFact::HasLayoutPrototype) || receivers.possibleChanges.contains(LayoutChange::ChangesPrototype);
}

static bool storeAsksGuardsForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, OptionSet<CellFact> askedOfCell)
{
    return readAsksGuardsForTypedAccess(receivers, askedOfCell) || storeAsksGuardsWherePrototypeStaysForTypedAccess(walker, receivers, field, key);
}

static constexpr auto choicesAmongFactsThatLooksAtCellShow = [] {
    constexpr unsigned numberOfFactsShown = std::size(factsThatLooksAtCellShow);
    std::array<OptionSet<CellFact>, 1u << numberOfFactsShown> choices { };
    size_t numberOfChoices = 0;
    for (unsigned size = 0; size <= numberOfFactsShown; ++size) {
        for (unsigned choice = 0; choice < 1u << numberOfFactsShown; ++choice) {
            if (static_cast<unsigned>(std::popcount(choice)) != size)
                continue;
            OptionSet<CellFact> facts;
            for (unsigned index = 0; index < numberOfFactsShown; ++index) {
                if (choice >> index & 1)
                    facts.add(factsThatLooksAtCellShow[index]);
            }
            choices[numberOfChoices++] = facts;
        }
    }
    return choices;
}();

static std::optional<OptionSet<CellFact>> smallestSufficientCellFacts(NOESCAPE const Invocable<bool(OptionSet<CellFact>)> auto& passes)
{
    for (OptionSet<CellFact> facts : choicesAmongFactsThatLooksAtCellShow) {
        if (passes(facts))
            return facts;
    }
    return std::nullopt;
}

static std::optional<OptionSet<CellFact>> askAheadAboutReadOfSlotThatMayBeEmpty(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    return smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return mayFindSlotEmptyBehindLooksInTypedRead(walker, receivers, field, key, facts, nullptr);
    });
}

static std::optional<OptionSet<CellFact>> askAheadAboutReadOffObjectPrototype(const ReceiversOfTypedAccess& receivers)
{
    return smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return !changesForbidReadOffObjectPrototype(receivers, facts);
    });
}

static TypedForm buildReadOffObjectPrototypeForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    if (value.opcode() != Opcode::PropGet || value.isFreeName())
        return TypedForm::None;
    BuiltinID location = findMemberOfObjectPrototypeForTypedAccess(walker, receivers, field, key);
    std::optional<OptionSet<CellFact>> asked = location ? askAheadAboutReadOffObjectPrototype(receivers) : std::nullopt;
    if (!asked)
        return TypedForm::None;
    if (operands.areOnlyTypes) {
        TypedForm form = buildReadOfMemberOffObjectPrototype(walker, value, operands.typeOfObject, { }, location);
        if (form == TypedForm::None || asked->isEmpty())
            return form;
        return walker.isInLoop() || walker.weighsWhatIsWritten() ? TypedForm::BuiltBehindLookAtCell : TypedForm::None;
    }
    if (!knownCellFacts(walker, operands.object).containsAll(*asked))
        return TypedForm::None;
    walker.checkFactsRememberedOfCell(operands.object, walker.originOf(value), *asked);
    return buildReadOfMemberOffObjectPrototype(walker, value, operands.typeOfObject, operands.object, location);
}

static bool statementsLayNameAboveObjectsOfForTypedAccess(const Walker& walker, LayoutID member, PropertyKeyID key, bool guardOfNameAnswersForUnknownKeys = false, KeptByTypedAccesses* climbedByOtherMembers = nullptr)
{
    const Program& program = walker.program();
    if (program.at(member).kind != LayoutKind::Instance)
        return false;
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    size_t numberOfClassesLeft = program.layouts.size();
    size_t firstOfThisClimb = climbedByOtherMembers ? climbedByOtherMembers->classesToForgetBehindQuestion.size() : 0;
    for (LayoutID current = declaredLayoutOf(program, member); current;) {
        if (!numberOfClassesLeft--)
            return true;
        const_cast<Walker&>(walker).count(StatisticID::AncestorClassesVisited);
        if (climbedByOtherMembers) {
            if (climbedByOtherMembers->classesClimbedForQuestion.set(current.index()))
                return std::ranges::contains(climbedByOtherMembers->classesToForgetBehindQuestion.span().subspan(firstOfThisClimb), static_cast<uint32_t>(current.index()));
            climbedByOtherMembers->classesToForgetBehindQuestion.append(current.index());
        }
        for (const LinkedNameOnPrototype& name : std::ranges::equal_range(namesOnPrototypes, current, { }, &LinkedNameOnPrototype::layout)) {
            if (name.key ? name.key == key : !guardOfNameAnswersForUnknownKeys)
                return true;
        }
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return true;
        current = parent;
    }
    return false;
}

static bool findsNothingBehindLooksInTypedRead(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, OptionSet<CellFact> knownOfCell, const WalkState* stateAskedAboutGuards)
{
    const Program& program = walker.program();
    if (field.coverage != CoverageOfTypedAccess::NoReceiver || !isLookedUpAsNameForTypedAccess(program, key) || program.at(key).kind != PropertyKeyKind::Text)
        return false;
    if (possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty, knownOfCell))
        return false;
    const SlotPlan& slots = walker.plan().slots();
    bool guardOfNameAnswersForUnknownKeys = receivers.possibleChanges.contains(LayoutChange::ChangesPrototype);
    std::optional<uint64_t> asked = questionKeptAboutReceiversForTypedAccess(walker, receivers, key);
    auto someMemberHasField = [&] {
        Vector<uint32_t, 16> places;
        if (!listMembersThatMayHaveKeyForTypedAccess(const_cast<Walker&>(walker), receivers, key, places)) {
            return std::ranges::any_of(receivers.members, [&](LayoutID member) {
                return !!slots.findField(program, member, key);
            });
        }
        return std::ranges::any_of(places, [&](uint32_t place) {
            return !!slots.findField(program, receivers.members[place], key);
        });
    };
    auto someMemberFindsNameLaidAbove = [&] {
        KeptByTypedAccesses* climbed = receivers.members.size() > 1 ? &keptByTypedAccesses(const_cast<Walker&>(walker)) : nullptr;
        bool someFinds = std::ranges::any_of(receivers.members, [&](LayoutID member) {
            return statementsLayNameAboveObjectsOfForTypedAccess(walker, member, key, guardOfNameAnswersForUnknownKeys, climbed);
        });
        if (climbed) {
            for (uint32_t index : climbed->classesToForgetBehindQuestion)
                climbed->classesClimbedForQuestion.clear(index);
            climbed->classesToForgetBehindQuestion.shrink(0);
        }
        return someFinds;
    };
    auto askEachMember = [&] {
        if (someMemberHasField())
            return false;
        if (!asked)
            return !someMemberFindsNameLaidAbove();
        KeptByTypedAccesses& kept = keptByTypedAccesses(const_cast<Walker&>(walker));
        if (!kept.namesLaidOnPrototypes) {
            KeptByTypedAccesses::NamesLaidOnPrototypes laid;
            for (const LinkedNameOnPrototype& name : walker.facts().held().namesOnPrototypes) {
                if (name.key)
                    laid.keys.set(positionOf(name.key));
                else
                    laid.someLieUnderKeysNobodyKnows = true;
            }
            kept.namesLaidOnPrototypes = WTF::move(laid);
        }
        if (kept.namesLaidOnPrototypes->keys.get(positionOf(key)) || (kept.namesLaidOnPrototypes->someLieUnderKeysNobodyKnows && !guardOfNameAnswersForUnknownKeys))
            return !someMemberFindsNameLaidAbove();
        uint32_t wordOfReceivers = static_cast<uint32_t>(*asked >> 32);
        auto known = kept.whetherSomeRowOfBasesIsNotNamed.find(wordOfReceivers);
        if (known == kept.whetherSomeRowOfBasesIsNotNamed.end()) {
            bool someIsNotNamed = someMemberFindsNameLaidAbove();
            known = kept.whetherSomeRowOfBasesIsNotNamed.add(wordOfReceivers, someIsNotNamed).iterator;
        }
        return !known->value;
    };
    bool noMemberHasNameOrFindsItLaidAbove;
    if (asked) {
        auto& kept = keptByTypedAccesses(const_cast<Walker&>(walker)).whetherNoMemberHasNameOrFindsItLaidAbove;
        auto known = kept.find(*asked);
        if (known == kept.end())
            known = kept.add(*asked, askEachMember()).iterator;
        noMemberHasNameOrFindsItLaidAbove = known->value;
    } else
        noMemberHasNameOrFindsItLaidAbove = askEachMember();
    if (!noMemberHasNameOrFindsItLaidAbove)
        return false;
    return lookupPassesAboveReceiversForTypedAccess(walker, receivers, field, key, LookupAbove::OfRead, stateAskedAboutGuards);
}

static std::optional<OptionSet<CellFact>> askAheadAboutReadThatFindsNothing(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    OptionSet<CellFact> everyFact;
    for (CellFact fact : factsThatLooksAtCellShow)
        everyFact.add(fact);
    if (!findsNothingBehindLooksInTypedRead(walker, receivers, field, key, everyFact, nullptr))
        return std::nullopt;
    return smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return !possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty, facts);
    });
}

static OptionSet<CellFact> cellFactsForKeyedLookup(const Walker& walker, Reference object, PropertyKeyID key)
{
    OptionSet<CellFact> known = knownCellFacts(walker, object);
    if (findKnown(walker, KnownNote::NoExtraPropertyUnderKey, object, { }, key.bits()))
        known.add(CellFact::NoExtraProperties);
    return known;
}

static Draft draftThatAsksRunTimeForNoExtraPropertyUnderKey(Walker& walker, Reference object, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, Origin origin)
{
    Draft test(Opcode::FindsNoMember, origin);
    walker.chooseConservativeFields(test);
    test.setObjectOperand(object);
    test.setKey(key);
    test.setReceivers(receivers.set);
    test.setAsksForNoExtraPropertyUnderKey(true);
    test.setAsksRunTime(true);
    return test;
}

static void askAgainWhetherFieldHidesAccessorForTypedAccess(Walker& walker, Reference object, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, Origin origin)
{
    if ((!walker.checksProofs() && !walker.checksWalk()) || !object || walker.isUnreachable())
        return;
    origin.isTailCall = false;
    walker.verifyRememberedTest(draftThatAsksRunTimeForNoExtraPropertyUnderKey(walker, object, receivers, key, origin));
}

static void checkNoteTakenForLookupUnderKey(Walker& walker, Reference object, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, Origin origin)
{
    if (!walker.checksProofs() || walker.isUnreachable() || walker.state().cellFacts(object).contains(CellFact::NoExtraProperties) || !findKnown(walker, KnownNote::NoExtraPropertyUnderKey, object, { }, key.bits()))
        return;
    Reference holds = walker.buildTestOfCheck(draftThatAsksRunTimeForNoExtraPropertyUnderKey(walker, object, receivers, key, origin));
    if (holds && holds.isValue() && !walker.isUnreachable())
        walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
}

static bool paysToLookAtCellWhereReadFindsNothing(const Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    if (walker.isInLoop() || walker.weighsWhatIsWritten())
        return true;
    Reference bound = walker.referenceOf(read.objectOperand());
    TypeID proven = bound ? walker.typeOf(bound) : TypeID();
    return bound && (walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) || (proven && walker.types().isSubset(walker.types().withoutNullish(proven), typeOfObject)));
}

static bool looksAtCellShowThatReadFindsNothing(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    const LinkTables& held = walker.facts().held();
    auto askEachMember = [&] {
        return std::ranges::none_of(receivers.members, [&](LayoutID member) {
            if (walker.program().at(member).kind == LayoutKind::Instance && (member.index() >= held.layouts.size() || !held.layout(member).listsEveryMemberAbove))
                return true;
            return !walker.weighsWhatIsWritten() && possibleChangesForbid(possibleChangesForTypedAccess(held, member), TypedAccess::FindsNoProperty);
        });
    };
    bool everyMemberMayBeAsked;
    if (std::optional<uint64_t> asked = questionKeptAboutReceiversForTypedAccess(walker, receivers, key)) {
        auto& kept = keptByTypedAccesses(const_cast<Walker&>(walker)).whetherEveryMemberMayBeAskedWhetherReadFindsNothing;
        uint64_t word = *asked >> 32 << 1 | static_cast<uint64_t>(walker.weighsWhatIsWritten());
        auto known = kept.find(word + 1);
        if (known == kept.end())
            known = kept.add(word + 1, askEachMember()).iterator;
        everyMemberMayBeAsked = known->value;
    } else
        everyMemberMayBeAsked = askEachMember();
    return everyMemberMayBeAsked && askAheadAboutReadThatFindsNothing(walker, receivers, field, key);
}

static TypedForm buildReadThatFindsNothingBehindLooksForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    if (value.opcode() != Opcode::PropGet || value.isFreeName())
        return TypedForm::None;
    if (operands.areOnlyTypes) {
        if (isReadForCallAloneInTypedAccess(walker, value))
            return TypedForm::None;
        return paysToLookAtCellWhereReadFindsNothing(walker, value, operands.typeOfObject) && looksAtCellShowThatReadFindsNothing(walker, receivers, field, key) ? TypedForm::BuiltBehindLookAtCell : TypedForm::None;
    }

    const WalkState& state = walker.state();
    if (!findsNothingBehindLooksInTypedRead(walker, receivers, field, key, cellFactsForKeyedLookup(walker, operands.object, key), &state))
        return TypedForm::None;
    if (walker.checksProofs()) {
        Origin origin = walker.originOf(value);
        OptionSet<CellFact> leanedOn = askAheadAboutReadThatFindsNothing(walker, receivers, field, key).value_or(OptionSet<CellFact>());
        if (findKnown(walker, KnownNote::NoExtraPropertyUnderKey, operands.object, { }, key.bits()))
            leanedOn.remove(CellFact::NoExtraProperties);
        walker.checkFactsRememberedOfCell(operands.object, origin, leanedOn);
        checkNoteTakenForLookupUnderKey(walker, operands.object, receivers, key, origin);
        Vector<MemberConditionRecord, 8> remembered;
        for (LayoutID member : receivers.members) {
            LayoutID classLayout;
            if (receivers.possibleChanges.contains(LayoutChange::ChangesPrototype) && whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, LookupAbove::OfRead, classLayout) == WhoAnswersForWhatStandsAbove::GuardOfClass)
                remembered.append(MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key });
        }
        checkGuardsThatAreRemembered(walker, remembered.span(), origin);
    }
    walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
    return TypedForm::Built;
}

static bool statementsLayNameOnPrototypeForTypedAccess(const Vector<LinkedNameOnPrototype>& namesOnPrototypes, LayoutID layout, PropertyKeyID key)
{
    return std::ranges::binary_search(namesOnPrototypes, std::pair { layout, key }, { }, [](const LinkedNameOnPrototype& name) {
        return std::pair { name.layout, name.key };
    });
}

static FunctionID findMethodDeclaredAboveObjectsOf(const Walker& walker, LayoutID member, PropertyKeyID key, LayoutID& classLayout, ASCIILiteral* whyNone = nullptr)
{
    auto none = [&](ASCIILiteral why) {
        if (whyNone)
            *whyNone = why;
        return FunctionID();
    };
    const Program& program = walker.program();
    if (program.at(member).kind != LayoutKind::Instance)
        return none("no field, and the layout is of no class"_s);
    if (!findsMemberWithoutCodeForTypedAccess(program, linkTablesForTypedAccesses(walker), key))
        return none("no field, and a lookup under the key may run code"_s);
    classLayout = declaredLayoutOf(program, member);
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    if (classHolderIsMadeWithName(program, ClassSide::Prototype, key)) {
        const Layout& layout = program.at(classLayout);
        if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.has(LayoutFlags::IsOfCreatedObject) || (layout.has(LayoutFlags::IsConstructorFunction) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor)) || !walker.plan().hasClassRecord(classLayout) || !walker.facts().isEvaluatedAtMostOnce(classLayout)
            || walker.facts().hasMemberUnder(classLayout, ClassSide::Prototype, { }) || walker.facts().hasMemberUnder(classLayout, ClassSide::Prototype, key) || statementsLayNameOnPrototypeForTypedAccess(namesOnPrototypes, classLayout, key))
            return none("no field, and every prototype object is made with the name"_s);
        return layout.constructor;
    }
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = classLayout; current;) {
        const Layout& layout = program.at(current);
        bool isOfCreatedObjects = layout.has(LayoutFlags::IsOfCreatedObject);
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || (!layout.constructor && !isOfCreatedObjects) || !walker.plan().hasClassRecord(current))
            return none("no field, and a class on the way up has no record"_s);
        if (!isOfCreatedObjects && !walker.facts().isEvaluatedAtMostOnce(current))
            return none("no field, and a class on the way up may be evaluated again"_s);
        if (layout.has(LayoutFlags::IsConstructorFunction) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
            return none("no field, and a constructor function on the way up holds captures"_s);
        if (walker.facts().hasMemberUnder(current, ClassSide::Prototype, { }))
            return none("no field, and a class on the way up has a member under a computed key"_s);
        std::span<const uint32_t> places = walker.facts().placesOfMembersUnder(current, ClassSide::Prototype, key);
        if (places.size() > 1)
            return none("no field, and a class lists the name twice"_s);
        if (!places.empty()) {
            const ClassMember& found = walker.facts().membersOf(current)[places[0]];
            if (found.kind != MethodKind::Method)
                return none("no field: an accessor above"_s);
            return found.function ? found.function : none("no field: a method above of which the record names no function"_s);
        }
        if (statementsLayNameOnPrototypeForTypedAccess(namesOnPrototypes, current, key))
            return none("no field, and statements lay more than one thing under the name on a prototype object"_s);
        if (isOfCreatedObjects)
            return none("no field, and nothing that is known to stand above the created object has the name"_s);
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return none("no field, and the row of classes goes on above the program's own"_s);
        current = parent;
    }
    return none("no field, and no class of the row declares the name"_s);
}

static bool cellSaysWhetherObjectHidesMethodForTypedAccess(const Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    const LinkTables& held = walker.facts().held();
    if (classLayout.index() >= held.layouts.size() || held.layout(classLayout).isFilledUnderComputedKeys)
        return false;
    if (held.layout(classLayout).instancesAreLaidAsPrototypes)
        return true;
    std::span<const LinkedNameBesideFields> names = held.namesStoredBesideFieldsOf(classLayout);
    if (!std::ranges::contains(names, key, &LinkedNameBesideFields::key))
        return false;
    return std::ranges::all_of(names, [&](const LinkedNameBesideFields& name) {
        LayoutID classOfObjects;
        return !!findMethodDeclaredAboveObjectsOf(walker, classLayout, name.key, classOfObjects);
    });
}

static MemberConditionRecord conditionOnMethodAboveObjectsOf(const Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    bool cellSaysWhetherObjectHides = cellSaysWhetherObjectHidesMethodForTypedAccess(walker, classLayout, key);
    return { cellSaysWhetherObjectHides ? MemberConditionKind::MemberUnchangedAbove : MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
}

static bool conditionOnMethodAboveObjectsIsKnownToHold(const Walker& walker, const WalkState& state, LayoutID classLayout, PropertyKeyID key)
{
    if (walker.shouldAssumeGuardsAndFacts())
        return true;
    if (cellSaysWhetherObjectHidesMethodForTypedAccess(walker, classLayout, key))
        return state.memberAboveIsUnchanged(classLayout, key);
    return state.guardOfReadsHolds(classLayout, key);
}

static std::optional<OptionSet<CellFact>> askAheadAboutReadOfMethodAbove(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    bool cellSaysWhetherObjectHides = std::ranges::any_of(receivers.members, [&](LayoutID member) {
        return cellSaysWhetherObjectHidesMethodForTypedAccess(walker, declaredLayoutOf(walker.program(), member), key);
    });
    return smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return !possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove, facts, true) && (!cellSaysWhetherObjectHides || facts.contains(CellFact::NoExtraProperties));
    });
}

static bool recordsDeclareMemberAboveObjectsOfLayout(const Walker& walker, LayoutID member, PropertyKeyID key, LayoutID declaredAtOrAbove)
{
    const Program& program = walker.program();
    if (program.at(member).kind != LayoutKind::Instance || !findsMemberWithoutCodeForTypedAccess(program, linkTablesForTypedAccesses(walker), key) || classHolderIsMadeWithName(program, ClassSide::Prototype, key))
        return false;
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    size_t numberOfClassesLeft = program.layouts.size();
    bool hasReachedClassThatMayDeclare = !declaredAtOrAbove;
    for (LayoutID current = declaredLayoutOf(program, member); current;) {
        const Layout& layout = program.at(current);
        hasReachedClassThatMayDeclare |= current == declaredAtOrAbove;
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || (!layout.constructor && !layout.has(LayoutFlags::IsOfCreatedObject)) || !walker.plan().hasClassRecord(current))
            return false;
        if (layout.has(LayoutFlags::IsConstructorFunction)
            && (!walker.facts().isEvaluatedAtMostOnce(current) || program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor)))
            return false;
        for (uint32_t place : walker.facts().placesOfMembersUnder(current, ClassSide::Prototype, { })) {
            FunctionID function = walker.facts().membersOf(current)[place].function;
            if (!function || !isLibraryCode(program, program.at(function)))
                return false;
        }
        if (std::span<const uint32_t> places = walker.facts().placesOfMembersUnder(current, ClassSide::Prototype, key); !places.empty()) {
            auto laid = std::ranges::equal_range(namesOnPrototypes, std::pair { current, key }, { }, [](const LinkedNameOnPrototype& name) {
                return std::pair { name.layout, name.key };
            });
            if (std::ranges::any_of(laid, &LinkedNameOnPrototype::isStoredIntoFromOutside))
                return false;
            bool allAreOfLibrary = hasReachedClassThatMayDeclare && std::ranges::all_of(places, [&](uint32_t place) {
                FunctionID function = walker.facts().membersOf(current)[place].function;
                return function && isLibraryCode(program, program.at(function));
            });
            if (allAreOfLibrary && walker.verifiesCode() && layout.constructor && !isLibraryCode(program, program.at(layout.constructor))) [[unlikely]] {
                bool wasBrought = std::ranges::all_of(places, [&](uint32_t place) {
                    return program.at(program.at(walker.facts().membersOf(current)[place].function).module).kind == ModuleKind::RuntimeSource;
                });
                if (wasBrought) {
                    const_cast<Walker&>(walker).fail("a function of the library hangs on a class of the program, and no mark says that it was brought there"_s);
                    return false;
                }
            }
            if (allAreOfLibrary && walker.verifiesCode() && layout.constructor) [[unlikely]] {
                bool closureWasBrought = std::ranges::any_of(places, [&](uint32_t place) {
                    const Function& function = program.at(walker.facts().membersOf(current)[place].function);
                    return function.module != program.at(layout.constructor).module && !function.captures.isEmpty();
                });
                if (closureWasBrought) {
                    const_cast<Walker&>(walker).fail("a closure that another file makes hangs on a class, and no mark says that it was brought there"_s);
                    return false;
                }
            }
            return allAreOfLibrary;
        }
        if (statementsLayNameOnPrototypeForTypedAccess(namesOnPrototypes, current, key))
            return false;
        if (layout.has(LayoutFlags::IsOfCreatedObject))
            return false;
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return false;
        current = parent;
    }
    return false;
}

bool findGuardsOfMethodAboveObjectsOf(Walker& walker, TypeID typeOfObjects, PropertyKeyID key, Vector<MemberConditionRecord, 4>& conditions)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObjects);
    if (!receivers || describeFieldOfTypedAccess(walker, *receivers, key, { }, true).coverage != CoverageOfTypedAccess::NoReceiver)
        return false;
    if (findMemberAboveReceiversForTypedAccess(walker, *receivers, key).kind != MemberFound::Kind::Unknown)
        return false;
    Vector<LayoutID, 4> classes;
    for (LayoutID member : receivers->members) {
        LayoutID classLayout;
        if (!findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout))
            return false;
        classes.append(classLayout);
    }
    for (LayoutID classLayout : classes) {
        bool isListed = conditions.containsIf([&](const MemberConditionRecord& listed) {
            return listed.layout == classLayout && listed.key == key;
        });
        if (!isListed)
            conditions.append(conditionOnMethodAboveObjectsOf(walker, classLayout, key));
    }
    return !classes.isEmpty();
}

FunctionID findOnlyMethodThatRecordsDeclareAbove(Walker& walker, TypeID declaring, PropertyKeyID key)
{
    const Program& program = walker.program();
    FunctionID foundByAll;
    for (LayoutID member : membersOf(walker.types(), walker.types().at(declaring).layouts)) {
        FunctionID found;
        size_t numberOfClassesLeft = program.layouts.size();
        for (LayoutID current = declaredLayoutOf(program, member); current && numberOfClassesLeft--;) {
            FunctionID constructor = program.at(current).constructor;
            std::span<const uint32_t> places = walker.facts().placesOfMembersUnder(current, ClassSide::Prototype, key);
            if (!places.empty() && constructor && isLibraryCode(program, program.at(constructor))) {
                const ClassMember& declared = walker.facts().membersOf(current)[places[0]];
                if (places.size() == 1 && declared.kind == MethodKind::Method)
                    found = declared.function;
                break;
            }
            LayoutID parent;
            if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
                break;
            current = parent;
        }
        if (!found || (foundByAll && found != foundByAll))
            return { };
        foundByAll = found;
    }
    return foundByAll && walker.facts().linked().mayBeMentionedBy(walker.unit(), foundByAll) ? foundByAll : FunctionID();
}

TypeID objectsAboveWhichRecordsDeclareMember(Walker& walker, TypeID typeOfObjects, PropertyKeyID key, LayoutID declaredAtOrAbove)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObjects);
    if (!receivers)
        return { };
    auto hasFieldUnderKey = [&](const ReceiversOfTypedAccess& asked) {
        return describeFieldOfTypedAccess(walker, asked, key, { }, true).coverage != CoverageOfTypedAccess::NoReceiver;
    };
    bool someReceiverHasField = hasFieldUnderKey(*receivers);
    Vector<LayoutID, 8> declaring;
    for (LayoutID member : receivers->members) {
        if (someReceiverHasField) {
            std::optional<ReceiversOfTypedAccess> alone = findReceiversOfTypedAccess(walker, walker.types().forLayouts(std::span<const LayoutID>(&member, 1)));
            if (!alone || hasFieldUnderKey(*alone))
                continue;
        }
        if (recordsDeclareMemberAboveObjectsOfLayout(walker, member, key, declaredAtOrAbove))
            declaring.append(member);
    }
    return declaring.isEmpty() ? TypeID() : walker.types().forLayouts(declaring.span());
}

static FunctionID findMethodDeclaredAboveReceiversForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, ASCIILiteral* whyNone = nullptr)
{
    FunctionID foundByAll;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout;
        FunctionID found = findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout, whyNone);
        if (!found)
            return { };
        if (foundByAll && found != foundByAll) {
            if (whyNone)
                *whyNone = "no field: the layouts of the family come to several methods"_s;
            return { };
        }
        foundByAll = found;
    }
    return foundByAll;
}

static bool isReadForCallAloneInTypedAccess(const Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = read.opcode() == Opcode::PropGet ? index.onlyUsingValue(read.id()) : ValueID();
    ValueID written = read.id();
    while (user && inputPassThroughOpcodes.contains(index.opcodeOf(user))) {
        written = user;
        user = index.onlyUsingValue(user);
    }
    if (user && index.opcodeOf(user) == Opcode::Construct)
        return index.value(user).calleeOperand() == written && !index.value(user).as<Opcode::Construct>().newTargetOperand();
    return user && written == read.id() && (index.opcodeOf(user) == Opcode::Call || index.opcodeOf(user) == Opcode::CallList) && index.value(user).calleeOperand() == read.id();
}

static bool closureTableHoldsMethodForTypedAccess(const Walker& walker, FunctionID method)
{
    const Plan& plan = walker.plan();
    return plan.hasClosureCell(method) && walker.program().at(method).captures.size() == plan.promotedEnvironments().numberOfRemovedCapturesOf(method);
}

static TypedForm bindClosureThatClosureTableHoldsForTypedAccess(Walker& walker, const BodyValue& value, FunctionID method, Origin origin)
{
    Draft closure(Opcode::NewClosure, origin);
    closure.setFunction(method);
    closure.setIsHeldByClosureTable(true);
    walker.chooseConservativeFields(closure);
    walker.bind(value.id(), walker.build(WTF::move(closure)));
    return TypedForm::Built;
}

static FunctionID findMethodReadBehindLooksForTypedAccess(const Walker& walker, const BodyValue& read, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, ASCIILiteral* whyNone = nullptr)
{
    bool isForCallAlone = isReadForCallAloneInTypedAccess(walker, read);
    if (!walker.has(Mechanism::DirectCalls) || (!isForCallAlone && read.opcode() != Opcode::PropGet)) {
        if (whyNone)
            *whyNone = "no field, and the read is not there for a call alone"_s;
        return { };
    }
    if (!walker.isInLoop() && walker.unit().role == CodeRole::CoroutineBody) {
        if (whyNone)
            *whyNone = "no field: a method above, outside of loops"_s;
        return { };
    }
    FunctionID method = findMethodDeclaredAboveReceiversForTypedAccess(walker, receivers, key, whyNone);
    if (method && !walker.facts().linked().mayBeMentionedBy(walker.unit(), method)) {
        if (whyNone)
            *whyNone = "no field: a method above that the unit may not mention"_s;
        return { };
    }
    if (!isForCallAlone) {
        if (method && !closureTableHoldsMethodForTypedAccess(walker, method)) {
            if (whyNone)
                *whyNone = "no field: a method above that is read as a value, and the closure table does not hold it"_s;
            return { };
        }
        return method;
    }
    BodyValue user = walker.currentBodyIndex().value(walker.currentBodyIndex().onlyUsingValue(read.id()));
    if (method && user.opcode() == Opcode::CallList && !listSpreadBehindArgumentsIsRestListOf(const_cast<Walker&>(walker), user, method)) {
        if (whyNone)
            *whyNone = "no field: a method above, called with a list that is laid out"_s;
        return { };
    }
    return method;
}

void Walker::checkFactsRememberedOfCell(Reference object, Origin origin, OptionSet<CellFact> leanedOn)
{
    if (!checksProofs() || isUnreachable() || !object || !object.isValue())
        return;
    auto check = [&](Draft&& test) {
        if (isUnreachable())
            return;
        Reference holds = buildTestOfCheck(WTF::move(test));
        if (holds && holds.isValue() && !isUnreachable())
            checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
    };
    OptionSet<CellFact> remembered = m_state.cellFacts(object) | leanedOn;
    bool knowsWord = remembered.contains(CellFact::OrderWordIsKnown);
    if (knowsWord || remembered.contains(CellFact::NotFrozen)) {
        Draft test(Opcode::IsNotFrozen, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(knowsWord || remembered.contains(CellFact::NoFieldOutsideSlot));
        test.setHasNoExtraProperties(knowsWord || remembered.contains(CellFact::NoExtraProperties));
        check(WTF::move(test));
    } else if (remembered.contains(CellFact::NoExtraProperties)) {
        Draft test(Opcode::HasNoExtraProperties, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(remembered.contains(CellFact::NoFieldOutsideSlot));
        check(WTF::move(test));
    } else if (remembered.contains(CellFact::NoFieldOutsideSlot)) {
        Draft test(Opcode::HasNoFieldOutsideSlot, origin);
        test.setObjectOperand(object);
        test.setMayMeetUnfinishedObject(true);
        check(WTF::move(test));
    }
    if (TypeID proven = typeOf(object); remembered.contains(CellFact::BearsNoMark) && proven && !m_types.at(proven).primitives && m_types.at(proven).hasOnlyLayouts() && !m_types.at(proven).traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow)) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(object);
        test.setLayouts(m_types.at(proven).layouts);
        test.setRejectsMarkedObjects(true);
        check(WTF::move(test));
    } else if (remembered.contains(CellFact::BearsNoMark))
        ++m_untestableProofCount;
    if (remembered.contains(CellFact::HasLayoutPrototype)) {
        Draft test(Opcode::HasLayoutPrototype, origin);
        test.setObjectOperand(object);
        check(WTF::move(test));
    }

    std::optional<uint32_t> orderWord = m_state.orderWordOf(object);
    TypeID type = typeOf(object);
    LayoutID layout = orderWord && type ? singleLayoutOf(m_types, m_types.at(type)) : LayoutID();
    if (!layout || *orderWord & ~orderAndCountMask || isOrderStoredOutOfLine(*orderWord))
        return;
    const KeyOrders& orders = plan().keyOrders();
    uint32_t count = countOf(*orderWord);
    if (count && (orderOf(*orderWord) >= orders.numberOfOrdersOf(layout) || count > orders.order(layout, orderOf(*orderWord)).size()))
        return;
    std::span<const FieldIndex> listed;
    if (count)
        listed = orders.order(layout, orderOf(*orderWord)).first(count);
    if (orders.findOrderWord(layout, listed) != orderWord)
        return;
    Draft test(Opcode::HasKeyOrder, origin);
    test.setObjectOperand(object);
    test.setLayout(layout);
    test.setFields(listed);
    check(WTF::move(test));
}

void Walker::checkWhatIsInSightOfList(Reference list, Origin origin)
{
    if (!checksProofs() || isUnreachable() || isInLoop() || !list || !list.isValue())
        return;
    TypeID type = typeOf(list);
    std::optional<uint32_t> inSight = type && hasKind(m_types, type, OperandKind::Array) ? lengthOfListInSight(*this, list) : std::nullopt;
    if (!inSight)
        return;
    origin.isTailCall = false;
    verifyPristineObject(list);
    auto integer = [&](size_t number) {
        Draft draft(Opcode::Integer, origin);
        draft.setInteger(static_cast<int64_t>(number));
        return constant(WTF::move(draft));
    };
    Draft length(Opcode::GetArrayLength, origin);
    chooseConservativeFields(length);
    length.setArrayOperand(list);
    checkThatMemoryAnswersAsBelieved(integer(*inSight), WTF::move(length));
    Vector<std::pair<uint32_t, Reference>, 8> noted;
    for (const WalkState::ElementOfObject& element : m_state.listElementsOf(Reference::value(underlyingCellOf(m_code, list.id())))) {
        const Operation* index = element.index.isConstant() ? &m_code.at(element.index.id()) : nullptr;
        if (index && index->opcode == Opcode::Integer && index->integer() >= 0 && index->integer() < static_cast<int64_t>(*inSight))
            noted.append({ static_cast<uint32_t>(index->integer()), element.value });
    }
    std::ranges::sort(noted, { }, &std::pair<uint32_t, Reference>::first);
    m_untestableProofCount += *inSight - noted.size();
    for (auto [position, believed] : noted) {
        if (isUnreachable())
            break;
        TypeID typeBelieved = !believed.isVirtual() ? typeOf(believed) : TypeID();
        if (!typeBelieved || !m_types.at(typeBelieved).isJavaScriptValue() || (m_types.at(typeBelieved).primitives & Primitive::Absent)) {
            ++m_untestableProofCount;
            continue;
        }
        Draft read(Opcode::GetArrayElement, origin);
        chooseConservativeFields(read);
        read.setArrayOperand(list);
        read.setPositionOperand(integer(position));
        read.setKeyIsIntegral(true);
        checkThatMemoryAnswersAsBelieved(believed, WTF::move(read));
    }
}

static TypedForm buildReadOfMethodBehindLooksForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    FunctionID method = findMethodReadBehindLooksForTypedAccess(walker, value, receivers, key);
    if (!method)
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return TypedForm::BuiltBehindLookAtCell;

    const WalkState& state = walker.state();
    Reference readByLook = walker.referenceBoundTo(value.id());
    Reference closureReadByLook = readByLook && readByLook.isValue() && walker.code().at(readByLook.id()).opcode == Opcode::PropGet ? state.checkedNameOf(readByLook) : Reference();
    if (closureReadByLook && singleFunctionOf(walker.types(), walker.types().at(walker.typeOf(closureReadByLook))) != method)
        closureReadByLook = { };
    if (!closureReadByLook) {
        std::optional<OptionSet<CellFact>> askedOfCell = askAheadAboutReadOfMethodAbove(walker, receivers, key);
        if (!askedOfCell || !knownCellFacts(walker, operands.object).containsAll(*askedOfCell))
            return TypedForm::None;
    }
    for (LayoutID member : receivers.members) {
        LayoutID classLayout;
        if (!findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout) || !conditionOnMethodAboveObjectsIsKnownToHold(walker, state, classLayout, key))
            return TypedForm::None;
    }

    if (walker.checksProofs()) {
        Origin origin = walker.originOf(value);
        walker.checkFactsRememberedOfCell(operands.object, origin, closureReadByLook ? OptionSet<CellFact>() : askAheadAboutReadOfMethodAbove(walker, receivers, key).value_or(OptionSet<CellFact>()));
        Vector<MemberConditionRecord, 8> remembered;
        for (LayoutID member : receivers.members) {
            LayoutID classLayout;
            if (findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout))
                remembered.append(conditionOnMethodAboveObjectsOf(walker, classLayout, key));
        }
        checkGuardsThatAreRemembered(walker, remembered.span(), origin);
    }

    Draft written = walker.draftOf(value);
    if (classHolderIsMadeWithName(walker.program(), ClassSide::Prototype, key))
        walker.count(StatisticID::ConstructorReadsResolvedToClasses);
    if (closureTableHoldsMethodForTypedAccess(walker, method))
        return bindClosureThatClosureTableHoldsForTypedAccess(walker, value, method, written.origin());
    if (!closureReadByLook)
        return TypedForm::None;
    walker.bind(value.id(), closureReadByLook);
    return TypedForm::Built;
}

static bool findAccessorDeclaredAboveObjectsOf(const Walker& walker, LayoutID member, PropertyKeyID key, LayoutID& classLayout, FunctionID& getter, FunctionID& setter)
{
    const Program& program = walker.program();
    if (program.at(member).kind != LayoutKind::Instance || !findsMemberWithoutCodeForTypedAccess(program, linkTablesForTypedAccesses(walker), key) || classHolderIsMadeWithName(program, ClassSide::Prototype, key))
        return false;
    classLayout = declaredLayoutOf(program, member);
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = classLayout; current;) {
        const Layout& layout = program.at(current);
        bool isOfCreatedObjects = layout.has(LayoutFlags::IsOfCreatedObject);
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || (!layout.constructor && !isOfCreatedObjects) || !walker.plan().hasClassRecord(current) || (!isOfCreatedObjects && !walker.facts().isEvaluatedAtMostOnce(current)))
            return false;
        if (layout.has(LayoutFlags::IsConstructorFunction) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
            return false;
        if (walker.facts().hasMemberUnder(current, ClassSide::Prototype, { }))
            return false;
        std::span<const uint32_t> places = walker.facts().placesOfMembersUnder(current, ClassSide::Prototype, key);
        for (uint32_t place : places) {
            const ClassMember& declared = walker.facts().membersOf(current)[place];
            FunctionID* half = declared.kind == MethodKind::Getter ? &getter : declared.kind == MethodKind::Setter ? &setter : nullptr;
            if (!half || *half || !declared.function)
                return false;
            *half = declared.function;
        }
        if (!places.empty())
            return true;
        if (statementsLayNameOnPrototypeForTypedAccess(namesOnPrototypes, current, key))
            return false;
        if (isOfCreatedObjects)
            return false;
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return false;
        current = parent;
    }
    return false;
}

static FunctionID findHalfRunBehindLooksForTypedAccess(Walker& walker, const BodyValue& access, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    bool isStore = access.opcode() == Opcode::PropSet;
    if (!walker.has(Mechanism::DirectCalls) || (access.opcode() != Opcode::PropGet && !isStore) || access.isFreeName())
        return { };
    if (!walker.isInLoop() && walker.unit().role == CodeRole::CoroutineBody)
        return { };
    FunctionID halfOfAll;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout;
        FunctionID getter;
        FunctionID setter;
        if (!findAccessorDeclaredAboveObjectsOf(walker, member, key, classLayout, getter, setter))
            return { };
        FunctionID half = isStore ? setter : getter;
        if (!half || (halfOfAll && half != halfOfAll))
            return { };
        halfOfAll = half;
    }
    return halfOfAll && closureTableHoldsMethodForTypedAccess(walker, halfOfAll) && accessorAdmitsDirectCall(walker, halfOfAll) && walker.facts().linked().mayBeMentionedBy(walker.unit(), halfOfAll) ? halfOfAll : FunctionID();
}

static TypedForm buildAccessToAccessorBehindLooksForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    FunctionID half = findHalfRunBehindLooksForTypedAccess(walker, value, receivers, key);
    if (!half)
        return TypedForm::None;
    if (operands.areOnlyTypes) {
        walker.noteFormAskedAhead(Opcode::CallDirect, KnowledgeLevel::Proven, half);
        return TypedForm::BuiltBehindLookAtCell;
    }

    const WalkState& state = walker.state();
    if (possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove, knownCellFacts(walker, operands.object), true))
        return TypedForm::None;
    Vector<MemberConditionRecord, 8> remembered;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout;
        FunctionID getter;
        FunctionID setter;
        if (!findAccessorDeclaredAboveObjectsOf(walker, member, key, classLayout, getter, setter) || !(walker.shouldAssumeGuardsAndFacts() || state.guardOfReadsHolds(classLayout, key)))
            return TypedForm::None;
        remembered.append(MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key });
    }
    if (walker.checksProofs()) {
        Origin origin = walker.originOf(value);
        walker.checkFactsRememberedOfCell(operands.object, origin, smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
            return !possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove, facts, true);
        }).value_or(OptionSet<CellFact>()));
        checkGuardsThatAreRemembered(walker, remembered.span(), origin);
    }
    if (callAccessor(walker, value, half, operands.object, value.opcode() == Opcode::PropSet ? operands.stored : Reference()) || walker.hasFailed())
        return TypedForm::Built;
    Draft asWritten = walker.draftOf(value);
    walker.chooseConservativeFields(asWritten);
    walker.bind(value.id(), walker.build(WTF::move(asWritten)));
    return TypedForm::Built;
}

namespace {

struct FoundOffConstructorFunction {
    LayoutID classLayout;
    FunctionID method;
};

} // anonymous namespace

static std::optional<FoundOffConstructorFunction> findWhatRowOfRecordsExpectsOffConstructorFunction(Walker& walker, LayoutID classLayout, PropertyKeyID key, ASCIILiteral* whyNothing)
{
    auto nothing = [&](ASCIILiteral why) -> std::optional<FoundOffConstructorFunction> {
        if (whyNothing)
            *whyNothing = why;
        return std::nullopt;
    };
    const Program& program = walker.program();
    if (!findsMemberWithoutCodeForTypedAccess(program, linkTablesForTypedAccesses(walker), key) || classHolderIsMadeWithName(program, ClassSide::Static, key) || key == program.keyOf(BuiltinNames::arguments) || key == program.keyOf(BuiltinNames::caller))
        return nothing("a constructor function, and a name that every function has for itself, or a key that is no name"_s);
    if (!walker.has(Mechanism::DirectCalls))
        return nothing("a constructor function, and the read is not there for a call alone"_s);
    if (!walker.isInLoop() && walker.unit().role == CodeRole::CoroutineBody)
        return nothing("a constructor function, outside of loops in a coroutine"_s);

    const LinkTables& held = walker.facts().held();
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = classLayout; current;) {
        const Layout& layout = program.at(current);
        if (!numberOfClassesLeft-- || !layout.constructor || !walker.plan().hasClassRecord(current))
            return nothing("a constructor function or a class, and a class on the way up has no record"_s);
        if (!walker.facts().isEvaluatedAtMostOnce(current) || program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
            return nothing("a constructor function on the way up may have several function objects"_s);
        std::span<const uint32_t> places = walker.facts().placesOfMembersUnder(current, ClassSide::Static, key);
        if (places.size() > 1 || walker.facts().hasMemberUnder(current, ClassSide::Static, { }))
            return nothing("a constructor function, and a record lists the name twice, or a member under a computed key"_s);
        if (const ClassMember* found = places.empty() ? nullptr : &walker.facts().membersOf(current)[places[0]]) {
            if (found->kind != MethodKind::Method || !found->function || !closureTableHoldsMethodForTypedAccess(walker, found->function))
                return nothing("a constructor function, and what the record expects under the name is no function that the closure table holds"_s);
            if (!walker.facts().linked().mayBeMentionedBy(walker.unit(), found->function))
                return nothing("a constructor function, and the unit may not mention the function that the record expects under the name"_s);
            return FoundOffConstructorFunction { classLayout, found->function };
        }
        if (held.findNameOnFunction(layout.constructor, key) || !layout.staticFields.isEmpty())
            return nothing("a constructor function, and statements lay more than one thing under the name"_s);
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return nothing("a constructor function, and the row of classes goes on above the program's own"_s);
        current = parent;
    }
    if (!typeOfFunctionReadOffFunctionPrototype(walker, key))
        return nothing("a constructor function, and neither a record nor Function.prototype names a function there"_s);
    return FoundOffConstructorFunction { classLayout, { } };
}

static std::optional<FoundOffConstructorFunction> findWhatRecordsExpectOffConstructorFunction(Walker& walker, const BodyValue& read, TypeID typeOfObject, ASCIILiteral* whyNothing = nullptr)
{
    if (!typeOfObject || read.opcode() != Opcode::PropGet || read.isFreeName())
        return std::nullopt;
    Type type = walker.types().at(typeOfObject);
    FunctionID function = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    LayoutID classLayout = classOfConstructorFunction(walker.program(), function);
    if (!classLayout && function)
        classLayout = classOfClassObject(walker.program(), function);
    if (!classLayout)
        return std::nullopt;
    std::optional<FoundOffConstructorFunction> found = findWhatRowOfRecordsExpectsOffConstructorFunction(walker, classLayout, read.key(), whyNothing);
    if (found && !isReadForCallAloneInTypedAccess(walker, read)) {
        if (whyNothing)
            *whyNothing = "a constructor function, and the read is not there for a call alone"_s;
        return std::nullopt;
    }
    return found;
}

std::optional<MemberConditionRecord> findGuardOfCallOffConstructorFunction(Walker& walker, FunctionID function)
{
    LayoutID classLayout = classOfConstructorFunction(walker.program(), function);
    PropertyKeyID key = walker.program().keyOf(BuiltinNames::call);
    std::optional<FoundOffConstructorFunction> found = classLayout ? findWhatRowOfRecordsExpectsOffConstructorFunction(walker, classLayout, key, nullptr) : std::nullopt;
    if (!found || found->method)
        return std::nullopt;
    return MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Static, key };
}

static TypedForm buildReadOffConstructorFunctionBehindGuardForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands)
{
    std::optional<FoundOffConstructorFunction> found = findWhatRecordsExpectOffConstructorFunction(walker, value, operands.typeOfObject);
    if (!found)
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return found->method || buildReadOffFunctionPrototype(walker, value, operands.typeOfObject, { }) != TypedForm::None ? TypedForm::BuiltBehindLookAtCell : TypedForm::None;
    if (!operands.object || !walker.state().guardOfReadsHolds(found->classLayout, value.key(), ClassSide::Static))
        return TypedForm::None;

    Origin origin = walker.originOf(value);
    MemberConditionRecord remembered { MemberConditionKind::MemberUnchanged, found->classLayout, ClassSide::Static, value.key() };
    checkGuardsThatAreRemembered(walker, singleElementSpan(remembered), origin);
    if (!found->method)
        return buildReadOffFunctionPrototype(walker, value, operands.typeOfObject, operands.object);
    return bindClosureThatClosureTableHoldsForTypedAccess(walker, value, found->method, origin);
}

void noteGuardsShownByTest(Walker& walker, std::span<const MemberConditionRecord> conditions, Reference test, Reference receiver)
{
    WalkState& state = walker.state();
    for (const MemberConditionRecord& condition : conditions) {
        if (condition.kind == MemberConditionKind::MemberUnchanged)
            state.noteGuardOfReadsHolds(condition.layout, condition.key, test, condition.side);
        else if (condition.kind == MemberConditionKind::MemberUnchangedAbove)
            state.noteGuardOfReadsHolds(condition.layout, condition.key, test, condition.side, WalkState::AskerOfGuardOfReads::ReadBehindLookAtObject);
        else if (condition.kind == MemberConditionKind::MemberPassedByStores) {
            state.noteStoresPassAbove(condition.layout, condition.key, test);
            if (!walker.program().at(condition.layout).has(LayoutFlags::IsOfCreatedObject) || !walker.isAllocatedInTrialIteration(receiver))
                walker.noteGuardOfStoresAskedInTries(condition);
        }
        bool speaksOfReads = condition.kind == MemberConditionKind::MemberUnchanged || condition.kind == MemberConditionKind::MemberUnchangedAbove;
        if (speaksOfReads && (!state.guardsHaveBegun(condition.layout) || walker.turnBeginsGuardsOf(condition.layout)) && walker.turnMakesObjectAtOrBelow(condition.layout)) {
            walker.noteGuardOfStoresAskedInTries(condition);
            walker.noteClassesOfBirthsAtOrBelowInTries(condition.layout);
        }
        bool isAskedWhateverHasBegun = conditions.size() == 1 || condition.side == ClassSide::Static || condition.kind == MemberConditionKind::ClassEvaluatedOnce;
        if (isAskedWhateverHasBegun && walker.program().at(condition.layout).has(LayoutFlags::IsConstructorFunction))
            state.noteThatGuardsHaveBegun(condition.layout);
    }
}

void askWholeSideWhereGuardsMayNotHaveBegun(Walker& walker, LayoutID classLayout, Vector<MemberConditionRecord, 8>& conditions)
{
    MemberConditionRecord wholeSide { MemberConditionKind::ClassEvaluatedOnce, classLayout, ClassSide::Prototype, { } };
    if (conditions.size() > 1 && !walker.state().guardsHaveBegun(classLayout) && !conditions.contains(wholeSide))
        conditions.append(wholeSide);
}

static bool unitMayLeaveCellUnmadeForTypedAccess(Walker& walker, Reference cell)
{
    if (!cell || !cell.isValue())
        return false;
    Code& code = walker.code();
    Vector<ValueID, 8> valuesToAsk { cell.id() };
    Vector<ValueID, 8> parametersAsked;
    while (!valuesToAsk.isEmpty()) {
        ValueID value = valuesToAsk.takeLast();
        bool isNamedByTest = false;
        while (cellPreservingOpcodes.contains(code.at(value).opcode)) {
            isNamedByTest |= code.at(value).opcode == Opcode::Narrowed;
            value = code.at(value).operand(0);
        }
        const Operation& made = code.at(value);
        if (made.opcode == Opcode::NewObject || (effectRuleOf(made.opcode) == EffectRule::Call && made.traits().isRemovableWhenUnused()))
            return true;
        if (made.opcode != Opcode::Parameter || isNamedByTest || parametersAsked.contains(value))
            continue;
        parametersAsked.append(value);
        const Block& block = code.at(made.block());
        if (block.isLoopHead)
            return true;
        std::span<const ValueID> parameters = block.parameters();
        size_t position = std::ranges::find(parameters, value) - parameters.begin();
        code.seal(made.block()).forEach([&](const Edge& edge) {
            if (position < edge.arguments().size() && edge.arguments()[position])
                valuesToAsk.append(edge.arguments()[position]);
        });
    }
    return false;
}

static void forgetGuardsOfClassesThatMayNotHaveBegunForTypedAccess(Walker& walker, const ReceiversOfTypedAccess& receivers)
{
    WalkState& state = walker.state();
    Vector<LayoutID, 4> classes;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout = declaredLayoutOf(walker.program(), member);
        if (walker.program().at(classLayout).has(LayoutFlags::IsConstructorFunction) && !state.guardsHaveBegun(classLayout) && state.someGuardHoldsOf(classLayout))
            classes.append(classLayout);
    }
    if (classes.isEmpty())
        return;
    Effects forgotten;
    forgotten.writes.add(AbstractHeap::MemberGuards);
    auto noObjectIsWritten = [](Reference) {
        return false;
    };
    auto isForgotten = [&](LayoutID classOfGuard, PropertyKeyID) {
        return classes.contains(classOfGuard);
    };
    state.invalidate(forgotten, noObjectIsWritten, isForgotten);
}

namespace {

struct MethodReadOffPrototypeOfClass {
    ValueID readOfPrototype;
    ValueID readOfMethod;
    LayoutID classLayout;
    FunctionID method;
    Vector<MemberConditionRecord, 2> conditions;
};

} // anonymous namespace

static std::optional<MethodReadOffPrototypeOfClass> findMethodReadOffPrototypeOfClass(Walker& walker, const BodyValue& read, TypeID typeOfFunctionObject = { })
{
    const Program& program = walker.program();
    if (!walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::Guesses) || read.opcode() != Opcode::PropGet || read.isFreeName())
        return std::nullopt;
    const BodyIndex& index = walker.currentBodyIndex();
    PropertyKeyID prototype = program.keyOf(BuiltinNames::prototype);
    ValueID first = read.key() == prototype ? read.id() : read.objectOperand();
    if (index.isParameter(first) || index.opcodeOf(first) != Opcode::PropGet)
        return std::nullopt;
    BodyValue readOfPrototype = walker.asWalked(index.value(first));
    ValueID second = index.onlyUsingValue(first);
    if (readOfPrototype.isFreeName() || readOfPrototype.key() != prototype || !second || second.index() != first.index() + 1 || index.opcodeOf(second) != Opcode::PropGet)
        return std::nullopt;
    BodyValue readOfMethod = walker.asWalked(index.value(second));
    if (readOfMethod.opcode() != Opcode::PropGet || readOfMethod.isFreeName() || readOfMethod.objectOperand() != first)
        return std::nullopt;

    if (!typeOfFunctionObject) {
        Reference function = walker.referenceOf(readOfPrototype.objectOperand());
        if (!function || !function.isRecord())
            return std::nullopt;
        typeOfFunctionObject = walker.typeOf(function);
    }
    Type type = walker.types().at(typeOfFunctionObject);
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    if (!constructor)
        return std::nullopt;
    LayoutID classLayout = classOfClassObject(program, constructor);
    bool isConstructorFunction = !classLayout;
    if (isConstructorFunction)
        classLayout = classOfConstructorFunction(program, constructor);
    if (!classLayout || program.at(classLayout).kind != LayoutKind::Instance || declaredLayoutOf(program, classLayout) != classLayout)
        return std::nullopt;

    LayoutID classOfObjects;
    FunctionID method = findMethodDeclaredAboveObjectsOf(walker, classLayout, readOfMethod.key(), classOfObjects);
    if (!method || !closureTableHoldsMethodForTypedAccess(walker, method) || !walker.facts().linked().mayBeMentionedBy(walker.unit(), method))
        return std::nullopt;
    MethodReadOffPrototypeOfClass found { first, second, classLayout, method, { } };
    found.conditions.append(conditionOnMethodAboveObjectsOf(walker, classOfObjects, readOfMethod.key()));
    if (isConstructorFunction && !walker.facts().linked().layouts[classLayout.index()].prototypeObjectIsSettledAheadOfEveryObject)
        found.conditions.append({ MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Static, prototype });
    return found;
}

static bool guardsOfMethodReadOffPrototypeOfClassHold(const Walker& walker, const MethodReadOffPrototypeOfClass& found)
{
    const WalkState& state = walker.state();
    return std::ranges::all_of(found.conditions, [&](const MemberConditionRecord& condition) {
        if (condition.side == ClassSide::Static)
            return state.guardOfReadsHolds(condition.layout, condition.key, ClassSide::Static);
        return conditionOnMethodAboveObjectsIsKnownToHold(walker, state, condition.layout, condition.key);
    });
}

bool findsMethodReadOffPrototypeOfClass(Walker& walker, const BodyValue& readOfPrototype, TypeID typeOfFunctionObject, bool& guardsHold)
{
    std::optional<MethodReadOffPrototypeOfClass> found = findMethodReadOffPrototypeOfClass(walker, readOfPrototype, typeOfFunctionObject);
    if (!found || found->readOfPrototype != readOfPrototype.id())
        return false;
    guardsHold = guardsOfMethodReadOffPrototypeOfClassHold(walker, *found);
    return true;
}

static TypedForm buildReadsOfMethodOffPrototypeOfClassForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands)
{
    if (operands.areOnlyTypes || !walker.state().mayHoldGuards() || value.opcode() != Opcode::PropGet || walker.isUnreachable())
        return TypedForm::None;
    bool readsPrototype = value.opcode() == Opcode::PropGet && value.key() == walker.program().keyOf(BuiltinNames::prototype);
    std::optional<MethodReadOffPrototypeOfClass> found = findMethodReadOffPrototypeOfClass(walker, value, readsPrototype ? operands.typeOfObject : TypeID());
    if (!found || !guardsOfMethodReadOffPrototypeOfClassHold(walker, *found))
        return TypedForm::None;
    if (value.id() == found->readOfPrototype) {
        if (!operands.object)
            return TypedForm::None;
        Draft prototypeObject(Opcode::GetClassPrototype, walker.originOf(value));
        walker.chooseConservativeFields(prototypeObject);
        prototypeObject.setClassObjectOperand(operands.object);
        prototypeObject.setLayout(found->classLayout);
        walker.bind(value.id(), walker.build(WTF::move(prototypeObject)));
        return TypedForm::Built;
    }

    Origin origin = walker.originOf(value);
    if (walker.checksProofs()) {
        Draft test(Opcode::AreMembersUnchanged, origin);
        test.setConditions(found->conditions.span());
        Reference holds = walker.buildTestOfCheck(WTF::move(test));
        if (holds && holds.isValue() && !walker.isUnreachable())
            walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
    }
    return bindClosureThatClosureTableHoldsForTypedAccess(walker, value, found->method, origin);
}

bool callThroughSuperBehindGuards(Walker& walker, const BodyValue& superBase)
{
    if (!walker.has(Mechanism::DirectCalls) || !walker.has(Mechanism::TypedOperations) || walker.isNestedTooDeeply() || walker.isUnreachable())
        return false;
    const Program& program = walker.program();
    const BodyIndex& index = walker.currentBodyIndex();
    auto written = superBase.as<Opcode::GetSuperBase>();
    BlockID inputBlock = index.blockOf(superBase.id());
    ValueID read = index.onlyUsingValue(superBase.id());
    if (written.homeKind() != HomeObjectKind::ClassPrototype || !read || index.opcodeOf(read) != Opcode::SuperGet || index.blockOf(read) != inputBlock)
        return false;
    auto lookup = index.value(read).as<Opcode::SuperGet>();
    ValueID call = index.onlyUsingValue(read);
    if (!lookup.key() || lookup.baseOperand() != superBase.id() || !call || index.opcodeOf(call) != Opcode::Call || index.blockOf(call) != inputBlock || index.value(call).as<Opcode::Call>().calleeOperand() != read)
        return false;
    uint32_t first = index.block(inputBlock).firstValue().index();
    uint32_t begin = superBase.id().index() - first;
    uint32_t placeOfRead = read.index() - first;
    uint32_t end = call.index() - first + 1;
    Reference home = walker.referenceOf(written.homeOperand());
    if (end > walker.endOfStretchBeingWalked() || !home)
        return false;
    bool paysBytesForTime = walker.isInLoop() || walker.isCurrentFrameRecursive();
    bool armsHoldCall = paysBytesForTime || end - begin == 3;
    if (!armsHoldCall)
        end = placeOfRead + 1;

    Type type = walker.types().at(walker.typeOf(home));
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    LayoutID classLayout = constructor ? classOfClassObject(program, constructor) : LayoutID();
    if (!classLayout || program.at(classLayout).kind != LayoutKind::Instance || declaredLayoutOf(program, classLayout) != classLayout || !walker.plan().hasClassRecord(classLayout) || !walker.facts().isEvaluatedAtMostOnce(classLayout))
        return false;
    LayoutID parent;
    if (walker.positions().classAbove(classLayout) || !findParentNamedByClassRecordForTypedAccess(walker, classLayout, parent) || !parent)
        return false;
    LayoutID classOfObjects;
    FunctionID method = findMethodDeclaredAboveObjectsOf(walker, parent, lookup.key(), classOfObjects);
    if (!method || !closureTableHoldsMethodForTypedAccess(walker, method) || !walker.facts().linked().mayBeMentionedBy(walker.unit(), method))
        return false;

    const MemberConditionRecord conditions[] {
        { MemberConditionKind::PrototypeLinkUnchanged, classLayout, ClassSide::Prototype, { } },
        conditionOnMethodAboveObjectsOf(walker, classOfObjects, lookup.key()),
    };
    Origin origin = walker.originOf(superBase);
    Walker::StretchInArms stretch = walker.stretchInArms(inputBlock, begin, end);
    Walker::ArmsThatMeet built;
    BlockID meeting = walker.addBlock();
    BlockID closed = walker.addBlock(BlockFrequency::Rare);
    walker.recordSpeculation();
    walker.count(StatisticID::SuperCallsDevirtualized);
    if (!armsHoldCall)
        walker.count(StatisticID::SuperMethodLookupsGuarded);
    for (const MemberConditionRecord& condition : conditions) {
        if (condition.kind == MemberConditionKind::PrototypeLinkUnchanged ? walker.shouldSkipSuperPrototypeGuard() : walker.shouldSkipSuperMethodGuard())
            continue;
        Draft look(Opcode::AreMembersUnchanged, origin);
        look.setConditions(singleElementSpan(condition));
        Reference holds = walker.build(WTF::move(look));
        if (!holds)
            return true;
        BlockID whereItHolds = walker.addBlock();
        walker.branch(holds, whereItHolds, closed);
        walker.continueIn(whereItHolds);
        noteGuardsShownByTest(walker, singleElementSpan(condition), holds);
    }
    for (uint32_t place = begin; place < end; ++place)
        walker.bind(ValueID(first + place), Reference());
    walker.walkStretch(inputBlock, begin + 1, placeOfRead);
    if (walker.checksProofs() && !walker.hasFailed() && !walker.isUnreachable()) {
        WalkState believed = walker.state();
        Reference baseAsWritten = walker.buildTestOfCheck(walker.draftOf(superBase));
        if (baseAsWritten && !walker.isUnreachable()) {
            walker.bind(superBase.id(), baseAsWritten);
            Draft readAsWritten = walker.draftOf(index.value(read));
            walker.bind(superBase.id(), Reference());
            bindClosureThatClosureTableHoldsForTypedAccess(walker, index.value(read), method, origin);
            walker.checkThatMemoryAnswersAsBelieved(walker.referenceOf(read), WTF::move(readAsWritten));
            walker.bind(read, Reference());
        }
        walker.state() = WTF::move(believed);
    }
    if (!armsHoldCall && !walker.hasFailed() && !walker.isUnreachable())
        bindClosureThatClosureTableHoldsForTypedAccess(walker, index.value(read), method, origin);
    if (armsHoldCall && !walker.hasFailed() && !walker.isUnreachable())
        walker.walkStretch(inputBlock, placeOfRead + 1, end - 1);
    if (armsHoldCall && !walker.hasFailed() && !walker.isUnreachable()) {
        auto callAsWritten = index.value(call).as<Opcode::Call>();
        Vector<Reference, 8> arguments;
        for (ValueID argument : callAsWritten.argumentsOperands())
            arguments.append(walker.referenceOf(argument));
        bindClosureThatClosureTableHoldsForTypedAccess(walker, index.value(read), method, origin);
        if (!callFunctionDirectly(walker, index.value(call), method, walker.referenceOf(callAsWritten.thisValueOperand()), arguments.span(), paysBytesForTime ? MayCopyFunctionCalledDirectly::Yes : MayCopyFunctionCalledDirectly::No))
            walker.walkStretch(inputBlock, end - 1, end);
    }
    if (walker.hasFailed())
        return true;
    walker.listArm(stretch, built);
    walker.walkClosedArm(closed, stretch, built);
    if (!walker.hasFailed())
        walker.joinArms(stretch, built, meeting);
    return true;
}

static void lookAtGuardsInFrontOfReadsOffConstructorFunction(Walker& walker, std::span<const ValueID> reads, Reference object, NOESCAPE const ScopedLambda<void(Reference test)>& continueIfCheckPasses)
{
    const BodyIndex& index = walker.currentBodyIndex();
    Vector<MemberConditionRecord, 4> conditions;
    WordsListedOnce conditionsListed;
    bool someGuardIsNotKnownToHold = false;
    for (ValueID id : reads) {
        BodyValue read = walker.asWalked(index.value(id));
        if (std::optional<MethodReadOffPrototypeOfClass> off = findMethodReadOffPrototypeOfClass(walker, read, walker.typeOf(object)); off && off->readOfPrototype == id) {
            someGuardIsNotKnownToHold |= !guardsOfMethodReadOffPrototypeOfClassHold(walker, *off);
            for (const MemberConditionRecord& condition : off->conditions) {
                conditionsListed.appendUnlessListed(conditions, condition);
            }
            continue;
        }
        std::optional<FoundOffConstructorFunction> found = findWhatRecordsExpectOffConstructorFunction(walker, read, walker.typeOf(object));
        if (!found)
            continue;
        someGuardIsNotKnownToHold |= !walker.state().guardOfReadsHolds(found->classLayout, read.key(), ClassSide::Static);
        MemberConditionRecord condition { MemberConditionKind::MemberUnchanged, found->classLayout, ClassSide::Static, read.key() };
        conditionsListed.appendUnlessListed(conditions, condition);
    }
    if (!someGuardIsNotKnownToHold || walker.isUnreachable())
        return;

    Origin origin = walker.originOf(index.value(reads.front()));
    if (walker.program().at(conditions[0].layout).has(LayoutFlags::IsConstructorFunction) && !walker.state().guardsHaveBegun(conditions[0].layout)) {
        Draft begin(Opcode::BeginMemberGuards, origin);
        begin.setLayout(conditions[0].layout);
        walker.build(WTF::move(begin));
    }
    Draft test(Opcode::AreMembersUnchanged, origin);
    test.setConditions(conditions.span());
    Reference passed = walker.isUnreachable() ? Reference() : walker.build(WTF::move(test));
    if (!passed)
        return;
    continueIfCheckPasses(passed);
    noteGuardsShownByTest(walker, conditions.span(), passed);
}

namespace {

struct FoundOffHeirOfBuiltin {
    LayoutID layout;
    FunctionID method;
    uint32_t field { 0 };
    BuiltinID memberOfBuiltin { };

    bool standsAboveObject() const { return method || memberOfBuiltin; }
};

} // anonymous namespace

static BuiltinID findMemberOfBuiltinAboveHeirForTypedAccess(const Walker& walker, LayoutID layout, PropertyKeyID key, ASCIILiteral& whyNone)
{
    const Program& program = walker.program();
    BuiltinClassID base = program.at(layout).writtenBuiltinBase;
    if (program.at(key).kind != PropertyKeyKind::Text) {
        whyNone = "a class below a built-in, and the look says nothing of own properties under symbols"_s;
        return { };
    }
    BuiltinID member = findMemberAboveHeirsOfBuiltinClass(walker, base, key);
    if (!member)
        whyNone = "a class below a built-in, and the name is no listed field, no method of the class, and no member above it that is a constant of the build"_s;
    return member;
}

static std::optional<FoundOffHeirOfBuiltin> findWhatReadFindsOffHeirOfBuiltin(const Walker& walker, const BodyValue& read, TypeID typeOfObject, ASCIILiteral* whyNothing = nullptr)
{
    LayoutID layout = read.opcode() == Opcode::PropGet && !read.isFreeName() ? layoutKnownByStructureOfClass(walker, typeOfObject) : LayoutID();
    if (!layout)
        return std::nullopt;
    auto nothing = [&](ASCIILiteral why) -> std::optional<FoundOffHeirOfBuiltin> {
        if (whyNothing)
            *whyNothing = why;
        return std::nullopt;
    };
    const Program& program = walker.program();
    PropertyKeyID key = read.key();
    if (!findsMemberWithoutCodeForTypedAccess(program, linkTablesForTypedAccesses(walker), key))
        return nothing("a class below a built-in, and a lookup under the key may run code"_s);
    if (std::optional<FieldReference> field = findField(program, layout, key)) {
        if (field->layout != layout || fieldAt(program, *field).flags.contains(FieldFlags::IsMethod) || walker.facts().hasMemberUnder(layout, ClassSide::Prototype, key))
            return nothing("a class below a built-in, and the name is a field and a member of the class"_s);
        if (!fieldAt(program, *field).flags.contains(FieldFlags::IsRequired))
            return nothing("a class below a built-in, and the constructor may leave the field out"_s);
        return FoundOffHeirOfBuiltin { layout, { }, field->field };
    }
    if (classHolderIsMadeWithName(program, ClassSide::Prototype, key))
        return nothing("a class below a built-in, and every prototype object is made with the name"_s);
    if (walker.facts().placesOfMembersUnder(layout, ClassSide::Prototype, key).empty()) {
        ASCIILiteral whyNone;
        BuiltinID member = findMemberOfBuiltinAboveHeirForTypedAccess(walker, layout, key, whyNone);
        return member ? std::optional(FoundOffHeirOfBuiltin { layout, { }, 0, member }) : nothing(whyNone);
    }
    ReceiversOfTypedAccess receivers;
    receivers.set = walker.types().recordOf(typeOfObject).layouts;
    receivers.members.append(layout);
    FunctionID method = findMethodReadBehindLooksForTypedAccess(walker, read, receivers, key, whyNothing);
    if (!method)
        return std::nullopt;
    if (!closureTableHoldsMethodForTypedAccess(walker, method))
        return nothing("a class below a built-in, and the closure table does not hold the method"_s);
    return FoundOffHeirOfBuiltin { layout, method, 0, { } };
}

static MemberConditionRecord conditionOnWhatStandsAboveHeirOfBuiltin(const Walker& walker, const FoundOffHeirOfBuiltin& found, PropertyKeyID key)
{
    if (found.method)
        return conditionOnMethodAboveObjectsOf(walker, found.layout, key);
    return { MemberConditionKind::MemberUnchanged, found.layout, ClassSide::Prototype, key };
}

static bool conditionOnWhatStandsAboveHeirOfBuiltinIsKnownToHold(const Walker& walker, const FoundOffHeirOfBuiltin& found, PropertyKeyID key)
{
    if (found.method)
        return conditionOnMethodAboveObjectsIsKnownToHold(walker, walker.state(), found.layout, key);
    return walker.shouldAssumeGuardsAndFacts() || walker.state().guardOfReadsHolds(found.layout, key);
}

static TypedForm buildReadOffHeirOfBuiltinForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands)
{
    std::optional<FoundOffHeirOfBuiltin> found = findWhatReadFindsOffHeirOfBuiltin(walker, value, operands.typeOfObject);
    if (!found)
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return found->memberOfBuiltin ? buildReadOfMemberAboveHeirOfBuiltinClass(walker, value, operands.typeOfObject, { }, found->memberOfBuiltin) : TypedForm::BuiltBehindLookAtCell;
    Reference name = nameBehindLookAtStructureOfClass(walker, operands.object, found->layout);
    if (!name || (found->standsAboveObject() && !conditionOnWhatStandsAboveHeirOfBuiltinIsKnownToHold(walker, *found, value.key())))
        return TypedForm::None;

    Origin origin = walker.originOf(value);
    checkStructureOfClassThatIsRemembered(walker, name, found->layout, origin);
    if (found->standsAboveObject()) {
        MemberConditionRecord remembered = conditionOnWhatStandsAboveHeirOfBuiltin(walker, *found, value.key());
        checkGuardsThatAreRemembered(walker, singleElementSpan(remembered), origin);
        if (found->method)
            return bindClosureThatClosureTableHoldsForTypedAccess(walker, value, found->method, origin);
        return walker.isUnreachable() ? TypedForm::None : buildReadOfMemberAboveHeirOfBuiltinClass(walker, value, operands.typeOfObject, operands.object, found->memberOfBuiltin);
    }
    walker.bind(value.id(), buildReadOfFieldOfHeir(walker, name, found->layout, found->field, origin));
    return TypedForm::Built;
}

static std::optional<FoundOffHeirOfBuiltin> findFieldThatStoreOverwritesInHeirOfBuiltin(const Walker& walker, const BodyValue& store, TypeID typeOfObject, ASCIILiteral* whyNothing = nullptr)
{
    LayoutID layout = store.opcode() == Opcode::PropSet && !store.isFreeName() ? layoutKnownByStructureOfClass(walker, typeOfObject) : LayoutID();
    if (!layout)
        return std::nullopt;
    auto nothing = [&](ASCIILiteral why) -> std::optional<FoundOffHeirOfBuiltin> {
        if (whyNothing)
            *whyNothing = why;
        return std::nullopt;
    };
    const Program& program = walker.program();
    PropertyKeyID key = store.key();
    if (!findsMemberWithoutCodeForTypedAccess(program, linkTablesForTypedAccesses(walker), key))
        return nothing("a class below a built-in, and a lookup under the key may run code"_s);
    std::optional<FieldReference> field = findField(program, layout, key);
    if (!field)
        return nothing("a class below a built-in, and the store is under no listed field"_s);
    const Field& listed = fieldAt(program, *field);
    if (field->layout != layout || listed.flags.contains(FieldFlags::IsMethod) || walker.facts().hasMemberUnder(layout, ClassSide::Prototype, key))
        return nothing("a class below a built-in, and the name is a field and a member of the class"_s);
    if (!listed.flags.contains(FieldFlags::IsRequired))
        return nothing("a class below a built-in, and the constructor may leave the field out"_s);
    if (!listed.form.isEmpty())
        return nothing("a class below a built-in, and the field is defined with attributes of its own"_s);
    if (isBeingMadeByBodyBeingReadInTypedAccess(walker, store.objectOperand()))
        return nothing("a class below a built-in, and the store may be the one that creates the field"_s);
    return FoundOffHeirOfBuiltin { layout, { }, field->field };
}

static TypedForm buildStoreIntoHeirOfBuiltinForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands)
{
    std::optional<FoundOffHeirOfBuiltin> found = operands.typeOfStored ? findFieldThatStoreOverwritesInHeirOfBuiltin(walker, value, operands.typeOfObject) : std::nullopt;
    if (!found)
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return TypedForm::BuiltBehindLookAtCell;
    Reference name = nameBehindLookAtStructureOfClass(walker, operands.object, found->layout);
    if (!name || !operands.stored)
        return TypedForm::None;

    Origin origin = walker.originOf(value);
    checkStructureOfClassThatIsRemembered(walker, name, found->layout, origin);
    if (walker.isUnreachable())
        return TypedForm::None;
    walker.bind(value.id(), buildStoreIntoFieldOfHeir(walker, name, operands.stored, found->layout, found->field, origin));
    return TypedForm::Built;
}

static TypeID numbersAskedOfValueReadOffHeirOfBuiltin(Walker& walker, const BodyValue& read, const FoundOffHeirOfBuiltin& found)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = found.standsAboveObject() ? ValueID() : index.onlyUsingValue(read.id());
    if (!user || index.blockOf(user) != index.blockOf(read.id()) || (!allNumericOperators.contains(index.opcodeOf(user)) && !allRelations.contains(index.opcodeOf(user))))
        return { };
    return numbersExpectedOfFieldOfHeir(walker, found.layout, found.field);
}

static void lookAtStructureInFrontOfAccessesToHeirOfBuiltin(Walker& walker, std::span<const ValueID> accesses, Reference object, LayoutID layout, NOESCAPE const ScopedLambda<void(Reference test)>& continueIfCheckPasses)
{
    const BodyIndex& index = walker.currentBodyIndex();
    Origin origin = walker.originOf(index.value(accesses.front()));
    auto buildCheckPreferablyHoisted = [&](Reference test) {
        BlockID whereWalkStood = walker.currentBlock();
        continueIfCheckPasses(test);
        return !walker.isUnreachable() && walker.currentBlock() == whereWalkStood;
    };
    Reference name = nameBehindLookAtStructureOfClass(walker, object, layout);
    if (!name) {
        Reference look;
        if (!walker.isUnreachable()) {
            Walker::SpecializedPreheaderScope preheaderScope(walker, walker.isInvariantInSpecializedVersion(object) && walker.iterationsPreservePrototypeChains());
            look = buildLookAtStructureOfClass(walker, object, layout, origin);
        }
        if (!look)
            return;
        Walker::SpecializedPreheaderScope preheaderScope(walker, buildCheckPreferablyHoisted(look));
        name = buildNameBehindLookAtStructureOfClass(walker, object, look, layout, origin);
        if (!name)
            return;
    }
    Vector<PropertyKeyID, 4> keysStoredInFrontOfRead;
    for (ValueID id : accesses) {
        BodyValue read = walker.asWalked(index.value(id));
        if (read.opcode() == Opcode::PropSet) {
            keysStoredInFrontOfRead.append(read.key());
            continue;
        }
        std::optional<FoundOffHeirOfBuiltin> found = findWhatReadFindsOffHeirOfBuiltin(walker, read, walker.typeOf(object));
        TypeID asked = found && !keysStoredInFrontOfRead.contains(read.key()) ? numbersAskedOfValueReadOffHeirOfBuiltin(walker, read, *found) : TypeID();
        if (!asked)
            continue;
        Reference loaded = buildReadOfFieldOfHeir(walker, name, layout, found->field, origin);
        while (Reference checked = loaded ? walker.state().checkedNameOf(loaded) : Reference())
            loaded = checked;
        if (!loaded || walker.isUnreachable())
            return;
        if (!loaded.isValue() || walker.types().isSubset(walker.typeOf(loaded), asked))
            continue;
        Reference test;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, walker.isInvariantInSpecializedVersion(loaded));
            test = buildTestOfNumber(walker, loaded, asked, origin);
        }
        if (!test)
            return;
        Walker::SpecializedPreheaderScope preheaderScope(walker, buildCheckPreferablyHoisted(test));
        if (Reference number = buildProofOfNumber(walker, loaded, test, origin))
            walker.state().noteCheckedName(loaded, number);
    }
    Vector<MemberConditionRecord, 4> conditions;
    WordsListedOnce conditionsListed;
    for (ValueID id : accesses) {
        BodyValue read = walker.asWalked(index.value(id));
        std::optional<FoundOffHeirOfBuiltin> found = findWhatReadFindsOffHeirOfBuiltin(walker, read, walker.typeOf(object));
        if (!found || !found->standsAboveObject() || conditionOnWhatStandsAboveHeirOfBuiltinIsKnownToHold(walker, *found, read.key()))
            continue;
        MemberConditionRecord condition = conditionOnWhatStandsAboveHeirOfBuiltin(walker, *found, read.key());
        conditionsListed.appendUnlessListed(conditions, condition);
    }
    if (conditions.isEmpty() || walker.isUnreachable())
        return;
    Draft test(Opcode::AreMembersUnchanged, origin);
    test.setConditions(conditions.span());
    Reference passed = walker.build(WTF::move(test));
    if (!passed)
        return;
    continueIfCheckPasses(passed);
    noteGuardsShownByTest(walker, conditions.span(), passed);
}

TypeID typeFoundBehindLooks(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    std::optional<ReceiversOfTypedAccess> receivers = read.opcode() == Opcode::PropGet ? findReceiversOfTypedAccess(walker, typeOfObject) : std::nullopt;
    if (std::optional<FoundOffHeirOfBuiltin> found = receivers ? std::nullopt : findWhatReadFindsOffHeirOfBuiltin(walker, read, typeOfObject)) {
        if (found->method)
            return walker.types().forFunction(found->method);
        if (found->memberOfBuiltin)
            return typeFoundOnObjectAsMade(walker, read, walker.types().builtinClass(walker.program().at(found->layout).writtenBuiltinBase));
        TypeID asked = numbersAskedOfValueReadOffHeirOfBuiltin(walker, read, *found);
        return asked ? asked : TypeID(TypeTable::anything);
    }
    if (std::optional<FoundOffConstructorFunction> found = receivers ? std::nullopt : findWhatRecordsExpectOffConstructorFunction(walker, read, typeOfObject))
        return found->method ? walker.types().forFunction(found->method) : typeOfFunctionReadOffFunctionPrototype(walker, read.key());
    if (!receivers)
        return { };
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, read.key(), { }, true);
    if (field.coverage != CoverageOfTypedAccess::NoReceiver)
        return { };
    if (FunctionID method = findMethodReadBehindLooksForTypedAccess(walker, read, *receivers, read.key()))
        return walker.types().forFunction(method);
    if (BuiltinID location = findMemberOfObjectPrototypeForTypedAccess(walker, *receivers, field, read.key()))
        return typeOfBuiltinObject(walker.typing(), location);
    return !read.isFreeName() && looksAtCellShowThatReadFindsNothing(walker, *receivers, field, read.key()) ? TypeID(TypeTable::undefined) : TypeID();
}

ALWAYS_INLINE_INTO_OTHER_FILES bool readOffConstructorFunctionAwaitsGuard(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    std::optional<FoundOffConstructorFunction> found = findWhatRecordsExpectOffConstructorFunction(walker, read, typeOfObject);
    if (!found || walker.state().guardOfReadsHolds(found->classLayout, read.key(), ClassSide::Static))
        return false;
    return found->method || buildReadOffFunctionPrototype(walker, read, typeOfObject, { }) != TypedForm::None;
}

namespace {

struct TypedStoreAskedAhead {
    TypedForm form { TypedForm::None };
    OptionSet<CellFact> askedOfCell;
    bool creates { false };
};

struct FieldInKnownOrder {
    bool isListed { false };
    std::optional<uint32_t> wordWithFieldListedNext;
};

} // anonymous namespace

static TypedStoreAskedAhead askAheadAboutTypedStore(const Walker& walker, const BodyValue& value, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, bool isDefinition)
{
    std::optional<OptionSet<CellFact>> toOverwrite = smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return mayOverwriteInTypedStore(receivers, facts);
    });
    if (toOverwrite && isFilledForBodyBeingReadInTypedAccess(walker, receivers, field, value.objectOperand(), key))
        return { toOverwrite->isEmpty() ? TypedForm::Built : TypedForm::BuiltBehindLookAtCell, *toOverwrite, false };

    std::optional<OptionSet<CellFact>> toCreate = smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return mayCreateInTypedStore(walker, receivers, field, key, isDefinition, facts, nullptr);
    });
    if (toCreate && toCreate->isEmpty() && (isDefinition || !storeAsksGuardsWherePrototypeStaysForTypedAccess(walker, receivers, field, key)))
        return { TypedForm::Built, { }, true };

    bool slotIsLikelyFilled = paysToLookAtSlotForTypedAccess(walker, field, key) && !field.isAppended && field.isOfBirth && !isBeingMadeByBodyBeingReadInTypedAccess(walker, value.objectOperand());
    if (toCreate && (!toOverwrite || !slotIsLikelyFilled))
        return { TypedForm::BuiltBehindLookAtCell, *toCreate, true };
    bool paysToLookUnderSymbol = field.isRequired && !field.mayBeListedWithoutStorage && isSymbolKind(walker.program().at(key).kind);
    if (!toOverwrite || !(paysToLookAtSlotForTypedAccess(walker, field, key) || paysToLookUnderSymbol))
        return { };
    return { toOverwrite->isEmpty() ? TypedForm::BuiltBehindFilledSlot : TypedForm::BuiltBehindFilledSlotAndLookAtCell, *toOverwrite, false };
}

static bool orderWordTellsKeysOfObjectsOf(const Walker& walker, LayoutID layout)
{
    const Plan& plan = walker.plan();
    return plan.layout(layout).holdsCompiledObjects() && plan.shapes().shapesOf(layout).count == 1;
}

static std::optional<FieldInKnownOrder> findFieldInKnownOrderForTypedAccess(const Walker& walker, LayoutID layout, PropertyKeyID key, uint32_t orderWord)
{
    const Plan& plan = walker.plan();
    const KeyOrders& orders = plan.keyOrders();
    std::optional<FieldIndex> field = plan.slots().findField(walker.program(), layout, key);
    if (!field || !orderWordTellsKeysOfObjectsOf(walker, layout) || !plan.slot(layout, *field).hasStorage() || !canLeaveSlot(plan.slot(layout, *field).representation))
        return std::nullopt;
    if (orderWord & ~orderAndCountMask || isOrderStoredOutOfLine(orderWord))
        return std::nullopt;

    Vector<FieldIndex, 16> listed;
    if (uint32_t count = countOf(orderWord)) {
        if (orderOf(orderWord) >= orders.numberOfOrdersOf(layout) || count > orders.order(layout, orderOf(orderWord)).size())
            return std::nullopt;
        std::span<const FieldIndex> order = orders.order(layout, orderOf(orderWord));
        if (count < order.size() && order[count] == *field) {
            ASSERT(orders.findOrderWord(layout, order.first(count + 1)) == orderWordOf(orderOf(orderWord), count + 1));
            return FieldInKnownOrder { false, orderWordOf(orderOf(orderWord), count + 1) };
        }
        listed.append(order.first(count));
    }
    if (listed.contains(*field))
        return FieldInKnownOrder { true, std::nullopt };
    listed.append(*field);
    return FieldInKnownOrder { false, orders.findOrderWord(layout, listed.span()) };
}

std::optional<uint32_t> orderWordWithFieldListedNext(const Walker& walker, LayoutID layout, PropertyKeyID key, uint32_t orderWord)
{
    std::optional<FieldInKnownOrder> found = findFieldInKnownOrderForTypedAccess(walker, layout, key, orderWord);
    return found && !found->isListed ? found->wordWithFieldListedNext : std::nullopt;
}

static std::optional<FieldInKnownOrder> findFieldInKnownOrderOfEveryReceiver(const Walker& walker, std::span<const LayoutID> receivers, PropertyKeyID key, uint32_t orderWord)
{
    std::optional<FieldInKnownOrder> common;
    for (LayoutID receiver : receivers) {
        std::optional<FieldInKnownOrder> found = findFieldInKnownOrderForTypedAccess(walker, receiver, key, orderWord);
        if (!found || (common && (found->isListed != common->isListed || found->wordWithFieldListedNext != common->wordWithFieldListedNext)))
            return std::nullopt;
        common = found;
    }
    return common;
}

static bool orderWordShowsThatCellLacksFieldForTypedAccess(Walker& walker, Reference object, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    std::optional<uint32_t> orderWord = walker.state().orderWordOf(object);
    std::optional<FieldInKnownOrder> inKnownOrder = orderWord ? findFieldInKnownOrderOfEveryReceiver(walker, receivers.members.span(), key, *orderWord) : std::nullopt;
    return inKnownOrder && !inKnownOrder->isListed;
}

static void finishBirthBehindStoreForTypedAccess(Walker& walker, const BodyValue& store, ValueID objectAsWritten, Reference object, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& stored)
{
    if (!stored.markAnswersForSlot || walker.isUnreachable() || !isBeingMadeByBodyBeingReadInTypedAccess(walker, objectAsWritten))
        return;
    OptionSet<CellFact> known = walker.state().cellFacts(object);
    if (known.contains(CellFact::BearsNoMark) || !known.contains(CellFact::NoFieldOutsideSlot))
        return;
    const Plan& plan = walker.plan();
    auto numberOfFieldsOfBirth = [&](LayoutID layout) {
        const PlannedLayout& planned = plan.layout(layout);
        return std::ranges::count_if(planned.slots.span().first(planned.numberOfFields()), &PlacedSlot::isFilledAtBirth);
    };
    LayoutID first = receivers.members[0];
    auto isShownFilled = [&](uint32_t index) {
        PropertyKeyID key = plan.slots().keyOf(walker.program(), { first, index });
        if (!key)
            return false;
        FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, receivers, key, { }, true);
        Reference inSlot = valueInSlotForTypedAccess(walker, object, field);
        if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.markAnswersForSlot || !field.liesInOneSlot || !provesSlotFilledForTypedAccess(walker, inSlot, key))
            return false;
        return !plan.layout(first).holdsRawValueFromBirth(index) || (field.markAnswersForValue && walker.typeOf(inSlot) && !walker.types().at(walker.typeOf(inSlot)).mayBeUndefinedOrNull());
    };
    const auto& declaredOrder = walker.program().at(first).declaredOrder;
    for (size_t place = declaredOrder.size(); place--;) {
        if (!plan.slot(first, declaredOrder[place]).isFilledAtBirth)
            continue;
        if (!isShownFilled(declaredOrder[place]))
            return;
        break;
    }
    for (uint32_t index = plan.layout(first).numberOfFields(); index--;) {
        if (plan.slot(first, index).isFilledAtBirth && !isShownFilled(index))
            return;
    }
    auto numberInFirst = numberOfFieldsOfBirth(first);
    for (LayoutID member : receivers.members) {
        if (numberOfFieldsOfBirth(member) != numberInFirst)
            return;
    }
    Origin origin = walker.originOf(store);
    origin.isTailCall = false;
    Draft finish(Opcode::FinishBirth, origin);
    finish.setObjectOperand(object);
    walker.chooseConservativeFields(finish);
    walker.build(WTF::move(finish));
    if (!walker.isUnreachable())
        walker.state().recordCellFacts(object, CellFact::BearsNoMark);
}

static Reference markOfClosureThatDefinitionAloneTakesForTypedAccess(Walker& walker, const BodyValue& definition, Reference object, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, Reference stored)
{
    if (definition.opcode() != Opcode::PropDefine || receivers.members.size() != 1 || !field.liesInOneSlot || !stored || !stored.isValue() || !object || !object.isValue())
        return { };
    const PlannedLayout& planned = walker.plan().layout(field.place.layout);
    const PlacedSlot& slot = planned.slots[field.place.field];
    if (!planned.environmentOfDeferredClosures || slot.representation != SlotRepresentation::DeferredClosure)
        return { };
    const Code& code = walker.code();
    ValueID root = underlyingCellOf(code, object.id());
    if (code.isConstant(root) || !walker.state().isFresh(Reference::value(root)))
        return { };
    const Operation& cell = code.at(root);
    if (cell.opcode != Opcode::NewObject || cell.layout() != field.place.layout || cell.fields().empty() || cell.fields().back() != *planned.environmentOfDeferredClosures || cell.fields().size() != cell.valuesOperands().size())
        return { };
    const Operation& made = code.at(stored.id());
    if (made.opcode != Opcode::NewClosure || made.function() != slot.deferredFunction || made.numberOfVariadicOperands() != 1 || made.closureForm() != ClosureForm::Slim || made.storage() != AllocationStorage::Heap)
        return { };
    if (made.variadicOperands()[0] != cell.valuesOperands().back())
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = definition.as<Opcode::PropDefine>().valueOperand();
    while (written && !index.isParameter(written) && index.hasOneUse(written) && inputIdentityOpcodes.contains(index.opcodeOf(written)))
        written = index.value(written).operand(0);
    if (!written || index.isParameter(written) || !index.hasOneUse(written) || index.opcodeOf(written) != Opcode::NewClosure)
        return { };
    BlockID writtenIn = index.blockOf(written);
    BlockID definedIn = index.blockOf(definition.id());
    if (writtenIn != definedIn && (index.hasIrreducibleFlow() || index.loopOf(writtenIn) != index.loopOf(definedIn)))
        return { };
    Draft unmade(Opcode::NewClosure, made.origin());
    unmade.setFunction(slot.deferredFunction);
    unmade.setIsMadeOnDemand(true);
    Reference scope = Reference::value(made.variadicOperands()[0]);
    unmade.setVariadicOperands(singleElementSpan(scope));
    walker.chooseConservativeFields(unmade);
    return walker.build(WTF::move(unmade));
}

static TypedForm buildStoreForTypedAccess(Walker& walker, const BodyValue& value, ValueID objectAsWritten, const OperandsOfTypedAccess& operands, PropertyKeyID key, ECMAMode mode, bool isDefinition)
{
    TypeID typeOfStored = operands.typeOfStored;
    std::optional<ReceiversOfTypedAccess> receivers = typeOfStored ? findReceiversOfTypedAccess(walker, operands.typeOfObject) : std::nullopt;
    auto storeBySetter = [&](const MemberFound& member) {
        if (isDefinition || !typeOfStored || !walker.has(Mechanism::DirectCalls) || member.kind != MemberFound::Kind::Accessor || !member.setter)
            return TypedForm::None;
        if (operands.areOnlyTypes)
            return TypedForm::BuiltAsCall;
        return callAccessor(walker, value, member.setter, operands.object, operands.stored) ? TypedForm::Built : TypedForm::None;
    };
    if (!receivers) {
        TypedForm form = storeBySetter(findMemberOfNamedValueForTypedAccess(walker, operands.typeOfObject, key));
        if (form != TypedForm::None || isDefinition || (!operands.areOnlyTypes && !operands.object))
            return form;
        form = buildStoreIntoHeirOfBuiltinForTypedAccess(walker, value, operands);
        if (form != TypedForm::None || walker.isUnreachable())
            return form;
        return buildStoreThroughSetterOfBuiltin(walker, value, operands.typeOfObject, typeOfStored, operands.object);
    }
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, typeOfStored, true);
    if (field.coverage == CoverageOfTypedAccess::NoReceiver) {
        MemberFound above = findMemberAboveReceiversForTypedAccess(walker, *receivers, key);
        if (above.kind == MemberFound::Kind::Accessor && !isDefinition && !operands.areOnlyTypes)
            askAgainWhetherFieldHidesAccessorForTypedAccess(walker, operands.object, *receivers, key, walker.originOf(value));
        TypedForm form = storeBySetter(above);
        return form != TypedForm::None || isDefinition ? form : buildAccessToAccessorBehindLooksForTypedAccess(walker, value, operands, *receivers, key);
    }
    if (!field.liesInOneSlot || !field.isStoredByProgram || !field.takesStoredValue)
        return TypedForm::None;

    if (operands.areOnlyTypes)
        return askAheadAboutTypedStore(walker, value, *receivers, field, key, isDefinition).form;

    Reference object = operands.object;
    Reference stored = operands.stored;
    OptionSet<CellFact> knownOfCell = knownCellFacts(walker, object);
    std::optional<FieldInKnownOrder> inKnownOrder;
    if (std::optional<uint32_t> orderWord = walker.state().orderWordOf(object))
        inKnownOrder = findFieldInKnownOrderOfEveryReceiver(walker, receivers->members.span(), key, *orderWord);
    bool isFilled = mayOverwriteInTypedStore(*receivers, knownOfCell) && ((inKnownOrder && inKnownOrder->isListed)
        || isFilledForBodyBeingReadInTypedAccess(walker, *receivers, field, objectAsWritten, key) || provesSlotFilledForTypedAccess(walker, valueInSlotForTypedAccess(walker, object, field), key));
    if (!isFilled && !mayCreateInTypedStore(walker, *receivers, field, key, isDefinition, knownOfCell, &walker.state()))
        return TypedForm::None;
    if (!isFilled && !isDefinition)
        askAgainGuardsAboveReceiversForTypedAccess(walker, *receivers, key, true, walker.originOf(value));
    if (walker.isUnreachable())
        return TypedForm::None;
    if (isFilled && walker.has(Mechanism::Folding) && storeLaysWhatSlotOfFreshObjectHolds(walker, object, valueInSlotForTypedAccess(walker, object, field), stored))
        return TypedForm::Built;
    Opcode opcode = isFilled ? Opcode::PutSlot : Opcode::PutSlotOrAdd;
    if (opcode == Opcode::PutSlot)
        walker.noteSlotStore(object, key, valueInSlotForTypedAccess(walker, object, field), stored);

    if (isDefinition) {
        if (Reference mark = markOfClosureThatDefinitionAloneTakesForTypedAccess(walker, value, object, *receivers, field, stored))
            stored = mark;
    }
    Draft store = draftOfSlotOperationForTypedAccess(walker, value, opcode, object, *receivers, key);
    store.setValueOperand(stored);
    store.setMode(mode);
    store.setIsDefinition(isDefinition);
    std::optional<uint32_t> orderWordAfter = inKnownOrder ? inKnownOrder->wordWithFieldListedNext : std::nullopt;
    if (opcode == Opcode::PutSlotOrAdd) {
        const Function& current = walker.program().at(walker.currentFunction());
        store.setIsExpectedToCreate(walker.state().isFresh(object) || !!current.constructedClass || current.kind == FunctionKind::Initializer);
        if (orderWordAfter)
            store.setOrderWordAfter(OrderWord(*orderWordAfter));
    }
    Reference built = walker.build(WTF::move(store));
    if (storesByNameIntoSlots.contains(value.opcode()))
        walker.bind(value.id(), built);

    TypeID typeOfObject = walker.typeOf(object);
    walker.state().invalidateAliasedSlots(object, field.place, scopedLambda<bool(Reference)>([&](Reference other) {
        TypeID typeOfOther = walker.typeOf(other);
        return !typeOfOther || walker.types().mayBeSameObject(typeOfObject, typeOfOther);
    }), scopedLambda<bool(Reference, LayoutID)>([&](Reference named, LayoutID layout) {
        return isOfLayoutAlone(walker, named, layout);
    }));
    walker.state().noteSlot(object, field.place, stored);
    if (opcode == Opcode::PutSlotOrAdd)
        walker.state().noteOrderWord(object, orderWordAfter);
    finishBirthBehindStoreForTypedAccess(walker, value, objectAsWritten, object, *receivers, field);
    return TypedForm::Built;
}

PropertyKeyID keyOfAccessToSlot(const Program& program, const BodyValue& access)
{
    if (!accessesUnderPrivateNames.contains(access.opcode()))
        return access.key();
    return fieldAt(program, FieldReference { access.layout(), access.field() }).key;
}

static bool slotTellsWhoHasPrivateNameForTypedAccess(const Walker& walker, LayoutID declaring)
{
    return walker.plan().hasOneInstance() && walker.facts().isEvaluatedAtMostOnce(declaredLayoutOf(walker.program(), declaring));
}

static bool definesPrivateNameOfObjectJustAllocatedForTypedAccess(const Walker& walker, const BodyValue& definition)
{
    const Program& program = walker.program();
    const Layout& declaring = program.at(definition.layout());
    if (declaring.has(LayoutFlags::HasHeritage) || declaring.base)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID root = valueBehindOtherNamesForTypedAccess(index, definition.objectOperand());
    if (!root)
        return false;
    const Function& function = program.at(walker.currentFunction());
    if (index.isParameter(root)) {
        std::optional<unsigned> thisParameter = function.thisParameterIndex();
        return function.kind == FunctionKind::Initializer && thisParameter && index.blockOf(root) == entryBlock && root == index.block(entryBlock).parameter(*thisParameter);
    }
    return function.kind == FunctionKind::BaseConstructor && index.opcodeOf(root) == Opcode::NewObject && index.value(root).as<Opcode::NewObject>().layout() == definition.layout();
}

static bool brandLiesInOneSlotForTypedAccess(Walker& walker, const BodyValue& access, const ReceiversOfTypedAccess& receivers)
{
    const Program& program = walker.program();
    LayoutID owner = program.at(keyOfAccessToSlot(program, access)).owner;
    for (const Field& field : program.at(access.layout()).fields) {
        if (field.flags.contains(FieldFlags::IsBrand) && field.key && program.at(field.key).owner == owner)
            return describeFieldOfTypedAccess(walker, receivers, field.key, { }).liesInOneSlot;
    }
    return false;
}

static TypedForm buildAccessUnderPrivateNameForTypedAccess(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = value.opcode();
    const Field& declared = fieldAt(walker.program(), FieldReference { value.layout(), value.field() });
    PropertyKeyID key = declared.key;
    bool stores = opcode == Opcode::PrivateSet || opcode == Opcode::PrivateDefine;
    TypeID typeOfStored = stores ? inputs.typeOf(value.valueOperand()) : TypeID();
    if (!key || (stores && !typeOfStored))
        return TypedForm::None;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, inputs.typeOf(value.objectOperand()));
    if (!receivers)
        return TypedForm::None;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, typeOfStored);
    if (!field.liesInOneSlot || !field.isMovedOutByNoStore)
        return TypedForm::None;
    if (stores && !field.takesStoredValue && !(declared.flags.contains(FieldFlags::IsBrand) && opcode == Opcode::PrivateDefine))
        return TypedForm::None;

    auto buildStore = [&](Opcode store) {
        Reference object = walker.referenceOf(value.objectOperand());
        Reference stored = walker.referenceOf(value.valueOperand());
        Draft draft = draftOfSlotOperationForTypedAccess(walker, value, store, object, *receivers, key);
        draft.setValueOperand(stored);
        draft.setMode(ECMAMode::Strict);
        draft.setIsDefinition(opcode == Opcode::PrivateDefine);
        std::optional<uint32_t> orderWordAfter;
        if (store == Opcode::PutSlotOrAdd) {
            draft.setIsExpectedToCreate(true);
            std::optional<uint32_t> orderWord = receivers->members.size() == 1 ? walker.state().orderWordOf(object) : std::nullopt;
            std::optional<FieldInKnownOrder> inKnownOrder = orderWord ? findFieldInKnownOrderForTypedAccess(walker, receivers->members[0], key, *orderWord) : std::nullopt;
            if (inKnownOrder && !inKnownOrder->isListed)
                orderWordAfter = inKnownOrder->wordWithFieldListedNext;
            if (orderWordAfter)
                draft.setOrderWordAfter(OrderWord(*orderWordAfter));
        }
        walker.bind(value.id(), walker.build(WTF::move(draft)));

        TypeID typeOfObject = walker.typeOf(object);
        walker.state().invalidateAliasedSlots(object, field.place, scopedLambda<bool(Reference)>([&](Reference other) {
            TypeID typeOfOther = walker.typeOf(other);
            return !typeOfOther || walker.types().mayBeSameObject(typeOfObject, typeOfOther);
        }), scopedLambda<bool(Reference, LayoutID)>([&](Reference named, LayoutID layout) {
            return isOfLayoutAlone(walker, named, layout);
        }));
        walker.state().noteSlot(object, field.place, stored);
        if (store == Opcode::PutSlotOrAdd)
            walker.state().noteOrderWord(object, orderWordAfter);
        return TypedForm::Built;
    };

    if (opcode == Opcode::PrivateDefine && definesPrivateNameOfObjectJustAllocatedForTypedAccess(walker, value))
        return inputs.areOnlyTypes() ? TypedForm::Built : buildStore(Opcode::PutSlotOrAdd);
    if (!slotTellsWhoHasPrivateNameForTypedAccess(walker, value.layout()))
        return inputs.areOnlyTypes() && brandLiesInOneSlotForTypedAccess(walker, value, *receivers) ? TypedForm::Built : TypedForm::None;
    if (opcode == Opcode::PrivateDefine) {
        if (inputs.areOnlyTypes())
            return TypedForm::BuiltBehindEmptySlot;
        Reference defined = walker.referenceOf(value.objectOperand());
        Reference held = walker.state().slot(defined, field.place);
        bool isKnownToLackName = (held && walker.typeOf(held) == TypeTable::absent) || (walker.has(Mechanism::Folding) && orderWordShowsThatCellLacksFieldForTypedAccess(walker, defined, *receivers, key));
        return isKnownToLackName ? buildStore(Opcode::PutSlotOrAdd) : TypedForm::None;
    }

    bool isFilledByConstructions = isFilledForBodyBeingReadInTypedAccess(walker, *receivers, field, value.objectOperand(), key) || (opcode == Opcode::PrivateGet && field.isFilledAtEveryRead && field.isMovedOutByNoStore);
    if (inputs.areOnlyTypes())
        return isFilledByConstructions || opcode == Opcode::PrivateHas ? TypedForm::Built : TypedForm::BuiltBehindFilledSlot;

    Reference object = walker.referenceOf(value.objectOperand());
    Reference held = valueInSlotForTypedAccess(walker, object, field);
    bool isFilled = isFilledByConstructions || provesSlotFilledForTypedAccess(walker, held, key);
    if (opcode == Opcode::PrivateHas) {
        if (isFilled && walker.has(Mechanism::Folding))
            bindBooleanForTypedAccess(walker, value, true);
        else if (walker.has(Mechanism::Folding) && orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, key))
            bindBooleanForTypedAccess(walker, value, false);
        else
            walker.bind(value.id(), walker.build(draftOfSlotOperationForTypedAccess(walker, value, Opcode::IsSlotFilled, object, *receivers, key)));
        return TypedForm::Built;
    }
    if (!isFilled)
        return TypedForm::None;
    if (opcode == Opcode::PrivateSet)
        return buildStore(Opcode::PutSlot);
    if (held && walker.has(Mechanism::Folding)) {
        walker.bind(value.id(), held);
        return TypedForm::Built;
    }
    std::optional<Walker::ProofsHoldEverywhere> filledWhereverObjectIs;
    if (isFilledByConstructions)
        filledWhereverObjectIs.emplace(walker);
    Reference read = walker.build(draftOfSlotOperationForTypedAccess(walker, value, Opcode::GetSlot, object, *receivers, key));
    walker.bind(value.id(), read);
    if (read)
        walker.state().noteSlot(object, field.place, read);
    return TypedForm::Built;
}

static bool lookAsksForValueOfWrittenType(Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, receivers, key, { });
    return field.coverage == CoverageOfTypedAccess::EveryReceiver && field.liesInOneSlot && lookAsksForValueOfWrittenTypeInTypedAccess(field);
}

Reference buildTestOfSlot(Walker& walker, const BodyValue& access, Reference object)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return { };
    Draft test(Opcode::IsSlotFilled, walker.originOf(access));
    fillSlotOperationForTypedAccess(test, object, *receivers, keyOfAccessToSlot(walker.program(), access));
    test.setHoldsDeclaredType(access.opcode() == Opcode::PropGet && lookAsksForValueOfWrittenType(walker, *receivers, keyOfAccessToSlot(walker.program(), access)));
    return walker.build(WTF::move(test));
}

Reference readSlotFoundFilled(Walker& walker, const BodyValue& access, Reference object)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return { };
    PropertyKeyID key = keyOfAccessToSlot(walker.program(), access);
    Draft read(readOfFilledSlotForTypedAccess(walker, *receivers, key), walker.originOf(access));
    fillSlotOperationForTypedAccess(read, object, *receivers, key);
    read.setHoldsDeclaredType(access.opcode() == Opcode::PropGet && lookAsksForValueOfWrittenType(walker, *receivers, key));
    Reference held = walker.build(WTF::move(read));
    readScopeBesideSlotForTypedAccess(walker, walker.originOf(access), held);
    if (held)
        walker.state().noteSlot(object, describeFieldOfTypedAccess(walker, *receivers, key, { }).place, held);
    return held;
}

Reference valueHeldOfFilledSlot(Walker& walker, const BodyValue& access, Reference object)
{
    std::optional<ReceiversOfTypedAccess> receivers = walker.has(Mechanism::Folding) ? findReceiversOfTypedAccess(walker, walker.typeOf(object)) : std::nullopt;
    if (!receivers)
        return { };
    PropertyKeyID key = keyOfAccessToSlot(walker.program(), access);
    Reference held = valueInSlotForTypedAccess(walker, object, describeFieldOfTypedAccess(walker, *receivers, key, { }));
    return provesSlotFilledForTypedAccess(walker, held, key) ? held : Reference();
}

void noteAnswerOfReadOfSlotThatMayBeEmpty(Walker& walker, const BodyValue& access, Reference object, Reference answer)
{
    if (!object || !object.isValue() || !answer || !answer.isValue() || !walker.has(Mechanism::Folding))
        return;
    uint64_t objectAndKey = static_cast<uint64_t>(keyOfAccessToSlot(walker.program(), access).bits()) << 32 | object.bits();
    noteKnown(walker, KnownNote::AnswerOfReadOfSlotThatMayBeEmpty, answer, { }, 0, Effects::ofReadByNameThatRunsNoCode().reads, objectAndKey, true);
}

void noteSlotFilledWhereAnswerOfItsReadIsTruthy(Walker& walker, Reference answer)
{
    std::optional<uint64_t> objectAndKey = findKnown(walker, KnownNote::AnswerOfReadOfSlotThatMayBeEmpty, answer);
    if (!objectAndKey)
        return;
    Reference object = Reference::fromBits(static_cast<uint32_t>(*objectAndKey));
    PropertyKeyID key = PropertyKeyID::fromBits(static_cast<uint32_t>(*objectAndKey >> 32));
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { });
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || walker.state().slot(object, field.place))
        return;
    walker.state().noteSlot(object, field.place, answer);
    walker.count(StatisticID::SlotsProvenFilledByTruthiness);
}

TypeID typeSeenToArriveInFilledSlot(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject);
    if (!receivers)
        return { };
    Draft asBuilt(Opcode::GetSlot);
    fillSlotOperationForTypedAccess(asBuilt, Reference::value(read.objectOperand()), *receivers, keyOfAccessToSlot(walker.program(), read));
    TypingContext byWhatIsHeld { walker.program(), walker.types(), walker.heldPositions(), TypeLevel::Proven };
    return resultTypeOf(byWhatIsHeld, asBuilt, OperandTypes { singleElementSpan(typeOfObject) });
}

TypeID typeFoundInFilledSlot(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    TypeArena& types = walker.types();
    PropertyKeyID key = keyOfAccessToSlot(walker.program(), read);
    bool isRemembered = types.keeps() == ArenaKeeps::UntilReset && walker.has(Mechanism::Guesses);
    if (std::optional<uint32_t> known = isRemembered ? types.rememberedAnswer(static_cast<uint8_t>(AnswerKeptInArena::TypeInFilledSlot), typeOfObject.bits(), key.bits()) : std::nullopt)
        return TypeID::fromBits(*known);
    TypeID found;
    if (std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject)) {
        Draft asBuilt(readOfFilledSlotForTypedAccess(walker, *receivers, key));
        fillSlotOperationForTypedAccess(asBuilt, Reference::value(read.objectOperand()), *receivers, key);
        asBuilt.setHoldsDeclaredType(lookAsksForValueOfWrittenType(walker, *receivers, key));
        found = resultTypeOf(walker.typing(), asBuilt, OperandTypes { singleElementSpan(typeOfObject) });
    }
    if (isRemembered)
        types.rememberAnswer(static_cast<uint8_t>(AnswerKeptInArena::TypeInFilledSlot), typeOfObject.bits(), key.bits(), found.bits());
    return found;
}

TypeID typeTakenBySlotOfStore(Walker& walker, const BodyValue& store, TypeID typeOfObject)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject);
    if (!receivers)
        return { };
    const SlotPlan& slots = walker.plan().slots();
    TypeID taken;
    for (LayoutID member : receivers->members) {
        std::optional<FieldIndex> field = slots.findField(walker.program(), member, store.key());
        TypeID type = field ? slots.slot(member, *field).typeInLayoutRecord() : TypeID();
        if (type)
            taken = taken ? walker.types().intersectionOf(taken, type) : type;
    }
    return taken;
}

Reference buildReadThatTestsShapeAndSlot(Walker& walker, const BodyValue& read, Reference object, TypeID guessed)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, guessed);
    if (!receivers)
        return { };
    if (slotHoldsMarkForTypedAccess(walker, *receivers, read.key()))
        return { };
    Draft draft = draftOfSlotOperationForTypedAccess(walker, read, Opcode::GetSlot, object, *receivers, read.key());
    draft.setHoldsDeclaredType(lookAsksForValueOfWrittenType(walker, *receivers, read.key()));
    draft.setLevel(KnowledgeLevel::Expected);
    if (!walker.types().isSubset(walker.typeOf(object), guessed) && markOfCellAnswersForFieldInTypedAccess(walker, *receivers, describeFieldOfTypedAccess(walker, *receivers, read.key(), { }, true), read.key(), read.objectOperand()))
        draft.setIsFilledWhereShapeFits(true);
    draft.setReceiverMayBeInBirth(isBeingMadeByBodyBeingReadInTypedAccess(walker, read.objectOperand()));
    if (resultRepresentationOf(walker, draft) != Representation::JSValue)
        return { };
    walker.letLeave(object);
    return walker.build(WTF::move(draft));
}

OptionSet<CellFact> factsThatSlotOperationLeansOn(Walker& walker, const Draft& operation)
{
    PropertyKeyID key = operation.key();
    if (!walker.checksProofs() || !operation.receivers())
        return { };
    PropertyKeyKind kind = walker.program().at(key).kind;
    if (kind == PropertyKeyKind::PrivateName || kind == PropertyKeyKind::ModulePrivateName || kind == PropertyKeyKind::EnginePrivateName)
        return { };
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversAmongLayoutsForTypedAccess(walker, operation.receivers());
    if (!receivers)
        return { };
    bool stores = operation.opcode == Opcode::PutSlot || operation.opcode == Opcode::PutSlotOrAdd;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, stores ? walker.typeOf(operation.valueOperand()) : TypeID(), true);
    if (!stores && mayFindSlotEmptyInTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsSlotThatMayBeEmpty))
        return { };
    return smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        if (operation.opcode == Opcode::PutSlot)
            return mayOverwriteInTypedStore(*receivers, facts);
        if (operation.opcode == Opcode::PutSlotOrAdd)
            return mayCreateInTypedStore(walker, *receivers, field, key, operation.isDefinition(), facts, nullptr);
        return mayFindSlotEmptyBehindLooksInTypedRead(walker, *receivers, field, key, facts, nullptr);
    }).value_or(OptionSet<CellFact>());
}

void askWhatLookupLeansOn(Walker& walker, const Draft& lookup)
{
    if (!walker.checksProofs() || walker.isUnreachable() || !lookup.receivers())
        return;
    std::optional<ReceiversOfTypedAccess> found = findReceiversAmongLayoutsForTypedAccess(walker, lookup.receivers());
    if (!found) {
        walker.fail("what a lookup leans on cannot be found from its draft"_s);
        return;
    }
    const ReceiversOfTypedAccess& receivers = *found;
    PropertyKeyID key = lookup.key();
    bool findsNothing = lookup.opcode == Opcode::FindsNoMember;
    bool stores = lookup.opcode == Opcode::PutSlotOrAdd;
    bool isProvenStore = stores && lookup.level() == KnowledgeLevel::Proven;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, receivers, key, stores ? walker.typeOf(lookup.valueOperand()) : TypeID(), !findsNothing);
    std::optional<OptionSet<CellFact>> askedAhead = findsNothing ? askAheadAboutReadThatFindsNothing(walker, receivers, field, key) : isProvenStore ? OptionSet<CellFact>() : stores ? smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return mayCreateInTypedStore(walker, receivers, field, key, false, facts, nullptr);
    }) : askAheadAboutReadOfSlotThatMayBeEmpty(walker, receivers, field, key);
    if (!askedAhead) {
        walker.fail("what a lookup leans on cannot be found from its draft"_s);
        return;
    }
    OptionSet<CellFact> factsLeanedOn = *askedAhead;
    bool leansOnGuards = findsNothing ? receivers.possibleChanges.contains(LayoutChange::ChangesPrototype) : isProvenStore ? receivers.possibleChanges.contains(LayoutChange::ChangesPrototype) || !nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key) : stores ? storeAsksGuardsForTypedAccess(walker, receivers, field, key, factsLeanedOn) : readAsksGuardsForTypedAccess(receivers, factsLeanedOn);
    auto guardsAsked = lookup.conditions();
    Vector<MemberConditionRecord, 8> guardsLeftOut;
    if (leansOnGuards) {
        for (LayoutID member : receivers.members) {
            LayoutID classLayout;
            if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, stores ? LookupAbove::OfStore : LookupAbove::OfRead, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass)
                continue;
            MemberConditionRecord condition { stores ? MemberConditionKind::MemberPassedByStores : MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
            bool isAskedByLookup = std::ranges::any_of(guardsAsked, [&](auto asked) { return asked == condition; });
            bool isAskedBesideBuilder = isProvenStore && walker.state().storesPassAbove(classLayout, key);
            if (!isAskedByLookup && !isAskedBesideBuilder && !guardsLeftOut.contains(condition))
                guardsLeftOut.append(condition);
        }
    }
    if (!guardsLeftOut.isEmpty())
        walker.askGuardsAgainWhereTheyAreLeanedOn(guardsLeftOut.span(), lookup.origin());
    if (!findsNothing || walker.isUnreachable())
        return;
    bool leavesOutExtraProperty = factsLeanedOn.contains(CellFact::NoExtraProperties) && !lookup.asksForNoExtraPropertyUnderKey();
    bool leavesOutFieldOutsideSlot = factsLeanedOn.contains(CellFact::NoFieldOutsideSlot) && !lookup.asksForNoFieldOutsideSlot();
    bool leavesOutLayoutPrototype = factsLeanedOn.contains(CellFact::HasLayoutPrototype) && !lookup.asksForLayoutPrototype();
    if (!leavesOutExtraProperty && !leavesOutFieldOutsideSlot && !leavesOutLayoutPrototype)
        return;
    Draft test(Opcode::FindsNoMember, lookup.origin());
    walker.chooseConservativeFields(test);
    test.setObjectOperand(lookup.objectOperand());
    test.setKey(key);
    TypeArena& types = walker.types();
    TypeID typeOfObject = walker.typeOf(lookup.objectOperand());
    bool isAskedOfPart = typeOfObject && types.at(typeOfObject).isObject() && types.at(typeOfObject).hasOnlyLayouts() && !types.isSubset(typeOfObject, types.forLayoutSet(receivers.set));
    test.setReceivers(isAskedOfPart ? types.at(typeOfObject).layouts : receivers.set);
    test.setAsksForNoExtraPropertyUnderKey(leavesOutExtraProperty);
    test.setAsksForNoFieldOutsideSlot(leavesOutFieldOutsideSlot);
    test.setAsksForLayoutPrototype(leavesOutLayoutPrototype);
    test.setAsksRunTime(leavesOutExtraProperty);
    Reference holds = walker.buildTestOfCheck(WTF::move(test));
    if (holds && holds.isValue() && !walker.isUnreachable())
        walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, lookup.origin());
}

static std::optional<ReceiversOfTypedAccess> findReceiversThatHaveNothingUnderKey(Walker& walker, const ReceiversOfTypedAccess& all, PropertyKeyID key)
{
    Vector<LayoutID, 16> part;
    for (LayoutID member : all.members) {
        if (walker.plan().shapes().shapesOf(member).isEmpty())
            return std::nullopt;
        std::optional<ReceiversOfTypedAccess> alone = findReceiversOfTypedAccess(walker, walker.types().forLayouts(singleElementSpan(member)));
        if (alone && askAheadAboutReadThatFindsNothing(walker, *alone, describeFieldOfTypedAccess(walker, *alone, key, { }), key))
            part.append(member);
    }
    if (part.isEmpty() || part.size() == all.members.size())
        return std::nullopt;
    return findReceiversOfTypedAccess(walker, walker.types().forLayouts(part.span()));
}

bool askCellAndGuardsWhetherNoMemberIsFound(Walker& walker, const BodyValue& question)
{
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    Reference object = walker.referenceOf(question.objectOperand());
    TypeID proven = object && object.isValue() ? walker.typeOf(object) : TypeID();
    if (proven && (!walker.types().at(proven).isObject() || !walker.types().at(proven).hasOnlyLayouts()))
        proven = objectsExpectedOfOperandOfQuestion(walker, question.objectOperand());
    if (!proven)
        return false;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, proven);
    if (!receivers)
        return false;
    PropertyKeyID key = question.key();
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { });
    std::optional<OptionSet<CellFact>> asked = askAheadAboutReadThatFindsNothing(walker, *receivers, field, key);
    ReceiversOfTypedAccess everyReceiver = *receivers;
    if (!asked) {
        receivers = findReceiversThatHaveNothingUnderKey(walker, everyReceiver, key);
        if (!receivers)
            return false;
        field = describeFieldOfTypedAccess(walker, *receivers, key, { });
        asked = askAheadAboutReadThatFindsNothing(walker, *receivers, field, key);
    }
    if (!asked || !asked->containsOnly({ CellFact::NoExtraProperties, CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype }))
        return false;
    Vector<MemberConditionRecord, 8> conditions;
    WordsListedOnce conditionsListed;
    if (receivers->possibleChanges.contains(LayoutChange::ChangesPrototype)) {
        for (LayoutID member : receivers->members) {
            LayoutID classLayout;
            if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, LookupAbove::OfRead, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass || walker.state().guardOfReadsHolds(classLayout, key))
                continue;
            MemberConditionRecord condition { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
            conditionsListed.appendUnlessListed(conditions, condition);
        }
    }
    OptionSet<CellFact> left = *asked - cellFactsForKeyedLookup(walker, object, key);
    if (asked->contains(CellFact::NoExtraProperties))
        checkNoteTakenForLookupUnderKey(walker, object, everyReceiver, key, walker.originOf(question));
    Draft stamped(Opcode::FindsNoMember, walker.originOf(question));
    walker.chooseConservativeFields(stamped);
    stamped.setObjectOperand(object);
    stamped.setKey(key);
    stamped.setReceivers(receivers->set);
    stamped.setConditions(conditions.span());
    stamped.setAsksForNoExtraPropertyUnderKey(left.contains(CellFact::NoExtraProperties));
    stamped.setAsksForNoFieldOutsideSlot(left.contains(CellFact::NoFieldOutsideSlot));
    stamped.setAsksForLayoutPrototype(left.contains(CellFact::HasLayoutPrototype));
    stamped.setIsQuestionOfLibrary(walker.checksWalk());
    Reference built = walker.build(WTF::move(stamped));
    walker.bind(question.id(), built);
    if (built && built.isValue())
        noteKnown(walker, KnownNote::AnswerOfQuestionStillStands, Reference::value(walker.code().at(built.id()).objectOperand()), built, 0, LocationMasks(AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards }), 1, true);
    walker.recordSpeculation();
    return true;
}

Reference buildReadThatTestsShapeAndCell(Walker& walker, const BodyValue& read, Reference object, TypeID guessed)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, guessed);
    if (!receivers)
        return { };
    PropertyKeyID key = read.key();
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage == CoverageOfTypedAccess::NoReceiver || !field.liesInOneSlot)
        return { };
    std::optional<OptionSet<CellFact>> asked = askAheadAboutReadOfSlotThatMayBeEmpty(walker, *receivers, field, key);
    if (!asked || !asked->containsOnly({ CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype }))
        return { };
    Vector<MemberConditionRecord, 8> conditions;
    WordsListedOnce conditionsListed;
    if (readAsksGuardsForTypedAccess(*receivers, *asked)) {
        for (LayoutID member : receivers->members) {
            LayoutID classLayout;
            if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, LookupAbove::OfRead, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass || walker.state().guardOfReadsHolds(classLayout, key))
                continue;
            MemberConditionRecord condition { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
            conditionsListed.appendUnlessListed(conditions, condition);
        }
    }
    Draft draft = draftOfSlotOperationForTypedAccess(walker, read, Opcode::GetSlotOrUndefined, object, *receivers, key);
    draft.setLevel(KnowledgeLevel::Expected);
    if (resultRepresentationOf(walker, draft) != Representation::JSValue)
        return { };
    draft.setReceiverMayBeInBirth(isBeingMadeByBodyBeingReadInTypedAccess(walker, read.objectOperand()));
    draft.setConditions(conditions.span());
    walker.letLeave(object);
    return walker.build(WTF::move(draft));
}

bool buildStoreThatTestsForItself(Walker& walker, const BodyValue& store, Reference object, TypeID guessed)
{
    TypeArena& types = walker.types();
    PropertyKeyID key = store.key();
    Reference stored = walker.referenceOf(store.valueOperand());
    TypeID typeOfStored = stored ? walker.typeOf(stored) : TypeID();
    if (!typeOfStored || store.isFreeName() || walker.representationOf(stored) == Representation::Void)
        return false;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, guessed);
    if (!receivers)
        return false;
    TypeID taken = typeTakenBySlotOfStore(walker, store, guessed);
    if (walker.withoutFunctionsThatMayNotBeMentioned(taken) != taken)
        return false;
    TypeID typeBehindTest = taken ? types.intersectionOf(typeOfStored, taken) : typeOfStored;
    if (isNeverType(typeBehindTest))
        return false;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, typeBehindTest, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || !field.isStoredByProgram || !field.takesStoredValue)
        return false;

    TypedStoreAskedAhead ahead = askAheadAboutTypedStore(walker, store, *receivers, field, key, false);
    switch (ahead.form) {
    case TypedForm::None:
    case TypedForm::BuiltAsCall:
    case TypedForm::BuiltBehindEmptySlot:
    case TypedForm::BuiltOnObjectAsMade:
        return false;
    case TypedForm::Built:
    case TypedForm::BuiltBehindFilledSlot:
    case TypedForm::BuiltBehindLookAtCell:
    case TypedForm::BuiltBehindFilledSlotAndLookAtCell:
        break;
    }
    if (!ahead.askedOfCell.containsOnly({ CellFact::NotFrozen, CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype }))
        return false;
    Vector<MemberConditionRecord, 8> conditions;
    WordsListedOnce conditionsListed;
    if (ahead.creates) {
        Reference root = object.isValue() ? Reference::value(underlyingCellOf(walker.code(), object.id())) : object;
        if (isBeingMadeByBodyBeingReadInTypedAccess(walker, store.objectOperand()) || walker.state().cellFacts(root).containsAny({ CellFact::IsLikelyAsAllocated, CellFact::IsLikelyAsConstructed }))
            return false;
        ValueID written = store.id();
        if (findLikelyOrderWordInFrontOfStores(walker, written, singleElementSpan(written), object, guessed))
            return false;
        if (storeAsksGuardsForTypedAccess(walker, *receivers, field, key, ahead.askedOfCell)) {
            for (LayoutID member : receivers->members) {
                LayoutID classLayout;
                if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, LookupAbove::OfStore, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass || walker.shouldAssumeGuardsAndFacts() || walker.state().storesPassAbove(classLayout, key))
                    continue;
                MemberConditionRecord condition { MemberConditionKind::MemberPassedByStores, classLayout, ClassSide::Prototype, key };
                conditionsListed.appendUnlessListed(conditions, condition);
            }
        }
    }

    if (!conditions.isEmpty() && walker.isInLoop())
        return false;

    Draft draft = draftOfSlotOperationForTypedAccess(walker, store, ahead.creates ? Opcode::PutSlotOrAdd : Opcode::PutSlot, object, *receivers, key);
    if (walker.hasFailed())
        return false;
    draft.setValueOperand(stored);
    draft.setMode(store.mode());
    draft.setIsDefinition(false);
    if (ahead.creates)
        draft.setConditions(conditions.span());
    draft.setLevel(KnowledgeLevel::Expected);
    walker.letLeave(object);
    walker.letLeave(stored);
    walker.bind(store.id(), walker.build(WTF::move(draft)));
    return !walker.hasFailed();
}

bool emptySlotIsUndefinedBehindLooks(Walker& walker, const BodyValue& read, Reference object)
{
    if (read.opcode() != Opcode::PropGet || !walker.has(Mechanism::Folding))
        return false;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return false;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, read.key(), { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || field.isOfBirth)
        return false;
    if (lookAsksForValueOfWrittenType(walker, *receivers, read.key()))
        return false;
    return !!askAheadAboutReadOfSlotThatMayBeEmpty(walker, *receivers, field, read.key());
}

void noteSlotFoundEmpty(Walker& walker, const BodyValue& access, Reference object)
{
    if (std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object)))
        walker.state().noteSlot(object, describeFieldOfTypedAccess(walker, *receivers, keyOfAccessToSlot(walker.program(), access), { }).place, walker.constant(Draft(Opcode::Absent)));
}

bool isBeingMadeByBodyBeingRead(const Walker& walker, ValueID object)
{
    return isBeingMadeByBodyBeingReadInTypedAccess(walker, object);
}

bool looksAtSlotOfObjectBeingMade(const Walker& walker, const BodyValue& access)
{
    return isBeingMadeByBodyBeingReadInTypedAccess(walker, access.objectOperand());
}

void lookAtCellInFrontOfStores(Walker& walker, std::span<const ValueID> stores, Reference object, NOESCAPE const ScopedLambda<void(Reference test)>& continueIfCheckPasses, std::span<const ValueID> accessesThatLookAtSlots)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers) {
        if (LayoutID heir = layoutKnownByStructureOfClass(walker, walker.typeOf(object)))
            lookAtStructureInFrontOfAccessesToHeirOfBuiltin(walker, stores, object, heir, continueIfCheckPasses);
        else
            lookAtGuardsInFrontOfReadsOffConstructorFunction(walker, stores, object, continueIfCheckPasses);
        return;
    }
    bool cellMayBeUnmade = unitMayLeaveCellUnmadeForTypedAccess(walker, object);
    if (cellMayBeUnmade)
        forgetGuardsOfClassesThatMayNotHaveBegunForTypedAccess(walker, *receivers);
    const BodyIndex& index = walker.currentBodyIndex();
    Origin origin = walker.originOf(index.value(stores.front()));

    OptionSet<CellFact> asked;
    Vector<MemberConditionRecord, 8> conditions;
    WordsListedOnce conditionsListed;
    Vector<std::pair<ValueID, FunctionID>, 2> methodsReadOffHolder;
    bool someStoreCreates = false;
    PropertyKeyID firstKeyCreated;
    auto askGuardsOfClasses = [&](PropertyKeyID key, LookupAbove lookup) {
        for (LayoutID member : receivers->members) {
            LayoutID classLayout;
            if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, lookup, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass)
                continue;
            if (lookup == LookupAbove::OfRead ? walker.state().guardOfReadsHolds(classLayout, key) : walker.state().storesPassAbove(classLayout, key))
                continue;
            MemberConditionRecord condition { lookup == LookupAbove::OfRead ? MemberConditionKind::MemberUnchanged : MemberConditionKind::MemberPassedByStores, classLayout, ClassSide::Prototype, key };
            conditionsListed.appendUnlessListed(conditions, condition);
        }
    };
    auto askLooksForAccessorAbove = [&](const BodyValue& access, PropertyKeyID key) {
        if (!findHalfRunBehindLooksForTypedAccess(walker, access, *receivers, key))
            return false;
        if (std::optional<OptionSet<CellFact>> askedByAccess = smallestSufficientCellFacts([&](OptionSet<CellFact> facts) { return !possibleChangesForbid(receivers->possibleChanges, TypedAccess::FindsMethodAbove, facts, true); }))
            asked.add(*askedByAccess);
        for (LayoutID member : receivers->members) {
            LayoutID classLayout;
            FunctionID getter;
            FunctionID setter;
            if (!findAccessorDeclaredAboveObjectsOf(walker, member, key, classLayout, getter, setter) || walker.state().guardOfReadsHolds(classLayout, key))
                continue;
            MemberConditionRecord condition { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
            conditionsListed.appendUnlessListed(conditions, condition);
        }
        return true;
    };
    for (ValueID id : stores) {
        BodyValue access = walker.asWalked(index.value(id));
        PropertyKeyID key = access.key();
        FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
        if (access.opcode() == Opcode::PropSet && field.coverage == CoverageOfTypedAccess::NoReceiver) {
            askLooksForAccessorAbove(access, key);
            continue;
        }
        if (!storesByNameIntoSlots.contains(access.opcode()) && field.coverage == CoverageOfTypedAccess::NoReceiver) {
            FunctionID method = findMethodReadBehindLooksForTypedAccess(walker, access, *receivers, key);
            if (!method && askLooksForAccessorAbove(access, key))
                continue;
            if (!method) {
                if (findMemberOfObjectPrototypeForTypedAccess(walker, *receivers, field, key)) {
                    asked.add(askAheadAboutReadOffObjectPrototype(*receivers).value_or(OptionSet<CellFact>()));
                    continue;
                }
                if (std::optional<OptionSet<CellFact>> askedByRead = askAheadAboutReadThatFindsNothing(walker, *receivers, field, key)) {
                    asked.add(*askedByRead - cellFactsForKeyedLookup(walker, object, key));
                    if (readAsksGuardsForTypedAccess(*receivers, *askedByRead))
                        askGuardsOfClasses(key, LookupAbove::OfRead);
                }
                continue;
            }
            if (!closureTableHoldsMethodForTypedAccess(walker, method))
                methodsReadOffHolder.append({ id, method });
            if (std::optional<OptionSet<CellFact>> askedByRead = askAheadAboutReadOfMethodAbove(walker, *receivers, key))
                asked.add(*askedByRead);
            for (LayoutID member : receivers->members) {
                LayoutID classLayout;
                if (!findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout) || conditionOnMethodAboveObjectsIsKnownToHold(walker, walker.state(), classLayout, key))
                    continue;
                MemberConditionRecord condition = conditionOnMethodAboveObjectsOf(walker, classLayout, key);
                conditionsListed.appendUnlessListed(conditions, condition);
            }
            continue;
        }
        if (!storesByNameIntoSlots.contains(access.opcode())) {
            std::optional<OptionSet<CellFact>> askedByRead = askAheadAboutReadOfSlotThatMayBeEmpty(walker, *receivers, field, key);
            if (!askedByRead)
                continue;
            asked.add(*askedByRead);
            if (readAsksGuardsForTypedAccess(*receivers, *askedByRead))
                askGuardsOfClasses(key, LookupAbove::OfRead);
            continue;
        }
        bool isDefinition = access.opcode() == Opcode::PropDefine;
        TypedStoreAskedAhead ahead = askAheadAboutTypedStore(walker, access, *receivers, field, key, isDefinition);
        bool slotIsAskedForValueInFront = ahead.creates && lookAsksForValueOfWrittenType(walker, *receivers, key) && std::ranges::any_of(accessesThatLookAtSlots, [&](ValueID earlier) {
            BodyValue look = walker.asWalked(index.value(earlier));
            return earlier < id && look.opcode() == Opcode::PropGet && look.key() == key;
        });
        if (std::optional<OptionSet<CellFact>> toOverwrite = slotIsAskedForValueInFront ? smallestSufficientCellFacts([&](OptionSet<CellFact> facts) { return mayOverwriteInTypedStore(*receivers, facts); }) : std::nullopt)
            ahead = { TypedForm::BuiltBehindFilledSlotAndLookAtCell, *toOverwrite, false };
        asked.add(ahead.askedOfCell);
        if (ahead.creates && !someStoreCreates)
            firstKeyCreated = key;
        someStoreCreates |= ahead.creates;
        if (ahead.creates && !isDefinition && storeAsksGuardsForTypedAccess(walker, *receivers, field, key, ahead.askedOfCell))
            askGuardsOfClasses(key, LookupAbove::OfStore);
    }

    Reference root = object.isValue() ? Reference::value(underlyingCellOf(walker.code(), object.id())) : object;
    bool asksForWholeWord = someStoreCreates && walker.state().cellFacts(root).contains(CellFact::IsLikelyAsAllocated)
        && (receivers->members.size() == 1 ? orderWordTellsKeysOfObjectsOf(walker, receivers->members[0]) : !!findFieldInKnownOrderOfEveryReceiver(walker, receivers->members.span(), firstKeyCreated, unorderedOrderWord));
    asked.remove(walker.state().cellFacts(object));
    if (cellMayBeUnmade) {
        size_t numberOfConditionsOnNames = conditions.size();
        for (size_t place = 0; place < numberOfConditionsOnNames; ++place) {
            LayoutID classLayout = conditions[place].layout;
            if (walker.program().at(classLayout).has(LayoutFlags::IsConstructorFunction))
                askWholeSideWhereGuardsMayNotHaveBegun(walker, classLayout, conditions);
        }
    }

    auto pass = [&](Draft&& test) {
        Reference built = walker.isUnreachable() ? Reference() : walker.build(WTF::move(test));
        if (built)
            continueIfCheckPasses(built);
        return built;
    };
    bool asksForNoExtraProperties = asked.contains(CellFact::NoExtraProperties);
    bool asksForFieldsInSlotsBesidePrototype = false;
    if (!asksForWholeWord && !asked.contains(CellFact::NotFrozen) && asksForNoExtraProperties) {
        bool asksForFieldsInSlots = asked.contains(CellFact::NoFieldOutsideSlot);
        Draft test(Opcode::HasNoExtraProperties, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(asksForFieldsInSlots);
        if (!pass(WTF::move(test)))
            return;
        walker.state().recordCellFacts(object, asksForFieldsInSlots ? OptionSet<CellFact> { CellFact::NoExtraProperties, CellFact::NoFieldOutsideSlot } : OptionSet<CellFact> { CellFact::NoExtraProperties });
    } else if (!asksForWholeWord && !asked.contains(CellFact::NotFrozen) && asked.contains(CellFact::NoFieldOutsideSlot)) {
        asksForFieldsInSlotsBesidePrototype = asked.contains(CellFact::HasLayoutPrototype);
        if (!asksForFieldsInSlotsBesidePrototype) {
            Draft test(Opcode::HasNoFieldOutsideSlot, origin);
            test.setObjectOperand(object);
            if (!pass(WTF::move(test)))
                return;
            walker.state().recordCellFacts(object, CellFact::NoFieldOutsideSlot);
        }
    } else if (asksForWholeWord || asked.containsAny({ CellFact::NotFrozen, CellFact::NoFieldOutsideSlot })) {
        bool asksForFieldsInSlots = asksForWholeWord || asked.contains(CellFact::NoFieldOutsideSlot);
        Draft test(Opcode::IsNotFrozen, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(asksForFieldsInSlots);
        test.setHasNoExtraProperties(asksForWholeWord || asksForNoExtraProperties);
        if (!pass(WTF::move(test)))
            return;
        walker.state().recordCellFacts(object, asksForFieldsInSlots ? OptionSet<CellFact> { CellFact::NotFrozen, CellFact::NoFieldOutsideSlot } : OptionSet<CellFact> { CellFact::NotFrozen });
        if (asksForNoExtraProperties)
            walker.state().recordCellFacts(object, CellFact::NoExtraProperties);
    }
    if (asksForWholeWord) {
        Draft test(Opcode::HasKeyOrder, origin);
        test.setObjectOperand(object);
        test.setLayout(receivers->members[0]);
        test.setFields(std::span<const FieldIndex> { });
        if (!pass(WTF::move(test)))
            return;
        walker.state().noteOrderWord(object, unorderedOrderWord);
    }
    if (asked.contains(CellFact::HasLayoutPrototype)) {
        Draft test(Opcode::HasLayoutPrototype, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(asksForFieldsInSlotsBesidePrototype);
        if (!pass(WTF::move(test)))
            return;
        walker.state().recordCellFacts(object, asksForFieldsInSlotsBesidePrototype ? OptionSet<CellFact> { CellFact::HasLayoutPrototype, CellFact::NoFieldOutsideSlot } : OptionSet<CellFact> { CellFact::HasLayoutPrototype });
    }
    if (!conditions.isEmpty()) {
        Draft test(Opcode::AreMembersUnchanged, origin);
        test.setConditions(conditions.span());
        Reference passed = pass(WTF::move(test));
        if (!passed)
            return;
        noteGuardsShownByTest(walker, conditions.span(), passed, root);
    }
    for (auto [id, method] : methodsReadOffHolder) {
        Draft written = walker.draftOf(walker.asWalked(index.value(id)));
        if (walker.hasFailed() || walker.isUnreachable())
            return;
        written.setObjectOperand(object);
        walker.chooseConservativeFields(written);
        Effects effects = Effects::ofReadByNameThatRunsNoCode();
        effects.writes.remove(AbstractHeap::MemberGuards);
        walker.letLeave(object);
        Reference found = walker.build(WTF::move(written), &effects);
        if (!found || !found.isValue())
            return;
        Draft test(Opcode::IsClosureOf, origin);
        walker.chooseConservativeFields(test);
        test.setFunction(method);
        test.setValueOperand(found);
        Reference isClosureOfMethod = pass(WTF::move(test));
        if (!isClosureOfMethod || !isClosureOfMethod.isValue() || walker.isUnreachable())
            return;
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(found);
        proof.setTestOperand(isClosureOfMethod);
        proof.setOperandIndex(OperandIndex::IsClosureOf::value);
        proof.setIsOnTakenEdge(true);
        Reference closure = walker.build(WTF::move(proof));
        if (!closure)
            return;
        walker.bind(id, found);
        walker.state().noteCheckedName(found, closure);
    }
}

StoresListedAtBirth findStoresThatAllocationMayList(Walker& walker, LayoutID layout, std::span<const PropertyKeyID> keys, std::span<const TypeID> stored, std::span<const PropertyKeyID> keysStoredFurtherOn)
{
    StoresListedAtBirth listed;
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    TypeArena& types = walker.types();
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, types.forLayout(layout));
    if (!receivers || receivers->members.size() != 1 || receivers->members[0] != layout || !orderWordTellsKeysOfObjectsOf(walker, layout))
        return listed;

    constexpr OptionSet<CellFact> knownOfNewCell { CellFact::NotFrozen, CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype };
    Vector<FieldIndex, 16> fields;
    BitVector isListed;
    for (size_t index = 0; index < keys.size(); ++index) {
        PropertyKeyID key = keys[index];
        if (!isLookedUpAsNameForTypedAccess(program, key))
            break;
        FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, stored[index], true);
        if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || !field.isStoredByProgram || !field.takesStoredValue)
            break;
        if (!mayCreateInTypedStore(walker, *receivers, field, key, false, knownOfNewCell, nullptr, LookupAbove::OfStoreIntoCellMadeHere))
            break;
        std::optional<FieldIndex> place = plan.slots().findField(program, layout, key);
        if (!place || isListed.get(*place) || !canLeaveSlot(plan.slot(layout, *place).representation))
            break;
        const PlacedSlot& slot = plan.slot(layout, *place);
        bool needsNoRoom = slot.isOutOfLine() && slot.mayExistWithoutStorage && !types.at(stored[index]).isNever() && types.at(stored[index]).hasOnlyPrimitives(Primitive::Undefined);
        if (slot.storage != SlotStorage::Inline && !needsNoRoom)
            break;
        FieldFacts facts = walker.facts().field(FieldReference { layout, *place });
        TypeID takenByFacts = facts.slotType && facts.facts.contains(FieldFact::SlotHoldsUndefinedAndNull) ? types.withPrimitives(facts.slotType, Primitive::UndefinedOrNull) : facts.slotType;
        if (facts.facts.contains(FieldFact::IsAccessor) || (takenByFacts && plan.slot(layout, *place).type && !types.isSubset(stored[index], takenByFacts)))
            break;
        isListed.set(*place);
        fields.append(*place);
    }

    size_t count = plan.keyOrders().lengthOfLongestBeginningWithOrderWord(layout, fields.span());
    std::optional<PlacedAllocation> placed;
    for (; count; --count) {
        placed = plan.allocation(program, layout, fields.span().first(count));
        if (placed)
            break;
    }
    if (!count)
        return listed;
    fields.shrink(count);
    listed.fields = WTF::move(fields);
    listed.isBornUnfinished = placed->isBornUnfinished;

    bool prototypeStays = !receivers->possibleChanges.contains(LayoutChange::ChangesPrototype);
    listed.asksForLayoutPrototype = !prototypeStays;
    Vector<uint32_t, 16> namesAsked;
    for (PropertyKeyID key : keys.first(count))
        namesAsked.append(key.bits());
    for (PropertyKeyID key : keysStoredFurtherOn) {
        if (isLookedUpAsNameForTypedAccess(program, key))
            namesAsked.append(key.bits());
    }
    std::ranges::sort(namesAsked);
    namesAsked.shrink(namesAsked.size() - std::ranges::unique(namesAsked).size());
    for (uint32_t bits : namesAsked) {
        PropertyKeyID key = PropertyKeyID::fromBits(bits);
        if (prototypeStays && nothingAnswersAboveReceiversForTypedAccess(walker, *receivers, key))
            continue;
        LayoutID classLayout;
        if (whoAnswersForWhatStandsAboveObjectsOf(walker, layout, key, LookupAbove::OfStoreIntoCellMadeHere, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass)
            continue;
        MemberConditionRecord condition { MemberConditionKind::MemberPassedByStores, classLayout, ClassSide::Prototype, key };
        if (!walker.state().storesPassAbove(classLayout, key) || !walker.state().guardsHaveBegun(classLayout))
            listed.conditions.append(condition);
        else if (walker.checksProofs())
            listed.conditionsRemembered.append(condition);
    }
    return listed;
}

bool looksAtCellsAskForLayoutPrototype(Walker& walker, LayoutID layout)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.types().forLayout(layout));
    return receivers && receivers->possibleChanges.contains(LayoutChange::ChangesPrototype);
}

static ValueID valueBehindOtherNamesAndJoinsForOrderWord(Walker& walker, const BodyIndex& index, ValueID value)
{
    value = valueBehindOtherNamesForTypedAccess(index, value);
    if (!value || !index.isParameter(value))
        return value;
    Walker::ValuesThatEveryWayHandsToParameters& known = walker.valuesThatEveryWayHandsToParameters();
    auto wordOf = [&](ValueID parameter) {
        return static_cast<uint64_t>(walker.currentFunction().index()) << 32 | parameter.index();
    };
    Vector<ValueID, 16> asked { value };
    while (!asked.isEmpty()) {
        size_t place = asked.size() - 1;
        ValueID parameter = asked[place];
        if (known.contains(wordOf(parameter))) {
            asked.removeLast();
            continue;
        }
        BlockID block = index.blockOf(parameter);
        uint32_t position = parameter.index() - index.block(block).parameter(0).index();
        bool everyWayHandsOneValue = block != entryBlock && !index.isLoopHead(block) && !index.isEnteredByHandlerEdge(block);
        bool waitsForEarlierParameter = false;
        ValueID handed;
        for (const BodyIncomingEdge& edge : everyWayHandsOneValue ? walker.incomingEdgesOf(index, block) : std::span<const BodyIncomingEdge>()) {
            std::span<const ValueID> arguments = edge.target.arguments();
            ValueID from = position < arguments.size() ? valueBehindOtherNamesForTypedAccess(index, arguments[position]) : ValueID();
            if (from && index.isParameter(from) && index.blockOf(from) != entryBlock) {
                auto found = known.find(wordOf(from));
                if (found == known.end() && from.index() < parameter.index()) {
                    asked.append(from);
                    waitsForEarlierParameter = true;
                    continue;
                }
                from = found == known.end() ? ValueID() : found->value;
            }
            if (!from || (handed && from != handed)) {
                everyWayHandsOneValue = false;
                break;
            }
            handed = from;
        }
        if (everyWayHandsOneValue && waitsForEarlierParameter)
            continue;
        asked.shrink(place);
        known.add(wordOf(parameter), everyWayHandsOneValue && handed ? handed : parameter);
    }
    return known.get(wordOf(value));
}

static bool namesSamePlaceForOrderWord(const BodyIndex& index, ValueID one, ValueID other)
{
    if (one == other)
        return true;
    if (!one || !other || index.isParameter(one) || index.isParameter(other) || index.opcodeOf(one) != Opcode::PropGet || index.opcodeOf(other) != Opcode::PropGet)
        return false;
    BodyValue first = index.value(one);
    BodyValue second = index.value(other);
    return first.key() == second.key() && valueBehindOtherNamesForTypedAccess(index, first.objectOperand()) == valueBehindOtherNamesForTypedAccess(index, second.objectOperand());
}

static bool isCreatedAfterConstruction(const KeyOrders& orders, LayoutID layout, FieldIndex field)
{
    for (uint32_t number = 0; number < orders.numberOfOrdersOf(layout); ++number) {
        std::span<const FieldIndex> order = orders.order(layout, number);
        std::span<const KeyOrderSource> sources = orders.sourcesOfFields(layout, number);
        for (size_t index = 0; index < order.size(); ++index) {
            if (order[index] == field && sources[index] == KeyOrderSource::StoresInSequence)
                return true;
        }
    }
    return false;
}

static void appendKeysStoredOnResult(const Walker& walker, FunctionID function, Vector<PropertyKeyID, 32>& keys, unsigned remainingDepth)
{
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    const ThinSummary& summary = walker.facts().summaries().at(function);
    if (!summary.callThatGivesResult.isEmpty() && remainingDepth) {
        if (FunctionID callee = link.calleeAt(function, summary.callThatGivesResult[0]))
            appendKeysStoredOnResult(walker, callee, keys, remainingDepth - 1);
    }
    for (const SummaryKeyAtSite& store : summary.storesOnResult) {
        if (PropertyKeyID key = store.key ? store.key : link.keyAt(function, store.site))
            keys.appendIfNotContains(key);
    }
}

std::optional<uint32_t> findLikelyOrderWordInFrontOfStores(Walker& walker, ValueID firstOfStretch, std::span<const ValueID> stores, Reference object, TypeID typeOfObject)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject ? typeOfObject : walker.typeOf(object));
    if (!receivers || receivers->members.size() != 1 || walker.state().orderWordOf(object))
        return std::nullopt;
    LayoutID layout = receivers->members[0];
    Reference root = object.isValue() ? Reference::value(underlyingCellOf(walker.code(), object.id())) : object;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = stores.empty() ? ValueID() : valueBehindOtherNamesAndJoinsForOrderWord(walker, index, index.value(stores[0]).objectOperand());
    auto isLiteral = [&] {
        if (index.isParameter(written) || index.opcodeOf(written) != Opcode::NewObject)
            return false;
        LayoutID allocated = index.value(written).as<Opcode::NewObject>().layout();
        return !allocated || walker.program().at(allocated).kind != LayoutKind::Instance;
    };
    bool isThisBeingMade = written && !isLiteral() && isBeingMadeByBodyBeingReadInTypedAccess(walker, written);
    bool isAnswerOfCallOfBody = written && !index.isParameter(written) && (index.opcodeOf(written) == Opcode::Call || index.opcodeOf(written) == Opcode::CallDirect || index.opcodeOf(written) == Opcode::Construct);
    auto isAnswerOfCall = [&](ValueID value) {
        return value && !index.isParameter(value) && (index.opcodeOf(value) == Opcode::Call || index.opcodeOf(value) == Opcode::CallDirect || index.opcodeOf(value) == Opcode::Construct);
    };
    bool liesInFieldOfAnswerOfCallOfBody = written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::PropGet && isAnswerOfCall(valueBehindOtherNamesForTypedAccess(index, index.value(written).objectOperand()));
    bool isLikelyInOrder = isThisBeingMade || isAnswerOfCallOfBody || liesInFieldOfAnswerOfCallOfBody || walker.state().cellFacts(root).contains(CellFact::IsLikelyAsConstructed);
    if (!orderWordTellsKeysOfObjectsOf(walker, layout) || !isLikelyInOrder)
        return std::nullopt;

    std::optional<FieldIndex> firstField = stores.empty() ? std::nullopt : walker.plan().slots().findField(walker.program(), layout, walker.asWalked(index.value(stores[0])).key());
    if (!firstField || !walker.plan().keyOrders().firstContinuationCreating(layout, singleElementSpan(*firstField)))
        return std::nullopt;
    Vector<FieldIndex, 16> created;
    BlockID blockOfRow = index.blockOf(stores[0]);
    Walker::KeysStoredInFrontOfRow& kept = walker.keysStoredInFrontOfLastRow();
    Vector<PropertyKeyID, 32> keysFoundForThisRow;
    if (kept.body != &index || kept.written != written || kept.layout != layout || kept.numberOfValuesGoneThrough > stores[0].index() || !index.dominates(kept.blockOfRow, blockOfRow)) {
        ValueID oldestThatNamesIt = written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::PropGet ? valueBehindOtherNamesForTypedAccess(index, index.value(written).objectOperand()) : written;
        kept = { &index, written, layout, blockOfRow, oldestThatNamesIt && !index.isParameter(oldestThatNamesIt) ? oldestThatNamesIt.index() + 1 : 0, { }, { } };
        if (isAnswerOfCallOfBody && index.opcodeOf(written) != Opcode::Construct) {
            FunctionID callee = index.opcodeOf(written) == Opcode::CallDirect ? index.value(written).as<Opcode::CallDirect>().function() : linkTablesForTypedAccesses(walker).calleeAt(walker.currentFunction(), written);
            if (callee)
                appendKeysStoredOnResult(walker, callee, keysFoundForThisRow, 8);
        }
    }
    kept.blockOfRow = blockOfRow;
    for (uint32_t earlier = kept.numberOfValuesGoneThrough; written && earlier < stores[0].index(); ++earlier) {
        ValueID id(earlier);
        if (!index.isParameter(id) && index.opcodeOf(id) == Opcode::CallDirect && index.dominates(index.blockOf(id), blockOfRow)) {
            BodyValue call = index.value(id);
            FunctionID callee = call.as<Opcode::CallDirect>().function();
            std::span<const ValueID> arguments = call.variadicOperands();
            if (walker.program().at(callee).kind != FunctionKind::Initializer || arguments.empty() || valueBehindOtherNamesAndJoinsForOrderWord(walker, index, arguments[0]) != written)
                continue;
            const ThinSummary& summary = walker.facts().summaries().at(callee);
            for (const SummaryConstruction& construction : summary.constructions) {
                if (construction.object != SummaryConstruction::Object::This)
                    continue;
                for (PropertyKeyID key : summary.keysFilledAtEnds.span().subspan(construction.firstKeyFilledAtEnd, construction.numberOfKeysFilledAtEnd))
                    keysFoundForThisRow.append(key);
                for (const SummaryConstruction::Key& defined : summary.keysOfConstructions.span().subspan(construction.firstKey, construction.numberOfKeys)) {
                    if (defined.isFilled)
                        keysFoundForThisRow.append(defined.key);
                }
            }
            continue;
        }
        if (index.isParameter(id) || !accessesThatMayStoreUnderNameForOrderWord.contains(index.opcodeOf(id)) || !index.dominates(index.blockOf(id), blockOfRow))
            continue;
        BodyValue store = walker.asWalked(index.value(id));
        if (storesByNameIntoSlots.contains(store.opcode()) && store.key() && namesSamePlaceForOrderWord(index, valueBehindOtherNamesAndJoinsForOrderWord(walker, index, store.objectOperand()), written))
            keysFoundForThisRow.append(store.key());
    }
    kept.numberOfValuesGoneThrough = stores[0].index();
    std::ranges::sort(keysFoundForThisRow);
    keysFoundForThisRow.shrink(std::ranges::unique(keysFoundForThisRow).begin() - keysFoundForThisRow.begin());
    keysFoundForThisRow.removeAllMatching([&](PropertyKeyID key) {
        return std::ranges::binary_search(kept.keys, key);
    });
    auto appendFieldThatSomeOrderCreates = [&](PropertyKeyID key, Vector<FieldIndex, 32>& fields) {
        if (std::optional<FieldIndex> field = walker.plan().slots().findField(walker.program(), layout, key); field && walker.plan().keyOrders().firstContinuationCreating(layout, singleElementSpan(*field)))
            fields.append(*field);
    };
    if (!keysFoundForThisRow.isEmpty()) {
        for (PropertyKeyID key : keysFoundForThisRow)
            appendFieldThatSomeOrderCreates(key, kept.fields);
        size_t numberOfKeysKept = kept.keys.size();
        kept.keys.appendVector(keysFoundForThisRow);
        std::inplace_merge(kept.keys.begin(), kept.keys.begin() + numberOfKeysKept, kept.keys.end());
    }
    Vector<PropertyKeyID, 4> keysTakenForFilledByThisRow;
    for (uint32_t earlier = firstOfStretch.index(); written && earlier < stores[0].index(); ++earlier) {
        ValueID id(earlier);
        if (index.isParameter(id) || index.opcodeOf(id) != Opcode::PrivateDefine)
            continue;
        BodyValue definition = index.value(id);
        if (valueBehindOtherNamesAndJoinsForOrderWord(walker, index, definition.objectOperand()) != written)
            continue;
        if (std::optional<FieldIndex> field = walker.plan().slots().findField(walker.program(), layout, keyOfAccessToSlot(walker.program(), definition)); field && !created.contains(*field))
            created.append(*field);
    }
    bool overwritesUnprovenSlot = false;
    for (ValueID id : stores) {
        BodyValue store = walker.asWalked(index.value(id));
        PropertyKeyID key = store.key();
        std::optional<FieldIndex> field = walker.plan().slots().findField(walker.program(), layout, key);
        if (!field || created.contains(*field) || !storesByNameIntoSlots.contains(store.opcode()))
            continue;
        bool isOutOfLine = walker.plan().slot(layout, *field).isOutOfLine();
        bool isLikelyFilled = std::ranges::binary_search(kept.keys, key) || keysTakenForFilledByThisRow.contains(key);
        if (isLikelyFilled && (overwritesUnprovenSlot || (!isOutOfLine && !isThisBeingMade)))
            continue;
        FieldOfTypedAccess described = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
        if (!isLikelyFilled && !isThisBeingMade && !described.isAppended && !isCreatedAfterConstruction(walker.plan().keyOrders(), layout, *field)) {
            isLikelyFilled = true;
            keysTakenForFilledByThisRow.append(key);
            if (!isOutOfLine)
                continue;
        }
        if (!askAheadAboutTypedStore(walker, store, *receivers, described, key, store.opcode() == Opcode::PropDefine).creates)
            continue;
        if (isLikelyFilled)
            overwritesUnprovenSlot = true;
        else
            created.append(*field);
    }
    Vector<FieldIndex, 32> fieldsStoredOnEveryWayToRow(kept.fields);
    for (PropertyKeyID key : keysTakenForFilledByThisRow) {
        appendFieldThatSomeOrderCreates(key, fieldsStoredOnEveryWayToRow);
    }
    if (created.isEmpty())
        return overwritesUnprovenSlot ? walker.plan().keyOrders().findOrderWordOfShortestBeginningThatLists(layout, fieldsStoredOnEveryWayToRow.span()) : std::nullopt;
    for (size_t count = fieldsStoredOnEveryWayToRow.isEmpty() ? 0 : created.size(); count; --count) {
        if (std::optional<KeyOrderContinuation> continuation = walker.plan().keyOrders().continuationCreatingBehindFewestKeys(layout, created.span().first(count), fieldsStoredOnEveryWayToRow.span()))
            return continuation->orderWordBefore;
    }
    if (!fieldsStoredOnEveryWayToRow.isEmpty())
        return std::nullopt;
    for (size_t count = created.size(); count; --count) {
        if (std::optional<KeyOrderContinuation> continuation = walker.plan().keyOrders().continuationCreatingBehindFewestKeys(layout, created.span().first(count)))
            return continuation->orderWordBefore;
    }
    return std::nullopt;
}

void testOrderWordInFrontOfStores(Walker& walker, ValueID firstStore, Reference object, uint32_t orderWord, NOESCAPE const ScopedLambda<void(Reference test)>& continueIfCheckPasses)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers || receivers->members.size() != 1)
        return;
    LayoutID layout = receivers->members[0];
    Origin origin = walker.originOf(walker.currentBodyIndex().value(firstStore));
    auto pass = [&](Draft&& test) {
        Reference built = walker.isUnreachable() ? Reference() : walker.build(WTF::move(test));
        if (built)
            continueIfCheckPasses(built);
        return built;
    };

    std::span<const FieldIndex> listed;
    if (uint32_t count = countOf(orderWord))
        listed = walker.plan().keyOrders().order(layout, orderOf(orderWord)).first(count);
    Draft keys(Opcode::HasKeyOrder, origin);
    keys.setObjectOperand(object);
    keys.setLayout(layout);
    keys.setFields(listed);
    keys.setHasNoFlags(true);
    if (!pass(WTF::move(keys)))
        return;
    walker.count(StatisticID::KeyOrderChecksBeforeStores);
    walker.state().recordCellFacts(object, { CellFact::NotFrozen, CellFact::NoFieldOutsideSlot });
    walker.state().noteOrderWord(object, orderWord);
}

void Walker::finishBirthWhereLiteralEnds(const BodyValue& end)
{
    Walker& walker = *this;
    Reference object = walker.referenceOf(end.operand(0));
    if (!walker.has(Mechanism::TypedOperations) || !object || !object.isValue() || !isBeingMadeByBodyBeingReadInTypedAccess(walker, end.operand(0)))
        return;
    if (walker.state().cellFacts(object).contains(CellFact::BearsNoMark))
        return;
    const Code& code = walker.code();
    bool isDefinedByLongWaySomewhere = false;
    for (uint32_t built = object.id().index() + 1; built < code.numberOfValues() && !isDefinedByLongWaySomewhere; ++built)
        isDefinedByLongWaySomewhere = code.at(ValueID(built)).opcode == Opcode::PropDefine && code.at(ValueID(built)).objectOperand() == object.id();
    if (!isDefinedByLongWaySomewhere)
        return;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers || receivers->members.size() != 1)
        return;
    LayoutID layout = receivers->members[0];
    const Plan& plan = walker.plan();
    uint32_t numberOfFields = plan.layout(layout).numberOfFields();
    BitVector isOfBirth(numberOfFields);
    size_t numberOfBirth = 0;
    for (uint32_t index = 0; index < numberOfFields; ++index) {
        if (!plan.slot(layout, index).isFilledAtBirth)
            continue;
        PropertyKeyID key = plan.slots().keyOf(walker.program(), { layout, index });
        if (!key || plan.layout(layout).holdsRawValueFromBirth(index))
            return;
        FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
        if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.markAnswersForSlot || !field.liesInOneSlot)
            return;
        isOfBirth.set(index);
        ++numberOfBirth;
    }
    if (!numberOfBirth)
        return;
    const KeyOrders& orders = plan.keyOrders();
    std::optional<uint32_t> wordOfWholeBirth;
    for (uint32_t order = 0; order < orders.numberOfOrdersOf(layout) && !wordOfWholeBirth; ++order) {
        std::span<const FieldIndex> listed = orders.order(layout, order);
        if (listed.size() < numberOfBirth)
            continue;
        listed = listed.first(numberOfBirth);
        if (std::ranges::all_of(listed, [&](FieldIndex field) { return isOfBirth.get(field); }))
            wordOfWholeBirth = orders.findOrderWord(layout, listed);
    }
    if (!wordOfWholeBirth)
        return;
    BlockID behind;
    testOrderWordInFrontOfStores(walker, end.id(), object, *wordOfWholeBirth, scopedLambda<void(Reference)>([&](Reference test) {
        behind = walker.addBlock();
        BlockID whole = walker.addBlock();
        walker.branch(test, whole, behind);
        walker.continueIn(whole);
    }));
    if (!behind)
        return;
    if (!walker.isUnreachable()) {
        Origin origin = walker.originOf(end);
        origin.isTailCall = false;
        Draft finish(Opcode::FinishBirth, origin);
        finish.setObjectOperand(object);
        walker.chooseConservativeFields(finish);
        walker.build(WTF::move(finish));
        walker.count(StatisticID::AllocationsFinishedAtLiteralEnds);
    }
    if (!walker.isUnreachable())
        walker.jumpTo(behind);
    walker.continueIn(behind);
}

void noteThisAtEntry(Walker& walker, Reference thisValue)
{
    const Function& function = walker.program().at(walker.unit().function);
    if (!walker.has(Mechanism::TypedOperations) || !thisValue || !thisValue.isValue() || !function.takesThisFromConstruction() || !function.constructedClass || function.isCoroutine())
        return;
    walker.state().recordCellFacts(thisValue, CellFact::IsLikelyAsAllocated);
}

static Reference buildAsWrittenForTypedAccess(Walker& walker, const BodyValue& value)
{
    Draft draft = walker.draftOf(value);
    walker.chooseConservativeFields(draft);
    Reference built = walker.build(WTF::move(draft));
    walker.bind(value.id(), built);
    return built;
}

static bool startsInDeadZoneForTypedAccess(const Program& program, FieldReference field)
{
    const Vector<Field>& fields = program.at(field.layout).fields;
    TypeID written = field.field < fields.size() ? fields[field.field].declared.written() : TypeID();
    return written && (program.at(written).primitives & Primitive::Absent);
}

void Walker::noteEnvironmentField(Reference environment, FieldReference field, Reference value)
{
    m_state.noteEnvironmentField(environment, field, value);
    if (!has(Mechanism::Folding) || !value || !startsInDeadZoneForTypedAccess(program(), field))
        return;
    TypeID type = typeOf(value);
    if (type && !(m_types.at(type).primitives & Primitive::Absent))
        m_state.noteEndOfDeadZone(environment, field);
}

static bool buildVariableReadForTypedAccess(Walker& walker, const BodyValue& value)
{
    VariableID variable = value.variable();
    if (walker.has(Mechanism::Folding)) {
        if (Reference held = walker.state().variable(variable)) {
            walker.bind(value.id(), held);
            return true;
        }
    }
    Draft draft = walker.draftOf(value);
    walker.chooseConservativeFields(draft);
    if (walker.has(Mechanism::Folding) && walker.state().isPastDeadZone(variable))
        draft.setIsKnownInitialized(true);
    Reference read = walker.build(WTF::move(draft));
    walker.bind(value.id(), read);
    if (read)
        walker.state().noteVariable(variable, read);
    noteValueOfStableBinding(walker, variable, read);
    return true;
}

static bool buildVariableStoreForTypedAccess(Walker& walker, const BodyValue& value)
{
    Reference stored = walker.referenceOf(value.valueOperand());
    buildAsWrittenForTypedAccess(walker, value);
    walker.state().noteVariable(value.variable(), stored);
    noteValueOfStableBinding(walker, value.variable(), stored);
    TypeID typeOfStored = stored ? walker.typeOf(stored) : TypeID();
    bool storesValue = typeOfStored && !(walker.types().at(typeOfStored).primitives & Primitive::Absent);
    bool isClaimedStoredFirst = value.variable().index() < walker.facts().linked().variables.size() && walker.facts().linked().variable(value.variable()).isFilled;
    if (storesValue && (isClaimedStoredFirst || (walker.has(Mechanism::Folding) && leavesDeadZoneForGood(walker.program(), value.variable()))))
        walker.state().noteEndOfDeadZone(value.variable());
    return true;
}

static bool buildEnvironmentReadForTypedAccess(Walker& walker, const BodyValue& value)
{
    Reference environment = walker.referenceOf(value.environmentOperand());
    if (!environment)
        return false;
    FieldReference field { value.layout(), value.field() };
    if (walker.has(Mechanism::Folding)) {
        if (Reference held = walker.state().environmentField(environment, field)) {
            walker.bind(value.id(), held);
            return true;
        }
    }
    Draft draft = walker.draftOf(value);
    walker.chooseConservativeFields(draft);
    if (walker.has(Mechanism::Folding) && walker.state().isPastDeadZone(environment, field))
        draft.setIsKnownInitialized(true);
    Reference read = walker.build(WTF::move(draft));
    walker.bind(value.id(), read);
    if (read)
        walker.holdWhatBodyReadsOfItsOwnScope(value, environment, read);
    if (read)
        walker.noteEnvironmentField(environment, field, read);
    noteValueOfStableBinding(walker, environment, field, read);
    return true;
}

static bool buildEnvironmentStoreForTypedAccess(Walker& walker, const BodyValue& value)
{
    Reference environment = walker.referenceOf(value.environmentOperand());
    if (!environment)
        return false;
    Reference stored = walker.referenceOf(value.valueOperand());
    buildAsWrittenForTypedAccess(walker, value);
    walker.noteEnvironmentField(environment, FieldReference { value.layout(), value.field() }, stored);
    walker.noteStoreIntoSlotStoredFirst(value, environment);
    noteValueOfStableBinding(walker, environment, FieldReference { value.layout(), value.field() }, stored);
    return true;
}

static bool buildSuperReadForTypedAccess(Walker& walker, const BodyValue& value)
{
    auto read = value.as<Opcode::SuperGet>();
    Reference base = walker.referenceOf(read.baseOperand());
    OperandsOfTypedAccess operands;
    operands.object = walker.referenceOf(read.receiverOperand());
    if (!read.key() || !base || !operands.object)
        return false;
    return buildReadOfMemberForTypedAccess(walker, value, operands, findMemberOfNamedValueForTypedAccess(walker, walker.typeOf(base), read.key())) == TypedForm::Built;
}

static bool buildSuperStoreForTypedAccess(Walker& walker, const BodyValue& value)
{
    auto store = value.as<Opcode::SuperSet>();
    Reference base = walker.referenceOf(store.baseOperand());
    Reference receiver = walker.referenceOf(store.receiverOperand());
    Reference stored = walker.referenceOf(store.valueOperand());
    if (!store.key() || !base || !receiver || !stored || !walker.has(Mechanism::DirectCalls))
        return false;
    MemberFound member = findMemberOfNamedValueForTypedAccess(walker, walker.typeOf(base), store.key());
    return member.kind == MemberFound::Kind::Accessor && member.setter && callAccessor(walker, value, member.setter, receiver, stored);
}

static TypedForm buildAccessByNameForTypedAccess(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = value.opcode();
    PropertyKeyID key = value.key();
    if (opcode == Opcode::PropGet && !value.isFreeName() && isPrivateAliasOfBuiltinMember(walker.program(), key)) {
        Reference object = inputs.areOnlyTypes() ? Reference() : walker.referenceOf(value.objectOperand());
        return inputs.areOnlyTypes() || object ? buildReadOfBuiltinMember(walker, value, inputs.typeOf(value.objectOperand()), object) : TypedForm::None;
    }
    if (!isLookedUpAsNameForTypedAccess(walker.program(), key))
        return TypedForm::None;

    OperandsOfTypedAccess operands;
    operands.areOnlyTypes = inputs.areOnlyTypes();
    operands.typeOfObject = inputs.typeOf(value.objectOperand());
    if (!operands.areOnlyTypes)
        operands.object = walker.referenceOf(value.objectOperand());
    if (opcode == Opcode::PropGet && value.isFreeName())
        return operands.areOnlyTypes || operands.object ? buildReadOfBuiltinMember(walker, value, operands.typeOfObject, operands.object) : TypedForm::None;
    if (TypedForm form = buildReadsOfMethodOffPrototypeOfClassForTypedAccess(walker, value, operands); form != TypedForm::None)
        return form;
    if (walker.types().isUninformative(operands.typeOfObject))
        return TypedForm::None;
    if (walker.state().mayHoldGuards() && unitMayLeaveCellUnmadeForTypedAccess(walker, operands.object)) {
        if (std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, operands.typeOfObject))
            forgetGuardsOfClassesThatMayNotHaveBegunForTypedAccess(walker, *receivers);
    }
    if (opcode == Opcode::PropGet)
        return buildReadForTypedAccess(walker, value, operands, key);
    if (opcode == Opcode::PropHas)
        return buildTestForTypedAccess(walker, value, operands, key);

    operands.typeOfStored = inputs.typeOf(value.valueOperand());
    if (!operands.areOnlyTypes)
        operands.stored = walker.referenceOf(value.valueOperand());
    if (opcode == Opcode::PropSet)
        return value.isFreeName() ? TypedForm::None : buildStoreForTypedAccess(walker, value, value.objectOperand(), operands, key, value.mode(), false);
    return buildStoreForTypedAccess(walker, value, value.objectOperand(), operands, key, ECMAMode::Strict, true);
}

bool definePlainFieldInSlot(Walker& walker, const BodyValue& call, ValueID objectAsWritten, Reference object, PropertyKeyID key, Reference stored)
{
    OperandsOfTypedAccess operands;
    operands.object = object;
    operands.stored = stored;
    operands.typeOfObject = walker.typeOf(object);
    operands.typeOfStored = walker.typeOf(stored);
    if (!isLookedUpAsNameForTypedAccess(walker.program(), key) || walker.types().isUninformative(operands.typeOfObject))
        return false;
    if (walker.state().mayHoldGuards() && unitMayLeaveCellUnmadeForTypedAccess(walker, object)) {
        if (std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, operands.typeOfObject))
            forgetGuardsOfClassesThatMayNotHaveBegunForTypedAccess(walker, *receivers);
    }
    return buildStoreForTypedAccess(walker, call, objectAsWritten, operands, key, ECMAMode::Strict, true) == TypedForm::Built;
}

static constexpr uint32_t closedOperationsOfCopyOfProperties = 3;

static LayoutID layoutOfEmptyTargetOfCopyForTypedAccess(Walker& walker, const BodyValue& copy)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID target = copy.as<Opcode::CopyDataProperties>().targetOperand();
    if (index.isParameter(target) || index.opcodeOf(target) != Opcode::NewObject || index.blockOf(target) != index.blockOf(copy.id()))
        return { };
    auto allocation = index.value(target).as<Opcode::NewObject>();
    std::optional<BodyUser> first = index.firstUser(target);
    if (!allocation.fields().empty() || allocation.prototypeOperand() || !first || first->kind != BodyUseKind::Operand || first->value != copy.id())
        return { };

    LayoutID layout = allocation.layout();
    Reference built = walker.referenceOf(target);
    if (!built || !built.isValue() || walker.program().at(layout).kind != LayoutKind::Record || !walker.plan().slots().holdsCompiledObjects(layout))
        return { };
    const Operation& made = walker.code().at(built.id());
    return made.opcode == Opcode::NewObject && made.layout() == layout && made.fields().empty() ? layout : LayoutID();
}

static bool isPlainPropertyForCopyInTypedAccess(const Walker& walker, FieldReference field)
{
    const Program& program = walker.program();
    if (field.field >= program.at(field.layout).fields.size() || !walker.plan().slot(field.layout, field.field).hasStorage())
        return false;
    FieldFacts facts = walker.facts().field(field);
    const Field& declared = program.at(field.layout).fields[field.field];
    if (facts.facts.contains(FieldFact::IsAccessor) || !facts.formOfValue.isEmpty() || walker.plan().slot(field.layout, field.field).representation == SlotRepresentation::DeferredClosure)
        return false;
    return declared.key && !declared.computedKey && declared.form.isEmpty() && !declared.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods, FieldFlags::IsHidden });
}

static std::optional<Vector<FieldIndex, 8>> fieldsOfTargetForCopyInTypedAccess(const Walker& walker, const BodyValue& copy, LayoutID target, LayoutID source, std::span<const FieldIndex> order, Vector<FieldIndex, 8>& copied)
{
    const Program& program = walker.program();
    auto excludedKeys = copy.as<Opcode::CopyDataProperties>().excludedKeys();
    Vector<FieldIndex, 8> fieldsOfTarget;
    for (FieldIndex index : order) {
        if (!isPlainPropertyForCopyInTypedAccess(walker, { source, index }))
            return std::nullopt;
        PropertyKeyID key = program.at(source).fields[index].key;
        if (std::ranges::contains(excludedKeys, key))
            continue;
        std::optional<FieldReference> field = findField(program, target, key);
        if (!field || field->layout != target || !isPlainPropertyForCopyInTypedAccess(walker, *field))
            return std::nullopt;
        copied.append(index);
        fieldsOfTarget.append(field->field);
    }
    return fieldsOfTarget;
}

static bool slotsAreAlikeForCopyInTypedAccess(const Walker& walker, LayoutID source, std::span<const FieldIndex> copied, LayoutID target, std::span<const FieldIndex> fields)
{
    const Plan& plan = walker.plan();
    const PlannedLayout& read = plan.layout(source);
    const PlannedLayout& written = plan.layout(target);
    if (plan.shapes().shapesOf(target).count != 1 || written.cellsAreHandedBack || !plan.keyOrders().findOrderWord(target, fields))
        return false;
    for (size_t index = 0; index < copied.size(); ++index) {
        const PlacedSlot& slotRead = read.slots[copied[index]];
        const PlacedSlot& slotWritten = written.slots[fields[index]];
        if (slotRead.storage != SlotStorage::Inline || slotWritten.storage != SlotStorage::Inline || slotRead.representation != slotWritten.representation)
            return false;
        if (read.holdsRawValueFromBirth(copied[index]) || written.holdsRawValueFromBirth(fields[index]))
            return false;
        TypeID found = slotRead.typeThatStoresAreHeldTo();
        for (TypeID held : { slotWritten.type, slotWritten.provenType }) {
            if (held && (!found || !walker.types().isSubset(found, held)))
                return false;
        }
    }
    return true;
}

static bool wordsOfCellAreItsPropertiesForCopyInTypedAccess(Walker& walker, LayoutID layout)
{
    return keptByTypedAccesses(walker).whetherWordsOfCellAreItsProperties.ensure(layout.index(), [&] {
        const Program& program = walker.program();
        if (program.at(layout).kind != LayoutKind::Record || !orderWordTellsKeysOfObjectsOf(walker, layout) || !walker.plan().slots().layout(layout).appendedKeys.isEmpty())
            return false;
        for (FieldIndex index = 0; index < program.at(layout).fields.size(); ++index) {
            if (!isPlainPropertyForCopyInTypedAccess(walker, { layout, index }))
                return false;
        }
        return true;
    }).iterator->value;
}

static Reference buildTargetOfCopyForTypedAccess(Walker& walker, Origin origin, LayoutID layout, std::span<const FieldIndex> fields, std::span<const Reference> values)
{
    Draft allocation(Opcode::NewObject, origin);
    allocation.standsForNoValueOfInput = true;
    allocation.setLayout(layout);
    allocation.setFields(fields);
    allocation.setValuesOperands(values);
    walker.chooseConservativeFields(allocation);
    Reference object = walker.isUnreachable() ? Reference() : walker.build(WTF::move(allocation));
    noteFreshObject(walker, object, fields.size());
    return object;
}

static bool buildCopyOfFreshObjectForTypedAccess(Walker& walker, const BodyValue& copy, LayoutID layout)
{
    std::optional<Vector<PropertyOfFreshObject, 8>> properties = propertiesOfFreshObject(walker, walker.referenceOf(copy.as<Opcode::CopyDataProperties>().sourceOperand()));
    if (!properties)
        return false;
    Vector<FieldIndex, 8> order;
    for (const PropertyOfFreshObject& property : *properties)
        order.append(property.field.field);
    Vector<FieldIndex, 8> copied;
    std::optional<Vector<FieldIndex, 8>> fields = properties->isEmpty() ? std::optional(Vector<FieldIndex, 8>()) : fieldsOfTargetForCopyInTypedAccess(walker, copy, layout, properties->first().field.layout, order.span(), copied);
    if (!fields)
        return false;
    Vector<Reference, 8> values;
    for (const PropertyOfFreshObject& property : *properties) {
        if (copied.contains(property.field.field))
            values.append(property.value);
    }
    if (Reference object = buildTargetOfCopyForTypedAccess(walker, walker.originOf(copy), layout, fields->span(), values.span()))
        walker.bind(copy.as<Opcode::CopyDataProperties>().targetOperand(), object);
    return true;
}

static KeptByTypedAccesses::LayoutsThatSourcesFit findLayoutsThatSourcesOfCopyFitForTypedAccess(Walker& walker, std::span<const LayoutID> members)
{
    static constexpr size_t layoutsThatDecideWhateverIsMade = 3;
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    KeptByTypedAccesses::LayoutsThatSourcesFit found;
    Vector<LayoutID, 8> made;
    for (LayoutID member : members) {
        if (program.at(member).kind == LayoutKind::Record && !plan.shapes().shapesOf(member).isEmpty())
            made.append(member);
    }
    if (made.size() == 1) {
        found.onlyLayoutMade = made[0];
        return found;
    }
    for (LayoutID candidate : made) {
        if (!program.hasDeclaredMembers(candidate))
            continue;
        SetView<LayoutID, LayoutSetID> fitting = program.at(program.declaredMembersOf(candidate));
        if (fitting.size() < made.size() || !std::ranges::all_of(made, [&](LayoutID other) { return fitting.contains(other); }))
            continue;
        found.layoutsThatAllFit.append(candidate);
        if (found.layoutsThatAllFit.size() == layoutsThatDecideWhateverIsMade)
            break;
    }
    return found;
}

static LayoutID chooseLayoutThatSourceOfCopyIsWrittenAsForTypedAccess(const KeptByTypedAccesses::LayoutsThatSourcesFit& found, LayoutID layoutMade)
{
    if (found.onlyLayoutMade)
        return found.onlyLayoutMade;
    LayoutID written;
    bool allFitLayoutMade = false;
    for (LayoutID candidate : found.layoutsThatAllFit) {
        if (candidate == layoutMade) {
            allFitLayoutMade = true;
            continue;
        }
        if (written)
            return { };
        written = candidate;
    }
    return written || !allFitLayoutMade ? written : layoutMade;
}

static LayoutID layoutThatSourceOfCopyIsWrittenAsForTypedAccess(Walker& walker, const ReceiversOfTypedAccess& receivers, LayoutID layoutMade)
{
    TypeArena::SetOfRound round = walker.types().setOfRound(receivers.set);
    if (!round.isKeptForBuild && liesInThread(receivers.set))
        return chooseLayoutThatSourceOfCopyIsWrittenAsForTypedAccess(findLayoutsThatSourcesOfCopyFitForTypedAccess(walker, receivers.members.span()), layoutMade);
    auto& kept = keptByTypedAccesses(walker).layoutsThatSourcesOfCopiesFit;
    auto known = kept.find(round.word);
    if (known == kept.end())
        known = kept.add(round.word, findLayoutsThatSourcesOfCopyFitForTypedAccess(walker, receivers.members.span())).iterator;
    return chooseLayoutThatSourceOfCopyIsWrittenAsForTypedAccess(known->value, layoutMade);
}

static std::optional<Vector<FieldIndex, 8>> fieldsFilledByLoadsForCopyInTypedAccess(Walker& walker, const BodyValue& copy, LayoutID layout, LayoutID layoutOfSource, std::span<const FieldIndex>& order, Vector<FieldIndex, 8>& copied)
{
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    if (!plan.slots().layout(layoutOfSource).appendedKeys.isEmpty() || !std::ranges::all_of(program.at(layoutOfSource).fields, [](const Field& field) { return field.flags.contains(FieldFlags::IsRequired); }))
        return std::nullopt;
    for (uint32_t number = 0; number < plan.keyOrders().numberOfOrdersOf(layoutOfSource); ++number) {
        if (plan.keyOrders().order(layoutOfSource, number).size() == program.at(layoutOfSource).fields.size())
            order = plan.keyOrders().order(layoutOfSource, number);
    }
    if (order.empty() || !plan.keyOrders().findOrderWord(layoutOfSource, order))
        return std::nullopt;
    return fieldsOfTargetForCopyInTypedAccess(walker, copy, layout, layoutOfSource, order, copied);
}

static void checkWordsBehindCopyForTypedAccess(Walker& walker, Origin origin, Reference made, LayoutID layout, std::span<const FieldIndex> fields)
{
    if (!walker.checksProofsOf(ProofSource::Field))
        return;
    const Plan& plan = walker.plan();
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.types().forLayout(layout));
    if (!receivers)
        return;
    for (FieldIndex field : fields) {
        Draft read(Opcode::GetSlot, origin);
        fillSlotOperationForTypedAccess(read, made, *receivers, walker.program().at(layout).fields[field].key);
        Reference copied = walker.build(WTF::move(read));
        if (walker.hasFailed() || walker.isUnreachable())
            return;
        if (!copied)
            continue;
        Representation form = walker.representationOf(copied);
        if (form == Representation::JSValue || form == Representation::Cell || form == Representation::Cell32)
            walker.checkThatSlotTakes(copied, plan.slot(layout, field).typeThatStoresAreHeldTo(), origin);
    }
}

static bool buildCopyBehindTestsForTypedAccess(Walker& walker, const BodyValue& copy, LayoutID layout, bool byWordsAlone = false)
{
    if (!walker.has(Mechanism::Guesses))
        return false;
    auto operation = copy.as<Opcode::CopyDataProperties>();
    TypeArena& types = walker.types();
    Reference source = walker.referenceOf(operation.sourceOperand());
    if (!source || !source.isValue())
        return false;

    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(source));
    bool isProven = !!receivers;
    bool isDeclared = true;
    if (!receivers) {
        ExpectationOfValue expectation = findExpectation(walker, operation.sourceOperand());
        if (!expectation.type || !types.overlap(walker.typeOf(source), types.withoutNullish(expectation.type)))
            return false;
        receivers = findReceiversOfTypedAccess(walker, types.withoutNullish(expectation.type));
        isDeclared = expectation.isDeclared;
    }
    if (!receivers)
        return false;

    const Plan& plan = walker.plan();
    const Program& program = walker.program();
    LayoutID layoutOfSource = layoutThatSourceOfCopyIsWrittenAsForTypedAccess(walker, *receivers, layout);
    if (!layoutOfSource)
        return false;

    bool copiesWords = !operation.excludedKeys().size() && receivers->members.contains(layout) && !plan.shapes().shapesOf(layout).isEmpty() && wordsOfCellAreItsPropertiesForCopyInTypedAccess(walker, layout);
    if (byWordsAlone && (layoutOfSource != layout || !copiesWords))
        return false;
    std::span<const FieldIndex> order;
    Vector<FieldIndex, 8> copied;
    std::optional<Vector<FieldIndex, 8>> fields;
    if (layoutOfSource != layout || !copiesWords) {
        if (!(fields = fieldsFilledByLoadsForCopyInTypedAccess(walker, copy, layout, layoutOfSource, order, copied)))
            return false;
    }

    TypeID typeOfLayout = types.forLayout(layoutOfSource);
    if (receivers->members.size() != 1 || !holdsOneMember(receivers->set)) {
        isProven = false;
        receivers = findReceiversOfTypedAccess(walker, typeOfLayout);
        if (!receivers)
            return false;
    }
    std::optional<ReceiversOfTypedAccess> receiversOfLayoutMade = fields && copiesWords ? findReceiversOfTypedAccess(walker, types.forLayout(layout)) : std::nullopt;
    if (!shouldOpenGuess(walker, source, typeOfLayout, isDeclared, closedOperationsOfCopyOfProperties))
        return false;

    Origin origin = walker.originOf(copy);
    Draft written = walker.draftOf(copy);
    if (walker.hasFailed())
        return true;
    walker.recordSpeculation();
    BlockID asWritten = walker.addBlock(BlockFrequency::Unlikely);
    BlockID behindCopy = walker.addBlock();
    auto continueIf = [&](Reference test, BlockID otherwise) {
        BlockID passed = walker.addBlock();
        walker.branch(test, passed, otherwise);
        walker.continueIn(passed);
    };
    auto narrowedBehindShapeTest = [&](const ReceiversOfTypedAccess& asked, BlockID otherwise) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(source);
        test.setLayouts(asked.set);
        Reference hasShape = walker.build(WTF::move(test));
        continueIf(hasShape, otherwise);
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(source);
        narrowed.setTestOperand(hasShape);
        narrowed.setOperandIndex(OperandIndex::ShapeTest::object);
        narrowed.setIsOnTakenEdge(true);
        return walker.build(WTF::move(narrowed));
    };
    auto copyWords = [&](Reference target, Reference from) {
        Draft words(Opcode::TryCopyFields, origin);
        words.setObjectOperand(target);
        words.setSourceOperand(from);
        words.setLayout(layout);
        continueIf(walker.build(WTF::move(words)), asWritten);
    };

    if (!fields) {
        Reference ofLayoutMade = isProven ? source : narrowedBehindShapeTest(*receivers, asWritten);
        copyWords(written.targetOperand(), ofLayoutMade);
        walker.jumpTo(behindCopy);

        walker.continueIn(asWritten);
        walker.chooseConservativeFields(written);
        walker.build(WTF::move(written));
        if (!walker.isUnreachable())
            walker.jumpTo(behindCopy);
        walker.continueIn(behindCopy);
        return true;
    }

    Reference filled = walker.addParameter(behindCopy, { }, Representation::Cell);
    BlockID ofOtherShape = receiversOfLayoutMade ? walker.addBlock() : asWritten;
    Reference ofLayoutWritten = isProven ? source : narrowedBehindShapeTest(*receivers, ofOtherShape);
    Draft hasOrder(Opcode::HasKeyOrder, origin);
    hasOrder.setObjectOperand(ofLayoutWritten);
    hasOrder.setLayout(layoutOfSource);
    hasOrder.setFields(order);
    continueIf(walker.build(WTF::move(hasOrder)), asWritten);
    Draft hasNothingElse(Opcode::HasNoExtraProperties, origin);
    hasNothingElse.setObjectOperand(ofLayoutWritten);
    hasNothingElse.setHasNoFieldOutsideSlot(true);
    continueIf(walker.build(WTF::move(hasNothingElse)), asWritten);

    if (slotsAreAlikeForCopyInTypedAccess(walker, layoutOfSource, copied.span(), layout, fields->span())) {
        if (Reference empty = walker.isUnreachable() ? Reference() : buildTargetOfCopyForTypedAccess(walker, origin, layout, { }, { })) {
            Draft words(Opcode::TryCopyFields, origin);
            words.setObjectOperand(empty);
            words.setSourceOperand(ofLayoutWritten);
            words.setLayout(layout);
            words.setSourceLayout(layoutOfSource);
            words.setFields(copied.span());
            walker.build(WTF::move(words));
            checkWordsBehindCopyForTypedAccess(walker, origin, empty, layout, fields->span());
            walker.state().noteOrderWord(empty, plan.keyOrders().findOrderWord(layout, fields->span()));
            walker.jumpTo(behindCopy, singleElementSpan(empty));
        }
    } else {
        Vector<Reference, 8> values;
        for (FieldIndex index : copied) {
            Draft read(Opcode::GetSlot, origin);
            fillSlotOperationForTypedAccess(read, ofLayoutWritten, *receivers, program.at(layoutOfSource).fields[index].key);
            values.append(walker.build(WTF::move(read)));
        }
        if (Reference object = walker.isUnreachable() ? Reference() : buildTargetOfCopyForTypedAccess(walker, origin, layout, fields->span(), values.span()))
            walker.jumpTo(behindCopy, singleElementSpan(object));
    }

    if (receiversOfLayoutMade) {
        walker.continueIn(ofOtherShape);
        Reference ofLayoutMade = narrowedBehindShapeTest(*receiversOfLayoutMade, asWritten);
        if (Reference empty = walker.isUnreachable() ? Reference() : buildTargetOfCopyForTypedAccess(walker, origin, layout, { }, { })) {
            copyWords(empty, ofLayoutMade);
            walker.jumpTo(behindCopy, singleElementSpan(empty));
        }
    }

    walker.continueIn(asWritten);
    if (Reference empty = walker.isUnreachable() ? Reference() : buildTargetOfCopyForTypedAccess(walker, origin, layout, { }, { })) {
        written.setTargetOperand(empty);
        walker.chooseConservativeFields(written);
        walker.build(WTF::move(written));
        if (!walker.isUnreachable())
            walker.jumpTo(behindCopy, singleElementSpan(empty));
    }
    walker.continueIn(behindCopy);
    if (!walker.isUnreachable())
        walker.bind(operation.targetOperand(), filled);
    return true;
}

static bool sourceOfCopyHasNoPropertiesForTypedAccess(Walker& walker, const BodyValue& copy)
{
    Reference source = walker.referenceOf(copy.as<Opcode::CopyDataProperties>().sourceOperand());
    if (!source)
        return false;
    Type type = walker.types().at(walker.typeOf(source));
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::NumberLike | Primitive::AnySymbol | Primitive::BigInt);
}

static bool buildCopyOfPropertiesForTypedAccess(Walker& walker, const BodyValue& copy)
{
    if (!copy.as<Opcode::CopyDataProperties>().excludedOperands().empty())
        return false;
    auto spread = copy.as<Opcode::CopyDataProperties>();
    LayoutID layout = layoutOfEmptyTargetOfCopyForTypedAccess(walker, copy);
    bool someFieldMayBeLeftOut = layout && keptByTypedAccesses(walker).whetherSomeFieldMayBeLeftOut.ensure(layout.index(), [&] {
        return std::ranges::any_of(walker.program().at(layout).fields, [](const Field& field) {
            return !field.flags.contains(FieldFlags::IsRequired);
        });
    }).iterator->value;
    Reference source = walker.referenceOf(spread.sourceOperand());
    if (someFieldMayBeLeftOut && walker.isInLoop() && walker.opensGuesses() && walker.currentBodyIndex().useCount(spread.targetOperand()) == 2 && source && !ownKeysInSight(walker, source)
        && buildCopyBehindTestsForTypedAccess(walker, copy, layout, true))
        return true;
    if (fillLiteralInSight(walker, copy))
        return true;
    if (layout && (buildCopyOfFreshObjectForTypedAccess(walker, copy, layout) || buildCopyBehindTestsForTypedAccess(walker, copy, layout)))
        return true;
    return defineOwnKeysOfSourceOnObjectThatStands(walker, copy);
}

bool buildTypedAccess(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (accessesByNameWithTypedForms.contains(opcode))
        return buildAccessByNameForTypedAccess(walker, value, InputsOfTypedOperation(walker, nullptr)) == TypedForm::Built;
    if (opcode == Opcode::CopyDataProperties) {
        if (sourceOfCopyHasNoPropertiesForTypedAccess(walker, value)) {
            walker.count(StatisticID::ObjectSpreadsRemovedAsEmpty);
            return true;
        }
        if (walkCopyApartFromSourceWithoutProperties(walker, value))
            return true;
        if (!buildCopyOfPropertiesForTypedAccess(walker, value))
            return false;
        walker.count(StatisticID::OwnKeyWalksExpanded);
        return true;
    }
    if (!operationsWithTypedAccesses.contains(opcode))
        return false;

    if (opcode == Opcode::GetModuleVariable)
        return buildVariableReadForTypedAccess(walker, value);
    if (opcode == Opcode::PutModuleVariable)
        return buildVariableStoreForTypedAccess(walker, value);
    if (opcode == Opcode::GetEnvironmentField)
        return buildEnvironmentReadForTypedAccess(walker, value);
    if (opcode == Opcode::PutEnvironmentField)
        return buildEnvironmentStoreForTypedAccess(walker, value);
    if (opcode == Opcode::SuperGet)
        return buildSuperReadForTypedAccess(walker, value);
    if (opcode == Opcode::SuperSet)
        return buildSuperStoreForTypedAccess(walker, value);
    return buildAccessUnderPrivateNameForTypedAccess(walker, value, InputsOfTypedOperation(walker, nullptr)) == TypedForm::Built;
}

ASCIILiteral whyAccessByNameHasNoTypedForm(Walker& walker, const BodyValue& value, TypeID typeOfObject)
{
    PropertyKeyID key = value.key();
    bool isStore = value.opcode() != Opcode::PropGet && value.opcode() != Opcode::PropHas;
    TypeArena& types = walker.types();
    Type type = types.at(typeOfObject);
    if (!isLookedUpAsNameForTypedAccess(walker.program(), key))
        return "the key is not looked up as a name"_s;
    if (!type.isJavaScriptValue())
        return "no value of JavaScript"_s;
    if (type.primitives)
        return "holds primitives too"_s;
    if (!type.hasOnlyLayouts()) {
        ASCIILiteral whyNothingIsExpected;
        if (findWhatRecordsExpectOffConstructorFunction(walker, value, typeOfObject, &whyNothingIsExpected))
            return "a constructor function: the form behind the guard of the name"_s;
        if (!whyNothingIsExpected.isNull())
            return whyNothingIsExpected;
        return type.hasUnboundedUnknown() ? "objects that nobody describes"_s : "objects without a layout (built-in classes, functions, named objects)"_s;
    }
    if (type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return "may have a layout below"_s;
    if (layoutKnownByStructureOfClass(walker, typeOfObject)) {
        ASCIILiteral whyNothing = "a class below a built-in: only reads and plain stores have a form behind the look at the structure"_s;
        bool hasForm = value.opcode() == Opcode::PropSet ? !!findFieldThatStoreOverwritesInHeirOfBuiltin(walker, value, typeOfObject, &whyNothing) : !!findWhatReadFindsOffHeirOfBuiltin(walker, value, typeOfObject, &whyNothing);
        return hasForm ? "a class below a built-in: the form behind the look at the structure"_s : whyNothing;
    }
    if (!holdsOneMember(type.layouts) && !familiesForTypedAccesses(walker).familyOf(type.layouts) && !familyAroundSetOfLayouts(walker, type.layouts) && types.at(type.layouts).size() > mostLayoutsWithoutFamilyAround)
        return "more layouts than a set holds that no family stands around"_s;
    const Plan& plan = walker.plan();
    unsigned inUse = 0;
    for (LayoutID member : membersOf(types, type.layouts)) {
        if (!plan.isInUse(member))
            continue;
        ++inUse;
        if (!plan.slots().holdsCompiledObjects(member))
            return "a layout whose objects are no compiled objects"_s;
        if (!isFoundByShapeTest(walker.program().at(member)))
            return "a layout that no test of the shape finds"_s;
    }
    if (!inUse)
        return "no layout of the type is in use: nothing is seen to make such an object"_s;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject);
    if (!receivers)
        return "no receivers, for a reason not listed"_s;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    bool isOne = receivers->members.size() == 1;
    if (field.coverage == CoverageOfTypedAccess::NoReceiver) {
        ASCIILiteral whyNoMethod = isOne ? "the name is no field of the one layout"_s : "the name is no field of any layout of the family"_s;
        if (!isStore && findMethodReadBehindLooksForTypedAccess(walker, value, *receivers, key, &whyNoMethod))
            return "a method above: the form behind the guard of its name"_s;
        if (value.opcode() == Opcode::PropGet && !value.isFreeName() && !isReadForCallAloneInTypedAccess(walker, value) && !paysToLookAtCellWhereReadFindsNothing(walker, value, typeOfObject) && looksAtCellShowThatReadFindsNothing(walker, *receivers, field, key))
            return "the read finds nothing, and the looks at the cell that would show it do not pay outside of loops where nobody has written what the receiver is"_s;
        return whyNoMethod;
    }
    if (field.coverage == CoverageOfTypedAccess::SomeReceivers) {
        if (isStore)
            return "the name is a field of some layouts of the family only"_s;
        if (!field.liesInOneSlotOfReceiversThatHaveIt)
            return "some layouts lack the name, and those that have it do not agree on one slot"_s;
        if (slotHoldsMarkForTypedAccess(walker, *receivers, key))
            return "some layouts lack the name, and a slot under it holds a mark"_s;
        if (possibleChangesForbid(receivers->possibleChanges, TypedAccess::FindsNoProperty))
            return "some layouts lack the name, and their objects may gain a property that nobody sees"_s;
        if (!field.isMovedOutByNoStore)
            return "some layouts lack the name, and a store may move it out of its slot in the others"_s;
        if (possibleChangesForbid(receivers->possibleChanges, TypedAccess::ReadsSlotThatMayBeEmpty))
            return "some layouts lack the name, and a possible change of the layout forbids the slot that may be empty"_s;
        return "some layouts lack the name, and something above the object may answer"_s;
    }
    if (!field.liesInOneSlot)
        return isOne ? "the field has no plain slot"_s : "the layouts of the family do not agree on one slot"_s;
    if (isStore && !field.isStoredByProgram)
        return "the field is a method or is never written"_s;
    TypedAccess mayBeEmpty = isStore ? TypedAccess::StoresIntoSlotThatMayBeEmpty : TypedAccess::ReadsSlotThatMayBeEmpty;
    TypedAccess filled = isStore ? TypedAccess::StoresIntoFilledSlot : TypedAccess::ReadsFilledSlot;
    if (possibleChangesForbid(receivers->possibleChanges, filled)) {
        if (!storesByNameIntoSlots.contains(value.opcode()))
            return "a possible change of the layout forbids even the filled slot"_s;
        TypedStoreAskedAhead ahead = askAheadAboutTypedStore(walker, value, *receivers, field, key, value.opcode() == Opcode::PropDefine);
        if (ahead.form != TypedForm::None)
            return ahead.creates ? "a store that may create, behind looks at the cell: the form is there"_s : "a store into a filled slot, behind looks at the cell: the form is there"_s;
        ASCIILiteral why = "a possible change of the layout forbids the store, and no look shows it"_s;
        for (LayoutID member : receivers->members) {
            LayoutID classLayout;
            if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, LookupAbove::OfStore, classLayout, &why) == WhoAnswersForWhatStandsAbove::Nobody)
                break;
        }
        return why;
    }
    if (!field.isRequired) {
        if (!field.isMovedOutByNoStore)
            return "optional field, and a store may move it out of its slot"_s;
        if (possibleChangesForbid(receivers->possibleChanges, mayBeEmpty))
            return "optional field, and a possible change of the layout forbids the slot that may be empty"_s;
        return "optional field, and something above the object may answer"_s;
    }
    return "has a slot: the form behind a filled slot"_s;
}

TypedForm typedFormOfAccess(Walker& walker, const BodyValue& value, const TypesOfInputs& types)
{
    if (accessesUnderPrivateNames.contains(value.opcode()))
        return buildAccessUnderPrivateNameForTypedAccess(walker, value, InputsOfTypedOperation(walker, &types));
    if (!accessesByNameWithTypedForms.contains(value.opcode()))
        return TypedForm::None;
    return buildAccessByNameForTypedAccess(walker, value, InputsOfTypedOperation(walker, &types));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
