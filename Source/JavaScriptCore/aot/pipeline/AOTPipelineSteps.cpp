/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPipelineInternal.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTAllocateInnerObjects.h"
#include "AOTBorrowedParameters.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTCheckTypesAtEntry.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCoroutines.h"
#include "AOTCallModules.h"
#include "AOTCreateEntrypoints.h"
#include "AOTDeferClosuresInFields.h"
#include "AOTDeferDefinitions.h"
#include "AOTDeferInitializers.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectSummaries.h"
#include "AOTElideStackChecks.h"
#include "AOTEmbedConstantCollections.h"
#include "AOTFactsOfSelectedBodies.h"
#include "AOTFlowApply.h"
#include "AOTFrameAllocation.h"
#include "AOTFreshResults.h"
#include "AOTHeat.h"
#include "AOTCreatedObjectLayouts.h"
#include "AOTNameCreatedSymbols.h"
#include "AOTMarkResultsUsedAsKeys.h"
#include "AOTNarrowConstantVariables.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTInlineHeads.h"
#include "AOTOutlineColdOperations.h"
#include "AOTPassList.h"
#include "AOTPlacement.h"
#include "AOTPromoteEnvironmentsToVariables.h"
#include "AOTReadCapturesFromEnvironments.h"
#include "AOTRegExpRequests.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTRemoveUnreadExports.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTRemoveUnusedParameters.h"
#include "AOTSiteCachePlacement.h"
#include "AOTSlotPlan.h"
#include "AOTSplitBehindEntryChecks.h"
#include "AOTStringArena.h"
#include "AOTSummarizeForInlining.h"
#include "AOTTaggedLayouts.h"
#include "AOTValueFlowResults.h"
#include "AOTVector.h"
#include "AOTVerifier.h"
#include "AOTVirtualObjectParameters.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static SlotPlanInputs createSlotPlanInputs(Pipeline& pipeline)
{
    const FlowResults& flow = pipeline.flow();
    const PipelineOutput& output = pipeline.output();
    return {
        .flow = flow,
        .callGraph = pipeline.analyses().callGraph(flow),
        .heat = pipeline.analyses().heat(flow),
        .frameAllocation = *output.frameAllocation,
    };
}

PlacementInputs Pipeline::createPlacementInputs(const CallGraph& callGraph)
{
    Pipeline& pipeline = *this;
    const FlowResults& flow = pipeline.flow();
    const PipelineOutput& output = pipeline.output();
    return {
        .flow = flow,
        .callGraph = callGraph,
        .heat = pipeline.analyses().heat(flow),
        .callingConventions = *output.callingConventions,
        .frameAllocation = *output.frameAllocation,
        .literalStrings = *output.literalStrings,
        .constantCollections = *output.constantCollections,
        .slots = *output.slots,
        .regExpRequests = *output.regExpRequests,
    };
}

