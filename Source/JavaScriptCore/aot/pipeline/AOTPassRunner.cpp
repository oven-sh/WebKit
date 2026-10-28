/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPassRunner.h"

#if ENABLE(AOT)

#include "AOTArrayChains.h"
#include "AOTBindGlobalNames.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCastsWithoutFacts.h"
#include "AOTCurrentWork.h"
#include "AOTEffectSummaries.h"
#include "AOTExactBuffer.h"
#include "AOTExpandOwnKeysInternal.h"
#include "AOTFlowApply.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTInt32Conversions.h"
#include "AOTObservableOperations.h"
#include "AOTPhaseTimes.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTProgramPasses.h"
#include "AOTRepresentationRules.h"
#include "AOTResolveAccesses.h"
#include "AOTResolveBuiltinsInternal.h"
#include "AOTSelectTypedResultsInternal.h"
#include "AOTSimplifyBuiltinCallsInternal.h"
#include "AOTSplitUnions.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include "AOTVirtualObject.h"
#include <array>
#include <bit>
#include <numeric>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/CPUTime.h>
#include <wtf/DataLog.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// In a body that holds nothing of the first list, or nothing of the second where there is one, the pass leaves the function as it is and reports nothing, whatever its other inputs say, on its first run as on a later one.
struct TriggersOfPass {
    PassID pass;
    BodyContents first;
    std::optional<BodyContents> second;
};

// The rows the other way round: for each thing that a body may hold, the passes that have it in the one list or in the other.
struct PassesByContent {
    struct Lists {
        PassSet first;
        PassSet other;
    };

    std::array<Lists, numberOfEnumerators<Opcode>> byOpcode { };
    std::array<Lists, numberOfEnumerators<TerminatorKind>> byTerminator { };
    std::array<Lists, 8> byTrait { };
    PassSet always;
    PassSet needingOneList;
};

} // anonymous namespace

// A pass without a row may have something to do in any body.
static constexpr TriggersOfPass triggersOfPasses[] {
    { PassID::ListLiveValuesAtSuspensions, { .opcodes = { Opcode::Await, Opcode::Yield, Opcode::InitialYield, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::ReadScope } } },
    { PassID::CheckInt32Promises, { .opcodes = { Opcode::Convert }, .terminators = { TerminatorKind::ReturnCall } } },
    { PassID::CheckRegExpLiterals, { .opcodes = { Opcode::NewRegExp } } },
    { PassID::AddGenericVersion, { .opcodes = { Opcode::Check, Opcode::Construct, Opcode::CallBuiltin } } },
    { PassID::ResolveBuiltins, { .opcodes = operationsResolvedToBuiltins, .traits = BodyTrait::ParametersOfRuntimeFunction } },
    { PassID::BindGlobalNames, { .opcodes = bindableAccesses } },
    { PassID::CallThroughMethodTable, { .opcodes = { Opcode::Call } }, { { .opcodes = { Opcode::PropGet } } } },
    { PassID::ResolveCalls, { .opcodes = callSiteOpcodes } },
    { PassID::ResolveAccesses, { .opcodes = accessesToResolve } },
    { PassID::SplitUnions, { .opcodes = accessesByNameOrKey } },
    { PassID::PlaceTypeTests, { .opcodes = opcodesPlaceTypeTestsLooksFor } },
    { PassID::ExpandIteration, { .opcodes = expandedOpcodes } },
    { PassID::ExpandProvenIterations, { .opcodes = iterationOpcodes } },
    { PassID::ExpandOwnKeys, { .opcodes = ownKeyExpansionOpcodes } },
    { PassID::SelectTypedResults, { .opcodes = typedResultCallOpcodes } },
    { PassID::InlineCalls, { .opcodes = { Opcode::CallDirect } } },
    { PassID::SimplifyCasts, { .opcodes = simplifiableCasts } },
    { PassID::SinkAllocations, { .opcodes = opcodesInterpretedAs(Interpretation::Allocation) } },
    { PassID::EliminateRedundantReads, { .opcodes = opcodesOfRedundantReadElimination } },
    { PassID::EliminateGuards, { .opcodes = opcodesOfGuardElimination } },
    { PassID::SeparateUnlikelyLoopPaths, { .opcodes = { Opcode::LoopHint } } },
    { PassID::ConvertCountersToIntegers, { .traits = BodyTrait::BlockParameters } },
    { PassID::RemoveDeadCountedLoops, { .opcodes = { Opcode::LoopHint } } },
    { PassID::UnrollShortLoops, { .opcodes = { Opcode::LoopHint } } },
    { PassID::FuseArrayChains, { .opcodes = arrayMethodCallOpcodes } },
    { PassID::FuseGeneratorLoops, { .opcodes = { Opcode::NewGenerator } }, { { .opcodes = { Opcode::StepGenerator } } } },
    { PassID::StartGeneratorsAtFirstStep, { .opcodes = { Opcode::NewGenerator } } },
    { PassID::CreateStringBuilders, { .opcodes = concatenationOpcodes } },
    { PassID::FuseConcatenations, { .opcodes = concatenationOpcodes } },
    { PassID::ExpandData, { .opcodes = { Opcode::Data } } },
    { PassID::FoldConstants, { .opcodes = foldableOpcodes } },
    { PassID::SimplifyControlFlow, { .traits = BodyTrait::SeveralBlocks } },
    { PassID::RemoveTrivialParameters, { .traits = BodyTrait::BlockParameters } },
    { PassID::PropagateBranchConditions, { .opcodes = { Opcode::ToBoolean }, .terminators = { TerminatorKind::Branch } } },
    { PassID::HoistTestsIntoPredecessors, { .opcodes = operationsHoistedIntoPredecessors }, { { .traits = BodyTrait::BlockParameters } } },
    { PassID::TestNarrowerTypesFirst, { .opcodes = { Opcode::TypeTest, Opcode::StrictEq } } },
    { PassID::ThreadJumps, { .opcodes = { Opcode::TypeTest }, .traits = BodyTrait::BlockParameters } },
    { PassID::RemoveRedundantHandlers, { .traits = BodyTrait::Handlers } },
    { PassID::HoistLoopInvariantReads, { .opcodes = hoistableOpcodes }, { { .traits = BodyTrait::SeveralBlocks } } },
    { PassID::SimplifyBuiltinCalls, { .opcodes = OpcodeSet { Opcode::CallBuiltin, Opcode::NewTemplateObject } | simplifiedIntrinsics | constantListSourceOpcodes }, { { .opcodes = OpcodeSet { Opcode::CallBuiltin, Opcode::NewTemplateObject } | simplifiedIntrinsics | constantListUserOpcodes } } },
    { PassID::ReduceIntegerDivisions, { .opcodes = { Opcode::Div } }, { { .opcodes = OpcodeSet { Opcode::CallBuiltin } | int32ConvertingOperations } } },
    { PassID::PlanArrayCapacities, { .opcodes = arrayCapacityOpcodes } },
    { PassID::PlanCollectionCapacities, { .opcodes = { Opcode::CallBuiltin } }, { { .traits = BodyTrait::SeveralBlocks } } },
    { PassID::FoldConstantRegExps, { .opcodes = { Opcode::CallBuiltin } } },
    { PassID::ShareRegExpLiterals, { .opcodes = { Opcode::NewRegExp } } },
    { PassID::InsertObjectCoercibleChecks, { .opcodes = objectCoercibleCheckOpcodes } },
    { PassID::EliminateTailRecursion, { .terminators = { TerminatorKind::ReturnCall } } },
    { PassID::ResolveAwaitedCalls, { .opcodes = { Opcode::AwaitBySavingFrame, Opcode::PromiseAllOfElements } } },
    { PassID::SinkConstants, { .traits = BodyTrait::SeveralBlocks } },
    { PassID::ExpandLongArrayLiterals, { .opcodes = { Opcode::NewArray } } },
    { PassID::FinishBirths, { .opcodes = { Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::NewObject }, .traits = BodyTrait::UnfinishedThis } },
    { PassID::RemoveRedundantBarriers, { .opcodes = operationsThatMayRunBarrier } },
};

static constexpr PassesByContent passesByContent = [] {
    PassesByContent result;
    auto add = [&](PassID pass, const BodyContents& contents, PassSet PassesByContent::Lists::* list) {
        contents.opcodes.forEach([&](Opcode opcode) {
            (result.byOpcode[static_cast<unsigned>(opcode)].*list).add(pass);
        });
        contents.terminators.forEach([&](TerminatorKind kind) {
            (result.byTerminator[static_cast<unsigned>(kind)].*list).add(pass);
        });
        for (unsigned traits = contents.traits.toRaw(); traits; traits &= traits - 1)
            (result.byTrait[std::countr_zero(traits)].*list).add(pass);
    };
    PassSet passesWithRow;
    for (const TriggersOfPass& row : triggersOfPasses) {
        passesWithRow.add(row.pass);
        add(row.pass, row.first, &PassesByContent::Lists::first);
        if (row.second)
            add(row.pass, *row.second, &PassesByContent::Lists::other);
        else
            result.needingOneList.add(row.pass);
    }
    for (PassID pass : allMembersOf<PassID>) {
        if (!passesWithRow.contains(pass))
            result.always.add(pass);
    }
    return result;
}();

