/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations, accesses: where the base is proven to have one layout, or one family that agrees on the slot, an access under a constant key goes to the plan's slot.
// All else that must hold of the object Link has ruled out for the whole program (whatChangesForbid) or the state has seen here, or the access stays closed. At a suspension and at a throw nothing is left to undo:
// what was noted of slots, variables and environment fields ends by the declared effects of what is built. Withheld: every access by name is closed, and no slot is noted.
// A variable and a field of an environment are built as written either way. foldOperation() hands them here while this is withheld, so that the state still learns what they hold.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
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

FamilyID familyAroundSetOfLayouts(const Walker&, LayoutSetID); // AOTWalkUnionSlots.cpp

namespace {

struct ReceiversOfTypedAccess {
    LayoutSetID set;
    // Null: one layout.
    FamilyID family;
    // Of a set that is no family: the smallest family that holds it.
    FamilyID familyAround;
    // Those that have objects.
    Vector<LayoutID, 8> members;
    // To some object of some member.
    OptionSet<LayoutChange> possibleChanges;
};

enum class CoverageOfTypedAccess : uint8_t { NoReceiver, SomeReceivers, EveryReceiver };

struct FieldOfTypedAccess {
    CoverageOfTypedAccess coverage { CoverageOfTypedAccess::NoReceiver };
    // Under which the state knows the slot: the field of the lowest receiver.
    FieldReference place;
    // While the slot is filled, the property is a value with the attributes of an assignment, at the one location that all receivers share.
    bool liesInOneSlot { false };
    // Whatever is stored under the name fits the slot, so no store moves the field out to the extra properties.
    bool isMovedOutByNoStore { false };
    // The source says that every receiver has the property.
    bool isRequired { true };
    bool isStoredByProgram { true };
    bool takesStoredValue { true };
    // A method behind an optional field, a field that hides a method.
    bool isNamedAbove { false };
    // LinkedCell::isFilled in every receiver.
    bool isFilledByEveryConstruction { true };
    // LinkedCell::isFilledAtEveryRead in every receiver.
    bool isFilledAtEveryRead { true };
    // The key order alone says whether an object has the property: one that holds undefined may have no granule for it. A read, a test and a store by name are told of such a field:
    // without a granule it reads as undefined like an empty slot, and whoever asks whether it is there or stores into it goes by the key order. SlotEmitter::loadSlot(), isFilled(), SlotAccessEmitter::generatePutSlotOrAdd().
    bool mayBeListedWithoutStorage { false };
    // No constructor and no literal fills it: some store by name creates it in each object that has it.
    bool isAppended { false };
    // Each receiver has it from a store of its own constructor on `this`: LinkTables::isAppendedFieldRequiredByTypeOfThis().
    bool isAppendedByConstructors { true };
    // LinkedCell::isOfBirth in every receiver. False: `const node = createBaseNode(kind); node.name = name;`: the type says that the property is there, and a store by name outside of the maker puts it there.
    bool isOfBirth { true };
    // In every receiver the slot has a form with marks for absent, undefined and null. JSValue has none: there a look for "filled" and a load are all that there is.
    bool liesInFormWithMarks { true };
    // In every receiver that form holds numbers or booleans.
    bool liesInFormOfNumbersOrBooleans { true };
    // `flags: SymbolFlags`. Not `next: Thing | undefined`, and not a name that a store appends.
    bool isWrittenWithoutUndefinedAndNull { true };
};

// A family may have hundreds of layouts, and an access asks each of them.
struct KeptByTypedAccesses final : Walker::KeptForBuild {
    // By family. Nothing: not asked yet.
    Vector<std::optional<std::optional<ReceiversOfTypedAccess>>> receiversOfFamilies;
    // By family, key and whether the access goes by the key order, and by the type of what is stored.
    HashMap<std::pair<uint64_t, uint32_t>, FieldOfTypedAccess> fieldsOfFamilies;
    // A union of hundreds of layouts is asked about hundreds of names, and nearly every member is without the name. Listed once the family was asked about several.
    struct MembersByKey {
        uint32_t numberOfKeysAsked { 0 };
        bool areListed { false };
        // Places in ReceiversOfTypedAccess::members, ascending. A member below a row of written bases may have the name from there, and is asked whatever the name.
        Vector<uint32_t> askedAboutEveryKey;
        HashMap<uint32_t, Vector<uint32_t, 1>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> withKey;
    };
    // By family.
    Vector<MembersByKey> membersOfFamiliesByKey;
    // Who answers for what stands above the objects of a family, under a name: whoAnswersForWhatStandsAboveObjectsOf() of every member.
    struct AnswersAbove {
        bool somebodyAnswersForEveryMember { false };
        // Each once.
        Vector<LayoutID, 2> classesWhoseGuardsAnswer;
    };
    // By family, key and whether a read or a store looks.
    HashMap<uint64_t, AnswersAbove> answersAboveFamilies;
    // The same two of a wide set that is no family: by TypeArena::setOfRound() of it and the key, and by the rest of what is asked.
    HashMap<std::pair<uint64_t, uint64_t>, FieldOfTypedAccess> fieldsOfSets;
    HashMap<std::pair<uint64_t, uint64_t>, AnswersAbove> answersAboveSets;
    // By class: whether the classes from it up pass all that whoAnswersForWhatStandsAboveObjectsOf() asks of them whatever the name. What the class above answers is kept for the class below.
    enum class RowAbove : uint8_t { NotAsked, IsAsked, Passes, Fails };
    Vector<RowAbove> rowsAboveClasses;
    // By key of the program: some class has a member of its prototype object under it, by its record or as Link has seen.
    std::optional<BitVector> keysOfMembersOfPrototypes;
};

struct OperandsOfTypedAccess {
    // Null where the access is only asked about.
    Reference object;
    Reference stored;
    TypeID typeOfObject;
    // Null: the access stores nothing.
    TypeID typeOfStored;
    bool areOnlyTypes { false };
};

struct WhatChangeForbids {
    LayoutChange change;
    OptionSet<TypedAccess> forbids;
    // The change leaves a mark in the cell. Where the state knows the cell of the receiver to be without it, these are forbidden no longer.
    OptionSet<TypedAccess> allowedWhereCellIsSeen;
    FactOfCell seenOfCell { FactOfCell::NotFrozen };
    // The change ends the guard that reads ask of the class and the name: object/AOTClassGuards.h.
    OptionSet<TypedAccess> allowedWhereGuardOfReadsHolds;
};

} // anonymous namespace

// A PASSED TEST OF THE SHAPE RULES OUT NONE OF THE FIVE. The shape word is written where the cell is made (Object::create()) and never again: it says which slots the cell has, for good, and nothing of what has become of them.
// What sees a change is the slot. It is filled only while the property is a value of the object's own with the attributes of its field: Object::getOwnPropertySlot() itself answers with a filled slot and looks no further.
// So an access to a slot that the state has seen filled is forbidden by no change that empties the slot or leaves it alone. The sight ends with the next operation that may write the field: WalkState::invalidate().
// What the slot does not show, the order word does, which is written again and again: a look at it holds until something may write what the fact rests on (FactOfCell).
static constexpr OptionSet<TypedAccess> storesOfTypedAccesses { TypedAccess::StoresIntoFilledSlot, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::DefinesSlotThatMayBeEmpty };
static constexpr OptionSet<TypedAccess> accessesToSlotsThatMayBeEmpty { TypedAccess::ReadsSlotThatMayBeEmpty, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::DefinesSlotThatMayBeEmpty };
static constexpr WhatChangeForbids whatChangesForbid[] {
    // Object::setIntegrity() sets bits of the order word. The slots stay as they are, and a store into one would go through. Reads are as right as before.
    { LayoutChange::IsFrozen, storesOfTypedAccesses, storesOfTypedAccesses, FactOfCell::NotFrozen },
    // Object::tryAddAbsentExtraProperty() lays a name that is no field among the extra properties, behind a bit of the order word. Under the name of a field, tryCreateField() fills the slot.
    { LayoutChange::GainsProperty, { TypedAccess::FindsNoProperty }, { TypedAccess::FindsNoProperty }, FactOfCell::NoExtraProperties },
    // Object::deleteField() empties the slot, and an empty slot then says what is true: there is no such property.
    { LayoutChange::LosesProperty, { }, { } },
    // Object::setPrototypeKeepingShape() gives the cell another Structure, and with it a bit. A property of the object's own hides whatever lies above, and an empty slot hides nothing.
    // The bit speaks of the link of the object alone. What has become of the objects above it the guards of its class say, and whoever passes here by the bit asks them as well: lookupPassesAboveReceiversForTypedAccess().
    { LayoutChange::ChangesPrototype, { TypedAccess::ReadsSlotThatMayBeEmpty, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::FindsNoProperty, TypedAccess::FindsMethodAbove }, { TypedAccess::ReadsSlotThatMayBeEmpty, TypedAccess::StoresIntoSlotThatMayBeEmpty, TypedAccess::FindsNoProperty, TypedAccess::FindsMethodAbove }, FactOfCell::HasLayoutPrototype, { } },
    // Object::moveFieldToExtraProperties() empties the slot and sets a bit of the order word: the property is still there, and the empty slot says that it is not.
    // A slot that is filled holds a plain value that may be written, unless the object is frozen: whatever is read only, hidden from lists, fixed or an accessor has left its slot.
    // Under a name that is no field the accessor is a property of the object's own in front of the member, and the first object of a class that gets one ends the guard of the name.
    { LayoutChange::GetsAccessor, accessesToSlotsThatMayBeEmpty | OptionSet<TypedAccess> { TypedAccess::FindsMethodAbove }, accessesToSlotsThatMayBeEmpty, FactOfCell::NoFieldOutsideSlot, { TypedAccess::FindsMethodAbove } },
    // It lies among the extra properties, as whatever else is laid under a name that is no field.
    { LayoutChange::GetsAccessor, { TypedAccess::FindsNoProperty }, { TypedAccess::FindsNoProperty }, FactOfCell::NoExtraProperties },
};
// lookAtCellInFrontOfStores().
static constexpr FactOfCell factsThatLooksAtCellShow[] { FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot, FactOfCell::HasLayoutPrototype, FactOfCell::NoExtraProperties };
static constexpr OpcodeSet storesByNameIntoSlots { Opcode::PropSet, Opcode::PropDefine };
static constexpr OpcodeSet accessesThatMayStoreUnderNameForOrderWord { Opcode::PropSet, Opcode::PropDefine, Opcode::ElemSet, Opcode::ElemDefine };
static constexpr OpcodeSet callsForOrderWord { Opcode::Call, Opcode::CallList, Opcode::CallDirect, Opcode::Construct, Opcode::ConstructList };

bool possibleChangesForbid(OptionSet<LayoutChange> possibleChanges, TypedAccess access, OptionSet<FactOfCell> knownOfCell, bool guardOfReadsHolds)
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

static LinkedCell cellOfFieldForTypedAccess(const LinkTables& link, FieldReference field)
{
    const LinkedCell* cell = link.findCellOfField(field);
    return cell ? *cell : LinkedCell { };
}

// An index and a number in print may be elements, and `__proto__` is an accessor of Object.prototype that changes the object.
static bool isLookedUpAsNameForTypedAccess(const Program& program, PropertyKeyID key)
{
    if (!key || liesInThread(key) || !program.types.hasStableNumber(key))
        return false;
    switch (program.at(key).kind) {
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
    // `res.$exchange`: a name of the library's own, which no program can say.
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
        break;
    case PropertyKeyKind::PrivateName:
        return false;
    }
    NameClass nameClass = program.nameClassOf(key);
    return !mayBeElementKey(nameClass) && nameClass != NameClass::Proto;
}

// In front of a lookup under a symbol the run time may call a function of the library: prepareForLookup().
static bool findsMemberWithoutCodeForTypedAccess(const Program& program, PropertyKeyID key)
{
    return isLookedUpAsNameForTypedAccess(program, key) && program.at(key).kind == PropertyKeyKind::Text;
}

// A store under the name on an object, or on a prototype object on the way, flows into the member, which then names no function. Whatever else could put something in front of the member is one of the two changes, or lets the class escape.
MemberFound findMemberAboveObjectsOf(const Program& program, const LinkTables& link, LayoutID layout, PropertyKeyID key)
{
    if (!findsMemberWithoutCodeForTypedAccess(program, key) || !isFoundByShapeTest(program.at(layout)) || findField(program, layout, key))
        return { };
    if (possibleChangesForbid(possibleChangesForTypedAccess(link, layout), TypedAccess::FindsMethodAbove))
        return { };
    return link.findMember(program, layout, ClassSide::Prototype, key);
}

LayoutID classOfClassObject(const Program& program, FunctionID function)
{
    const Function& record = program.at(function);
    if (!record.constructedClass || !isClassConstructorOf(record.kind) || program.at(record.constructedClass).constructor != function)
        return { };
    return record.constructedClass;
}

// Whoever lays another object above a prototype object or a class object, defines a property of either by a descriptor or deletes one lets the class escape, with the classes below it. An escaped layout may change in every way.
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
    if (!findsMemberWithoutCodeForTypedAccess(program, key))
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
    if (side == ClassSide::Prototype)
        return link.findMember(program, classLayout, side, key);

    // A static field is a property of the class object that declares it, and hides what stands above. One under a computed key answers to a name that nobody knows.
    MemberFound found = link.findMember(program, classLayout, side, key);
    if (found.kind == MemberFound::Kind::Unknown)
        return found;
    for (LayoutID current = classLayout; current; current = program.at(current).base) {
        for (const StaticField& field : program.at(current).staticFields) {
            if (!field.key || field.key == key)
                return { };
        }
    }
    return found;
}

// ---- The receivers

static KeptByTypedAccesses& keptByTypedAccesses(Walker& walker)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByTypedAccesses();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<KeptByTypedAccesses>();
    return static_cast<KeptByTypedAccesses&>(*kept);
}

static std::optional<ReceiversOfTypedAccess> findReceiversAmongMembersForTypedAccess(Walker&, ReceiversOfTypedAccess&&);

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
        known = findReceiversAmongMembersForTypedAccess(walker, WTF::move(receivers));
    }
    return *known;
}

static std::optional<ReceiversOfTypedAccess> findReceiversOfTypedAccess(Walker& walker, TypeID proven)
{
    if (!proven)
        return std::nullopt;
    TypeArena& types = walker.types();
    Type type = types.at(proven);
    // Undefined and null throw, and any other primitive is looked up above its wrapper.
    if (!type.isJavaScriptValue() || type.primitives || !type.hasOnlyLayouts() || type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return std::nullopt;

    ReceiversOfTypedAccess receivers;
    receivers.set = type.layouts;
    if (!holdsOneMember(receivers.set)) {
        receivers.family = familiesForTypedAccesses(walker).familyOf(receivers.set);
        if (receivers.family)
            return receiversOfFamilyForTypedAccess(walker, receivers.family);
        receivers.familyAround = familyAroundSetOfLayouts(walker, receivers.set);
        if (!receivers.familyAround)
            return std::nullopt;
    }
    return findReceiversAmongMembersForTypedAccess(walker, WTF::move(receivers));
}

static std::optional<ReceiversOfTypedAccess> findReceiversAmongMembersForTypedAccess(Walker& walker, ReceiversOfTypedAccess&& receivers)
{
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    static constexpr size_t mostMembersAskedWithoutFamily = 16;
    std::span<const LayoutID> membersOfSet = membersOf(types, receivers.set);
    if (receivers.familyAround && membersOfSet.size() > mostMembersAskedWithoutFamily) {
        // What every member of the family has passed, the members of the set have passed.
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
            if (receivers.members.isEmpty())
                return std::nullopt;
            return receivers;
        }
    }
    for (LayoutID member : membersOfSet) {
        if (!plan.isInUse(member))
            continue;
        // Only a compiled object has slots. An instance of a class below a built-in class is a cell of that class, and a dictionary is an ordinary object.
        if (!plan.slots().holdsCompiledObjects(member) || !isFoundByShapeTest(program.at(member)))
            return std::nullopt;
        receivers.members.append(member);
        receivers.possibleChanges.add(possibleChangesForTypedAccess(link, member));
    }
    if (receivers.members.isEmpty())
        return std::nullopt;
    return receivers;
}

// ---- The field

// `stored`: the proven type of what a store brings. Null: the access stores nothing, or is only asked about.
// PutSlot and PutSlotOrAdd see to the granule of a field out of line that may be listed without one. No read does.
static FieldOfTypedAccess describeFieldAnewForTypedAccess(Walker&, const ReceiversOfTypedAccess&, PropertyKeyID, TypeID stored, bool goesByKeyOrder);

