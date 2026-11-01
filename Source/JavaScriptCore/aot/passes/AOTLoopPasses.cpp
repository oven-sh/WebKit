/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTDecidedBranches.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEscapeAnalysis.h"
#include "AOTFlowApply.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGenericCopy.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTInductionVariables.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTMemoryLocation.h"
#include "AOTNaturalLoops.h"
#include "AOTObservableOperations.h"
#include "AOTOperatorForms.h"
#include "AOTProgramPasses.h"
#include "AOTRegionCloner.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTSummarizeForInlining.h"
#include "AOTThreshold.h"
#include "AOTTypeTestSite.h"
#include "AOTUseEffectHistory.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CounterCandidate {
    ValueID parameter;
    ValueID comparison;
    uint32_t testedOperandIndex;
    Edge stayingEdge;
    Vector<ValueID, 1> stepsEqualToTested { };
    // `i-- > 0`, `++i < n`: what is compared is no variable, so the front end has passed on nothing of what the comparison says.
    bool comparesNoVariable { false };
};

struct SumWithCounter {
    ValueID counter;
    int64_t addend { 0 };
    uint32_t counterIndex { 0 };
};

struct SumAheadOfTest {
    ValueID sum;
    uint32_t counterIndex;
};

struct LoopClobbers {
    Vector<MemoryLocation> locations;
    LocationMasks writes;
    bool maySuspend { false };
    bool otherCodeMayWriteAnything { false };
};

struct DeadLoop {
    BlockID header;
    BlockID exit;
    BitVector body;
};

} // anonymous namespace

static bool isNumberBeyondInt32(const Type& type)
{
    return type.isNumber() && !type.isInt32();
}

static bool hasNumberParameter(const Program& program, const Function& function)
{
    for (size_t index = 0; index < function.blocks.size(); ++index) {
        if (BlockID(index) == entryBlock)
            continue;
        for (ValueID parameter : function.blocks[index].parameters) {
            if (isNumberBeyondInt32(program.typeOf(function, parameter)))
                return true;
        }
    }
    return false;
}

static bool hasIntegerBeyondInt32(const Program& program, const Function& function)
{
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (isNumberBeyondInt32(program.typeOf(function, id)) && program.typeOf(function, id).isInteger())
                return true;
        }
    }
    return false;
}

// An int32 counter whose step may overflow int32 is tested after every step. A counter below an int32 bound is still an int32 one step later.
static bool stepsBeyondInt32(FunctionPassContext& context, const InductionVariable& variable)
{
    return context.program.typeOf(context.function, variable.parameter).isInt32() && std::ranges::any_of(variable.steppedValues, [&](ValueID stepped) {
        return isNumberBeyondInt32(context.program.typeOf(context.function, stepped));
    });
}

static std::optional<SumWithCounter> findSumWithConstant(FunctionPassContext& context, ValueID id)
{
    const Function& function = context.function;
    const Value& sum = function.at(context.analyses.identityOf(id));
    auto constantAt = [&](uint32_t index) {
        return integerConstantThroughCasts(context.program, function, sum.operand(index));
    };
    switch (sum.opcode) {
    case Opcode::Increment:
        return SumWithCounter { sum.operand(0), 1 };
    case Opcode::Decrement:
        return SumWithCounter { sum.operand(0), -1 };
    case Opcode::Add:
        if (std::optional<int64_t> right = constantAt(1))
            return SumWithCounter { sum.operand(0), *right };
        if (std::optional<int64_t> left = constantAt(0))
            return SumWithCounter { sum.operand(1), *left, 1 };
        return std::nullopt;
    case Opcode::Sub:
        if (std::optional<int64_t> right = constantAt(1); right && *right != std::numeric_limits<int64_t>::min())
            return SumWithCounter { sum.operand(0), -*right };
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

static ValueID sameNumberThroughConversions(FunctionPassContext& context, ValueID id)
{
    for (id = context.analyses.identityOf(id);; id = context.analyses.identityOf(context.function.at(id).operand(0))) {
        const Value& value = context.function.at(id);
        if ((value.opcode != Opcode::ToNumber && value.opcode != Opcode::ToNumeric) || !context.program.typeOf(context.function, value.operand(0)).isNumber())
            return id;
    }
}

// The block parameter that a number is, or was before constants were added to it.
static ValueID findParameterCountedFrom(FunctionPassContext& context, ValueID id)
{
    constexpr unsigned mostSums = 4;
    for (unsigned numberOfSums = 0; numberOfSums <= mostSums; ++numberOfSums) {
        id = sameNumberThroughConversions(context, id);
        if (context.analyses.useLists().locationOf(id).placement == ValuePlacement::Parameter)
            return id;
        std::optional<SumWithCounter> sum = findSumWithConstant(context, id);
        if (!sum)
            return { };
        id = sum->counter;
    }
    return { };
}

// `turns++ < 16`, `i-- > 0`: the step stands in front of the test, so the test says nothing about it. These are the sums of the tested number and a constant that stand there.
static Vector<SumAheadOfTest, 2> findSumsAheadOfTest(FunctionPassContext& context, Edge stayingEdge, ValueID tested)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    Vector<SumAheadOfTest, 2> sums;
    for (ValueID number = tested;;) {
        for (const Use& use : analyses.useLists().usesOf(number)) {
            if (use.kind != Use::Kind::Operand || analyses.identityOf(use.user) != use.user || dominatesAfterSplit(analyses, stayingEdge, use.block))
                continue;
            std::optional<SumWithCounter> sum = findSumWithConstant(context, use.user);
            if (sum && sum->counterIndex == use.operandIndex && !std::ranges::contains(sums, use.user, &SumAheadOfTest::sum))
                sums.append({ use.user, sum->counterIndex });
        }
        const Value& value = function.at(number);
        bool isConversion = value.opcode == Opcode::ToNumber || value.opcode == Opcode::ToNumeric;
        if (!isCastWithoutTest(value.opcode) && (!isConversion || !context.program.typeOf(function, value.operand(0)).isNumber()))
            return sums;
        number = value.operand(0);
    }
}

// A staying edge into a shared block is split before it is used.
static bool dominatesEveryStep(FunctionAnalyses& analyses, Edge edge, const InductionVariable& variable)
{
    const UseLists& useLists = analyses.useLists();
    return std::ranges::all_of(variable.steppedValues, [&](ValueID stepped) {
        return dominatesAfterSplit(analyses, edge, useLists.blockOf(stepped));
    });
}

// A step in front of the test is repeated behind it. That bounds the counter where only turns that have passed the test hand it on.
static bool dominatesEveryStepOrItsWayOn(FunctionPassContext& context, const LoopInduction& induction, Edge edge, const InductionVariable& variable, ValueID tested)
{
    FunctionAnalyses& analyses = context.analyses;
    if (dominatesEveryStep(analyses, edge, variable))
        return true;
    Vector<SumAheadOfTest, 2> sums = findSumsAheadOfTest(context, edge, tested);
    bool canRepeatSteps = std::ranges::all_of(variable.steppedValues, [&](ValueID stepped) {
        return std::ranges::contains(sums, stepped, &SumAheadOfTest::sum) || dominatesAfterSplit(analyses, edge, analyses.useLists().blockOf(stepped));
    });
    const NaturalLoop& loop = analyses.naturalLoops().loop(induction.loopIndex);
    return canRepeatSteps && std::ranges::all_of(analyses.cfg().incomingEdges(induction.header), [&](Edge incoming) {
        return !analyses.naturalLoops().belongsTo(incoming.from, loop) || dominatesAfterSplit(analyses, edge, incoming.from);
    });
}

static bool mayBecomeIntegerCounter(FunctionPassContext& context, const InductionVariable& variable, BitVector& parametersOverStepLimit)
{
    if (!variable.start || (!isNumberBeyondInt32(context.program.typeOf(context.function, variable.parameter)) && !stepsBeyondInt32(context, variable)))
        return false;
    // What another loop hands over, in a join in front of this one or as it has counted it up, is an integer if that loop's counter becomes one.
    const Type& start = context.program.typeOf(context.function, variable.start);
    if (!start.isInteger()) {
        ValueLocation location = context.analyses.useLists().locationOf(context.analyses.identityOf(variable.start));
        if (!start.isNumberWithoutFraction() || (location.placement == ValuePlacement::Parameter && location.block == entryBlock))
            return false;
    }
    if (variable.step.kind != InductionStepKind::Constant || !variable.step.constant || parametersOverStepLimit.get(variable.parameter.index()))
        return false;

    int64_t step = variable.step.constant;
    uint64_t magnitude = step < 0 ? 0 - static_cast<uint64_t>(step) : static_cast<uint64_t>(step);
    if (!context.exceeds(LimitID::MaxCounterStep, magnitude, variable.parameter))
        return true;
    parametersOverStepLimit.set(variable.parameter.index());
    return false;
}

static std::optional<CounterCandidate> findCandidateTestedBeforeStep(FunctionPassContext& context, const LoopInduction& induction, const InductionVariable& variable)
{
    FunctionAnalyses& analyses = context.analyses;
    for (const InductionTest& test : variable.tests) {
        if (test.tested != TestedValue::BeforeStep || !isBoundingRelation(test.relation, variable.direction))
            continue;
        if (!context.program.typeOf(context.function, test.bound).isInteger())
            continue;
        const Value& comparison = context.function.at(test.comparison);
        ValueID bound = analyses.identityOf(test.bound);
        bool isLeftBound = analyses.identityOf(comparison.operand(0)) == bound;
        bool isRightBound = analyses.identityOf(comparison.operand(1)) == bound;
        if (isLeftBound == isRightBound)
            continue;
        uint32_t testedOperandIndex = isLeftBound ? 1 : 0;
        if (!dominatesEveryStepOrItsWayOn(context, induction, test.stayingEdge, variable, comparison.operand(testedOperandIndex)))
            continue;
        if (targetOf(context.function, test.stayingEdge).proofs.contains(testedOperandIndex))
            return std::nullopt;
        return CounterCandidate { variable.parameter, test.comparison, testedOperandIndex, test.stayingEdge, { }, !dominatesEveryStep(analyses, test.stayingEdge, variable) };
    }
    return std::nullopt;
}

static std::optional<CounterCandidate> findCandidateTestedAheadOfStep(FunctionPassContext& context, const LoopInduction& induction, const InductionVariable& variable)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    auto isParameterPlusStep = [&](ValueID id) {
        const Value& sum = function.at(analyses.identityOf(id));
        if (sum.opcode != Opcode::Add)
            return false;
        for (uint32_t side = 0; side < 2; ++side) {
            if (analyses.identityOf(sum.operand(side)) != variable.parameter)
                continue;
            if (integerConstantThroughCasts(context.program, function, sum.operand(1 - side)) == variable.step.constant)
                return true;
        }
        return false;
    };
    if (!std::ranges::all_of(variable.steppedValues, isParameterPlusStep))
        return std::nullopt;

    const NaturalLoops& loops = analyses.naturalLoops();
    const NaturalLoop& loop = loops.loop(induction.loopIndex);
    for (unsigned index = 0; index < loop.size(); ++index) {
        BlockID block = loop.at(index);
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || !loops.belongsTo(terminator.taken().block, loop) || loops.belongsTo(terminator.notTaken().block, loop))
            continue;
        const Value& comparison = function.at(terminator.conditionOperand());
        if (!relationalOperators.contains(comparison.opcode))
            continue;
        for (uint32_t side = 0; side < 2; ++side) {
            ValueID tested = comparison.operand(side);
            if (variable.steppedValues.contains(analyses.identityOf(tested)) || !isParameterPlusStep(tested))
                continue;
            Opcode relation = side ? mirroredComparison(comparison.opcode) : comparison.opcode;
            if (!isBoundingRelation(relation, variable.direction) || !context.program.typeOf(function, comparison.operand(1 - side)).isInteger())
                continue;
            Edge taken { block, 0 };
            if (!dominatesEveryStep(analyses, taken, variable) || terminator.taken().proofs.contains(side))
                continue;
            return CounterCandidate { variable.parameter, terminator.conditionOperand(), side, taken, variable.steppedValues };
        }
    }
    return std::nullopt;
}

// The first turn of a loop may stand in front of it. Below an int32 bound its counter is still an int32 one step later, as in the loop.
// One bound may be tested by every loop of a function, so its uses are walked once, not once for each test.
struct StepsBeyondInt32 {
    HashMap<ValueID, Vector<BlockID, 1>> blocksOfSteps;
    uint64_t usesRead { 0 };
};
static bool stepsBeyondInt32Behind(FunctionPassContext& context, Edge edge, ValueID tested, StepsBeyondInt32& steps)
{
    FunctionAnalyses& analyses = context.analyses;
    auto entry = steps.blocksOfSteps.add(tested, Vector<BlockID, 1> { });
    if (entry.isNewEntry) {
        for (const Use& use : analyses.useLists().usesOf(tested)) {
            ++steps.usesRead;
            if (use.kind != Use::Kind::Operand || !isNumberBeyondInt32(context.program.typeOf(context.function, use.user)))
                continue;
            std::optional<SumWithCounter> sum = findSumWithConstant(context, use.user);
            if (sum && sum->counterIndex == use.operandIndex)
                entry.iterator->value.append(use.block);
        }
    }
    steps.usesRead += entry.iterator->value.size();
    return std::ranges::any_of(entry.iterator->value, [&](BlockID block) {
        return dominatesAfterSplit(analyses, edge, block);
    });
}

static Vector<CounterCandidate, 4> findCounterCandidates(FunctionPassContext& context, BitVector& parametersOverStepLimit)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    auto typeOf = [&](ValueID id) -> const Type& {
        return context.program.typeOf(function, id);
    };
    Vector<CounterCandidate, 4> candidates;
    for (const LoopInduction& loop : analyses.inductionVariables().loops()) {
        for (const InductionVariable& variable : loop.variables) {
            if (!mayBecomeIntegerCounter(context, variable, parametersOverStepLimit))
                continue;
            if (std::optional<CounterCandidate> candidate = findCandidateTestedBeforeStep(context, loop, variable))
                candidates.append(*candidate);
            else if (std::optional<CounterCandidate> candidate = findCandidateTestedAheadOfStep(context, loop, variable))
                candidates.append(*candidate);
        }
    }

    // A loop and the copy of its failed tests, which has a loop of its own, hand each other the counter: the way back gives the typed loop what the copy has counted up, and a failed test gives the copy what the typed loop has.
    // Where the copy comes back in the middle of the body the step adds to a join, and the loop has no induction variable though its cycle has one entry.
    // `while (b)`: that b is neither zero nor NaN says nothing of a number without a fraction, so nobody kept it, and it is all that a remainder by b needs to be an integer.
    const UseLists& useLists = analyses.useLists();
    const BlockCycles& cycles = analyses.blockCycles();
    // What goes up or down by a constant has been judged as a counter.
    BitVector isJudgedAsCounter;
    for (const LoopInduction& loop : analyses.inductionVariables().loops()) {
        for (const InductionVariable& variable : loop.variables) {
            if (variable.step.kind == InductionStepKind::Constant)
                isJudgedAsCounter.set(variable.parameter.index());
        }
    }
    StepsBeyondInt32 steps;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            continue;
        std::optional<unsigned> cycle = cycles.innermostCycleOf(block);
        if (!cycle)
            continue;
        const Value& comparison = function.at(terminator.conditionOperand());
        bool testsForTruth = comparison.opcode == Opcode::ToBoolean;
        if (!testsForTruth && !relationalOperators.contains(comparison.opcode))
            continue;
        for (uint32_t operandIndex = 0; operandIndex < comparison.numberOfOperands(); ++operandIndex) {
            const Type& tested = typeOf(comparison.operand(operandIndex));
            if (!tested.isNumberWithoutFraction() || (!testsForTruth && !typeOf(comparison.operand(1 - operandIndex)).isInteger()))
                continue;
            if (tested.isInt32() && (testsForTruth || terminator.taken().proofs.contains(operandIndex) || !stepsBeyondInt32Behind(context, Edge { block, 0 }, comparison.operand(operandIndex), steps)))
                continue;
            // `while (++i < n)`: what a turn has counted up is tested as often as what it is about to.
            ValueID number = sameNumberThroughConversions(context, comparison.operand(operandIndex));
            ValueID parameter = findParameterCountedFrom(context, number);
            if (!parameter || useLists.blockOf(parameter) == entryBlock || terminator.taken().proofs.contains(operandIndex))
                continue;
            if (!testsForTruth && number == parameter && cycles.forest()[*cycle].isReducible() && isJudgedAsCounter.get(parameter.index()))
                continue;
            if (std::ranges::contains(candidates, Edge { block, 0 }, &CounterCandidate::stayingEdge))
                continue;
            bool comparesNoVariable = number != parameter || !findSumsAheadOfTest(context, Edge { block, 0 }, comparison.operand(operandIndex)).isEmpty();
            candidates.append(CounterCandidate { number, terminator.conditionOperand(), operandIndex, Edge { block, 0 }, { }, comparesNoVariable });
        }
    }

    context.build.statistics().count(StatisticID::UsesReadForStepsBehindTests, steps.usesRead);

    // What has failed the test for an integer is none. Where the loop by itself keeps its counter an integer nothing is left of it on that edge, so what the copy behind the edge counts up comes back to nobody.
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || !cycles.innermostCycleOf(block))
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        if (test.opcode != Opcode::TypeTest || !context.types.at(test.testedType()).isInteger() || terminator.notTaken().proofs.contains(OperandIndex::TypeTest::value))
            continue;
        const Type& tested = typeOf(test.valueOperand());
        ValueID parameter = analyses.identityOf(test.valueOperand());
        if (tested.isNumberWithoutFraction() && !tested.isInteger() && useLists.locationOf(parameter).placement == ValuePlacement::Parameter)
            candidates.append(CounterCandidate { parameter, terminator.conditionOperand(), OperandIndex::TypeTest::value, Edge { block, 1 } });
    }
    return candidates;
}

