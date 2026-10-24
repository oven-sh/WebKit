/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPipelineInternal.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTAllocateInnerObjects.h"
#include "AOTBackgroundTask.h"
#include "AOTBorrowedParameters.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTCallModules.h"
#include "AOTCheckTypesAtEntry.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCoroutines.h"
#include "AOTCreateEntrypoints.h"
#include "AOTCreatedObjectLayouts.h"
#include "AOTDeferClosuresInFields.h"
#include "AOTDeferDefinitions.h"
#include "AOTDeferInitializers.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsCheck.h"
#include "AOTElideStackChecks.h"
#include "AOTEmbedConstantCollections.h"
#include "AOTExternalFunctionBodies.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTFreshResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeat.h"
#include "AOTIR.h"
#include "AOTInlineHeads.h"
#include "AOTLongExpectations.h"
#include "AOTMarkResultsUsedAsKeys.h"
#include "AOTNameCreatedSymbols.h"
#include "AOTNarrowConstantVariables.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTOutlineColdOperations.h"
#include "AOTOutliningCandidates.h"
#include "AOTPassList.h"
#include "AOTPhaseTimes.h"
#include "AOTPlacement.h"
#include "AOTPromoteEnvironmentsToVariables.h"
#include "AOTReadCapturesFromEnvironments.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpRequests.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTRemoveUnreadExports.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTRemoveUnusedParameters.h"
#include "AOTRemoveUnusedResults.h"
#include "AOTReuseEnvironments.h"
#include "AOTSlotPlan.h"
#include "AOTSplitBehindEntryChecks.h"
#include "AOTStringArena.h"
#include "AOTSummarizeForInlining.h"
#include "AOTTaggedLayouts.h"
#include "AOTThreads.h"
#include "AOTValueFlowResults.h"
#include "AOTVerifier.h"
#include "AOTVirtualObjectParameters.h"
#include "AOTWholeLiterals.h"
#include <algorithm>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// What the steps between selection and the choice of representations take from one body by itself: what it names, for the call graph, and what has to be numbered.
class FactsOfSelectedBodies {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FactsOfSelectedBodies);
    WTF_MAKE_NONCOPYABLE(FactsOfSelectedBodies);
public:
    explicit FactsOfSelectedBodies(Pipeline& pipeline)
        : m_program(pipeline.program())
        , m_flow(pipeline.flow())
        , m_build(pipeline.runner().build())
        , m_inputsOfScans(pipeline.createPlacementInputs(m_noCallGraph))
        , m_scansForPlacement(m_program, m_inputsOfScans, m_build)
        , m_entriesThatRunNoCode(findEntriesThatRunNoCode())
        , m_references(m_program.functions.size())
    {
    }

    // From any thread, no two for one function at a time, while the caches of the program and an interning phase of its types are open. The body holds no provisional id.
    void read(FunctionID id)
    {
        readReferences(id);
        m_scansForPlacement.scan(id);
    }

    // The first half of read(), for a caller that takes many functions through one half and then through the other.
    void readReferences(FunctionID id)
    {
        m_references[id.index()] = makeUniqueWithoutFastMallocCheck<ReferencesOfBody>(collectReferencesOf(m_program, m_flow, id, m_entriesThatRunNoCode));
    }

    // Null for a function that was not read.
    const ReferencesOfBody* referencesOf(FunctionID id) const { return id.index() < m_references.size() ? m_references[id.index()].get() : nullptr; }

    // By one thread, while nobody reads.
    void didAddFunctions()
    {
        m_references.grow(m_program.functions.size());
        m_scansForPlacement.didAddFunctions();
    }

    void giveReferencesTo(ProgramAnalyses& analyses)
    {
        analyses.takeReferencesOfBodies(m_references.span());
        m_build.destroyLater(WTF::move(m_references));
    }

    BodyScansForPlacement& scansForPlacement() { return m_scansForPlacement; }

private:
    const Program& m_program;
    const FlowResults& m_flow;
    BuildContext& m_build;
    // The scans ask nothing of it.
    CallGraph m_noCallGraph;
    PlacementInputs m_inputsOfScans;
    BodyScansForPlacement m_scansForPlacement;
    EntriesThatRunNoCode m_entriesThatRunNoCode;
    Vector<std::unique_ptr<ReferencesOfBody>> m_references;
};

class Pipeline::TimedStage : public PhaseTimes {
public:
    TimedStage(Pipeline& pipeline, Stage stage)
        : PhaseTimes(pipeline.m_build, stage)
        , m_pipeline(pipeline)
    {
    }

    // What a stage does between two passes. Without a line of its own it counts as time of the pass behind it.
    using PhaseTimes::didFinish;

    void run(PassID id)
    {
        m_pipeline.runProgramPass(id);
        didFinish(nameOf(id));
    }

    void run(FunctionSchedule schedule, const StepsAroundSchedule& steps = { })
    {
        m_pipeline.runSchedule(schedule, steps);
        didFinish(descriptorOf(schedule).name);
    }

    void verify(IRForm form)
    {
        m_pipeline.verify(form);
        didFinish(nameOf(PassID::VerifyProgram));
    }

    void verifyOnRequest(IRForm form)
    {
        if (m_pipeline.m_build.options().verifiesAfterEveryStage)
            verify(form);
    }

