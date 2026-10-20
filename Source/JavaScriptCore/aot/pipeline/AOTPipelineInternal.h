/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPassRunner.h"
#include "AOTPipeline.h"
#include "AOTProgramAnalyses.h"
#include <memory>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

class Pipeline {
    WTF_MAKE_NONCOPYABLE(Pipeline);
public:
    Pipeline(Program&, PassRunner&);
    ~Pipeline();

    void runAllStages();
    std::unique_ptr<PipelineOutput> takeOutput();

    void runReadStage();
    void runFlowStage();
    void runOptimizeStage();
    void runSlotStage();
    void runSelectStage();
    void runPlacementStage();
    void runRepresentationStage();

    PassOutcome runProgramPass(PassID);
    void runSchedule(FunctionSchedule);
    void dumpResultOf(PassID, PrintStream&) const;

    PassOutcome admitAssertedObjects();
    PassOutcome verify(IRForm);
    PassOutcome analyzeFlow();
    PassOutcome applyFlowResults();
    PassOutcome checkLockedBuiltinWrites();
    PassOutcome buildCallGraph();
    PassOutcome computeEffectSummaries();
    PassOutcome estimateHeat();
    PassOutcome summarizeForInlining();
    PassOutcome removeUnreadExports();
    PassOutcome deferDefinitions();
    PassOutcome deferClosuresInFields();
    PassOutcome promoteEnvironmentsToVariables();
    PassOutcome removeUnreadFields();
    PassOutcome removeUnusedResults();
    PassOutcome removeUnusedParameters();
    PassOutcome removeUnreachableFunctions();
    PassOutcome splitFreshResults();
    PassOutcome promoteAggregateParameters();
    PassOutcome splitCoroutines();
    PassOutcome createEntrypoints();
    PassOutcome groupCallsByValue();
    PassOutcome chooseCallingConventions();
    PassOutcome allocateInFrames();
    PassOutcome reuseEnvironments();
    PassOutcome numberLiteralStrings();
    PassOutcome embedConstantCollections();
    PassOutcome collectRegExpLiterals();
    PassOutcome outlineColdOperations();
    PassOutcome splitFunctionsAtCalls();
    PassOutcome elideStackChecks();
    PassOutcome planSlots();
    PassOutcome numberAndEncode();
    PassOutcome placeSiteCaches();
    PassOutcome checkPlacement();

    Program& program() { return m_program; }
    PassRunner& runner() { return m_runner; }
    ProgramAnalyses& analyses() { return m_analyses; }
    PipelineOutput& output() { return *m_output; }
    const FlowResults& flow() const { return *m_output->flow; }

    OptimizationInputs createInputs(OptionSet<ProgramResult> needed);
    OptimizationInputs inputsFor(PassID id) { return createInputs(descriptorOf(id).inputs); }
    OptimizationInputs inputsFor(FunctionSchedule schedule) { return createInputs(inputsOf(schedule)); }

private:
    template<typename Functor>
    auto edit(PassID id, const Functor& functor)
    {
        inputsFor(id);
        return m_runner.runProgramPass(id, m_program, m_analyses, functor);
    }

    template<typename Functor>
    auto editProgram(PassID id, const Functor& functor)
    {
        inputsFor(id);
        return m_runner.runProgramPassWithoutEditor(id, m_program, m_analyses, functor);
    }

    template<typename Functor>
    auto read(PassID id, const Functor& functor)
    {
        inputsFor(id);
        return m_runner.runAnalysis(id, m_program, functor);
    }

    void advanceTo(IRForm, bool didRunProducer);
    void didAddFunctions();

    std::unique_ptr<PipelineOutput> m_output;
    Program& m_program;
    PassRunner& m_runner;
    BuildContext& m_build;
    ProgramAnalyses m_analyses;
    PlacedTableSizes m_placedTableSizes;
};

void computeResultsForTesting(Pipeline&, OptionSet<ProgramResult> needed);
PassOutcome runPassForTesting(Pipeline&, PassID, PrintStream& results);
void commitRequestedFunctionsForTesting(Pipeline&, std::span<const DerivedFunctionKey>, PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
