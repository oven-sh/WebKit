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
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkStrings.h"
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
static constexpr size_t mostLayoutsInLoop = 16;

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
    bool isNeverDeleted { true };
    bool isRequired { true };
    bool isStoredByProgram { true };
    bool takesStoredValue { true };
    bool isNamedAbove { false };
    bool mayBeDefinedAbove { false };
    bool isFilledByEveryConstruction { true };
    bool isFilledAtEveryRead { true };
    bool mayBeListedWithoutStorage { false };
    bool isAppended { false };
    bool isAppendedByConstructors { true };
    bool isLaidLate { false };
    bool holdsNumbersStoredLate { true };
    bool isOfBirth { true };
    bool isDeclaredByStores { true };
    bool isFilledByInitializer { true };
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
    struct ReceiversOfSet {
        size_t size { 0 };
        FamilyID familyAround;
        std::optional<ReceiversOfTypedAccess> receivers;
    };
    HashMap<uint32_t, std::unique_ptr<ReceiversOfSet>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> receiversOfSets;
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
    struct AnswerAboveObjects {
        uint64_t question { UINT64_MAX };
        LayoutID classLayout;
        uint8_t who { 0 };
    };
    static constexpr unsigned prototypeAnswerLog = 10;
    Vector<AnswerAboveObjects> answersAboveObjects;
    HashMap<uint64_t, MemberFound> membersFoundAbove;
    HashMap<uint64_t, FunctionID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> methodsInRows;
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

static constexpr OptionSet<TypedAccess> storesOfExtensibleObjects { TypedAccess::DefinesFilledSlot, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::DefinesSlotThatMayBeEmpty };
static constexpr OptionSet<TypedAccess> accessesToSlotsThatMayBeEmpty { TypedAccess::ReadsSlotThatMayBeEmpty, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::DefinesSlotThatMayBeEmpty };
static constexpr WhatChangeForbids whatChangesForbid[] {
    { LayoutChange::IsFrozen, { TypedAccess::StoresIntoFilledSlot }, { TypedAccess::StoresIntoFilledSlot }, CellFact::NotFrozen },
    { LayoutChange::IsFrozen, storesOfExtensibleObjects, storesOfExtensibleObjects, CellFact::IsExtensible },
    { LayoutChange::GainsProperty, { TypedAccess::FindsNoProperty }, { TypedAccess::FindsNoProperty }, CellFact::NoExtraProperties },
    { LayoutChange::LosesProperty, { }, { } },
    { LayoutChange::GetsObjectAbove, { TypedAccess::ReadsSlotThatMayBeEmpty, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::FindsNoProperty }, { TypedAccess::ReadsSlotThatMayBeEmpty, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::FindsNoProperty }, CellFact::HasLayoutPrototype, { } },
    { LayoutChange::ChangesPrototype, { TypedAccess::FindsMethodAbove }, { TypedAccess::FindsMethodAbove }, CellFact::HasLayoutPrototype, { } },
    { LayoutChange::GetsAccessor, accessesToSlotsThatMayBeEmpty | OptionSet<TypedAccess> { TypedAccess::FindsMethodAbove }, accessesToSlotsThatMayBeEmpty, CellFact::NoFieldOutsideSlot, { TypedAccess::FindsMethodAbove } },
    { LayoutChange::GetsAccessor, { TypedAccess::FindsNoProperty }, { TypedAccess::FindsNoProperty }, CellFact::NoExtraProperties },
};
static constexpr OptionSet<CellFact> factsOfExtensibleObject { CellFact::NotFrozen, CellFact::IsExtensible };
static constexpr OptionSet<CellFact> factsThatLooksAtCellShow[] { factsOfExtensibleObject, CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype, CellFact::NoExtraProperties };
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
    return program.types.isLookedUpByName(key);
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

static const KeptByTypedAccesses::ReceiversOfSet& setReceiversForTypedAccess(Walker& walker, LayoutSetID set)
{
    return *keptByTypedAccesses(walker).receiversOfSets.ensure(set.bits(), [&] {
        auto kept = makeUniqueWithoutFastMallocCheck<KeptByTypedAccesses::ReceiversOfSet>();
        kept->size = walker.types().at(set).size();
        kept->familyAround = familyAroundSetOfLayouts(walker, set);
        ReceiversOfTypedAccess receivers;
        receivers.set = set;
        receivers.familyAround = kept->familyAround;
        if (addMembersInUseForTypedAccess(walker, receivers))
            kept->receivers = WTF::move(receivers);
        return kept;
    }).iterator->value;
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
        if (liesInProgram(receivers.set)) {
            const KeptByTypedAccesses::ReceiversOfSet& kept = setReceiversForTypedAccess(walker, receivers.set);
            if (!kept.receivers || (!kept.familyAround && kept.size > (walker.isInLoop() ? mostLayoutsInLoop : mostLayoutsWithoutFamilyAround))) {
                found.reset();
                return found;
            }
            receivers.familyAround = kept.familyAround;
            receivers.members = LayoutsOwnedOrLent<8>::lentBy(kept.receivers->members);
            receivers.possibleChanges = kept.receivers->possibleChanges;
            return found;
        }
        receivers.familyAround = familyAroundSetOfLayouts(walker, receivers.set);
        if (!receivers.familyAround && walker.types().at(receivers.set).size() > (walker.isInLoop() ? mostLayoutsInLoop : mostLayoutsWithoutFamilyAround)) {
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

static bool isBornWithoutFields(const Walker& walker, LayoutID layout)
{
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    const Vector<Field>& fields = walker.program().at(layout).fields;
    for (uint32_t field = 0; field < fields.size(); ++field) {
        if (cellOfFieldForTypedAccess(link, { layout, field }).isOfBirth)
            return false;
    }
    return true;
}

static bool isRequiredByParentForTypedAccess(const Program& program, LayoutID member, PropertyKeyID key)
{
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID parent = program.at(member).base; parent && numberOfClassesLeft--; parent = program.at(parent).base) {
        std::optional<FieldReference> field = findField(program, parent, key);
        if (!field)
            return false;
        if (fieldAt(program, *field).flags.contains(FieldFlags::IsRequired))
            return true;
    }
    return false;
}

static bool isWrittenWithoutUndefinedAndNull(TypeArena& types, const auto& declared)
{
    TypeID written = declared.written();
    if (!written || types.at(written).isAnything())
        return false;
    return !(types.at(written).primitives & Primitive::UndefinedOrNull & ~(declared.has(PositionFlags::OmitsUndefined) ? Primitive::Undefined : 0) & ~(declared.has(PositionFlags::OmitsNull) ? Primitive::Null : 0));
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
        field.isNeverDeleted &= walker.facts().field(reference).stability.contains(FieldStability::IsNeverDeleted);
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
            if (field.holdsNumbersStoredLate) {
                const auto& stored = walker.facts().held().classesStoredUnderNames;
                auto found = stored.isEmpty() ? stored.end() : stored.find(LinkTables::layoutAndName(member, key));
                field.holdsNumbersStoredLate = found != stored.end();
            }
            field.isAppendedByConstructors &= link.isAppendedFieldRequiredByTypeOfThis(reference) && !isLaidLate;
            field.isWrittenWithoutUndefinedAndNull = false;
            field.isDeclaredByStores = false;
            field.isFilledByInitializer = false;
            continue;
        }
        const Field& record = *recordOrNull;
        if (field.holdsNumbersStoredLate) {
            const LinkedCell* held = cell.isOfBirth ? nullptr : walker.facts().held().findCellOfField(reference);
            field.holdsNumbersStoredLate = held && held->proven && types.at(held->proven).primitives & Primitive::Number && types.at(held->proven).isNumberOrNullish();
        }

        bool isMethodOfClass = layout.kind == LayoutKind::Instance && record.flags.contains(FieldFlags::IsMethod);
        field.isStoredByProgram &= !isMethodOfClass && !record.flags.contains(FieldFlags::IsNeverWritten);
        bool isRequiredByWrittenType = record.originLayout && record.originField && fieldAt(program, { record.originLayout, *record.originField }).flags.contains(FieldFlags::IsRequired);
        const auto& declared = (record.originLayout && record.originField ? fieldAt(program, { record.originLayout, *record.originField }) : record).declared;
        TypeID written = declared.written();
        field.isWrittenWithoutUndefinedAndNull &= isWrittenWithoutUndefinedAndNull(types, declared);
        field.isFilledByInitializer = field.isFilledByInitializer && !cell.isOfBirth && layout.kind == LayoutKind::Instance && walker.facts().linked().fieldsFilledByInitializers.names(program, member, key) && isBornWithoutFields(walker, member);
        field.isDeclaredByStores &= layout.kind == LayoutKind::Instance && (!written || types.at(written).isAnything() || !declared.isGuarded());
        bool isRequired = record.flags.contains(FieldFlags::IsRequired) || isRequiredByWrittenType || layout.kind == LayoutKind::Container;
        if (!isRequired && field.isRequired && layout.kind == LayoutKind::Instance && isRequiredByParentForTypedAccess(program, member, key)) {
            isRequired = true;
            walker.count(StatisticID::FieldsRequiredByParents);
        }
        field.isRequired &= isRequired;
        field.isNamedAbove |= isMethodOfClass;
        field.mayBeDefinedAbove |= record.flags.contains(FieldFlags::MayBeDefinedAbove);
    }

    if (!numberWithField) {
        field.isFilledByInitializer = false;
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

static TypeID typeOfPrimitivesAboveObjectsOf(const Program& program, const LinkTables& link, TypeArena& types, LayoutID layout, PropertyKeyID key, bool* mayStopStores = nullptr)
{
    if (isOnObjectPrototype(program.nameClassOf(key)) || program.at(layout).kind != LayoutKind::Instance || program.at(layout).has(LayoutFlags::IsOfCreatedObject))
        return { };
    Vector<TypeID, 4> laid;
    if (!link.findPrimitivesStoredInSuperclasses(program, layout, key, laid, mayStopStores) || laid.isEmpty())
        return { };
    TypeID found = TypeTable::undefined;
    for (TypeID type : laid)
        found = types.unionOf(found, type);
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
        bool isNamedAbove = record && record->flags.contains(FieldFlags::IsMethod);
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
    bool isNamedAbove = record && record->flags.contains(FieldFlags::IsMethod);
    if (isNamedAbove || possibleChangesForbid(possibleChangesForTypedAccess(link, layout), TypedAccess::ReadsSlotThatMayBeEmpty))
        return { };
    if (nothingAnswersAboveObjectsOfForTypedAccess(program, link, layout, key))
        return types.withPrimitives(types.withoutPrimitives(atDoors, Primitive::Absent), Primitive::Undefined);
    auto isDeclaredByStores = [&] {
        TypeID written = record->declared.written();
        return program.at(layout).kind == LayoutKind::Instance && (!written || types.at(written).isAnything() || !record->declared.isGuarded());
    };
    TypeID above = isAppended || isDeclaredByStores() ? typeOfPrimitivesAboveObjectsOf(program, link, types, layout, key) : TypeID();
    return above ? types.unionOf(types.withoutPrimitives(atDoors, Primitive::Absent), above) : TypeID();
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

static bool emptySlotTakesPrimitiveAbove(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    if ((!field.isAppended && !field.isDeclaredByStores) || field.isNamedAbove || !field.isMovedOutByNoStore || field.coverage != CoverageOfTypedAccess::EveryReceiver || possibleChangesForbid(changesThatMayReachFieldForTypedAccess(receivers, field), TypedAccess::ReadsSlotThatMayBeEmpty))
        return false;
    const Program& program = walker.program();
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    return std::ranges::all_of(receivers.members, [&](LayoutID member) {
        return !!typeOfPrimitivesAboveObjectsOf(program, link, const_cast<Walker&>(walker).types(), member, key);
    });
}

static bool findsNothingForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    return field.coverage == CoverageOfTypedAccess::NoReceiver && !possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty)
        && nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key);
}

static BuiltinID findObjectPrototypeMember(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
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
    bool allReceiversInherit = std::ranges::all_of(receivers.members, [&](LayoutID member) {
        return nothingAnswersBelowObjectPrototypeForTypedAccess(program, link, member, key) && !slots.findField(program, member, key);
    });
    return allReceiversInherit ? location : BuiltinID();
}

static bool changesForbidObjectPrototypeRead(const ReceiversOfTypedAccess& receivers, OptionSet<CellFact> knownOfCell)
{
    return possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty, knownOfCell) || possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove, knownOfCell);
}

static bool paysToLookAtSlotForTypedAccess(const Walker& walker, const FieldOfTypedAccess& field, PropertyKeyID key, bool isRead = false, bool isLastResort = false)
{
    if (field.mayBeListedWithoutStorage)
        return false;
    bool isHeldFromBirth = field.isOfBirth && field.markAnswersForSlot && !field.isAppended;
    if (isHeldFromBirth && !field.isRequired)
        const_cast<Walker&>(walker).count(StatisticID::FieldsExpectedFromAllocation);
    bool isLikelyThere = isLastResort || field.isRequired || field.isFilledByInitializer || isHeldFromBirth || (isRead && (field.isAppended ? field.isAppendedByConstructors : field.isOfBirth));
    if (!isLikelyThere) {
        bool isNumberStoredLate = isRead && (field.isAppended || !field.isOfBirth) && field.holdsNumbersStoredLate && (walker.isInLoop() || walker.facts().linked().isCalledInLoops(walker.currentFunction()));
        if (!isNumberStoredLate)
            return false;
    }
    PropertyKeyKind kind = walker.program().at(key).kind;
    return kind == PropertyKeyKind::Text || kind == PropertyKeyKind::EnginePrivateName || kind == PropertyKeyKind::ModulePrivateName || (isRead && kind == PropertyKeyKind::CreatedSymbol);
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

static bool takesObjectBeingMade(const Walker& walker, const Function& function)
{
    return function.kind == FunctionKind::Initializer || (function.takesThisFromConstruction() && function.constructedClass) || walker.facts().linked().function(walker.currentFunction()).fillsNewObjects;
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
        return !takesObjectBeingMade(walker, function) || !thisParameter || index.blockOf(root) != entryBlock || root != index.block(entryBlock).parameter(*thisParameter) || (function.kind != FunctionKind::Initializer && everyReceiverIsBornByLiteralInTypedAccess(walker, receivers));
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

static bool isConstantLiteralInTypedAccess(const Walker& walker, ValueID made)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!made || index.isParameter(made) || index.opcodeOf(made) != Opcode::Data)
        return false;
    DataID data = index.value(made).as<Opcode::Data>().data();
    const DataNode::Object* literal = data ? walker.program().at(data).root.tryAs<DataNode::Object>() : nullptr;
    return literal && literal->layout;
}

static bool isBeingMadeByBodyBeingReadInTypedAccess(const Walker& walker, ValueID object)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID root = valueBehindOtherNamesForTypedAccess(index, object, walker.facts().summaries().at(walker.currentFunction()).otherNamesOfObjectsBeingMade.span());
    if (!root)
        return false;
    if (!index.isParameter(root)) {
        Opcode opcode = index.opcodeOf(root);
        if (opcode == Opcode::Data)
            return isConstantLiteralInTypedAccess(walker, root);
        return opcode == Opcode::SuperConstruct || opcode == Opcode::SuperConstructList || opcode == Opcode::NewObject;
    }
    const Function& function = walker.program().at(walker.currentFunction());
    std::optional<unsigned> thisParameter = function.thisParameterIndex();
    return takesObjectBeingMade(walker, function) && thisParameter && index.blockOf(root) == entryBlock && root == index.block(entryBlock).parameter(*thisParameter);
}

bool canBePartiallyConstructed(Walker& walker, ValueID object, LayoutSetID layouts)
{
    const BodyIndex& index = walker.currentBodyIndex();
    FunctionID current = walker.currentFunction();
    ValueID root = walker.plan().keyOrders().isMethodOfConstructions(current) ? valueBehindOtherNamesForTypedAccess(index, object) : ValueID();
    std::optional<unsigned> thisParameter = root ? walker.program().at(current).thisParameterIndex() : std::nullopt;
    if (!thisParameter || !index.isParameter(root) || index.blockOf(root) != entryBlock || root != index.block(entryBlock).parameter(*thisParameter))
        return false;
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    std::span<const PropertyKeyID> names = walker.namesUsedOff(root);
    bool namesNoAllocatedField = !names.empty() && std::ranges::all_of(walker.types().listOf(layouts), [&](LayoutID layout) {
        return std::ranges::none_of(names, [&](PropertyKeyID name) {
            std::optional<FieldReference> field = findField(walker.program(), layout, name);
            return field && (cellOfFieldForTypedAccess(link, *field).isOfBirth || cellOfFieldForTypedAccess(link, declarationOf(walker.program(), *field)).isOfBirth);
        });
    });
    if (namesNoAllocatedField)
        walker.count(StatisticID::UnfinishedObjectsAdmitted);
    return namesNoAllocatedField;
}

static Reference valueInSlotForTypedAccess(Walker& walker, Reference object, const FieldOfTypedAccess& field)
{
    Reference held = walker.state().slot(object, field.place);
    TypeID type = held && !isClosureLeftUnmade(walker, held) ? walker.typeOf(held) : TypeID();
    return type && !(walker.types().at(type).primitives & Primitive::Absent) ? held : Reference();
}

static bool isSlotKnownEmpty(Walker& walker, Reference object, const FieldOfTypedAccess& field)
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

static NEVER_INLINE bool slotUnderMarkedKeyHoldsMark(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    const SlotPlan& slots = walker.plan().slots();
    for (LayoutID member : receivers.members) {
        std::optional<FieldIndex> field = slots.findField(walker.program(), member, key);
        if (field && slots.slot(member, *field).representation == SlotRepresentation::DeferredClosure)
            return true;
    }
    return false;
}

static ALWAYS_INLINE bool slotHoldsMarkForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    return walker.plan().slots().someSlotUnderKeyHoldsMark(key) && slotUnderMarkedKeyHoldsMark(walker, receivers, key);
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

static bool buildModuleMethod(Walker& walker, const BodyValue& read, FunctionID method, bool mayReadRunCode = false)
{
    if (!method || (read.opcode() != Opcode::PropGet && read.opcode() != Opcode::SuperGet) || !walker.has(Mechanism::DirectCalls) || !capturesOnlyModuleScopes(walker, method)
        || !walker.facts().linked().mayBeMentionedBy(walker.unit(), method))
        return false;
    ValueID user = walker.currentBodyIndex().onlyUsingValue(read.id());
    if (!user || walker.currentBodyIndex().opcodeOf(user) != Opcode::Call || walker.currentBodyIndex().value(user).calleeOperand() != read.id())
        return false;
    Origin origin = walker.originOf(read);
    ModuleID module = walker.program().at(method).module;
    std::optional<SingleInstance> only = findSingleInstance(walker, module, origin);
    if (!only)
        return walker.hasFailed() || walker.isUnreachable();
    if (!only->isSingleInstance) {
        walker.bind(read.id(), buildClosureOverInstance(walker, method, only->instance, origin));
        return true;
    }

    uint32_t numberOfCaptures = static_cast<uint32_t>(walker.program().at(method).captures.size());
    BlockID readsClosure = walker.addBlock(BlockFrequency::Rare);
    BlockID hasOneInstance = walker.addBlock();
    BlockID join = walker.addBlock();
    Vector<Reference, 2> captures;
    for (uint32_t capture = 0; capture < numberOfCaptures; ++capture)
        captures.append(walker.addParameter(join, { }, Representation::Cell));
    walker.branch(only->isSingleInstance, hasOneInstance, readsClosure);
    walker.recordSpeculation();

    walker.continueIn(readsClosure);
    Draft asWritten = walker.draftOf(read);
    walker.chooseConservativeFields(asWritten);
    Effects effects = Effects::ofReadByNameThatRunsNoCode();
    Vector<Reference, 2> arguments;
    if (Reference closure = walker.build(WTF::move(asWritten), mayReadRunCode ? nullptr : &effects)) {
        for (uint32_t capture = 0; capture < numberOfCaptures; ++capture) {
            Draft captured(Opcode::GetCapture, origin);
            walker.chooseConservativeFields(captured);
            captured.setFunction(method);
            captured.setIndex(capture);
            captured.setClosureOperand(closure);
            arguments.append(walker.build(WTF::move(captured)));
        }
    }
    if (walker.hasFailed())
        return true;
    if (!walker.isUnreachable())
        walker.jumpTo(join, arguments.span());

    walker.continueIn(hasOneInstance);
    arguments.shrink(0);
    for (Reference scope : capturedModuleScopes(walker, method, instanceAfterTest(walker, module, *only, origin), origin))
        arguments.append(scope);
    if (walker.hasFailed())
        return true;
    walker.jumpTo(join, arguments.span());
    walker.continueIn(join);
    walker.bind(read.id(), buildClosureOverScopes(walker, method, captures.span(), origin));
    return true;
}

static FunctionID findStaticMethod(const Walker& walker, const BodyValue& read, const OperandsOfTypedAccess& operands, PropertyKeyID key)
{
    Type type = walker.types().at(operands.typeOfObject);
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    LayoutID classLayout = constructor ? walker.program().at(constructor).constructedClass : LayoutID();
    std::span<const uint32_t> places = classLayout ? walker.facts().placesOfMembersUnder(classLayout, ClassSide::Static, key) : std::span<const uint32_t>();
    if (places.size() != 1)
        return { };
    const ClassMember& member = walker.facts().membersOf(classLayout)[places[0]];
    TypeID found = member.kind == MethodKind::Method && member.function ? widestTypeOfOperation(const_cast<Walker&>(walker), read) : TypeID();
    return found && walker.types().isSubset(found, walker.types().forFunction(member.function)) ? member.function : FunctionID();
}

FunctionID findModuleScopeGetter(Walker& walker, const BodyValue& read)
{
    if (read.opcode() != Opcode::PropGet || walker.program().moduleInstances.isEmpty() || read.isFreeName() || !walker.has(Mechanism::DirectCalls))
        return { };
    Reference object = walker.referenceOf(read.objectOperand());
    TypeID type = object && object.isValue() ? walker.typeOf(object) : TypeID();
    std::optional<ReceiversOfTypedAccess> receivers = type && walker.types().at(type).isObject() ? findReceiversOfTypedAccess(walker, type) : std::nullopt;
    if (!receivers || describeFieldOfTypedAccess(walker, *receivers, read.key(), { }, true).coverage != CoverageOfTypedAccess::NoReceiver)
        return { };
    MemberFound member = findMemberAboveReceiversForTypedAccess(walker, *receivers, read.key());
    if (member.kind != MemberFound::Kind::Accessor || !member.getter || !capturesOnlyModuleScopes(walker, member.getter))
        return { };
    return walker.facts().linked().mayBeMentionedBy(walker.unit(), member.getter) ? member.getter : FunctionID();
}

static bool isReadForCallAloneInTypedAccess(const Walker&, const BodyValue&);

static bool readsCalledStaticMethod(Walker& walker, const BodyValue& read, const OperandsOfTypedAccess& operands, FunctionID method)
{
    const Program& program = walker.program();
    Type holder = operands.typeOfObject ? walker.types().at(operands.typeOfObject) : Type();
    FunctionID constructor = operands.typeOfObject && holder.isObject() && holder.isJavaScriptValue() ? singleFunctionOf(walker.types(), holder) : FunctionID();
    LayoutID classLayout = constructor ? classOfClassObject(program, constructor) : LayoutID();
    if (!classLayout || walker.facts().linked().layout(classLayout).namesAreDefinedPastLists[static_cast<size_t>(ClassSide::Static)])
        return false;
    const Function& callee = program.at(method);
    for (uint32_t parameter = 0; parameter < callee.numberOfEntryParameters(); ++parameter) {
        if (callee.entryParameterKind(parameter) == EntryParameter::Callee)
            return false;
    }
    return callee.captures.isEmpty() && walker.has(Mechanism::DirectCalls) && isReadForCallAloneInTypedAccess(walker, read);
}