static Vector<ValueID, 4> passCountersOnEdges(FunctionPassContext& context, IRBuilder& builder, std::span<const CounterCandidate> candidates)
{
    Function& function = context.function;
    Vector<Edge, 2> sharedEdges;
    for (const CounterCandidate& candidate : candidates) {
        BlockID target = targetOf(function, candidate.stayingEdge).block;
        bool isShared = !context.analyses.hasSingleIncomingEdge(target);
        // The handler of a block sees nothing that the block defines, so a sum that is repeated in front of a `try` gets a block of its own.
        if (!isShared && function.at(target).handler)
            isShared = !findSumsAheadOfTest(context, candidate.stayingEdge, function.at(candidate.comparison).operand(candidate.testedOperandIndex)).isEmpty();
        if (isShared && !sharedEdges.contains(candidate.stayingEdge))
            sharedEdges.append(candidate.stayingEdge);
    }
    for (Edge edge : sharedEdges)
        builder.splitEdge(edge);

    struct RepeatedSum {
        SumAheadOfTest original;
        Vector<Use, 4> dominatedUses;
    };
    const UseLists& useLists = context.analyses.useLists();
    Vector<Vector<Use, 8>, 4> dominatedUses(candidates.size());
    Vector<Vector<RepeatedSum, 1>, 4> repeatedSums(candidates.size());
    for (size_t index = 0; index < candidates.size(); ++index) {
        const CounterCandidate& candidate = candidates[index];
        auto collectUsesOf = [&](ValueID value, auto& uses) {
            for (const Use& use : useLists.usesOf(value)) {
                if (context.analyses.dominates(candidate.stayingEdge, use.block))
                    uses.append(use);
            }
        };
        ValueID tested = function.at(candidate.comparison).operand(candidate.testedOperandIndex);
        collectUsesOf(tested, dominatedUses[index]);
        for (ValueID stepped : candidate.stepsEqualToTested)
            collectUsesOf(stepped, dominatedUses[index]);
        if (candidate.stepsEqualToTested.isEmpty() && tested != candidate.parameter && function.at(tested).type == function.at(candidate.parameter).type)
            collectUsesOf(candidate.parameter, dominatedUses[index]);
        for (SumAheadOfTest sum : findSumsAheadOfTest(context, candidate.stayingEdge, tested)) {
            RepeatedSum repeated { sum, { } };
            // A cast that says no more of the sum may stand in front of the test too.
            Vector<ValueID, 2> names { sum.sum };
            for (size_t nameIndex = 0; nameIndex < names.size(); ++nameIndex) {
                collectUsesOf(names[nameIndex], repeated.dominatedUses);
                for (const Use& use : useLists.usesOf(names[nameIndex])) {
                    if (use.kind == Use::Kind::Operand && !use.operandIndex && isCastWithoutTest(function.at(use.user).opcode) && function.at(use.user).type == function.at(sum.sum).type && !context.analyses.dominates(candidate.stayingEdge, use.block))
                        names.append(use.user);
                }
            }
            if (!repeated.dominatedUses.isEmpty())
                repeatedSums[index].append(WTF::move(repeated));
        }
    }

    Vector<ValueID, 4> narrowedCounters;
    for (size_t index = 0; index < candidates.size(); ++index) {
        builder.setOrigin(function.at(candidates[index].comparison).origin());
        narrowedCounters.append(builder.addEdgeResult(candidates[index].stayingEdge, candidates[index].testedOperandIndex));
        for (const Use& use : dominatedUses[index])
            replaceUse(function, use, narrowedCounters.last());
        for (const RepeatedSum& repeated : repeatedSums[index]) {
            const Value& original = function.at(repeated.original.sum);
            Value sum = original.numberOfOperands() == 1 ? IRBuilder::make(original.opcode, { narrowedCounters.last() }) : IRBuilder::make(original.opcode, { original.operand(0), original.operand(1) });
            sum.setOperand(repeated.original.counterIndex, narrowedCounters.last());
            builder.setOrigin(original.origin());
            ValueID behindTest = builder.insertBefore(ProgramPoint::atHeadOf(targetOf(function, candidates[index].stayingEdge).block), WTF::move(sum));
            for (const Use& use : repeated.dominatedUses)
                replaceUse(function, use, behindTest);
        }
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return narrowedCounters;
}

static bool isHandedToBoundedCounter(FunctionPassContext& context, ValueID parameter)
{
    constexpr size_t mostJoins = 8;
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    Vector<ValueID, mostJoins> joins { parameter };
    for (size_t index = 0; index < joins.size(); ++index) {
        if (const InductionVariable* variable = analyses.inductionVariables().find(joins[index]))
            return variable->step.kind == InductionStepKind::Constant && variable->boundingTest();
        for (const Use& use : analyses.useLists().usesOf(joins[index])) {
            if (use.kind != Use::Kind::EdgeArgument || joins.size() == mostJoins)
                continue;
            const Block& target = function.at(targetOf(function, use.edge()).block);
            if (target.frequency.predicted() == BlockFrequency::Normal && use.operandIndex < target.parameters.size() && !joins.contains(target.parameters[use.operandIndex]))
                joins.append(target.parameters[use.operandIndex]);
        }
    }
    return false;
}

// A way back from the copy of failed tests is planned while the typed loop's counter is still a number: `i = values.length - 1` before anybody has tested `values`. It asks only whether what the copy has counted is without a fraction,
// and with that in its join the typed loop's counter never becomes an integer. So it asks for an integer as far from zero as an int32 or as what else enters the loop. Whatever fails goes on in the copy, as a fraction did.
static Vector<ValueID, 2> testForIntegersOnWaysBack(FunctionPassContext& context)
{
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    Vector<ValueID, 2> narrowed;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || function.at(block).frequency.predicted() == BlockFrequency::Normal || !analyses.isInCycle(block))
            continue;
        ValueID id = terminator.conditionOperand();
        const Value& test = function.at(id);
        if (test.opcode != Opcode::TypeTest || analyses.useLists().useCount(id) != 1)
            continue;
        const Type& tested = context.types.at(test.testedType());
        size_t proofIndex = terminator.taken().proofs.find(OperandIndex::TypeTest::value);
        if (!tested.isNumberWithoutFraction() || tested.isInteger() || proofIndex == notFound || !terminator.notTaken().proofs.isEmpty())
            continue;
        ValueID proof = function.at(terminator.taken().block).parameters[firstEdgeResultParameter(function, block, 0) + proofIndex];
        std::optional<Use> handedOn = analyses.useLists().singleUse(proof);
        if (!handedOn || handedOn->kind != Use::Kind::EdgeArgument)
            continue;
        BlockID join = targetOf(function, handedOn->edge()).block;
        if (handedOn->operandIndex >= function.at(join).parameters.size())
            continue;
        Range range = Range::int32();
        bool othersAreIntegers = std::ranges::all_of(analyses.cfg().incomingEdges(join), [&](Edge edge) {
            if (edge == handedOn->edge())
                return true;
            IncomingValue incoming = edge.isHandlerEdge() ? IncomingValue { IncomingValue::Kind::EdgeResult } : incomingValueOf(function, edge, handedOn->operandIndex);
            if (incoming.kind != IncomingValue::Kind::Argument || !context.program.typeOf(function, incoming.argument).isInteger())
                return false;
            range = range.unionWith(context.program.typeOf(function, incoming.argument).integers);
            return true;
        });
        if (!othersAreIntegers || !range.isSubsetOf(Range::exact()) || !isHandedToBoundedCounter(context, function.at(join).parameters[handedOn->operandIndex]))
            continue;
        function.edit(id, ValueEdit::Definition).setTestedType(context.types.intern(Type::integer(range)));
        narrowed.append(proof);
    }
    if (!narrowed.isEmpty())
        analyses.didChangeValues();
    return narrowed;
}

namespace {

// By how much a value differs from what the loop had at the head of the turn.
struct ChangeInTurn {
    int64_t least { 0 };
    int64_t most { 0 };
};

struct TallyFinder {
    AOT_CLASS_THRESHOLD(unsigned, mostValuesBetweenTurns, 256);

    FunctionPassContext& context;
    const LoopInduction& loop;
    ValueID parameter;
    std::optional<unsigned> cycle { context.analyses.blockCycles().innermostCycleOf(loop.header) };
    unsigned numberOfValues { 0 };

    std::optional<ChangeInTurn> plus(ValueID base, std::optional<int64_t> constant, bool isSubtracted)
    {
        std::optional<ChangeInTurn> change = constant && Range::int32().contains(*constant) ? changeOf(base) : std::nullopt;
        if (!change)
            return std::nullopt;
        int64_t step = isSubtracted ? -*constant : *constant;
        return ChangeInTurn { change->least + step, change->most + step };
    }

    std::optional<ChangeInTurn> changeOfJoin(ValueLocation location)
    {
        // A join of an inner loop takes what that loop has made of the value in any number of turns.
        FunctionAnalyses& analyses = context.analyses;
        if (location.block == loop.header || analyses.blockCycles().innermostCycleOf(location.block) != cycle || context.function.at(location.block).isHandlerBlock())
            return std::nullopt;
        std::optional<ChangeInTurn> joined;
        for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
            IncomingValue incoming = edge.isHandlerEdge() ? IncomingValue { IncomingValue::Kind::EdgeResult } : incomingValueOf(context.function, edge, location.index);
            std::optional<ChangeInTurn> change = incoming.kind == IncomingValue::Kind::Argument ? changeOf(incoming.argument) : std::nullopt;
            if (!change)
                return std::nullopt;
            joined = joined ? ChangeInTurn { std::min(joined->least, change->least), std::max(joined->most, change->most) } : *change;
        }
        return joined;
    }

    std::optional<ChangeInTurn> changeOf(ValueID id)
    {
        if (id == parameter)
            return ChangeInTurn { };
        if (++numberOfValues > mostValuesBetweenTurns)
            return std::nullopt;
        const Function& function = context.function;
        const Value& value = function.at(id);
        auto constant = [&](uint32_t index) { return integerConstantThroughCasts(context.program, function, value.operand(index)); };
        if (value.opcode == Opcode::Parameter)
            return changeOfJoin(context.analyses.useLists().locationOf(id));
        if (value.opcode == Opcode::Increment || value.opcode == Opcode::Decrement)
            return plus(value.operand(0), 1, value.opcode == Opcode::Decrement);
        if (value.opcode == Opcode::Add)
            return constant(1) ? plus(value.operand(0), constant(1), false) : plus(value.operand(1), constant(0), false);
        if (value.opcode == Opcode::Sub)
            return plus(value.operand(0), constant(1), true);
        // `count++` of a variable without a declared type converts it first.
        bool isConversionOfNumber = (value.opcode == Opcode::ToNumber || value.opcode == Opcode::ToNumeric) && context.program.typeOf(function, value.operand(0)).isNumber();
        return isCastWithoutTest(value.opcode) || isConversionOfNumber ? changeOf(value.operand(0)) : std::nullopt;
    }
};

struct Tally {
    uint32_t parameterIndex;
    Range range;
    Vector<Edge, 2> edgesOfNextTurns;
};

} // anonymous namespace

// An integer counter takes each value of its range at most once. Not 2 ** 53: one more is no double, so a number that has come there stays there.
static std::optional<int64_t> mostTurnsBehindFirst(FunctionPassContext& context, const LoopInduction& loop)
{
    constexpr Range changedByEveryStep { -maxExactInteger + 1, maxExactInteger - 1 };
    std::optional<int64_t> fewest;
    for (const InductionVariable& variable : loop.variables) {
        const Type& type = context.program.typeOf(context.function, variable.parameter);
        if (!type.isInteger() || !type.integers.isSubsetOf(changedByEveryStep) || variable.step.kind != InductionStepKind::Constant || !variable.step.constant)
            continue;
        int64_t turns = (type.integers.maximum - type.integers.minimum) / std::abs(variable.step.constant);
        fewest = fewest ? std::min(*fewest, turns) : turns;
    }
    return fewest;
}

// `if (..) count++` in a loop over a list: nothing tests the tally, and it cannot outrun the counter.
static Vector<Tally, 2> findTallies(FunctionPassContext& context)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    Vector<Tally, 2> tallies;
    for (const LoopInduction& loop : analyses.inductionVariables().loops()) {
        std::optional<int64_t> turns;
        const NaturalLoop& naturalLoop = analyses.naturalLoops().loop(loop.loopIndex);
        const CompactList<ValueID>& parameters = function.at(loop.header).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index) {
            const Type& type = context.program.typeOf(function, parameters[index]);
            if (!type.isNumberWithoutFraction() || type.isInteger())
                continue;
            if (!turns && !(turns = mostTurnsBehindFirst(context, loop)))
                break;
            Range starts = Range::empty();
            ChangeInTurn change;
            Vector<Edge, 2> edgesOfNextTurns;
            bool isTally = std::ranges::all_of(analyses.cfg().incomingEdges(loop.header), [&](Edge edge) {
                IncomingValue incoming = edge.isHandlerEdge() ? IncomingValue { IncomingValue::Kind::EdgeResult } : incomingValueOf(function, edge, index);
                if (incoming.kind != IncomingValue::Kind::Argument)
                    return false;
                if (!analyses.naturalLoops().belongsTo(edge.from, naturalLoop)) {
                    const Type& start = context.program.typeOf(function, incoming.argument);
                    starts = starts.unionWith(start.integers);
                    return start.isInteger();
                }
                // Typing has left the join wider than the bound that its edges were given. Where the block may also leave the loop, what it holds need not reach a next turn.
                if (context.program.typeOf(function, incoming.argument).isInteger() || function.at(edge.from).terminator.kind != TerminatorKind::Jump)
                    return false;
                std::optional<ChangeInTurn> onEdge = TallyFinder { context, loop, parameters[index] }.changeOf(incoming.argument);
                if (!onEdge)
                    return false;
                edgesOfNextTurns.append(edge);
                change = { std::min(change.least, onEdge->least), std::max(change.most, onEdge->most) };
                return true;
            });
            if (!isTally || starts.isEmpty() || !starts.isSubsetOf(Range::int32()) || *turns > UINT32_MAX)
                continue;
            CheckedInt64 least = CheckedInt64(change.least) * *turns + starts.minimum;
            CheckedInt64 most = CheckedInt64(change.most) * *turns + starts.maximum;
            if (least.hasOverflowed() || most.hasOverflowed())
                continue;
            Range range { least.value(), most.value() };
            if (range.isSubsetOf(Range::exact()))
                tallies.append({ index, range, WTF::move(edgesOfNextTurns) });
        }
    }
    return tallies;
}

// What a turn hands to the next lies within the bounds. What it has added to may stand one step outside, where the loop is left in the same turn.
static Vector<ValueID, 4> boundTallies(FunctionPassContext& context, IRBuilder& builder, std::span<const Tally> tallies)
{
    Function& function = context.function;
    Vector<ValueID, 4> bounded;
    for (const Tally& tally : tallies) {
        TypeID type = context.types.intern(Type::integer(tally.range));
        for (Edge edge : tally.edgesOfNextTurns) {
            ValueID handedOn = targetOf(function, edge).arguments[tally.parameterIndex];
            ProgramPoint end = pointBeforeTerminatorOf(function, edge.from);
            builder.setOrigin(function.at(edge.from).terminator.origin());
            if (context.build.validates())
                builder.insertBefore(end, makeValidateType(handedOn, type));
            ValueID turnEndsHere = builder.insertBefore(end, IRBuilder::make(Opcode::Specified));
            bounded.append(builder.insertBefore(end, makeAssumeType(handedOn, turnEndsHere, Proven::behindFact(type))));
            replaceUse(function, Use { .kind = Use::Kind::EdgeArgument, .block = edge.from, .operandIndex = tally.parameterIndex, .targetIndex = edge.targetIndex }, bounded.last());
        }
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return bounded;
}

// On an integer kept as a double, such an operator converts its operand in every iteration, and calls if that fails. A number that is only incremented is cheapest as a double.
static bool hasUseCheaperOnIntegers(FunctionPassContext& context, const NaturalLoop& loop, ValueID parameter, const BitVector& parametersExpectedToBeInt32)
{
    const Function& function = context.function;
    for (unsigned index = 0; index < loop.size(); ++index) {
        for (ValueID id : function.at(loop.at(index)).values) {
            const Value& value = function.at(id);
            if (!hasOperatorForm(value.opcode) || !isLegalOperatorForm(value.opcode, OperatorForm::Integers) || !mayCallRuntime(value.opcode, OperatorForm::Numbers))
                continue;
            bool usesParameter = false;
            bool othersAreIntegers = true;
            value.forEachOperand([&](ValueID operand, uint32_t) {
                if (context.analyses.identityOf(operand) == parameter)
                    usesParameter = true;
                else
                    othersAreIntegers &= context.program.typeOf(function, operand).isInteger() || parametersExpectedToBeInt32.get(context.analyses.identityOf(operand).index());
            });
            if (usesParameter && othersAreIntegers)
                return true;
        }
    }
    return false;
}

struct HandedOnArgument {
    Edge edge;
    uint32_t parameterIndex;
};

// `n % d` where a loop carries n: what a program takes the remainder of is nearly always an int32, and the remainder of two numbers costs several times that of two integers. Such a number is expected to be an int32, and so is the number of a loop around it that it comes from or goes back to. The expectation is tested wherever something that need not be an int32 is handed to the loop: `n /= d`.
static BitVector findDividendsCarriedByLoops(FunctionPassContext& context, Vector<HandedOnArgument, 4>& arguments)
{
    struct CarriedNumber {
        BlockID header;
        uint32_t parameterIndex;
    };
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    BitVector dividends;
    if (!analyses.opcodesOfValues().contains(Opcode::Mod))
        return dividends;
    // A cold function of a very large program gets no loop of its own for what is expected: each use would test for itself.
    const Heat& heat = context.inputs.heat;
    if (heat.isOfVeryLargeProgram() && isCold(heat.of(context.program, context.id)).predicted())
        return dividends;

    UncheckedKeyHashMap<uint32_t, CarriedNumber, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> carriedNumbers;
    for (unsigned index = 0; index < analyses.naturalLoops().numLoops(); ++index) {
        BlockID header = analyses.naturalLoops().loop(index).header();
        if (!isExpectedToRun(context, analyses.naturalLoops().loop(index)).predicted())
            continue;
        const CompactList<ValueID>& parameters = function.at(header).parameters;
        for (uint32_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
            const Type& type = context.program.typeOf(function, parameters[parameterIndex]);
            if (type.isNumber() && !type.isInteger() && !function.at(parameters[parameterIndex]).expected().predicted())
                carriedNumbers.add(parameters[parameterIndex].index(), CarriedNumber { header, parameterIndex });
        }
    }
    if (carriedNumbers.isEmpty())
        return dividends;

    Vector<ValueID, 4> worklist;
    auto add = [&](ValueID value) {
        ValueID identity = analyses.identityOf(value);
        if (carriedNumbers.contains(identity.index()) && !dividends.set(identity.index()))
            worklist.append(identity);
    };
    for (size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (ValueID id : function.blocks[blockIndex].values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::Mod || !context.program.typeOf(function, value.rightOperand()).isNumberWithoutFraction())
                continue;
            auto carried = carriedNumbers.find(analyses.identityOf(value.leftOperand()).index());
            if (carried == carriedNumbers.end())
                continue;
            const NaturalLoop& loop = *analyses.naturalLoops().headerOf(carried->value.header);
            if (analyses.naturalLoops().belongsTo(BlockID(blockIndex), loop))
                add(value.leftOperand());
        }
    }
    while (!worklist.isEmpty()) {
        auto [header, parameterIndex] = carriedNumbers.get(worklist.takeLast().index());
        for (Edge edge : analyses.cfg().incomingEdges(header)) {
            IncomingValue incoming = edge.isHandlerEdge() ? IncomingValue { IncomingValue::Kind::EdgeResult } : incomingValueOf(function, edge, parameterIndex);
            if (incoming.kind != IncomingValue::Kind::Argument)
                return { };
            // A turn that hands on a number of any kind would pay for a test and two conversions. An integer is tested by its range alone, and a quotient without remainder becomes a division of integers.
            const Type& handedOn = context.program.typeOf(function, incoming.argument);
            if (analyses.naturalLoops().belongsTo(edge.from, *analyses.naturalLoops().headerOf(header)) && !handedOn.isNumberWithoutFraction() && !carriedNumbers.contains(analyses.identityOf(incoming.argument).index())) {
                ValueID definition = sameObjectThroughCasts(function, incoming.argument).valueInFrontOfFacts();
                if (function.at(definition).opcode != Opcode::Div || !followsTestForNoRemainder(context.program, function, analyses.cfg(), analyses.useLists().blockOf(definition), function.at(definition)))
                    return { };
            }
            add(incoming.argument);
            if (!handedOn.isInt32())
                arguments.append({ edge, parameterIndex });
        }
    }
    return dividends;
}

