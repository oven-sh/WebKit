/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFuseGeneratorLoops.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineCopy.h"
#include "AOTLiveness.h"
#include "AOTRegionCloner.h"
#include "AOTResultRules.h"
#include "AOTSSARepair.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CarriedValues {
    BitVector bodyValues;
    BitVector callerValues;
};

struct GeneratorLoopCut {
    ProgramPoint point;
    unsigned index { 0 };
    Vector<ValueID, 2> resultReads;
};

struct Merge {
    Merge(FunctionPassContext& pass, IRBuilder& builder, SSARepair& repair, const FusionCandidate& candidate)
        : pass(pass)
        , builder(builder)
        , repair(repair)
        , candidate(candidate)
    {
    }

    FunctionPassContext& pass;
    IRBuilder& builder;
    SSARepair& repair;
    const FusionCandidate& candidate;
    Origin origin;
    Origin originOfSteps;
    BlockFrequency frequency { BlockFrequency::Normal };
    Vector<IRBuilder::Expansion, 4> siteCuts;
    Vector<IRBuilder::Expansion, 4> yieldCuts;
    BlockID start;
    BlockID enter;
    BlockID leave;
    BlockID failed;
    ValueID sent;
    ValueID mode;
    ValueID value;
    ValueID status;
    ValueID resumePoint;
    ValueID returnSite;
    SSARepair::Variable resumePointVariable { 0 };
    SSARepair::Variable returnSiteVariable { 0 };
};

constexpr int64_t completed = -1;
constexpr size_t startSite = 0;

} // anonymous namespace

template<typename Enumeration>
static constexpr int64_t integerOf(Enumeration value)
{
    return static_cast<int64_t>(value);
}