static ALWAYS_INLINE TypedForm buildReadOfMemberForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const MemberFound& member)
{
    switch (member.kind) {
    case MemberFound::Kind::Unknown:
        return TypedForm::None;
    case MemberFound::Kind::Method: {
        if (operands.areOnlyTypes)
            return TypedForm::Built;
        if (!walker.program().moduleInstances.isEmpty() && buildModuleMethod(walker, value, member.method))
            return TypedForm::Built;
        Draft read = walker.draftOf(value);
        walker.chooseConservativeFields(read);
        Effects effects = Effects::ofReadByNameThatRunsNoCode();
        if (walker.isInLoop() && readsCalledStaticMethod(walker, value, operands, member.method))
            effects.writes.remove(AbstractHeapSet { AbstractHeap::ClassObjects });
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
static TypedForm buildObjectPrototypeRead(Walker&, const BodyValue&, const OperandsOfTypedAccess&, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID);
static TypedForm buildAccessToAccessorBehindLooksForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&, const ReceiversOfTypedAccess&, PropertyKeyID);
static TypedForm buildReadOffConstructorFunctionBehindGuardForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&);
static TypedForm buildReadOffHeirOfBuiltinForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&);
static TypedForm buildReadsOfMethodOffPrototypeOfClassForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&);
static TypedForm buildClassPrototypeReadForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&);
static bool orderWordShowsThatCellLacksFieldForTypedAccess(Walker&, Reference object, const ReceiversOfTypedAccess&, PropertyKeyID);
static FunctionID findMethodForEmptyField(const Walker&, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID);
static bool shouldReadMethodUnderField(const Walker&);
static TypedForm buildFieldOverMethodRead(Walker&, const BodyValue&, Reference object, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID, FunctionID method);
static bool closureTableHoldsMethodForTypedAccess(const Walker&, FunctionID method);
static TypedForm bindClosureThatClosureTableHoldsForTypedAccess(Walker&, const BodyValue&, FunctionID method, Origin);
static bool isReadForCallAloneInTypedAccess(const Walker&, const BodyValue&);
static void askAgainGuardsAboveReceiversForTypedAccess(Walker&, const ReceiversOfTypedAccess&, PropertyKeyID, bool stores, Origin);
static void askAgainWhetherFieldHidesAccessorForTypedAccess(Walker&, Reference object, const ReceiversOfTypedAccess&, PropertyKeyID, Origin);

static Reference slotValueAfterStep(Walker& walker, uint32_t firstBuilt, Opcode opcode, PropertyKeyID key, Reference stored)
{
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    const Code& code = walker.code();
    if (!stored || !stored.isValue() || !walker.isInLoop() || walker.representationOf(stored) != Representation::Double || !steps.contains(code.at(stored.id()).opcode))
        return stored;
    for (uint32_t built = code.numberOfValues(); built-- > firstBuilt;) {
        const Operation& store = code.at(ValueID(built));
        if (store.opcode != opcode || store.key() != key)
            continue;
        ValueID held = store.valueOperand();
        while (!code.isConstant(held) && conversionsOfForm.contains(code.at(held).opcode) && code.at(held).opcode != Opcode::NumberToInteger)
            held = code.at(held).operand(0);
        if (code.isConstant(held) || code.at(held).opcode != Opcode::NumberToInteger || code.at(held).operand(0) != stored.id())
            return stored;
        walker.count(StatisticID::StepsFromIntegersOfSlots);
        return Reference::value(held);
    }
    return stored;
}

bool unmarkedObjectsAreNotFrozen(const Walker& walker, TypeID typeOfObject)
{
    if (!typeOfObject)
        return false;
    Type type = walker.types().at(typeOfObject);
    return !type.primitives && type.layouts && type.hasOnlyLayouts() && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) && std::ranges::all_of(membersOf(walker.types(), type.layouts), [&](LayoutID member) {
        return walker.plan().layout(member).marksFrozenObjects;
    });
}

bool unmarkedObjectsHaveLayoutPrototype(const Walker& walker, TypeID typeOfObject)
{
    if (!typeOfObject)
        return false;
    Type type = walker.types().at(typeOfObject);
    return !type.primitives && type.layouts && type.hasOnlyLayouts() && !type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow) && std::ranges::all_of(membersOf(walker.types(), type.layouts), [&](LayoutID member) {
        return walker.plan().layout(member).marksOtherPrototypes;
    });
}

static OptionSet<CellFact> cellFactsWithMark(const Walker& walker, Reference object)
{
    OptionSet<CellFact> facts = walker.state().cellFacts(object);
    if (!facts.contains(CellFact::BearsNoMark))
        return facts;
    if (!facts.contains(CellFact::NotFrozen) && unmarkedObjectsAreNotFrozen(walker, walker.typeOf(object)))
        facts.add(CellFact::NotFrozen);
    if (!facts.contains(CellFact::HasLayoutPrototype) && unmarkedObjectsHaveLayoutPrototype(walker, walker.typeOf(object)))
        facts.add(CellFact::HasLayoutPrototype);
    return facts;
}

OptionSet<CellFact> knownCellFacts(const Walker& walker, Reference object)
{
    OptionSet<CellFact> facts = cellFactsWithMark(walker, object);
    if (walker.shouldAssumeGuardsAndFacts())
        facts.add({ CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype, CellFact::NoExtraProperties });
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
    if (!field.liesInFormWithMarks)
        return false;
    if (field.markAnswersForValue && field.markAnswersForSlot)
        return true;
    if (!field.isRequired)
        return false;
    return field.isWrittenWithoutUndefinedAndNull || (field.liesInFormOfNumbersOrBooleans && !field.liesInOptionalBoolean);
}

static ALWAYS_INLINE bool markOfCellAnswersForFieldInTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, ValueID object)
{
    bool lookAsksForValue = lookAsksForValueOfWrittenTypeInTypedAccess(field) && !field.markAnswersForValue;
    return field.liesInOneSlot && field.coverage == CoverageOfTypedAccess::EveryReceiver && field.markAnswersForSlot && !lookAsksForValue && !field.isAppended && paysToLookAtSlotForTypedAccess(walker, field, key)
        && !possibleChangesForbid(receivers.possibleChanges, TypedAccess::ReadsFilledSlot) && !isBeingMadeByBodyBeingReadInTypedAccess(walker, object);
}

static bool slotMayBeAskedForWrittenType(const Walker& walker, LayoutID member, FieldIndex index, PropertyKeyID key)
{
    const Program& program = walker.program();
    const PlacedSlot& slot = walker.plan().slots().slot(member, index);
    bool holdsBoxedNumbers = slot.representation == SlotRepresentation::JSValue && slot.type && walker.types().at(slot.type).isNumberOrNullish();
    if (!holdsBoxedNumbers && (slot.representation == SlotRepresentation::JSValue || slot.representation == SlotRepresentation::DeferredClosure || !canLeaveSlot(slot.representation)))
        return false;
    bool holdsNumbersOrBooleans = holdsBoxedNumbers || !traitsOf(slot.representation).holdsCell;
    const Layout& layout = program.at(member);
    if (index >= layout.fields.size())
        return holdsNumbersOrBooleans && linkTablesForTypedAccesses(walker).isAppendedFieldRequiredBySomeWrittenType(FieldReference { member, index });
    const Field& record = layout.fields[index];
    const Field& origin = record.originLayout && record.originField ? fieldAt(program, { record.originLayout, *record.originField }) : record;
    if (!holdsNumbersOrBooleans && !isWrittenWithoutUndefinedAndNull(walker.types(), origin.declared))
        return false;
    return origin.flags.contains(FieldFlags::IsRequired) || record.flags.contains(FieldFlags::IsRequired) || layout.kind == LayoutKind::Container || (layout.kind == LayoutKind::Instance && isRequiredByParentForTypedAccess(program, member, key));
}

void addEffectsOfStoringNothing(const Walker& walker, std::span<const LayoutID> receivers, PropertyKeyID key, TypeID typeOfStored, Effects& effects)
{
    if (typeOfStored && !walker.types().at(typeOfStored).mayBeUndefinedOrNull())
        return;
    for (LayoutID member : receivers) {
        std::optional<FieldIndex> field = walker.plan().slots().findField(walker.program(), member, key);
        if (field && walker.plan().layout(member).holdsRawValueFromBirth(*field))
            effects.writes.add(AbstractHeap::NamedProperties);
        else if (field && slotMayBeAskedForWrittenType(walker, member, *field, key))
            effects.writes.add(AbstractHeap::WrittenTypes);
    }
}

void addEffectsOfStoringNothingAhead(Walker& walker, TypeID typeOfObject, PropertyKeyID key, TypeID typeOfStored, Effects& effects)
{
    if (std::optional<ReceiversOfTypedAccess> receivers = typeOfObject ? findReceiversOfTypedAccess(walker, typeOfObject) : std::nullopt)
        addEffectsOfStoringNothing(walker, receivers->members.span(), key, typeOfStored, effects);
    else
        effects.writes.add(AbstractHeapSet { AbstractHeap::NamedProperties, AbstractHeap::WrittenTypes });
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

std::optional<FilledSlot> findPermanentlyFilledSlot(Walker& walker, TypeID typeOfObject, PropertyKeyID key, bool everyStoreFits)
{
    if (!walker.has(Mechanism::TypedOperations) || !typeOfObject || walker.types().at(typeOfObject).primitives || !walker.types().at(typeOfObject).isJavaScriptValue())
        return std::nullopt;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject);
    if (!receivers)
        return std::nullopt;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || (!field.isMovedOutByNoStore && !everyStoreFits) || slotHoldsMarkForTypedAccess(walker, *receivers, key) || possibleChangesForbid(receivers->possibleChanges, TypedAccess::ReadsFilledSlot))
        return std::nullopt;
    return FilledSlot { field.place, receivers->set };
}

std::optional<FilledSlot> findFilledSlotOf(Walker& walker, Reference object, PropertyKeyID key)
{
    TypeID typeOfObject = walker.typeOf(object);
    if (std::optional<FilledSlot> filled = findFilledSlot(walker, typeOfObject, key))
        return filled;
    return findKnown(walker, KnownNote::FilledSlot, object, { }, key.bits()) ? findPermanentlyFilledSlot(walker, typeOfObject, key) : std::nullopt;
}

static bool slotStaysFilledForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    return field.coverage == CoverageOfTypedAccess::EveryReceiver && field.liesInOneSlot && field.isMovedOutByNoStore && field.isNeverDeleted && field.isNeverRedefined && !field.mayBeListedWithoutStorage && !field.isFilledAtEveryRead && !slotHoldsMarkForTypedAccess(walker, receivers, key);
}

static void noteFilledSlotForTypedAccess(Walker& walker, Reference object, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    if (object && object.isValue() && !walker.code().isConstant(object.id()) && slotStaysFilledForTypedAccess(walker, receivers, field, key))
        noteKnown(walker, KnownNote::FilledSlot, object, { }, key.bits(), LocationMasks(), 1);
}

static bool isSlotKnownFilledForTypedAccess(const Walker& walker, Reference object, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    return object && object.isValue() && findKnown(walker, KnownNote::FilledSlot, object, { }, key.bits()) && slotStaysFilledForTypedAccess(walker, receivers, field, key);
}

bool slotStaysFilled(Walker& walker, TypeID typeOfObject, PropertyKeyID key)
{
    std::optional<ReceiversOfTypedAccess> receivers = findPermanentlyFilledSlot(walker, typeOfObject, key) ? findReceiversOfTypedAccess(walker, typeOfObject) : std::nullopt;
    return receivers && slotStaysFilledForTypedAccess(walker, *receivers, describeFieldOfTypedAccess(walker, *receivers, key, { }, true), key);
}

Reference buildTestOfFilledSlot(Walker& walker, Reference object, PropertyKeyID key, Origin origin)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return { };
    Draft test(Opcode::IsSlotFilled, origin);
    fillSlotOperationForTypedAccess(test, object, *receivers, key);
    return walker.build(WTF::move(test));
}

void noteFilledSlot(Walker& walker, Reference object, PropertyKeyID key)
{
    if (slotStaysFilled(walker, walker.typeOf(object), key))
        noteKnown(walker, KnownNote::FilledSlot, object, { }, key.bits(), LocationMasks(), 1);
}

static bool updatesOwnValueForTypedAccess(const Code& code, Reference stored, Reference held)
{
    static constexpr OpcodeSet updates = allBitwiseOperators | OpcodeSet { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    static constexpr unsigned mostOperations = 2;
    auto isComputedFromHeld = [&](auto& self, ValueID value, unsigned operationsLeft) -> bool {
        if (code.isConstant(value))
            return false;
        const Operation& operation = code.at(underlyingCellOf(code, value));
        return operationsLeft && updates.contains(operation.opcode) && std::ranges::any_of(operation.operands(), [&](ValueID operand) {
            return operand && (underlyingCellOf(code, operand) == underlyingCellOf(code, held.id()) || self(self, operand, operationsLeft - 1));
        });
    };
    return held && held.isValue() && !code.isConstant(held.id()) && code.at(underlyingCellOf(code, held.id())).opcode == Opcode::GetSlotOrUndefined && stored.isValue() && isComputedFromHeld(isComputedFromHeld, stored.id(), mostOperations);
}

static bool nameMayBeAssigned(const Walker&, PropertyKeyID);
static LayoutID findClassOfAssignedValue(const Walker&, const ReceiversOfTypedAccess&, PropertyKeyID);
static std::optional<OptionSet<CellFact>> assignedValueCellFacts(const ReceiversOfTypedAccess&);

static TypedForm buildAssignedValueRead(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    LayoutID classLayout = value.opcode() == Opcode::PropGet && walker.isInLoop() ? findClassOfAssignedValue(walker, receivers, key) : LayoutID();
    std::optional<OptionSet<CellFact>> asked = classLayout ? assignedValueCellFacts(receivers) : std::nullopt;
    if (!asked)
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return TypedForm::BuiltBehindLookAtCell;
    if (!knownCellFacts(walker, operands.object).containsAll(*asked) || !walker.state().isMemberAssigned(classLayout, key))
        return TypedForm::None;
    MemberConditionRecord condition { MemberConditionKind::MemberAssigned, classLayout, ClassSide::Prototype, key };
    if (walker.checksProofs()) {
        Origin origin = walker.originOf(value);
        walker.checkFactsRememberedOfCell(operands.object, origin, *asked);
        checkGuardsThatAreRemembered(walker, singleElementSpan(condition), origin);
    }
    Draft member(Opcode::GetAssignedMember, walker.originOf(value));
    walker.chooseConservativeFields(member);
    member.setConditions(singleElementSpan(condition));
    walker.bind(value.id(), walker.build(WTF::move(member)));
    return TypedForm::Built;
}

enum class GuardOfFilledSlot : uint8_t { None, InIteration, InFront };

static GuardOfFilledSlot guardFilledSlot(Walker& walker, const BodyValue& read, Reference object, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !walker.canRestartIteration())
        return GuardOfFilledSlot::None;
    if (!field.liesInOneSlot || possibleChangesForbid(receivers.possibleChanges, TypedAccess::ReadsFilledSlot) || slotHoldsMarkForTypedAccess(walker, receivers, key))
        return GuardOfFilledSlot::None;
    TypeID type = walker.resultTypeOf(draftOfSlotOperationForTypedAccess(walker, read, Opcode::GetSlot, object, receivers, key));
    if (!type || isNeverType(type) || !walker.types().at(type).isNumber() || !isOnlyCalculatedWith(walker, read.id()))
        return GuardOfFilledSlot::None;
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    Walker::IterationPurity purity = walker.arrayGuardPurity();
    bool isStoredInLoop = walker.canTestStoredSlotBeforeLoop(object, key, purity);
    Reference isFilled;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, isStoredInLoop, purity);
        isFilled = walker.build(draftOfSlotOperationForTypedAccess(walker, read, Opcode::IsSlotFilled, object, receivers, key));
    }
    if (!isFilled || walker.hasFailed() || walker.isUnreachable())
        return GuardOfFilledSlot::None;
    if (isStoredInLoop && isFilled.isValue() && walker.tryBranchInPreheader(isFilled, true, purity, Walker::GuardedIterations::SomeWithUnguardedVersion)) {
        walker.count(StatisticID::StoredSlotsTestedBeforeLoops);
        walker.recordSpeculation();
        return !walker.isUnreachable() && !walker.hasFailed() ? GuardOfFilledSlot::InFront : GuardOfFilledSlot::None;
    }
    BlockID filled = walker.addBlock();
    BlockID empty = walker.addBlock(BlockFrequency::Rare);
    walker.letGuessFailUnasked(empty, origin);
    walker.branch(isFilled, filled, empty);
    walker.continueIn(empty);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(filled);
    walker.recordSpeculation();
    return !walker.isUnreachable() && !walker.hasFailed() ? GuardOfFilledSlot::InIteration : GuardOfFilledSlot::None;
}

static bool classObjectLacksKeyForTypedAccess(Walker& walker, TypeID proven, PropertyKeyID key)
{
    if (!proven || !key)
        return false;
    const Program& program = walker.program();
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    Type type = walker.types().at(proven);
    FunctionID function = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    LayoutID classLayout = function ? classOfClassObject(program, function) : LayoutID();
    if (!classLayout || isOnObjectPrototype(program.nameClassOf(key)) || !findsMemberWithoutCodeForTypedAccess(program, link, key))
        return false;
    const PropertyKey& record = program.at(key);
    if (record.kind != PropertyKeyKind::Text || program.isOpenedSomewhere(record.text))
        return false;
    for (LayoutID current = classLayout; current; current = program.at(current).base) {
        const Layout& written = program.at(current);
        if (!classStaysAsWrittenForTypedAccess(link, current) || (!written.base && written.has(LayoutFlags::HasHeritage)))
            return false;
    }
    const BuiltinTable& builtins = program.builtins();
    BuiltinName name = program.builtinNameOf(key);
    if (name && builtins.findMember(builtins.canonicalID(Builtins::FunctionPrototype), name).outcome != BuiltinMemberLookup::Outcome::Absent)
        return false;
    return link.findsNothing(program, classLayout, ClassSide::Static, key, Of::HolderAndAbove).has_value();
}

static TypedForm buildClassPrototypeReadForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, PropertyKeyID key)
{
    if (value.opcode() != Opcode::PropGet || !operands.typeOfObject || key != walker.program().keyOf(BuiltinNames::prototype))
        return TypedForm::None;
    Type type = walker.types().at(operands.typeOfObject);
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() && type.hasOnlyClosures() ? singleFunctionOf(walker.types(), type) : FunctionID();
    LayoutID classLayout = constructor ? classOfClassObject(walker.program(), constructor) : LayoutID();
    if (!classLayout)
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return TypedForm::Built;
    if (canGuardMethodOffPrototype(walker, value))
        return TypedForm::None;
    Draft prototypeObject(Opcode::GetClassPrototype, walker.originOf(value));
    walker.chooseConservativeFields(prototypeObject);
    prototypeObject.setClassObjectOperand(operands.object);
    prototypeObject.setLayout(classLayout);
    walker.bind(value.id(), walker.build(WTF::move(prototypeObject)));
    return TypedForm::Built;
}

static TypedForm buildReadForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, PropertyKeyID key)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, operands.typeOfObject);
    if (!receivers) {
        if (TypedForm form = buildClassPrototypeReadForTypedAccess(walker, value, operands, key); form != TypedForm::None)
            return form;
        if (TypedForm form = buildReadOfMemberForTypedAccess(walker, value, operands, findMemberOfNamedValueForTypedAccess(walker, operands.typeOfObject, key)); form != TypedForm::None)
            return form;
    }
    if (!receivers && operands.typeOfObject && walker.types().at(operands.typeOfObject).isObject() && classObjectLacksKeyForTypedAccess(walker, operands.typeOfObject, key)) {
        if (!operands.areOnlyTypes)
            walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
        return TypedForm::Built;
    }
    if (!receivers && !operands.areOnlyTypes && !walker.program().moduleInstances.isEmpty()) {
        if (FunctionID method = findStaticMethod(walker, value, operands, key); method && buildModuleMethod(walker, value, method, true))
            return TypedForm::Built;
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
        if (form == TypedForm::None && nameMayBeAssigned(walker, key))
            form = buildAssignedValueRead(walker, value, operands, *receivers, key);
        if (form == TypedForm::None)
            form = buildObjectPrototypeRead(walker, value, operands, *receivers, field, key);
        return form != TypedForm::None ? form : buildReadThatFindsNothingBehindLooksForTypedAccess(walker, value, operands, *receivers, field, key);
    }
    if (!field.liesInOneSlot)
        return TypedForm::None;
    if (!operands.areOnlyTypes && !walker.program().moduleInstances.isEmpty() && isReadForCallAloneInTypedAccess(walker, value)) {
        TypeID found = widestTypeOfOperation(walker, value);
        if (FunctionID method = found ? singleFunctionOf(walker.types(), walker.types().at(found)) : FunctionID(); method && walker.types().isSubset(found, walker.types().forFunction(method)) && buildModuleMethod(walker, value, method))
            return TypedForm::Built;
    }

    bool mayFindSlotEmpty = mayFindSlotEmptyInTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsSlotThatMayBeEmpty);
    bool isFilledByConstructions = (field.isFilledAtEveryRead && field.isMovedOutByNoStore) || isFilledForBodyBeingReadInTypedAccess(walker, *receivers, field, value.objectOperand(), key);
    auto readsFilledSlotForCallAlone = [&] {
        return isFilledByConstructions && walker.has(Mechanism::DirectCalls) && isReadForCallAloneInTypedAccess(walker, value);
    };
    bool looksAboveEmptySlot = !mayFindSlotEmpty && !isFilledByConstructions && value.opcode() == Opcode::PropGet && !slotHoldsMarkForTypedAccess(walker, *receivers, key) && emptySlotTakesPrimitiveAbove(walker, *receivers, field, key);
    if (operands.areOnlyTypes && looksAboveEmptySlot)
        return TypedForm::Built;
    if (operands.areOnlyTypes && !mayFindSlotEmpty && !isFilledByConstructions && value.opcode() == Opcode::PropGet && shouldReadMethodUnderField(walker) && findMethodForEmptyField(walker, *receivers, field, key))
        return TypedForm::Built;
    if (operands.areOnlyTypes) {
        if (mayFindSlotEmpty || isFilledByConstructions) {
            if (slotHoldsMarkForTypedAccess(walker, *receivers, key) && !readsFilledSlotForCallAlone())
                walker.noteFormAskedAhead(Opcode::GetDeferredClosureSlotOrUndefined, KnowledgeLevel::Proven);
            return TypedForm::Built;
        }
        if (field.isAppended && isBeingMadeByBodyBeingReadInTypedAccess(walker, value.objectOperand()) && !nothingAnswersAboveReceiversForTypedAccess(walker, *receivers, key))
            return TypedForm::None;
        TypedForm behindFilledSlot = formBehindFilledSlotForTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsFilledSlot);
        if (behindFilledSlot != TypedForm::None)
            return behindFilledSlot;
        static constexpr size_t fewestNamesOfLargeObject = 8;
        bool paysToLookAtCell = walker.isInLoop() || walker.weighsWhatIsWritten() || walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) || field.isLaidLate || field.isDeclaredByStores;
        if (!paysToLookAtCell) {
            if (!isReachedByLoops(walker) || walker.namesUsedOff(walker.currentBodyIndex().stripCasts(value.objectOperand())).size() < fewestNamesOfLargeObject)
                return TypedForm::None;
            walker.count(StatisticID::LargeObjectFieldsChecked);
        }
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
    bool isKnownFilled = !isFilled && isSlotKnownFilledForTypedAccess(walker, object, *receivers, field, key);
    isFilled |= isKnownFilled;
    if (!isFilled && looksAboveEmptySlot) {
        if (held && walker.has(Mechanism::Folding)) {
            walker.bind(value.id(), held);
            return TypedForm::Built;
        }
        Draft draft = draftOfSlotOperationForTypedAccess(walker, value, Opcode::GetSlotOrUndefined, object, *receivers, key);
        draft.setLevel(KnowledgeLevel::Expected);
        draft.setLooksAboveEmptySlot(true);
        walker.letLeave(object);
        Effects effects = Effects::ofReadByNameThatRunsNoCode();
        effects.writes.remove(AbstractHeapSet { AbstractHeap::MemberGuards });
        Reference read = walker.build(WTF::move(draft), &effects);
        walker.bind(value.id(), read);
        if (read)
            walker.state().noteSlot(object, field.place, read);
        return TypedForm::Built;
    }
    if (!isFilled && !mayFindSlotEmpty)
        mayFindSlotEmpty = mayFindSlotEmptyBehindLooksInTypedRead(walker, *receivers, field, key, knownCellFacts(walker, object), &walker.state());
    if (!isFilled && mayFindSlotEmpty)
        askAgainGuardsAboveReceiversForTypedAccess(walker, *receivers, key, false, walker.originOf(value));
    if (FunctionID method = !isFilled && !mayFindSlotEmpty ? findMethodForEmptyField(walker, *receivers, field, key) : FunctionID()) {
        if (TypedForm form = buildFieldOverMethodRead(walker, value, object, *receivers, field, key, method); form != TypedForm::None)
            return form;
    }
    if (!isFilled && !mayFindSlotEmpty)
        return TypedForm::None;
    if (held && walker.has(Mechanism::Folding)) {
        walker.bind(value.id(), held);
        return TypedForm::Built;
    }

    if (!isFilled && walker.has(Mechanism::Folding) && (orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, key) || isSlotKnownEmpty(walker, object, field))) {
        walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
        return TypedForm::Built;
    }

    GuardOfFilledSlot guard = isFilled ? GuardOfFilledSlot::None : guardFilledSlot(walker, value, object, *receivers, field, key);
    isFilled |= guard != GuardOfFilledSlot::None;
    if (walker.isUnreachable() || walker.hasFailed())
        return TypedForm::Built;
    if (guard == GuardOfFilledSlot::InFront) {
        Draft read = draftOfSlotOperationForTypedAccess(walker, value, Opcode::GetSlot, object, *receivers, key);
        Reference carried = walker.readCarriedField(read, field.place);
        if (!carried)
            carried = walker.build(WTF::move(read));
        if (carried) {
            walker.state().noteSlot(object, field.place, carried);
            noteFilledSlotForTypedAccess(walker, object, *receivers, field, key);
        }
        walker.bind(value.id(), carried);
        return TypedForm::Built;
    }

    if (isFilledByConstructions && receivers->members.size() == 1 && walker.has(Mechanism::Folding) && !walker.checksProofs()) {
        LinkedCell cell = cellOfFieldForTypedAccess(walker.facts().linked(), field.place);
        if (Reference only = cell.everyWriterIsKnown && cell.isFilledAtEveryRead ? walker.onlyValueOfType(cell.proven) : Reference()) {
            walker.bind(value.id(), only);
            return TypedForm::Built;
        }
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
    Reference carried = opcodeOfRead == Opcode::GetSlot && (isFilledByConstructions || isKnownFilled) && !markAnswers ? walker.readCarriedField(draft, field.place, isFilledByConstructions) : Reference();
    Reference read = carried ? carried : walker.build(WTF::move(draft));
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

static std::optional<FieldOfTypedAccess> findSlotField(Walker& walker, Reference object, PropertyKeyID key)
{
    if (!object || !object.isValue())
        return std::nullopt;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return std::nullopt;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || slotHoldsMarkForTypedAccess(walker, *receivers, key))
        return std::nullopt;
    return field;
}

