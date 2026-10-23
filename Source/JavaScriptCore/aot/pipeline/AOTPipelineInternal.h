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
#include "AOTProgramCaches.h"
#include "AOTTypeArena.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

class FactsOfSelectedBodies;
class OutliningSiteFinder;
struct PlacementInputs;
struct ReadersOfOutlinedFunctions;

class CachesAroundStepsThatRunBesideEachOther {
    WTF_MAKE_NONCOPYABLE(CachesAroundStepsThatRunBesideEachOther);
public:
    CachesAroundStepsThatRunBesideEachOther(const Program& program, BuildContext& build)
    {
        if (!program.types.hasOpenPhase())
            m_scratch.emplace(program.types, 0u, TypeInterningPhase::Scratch);
        m_caches.emplace(program, build);
    }

    ~CachesAroundStepsThatRunBesideEachOther()
    {
        m_caches.reset();
        if (m_scratch)
            m_scratch->discard();
    }

private:
    std::optional<TypeInterningPhase> m_scratch;
    std::optional<ProgramCaches> m_caches;
};

class Pipeline {
    WTF_MAKE_NONCOPYABLE(Pipeline);
public:
    Pipeline(Program&, PassRunner&);
    ~Pipeline();

    void runAllStages();
    void runAllStages(NOESCAPE const ReaderOfFinishedBodies& readBesideLastEffectSummaries);
    std::unique_ptr<PipelineOutput> takeOutput();

    void runReadStage();
    void runFlowStage();
    void runOptimizeStage();
    void runSlotStage();
    void runSelectStage();
    void runPlacementStage();
    void runRepresentationStage();
    void runPlacementAndRepresentationStagesTogether();

    PassOutcome runProgramPass(PassID);
    void runSchedule(FunctionSchedule, const StepsAroundSchedule& = { });
    void dumpResultOf(PassID, PrintStream&) const;

    PassOutcome allocateTaggedLayouts();
    PassOutcome checkTypesAtEntry();
    PassOutcome splitBehindEntryChecks();
    PassOutcome skipAnsweredEntryChecks();
    PassOutcome narrowFieldTypes();
    PassOutcome nameCreatedSymbols();
    PassOutcome verify(IRForm);
    void verifyUnspecifiedValues();
    PassOutcome guardTaintedPositions();
    PassOutcome analyzeFlow();
    PassOutcome applyFlowResults();
    PassOutcome checkLockedBuiltinWrites();
    PassOutcome buildCallGraph();
    PassOutcome computeEffectSummaries();
    PassOutcome estimateHeat();
    PassOutcome summarizeForInlining();
    PassOutcome removeUnreadExports();
    PassOutcome deferDefinitions();
    PassOutcome narrowConstantVariables();
    PassOutcome deferInitializers();
    PassOutcome markResultsUsedAsKeys();
    PassOutcome deferClosuresInFields();
    PassOutcome promoteEnvironmentsToVariables();
    PassOutcome readCapturesFromEnvironments();
    PassOutcome removeUnreadFields();
    PassOutcome removeUnusedResults();
    PassOutcome removeUnusedParameters();
    PassOutcome removeUnreachableFunctions();
    PassOutcome splitFreshResults();
    PassOutcome promoteAggregateParameters();
    PassOutcome splitCoroutines();
    PassOutcome callModules();
    PassOutcome createEntrypoints();
    PassOutcome chooseCallingConventions();
    PassOutcome allocateInFrames();
    PassOutcome allocateInnerObjects();
    PassOutcome flattenInnerObjects();
    PassOutcome reuseEnvironments();
    PassOutcome numberLiteralStrings();
    PassOutcome embedConstantCollections();
    PassOutcome collectRegExpLiterals();
    PassOutcome outlineColdOperations(OutliningSiteFinder* = nullptr, const ReadersOfOutlinedFunctions* = nullptr);
    PassOutcome inlineHeads(const ReaderOfScheduledFunction* readEditedFunction = nullptr);
    PassOutcome elideStackChecks();
    PassOutcome planSlots();
    PassOutcome numberAndEncode();
    PassOutcome placeSiteCaches();
    PassOutcome checkPlacement();

    void planSlotsBesideOtherReadersOfPreparedBodies();
    PassOutcome numberAndEncodeBesideOtherReadersOfSelectedBodies();
    PassOutcome numberAndEncode(NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites);
    void collectRegExpsAndElideStackChecks(const CallGraph&);
    // The stages NumberAndEncode and ChooseRepresentations as one. It says when a step has ended.
    void numberBesideChoosingRepresentations(NOESCAPE const ScopedLambda<void(ASCIILiteral)>& didFinish);
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
    // To bring the effect summaries up to date is to read every body that has changed and to solve the program again.
    static bool readsEffectSummaries(PassID id)
    {
        return descriptorOf(id).scope == PassScope::Function || id == PassID::AllocateInFrames || id == PassID::OutlineColdOperations;
    }
    static bool readsEffectSummaries(FunctionSchedule schedule) { return schedule != FunctionSchedule::Resolve && schedule != FunctionSchedule::Represent; }

    template<typename Functor>
    auto edit(PassID id, const Functor& functor)
    {
        inputsFor(id);
        return m_runner.runProgramPass(id, m_program, m_analyses, functor);
    }

    template<typename Functor>
    auto editProgram(PassID id, const Functor& functor, BodiesAreChanged bodiesAreChanged = BodiesAreChanged::Anyhow)
    {
        inputsFor(id);
        return m_runner.runProgramPassWithoutEditor(id, m_program, m_analyses, functor, bodiesAreChanged);
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
    // From the loop that selects until the bodies are numbered.
    std::unique_ptr<FactsOfSelectedBodies> m_factsOfSelectedBodies;
    Vector<Int32ParameterCandidate> m_int32ParameterCandidates;
};

void computeResultsForTesting(Pipeline&, OptionSet<ProgramResult> needed);
PassOutcome runPassForTesting(Pipeline&, PassID, PrintStream& results);
void commitRequestedFunctionsForTesting(Pipeline&, std::span<const DerivedFunctionKey>, PrintStream&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
