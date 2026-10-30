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
#include "AOTFlowApply.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGenericCopy.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTInductionVariables.h"
#include "AOTMemoryLocation.h"
#include "AOTNaturalLoops.h"
#include "AOTObservableOperations.h"
#include "AOTOperatorForms.h"
#include "AOTProgramPasses.h"
#include "AOTRegionCloner.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
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
};

struct LoopClobbers {
    Vector<MemoryLocation> locations;
    LocationMasks writes;
    bool maySuspend { false };
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

// A staying edge into a shared block is split before it is used.
static bool dominatesEveryStep(FunctionAnalyses& analyses, Edge edge, const InductionVariable& variable)
{
    const UseLists& useLists = analyses.useLists();
    return std::ranges::all_of(variable.steppedValues, [&](ValueID stepped) {
        return dominatesAfterSplit(analyses, edge, useLists.blockOf(stepped));
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

static std::optional<CounterCandidate> findCandidateTestedBeforeStep(FunctionPassContext& context, const InductionVariable& variable)
{
    FunctionAnalyses& analyses = context.analyses;
    for (const InductionTest& test : variable.tests) {
        if (test.tested != TestedValue::BeforeStep || !isBoundingRelation(test.relation, variable.direction))
            continue;
        if (!context.program.typeOf(context.function, test.bound).isInteger() || !dominatesEveryStep(analyses, test.stayingEdge, variable))
            continue;
        const Value& comparison = context.function.at(test.comparison);
        ValueID bound = analyses.identityOf(test.bound);
        bool isLeftBound = analyses.identityOf(comparison.operand(0)) == bound;
        bool isRightBound = analyses.identityOf(comparison.operand(1)) == bound;
        if (isLeftBound == isRightBound)
            continue;
        uint32_t testedOperandIndex = isLeftBound ? 1 : 0;
        if (targetOf(context.function, test.stayingEdge).proofs.contains(testedOperandIndex))
            return std::nullopt;
        return CounterCandidate { variable.parameter, test.comparison, testedOperandIndex, test.stayingEdge };
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
            if (std::optional<CounterCandidate> candidate = findCandidateTestedBeforeStep(context, variable))
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
            if (!tested.isNumberWithoutFraction() || tested.isInt32() || (!testsForTruth && !typeOf(comparison.operand(1 - operandIndex)).isInteger()))
                continue;
            ValueID parameter = analyses.identityOf(comparison.operand(operandIndex));
            ValueLocation location = useLists.locationOf(parameter);
            if (location.placement != ValuePlacement::Parameter || location.block == entryBlock || terminator.taken().proofs.contains(operandIndex))
                continue;
            if (!testsForTruth && cycles.forest()[*cycle].isReducible() && isJudgedAsCounter.get(parameter.index()))
                continue;
            if (!std::ranges::contains(candidates, Edge { block, 0 }, &CounterCandidate::stayingEdge))
                candidates.append(CounterCandidate { parameter, terminator.conditionOperand(), operandIndex, Edge { block, 0 } });
        }
    }

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
        bool isShared = !context.analyses.hasSingleIncomingEdge(targetOf(function, candidate.stayingEdge).block);
        if (isShared && !sharedEdges.contains(candidate.stayingEdge))
            sharedEdges.append(candidate.stayingEdge);
    }
    for (Edge edge : sharedEdges)
        builder.splitEdge(edge);

    const UseLists& useLists = context.analyses.useLists();
    Vector<Vector<Use, 8>, 4> dominatedUses(candidates.size());
    for (size_t index = 0; index < candidates.size(); ++index) {
        const CounterCandidate& candidate = candidates[index];
        auto collectUsesOf = [&](ValueID value) {
            for (const Use& use : useLists.usesOf(value)) {
                if (context.analyses.dominates(candidate.stayingEdge, use.block))
                    dominatedUses[index].append(use);
            }
        };
        ValueID tested = function.at(candidate.comparison).operand(candidate.testedOperandIndex);
        collectUsesOf(tested);
        for (ValueID stepped : candidate.stepsEqualToTested)
            collectUsesOf(stepped);
        if (candidate.stepsEqualToTested.isEmpty() && tested != candidate.parameter && function.at(tested).type == function.at(candidate.parameter).type)
            collectUsesOf(candidate.parameter);
    }

    Vector<ValueID, 4> narrowedCounters;
    for (size_t index = 0; index < candidates.size(); ++index) {
        builder.setOrigin(function.at(candidates[index].comparison).origin());
        narrowedCounters.append(builder.addEdgeResult(candidates[index].stayingEdge, candidates[index].testedOperandIndex));
        for (const Use& use : dominatedUses[index])
            replaceUse(function, use, narrowedCounters.last());
    }
    context.analyses.didChangeValues();
    return narrowedCounters;
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
        return isCastWithoutTest(value.opcode) ? changeOf(value.operand(0)) : std::nullopt;
    }
};

struct Tally {
    uint32_t parameterIndex;
    Range range;
    Vector<Edge, 2> edgesOfNextTurns;
};

} // anonymous namespace