Reference findFieldValue(Walker& walker, Reference object, PropertyKeyID key)
{
    std::optional<FieldOfTypedAccess> field = findSlotField(walker, object, key);
    return field ? valueInSlotForTypedAccess(walker, object, *field) : Reference();
}

void noteFieldProof(Walker& walker, Reference object, PropertyKeyID key, Reference proof)
{
    std::optional<FieldOfTypedAccess> field = findSlotField(walker, object, key);
    if (!field || !valueInSlotForTypedAccess(walker, object, *field))
        return;
    walker.state().noteSlot(object, field->place, proof);
    walker.noteStableProof(object, key, proof);
}

static bool slotTellsWhoHasPrivateNameForTypedAccess(const Walker&, LayoutID declaring);

static std::optional<OptionSet<CellFact>> factsRequiredOfHoistedField(Walker& walker, const BodyValue& read, Reference object)
{
    if (read.opcode() != Opcode::PropGet || read.isFreeName() || !object || !object.isValue() || walker.isUnreachable())
        return std::nullopt;
    PropertyKeyID key = read.key();
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return std::nullopt;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || slotHoldsMarkForTypedAccess(walker, *receivers, key))
        return std::nullopt;
    std::optional<OptionSet<CellFact>> asked = askAheadAboutReadOfSlotThatMayBeEmpty(walker, *receivers, field, key);
    return asked && asked->containsOnly({ CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype }) ? asked : std::nullopt;
}

bool isFieldOfUnknownObject(Walker& walker, const BodyValue& read, Reference object)
{
    if (read.opcode() != Opcode::PropGet || read.isFreeName() || !object || !object.isRecord() || walker.isUnreachable() || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    TypeID type = walker.typeOf(object);
    return type && (walker.types().isUninformative(type) || walker.types().at(type).anyObject) && siteMayAnswerReadWithoutOtherCode(walker, read);
}

bool canHoistFieldRead(Walker& walker, const BodyValue& read, Reference object)
{
    return factsRequiredOfHoistedField(walker, read, object) || isFieldOfUnknownObject(walker, read, object);
}

Reference buildHoistedFieldRead(Walker& walker, const BodyValue& read, Reference object, BlockID missing)
{
    std::optional<OptionSet<CellFact>> asked = factsRequiredOfHoistedField(walker, read, object);
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    if (!asked && isFieldOfUnknownObject(walker, read, object)) {
        Draft lookup(Opcode::GetValueAtSiteOrAbsent, origin);
        lookup.setObjectOperand(object);
        lookup.setKey(read.key());
        lookup.setReceiverIsNoObjectOfCompiledCode(isShapedLikeLayoutsThatItCanNeverBe(walker, read.objectOperand()));
        lookup.setValueType(TypeTable::anythingOrAbsent);
        return walker.build(WTF::move(lookup));
    }
    if (!asked)
        return { };
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (asked->contains(CellFact::NoFieldOutsideSlot) && !knownCellFacts(walker, object).contains(CellFact::NoFieldOutsideSlot)) {
        Draft test(Opcode::HasNoFieldOutsideSlot, origin);
        test.setObjectOperand(object);
        Reference passes = walker.build(WTF::move(test));
        if (!passes || walker.hasFailed())
            return { };
        BlockID passed = walker.addBlock();
        walker.branch(passes, passed, missing);
        walker.continueIn(passed);
        if (walker.isUnreachable())
            return { };
        walker.state().recordCellFacts(object, CellFact::NoFieldOutsideSlot);
    }
    Draft held(Opcode::GetSlotOrUndefined, origin);
    walker.chooseConservativeFields(held);
    fillSlotOperationForTypedAccess(held, object, *receivers, read.key());
    return walker.build(WTF::move(held));
}

static Reference readFieldArrayInPreheader(Walker& walker, PropertyKeyID key, Origin readOrigin, Reference object, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field)
{
    if (std::optional<uint64_t> known = findKnown(walker, KnownNote::FieldArrayInPreheader, object, { }, key.index()))
        return Reference::fromBits(static_cast<uint32_t>(*known));
    if (formBehindFilledSlotForTypedAccess(walker, receivers, field, key, TypedAccess::ReadsFilledSlot, true) != TypedForm::BuiltBehindFilledSlot)
        return { };
    Origin origin = readOrigin;
    origin.isTailCall = false;
    Draft held(Opcode::GetSlot, origin);
    walker.chooseConservativeFields(held);
    fillSlotOperationForTypedAccess(held, object, receivers, key);
    TypeID type = walker.resultTypeOf(held);
    if (!type || !hasKind(walker.types(), walker.types().withoutNullish(type), OperandKind::Array))
        return { };
    Draft test(Opcode::IsSlotFilled, origin);
    walker.chooseConservativeFields(test);
    fillSlotOperationForTypedAccess(test, object, receivers, key);
    Reference isFilled = walker.build(WTF::move(test));
    if (!isFilled || walker.isUnreachable())
        return { };
    BlockID filled = walker.addBlock();
    BlockID empty = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isFilled, filled, empty);
    Vector<Walker::JoinArm, 2> arms;
    walker.continueIn(empty);
    if (!walker.isUnreachable())
        arms.append({ empty, { walker.constant(Draft(Opcode::Undefined)) } });
    walker.continueIn(filled);
    if (Reference value = walker.isUnreachable() ? Reference() : walker.build(WTF::move(held)))
        arms.append({ filled, { value } });
    std::array<Reference, 1> array;
    if (walker.hasFailed() || !walker.joinArms(arms.span(), walker.addBlock(), array) || !array[0].isValue())
        return { };
    LocationMasks slot;
    slot.add(walker.program(), MemoryLocation::objectField(key));
    noteKnown(walker, KnownNote::FieldArrayInPreheader, object, { }, key.index(), slot, array[0].bits());
    walker.addFieldArrayRead({ object, key, field.place, array[0] });
    return array[0];
}

static Reference readNullishHolderFieldArray(Walker& walker, const BodyValue& read, Reference object)
{
    TypeArena& types = walker.types();
    TypeID known = walker.typeOf(object);
    TypeID objects = types.withoutNullish(known);
    if (objects == known || isNeverType(objects) || !types.at(known).isJavaScriptValue() || walker.representationOf(object) != Representation::JSValue)
        return { };
    PropertyKeyID key = read.key();
    if (std::optional<uint64_t> held = findKnown(walker, KnownNote::FieldArrayInPreheader, object, { }, key.index()))
        return Reference::fromBits(static_cast<uint32_t>(*held));
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, objects);
    if (!receivers)
        return { };
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || slotHoldsMarkForTypedAccess(walker, *receivers, key) || formBehindFilledSlotForTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsFilledSlot, true) != TypedForm::BuiltBehindFilledSlot)
        return { };
    Origin origin = walker.originOf(read);
    origin.isTailCall = false;
    Draft question(Opcode::TypeTest, origin);
    question.setValueOperand(object);
    question.setTestedType(TypeTable::object);
    Reference test = walker.build(WTF::move(question));
    if (!test || !test.isValue() || walker.isUnreachable() || walker.hasFailed())
        return { };
    BlockID isObject = walker.addBlock();
    BlockID isNullish = walker.addBlock(BlockFrequency::Rare);
    walker.branch(test, isObject, isNullish);
    Reference nothing = walker.constant(Draft(Opcode::Undefined));
    Vector<Walker::JoinArm, 2> arms;
    walker.continueIn(isNullish);
    if (!walker.isUnreachable())
        arms.append({ isNullish, { nothing } });
    walker.continueIn(isObject);
    if (!walker.isUnreachable()) {
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(object);
        proof.setTestOperand(test);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(true);
        Reference holder = walker.build(WTF::move(proof));
        Reference held = holder && holder.isValue() ? readFieldArrayInPreheader(walker, key, origin, holder, *receivers, field) : Reference();
        if (walker.hasFailed())
            return { };
        if (!walker.isUnreachable())
            arms.append({ walker.currentBlock(), { held ? held : nothing } });
    }
    std::array<Reference, 1> array;
    if (walker.hasFailed() || !walker.joinArms(arms.span(), walker.addBlock(), array) || !array[0].isValue())
        return { };
    LocationMasks slot;
    slot.add(walker.program(), MemoryLocation::objectField(key));
    noteKnown(walker, KnownNote::FieldArrayInPreheader, object, { }, key.index(), slot, array[0].bits());
    walker.addFieldArrayRead({ object, key, field.place, array[0] });
    walker.count(StatisticID::NullishHoldersReadInPreheaders);
    return array[0];
}

void Walker::noteFieldArrayHolder(Reference holder, Reference proof)
{
    if (!proof || !proof.isValue() || proof == holder)
        return;
    for (const FieldArrayRead& read : m_fieldArrayReads) {
        Reference held = read.holder == holder ? m_state.slot(holder, read.field) : Reference();
        if (held)
            m_state.noteSlot(proof, read.field, held);
    }
}

void Walker::noteFieldArrayProof(Reference array, Reference proof)
{
    if (!proof || !proof.isValue() || !m_types.at(typeOf(proof)).isObject())
        return;
    for (const FieldArrayRead& read : m_fieldArrayReads) {
        std::optional<uint64_t> known = read.array == array ? findKnown(*this, KnownNote::FieldArrayInPreheader, read.holder, { }, read.key.index()) : std::nullopt;
        if (known && *known == array.bits())
            m_state.noteSlot(read.holder, read.field, proof);
    }
}

Reference readFilledFieldInPreheader(Walker& walker, const BodyValue& read, Reference object, bool mustHoldArray, bool mayCheckSlot)
{
    if (!object || !object.isValue() || walker.isUnreachable())
        return { };
    PropertyKeyID key = keyOfAccessToSlot(walker.program(), read);
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers && mayCheckSlot && read.opcode() == Opcode::PropGet)
        return readNullishHolderFieldArray(walker, read, object);
    if (!receivers || (read.opcode() == Opcode::PrivateGet && !slotTellsWhoHasPrivateNameForTypedAccess(walker, read.layout())))
        return { };
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || slotHoldsMarkForTypedAccess(walker, *receivers, key))
        return { };
    if (!isFilledForBodyBeingReadInTypedAccess(walker, *receivers, field, read.objectOperand(), key)) {
        Reference filled = mustHoldArray ? Reference() : readFilledFieldOfProvenObject(walker, object, key, walker.originOf(read));
        return filled || !mayCheckSlot || read.opcode() != Opcode::PropGet ? filled : readFieldArrayInPreheader(walker, read.key(), walker.originOf(read), object, *receivers, field);
    }
    if (Reference held = valueInSlotForTypedAccess(walker, object, field))
        return held;
    Walker::ProofsHoldEverywhere filledWhereverObjectIs(walker);
    Draft draft(Opcode::GetSlot, walker.originOf(read));
    walker.chooseConservativeFields(draft);
    fillSlotOperationForTypedAccess(draft, object, *receivers, key);
    if (TypeID held = mustHoldArray ? walker.resultTypeOf(draft) : TypeID(); mustHoldArray && (!held || !hasKind(walker.types(), walker.types().withoutNullish(held), OperandKind::Array)))
        return { };
    Reference value = walker.build(WTF::move(draft));
    if (value)
        walker.state().noteSlot(object, field.place, value);
    return value;
}

Reference readFieldArrayOfHolder(Walker& walker, Reference object, PropertyKeyID key, Origin origin)
{
    if (!object || !object.isValue() || walker.isUnreachable())
        return { };
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return { };
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || slotHoldsMarkForTypedAccess(walker, *receivers, key))
        return { };
    if (Reference held = valueInSlotForTypedAccess(walker, object, field))
        return held;
    return readFieldArrayInPreheader(walker, key, origin, object, *receivers, field);
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

Reference readMethodForCallOnly(Walker& walker, Reference object, PropertyKeyID key, Origin origin)
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
        bool noReceiverHasProperty = !possibleChangesForbid(receivers->possibleChanges - LayoutChange::GetsObjectAbove, TypedAccess::FindsNoProperty) && std::ranges::all_of(receivers->members, [&](LayoutID member) {
            return link.findsNothing(program, member, ClassSide::Prototype, key, Of::ObjectItself) && !slots.findField(program, member, key);
        });
        return noReceiverHasProperty && bindBooleanForTypedAccess(walker, question, false);
    }
    if (!field.liesInOneSlot || !field.isMovedOutByNoStore || possibleChangesForbid(changesThatMayReachFieldForTypedAccess(*receivers, field) - LayoutChange::GetsObjectAbove, TypedAccess::ReadsSlotThatMayBeEmpty))
        return false;
    bool isFilled = provesSlotFilledForTypedAccess(walker, valueInSlotForTypedAccess(walker, object, field), key) || everyReceiverKeepsFieldInSlotThatIsNeverEmpty(walker, receivers->members.span(), key);
    if (walker.has(Mechanism::Folding) && isFilled)
        return bindBooleanForTypedAccess(walker, question, true);
    if (walker.has(Mechanism::Folding) && (orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, key) || isSlotKnownEmpty(walker, object, field)))
        return bindBooleanForTypedAccess(walker, question, false);
    walker.bind(question.id(), walker.build(draftOfSlotOperationForTypedAccess(walker, question, Opcode::IsSlotFilled, object, *receivers, key)));
    return true;
}

static bool findEveryOwnName(Walker& walker, TypeID typeOfObject, Vector<PropertyKeyID, mostTextsToldApart>& names)
{
    const Program& program = walker.program();
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject);
    if (!receivers || receivers->members.size() != 1 || possibleChangesForbid(receivers->possibleChanges - LayoutChange::GetsObjectAbove, TypedAccess::FindsNoProperty))
        return false;
    LayoutID layout = receivers->members.span()[0];
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    const LinkedLayout& linked = link.layouts[layout.index()];
    if (linked.isFilledUnderComputedKeys || linked.possibleChanges.containsAny({ LayoutChange::GainsProperty, LayoutChange::GetsAccessor }))
        return false;
    std::span<const Field> fields = program.at(layout).fields;
    if (program.at(layout).kind != LayoutKind::Record || fields.empty() || fields.size() > mostTextsToldApart || link.numberOfAppendedKeysOfPlan(layout) || !walker.plan().slots().layout(layout).appendedKeys.isEmpty())
        return false;
    for (const Field& listed : fields) {
        if (!listed.key || program.at(listed.key).kind != PropertyKeyKind::Text || !isLookedUpAsNameForTypedAccess(program, listed.key))
            return false;
        FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, listed.key, { }, true);
        bool slotAnswers = field.coverage == CoverageOfTypedAccess::EveryReceiver && field.liesInOneSlot && field.isMovedOutByNoStore && !field.mayBeListedWithoutStorage
            && !possibleChangesForbid(changesThatMayReachFieldForTypedAccess(*receivers, field) - LayoutChange::GetsObjectAbove, TypedAccess::ReadsSlotThatMayBeEmpty);
        if (!slotAnswers)
            return false;
        names.append(listed.key);
    }
    return true;
}

bool dispatchOwnPropertyTest(Walker& walker, const BodyValue& question, Reference object, Reference key)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses) || !object || !object.isValue() || !key || !key.isValue() || !walker.isInLoop() || walker.isUnreachable())
        return false;
    if (walker.code().at(walker.currentBlock()).frequency == BlockFrequency::Rare || !walker.types().at(walker.typeOf(key)).hasOnlyPrimitives(Primitive::AnyString))
        return false;
    Vector<PropertyKeyID, mostTextsToldApart> names;
    if (!findEveryOwnName(walker, walker.typeOf(object), names))
        return false;
    Origin origin = walker.originOf(question);
    origin.isTailCall = false;
    Vector<StringID, mostTextsToldApart> strings;
    Vector<BlockID, mostTextsToldApart> underNames;
    for (PropertyKeyID name : names) {
        strings.append(walker.program().at(name).text);
        underNames.append(walker.addBlock());
    }
    BlockID underNoName = walker.addBlock();
    BlockID join = walker.addBlock();
    Reference answer = walker.addParameter(join, TypeTable::boolean, Representation::Boolean);
    walker.count(StatisticID::OwnPropertyTestsDispatchedOverNames);
    walker.switchOnString(key, strings.span(), underNames.span(), underNoName, origin);
    for (size_t place = 0; place < names.size() && !walker.hasFailed(); ++place) {
        walker.continueIn(underNames[place]);
        if (walker.isUnreachable() || !buildOwnPropertyTest(walker, question, object, names[place]) || walker.isUnreachable())
            continue;
        Reference isOwn = walker.referenceOf(question.id());
        walker.jumpTo(join, singleElementSpan(isOwn));
    }
    walker.continueIn(underNoName);
    if (!walker.isUnreachable()) {
        bindBooleanForTypedAccess(walker, question, false);
        Reference isNotOwn = walker.referenceOf(question.id());
        walker.jumpTo(join, singleElementSpan(isNotOwn));
    }
    walker.continueIn(join);
    walker.bind(question.id(), answer);
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
    if (!isFilled && walker.has(Mechanism::Folding) && (orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, key) || isSlotKnownEmpty(walker, object, field))) {
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

bool areReceiversFilledAtAllReads(Walker& walker, TypeID receivers, PropertyKeyID key)
{
    std::optional<ReceiversOfTypedAccess> found = isLookedUpAsNameForTypedAccess(walker.program(), key) ? findReceiversOfTypedAccess(walker, receivers) : std::nullopt;
    if (!found)
        return false;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *found, key, { }, true);
    return field.coverage == CoverageOfTypedAccess::EveryReceiver && field.isFilledAtEveryRead && field.isMovedOutByNoStore;
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
    bool liesInOneSlot = field.coverage == CoverageOfTypedAccess::EveryReceiver ? field.liesInOneSlot : field.liesInOneSlotOfReceiversThatHaveIt;
    return liesInOneSlot && !field.mayBeListedWithoutStorage;
}

static OptionSet<CellFact> factsRequiredToOverwrite(const ReceiversOfTypedAccess& receivers, bool isDefinition)
{
    if (!receivers.possibleChanges.contains(LayoutChange::IsFrozen))
        return { };
    return isDefinition ? factsOfExtensibleObject : OptionSet<CellFact> { CellFact::NotFrozen };
}

static bool mayOverwriteInTypedStore(const ReceiversOfTypedAccess& receivers, bool isDefinition, OptionSet<CellFact> knownOfCell)
{
    return !possibleChangesForbid(receivers.possibleChanges, isDefinition ? TypedAccess::DefinesFilledSlot : TypedAccess::StoresIntoFilledSlot, knownOfCell);
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
            || (layout.has(LayoutFlags::IsConstructorFunction) && !walker.facts().isEvaluatedAtMostOnce(current) && !hasGuardsOfOneEvaluation(walker, current) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
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

bool hasOnlyNamedInheritedMembers(Walker& walker, LayoutID classLayout)
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

static bool mayObjectPrototypePassStore(const Program& program, PropertyKeyID key, LookupAbove lookup)
{
    if (lookup == LookupAbove::OfRead || program.admitsReplacedBuiltins || liesInThread(key) || program.at(key).kind != PropertyKeyKind::Text)
        return false;
    return program.nameClassOf(key) == NameClass::ObjectPrototypeMember;
}

bool hasGuardsOfOneEvaluation(const Walker& walker, LayoutID classLayout)
{
    const Program& program = walker.program();
    if (!walker.facts().isEvaluatedOncePerInstance(classLayout) || !walker.plan().hasClassRecord(classLayout))
        return false;
    FunctionID constructor = program.at(classLayout).constructor;
    if (!constructor || !(isClassConstructorOf(program.at(constructor).kind) || walker.plan().hasClosureCell(constructor)))
        return false;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID above = writtenBaseOf(program.at(classLayout)); above && numberOfClassesLeft--; above = writtenBaseOf(program.at(above))) {
        if (FunctionID function = program.at(above).constructor; function && isLibraryCode(program, program.at(function)))
            return true;
    }
    return false;
}

static WhoAnswersForWhatStandsAbove findPrototypeAnswerForObjectsOf(const Walker& walker, LayoutID member, PropertyKeyID key, LookupAbove lookup, LayoutID& classLayout, ASCIILiteral* whyNobody)
{
    auto nobody = [&](ASCIILiteral why) {
        if (whyNobody)
            *whyNobody = why;
        return WhoAnswersForWhatStandsAbove::Nobody;
    };
    const Program& program = walker.program();
    if (objectPrototypeMayGainNameUnder(walker, key))
        return nobody("a test run may lay the name on Object.prototype"_s);
    if (program.at(member).kind == LayoutKind::Record) {
        if (nothingAnswersAboveObjectsOfForTypedAccess(program, linkTablesForTypedAccesses(walker), member, key))
            return WhoAnswersForWhatStandsAbove::NothingStandsThere;
        if (!mayObjectPrototypePassStore(program, key, lookup) || !nothingAnswersBelowObjectPrototypeForTypedAccess(program, linkTablesForTypedAccesses(walker), member, key))
            return nobody("something above a literal's object may answer"_s);
        classLayout = member;
        return WhoAnswersForWhatStandsAbove::GuardOfClass;
    }

    const LinkTables& link = linkTablesForTypedAccesses(walker);
    classLayout = declaredLayoutOf(program, member);
    if (isOnObjectPrototype(program.nameClassOf(key)) && !mayObjectPrototypePassStore(program, key, lookup))
        return nobody("Object.prototype has the name"_s);
    if (program.at(member).has(LayoutFlags::IsOfCreatedObject)) {
        if (!walker.plan().hasClassRecord(classLayout))
            return nobody("the layout is of objects made by Object.create, and it has no record"_s);
        if (walker.facts().hasMemberUnder(classLayout, ClassSide::Prototype, key) || walker.facts().hasMemberUnder(classLayout, ClassSide::Static, key))
            return nobody("something that is known to stand above the created object has the name"_s);
        return WhoAnswersForWhatStandsAbove::GuardOfClass;
    }
    auto mayHaveOtherEvaluation = [&](LayoutID layout) {
        return !walker.facts().isEvaluatedAtMostOnce(layout) && !hasGuardsOfOneEvaluation(walker, layout);
    };
    if (mayHaveOtherEvaluation(classLayout) && (lookup != LookupAbove::OfStoreIntoCellMadeHere || !firstFunctionObjectIsClassForTypedAccess(program, link, member, classLayout)))
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
        if (layout.has(LayoutFlags::IsConstructorFunction) && mayHaveOtherEvaluation(current) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
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

static WhoAnswersForWhatStandsAbove whoAnswersForWhatStandsAboveObjectsOf(const Walker& walker, LayoutID member, PropertyKeyID key, LookupAbove lookup, LayoutID& classLayout, ASCIILiteral* whyNobody = nullptr)
{
    if (whyNobody) [[unlikely]]
        return findPrototypeAnswerForObjectsOf(walker, member, key, lookup, classLayout, whyNobody);
    constexpr uint64_t multiplier = 0x9e3779b97f4a7c15ull;
    uint64_t question = (static_cast<uint64_t>(member.index()) << 32 | key.bits()) * 3 + static_cast<uint64_t>(lookup);
    Vector<KeptByTypedAccesses::AnswerAboveObjects>& answers = keptByTypedAccesses(const_cast<Walker&>(walker)).answersAboveObjects;
    if (answers.isEmpty()) [[unlikely]]
        answers.grow(1u << KeptByTypedAccesses::prototypeAnswerLog);
    size_t place = (question * multiplier) >> (64 - KeptByTypedAccesses::prototypeAnswerLog);
    if (answers[place].question != question) {
        LayoutID found;
        WhoAnswersForWhatStandsAbove who = findPrototypeAnswerForObjectsOf(walker, member, key, lookup, found, nullptr);
        answers[place] = { question, found, static_cast<uint8_t>(who) };
    }
    if (LayoutID found = answers[place].classLayout)
        classLayout = found;
    return static_cast<WhoAnswersForWhatStandsAbove>(answers[place].who);
}

static bool storePassesPrimitivesAbove(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    if (receivers.possibleChanges.contains(LayoutChange::IsFrozen))
        return false;
    const Program& program = walker.program();
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    return std::ranges::all_of(receivers.members, [&](LayoutID member) {
        bool mayStopStores = false;
        return typeOfPrimitivesAboveObjectsOf(program, link, const_cast<Walker&>(walker).types(), member, key, &mayStopStores) && !mayStopStores;
    });
}

static FunctionID methodUnderAppendedField(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    if (!findsMemberWithoutCodeForTypedAccess(walker.program(), link, key) || possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove))
        return { };
    FunctionID method;
    for (LayoutID member : receivers.members) {
        auto under = link.methodsUnderFields.find(LinkTables::layoutAndName(member, key));
        if (under == link.methodsUnderFields.end() || (method && method != under->value))
            return { };
        method = under->value;
    }
    return method;
}

static bool storePassesMethodAbove(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    if (!key || liesInThread(key) || !linkTablesForTypedAccesses(walker).keysOfFieldsOverMethods.get(key.index()) || receivers.possibleChanges.contains(LayoutChange::IsFrozen))
        return false;
    return !!methodUnderAppendedField(walker, receivers, key);
}

static bool storePassesAboveWithoutGuards(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    return nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key) || storePassesPrimitivesAbove(walker, receivers, key) || storePassesMethodAbove(walker, receivers, key);
}

static FunctionID findMethodForEmptyField(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    if (link.methodsUnderFields.isEmpty() || !key || liesInThread(key) || !link.keysOfFieldsOverMethods.get(key.index()) || !walker.has(Mechanism::Folding) || slotHoldsMarkForTypedAccess(walker, receivers, key))
        return { };
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || !field.isMovedOutByNoStore || possibleChangesForbid(changesThatMayReachFieldForTypedAccess(receivers, field), TypedAccess::ReadsSlotThatMayBeEmpty))
        return { };
    FunctionID method = methodUnderAppendedField(walker, receivers, key);
    return method && closureTableHoldsMethodForTypedAccess(walker, method) ? method : FunctionID();
}

