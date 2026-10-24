/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTInductionVariables.h"
#include "AOTNaturalLoops.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CounterCandidate {
    ValueID parameter;
    ValueID comparison;
    uint32_t testedOperandIndex;
    Edge stayingEdge;
    Vector<ValueID, 1> stepsEqualToTested { };
};

using Uses = Vector<Use, 8>;

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

static std::optional<uint32_t> findTestedOperand(FunctionAnalyses& analyses, const InductionTest& test)
{
    const Value& comparison = analyses.function().at(test.comparison);
    ValueID bound = analyses.identityOf(test.bound);
    bool isLeftBound = analyses.identityOf(comparison.operand(0)) == bound;
    bool isRightBound = analyses.identityOf(comparison.operand(1)) == bound;
    if (isLeftBound == isRightBound)
        return std::nullopt;
    return isLeftBound ? 1 : 0;
}

// A staying edge into a shared block is split before it is used.
static bool dominatesEveryStep(FunctionAnalyses& analyses, Edge edge, const InductionVariable& variable)
{
    const UseLists& useLists = analyses.useLists();
    return std::ranges::all_of(variable.steppedValues, [&](ValueID stepped) {
        return dominatesAfterSplit(analyses, edge, useLists.blockOf(stepped));
    });
}

static std::optional<uint32_t> findBoundedOperand(FunctionPassContext& context, const InductionVariable& variable, const InductionTest& test)
{
    if (test.tested != TestedValue::BeforeStep || !isBoundingRelation(test.relation, variable.direction))
        return std::nullopt;
    if (!context.program.typeOf(context.function, test.bound).isInteger())
        return std::nullopt;
    if (!dominatesEveryStep(context.analyses, test.stayingEdge, variable))
        return std::nullopt;
    return findTestedOperand(context.analyses, test);
}

static bool hasStepWithinLimit(FunctionPassContext& context, const InductionVariable& variable, BitVector& parametersOverStepLimit)
{
    if (variable.step.kind != InductionStepKind::Constant || !variable.step.constant)
        return false;
    if (parametersOverStepLimit.get(variable.parameter.index()))
        return false;

    int64_t step = variable.step.constant;
    uint64_t magnitude = step < 0 ? 0 - static_cast<uint64_t>(step) : static_cast<uint64_t>(step);
    if (!context.exceeds(LimitID::MaxCounterStep, magnitude, variable.parameter))
        return true;
    parametersOverStepLimit.set(variable.parameter.index());
    return false;
}

// What another loop hands over, in a join in front of this one or as it has counted it up, is an integer if that loop's counter becomes one.
static bool mayBecomeInteger(FunctionPassContext& context, ValueID start)
{
    const Type& type = context.program.typeOf(context.function, start);
    if (type.isInteger())
        return true;
    ValueLocation location = context.analyses.useLists().locationOf(context.analyses.identityOf(start));
    return type.isNumberWithoutFraction() && (location.placement != ValuePlacement::Parameter || location.block != entryBlock);
}

static std::optional<CounterCandidate> findCandidate(FunctionPassContext& context, const InductionVariable& variable, BitVector& parametersOverStepLimit)
{
    const Function& function = context.function;
    if (!isNumberBeyondInt32(context.program.typeOf(function, variable.parameter)))
        return std::nullopt;
    if (!variable.start || !mayBecomeInteger(context, variable.start))
        return std::nullopt;
    if (!hasStepWithinLimit(context, variable, parametersOverStepLimit))
        return std::nullopt;

    for (const InductionTest& test : variable.tests) {
        std::optional<uint32_t> testedOperandIndex = findBoundedOperand(context, variable, test);
        if (!testedOperandIndex)
            continue;
        if (targetOf(function, test.stayingEdge).proofs.contains(*testedOperandIndex))
            return std::nullopt;
        return CounterCandidate { variable.parameter, test.comparison, *testedOperandIndex, test.stayingEdge };
    }
    return std::nullopt;
}

static bool isParameterPlusStep(FunctionPassContext& context, const InductionVariable& variable, ValueID id)
{
    const Value& sum = context.function.at(context.analyses.identityOf(id));
    if (sum.opcode != Opcode::Add)
        return false;
    for (uint32_t side = 0; side < 2; ++side) {
        if (context.analyses.identityOf(sum.operand(side)) != variable.parameter)
            continue;
        if (integerConstantThroughCasts(context.program, context.function, sum.operand(1 - side)) == variable.step.constant)
            return true;
    }
    return false;
}

