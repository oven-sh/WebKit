/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONSerializerBuilder.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinPristine.h"
#include "AOTEffectsDescription.h"
#include "AOTKnownValue.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTTypeTestSite.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesWithJSONText = Primitive::String | Primitive::Number | Primitive::Boolean | Primitive::Null;
static constexpr uint32_t cursorResultOfJSONAppendValue = 1;

static bool canNarrow(const Program& program, TypeArena& types, const Type& type, const Type& tested)
{
    Type narrowed = type.intersectWith(tested, types);
    return !narrowed.isNever() && narrowed.isSubsetOf(tested, program);
}

static BuiltinOverloadID findInstanceOverload(const Emission& emission, BuiltinClassID builtinClass, BuiltinName name, BuiltinCallMode mode)
{
    BuiltinMemberLookup lookup = emission.program().builtins().findForInstance(builtinClass, name);
    if (lookup.outcome != BuiltinMemberLookup::Outcome::Found)
        return { };

    TypingContext typing { emission.program(), emission.function(), emission.inputs().types };
    BuiltinOperandTypes operands { typing.types.intern(Type::builtinClass(builtinClass)), { } };
    OverloadMatch match = matchOverload({ typing, lookup.member, mode, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty())
        return { };
    return match.overload;
}

namespace {

struct TypeArm {
    KnownBoolean matches { KnownBoolean::Unknown };
    ValueID narrowed;
    ValueID remaining;
    BlockID otherwise;
};

} // anonymous namespace

static TypeArm beginTypeArm(Emission& emission, ValueID value, TypeID tested, BlockFrequency otherwise)
{
    const Program& program = emission.program();
    TypeArm arm { evaluateTypeTest(program, emission.typeOf(value), program.at(tested)), value, value, { } };
    if (arm.matches != KnownBoolean::Unknown)
        return arm;

    IRBuilder& builder = emission.builder();
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(value, tested), testedOperandIndex, BlockFrequency::Normal, otherwise);
    arm.narrowed = branch.provenOnTaken;
    arm.remaining = branch.provenOnNotTaken;
    arm.otherwise = branch.notTaken;
    return arm;
}

JSONSerializerBuilder::JSONSerializerBuilder(IRBuilder& builder, const EmissionInputs& inputs, ValueID stringBuilder)
    : Emission(builder, inputs)
    , m_stringBuilder(stringBuilder)
{
}

void JSONSerializerBuilder::reserve(uint32_t bytes)
{
    RELEASE_ASSERT(bytes <= maxStringBuilderReservation);
    commit();

    uint64_t size = std::max<uint64_t>(bytes, inputs().build.limit(LimitID::MinRequestedJSONCapacity));
    RELEASE_ASSERT(size <= maxStringBuilderReservation);

    Value reservation = IRBuilder::make(Opcode::StringBuilderReserve, { m_stringBuilder });
    reservation.setSize(static_cast<uint32_t>(size));
    m_cursor = { builder().append(WTF::move(reservation)), static_cast<uint32_t>(size) };
}

void JSONSerializerBuilder::ensureCursor(uint32_t bytes)
{
    if (!hasCursor() || m_cursor.reservedBytes < bytes)
        reserve(bytes);
}

void JSONSerializerBuilder::append(Value&& cursorRow)
{
    Opcode opcode = cursorRow.opcode;
    RELEASE_ASSERT(opcode != Opcode::JSONAppendValue);

    uint32_t taken = bytesTakenFromReservation(program(), cursorRow);
    ensureCursor(taken);
    cursorRow.setBuilderOperand(m_stringBuilder);
    cursorRow.setCursorOperand(m_cursor.value);
    ValueID cursor = builder().append(WTF::move(cursorRow));
    m_cursor = { cursor, renewsReservation(opcode) ? maxStringBuilderReservation : m_cursor.reservedBytes - taken };
}

void JSONSerializerBuilder::appendToken(JSONToken token)
{
    Value row = IRBuilder::make(Opcode::JSONAppendToken);
    row.setJsonToken(token);
    append(WTF::move(row));
}