static_assert(!passesByContent.always.contains(PassID::SimplifyCasts));

bool hasTriggers(PassID id)
{
    return !passesByContent.always.contains(id);
}

PassSet passesThatMayFireIn(const BodyContents& contents)
{
    PassSet first;
    PassSet other = passesByContent.needingOneList;
    auto add = [&](const PassesByContent::Lists& lists) {
        first |= lists.first;
        other |= lists.other;
    };
    contents.opcodes.forEach([&](Opcode opcode) {
        add(passesByContent.byOpcode[static_cast<unsigned>(opcode)]);
    });
    contents.terminators.forEach([&](TerminatorKind kind) {
        add(passesByContent.byTerminator[static_cast<unsigned>(kind)]);
    });
    for (unsigned traits = contents.traits.toRaw(); traits; traits &= traits - 1)
        add(passesByContent.byTrait[std::countr_zero(traits)]);
    first &= other;
    first |= passesByContent.always;
    return first;
}

static size_t findFixpointEnd(std::span<const ScheduleStep> steps, size_t begin)
{
    unsigned depth = 0;
    for (size_t index = begin; index < steps.size(); ++index) {
        switch (steps[index].kind) {
        case ScheduleStep::Kind::BeginFixpoint:
            ++depth;
            break;
        case ScheduleStep::Kind::EndFixpoint:
            if (!--depth)
                return index;
            break;
        case ScheduleStep::Kind::RunPass:
        case ScheduleStep::Kind::RunSchedule:
            break;
        }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto PassRunner::stepsInsideStepsOf(FunctionSchedule schedule) -> std::span<const StepsInside>
{
    using Table = std::array<Vector<StepsInside>, numberOfFunctionSchedules>;
    static const NeverDestroyed<Table> table = [] {
        Table result;
        auto fill = [&](auto& fill, FunctionSchedule filled) -> void {
            Vector<StepsInside>& rows = result[static_cast<unsigned>(filled)];
            std::span<const ScheduleStep> steps = stepsOf(filled);
            if (rows.size() == steps.size())
                return;
            rows.fill(StepsInside { }, steps.size());
            // From the last step to the first: a fixpoint finds the rows of the steps that it holds.
            for (size_t index = steps.size(); index--;) {
                StepsInside& row = rows[index];
                switch (steps[index].kind) {
                case ScheduleStep::Kind::RunPass:
                    row.passes.set(static_cast<unsigned>(steps[index].pass));
                    row.mayBeLeftOutAtRest = !steps[index].isBehindRounds;
                    break;
                case ScheduleStep::Kind::RunSchedule: {
                    fill(fill, steps[index].schedule);
                    std::span<const StepsInside> included = result[static_cast<unsigned>(steps[index].schedule)].span();
                    row.mayBeLeftOutAtRest = true;
                    for (const StepsInside& inside : included.first(included.size() - !steps[index].compacts)) {
                        row.passes.merge(inside.passes);
                        row.mayBeLeftOutAtRest &= inside.mayBeLeftOutAtRest;
                    }
                    break;
                }
                case ScheduleStep::Kind::BeginFixpoint:
                    row.numberOfStepsToEnd = findFixpointEnd(steps, index) - index;
                    row.mayBeLeftOutAtRest = true;
                    for (size_t inside = index + 1; inside < index + row.numberOfStepsToEnd; ++inside) {
                        row.passes.merge(rows[inside].passes);
                        row.mayBeLeftOutAtRest &= rows[inside].mayBeLeftOutAtRest;
                    }
                    break;
                case ScheduleStep::Kind::EndFixpoint:
                    row.mayBeLeftOutAtRest = true;
                    break;
                }
            }
        };
        for (unsigned schedule = 0; schedule < numberOfFunctionSchedules; ++schedule)
            fill(fill, static_cast<FunctionSchedule>(schedule));
        return result;
    }();
    return table.get()[static_cast<unsigned>(schedule)].span();
}

static void invalidateAnalyses(FunctionAnalyses& analyses, InvalidatedAnalyses invalidated, AnalysisSet kept)
{
    // What the cache of effects answers within a round depends on who has asked before, so no round outlives a run that has changed the function.
    kept.remove(FunctionAnalysis::Effects);
    switch (invalidated) {
    case InvalidatedAnalyses::None:
        return;
    case InvalidatedAnalyses::Values:
        analyses.invalidate(valueAnalyses - kept);
        analyses.didRunPass();
        return;
    case InvalidatedAnalyses::All:
        analyses.invalidate(allAnalyses - kept);
        analyses.didRunPass();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasCodeBehindNeverValue(const Function& function, const Block& block)
{
    size_t index = block.values.findIf([&](ValueID id) {
        const Value& value = function.at(id);
        return value.type == TypeTable::never && resultRuleOf(value.opcode) != ResultRule::None;
    });
    if (index == notFound)
        return false;
    return index + 1 != block.values.size() || block.terminator.kind != TerminatorKind::Unreachable;
}

// They type no value, put none behind another and make no block reachable. Nobody walks the body behind them to find that out.
static constexpr PassSet passesThatLeaveNoCodeBehindNeverValues { PassID::CompactBlocks, PassID::RemoveTrivialParameters, PassID::ThreadJumps };

// A pass, or whoever built the function, may leave a conversion that always fails, or a value of whose type nothing is left, in the middle of a block.
static bool cutCodeBehindNeverValues(FunctionPassContext& context, const BodyContents& contents)
{
    if (!contents.traits.contains(BodyTrait::NeverValues))
        return false;
    const Function& function = context.function;
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        if (!hasCodeBehindNeverValue(function, function.blocks[index]) || !context.analyses.cfg().isReachable(BlockID(index)))
            continue;
        if (waitsForGenericVersion(context.program, function))
            return false;
        finishTypedValues(TypePropagationInputs { context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo });
        return true;
    }
    return false;
}

auto PassRunner::passesAffectedBy(PassID changer) -> const AffectedPasses&
{
    static const std::array<AffectedPasses, numberOfPasses> passesByChanger = [] {
        std::array<AffectedPasses, numberOfPasses> result { };
        for (PassID pass : allMembersOf<PassID>) {
            PassBits& givenWork = result[static_cast<unsigned>(pass)].givenWork;
            givenWork.setAll();
            passesThatCanWaitBehind(pass).forEach([&](PassID waiting) {
                givenWork.clear(static_cast<unsigned>(waiting));
            });
            if (pass == PassID::None)
                continue;
            passesEnabling(pass).forEach([&](PassID enabling) {
                result[static_cast<unsigned>(enabling)].enabled.set(static_cast<unsigned>(pass));
            });
        }
        return result;
    }();
    return passesByChanger[static_cast<unsigned>(changer)];
}

PassRunner::PassRunner(BuildContext& build, std::optional<PassID> stopAfter)
    : m_build(build)
    , m_stopAfter(stopAfter)
    , m_isStopped(build.diagnostics().hasErrors())
{
    m_everyFixpointMayTakeRound = true;
    for (unsigned schedule = 0; schedule < numberOfFunctionSchedules; ++schedule) {
        for (const ScheduleStep& step : stepsOf(static_cast<FunctionSchedule>(schedule)))
            m_everyFixpointMayTakeRound &= step.kind != ScheduleStep::Kind::BeginFixpoint || build.limit(step.limit);
    }
}

PassOutcome PassRunner::runFunctionPass(PassID id, FunctionPassContext& context) const
{
    RELEASE_ASSERT(context.analyses.context().id == context.id);
    ScheduleRun run;
    prepare(run);
    size_t numberOfValuesAtStart = context.function.values.size();
    run.functionAsRead = context.subject().function;
    CurrentWorkScope workScope(context.program, run.functionAsRead);
    PassOutcome outcome = runFunctionPass(id, context, run);
    addTotals(run, numberOfValuesAtStart);
    return run.didMakeFacts ? PassOutcome::Changed : outcome;
}

void PassRunner::prepare(ScheduleRun& run) const
{
    run.recordsChangers = m_build.options().verifiesAfterEveryPass;
    if (run.recordsChangers || m_build.options().logging.logsPhaseTimes) [[unlikely]]
        run.notes.emplace();
}

static constexpr PassSet optionalPassesOfCompactFunctions {
    PassID::BindGlobalNames, PassID::SimplifyBuiltinCalls, PassID::CallThroughMethodTable, PassID::ResolveCalls, PassID::ResolveAccesses, PassID::SimplifyCasts, PassID::FoldConstants, PassID::SimplifyControlFlow,
    PassID::RemoveTrivialParameters, PassID::EliminateDeadCode, PassID::RemoveRedundantHandlers, PassID::RetypeValues, PassID::CompactBlocks, PassID::ResolveAwaitedCalls,
};

static constexpr PassSet optionalPassesOfGenericVersions = optionalPassesOfCompactFunctions | PassSet { PassID::InsertObjectCoercibleChecks, PassID::EliminateRedundantReads };

static constexpr PassSet optionalPassesOfColdAsyncFunctions = optionalPassesOfCompactFunctions | PassSet { PassID::PlaceTypeTests, PassID::InsertObjectCoercibleChecks, PassID::SinkAllocations, PassID::FinishBirths };

// An async function waits between its steps, so what it costs is seldom its own code.
static const PassSet* optionalPassesOfShortScheduleFor(const Function& function, bool hasNoWarmCode, bool isCompact)
{
    if (function.role == FunctionRole::GenericVersion)
        return &optionalPassesOfGenericVersions;
    if (isCompact)
        return &optionalPassesOfCompactFunctions;
    if (hasNoWarmCode && function.has(FunctionFlags::IsAsync))
        return &optionalPassesOfColdAsyncFunctions;
    return nullptr;
}

AOT_THRESHOLD(uint64_t, maxRoundsOfShortSchedule, 1);

PassOutcome PassRunner::runSchedule(FunctionSchedule schedule, const Program& program, const OptimizationInputs& inputs, FunctionID id, Function& function, TaskArenas& arenas, const std::optional<BodyContents>& contents) const
{
    FunctionAnalyses analyses(AnalysisContext { m_build, program, &inputs.flow, &inputs.effects, id }, function);
    FunctionPassContext context { program, inputs, id, function, analyses, arenas.types, arenas.strings, m_build };
    analyses.setTypingMemo(&context.typingMemo);
    context.openSites.setLogIsWritten(true);
    context.openSites.setVerifiesLists(m_build.options().verifiesAfterEveryPass);
    ScheduleRun run;
    prepare(run);
    size_t numberOfValuesAtStart = function.values.size();
    run.functionAsRead = context.subject().function;
    CurrentWorkScope workScope(program, run.functionAsRead);
    const bool readsHeat = schedule == FunctionSchedule::Prepare || schedule == FunctionSchedule::Optimize;
    const bool hasNoWarmCode = readsHeat && !inputs.heat.hasWarmCode(program, id).predicted();
    run.optionalPassesOfShortSchedule = optionalPassesOfShortScheduleFor(function, hasNoWarmCode, readsHeat && inputs.heat.isCompact(program, id).predicted());
    if (run.optionalPassesOfShortSchedule)
        run.maxRounds = maxRoundsOfShortSchedule;
    if (contents) {
        RELEASE_ASSERT(!m_build.options().verifiesAfterEveryPass || *contents == contentsOf(function));
        run.setContents(*contents, function);
    }
    if (program.form() == IRForm::Prepared && cutCodeBehindNeverValues(context, run.contentsOf(function, context.changes))) {
        run.contents = std::nullopt;
        ++run.numberOfChanges;
    }
    // Whoever edits a function takes that word back.
    const bool entersTyped = program.form() == IRForm::Prepared && function.hasBody() && function.valuesAreTypedAsDefined();
    if (entersTyped)
        context.pendingTyping.didEnterScheduleTyped(context.changes);
    runSteps(stepsOf(schedule), stepsInsideStepsOf(schedule), context, run, WhatFollows::Nothing);
    // The one schedule that does not end by typing what it has changed.
    if (schedule == FunctionSchedule::Resolve && entersTyped && !function.valuesAreTypedAsDefined() && !run.isStopped)
        runFunctionPass(PassID::RetypeValues, context, run);
    addTotals(run, numberOfValuesAtStart);
    return run.numberOfChanges || run.didMakeFacts ? PassOutcome::Changed : PassOutcome::Unchanged;
}

void PassRunner::didFinishFunctionLoop()
{
    flushFunctionPassTotals();
    if (m_build.diagnostics().hasErrors() || m_didRunLastPass.load(std::memory_order_relaxed))
        m_isStopped = true;
}

void shrinkToFit(Function& function)
{
    for (Block& block : function.storageOfBlocks()) {
        block.parameters.shrinkToFit();
        block.values.shrinkToFit();
        block.forEachTarget([](Target& target) {
            target.arguments.shrinkToFit();
            target.proofs.shrinkToFit();
        });
    }
    moveToExactBuffer(function.storageOfValues());
    moveToExactBuffer(function.storageOfBlocks());
}

void PassRunner::shrinkChangedFunctionsToFit(Program& program, const ProgramEditor& editor) const
{
    BodyIndex* index = program.bodyIndex();
    RELEASE_ASSERT(index);
    BodyIDs idsInRows = program.form() == IRForm::Prepared ? BodyIDs::Final : BodyIDs::MayBeProvisional;
    Vector<FunctionID> changed = editor.functionsWithChangedOperations();
    changed.appendVector(editor.functionsThatWereOnlyLowered());
    forEachIndexInParallel(m_build.parallelLoops(), changed.size(), [&](unsigned position) {
        Function& function = program.at(changed[position]);
        if (index->write(changed[position], function, idsInRows))
            shrinkToFit(function);
    });
}

enum class TakesHandlersAway : bool { No, Yes };

struct OperationsThatPassAnswersFor {
    PassID pass;
    InPlaceOfOperation reason;
    bool isEveryOperation;
    TakesHandlersAway takesHandlersAway;
    OpcodeSet opcodes;
};

// An operation that may throw or run code of the program goes where ObservableOperations says that it does neither, where the pass names its reason at the place (explainOperationThatGoes),
// where something new that may do the same stands at its place in the source, or where this table has the pass answer for it. Whatever else goes stops a build that verifies each pass.
static constexpr OperationsThatPassAnswersFor operationsThatPassesAnswerFor[] {
    { PassID::AddGenericVersion, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::No, { } },
    { PassID::ApplyFlowResults, InPlaceOfOperation::NothingBecauseNothingComesHere, true, TakesHandlersAway::No, { } },
    { PassID::CallModules, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::Yes, { } },
    { PassID::ConvertCountersToIntegers, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::No, { } },
    { PassID::DeferInitializers, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::No, { } },
    { PassID::EliminateTailRecursion, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::ExpandIteration, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::Yes, { } },
    { PassID::ExpandOwnKeys, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::ExpandProvenIterations, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::Yes, { } },
    { PassID::InlineCalls, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::InlineHeads, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::No, { } },
    { PassID::OutlineColdOperations, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::No, { } },
    { PassID::PlaceTypeTests, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::RemoveRedundantHandlers, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::Yes, { } },
    { PassID::RemoveUnreachableFunctions, InPlaceOfOperation::NothingBecauseNothingComesHere, true, TakesHandlersAway::No, { } },
    { PassID::ResolveAccesses, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::ResolveAwaitedCalls, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::ResolveBuiltins, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::ResolveCalls, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::RetypeValues, InPlaceOfOperation::NothingBecauseNothingComesHere, true, TakesHandlersAway::No, { } },
    { PassID::Select, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::Yes, { } },
    { PassID::SeparateUnlikelyLoopPaths, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::No, { } },
    { PassID::SinkAllocations, InPlaceOfOperation::NothingByProofOfThePass, true, TakesHandlersAway::No, { } },
    { PassID::SplitBehindEntryChecks, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::Yes, { } },
    { PassID::SplitCoroutines, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::Yes, { } },
    { PassID::SplitFreshResults, InPlaceOfOperation::TheSameOperationElsewhere, true, TakesHandlersAway::No, { } },
    { PassID::SplitUnions, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::StartGeneratorsAtFirstStep, InPlaceOfOperation::ItsExpansion, true, TakesHandlersAway::No, { } },
    { PassID::CreateStringBuilders, InPlaceOfOperation::ItsExpansion, false, TakesHandlersAway::No, { Opcode::Add, Opcode::Strcat } },
    { PassID::DeferClosuresInFields, InPlaceOfOperation::NothingByProofOfThePass, false, TakesHandlersAway::No, { Opcode::Call } },
    { PassID::DeferDefinitions, InPlaceOfOperation::NothingByProofOfThePass, false, TakesHandlersAway::No, { Opcode::CheckInitialized, Opcode::DefineMethod, Opcode::NewClass } },
    { PassID::EliminateRedundantReads, InPlaceOfOperation::NothingByProofOfThePass, false, TakesHandlersAway::No, { Opcode::Add, Opcode::AppendElements, Opcode::Call, Opcode::CallBuiltin, Opcode::CallDirect, Opcode::CallList, Opcode::Construct, Opcode::JSONAppendValue, Opcode::PrivateGet, Opcode::PropDefine, Opcode::PropGet, Opcode::PropSet, Opcode::RequireObjectCoercible } },
    { PassID::EmbedConstantCollections, InPlaceOfOperation::NothingByProofOfThePass, false, TakesHandlersAway::No, { Opcode::CallBuiltin, Opcode::PropDefine } },
    { PassID::FoldConstants, InPlaceOfOperation::NothingByProofOfThePass, false, TakesHandlersAway::No, { Opcode::PropHas } },
    { PassID::FuseConcatenations, InPlaceOfOperation::ItsExpansion, false, TakesHandlersAway::No, { Opcode::Add, Opcode::Strcat } },
    { PassID::GuardTaintedPositions, InPlaceOfOperation::NothingByProofOfThePass, false, TakesHandlersAway::No, { Opcode::ElemGet, Opcode::PropHas } },
    { PassID::RemoveUnusedParameters, InPlaceOfOperation::NothingByProofOfThePass, false, TakesHandlersAway::No, { Opcode::Add } },
    { PassID::SimplifyCasts, InPlaceOfOperation::NothingByProofOfThePass, false, TakesHandlersAway::No, { Opcode::CheckInitialized, Opcode::RequireObject, Opcode::RequireObjectCoercible } },
    { PassID::SimplifyBuiltinCalls, InPlaceOfOperation::NothingByProofOfThePass, false, TakesHandlersAway::No, { Opcode::CallBuiltin, Opcode::AwaitBySavingFrame } },
};

// A pass that runs inside another one, on a function that the other has edited, answers for the other one too.
static String findOperationNobodyAnswersFor(PassID id, const Vector<OperationThatWent>& operations, ReasonsForOperationsThatWent* listenerOutside = nullptr)
{
    const OperationsThatPassAnswersFor* entry = nullptr;
    for (const OperationsThatPassAnswersFor& candidate : operationsThatPassesAnswerFor) {
        if (candidate.pass == id)
            entry = &candidate;
    }
    for (const OperationThatWent& operation : operations) {
        bool isAnswered = entry && (operation.how == OperationThatWent::How::LostItsHandler ? entry->takesHandlersAway == TakesHandlersAway::Yes : entry->isEveryOperation || entry->opcodes.contains(operation.opcode));
        if (!isAnswered)
            return describe(operation);
        if (listenerOutside)
            listenerOutside->addForOperationThatWent(operation.value, entry->reason);
    }
    return { };
}

PassOutcome PassRunner::updateAnalyses(PassID id, const ProgramEditor& editor, ProgramAnalyses& analyses)
{
    const PassDescriptor& descriptor = descriptorOf(id);
    if (editor.changedFunctions().isEmpty()) {
        analyses.invalidate({ }, editor, descriptor.editedFunctions, descriptor.effectsAddedByEdits);
        bool changedRecords = !editor.changedLayouts().isEmpty() || !editor.changedFields().isEmpty() || !editor.changedVariables().isEmpty();
        return changedRecords ? PassOutcome::Changed : PassOutcome::Unchanged;
    }
    analyses.invalidate(descriptor.invalidatedResults, editor, descriptor.editedFunctions, descriptor.effectsAddedByEdits);
    return PassOutcome::Changed;
}

void PassRunner::checkFormAndInputs(const PassDescriptor& descriptor, const Program& program) const
{
    RELEASE_ASSERT(descriptor.accepts(program.form()));
    RELEASE_ASSERT(m_availableResults.containsAll(descriptor.inputs));
}

auto PassRunner::beginProgramPass(PassID id, const Program& program, MayChangeBodies mayChangeBodies) -> std::optional<PassTimer>
{
    const PassDescriptor& descriptor = descriptorOf(id);
    RELEASE_ASSERT(descriptor.scope == PassScope::Program);
    m_lastProgramPassOutcome = PassOutcome::Unchanged;
    if (m_isStopped)
        return std::nullopt;
    if (descriptor.necessity == PassNecessity::Optional && !m_build.isEnabled(id, FunctionID())) {
        didCompute(descriptor.outputs);
        return std::nullopt;
    }
    checkFormAndInputs(descriptor, program);
    PassTimer timer { MonotonicTime::now(), CPUTime::forCurrentThread() };
    CurrentWork& work = CurrentWork::ofCurrentThread();
    timer.enclosingPass = work.pass.load(std::memory_order_relaxed);
    timer.startOfEnclosingPassInSeconds = work.startInSeconds.load(std::memory_order_relaxed);
    work.program.store(&program, std::memory_order_relaxed);
    work.startInSeconds.store(timer.wallStart.secondsSinceEpoch().value(), std::memory_order_relaxed);
    work.finishedPart.store(nullptr, std::memory_order_relaxed);
    work.setPass(id);
    if (m_build.recordedOptions().sabotage) [[unlikely]]
        stopWhereSabotaged(m_build, std::nullopt, id);
    if (mayChangeBodies == MayChangeBodies::Yes && m_build.options().verifiesAfterEveryPass) [[unlikely]] {
        timer.bodiesBefore = std::make_shared<BodiesBeforeProgramPass>();
        timer.bodiesBefore->functions = program.functions;
        timer.bodiesBefore->reasons = Vector<ReasonsForOperationsThatWent>(program.functions.size());
        for (size_t index = 0; index < program.functions.size(); ++index)
            program.functions[index].setListenerForOperationsThatGo(&timer.bodiesBefore->reasons[index]);
    }
    return timer;
}

void PassRunner::endProgramPass(PassID id, const Program& program, PassTimer timer, PassOutcome outcome)
{
    const PassDescriptor& descriptor = descriptorOf(id);

    auto leavePass = makeScopeExit([&] {
        currentWork().setPass(timer.enclosingPass);
        currentWork().startInSeconds.store(timer.startOfEnclosingPassInSeconds, std::memory_order_relaxed);
    });
    m_lastProgramPassOutcome = outcome;
    m_build.statistics().addPassTime(id, { .wall = MonotonicTime::now() - timer.wallStart, .cpu = CPUTime::forCurrentThread() - timer.cpuStart, .runs = 1 });
    if (outcome == PassOutcome::Changed && !descriptor.countsOwnChanges)
        m_build.statistics().countChanges(id, 1);
    flushFunctionPassTotals();

    didCompute(descriptor.outputs);

    if (timer.bodiesBefore) [[unlikely]] {
        for (const Function& function : program.functions)
            function.setListenerForOperationsThatGo(nullptr);
        EffectSummaries noSummaries;
        size_t count = std::min(program.functions.size(), timer.bodiesBefore->functions.size());
        for (size_t index = 0; index < count && outcome == PassOutcome::Changed; ++index) {
            String problem = findOperationNobodyAnswersFor(id, findOperationsThatWentWithoutReason({ program, noSummaries }, timer.bodiesBefore->functions[index], program.functions[index], timer.bodiesBefore->reasons[index]));
            if (problem.isNull())
                continue;
            m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::VerificationFailedAfterPass, .function = findFunctionAsRead(&program, FunctionID(index)), .pass = id, .detail = problem });
            break;
        }
    }

    if (outcome == PassOutcome::Changed) {
        m_build.resolveFunctionToDump(program);
        verifyAfterProgramPass(descriptor, program);
    }

    if (m_build.diagnostics().hasErrors() || m_stopAfter == id || m_didRunLastPass.load(std::memory_order_relaxed))
        m_isStopped = true;
}

// The messages are worded at the end of the build, from the body that the program has then. This is the body that failed.
static String describeError(const InternalError& error, const Function& verifiedBody)
{
    StringBuilder description;
    description.append(nameOf(error.kind));
    if (error.block)
        description.append(", block b"_s, error.block.index());
    if (error.value) {
        description.append(", value v"_s, error.value.index());
        if (error.value.index() < verifiedBody.values.size())
            description.append(" ("_s, nameOf(verifiedBody.at(error.value).opcode), ')');
    }
    return description.toString();
}

void PassRunner::verifyAfterProgramPass(const PassDescriptor& descriptor, const Program& program) const
{
    if (!m_build.options().verifiesAfterEveryPass)
        return;
    if (const BodyIndex* index = program.bodyIndex())
        index->check(program, m_build.parallelLoops());
    if (descriptor.isVerifiedAfterNextPass)
        return;
    IRForm form = descriptor.produces.value_or(program.form());
    if (!canVerify(form))
        return;

    // Not ProgramAnalyses::effects(): summaries cached here would reach later passes, and the output would depend on this option.
    EffectSummaries effects;
    const PassDescriptor& summarizer = descriptorOf(PassID::ComputeEffectSummaries);
    // A pass that is run by itself was given summaries made without results of the flow analysis. What it leaves is judged by such too.
    FlowResults noResults;
    if (summarizer.accepts(program.form()) && m_build.isEnabled(summarizer.id, FunctionID()))
        effects = computeEffectSummaries(program, m_verifierInputs.flow ? *m_verifierInputs.flow : noResults, m_build);

    VerifierInputs inputs = m_verifierInputs;
    inputs.isOptimizing = form == IRForm::Prepared;
    inputs.effects = &effects;
    if (verifyProgram(program, form, m_build, inputs))
        return;
    InternalError failure { .kind = InternalErrorKind::VerificationFailedAfterPass, .pass = descriptor.id };
    for (size_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            continue;
        Vector<InternalError> errors = findErrorsInFunction(program, FunctionID(index), function, form, m_build, inputs);
        if (errors.isEmpty())
            continue;
        failure.function = findFunctionAsRead(&program, FunctionID(index));
        failure.detail = describeError(errors.first(), function);
        break;
    }
    m_build.diagnostics().add(failure);
}

bool PassRunner::mayRunAnalysesTogether() const
{
    return !m_stopAfter && !m_build.options().logging.dumpedPass;
}

void PassRunner::runTogether(NOESCAPE const ScopedLambda<void()>& first, NOESCAPE const ScopedLambda<void()>& second) const
{
    forEachIndexInParallel(m_build.parallelLoops(), 2, [&](unsigned index) {
        index ? second() : first();
    });
}

static bool hasSameWords(const Instruction& a, const Instruction& b)
{
    return a.origin() == b.origin() && a.hasSamePayloadAs(b) && a.numberOfNamedOperands() == b.numberOfNamedOperands() && std::ranges::equal(a.operands(), b.operands());
}

static OptionSet<ValueEdit> editsBetween(const Value& before, const Value& after)
{
    OptionSet<ValueEdit> edits;
    if (before.opcode != after.opcode || !hasSameWords(before, after))
        edits.add(ValueEdit::Definition);
    if (before.type != after.type || before.expected().predicted() != after.expected().predicted() || before.representation != after.representation)
        edits.add(ValueEdit::Type);
    return edits;
}

static OptionSet<BlockEdit> editsBetween(const Block& before, const Block& after)
{
    OptionSet<BlockEdit> edits;
    if (before.values != after.values)
        edits.add(BlockEdit::Values);
    if (before.parameters != after.parameters)
        edits.add(BlockEdit::Parameters);
    if (before.terminator.kind != after.terminator.kind || !hasSameWords(before.terminator, after.terminator))
        edits.add(BlockEdit::Terminator);
    auto compareTargets = [&](const Target& a, const Target& b) {
        if (a.block != b.block)
            edits.add(BlockEdit::Edges);
        if (a.arguments != b.arguments || a.proofs != b.proofs)
            edits.add(BlockEdit::Arguments);
    };
    if (before.terminator.targets.size() != after.terminator.targets.size() || !before.handler != !after.handler)
        edits.add({ BlockEdit::Edges, BlockEdit::Arguments });
    else {
        for (size_t index = 0; index < before.terminator.targets.size(); ++index)
            compareTargets(before.terminator.targets[index], after.terminator.targets[index]);
        if (before.handler)
            compareTargets(*before.handler, *after.handler);
    }
    if (before.subject != after.subject || before.role != after.role || !before.frequency.isSamePredictionAs(after.frequency) || before.shape != after.shape)
        edits.add(BlockEdit::Rest);
    return edits;
}

// Every difference between the two bodies has an entry behind the cursor that names it. None where that holds.
static String findEditWithoutEntry(const Function& before, const Function& after, const ChangeLog& log, const ChangeLog::Cursor& start)
{
    if (log.hasForgottenSince(start))
        return { };
    Vector<OptionSet<ValueEdit>> editsOfValues(after.values.size());
    Vector<OptionSet<BlockEdit>> editsOfBlocks(after.blocks.size());
    for (const ValueChange& change : log.valuesSince(start)) {
        if (change.value.index() >= after.values.size())
            return makeString("the log names v"_s, change.value.index(), ", which the function does not have"_s);
        editsOfValues[change.value.index()].add(change.edits);
    }
    for (const BlockChange& change : log.blocksSince(start)) {
        if (change.block.index() >= after.blocks.size())
            return makeString("the log names b"_s, change.block.index(), ", which the function does not have"_s);
        editsOfBlocks[change.block.index()].add(change.edits);
    }
    if (after.values.size() < before.values.size() || after.blocks.size() < before.blocks.size())
        return "the function has lost values or blocks, and the log has not forgotten"_s;
    for (size_t index = 0; index < after.values.size(); ++index) {
        OptionSet<ValueEdit> edits = index < before.values.size() ? editsBetween(before.values[index], after.values[index]) : OptionSet<ValueEdit> { ValueEdit::Added };
        if (!editsOfValues[index].containsAll(edits))
            return makeString("v"_s, index, " was edited ("_s, edits.toRaw(), ") and the log says "_s, editsOfValues[index].toRaw());
    }
    for (size_t index = 0; index < after.blocks.size(); ++index) {
        OptionSet<BlockEdit> edits = index < before.blocks.size() ? editsBetween(before.blocks[index], after.blocks[index]) : OptionSet<BlockEdit> { BlockEdit::Added };
        if (!editsOfBlocks[index].containsAll(edits))
            return makeString("b"_s, index, " was edited ("_s, edits.toRaw(), ") and the log says "_s, editsOfBlocks[index].toRaw());
    }
    return { };
}

auto PassRunner::PassClock::now() -> PassClock
{
    return { CPUTime::forCurrentThread(), InstructionCounts::ofCurrentThread() };
}

// A pass on a function of many values is timed. Of the small functions one in sixteen is, and stands for all sixteen: which ones does not depend on the run.
static uint32_t numberOfRunsThatTimedRunStandsFor(FunctionID function, size_t numberOfValues)
{
    AOT_THRESHOLD(size_t, fewestValuesOfFunctionThatIsAlwaysTimed, 64);
    AOT_THRESHOLD(uint32_t, smallFunctionsForEachTimedOne, 16);
    if (numberOfValues >= fewestValuesOfFunctionThatIsAlwaysTimed)
        return 1;
    return function.index() % smallFunctionsForEachTimedOne ? 0 : smallFunctionsForEachTimedOne;
}

PassOutcome PassRunner::runFunctionPass(PassID id, FunctionPassContext& context, ScheduleRun& run) const
{
    const unsigned index = static_cast<unsigned>(id);
    const bool verifies = m_build.options().verifiesAfterEveryPass;

    context.changes.settle();
    const bool reachedFixpoint = run.reachedFixpoint.get(index);
    run.reachedFixpoint.clear(index);
    if (run.passesThatRan.contains(id) && !run.passesWithWork.get(index)) {
        if (reachedFixpoint || run.passesEnabledByTable.get(index)) {
            // Leave the same state behind as the idle run that this skips.
            ++run.numberOfSkippedRuns;
            run.numberOfValuesWalked += context.function.values.size();
            const bool waits = run.passesThatWait.get(index);
            if (verifies && !waits) [[unlikely]]
                verifySkippedFunctionPass(descriptorOf(id), context, run);
            run.didLookAtFunction(id);
            if (waits)
                run.passesThatWait.set(index);
        } else if (run.passesBehindChange.get(index)) {
            ++run.numberOfSkippedRuns;
            if (verifies && !run.passesThatWait.get(index)) [[unlikely]]
                verifySkippedFunctionPass(descriptorOf(id), context, run);
        }
        return PassOutcome::Unchanged;
    }
    const PassDescriptor& descriptor = descriptorOf(id);
    RELEASE_ASSERT(descriptor.scope == PassScope::Function);
    if (descriptor.necessity == PassNecessity::Optional && !m_build.isEnabled(id, run.functionAsRead))
        return PassOutcome::Unchanged;
    if (run.optionalPassesOfShortSchedule && descriptor.necessity == PassNecessity::Optional && !run.optionalPassesOfShortSchedule->contains(id))
        return PassOutcome::Unchanged;
    checkFormAndInputs(descriptor, context.program);
    RELEASE_ASSERT(!descriptor.derivesFunctions || context.inputs.derivedFunctions);

    if (hasTriggers(id) && !run.mayFire(id, context.function, context.changes)) {
        // All that later runs go by is as it is behind a run that has found nothing to do.
        if (run.notes) [[unlikely]]
            ++run.notes->runsWithoutTrigger[index];
        run.numberOfValuesWalked += context.function.values.size();
        if (verifies) [[unlikely]]
            verifySkippedFunctionPass(descriptor, context, run);
        run.didRun(id);
        run.didLookAtFunction(id);
        stopBehind(id, run);
        return PassOutcome::Unchanged;
    }
    return runFunctionPassThatMayFire(id, context, run);
}

// Until the program is analyzed as a whole, what the passes make of library code is the same in every build. A fuzzer makes millions of builds.
static bool isVerifiedElsewhere(const FunctionPassContext& context)
{
#if ENABLE(FUZZILLI)
    return context.program.form() == IRForm::AsRead && isLibraryCode(context.program, context.function);
#else
    UNUSED_PARAM(context);
    return false;
#endif
}

// Apart from the steps that are left out, which are nine in ten and need none of what a run needs.
// These put operations where the program had none: in front of a loop, on the edges into it, at the head of a web. What they may put there is what names nothing that is defined behind the place.
// Other passes put values in place of values, cut the code between a value and a cast of it and make casts where values are used, and move no cast: so each cast that stands apart from its value holds where
// it stands, and is tied there before anything leaves a loop.
static constexpr PassSet passesThatMoveOperationsOutOfLoops {
    PassID::HoistLoopInvariantReads, PassID::PlaceTypeTests, PassID::SeparateUnlikelyLoopPaths, PassID::CreateStringBuilders, PassID::PlanArrayCapacities, PassID::PlanCollectionCapacities, PassID::Select,
};

NEVER_INLINE PassOutcome PassRunner::runFunctionPassThatMayFire(PassID id, FunctionPassContext& context, ScheduleRun& run) const
{
    const PassDescriptor& descriptor = descriptorOf(id);
    const unsigned index = static_cast<unsigned>(id);

    auto tieCasts = [&] {
        if (run.numberOfChangesWhenCastsWereTied == run.numberOfChanges)
            return;
        // A fact gives no pass anything to do, so no round follows for its sake.
        if (Vector<BlockID, 4> changedBlocks = tieCastsWhereTheyStand(context.program, context.function); !changedBlocks.isEmpty()) {
            context.analyses.didAddDetachedValue({ });
            for (BlockID block : changedBlocks)
                context.analyses.didChangeBlock(block);
            run.contents = std::nullopt;
            run.didMakeFacts = true;
        }
        run.numberOfChangesWhenCastsWereTied = run.numberOfChanges;
    };
    if (passesThatMoveOperationsOutOfLoops.contains(id) && context.program.form() == IRForm::Prepared) {
        const OpcodeSet& opcodes = run.contentsOf(context.function, context.changes).opcodes;
        if (opcodes.contains(Opcode::LoopHint) && opcodes.contains(Opcode::AssumeType)) {
            tieCasts();
            if (m_build.options().verifiesProgram) [[unlikely]] {
                if (Vector<CastAwayFromItsValue, 4> found = findCastsAwayFromTheirValues(context.program, context.function); !found.isEmpty()) {
                    m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::CastWithoutFact, .function = context.id, .block = found[0].block, .value = found[0].cast, .pass = id });
                    run.isStopped = true;
                    return PassOutcome::Unchanged;
                }
            }
        }
    }
    const bool isTimed = m_build.options().logging.logsPhaseTimes;
    size_t numberOfValuesBefore = context.function.values.size();
    const uint32_t weight = isTimed ? numberOfRunsThatTimedRunStandsFor(run.functionAsRead, numberOfValuesBefore) : 0;
    PassClock start;
    if (weight)
        start = run.clockAtEndOfLastPass ? *run.clockAtEndOfLastPass : PassClock::now();
    run.clockAtEndOfLastPass = std::nullopt;
    AnalysisSet analysesBefore = isTimed ? context.analyses.computed() : AnalysisSet { };
    ++run.runs[index];
    run.numberOfValuesWalked += numberOfValuesBefore;
    context.analyses.setRunningPass(id);
    context.keptAnalyses = { };
    context.reachedFixpoint = false;
    context.passesLeftIdle = { };
    context.passesThatCanWait = { };
    context.lookedOnlyNearby = false;
    context.mayDecideTestsAcrossBlocks = run.mayDecideTestsAcrossBlocks;
    context.opcodesWhereRunBegins = hasTriggers(id) ? &run.contentsOf(context.function, context.changes).opcodes : nullptr;
    if (id == PassID::SimplifyControlFlow)
        run.mayDecideTestsAcrossBlocks = false;
    std::optional<Function> functionBeforeRun;
    ChangeLog::Cursor logBeforeRun;
    if (m_build.options().verifiesAfterEveryPass && !isVerifiedElsewhere(context)) [[unlikely]]
        functionBeforeRun.emplace(context.function);
    if (functionBeforeRun || isTimed) [[unlikely]]
        logBeforeRun = context.changes.end();
    std::optional<ReasonsForOperationsThatWent> reasons;
    if (functionBeforeRun) [[unlikely]] {
        reasons.emplace(context.function.listenerForOperationsThatGo());
        context.function.setListenerForOperationsThatGo(&*reasons);
    }
    currentWork().setPass(id);
    PassOutcome outcome = descriptor.run(context);
    if (reasons) [[unlikely]]
        context.function.setListenerForOperationsThatGo(reasons->outer());
    auto stopWhereThereIsOne = [&](const String& problem) {
        if (problem.isNull())
            return;
        m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::VerificationFailedAfterPass, .function = run.functionAsRead, .pass = id, .detail = problem });
        run.isStopped = true;
    };
    if (m_build.options().checksThatUsedValuesAreListed && outcome == PassOutcome::Changed) [[unlikely]]
        stopWhereThereIsOne(findValueListedWrongly(context.function));
    if (functionBeforeRun) [[unlikely]] {
        stopWhereThereIsOne(findEditWithoutEntry(*functionBeforeRun, context.function, context.changes, logBeforeRun));
        if (outcome == PassOutcome::Changed)
            stopWhereThereIsOne(findOperationNobodyAnswersFor(id, findOperationsThatWentWithoutReason({ context.program, context.inputs.effects }, *functionBeforeRun, context.function, *reasons), &*reasons));
    }
    if (isTimed) {
        if (outcome == PassOutcome::Unchanged && !context.analyses.computed().containsAll(analysesBefore))
            m_functionPassTotals[index].needlessDrops.fetch_add(1, std::memory_order_relaxed);
        if (context.changes.hasForgottenSince(logBeforeRun))
            m_functionPassTotals[index].runsThatMadeLogForget.fetch_add(1, std::memory_order_relaxed);
        else if (size_t entries = context.changes.numberOfEntriesSince(logBeforeRun)) {
            m_functionPassTotals[index].logEntries.fetch_add(entries, std::memory_order_relaxed);
            if (outcome == PassOutcome::Unchanged)
                m_functionPassTotals[index].idleRunsWithLogEntries.fetch_add(1, std::memory_order_relaxed);
        }
        uint64_t nanoseconds = 0;
        if (weight) {
            PassClock end = PassClock::now();
            nanoseconds = (end.time - start.time).nanosecondsAs<uint64_t>() * weight;
            if (start.instructions && end.instructions)
                run.notes->instructions[index] += (*end.instructions - *start.instructions) * weight;
            run.clockAtEndOfLastPass = end;
        }
        run.notes->nanoseconds[index] += nanoseconds;
        if (outcome == PassOutcome::Unchanged)
            m_functionPassTotals[index].nanosecondsWithoutChange.fetch_add(nanoseconds, std::memory_order_relaxed);
        m_functionPassTotals[index].valuesAdded.fetch_add(static_cast<int64_t>(context.function.values.size()) - static_cast<int64_t>(numberOfValuesBefore), std::memory_order_relaxed);
    }

    run.didRun(id);
    run.didLookAtFunction(id);
    run.reachedFixpoint.set(index, outcome == PassOutcome::Changed && context.reachedFixpoint);
    if (outcome == PassOutcome::Changed) {
        invalidateAnalyses(context.analyses, descriptor.invalidatedAnalyses, context.keptAnalyses);
        if (run.notes && !context.keptAnalyses.isEmpty()) [[unlikely]]
            ++run.notes->runsThatKeptAnalyses[index];
        run.contents = std::nullopt;
        run.didChange(id, context.passesLeftIdle, context.passesThatCanWait);
        if (run.reachedFixpoint.get(index))
            run.didLookAtFunction(id);
        const bool mayLeaveCodeBehindNeverValues = !passesThatLeaveNoCodeBehindNeverValues.contains(id);
        if (descriptor.produces.value_or(context.program.form()) == IRForm::Prepared && !descriptor.isVerifiedAfterNextPass && (mayLeaveCodeBehindNeverValues || m_build.options().verifiesAfterEveryPass) && cutCodeBehindNeverValues(context, run.contentsOf(context.function, context.changes))) {
            RELEASE_ASSERT(mayLeaveCodeBehindNeverValues);
            run.contents = std::nullopt;
            run.didChange(PassID::None);
        }
        ++run.numberOfChanges;
        ++run.changes[index];
        if (isTimed)
            run.changingRuns.append(id);
    }

    if (context.lookedOnlyNearby)
        run.passesThatWait.set(index);
    verifyAfterFunctionPass(descriptor, context, run);
    stopBehind(id, run);
    return outcome;
}

