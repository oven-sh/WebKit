/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTableBindings.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLimits.h"
#include "AOTOperatorForms.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCompiledMethods.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTWalkLoopsOverGeneratorsBuilder.h"
#include "AOTWalkTextBuilderRows.h"
#include "AOTWalkNumberForms.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkResultsInParts.h"
#include "AOTWalkStrings.h"
#include "AOTWalkStringFolds.h"
#include "AOTWalkKnownCalls.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkListedOwnKeys.h"
#include "AOTWalkWrittenDescriptors.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet callsOfListsInWalkBuiltins { Opcode::CallList, Opcode::ConstructList };
static constexpr OpcodeSet opcodesOfWalkBuiltins = callsOfListsInWalkBuiltins | OpcodeSet { Opcode::Call, Opcode::Construct };

static bool isConstantLocationInWalkBuiltins(const Program& program, const Facts& facts, BuiltinID member)
{
    const BuiltinTable& table = program.builtins();
    if (table.has(BuiltinMark::MayBeAbsent, member) || program.isUnlocked(member) || program.isUnlocked(table.canonicalID(member)))
        return false;
    if (table.at(member).mutability == BuiltinMutability::AlwaysLocked || table.isLocationOfLockedObject(member))
        return true;
    return facts.builtinLocation(member) == BuiltinLocationState::Constant;
}

BuiltinID findConstantMemberFrom(const Program& program, const Facts& facts, BuiltinID start, PropertyKeyID key, MembersBehindWatch watch)
{
    const BuiltinTable& table = program.builtins();
    if (liesInThread(key) || !builtinMemberNameOf(program, key))
        return { };
    MembersUnderKey members(program, key);
    BuiltinID holder = table.canonicalID(start);
    while (true) {
        if (compiledModuleWithObject(program, holder) || table.at(table.rootOf(holder)).scope != BuiltinScope::Realm)
            return { };
        if (BuiltinID member = members.findOwnMember(holder)) {
            bool liesOnHolder = !table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
            bool isBelieved = isConstantLocationInWalkBuiltins(program, facts, member) || (watch == MembersBehindWatch::AreAsBuilt && facts.isBehindWatch(member));
            return liesOnHolder && isBelieved ? member : BuiltinID();
        }
        const Builtin& object = table.at(holder);
        auto opened = watch == MembersBehindWatch::AreAsBuilt ? MembersUnderKey::NamesOpenedForTestRun::AreAbsent : MembersUnderKey::NamesOpenedForTestRun::MayLieOnHolder;
        if (!listsEveryMemberUnder(program, holder, key) || !members.isClosedKey(holder, opened) || !table.hasLockedPrototypeLink(holder))
            return { };
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
        case BuiltinPrototypeLink::Null:
            return { };
        case BuiltinPrototypeLink::Builtin:
            holder = table.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

BuiltinID findConstantMemberOnceForBuild(const Walker& walker, BuiltinID start, PropertyKeyID key)
{
    static constexpr uint8_t number = static_cast<uint8_t>(AnswerKeptInArena::ConstantMemberOfBuiltin);
    TypeArena& types = walker.types();
    if (types.keeps() != ArenaKeeps::UntilReset || liesInThread(key))
        return findConstantMemberFrom(walker.program(), walker.facts(), start, key);
    if (std::optional<uint32_t> kept = types.rememberedAnswer(number, start.index(), key.bits()))
        return BuiltinID(*kept);
    BuiltinID member = findConstantMemberFrom(walker.program(), walker.facts(), start, key);
    types.rememberAnswer(number, start.index(), key.bits(), member.index());
    return member;
}

static bool createdClosureLacksName(const Program& program, FunctionID function, PropertyKeyID key)
{
    const Function& record = program.at(function);
    if (record.isCoroutine() || isClassConstructorOf(record.kind))
        return false;
    for (BuiltinName own : { BuiltinName(BuiltinNames::name), BuiltinName(BuiltinNames::length), BuiltinName(BuiltinNames::prototype), BuiltinName(BuiltinNames::arguments), BuiltinName(BuiltinNames::caller) }) {
        if (program.keyOf(own) == key)
            return false;
    }
    return true;
}

static bool lookupFromClosuresGoesToFunctionPrototypeInWalkBuiltins(const Walker& walker, FunctionID function, PropertyKeyID key)
{
    return walker.facts().linked().closuresLackName(function, key) && createdClosureLacksName(walker.program(), function, key);
}

static constexpr AbstractHeapSet heapsUnderTestOfBeingAsMadeInWalkBuiltins { AbstractHeap::NamedProperties, AbstractHeap::PrototypeLinks };

static BuiltinClassID classOfPrimitivesWatchedInWalkBuiltins(const Walker& walker, TypeID value)
{
    if (!walker.program().admitsReplacedBuiltins || !value)
        return { };
    Type type = walker.types().at(value);
    return type.isJavaScriptValue() && !type.hasObjects() ? primitiveClassOf(type.primitives) : BuiltinClassID();
}

void noteObjectPassedTestOfBeingAsMade(Walker& walker, Reference object, Reference test)
{
    if (!walker.program().admitsReplacedBuiltins || !test || !test.isValue())
        return;
    if (BuiltinClassID ofPrimitives = classOfPrimitivesWatchedInWalkBuiltins(walker, walker.typeOf(object))) {
        noteKnown(walker, KnownNote::PassedTestOfBeingAsMade, { }, { }, ofPrimitives.index() + 1, LocationMasks(heapsUnderTestOfBeingAsMadeInWalkBuiltins), 1);
        return;
    }
    noteKnown(walker, KnownNote::PassedTestOfBeingAsMade, { }, object, 0, LocationMasks(heapsUnderTestOfBeingAsMadeInWalkBuiltins), 1);
}

static MembersBehindWatch membersBehindWatchAboveInWalkBuiltins(Walker& walker, Reference object)
{
    if (!walker.program().admitsReplacedBuiltins || !object)
        return MembersBehindWatch::AreOpen;
    bool hasPassed = false;
    if (BuiltinClassID ofPrimitives = classOfPrimitivesWatchedInWalkBuiltins(walker, walker.typeOf(object)))
        hasPassed = !!findKnown(walker, KnownNote::PassedTestOfBeingAsMade, { }, { }, ofPrimitives.index() + 1);
    else
        hasPassed = object.isValue() && findKnown(walker, KnownNote::PassedTestOfBeingAsMade, { }, object);
    return hasPassed ? MembersBehindWatch::AreAsBuilt : MembersBehindWatch::AreOpen;
}

static BuiltinName memberNameOf(const Program& program, PropertyKeyID key)
{
    if (liesInThread(key))
        return { };
    return isPrivateAliasOfBuiltinMember(program, key) ? builtinMemberNameOf(program, key).name : program.builtinNameOf(key);
}

static BuiltinID findMemberReadOffInWalkBuiltins(const Walker& walker, TypeID object, PropertyKeyID key, bool isAsItsClassMakesIt, MembersBehindWatch watch = MembersBehindWatch::AreOpen)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    TypeArena& types = walker.types();
    object = types.withoutNullish(object);
    Type type = types.at(object);
    if (type.isNever() || !type.isJavaScriptValue())
        return { };

    BuiltinClassID builtinClass;
    bool isPrimitive = !type.hasObjects();
    bool standsAboveViews = false;
    bool standsBesideInstancesBelow = false;
    if (isPrimitive)
        builtinClass = primitiveClassOf(type.primitives);
    else if (type.isObject()) {
        if (BuiltinID holder = singleBuiltinOf(types, type))
            return findConstantMemberOnceForBuild(walker, holder, key);
        FunctionID function = singleFunctionOf(types, type);
        if (function && lookupFromClosuresGoesToFunctionPrototypeInWalkBuiltins(walker, function, key))
            return findConstantMemberOnceForBuild(walker, Builtins::FunctionPrototype, key);
        if (!isAsItsClassMakesIt)
            return { };
        if (function)
            return createdClosureLacksName(program, function, key) ? findConstantMemberOnceForBuild(walker, Builtins::FunctionPrototype, key) : BuiltinID();
        if (const ClassPart* part = singleBuiltinClassOf(types, type))
            builtinClass = part->builtinClass;
        else if (type.hasOnlyClassObjects()) {
            builtinClass = classAboveViewsOfSeveralKinds(program, types, object);
            standsAboveViews = !!builtinClass;
            if (!builtinClass) {
                builtinClass = classOfViewBesideInstancesBelowIt(walker, object);
                standsBesideInstancesBelow = !!builtinClass;
            }
        }
    }
    BuiltinName name = builtinClass ? memberNameOf(program, key) : BuiltinName();
    if (!name)
        return { };

    BuiltinID prototype;
    for (BuiltinClassID current = builtinClass; current && !prototype; current = table.at(current).parent)
        prototype = table.at(current).prototype;
    BuiltinID member = prototype ? findConstantMemberOnceForBuild(walker, prototype, key) : BuiltinID();
    if (!member && prototype && watch == MembersBehindWatch::AreAsBuilt)
        member = findConstantMemberFrom(program, walker.facts(), prototype, key, watch);
    if (!member || table.isRedefinedBelow(builtinClass, name))
        return { };
    if (isPrimitive)
        return member;
    if (standsBesideInstancesBelow) {
        BuiltinID ofInstancesBelow = findMemberReadOffInWalkBuiltins(walker, types.builtinClass(classBelowWithTestedStructure(table, builtinClass)), key, isAsItsClassMakesIt, watch);
        if (!ofInstancesBelow || table.canonicalID(ofInstancesBelow) != table.canonicalID(member))
            return { };
    }

    if (!standsAboveViews && !table.mayHavePristineInstances(builtinClass) && pristineTestOf(table, builtinClass) != PristineTest::InstanceStructure)
        return { };
    return isAsItsClassMakesIt ? member : BuiltinID();
}

BuiltinID findConstantMemberReadOff(const Walker& walker, TypeID object, PropertyKeyID key)
{
    return object ? findMemberReadOffInWalkBuiltins(walker, object, key, false) : BuiltinID();
}

BuiltinID findMemberAskedByItsByte(const Walker& walker, BuiltinID holder, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (!program.admitsReplacedBuiltins || !holder || !key || liesInThread(key) || !walker.has(Mechanism::BuiltinCalls))
        return { };
    BuiltinName name = program.builtinNameOf(key);
    if (!name)
        return { };
    const BuiltinTable& table = program.builtins();
    holder = table.canonicalID(holder);
    if (compiledModuleWithObject(program, holder) || table.at(table.rootOf(holder)).scope != BuiltinScope::Realm)
        return { };
    BuiltinID member = table.findOwnMember(holder, name);
    if (!member || table.canonicalID(member) != member || !walker.facts().isBehindWatch(member))
        return { };
    return isObjectKind(table.at(member).kind) && BuiltinCellKey::object(member).hasCell(table) ? member : BuiltinID();
}

BuiltinID findMemberAskedByItsByteOffType(const Walker& walker, TypeID typeOfHolder, PropertyKeyID key)
{
    if (!walker.program().admitsReplacedBuiltins || !typeOfHolder || holdsOnlyPrimitives(typeOfHolder))
        return { };
    const Type& written = walker.types().recordOf(typeOfHolder);
    return written.isObject() ? findMemberAskedByItsByte(walker, singleBuiltinOf(walker.types(), written), key) : BuiltinID();
}

TypeID typeOfMemberAskedByItsByte(Walker& walker, const BodyValue& read, TypeID typeOfHolder)
{
    BuiltinID member = findMemberThatGuessAsksByItsByte(walker, read, typeOfHolder);
    return member ? typeOfBuiltinObject(walker.typing(), member) : TypeID();
}

Reference buildTestOfMemberAsBuilt(Walker& walker, BuiltinID member, Origin origin)
{
    Draft test(Opcode::IsMemberAsBuilt, origin);
    walker.chooseConservativeFields(test);
    test.setBuiltin(member);
    return walker.build(WTF::move(test));
}

bool askWhetherMemberOfBuiltinIsAsBuilt(Walker& walker, const BodyValue& question)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    Reference object = walker.referenceOf(question.objectOperand());
    TypeID typeOfObject = object ? walker.typeOf(object) : TypeID();
    PropertyKeyID key = question.key();
    if (question.declaredByOperand() || !walker.has(Mechanism::BuiltinCalls) || !typeOfObject || holdsOnlyPrimitives(typeOfObject) || !key || liesInThread(key))
        return false;
    const Type& written = walker.types().recordOf(typeOfObject);
    BuiltinID holder = written.isObject() ? singleBuiltinOf(walker.types(), written) : BuiltinID();
    BuiltinName name = holder ? program.builtinNameOf(key) : BuiltinName();
    if (!name)
        return false;
    holder = table.canonicalID(holder);
    if (compiledModuleWithObject(program, holder) || table.at(table.rootOf(holder)).scope != BuiltinScope::Realm)
        return false;
    BuiltinID member = table.findOwnMember(holder, name);
    if (!member || table.canonicalID(member) != member || table.has(BuiltinMark::MayBeAbsent, member))
        return false;
    if (isConstantLocationInWalkBuiltins(program, walker.facts(), member)) {
        Draft answer(Opcode::Boolean, walker.originOf(question));
        answer.setBoolean(true);
        walker.bind(question.id(), walker.constant(WTF::move(answer)));
        return true;
    }
    if (!isMemberAskedByByteAlone(table, member) && !cellThatNamesMemberAskedByItsByte(table, member).hasCell(table))
        return false;
    walker.bind(question.id(), buildTestOfMemberAsBuilt(walker, member, walker.originOf(question)));
    return true;
}

void noteMemberPassedTestOfBeingAsBuilt(Walker& walker, BuiltinID member)
{
    noteKnown(walker, KnownNote::MemberOfBuiltinIsAsBuilt, { }, { }, member.index(), LocationMasks(AbstractHeapSet { AbstractHeap::NamedProperties }), 1, true);
}

Reference buildTestOfHolderWithoutAddedNames(Walker& walker, BuiltinID holder, Origin origin)
{
    Draft test(Opcode::IsHolderWithoutAddedNames, origin);
    walker.chooseConservativeFields(test);
    test.setBuiltin(holder);
    return walker.build(WTF::move(test));
}

void noteHolderPassedTestOfHavingNoAddedNames(Walker& walker, BuiltinID holder)
{
    noteKnown(walker, KnownNote::HolderOfBuiltinHasNoAddedNames, { }, { }, holder.index(), LocationMasks(AbstractHeapSet { AbstractHeap::NamedProperties }), 1, true);
}

BuiltinID findConstantMemberReadOff(Walker& walker, Reference object, PropertyKeyID key)
{
    TypeID type = object ? walker.typeOf(object) : TypeID();
    if (BuiltinID member = findMemberAskedByItsByteOffType(walker, type, key); member && findKnown(walker, KnownNote::MemberOfBuiltinIsAsBuilt, { }, { }, member.index())) {
        walker.askByteOfMemberAgainWhereNoteIsLeanedOn(member);
        return member;
    }
    if (!type)
        return { };
    bool isAsMade = isKnownPristine(walker, object);
    BuiltinID believed = findMemberReadOffInWalkBuiltins(walker, type, key, isAsMade, membersBehindWatchAboveInWalkBuiltins(walker, object));
    if (believed)
        walker.verifyPristineObject(object);
    if (believed || !walker.program().admitsReplacedBuiltins)
        return believed;
    BuiltinID asked = findMemberReadOffInWalkBuiltins(walker, type, key, isAsMade, MembersBehindWatch::AreAsBuilt);
    HoldersBelowMember below;
    bool hasPassed = asked && beliefInMemberFoundOnObjectAsMade(walker, asked, type, key, &below) == BeliefInMemberAbove::IsAskedByItsByte && findKnown(walker, KnownNote::MemberOfBuiltinIsAsBuilt, { }, { }, asked.index());
    hasPassed = hasPassed && std::ranges::all_of(below, [&](BuiltinID holder) {
        return !!findKnown(walker, KnownNote::HolderOfBuiltinHasNoAddedNames, { }, { }, holder.index());
    });
    if (!hasPassed)
        return { };
    walker.askByteOfMemberAgainWhereNoteIsLeanedOn(asked);
    return asked;
}

BuiltinID findMemberOnObjectAsMade(const Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    return findMemberOnObjectAsMade(walker, typeOfObject, read.key());
}

BuiltinID findMemberOnObjectAsMade(const Walker& walker, TypeID typeOfObject, PropertyKeyID key)
{
    return findMemberReadOffInWalkBuiltins(walker, typeOfObject, key, true, MembersBehindWatch::AreAsBuilt);
}

BeliefInMemberAbove beliefInMemberFoundOnObjectAsMade(const Walker& walker, BuiltinID member, TypeID typeOfObject, PropertyKeyID key, HoldersBelowMember* holdersBelow)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    Type type = walker.types().at(walker.types().withoutNullish(typeOfObject));
    const ClassPart* part = type.isObject() ? singleBuiltinClassOf(walker.types(), type) : nullptr;
    BuiltinID next;
    for (BuiltinClassID current = part ? part->builtinClass : primitiveClassOf(type.primitives); current && !next; current = table.at(current).parent)
        next = table.at(current).prototype;
    if (next && findConstantMemberFrom(program, walker.facts(), next, key) == member)
        return BeliefInMemberAbove::IsConstant;
    BuiltinID holderOfMember = table.canonicalID(table.at(member).holder);
    bool objectLiesOnSameHolder = table.canonicalID(table.at(table.canonicalID(member)).holder) == holderOfMember;
    if (!walker.facts().isBehindWatch(member) || !objectLiesOnSameHolder)
        return BeliefInMemberAbove::RestsOnByteOfClass;
    for (BuiltinID above = next; above;) {
        BuiltinID holder = table.canonicalID(above);
        if (holder == holderOfMember)
            return BeliefInMemberAbove::IsAskedByItsByte;
        if (!holdersBelow)
            break;
        holdersBelow->append(holder);
        above = table.at(holder).prototypeLink == BuiltinPrototypeLink::Builtin ? table.at(holder).inheritsFrom : BuiltinID();
    }
    if (holdersBelow)
        holdersBelow->clear();
    return BeliefInMemberAbove::RestsOnByteOfClass;
}

static constexpr PristineAspectSet aspectsOfObjectAsMade { PristineAspects::Storage, PristineAspects::Names, PristineAspects::InheritedNamesKept, PristineAspects::Prototype };

static ALWAYS_INLINE BuiltinClassID classThatHasTestOfObjectAsMadeInWalkBuiltins(const Walker& walker, TypeID object)
{
    Type type = walker.types().at(object);
    const ClassPart* part = type.isObject() ? singleBuiltinClassOf(walker.types(), type) : nullptr;
    if (!part) {
        if (FunctionID function = type.isObject() ? singleFunctionOf(walker.types(), type) : FunctionID(); function && !walker.program().at(function).isCoroutine() && !isClassConstructorOf(walker.program().at(function).kind))
            return BuiltinClasses::Function;
        if (!type.hasOnlyClassObjects())
            return { };
        if (BuiltinClassID above = classAboveViewsOfSeveralKinds(walker.program(), walker.types(), object))
            return above;
        BuiltinClassID beside = classOfViewBesideInstancesBelowIt(walker, object);
        return beside && pristineTestOf(walker.program().builtins(), beside) == PristineTest::MayPass ? beside : BuiltinClassID();
    }
    PristineTest test = pristineTestOf(walker.program().builtins(), part->builtinClass);
    return test == PristineTest::MayPass || test == PristineTest::InstanceStructure ? part->builtinClass : BuiltinClassID();
}

static BuiltinClassID classWhoseInstancesAreTestedInWalkBuiltins(const Walker& walker, TypeID object)
{
    return walker.has(Mechanism::EscapeAnalysis) ? classThatHasTestOfObjectAsMadeInWalkBuiltins(walker, object) : BuiltinClassID();
}

enum class TestOfInstanceInWalkBuiltins : uint8_t { AsksStorageToo, AsksStorageTooOfCellAlone, AsksOneBitOfCell };
static Reference buildTestOfInstanceAsMadeInWalkBuiltins(Walker&, Reference object, BuiltinClassID, Origin, TestOfInstanceInWalkBuiltins = TestOfInstanceInWalkBuiltins::AsksStorageToo);

Reference buildTestOfObjectAsMade(Walker& walker, Reference object, Origin origin, TestOfObjectAsMade asked)
{
    if (BuiltinClassID ofPrimitives = classOfPrimitivesWatchedInWalkBuiltins(walker, walker.typeOf(object))) {
        if (asked == TestOfObjectAsMade::AsksObjectAlone)
            return { };
        Draft test(Opcode::IsPristine, origin);
        walker.chooseConservativeFields(test);
        test.setValueOperand(walker.constant(Draft(Opcode::Undefined, origin)));
        test.setBuiltinClass(ofPrimitives);
        test.setAspects({ PristineAspects::Names });
        return walker.build(WTF::move(test));
    }
    return buildTestOfInstanceAsMadeInWalkBuiltins(walker, object, classWhoseInstancesAreTestedInWalkBuiltins(walker, walker.typeOf(object)), origin, asked == TestOfObjectAsMade::AsksObjectAlone ? TestOfInstanceInWalkBuiltins::AsksStorageTooOfCellAlone : TestOfInstanceInWalkBuiltins::AsksStorageToo);
}

static bool readsOnlyInstanceNames(JSType type)
{
    switch (type) {
    case ArrayBufferType:
    case DataViewType:
    case JSDateType:
    case JSPromiseType:
    case RegExpObjectType:
        return true;
    default:
        return false;
    }
}