PassOutcome Pipeline::allocateTaggedLayouts()
{
    editProgram(PassID::AllocateTaggedLayouts, [&] {
        Typed::allocateTaggedLayouts(m_program, m_build);
        Typed::giveLayoutsToCreatedObjects(m_program, m_build);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::checkTypesAtEntry()
{
    edit(PassID::CheckTypesAtEntry, [&](ProgramEditor& editor) {
        Typed::checkTypesAtEntry(editor, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::narrowFieldTypes()
{
    editProgram(PassID::NarrowFieldTypes, [&] {
        m_int32ParameterCandidates = Typed::narrowFieldTypes(m_program, m_build);
    }, BodiesAreChanged::ThroughEditors);
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::nameCreatedSymbols()
{
    editProgram(PassID::NameCreatedSymbols, [&] {
        Typed::nameCreatedSymbols(m_program, m_build);
    }, BodiesAreChanged::ThroughEditors);
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::verify(IRForm form)
{
    VerifierInputs inputs = m_runner.verifierInputs();
    if (inputs.flow)
        inputs.effects = &m_analyses.effects(*inputs.flow);
    bool isRunning = !m_runner.isStopped();
    bool isValid = read(PassID::VerifyProgram, [&] {
        return verifyProgram(m_program, form, m_build, inputs);
    });
    if (isRunning && !isValid && form != IRForm::AsRead && !m_build.options().verifiesAfterEveryStage)
        m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::VerificationFailedAtTheEnd });
    return m_runner.lastProgramPassOutcome();
}

void Pipeline::verifyUnspecifiedValues()
{
    bool isRunning = !m_runner.isStopped();
    bool isValid = read(PassID::VerifyProgram, [&] {
        return Typed::verifyUnspecifiedValues(m_program, m_build);
    });
    if (isRunning && !isValid)
        m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::VerificationFailedAtTheEnd });
}

PassOutcome Pipeline::guardTaintedPositions()
{
    editProgram(PassID::GuardTaintedPositions, [&] {
        Typed::guardTaintedPositions(m_program, m_build, TaintedPositionsRun::BeforeFlow);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::analyzeFlow()
{
    *m_output->flow = read(PassID::AnalyzeFlow, [&] {
        return analyzeValueFlow(m_program, m_program.types, m_build);
    });
    m_runner.setVerifierInputs({ .flow = &flow() });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::applyFlowResults()
{
    editProgram(PassID::ApplyFlowResults, [&] {
        Typed::applyFlowResults(m_program, *m_output->flow, m_build);
        m_build.destroyLater(FlowApplyAccess::takeApplication(*m_output->flow));
        if (m_output->flow->hasResults())
            Typed::guardTaintedPositionsAfterFlow(m_program, *m_output->flow, m_build);
        if (m_output->flow->hasResults())
            Typed::expectInt32ParametersWithRemainingChecks(m_program, std::exchange(m_int32ParameterCandidates, { }).span(), m_build);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::checkLockedBuiltinWrites()
{
    read(PassID::CheckLockedBuiltinWrites, [&] {
        Typed::checkLockedBuiltinWrites(m_program, flow(), m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::buildCallGraph()
{
    m_analyses.callGraph(flow());
    return PassOutcome::Unchanged;
}

PassOutcome Pipeline::computeEffectSummaries()
{
    m_analyses.effects(flow());
    return PassOutcome::Unchanged;
}

PassOutcome Pipeline::estimateHeat()
{
    m_analyses.heat(flow());
    return PassOutcome::Unchanged;
}

PassOutcome Pipeline::summarizeForInlining()
{
    m_analyses.inlining(flow());
    return PassOutcome::Unchanged;
}

PassOutcome Pipeline::splitBehindEntryChecks()
{
    // The pass drops the heat that it asks for, and leaves most bodies as they are.
    m_analyses.keepPartsOfBodiesForNextHeat();
    edit(PassID::SplitBehindEntryChecks, [&](ProgramEditor& editor) {
        Typed::splitBehindEntryChecks(editor, &inputsFor(PassID::SplitBehindEntryChecks).heat, m_build);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::skipAnsweredEntryChecks()
{
    edit(PassID::SkipAnsweredEntryChecks, [&](ProgramEditor& editor) {
        Typed::skipAnsweredEntryChecks(editor, inputsFor(PassID::SkipAnsweredEntryChecks).heat, m_build);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::removeUnreadExports()
{
    edit(PassID::RemoveUnreadExports, [&](ProgramEditor& editor) {
        Typed::removeUnreadExports(editor, flow(), m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::deferDefinitions()
{
    edit(PassID::DeferDefinitions, [&](ProgramEditor& editor) {
        Typed::deferDefinitions(editor, flow(), m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::narrowConstantVariables()
{
    edit(PassID::NarrowConstantVariables, [&](ProgramEditor& editor) {
        m_output->flow->setConstantLists(Typed::narrowConstantVariables(editor, flow(), m_build));
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::deferInitializers()
{
    edit(PassID::DeferInitializers, [&](ProgramEditor& editor) {
        Typed::deferInitializers(editor, inputsFor(PassID::DeferInitializers), m_runner);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::markResultsUsedAsKeys()
{
    edit(PassID::MarkResultsUsedAsKeys, [&](ProgramEditor& editor) {
        Typed::markResultsUsedAsKeys(editor, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::deferClosuresInFields()
{
    edit(PassID::DeferClosuresInFields, [&](ProgramEditor& editor) {
        Typed::deferClosuresInFields(editor, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::promoteEnvironmentsToVariables()
{
    edit(PassID::PromoteEnvironmentsToVariables, [&](ProgramEditor& editor) {
        Typed::promoteEnvironmentsToVariables(editor, *m_output->flow, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::readCapturesFromEnvironments()
{
    edit(PassID::ReadCapturesFromEnvironments, [&](ProgramEditor& editor) {
        Typed::readCapturesFromEnvironments(editor, *m_output->flow, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::removeUnreadFields()
{
    edit(PassID::RemoveUnreadFields, [&](ProgramEditor& editor) {
        Typed::removeUnreadFields(editor, inputsFor(PassID::RemoveUnreadFields), m_runner);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::removeUnusedParameters()
{
    edit(PassID::RemoveUnusedParameters, [&](ProgramEditor& editor) {
        Typed::removeUnusedParameters(editor, inputsFor(PassID::RemoveUnusedParameters), m_runner);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::callModules()
{
    edit(PassID::CallModules, [&](ProgramEditor& editor) {
        Typed::callModules(editor, flow(), m_build);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::removeUnreachableFunctions()
{
    edit(PassID::RemoveUnreachableFunctions, [&](ProgramEditor& editor) {
        // Select gives calls by value other forms, and the body that throws is written in the form in front of it.
        if (m_program.form() == IRForm::Prepared)
            closeEntrypointsThatNothingEnters(editor, flow(), m_output->calleesOfSites, m_build);
        Typed::removeUnreachableFunctions(editor, m_analyses.callGraph(flow()), m_build);
    });
    m_output->derivedFunctions->forgetFunctionsWithoutBody(m_program);
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::splitFreshResults()
{
    edit(PassID::SplitFreshResults, [&](ProgramEditor& editor) {
        OptimizationInputs inputs = inputsFor(PassID::SplitFreshResults);
        inputs.callGraph.computeRanks(m_build);
        Typed::splitFreshResults(editor, inputs, m_runner);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::promoteAggregateParameters()
{
    edit(PassID::PromoteAggregateParameters, [&](ProgramEditor& editor) {
        Typed::promoteAggregateParameters(editor, inputsFor(PassID::PromoteAggregateParameters), m_build);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::splitCoroutines()
{
    edit(PassID::SplitCoroutines, [&](ProgramEditor& editor) {
        Typed::splitCoroutines(editor, inputsFor(PassID::SplitCoroutines), m_runner);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::createEntrypoints()
{
    edit(PassID::CreateEntrypoints, [&](ProgramEditor& editor) {
        Typed::createEntrypoints(editor, inputsFor(PassID::CreateEntrypoints), m_build);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::chooseCallingConventions()
{
    *m_output->callingConventions = edit(PassID::ChooseCallingConventions, [&](ProgramEditor& editor) {
        return Typed::chooseCallingConventions(editor, inputsFor(PassID::ChooseCallingConventions), m_runner);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::allocateInFrames()
{
    if (!m_runner.isStopped()) {
        inputsFor(PassID::AllocateInFrames);
        m_output->flow->setBorrowedParameters(m_analyses.borrowedParameters());
        m_output->flow->setFrameArrayParameters(computeFrameArrayParameters(m_program, m_build));
    }
    *m_output->frameAllocation = edit(PassID::AllocateInFrames, [&](ProgramEditor& editor) {
        return Typed::allocateInFrames(editor, inputsFor(PassID::AllocateInFrames), *m_output->flow, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::allocateInnerObjects()
{
    edit(PassID::AllocateInnerObjects, [&](ProgramEditor& editor) {
        OptimizationInputs inputs = inputsFor(PassID::AllocateInnerObjects);
        Typed::allocateInnerObjects(editor, flow(), inputs.callGraph, inputs.heat, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::numberLiteralStrings()
{
    internPiecesOfModulePaths(m_program);
    *m_output->literalStrings = read(PassID::NumberLiteralStrings, [&] {
        return Typed::numberLiteralStrings(m_program, flow(), m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::embedConstantCollections()
{
    *m_output->constantCollections = edit(PassID::EmbedConstantCollections, [&](ProgramEditor& editor) {
        // The table of borrowed parameters looks at the bodies that have changed. What it reads of them is kept for the effect summaries, which are brought up to date a few passes on.
        const EffectSummaries* summariesFromFlow = m_analyses.keptSummariesFromFlow();
        std::optional<EffectSummaries> ownSummaries;
        if (!summariesFromFlow)
            summariesFromFlow = &ownSummaries.emplace(createSummariesFromFlow(m_program, flow(), m_analyses.borrowedParameters(), m_build));
        return Typed::embedConstantCollections(editor, flow(), summariesFromFlow, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::collectRegExpLiterals()
{
    *m_output->regExpRequests = read(PassID::CollectRegExpLiterals, [&] {
        StringArena strings(m_program.strings);
        return collectRegExpRequests(m_program, flow(), strings, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::outlineColdOperations(OutliningSiteFinder* finder, const ReadersOfOutlinedFunctions* readers)
{
    edit(PassID::OutlineColdOperations, [&](ProgramEditor& editor) {
        Typed::outlineColdOperations(editor, inputsFor(PassID::OutlineColdOperations), m_build, finder, readers);
    });
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
    edit(PassID::InlineHeads, [&](ProgramEditor& editor) {
        Typed::inlineHeads(editor, inputsFor(PassID::InlineHeads), m_build, readEditedFunction, &directCalleesOf);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::elideStackChecks()
{
    *m_output->stackChecks = read(PassID::ElideStackChecks, [&] {
        return Typed::elideStackChecks(m_program, m_analyses.callGraph(flow()), m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::planSlots()
{
    SlotPlanInputs inputs = createSlotPlanInputs(*this);
    *m_output->slots = read(PassID::PlanSlots, [&] {
        return Typed::planSlots(m_program, inputs, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

void Pipeline::planSlotsBesideOtherReadersOfPreparedBodies()
{
    internPiecesOfModulePaths(m_program);
    SlotPlanInputs inputs = createSlotPlanInputs(*this);
    inputsFor(PassID::PlanSlots);
    inputsFor(PassID::NumberLiteralStrings);
    CachesAroundStepsThatRunBesideEachOther caches(m_program, m_build);
    *m_output->slots = m_runner.runAnalysisBeside(PassID::PlanSlots, m_program, [&] {
        return Typed::planSlots(m_program, inputs, m_build);
    }, [&] {
        *m_output->literalStrings = m_runner.runAnalysisBeside(PassID::NumberLiteralStrings, m_program, [&] {
            return Typed::numberLiteralStrings(m_program, flow(), m_build);
        }, [&] {
            m_analyses.effects(flow());
        });
    });
}

PassOutcome Pipeline::numberAndEncodeBesideOtherReadersOfSelectedBodies()
{
    inputsFor(PassID::CollectRegExpLiterals);
    inputsFor(PassID::ElideStackChecks);
    const CallGraph& callGraph = m_analyses.callGraph(flow());
    m_runner.didCompute(descriptorOf(PassID::CollectRegExpLiterals).outputs);
    return numberAndEncode([&] {
        collectRegExpsAndElideStackChecks(callGraph);
    });
}

void Pipeline::collectRegExpsAndElideStackChecks(const CallGraph& callGraph)
{
    *m_output->regExpRequests = m_runner.runAnalysisBeside(PassID::CollectRegExpLiterals, m_program, [&] {
        StringArena strings(m_program.strings);
        return collectRegExpRequests(m_program, flow(), strings, m_build);
    }, [&] {
        *m_output->stackChecks = m_runner.runAnalysis(PassID::ElideStackChecks, m_program, [&] {
            return Typed::elideStackChecks(m_program, callGraph, m_build);
        });
    });
    if (!m_runner.isStopped())
        m_output->startCompilingRegExps(m_program, m_build);
}

// Of a placement, the choice of representations asks for slots, for the field access of an operation and for the statics.
void Pipeline::numberBesideChoosingRepresentations(NOESCAPE const ScopedLambda<void(ASCIILiteral)>& didFinish)
{
    bool wasRunning = !m_runner.isStopped();
    inputsFor(PassID::CollectRegExpLiterals);
    inputsFor(PassID::ElideStackChecks);
    const CallGraph& callGraph = m_analyses.callGraph(flow());
    m_runner.didCompute(descriptorOf(PassID::CollectRegExpLiterals).outputs);
    m_build.statistics().count(StatisticID::GenericVersionsBehindEntryChecksCompiled, std::ranges::count_if(m_program.functions, [](const Function& function) {
        return function.hasBody() && function.role == FunctionRole::GenericVersion;
    }));

    PlacementInputs inputs = createPlacementInputs(callGraph);
    PlacementInSteps steps(inputs, m_build, m_factsOfSelectedBodies ? &m_factsOfSelectedBodies->scansForPlacement() : nullptr);
    Placement& placement = *m_output->placement;
    placement = edit(PassID::NumberAndEncode, [&](ProgramEditor& editor) {
        return steps.numberWhatRepresentationsAsk(editor, [&] {
            collectRegExpsAndElideStackChecks(callGraph);
        });
    });
    didFinish(nameOf(PassID::NumberAndEncode));
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
    runSchedule(FunctionSchedule::Represent, { .inFront = &inFront, .behind = readersOfRepresentedFunction, .beside = &beside });
    didFinish(descriptorOf(FunctionSchedule::Represent).name);

    *m_output->siteCaches = edit(PassID::PlaceSiteCaches, [&](ProgramEditor& editor) {
        steps.resolveSites(editor, placement);
        return Typed::placeSiteCaches(editor, placement, flow(), heat, m_build, siteCacheRequests.span());
    });
    m_build.destroyLater(WTF::move(siteCacheRequests));
    m_factsOfSelectedBodies = nullptr;
    m_placedTableSizes = placement.tableSizes();
    m_runner.setVerifierInputs({ .flow = &flow(), .placed = &m_placedTableSizes });
    didFinish(nameOf(PassID::PlaceSiteCaches));
}

PassOutcome Pipeline::numberAndEncode()
{
    return numberAndEncode([] { });
}

PassOutcome Pipeline::numberAndEncode(NOESCAPE const ScopedLambda<void()>& readBodiesBesideCollectingSites)
{
    m_build.statistics().count(StatisticID::GenericVersionsBehindEntryChecksCompiled, std::ranges::count_if(m_program.functions, [](const Function& function) {
        return function.hasBody() && function.role == FunctionRole::GenericVersion;
    }));
    PlacementInputs inputs = createPlacementInputs(m_analyses.callGraph(flow()));
    *m_output->placement = edit(PassID::NumberAndEncode, [&](ProgramEditor& editor) {
        return Typed::numberAndEncode(editor, inputs, m_build, readBodiesBesideCollectingSites, m_factsOfSelectedBodies ? &m_factsOfSelectedBodies->scansForPlacement() : nullptr);
    });
    m_factsOfSelectedBodies = nullptr;
    m_placedTableSizes = m_output->placement->tableSizes();
    m_runner.setVerifierInputs({ .flow = &flow(), .placed = &m_placedTableSizes });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::placeSiteCaches()
{
    *m_output->siteCaches = edit(PassID::PlaceSiteCaches, [&](ProgramEditor& editor) {
        return Typed::placeSiteCaches(editor, *m_output->placement, flow(), m_analyses.heat(flow()), m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::checkPlacement()
{
    PlacementInputs inputs = createPlacementInputs(m_analyses.callGraph(flow()));
    read(PassID::CheckPlacement, [&] {
        Typed::checkPlacement(m_program, inputs, *m_output->placement, *m_output->siteCaches, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

#define AOT_FUNCTION_PASS_CASE(id, function) case PassID::id:

PassOutcome Pipeline::runProgramPass(PassID id)
{
    switch (id) {
    case PassID::AllocateTaggedLayouts:
        return allocateTaggedLayouts();
    case PassID::CheckTypesAtEntry:
        return checkTypesAtEntry();
    case PassID::SplitBehindEntryChecks:
        return splitBehindEntryChecks();
    case PassID::SkipAnsweredEntryChecks:
        return skipAnsweredEntryChecks();
    case PassID::NarrowFieldTypes:
        return narrowFieldTypes();
    case PassID::NameCreatedSymbols:
        return nameCreatedSymbols();
    case PassID::VerifyProgram:
        return verify(m_program.form());
    case PassID::GuardTaintedPositions:
        return guardTaintedPositions();
    case PassID::AnalyzeFlow:
        return analyzeFlow();
    case PassID::ApplyFlowResults:
        return applyFlowResults();
    case PassID::CheckLockedBuiltinWrites:
        return checkLockedBuiltinWrites();
    case PassID::BuildCallGraph:
        return buildCallGraph();
    case PassID::ComputeEffectSummaries:
        return computeEffectSummaries();
    case PassID::EstimateHeat:
        return estimateHeat();
    case PassID::SummarizeForInlining:
        return summarizeForInlining();
    case PassID::RemoveUnreadExports:
        return removeUnreadExports();
    case PassID::PromoteEnvironmentsToVariables:
        return promoteEnvironmentsToVariables();
    case PassID::DeferDefinitions:
        return deferDefinitions();
    case PassID::NarrowConstantVariables:
        return narrowConstantVariables();
    case PassID::DeferInitializers:
        return deferInitializers();
    case PassID::MarkResultsUsedAsKeys:
        return markResultsUsedAsKeys();
    case PassID::DeferClosuresInFields:
        return deferClosuresInFields();
    case PassID::ReadCapturesFromEnvironments:
        return readCapturesFromEnvironments();
    case PassID::RemoveUnreadFields:
        return removeUnreadFields();
    case PassID::RemoveUnusedParameters:
        return removeUnusedParameters();
    case PassID::RemoveUnreachableFunctions:
        return removeUnreachableFunctions();
    case PassID::SplitFreshResults:
        return splitFreshResults();
    case PassID::PromoteAggregateParameters:
        return promoteAggregateParameters();
    case PassID::SplitCoroutines:
        return splitCoroutines();
    case PassID::CallModules:
        return callModules();
    case PassID::CreateEntrypoints:
        return createEntrypoints();
    case PassID::ChooseCallingConventions:
        return chooseCallingConventions();
    case PassID::AllocateInFrames:
        return allocateInFrames();
    case PassID::AllocateInnerObjects:
        return allocateInnerObjects();
    case PassID::NumberLiteralStrings:
        return numberLiteralStrings();
    case PassID::EmbedConstantCollections:
        return embedConstantCollections();
    case PassID::CollectRegExpLiterals:
        return collectRegExpLiterals();
    case PassID::OutlineColdOperations:
        return outlineColdOperations();
    case PassID::InlineHeads:
        return inlineHeads();
    case PassID::ElideStackChecks:
        return elideStackChecks();
    case PassID::PlanSlots:
        return planSlots();
    case PassID::NumberAndEncode:
        return numberAndEncode();
    case PassID::PlaceSiteCaches:
        return placeSiteCaches();
    case PassID::CheckPlacement:
        return checkPlacement();
    case PassID::None:
    case PassID::GenerateCode:
    FOR_EACH_AOT_FUNCTION_PASS(AOT_FUNCTION_PASS_CASE)
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#undef AOT_FUNCTION_PASS_CASE

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