static bool shouldReadMethodUnderField(const Walker& walker)
{
    return walker.isInLoop() || walker.facts().linked().isCalledInLoops(walker.currentFunction());
}

static TypedForm buildFieldOverMethodRead(Walker& walker, const BodyValue& value, Reference object, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, FunctionID method)
{
    Origin origin = walker.originOf(value);
    origin.isTailCall = false;
    if (orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, receivers, key) || isSlotKnownEmpty(walker, object, field)) {
        walker.count(StatisticID::MethodsReadUnderEmptyFields);
        return bindClosureThatClosureTableHoldsForTypedAccess(walker, value, method, origin);
    }
    if (!shouldReadMethodUnderField(walker))
        return TypedForm::None;
    if (callOwnOrMethodByArms(walker, value))
        return TypedForm::Built;
    Reference isFilled = walker.build(draftOfSlotOperationForTypedAccess(walker, value, Opcode::IsSlotFilled, object, receivers, key));
    if (!isFilled || walker.hasFailed() || walker.isUnreachable())
        return TypedForm::Built;
    BlockID filled = walker.addBlock();
    BlockID empty = walker.addBlock();
    walker.branch(isFilled, filled, empty);
    Vector<Walker::JoinArm, 2> arms;
    walker.continueIn(empty);
    if (!walker.isUnreachable()) {
        TypedForm form = bindClosureThatClosureTableHoldsForTypedAccess(walker, value, method, origin);
        if (Reference closure = form == TypedForm::Built ? walker.referenceBoundTo(value.id()) : Reference(); closure && !walker.isUnreachable())
            arms.append({ walker.currentBlock(), { closure } });
        walker.count(StatisticID::MethodsReadUnderEmptyFields);
    }
    walker.continueIn(filled);
    if (Reference held = walker.isUnreachable() ? Reference() : walker.build(draftOfSlotOperationForTypedAccess(walker, value, Opcode::GetSlot, object, receivers, key)))
        arms.append({ walker.currentBlock(), { held } });
    std::array<Reference, 1> met;
    if (walker.hasFailed() || !walker.joinArms(arms.span(), walker.addBlock(), met))
        return TypedForm::Built;
    walker.bind(value.id(), met[0]);
    return TypedForm::Built;
}

bool testsOnlyFieldOverMethod(Walker& walker, const BodyValue& access)
{
    if (access.isFreeName() || !access.key() || liesInThread(access.key()) || !walker.facts().linked().keysOfFieldsOverMethods.get(access.key().index()))
        return false;
    Reference object = walker.referenceOf(access.objectOperand());
    std::optional<ReceiversOfTypedAccess> receivers = object && object.isValue() && !walker.isUnreachable() ? findReceiversOfTypedAccess(walker, walker.typeOf(object)) : std::nullopt;
    if (!receivers)
        return false;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, access.key(), { }, true);
    if (!findMethodForEmptyField(walker, *receivers, field, access.key()))
        return false;
    if (access.opcode() == Opcode::PropGet && shouldReadMethodUnderField(walker))
        return true;
    return orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, access.key()) || isSlotKnownEmpty(walker, object, field) || provesSlotFilledForTypedAccess(walker, valueInSlotForTypedAccess(walker, object, field), access.key());
}

static bool lookupPassesAboveReceiversForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, LookupAbove lookup, const WalkState* stateAskedAboutGuards)
{
    if (field.isNamedAbove || (field.mayBeDefinedAbove && walker.expectsNamesDefinedAbove()))
        return false;
    if (!receivers.possibleChanges.contains(LayoutChange::ChangesPrototype)) {
        if (nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key) || (lookup != LookupAbove::OfRead && (storePassesPrimitivesAbove(walker, receivers, key) || storePassesMethodAbove(walker, receivers, key))))
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
    size_t numberOfGuardedReceivers = 0;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout;
        if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, stores ? LookupAbove::OfStore : LookupAbove::OfRead, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass)
            continue;
        if (!asksWhatReadLeansOn && !(stores ? walker.state().storesPassAbove(classLayout, key) : walker.state().guardOfReadsHolds(classLayout, key)))
            continue;
        ++numberOfGuardedReceivers;
        MemberConditionRecord condition { stores ? MemberConditionKind::MemberPassedByStores : MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
        rememberedListed.appendUnlessListed(remembered, condition);
    }
    if (remembered.isEmpty())
        return;
    if (remembered.size() == 1) {
        LayoutID classLayout = remembered[0].layout;
        bool isConstructorFunction = walker.program().at(classLayout).has(LayoutFlags::IsConstructorFunction);
        bool mayBelongToOtherClass = isConstructorFunction ? receivers.members.size() > 1 : numberOfGuardedReceivers < receivers.members.size();
        if (mayBelongToOtherClass && !walker.state().guardsHaveBegun(classLayout))
            return;
    }
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
    return !field.isNamedAbove && !receivers.possibleChanges.contains(LayoutChange::ChangesPrototype) && !storePassesAboveWithoutGuards(walker, receivers, key);
}

static bool mayCreateInTypedStore(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, bool isDefinition, OptionSet<CellFact> knownOfCell, const WalkState* stateAskedAboutGuards, LookupAbove lookup = LookupAbove::OfStore, bool objectPassesStores = false)
{
    bool emptySlotSaysThatPropertyIsMissing = field.isMovedOutByNoStore || knownOfCell.contains(CellFact::NoFieldOutsideSlot);
    if (!emptySlotSaysThatPropertyIsMissing || possibleChangesForbid(receivers.possibleChanges, isDefinition ? TypedAccess::DefinesSlotThatMayBeEmpty : TypedAccess::StoresIntoSlotThatMayBeEmpty, knownOfCell))
        return false;
    return isDefinition || objectPassesStores || lookupPassesAboveReceiversForTypedAccess(walker, receivers, field, key, lookup, stateAskedAboutGuards);
}

static bool mayFindSlotEmptyBehindLooksInTypedRead(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, OptionSet<CellFact> knownOfCell, const WalkState* stateAskedAboutGuards)
{
    bool emptySlotSaysThatPropertyIsMissing = field.isMovedOutByNoStore || knownOfCell.contains(CellFact::NoFieldOutsideSlot);
    if (!emptySlotSaysThatPropertyIsMissing || possibleChangesForbid(changesThatMayReachFieldForTypedAccess(receivers, field), TypedAccess::ReadsSlotThatMayBeEmpty, knownOfCell))
        return false;
    return lookupPassesAboveReceiversForTypedAccess(walker, receivers, field, key, LookupAbove::OfRead, stateAskedAboutGuards);
}

static bool readNeedsGuards(const ReceiversOfTypedAccess& receivers, OptionSet<CellFact> askedOfCell)
{
    return askedOfCell.contains(CellFact::HasLayoutPrototype) || receivers.possibleChanges.contains(LayoutChange::ChangesPrototype);
}

static bool storeNeedsGuards(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, OptionSet<CellFact> askedOfCell)
{
    return readNeedsGuards(receivers, askedOfCell) || storeAsksGuardsWherePrototypeStaysForTypedAccess(walker, receivers, field, key);
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

static std::optional<OptionSet<CellFact>> queryObjectPrototypeRead(const ReceiversOfTypedAccess& receivers)
{
    return smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return !changesForbidObjectPrototypeRead(receivers, facts);
    });
}

static TypedForm buildObjectPrototypeRead(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    if (value.opcode() != Opcode::PropGet || value.isFreeName())
        return TypedForm::None;
    BuiltinID location = findObjectPrototypeMember(walker, receivers, field, key);
    std::optional<OptionSet<CellFact>> asked = location ? queryObjectPrototypeRead(receivers) : std::nullopt;
    if (!asked)
        return TypedForm::None;
    if (operands.areOnlyTypes) {
        TypedForm form = buildObjectPrototypeMemberRead(walker, value, operands.typeOfObject, { }, location);
        if (form == TypedForm::None || asked->isEmpty())
            return form;
        return walker.isInLoop() || walker.weighsWhatIsWritten() ? TypedForm::BuiltBehindLookAtCell : TypedForm::None;
    }
    if (!knownCellFacts(walker, operands.object).containsAll(*asked))
        return TypedForm::None;
    walker.checkFactsRememberedOfCell(operands.object, walker.originOf(value), *asked);
    return buildObjectPrototypeMemberRead(walker, value, operands.typeOfObject, operands.object, location);
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
    for (OptionSet<CellFact> facts : factsThatLooksAtCellShow)
        everyFact.add(facts);
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

static bool closureTableHoldsMethodForTypedAccess(const Walker&, FunctionID method);

static bool statementsLayNameOnPrototypeForTypedAccess(const Vector<LinkedNameOnPrototype>& namesOnPrototypes, LayoutID layout, PropertyKeyID key)
{
    return std::ranges::binary_search(namesOnPrototypes, std::pair { layout, key }, { }, [](const LinkedNameOnPrototype& name) {
        return std::pair { name.layout, name.key };
    });
}

static AssignedMember findChosenAssignedMember(const Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    const LinkTables& held = walker.facts().held();
    AssignedMember member = findAssignedMember(walker.program(), walker.facts(), classLayout, key);
    if (!member || !walker.plan().hasClassRecord(classLayout) || classLayout.index() >= held.layouts.size() || held.layout(classLayout).isFilledUnderComputedKeys || held.layout(classLayout).hasSeveralHeritages)
        return { };
    if (member.isDeclared && !walker.state().isMemberAssigned(classLayout, key)) {
        bool isChosen = walker.assignedMemberChoice() == AssignedMemberChoice::Always || (walker.assignedMemberChoice() == AssignedMemberChoice::WhenUnconditional && member.isUnconditional);
        if (!isChosen || walker.state().memberAboveIsUnchanged(classLayout, key))
            return { };
    }
    for (FunctionID function : { member.method, member.getter, member.setter }) {
        if (function && !walker.facts().linked().mayBeMentionedBy(walker.unit(), function))
            return { };
    }
    return member;
}

static bool isAssignedValue(const Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    const LinkTables& held = walker.facts().held();
    if (!findAssignedMember(walker.program(), walker.facts(), classLayout, key).holdsValue || classHolderIsMadeWithName(walker.program(), ClassSide::Prototype, key))
        return false;
    return walker.plan().hasClassRecord(classLayout) && classLayout.index() < held.layouts.size() && !held.layout(classLayout).isFilledUnderComputedKeys && !held.layout(classLayout).hasSeveralHeritages;
}

static ALWAYS_INLINE bool nameMayBeAssigned(const Walker& walker, PropertyKeyID key)
{
    return key && !liesInThread(key) && walker.facts().tables().assignedMemberKeys.get(key.index());
}

static ALWAYS_INLINE FunctionID findChosenAssignedMethod(const Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    return nameMayBeAssigned(walker, key) ? findChosenAssignedMember(walker, classLayout, key).method : FunctionID();
}

static ALWAYS_INLINE AssignedMember findChosenAssignedAccessor(const Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    if (!nameMayBeAssigned(walker, key))
        return { };
    AssignedMember member = findChosenAssignedMember(walker, classLayout, key);
    return member.method ? AssignedMember() : member;
}

static FunctionID findMethodInRow(const Walker&, LayoutID classLayout, PropertyKeyID, ASCIILiteral* whyNone);

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
        if (layout.kind != LayoutKind::Instance || !layout.constructor || layout.has(LayoutFlags::IsOfCreatedObject) || (layout.has(LayoutFlags::IsConstructorFunction) && !walker.facts().isEvaluatedAtMostOnce(classLayout) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor)) || !walker.plan().hasClassRecord(classLayout) || !walker.facts().isEvaluatedAtMostOnce(classLayout)
            || walker.facts().hasMemberUnder(classLayout, ClassSide::Prototype, { }) || walker.facts().hasMemberUnder(classLayout, ClassSide::Prototype, key) || statementsLayNameOnPrototypeForTypedAccess(namesOnPrototypes, classLayout, key))
            return none("no field, and every prototype object is made with the name"_s);
        return layout.constructor;
    }
    if (FunctionID assigned = findChosenAssignedMethod(walker, classLayout, key))
        return assigned;
    if (whyNone)
        return findMethodInRow(walker, classLayout, key, whyNone);
    auto& kept = keptByTypedAccesses(const_cast<Walker&>(walker)).methodsInRows;
    uint64_t place = static_cast<uint64_t>(classLayout.index()) << 32 | key.bits();
    if (auto known = kept.find(place); known != kept.end())
        return known->value;
    FunctionID method = findMethodInRow(walker, classLayout, key, nullptr);
    kept.add(place, method);
    return method;
}

static FunctionID findMethodInRow(const Walker& walker, LayoutID classLayout, PropertyKeyID key, ASCIILiteral* whyNone)
{
    auto none = [&](ASCIILiteral why) {
        if (whyNone)
            *whyNone = why;
        return FunctionID();
    };
    const Program& program = walker.program();
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = classLayout; current;) {
        const Layout& layout = program.at(current);
        bool isOfCreatedObjects = layout.has(LayoutFlags::IsOfCreatedObject);
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || (!layout.constructor && !isOfCreatedObjects) || !walker.plan().hasClassRecord(current))
            return none("no field, and a class on the way up has no record"_s);
        if (!isOfCreatedObjects && !walker.facts().isEvaluatedAtMostOnce(current))
            return none("no field, and a class on the way up may be evaluated again"_s);
        if (layout.has(LayoutFlags::IsConstructorFunction) && !walker.facts().isEvaluatedAtMostOnce(current) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
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
            if (const LinkTables& held = walker.facts().held(); classLayout.index() < held.layouts.size() && held.layout(classLayout).hasSeveralHeritages)
                return none("no field, and a class on the way up extends several classes"_s);
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

FunctionID methodDeclaredAboveObjects(const Walker& walker, LayoutID layout, PropertyKeyID key)
{
    LayoutID classLayout;
    FunctionID method = findMethodDeclaredAboveObjectsOf(walker, layout, key, classLayout);
    return method && closureTableHoldsMethodForTypedAccess(walker, method) ? method : FunctionID();
}

static bool cellSaysWhetherObjectHidesMethodForTypedAccess(const Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    if (findChosenAssignedMethod(walker, classLayout, key))
        return true;
    const LinkTables& held = walker.facts().held();
    if (classLayout.index() >= held.layouts.size() || held.layout(classLayout).isFilledUnderComputedKeys)
        return false;
    if (held.layout(classLayout).instancesAreLaidAsPrototypes)
        return true;
    std::span<const LinkedNameBesideFields> names = held.namesStoredBesideFieldsOf(classLayout);
    if (!std::ranges::binary_search(names, key, { }, &LinkedNameBesideFields::key))
        return false;
    return std::ranges::all_of(names, [&](const LinkedNameBesideFields& name) {
        LayoutID classOfObjects;
        return !!findMethodDeclaredAboveObjectsOf(walker, classLayout, name.key, classOfObjects);
    });
}

static MemberConditionRecord conditionOnMethodAboveObjectsOf(const Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    if (findChosenAssignedMethod(walker, classLayout, key))
        return { MemberConditionKind::MemberAssigned, classLayout, ClassSide::Prototype, key };
    bool cellSaysWhetherObjectHides = cellSaysWhetherObjectHidesMethodForTypedAccess(walker, classLayout, key);
    return { cellSaysWhetherObjectHides ? MemberConditionKind::MemberUnchangedAbove : MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
}

static bool conditionOnMethodAboveObjectsIsKnownToHold(const Walker& walker, const WalkState& state, LayoutID classLayout, PropertyKeyID key)
{
    if (walker.shouldAssumeGuardsAndFacts())
        return true;
    if (findChosenAssignedMethod(walker, classLayout, key))
        return state.isMemberAssigned(classLayout, key);
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

bool instancesShadowMethod(const Walker& walker, TypeID typeOfObjects, PropertyKeyID key)
{
    const LinkTables& held = walker.facts().held();
    const Type& objects = walker.types().at(typeOfObjects);
    if (!held.keysStoredOutsideFields.get(key.bits()) || !objects.layouts)
        return false;
    return std::ranges::any_of(walker.types().listOf(objects.layouts), [&](LayoutID member) {
        LayoutID classLayout = declaredLayoutOf(walker.program(), member);
        return std::ranges::binary_search(held.namesStoredBesideFieldsOf(classLayout), key, { }, &LinkedNameBesideFields::key) && findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout);
    });
}

static bool rowMayBeEvaluatedAgain(const Walker& walker, LayoutID from)
{
    size_t stepsLeft = walker.program().layouts.size();
    for (LayoutID current = from; current && stepsLeft; --stepsLeft) {
        if (!walker.program().at(current).has(LayoutFlags::IsOfCreatedObject) && !walker.facts().isEvaluatedAtMostOnce(current))
            return true;
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return true;
        current = parent;
    }
    return false;
}

static FunctionID findStaticAfterEndedGuard(Walker& walker, FunctionID constructor, PropertyKeyID key, bool& isReplaced)
{
    const LinkTables& held = walker.facts().held();
    LayoutID classObject = classOfClassObject(walker.program(), constructor);
    size_t stepsLeft = walker.program().layouts.size();
    for (LayoutID current = classObject; current && stepsLeft; current = walker.program().at(current).base, --stepsLeft) {
        std::span<const uint32_t> places = held.membersUnder(current, ClassSide::Static, key);
        if (places.empty())
            continue;
        const LinkedMember& listed = held.members[places[0]];
        if (places.size() != 1 || !listed.isReplaced || walker.state().guardOfReadsHolds(classObject, key, ClassSide::Static))
            return { };
        isReplaced = true;
        return listed.replacement && walker.facts().linked().mayBeMentionedBy(walker.unit(), listed.replacement) ? listed.replacement : FunctionID();
    }
    return { };
}

bool isAccessorReplaced(Walker& walker, TypeID typeOfObjects, PropertyKeyID key)
{
    const LinkTables& held = walker.facts().held();
    if ((!held.someAccessorIsReplaced && held.namesOnPrototypes.isEmpty()) || !held.keysOfMembers[static_cast<size_t>(ClassSide::Prototype)].get(key.index()))
        return false;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObjects);
    if (!receivers)
        return false;
    return std::ranges::any_of(receivers->members, [&](LayoutID member) {
        size_t stepsLeft = walker.program().layouts.size();
        for (LayoutID current = declaredLayoutOf(walker.program(), member); current && stepsLeft; --stepsLeft) {
            std::span<const uint32_t> places = held.membersUnder(current, ClassSide::Prototype, key);
            if (!places.empty()) {
                return std::ranges::any_of(places, [&](uint32_t place) {
                    return held.members[place].kind != MethodKind::Method && (held.members[place].isReplaced || statementsLayNameOnPrototypeForTypedAccess(held.namesOnPrototypes, current, key));
                });
            }
            LayoutID parent;
            if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
                return false;
            current = parent;
        }
        return false;
    });
}

FunctionID findMethodAfterEndedGuard(Walker& walker, TypeID typeOfObjects, PropertyKeyID key, bool& isReplaced, FunctionID* written)
{
    Type subject = walker.types().at(typeOfObjects);
    if (FunctionID constructor = singleFunctionOf(walker.types(), subject))
        return findStaticAfterEndedGuard(walker, constructor, key, isReplaced);
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObjects);
    if (!receivers)
        return { };
    const LinkTables& held = walker.facts().held();
    FunctionID foundByAll;
    for (LayoutID member : receivers->members) {
        LayoutID classLayout;
        FunctionID declared = findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout);
        if (!declared || conditionOnMethodAboveObjectsIsKnownToHold(walker, walker.state(), classLayout, key))
            return { };
        FunctionID found;
        bool mayBePutBack = false;
        size_t stepsLeft = walker.program().layouts.size();
        for (LayoutID current = classLayout; current && stepsLeft; --stepsLeft) {
            std::span<const uint32_t> places = held.membersUnder(current, ClassSide::Prototype, key);
            if (!places.empty()) {
                const LinkedMember& listed = held.members[places[0]];
                if (places.size() == 1 && listed.written == declared) {
                    found = listed.replacement;
                    isReplaced |= listed.isReplaced;
                    mayBePutBack = listed.isAssignedToClass;
                }
                break;
            }
            LayoutID parent;
            if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
                break;
            current = parent;
        }
        if (!found && rowMayBeEvaluatedAgain(walker, classLayout))
            found = declared;
        isReplaced |= !!found;
        if (!found || (foundByAll && found != foundByAll) || !walker.facts().linked().mayBeMentionedBy(walker.unit(), found))
            return { };
        *written = mayBePutBack && (!foundByAll || *written == declared) ? declared : FunctionID();
        foundByAll = found;
    }
    return foundByAll;
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