static Draft testOfInstanceAsMadeInWalkBuiltins(Walker& walker, Reference object, BuiltinClassID builtinClass, Origin origin, TestOfInstanceInWalkBuiltins asked)
{
    Draft test(Opcode::IsPristine, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(object);
    test.setBuiltinClass(builtinClass);
    bool isViewThatNobodyGrows = isTypedView(walker.program().builtins().typedArrayTypeOf(builtinClass)) && !someThreadMayGrowSharedMemory(walker);
    PristineTest pristineTest = pristineTestOf(walker.program().builtins(), builtinClass);
    if (pristineTest == PristineTest::InstanceStructure || pristineTest == PristineTest::ClosureAsCreated || (asked == TestOfInstanceInWalkBuiltins::AsksOneBitOfCell && builtinClass != BuiltinClasses::Array) || isViewThatNobodyGrows || readsOnlyInstanceNames(walker.program().builtins().at(builtinClass).type))
        test.setAspects({ PristineAspects::Names });
    else
        test.setAspects({ PristineAspects::Storage, PristineAspects::Names });
    if (asked != TestOfInstanceInWalkBuiltins::AsksStorageToo)
        test.setAspects(test.aspects() | PristineAspects::CellAlone);
    return test;
}

static Reference buildTestOfInstanceAsMadeInWalkBuiltins(Walker& walker, Reference object, BuiltinClassID builtinClass, Origin origin, TestOfInstanceInWalkBuiltins asked)
{
    return builtinClass ? walker.build(testOfInstanceAsMadeInWalkBuiltins(walker, object, builtinClass, origin, asked)) : Reference();
}

void Walker::verifyPristineObject(Reference object)
{
    if (!checksProofs() || isUnreachable() || hasFailed() || !object || !object.isValue() || !isKnownPristine(*this, object) || isMadeByCallThatListsEntriesFlat(*this, object))
        return;
    BuiltinClassID builtinClass = classThatHasTestOfObjectAsMadeInWalkBuiltins(*this, typeOf(object));
    if (!builtinClass)
        return;
    Origin origin = m_origin;
    origin.isTailCall = false;
    verifyRememberedTest(testOfInstanceAsMadeInWalkBuiltins(*this, object, builtinClass, origin, TestOfInstanceInWalkBuiltins::AsksOneBitOfCell));
}

bool linksWordCanBeHeldAgainst(const Walker& walker, Reference object)
{
    return object && object.isValue() && classThatHasTestOfObjectAsMadeInWalkBuiltins(walker, walker.typeOf(object));
}

void holdLinksWordAgainstObjectAsMade(Walker& walker, Reference object, Origin origin)
{
    if (walker.isUnreachable() || walker.hasFailed())
        return;
    if (!linksWordCanBeHeldAgainst(walker, object)) {
        walker.fail("a word that an object stays as made is taken of what no test holds it against"_s);
        return;
    }
    if (isKnownPristine(walker, object)) {
        walker.verifyPristineObject(object);
        return;
    }
    origin.isTailCall = false;
    Reference test = buildTestOfInstanceAsMadeInWalkBuiltins(walker, object, classThatHasTestOfObjectAsMadeInWalkBuiltins(walker, walker.typeOf(object)), origin, TestOfInstanceInWalkBuiltins::AsksOneBitOfCell);
    if (!test)
        return;
    walker.buildCheckThatTestHolds(test, ProofSource::OtherOperation, origin);
    markObjectPristine(walker, object);
}

TypeID typeFoundOnObjectAsMade(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    BuiltinID member = findMemberReadOffInWalkBuiltins(walker, typeOfObject, read.key(), true, MembersBehindWatch::AreAsBuilt);
    if (!member)
        return { };
    const BuiltinTable& table = walker.program().builtins();
    if (isObjectKind(table.at(table.canonicalID(member)).kind))
        return typeOfBuiltinObject(walker.typing(), member);
    BuiltinOperandTypes passed;
    passed.receiver = walker.types().withoutNullish(typeOfObject);
    return resultTypeOfReachableOverloads({ walker.program(), walker.types() }, table.canonicalID(member), BuiltinCallMode::Get, passed);
}

TypeID typeFoundAbovePrimitiveBehindWatch(Walker& walker, const BodyValue& read, TypeID typeOfPrimitive)
{
    if (!classOfPrimitivesWatchedInWalkBuiltins(walker, typeOfPrimitive) || findConstantMemberReadOff(std::as_const(walker), typeOfPrimitive, read.key()))
        return { };
    return typeFoundOnObjectAsMade(walker, read, typeOfPrimitive);
}

bool readOffObjectAsMadeCallsGetter(Walker& walker, const BodyValue& read, TypeID typeOfObject)
{
    BuiltinID member = findMemberReadOffInWalkBuiltins(walker, typeOfObject, read.key(), true, MembersBehindWatch::AreAsBuilt);
    const BuiltinTable& table = walker.program().builtins();
    return member && table.at(table.canonicalID(member)).kind == BuiltinKind::Accessor;
}

static bool firstReadIsSeenInWalkBuiltins(const Walker& walker, BuiltinID location, PropertyKeyID key)
{
    const BuiltinTable& table = walker.program().builtins();
    if (isMadeWithCodeOfLibrary(table, location))
        return true;
    const Builtin& entry = table.at(location);
    if (!entry.isAddedAtFirstRead || !entry.holder || walker.types().at(key).kind == PropertyKeyKind::EnginePrivateName)
        return false;
    return walker.facts().listsKeysOf(table.canonicalID(entry.holder));
}

static void buildFirstReadInWalkBuiltins(Walker& walker, BuiltinID location, Origin origin)
{
    if (walker.has(Mechanism::Folding) && walker.state().hasReadBuiltin(location))
        return;
    Draft initialization(opcodeOfFirstReadOffHolder(walker.program().builtins(), location), origin);
    walker.chooseConservativeFields(initialization);
    initialization.setBuiltin(location);
    walker.build(WTF::move(initialization));
    if (!walker.isUnreachable())
        walker.state().noteReadOfBuiltin(location);
}

bool firstReadOfNamedBuiltinIsSeen(const Walker& walker, const BodyValue& named)
{
    auto written = named.as<Opcode::Builtin>();
    if (!standsForFirstReadOffHolder(walker.program().builtins(), written.builtin(), written.isNotReadFromHolder()))
        return false;
    const BuiltinTable& table = walker.program().builtins();
    return isMadeWithCodeOfLibrary(table, written.builtin()) || walker.facts().listsKeysOf(table.canonicalID(table.at(written.builtin()).holder));
}

void buildFirstReadOfNamedBuiltin(Walker& walker, const BodyValue& named)
{
    if (firstReadOfNamedBuiltinIsSeen(walker, named))
        buildFirstReadInWalkBuiltins(walker, named.as<Opcode::Builtin>().builtin(), walker.originOf(named));
}

static bool buildReadByRowOfMemberInWalkBuiltins(Walker&, const BodyValue& read, TypeID typeOfObject, Reference object, BuiltinID location);
static bool buildReadOfWatchedValueInWalkBuiltins(Walker&, const BodyValue& read, TypeID typeOfObject, Reference object);

static bool programNamesWhatGrowsSharedMemoryInWalkBuiltins(const Walker& walker)
{
    std::span<const BuiltinUseRecord> uses = walker.facts().builtinUses();
    return findFactRow(uses, BuiltinID(Builtins::SharedArrayBufferPrototypeGrow).index(), &BuiltinUseRecord::builtin) || findFactRow(uses, BuiltinID(Builtins::WebAssemblyMemoryPrototypeGrow).index(), &BuiltinUseRecord::builtin)
        || findFactRow(uses, BuiltinID(Builtins::WebAssemblyMemoryPrototypeToResizableBuffer).index(), &BuiltinUseRecord::builtin);
}

bool someThreadMayGrowSharedMemory(const Walker& walker)
{
    return walker.facts().unseenCodeCanRun() || programNamesWhatGrowsSharedMemoryInWalkBuiltins(walker);
}

bool viewMayFollowSharedMemoryThatGrows(const Walker& walker, Reference view)
{
    if (!someThreadMayGrowSharedMemory(walker))
        return false;
    if (!view || !view.isValue())
        return true;
    if (findKnown(walker, KnownNote::ViewOverMemoryThatNoOtherThreadGrows, view))
        return false;
    const Code& code = walker.code();
    const Operation& made = code.at(underlyingCellOf(code, view.id()));
    if (made.opcode != Opcode::CallBuiltin && made.opcode != Opcode::CallInline)
        return true;
    if (walker.program().builtins().at(made.overload()).mode != BuiltinCallMode::Construct || (made.opcode == Opcode::CallBuiltin && made.builtinCallForm() != BuiltinCallForm::Whole))
        return true;
    TypeID sharedMemory = walker.types().builtinClass(BuiltinClasses::SharedArrayBuffer);
    return std::ranges::any_of(made.argumentsOperands(), [&](ValueID argument) {
        return argument && walker.types().overlap(code.at(argument).type, sharedMemory);
    });
}

bool readsOffEntryParameterThatStaysAsMade(Walker& walker, const BodyValue& access)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (access.id().index() >= index.numberOfValues() || index.value(access.id()).words() != access.words())
        return false;
    ValueID input = access.objectOperand() ? index.stripCasts(access.objectOperand()) : ValueID();
    if (!input || !index.isParameter(input) || index.blockOf(input) != entryBlock)
        return false;
    std::span<const LinkedParameter> parameters = walker.facts().linked().parametersOf(walker.currentFunction());
    return input.index() < parameters.size() && parameters[input.index()].staysAsItsClassMakesIt;
}

static bool namesEntryParameterThatStaysAsMadeInWalkBuiltins(Walker& walker, const BodyValue& user, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (user.id().index() >= index.numberOfValues() || index.value(user.id()).words() != user.words())
        return false;
    ValueID input = operand ? index.stripCasts(operand) : ValueID();
    if (!input || !index.isParameter(input) || index.blockOf(input) != entryBlock)
        return false;
    std::span<const LinkedParameter> parameters = walker.facts().linked().parametersOf(walker.currentFunction());
    return input.index() < parameters.size() && parameters[input.index()].staysAsItsClassMakesIt;
}

static bool namesVariableThatStaysAsMadeInWalkBuiltins(Walker& walker, const BodyValue& user, ValueID operand)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (user.id().index() >= index.numberOfValues() || index.value(user.id()).words() != user.words())
        return false;
    ValueID input = operand ? index.stripCasts(operand) : ValueID();
    while (input && !index.isParameter(input) && index.opcodeOf(input) == Opcode::CheckInitialized)
        input = index.stripCasts(index.value(input).operand(0));
    if (!input || index.isParameter(input) || index.opcodeOf(input) != Opcode::GetModuleVariable)
        return false;
    return walker.facts().linked().variable(index.value(input).variable()).staysAsItsClassMakesIt;
}

static bool indexesAreAbsentAboveInstancesInWalkBuiltins(const Walker& walker, BuiltinClassID builtinClass)
{
    const Program& program = walker.program();
    const BuiltinTable& builtins = program.builtins();
    BuiltinID holder;
    for (BuiltinClassID current = builtinClass; current && !holder; current = builtins.at(current).parent)
        holder = builtins.at(current).prototype;
    if (!holder)
        return false;
    holder = builtins.canonicalID(holder);
    while (true) {
        if (!builtins.closesIndexKeys(holder) || !builtins.hasLockedPrototypeLink(holder))
            return false;
        const Builtin& object = builtins.at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = builtins.canonicalID(object.inheritsFrom);
            break;
        }
    }
}

static bool methodKeepsReceiverHereInWalkBuiltins(const BuiltinTable& table, BuiltinID method, bool& givesReceiverBack)
{
    for (BuiltinID kept : { BuiltinID(Builtins::MapPrototypeSet), BuiltinID(Builtins::SetPrototypeAdd) }) {
        if (table.contains(kept) && method == table.canonicalID(kept)) {
            givesReceiverBack = true;
            return true;
        }
    }
    for (BuiltinID kept : { BuiltinID(Builtins::ArrayPrototypePush), BuiltinID(Builtins::ArrayPrototypePop), BuiltinID(Builtins::ArrayPrototypeShift), BuiltinID(Builtins::ArrayPrototypeUnshift),
        BuiltinID(Builtins::ArrayPrototypeIndexOf), BuiltinID(Builtins::ArrayPrototypeLastIndexOf), BuiltinID(Builtins::ArrayPrototypeIncludes), BuiltinID(Builtins::ArrayPrototypeJoin),
        BuiltinID(Builtins::MapPrototypeGet), BuiltinID(Builtins::MapPrototypeHas), BuiltinID(Builtins::MapPrototypeDelete), BuiltinID(Builtins::MapPrototypeClear),
        BuiltinID(Builtins::SetPrototypeHas), BuiltinID(Builtins::SetPrototypeDelete), BuiltinID(Builtins::SetPrototypeClear) }) {
        if (table.contains(kept) && method == table.canonicalID(kept))
            return true;
    }
    return false;
}

static constexpr OpcodeSet questionsThatHandObjectToNobodyInWalkBuiltins { Opcode::StrictEq, Opcode::ToBoolean, Opcode::TypeOf, Opcode::TypeTest };
static constexpr OpcodeSet makersOfReceiversInWalkBuiltins { Opcode::NewArray, Opcode::Data, Opcode::Construct };
static constexpr unsigned numberOfClassesOfReceiversMadeInBodies = 3;

static std::optional<unsigned> numberAmongClassesOfReceiversMadeInBodies(BuiltinClassID builtinClass)
{
    if (builtinClass == BuiltinClasses::Array)
        return 0;
    if (builtinClass == BuiltinClasses::Map)
        return 1;
    if (builtinClass == BuiltinClasses::Set)
        return 2;
    return std::nullopt;
}

static uint32_t placeBehindMakerInWalkBuiltins(const BodyIndex& index, BlockID maker, BlockID block)
{
    BlockID place = block;
    for (LoopID loop = index.loopOf(block); loop && !index.isInLoop(maker, loop); loop = index.loop(loop).parent)
        place = index.loop(loop).head;
    return place.index();
}

static Vector<Walker::ReceiverMadeInBody, 4> lookForReceiversThatLeaveByReturnAloneInWalkBuiltins(Walker& walker, const BodyIndex& index)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    TypeArena& types = walker.types();
    std::array<TypeID, numberOfClassesOfReceiversMadeInBodies> instances { types.builtinClass(BuiltinClasses::Array), types.builtinClass(BuiltinClasses::Map), types.builtinClass(BuiltinClasses::Set) };

    Vector<Walker::ReceiverMadeInBody, 4> receivers;
    Vector<uint32_t> receiverNamedBy;
    receiverNamedBy.fill(0, index.numberOfValues());
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            if (!makersOfReceiversInWalkBuiltins.contains(operation.opcode()))
                continue;
            receivers.append({ operation.id(), (1u << numberOfClassesOfReceiversMadeInBodies) - 1, UINT32_MAX });
            receiverNamedBy[operation.id().index()] = receivers.size();
        }
    }
    if (receivers.isEmpty())
        return receivers;

    auto receiverOf = [&](ValueID value) {
        return value ? receiverNamedBy[value.index()] : 0;
    };
    auto lose = [&](uint32_t receiver) {
        if (receiver)
            receivers[receiver - 1].classesItStaysAsMadeOf = 0;
    };
    auto memberReadBy = [&](const BodyValue& read, unsigned classNumber) {
        return read.isFreeName() || liesInThread(read.key()) ? BuiltinID() : findMemberReadOffInWalkBuiltins(walker, instances[classNumber], read.key(), true);
    };
    Vector<bool> wasFoundWithoutName;
    wasFoundWithoutName.fill(false, index.numberOfValues());
    auto readOfMethodCalledOn = [&](const BodyValue& call, uint32_t receiver) {
        ValueID read = index.stripCasts(call.calleeOperand());
        if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet)
            return ValueID();
        ValueID object = index.value(read).objectOperand();
        if (object && !receiverOf(object))
            wasFoundWithoutName[object.index()] = true;
        return receiverOf(object) == receiver ? read : ValueID();
    };
    auto mayGiveReceiverBack = [&](const BodyValue& call, uint32_t receiver) {
        ValueID read = readOfMethodCalledOn(call, receiver);
        if (!read)
            return false;
        for (unsigned classNumber = 0; classNumber < numberOfClassesOfReceiversMadeInBodies; ++classNumber) {
            BuiltinID member = memberReadBy(index.value(read), classNumber);
            bool givesReceiverBack = false;
            if (member && methodKeepsReceiverHereInWalkBuiltins(table, table.canonicalID(member), givesReceiverBack) && givesReceiverBack)
                return true;
        }
        return false;
    };
    auto forEachValueThatReceiverIsHandedOnAs = [&](const Invocable<void(ValueID, uint32_t)> auto& functor) {
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            BlockID id(number);
            BodyBlock block = index.block(id);
            for (BodyValue operation : block.values()) {
                Opcode opcode = operation.opcode();
                operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    uint32_t receiver = receiverOf(operand);
                    if (!receiver)
                        return;
                    if (opcodesThatStateTypes.contains(opcode) || (opcode == Opcode::Call && operandIndex == OperandIndex::Call::thisValue && !operation.isDoneInPlace() && mayGiveReceiverBack(operation, receiver)))
                        functor(operation.id(), receiver);
                });
            }
            block.forEachOutgoingEdge([&](BodyTarget target, bool) {
                BodyBlock entered = index.block(target.block());
                std::span<const ValueID> arguments = target.arguments();
                for (uint32_t position = 0; position < arguments.size(); ++position) {
                    if (uint32_t receiver = receiverOf(arguments[position]))
                        functor(entered.parameter(position), receiver);
                }
            });
            index.forEachEdgeResult(id, [&](const BodyEdgeResult& result) {
                if (uint32_t receiver = receiverOf(result.subject); receiver && result.parameter)
                    functor(result.parameter, receiver);
            });
        }
    };
    auto weighUseByOperation = [&](const BodyValue& operation, BlockID block, uint32_t operandIndex, uint32_t receiver) {
        Opcode opcode = operation.opcode();
        if (opcodesThatStateTypes.contains(opcode))
            return;
        uint8_t& classes = receivers[receiver - 1].classesItStaysAsMadeOf;
        bool isReadOffReceiver = opcode == Opcode::PropGet && operandIndex == OperandIndex::PropGet::object;
        bool isCallOnReceiver = opcode == Opcode::Call && operandIndex == OperandIndex::Call::thisValue && !operation.isDoneInPlace();
        if (!isReadOffReceiver && !isCallOnReceiver) {
            if (questionsThatHandObjectToNobodyInWalkBuiltins.contains(opcode))
                return;
            if (index.hasIrreducibleFlow()) {
                classes = 0;
                return;
            }
            Walker::ReceiverMadeInBody& handedOn = receivers[receiver - 1];
            handedOn.firstPlaceWhereItIsHandedOn = std::min(handedOn.firstPlaceWhereItIsHandedOn, placeBehindMakerInWalkBuiltins(index, index.blockOf(handedOn.made), block));
            return;
        }
        ValueID read = isReadOffReceiver ? operation.id() : readOfMethodCalledOn(operation, receiver);
        for (unsigned classNumber = 0; classNumber < numberOfClassesOfReceiversMadeInBodies; ++classNumber) {
            if (!(classes & 1u << classNumber))
                continue;
            BuiltinID member = read ? memberReadBy(index.value(read), classNumber) : BuiltinID();
            bool givesReceiverBack = false;
            bool keepsReceiverHere = isReadOffReceiver
                ? member || (!classNumber && operation.key() == program.keyOf(BuiltinNames::length))
                : member && methodKeepsReceiverHereInWalkBuiltins(table, table.canonicalID(member), givesReceiverBack);
            if (!keepsReceiverHere)
                classes &= ~(1u << classNumber);
        }
    };

    Vector<bool> wasNamedWhileReading;
    wasNamedWhileReading.fill(false, index.numberOfValues());
    uint64_t usesOfNamesStillToMeet = 0;
    bool someNameMayHaveComeBehindUse = false;
    auto meetUse = [&](ValueID used) {
        if (wasNamedWhileReading[used.index()])
            --usesOfNamesStillToMeet;
    };
    auto handOn = [&](ValueID name, uint32_t receiver) {
        uint32_t& named = receiverNamedBy[name.index()];
        if (named) {
            if (named != receiver) {
                lose(named);
                lose(receiver);
            }
            return;
        }
        named = receiver;
        wasNamedWhileReading[name.index()] = true;
        usesOfNamesStillToMeet += index.useCount(name);
        someNameMayHaveComeBehindUse |= index.useCount(name) == BodyIndex::maxCountedUses || wasFoundWithoutName[name.index()];
    };
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID id(number);
        BodyBlock block = index.block(id);
        if (receiverOf(block.subject()))
            meetUse(block.subject());
        for (BodyValue operation : block.values()) {
            Opcode opcode = operation.opcode();
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                uint32_t receiver = receiverOf(operand);
                if (!receiver)
                    return;
                meetUse(operand);
                if (opcodesThatStateTypes.contains(opcode) || (opcode == Opcode::Call && operandIndex == OperandIndex::Call::thisValue && !operation.isDoneInPlace() && mayGiveReceiverBack(operation, receiver)))
                    handOn(operation.id(), receiver);
                weighUseByOperation(operation, id, operandIndex, receiver);
            });
        }
        block.forEachOutgoingEdge([&](BodyTarget target, bool) {
            BodyBlock entered = index.block(target.block());
            std::span<const ValueID> arguments = target.arguments();
            for (uint32_t position = 0; position < arguments.size(); ++position) {
                if (uint32_t receiver = receiverOf(arguments[position])) {
                    meetUse(arguments[position]);
                    handOn(entered.parameter(position), receiver);
                }
            }
        });
        index.forEachEdgeResult(id, [&](const BodyEdgeResult& result) {
            uint32_t receiver = receiverOf(result.subject);
            if (!receiver)
                return;
            if (result.proof != Proof::None)
                meetUse(result.subject);
            if (result.parameter)
                handOn(result.parameter, receiver);
        });
        BodyTerminator terminator = block.terminator();
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            uint32_t receiver = receiverOf(operand);
            if (!receiver)
                return;
            meetUse(operand);
            if (terminator.kind() != TerminatorKind::Return)
                lose(receiver);
        });
    }
    if (!someNameMayHaveComeBehindUse && !usesOfNamesStillToMeet)
        return receivers;

    for (Walker::ReceiverMadeInBody& receiver : receivers) {
        receiver.classesItStaysAsMadeOf = (1u << numberOfClassesOfReceiversMadeInBodies) - 1;
        receiver.firstPlaceWhereItIsHandedOn = UINT32_MAX;
    }
    forEachValueThatReceiverIsHandedOnAs([&](ValueID name, uint32_t receiver) {
        if (receiverOf(name) == receiver)
            return;
        lose(receiverOf(name));
        lose(receiver);
    });
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BodyBlock block = index.block(BlockID(number));
        for (BodyValue operation : block.values()) {
            operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (uint32_t receiver = receiverOf(operand))
                    weighUseByOperation(operation, BlockID(number), operandIndex, receiver);
            });
        }
        BodyTerminator terminator = block.terminator();
        terminator.forEachOperand([&](ValueID operand, uint32_t) {
            if (terminator.kind() != TerminatorKind::Return)
                lose(receiverOf(operand));
        });
    }
    return receivers;
}

static bool isMadeInFrontOfLoopThatHoldsOperationInWalkBuiltins(const BodyIndex& index, const BodyValue& operation)
{
    ValueID made = index.stripCasts(operation.opcode() == Opcode::Call ? operation.as<Opcode::Call>().thisValueOperand() : operation.objectOperand());
    if (!made || index.isParameter(made) || !makersOfReceiversInWalkBuiltins.contains(index.opcodeOf(made)))
        return false;
    LoopID loop = index.loopOf(index.blockOf(operation.id()));
    return loop && !index.isInLoop(index.blockOf(made), loop);
}

static bool receiverIsMadeHereAndLeavesByReturnAloneInWalkBuiltins(Walker& walker, const BodyValue& operation, Reference receiver)
{
    if (!walker.has(Mechanism::EscapeAnalysis) || !receiver.isValue())
        return false;
    const Code& code = walker.code();
    ValueID cell = underlyingCellOf(code, receiver.id());
    if (code.isConstant(cell))
        return false;
    const Operation& built = code.at(cell);
    bool isMadeAsItsClassMakesIt = built.opcode == Opcode::NewArray || built.opcode == Opcode::NewMapFromData || built.opcode == Opcode::NewSetFromData
        || (built.opcode == Opcode::CallInline && (built.inlineBuiltin() == InlineBuiltin::NewMap || built.inlineBuiltin() == InlineBuiltin::NewSet));
    if (!isMadeAsItsClassMakesIt || built.origin().inlineFrame != walker.originOf(operation).inlineFrame)
        return false;
    const ClassPart* onlyClass = singleBuiltinClassOf(walker.types(), walker.types().at(walker.typeOf(receiver)));
    std::optional<unsigned> classNumber = onlyClass ? numberAmongClassesOfReceiversMadeInBodies(onlyClass->builtinClass) : std::nullopt;
    if (!classNumber)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID made = index.stripCasts(operation.opcode() == Opcode::Call ? operation.as<Opcode::Call>().thisValueOperand() : operation.objectOperand());
    Reference boundToMade = walker.referenceOf(made);
    if (!boundToMade || !boundToMade.isValue() || underlyingCellOf(code, boundToMade.id()) != cell)
        return false;
    auto looked = walker.receiversMadeInBodies.ensure(walker.currentFunction().index(), [&] {
        return lookForReceiversThatLeaveByReturnAloneInWalkBuiltins(walker, index);
    });
    const auto& receivers = looked.iterator->value;
    auto found = std::ranges::lower_bound(receivers, made.index(), { }, [](const Walker::ReceiverMadeInBody& known) {
        return known.made.index();
    });
    return found != receivers.end() && found->made == made && (found->classesItStaysAsMadeOf & 1u << *classNumber)
        && placeBehindMakerInWalkBuiltins(index, index.blockOf(made), index.blockOf(operation.id())) < found->firstPlaceWhereItIsHandedOn;
}

static bool receiverStaysAsMadeByLinkInWalkBuiltins(Walker& walker, const BodyValue& operation, WordOfBodyOnAsMade wordOfBody, Reference knownToTakeLinksWord)
{
    const LinkTables& linked = walker.facts().linked();
    if (!walker.has(Mechanism::TypedOperations))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    bool linkHasWord = !linked.receiversThatStayAsMade.isEmpty();
    bool bodyMayMakeReceiver = index.hasLoops() && index.holdsAnyOf(makersOfReceiversInWalkBuiltins);
    if (!linkHasWord && !bodyMayMakeReceiver)
        return false;
    if (operation.id().index() >= index.numberOfValues() || index.value(operation.id()).words() != operation.words())
        return false;
    Reference receiver = walker.referenceOf(operation.opcode() == Opcode::Call ? operation.as<Opcode::Call>().thisValueOperand() : operation.objectOperand());
    TypeID proven = receiver ? walker.typeOf(receiver) : TypeID();
    if (!proven || !walker.types().at(proven).isObject() || !singleBuiltinClassOf(walker.types(), walker.types().at(proven)))
        return false;
    if ((!knownToTakeLinksWord || receiver != knownToTakeLinksWord) && !linksWordCanBeHeldAgainst(walker, receiver))
        return false;
    FunctionID function = walker.currentFunction();
    if (linked.receiverStaysAsMadeAt(function, operation.id()))
        return true;
    if (wordOfBody == WordOfBodyOnAsMade::IsTaken && !walker.program().admitsReplacedBuiltins && bodyMayMakeReceiver && linksWordCanBeHeldAgainst(walker, receiver) && isMadeInFrontOfLoopThatHoldsOperationInWalkBuiltins(index, operation) && receiverIsMadeHereAndLeavesByReturnAloneInWalkBuiltins(walker, operation, receiver))
        return true;
    if (operation.opcode() != Opcode::PropGet)
        return false;
    ValueID user = index.onlyUsingValue(operation.id());
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    if (!user || index.opcodeOf(user) != Opcode::Call)
        return false;
    auto call = index.value(user).as<Opcode::Call>();
    return call.calleeKey() == operation.key() && call.thisValueOperand() && index.stripCasts(call.calleeOperand()) == operation.id()
        && index.stripCasts(call.thisValueOperand()) == index.stripCasts(operation.objectOperand()) && linked.receiverStaysAsMadeAt(function, user);
}

bool receiverStaysAsMadeByLink(Walker& walker, const BodyValue& operation, WordOfBodyOnAsMade wordOfBody)
{
    return receiverStaysAsMadeByLinkInWalkBuiltins(walker, operation, wordOfBody, { });
}

BuiltinClassID classOfIterableThatStaysAsMadeByLink(Walker& walker, const BodyValue& operation, ValueID iterable)
{
    const LinkTables& linked = walker.facts().linked();
    if (!walker.has(Mechanism::TypedOperations) || !iterable)
        return { };
    const BodyIndex& index = walker.currentBodyIndex();
    if (operation.id().index() >= index.numberOfValues() || index.value(operation.id()).words() != operation.words())
        return { };
    Reference iterated = walker.referenceOf(iterable);
    TypeID proven = iterated && iterated.isValue() ? walker.types().withoutNullish(walker.typeOf(iterated)) : TypeID();
    const ClassPart* onlyClass = proven && !isNeverType(proven) && walker.types().at(proven).isObject() ? singleBuiltinClassOf(walker.types(), walker.types().at(proven)) : nullptr;
    if (!onlyClass)
        return { };
    if (walker.program().builtins().typedArrayTypeOf(onlyClass->builtinClass) != NotTypedArray)
        return { };
    if (!linked.iterableStaysAsMadeAt(walker.currentFunction(), operation.id()) && !namesEntryParameterThatStaysAsMadeInWalkBuiltins(walker, operation, iterable))
        return { };
    return onlyClass->builtinClass;
}