static FieldOfTypedAccess describeFieldOfTypedAccess(Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, TypeID stored, bool goesByKeyOrder = false)
{
    if (liesInThread(stored) || liesInThread(key))
        return describeFieldAnewForTypedAccess(walker, receivers, key, stored, goesByKeyOrder);
    if (!receivers.family) {
        // One layout, the most frequent receiver of all, and a few: the number of such a set lasts where it does not lie in the thread.
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

// False: every member is to be asked. Otherwise the places of all the members in which SlotPlan::findField() or findField() may find the key, ascending.
static bool listMembersThatMayHaveKeyForTypedAccess(Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, Vector<uint32_t, 16>& places)
{
    static constexpr size_t mostMembersAskedOneByOne = 16;
    static constexpr uint32_t mostKeysAskedOfEveryMember = 8;
    FamilyID family = receivers.family ? receivers.family : receivers.familyAround;
    if (!family || receivers.members.size() <= mostMembersAskedOneByOne || liesInThread(key))
        return false;
    // Some members of a family are asked through the list of the family.
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
    // Twenty members of a family of which three hundred are asked whatever the name: the twenty are the fewer.
    if (!receivers.family && members.askedAboutEveryKey.size() + found.size() >= receivers.members.size() / 4)
        return false;
    places.grow(members.askedAboutEveryKey.size() + found.size());
    std::ranges::merge(members.askedAboutEveryKey, found, places.begin());
    if (receivers.family)
        return true;
    // Both lists ascend by layout.
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
    // No store moves the field out to the extra properties.
    bool staysInSlot = true;
    Vector<uint32_t, 16> places;
    bool asksListedMembers = listMembersThatMayHaveKeyForTypedAccess(walker, receivers, key, places);
    size_t numberAsked = asksListedMembers ? places.size() : receivers.members.size();
    for (size_t asked = 0; asked < numberAsked; ++asked) {
        LayoutID member = receivers.members[asksListedMembers ? places[asked] : asked];
        std::optional<FieldIndex> index = slots.findField(program, member, key);
        if (!index) {
            // A row of classes may declare more fields than a layout lists.
            isDeclaredWithoutSlot |= !!findField(program, member, key);
            continue;
        }
        FieldReference reference { member, *index };
        if (!numberWithField++)
            field.place = findField(program, member, key).value_or(reference);

        const Layout& layout = program.at(member);
        // A field that Link appended has no record. It is a plain property that stores by name create, and no name of a member above the objects.
        const Field* recordOrNull = *index < layout.fields.size() ? &layout.fields[*index] : nullptr;
        const PlacedSlot& slot = slots.slot(member, *index);

        // The slot of an accessor holds its functions, that of a deferred closure a mark, and a store by name creates a field that is declared with other attributes beside its slot.
        bool mayBeListedWithoutStorage = slot.hasStorage() && slot.isOutOfLine() && slot.mayExistWithoutStorage;
        field.mayBeListedWithoutStorage |= mayBeListedWithoutStorage;
        bool mayLackStorage = !slot.hasStorage() || (mayBeListedWithoutStorage && !goesByKeyOrder);
        // A field that Link appended with a form: a read finds a value in its slot whatever its attributes are. A store may be refused, and creates the property in another form.
        if (OptionSet<FieldForm> form = recordOrNull ? OptionSet<FieldForm>() : walker.facts().field(reference).formOfValue; !form.isEmpty())
            holdsPlainValue &= !stored && !form.contains(FieldForm::IsAccessor);
        // A read of a slot that holds a mark has an operation of its own, which makes the closure: readOfFilledSlotForTypedAccess(). A store has none.
        holdsPlainValue &= (!recordOrNull || recordOrNull->form.isEmpty()) && !slot.holdsAccessor && !mayLackStorage && (slot.representation != SlotRepresentation::DeferredClosure || !stored);

        LinkedCell cell = cellOfFieldForTypedAccess(link, reference);
        bool takesEveryValue = slot.representation == SlotRepresentation::JSValue && (!slot.type || types.at(slot.type).isAnything());
        field.liesInFormWithMarks &= slot.representation != SlotRepresentation::JSValue && slot.representation != SlotRepresentation::DeferredClosure && canLeaveSlot(slot.representation);
        field.liesInFormOfNumbersOrBooleans &= !traitsOf(slot.representation).holdsCell;
        bool everyStoreFits = cell.everyWriterIsKnown && cell.proven && slot.type && types.isSubset(cell.proven, slot.type);
        staysInSlot &= takesEveryValue || everyStoreFits;
        field.isFilledByEveryConstruction &= cell.isFilled;
        field.isFilledAtEveryRead &= cell.isFilledAtEveryRead;
        // Link follows the constructions of the layout that has the constructor. What a layout below has from it is created by the same constructor.
        field.isOfBirth &= cell.isOfBirth || (recordOrNull && recordOrNull->flags.contains(FieldFlags::IsInherited) && cellOfFieldForTypedAccess(link, declarationOf(program, reference)).isOfBirth);
        if (stored)
            field.takesStoredValue &= takesEveryValue || (slot.type && types.isSubset(stored, slot.type));
        if (!recordOrNull) {
            field.isRequired &= link.isAppendedFieldRequiredBySomeWrittenType(reference);
            field.isAppended = true;
            field.isAppendedByConstructors &= link.isAppendedFieldRequiredByTypeOfThis(reference);
            field.isWrittenWithoutUndefinedAndNull = false;
            continue;
        }
        const Field& record = *recordOrNull;

        // A method of a literal is a property of the object's own. A method of a class lies above its instances, whose written type lists it all the same.
        bool isMethodOfClass = layout.kind == LayoutKind::Instance && record.flags.contains(FieldFlags::IsMethod);
        field.isStoredByProgram &= !isMethodOfClass && !record.flags.contains(FieldFlags::IsNeverWritten);
        // `function Response(this: $Response, ..)` stores `req` on one of its ways alone, and the type that the program writes for the object requires it.
        bool isRequiredByWrittenType = record.originLayout && record.originField && fieldAt(program, { record.originLayout, *record.originField }).flags.contains(FieldFlags::IsRequired);
        const auto& declared = (record.originLayout && record.originField ? fieldAt(program, { record.originLayout, *record.originField }) : record).declared;
        TypeID written = declared.written();
        // The front end lays undefined and null into every written type that holds references, and the flags say which of the two the text leaves out.
        uint16_t nullishAsWritten = written ? types.at(written).primitives & Primitive::UndefinedOrNull & ~(declared.has(PositionFlags::OmitsUndefined) ? Primitive::Undefined : 0) & ~(declared.has(PositionFlags::OmitsNull) ? Primitive::Null : 0) : 0;
        field.isWrittenWithoutUndefinedAndNull &= written && !types.at(written).isAnything() && !nullishAsWritten;
        // What a namespace or an enum exports, the block that makes the object stores.
        field.isRequired &= record.flags.contains(FieldFlags::IsRequired) || isRequiredByWrittenType || layout.kind == LayoutKind::Container;
        field.isNamedAbove |= isMethodOfClass || record.flags.contains(FieldFlags::IsAlsoInPrototype);
    }

    if (!numberWithField) {
        field.coverage = isDeclaredWithoutSlot ? CoverageOfTypedAccess::SomeReceivers : CoverageOfTypedAccess::NoReceiver;
        return field;
    }
    if (numberWithField < receivers.members.size()) {
        field.coverage = CoverageOfTypedAccess::SomeReceivers;
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

// ---- What a lookup finds above the object

// Above a record stands Object.prototype alone. Above an instance stand the members of its row of classes, which Link lists, and then Object.prototype. What Object.create() makes has whatever it was handed above it.
static bool nothingAnswersBelowObjectPrototypeForTypedAccess(const Program& program, const LinkTables& link, LayoutID id, PropertyKeyID key)
{
    const Layout& layout = program.at(id);
    if (layout.has(LayoutFlags::IsOfCreatedObject))
        return false;
    if (layout.kind == LayoutKind::Record)
        return !layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
    return link.findMember(program, id, ClassSide::Prototype, key).kind == MemberFound::Kind::Nothing;
}

static bool nothingAnswersAboveObjectsOfForTypedAccess(const Program& program, const LinkTables& link, LayoutID id, PropertyKeyID key)
{
    return !isOnObjectPrototype(program.nameClassOf(key)) && nothingAnswersBelowObjectPrototypeForTypedAccess(program, link, id, key);
}

static bool nothingAnswersAboveReceiversForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    // The name is asked about once, not once for each receiver. There is a receiver.
    const Program& program = walker.program();
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    return !isOnObjectPrototype(program.nameClassOf(key)) && std::ranges::all_of(receivers.members, [&](LayoutID member) {
        return nothingAnswersBelowObjectPrototypeForTypedAccess(program, link, member, key);
    });
}

// `F.prototype.m = function () { .. }`: the read finds a closure of a function that the program lays under the name above the object, or nothing, where the store has not run yet.
static TypeID typeLaidAboveObjectsOfForTypedAccess(const Program& program, const LinkTables& link, TypeArena& types, LayoutID id, PropertyKeyID key)
{
    const Layout& layout = program.at(id);
    if (!findsMemberWithoutCodeForTypedAccess(program, key) || isOnObjectPrototype(program.nameClassOf(key)) || layout.kind != LayoutKind::Instance || layout.has(LayoutFlags::IsOfCreatedObject))
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

// A value that does not fit the slot lies among the extra properties, and has arrived at the field like any other. The mark of an empty slot is no value, and no read by name finds it.
TypeID typeReadByNameOffObjectsOf(const Program& program, const LinkTables& link, TypeArena& types, LayoutID layout, PropertyKeyID key)
{
    if (!isLookedUpAsNameForTypedAccess(program, key) || !isFoundByShapeTest(program.at(layout)))
        return { };
    std::optional<FieldReference> field = findField(program, layout, key);
    // A field that Link appended has no record.
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
    if ((record && !record->form.isEmpty()) || !cell.everyWriterIsKnown || !cell.proven)
        return { };
    // `{ get total() { .. } }`: the read runs the getter, whatever form it has. What the getter returns has arrived at the field, beside what is stored for the setter.
    if (cell.holdsAccessorFromBirth) {
        const LinkedAccessorsOfField* laid = link.findAccessorsExpectedIn(*field);
        TypeID returned = laid && laid->getterIsProven && !program.at(laid->getter).isCoroutine() ? link.function(laid->getter).result : TypeID();
        return returned ? returned : cell.proven;
    }
    if (cell.isFilledAtEveryRead)
        return cell.proven;

    bool isNamedAbove = record && record->flags.containsAny({ FieldFlags::IsMethod, FieldFlags::IsAlsoInPrototype });
    if (isNamedAbove || possibleChangesForbid(possibleChangesForTypedAccess(link, layout), TypedAccess::ReadsSlotThatMayBeEmpty) || !nothingAnswersAboveObjectsOfForTypedAccess(program, link, layout, key))
        return { };
    return types.withPrimitives(types.withoutPrimitives(cell.proven, Primitive::Absent), Primitive::Undefined);
}

// An empty slot reads as undefined, is not there for `in`, and a store into it meets neither a setter nor a property that is read only. A definition asks nothing of what lies above the object.
static bool mayFindSlotEmptyInTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, TypedAccess access)
{
    if (!field.isMovedOutByNoStore || possibleChangesForbid(receivers.possibleChanges, access))
        return false;
    return access == TypedAccess::DefinesSlotThatMayBeEmpty || (!field.isNamedAbove && nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key));
}

static bool findsNothingForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    return field.coverage == CoverageOfTypedAccess::NoReceiver && !possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty)
        && nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key);
}

// What is asked ahead knows nothing of the state. A test finds the slot of a property that objects leave out empty as often as not.
// In front of a lookup under a symbol the run time may call a function of the library: prepareForLookup().
static bool paysToLookAtSlotForTypedAccess(const Walker& walker, const FieldOfTypedAccess& field, PropertyKeyID key, bool isRead = false)
{
    PropertyKeyKind kind = walker.program().at(key).kind;
    // The source says that the property is there, or every construction in sight gives it: `{ price: i, count: i & 3 }` in a program without types, where nothing is written of any field.
    // For a read alone: a store goes by more than whether the slot is filled (askAheadAboutTypedStore()), and which part of a set of layouts has typed forms for it hangs on this answer.
    // `this[kReplyHeaders] = {}` in `function Reply(..)`: the constructor gives the field to every object, though no layout lists it.
    // A FILLED slot is the object's own property, which a lookup finds first whatever is prepared in front of it: a read under a symbol that the program makes looks at the slot like any other, and goes as written where the slot is empty.
    bool isLikelyThere = field.isRequired || (isRead && (field.isAppended ? field.isAppendedByConstructors : field.isOfBirth));
    bool isNameOrSymbolThatIsRead = kind == PropertyKeyKind::Text || kind == PropertyKeyKind::EnginePrivateName || kind == PropertyKeyKind::ModulePrivateName || (isRead && kind == PropertyKeyKind::CreatedSymbol);
    return isLikelyThere && !field.mayBeListedWithoutStorage && isNameOrSymbolThatIsRead;
}

static TypedForm formBehindFilledSlotForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, TypedAccess access)
{
    if (!paysToLookAtSlotForTypedAccess(walker, field, key, access == TypedAccess::ReadsFilledSlot) || possibleChangesForbid(receivers.possibleChanges, access))
        return TypedForm::None;
    return TypedForm::BuiltBehindFilledSlot;
}

// Unknown: the receivers do not all come to the same.
static MemberFound findMemberAboveReceiversForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    MemberFound foundByAll;
    for (LayoutID member : receivers.members) {
        MemberFound found = findMemberAboveObjectsOf(walker.program(), linkTablesForTypedAccesses(walker), member, key);
        if (member != receivers.members[0] && (found.kind != foundByAll.kind || found.method != foundByAll.method || found.getter != foundByAll.getter || found.setter != foundByAll.setter))
            return { };
        foundByAll = found;
    }
    return foundByAll;
}

static MemberFound findMemberOfNamedValueForTypedAccess(const Walker& walker, TypeID proven, PropertyKeyID key)
{
    if (!proven)
        return { };
    Type type = walker.types().at(proven);
    ObjectID object = type.isObject() && type.isJavaScriptValue() ? singleObjectOf(type) : ObjectID();
    return object ? findMemberOfNamedObject(walker.program(), linkTablesForTypedAccesses(walker), walker.types().objects().at(object), key) : MemberFound();
}

FunctionID findGetterOfRead(Walker& walker, Opcode opcode, TypeID lookedAt, PropertyKeyID key)
{
    if (!key || !lookedAt || !walker.has(Mechanism::DirectCalls))
        return { };
    std::optional<ReceiversOfTypedAccess> receivers = opcode == Opcode::SuperGet ? std::nullopt : findReceiversOfTypedAccess(walker, lookedAt);
    MemberFound member = receivers ? findMemberAboveReceiversForTypedAccess(walker, *receivers, key) : findMemberOfNamedValueForTypedAccess(walker, lookedAt, key);
    if (member.kind == MemberFound::Kind::Accessor)
        return member.getter;
    return opcode == Opcode::PropGet ? findGetterExpectedInSlot(walker, lookedAt, key) : FunctionID();
}

// The lookup comes to a value on an object above the receiver, so it runs no code of the program and throws nothing. The run time may make the closure of a member when it is first looked for, and it files what it finds.
static Effects effectsOfReadingMethodForTypedAccess()
{
    Effects effects = Effects::unknownCode();
    effects.writes = effects.writes.restrictedTo(heapsWhoseWritesNobodySees);
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    effects.mayCallUnknownCode = false;
    effects.mayBeTerminated = false;
    return effects;
}

// ---- What every construction has filled