// An integer that a loop carries, that starts as an int32, changes by a constant in every iteration and is bounded by no test is expected to stay an int32. The expectation is tested after the step.
static bool expectCarriedNumbersToStayInt32(FunctionPassContext& context, IRBuilder& builder)
{
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    Vector<HandedOnArgument, 4> arguments;
    BitVector dividends = findDividendsCarriedByLoops(context, arguments);
    if (dividends.isEmpty())
        arguments.clear();
    for (const LoopInduction& loop : analyses.inductionVariables().loops()) {
        const NaturalLoop& naturalLoop = analyses.naturalLoops().loop(loop.loopIndex);
        std::optional<bool> isLoopExpectedToRun;
        for (const InductionVariable& variable : loop.variables) {
            const Type& type = context.program.typeOf(function, variable.parameter);
            if (!type.isNumberWithoutFraction() || type.isInteger() || function.at(variable.parameter).expected().predicted() || dividends.get(variable.parameter.index()))
                continue;
            if (!isLoopExpectedToRun)
                isLoopExpectedToRun = isExpectedToRun(context, naturalLoop).predicted();
            if (!*isLoopExpectedToRun)
                break;
            if (!variable.start || !context.program.typeOf(function, variable.start).isInt32() || variable.step.kind != InductionStepKind::Constant || !Range::int32().contains(variable.step.constant))
                continue;
            // If a test bounds it by a number, that number is tested for being an integer before the loop.
            if (std::ranges::any_of(variable.tests, [&](const InductionTest& test) { return isBoundingRelation(test.relation, variable.direction); }))
                continue;
            if (!hasUseCheaperOnIntegers(context, naturalLoop, variable.parameter, dividends))
                continue;
            for (Edge edge : analyses.cfg().incomingEdges(loop.header)) {
                if (analyses.naturalLoops().belongsTo(edge.from, naturalLoop))
                    arguments.append({ edge, variable.parameterIndex });
            }
        }
    }
    for (auto [edge, parameterIndex] : arguments) {
        ValueID handedOn = targetOf(function, edge).arguments[parameterIndex];
        TypeID type = function.at(handedOn).type;
        Value cast = IRBuilder::make(Opcode::Upcast, { handedOn });
        cast.setCastType(type);
        cast.setExpected(ExpectedType(context.types.intersectionOf(type, TypeTable::int32)));
        builder.setOrigin(function.at(handedOn).origin());
        ValueID expected = builder.insertBefore(pointBeforeTerminatorOf(function, edge.from), WTF::move(cast));
        replaceUse(function, Use { .kind = Use::Kind::EdgeArgument, .block = edge.from, .operandIndex = parameterIndex, .targetIndex = edge.targetIndex }, expected);
    }
    if (arguments.isEmpty())
        return false;
    builder.applyInsertions();
    analyses.didChangeValues();
    return true;
}

namespace {

struct TestOfSum {
    ValueID comparison;
    uint32_t sideOfSum;
    SumWithCounter sum;
};

} // anonymous namespace

static bool testsSumInCycle(FunctionPassContext& context)
{
    const Function& function = context.function;
    constexpr OpcodeSet sums { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    return std::ranges::any_of(context.analyses.cfg().reversePostOrder(), [&](BlockID block) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || !relationalOperators.contains(function.at(terminator.conditionOperand()).opcode) || !context.analyses.isInCycle(block))
            return false;
        const Value& comparison = function.at(terminator.conditionOperand());
        return sums.contains(function.at(context.analyses.identityOf(comparison.operand(0))).opcode) || sums.contains(function.at(context.analyses.identityOf(comparison.operand(1))).opcode);
    });
}

// `k + 1 < length` says the same about the counter as `k < length - 1`, and only a test of the counter itself bounds it for later code.
// An integral number plus a small integer is exact whenever it is near an integer far from the limit of exact integers; elsewhere its sign decides both comparisons.
static bool testCountersInsteadOfSums(FunctionPassContext& context, IRBuilder& builder)
{
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    constexpr int64_t farthestAddend = 1ll << 31;
    constexpr int64_t farthestBound = 1ll << 51;
    const NaturalLoops& loops = analyses.naturalLoops();
    const UseLists& useLists = analyses.useLists();
    Vector<TestOfSum, 2> tests;
    for (const LoopInduction& induction : analyses.inductionVariables().loops()) {
        const NaturalLoop& loop = loops.loop(induction.loopIndex);
        for (unsigned index = 0; index < loop.size(); ++index) {
            BlockID block = loop.at(index);
            const Terminator& terminator = function.at(block).terminator;
            if (terminator.kind != TerminatorKind::Branch || loops.belongsTo(terminator.taken().block, loop) == loops.belongsTo(terminator.notTaken().block, loop))
                continue;
            if (!terminator.taken().proofs.isEmpty() || !terminator.notTaken().proofs.isEmpty())
                continue;
            ValueID id = terminator.conditionOperand();
            const Value& comparison = function.at(id);
            if (!relationalOperators.contains(comparison.opcode) || useLists.useCount(id) != 1 || useLists.blockOf(id) != block)
                continue;
            for (uint32_t side = 0; side < 2; ++side) {
                std::optional<SumWithCounter> sum = findSumWithConstant(context, comparison.operand(side));
                if (!sum || !sum->addend || std::abs(sum->addend) > farthestAddend)
                    continue;
                const InductionVariable* variable = induction.find(analyses.identityOf(sum->counter));
                if (!variable || variable->step.kind != InductionStepKind::Constant || variable->steppedValues.contains(analyses.identityOf(comparison.operand(side))))
                    continue;
                const Type& bound = context.program.typeOf(function, comparison.operand(1 - side));
                if (!context.program.typeOf(function, sum->counter).isNumberWithoutFraction() || !bound.isInteger() || bound.integers.minimum < -farthestBound || bound.integers.maximum > farthestBound)
                    continue;
                tests.append({ id, side, *sum });
                break;
            }
        }
    }
    for (const TestOfSum& test : tests) {
        ProgramPoint point = useLists.pointOf(test.comparison);
        builder.setOrigin(function.at(test.comparison).origin());
        Value addend = IRBuilder::make(Opcode::Integer);
        addend.setInteger(test.sum.addend);
        ValueID bound = builder.insertBefore(point, IRBuilder::make(Opcode::Sub, { function.at(test.comparison).operand(1 - test.sideOfSum), builder.insertBefore(point, WTF::move(addend)) }));
        Value& comparison = function.edit(test.comparison, ValueEdit::Definition);
        comparison.setOperand(test.sideOfSum, test.sum.counter);
        comparison.setOperand(1 - test.sideOfSum, bound);
    }
    if (tests.isEmpty())
        return false;
    builder.applyInsertions();
    analyses.didChangeValues();
    return true;
}

// `while (i--)` with a counter that is never negative: that it is not zero says that it is above zero, and only a comparison bounds the counter for later code, here and in the back end.
static bool compareCountersTestedForTruthWithZero(FunctionPassContext& context, IRBuilder& builder)
{
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    const NaturalLoops& loops = analyses.naturalLoops();
    const UseLists& useLists = analyses.useLists();
    Vector<ValueID, 2> tests;
    for (const LoopInduction& induction : analyses.inductionVariables().loops()) {
        const NaturalLoop& loop = loops.loop(induction.loopIndex);
        for (unsigned index = 0; index < loop.size(); ++index) {
            BlockID block = loop.at(index);
            const Terminator& terminator = function.at(block).terminator;
            if (terminator.kind != TerminatorKind::Branch || !loops.belongsTo(terminator.taken().block, loop) || loops.belongsTo(terminator.notTaken().block, loop))
                continue;
            ValueID id = terminator.conditionOperand();
            const Value& test = function.at(id);
            if (test.opcode != Opcode::ToBoolean || useLists.useCount(id) != 1 || useLists.blockOf(id) != block || tests.contains(id))
                continue;
            const Type& tested = context.program.typeOf(function, test.operand(0));
            if (!tested.isInteger() || tested.integers.minimum < 0)
                continue;
            const InductionVariable* variable = induction.find(sameNumberThroughConversions(context, test.operand(0)));
            if (variable && variable->direction == InductionDirection::Decreasing)
                tests.append(id);
        }
    }
    for (ValueID id : tests) {
        Use use = *useLists.singleUse(id);
        ProgramPoint point = useLists.pointOf(id);
        builder.setOrigin(function.at(id).origin());
        ValueID zero = builder.insertBefore(point, IRBuilder::make(Opcode::Integer));
        replaceUse(function, use, builder.insertBefore(point, IRBuilder::make(Opcode::Greater, { function.at(id).operand(0), zero })));
    }
    if (tests.isEmpty())
        return false;
    builder.applyInsertions();
    analyses.didChangeValues();
    return true;
}

// `let sum = i; for (..) sum = (sum + x) | 0`: every turn hands on an int32, and only the first value may be no int32, yet that value makes the sum a double in every turn.
// So the first value is tested once in front of the loop, and the loop gets a copy whose sum is an integer.
static bool copyLoopWhoseFirstValueIsTheOnlyNumber(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    const NaturalLoops& loops = analyses.naturalLoops();
    // `b(x: i32, y: number)` where every edge passes one value for both: y is x, and only x is typed as an int32. The types of the copy go by what the edges pass.
    struct SameValueOfNarrowerType {
        Edge edge;
        uint32_t index;
        ValueID value;
    };
    Vector<SameValueOfNarrowerType, 2> sameValuesOfNarrowerType;
    // What an inner loop hands on is its own first value or an int32, and its first value is that of the outer loop.
    auto isInt32OrComesFrom = [&](ValueID parameter, Edge firstEdge, uint32_t firstIndex) {
        BitVector seen(function.values.size());
        Vector<std::pair<Edge, uint32_t>, 8> worklist { { firstEdge, firstIndex } };
        while (!worklist.isEmpty()) {
            auto [passingEdge, passedIndex] = worklist.takeLast();
            if (passedIndex >= numberOfExplicitArguments(function, passingEdge))
                return false;
            ValueID passed = targetOf(function, passingEdge).arguments[passedIndex];
            ValueID value = analyses.identityOf(passed);
            if (value != parameter && value != passed && program.typeOf(function, value).isInt32() && !program.typeOf(function, passed).isInt32()) {
                if (!analyses.dominates(value, pointBeforeTerminatorOf(function, passingEdge.from)))
                    return false;
                sameValuesOfNarrowerType.append({ passingEdge, passedIndex, value });
            }
            if (value == parameter || program.typeOf(function, value).isInt32() || seen.quickSet(value.index()))
                continue;
            ValueLocation location = analyses.useLists().locationOf(value);
            if (location.placement != ValuePlacement::Parameter || !analyses.isInCycle(location.block))
                return false;
            const CompactList<ValueID>& parametersOfBlock = function.at(location.block).parameters;
            auto found = std::ranges::find(parametersOfBlock, value);
            if (found == parametersOfBlock.end())
                return false;
            uint32_t indexInBlock = found - parametersOfBlock.begin();
            for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
                if (edge.isHandlerEdge() || hasEdgeResults(function, edge))
                    return false;
                worklist.append({ edge, indexInBlock });
            }
        }
        return true;
    };
    // Outer loops come first: their copy holds a copy of what is inside.
    Vector<const NaturalLoop*, 8> widestFirst;
    for (unsigned loopIndex = 0; loopIndex < loops.numLoops(); ++loopIndex)
        widestFirst.append(&loops.loop(loopIndex));
    AOT::stableSortInLine(widestFirst, std::ranges::greater { }, &NaturalLoop::size);
    for (const NaturalLoop* candidate : widestFirst) {
        const NaturalLoop& loop = *candidate;
        BlockID header = loop.header();
        if (function.at(header).frequency.predicted() != BlockFrequency::Normal || !isHot(heatClassOf(context, header)).predicted())
            continue;
        std::optional<Edge> entry = loops.findSingleEntry(loop);
        if (!entry || entry->isHandlerEdge())
            continue;
        std::optional<uint32_t> parameterIndex;
        const CompactList<ValueID>& parameters = function.at(header).parameters;
        for (uint32_t index = 0; index < parameters.size() && !parameterIndex; ++index) {
            const Type& type = program.typeOf(function, parameters[index]);
            if (!type.isNumberWithoutFraction() || type.isInteger() || index >= numberOfExplicitArguments(function, *entry))
                continue;
            sameValuesOfNarrowerType.clear();
            bool handsOnInt32 = std::ranges::all_of(analyses.cfg().incomingEdges(header), [&](Edge edge) {
                return !loops.belongsTo(edge.from, loop) || isInt32OrComesFrom(parameters[index], edge, index);
            });
            ValueID first = targetOf(function, *entry).arguments[index];
            if (!handsOnInt32 || !program.typeOf(function, first).isNumberWithoutFraction())
                continue;
            // An outer loop that takes the sum back holds it as a number all the same, so the test in each of its turns gains nothing.
            auto isTakenBackByOuterLoop = [&] {
                BitVector seen(function.values.size());
                Vector<ValueID, 8> worklist { first };
                while (!worklist.isEmpty()) {
                    ValueID value = analyses.identityOf(worklist.takeLast());
                    if (value == parameters[index])
                        return true;
                    ValueLocation location = analyses.useLists().locationOf(value);
                    if (seen.quickSet(value.index()) || location.placement != ValuePlacement::Parameter)
                        continue;
                    const CompactList<ValueID>& parametersOfBlock = function.at(location.block).parameters;
                    auto found = std::ranges::find(parametersOfBlock, value);
                    if (found == parametersOfBlock.end())
                        continue;
                    uint32_t indexInBlock = found - parametersOfBlock.begin();
                    for (Edge edge : analyses.cfg().incomingEdges(location.block)) {
                        if (!edge.isHandlerEdge() && !hasEdgeResults(function, edge))
                            worklist.append(targetOf(function, edge).arguments[indexInBlock]);
                    }
                }
                return false;
            };
            if (isTakenBackByOuterLoop())
                continue;
            // Beside what may be a fraction the integer is converted in every turn.
            auto isWithoutFraction = [&](ValueID operand) {
                return !(program.typeOf(function, operand).primitives & Primitive::FractionalNumber);
            };
            bool isReckonedWith = std::ranges::any_of(analyses.useLists().usesOf(parameters[index]), [&](const Use& use) {
                if (use.kind != Use::Kind::Operand)
                    return false;
                const Value& user = function.at(use.user);
                if (!hasOperatorForm(user.opcode) || relationalOperators.contains(user.opcode) || !loops.belongsTo(use.block, loop))
                    return false;
                return user.numberOfOperands() != 2 || isWithoutFraction(user.operand(1 - use.operandIndex));
            });
            if (isReckonedWith)
                parameterIndex = index;
        }
        if (!parameterIndex || !isExpectedToRun(context, loop).predicted())
            continue;
        Vector<BlockID> region;
        for (unsigned index = 0; index < loop.size(); ++index)
            region.append(loop.at(index));
        if (RegionCloner::checkCanDuplicate(analyses, region.span()) != CloneCheck::CanClone)
            continue;

        for (const SameValueOfNarrowerType& same : sameValuesOfNarrowerType)
            editTarget(function, same.edge).arguments[same.index] = same.value;
        if (!sameValuesOfNarrowerType.isEmpty())
            analyses.didChangeValues();
        IRBuilder builder(analyses.context(), function, context.types, &analyses);
        builder.setOrigin(function.at(entry->from).terminator.origin());
        BlockID front = builder.splitEdge(*entry);
        Expected<BlockFrequency> frequency = function.at(front).frequency;
        Arguments arguments(function.at(front).terminator.next().arguments);
        function.edit(front, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
        builder.moveToEndOf(front);
        ValueID test = builder.appendTypeTest(arguments[*parameterIndex], TypeTable::int32);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, BlockFrequency::Rare);
        RegionCloner cloner(builder, function);
        cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(header, Arguments(arguments));
        arguments[*parameterIndex] = branch.provenOnTaken;
        builder.moveToEndOf(branch.taken);
        builder.emitJump(cloner.cloneOf(header), WTF::move(arguments));
        analyses.didChangeControlFlow();
        cloner.repairSSA(analyses);
        analyses.didChangeControlFlow();
        for (BlockID block : region)
            function.edit(block, BlockEdit::Rest).frequency = BlockFrequency::Rare;
        analyses.didChangeControlFlow();
        ValueID narrowed = function.at(cloner.cloneOf(header)).parameters[*parameterIndex];
        narrowValueTypes({ context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo }, std::span(&narrowed, 1));
        return true;
    }
    return false;
}

// `for (let i = values.length; i-- > 0;)` over a list that nobody has proven: the counter is of any type until `values` is tested, so the head of the loop asks it for an int32 in every turn.
// What a turn hands on passes by its type, so only the first value can fail. It is asked once, in front of the loop, and goes where the head would have sent it.
static bool testFirstValuesInFrontOfLoops(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    const NaturalLoops& loops = analyses.naturalLoops();
    for (unsigned loopIndex = 0; loopIndex < loops.numLoops(); ++loopIndex) {
        const NaturalLoop& loop = loops.loop(loopIndex);
        BlockID header = loop.header();
        const Terminator& terminator = function.at(header).terminator;
        if (function.at(header).frequency.predicted() != BlockFrequency::Normal || terminator.kind != TerminatorKind::Branch)
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        if (test.opcode != Opcode::TypeTest || !terminator.notTaken().proofs.isEmpty())
            continue;
        BlockID whenPassed = terminator.taken().block;
        BlockID whenFailed = terminator.notTaken().block;
        if (!loops.belongsTo(whenPassed, loop) || !analyses.hasSingleIncomingEdge(whenPassed) || function.at(whenFailed).frequency.predicted() == BlockFrequency::Normal)
            continue;
        const CompactList<ValueID>& parameters = function.at(header).parameters;
        // A first value that fails does not come by the head, so where it goes nothing may name what the head defines.
        auto isNamedOnlyWherePassed = [&](ValueID id) {
            return std::ranges::all_of(analyses.useLists().usesOf(id), [&](const Use& use) {
                return use.block == header || analyses.dominators().dominates(whenPassed, use.block);
            });
        };
        if (!std::ranges::all_of(parameters, isNamedOnlyWherePassed) || !std::ranges::all_of(function.at(header).values, isNamedOnlyWherePassed))
            continue;
        auto indexAmongParameters = [&](ValueID id) {
            return static_cast<size_t>(std::ranges::find(parameters, id) - parameters.begin());
        };
        size_t parameterIndex = indexAmongParameters(analyses.identityOf(test.valueOperand()));
        if (parameterIndex == parameters.size())
            continue;
        // In front of the test the head only polls. A first value that fails polls too: from where it goes a way may lead back here, and every cycle polls.
        Vector<ValueID, 1> polls;
        bool onlyPolls = std::ranges::all_of(function.at(header).values, [&](ValueID id) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::LoopHint)
                return analyses.isUnobservable(id);
            polls.append(id);
            return !value.throwsOwedError() && !value.leavesOwedError() && !value.replacesTailCalls();
        });
        std::optional<Edge> entry = loops.findSingleEntry(loop);
        if (!onlyPolls || !entry || entry->isHandlerEdge() || hasEdgeResults(function, *entry))
            continue;
        const Type& tested = context.types.at(test.testedType());
        bool everyTurnPasses = std::ranges::all_of(analyses.cfg().incomingEdges(header), [&](Edge edge) {
            if (edge == *entry)
                return true;
            return !edge.isHandlerEdge() && !hasEdgeResults(function, edge) && program.typeOf(function, targetOf(function, edge).arguments[parameterIndex]).isSubsetOf(tested, program);
        });
        if (!everyTurnPasses || program.typeOf(function, targetOf(function, *entry).arguments[parameterIndex]).isSubsetOf(tested, program))
            continue;
        // Whatever its type says, a first value that has passed this very test is not asked again.
        ValueLocation firstValue = analyses.useLists().locationOf(targetOf(function, *entry).arguments[parameterIndex]);
        bool hasBeenAsked = firstValue.placement == ValuePlacement::Parameter && firstValue.block != entryBlock && std::ranges::all_of(analyses.cfg().incomingEdges(firstValue.block), [&](Edge edge) {
            const Terminator& asking = function.at(edge.from).terminator;
            if (edge.isHandlerEdge() || asking.kind != TerminatorKind::Branch)
                return false;
            const Value& asked = function.at(asking.conditionOperand());
            return asked.opcode == Opcode::TypeTest && asked.testedType() == test.testedType();
        });
        if (hasBeenAsked)
            continue;
        Arguments first(targetOf(function, *entry).arguments);
        Arguments handedOnWhenFailed;
        bool canHandOn = std::ranges::all_of(terminator.notTaken().arguments, [&](ValueID argument) {
            size_t index = indexAmongParameters(argument);
            if (index != parameters.size()) {
                handedOnWhenFailed.append(first[index]);
                return true;
            }
            handedOnWhenFailed.append(argument);
            return !loops.belongsTo(analyses.useLists().blockOf(argument), loop);
        });
        if (!canHandOn)
            continue;

        TypeID testedType = test.testedType();
        Expected<BlockFrequency> frequencyWhenFailed = function.at(whenFailed).frequency;
        IRBuilder builder(analyses.context(), function, context.types, &analyses);
        builder.setOrigin(test.origin());
        BlockID front = builder.splitEdge(*entry);
        Expected<BlockFrequency> frequency = function.at(front).frequency;
        function.edit(front, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
        builder.moveToEndOf(front);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(first[parameterIndex], testedType), OperandIndex::TypeTest::value, frequency, frequencyWhenFailed);
        builder.moveToEndOf(branch.notTaken);
        for (ValueID poll : polls) {
            builder.setOrigin(function.at(poll).origin());
            builder.append(Value(function.at(poll)));
        }
        builder.emitJump(whenFailed, WTF::move(handedOnWhenFailed));
        first[parameterIndex] = branch.provenOnTaken;
        builder.moveToEndOf(branch.taken);
        builder.emitJump(header, WTF::move(first));
        analyses.didChangeControlFlow();
        ValueID narrowed = function.at(header).parameters[parameterIndex];
        narrowValueTypes({ context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo }, std::span(&narrowed, 1));
        return true;
    }
    return false;
}

