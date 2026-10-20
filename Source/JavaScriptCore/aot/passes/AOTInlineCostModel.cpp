/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineCostModel.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTKnownValue.h"
#include "AOTSinkAllocations.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <ranges>
#include <wtf/MathExtras.h>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

namespace InlineModel {

constexpr uint32_t callOverheadBytes = 16;
constexpr uint32_t argumentOverheadBytes = 4;
constexpr uint32_t knownCalleeArgumentBytes = 56;
constexpr uint32_t knownReceiverAccessBytes = 64;
constexpr uint32_t handlerIntroductionBytes = 320;

} // namespace InlineModel

namespace {

class ConditionEvaluator {
public:
    ConditionEvaluator(const Program& program, const Function& caller, ValueID argument, const Function& callee, ValueID parameter)
        : m_program(program)
        , m_caller(caller)
        , m_argument(argument)
        , m_callee(callee)
        , m_parameter(parameter)
    {
    }

    KnownValue evaluate(ValueID) const;

private:
    const Program& m_program;
    const Function& m_caller;
    ValueID m_argument;
    const Function& m_callee;
    ValueID m_parameter;
};

KnownValue ConditionEvaluator::evaluate(ValueID id) const
{
    if (id == m_parameter)
        return knownValueOf(m_program, m_caller, m_argument);

    const Value& value = m_callee.at(id);
    if (value.opcode == Opcode::TypeTest && sameObjectThroughCasts(m_callee, value.valueOperand()) == m_parameter) {
        switch (evaluateTypeTest(m_program, m_program.typeOf(m_caller, m_argument), m_program.at(value.testedType()))) {
        case KnownBoolean::True:
            return KnownValue::boolean(true);
        case KnownBoolean::False:
            return KnownValue::boolean(false);
        case KnownBoolean::Unknown:
            break;
        }
    }

    if (isRematerializable(value))
        return knownValueOf(m_program, m_callee, id);
    return evaluateWithKnownOperands(m_program, m_callee, value, [&](ValueID operand) {
        return evaluate(operand);
    });
}

} // anonymous namespace

static ValueID argumentAt(const Value& call, size_t entryParameter)
{
    if (entryParameter >= call.numberOfOperands())
        return { };
    return call.operand(static_cast<uint32_t>(entryParameter));
}

static uint32_t computeFoldedBytes(const Program& program, const Function& caller, const Value& call, const InliningSummary& summary)
{
    const Function& callee = program.at(call.function());
    uint32_t foldedBytes = 0;
    for (const ParameterCondition& condition : summary.conditions) {
        ValueID argument = argumentAt(call, condition.entryParameter);
        if (!argument)
            continue;
        ConditionEvaluator evaluator(program, caller, argument, callee, callee.at(entryBlock).parameters[condition.entryParameter]);
        switch (evaluator.evaluate(condition.condition).toBoolean(program)) {
        case KnownBoolean::True:
            foldedBytes = saturatingSum<uint32_t>(foldedBytes, condition.bytesOnlyIfFalse);
            break;
        case KnownBoolean::False:
            foldedBytes = saturatingSum<uint32_t>(foldedBytes, condition.bytesOnlyIfTrue);
            break;
        case KnownBoolean::Unknown:
            break;
        }
    }
    ASSERT(foldedBytes <= summary.codeBytes);
    return foldedBytes;
}

static uint64_t computeKnownCalleeBenefit(const FunctionPassContext& context, const Value& call, const InliningSummary& summary)
{
    uint64_t cycleWeight = context.limit(LimitID::InlineCycleWeight);
    uint64_t bytes = 0;
    for (size_t entryParameter : summary.calledParameters) {
        ValueID argument = argumentAt(call, entryParameter);
        if (!argument || !context.program.typeOf(context.function, argument).singleFunction())
            continue;
        uint64_t weight = summary.parametersCalledInCycles.get(entryParameter) ? cycleWeight : 1;
        bytes += InlineModel::knownCalleeArgumentBytes * weight;
    }
    return bytes;
}

static bool hasKnownLayout(const Program& program, const Function& caller, ValueID argument)
{
    if (program.typeOf(caller, argument).layouts)
        return true;
    return proofsOfAllocation(program, caller, caller.at(sameObjectThroughCasts(caller, argument))).contains(Proof::Shape);
}

static uint64_t computeKnownReceiverBenefit(const FunctionPassContext& context, const Value& call, const InliningSummary& summary)
{
    uint64_t bytes = 0;
    for (const ReceiverParameter& receiver : summary.receiverParameters) {
        ValueID argument = argumentAt(call, receiver.entryParameter);
        if (argument && hasKnownLayout(context.program, context.function, argument))
            bytes += uint64_t { InlineModel::knownReceiverAccessBytes } * receiver.accessesByName;
    }
    return bytes;
}

static bool hasSingleCallSite(const CallGraph& callGraph, FunctionID function)
{
    return callGraph.numberOfCallSites(function) == 1 && callGraph.callSiteSet(function) == CallSiteSet::Complete;
}