// An integer counter takes each value of its range at most once.
static std::optional<int64_t> mostTurnsBehindFirst(FunctionPassContext& context, const LoopInduction& loop)
{
    std::optional<int64_t> fewest;
    for (const InductionVariable& variable : loop.variables) {
        const Type& type = context.program.typeOf(context.function, variable.parameter);
        if (!type.isInteger() || !type.integers.isSubsetOf(Range::exact()) || variable.step.kind != InductionStepKind::Constant || !variable.step.constant)
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

struct SumWithCounter {
    ValueID counter;
    int64_t addend { 0 };
};

struct TestOfSum {
    ValueID comparison;
    uint32_t sideOfSum;
    SumWithCounter sum;
};

} // anonymous namespace

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
            return SumWithCounter { sum.operand(1), *left };
        return std::nullopt;
    case Opcode::Sub:
        if (std::optional<int64_t> right = constantAt(1); right && *right != std::numeric_limits<int64_t>::min())
            return SumWithCounter { sum.operand(0), -*right };
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

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

// `let sum = i; for (..) sum = (sum + x) | 0`: every turn hands on an int32, and only the first value may be no int32, yet that value makes the sum a double in every turn.
// So the first value is tested once in front of the loop, and the loop gets a copy whose sum is an integer.
static bool copyLoopWhoseFirstValueIsTheOnlyNumber(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    const NaturalLoops& loops = analyses.naturalLoops();
    // What an inner loop hands on is its own first value or an int32, and its first value is that of the outer loop.
    auto isInt32OrComesFrom = [&](ValueID parameter, ValueID start) {
        BitVector seen(function.values.size());
        Vector<ValueID, 8> worklist { start };
        while (!worklist.isEmpty()) {
            ValueID value = analyses.identityOf(worklist.takeLast());
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
                worklist.append(targetOf(function, edge).arguments[indexInBlock]);
            }
        }
        return true;
    };
    // Outer loops come first: their copy holds a copy of what is inside.
    Vector<const NaturalLoop*, 8> widestFirst;
    for (unsigned loopIndex = 0; loopIndex < loops.numLoops(); ++loopIndex)
        widestFirst.append(&loops.loop(loopIndex));
    AOT::stableSort(widestFirst, std::ranges::greater { }, &NaturalLoop::size);
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
            if (!type.isNumberWithoutFraction() || type.isInteger())
                continue;
            bool handsOnInt32 = std::ranges::all_of(analyses.cfg().incomingEdges(header), [&](Edge edge) {
                return !loops.belongsTo(edge.from, loop) || isInt32OrComesFrom(parameters[index], targetOf(function, edge).arguments[index]);
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
        uint64_t bytes = 0;
        for (unsigned index = 0; index < loop.size(); ++index) {
            region.append(loop.at(index));
            bytes += estimatedCodeBytes(program, function, function.at(loop.at(index)));
        }
        if (context.exceeds(LimitID::MaxBytesOfSeparatedLoop, bytes) || RegionCloner::checkCanDuplicate(analyses, region.span()) != CloneCheck::CanClone)
            continue;

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

PassOutcome convertCountersToIntegers(FunctionPassContext& context)
{
    if (context.isBehindRounds && std::exchange(context.hasConvertedCountersBehindRounds, true) && !std::exchange(context.hasBoundsTestedBehindRounds, false))
        return PassOutcome::Unchanged;
    if (!context.analyses.cfg().hasCycle() || (!hasNumberParameter(context.program, context.function) && !hasIntegerBeyondInt32(context.program, context.function)))
        return PassOutcome::Unchanged;

    PassOutcome outcome = PassOutcome::Unchanged;
    uint64_t numberOfConvertedCounters = 0;
    BitVector parametersOverStepLimit;
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    if (testsSumInCycle(context) && testCountersInsteadOfSums(context, builder))
        outcome = PassOutcome::Changed;
    // During the rounds, only a counter that has just got its own test is converted, before the tests that the round would place on it. Other counters have their bound by then or get it after the rounds.
    if (context.isInRounds && outcome == PassOutcome::Unchanged)
        return outcome;
    for (;;) {
        Vector<CounterCandidate, 4> candidates = findCounterCandidates(context, parametersOverStepLimit);
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

    if (numberOfConvertedCounters) {
        context.build.statistics().count(StatisticID::CountersConverted, numberOfConvertedCounters);
        context.hasCountersConvertedSinceTestsWerePlaced = true;
    }
    if (expectCarriedNumbersToStayInt32(context, builder)) {
        context.hasCountersConvertedSinceTestsWerePlaced = true;
        outcome = PassOutcome::Changed;
    }
    while (copyLoopWhoseFirstValueIsTheOnlyNumber(context)) {
        context.hasCountersConvertedSinceTestsWerePlaced = true;
        outcome = PassOutcome::Changed;
    }
    // The analyses were told of every edit, and the search that found no more candidates came behind the last one.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
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
    if (any.mayRunOtherCode())
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

static unsigned hoistReadsOutOf(FunctionPassContext& context, const NaturalLoop& loop, Vector<BlockID>& definingBlock, const BitVector& headersWhoseTestHoldsOnEntry, Vector<Edge, 2>& entriesFromBranches)
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
            if (lookupsHoistedWhereTheyRunNoCode.contains(value.opcode) || testsOfValuesThemselves.contains(value.opcode) || isCollectionLookup(program, value))
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
    AOT::sort(clobbers.locations);
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
    LoopClobbers clobbersSinceEntry;
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
    AOT::sort(loopsInnermostFirst, [&](const NaturalLoop* a, const NaturalLoop* b) {
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

    unsigned numberOfHoistedReads = 0;
    for (const NaturalLoop* loop : loopsInnermostFirst)
        numberOfHoistedReads += hoistReadsOutOf(context, *loop, definingBlock, headersWhoseTestHoldsOnEntry, entriesFromBranches);
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
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    Vector<std::pair<Edge, BlockID>, 2> blocksInFront;
    Vector<std::pair<ValueID, ValueID>, 4> variables;
    auto heldTypeOf = [&](const JoinKeptInLoop& kept) {
        return context.types.intern(program.at(function.at(kept.join).type).withPrimitives(Primitive::Absent));
    };
    for (const JoinKeptInLoop& kept : joins) {
        builder.setOrigin(function.at(kept.join).origin());
        auto known = std::ranges::find(blocksInFront, kept.entry, &std::pair<Edge, BlockID>::first);
        if (known == blocksInFront.end()) {
            blocksInFront.append({ kept.entry, builder.splitEdge(kept.entry) });
            known = &blocksInFront.last();
        }
        ValueID nothing = builder.addDetached(IRBuilder::make(Opcode::Absent));
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
        IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(variables[position].first, TypeTable::absent), 0, BlockFrequency::Rare, frequency);
        if (!madeForJoin.isEmpty()) {
            function.edit(branch.taken).values.appendVector(madeForJoin);
            context.analyses.didChangeValues();
        }
        builder.endExpansion(expansion, builder.append(WTF::move(join)));
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

static bool isCountedAndEnds(FunctionPassContext& context, const NaturalLoop& loop)
{
    const InductionVariable* counter = context.analyses.inductionVariables().of(loop).counter();
    if (!counter || counter->step.kind != InductionStepKind::Constant || !counter->step.constant)
        return false;
    const InductionTest* test = counter->controllingTest();
    if (!test || test->boundKind != LoopBoundKind::Invariant || !isBoundingRelation(test->relation, counter->direction))
        return false;
    return context.program.typeOf(context.function, counter->parameter).isInteger() && context.program.typeOf(context.function, test->bound).isInteger();
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

// The one block that the loop leaves for, if the loop does nothing but turn and end.
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

    BlockID exit;
    for (size_t index : body) {
        for (const Target& target : function.at(BlockID(index)).terminator.targets) {
            if (body.get(target.block.index()))
                continue;
            if (!target.arguments.isEmpty() || !target.proofs.isEmpty() || (exit && exit != target.block))
                return std::nullopt;
            exit = target.block;
        }
    }
    if (!exit)
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
    ShortLoop found { loop.header(), numberOfTurns, { }, { }, { } };
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
    return planTurnsInFrontOf(context, loop, *numberOfTurns);
}

// The turns stand one behind the other, each a copy of the loop that goes on to the next one where the loop went back to its header. The loop itself stands behind the last: whoever folds the test of its counter finds that it is left at once.
static void unroll(FunctionPassContext& context, const ShortLoop& loop)
{
    Function& function = context.function;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    Vector<std::unique_ptr<RegionCloner>> turns;
    for (uint64_t turn = 0; turn < loop.numberOfTurns; ++turn) {
        turns.append(makeUniqueWithoutFastMallocCheck<RegionCloner>(builder, function));
        turns.last()->cloneBlocks(loop.region.span(), [](ValueID, Value&) { });
    }

    for (Edge entry : loop.entries)
        builder.redirectEdge(entry, turns.first()->cloneOf(loop.header), Arguments(targetOf(function, entry).arguments));
    for (size_t turn = 0; turn < turns.size(); ++turn) {
        BlockID next = turn + 1 < turns.size() ? turns[turn + 1]->cloneOf(loop.header) : loop.header;
        for (Edge backEdge : loop.backEdges) {
            Edge leaving { turns[turn]->cloneOf(backEdge.from), backEdge.targetIndex };
            builder.redirectEdge(leaving, next, Arguments(targetOf(function, leaving).arguments));
        }
    }
    context.analyses.didChangeControlFlow();
    RegionCloner::repairSSA(context.analyses, turns.span());
    context.analyses.didChangeControlFlow();
    for (const std::unique_ptr<RegionCloner>& turn : turns) {
        if (!comesBackPastNoOtherMark(context, turn->cloneOf(loop.header)))
            eraseLoopHints(builder, function, turn->cloneOf(loop.header));
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
    PassOutcome outcome = PassOutcome::Unchanged;
    for (uint64_t taken = 0; taken < context.build.limit(LimitID::MaxUnrolledLoopsPerFunction); ++taken) {
        std::optional<ShortLoop> found;
        Vector<ValueID, 4> initializations;
        NaturalLoops& loops = context.analyses.naturalLoops();
        for (unsigned index = 0; index < loops.numLoops() && !found; ++index) {
            const NaturalLoop& loop = loops.loop(index);
            if (!initializes(loop) || !isExpectedToRun(context, loop).predicted())
                continue;
            std::optional<ShortLoop> plan = planTurnsInFrontOf(context, loop, 1);
            if (!plan || plan->entries.isEmpty() || keepsNothingAtHandAnyway(context, *plan))
                continue;
            initializations = findInitializationsOfEveryTurn(context, *plan);
            if (!initializations.isEmpty())
                found = WTF::move(plan);
        }
        if (!found)
            break;
        unroll(context, *found);

        IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
        for (BlockID block : found->region) {
            const BlockValues& values = function.at(block).values;
            BitVector erasedIndexes;
            for (size_t index = 0; index < values.size(); ++index) {
                if (initializations.contains(values[index]))
                    erasedIndexes.set(index);
            }
            if (!erasedIndexes.isEmpty())
                builder.eraseValues(block, erasedIndexes);
        }
        context.analyses.didChangeValues();
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

PassOutcome unrollShortLoops(FunctionPassContext& context)
{
    PassOutcome outcome = takeFirstTurnsThatInitializeOutOfLoops(context);
    const CFG& cfg = context.analyses.cfg();
    bool mayHaveShortLoop = std::ranges::any_of(cfg.reversePostOrder(), [&](BlockID block) {
        return mayBeHeaderOfShortLoop(context.program, context.function, context.function.at(block)) && std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) {
            return cfg.isReachable(edge.from) && cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(block);
        });
    });
    if (!mayHaveShortLoop)
        return outcome;
    for (uint64_t unrolled = 0; unrolled < context.build.limit(LimitID::MaxUnrolledLoopsPerFunction); ++unrolled) {
        std::optional<ShortLoop> found;
        NaturalLoops& loops = context.analyses.naturalLoops();
        for (unsigned index = 0; index < loops.numLoops() && !found; ++index)
            found = findShortLoop(context, loops.loop(index));
        if (!found)
            break;
        unroll(context, *found);
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