// A parameter of a block that one written edge enters, which is no edge to a handler, is another name of what the edge hands it.
// `N || (N = {})` where N is undefined for all to see, `if (never !== 1) self = this`: several edges are written and the constants of the body leave one that a run can take. Which, the scan decides, and it goes on following
// the object under the parameter (ThinSummary::otherNamesOfObjectsBeingMade). Whoever took that name for an object from elsewhere would believe of it what Link says to those outside its construction.
static ValueID valueBehindOtherNamesForTypedAccess(const BodyIndex& index, ValueID value, std::span<const OtherNameOfObjectBeingMade> namesFollowedByScan = { })
{
    while (value) {
        ValueID named;
        if (!index.isParameter(value)) {
            if (inputOperationsThatHandOn.contains(index.opcodeOf(value)))
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

// Link's word holds for whoever has the object from outside its construction. The body that is being read may be making the object itself:
// what it allocates, what `super()` answers it, `this` of an initializer of fields and of a function that `new` hands the object. It goes by what the allocation lists and by what the state has seen stored.
// Wherever such an object goes under any other name, the scan has counted that as a point at which it leaves.
static bool isFilledForBodyBeingReadInTypedAccess(const Walker& walker, const FieldOfTypedAccess& field, ValueID object, PropertyKeyID key)
{
    // A store of what does not fit the slot moves the field out to the extra properties, and empties the slot.
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
        return !isHandedObjectBeingMade || !thisParameter || index.blockOf(root) != entryBlock || root != index.block(entryBlock).parameter(*thisParameter);
    }
    BodyValue made = index.value(root);
    if (made.opcode() == Opcode::SuperConstruct || made.opcode() == Opcode::SuperConstructList)
        return false;
    if (made.opcode() == Opcode::Data) {
        DataID data = made.as<Opcode::Data>().data();
        const DataNode::Object* constant = data ? walker.program().at(data).root.tryAs<DataNode::Object>() : nullptr;
        return constant && std::ranges::contains(constant->members, key, &DataMember::key);
    }
    if (made.opcode() != Opcode::NewObject)
        return true;
    auto allocation = made.as<Opcode::NewObject>();
    std::optional<uint32_t> listed = allocation.layout() ? walker.program().at(allocation.layout()).findField(key) : std::nullopt;
    return listed && std::ranges::contains(allocation.fields(), *listed);
}

// `this` of an initializer of fields and of a function that `new` hands the object, what `super()` answers, what the body allocates: a store under a name is likely to be the first.
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

// ---- What the state has seen of the slot

static Reference valueInSlotForTypedAccess(Walker& walker, Reference object, const FieldOfTypedAccess& field)
{
    Reference held = walker.state().slot(object, field.place);
    TypeID type = held ? walker.typeOf(held) : TypeID();
    return type && !(walker.types().at(type).primitives & Primitive::Absent) ? held : Reference();
}

// An allocation or a store has laid the value there, or a read has found it. A read that takes an empty slot for undefined has found nothing.
static bool provesSlotFilledForTypedAccess(const Walker& walker, Reference held, PropertyKeyID key)
{
    if (!held)
        return false;
    if (!held.isValue())
        return true;
    const Operation& origin = walker.code().at(held.id());
    return (origin.opcode != Opcode::GetSlotOrUndefined && origin.opcode != Opcode::GetDeferredClosureSlotOrUndefined) || origin.key() != key;
}

// ---- Building

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

// `reader.next(await more())`. What a call through the slot needs of the closure is its scope, and that is read here, beside the slot: between the read of a callee and its call the program may suspend, or take the property away.
// findScopeOfClosureReadOffObjectInSight() finds it for the call. Nobody calls: nobody reads it, and the sweep takes it.
static void readScopeBesideSlotForTypedAccess(Walker& walker, Origin origin, Reference closure)
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

// The read of a method stays a read by name, typed by its row. It is there for whoever takes the function object: a callee with captures, a method that is handed on as a value.
static TypedForm buildReadOfMemberForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const MemberFound& member)
{
    switch (member.kind) {
    case MemberFound::Kind::Unknown:
    case MemberFound::Kind::Nothing:
        return TypedForm::None;
    case MemberFound::Kind::Method: {
        if (operands.areOnlyTypes)
            return TypedForm::Built;
        Draft read = walker.draftOf(value);
        walker.chooseConservativeFields(read);
        Effects effects = effectsOfReadingMethodForTypedAccess();
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
    // What is asked ahead is whether a proof of the receiver pays. Whether the getter takes the call is seen when the call is built.
    if (operands.areOnlyTypes)
        return TypedForm::BuiltAsCall;
    return callAccessor(walker, value, member.getter, operands.object, { }) ? TypedForm::Built : TypedForm::None;
}

static bool mayFindSlotEmptyBehindLooksInTypedRead(const Walker&, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID, OptionSet<FactOfCell> knownOfCell, const WalkState* stateAskedAboutGuards);
static std::optional<OptionSet<FactOfCell>> askAheadAboutReadOfSlotThatMayBeEmpty(const Walker&, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID);
static TypedForm buildReadOfMethodBehindLooksForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&, const ReceiversOfTypedAccess&, PropertyKeyID);
static TypedForm buildReadThatFindsNothingBehindLooksForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&, const ReceiversOfTypedAccess&, const FieldOfTypedAccess&, PropertyKeyID);
static TypedForm buildAccessToAccessorBehindLooksForTypedAccess(Walker&, const BodyValue&, const OperandsOfTypedAccess&, const ReceiversOfTypedAccess&, PropertyKeyID);
static bool orderWordShowsThatCellLacksFieldForTypedAccess(Walker&, Reference object, const ReceiversOfTypedAccess&, PropertyKeyID);

static TypedForm buildReadForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, PropertyKeyID key)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, operands.typeOfObject);
    if (!receivers) {
        if (TypedForm form = buildReadOfMemberForTypedAccess(walker, value, operands, findMemberOfNamedValueForTypedAccess(walker, operands.typeOfObject, key)); form != TypedForm::None)
            return form;
    }
    if (!receivers)
        return operands.areOnlyTypes || operands.object ? buildReadOfBuiltinMember(walker, value, operands.typeOfObject, operands.object) : TypedForm::None;
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (findsNothingForTypedAccess(walker, *receivers, field, key)) {
        if (!operands.areOnlyTypes)
            walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
        return TypedForm::Built;
    }
    if (field.coverage == CoverageOfTypedAccess::NoReceiver) {
        TypedForm form = buildReadOfMemberForTypedAccess(walker, value, operands, findMemberAboveReceiversForTypedAccess(walker, *receivers, key));
        if (form == TypedForm::None)
            form = buildReadOfMethodBehindLooksForTypedAccess(walker, value, operands, *receivers, key);
        if (form == TypedForm::None)
            form = buildAccessToAccessorBehindLooksForTypedAccess(walker, value, operands, *receivers, key);
        return form != TypedForm::None ? form : buildReadThatFindsNothingBehindLooksForTypedAccess(walker, value, operands, *receivers, field, key);
    }
    if (!field.liesInOneSlot)
        return TypedForm::None;

    bool mayFindSlotEmpty = mayFindSlotEmptyInTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsSlotThatMayBeEmpty);
    // The closed read is typed without undefined where every read finds the field filled, and no form may give more than the closed one.
    bool isFilledByConstructions = (field.isFilledAtEveryRead && field.isMovedOutByNoStore) || isFilledForBodyBeingReadInTypedAccess(walker, field, value.objectOperand(), key);
    if (operands.areOnlyTypes) {
        if (mayFindSlotEmpty || isFilledByConstructions) {
            // The read may make the closure and lay it into the slot: whoever looks at a loop counts on that: effectsOfTypedFormAhead().
            if (slotHoldsMarkForTypedAccess(walker, *receivers, key))
                walker.noteFormAskedAhead(Opcode::GetDeferredClosureSlotOrUndefined, KnowledgeLevel::Proven);
            return TypedForm::Built;
        }
        // A property that objects leave out: what may have become of an object that is out of sight, the object itself says, and behind its answer an empty slot IS undefined: empty costs what filled costs.
        // The form is more code than the closed read, so it stands where the text repeats: in a loop, or in a unit whose function may be entered again from below itself (a word of Link's on the text).
        // Outside of loops only where somebody has written what the receiver is: a guess at it fails too often elsewhere.
        TypedForm behindFilledSlot = formBehindFilledSlotForTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsFilledSlot);
        // A function that a call in a loop enters repeats as the loop does (LinkedFunction::isCalledInLoop).
        const LinkedFunction& functionOfUnit = walker.facts().linked().function(walker.unit().function);
        bool isWrittenInWhatRepeats = walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) && (functionOfUnit.isNamedInItsCycle || functionOfUnit.isCalledInLoop);
        bool paysToLookAtCell = walker.isInLoop() || isWrittenInWhatRepeats;
        if (behindFilledSlot != TypedForm::None || !paysToLookAtCell || !askAheadAboutReadOfSlotThatMayBeEmpty(walker, *receivers, field, key))
            return behindFilledSlot;
        return TypedForm::BuiltBehindLookAtCell;
    }

    Reference object = operands.object;
    Reference held = valueInSlotForTypedAccess(walker, object, field);
    bool isFilled = isFilledByConstructions || (provesSlotFilledForTypedAccess(walker, held, key) && !possibleChangesForbid(receivers->possibleChanges, TypedAccess::ReadsFilledSlot));
    if (!isFilled && !mayFindSlotEmpty)
        mayFindSlotEmpty = mayFindSlotEmptyBehindLooksInTypedRead(walker, *receivers, field, key, walker.state().factsOfCell(object), &walker.state());
    if (!isFilled && !mayFindSlotEmpty)
        return TypedForm::None;
    if (held && walker.has(Mechanism::Folding)) {
        walker.bind(value.id(), held);
        return TypedForm::Built;
    }

    // `render({ width })`, copied in line: `height`, `style`, `onDone` of an object that was made a moment ago. The word of the cell lists its keys and this one is not among them: the slot is empty, and here an empty slot is undefined.
    if (!isFilled && walker.has(Mechanism::Folding) && orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, key)) {
        walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
        return TypedForm::Built;
    }

    // Every construction fills the slot, so it is filled wherever anybody has the object.
    std::optional<Walker::ProofsHoldEverywhere> filledWhereverObjectIs;
    if (isFilledByConstructions)
        filledWhereverObjectIs.emplace(walker);
    bool holdsMark = slotHoldsMarkForTypedAccess(walker, *receivers, key);
    Opcode opcodeOfRead = isFilled ? (holdsMark ? Opcode::GetDeferredClosureSlot : Opcode::GetSlot) : (holdsMark ? Opcode::GetDeferredClosureSlotOrUndefined : Opcode::GetSlotOrUndefined);
    Reference read = walker.build(draftOfSlotOperationForTypedAccess(walker, value, opcodeOfRead, object, *receivers, key));
    readScopeBesideSlotForTypedAccess(walker, walker.originOf(value), read);
    walker.bind(value.id(), read);
    if (read)
        walker.state().noteSlot(object, field.place, read);
    return TypedForm::Built;
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
    bool isFilledByConstructions = isFilledForBodyBeingReadInTypedAccess(walker, field, value.objectOperand(), key);
    if (operands.areOnlyTypes)
        return mayFindSlotEmpty || isFilledByConstructions ? TypedForm::Built : formBehindFilledSlotForTypedAccess(walker, *receivers, field, key, TypedAccess::ReadsFilledSlot);

    Reference object = operands.object;
    bool isFilled = isFilledByConstructions
        || (provesSlotFilledForTypedAccess(walker, valueInSlotForTypedAccess(walker, object, field), key) && !possibleChangesForbid(receivers->possibleChanges, TypedAccess::ReadsFilledSlot));
    if (!isFilled && !mayFindSlotEmpty)
        return TypedForm::None;
    isFilled |= everyReceiverKeepsFieldInSlotThatIsNeverEmpty(walker, receivers->members.span(), key);
    if (!isFilled && walker.has(Mechanism::Folding) && orderWordShowsThatCellLacksFieldForTypedAccess(walker, object, *receivers, key)) {
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

// ---- Stores. A store into a filled slot overwrites a plain value of the object's own, and asks that the object is not frozen. A store into an empty slot creates the property:
// the object has to take new properties, the property must not lie elsewhere in the object, and of an assignment nothing above the object may take the store.

static bool mayOverwriteInTypedStore(const ReceiversOfTypedAccess& receivers, OptionSet<FactOfCell> knownOfCell)
{
    return !possibleChangesForbid(receivers.possibleChanges, TypedAccess::StoresIntoFilledSlot, knownOfCell);
}

// parentNamedByClassRecord() of image/AOTClassRecords.cpp. False: above the class stands what no record of the program describes.
static bool findParentNamedByClassRecordForTypedAccess(const Walker& walker, LayoutID classLayout, LayoutID& parent)
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
        case BaseState::BuiltinClass:
            return false;
        }
    }
    return parent || !layout.has(LayoutFlags::HasHeritage);
}

namespace {

// A store passes a plain value that may be written. A read finds it.
enum class LookupAbove : uint8_t { OfStore, OfRead };

enum class WhoAnswersForWhatStandsAbove : uint8_t {
    Nobody,
    // While the object stands where its layout says, only Object.prototype stands above it, which has no such name and gets none.
    NothingStandsThere,
    // The guard of the class and the name, that which stores ask or that which reads ask: object/AOTClassGuards.h.
    GuardOfClass,
};

} // anonymous namespace

// While the guard holds, each prototype object from that of the class up to the last that the records name has nothing under the name, or a plain value that may be written, beside what its record declares.
// So no record on the way may declare a member under the name, or under a key that nobody knows. The run time looks at the objects themselves where the guard begins, and hears of every change from then on.
// Above the last of them stands Object.prototype. The guards of a function that may be evaluated again, or whose closures hold a capture, never begin: classOfLayoutRecordIsEvaluatedOnce() of image/AOTLayoutTables.cpp.
// The guard that reads ask holds while each of them has nothing at all under the name beside what its record declares.
// The run time refuses every new name on Object.prototype, but those that a test run has opened: WriterOfLinkLayouts::objectPrototypeMayGainName(). No look at an object and no guard of a class sees what comes to lie there.
static bool objectPrototypeMayGainNameForTypedAccess(const Program& program)
{
    const BuiltinTable& builtins = program.builtins();
    BuiltinID objectPrototype = builtins.canonicalID(Builtins::ObjectPrototype);
    if (!builtins.at(objectPrototype).flags.containsAny({ BuiltinFlag::IsLockedObject, BuiltinFlag::IsClosedToOtherNames }) || !builtins.openNamesOf(objectPrototype).empty() || !builtins.hasLockedPrototypeLink(objectPrototype))
        return true;
    return program.isSomeNameOpenedOn(objectPrototype);
}

// What whoAnswersForWhatStandsAboveObjectsOf() asks of each class on the way up, but for the name.
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
        // A row that comes back to a class that is being asked has no end.
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