    // The program that is read, where it is placed, and the program that goes to the back end. The front end and the passes are ours, so a build of a user's does not pay for it: every test does.
    bool verifiesProgram() const
    {
        const auto& options = m_pipeline.m_build.options();
        return options.verifiesProgram || options.isVerifyOnly();
    }

    void verifyAtBoundary(IRForm form)
    {
        if (verifiesProgram())
            verify(form);
    }

    void verifyAtEndOfPreparedForm()
    {
        BuildContext& build = m_pipeline.m_build;
        if (build.options().verifiesAfterEveryStage) {
            verify(IRForm::Prepared);
            return;
        }
        if (!verifiesProgram())
            return;
        bool isRunning = !m_pipeline.m_runner.isStopped();
        if (!m_pipeline.read(PassID::VerifyProgram, verifyUnspecifiedValues, m_pipeline.m_program, build) && isRunning)
            build.diagnostics().add(InternalError { .kind = InternalErrorKind::VerificationFailedAtTheEnd });
        didFinish(nameOf(PassID::VerifyProgram));
    }

private:
    Pipeline& m_pipeline;
};

static std::unique_ptr<PipelineOutput> createOutput(BuildContext& build)
{
    auto output = makeUnique<PipelineOutput>();
    output->derivedFunctions = makeUnique<DerivedFunctions>(build);
    auto createDefaultResults = []<typename... Results>(std::unique_ptr<Results>&... results) {
        ((results = makeUniqueWithoutFastMallocCheck<Results>()), ...);
    };
    createDefaultResults(output->flow, output->effects, output->heat, output->callingConventions, output->frameAllocation, output->environmentReuse, output->literalStrings,
        output->constantCollections, output->slots, output->regExpRequests, output->stackChecks, output->placement, output->siteCaches);
    return output;
}

static void checkEffectSummaries(const Program& program, const FlowResults& flow, const EffectSummaries& effects, BuildContext& build)
{
    EffectContext context { program, effects };
    EffectSummaries seed = createSummariesFromFlow(program, flow, build);
    EffectContext contextOfSeed { program, seed };
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        FunctionID id(index);
        auto analyses = createAnalysesForSummary(build, contextOfSeed, flow, id, function);
        checkSummaryCoversBody(context, id, *analyses, build.diagnostics());
    });
}

PipelineOutput::PipelineOutput() = default;

PipelineOutput::~PipelineOutput()
{
    waitForRegExps();
}

void PipelineOutput::waitForRegExps()
{
    if (compilationOfRegExps)
        compilationOfRegExps->finish();
}

void PipelineOutput::startCompilingRegExps(const Program& programWithRegExps, BuildContext& build)
{
    RELEASE_ASSERT(!compilationOfRegExps);
    compiledRegExps = makeUniqueWithoutFastMallocCheck<CompiledRegExps>();
    compilationOfRegExps = BackgroundTask::start(build.parallelLoops(), [this, &programWithRegExps, &build] {
        *compiledRegExps = compileRegExps(programWithRegExps, *regExpRequests, build);
    });
}

CompiledRegExps PipelineOutput::takeCompiledRegExps(BuildContext& build)
{
    if (!compilationOfRegExps)
        return compileRegExps(*program, *regExpRequests, build);
    compilationOfRegExps->finish();
    compilationOfRegExps = nullptr;
    return WTF::move(*std::exchange(compiledRegExps, nullptr));
}

void PipelineOutput::releaseCodeGenerationInputs(BuildContext& build)
{
    auto destroyLater = [&](auto&... results) {
        (build.destroyLater(WTF::move(results)), ...);
    };
    destroyLater(derivedFunctions, effects, callingConventions, frameAllocation, environmentReuse, literalStrings, constantCollections, slots, stackChecks, siteCaches, calleesOfSites);
}

Pipeline::Pipeline(Program& program, PassRunner& runner)
    : m_output(createOutput(runner.build()))
    , m_program(program)
    , m_runner(runner)
    , m_build(runner.build())
    , m_analyses(program, runner, *m_output->derivedFunctions)
{
    didAddFunctions();
}

Pipeline::~Pipeline() = default;

OptimizationInputs Pipeline::createInputs(OptionSet<ProgramResult> needed, EffectSummariesFor summariesFor)
{
    OptimizationInputs inputs = m_analyses.inputs(flow(), needed, summariesFor);
    if (needed.contains(ProgramResult::CallingConventions))
        inputs.callingConventions = m_output->callingConventions.get();
    if (needed.contains(ProgramResult::SlotPlan))
        inputs.slots = m_output->slots.get();
    if (needed.contains(ProgramResult::Placement))
        inputs.placement = m_output->placement.get();
    return inputs;
}

static SlotPlanInputs createSlotPlanInputs(Pipeline& pipeline)
{
    const FlowResults& flow = pipeline.flow();
    return {
        .flow = flow,
        .callGraph = pipeline.analyses().callGraph(flow),
        .heat = pipeline.analyses().heat(flow),
        .frameAllocation = *pipeline.output().frameAllocation,
        .environmentReuse = *pipeline.output().environmentReuse,
    };
}

PlacementInputs Pipeline::createPlacementInputs(const CallGraph& callGraph)
{
    return {
        .flow = flow(),
        .callGraph = callGraph,
        .heat = m_analyses.heat(flow()),
        .callingConventions = *m_output->callingConventions,
        .frameAllocation = *m_output->frameAllocation,
        .environmentReuse = *m_output->environmentReuse,
        .slots = *m_output->slots,
        .regExpRequests = *m_output->regExpRequests,
    };
}

