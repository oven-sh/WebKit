/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMethodDispatch.h"

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTTypeTestSite.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::IsSlotFilled::object == testedOperandIndex);
static_assert(OperandIndex::HasNoExtraProperties::object == testedOperandIndex);
static_assert(OperandIndex::HasNoFieldOutsideSlot::object == testedOperandIndex);
static_assert(OperandIndex::HasBirthFieldsFilled::object == testedOperandIndex);
static_assert(OperandIndex::IsNotFrozen::object == testedOperandIndex);
static_assert(OperandIndex::HasLayoutPrototype::object == testedOperandIndex);
static_assert(OperandIndex::BelongsToInstance::object == testedOperandIndex);

namespace {

struct ArmWeight {
    bool findsMember { false };
    uint32_t expectedMembers { 0 };
    uint32_t members { 0 };
    LayoutID firstMember;
};

} // anonymous namespace

static constexpr std::array objectProofsInTestOrder {
    Proof::OwnInstance,
    Proof::PrototypeLinkUnchanged,
    Proof::NoExtraProperties,
    Proof::NoFieldOutsideSlot,
    Proof::BirthFieldsFilled,
    Proof::NotFrozen,
    Proof::SlotFilled,
};

static constexpr ProofSet testableProofs = [] {
    ProofSet proofs { Proof::NotNullish };
    for (Proof proof : objectProofsInTestOrder)
        proofs.add(proof);
    return proofs;
}();

void setSlotStamp(Value& value, const SlotStamp& stamp)
{
    value.setKey(stamp.key);
    value.setReceivers(stamp.receivers);
    value.setLevel(stamp.level);
    if (stamp.accessSite)
        value.setAccessSite(stamp.accessSite);
}

