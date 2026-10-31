/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionLoop.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPassRunner.h"
#include "AOTPipeline.h"
#include "AOTProgramAnalyses.h"
#include "AOTVector.h"
#include <memory>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

class FactsOfSelectedBodies;
class OutliningSiteFinder;
struct PlacementInputs;
struct ReadersOfOutlinedFunctions;

class Pipeline {
    WTF_MAKE_NONCOPYABLE(Pipeline);
public:
    Pipeline(Program&, PassRunner&);
    ~Pipeline();

    void runAllStages(NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries = [](const PipelineOutput&) { }, NOESCAPE const JudgeOfUsesOfLibraryCode& goesOnBehindFlowAnalysis = [](const FlowReport&) { return true; });
    std::unique_ptr<PipelineOutput> takeOutput() { return WTF::move(m_output); }

    void runReadStage();
    void runPlacementStage();
    void runRepresentationStage();
    bool checkNumberOfFunctions();
    void checkNumberOfClosureFunctions();
    void logSizeOfProgram() const;

    PassOutcome runProgramPass(PassID);
    PassOutcome verify(IRForm);
    void runSchedule(FunctionSchedule, const StepsAroundSchedule& = { });
    void dumpResultOf(PassID, PrintStream&) const;

    PlacementInputs createPlacementInputs(const CallGraph&);

    Program& program() { return m_program; }
    PassRunner& runner() { return m_runner; }
    ProgramAnalyses& analyses() { return m_analyses; }
    PipelineOutput& output() { return *m_output; }
    const FlowResults& flow() const { return *m_output->flow; }

    OptimizationInputs createInputs(OptionSet<ProgramResult> needed, EffectSummariesFor = EffectSummariesFor::Reader);
    OptimizationInputs inputsFor(PassID id) { return createInputs(descriptorOf(id).inputs, readsEffectSummaries(id) ? EffectSummariesFor::Reader : EffectSummariesFor::Nobody); }
    OptimizationInputs inputsFor(FunctionSchedule schedule) { return createInputs(inputsOf(schedule), readsEffectSummaries(schedule) ? EffectSummariesFor::Reader : EffectSummariesFor::Nobody); }

private:
    class TimedStage;

    // To bring the effect summaries up to date is to read every body that has changed and to solve the program again.
    static bool readsEffectSummaries(PassID id)
    {
        return descriptorOf(id).scope == PassScope::Function || id == PassID::RemoveUnusedCalls || id == PassID::AllocateInFrames || id == PassID::OutlineColdOperations;
    }
    static bool readsEffectSummaries(FunctionSchedule schedule) { return schedule != FunctionSchedule::Resolve && schedule != FunctionSchedule::Represent; }

    // The arguments are those that the pass takes behind the editor.
    template<typename Pass, typename... Arguments>
    auto edit(PassID id, const Pass& pass, Arguments&&... arguments)
    {
        inputsFor(id);
        return m_runner.runProgramPass(id, m_program, m_analyses, [&](ProgramEditor& editor) {
            return pass(editor, arguments...);
        });
    }

    template<typename Functor>
    auto editProgram(PassID id, const Functor& functor, BodiesAreChanged bodiesAreChanged = BodiesAreChanged::Anyhow)
    {
        inputsFor(id);
        return m_runner.runProgramPassWithoutEditor(id, m_program, m_analyses, functor, bodiesAreChanged);
    }

    template<typename Analysis, typename... Arguments>
    auto read(PassID id, const Analysis& analysis, Arguments&&... arguments)
    {
        inputsFor(id);
        return m_runner.runAnalysis(id, m_program, [&] {
            return analysis(arguments...);
        });
    }

    void runFlowStage();
    void runOptimizeStage();
    void runSlotStage();
    void runSelectStage();
    // The stages NumberAndEncode and ChooseRepresentations as one.
    void runPlacementAndRepresentationStagesTogether();

    PassOutcome outlineColdOperations(OutliningSiteFinder* = nullptr, const ReadersOfOutlinedFunctions* = nullptr);
    PassOutcome inlineHeads(const ReaderOfScheduledFunction* readEditedFunction = nullptr);
    PassOutcome numberAndEncode(NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites);
    const CallGraph& prepareToReadSelectedBodiesBesideNumbering();
    void collectRegExpsAndElideStackChecks(const CallGraph&);
    void countGenericVersions();
    void didPlace();
    // None unless the report lists the sites.
    const Heat* heatForReport();

    void advanceTo(IRForm, bool didRunProducer);
    void didAddFunctions();

    std::unique_ptr<PipelineOutput> m_output;
    Program& m_program;
    PassRunner& m_runner;
    BuildContext& m_build;
    ProgramAnalyses m_analyses;
    PlacedTableSizes m_placedTableSizes;
    // From the loop that selects until the bodies are numbered.
    std::unique_ptr<FactsOfSelectedBodies> m_factsOfSelectedBodies;
    BitVector m_copiedFunctions;
    Vector<Int32ParameterCandidate> m_int32ParameterCandidates;
    const JudgeOfUsesOfLibraryCode* m_goesOnBehindFlowAnalysis { nullptr };
};

void computeResultsForTesting(Pipeline&, OptionSet<ProgramResult> needed);
PassOutcome runPassForTesting(Pipeline&, PassID, PrintStream& results);
void commitRequestedFunctionsForTesting(Pipeline&, std::span<const DerivedFunctionKey>, PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