void PassRunner::stopBehind(PassID id, ScheduleRun& run) const
{
    if (m_stopAfter != id) [[likely]]
        return;
    run.isStopped = true;
    m_didRunLastPass.store(true, std::memory_order_relaxed);
}

void PassRunner::verifyAfterFunctionPass(const PassDescriptor& descriptor, FunctionPassContext& context, ScheduleRun& run) const
{
    if (!m_build.options().verifiesAfterEveryPass || isVerifiedElsewhere(context)) [[likely]]
        return;
    IRForm form = descriptor.produces.value_or(context.program.form());
    VerifierInputs inputs = m_verifierInputs;
    inputs.isOptimizing = form == IRForm::Prepared;
    if (!inputs.effects)
        inputs.effects = &context.inputs.effects;
    // A program pass that is given no summaries simplifies functions which the Optimize loop has left: their guards stand across calls that only the summaries call harmless.
    inputs.cannotJudgeGuards = context.inputs.effects.isEmpty() && m_build.isEnabled(PassID::ComputeEffectSummaries, FunctionID());
    inputs.otherFunctionsMayChange = true;
    const uint32_t numberOfEdits = context.function.numberOfEdits();
    const bool isAsVerified = run.verified && run.verified->numberOfEdits == numberOfEdits && run.verified->form == form && run.verified->cannotJudgeGuards == inputs.cannotJudgeGuards;
    if (isAsVerified && run.verified->analyses == context.analyses.computed())
        return;
    const bool verifiesFunction = canVerify(form) && !descriptor.isVerifiedAfterNextPass;
    Vector<InternalError> errors;
    if (verifiesFunction && !isAsVerified) {
        errors = findErrorsInFunction(context.program, context.id, context.function, form, m_build, inputs);
        bool someGuardDoesNotLast = std::ranges::any_of(errors, [](const InternalError& error) {
            return error.kind == InternalErrorKind::GuardNotAvailable;
        });
        if (someGuardDoesNotLast && inputs.effectsByTypesOfOperands) {
            VerifierInputs finer = inputs;
            finer.effects = inputs.effectsByTypesOfOperands;
            errors = findErrorsInFunction(context.program, context.id, context.function, form, m_build, finer);
        }
    }
    if (errors.isEmpty() && !isAsVerified && hasTypedValues(context.function)) {
        // In an arena of its own: what is interned here must not reach the program, or the output would depend on this option.
        ScratchTypeArena types(context.program.types);
        if (std::optional<BrokenResultRule> broken = findBrokenResultRule(context.program, context.function, types, run.shapesWithMonotoneResult)) {
            errors.append(InternalError { .kind = InternalErrorKind::BrokenResultRule, .function = context.id, .value = broken->value, .detail = makeString("operand "_s, broken->operandIndex, "; laws-of-result-rules of testaot prints the types"_s) });
        }
    }
    if (errors.isEmpty()) {
        run.verified = std::nullopt;
        if (!context.analyses.verifyCached())
            run.isStopped = true;
        else if (verifiesFunction || isAsVerified)
            run.verified = ScheduleRun::VerifiedState { numberOfEdits, form, inputs.cannotJudgeGuards, context.analyses.computed() };
        return;
    }

    // A listing of many errors is cut short, so the line that names the pass also says what the verifier found.
    InternalError failure { .kind = InternalErrorKind::VerificationFailedAfterPass, .function = run.functionAsRead, .pass = descriptor.id, .detail = describeError(errors.first(), context.function) };
    for (InternalError& error : errors)
        error.pass = descriptor.id;
    m_build.diagnostics().append(WTF::move(errors));
    m_build.diagnostics().add(failure);
    run.isStopped = true;
}