bool isArrayPrototypeChainSane(const Walker& walker)
{
    return indexesAreAbsentAboveInstancesInWalkBuiltins(walker, BuiltinClasses::Array);
}

bool readsPlainListByLink(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (walker.isInLoop() || !walker.has(Mechanism::TypedOperations) || read.opcode() != Opcode::ElemGet || read.id().index() >= index.numberOfValues() || index.value(read.id()).words() != read.words())
        return false;
    Reference list = walker.referenceOf(read.objectOperand());
    if (!list || !list.isValue() || !linksWordCanBeHeldAgainst(walker, list))
        return false;
    return walker.facts().linked().readsPlainListAt(walker.currentFunction(), read.id()) && indexesAreAbsentAboveInstancesInWalkBuiltins(walker, BuiltinClasses::Array);
}

bool readsOffListThatStaysAsMadeByLink(Walker& walker, const BodyValue& read)
{
    return receiverStaysAsMadeByLink(walker, read) && indexesAreAbsentAboveInstancesInWalkBuiltins(walker, BuiltinClasses::Array);
}

static TypedForm buildReadAtLocationInWalkBuiltins(Walker& walker, const BodyValue& read, TypeID typeOfObject, Reference object, BuiltinID location, TypedForm formAskedAbout)
{
    PropertyKeyID key = read.key();
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID member = table.canonicalID(location);
    if (table.at(member).kind == BuiltinKind::Accessor || table.at(member).kind == BuiltinKind::DataProperty)
        return buildReadByRowOfMemberInWalkBuiltins(walker, read, typeOfObject, object, location) ? formAskedAbout : TypedForm::None;
    if (!isObjectKind(table.at(member).kind) || !BuiltinCellKey::object(member).hasCell(table))
        return TypedForm::None;
    if (!object) {
        if (firstReadIsSeenInWalkBuiltins(walker, location, key))
            walker.noteFormAskedAhead(opcodeOfFirstReadOffHolder(table, location), KnowledgeLevel::Proven);
        return formAskedAbout;
    }

    Origin origin = walker.originOf(read);
    if (firstReadIsSeenInWalkBuiltins(walker, location, key))
        buildFirstReadInWalkBuiltins(walker, location, origin);
    Draft found(Opcode::Builtin, origin);
    walker.chooseConservativeFields(found);
    found.setBuiltin(member);
    Reference cell = walker.has(Mechanism::Folding) ? walker.cellThatRunTimeKeeps(found) : Reference();
    walker.bind(read.id(), cell ? cell : walker.build(WTF::move(found)));
    return TypedForm::Built;
}

TypedForm buildReadOfBuiltinMember(Walker& walker, const BodyValue& read, TypeID typeOfObject, Reference object)
{
    if (!typeOfObject || walker.types().at(typeOfObject).primitives & Primitive::UndefinedOrNull)
        return TypedForm::None;
    PropertyKeyID key = read.key();
    BuiltinID location = object ? findConstantMemberReadOff(walker, object, key) : findConstantMemberReadOff(std::as_const(walker), typeOfObject, key);
    TypedForm formAskedAbout = TypedForm::Built;
    bool wordCanBeHeld = !location && classThatHasTestOfObjectAsMadeInWalkBuiltins(walker, typeOfObject);
    if (wordCanBeHeld && (readsOffEntryParameterThatStaysAsMade(walker, read) || receiverStaysAsMadeByLink(walker, read, WordOfBodyOnAsMade::IsTaken))) {
        location = findMemberReadOffInWalkBuiltins(walker, typeOfObject, key, true);
        if (location && object)
            holdLinksWordAgainstObjectAsMade(walker, object, walker.originOf(read));
    }
    BuiltinClassID tested = !location && !object ? classWhoseInstancesAreTestedInWalkBuiltins(walker, typeOfObject) : BuiltinClassID();
    if (tested == BuiltinClasses::Function && !walker.isInLoop())
        tested = { };
    if (!location && !object && (tested || classOfPrimitivesWatchedInWalkBuiltins(walker, typeOfObject))) {
        location = findMemberReadOffInWalkBuiltins(walker, typeOfObject, key, true, MembersBehindWatch::AreAsBuilt);
        formAskedAbout = TypedForm::BuiltOnObjectAsMade;
    }
    if (!location && !object) {
        location = findMemberThatGuessAsksByItsByte(walker, read, typeOfObject);
        if (location)
            formAskedAbout = TypedForm::BuiltOnObjectAsMade;
    }
    if (!location)
        return buildReadOfWatchedValueInWalkBuiltins(walker, read, typeOfObject, object) ? TypedForm::Built : TypedForm::None;
    return buildReadAtLocationInWalkBuiltins(walker, read, typeOfObject, object, location, formAskedAbout);
}

TypeID typeOfFunctionReadOffFunctionPrototype(Walker& walker, PropertyKeyID key)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID location = findConstantMemberOnceForBuild(walker, Builtins::FunctionPrototype, key);
    return location && isObjectKind(table.at(table.canonicalID(location)).kind) ? typeOfBuiltinObject(walker.typing(), location) : TypeID();
}

TypedForm buildReadOffFunctionPrototype(Walker& walker, const BodyValue& read, TypeID typeOfObject, Reference object)
{
    if (!typeOfFunctionReadOffFunctionPrototype(walker, read.key()))
        return TypedForm::None;
    return buildReadAtLocationInWalkBuiltins(walker, read, typeOfObject, object, findConstantMemberOnceForBuild(walker, Builtins::FunctionPrototype, read.key()), TypedForm::Built);
}

TypedForm buildObjectPrototypeMemberRead(Walker& walker, const BodyValue& read, TypeID typeOfObject, Reference object, BuiltinID location)
{
    return buildReadAtLocationInWalkBuiltins(walker, read, typeOfObject, object, location, TypedForm::Built);
}

BuiltinID findMemberAboveHeirsOfBuiltinClass(const Walker& walker, BuiltinClassID builtinClass, PropertyKeyID key)
{
    TypeID instances = walker.types().builtinClass(builtinClass);
    BuiltinID location = findMemberOnObjectAsMade(walker, instances, key);
    if (!location || beliefInMemberFoundOnObjectAsMade(walker, location, instances, key) != BeliefInMemberAbove::IsConstant)
        return { };
    const BuiltinTable& table = walker.program().builtins();
    BuiltinTypeContext context { walker.program(), walker.types() };
    bool someRowAsksMoreThanClass = std::ranges::any_of(table.overloadsOf(table.canonicalID(location)), [&](const BuiltinOverload& row) {
        if (!row.receiver || !row.receiver->accepted)
            return true;
        OperandFit fit = fitOf(context, instances, row.receiver->accepted);
        return fit.fit != TypeFit::Outside && (fit.fit != TypeFit::Inside || fit.unprovenPristine);
    });
    return someRowAsksMoreThanClass ? BuiltinID() : location;
}

TypedForm buildReadOfMemberAboveHeirOfBuiltinClass(Walker& walker, const BodyValue& read, TypeID typeOfObject, Reference object, BuiltinID location)
{
    return buildReadAtLocationInWalkBuiltins(walker, read, typeOfObject, object, location, object ? TypedForm::Built : TypedForm::BuiltBehindLookAtCell);
}

static_assert(opcodesThatBuildInstanceOfBuiltinTakes == (OpcodeSet { Opcode::InstanceOf }));

bool buildInstanceOfBuiltinOfItsOpcodes(Walker& walker, const BodyValue& test, const InputsOfTypedOperation& inputs)
{
    if (test.opcode() != Opcode::InstanceOf)
        return false;
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    TypeID typeOfConstructor = inputs.typeOf(test.constructorOperand());
    if (!typeOfConstructor || !inputs.typeOf(test.valueOperand()))
        return false;
    Type written = walker.types().at(typeOfConstructor);
    BuiltinID constructor = written.isObject() ? singleBuiltinOf(walker.types(), written) : BuiltinID();
    if (!constructor || table.at(constructor).kind != BuiltinKind::Constructor)
        return false;

    if (table.canonicalID(constructor) == table.canonicalID(Builtins::File))
        return false;
    BuiltinID asked = findConstantMemberFrom(program, walker.facts(), constructor, program.keyOf(table.nameOf(WellKnownSymbol::HasInstance)));
    if (!asked || table.canonicalID(asked) != Builtins::FunctionPrototypeSymbolHasInstance)
        return false;
    BuiltinID location = findConstantMemberFrom(program, walker.facts(), constructor, program.keyOf(BuiltinNames::prototype));
    BuiltinID prototype = location ? table.canonicalID(location) : BuiltinID();
    if (!prototype || !isObjectKind(table.at(prototype).kind) || !BuiltinCellKey::object(prototype).hasCell(table) || isMadeWithCodeOfLibrary(table, location))
        return false;
    if (inputs.areOnlyTypes())
        return true;

    Origin origin = walker.originOf(test);
    Draft found(Opcode::Builtin, origin);
    walker.chooseConservativeFields(found);
    found.setBuiltin(prototype);
    Reference above = walker.build(WTF::move(found));
    if (!above)
        return true;
    Draft chainWalk(Opcode::HasInPrototypeChain, origin);
    walker.chooseConservativeFields(chainWalk);
    chainWalk.setValueOperand(walker.referenceOf(test.valueOperand()));
    chainWalk.setPrototypeOperand(above);
    chainWalk.inputValueGiven = test.id();
    walker.bind(test.id(), walker.build(WTF::move(chainWalk)));
    return true;
}

bool cellAnswersWhetherValueIsArray(const Walker& walker, TypeID asked)
{
    const TypeArena& types = walker.types();
    if (!asked)
        return false;
    Type type = types.at(asked);
    if (!type.isJavaScriptValue() || type.hasUnboundedUnknown() || type.traitsOfNamed.containsAny({ ObjectTrait::IsBuiltin, ObjectTrait::IsOtherObject, ObjectTrait::MayHaveLayoutBelow }))
        return false;
    for (const ClassPart& part : classPartsOf(types, type)) {
        if (part.builtinClass == BuiltinClasses::Proxy)
            return false;
    }
    for (LayoutID layout : membersOf(types, type)) {
        if (builtinClassOfInstances(walker.program().at(layout)))
            return false;
        LayoutID highest = layout;
        while (LayoutID base = walker.program().at(highest).base)
            highest = base;
        if (walker.program().at(highest).has(LayoutFlags::HasHeritage))
            return false;
    }
    return true;
}

bool isLookAtCellInFrontOfIsArray(const Walker& walker, const BodyIndex& index, const BodyValue& test)
{
    if (test.opcode() != Opcode::TypeTest)
        return false;
    Type tested = walker.types().at(test.as<Opcode::TypeTest>().testedType());
    const ClassPart* testedClass = tested.isObject() ? singleBuiltinClassOf(walker.types(), tested) : nullptr;
    if (!testedClass || testedClass->builtinClass != BuiltinClasses::Array)
        return false;
    BodyTerminator end = index.terminator(index.blockOf(test.id()));
    if (end.kind() != TerminatorKind::Branch || end.operand(0) != test.id())
        return false;
    for (BodyValue operation : index.block(end.targets()[1].block()).values()) {
        Opcode opcode = operation.opcode();
        if (opcode == Opcode::NarrowByTest || opcode == Opcode::Builtin)
            continue;
        if (opcode != Opcode::Call || operation.argumentsOperands().size() != 1)
            return false;
        ValueID callee = operation.calleeOperand();
        if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin || index.value(callee).as<Opcode::Builtin>().builtin() != Builtins::ArrayIsArray)
            return false;
        return index.stripCasts(operation.argumentsOperands()[0]) == index.stripCasts(test.operand(0));
    }
    return false;
}

TypeID typeOfIsArrayAhead(Walker& walker, const BodyIndex& index, const BodyValue& call, const TypesOfInputs& typesOfInputs)
{
    if (!walker.has(Mechanism::BuiltinCalls) || !walker.has(Mechanism::Folding) || call.opcode() != Opcode::Call || call.isDoneInPlace() || call.argumentsOperands().size() != 1)
        return { };
    ValueID callee = call.calleeOperand();
    if (!callee || index.isParameter(callee) || index.opcodeOf(callee) != Opcode::Builtin || index.value(callee).as<Opcode::Builtin>().builtin() != Builtins::ArrayIsArray)
        return { };
    TypeID asked = typesOfInputs(call.argumentsOperands()[0]);
    if (!cellAnswersWhetherValueIsArray(walker, asked))
        return { };
    TypeArena& types = walker.types();
    TypeID lists = types.builtinClass(BuiltinClasses::Array);
    if (!types.overlap(asked, lists))
        return TypeArena::fromPrimitives(Primitive::False);
    return types.isSubset(asked, lists) ? TypeArena::fromPrimitives(Primitive::True) : TypeID();
}

namespace {

class EffectsSinkInWalkBuiltins : public EffectBoundBuilder {
public:
    constexpr void asOverload() { m_standsForOverload = true; }
    constexpr bool standsForOverload() const { return m_standsForOverload; }

private:
    bool m_standsForOverload { false };
};

class CallInWalkBuiltins {
public:
    CallInWalkBuiltins(Walker& walker, const BodyValue& call, const TypesOfInputs* typesAskedAbout = nullptr, BuiltinID locationFoundByRead = { })
        : m_walker(walker)
        , m_call(call)
        , m_program(walker.program())
        , m_table(walker.program().builtins())
        , m_types(walker.types())
        , m_context { walker.program(), walker.types() }
        , m_inputs(walker, typesAskedAbout)
        , m_location(locationFoundByRead)
    {
        if (locationFoundByRead)
            m_mode = call.opcode() == Opcode::PropSet ? BuiltinCallMode::Set : BuiltinCallMode::Get;
    }

    CallInWalkBuiltins(Walker& walker, const BodyValue& call, const Draft& forwarded)
        : CallInWalkBuiltins(walker, call)
    {
        m_forwarded = &forwarded;
    }

    bool plan();
    FunctionID findLibraryFunctionOfRowsReached(RowsThatNameLibraryFunction);
    TypeID findTypeOfResultOfRowsReached();
    BuiltinOverloadID findMatchingOverload();
    void holdLinksWordAgainstOperands();
    bool goesByOneRow() const { return m_form == BuiltinCallForm::Whole; }
    bool convertsArgumentThatComesRoundLoop() const;
    bool standsInLine() const { return m_inlineForm != InlineForm::Call || m_ownPropertyAskedFor; }
    const Effects& effects() const LIFETIME_BOUND { return m_effects; }
    Effects effectsOfAllThatIsBuilt() const
    {
        Effects all = m_effects;
        if (m_inlineForm == InlineForm::TwoWay)
            all.merge(m_effectsOfAttempt);
        return all;
    }
    void build();
    bool callsKnownBuiltin() const { return !!m_builtin; }
    StatisticID statisticOfRow() const;

private:
    enum class Outcome : uint8_t { Planned, NoMoreOperands, GaveUp };

    struct Operand {
        Reference written;
        Reference value;
        TypeID type;
        const BuiltinParameter* parameter { nullptr };
        BuiltinConversion conversion { BuiltinConversion::None };
        bool staysAsMade { false };
        LayoutID heirBehindLook { };
    };

    enum class LookAtReceiver : bool { IsLeftAside, IsCountedOn };

    static bool mayBeObject(const Type& type) { return type.hasObjects() || !type.isJavaScriptValue(); }
    static bool holdsValue(const Operand& operand) { return operand.type && !isNeverType(operand.type); }
    bool isDoneInPlace() const { return m_call.opcode() == Opcode::Call && m_call.isDoneInPlace(); }

    bool findCallee();
    bool mayBelieveRows() const;
    bool mayBeTrimmed() const;
    bool constructorFindsItsAdderUnchanged() const;

    Operand operandAsWritten(ValueID, LookAtReceiver = LookAtReceiver::IsCountedOn) const;
    bool describeOperands();
    bool rowsSpeakOfThis() const;
    BuiltinOverloadID generalFormOf(BuiltinOverloadID) const;
    BuiltinOperandTypes typesAndAspectsPassed() const;
    bool matchOverload();

    PristineAspectSet aspectsSeenOf(const Operand&) const;
    bool isProvenToLieIn(const Operand&, BuiltinTypeID) const;
    TypeID typeConvertedWithoutCode(TypeID, BuiltinConversion) const;
    bool conversionsMayRunCode() const;
    bool planConversion(Operand&) const;
    bool tryGuardStringOperand(Operand&, Origin);
    Outcome planOperand(Operand&, BuiltinOperand) const;
    bool planOperands();
    bool hasNativeEntry() const;
    bool runsAtMostOnce() const;
    bool chooseForm();
    void forgetWhatIsPlanned();
    bool rowsFindNothingAboveLiteral(std::span<const BuiltinOverloadID> rowsAsWritten, unsigned place, std::span<const BuiltinName> namesOfParts) const;
    bool planOnWidestRowOfOneNativeFunction();
    bool planOnRowsThatEnterOneDoor();
    bool planOnPartsOfLiteral();
    bool planOnAnyOperands();
    std::optional<Effects> effectsIfOnlyNullishRejected();
    bool asksForOwnPropertyUnderWrittenName();

    bool mayRunCodeThatNoOperandNames() const { return mayRunCodeThatNoOperandNames(*m_overload); }
    bool mayRunCodeThatNoOperandNames(const BuiltinOverload&) const;
    bool everyRowThatCallMayTake(const Invocable<bool(const BuiltinOverload&)> auto& holds) const;
    bool namesNoHeapForItsUses() const;
    bool takesObjectsByClassAlone(BuiltinTypeID accepted) const;
    bool mayRaiseErrorOn(const BuiltinParameter&, TypeID operandOrNotPassed, const Type&) const;
    bool callsBeforeReturn(const BuiltinParameter&) const;
    bool addEffectsOfFunctionsCalled(Effects&, const BuiltinParameter&, const Type&) const;
    void addEffectsOn(Effects&, const Operand&) const;
    Effects effectsOfRow(std::optional<InlineBuiltin> codeInLine) const;
    bool holdsOnlyCompiledRecords(TypeID operand) const;
    bool makesFreshResult(Reference result) const;
    Vector<Reference, 4> freshOperandsThatStayHere() const;
    Vector<Reference, 4> operandsThatStayAsMade() const;

    bool settle(Operand&, Origin);
    bool buildWithRegExpWrittenForCall(std::span<Reference> arguments);
    std::optional<HeldRegExp> findRegExpMatchedByCall() const;
    template<typename CallDraft> void fillCodeInLine(CallDraft&, std::span<const Reference> arguments) const;
    TypeID provenContentsOfReceiver() const;
    bool isOnlyCutToInt32(TypeID contents) const;
    Reference buildCall(std::span<const Reference> arguments);
    Reference buildCodeInLineOrCall(std::span<const Reference> arguments);
    Reference buildElementAt(Reference index, Origin);
    Reference buildElementInside(Reference position, Origin);
    Reference buildMembershipInListOfFile(std::span<const Reference> arguments, Origin);
    Reference buildComparisonByContents(std::span<const Reference> arguments);
    DataID findDataThatConstructionAloneSees(bool entriesArePairs) const;
    bool buildCollectionFromData();

    Walker& m_walker;
    const BodyValue& m_call;
    const Draft* m_forwarded { nullptr };
    const Program& m_program;
    const BuiltinTable& m_table;
    TypeArena& m_types;
    BuiltinTypeContext m_context;
    InputsOfTypedOperation m_inputs;

    Origin m_origin { };
    ExpectedType m_expected;
    bool m_resultIsReturnedAtOnce { false };
    BuiltinCallMode m_mode { BuiltinCallMode::Call };
    BuiltinID m_builtin;
    BuiltinID m_location;
    std::optional<Operand> m_thisValue;
    Vector<Operand, 6> m_passed;

