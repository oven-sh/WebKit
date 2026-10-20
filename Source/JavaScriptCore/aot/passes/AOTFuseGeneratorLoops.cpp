/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFuseGeneratorLoops.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineCopy.h"
#include "AOTSummarizeForInlining.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t takenTargetIndex = 0;

namespace {

struct ListedStep {
    BlockID block;
    ValueID value;
};

struct GeneratorOperations {
    Vector<ValueID, 4> generators;
    Vector<ListedStep, 8> steps;
};

struct CandidateSearch {
    FunctionPassContext& pass;
    const EscapeAnalysis& escape;
    std::span<const ListedStep> steps;
    uint64_t callerBytes { 0 };
};

} // anonymous namespace

static bool hasListedGenerator(const Function& function)
{
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (function.at(id).opcode == Opcode::NewGenerator)
                return true;
        }
    }
    return false;
}

static GeneratorOperations collectGeneratorOperations(FunctionPassContext& pass)
{
    GeneratorOperations operations;
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        for (ValueID id : pass.function.at(block).values) {
            Opcode opcode = pass.function.at(id).opcode;
            if (opcode == Opcode::NewGenerator)
                operations.generators.append(id);
            else if (opcode == Opcode::StepGenerator)
                operations.steps.append(ListedStep { block, id });
        }
    }
    return operations;
}

static uint64_t codeBytesOf(const Program& program, const Function& function)
{
    uint64_t bytes = 0;
    for (const Block& block : function.blocks)
        bytes += estimatedCodeBytes(program, function, block);
    return bytes;
}

template<typename Matches>
static Vector<ValueID, 4> collectMatchingUsers(const UseLists& useLists, std::span<const ValueID> values, NOESCAPE const Matches& matches)
{
    Vector<ValueID, 4> users;
    for (ValueID value : values) {
        for (const Use& use : useLists.usesOf(value)) {
            if (use.kind == Use::Kind::Operand && matches(use))
                users.append(use.user);
        }
    }
    return users;
}

static bool isStatusRead(const Function& function, const Use& use)
{
    const Value& user = function.at(use.user);
    return user.opcode == Opcode::ResultAt && user.index() == static_cast<uint32_t>(StepGeneratorResult::Status);
}

static bool isComparisonWithNotStepped(const Function& function, const Use& use)
{
    const Value& user = function.at(use.user);
    if (user.opcode != Opcode::StrictEq)
        return false;
    const Value& other = function.at(use.operandIndex == OperandIndex::StrictEq::left ? user.rightOperand() : user.leftOperand());
    return other.opcode == Opcode::Integer && other.integer() == static_cast<int64_t>(GeneratorStepStatus::NotStepped);
}

static BitVector findBlocksAfterImpossibleStatus(FunctionPassContext& pass, std::span<const ValueID> steps)
{
    const Function& function = pass.function;
    const UseLists& useLists = pass.analyses.useLists();
    Vector<ValueID, 4> statuses = collectMatchingUsers(useLists, steps, [&](const Use& use) {
        return isStatusRead(function, use);
    });
    Vector<ValueID, 4> comparisons = collectMatchingUsers(useLists, statuses.span(), [&](const Use& use) {
        return isComparisonWithNotStepped(function, use);
    });

    Vector<Edge, 4> impossibleEdges;
    for (ValueID comparison : comparisons) {
        for (const Use& use : useLists.usesOf(comparison)) {
            if (use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.kind == TerminatorKind::Branch)
                impossibleEdges.append(Edge { use.block, takenTargetIndex });
        }
    }

    BitVector blocks;
    if (impossibleEdges.isEmpty())
        return blocks;
    blocks.ensureSize(function.blocks.size());
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        bool isAfterImpossibleEdge = std::ranges::any_of(impossibleEdges, [&](Edge edge) {
            return pass.analyses.dominates(edge, block);
        });
        if (isAfterImpossibleEdge)
            blocks.quickSet(block.index());
    }
    return blocks;
}

static Vector<ValueID, 4> findUncheckedStepsOf(const CandidateSearch& search, ObjectID object)
{
    Vector<ValueID, 4> steps;
    for (const ListedStep& step : search.steps) {
        const Value& value = search.pass.function.at(step.value);
        if (value.check() == GeneratorStepCheck::None && search.escape.singleObject(value.generatorOperand()) == object)
            steps.append(step.value);
    }
    return steps;
}