static std::optional<CounterCandidate> findCandidateTestedAheadOfStep(FunctionPassContext& context, const LoopInduction& induction, const InductionVariable& variable, BitVector& parametersOverStepLimit)
{
    constexpr uint32_t takenTargetIndex = 0;
    const Function& function = context.function;
    if (!isNumberBeyondInt32(context.program.typeOf(function, variable.parameter)))
        return std::nullopt;
    if (!variable.start || !mayBecomeInteger(context, variable.start))
        return std::nullopt;
    if (!hasStepWithinLimit(context, variable, parametersOverStepLimit))
        return std::nullopt;
    bool everyStepIsTheSum = std::ranges::all_of(variable.steppedValues, [&](ValueID stepped) {
        return isParameterPlusStep(context, variable, stepped);
    });
    if (!everyStepIsTheSum)
        return std::nullopt;

    const NaturalLoops& loops = context.analyses.naturalLoops();
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
            if (variable.steppedValues.contains(context.analyses.identityOf(tested)) || !isParameterPlusStep(context, variable, tested))
                continue;
            Opcode relation = side ? mirroredComparison(comparison.opcode) : comparison.opcode;
            if (!isBoundingRelation(relation, variable.direction) || !context.program.typeOf(function, comparison.operand(1 - side)).isInteger())
                continue;
            Edge stayingEdge { block, takenTargetIndex };
            if (!dominatesEveryStep(context.analyses, stayingEdge, variable) || terminator.taken().proofs.contains(side))
                continue;
            return CounterCandidate { variable.parameter, terminator.conditionOperand(), side, stayingEdge, variable.steppedValues };
        }
    }
    return std::nullopt;
}

static constexpr OpcodeSet orderComparisons { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };

// A loop and the copy of its failed tests, which has a loop of its own, hand each other the counter: the way back gives the typed loop what the copy has counted up, and a failed test gives the copy what the typed loop has.
// Where the copy comes back in the middle of the body the step adds to a join, and the loop has no induction variable though its cycle has one entry.
// `while (b)`: that b is neither zero nor NaN says nothing of a number without a fraction, so nobody kept it, and it is all that a remainder by b needs to be an integer.
static void appendComparedParametersOfCycles(FunctionPassContext& context, Vector<CounterCandidate, 4>& candidates)
{
    constexpr uint32_t takenTargetIndex = 0;
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    const BlockCycles& cycles = context.analyses.blockCycles();
    // What goes up or down by a constant has been judged as a counter.
    BitVector isJudgedAsCounter;
    for (const LoopInduction& loop : context.analyses.inductionVariables().loops()) {
        for (const InductionVariable& variable : loop.variables) {
            if (variable.step.kind == InductionStepKind::Constant)
                isJudgedAsCounter.set(variable.parameter.index());
        }
    }
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            continue;
        std::optional<unsigned> cycle = cycles.innermostCycleOf(block);
        if (!cycle)
            continue;
        const Value& comparison = function.at(terminator.conditionOperand());
        bool testsForTruth = comparison.opcode == Opcode::ToBoolean;
        if (!testsForTruth && !orderComparisons.contains(comparison.opcode))
            continue;
        for (uint32_t operandIndex = 0; operandIndex < comparison.numberOfOperands(); ++operandIndex) {
            const Type& tested = context.program.typeOf(function, comparison.operand(operandIndex));
            if (!tested.isNumberWithoutFraction() || tested.isInt32() || (!testsForTruth && !context.program.typeOf(function, comparison.operand(1 - operandIndex)).isInteger()))
                continue;
            ValueID parameter = context.analyses.identityOf(comparison.operand(operandIndex));
            ValueLocation location = useLists.locationOf(parameter);
            if (location.placement != ValuePlacement::Parameter || location.block == entryBlock || terminator.taken().proofs.contains(operandIndex))
                continue;
            if (!testsForTruth && cycles.forest()[*cycle].isReducible() && isJudgedAsCounter.get(parameter.index()))
                continue;
            Edge taken { block, takenTargetIndex };
            bool isListed = std::ranges::any_of(candidates, [&](const CounterCandidate& candidate) {
                return candidate.stayingEdge == taken;
            });
            if (!isListed)
                candidates.append(CounterCandidate { parameter, terminator.conditionOperand(), operandIndex, taken });
        }
    }
}

// What has failed the test for an integer is none. Where the loop by itself keeps its counter an integer nothing is left of it on that edge, so what the copy behind the edge counts up comes back to nobody.
static void appendParametersThatFailedTestForInteger(FunctionPassContext& context, Vector<CounterCandidate, 4>& candidates)
{
    constexpr uint32_t notTakenTargetIndex = 1;
    const Function& function = context.function;
    const BlockCycles& cycles = context.analyses.blockCycles();
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        const Terminator& terminator = function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || !cycles.innermostCycleOf(block))
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        if (test.opcode != Opcode::TypeTest || !context.types.at(test.testedType()).isInteger() || terminator.notTaken().proofs.contains(OperandIndex::TypeTest::value))
            continue;
        const Type& tested = context.program.typeOf(function, test.valueOperand());
        ValueID parameter = context.analyses.identityOf(test.valueOperand());
        if (tested.isNumberWithoutFraction() && !tested.isInteger() && context.analyses.useLists().locationOf(parameter).placement == ValuePlacement::Parameter)
            candidates.append(CounterCandidate { parameter, terminator.conditionOperand(), OperandIndex::TypeTest::value, Edge { block, notTakenTargetIndex } });
    }
}

