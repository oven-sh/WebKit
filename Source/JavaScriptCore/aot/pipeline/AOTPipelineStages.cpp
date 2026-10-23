/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPipelineInternal.h"

#if ENABLE(AOT)

#include "AOTBackgroundTask.h"
#include "AOTBuildContext.h"
#include "AOTChooseCallingConventions.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsCheck.h"
#include "AOTElideStackChecks.h"
#include "AOTEmbedConstantCollections.h"
#include "AOTExternalFunctionBodies.h"
#include "AOTFactsOfSelectedBodies.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionLoop.h"
#include "AOTHeat.h"
#include "AOTIR.h"
#include "AOTLongExpectations.h"
#include "AOTNarrowConstantVariables.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTOutlineColdOperations.h"
#include "AOTOutliningCandidates.h"
#include "AOTPhaseTimes.h"
#include "AOTPlacement.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpRequests.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTReuseEnvironments.h"
#include "AOTSiteCachePlacement.h"
#include "AOTSlotPlan.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include "AOTWholeLiterals.h"
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class TimedStage {
    WTF_MAKE_NONCOPYABLE(TimedStage);
public:
    TimedStage(Pipeline& pipeline, Stage stage)
        : m_pipeline(pipeline)
        , m_times(pipeline.runner().build(), stage)
    {
    }

    void run(PassID id)
    {
        m_pipeline.runProgramPass(id);
        m_times.didFinish(nameOf(id));
    }

    void run(FunctionSchedule schedule)
    {
        m_pipeline.runSchedule(schedule);
        m_times.didFinish(descriptorOf(schedule).name);
    }

    // What a stage does between two passes. Without a line of its own it counts as time of the pass behind it.
    void didFinish(ASCIILiteral name) { m_times.didFinish(name); }

    void verify(IRForm form)
    {
        m_pipeline.verify(form);
        m_times.didFinish(nameOf(PassID::VerifyProgram));
    }

    void verifyOnRequest(IRForm form)
    {
        if (m_pipeline.runner().build().options().verifiesAfterEveryStage)
            verify(form);
    }

    // The program that is read, where it is placed, and the program that goes to the back end. The front end and the passes are ours, so a build of a user's does not pay for it: every test does.
    bool verifiesProgram() const
    {
        const auto& options = m_pipeline.runner().build().options();
        return options.verifiesProgram || options.isVerifyOnly();
    }

    void verifyAtBoundary(IRForm form)
    {
        if (verifiesProgram())
            verify(form);
    }

    void verifyAtEndOfPreparedForm()
    {
        if (m_pipeline.runner().build().options().verifiesAfterEveryStage) {
            verify(IRForm::Prepared);
            return;
        }
        if (!verifiesProgram())
            return;
        m_pipeline.verifyUnspecifiedValues();
        m_times.didFinish(nameOf(PassID::VerifyProgram));
    }

private:
    Pipeline& m_pipeline;
    PhaseTimes m_times;
};

} // anonymous namespace

template<typename Result>
static void createDefaultResult(std::unique_ptr<Result>& result)
{
    result = makeUniqueWithoutFastMallocCheck<Result>();
}

static std::unique_ptr<PipelineOutput> createOutput(BuildContext& build)
{
    auto output = makeUnique<PipelineOutput>();
    output->derivedFunctions = makeUnique<DerivedFunctions>(build);
    createDefaultResult(output->flow);
    createDefaultResult(output->effects);
    createDefaultResult(output->heat);
    createDefaultResult(output->callingConventions);
    createDefaultResult(output->frameAllocation);
    createDefaultResult(output->environmentReuse);
    createDefaultResult(output->literalStrings);
    createDefaultResult(output->constantCollections);
    createDefaultResult(output->slots);
    createDefaultResult(output->regExpRequests);
    createDefaultResult(output->stackChecks);
    createDefaultResult(output->placement);
    createDefaultResult(output->siteCaches);
    return output;
}

static void checkEffectSummaries(const Program& program, const FlowResults& flow, const EffectSummaries& effects, BuildContext& build)
{
    EffectContext context { program, effects };
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;
        FunctionID id(index);
        auto analyses = createAnalysesForSummary(build, context, flow, id, function);
        checkSummaryCoversBody(context, id, *analyses, build.diagnostics());
    });
}

static std::unique_ptr<Program> readProgram(std::span<const uint8_t> programBytes, const ExternalFunctionBodies* externalBodies, BuildContext& build)
{
    PhaseTimes times(build, Stage::ReadAndVerify);
    auto program = readProgramWithExternalBodies(programBytes, externalBodies, build);
    times.didFinish("Read"_s);
    return program;
}

PipelineOutput::PipelineOutput() = default;

PipelineOutput::~PipelineOutput()
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
    build.destroyLater(WTF::move(derivedFunctions));
    build.destroyLater(WTF::move(effects));
    build.destroyLater(WTF::move(callingConventions));
    build.destroyLater(WTF::move(frameAllocation));
    build.destroyLater(WTF::move(environmentReuse));
    build.destroyLater(WTF::move(literalStrings));
    build.destroyLater(WTF::move(constantCollections));
    build.destroyLater(WTF::move(slots));
    build.destroyLater(WTF::move(stackChecks));
    build.destroyLater(WTF::move(siteCaches));
    build.destroyLater(WTF::move(calleesOfSites));
}