static Vector<ListedStep, 4> findStepsReaching(const CandidateSearch& search, ObjectID object, const BitVector& ignoredBlocks)
{
    Vector<ListedStep, 4> steps;
    for (const ListedStep& step : search.steps) {
        if (ignoredBlocks.get(step.block.index()))
            continue;
        if (search.escape.pointsTo(search.pass.function.at(step.value).generatorOperand()).objects.contains(object))
            steps.append(step);
    }
    return steps;
}

static bool isListed(FunctionAnalyses& analyses, ValueID value)
{
    return value && analyses.useLists().locationOf(value).placement == ValuePlacement::InBlock;
}

static Use generatorUseOf(const ListedStep& step)
{
    return Use { Use::Kind::Operand, step.block, step.value, OperandIndex::StepGenerator::generator, 0 };
}

static GeneratorFusionDeclineReason addSites(const CandidateSearch& search, std::span<const ListedStep> steps, FusionCandidate& candidate)
{
    const Function& function = search.pass.function;
    candidate.sites.append(GeneratorStepSite { candidate.body.startAtCreation, ResumeMode::Next, ValueID() });
    for (const ListedStep& step : steps) {
        const Value& value = function.at(step.value);
        if (search.escape.singleObject(value.generatorOperand()) != candidate.object) {
            candidate.witness = generatorUseOf(step);
            return GeneratorFusionDeclineReason::StepMayReachOtherGenerator;
        }
        candidate.sites.append(GeneratorStepSite { step.value, value.resumeMode(), value.valueOperand() });
    }

    for (const ListedStep& step : steps) {
        if (function.at(step.value).check() != GeneratorStepCheck::None) {
            candidate.witness = generatorUseOf(step);
            return GeneratorFusionDeclineReason::MayEscapeBeforeStep;
        }
    }
    return GeneratorFusionDeclineReason::None;
}

static bool isGeneratorOperandOfSite(const FusionCandidate& candidate, const Use& use)
{
    if (use.kind != Use::Kind::Operand)
        return false;
    if (use.user == candidate.body.startAtCreation)
        return candidate.body.generatorParameter && use.operandIndex == OperandIndex::CallDirect::arguments + *candidate.body.generatorParameter;
    return use.operandIndex == OperandIndex::StepGenerator::generator && candidate.sites.containsIf([&](const GeneratorStepSite& site) {
        return site.step == use.user;
    });
}

static bool readsGeneratorState(FunctionAnalyses& analyses, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return analyses.effectsOf(use.user).onAnyEdge().reads.contains(AbstractHeap::InternalFields);
    case Use::Kind::TerminatorOperand:
        return analyses.effectsOfTerminator(use.block).onAnyEdge().reads.contains(AbstractHeap::InternalFields);
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static GeneratorFusionDeclineReason findUseObservingGenerator(const CandidateSearch& search, const BitVector& ignoredBlocks, FusionCandidate& candidate)
{
    for (const ObjectUse& objectUse : search.escape.usesOf(candidate.object)) {
        const Use& use = objectUse.use;
        if (ignoredBlocks.get(use.block.index()) || isGeneratorOperandOfSite(candidate, use))
            continue;
        if (!objectUse.isEscape && !objectUse.isContentsStore && !readsGeneratorState(search.pass.analyses, use))
            continue;
        candidate.witness = use;
        return GeneratorFusionDeclineReason::MayEscapeBeforeStep;
    }
    return GeneratorFusionDeclineReason::None;
}

static bool usesGeneratorOnlyToYield(const Value& value, ValueID generatorParameter)
{
    bool usesOnlyToYield = true;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operand == generatorParameter)
            usesOnlyToYield &= value.opcode == Opcode::YieldBySavingFrame && operandIndex == OperandIndex::YieldBySavingFrame::generator;
    });
    return usesOnlyToYield;
}

