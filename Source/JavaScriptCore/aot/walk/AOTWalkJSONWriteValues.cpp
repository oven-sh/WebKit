/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What JSONTextWriter writes for a value by its plan: undefined and null, scalars, dates, unions arm by arm, an object of a layout member by member or by the serializer of its layout.
// Every test of an object stands in front of its first byte, the key that it stands under included. At a suspension and at a throw: AOTWalkJSONWrites.cpp. Withheld: nobody makes a writer.

#include "config.h"
#include "AOTWalkJSONWrites.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTCallingConventions.h"
#include "AOTEffects.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTSlotPlan.h"
#include "AOTStringBuilderWords.h"
#include "AOTTypeArena.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

// An integer takes no more of the reservation than a number does.
static_assert(jsonIntegerBytes <= jsonNumberBytes);

// ---- Values

void JSONTextWriter::appendValue(Reference value, const JSONValuePlan& plan, const PositionOfJSONText& position)
{
    if (hasStopped())
        return;
    if (!plan.mayBeNullish || plan.kind == JSONValueKind::Generic) {
        appendPlannedValue(value, plan, position);
        return;
    }
    // Undefined and null are among what fails the test of the plan, which appendGeneric() writes. At the root nothing is left to it: there they get arms where they are proven to come, and go by the call as written where the plan is a guess.
    if (plan.kind != JSONValueKind::Nullish && (position.kind != JSONPosition::Root || plan.isExpected)) {
        appendPlannedValue(value, plan, position);
        return;
    }

    BlockID done = openMerge();
    for (TypeID nullish : { TypeID(TypeTable::undefined), TypeID(TypeTable::null) }) {
        Reference tested = value;
        ArmOfJSONText arm = armOfTypeTest(tested, nullish);
        if (arm.matches == AnswerOfJSONTest::No)
            continue;

        if (nullish == TypeTable::null || position.kind == JSONPosition::Element) {
            appendKeyOf(position);
            appendToken(JSONToken::Null);
            appendCommaOf(position);
        } else if (position.kind == JSONPosition::Root)
            takeSlowPath();
        jumpTo(done);
        if (!continueWithOtherValues(arm, value)) {
            continueAt(done);
            return;
        }
    }

    appendPlannedValue(value, plan, position);
    jumpTo(done);
    continueAt(done);
}

void JSONTextWriter::appendPlannedValue(Reference value, const JSONValuePlan& plan, const PositionOfJSONText& position)
{
    if (hasStopped())
        return;
    switch (plan.kind) {
    case JSONValueKind::Generic:
    case JSONValueKind::Nullish:
        appendGeneric(value, position);
        return;
    case JSONValueKind::Objects:
        appendObjects(value, plan, position);
        return;
    case JSONValueKind::ObjectParts:
        appendObjectParts(value, plan, position);
        return;
    case JSONValueKind::Scalar:
    case JSONValueKind::Array:
        break;
    }

    RELEASE_ASSERT(plan.tested);
    Reference tested = value;
    ArmOfJSONText arm = armOfTypeTest(tested, plan.tested, plan.isExpected ? BlockFrequency::Rare : BlockFrequency::Normal);
    if (arm.matches == AnswerOfJSONTest::No) {
        appendGeneric(value, position);
        return;
    }
    if (arm.matches == AnswerOfJSONTest::Yes) {
        appendTestedValue(value, plan, position);
        return;
    }

    BlockID done = openMerge();
    appendTestedValue(tested, plan, position);
    jumpTo(done);
    if (continueWithOtherValues(arm, value)) {
        appendGeneric(value, position);
        jumpTo(done);
    }
    continueAt(done);
}

// The value is proven to have the type that the plan tests for, so it is taken in the form of that type: nothing is boxed, and nothing looks at the value to find out what it is.
void JSONTextWriter::appendTestedValue(Reference value, const JSONValuePlan& plan, const PositionOfJSONText& position)
{
    if (hasStopped())
        return;
    if (plan.kind == JSONValueKind::Array) {
        appendArray(value, plan, position);
        return;
    }
    if (plan.row == Opcode::JSONAppendDate) {
        appendDate(value, position);
        return;
    }

    // A number further from zero than an exact integer may be written with an exponent.
    Opcode row = plan.row;
    if (row == Opcode::JSONAppendNumber) {
        Type type = typeOf(value);
        if (type.isInteger() && type.integers.isSubsetOf(Range::exact()))
            row = Opcode::JSONAppendInteger;
    }
    appendKeyOf(position);
    Draft scalar = draft(row);
    scalar.setValueOperand(value);
    append(WTF::move(scalar));
    appendCommaOf(position);
}

