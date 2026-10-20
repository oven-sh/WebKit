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
static bool anyOperationBetweenSteps(FunctionPassContext& pass, const Iteration& iteration, NOESCAPE const Invocable<bool(ValueID)> auto& matchesValue, NOESCAPE const Invocable<bool(BlockID)> auto& matchesTerminator)
{
    const Function& function = pass.function;
    FunctionAnalyses& analyses = pass.analyses;

    Vector<BlockID> region = findLiveRegion(pass, iteration.open);
    BitVector liveAtHead(function.blocks.size());
    for (BlockID id : region)
        liveAtHead.quickSet(id.index());

    for (BlockID id : region) {
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

static bool regionInterferesWithSteps(FunctionPassContext& pass, const Iteration& iteration)
{
    constexpr Effects step = Effects::unknownCode();
    return anyOperationBetweenSteps(pass, iteration, [&](ValueID value) {
        return pass.analyses.effectsOf(value).onAnyEdge().interferesWith(step);
    }, [&](BlockID block) {
        return pass.analyses.effectsOfTerminator(block).onAnyEdge().interferesWith(step);
    });
}

// The reads stay where the steps stood, so what stands between them only has to leave alone what they rely on: the length, and what the open tested.
// Whatever would end the proof of a test of those ends this one. What the type proves holds for good.
static bool regionMayChangeArray(FunctionPassContext& pass, const Iteration& iteration, const IterationArm& arm, ValueID array)
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

    return anyOperationBetweenSteps(pass, iteration, [&](ValueID value) {
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
static bool hasStepsThatReadElements(FunctionPassContext& pass, const Iteration& iteration, const IterationArm& arm, uint32_t count)
{
    if (arm.kind != JSC::IterationKind::Values)
        return false;
    ValueID array = pass.function.at(iteration.open).iterableOperand();
    // The test of the names has a failing edge, and the elements are tested with them.
    if (arm.namesAreProven && !hasProvenPlainElements(pass.program.typeOf(pass.function, array)))
        return false;
    return stepsMatchElements(pass.function, iteration, count) && !regionMayChangeArray(pass, iteration, arm, array);
}

PatternRead choosePatternRead(FunctionPassContext& pass, const Iteration& iteration, const IterationArm& arm)
{
    if (iteration.kind == IteratorKind::Async)
        return PatternRead::BySteps;

    const Function& function = pass.function;
    const Value& open = function.at(iteration.open);
    std::optional<uint32_t> count = open.elementCount();
    if (!open.isDestructuring() || open.hasRest() || !count || !*count)
        return PatternRead::BySteps;
    if (arm.form == IterationForm::Array)
        return hasStepsThatReadElements(pass, iteration, arm, *count) ? PatternRead::ByIndex : PatternRead::BySteps;
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
    if (regionInterferesWithSteps(pass, iteration))
        return PatternRead::BySteps;
    return PatternRead::InOneRead;
}

static void redirectResultReads(FunctionPassContext& pass, const Iteration& iteration, ValueID oneRead)
{
    Function& function = pass.function;
    const UseLists& useLists = pass.analyses.useLists();
    for (ValueID resume : iteration.resumes) {
        uint32_t stepIndex = *function.at(resume).stepIndex();
        for (const Use& use : useLists.usesOf(resume)) {
            RELEASE_ASSERT(use.kind == Use::Kind::Operand);
            Value& read = function.at(use.user);
            RELEASE_ASSERT(read.opcode == Opcode::ResultAt);
            switch (static_cast<IteratorResumeResult>(read.index())) {
            case IteratorResumeResult::Done:
                read.become(Opcode::Boolean);
                read.setBoolean(false);
                continue;
            case IteratorResumeResult::Value:
                read.setResultsOperand(oneRead);
                read.setIndex(stepIndex);
                continue;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    pass.analyses.didChangeValues();
}

static void eraseOperations(FunctionPassContext& pass, IRBuilder& builder, const Iteration& iteration)
{
    Vector<ProgramPoint, 8> points;
    {
        const UseLists& useLists = pass.analyses.useLists();
        for (ValueID resume : iteration.resumes)
            points.append(useLists.pointOf(resume));
        for (ValueID close : iteration.closes)
            points.append(useLists.pointOf(close));
    }
    std::ranges::sort(points, [](const ProgramPoint& a, const ProgramPoint& b) {
        return std::tie(a.block, a.index) < std::tie(b.block, b.index);
    });

    for (size_t first = 0; first < points.size();) {
        BlockID block = points[first].block;
        BitVector indexes;
        for (; first < points.size() && points[first].block == block; ++first)
            indexes.set(points[first].index);
        builder.eraseValues(block, indexes);
    }
}

void expandPatternInOneRead(FunctionPassContext& pass, IRBuilder& builder, const Iteration& iteration, ValueID iterable)
{
    Function& function = pass.function;
    const EffectContext& effectContext = pass.analyses.effectContext();

    ReplacedEffects replaced;
    replaced.add(effectContext, function, function.at(iteration.open));
    for (ValueID resume : iteration.resumes)
        replaced.add(effectContext, function, function.at(resume));
    for (ValueID close : iteration.closes)
        replaced.add(effectContext, function, function.at(close));

    ProgramPoint firstStep = pass.analyses.useLists().pointOf(findFirstStep(function, iteration));
    builder.setOrigin(function.at(iteration.open).origin());
    Value read = IRBuilder::make(Opcode::DestructureIterable, { iterable });
    read.setCount(*function.at(iteration.open).elementCount());
    ValueID oneRead = builder.insertBefore(firstStep, WTF::move(read));
    builder.applyInsertions();
    checkReplacement(effectContext, function, ReplacementSite { pass.id, firstStep.block, oneRead }, function.at(oneRead), replaced, pass.build.diagnostics());

    redirectResultReads(pass, iteration, oneRead);
    eraseOperations(pass, builder, iteration);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