static bool someClassHasMemberOfPrototypeUnderForTypedAccess(const Walker& walker, KeptByTypedAccesses& kept, PropertyKeyID key)
{
    // The table of the program lacks it, so no record names it.
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

// So many classes are gone through sooner than anything is looked up.
static constexpr unsigned classesGoneThroughBeforeRowIsLookedUp = 8;

static WhoAnswersForWhatStandsAbove whoAnswersForWhatStandsAboveObjectsOf(const Walker& walker, LayoutID member, PropertyKeyID key, LookupAbove lookup, LayoutID& classLayout, ASCIILiteral* whyNobody = nullptr)
{
    auto nobody = [&](ASCIILiteral why) {
        if (whyNobody)
            *whyNobody = why;
        return WhoAnswersForWhatStandsAbove::Nobody;
    };
    const Program& program = walker.program();
    if (objectPrototypeMayGainNameForTypedAccess(program))
        return nobody("a test run has opened names on Object.prototype"_s);
    if (program.at(member).kind == LayoutKind::Record)
        return nothingAnswersAboveObjectsOfForTypedAccess(program, linkTablesForTypedAccesses(walker), member, key) ? WhoAnswersForWhatStandsAbove::NothingStandsThere : nobody("something above a literal's object may answer"_s);

    const LinkTables& link = linkTablesForTypedAccesses(walker);
    classLayout = declaredLayoutOf(program, member);
    if (isOnObjectPrototype(program.nameClassOf(key)))
        return nobody("Object.prototype has the name"_s);
    // No record tells what stands above such an object. The run time looks for itself where the first of them is linked, under each name that stores ask about, and hears of every change from then on: ClassTable::defineClassOfCreatedObjects().
    if (program.at(member).has(LayoutFlags::IsOfCreatedObject))
        return lookup == LookupAbove::OfStore && walker.plan().hasClassRecord(classLayout) ? WhoAnswersForWhatStandsAbove::GuardOfClass : nobody("the layout is of objects made by Object.create"_s);
    if (!walker.facts().isEvaluatedAtMostOnce(classLayout))
        return nobody("the class may be evaluated again"_s);
    size_t numberOfClassesLeft = program.layouts.size();
    unsigned numberOfClassesGoneThrough = 0;
    for (LayoutID current = classLayout; current;) {
        // Most names are those of fields alone. Of such a name the rest of a long row is asked what it is asked whatever the name: a store in each class of a row went up the whole row.
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
        // Where the guards begin, the run time looks at each prototype object of the row and ends them under every name at which something stands that the RECORD of the class does not declare (ClassTable::armMemberGuards()).
        // A member that is as its record declares it ends none. So whatever the records declare is asked about here, of the table that they are written from (image/AOTClassRecords.cpp).
        for (const ClassMember& declared : walker.facts().membersOf(current)) {
            if (declared.side == ClassSide::Prototype && (!declared.key || declared.key == key))
                return nobody("a record on the way up declares the name, or a member under a computed key"_s);
        }
        // What Link has seen defined saves a test that never passes: an accessor takes the store, under a computed key may stand one, and a method takes none.
        for (const LinkedMember& declared : std::ranges::equal_range(link.members, current, { }, &LinkedMember::classLayout)) {
            if (declared.side == ClassSide::Prototype && (!declared.key || (declared.key == key && (lookup == LookupAbove::OfRead || declared.kind != MethodKind::Method))))
                return nobody("Link has seen a member defined under the name, or under a computed key"_s);
        }
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return nobody("the row of classes goes on above the program's own"_s);
        current = parent;
    }
    return WhoAnswersForWhatStandsAbove::GuardOfClass;
}

// `stateAskedAboutGuards`: null where the question is whether tests could show it.
static bool lookupPassesAboveReceiversForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, LookupAbove lookup, const WalkState* stateAskedAboutGuards)
{
    if (field.isNamedAbove)
        return false;
    if (!receivers.possibleChanges.contains(LayoutChange::ChangesPrototype)) {
        if (nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key))
            return true;
        // A read finds what lies above. A store goes on to the guards: storeAsksGuardsWherePrototypeStaysForTypedAccess().
        if (lookup == LookupAbove::OfRead)
            return false;
    }
    auto askEachMember = [&] {
        KeptByTypedAccesses::AnswersAbove answers;
        answers.somebodyAnswersForEveryMember = std::ranges::all_of(receivers.members, [&](LayoutID member) {
            LayoutID classLayout;
            WhoAnswersForWhatStandsAbove who = whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, lookup, classLayout);
            if (who == WhoAnswersForWhatStandsAbove::GuardOfClass && !answers.classesWhoseGuardsAnswer.contains(classLayout))
                answers.classesWhoseGuardsAnswer.append(classLayout);
            return who != WhoAnswersForWhatStandsAbove::Nobody;
        });
        return answers;
    };
    auto answerBy = [&](const KeptByTypedAccesses::AnswersAbove& answers) {
        if (!answers.somebodyAnswersForEveryMember || !stateAskedAboutGuards)
            return answers.somebodyAnswersForEveryMember;
        return std::ranges::all_of(answers.classesWhoseGuardsAnswer, [&](LayoutID classLayout) {
            return lookup == LookupAbove::OfRead ? stateAskedAboutGuards->guardOfReadsHolds(classLayout, key) : stateAskedAboutGuards->storesPassAbove(classLayout, key);
        });
    };
    if (receivers.family && !liesInThread(key)) {
        auto& answersAboveFamilies = keptByTypedAccesses(const_cast<Walker&>(walker)).answersAboveFamilies;
        uint64_t asked = static_cast<uint64_t>(receivers.family.index() + 1) << 33 | static_cast<uint64_t>(key.bits()) << 1 | (lookup == LookupAbove::OfRead);
        auto known = answersAboveFamilies.find(asked);
        if (known == answersAboveFamilies.end())
            known = answersAboveFamilies.add(asked, askEachMember()).iterator;
        return answerBy(known->value);
    }
    if (TypeArena::SetOfRound round = liesInThread(key) ? TypeArena::SetOfRound() : walker.types().setOfRound(receivers.set); round.isKeptForBuild) {
        auto& answersAboveSets = keptByTypedAccesses(const_cast<Walker&>(walker)).answersAboveSets;
        std::pair<uint64_t, uint64_t> asked { static_cast<uint64_t>(round.word) << 32 | key.bits(), lookup == LookupAbove::OfRead };
        auto known = answersAboveSets.find(asked);
        if (known == answersAboveSets.end())
            known = answersAboveSets.add(asked, askEachMember()).iterator;
        return answerBy(known->value);
    }
    return std::ranges::all_of(receivers.members, [&](LayoutID member) {
        LayoutID classLayout;
        switch (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, lookup, classLayout)) {
        case WhoAnswersForWhatStandsAbove::Nobody:
            return false;
        case WhoAnswersForWhatStandsAbove::NothingStandsThere:
            return true;
        case WhoAnswersForWhatStandsAbove::GuardOfClass:
            if (!stateAskedAboutGuards)
                return true;
            return lookup == LookupAbove::OfRead ? stateAskedAboutGuards->guardOfReadsHolds(classLayout, key) : stateAskedAboutGuards->storesPassAbove(classLayout, key);
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

// `Reply.prototype.sent = true`, and `this.sent = false` in the constructor: the objects stay where their layout says, and something lies above them under the name.
// A store passes a plain value that may be written, and whether it still is one the guard that stores ask says. No look at the cell stands for that.
static bool storeAsksGuardsWherePrototypeStaysForTypedAccess(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    return !field.isNamedAbove && !receivers.possibleChanges.contains(LayoutChange::ChangesPrototype) && !nothingAnswersAboveReceiversForTypedAccess(walker, receivers, key);
}

static bool mayCreateInTypedStore(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, bool isDefinition, OptionSet<FactOfCell> knownOfCell, const WalkState* stateAskedAboutGuards)
{
    bool emptySlotSaysThatPropertyIsMissing = field.isMovedOutByNoStore || knownOfCell.contains(FactOfCell::NoFieldOutsideSlot);
    if (!emptySlotSaysThatPropertyIsMissing || possibleChangesForbid(receivers.possibleChanges, isDefinition ? TypedAccess::DefinesSlotThatMayBeEmpty : TypedAccess::StoresIntoSlotThatMayBeEmpty, knownOfCell))
        return false;
    // A definition asks nothing of what lies above the object.
    return isDefinition || lookupPassesAboveReceiversForTypedAccess(walker, receivers, field, key, LookupAbove::OfStore, stateAskedAboutGuards);
}

// An empty slot reads as undefined: the property lies nowhere else in the object, and nothing above the object answers.
static bool mayFindSlotEmptyBehindLooksInTypedRead(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, OptionSet<FactOfCell> knownOfCell, const WalkState* stateAskedAboutGuards)
{
    bool emptySlotSaysThatPropertyIsMissing = field.isMovedOutByNoStore || knownOfCell.contains(FactOfCell::NoFieldOutsideSlot);
    if (!emptySlotSaysThatPropertyIsMissing || possibleChangesForbid(receivers.possibleChanges, TypedAccess::ReadsSlotThatMayBeEmpty, knownOfCell))
        return false;
    return lookupPassesAboveReceiversForTypedAccess(walker, receivers, field, key, LookupAbove::OfRead, stateAskedAboutGuards);
}

// The fewest facts with which the table lets an access pass. Nothing: no look at the cell helps.
static std::optional<OptionSet<FactOfCell>> fewestFactsOfCellWith(NOESCAPE const Invocable<bool(OptionSet<FactOfCell>)> auto& passes)
{
    constexpr unsigned numberOfFacts = std::size(factsThatLooksAtCellShow);
    for (unsigned size = 0; size <= numberOfFacts; ++size) {
        for (unsigned choice = 0; choice < 1u << numberOfFacts; ++choice) {
            if (static_cast<unsigned>(std::popcount(choice)) != size)
                continue;
            OptionSet<FactOfCell> facts;
            for (unsigned index = 0; index < numberOfFacts; ++index) {
                if (choice >> index & 1)
                    facts.add(factsThatLooksAtCellShow[index]);
            }
            if (passes(facts))
                return facts;
        }
    }
    return std::nullopt;
}

static std::optional<OptionSet<FactOfCell>> askAheadAboutReadOfSlotThatMayBeEmpty(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    return fewestFactsOfCellWith([&](OptionSet<FactOfCell> facts) {
        return mayFindSlotEmptyBehindLooksInTypedRead(walker, receivers, field, key, facts, nullptr);
    });
}

// ---- A name that no layout lists

static bool findParentNamedByClassRecordForTypedAccess(const Walker&, LayoutID, LayoutID& parent);
static bool isReadForCallAloneInTypedAccess(const Walker&, const BodyValue&);

// `Object.defineProperty(F.prototype, "f", { get() { .. } })`, `F.prototype.on = function () { .. }`: statements in sight lay something under the name on a prototype object of the row, or under a key that nobody knows.
// No record declares that, so the guard that reads ask of the class and the name would never hold, and the read finds something.
static bool statementsLayNameAboveObjectsOfForTypedAccess(const Walker& walker, LayoutID member, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (program.at(member).kind != LayoutKind::Instance)
        return false;
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = declaredLayoutOf(program, member); current;) {
        if (!numberOfClassesLeft--)
            return true;
        for (const LinkedNameOnPrototype& name : std::ranges::equal_range(namesOnPrototypes, current, { }, &LinkedNameOnPrototype::layout)) {
            if (!name.key || name.key == key)
                return true;
        }
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return true;
        current = parent;
    }
    return false;
}

// `if (x.aborted === true)`, `options.signal`, `error.code`, off objects that nobody gives such a property: no receiver has a field for the name, and nothing above them declares it.
// The read finds nothing: the object has no property but its fields, which its order word says, and the lookup passes whatever stands above it.
static bool findsNothingBehindLooksInTypedRead(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, OptionSet<FactOfCell> knownOfCell, const WalkState* stateAskedAboutGuards)
{
    const Program& program = walker.program();
    if (field.coverage != CoverageOfTypedAccess::NoReceiver || !isLookedUpAsNameForTypedAccess(program, key) || program.at(key).kind != PropertyKeyKind::Text || !findsMemberWithoutCodeForTypedAccess(program, key))
        return false;
    if (possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty, knownOfCell))
        return false;
    // The plan may have a place for a name that the record does not list.
    const SlotPlan& slots = walker.plan().slots();
    for (LayoutID member : receivers.members) {
        if (slots.findField(program, member, key) || statementsLayNameAboveObjectsOfForTypedAccess(walker, member, key))
            return false;
    }
    return lookupPassesAboveReceiversForTypedAccess(walker, receivers, field, key, LookupAbove::OfRead, stateAskedAboutGuards);
}

static std::optional<OptionSet<FactOfCell>> askAheadAboutReadThatFindsNothing(const Walker& walker, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    // All but the table is asked once: with every fact at hand.
    OptionSet<FactOfCell> everyFact;
    for (FactOfCell fact : factsThatLooksAtCellShow)
        everyFact.add(fact);
    if (!findsNothingBehindLooksInTypedRead(walker, receivers, field, key, everyFact, nullptr))
        return std::nullopt;
    return fewestFactsOfCellWith([&](OptionSet<FactOfCell> facts) {
        return !possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsNoProperty, facts);
    });
}

static TypedForm buildReadThatFindsNothingBehindLooksForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key)
{
    if (value.opcode() != Opcode::PropGet || value.isFreeName())
        return TypedForm::None;
    if (operands.areOnlyTypes) {
        // The looks and the arm as written are more code than the closed read, so they stand where the text repeats, as those of a field that is left out do. Outside of loops only where nobody has to guess at the receiver:
        // it is written, or proven already.
        // `this.emit("data", v)`: nobody calls what he expects not to be there: something stands above the object that no record tells of.
        if (isReadForCallAloneInTypedAccess(walker, value))
            return TypedForm::None;
        bool paysToLookAtCell = walker.isInLoop();
        if (!paysToLookAtCell) {
            const LinkedFunction& functionOfUnit = walker.facts().linked().function(walker.unit().function);
            Reference bound = functionOfUnit.isNamedInItsCycle || functionOfUnit.isCalledInLoop ? walker.referenceOf(value.objectOperand()) : Reference();
            TypeID proven = bound ? walker.typeOf(bound) : TypeID();
            paysToLookAtCell = bound && (walker.program().at(walker.currentFunction()).has(FunctionFlags::IsChecked) || (proven && walker.types().isSubset(walker.types().withoutNullish(proven), operands.typeOfObject)));
        }
        // What code IN SIGHT does to such objects it has likely done to this one, and the looks would fail: `Object.setPrototypeOf(root, proto)`, `Object.assign(target, ..)`, `EventEmitter.call(this)`.
        // The reading in which unseen code only reads and calls says what code in sight does. The looks are there for what unseen code may have done.
        const LinkTables& held = walker.facts().held();
        paysToLookAtCell = paysToLookAtCell && std::ranges::none_of(receivers.members, [&](LayoutID member) {
            // `inherits(Source, EventEmitter)`: more stands above the objects than the records say, and the guard of the name would never hold.
            if (walker.program().at(member).kind == LayoutKind::Instance && (member.index() >= held.layouts.size() || !held.layout(member).listsEveryMemberAbove))
                return true;
            return possibleChangesForbid(possibleChangesForTypedAccess(held, member), TypedAccess::FindsNoProperty);
        });
        return paysToLookAtCell && askAheadAboutReadThatFindsNothing(walker, receivers, field, key) ? TypedForm::BuiltBehindLookAtCell : TypedForm::None;
    }

    const WalkState& state = walker.state();
    if (!findsNothingBehindLooksInTypedRead(walker, receivers, field, key, state.factsOfCell(operands.object), &state))
        return TypedForm::None;
    if (walker.checksProofs()) {
        Origin origin = walker.originOf(value);
        walker.checkFactsRememberedOfCell(operands.object, origin);
        Vector<MemberConditionRecord, 8> remembered;
        for (LayoutID member : receivers.members) {
            LayoutID classLayout;
            if (receivers.possibleChanges.contains(LayoutChange::ChangesPrototype) && whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, LookupAbove::OfRead, classLayout) == WhoAnswersForWhatStandsAbove::GuardOfClass)
                remembered.append(MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key });
        }
        if (!remembered.isEmpty() && !walker.isUnreachable()) {
            Draft test(Opcode::AreMembersUnchanged, origin);
            test.setConditions(remembered.span());
            Reference holds = walker.build(WTF::move(test));
            if (holds && holds.isValue() && !walker.isUnreachable())
                walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::WhatIsRemembered, origin);
        }
    }
    walker.bind(value.id(), walker.constant(Draft(Opcode::Undefined)));
    return TypedForm::Built;
}

// ---- A method above an object that is out of sight

// What a lookup under the key comes to above the objects of the layout while the guard that reads ask of the class and the name holds: the member that the first record of the row declares there, from the object's own class upward.
// It goes by the table that the records are written from: a method that `class` writes, or the one function that statements in sight lay on the prototype object of a constructor function, which the record only expects.
// Whether it lies there the guard says, not the table: that of an expected member begins where the first object is made and a closure of the function lies under the name.
// `whyNone`, for `testaot why-closed`: what stood in the way.
static FunctionID findMethodDeclaredAboveObjectsOf(const Walker& walker, LayoutID member, PropertyKeyID key, LayoutID& classLayout, ASCIILiteral* whyNone = nullptr)
{
    auto none = [&](ASCIILiteral why) {
        if (whyNone)
            *whyNone = why;
        return FunctionID();
    };
    const Program& program = walker.program();
    if (program.at(member).kind != LayoutKind::Instance || program.at(member).has(LayoutFlags::IsOfCreatedObject))
        return none("no field, and the layout is of no class"_s);
    if (!findsMemberWithoutCodeForTypedAccess(program, key))
        return none("no field, and a lookup under the key may run code"_s);
    classLayout = declaredLayoutOf(program, member);
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = classLayout; current;) {
        const Layout& layout = program.at(current);
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || !layout.constructor || !walker.plan().hasClassRecord(current))
            return none("no field, and a class on the way up has no record"_s);
        if (!walker.facts().isEvaluatedAtMostOnce(current))
            return none("no field, and a class on the way up may be evaluated again"_s);
        if (layout.has(LayoutFlags::IsConstructorFunction) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
            return none("no field, and a constructor function on the way up holds captures"_s);
        const ClassMember* found = nullptr;
        for (const ClassMember& declared : walker.facts().membersOf(current)) {
            if (declared.side != ClassSide::Prototype)
                continue;
            // Under a computed key may stand the name, and of two members under one name the later is there.
            if (!declared.key)
                return none("no field, and a class on the way up has a member under a computed key"_s);
            if (declared.key == key && found)
                return none("no field, and a class lists the name twice"_s);
            if (declared.key == key)
                found = &declared;
        }
        if (found) {
            if (found->kind != MethodKind::Method)
                return none("no field: an accessor above"_s);
            return found->function ? found->function : none("no field: a method above of which the record names no function"_s);
        }
        // Something is laid there that no record declares: the guard would never hold.
        for (const LinkedNameOnPrototype& name : std::ranges::equal_range(namesOnPrototypes, current, { }, &LinkedNameOnPrototype::layout)) {
            if (!name.key || name.key == key)
                return none("no field, and statements lay more than one thing under the name on a prototype object"_s);
        }
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return none("no field, and the row of classes goes on above the program's own"_s);
        current = parent;
    }
    return none("no field, and no class of the row declares the name"_s);
}

// As findMethodDeclaredAboveObjectsOf() goes up the row. The run time sees every evaluation of a `class` and ends its guards at the second (ClassTable::armMemberGuards()), so such a class need not be proven to be evaluated once:
// nothing is taken from the closure table here. Of a function it sees none.
static bool recordsDeclareMemberAboveObjectsOfLayout(const Walker& walker, LayoutID member, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (program.at(member).kind != LayoutKind::Instance || program.at(member).has(LayoutFlags::IsOfCreatedObject) || !findsMemberWithoutCodeForTypedAccess(program, key))
        return false;
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = declaredLayoutOf(program, member); current;) {
        const Layout& layout = program.at(current);
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || !layout.constructor || !walker.plan().hasClassRecord(current))
            return false;
        if (layout.has(LayoutFlags::IsConstructorFunction)
            && (!walker.facts().isEvaluatedAtMostOnce(current) || program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor)))
            return false;
        bool isDeclared = false;
        for (const ClassMember& declared : walker.facts().membersOf(current)) {
            if (declared.side != ClassSide::Prototype)
                continue;
            // A member under a computed key that lands on the name is a member that the class declares, and the run time takes it for one: numberOfComputedNameMembersInProperty().
            if (declared.key != key)
                continue;
            if (!declared.function)
                return false;
            isDeclared = true;
        }
        if (isDeclared)
            return true;
        // Something is laid there that no record declares: the guard would never hold. What is laid under a key that nobody knows is seldom this name, and the question costs a few instructions.
        for (const LinkedNameOnPrototype& name : std::ranges::equal_range(namesOnPrototypes, current, { }, &LinkedNameOnPrototype::layout)) {
            if (name.key == key)
                return false;
        }
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
    // What Link proves of a member is read without a guard.
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
            conditions.append(MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key });
    }
    return !classes.isEmpty();
}

bool recordsDeclareMemberAboveObjectsOf(Walker& walker, TypeID typeOfObjects, PropertyKeyID key)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObjects);
    if (!receivers || describeFieldOfTypedAccess(walker, *receivers, key, { }, true).coverage != CoverageOfTypedAccess::NoReceiver)
        return false;
    return std::ranges::all_of(receivers->members, [&](LayoutID member) {
        return recordsDeclareMemberAboveObjectsOfLayout(walker, member, key);
    });
}

// Null: the receivers do not all come to the same method.
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

// Closed, the call reads the method itself. So the read gains only where its call becomes a call of the function.
static bool isReadForCallAloneInTypedAccess(const Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID user = read.opcode() == Opcode::PropGet ? index.onlyUsingValue(read.id()) : ValueID();
    return user && index.opcodeOf(user) == Opcode::Call && index.value(user).as<Opcode::Call>().calleeOperand() == read.id();
}