void JSONSerializerBuilder::appendText(ValueID string)
{
    commit();
    builder().append(Opcode::StringBuilderAppend, { m_stringBuilder, string });
}

void JSONSerializerBuilder::commit()
{
    if (!hasCursor())
        return;
    builder().append(Opcode::StringBuilderCommit, { m_stringBuilder, m_cursor.value });
    m_cursor = { };
}

auto JSONSerializerBuilder::createMergePoint(BlockFrequency frequency) -> MergePoint
{
    return { .frequency = rarerOf(frequency, builder().function().at(builder().currentBlock()).frequency) };
}

void JSONSerializerBuilder::jumpTo(MergePoint& merge, ValueID guard)
{
    if (!isInBlock())
        return;
    ensureCursor(0);
    if (!merge.block) {
        merge.block = builder().addBlock(merge.frequency);
        merge.cursor = builder().addParameter(merge.block, inputs().types.intern(Type::forOpaque(OpaqueKind::BuilderCursor)));
        if (guard)
            merge.guard = builder().addParameter(merge.block, TypeTable::guard);
    }
    RELEASE_ASSERT(!guard == !merge.guard);
    merge.reservedBytes = std::min(merge.reservedBytes, m_cursor.reservedBytes);
    if (guard)
        builder().emitJump(merge.block, { m_cursor.value, guard });
    else
        builder().emitJump(merge.block, { m_cursor.value });
    m_cursor = { };
}

void JSONSerializerBuilder::continueAt(const MergePoint& merge)
{
    if (!merge.block) {
        RELEASE_ASSERT(!isInBlock());
        return;
    }
    builder().moveToEndOf(merge.block);
    m_cursor = { merge.cursor, merge.reservedBytes };
}

void JSONSerializerBuilder::leaveAfterUnknownCode()
{
    if (!m_afterUnknownCode || !isInBlock())
        return;
    BlockFrequency frequency = builder().function().at(builder().currentBlock()).frequency;
    jumpTo(*m_afterUnknownCode);
    Block& block = builder().function().edit(m_afterUnknownCode->block);
    block.frequency = moreFrequentOf(block.frequency, frequency);
}

// The host and a serializer run for a value what JSON.stringify runs for it.
void JSONSerializerBuilder::leaveIfWritingRunsCode(ValueID written)
{
    EffectDescription description;
    describeOverload(program(), program().builtins().overloadIDsOf(Builtins::JSONStringify).at(0), typeOf(written), description);
    if (description.callsUnknownCode || !description.functions.isEmpty() || !description.slowPathFunctions.isEmpty())
        leaveAfterUnknownCode();
}

auto JSONSerializerBuilder::planValue(ValueID id) const -> ValuePlan
{
    const Value& value = function().at(id);
    TypeID expected = value.expected();
    return planType(program().at(value.type), expected ? &program().at(expected) : nullptr);
}

auto JSONSerializerBuilder::planType(const Type& proven, const Type* expected) const -> ValuePlan
{
    Type defined = proven.withoutNullish();
    bool mayBeNullish = proven != defined;
    if (defined.isNever())
        return { .kind = mayBeNullish ? ValueKind::Nullish : ValueKind::Generic, .certainty = TypeCertainty::Proven, .mayBeNullish = mayBeNullish };

    TypeArena& types = inputs().types;
    ValuePlan plan = planDefinedType(defined, TypeCertainty::Proven);
    ValuePlan expectedPlan;
    if (expected && (plan.kind == ValueKind::Generic || plan.kind == ValueKind::Array))
        expectedPlan = planDefinedType(expected->withoutNullish(), TypeCertainty::Expected);

    if (plan.kind == ValueKind::Array && expectedPlan.kind == ValueKind::Array) {
        TypeID element = types.intersectionOf(plan.element, expectedPlan.element);
        if (element != TypeTable::never)
            plan.element = element;
    }
    if (plan.kind == ValueKind::Generic && removePartsThatCannotPass(expectedPlan, defined))
        plan = WTF::move(expectedPlan);
    plan.mayBeNullish = mayBeNullish;
    return plan;
}