bool isNameOutsideFields(Walker& walker, TypeID typeOfObjects, PropertyKeyID key)
{
    const Program& program = walker.program();
    const Type& type = walker.types().at(walker.types().withoutNullish(typeOfObjects));
    if (!isLookedUpAsNameForTypedAccess(program, key) || !type.hasOnlyLayouts() || type.primitives)
        return false;
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    size_t numberOfLayouts = program.layouts.size();
    return std::ranges::all_of(membersOf(walker.types(), type.layouts), [&](LayoutID member) {
        if (builtinClassOfInstances(program.at(member)) || findField(program, member, key) || std::ranges::contains(link.appendedKeysOf(member), key) || !possibleChangesForbid(possibleChangesForTypedAccess(link, member), TypedAccess::FindsNoProperty))
            return false;
        size_t stepsLeft = numberOfLayouts;
        for (LayoutID current = declaredLayoutOf(program, member); current && stepsLeft--; current = program.at(current).base) {
            if (walker.facts().hasMemberUnder(current, ClassSide::Prototype, key))
                return false;
        }
        Vector<FunctionID, 4> laid;
        return !link.findFunctionsLaidAbove(program, member, key, laid) || laid.isEmpty();
    });
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

ValueID findForwardedCall(const Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (read.opcode() != Opcode::PropGet || index.useCount(read.id()) != 2)
        return { };
    std::optional<BodyUser> user = index.firstUser(read.id());
    if (!user || !user->value || index.opcodeOf(user->value) != Opcode::PropGet)
        return { };
    BodyValue forwarder = index.value(user->value);
    bool namesForwarding = forwarder.key() == walker.program().keyOf(BuiltinNames::call) || forwarder.key() == walker.program().keyOf(BuiltinNames::apply);
    if (!namesForwarding || forwarder.isFreeName() || forwarder.objectOperand() != read.id())
        return { };
    ValueID call = index.onlyUsingValue(forwarder.id());
    if (!call || index.opcodeOf(call) != Opcode::Call || index.value(call).calleeOperand() != forwarder.id())
        return { };
    return index.value(call).as<Opcode::Call>().thisValueOperand() == read.id() ? call : ValueID();
}

static bool closureTableHoldsMethodForTypedAccess(const Walker& walker, FunctionID method)
{
    const Plan& plan = walker.plan();
    if (LayoutID constructed = walker.program().at(method).constructedClass; constructed && !walker.facts().isEvaluatedAtMostOnce(constructed))
        return false;
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
    if (!walker.isInLoop() && walker.unit().role == CodeRole::CoroutineBody && walker.m_generatorReadTested != read.id()) {
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
    if (method && user.opcode() == Opcode::CallList && !listSpreadBehindArgumentsIsRestListOf(const_cast<Walker&>(walker), user, method) && !hasKnownSpreadElements(const_cast<Walker&>(walker), user, read.id())) {
        if (whyNone)
            *whyNone = "no field: a method above, called with a list that is laid out"_s;
        return { };
    }
    return method;
}

void Walker::checkPassedStores(Reference object, Origin origin)
{
    if (!checksProofs() || isUnreachable() || !object || !object.isValue())
        return;
    Draft test(Opcode::PassesStoresAbove, origin);
    test.setObjectOperand(object);
    Reference holds = buildTestOfCheck(WTF::move(test));
    if (holds && holds.isValue() && !isUnreachable())
        checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
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
    if (knowsWord || remembered.containsAny(factsOfExtensibleObject)) {
        Draft test(Opcode::IsNotFrozen, origin);
        test.setObjectOperand(object);
        test.setIsExtensible(knowsWord || remembered.contains(CellFact::IsExtensible));
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
    bool isReadByLook = readByLook && readByLook.isValue() && (walker.code().at(readByLook.id()).opcode == Opcode::PropGet || walker.code().at(readByLook.id()).opcode == Opcode::GetAssignedMember);
    Reference closureReadByLook = isReadByLook ? state.checkedNameOf(readByLook) : Reference();
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

std::optional<AssignedMemberChoice> findOtherMemberChoice(Walker& walker, const BodyValue& read, TypeID typeOfObjects, bool isTypeKnown)
{
    if (walker.assignedMemberChoice() != AssignedMemberChoice::WhenUnconditional || !nameMayBeAssigned(walker, read.key()))
        return std::nullopt;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObjects);
    if (!receivers)
        return std::nullopt;
    bool isUnconditional = false;
    for (LayoutID member : receivers->members) {
        AssignedMember assigned = findAssignedMember(walker.program(), walker.facts(), declaredLayoutOf(walker.program(), member), read.key());
        if (!assigned.isDeclared)
            return std::nullopt;
        isUnconditional |= assigned.isUnconditional;
    }
    AssignedMemberChoice choice = isUnconditional ? AssignedMemberChoice::Never : AssignedMemberChoice::Always;
    SetForScope chosen(walker.assignedMemberChoice(), choice);
    FunctionID method = findMethodReadBehindLooksForTypedAccess(walker, read, *receivers, read.key());
    if (!method || (isTypeKnown && !closureTableHoldsMethodForTypedAccess(walker, method)))
        return std::nullopt;
    Vector<MemberConditionRecord, 4> conditions;
    bool someAreOpen = findGuardsOfMethodAboveObjectsOf(walker, typeOfObjects, read.key(), conditions) && std::ranges::any_of(conditions, [&](const MemberConditionRecord& condition) {
        return !conditionOnMethodAboveObjectsIsKnownToHold(walker, walker.state(), condition.layout, condition.key);
    });
    return someAreOpen ? std::optional(choice) : std::nullopt;
}

static bool findAccessorDeclaredAboveObjectsOf(const Walker& walker, LayoutID member, PropertyKeyID key, LayoutID& classLayout, FunctionID& getter, FunctionID& setter)
{
    const Program& program = walker.program();
    if (program.at(member).kind != LayoutKind::Instance || !findsMemberWithoutCodeForTypedAccess(program, linkTablesForTypedAccesses(walker), key) || classHolderIsMadeWithName(program, ClassSide::Prototype, key))
        return false;
    classLayout = declaredLayoutOf(program, member);
    if (AssignedMember assigned = findChosenAssignedAccessor(walker, classLayout, key)) {
        getter = assigned.getter;
        setter = assigned.setter;
        return true;
    }
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = classLayout; current;) {
        const Layout& layout = program.at(current);
        bool isOfCreatedObjects = layout.has(LayoutFlags::IsOfCreatedObject);
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || (!layout.constructor && !isOfCreatedObjects) || !walker.plan().hasClassRecord(current) || (!isOfCreatedObjects && !walker.facts().isEvaluatedAtMostOnce(current)))
            return false;
        if (layout.has(LayoutFlags::IsConstructorFunction) && !walker.facts().isEvaluatedAtMostOnce(current) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
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

static std::optional<MemberConditionRecord> findAssignedGetterCondition(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    LayoutID only;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout = declaredLayoutOf(walker.program(), member);
        if ((only && classLayout != only) || !findChosenAssignedAccessor(walker, classLayout, key).getter)
            return std::nullopt;
        only = classLayout;
    }
    return only ? std::optional(MemberConditionRecord { MemberConditionKind::MemberAssigned, only, ClassSide::Prototype, key }) : std::nullopt;
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
    if (halfOfAll && !closureTableHoldsMethodForTypedAccess(walker, halfOfAll) && (isStore || !walker.isInLoop() || !findAssignedGetterCondition(walker, receivers, key)))
        return { };
    return halfOfAll && accessorAdmitsDirectCall(walker, halfOfAll) && walker.facts().linked().mayBeMentionedBy(walker.unit(), halfOfAll) ? halfOfAll : FunctionID();
}

static std::optional<OptionSet<CellFact>> findAccessorCellFacts(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    bool isAssigned = std::ranges::any_of(receivers.members, [&](LayoutID member) {
        return !!findChosenAssignedAccessor(walker, declaredLayoutOf(walker.program(), member), key);
    });
    return smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return !possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove, facts, true) && (!isAssigned || facts.contains(CellFact::NoExtraProperties));
    });
}

static MemberConditionRecord accessorCondition(const Walker& walker, LayoutID classLayout, PropertyKeyID key)
{
    return { findChosenAssignedAccessor(walker, classLayout, key) ? MemberConditionKind::MemberAssigned : MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
}

static bool accessorConditionHolds(const WalkState& state, const MemberConditionRecord& condition)
{
    return condition.kind == MemberConditionKind::MemberAssigned ? state.isMemberAssigned(condition.layout, condition.key) : state.guardOfReadsHolds(condition.layout, condition.key);
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
    if (std::optional<OptionSet<CellFact>> asked = findAccessorCellFacts(walker, receivers, key); !asked || !knownCellFacts(walker, operands.object).containsAll(*asked))
        return TypedForm::None;
    Reference closureOfHalf;
    if (!closureTableHoldsMethodForTypedAccess(walker, half)) {
        std::optional<MemberConditionRecord> condition = findAssignedGetterCondition(walker, receivers, key);
        if (!condition || !state.isMemberAssigned(condition->layout, key))
            return TypedForm::None;
        std::optional<uint64_t> known = findKnown(walker, KnownNote::AssignedMemberClosure, state.assignedMemberTest(condition->layout, key));
        closureOfHalf = known ? Reference::fromBits(static_cast<uint32_t>(*known)) : Reference();
        if (!closureOfHalf || singleFunctionOf(walker.types(), walker.types().at(walker.typeOf(closureOfHalf))) != half)
            return TypedForm::None;
    }
    Vector<MemberConditionRecord, 8> remembered;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout;
        FunctionID getter;
        FunctionID setter;
        if (!findAccessorDeclaredAboveObjectsOf(walker, member, key, classLayout, getter, setter))
            return TypedForm::None;
        MemberConditionRecord condition = accessorCondition(walker, classLayout, key);
        if (!walker.shouldAssumeGuardsAndFacts() && !accessorConditionHolds(state, condition))
            return TypedForm::None;
        remembered.append(condition);
    }
    if (walker.checksProofs()) {
        Origin origin = walker.originOf(value);
        walker.checkFactsRememberedOfCell(operands.object, origin, findAccessorCellFacts(walker, receivers, key).value_or(OptionSet<CellFact>()));
        checkGuardsThatAreRemembered(walker, remembered.span(), origin);
    }
    if (callAccessor(walker, value, half, operands.object, value.opcode() == Opcode::PropSet ? operands.stored : Reference(), closureOfHalf) || walker.hasFailed())
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
    FunctionID getter { };
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
            bool isGetter = found->kind == MethodKind::Getter;
            if ((found->kind != MethodKind::Method && !isGetter) || !found->function || !closureTableHoldsMethodForTypedAccess(walker, found->function))
                return nothing("a constructor function, and what the record expects under the name is no function that the closure table holds"_s);
            if (!walker.facts().linked().mayBeMentionedBy(walker.unit(), found->function))
                return nothing("a constructor function, and the unit may not mention the function that the record expects under the name"_s);
            if (!isGetter)
                return FoundOffConstructorFunction { classLayout, found->function };
            if (!accessorAdmitsDirectCall(walker, found->function))
                return nothing("a constructor function, and a getter that no call enters directly"_s);
            return FoundOffConstructorFunction { classLayout, { }, found->function };
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
    if (found && !found->getter && !isReadForCallAloneInTypedAccess(walker, read) && !(found->method && findForwardedCall(walker, read))) {
        if (whyNothing)
            *whyNothing = "a constructor function, and the read is not there for a call alone"_s;
        return std::nullopt;
    }
    return found;
}

std::optional<MemberConditionRecord> findStaticMethodGuard(Walker& walker, FunctionID function, PropertyKeyID key)
{
    LayoutID classLayout = classOfConstructorFunction(walker.program(), function);
    std::optional<FoundOffConstructorFunction> found = classLayout ? findWhatRowOfRecordsExpectsOffConstructorFunction(walker, classLayout, key, nullptr) : std::nullopt;
    if (!found || !found->method || walker.state().guardOfReadsHolds(classLayout, key, ClassSide::Static))
        return std::nullopt;
    return MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Static, key };
}

std::optional<MemberConditionRecord> findGuardOfCallOffConstructorFunction(Walker& walker, FunctionID function)
{
    LayoutID classLayout = classOfConstructorFunction(walker.program(), function);
    PropertyKeyID key = walker.program().keyOf(BuiltinNames::call);
    std::optional<FoundOffConstructorFunction> found = classLayout ? findWhatRowOfRecordsExpectsOffConstructorFunction(walker, classLayout, key, nullptr) : std::nullopt;
    if (!found || found->method || found->getter)
        return std::nullopt;
    return MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Static, key };
}

InitializerMethod findInitializerMethod(Walker& walker, LayoutID layout, PropertyKeyID key)
{
    InitializerMethod found;
    if (!walker.has(Mechanism::DirectCalls))
        return found;
    if (MemberFound proven = findMemberAboveObjectsOf(walker.program(), linkTablesForTypedAccesses(walker), layout, key); proven.kind == MemberFound::Kind::Method) {
        found.function = proven.method;
        return found;
    }
    LayoutID classOfObjects;
    FunctionID declared = findMethodDeclaredAboveObjectsOf(walker, layout, key, classOfObjects);
    if (!declared || conditionOnMethodAboveObjectsIsKnownToHold(walker, walker.state(), classOfObjects, key))
        return found;
    found.guard = conditionOnMethodAboveObjectsOf(walker, classOfObjects, key);
    if (!findField(walker.program(), layout, key))
        found.function = declared;
    return found;
}

static TypedForm buildReadOffConstructorFunctionBehindGuardForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands)
{
    std::optional<FoundOffConstructorFunction> found = findWhatRecordsExpectOffConstructorFunction(walker, value, operands.typeOfObject);
    if (!found)
        return TypedForm::None;
    if (operands.areOnlyTypes && found->getter) {
        walker.noteFormAskedAhead(Opcode::CallDirect, KnowledgeLevel::Proven, found->getter);
        return TypedForm::BuiltBehindLookAtCell;
    }
    if (operands.areOnlyTypes)
        return found->method || buildReadOffFunctionPrototype(walker, value, operands.typeOfObject, { }) != TypedForm::None ? TypedForm::BuiltBehindLookAtCell : TypedForm::None;
    if (!operands.object || !walker.state().guardOfReadsHolds(found->classLayout, value.key(), ClassSide::Static))
        return TypedForm::None;

    Origin origin = walker.originOf(value);
    MemberConditionRecord remembered { MemberConditionKind::MemberUnchanged, found->classLayout, ClassSide::Static, value.key() };
    checkGuardsThatAreRemembered(walker, singleElementSpan(remembered), origin);
    if (found->getter) {
        if (callAccessor(walker, value, found->getter, operands.object, { }) || walker.hasFailed())
            return TypedForm::Built;
        Draft asWritten = walker.draftOf(value);
        walker.chooseConservativeFields(asWritten);
        walker.bind(value.id(), walker.build(WTF::move(asWritten)));
        return TypedForm::Built;
    }
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
        else if (condition.kind == MemberConditionKind::MemberAssigned) {
            state.noteGuardOfReadsHolds(condition.layout, condition.key, test, condition.side, WalkState::AskerOfGuardOfReads::AssignedMember);
            walker.count(StatisticID::AssignedMemberChecks);
        } else if (condition.kind == MemberConditionKind::PrototypeLinkUnchanged)
            state.noteGuardOfReadsHolds(condition.layout, { }, test, condition.side, WalkState::AskerOfGuardOfReads::PrototypeLink);
        else if (condition.kind == MemberConditionKind::MemberPassedByStores) {
            state.noteStoresPassAbove(condition.layout, condition.key, test);
            if (!walker.program().at(condition.layout).has(LayoutFlags::IsOfCreatedObject) || !walker.isAllocatedInTrialIteration(receiver))
                walker.noteGuardOfStoresAskedInTries(condition);
        }
        bool speaksOfReads = condition.kind == MemberConditionKind::MemberUnchanged || condition.kind == MemberConditionKind::MemberUnchangedAbove;
        if (speaksOfReads && (!state.guardsHaveBegun(condition.layout) || walker.turnBeginsGuardsOf(condition.layout))) {
            if (walker.turnMakesObjectAtOrBelow(condition.layout)) {
                walker.noteGuardOfStoresAskedInTries(condition);
                walker.noteClassesOfBirthsAtOrBelowInTries(condition.layout);
            } else if (condition.side == ClassSide::Static && walker.program().at(condition.layout).has(LayoutFlags::IsConstructorFunction))
                walker.noteGuardOfStoresAskedInTries(condition);
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
        if (block.isLoopHead || block.beliefAheadOfEdges != BeliefAheadOfEdges::NoEdgeIsAwaited)
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

static ValueID findReadOffPrototypeObject(const BodyIndex& index, ValueID prototypeObject)
{
    ValueID read(prototypeObject.index() + 1);
    if (index.onlyUsingValue(prototypeObject) == read)
        return read;
    if (index.useCount(prototypeObject) != 2 || !index.isInBlock(read, index.blockOf(prototypeObject)) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).objectOperand() != prototypeObject)
        return { };
    ValueID call = index.onlyUsingValue(read);
    if (!call || index.opcodeOf(call) != Opcode::Call)
        return { };
    auto written = index.value(call).as<Opcode::Call>();
    return written.calleeOperand() == read && written.thisValueOperand() == prototypeObject ? read : ValueID();
}

static bool isClosureNeverObserved(const Walker& walker, const BodyValue& read, FunctionID method)
{
    const Function& callee = walker.program().at(method);
    if (callee.isCoroutine())
        return false;
    for (uint32_t parameter = 0; parameter < callee.numberOfEntryParameters(); ++parameter) {
        if (callee.entryParameterKind(parameter) == EntryParameter::Callee)
            return false;
    }
    const BodyIndex& index = walker.currentBodyIndex();
    if (ValueID call = findForwardedCall(walker, read))
        return linkTablesForTypedAccesses(walker).closuresLackName(method, index.value(index.value(call).calleeOperand()).key());
    ValueID user = index.onlyUsingValue(read.id());
    return user && (index.opcodeOf(user) == Opcode::Call || index.opcodeOf(user) == Opcode::CallList) && isReadForCallAloneInTypedAccess(walker, read);
}

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
    ValueID second = findReadOffPrototypeObject(index, first);
    if (readOfPrototype.isFreeName() || readOfPrototype.key() != prototype || !second || index.opcodeOf(second) != Opcode::PropGet)
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
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    if (isConstructorFunction && walker.facts().isEvaluatedAtMostOnce(classLayout) && classStaysAsWrittenForTypedAccess(link, classLayout) && findsMemberWithoutCodeForTypedAccess(program, link, readOfMethod.key()) && isClosureNeverObserved(walker, readOfMethod, method)) {
        MemberFound laidFirst = link.findMember(program, classLayout, ClassSide::Prototype, readOfMethod.key());
        if (laidFirst.kind == MemberFound::Kind::Method && laidFirst.method == method)
            return found;
    }
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

bool findsMethodReadOffPrototypeOfClass(Walker& walker, const BodyValue& readOfPrototype, TypeID typeOfFunctionObject, bool& guardsHold, bool& needsNoGuards)
{
    std::optional<MethodReadOffPrototypeOfClass> found = findMethodReadOffPrototypeOfClass(walker, readOfPrototype, typeOfFunctionObject);
    if (!found || found->readOfPrototype != readOfPrototype.id())
        return false;
    needsNoGuards = found->conditions.isEmpty();
    guardsHold = needsNoGuards || guardsOfMethodReadOffPrototypeOfClassHold(walker, *found);
    return true;
}

static TypedForm buildReadsOfMethodOffPrototypeOfClassForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands)
{
    if (value.opcode() != Opcode::PropGet || walker.isUnreachable())
        return TypedForm::None;
    bool readsPrototype = value.key() == walker.program().keyOf(BuiltinNames::prototype);
    if (operands.areOnlyTypes && !readsPrototype)
        return TypedForm::None;
    std::optional<MethodReadOffPrototypeOfClass> found = findMethodReadOffPrototypeOfClass(walker, value, readsPrototype ? operands.typeOfObject : TypeID());
    if (!found || (!found->conditions.isEmpty() && (operands.areOnlyTypes || !walker.state().mayHoldGuards() || !guardsOfMethodReadOffPrototypeOfClassHold(walker, *found))))
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return value.id() == found->readOfPrototype ? TypedForm::Built : TypedForm::None;
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
    if (walker.checksProofs() && !found->conditions.isEmpty()) {
        Draft test(Opcode::AreMembersUnchanged, origin);
        test.setConditions(found->conditions.span());
        Reference holds = walker.buildTestOfCheck(WTF::move(test));
        if (holds && holds.isValue() && !walker.isUnreachable())
            walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::RememberedFact, origin);
    }
    return bindClosureThatClosureTableHoldsForTypedAccess(walker, value, found->method, origin);
}

static TypedForm buildClassPrototypeReadForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands)
{
    const Program& program = walker.program();
    if (value.opcode() != Opcode::PropGet || value.isFreeName() || value.key() != program.keyOf(BuiltinNames::prototype) || !operands.typeOfObject)
        return TypedForm::None;
    Type type = walker.types().at(operands.typeOfObject);
    FunctionID constructor = type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
    if (!constructor || !isClassConstructorOf(program.at(constructor).kind))
        return TypedForm::None;
    LayoutID classLayout = classOfClassObject(program, constructor);
    if (!classLayout || program.at(classLayout).kind != LayoutKind::Instance || declaredLayoutOf(program, classLayout) != classLayout || !walker.facts().isEvaluatedAtMostOnce(classLayout))
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return TypedForm::Built;
    if (!operands.object || canGuardMethodOffPrototype(walker, value))
        return TypedForm::None;
    Draft prototypeObject(Opcode::GetClassPrototype, walker.originOf(value));
    walker.chooseConservativeFields(prototypeObject);
    prototypeObject.inputValueGiven = value.id();
    prototypeObject.setClassObjectOperand(operands.object);
    prototypeObject.setLayout(classLayout);
    walker.bind(value.id(), walker.build(WTF::move(prototypeObject)));
    return TypedForm::Built;
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
    bool isProvenAbove = walker.positions().classAbove(classLayout) && !nameMayBeAssigned(walker, lookup.key());
    if (isProvenAbove || !findParentNamedByClassRecordForTypedAccess(walker, classLayout, parent) || !parent)
        return false;
    LayoutID classOfObjects;
    FunctionID method = findMethodDeclaredAboveObjectsOf(walker, parent, lookup.key(), classOfObjects);
    if (!method || !walker.facts().linked().mayBeMentionedBy(walker.unit(), method))
        return false;

    const MemberConditionRecord conditions[] {
        { MemberConditionKind::PrototypeLinkUnchanged, classLayout, ClassSide::Prototype, { } },
        conditionOnMethodAboveObjectsOf(walker, classOfObjects, lookup.key()),
    };
    bool closureTableHoldsMethod = closureTableHoldsMethodForTypedAccess(walker, method);
    if (!closureTableHoldsMethod && conditions[1].kind != MemberConditionKind::MemberAssigned)
        return false;
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
    Reference closureOfMethod;
    if (!closureTableHoldsMethod) {
        Draft member(Opcode::GetAssignedMember, origin);
        walker.chooseConservativeFields(member);
        member.setConditions(singleElementSpan(conditions[1]));
        Reference found = walker.build(WTF::move(member));
        if (!found || !found.isValue())
            return true;
        Draft test(Opcode::IsClosureOf, origin);
        walker.chooseConservativeFields(test);
        test.setFunction(method);
        test.setValueOperand(found);
        Reference isClosureOfMethod = walker.build(WTF::move(test));
        if (!isClosureOfMethod || !isClosureOfMethod.isValue())
            return true;
        BlockID whereItIs = walker.addBlock();
        walker.branch(isClosureOfMethod, whereItIs, closed);
        walker.continueIn(whereItIs);
        Draft proof(Opcode::Narrowed, origin);
        walker.chooseConservativeFields(proof);
        proof.setValueOperand(found);
        proof.setTestOperand(isClosureOfMethod);
        proof.setOperandIndex(OperandIndex::IsClosureOf::value);
        proof.setIsOnTakenEdge(true);
        closureOfMethod = walker.build(WTF::move(proof));
        if (!closureOfMethod)
            return true;
    }
    auto bindMethod = [&] {
        if (closureOfMethod)
            walker.bind(read, closureOfMethod);
        else
            bindClosureThatClosureTableHoldsForTypedAccess(walker, index.value(read), method, origin);
    };
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
            bindMethod();
            walker.checkThatMemoryAnswersAsBelieved(walker.referenceOf(read), WTF::move(readAsWritten));
            walker.bind(read, Reference());
        }
        walker.state() = WTF::move(believed);
    }
    if (!armsHoldCall && !walker.hasFailed() && !walker.isUnreachable())
        bindMethod();
    if (armsHoldCall && !walker.hasFailed() && !walker.isUnreachable())
        walker.walkStretch(inputBlock, placeOfRead + 1, end - 1);
    if (armsHoldCall && !walker.hasFailed() && !walker.isUnreachable()) {
        auto callAsWritten = index.value(call).as<Opcode::Call>();
        Vector<Reference, 8> arguments;
        for (ValueID argument : callAsWritten.argumentsOperands())
            arguments.append(walker.referenceOf(argument));
        bindMethod();
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
    FunctionID getter { };

    bool standsAboveObject() const { return method || memberOfBuiltin || getter; }
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
    BuiltinID member = base ? findMemberAboveHeirsOfBuiltinClass(walker, base, key) : BuiltinID();
    if (!member)
        whyNone = "a class below a built-in, and the name is no listed field, no method of the class, and no member above it that is a constant of the build"_s;
    return member;
}

