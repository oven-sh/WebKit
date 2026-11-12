/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTClassGuards.h"
#include "AOTClassMemberTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTPlan.h"
#include "AOTRowsOfLayouts.h"
#include "AOTSort.h"
#include <algorithm>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT {

static_assert(LayoutID().index() == LayoutData::noBase && FunctionID().index() == LayoutData::noBase);
static_assert(MethodData::nameBits == FieldData::nameBits);
static_assert(numberOfClassSides == 2);

namespace {

struct DefinitionInClassRow {
    uint32_t name;
    MethodKind kind;
    uint32_t cell;
};

} // anonymous namespace

static bool isRankedAmongClassRecords(const TableEncoder& encoder, LayoutID id)
{
    return encoder.program.at(id).kind == LayoutKind::Instance && encoder.numbers.rankOfClass(id);
}

static size_t placeOfSideOfClass(LayoutID classLayout, ClassSide side)
{
    return static_cast<size_t>(classLayout.index()) * numberOfClassSides + static_cast<size_t>(side);
}

static BitVector findSidesWithClassGuardsHereOrBelow(const TableEncoder& encoder, std::span<const LayoutID> parentsThatListClasses)
{
    BitVector sides(parentsThatListClasses.size() * numberOfClassSides);
    auto markUpwardFrom = [&](LayoutID classLayout, ClassSide side) {
        while (classLayout && !sides.quickSet(placeOfSideOfClass(classLayout, side)))
            classLayout = parentsThatListClasses[classLayout.index()];
    };
    const PlacedStatics& statics = encoder.numbers.statics();
    for (size_t place = 0; place < statics.classGuardBits.size(); ++place) {
        if (statics.classGuardBits[place] != noClassGuardBit)
            markUpwardFrom(LayoutID(place / numberOfClassSides), static_cast<ClassSide>(place % numberOfClassSides));
    }
    for (const PlacedClassGuardOfName& guard : statics.classGuardsOfNames)
        markUpwardFrom(guard.classLayout, guard.side);
    for (const PlacedClassGuardOfName& guard : statics.namesOfClassGuardsOfSeveralNames)
        markUpwardFrom(guard.classLayout, guard.side);
    for (const PlacedClassGuardOfName& guard : statics.guardsOfAssignedMembers)
        markUpwardFrom(guard.classLayout, guard.side);
    return sides;
}

static Typed::BitVector findClassesWithRecordsAllTheWayUp(const TableEncoder& encoder)
{
    const Typed::Program& program = encoder.program;
    return Typed::answerAlongRows(program.layouts.size(), false, [&](LayoutID id) -> Typed::StepAlongRow {
        const Typed::Layout& layout = program.at(id);
        if (!isRankedAmongClassRecords(encoder, id) || !layout.constructor || !encoder.findEntrypoint(layout.constructor) || Typed::parentNamedByClassRecord(program, encoder.facts, id).layout != layout.base)
            return { false, { } };
        if (!layout.base)
            return { true, { } };
        return { std::nullopt, layout.base };
    });
}

static Typed::BitVector findClassesWhoseClassObjectsFindNoHasInstance(const TableEncoder& encoder)
{
    const Typed::Program& program = encoder.program;
    PropertyKeyID key = program.keyOf(program.builtins().nameOf(WellKnownSymbol::HasInstance));
    return Typed::answerAlongRows(program.layouts.size(), false, [&](LayoutID id) -> Typed::StepAlongRow {
        if (!encoder.facts.linked().findsNothing(program, id, ClassSide::Static, key, Typed::Of::HolderItself))
            return { false, { } };
        if (!program.at(id).base)
            return { true, { } };
        return { std::nullopt, program.at(id).base };
    });
}

static BitVector findClassesWhosePrivateNamesTextsKnow(const Typed::Program& program)
{
    BitVector known(program.layouts.size());
    for (const Typed::EvalScope& scope : program.evalScopes) {
        for (const Typed::EvalBinding& binding : scope.bindings) {
            if (binding.layout && !program.at(binding.layout).isEnvironment())
                known.quickSet(binding.layout.index());
        }
    }
    return known;
}