template<typename Enumeration>
static Range rangeOf(std::initializer_list<Enumeration> values)
{
    Range range = Range::empty();
    for (Enumeration value : values)
        range = range.unionWith(Range::one(integerOf(value)));
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

static bool hasLoopHint(const Function& function, std::span<const ValueID> values)
{
    return std::ranges::any_of(values, [&](ValueID id) {
        return function.at(id).opcode == Opcode::LoopHint;
    });
}

static AnalysisContext contextOfBody(const FunctionPassContext& pass, const FusionCandidate& candidate)
{
    return { pass.build, pass.program, &pass.inputs.flow, &pass.inputs.effects, candidate.body.body };
}

static Vector<ValueID, 4> findYields(FunctionAnalyses& bodyAnalyses)
{
    const Function& body = bodyAnalyses.function();
    Vector<ValueID, 4> yields;
    for (BlockID block : bodyAnalyses.cfg().reversePostOrder()) {
        for (ValueID id : body.at(block).values) {
            if (isYield(body, id))
                yields.append(id);
        }
    }
    return yields;
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
    carried.bodyValues = collectValuesLiveAcross(bodyAnalyses, [&](ValueID id) {
        return isYield(body, id);
    });
    for (ValueID parameter : body.at(entryBlock).parameters)
        carried.bodyValues.clear(parameter.index());

    BitVector isStep(pass.function.values.size());
    for (const GeneratorStepSite& site : candidate.sites.subspan(startSite + 1))
        isStep.set(site.step.index());
    BitVector liveAcrossSteps = collectValuesLiveAcross(pass.analyses, [&](ValueID id) {
        return isStep.get(id.index());
    });
    ProgramPoint start = pass.analyses.useLists().pointOf(candidate.body.startAtCreation);
    for (size_t index : liveAcrossSteps) {
        if (!pass.analyses.dominates(ValueID(index), start))
            carried.callerValues.set(index);
    }
    return carried;
}

static bool isTestOfProvingBranch(const Function& function, const Use& use)
{
    if (use.kind != Use::Kind::TerminatorOperand)
        return false;
    const Terminator& terminator = function.at(use.block).terminator;
    return terminator.isConditionalBranch() && terminator.targets.containsIf([](const Target& target) {
        return !target.proofs.isEmpty();
    });
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
        return isTestOfProvingBranch(function, use);
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
        origin = { frame.callPosition, frame.parent };
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

static bool isRunFromOnePlace(const Program& program, const Function& function, const FusionCandidate& candidate)
{
    Origin origin = originOfSteps(program, function, candidate);
    return std::ranges::all_of(candidate.sites.subspan(startSite + 1), [&](const GeneratorStepSite& site) {
        return site.mode == ResumeMode::Return || visibleOriginOf(program, function, site.step) == origin;
    });
}

// Behind a yield stands the throw of what a step may send in.
static bool throwsWhatStepSends(const Function& body, const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::Throw)
        return false;
    const Value& thrown = body.at(terminator.valueOperand());
    return thrown.opcode == Opcode::ResultAt && isYield(body, thrown.operand(0));
}

// Only what throws or runs other code has its callers listed.
static bool mayHaveItsCallersListed(FunctionAnalyses& bodyAnalyses, const FusionCandidate& candidate)
{
    const Function& body = bodyAnalyses.function();
    bool isSentWhatToThrow = candidate.sites.containsIf([](const GeneratorStepSite& site) {
        return site.mode == ResumeMode::Throw;
    });
    auto listsCallers = [](const Effects& effects) {
        return effects.mayThrow || effects.mayCallUnknownCode;
    };
    return std::ranges::any_of(bodyAnalyses.cfg().reversePostOrder(), [&](BlockID block) {
        if (listsCallers(bodyAnalyses.effectsOfTerminator(block).onAnyEdge()) && (isSentWhatToThrow || !throwsWhatStepSends(body, body.at(block).terminator)))
            return true;
        return std::ranges::any_of(body.at(block).values, [&](ValueID id) {
            return !isYield(body, id) && listsCallers(bodyAnalyses.effectsOf(id).onAnyEdge());
        });
    });
}

static GeneratorFusionDeclineReason findObstacle(FunctionPassContext& pass, const FusionCandidate& candidate, FunctionAnalyses& bodyAnalyses, const CarriedValues& carried)
{
    for (const GeneratorStepSite& site : candidate.sites.subspan(startSite + 1)) {
        if (!pass.analyses.dominates(candidate.body.startAtCreation, pass.analyses.useLists().pointOf(site.step)))
            return GeneratorFusionDeclineReason::Unknown;
    }
    if (!isRunFromOnePlace(pass.program, pass.function, candidate) && mayHaveItsCallersListed(bodyAnalyses, candidate))
        return GeneratorFusionDeclineReason::Unknown;
    for (size_t index : carried.bodyValues) {
        ValueID id(index);
        if (!isYield(bodyAnalyses.function(), id) && !canBecomeParameter(bodyAnalyses, id))
            return GeneratorFusionDeclineReason::BodyIsBoundToFrame;
    }
    for (size_t index : carried.callerValues) {
        ValueID id(index);
        if (canBecomeParameter(pass.analyses, id))
            continue;
        bool isSite = candidate.sites.containsIf([&](const GeneratorStepSite& site) {
            return site.step == id;
        });
        if (!isSite)
            return GeneratorFusionDeclineReason::Unknown;
    }
    return GeneratorFusionDeclineReason::None;
}

static Value makeIntegerValue(int64_t integer)
{
    Value value = IRBuilder::make(Opcode::Integer);
    value.setInteger(integer);
    return value;
}

static Value makeSeed(const Value& definition)
{
    if (isRematerializable(definition))
        return definition;
    Value seed = IRBuilder::make(Opcode::Unspecified);
    seed.setValueType(definition.type);
    seed.setExpected(definition.expected());
    return seed;
}

static void defineResumePoint(Merge& merge, int64_t resumePoint)
{
    merge.repair.addDefinition(merge.resumePointVariable, merge.builder.appendInteger(resumePoint));
}

static void emitJumpToLeave(Merge& merge, ValueID value, GeneratorStepStatus status)
{
    ValueID statusValue = merge.builder.appendInteger(integerOf(status));
    merge.builder.emitJump(merge.leave, { value, statusValue });
}

static void emitSwitchOnReturnSite(Merge& merge, Vector<Target, 4>&& targets)
{
    Target last = targets.takeLast();
    if (targets.isEmpty()) {
        merge.builder.emitJump(last.block, WTF::move(last.arguments));
        return;
    }
    Vector<std::pair<int64_t, Target>> cases;
    cases.reserveInitialCapacity(targets.size());
    for (size_t site = 0; site < targets.size(); ++site)
        cases.append({ static_cast<int64_t>(site), WTF::move(targets[site]) });
    merge.builder.emitSwitch(merge.returnSite, WTF::move(cases), WTF::move(last));
}

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
    AOT::sort(cuts, std::ranges::greater { }, [](const GeneratorLoopCut& cut) {
        return std::tuple(cut.point.block, cut.point.index);
    });
    return cuts;
}

