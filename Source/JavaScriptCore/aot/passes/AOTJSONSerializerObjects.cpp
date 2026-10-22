/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONSerializerBuilder.h"

#if ENABLE(AOT)

#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

std::optional<JSONToken> JSONSerializerBuilder::separatorAfter(MemberListState state)
{
    switch (state) {
    case MemberListState::NotOpened:
        return JSONToken::OpenObject;
    case MemberListState::AfterMember:
        return JSONToken::Comma;
    case MemberListState::AfterSeparator:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSONToken JSONSerializerBuilder::closingTokenOf(MemberListState state)
{
    switch (state) {
    case MemberListState::NotOpened:
        return JSONToken::EmptyObject;
    case MemberListState::AfterMember:
        return JSONToken::CloseObject;
    case MemberListState::AfterSeparator:
        return JSONToken::CloseObjectOverComma;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BlockFrequency currentFrequency(IRBuilder& builder)
{
    return builder.function().at(builder.currentBlock()).frequency;
}

void JSONSerializerBuilder::appendObjects(ValueID value, const ValuePlan& plan, const Position& position)
{
    RELEASE_ASSERT(!plan.layouts.isEmpty());
    commit();

    bool isProperty = position.kind == JSONPosition::Property;
    BlockFrequency frequency = currentFrequency(builder());
    MergePoint done = createMergePoint();
    BlockID generic = builder().addBlock(BlockFrequency::Rare);
    // A serializer that gives up has been called all the same, so what comes behind it has run unknown code.
    BlockID afterSerializerGaveUp = position.kind == JSONPosition::Root ? generic : builder().addBlock(BlockFrequency::Rare);
    ValueID object = emitTypeTest(value, TypeTable::object, generic);

    for (size_t index = 0; index < plan.layouts.size(); ++index) {
        LayoutID layout = plan.layouts[index];
        bool isLast = index + 1 == plan.layouts.size();
        BlockID next = isLast ? generic : builder().addBlock(frequency);
        TestedObject tested = emitShapeTest(object, singleElementSpan(layout), next);

        ValueID checkpoint;
        if (isProperty) {
            checkpoint = builder().append(Opcode::StringBuilderCheckpoint, { m_stringBuilder });
            appendKeyOf(position);
            commit();
        }

        BlockID unknown = builder().addBlock(BlockFrequency::Rare);
        appendCallOfSerializer(tested.object, layout, unknown);
        if (position.endsWithComma())
            appendToken(JSONToken::Comma);
        leaveIfWritingRunsCode(tested.object);
        jumpTo(done);

        builder().moveToEndOf(unknown);
        if (isProperty)
            builder().append(Opcode::StringBuilderRollBack, { m_stringBuilder, checkpoint });
        builder().emitJump(afterSerializerGaveUp);

        builder().moveToEndOf(next);
    }

    appendGeneric(value, position);
    jumpTo(done);

    if (afterSerializerGaveUp != generic) {
        builder().moveToEndOf(afterSerializerGaveUp);
        appendByHost(object, position);
        leaveIfWritingRunsCode(object);
        jumpTo(done);
    }
    continueAt(done);
}

void JSONSerializerBuilder::appendCallOfSerializer(ValueID object, LayoutID layout, BlockID whenUnknown)
{
    RELEASE_ASSERT(!hasCursor());
    FunctionID serializer = inputs().functions.ensure(derivedFunctionKey(FunctionRole::Serializer, layout));
    BlockID parsed = builder().addBlock(currentFrequency(builder()));
    emitBranchOnOutcome(appendCallOf(serializer, { object, m_stringBuilder }), parsed, whenUnknown, whenUnknown);
    builder().moveToEndOf(parsed);
}

void JSONSerializerBuilder::appendObjectOfLayout(ValueID object, LayoutID layout, BlockID whenShapeDiffers)
{
    commit();
    TestedObject tested = emitShapeTest(object, singleElementSpan(layout), whenShapeDiffers);
    if (!receiverStateOf(program(), layout).cannot(ObjectMutation::ChangePrototype))
        emitJumpUnless(builder().append(Opcode::HasLayoutPrototype, { tested.object, tested.guard }), whenShapeDiffers);

    BlockID done = builder().addBlock();
    BlockID noOrder = builder().addBlock(BlockFrequency::Rare);
    appendKeyOrderArms(tested, layout, noOrder, done);

    builder().moveToEndOf(noOrder);
    builder().emitJump(whenShapeDiffers);
    builder().moveToEndOf(done);
}

// An arm of its own is the fastest code for an order, so orders share an arm only where there are more of them than arms, and those first that differ least.
// A key that an object does not have is not looked for, so only a member that is read from its slot may be missing.
// A member that may be undefined is tested for it in every arm, so the object that has it pays nothing for the one that does not.
auto JSONSerializerBuilder::groupKeyOrders(LayoutID layout, std::span<const KeyOrderFields> orders, uint64_t maxArms) const -> Vector<KeyOrderFamily, 4>
{
    const Vector<Field>& fields = program().at(layout).fields;
    auto canBeMissingFrom = [&](std::span<const uint32_t> order, size_t count) {
        return std::ranges::all_of(order.subspan(count), [&](uint32_t field) {
            switch (jsonMemberSupportOf(program(), inputs().flow, layout, field)) {
            case JSONMemberSupport::NotSerialized:
            case JSONMemberSupport::WithoutText:
                return true;
            case JSONMemberSupport::ByName:
                return false;
            case JSONMemberSupport::Slot:
                return !!(program().at(fields[field].slotType ? fields[field].slotType : fields[field].declared.type).primitives & Primitive::Undefined);
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
    };

    Vector<KeyOrderFamily, 4> families;
    for (const KeyOrderFields& order : orders)
        families.append(KeyOrderFamily { order, { static_cast<uint32_t>(order.size()) } });

    while (families.size() > maxArms) {
        struct { size_t shorter { notFound }; size_t longer { notFound }; size_t numberOfMissingFields { 0 }; } best;
        for (size_t shorter = 0; shorter < families.size(); ++shorter) {
            for (size_t longer = 0; longer < families.size(); ++longer) {
                std::span<const uint32_t> longestOrder = families[longer].longestOrder.span();
                if (longestOrder.size() <= families[shorter].longestOrder.size() || !spanHasPrefix(longestOrder, families[shorter].longestOrder.span()))
                    continue;
                size_t smallestCount = families[shorter].smallestCount();
                size_t numberOfMissingFields = longestOrder.size() - smallestCount;
                bool isBetter = best.shorter == notFound || numberOfMissingFields < best.numberOfMissingFields
                    || (numberOfMissingFields == best.numberOfMissingFields && longestOrder.size() > families[best.longer].longestOrder.size());
                if (isBetter && canBeMissingFrom(longestOrder, smallestCount))
                    best = { shorter, longer, numberOfMissingFields };
            }
        }
        if (best.shorter == notFound)
            break;

        KeyOrderFamily merged = WTF::move(families[best.longer]);
        merged.descendingCounts.appendVector(families[best.shorter].descendingCounts);
        std::ranges::sort(merged.descendingCounts, std::ranges::greater { });
        // Flow lists the most common order first. The arms are tested in list order, and the limit cuts off the end of the list.
        families[std::min(best.shorter, best.longer)] = WTF::move(merged);
        families.removeAt(std::max(best.shorter, best.longer));
    }
    return families;
}

auto JSONSerializerBuilder::emitKeyOrderTests(const TestedObject& tested, const ObjectArm& arm, BlockID notTaken) -> TestedKeyOrder
{
    std::span<const uint32_t> longestOrder = arm.orders.longestOrder.span();
    std::span<const uint32_t> counts = arm.orders.descendingCounts.span();
    if (counts.size() == 1)
        return { emitKeyOrderTest(tested, arm.layout, longestOrder, notTaken), { } };

    BlockFrequency frequency = currentFrequency(builder());
    BlockID matched = builder().addBlock(frequency);
    ValueID guard = builder().addParameter(matched, TypeTable::guard);
    ValueID numberOfOwnMembers = builder().addParameter(matched, TypeTable::int32);
    for (uint32_t count : counts) {
        bool isLastCount = count == counts.back();
        BlockID next = isLastCount ? notTaken : builder().addBlock(frequency);
        ValueID guardOfOrder = emitKeyOrderTest(tested, arm.layout, longestOrder.first(count), next);
        auto numberOfMembers = std::ranges::count_if(arm.members, [&](const Member& member) {
            return member.positionInOrder < count;
        });
        builder().emitJump(matched, { guardOfOrder, builder().appendInteger(numberOfMembers) });
        if (!isLastCount)
            builder().moveToEndOf(next);
    }
    builder().moveToEndOf(matched);
    return { guard, numberOfOwnMembers };
}

void JSONSerializerBuilder::appendKeyOrderArms(const TestedObject& tested, LayoutID layout, BlockID whenNoOrderMatches, BlockID done)
{
    // An order that is a prefix of another order shares its arm and costs one test, and an order of n fields has n + 1 prefixes.
    BuildContext& build = inputs().build;
    uint64_t maxArms = build.limit(LimitID::MaxExpandedKeyOrders);
    uint64_t maxFields = build.limit(LimitID::MaxExpandedKeyFields);
    ExpectedKeyOrders expected = expectedKeyOrdersOf(program(), layout, KeyOrderLimits { maxArms * (maxFields + 1), maxFields });
    Vector<KeyOrderFamily, 4> families = groupKeyOrders(layout, expected.orders.span(), maxArms);

    uint64_t numberOfArmsLeftOut = expected.numberOfOrdersLeftOut;
    if (families.size() > maxArms) {
        numberOfArmsLeftOut += families.size() - maxArms;
        families.shrink(static_cast<size_t>(maxArms));
    }
    if (numberOfArmsLeftOut)
        build.exceeds(LimitID::MaxExpandedKeyOrders, maxArms + numberOfArmsLeftOut, Subject { .function = inputs().functionAsRead, .layout = layout });

    const Vector<Field>& fields = program().at(layout).fields;
    TypingContext typing { program(), function(), inputs().types };
    for (const KeyOrderFamily& family : families) {
        bool isLastFamily = &family == &families.last();
        BlockID next = builder().addBlock(isLastFamily ? BlockFrequency::Rare : BlockFrequency::Normal);

        ObjectArm arm { { tested.object, { } }, layout, family, { }, { }, { }, done };
        for (uint32_t position = 0; position < family.longestOrder.size(); ++position) {
            uint32_t fieldIndex = family.longestOrder[position];
            if (jsonMemberSupportOf(program(), inputs().flow, layout, fieldIndex) == JSONMemberSupport::NotSerialized)
                continue;
            const Field& field = fields[fieldIndex];
            bool holdsMoreThanDeclared = field.slotType || field.declared.flags.contains(PositionFlags::IsExpected);
            Type proven = field.slotType ? program().at(slotTypeOf(program(), field)) : typing.typeOf(field.declared);
            const Type* expected = holdsMoreThanDeclared ? &program().at(field.declared.type) : nullptr;
            arm.members.append(Member { field.key, fieldIndex, position, planType(proven, expected) });
        }

        TestedKeyOrder order = emitKeyOrderTests(tested, arm, next);
        arm.object.guard = order.guard;
        arm.numberOfOwnMembers = order.numberOfOwnMembers;

        appendMembers(arm);
        appendRemainder(arm);
        builder().moveToEndOf(next);
    }
    builder().emitJump(whenNoOrderMatches);
}

void JSONSerializerBuilder::appendMembers(ObjectArm& arm)
{
    MemberListState memberList = MemberListState::NotOpened;
    for (size_t index = 0; index < arm.members.size(); ++index)
        appendMember(arm, index, memberList);

    appendToken(closingTokenOf(memberList));
    commit();
    builder().emitJump(arm.done);
}

void JSONSerializerBuilder::appendMember(ObjectArm& arm, size_t index, MemberListState& memberList)
{
    const Member& member = arm.members[index];
    const ValuePlan& plan = member.plan;
    BlockFrequency frequency = currentFrequency(builder());

    ValueID value;
    bool isReadByName = false;
    switch (jsonMemberSupportOf(program(), inputs().flow, arm.layout, member.field)) {
    case JSONMemberSupport::WithoutText:
        // The member stays in the list: where the guards go stale, the remaining keys are read by name.
        return;
    case JSONMemberSupport::Slot:
        value = member.positionInOrder < arm.orders.smallestCount() ? appendFieldRead(arm.object, arm.layout, member.field) : appendFieldReadOrUndefined(arm.object, arm.layout, member.field);
        break;
    case JSONMemberSupport::ByName: {
        commit();
        Value read = IRBuilder::make(Opcode::PropGet, { arm.object.object });
        read.setKey(member.key);
        value = builder().append(WTF::move(read));
        isReadByName = true;
        break;
    }
    case JSONMemberSupport::NotSerialized:
        RELEASE_ASSERT_NOT_REACHED();
    }

    const Type& type = typeOf(value);
    bool mayBeOmitted = plan.kind == ValueKind::Generic || plan.certainty == TypeCertainty::Expected || (type.primitives & primitivesWithoutJSONText) || type.hasObjects();
    if (mayBeOmitted) {
        if (std::optional<JSONToken> separator = separatorAfter(memberList)) {
            appendToken(*separator);
            memberList = MemberListState::AfterSeparator;
        }
    }

    MergePoint afterUnknownCode { .frequency = BlockFrequency::Rare };
    MergePoint* outer = std::exchange(m_afterUnknownCode, &afterUnknownCode);
    appendValue(value, plan, Position { JSONPosition::Property, member.key, { }, memberList });
    if (isReadByName)
        leaveAfterUnknownCode();
    m_afterUnknownCode = outer;
    if (memberList == MemberListState::NotOpened)
        memberList = MemberListState::AfterMember;

    if (!afterUnknownCode.block)
        return;
    MergePoint next { .frequency = frequency };
    if (index + 1 == arm.members.size()) {
        jumpTo(next);
        continueAt(afterUnknownCode);
        jumpTo(next);
        continueAt(next);
        return;
    }

    jumpTo(next, arm.object.guard);
    continueAt(afterUnknownCode);
    renewGuards(arm, index + 1, memberList);
    jumpTo(next, arm.object.guard);
    continueAt(next);
    arm.object.guard = next.guard;
}

void JSONSerializerBuilder::renewGuards(ObjectArm& arm, size_t nextMember, MemberListState memberList)
{
    commit();
    BlockID stale = builder().addBlock(BlockFrequency::Rare);
    TestedObject tested = emitShapeTest(arm.object.object, singleElementSpan(arm.layout), stale);
    TestedKeyOrder order = emitKeyOrderTests(tested, arm, stale);
    // The keys that are written are those the object had at the beginning.
    if (order.numberOfOwnMembers)
        emitJumpUnless(builder().append(Opcode::StrictEq, { order.numberOfOwnMembers, arm.numberOfOwnMembers }), stale);
    arm.object.guard = order.guard;
    BlockID renewed = builder().currentBlock();

    builder().moveToEndOf(stale);
    if (std::optional<JSONToken> separator = separatorAfter(memberList))
        appendToken(*separator);
    commit();
    if (!arm.remainder) {
        arm.remainder = builder().addBlock(BlockFrequency::Rare);
        builder().addParameter(arm.remainder, TypeTable::int32);
    }
    builder().emitJump(arm.remainder, { builder().appendInteger(static_cast<int64_t>(nextMember)) });

    builder().moveToEndOf(renewed);
}

void JSONSerializerBuilder::appendRemainder(const ObjectArm& arm)
{
    if (!arm.remainder)
        return;

    RELEASE_ASSERT(!hasCursor());
    builder().moveToEndOf(arm.remainder);
    ValueID firstMember = builder().function().at(arm.remainder).parameters[0];

    Vector<ValueID, 8> keyTexts;
    for (const Member& member : arm.members)
        keyTexts.append(appendString(program().at(member.key).text));
    Value keyList = IRBuilder::make(Opcode::NewArray);
    keyList.setContents(TypeTable::string);
    keyList.setElementsOperands(keyTexts.span());
    ValueID keys = builder().append(WTF::move(keyList));
    if (arm.numberOfOwnMembers) {
        Value truncation = IRBuilder::make(Opcode::PropSet, { keys, arm.numberOfOwnMembers });
        truncation.setKey(program().keyOf(BuiltinNames::length));
        truncation.setMode(ECMAMode::Strict);
        builder().append(WTF::move(truncation));
    }

    ValueID text = appendHelperCall(Builtins::CompilerHelpersStringifyProperties, { arm.object.object, keys, firstMember, builder().appendBoolean(true) });
    ValueID length = builder().append(Opcode::GetStringLength, { text });
    ValueID isEmpty = builder().append(Opcode::StrictEq, { length, builder().appendInteger(0) });
    BlockID hasText = builder().emitBranch(isEmpty).notTaken;
    appendToken(JSONToken::CloseObjectOverComma);
    commit();
    builder().emitJump(arm.done);

    builder().moveToEndOf(hasText);
    appendText(text);
    appendText(appendString("}"_s));
    builder().emitJump(arm.done);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