// While the guard holds, the closure on the holder is the one that the closure table holds: holdsMethodOfClassRecord() and definesMemberOfClassRecord() of object/AOTClassGuards.cpp. The table holds none that has captures.
static bool closureTableHoldsMethodForTypedAccess(const Walker& walker, FunctionID method)
{
    const Plan& plan = walker.plan();
    return plan.hasClosureCell(method) && walker.program().at(method).captures.size() == plan.promotedEnvironments().numberOfRemovedCapturesOf(method);
}

static FunctionID findMethodReadBehindLooksForTypedAccess(const Walker& walker, const BodyValue& read, const ReceiversOfTypedAccess& receivers, PropertyKeyID key, ASCIILiteral* whyNone = nullptr)
{
    if (!walker.has(Mechanism::DirectCalls) || !isReadForCallAloneInTypedAccess(walker, read)) {
        if (whyNone)
            *whyNone = "no field, and the read is not there for a call alone"_s;
        return { };
    }
    // Two arms at every call of a method cost bytes and build. They pay where the call comes again, in a loop, and in a plain body: what an application runs most is small functions that call methods (1% of its instructions).
    // In the body of a coroutine, outside of loops, they were a third of the bytes for one in a hundred of the calls.
    if (!walker.isInLoop() && walker.unit().role == CodeRole::CoroutineBody) {
        if (whyNone)
            *whyNone = "no field: a method above, outside of loops"_s;
        return { };
    }
    return findMethodDeclaredAboveReceiversForTypedAccess(walker, receivers, key, whyNone);
}

// TestingSwitch::CheckProofsAtRunTime. What the state remembers of a cell is the walk's own belief, like what it remembers of a place: a test has passed or the object was made here, and nothing since is taken to have written the words.
// No read shows it, so the tests are built again in front of whoever leans on them.
void Walker::checkFactsRememberedOfCell(Reference object, Origin origin)
{
    if (!checksProofs() || isUnreachable() || !object || !object.isValue())
        return;
    auto check = [&](Draft&& test) {
        if (isUnreachable())
            return;
        Reference holds = build(WTF::move(test));
        if (holds && holds.isValue() && !isUnreachable())
            checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::WhatIsRemembered, origin);
    };
    OptionSet<FactOfCell> remembered = m_state.factsOfCell(object);
    // The whole word is known: it has no flag.
    bool knowsWord = remembered.contains(FactOfCell::OrderWordIsKnown);
    if (knowsWord || remembered.contains(FactOfCell::NotFrozen)) {
        Draft test(Opcode::IsNotFrozen, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(knowsWord || remembered.contains(FactOfCell::NoFieldOutsideSlot));
        test.setHasNoExtraProperties(knowsWord || remembered.contains(FactOfCell::NoExtraProperties));
        check(WTF::move(test));
    } else if (remembered.contains(FactOfCell::NoExtraProperties)) {
        Draft test(Opcode::HasNoExtraProperties, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(remembered.contains(FactOfCell::NoFieldOutsideSlot));
        check(WTF::move(test));
    } else if (remembered.contains(FactOfCell::NoFieldOutsideSlot)) {
        Draft test(Opcode::HasNoFieldOutsideSlot, origin);
        test.setObjectOperand(object);
        check(WTF::move(test));
    }
    if (remembered.contains(FactOfCell::HasLayoutPrototype)) {
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

// TestingSwitch::CheckProofsAtRunTime. A call that takes the elements of a list from the state reads nothing. So the length is read here and held against their number.
// Not in a loop: the look at the loop has not counted on the read.
void Walker::checkElementsRememberedOfList(Reference list, std::span<const Reference> elements, Origin origin)
{
    if (!checksProofs() || isUnreachable() || isInLoop() || !list || !list.isValue())
        return;
    TypeID type = typeOf(list);
    if (!type || !hasKind(m_types, type, OperandKind::Array))
        return;
    Draft read(Opcode::GetArrayLength, origin);
    chooseConservativeFields(read);
    read.setArrayOperand(list);
    Reference found = build(WTF::move(read));
    Draft number(Opcode::Integer, origin);
    number.setInteger(static_cast<int64_t>(elements.size()));
    Reference remembered = constant(WTF::move(number));
    if (!found || !found.isValue() || !remembered || isUnreachable())
        return;
    Draft comparison(Opcode::StrictEq, origin);
    chooseConservativeFields(comparison);
    comparison.setLeftOperand(found);
    comparison.setRightOperand(remembered);
    Reference holds = build(WTF::move(comparison));
    if (holds && holds.isValue() && !isUnreachable())
        checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::WhatIsRemembered, origin);
}

static TypedForm buildReadOfMethodBehindLooksForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    FunctionID method = findMethodReadBehindLooksForTypedAccess(walker, value, receivers, key);
    if (!method)
        return TypedForm::None;
    if (operands.areOnlyTypes)
        return TypedForm::BuiltBehindLookAtCell;

    const WalkState& state = walker.state();
    // A closure with captures is read off the holder, by the look that has asked it what it is a closure of: lookAtCellInFrontOfStores(). That read lets a fresh object go, and with it what was known of its cell, behind which the look stood.
    // The guards below are asked all the same: they hold until something may run code, and so does what the look has read.
    Reference readByLook = walker.referenceBoundTo(value.id());
    Reference closureReadByLook = readByLook && readByLook.isValue() && walker.code().at(readByLook.id()).opcode == Opcode::PropGet ? state.checkedNameOf(readByLook) : Reference();
    if (closureReadByLook && singleFunctionOf(walker.types(), walker.types().at(walker.typeOf(closureReadByLook))) != method)
        closureReadByLook = { };
    if (!closureReadByLook && possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove, state.factsOfCell(operands.object), true))
        return TypedForm::None;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout;
        if (!findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout) || !state.guardOfReadsHolds(classLayout, key))
            return TypedForm::None;
    }

    if (walker.checksProofs()) {
        Origin origin = walker.originOf(value);
        walker.checkFactsRememberedOfCell(operands.object, origin);
        Vector<MemberConditionRecord, 8> remembered;
        for (LayoutID member : receivers.members) {
            LayoutID classLayout;
            if (findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout))
                remembered.append(MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key });
        }
        if (!remembered.isEmpty() && !walker.isUnreachable()) {
            Draft test(Opcode::AreMembersUnchanged, origin);
            test.setConditions(remembered.span());
            Reference holds = walker.build(WTF::move(test));
            if (holds && holds.isValue() && !walker.isUnreachable())
                walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::WhatIsRemembered, origin);
        }
    }

    Draft written = walker.draftOf(value);
    if (closureTableHoldsMethodForTypedAccess(walker, method)) {
        Draft closure(Opcode::NewClosure, written.origin());
        closure.setFunction(method);
        closure.setIsHeldByClosureTable(true);
        walker.chooseConservativeFields(closure);
        walker.bind(value.id(), walker.build(WTF::move(closure)));
        return TypedForm::Built;
    }
    // No look has read it: the read and the call as they are written, or a guess that looks.
    if (!closureReadByLook)
        return TypedForm::None;
    walker.bind(value.id(), closureReadByLook);
    return TypedForm::Built;
}

// ---- An accessor above an object that is out of sight

// `class Temp { get f() { .. } set f(v) { .. } }`, `Object.defineProperty(Temp.prototype, "f", { get: function () { .. }, set: function (v) { .. } })`, off an object whose layout has left.
// As findMethodDeclaredAboveObjectsOf() goes up the row: while the guard that reads ask of the class and the name holds, a lookup under the key comes to the accessor that the first record of the row declares there,
// with closures of its functions: holdsMemberOfClassRecord() of object/AOTClassGuards.cpp asks both halves. A half that the record does not name is not there.
static bool findAccessorDeclaredAboveObjectsOf(const Walker& walker, LayoutID member, PropertyKeyID key, LayoutID& classLayout, FunctionID& getter, FunctionID& setter)
{
    const Program& program = walker.program();
    if (program.at(member).kind != LayoutKind::Instance || program.at(member).has(LayoutFlags::IsOfCreatedObject) || !findsMemberWithoutCodeForTypedAccess(program, key))
        return false;
    classLayout = declaredLayoutOf(program, member);
    const Vector<LinkedNameOnPrototype>& namesOnPrototypes = walker.facts().held().namesOnPrototypes;
    size_t numberOfClassesLeft = program.layouts.size();
    for (LayoutID current = classLayout; current;) {
        const Layout& layout = program.at(current);
        if (!numberOfClassesLeft-- || layout.kind != LayoutKind::Instance || !layout.constructor || !walker.plan().hasClassRecord(current) || !walker.facts().isEvaluatedAtMostOnce(current))
            return false;
        if (layout.has(LayoutFlags::IsConstructorFunction) && program.at(layout.constructor).captures.size() != walker.plan().promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
            return false;
        bool isFound = false;
        for (const ClassMember& declared : walker.facts().membersOf(current)) {
            if (declared.side != ClassSide::Prototype)
                continue;
            // Under a computed key may stand the name.
            if (!declared.key)
                return false;
            if (declared.key != key)
                continue;
            FunctionID* half = declared.kind == MethodKind::Getter ? &getter : declared.kind == MethodKind::Setter ? &setter : nullptr;
            if (!half || *half || !declared.function)
                return false;
            *half = declared.function;
            isFound = true;
        }
        if (isFound)
            return true;
        // Something is laid there that no record declares: the guard would never hold.
        for (const LinkedNameOnPrototype& name : std::ranges::equal_range(namesOnPrototypes, current, { }, &LinkedNameOnPrototype::layout)) {
            if (!name.key || name.key == key)
                return false;
        }
        LayoutID parent;
        if (!findParentNamedByClassRecordForTypedAccess(walker, current, parent))
            return false;
        current = parent;
    }
    return false;
}

// The half that the access runs. Null: the receivers do not all come to one accessor, the half is not there, or it needs its closure, which nobody has read.
static FunctionID findHalfRunBehindLooksForTypedAccess(Walker& walker, const BodyValue& access, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    bool isStore = access.opcode() == Opcode::PropSet;
    if (!walker.has(Mechanism::DirectCalls) || (access.opcode() != Opcode::PropGet && !isStore) || access.isFreeName())
        return { };
    // As a method above: in the body of a coroutine, outside of loops, the arms cost more bytes than they save.
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
    return halfOfAll && closureTableHoldsMethodForTypedAccess(walker, halfOfAll) && accessorAdmitsDirectCall(walker, halfOfAll) ? halfOfAll : FunctionID();
}

static TypedForm buildAccessToAccessorBehindLooksForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    FunctionID half = findHalfRunBehindLooksForTypedAccess(walker, value, receivers, key);
    if (!half)
        return TypedForm::None;
    // Whoever looks at a loop counts on what the half does: effectsOfTypedFormAhead().
    if (operands.areOnlyTypes) {
        walker.noteFormAskedAhead(Opcode::CallDirect, KnowledgeLevel::Proven, half);
        return TypedForm::BuiltBehindLookAtCell;
    }

    const WalkState& state = walker.state();
    if (possibleChangesForbid(receivers.possibleChanges, TypedAccess::FindsMethodAbove, state.factsOfCell(operands.object), true))
        return TypedForm::None;
    Vector<MemberConditionRecord, 8> remembered;
    for (LayoutID member : receivers.members) {
        LayoutID classLayout;
        FunctionID getter;
        FunctionID setter;
        if (!findAccessorDeclaredAboveObjectsOf(walker, member, key, classLayout, getter, setter) || !state.guardOfReadsHolds(classLayout, key))
            return TypedForm::None;
        remembered.append(MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key });
    }
    if (walker.checksProofs()) {
        Origin origin = walker.originOf(value);
        walker.checkFactsRememberedOfCell(operands.object, origin);
        if (!walker.isUnreachable()) {
            Draft test(Opcode::AreMembersUnchanged, origin);
            test.setConditions(remembered.span());
            Reference holds = walker.build(WTF::move(test));
            if (holds && holds.isValue() && !walker.isUnreachable())
                walker.checkProof(holds, TypeArena::fromPrimitives(Primitive::True), TypeTable::boolean, ProofSource::WhatIsRemembered, origin);
        }
    }
    if (callAccessor(walker, value, half, operands.object, value.opcode() == Opcode::PropSet ? operands.stored : Reference()) || walker.hasFailed())
        return TypedForm::Built;
    // The looks have been asked for this access, or hold already. Whoever came behind it would be told "behind a look" again, and might find no test left to ask.
    Draft asWritten = walker.draftOf(value);
    walker.chooseConservativeFields(asWritten);
    walker.bind(value.id(), walker.build(WTF::move(asWritten)));
    return TypedForm::Built;
}

TypeID typeOfMethodFoundBehindLooks(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    std::optional<ReceiversOfTypedAccess> receivers = read.opcode() == Opcode::PropGet ? findReceiversOfTypedAccess(walker, typeOfObject) : std::nullopt;
    if (!receivers || describeFieldOfTypedAccess(walker, *receivers, read.key(), { }, true).coverage != CoverageOfTypedAccess::NoReceiver)
        return { };
    FunctionID method = findMethodReadBehindLooksForTypedAccess(walker, read, *receivers, read.key());
    return method ? walker.types().forFunction(method) : TypeID();
}

namespace {

// What is asked ahead knows nothing of the state.
struct TypedStoreAskedAhead {
    TypedForm form { TypedForm::None };
    // What the state has to know of the cell where the store is built.
    OptionSet<FactOfCell> askedOfCell;
    bool creates { false };
};

// What the order word of a cell that holds every property in its slot says of one field.
struct FieldInKnownOrder {
    // The object lists the key, so the slot is filled.
    bool isListed { false };
    // The word once the object lists the key behind the others. Nothing: the image has no order that goes on so, and the run time makes one.
    std::optional<uint32_t> wordWithFieldListedNext;
};

} // anonymous namespace

static TypedStoreAskedAhead askAheadAboutTypedStore(const Walker& walker, const BodyValue& value, const ReceiversOfTypedAccess& receivers, const FieldOfTypedAccess& field, PropertyKeyID key, bool isDefinition)
{
    std::optional<OptionSet<FactOfCell>> toOverwrite = fewestFactsOfCellWith([&](OptionSet<FactOfCell> facts) {
        return mayOverwriteInTypedStore(receivers, facts);
    });
    if (toOverwrite && isFilledForBodyBeingReadInTypedAccess(walker, field, value.objectOperand(), key))
        return { toOverwrite->isEmpty() ? TypedForm::Built : TypedForm::BuiltBehindLookAtCell, *toOverwrite, false };

    std::optional<OptionSet<FactOfCell>> toCreate = fewestFactsOfCellWith([&](OptionSet<FactOfCell> facts) {
        return mayCreateInTypedStore(walker, receivers, field, key, isDefinition, facts, nullptr);
    });
    if (toCreate && toCreate->isEmpty() && (isDefinition || !storeAsksGuardsWherePrototypeStaysForTypedAccess(walker, receivers, field, key)))
        return { TypedForm::Built, { }, true };

    // Where something has to be looked at either way: a store into a slot that a look has found filled is one instruction, and one that may create the property asks whether it does.
    // So the slot is looked at where the source says that the property is there and the body is not the one that puts it there.
    bool slotIsLikelyFilled = paysToLookAtSlotForTypedAccess(walker, field, key) && !field.isAppended && field.isOfBirth && !isBeingMadeByBodyBeingReadInTypedAccess(walker, value.objectOperand());
    if (toCreate && (!toOverwrite || !slotIsLikelyFilled))
        return { TypedForm::BuiltBehindLookAtCell, *toCreate, true };
    // `socket[kRequest] = request`, on a socket that serves many requests: nobody prepares a store.
    bool paysToLookUnderSymbol = field.isRequired && !field.mayBeListedWithoutStorage && isSymbolKind(walker.program().at(key).kind);
    if (!toOverwrite || !(paysToLookAtSlotForTypedAccess(walker, field, key) || paysToLookUnderSymbol))
        return { };
    return { toOverwrite->isEmpty() ? TypedForm::BuiltBehindFilledSlot : TypedForm::BuiltBehindFilledSlotAndLookAtCell, *toOverwrite, false };
}

// An allocation of the layout that lists nothing leaves the word of no keys, and from there on the word of a cell says which keys it has got. An order is numbered within its shape.
static bool orderWordTellsKeysOfObjectsOf(const Walker& walker, LayoutID layout)
{
    const Plan& plan = walker.plan();
    return plan.layout(layout).holdsCompiledObjects() && plan.shapes().shapesOf(layout).count == 1;
}

// Nothing: the word says nothing of the field. A slot without an absent state counts as filled from the allocation on, and no order lists it.
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
        listed.append(orders.order(layout, orderOf(orderWord)).first(count));
    }
    if (listed.contains(*field))
        return FieldInKnownOrder { true, std::nullopt };
    listed.append(*field);
    return FieldInKnownOrder { false, orders.findOrderWord(layout, listed.span()) };
}

// `class Shape { constructor() { this.id = 0; } }   class Circle extends Shape { .. }`, and a maker with its members by tag: an order is numbered within its layout,
// and the orders of what is written below begin as those of the layout above. Nothing: some receiver's word says nothing of the field, or they do not all come to the same.
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