// No plan. A primitive is written in place, and anything else is turned away.
void JSONTextWriter::appendGeneric(Reference value, const PositionOfJSONText& position)
{
    if (hasStopped())
        return;
    if (position.kind == JSONPosition::Root) {
        takeSlowPath();
        return;
    }

    BlockID done = openMerge();
    Reference tested = value;
    ArmOfJSONText nullArm = armOfTypeTest(tested, TypeTable::null);
    if (nullArm.matches != AnswerOfJSONTest::No) {
        appendKeyOf(position);
        appendToken(JSONToken::Null);
        appendCommaOf(position);
        jumpTo(done);
        if (!continueWithOtherValues(nullArm, value)) {
            continueAt(done);
            return;
        }
    }

    tested = value;
    ArmOfJSONText primitiveArm = armOfTypeTest(tested, TypeArena::fromPrimitives(primitivesWithJSONText));
    if (primitiveArm.matches != AnswerOfJSONTest::No) {
        appendKeyOf(position);
        Draft primitive = draft(Opcode::JSONAppendValue);
        primitive.setJsonPosition(JSONPosition::Element);
        primitive.setValueOperand(tested);
        append(WTF::move(primitive));
        // A string, a number, a boolean or null always has text.
        takeResultsOfJSONAppendValue(false);
        appendCommaOf(position);
        jumpTo(done);
        if (!continueWithOtherValues(primitiveArm, value)) {
            continueAt(done);
            return;
        }
    }

    if (hasNoJSONText(m_context, m_walker.typeOf(value))) {
        if (position.kind == JSONPosition::Element)
            appendToken(JSONToken::Null);
    } else {
        tested = value;
        ArmOfJSONText undefinedArm = armOfTypeTest(tested, TypeTable::undefined, BlockFrequency::Rare);
        if (undefinedArm.matches != AnswerOfJSONTest::No) {
            if (position.kind == JSONPosition::Element)
                appendToken(JSONToken::Null);
            jumpTo(done);
            if (!continueWithOtherValues(undefinedArm, value)) {
                continueAt(done);
                return;
            }
        }
        takeSlowPath();
    }
    jumpTo(done);
    continueAt(done);
}

// ---- Instances of the engine's classes

void JSONTextWriter::requirePristine(Reference value, BuiltinClassID builtinClass, PristineAspectSet aspects)
{
    if (hasStopped())
        return;
    PristineAspectSet known = pristineAspectsProvenBy(builtinTypeContextOf(m_context), m_walker.typeOf(value), builtinClass);
    if (isListAsMade(m_walker, value))
        known.add({ PristineAspects::Storage, PristineAspects::Names, PristineAspects::NamesKeptBelow, PristineAspects::Prototype });
    PristineAspectSet untested = aspects - known;
    if (untested.isEmpty())
        return;

    Draft test = draft(Opcode::IsPristine);
    test.setValueOperand(value);
    test.setBuiltinClass(builtinClass);
    test.setAspects(untested);
    requireOrTakeSlowPath(build(WTF::move(test)));
    if (!hasStopped() && untested.containsAll(aspectsOfArrayWrittenAsJSON))
        noteListAsMade(m_walker, value);
}

// A holder adds some members when they are first read, which shows where its keys are listed.
bool JSONTextWriter::firstReadIsSeen(BuiltinID location)
{
    const BuiltinTable& table = m_context.program.builtins();
    if (isMadeWithCodeOfLibrary(table, location))
        return true;
    const Builtin& entry = table.at(location);
    return entry.isAddedAtFirstRead && entry.holder && m_walker.facts().listsKeysOf(table.canonicalID(entry.holder));
}

void JSONTextWriter::buildFirstReadOf(BuiltinID location)
{
    if (hasStopped() || !firstReadIsSeen(location))
        return;
    if (m_walker.has(Mechanism::Folding) && m_walker.state().hasReadBuiltin(location))
        return;
    Draft initialization = draft(Opcode::InitializeBuiltin);
    initialization.setBuiltin(location);
    build(WTF::move(initialization));
    if (!hasStopped())
        m_walker.state().noteReadOfBuiltin(location);
}

