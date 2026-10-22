/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineCostModel.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTCalleeVersions.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTCoroutines.h"
#include "AOTCreateEntrypoints.h"
#include "AOTFlowResults.h"
#include "AOTFreshResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTKnownValue.h"
#include "AOTRepresentation.h"
#include "AOTSinkAllocations.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <atomic>
#include <ranges>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/SaturatingArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

namespace InlineModel {

constexpr uint32_t callOverheadBytes = 16;
constexpr uint32_t argumentOverheadBytes = 4;
constexpr uint32_t allocationCallBytes = 24;
// What goes with the call by value of an argument that is known: a direct call takes its place.
constexpr uint32_t knownCalleeArgumentBytes = 16;
// A loop of the library calls what it is handed once for each element, so the direct call in the copy pays more often than in a callee that calls its parameter in a cycle of any kind.
constexpr uint32_t libraryLoopFactor = 2;
constexpr uint32_t knownReceiverAccessBytes = 64;
// What goes with an access by name to a receiver that is known: an access by slot takes its place.
constexpr uint32_t slotInPlaceOfNameBytes = 12;
constexpr unsigned depthWithoutBets = 3;
constexpr uint32_t constructorBytes = 256;
constexpr uint8_t percentileOfMostEnteredFunctions = 98;
constexpr uint32_t storeIntoOwnAllocationBytes = 32;
constexpr uint32_t handlerIntroductionBytes = 320;
// What an entrypoint that has to convert the result pays to enter the tests: a frame, a call, a return.
constexpr uint32_t bodyCopiedIntoHotEntrypointBytes = 48;

} // namespace InlineModel

namespace {

// A benefit above the bytes that go with the call is a bet that the copy runs often enough to pay for its bytes.
enum class Bets : uint8_t { OnSpeed, None };

class ConditionEvaluator {
public:
    ConditionEvaluator(const Program& program, const Function& caller, ValueID argument, const Function& callee)
        : m_program(program)
        , m_caller(caller)
        , m_argument(argument)
        , m_callee(callee)
    {
    }