// `for (let i = 0; i < n; i++) { for (..) .. }` with `n: number`. Below an int32 the counter is one. PlaceTypeTests tests such a bound in front of a loop that holds no other loop: its copies hold none.
// So the bound of a loop that holds one is tested here, once in front of the loop, and the loop gets a copy whose bound is an int32.
enum class LoopNesting : bool { Outer, Innermost };
static bool copyLoopWhoseBoundIsTheOnlyNumber(FunctionPassContext& context, IRBuilder& builder, LoopNesting nesting)
{
    const Program& program = context.program;
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    const NaturalLoops& loops = analyses.naturalLoops();
    if (nesting == LoopNesting::Outer && loops.numLoops() < 2)
        return false;
    auto typeOf = [&](ValueID id) -> const Type& {
        return program.typeOf(function, id);
    };
    // A sum or a product costs a number no more than an integer, and a list or a variable of numbers takes a number as it is. What pays is an operator that converts a number first, `(i + k) | 0` or `i % 3`, and a test for an integer.
    // Beside what may be a fraction the integer is converted in every turn.
    auto isCheaperAsInteger = [&](const NaturalLoop& loop, const InductionVariable& counter) {
        constexpr size_t mostValuesReckonedFromCounter = 16;
        constexpr OpcodeSet sumsAndProducts { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement, Opcode::Negate };
        Vector<ValueID, mostValuesReckonedFromCounter> reckoned { counter.parameter };
        for (size_t index = 0; index < reckoned.size(); ++index) {
            for (const Use& use : analyses.useLists().usesOf(reckoned[index])) {
                if (use.kind != Use::Kind::Operand || function.at(use.block).frequency.predicted() != BlockFrequency::Normal || !loops.belongsTo(use.block, loop) || counter.steppedValues.contains(use.user))
                    continue;
                const Value& user = function.at(use.user);
                if (user.opcode == Opcode::TypeTest && context.types.at(user.testedType()).isInteger())
                    return true;
                bool isFollowed = isCastWithoutTest(user.opcode);
                // What flow knows to be an integer however far it counts is reckoned with as one.
                if (isFollowed && typeOf(use.user).isInteger() && !typeOf(reckoned[index]).isInteger())
                    return true;
                if (hasOperatorForm(user.opcode) && isLegalOperatorForm(user.opcode, OperatorForm::Integers) && (user.numberOfOperands() != 2 || !(typeOf(user.operand(1 - use.operandIndex)).primitives & Primitive::FractionalNumber))) {
                    if (mayCallRuntime(user.opcode, OperatorForm::Numbers))
                        return true;
                    isFollowed = sumsAndProducts.contains(user.opcode);
                }
                if (isFollowed && !reckoned.contains(use.user) && reckoned.size() < mostValuesReckonedFromCounter)
                    reckoned.append(use.user);
            }
        }
        return false;
    };
    // The loop-carried add has three to four times the latency on doubles.
    auto isBoundByCounterLatency = [&](const NaturalLoop& loop, const InductionVariable& counter, ValueID comparisonWithBound) {
        for (unsigned index = 0; index < loop.size(); ++index) {
            const Block& block = function.at(loop.at(index));
            if (block.frequency.predicted() == BlockFrequency::Normal && std::ranges::any_of(block.values, [&](ValueID id) { return analyses.mayRunOtherCodeOnAnyEdge(id); }))
                return false;
        }
        Vector<ValueID, 4> names { counter.parameter };
        for (size_t index = 0; index < names.size(); ++index) {
            for (const Use& use : analyses.useLists().usesOf(names[index])) {
                if (use.kind != Use::Kind::Operand || function.at(use.block).frequency.predicted() != BlockFrequency::Normal || !loops.belongsTo(use.block, loop) || counter.steppedValues.contains(use.user))
                    continue;
                const Value& user = function.at(use.user);
                if (use.user == comparisonWithBound || (relationalOperators.contains(user.opcode) && !(typeOf(user.operand(1 - use.operandIndex)).primitives & Primitive::FractionalNumber)))
                    continue;
                if (!isCastWithoutTest(user.opcode))
                    return false;
                if (!names.contains(use.user))
                    names.append(use.user);
            }
        }
        return true;
    };
    // Every reentry repeats the tests in front of the loop.
    auto isReenteredAfterSideExit = [&](const NaturalLoop& loop, BlockID testBlock) {
        for (unsigned index = 0; index < loop.size(); ++index) {
            if (loop.at(index) == testBlock)
                continue;
            for (BlockID successor : analyses.cfg().successors(loop.at(index))) {
                if (loops.belongsTo(successor, loop))
                    continue;
                for (unsigned outerIndex = 0; outerIndex < loops.numLoops(); ++outerIndex) {
                    const NaturalLoop& outer = loops.loop(outerIndex);
                    if (&outer != &loop && loops.belongsTo(successor, outer) && loops.belongsTo(loop.header(), outer))
                        return true;
                }
            }
        }
        return false;
    };
    for (unsigned loopIndex = 0; loopIndex < loops.numLoops(); ++loopIndex) {
        const NaturalLoop& loop = loops.loop(loopIndex);
        BlockID header = loop.header();
        auto isNumberThatMayBeCounter = [&](ValueID parameter) {
            return typeOf(parameter).isNumberWithoutFraction() && !typeOf(parameter).isInteger();
        };
        if (function.at(header).frequency.predicted() != BlockFrequency::Normal || std::ranges::none_of(function.at(header).parameters, isNumberThatMayBeCounter))
            continue;
        bool holdsLoop = false;
        for (unsigned index = 0; index < loop.size() && !holdsLoop; ++index)
            holdsLoop = loops.innerMostLoopOf(loop.at(index)) != &loop;
        if (holdsLoop != (nesting == LoopNesting::Outer))
            continue;
        std::optional<Edge> entry = loops.findSingleEntry(loop);
        if (!entry || entry->isHandlerEdge())
            continue;
        ValueID bound;
        std::optional<uint32_t> testedStartIndex;
        const LoopInduction& induction = analyses.inductionVariables().of(loop);
        for (const InductionVariable& variable : induction.variables) {
            if (!isNumberThatMayBeCounter(variable.parameter) || variable.step.kind != InductionStepKind::Constant || !variable.step.constant || !variable.start)
                continue;
            bool isStartInt32 = typeOf(variable.start).isInt32();
            if (!isStartInt32 && (holdsLoop || !typeOf(variable.start).isNumberWithoutFraction()))
                continue;
            int64_t step = variable.step.constant;
            if ((step < 0 ? 0 - static_cast<uint64_t>(step) : static_cast<uint64_t>(step)) > context.limit(LimitID::MaxCounterStep))
                continue;
            for (const InductionTest& test : variable.tests) {
                if (test.tested != TestedValue::BeforeStep || test.boundKind != LoopBoundKind::Invariant || !isBoundingRelation(test.relation, variable.direction))
                    continue;
                const Value& comparison = function.at(test.comparison);
                bool isLeftBound = analyses.identityOf(comparison.operand(0)) == analyses.identityOf(test.bound);
                bool isRightBound = analyses.identityOf(comparison.operand(1)) == analyses.identityOf(test.bound);
                if (isLeftBound == isRightBound)
                    continue;
                ValueID compared = comparison.operand(isLeftBound ? 0 : 1);
                if (!typeOf(compared).isNumber() || typeOf(compared).isInteger() || loops.belongsTo(analyses.useLists().blockOf(compared), loop))
                    continue;
                bool isWorthCopy = holdsLoop ? isCheaperAsInteger(loop, variable) : isBoundByCounterLatency(loop, variable, test.comparison) && !isReenteredAfterSideExit(loop, test.stayingEdge.from);
                if (isWorthCopy && dominatesEveryStepOrItsWayOn(context, induction, test.stayingEdge, variable, comparison.operand(isLeftBound ? 1 : 0))) {
                    bound = compared;
                    if (!isStartInt32)
                        testedStartIndex = variable.parameterIndex;
                }
                break;
            }
            if (bound)
                break;
        }
        if (!bound || !isExpectedToRun(context, loop).predicted())
            continue;
        // The loop as it was becomes rare and is not looked at again, so a nest of loops is copied once for each of its levels at the most.
        Vector<BlockID> region;
        for (unsigned index = 0; index < loop.size(); ++index)
            region.append(loop.at(index));
        if (RegionCloner::checkCanDuplicate(analyses, region.span()) != CloneCheck::CanClone)
            continue;

        builder.setOrigin(function.at(entry->from).terminator.origin());
        BlockID front = builder.splitEdge(*entry);
        Expected<BlockFrequency> frequency = function.at(front).frequency;
        Arguments arguments(function.at(front).terminator.next().arguments);
        function.edit(front, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
        builder.moveToEndOf(front);
        ValueID test = builder.appendTypeTest(bound, TypeTable::int32);
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, BlockFrequency::Rare);
        RegionCloner cloner(builder, function);
        cloner.mapValue(bound, branch.provenOnTaken);
        cloner.cloneBlocks(region.span(), [](ValueID, Value&) { });
        builder.moveToEndOf(branch.notTaken);
        builder.emitJump(header, Arguments(arguments));
        builder.moveToEndOf(branch.taken);
        if (testedStartIndex) {
            IRBuilder::ProvenBranch startBranch = builder.emitBranchWithProof(builder.appendTypeTest(arguments[*testedStartIndex], TypeTable::int32), OperandIndex::TypeTest::value, frequency, BlockFrequency::Rare);
            builder.moveToEndOf(startBranch.notTaken);
            builder.emitJump(header, Arguments(arguments));
            arguments[*testedStartIndex] = startBranch.provenOnTaken;
            builder.moveToEndOf(startBranch.taken);
        }
        builder.emitJump(cloner.cloneOf(header), WTF::move(arguments));
        analyses.didChangeControlFlow();
        cloner.repairSSA(analyses);
        analyses.didChangeControlFlow();
        for (BlockID block : region)
            function.edit(block, BlockEdit::Rest).frequency = BlockFrequency::Rare;
        analyses.didChangeControlFlow();
        return true;
    }
    return false;
}

// The numbers that are no integers among what the loops that are not rare carry from turn to turn.
static size_t countNumbersCarriedByLoops(FunctionPassContext& context)
{
    const Function& function = context.function;
    const NaturalLoops& loops = context.analyses.naturalLoops();
    size_t count = 0;
    for (unsigned index = 0; index < loops.numLoops(); ++index) {
        const Block& header = function.at(loops.loop(index).header());
        if (header.frequency.predicted() != BlockFrequency::Normal)
            continue;
        for (ValueID parameter : header.parameters) {
            const Type& type = context.program.typeOf(function, parameter);
            count += type.isNumberWithoutFraction() && !type.isInteger();
        }
    }
    return count;
}

static bool comparesNoVariableInCycle(FunctionPassContext& context)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    return std::ranges::any_of(analyses.cfg().reversePostOrder(), [&](BlockID block) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || function.at(block).frequency.predicted() != BlockFrequency::Normal)
            return false;
        const Value& comparison = function.at(terminator.conditionOperand());
        if ((comparison.opcode != Opcode::ToBoolean && !relationalOperators.contains(comparison.opcode)) || !analyses.isInCycle(block))
            return false;
        for (uint32_t operandIndex = 0; operandIndex < comparison.numberOfOperands(); ++operandIndex) {
            const Type& tested = context.program.typeOf(function, comparison.operand(operandIndex));
            if (!tested.isNumberWithoutFraction() || tested.isInt32() || terminator.taken().proofs.contains(operandIndex))
                continue;
            // `++i < values.length` before anybody has tested `values`: the one round that tries is the one in which the bound is an integer.
            if (comparison.opcode != Opcode::ToBoolean && !context.program.typeOf(function, comparison.operand(1 - operandIndex)).isInteger())
                continue;
            ValueID number = sameNumberThroughConversions(context, comparison.operand(operandIndex));
            if (analyses.useLists().locationOf(number).placement != ValuePlacement::Parameter ? !!findParameterCountedFrom(context, number) : !findSumsAheadOfTest(context, Edge { block, 0 }, comparison.operand(operandIndex)).isEmpty())
                return true;
        }
        return false;
    });
}

static bool specializeLoopsForInt32Bounds(FunctionPassContext& context, IRBuilder& builder, LoopNesting nesting)
{
    // All the copies together may not outgrow the function, or what a few small loops come to.
    AOT_THRESHOLD(size_t, valuesThatCopiesOfLoopsMayAlwaysAdd, 4096);
    const size_t mostValuesWithCopies = context.function.values.size() + std::max(context.function.values.size(), valuesThatCopiesOfLoopsMayAlwaysAdd);
    bool hasCopy = false;
    while (context.function.values.size() <= mostValuesWithCopies && copyLoopWhoseBoundIsTheOnlyNumber(context, builder, nesting))
        hasCopy = true;
    return hasCopy;
}