// The host reads toJSON and valueOf by name and, for a valid date alone, toISOString.
void JSONTextWriter::appendFirstReadsOfMembersOfDate(Reference time)
{
    buildFirstReadOf(Builtins::DatePrototypeToJSON);
    buildFirstReadOf(Builtins::DatePrototypeValueOf);
    BuiltinID toISOString = Builtins::DatePrototypeToISOString;
    if (hasStopped() || !firstReadIsSeen(toISOString) || (m_walker.has(Mechanism::Folding) && m_walker.state().hasReadBuiltin(toISOString)))
        return;

    BlockID done = openMerge();
    ArmOfJSONText isValid = armOfTest(buildOperator(Opcode::StrictEq, time, time), BlockFrequency::Rare);
    if (isValid.matches != AnswerOfJSONTest::No) {
        buildFirstReadOf(toISOString);
        jumpTo(done);
        if (!continueWithOtherValues(isValid)) {
            continueAt(done);
            return;
        }
    }
    jumpTo(done);
    continueAt(done);
}

void JSONTextWriter::appendDate(Reference date, const PositionOfJSONText& position)
{
    constexpr bool writesDatesInPlace = true;
    if (!writesDatesInPlace) {
        takeSlowPath();
        return;
    }
    requirePristine(date, BuiltinClasses::Date, PristineAspectSet { PristineAspects::Names });
    if (hasStopped())
        return;

    // The function is called whatever its name finds by now: the host reads the time out of the cell.
    const BuiltinTable& table = m_context.program.builtins();
    BuiltinOverloadID getTime = findGetTimeForJSON(m_context);
    const BuiltinOverload* overload = getTime ? &table.at(getTime) : nullptr;
    std::optional<InlineBuiltin> codeInLine = overload ? overload->code.implementation.inlineBuiltin : std::nullopt;
    BuiltinOperandTypes received;
    received.receiver = m_walker.typeOf(date);
    bool isHiddenInStackTraces = m_context.program.at(m_walker.currentFunction()).isHiddenInStackTraces();
    if (!codeInLine || chooseInlineForm({ m_context, *overload, *codeInLine, BuiltinCallForm::Whole, received, !!overload->code.implementation.otherwise, false, isHiddenInStackTraces }) != InlineForm::Whole) {
        takeSlowPath();
        return;
    }

    appendKeyOf(position);
    Vector<uint32_t, 4> parameters;
    for (uint8_t parameter : std::span(overload->code.implementation.inlineParameters).first(numberOfParametersOf(*codeInLine)))
        parameters.append(parameter);
    Draft call = draft(Opcode::CallInline);
    call.setInlineBuiltin(*codeInLine);
    call.setInlineParameters(parameters.span());
    call.setOverload(getTime);
    call.setReceiverOperand(date);
    Effects readsTime = Effects::none();
    readsTime.reads.add(AbstractHeap::InternalFields);
    Reference time = build(WTF::move(call), &readsTime);
    if (hasStopped())
        return;
    if (!time || !typeOf(time).isNumber()) {
        takeSlowPath();
        return;
    }
    appendFirstReadsOfMembersOfDate(time);

    Draft text = draft(Opcode::JSONAppendDate);
    text.setValueOperand(time);
    append(WTF::move(text));
    appendCommaOf(position);
}

// ---- Objects

void JSONTextWriter::appendObjectParts(Reference value, const JSONValuePlan& plan, const PositionOfJSONText& position)
{
    BlockID done = openMerge();
    for (const JSONValuePlan& part : plan.parts) {
        if (hasStopped())
            break;
        if (part.kind == JSONValueKind::Objects) {
            appendObjects(value, part, position);
            jumpTo(done);
            continueAt(done);
            return;
        }

        Reference tested = value;
        ArmOfJSONText arm = armOfTypeTest(tested, part.tested);
        if (arm.matches == AnswerOfJSONTest::No)
            continue;
        appendTestedValue(tested, part, position);
        jumpTo(done);
        if (!continueWithOtherValues(arm, value)) {
            continueAt(done);
            return;
        }
    }
    appendGeneric(value, position);
    jumpTo(done);
    continueAt(done);
}