static IRBuilder::Expansion beginCut(Merge& merge, const GeneratorLoopCut& cut)
{
    const Function& function = merge.pass.function;
    ValueID transfer = function.at(cut.point.block).values[cut.point.index];
    if (!numberOfResultsOf(merge.pass.program, function, function.at(transfer)))
        return merge.builder.beginExpansion(cut.point);
    // The overload without reads erases them wherever they stand, which moves the points of cuts that are still to come.
    return merge.builder.beginExpansion(cut.point, cut.resultReads.span());
}

static void insertStateSeeds(Merge& merge)
{
    const Function& function = merge.pass.function;
    ProgramPoint point = merge.pass.analyses.useLists().pointAfter(merge.candidate.generator);
    merge.origin = function.at(merge.candidate.generator).origin();
    merge.originOfSteps = originOfSteps(merge.pass.program, function, merge.candidate);
    merge.frequency = function.at(point.block).frequency;

    merge.builder.setOrigin(merge.origin);
    merge.resumePoint = merge.builder.insertBefore(point, makeIntegerValue(0));
    merge.returnSite = merge.builder.insertBefore(point, makeIntegerValue(0));
    merge.builder.applyInsertions();
    merge.resumePointVariable = merge.repair.addVariable(merge.resumePoint);
    merge.returnSiteVariable = merge.repair.addVariable(merge.returnSite);
}

static void addTransferBlocks(Merge& merge)
{
    IRBuilder& builder = merge.builder;
    TypeArena& types = merge.pass.types;
    builder.setHandler(std::nullopt);
    merge.start = builder.addBlock(merge.frequency);

    merge.enter = builder.addBlock(merge.frequency);
    merge.sent = builder.addParameter(merge.enter, TypeTable::anything);
    merge.mode = builder.addParameter(merge.enter, types.intern(Type::integer(rangeOf({ ResumeMode::Next, ResumeMode::Throw, ResumeMode::Return }))));

    merge.leave = builder.addBlock(merge.frequency);
    merge.value = builder.addParameter(merge.leave, TypeTable::anything);
    merge.status = builder.addParameter(merge.leave, types.intern(Type::integer(rangeOf({ GeneratorStepStatus::Yielded, GeneratorStepStatus::Completed }))));
}

static void emitTransferToGenerator(Merge& merge, size_t site, uint32_t firstIndexOfHead)
{
    IRBuilder& builder = merge.builder;
    const Function& function = merge.pass.function;
    // Every cycle the merge creates passes through the block in front of a site. A hint in enter or leave would keep jumps from being threaded through them.
    if (!hasLoopHint(function, function.at(builder.currentBlock()).values.subspan(firstIndexOfHead)))
        builder.append(Opcode::LoopHint, { });
    merge.repair.addDefinition(merge.returnSiteVariable, builder.appendInteger(static_cast<int64_t>(site)));
    if (site == startSite) {
        builder.emitJump(merge.start);
        return;
    }

    const GeneratorStepSite& step = merge.candidate.sites[site];
    ValueID sent = step.sent ? step.sent : builder.appendUndefined();
    ValueID mode = builder.appendInteger(integerOf(step.mode));
    builder.emitJump(merge.enter, { sent, mode });
}

static void cutSites(Merge& merge)
{
    const auto& sites = merge.candidate.sites;
    Vector<ValueID, 4> steps(sites.size(), [&](size_t site) {
        return sites[site].step;
    });
    merge.siteCuts.grow(steps.size());
    Vector<GeneratorLoopCut, 4> cuts = findCuts(merge.pass.analyses, steps.span());
    for (size_t position = 0; position < cuts.size(); ++position) {
        const GeneratorLoopCut& cut = cuts[position];
        // The next cut, if it is in the same block, leaves only what follows it in front of this site.
        bool isAfterCutInBlock = position + 1 < cuts.size() && cuts[position + 1].point.block == cut.point.block;
        uint32_t firstIndexOfHead = isAfterCutInBlock ? cuts[position + 1].point.index + 1 : 0;
        merge.siteCuts[cut.index] = beginCut(merge, cut);
        emitTransferToGenerator(merge, cut.index, firstIndexOfHead);
    }
}