static String describeChangers(const PassSet& changers)
{
    StringBuilder description;
    description.append("after "_s);
    bool isFirst = true;
    for (PassID id : allMembersOf<PassID>) {
        if (!changers.contains(id))
            continue;
        if (!std::exchange(isFirst, false))
            description.append(", "_s);
        description.append(nameOf(id));
    }
    return description.toString();
}

static String describeFirstDifference(const Function& function, const Function& changed)
{
    StringPrintStream out;
    if (function.blocks.size() != changed.blocks.size() || function.values.size() != changed.values.size()) {
        out.print("; ", function.blocks.size(), " blocks and ", function.values.size(), " values become ", changed.blocks.size(), " and ", changed.values.size());
        return out.toString();
    }
    for (size_t index = 0; index < function.values.size(); ++index) {
        const Value& before = function.values[index];
        const Value& after = changed.values[index];
        if (before.opcode == after.opcode && before.type == after.type && before.expected().predicted() == after.expected().predicted())
            continue;
        out.print("; v", index, " (", nameOf(before.opcode), ")");
        if (before.opcode != after.opcode)
            out.print(" becomes ", nameOf(after.opcode));
        if (before.type != after.type)
            out.print(" gets another type");
        if (before.expected().predicted() != after.expected().predicted())
            out.print(" is expected to be of another type");
        break;
    }
    return out.toString();
}

