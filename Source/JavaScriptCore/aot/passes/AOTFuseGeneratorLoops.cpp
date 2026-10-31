/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTCodeSizeEstimate.h"
#include "AOTCoroutines.h"
#include "AOTEscapeAnalysis.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTInlineCopy.h"
#include "AOTLiveness.h"
#include "AOTObservableOperations.h"
#include "AOTRegionCloner.h"
#include "AOTResultRules.h"
#include "AOTRuleSupport.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTSummarizeForInlining.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct GeneratorBodyDescription {
    FunctionID body;
    Vector<ValueID, 8> entryArguments;
    unsigned generatorParameter { 0 };
    ValueID startAtCreation;
};

struct GeneratorStepSite {
    ValueID step;
    ResumeMode mode { ResumeMode::Next };
    ValueID sent;
};

struct FusionCandidate {
    ValueID generator;
    GeneratorFusionDeclineReason obstacle { GeneratorFusionDeclineReason::Unknown };
    GeneratorBodyDescription body;
    // The first site is body.startAtCreation: a CallDirect with one result, the value, and no sent value. Every other site is a StepGenerator.
    Vector<GeneratorStepSite, 4> sites;
};

struct CarriedValues {
    BitVector bodyValues;
    BitVector callerValues;
};

struct GeneratorLoopCut {
    ProgramPoint point;
    unsigned index { 0 };
    Vector<ValueID, 2> resultReads;
};

constexpr int64_t completed = -1;
constexpr size_t startSite = 0;

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

static Vector<ValueID, 4> listValues(FunctionAnalyses& analyses, Opcode opcode)
{
    Vector<ValueID, 4> values;
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID id : analyses.function().at(block).values) {
            if (analyses.function().at(id).opcode == opcode)
                values.append(id);
        }
    }
    return values;
}

static std::optional<GeneratorBodyDescription> describeGeneratorBody(const Program& program, const Function& caller, ValueID newGenerator)
{
    const Value& allocation = caller.at(newGenerator);
    RELEASE_ASSERT(allocation.opcode == Opcode::NewGenerator);

    const CreatorRecord::Coroutine* record = coroutineOf(program.at(allocation.function()));
    if (!record || record->kind != CoroutineKind::Generator || !record->promiseParameter)
        return std::nullopt;
    uint32_t generatorParameter = *record->promiseParameter;

    ValueID start;
    for (const Block& block : caller.blocks) {
        for (ValueID id : block.values) {
            const Value& value = caller.at(id);
            if (value.opcode != Opcode::CallDirect || value.function() != record->body)
                continue;
            std::span<const ValueID> arguments = value.argumentsOperands();
            if (generatorParameter >= arguments.size() || arguments[generatorParameter] != newGenerator)
                continue;
            if (start)
                return std::nullopt;
            start = id;
        }
    }
    if (!start)
        return std::nullopt;
    return GeneratorBodyDescription { record->body, Vector<ValueID, 8>(caller.at(start).argumentsOperands()), generatorParameter, start };
}

// FuseGeneratorLoops: whether the body can be merged into the caller.

template<typename Enumeration>
static Range rangeOf(std::initializer_list<Enumeration> values)
{
    Range range = Range::empty();
    for (Enumeration value : values)
        range = range.unionWith(Range::one(static_cast<int64_t>(value)));
    return range;
}

static constexpr int64_t resumePointAfter(unsigned yieldIndex)
{
    return static_cast<int64_t>(yieldIndex) + 1;
}

static bool isYield(const Function& function, ValueID id)
{
    return function.at(id).opcode == Opcode::YieldBySavingFrame;
}

static AnalysisContext contextOfBody(const FunctionPassContext& pass, const FusionCandidate& candidate)
{
    return { pass.build, pass.program, &pass.inputs.flow, &pass.inputs.effects, candidate.body.body };
}

static BitVector collectValuesLiveAcross(FunctionAnalyses& analyses, NOESCAPE const Invocable<bool(ValueID)> auto& isTransfer)
{
    const Function& function = analyses.function();
    const Liveness& liveness = analyses.liveness();
    BitVector liveAcross(function.values.size());
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        if (std::ranges::none_of(values, isTransfer))
            continue;
        Liveness::LocalCalc calc(liveness, block);
        for (uint32_t index = values.size(); index--;) {
            ValueID transfer = values[index];
            if (isTransfer(transfer)) {
                calc.forEachLive([&](ValueID live) {
                    if (live != transfer)
                        liveAcross.set(live.index());
                });
            }
            calc.execute(index);
        }
    }
    return liveAcross;
}

static CarriedValues collectCarriedValues(FunctionPassContext& pass, const FusionCandidate& candidate, FunctionAnalyses& bodyAnalyses)
{
    const Function& body = bodyAnalyses.function();
    CarriedValues carried;
    carried.bodyValues = collectValuesLiveAcross(bodyAnalyses, [&](ValueID id) { return isYield(body, id); });
    for (ValueID parameter : body.at(entryBlock).parameters)
        carried.bodyValues.clear(parameter.index());

    BitVector isStep(pass.function.values.size());
    for (const GeneratorStepSite& site : candidate.sites.subspan(startSite + 1))
        isStep.set(site.step.index());
    BitVector liveAcrossSteps = collectValuesLiveAcross(pass.analyses, [&](ValueID id) { return isStep.get(id.index()); });
    ProgramPoint start = pass.analyses.useLists().pointOf(candidate.body.startAtCreation);
    for (size_t index : liveAcrossSteps) {
        if (!pass.analyses.dominates(ValueID(index), start))
            carried.callerValues.set(index);
    }
    return carried;
}

static bool canBecomeParameter(FunctionAnalyses& analyses, ValueID id)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    if (numberOfResultsOf(program, function, function.at(id)))
        return false;
    std::optional<OpaqueKind> opaque = program.typeOf(function, id).opaque;
    if (opaque && (*opaque == OpaqueKind::Guard || mustBeDefinition(operandKindOf(*opaque))))
        return false;
    return std::ranges::none_of(analyses.useLists().usesOf(id), [&](const Use& use) {
        if (use.kind != Use::Kind::TerminatorOperand)
            return false;
        const Terminator& terminator = function.at(use.block).terminator;
        return terminator.isConditionalBranch() && terminator.targets.containsIf([](const Target& target) { return !target.proofs.isEmpty(); });
    });
}

// next() of a generator is in no stack trace: the step stands where the innermost frame that is in one called it.
static Origin visibleOriginOf(const Program& program, const Function& function, ValueID step)
{
    Origin origin = function.at(step).origin();
    while (origin.inlineFrame) {
        const InlineFrame& frame = function.inlineFrames[origin.inlineFrame];
        if (!frame.callee || !program.at(frame.callee).isHiddenInStackTraces())
            break;
        origin = { frame.callPosition, frame.parent, frame.isTailCall, frame.callIsPartOfConstruction };
    }
    return origin;
}

