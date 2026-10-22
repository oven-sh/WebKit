/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTDominators.h"
#include "AOTEffectsCheck.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTFunctionAnalyses.h"
#include "AOTTypeTestSite.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool stepsMatchElements(const Function& function, const Iteration& iteration, uint32_t count)
{
    BitVector steppedIndexes(count);
    for (ValueID id : iteration.resumes) {
        const Value& resume = function.at(id);
        std::optional<uint32_t> index = resume.stepIndex();
        if (resume.resumeMode() != ResumeMode::Next || resume.sentOperand() || resume.readsValueWhenDone() || !index || *index >= count)
            return false;
        if (steppedIndexes.set(*index))
            return false;
    }
    return steppedIndexes.bitCount() == count;
}

static ValueID findFirstStep(const Function& function, const Iteration& iteration)
{
    for (ValueID resume : iteration.resumes) {
        if (function.at(resume).stepIndex() == 0u)
            return resume;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool firstStepDominatesSteps(FunctionPassContext& pass, const Iteration& iteration)
{
    ValueID first = findFirstStep(pass.function, iteration);
    const UseLists& useLists = pass.analyses.useLists();
    return std::ranges::all_of(iteration.resumes, [&](ValueID resume) {
        return resume == first || pass.analyses.dominates(first, useLists.pointOf(resume));
    });
}

static bool findsKnownIteratorMethod(const Program& program, const Type& iterable)
{
    ProvenAccess method = provenReadOf(program, iterable, program.builtins().nameOf(WellKnownSymbol::Iterator));
    return method.isComplete && !method.methods.isEmpty();
}

static bool isOperationOf(ValueID open, const Value& value)
{
    return (value.opcode == Opcode::IteratorResume || value.opcode == Opcode::IteratorClose) && value.iteratorOperand() == open;
}

static bool isLandingCode(const Dominators& dominators, const Iteration& iteration, BlockID block)
{
    return iteration.cleanupBlocks.containsIf([&](BlockID cleanupBlock) {
        return dominators.dominates(cleanupBlock, block);
    });
}

static bool hasSuccessorIn(const CFG& cfg, const BitVector& blocks, BlockID block)
{
    return std::ranges::any_of(cfg.successors(block), [&](BlockID successor) {
        return blocks.get(successor.index());
    });
}

static size_t indexAfterLastOperation(const Function& function, ValueID open, const Block& block)
{
    size_t lastOperation = block.values.reverseFindIf([&](ValueID value) {
        return isOperationOf(open, function.at(value));
    });
    return lastOperation == notFound ? 0 : lastOperation + 1;
}

// What may run between the open and the last operation of the pattern, but for those operations themselves and the landing code.
static bool anyOperationBetweenSteps(FunctionPassContext& pass, const Iteration& iteration, std::span<const BlockID> liveRegion, NOESCAPE const Invocable<bool(ValueID)> auto& matchesValue, NOESCAPE const Invocable<bool(BlockID)> auto& matchesTerminator)
{
    const Function& function = pass.function;
    FunctionAnalyses& analyses = pass.analyses;

    BitVector liveAtHead(function.blocks.size());
    for (BlockID id : liveRegion)
        liveAtHead.quickSet(id.index());

    for (BlockID id : liveRegion) {
        if (isLandingCode(analyses.dominators(), iteration, id))
            continue;

        // The block of the last close goes on with the code after the pattern, which no step moves across.
        const Block& block = function.at(id);
        bool isLiveAtTail = hasSuccessorIn(analyses.cfg(), liveAtHead, id);
        size_t end = isLiveAtTail ? block.values.size() : indexAfterLastOperation(function, iteration.open, block);
        for (ValueID value : block.values.span().first(end)) {
            if (!isOperationOf(iteration.open, function.at(value)) && matchesValue(value))
                return true;
        }
        if (isLiveAtTail && matchesTerminator(id))
            return true;
    }
    return false;
}

static bool regionInterferesWithSteps(FunctionPassContext& pass, const Iteration& iteration, std::span<const BlockID> liveRegion)
{
    constexpr Effects step = Effects::unknownCode();
    return anyOperationBetweenSteps(pass, iteration, liveRegion, [&](ValueID value) {
        return pass.analyses.effectsOf(value).onAnyEdge().interferesWith(step);
    }, [&](BlockID block) {
        return pass.analyses.effectsOfTerminator(block).onAnyEdge().interferesWith(step);
    });
}

// The reads stay where the steps stood, so what stands between them only has to leave alone what they rely on: the length, and what the open tested.
// Whatever would end the proof of a test of those ends this one. What the type proves holds for good.
static bool regionMayChangeArray(FunctionPassContext& pass, const Iteration& iteration, const IterationArm& arm, std::span<const BlockID> liveRegion, ValueID array)
{
    FunctionAnalyses& analyses = pass.analyses;
    Vector<Value, 2> tests { makeArrayStorageTest(array, ArrayStorage::Contiguous) };
    if (!arm.namesAreProven)
        tests.append(makePristineTest(array, arm.builtinClass, { PristineAspects::Storage, PristineAspects::Names }));

    Vector<GuardDependencies, 2> dependencies;
    for (const Value& test : tests) {
        std::optional<GuardDependencies> dependenciesOfTest = dependenciesOfGuardOf(analyses, test);
        RELEASE_ASSERT(dependenciesOfTest);
        dependencies.append(WTF::move(*dependenciesOfTest));
    }

    return anyOperationBetweenSteps(pass, iteration, liveRegion, [&](ValueID value) {
        GuardWriter writer(analyses, value);
        return std::ranges::any_of(dependencies, [&](const GuardDependencies& relied) {
            return writer.mayEnd(relied);
        });
    }, [&](BlockID block) {
        Effects effects = analyses.effectsOfTerminator(block).onAnyEdge();
        return effects.maySuspend || std::ranges::any_of(dependencies, [&](const GuardDependencies& relied) {
            return effects.writes.overlaps(relied.masks);
        });
    });
}

static std::optional<Target> handlerAroundPattern(const Function& function, const Iteration& iteration, BlockID id)
{
    const std::optional<Target>& handler = function.at(id).handler;
    if (!handler || !iteration.cleanupBlocks.contains(handler->block))
        return handler;

    const Block& cleanup = function.at(handler->block);
    std::optional<Target> outer = cleanup.handler;
    if (!outer)
        return outer;
    for (ValueID& argument : outer->arguments) {
        if (size_t parameter = cleanup.parameters.find(argument); parameter != notFound)
            argument = handler->arguments[parameter];
    }
    return outer;
}

static bool operationsShareHandler(FunctionPassContext& pass, const Iteration& iteration)
{
    const Function& function = pass.function;
    const Dominators& dominators = pass.analyses.dominators();
    const UseLists& useLists = pass.analyses.useLists();
    std::optional<Target> shared = handlerAroundPattern(function, iteration, useLists.pointOf(iteration.open).block);
    auto hasSharedHandler = [&](ValueID operation) {
        BlockID block = useLists.pointOf(operation).block;
        return isLandingCode(dominators, iteration, block) || handlerAroundPattern(function, iteration, block) == shared;
    };
    return std::ranges::all_of(iteration.resumes, hasSharedHandler) && std::ranges::all_of(iteration.closes, hasSharedHandler);
}

bool hasProvenPlainElements(const Type& iterable)
{
    const ClassPart* array = iterable.singleBuiltinClass();
    return array && array->provenPristine.contains(PristineAspects::Storage);
}

// No read of an element runs code, and a read past the end or of a hole finds undefined, as a step past the end does. An array that nothing changes
// between the steps has one length for all of them, so a step after the last one is a read past the end.
static bool hasStepsThatReadElements(FunctionPassContext& pass, const Iteration& iteration, const IterationArm& arm, std::span<const BlockID> liveRegion, uint32_t count)
{
    if (arm.kind != JSC::IterationKind::Values)
        return false;
    ValueID array = pass.function.at(iteration.open).iterableOperand();
    // The test of the names has a failing edge, and the elements are tested with them.
    if (arm.namesAreProven && !hasProvenPlainElements(pass.program.typeOf(pass.function, array)))
        return false;
    return stepsMatchElements(pass.function, iteration, count) && !regionMayChangeArray(pass, iteration, arm, liveRegion, array);
}

PatternRead choosePatternRead(FunctionPassContext& pass, const Iteration& iteration, const IterationArm& arm, std::span<const BlockID> liveRegion)
{
    if (iteration.kind == IteratorKind::Async)
        return PatternRead::BySteps;

    const Function& function = pass.function;
    const Value& open = function.at(iteration.open);
    std::optional<uint32_t> count = open.elementCount();
    if (!open.isDestructuring() || open.hasRest() || !count || !*count)
        return PatternRead::BySteps;
    if (arm.form == IterationForm::Array)
        return hasStepsThatReadElements(pass, iteration, arm, liveRegion, *count) ? PatternRead::ByIndex : PatternRead::BySteps;
    if (arm.form != IterationForm::Protocol)
        return PatternRead::BySteps;
    if (pass.exceeds(LimitID::MaxDestructuredElements, *count, iteration.open))
        return PatternRead::BySteps;
    if (!stepsMatchElements(function, iteration, *count) || !firstStepDominatesSteps(pass, iteration))
        return PatternRead::BySteps;
    if (!operationsShareHandler(pass, iteration))
        return PatternRead::BySteps;
    if (findsKnownIteratorMethod(pass.program, pass.program.typeOf(function, open.iterableOperand())))
        return PatternRead::BySteps;
    if (regionInterferesWithSteps(pass, iteration, liveRegion))
        return PatternRead::BySteps;
    return PatternRead::InOneRead;
}

namespace {

struct ResultRead {
    ValueID read;
    uint32_t stepIndex { 0 };
};

// What a pattern takes from the function as it stands before any pattern is changed.
struct PlannedRead {
    ProgramPoint firstStep;
    Origin origin;
    uint32_t count { 0 };
    ReplacedEffects replaced;
    Vector<ResultRead, 8> resultReads;
    ValueID oneRead;
};

} // anonymous namespace

static bool isBefore(const ProgramPoint& a, const ProgramPoint& b)
{
    return std::tie(a.block, a.index) < std::tie(b.block, b.index);
}

static PlannedRead planOneRead(FunctionPassContext& pass, const UseLists& useLists, const Iteration& operations, Vector<ProgramPoint>& pointsOfOperations)
{
    const Function& function = pass.function;
    const EffectContext& effectContext = pass.analyses.effectContext();
    const Value& open = function.at(operations.open);

    PlannedRead plan;
    plan.firstStep = useLists.pointOf(findFirstStep(function, operations));
    plan.origin = open.origin();
    plan.count = *open.elementCount();
    plan.replaced.add(effectContext, function, open);

    for (ValueID resume : operations.resumes) {
        plan.replaced.add(effectContext, function, function.at(resume));
        pointsOfOperations.append(useLists.pointOf(resume));
        uint32_t stepIndex = *function.at(resume).stepIndex();
        for (const Use& use : useLists.usesOf(resume)) {
            RELEASE_ASSERT(use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ResultAt);
            plan.resultReads.append(ResultRead { use.user, stepIndex });
        }
    }
    for (ValueID close : operations.closes) {
        plan.replaced.add(effectContext, function, function.at(close));
        pointsOfOperations.append(useLists.pointOf(close));
    }
    return plan;
}

static void eraseOperations(IRBuilder& builder, std::span<const ProgramPoint> sortedPoints)
{
    for (size_t first = 0; first < sortedPoints.size();) {
        BlockID block = sortedPoints[first].block;
        BitVector indexes;
        for (; first < sortedPoints.size() && sortedPoints[first].block == block; ++first)
            indexes.set(sortedPoints[first].index);
        builder.eraseValues(block, indexes);
    }
}

static ProgramPoint pointAfterErasing(std::span<const ProgramPoint> sortedPoints, ProgramPoint point)
{
    auto firstInBlock = std::ranges::lower_bound(sortedPoints, ProgramPoint::atHeadOf(point.block), isBefore);
    auto firstNotBefore = std::ranges::lower_bound(sortedPoints, point, isBefore);
    return { point.block, point.index - static_cast<uint32_t>(firstNotBefore - firstInBlock) };
}

static void redirectResultReads(Function& function, const PlannedRead& plan)
{
    for (const ResultRead& resultRead : plan.resultReads) {
        Value& read = function.edit(resultRead.read);
        switch (static_cast<IteratorResumeResult>(read.index())) {
        case IteratorResumeResult::Done:
            read.become(Opcode::Boolean);
            read.setBoolean(false);
            continue;
        case IteratorResumeResult::Value:
            read.setResultsOperand(plan.oneRead);
            read.setIndex(resultRead.stepIndex);
            continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// Every point and every use is read before the first change, so the use lists are computed once for all patterns.
void expandPatternsInOneRead(FunctionPassContext& pass, IRBuilder& builder, std::span<const PatternInOneRead> patterns)
{
    if (patterns.empty())
        return;

    Function& function = pass.function;
    Vector<PlannedRead> plans;
    plans.reserveInitialCapacity(patterns.size());
    Vector<ProgramPoint> pointsOfOperations;
    {
        const UseLists& useLists = pass.analyses.useLists();
        for (const PatternInOneRead& pattern : patterns)
            plans.append(planOneRead(pass, useLists, pattern.operations, pointsOfOperations));
    }
    std::ranges::sort(pointsOfOperations, isBefore);
    eraseOperations(builder, pointsOfOperations.span());

    for (size_t index = 0; index < patterns.size(); ++index) {
        PlannedRead& plan = plans[index];
        builder.setOrigin(plan.origin);
        Value read = IRBuilder::make(Opcode::DestructureIterable, { patterns[index].iterable });
        read.setCount(plan.count);
        plan.oneRead = builder.insertBefore(pointAfterErasing(pointsOfOperations.span(), plan.firstStep), WTF::move(read));
    }
    builder.applyInsertions();

    const EffectContext& effectContext = pass.analyses.effectContext();
    for (const PlannedRead& plan : plans) {
        checkReplacement(effectContext, function, ReplacementSite { pass.id, plan.firstStep.block, plan.oneRead }, function.at(plan.oneRead), plan.replaced, pass.build.diagnostics());
        redirectResultReads(function, plan);
    }
    pass.analyses.didChangeValues();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
