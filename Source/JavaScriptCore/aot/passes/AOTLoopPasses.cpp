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
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGenericCopy.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTInductionVariables.h"
#include "AOTMemoryLocation.h"
#include "AOTNaturalLoops.h"
#include "AOTProgramPasses.h"
#include "AOTRegionCloner.h"
#include "AOTSort.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
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
    if (!isNumberBeyondInt32(context.program.typeOf(context.function, variable.parameter)) || !variable.start)
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

PassOutcome convertCountersToIntegers(FunctionPassContext& context)
{
    if (!context.analyses.cfg().hasCycle() || !hasNumberParameter(context.program, context.function))
        return PassOutcome::Unchanged;

    PassOutcome outcome = PassOutcome::Unchanged;
    uint64_t numberOfConvertedCounters = 0;
    BitVector parametersOverStepLimit;
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    for (;;) {
        Vector<CounterCandidate, 4> candidates = findCounterCandidates(context, parametersOverStepLimit);
        if (candidates.isEmpty())
            break;
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
    // The analyses were told of every edit, and the search that found no more candidates came behind the last one.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

static MemoryAccess variableAccessOf(const Function& function, const Value& value)
{
    constexpr AbstractHeapSet hoistableHeaps { AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields };
    MemoryAccess access = memoryAccessOf(function, value);
    if (!access || !hoistableHeaps.contains(access.location.heap()))
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

// In the header what stands in front of a read is known. Anything in the loop may have run in front of a read in another block.
static unsigned hoistReadsOutOf(FunctionPassContext& context, const NaturalLoop& loop, Vector<BlockID>& definingBlock, const BitVector& testsThatStay)
{
    const Program& program = context.program;
    Function& function = context.function;
    BlockID header = loop.header();
    if (isClosedOnlyByWaysBack(context.analyses, loop))
        return 0;
    bool hasRead = false;
    BitVector body(function.blocks.size());
    for (unsigned i = 0; i < loop.size(); ++i) {
        body.quickSet(loop.at(i).index());
        hasRead |= std::ranges::any_of(function.at(loop.at(i)).values, [&](ValueID id) {
            const Value& value = function.at(id);
            return value.opcode == Opcode::ToBoolean || (hoistableReadOpcodes.contains(value.opcode) && variableAccessOf(function, value).kind == MemoryAccess::Kind::Read);
        });
    }
    if (!hasRead || function.at(header).isHandlerBlock())
        return 0;
    std::optional<Edge> entry = context.analyses.naturalLoops().findSingleEntry(loop);
    if (!entry || function.at(entry->from).terminator.kind != TerminatorKind::Jump)
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
    auto hasInvariantOperands = [&](const Value& value) {
        bool result = true;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            BlockID block = definingBlock[operand.index()];
            result &= block && !body.get(block.index());
        });
        return result;
    };
    auto canHoist = [&](ValueID id, bool isInHeader, const LoopClobbers& clobbersBeforeRead) {
        const Value& value = function.at(id);
        if (value.opcode == Opcode::ToBoolean)
            return !keptReads.get(id.index()) && !testsThatStay.get(id.index()) && hasInvariantOperands(value);
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
        if (!isInHeader && stability != LocationStability::UnchangedDuringActivation)
            return false;
        if (stability != LocationStability::UnchangedDuringActivation && clobbersBeforeRead.writes.overlaps(program, access.location))
            return false;
        if (stability == LocationStability::Unknown && clobbers.writes.overlaps(program, access.location))
            return false;
        return hasInvariantOperands(value);
    };

    unsigned numberOfHoistedReads = 0;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    const BlockFrequencies& frequencies = context.analyses.blockFrequencies();
    BlockFrequency frequencyOfPreheader = frequencies.classOf(preheader);
    for (BlockID blockID : context.analyses.cfg().reversePostOrder()) {
        if (!body.get(blockID.index()))
            continue;
        // In front of the loop a read runs each time the loop is entered, and what it read is kept all the way round. A block that seldom runs pays for its own reads.
        BlockFrequency frequency = frequencies.classOf(blockID);
        if (frequency == BlockFrequency::Rare || frequency > frequencyOfPreheader)
            continue;
        LoopClobbers clobbersInHeader;
        unsigned numberOfReadsBefore = numberOfHoistedReads;
        BlockValues remaining;
        remaining.reserveInitialCapacity(function.at(blockID).values.size());
        for (ValueID id : function.at(blockID).values) {
            if (!canHoist(id, blockID == header, blockID == header ? clobbersInHeader : clobbers)) {
                addClobbers(context, id, clobbersInHeader);
                remaining.append(id);
                continue;
            }
            function.edit(preheader).values.append(id);
            definingBlock[id.index()] = preheader;
            ++numberOfHoistedReads;
            appendKeptReadForValidation(builder, remaining, id);
            if (context.build.validates() && blockID != header && function.at(id).opcode == Opcode::GetModuleVariable)
                function.edit(id).setMayPrecedeItsStore(true);
        }
        if (numberOfHoistedReads > numberOfReadsBefore)
            function.edit(blockID).values = WTF::move(remaining);
        while (definingBlock.size() < function.values.size())
            definingBlock.append(blockID);
    }
    return numberOfHoistedReads;
}

PassOutcome hoistLoopInvariantReads(FunctionPassContext& context)
{
    const Function& function = context.function;
    CFG& cfg = context.analyses.cfg();
    if (!cfg.hasCycle())
        return PassOutcome::Unchanged;

    NaturalLoops& naturalLoops = context.analyses.naturalLoops();
    Vector<const NaturalLoop*> loopsInnermostFirst;
    loopsInnermostFirst.reserveInitialCapacity(naturalLoops.numLoops());
    for (unsigned i = 0; i < naturalLoops.numLoops(); ++i)
        loopsInnermostFirst.append(&naturalLoops.loop(i));
    if (loopsInnermostFirst.isEmpty())
        return PassOutcome::Unchanged;
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

    // A value that may be unspecified is tested only where the program tests it. Asked before anything moves: the answer follows the uses of values.
    BitVector testsThatStay;
    if (context.analyses.opcodesOfValues().contains(Opcode::ToBoolean)) {
        UnspecifiedValues unspecified(context.analyses);
        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                if (function.at(id).opcode == Opcode::ToBoolean && unspecified.mayContain(function.at(id).operand(0)))
                    testsThatStay.set(id.index());
            }
        }
    }

    unsigned numberOfHoistedReads = 0;
    for (const NaturalLoop* loop : loopsInnermostFirst)
        numberOfHoistedReads += hoistReadsOutOf(context, *loop, definingBlock, testsThatStay);
    if (!numberOfHoistedReads)
        return PassOutcome::Unchanged;

    context.analyses.didChangeValues();
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
    return true;
}