PassOutcome convertCountersToIntegers(FunctionPassContext& context)
{
    bool isFinalRun = context.isBehindRounds && std::exchange(context.hasConvertedCountersBehindRounds, true);
    bool hasNewlyTestedBounds = !isFinalRun || std::exchange(context.hasBoundsTestedBehindRounds, false);
    if (!context.analyses.cfg().hasCycle() || (!hasNumberParameter(context.program, context.function) && !hasIntegerBeyondInt32(context.program, context.function)))
        return PassOutcome::Unchanged;

    PassOutcome outcome = PassOutcome::Unchanged;
    uint64_t numberOfConvertedCounters = 0;
    BitVector parametersOverStepLimit;
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    if (isFinalRun && specializeLoopsForInt32Bounds(context, builder, LoopNesting::Innermost))
        outcome = PassOutcome::Changed;
    else if (!hasNewlyTestedBounds)
        return outcome;
    if (testsSumInCycle(context) && testCountersInsteadOfSums(context, builder))
        outcome = PassOutcome::Changed;
    if (compareCountersTestedForTruthWithZero(context, builder))
        outcome = PassOutcome::Changed;
    // During the rounds, only a counter that has just got its own test, or of whose test the front end has passed on nothing, is converted, before the tests that the round would place on it:
    // one for int32 stays in the loop for good where the counter may pass int32. Other counters have their bound by then or get it after the rounds.
    bool convertsOnlyWhatComparesNoVariable = context.isInRounds && outcome == PassOutcome::Unchanged;
    if (convertsOnlyWhatComparesNoVariable && (context.hasTriedCountersThatCompareNoVariable || !comparesNoVariableInCycle(context)))
        return outcome;
    context.hasTriedCountersThatCompareNoVariable |= convertsOnlyWhatComparesNoVariable;
    // Typing may cut blocks and renumber the rest, so each search asks the function as it stands. A loop whose first value has been asked hands its head nothing that can fail.
    while (!convertsOnlyWhatComparesNoVariable && testFirstValuesInFrontOfLoops(context))
        outcome = PassOutcome::Changed;
    if (Vector<ValueID, 2> narrowed = convertsOnlyWhatComparesNoVariable ? Vector<ValueID, 2> { } : testForIntegersOnWaysBack(context); !narrowed.isEmpty()) {
        outcome = PassOutcome::Changed;
        narrowValueTypes({ context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo }, narrowed.span());
    }
    for (;;) {
        Vector<CounterCandidate, 4> candidates = findCounterCandidates(context, parametersOverStepLimit);
        if (convertsOnlyWhatComparesNoVariable) {
            // An unlikely path that widens the counter is taken out of its loop behind the rounds, and only as long as it widens it.
            candidates.removeAllMatching([&](const CounterCandidate& candidate) {
                return !candidate.comparesNoVariable || context.function.at(candidate.stayingEdge.from).frequency.predicted() != BlockFrequency::Normal;
            });
            if (candidates.isEmpty() && !numberOfConvertedCounters)
                break;
        }
        if (candidates.isEmpty()) {
            Vector<Tally, 2> tallies = findTallies(context);
            if (tallies.isEmpty())
                break;
            Vector<ValueID, 4> bounded = boundTallies(context, builder, tallies.span());
            outcome = PassOutcome::Changed;
            numberOfConvertedCounters += tallies.size();
            if (narrowValueTypes({ context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo }, bounded.span()).didChangeControlFlow())
                parametersOverStepLimit.clearAll();
            continue;
        }
        Vector<ValueID, 4> narrowedCounters = passCountersOnEdges(context, builder, candidates.span());
        outcome = PassOutcome::Changed;

        // A counter is typed from itself, so it and whatever else the narrowed values reach start over from never. Nothing else is typed from them.
        TypePropagationOutcome typing = narrowValueTypes({ context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo }, narrowedCounters.span());
        // Cutting a block renumbers the values, so no id from before names its value any more.
        if (typing.didChangeControlFlow()) {
            parametersOverStepLimit.clearAll();
            continue;
        }
        numberOfConvertedCounters += std::ranges::count_if(candidates, [&](const CounterCandidate& candidate) {
            return context.program.typeOf(context.function, candidate.parameter).isInteger();
        });
    }

    // What a comparison has just been found to say may be all that a turn needed to hand on what passes.
    while (!convertsOnlyWhatComparesNoVariable && testFirstValuesInFrontOfLoops(context))
        outcome = PassOutcome::Changed;

    if (numberOfConvertedCounters) {
        context.build.statistics().count(StatisticID::CountersConverted, numberOfConvertedCounters);
        context.hasCountersConvertedSinceTestsWerePlaced = true;
    }
    if (!convertsOnlyWhatComparesNoVariable && expectCarriedNumbersToStayInt32(context, builder)) {
        context.hasCountersConvertedSinceTestsWerePlaced = true;
        outcome = PassOutcome::Changed;
    }
    // The loop that is copied becomes rare, and its copy carries one number fewer. Where the types do not bear that out, a copy of the copy would fare no better, and so on without end.
    // Typing the copy may cut blocks and renumber the rest, so this asks the function as it stands and keeps no id.
    // Each number takes a copy of its own, so a loop that carries many would be copied as often. The copies together may not outgrow the function, or what a few small loops come to.
    size_t numbersCarried = convertsOnlyWhatComparesNoVariable ? 0 : countNumbersCarriedByLoops(context);
    AOT_THRESHOLD(size_t, valuesThatCopiesOfLoopsMayAlwaysAdd, 4096);
    size_t mostValuesWithCopies = context.function.values.size() + std::max(context.function.values.size(), valuesThatCopiesOfLoopsMayAlwaysAdd);
    while (numbersCarried && context.function.values.size() <= mostValuesWithCopies && copyLoopWhoseFirstValueIsTheOnlyNumber(context)) {
        context.hasCountersConvertedSinceTestsWerePlaced = true;
        outcome = PassOutcome::Changed;
        size_t numbersLeft = countNumbersCarriedByLoops(context);
        if (numbersLeft >= numbersCarried)
            break;
        numbersCarried = numbersLeft;
    }
    // The analyses were told of every edit, and the search that found no more candidates came behind the last one.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

// In the rounds a loop inside may yet go: its turns are written out, it is fused, what it walks is sunk. Then PlaceTypeTests tests the bound, and a copy made here would stand in the way.
PassOutcome testNumbersThatBoundLoopsAroundLoops(FunctionPassContext& context)
{
    RELEASE_ASSERT(!context.isInRounds);
    bool hasCopy = false;
    {
        IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
        hasCopy = specializeLoopsForInt32Bounds(context, builder, LoopNesting::Outer);
    }
    if (!hasCopy)
        return PassOutcome::Unchanged;
    convertCountersToIntegers(context);
    return PassOutcome::Changed;
}

static MemoryAccess variableAccessOf(const Function& function, const Value& value)
{
    MemoryAccess access = memoryAccessOf(function, value);
    if (!access || !heapsOfVariables.contains(access.location.heap()))
        return { };
    return access;
}

static void addEffects(const EffectsOnEdges& effects, LoopClobbers& clobbers)
{
    Effects any = effects.onAnyEdge();
    clobbers.maySuspend |= any.maySuspend;
    if (any.maySuspend || (clobbers.otherCodeMayWriteAnything && any.mayRunOtherCode()))
        clobbers.writes = LocationMasks::top();
    else
        clobbers.writes.merge(any.writes);
}

static void addClobbers(FunctionPassContext& context, ValueID id, LoopClobbers& clobbers)
{
    MemoryAccess access = variableAccessOf(context.function, context.function.at(id));
    if (access.kind == MemoryAccess::Kind::Write)
        clobbers.locations.append(access.location);
    else
        addEffects(context.analyses.effectsOf(id), clobbers);
}

static bool staysBehindFirstTest(FunctionPassContext& context, const NaturalLoop& loop)
{
    return std::ranges::any_of(context.analyses.inductionVariables().of(loop).variables, [&](const InductionVariable& variable) {
        return std::ranges::any_of(variable.tests, [&](const InductionTest& test) {
            return test.block == loop.header() && test.tested == TestedValue::BeforeStep && test.entryRelation == EntryRelation::Holds;
        });
    });
}

// The one block of the loop that the branch of the header leads to, if nothing else leads there.
static BlockID findBlockBehindHeader(FunctionPassContext& context, const NaturalLoop& loop)
{
    const NaturalLoops& loops = context.analyses.naturalLoops();
    const Terminator& terminator = context.function.at(loop.header()).terminator;
    if (!terminator.isConditionalBranch() || loops.belongsTo(terminator.taken().block, loop) == loops.belongsTo(terminator.notTaken().block, loop))
        return { };
    BlockID next = loops.belongsTo(terminator.taken().block, loop) ? terminator.taken().block : terminator.notTaken().block;
    return next != loop.header() && context.analyses.cfg().incomingEdges(next).size() == 1 ? next : BlockID();
}

static bool readsWhatMayChangeBehindHeader(FunctionPassContext& context, const NaturalLoop& loop)
{
    BlockID behindHeader = findBlockBehindHeader(context, loop);
    return behindHeader && std::ranges::any_of(context.function.at(behindHeader).values, [&](ValueID id) {
        const Value& value = context.function.at(id);
        if (!hoistableReadOpcodes.contains(value.opcode))
            return false;
        MemoryAccess access = variableAccessOf(context.function, value);
        return access.kind == MemoryAccess::Kind::Read && stabilityOf(context.program, value, access) != LocationStability::UnchangedDuringActivation;
    });
}

// The read goes ahead of the fact that it names, so the word it finds may be anything. The cast that stays names the fact.
static ValueID readAheadAndAssumeInPlace(FunctionPassContext& context, IRBuilder& builder, BlockValues& remaining, ValueID id)
{
    Function& function = context.function;
    Value ahead = function.at(id);
    ValueID fact = ahead.factOperand();
    ahead.setFactOperand({ });
    ahead.setMayPrecedeItsStore(true);
    builder.setOrigin(ahead.origin());
    ValueID read = builder.addDetached(WTF::move(ahead));

    Value& value = function.edit(id, ValueEdit::Definition);
    value.becomeAssumeType(read, Proven::behindFact(value.type));
    value.setFactOperand(fact);
    remaining.append(id);
    return read;
}

// In the header what stands in front of a read is known, and so it is in the block behind a test of the header that holds when the loop is entered. Anything in the loop may have run in front of a read in another block.
// A loop that is entered from a branch has no block in front of it for what leaves it: such an entry is listed where something would leave.
static bool isCollectionLookup(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    const BuiltinOverload& overload = program.builtins().at(value.overload());
    if (overload.mode != BuiltinCallMode::Call)
        return false;
    BuiltinID builtin = program.builtins().canonicalID(overload.builtin);
    return builtin == Builtins::MapPrototypeGet || builtin == Builtins::MapPrototypeHas || builtin == Builtins::SetPrototypeHas;
}

static ValueID listWhoseLengthIsRead(const Program& program, const Value& value)
{
    if (value.opcode == Opcode::GetArrayLength)
        return value.arrayOperand();
    if (value.opcode != Opcode::CallBuiltin || !value.receiverOperand() || value.needsPristineReceiver())
        return { };
    const BuiltinOverload& overload = program.builtins().at(value.overload());
    return overload.mode == BuiltinCallMode::Get && program.builtins().canonicalID(overload.builtin) == Builtins::ArrayPrototypeLength ? value.receiverOperand() : ValueID();
}

// These look at how a list keeps its elements and not at how many it has.
static constexpr OpcodeSet readersOfStorageFormAlone { Opcode::HasArrayStorage, Opcode::NewArrayWithCapacity };

struct ListsLeftAlone {
    BitVector lengths;
    Vector<ValueID> tests;
    // Tests in the loop that have been told already. What one of them was told holds for all of them: the list has one length all the way round.
    Vector<ValueID> testsToldLength;
    // In order. The tests of such a list are told the length only if it leaves the loop: they and the comparison with it go on asking the same.
    Vector<ValueID> listsMeasuredInLoop;
};

// `for (let k = 0; k < xs.length; k++) out.push(xs[k])`: the loop asks for the length of one list and writes the length of another. Asked before anything moves: a program point is a place in the blocks as they stand.
// Where the loop writes to no list at all there is nothing to find: the machine code reads each length once by itself.
// A length leaves the loop only where everything in the loop that asks for it can be told it. Otherwise one would compare with the length in front of the loop and another with the length in the storage, and nobody could see that they ask the same.
static ListsLeftAlone findListsTheLoopLeavesAlone(FunctionPassContext& context, const NaturalLoop& loop, std::optional<UseEffectHistory>& captures)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;

    auto reliesOnProofOfPosition = [](const Value& value) {
        return Value::opcodesWithIsInBounds.contains(value.opcode) && value.isInBounds();
    };
    struct Reader {
        ValueID list;
        ValueID id;
    };
    Vector<Reader, 8> readers;
    BitVector isLengthRead;
    bool hasSomethingToTellOrMove = false;
    LocationMasks reads { AbstractHeapSet { AbstractHeap::ArrayStorage } };
    for (unsigned i = 0; i < loop.size(); ++i) {
        for (ValueID id : function.at(loop.at(i)).values) {
            const Value& value = function.at(id);
            if (testsOfPositionsInLists.contains(value.opcode)) {
                bool isTold = !value.lengthOperands().empty();
                if (isTold || !reliesOnProofOfPosition(value)) {
                    readers.append({ analyses.identityOf(value.arrayOperand()), id });
                    hasSomethingToTellOrMove |= !isTold;
                }
                continue;
            }
            ValueID list = listWhoseLengthIsRead(context.program, value);
            if (!list || !context.program.typeOf(function, id).isInteger())
                continue;
            Effects effects = analyses.effectsOf(id).onAnyEdge();
            if (!effects.isUnobservable())
                continue;
            readers.append({ analyses.identityOf(list), id });
            isLengthRead.set(id.index());
            hasSomethingToTellOrMove = true;
            reads.merge(effects.reads);
        }
    }
    if (!hasSomethingToTellOrMove)
        return { };

    struct Writer {
        ValueID list;
        ProgramPoint point;
    };
    Vector<Writer, 8> writers;
    Vector<ValueID, 8> listsReadOtherwise;
    for (unsigned i = 0; i < loop.size(); ++i) {
        BlockID block = loop.at(i);
        Effects ofTerminator = analyses.effectsOfTerminator(block).onAnyEdge();
        if (ofTerminator.mayRunOtherCode() || ofTerminator.writes.overlaps(reads))
            return { };
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& value = function.at(values[index]);
            const EffectsOnEdges& effects = analyses.effectsOf(values[index]);
            Effects any = effects.onAnyEdge();
            bool mayWrite = any.mayRunOtherCode() || any.writes.overlaps(reads);
            bool readsOtherwise = any.reads.contains(AbstractHeap::ArrayStorage) && !isLengthRead.get(values[index].index()) && !testsOfPositionsInLists.contains(value.opcode) && !readersOfStorageFormAlone.contains(value.opcode) && !reliesOnProofOfPosition(value);
            if (!mayWrite && !readsOtherwise)
                continue;
            // A push that runs no code reads and writes the list it pushes to and nothing else.
            if (value.opcode == Opcode::CallBuiltin && !any.mayRunOtherCode() && inlineImplementationOf(context.program, value) == InlineBuiltin::ArrayPush) {
                writers.append({ value.receiverOperand(), { block, index } });
                continue;
            }
            Effects beyond = effectsBeyondOperands(analyses.effectContext(), function, value, effects).onAnyEdge();
            if (beyond.mayRunOtherCode() || beyond.writes.overlaps(reads))
                return { };
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                OperandMemory memory = memoryAccessedThrough(analyses.effectContext(), function, value, operandIndex, effects);
                if (memory.writes.overlaps(reads))
                    writers.append({ operand, { block, index } });
                else if (readsOtherwise && memory.reads.contains(AbstractHeap::ArrayStorage))
                    listsReadOtherwise.append(analyses.identityOf(operand));
            });
        }
    }
    if (writers.isEmpty())
        return { };
    const EscapeAnalysis& escape = analyses.escape();
    if (escape.gaveUp())
        return { };

    // Each list that is written to is looked at once, and each list that is measured is held against those.
    AOT::sortInLine(writers, { }, [](const Writer& writer) { return writer.list; });
    AOT::sortInLine(readers, { }, [](const Reader& reader) { return reader.list; });
    AOT::sortInLine(listsReadOtherwise);
    struct WrittenList {
        ValueID list;
        bool isHeldByNobodyElse;
    };
    // A list that nobody else holds yet is not the list that is measured, whatever the measured one came from. The history of captures starts over where the list is made, so an earlier turn's list does not count.
    auto isHeldByNobodyElse = [&](std::span<const Writer> writersOfList) {
        PointsToSet written = escape.pointsTo(writersOfList.front().list);
        if (written.mayBeUntracked || written.objects.isEmpty())
            return false;
        for (ObjectID object : written.objects) {
            if (escape.at(object).kind != ObjectKind::Allocation || escape.at(object).state == EscapeState::Unknown)
                return false;
            if (!captures)
                captures.emplace(analyses, UseEffectHistoryOptions { .uses = { OperandUse::Retains } });
            for (const Writer& writer : writersOfList) {
                if (captures->occurredBefore(object, writer.point) != Occurrence::HasNotOccurred)
                    return false;
            }
        }
        return true;
    };
    Vector<WrittenList, 4> writtenLists;
    for (size_t first = 0, end; first < writers.size(); first = end) {
        for (end = first + 1; end < writers.size() && writers[end].list == writers[first].list; ++end) { }
        writtenLists.append({ writers[first].list, isHeldByNobodyElse(writers.span().subspan(first, end - first)) });
    }
    auto isLeftAlone = [&](ValueID measured) {
        return std::ranges::all_of(writtenLists, [&](const WrittenList& written) {
            if (!escape.mayAlias(written.list, measured))
                return true;
            return written.isHeldByNobodyElse && !escape.pointsTo(written.list).objects.overlaps(escape.pointsTo(measured).objects);
        });
    };

    ListsLeftAlone found;
    for (size_t first = 0, end; first < readers.size(); first = end) {
        for (end = first + 1; end < readers.size() && readers[end].list == readers[first].list; ++end) { }
        std::span<const Reader> readersOfList = readers.span().subspan(first, end - first);
        bool isMeasuredInLoop = std::ranges::any_of(readersOfList, [&](const Reader& reader) { return isLengthRead.get(reader.id.index()); });
        if ((isMeasuredInLoop && std::ranges::binary_search(listsReadOtherwise, readers[first].list)) || !isLeftAlone(readers[first].list))
            continue;
        if (isMeasuredInLoop)
            found.listsMeasuredInLoop.append(readers[first].list);
        for (const Reader& reader : readersOfList) {
            const Value& value = function.at(reader.id);
            if (!testsOfPositionsInLists.contains(value.opcode))
                found.lengths.set(reader.id.index());
            else if (value.lengthOperands().empty())
                found.tests.append(reader.id);
            else
                found.testsToldLength.append(reader.id);
        }
    }
    return found;
}

// The list has the length that was read in front of the loop all the way round, so whoever tests a position need not read it again. Nor does a store to another list in the loop then make them read it: both lengths lie in one heap for the machine code.
// A length that has left this loop holds in all of it, however far it goes from here.
static unsigned tellTestsLengthInFrontOfLoop(FunctionPassContext& context, const NaturalLoop& loop, std::span<const ValueID> lengthsThatLeftLoop, const ListsLeftAlone& lists)
{
    std::span<const ValueID> tests = lists.tests.span();
    if (tests.empty())
        return 0;
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    struct Length {
        ValueID list;
        ValueID id;
    };
    Vector<Length, 4> lengths;
    for (ValueID id : lengthsThatLeftLoop)
        lengths.append({ analyses.identityOf(listWhoseLengthIsRead(context.program, function.at(id))), id });
    size_t numberThatLeftLoop = lengths.size();
    for (ValueID id : lists.testsToldLength)
        lengths.append({ analyses.identityOf(function.at(id).arrayOperand()), function.at(id).lengthOperands()[0] });

    LocationMasks reads { AbstractHeapSet { AbstractHeap::ArrayStorage } };
    auto mayChangeLength = [&](Effects effects) {
        return effects.mayRunOtherCode() || effects.writes.overlaps(reads);
    };
    std::optional<Edge> entry = analyses.naturalLoops().findSingleEntry(loop);
    if (entry && !entry->isHandlerEdge() && !mayChangeLength(analyses.effectsOfTerminator(entry->from).onAnyEdge())) {
        const BlockValues& valuesInFront = function.at(entry->from).values;
        for (size_t index = valuesInFront.size(); index--;) {
            ValueID id = valuesInFront[index];
            if (mayChangeLength(analyses.effectsOf(id).onAnyEdge()))
                break;
            if (ValueID measured = listWhoseLengthIsRead(context.program, function.at(id)); measured && context.program.typeOf(function, id).isInteger())
                lengths.append({ analyses.identityOf(measured), id });
        }
    }
    if (lengths.isEmpty())
        return 0;
    unsigned numberOfTestsTold = 0;
    for (ValueID test : tests) {
        if (!function.at(test).lengthOperands().empty())
            continue;
        ValueID list = analyses.identityOf(function.at(test).arrayOperand());
        size_t index = lengths.findIf([&](const Length& length) { return length.list == list; });
        if (index == notFound || (index >= numberThatLeftLoop && std::ranges::binary_search(lists.listsMeasuredInLoop, list)))
            continue;
        function.edit(test).setLengthOperands(std::span(&lengths[index].id, 1));
        ++numberOfTestsTold;
    }
    return numberOfTestsTold;
}