static RegExpRequests collectRegExpRequestsOf(Program& program, const FlowResults& flow, BuildContext& build)
{
    StringArena strings(program.strings);
    return collectRegExpRequests(program, flow, strings, build);
}

void Pipeline::runSchedule(FunctionSchedule schedule, const StepsAroundSchedule& steps)
{
    if (schedule == FunctionSchedule::Prepare)
        m_analyses.summarizeNoBodiesUntilTheyChange();
    OptimizationInputs inputs = inputsFor(schedule);
    if (m_runner.isStopped())
        return;
    const VerifierInputs verifierInputs = m_runner.verifierInputs();
    const PassDescriptor& summarizer = descriptorOf(PassID::ComputeEffectSummaries);
    bool canSummarize = verifierInputs.flow && summarizer.accepts(m_program.form()) && m_build.isEnabled(summarizer.id, FunctionID());
    EffectSummaries effectsForVerifier;
    if (m_build.options().verifiesAfterEveryPass && !readsEffectSummaries(schedule) && canSummarize) {
        effectsForVerifier = computeEffectSummaries(m_program, *verifierInputs.flow, m_build);
        VerifierInputs withEffects = verifierInputs;
        withEffects.effects = &effectsForVerifier;
        m_runner.setVerifierInputs(withEffects);
    }
    runOnFunctions(schedule, m_program, inputs, m_runner, m_analyses, steps);
    m_runner.setVerifierInputs(verifierInputs);
    m_runner.didFinishFunctionLoop();
}

void Pipeline::advanceTo(IRForm form, bool didRunProducer)
{
    if (!didRunProducer || m_build.diagnostics().hasErrors())
        return;
    m_program.setForm(form);
}

void Pipeline::didAddFunctions()
{
    m_output->derivedFunctions->recordFunctionsOf(m_program);
}

