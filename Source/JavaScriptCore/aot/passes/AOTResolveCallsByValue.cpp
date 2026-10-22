/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTFlowApply.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTTypeTestSite.h"
#include "AOTValueSubstitution.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static Vector<FunctionID, 4> collectMembers(const FunctionPassContext& context, const CalleeCandidates& candidates, const ScopedLambda<bool(FunctionID)>& canEnter)
{
    Vector<FunctionID, 4> members;
    for (FunctionID member : candidates.listed()) {
        if (canEnter(member))
            members.append(member);
    }
    // The value of a provisional id depends on which thread asked for the function first.
    std::ranges::sort(members, [&](FunctionID a, FunctionID b) {
        return context.inputs.derivedFunctions->compare(a, b) < 0;
    });
    return members;
}

// What stays behind the arms for expected callees expects them as well.
static bool standsBehindFailedTest(FunctionPassContext& context, const CallSite& site, const CalleeCandidates& candidates)
{
    if (candidates.isProven() && !candidates.hasOtherCallees)
        return false;
    context.planAsksAboutPlace = true;
    return std::ranges::any_of(candidates.listed(), [&](FunctionID member) {
        Value test = makeTypeTest(candidates.value, context.types.intern(Type::forFunction(member)));
        return context.analyses.findEarlierTest(test, site.position.block) == EarlierTest::Failed;
    });
}

PlanOrDecline<Vector<FunctionID, 4>> CallResolver::chooseDispatchedCallees(const CallSite& site, const CalleeCandidates& candidates, NOESCAPE const CanEnterCallee& canEnter)
{
    if (!candidates.isDispatchedAtOperation())
        return std::unexpected(declineOfUnprovenCallee(candidates));
    if (standsBehindFailedTest(m_context, site, candidates))
        return std::unexpected(CallDeclineReason::None);

    Vector<FunctionID, 4> members = collectMembers(m_context, candidates, canEnter);
    if (members.isEmpty())
        return std::unexpected(CallDeclineReason::CalleeNotCallable);
    // Callees get an arm each or none: arms for some of them would be chosen by id, and the next run would take as many from the rest.
    // Arms for all the callees there are take the place of the call. Where another function may come the call stays behind them, and it serves several callees as well as their arms would.
    bool listsEveryCallee = candidates.isProven() && !candidates.hasOtherCallees;
    // What exceeds a limit is reported, site by site.
    m_context.planAsksAboutPlace |= listsEveryCallee || members.size() <= 1;
    if ((!listsEveryCallee && members.size() > 1) || m_context.exceeds(LimitID::MaxDispatchCases, members.size(), site.position.value))
        return std::unexpected(CallDeclineReason::TooManyCallees);
    return members;
}

ValueSplit CallResolver::createCalleeSplit(const CallSite& site, const CalleeCandidates& candidates, std::span<const FunctionID> dispatchedCallees)
{
    BlockFrequency frequency = m_function.at(site.position.block).frequency;

    ValueSplit split;
    split.value = candidates.value;
    Type covered;
    for (FunctionID member : dispatchedCallees) {
        split.arms.append(SplitArm { m_context.types.intern(Type::forFunction(member)), frequency });
        covered.functions.add(member);
    }

    split.hasRest = !typeOf(candidates.value).isSubsetOf(covered, m_program);
    // A function without a body is one that nothing enters.
    bool leavesOutListedCallee = std::ranges::any_of(candidates.listed(), [&](FunctionID listed) {
        return m_program.at(listed).hasBody() && !covered.functions.contains(listed);
    });
    split.restFrequency = leavesOutListedCallee ? frequency : rarerOf(frequency, BlockFrequency::Unlikely);
    return split;
}

CallArm CallResolver::createCalleeArm(const CalleeCandidates& candidates, FunctionID member) const
{
    CallArm arm = createDirectCallArm(member);
    arm.closureCreation = findClosureCreation(m_function, candidates.value, member);
    return arm;
}

PlanOrDecline<CalleeDispatchPlan> CallResolver::planCalleeDispatch(const CallSite& site)
{
    RELEASE_ASSERT(site.opcode == Opcode::Call);
    const Value& call = m_function.at(site.position.value);
    CalleeCandidates candidates = calleeCandidatesOf(m_program, m_function, call, call.operand(OperandIndex::Call::callee));

    PlanOrDecline<Vector<FunctionID, 4>> members = chooseDispatchedCallees(site, candidates, [&](FunctionID member) {
        return mayBeCalledDirectly(m_program, member);
    });
    if (!members)
        return std::unexpected(members.error());

    CalleeDispatchPlan plan;
    plan.callee = candidates.value;
    plan.proof = candidates.proof;
    for (FunctionID member : *members)
        plan.arms.append(createCalleeArm(candidates, member));
    plan.split = createCalleeSplit(site, candidates, members->span());
    return plan;
}