    BuiltinOverloadID m_overloadID;
    const BuiltinOverload* m_overload { nullptr };
    std::optional<Operand> m_receiver;
    Vector<Operand, 6> m_arguments;
    PropertyKeyID m_ownPropertyAskedFor;
    LayoutSetID m_receiversAskedForOwnProperty;
    InlineForm m_inlineForm { InlineForm::Call };
    std::optional<InlineBuiltin> m_inlineBuiltin;
    BuiltinCallForm m_form { BuiltinCallForm::Whole };
    bool m_rowStandsForSeveral { false };
    Vector<BuiltinOverloadID, 4> m_rowsThatOneStandsFor;
    Effects m_effects { Effects::unknownCode() };
    Effects m_effectsOfAttempt { Effects::unknownCode() };
};

bool CallInWalkBuiltins::findCallee()
{
    if (m_mode == BuiltinCallMode::Get || m_mode == BuiltinCallMode::Set) {
        m_builtin = m_table.canonicalID(m_location);
        return true;
    }
    if (m_forwarded) {
        Reference callee = m_forwarded->calleeOperand();
        TypeID proven = callee ? m_walker.typeOf(callee) : TypeID();
        BuiltinID named = proven && m_types.at(proven).isObject() ? singleBuiltinOf(m_types, m_types.at(proven)) : BuiltinID();
        if (!named)
            return false;
        m_builtin = m_table.canonicalID(named);
        return std::ranges::none_of(m_table.overloadsOf(m_builtin), [](const BuiltinOverload& overload) {
            return std::ranges::any_of(overload.flow.callbacks, [](const BuiltinCallback& callback) {
                return callback.resultUse == BuiltinCallbackResultUse::Returned;
            });
        });
    }
    if (!m_inputs.areOnlyTypes() && !m_walker.referenceOf(m_call.calleeOperand()))
        return false;

    const BodyIndex& index = m_walker.currentBodyIndex();
    ValueID read = index.stripCasts(m_call.calleeOperand());
    bool isReadByName = read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName();

    TypeID proven = m_inputs.typeOf(m_call.calleeOperand());
    if (!proven)
        return false;
    Type type = m_types.at(proven);
    BuiltinID named = type.isObject() ? singleBuiltinOf(m_types, type) : BuiltinID();
    if (!named && isReadByName) {
        ValueID objectOfRead = index.value(read).objectOperand();
        PropertyKeyID key = index.value(read).key();
        m_location = m_inputs.areOnlyTypes() ? findConstantMemberReadOff(std::as_const(m_walker), m_inputs.typeOf(objectOfRead), key) : findConstantMemberReadOff(m_walker, m_walker.referenceOf(objectOfRead), key);
        named = m_location;
    }
    if (!named)
        return false;
    m_builtin = m_table.canonicalID(named);
    if (m_builtin == m_table.canonicalID(Builtins::ObjectEntries) && m_mode == BuiltinCallMode::Call && !m_inputs.areOnlyTypes() && m_table.contains(Builtins::CompilerHelpersOwnEntriesFlat) && isListOfEntriesTakenApartAtOnce(m_walker, m_call))
        m_builtin = m_table.canonicalID(Builtins::CompilerHelpersOwnEntriesFlat);
    return true;
}

bool CallInWalkBuiltins::mayBeTrimmed() const
{
    if (!hasFeatureGuard(m_table, m_builtin) && !(m_location && hasFeatureGuard(m_table, m_location)))
        return false;
    auto isListed = [&](BuiltinID id) {
        return id && m_walker.facts().isUsedWithoutNamesFromRunTime(id);
    };
    return !isListed(m_builtin) && !isListed(m_location);
}

bool CallInWalkBuiltins::constructorFindsItsAdderUnchanged() const
{
    const Builtin& entry = m_table.at(m_builtin);
    if (entry.kind != BuiltinKind::Constructor || !entry.instanceClass)
        return true;
    BuiltinName adder = m_table.at(entry.instanceClass).constructorAdder;
    if (!adder)
        return true;
    BuiltinMemberLookup found = m_table.findForInstance(entry.instanceClass, adder);
    return found.outcome == BuiltinMemberLookup::Outcome::Found && isConstantLocationInWalkBuiltins(m_program, m_walker.facts(), found.member);
}

bool CallInWalkBuiltins::mayBelieveRows() const
{
    if (m_mode == BuiltinCallMode::Get || m_mode == BuiltinCallMode::Set)
        return m_table.dispatchOf(m_builtin, m_mode) && !mayBeTrimmed();
    if (!isCallableBuiltin(m_table, m_builtin) || requiresWalkLowering(m_table, m_builtin))
        return false;
    return !mayBeTrimmed() && constructorFindsItsAdderUnchanged();
}

CallInWalkBuiltins::Operand CallInWalkBuiltins::operandAsWritten(ValueID input, LookAtReceiver look) const
{
    Reference written = m_inputs.areOnlyTypes() ? Reference() : m_walker.referenceOf(input);
    Operand operand { written, written, m_inputs.areOnlyTypes() ? m_inputs.typeOf(input) : written ? m_walker.typeOf(written) : TypeID() };
    bool isReceiver = input && input == (m_call.opcode() == Opcode::Call ? m_call.as<Opcode::Call>().thisValueOperand() : m_call.opcode() == Opcode::PropGet ? m_call.objectOperand() : ValueID());
    bool wordCanBeHeld = m_inputs.areOnlyTypes() ? operand.type && classThatHasTestOfObjectAsMadeInWalkBuiltins(m_walker, operand.type) : linksWordCanBeHeldAgainst(m_walker, written);
    operand.staysAsMade = wordCanBeHeld && (namesEntryParameterThatStaysAsMadeInWalkBuiltins(m_walker, m_call, input) || namesVariableThatStaysAsMadeInWalkBuiltins(m_walker, m_call, input) || (isReceiver && receiverStaysAsMadeByLinkInWalkBuiltins(m_walker, m_call, WordOfBodyOnAsMade::IsTaken, written)));
    LayoutID heir = isReceiver && look == LookAtReceiver::IsCountedOn && operand.type ? layoutKnownByStructureOfClass(m_walker, operand.type) : LayoutID();
    Reference name = heir && written ? nameBehindLookAtStructureOfClass(m_walker, written, heir) : Reference();
    if (heir && m_program.at(heir).writtenBuiltinBase && (m_inputs.areOnlyTypes() || name)) {
        operand.value = name;
        operand.type = m_types.builtinClass(m_program.at(heir).writtenBuiltinBase);
        operand.staysAsMade = false;
        operand.heirBehindLook = heir;
    }
    return operand;
}

bool CallInWalkBuiltins::describeOperands()
{
    if (m_mode == BuiltinCallMode::Get) {
        m_thisValue = operandAsWritten(m_call.objectOperand());
        return m_thisValue->type && !isNeverType(m_thisValue->type);
    }
    if (m_mode == BuiltinCallMode::Set) {
        m_thisValue = operandAsWritten(m_call.objectOperand());
        m_passed.append(operandAsWritten(m_call.valueOperand()));
        return holdsValue(*m_thisValue) && holdsValue(m_passed[0]);
    }
    if (m_forwarded) {
        Reference thisValue = m_forwarded->thisValueOperand();
        if (!thisValue)
            return false;
        m_thisValue = Operand { thisValue, thisValue, m_walker.typeOf(thisValue) };
        std::optional<Vector<Reference, 8>> elements;
        if (m_forwarded->opcode == Opcode::CallList) {
            elements = elementsOfFreshList(m_walker, m_forwarded->listOperand());
            if (!elements)
                return false;
            m_walker.checkWhatIsInSightOfList(m_forwarded->listOperand(), m_forwarded->origin());
        }
        for (Reference argument : elements ? elements->span() : m_forwarded->argumentsOperands())
            m_passed.append({ argument, argument, m_walker.typeOf(argument) });
        return holdsValue(*m_thisValue) && std::ranges::all_of(m_passed, holdsValue);
    }
    if (m_call.opcode() == Opcode::Construct || m_call.opcode() == Opcode::ConstructList) {
        m_mode = BuiltinCallMode::Construct;
        ValueID newTarget = m_call.newTargetOperand();
        const BodyIndex& index = m_walker.currentBodyIndex();
        bool isCallee = !newTarget || (m_inputs.areOnlyTypes()
            ? index.stripCasts(newTarget) == index.stripCasts(m_call.calleeOperand())
            : m_walker.referenceOf(newTarget) == m_walker.referenceOf(m_call.calleeOperand()));
        if (!isCallee)
            return false;
    } else if (ValueID thisValue = m_call.thisValueOperand())
        m_thisValue = operandAsWritten(thisValue);
    if (callsOfListsInWalkBuiltins.contains(m_call.opcode())) {
        std::optional<Vector<Reference, 8>> elements = elementsOfFreshList(m_walker, m_walker.referenceOf(m_call.listOperand()));
        if (!elements)
            return false;
        m_walker.checkWhatIsInSightOfList(m_walker.referenceOf(m_call.listOperand()), m_walker.originOf(m_call));
        for (Reference element : *elements)
            m_passed.append({ element, element, m_walker.typeOf(element) });
    } else {
        for (ValueID argument : m_call.argumentsOperands())
            m_passed.append(operandAsWritten(argument));
    }
    if (!std::ranges::all_of(m_passed, holdsValue))
        return false;
    bool looksAtFirstOperandAlone = m_builtin == m_table.canonicalID(Builtins::Number) || m_builtin == m_table.canonicalID(Builtins::String) || m_builtin == m_table.canonicalID(Builtins::Boolean);
    if (m_mode == BuiltinCallMode::Call && looksAtFirstOperandAlone && m_passed.size() > 1)
        m_passed.shrink(1);
    return !m_thisValue || holdsValue(*m_thisValue);
}

bool CallInWalkBuiltins::rowsSpeakOfThis() const
{
    if (m_thisValue)
        return rowsSpeakOfCallOnReceiver(m_context, m_builtin, m_thisValue->type);
    if (!m_table.at(m_builtin).holder)
        return true;
    return std::ranges::none_of(m_table.overloadsOf(m_builtin), [](const BuiltinOverload& overload) {
        return overload.mode == BuiltinCallMode::Call && (overload.receiver || overload.isUndeclared());
    });
}

BuiltinOverloadID CallInWalkBuiltins::generalFormOf(BuiltinOverloadID overload) const
{
    while (overload && m_table.at(overload).flags.contains(BuiltinOverloadFlag::IsSpecialCase)) {
        BuiltinOverloadID next;
        for (BuiltinOverloadID other : m_table.overloadIDsOf(m_table.at(overload).builtin)) {
            if (other > overload && m_table.at(other).mode == m_table.at(overload).mode) {
                next = other;
                break;
            }
        }
        overload = next;
    }
    return overload;
}

BuiltinOperandTypes CallInWalkBuiltins::typesAndAspectsPassed() const
{
    BuiltinOperandTypes passed;
    if (m_thisValue) {
        passed.receiver = m_thisValue->type;
        passed.aspectsOfReceiver = aspectsSeenOf(*m_thisValue);
    }
    for (const Operand& argument : m_passed) {
        passed.arguments.append(argument.type);
        passed.aspectsOfArguments.append(aspectsSeenOf(argument));
    }
    return passed;
}

bool CallInWalkBuiltins::matchOverload()
{
    if (m_mode == BuiltinCallMode::Call && !rowsSpeakOfThis())
        return false;

    Vector<BuiltinOverloadID, 4> reached = overloadsReachedBy(m_context, m_builtin, m_mode, typesAndAspectsPassed());
    if (reached.isEmpty())
        return false;
    m_overloadID = reached.size() == 1 ? reached[0] : generalFormOf(reached[0]);
    for (BuiltinOverloadID other : reached.span().subspan(1)) {
        if (generalFormOf(other) != m_overloadID)
            return false;
    }
    if (!m_overloadID)
        return false;
    m_overload = &m_table.at(m_overloadID);
    m_rowsThatOneStandsFor.clear();
    if (reached.size() > 1) {
        m_rowsThatOneStandsFor = WTF::move(reached);
        m_rowsThatOneStandsFor.append(m_overloadID);
    }
    return !m_overload->isUndeclared();
}

static bool membersAboveInstancesAreAsBuiltInWalkBuiltins(const Walker& walker, BuiltinClassID builtinClass)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    BuiltinID holder;
    for (BuiltinClassID current = builtinClass; current && !holder; current = table.at(current).parent)
        holder = table.at(current).prototype;
    while (holder) {
        holder = table.canonicalID(holder);
        if (walker.facts().hasOpenMember(holder) || program.isSomeNameOpenedOn(holder))
            return false;
        const Builtin& object = table.at(holder);
        holder = object.prototypeLink == BuiltinPrototypeLink::Builtin ? object.inheritsFrom : BuiltinID();
    }
    return true;
}

PristineAspectSet CallInWalkBuiltins::aspectsSeenOf(const Operand& operand) const
{
    PristineAspectSet seen;
    if (m_program.admitsReplacedBuiltins) {
        const ClassPart* onlyClass = operand.type ? singleBuiltinClassOf(m_types, m_types.at(operand.type)) : nullptr;
        if (!onlyClass || !membersAboveInstancesAreAsBuiltInWalkBuiltins(m_walker, onlyClass->builtinClass))
            return seen;
    }
    if (operand.staysAsMade) {
        seen = { PristineAspects::Names, PristineAspects::InheritedNamesKept, PristineAspects::Prototype };
        const ClassPart* part = operand.type ? singleBuiltinClassOf(m_types, m_types.at(operand.type)) : nullptr;
        if (part && indexesAreAbsentAboveInstancesInWalkBuiltins(m_walker, part->builtinClass))
            seen.add(PristineAspects::Storage);
    }
    if (!operand.written)
        return seen;
    return seen | (isKnownPristine(m_walker, operand.written) ? aspectsOfObjectAsMade : PristineAspectSet());
}

bool CallInWalkBuiltins::isProvenToLieIn(const Operand& operand, BuiltinTypeID tested) const
{
    OperandFit fit = fitOf(m_context, operand.type, tested);
    return fit.fit == TypeFit::Inside && (!fit.unprovenPristine || aspectsSeenOf(operand).containsAll(fit.unprovenPristine.aspects));
}

static bool isLeftUnchangedByConversionInWalkBuiltins(const Type& type, BuiltinConversion conversion)
{
    switch (conversion) {
    case BuiltinConversion::ToBoolean:
        return type.hasOnlyPrimitives(Primitive::Boolean);
    case BuiltinConversion::ToNumber:
    case BuiltinConversion::ToIntegerOrInfinity:
    case BuiltinConversion::ToInt32:
    case BuiltinConversion::ToUint32:
        return type.hasOnlyPrimitives(Primitive::Number);
    case BuiltinConversion::ToNumeric:
        return type.hasOnlyPrimitives(Primitive::Number | Primitive::BigInt);
    case BuiltinConversion::ToString:
        return type.hasOnlyPrimitives(Primitive::AnyString);
    case BuiltinConversion::ToPropertyKey:
        return type.hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol);
    case BuiltinConversion::ToObject:
        return !type.primitives && type.isJavaScriptValue();
    case BuiltinConversion::None:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToIndex:
    case BuiltinConversion::ToBigInt:
    case BuiltinConversion::Dictionary:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID CallInWalkBuiltins::typeConvertedWithoutCode(TypeID operand, BuiltinConversion conversion) const
{
    constexpr uint16_t everyPrimitive = Primitive::All | Primitive::LiteralString | Primitive::LiteralSymbol;
    constexpr uint16_t takenByToNumber = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString;

    uint16_t taken = 0;
    bool takesObjects = false;
    switch (conversion) {
    case BuiltinConversion::ToBoolean:
        taken = everyPrimitive;
        takesObjects = true;
        break;
    case BuiltinConversion::ToObject:
        taken = static_cast<uint16_t>(everyPrimitive & ~Primitive::UndefinedOrNull);
        takesObjects = true;
        break;
    case BuiltinConversion::ToPropertyKey:
        taken = everyPrimitive;
        break;
    case BuiltinConversion::ToNumber:
    case BuiltinConversion::ToIntegerOrInfinity:
    case BuiltinConversion::ToInt32:
    case BuiltinConversion::ToUint32:
        taken = takenByToNumber;
        break;
    case BuiltinConversion::ToNumeric:
    case BuiltinConversion::ToString:
        taken = takenByToNumber | Primitive::BigInt;
        break;
    case BuiltinConversion::None:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToIndex:
    case BuiltinConversion::ToBigInt:
    case BuiltinConversion::Dictionary:
        return TypeTable::never;
    }
    if (!m_types.at(operand).isJavaScriptValue())
        return TypeTable::never;
    TypeID converted = m_types.withoutPrimitives(operand, static_cast<uint16_t>(Primitive::Defined & ~taken));
    return takesObjects ? converted : m_types.withoutObjects(converted);
}

bool CallInWalkBuiltins::conversionsMayRunCode() const
{
    if (m_inputs.areOnlyTypes() || m_forwarded || m_call.opcode() != Opcode::Call || m_overload->flags.contains(BuiltinOverloadFlag::ReadsStateBeforeConversions))
        return false;
    bool onlyComputesWithNumbers = !m_overload->receiver && m_overload->effects.capability == BuiltinCapability::None && m_table.at(m_overload->builtin).holder == Builtins::Math
        && std::ranges::all_of(m_overload->parameters, [](const BuiltinParameter& parameter) {
            return parameter.conversion == BuiltinConversion::ToNumber || parameter.conversion == BuiltinConversion::ToInt32 || parameter.conversion == BuiltinConversion::ToUint32;
        });
    if (onlyComputesWithNumbers)
        return true;
    if (!m_overload->receiver || m_overload->parameters.size() != 1)
        return false;
    if (findRegExpWrittenForCall(m_walker, m_call, m_call.thisValueOperand()))
        return true;
    return m_overload->code.implementation.inlineBuiltin == InlineBuiltin::RegExpExec && m_receiver && m_receiver->conversion == BuiltinConversion::None;
}

bool CallInWalkBuiltins::convertsArgumentThatComesRoundLoop() const
{
    if (m_call.opcode() != Opcode::Call)
        return false;
    const BodyIndex& index = m_walker.currentBodyIndex();
    std::span<const ValueID> written = m_call.argumentsOperands();
    for (size_t position = 0; position < std::min(written.size(), m_arguments.size()); ++position) {
        const Operand& operand = m_arguments[position];
        ValueID carried = written[position] ? index.stripCasts(written[position]) : ValueID();
        if (!carried || !index.isParameter(carried) || !index.isLoopHead(index.blockOf(carried)))
            continue;
        if (operand.conversion != BuiltinConversion::None && operand.written && operand.written.isValue() && !isLeftUnchangedByConversionInWalkBuiltins(m_types.at(m_walker.typeOf(operand.written)), operand.conversion))
            return true;
    }
    return false;
}

bool CallInWalkBuiltins::planConversion(Operand& operand) const
{
    const BuiltinParameter& parameter = *operand.parameter;
    if (conversionOpcodesOf(parameter.conversion).empty())
        return false;
    TypeID takenUnconverted = m_types.intersectionOf(operand.type, typeOfBuiltinType(m_context, parameter.accepted));
    if (!isLeftUnchangedByConversionInWalkBuiltins(m_types.at(takenUnconverted), parameter.conversion))
        return false;
    if (typeConvertedWithoutCode(operand.type, parameter.conversion) != operand.type && !conversionsMayRunCode())
        return false;
    operand.conversion = parameter.conversion;
    operand.type = typeOfBuiltinType(m_context, parameter.converted ? parameter.converted : parameter.accepted);
    return true;
}

ALWAYS_INLINE_INTO_OTHER_FILES CallInWalkBuiltins::Outcome CallInWalkBuiltins::planOperand(Operand& operand, BuiltinOperand position) const
{
    operand.parameter = m_table.parameterFor(*m_overload, position);
    if (!operand.parameter)
        return Outcome::NoMoreOperands;
    const BuiltinParameter& parameter = *operand.parameter;
    if (!parameter.accepted)
        return Outcome::GaveUp;

    if (!operand.type) {
        switch (parameter.mode) {
        case BuiltinParameterMode::Required:
            if (distinguishesOmittedFromUndefined(m_table, parameter))
                return Outcome::NoMoreOperands;
            operand.type = TypeTable::undefined;
            break;
        case BuiltinParameterMode::Optional:
        case BuiltinParameterMode::Rest:
            return Outcome::NoMoreOperands;
        }
    }
    if (isProvenToLieIn(operand, parameter.accepted))
        return Outcome::Planned;
    if (parameter.refused && fitOf(m_context, operand.type, parameter.refused).fit != TypeFit::Outside)
        return Outcome::GaveUp;
    return planConversion(operand) ? Outcome::Planned : Outcome::GaveUp;
}

bool CallInWalkBuiltins::planOperands()
{
    if (m_overload->receiver) {
        if (!m_thisValue)
            return false;
        m_receiver = *m_thisValue;
        if (planOperand(*m_receiver, BuiltinOperand::receiver()) != Outcome::Planned)
            return false;
    }
    for (unsigned index = 0;; ++index) {
        Operand operand = index < m_passed.size() ? m_passed[index] : Operand { };
        Outcome outcome = planOperand(operand, BuiltinOperand::argument(index));
        if (outcome == Outcome::GaveUp)
            return false;
        if (outcome == Outcome::NoMoreOperands)
            break;
        m_arguments.append(operand);
    }

    BuiltinOperandTypes received;
    if (m_receiver)
        received.receiver = m_receiver->type;
    for (const Operand& argument : m_arguments)
        received.arguments.append(argument.type);
    return overloadTakesOperands(m_context, m_overloadID, BuiltinCallForm::Whole, received);
}

bool CallInWalkBuiltins::hasNativeEntry() const
{
    if (BuiltinBindings::directFunctionNumberOf(m_overloadID, m_arguments.size()))
        return true;
    if (m_table.at(m_overload->builtin).flags.contains(BuiltinFlag::IsCompilerHelper))
        return !BuiltinBindings::hasHost() && m_overload->code.implementation.kind == BuiltinImplementationKind::HostCall;
    BuiltinCellKey callee = m_mode == BuiltinCallMode::Get ? BuiltinCellKey::getter(m_overload->builtin) : m_mode == BuiltinCallMode::Set ? BuiltinCellKey::setter(m_overload->builtin) : BuiltinCellKey::object(m_overload->builtin);
    if (!callee.hasCell(m_table))
        return false;

    const BuiltinImplementation& implementation = m_overload->code.implementation;
    switch (implementation.kind) {
    case BuiltinImplementationKind::HostCall:
    case BuiltinImplementationKind::Compiled:
        return entersNativeCode(implementation);
    case BuiltinImplementationKind::Inline:
        return entersNativeCode(implementation) || m_overload->effects.capability != BuiltinCapability::JS;
    case BuiltinImplementationKind::Generic:
    case BuiltinImplementationKind::Number:
    case BuiltinImplementationKind::Cell:
    case BuiltinImplementationKind::CachedGetter:
    case BuiltinImplementationKind::OwnSlot:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool CallInWalkBuiltins::runsAtMostOnce() const
{
    FunctionID function = m_walker.unit().function;
    return m_program.at(function).isModuleBody();
}

bool CallInWalkBuiltins::asksForOwnPropertyUnderWrittenName()
{
    bool asksReceiver = !!m_overload->receiver;
    const Operand* object = asksReceiver ? (m_receiver ? &*m_receiver : nullptr) : (m_arguments.isEmpty() ? nullptr : &m_arguments[0]);
    size_t placeOfKey = asksReceiver ? 0 : 1;
    StringID name = object && placeOfKey < m_arguments.size() ? singleStringLiteralOf(m_types.at(m_arguments[placeOfKey].type)) : StringID();
    PropertyKeyID key = name && !liesInThread(name) ? m_types.textKey(name) : PropertyKeyID();
    if (!key || liesInThread(key) || mayBeElementKey(m_program.nameClassOf(key)))
        return false;
    ValueID written;
    if (m_call.opcode() == Opcode::Call && !m_inputs.areOnlyTypes()) {
        ValueID firstArgument = m_call.argumentsOperands().empty() ? ValueID() : m_call.argumentsOperands()[0];
        if (asksReceiver)
            written = m_forwarded ? firstArgument : m_call.thisValueOperand();
        else if (!m_forwarded)
            written = firstArgument;
    }
    LayoutSetID receivers = written && m_walker.referenceOf(written) == object->written ? receiversWhoseCellsAnswerForOwnProperty(m_walker, written, key) : LayoutSetID();
    m_ownPropertyAskedFor = key;
    m_receiversAskedForOwnProperty = receivers;
    return true;
}

bool CallInWalkBuiltins::chooseForm()
{
    BuiltinTypeID result = m_overload->result.type;
    if (m_mode == BuiltinCallMode::Construct && result && m_table.at(result).isNever())
        return false;

    const BuiltinImplementation& implementation = m_overload->code.implementation;
    if (implementation.inlineBuiltin == InlineBuiltin::ObjectHasOwn && asksForOwnPropertyUnderWrittenName())
        return true;

    if (implementation.kind == BuiltinImplementationKind::Inline && implementation.inlineBuiltin) {
        BuiltinOperandTypes received;
        if (m_receiver)
            received.receiver = m_receiver->type;
        for (const Operand& argument : m_arguments)
            received.arguments.append(argument.type);
        AccessContext context { m_program, WalkFacts(m_walker.facts()), m_walker.plan(), m_types, m_walker.currentFunction() };
        bool isHiddenInStackTraces = m_program.at(m_walker.currentFunction()).isHiddenInStackTraces();
        m_inlineForm = chooseInlineForm({ context, *m_overload, *implementation.inlineBuiltin, BuiltinCallForm::Whole, received, !!implementation.otherwise, isDoneInPlace(), isHiddenInStackTraces });
        if (m_inlineForm == InlineForm::TwoWay && *implementation.inlineBuiltin != InlineBuiltin::ReflectConstruct && ((!m_walker.isInLoop() && runsAtMostOnce()) || m_walker.isOutsideLoopsOfAsyncBody()))
            m_inlineForm = InlineForm::Call;
        bool stubDoesSameWork = !m_walker.isInLoop() && implementation.inlineBuiltin == InlineBuiltin::ArrayAt;
        bool pushesSeveralOnce = implementation.inlineBuiltin == InlineBuiltin::ArrayPush && m_arguments.size() > 1 && !m_walker.isInRepeatedCode();
        if (stubDoesSameWork || pushesSeveralOnce || (m_inlineForm != InlineForm::TwoWay && hostReadsMembersFirstIn(m_table, *m_overload)))
            m_inlineForm = InlineForm::Call;
    }
    if (m_inlineForm != InlineForm::Whole && !hasNativeEntry())
        return false;
    if (m_inlineForm != InlineForm::Call)
        m_inlineBuiltin = *implementation.inlineBuiltin;
    return true;
}

void CallInWalkBuiltins::forgetWhatIsPlanned()
{
    m_arguments.clear();
    m_ownPropertyAskedFor = { };
    m_receiversAskedForOwnProperty = { };
    m_inlineForm = InlineForm::Call;
    m_inlineBuiltin = std::nullopt;
    m_receiver = std::nullopt;
    m_rowsThatOneStandsFor.clear();
}

bool CallInWalkBuiltins::rowsFindNothingAboveLiteral(std::span<const BuiltinOverloadID> rowsAsWritten, unsigned place, std::span<const BuiltinName> namesOfParts) const
{
    auto isAnsweredByLiteralAlone = [&](BuiltinName name) {
        auto part = std::ranges::find(namesOfParts, name);
        if (part != namesOfParts.end() && m_passed[place + (part - namesOfParts.begin())].written)
            return true;
        return literalsFindNothingUnder(m_program, m_program.keyOf(name));
    };
    for (BuiltinOverloadID id : rowsAsWritten) {
        const BuiltinOverload& row = m_table.at(id);
        const BuiltinParameter* parameter = m_table.parameterFor(row, BuiltinOperand::argument(place));
        if (row.isUndeclared() || row.receiver || !parameter)
            return false;
        if ((parameter->protocols.contains(Protocol::Named) && parameter->namedMembers.empty()) || (parameter->protocols.contains(Protocol::CallsNamed) && parameter->calledMembers.empty()))
            return false;
        for (std::span<const BuiltinName> asked : { parameter->namedMembers.span(), parameter->calledMembers.span(), parameter->convertedMembers.span() }) {
            if (!std::ranges::all_of(asked, isAnsweredByLiteralAlone))
                return false;
        }
    }
    return !rowsAsWritten.empty() && std::ranges::all_of(namesOfParts, isAnsweredByLiteralAlone);
}

bool CallInWalkBuiltins::planOnWidestRowOfOneNativeFunction()
{
    forgetWhatIsPlanned();
    Vector<BuiltinOverloadID, 4> reached = overloadsReachedBy(m_context, m_builtin, m_mode, typesAndAspectsPassed());
    if (reached.isEmpty())
        return false;
    m_overloadID = *std::ranges::max_element(reached);
    m_overload = &m_table.at(m_overloadID);
    auto functionEnteredBy = [](BuiltinOverloadID id) {
        std::optional<uint32_t> number = BuiltinBindings::directFunctionNumberOf(id);
        return number ? BuiltinBindings::directFunctionAt(*number).function : nullptr;
    };
    bool isOneFunctionWithOneResult = std::ranges::all_of(reached, [&](BuiltinOverloadID id) {
        return functionEnteredBy(id) == functionEnteredBy(m_overloadID) && m_table.at(id).result.type == m_overload->result.type && !m_table.at(id).isUndeclared();
    });
    m_rowStandsForSeveral = isOneFunctionWithOneResult && planOperands() && chooseForm() && m_inlineForm == InlineForm::Call;
    if (m_rowStandsForSeveral)
        m_rowsThatOneStandsFor = WTF::move(reached);
    return m_rowStandsForSeveral;
}

bool CallInWalkBuiltins::planOnRowsThatEnterOneDoor()
{
    if (m_table.at(m_builtin).origin == BuiltinOrigin::Host && planOnWidestRowOfOneNativeFunction() && BuiltinBindings::directFunctionNumberOf(m_overloadID))
        return true;
    forgetWhatIsPlanned();
    m_rowStandsForSeveral = false;
    return false;
}

bool CallInWalkBuiltins::planOnPartsOfLiteral()
{
    const BuiltinForms* forms = m_table.formsOf(m_builtin);
    if (!forms || !forms->literalPartsForm || m_inputs.areOnlyTypes() || m_forwarded || isDoneInPlace() || callsOfListsInWalkBuiltins.contains(m_call.opcode()) || !m_walker.has(Mechanism::EscapeAnalysis))
        return false;
    unsigned place = forms->literalPartsArgument->argumentIndex();
    if (m_mode != (m_table.at(m_builtin).kind == BuiltinKind::Constructor ? BuiltinCallMode::Construct : BuiltinCallMode::Call) || m_passed.size() != place + 1)
        return false;
    std::optional<Vector<PropertyOfFreshObject, 8>> properties = propertiesOfLiteralWrittenForCall(m_walker, m_call, m_call.argumentsOperands()[place], m_passed[place].written);
    if (!properties)
        return false;

    std::span<const BuiltinName> names = std::span(forms->literalPartNames).first(std::ranges::count_if(forms->literalPartNames, [](BuiltinName name) {
        return !!name;
    }));
    Vector<Operand, 6> operands;
    for (const Operand& operand : m_passed.span().first(place))
        operands.append(operand);
    for (size_t index = 0; index < names.size(); ++index)
        operands.append(Operand { { }, { }, TypeTable::undefined });
    for (const PropertyOfFreshObject& property : *properties) {
        PropertyKeyID key = m_walker.plan().slots().keyOf(m_program, property.field);
        BuiltinName name = key ? m_program.builtinNameOf(key) : BuiltinName();
        auto part = name ? std::ranges::find(names, name) : names.end();
        TypeID type = m_walker.typeOf(property.value);
        if (part == names.end() || m_walker.plan().slot(property.field.layout, property.field.field).holdsAccessor || !type)
            return false;
        if (!m_types.at(type).isJavaScriptValue() || m_types.at(type).primitives & (Primitive::Absent | Primitive::Unspecified))
            return false;
        operands[place + (part - names.begin())] = Operand { property.value, property.value, type };
    }

    Vector<BuiltinOverloadID, 4> rowsAsWritten = overloadsReachedBy(m_context, m_builtin, m_mode, typesAndAspectsPassed());
    BuiltinID builtinAsWritten = std::exchange(m_builtin, m_table.canonicalID(forms->literalPartsForm));
    BuiltinCallMode modeAsWritten = std::exchange(m_mode, BuiltinCallMode::Call);
    std::optional<Operand> thisValueAsWritten = std::exchange(m_thisValue, std::nullopt);
    std::swap(m_passed, operands);
    if (((matchOverload() && planOperands() && chooseForm()) || planOnWidestRowOfOneNativeFunction()) && rowsFindNothingAboveLiteral(rowsAsWritten.span(), place, names)) {
        m_walker.count(StatisticID::LiteralArgumentsScalarized);
        return true;
    }
    forgetWhatIsPlanned();
    m_rowStandsForSeveral = false;
    m_builtin = builtinAsWritten;
    m_mode = modeAsWritten;
    m_thisValue = thisValueAsWritten;
    std::swap(m_passed, operands);
    return false;
}

static bool isTypeOnlyResult(const BuiltinResult& result)
{
    return result.rangeRule == BuiltinRangeRule::None && !result.rangeOperand && !result.substringOf && !result.rangeOf && result.typedResultForm == TypedResultForm::None && !result.typedResultSource
        && !result.iteratedOperand && !result.prototypeSource && !result.testedOperand && !result.testedType && !result.properties.size();
}

std::optional<Effects> CallInWalkBuiltins::effectsIfOnlyNullishRejected()
{
    if (m_mode != BuiltinCallMode::Call || m_overload->receiver || !m_walker.opensGuesses())
        return std::nullopt;
    Vector<Operand, 6> written;
    for (size_t place = 0; place < m_arguments.size(); ++place) {
        const BuiltinParameter* parameter = m_table.parameterFor(*m_overload, BuiltinOperand::argument(place));
        TypeID rest = m_types.withoutNullish(m_arguments[place].type);
        OperandFit fit = parameter && !isNeverType(rest) ? fitOf(m_context, rest, parameter->accepted) : OperandFit();
        if (fit.fit != TypeFit::Inside || fit.unprovenPristine)
            return std::nullopt;
        written.append(m_arguments[place]);
        written.last().parameter = parameter;
    }
    std::swap(m_arguments, written);
    Effects effects = effectsOfRow(m_inlineBuiltin);
    std::swap(m_arguments, written);
    if (effects.mayRunOtherCode())
        return std::nullopt;
    effects.mayThrow = true;
    effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    return effects;
}

bool CallInWalkBuiltins::planOnAnyOperands()
{
    forgetWhatIsPlanned();
    if (m_table.at(m_builtin).scope == BuiltinScope::Module || !isTestableBuiltin(m_table, m_builtin))
        return false;
    if (m_thisValue && m_thisValue->heirBehindLook)
        m_thisValue = operandAsWritten(m_call.thisValueOperand(), LookAtReceiver::IsLeftAside);

    BuiltinOperandTypes passed;
    if (m_mode == BuiltinCallMode::Call) {
        m_receiver = m_thisValue ? *m_thisValue : Operand { { }, { }, TypeTable::undefined };
        passed.receiver = m_receiver->type;
    }
    for (const Operand& argument : m_passed)
        passed.arguments.append(argument.type);

    Vector<BuiltinOverloadID, 4> reached = overloadsReachedBy(m_context, m_builtin, m_mode, passed);
    bool everyRowEntersNativeCode = std::ranges::all_of(reached, [&](BuiltinOverloadID id) {
        const BuiltinOverload& row = m_table.at(id);
        bool isRefusedConstruction = m_mode == BuiltinCallMode::Construct && row.result.type && m_table.at(row.result.type).isNever();
        bool leavesByTailCall = m_origin.isTailCall && !entersNativeCode(row.code.implementation);
        return row.code.implementation.kind != BuiltinImplementationKind::Generic && !row.libraryFunction() && !isRefusedConstruction && !leavesByTailCall;
    });
    if (reached.isEmpty() || !everyRowEntersNativeCode)
        return false;

    m_overloadID = reached[0];
    m_overload = &m_table.at(m_overloadID);
    m_rowsThatOneStandsFor = reached;
    m_arguments = m_passed;
    m_form = BuiltinCallForm::AnyReceiver;
    m_effects = Effects::unknownCode();

    const BuiltinImplementation& implementation = m_overload->code.implementation;
    bool everyRowHasTheSameCodeInLine = std::ranges::all_of(reached, [&](BuiltinOverloadID id) {
        const BuiltinOverload& row = m_table.at(id);
        const BuiltinImplementation& ofRow = row.code.implementation;
        return ofRow.kind == BuiltinImplementationKind::Inline && ofRow.inlineBuiltin && ofRow.inlineBuiltin == implementation.inlineBuiltin && ofRow.inlineParameters == implementation.inlineParameters
            && ofRow.otherwise == implementation.otherwise && !row.receiver == !m_overload->receiver && !hostReadsMembersFirstIn(m_table, row)
            && (reached.size() == 1 || (row.result.type == m_overload->result.type && isTypeOnlyResult(row.result)));
    });
    if (everyRowHasTheSameCodeInLine) {
        if (!m_overload->receiver)
            passed.receiver = { };
        AccessContext context { m_program, WalkFacts(m_walker.facts()), m_walker.plan(), m_types, m_walker.currentFunction() };
        bool isHiddenInStackTraces = m_program.at(m_walker.currentFunction()).isHiddenInStackTraces();
        InlineForm formOnAnyOperands = chooseInlineForm({ context, *m_overload, *implementation.inlineBuiltin, BuiltinCallForm::Whole, passed, !!implementation.otherwise, isDoneInPlace(), isHiddenInStackTraces, true, m_walker.isInLoop() });
        if (formOnAnyOperands == InlineForm::Whole && reached.size() == 1) {
            m_inlineForm = InlineForm::Whole;
            m_inlineBuiltin = *implementation.inlineBuiltin;
            m_form = BuiltinCallForm::Whole;
            if (!m_overload->receiver)
                m_receiver = std::nullopt;
            if (*implementation.inlineBuiltin == InlineBuiltin::ObjectHasOwn)
                asksForOwnPropertyUnderWrittenName();
            if (std::optional<Effects> effects = effectsIfOnlyNullishRejected())
                m_effects = *effects;
        }
        if (formOnAnyOperands == InlineForm::TwoWay && (m_walker.isInLoop() || (reached.size() == 1 && !runsAtMostOnce() && !m_walker.isOutsideLoopsOfAsyncBody()))) {
            m_inlineForm = InlineForm::TwoWay;
            m_inlineBuiltin = *implementation.inlineBuiltin;
            m_effectsOfAttempt = Effects::unknownCode();
        }
    }
    return true;
}

bool CallInWalkBuiltins::everyRowThatCallMayTake(const Invocable<bool(const BuiltinOverload&)> auto& holds) const
{
    if (m_rowsThatOneStandsFor.isEmpty() || m_walker.shouldIgnoreAlternativeOverloads())
        return holds(*m_overload);
    return std::ranges::all_of(m_rowsThatOneStandsFor, [&](BuiltinOverloadID id) {
        return holds(m_table.at(id));
    });
}

bool CallInWalkBuiltins::mayRunCodeThatNoOperandNames(const BuiltinOverload& row) const
{
    if (row.effects.capability == BuiltinCapability::JS && !row.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode))
        return true;
    if (m_table.at(row.builtin).flags.containsAny({ BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode }))
        return true;
    if (m_table.has(BuiltinMark::RunsWaitingJobs, row.builtin))
        return true;
    return std::ranges::any_of(row.flow.handlerCalls, [](const BuiltinHandlerCall& call) {
        return !isCalledAfterReturn(call.times);
    });
}

bool CallInWalkBuiltins::namesNoHeapForItsUses() const
{
    OperandUses uses;
    if (m_overload->receiver)
        uses.add(m_overload->receiver->uses);
    for (const BuiltinParameter& parameter : m_overload->parameters)
        uses.add(parameter.uses);
    if (uses.containsAny(usesThatChangeOperand) && m_overload->effects.writes.isEmpty())
        return true;
    return uses.contains(OperandUse::ReadsContents) && m_overload->effects.reads.isEmpty();
}

bool CallInWalkBuiltins::takesObjectsByClassAlone(BuiltinTypeID acceptedID) const
{
    const BuiltinType& accepted = m_table.at(acceptedID);
    if (accepted.flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasPlainData }))
        return false;
    return std::ranges::all_of(accepted.parts, [](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Instance && part.contents.empty();
    });
}

