/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

static_assert(jsonIntegerBytes <= jsonNumberBytes);

void JSONTextWriter::appendValue(Reference value, const JSONValuePlan& plan, const PositionOfJSONText& position)
{
    if (hasStopped())
        return;
    if (!plan.mayBeNullish || plan.kind == JSONValueKind::Generic) {
        appendPlannedValue(value, plan, position);
        return;
    }
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

void JSONTextWriter::appendTestedValue(Reference value, const JSONValuePlan& plan, const PositionOfJSONText& position)
{
    if (hasStopped())
        return;
    if (plan.kind == JSONValueKind::Array) {
        appendArray(value, plan, position);
        return;
    }

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

void JSONTextWriter::requirePristine(Reference value, BuiltinClassID builtinClass, PristineAspectSet aspects)
{
    if (hasStopped())
        return;
    PristineAspectSet known;
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

void JSONTextWriter::lookAtCell(Reference object, const JSONObjectPlan& plan)
{
    WalkState& state = m_walker.state();
    OptionSet<FactOfCell> asked = plan.looksAtCell;
    if (!plan.classesGuardedAgainstToJSON.isEmpty())
        asked.add(FactOfCell::HasLayoutPrototype);
    asked.remove(factsKnownOfCell(object));

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
    takeSlowPath();
    m_layoutsAround.removeLast();
    continueAt(done);
}

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