static unsigned hoistReadsOutOf(FunctionPassContext& context, const NaturalLoop& loop, Vector<BlockID>& definingBlock, const BitVector& headersWhoseTestHoldsOnEntry, const BitVector& lengthsLeftAlone, Vector<ValueID, 4>& lengthsThatLeftLoop, Vector<Edge, 2>& entriesFromBranches)
{
    const Program& program = context.program;
    Function& function = context.function;
    BlockID header = loop.header();
    if (isClosedOnlyByWaysBack(context.analyses, loop).predicted())
        return 0;
    if (function.at(header).isHandlerBlock())
        return 0;
    BitVector body(function.blocks.size());
    for (unsigned i = 0; i < loop.size(); ++i)
        body.quickSet(loop.at(i).index());
    auto hasInvariantOperands = [&](const Value& value) {
        bool result = true;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            BlockID block = definingBlock[operand.index()];
            result &= block && !body.get(block.index());
        });
        return result;
    };
    bool hasCandidate = false;
    for (unsigned i = 0; i < loop.size() && !hasCandidate; ++i) {
        hasCandidate = std::ranges::any_of(function.at(loop.at(i)).values, [&](ValueID id) {
            const Value& value = function.at(id);
            if (lookupsHoistedWhereTheyRunNoCode.contains(value.opcode) || testsOfValuesThemselves.contains(value.opcode) || isCollectionLookup(program, value) || lengthsLeftAlone.get(id.index()))
                return hasInvariantOperands(value);
            return hoistableReadOpcodes.contains(value.opcode) && variableAccessOf(function, value).kind == MemoryAccess::Kind::Read;
        });
    }
    if (!hasCandidate)
        return 0;
    std::optional<Edge> entry = context.analyses.naturalLoops().findSingleEntry(loop);
    if (!entry || entry->isHandlerEdge())
        return 0;
    bool isEnteredFromBranch = function.at(entry->from).terminator.kind != TerminatorKind::Jump;
    if (isEnteredFromBranch && !function.at(entry->from).terminator.isConditionalBranch())
        return 0;
    BlockID preheader = entry->from;

    LoopClobbers clobbers;
    for (size_t index : body) {
        for (ValueID id : function.at(BlockID(index)).values)
            addClobbers(context, id, clobbers);
        addEffects(context.analyses.effectsOfTerminator(BlockID(index)), clobbers);
    }
    AOT::sortInLine(clobbers.locations);
    if (clobbers.maySuspend)
        return 0;

    BitVector keptReads = context.build.validates() ? findReadsKeptForValidation(function) : BitVector();
    auto canGoAheadOfItsFact = [&](const Value& value) {
        return value.opcode == Opcode::GetModuleVariable && program.at(value.type).isJavaScriptValue() && !program.at(value.type).mayBeUnspecified();
    };
    // What was entered where the first turn is sure to get to, with every variable as it is ahead of the loop, may as well be entered ahead of the loop. What names the fact follows it.
    bool firstTurnGetsHere = true;
    Vector<ValueID, 4> factsOfFirstTurn;
    auto namesFactOfFirstTurn = [&](const Value& value) {
        return value.opcode == Opcode::GetModuleVariable && factsOfFirstTurn.contains(value.factOperand());
    };
    BlockID behindHeader = headersWhoseTestHoldsOnEntry.get(header.index()) ? findBlockBehindHeader(context, loop) : BlockID();
    auto canHoist = [&](ValueID id, BlockID block, const LoopClobbers& clobbersBeforeRead) {
        const Value& value = function.at(id);
        if (testsOfValuesThemselves.contains(value.opcode)) {
            // A comparison is declared to throw where it flattens a rope and memory runs out: it does not leave its handler.
            bool staysUnderItsHandler = function.at(block).handler == function.at(preheader).handler || !context.analyses.effectsOf(id).normal.mayThrow;
            return staysUnderItsHandler && !keptReads.get(id.index()) && hasInvariantOperands(value);
        }
        // A look-up that runs no code and cannot throw finds the same in every turn while nothing that it reads is written, and nobody sees it where no turn asks.
        if (lookupsHoistedWhereTheyRunNoCode.contains(value.opcode)) {
            if (keptReads.get(id.index()) || !hasInvariantOperands(value))
                return false;
            // An accessor that nobody sees run may still answer with a new object each time.
            ProvenAccess storage;
            const ProvenAccess& proven = provenAccessOf(program, function, value, storage);
            if (!proven.isComplete || !proven.accessors.isEmpty())
                return false;
            Effects effects = context.analyses.effectsOf(id).onAnyEdge();
            return effects.isUnobservable() && !effects.reads.overlaps(clobbers.writes);
        }
        // So does a look-up in a Map or a Set while the loop writes to none.
        if (isCollectionLookup(program, value)) {
            if (keptReads.get(id.index()) || !hasInvariantOperands(value))
                return false;
            // What the look-up writes itself is seen nowhere: the hash of a string, a table laid out anew. It throws only for lack of memory.
            Effects effects = context.analyses.effectsOf(id).onAnyEdge();
            return effects.isRemovableWhenUnused() && !effects.reads.without(heapsWhoseWritesNobodySees).overlaps(clobbers.writes);
        }
        if (lengthsLeftAlone.get(id.index()))
            return !keptReads.get(id.index()) && hasInvariantOperands(value);
        if (!hoistableReadOpcodes.contains(value.opcode))
            return false;
        MemoryAccess access = variableAccessOf(function, value);
        if (access.kind != MemoryAccess::Kind::Read || keptReads.get(id.index()))
            return false;
        if (std::ranges::binary_search(clobbers.locations, access.location) && !context.build.isSabotaged(Sabotage::HoistedReadsAreStoredInTheirLoops))
            return false;

        LocationStability stability = stabilityOf(program, value, access);
        bool mayPrecedeStore = program.at(value.type).primitives & Primitive::Absent;
        if (stability == LocationStability::ChangedByOwnStores && mayPrecedeStore)
            stability = LocationStability::Unknown;
        // That a location is stored before it is read may hang on a test in front of the read, and a block that is not the header may not run at all.
        if (stability != LocationStability::UnchangedDuringActivation && block != header && block != behindHeader)
            return false;
        if (stability != LocationStability::UnchangedDuringActivation && clobbersBeforeRead.writes.overlaps(program, access.location))
            return false;
        if (stability == LocationStability::Unknown && clobbers.writes.overlaps(program, access.location))
            return false;
        return hasInvariantOperands(value) || namesFactOfFirstTurn(value) || canGoAheadOfItsFact(value);
    };

    unsigned numberOfHoistedReads = 0;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
    Expected<BlockFrequency> frequencyOfPreheader = frequencies.classOf(preheader);
    LoopClobbers clobbersSinceEntry { .otherCodeMayWriteAnything = true };
    for (BlockID blockID : context.analyses.cfg().reversePostOrder()) {
        if (!body.get(blockID.index()))
            continue;
        // In front of the loop a read runs each time the loop is entered, and what it read is kept all the way round. A block that seldom runs pays for its own reads.
        Expected<BlockFrequency> frequency = frequencies.classOf(blockID);
        if (frequency.predicted() == BlockFrequency::Rare || (frequency > frequencyOfPreheader).predicted()) {
            if (blockID == header)
                behindHeader = { };
            continue;
        }
        bool followsEntry = blockID == header || blockID == behindHeader;
        unsigned numberOfReadsBefore = numberOfHoistedReads;
        BlockValues remaining;
        remaining.reserveInitialCapacity(function.at(blockID).values.size());
        for (ValueID id : function.at(blockID).values) {
            if (!canHoist(id, blockID, followsEntry ? clobbersSinceEntry : clobbers)) {
                if (followsEntry) {
                    addClobbers(context, id, clobbersSinceEntry);
                    firstTurnGetsHere &= !mayLeaveOrStoreToVariable(context.analyses.effectsOf(id).onAnyEdge());
                    if (firstTurnGetsHere && function.at(id).opcode == Opcode::Entered && hasInvariantOperands(function.at(id)))
                        factsOfFirstTurn.append(id);
                }
                remaining.append(id);
                continue;
            }
            if (isEnteredFromBranch) {
                entriesFromBranches.append(*entry);
                return 0;
            }
            ++numberOfHoistedReads;
            if (namesFactOfFirstTurn(function.at(id))) {
                ValueID fact = function.at(id).factOperand();
                factsOfFirstTurn.removeFirst(fact);
                BlockValues& valuesAroundFact = definingBlock[fact.index()] == blockID ? remaining : function.edit(definingBlock[fact.index()]).values;
                valuesAroundFact.removeFirst(fact);
                function.edit(preheader).values.append(fact);
                definingBlock[fact.index()] = preheader;
            }
            if (!hasInvariantOperands(function.at(id))) {
                function.edit(preheader).values.append(readAheadAndAssumeInPlace(context, builder, remaining, id));
                appendKeptReadForValidation(builder, remaining, id);
                continue;
            }
            function.edit(preheader).values.append(id);
            definingBlock[id.index()] = preheader;
            appendKeptReadForValidation(builder, remaining, id);
            if (lengthsLeftAlone.get(id.index()))
                lengthsThatLeftLoop.append(id);
            if (context.build.validates() && blockID != header && function.at(id).opcode == Opcode::GetModuleVariable)
                function.edit(id).setIsValidatedWhereLookedAt(true);
        }
        if (blockID == header) {
            addEffects(context.analyses.effectsOfTerminator(header), clobbersSinceEntry);
            firstTurnGetsHere &= !mayLeaveOrStoreToVariable(context.analyses.effectsOfTerminator(header).onAnyEdge());
        }
        if (numberOfHoistedReads > numberOfReadsBefore)
            function.edit(blockID).values = WTF::move(remaining);
        while (definingBlock.size() < function.values.size())
            definingBlock.append(blockID);
    }
    return numberOfHoistedReads;
}

static unsigned hoistReadsOutOfLoops(FunctionPassContext& context, Vector<Edge, 2>& entriesFromBranches)
{
    const Function& function = context.function;
    CFG& cfg = context.analyses.cfg();
    NaturalLoops& naturalLoops = context.analyses.naturalLoops();
    Vector<const NaturalLoop*> loopsInnermostFirst;
    loopsInnermostFirst.reserveInitialCapacity(naturalLoops.numLoops());
    for (unsigned i = 0; i < naturalLoops.numLoops(); ++i)
        loopsInnermostFirst.append(&naturalLoops.loop(i));
    if (loopsInnermostFirst.isEmpty())
        return 0;
    AOT::sortInLine(loopsInnermostFirst, [&](const NaturalLoop* a, const NaturalLoop* b) {
        return cfg.reversePostOrderIndex(a->header()) > cfg.reversePostOrderIndex(b->header());
    });

    Vector<BlockID> definingBlock;
    definingBlock.fill(BlockID(), function.values.size());
    for (BlockID blockID : cfg.reversePostOrder()) {
        for (ValueID id : function.at(blockID).parameters)
            definingBlock[id.index()] = blockID;
        for (ValueID id : function.at(blockID).values)
            definingBlock[id.index()] = blockID;
    }

    // Whether a loop turns once is asked before anything moves: the answer follows the uses of values.
    BitVector headersWhoseTestHoldsOnEntry;
    for (const NaturalLoop* loop : loopsInnermostFirst) {
        if (readsWhatMayChangeBehindHeader(context, *loop) && staysBehindFirstTest(context, *loop))
            headersWhoseTestHoldsOnEntry.set(loop->header().index());
    }

    std::optional<UseEffectHistory> captures;
    Vector<ListsLeftAlone> listsLeftAlone = WTF::map(loopsInnermostFirst, [&](const NaturalLoop* loop) {
        return findListsTheLoopLeavesAlone(context, *loop, captures);
    });

    unsigned numberOfHoistedReads = 0;
    for (unsigned i = 0; i < loopsInnermostFirst.size(); ++i) {
        Vector<ValueID, 4> lengthsThatLeftLoop;
        numberOfHoistedReads += hoistReadsOutOf(context, *loopsInnermostFirst[i], definingBlock, headersWhoseTestHoldsOnEntry, listsLeftAlone[i].lengths, lengthsThatLeftLoop, entriesFromBranches);
        numberOfHoistedReads += tellTestsLengthInFrontOfLoop(context, *loopsInnermostFirst[i], lengthsThatLeftLoop.span(), listsLeftAlone[i]);
    }
    if (numberOfHoistedReads)
        context.analyses.didChangeValues();
    return numberOfHoistedReads;
}

static bool mayChangeKeys(const EffectsOnEdges& effects)
{
    constexpr AbstractHeapSet heapsDescribingKeys { AbstractHeap::ShapeWords, AbstractHeap::OrderWords, AbstractHeap::NamedProperties };
    Effects any = effects.onAnyEdge();
    return any.mayRunOtherCode() || any.writes.containsAny(heapsDescribingKeys);
}

// There is one path from after the enumerator's open to the loop entry, and nothing on it changes any object's keys.
static bool leadsToLoopWithoutChangeOfKeys(FunctionPassContext& context, ValueID open, BlockID lastBlock)
{
    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    AOT_THRESHOLD(unsigned, maxBlocksBetweenOpeningAndLoop, 4);
    BlockID block = lastBlock;
    for (unsigned step = 0; step < maxBlocksBetweenOpeningAndLoop; ++step) {
        if (function.at(block).terminator.kind != TerminatorKind::Jump || mayChangeKeys(context.analyses.effectsOfTerminator(block)))
            return false;
        const BlockValues& values = function.at(block).values;
        for (size_t index = values.size(); index--;) {
            if (values[index] == open)
                return true;
            if (mayChangeKeys(context.analyses.effectsOf(values[index])))
                return false;
        }
        std::span<const BlockID> predecessors = cfg.predecessors(block);
        if (predecessors.size() != 1)
            return false;
        block = predecessors[0];
    }
    return false;
}

// Each step checks whether the enumerator still describes its object's keys. If nothing else in the loop can change an object's keys, the result only changes in a step that the runtime handles.
static unsigned markStepsOfKeyPreservingLoops(FunctionPassContext& context)
{
    Function& function = context.function;
    NaturalLoops& naturalLoops = context.analyses.naturalLoops();
    unsigned numberOfMarkedSteps = 0;
    for (unsigned i = 0; i < naturalLoops.numLoops(); ++i) {
        const NaturalLoop& loop = naturalLoops.loop(i);
        ValueID step;
        bool preservesKeys = true;
        for (unsigned j = 0; j < loop.size() && preservesKeys; ++j) {
            BlockID block = loop.at(j);
            for (ValueID id : function.at(block).values) {
                if (function.at(id).opcode == Opcode::ForInStep && !step && naturalLoops.innerMostLoopOf(block) == &loop)
                    step = id;
                else
                    preservesKeys &= !mayChangeKeys(context.analyses.effectsOf(id));
            }
            preservesKeys &= !function.at(block).isHandlerBlock() && !mayChangeKeys(context.analyses.effectsOfTerminator(block));
        }
        if (!step || !preservesKeys || function.at(step).keysChangeOnlyInSteps() || !function.at(step).positionOperand())
            continue;
        // It takes a few more bytes.
        if (!isHot(heatClassOf(context, loop.header())).predicted())
            continue;
        ValueID open = function.at(step).enumeratorOperand();
        if (function.at(open).opcode != Opcode::ForInOpen || function.at(open).objectOperand() != function.at(step).objectOperand())
            continue;
        std::optional<Edge> entry = naturalLoops.findSingleEntry(loop);
        if (!entry || entry->isHandlerEdge() || !leadsToLoopWithoutChangeOfKeys(context, open, entry->from))
            continue;
        function.edit(step).setKeysChangeOnlyInSteps(true);
        ++numberOfMarkedSteps;
    }
    return numberOfMarkedSteps;
}

namespace {

struct JoinKeptInLoop {
    BlockID block;
    ValueID join;
    Edge entry;
    // What stands in the block for the join alone, in the order of the block.
    Vector<ValueID, 2> madeForJoin;
};

} // namespace

static constexpr OpcodeSet textsOfValuesThemselves = joinsOfStrings | OpcodeSet { Opcode::ToString };

static bool makesTextWithoutRunningCode(FunctionPassContext& context, ValueID id)
{
    const Value& value = context.function.at(id);
    if (!textsOfValuesThemselves.contains(value.opcode) || !context.program.at(value.type).isAnyString())
        return false;
    Effects effects = context.analyses.effectsOf(id).onAnyEdge();
    return !effects.mayRunOtherCode() && !effects.mayBeTerminated && effects.writes.containsOnly(heapsWhoseWritesNobodySees);
}

// The outermost loop around the join that changes none of its parts, if the first iteration that joins them can hand the string on to the later ones.
static std::optional<Edge> findEntryOfLoopThatKeepsJoin(FunctionPassContext& context, const Vector<BlockID>& definingBlock, BlockID block, const Value& join)
{
    const Function& function = context.function;
    const NaturalLoops& loops = context.analyses.naturalLoops();
    AOT_THRESHOLD(unsigned, mostStepsToPartFromOutside, 6);
    auto isSameInEveryIteration = [&](auto& self, ValueID part, const NaturalLoop& loop, unsigned stepsLeft) -> bool {
        BlockID definedIn = definingBlock[part.index()];
        if (!definedIn)
            return false;
        const Value& value = function.at(part);
        if (!loops.belongsTo(definedIn, loop) || isRematerializable(value))
            return true;
        if (!stepsLeft)
            return false;
        if (isCast(value.opcode))
            return self(self, value.operand(0), loop, stepsLeft - 1);
        if (!makesTextWithoutRunningCode(context, part))
            return false;
        bool hasSameOperands = true;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            hasSameOperands = hasSameOperands && self(self, operand, loop, stepsLeft - 1);
        });
        return hasSameOperands;
    };
    auto canCarry = [&](const NaturalLoop& loop) {
        if (function.at(loop.header()).isHandlerBlock() || isClosedOnlyByWaysBack(context.analyses, loop).predicted())
            return false;
        for (unsigned i = 0; i < loop.size(); ++i) {
            if (context.analyses.effectsOfTerminator(loop.at(i)).onAnyEdge().maySuspend)
                return false;
            for (ValueID id : function.at(loop.at(i)).values) {
                if (context.analyses.effectsOf(id).onAnyEdge().maySuspend)
                    return false;
            }
        }
        return true;
    };
    std::optional<Edge> found;
    for (const NaturalLoop* loop = loops.innerMostLoopOf(block); loop; loop = loops.innerMostOuterLoop(*loop)) {
        bool hasSameParts = true;
        join.forEachOperand([&](ValueID part, uint32_t) {
            hasSameParts = hasSameParts && isSameInEveryIteration(isSameInEveryIteration, part, *loop, mostStepsToPartFromOutside);
        });
        std::optional<Edge> entry = hasSameParts && canCarry(*loop) ? loops.findSingleEntry(*loop) : std::nullopt;
        if (!entry || entry->isHandlerEdge())
            break;
        found = entry;
    }
    return found;
}

static Vector<JoinKeptInLoop, 4> findJoinsToKeepInLoops(FunctionPassContext& context)
{
    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    const NaturalLoops& loops = context.analyses.naturalLoops();
    const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
    Vector<JoinKeptInLoop, 4> joins;
    Vector<BlockID> definingBlock;
    BitVector isMadeForJoin;
    for (BlockID blockID : cfg.reversePostOrder()) {
        const NaturalLoop* loop = loops.innerMostLoopOf(blockID);
        if (!loop || isRarerThan(frequencies.classOf(blockID), frequencies.classOf(loop->header())).predicted())
            continue;
        const BlockValues& values = function.at(blockID).values;
        for (uint32_t index = values.size(); index--;) {
            ValueID id = values[index];
            if (!joinsOfStrings.contains(function.at(id).opcode) || isMadeForJoin.get(id.index()) || !makesTextWithoutRunningCode(context, id))
                continue;
            if (definingBlock.isEmpty()) {
                definingBlock.fill(BlockID(), function.values.size());
                for (BlockID definedIn : cfg.reversePostOrder()) {
                    for (ValueID parameter : function.at(definedIn).parameters)
                        definingBlock[parameter.index()] = definedIn;
                    for (ValueID value : function.at(definedIn).values)
                        definingBlock[value.index()] = definedIn;
                }
            }
            std::optional<Edge> entry = findEntryOfLoopThatKeepsJoin(context, definingBlock, blockID, function.at(id));
            if (!entry)
                continue;
            JoinKeptInLoop kept { blockID, id, *entry, { } };
            // A text that only the join reads, and that nobody misses, is made where the join is.
            auto takeAlong = [&](auto& self, ValueID user) -> void {
                function.at(user).forEachOperand([&](ValueID part, uint32_t) {
                    if (definingBlock[part.index()] != blockID || !makesTextWithoutRunningCode(context, part) || !context.analyses.isRemovableWhenUnused(part) || context.analyses.useLists().usesOf(part).size() != 1)
                        return;
                    isMadeForJoin.set(part.index());
                    kept.madeForJoin.append(part);
                    self(self, part);
                });
            };
            takeAlong(takeAlong, id);
            joins.append(WTF::move(kept));
        }
    }
    return joins;
}

// `for (const key of keys) if (key.startsWith(`${path}.`)) ..`: every iteration would join the same string and flatten it. The first one that gets there joins it, where it stands, so
// whatever the join throws it throws where it did, and a loop that never gets there joins nothing.
PassOutcome keepStringsJoinedInLoops(FunctionPassContext& context)
{
    if (!context.analyses.cfg().hasCycle())
        return PassOutcome::Unchanged;
    const Program& program = context.program;
    Function& function = context.function;
    Vector<JoinKeptInLoop, 4> joins = findJoinsToKeepInLoops(context);
    if (joins.isEmpty())
        return PassOutcome::Unchanged;

    // A test is typed from the definition it names, before SSARepair picks the one that reaches it: the parameter of a block in front of the loop has the type of all of them.
    // The blocks in front come first: a cut takes the edges of its block along.
    // Until it is joined the variable holds false: beside undefined, null or nothing at all a string may be held as the word of a literal, which a joined string has to be looked up for.
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    Vector<std::pair<Edge, BlockID>, 2> blocksInFront;
    Vector<std::pair<ValueID, ValueID>, 4> variables;
    auto heldTypeOf = [&](const JoinKeptInLoop& kept) {
        return context.types.unionOf(function.at(kept.join).type, TypeTable::boolean);
    };
    for (const JoinKeptInLoop& kept : joins) {
        builder.setOrigin(function.at(kept.join).origin());
        auto known = std::ranges::find(blocksInFront, kept.entry, &std::pair<Edge, BlockID>::first);
        if (known == blocksInFront.end()) {
            blocksInFront.append({ kept.entry, builder.splitEdge(kept.entry) });
            known = &blocksInFront.last();
        }
        ValueID nothing = builder.addDetached(IRBuilder::make(Opcode::Boolean));
        function.edit(kept.entry.from).values.append(nothing);
        context.analyses.didChangeValues();
        variables.append({ builder.addParameter(known->second, heldTypeOf(kept)), ValueID() });
        builder.appendArgument(kept.entry, nothing);
    }

    // Of the joins of a block the last one is cut out first, so each is still in the block where it was found.
    for (size_t position = 0; position < joins.size(); ++position) {
        const JoinKeptInLoop& kept = joins[position];
        Value join = function.at(kept.join);
        TypeID heldType = heldTypeOf(kept);
        BlockValues madeForJoin;
        if (!kept.madeForJoin.isEmpty()) {
            BlockValues remaining;
            for (ValueID id : function.at(kept.block).values)
                (kept.madeForJoin.contains(id) ? madeForJoin : remaining).append(id);
            function.edit(kept.block).values = WTF::move(remaining);
            context.analyses.didChangeValues();
        }

        explainOperationThatGoes(function, kept.join, InPlaceOfOperation::ItsExpansion);
        Expected<BlockFrequency> frequency = function.at(kept.block).frequency;
        IRBuilder::Expansion expansion = builder.beginExpansion({ kept.block, static_cast<uint32_t>(function.at(kept.block).values.find(kept.join)) });
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(variables[position].first, TypeTable::boolean), 0, BlockFrequency::Rare, frequency);
        if (!madeForJoin.isEmpty()) {
            function.edit(branch.taken).values.appendVector(madeForJoin);
            context.analyses.didChangeValues();
        }
        // Every later iteration reads it, so it is made in one piece, as a key is: no rope for the first reader to flatten.
        ValueID joined = builder.append(WTF::move(join));
        if (canBeMarkedAsLikelyKey(program, function, function.at(joined)))
            markAsLikelyKey(function, joined);
        builder.endExpansion(expansion, joined);
        builder.moveToEndOf(branch.notTaken);
        builder.endExpansion(expansion, branch.provenOnNotTaken);

        Value held = IRBuilder::make(Opcode::Upcast, { expansion.results.first() });
        held.setCastType(heldType);
        variables[position].second = builder.insertBefore(ProgramPoint::atHeadOf(expansion.continuation), WTF::move(held));
        builder.applyInsertions();
    }

    context.analyses.didChangeControlFlow();
    SSARepair repair(builder, context.analyses);
    for (auto [heldAtEntry, heldBehindJoin] : variables)
        repair.addDefinition(repair.addVariable(heldAtEntry), heldBehindJoin);
    repair.run();
    context.analyses.didChangeControlFlow();
    return PassOutcome::Changed;
}