void askRowsOfClassesForClassRecords(TableEncoder& encoder)
{
    encoder.classesWithRecordsAllTheWayUp = findClassesWithRecordsAllTheWayUp(encoder);
    encoder.classesWhoseClassObjectsFindNoHasInstance = findClassesWhoseClassObjectsFindNoHasInstance(encoder);
    encoder.classesWhosePrivateNamesTextsKnow = findClassesWhosePrivateNamesTextsKnow(encoder.program);
}

static bool isClosedClassSide(const TableEncoder& encoder, LayoutID classLayout, ClassSide side)
{
    return encoder.classesWithRecordsAllTheWayUp.quickGet(classLayout.index()) && encoder.facts.linked().holderIsChangedInSightAlone(encoder.program, classLayout, side);
}

static void countClosedObjects(const TableEncoder& encoder)
{
    size_t numberOfSides[numberOfClassSides] { };
    size_t sidesInheritingAddedNames = 0;
    for (size_t index = 0; index < encoder.program.layouts.size(); ++index) {
        for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
            bool isHeld = isClosedClassSide(encoder, LayoutID(index), side);
            numberOfSides[static_cast<unsigned>(side)] += isHeld;
            sidesInheritingAddedNames += isHeld && encoder.facts.linked().layout(LayoutID(index)).namesAreDefinedPastLists[static_cast<unsigned>(side)];
        }
    }
    encoder.build.statistics().count(StatisticID::ClosedSidesInheritingAddedNames, sidesInheritingAddedNames);
    encoder.build.statistics().count(StatisticID::ClosedPrototypeObjects, numberOfSides[static_cast<unsigned>(ClassSide::Prototype)]);
    encoder.build.statistics().count(StatisticID::ClosedClassObjects, numberOfSides[static_cast<unsigned>(ClassSide::Static)]);
    encoder.build.statistics().count(StatisticID::FunctionsWithClosedClosures, encoder.plan.slots().closedClosureFunctionCount());
}

void groupClassesByParentInClassRecords(TableEncoder& encoder, std::span<const LayoutID> parentsThatListClasses)
{
    encoder.sidesWithClassGuardsHereOrBelow = findSidesWithClassGuardsHereOrBelow(encoder, parentsThatListClasses);
    countClosedObjects(encoder);
    Vector<uint32_t> firstClassBelow(FillWith { }, parentsThatListClasses.size() + 1, 0);
    for (LayoutID parent : parentsThatListClasses) {
        if (parent)
            ++firstClassBelow[parent.index() + 1];
    }
    for (size_t index = 0; index < parentsThatListClasses.size(); ++index)
        firstClassBelow[index + 1] += firstClassBelow[index];

    Vector<uint32_t> classesByParent(FillWith { }, firstClassBelow.last(), 0);
    Vector<uint32_t> nextPlace = firstClassBelow;
    for (size_t index = 0; index < parentsThatListClasses.size(); ++index) {
        if (LayoutID parent = parentsThatListClasses[index])
            classesByParent[nextPlace[parent.index()]++] = static_cast<uint32_t>(index);
    }
    encoder.classesByParent = WTF::move(classesByParent);
    encoder.firstClassBelow = WTF::move(firstClassBelow);
}

static std::span<const uint32_t> classesBelowInClassRecords(const TableEncoder& encoder, LayoutID parent)
{
    uint32_t begin = encoder.firstClassBelow[parent.index()];
    uint32_t end = encoder.firstClassBelow[parent.index() + 1];
    return encoder.classesByParent.span().subspan(begin, end - begin);
}

static uint32_t nameInClassRecord(const TableEncoder& encoder, PropertyKeyID key, ASCIILiteral use, LayoutID classLayout)
{
    uint32_t name = encoder.identifierNumber(key, use, classLayout);
    if (name >> MethodData::nameBits) {
        encoder.fail("NameDoesNotFitClassRecord"_s, classLayout);
        return 0;
    }
    return name;
}

static Typed::MemberCreation creationOfMemberInClassRecord(const Typed::ClassCreationPlan& creation, size_t member)
{
    return member < creation.members.size() ? creation.members[member] : Typed::MemberCreation::AtDefinition;
}