static TypedForm buildStoreForTypedAccess(Walker& walker, const BodyValue& value, const OperandsOfTypedAccess& operands, PropertyKeyID key, ECMAMode mode, bool isDefinition)
{
    TypeID typeOfStored = operands.typeOfStored;
    std::optional<ReceiversOfTypedAccess> receivers = typeOfStored ? findReceiversOfTypedAccess(walker, operands.typeOfObject) : std::nullopt;
    // A definition looks at the object alone. A store that comes to an accessor without a setter throws or does nothing, by its mode: that stays closed.
    auto storeBySetter = [&](const MemberFound& member) {
        if (isDefinition || !typeOfStored || !walker.has(Mechanism::DirectCalls) || member.kind != MemberFound::Kind::Accessor || !member.setter)
            return TypedForm::None;
        if (operands.areOnlyTypes)
            return TypedForm::BuiltAsCall;
        return callAccessor(walker, value, member.setter, operands.object, operands.stored) ? TypedForm::Built : TypedForm::None;
    };
    if (!receivers)
        return storeBySetter(findMemberOfNamedValueForTypedAccess(walker, operands.typeOfObject, key));
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, typeOfStored, true);
    if (field.coverage == CoverageOfTypedAccess::NoReceiver) {
        TypedForm form = storeBySetter(findMemberAboveReceiversForTypedAccess(walker, *receivers, key));
        return form != TypedForm::None || isDefinition ? form : buildAccessToAccessorBehindLooksForTypedAccess(walker, value, operands, *receivers, key);
    }
    if (!field.liesInOneSlot || !field.isStoredByProgram || !field.takesStoredValue)
        return TypedForm::None;

    if (operands.areOnlyTypes)
        return askAheadAboutTypedStore(walker, value, *receivers, field, key, isDefinition).form;

    Reference object = operands.object;
    Reference stored = operands.stored;
    OptionSet<FactOfCell> knownOfCell = walker.state().factsOfCell(object);
    std::optional<FieldInKnownOrder> inKnownOrder;
    if (std::optional<uint32_t> orderWord = walker.state().orderWordOf(object))
        inKnownOrder = findFieldInKnownOrderOfEveryReceiver(walker, receivers->members.span(), key, *orderWord);
    bool isFilled = mayOverwriteInTypedStore(*receivers, knownOfCell) && ((inKnownOrder && inKnownOrder->isListed)
        || isFilledForBodyBeingReadInTypedAccess(walker, field, value.objectOperand(), key) || provesSlotFilledForTypedAccess(walker, valueInSlotForTypedAccess(walker, object, field), key));
    if (!isFilled && !mayCreateInTypedStore(walker, *receivers, field, key, isDefinition, knownOfCell, &walker.state()))
        return TypedForm::None;
    if (isFilled && walker.has(Mechanism::Folding) && storeLaysWhatSlotOfFreshObjectHolds(walker, object, valueInSlotForTypedAccess(walker, object, field), stored))
        return TypedForm::Built;
    Opcode opcode = isFilled ? Opcode::PutSlot : Opcode::PutSlotOrAdd;

    Draft store = draftOfSlotOperationForTypedAccess(walker, value, opcode, object, *receivers, key);
    store.setValueOperand(stored);
    store.setMode(mode);
    store.setIsDefinition(isDefinition);
    // The cell lacks the field, and the word that lists it next is written in the place of a search.
    std::optional<uint32_t> orderWordAfter = inKnownOrder ? inKnownOrder->wordWithFieldListedNext : std::nullopt;
    if (opcode == Opcode::PutSlotOrAdd) {
        store.setIsExpectedToCreate(walker.state().isFresh(object) || !!walker.program().at(walker.currentFunction()).constructedClass);
        if (orderWordAfter)
            store.setOrderWordAfter(OrderWord(*orderWordAfter));
    }
    walker.bind(value.id(), walker.build(WTF::move(store)));

    TypeID typeOfObject = walker.typeOf(object);
    walker.state().invalidateAliasedSlots(object, field.place, scopedLambda<bool(Reference)>([&](Reference other) {
        TypeID typeOfOther = walker.typeOf(other);
        return !typeOfOther || walker.types().mayBeSameObject(typeOfObject, typeOfOther);
    }));
    walker.state().noteSlot(object, field.place, stored);
    if (opcode == Opcode::PutSlotOrAdd)
        walker.state().noteOrderWord(object, orderWordAfter);
    return TypedForm::Built;
}

// ---- Private names. The class declares the name, so it is a field of the layout and of the layouts below, with a slot like any other. Nobody outside the class can name it:
// it is not deleted, redefined or frozen, nothing above the object answers for it, and none of the five changes comes near it. An object has the name or the access throws.

PropertyKeyID keyOfAccessToSlot(const Program& program, const BodyValue& access)
{
    if (!accessesUnderPrivateNames.contains(access.opcode()))
        return access.key();
    return fieldAt(program, FieldReference { access.layout(), access.field() }).key;
}

// Each evaluation of a class has names of its own, in each realm that runs the image, and all of them share the layout. Where the process has one evaluation, the slot says whether the object has the name:
// only the definition of the field fills it, nothing empties it, and the run time lays the name elsewhere only in an object whose layout lacks the field (operationAOTPrivateDefine()).
static bool slotTellsWhoHasPrivateNameForTypedAccess(const Walker& walker, LayoutID declaring)
{
    return walker.plan().hasOneInstance() && walker.facts().isEvaluatedAtMostOnce(declaredLayoutOf(walker.program(), declaring));
}

// `#x = 1` in a class without a parent. The definitions of its fields are written once each, behind the allocation, and run on the object that this very construction has allocated: none finds the name taken,
// and the object belongs to the evaluation that is making it. In a class with a parent `super()` may answer with any object, one that has the name among them, and there the definition throws.
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
    // A store of what does not fit the slot would move the field out to the extra properties, and the empty slot would say that the name is not there.
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, typeOfStored);
    if (!field.liesInOneSlot || !field.isMovedOutByNoStore)
        return TypedForm::None;
    // The brand takes the token of the evaluation, which is no value of JavaScript.
    if (stores && !field.takesStoredValue && !(declared.flags.contains(FieldFlags::IsBrand) && opcode == Opcode::PrivateDefine))
        return TypedForm::None;

    auto buildStore = [&](Opcode store) {
        Reference object = walker.referenceOf(value.objectOperand());
        Reference stored = walker.referenceOf(value.valueOperand());
        Draft draft = draftOfSlotOperationForTypedAccess(walker, value, store, object, *receivers, key);
        draft.setValueOperand(stored);
        draft.setMode(ECMAMode::Strict);
        draft.setIsDefinition(opcode == Opcode::PrivateDefine);
        // As in buildStoreForTypedAccess(): the order lists a private name like any field that is created.
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
        }));
        walker.state().noteSlot(object, field.place, stored);
        if (store == Opcode::PutSlotOrAdd)
            walker.state().noteOrderWord(object, orderWordAfter);
        return TypedForm::Built;
    };

    if (opcode == Opcode::PrivateDefine && definesPrivateNameOfObjectJustAllocatedForTypedAccess(walker, value))
        return inputs.areOnlyTypes() ? TypedForm::Built : buildStore(Opcode::PutSlotOrAdd);
    // The operation stays as it is written, and on a receiver whose layout is proven the emitter compares the brand in line: PrivateAccessEmitter::findSlotsOfProvenReceiver(). So the proof pays for its test.
    if (!slotTellsWhoHasPrivateNameForTypedAccess(walker, value.layout()))
        return inputs.areOnlyTypes() && brandLiesInOneSlotForTypedAccess(walker, value, *receivers) ? TypedForm::Built : TypedForm::None;
    if (opcode == Opcode::PrivateDefine) {
        if (inputs.areOnlyTypes())
            return TypedForm::BuiltBehindEmptySlot;
        Reference held = walker.state().slot(walker.referenceOf(value.objectOperand()), field.place);
        return held && walker.typeOf(held) == TypeTable::absent ? buildStore(Opcode::PutSlotOrAdd) : TypedForm::None;
    }

    bool isFilledByConstructions = isFilledForBodyBeingReadInTypedAccess(walker, field, value.objectOperand(), key) || (opcode == Opcode::PrivateGet && field.isFilledAtEveryRead);
    if (inputs.areOnlyTypes())
        return isFilledByConstructions || opcode == Opcode::PrivateHas ? TypedForm::Built : TypedForm::BuiltBehindFilledSlot;

    Reference object = walker.referenceOf(value.objectOperand());
    Reference held = valueInSlotForTypedAccess(walker, object, field);
    bool isFilled = isFilledByConstructions || provesSlotFilledForTypedAccess(walker, held, key);
    if (opcode == Opcode::PrivateHas) {
        if (isFilled && walker.has(Mechanism::Folding))
            bindBooleanForTypedAccess(walker, value, true);
        else
            walker.bind(value.id(), walker.build(draftOfSlotOperationForTypedAccess(walker, value, Opcode::IsSlotFilled, object, *receivers, key)));
        return TypedForm::Built;
    }
    // The access throws where the slot is empty.
    if (!isFilled)
        return TypedForm::None;
    if (opcode == Opcode::PrivateSet)
        return buildStore(Opcode::PutSlot);
    if (held && walker.has(Mechanism::Folding)) {
        walker.bind(value.id(), held);
        return TypedForm::Built;
    }
    Reference read = walker.build(draftOfSlotOperationForTypedAccess(walker, value, Opcode::GetSlot, object, *receivers, key));
    walker.bind(value.id(), read);
    if (read)
        walker.state().noteSlot(object, field.place, read);
    return TypedForm::Built;
}

// ---- For a guess: the look at the slot that the forms for a filled slot stand behind

// `flags: SymbolFlags`, in four bytes: absent, undefined and null are marks below the least number. The source says that none of them is read there, so one compare asks for a number, and whoever finds a mark reads by name.
// `socket: Socket`, and null until it is connected: a reference is asked for so only where what is written admits neither undefined nor null.
static bool lookAsksForValueOfWrittenType(Walker& walker, const ReceiversOfTypedAccess& receivers, PropertyKeyID key)
{
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, receivers, key, { });
    return field.coverage == CoverageOfTypedAccess::EveryReceiver && field.liesInOneSlot && field.isRequired && field.liesInFormWithMarks
        && (field.liesInFormOfNumbersOrBooleans || field.isWrittenWithoutUndefinedAndNull);
}

Reference buildTestOfSlot(Walker& walker, const BodyValue& access, Reference object)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return { };
    Draft test(Opcode::IsSlotFilled, walker.originOf(access));
    fillSlotOperationForTypedAccess(test, object, *receivers, keyOfAccessToSlot(walker.program(), access));
    // A store asks whether the property is there. `symbol.parent = found`, over the undefined of the constructor.
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

// The row of the read by name answers for all that the name may find: an accessor that may have been laid over the field, what stands above the object. Behind the look the slot is filled, and it holds what was stored into the field.
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
    // It follows from the type, the name and Link's tables. A slow body is typed without the tables, and opens no guess.
    TypeArena& types = walker.types();
    PropertyKeyID key = keyOfAccessToSlot(walker.program(), read);
    bool isRemembered = types.keeps() == ArenaKeeps::UntilReset && walker.has(Mechanism::Guesses);
    if (std::optional<uint32_t> known = isRemembered ? types.rememberedAnswer(static_cast<uint8_t>(AnswerKeptInArena::TypeInFilledSlot), typeOfObject.bits(), key.bits()) : std::nullopt)
        return TypeID::fromBits(*known);
    TypeID found;
    if (std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject)) {
        // A rule asks of an operand whether it is there, and its type. It never follows the reference.
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
        TypeID type = field ? slots.slot(member, *field).type : TypeID();
        if (type)
            taken = taken ? walker.types().intersectionOf(taken, type) : type;
    }
    return taken;
}

Reference buildReadThatTestsShapeAndSlot(Walker& walker, const BodyValue& read, Reference object, TypeID guessed, bool expectsCell)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, guessed);
    if (!receivers)
        return { };
    if (slotHoldsMarkForTypedAccess(walker, *receivers, read.key()))
        return { };
    Draft draft = draftOfSlotOperationForTypedAccess(walker, read, Opcode::GetSlot, object, *receivers, read.key());
    draft.setHoldsDeclaredType(lookAsksForValueOfWrittenType(walker, *receivers, read.key()));
    draft.setLevel(KnowledgeLevel::Expected);
    if (resultRepresentationOf(walker, draft) != Representation::JSValue)
        return { };
    draft.setExpectsCell(expectsCell);
    // The closed read may hand the object to a getter.
    walker.letLeave(object);
    return walker.build(WTF::move(draft));
}

Reference buildReadThatTestsShapeAndCell(Walker& walker, const BodyValue& read, Reference object, TypeID guessed, bool expectsCell)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, guessed);
    if (!receivers)
        return { };
    PropertyKeyID key = read.key();
    FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
    if (field.coverage == CoverageOfTypedAccess::NoReceiver || !field.liesInOneSlot)
        return { };
    std::optional<OptionSet<FactOfCell>> asked = askAheadAboutReadOfSlotThatMayBeEmpty(walker, *receivers, field, key);
    if (!asked || !asked->containsOnly({ FactOfCell::NoFieldOutsideSlot, FactOfCell::HasLayoutPrototype }))
        return { };
    Vector<MemberConditionRecord, 8> conditions;
    if (asked->contains(FactOfCell::HasLayoutPrototype)) {
        for (LayoutID member : receivers->members) {
            LayoutID classLayout;
            if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, LookupAbove::OfRead, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass || walker.state().guardOfReadsHolds(classLayout, key))
                continue;
            MemberConditionRecord condition { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
            if (!conditions.contains(condition))
                conditions.append(condition);
        }
    }
    Draft draft = draftOfSlotOperationForTypedAccess(walker, read, Opcode::GetSlotOrUndefined, object, *receivers, key);
    // The operation boxes what it loads, whatever the form of the slot.
    draft.setLevel(KnowledgeLevel::Expected);
    if (resultRepresentationOf(walker, draft) != Representation::JSValue)
        return { };
    draft.setExpectsCell(expectsCell);
    draft.setConditions(conditions.span());
    // The closed read may hand the object to a getter.
    walker.letLeave(object);
    return walker.build(WTF::move(draft));
}

bool buildStoreThatTestsForItself(Walker& walker, const BodyValue& store, Reference object, TypeID guessed, bool expectsCell)
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
    if (!ahead.askedOfCell.containsOnly({ FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot, FactOfCell::HasLayoutPrototype }))
        return false;
    Vector<MemberConditionRecord, 8> conditions;
    if (ahead.creates) {
        Reference root = object.isValue() ? Reference::value(rootOfCellInSweptCode(walker.code(), object.id())) : object;
        if (isBeingMadeByBodyBeingReadInTypedAccess(walker, store.objectOperand()) || walker.state().factsOfCell(root).containsAny({ FactOfCell::IsLikelyAsAllocated, FactOfCell::IsLikelyAsConstructed }))
            return false;
        ValueID written = store.id();
        if (findLikelyOrderWordInFrontOfStores(walker, written, singleElementSpan(written), object, guessed))
            return false;
        if (ahead.askedOfCell.contains(FactOfCell::HasLayoutPrototype) || storeAsksGuardsWherePrototypeStaysForTypedAccess(walker, *receivers, field, key)) {
            for (LayoutID member : receivers->members) {
                LayoutID classLayout;
                if (whoAnswersForWhatStandsAboveObjectsOf(walker, member, key, LookupAbove::OfStore, classLayout) != WhoAnswersForWhatStandsAbove::GuardOfClass || walker.state().storesPassAbove(classLayout, key))
                    continue;
                MemberConditionRecord condition { MemberConditionKind::MemberPassedByStores, classLayout, ClassSide::Prototype, key };
                if (!conditions.contains(condition))
                    conditions.append(condition);
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
    draft.setExpectsCell(expectsCell);
    walker.letLeave(object);
    walker.letLeave(stored);
    walker.bind(store.id(), walker.build(WTF::move(draft)));
    return !walker.hasFailed();
}

void noteSlotFoundEmpty(Walker& walker, const BodyValue& access, Reference object)
{
    if (std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object)))
        walker.state().noteSlot(object, describeFieldOfTypedAccess(walker, *receivers, keyOfAccessToSlot(walker.program(), access), { }).place, walker.constant(Draft(Opcode::Absent)));
}

bool looksAtSlotOfObjectBeingMade(const Walker& walker, const BodyValue& access)
{
    return isBeingMadeByBodyBeingReadInTypedAccess(walker, access.objectOperand());
}