static FunctionID findGetterOfSubclass(const Walker& walker, LayoutID subclass, PropertyKeyID key)
{
    if (!walker.has(Mechanism::DirectCalls) || walker.program().at(subclass).mixedBuiltinBase || (!walker.isInLoop() && walker.unit().role == CodeRole::CoroutineBody) || findChosenAssignedAccessor(walker, subclass, key))
        return { };
    LayoutID classLayout;
    FunctionID getter;
    FunctionID setter;
    if (!findAccessorDeclaredAboveObjectsOf(walker, subclass, key, classLayout, getter, setter) || classLayout != subclass || !getter)
        return { };
    bool isCalledDirectly = closureTableHoldsMethodForTypedAccess(walker, getter) && accessorAdmitsDirectCall(const_cast<Walker&>(walker), getter) && walker.facts().linked().mayBeMentionedBy(walker.unit(), getter);
    return isCalledDirectly ? getter : FunctionID();
}

static std::optional<FoundOffHeirOfBuiltin> findBuiltinSubclassPrivateField(const Walker& walker, const BodyValue& access, TypeID typeOfObject)
{
    LayoutID layout = layoutKnownByStructureOfClass(walker, typeOfObject);
    if (!layout || layout != access.layout() || !isDecidedByClassStructure(fieldAt(walker.program(), FieldReference { layout, access.field() })))
        return std::nullopt;
    if (access.opcode() == Opcode::PrivateSet && isBeingMadeByBodyBeingReadInTypedAccess(walker, access.objectOperand()))
        return std::nullopt;
    return FoundOffHeirOfBuiltin { layout, { }, access.field() };
}

static std::optional<FoundOffHeirOfBuiltin> findWhatReadFindsOffHeirOfBuiltin(const Walker& walker, const BodyValue& read, TypeID typeOfObject, ASCIILiteral* whyNothing = nullptr)
{
    if (read.opcode() == Opcode::PrivateGet)
        return findBuiltinSubclassPrivateField(walker, read, typeOfObject);
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
    bool isConstructorRead = classHolderIsMadeWithName(program, ClassSide::Prototype, key);
    if (!isConstructorRead && walker.facts().placesOfMembersUnder(layout, ClassSide::Prototype, key).empty()) {
        ASCIILiteral whyNone;
        BuiltinID member = findMemberOfBuiltinAboveHeirForTypedAccess(walker, layout, key, whyNone);
        return member ? std::optional(FoundOffHeirOfBuiltin { layout, { }, 0, member }) : nothing(whyNone);
    }
    if (FunctionID getter = findGetterOfSubclass(walker, layout, key))
        return FoundOffHeirOfBuiltin { layout, { }, 0, { }, getter };
    ReceiversOfTypedAccess receivers;
    receivers.set = walker.types().recordOf(typeOfObject).layouts;
    receivers.members.append(layout);
    FunctionID method = findMethodReadBehindLooksForTypedAccess(walker, read, receivers, key, whyNothing);
    if (!method)
        return std::nullopt;
    if (!closureTableHoldsMethodForTypedAccess(walker, method))
        return nothing("a class below a built-in, and the closure table does not hold the method"_s);
    bool isOwnMethod = isConstructorRead || !program.at(layout).mixedBuiltinBase || std::ranges::any_of(walker.facts().membersOf(layout), [&](const ClassMember& member) {
        return member.side == ClassSide::Prototype && member.key == key && member.function == method;
    });
    if (!isOwnMethod)
        return nothing("a class below a mixin, and the class itself does not define the method"_s);
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
    bool isLengthOfView = found->memberOfBuiltin && isLengthOfTypedArraySubclass(walker, value, found->layout);
    if (operands.areOnlyTypes && isLengthOfView) {
        walker.noteFormAskedAhead(Opcode::GetTypedArrayLength, KnowledgeLevel::Proven);
        return TypedForm::BuiltBehindLookAtCell;
    }
    if (operands.areOnlyTypes && found->getter) {
        walker.noteFormAskedAhead(Opcode::CallDirect, KnowledgeLevel::Proven, found->getter);
        return TypedForm::BuiltBehindLookAtCell;
    }
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
        if (found->getter && !walker.isUnreachable()) {
            if (callAccessor(walker, value, found->getter, operands.object, { }) || walker.hasFailed())
                return TypedForm::Built;
            Draft asWritten = walker.draftOf(value);
            walker.chooseConservativeFields(asWritten);
            walker.bind(value.id(), walker.build(WTF::move(asWritten)));
            return TypedForm::Built;
        }
        if (isLengthOfView && !walker.isUnreachable()) {
            buildTypedArraySubclassLength(walker, value, operands.object, found->layout);
            return TypedForm::Built;
        }
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
    if (!user || index.blockOf(user) != index.blockOf(read.id()) || (!allNumericOperators.contains(index.opcodeOf(user)) && !allRelations.contains(index.opcodeOf(user)) && index.opcodeOf(user) != Opcode::ToNumeric))
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
    auto isReadOffClassObject = [&](const BodyValue& read) {
        return std::ranges::contains(accesses, read.objectOperand());
    };
    Vector<PropertyKeyID, 4> keysStoredInFrontOfRead;
    for (ValueID id : accesses) {
        BodyValue read = walker.asWalked(index.value(id));
        if (isReadOffClassObject(read))
            continue;
        if (read.opcode() == Opcode::PropSet || read.opcode() == Opcode::PrivateSet) {
            keysStoredInFrontOfRead.append(keyOfAccessToSlot(walker.program(), read));
            continue;
        }
        std::optional<FoundOffHeirOfBuiltin> found = findWhatReadFindsOffHeirOfBuiltin(walker, read, walker.typeOf(object));
        TypeID asked = found && !keysStoredInFrontOfRead.contains(keyOfAccessToSlot(walker.program(), read)) ? numbersAskedOfValueReadOffHeirOfBuiltin(walker, read, *found) : TypeID();
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
        if (isReadOffClassObject(read)) {
            std::optional<FoundOffHeirOfBuiltin> classObject = findWhatReadFindsOffHeirOfBuiltin(walker, walker.asWalked(index.value(read.objectOperand())), walker.typeOf(object));
            std::optional<FoundOffConstructorFunction> member = classObject && classObject->method ? findWhatRecordsExpectOffConstructorFunction(walker, read, walker.types().forFunction(classObject->method)) : std::nullopt;
            if (member && !walker.state().guardOfReadsHolds(member->classLayout, read.key(), ClassSide::Static))
                conditionsListed.appendUnlessListed(conditions, MemberConditionRecord { MemberConditionKind::MemberUnchanged, member->classLayout, ClassSide::Static, read.key() });
            continue;
        }
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

bool readsStaticOfClass(Walker& walker, const BodyValue& read, TypeID typeOfClassObject)
{
    std::optional<FoundOffConstructorFunction> found = read.opcode() == Opcode::PropGet ? findWhatRecordsExpectOffConstructorFunction(walker, read, typeOfClassObject) : std::nullopt;
    return found && !walker.program().at(found->classLayout).has(LayoutFlags::IsConstructorFunction);
}

bool findsOneMethodThroughLookups(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    auto typeOfInput = [&](ValueID input) {
        return input == read.objectOperand() ? typeOfObject : walker.typeOf(walker.referenceOf(input));
    };
    if (typedFormOf(walker, read, TypesOfInputs(typeOfInput)) != TypedForm::BuiltBehindLookAtCell)
        return false;
    TypeID found = typeFoundBehindLooks(walker, read, typeOfObject);
    return found && singleFunctionOf(walker.types(), walker.types().at(found));
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
        return found->method ? walker.types().forFunction(found->method) : found->getter ? TypeID(TypeTable::anything) : typeOfFunctionReadOffFunctionPrototype(walker, read.key());
    if (!receivers)
        return { };
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, read.key(), { }, true);
    if (field.coverage != CoverageOfTypedAccess::NoReceiver)
        return { };
    if (FunctionID method = findMethodReadBehindLooksForTypedAccess(walker, read, *receivers, read.key()))
        return walker.types().forFunction(method);
    if (BuiltinID location = findObjectPrototypeMember(walker, *receivers, field, read.key()))
        return typeOfBuiltinObject(walker.typing(), location);
    return !read.isFreeName() && looksAtCellShowThatReadFindsNothing(walker, *receivers, field, read.key()) ? TypeID(TypeTable::undefined) : TypeID();
}

ALWAYS_INLINE_INTO_OTHER_FILES bool readOffConstructorFunctionAwaitsGuard(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    std::optional<FoundOffConstructorFunction> found = findWhatRecordsExpectOffConstructorFunction(walker, read, typeOfObject);
    if (!found || walker.state().guardOfReadsHolds(found->classLayout, read.key(), ClassSide::Static))
        return false;
    return found->method || found->getter || buildReadOffFunctionPrototype(walker, read, typeOfObject, { }) != TypedForm::None;
}

namespace {

struct TypedStoreAskedAhead {
    TypedForm form { TypedForm::None };
    OptionSet<CellFact> askedOfCell;
    bool creates { false };
    bool asksWhatIsAboveObject { false };
};

struct FieldInKnownOrder {
    bool isListed { false };
    std::optional<uint32_t> wordWithFieldListedNext;
};

} // anonymous namespace

static bool updatesSlotForTypedAccess(const Walker& walker, const BodyValue& store, PropertyKeyID key)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (store.opcode() != Opcode::PropSet || index.isParameter(store.valueOperand()))
        return false;
    bool isUpdate = false;
    index.value(store.valueOperand()).forEachOperand([&](ValueID operand, uint32_t) {
        if (!index.isParameter(operand) && index.opcodeOf(operand) == Opcode::PropGet)
            isUpdate |= index.value(operand).key() == key && index.value(operand).objectOperand() == store.objectOperand();
    });
    return isUpdate;
}

static bool isGivenBySuperCall(const Walker& walker, ValueID object)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID root = valueBehindOtherNamesForTypedAccess(index, object, walker.facts().summaries().at(walker.currentFunction()).otherNamesOfObjectsBeingMade.span());
    if (!root || index.isParameter(root))
        return false;
    Opcode opcode = index.opcodeOf(root);
    if (opcode == Opcode::SuperConstruct || opcode == Opcode::SuperConstructList)
        return true;
    if (opcode != Opcode::NewObject)
        return false;
    if (walker.depthOfCopies() && walker.program().at(walker.currentFunction()).constructedClass)
        return true;
    const LinkTables& linked = walker.facts().linked();
    uint32_t function = walker.currentFunction().index();
    bool isConstructedRepeatedly = walker.isInLoop() || linked.constructorsCalledInNestedIterations.get(function) || linked.functionsFinishedInNestedIterations.get(function) || linked.functionsReachedByRecursion.get(function);
    return isConstructedRepeatedly && index.value(root).as<Opcode::NewObject>().prototypeOperand();
}

static TypedStoreAskedAhead askAheadAboutTypedStore(const Walker& walker, const BodyValue& value, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, bool isDefinition)
{
    OptionSet<CellFact> toOverwrite = factsRequiredToOverwrite(receivers, isDefinition);
    if (isFilledForBodyBeingReadInTypedAccess(walker, receivers, field, value.objectOperand(), key))
        return { toOverwrite.isEmpty() ? TypedForm::Built : TypedForm::BuiltBehindLookAtCell, toOverwrite, false };

    std::optional<OptionSet<CellFact>> toCreate = smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return mayCreateInTypedStore(walker, receivers, field, key, isDefinition, facts, nullptr);
    });
    bool asksWhatIsAboveObject = !toCreate && !isDefinition && isGivenBySuperCall(walker, value.objectOperand());
    if (asksWhatIsAboveObject) {
        toCreate = smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
            return mayCreateInTypedStore(walker, receivers, field, key, false, facts, nullptr, LookupAbove::OfStore, true);
        });
        if (toCreate)
            return { TypedForm::BuiltBehindLookAtCell, *toCreate, true, true };
    }
    if (toCreate && toCreate->isEmpty() && (isDefinition || !storeAsksGuardsWherePrototypeStaysForTypedAccess(walker, receivers, field, key)))
        return { TypedForm::Built, { }, true };

    bool isStoredByInitializer = walker.facts().linked().initializerMethods.get(walker.currentFunction().index());
    bool slotIsLikelyFilled = paysToLookAtSlotForTypedAccess(walker, field, key) && !field.isAppended && (field.isOfBirth || (field.isFilledByInitializer && !isStoredByInitializer)) && !isBeingMadeByBodyBeingReadInTypedAccess(walker, value.objectOperand());
    bool isLateNumber = !slotIsLikelyFilled && (field.isAppended || !field.isOfBirth) && field.holdsNumbersStoredLate && paysToLookAtSlotForTypedAccess(walker, field, key, true) && updatesSlotForTypedAccess(walker, value, key)
        && !isBeingMadeByBodyBeingReadInTypedAccess(walker, value.objectOperand());
    auto isFilledBeforeLoop = [&] {
        if (!toCreate || isDefinition || field.mayBeListedWithoutStorage || !walker.canRestartIteration() || !walker.isInCopyOfAccessor() || possibleChangesForbid(receivers.possibleChanges, TypedAccess::ReadsFilledSlot))
            return false;
        Reference object = walker.referenceOf(value.objectOperand());
        return object && object.isValue() && !walker.state().isFresh(object) && walker.isInvariantObjectInSpecializedVersion(object, Walker::IterationPurity::KnownEffectsOnly);
    };
    if (!slotIsLikelyFilled && !isLateNumber && isFilledBeforeLoop())
        return { toOverwrite.isEmpty() ? TypedForm::BuiltBehindFilledSlot : TypedForm::BuiltBehindFilledSlotAndLookAtCell, toOverwrite, false };
    if (toCreate && !slotIsLikelyFilled && !isLateNumber)
        return { TypedForm::BuiltBehindLookAtCell, *toCreate, true };
    bool paysToLookUnderSymbol = field.isRequired && !field.mayBeListedWithoutStorage && isSymbolKind(walker.program().at(key).kind);
    if (!(isLateNumber || paysToLookAtSlotForTypedAccess(walker, field, key) || paysToLookUnderSymbol))
        return { };
    return { toOverwrite.isEmpty() ? TypedForm::BuiltBehindFilledSlot : TypedForm::BuiltBehindFilledSlotAndLookAtCell, toOverwrite, false };
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
    if (!field || !orderWordTellsKeysOfObjectsOf(walker, layout) || !plan.slot(layout, *field).hasStorage() || !mayBeCreatedByStore(plan.slot(layout, *field).representation))
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
    ValueID root = valueBehindOtherNamesForTypedAccess(walker.currentBodyIndex(), objectAsWritten, walker.facts().summaries().at(walker.currentFunction()).otherNamesOfObjectsBeingMade.span());
    const bool isLiteral = receivers.members.size() == 1 && isConstantLiteralInTypedAccess(walker, root);
    if (known.contains(CellFact::BearsNoMark) || (!isLiteral && !known.contains(CellFact::NoFieldOutsideSlot)))
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
    bool everyFieldIsShown = known.contains(CellFact::NoFieldOutsideSlot);
    for (uint32_t index = plan.layout(first).numberOfFields(); index-- && everyFieldIsShown;)
        everyFieldIsShown = !plan.slot(first, index).isFilledAtBirth || isShownFilled(index);
    if (!everyFieldIsShown && !isLiteral)
        return;
    auto numberInFirst = numberOfFieldsOfBirth(first);
    for (LayoutID member : receivers.members) {
        if (numberOfFieldsOfBirth(member) != numberInFirst)
            return;
    }
    Origin origin = walker.originOf(store);
    origin.isTailCall = false;
    bool mayBeFrozen = !known.contains(CellFact::NotFrozen) && std::ranges::any_of(receivers.members, [&](LayoutID member) { return plan.layout(member).marksFrozenObjects; });
    bool mayHaveOtherPrototype = !known.contains(CellFact::HasLayoutPrototype) && std::ranges::any_of(receivers.members, [&](LayoutID member) { return plan.layout(member).marksOtherPrototypes; });
    everyFieldIsShown = everyFieldIsShown && !mayBeFrozen && !mayHaveOtherPrototype;
    Draft finish(everyFieldIsShown ? Opcode::FinishBirth : Opcode::TryFinishBirth, origin);
    finish.setObjectOperand(object);
    walker.chooseConservativeFields(finish);
    walker.build(WTF::move(finish));
    if (!walker.isUnreachable() && everyFieldIsShown)
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

static bool isCalledInLoops(const Walker& walker)
{
    FunctionID function = walker.currentFunction();
    return walker.isInRepeatedCode() || (function && walker.facts().linked().functionsCalledInLoops.get(function.index()));
}

static bool isAllocatedOnOneJoinPredecessor(const Code& code, Reference object)
{
    ValueID cell = object.isValue() && !code.isConstant(object.id()) ? underlyingCellOf(code, object.id()) : ValueID();
    if (!cell || code.isConstant(cell) || code.at(cell).opcode != Opcode::Parameter)
        return false;
    const Block& join = code.at(code.at(cell).block());
    EdgeID first = join.firstEdgeIn();
    EdgeID second = first ? code.at(first).nextInto() : EdgeID();
    if (join.isLoopHead || join.isHandlerBlock() || join.isEntry || !second || code.at(second).nextInto())
        return false;
    size_t place = join.placeOfParameter(cell);
    for (EdgeID edge : { first, second }) {
        std::span<const ValueID> arguments = code.at(edge).arguments();
        ValueID brought = place < arguments.size() && arguments[place] && !code.isConstant(arguments[place]) ? underlyingCellOf(code, arguments[place]) : ValueID();
        if (brought && !code.isConstant(brought) && code.at(brought).opcode == Opcode::NewObject)
            return true;
    }
    return false;
}

static Reference arrayFreedByStoreInTypedAccess(Walker& walker, const BodyValue& store, Reference object, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    if (store.opcode() != Opcode::PropSet || !walker.facts().linked().storeFreesList(walker.currentFunction(), store.id()) || !walker.isDoneInIterations() || !walker.has(Mechanism::EscapeAnalysis) || !walker.has(Mechanism::CellsInFrames))
        return { };
    Reference held = valueInSlotForTypedAccess(walker, object, field);
    if (!held) {
        Origin origin = walker.originOf(store);
        origin.isTailCall = false;
        Draft read(Opcode::GetSlotOrUndefined, origin);
        walker.chooseConservativeFields(read);
        fillSlotOperationForTypedAccess(read, object, receivers, key);
        held = walker.build(WTF::move(read));
    }
    TypeID type = held && held.isRecord() && !walker.isUnreachable() ? walker.typeOf(held) : TypeID();
    return type && hasKind(walker.types(), walker.types().withoutNullish(type), OperandKind::Array) ? held : Reference();
}

static void releaseListInTypedAccess(Walker& walker, const BodyValue& store, Reference list)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID stored = index.stripCasts(store.as<Opcode::PropSet>().valueOperand());
    Origin origin = walker.originOf(store);
    origin.isTailCall = false;
    Draft release(Opcode::ReleaseList, origin);
    walker.chooseConservativeFields(release);
    release.setListOperand(list);
    release.setIsCopy(!index.isParameter(stored) && index.opcodeOf(stored) == Opcode::Call);
    walker.build(WTF::move(release));
    walker.count(StatisticID::ListsOfFieldsReleased);
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
    bool isFilled = mayOverwriteInTypedStore(*receivers, isDefinition, knownOfCell) && ((inKnownOrder && inKnownOrder->isListed)
        || isFilledForBodyBeingReadInTypedAccess(walker, *receivers, field, objectAsWritten, key) || provesSlotFilledForTypedAccess(walker, valueInSlotForTypedAccess(walker, object, field), key)
        || isSlotKnownFilledForTypedAccess(walker, object, *receivers, field, key));
    if (!isFilled && !mayCreateInTypedStore(walker, *receivers, field, key, isDefinition, knownOfCell, &walker.state())) {
        bool objectPassesStores = !isDefinition && findKnown(walker, KnownNote::StoresPassAbove, { }, object, 0);
        if (!objectPassesStores || !mayCreateInTypedStore(walker, *receivers, field, key, false, knownOfCell, &walker.state(), LookupAbove::OfStore, true))
            return TypedForm::None;
        walker.checkPassedStores(object, walker.originOf(value));
    }
    if (!isFilled && !isDefinition)
        askAgainGuardsAboveReceiversForTypedAccess(walker, *receivers, key, true, walker.originOf(value));
    if (walker.isUnreachable())
        return TypedForm::None;
    if (isFilled && walker.has(Mechanism::Folding) && storeLaysWhatSlotOfFreshObjectHolds(walker, object, valueInSlotForTypedAccess(walker, object, field), stored))
        return TypedForm::Built;
    Opcode opcode = isFilled ? Opcode::PutSlot : Opcode::PutSlotOrAdd;
    if (opcode == Opcode::PutSlot) {
        const BodyIndex& index = walker.currentBodyIndex();
        bool isFirstFill = walker.isInLoop() && value.id().index() < index.numberOfValues() && value.opcode() == Opcode::PropSet && findFirstFill(walker.types(), index, index.blockOf(value.id())) == value.id() && !walker.types().overlap(walker.typeOf(stored), TypeTable::nullish);
        walker.noteSlotStore(object, key, valueInSlotForTypedAccess(walker, object, field), stored, isFirstFill);
    } else if (walker.isInLoop() && field.liesInFormOfNumbersOrBooleans && updatesOwnValueForTypedAccess(walker.code(), stored, valueInSlotForTypedAccess(walker, object, field)) && slotStaysFilledForTypedAccess(walker, *receivers, field, key))
        walker.noteUnfilledSlot(object, key);

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
        store.setIsExpectedToCreate(walker.state().isFresh(object) || !!current.constructedClass || current.kind == FunctionKind::Initializer || (isCalledInLoops(walker) && isAllocatedOnOneJoinPredecessor(walker.code(), object)));
        if (orderWordAfter)
            store.setOrderWordAfter(OrderWord(*orderWordAfter));
    }
    Reference overwritten = isDefinition ? Reference() : arrayFreedByStoreInTypedAccess(walker, value, object, *receivers, field, key);
    uint32_t firstBuilt = walker.code().numberOfValues();
    Reference built = walker.build(WTF::move(store));
    stored = slotValueAfterStep(walker, firstBuilt, opcode, key, stored);
    if (overwritten && !walker.isUnreachable())
        releaseListInTypedAccess(walker, value, overwritten);
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
    noteFilledSlotForTypedAccess(walker, object, *receivers, field, key);
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
    LayoutID classLayout = declaredLayoutOf(walker.program(), declaring);
    if (!walker.plan().hasOneInstance())
        return false;
    return walker.facts().isEvaluatedAtMostOnce(classLayout) || (walker.facts().isEvaluatedOncePerInstance(classLayout) && hasKnownSingleInstance(walker, walker.program().at(classLayout).module));
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

static LayoutSetID findPrivateNameHolders(Walker& walker, PropertyKeyID key)
{
    static constexpr size_t maxHolders = 8;
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    Vector<LayoutID, maxHolders> holders;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutID layout(index);
        if (!plan.slots().holdsCompiledObjects(layout) || !findField(program, layout, key))
            continue;
        if (holders.size() == maxHolders || !isFoundByShapeTest(program.at(layout)) || plan.shapes().shapesOf(layout).isEmpty())
            return { };
        holders.append(layout);
    }
    return holders.isEmpty() ? LayoutSetID() : walker.types().layoutSets().ofSorted(holders.span());
}

static std::optional<ReceiversOfTypedAccess> privateNameHolders(Walker& walker, PropertyKeyID key)
{
    LayoutSetID holders;
    if (std::optional<uint32_t> kept = walker.roundKept(Walker::RoundKeptByWalker::PrivateNameHolders, { }, key.index()))
        holders = LayoutSetID::fromBits(*kept);
    else {
        holders = findPrivateNameHolders(walker, key);
        walker.keepRound(Walker::RoundKeptByWalker::PrivateNameHolders, { }, key.index(), holders.bits());
    }
    return holders ? findReceiversAmongLayoutsForTypedAccess(walker, holders) : std::nullopt;
}