static LayoutID environmentHeldByClassObject(const Typed::Program& program, const Typed::Plan& plan, const Typed::Layout& layout)
{
    if (!layout.constructor || plan.promotedEnvironments().numberOfRemovedCapturesOf(layout.constructor))
        return { };
    const auto& captures = program.at(layout.constructor).captures;
    if (captures.size() != 1 || !captures[0].written())
        return { };
    return program.at(captures[0].written()).environment;
}

static bool memberCapturesAtMostEnvironmentOfClassObject(const Typed::Program& program, const Typed::Layout& layout, LayoutID environmentOfClassObject, FunctionID member)
{
    const auto& captures = program.at(member).captures;
    if (captures.isEmpty())
        return true;
    return environmentOfClassObject && captures.size() == 1 && captures[0].written() == program.at(layout.constructor).captures[0].written();
}

static bool codeCreatesClass(const TableEncoder& encoder, LayoutID classLayout)
{
    uint32_t request[] { static_cast<uint32_t>(RequestKind::NewClass), classLayout.index() };
    return !!encoder.numbers.numberOf(NumberSpace::Request, request);
}

static MethodData describeMemberInClassRecord(const TableEncoder& encoder, LayoutID classLayout, LayoutID environmentOfClassObject, const Typed::ClassMember& member, Typed::MemberCreation creation, bool holdsOneFunction, bool isLaidFirst)
{
    const Typed::Program& program = encoder.program;
    bool takesEnvironment = member.function && program.at(member.function).captures.size() > encoder.plan.promotedEnvironments().numberOfRemovedCapturesOf(member.function);
    MethodData method { };
    method.name = member.key ? nameInClassRecord(encoder, member.key, "a member in the record of"_s, classLayout) : 0;
    method.kindBits = static_cast<uint32_t>(member.kind);
    method.flags = (member.key ? 0 : methodHasComputedName) | (takesEnvironment ? methodTakesEnvironment : 0);
    if (!holdsOneFunction)
        method.flags |= methodMayBeStoredOver;
    if (isLaidFirst)
        method.flags |= methodIsStoredFirst;
    method.function = FunctionID().index();
    switch (creation) {
    case Typed::MemberCreation::AtDefinition:
        method.flags |= methodIsCreatedAtDefinition;
        if (std::optional<FunctionID> entrypoint = member.function ? encoder.findEntrypoint(member.function) : std::nullopt)
            method.function = entrypoint->index();
        break;
    case Typed::MemberCreation::ByClassRecord:
        if (!member.key || !member.function || !memberCapturesAtMostEnvironmentOfClassObject(program, program.at(classLayout), environmentOfClassObject, member.function)) {
            encoder.fail("ClassRecordCannotCreateMember"_s, classLayout);
            break;
        }
        if (std::optional<FunctionID> entrypoint = encoder.findEntrypoint(member.function); entrypoint || !codeCreatesClass(encoder, classLayout)) {
            method.function = entrypoint.value_or(FunctionID()).index();
            break;
        }
        method.function = encoder.entrypointOf(member.function, "a member that the run time creates for"_s, classLayout);
        break;
    }
    return method;
}

static uint32_t guardBitInClassRecord(const TableEncoder& encoder, LayoutID classLayout, ClassSide side)
{
    const Vector<uint32_t>& bits = encoder.numbers.statics().classGuardBits;
    size_t place = placeOfSideOfClass(classLayout, side);
    return place < bits.size() ? bits[place] : noClassGuardBit;
}

Vector<ClassBelowWithGuardOfNameData> collectClassesBelowWithGuardsOfNames(const TableEncoder& encoder)
{
    const PlacedStatics& statics = encoder.numbers.statics();
    Vector<ClassBelowWithGuardOfNameData> classes;
    for (const Vector<PlacedClassGuardOfName>* guards : { &statics.classGuardsOfNames, &statics.namesOfClassGuardsOfSeveralNames }) {
        for (const PlacedClassGuardOfName& guard : *guards) {
            if (!Typed::parentNamedByClassRecord(encoder.inputs.program, encoder.facts, guard.classLayout).layout)
                continue;
            if (std::optional<uint32_t> name = encoder.numbers.numberOf(guard.spaceOfName, std::span<const uint32_t>(&guard.name, 1)))
                classes.append(ClassBelowWithGuardOfNameData { *name, guard.classLayout.index() * numberOfClassSides + static_cast<uint32_t>(guard.side) });
        }
    }
    AOT::sort(classes);
    classes.shrink(static_cast<size_t>(std::ranges::unique(classes).begin() - classes.begin()));
    return classes;
}

