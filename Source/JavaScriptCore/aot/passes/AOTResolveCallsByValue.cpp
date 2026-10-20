/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCalls.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTFlowResults.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static Vector<FunctionID, 4> collectMembers(const FunctionPassContext& context, const CalleeCandidates& candidates, const ScopedLambda<bool(FunctionID)>& canEnter)
{
    Vector<FunctionID, 4> members;
    for (FunctionID member : candidates.proven) {
        if (canEnter(member))
            members.append(member);
    }
    // The value of a provisional id depends on which thread asked for the function first.
    std::ranges::sort(members, [&](FunctionID a, FunctionID b) {
        return context.inputs.derivedFunctions->compare(a, b) < 0;
    });
    std::ranges::stable_partition(members, [&](FunctionID member) {
        return candidates.predicted.contains(member);
    });
    return members;
}

// Callees that are equally likely get an arm each or none: arms for some of them would be chosen by id, and the next run would take as many from the rest.
static void keepPredictedMembers(Vector<FunctionID, 4>& members, const CalleeCandidates& candidates, uint64_t maxDispatchCases)
{
    size_t numberOfPredictedMembers = std::ranges::count_if(members, [&](FunctionID member) {
        return candidates.predicted.contains(member);
    });
    members.shrink(numberOfPredictedMembers <= maxDispatchCases ? numberOfPredictedMembers : 0);
}

static CalleeSetID createRestCallees(const Program& program, const FlowResults& flow, TypeArena& types, CalleeSetID stamp, std::span<const FunctionID> dispatchedCallees)
{
    if (!stamp)
        return { };

    FunctionList versionsOfArms;
    for (FunctionID closureFunction : dispatchedCallees) {
        versionsOfArms.add(closureFunction);
        versionsOfArms.add(versionToEnter(program, flow, closureFunction, { }));
        versionsOfArms.add(versionToEnter(program, flow, closureFunction, stamp));
    }

    const CalleeSet& stamped = program.at(stamp);
    CalleeSet rest { { }, stamped.builtins, stamped.isComplete };
    for (FunctionID version : stamped.functions) {
        if (!versionsOfArms.contains(version))
            rest.functions.append(version);
    }
    return types.intern(WTF::move(rest));
}

PlanOrDecline<Vector<FunctionID, 4>> CallResolver::chooseDispatchedCallees(const CallSite& site, const CalleeCandidates& candidates, NOESCAPE const CanEnterCallee& canEnter)
{
    Vector<FunctionID, 4> members = collectMembers(m_context, candidates, canEnter);
    if (members.isEmpty())
        return std::unexpected(CallDeclineReason::CalleeNotCallable);
    if (m_context.exceeds(LimitID::MaxDispatchCases, members.size(), site.position.value))
        keepPredictedMembers(members, candidates, m_context.limit(LimitID::MaxDispatchCases));
    if (members.isEmpty())
        return std::unexpected(CallDeclineReason::TooManyCallees);
    return members;
}

CalleeSplit CallResolver::createCalleeSplit(const CallSite& site, const CalleeCandidates& candidates, std::span<const FunctionID> dispatchedCallees, CalleeSetID stamp)
{
    BlockFrequency frequency = m_function.at(site.position.block).frequency;
    BlockFrequency unlikelyFrequency = rarerOf(frequency, BlockFrequency::Unlikely);

    CalleeSplit result;
    ValueSplit& split = result.split;
    split.value = candidates.value;
    Type covered;
    size_t numberOfPredictedArms = 0;
    for (FunctionID member : dispatchedCallees) {
        bool isPredicted = candidates.predicted.contains(member);
        bool isLikely = isPredicted || candidates.predicted.isEmpty();
        split.arms.append(SplitArm { m_context.types.intern(Type::forFunction(member)), isLikely ? frequency : unlikelyFrequency });
        covered.functions.add(member);
        numberOfPredictedArms += isPredicted;
    }

    split.hasRest = !typeOf(candidates.value).isSubsetOf(covered, m_program);
    split.restFrequency = numberOfPredictedArms < candidates.predicted.size() ? frequency : unlikelyFrequency;
    if (split.hasRest)
        result.restCallees = createRestCallees(m_program, m_flow, m_context.types, stamp, dispatchedCallees);
    return result;
}

PlanOrDecline<CalleeDispatchPlan> CallResolver::planCalleeDispatch(const CallSite& site)
{
    RELEASE_ASSERT(site.opcode == Opcode::Call);
    const Value& call = m_function.at(site.position.value);
    CalleeCandidates candidates = calleeCandidates(m_program, m_flow, m_function, call.operand(OperandIndex::Call::callee), call.callees(), call.accessSite());
    requireArmsForCallSiteVersions(m_program, candidates, call.callees());
    if (!candidates.isProven())
        return std::unexpected(declineOfUnprovenCallee(candidates));

    PlanOrDecline<Vector<FunctionID, 4>> members = chooseDispatchedCallees(site, candidates, [&](FunctionID member) {
        return mayBeCalledDirectly(m_program, member);
    });
    if (!members)
        return std::unexpected(members.error());

    CalleeDispatchPlan plan;
    plan.callee = candidates.value;
    for (FunctionID member : *members) {
        CallArm arm = createDirectCallArm(m_program, m_flow, member, call.callees());
        arm.closureCreation = findClosureCreation(m_function, candidates.value, member);
        plan.arms.append(arm);
    }
    plan.split = createCalleeSplit(site, candidates, members->span(), call.callees());
    return plan;
}

void CallResolver::rewriteCalleeDispatch(const CallSite& site, const CalleeDispatchPlan& plan)
{
    m_expansion.begin(site.position);
    const Value& original = m_expansion.original();
    ValueID thisValue = original.operand(OperandIndex::Call::thisValue);
    std::span<const ValueID> arguments = original.variadicOperands();

    emitSplit(m_expansion, plan.split.split,
        [&](uint32_t armIndex, ValueID narrowed) {
            ValueID result = appendCall(plan.arms[armIndex], thisValue, narrowed, arguments);
            if (typeOf(result).isNever()) {
                m_expansion.finishArmAsUnreachable();
                return;
            }
            m_expansion.finishArm(result);
        },
        [&](ValueID narrowed) {
            m_expansion.finishArm(m_expansion.appendOriginal({ OperandIndex::Call::callee, narrowed, { }, plan.split.restCallees }));
        });
    m_expansion.end();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