// In a stack trace the caller of the body stands at the step that runs it, not where the generator was made.
static Origin originOfSteps(const Program& program, const Function& function, const FusionCandidate& candidate)
{
    std::optional<Origin> ending;
    for (const GeneratorStepSite& site : candidate.sites.subspan(startSite + 1)) {
        if (site.mode != ResumeMode::Return)
            return visibleOriginOf(program, function, site.step);
        ending = visibleOriginOf(program, function, site.step);
    }
    return ending ? *ending : function.at(candidate.generator).origin();
}

// Only what throws or runs other code has its callers listed.
static bool mayHaveItsCallersListed(FunctionAnalyses& bodyAnalyses, const FusionCandidate& candidate)
{
    const Function& body = bodyAnalyses.function();
    bool isSentWhatToThrow = candidate.sites.containsIf([](const GeneratorStepSite& site) { return site.mode == ResumeMode::Throw; });
    auto listsCallers = [](const Effects& effects) { return effects.mayThrow || effects.mayCallUnknownCode; };
    // Behind a yield stands the throw of what a step may send in.
    auto throwsWhatStepSends = [&](const Terminator& terminator) {
        if (terminator.kind != TerminatorKind::Throw)
            return false;
        const Value& thrown = body.at(terminator.valueOperand());
        return thrown.opcode == Opcode::ResultAt && isYield(body, thrown.operand(0));
    };
    return std::ranges::any_of(bodyAnalyses.cfg().reversePostOrder(), [&](BlockID block) {
        if (listsCallers(bodyAnalyses.effectsOfTerminator(block).onAnyEdge()) && (isSentWhatToThrow || !throwsWhatStepSends(body.at(block).terminator)))
            return true;
        return std::ranges::any_of(body.at(block).values, [&](ValueID id) {
            return !isYield(body, id) && body.at(id).opcode != Opcode::LoopHint && listsCallers(bodyAnalyses.effectsOf(id).onAnyEdge());
        });
    });
}

static GeneratorFusionDeclineReason findMergeObstacle(FunctionPassContext& pass, const FusionCandidate& candidate, FunctionAnalyses& bodyAnalyses, const CarriedValues& carried)
{
    for (const GeneratorStepSite& site : candidate.sites.subspan(startSite + 1)) {
        if (!pass.analyses.dominates(candidate.body.startAtCreation, pass.analyses.useLists().pointOf(site.step)))
            return GeneratorFusionDeclineReason::Unknown;
    }
    Origin origin = originOfSteps(pass.program, pass.function, candidate);
    // A step that returns runs code of the body only where a yield stands in a try statement.
    const Function& body = bodyAnalyses.function();
    bool returnStepRunsBody = std::ranges::any_of(bodyAnalyses.cfg().reversePostOrder(), [&](BlockID block) {
        return body.at(block).handler && std::ranges::any_of(body.at(block).values, [&](ValueID id) { return isYield(body, id); });
    });
    bool isRunFromOnePlace = std::ranges::all_of(candidate.sites.subspan(startSite + 1), [&](const GeneratorStepSite& site) {
        return (site.mode == ResumeMode::Return && !returnStepRunsBody) || visibleOriginOf(pass.program, pass.function, site.step) == origin;
    });
    if (!isRunFromOnePlace && mayHaveItsCallersListed(bodyAnalyses, candidate))
        return GeneratorFusionDeclineReason::Unknown;
    for (size_t index : carried.bodyValues) {
        if (!isYield(bodyAnalyses.function(), ValueID(index)) && !canBecomeParameter(bodyAnalyses, ValueID(index)))
            return GeneratorFusionDeclineReason::BodyIsBoundToFrame;
    }
    for (size_t index : carried.callerValues) {
        if (!canBecomeParameter(pass.analyses, ValueID(index)) && !candidate.sites.containsIf([&](const GeneratorStepSite& site) { return site.step == ValueID(index); }))
            return GeneratorFusionDeclineReason::Unknown;
    }
    return GeneratorFusionDeclineReason::None;
}

// FuseGeneratorLoops: the merge.

static Vector<GeneratorLoopCut, 4> findCuts(FunctionAnalyses& analyses, std::span<const ValueID> transfers)
{
    const Function& function = analyses.function();
    const UseLists& useLists = analyses.useLists();
    Vector<GeneratorLoopCut, 4> cuts;
    for (unsigned index = 0; index < transfers.size(); ++index) {
        GeneratorLoopCut cut { useLists.pointOf(transfers[index]), index, { } };
        for (const Use& use : useLists.usesOf(transfers[index])) {
            if (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::ResultAt)
                cut.resultReads.append(use.user);
        }
        cuts.append(WTF::move(cut));
    }
    AOT::sort(cuts, std::ranges::greater { }, [](const GeneratorLoopCut& cut) { return std::tuple(cut.point.block, cut.point.index); });
    return cuts;
}

// The run that begins at the creation ends at the initial yield. A yield that no later run comes to always returns behind the creation.
static BitVector findYieldsOnlyFirstRunReaches(FunctionAnalyses& bodyAnalyses, std::span<const ValueID> yields)
{
    const CFG& cfg = bodyAnalyses.cfg();
    const UseLists& useLists = bodyAnalyses.useLists();
    BitVector enteredByLaterRuns;
    Vector<BlockID, 16> worklist;
    for (ValueID yield : yields)
        worklist.append(useLists.pointOf(yield).block);
    while (!worklist.isEmpty()) {
        for (BlockID successor : cfg.successors(worklist.takeLast())) {
            if (!enteredByLaterRuns.set(successor.index()))
                worklist.append(successor);
        }
    }

    BitVector result;
    for (unsigned index = 0; index < yields.size(); ++index) {
        ProgramPoint point = useLists.pointOf(yields[index]);
        bool followsYieldInBlock = std::ranges::any_of(yields, [&](ValueID other) {
            ProgramPoint pointOfOther = useLists.pointOf(other);
            return pointOfOther.block == point.block && pointOfOther.index < point.index;
        });
        if (!followsYieldInBlock && !enteredByLaterRuns.get(point.block.index()))
            result.set(index);
    }
    return result;
}

