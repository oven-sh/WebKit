/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPipelineInternal.h"

#if ENABLE(AOT)

#include "AOTAdmitAssertedObjects.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCoroutines.h"
#include "AOTCreateEntrypoints.h"
#include "AOTDeferClosuresInFields.h"
#include "AOTDeferDefinitions.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectSummaries.h"
#include "AOTElideStackChecks.h"
#include "AOTEmbedConstantCollections.h"
#include "AOTFlowAnalysis.h"
#include "AOTFrameAllocation.h"
#include "AOTFreshResults.h"
#include "AOTGroupCallsByValue.h"
#include "AOTHeat.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTOutlineColdOperations.h"
#include "AOTPassList.h"
#include "AOTPlacement.h"
#include "AOTPromoteEnvironmentsToVariables.h"
#include "AOTRegExpRequests.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTRemoveUnreadExports.h"
#include "AOTRemoveUnreadFields.h"
#include "AOTRemoveUnusedParameters.h"
#include "AOTRemoveUnusedResults.h"
#include "AOTReuseEnvironments.h"
#include "AOTSiteCachePlacement.h"
#include "AOTSlotPlan.h"
#include "AOTSplitFunctionsAtCalls.h"
#include "AOTStringArena.h"
#include "AOTSummarizeForInlining.h"
#include "AOTValueFlowResults.h"
#include "AOTVerifier.h"
#include "AOTVirtualObjectParameters.h"

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
        .environmentReuse = *output.environmentReuse,
    };
}

static PlacementInputs createPlacementInputs(Pipeline& pipeline)
{
    const FlowResults& flow = pipeline.flow();
    const PipelineOutput& output = pipeline.output();
    return {
        .flow = flow,
        .callGraph = pipeline.analyses().callGraph(flow),
        .heat = pipeline.analyses().heat(flow),
        .callingConventions = *output.callingConventions,
        .frameAllocation = *output.frameAllocation,
        .environmentReuse = *output.environmentReuse,
        .literalStrings = *output.literalStrings,
        .constantCollections = *output.constantCollections,
        .slots = *output.slots,
        .regExpRequests = *output.regExpRequests,
    };
}