Value makeObjectTest(Proof proof, ValueID object, const SlotStamp& stamp)
{
    switch (proof) {
    case Proof::SlotFilled: {
        Value test = IRBuilder::make(Opcode::IsSlotFilled, { object });
        // Sites share a test when its payload is equal, so the test does not name the access site it was made for.
        setSlotStamp(test, { stamp.key, stamp.receivers, stamp.level, { } });
        return test;
    }
    case Proof::NoExtraProperties:
        return IRBuilder::make(Opcode::HasNoExtraProperties, { object });
    case Proof::NoFieldOutsideSlot:
        return IRBuilder::make(Opcode::HasNoFieldOutsideSlot, { object });
    case Proof::BirthFieldsFilled:
        return IRBuilder::make(Opcode::HasBirthFieldsFilled, { object });
    case Proof::NotFrozen:
        return IRBuilder::make(Opcode::IsNotFrozen, { object });
    case Proof::OwnInstance:
        return IRBuilder::make(Opcode::BelongsToInstance, { object });
    case Proof::PrototypeLinkUnchanged:
        return IRBuilder::make(Opcode::HasLayoutPrototype, { object });
    case Proof::None:
    case Proof::TestedType:
    case Proof::Truthiness:
    case Proof::Equality:
    case Proof::IntegerBound:
    case Proof::ExactFunction:
    case Proof::CaseValue:
    case Proof::NotNullish:
    case Proof::Shape:
    case Proof::Pristine:
    case Proof::ArrayStorage:
    case Proof::InBounds:
    case Proof::NotThenable:
    case Proof::MemberUnchanged:
    case Proof::EvaluatedOnce:
    case Proof::KeyOrder:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::PlainData:
    case Proof::SourceOpen:
    case Proof::TextMatches:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MemberConditionKind kindInRecordOf(MemberCondition::Kind kind)
{
    switch (kind) {
    case MemberCondition::Kind::MemberUnchanged:
        return MemberConditionKind::MemberUnchanged;
    case MemberCondition::Kind::PrototypeLinkUnchanged:
        return MemberConditionKind::PrototypeLinkUnchanged;
    case MemberCondition::Kind::ClassEvaluatedOnce:
        return MemberConditionKind::ClassEvaluatedOnce;
    case MemberCondition::Kind::InstancePrototypeUnchanged:
        return MemberConditionKind::InstancePrototypeUnchanged;
    case MemberCondition::Kind::MemberNotHidden:
        return MemberConditionKind::MemberNotHidden;
    case MemberCondition::Kind::MemberPassedByStores:
        return MemberConditionKind::MemberPassedByStores;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MemberConditionRecord recordOf(const MemberCondition& condition)
{
    return { .kind = kindInRecordOf(condition.kind), .layout = condition.layout, .side = condition.side, .key = condition.key };
}

// Placement gives each set of conditions one bit, which the run time clears with the bit of any of them. Sites share a test when they ask for the same set, so the set is listed in one order.
Value makeConditionsTest(TypeArena&, std::span<const MemberCondition> conditions)
{
    RELEASE_ASSERT(!conditions.empty() && std::ranges::is_sorted(conditions));
    Vector<MemberConditionRecord, 4> records(conditions.size(), [&](size_t index) {
        return recordOf(conditions[index]);
    });

    Value test = IRBuilder::make(Opcode::AreMembersUnchanged);
    test.setConditions(records.span());
    return test;
}

bool testsCoverConditions(std::span<const Value> tests, std::span<const MemberCondition> conditions)
{
    return std::ranges::all_of(conditions, [&](const MemberCondition& condition) {
        MemberConditionRecord record = recordOf(condition);
        return std::ranges::any_of(tests, [&](const Value& test) {
            return test.opcode == Opcode::AreMembersUnchanged && std::ranges::contains(test.conditions(), record);
        });
    });
}

static Type testableTypeOf(const PropertyAccessContext& context, const Type& stored, const UnionPart& part)
{
    switch (part.kind) {
    case UnionPartKind::Layouts:
        // A value whose type lists layouts counts as an object of this Instance, which a test of its layout does not show.
        if (stored.layouts || instanceCountOf(context) == InstanceCount::AtMostOne)
            return part.type;
        return Type::never();
    case UnionPartKind::BuiltinClass:
        return part.type.withoutClassProofs();
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
        return part.type;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A TypeTest proves no contents, storage or pristine state: a class part counts only when the value's own type has the proofs the field asks for.
Type testableStoredTypeOf(const PropertyAccessContext& context, const Type& stored, const Type& fieldType, TypeArena& types)
{
    Type testable = Type::never();
    for (const UnionPart& part : unionPartsOf(fieldType.intersectWith(stored, types))) {
        Type testablePart = testableTypeOf(context, stored, part);
        if (stored.castTo(testablePart, types).isSubsetOf(fieldType, context.program))
            testable = testable.unionWith(testablePart, types);
    }
    return testable;
}

AccessTests createAccessTests(FunctionPassContext& context, const PropertyAccessPlan& plan, const AccessTestInputs& inputs)
{
    RELEASE_ASSERT((plan.requiredProofs | testableProofs) == testableProofs);

    const Program& program = context.program;
    TypeArena& types = context.types;
    AccessTests result;

    switch (plan.receiverTest) {
    case ReceiverTest::None:
        // The layouts of the receiver are proven, and undefined and null are members of its type beside them.
        if (plan.requiredProofs.contains(Proof::NotNullish))
            result.tests.append(makeTypeTest(inputs.receiver, TypeTable::object));
        break;
    case ReceiverTest::Type:
        result.receiverLayoutTest = static_cast<uint32_t>(result.tests.size());
        result.tests.append(makeTypeTest(inputs.receiver, types.intern(Type::forLayouts(plan.receivers.span(), types))));
        break;
    case ReceiverTest::ClassObject:
        result.receiverLayoutTest = static_cast<uint32_t>(result.tests.size());
        result.tests.append(makeTypeTest(inputs.receiver, types.intern(Type::forFunction(plan.testedClassObject))));
        break;
    }

    for (Proof proof : objectProofsInTestOrder) {
        if (!plan.requiredProofs.contains(proof))
            continue;
        Value test = makeObjectTest(proof, inputs.receiver, inputs.stamp);
        if (proof == Proof::SlotFilled && plan.testsDeclaredType)
            test.setHoldsDeclaredType(true);
        result.tests.append(WTF::move(test));
    }

    // The first test that reads the receiver also finds out that it is undefined or null: it faults. So nothing that branches stands between the check for those two and that test.
    if (!plan.conditions.isEmpty())
        result.tests.append(makeConditionsTest(types, plan.conditions.span()));

    // The test of the stored value comes last: whoever plans the store behind the tests takes it off the end.
    if (plan.stores() && !plan.storedValueFits) {
        Type fieldType = storedTypeOf(program, plan.receivers.span(), inputs.stamp.key, types);
        Type testable = testableStoredTypeOf({ program, &context.inputs.flow }, program.typeOf(context.function, inputs.stored), fieldType, types);
        if (testable.isNever())
            return { };
        result.tests.append(makeTypeTest(inputs.stored, types.intern(testable)));
    }
    return result;
}

std::optional<ReceiverCandidates> receiversEstablishedBy(const Program& program, std::span<const Value> tests)
{
    // The test of a receiver comes first. A later TypeTest is about the stored value, whose type may have layouts too.
    if (tests.empty() || tests[0].opcode != Opcode::TypeTest)
        return std::nullopt;
    const Type& tested = program.at(tests[0].testedType());
    if (LayoutID classLayout = classOfClassObject(program, tested))
        return ReceiverCandidates::underGuard(singleElementSpan(classLayout));
    if (!tested.layouts)
        return std::nullopt;
    return ReceiverCandidates::underGuard(membersOf(program, tested));
}

static ArmWeight weightOf(const MemberArm& arm, const ReceiverCandidates& candidates)
{
    LayoutList expectedMembers = arm.layouts;
    expectedMembers.intersect(candidates.predicted);
    return { arm.answer.kind != MemberKind::Absent, static_cast<uint32_t>(expectedMembers.size()), static_cast<uint32_t>(arm.layouts.size()), arm.layouts[0] };
}

static bool isHeavier(const ArmWeight& a, const ArmWeight& b)
{
    if (a.findsMember != b.findsMember)
        return a.findsMember;
    if (a.expectedMembers != b.expectedMembers)
        return a.expectedMembers > b.expectedMembers;
    if (a.members != b.members)
        return a.members > b.members;
    return a.firstMember < b.firstMember;
}

DispatchPlan planDispatch(FunctionPassContext& context, const MemberPartition& partition, const ReceiverCandidates& candidates, ValueID site, NOESCAPE const CanEmitArm& canEmitArm)
{
    DispatchPlan plan;
    Vector<ArmWeight, 4> weights;
    for (uint32_t index = 0; index < partition.arms.size(); ++index) {
        const MemberArm& arm = partition.arms[index];
        weights.append(weightOf(arm, candidates));
        if (canEmitArm(arm))
            plan.arms.append(index);
    }
    AOT::sort(plan.arms, [&](uint32_t a, uint32_t b) {
        return isHeavier(weights[a], weights[b]);
    });
    if (context.exceeds(LimitID::MaxDispatchCases, plan.arms.size(), site))
        plan.arms.shrink(static_cast<size_t>(context.limit(LimitID::MaxDispatchCases)));

    plan.rest = partition.rest;
    for (uint32_t index = 0; index < partition.arms.size(); ++index) {
        if (!plan.arms.contains(index))
            plan.rest.unite(partition.arms[index].layouts);
    }
    plan.restIsLikely = plan.rest.overlaps(candidates.predicted);
    return plan;
}

static LayoutList membersNeverAllocated(const Program& program, const Type& type)
{
    LayoutList members;
    for (LayoutID member : membersOf(program, type)) {
        if (program.at(member).flow.allocation == AllocationState::NeverAllocated)
            members.add(member);
    }
    return members;
}

ValueSplit createSplit(FunctionPassContext& context, const DispatchPlan& dispatch, const MemberPartition& partition, ValueID receiver, BlockFrequency siteFrequency)
{
    const Program& program = context.program;
    TypeArena& types = context.types;
    BlockFrequency unlikelyFrequency = rarerOf(siteFrequency, BlockFrequency::Unlikely);

    LayoutList expected(membersOf(program, program.expectedOrTypeOf(context.function, receiver)));
    auto hasExpectedMember = [&](uint32_t armIndex) {
        return partition.arms[armIndex].layouts.overlaps(expected);
    };
    bool hasExpectation = dispatch.restIsLikely || std::ranges::any_of(dispatch.arms, hasExpectedMember);

    ValueSplit split;
    split.value = receiver;
    LayoutList covered;
    for (uint32_t armIndex : dispatch.arms) {
        const LayoutList& layouts = partition.arms[armIndex].layouts;
        bool isLikely = split.arms.isEmpty() || !hasExpectation || hasExpectedMember(armIndex);
        split.arms.append(SplitArm { types.intern(Type::forLayouts(layouts.span(), types)), isLikely ? siteFrequency : unlikelyFrequency });
        covered.unite(layouts);
    }
    split.restFrequency = dispatch.restIsLikely ? siteFrequency : unlikelyFrequency;

    const Type& receiverType = program.typeOf(context.function, receiver);
    split.hasRest = !receiverType.isSubsetOf(Type::forLayouts(covered.span(), types), program);
    if (!split.hasRest || split.arms.isEmpty())
        return split;

    // A family keeps the layouts that nothing allocates. No object has one of them, so they ask for no arm and for no rest: the arm that is entered without a test takes them along.
    LayoutList neverAllocated = membersNeverAllocated(program, receiverType);
    covered.unite(neverAllocated);
    if (neverAllocated.isEmpty() || !receiverType.isSubsetOf(Type::forLayouts(covered.span(), types), program))
        return split;

    LayoutList layoutsOfLastArm = partition.arms[dispatch.arms.last()].layouts;
    layoutsOfLastArm.unite(neverAllocated);
    split.arms.last().testedType = types.intern(Type::forLayouts(layoutsOfLastArm.span(), types));
    split.hasRest = false;
    return split;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