static bool isOnlyCallSite(const CallGraph& callGraph, const Function& caller, const Value& call)
{
    if (!hasSingleCallSite(callGraph, call.function()))
        return false;
    for (uint32_t frame = call.inlineFrame; frame; frame = caller.inlineFrames[frame].parent) {
        if (!hasSingleCallSite(callGraph, caller.inlineFrames[frame].callee))
            return false;
    }
    return true;
}

// A body with one call site moves into its caller and is no copy. So what is called from it has the depth that it had there.
static unsigned depthOfCopies(const Program& program, const CallGraph& callGraph, const Function& caller, uint32_t inlineFrame)
{
    unsigned depth = inlineDepthOf(program, caller, inlineFrame);
    Vector<FunctionID, 8> calleesFromInnermost;
    for (uint32_t frame = inlineFrame; frame; frame = caller.inlineFrames[frame].parent)
        calleesFromInnermost.append(caller.inlineFrames[frame].callee);
    for (FunctionID callee : calleesFromInnermost | std::views::reverse) {
        if (!hasSingleCallSite(callGraph, callee))
            break;
        depth -= !isWrapperCreatedByEngine(program.at(callee));
    }
    return depth;
}

// An entrypoint tests what it was given and enters the body. Where nothing else enters the body it takes the body in, and the body goes.
// Beside a body that compiled code enters too, a copy costs its bytes. A hot entrypoint is worth them: its copy is compiled for the calls by value alone.
static bool wouldCopySharedBodyIntoEntrypoint(const FunctionPassContext& context, const Value& call, HeatClass heat)
{
    // The entrypoints of a group exist for calls in hot code, and a callee set names every member.
    if (context.function.role != FunctionRole::GenericEntrypoint || isHot(heat))
        return false;
    // The call graph counts the call of an entrypoint that existed when the graph was built.
    const CallGraph& callGraph = context.inputs.callGraph;
    unsigned ownCallSites = context.id.index() < callGraph.numberOfFunctions() ? 1 : 0;
    return callGraph.numberOfCallSitesInCode(call.function()) > ownCallSites;
}

InlineVerdict computeInlineVerdict(FunctionPassContext& context, const InliningSummary& summary, ValueID call)
{
    const Program& program = context.program;
    const Function& caller = context.function;
    const Value& value = caller.at(call);
    RELEASE_ASSERT(value.opcode == Opcode::CallDirect);

    bool canInlineWholeBody = summary.canInlineWholeBody();
    if (!canInlineWholeBody && !summary.canInlineHead())
        return { .decision = InlineDecision::Barrier };

    uint32_t foldedBytes = computeFoldedBytes(program, caller, value, summary);
    uint32_t removedBytes = saturatingSum<uint32_t>(foldedBytes, estimatedCodeBytes(program, caller, value));
    uint32_t cost = summary.codeBytes > removedBytes ? summary.codeBytes - removedBytes : 0;
    bool introducesHandlers = !summary.blocksWithHandlers.isEmpty() && !context.analyses.cfg().hasHandlers();
    if (introducesHandlers)
        cost = saturatingSum<uint32_t>(cost, InlineModel::handlerIntroductionBytes);

    HeatClass heat = heatClassOf(context, context.analyses.useLists().blockOf(call));
    unsigned depth = depthOfCopies(program, context.inputs.callGraph, caller, value.inlineFrame);
    uint32_t budget = wouldCopySharedBodyIntoEntrypoint(context, value, heat) ? 0 : inlineBudgetBytes(context, heat, depth);
    InlineVerdict verdict { .costBytes = cost, .budgetBytes = budget, .introducesHandlers = introducesHandlers };

    auto addBenefit = [&](InlineBenefit benefit, uint64_t bytes) {
        verdict.benefits.add(benefit);
        verdict.benefitBytes = clampTo<uint32_t>(verdict.benefitBytes + bytes);
    };
    auto isWithinBudget = [&] {
        return cost <= uint64_t { verdict.budgetBytes } + verdict.benefitBytes;
    };

    if (canInlineWholeBody && (value.isAlwaysInlined() || summary.isWrapper)) {
        addBenefit(value.isAlwaysInlined() ? InlineBenefit::CallOverhead : InlineBenefit::Wrapper, cost);
        verdict.decision = InlineDecision::Inline;
        return verdict;
    }

    if (foldedBytes)
        verdict.benefits.add(InlineBenefit::FoldedCode);
    uint64_t callOverheadBytes = InlineModel::callOverheadBytes + uint64_t { InlineModel::argumentOverheadBytes } * value.numberOfOperands();
    addBenefit(InlineBenefit::CallOverhead, callOverheadBytes);
    if (uint64_t bytes = computeKnownCalleeBenefit(context, value, summary))
        addBenefit(InlineBenefit::KnownCalleeArgument, bytes);
    if (uint64_t bytes = computeKnownReceiverBenefit(context, value, summary))
        addBenefit(InlineBenefit::KnownReceiver, bytes);

    bool isTooDeep = false;
    if (canInlineWholeBody) {
        if (isOnlyCallSite(context.inputs.callGraph, caller, value) && !context.exceeds(LimitID::MaxOnlyCallSiteBytes, cost, call))
            addBenefit(InlineBenefit::OnlyCallSite, cost);
        // The limit bounds what each level adds to the caller. A body that costs no more than its call adds nothing.
        isTooDeep = cost > callOverheadBytes && context.exceeds(LimitID::MaxInlineDepth, depth + 1, call);
        if (!isTooDeep && !isWithinBudget() && !isCold(heat)) {
            if (unsigned allocations = sinkableAllocationsAfterInlining(context, call, summary.returnsFreshAggregate))
                addBenefit(InlineBenefit::SunkAllocation, allocations * context.limit(LimitID::InlineSunkAllocationBytes));
        }
    }

    if (canInlineWholeBody && !isTooDeep && isWithinBudget())
        verdict.decision = InlineDecision::Inline;
    else if (isHot(heat) && summary.canInlineHead()) {
        verdict.decision = InlineDecision::InlineHead;
        verdict.costBytes = summary.headBytes;
        verdict.introducesHandlers = false;
    } else if (!canInlineWholeBody)
        verdict.decision = InlineDecision::Barrier;
    else
        verdict.decision = isTooDeep ? InlineDecision::DepthLimit : InlineDecision::CostExceedsBenefit;
    return verdict;
}