Vector<AssignedMemberData> collectAssignedMembers(const TableEncoder& encoder)
{
    auto entrypointOf = [&](FunctionID function) {
        return (function ? encoder.findEntrypoint(function).value_or(FunctionID()) : FunctionID()).index();
    };
    Vector<AssignedMemberData> members;
    for (const PlacedClassGuardOfName& guard : encoder.numbers.statics().guardsOfAssignedMembers) {
        if (!isRankedAmongClassRecords(encoder, guard.classLayout))
            encoder.fail("CodeTestsGuardOfClassWithoutRank"_s, guard.classLayout);
        std::optional<uint32_t> name = encoder.numbers.numberOf(guard.spaceOfName, std::span<const uint32_t>(&guard.name, 1));
        if (!name || guard.spaceOfName != NumberSpace::Name) {
            encoder.fail("GuardedNameOfClassHasNoNumber"_s, guard.classLayout);
            continue;
        }
        Typed::AssignedMember member = Typed::findAssignedMember(encoder.program, encoder.facts, guard.classLayout, PropertyKeyID(guard.name));
        members.append(AssignedMemberData { guard.classLayout.index() * numberOfClassSides + static_cast<uint32_t>(guard.side), *name, guard.bit, entrypointOf(member.method), entrypointOf(member.getter), entrypointOf(member.setter), member.holdsValue });
    }
    AOT::sort(members, { }, &AssignedMemberData::place);
    return members;
}

static Vector<uint32_t> collectGuardsOfNamesInClassRecord(const TableEncoder& encoder, LayoutID classLayout, ClassSide side)
{
    auto sideOfGuard = [](const PlacedClassGuardOfName& guard) {
        return std::pair(guard.classLayout, guard.sideWord());
    };
    const PlacedStatics& statics = encoder.numbers.statics();
    auto guardsOfSide = std::ranges::equal_range(statics.classGuardsOfNames, std::pair(classLayout, sideWordOfClassGuardOfName(side, AskerOfClassGuardOfName::Reads)), { }, sideOfGuard);
    auto guardsThatStoresAloneAsk = std::ranges::equal_range(statics.classGuardsOfNames, std::pair(classLayout, sideWordOfClassGuardOfName(side, AskerOfClassGuardOfName::StoresAlone)), { }, sideOfGuard);
    auto guardsAskedBehindLooksAtObjects = std::ranges::equal_range(statics.classGuardsOfNames, std::pair(classLayout, sideWordOfClassGuardOfName(side, AskerOfClassGuardOfName::ReadsBehindLookAtObject)), { }, sideOfGuard);
    auto namesOfGuardsOfSeveralNames = std::ranges::equal_range(statics.namesOfClassGuardsOfSeveralNames, std::pair(classLayout, sideWordOfClassGuardOfName(side, AskerOfClassGuardOfName::Reads)), { }, sideOfGuard);
    if (guardsOfSide.empty() && guardsThatStoresAloneAsk.empty() && guardsAskedBehindLooksAtObjects.empty() && namesOfGuardsOfSeveralNames.empty())
        return { };
    if (!isRankedAmongClassRecords(encoder, classLayout))
        encoder.fail("CodeTestsGuardOfClassWithoutRank"_s, classLayout);

    Vector<NamedClassGuardData, 8> guards;
    auto listUnderNumberOfName = [&](const PlacedClassGuardOfName& guard) {
        std::optional<uint32_t> name = encoder.numbers.numberOf(guard.spaceOfName, std::span<const uint32_t>(&guard.name, 1));
        if (!name) {
            encoder.fail("GuardedNameOfClassHasNoNumber"_s, classLayout);
            return;
        }
        RELEASE_ASSERT(!(guard.bit & (NamedClassGuardData::isAskedByStoresAlone | NamedClassGuardData::isAskedBehindLookAtObject)));
        uint32_t flagOfAsker = 0;
        if (guard.asker == AskerOfClassGuardOfName::StoresAlone)
            flagOfAsker = NamedClassGuardData::isAskedByStoresAlone;
        else if (guard.asker == AskerOfClassGuardOfName::ReadsBehindLookAtObject)
            flagOfAsker = NamedClassGuardData::isAskedBehindLookAtObject;
        guards.append(NamedClassGuardData { *name, guard.bit | flagOfAsker });
    };
    std::ranges::for_each(guardsOfSide, listUnderNumberOfName);
    std::ranges::for_each(guardsThatStoresAloneAsk, listUnderNumberOfName);
    std::ranges::for_each(guardsAskedBehindLooksAtObjects, listUnderNumberOfName);
    std::ranges::for_each(namesOfGuardsOfSeveralNames, listUnderNumberOfName);
    AOT::sort(guards, { }, [](const NamedClassGuardData& guard) {
        return std::pair(guard.name, guard.bit);
    });

    Vector<uint32_t> list;
    list.reserveInitialCapacity(1 + 2 * guards.size());
    list.append(static_cast<uint32_t>(guards.size()));
    for (const NamedClassGuardData& guard : guards) {
        list.append(guard.name);
        list.append(guard.bit);
    }
    return list;
}

