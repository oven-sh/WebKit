/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTLayout.h"
#include "AOTMethodTableEntrypoints.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

PassOutcome callThroughMethodTable(FunctionPassContext& context)
{
    // The lookup of a selector is where the table sends a receiver that finds something else than the class definitions say.
    if (context.function.role == FunctionRole::TableLookup)
        return PassOutcome::Unchanged;
    return CallResolver(context, CallResolver::Resolution::MethodTable).run();
}

static bool isTailCall(const Function& function, const ValueSite& call)
{
    const Terminator& terminator = function.at(call.block).terminator;
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == call.value;
}

bool CallResolver::mayCallThroughTable(const ValueSite& position) const
{
    const Value& call = m_function.at(position.value);
    if (call.opcode != Opcode::Call || !call.calleeKey())
        return false;
    if (call.expectedResult() || call.argumentsOperands().size() > maxTableCallArguments || isTailCall(m_function, position))
        return false;
    const Value& read = m_function.at(CastChain(m_function, call.operand(OperandIndex::Call::callee)).last());
    if (read.opcode != Opcode::PropGet || read.key() != call.calleeKey() || read.isFreeName())
        return false;
    ValueID receiver = read.operand(OperandIndex::PropGet::object);
    if (classOfClassObject(m_program, typeOf(receiver)))
        return false;
    ReceiverCandidates candidates = receiverCandidates(m_program, m_function, receiver, { });
    return candidates.proof == ReceiverProof::Type && !candidates.hasOtherParts && candidates.proven.size() <= m_context.limit(LimitID::MaxAccessPlanMembers);
}

// The layouts of the receiver's family find methods of classes under the name, and not all of them the same one.
PlanOrDecline<TableCallPlan> CallResolver::planTableCall(const CallSite& site)
{
    constexpr auto declined = std::unexpected(CallDeclineReason::None);

    const Value* read = findMethodRead(site, Opcode::PropGet);
    if (!read || read->isFreeName() || !site.memberRead.canSinkToCall)
        return declined;
    const Value& call = m_function.at(site.position.value);
    if (call.expectedResult() || call.argumentsOperands().size() > maxTableCallArguments || isTailCall(m_function, site.position))
        return declined;

    TableCallPlan plan;
    plan.selector = { read->key(), static_cast<uint32_t>(call.argumentsOperands().size()), ArgumentForm::Typed };
    plan.receiver = read->operand(OperandIndex::PropGet::object);
    if (classOfClassObject(m_program, typeOf(plan.receiver)))
        return declined;

    // The type alone says which rows the call can ask for: what flow knows beside it is not in the type of the operand.
    ReceiverCandidates candidates = receiverCandidates(m_program, m_function, plan.receiver, { });
    if (candidates.proof != ReceiverProof::Type || candidates.hasOtherParts || candidates.proven.size() > m_context.limit(LimitID::MaxAccessPlanMembers))
        return declined;
    plan.checksReceiverAtRead = candidates.mayBeNullish;

    Vector<TypeID, 4> parameters = parameterTypesOf(m_program, m_context.types, plan.selector);
    for (uint32_t index = 0; index < plan.selector.numberOfArguments; ++index) {
        if (!typeOf(call.argumentsOperands()[index]).isSubsetOf(m_context.types.at(parameters[index]), m_program))
            plan.selector.form = ArgumentForm::Boxed;
    }
    // Once for all the methods: it walks every method of the name. A thousand classes with one method name, called at a thousand sites, were the cube of a thousand.
    if (plan.selector.form == ArgumentForm::Boxed)
        parameters = parameterTypesOf(m_program, m_context.types, plan.selector);

    MemberPartition partition = partitionMembers(accessContext(), candidates.proven.span(), plan.selector.key, Access::Read);
    LayoutList receivers;
    LayoutList otherReceivers(partition.rest.span());
    bool hasConditions = false;
    for (const MemberArm& arm : partition.arms) {
        const MemberAnswer& answer = arm.answer;
        bool isEnteredThroughTable = answer.kind == MemberKind::Method && answer.receiverProofs.isEmpty() && canRetireMemberRead(site, answer.method)
            && canHaveTableEntrypoint(m_program, m_context.types, answer.method, parameters.span());
        for (LayoutID layout : arm.layouts)
            (isEnteredThroughTable && !builtinBaseClassOf(m_program, layout) ? receivers : otherReceivers).add(layout);
        if (!isEnteredThroughTable)
            continue;
        hasConditions |= !answer.conditions.isEmpty();
        plan.methods.append(answer.method);
    }
    // One method: a test of one bit in front of a direct call, which can be inlined, costs less than the load of an entry and an indirect call.
    if (plan.methods.size() < 2 || (!hasConditions && partition.arms.size() <= m_context.limit(LimitID::MaxDispatchCases)))
        return declined;

    plan.receivers = m_context.types.completeLayoutSet(receivers.span());
    if (!otherReceivers.isEmpty())
        plan.otherReceivers = m_context.types.completeLayoutSet(otherReceivers.span());
    return plan;
}

void CallResolver::rewriteTableCall(const CallSite& site, const TableCallPlan& plan)
{
    m_expansion.begin(site.position);
    retireMemberRead(site, plan.checksReceiverAtRead, plan.selector.key);

    FunctionID lookup = m_context.derivedFunction(tableLookupKey(plan.selector));
    Vector<FunctionID, 4> entrypoints = plan.methods.map<Vector<FunctionID, 4>>([&](FunctionID method) {
        return m_context.derivedFunction(tableEntrypointKey(method, plan.selector));
    });

    auto finishWithTableCall = [&](ValueID receiver) {
        Vector<ValueID, 8> operands { receiver };
        operands.append(m_expansion.original().argumentsOperands());

        Value tableCall = IRBuilder::make(Opcode::CallTable);
        tableCall.setFunction(lookup);
        tableCall.setArgumentsOperands(operands.span());
        tableCall.setKey(plan.selector.key);
        tableCall.setReceivers(plan.receivers);
        tableCall.setEntrypoints(entrypoints.span());
        tableCall.setAccessSite(m_expansion.original().accessSite());
        tableCall.setCallees(m_expansion.original().callees());
        m_expansion.finishArm(m_expansion.append(WTF::move(tableCall)));
    };

    ValueID receiver = plan.checksReceiverAtRead ? site.memberRead.read().value : plan.receiver;
    if (!plan.otherReceivers)
        finishWithTableCall(receiver);
    else {
        ValueSplit split { .value = receiver, .arms = { }, .hasRest = true, .restFrequency = m_expansion.frequency() };
        split.arms.append(SplitArm { m_context.types.intern(Type::forLayoutSet(plan.receivers)), m_expansion.frequency() });
        emitSplit(m_expansion, split,
            [&](uint32_t, ValueID narrowed) {
                finishWithTableCall(narrowed);
            },
            [&](ValueID narrowed) {
                m_expansion.finishArm(m_expansion.appendOriginal({ OperandIndex::Call::thisValue, narrowed, plan.otherReceivers, { } }));
            });
    }
    m_expansion.end();
    m_context.build.statistics().count(StatisticID::TableCalls);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