static TypedForm buildPrivateNameTest(Walker& walker, const BodyValue& test, const InputsOfTypedOperation& inputs, PropertyKeyID key)
{
    if (!walker.isInLoop() || !walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !slotTellsWhoHasPrivateNameForTypedAccess(walker, test.layout()))
        return TypedForm::None;
    TypeID typeOfValue = inputs.typeOf(test.objectOperand());
    if (!typeOfValue || !walker.types().at(typeOfValue).isJavaScriptValue() || walker.types().at(typeOfValue).mayBeUnspecified())
        return TypedForm::None;
    std::optional<ReceiversOfTypedAccess> receivers = privateNameHolders(walker, key);
    if (!receivers)
        return TypedForm::None;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { });
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || !field.isMovedOutByNoStore)
        return TypedForm::None;
    TypeArena& types = walker.types();
    TypeID expected = findExpectation(walker, test.objectOperand()).type;
    if (expected && types.at(expected).hasObjects() && types.isSubset(types.withOnlyObjects(expected), types.forLayoutSet(receivers->set)))
        return TypedForm::None;
    if (inputs.areOnlyTypes())
        return TypedForm::Built;

    Reference value = walker.referenceOf(test.objectOperand());
    if (!value || !value.isValue())
        return TypedForm::None;
    Origin origin = walker.originOf(test);
    bool armsEndWithBranch = walker.canThreadBranchAfter(test);
    BlockID meeting;
    Reference answer;
    auto arrive = [&](Reference found) {
        if (!found || walker.isUnreachable())
            return;
        if (armsEndWithBranch) {
            walker.bind(test.id(), found);
            if (!walker.threadBranchAfter(test))
                walker.fail("an arm of a test of a private name does not walk the end of its block"_s);
            return;
        }
        if (!meeting) {
            meeting = walker.addBlock();
            answer = walker.addParameter(meeting, TypeTable::boolean, walker.formOfParameter(TypeTable::boolean, { }));
        }
        walker.jumpTo(meeting, singleElementSpan(found));
    };
    auto booleanConstant = [&](bool boolean) {
        Draft constant(Opcode::Boolean);
        constant.setBoolean(boolean);
        return walker.constant(WTF::move(constant));
    };
    auto narrowedBy = [&](Reference condition, uint32_t operandIndex) {
        Draft narrowed(Opcode::Narrowed, origin);
        walker.chooseConservativeFields(narrowed);
        narrowed.setValueOperand(value);
        narrowed.setTestOperand(condition);
        narrowed.setOperandIndex(operandIndex);
        narrowed.setIsOnTakenEdge(true);
        return walker.build(WTF::move(narrowed));
    };

    Draft shapeTest(Opcode::ShapeTest, origin);
    walker.chooseConservativeFields(shapeTest);
    shapeTest.setObjectOperand(value);
    shapeTest.setLayouts(receivers->set);
    Reference isHolder = walker.build(WTF::move(shapeTest));
    if (!isHolder)
        return TypedForm::Built;
    walker.recordSpeculation();
    BlockID holder = walker.addBlock();
    BlockID noHolder = walker.addBlock();
    BlockID asksRunTime = walker.addBlock(BlockFrequency::Rare);
    walker.branch(isHolder, holder, noHolder);

    walker.continueIn(holder);
    if (Reference object = narrowedBy(isHolder, OperandIndex::ShapeTest::object)) {
        if (armsEndWithBranch && !walker.state().checkedNameOf(value))
            walker.state().noteCheckedName(value, object);
        Reference held = valueInSlotForTypedAccess(walker, object, field);
        bool isFilled = isFilledForBodyBeingReadInTypedAccess(walker, *receivers, field, test.objectOperand(), key) || provesSlotFilledForTypedAccess(walker, held, key);
        arrive(isFilled && walker.has(Mechanism::Folding) ? booleanConstant(true) : walker.build(draftOfSlotOperationForTypedAccess(walker, test, Opcode::IsSlotFilled, object, *receivers, key)));
    }

    walker.continueIn(noHolder);
    Draft objectTest(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(objectTest);
    objectTest.setValueOperand(value);
    objectTest.setTestedType(TypeTable::object);
    if (Reference isObject = walker.build(WTF::move(objectTest))) {
        BlockID objects = walker.addBlock();
        walker.branch(isObject, objects, asksRunTime);
        walker.continueIn(objects);
        if (Reference object = narrowedBy(isObject, OperandIndex::TypeTest::value)) {
            Draft ordinaryTest(Opcode::HasNoExtraProperties, origin);
            walker.chooseConservativeFields(ordinaryTest);
            ordinaryTest.setObjectOperand(object);
            if (Reference isOrdinary = walker.build(WTF::move(ordinaryTest))) {
                BlockID ordinary = walker.addBlock();
                walker.branch(isOrdinary, ordinary, asksRunTime);
                walker.continueIn(ordinary);
                arrive(booleanConstant(false));
            }
        }
    }

    walker.continueIn(asksRunTime);
    if (!walker.isUnreachable()) {
        Draft written = walker.draftOf(test);
        walker.chooseConservativeFields(written);
        arrive(walker.build(WTF::move(written)));
    }
    if (meeting) {
        walker.continueIn(meeting);
        walker.bind(test.id(), answer);
    }
    return TypedForm::Built;
}

static TypedForm buildSubclassPrivateNameAccessForTypedAccess(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    bool stores = value.opcode() == Opcode::PrivateSet;
    std::optional<FoundOffHeirOfBuiltin> found = stores || value.opcode() == Opcode::PrivateGet ? findBuiltinSubclassPrivateField(walker, value, inputs.typeOf(value.objectOperand())) : std::nullopt;
    if (!found)
        return TypedForm::None;
    if (inputs.areOnlyTypes())
        return TypedForm::BuiltBehindLookAtCell;
    Reference name = nameBehindLookAtStructureOfClass(walker, walker.referenceOf(value.objectOperand()), found->layout);
    Reference stored = stores ? walker.referenceOf(value.valueOperand()) : Reference();
    if (!name || (stores && !stored))
        return TypedForm::None;
    Origin origin = walker.originOf(value);
    checkStructureOfClassThatIsRemembered(walker, name, found->layout, origin);
    if (walker.isUnreachable())
        return TypedForm::None;
    walker.bind(value.id(), stores ? buildStoreIntoFieldOfHeir(walker, name, stored, found->layout, found->field, origin) : buildReadOfFieldOfHeir(walker, name, found->layout, found->field, origin));
    return TypedForm::Built;
}

static bool isObjectLiteralWithoutPrivateNames(Walker& walker, Reference object)
{
    if (!object || !object.isValue() || !walker.has(Mechanism::Folding))
        return false;
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, object.id());
    if (code.isConstant(cell) || code.at(cell).opcode != Opcode::NewObject || code.at(cell).block() != walker.currentBlock() || !walker.state().isFresh(Reference::value(cell)))
        return false;
    for (uint32_t number = cell.index() + 1; number < code.numberOfValues(); ++number) {
        if (code.at(ValueID(number)).opcode == Opcode::PrivateDefine)
            return false;
    }
    const Layout& layout = walker.program().at(code.at(cell).layout());
    return layout.kind == LayoutKind::Record && !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
}

static TypedForm buildAccessUnderPrivateNameForTypedAccess(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::PrivateHas && !inputs.areOnlyTypes() && isObjectLiteralWithoutPrivateNames(walker, walker.referenceOf(value.objectOperand()))) {
        bindBooleanForTypedAccess(walker, value, false);
        return TypedForm::Built;
    }
    const Field& declared = fieldAt(walker.program(), FieldReference { value.layout(), value.field() });
    PropertyKeyID key = declared.key;
    bool stores = opcode == Opcode::PrivateSet || opcode == Opcode::PrivateDefine;
    TypeID typeOfStored = stores ? inputs.typeOf(value.valueOperand()) : TypeID();
    if (!key || (stores && !typeOfStored))
        return TypedForm::None;
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, inputs.typeOf(value.objectOperand()));
    if (!receivers)
        return opcode == Opcode::PrivateHas ? buildPrivateNameTest(walker, value, inputs, key) : buildSubclassPrivateNameAccessForTypedAccess(walker, value, inputs);
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, typeOfStored);
    if (!field.liesInOneSlot || !field.isMovedOutByNoStore)
        return TypedForm::None;
    if (stores && !field.takesStoredValue && !(declared.flags.contains(FieldFlags::IsBrand) && opcode == Opcode::PrivateDefine))
        return TypedForm::None;

    auto buildStore = [&](Opcode store) {
        Reference object = walker.referenceOf(value.objectOperand());
        Reference stored = walker.referenceOf(value.valueOperand());
        if (store == Opcode::PutSlot)
            walker.noteSlotStore(object, key, valueInSlotForTypedAccess(walker, object, field), stored);
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
        uint32_t firstBuilt = walker.code().numberOfValues();
        walker.bind(value.id(), walker.build(WTF::move(draft)));
        stored = slotValueAfterStep(walker, firstBuilt, store, key, stored);

        TypeID typeOfObject = walker.typeOf(object);
        walker.state().invalidateAliasedSlots(object, field.place, scopedLambda<bool(Reference)>([&](Reference other) {
            TypeID typeOfOther = walker.typeOf(other);
            return !typeOfOther || walker.types().mayBeSameObject(typeOfObject, typeOfOther);
        }), scopedLambda<bool(Reference, LayoutID)>([&](Reference named, LayoutID layout) {
            return isOfLayoutAlone(walker, named, layout);
        }));
        walker.state().noteSlot(object, field.place, stored);
        if (store == Opcode::PutSlotOrAdd) {
            walker.state().noteOrderWord(object, orderWordAfter);
            bool wasUnfinished = !walker.state().cellFacts(object).contains(CellFact::BearsNoMark);
            finishBirthBehindStoreForTypedAccess(walker, value, value.objectOperand(), object, *receivers, field);
            if (wasUnfinished && walker.state().cellFacts(object).contains(CellFact::BearsNoMark))
                walker.count(StatisticID::PrivateFieldsFinishingBirths);
        }
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
    Draft draft = draftOfSlotOperationForTypedAccess(walker, value, Opcode::GetSlot, object, *receivers, key);
    Reference carried = isFilledByConstructions ? walker.readCarriedField(draft, field.place) : Reference();
    Reference read = carried ? carried : walker.build(WTF::move(draft));
    walker.bind(value.id(), read);
    if (read) {
        walker.state().noteSlot(object, field.place, read);
        walker.noteSlotRead(object, key, read);
    }
    return TypedForm::Built;
}

static bool lookAsksForValueOfWrittenType(Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, receivers, key, { });
    return field.coverage == CoverageOfTypedAccess::EveryReceiver && field.liesInOneSlot && lookAsksForValueOfWrittenTypeInTypedAccess(field);
}

bool looksAskForValueOfWrittenType(Walker& walker, TypeID typeOfObject, PropertyKeyID key)
{
    std::optional<ReceiversOfTypedAccess> receivers = typeOfObject ? findReceiversOfTypedAccess(walker, typeOfObject) : std::nullopt;
    return receivers && lookAsksForValueOfWrittenType(walker, *receivers, key);
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

Reference readSlotFoundFilled(Walker& walker, const BodyValue& access, Reference object, bool isStoredInLoop)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return { };
    PropertyKeyID key = keyOfAccessToSlot(walker.program(), access);
    Draft read(readOfFilledSlotForTypedAccess(walker, *receivers, key), walker.originOf(access));
    fillSlotOperationForTypedAccess(read, object, *receivers, key);
    read.setHoldsDeclaredType(access.opcode() == Opcode::PropGet && lookAsksForValueOfWrittenType(walker, *receivers, key));
    if (read.opcode == Opcode::GetSlot && !read.holdsDeclaredType() && access.opcode() == Opcode::PropGet && walker.state().cellFacts(object).contains(CellFact::BearsNoMark)) {
        FieldOfTypedAccess raw = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
        read.setHoldsDeclaredType(raw.coverage == CoverageOfTypedAccess::EveryReceiver && raw.liesInOneSlot && raw.markAnswersForSlot && raw.markAnswersForValue);
    }
    Reference held;
    if (isStoredInLoop && read.opcode == Opcode::GetSlot && !read.holdsDeclaredType()) {
        read.setLevel(KnowledgeLevel::Proven);
        held = walker.readCarriedField(read, describeFieldOfTypedAccess(walker, *receivers, key, { }).place);
    }
    if (!held) {
        held = walker.build(WTF::move(read));
        if (held && isStoredInLoop)
            walker.noteSlotReadOnEveryIteration(object, held);
    }
    readScopeBesideSlotForTypedAccess(walker, walker.originOf(access), held);
    if (held) {
        FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { });
        walker.state().noteSlot(object, field.place, held);
        noteFilledSlotForTypedAccess(walker, object, *receivers, field, key);
    }
    return held;
}

void noteSlotOfAccess(Walker& walker, const BodyValue& access, Reference object, Reference held)
{
    std::optional<ReceiversOfTypedAccess> receivers = held ? findReceiversOfTypedAccess(walker, walker.typeOf(object)) : std::nullopt;
    if (receivers)
        walker.state().noteSlot(object, describeFieldOfTypedAccess(walker, *receivers, keyOfAccessToSlot(walker.program(), access), { }).place, held);
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
    if (operation.opcode == Opcode::PutSlot)
        return factsRequiredToOverwrite(*receivers, operation.isDefinition());
    return smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        if (operation.opcode == Opcode::PutSlotOrAdd)
            return mayCreateInTypedStore(walker, *receivers, field, key, operation.isDefinition(), facts, nullptr);
        return mayFindSlotEmptyBehindLooksInTypedRead(walker, *receivers, field, key, facts, nullptr);
    }).value_or(OptionSet<CellFact>());
}

void askWhatLookupLeansOn(Walker& walker, const Draft& lookup)
{
    if (lookup.opcode == Opcode::GetSlotOrUndefined && lookup.looksAboveEmptySlot())
        return;
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
    bool asksAmongExtrasAlone = findsNothing && lookup.asksForNoExtraPropertyUnderKey() && std::ranges::all_of(receivers.members, [&](LayoutID member) {
        LayoutID classLayout;
        return !!findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout);
    });
    if (asksAmongExtrasAlone)
        return;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, receivers, key, stores ? walker.typeOf(lookup.valueOperand()) : TypeID(), !findsNothing);
    std::optional<OptionSet<CellFact>> askedAhead = findsNothing ? askAheadAboutReadThatFindsNothing(walker, receivers, field, key) : isProvenStore ? OptionSet<CellFact>() : stores ? smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return mayCreateInTypedStore(walker, receivers, field, key, false, facts, nullptr);
    }) : askAheadAboutReadOfSlotThatMayBeEmpty(walker, receivers, field, key);
    if (!askedAhead) {
        walker.fail("what a lookup leans on cannot be found from its draft"_s);
        return;
    }
    OptionSet<CellFact> factsLeanedOn = *askedAhead;
    bool leansOnGuards = findsNothing ? receivers.possibleChanges.contains(LayoutChange::ChangesPrototype) : isProvenStore ? receivers.possibleChanges.contains(LayoutChange::ChangesPrototype) || !storePassesAboveWithoutGuards(walker, receivers, key) : stores ? storeNeedsGuards(walker, receivers, field, key, factsLeanedOn) : readNeedsGuards(receivers, factsLeanedOn);
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
    if (readNeedsGuards(*receivers, *asked)) {
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
    draft.setReceiverMayBeInBirth(isBeingMadeByBodyBeingReadInTypedAccess(walker, read.objectOperand()) || canBePartiallyConstructed(walker, read.objectOperand(), receivers->set));
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
    if (ahead.asksWhatIsAboveObject || !ahead.askedOfCell.containsOnly({ CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype }))
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
        if (storeNeedsGuards(walker, *receivers, field, key, ahead.askedOfCell)) {
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
    if (ahead.creates) {
        draft.setConditions(conditions.span());
        draft.setReceiverMayBeInBirth(canBePartiallyConstructed(walker, store.objectOperand(), receivers->set));
    }
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

bool buildGuardedStringEquality(Walker& walker, const BodyValue& equality)
{
    if (equality.opcode() != Opcode::StrictEq || !walker.has(Mechanism::Guesses) || walker.isNestedTooDeeply() || walker.isInLoop() || walker.code().at(walker.currentBlock()).frequency == BlockFrequency::Rare)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (!index.hasOneUse(equality.id()) || !walker.isAskedByBranchOfItsBlock(equality))
        return false;
    BodyBlock block = index.block(walker.inputBlockOf(equality.id()));
    if (equality.id().index() + 1 != block.firstValue().index() + block.numberOfValues())
        return false;
    BodyTerminator end = block.terminator();
    BodyBlock next = index.block(end.taken().block());
    BodyTerminator question = next.terminator();
    if (next.numberOfValues() != 2 || next.hasHandler() || question.kind() != TerminatorKind::Branch)
        return false;
    BodyTarget unequal = end.notTaken();
    BodyTarget unmarked = question.notTaken();
    if (unequal.block() != unmarked.block() || unequal.numberOfIncomingValues() || unmarked.numberOfIncomingValues())
        return false;
    BodyValue read = index.value(next.firstValue());
    BodyValue truth = index.value(ValueID(next.firstValue().index() + 1));
    if (read.opcode() != Opcode::PropGet || read.isFreeName() || truth.opcode() != Opcode::ToBoolean || truth.operand(0) != read.id() || question.operand(0) != truth.id())
        return false;

    ValueID holder = index.stripCasts(read.objectOperand());
    auto isReadOffHolder = [&](ValueID operand) {
        ValueID source = index.stripCasts(operand);
        return !index.isParameter(source) && index.opcodeOf(source) == Opcode::PropGet && index.stripCasts(index.value(source).objectOperand()) == holder;
    };
    if (!isReadOffHolder(equality.operand(0)) && !isReadOffHolder(equality.operand(1)))
        return false;
    TypeArena& types = walker.types();
    Reference left = walker.referenceOf(equality.operand(0));
    Reference right = walker.referenceOf(equality.operand(1));
    auto isUnknownString = [&](Reference held) {
        return held && held.isValue() && !held.isConstant() && types.at(walker.typeOf(held)).isAnyStringOrNullish();
    };
    if (!isUnknownString(left) || !isUnknownString(right))
        return false;
    Reference object = walker.referenceOf(read.objectOperand());
    std::optional<ReceiversOfTypedAccess> receivers = object && object.isValue() ? findReceiversOfTypedAccess(walker, walker.typeOf(object)) : std::nullopt;
    if (!receivers || !walker.has(Mechanism::Folding))
        return false;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, read.key(), { }, true);
    if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || lookAsksForValueOfWrittenType(walker, *receivers, read.key()))
        return false;
    std::optional<OptionSet<CellFact>> asked = askAheadAboutReadOfSlotThatMayBeEmpty(walker, *receivers, field, read.key());
    if (!asked || !asked->containsOnly({ CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype }) || slotHoldsMarkForTypedAccess(walker, *receivers, read.key()))
        return false;
    FieldAccess access = walker.plan().fieldAccess(walker.program(), read.key(), receivers->set, receivers->members.span(), false);
    if (!access.hasLocation || !access.slot.isOutOfLine())
        return false;

    Origin origin = walker.originOf(equality);
    origin.isTailCall = false;
    Draft test(Opcode::IsSlotFilled, origin);
    fillSlotOperationForTypedAccess(test, object, *receivers, read.key());
    test.setHoldsDeclaredType(true);
    Reference isFilled = walker.build(WTF::move(test));
    if (!isFilled || !isFilled.isValue() || walker.isUnreachable() || walker.hasFailed())
        return false;
    BlockID filled = walker.addBlock();
    BlockID empty = walker.addBlock();
    walker.branch(isFilled, filled, empty);
    Vector<Walker::JoinArm, 2> arms;
    walker.continueIn(empty);
    const ValueID reads[] { read.id() };
    lookAtCellInFrontOfStores(walker, reads, object, scopedLambda<void(Reference)>([&](Reference test) {
        if (!test || walker.isUnreachable())
            return;
        BlockID passed = walker.addBlock();
        walker.branch(test, passed, filled);
        walker.continueIn(passed);
    }));
    if (!walker.isUnreachable() && !walker.hasFailed())
        arms.append({ walker.currentBlock(), { walker.constant(PrimitiveKnownToFolding::boolean(false)) } });
    walker.continueIn(filled);
    if (Reference isEqual = walker.isUnreachable() || walker.hasFailed() ? Reference() : buildOperatorForStrings(walker, Opcode::StrictEq, left, right, origin))
        arms.append({ walker.currentBlock(), { isEqual } });
    std::array<Reference, 1> met;
    if (walker.hasFailed() || !walker.joinArms(arms.span(), walker.addBlock(), met))
        return true;
    walker.bind(equality.id(), met[0]);
    return true;
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

static LayoutID findClassOfAssignedValue(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    LayoutID only;
    for (LayoutID member : receivers.members) {
        if (walker.program().at(member).kind != LayoutKind::Instance)
            return { };
        LayoutID classLayout = declaredLayoutOf(walker.program(), member);
        if ((only && classLayout != only) || !isAssignedValue(walker, classLayout, key))
            return { };
        only = classLayout;
    }
    return only;
}

static std::optional<OptionSet<CellFact>> assignedValueCellFacts(const ReceiversOfTypedAccess& receivers)
{
    return smallestSufficientCellFacts([&](OptionSet<CellFact> facts) {
        return !possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove, facts, true) && facts.contains(CellFact::NoExtraProperties);
    });
}

bool looksAtSlotOfObjectBeingMade(const Walker& walker, const BodyValue& access)
{
    return isBeingMadeByBodyBeingReadInTypedAccess(walker, access.objectOperand());
}

static Reference checkClosureOf(Walker& walker, Reference found, FunctionID function, Origin origin, NOESCAPE const ScopedLambda<void(Reference test)>& continueIfCheckPasses)
{
    Draft test(Opcode::IsClosureOf, origin);
    walker.chooseConservativeFields(test);
    test.setFunction(function);
    test.setValueOperand(found);
    Reference isClosure;
    {
        Walker::SpecializedPreheaderScope preheaderScope(walker, walker.isInvariantInSpecializedVersion(found, Walker::IterationPurity::EffectFree), Walker::IterationPurity::EffectFree);
        isClosure = walker.build(WTF::move(test));
    }
    BlockID blockOfTest = walker.currentBlock();
    if (isClosure)
        continueIfCheckPasses(isClosure);
    if (!isClosure || !isClosure.isValue() || walker.isUnreachable())
        return { };
    bool isTestedInPreheader = walker.currentBlock() == blockOfTest;
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(found);
    proof.setTestOperand(isClosure);
    proof.setOperandIndex(OperandIndex::IsClosureOf::value);
    proof.setIsOnTakenEdge(true);
    Walker::SpecializedPreheaderScope preheaderScope(walker, isTestedInPreheader && walker.isInvariantInSpecializedVersion(isClosure, Walker::IterationPurity::EffectFree), Walker::IterationPurity::EffectFree);
    return walker.build(WTF::move(proof));
}

static std::optional<MemberConditionRecord> findAssignedMethodCondition(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    LayoutID only;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout = declaredLayoutOf(walker.program(), member);
        if ((only && classLayout != only) || !findChosenAssignedMethod(walker, classLayout, key) || !walker.state().isMemberAssigned(classLayout, key))
            return std::nullopt;
        only = classLayout;
    }
    return only ? std::optional(MemberConditionRecord { MemberConditionKind::MemberAssigned, only, ClassSide::Prototype, key }) : std::nullopt;
}