// The table says that the pass finds nothing to do here. A run on a copy shows whether that is true.
void PassRunner::verifySkippedFunctionPass(const PassDescriptor& descriptor, const FunctionPassContext& context, ScheduleRun& run) const
{
    if (!m_build.options().verifiesAfterEveryPass || isVerifiedElsewhere(context)) [[likely]]
        return;
    Function copy(context.function);
    FunctionAnalyses analyses(context.analyses.context(), copy);
    FunctionPassContext contextOfCopy { context.program, context.inputs, context.id, copy, analyses, context.types, context.strings, m_build };
    contextOfCopy.pendingTyping = context.pendingTyping.forCopyWith(contextOfCopy.changes, context.changes);
    contextOfCopy.mayDecideTestsAcrossBlocks = run.mayDecideTestsAcrossBlocks;
    currentWork().setPass(descriptor.id);
    if (descriptor.run(contextOfCopy) == PassOutcome::Unchanged)
        return;

    const PassSet& changers = run.notes->changersSinceLastRun[static_cast<unsigned>(descriptor.id)];
    m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::SkippedPassChangesFunction, .function = run.functionAsRead, .pass = descriptor.id, .detail = makeString(describeChangers(changers), describeFirstDifference(context.function, copy)) });
    run.isStopped = true;
}