void lookAtCellInFrontOfStores(Walker& walker, std::span<const ValueID> stores, Reference object, NOESCAPE const ScopedLambda<void(Reference test)>& goOnWhereTestPasses)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers)
        return;
    const BodyIndex& index = walker.currentBodyIndex();
    Origin origin = walker.originOf(index.value(stores.front()));

    OptionSet<FactOfCell> asked;
    Vector<MemberConditionRecord, 8> conditions;
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
            if (!conditions.contains(condition))
                conditions.append(condition);
        }
    };
    // A read or a store that runs the half of an accessor above the object: what a method above asks of the cell, and the guard of the class and the name.
    auto askLooksForAccessorAbove = [&](const BodyValue& access, PropertyKeyID key) {
        if (!findHalfRunBehindLooksForTypedAccess(walker, access, *receivers, key))
            return false;
        if (std::optional<OptionSet<FactOfCell>> askedByAccess = fewestFactsOfCellWith([&](OptionSet<FactOfCell> facts) { return !possibleChangesForbid(receivers->possibleChanges, TypedAccess::FindsMethodAbove, facts, true); }))
            asked.add(*askedByAccess);
        for (LayoutID member : receivers->members) {
            LayoutID classLayout;
            FunctionID getter;
            FunctionID setter;
            if (!findAccessorDeclaredAboveObjectsOf(walker, member, key, classLayout, getter, setter) || walker.state().guardOfReadsHolds(classLayout, key))
                continue;
            MemberConditionRecord condition { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
            if (!conditions.contains(condition))
                conditions.append(condition);
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
                if (std::optional<OptionSet<FactOfCell>> askedByRead = askAheadAboutReadThatFindsNothing(walker, *receivers, field, key)) {
                    asked.add(*askedByRead);
                    if (askedByRead->contains(FactOfCell::HasLayoutPrototype))
                        askGuardsOfClasses(key, LookupAbove::OfRead);
                }
                continue;
            }
            if (!closureTableHoldsMethodForTypedAccess(walker, method))
                methodsReadOffHolder.append({ id, method });
            if (std::optional<OptionSet<FactOfCell>> askedByRead = fewestFactsOfCellWith([&](OptionSet<FactOfCell> facts) { return !possibleChangesForbid(receivers->possibleChanges, TypedAccess::FindsMethodAbove, facts, true); }))
                asked.add(*askedByRead);
            for (LayoutID member : receivers->members) {
                LayoutID classLayout;
                if (!findMethodDeclaredAboveObjectsOf(walker, member, key, classLayout) || walker.state().guardOfReadsHolds(classLayout, key))
                    continue;
                MemberConditionRecord condition { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, key };
                if (!conditions.contains(condition))
                    conditions.append(condition);
            }
            continue;
        }
        if (!storesByNameIntoSlots.contains(access.opcode())) {
            std::optional<OptionSet<FactOfCell>> askedByRead = askAheadAboutReadOfSlotThatMayBeEmpty(walker, *receivers, field, key);
            if (!askedByRead)
                continue;
            asked.add(*askedByRead);
            if (askedByRead->contains(FactOfCell::HasLayoutPrototype))
                askGuardsOfClasses(key, LookupAbove::OfRead);
            continue;
        }
        bool isDefinition = access.opcode() == Opcode::PropDefine;
        TypedStoreAskedAhead ahead = askAheadAboutTypedStore(walker, access, *receivers, field, key, isDefinition);
        asked.add(ahead.askedOfCell);
        if (ahead.creates && !someStoreCreates)
            firstKeyCreated = key;
        someStoreCreates |= ahead.creates;
        if (ahead.creates && !isDefinition && (ahead.askedOfCell.contains(FactOfCell::HasLayoutPrototype) || storeAsksGuardsWherePrototypeStaysForTypedAccess(walker, *receivers, field, key)))
            askGuardsOfClasses(key, LookupAbove::OfStore);
    }

    // The state knows `this` under the name that it has at the entry.
    Reference root = object.isValue() ? Reference::value(rootOfCellInSweptCode(walker.code(), object.id())) : object;
    bool asksForWholeWord = someStoreCreates && walker.state().factsOfCell(root).contains(FactOfCell::IsLikelyAsAllocated)
        && (receivers->members.size() == 1 ? orderWordTellsKeysOfObjectsOf(walker, receivers->members[0]) : !!findFieldInKnownOrderOfEveryReceiver(walker, receivers->members.span(), firstKeyCreated, unorderedOrderWord));
    asked.remove(walker.state().factsOfCell(object));

    auto pass = [&](Draft&& test) {
        Reference built = walker.isUnreachable() ? Reference() : walker.build(WTF::move(test));
        if (built)
            goOnWhereTestPasses(built);
        return built;
    };
    bool asksForNoExtraProperties = asked.contains(FactOfCell::NoExtraProperties);
    if (!asksForWholeWord && !asked.contains(FactOfCell::NotFrozen) && asksForNoExtraProperties) {
        // Reads alone, one of them under a name that is no field.
        bool asksForFieldsInSlots = asked.contains(FactOfCell::NoFieldOutsideSlot);
        Draft test(Opcode::HasNoExtraProperties, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(asksForFieldsInSlots);
        if (!pass(WTF::move(test)))
            return;
        walker.state().noteFactsOfCell(object, asksForFieldsInSlots ? OptionSet<FactOfCell> { FactOfCell::NoExtraProperties, FactOfCell::NoFieldOutsideSlot } : OptionSet<FactOfCell> { FactOfCell::NoExtraProperties });
    } else if (!asksForWholeWord && !asked.contains(FactOfCell::NotFrozen) && asked.contains(FactOfCell::NoFieldOutsideSlot)) {
        // Reads alone: a frozen object is read like any other.
        Draft test(Opcode::HasNoFieldOutsideSlot, origin);
        test.setObjectOperand(object);
        if (!pass(WTF::move(test)))
            return;
        walker.state().noteFactsOfCell(object, FactOfCell::NoFieldOutsideSlot);
    } else if (asksForWholeWord || asked.containsAny({ FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot })) {
        bool asksForFieldsInSlots = asksForWholeWord || asked.contains(FactOfCell::NoFieldOutsideSlot);
        Draft test(Opcode::IsNotFrozen, origin);
        test.setObjectOperand(object);
        test.setHasNoFieldOutsideSlot(asksForFieldsInSlots);
        test.setHasNoExtraProperties(asksForWholeWord || asksForNoExtraProperties);
        if (!pass(WTF::move(test)))
            return;
        walker.state().noteFactsOfCell(object, asksForFieldsInSlots ? OptionSet<FactOfCell> { FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot } : OptionSet<FactOfCell> { FactOfCell::NotFrozen });
        if (asksForNoExtraProperties)
            walker.state().noteFactsOfCell(object, FactOfCell::NoExtraProperties);
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
    if (asked.contains(FactOfCell::HasLayoutPrototype)) {
        Draft test(Opcode::HasLayoutPrototype, origin);
        test.setObjectOperand(object);
        if (!pass(WTF::move(test)))
            return;
        walker.state().noteFactsOfCell(object, FactOfCell::HasLayoutPrototype);
    }
    if (!conditions.isEmpty()) {
        Draft test(Opcode::AreMembersUnchanged, origin);
        test.setConditions(conditions.span());
        Reference passed = pass(WTF::move(test));
        if (!passed)
            return;
        for (const MemberConditionRecord& condition : conditions) {
            if (condition.kind == MemberConditionKind::MemberUnchanged)
                walker.state().noteGuardOfReadsHolds(condition.layout, condition.key, passed);
            else
                walker.state().noteStoresPassAbove(condition.layout, condition.key, passed);
        }
    }
    // The guard says what a lookup under the name finds, and Code has no word for that: the value itself is asked. Behind the looks above the read runs no code and writes nothing, so it is a look like them, and the arm as written reads again.
    for (auto [id, method] : methodsReadOffHolder) {
        Draft written = walker.draftOf(walker.asWalked(index.value(id)));
        if (walker.hasFailed() || walker.isUnreachable())
            return;
        written.setObjectOperand(object);
        walker.chooseConservativeFields(written);
        // A guard ends because of what is done to an object, and the lookup finds a method where its class has laid it: nothing is done to anything. So the guards that have passed hold behind the read, for which the read at its place asks.
        Effects effects = effectsOfReadingMethodForTypedAccess();
        effects.writes.remove(AbstractHeap::MemberGuards);
        // A fresh object that an operation as written takes is taken to go where anything may be done, and all that is known ends with that. This read hands it to nobody: it is let go in front.
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

// AOTWalkBirths.h. The questions are those of a store that creates its property in a cell which is as it was made: buildStoreForTypedAccess(), lookAtCellInFrontOfStores().
// What settleAllocationOfObjectInChoices() would define by name behind the allocation is not listed.
StoresListedAtBirth findStoresThatAllocationMayList(Walker& walker, LayoutID layout, std::span<const PropertyKeyID> keys, std::span<const TypeID> stored, std::span<const PropertyKeyID> keysStoredFurtherOn)
{
    StoresListedAtBirth listed;
    const Program& program = walker.program();
    const Plan& plan = walker.plan();
    TypeArena& types = walker.types();
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, types.forLayout(layout));
    if (!receivers || receivers->members.size() != 1 || receivers->members[0] != layout || !orderWordTellsKeysOfObjectsOf(walker, layout))
        return listed;

    // A cell is made extensible, with nothing outside its slots. Where it stands is looked at behind the allocation.
    constexpr OptionSet<FactOfCell> knownOfNewCell { FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot, FactOfCell::HasLayoutPrototype };
    Vector<FieldIndex, 16> fields;
    BitVector isListed;
    for (size_t index = 0; index < keys.size(); ++index) {
        PropertyKeyID key = keys[index];
        if (!isLookedUpAsNameForTypedAccess(program, key))
            break;
        FieldOfTypedAccess field = describeFieldOfTypedAccess(walker, *receivers, key, stored[index], true);
        if (field.coverage != CoverageOfTypedAccess::EveryReceiver || !field.liesInOneSlot || !field.isStoredByProgram || !field.takesStoredValue)
            break;
        if (!mayCreateInTypedStore(walker, *receivers, field, key, false, knownOfNewCell, nullptr))
            break;
        // The second store under a name overwrites: the row ends in front of it.
        std::optional<FieldIndex> place = plan.slots().findField(program, layout, key);
        if (!place || isListed.get(*place) || plan.slot(layout, *place).storage != SlotStorage::Inline || !canLeaveSlot(plan.slot(layout, *place).representation))
            break;
        // valueFitsSlotInChoices().
        FieldFacts facts = walker.facts().field(FieldReference { layout, *place });
        TypeID takenByFacts = facts.slotType && facts.facts.contains(FieldFact::SlotHoldsUndefinedAndNull) ? types.withPrimitives(facts.slotType, Primitive::UndefinedOrNull) : facts.slotType;
        if (facts.facts.contains(FieldFact::IsAccessor) || (takenByFacts && plan.slot(layout, *place).type && !types.isSubset(stored[index], takenByFacts)))
            break;
        isListed.set(*place);
        fields.append(*place);
    }

    // The image has an order word only for some beginnings.
    size_t count = plan.keyOrders().lengthOfLongestBeginningWithOrderWord(layout, fields.span());
    std::optional<PlacedAllocation> placed;
    for (; count; --count) {
        placed = plan.allocation(program, layout, AllocationClassID(), fields.span().first(count));
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
    // Each name once.
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
        if (whoAnswersForWhatStandsAboveObjectsOf(walker, layout, key, LookupAbove::OfStore, classLayout) == WhoAnswersForWhatStandsAbove::GuardOfClass && !walker.state().storesPassAbove(classLayout, key))
            listed.conditions.append(MemberConditionRecord { MemberConditionKind::MemberPassedByStores, classLayout, ClassSide::Prototype, key });
    }
    return listed;
}

// `function Node(kind, pos, end) { this.pos = pos; this.end = end; .. }`: whoever constructs makes the cell and hands it over at once, so it arrives as its allocation left it. `Node.call(other)` hands over whatever it likes.
// `result.links.target = symbol; result.links.mapper = mapper`: each read of the field is a value of its own, and they name one place. A guess: what has been stored into the field in between shows at the look.
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
    uint32_t returned = noSummaryNode;
    for (const SummaryEdge& edge : summary.edges) {
        if (summary.nodes[edge.to].kind != SummaryNode::Kind::Result)
            continue;
        if (returned != noSummaryNode && returned != edge.from)
            return;
        returned = edge.from;
    }
    if (returned == noSummaryNode)
        return;
    const SummaryNode& node = summary.nodes[returned];
    if (node.kind != SummaryNode::Kind::ResultOfCall && node.kind != SummaryNode::Kind::Allocation)
        return;
    if (node.kind == SummaryNode::Kind::ResultOfCall && !summary.calls[node.first].isConstruction && remainingDepth) {
        if (FunctionID callee = link.calleeAt(function, summary.calls[node.first].site))
            appendKeysStoredOnResult(walker, callee, keys, remainingDepth - 1);
    }
    for (const SummaryAccess& access : summary.accesses) {
        if ((access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define) || access.isToContents() || access.subject != returned)
            continue;
        if (PropertyKeyID key = access.key ? access.key : link.keyAt(function, access.site))
            keys.appendIfNotContains(key);
    }
}

std::optional<uint32_t> findLikelyOrderWordInFrontOfStores(Walker& walker, ValueID firstOfStretch, std::span<const ValueID> stores, Reference object, TypeID typeOfObject)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, typeOfObject ? typeOfObject : walker.typeOf(object));
    if (!receivers || receivers->members.size() != 1 || walker.state().orderWordOf(object))
        return std::nullopt;
    LayoutID layout = receivers->members[0];
    // An order of a maker begins with what the construction creates. An object that another maker has had in its hands lists more.
    Reference root = object.isValue() ? Reference::value(rootOfCellInSweptCode(walker.code(), object.id())) : object;
    const BodyIndex& index = walker.currentBodyIndex();
    // The body of a constructor function creates what it stores on `this` in the order in which it is written, which is an order of the layout. Whatever has run between two rows of stores, the second row is likely to go on where the first ended.
    ValueID written = stores.empty() ? ValueID() : valueBehindOtherNamesForTypedAccess(index, index.value(stores[0]).objectOperand());
    // Into a literal a spread may have copied keys in the order of its source.
    // `class Exchange { flags: number; constructor() { this.flags = 0; .. } }`: the constructor of a base class allocates `this` itself, calls the initializer of fields and then stores: no literal.
    auto isLiteral = [&] {
        if (index.isParameter(written) || index.opcodeOf(written) != Opcode::NewObject)
            return false;
        LayoutID allocated = index.value(written).as<Opcode::NewObject>().layout();
        return !allocated || walker.program().at(allocated).kind != LayoutKind::Instance;
    };
    bool isThisBeingMade = written && !isLiteral() && isBeingMadeByBodyBeingReadInTypedAccess(walker, written);
    // `const type = createTypeWithSymbol(flags, symbol); type.objectFlags = objectFlags; ..`: the body goes on where the makers in front of it ended, row after row. A guess: the test says.
    bool isAnswerOfCallOfBody = written && !index.isParameter(written) && (index.opcodeOf(written) == Opcode::Call || index.opcodeOf(written) == Opcode::CallDirect || index.opcodeOf(written) == Opcode::Construct);
    // `const result = createSymbol(..); result.links.target = symbol;`: what lies in a field of such an answer was made with it.
    auto isAnswerOfCall = [&](ValueID value) {
        return value && !index.isParameter(value) && (index.opcodeOf(value) == Opcode::Call || index.opcodeOf(value) == Opcode::CallDirect || index.opcodeOf(value) == Opcode::Construct);
    };
    bool liesInFieldOfAnswerOfCallOfBody = written && !index.isParameter(written) && index.opcodeOf(written) == Opcode::PropGet && isAnswerOfCall(valueBehindOtherNamesForTypedAccess(index, index.value(written).objectOperand()));
    bool isLikelyInOrder = isThisBeingMade || isAnswerOfCallOfBody || liesInFieldOfAnswerOfCallOfBody || walker.state().factsOfCell(root).contains(FactOfCell::IsLikelyAsConstructed);
    if (!orderWordTellsKeysOfObjectsOf(walker, layout) || !isLikelyInOrder)
        return std::nullopt;

    // Asking about a store costs far more than this look.
    std::optional<FieldIndex> firstField = stores.empty() ? std::nullopt : walker.plan().slots().findField(walker.program(), layout, walker.asWalked(index.value(stores[0])).key());
    if (!firstField || !walker.plan().keyOrders().continuationCreatingBehindFewestKeys(layout, singleElementSpan(*firstField)))
        return std::nullopt;
    Vector<FieldIndex, 16> created;
    // `this.sendDate = false; .. register(this); this.sendDate = true; this.req = req;`: whatever has run in between, the second store of a name creates nothing.
    Vector<PropertyKeyID, 32> keysStoredOnEveryWayToRow;
    BlockID blockOfRow = index.blockOf(stores[0]);
    if (isAnswerOfCallOfBody && index.opcodeOf(written) != Opcode::Construct) {
        FunctionID callee = index.opcodeOf(written) == Opcode::CallDirect ? index.value(written).as<Opcode::CallDirect>().function() : linkTablesForTypedAccesses(walker).calleeAt(walker.currentFunction(), written);
        if (callee)
            appendKeysStoredOnResult(walker, callee, keysStoredOnEveryWayToRow, 8);
    }
    for (uint32_t earlier = 0; written && earlier < stores[0].index(); ++earlier) {
        ValueID id(earlier);
        if (!isThisBeingMade && earlier > written.index() && !index.isParameter(id) && callsForOrderWord.contains(index.opcodeOf(id))) {
            bool isPassedToCall = false;
            index.value(id).forEachOperand([&](ValueID operand, uint32_t) {
                isPassedToCall |= operand && valueBehindOtherNamesForTypedAccess(index, operand) == written;
            });
            if (isPassedToCall)
                return std::nullopt;
        }
        // `class Exchange { flags: number; constructor() { this.flags = 0; } }`: the initializer of fields has defined `flags` when the constructor stores into it.
        if (!index.isParameter(id) && index.opcodeOf(id) == Opcode::CallDirect && index.dominates(index.blockOf(id), blockOfRow)) {
            BodyValue call = index.value(id);
            FunctionID callee = call.as<Opcode::CallDirect>().function();
            std::span<const ValueID> arguments = call.variadicOperands();
            if (walker.program().at(callee).kind != FunctionKind::Initializer || arguments.empty() || valueBehindOtherNamesForTypedAccess(index, arguments[0]) != written)
                continue;
            const ThinSummary& summary = walker.facts().summaries().at(callee);
            for (const SummaryConstruction& construction : summary.constructions) {
                if (construction.object != SummaryConstruction::Object::This)
                    continue;
                // `class Context { path = "/"; status = code => { this.code = code; }; text = ..; constructor(path) { this.path = path; } }`: the arrow captures `this`, so what the initializer defines behind it is filled for nobody's proof.
                // The object lists it all the same when the constructor goes on, and this is a guess.
                for (PropertyKeyID key : summary.keysFilledAtEnds.span().subspan(construction.firstKeyFilledAtEnd, construction.numberOfKeysFilledAtEnd))
                    keysStoredOnEveryWayToRow.appendIfNotContains(key);
                for (const SummaryConstruction::Key& defined : summary.keysOfConstructions.span().subspan(construction.firstKey, construction.numberOfKeys)) {
                    if (defined.isFilled)
                        keysStoredOnEveryWayToRow.appendIfNotContains(defined.key);
                }
            }
            continue;
        }
        if (index.isParameter(id) || !accessesThatMayStoreUnderNameForOrderWord.contains(index.opcodeOf(id)) || !index.dominates(index.blockOf(id), blockOfRow))
            continue;
        BodyValue store = walker.asWalked(index.value(id));
        if (storesByNameIntoSlots.contains(store.opcode()) && store.key() && namesSamePlaceForOrderWord(index, valueBehindOtherNamesForTypedAccess(index, store.objectOperand()), written))
            keysStoredOnEveryWayToRow.appendIfNotContains(store.key());
    }
    // `class Context { #raw = 0; #path = "/"; env = {}; .. }`: the definition of a private name asks the cell nothing, so it is none of the stores. It creates a field as they do, and the test stands in front of the stretch:
    // the word in front of `env` lists the private names, which the cell that has just been made does not have yet.
    for (uint32_t earlier = firstOfStretch.index(); written && earlier < stores[0].index(); ++earlier) {
        ValueID id(earlier);
        if (index.isParameter(id) || index.opcodeOf(id) != Opcode::PrivateDefine)
            continue;
        BodyValue definition = index.value(id);
        if (valueBehindOtherNamesForTypedAccess(index, definition.objectOperand()) != written)
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
        bool isLikelyFilled = keysStoredOnEveryWayToRow.contains(key);
        if (isLikelyFilled && (overwritesUnprovenSlot || (!isOutOfLine && !isThisBeingMade)))
            continue;
        FieldOfTypedAccess described = describeFieldOfTypedAccess(walker, *receivers, key, { }, true);
        if (!isLikelyFilled && !isThisBeingMade && !described.isAppended && !isCreatedAfterConstruction(walker.plan().keyOrders(), layout, *field)) {
            isLikelyFilled = true;
            keysStoredOnEveryWayToRow.append(key);
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
    // `this.url = url; .. this._events = events;`: the layout may have an order of the class above that begins with `_events`. The object that comes to this row lists what the body has stored on it.
    Vector<FieldIndex, 32> fieldsStoredOnEveryWayToRow;
    for (PropertyKeyID key : keysStoredOnEveryWayToRow) {
        if (std::optional<FieldIndex> field = walker.plan().slots().findField(walker.program(), layout, key); field && walker.plan().keyOrders().continuationCreatingBehindFewestKeys(layout, singleElementSpan(*field)))
            fieldsStoredOnEveryWayToRow.append(*field);
    }
    if (created.isEmpty())
        return overwritesUnprovenSlot ? walker.plan().keyOrders().findOrderWordOfShortestBeginningThatLists(layout, fieldsStoredOnEveryWayToRow.span()) : std::nullopt;
    for (size_t count = fieldsStoredOnEveryWayToRow.isEmpty() ? 0 : created.size(); count; --count) {
        if (std::optional<KeyOrderContinuation> continuation = walker.plan().keyOrders().continuationCreatingBehindFewestKeys(layout, created.span().first(count), fieldsStoredOnEveryWayToRow.span()))
            return continuation->orderWordBefore;
    }
    if (!fieldsStoredOnEveryWayToRow.isEmpty())
        return std::nullopt;
    // The longest beginning of them that some order lists one behind the other.
    for (size_t count = created.size(); count; --count) {
        if (std::optional<KeyOrderContinuation> continuation = walker.plan().keyOrders().continuationCreatingBehindFewestKeys(layout, created.span().first(count)))
            return continuation->orderWordBefore;
    }
    return std::nullopt;
}

void testOrderWordInFrontOfStores(Walker& walker, ValueID firstStore, Reference object, uint32_t orderWord, NOESCAPE const ScopedLambda<void(Reference test)>& goOnWhereTestPasses)
{
    std::optional<ReceiversOfTypedAccess> receivers = findReceiversOfTypedAccess(walker, walker.typeOf(object));
    if (!receivers || receivers->members.size() != 1)
        return;
    LayoutID layout = receivers->members[0];
    Origin origin = walker.originOf(walker.currentBodyIndex().value(firstStore));
    auto pass = [&](Draft&& test) {
        Reference built = walker.isUnreachable() ? Reference() : walker.build(WTF::move(test));
        if (built)
            goOnWhereTestPasses(built);
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
    walker.state().noteFactsOfCell(object, { FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot });
    walker.state().noteOrderWord(object, orderWord);
}

void noteThisAtEntry(Walker& walker, Reference thisValue)
{
    const Function& function = walker.program().at(walker.unit().function);
    if (!walker.has(Mechanism::TypedOperations) || !thisValue || !thisValue.isValue() || !function.takesThisFromConstruction() || !function.constructedClass || function.isCoroutine())
        return;
    walker.state().noteFactsOfCell(thisValue, FactOfCell::IsLikelyAsAllocated);

    // Where Link has seen that whoever enters constructs, the cell was made for this entry and nothing else has it yet. Which keys its allocation has listed is still looked at.
    const LinkTables& link = linkTablesForTypedAccesses(walker);
    FunctionID id = walker.unit().function;
    if (id.index() >= link.functions.size() || !link.function(id).everyEntryIsConstruction)
        return;
    walker.state().noteFactsOfCell(thisValue, { FactOfCell::NotFrozen, FactOfCell::NoFieldOutsideSlot });
    if (walker.has(Mechanism::EscapeAnalysis))
        walker.state().noteFresh(thisValue);
}

// ---- Variables and environments. The operation is the one that was written. Walker::build() types it, as it does GetCapture, by Walker::positions(), which answers with the LinkedCell of the place.
// What is added here is that the state learns what the place holds.

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
        draft.setFollowsItsStore(true);
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
    if (walker.has(Mechanism::Folding) && storesValue && leavesDeadZoneForGood(walker.program(), value.variable()))
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
    // Link's OrderOfScopes speaks of the scope that the function has captured, and a function gets at no other scope of the layout.
    // A slow version reads none of Link's tables: it is what runs where a proof of the fast one has not held.
    bool isFilledByOrderOfScope = walker.unit().role != CodeRole::SlowBody && walker.facts().linked().orderOfScopes.isFilledWhenever(walker.currentFunction(), field);
    if (walker.has(Mechanism::Folding) && (walker.state().isPastDeadZone(environment, field) || isFilledByOrderOfScope))
        draft.setFollowsItsStore(true);
    Reference read = walker.build(WTF::move(draft));
    walker.bind(value.id(), read);
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
    noteValueOfStableBinding(walker, environment, FieldReference { value.layout(), value.field() }, stored);
    return true;
}

// `super.x`: the lookup starts at what stands above the home object, which the type of the base names, and a getter runs with the receiver as `this`.
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

// Where the lookup comes to no setter the store lays the property on the receiver, throws or does nothing: that stays closed.
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
    // A free name that finds a member of the global object which never changes is that member: no script can declare the name in front of it.
    if (opcode == Opcode::PropGet && value.isFreeName())
        return operands.areOnlyTypes || operands.object ? buildReadOfBuiltinMember(walker, value, operands.typeOfObject, operands.object) : TypedForm::None;
    // No access by name has a typed form on a receiver of which nothing is proven.
    if (walker.types().saysNothing(operands.typeOfObject))
        return TypedForm::None;
    if (opcode == Opcode::PropGet)
        return buildReadForTypedAccess(walker, value, operands, key);
    if (opcode == Opcode::PropHas)
        return buildTestForTypedAccess(walker, value, operands, key);

    operands.typeOfStored = inputs.typeOf(value.valueOperand());
    if (!operands.areOnlyTypes)
        operands.stored = walker.referenceOf(value.valueOperand());
    if (opcode == Opcode::PropSet)
        return value.isFreeName() ? TypedForm::None : buildStoreForTypedAccess(walker, value, operands, key, value.mode(), false);
    return buildStoreForTypedAccess(walker, value, operands, key, ECMAMode::Strict, true);
}