bool CallInWalkBuiltins::mayRaiseErrorOn(const BuiltinParameter& parameter, TypeID operandOrNotPassed, const Type& type) const
{
    constexpr Protocols protocolsThatNeedMembers { Protocol::Iterate, Protocol::CallsNamed, Protocol::Species, Protocol::HasInstance };
    constexpr uint16_t primitivesThatBecomeStringsAndNumbers = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString;

    if (parameter.admitted)
        return mayBeObject(type) || fitOf(m_context, operandOrNotPassed, parameter.admitted).fit != TypeFit::Inside;
    if (parameter.protocols.containsAny(protocolsThatNeedMembers))
        return true;
    if (parameter.protocols.contains(Protocol::ToPrimitive) && !type.hasOnlyPrimitives(primitivesThatBecomeStringsAndNumbers))
        return true;
    return mayBeObject(type) && parameter.uses.containsAny(usesThatChangeOperand);
}

bool CallInWalkBuiltins::callsBeforeReturn(const BuiltinParameter& parameter) const
{
    if (!parameter.uses.containsAny(usesThatCallOperand))
        return false;
    bool isListedAsCalledLater = false;
    for (const BuiltinCallback& callback : m_overload->flow.callbacks) {
        if (callback.callee != parameter.operand)
            continue;
        if (!isCalledAfterReturn(callback.times))
            return true;
        isListedAsCalledLater = true;
    }
    return !isListedAsCalledLater;
}