static void addFailedBlock(Merge& merge)
{
    IRBuilder& builder = merge.builder;
    builder.setOrigin(merge.origin);
    builder.setHandler(std::nullopt);
    merge.failed = builder.addHandlerBlock(BlockRole::CleanupHandler, ValueID());
    builder.moveToEndOf(merge.failed);
    ValueID exception = builder.append(Opcode::CatchException, { });
    defineResumePoint(merge, completed);

    TypeID exceptionType = merge.pass.function.at(exception).type;
    Vector<Target, 4> targets;
    Vector<std::pair<BlockID, ValueID>, 4> rethrows;
    for (const IRBuilder::Expansion& cut : merge.siteCuts) {
        builder.setHandler(cut.handler);
        BlockID block = builder.addBlock(BlockFrequency::Rare);
        rethrows.append({ block, builder.addParameter(block, exceptionType) });
        targets.append(Target { block, { exception }, { } });
    }
    builder.setHandler(std::nullopt);
    emitSwitchOnReturnSite(merge, WTF::move(targets));

    for (auto [block, rethrown] : rethrows) {
        builder.moveToEndOf(block);
        builder.emitRethrow(rethrown);
    }
}

static ValueID appendEntryArgument(Merge& merge, const Value& parameter, unsigned index)
{
    IRBuilder& builder = merge.builder;
    const GeneratorBodyDescription& description = merge.candidate.body;
    if (description.generatorParameter == index)
        return merge.candidate.generator;

    const GeneratorStepSite& start = merge.candidate.sites[startSite];
    ValueID argument = description.entryArguments[index];
    if (description.sentParameter == index)
        argument = start.sent ? start.sent : builder.appendUndefined();
    else if (description.modeParameter == index)
        argument = builder.appendInteger(integerOf(start.mode));
    return upcastToParameterType(builder, parameter, argument);
}

static void copyBody(Merge& merge, RegionCloner& cloner, const Function& body, std::span<const BlockID> reachableBlocks)
{
    IRBuilder& builder = merge.builder;
    Function& function = merge.pass.function;
    uint32_t firstFrame = appendInlineFrames(function, InlineFrame { .callee = merge.candidate.body.body, .callPosition = merge.originOfSteps.position, .parent = merge.originOfSteps.inlineFrame, .module = body.module }, body);

    builder.setOrigin(merge.origin);
    builder.moveToEndOf(merge.start);
    const auto& parameters = body.at(entryBlock).parameters;
    RELEASE_ASSERT(parameters.size() == merge.candidate.body.entryArguments.size());
    for (unsigned index = 0; index < parameters.size(); ++index)
        cloner.mapValue(parameters[index], appendEntryArgument(merge, body.at(parameters[index]), index));

    builder.setHandler(Target { merge.failed, { }, { } });
    cloneBlocksIntoInlineFrame(builder, cloner, reachableBlocks, firstFrame);
    builder.setHandler(std::nullopt);
    builder.emitJump(cloner.cloneOf(entryBlock));

    for (BlockID clone : cloner.clonedBlocks()) {
        function.edit(clone, BlockEdit::Rest).frequency = rarerOf(function.at(clone).frequency, merge.frequency);
        // The first site is the start at the creation, which runs up to the initial yield.
        for (ValueID id : function.at(clone).values) {
            Value& value = function.edit(id);
            if (value.opcode != Opcode::IsGeneratorRunning)
                continue;
            value.become(Opcode::Boolean);
            value.setBoolean(false);
        }
    }
    merge.pass.analyses.didChangeControlFlow();
}