// ---- Copies of properties: `{ ...source, more }`, `const { a, ...rest } = source`
// The front end makes an empty object of the layout that the checker has for the literal or for the rest, and copies into it. Where the properties of the source are known by name and in their order,
// the object is made with them: loads, and an allocation that lists them. The empty object is built as ever, nothing comes to need it, and the sweep drops it.
// A source of the layout of the empty object itself hands over the words of its cell, whichever of the keys it has: one operation, as long for two fields as for two hundred.

// How many closed operations stand for a copy by the run time at the least: it lists the keys, reads each and defines each.
static constexpr uint32_t closedOperationsOfCopyOfProperties = 3;

// The layout of the object that the copy fills, where nothing has named the object since it was made empty, a few operations further up in the block. Null: there is no such object.
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

// The bits of its slot are all there is to the property, and a copy takes it.
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

// By field of the source that is copied: the field of the target under the same key. Nothing: some property is no plain one, or the target has no field for it.
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

// The words of a cell of the layout are all there is to its properties, and the order word says which of them it has.
static bool wordsOfCellAreItsPropertiesForCopyInTypedAccess(const Walker& walker, LayoutID layout)
{
    const Program& program = walker.program();
    if (program.at(layout).kind != LayoutKind::Record || !orderWordTellsKeysOfObjectsOf(walker, layout) || !walker.plan().slots().layout(layout).appendedKeys.isEmpty())
        return false;
    for (FieldIndex index = 0; index < program.at(layout).fields.size(); ++index) {
        if (!isPlainPropertyForCopyInTypedAccess(walker, { layout, index }))
            return false;
    }
    return true;
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

// Nothing but this code has the source, and the state knows what it holds.
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

// The source is proven or expected to be an object of one layout. One word of the cell says which of its fields it has, in which order it got them, and whether it has anything beside them.
// Of the layout of the empty object: the words of the cell are copied into it, whatever the word says but that there is more. Where that is refused: the copy as written, into the same object.
// Of another layout: behind the tests a load for each field, and where one fails an empty object and the copy as written, in a block of its own. The two meet behind the copy.
static bool buildCopyBehindTestsForTypedAccess(Walker& walker, const BodyValue& copy, LayoutID layout)
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

    // One test asks for one layout: of a family, the first member as the program lists them that has objects. An object of another member goes by the copy as written.
    const Plan& plan = walker.plan();
    const Program& program = walker.program();
    LayoutID layoutOfSource;
    for (LayoutID member : receivers->members) {
        if (program.at(member).kind == LayoutKind::Record && !plan.shapes().shapesOf(member).isEmpty()) {
            layoutOfSource = member;
            break;
        }
    }
    if (!layoutOfSource)
        return false;

    bool copiesWords = layoutOfSource == layout && !operation.excludedKeys().size() && wordsOfCellAreItsPropertiesForCopyInTypedAccess(walker, layout);
    std::span<const FieldIndex> order;
    Vector<FieldIndex, 8> copied;
    std::optional<Vector<FieldIndex, 8>> fields;
    if (!copiesWords) {
        // The order that lists every field: an object that lacks one, or got them otherwise, goes by the copy as written too. Where the source says that objects leave a field out, that is most of them.
        if (!plan.slots().layout(layoutOfSource).appendedKeys.isEmpty() || !std::ranges::all_of(program.at(layoutOfSource).fields, [](const Field& field) { return field.flags.contains(FieldFlags::IsRequired); }))
            return false;
        for (uint32_t number = 0; number < plan.keyOrders().numberOfOrdersOf(layoutOfSource); ++number) {
            if (plan.keyOrders().order(layoutOfSource, number).size() == program.at(layoutOfSource).fields.size())
                order = plan.keyOrders().order(layoutOfSource, number);
        }
        if (order.empty() || !plan.keyOrders().findOrderWord(layoutOfSource, order))
            return false;
        if (!(fields = fieldsOfTargetForCopyInTypedAccess(walker, copy, layout, layoutOfSource, order, copied)))
            return false;
    }
    TypeID typeOfLayout = types.forLayout(layoutOfSource);
    if (receivers->members.size() != 1 || !holdsOneMember(receivers->set)) {
        isProven = false;
        receivers = findReceiversOfTypedAccess(walker, typeOfLayout);
        if (!receivers)
            return false;
    }
    if (!shouldOpenGuess(walker, source, typeOfLayout, isDeclared, closedOperationsOfCopyOfProperties))
        return false;

    Origin origin = walker.originOf(copy);
    Draft written = walker.draftOf(copy);
    if (walker.hasFailed())
        return true;
    walker.noteGuessOpened();
    BlockID asWritten = walker.addBlock(BlockFrequency::Unlikely);
    BlockID behindCopy = walker.addBlock();
    auto goOnWhere = [&](Reference test) {
        BlockID passed = walker.addBlock();
        walker.branch(test, passed, asWritten);
        walker.continueIn(passed);
    };

    if (!isProven) {
        Draft test(Opcode::ShapeTest, origin);
        test.setObjectOperand(source);
        test.setLayouts(receivers->set);
        Reference hasShape = walker.build(WTF::move(test));
        goOnWhere(hasShape);
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(source);
        narrowed.setTestOperand(hasShape);
        narrowed.setOperandIndex(OperandIndex::ShapeTest::object);
        narrowed.setIsOnTakenEdge(true);
        source = walker.build(WTF::move(narrowed));
    }
    if (copiesWords) {
        Draft words(Opcode::TryCopyFields, origin);
        words.setObjectOperand(written.targetOperand());
        words.setSourceOperand(source);
        words.setLayout(layout);
        goOnWhere(walker.build(WTF::move(words)));
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
    Draft hasOrder(Opcode::HasKeyOrder, origin);
    hasOrder.setObjectOperand(source);
    hasOrder.setLayout(layoutOfSource);
    hasOrder.setFields(order);
    goOnWhere(walker.build(WTF::move(hasOrder)));
    Draft hasNothingElse(Opcode::HasNoExtraProperties, origin);
    hasNothingElse.setObjectOperand(source);
    hasNothingElse.setHasNoFieldOutsideSlot(true);
    goOnWhere(walker.build(WTF::move(hasNothingElse)));

    Vector<Reference, 8> values;
    for (FieldIndex index : copied) {
        Draft read(Opcode::GetSlot, origin);
        fillSlotOperationForTypedAccess(read, source, *receivers, program.at(layoutOfSource).fields[index].key);
        values.append(walker.build(WTF::move(read)));
    }
    if (Reference object = walker.isUnreachable() ? Reference() : buildTargetOfCopyForTypedAccess(walker, origin, layout, fields->span(), values.span()))
        walker.jumpTo(behindCopy, singleElementSpan(object));

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

static bool buildCopyOfPropertiesForTypedAccess(Walker& walker, const BodyValue& copy)
{
    if (!copy.as<Opcode::CopyDataProperties>().excludedOperands().empty())
        return false;
    LayoutID layout = layoutOfEmptyTargetOfCopyForTypedAccess(walker, copy);
    return layout && (buildCopyOfFreshObjectForTypedAccess(walker, copy, layout) || buildCopyBehindTestsForTypedAccess(walker, copy, layout));
}

bool buildTypedAccess(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::CopyDataProperties) {
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
    if (accessesUnderPrivateNames.contains(opcode))
        return buildAccessUnderPrivateNameForTypedAccess(walker, value, InputsOfTypedOperation(walker, nullptr)) == TypedForm::Built;
    return buildAccessByNameForTypedAccess(walker, value, InputsOfTypedOperation(walker, nullptr)) == TypedForm::Built;
}

// A variable and a field of an environment are built as written whatever is proven.
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
    if (!type.hasOnlyLayouts())
        return type.hasUnboundedUnknown() ? "objects that nobody describes"_s : "objects without a layout (built-in classes, functions, named objects)"_s;
    if (type.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow))
        return "may have a layout below"_s;
    if (!holdsOneMember(type.layouts) && !familiesForTypedAccesses(walker).familyOf(type.layouts))
        return "several layouts that are no family"_s;
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
        return whyNoMethod;
    }
    if (field.coverage == CoverageOfTypedAccess::SomeReceivers)
        return "the name is a field of some layouts of the family only"_s;
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