ClassRecordParts collectClassRecordParts(const TableEncoder& encoder, LayoutID classLayout)
{
    const Typed::Program& program = encoder.program;
    const Typed::Layout& layout = program.at(classLayout);
    const Typed::ClassCreationPlan& creation = encoder.plan.slots().classCreation(classLayout);
    std::optional<FunctionID> classConstructor = layout.constructor ? encoder.findEntrypoint(layout.constructor) : std::nullopt;
    LayoutID environment = environmentHeldByClassObject(program, encoder.plan, layout);
    ClassRecordParts parts;
    ClassData& data = parts.data;

    if (classConstructor || layout.has(Typed::LayoutFlags::IsOfCreatedObject)) {
        std::span<const Typed::ClassMember> members = encoder.facts.membersOf(classLayout);
        parts.methods.reserveInitialCapacity(members.size());
        bool answerHangsOnFunction = layout.has(Typed::LayoutFlags::IsConstructorFunction);
        UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> namesThatHoldOneFunction;
        for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
            namesThatHoldOneFunction.clear();
            for (size_t index = 0; index < members.size(); ++index) {
                const Typed::ClassMember& member = members[index];
                if (member.side != side)
                    continue;
                auto ask = [&] {
                    return encoder.facts.linked().memberHoldsOneFunction(program, classLayout, side, member.key, member.function);
                };
                bool holdsOneFunction = member.key && (answerHangsOnFunction ? ask() : namesThatHoldOneFunction.ensure(member.key.bits(), ask).iterator->value);
                bool isLaidFirst = answerHangsOnFunction && side == ClassSide::Prototype && member.key && member.function && encoder.facts.linked().findMember(program, classLayout, side, member.key).method == member.function;
                parts.methods.append(describeMemberInClassRecord(encoder, classLayout, environment, member, creationOfMemberInClassRecord(creation, index), holdsOneFunction, isLaidFirst));
                ++(side == ClassSide::Static ? data.numberOfStaticMethods : data.numberOfMethods);
            }
        }
    }

    const Vector<PlacedVariable>& variables = encoder.numbers.statics().variables;
    for (const Typed::StaticField& field : layout.staticFields) {
        if (!field.key)
            continue;
        FieldData declared { };
        declared.name = nameInClassRecord(encoder, field.key, "a static field in the record of"_s, classLayout);
        if (field.variable.index() < variables.size() && variables[field.variable.index()].hasWord && variables[field.variable.index()].plan.representation == SlotRepresentation::JSValue)
            declared.slot = SlotData::create(variables[field.variable.index()].offset, SlotRepresentation::JSValue);
        else
            encoder.fail("StaticFieldWithoutBoxedVariable"_s, classLayout);
        parts.staticFields.append(declared);
    }

    Typed::ParentInClassRecord parent = Typed::parentNamedByClassRecord(program, encoder.facts, classLayout);
    if (parent.isProven && (!parent.layout || !isRankedAmongClassRecords(encoder, parent.layout)))
        encoder.fail("ProvenParentOfClassHasNoRecord"_s, classLayout);
    if (isRankedAmongClassRecords(encoder, classLayout) && parent.layout && program.at(parent.layout).kind == LayoutKind::Instance)
        parts.parentThatListsClass = parent.layout;
    data.base = (parent.isProven ? parent.layout : LayoutID()).index();
    data.expectedBase = (parent.isProven ? LayoutID() : parent.layout).index();
    data.classConstructor = classConstructor.value_or(FunctionID()).index();
    data.environment = environment.index();
    data.numberOfStaticFields = static_cast<uint32_t>(parts.staticFields.size());

    uint32_t guardOfPrototype = guardBitInClassRecord(encoder, classLayout, ClassSide::Prototype);
    uint32_t guardOfClassObject = guardBitInClassRecord(encoder, classLayout, ClassSide::Static);
    if ((guardOfPrototype != noClassGuardBit || guardOfClassObject != noClassGuardBit) && !isRankedAmongClassRecords(encoder, classLayout))
        encoder.fail("CodeTestsGuardOfClassWithoutRank"_s, classLayout);
    data.guardBit[static_cast<unsigned>(ClassSide::Prototype)] = guardOfPrototype;
    data.guardBit[static_cast<unsigned>(ClassSide::Static)] = guardOfClassObject;
    for (ClassSide side : { ClassSide::Prototype, ClassSide::Static })
        parts.namedGuards[static_cast<unsigned>(side)] = collectGuardsOfNamesInClassRecord(encoder, classLayout, side);
    data.guardsHoldWhileClear = encoder.plan.classGuardsHoldWhileClear(classLayout);
    data.guardsOfPrototypeSideHoldWhileClear = encoder.plan.bitsOfClassGuardsAreClearWhileTheyHold(classLayout, ClassSide::Prototype);
    data.guardsOfNamesAwaitBeginning = guardsOfNamesAwaitBeginningOf(program, encoder.plan, classLayout);
    for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
        if (isClosedClassSide(encoder, classLayout, side))
            data.sidesChangedInSightAlone |= 1u << static_cast<unsigned>(side);
    }
    const Typed::LinkTables& linked = encoder.facts.linked();
    data.namesAreLaidUnderWrittenKeysAlone = !std::ranges::binary_search(linked.namesOnPrototypes, std::pair { classLayout, PropertyKeyID() }, { }, [](const Typed::LinkedNameOnPrototype& laid) {
        return std::pair { laid.layout, laid.key };
    });
    data.classObjectLacksHasInstance = data.isChangedInSightAlone(ClassSide::Static) && encoder.classesWhoseClassObjectsFindNoHasInstance.quickGet(classLayout.index());
    data.privateNamesAreKnownToTexts = encoder.classesWhosePrivateNamesTextsKnow.quickGet(classLayout.index());
    data.hasComputedMemberNames = std::ranges::any_of(parts.methods, [](const MethodData& method) {
        return !!(method.flags & methodHasComputedName);
    });
    return parts;
}