PassOutcome hoistLoopInvariantReads(FunctionPassContext& context)
{
    if (!context.analyses.cfg().hasCycle())
        return PassOutcome::Unchanged;

    Vector<Edge, 2> entriesFromBranches;
    unsigned numberOfChanges = markStepsOfKeyPreservingLoops(context);
    numberOfChanges += hoistReadsOutOfLoops(context, entriesFromBranches);
    if (!entriesFromBranches.isEmpty()) {
        IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
        for (Edge entry : entriesFromBranches)
            builder.splitEdge(entry);
        numberOfChanges += entriesFromBranches.size();
        entriesFromBranches.clear();
        numberOfChanges += hoistReadsOutOfLoops(context, entriesFromBranches);
    }
    if (!numberOfChanges)
        return PassOutcome::Unchanged;

    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

static bool hasNoEffectInLoop(FunctionPassContext& context, const Block& block)
{
    return std::ranges::all_of(block.values, [&](ValueID id) {
        return context.function.at(id).opcode == Opcode::LoopHint || context.analyses.isRemovableWhenUnused(id);
    });
}

// Some whole number goes at least one step towards a bound in every turn, every turn asks whether it is there, and the bound is the same in all of them:
// it stands in front of the loop, or the loop makes it anew from what stands there. A variable has a direction only where its least step is not zero.
static bool isCountedAndEnds(FunctionPassContext& context, const NaturalLoop& loop)
{
    auto isInteger = [&](ValueID id) {
        return context.program.typeOf(context.function, id).isInteger();
    };
    auto hasBoundedInteger = [&](const LoopInduction& induction) {
        return std::ranges::any_of(induction.variables, [&](const InductionVariable& variable) {
            const InductionTest* test = variable.boundingTest();
            return test && test->boundKind != LoopBoundKind::Unknown && isInteger(variable.parameter) && isInteger(test->bound);
        });
    };
    if (hasBoundedInteger(context.analyses.inductionVariables().of(loop)))
        return true;
    // The copy of the failed tests of a loop counts on by itself and hands the loop what it has counted.
    const NaturalLoops& loops = context.analyses.naturalLoops();
    bool holdsLoop = false;
    for (unsigned index = 0; index < loop.size() && !holdsLoop; ++index)
        holdsLoop = loops.innerMostLoopOf(loop.at(index)) != &loop;
    return holdsLoop && hasBoundedInteger(findInductionThroughLoopsInside(context.analyses, loop));
}

static bool alwaysTerminates(FunctionPassContext& context, const BitVector& body)
{
    CFG& cfg = context.analyses.cfg();
    Dominators& dominators = context.analyses.dominators();
    for (size_t index : body) {
        BlockID from(index);
        for (BlockID to : cfg.successors(from)) {
            if (body.get(to.index()) && cfg.reversePostOrderIndex(to) <= cfg.reversePostOrderIndex(from) && !dominators.dominates(to, from))
                return false;
        }
    }
    NaturalLoops& loops = context.analyses.naturalLoops();
    for (unsigned i = 0; i < loops.numLoops(); ++i) {
        if (body.get(loops.loop(i).header().index()) && !isCountedAndEnds(context, loops.loop(i)))
            return false;
    }
    return true;
}

namespace {

// Whether a run goes on at one block or at another makes no difference where both hold the same operations on the same values and leave for blocks of which that is true again.
// A test gives its failing path a copy of the code in which the tested value is alive. Once nobody reads that value, nothing tells the copy from the original.
class CodeComparison {
public:
    CodeComparison(FunctionPassContext& context, BlockID first, BlockID second)
        : m_function(context.function)
        , m_analyses(context.analyses)
        , m_first(first)
        , m_second(second)
    {
    }

    bool findsSameCode()
    {
        if (!pair(m_first, m_second))
            return false;
        while (!m_pairsToCompare.isEmpty()) {
            BlockID block = m_pairsToCompare.takeLast();
            if (!holdSameCode(m_function.at(block), m_function.at(m_partnerOfBlock.get(block.index()))))
                return false;
        }
        return isReadOnlyOnItsSide(m_isOnFirstSide) && isReadOnlyOnItsSide(m_isOnSecondSide);
    }

private:
    ValueID valueInFrontOfCasts(ValueID id) const
    {
        while (id && isCastThatProvesNothing(m_function.at(id).opcode))
            id = m_function.at(id).operand(0);
        return id;
    }

    bool isSameValue(ValueID onFirstSide, ValueID onSecondSide) const
    {
        onFirstSide = valueInFrontOfCasts(onFirstSide);
        onSecondSide = valueInFrontOfCasts(onSecondSide);
        if (!onFirstSide || !onSecondSide)
            return onFirstSide == onSecondSide;
        auto partner = m_partnerOfValue.find(onFirstSide);
        return partner != m_partnerOfValue.end() ? partner->value == onSecondSide : onFirstSide == onSecondSide;
    }

    // A block that its side's first block dominates runs behind it, so whatever it reads of its side has been made since the sides parted.
    bool pair(BlockID onFirstSide, BlockID onSecondSide)
    {
        auto isOnSide = [&](BlockID block) {
            return m_isOnFirstSide.get(block.index()) || m_isOnSecondSide.get(block.index());
        };
        if (onFirstSide == onSecondSide) {
            m_isWhereSidesMeet.set(onFirstSide.index());
            return !isOnSide(onFirstSide);
        }
        if (auto partner = m_partnerOfBlock.find(onFirstSide.index()); partner != m_partnerOfBlock.end())
            return partner->value == onSecondSide;
        if (isOnSide(onFirstSide) || isOnSide(onSecondSide) || m_isWhereSidesMeet.get(onFirstSide.index()) || m_isWhereSidesMeet.get(onSecondSide.index()))
            return false;
        const Dominators& dominators = m_analyses.dominators();
        if (!dominators.dominates(m_first, onFirstSide) || !dominators.dominates(m_second, onSecondSide))
            return false;
        const CompactList<ValueID>& parameters = m_function.at(onFirstSide).parameters;
        const CompactList<ValueID>& partners = m_function.at(onSecondSide).parameters;
        if (parameters.size() != partners.size())
            return false;
        for (size_t index = 0; index < parameters.size(); ++index) {
            if (m_function.at(parameters[index]).type != m_function.at(partners[index]).type)
                return false;
            m_partnerOfValue.set(parameters[index], partners[index]);
        }
        m_partnerOfBlock.set(onFirstSide.index(), onSecondSide);
        m_isOnFirstSide.set(onFirstSide.index());
        m_isOnSecondSide.set(onSecondSide.index());
        m_pairsToCompare.append(onFirstSide);
        return true;
    }

    bool haveSameOperands(const Instruction& a, const Instruction& b) const
    {
        return a.hasSamePayloadAs(b) && a.numberOfNamedOperands() == b.numberOfNamedOperands() && std::ranges::equal(a.operands(), b.operands(), [&](ValueID inFirst, ValueID inSecond) {
            return isSameValue(inFirst, inSecond);
        });
    }

    bool leaveForSameCode(const Target& a, const Target& b)
    {
        bool passSameValues = a.proofs == b.proofs && std::ranges::equal(a.arguments, b.arguments, [&](ValueID inFirst, ValueID inSecond) {
            return isSameValue(inFirst, inSecond);
        });
        return passSameValues && pair(a.block, b.block);
    }

    bool holdSameCode(const Block& a, const Block& b)
    {
        if (a.isHandlerBlock() || b.isHandlerBlock() || a.shape != b.shape || !isSameValue(a.subject, b.subject) || !a.handler != !b.handler)
            return false;
        auto skipCasts = [&](const Block& block, size_t& position) {
            while (position < block.values.size() && isCastThatProvesNothing(m_function.at(block.values[position]).opcode))
                ++position;
        };
        for (size_t inFirst = 0, inSecond = 0;; ++inFirst, ++inSecond) {
            skipCasts(a, inFirst);
            skipCasts(b, inSecond);
            if (inFirst == a.values.size() || inSecond == b.values.size()) {
                if (inFirst != a.values.size() || inSecond != b.values.size())
                    return false;
                break;
            }
            const Value& value = m_function.at(a.values[inFirst]);
            const Value& partner = m_function.at(b.values[inSecond]);
            if (value.opcode != partner.opcode || value.type != partner.type || value.representation != partner.representation || value.origin() != partner.origin() || !haveSameOperands(value, partner))
                return false;
            m_partnerOfValue.set(a.values[inFirst], b.values[inSecond]);
        }
        const Terminator& terminator = a.terminator;
        const Terminator& partner = b.terminator;
        if (terminator.kind != partner.kind || terminator.targets.size() != partner.targets.size() || !haveSameOperands(terminator, partner))
            return false;
        if (terminator.kind != TerminatorKind::Jump && terminator.origin() != partner.origin())
            return false;
        for (size_t index = 0; index < terminator.targets.size(); ++index) {
            if (!leaveForSameCode(terminator.targets[index], partner.targets[index]))
                return false;
        }
        return !a.handler || leaveForSameCode(*a.handler, *b.handler);
    }

    // Where the sides meet again, or one comes round to the other, both find what stood there before they parted.
    bool isReadOnlyOnItsSide(const BitVector& side) const
    {
        const UseLists& useLists = m_analyses.useLists();
        auto isReadOnSide = [&](ValueID id) {
            return std::ranges::all_of(useLists.usesOf(id), [&](const Use& use) {
                return side.get(use.block.index());
            });
        };
        for (size_t index : side) {
            const Block& block = m_function.at(BlockID(index));
            if (!std::ranges::all_of(block.parameters, isReadOnSide) || !std::ranges::all_of(block.values, isReadOnSide))
                return false;
        }
        return true;
    }

    const Function& m_function;
    FunctionAnalyses& m_analyses;
    BlockID m_first;
    BlockID m_second;
    UncheckedKeyHashMap<uint32_t, BlockID, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_partnerOfBlock;
    UncheckedKeyHashMap<ValueID, ValueID> m_partnerOfValue;
    BitVector m_isOnFirstSide;
    BitVector m_isOnSecondSide;
    BitVector m_isWhereSidesMeet;
    Vector<BlockID, 16> m_pairsToCompare;
};

} // anonymous namespace

// The block that the loop leaves for, if the loop does nothing but turn and end. Where it leaves for several, all it decides is which, so they have to do the same.
static std::optional<BlockID> findExitOfDeadLoop(FunctionPassContext& context, const BitVector& body)
{
    const Function& function = context.function;
    for (size_t index : body) {
        const Block& block = function.at(BlockID(index));
        if (block.terminator.kind != TerminatorKind::Jump && block.terminator.kind != TerminatorKind::Branch)
            return std::nullopt;
        if (block.handler || block.isHandlerBlock())
            return std::nullopt;
        if (!hasNoEffectInLoop(context, block) && !context.build.isSabotaged(Sabotage::RemovedLoopsHaveEffects))
            return std::nullopt;
    }

    Vector<BlockID, 2> exits;
    for (size_t index : body) {
        for (const Target& target : function.at(BlockID(index)).terminator.targets) {
            if (body.get(target.block.index()))
                continue;
            if (!target.arguments.isEmpty() || !target.proofs.isEmpty())
                return std::nullopt;
            if (!exits.contains(target.block))
                exits.append(target.block);
        }
    }
    if (exits.isEmpty())
        return std::nullopt;

    const UseLists& useLists = context.analyses.useLists();
    auto isUsedOutside = [&](ValueID id) {
        return std::ranges::any_of(useLists.usesOf(id), [&](const Use& use) {
            return !body.get(use.block.index());
        });
    };
    for (size_t index : body) {
        const Block& block = function.at(BlockID(index));
        if (std::ranges::any_of(block.parameters, isUsedOutside) || std::ranges::any_of(block.values, isUsedOutside))
            return std::nullopt;
    }

    BlockID exit = exits.first();
    for (BlockID other : exits) {
        if (isRarerThan(function.at(exit).frequency, function.at(other).frequency).predicted())
            exit = other;
    }
    bool doTheSame = std::ranges::all_of(exits, [&](BlockID other) {
        return other == exit || CodeComparison(context, exit, other).findsSameCode();
    });
    if (!doTheSame)
        return std::nullopt;
    if (!alwaysTerminates(context, body) && !context.build.isSabotaged(Sabotage::RemovedLoopsMayNotEnd))
        return std::nullopt;
    return exit;
}

// A wider cycle may leave a turn for code that comes round to the turn again and passes the loop by. The mark of the turn is then the only one on that way.
static bool comesBackPastNoOtherMark(FunctionPassContext& context, BlockID start)
{
    const Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    auto hasLoopMark = [&](BlockID block) {
        return std::ranges::any_of(function.at(block).values, [&](ValueID id) {
            return function.at(id).opcode == Opcode::LoopHint;
        });
    };
    BitVector seen(function.blocks.size());
    Vector<BlockID, 16> worklist { start };
    while (!worklist.isEmpty()) {
        for (BlockID next : cfg.successors(worklist.takeLast())) {
            if (next == start)
                return true;
            if (seen.get(next.index()) || hasLoopMark(next))
                continue;
            seen.set(next.index());
            worklist.append(next);
        }
    }
    return false;
}

static bool removeOutermostDeadLoops(FunctionPassContext& context)
{
    const Function& function = context.function;
    bool mayHaveDeadLoop = std::ranges::any_of(function.blocks, [&](const Block& block) {
        bool hasLoopHint = std::ranges::any_of(block.values, [&](ValueID id) {
            return function.at(id).opcode == Opcode::LoopHint;
        });
        return hasLoopHint && hasNoEffectInLoop(context, block);
    });
    if (!mayHaveDeadLoop)
        return false;

    Vector<DeadLoop> found;
    NaturalLoops& loops = context.analyses.naturalLoops();
    for (unsigned i = 0; i < loops.numLoops(); ++i) {
        const NaturalLoop& loop = loops.loop(i);
        BitVector body(function.blocks.size());
        for (unsigned position = 0; position < loop.size(); ++position)
            body.quickSet(loop.at(position).index());
        if (std::optional<BlockID> exit = findExitOfDeadLoop(context, body))
            found.append(DeadLoop { loop.header(), *exit, WTF::move(body) });
    }
    if (found.isEmpty())
        return false;
    AOT::sort(found, [](const DeadLoop& a, const DeadLoop& b) {
        size_t aSize = a.body.bitCount();
        size_t bSize = b.body.bitCount();
        return aSize != bSize ? aSize > bSize : a.header.index() < b.header.index();
    });

    Vector<std::pair<Edge, BlockID>> entriesAndExits;
    BitVector covered(function.blocks.size());
    for (const DeadLoop& loop : found) {
        if (covered.get(loop.header.index()))
            continue;
        covered.merge(loop.body);
        for (Edge edge : context.analyses.cfg().incomingEdges(loop.header)) {
            if (!loop.body.get(edge.from.index()))
                entriesAndExits.append({ edge, loop.exit });
        }
    }

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    for (auto [entry, exit] : entriesAndExits)
        builder.redirectEdge(entry, Target { exit, { }, { } });
    builder.emptyUnreachableBlocks();
    // A loop around the removed one may have no mark of its own: every iteration of it went through the header that is gone.
    for (auto [entry, exit] : entriesAndExits) {
        bool hasMark = std::ranges::any_of(function.at(exit).values, [&](ValueID id) { return function.at(id).opcode == Opcode::LoopHint; });
        if (hasMark || !comesBackPastNoOtherMark(context, exit))
            continue;
        builder.setOrigin(function.at(exit).terminator.origin());
        builder.insertBefore(ProgramPoint::atHeadOf(exit), IRBuilder::make(Opcode::LoopHint));
        builder.applyInsertions();
    }
    return true;
}

static bool eraseLoopHints(IRBuilder& builder, const Function& function, BlockID block)
{
    BitVector hints;
    std::span<const ValueID> values = function.at(block).values.span();
    for (size_t index = 0; index < values.size(); ++index) {
        if (function.at(values[index]).opcode == Opcode::LoopHint && !function.at(values[index]).throwsOwedError())
            hints.set(index);
    }
    if (hints.isEmpty())
        return false;
    builder.eraseValues(block, hints);
    return true;
}

// What is left of a loop whose way back has been folded away turns no more.
static bool removeHintsOutsideCycles(FunctionPassContext& context)
{
    Vector<BlockID, 8> outsideCycles;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        if (!context.analyses.isInCycle(block))
            outsideCycles.append(block);
    }
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    bool changed = false;
    for (BlockID block : outsideCycles)
        changed |= eraseLoopHints(builder, context.function, block);
    return changed;
}

// Every path around a loop goes through its header. If the header polls, another poll in the same iteration adds nothing.
static bool removeDuplicateIterationHints(FunctionPassContext& context)
{
    const Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    if (!analyses.cfg().hasCycle())
        return false;
    auto isHintOfLoop = [&](ValueID id) {
        return function.at(id).opcode == Opcode::LoopHint;
    };
    const NaturalLoops& loops = analyses.naturalLoops();
    const BlockCycles& cycles = analyses.blockCycles();
    Vector<std::pair<BlockID, BitVector>, 2> duplicateHints;
    for (unsigned loopIndex = 0; loopIndex < loops.numLoops(); ++loopIndex) {
        const NaturalLoop& loop = loops.loop(loopIndex);
        BlockID header = loop.header();
        std::optional<unsigned> cycle = cycles.innermostCycleOf(header);
        size_t hintOfHeader = function.at(header).values.findIf(isHintOfLoop);
        if (hintOfHeader == notFound || !cycle || !cycles.forest()[*cycle].isReducible())
            continue;
        for (unsigned index = 0; index < loop.size(); ++index) {
            BlockID block = loop.at(index);
            if (cycles.innermostCycleOf(block) != cycle || function.at(block).handler != function.at(header).handler)
                continue;
            BitVector hints;
            std::span<const ValueID> values = function.at(block).values.span();
            for (size_t position = block == header ? hintOfHeader + 1 : 0; position < values.size(); ++position) {
                if (isHintOfLoop(values[position]) && !function.at(values[position]).replacesTailCalls())
                    hints.set(position);
            }
            if (!hints.isEmpty())
                duplicateHints.append({ block, WTF::move(hints) });
        }
    }
    IRBuilder builder(analyses.context(), context.function, context.types, &analyses);
    for (auto& [block, hints] : duplicateHints)
        builder.eraseValues(block, hints);
    return !duplicateHints.isEmpty();
}