PassOutcome Pipeline::verify(IRForm form)
{
    VerifierInputs inputs = m_runner.verifierInputs();
    if (inputs.flow)
        inputs.effects = &m_analyses.effects(*inputs.flow);
    bool isRunning = !m_runner.isStopped();
    bool isValid = read(PassID::VerifyProgram, verifyProgram, m_program, form, m_build, inputs);
    if (isRunning && !isValid && form != IRForm::AsRead && !m_build.options().verifiesAfterEveryStage)
        m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::VerificationFailedAtTheEnd });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::outlineColdOperations(OutliningSiteFinder* finder, const ReadersOfOutlinedFunctions* readers)
{
    edit(PassID::OutlineColdOperations, Typed::outlineColdOperations, inputsFor(PassID::OutlineColdOperations), m_build, finder, readers);
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::inlineHeads(const ReaderOfScheduledFunction* readEditedFunction)
{
    auto findDirectCallees = [&](FunctionID id) -> const Vector<FunctionID>* {
        const ReferencesOfBody* references = m_factsOfSelectedBodies ? m_factsOfSelectedBodies->referencesOf(id) : nullptr;
        return references ? &references->directCallees : nullptr;
    };
    DirectCalleesOfFunction directCalleesOf(findDirectCallees);
    edit(PassID::InlineHeads, Typed::inlineHeads, inputsFor(PassID::InlineHeads), m_build, readEditedFunction, &directCalleesOf);
    return m_runner.lastProgramPassOutcome();
}

void Pipeline::countGenericVersions()
{
    m_build.statistics().count(StatisticID::GenericVersionsBehindEntryChecksCompiled, std::ranges::count_if(m_program.functions, [](const Function& function) {
        return function.hasBody() && function.role == FunctionRole::GenericVersion;
    }));
}

void Pipeline::didPlace()
{
    m_factsOfSelectedBodies = nullptr;
    m_placedTableSizes = m_output->placement->tableSizes();
    m_runner.setVerifierInputs({ .flow = &flow(), .placed = &m_placedTableSizes });
}

PassOutcome Pipeline::numberAndEncode(NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites)
{
    countGenericVersions();
    PlacementInputs inputs = createPlacementInputs(m_analyses.callGraph(flow()));
    *m_output->placement = edit(PassID::NumberAndEncode, [&](ProgramEditor& editor) {
        return Typed::numberAndEncode(editor, inputs, m_build, readBodiesBesideCollectingSites, m_factsOfSelectedBodies ? &m_factsOfSelectedBodies->scansForPlacement() : nullptr);
    });
    didPlace();
    return m_runner.lastProgramPassOutcome();
}

const CallGraph& Pipeline::prepareToReadSelectedBodiesBesideNumbering()
{
    inputsFor(PassID::CollectRegExpLiterals);
    inputsFor(PassID::ElideStackChecks);
    const CallGraph& callGraph = m_analyses.callGraph(flow());
    m_runner.didCompute(descriptorOf(PassID::CollectRegExpLiterals).outputs);
    return callGraph;
}

void Pipeline::collectRegExpsAndElideStackChecks(const CallGraph& callGraph)
{
    *m_output->regExpRequests = m_runner.runAnalysisBeside(PassID::CollectRegExpLiterals, m_program, [&] {
        return collectRegExpRequestsOf(m_program, flow(), m_build);
    }, [&] {
        *m_output->stackChecks = m_runner.runAnalysis(PassID::ElideStackChecks, m_program, [&] {
            return elideStackChecks(m_program, callGraph, m_build);
        });
    });
    if (!m_runner.isStopped())
        m_output->startCompilingRegExps(m_program, m_build);
}

#define AOT_FUNCTION_PASS_CASE(id, function) case PassID::id:

PassOutcome Pipeline::runProgramPass(PassID id)
{
    PipelineOutput& output = *m_output;
    switch (id) {
    case PassID::AllocateTaggedLayouts:
        editProgram(id, [&] {
            allocateTaggedLayouts(m_program, m_build);
            giveLayoutsToCreatedObjects(m_program, m_build);
        });
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::CheckTypesAtEntry:
        edit(id, checkTypesAtEntry, m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::SplitBehindEntryChecks:
        // The pass drops the heat that it asks for, and leaves most bodies as they are.
        m_analyses.keepPartsOfBodiesForNextHeat();
        edit(id, splitBehindEntryChecks, &inputsFor(id).heat, m_build);
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::SkipAnsweredEntryChecks:
        edit(id, skipAnsweredEntryChecks, inputsFor(id).heat, m_build);
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::NarrowFieldTypes:
        editProgram(id, [&] {
            m_int32ParameterCandidates = narrowFieldTypes(m_program, m_build);
        }, BodiesAreChanged::ThroughEditors);
        return m_runner.lastProgramPassOutcome();
    case PassID::NameCreatedSymbols:
        editProgram(id, [&] {
            nameCreatedSymbols(m_program, m_build);
        }, BodiesAreChanged::ThroughEditors);
        return m_runner.lastProgramPassOutcome();
    case PassID::VerifyProgram:
        return verify(m_program.form());
    case PassID::GuardTaintedPositions:
        editProgram(id, [&] {
            guardTaintedPositions(m_program, m_build, TaintedPositionsRun::BeforeFlow);
        });
        return m_runner.lastProgramPassOutcome();
    case PassID::AnalyzeFlow:
        *output.flow = read(id, [&] {
            return analyzeValueFlow(m_program, m_program.types, m_build);
        });
        m_runner.setVerifierInputs({ .flow = &flow() });
        return m_runner.lastProgramPassOutcome();
    case PassID::ApplyFlowResults:
        editProgram(id, [&] {
            applyFlowResults(m_program, *output.flow, m_build);
            m_build.destroyLater(FlowApplyAccess::takeApplication(*output.flow));
            if (!output.flow->hasResults())
                return;
            guardTaintedPositionsAfterFlow(m_program, *output.flow, m_build);
            expectInt32ParametersWithRemainingChecks(m_program, std::exchange(m_int32ParameterCandidates, { }).span(), m_build);
        });
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::CheckLockedBuiltinWrites:
        read(id, checkLockedBuiltinWrites, m_program, flow(), m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::BuildCallGraph:
        m_analyses.callGraph(flow());
        return PassOutcome::Unchanged;
    case PassID::ComputeEffectSummaries:
        m_analyses.effects(flow());
        return PassOutcome::Unchanged;
    case PassID::EstimateHeat:
        m_analyses.heat(flow());
        return PassOutcome::Unchanged;
    case PassID::SummarizeForInlining:
        m_analyses.inlining(flow());
        return PassOutcome::Unchanged;
    case PassID::RemoveUnreadExports:
        edit(id, removeUnreadExports, flow(), m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::PromoteEnvironmentsToVariables:
        edit(id, promoteEnvironmentsToVariables, *output.flow, m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::DeferDefinitions:
        edit(id, deferDefinitions, flow(), m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::NarrowConstantVariables:
        edit(id, [&](ProgramEditor& editor) {
            output.flow->setConstantLists(narrowConstantVariables(editor, flow(), m_build));
        });
        return m_runner.lastProgramPassOutcome();
    case PassID::DeferInitializers:
        edit(id, deferInitializers, inputsFor(id), m_runner);
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::MarkResultsUsedAsKeys:
        edit(id, markResultsUsedAsKeys, m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::DeferClosuresInFields:
        edit(id, deferClosuresInFields, m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::ReadCapturesFromEnvironments:
        edit(id, readCapturesFromEnvironments, flow(), m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::RemoveUnreadFields:
        edit(id, removeUnreadFields, inputsFor(id), m_runner);
        return m_runner.lastProgramPassOutcome();
    case PassID::RemoveUnusedResults:
        edit(id, removeUnusedResults, inputsFor(id), m_runner);
        return m_runner.lastProgramPassOutcome();
    case PassID::RemoveUnusedParameters:
        edit(id, removeUnusedParameters, inputsFor(id), m_runner);
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::RemoveUnreachableFunctions:
        edit(id, [&](ProgramEditor& editor) {
            // Select gives calls by value other forms, and the body that throws is written in the form in front of it.
            if (m_program.form() == IRForm::Prepared)
                closeEntrypointsThatNothingEnters(editor, flow(), output.calleesOfSites, m_build);
            removeUnreachableFunctions(editor, m_analyses.callGraph(flow()), m_build);
        });
        output.derivedFunctions->forgetFunctionsWithoutBody(m_program);
        return m_runner.lastProgramPassOutcome();
    case PassID::SplitFreshResults:
        edit(id, [&](ProgramEditor& editor) {
            OptimizationInputs inputs = inputsFor(id);
            inputs.callGraph.computeRanks(m_build);
            splitFreshResults(editor, inputs, m_runner);
        });
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::PromoteAggregateParameters:
        edit(id, promoteAggregateParameters, inputsFor(id), m_build);
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::SplitCoroutines:
        edit(id, splitCoroutines, inputsFor(id), m_runner);
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::CallModules:
        edit(id, callModules, flow(), m_build);
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::CreateEntrypoints:
        edit(id, createEntrypoints, inputsFor(id), m_build);
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::ChooseCallingConventions:
        *output.callingConventions = edit(id, chooseCallingConventions, inputsFor(id), m_runner);
        return m_runner.lastProgramPassOutcome();
    case PassID::AllocateInFrames:
        if (!m_runner.isStopped()) {
            inputsFor(id);
            output.flow->setBorrowedParameters(m_analyses.borrowedParameters());
            output.flow->setFrameArrayParameters(computeFrameArrayParameters(m_program, m_build));
        }
        *output.frameAllocation = edit(id, allocateInFrames, inputsFor(id), *output.flow, m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::AllocateInnerObjects: {
        OptimizationInputs inputs = inputsFor(id);
        edit(id, allocateInnerObjects, flow(), inputs.callGraph, inputs.heat, m_build);
        return m_runner.lastProgramPassOutcome();
    }
    case PassID::ReuseEnvironments:
        *output.environmentReuse = edit(id, [&](ProgramEditor& editor) {
            return reuseEnvironments(editor, flow(), *output.frameAllocation, m_build, [&] {
                return m_analyses.borrowedParameters();
            });
        });
        return m_runner.lastProgramPassOutcome();
    case PassID::NumberLiteralStrings:
        internPiecesOfModulePaths(m_program);
        *output.literalStrings = read(id, numberLiteralStrings, m_program, flow(), m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::EmbedConstantCollections:
        *output.constantCollections = edit(id, [&](ProgramEditor& editor) {
            // The table of borrowed parameters looks at the bodies that have changed. What it reads of them is kept for the effect summaries, which are brought up to date a few passes on.
            const EffectSummaries* summariesFromFlow = m_analyses.keptSummariesFromFlow();
            std::optional<EffectSummaries> ownSummaries;
            if (!summariesFromFlow)
                summariesFromFlow = &ownSummaries.emplace(createSummariesFromFlow(m_program, flow(), m_analyses.borrowedParameters(), m_build));
            return embedConstantCollections(editor, flow(), summariesFromFlow, m_build);
        });
        return m_runner.lastProgramPassOutcome();
    case PassID::CollectRegExpLiterals:
        *output.regExpRequests = read(id, collectRegExpRequestsOf, m_program, flow(), m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::OutlineColdOperations:
        return outlineColdOperations();
    case PassID::InlineHeads:
        return inlineHeads();
    case PassID::ElideStackChecks:
        *output.stackChecks = read(id, elideStackChecks, m_program, inputsFor(id).callGraph, m_build);
        return m_runner.lastProgramPassOutcome();
    case PassID::PlanSlots: {
        SlotPlanInputs inputs = createSlotPlanInputs(*this);
        *output.slots = read(id, planSlots, m_program, inputs, m_build);
        return m_runner.lastProgramPassOutcome();
    }
    case PassID::NumberAndEncode:
        return numberAndEncode([] { });
    case PassID::PlaceSiteCaches:
        *output.siteCaches = edit(id, [&](ProgramEditor& editor) {
            return placeSiteCaches(editor, *output.placement, flow(), m_analyses.heat(flow()), m_build);
        });
        return m_runner.lastProgramPassOutcome();
    case PassID::CheckPlacement: {
        PlacementInputs inputs = createPlacementInputs(m_analyses.callGraph(flow()));
        read(id, checkPlacement, m_program, inputs, *output.placement, *output.siteCaches, m_build);
        return m_runner.lastProgramPassOutcome();
    }
    case PassID::None:
    case PassID::GenerateCode:
    FOR_EACH_AOT_FUNCTION_PASS(AOT_FUNCTION_PASS_CASE)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#undef AOT_FUNCTION_PASS_CASE

static void forgetLongExpectations(Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    LongExpectations expectations(program, loops);
    if (!expectations.hasAny())
        return;

    if (!program.readerPreparedFunctionsForTyping()) {
        forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index) {
            expectations.forgetInBodyOf(program.functions[index]);
            expectations.forgetInRecordOf(program.functions[index]);
        });
    }
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        for (Field& field : program.layouts[index].fields)
            expectations.forget(field.declared);
    });
    for (Variable& variable : program.variables)
        expectations.forget(variable.declared);
}

void Pipeline::runReadStage()
{
    TimedStage stage(*this, Stage::ReadAndVerify);
    forgetLongExpectations(m_program, m_build);
    stage.run(PassID::AllocateTaggedLayouts);
    stage.run(PassID::CheckTypesAtEntry);
    markEnforcedFields(m_program, m_build);
    stage.run(FunctionSchedule::Type);
    stage.run(PassID::NarrowFieldTypes);
    stage.run(PassID::NameCreatedSymbols);
    stage.verifyAtBoundary(IRForm::AsRead);
}

void Pipeline::runFlowStage()
{
    TimedStage stage(*this, Stage::AnalyzeFlow);
    stage.run(PassID::GuardTaintedPositions);
    stage.run(PassID::AnalyzeFlow);
    bool wasRunning = !m_runner.isStopped();
    stage.run(PassID::ApplyFlowResults);
    advanceTo(IRForm::Prepared, wasRunning);
    m_output->remainingChecks = collectRemainingChecks(m_program, m_build);
    if (!m_runner.isStopped())
        m_output->calleesOfSites = collectCalleesOfSites(m_program, m_build);
    stage.didFinish("collect remaining checks and callees of sites"_s);
    stage.run(PassID::CheckLockedBuiltinWrites);
}

void Pipeline::runOptimizeStage()
{
    TimedStage stage(*this, Stage::Optimize);
    if (m_build.validates() && !m_runner.isStopped())
        checkDeclaredEffects(m_build.diagnostics());
    // What places, slots and the names of type tests are weighed with is read from the Optimize schedule on, and CallModules drops the heat in front of that.
    if (m_runner.mayRunAnalysesTogether())
        m_analyses.estimateHeatFor(HeatFor::Preparation);
    stage.run(PassID::PromoteEnvironmentsToVariables);
    stage.run(PassID::RemoveUnreadExports);
    stage.run(PassID::DeferDefinitions);
    stage.run(PassID::SplitBehindEntryChecks);
    if (!m_runner.isStopped()) {
        m_output->flow->setSitesOfWholeLiterals(collectSitesOfWholeLiterals(m_program, flow(), m_build));
        m_output->flow->setConstantLists(collectConstantLists(m_program, flow(), m_build));
    }
    stage.didFinish("collect whole literals and constant lists"_s);
    stage.run(FunctionSchedule::Prepare);
    stage.run(PassID::NarrowConstantVariables);
    stage.run(PassID::DeferInitializers);
    stage.run(PassID::PromoteAggregateParameters);
    stage.run(PassID::SplitFreshResults);
    stage.run(PassID::SplitCoroutines);
    stage.run(PassID::CallModules);
    m_analyses.estimateHeatFor(HeatFor::Everything);
    stage.run(PassID::CreateEntrypoints);
    stage.run(FunctionSchedule::Resolve);
    Vector<InternalError> functionsThatEndInFirstBlock;
    if (m_build.options().checksThatFunctionsGetPastEntry && !m_runner.isStopped())
        functionsThatEndInFirstBlock = findFunctionsThatEndInFirstBlock(m_program, findReachableFunctions(m_program, m_analyses.callGraph(flow()), m_build), m_build);
    stage.run(FunctionSchedule::Optimize);
    stage.run(PassID::ReadCapturesFromEnvironments);
    stage.run(PassID::RemoveUnreadFields);
    stage.run(PassID::RemoveUnusedResults);
    stage.run(PassID::RemoveUnusedParameters);
    stage.run(PassID::SplitFreshResults);
    stage.run(PassID::SkipAnsweredEntryChecks);
    stage.run(PassID::RemoveUnusedResults);
    stage.run(PassID::MarkResultsUsedAsKeys);
    stage.run(PassID::EmbedConstantCollections);
    stage.run(PassID::RemoveUnreachableFunctions);
    if (!m_runner.isStopped())
        reportFunctionsThatEndInFirstBlock(m_program, WTF::move(functionsThatEndInFirstBlock), m_build);
    stage.run(PassID::ChooseCallingConventions);
    stage.run(PassID::DeferClosuresInFields);
    stage.run(PassID::AllocateInFrames);
    stage.run(PassID::AllocateInnerObjects);
    stage.run(PassID::ReuseEnvironments);
    stage.verifyAtEndOfPreparedForm();
}

void Pipeline::runSlotStage()
{
    TimedStage stage(*this, Stage::PlanSlots);
    if (!m_runner.mayRunAnalysesTogether()) {
        stage.run(PassID::NumberLiteralStrings);
        stage.run(PassID::PlanSlots);
        return;
    }
    internPiecesOfModulePaths(m_program);
    SlotPlanInputs inputs = createSlotPlanInputs(*this);
    inputsFor(PassID::PlanSlots);
    inputsFor(PassID::NumberLiteralStrings);
    {
        ProgramCachesWithScratchTypes caches(m_program, m_build);
        *m_output->slots = m_runner.runAnalysisBeside(PassID::PlanSlots, m_program, [&] {
            return planSlots(m_program, inputs, m_build);
        }, [&] {
            *m_output->literalStrings = m_runner.runAnalysisBeside(PassID::NumberLiteralStrings, m_program, [&] {
                return numberLiteralStrings(m_program, flow(), m_build);
            }, [&] {
                m_analyses.effects(flow());
            });
        });
    }
    stage.didFinish(nameOf(PassID::PlanSlots));
}

const Heat* Pipeline::heatForReport()
{
    return hasFlag(m_build.options().reports, ReportSet::Flow) && !m_runner.isStopped() ? &m_analyses.heat(flow()) : nullptr;
}

void Pipeline::runSelectStage()
{
    TimedStage stage(*this, Stage::Select);
    bool wasRunning = !m_runner.isStopped();
    if (wasRunning && m_runner.mayRunAnalysesTogether() && !m_build.options().verifiesAfterEveryStage) {
        // Whoever only reads a function reads it where it is in the cache: behind its selection, or behind the last step that changes it.
        bool findsOutliningSites = m_build.isEnabled(PassID::OutlineColdOperations, FunctionID());
        OutliningSiteFinder finder(m_program, inputsFor(PassID::OutlineColdOperations), m_build);
        AccessSiteObserver observer(m_program, flow(), m_build, heatForReport());
        m_factsOfSelectedBodies = makeUnique<FactsOfSelectedBodies>(*this);
        FactsOfSelectedBodies& facts = *m_factsOfSelectedBodies;
        std::span<const uint8_t> hasOutliningSites = finder.hasSites();
        auto observe = [&](FunctionID id) {
            observer.observe(id);
        };
        auto findOutliningSites = [&](FunctionID id) {
            if (findsOutliningSites)
                finder.findIn(id);
        };
        auto readReferences = [&](FunctionID id) {
            if (!hasOutliningSites[id.index()])
                facts.readReferences(id);
        };
        auto scanForPlacement = [&](FunctionID id) {
            if (!hasOutliningSites[id.index()])
                facts.scansForPlacement().scan(id);
        };
        ReaderOfScheduledFunction readers1[] = { observe, findOutliningSites, readReferences, scanForPlacement };
        const ReaderOfScheduledFunction* readersOfSelectedFunction[] = { &readers1[0], &readers1[1], &readers1[2], &readers1[3] };
        stage.run(FunctionSchedule::Select, { .behind = readersOfSelectedFunction });
        advanceTo(IRForm::Selected, wasRunning);
        if (!m_runner.isStopped())
            m_output->accessSiteObservations = observer.finish();
        stage.didFinish("observe access sites"_s);

        auto noteAddedFunctions = [&] {
            facts.didAddFunctions();
        };
        auto readFinishedFunction = [&](FunctionID id) {
            facts.read(id);
        };
        ScopedLambda<void()> didAddFunctions(noteAddedFunctions);
        ReaderOfScheduledFunction readerOfFinishedFunction(readFinishedFunction);
        ReadersOfOutlinedFunctions readers { didAddFunctions, readerOfFinishedFunction };
        outlineColdOperations(&finder, &readers);
        stage.didFinish(nameOf(PassID::OutlineColdOperations));
        inlineHeads(&readerOfFinishedFunction);
        stage.didFinish(nameOf(PassID::InlineHeads));
        if (m_runner.isStopped())
            m_factsOfSelectedBodies = nullptr;
        else
            facts.giveReferencesTo(m_analyses);
        stage.didFinish("give references to the call graph"_s);
    } else {
        stage.run(FunctionSchedule::Select);
        stage.verifyOnRequest(IRForm::Selected);
        advanceTo(IRForm::Selected, wasRunning);
        if (!m_runner.isStopped())
            m_output->accessSiteObservations = observeAccessSites(m_program, flow(), m_build, heatForReport());
        stage.didFinish("observe access sites"_s);
        stage.run(PassID::OutlineColdOperations);
        stage.run(PassID::InlineHeads);
    }
    stage.run(PassID::RemoveUnreachableFunctions);
    if (m_runner.mayRunAnalysesTogether())
        return;
    stage.run(PassID::ElideStackChecks);
    stage.run(PassID::CollectRegExpLiterals);
}

void Pipeline::runPlacementStage()
{
    TimedStage stage(*this, Stage::NumberAndEncode);
    bool wasRunning = !m_runner.isStopped();
    if (m_runner.mayRunAnalysesTogether()) {
        const CallGraph& callGraph = prepareToReadSelectedBodiesBesideNumbering();
        numberAndEncode([&] {
            collectRegExpsAndElideStackChecks(callGraph);
        });
        stage.didFinish(nameOf(PassID::NumberAndEncode));
    } else
        stage.run(PassID::NumberAndEncode);
    stage.verifyOnRequest(IRForm::Placed);
    advanceTo(IRForm::Placed, wasRunning);
    stage.run(PassID::PlaceSiteCaches);
    if (stage.verifiesProgram())
        stage.run(PassID::CheckPlacement);
}

void Pipeline::runRepresentationStage()
{
    TimedStage stage(*this, Stage::ChooseRepresentations);
    stage.run(FunctionSchedule::Represent);
    stage.verifyAtBoundary(IRForm::Placed);
}

// Of a placement, the choice of representations asks for slots, for the field access of an operation and for the statics.
void Pipeline::runPlacementAndRepresentationStagesTogether()
{
    TimedStage stage(*this, Stage::ChooseRepresentations);
    bool wasRunning = !m_runner.isStopped();
    const CallGraph& callGraph = prepareToReadSelectedBodiesBesideNumbering();
    countGenericVersions();

    PlacementInputs inputs = createPlacementInputs(callGraph);
    PlacementInSteps steps(inputs, m_build, m_factsOfSelectedBodies ? &m_factsOfSelectedBodies->scansForPlacement() : nullptr);
    Placement& placement = *m_output->placement;
    placement = edit(PassID::NumberAndEncode, [&](ProgramEditor& editor) {
        return steps.numberWhatRepresentationsAsk(editor, [&] {
            collectRegExpsAndElideStackChecks(callGraph);
        });
    });
    stage.didFinish(nameOf(PassID::NumberAndEncode));
    advanceTo(IRForm::Placed, wasRunning);
    if (m_runner.isStopped()) {
        m_factsOfSelectedBodies = nullptr;
        return;
    }

    const Heat& heat = m_analyses.heat(flow());
    steps.prepareToResolveFieldAccesses();
    Vector<Vector<SiteCacheRequest>> siteCacheRequests(m_program.functions.size());
    bool needsWeightsOfSiteCaches = !m_factsOfSelectedBodies || m_factsOfSelectedBodies->scansForPlacement().numberOfOperationsWithWordForSiteCache() > m_build.limit(LimitID::MaxSiteCaches);
    auto readPlacedFunction = [&](FunctionID id, Function& function) {
        steps.resolveFieldAccessesOf(placement, id, function);
        collectSiteCacheRequests(m_program, placement, flow(), heat, m_build, id, needsWeightsOfSiteCaches, siteCacheRequests[id.index()]);
    };
    auto numberRest = [&] {
        steps.numberRest(placement);
    };
    m_output->calledFunctions.grow(m_program.functions.size());
    auto listCalledFunctions = [&](FunctionID id) {
        Vector<FunctionID>& called = m_output->calledFunctions[id.index()];
        called.shrink(0);
        forEachCalledFunction(m_program.functions[id.index()], [&](FunctionID target) {
            called.append(target);
        });
    };
    ScopedLambda<void(FunctionID, Function&)> inFront(readPlacedFunction);
    ScopedLambda<void()> beside(numberRest);
    ReaderOfScheduledFunction readerOfRepresentedFunction(listCalledFunctions);
    const ReaderOfScheduledFunction* readersOfRepresentedFunction[] = { &readerOfRepresentedFunction };
    stage.run(FunctionSchedule::Represent, { .inFront = &inFront, .behind = readersOfRepresentedFunction, .beside = &beside });

    *m_output->siteCaches = edit(PassID::PlaceSiteCaches, [&](ProgramEditor& editor) {
        steps.resolveSites(editor, placement);
        return placeSiteCaches(editor, placement, flow(), heat, m_build, siteCacheRequests.span());
    });
    m_build.destroyLater(WTF::move(siteCacheRequests));
    didPlace();
    stage.didFinish(nameOf(PassID::PlaceSiteCaches));
}

void Pipeline::runAllStages(NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries)
{
    runReadStage();
    if (m_build.options().isVerifyOnly())
        return;
    runFlowStage();
    // Nothing behind this refuses a program for what it says. The limits of what follows are about the executable.
    if (m_build.options().isCheckOnly())
        return;
    runOptimizeStage();
    runSlotStage();
    runSelectStage();
    // Whoever stops, prints or verifies between the two stages sees bodies that have all their words.
    const auto& options = m_build.options();
    if (m_runner.mayRunAnalysesTogether() && !options.verifiesProgram && !options.isVerifyOnly())
        runPlacementAndRepresentationStagesTogether();
    else {
        runPlacementStage();
        runRepresentationStage();
    }
    {
        // A run that was stopped after a pass hands out what its passes used. What it never computed stays the default result.
        PhaseTimes times(m_build, Stage::GenerateCode);
        if (m_runner.isStopped() || m_build.diagnostics().hasErrors() || !m_runner.mayRunAnalysesTogether())
            m_output->effects = m_analyses.takeEffects(flow());
        else {
            ProgramCachesWithScratchTypes caches(m_program, m_build);
            runTasksInParallel(m_build.parallelLoops(), {
                [&] { m_output->effects = m_analyses.takeEffects(flow()); },
                [&] { readBesideLastEffectSummaries(*m_output); },
            });
        }
        m_output->heat = m_analyses.takeHeat(flow());
        if (m_output->accessSiteObservations.isEmpty())
            m_output->accessSiteObservations = observeAccessSites(m_program, flow(), m_build);
        if (m_build.validates() && !m_runner.isStopped())
            checkEffectSummaries(m_program, flow(), *m_output->effects, m_build);
        times.didFinish(nameOf(PassID::ComputeEffectSummaries));
    }
    m_program.types.addTo(m_build.statistics());
    m_output->isComplete = !m_runner.isStopped();
}

std::unique_ptr<PipelineOutput> runPipeline(std::span<const uint8_t> programBytes, const ExternalFunctionBodies* externalBodies, BuildContext& build, NOESCAPE const ScopedLambda<void()>& didReadProgram, NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries)
{
    std::unique_ptr<Program> program;
    {
        PhaseTimes times(build, Stage::ReadAndVerify);
        program = readProgramWithExternalBodies(programBytes, externalBodies, build);
        times.didFinish("Read"_s);
    }
    didReadProgram();
    if (!program) {
        build.diagnostics().finish(nullptr);
        return nullptr;
    }
    return runPipeline(WTF::move(program), build, readBesideLastEffectSummaries);
}

std::unique_ptr<PipelineOutput> runPipeline(std::unique_ptr<Program> program, BuildContext& build, NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries, std::optional<PassID> last)
{
    build.resolveFunctionToDump(*program);
    PassRunner runner(build, last);
    std::unique_ptr<PipelineOutput> output;
    {
        Pipeline pipeline(*program, runner);
        pipeline.output().program = WTF::move(program);
        pipeline.runAllStages(readBesideLastEffectSummaries);
        output = pipeline.takeOutput();
    }
    if (build.diagnostics().hasErrors()) {
        output->waitForRegExps();
        build.diagnostics().finish(output->program.get());
        return nullptr;
    }
    return output;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