static bool firstRunAlwaysEndsAt(FunctionAnalyses& bodyAnalyses, std::span<const ValueID> yields, const BitVector& yieldsOfFirstRun)
{
    const Function& body = bodyAnalyses.function();
    const CFG& cfg = bodyAnalyses.cfg();
    const UseLists& useLists = bodyAnalyses.useLists();
    auto findFirstYieldIn = [&](BlockID block) {
        std::optional<unsigned> first;
        for (unsigned index = 0; index < yields.size(); ++index) {
            ProgramPoint point = useLists.pointOf(yields[index]);
            if (point.block == block && (!first || point.index < useLists.pointOf(yields[*first]).index))
                first = index;
        }
        return first;
    };

    BitVector visited;
    Vector<BlockID, 16> worklist { entryBlock };
    visited.set(entryBlock.index());
    while (!worklist.isEmpty()) {
        BlockID block = worklist.takeLast();
        if (std::optional<unsigned> yield = findFirstYieldIn(block)) {
            if (!yieldsOfFirstRun.get(*yield))
                return false;
            if (body.at(block).handler && !visited.set(body.at(block).handler->block.index()))
                worklist.append(body.at(block).handler->block);
            continue;
        }
        const Terminator& terminator = body.at(block).terminator;
        if (terminator.kind == TerminatorKind::Return || terminator.kind == TerminatorKind::ReturnCall)
            return false;
        // The first run finds a generator that is not running yet.
        bool asksWhetherRunning = terminator.kind == TerminatorKind::Branch && body.at(terminator.conditionOperand()).opcode == Opcode::IsGeneratorRunning;
        BlockID skipped = asksWhetherRunning && terminator.targets[0].block != terminator.targets[1].block ? terminator.targets[0].block : BlockID();
        for (BlockID successor : cfg.successors(block)) {
            if (successor != skipped && !visited.set(successor.index()))
                worklist.append(successor);
        }
    }
    return true;
}