void lookAtCellInFrontOfStores(Walker& walker, std::span<const ValueID> stores, Reference object, NOESCAPE const ScopedLambda<void(Reference test)>& continueIfCheckPasses, std::span<const ValueID> accessesThatLookAtSlots, OptionSet<CellFact>* factsAsked)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers && factsAsked)
        return;
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

    Vector<PropertyKeyID, 2> keysAskedAmongExtras;
    OptionSet<CellFact> asked;
    Vector<MemberConditionRecord, 8> conditions;
    WordsListedOnce conditionsListed;
    Vector<std::pair<ValueID, FunctionID>, 2> methodsReadOffHolder;
    Vector<std::pair<PropertyKeyID, FunctionID>, 2> gettersReadOffHolder;
    bool someStoreCreates = false;
    bool asksWhatIsAboveObject = false;
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
        FunctionID half = findHalfRunBehindLooksForTypedAccess(walker, access, *receivers, key);
        if (!half)
            return false;
        if (!closureTableHoldsMethodForTypedAccess(walker, half))
            gettersReadOffHolder.append({ key, half });
        if (std::optional<OptionSet<CellFact>> askedByAccess = findAccessorCellFacts(walker, *receivers, key))
            asked.add(*askedByAccess);
        for (LayoutID member : receivers->members) {
            LayoutID classLayout;
            FunctionID getter;
            FunctionID setter;
            if (!findAccessorDeclaredAboveObjectsOf(walker, member, key, classLayout, getter, setter))
                continue;
            MemberConditionRecord condition = accessorCondition(walker, classLayout, key);
            if (!accessorConditionHolds(walker.state(), condition))
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
            if (LayoutID classLayout = !method && access.opcode() == Opcode::PropGet && nameMayBeAssigned(walker, key) && walker.isInLoop() ? findClassOfAssignedValue(walker, *receivers, key) : LayoutID()) {
                if (std::optional<OptionSet<CellFact>> askedByRead = assignedValueCellFacts(*receivers)) {
                    asked.add(*askedByRead);
                    if (!walker.state().isMemberAssigned(classLayout, key))
                        conditionsListed.appendUnlessListed(conditions, MemberConditionRecord { MemberConditionKind::MemberAssigned, classLayout, ClassSide::Prototype, key });
                    continue;
                }
            }
            if (!method) {
                if (findObjectPrototypeMember(walker, *receivers, field, key)) {
                    asked.add(queryObjectPrototypeRead(*receivers).value_or(OptionSet<CellFact>()));
                    continue;
                }
                if (std::optional<OptionSet<CellFact>> askedByRead = askAheadAboutReadThatFindsNothing(walker, *receivers, field, key)) {
                    asked.add(*askedByRead - cellFactsForKeyedLookup(walker, object, key));
                    if (readNeedsGuards(*receivers, *askedByRead))
                        askGuardsOfClasses(key, LookupAbove::OfRead);
                }
                continue;
            }
            if (!closureTableHoldsMethodForTypedAccess(walker, method))
                methodsReadOffHolder.append({ id, method });
            if (std::optional<OptionSet<CellFact>> askedByRead = askAheadAboutReadOfMethodAbove(walker, *receivers, key)) {
                if (walker.isInLoop() && askedByRead->contains(CellFact::NoExtraProperties) && !closureTableHoldsMethodForTypedAccess(walker, method)) {
                    askedByRead->remove(CellFact::NoExtraProperties);
                    if (!cellFactsForKeyedLookup(walker, object, key).contains(CellFact::NoExtraProperties))
                        keysAskedAmongExtras.appendIfNotContains(key);
                }
                asked.add(*askedByRead);
            }
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
            if (readNeedsGuards(*receivers, *askedByRead))
                askGuardsOfClasses(key, LookupAbove::OfRead);
            continue;
        }
        bool isDefinition = access.opcode() == Opcode::PropDefine;
        TypedStoreAskedAhead ahead = askAheadAboutTypedStore(walker, access, *receivers, field, key, isDefinition);
        bool slotIsAskedForValueInFront = ahead.creates && lookAsksForValueOfWrittenType(walker, *receivers, key) && std::ranges::any_of(accessesThatLookAtSlots, [&](ValueID earlier) {
            BodyValue look = walker.asWalked(index.value(earlier));
            return earlier < id && look.opcode() == Opcode::PropGet && look.key() == key;
        });
        if (slotIsAskedForValueInFront)
            ahead = { TypedForm::BuiltBehindFilledSlotAndLookAtCell, factsRequiredToOverwrite(*receivers, isDefinition), false };
        asked.add(ahead.askedOfCell);
        if (ahead.creates && !someStoreCreates)
            firstKeyCreated = key;
        someStoreCreates |= ahead.creates;
        asksWhatIsAboveObject |= ahead.asksWhatIsAboveObject;
        if (ahead.creates && !isDefinition && storeNeedsGuards(walker, *receivers, field, key, ahead.askedOfCell))
            askGuardsOfClasses(key, LookupAbove::OfStore);
    }

    Reference root = object.isValue() ? Reference::value(underlyingCellOf(walker.code(), object.id())) : object;
    bool asksForWholeWord = someStoreCreates && walker.state().cellFacts(root).contains(CellFact::IsLikelyAsAllocated)
        && (receivers->members.size() == 1 ? orderWordTellsKeysOfObjectsOf(walker, receivers->members[0]) : !!findFieldInKnownOrderOfEveryReceiver(walker, receivers->members.span(), firstKeyCreated, unorderedOrderWord));
    OptionSet<CellFact> knownByMark = cellFactsWithMark(walker, object) - walker.state().cellFacts(object);
    if (asked.contains(CellFact::HasLayoutPrototype) && knownByMark.contains(CellFact::HasLayoutPrototype))
        walker.count(StatisticID::PrototypesKnownByMarks);
    asked.remove(cellFactsWithMark(walker, object));
    if (factsAsked) {
        if (!asksForWholeWord)
            *factsAsked = asked;
        return;
    }
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
    if (!asksForWholeWord && !asked.containsAny(factsOfExtensibleObject) && asksForNoExtraProperties) {
        bool asksForFieldsInSlots = asked.contains(CellFact::NoFieldOutsideSlot);
        Draft test(Opcode::HasNoExtraProperties, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(asksForFieldsInSlots);
        if (!pass(WTF::move(test)))
            return;
        walker.state().recordCellFacts(object, asksForFieldsInSlots ? OptionSet<CellFact> { CellFact::NoExtraProperties, CellFact::NoFieldOutsideSlot } : OptionSet<CellFact> { CellFact::NoExtraProperties });
    } else if (!asksForWholeWord && !asked.containsAny(factsOfExtensibleObject) && asked.contains(CellFact::NoFieldOutsideSlot)) {
        asksForFieldsInSlotsBesidePrototype = asked.contains(CellFact::HasLayoutPrototype);
        if (!asksForFieldsInSlotsBesidePrototype) {
            Draft test(Opcode::HasNoFieldOutsideSlot, origin);
            test.setObjectOperand(object);
            if (!pass(WTF::move(test)))
                return;
            walker.state().recordCellFacts(object, CellFact::NoFieldOutsideSlot);
        }
    } else if (asksForWholeWord || asked.containsAny({ CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot })) {
        bool asksForFieldsInSlots = asksForWholeWord || asked.contains(CellFact::NoFieldOutsideSlot);
        bool asksForExtensible = asksForWholeWord || asked.contains(CellFact::IsExtensible);
        Draft test(Opcode::IsNotFrozen, origin);
        test.setObjectOperand(object);
        test.setIsExtensible(asksForExtensible);
        test.setHasNoFieldOutsideSlot(asksForFieldsInSlots);
        test.setHasNoExtraProperties(asksForWholeWord || asksForNoExtraProperties);
        if (!pass(WTF::move(test)))
            return;
        walker.state().recordCellFacts(object, asksForFieldsInSlots ? OptionSet<CellFact> { CellFact::NotFrozen, CellFact::NoFieldOutsideSlot } : OptionSet<CellFact> { CellFact::NotFrozen });
        if (asksForExtensible)
            walker.state().recordCellFacts(object, CellFact::IsExtensible);
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
    for (PropertyKeyID key : keysAskedAmongExtras) {
        if (asksForNoExtraProperties || asksForWholeWord)
            break;
        Draft test(Opcode::FindsNoMember, origin);
        walker.chooseConservativeFields(test);
        test.setObjectOperand(object);
        test.setKey(key);
        test.setReceivers(receivers->set);
        test.setAsksForNoExtraPropertyUnderKey(true);
        if (!pass(WTF::move(test)))
            return;
        noteKnown(walker, KnownNote::NoExtraPropertyUnderKey, object, { }, key.bits(), LocationMasks(AbstractHeapSet { AbstractHeap::NamedProperties }), 1, true);
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
    if (asksWhatIsAboveObject && !findKnown(walker, KnownNote::StoresPassAbove, { }, object, 0)) {
        Draft test(Opcode::PassesStoresAbove, origin);
        walker.chooseConservativeFields(test);
        test.setObjectOperand(object);
        if (!pass(WTF::move(test)))
            return;
        noteKnown(walker, KnownNote::StoresPassAbove, { }, object, 0, LocationMasks(AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks }), 1, true);
    }
    for (auto [key, getter] : gettersReadOffHolder) {
        std::optional<MemberConditionRecord> condition = findAssignedGetterCondition(walker, *receivers, key);
        if (!condition || walker.isUnreachable())
            return;
        Draft member(Opcode::GetAssignedMember, origin);
        walker.chooseConservativeFields(member);
        member.setConditions(singleElementSpan(*condition));
        Reference found = walker.build(WTF::move(member));
        if (!found || !found.isValue())
            return;
        Reference closure = checkClosureOf(walker, found, getter, origin, continueIfCheckPasses);
        if (!closure)
            return;
        noteKnown(walker, KnownNote::AssignedMemberClosure, walker.state().assignedMemberTest(condition->layout, key), { }, 0, declaredBoundInTable(Opcode::AreMembersUnchanged).reads, closure.bits());
    }
    for (auto [id, method] : methodsReadOffHolder) {
        BodyValue read = walker.asWalked(index.value(id));
        auto draftReadOffObject = [&] {
            Walker::StandIn readOffObject(walker, read.objectOperand(), object);
            return walker.draftOf(read);
        };
        Draft written = draftReadOffObject();
        if (walker.hasFailed() || walker.isUnreachable())
            return;
        Reference found;
        if (std::optional<MemberConditionRecord> assigned = findAssignedMethodCondition(walker, *receivers, written.key())) {
            Draft member(Opcode::GetAssignedMember, origin);
            walker.chooseConservativeFields(member);
            member.setConditions(singleElementSpan(*assigned));
            found = walker.build(WTF::move(member));
        } else {
            written.setObjectOperand(object);
            walker.chooseConservativeFields(written);
            Effects effects = Effects::ofReadByNameThatRunsNoCode();
            effects.writes.remove(AbstractHeap::MemberGuards);
            walker.letLeave(object);
            found = walker.build(WTF::move(written), &effects);
        }
        Reference lastName = found && found.isValue() ? found : Reference();
        for (Reference checked = lastName ? walker.state().checkedNameOf(lastName) : Reference(); checked; checked = walker.state().checkedNameOf(lastName))
            lastName = checked;
        if (lastName && lastName != found && singleFunctionOf(walker.types(), walker.types().at(walker.typeOf(lastName))) == method) {
            walker.bind(id, found);
            continue;
        }
        if (!found || !found.isValue())
            return;
        Reference closure = checkClosureOf(walker, found, method, origin, continueIfCheckPasses);
        if (!closure)
            return;
        walker.bind(id, found);
        walker.state().noteCheckedName(lastName, closure);
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

    constexpr OptionSet<CellFact> knownOfNewCell { CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot, CellFact::HasLayoutPrototype };
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
        if (!place || isListed.get(*place) || !mayBeCreatedByStore(plan.slot(layout, *place).representation))
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

static void appendKeysFilledByConstruction(const ThinSummary& summary, const SummaryConstruction& construction, Vector<PropertyKeyID, 32>& keys)
{
    keys.append(summary.keysFilledAtEnds.span().subspan(construction.firstKeyFilledAtEnd, construction.numberOfKeysFilledAtEnd));
    for (const SummaryConstruction::Key& defined : summary.keysOfConstructions.span().subspan(construction.firstKey, construction.numberOfKeys)) {
        if (defined.isFilled)
            keys.append(defined.key);
    }
}

static void appendKeysFilledByConstruction(const ThinSummary& summary, Vector<PropertyKeyID, 32>& keys)
{
    for (const SummaryConstruction& construction : summary.constructions) {
        if (construction.object == SummaryConstruction::Object::This)
            appendKeysFilledByConstruction(summary, construction, keys);
    }
}

static void appendKeysFilledByBases(const Walker& walker, Vector<PropertyKeyID, 32>& keys)
{
    const Program& program = walker.program();
    LayoutID constructed = program.at(walker.currentFunction()).constructedClass;
    for (LayoutID base = constructed ? program.at(constructed).base : LayoutID(); base; base = program.at(base).base) {
        FunctionID constructor = program.at(base).constructor;
        if (!constructor)
            continue;
        const ThinSummary& summary = walker.facts().summaries().at(constructor);
        for (const SummaryConstruction& construction : summary.constructions)
            appendKeysFilledByConstruction(summary, construction, keys);
    }
}

static FunctionID findMethodCalledOnThis(const Walker& walker, LayoutID layout, FunctionID caller, const BodyIndex& body, ValueID site)
{
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    if (FunctionID callee = link.calleeAt(caller, site))
        return callee;
    ValueID callee = valueBehindOtherNamesForTypedAccess(body, body.value(site).as<Opcode::Call>().calleeOperand());
    if (!callee || body.isParameter(callee) || body.opcodeOf(callee) != Opcode::PropGet)
        return { };
    return link.findWrittenMethod(walker.program(), layout, body.value(callee).key());
}

static void appendKeysStoredOnThis(const Walker& walker, LayoutID layout, FunctionID method, Vector<PropertyKeyID, 32>& keys, unsigned remainingDepth)
{
    std::optional<unsigned> thisParameter = walker.program().at(method).thisParameterIndex();
    const BodyIndex* body = walker.facts().bodies().find(method);
    if (!thisParameter || !body)
        return;
    auto isThis = [&](ValueID value) {
        ValueID origin = value ? valueBehindOtherNamesForTypedAccess(*body, value) : ValueID();
        return origin && body->isParameter(origin) && body->blockOf(origin) == entryBlock && origin.index() == *thisParameter;
    };
    for (uint32_t place = 0; place < body->numberOfValues(); ++place) {
        ValueID id(place);
        if (body->isParameter(id))
            continue;
        Opcode opcode = body->opcodeOf(id);
        if (opcode == Opcode::Call) {
            FunctionID callee = remainingDepth && isThis(body->value(id).as<Opcode::Call>().thisValueOperand()) ? findMethodCalledOnThis(walker, layout, method, *body, id) : FunctionID();
            if (callee)
                appendKeysStoredOnThis(walker, layout, callee, keys, remainingDepth - 1);
        } else if (storesByNameIntoSlots.contains(opcode)) {
            BodyValue store = body->value(id);
            if (store.key() && isThis(store.objectOperand()))
                keys.append(store.key());
        }
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
        } else if (FunctionID constructor = isAnswerOfCallOfBody ? walker.program().at(layout).constructor : FunctionID()) {
            walker.count(StatisticID::OrdersBehindConstructions);
            appendKeysFilledByConstruction(walker.facts().summaries().at(constructor), keysFoundForThisRow);
        } else if (written && !index.isParameter(written) && OpcodeSet { Opcode::SuperConstruct, Opcode::SuperConstructList }.contains(index.opcodeOf(written)))
            appendKeysFilledByBases(walker, keysFoundForThisRow);
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
            appendKeysFilledByConstruction(walker.facts().summaries().at(callee), keysFoundForThisRow);
            continue;
        }
        if (isThisBeingMade && !index.isParameter(id) && index.opcodeOf(id) == Opcode::Call && index.dominates(index.blockOf(id), blockOfRow)) {
            auto call = index.value(id).as<Opcode::Call>();
            bool isOnThis = call.thisValueOperand() && valueBehindOtherNamesAndJoinsForOrderWord(walker, index, call.thisValueOperand()) == written;
            if (FunctionID method = isOnThis ? findMethodCalledOnThis(walker, layout, walker.currentFunction(), index, id) : FunctionID()) {
                static constexpr unsigned maxDepthOfMethods = 2;
                walker.count(StatisticID::OrdersAfterMethods);
                appendKeysStoredOnThis(walker, layout, method, keysFoundForThisRow, maxDepthOfMethods);
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
        if (provesSlotFilledForTypedAccess(walker, valueInSlotForTypedAccess(walker, object, described), key)) {
            walker.count(StatisticID::KeyOrderChecksOfFilledSlotsRemoved);
            continue;
        }
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
    walker.state().recordCellFacts(object, { CellFact::NotFrozen, CellFact::IsExtensible, CellFact::NoFieldOutsideSlot });
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
        Draft finish(plan.layout(layout).marksOtherPrototypes && !walker.state().cellFacts(object).contains(CellFact::HasLayoutPrototype) ? Opcode::TryFinishBirth : Opcode::FinishBirth, origin);
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
            walker.noteVariableReadByCopy(variable, held);
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

static bool mayHoldAbsence(Walker& walker, Reference held)
{
    return walker.types().at(walker.typeOf(held)).primitives & (Primitive::Absent | Primitive::Unspecified);
}

static bool buildVariableStoreForTypedAccess(Walker& walker, const BodyValue& value)
{
    Reference stored = walker.referenceOf(value.valueOperand());
    VariableID variable = value.variable();
    bool mayRecordStore = walker.opensGuesses() && walker.mayUndoCopiedStore() && !walker.isVariableStoreUndoable(variable) && !walker.plan().deferredDefinitions().findDeferred(variable) && !walker.plan().variable(variable).function;
    if (Reference held = mayRecordStore ? walker.state().variable(variable) : Reference(); held && !mayHoldAbsence(walker, held))
        walker.recordUndoableVariableStore(variable, held);
    buildAsWrittenForTypedAccess(walker, value);
    walker.takePendingUndoableStore();
    TypeID proven = walker.positions().variable(TypeLevel::Proven, variable);
    bool readIsNarrower = walker.hasUninformativeType(stored) && proven && !walker.types().isUninformative(proven);
    walker.state().noteVariable(value.variable(), readIsNarrower ? Reference() : stored);
    if (!readIsNarrower)
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
    if (read && (walker.program().moduleInstances.isEmpty() || !walker.program().isInstanceConstant(field)))
        walker.noteEnvironmentField(environment, field, read);
    noteValueOfStableBinding(walker, environment, field, read);
    return true;
}

static Reference nameGivenByTestOfStoreForTypedAccess(const Walker& walker, FieldReference field, Reference stored)
{
    if (walker.isUnreachable())
        return { };
    const Code& code = walker.code();
    const ValueID last = code.lastOperationOf(walker.currentBlock());
    if (!last || code.at(last).opcode != Opcode::PutEnvironmentField || FieldReference { code.at(last).layout(), code.at(last).field() } != field)
        return { };
    auto inFrontOfConversions = [&](ValueID value) {
        while (!code.isConstant(value) && conversionsOfForm.contains(code.at(value).opcode))
            value = code.at(value).operand(0);
        return value;
    };
    const ValueID name = inFrontOfConversions(code.at(last).valueOperand());
    ValueID tested = name;
    while (!code.isConstant(tested) && code.at(tested).opcode == Opcode::Narrowed && Reference::value(tested) != stored)
        tested = inFrontOfConversions(code.at(tested).valueOperand());
    const bool isStored = !code.isConstant(tested) && Reference::value(tested) == stored && !walker.hasUninformativeType(Reference::value(name));
    return isStored ? Reference::value(name) : Reference();
}

static bool buildEnvironmentStoreForTypedAccess(Walker& walker, const BodyValue& value)
{
    Reference environment = walker.referenceOf(value.environmentOperand());
    if (!environment)
        return false;
    Reference stored = walker.referenceOf(value.valueOperand());
    FieldReference field { value.layout(), value.field() };
    if (Reference held = walker.mayUndoCopiedStore() && walker.opensGuesses() ? walker.state().environmentField(environment, field) : Reference(); held && !mayHoldAbsence(walker, held))
        walker.recordUndoableScopeStore(environment, field, held);
    buildAsWrittenForTypedAccess(walker, value);
    walker.takePendingUndoableStore();
    TypeID proven = walker.positions().environmentField(TypeLevel::Proven, field.layout, field.field);
    bool readIsNarrower = walker.hasUninformativeType(stored) && proven && !walker.types().isUninformative(proven);
    walker.noteEnvironmentField(environment, field, readIsNarrower ? nameGivenByTestOfStoreForTypedAccess(walker, field, stored) : stored);
    walker.noteStoreIntoSlotStoredFirst(value, environment);
    if (!readIsNarrower)
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
    if (TypedForm form = buildClassPrototypeReadForTypedAccess(walker, value, operands); form != TypedForm::None)
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

static bool isAccessorForCopyInTypedAccess(const Walker& walker, FieldReference field)
{
    if (field.field >= walker.program().at(field.layout).fields.size() || !isAccessorAmongOwnKeys(walker, field.layout, field.field))
        return false;
    const Field& declared = walker.program().at(field.layout).fields[field.field];
    OptionSet<FieldForm> form = walker.facts().field(field).formOfValue | declared.form;
    bool isEnumerableName = !form.contains(FieldForm::IsNotEnumerable) && declared.key && walker.program().at(declared.key).kind == PropertyKeyKind::Text;
    return isEnumerableName && !declared.computedKey && !declared.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods, FieldFlags::IsHidden });
}

static std::optional<Vector<FieldIndex, 8>> fieldsOfTargetForCopyInTypedAccess(const Walker& walker, const BodyValue& copy, LayoutID target, LayoutID source, std::span<const FieldIndex> order, Vector<FieldIndex, 8>& copied, AccessorsAmongOwnKeys accessors)
{
    const Program& program = walker.program();
    auto excludedKeys = copy.as<Opcode::CopyDataProperties>().excludedKeys();
    Vector<FieldIndex, 8> fieldsOfTarget;
    bool callsGetter = false;
    for (FieldIndex index : order) {
        if (callsGetter && !staysOwnKeyAfterCalls(walker, source, index))
            return std::nullopt;
        bool isAccessor = false;
        if (!isPlainPropertyForCopyInTypedAccess(walker, { source, index })) {
            isAccessor = accessors == AccessorsAmongOwnKeys::AreCalled && isAccessorForCopyInTypedAccess(walker, { source, index });
            if (!isAccessor)
                return std::nullopt;
        }
        PropertyKeyID key = program.at(source).fields[index].key;
        if (std::ranges::contains(excludedKeys, key))
            continue;
        callsGetter |= isAccessor;
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
        if (slotRead.holdsAccessor)
            return false;
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
    std::optional<Vector<FieldIndex, 8>> fields = properties->isEmpty() ? std::optional(Vector<FieldIndex, 8>()) : fieldsOfTargetForCopyInTypedAccess(walker, copy, layout, properties->first().field.layout, order.span(), copied, AccessorsAmongOwnKeys::AreRefused);
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
    return fieldsOfTargetForCopyInTypedAccess(walker, copy, layout, layoutOfSource, order, copied, AccessorsAmongOwnKeys::AreCalled);
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
            if (!copied.isEmpty()) {
                Draft words(Opcode::TryCopyFields, origin);
                words.setObjectOperand(empty);
                words.setSourceOperand(ofLayoutWritten);
                words.setLayout(layout);
                words.setSourceLayout(layoutOfSource);
                words.setFields(copied.span());
                walker.build(WTF::move(words));
                checkWordsBehindCopyForTypedAccess(walker, origin, empty, layout, fields->span());
            }
            walker.state().noteOrderWord(empty, plan.keyOrders().findOrderWord(layout, fields->span()));
            walker.jumpTo(behindCopy, singleElementSpan(empty));
        }
    } else {
        Vector<Reference, 8> values;
        for (FieldIndex index : copied) {
            if (plan.slot(layoutOfSource, index).holdsAccessor) {
                Reference value = walker.isUnreachable() ? Reference() : valueOfCopiedKey(walker, copy, ofLayoutWritten, layoutOfSource, index, origin);
                if (!value)
                    break;
                values.append(value);
                continue;
            }
            Draft read(Opcode::GetSlot, origin);
            fillSlotOperationForTypedAccess(read, ofLayoutWritten, *receivers, program.at(layoutOfSource).fields[index].key);
            values.append(walker.build(WTF::move(read)));
        }
        if (Reference object = walker.isUnreachable() || walker.hasFailed() ? Reference() : buildTargetOfCopyForTypedAccess(walker, origin, layout, fields->span(), values.span()))
            walker.jumpTo(behindCopy, singleElementSpan(object));
    }
    if (walker.hasFailed())
        return true;

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
    bool comesInSeveralOrders = layout && !someFieldMayBeLeftOut && walker.isInLoop() && walker.opensGuesses() && objectsOfLayoutComeInSeveralOrders(walker, layout);
    if (comesInSeveralOrders && source && !ownKeysInSight(walker, source) && buildCopyBehindTestsForTypedAccess(walker, copy, layout, true))
        return true;
    if (fillLiteralInSight(walker, copy))
        return true;
    if (layout && (buildCopyOfFreshObjectForTypedAccess(walker, copy, layout) || buildCopyBehindTestsForTypedAccess(walker, copy, layout)))
        return true;
    return defineOwnKeysOfSourceOnObjectThatStands(walker, copy);
}

static NEVER_INLINE bool buildTypedAccessOtherThanByName(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
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

bool buildTypedAccess(Walker& walker, const BodyValue& value)
{
    if (accessesByNameWithTypedForms.contains(value.opcode()))
        return buildAccessByNameForTypedAccess(walker, value, InputsOfTypedOperation(walker, nullptr)) == TypedForm::Built;
    return buildTypedAccessOtherThanByName(walker, value);
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