static bool eraseLoopHints(IRBuilder& builder, const Function& function, BlockID block)
{
    BitVector hints;
    std::span<const ValueID> values = function.at(block).values.span();
    for (size_t index = 0; index < values.size(); ++index) {
        if (function.at(values[index]).opcode == Opcode::LoopHint)
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

PassOutcome removeDeadCountedLoops(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    while (removeOutermostDeadLoops(context))
        outcome = PassOutcome::Changed;
    if (removeHintsOutsideCycles(context))
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

static std::optional<ShortLoop> findShortLoop(FunctionPassContext& context, const NaturalLoop& loop)
{
    const Function& function = context.function;
    NaturalLoops& loops = context.analyses.naturalLoops();
    std::optional<uint64_t> numberOfTurns = findNumberOfTurns(context, loop);
    if (!numberOfTurns || !*numberOfTurns || *numberOfTurns > context.build.limit(LimitID::MaxTurnsOfUnrolledLoop) || !isHotLoop(context, loop))
        return std::nullopt;

    ShortLoop found { loop.header(), *numberOfTurns, { }, { }, { } };
    uint64_t bytes = 0;
    for (unsigned index = 0; index < loop.size(); ++index) {
        if (loops.innerMostLoopOf(loop.at(index)) != &loop)
            return std::nullopt;
        found.region.append(loop.at(index));
        bytes += estimatedCodeBytes(context.program, function, function.at(loop.at(index)));
    }
    if (bytes * *numberOfTurns > context.build.limit(LimitID::MaxBytesOfUnrolledLoop))
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
        eraseLoopHints(builder, function, turns[turn]->cloneOf(loop.header));
    }
    context.analyses.didChangeControlFlow();
    RegionCloner::repairSSA(context.analyses, turns.span());
    context.analyses.didChangeControlFlow();
}

PassOutcome unrollShortLoops(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
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