static bool rowLooksIntoAnswerInWalkBuiltins(BuiltinCallbackResultUse use)
{
    switch (use) {
    case BuiltinCallbackResultUse::Ignored:
    case BuiltinCallbackResultUse::Tested:
    case BuiltinCallbackResultUse::Returned:
        return false;
    case BuiltinCallbackResultUse::Escapes:
    case BuiltinCallbackResultUse::Stored:
    case BuiltinCallbackResultUse::Flattened:
    case BuiltinCallbackResultUse::AskedWhetherIterable:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool CallInWalkBuiltins::addEffectsOfFunctionsCalled(Effects& effects, const BuiltinParameter& parameter, const Type& type) const
{
    if (!type.hasOnlyClosures() || type.namesMergedObjects())
        return false;

    bool rowListsCall = false;
    for (const BuiltinCallback& callback : m_overload->flow.callbacks) {
        if (callback.callee != parameter.operand)
            continue;
        if (callback.constructs || callback.afterwards != BuiltinCallbackAftermath::TouchesOnlyWhatRowNames || rowLooksIntoAnswerInWalkBuiltins(callback.resultUse))
            return false;
        rowListsCall = true;
    }
    if (!rowListsCall)
        return false;

    const Facts& facts = m_walker.facts();
    Effects ofFunctions = Effects::none();
    bool everyBodyIsEntered = true;
    forEachFunctionOf(m_types, type, [&](FunctionID function) {
        const Function& callee = m_program.at(function);
        everyBodyIsEntered &= callee.hasBody() && !isClassConstructorOf(callee.kind);
        ofFunctions.merge(facts.effectsOnReturn(function));
        ofFunctions.merge(facts.effectsOnThrow(function));
    });
    if (!everyBodyIsEntered)
        return false;

    ofFunctions.mayThrow |= ThrowEffect::onlyStackOverflow();
    if (type.primitives)
        ofFunctions.mayThrow = true;
    effects.merge(ofFunctions);
    return true;
}

void CallInWalkBuiltins::addEffectsOn(Effects& effects, const Operand& operand) const
{
    if (effects.mayRunOtherCode())
        return;
    const BuiltinParameter& parameter = *operand.parameter;
    Type type = m_types.at(operand.type);
    bool isObject = mayBeObject(type);

    if (readsCharactersOfString(parameter.conversion) && (type.primitives & Primitive::AnyString)) {
        Effects ofReadingCharacters = declaredBoundOf(NamedEffectSet::ResolvesRopes);
        ofReadingCharacters.mayThrow = ThrowEffect::onlyForLackOfMemory();
        effects.merge(ofReadingCharacters);
    }
    if (m_overload->onlyPropagatesErrors() && mayRaiseErrorOn(parameter, operand.type, type))
        effects.mayThrow = true;
    if (!parameter.protocols.isEmpty()) {
        TypingContext typing { m_program, m_types, PositionTypes::knowingNothing() };
        bool reachesBeyondOperand = isObject && parameter.uses.containsAny(usesThatReachBeyondOperand);
        std::optional<Effects> resolved = isObject && !reachesBeyondOperand ? effectsOfProtocolsResolvedOn(m_walker, parameter.protocols, operand.type) : std::nullopt;
        effects.merge(resolved ? *resolved : reachesBeyondOperand ? Effects::unknownCode() : effectsOfProtocolsOn(typing, parameter.protocols, operand.type));
    }
    if (effects.mayRunOtherCode() || !isObject)
        return;

    bool mayBeDescribedInMessage = parameter.roles.contains(BuiltinOperandRole::DescribedWhenRefused) && !type.hasOnlyClosures() && !takesObjectsByClassAlone(parameter.accepted);
    if (parameter.uses.containsAny(usesThatStoreByKey) || mayBeDescribedInMessage || (callsBeforeReturn(parameter) && !addEffectsOfFunctionsCalled(effects, parameter, type))) {
        effects.merge(Effects::unknownCode());
        return;
    }
    if (parameter.uses.containsAny(usesThatChangeShape))
        effects.writes.add(AbstractHeapSet { AbstractHeap::PrototypeLinks, AbstractHeap::MemberGuards, AbstractHeap::ShapeWords, AbstractHeap::OrderWords });
    if (parameter.uses.contains(OperandUse::DefinesName))
        effects.writes.add(AbstractHeapSet { AbstractHeap::NamedProperties });
}

bool CallInWalkBuiltins::holdsOnlyCompiledRecords(TypeID operand) const
{
    Type type = m_types.at(m_types.withoutNullish(operand));
    if (!type.isObject() || !type.hasOnlyLayouts() || type.hasOnlyClosures())
        return false;
    return std::ranges::all_of(membersOf(m_types, type.layouts), [&](LayoutID member) {
        LayoutKind kind = m_program.at(member).kind;
        return (kind == LayoutKind::Record || kind == LayoutKind::Instance) && m_walker.plan().layout(member).holdsCompiledObjects();
    });
}

Effects CallInWalkBuiltins::effectsOfRow(std::optional<InlineBuiltin> codeInLine) const
{
    const BuiltinOverload& row = *m_overload;
    if (row.libraryFunction())
        return effectsOfLibraryFunctionBehindRow(m_walker, row);
    if (mayRunCodeThatNoOperandNames() || namesNoHeapForItsUses())
        return Effects::unknownCode();

    Effects effects = Effects::none();
    EffectsSinkInWalkBuiltins ofCodeInLine;
    if (codeInLine)
        forEachDeclaredEffect(*codeInLine, ofCodeInLine);
    if (codeInLine && !ofCodeInLine.standsForOverload()) {
        effects = effectsOfSearchForWhatIsNoString(m_types, *codeInLine, m_arguments.isEmpty() ? TypeID() : m_arguments[0].type).value_or(ofCodeInLine.effects());
        if (effects.mayThrow && row.onlyPropagatesErrors())
            effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    } else {
        if (!row.receiver && row.parameters.empty() && row.effects.reads.isEmpty() && row.effects.writes.isEmpty())
            return Effects::unknownCode();
        effects.reads.add(row.effects.reads);
        effects.writes.add(row.effects.writes);
        switch (row.effects.capability) {
        case BuiltinCapability::None:
            break;
        case BuiltinCapability::Heap:
        case BuiltinCapability::JS: {
            Effects ofHeap = declaredBoundOf(NamedEffectSet::Allocates);
            ofHeap.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
            ofHeap.merge(declaredBoundOf(NamedEffectSet::ResolvesRopes));
            ofHeap.mayThrow = ThrowEffect::onlyForLackOfMemory();
            effects.merge(ofHeap);
            break;
        }
        }
        if (row.throws())
            effects.mayThrow |= row.onlyPropagatesErrors() ? ThrowEffect::onlyForLackOfMemory() : ThrowEffect(true);
    }
    if (m_table.has(BuiltinMark::MayTerminateProcess, row.builtin))
        effects.mayBeTerminated = true;

    if (codeInLine == InlineBuiltin::OwnPropertyKeys && m_arguments.size() == 1 && holdsOnlyCompiledRecords(m_arguments[0].type))
        effects.writes.remove(AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::NamedProperties });
    if (m_receiver)
        addEffectsOn(effects, *m_receiver);
    for (const Operand& argument : m_arguments)
        addEffectsOn(effects, argument);
    if (effects.mayRunOtherCode())
        return Effects::unknownCode();

    if (row.onlyPropagatesErrors()) {
        Type undefined = m_types.at(TypeTable::undefined);
        for (const BuiltinParameter& parameter : row.parameters) {
            bool isLeftOut = parameter.operand.kind() == BuiltinOperand::Kind::Argument && parameter.operand.argumentIndex() >= m_arguments.size();
            if (isLeftOut && mayRaiseErrorOn(parameter, TypeID(), undefined))
                effects.mayThrow = true;
        }
    }
    if (effects.mayThrow)
        effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    return effects;
}

bool CallInWalkBuiltins::makesFreshResult(Reference result) const
{
    if (!m_walker.has(Mechanism::EscapeAnalysis) || !result.isValue() || m_effects.mayRunOtherCode() || isDoneInPlace())
        return false;
    BuiltinTypeID written = m_overload->result.type;
    bool isInjectedAsFresh = m_program.builtinInjectedAsFresh == m_overload->builtin.index();
    return written && (m_table.at(written).freshness == BuiltinFreshness::Yes || isInjectedAsFresh) && m_types.at(m_walker.typeOf(result)).isObject();
}

Vector<Reference, 4> CallInWalkBuiltins::freshOperandsThatStayHere() const
{
    static constexpr OperandUses usesThatHandOperandToNobody {
        OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::RetainsContents, OperandUse::WritesContents, OperandUse::MovesElements,
        OperandUse::ChangesIntegrity,
    };

    Vector<Reference, 4> staying;
    if (!m_walker.has(Mechanism::EscapeAnalysis) || m_effects.mayRunOtherCode() || isDoneInPlace())
        return staying;

    Vector<Reference, 4> leaving;
    auto sort = [&](const Operand& operand) {
        if (!operand.value.isValue())
            return;
        Reference object = Reference::value(underlyingCellOf(m_walker.code(), operand.value.id()));
        if (!m_walker.state().isFresh(object))
            return;
        if (operand.parameter->uses.containsOnly(usesThatHandOperandToNobody))
            staying.append(object);
        else
            leaving.append(object);
    };
    if (m_receiver)
        sort(*m_receiver);
    for (const Operand& argument : m_arguments)
        sort(argument);
    staying.removeAllMatching([&](Reference object) {
        return leaving.contains(object);
    });
    return staying;
}

Vector<Reference, 4> CallInWalkBuiltins::operandsThatStayAsMade() const
{
    auto mayChangeObjects = [](const Effects& effects) {
        return effects.mayRunOtherCode() || effects.writes.containsAny({ AbstractHeap::PrototypeLinks, AbstractHeap::ShapeWords });
    };
    Vector<Reference, 4> staying;
    if (mayChangeObjects(m_effects) || (m_inlineForm == InlineForm::TwoWay && mayChangeObjects(m_effectsOfAttempt)))
        return staying;
    auto add = [&](const Operand& operand) {
        if (operand.value.isValue() && isKnownPristine(m_walker, operand.value))
            staying.append(operand.value);
    };
    if (m_receiver)
        add(*m_receiver);
    for (const Operand& argument : m_arguments)
        add(argument);
    return staying;
}

bool CallInWalkBuiltins::plan()
{
    if (!findCallee() || !mayBelieveRows())
        return false;
    if (!m_inputs.areOnlyTypes()) {
        Draft written = m_walker.draftOf(m_call);
        m_origin = written.origin();
        m_expected = written.expected;
        m_resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    }
    if (!describeOperands())
        return false;
    if (!planOnPartsOfLiteral() && (!matchOverload() || !planOperands() || !chooseForm()) && !planOnRowsThatEnterOneDoor())
        return m_mode != BuiltinCallMode::Get && m_mode != BuiltinCallMode::Set && planOnAnyOperands();
    m_effects = m_rowStandsForSeveral ? Effects::unknownCode() : effectsOfRow(m_inlineForm == InlineForm::Whole ? m_inlineBuiltin : std::nullopt);
    if (m_inlineForm == InlineForm::TwoWay)
        m_effectsOfAttempt = effectsOfRow(m_inlineBuiltin);
    if (m_forwarded && m_overload->receiver && !m_overload->receiver->admitted && m_types.at(m_thisValue->type).mayBeUndefinedOrNull()) {
        m_effects = Effects::unknownCode();
        m_effectsOfAttempt = Effects::unknownCode();
    }
    return true;
}

FunctionID CallInWalkBuiltins::findLibraryFunctionOfRowsReached(RowsThatNameLibraryFunction taken)
{
    if (!findCallee() || !mayBelieveRows() || !describeOperands() || m_mode != BuiltinCallMode::Call || !rowsSpeakOfThis())
        return { };
    FunctionID named;
    for (BuiltinOverloadID id : overloadsReachedBy(m_context, m_builtin, m_mode, typesAndAspectsPassed())) {
        const BuiltinOverload& row = m_table.at(id);
        const RuntimeFunctionEntry* held = row.isUndeclared() ? nullptr : findRuntimeFunction(m_program, row.libraryFunction());
        if (!held || !held->function || (named && named != held->function))
            return { };
        if (taken == RowsThatNameLibraryFunction::SpecialCasesAlone && !row.flags.contains(BuiltinOverloadFlag::IsSpecialCase))
            return { };
        named = held->function;
    }
    return named && takesNothingOfFunctionObject(m_program, named) ? named : FunctionID();
}

BuiltinOverloadID CallInWalkBuiltins::findMatchingOverload()
{
    if (!findCallee() || !mayBelieveRows() || !describeOperands() || m_mode != BuiltinCallMode::Call || !rowsSpeakOfThis())
        return { };
    BuiltinOverloadID match;
    for (BuiltinOverloadID id : overloadsReachedBy(m_context, m_builtin, m_mode, typesAndAspectsPassed())) {
        if (match)
            return { };
        match = id;
    }
    return match;
}
TypeID CallInWalkBuiltins::findTypeOfResultOfRowsReached()
{
    if (!findCallee() || !mayBelieveRows() || !describeOperands() || m_mode != BuiltinCallMode::Call || !rowsSpeakOfThis())
        return { };
    return resultTypeOfReachableOverloads(m_context, m_builtin, m_mode, typesAndAspectsPassed());
}

bool CallInWalkBuiltins::tryGuardStringOperand(Operand& operand, Origin origin)
{
    TypeID type = m_walker.typeOf(operand.value);
    if (!type || typeConvertedWithoutCode(type, BuiltinConversion::ToString) == type || !m_types.overlap(type, TypeTable::string))
        return false;
    if (!m_walker.opensGuesses() || !m_walker.canRestartIteration() || !m_walker.canBuildIterationGuard())
        return false;
    origin.isTailCall = false;
    Reference isText = buildTestOfNumber(m_walker, operand.value, TypeTable::string, origin);
    if (!isText)
        return false;
    BlockID passed = m_walker.addBlock();
    BlockID failed = m_walker.addBlock(BlockFrequency::Rare);
    m_walker.letGuessFailUnasked(failed, origin);
    m_walker.branch(isText, passed, failed);
    m_walker.continueIn(failed);
    if (!m_walker.isUnreachable())
        m_walker.restartIteration();
    m_walker.continueIn(passed);
    m_walker.recordSpeculation();
    if (m_walker.isUnreachable() || m_walker.hasFailed())
        return false;
    Reference text = buildProofOfNumber(m_walker, operand.value, isText, origin);
    if (!text)
        return false;
    m_walker.count(StatisticID::StringOperandsGuardedInIterations);
    operand.value = text;
    return true;
}

bool CallInWalkBuiltins::settle(Operand& operand, Origin origin)
{
    if (!operand.written)
        operand.value = m_walker.constant(Draft(Opcode::Undefined, origin));
    for (Opcode opcode : conversionOpcodesOf(operand.conversion)) {
        if (!operand.value)
            return false;
        if (opcode == Opcode::ToNumber && m_types.at(m_walker.typeOf(operand.value)).isNumber())
            continue;
        if (opcode == Opcode::ToString && tryGuardStringOperand(operand, origin))
            continue;
        bool isTyped = m_walker.has(Mechanism::TypedOperations);
        if (isTyped && m_walker.isUnreachable())
            return false;
        bool namesBuiltin = Operation::opcodesWithCalledBuiltin.contains(opcode);
        Origin originOfConversion = origin;
        originOfConversion.isTailCall = namesBuiltin && m_origin.isTailCall;
        Draft conversion(opcode, originOfConversion);
        m_walker.chooseConservativeFields(conversion);
        conversion.setOperand(0, operand.value);
        if (namesBuiltin)
            conversion.setCalledBuiltin(m_builtin);
        if (isTyped && hasOperatorForm(opcode))
            conversion.setForm(operatorFormFor(m_walker, conversion));
        operand.value = m_walker.build(WTF::move(conversion));
        if (isTyped && operand.value)
            operand.value = boxNumberIfNeeded(m_walker, operand.value, origin);
    }
    return !!operand.value;
}

static bool isPartOfCollectionDataInWalkBuiltins(const DataNode& node)
{
    if (const auto* number = node.tryAs<DataNode::Number>())
        return !isImpureNaN(number->value);
    return !node.is<DataNode::Hole>();
}

static bool isObjectOfDataInWalkBuiltins(const DataNode& node)
{
    return node.is<DataNode::Array>() || node.is<DataNode::Object>();
}

static bool isUsableAsCollectionDataInWalkBuiltins(const DataNode& root, bool entriesArePairs, bool& holdsObjects)
{
    const auto* list = root.tryAs<DataNode::Array>();
    if (!list || !list->elements.size())
        return false;
    for (const DataNode& entry : list->elements) {
        if (!entriesArePairs) {
            if (!isPartOfCollectionDataInWalkBuiltins(entry))
                return false;
            holdsObjects |= isObjectOfDataInWalkBuiltins(entry);
            continue;
        }
        const auto* pair = entry.tryAs<DataNode::Array>();
        if (!pair || pair->elements.size() != 2 || !isPartOfCollectionDataInWalkBuiltins(pair->elements[0]) || !isPartOfCollectionDataInWalkBuiltins(pair->elements[1]))
            return false;
        holdsObjects |= isObjectOfDataInWalkBuiltins(pair->elements[0]) || isObjectOfDataInWalkBuiltins(pair->elements[1]);
    }
    return true;
}

DataID CallInWalkBuiltins::findDataThatConstructionAloneSees(bool entriesArePairs) const
{
    const BodyIndex& index = m_walker.currentBodyIndex();
    ValueID argument = m_call.as<Opcode::Construct>().argumentsOperands()[0];
    ValueID definition = index.stripCasts(argument);
    if (index.isParameter(definition) || index.opcodeOf(definition) != Opcode::Data)
        return { };
    for (ValueID link = definition; link != argument;) {
        link = index.onlyUsingValue(link);
        if (!link || link.index() > argument.index())
            return { };
    }
    if (index.onlyUsingValue(argument) != m_call.id())
        return { };

    DataID data = index.value(definition).as<Opcode::Data>().data();
    bool holdsObjects = false;
    if (!isUsableAsCollectionDataInWalkBuiltins(m_program.at(data).root, entriesArePairs, holdsObjects))
        return { };
    if (holdsObjects && !tableOfImageHoldsData(m_walker, data))
        return { };
    return !holdsObjects || index.blockOf(definition) == index.blockOf(m_call.id()) ? data : DataID();
}

bool CallInWalkBuiltins::buildCollectionFromData()
{
    if (m_mode != BuiltinCallMode::Construct || m_call.opcode() != Opcode::Construct || m_receiver || m_arguments.size() != 1 || m_passed.size() != 1)
        return false;
    BuiltinClassID made = m_table.at(m_builtin).instanceClass;
    if ((made != BuiltinClasses::Set && made != BuiltinClasses::Map) || !m_table.mayHavePristineInstances(made) || !isArrayIteratorProtocolIntact(m_walker))
        return false;
    bool entriesArePairs = made == BuiltinClasses::Map;
    DataID data = findDataThatConstructionAloneSees(entriesArePairs);
    if (!data)
        return false;

    Draft collection(entriesArePairs ? Opcode::NewMapFromData : Opcode::NewSetFromData, m_origin);
    collection.setData(data);
    collection.expected = m_expected;
    collection.resultIsReturnedAtOnce = m_resultIsReturnedAtOnce;
    collection.inputValueGiven = m_call.id();
    Reference result = m_walker.build(WTF::move(collection));
    if (!result)
        return true;
    m_walker.bind(m_call.id(), result);
    m_walker.state().noteFresh(Reference::value(underlyingCellOf(m_walker.code(), result.id())));
    markObjectPristine(m_walker, result);
    return true;
}

void CallInWalkBuiltins::holdLinksWordAgainstOperands()
{
    Origin origin = m_walker.originOf(m_call);
    if (m_thisValue) {
        if (m_thisValue->heirBehindLook)
            checkStructureOfClassThatIsRemembered(m_walker, m_thisValue->value, m_thisValue->heirBehindLook, origin);
        m_walker.verifyPristineObject(m_thisValue->written);
        if (m_thisValue->staysAsMade)
            holdLinksWordAgainstObjectAsMade(m_walker, m_thisValue->written, origin);
    }
    for (const Operand& operand : m_passed) {
        m_walker.verifyPristineObject(operand.written);
        if (operand.staysAsMade)
            holdLinksWordAgainstObjectAsMade(m_walker, operand.written, origin);
    }
}

StatisticID CallInWalkBuiltins::statisticOfRow() const
{
    BuiltinOverloadID widest;
    uint32_t numberOfRows = 0;
    for (BuiltinOverloadID id : m_table.overloadIDsOf(m_builtin)) {
        if (m_table.at(id).mode != m_mode)
            continue;
        widest = id;
        ++numberOfRows;
    }
    if (numberOfRows == 1)
        return StatisticID::BuiltinCallsOnOnlyRow;
    bool isOneRowReached = m_overload && !m_rowStandsForSeveral && m_rowsThatOneStandsFor.isEmpty();
    return isOneRowReached && m_overloadID != widest ? StatisticID::BuiltinCallsOnNarrowRow : StatisticID::BuiltinCallsOnWideRow;
}

static void countBuiltinCallOfProgram(Walker& walker, StatisticID statistic)
{
    const Program& program = walker.program();
    if (!isLibraryCode(program, program.at(walker.currentFunction())))
        walker.count(statistic);
}

void CallInWalkBuiltins::build()
{
    holdLinksWordAgainstOperands();
    if (m_walker.isUnreachable() || m_walker.hasFailed())
        return;
    if (buildCollectionFromData())
        return;

    Origin originInFrontOfCall = m_origin;
    originInFrontOfCall.isTailCall = false;
    if (m_receiver && !settle(*m_receiver, originInFrontOfCall))
        return;
    Vector<Reference, 8> arguments;
    for (Operand& argument : m_arguments) {
        if (!settle(argument, originInFrontOfCall))
            return;
        arguments.append(argument.value);
    }

    if (m_ownPropertyAskedFor) {
        Reference object = m_overload->receiver ? m_receiver->value : arguments[0];
        if (buildOwnPropertyTest(m_walker, m_call, object, m_ownPropertyAskedFor))
            return;
        Draft test(Opcode::PropHasOwn, originInFrontOfCall);
        m_walker.chooseConservativeFields(test);
        test.setObjectOperand(object);
        test.setKey(m_ownPropertyAskedFor);
        if (m_receiversAskedForOwnProperty) {
            test.setReceivers(m_receiversAskedForOwnProperty);
            m_walker.recordSpeculation();
        }
        test.setCalledBuiltin(m_builtin);
        test.inputValueGiven = m_call.id();
        if (Reference answer = m_walker.build(WTF::move(test)))
            m_walker.bind(m_call.id(), answer);
        return;
    }

    if (m_overload->code.implementation.inlineBuiltin == InlineBuiltin::ArrayIsArray && m_mode == BuiltinCallMode::Call && arguments.size() == 1 && arguments[0]) {
        if (TypeID asked = m_walker.typeOf(arguments[0]); cellAnswersWhetherValueIsArray(m_walker, asked)) {
            TypeID lists = m_types.builtinClass(BuiltinClasses::Array);
            if (!m_types.overlap(asked, lists) || m_types.isSubset(asked, lists)) {
                Draft answer(Opcode::Boolean, originInFrontOfCall);
                answer.setBoolean(m_types.overlap(asked, lists));
                m_walker.bind(m_call.id(), m_walker.constant(WTF::move(answer)));
                return;
            }
            Draft test(Opcode::TypeTest, originInFrontOfCall);
            test.setValueOperand(arguments[0]);
            test.setTestedType(m_types.builtinClass(BuiltinClasses::Array));
            test.inputValueGiven = m_call.id();
            if (Reference answer = m_walker.build(WTF::move(test)))
                m_walker.bind(m_call.id(), answer);
            return;
        }
    }

    if (goesByOneRow() && m_mode == BuiltinCallMode::Call) {
        CallByOneRow onNumbers { .call = m_call, .builtin = m_builtin, .overload = m_overloadID, .codeInLine = m_overload->code.implementation.inlineBuiltin, .receiver = m_receiver ? m_receiver->value : Reference(), .arguments = arguments.span(), .origin = originInFrontOfCall, .isForwarded = !!m_forwarded };
        if (Reference result = buildCallOnNumbers(m_walker, onNumbers)) {
            m_walker.bind(m_call.id(), result);
            return;
        }
        if (m_walker.isUnreachable() || m_walker.hasFailed())
            return;
    }

    if (!m_forwarded && buildWithRegExpWrittenForCall(arguments.mutableSpan()))
        return;

    if (!m_forwarded && (m_form == BuiltinCallForm::Whole || (m_form == BuiltinCallForm::AnyReceiver && m_mode == BuiltinCallMode::Construct))) {
        CallByOneRow byOneRow { m_call, m_table.canonicalID(m_builtin), m_overloadID, m_mode, m_overload->code.implementation.inlineBuiltin, m_receiver ? m_receiver->value : Reference(), arguments.span(), originInFrontOfCall, m_expected };
        if (m_form == BuiltinCallForm::Whole ? buildCallByOneRowForStrings(m_walker, byOneRow) : buildConstructionOnAnyOperandsForStrings(m_walker, byOneRow))
            return;
    }

    if (m_mode == BuiltinCallMode::Call && m_receiver && !isDoneInPlace() && m_builtin == m_table.canonicalID(Builtins::ArrayPrototypeSlice)) {
        bool isListByItsRow = m_form == BuiltinCallForm::Whole && m_overload->code.implementation.inlineBuiltin == InlineBuiltin::ArraySlice;
        if (Reference list = buildSliceOfElementsInSight(m_walker, SliceOfElementsInSight { m_call, m_receiver->value, isListByItsRow, arguments.span(), originInFrontOfCall, m_expected, m_resultIsReturnedAtOnce })) {
            m_walker.bind(m_call.id(), list);
            return;
        }
    }

    if (!m_forwarded && m_mode == BuiltinCallMode::Call && m_form == BuiltinCallForm::Whole && m_receiver && !m_receiver->heirBehindLook && arguments.size() == 1 && m_inlineBuiltin == InlineBuiltin::ArrayAt && !isDoneInPlace()) {
        if (Reference element = buildElementAt(arguments[0], originInFrontOfCall)) {
            m_walker.bind(m_call.id(), element);
            return;
        }
        if (m_walker.isUnreachable() || m_walker.hasFailed())
            return;
    }

    if (!m_forwarded && m_mode == BuiltinCallMode::Call && m_receiver && arguments.size() == 1 && m_builtin == m_table.canonicalID(Builtins::ArrayPrototypeIncludes)) {
        if (Reference answer = buildMembershipInListMadeInSight(m_walker, m_receiver->value, arguments[0], originInFrontOfCall)) {
            m_walker.bind(m_call.id(), answer);
            return;
        }
        if (m_walker.isUnreachable() || m_walker.hasFailed())
            return;
        if (Reference answer = buildMembershipInListOfFile(arguments.span(), originInFrontOfCall)) {
            m_walker.bind(m_call.id(), answer);
            return;
        }
        if (m_walker.isUnreachable() || m_walker.hasFailed())
            return;
    }

    if (!m_forwarded && joinOfWhatIsInSight(m_walker, m_call, m_builtin))
        return;
    Vector<Reference, 4> staying = freshOperandsThatStayHere();
    Vector<Reference, 4> stayingAsMade = operandsThatStayAsMade();
    Reference result = m_inlineForm == InlineForm::TwoWay ? buildCodeInLineOrCall(arguments.span()) : buildCall(arguments.span());
    if (!result)
        return;
    for (Reference object : staying)
        m_walker.state().noteFresh(object);
    for (Reference object : stayingAsMade)
        markObjectPristine(m_walker, object);
    if (m_mode == BuiltinCallMode::Set)
        return;
    m_walker.bind(m_call.id(), result);
    bool isInstanceMadeByCall = m_mode == BuiltinCallMode::Call && result.isValue() && classWhoseInstancesAreTestedInWalkBuiltins(m_walker, m_walker.typeOf(result)) && !isDoneInPlace()
        && everyRowThatCallMayTake([&](const BuiltinOverload& row) {
            if (mayRunCodeThatNoOperandNames(row) || !row.result.type || m_table.at(row.result.type).freshness != BuiltinFreshness::Yes)
                return false;
            return std::ranges::all_of(m_table.at(row.result.type).parts, [&](const BuiltinTypePart& part) {
                return part.isOfClassItself || !part.builtinClass || !classBelowWithTestedStructure(m_table, part.builtinClass);
            });
        });
    if (m_inlineForm != InlineForm::TwoWay && (makesFreshResult(result) || isInstanceMadeByCall)) {
        m_walker.state().noteFresh(Reference::value(underlyingCellOf(m_walker.code(), result.id())));
        bool bearsNothingOfItsOwn = everyRowThatCallMayTake([](const BuiltinOverload& row) {
            return !row.result.prototypeSource && row.result.properties.empty();
        });
        if (((m_mode == BuiltinCallMode::Construct || isInstanceMadeByCall) && bearsNothingOfItsOwn) || m_walker.code().at(result.id()).opcode == Opcode::NewRegExp)
            markObjectPristine(m_walker, result);
    }
}

bool CallInWalkBuiltins::buildWithRegExpWrittenForCall(std::span<Reference> arguments)
{
    if (m_call.opcode() != Opcode::Call || m_form != BuiltinCallForm::Whole)
        return false;
    Origin origin = m_origin;
    origin.isTailCall = false;

    auto take = [&](BuiltinOperand operand, ValueID written, Reference& object, Reference string) {
        std::optional<RegExpWrittenForCall> regExp = findRegExpWrittenForCall(m_walker, m_call, written);
        if (!regExp)
            return false;
        if (Reference shared = buildRegExpSharedForCall(m_walker, m_overloadID, operand, *regExp))
            object = shared;
        if (arguments.size() == 1 && buildMatchInPartsByCodeOfRegExp(m_walker, m_call, m_overloadID, *regExp, object, string, origin))
            return true;
        Reference match = arguments.size() == 1 ? buildMatchByCodeOfRegExp(m_walker, m_call, m_overloadID, *regExp, object, string, origin, m_expected) : Reference();
        if (match)
            m_walker.bind(m_call.id(), match);
        return !!match || m_walker.hasFailed();
    };
    if (m_receiver && m_receiver->written && take(BuiltinOperand::receiver(), m_call.thisValueOperand(), m_receiver->value, arguments.empty() ? Reference() : arguments[0]))
        return true;
    std::span<const ValueID> written = m_call.argumentsOperands();
    return !arguments.empty() && !written.empty() && m_arguments[0].written && take(BuiltinOperand::argument(0), written[0], arguments[0], m_receiver ? m_receiver->value : Reference());
}

std::optional<HeldRegExp> CallInWalkBuiltins::findRegExpMatchedByCall() const
{
    if (m_call.opcode() != Opcode::Call)
        return std::nullopt;
    auto find = [&](ValueID operand) -> std::optional<HeldRegExp> {
        if (std::optional<RegExpWrittenForCall> written = findRegExpWrittenForCall(m_walker, m_call, operand))
            return HeldRegExp { written->pattern, written->flags };
        return findHeldRegExp(m_walker, operand);
    };
    switch (*m_inlineBuiltin) {
    case InlineBuiltin::RegExpTest:
    case InlineBuiltin::RegExpExec:
        return m_receiver && m_receiver->written ? find(m_call.thisValueOperand()) : std::nullopt;
    case InlineBuiltin::StringMatch:
    case InlineBuiltin::StringSearch: {
        std::span<const ValueID> arguments = m_call.argumentsOperands();
        std::optional<HeldRegExp> regExp = !arguments.empty() && !m_arguments.isEmpty() && m_arguments[0].written ? find(arguments[0]) : std::nullopt;
        const bool listsMatches = *m_inlineBuiltin == InlineBuiltin::StringMatch && regExp && regExp->flags.contains(RegExpFlags::Global);
        return listsMatches ? std::nullopt : regExp;
    }
    default:
        return std::nullopt;
    }
}

template<typename CallDraft>
void CallInWalkBuiltins::fillCodeInLine(CallDraft& call, std::span<const Reference> arguments) const
{
    Vector<uint32_t, 4> parameters;
    for (uint8_t parameter : std::span(m_overload->code.implementation.inlineParameters).first(numberOfParametersOf(*m_inlineBuiltin)))
        parameters.append(parameter);
    if (*m_inlineBuiltin == InlineBuiltin::ObjectCreate && m_call.opcode() == Opcode::Call && arguments.size() == 1 && m_call.argumentsOperands().size() == 2)
        parameters.append(objectCreateDescriptorCount(m_walker, m_call));
    if (std::optional<HeldRegExp> regExp = findRegExpMatchedByCall()) {
        parameters.append(regExp->pattern.bits());
        parameters.append(regExp->flags.toRaw());
    }
    call.setInlineBuiltin(*m_inlineBuiltin);
    call.setInlineParameters(parameters.span());
    call.setOverload(m_overloadID);
    if (m_receiver && m_overload->receiver)
        call.setReceiverOperand(m_receiver->value);
    call.setArgumentsOperands(arguments);
}

static void noteLengthOfViewMadeWithLengthInWalkBuiltins(Walker& walker, Reference view, std::span<const Reference> arguments)
{
    if (!view || !view.isValue() || arguments.size() != 1 || !arguments[0] || !walker.has(Mechanism::TypedOperations))
        return;
    const Operation& made = walker.code().at(view.id());
    if (made.opcode != Opcode::CallInline || made.inlineBuiltin() != InlineBuiltin::TypedArrayWithLength)
        return;
    Type length = walker.types().at(walker.typeOf(arguments[0]));
    if (!length.isInteger() || length.integers.minimum < 0)
        return;
    noteKnown(walker, KnownNote::LengthOfList, view, { }, 0, LocationMasks(AbstractHeapSet { AbstractHeap::TypedArrayStorage }), arguments[0].bits());
}

static bool answerHangsOnTextAndNumbersAloneInWalkBuiltins(InlineBuiltin builtin)
{
    switch (builtin) {
    case InlineBuiltin::StringToLowerCase:
    case InlineBuiltin::StringToUpperCase:
    case InlineBuiltin::StringTrim:
    case InlineBuiltin::StringTrimStart:
    case InlineBuiltin::StringTrimEnd:
    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr:
        return true;
    default:
        return false;
    }
}

static Reference findSameStoreIntoTableBuiltLastInWalkBuiltins(Walker& walker, InlineBuiltin builtin, Reference table, std::span<const Reference> arguments)
{
    const Code& code = walker.code();
    auto inFrontOfConversions = [&](ValueID value) {
        while (value && !code.isConstant(value) && conversionsOfForm.contains(code.at(value).opcode))
            value = code.at(value).operand(0);
        return value;
    };
    auto isSame = [&](ValueID built, Reference asked) {
        return asked && asked.isRecord() && inFrontOfConversions(built) == inFrontOfConversions(asked.id());
    };
    for (ValueID id = code.lastOperationOf(walker.currentBlock()); id; id = code.at(id).previous) {
        const Operation& operation = code.at(id);
        if (operation.traits().isUnobservable())
            continue;
        if (operation.opcode != Opcode::CallInline || operation.inlineBuiltin() != builtin || !isSame(operation.receiverOperand(), table))
            return { };
        std::span<const ValueID> stored = operation.argumentsOperands();
        if (stored.size() != arguments.size())
            return { };
        for (size_t position = 0; position < stored.size(); ++position) {
            if (!isSame(stored[position], arguments[position]))
                return { };
        }
        return Reference::value(id);
    }
    return { };
}

TypeID CallInWalkBuiltins::provenContentsOfReceiver() const
{
    if (!m_receiver || m_receiver->heirBehindLook || m_mode != BuiltinCallMode::Call)
        return { };
    bool returnsContents = std::ranges::any_of(std::array { BuiltinID(Builtins::MapPrototypeGet), BuiltinID(Builtins::WeakMapPrototypeGet), BuiltinID(Builtins::ArrayPrototypePop), BuiltinID(Builtins::ArrayPrototypeShift), BuiltinID(Builtins::ArrayPrototypeAt) }, [&](BuiltinID method) {
        return m_builtin == m_table.canonicalID(method);
    });
    const LinkedContentsAtOperation* contents = returnsContents ? m_walker.facts().linked().contentsAtOperation(m_walker.currentFunction(), m_call.id()) : nullptr;
    bool isKnown = contents && contents->value;
    return isKnown ? contents->value : TypeID();
}

bool CallInWalkBuiltins::isOnlyCutToInt32(TypeID contents) const
{
    Type type = m_types.at(contents);
    if (!m_walker.isInLoop() || !m_walker.opensGuesses() || !(type.primitives & Primitive::Number) || !type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull))
        return false;
    const BodyIndex& index = m_walker.currentBodyIndex();
    ValueID user = index.onlyUsingValue(m_call.id());
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user)))
        user = index.onlyUsingValue(user);
    return user && allBitwiseOperators.contains(index.opcodeOf(user));
}

