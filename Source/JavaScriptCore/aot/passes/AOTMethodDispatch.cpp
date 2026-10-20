/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMethodDispatch.h"

#if ENABLE(AOT)

#include "AOTTypeTestSite.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::IsSlotFilled::object == testedOperandIndex);
static_assert(OperandIndex::HasNoExtraProperties::object == testedOperandIndex);
static_assert(OperandIndex::IsNotFrozen::object == testedOperandIndex);
static_assert(OperandIndex::HasLayoutPrototype::object == testedOperandIndex);
static_assert(OperandIndex::BelongsToInstance::object == testedOperandIndex);

static constexpr std::array objectProofsInTestOrder {
    Proof::OwnInstance,
    Proof::PrototypeLinkUnchanged,
    Proof::NoExtraProperties,
    Proof::NotFrozen,
    Proof::SlotFilled,
};

static constexpr ProofSet testableObjectProofs = [] {
    ProofSet proofs;
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
    case Proof::Shape:
    case Proof::Pristine:
    case Proof::ArrayStorage:
    case Proof::InBounds:
    case Proof::NotThenable:
    case Proof::MemberUnchanged:
    case Proof::EvaluatedOnce:
    case Proof::KeyOrder:
    case Proof::EpochCurrent:
    case Proof::UnmodifiedGenerator:
    case Proof::EnumeratorCurrent:
    case Proof::PlainData:
    case Proof::SourceOpen:
    case Proof::TextMatches:
    case Proof::RequireEdgeReady:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Value makeConditionTest(const MemberCondition& condition)
{
    switch (condition.kind) {
    case MemberCondition::Kind::MemberUnchanged: {
        Value test = IRBuilder::make(Opcode::IsMemberUnchanged);
        test.setLayout(condition.layout);
        test.setSide(condition.side);
        test.setKey(condition.key);
        return test;
    }
    case MemberCondition::Kind::PrototypeLinkUnchanged: {
        Value test = IRBuilder::make(Opcode::IsPrototypeLinkUnchanged);
        test.setLayout(condition.layout);
        test.setSide(condition.side);
        return test;
    }
    case MemberCondition::Kind::ClassEvaluatedOnce: {
        Value test = IRBuilder::make(Opcode::IsClassEvaluatedOnce);
        test.setLayout(condition.layout);
        return test;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type testableTypeOf(const PropertyAccessContext& context, const Type& stored, const UnionPart& part)
{
    switch (part.kind) {
    case UnionPartKind::Layouts: {
        // A value whose type lists layouts counts as an object of this Instance, which a test of its layout does not show.
        bool isObjectOfThisInstance = stored.layouts || instanceCountOf(context) == InstanceCount::AtMostOne;
        if (isObjectOfThisInstance && layoutTestKindFor(context.program, membersOf(context.program, part.type)) == LayoutTestKind::Type)
            return part.type;
        return Type::never();
    }
    case UnionPartKind::BuiltinClass:
        return part.type.withoutProofs();
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
    RELEASE_ASSERT((plan.requiredProofs | testableObjectProofs) == testableObjectProofs);

    const Program& program = context.program;
    TypeArena& types = context.types;
    AccessTests result;

    switch (plan.receiverTest) {
    case ReceiverTest::None:
        break;
    case ReceiverTest::Shape: {
        const Type& type = program.typeOf(context.function, inputs.receiver);
        if (!type.isSubsetOf(Type::object(), program)) {
            Type objects = type.intersectWith(Type::object(), types);
            if (objects.isNever())
                return { };
            result.tests.append(makeTypeTest(inputs.receiver, types.intern(objects.withoutClassProofs())));
        }
        result.receiverLayoutTest = static_cast<uint32_t>(result.tests.size());
        result.tests.append(makeShapeTest(inputs.receiver, types.completeLayoutSet(plan.receivers.span())));
        break;
    }
    case ReceiverTest::Type:
        result.receiverLayoutTest = static_cast<uint32_t>(result.tests.size());
        result.tests.append(makeTypeTest(inputs.receiver, types.intern(Type::forLayouts(plan.receivers.span(), types))));
        break;
    }

    for (const MemberCondition& condition : plan.conditions)
        result.tests.append(makeConditionTest(condition));

    for (Proof proof : objectProofsInTestOrder) {
        if (plan.requiredProofs.contains(proof))
            result.tests.append(makeObjectTest(proof, inputs.receiver, inputs.stamp));
    }

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
    for (const Value& test : tests) {
        if (test.opcode == Opcode::ShapeTest)
            return ReceiverCandidates::underGuard(program.at(test.layouts()).members.span());
    }
    // The TypeTest of a receiver comes first. A later one is about the stored value, whose type may have layouts too.
    if (tests.empty() || tests[0].opcode != Opcode::TypeTest)
        return std::nullopt;
    const Type& tested = program.at(tests[0].testedType());
    if (!tested.layouts)
        return std::nullopt;
    return ReceiverCandidates::underGuard(membersOf(program, tested));
}

ArmWeight weightOf(const MemberArm& arm, const ReceiverCandidates& candidates)
{
    LayoutList predictedMembers = arm.layouts;
    predictedMembers.intersect(candidates.predicted);
    return { arm.answer.kind != MemberKind::Absent, static_cast<uint32_t>(predictedMembers.size()), static_cast<uint32_t>(arm.layouts.size()), arm.layouts[0] };
}

bool isHeavier(const ArmWeight& a, const ArmWeight& b)
{
    if (a.findsMember != b.findsMember)
        return a.findsMember;
    if (a.predictedMembers != b.predictedMembers)
        return a.predictedMembers > b.predictedMembers;
    if (a.members != b.members)
        return a.members > b.members;
    return a.firstMember < b.firstMember;
}

const MemberArm* findHeaviestArm(const MemberPartition& partition, const ReceiverCandidates& candidates, NOESCAPE const ScopedLambda<bool(const MemberArm&)>& isWanted)
{
    const MemberArm* heaviest = nullptr;
    ArmWeight heaviestWeight;
    for (const MemberArm& arm : partition.arms) {
        if (!isWanted(arm))
            continue;
        ArmWeight weight = weightOf(arm, candidates);
        if (heaviest && !isHeavier(weight, heaviestWeight))
            continue;
        heaviest = &arm;
        heaviestWeight = weight;
    }
    return heaviest;
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
    std::ranges::sort(plan.arms, [&](uint32_t a, uint32_t b) {
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

ValueSplit createSplit(FunctionPassContext& context, const DispatchPlan& dispatch, const MemberPartition& partition, ValueID receiver, BlockFrequency siteFrequency)
{
    const Program& program = context.program;
    TypeArena& types = context.types;
    BlockFrequency unlikelyFrequency = rarerOf(siteFrequency, BlockFrequency::Unlikely);

    LayoutList predicted(membersOf(program, program.predictionOrTypeOf(context.function, receiver)));
    auto hasPredictedMember = [&](uint32_t armIndex) {
        return partition.arms[armIndex].layouts.overlaps(predicted);
    };
    bool hasPrediction = dispatch.restIsLikely || std::ranges::any_of(dispatch.arms, hasPredictedMember);

    ValueSplit split;
    split.value = receiver;
    LayoutList covered;
    for (uint32_t armIndex : dispatch.arms) {
        const LayoutList& layouts = partition.arms[armIndex].layouts;
        bool isLikely = split.arms.isEmpty() || !hasPrediction || hasPredictedMember(armIndex);
        split.arms.append(SplitArm { types.intern(Type::forLayouts(layouts.span(), types)), isLikely ? siteFrequency : unlikelyFrequency });
        covered.unite(layouts);
    }
    split.hasRest = !program.typeOf(context.function, receiver).isSubsetOf(Type::forLayouts(covered.span(), types), program);
    split.restFrequency = dispatch.restIsLikely ? siteFrequency : unlikelyFrequency;
    return split;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