Pipeline::Pipeline(Program& program, PassRunner& runner)
    : m_output(createOutput(runner.build()))
    , m_program(program)
    , m_runner(runner)
    , m_build(runner.build())
    , m_analyses(program, runner, *m_output->derivedFunctions)
{
    m_output->derivedFunctions->recordFunctionsOf(program);
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

void Pipeline::runSchedule(FunctionSchedule schedule, const StepsAroundSchedule& steps)
{
    if (schedule == FunctionSchedule::Prepare)
        m_analyses.summarizeNoBodiesUntilTheyChange();
    OptimizationInputs inputs = inputsFor(schedule);
    if (m_runner.isStopped())
        return;
    const VerifierInputs verifierInputs = m_runner.verifierInputs();
    const PassDescriptor& summarizer = descriptorOf(PassID::ComputeEffectSummaries);
    bool canSummarize = verifierInputs.flow && summarizer.firstForm <= m_program.form() && m_program.form() <= summarizer.lastForm && m_build.isEnabled(summarizer.id, FunctionID());
    EffectSummaries effectsForVerifier;
    if (m_build.options().verifiesAfterEveryPass && !readsEffectSummaries(schedule) && canSummarize) {
        effectsForVerifier = Typed::computeEffectSummaries(m_program, *verifierInputs.flow, m_build);
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
    m_runner.didReachForm(m_program);
}

void Pipeline::didAddFunctions()
{
    m_output->derivedFunctions->recordFunctionsOf(m_program);
}

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
    m_runner.didReachForm(m_program);
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
    planSlotsBesideOtherReadersOfPreparedBodies();
    stage.didFinish(nameOf(PassID::PlanSlots));
}

void Pipeline::runSelectStage()
{
    TimedStage stage(*this, Stage::Select);
    bool wasRunning = !m_runner.isStopped();
    if (wasRunning && m_runner.mayRunAnalysesTogether() && !m_build.options().verifiesAfterEveryStage) {
        // Whoever only reads a function reads it where it is in the cache: behind its selection, or behind the last step that changes it.
        bool findsOutliningSites = m_build.isEnabled(PassID::OutlineColdOperations, FunctionID());
        OutliningSiteFinder finder(m_program, inputsFor(PassID::OutlineColdOperations), m_build);
        AccessSiteObserver observer(m_program, flow(), m_build);
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
        runSchedule(FunctionSchedule::Select, { .behind = readersOfSelectedFunction });
        stage.didFinish(descriptorOf(FunctionSchedule::Select).name);
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
            m_output->accessSiteObservations = observeAccessSites(m_program, flow(), m_build);
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
        numberAndEncodeBesideOtherReadersOfSelectedBodies();
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

void Pipeline::runPlacementAndRepresentationStagesTogether()
{
    TimedStage stage(*this, Stage::ChooseRepresentations);
    numberBesideChoosingRepresentations([&](ASCIILiteral name) {
        stage.didFinish(name);
    });
}

void Pipeline::runAllStages()
{
    runAllStages([](const PipelineOutput&) { });
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
    if (m_runner.mayRunAnalysesTogether() && !options.verifiesProgram && !options.isVerifyOnly() && !options.logging.dumpedForms)
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
            CachesAroundStepsThatRunBesideEachOther caches(m_program, m_build);
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

std::unique_ptr<PipelineOutput> Pipeline::takeOutput()
{
    return WTF::move(m_output);
}

std::unique_ptr<PipelineOutput> runPasses(std::unique_ptr<Program> program, PassRunner& runner)
{
    return runPasses(WTF::move(program), runner, [](const PipelineOutput&) { });
}

std::unique_ptr<PipelineOutput> runPasses(std::unique_ptr<Program> program, PassRunner& runner, NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries)
{
    Pipeline pipeline(*program, runner);
    pipeline.output().program = WTF::move(program);
    pipeline.runAllStages(readBesideLastEffectSummaries);
    return pipeline.takeOutput();
}

std::unique_ptr<PipelineOutput> runPipeline(std::span<const uint8_t> programBytes, const ExternalFunctionBodies* externalBodies, BuildContext& build, NOESCAPE const ScopedLambda<void()>& didReadProgram, NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries)
{
    auto program = readProgram(programBytes, externalBodies, build);
    didReadProgram();
    if (!program) {
        build.diagnostics().finish(nullptr);
        return nullptr;
    }
    return runPipeline(WTF::move(program), build, readBesideLastEffectSummaries);
}

std::unique_ptr<PipelineOutput> runPipeline(std::unique_ptr<Program> program, BuildContext& build)
{
    return runPipeline(WTF::move(program), build, [](const PipelineOutput&) { });
}

std::unique_ptr<PipelineOutput> runPipeline(std::unique_ptr<Program> program, BuildContext& build, NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries)
{
    build.resolveFunctionToDump(*program);
    PassRunner runner(build);
    auto output = runPasses(WTF::move(program), runner, readBesideLastEffectSummaries);
    if (build.diagnostics().hasErrors()) {
        build.diagnostics().finish(output->program.get());
        return nullptr;
    }
    return output;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