static void fuseGenerator(FunctionPassContext& pass, IRBuilder& builder, const FusionCandidate& candidate)
{
    RELEASE_ASSERT(candidate.sites[startSite].step == candidate.body.startAtCreation);
    const Program& program = pass.program;
    Function& function = pass.function;
    FunctionAnalyses& analyses = pass.analyses;
    const Function& body = program.at(candidate.body.body);
    FunctionAnalyses bodyAnalyses(contextOfBody(pass, candidate), body);
    CarriedValues carried = collectCarriedValues(pass, candidate, bodyAnalyses);
    RELEASE_ASSERT(findMergeObstacle(pass, candidate, bodyAnalyses, carried) == GeneratorFusionDeclineReason::None);
    Vector<ValueID, 4> yields = listValues(bodyAnalyses, Opcode::YieldBySavingFrame);
    BitVector yieldsOfFirstRun = findYieldsOnlyFirstRunReaches(bodyAnalyses, yields.span());
    bool firstRunSkipsLeave = candidate.sites.size() > 1 && firstRunAlwaysEndsAt(bodyAnalyses, yields.span(), yieldsOfFirstRun);
    SSARepair repair(builder, analyses);

    // Where the generator was made, the two variables of its state: where the body goes on, and which site it returns to.
    ProgramPoint pointOfSeeds = analyses.useLists().pointAfter(candidate.generator);
    Origin origin = function.at(candidate.generator).origin();
    Origin originOfBody = originOfSteps(program, function, candidate);
    Expected<BlockFrequency> frequency = function.at(pointOfSeeds.block).frequency;
    builder.setOrigin(origin);
    ValueID resumePoint = builder.insertBefore(pointOfSeeds, makeInteger(0));
    ValueID returnSite = builder.insertBefore(pointOfSeeds, makeInteger(0));
    builder.applyInsertions();
    SSARepair::Variable resumePointVariable = repair.addVariable(resumePoint);
    SSARepair::Variable returnSiteVariable = repair.addVariable(returnSite);

    builder.setHandler(std::nullopt);
    BlockID start = builder.addBlock(frequency);
    BlockID enter = builder.addBlock(frequency);
    ValueID sent = builder.addParameter(enter, TypeTable::anything);
    ValueID mode = builder.addParameter(enter, pass.types.intern(Type::integer(rangeOf({ ResumeMode::Next, ResumeMode::Throw, ResumeMode::Return }))));
    BlockID leave = builder.addBlock(frequency);
    ValueID value = builder.addParameter(leave, TypeTable::anything);
    ValueID status = builder.addParameter(leave, pass.types.intern(Type::integer(rangeOf({ GeneratorStepStatus::Yielded, GeneratorStepStatus::Completed }))));

    auto defineResumePoint = [&](int64_t point) {
        repair.addDefinition(resumePointVariable, builder.appendInteger(point));
    };
    auto emitJumpToLeave = [&](ValueID result, GeneratorStepStatus statusOfResult) {
        builder.emitJump(leave, { result, builder.appendInteger(static_cast<int64_t>(statusOfResult)) });
    };
    auto emitSwitchOnReturnSite = [&](Vector<Target, 4>&& targets, size_t firstSite = startSite) {
        Target last = targets.takeLast();
        if (targets.size() <= firstSite) {
            builder.emitJump(last.block, WTF::move(last.arguments));
            return;
        }
        Vector<std::pair<int64_t, Target>> cases;
        cases.reserveInitialCapacity(targets.size() - firstSite);
        for (size_t site = firstSite; site < targets.size(); ++site)
            cases.append({ static_cast<int64_t>(site), WTF::move(targets[site]) });
        builder.emitSwitch(returnSite, WTF::move(cases), WTF::move(last));
    };
    auto beginCut = [&](const GeneratorLoopCut& cut) {
        if (!numberOfResultsOf(program, function, function.at(function.at(cut.point.block).values[cut.point.index])))
            return builder.beginExpansion(cut.point);
        // The overload without reads erases them wherever they stand, which moves the points of cuts that are still to come.
        return builder.beginExpansion(cut.point, cut.resultReads.span());
    };

    Vector<ValueID, 4> steps(candidate.sites.size(), [&](size_t site) { return candidate.sites[site].step; });
    Vector<IRBuilder::Expansion, 4> siteCuts;
    siteCuts.grow(steps.size());
    Vector<GeneratorLoopCut, 4> cuts = findCuts(analyses, steps.span());
    for (size_t position = 0; position < cuts.size(); ++position) {
        const GeneratorLoopCut& cut = cuts[position];
        // The next cut, if it is in the same block, leaves only what follows it in front of this site.
        bool isAfterCutInBlock = position + 1 < cuts.size() && cuts[position + 1].point.block == cut.point.block;
        uint32_t firstIndexOfHead = isAfterCutInBlock ? cuts[position + 1].point.index + 1 : 0;
        // The copy of the body runs in its place. If the copy never returns, nothing remains of the site.
        explainOperationThatGoes(function, steps[cut.index], InPlaceOfOperation::ItsExpansion);
        siteCuts[cut.index] = beginCut(cut);
        // Every cycle the merge creates passes through the block in front of a site. A hint in enter or leave would keep jumps from being threaded through them.
        if (std::ranges::none_of(function.at(builder.currentBlock()).values.subspan(firstIndexOfHead), [&](ValueID id) { return function.at(id).opcode == Opcode::LoopHint; }))
            builder.append(Opcode::LoopHint, { });
        repair.addDefinition(returnSiteVariable, builder.appendInteger(static_cast<int64_t>(cut.index)));
        if (cut.index == startSite) {
            builder.emitJump(start);
            continue;
        }
        const GeneratorStepSite& step = candidate.sites[cut.index];
        ValueID sentBySite = step.sent ? step.sent : builder.appendUndefined();
        builder.emitJump(enter, { sentBySite, builder.appendInteger(static_cast<int64_t>(step.mode)) });
    }

    // What the body throws completes the generator and goes on to the handler of the site that runs it.
    builder.setOrigin(origin);
    builder.setHandler(std::nullopt);
    BlockID failed = builder.addHandlerBlock(BlockRole::CleanupHandler, ValueID());
    builder.moveToEndOf(failed);
    ValueID exception = builder.append(Opcode::CatchException, { });
    defineResumePoint(completed);
    TypeID exceptionType = function.at(exception).type;
    Vector<Target, 4> rethrowTargets;
    Vector<std::pair<BlockID, ValueID>, 4> rethrows;
    for (const IRBuilder::Expansion& cut : siteCuts) {
        builder.setHandler(cut.handler);
        BlockID block = builder.addBlock(BlockFrequency::Rare);
        rethrows.append({ block, builder.addParameter(block, exceptionType) });
        rethrowTargets.append(Target { block, { exception }, { } });
    }
    builder.setHandler(std::nullopt);
    emitSwitchOnReturnSite(WTF::move(rethrowTargets));
    for (auto [block, rethrown] : rethrows) {
        builder.moveToEndOf(block);
        builder.emitRethrow(rethrown);
    }

    RegionCloner cloner(builder, body);
    uint32_t firstFrame = appendInlineFrames(function, InlineFrame { .callee = candidate.body.body, .callPosition = originOfBody.position, .parent = originOfBody.inlineFrame, .isTailCall = originOfBody.isTailCall, .module = body.module }, body);
    builder.setOrigin(origin);
    builder.moveToEndOf(start);
    const auto& parameters = body.at(entryBlock).parameters;
    RELEASE_ASSERT(parameters.size() == candidate.body.entryArguments.size());
    for (unsigned index = 0; index < parameters.size(); ++index)
        cloner.mapValue(parameters[index], index == candidate.body.generatorParameter ? candidate.generator : upcastToParameterType(builder, body.at(parameters[index]), candidate.body.entryArguments[index]));

    builder.setHandler(Target { failed, { }, { } });
    cloneBlocksIntoInlineFrame(builder, cloner, bodyAnalyses.cfg().reversePostOrder(), firstFrame);
    builder.setHandler(std::nullopt);
    builder.emitJump(cloner.cloneOf(entryBlock));
    for (BlockID clone : cloner.clonedBlocks()) {
        function.edit(clone, BlockEdit::Rest).frequency = rarerOf(function.at(clone).frequency, frequency);
        // The first site is the start at the creation, which runs up to the initial yield.
        for (ValueID id : function.at(clone).values) {
            Value& copy = function.edit(id);
            if (copy.opcode != Opcode::IsGeneratorRunning)
                continue;
            copy.become(Opcode::Boolean);
            copy.setBoolean(false);
        }
    }
    analyses.didChangeControlFlow();

    // A cut moves the terminator of its block into a block that clonedBlocks() does not list, so the returns come first.
    for (BlockID block : cloner.clonedBlocks()) {
        if (function.at(block).terminator.kind == TerminatorKind::ReturnCall)
            builder.demoteReturnCall(block);
        if (function.at(block).terminator.kind != TerminatorKind::Return)
            continue;
        Terminator returned = std::exchange(function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, Terminator());
        analyses.didChangeControlFlow();
        builder.setOrigin(returned.origin());
        builder.moveToEndOf(block);
        ValueID result = returned.valueOperand() ? returned.valueOperand() : builder.appendUndefined();
        defineResumePoint(completed);
        emitJumpToLeave(result, GeneratorStepStatus::Completed);
    }

    Vector<ValueID, 4> copiesOfYields(yields.size(), [&](size_t index) { return cloner.cloneOf(yields[index]); });
    Vector<IRBuilder::Expansion, 4> yieldCuts;
    yieldCuts.grow(copiesOfYields.size());
    for (const GeneratorLoopCut& cut : findCuts(analyses, copiesOfYields.span())) {
        ValueID yielded = function.at(copiesOfYields[cut.index]).valueOperand();
        yieldCuts[cut.index] = beginCut(cut);
        defineResumePoint(resumePointAfter(cut.index));
        if (firstRunSkipsLeave && yieldsOfFirstRun.get(cut.index))
            builder.emitJump(siteCuts[startSite].continuation, { yielded });
        else
            emitJumpToLeave(yielded, GeneratorStepStatus::Yielded);
    }

    builder.setOrigin(origin);
    Expected<BlockFrequency> frequencyWhenFinished = rarerOf(frequency, BlockFrequency::Unlikely);
    builder.setHandler(std::nullopt);
    BlockID finished = builder.addBlock(frequencyWhenFinished);
    BlockID stepped = builder.addBlock(frequencyWhenFinished);
    BlockID returned = builder.addBlock(frequencyWhenFinished);
    builder.setHandler(Target { failed, { }, { } });
    BlockID thrown = builder.addBlock(BlockFrequency::Rare);
    builder.setHandler(std::nullopt);

    builder.moveToEndOf(finished);
    builder.emitSwitch(mode, { { static_cast<int64_t>(ResumeMode::Throw), Target { thrown, { }, { } } }, { static_cast<int64_t>(ResumeMode::Return), Target { returned, { }, { } } } }, Target { stepped, { }, { } });
    builder.moveToEndOf(stepped);
    emitJumpToLeave(builder.appendUndefined(), GeneratorStepStatus::Completed);
    builder.moveToEndOf(returned);
    emitJumpToLeave(sent, GeneratorStepStatus::Completed);
    builder.moveToEndOf(thrown);
    builder.emitThrow(sent);

    // enter goes on behind the yield that the body stopped at, leave behind the site that runs it.
    std::array<ValueID, numberOfEnumerators<YieldResult>> resultsOfYield;
    resultsOfYield[static_cast<size_t>(YieldResult::Sent)] = sent;
    resultsOfYield[static_cast<size_t>(YieldResult::Mode)] = mode;
    Vector<std::pair<int64_t, Target>> resumeCases;
    resumeCases.reserveInitialCapacity(yieldCuts.size());
    for (unsigned index = 0; index < yieldCuts.size(); ++index)
        resumeCases.append({ resumePointAfter(index), Target { yieldCuts[index].continuation, std::span<const ValueID>(resultsOfYield), { } } });
    builder.moveToEndOf(enter);
    if (resumeCases.isEmpty())
        builder.emitJump(finished);
    else
        builder.emitSwitch(resumePoint, WTF::move(resumeCases), Target { finished, { }, { } });

    std::array<ValueID, numberOfEnumerators<StepGeneratorResult>> resultsOfStep;
    resultsOfStep[static_cast<size_t>(StepGeneratorResult::Value)] = value;
    resultsOfStep[static_cast<size_t>(StepGeneratorResult::Status)] = status;
    Vector<Target, 4> continuations;
    for (size_t site = 0; site < siteCuts.size(); ++site)
        continuations.append(Target { siteCuts[site].continuation, site == startSite ? Arguments { value } : Arguments(std::span<const ValueID>(resultsOfStep)), { } });
    builder.setOrigin(origin);
    builder.moveToEndOf(leave);
    emitSwitchOnReturnSite(WTF::move(continuations), firstRunSkipsLeave ? startSite + 1 : startSite);

    // What lives across a yield or a step has one definition on each way round.
    Vector<ValueID> definitions;
    BitVector isListed(function.values.size());
    auto add = [&](ValueID definition) {
        if (!numberOfResultsOf(program, function, function.at(definition)) && !isListed.set(definition.index()))
            definitions.append(definition);
    };
    for (unsigned index = 0; index < yields.size(); ++index) {
        if (carried.bodyValues.get(yields[index].index()))
            std::ranges::for_each(yieldCuts[index].results, add);
    }
    for (size_t index : carried.bodyValues)
        add(cloner.cloneOf(ValueID(index)));
    for (size_t site = 0; site < siteCuts.size(); ++site) {
        if (carried.callerValues.get(candidate.sites[site].step.index()))
            std::ranges::for_each(siteCuts[site].results, add);
    }
    for (size_t index : carried.callerValues)
        add(ValueID(index));

    pointOfSeeds = analyses.useLists().pointAfter(candidate.generator);
    builder.setOrigin(origin);
    Vector<ValueID> seeds;
    seeds.reserveInitialCapacity(definitions.size());
    for (ValueID id : definitions) {
        const Value& definition = function.at(id);
        Value seed = isRematerializable(definition) ? definition : IRBuilder::make(Opcode::Unspecified);
        if (!isRematerializable(definition)) {
            seed.setValueType(definition.type);
            seed.setExpected(definition.expected());
        }
        seeds.append(builder.insertBefore(pointOfSeeds, WTF::move(seed)));
    }
    builder.applyInsertions();
    for (size_t index = 0; index < definitions.size(); ++index)
        repair.addDefinition(repair.addVariable(seeds[index]), definitions[index]);
    repair.run();
    builder.setHandler(std::nullopt);
}