Reference CallInWalkBuiltins::buildCall(std::span<const Reference> arguments)
{
    if (m_form == BuiltinCallForm::Whole && m_mode != BuiltinCallMode::Get && !m_receiver && m_builtin == m_table.canonicalID(Builtins::RegExp)) {
        Origin origin = m_origin;
        origin.isTailCall = false;
        if (Reference literal = buildRegExpOfKnownTexts(m_walker, arguments.empty() ? Reference() : arguments[0], arguments.size() > 1 ? arguments[1] : Reference(), origin))
            return literal;
    }
    if (m_form == BuiltinCallForm::Whole && m_mode != BuiltinCallMode::Get && !m_receiver && !isDoneInPlace() && arguments.size() == 1 && m_builtin == m_table.canonicalID(Builtins::Array)
        && m_walker.has(Mechanism::TypedOperations) && m_types.at(m_walker.typeOf(arguments[0])).isInteger()) {
        Origin origin = m_origin;
        origin.isTailCall = false;
        Draft list(Opcode::NewArrayLike, origin);
        m_walker.chooseConservativeFields(list);
        list.setLengthOperand(arguments[0]);
        const LinkedContentsAtOperation* stored = m_walker.facts().linked().contentsAtOperation(m_walker.currentFunction(), m_call.id());
        ArrayStorage holding = stored ? arrayStorageHolding(m_types, stored->value) : ArrayStorage::Other;
        bool holdsWholeNumbersAlone = holding == ArrayStorage::Double && !(m_types.at(stored->value).primitives & Primitive::FractionalNumber);
        list.setArrayStorage(holdsWholeNumbersAlone ? ArrayStorage::Int32 : holding);
        list.setRemembersStorageAtSite(!stored || holdsWholeNumbersAlone);
        list.expected = m_expected;
        list.resultIsReturnedAtOnce = m_resultIsReturnedAtOnce;
        list.inputValueGiven = m_call.id();
        Reference made = m_walker.build(WTF::move(list));
        noteFreshList(m_walker, made);
        return made;
    }
    if (!m_forwarded && (m_mode == BuiltinCallMode::Call || m_mode == BuiltinCallMode::Construct) && !isDoneInPlace() && arguments.size() == 1 && arguments[0]
        && m_builtin == m_table.canonicalID(Builtins::Array) && m_walker.has(Mechanism::TypedOperations) && m_types.overlap(m_walker.typeOf(arguments[0]), TypeTable::int32)) {
        Origin origin = m_origin;
        origin.isTailCall = false;
        Draft list(Opcode::NewArrayOfGiven, origin);
        m_walker.chooseConservativeFields(list);
        list.setGivenOperand(arguments[0]);
        list.expected = m_expected;
        list.resultIsReturnedAtOnce = m_resultIsReturnedAtOnce;
        list.inputValueGiven = m_call.id();
        m_walker.count(StatisticID::SingleArgumentArrayCallsInlined);
        return m_walker.build(WTF::move(list));
    }
    bool listsElementsByArrayOf = m_mode == BuiltinCallMode::Call && m_rowsThatOneStandsFor.isEmpty() && m_call.opcode() == Opcode::Call && m_builtin == m_table.canonicalID(Builtins::ArrayOf) && m_overloadID == *m_table.overloadIDsOf(m_builtin).begin();
    bool listsElementsByArray = m_mode != BuiltinCallMode::Get && !m_receiver && m_builtin == m_table.canonicalID(Builtins::Array) && arguments.size() >= 2;
    if (!m_forwarded && m_form == BuiltinCallForm::Whole && !isDoneInPlace() && (listsElementsByArrayOf || listsElementsByArray) && arguments.size() <= maxComputedElementsOfArrayLiteral && m_walker.has(Mechanism::TypedOperations)) {
        Origin origin = m_origin;
        origin.isTailCall = false;
        Draft literal(Opcode::NewArray, origin);
        m_walker.chooseConservativeFields(literal);
        literal.setContents(TypeTable::anything);
        literal.setVariadicOperands(arguments);
        literal.expected = m_expected;
        literal.resultIsReturnedAtOnce = m_resultIsReturnedAtOnce;
        literal.inputValueGiven = m_call.id();
        Reference made = m_walker.build(WTF::move(literal));
        noteFreshList(m_walker, made);
        return made;
    }
    bool isCodeInLine = m_inlineForm == InlineForm::Whole;
    if (isCodeInLine && m_mode == BuiltinCallMode::Call && m_receiver && !m_walker.isUnreachable() && (m_inlineBuiltin == InlineBuiltin::MapSet || m_inlineBuiltin == InlineBuiltin::SetAdd)) {
        if (Reference same = findSameStoreIntoTableBuiltLastInWalkBuiltins(m_walker, *m_inlineBuiltin, m_receiver->value, arguments)) {
            m_walker.count(StatisticID::DuplicateCollectionStoresRemoved);
            return same;
        }
    }
    Draft call(isCodeInLine ? Opcode::CallInline : Opcode::CallBuiltin, m_origin);
    m_walker.chooseConservativeFields(call);
    if (isCodeInLine)
        fillCodeInLine(call, arguments);
    else {
        call.setBuiltinCallForm(m_form);
        call.setOverload(m_overloadID);
        if (m_receiver)
            call.setReceiverOperand(m_receiver->value);
        call.setArgumentsOperands(arguments);
    }
    call.setIsDoneInPlace(isDoneInPlace());
    call.expected = m_expected;
    call.resultIsReturnedAtOnce = m_resultIsReturnedAtOnce;
    if (m_mode != BuiltinCallMode::Set)
        call.inputValueGiven = m_call.id();
    TypeID before = m_walker.exchangeContentsBeingTyped(provenContentsOfReceiver());
    bool looksUpInTable = isCodeInLine && m_receiver && arguments.size() == 1 && isLookupInTable(*m_inlineBuiltin);
    std::array<Reference, 2> searched { looksUpInTable ? m_receiver->value : Reference(), looksUpInTable ? arguments[0] : Reference() };
    std::optional<uint32_t> loop = looksUpInTable ? m_walker.outermostLoopToLookUpInFrontOf(searched, m_effects) : std::nullopt;
    std::optional<Draft> lookedUpAgain = looksUpInTable && m_walker.checksProofs() ? std::optional<Draft>(call) : std::nullopt;
    auto isLoopInvariant = [&](Reference operand) {
        return m_walker.isInvariantInSpecializedVersion(operand);
    };
    bool answerHangsOnOperandsAlone = m_inlineBuiltin && ((answerHangsOnTextAndNumbersAloneInWalkBuiltins(*m_inlineBuiltin) && !m_effects.mayThrowByRule() && !m_effects.mayRunOtherCode()) || m_walker.shouldAssumeInlineBuiltinsPure());
    bool isCalledInFront = !loop && isCodeInLine && m_receiver && answerHangsOnOperandsAlone
        && isLoopInvariant(m_receiver->value) && std::ranges::all_of(arguments, isLoopInvariant) && m_walker.tryPreparePreheaderForCall();
    std::optional<Draft> calledAgain = isCalledInFront && m_walker.checksHoistedCalls() ? std::optional<Draft>(call) : std::nullopt;
    Reference built;
    bool isStringCallOnInvariants = !loop && !isCalledInFront && isCodeInLine && m_receiver && m_mode == BuiltinCallMode::Call && answerHangsOnOperandsAlone
        && m_walker.isInvariantInInnermostLoop(m_receiver->value) && std::ranges::all_of(arguments, [&](Reference argument) { return m_walker.isInvariantInInnermostLoop(argument); });
    if (isStringCallOnInvariants || (looksUpInTable && !loop && m_mode == BuiltinCallMode::Call && m_walker.isLookupLoopInvariant(searched, m_effects))) {
        built = m_walker.buildOnFirstArrival(m_call.id(), call, m_effects);
        if (m_walker.hasFailed())
            return { };
        if (built && !lookedUpAgain && m_walker.checksHoistedCalls())
            calledAgain = call;
    }
    if (!built) {
        Walker::SpecializedPreheaderScope preheaderScope(m_walker, isCalledInFront);
        uint32_t numberOfRecordsInFront = m_walker.code().numberOfValues();
        built = loop ? m_walker.buildInFrontOfOpenLoop(*loop, WTF::move(call), &m_effects) : m_walker.build(WTF::move(call), &m_effects);
        if (!loop && m_walker.wasBuiltInPlaceSince(numberOfRecordsInFront, built))
            lookedUpAgain = std::nullopt;
    }
    if (lookedUpAgain)
        m_walker.holdLookupInFrontAgainstLookupInTurn(built, WTF::move(*lookedUpAgain), m_effects);
    if (calledAgain)
        m_walker.holdLookupInFrontAgainstLookupInTurn(built, WTF::move(*calledAgain), m_effects, ProofSource::HoistedCall);
    if (loop && built)
        m_walker.count(StatisticID::CollectionLookupsHoisted);
    m_walker.exchangeContentsBeingTyped(before);
    if (isCodeInLine)
        noteLengthOfViewMadeWithLengthInWalkBuiltins(m_walker, built, arguments);
    if (looksUpInTable && m_inlineBuiltin == InlineBuiltin::MapGet && m_mode == BuiltinCallMode::Call)
        return guardNumericCallResult(m_walker, m_call, built, m_origin);
    return built;
}

Reference CallInWalkBuiltins::buildComparisonByContents(std::span<const Reference> arguments)
{
    Origin origin = m_origin;
    origin.isTailCall = false;
    Draft comparison(Opcode::StrictEq, origin);
    m_walker.chooseConservativeFields(comparison);
    comparison.setLeftOperand(arguments[0]);
    comparison.setRightOperand(arguments[1]);
    return m_walker.build(WTF::move(comparison));
}

Reference CallInWalkBuiltins::buildMembershipInListOfFile(std::span<const Reference> arguments, Origin origin)
{
    Reference list = m_receiver->value;
    if (!m_walker.isInRepeatedCode() || m_call.opcode() != Opcode::Call || !list || !list.isValue() || !m_types.isSubset(m_walker.typeOf(list), m_types.builtinClass(BuiltinClasses::Array)))
        return { };
    WrittenListOfVariable written = writtenListReadBy(m_walker, m_call.thisValueOperand());
    const auto* elements = written && written.length <= mostElementsComparedInLoop ? m_walker.program().at(written.data).root.tryAs<DataNode::Array>() : nullptr;
    bool areCompared = elements && std::ranges::all_of(elements->elements, [](const DataNode& element) {
        const auto* number = element.tryAs<DataNode::Number>();
        return number ? number->value == number->value : element.is<DataNode::String>();
    });
    if (!areCompared)
        return { };
    Draft isAsWritten(Opcode::HasArrayStorage, origin);
    m_walker.chooseConservativeFields(isAsWritten);
    isAsWritten.setArrayOperand(list);
    isAsWritten.setArrayStorage(written.storage);
    isAsWritten.setData(written.data);
    Reference test = m_walker.build(WTF::move(isAsWritten));
    if (!test || m_walker.isUnreachable() || m_walker.hasFailed())
        return { };
    BlockID inPlace = m_walker.addBlock();
    BlockID byCall = m_walker.addBlock(BlockFrequency::Unlikely);
    m_walker.branch(test, inPlace, byCall);
    Vector<Walker::JoinArm, 2> arms;
    m_walker.continueIn(inPlace);
    if (Reference found = buildMembershipInWrittenList(m_walker, written.data, arguments[0], origin); found && !m_walker.isUnreachable())
        arms.append({ m_walker.currentBlock(), { found } });
    if (m_walker.hasFailed())
        return { };
    m_walker.continueIn(byCall);
    Reference called = m_walker.isUnreachable() ? Reference() : m_inlineForm == InlineForm::TwoWay ? buildCodeInLineOrCall(arguments) : buildCall(arguments);
    if (called && !m_walker.isUnreachable())
        arms.append({ m_walker.currentBlock(), { called } });
    if (m_walker.hasFailed() || arms.isEmpty())
        return { };
    Reference met;
    return m_walker.joinArms(arms.span(), m_walker.addBlock(), std::span<Reference>(&met, 1)) ? met : Reference();
}

Reference CallInWalkBuiltins::buildElementInside(Reference position, Origin origin)
{
    Reference list = m_receiver->value;
    TypeID contents = provenContentsOfReceiver();
    std::optional<PassedTest> insideLength = m_walker.findWhatHoldsPositionInsideList(list, position);
    std::optional<HoistedArrayGuard> tested = findHoistedArrayGuard(m_walker, list);
    if (!tested)
        tested = tryHoistArrayGuardForCall(m_walker, m_call, list, contents);
    Draft read(Opcode::GetArrayElement, origin);
    m_walker.chooseConservativeFields(read);
    read.inputValueGiven = m_call.id();
    read.expected = m_expected;
    read.setArrayOperand(list);
    read.setPositionOperand(position);
    read.setLevel(KnowledgeLevel::Proven);
    read.setKeyIsIntegral(true);
    if (tested)
        nameKindOfListTestedInFrontOfLoop(read, *tested);
    if (insideLength)
        read.setIsInBounds(*insideLength);
    if (tested)
        takeStorageAndLengthFromInFrontOfLoop(m_walker, read, *tested);
    TypeID before = m_walker.exchangeResultOfReadBeingTyped(contents ? m_types.withPrimitives(contents, Primitive::Undefined) : TypeID());
    Reference element = m_walker.build(WTF::move(read));
    m_walker.exchangeResultOfReadBeingTyped(before);
    return element;
}

Reference CallInWalkBuiltins::buildElementAt(Reference index, Origin origin)
{
    Reference list = m_receiver->value;
    if (!list || !list.isValue() || !index || !m_types.isSubset(m_walker.typeOf(list), m_types.builtinClass(BuiltinClasses::Array)) || !m_walker.has(Mechanism::TypedOperations))
        return { };
    Type typeOfIndex = m_types.at(m_walker.typeOf(index));
    bool isWritten = index.isConstant() && m_walker.code().at(index.id()).opcode == Opcode::Integer;
    int64_t written = isWritten ? m_walker.code().at(index.id()).integer() : 0;
    bool isFromEnd = isWritten && written < 0 && written >= std::numeric_limits<int32_t>::min();
    if (!isFromEnd && (!typeOfIndex.isInteger() || typeOfIndex.integers.minimum < 0 || typeOfIndex.integers.minimum > std::numeric_limits<int32_t>::max()))
        return { };
    auto integer = [&](int64_t value) {
        Draft constant(Opcode::Integer, origin);
        constant.setInteger(value);
        return m_walker.constant(WTF::move(constant));
    };
    Draft measured(Opcode::GetArrayLength, origin);
    m_walker.chooseConservativeFields(measured);
    measured.setArrayOperand(list);
    Reference length = m_walker.build(WTF::move(measured));
    Reference isInside = !length ? Reference() : isFromEnd ? buildIntegerOperator(m_walker, Opcode::Greater, length, integer(-written - 1), origin) : buildIntegerOperator(m_walker, Opcode::Less, index, length, origin);
    if (!isInside || !isInside.isValue() || m_walker.isUnreachable() || m_walker.hasFailed())
        return { };
    BlockID inside = m_walker.addBlock();
    BlockID outside = m_walker.addBlock();
    m_walker.branch(isInside, inside, outside);
    m_walker.continueIn(inside);
    Vector<Walker::JoinArm, 2> arms;
    if (!m_walker.isUnreachable()) {
        auto narrowed = [&](Reference value, uint32_t operandIndex) {
            if (!value.isValue())
                return value;
            Draft proof(Opcode::Narrowed, origin);
            m_walker.chooseConservativeFields(proof);
            proof.setValueOperand(value);
            proof.setTestOperand(isInside);
            proof.setOperandIndex(operandIndex);
            proof.setIsOnTakenEdge(true);
            return m_walker.build(WTF::move(proof));
        };
        Reference position = isFromEnd ? narrowed(length, OperandIndex::Greater::left) : narrowed(index, OperandIndex::Less::left);
        if (position && isFromEnd)
            position = buildIntegerOperator(m_walker, Opcode::Sub, position, integer(-written), origin);
        Reference element = position && !m_walker.isUnreachable() ? buildElementInside(position, origin) : Reference();
        if (!element && !m_walker.hasFailed())
            m_walker.endAsUnreachable(origin);
        if (element && !m_walker.isUnreachable())
            arms.append({ m_walker.currentBlock(), { element } });
        if (m_walker.hasFailed())
            return { };
    }
    m_walker.continueIn(outside);
    if (!m_walker.isUnreachable())
        arms.append({ m_walker.currentBlock(), { m_walker.constant(Draft(Opcode::Undefined, origin)) } });
    if (m_walker.hasFailed() || arms.isEmpty())
        return { };
    Reference met;
    return m_walker.joinArms(arms.span(), m_walker.addBlock(), std::span<Reference>(&met, 1)) ? met : Reference();
}

Reference CallInWalkBuiltins::buildCodeInLineOrCall(std::span<const Reference> arguments)
{
    Origin origin = m_origin;
    origin.isTailCall = false;
    TerminatorDraft attempt(TerminatorKind::TryCallInline, m_origin);
    fillCodeInLine(attempt, arguments);

    BlockID done = m_walker.addBlock();
    BlockID miss = m_walker.addBlock(BlockFrequency::Rare);
    BlockID join = m_walker.addBlock();
    TypeID before = m_walker.exchangeContentsBeingTyped(provenContentsOfReceiver());
    Reference answered = m_walker.tryCallInline(WTF::move(attempt), done, miss, &m_effectsOfAttempt);
    bool isTypedByContents = !!m_walker.exchangeContentsBeingTyped(before);
    if (!answered)
        return { };

    m_walker.continueIn(miss);
    Reference called = m_inlineBuiltin == InlineBuiltin::ObjectIs ? buildComparisonByContents(arguments) : buildCall(arguments);
    if (m_walker.hasFailed())
        return { };
    BlockID endOfMiss = called ? m_walker.currentBlock() : BlockID();

    m_walker.continueIn(done);
    if (m_walker.checksProofs() && answered.isValue())
        m_walker.checkProof(answered, TypeTable::anything, ProofSource::ResultOfOtherCall, origin);
    else if (isTypedByContents && answered.isValue() && !m_types.at(m_walker.typeOf(answered)).isAnything())
        m_walker.buildCheckOfProof(answered, m_walker.typeOf(answered), TypeTable::anything, ProofSource::Contents, origin, true);
    if (hostReadsMembersFirstIn(m_table, *m_overload)) {
        for (BuiltinID location : { BuiltinID(Builtins::DatePrototypeValueOf), BuiltinID(Builtins::DatePrototypeToISOString) }) {
            const Builtin& entry = m_table.at(location);
            if (!isMadeWithCodeOfLibrary(m_table, location) && !(entry.isAddedAtFirstRead && entry.holder && m_walker.facts().listsKeysOf(m_table.canonicalID(entry.holder))))
                continue;
            Draft initialization(opcodeOfFirstReadOffHolder(m_table, location), origin);
            m_walker.chooseConservativeFields(initialization);
            initialization.setBuiltin(location);
            m_walker.build(WTF::move(initialization));
        }
    }
    Vector<Walker::JoinArm, 2> arms { Walker::JoinArm { m_walker.currentBlock(), { answered } } };
    if (endOfMiss)
        arms.append(Walker::JoinArm { endOfMiss, { called } });
    Reference result;
    return m_walker.joinArms(arms.span(), join, singleElementSpan(result)) ? result : Reference();
}

} // anonymous namespace

static bool isValueComputedOnceByHostInWalkBuiltins(const Builtin& member)
{
    return member.kind == BuiltinKind::DataProperty && member.scope == BuiltinScope::Realm && member.mutability != BuiltinMutability::ChangesOnItsOwn
        && member.flags.contains(BuiltinFlag::IsLazilyCreated) && !member.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
}

static bool buildReadOfWatchedValueInWalkBuiltins(Walker& walker, const BodyValue& read, TypeID typeOfObject, Reference object)
{
    if (!walker.has(Mechanism::BuiltinCalls))
        return false;
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    Type type = walker.types().at(typeOfObject);
    PropertyKeyID key = read.key();
    BuiltinID holder = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    BuiltinName name = holder && !liesInThread(key) ? program.builtinNameOf(key) : BuiltinName();
    if (!name || compiledModuleWithObject(program, table.canonicalID(holder)) || table.at(table.rootOf(holder)).scope != BuiltinScope::Realm)
        return false;
    BuiltinID member = table.findOwnMember(table.canonicalID(holder), name);
    if (!member || table.canonicalID(member) != member || !isValueComputedOnceByHostInWalkBuiltins(table.at(member)) || !BuiltinCellKey::value(member).hasCell(table) || table.has(BuiltinMark::MayBeAbsent, member))
        return false;
    std::span<const BuiltinLeftOpen> open = walker.facts().builtinMembersLeftOpen();
    auto found = std::ranges::find(open, member, &BuiltinLeftOpen::member);
    if (found == open.end() || found->reason == ReasonBuiltinStaysOpen::IsWrittenUnderItsName || found->reason == ReasonBuiltinStaysOpen::IsLeftOpenForTests)
        return false;
    if (!object) {
        walker.noteFormAskedAhead(Opcode::GetWatchedBuiltinValue, KnowledgeLevel::Proven);
        return true;
    }
    if (walker.has(Mechanism::Guesses) && walker.opensGuesses() && walker.canBuildIterationGuard() && walker.canRestartIteration() && !walker.hasUninlinedCallInIterations() && walker.canHoistNamedLookup(key)) {
        Origin origin = walker.originOf(read);
        origin.isTailCall = false;
        Reference found;
        Reference test;
        {
            Walker::SpecializedPreheaderScope preheaderScope(walker, true, Walker::IterationPurity::KnownEffectsOnly);
            Draft lookup(Opcode::GetWatchedBuiltinValueOrAbsent, origin);
            lookup.setBuiltin(member);
            lookup.setValueType(TypeTable::anythingOrAbsent);
            found = walker.build(WTF::move(lookup));
            if (!found || walker.hasFailed())
                return true;
            Draft question(Opcode::TypeTest, origin);
            question.setValueOperand(found);
            question.setTestedType(TypeArena::fromPrimitives(Primitive::Absent));
            test = walker.build(WTF::move(question));
            if (!test || walker.hasFailed())
                return true;
        }
        if (walker.tryBranchInPreheader(test, false, Walker::IterationPurity::KnownEffectsOnly)) {
            walker.recordSpeculation();
            Walker::SpecializedPreheaderScope preheaderScope(walker, true, Walker::IterationPurity::KnownEffectsOnly);
            Draft proof(Opcode::Narrowed, origin);
            proof.setValueOperand(found);
            proof.setTestOperand(test);
            proof.setOperandIndex(OperandIndex::TypeTest::value);
            proof.setIsOnTakenEdge(false);
            walker.bind(read.id(), walker.build(WTF::move(proof)));
            return true;
        }
    }
    Draft value(Opcode::GetWatchedBuiltinValue, walker.originOf(read));
    walker.chooseConservativeFields(value);
    value.setBuiltin(member);
    value.inputValueGiven = read.id();
    walker.bind(read.id(), walker.build(WTF::move(value)));
    return true;
}

static std::optional<Effects> effectsOfValueComputedOnceInWalkBuiltins(const BuiltinTable& table, BuiltinOverloadID id)
{
    const BuiltinOverload& row = table.at(id);
    if (row.isUndeclared() || row.libraryFunction() || row.effects.capability == BuiltinCapability::JS || !row.flow.handlerCalls.empty() || !row.throws())
        return std::nullopt;
    if (table.at(row.builtin).flags.containsAny({ BuiltinFlag::EvaluatesText, BuiltinFlag::LoadsModuleByName, BuiltinFlag::RunsUnseenCode }))
        return std::nullopt;
    if (table.has(BuiltinMark::RunsWaitingJobs, row.builtin) || table.has(BuiltinMark::MayTerminateProcess, row.builtin))
        return std::nullopt;
    BuiltinID member = table.canonicalID(row.builtin);
    bool readCallsFunction = std::ranges::any_of(table.calledFunctionsOf(member), [&](const BuiltinCalledFunction& called) {
        return (!called.condition || !called.condition->objectNobodyKnowsIsWritten) && (!table.contains(called.function) || table.at(called.function).platforms.contains(targetOfThisBuild().platform));
    });
    if (readCallsFunction || !table.moduleLoadsOf(member).empty())
        return std::nullopt;
    Effects effects = Effects::none();
    effects.reads.add(row.effects.reads);
    effects.writes.add(row.effects.writes);
    effects.merge(declaredBoundOf(NamedEffectSet::Allocates));
    effects.merge(declaredBoundOf(NamedEffectSet::UsesRuntime));
    effects.mayThrow = true;
    return effects;
}

static bool buildReadByRowOfMemberInWalkBuiltins(Walker& walker, const BodyValue& read, TypeID typeOfObject, Reference object, BuiltinID location)
{
    if (!walker.has(Mechanism::BuiltinCalls))
        return false;
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID member = table.canonicalID(location);
    if (isValueComputedOnceByHostInWalkBuiltins(table.at(member))) {
        BuiltinOverloadID row;
        for (BuiltinOverloadID id : table.overloadIDsOf(member)) {
            if (!row && table.at(id).mode == BuiltinCallMode::Get)
                row = id;
        }
        if (!row || !BuiltinCellKey::value(member).hasCell(table))
            return false;
        if (!object)
            return true;
        Draft value(Opcode::GetBuiltinValue, walker.originOf(read));
        walker.chooseConservativeFields(value);
        value.setBuiltin(member);
        value.setOverload(row);
        value.inputValueGiven = read.id();
        std::optional<Effects> byRow = effectsOfValueComputedOnceInWalkBuiltins(table, row);
        walker.bind(read.id(), walker.build(WTF::move(value), byRow ? &*byRow : nullptr));
        return true;
    }

    ValueID receiver = read.objectOperand();
    auto typeOfInput = [&](ValueID input) {
        return input == receiver ? typeOfObject : TypeID();
    };
    TypesOfInputs typesAskedAbout(typeOfInput);
    CallInWalkBuiltins call(walker, read, object ? nullptr : &typesAskedAbout, location);
    if (!call.plan() || !call.goesByOneRow())
        return false;
    if (object)
        call.holdLinksWordAgainstOperands();
    if (object && !buildTypedElementAccess(walker, read))
        call.build();
    return true;
}

Reference buildLazyBuiltinValue(Walker& walker, BuiltinID builtin, BuiltinOverloadID overload, Origin origin)
{
    const BuiltinTable& table = walker.program().builtins();
    origin.isTailCall = false;
    Draft value(Opcode::GetBuiltinValue, origin);
    walker.chooseConservativeFields(value);
    value.setBuiltin(builtin);
    value.setOverload(overload);
    std::optional<Effects> byRow = effectsOfValueComputedOnceInWalkBuiltins(table, overload);
    return byRow && !byRow->mayRunOtherCode() && !byRow->mayBeTerminated ? walker.build(WTF::move(value), &*byRow) : Reference();
}

TypedForm buildStoreThroughSetterOfBuiltin(Walker& walker, const BodyValue& store, TypeID typeOfObject, TypeID typeOfStored, Reference object)
{
    if (!walker.has(Mechanism::BuiltinCalls) || !typeOfObject || !typeOfStored || walker.types().at(typeOfObject).primitives & Primitive::UndefinedOrNull)
        return TypedForm::None;
    PropertyKeyID key = store.key();
    BuiltinID location = object ? findConstantMemberReadOff(walker, object, key) : BuiltinID();
    TypedForm formAskedAbout = TypedForm::Built;
    if (!object && classWhoseInstancesAreTestedInWalkBuiltins(walker, typeOfObject)) {
        location = findMemberReadOffInWalkBuiltins(walker, typeOfObject, key, true, MembersBehindWatch::AreAsBuilt);
        formAskedAbout = TypedForm::BuiltOnObjectAsMade;
    }
    const BuiltinTable& table = walker.program().builtins();
    if (!location || table.at(table.canonicalID(location)).kind != BuiltinKind::Accessor)
        return TypedForm::None;

    ValueID receiver = store.objectOperand();
    ValueID stored = store.valueOperand();
    auto typeOfInput = [&](ValueID input) {
        return input == receiver ? typeOfObject : input == stored ? typeOfStored : TypeID();
    };
    TypesOfInputs typesAskedAbout(typeOfInput);
    CallInWalkBuiltins call(walker, store, object ? nullptr : &typesAskedAbout, location);
    if (!call.plan() || !call.goesByOneRow())
        return TypedForm::None;
    if (object)
        call.build();
    return formAskedAbout;
}

static BuiltinID findMethodThatStubOfClassEntersInWalkBuiltins(const Walker& walker, TypeID instances, PropertyKeyID key)
{
    BuiltinID member = findMemberReadOffInWalkBuiltins(walker, instances, key, true);
    if (member || !walker.program().admitsReplacedBuiltins)
        return member;
    member = findMemberReadOffInWalkBuiltins(walker, instances, key, true, MembersBehindWatch::AreAsBuilt);
    const BuiltinTable& table = walker.program().builtins();
    if (!member || !table.boxedCallStubOf(table.canonicalID(member)) || beliefInMemberFoundOnObjectAsMade(walker, member, instances, key) != BeliefInMemberAbove::IsAskedByItsByte)
        return { };
    return member;
}

