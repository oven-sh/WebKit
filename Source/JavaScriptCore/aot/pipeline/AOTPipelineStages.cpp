/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPipelineInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTChooseCallingConventions.h"
#include "AOTDefinitions.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsCheck.h"
#include "AOTElideStackChecks.h"
#include "AOTEmbedConstantCollections.h"
#include "AOTFlowResults.h"
#include "AOTFrameAllocation.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionLoop.h"
#include "AOTGroupCallsByValue.h"
#include "AOTHeat.h"
#include "AOTIR.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTPhaseTimes.h"
#include "AOTPlacement.h"
#include "AOTRegExpRequests.h"
#include "AOTReuseEnvironments.h"
#include "AOTSiteCachePlacement.h"
#include "AOTSlotPlan.h"
#include "AOTThreads.h"
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

    void verify(IRForm form)
    {
        m_pipeline.verify(form);
        m_times.didFinish(nameOf(PassID::VerifyProgram));
    }

    // Every build verifies the program it reads and the program it hands to the back end.
    void verifyOnRequest(IRForm form)
    {
        if (m_pipeline.runner().build().options().verifiesAfterEveryStage)
            verify(form);
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
    createDefaultResult(output->callGroups);
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

static std::unique_ptr<Program> readProgram(std::span<const uint8_t> programBytes, BuildContext& build)
{
    PhaseTimes times(build, Stage::ReadAndVerify);
    auto program = Program::read(programBytes, build);
    times.didFinish("Read"_s);
    return program;
}

PipelineOutput::PipelineOutput() = default;
PipelineOutput::~PipelineOutput() = default;

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

OptimizationInputs Pipeline::createInputs(OptionSet<ProgramResult> needed)
{
    OptimizationInputs inputs = m_analyses.inputs(flow(), needed);
    if (needed.contains(ProgramResult::CallingConventions))
        inputs.callingConventions = m_output->callingConventions.get();
    if (needed.contains(ProgramResult::SlotPlan))
        inputs.slots = m_output->slots.get();
    if (needed.contains(ProgramResult::Placement))
        inputs.placement = m_output->placement.get();
    return inputs;
}

void Pipeline::runSchedule(FunctionSchedule schedule)
{
    OptimizationInputs inputs = inputsFor(schedule);
    if (m_runner.isStopped())
        return;
    runOnFunctions(schedule, m_program, inputs, m_runner, m_analyses);
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

void Pipeline::runReadStage()
{
    TimedStage stage(*this, Stage::ReadAndVerify);
    m_runner.didReachForm(m_program);
    stage.run(PassID::AdmitAssertedObjects);
    stage.run(FunctionSchedule::Type);
    stage.verify(IRForm::AsRead);
    if (m_build.options().logging.dumpedPass == PassID::AdmitAssertedObjects) [[unlikely]]
        Definitions(m_program, m_build).dumpCounts(WTF::dataFile());
}

void Pipeline::runFlowStage()
{
    TimedStage stage(*this, Stage::AnalyzeFlow);
    stage.run(PassID::AnalyzeFlow);
    bool wasRunning = !m_runner.isStopped();
    stage.run(PassID::ApplyFlowResults);
    advanceTo(IRForm::Prepared, wasRunning);
    stage.run(PassID::CheckLockedBuiltinWrites);
}

void Pipeline::runOptimizeStage()
{
    TimedStage stage(*this, Stage::Optimize);
    if (m_build.validates() && !m_runner.isStopped())
        checkDeclaredEffects(m_build.diagnostics());
    stage.run(PassID::PromoteEnvironmentsToVariables);
    stage.run(PassID::RemoveUnreadExports);
    stage.run(PassID::DeferDefinitions);
    stage.run(FunctionSchedule::Prepare);
    stage.run(PassID::PromoteAggregateParameters);
    stage.run(PassID::SplitFreshResults);
    stage.run(PassID::SplitCoroutines);
    stage.run(PassID::GroupCallsByValue);
    stage.run(PassID::CreateEntrypoints);
    stage.run(FunctionSchedule::Optimize);
    stage.run(PassID::RemoveUnreadFields);
    stage.run(PassID::RemoveUnusedResults);
    stage.run(PassID::RemoveUnusedParameters);
    stage.run(PassID::SplitFreshResults);
    stage.run(PassID::RemoveUnusedResults);
    stage.run(PassID::EmbedConstantCollections);
    stage.run(PassID::ChooseCallingConventions);
    stage.run(PassID::AllocateInFrames);
    stage.run(PassID::ReuseEnvironments);
    stage.run(PassID::NumberLiteralStrings);
    stage.verifyOnRequest(IRForm::Prepared);
}

void Pipeline::runSlotStage()
{
    TimedStage stage(*this, Stage::PlanSlots);
    stage.run(PassID::PlanSlots);
}

void Pipeline::runSelectStage()
{
    TimedStage stage(*this, Stage::Select);
    bool wasRunning = !m_runner.isStopped();
    stage.run(FunctionSchedule::Select);
    stage.verifyOnRequest(IRForm::Selected);
    advanceTo(IRForm::Selected, wasRunning);
    if (!m_runner.isStopped())
        m_output->accessSiteLevels = gradeAccessSites(m_program, flow(), m_build);
    stage.run(PassID::OutlineColdOperations);
    stage.run(PassID::SplitFunctionsAtCalls);
    stage.run(PassID::RemoveUnreachableFunctions);
    stage.run(PassID::ElideStackChecks);
    stage.run(PassID::CollectRegExpLiterals);
}

void Pipeline::runPlacementStage()
{
    TimedStage stage(*this, Stage::NumberAndEncode);
    bool wasRunning = !m_runner.isStopped();
    stage.run(PassID::NumberAndEncode);
    stage.verifyOnRequest(IRForm::Placed);
    advanceTo(IRForm::Placed, wasRunning);
    stage.run(PassID::PlaceSiteCaches);
    stage.run(PassID::CheckPlacement);
}

void Pipeline::runRepresentationStage()
{
    TimedStage stage(*this, Stage::ChooseRepresentations);
    stage.run(FunctionSchedule::Represent);
    stage.verify(IRForm::Placed);
}

void Pipeline::runAllStages()
{
    runReadStage();
    if (m_build.options().isVerifyOnly())
        return;
    runFlowStage();
    runOptimizeStage();
    runSlotStage();
    runSelectStage();
    runPlacementStage();
    runRepresentationStage();
    {
        // A run that was stopped after a pass hands out what its passes used. What it never computed stays the default result.
        PhaseTimes times(m_build, Stage::GenerateCode);
        m_output->effects = m_analyses.takeEffects(flow());
        m_output->heat = m_analyses.takeHeat(flow());
        if (m_output->accessSiteLevels.isEmpty())
            m_output->accessSiteLevels = gradeAccessSites(m_program, flow(), m_build);
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
    Pipeline pipeline(*program, runner);
    pipeline.runAllStages();
    auto output = pipeline.takeOutput();
    output->program = WTF::move(program);
    return output;
}

std::unique_ptr<PipelineOutput> runPipeline(std::span<const uint8_t> programBytes, BuildContext& build)
{
    auto program = readProgram(programBytes, build);
    if (!program) {
        build.diagnostics().finish(nullptr);
        return nullptr;
    }
    return runPipeline(WTF::move(program), build);
}

std::unique_ptr<PipelineOutput> runPipeline(std::unique_ptr<Program> program, BuildContext& build)
{
    build.resolveFunctionToDump(*program);
    PassRunner runner(build);
    auto output = runPasses(WTF::move(program), runner);
    if (build.diagnostics().hasErrors()) {
        build.diagnostics().finish(output->program.get());
        return nullptr;
    }
    return output;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