// FuseGeneratorLoops: the search.

static BitVector findBlocksAfterImpossibleStatus(FunctionPassContext& pass, std::span<const ValueID> steps)
{
    const Function& function = pass.function;
    const UseLists& useLists = pass.analyses.useLists();
    auto isComparisonWithNotStepped = [&](const Use& use) {
        const Value& user = function.at(use.user);
        if (user.opcode != Opcode::StrictEq)
            return false;
        const Value& other = function.at(use.operandIndex == OperandIndex::StrictEq::left ? user.rightOperand() : user.leftOperand());
        return other.opcode == Opcode::Integer && other.integer() == static_cast<int64_t>(GeneratorStepStatus::NotStepped);
    };

    Vector<Edge, 4> impossibleEdges;
    for (ValueID step : steps) {
        for (const Use& read : useLists.usesOf(step)) {
            if (read.kind != Use::Kind::Operand || function.at(read.user).opcode != Opcode::ResultAt || function.at(read.user).index() != static_cast<uint32_t>(StepGeneratorResult::Status))
                continue;
            for (const Use& comparison : useLists.usesOf(read.user)) {
                if (comparison.kind != Use::Kind::Operand || !isComparisonWithNotStepped(comparison))
                    continue;
                for (const Use& use : useLists.usesOf(comparison.user)) {
                    if (use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.kind == TerminatorKind::Branch)
                        impossibleEdges.append(Edge { use.block, 0 });
                }
            }
        }
    }

    BitVector blocks;
    if (impossibleEdges.isEmpty())
        return blocks;
    blocks.ensureSize(function.blocks.size());
    for (BlockID block : pass.analyses.cfg().reversePostOrder()) {
        if (std::ranges::any_of(impossibleEdges, [&](Edge edge) { return pass.analyses.dominates(edge, block); }))
            blocks.quickSet(block.index());
    }
    return blocks;
}

static bool isGeneratorOperandOfSite(const FusionCandidate& candidate, const Use& use)
{
    if (use.kind != Use::Kind::Operand)
        return false;
    if (use.user == candidate.body.startAtCreation)
        return use.operandIndex == OperandIndex::CallDirect::arguments + candidate.body.generatorParameter;
    return use.operandIndex == OperandIndex::StepGenerator::generator && candidate.sites.containsIf([&](const GeneratorStepSite& site) { return site.step == use.user; });
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

static bool canLeaveFrame(const Function& body, const Block& block, ValueID generatorParameter)
{
    bool canLeave = boundToOf(block.terminator.kind) != BoundTo::Frame;
    for (uint32_t index = 0; index < block.values.size(); ++index) {
        const Value& value = body.at(block.values[index]);
        canLeave &= value.opcode == Opcode::YieldBySavingFrame || !isBoundToFrame(body, block, index);
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (operand == generatorParameter)
                canLeave &= value.opcode == Opcode::IsGeneratorRunning || (value.opcode == Opcode::YieldBySavingFrame && operandIndex == OperandIndex::YieldBySavingFrame::generator);
        });
    }
    block.forEachChildOutsideValues([&](ValueID child) {
        canLeave &= !child || child != generatorParameter;
    });
    return canLeave;
}