void appendListsOfClassRecord(TableEncoder& encoder, LayoutID classLayout, const std::array<Vector<uint32_t>, numberOfClassSides>& namedGuards, ClassData& data)
{
    for (ClassSide side : { ClassSide::Prototype, ClassSide::Static }) {
        const Vector<uint32_t>& guards = namedGuards[static_cast<unsigned>(side)];
        data.namedGuardsOffset[static_cast<unsigned>(side)] = guards.isEmpty() ? 0 : encoder.part.append(guards);
        if (encoder.sidesWithClassGuardsHereOrBelow.get(placeOfSideOfClass(classLayout, side)))
            data.sidesWithGuardsHereOrBelow |= 1u << static_cast<unsigned>(side);
    }
    if (std::span<const uint32_t> subclasses = classesBelowInClassRecords(encoder, classLayout); !subclasses.empty()) {
        Vector<uint32_t, 8> list;
        list.reserveInitialCapacity(subclasses.size() + 1);
        list.append(static_cast<uint32_t>(subclasses.size()));
        list.append(subclasses);
        data.subclassesOffset = encoder.part.append(list);
    }
    if (const Typed::Layout& layout = encoder.program.at(classLayout); Typed::isKnownByStructureOfClass(layout)) {
        Vector<uint32_t, 8> list;
        list.reserveInitialCapacity(layout.fields.size() + 2);
        list.append(Typed::builtinClassOfInstances(layout).index() | (layout.mixedBuiltinBase ? PropertiesOfHeirsData::extendsMixin : 0));
        list.append(static_cast<uint32_t>(layout.fields.size()));
        for (const Typed::Field& field : layout.fields)
            list.append(nameInClassRecord(encoder, field.key, "a property of the instances of"_s, classLayout) | (Typed::isToldByStructureOfClass(field) ? 0 : PropertiesOfHeirsData::mayBeStoredLate));
        data.propertiesOfHeirsOffset = encoder.part.append(list);
    }
}