bool JSONSerializerBuilder::removePartsThatCannotPass(ValuePlan& expected, const Type& defined) const
{
    TypeArena& types = inputs().types;
    switch (expected.kind) {
    case ValueKind::Objects:
        expected.layouts.removeAllMatching([&](LayoutID layout) {
            return !canNarrow(program(), types, defined, Type::forLayout(layout, types));
        });
        return !expected.layouts.isEmpty();
    case ValueKind::ObjectParts:
        expected.parts.removeAllMatching([&](ValuePlan& part) {
            return !removePartsThatCannotPass(part, defined);
        });
        return !expected.parts.isEmpty();
    case ValueKind::Generic:
    case ValueKind::Nullish:
    case ValueKind::Boolean:
    case ValueKind::Integer:
    case ValueKind::Number:
    case ValueKind::String:
    case ValueKind::Date:
    case ValueKind::Array:
        return expected.tested && canNarrow(program(), types, defined, types.at(expected.tested));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto JSONSerializerBuilder::planDefinedType(const Type& defined, TypeCertainty certainty) const -> ValuePlan
{
    TypeArena& types = inputs().types;
    if (defined.isBoolean())
        return { .kind = ValueKind::Boolean, .certainty = certainty, .tested = TypeTable::boolean };
    if (defined.isInteger() && defined.integers.isSubsetOf(Range::exact()))
        return { .kind = ValueKind::Integer, .certainty = certainty, .tested = defined.isInt32() ? TypeTable::int32 : types.intern(Type::integer(Range::exact())) };
    if (defined.isNumber())
        return { .kind = ValueKind::Number, .certainty = certainty, .tested = TypeTable::number };
    if (defined.isAnyString())
        return { .kind = ValueKind::String, .certainty = certainty, .tested = TypeTable::string };

    ValuePlan plan { .kind = ValueKind::ObjectParts, .certainty = certainty };
    ValuePlan layouts;
    for (const UnionPart& part : unionPartsOf(defined.withOnlyObjects())) {
        switch (part.kind) {
        case UnionPartKind::Layouts:
            layouts = planLayouts(part.type, certainty);
            break;
        case UnionPartKind::BuiltinClass:
            if (ValuePlan instances = planBuiltinClass(part.type, certainty); instances.kind != ValueKind::Generic)
                plan.parts.append(WTF::move(instances));
            break;
        case UnionPartKind::OtherObjects:
        case UnionPartKind::Nullish:
        case UnionPartKind::Boolean:
        case UnionPartKind::Number:
        case UnionPartKind::String:
        case UnionPartKind::Symbol:
        case UnionPartKind::BigInt:
        case UnionPartKind::Absent:
        case UnionPartKind::Functions:
        case UnionPartKind::Builtins:
            break;
        }
    }
    if (layouts.kind != ValueKind::Generic)
        plan.parts.append(WTF::move(layouts));

    if (plan.parts.isEmpty())
        return { };
    if (plan.parts.size() == 1)
        return plan.parts.takeLast();
    return plan;
}

auto JSONSerializerBuilder::planBuiltinClass(const Type& instances, TypeCertainty certainty) const -> ValuePlan
{
    TypeArena& types = inputs().types;
    const BuiltinTable& builtins = program().builtins();
    const ClassPart* part = instances.singleBuiltinClass();
    RELEASE_ASSERT(part);
    if (pristineTestOf(builtins, part->builtinClass) == PristineTest::NeverPasses)
        return { };
    if (part->builtinClass == BuiltinClasses::Date && findInstanceOverload(*this, BuiltinClasses::Date, BuiltinNames::getTime, BuiltinCallMode::Call))
        return { .kind = ValueKind::Date, .certainty = certainty, .tested = types.intern(instances.withoutClassProofs()) };
    if (part->builtinClass == BuiltinClasses::Array)
        return { .kind = ValueKind::Array, .certainty = certainty, .tested = types.intern(instances.withoutClassProofs()), .element = part->contentsAt(builtins, ContentsSlot::Element) };
    return { };
}

auto JSONSerializerBuilder::planLayouts(const Type& objects, TypeCertainty certainty) const -> ValuePlan
{
    ValuePlan plan { .kind = ValueKind::Objects, .certainty = certainty };
    for (LayoutID layout : membersOf(program(), objects)) {
        if (jsonSupportOf(program(), inputs().flow, layout) == JSONSupport::Compiled)
            plan.layouts.append(layout);
    }
    // The order of the tests, and who gets one where there are too many.
    if (plan.layouts.size() > 1) {
        Vector<std::pair<JSONLikelihood, LayoutID>, 8> ranked(plan.layouts.size(), [&](size_t index) {
            return std::pair { jsonLikelihoodOf(program(), inputs().flow, plan.layouts[index]), plan.layouts[index] };
        });
        AOT::sort(ranked);
        for (size_t index = 0; index < ranked.size(); ++index)
            plan.layouts[index] = ranked[index].second;
    }
    BuildContext& build = inputs().build;
    uint64_t limit = build.limit(LimitID::MaxJSONLayoutSetMembers);
    if (plan.layouts.size() > limit) {
        build.gaveUp(LimitID::MaxJSONLayoutSetMembers, Subject { .function = inputs().functionAsRead, .layout = plan.layouts[limit] });
        plan.layouts.shrink(limit);
    }
    if (plan.layouts.isEmpty())
        return { };
    return plan;
}

void JSONSerializerBuilder::appendRoot(ValueID value, BlockID whenThereIsNoText)
{
    ValuePlan plan = planValue(value);
    if (plan.kind == ValueKind::Generic) {
        builder().emitJump(whenThereIsNoText);
        builder().moveToEndOf(builder().addBlock(BlockFrequency::Rare));
        return;
    }

    ValueID start = builder().append(Opcode::StringBuilderCheckpoint, { m_stringBuilder });
    appendValue(value, plan, Position { });
    commit();
    ValueID end = builder().append(Opcode::StringBuilderCheckpoint, { m_stringBuilder });
    emitJumpIf(builder().append(Opcode::StrictEq, { start, end }), whenThereIsNoText);
}

void JSONSerializerBuilder::appendValue(ValueID value, const ValuePlan& plan, const Position& position)
{
    if (!plan.mayBeNullish || plan.kind == ValueKind::Generic) {
        appendPlannedValue(value, plan, position);
        return;
    }

    MergePoint done = createMergePoint();
    Cursor start = m_cursor;
    ValueID defined = value;
    for (TypeID nullish : { TypeTable::undefined, TypeTable::null }) {
        TypeArm arm = beginTypeArm(*this, defined, nullish, BlockFrequency::Normal);
        if (arm.matches == KnownBoolean::False)
            continue;

        if (nullish == TypeTable::null || position.kind == JSONPosition::Element)
            appendNullish(position);
        jumpTo(done);
        if (!arm.otherwise) {
            continueAt(done);
            return;
        }
        builder().moveToEndOf(arm.otherwise);
        m_cursor = start;
        defined = arm.remaining;
    }

    appendPlannedValue(defined, plan, position);
    if (!done.block)
        return;
    jumpTo(done);
    continueAt(done);
}

void JSONSerializerBuilder::appendPlannedValue(ValueID value, const ValuePlan& plan, const Position& position)
{
    switch (plan.kind) {
    case ValueKind::Generic:
    case ValueKind::Nullish:
        appendGeneric(value, position);
        return;
    case ValueKind::Objects:
        appendObjects(value, plan, position);
        return;
    case ValueKind::ObjectParts:
        appendObjectParts(value, plan, position);
        return;
    case ValueKind::Boolean:
    case ValueKind::Integer:
    case ValueKind::Number:
    case ValueKind::String:
    case ValueKind::Date:
    case ValueKind::Array:
        break;
    }

    RELEASE_ASSERT(plan.tested);
    KnownBoolean hasTestedType = evaluateTypeTest(program(), typeOf(value), program().at(plan.tested));
    if (hasTestedType == KnownBoolean::False) {
        appendGeneric(value, position);
        return;
    }

    MergePoint done = createMergePoint();
    Cursor start = m_cursor;
    BlockID generic;
    auto ensureGenericBlock = [&] {
        if (!generic)
            generic = builder().addBlock(BlockFrequency::Rare);
        return generic;
    };

    ValueID narrowed = value;
    if (hasTestedType == KnownBoolean::Unknown)
        narrowed = emitTypeTest(value, plan.tested, ensureGenericBlock());
    if (plan.kind == ValueKind::Date && !pristineAspectsProvenBy(typeOf(narrowed), BuiltinClasses::Date).contains(PristineAspects::Names))
        emitJumpUnless(builder().append(makePristineTest(narrowed, BuiltinClasses::Date, PristineAspects::Names)), ensureGenericBlock());

    if (plan.kind == ValueKind::Array)
        appendArray(narrowed, plan, position);
    else {
        appendKeyOf(position);
        appendScalar(narrowed, plan);
        if (position.endsWithComma())
            appendToken(JSONToken::Comma);
    }
    if (!generic)
        return;
    jumpTo(done);

    builder().moveToEndOf(generic);
    m_cursor = start;
    appendGeneric(value, position);
    jumpTo(done);
    continueAt(done);
}

void JSONSerializerBuilder::appendKeyOf(const Position& position)
{
    if (position.kind != JSONPosition::Property)
        return;

    StringID text = program().at(position.key).text;
    std::optional<JSONToken> token = separatorAfter(position.memberList);
    if (canWriteJSONKeyAtCursor(program(), text)) {
        Value key = IRBuilder::make(Opcode::JSONAppendKey);
        key.setString(text);
        key.setOpensObject(token == JSONToken::OpenObject);
        key.setFollowsMember(token == JSONToken::Comma);
        append(WTF::move(key));
        return;
    }

    if (token)
        appendToken(*token);
    StringBuilder quotedKey;
    quotedKey.appendQuotedJSONString(program().strings.toWTFString(text));
    quotedKey.append(':');
    appendText(appendString(quotedKey.toString()));
}

void JSONSerializerBuilder::appendScalar(ValueID value, const ValuePlan& plan)
{
    auto appendRow = [&](Opcode opcode, ValueID operand) {
        Value row = IRBuilder::make(opcode);
        row.setValueOperand(operand);
        append(WTF::move(row));
    };

    switch (plan.kind) {
    case ValueKind::Boolean:
        appendRow(Opcode::JSONAppendBoolean, value);
        return;
    case ValueKind::Integer:
        appendRow(Opcode::JSONAppendInteger, value);
        return;
    case ValueKind::Number:
        appendRow(Opcode::JSONAppendNumber, value);
        return;
    case ValueKind::String:
        appendRow(Opcode::JSONAppendString, value);
        return;
    case ValueKind::Date: {
        BuiltinOverloadID getTime = findInstanceOverload(*this, BuiltinClasses::Date, BuiltinNames::getTime, BuiltinCallMode::Call);
        RELEASE_ASSERT(getTime);
        appendRow(Opcode::JSONAppendDate, appendBuiltinCall(getTime, value, { }));
        return;
    }
    case ValueKind::Generic:
    case ValueKind::Nullish:
    case ValueKind::Array:
    case ValueKind::Objects:
    case ValueKind::ObjectParts:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void JSONSerializerBuilder::appendObjectParts(ValueID value, const ValuePlan& plan, const Position& position)
{
    MergePoint done = createMergePoint();
    Cursor start = m_cursor;
    ValueID remaining = value;
    bool isWritten = false;
    for (const ValuePlan& part : plan.parts) {
        if (part.kind == ValueKind::Objects) {
            appendObjects(remaining, part, position);
            isWritten = true;
            break;
        }

        TypeArm arm = beginTypeArm(*this, remaining, part.tested, BlockFrequency::Normal);
        if (arm.matches == KnownBoolean::False)
            continue;
        appendPlannedValue(arm.narrowed, part, position);
        if (!arm.otherwise) {
            isWritten = true;
            break;
        }
        jumpTo(done);
        builder().moveToEndOf(arm.otherwise);
        m_cursor = start;
        remaining = arm.remaining;
    }

    if (!isWritten)
        appendGeneric(remaining, position);
    if (!done.block)
        return;
    jumpTo(done);
    continueAt(done);
}

void JSONSerializerBuilder::appendNullish(const Position& position)
{
    appendKeyOf(position);
    appendToken(JSONToken::Null);
    if (position.endsWithComma())
        appendToken(JSONToken::Comma);
}

void JSONSerializerBuilder::appendGeneric(ValueID value, const Position& position)
{
    if (position.kind == JSONPosition::Root)
        return;

    MergePoint done = createMergePoint();
    Cursor start = m_cursor;
    auto continuesAfter = [&](const TypeArm& arm) {
        jumpTo(done);
        if (!arm.otherwise) {
            continueAt(done);
            return false;
        }
        builder().moveToEndOf(arm.otherwise);
        m_cursor = start;
        return true;
    };

    TypeArm primitiveArm = beginTypeArm(*this, value, inputs().types.intern(Type::fromPrimitives(primitivesWithJSONText)), BlockFrequency::Unlikely);
    if (primitiveArm.matches != KnownBoolean::False) {
        appendKeyOf(position);
        Value row = IRBuilder::make(Opcode::JSONAppendValue);
        row.setJsonPosition(JSONPosition::Element);
        ensureCursor(bytesTakenFromReservation(program(), row));
        row.setBuilderOperand(m_stringBuilder);
        row.setCursorOperand(m_cursor.value);
        row.setValueOperand(primitiveArm.narrowed);
        m_cursor = { appendResultAt(builder().append(WTF::move(row)), cursorResultOfJSONAppendValue), maxStringBuilderReservation };
        if (position.endsWithComma())
            appendToken(JSONToken::Comma);
        if (!continuesAfter(primitiveArm))
            return;
    }

    if (hasNoJSONText(program(), inputs().flow, typeOf(primitiveArm.remaining))) {
        if (position.kind == JSONPosition::Element)
            appendToken(JSONToken::Null);
    } else {
        TypeArm undefinedArm = beginTypeArm(*this, primitiveArm.remaining, TypeTable::undefined, BlockFrequency::Rare);
        if (undefinedArm.matches != KnownBoolean::False) {
            if (position.kind == JSONPosition::Element)
                appendToken(JSONToken::Null);
            if (!continuesAfter(undefinedArm))
                return;
        }
        appendByHost(undefinedArm.remaining, position);
        leaveIfWritingRunsCode(undefinedArm.remaining);
    }
    if (!done.block)
        return;
    jumpTo(done);
    continueAt(done);
}

void JSONSerializerBuilder::appendByHost(ValueID value, const Position& position)
{
    auto branchOnTextOf = [&](ValueID property) {
        commit();
        return builder().emitBranch(builder().append(Opcode::StringBuilderAppendJSON, { m_stringBuilder, value, property }));
    };

    MergePoint done = createMergePoint();
    switch (position.kind) {
    case JSONPosition::Root:
        break;
    case JSONPosition::Element: {
        BlockID hasNoText = branchOnTextOf(position.index).notTaken;
        jumpTo(done);

        builder().moveToEndOf(hasNoText);
        appendToken(JSONToken::Null);
        jumpTo(done);
        continueAt(done);
        return;
    }
    case JSONPosition::Property: {
        commit();
        ValueID checkpoint = builder().append(Opcode::StringBuilderCheckpoint, { m_stringBuilder });
        appendKeyOf(position);
        BlockID hasNoText = branchOnTextOf(appendString(program().at(position.key).text)).notTaken;
        if (position.endsWithComma())
            appendToken(JSONToken::Comma);
        jumpTo(done);

        builder().moveToEndOf(hasNoText);
        builder().append(Opcode::StringBuilderRollBack, { m_stringBuilder, checkpoint });
        jumpTo(done);
        continueAt(done);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