static GeneratorFusionDeclineReason findObstacle(FunctionPassContext& pass, const EscapeAnalysis& escape, std::span<const ValueID> allSteps, uint64_t inlinedBytes, FusionCandidate& candidate)
{
    const Function& function = pass.function;
    ValueID generator = candidate.generator;
    std::optional<ObjectID> object = escape.objectDefinedBy(generator);
    if (!object)
        return GeneratorFusionDeclineReason::Unknown;

    Vector<ValueID, 4> uncheckedSteps;
    for (ValueID step : allSteps) {
        if (function.at(step).check() == GeneratorStepCheck::None && escape.singleObject(function.at(step).generatorOperand()) == *object)
            uncheckedSteps.append(step);
    }
    BitVector ignoredBlocks = findBlocksAfterImpossibleStatus(pass, uncheckedSteps.span());
    const UseLists& useLists = pass.analyses.useLists();
    Vector<ValueID, 4> steps;
    for (ValueID step : allSteps) {
        if (!ignoredBlocks.get(useLists.blockOf(step).index()) && escape.pointsTo(function.at(step).generatorOperand()).objects.contains(*object))
            steps.append(step);
    }
    if (steps.isEmpty())
        return GeneratorFusionDeclineReason::HasNoStep;

    std::optional<GeneratorBodyDescription> description = describeGeneratorBody(pass.program, function, generator);
    if (!description || useLists.locationOf(description->startAtCreation).placement != ValuePlacement::InBlock)
        return GeneratorFusionDeclineReason::BodyIsNotDescribed;
    candidate.body = WTF::move(*description);
    if (escape.liveInstancesOf(*object) != LiveInstances::AtMostOne)
        return GeneratorFusionDeclineReason::SeveralLiveInstances;

    candidate.sites.append(GeneratorStepSite { candidate.body.startAtCreation, ResumeMode::Next, ValueID() });
    for (ValueID step : steps) {
        const Value& value = function.at(step);
        if (escape.singleObject(value.generatorOperand()) != *object)
            return GeneratorFusionDeclineReason::StepMayReachOtherGenerator;
        candidate.sites.append(GeneratorStepSite { step, value.resumeMode(), value.valueOperand() });
    }
    if (std::ranges::any_of(steps, [&](ValueID step) { return function.at(step).check() != GeneratorStepCheck::None; }))
        return GeneratorFusionDeclineReason::MayEscapeBeforeStep;

    for (const ObjectUse& objectUse : escape.usesOf(*object)) {
        const Use& use = objectUse.use;
        if (ignoredBlocks.get(use.block.index()) || isGeneratorOperandOfSite(candidate, use))
            continue;
        if (!objectUse.isEscape && !objectUse.isContentsStore && !readsGeneratorState(pass.analyses, use))
            continue;
        if (!pass.build.isSabotaged(Sabotage::FusedGeneratorsAreSeenByOthers))
            return GeneratorFusionDeclineReason::MayEscapeBeforeStep;
    }

    const Function& body = pass.program.at(candidate.body.body);
    if (!body.hasBody() || body.at(entryBlock).parameters.size() != candidate.body.entryArguments.size())
        return GeneratorFusionDeclineReason::BodyIsNotDescribed;
    ValueID generatorParameter = body.at(entryBlock).parameters[candidate.body.generatorParameter];
    if (!std::ranges::all_of(body.blocks, [&](const Block& block) { return canLeaveFrame(body, block, generatorParameter); }))
        return GeneratorFusionDeclineReason::BodyIsBoundToFrame;

    uint64_t bodyBytes = 0;
    for (const Block& block : body.blocks)
        bodyBytes += estimatedCodeBytes(pass.program, body, block);
    if (pass.exceeds(LimitID::MaxFusedGeneratorBytes, bodyBytes, generator))
        return GeneratorFusionDeclineReason::BodyIsTooLarge;
    if (pass.exceeds(LimitID::MaxInlinedCallerBytes, inlinedBytes + bodyBytes, generator) || pass.exceeds(LimitID::InlineFramesPerFunction, inlineFrameCountOf(function) + inlineFrameCountOf(body), generator))
        return GeneratorFusionDeclineReason::CallerIsTooLarge;

    uint32_t inlineFrame = function.at(generator).inlineFrame;
    bool isInsideBody = pass.id == candidate.body.body;
    for (uint32_t frame = inlineFrame; frame; frame = function.inlineFrames[frame].parent)
        isInsideBody |= function.inlineFrames[frame].callee == candidate.body.body;
    if (isInsideBody || pass.exceeds(LimitID::MaxInlineDepth, inlineDepthOf(pass.program, function, inlineFrame) + 1, generator))
        return GeneratorFusionDeclineReason::InlineDepth;

    FunctionAnalyses bodyAnalyses(contextOfBody(pass, candidate), body);
    return findMergeObstacle(pass, candidate, bodyAnalyses, collectCarriedValues(pass, candidate, bodyAnalyses));
}

static Vector<FusionCandidate> findFusionCandidates(FunctionPassContext& pass)
{
    Vector<ValueID, 4> steps = listValues(pass.analyses, Opcode::StepGenerator);
    GeneratorFusionDeclineReason initialObstacle = steps.isEmpty() ? GeneratorFusionDeclineReason::HasNoStep : GeneratorFusionDeclineReason::Unknown;
    Vector<FusionCandidate> candidates = listValues(pass.analyses, Opcode::NewGenerator).map([&](ValueID generator) {
        return FusionCandidate { .generator = generator, .obstacle = initialObstacle };
    });
    if (candidates.isEmpty() || steps.isEmpty())
        return candidates;

    const EscapeAnalysis& escape = pass.analyses.escapeWithLiveInstances();
    if (escape.gaveUp())
        return candidates;

    // The limit on the caller is on what has been added to it. A caller that is large by itself takes in a body like any other.
    uint64_t inlinedBytes = 0;
    for (const Block& block : pass.function.blocks) {
        for (ValueID id : block.values) {
            if (pass.function.at(id).inlineFrame)
                inlinedBytes += estimatedCodeBytes(pass.program, pass.function, pass.function.at(id));
        }
    }
    for (FusionCandidate& candidate : candidates)
        candidate.obstacle = findObstacle(pass, escape, steps.span(), inlinedBytes, candidate);
    return candidates;
}

