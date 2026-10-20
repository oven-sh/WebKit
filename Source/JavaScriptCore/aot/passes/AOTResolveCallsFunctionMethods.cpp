/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTCreateBoundFunctions.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

template<typename Plan>
static PlanOrDecline<CallPlan> toCallPlan(PlanOrDecline<Plan>&& planned)
{
    if (!planned)
        return std::unexpected(planned.error());
    return CallPlan(WTF::move(*planned));
}

static unsigned numberOfBoundArguments(const BuiltinCall& bind)
{
    return bind.arguments.empty() ? 0 : static_cast<unsigned>(bind.arguments.size() - 1);
}

static unsigned numberOfCapturedArguments(const Function& declared, unsigned numberOfBound)
{
    return std::min(numberOfBound, declared.numberOfPositionalParameters());
}

static bool canBind(FunctionPassContext& context, const BuiltinCall& bind, FunctionID targetID, ValueID site)
{
    const Program& program = context.program;
    const Function& target = program.at(targetID);
    if (!canCreateBoundFunction(program, targetID))
        return false;

    const Function& declared = program.at(versionToEnter(program, context.inputs.flow, targetID, { }));
    unsigned capturedArgumentCount = numberOfCapturedArguments(declared, numberOfBoundArguments(bind));
    auto isWithin = [&](TypeID type, const DeclaredType& bound) {
        return program.at(type).isSubsetOf(program.at(bound.type), program);
    };
    if (target.has(FunctionFlags::HasThis)) {
        TypeID boundThis = bind.arguments.empty() ? TypeTable::undefined : context.function.at(bind.arguments[0]).type;
        if (!isWithin(boundThis, declared.thisType))
            return false;
    }
    for (unsigned index = 0; index < capturedArgumentCount; ++index) {
        if (!isWithin(context.function.at(bind.arguments[index + 1]).type, declared.parameters[index]))
            return false;
    }
    return !context.exceeds(LimitID::MaxCaptures, declared.captures.size() + numberOfBoundThisAndTargetCaptures + capturedArgumentCount, site);
}

// A closure of a bound function answers with the name and the length its target was written with.
static bool findsMembersAsCreated(const Program& program, const FlowResults& flow, const BuiltinParameter& receiver, FunctionID target)
{
    Vector<PropertyKeyID, 2> keys(receiver.namedMembers.size(), [&](size_t index) {
        return program.keyOf(receiver.namedMembers[index]);
    });
    return flow.areClosuresAsCreated(target, keys.span());
}

PlanOrDecline<CallPlan> CallResolver::planFunctionMethod(const CallSite& site)
{
    const Value& value = m_function.at(site.position.value);
    if (value.builtinCallForm() != BuiltinCallForm::Whole)
        return std::unexpected(CallDeclineReason::None);

    const BuiltinOverload& overload = m_program.builtins().at(value.overload());
    if (overload.builtin == Builtins::FunctionPrototypeBind)
        return toCallPlan(planBind(site, overload));
    return toCallPlan(planForwardedCall(site));
}

PlanOrDecline<ForwardedCallPlan> CallResolver::planForwardedCall(const CallSite& site)
{
    if (!typeOf(site.position.value).isAnything())
        return std::unexpected(CallDeclineReason::None);
    std::optional<ForwardedCallPlan> plan = Typed::planForwardedCall(m_program, m_function, builtinCallOf(m_program, m_function.at(site.position.value)));
    if (!plan || (plan->list && !isAlwaysArray(typeOf(plan->list))))
        return std::unexpected(CallDeclineReason::None);
    return *plan;
}

void CallResolver::rewriteForwardedCall(const CallSite& site, const ForwardedCallPlan& plan)
{
    becomeForwardedCall(m_function.at(site.position.value), plan);
    m_context.analyses.didChangeValues();
}

PlanOrDecline<BindPlan> CallResolver::planBind(const CallSite& site, const BuiltinOverload& overload)
{
    BuiltinCall bind = builtinCallOf(m_program, m_function.at(site.position.value));
    if (!bind.receiver)
        return std::unexpected(CallDeclineReason::None);

    CalleeCandidates candidates = calleeCandidates(m_program, m_flow, m_function, bind.receiver, { }, { });
    if (!candidates.isProven() || candidates.proven.size() != 1 || candidates.hasOtherCallees || candidates.mayBeNullish)
        return std::unexpected(candidates.predicted.isEmpty() ? CallDeclineReason::UnknownCallee : CallDeclineReason::NeedsTest);

    FunctionID targetID = candidates.proven[0];
    if (!canBind(m_context, bind, targetID, site.position.value))
        return std::unexpected(CallDeclineReason::CalleeNotCallable);
    if (!findsMembersAsCreated(m_program, m_flow, *overload.receiver, targetID))
        return std::unexpected(CallDeclineReason::UnknownMember);

    unsigned numberOfBound = numberOfBoundArguments(bind);
    FunctionID boundFunction = m_context.derivedFunction(derivedFunctionKey(FunctionRole::BoundFunction, targetID, numberOfBound));
    const Function& target = m_program.at(targetID);
    const Function& bound = m_program.at(boundFunction);
    const CreatorRecord::BoundFunction& record = bound.creator.as<CreatorRecord::BoundFunction>();
    bool capturesThis = target.has(FunctionFlags::HasThis);
    size_t numberOfCaptures = target.captures.size() + capturesThis + record.numberOfCapturedArguments + record.capturesTargetClosure;
    if (record.numberOfCapturedArguments > numberOfBound || numberOfCaptures != bound.captures.size())
        return std::unexpected(CallDeclineReason::None);

    BindPlan plan;
    plan.arm = createDirectCallArm(m_program, m_flow, targetID, { });
    plan.arm.closureCreation = findClosureCreation(m_function, candidates.value, targetID);
    plan.closure = candidates.value;
    plan.boundFunction = boundFunction;
    plan.numberOfCapturedArguments = record.numberOfCapturedArguments;
    plan.capturesThis = capturesThis;
    plan.capturesTargetClosure = record.capturesTargetClosure;
    return plan;
}

void CallResolver::rewriteBind(const CallSite& site, const BindPlan& plan)
{
    m_expansion.begin(site.position);
    std::span<const ValueID> arguments = m_expansion.original().argumentsOperands();

    Vector<ValueID, 8> captures = capturesOf(plan.arm, plan.closure, { });
    if (plan.capturesThis)
        captures.append(arguments.empty() ? m_builder.appendUndefined() : arguments[0]);
    for (uint32_t index = 0; index < plan.numberOfCapturedArguments; ++index)
        captures.append(arguments[index + 1]);
    if (plan.capturesTargetClosure)
        captures.append(plan.closure);

    Value creation = IRBuilder::make(Opcode::NewClosure);
    creation.setFunction(plan.boundFunction);
    creation.setCapturesOperands(captures.span());
    m_expansion.finishArm(m_expansion.append(WTF::move(creation)));
    m_expansion.end();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