static uint32_t closureCellForClassRow(const TableEncoder& encoder, FunctionID function)
{
    uint32_t cell = function ? encoder.numbers.closureCellNumber(function) : 0;
    return cell <= ClassRowEntry::maxNumber ? cell : 0;
}

std::optional<Vector<DeclaredClassMember>> collectDeclaredClassMembers(const TableEncoder& encoder, LayoutID classLayout)
{
    const Typed::Program& program = encoder.program;
    const Typed::Layout& layout = program.at(classLayout);
    const Typed::ClassCreationPlan& creation = encoder.plan.slots().classCreation(classLayout);
    const IdentifierNumbering& identifiers = encoder.numbers.identifiers();
    if (!creation.createsMembersLazily || !layout.constructor || !encoder.findEntrypoint(layout.constructor))
        return std::nullopt;

    std::span<const Typed::ClassMember> members = encoder.facts.membersOf(classLayout);
    Vector<DefinitionInClassRow> definitions;
    for (size_t index = 0; index < members.size(); ++index) {
        const Typed::ClassMember& member = members[index];
        if (member.side != ClassSide::Prototype)
            continue;
        uint32_t cell = closureCellForClassRow(encoder, member.function);
        if (!member.key || creationOfMemberInClassRecord(creation, index) != Typed::MemberCreation::ByClassRecord || !cell)
            return std::nullopt;
        uint32_t name = nameInClassRecord(encoder, member.key, "a member in the row of"_s, classLayout);
        if (identifiers.hasFieldTableColumn(name))
            definitions.append({ name, member.kind, cell });
    }

    AOT::stableSort(definitions, { }, &DefinitionInClassRow::name);
    Vector<DeclaredClassMember> declared;
    for (const DefinitionInClassRow& definition : definitions) {
        if (declared.isEmpty() || declared.last().name != definition.name)
            declared.append(DeclaredClassMember { definition.name });
        DeclaredClassMember& member = declared.last();
        switch (definition.kind) {
        case MethodKind::Method:
            member = DeclaredClassMember { definition.name, definition.cell };
            break;
        case MethodKind::Getter:
            member.method = 0;
            member.getter = definition.cell;
            break;
        case MethodKind::Setter:
            member.method = 0;
            member.setter = definition.cell;
            break;
        }
    }

    PropertyKeyID keyOfConstructor = program.keyOf(BuiltinNames::constructor);
    std::optional<uint32_t> nameOfConstructor = keyOfConstructor ? identifiers.numberOf(keyOfConstructor) : std::nullopt;
    if (nameOfConstructor && identifiers.hasFieldTableColumn(*nameOfConstructor) && !std::ranges::binary_search(declared, *nameOfConstructor, { }, &DeclaredClassMember::name)) {
        uint32_t cell = closureCellForClassRow(encoder, layout.constructor);
        if (!cell)
            return std::nullopt;
        declared.append(DeclaredClassMember { *nameOfConstructor, cell });
        AOT::sort(declared, { }, &DeclaredClassMember::name);
    }
    return declared;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