static LimitID limitOf(GeneratorFusionDeclineReason obstacle)
{
    switch (obstacle) {
    case GeneratorFusionDeclineReason::BodyIsTooLarge:
        return LimitID::MaxFusedGeneratorBytes;
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

PassOutcome fuseGeneratorLoops(FunctionPassContext& pass)
{
    if (!hasListedGenerator(pass.function))
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    PassOutcome outcome = PassOutcome::Unchanged;
    for (;;) {
        Vector<FusionCandidate> candidates = findFusionCandidates(pass);
        auto fusible = std::ranges::find(candidates, GeneratorFusionDeclineReason::None, &FusionCandidate::obstacle);
        if (fusible != candidates.end()) {
            fuseGenerator(pass, builder, *fusible);
            pass.build.statistics().count(StatisticID::GeneratorLoopsFused);
            outcome = PassOutcome::Changed;
            continue;
        }
        for (const FusionCandidate& candidate : candidates) {
            if (candidate.obstacle != GeneratorFusionDeclineReason::HasNoStep)
                pass.addDecline(Wire::DeclineKind::GeneratorLoopNotFused, candidate.generator, candidate.obstacle, limitOf(candidate.obstacle));
        }
        // The analyses were told of every edit, and the search that found no more candidates came behind the last one.
        if (outcome == PassOutcome::Changed)
            pass.keptAnalyses = allAnalyses;
        return outcome;
    }
}

// StartGeneratorsAtFirstStep.

// A step at the head of a loop is the first step exactly when the loop is entered. Returns the entering edges, or nothing if an edge may arrive either way.
static Vector<Edge, 2> findEdgesToFirstStep(FunctionAnalyses& analyses, const Function& function, ValueID step, BlockID creation, std::optional<Edge> edgeToProtocol)
{
    ValueLocation location = analyses.useLists().locationOf(step);
    const Block& block = function.at(location.block);
    const CFG& cfg = analyses.cfg();
    // The entering edges get a copy of everything before the step.
    if (numberOfSharedExplicitParameters(function, cfg, location.block) != block.parameters.size())
        return { };
    for (ValueID id : block.values.span().first(location.index)) {
        if (function.at(id).opcode != Opcode::LoopHint && (function.at(id).numberOfOperands() || !analyses.hasNoEffects(id)))
            return { };
    }

    BitVector barrier;
    barrier.set(creation.index());
    if (edgeToProtocol && cfg.blocksReachableFrom(targetOf(function, *edgeToProtocol).block, barrier).get(location.block.index()))
        return { };
    BitVector blocksAfterStep = cfg.blocksReachableFrom(location.block, barrier);
    Vector<Edge, 2> enteringEdges;
    for (Edge edge : cfg.incomingEdges(location.block)) {
        if (analyses.dominates(step, pointBeforeTerminatorOf(function, edge.from)))
            continue;
        if (edge.isHandlerEdge() || blocksAfterStep.get(edge.from.index()))
            return { };
        enteringEdges.append(edge);
    }
    return enteringEdges;
}

static PassOutcome startAtFirstStep(FunctionPassContext& pass, IRBuilder& builder, const EscapeAnalysis& escape, ValueID generator)
{
    Function& function = pass.function;
    FunctionAnalyses& analyses = pass.analyses;
    std::optional<ObjectID> object = escape.objectDefinedBy(generator);
    if (!object || escape.liveInstancesOf(*object) != LiveInstances::AtMostOne)
        return PassOutcome::Unchanged;
    std::optional<GeneratorBodyDescription> description = describeGeneratorBody(pass.program, function, generator);
    if (!description)
        return PassOutcome::Unchanged;
    // The call that starts a generator at its creation runs the head of the body, which evaluates the parameters, up to the initial yield. Nothing tells when a head without an effect has run, and only behind such a head does a body ask whether it is running.
    const Function& body = pass.program.at(description->body);
    bool suspendsBeforeAnyEffect = body.hasBody() && std::ranges::any_of(body.at(entryBlock).values, [&, driver = body.at(entryBlock).parameters[description->generatorParameter]](ValueID id) {
        return body.at(id).opcode == Opcode::IsGeneratorRunning && body.at(id).generatorOperand() == driver;
    });
    if (!suspendsBeforeAnyEffect)
        return PassOutcome::Unchanged;

    const UseLists& useLists = analyses.useLists();
    ValueID startAtCreation = description->startAtCreation;
    ValueLocation location = useLists.locationOf(startAtCreation);
    if (location.placement != ValuePlacement::InBlock || useLists.useCount(startAtCreation))
        return PassOutcome::Unchanged;
    // The edge on which the generator has failed the test for an unmodified one. Behind it anything may look at the generator, so it is started there as at its creation.
    std::optional<Edge> edgeToProtocol;
    if (const Terminator& terminator = function.at(location.block).terminator; terminator.kind == TerminatorKind::Branch) {
        const Value& test = function.at(terminator.conditionOperand());
        if (test.opcode == Opcode::IsUnmodifiedGenerator && analyses.sameObjectThroughCasts(test.valueOperand()) == generator)
            edgeToProtocol = Edge { location.block, 1 };
    }
    auto isBehindFailedTest = [&](BlockID block) { return edgeToProtocol && analyses.dominates(*edgeToProtocol, block); };

    Vector<ValueID, 2> steps;
    for (const ObjectUse& objectUse : escape.usesOf(*object)) {
        const Use& use = objectUse.use;
        if (use.kind != Use::Kind::Operand || isBehindFailedTest(use.block))
            continue;
        const Value& user = function.at(use.user);
        if (user.opcode != Opcode::StepGenerator || user.resumeMode() != ResumeMode::Next || use.operandIndex != OperandIndex::StepGenerator::generator)
            continue;
        // What the body throws in its first run leaves the generator as running. Nobody sees that where the exception leaves the function.
        if (user.check() != GeneratorStepCheck::None || escape.singleObject(user.generatorOperand()) != *object || (function.at(use.block).handler && !pass.build.isSabotaged(Sabotage::FirstStepsStandUnderHandlers)))
            return PassOutcome::Unchanged;
        if (!analyses.dominates(startAtCreation, useLists.pointOf(use.user)))
            return PassOutcome::Unchanged;
        steps.append(use.user);
    }
    // A step behind another step is never the first.
    Vector<ValueID, 2> firstSteps;
    for (ValueID step : steps) {
        if (!steps.containsIf([&](ValueID other) { return other != step && analyses.dominates(other, useLists.pointOf(step)); }))
            firstSteps.append(step);
    }
    if (firstSteps.isEmpty())
        return PassOutcome::Unchanged;

    // Every other reader finds a generator that has taken a step.
    for (const ObjectUse& objectUse : escape.usesOf(*object)) {
        const Use& use = objectUse.use;
        if (isBehindFailedTest(use.block) || (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::IsUnmodifiedGenerator))
            continue;
        if (use.kind == Use::Kind::Operand && (use.user == startAtCreation || firstSteps.contains(use.user)))
            continue;
        bool isBehindStep = firstSteps.containsIf([&](ValueID step) { return analyses.dominates(step, use); });
        if (!isBehindStep && !pass.build.isSabotaged(Sabotage::GeneratorsAreReadBeforeTheirFirstStep))
            return PassOutcome::Unchanged;
    }

    Value start(function.at(startAtCreation));
    // One step outside of every loop is the first step whenever it runs.
    bool isKnownToBeFirst = firstSteps.size() == 1 && (!analyses.isInCycle(analyses.useLists().blockOf(firstSteps[0])) || pass.build.isSabotaged(Sabotage::StepsInCyclesAreFirstSteps));
    // A step that may be the first takes what the body is started with. A frame that is saved in front of it would hold all of that though the generator has long started.
    bool savesFrame = std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) { return maySuspend(function.at(id).opcode); });
    });
    // The edges that enter a loop start the body themselves, so what it is started with is not live in the loop.
    Vector<Edge, 2> enteringEdges;
    if (!isKnownToBeFirst && firstSteps.size() == 1)
        enteringEdges = findEdgesToFirstStep(analyses, function, firstSteps[0], location.block, edgeToProtocol);
    if (!isKnownToBeFirst && enteringEdges.isEmpty() && savesFrame)
        return PassOutcome::Unchanged;

    BitVector erased;
    erased.set(location.index);
    builder.eraseValues(location.block, erased);
    if (edgeToProtocol) {
        BlockID added = builder.splitEdge(*edgeToProtocol);
        builder.insertBefore(ProgramPoint::atHeadOf(added), Value(start));
        builder.applyInsertions();
    }

    SSARepair repair(builder, analyses);
    for (ValueID site : firstSteps) {
        Value step(function.at(site));
        ValueID stepped = step.generatorOperand();
        builder.setOrigin(step.origin());
        ProgramPoint point = analyses.useLists().pointOf(site);
        Vector<ValueID, 2> constantsUsedAfterStep;
        for (ValueID id : function.at(point.block).values.span().first(enteringEdges.isEmpty() ? 0 : point.index)) {
            if (std::ranges::any_of(analyses.useLists().usesOf(id), [&](const Use& use) { return use.kind != Use::Kind::Operand || use.user != site; }))
                constantsUsedAfterStep.append(id);
        }
        IRBuilder::Expansion expansion = builder.beginExpansion(point);
        std::optional<BlockID> resumes;
        if (!enteringEdges.isEmpty()) {
            resumes = builder.currentBlock();
            BlockID first = builder.addBlock(function.at(*resumes).frequency);
            for (ValueID parameter : Vector<ValueID, 2>(function.at(*resumes).parameters.span()))
                repair.addDefinition(repair.addVariable(parameter), builder.addParameter(first, function.at(parameter).type));
            for (Edge edge : enteringEdges) {
                Target target(targetOf(function, edge));
                target.block = first;
                builder.redirectEdge(edge, WTF::move(target));
            }
            builder.moveToEndOf(first);
            for (ValueID id : constantsUsedAfterStep)
                repair.addDefinition(repair.addVariable(id), builder.append(Value(function.at(id))));
        } else if (!isKnownToBeFirst)
            resumes = builder.emitBranch(builder.append(Opcode::IsGeneratorUnstarted, { stepped })).notTaken;
        builder.append(Opcode::MarkGeneratorRunning, { stepped });
        ValueID value = builder.append(Value(start));
        ValueID status = builder.append(Opcode::TakeGeneratorStepStatus, { stepped });
        builder.endExpansion(expansion, { { value, status } });
        if (!resumes)
            continue;
        builder.moveToEndOf(*resumes);
        ValueID resumed = builder.append(WTF::move(step));
        ValueID valueOfStep = builder.append(makeResultAt(resumed, static_cast<uint32_t>(StepGeneratorResult::Value)));
        builder.endExpansion(expansion, { { valueOfStep, builder.append(makeResultAt(resumed, static_cast<uint32_t>(StepGeneratorResult::Status))) } });
    }
    repair.run();
    return PassOutcome::Changed;
}