static void replaceReturns(Merge& merge, std::span<const BlockID> clonedBlocks)
{
    IRBuilder& builder = merge.builder;
    Function& function = merge.pass.function;
    for (BlockID block : clonedBlocks) {
        if (function.at(block).terminator.kind == TerminatorKind::ReturnCall)
            builder.demoteReturnCall(block);
        if (function.at(block).terminator.kind != TerminatorKind::Return)
            continue;

        Terminator returned = std::exchange(function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator, Terminator());
        merge.pass.analyses.didChangeControlFlow();
        builder.setOrigin(returned.origin());
        builder.moveToEndOf(block);
        ValueID value = returned.valueOperand() ? returned.valueOperand() : builder.appendUndefined();
        defineResumePoint(merge, completed);
        emitJumpToLeave(merge, value, GeneratorStepStatus::Completed);
    }
}

static void cutYields(Merge& merge, const RegionCloner& cloner, std::span<const ValueID> yields)
{
    const Function& function = merge.pass.function;
    Vector<ValueID, 4> copies(yields.size(), [&](size_t index) {
        return cloner.cloneOf(yields[index]);
    });
    merge.yieldCuts.grow(copies.size());
    for (const GeneratorLoopCut& cut : findCuts(merge.pass.analyses, copies.span())) {
        ValueID yielded = function.at(copies[cut.index]).valueOperand();
        merge.yieldCuts[cut.index] = beginCut(merge, cut);
        defineResumePoint(merge, resumePointAfter(cut.index));
        emitJumpToLeave(merge, yielded, GeneratorStepStatus::Yielded);
    }
}

static BlockID addFinishedBlocks(Merge& merge)
{
    IRBuilder& builder = merge.builder;
    BlockFrequency frequency = rarerOf(merge.frequency, BlockFrequency::Unlikely);
    builder.setHandler(std::nullopt);
    BlockID finished = builder.addBlock(frequency);
    BlockID stepped = builder.addBlock(frequency);
    BlockID returned = builder.addBlock(frequency);
    builder.setHandler(Target { merge.failed, { }, { } });
    BlockID thrown = builder.addBlock(BlockFrequency::Rare);
    builder.setHandler(std::nullopt);

    builder.moveToEndOf(finished);
    Vector<std::pair<int64_t, Target>> cases;
    cases.append({ integerOf(ResumeMode::Throw), Target { thrown, { }, { } } });
    cases.append({ integerOf(ResumeMode::Return), Target { returned, { }, { } } });
    builder.emitSwitch(merge.mode, WTF::move(cases), Target { stepped, { }, { } });

    builder.moveToEndOf(stepped);
    emitJumpToLeave(merge, builder.appendUndefined(), GeneratorStepStatus::Completed);
    builder.moveToEndOf(returned);
    emitJumpToLeave(merge, merge.sent, GeneratorStepStatus::Completed);
    builder.moveToEndOf(thrown);
    builder.emitThrow(merge.sent);
    return finished;
}

static void emitEnter(Merge& merge)
{
    IRBuilder& builder = merge.builder;
    builder.setOrigin(merge.origin);
    BlockID finished = addFinishedBlocks(merge);

    std::array<ValueID, numberOfEnumerators<YieldResult>> results;
    results[static_cast<size_t>(YieldResult::Sent)] = merge.sent;
    results[static_cast<size_t>(YieldResult::Mode)] = merge.mode;
    Vector<std::pair<int64_t, Target>> cases;
    cases.reserveInitialCapacity(merge.yieldCuts.size());
    for (unsigned index = 0; index < merge.yieldCuts.size(); ++index)
        cases.append({ resumePointAfter(index), Target { merge.yieldCuts[index].continuation, std::span<const ValueID>(results), { } } });

    builder.moveToEndOf(merge.enter);
    if (cases.isEmpty()) {
        builder.emitJump(finished);
        return;
    }
    builder.emitSwitch(merge.resumePoint, WTF::move(cases), Target { finished, { }, { } });
}

static Arguments resultsOfSite(const Merge& merge, size_t site)
{
    if (site == startSite)
        return { merge.value };
    std::array<ValueID, numberOfEnumerators<StepGeneratorResult>> results;
    results[static_cast<size_t>(StepGeneratorResult::Value)] = merge.value;
    results[static_cast<size_t>(StepGeneratorResult::Status)] = merge.status;
    return std::span<const ValueID>(results);
}