    KnownValue evaluate(ValueID) const;

private:
    const Program& m_program;
    const Function& m_caller;
    ValueID m_argument;
    const Function& m_callee;
};

// Every block parameter that a summarized condition depends on has the identity of the one entry parameter that decides it.
KnownValue ConditionEvaluator::evaluate(ValueID id) const
{
    const Value& value = m_callee.at(id);
    if (value.opcode == Opcode::Parameter)
        return knownValueOf(m_program, m_caller, m_argument);

    if (value.opcode == Opcode::TypeTest && m_callee.at(sameObjectThroughCasts(m_callee, value.valueOperand())).opcode == Opcode::Parameter) {
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

static bool passesEveryEntryCheck(const Program& program, const Function& caller, const Value& call)
{
    const Function& checks = program.at(call.function());
    auto& parameters = checks.at(entryBlock).parameters;
    return std::ranges::all_of(checks.blocks, [&](const Block& block) {
        return std::ranges::all_of(block.values, [&](ValueID id) {
            const Value& test = checks.at(id);
            if (test.opcode != Opcode::TypeTest)
                return true;
            size_t index = parameters.find(test.valueOperand());
            ValueID argument = index == notFound ? ValueID() : argumentAt(call, index);
            return argument && program.typeOf(caller, argument).isSubsetOf(program.at(test.testedType()), program);
        });
    });
}

// The tests only pass their arguments on, so what the caller knows of the function behind them is lost at a call of the tests.
static bool knowsMoreThanEntryChecksPassOn(FunctionPassContext& context, ValueID callID)
{
    const Program& program = context.program;
    const Function& caller = context.function;
    const Value& call = caller.at(callID);
    if (willEnterAwaitedCalleeWithoutPromise(context, callID))
        return true;
    FunctionID body = program.at(call.function()).creator.as<CreatorRecord::EntryChecks>().body;
    for (size_t entryParameter : context.inputs.inlining.of(program, body).calledParameters) {
        ValueID argument = argumentAt(call, entryParameter);
        if (argument && (program.typeOf(caller, argument).singleFunction() || singleBuiltinFunction(program, program.typeOf(caller, argument))) && canHaveCalleeVersion(program, body, static_cast<uint32_t>(entryParameter)))
            return true;
    }
    return false;
}

static uint32_t computeFoldedBytes(const Program& program, const Function& caller, const Value& call, const InliningSummary& summary)
{
    const Function& callee = program.at(call.function());
    Vector<DominatedRange, 4> foldedRanges;
    bool everyTestPasses = true;
    for (const ParameterCondition& condition : summary.conditions) {
        ValueID argument = argumentAt(call, condition.entryParameter);
        if (!argument) {
            everyTestPasses &= !condition.isTestInFrontOfSharedBytes;
            continue;
        }
        ConditionEvaluator evaluator(program, caller, argument, callee);
        KnownBoolean outcome = evaluator.evaluate(condition.condition).toBoolean(program);
        everyTestPasses &= outcome == KnownBoolean::True || !condition.isTestInFrontOfSharedBytes;
        switch (outcome) {
        case KnownBoolean::True:
            foldedRanges.append(condition.rangeIfFalse);
            break;
        case KnownBoolean::False:
            foldedRanges.append(condition.rangeIfTrue);
            break;
        case KnownBoolean::Unknown:
            break;
        }
    }
    for (const ParameterSwitch& parameterSwitch : summary.switches) {
        ValueID argument = argumentAt(call, parameterSwitch.entryParameter);
        std::optional<int64_t> constant;
        if (argument && !parameterSwitch.isOnStrings)
            constant = integerConstantThroughCasts(program, caller, argument);
        else if (StringID literal = argument ? program.typeOf(caller, argument).singleStringLiteral() : StringID())
            constant = literal.index();
        if (!constant)
            continue;
        bool isListed = false;
        for (size_t position = 0; position < parameterSwitch.bytesOnlyInCase.size(); ++position) {
            if (parameterSwitch.bytesOnlyInCase[position].first == *constant)
                isListed = true;
            else
                foldedRanges.append(parameterSwitch.caseRanges[position]);
        }
        if (isListed)
            foldedRanges.append(parameterSwitch.rangeOtherwise);
    }

    uint32_t foldedBytes = everyTestPasses ? summary.bytesOnlyIfSomeTestFails : 0;
    auto addIfFolded = [&](const DominatedRange& range, uint32_t bytes) {
        bool isFolded = foldedRanges.containsIf([&](const DominatedRange& folded) {
            return folded.contains(range);
        });
        if (isFolded)
            foldedBytes = saturatingSum<uint32_t>(foldedBytes, bytes);
    };
    for (const ParameterCondition& condition : summary.conditions) {
        addIfFolded(condition.rangeIfTrue, condition.bytesOnlyIfTrue);
        addIfFolded(condition.rangeIfFalse, condition.bytesOnlyIfFalse);
    }
    for (const ParameterSwitch& parameterSwitch : summary.switches) {
        for (size_t position = 0; position < parameterSwitch.bytesOnlyInCase.size(); ++position)
            addIfFolded(parameterSwitch.caseRanges[position], parameterSwitch.bytesOnlyInCase[position].second);
        addIfFolded(parameterSwitch.rangeOtherwise, parameterSwitch.bytesOnlyOtherwise);
    }
    ASSERT(foldedBytes <= summary.codeBytes);
    return foldedBytes;
}

static bool isHeldByCallAlone(FunctionPassContext& context, ValueID value, ValueID call)
{
    const UseLists& useLists = context.analyses.useLists();
    while (true) {
        std::span<const Use> uses = useLists.usesOf(value);
        if (uses.size() != 1 || uses[0].kind != Use::Kind::Operand)
            return false;
        if (uses[0].user == call)
            return true;
        if (!isCastWithoutTest(context.function.at(uses[0].user).opcode))
            return false;
        value = uses[0].user;
    }
}

// A closure that the caller makes for the call alone is not made where the copy calls its function directly.
static uint64_t bytesOfClosureMadeForCall(FunctionPassContext& context, ValueID argument, ValueID call)
{
    const Function& caller = context.function;
    ValueID definition = sameObjectThroughCasts(caller, argument);
    if (caller.at(definition).opcode != Opcode::NewClosure || !isHeldByCallAlone(context, definition, call))
        return 0;
    return estimatedCodeBytes(context.program, caller, caller.at(definition));
}

static uint64_t computeKnownCalleeBenefit(FunctionPassContext& context, ValueID callID, const InliningSummary& summary, Bets bets)
{
    const Value& call = context.function.at(callID);
    uint64_t cycleWeight = bets == Bets::OnSpeed ? context.limit(LimitID::InlineCycleWeight) : 1;
    if (bets == Bets::OnSpeed && context.program.at(call.function()).role == FunctionRole::Library)
        cycleWeight *= InlineModel::libraryLoopFactor;
    uint64_t bytes = 0;
    for (size_t entryParameter : summary.calledParameters) {
        ValueID argument = argumentAt(call, entryParameter);
        if (!argument || (!context.program.typeOf(context.function, argument).singleFunction() && !singleBuiltinFunction(context.program, context.program.typeOf(context.function, argument))))
            continue;
        uint64_t weight = summary.parametersCalledInCycles.get(entryParameter) ? cycleWeight : 1;
        bytes += InlineModel::knownCalleeArgumentBytes * weight + bytesOfClosureMadeForCall(context, argument, callID);
    }
    return bytes;
}

static bool hasKnownLayout(const Program& program, const Function& caller, ValueID argument)
{
    if (program.typeOf(caller, argument).layouts)
        return true;
    return proofsOfAllocation(program, caller, caller.at(sameObjectThroughCasts(caller, argument))).contains(Proof::Shape);
}

static bool isAllocatedByCaller(const Program& program, const Function& caller, ValueID argument)
{
    const Value& allocation = caller.at(sameObjectThroughCasts(caller, argument));
    return allocation.opcode == Opcode::NewObject && allocatesOwnLayout(program, allocation);
}

static bool isKeyedStore(Opcode opcode)
{
    return storedOf(opcode) && receiverOf(opcode) && Value::opcodesWithKey.contains(opcode);
}

// The summary counts neither the bytes nor the stores of these blocks.
static bool isSlowPath(const Function& function, const Block& block)
{
    return block.frequency == BlockFrequency::Rare && function.at(entryBlock).frequency != BlockFrequency::Rare;
}

// The field of an object can make such a closure when it is first read.
static bool capturesAtMostHolder(const Program& program, const Function& function, const Value& closure, ValueID holder)
{
    if (closure.opcode != Opcode::NewClosure || program.at(closure.function()).constructorForm() != ConstructorForm::NotConstructor)
        return false;
    std::span<const ValueID> captures = closure.capturesOperands();
    return captures.empty() || (captures.size() == 1 && sameObjectThroughCasts(function, captures[0]) == holder);
}

// Beside the allocation a store that creates a field becomes an entry of the allocation's list, and the allocation proves what the store tests.
static uint64_t computeOwnAllocationBenefit(const Program& program, const Function& callee, const ReceiverParameter& receiver)
{
    uint64_t bytes = uint64_t { InlineModel::storeIntoOwnAllocationBytes } * receiver.stores;
    ValueID object = callee.at(entryBlock).parameters[receiver.entryParameter];
    for (const Block& block : callee.blocks) {
        if (isSlowPath(callee, block))
            continue;
        for (ValueID id : block.values) {
            const Value& store = callee.at(id);
            if (!isKeyedStore(store.opcode) || sameObjectThroughCasts(callee, store.operand(*receiverOf(store.opcode))) != object)
                continue;
            const Value& stored = callee.at(definitionThroughUpcasts(callee, store.operand(*storedOf(store.opcode))));
            if (capturesAtMostHolder(program, callee, stored, object))
                bytes += estimatedCodeBytes(program, callee, stored);
        }
    }
    return bytes;
}

static uint64_t computeKnownReceiverBenefit(const FunctionPassContext& context, const Value& call, const InliningSummary& summary, Bets bets)
{
    const Program& program = context.program;
    const Function& caller = context.function;
    uint64_t bytesOfAccess = bets == Bets::OnSpeed ? InlineModel::knownReceiverAccessBytes : InlineModel::slotInPlaceOfNameBytes;
    uint64_t bytes = 0;
    for (const ReceiverParameter& receiver : summary.receiverParameters) {
        ValueID argument = argumentAt(call, receiver.entryParameter);
        if (!argument)
            continue;
        if (hasKnownLayout(program, caller, argument))
            bytes += bytesOfAccess * receiver.accessesByName;
        if (receiver.stores && isAllocatedByCaller(program, caller, argument))
            bytes += computeOwnAllocationBenefit(program, program.at(call.function()), receiver);
    }
    return bytes;
}

// Only where the creation of a generator and the loop over it stand in one function is the loop merged with the body of the generator, or takes at least its first step without a suspension.
static bool iteratesGeneratorOfCaller(const FunctionPassContext& context, const Value& call, const InliningSummary& summary)
{
    const Function& caller = context.function;
    for (size_t parameter : summary.iteratedParameters) {
        ValueID argument = argumentAt(call, static_cast<uint32_t>(parameter));
        if (argument && caller.at(sameObjectThroughCasts(caller, argument)).opcode == Opcode::NewGenerator)
            return true;
    }
    return false;
}

// The frame holds the call that leads to the function here.
static unsigned numberOfCallSitesCountedHere(const FunctionPassContext& context, uint32_t frame)
{
    FunctionID holder = frame ? context.function.inlineFrames[frame].callee : context.id;
    return holder.index() < context.inputs.callGraph.numberOfFunctions() ? 1 : 0;
}

// The call graph is built before calls are resolved: a call by value that lists the function may be a direct call by the time its caller is optimized.
static bool mayBeEnteredElsewhere(const FunctionPassContext& context, FunctionID function, uint32_t frame)
{
    const CallGraph& callGraph = context.inputs.callGraph;
    return callGraph.numberOfCallSitesInCode(function) > numberOfCallSitesCountedHere(context, frame) || callGraph.isExpectedByCode(function);
}

// Flow lists a function at every call that it has followed a closure to, and types name it where a built-in or an access may enter it.
static bool isNamedByOtherFunctions(const FunctionPassContext& context, FunctionID function, HeatClass heat)
{
    const CallGraph& callGraph = context.inputs.callGraph;
    if (!context.inputs.flow.hasResults() || function.index() >= callGraph.numberOfFunctions())
        return true;
    // Hot code is worth the copy beside the body that such a call keeps.
    if (callGraph.isExpectedByCode(function) && !isHot(heat))
        return true;
    return std::ranges::any_of(callGraph.callersOf(function), [&](FunctionID other) {
        return other != context.id;
    });
}

// Closures, class records, bound functions and the host name the entrypoint of a function, so what finds the function there enters the body through the call in the entrypoint.
static bool isEnteredThroughCallerAlone(const FunctionPassContext& context, FunctionID function, HeatClass heat)
{
    const Function& caller = context.function;
    const auto* entrypoint = caller.creator.tryAs<CreatorRecord::Entrypoint>();
    if (caller.role != FunctionRole::GenericEntrypoint || !entrypoint || entrypoint->target != function)
        return false;
    if (entrypointVariantsOf(context.program, context.program.at(function)) == EntrypointVariants::Both)
        return false;
    return !isNamedByOtherFunctions(context, function, heat);
}

// The body goes only where the call in the frame is the one call that the graph counts.
static bool hasSingleCallSite(const FunctionPassContext& context, FunctionID function, uint32_t frame, HeatClass heat)
{
    const CallGraph& callGraph = context.inputs.callGraph;
    bool isTheCallThatIsCounted = numberOfCallSitesCountedHere(context, frame) || isHot(heat);
    if (isTheCallThatIsCounted && callGraph.numberOfCallSites(function) == 1 && callGraph.callSiteSet(function) == CallSiteSet::Complete)
        return true;
    return isEnteredThroughCallerAlone(context, function, heat);
}

// A body with one call site moves into its caller and is no copy. So what is called from it has the depth that it had there.
static unsigned depthOfCopies(const FunctionPassContext& context, uint32_t inlineFrame, HeatClass heat)
{
    const Program& program = context.program;
    const Function& caller = context.function;
    unsigned depth = inlineDepthOf(program, caller, inlineFrame);
    Vector<uint32_t, 8> framesFromInnermost;
    for (uint32_t frame = inlineFrame; frame; frame = caller.inlineFrames[frame].parent)
        framesFromInnermost.append(frame);
    for (uint32_t frame : framesFromInnermost | std::views::reverse) {
        FunctionID callee = caller.inlineFrames[frame].callee;
        if (!hasSingleCallSite(context, callee, caller.inlineFrames[frame].parent, heat))
            break;
        depth -= !isWrapperCreatedByEngine(program.at(callee));
    }
    return depth;
}

// An entrypoint tests what it was given and enters the body.
static bool wouldCopySharedBodyIntoEntrypoint(const FunctionPassContext& context, const Value& call, HeatClass heat)
{
    const Function& caller = context.function;
    if (caller.role != FunctionRole::GenericEntrypoint || isHot(heat))
        return false;
    // Wrappers lead to the body. What the body calls has the budget that it has there, unless the body is itself a copy.
    bool entersBody = !inlineDepthOf(context.program, caller, call.inlineFrame);
    if (entersBody && mayBeEnteredElsewhere(context, call.function(), call.inlineFrame))
        return true;
    for (uint32_t frame = call.inlineFrame; frame; frame = caller.inlineFrames[frame].parent) {
        if (mayBeEnteredElsewhere(context, caller.inlineFrames[frame].callee, caller.inlineFrames[frame].parent))
            return true;
    }
    return false;
}

// The tests that were split off the head of a function go where the entrypoint that takes them in is all that enters them.
static bool wouldCopySharedEntryChecksIntoEntrypoint(const FunctionPassContext& context, const Value& call, HeatClass heat)
{
    const Program& program = context.program;
    const Function& callee = program.at(call.function());
    if (context.function.role != FunctionRole::GenericEntrypoint || call.isAlwaysInlined() || callee.role != FunctionRole::EntryChecks)
        return false;
    if (isHot(heat)) {
        Representation result = defaultRepresentationOf(program.at(call.type));
        bool convertsResult = result != Representation::JSValue && result != Representation::Cell;
        if (convertsResult && context.inputs.inlining.of(program, callee.creator.as<CreatorRecord::EntryChecks>().body).codeBytes <= InlineModel::bodyCopiedIntoHotEntrypointBytes)
            return false;
    }
    return mayBeEnteredElsewhere(context, call.function(), call.inlineFrame);
}

// A function that is entered from many places runs often, though none of the places may.
static bool isAmongMostEnteredFunctions(const FunctionPassContext& context, FunctionID function)
{
    return context.inputs.heat.percentileOf(context.program, function) >= InlineModel::percentileOfMostEnteredFunctions;
}

static uint32_t budgetAt(const FunctionPassContext& context, const Value& call, HeatClass heat, unsigned depth)
{
    uint32_t budget = inlineBudgetBytes(context, heat, depth);
    if (isHot(heat) || isCold(heat) || !isAmongMostEnteredFunctions(context, call.function()))
        return budget;
    return std::max(budget, inlineBudgetBytes(context, HeatClass::Hot, depth) / 4 * 3);
}

static bool makesOrFillsNewObject(const Function& callee)
{
    return callee.role == FunctionRole::ConstructBody || callee.role == FunctionRole::AllocatedThisVersion || callee.has(FunctionFlags::AllocatesOwnThis);
}

// The inliner makes a summary of its own for a callee that was derived in this loop, against the effect summaries of the loop.
static bool calleeReturnsFreshAggregate(FunctionPassContext& context, const InliningSummary& summary, FunctionID callee)
{
    const InliningSummaries& inlining = context.inputs.inlining;
    if (inlining.contains(callee))
        return inlining.returnsFreshAggregate(context.program, callee);
    std::atomic_ref answer(summary.returnsFreshAggregate);
    InliningSummary::Answer known = answer.load(std::memory_order_relaxed);
    if (known == InliningSummary::Answer::NotAsked) {
        FunctionAnalyses analyses(AnalysisContext { context.build, context.program, &context.inputs.flow, &context.inputs.effects, callee }, context.program.at(callee));
        known = returnsFreshAggregate(analyses) ? InliningSummary::Answer::Yes : InliningSummary::Answer::No;
        answer.store(known, std::memory_order_relaxed);
    }
    return known == InliningSummary::Answer::Yes;
}

InlineVerdict computeInlineVerdict(FunctionPassContext& context, const InliningSummary& summary, BlockID block, ValueID call)
{
    const Program& program = context.program;
    const Function& caller = context.function;
    const Value& value = caller.at(call);
    RELEASE_ASSERT(value.opcode == Opcode::CallDirect);

    if (program.at(value.function()).role == FunctionRole::CoroutineRamp && willEnterAwaitedCalleeWithoutPromise(context, call))
        return { .decision = InlineDecision::Barrier };

    bool canInlineWholeBody = summary.canInlineWholeBody();
    if (!canInlineWholeBody && !summary.canInlineHead())
        return { .decision = InlineDecision::Barrier };

    uint32_t foldedBytes = computeFoldedBytes(program, caller, value, summary);
    uint32_t removedBytes = saturatingSum<uint32_t>(foldedBytes, estimatedCodeBytes(program, caller, value));
    uint32_t cost = summary.codeBytes > removedBytes ? summary.codeBytes - removedBytes : 0;
    bool introducesHandlers = !summary.blocksWithHandlers.isEmpty() && !context.analyses.cfg().hasHandlers();
    if (introducesHandlers)
        cost = saturatingSum<uint32_t>(cost, InlineModel::handlerIntroductionBytes);

    HeatClass heat = heatClassOf(context, block);
    // However often the caller runs, this part of it is not expected to.
    if (caller.at(block).frequency != BlockFrequency::Normal && !isCold(heat))
        heat = HeatClass::Cold;
    // In a loop within a loop of the caller the call runs more often, for each time that the caller runs, than a loop goes round.
    if (heat == HeatClass::Warm && context.inputs.heat.callsInLoopsAreHot() && context.analyses.blockFrequencies().frequencyOf(block) >= frequencyConstants.iterationsPerLoop)
        heat = HeatClass::Hot;
    unsigned depth = depthOfCopies(context, value.inlineFrame, heat);
    // The budget halves with each level and is small where code runs seldom. A bet that did neither let a body in at every level, and what a caller knew of one object cost it many times its size.
    Bets bets = isCold(heat) || depth >= InlineModel::depthWithoutBets ? Bets::None : Bets::OnSpeed;
    uint32_t budget = wouldCopySharedBodyIntoEntrypoint(context, value, heat) ? 0 : budgetAt(context, value, heat, depth);
    InlineVerdict verdict { .costBytes = cost, .budgetBytes = budget, .introducesHandlers = introducesHandlers };

    auto addBenefit = [&](InlineBenefit benefit, uint64_t bytes) {
        verdict.benefits.add(benefit);
        verdict.benefitBytes = clampTo<uint32_t>(verdict.benefitBytes + bytes);
    };
    auto isWithinBudget = [&] {
        return cost <= uint64_t { verdict.budgetBytes } + verdict.benefitBytes;
    };

    bool isEntryChecks = program.at(value.function()).role == FunctionRole::EntryChecks;
    bool callerIsEntrypoint = caller.role == FunctionRole::GenericEntrypoint || caller.role == FunctionRole::TableEntrypoint;
    bool leavesEntryChecksOutside = wouldCopySharedEntryChecksIntoEntrypoint(context, value, heat);
    bool opensTypedBody = isEntryChecks && (callerIsEntrypoint || passesEveryEntryCheck(program, caller, value) || (!isCold(heat) && knowsMoreThanEntryChecksPassOn(context, call)));
    bool isAlwaysWorthInlining = summary.isWrapper && !leavesEntryChecksOutside && (!isEntryChecks || opensTypedBody);
    if (canInlineWholeBody && (value.isAlwaysInlined() || isAlwaysWorthInlining)) {
        addBenefit(value.isAlwaysInlined() ? InlineBenefit::CallOverhead : InlineBenefit::Wrapper, cost);
        verdict.decision = InlineDecision::Inline;
        return verdict;
    }
    if (caller.role == FunctionRole::EntryChecks || leavesEntryChecksOutside) {
        verdict.decision = InlineDecision::CostExceedsBenefit;
        return verdict;
    }
    if (isEntryChecks) {
        const auto& record = std::get<CreatorRecord::EntryChecks>(program.at(value.function()).creator.cases);
        const InliningSummaries& inlining = context.inputs.inlining;
        cost = saturatingSum<uint32_t>(cost, inlining.of(program, record.body).codeBytes, inlining.of(program, record.fallback).codeBytes);
        verdict.costBytes = cost;
    }

    if (foldedBytes)
        verdict.benefits.add(InlineBenefit::FoldedCode);
    uint64_t callOverheadBytes = InlineModel::callOverheadBytes + uint64_t { InlineModel::argumentOverheadBytes } * value.numberOfOperands();
    addBenefit(InlineBenefit::CallOverhead, callOverheadBytes);
    if (uint64_t bytes = computeKnownCalleeBenefit(context, call, summary, bets))
        addBenefit(InlineBenefit::KnownCalleeArgument, bytes);
    if (uint64_t bytes = computeKnownReceiverBenefit(context, value, summary, bets))
        addBenefit(InlineBenefit::KnownReceiver, bytes);

    bool isTooDeep = false;
    if (canInlineWholeBody) {
        if (iteratesGeneratorOfCaller(context, value, summary) && !context.exceeds(LimitID::MaxOnlyCallSiteBytes, cost, call))
            addBenefit(InlineBenefit::SunkAllocation, cost);
        // A factory is mostly called by value, so heat says little of the `new` inside it. A small constructor goes where its object is wanted, whatever the heat.
        if (cost <= InlineModel::constructorBytes && makesOrFillsNewObject(program.at(value.function())))
            addBenefit(InlineBenefit::SunkAllocation, cost);
        // The limit bounds what each level adds to the caller. A body that costs no more than its call adds nothing.
        isTooDeep = cost > callOverheadBytes && context.exceeds(LimitID::MaxInlineDepth, depth + 1, call);
        // Where code runs seldom, an allocation that goes is worth its own bytes and no more.
        if (!isTooDeep && !isWithinBudget() && (!isCold(heat) || callerIsEntrypoint)) {
            uint64_t bytesOfAllocation = isCold(heat) ? InlineModel::allocationCallBytes : context.limit(LimitID::InlineSunkAllocationBytes);
            unsigned allocations = sinkableAllocationsAfterInlining(context, block, call, [&] {
                return calleeReturnsFreshAggregate(context, summary, value.function());
            });
            if (allocations)
                addBenefit(InlineBenefit::SunkAllocation, allocations * bytesOfAllocation);
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