void JSONTextWriter::appendObjects(Reference value, const JSONValuePlan& plan, const PositionOfJSONText& position)
{
    RELEASE_ASSERT(!plan.layouts.isEmpty());
    BlockID done = openMerge();
    for (LayoutID layout : plan.layouts) {
        if (hasStopped())
            break;
        std::optional<JSONObjectPlan> object = m_walker.plan().isInUse(layout) ? planJSONObject(m_context, layout) : std::nullopt;
        if (!object)
            continue;

        Reference tested = value;
        ArmOfJSONText arm = armOfShapeTest(tested, layout);
        if (arm.matches == AnswerOfJSONTest::No)
            continue;
        uint32_t members = numberOfJSONMembersWrittenOutFor(m_context, *object);
        if (m_layoutsAround.contains(layout) || members > m_membersLeft || (!m_buildsLoops && holdsListAmongJSONMembers(m_context, *object)))
            appendObjectByCall(tested, layout, position);
        else {
            m_membersLeft -= members;
            appendObjectOfLayout(tested, *object, position);
        }
        jumpTo(done);
        if (!continueWithOtherValues(arm, value)) {
            continueAt(done);
            return;
        }
    }
    appendGeneric(value, position);
    jumpTo(done);
    continueAt(done);
}

// What has to be seen of the cell beside its shape and the order of its keys. The shape word is written once, so nothing of this shows in it.
void JSONTextWriter::lookAtCell(Reference object, const JSONObjectPlan& plan)
{
    WalkState& state = m_walker.state();
    OptionSet<FactOfCell> asked = plan.looksAtCell;
    // A guard of a class speaks of what stands above the prototype object of the class.
    if (!plan.classesGuardedAgainstToJSON.isEmpty())
        asked.add(FactOfCell::HasLayoutPrototype);
    asked.remove(factsKnownOfCell(object));

    // A name that the layout has no field for, an index, and a field that has left its slot (an accessor was laid over it, it is listed no more, it got a value that its slot cannot hold) lie among the extra properties.
    // One test asks for both. A frozen object passes it, so the state is not told that it knows the whole order word.
    if (asked.containsAny({ FactOfCell::NoFieldOutsideSlot, FactOfCell::OrderWordIsKnown })) {
        Draft hasNothingElse = draft(Opcode::HasNoExtraProperties);
        hasNothingElse.setObjectOperand(object);
        hasNothingElse.setHasNoFieldOutsideSlot(true);
        requireOrTakeSlowPath(build(WTF::move(hasNothingElse)));
        if (hasStopped())
            return;
        state.noteFactsOfCell(object, FactOfCell::NoFieldOutsideSlot);
    }
    if (asked.contains(FactOfCell::NotFrozen)) {
        Draft isNotFrozen = draft(Opcode::IsNotFrozen);
        isNotFrozen.setObjectOperand(object);
        requireOrTakeSlowPath(build(WTF::move(isNotFrozen)));
        if (hasStopped())
            return;
        state.noteFactsOfCell(object, FactOfCell::NotFrozen);
    }
    if (asked.contains(FactOfCell::HasLayoutPrototype)) {
        Draft hasLayoutPrototype = draft(Opcode::HasLayoutPrototype);
        hasLayoutPrototype.setObjectOperand(object);
        requireOrTakeSlowPath(build(WTF::move(hasLayoutPrototype)));
        if (hasStopped())
            return;
        state.noteFactsOfCell(object, FactOfCell::HasLayoutPrototype);
    }

    PropertyKeyID toJSON = m_context.program.keyOf(BuiltinNames::toJSON);
    Vector<MemberConditionRecord, 2> conditions;
    for (LayoutID classLayout : plan.classesGuardedAgainstToJSON) {
        if (!state.guardOfReadsHolds(classLayout, toJSON))
            conditions.append(MemberConditionRecord { MemberConditionKind::MemberUnchanged, classLayout, ClassSide::Prototype, toJSON });
    }
    if (conditions.isEmpty())
        return;
    Draft areUnchanged = draft(Opcode::AreMembersUnchanged);
    areUnchanged.setConditions(conditions.span());
    Reference passed = build(WTF::move(areUnchanged));
    requireOrTakeSlowPath(passed);
    if (hasStopped())
        return;
    for (const MemberConditionRecord& condition : conditions)
        state.noteGuardOfReadsHolds(condition.layout, condition.key, passed);
}