static bool canLeaveFrame(const Function& body, const Block& block, ValueID generatorParameter)
{
    bool canLeave = boundToOf(block.terminator.kind) != BoundTo::Frame;
    for (uint32_t index = 0; index < block.values.size(); ++index) {
        const Value& value = body.at(block.values[index]);
        canLeave &= value.opcode == Opcode::YieldBySavingFrame || !isBoundToFrame(body, block, index);
        canLeave &= usesGeneratorOnlyToYield(value, generatorParameter);
    }
    block.forEachChildOutsideValues([&](ValueID child) {
        canLeave &= !child || child != generatorParameter;
    });
    return canLeave;
}

static GeneratorFusionDeclineReason findObstacleInBody(const Function& body, const GeneratorBodyDescription& description)
{
    if (!body.hasBody() || body.at(entryBlock).parameters.size() != description.entryArguments.size())
        return GeneratorFusionDeclineReason::BodyIsNotDescribed;

    ValueID generatorParameter;
    if (description.generatorParameter)
        generatorParameter = body.at(entryBlock).parameters[*description.generatorParameter];

    bool canLeave = std::ranges::all_of(body.blocks, [&](const Block& block) {
        return canLeaveFrame(body, block, generatorParameter);
    });
    return canLeave ? GeneratorFusionDeclineReason::None : GeneratorFusionDeclineReason::BodyIsBoundToFrame;
}

static GeneratorFusionDeclineReason findSizeObstacle(const CandidateSearch& search, const Function& body, ValueID generator)
{
    FunctionPassContext& pass = search.pass;
    uint64_t bodyBytes = codeBytesOf(pass.program, body);
    if (pass.exceeds(LimitID::MaxOnlyCallSiteBytes, bodyBytes, generator))
        return GeneratorFusionDeclineReason::BodyIsTooLarge;
    if (pass.exceeds(LimitID::MaxInlinedCallerBytes, search.callerBytes + bodyBytes, generator))
        return GeneratorFusionDeclineReason::CallerIsTooLarge;
    if (pass.exceeds(LimitID::InlineFramesPerFunction, inlineFrameCountOf(pass.function) + inlineFrameCountOf(body), generator))
        return GeneratorFusionDeclineReason::CallerIsTooLarge;
    return GeneratorFusionDeclineReason::None;
}

static GeneratorFusionDeclineReason findDepthObstacle(FunctionPassContext& pass, const FusionCandidate& candidate)
{
    const Function& function = pass.function;
    uint32_t inlineFrame = function.at(candidate.generator).inlineFrame;
    bool isInsideBody = pass.id == candidate.body.body;
    for (uint32_t frame = inlineFrame; frame; frame = function.inlineFrames[frame].parent)
        isInsideBody |= function.inlineFrames[frame].callee == candidate.body.body;
    if (isInsideBody || pass.exceeds(LimitID::MaxInlineDepth, inlineDepthOf(pass.program, function, inlineFrame) + 1, candidate.generator))
        return GeneratorFusionDeclineReason::InlineDepth;
    return GeneratorFusionDeclineReason::None;
}

static GeneratorFusionDeclineReason findObstacle(const CandidateSearch& search, FusionCandidate& candidate)
{
    FunctionPassContext& pass = search.pass;
    std::optional<ObjectID> object = search.escape.objectDefinedBy(candidate.generator);
    if (!object)
        return GeneratorFusionDeclineReason::Unknown;
    candidate.object = *object;

    BitVector ignoredBlocks = findBlocksAfterImpossibleStatus(pass, findUncheckedStepsOf(search, *object).span());
    Vector<ListedStep, 4> steps = findStepsReaching(search, *object, ignoredBlocks);
    if (steps.isEmpty())
        return GeneratorFusionDeclineReason::HasNoStep;

    std::optional<GeneratorBodyDescription> description = describeGeneratorBody(pass.program, pass.function, candidate.generator);
    if (!description || !isListed(pass.analyses, description->startAtCreation))
        return GeneratorFusionDeclineReason::BodyIsNotDescribed;
    candidate.body = WTF::move(*description);

    if (search.escape.at(*object).liveInstances != LiveInstances::AtMostOne)
        return GeneratorFusionDeclineReason::SeveralLiveInstances;
    if (GeneratorFusionDeclineReason obstacle = addSites(search, steps.span(), candidate); obstacle != GeneratorFusionDeclineReason::None)
        return obstacle;
    if (GeneratorFusionDeclineReason obstacle = findUseObservingGenerator(search, ignoredBlocks, candidate); obstacle != GeneratorFusionDeclineReason::None)
        return obstacle;

    const Function& body = pass.program.at(candidate.body.body);
    if (GeneratorFusionDeclineReason obstacle = findObstacleInBody(body, candidate.body); obstacle != GeneratorFusionDeclineReason::None)
        return obstacle;
    if (GeneratorFusionDeclineReason obstacle = findSizeObstacle(search, body, candidate.generator); obstacle != GeneratorFusionDeclineReason::None)
        return obstacle;
    if (GeneratorFusionDeclineReason obstacle = findDepthObstacle(pass, candidate); obstacle != GeneratorFusionDeclineReason::None)
        return obstacle;
    return findMergeObstacle(pass, candidate);
}