static Vector<CounterCandidate, 4> findCandidates(FunctionPassContext& context, BitVector& parametersOverStepLimit)
{
    Vector<CounterCandidate, 4> candidates;
    for (const LoopInduction& loop : context.analyses.inductionVariables().loops()) {
        for (const InductionVariable& variable : loop.variables) {
            if (std::optional<CounterCandidate> candidate = findCandidate(context, variable, parametersOverStepLimit))
                candidates.append(*candidate);
            else if (std::optional<CounterCandidate> candidate = findCandidateTestedAheadOfStep(context, loop, variable, parametersOverStepLimit))
                candidates.append(*candidate);
        }
    }
    appendComparedParametersOfCycles(context, candidates);
    appendParametersThatFailedTestForInteger(context, candidates);
    return candidates;
}

static void splitSharedStayingEdges(FunctionPassContext& context, IRBuilder& builder, std::span<const CounterCandidate> candidates)
{
    Vector<Edge, 2> sharedEdges;
    for (const CounterCandidate& candidate : candidates) {
        bool isShared = !context.analyses.hasSingleIncomingEdge(targetOf(context.function, candidate.stayingEdge).block);
        if (isShared && !sharedEdges.contains(candidate.stayingEdge))
            sharedEdges.append(candidate.stayingEdge);
    }
    for (Edge edge : sharedEdges)
        builder.splitEdge(edge);
}

static Uses findDominatedUses(FunctionPassContext& context, const CounterCandidate& candidate)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    Uses uses;
    auto collectUsesOf = [&](ValueID value) {
        for (const Use& use : useLists.usesOf(value)) {
            if (context.analyses.dominates(candidate.stayingEdge, use.block))
                uses.append(use);
        }
    };

    ValueID tested = function.at(candidate.comparison).operand(candidate.testedOperandIndex);
    collectUsesOf(tested);
    if (!candidate.stepsEqualToTested.isEmpty()) {
        for (ValueID stepped : candidate.stepsEqualToTested)
            collectUsesOf(stepped);
        return uses;
    }
    if (tested != candidate.parameter && function.at(tested).type == function.at(candidate.parameter).type)
        collectUsesOf(candidate.parameter);
    return uses;
}

static void passCounterOnEdge(FunctionPassContext& context, IRBuilder& builder, const CounterCandidate& candidate, const Uses& dominatedUses)
{
    builder.setOrigin(context.function.at(candidate.comparison).origin());
    ValueID narrowed = builder.addEdgeResult(candidate.stayingEdge, candidate.testedOperandIndex);
    for (const Use& use : dominatedUses)
        replaceUse(context.function, use, narrowed);
}

static void passCountersOnEdges(FunctionPassContext& context, IRBuilder& builder, std::span<const CounterCandidate> candidates)
{
    splitSharedStayingEdges(context, builder, candidates);

    Vector<Uses, 4> dominatedUses;
    for (const CounterCandidate& candidate : candidates)
        dominatedUses.append(findDominatedUses(context, candidate));

    for (size_t index = 0; index < candidates.size(); ++index)
        passCounterOnEdge(context, builder, candidates[index], dominatedUses[index]);
    context.analyses.didChangeValues();
}

PassOutcome convertCountersToIntegers(FunctionPassContext& context)
{
    if (!hasNumberParameter(context.program, context.function))
        return PassOutcome::Unchanged;

    PassOutcome outcome = PassOutcome::Unchanged;
    uint64_t numberOfConvertedCounters = 0;
    BitVector parametersOverStepLimit;
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    for (;;) {
        Vector<CounterCandidate, 4> candidates = findCandidates(context, parametersOverStepLimit);
        if (candidates.isEmpty())
            break;
        passCountersOnEdges(context, builder, candidates.span());
        outcome = PassOutcome::Changed;

        TypePropagationOutcome typing = typeValuesFromNever({ context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo });
        // Cutting a block renumbers the values, so no id from before names its value any more.
        if (typing.didChangeControlFlow()) {
            parametersOverStepLimit.clearAll();
            continue;
        }
        numberOfConvertedCounters += std::ranges::count_if(candidates, [&](const CounterCandidate& candidate) {
            return context.program.typeOf(context.function, candidate.parameter).isInteger();
        });
    }

    if (numberOfConvertedCounters)
        context.build.statistics().count(StatisticID::CountersConverted, numberOfConvertedCounters);
    // The analyses were told of every edit, and the search that found no more candidates came behind the last one.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