PassOutcome removeDeadCountedLoops(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    while (removeOutermostDeadLoops(context))
        outcome = PassOutcome::Changed;
    if (removeHintsOutsideCycles(context))
        outcome = PassOutcome::Changed;
    if (removeDuplicateIterationHints(context))
        outcome = PassOutcome::Changed;
    // The builder makes every edit and tells the analyses, and the search that found no more loops came behind the last one.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

// UnrollShortLoops.

// How often the body runs, where the counter starts at a constant, steps by a constant and is tested against a constant.
static std::optional<uint64_t> findNumberOfTurns(FunctionPassContext& context, const NaturalLoop& loop)
{
    const InductionVariable* counter = context.analyses.inductionVariables().of(loop).counter();
    if (!counter || counter->step.kind != InductionStepKind::Constant || !counter->step.constant)
        return std::nullopt;
    const InductionTest* test = counter->controllingTest();
    if (!test || test->tested != TestedValue::BeforeStep || test->block != loop.header() || !isBoundingRelation(test->relation, counter->direction) || !counter->start || !test->bound)
        return std::nullopt;
    std::optional<int64_t> start = integerConstantThroughCasts(context.program, context.function, counter->start);
    std::optional<int64_t> bound = integerConstantThroughCasts(context.program, context.function, test->bound);
    constexpr int64_t farthest = 1 << 30;
    if (!start || !bound || std::abs(*start) > farthest || std::abs(*bound) > farthest)
        return std::nullopt;

    int64_t distance = counter->direction == InductionDirection::Increasing ? *bound - *start : *start - *bound;
    if (test->relation == Opcode::LessEq || test->relation == Opcode::GreaterEq)
        ++distance;
    uint64_t step = static_cast<uint64_t>(std::abs(counter->step.constant));
    return distance <= 0 ? 0 : (static_cast<uint64_t>(distance) + step - 1) / step;
}

namespace {

struct ShortLoop {
    BlockID header;
    uint64_t numberOfTurns { 0 };
    bool exitsAfterLastTurn { false };
    Vector<BlockID, 8> region;
    Vector<Edge, 2> entries;
    Vector<Edge, 2> backEdges;
};

} // anonymous namespace

// The header takes the counter as a parameter and ends by comparing it with a constant.
static bool mayBeHeaderOfShortLoop(const Program& program, const Function& function, const Block& block)
{
    if (block.parameters.isEmpty() || !block.terminator.isConditionalBranch())
        return false;
    const Value& comparison = function.at(block.terminator.conditionOperand());
    return relationalOperators.contains(comparison.opcode) && (integerConstantThroughCasts(program, function, comparison.operand(0)) || integerConstantThroughCasts(program, function, comparison.operand(1)));
}

// An innermost loop that is small enough to stand in the function once more for each of the turns.
static std::optional<ShortLoop> planTurnsInFrontOf(FunctionPassContext& context, const NaturalLoop& loop, uint64_t numberOfTurns)
{
    const Function& function = context.function;
    NaturalLoops& loops = context.analyses.naturalLoops();
    ShortLoop found { loop.header(), numberOfTurns, false, { }, { }, { } };
    uint64_t bytes = 0;
    for (unsigned index = 0; index < loop.size(); ++index) {
        if (loops.innerMostLoopOf(loop.at(index)) != &loop)
            return std::nullopt;
        found.region.append(loop.at(index));
        bytes += estimatedCodeBytes(context.program, function, function.at(loop.at(index)));
    }
    if (bytes * numberOfTurns > context.build.limit(LimitID::MaxBytesOfUnrolledLoop))
        return std::nullopt;

    for (Edge edge : context.analyses.cfg().incomingEdges(loop.header())) {
        if (edge.isHandlerEdge() || hasEdgeResults(function, edge))
            return std::nullopt;
        (loops.belongsTo(edge.from, loop) ? found.backEdges : found.entries).append(edge);
    }
    if (RegionCloner::checkCanDuplicate(context.analyses, found.region.span()) != CloneCheck::CanClone)
        return std::nullopt;
    return found;
}

static std::optional<ShortLoop> findShortLoop(FunctionPassContext& context, const NaturalLoop& loop)
{
    if (!mayBeHeaderOfShortLoop(context.program, context.function, context.function.at(loop.header())) || !isExpectedToRun(context, loop).predicted())
        return std::nullopt;
    std::optional<uint64_t> numberOfTurns = findNumberOfTurns(context, loop);
    if (!numberOfTurns || !*numberOfTurns || *numberOfTurns > context.build.limit(LimitID::MaxTurnsOfUnrolledLoop))
        return std::nullopt;
    std::optional<ShortLoop> found = planTurnsInFrontOf(context, loop, *numberOfTurns);
    if (found)
        found->exitsAfterLastTurn = true;
    return found;
}

// The turns stand one behind the other, each a copy of the loop that goes on to the next one where the loop went back to its header. The loop itself stands behind the last.
static void unroll(FunctionPassContext& context, std::span<const ShortLoop> loops)
{
    Function& function = context.function;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    using Turns = Vector<std::unique_ptr<RegionCloner>, 8>;
    Vector<Turns> turnsByLoop(loops.size());
    HashMap<BlockID, size_t> loopContaining;
    for (size_t index = 0; index < loops.size(); ++index) {
        for (uint64_t turn = 0; turn < loops[index].numberOfTurns; ++turn) {
            turnsByLoop[index].append(makeUniqueWithoutFastMallocCheck<RegionCloner>(builder, function));
            turnsByLoop[index].last()->cloneBlocks(loops[index].region.span(), [](ValueID, Value&) { });
        }
        for (BlockID block : loops[index].region)
            loopContaining.add(block, index);
    }

    for (size_t index = 0; index < loops.size(); ++index) {
        const ShortLoop& loop = loops[index];
        const Turns& turns = turnsByLoop[index];
        auto enterAtFirstTurn = [&](Edge entry) {
            builder.redirectEdge(entry, turns.first()->cloneOf(loop.header), Arguments(targetOf(function, entry).arguments));
        };
        for (Edge entry : loop.entries) {
            enterAtFirstTurn(entry);
            // An exit edge of another unrolled loop: its clones enter here too.
            if (auto exited = loopContaining.find(entry.from); exited != loopContaining.end()) {
                for (const std::unique_ptr<RegionCloner>& turn : turnsByLoop[exited->value])
                    enterAtFirstTurn({ turn->cloneOf(entry.from), entry.targetIndex });
            }
        }
        for (size_t turn = 0; turn < turns.size(); ++turn) {
            BlockID next = turn + 1 < turns.size() ? turns[turn + 1]->cloneOf(loop.header) : loop.header;
            for (Edge backEdge : loop.backEdges) {
                Edge leaving { turns[turn]->cloneOf(backEdge.from), backEdge.targetIndex };
                builder.redirectEdge(leaving, next, Arguments(targetOf(function, leaving).arguments));
            }
        }
        if (loop.exitsAfterLastTurn) {
            builder.setOrigin(function.at(loop.header).terminator.origin());
            BlockID neverReached = builder.addBlock(BlockFrequency::Rare);
            builder.moveToEndOf(neverReached);
            builder.emitUnreachable();
            for (Edge backEdge : loop.backEdges)
                builder.redirectEdge(backEdge, neverReached, { });
        }
    }
    context.analyses.didChangeControlFlow();
    {
        SSARepair repair(builder, context.analyses);
        for (const Turns& turns : turnsByLoop)
            RegionCloner::addVariablesTo(repair, context.analyses, turns.span());
        repair.run();
    }
    context.analyses.didChangeControlFlow();
    for (size_t index = 0; index < loops.size(); ++index) {
        for (const std::unique_ptr<RegionCloner>& turn : turnsByLoop[index]) {
            if (!comesBackPastNoOtherMark(context, turn->cloneOf(loops[index].header)))
                eraseLoopHints(builder, function, turn->cloneOf(loops[index].header));
        }
        if (loops[index].exitsAfterLastTurn && !comesBackPastNoOtherMark(context, loops[index].header))
            eraseLoopHints(builder, function, loops[index].header);
    }
}

// What every turn runs on its way back to the header.
static Vector<ValueID, 4> findInitializationsOfEveryTurn(FunctionPassContext& context, const ShortLoop& loop)
{
    const Function& function = context.function;
    const Dominators& dominators = context.analyses.dominators();
    Vector<ValueID, 4> found;
    for (BlockID block : loop.region) {
        bool runsInEveryTurn = std::ranges::all_of(loop.backEdges, [&](Edge backEdge) {
            return backEdge.from == block || dominators.terminatorDominates(block, backEdge.from);
        });
        if (!runsInEveryTurn)
            continue;
        for (ValueID id : function.at(block).values) {
            if (function.at(id).opcode == Opcode::InitializeBuiltin)
                found.append(id);
        }
    }
    return found;
}

// Behind an operation that runs other code, or that writes outside the program as InitializeBuiltin does, whatever the loop keeps at hand is read anew in any case.
static bool keepsNothingAtHandAnyway(FunctionPassContext& context, const ShortLoop& loop)
{
    const Function& function = context.function;
    static_assert(declaredBoundOf(Opcode::InitializeBuiltin).writes.contains(AbstractHeap::ExternalState));
    auto reachesAsFar = [&](const Effects& effects) {
        return effects.mayRunOtherCode() || effects.writes.contains(AbstractHeap::ExternalState);
    };
    return std::ranges::any_of(loop.region, [&](BlockID block) {
        if (context.analyses.blockFrequencies().classOf(block).predicted() == BlockFrequency::Rare)
            return false;
        return reachesAsFar(context.analyses.effectsOfTerminator(block).onAnyEdge()) || std::ranges::any_of(function.at(block).values, [&](ValueID id) {
            return function.at(id).opcode != Opcode::InitializeBuiltin && reachesAsFar(context.analyses.effectsOf(id).onAnyEdge());
        });
    });
}

// Only the first InitializeBuiltin of a member does anything. Where every turn of a hot loop runs one, the first turn stands in front of the loop and keeps it, and the loop, which is entered from the end of that turn alone, has none.
static PassOutcome takeFirstTurnsThatInitializeOutOfLoops(FunctionPassContext& context)
{
    if (!context.analyses.opcodesOfValues().contains(Opcode::InitializeBuiltin))
        return PassOutcome::Unchanged;

    Function& function = context.function;
    auto initializes = [&](const NaturalLoop& loop) {
        for (unsigned index = 0; index < loop.size(); ++index) {
            for (ValueID id : function.at(loop.at(index)).values) {
                if (function.at(id).opcode == Opcode::InitializeBuiltin)
                    return true;
            }
        }
        return false;
    };
    Vector<ShortLoop> found;
    Vector<Vector<ValueID, 4>> initializationsByLoop;
    NaturalLoops& loops = context.analyses.naturalLoops();
    for (unsigned index = 0; index < loops.numLoops(); ++index) {
        const NaturalLoop& loop = loops.loop(index);
        if (!initializes(loop) || !isExpectedToRun(context, loop).predicted())
            continue;
        std::optional<ShortLoop> plan = planTurnsInFrontOf(context, loop, 1);
        if (!plan || plan->entries.isEmpty() || keepsNothingAtHandAnyway(context, *plan))
            continue;
        Vector<ValueID, 4> initializations = findInitializationsOfEveryTurn(context, *plan);
        if (initializations.isEmpty())
            continue;
        found.append(WTF::move(*plan));
        initializationsByLoop.append(WTF::move(initializations));
    }
    if (found.isEmpty())
        return PassOutcome::Unchanged;
    unroll(context, found.span());

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (size_t loop = 0; loop < found.size(); ++loop) {
        for (BlockID block : found[loop].region) {
            const BlockValues& values = function.at(block).values;
            BitVector erasedIndexes;
            for (size_t index = 0; index < values.size(); ++index) {
                if (initializationsByLoop[loop].contains(values[index]))
                    erasedIndexes.set(index);
            }
            if (!erasedIndexes.isEmpty())
                builder.eraseValues(block, erasedIndexes);
        }
    }
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

namespace {

struct LoopThatCallsOneValue {
    ShortLoop loop;
    ValueID callee;
    Vector<FunctionID, 4> functions;
    bool everyListedFunctionHasCopy { true };
};

} // anonymous namespace

// The functions that the type of the value lists, where the loop calls a value that it was handed. Those that a test in front of the loop has ruled out are not asked for again.
static std::optional<LoopThatCallsOneValue> findLoopThatCallsOneValue(FunctionPassContext& context, const NaturalLoop& loop)
{
    const Program& program = context.program;
    const Function& function = context.function;
    NaturalLoops& loops = context.analyses.naturalLoops();
    if (function.at(loop.header()).frequency.predicted() != BlockFrequency::Normal || function.runsOnlyBehindFailedTest())
        return std::nullopt;
    // A cold function of a very large program gets its type tests behind the rounds, and nothing is put in line there. The copies would call as the loop did, and the one bound that they all read
    // would get no version for an int32: each turn of each copy would compare a counter of any size with a number.
    if (context.inputs.heat.isOfVeryLargeProgram() && isCold(context.inputs.heat.of(program, context.id)).predicted())
        return std::nullopt;
    for (unsigned index = 0; index < loop.size(); ++index) {
        const Block& block = function.at(loop.at(index));
        if (block.frequency.predicted() != BlockFrequency::Normal)
            continue;
        for (ValueID id : block.values) {
            const Value& call = function.at(id);
            if (call.opcode != Opcode::Call || call.isTailCall)
                continue;
            CalleeCandidates candidates = calleeCandidates(program, function, call.calleeOperand());
            if (!candidates.isProven() || candidates.only() || loops.belongsTo(context.analyses.useLists().blockOf(candidates.value), loop))
                continue;
            if (!isExpectedToRun(context, loop).predicted())
                return std::nullopt;
            // A copy pays where the function stands in line in it: what InlineCalls takes at a call as hot as this one.
            uint64_t mostBytesInLine = context.build.limit(isHot(heatClassOf(context, loop.at(index))).predicted() ? LimitID::InlineBudgetHotBytes : LimitID::InlineBudgetWarmBytes);
            LoopThatCallsOneValue found { { }, candidates.value, { }, true };
            unsigned numberOfListedFunctions = 0;
            uint64_t bytesInLine = 0;
            for (FunctionID listed : candidates.listed().predicted()) {
                // A function without a body is one that nothing enters.
                if (!program.at(listed).hasBody())
                    continue;
                if (context.analyses.findEarlierTest(makeTypeTest(candidates.value, context.types.intern(Type::forFunction(listed))), loop.header()) == EarlierTest::Failed)
                    continue;
                ++numberOfListedFunctions;
                const InliningSummary& summary = context.inputs.inlining.of(program, listed);
                if (mayBeCalledDirectlyWhereCalleeIsKept(program, listed) && summary.canInlineWholeBody() && summary.codeBytes <= mostBytesInLine) {
                    found.functions.append(listed);
                    bytesInLine += summary.codeBytes;
                } else
                    found.everyListedFunctionHasCopy = false;
            }
            // One function gets its test from ResolveCalls or PlaceTypeTests, where the call stands.
            if (found.functions.isEmpty() || numberOfListedFunctions < 2 || found.functions.size() > context.build.limit(LimitID::MaxTurnsOfUnrolledLoop) || bytesInLine > context.build.limit(LimitID::MaxBytesOfUnrolledLoop))
                continue;
            std::optional<ShortLoop> plan = planTurnsInFrontOf(context, loop, found.functions.size());
            if (!plan || plan->entries.size() != 1)
                return std::nullopt;
            found.loop = WTF::move(*plan);
            return found;
        }
    }
    return std::nullopt;
}

// The value, and the casts of it in front of the loop that say no more of it: the loop may use any of them.
static Vector<ValueID, 2> findLinksInFrontOf(FunctionPassContext& context, const ShortLoop& loop, ValueID value)
{
    const Function& function = context.function;
    const Type& type = context.program.typeOf(function, value);
    Vector<ValueID, 2> links { value };
    for (size_t index = 0; index < links.size(); ++index) {
        ValueID link = links[index];
        for (const Use& use : context.analyses.useLists().usesOf(link)) {
            if (use.kind != Use::Kind::Operand || use.operandIndex || loop.region.contains(use.block) || !type.isSubsetOf(context.program.typeOf(function, use.user), context.program))
                continue;
            for (ValueID below : CastChain(function, use.user)) {
                if (below == link)
                    links.append(use.user);
            }
        }
    }
    return links;
}

// `for (const item of items) total += priceOf(item)` with `priceOf` handed in: every turn calls the same function, and the type of the value lists the few that it may be.
// A call of a value boxes what it passes and what it returns, lays a rest list into the frame, and the loop tests what comes back, in every turn. So the loop asks once, in front of its first turn,
// which function it was handed, and stands in the function once more for each of them. In its copy the function is known: it is called directly or stands in line.
// Whatever else the value is goes through the loop as it was.
static PassOutcome copyLoopsForFunctionsTheyCall(FunctionPassContext& context)
{
    if (!context.analyses.cfg().hasCycle() || !context.analyses.opcodesOfValues().contains(Opcode::Call))
        return PassOutcome::Unchanged;

    Function& function = context.function;
    PassOutcome outcome = PassOutcome::Unchanged;
    // A copy renumbers nothing, yet it changes the loops: each search asks the function as it stands.
    for (;;) {
        std::optional<LoopThatCallsOneValue> found;
        NaturalLoops& loops = context.analyses.naturalLoops();
        for (unsigned index = 0; index < loops.numLoops() && !found; ++index)
            found = findLoopThatCallsOneValue(context, loops.loop(index));
        if (!found)
            return outcome;

        Vector<ValueID, 2> links = findLinksInFrontOf(context, found->loop, found->callee);
        IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
        Edge entry = found->loop.entries[0];
        builder.setOrigin(function.at(entry.from).terminator.origin());
        BlockID front = builder.splitEdge(entry);
        Expected<BlockFrequency> frequency = function.at(front).frequency;
        Arguments arguments(function.at(front).terminator.next().arguments);
        function.edit(front, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
        builder.moveToEndOf(front);
        Vector<std::unique_ptr<RegionCloner>, 8> copies;
        ValueID whatIsLeft = found->callee;
        for (FunctionID callee : found->functions) {
            ValueID test = builder.appendTypeTest(whatIsLeft, Type::forFunction(callee));
            // Behind the last test the value is none of the functions that the program was seen to put there.
            bool leavesNoListedFunction = callee == found->functions.last() && found->everyListedFunctionHasCopy;
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, frequency, leavesNoListedFunction ? rarerOf(frequency, BlockFrequency::Unlikely) : frequency);
            copies.append(makeUniqueWithoutFastMallocCheck<RegionCloner>(builder, function));
            for (ValueID link : links)
                copies.last()->mapValue(link, branch.provenOnTaken);
            copies.last()->cloneBlocks(found->loop.region.span(), [](ValueID, Value&) { });
            builder.moveToEndOf(branch.taken);
            builder.emitJump(copies.last()->cloneOf(found->loop.header), Arguments(arguments));
            builder.moveToEndOf(branch.notTaken);
            whatIsLeft = branch.provenOnNotTaken;
        }
        builder.emitJump(found->loop.header, WTF::move(arguments));
        context.analyses.didChangeControlFlow();
        RegionCloner::repairSSA(context.analyses, copies.span());
        context.analyses.didChangeControlFlow();
        // The loop as it was is entered behind the last test alone: in it the value is what the tests have left.
        Vector<Use, 4> usesInLoop;
        for (ValueID link : links) {
            for (const Use& use : context.analyses.useLists().usesOf(link)) {
                if (found->loop.region.contains(use.block))
                    usesInLoop.append(use);
            }
        }
        for (const Use& use : usesInLoop)
            replaceUse(function, use, whatIsLeft);
        context.analyses.didChangeValues();
        if (found->everyListedFunctionHasCopy) {
            for (BlockID block : found->loop.region)
                function.edit(block, BlockEdit::Rest).frequency = rarerOf(function.at(block).frequency, BlockFrequency::Unlikely);
            context.analyses.didChangeControlFlow();
        }
        outcome = PassOutcome::Changed;
    }
}

PassOutcome unrollShortLoops(FunctionPassContext& context)
{
    PassOutcome outcome = takeFirstTurnsThatInitializeOutOfLoops(context);
    if (copyLoopsForFunctionsTheyCall(context) == PassOutcome::Changed)
        outcome = PassOutcome::Changed;
    const CFG& cfg = context.analyses.cfg();
    bool mayHaveShortLoop = std::ranges::any_of(cfg.reversePostOrder(), [&](BlockID block) {
        return mayBeHeaderOfShortLoop(context.program, context.function, context.function.at(block)) && std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) {
            return cfg.isReachable(edge.from) && cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(block);
        });
    });
    if (!mayHaveShortLoop)
        return outcome;
    Vector<ShortLoop> found;
    NaturalLoops& loops = context.analyses.naturalLoops();
    for (unsigned index = 0; index < loops.numLoops(); ++index) {
        if (std::optional<ShortLoop> loop = findShortLoop(context, loops.loop(index)))
            found.append(WTF::move(*loop));
    }
    if (found.isEmpty())
        return outcome;
    unroll(context, found.span());
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