// The shape is proven.
void JSONTextWriter::appendObjectOfLayout(Reference object, const JSONObjectPlan& plan, const PositionOfJSONText& position)
{
    if (hasStopped())
        return;
    Type type = typeOf(object);
    if (type.primitives || !type.hasOnlyLayouts() || singleLayoutOf(TypeReader(types()), type) != plan.layout) {
        takeSlowPath();
        return;
    }

    LevelScope level(*this);
    SetForScope liesBetweenLists(m_firstListDirectlyAround, static_cast<uint32_t>(m_listsAround.size()));
    lookAtCell(object, plan);
    if (hasStopped())
        return;

    BlockID done = openMerge();
    m_layoutsAround.append(plan.layout);
    for (const JSONKeyOrderPlan& order : plan.orders) {
        if (hasStopped())
            break;
        ArmOfJSONText hasOrder = armOfKeyOrderTest(object, plan.layout, order.created.span());
        if (hasOrder.matches == AnswerOfJSONTest::No)
            continue;

        // The order says which fields the object has, and none of them lies outside its slot: each read finds its slot filled.
        appendKeyOf(position);
        MemberListOfJSONText memberList = notOpened;
        for (const JSONMemberPlan& member : order.members)
            appendMember(object, plan.layout, member, order.created.contains(member.field), memberList);
        appendToken(memberList.closing);
        appendCommaOf(position);
        jumpTo(done);
        if (!continueWithOtherValues(hasOrder)) {
            m_layoutsAround.removeLast();
            continueAt(done);
            return;
        }
    }
    // An object in an order that has no arm.
    takeSlowPath();
    m_layoutsAround.removeLast();
    continueAt(done);
}

// The shape is proven. The serializer asks the object whatever else there is to ask, and what it has written by the time it turns an object away is dropped with the rest of the text.
void JSONTextWriter::appendObjectByCall(Reference object, LayoutID layout, const PositionOfJSONText& position)
{
    if (hasStopped())
        return;
    if (!m_mayCallSerializers || !m_walker.plan().conventions().serializerOf(layout) || !types().isSubset(m_walker.typeOf(object), types().forLayout(layout))) {
        takeSlowPath();
        return;
    }
    Reference levels = levelsOpenHere();
    if (hasStopped())
        return;
    if (!levels || !typeOf(levels).isInteger()) {
        takeSlowPath();
        return;
    }

    appendKeyOf(position);
    // No cursor crosses a call.
    commit();
    Draft call = draft(Opcode::JSONAppendObject);
    call.setLayout(layout);
    call.setBuilderOperand(m_builder);
    call.setObjectOperand(object);
    call.setLevelsOperand(levels);
    Reference outcome = build(WTF::move(call));
    if (hasStopped())
        return;
    requireOrTakeSlowPath(buildOperator(Opcode::StrictEq, outcome, integer(static_cast<int64_t>(JSONOutcome::Parsed))));
    appendCommaOf(position);
}

void JSONTextWriter::appendMember(Reference object, LayoutID layout, const JSONMemberPlan& member, bool isFilled, MemberListOfJSONText& memberList)
{
    if (hasStopped())
        return;
    // What lies in such a slot is no value.
    const PlacedSlot& slot = m_walker.plan().slot(layout, member.field);
    if (slot.holdsAccessor || slot.deferredFunction || !slot.hasStorage()) {
        takeSlowPath();
        return;
    }

    Reference value = m_walker.state().slot(object, FieldReference { layout, member.field });
    if (!value)
        value = m_walker.state().slot(firstNameOf(object), FieldReference { layout, member.field });
    if (!value || !value.isRecord()) {
        Draft read = draft(isFilled ? Opcode::GetSlot : Opcode::GetSlotOrUndefined);
        read.setObjectOperand(object);
        read.setKey(member.key);
        read.setReceivers(LayoutSets::of(layout));
        read.setLevel(KnowledgeLevel::Proven);
        value = build(WTF::move(read));
    }
    if (hasStopped())
        return;
    if (!value) {
        takeSlowPath();
        return;
    }

    // Nothing reads what was read, so the sweep drops it: the member costs nothing.
    TypeID proven = m_walker.typeOf(value);
    if (hasNoJSONText(m_context, proven))
        return;

    JSONValuePlan plan = planJSONValue(m_context, proven, writtenTypeOfJSONMember(m_context, layout, member.field));
    Type type = types().at(proven);
    bool mayBeOmitted = plan.kind == JSONValueKind::Generic || plan.isExpected || (type.primitives & primitivesWithoutJSONText) || type.hasObjects();
    if (mayBeOmitted && memberList.separator) {
        appendToken(*memberList.separator);
        memberList = afterSeparator;
    }

    appendValue(value, plan, PositionOfJSONText { JSONPosition::Property, member.key, memberList });
    if (memberList.separator == JSONToken::OpenObject)
        memberList = afterMember;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