PassOutcome Pipeline::admitAssertedObjects()
{
    editProgram(PassID::AdmitAssertedObjects, [&] {
        Typed::admitAssertedObjects(m_program, m_build);
    });
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

PassOutcome Pipeline::analyzeFlow()
{
    *m_output->flow = read(PassID::AnalyzeFlow, [&] {
        if (m_build.recordedOptions().analyzesValueFlow())
            return analyzeValueFlow(m_program, m_program.types, m_build);
        return Typed::analyzeFlow(m_program, m_program.types, m_build);
    });
    m_runner.setVerifierInputs({ .flow = &flow() });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::applyFlowResults()
{
    editProgram(PassID::ApplyFlowResults, [&] {
        Typed::applyFlowResults(m_program, flow(), m_build);
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
        Typed::deferDefinitions(editor, m_build);
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

PassOutcome Pipeline::removeUnreadFields()
{
    edit(PassID::RemoveUnreadFields, [&](ProgramEditor& editor) {
        Typed::removeUnreadFields(editor, inputsFor(PassID::RemoveUnreadFields), m_runner);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::removeUnusedResults()
{
    edit(PassID::RemoveUnusedResults, [&](ProgramEditor& editor) {
        Typed::removeUnusedResults(editor, inputsFor(PassID::RemoveUnusedResults), m_runner);
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

PassOutcome Pipeline::removeUnreachableFunctions()
{
    edit(PassID::RemoveUnreachableFunctions, [&](ProgramEditor& editor) {
        Typed::removeUnreachableFunctions(editor, m_analyses.callGraph(flow()), m_build);
    });
    m_output->derivedFunctions->forgetFunctionsWithoutBody(m_program);
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::splitFreshResults()
{
    edit(PassID::SplitFreshResults, [&](ProgramEditor& editor) {
        Typed::splitFreshResults(editor, inputsFor(PassID::SplitFreshResults), m_runner);
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
        Typed::createEntrypoints(editor, inputsFor(PassID::CreateEntrypoints), *m_output->callGroups, m_build);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::groupCallsByValue()
{
    *m_output->callGroups = read(PassID::GroupCallsByValue, [&] {
        return Typed::groupCallsByValue(m_program, flow(), m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::chooseCallingConventions()
{
    *m_output->callingConventions = edit(PassID::ChooseCallingConventions, [&](ProgramEditor& editor) {
        return Typed::chooseCallingConventions(editor, inputsFor(PassID::ChooseCallingConventions), *m_output->callGroups, m_runner);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::allocateInFrames()
{
    *m_output->frameAllocation = edit(PassID::AllocateInFrames, [&](ProgramEditor& editor) {
        return Typed::allocateInFrames(editor, inputsFor(PassID::AllocateInFrames), m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::reuseEnvironments()
{
    *m_output->environmentReuse = edit(PassID::ReuseEnvironments, [&](ProgramEditor& editor) {
        return Typed::reuseEnvironments(editor, flow(), *m_output->frameAllocation, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::numberLiteralStrings()
{
    *m_output->literalStrings = read(PassID::NumberLiteralStrings, [&] {
        return Typed::numberLiteralStrings(m_program, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::embedConstantCollections()
{
    *m_output->constantCollections = edit(PassID::EmbedConstantCollections, [&](ProgramEditor& editor) {
        return Typed::embedConstantCollections(editor, flow(), m_build);
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

PassOutcome Pipeline::outlineColdOperations()
{
    edit(PassID::OutlineColdOperations, [&](ProgramEditor& editor) {
        Typed::outlineColdOperations(editor, inputsFor(PassID::OutlineColdOperations), m_build);
    });
    didAddFunctions();
    return m_runner.lastProgramPassOutcome();
}

PassOutcome Pipeline::splitFunctionsAtCalls()
{
    edit(PassID::SplitFunctionsAtCalls, [&](ProgramEditor& editor) {
        Typed::splitFunctionsAtCalls(editor, inputsFor(PassID::SplitFunctionsAtCalls), m_build);
    });
    didAddFunctions();
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

PassOutcome Pipeline::numberAndEncode()
{
    PlacementInputs inputs = createPlacementInputs(*this);
    *m_output->placement = edit(PassID::NumberAndEncode, [&](ProgramEditor& editor) {
        return Typed::numberAndEncode(editor, inputs, m_build);
    });
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
    PlacementInputs inputs = createPlacementInputs(*this);
    read(PassID::CheckPlacement, [&] {
        Typed::checkPlacement(m_program, inputs, *m_output->placement, *m_output->siteCaches, m_build);
    });
    return m_runner.lastProgramPassOutcome();
}

#define AOT_FUNCTION_PASS_CASE(id, function) case PassID::id:

PassOutcome Pipeline::runProgramPass(PassID id)
{
    switch (id) {
    case PassID::AdmitAssertedObjects:
        return admitAssertedObjects();
    case PassID::VerifyProgram:
        return verify(m_program.form());
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
    case PassID::DeferClosuresInFields:
        return deferClosuresInFields();
    case PassID::RemoveUnreadFields:
        return removeUnreadFields();
    case PassID::RemoveUnusedResults:
        return removeUnusedResults();
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
    case PassID::CreateEntrypoints:
        return createEntrypoints();
    case PassID::GroupCallsByValue:
        return groupCallsByValue();
    case PassID::ChooseCallingConventions:
        return chooseCallingConventions();
    case PassID::AllocateInFrames:
        return allocateInFrames();
    case PassID::ReuseEnvironments:
        return reuseEnvironments();
    case PassID::NumberLiteralStrings:
        return numberLiteralStrings();
    case PassID::EmbedConstantCollections:
        return embedConstantCollections();
    case PassID::CollectRegExpLiterals:
        return collectRegExpLiterals();
    case PassID::OutlineColdOperations:
        return outlineColdOperations();
    case PassID::SplitFunctionsAtCalls:
        return splitFunctionsAtCalls();
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

void Pipeline::dumpResultOf(PassID id, PrintStream& out) const
{
    const PipelineOutput& output = *m_output;
    switch (id) {
    case PassID::AnalyzeFlow:
        output.flow->dump(out, m_program);
        return;
    case PassID::BuildCallGraph:
        m_analyses.callGraph(flow()).dump(out, m_program);
        return;
    // A run of all stages that was stopped after a pass has handed these two to the output, and a stopped runner computes nothing again.
    case PassID::ComputeEffectSummaries:
        (m_runner.isStopped() ? *output.effects : m_analyses.effects(flow())).dump(out, m_program);
        return;
    case PassID::EstimateHeat:
        (m_runner.isStopped() ? *output.heat : m_analyses.heat(flow())).dump(out, m_program);
        return;
    case PassID::SummarizeForInlining:
        m_analyses.inlining(flow()).dump(out, m_program);
        return;
    case PassID::GroupCallsByValue:
        output.callGroups->dump(out, m_program);
        return;
    case PassID::ChooseCallingConventions:
        output.callingConventions->dump(out, m_program);
        return;
    case PassID::AllocateInFrames:
        output.frameAllocation->dump(out, m_program);
        return;
    case PassID::ReuseEnvironments:
        output.environmentReuse->dump(out, m_program);
        return;
    case PassID::NumberLiteralStrings:
        output.literalStrings->dump(out, m_program);
        return;
    case PassID::EmbedConstantCollections:
        output.constantCollections->dump(out, m_program);
        return;
    case PassID::CollectRegExpLiterals:
        output.regExpRequests->dump(out, m_program);
        return;
    case PassID::ElideStackChecks:
        output.stackChecks->dump(out, m_program);
        return;
    case PassID::PlanSlots:
        output.slots->dump(out, m_program);
        return;
    case PassID::NumberAndEncode:
        output.placement->dump(out, m_program);
        return;
    case PassID::PlaceSiteCaches:
        output.siteCaches->dump(out, m_program);
        return;
    case PassID::None:
    case PassID::AdmitAssertedObjects:
    case PassID::VerifyProgram:
    case PassID::ApplyFlowResults:
    case PassID::CheckLockedBuiltinWrites:
    case PassID::RemoveUnreadExports:
    case PassID::PromoteEnvironmentsToVariables:
    case PassID::DeferDefinitions:
    case PassID::DeferClosuresInFields:
    case PassID::RemoveUnreadFields:
    case PassID::RemoveUnusedResults:
    case PassID::RemoveUnusedParameters:
    case PassID::RemoveUnreachableFunctions:
    case PassID::SplitFreshResults:
    case PassID::PromoteAggregateParameters:
    case PassID::SplitCoroutines:
    case PassID::CreateEntrypoints:
    case PassID::OutlineColdOperations:
    case PassID::SplitFunctionsAtCalls:
    case PassID::CheckPlacement:
    case PassID::GenerateCode:
    FOR_EACH_AOT_FUNCTION_PASS(AOT_FUNCTION_PASS_CASE)
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#undef AOT_FUNCTION_PASS_CASE

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