void PassRunner::runSteps(std::span<const ScheduleStep> steps, std::span<const StepsInside> stepsInside, FunctionPassContext& context, ScheduleRun& run, WhatFollows whatFollows) const
{
    auto doNothing = [&](const StepsInside& inside) {
        return m_everyFixpointMayTakeRound && inside.mayBeLeftOutAtRest && run.areAtRest(inside.passes);
    };
    for (size_t index = 0; index < steps.size() && !run.isStopped; ++index) {
        const ScheduleStep& step = steps[index];
        WhatFollows whatFollowsStep = index + 1 == steps.size() ? whatFollows : WhatFollows::MoreSteps;
        switch (step.kind) {
        case ScheduleStep::Kind::RunPass:
            if (!step.isBehindRounds && run.isAtRest(step.pass))
                break;
            if (step.isBehindRounds) {
                // It has run in the rounds and changed nothing there, on purpose.
                context.isBehindRounds = true;
                run.giveWorkTo(step.pass);
            }
            context.isLastStepOfSchedule = whatFollowsStep == WhatFollows::Nothing;
            runFunctionPass(step.pass, context, run);
            context.isLastStepOfSchedule = false;
            context.isBehindRounds = false;
            break;
        case ScheduleStep::Kind::RunSchedule: {
            if (doNothing(stepsInside[index]))
                break;
            std::span<const ScheduleStep> included = stepsOf(step.schedule);
            if (!step.compacts)
                included = included.first(included.size() - 1);
            runSteps(included, stepsInsideStepsOf(step.schedule).first(included.size()), context, run, whatFollowsStep);
            break;
        }
        case ScheduleStep::Kind::BeginFixpoint: {
            size_t end = index + stepsInside[index].numberOfStepsToEnd;
            std::span<const ScheduleStep> body = steps.subspan(index + 1, end - index - 1);
            const uint64_t maxRounds = m_build.limit(step.limit);
            bool wasInRounds = std::exchange(context.isInRounds, true);
            for (uint64_t round = 0; ; ++round) {
                if (round >= maxRounds) {
                    m_build.gaveUp(step.limit, context.subject());
                    break;
                }
                if (round >= run.maxRounds)
                    break;
                if (round && run.numberOfValuesWalked > m_build.limit(LimitID::MaxOptimizationWork)) {
                    if (!std::exchange(run.hasSpentItsWork, true))
                        m_build.gaveUp(LimitID::MaxOptimizationWork, context.subject());
                    break;
                }
                if (doNothing(stepsInside[index]))
                    break;
                uint32_t numberOfChangesBefore = run.numberOfChanges;
                runSteps(body, stepsInside.subspan(index + 1, end - index - 1), context, run);
                if (run.numberOfChanges == numberOfChangesBefore || run.isStopped)
                    break;
            }
            context.isInRounds = wasInRounds;
            index = end;
            break;
        }
        case ScheduleStep::Kind::EndFixpoint:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
}

void PassRunner::addTotals(const ScheduleRun& run, size_t numberOfValuesAtStart) const
{
    if (!m_hasFunctionPassTotals.load(std::memory_order_relaxed))
        m_hasFunctionPassTotals.store(true, std::memory_order_relaxed);
    if (run.numberOfSkippedRuns)
        m_build.statistics().count(StatisticID::FunctionPassRunsSkipped, run.numberOfSkippedRuns);
    if (m_build.options().logging.logsPhaseTimes) {
        FunctionPassTotals& totals = m_totalsBySize[std::min<unsigned>(std::bit_width(numberOfValuesAtStart), numberOfSizeClasses - 1)];
        totals.runs.fetch_add(1, std::memory_order_relaxed);
        totals.changes.fetch_add(numberOfValuesAtStart, std::memory_order_relaxed);
        uint64_t nanoseconds = std::reduce(run.notes->nanoseconds.begin(), run.notes->nanoseconds.end(), uint64_t { 0 });
        totals.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
        constexpr uint64_t nanosecondsOfSlowFunction = 500'000'000;
        if (nanoseconds >= nanosecondsOfSlowFunction) {
            StringPrintStream passes;
            for (unsigned index = 0; index < numberOfPasses; ++index) {
                if (run.notes->nanoseconds[index] * 20 >= nanoseconds)
                    passes.print(" | ", nameOf(static_cast<PassID>(index)), " ", run.notes->nanoseconds[index] / 1'000'000, " ms in ", run.runs[index], " runs");
            }
            dataLogLn("AOT slow function: ", numberOfValuesAtStart, " values at the start | cpu ", nanoseconds / 1'000'000, " ms | ", run.numberOfValuesWalked / 1'000'000, " M values walked", passes.toString());
            StringPrintStream repeatedPasses;
            for (unsigned index = 0; index < numberOfPasses; ++index) {
                if (run.runs[index] >= 10)
                    repeatedPasses.print(" | ", nameOf(static_cast<PassID>(index)), " ", run.runs[index], " runs, ", run.changes[index], " changed");
            }
            dataLogLn("AOT repeated passes: ", numberOfValuesAtStart, " values at the start", repeatedPasses.toString());
            StringPrintStream changingRuns;
            for (PassID pass : run.changingRuns)
                changingRuns.print(" ", nameOf(pass));
            dataLogLn("AOT changing runs: ", numberOfValuesAtStart, " values at the start |", changingRuns.toString());
        }
    }
    static std::atomic<unsigned> numberOfThreadsWithShare { 0 };
    static thread_local unsigned shareOfThread = numberOfThreadsWithShare.fetch_add(1, std::memory_order_relaxed) % numberOfSharesOfCounts;
    CountsOfPasses& counts = m_countsOfPasses[shareOfThread];
    run.passesThatRan.forEach([&](PassID pass) {
        const unsigned index = static_cast<unsigned>(pass);
        if (!run.runs[index])
            return;
        counts[index].runs.fetch_add(run.runs[index], std::memory_order_relaxed);
        if (uint32_t changes = run.changes[index])
            counts[index].changes.fetch_add(changes, std::memory_order_relaxed);
    });
    if (!run.notes) [[likely]]
        return;
    run.passesThatRan.forEach([&](PassID pass) {
        const unsigned index = static_cast<unsigned>(pass);
        FunctionPassTotals& totals = m_functionPassTotals[index];
        if (uint32_t runsWithoutTrigger = run.notes->runsWithoutTrigger[index])
            totals.runsWithoutTrigger.fetch_add(runsWithoutTrigger, std::memory_order_relaxed);
        if (!run.runs[index])
            return;
        if (uint32_t runsThatKeptAnalyses = run.notes->runsThatKeptAnalyses[index])
            totals.runsThatKeptAnalyses.fetch_add(runsThatKeptAnalyses, std::memory_order_relaxed);
        if (uint64_t nanoseconds = run.notes->nanoseconds[index])
            totals.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
        if (uint64_t instructions = run.notes->instructions[index])
            totals.instructions.fetch_add(instructions, std::memory_order_relaxed);
    });
}

void PassRunner::flushFunctionPassTotals()
{
    // Most program passes run no schedule of function passes, and the counts are thousands of words.
    if (!m_hasFunctionPassTotals.exchange(false, std::memory_order_relaxed) && !m_build.options().logging.logsPhaseTimes)
        return;
    for (PassID id : allMembersOf<PassID>) {
        FunctionPassTotals& totals = m_functionPassTotals[static_cast<unsigned>(id)];
        uint64_t runs = 0;
        uint64_t changes = 0;
        for (CountsOfPasses& counts : m_countsOfPasses) {
            runs += counts[static_cast<unsigned>(id)].runs.exchange(0, std::memory_order_relaxed);
            changes += counts[static_cast<unsigned>(id)].changes.exchange(0, std::memory_order_relaxed);
        }
        uint64_t runsWithoutTrigger = totals.runsWithoutTrigger.exchange(0, std::memory_order_relaxed);
        if (runsWithoutTrigger && m_build.options().logging.logsPhaseTimes)
            dataLogLn("AOT function loop: ", nameOf(id), " | ", runsWithoutTrigger, " runs left out: the body holds nothing that the pass looks for | ", totals.runsThatKeptAnalyses.exchange(0, std::memory_order_relaxed), " changing runs kept analyses");
        if (!runs)
            continue;
        uint64_t nanoseconds = totals.nanoseconds.exchange(0, std::memory_order_relaxed);
        if (int64_t valuesAdded = totals.valuesAdded.exchange(0, std::memory_order_relaxed))
            dataLogLn("AOT function loop: ", nameOf(id), " | ", valuesAdded, " values added");
        if (m_build.options().logging.logsPhaseTimes)
            dataLogLn("AOT function loop: ", nameOf(id), " | ", runs, " runs | ", changes, " changed the function | ", totals.needlessDrops.exchange(0, std::memory_order_relaxed), " changed nothing and dropped analyses | cpu ", nanoseconds / 1'000'000, " ms, ", totals.nanosecondsWithoutChange.exchange(0, std::memory_order_relaxed) / 1'000'000, " ms of it in runs that changed nothing | instructions ", totals.instructions.exchange(0, std::memory_order_relaxed) / 1'000'000, " M | log entries ", totals.logEntries.exchange(0, std::memory_order_relaxed), " | runs that made the log forget ", totals.runsThatMadeLogForget.exchange(0, std::memory_order_relaxed), " | idle runs with entries ", totals.idleRunsWithLogEntries.exchange(0, std::memory_order_relaxed));
        m_build.statistics().addPassTime(id, { .wall = Seconds(0), .cpu = Seconds::fromNanoseconds(static_cast<double>(nanoseconds)), .runs = runs });
        m_build.statistics().countChanges(id, changes);
    }
    for (unsigned sizeClass = 0; sizeClass < numberOfSizeClasses; ++sizeClass) {
        FunctionPassTotals& totals = m_totalsBySize[sizeClass];
        uint64_t functions = totals.runs.exchange(0, std::memory_order_relaxed);
        uint64_t values = totals.changes.exchange(0, std::memory_order_relaxed);
        uint64_t nanoseconds = totals.nanoseconds.exchange(0, std::memory_order_relaxed);
        if (functions)
            dataLogLn("AOT function loop: functions of fewer than ", 1u << sizeClass, " values | ", functions, " functions | ", values, " values | cpu ", static_cast<double>(nanoseconds) / 1e6, " ms");
    }
    if (m_build.options().logging.logsPhaseTimes)
        FunctionAnalyses::logBuilds();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