// Says yes only where planCalleeDispatch() plans one arm without a test and rewriteCalleeDispatch() ends that arm with the result of the call as it is.
std::optional<OnlyCalleePlan> CallResolver::planCallOfOnlyCallee(const CallSite& site)
{
    if (m_resolution != Resolution::Everything || site.opcode != Opcode::Call || !site.memberRead.isEmpty())
        return std::nullopt;
    const Value& call = m_function.at(site.position.value);
    std::optional<OnlyCallee> only = findOnlyCallee(m_program, m_function, call.operand(OperandIndex::Call::callee));
    if (!only || !mayBeCalledDirectly(m_program, only->function))
        return std::nullopt;
    const Terminator& terminator = m_function.at(site.position.block).terminator;
    if (m_context.limit(LimitID::MaxDispatchCases) < 1 || (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == site.position.value))
        return std::nullopt;

    const Function& callee = m_program.at(only->function);
    ValueID thisValue = call.operand(OperandIndex::Call::thisValue);
    // appendCall() puts no call where the receiver cannot be what the callee takes.
    if (thisValue && callee.has(FunctionFlags::HasThis) && callee.thisType.type && !callee.thisType.flags.contains(PositionFlags::IsExpected)) {
        const Type& declared = m_program.at(callee.thisType.type);
        if (typeOf(thisValue).hasOnlyLayouts(m_program) && declared.hasOnlyLayouts(m_program) && !typeOf(thisValue).overlaps(declared, m_program))
            return std::nullopt;
    }
    TypeID result = TypingContext { m_program, m_function, m_context.types }.typeIDOf(callee.result());
    if (TypeTable::isProvisional(result) || result == TypeTable::never || !m_program.at(result).isSubsetOf(typeOf(site.position.value), m_program))
        return std::nullopt;
    return OnlyCalleePlan { only->value, only->function, findClosureCreation(m_function, only->value, only->function) };
}

void CallResolver::rewriteCallOfOnlyCallee(const CallSite& site, const OnlyCalleePlan& plan)
{
    const Value original = m_function.at(site.position.value);
    const Function& callee = m_program.at(plan.function);
    ProgramPoint point = site.position.point();
    m_builder.setOrigin(original.origin());
    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertBeforeCall = [&](auto&& value) {
        return m_builder.insertBefore(point, WTF::move(value));
    };
    InsertValue insert = insertBeforeCall;
    ValueInsertion insertion { m_builder.context(), m_function, m_context.types, original.origin(), insert };

    if (m_context.build.validates())
        insert(makeValidateCallee(sameObjectThroughCasts(m_function, plan.callee), m_function.at(plan.callee).type));
    Vector<ValueID, 8> captures;
    if (!callee.captures.isEmpty())
        captures = plan.closureCreation ? Vector<ValueID, 8>(m_function.at(plan.closureCreation).variadicOperands()) : insertCaptures(insertion, plan.function, plan.callee);
    ValueID result = insert(makeDirectCall(insertion, {
        .callee = plan.function,
        .captures = captures.span(),
        .thisValue = original.operand(OperandIndex::Call::thisValue),
        .calleeValue = callee.has(FunctionFlags::TakesCallee) ? plan.callee : ValueID(),
        .arguments = original.variadicOperands(),
        .accessSite = original.accessSite(),
        .isNeverInlined = original.isNeverInlined(),
        .isDemotedTailCall = FlagMask::isDemotedTailCall(original.opcode) && original.isDemotedTailCall(),
    }));
    RELEASE_ASSERT(typeOf(result).isSubsetOf(m_program.at(original.type), m_program) && !typeOf(result).isNever());
    becomeUpcast(m_function, site.position.value, result);
    m_context.analyses.didChangeValues();
}

void CallResolver::rewriteCalleeDispatch(const CallSite& site, const CalleeDispatchPlan& plan)
{
    m_expansion.begin(site.position);
    const Value& original = m_expansion.original();
    ValueID thisValue = original.operand(OperandIndex::Call::thisValue);
    std::span<const ValueID> arguments = original.variadicOperands();

    if (plan.proof == CalleeProof::Type)
        appendCalleeValidation(m_builder, plan.callee);
    emitSplit(m_expansion, plan.split,
        [&](uint32_t armIndex, ValueID narrowed) {
            m_expansion.finishArm(appendCall(plan.arms[armIndex], thisValue, narrowed, arguments));
        },
        [&](ValueID narrowed) {
            m_expansion.finishArm(m_expansion.appendOriginal({ OperandIndex::Call::callee, narrowed }));
        });
    m_expansion.end();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