Vector<FusionCandidate> findFusionCandidates(FunctionPassContext& pass)
{
    GeneratorOperations operations = collectGeneratorOperations(pass);
    GeneratorFusionDeclineReason initialObstacle = operations.steps.isEmpty() ? GeneratorFusionDeclineReason::HasNoStep : GeneratorFusionDeclineReason::Unknown;
    Vector<FusionCandidate> candidates;
    candidates.reserveInitialCapacity(operations.generators.size());
    for (ValueID generator : operations.generators)
        candidates.append(FusionCandidate { .generator = generator, .obstacle = initialObstacle });
    if (candidates.isEmpty() || operations.steps.isEmpty())
        return candidates;

    const EscapeAnalysis& escape = pass.analyses.escape();
    if (escape.gaveUp())
        return candidates;

    CandidateSearch search { pass, escape, operations.steps.span(), codeBytesOf(pass.program, pass.function) };
    for (FusionCandidate& candidate : candidates)
        candidate.obstacle = findObstacle(search, candidate);
    return candidates;
}

static LimitID limitOf(GeneratorFusionDeclineReason obstacle)
{
    switch (obstacle) {
    case GeneratorFusionDeclineReason::BodyIsTooLarge:
        return LimitID::MaxOnlyCallSiteBytes;
    case GeneratorFusionDeclineReason::CallerIsTooLarge:
        return LimitID::MaxInlinedCallerBytes;
    case GeneratorFusionDeclineReason::InlineDepth:
        return LimitID::MaxInlineDepth;
    case GeneratorFusionDeclineReason::Unknown:
    case GeneratorFusionDeclineReason::None:
    case GeneratorFusionDeclineReason::BodyIsNotDescribed:
    case GeneratorFusionDeclineReason::HasNoStep:
    case GeneratorFusionDeclineReason::SeveralLiveInstances:
    case GeneratorFusionDeclineReason::StepMayReachOtherGenerator:
    case GeneratorFusionDeclineReason::MayEscapeBeforeStep:
    case GeneratorFusionDeclineReason::BodyIsBoundToFrame:
        return LimitID::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void reportDecline(FunctionPassContext& pass, const FusionCandidate& candidate)
{
    if (candidate.obstacle == GeneratorFusionDeclineReason::HasNoStep)
        return;
    pass.addDecline(Wire::DeclineKind::GeneratorLoopNotFused, candidate.generator, candidate.obstacle, limitOf(candidate.obstacle));
}

PassOutcome fuseGeneratorLoops(FunctionPassContext& pass)
{
    if (!hasListedGenerator(pass.function))
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    PassOutcome outcome = PassOutcome::Unchanged;
    Vector<FusionCandidate> candidates = findFusionCandidates(pass);
    for (;;) {
        auto fusible = std::ranges::find(candidates, GeneratorFusionDeclineReason::None, &FusionCandidate::obstacle);
        if (fusible == candidates.end())
            break;
        fuseGenerator(pass, builder, *fusible);
        pass.build.statistics().count(StatisticID::GeneratorLoopsFused);
        outcome = PassOutcome::Changed;
        candidates = findFusionCandidates(pass);
    }

    for (const FusionCandidate& candidate : candidates)
        reportDecline(pass, candidate);
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