// The program never gets hold of a generator that goes nowhere but into an iterator helper, so nothing tells which object it inherits from.
static bool isHeldOnlyByIteratorHelper(FunctionPassContext& pass, const EscapeAnalysis& escape, ValueID generator)
{
    const Function& function = pass.function;
    std::optional<ObjectID> object = escape.objectDefinedBy(generator);
    std::optional<GeneratorBodyDescription> description = describeGeneratorBody(pass.program, function, generator);
    if (!object || !description)
        return false;

    const Function& body = pass.program.at(description->body);
    if (!body.hasBody() || description->generatorParameter >= body.at(entryBlock).parameters.size())
        return false;
    ValueID generatorParameter = body.at(entryBlock).parameters[description->generatorParameter];
    bool bodyOnlySuspends = std::ranges::all_of(body.blocks, [&](const Block& block) {
        bool onlySuspends = true;
        for (ValueID id : block.values) {
            const Value& value = body.at(id);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (operand == generatorParameter)
                    onlySuspends &= value.opcode == Opcode::IsGeneratorRunning || (value.opcode == Opcode::YieldBySavingFrame && operandIndex == OperandIndex::YieldBySavingFrame::generator);
            });
        }
        block.forEachChildOutsideValues([&](ValueID child) {
            onlySuspends &= !child || child != generatorParameter;
        });
        return onlySuspends;
    });
    if (!bodyOnlySuspends)
        return false;

    return std::ranges::all_of(escape.usesOf(*object), [&](const ObjectUse& objectUse) {
        const Use& use = objectUse.use;
        if (use.kind == Use::Kind::EdgeArgument)
            return true;
        if (use.kind != Use::Kind::Operand)
            return false;
        const Value& user = function.at(use.user);
        if (isCastWithoutTest(user.opcode))
            return true;
        switch (user.opcode) {
        case Opcode::CallDirect:
            return use.user == description->startAtCreation && use.operandIndex == OperandIndex::CallDirect::arguments + description->generatorParameter;
        case Opcode::CallBuiltin:
            return pass.program.builtins().at(user.overload()).builtin == Builtins::LinkTimeConstantIteratorHelperCreate && use.operandIndex == OperandIndex::CallBuiltin::arguments;
        // The other two checks ask for the Structure of a prototype object of its own.
        case Opcode::StepGenerator:
            return use.operandIndex == OperandIndex::StepGenerator::generator && (user.check() == GeneratorStepCheck::None || user.check() == GeneratorStepCheck::IsGenerator);
        case Opcode::ResumeGenerator:
            return use.operandIndex == OperandIndex::ResumeGenerator::generator;
        case Opcode::IsGeneratorSuspendedAtStart:
        case Opcode::IsGeneratorUnstarted:
        case Opcode::MarkGeneratorRunning:
        case Opcode::TakeGeneratorStepStatus:
            return true;
        default:
            return false;
        }
    });
}

// Such a generator inherits from %GeneratorPrototype% itself: its function makes no prototype object and no Structure for it.
static PassOutcome dropCalleesOfHiddenGenerators(FunctionPassContext& pass)
{
    const EscapeAnalysis& escape = pass.analyses.escapeWithLiveInstances();
    if (escape.gaveUp())
        return PassOutcome::Unchanged;
    Vector<ValueID, 4> hidden;
    for (ValueID generator : listValues(pass.analyses, Opcode::NewGenerator)) {
        if (pass.function.at(generator).calleeOperand() && isHeldOnlyByIteratorHelper(pass, escape, generator))
            hidden.append(generator);
    }
    if (hidden.isEmpty())
        return PassOutcome::Unchanged;
    for (ValueID generator : hidden)
        pass.function.edit(generator, ValueEdit::Definition).setOperand(OperandIndex::NewGenerator::callee, ValueID());
    pass.analyses.didChangeValues();
    return PassOutcome::Changed;
}

// A generator that no loop could be fused with is still seldom looked at between its creation and its first step.
PassOutcome startGeneratorsAtFirstStep(FunctionPassContext& pass)
{
    if (!hasListedGenerator(pass.function))
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    PassOutcome outcome = dropCalleesOfHiddenGenerators(pass);
    BitVector isTried;
    for (;;) {
        const EscapeAnalysis& escape = pass.analyses.escapeWithLiveInstances();
        if (escape.gaveUp())
            return outcome;
        Vector<ValueID, 4> generators = listValues(pass.analyses, Opcode::NewGenerator);
        if (!generators.containsIf([&](ValueID id) { return !isTried.set(id.index()) && startAtFirstStep(pass, builder, escape, id) == PassOutcome::Changed; }))
            return outcome;
        pass.build.statistics().count(StatisticID::GeneratorsStartedAtFirstStep);
        outcome = PassOutcome::Changed;
        // The builder makes every edit and tells the analyses, and the pass ends with a search behind the last one.
        pass.keptAnalyses = allAnalyses;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
