/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"

namespace JSC { namespace AOT { namespace Typed {

static Vector<ValueID, 4> argumentsOfAccess(const Value& original, MethodKind kind)
{
    switch (kind) {
    case MethodKind::Method:
        return Vector<ValueID, 4>(original.variadicOperands());
    case MethodKind::Getter:
        return { };
    case MethodKind::Setter:
        return Vector<ValueID, 4> { original.operand(*storedOf(original.opcode)) };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Access lookupAccessOf(MethodKind kind)
{
    switch (kind) {
    case MethodKind::Method:
    case MethodKind::Getter:
        return Access::Read;
    case MethodKind::Setter:
        return Access::Store;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<CallArm> planAccessorArm(const Program& program, const FlowResults& flow, const MemberAnswer& answer, MethodKind kind)
{
    if (kind == MethodKind::Method || !answer.needsNoTest())
        return std::nullopt;

    FunctionID accessor = answer.accessorFor(lookupAccessOf(kind));
    if (!accessor) {
        if (kind == MethodKind::Getter)
            return CallArm { .kind = CallArmKind::Undefined };
        return std::nullopt;
    }
    if (!canCallWithoutClosure(program, accessor))
        return std::nullopt;
    return createDirectCallArm(program, flow, accessor, { });
}

static std::optional<CallArm> planOriginalOperationsArm(const CallSite& site, MethodKind kind)
{
    if (kind != MethodKind::Method || !site.memberRead.canSinkToCall)
        return std::nullopt;
    return CallArm { .kind = CallArmKind::OriginalOperations };
}

static PropertyAccessPlan planCalledFieldRead(const PropertyAccessContext& context, const MemberArm& arm, PropertyKeyID key, uint64_t maxMembers)
{
    PropertyAccessQuery query;
    query.access = Access::Read;
    query.key = key;
    query.receivers.proof = ReceiverProof::Type;
    query.receivers.level = KnowledgeLevel::Proven;
    query.receivers.proven = arm.layouts;
    query.maxMembers = maxMembers;
    query.isOnlyCalled = true;
    return planPropertyAccess(context, query);
}

static bool canCallDeferredClosure(const Program& program, TypeArena& types, const PropertyAccessPlan& read)
{
    if (read.form != AccessForm::Method || !read.needsNoTest() || !mayBeCalledDirectly(program, read.member.method))
        return false;
    const Function& method = program.at(read.member.method);
    if (method.has(FunctionFlags::TakesCallee))
        return false;
    if (!read.capturesReceiver)
        return method.captures.isEmpty();
    return method.captures.size() == 1 && Type::forLayouts(read.receivers.span(), types).isSubsetOf(program.at(method.captures[0].type), program);
}

static CallDeclineReason declineWithoutResolvedArm(const MemberPartition& partition, MethodKind kind)
{
    bool needsTest = partition.arms.containsIf([](const MemberArm& arm) {
        return !arm.answer.needsNoTest();
    });
    if (needsTest)
        return CallDeclineReason::NeedsTest;
    bool mayRunFunction = kind == MethodKind::Method || partition.findArm(MemberKind::Accessor);
    return mayRunFunction ? CallDeclineReason::UnknownMember : CallDeclineReason::None;
}

static bool coversObjectsOf(FunctionPassContext& context, const ValueSplit& split, const Type& receiver)
{
    Type covered;
    for (const SplitArm& arm : split.arms)
        covered = covered.unionWith(context.types.at(arm.testedType), context.types);
    return receiver.withoutNullish().isSubsetOf(covered, context.program);
}

static FunctionID findMethodToPredict(FunctionPassContext& context, const CallSite& site, const Value& read)
{
    const Program& program = context.program;
    const Function& function = context.function;
    ReceiverCandidates candidates = receiverCandidates(program, function, read.operand(OperandIndex::PropGet::object), read.receivers());
    const LayoutList& members = candidates.isProven() ? candidates.proven : candidates.predicted;

    // planReceiverDispatch has recorded the loss of a proven set that is over the limit.
    bool isOverLimit = candidates.isProven()
        ? members.size() > context.limit(LimitID::MaxAccessPlanMembers)
        : context.exceeds(LimitID::MaxAccessPlanMembers, members.size(), site.position.value);
    if (isOverLimit)
        return { };

    MemberPartition partition = partitionMembers({ program, &context.inputs.flow }, members.span(), read.key(), Access::Read);
    const MemberArm* arm = findHeaviestArm(partition, candidates, [](const MemberArm& candidate) {
        return candidate.answer.kind == MemberKind::Method;
    });
    if (!arm)
        return { };

    FunctionID method = arm->answer.method;
    if (!mayBeCalledDirectly(program, method) || canCallWithoutClosure(program, method))
        return { };

    Type predicted = Type::forFunction(method);
    const Type& known = program.predictionOrTypeOf(function, function.at(site.position.value).operand(OperandIndex::Call::callee));
    if (known == predicted || !known.overlaps(predicted, program))
        return { };
    return method;
}

PlanOrDecline<CallPlan> CallResolver::planMemberCall(const CallSite& site)
{
    const Value* read = findMethodRead(site, Opcode::PropGet);
    if (!read || read->isFreeName())
        return std::unexpected(CallDeclineReason::None);
    if (classOfClassObject(m_program, typeOf(read->operand(OperandIndex::PropGet::object))))
        return std::unexpected(CallDeclineReason::None);

    PlanOrDecline<ReceiverDispatchPlan> dispatch = planReceiverDispatch(site, MethodKind::Method);
    if (dispatch)
        return CallPlan(WTF::move(*dispatch));

    if (FunctionID method = findMethodToPredict(m_context, site, *read))
        return CallPlan(MethodPredictionPlan { method });
    return std::unexpected(dispatch.error());
}

PlanOrDecline<ReceiverDispatchPlan> CallResolver::planReceiverDispatch(const CallSite& site, MethodKind kind)
{
    bool isCall = kind == MethodKind::Method;
    const Value& access = m_function.at(isCall ? site.memberRead.read().value : site.position.value);
    if (access.isFreeName())
        return std::unexpected(CallDeclineReason::None);

    ReceiverDispatchPlan plan;
    plan.kind = kind;
    plan.key = access.key();
    plan.receiver = access.operand(*receiverOf(access.opcode));
    plan.receiverOperandIndex = isCall ? OperandIndex::Call::thisValue : *receiverOf(access.opcode);
    plan.retiresMemberRead = isCall;

    ReceiverCandidates candidates = receiverCandidates(m_program, m_function, plan.receiver, access.receivers());
    if (!candidates.isProven()) {
        if (!candidates.predicted.isEmpty())
            return std::unexpected(CallDeclineReason::NeedsTest);
        return std::unexpected(isCall ? CallDeclineReason::UnknownMember : CallDeclineReason::None);
    }
    if (m_context.exceeds(LimitID::MaxAccessPlanMembers, candidates.proven.size(), site.position.value))
        return std::unexpected(CallDeclineReason::UnknownMember);
    if (candidates.mayBeNullish) {
        if (!isCall)
            return std::unexpected(CallDeclineReason::None);
        plan.checksReceiverAtRead = true;
    }

    MemberPartition partition = partitionMembers(accessContext(), candidates.proven.span(), plan.key, lookupAccessOf(kind));
    Vector<std::optional<CallArm>, 4> arms;
    for (const MemberArm& arm : partition.arms)
        arms.append(planReceiverArm(site, arm, kind));

    bool resolvesSomeArm = arms.containsIf([](const std::optional<CallArm>& arm) {
        return arm && arm->kind != CallArmKind::OriginalOperations;
    });
    if (!resolvesSomeArm)
        return std::unexpected(declineWithoutResolvedArm(partition, kind));

    DispatchPlan dispatch = planDispatch(m_context, partition, candidates, site.position.value, [&](const MemberArm& arm) {
        return arms[static_cast<size_t>(&arm - partition.arms.begin())].has_value();
    });
    if (dispatch.arms.isEmpty())
        return std::unexpected(CallDeclineReason::UnknownMember);

    plan.split = createSplit(m_context, dispatch, partition, plan.receiver, m_function.at(site.position.block).frequency);
    if (plan.checksReceiverAtRead)
        plan.split.hasRest = !coversObjectsOf(m_context, plan.split, typeOf(plan.receiver));

    bool copiesMemberRead = plan.split.hasRest;
    for (uint32_t armIndex : dispatch.arms) {
        CallArm arm = *arms[armIndex];
        arm.receivers = m_context.types.completeLayoutSet(partition.arms[armIndex].layouts.span());
        copiesMemberRead |= arm.kind == CallArmKind::OriginalOperations;
        plan.arms.append(arm);
    }
    if (isCall && copiesMemberRead && !site.memberRead.canSinkToCall)
        return std::unexpected(CallDeclineReason::EffectsBeforeCall);

    if (dispatch.hasRest())
        plan.restReceivers = m_context.types.completeLayoutSet(dispatch.rest.span());
    return plan;
}

std::optional<CallArm> CallResolver::planReceiverArm(const CallSite& site, const MemberArm& arm, MethodKind kind) const
{
    const MemberAnswer& answer = arm.answer;
    switch (answer.kind) {
    case MemberKind::Method:
        if (kind != MethodKind::Method || !answer.needsNoTest())
            return std::nullopt;
        if (!canCallWithoutClosure(m_program, answer.method) || !canRetireMemberRead(site, answer.method))
            return std::nullopt;
        return createDirectCallArm(m_program, m_flow, answer.method, m_function.at(site.position.value).callees());
    case MemberKind::Accessor:
        return planAccessorArm(m_program, m_flow, answer, kind);
    case MemberKind::Field: {
        if (kind != MethodKind::Method)
            return std::nullopt;
        PropertyKeyID key = m_function.at(site.memberRead.read().value).key();
        PropertyAccessPlan read = planCalledFieldRead(accessContext(), arm, key, m_context.limit(LimitID::MaxAccessPlanMembers));
        if (!canCallDeferredClosure(m_program, m_context.types, read) || !canRetireMemberRead(site, read.member.method))
            return planOriginalOperationsArm(site, kind);
        CallArm direct = createDirectCallArm(m_program, m_flow, read.member.method, m_function.at(site.position.value).callees());
        direct.capturesReceiver = read.capturesReceiver;
        return direct;
    }
    case MemberKind::Builtin:
    case MemberKind::Variable:
    case MemberKind::Namespace:
        return planOriginalOperationsArm(site, kind);
    case MemberKind::Absent:
    case MemberKind::Unknown:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CallResolver::rewriteReceiverDispatch(const CallSite& site, const ReceiverDispatchPlan& plan)
{
    m_expansion.begin(site.position);
    if (plan.retiresMemberRead)
        retireMemberRead(site, plan.checksReceiverAtRead, plan.key);

    ValueSplit split = plan.split;
    if (plan.checksReceiverAtRead)
        split.value = site.memberRead.read().value;

    Vector<ValueID, 4> arguments = argumentsOfAccess(m_expansion.original(), plan.kind);
    emitSplit(m_expansion, split,
        [&](uint32_t armIndex, ValueID narrowed) {
            emitReceiverArm(plan, plan.arms[armIndex], narrowed, arguments.span());
        },
        [&](ValueID narrowed) {
            m_expansion.finishArm(m_expansion.appendOriginal({ plan.receiverOperandIndex, narrowed, plan.restReceivers, { } }));
        });
    m_expansion.end();
    m_context.build.statistics().count(StatisticID::DispatchArms, plan.arms.size());
}

void CallResolver::emitReceiverArm(const ReceiverDispatchPlan& plan, const CallArm& arm, ValueID receiver, std::span<const ValueID> arguments)
{
    switch (arm.kind) {
    case CallArmKind::DirectCall: {
        ValueID result = appendCall(arm, receiver, { }, arguments);
        if (typeOf(result).isNever())
            m_expansion.finishArmAsUnreachable();
        else if (plan.kind == MethodKind::Setter)
            m_expansion.finishArmWithoutResult();
        else
            m_expansion.finishArm(result);
        return;
    }
    case CallArmKind::Undefined:
        m_expansion.finishArm(m_builder.appendUndefined());
        return;
    case CallArmKind::OriginalOperations:
        m_expansion.finishArm(m_expansion.appendOriginal({ plan.receiverOperandIndex, receiver, arm.receivers, { } }));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CallResolver::rewriteMethodPrediction(const CallSite& site, const MethodPredictionPlan& plan)
{
    const Value& call = m_function.at(site.position.value);
    Value predict = IRBuilder::make(Opcode::Predict, { call.operand(OperandIndex::Call::callee) });
    predict.setPredicted(m_context.types.intern(Type::forFunction(plan.method)));
    m_builder.setOrigin(call.origin());

    ValueID predicted = m_builder.insertBefore(site.position.point(), WTF::move(predict));
    m_function.at(site.position.value).setOperand(OperandIndex::Call::callee, predicted);
    m_builder.applyInsertions();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