static void emitLeave(Merge& merge)
{
    Vector<Target, 4> targets;
    for (size_t site = 0; site < merge.siteCuts.size(); ++site)
        targets.append(Target { merge.siteCuts[site].continuation, resultsOfSite(merge, site), { } });
    merge.builder.setOrigin(merge.origin);
    merge.builder.moveToEndOf(merge.leave);
    emitSwitchOnReturnSite(merge, WTF::move(targets));
}

static Vector<ValueID> collectCarriedDefinitions(const Merge& merge, const RegionCloner& cloner, std::span<const ValueID> yields, const CarriedValues& carried)
{
    const Program& program = merge.pass.program;
    const Function& function = merge.pass.function;
    Vector<ValueID> definitions;
    BitVector isListed(function.values.size());
    auto add = [&](ValueID definition) {
        if (numberOfResultsOf(program, function, function.at(definition)) || isListed.set(definition.index()))
            return;
        definitions.append(definition);
    };
    auto addResults = [&](const IRBuilder::Expansion& cut) {
        for (ValueID result : cut.results)
            add(result);
    };

    for (unsigned index = 0; index < yields.size(); ++index) {
        if (carried.bodyValues.get(yields[index].index()))
            addResults(merge.yieldCuts[index]);
    }
    for (size_t index : carried.bodyValues)
        add(cloner.cloneOf(ValueID(index)));

    for (size_t site = 0; site < merge.siteCuts.size(); ++site) {
        if (carried.callerValues.get(merge.candidate.sites[site].step.index()))
            addResults(merge.siteCuts[site]);
    }
    for (size_t index : carried.callerValues)
        add(ValueID(index));
    return definitions;
}

static void repairCarriedValues(Merge& merge, std::span<const ValueID> definitions)
{
    IRBuilder& builder = merge.builder;
    const Function& function = merge.pass.function;
    ProgramPoint point = merge.pass.analyses.useLists().pointAfter(merge.candidate.generator);
    builder.setOrigin(merge.origin);
    Vector<ValueID> seeds;
    seeds.reserveInitialCapacity(definitions.size());
    for (ValueID definition : definitions)
        seeds.append(builder.insertBefore(point, makeSeed(function.at(definition))));
    builder.applyInsertions();

    for (size_t index = 0; index < definitions.size(); ++index) {
        SSARepair::Variable variable = merge.repair.addVariable(seeds[index]);
        merge.repair.addDefinition(variable, definitions[index]);
    }
    merge.repair.run();
}

GeneratorFusionDeclineReason findMergeObstacle(FunctionPassContext& pass, const FusionCandidate& candidate)
{
    FunctionAnalyses bodyAnalyses(contextOfBody(pass, candidate), pass.program.at(candidate.body.body));
    return findObstacle(pass, candidate, bodyAnalyses, collectCarriedValues(pass, candidate, bodyAnalyses));
}

void fuseGenerator(FunctionPassContext& pass, IRBuilder& builder, const FusionCandidate& candidate)
{
    RELEASE_ASSERT(candidate.sites[startSite].step == candidate.body.startAtCreation);
    const Function& body = pass.program.at(candidate.body.body);
    FunctionAnalyses bodyAnalyses(contextOfBody(pass, candidate), body);
    CarriedValues carried = collectCarriedValues(pass, candidate, bodyAnalyses);
    RELEASE_ASSERT(findObstacle(pass, candidate, bodyAnalyses, carried) == GeneratorFusionDeclineReason::None);
    Vector<ValueID, 4> yields = findYields(bodyAnalyses);

    SSARepair repair(builder, pass.analyses);
    Merge merge(pass, builder, repair, candidate);
    insertStateSeeds(merge);
    addTransferBlocks(merge);
    cutSites(merge);
    addFailedBlock(merge);

    RegionCloner cloner(builder, body);
    copyBody(merge, cloner, body, bodyAnalyses.cfg().reversePostOrder());
    // A cut moves the terminator of its block into a block that clonedBlocks() does not list.
    replaceReturns(merge, cloner.clonedBlocks());
    cutYields(merge, cloner, yields.span());
    emitEnter(merge);
    emitLeave(merge);

    repairCarriedValues(merge, collectCarriedDefinitions(merge, cloner, yields.span(), carried).span());
    builder.setHandler(std::nullopt);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