BuiltinID findMethodThatSomeClassGivesUnderKey(Walker& walker, TypeID receiver, PropertyKeyID key, unsigned numberOfArguments)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinName name = memberNameOf(walker.program(), key);
    if (!name)
        return { };
    TypeArena& types = walker.types();
    auto instancesOf = [&](BuiltinClassID builtinClass) {
        return builtinClass == BuiltinClasses::String ? TypeTable::string : types.builtinClass(builtinClass);
    };
    auto added = walker.methodsGivenUnderKeys.add((static_cast<uint64_t>(key.index()) + 1) << 3 | std::min(numberOfArguments, 7u), Vector<Walker::MethodGivenUnderKey, 2> { });
    Vector<Walker::MethodGivenUnderKey, 2>& given = added.iterator->value;
    if (added.isNewEntry) {
        Vector<BuiltinBoxedStub, 4> found;
        for (BuiltinClassID builtinClass : { BuiltinClasses::Array, BuiltinClasses::Map, BuiltinClasses::Set, BuiltinClasses::String, BuiltinClasses::WeakMap, BuiltinClasses::WeakSet, BuiltinClasses::RegExp, BuiltinClasses::Promise, BuiltinClasses::Function }) {
            BuiltinID member = builtinClass == BuiltinClasses::Function ? findConstantMemberOnceForBuild(walker, Builtins::FunctionPrototype, key) : findMethodThatStubOfClassEntersInWalkBuiltins(walker, instancesOf(builtinClass), key);
            std::optional<BuiltinBoxedStub> boxedCall = member ? table.boxedCallStubOf(table.canonicalID(member)) : std::nullopt;
            if (!boxedCall || !servesCallsByName(*boxedCall, numberOfArguments))
                continue;
            found.append(*boxedCall);
            given.append({ builtinClass, table.canonicalID(member) });
        }
        given.removeAllMatching([&](const Walker::MethodGivenUnderKey& method) {
            for (std::optional<BuiltinBoxedStub> behind = formAskedBehind(*table.boxedCallStubOf(method.method)); behind; behind = formAskedBehind(*behind)) {
                if (servesCallsByName(*behind, numberOfArguments) && !found.contains(*behind))
                    return true;
            }
            return false;
        });
    }
    for (const Walker::MethodGivenUnderKey& method : given) {
        if (types.overlap(receiver, instancesOf(method.builtinClass)))
            return method.method;
        if (method.builtinClass == BuiltinClasses::Function && (types.at(receiver).traitsOfNamed.contains(ObjectTrait::IsClosure) || types.at(receiver).hasOnlyClosures() || mayContainAnyClosure(types, types.at(receiver))))
            return method.method;
    }
    return { };
}

static bool stubEntersCompiledMethodInWalkBuiltins(Walker& walker, BuiltinID method, unsigned numberOfArguments)
{
    if (numberOfArguments > maxArgumentCountOfCompiledMethodOfArray || !compiledMethodOfArray(method))
        return false;
    const Program& program = walker.program();
    bool namesFunction = false;
    for (const BuiltinOverload& overload : program.builtins().overloadsOf(method)) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        const RuntimeFunctionEntry* held = findRuntimeFunction(program, overload.code.implementation.function);
        if (!held || !held->function)
            return false;
        const Function& function = program.at(held->function);
        if (!function.captures.isEmpty() || function.has(FunctionFlags::TakesCallee))
            return false;
        namesFunction = true;
    }
    return namesFunction;
}

TypeID typeThatMapIsExpectedToGive(Walker& walker, const BodyValue& call)
{
    const LinkedContentsAtOperation* contents = call.opcode() == Opcode::Call ? walker.facts().held().contentsAtOperation(walker.currentFunction(), call.id()) : nullptr;
    if (!contents || !contents->value)
        return { };
    auto operation = call.as<Opcode::Call>();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(operation.calleeOperand());
    if (!read || !operation.thisValueOperand() || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return { };
    if (index.stripCasts(index.value(read).objectOperand()) != index.stripCasts(operation.thisValueOperand()))
        return { };
    const BuiltinTable& table = walker.program().builtins();
    for (auto [builtinClass, method] : { std::pair { BuiltinClassID(BuiltinClasses::Map), BuiltinID(Builtins::MapPrototypeGet) }, std::pair { BuiltinClassID(BuiltinClasses::WeakMap), BuiltinID(Builtins::WeakMapPrototypeGet) } }) {
        BuiltinID member = findMemberReadOffInWalkBuiltins(walker, walker.types().builtinClass(builtinClass), index.value(read).key(), true);
        if (member && table.canonicalID(member) == table.canonicalID(method))
            return walker.types().withPrimitives(contents->value, Primitive::Undefined);
    }
    return { };
}

bool lookupInMapIsOnlyReckonedWithAsNumber(Walker& walker, const BodyValue& call, const TypesOfInputs& types)
{
    if (call.opcode() != Opcode::Call || call.argumentsOperands().size() != 1)
        return false;
    auto operation = call.as<Opcode::Call>();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(operation.calleeOperand());
    if (!read || !operation.thisValueOperand() || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return false;
    if (index.stripCasts(index.value(read).objectOperand()) != index.stripCasts(operation.thisValueOperand()))
        return false;
    TypeID map = walker.types().builtinClass(BuiltinClasses::Map);
    TypeID receiver = types(operation.thisValueOperand());
    if (!receiver || !walker.types().isSubset(receiver, map))
        return false;
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID member = findMemberReadOffInWalkBuiltins(walker, map, index.value(read).key(), true);
    if (!member || table.canonicalID(member) != table.canonicalID(Builtins::MapPrototypeGet))
        return false;
    ValueID user = index.onlyUsingValue(call.id());
    return user && index.blockOf(user) == index.blockOf(call.id()) && valueIsOnlyReckonedWithAsNumber(walker, call.id(), types);
}

static BuiltinID findFunctionThatCallNamesOnBuiltinObject(const Walker& walker, const BodyIndex& index, ValueID receiver, PropertyKeyID key, unsigned count)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    ValueID written = index.stripCasts(receiver);
    if (!written || index.isParameter(written))
        return { };
    BuiltinID holder;
    BodyValue value = index.value(written);
    if (value.opcode() == Opcode::Builtin)
        holder = value.as<Opcode::Builtin>().builtin();
    else if (value.opcode() == Opcode::PropGet && value.isFreeName() && !liesInThread(value.key())) {
        if (BuiltinName name = program.builtinNameOf(value.key()))
            holder = table.findOwnMember(Builtins::GlobalThis, name);
    }
    BuiltinName name = program.builtinNameOf(key);
    if (!holder || !name || table.at(table.canonicalID(holder)).kind == BuiltinKind::Prototype)
        return { };
    BuiltinID member = table.findOwnMember(table.canonicalID(holder), name);
    std::optional<BuiltinBoxedStub> boxedCall = member ? table.boxedCallStubOf(table.canonicalID(member)) : std::nullopt;
    if (!boxedCall || cellTypesOfReceiversOf(*boxedCall) || !acceptsArgumentCount(*boxedCall, count) || table.methodCalledBy(*boxedCall) != table.canonicalID(member))
        return { };
    return table.canonicalID(member);
}

void nameMethodOfReceiverAsMade(Walker& walker, const BodyValue& call, Draft& draft)
{
    auto operation = call.as<Opcode::Call>();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(operation.calleeOperand());
    if (!read || !operation.thisValueOperand() || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return;
    if (index.stripCasts(index.value(read).objectOperand()) != index.stripCasts(operation.thisValueOperand()))
        return;
    if (PropertyKeyID key = index.value(read).key(); liesInThread(key) || !builtinMemberNameOf(walker.program(), key))
        return;
    if (BuiltinID function = findFunctionThatCallNamesOnBuiltinObject(walker, index, operation.thisValueOperand(), index.value(read).key(), static_cast<unsigned>(draft.argumentsOperands().size()))) {
        draft.setMethodOfReceiverAsMade(function);
        return;
    }
    Reference receiver = walker.referenceOf(operation.thisValueOperand());
    TypeArena& types = walker.types();
    bool isKnownClosure = receiver && receiver.isRecord() && types.at(walker.typeOf(receiver)).hasOnlyClosures();
    if (!receiver || (!receiver.isValue() && !isKnownClosure))
        return;

    auto isOfOneClass = [&](TypeID type) {
        if (!type)
            return false;
        Type known = types.at(type);
        return known.isObject() ? !!singleBuiltinClassOf(types, known) : known.isJavaScriptValue() && !known.isNever() && known.hasOnlyPrimitives(Primitive::AnyString);
    };
    TypeID known = types.withoutNullish(walker.typeOf(receiver));
    const bool classIsOnlyExpected = !isOfOneClass(known);
    if (classIsOnlyExpected) {
        TypeID expected = findExpectation(walker, operation.thisValueOperand()).type;
        known = expected ? types.withoutNullish(expected) : TypeID();
        if (!isOfOneClass(known) || !types.overlap(walker.typeOf(receiver), known)) {
            if (BuiltinID method = findMethodThatSomeClassGivesUnderKey(walker, walker.typeOf(receiver), index.value(read).key(), static_cast<unsigned>(draft.argumentsOperands().size()))) {
                draft.setMethodOfReceiverAsMade(method);
                draft.setMethodIsNamedByKeyAlone(true);
            }
            return;
        }
    }
    BuiltinID member = findMethodThatStubOfClassEntersInWalkBuiltins(walker, known, index.value(read).key());
    const BuiltinTable& table = walker.program().builtins();
    std::optional<BuiltinBoxedStub> boxedCall = member ? table.boxedCallStubOf(table.canonicalID(member)) : std::nullopt;
    if (boxedCall && classIsOnlyExpected && formAskedBehind(*boxedCall) && findMethodThatSomeClassGivesUnderKey(walker, known, index.value(read).key(), static_cast<unsigned>(draft.argumentsOperands().size())) != table.canonicalID(member))
        return;
    if (boxedCall && servesCallsByName(*boxedCall, static_cast<unsigned>(draft.argumentsOperands().size())))
        draft.setMethodOfReceiverAsMade(table.canonicalID(member));
    else if (member && member == findMemberReadOffInWalkBuiltins(walker, known, index.value(read).key(), true) && stubEntersCompiledMethodInWalkBuiltins(walker, table.canonicalID(member), static_cast<unsigned>(draft.argumentsOperands().size())))
        draft.setMethodOfReceiverAsMade(table.canonicalID(member));
}

BuiltinCallAskedAbout wouldCallBuiltin(Walker& walker, const BodyValue& call, const TypesOfInputs& types, bool* standsInLine)
{
    if (!opcodesOfWalkBuiltins.contains(call.opcode()) || callsOfListsInWalkBuiltins.contains(call.opcode()) || (call.opcode() == Opcode::Call && call.isDoneInPlace()))
        return BuiltinCallAskedAbout::None;
    CallInWalkBuiltins asked(walker, call, &types);
    if (!asked.plan())
        return BuiltinCallAskedAbout::None;
    if (standsInLine)
        *standsInLine = asked.standsInLine();
    return asked.goesByOneRow() ? BuiltinCallAskedAbout::ByOneRow : BuiltinCallAskedAbout::OnAnyOperands;
}

std::optional<Effects> effectsOfCallByOneRowAhead(Walker& walker, const BodyValue& call, const TypesOfInputs& types)
{
    if (!opcodesOfWalkBuiltins.contains(call.opcode()) || callsOfListsInWalkBuiltins.contains(call.opcode()) || (call.opcode() == Opcode::Call && call.isDoneInPlace()))
        return std::nullopt;
    CallInWalkBuiltins asked(walker, call, &types);
    if (!asked.plan() || !asked.goesByOneRow())
        return std::nullopt;
    return asked.effectsOfAllThatIsBuilt();
}

static bool nothingSaysWhichBuiltinIsCalled(Walker& walker, const BodyValue& call)
{
    ValueID callee = call.calleeOperand();
    Reference called = walker.referenceOf(callee);
    if (!walker.hasUninformativeType(called)) {
        TypeID proven = called ? walker.typeOf(called) : TypeID();
        if (!proven)
            return false;
        Type type = walker.types().at(proven);
        if (type.isObject() && singleBuiltinOf(walker.types(), type))
            return false;
        if (type.isObject() && type.hasOnlyClosures())
            return true;
    }
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = index.stripCasts(callee);
    bool isReadByName = read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName();
    if (!isReadByName)
        return true;
    PropertyKeyID key = index.value(read).key();
    if (liesInThread(key) || !builtinMemberNameOf(walker.program(), key))
        return true;
    return walker.hasUninformativeType(walker.referenceOf(index.value(read).objectOperand()));
}

FunctionID findLibraryFunctionOfRowsReachedBy(Walker& walker, const BodyValue& call, RowsThatNameLibraryFunction taken)
{
    CallInWalkBuiltins asked(walker, call);
    FunctionID reached = asked.findLibraryFunctionOfRowsReached(taken);
    if (reached)
        asked.holdLinksWordAgainstOperands();
    return reached;
}

TypeID findTypeOfResultOfRowsReachedBy(Walker& walker, const BodyValue& call)
{
    CallInWalkBuiltins asked(walker, call);
    TypeID result = asked.findTypeOfResultOfRowsReached();
    if (result)
        asked.holdLinksWordAgainstOperands();
    return result;
}

BuiltinOverloadID findMatchingOverload(Walker& walker, const BodyValue& call)
{
    CallInWalkBuiltins asked(walker, call);
    BuiltinOverloadID match = asked.findMatchingOverload();
    if (match)
        asked.holdLinksWordAgainstOperands();
    return match;
}
constinit const OpcodeSet opcodesThatCallBuiltinTakes = opcodesOfWalkBuiltins;
static_assert(opcodesThatCallBuiltinTakesInChains == opcodesOfWalkBuiltins);

bool callBuiltinForwarded(Walker& walker, const BodyValue& call, const Draft& forwarded)
{
    if (!walker.has(Mechanism::BuiltinCalls))
        return false;
    CallInWalkBuiltins planned(walker, call, forwarded);
    if (!planned.plan())
        return false;
    planned.build();
    return true;
}

static constexpr uint32_t closedOperationsOfPushOfList = 2;
static constexpr int64_t lengthOfShortListPassedToPush = defaultLimitValue(LimitID::MaxReservedStackArguments, hostArchitecture());

OperatorForm operatorFormFor(Walker&, const Draft&);

static bool appendListPassedToPush(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::CallList || !walker.has(Mechanism::Guesses) || !walker.opensGuesses() || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    auto operation = call.as<Opcode::CallList>();
    bool isUnused = index.isUnused(call.id()) || (walker.isInRepeatedCode() && isResultUnused(index, call.id()));
    if (!isUnused || !operation.thisValueOperand())
        return false;
    ValueID read = index.stripCasts(operation.calleeOperand());
    if (!read || index.isParameter(read) || index.opcodeOf(read) != Opcode::PropGet || index.value(read).isFreeName())
        return false;
    if (index.stripCasts(index.value(read).objectOperand()) != index.stripCasts(operation.thisValueOperand()))
        return false;
    ValueID list = operation.listOperand();
    ValueID spread(call.id().index() - 1);
    if (index.isParameter(list) || index.opcodeOf(list) != Opcode::NewArray || index.blockOf(list) != index.blockOf(call.id()) || index.useCount(list) != 2)
        return false;
    if (index.isParameter(spread) || index.blockOf(spread) != index.blockOf(call.id()) || index.opcodeOf(spread) != Opcode::AppendSpread || index.value(spread).operand(0) != list)
        return false;

    const BuiltinTable& table = walker.program().builtins();
    BuiltinID push = table.canonicalID(Builtins::ArrayPrototypePush);
    BuiltinID member = findConstantMemberOnceForBuild(walker, table.at(BuiltinClasses::Array).prototype, index.value(read).key());
    if (!member || table.canonicalID(member) != push || !isTestableBuiltin(table, push))
        return false;

    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(operation.calleeOperand());
    Reference receiver = walker.referenceOf(operation.thisValueOperand());
    Reference passed = walker.referenceOf(list);
    TypeID expected = types.forBuiltin(push);
    if (!callee || !receiver || !receiver.isValue() || !passed || !hasKind(types, walker.typeOf(receiver), OperandKind::Array) || !types.overlap(walker.typeOf(callee), expected))
        return false;
    bool calleeIsProven = types.isSubset(walker.typeOf(callee), expected);
    if (!calleeIsProven && !callee.isValue())
        return false;
    bool receiverIsAsMade = isKnownPristine(walker, receiver);
    if ((!calleeIsProven || !receiverIsAsMade) && !shouldOpenGuess(walker, callee, expected, true, closedOperationsOfPushOfList))
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    Origin origin = written.origin();
    origin.isTailCall = false;

    Vector<Reference, 3> tests;
    Draft length(Opcode::GetArrayLength, origin);
    walker.chooseConservativeFields(length);
    length.setArrayOperand(passed);
    Draft lengthOfShortList(Opcode::Integer, origin);
    lengthOfShortList.setInteger(lengthOfShortListPassedToPush);
    Draft comparison(Opcode::LessEq, origin);
    walker.chooseConservativeFields(comparison);
    comparison.setLeftOperand(walker.build(WTF::move(length)));
    comparison.setRightOperand(walker.constant(WTF::move(lengthOfShortList)));
    if (!comparison.leftOperand() || !comparison.rightOperand())
        return false;
    if (walker.has(Mechanism::TypedOperations))
        comparison.setForm(operatorFormFor(walker, comparison));
    tests.append(walker.build(WTF::move(comparison)));
    if (!receiverIsAsMade)
        tests.append(buildTestOfObjectAsMade(walker, receiver, origin));
    if (!calleeIsProven) {
        Draft test(Opcode::TypeTest, origin);
        test.setValueOperand(callee);
        test.setTestedType(expected);
        tests.append(walker.build(WTF::move(test)));
    }
    if (std::ranges::any_of(tests, [](Reference test) { return !test; }))
        return false;

    walker.recordSpeculation();
    BlockID asWritten = walker.addBlock(BlockFrequency::Unlikely);
    for (Reference test : tests) {
        BlockID passes = walker.addBlock();
        walker.branch(test, passes, asWritten);
        walker.continueIn(passes);
    }
    if (!receiverIsAsMade)
        markObjectPristine(walker, receiver);
    Draft append(Opcode::AppendElements, origin);
    walker.chooseConservativeFields(append);
    append.setArrayOperand(receiver);
    append.setSourceOperand(passed);
    walker.build(WTF::move(append));
    walker.count(StatisticID::SpreadPushesCompiledAsAppends);

    BlockID behind = walker.addBlock();
    Reference answer = index.isUnused(call.id()) ? Reference() : walker.addParameter(behind, { }, Representation::JSValue);
    auto jumpBehind = [&](Reference result) {
        if (walker.isUnreachable())
            return;
        if (answer)
            walker.jumpTo(behind, singleElementSpan(result));
        else
            walker.jumpTo(behind);
    };
    jumpBehind(answer && !walker.isUnreachable() ? buildArrayLength(walker, receiver, origin) : Reference());
    walker.continueIn(asWritten);
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        walker.leaveAnythingAt(listed);
    Draft closed = written;
    walker.chooseConservativeFields(closed);
    jumpBehind(walker.build(WTF::move(closed)));
    walker.continueIn(behind);
    if (answer)
        walker.bind(call.id(), answer);
    return true;
}

bool canCallCompilerHelper(Walker&, FunctionID helper);
Reference callCompilerHelper(Walker&, FunctionID helper, std::span<const Reference> arguments, Origin);

static FunctionID findFunctionThatGoesOverListPassedTo(Walker& walker, BuiltinID called)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    BuiltinID helper;
    if (called == table.canonicalID(Builtins::MathMax))
        helper = Builtins::CompilerHelpersMaxOfList;
    else if (called == table.canonicalID(Builtins::MathMin))
        helper = Builtins::CompilerHelpersMinOfList;
    if (!helper || !table.contains(helper))
        return { };
    for (const BuiltinOverload& row : table.overloadsOf(table.canonicalID(helper))) {
        const RuntimeFunctionEntry* held = row.mode == BuiltinCallMode::Call ? findRuntimeFunction(program, row.libraryFunction()) : nullptr;
        if (held && held->function && program.at(held->function).hasBody())
            return held->function;
    }
    return { };
}

bool goOverListInPlaceOfCallOfBuiltin(Walker& walker, const BodyValue& call, Draft& written)
{
    if (written.opcode != Opcode::CallList || walker.isUnreachable())
        return false;
    Reference callee = written.calleeOperand();
    Reference list = written.listOperand();
    TypeID proven = callee ? walker.typeOf(callee) : TypeID();
    if (!proven || !list)
        return false;
    Type type = walker.types().at(proven);
    BuiltinID called = type.isObject() ? singleBuiltinOf(walker.types(), type) : BuiltinID();
    FunctionID function = called ? findFunctionThatGoesOverListPassedTo(walker, walker.program().builtins().canonicalID(called)) : FunctionID();
    if (!function || !canCallCompilerHelper(walker, function))
        return false;
    Origin origin = written.origin();
    origin.isTailCall = false;
    Reference found = callCompilerHelper(walker, function, singleElementSpan(list), origin);
    if (!found || walker.isUnreachable() || walker.hasFailed())
        return true;
    walker.count(StatisticID::LibraryFunctionsCalledDirectly);

    Draft question(Opcode::TypeTest, origin);
    question.setValueOperand(found);
    question.setTestedType(TypeTable::undefined);
    Reference isLeftToCall = walker.build(WTF::move(question));
    if (!isLeftToCall)
        return true;
    BlockID asWritten = walker.addBlock(BlockFrequency::Unlikely);
    BlockID isFound = walker.addBlock();
    BlockID behind = walker.addBlock();
    Reference result = walker.addParameter(behind, { }, Representation::JSValue);
    walker.branch(isLeftToCall, asWritten, isFound);
    walker.continueIn(isFound);
    if (!walker.isUnreachable()) {
        Draft proof(Opcode::Narrowed, origin);
        proof.setValueOperand(found);
        proof.setTestOperand(isLeftToCall);
        proof.setOperandIndex(OperandIndex::TypeTest::value);
        proof.setIsOnTakenEdge(false);
        Reference answer = isLeftToCall.isValue() ? walker.build(WTF::move(proof)) : found;
        TypeArena& types = walker.types();
        TypeID byRows = walker.resultTypeOf(written);
        if (answer && byRows && !types.isSubset(walker.typeOf(answer), byRows)) {
            answer = walker.proveOrTrap(answer, walker.typeThatUnitMayTestFor(types.testablePartOf(byRows)), ProofSource::ResultOfCall, origin);
            walker.count(StatisticID::LibraryResultsCheckedAgainstSignatures);
        }
        if (answer && !walker.isUnreachable())
            walker.jumpTo(behind, singleElementSpan(answer));
    }
    walker.continueIn(asWritten);
    Draft closed = written;
    closed.setOrigin(origin);
    closed.resultIsReturnedAtOnce = false;
    walker.chooseConservativeFields(closed);
    Reference answered = walker.build(WTF::move(closed));
    if (answered && !walker.isUnreachable())
        walker.jumpTo(behind, singleElementSpan(answered));
    walker.continueIn(behind);
    walker.bind(call.id(), result);
    return true;
}

static bool goOverListPassedInPlaceOfCall(Walker& walker, const BodyValue& call)
{
    if (call.opcode() != Opcode::CallList || walker.isUnreachable())
        return false;
    Draft written = walker.draftOf(call);
    if (walker.hasFailed())
        return false;
    Reference list = findElidableSpreadSource(walker, call, written.listOperand());
    if (list)
        written.setListOperand(list);
    else if (Reference made = written.listOperand(); !made || !made.isValue() || walker.code().at(made.id()).opcode != Opcode::NewArray)
        return false;
    if (!goOverListInPlaceOfCallOfBuiltin(walker, call, written))
        return false;
    if (list)
        walker.count(StatisticID::SpreadCallArrayCopiesElided);
    return true;
}

bool callBuiltinOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!opcodesOfWalkBuiltins.contains(value.opcode()))
        return false;
    if (nothingSaysWhichBuiltinIsCalled(walker, value))
        return false;
    if (!callsOfListsInWalkBuiltins.contains(value.opcode()) && (isCallForwardedInPlace(walker, value) || keyOfSymbolMadeOnce(walker, value)))
        return false;
    if (forwardCallOfBuiltin(walker, value) || forwardConstructionBehindTestOfTarget(walker, value))
        return true;
    if (buildToStringOfPrimitiveByRows(walker, value))
        return true;
    if (buildByteLiteral(walker, value))
        return true;
    if (callLibraryFunctionBehindRows(walker, value)) {
        countBuiltinCallOfProgram(walker, StatisticID::BuiltinCallsOnLibraryFunction);
        return true;
    }
    if (appendListPassedToPush(walker, value))
        return true;
    if (buildPushOntoLiteralThatLoopFills(walker, value))
        return true;
    std::optional<Walker::HeardAsArm> heardAsArm;
    if (hearRaiseWherePushIsBuiltAsWritten(walker, value))
        heardAsArm.emplace(walker);
    CallInWalkBuiltins call(walker, value);
    if (!call.plan()) {
        if (goOverListPassedInPlaceOfCall(walker, value))
            return true;
        if (call.callsKnownBuiltin())
            countBuiltinCallOfProgram(walker, StatisticID::BuiltinCallsLeftGeneric);
        return false;
    }
    if (!call.goesByOneRow() && openGuessAtOperandOfBuiltinCall(walker, value))
        return true;
    if (call.goesByOneRow() && walker.isInLoop() && call.standsInLine() && call.convertsArgumentThatComesRoundLoop() && openGuessAtOperandOfBuiltinCall(walker, value, OperandsTestedAtBuiltinCall::Arguments))
        return true;
    countBuiltinCallOfProgram(walker, call.statisticOfRow());
    call.build();
    noteListedOwnKeys(walker, value.id());
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