unsigned inlineDepthOf(const Program& program, const Function& caller, uint32_t inlineFrame)
{
    unsigned depth = 0;
    for (uint32_t frame = inlineFrame; frame; frame = caller.inlineFrames[frame].parent)
        depth += !isWrapperCreatedByEngine(program.at(caller.inlineFrames[frame].callee));
    return depth;
}

uint32_t inlineBudgetBytes(const FunctionPassContext& context, HeatClass heat, unsigned depth)
{
    uint64_t budget = 0;
    switch (heat) {
    case HeatClass::RunsOnce:
    case HeatClass::Cold:
        budget = context.limit(LimitID::InlineBudgetColdBytes);
        break;
    case HeatClass::Unknown:
    case HeatClass::Warm:
        budget = context.limit(LimitID::InlineBudgetWarmBytes);
        break;
    case HeatClass::Hot:
        budget = context.limit(LimitID::InlineBudgetHotBytes);
        break;
    }
    return clampTo<uint32_t>(budget >> std::min(depth, 31u));
}

ASCIILiteral nameOf(InlineDecision decision)
{
    switch (decision) {
    case InlineDecision::Unknown:
        return "Unknown"_s;
    case InlineDecision::Inline:
        return "Inline"_s;
    case InlineDecision::InlineHead:
        return "InlineHead"_s;
    case InlineDecision::Barrier:
        return "Barrier"_s;
    case InlineDecision::SignatureMismatch:
        return "SignatureMismatch"_s;
    case InlineDecision::ClosesCycle:
        return "ClosesCycle"_s;
    case InlineDecision::DepthLimit:
        return "DepthLimit"_s;
    case InlineDecision::CallerSizeLimit:
        return "CallerSizeLimit"_s;
    case InlineDecision::CostExceedsBenefit:
        return "CostExceedsBenefit"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StatisticID statisticFor(InlineDecision decision)
{
    switch (decision) {
    case InlineDecision::Unknown:
        return StatisticID::InlineDecisionUnknown;
    case InlineDecision::Inline:
        return StatisticID::InlineDecisionInline;
    case InlineDecision::InlineHead:
        return StatisticID::InlineDecisionInlineHead;
    case InlineDecision::Barrier:
        return StatisticID::InlineDecisionBarrier;
    case InlineDecision::SignatureMismatch:
        return StatisticID::InlineDecisionSignatureMismatch;
    case InlineDecision::ClosesCycle:
        return StatisticID::InlineDecisionClosesCycle;
    case InlineDecision::DepthLimit:
        return StatisticID::InlineDecisionDepthLimit;
    case InlineDecision::CallerSizeLimit:
        return StatisticID::InlineDecisionCallerSizeLimit;
    case InlineDecision::CostExceedsBenefit:
        return StatisticID::InlineDecisionCostExceedsBenefit;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StatisticID statisticFor(InlineBenefit benefit)
{
    switch (benefit) {
    case InlineBenefit::CallOverhead:
        return StatisticID::InlineBenefitCallOverhead;
    case InlineBenefit::FoldedCode:
        return StatisticID::InlineBenefitFoldedCode;
    case InlineBenefit::KnownCalleeArgument:
        return StatisticID::InlineBenefitKnownCalleeArgument;
    case InlineBenefit::KnownReceiver:
        return StatisticID::InlineBenefitKnownReceiver;
    case InlineBenefit::SunkAllocation:
        return StatisticID::InlineBenefitSunkAllocation;
    case InlineBenefit::OnlyCallSite:
        return StatisticID::InlineBenefitOnlyCallSite;
    case InlineBenefit::Wrapper:
        return StatisticID::InlineBenefitWrapper;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
