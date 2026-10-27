/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPipeline.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTBackgroundTask.h"
#include "AOTBorrowedParameters.h"
#include "AOTBuildContext.h"
#include "AOTCallGraph.h"
#include "AOTCallModules.h"
#include "AOTCheckTypesAtEntry.h"
#include "AOTChooseCallingConventions.h"
#include "AOTCopyClosureFactories.h"
#include "AOTCoroutines.h"
#include "AOTCreateEntrypoints.h"
#include "AOTCreatedObjectLayouts.h"
#include "AOTDeferClosuresInFields.h"
#include "AOTDeferDefinitions.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsCheck.h"
#include "AOTEffectsDeclared.h"
#include "AOTElideStackChecks.h"
#include "AOTEmbedConstantCollections.h"
#include "AOTExpectFieldsByNames.h"
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
#include "AOTOpcode.h"
#include "AOTOutlineColdOperations.h"
#include "AOTOutliningCandidates.h"
#include "AOTPass.h"
#include "AOTPassRunner.h"
#include "AOTPhaseTimes.h"
#include "AOTPipelineInternal.h"
#include "AOTPlacement.h"
#include "AOTPredictedValues.h"
#include "AOTProgramPasses.h"
#include "AOTPromoteEnvironmentsToVariables.h"
#include "AOTReadCapturesFromEnvironments.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpRequests.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTRemoveUnreadExports.h"
#include "AOTRemoveUnreadFields.h"
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
#include <array>
#include <wtf/FastMalloc.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

namespace PassTable {

using enum Wire::PassID;
using enum ProgramResult;
using enum PassNecessity;
using enum IRForm;

using Results = OptionSet<ProgramResult>;

struct Forms {
    IRForm first;
    IRForm last;
};

constexpr PassDescriptor programPass(PassID id, PassNecessity necessity, Forms forms, Results inputs, Results outputs, Results invalidatedResults)
{
    return { .id = id, .scope = PassScope::Program, .necessity = necessity, .firstForm = forms.first, .lastForm = forms.last, .inputs = inputs, .outputs = outputs, .invalidatedResults = invalidatedResults };
}

constexpr PassDescriptor functionPass(PassID id, FunctionPass run, PassNecessity necessity, IRForm form, Results inputs, InvalidatedAnalyses invalidatedAnalyses)
{
    return { .id = id, .scope = PassScope::Function, .necessity = necessity, .firstForm = form, .lastForm = form, .inputs = inputs, .invalidatedAnalyses = invalidatedAnalyses, .run = run };
}

constexpr PassDescriptor producing(IRForm form, PassDescriptor descriptor)
{
    descriptor.produces = form;
    return descriptor;
}

constexpr PassDescriptor addingOnly(std::initializer_list<Opcode> operations, PassDescriptor descriptor)
{
    descriptor.editedFunctions = EditedFunctions::DoNoMore;
    for (Opcode opcode : operations)
        descriptor.effectsAddedByEdits.merge(declaredBoundOf(opcode));
    return descriptor;
}

constexpr PassDescriptor takingAway(PassDescriptor descriptor)
{
    descriptor.editedFunctions = EditedFunctions::DoLess;
    return descriptor;
}

template<bool PassDescriptor::* trait>
constexpr PassDescriptor having(PassDescriptor descriptor)
{
    descriptor.*trait = true;
    return descriptor;
}

constexpr auto readingOtherBodies = having<&PassDescriptor::readsOtherBodies>;
constexpr auto derivingFunctions = having<&PassDescriptor::derivesFunctions>;
constexpr auto countingOwnChanges = having<&PassDescriptor::countsOwnChanges>;
constexpr auto verifiedAfterNextPass = having<&PassDescriptor::isVerifiedAfterNextPass>;

constexpr InvalidatedAnalyses nothing = InvalidatedAnalyses::None;
constexpr InvalidatedAnalyses values = InvalidatedAnalyses::Values;
constexpr InvalidatedAnalyses everything = InvalidatedAnalyses::All;

constexpr Results slotInputs { Flow, CallGraph, Heat, FrameAllocation, EnvironmentReuse };
constexpr Results placementInputs { Flow, CallGraph, Heat, CallingConventions, FrameAllocation, EnvironmentReuse, LiteralStrings, ConstantCollections, SlotPlan, RegExpRequests };
constexpr Results codeGenerationInputs { Flow, EffectSummaries, Heat, CallingConventions, FrameAllocation, EnvironmentReuse, LiteralStrings, ConstantCollections, RegExpRequests, StackChecks, Placement, SiteCaches };

constexpr std::array rows {
    verifiedAfterNextPass(programPass(CopyClosureFactories, Optional, { AsRead, AsRead }, { }, { }, { })),
    verifiedAfterNextPass(programPass(AllocateTaggedLayouts, Required, { AsRead, AsRead }, { }, { }, { })),
    verifiedAfterNextPass(programPass(CheckTypesAtEntry, Required, { AsRead, AsRead }, { }, { }, { })),
    programPass(VerifyProgram, Required, { AsRead, Placed }, { }, { }, { }),
    programPass(NarrowFieldTypes, Required, { AsRead, AsRead }, { }, { }, resultsAboutBodies),
    programPass(NameCreatedSymbols, Optional, { AsRead, AsRead }, { }, { }, { }),
    programPass(GuardTaintedPositions, Required, { AsRead, AsRead }, { }, { }, resultsAboutBodies),
    programPass(AnalyzeFlow, Optional, { AsRead, AsRead }, { }, { Flow }, { }),
    producing(Prepared, programPass(ApplyFlowResults, Required, { AsRead, AsRead }, { Flow }, { }, resultsAboutBodies)),
    programPass(CheckLockedBuiltinWrites, Required, { Prepared, Prepared }, { Flow }, { }, { }),
    programPass(BuildCallGraph, Required, { Prepared, Placed }, { Flow }, { CallGraph }, { }),
    programPass(ComputeEffectSummaries, Optional, { Prepared, Placed }, { Flow }, { EffectSummaries }, { }),
    programPass(EstimateHeat, Optional, { Prepared, Placed }, { Flow, CallGraph }, { Heat }, { }),
    programPass(SummarizeForInlining, Optional, { Prepared, Prepared }, { Flow, CallGraph, Heat }, { InliningSummaries }, { }),
    programPass(RemoveUnreadExports, Optional, { Prepared, Prepared }, { Flow }, { }, resultsAboutBodies),
    programPass(PromoteEnvironmentsToVariables, Optional, { Prepared, Prepared }, { Flow }, { }, resultsAboutBodies),
    programPass(DeferDefinitions, Optional, { Prepared, Prepared }, { Flow }, { }, resultsAboutBodies),
    programPass(NarrowConstantVariables, Optional, { Prepared, Prepared }, { Flow }, { }, { }),
    programPass(DeferInitializers, Optional, { Prepared, Prepared }, simplifyInputs, { }, resultsAboutBodies),
    programPass(MarkResultsUsedAsKeys, Optional, { Prepared, Prepared }, { }, { }, { }),
    programPass(ReadCapturesFromEnvironments, Optional, { Prepared, Prepared }, { Flow }, { }, resultsAboutBodies),
    takingAway(programPass(RemoveUnreadFields, Optional, { Prepared, Prepared }, simplifyInputs, { }, resultsAboutBodies)),
    addingOnly({ }, programPass(RemoveUnusedResults, Optional, { Prepared, Prepared }, simplifyInputs | CallGraph, { }, resultsAboutBodies)),
    addingOnly({ }, programPass(ProveResults, Optional, { Prepared, Prepared }, simplifyInputs | CallGraph, { }, resultsAboutBodies)),
    addingOnly({ }, programPass(RemoveUnusedParameters, Optional, { Prepared, Prepared }, simplifyInputs | CallGraph, { }, resultsAboutBodies)),
    programPass(RemoveUnreachableFunctions, Optional, { Prepared, Selected }, { CallGraph }, { }, { }),
    programPass(SplitBehindEntryChecks, Optional, { Prepared, Prepared }, { Flow, CallGraph, Heat }, { }, resultsAboutBodies | Results { Heat }),
    addingOnly({ }, programPass(SkipAnsweredEntryChecks, Optional, { Prepared, Prepared }, { Heat }, { }, resultsAboutBodies)),
    addingOnly({ }, programPass(SplitFreshResults, Optional, { Prepared, Prepared }, simplifyInputs | Results { CallGraph, Heat }, { }, resultsAboutBodies)),
    programPass(PromoteAggregateParameters, Optional, { Prepared, Prepared }, { Flow, EffectSummaries }, { }, resultsAboutBodies),
    programPass(SplitCoroutines, Required, { Prepared, Prepared }, simplifyInputs, { }, resultsAboutBodies),
    programPass(CallModules, Required, { Prepared, Prepared }, { Flow }, { }, resultsAboutBodies | Results { Heat }),
    countingOwnChanges(programPass(CreateEntrypoints, Required, { Prepared, Prepared }, { Flow }, { }, resultsAboutBodies)),
    addingOnly({ }, countingOwnChanges(programPass(ChooseCallingConventions, Optional, { Prepared, Prepared }, simplifyInputs | Results { CallGraph }, { CallingConventions }, resultsAboutBodies))),
    addingOnly({ Opcode::GetDeferredClosureSlot, Opcode::GetDeferredClosureSlotOrUndefined }, programPass(DeferClosuresInFields, Optional, { Prepared, Prepared }, { Heat }, { }, resultsAboutBodies)),
    addingOnly({ Opcode::MaterializeEnvironment, Opcode::EmptyArgumentList }, programPass(AllocateInFrames, Optional, { Prepared, Prepared }, { Flow, CallGraph, Heat, EffectSummaries }, { FrameAllocation }, { EffectSummaries })),
    addingOnly({ Opcode::ReleaseEnvironment, Opcode::MarkEnvironmentEscaped }, programPass(ReuseEnvironments, Optional, { Prepared, Prepared }, { Flow, FrameAllocation }, { EnvironmentReuse }, { EffectSummaries })),
    programPass(NumberLiteralStrings, Required, { Prepared, Placed }, { }, { LiteralStrings }, { }),
    addingOnly({ Opcode::Data, Opcode::NewSetFromData, Opcode::NewMapFromData }, programPass(EmbedConstantCollections, Optional, { Prepared, Prepared }, { Flow }, { ConstantCollections }, { })),
    countingOwnChanges(programPass(CollectRegExpLiterals, Required, { Selected, Selected }, { Flow }, { RegExpRequests }, { })),
    programPass(OutlineColdOperations, Optional, { Selected, Selected }, { Flow, Heat, EffectSummaries, CallingConventions }, { }, resultsAboutBodies),
    programPass(InlineHeads, Optional, { Selected, Selected }, { Flow, Heat, EffectSummaries }, { }, resultsAboutBodies),
    programPass(ElideStackChecks, Optional, { Selected, Selected }, { CallGraph }, { StackChecks }, { }),
    programPass(PlanSlots, Required, { Prepared, Selected }, slotInputs, { SlotPlan }, { }),
    producing(Placed, programPass(NumberAndEncode, Required, { Selected, Selected }, placementInputs, { Placement }, { })),
    programPass(PlaceSiteCaches, Required, { Placed, Placed }, { Heat, Placement }, { SiteCaches }, { }),
    programPass(CheckPlacement, Required, { Placed, Placed }, placementInputs | Results { Placement, SiteCaches }, { }, { }),
    programPass(GenerateCode, Required, { Placed, Placed }, codeGenerationInputs, { }, { }),

    verifiedAfterNextPass(functionPass(ListLiveValuesAtSuspensions, listLiveValuesAtSuspensions, Required, AsRead, { }, values)),
    verifiedAfterNextPass(functionPass(TypeValues, typeValues, Required, AsRead, { }, everything)),
    verifiedAfterNextPass(functionPass(CheckInt32Promises, checkInt32Promises, Required, AsRead, { }, values)),
    verifiedAfterNextPass(functionPass(CheckRegExpLiterals, checkRegExpLiterals, Required, AsRead, { }, everything)),

    functionPass(AddGenericVersion, addGenericVersion, Required, Prepared, { Flow, Heat, EffectSummaries }, everything),
    functionPass(ResolveBuiltins, resolveBuiltins, Required, Prepared, { Flow, EffectSummaries }, everything),
    functionPass(BindGlobalNames, bindGlobalNames, Optional, Prepared, { Flow }, values),
    derivingFunctions(readingOtherBodies(functionPass(CallThroughMethodTable, callThroughMethodTable, Optional, Prepared, { Flow, Heat, EffectSummaries }, everything))),
    derivingFunctions(readingOtherBodies(functionPass(ResolveCalls, resolveCalls, Optional, Prepared, { Flow, Heat, EffectSummaries }, everything))),
    functionPass(ResolveAccesses, resolveAccesses, Optional, Prepared, { Flow, EffectSummaries }, everything),
    functionPass(SplitUnions, splitUnions, Optional, Prepared, { Flow, Heat, EffectSummaries }, everything),
    functionPass(PlaceTypeTests, placeTypeTests, Optional, Prepared, { Flow, Heat, EffectSummaries }, everything),
    derivingFunctions(functionPass(ExpandIteration, expandIteration, Required, Prepared, { Flow, EffectSummaries }, everything)),
    functionPass(ExpandProvenIterations, expandProvenIterations, Optional, Prepared, { Flow, EffectSummaries }, everything),
    derivingFunctions(functionPass(ExpandOwnKeys, expandOwnKeys, Optional, Prepared, { Flow, Heat, EffectSummaries }, everything)),
    derivingFunctions(readingOtherBodies(functionPass(SelectTypedResults, selectTypedResults, Required, Prepared, { Flow, Heat, EffectSummaries }, everything))),
    functionPass(RetypeValues, retypeValues, Required, Prepared, { }, nothing),
    derivingFunctions(readingOtherBodies(functionPass(InlineCalls, inlineCalls, Optional, Prepared, { Flow, CallGraph, Heat, InliningSummaries, EffectSummaries }, everything))),
    functionPass(SimplifyCasts, simplifyCasts, Optional, Prepared, { }, everything),
    functionPass(SinkAllocations, sinkAllocations, Optional, Prepared, { Flow, EffectSummaries }, everything),
    functionPass(EliminateRedundantReads, eliminateRedundantReads, Optional, Prepared, { Flow, EffectSummaries }, everything),
    functionPass(EliminateGuards, eliminateGuards, Optional, Prepared, { Flow, EffectSummaries }, everything),
    functionPass(SeparateUnlikelyLoopPaths, separateUnlikelyLoopPaths, Optional, Prepared, { Flow, Heat, EffectSummaries }, everything),
    functionPass(ConvertCountersToIntegers, convertCountersToIntegers, Optional, Prepared, { }, everything),
    functionPass(RemoveDeadCountedLoops, removeDeadCountedLoops, Optional, Prepared, { EffectSummaries }, everything),
    functionPass(UnrollShortLoops, unrollShortLoops, Optional, Prepared, { Heat }, everything),
    derivingFunctions(functionPass(FuseArrayChains, fuseArrayChains, Optional, Prepared, { Flow, Heat, EffectSummaries }, everything)),
    readingOtherBodies(functionPass(FuseGeneratorLoops, fuseGeneratorLoops, Optional, Prepared, { Flow, EffectSummaries }, everything)),
    readingOtherBodies(functionPass(StartGeneratorsAtFirstStep, startGeneratorsAtFirstStep, Optional, Prepared, { Flow, EffectSummaries }, everything)),
    functionPass(CreateStringBuilders, createStringBuilders, Optional, Prepared, { Heat, EffectSummaries }, everything),
    functionPass(FuseConcatenations, fuseConcatenations, Optional, Prepared, { }, everything),
    functionPass(ExpandData, expandData, Required, Prepared, { Flow }, values),
    functionPass(FoldConstants, foldConstants, Optional, Prepared, { Flow, EffectSummaries }, values),
    functionPass(SimplifyControlFlow, simplifyControlFlow, Optional, Prepared, { Flow, EffectSummaries }, everything),
    functionPass(RemoveTrivialParameters, removeTrivialParameters, Optional, Prepared, { }, values),
    functionPass(PropagateBranchConditions, propagateBranchConditions, Optional, Prepared, { }, values),
    functionPass(HoistTestsIntoPredecessors, hoistTestsIntoPredecessors, Optional, Prepared, { }, values),
    functionPass(TestNarrowerTypesFirst, testNarrowerTypesFirst, Optional, Prepared, { }, everything),
    functionPass(ThreadJumps, threadJumps, Optional, Prepared, { EffectSummaries }, everything),
    functionPass(EliminateDeadCode, eliminateDeadCode, Optional, Prepared, { Flow, EffectSummaries }, values),
    functionPass(CompactBlocks, compactBlocks, Required, Prepared, { }, everything),
    functionPass(RemoveRedundantHandlers, removeRedundantHandlers, Optional, Prepared, { EffectSummaries }, everything),
    functionPass(HoistLoopInvariantReads, hoistLoopInvariantReads, Optional, Prepared, { Flow, EffectSummaries }, everything),
    functionPass(SimplifyBuiltinCalls, simplifyBuiltinCalls, Optional, Prepared, { EffectSummaries }, everything),
    functionPass(ReduceIntegerDivisions, reduceIntegerDivisions, Optional, Prepared, { }, values),
    functionPass(PlanArrayCapacities, planArrayCapacities, Optional, Prepared, { Flow, Heat, EffectSummaries }, everything),
    functionPass(PlanCollectionCapacities, planCollectionCapacities, Optional, Prepared, { Flow, EffectSummaries }, everything),
    functionPass(FoldConstantRegExps, foldConstantRegExps, Optional, Prepared, { Flow }, everything),
    functionPass(ShareRegExpLiterals, shareRegExpLiterals, Optional, Prepared, { Flow }, values),
    functionPass(InsertObjectCoercibleChecks, insertObjectCoercibleChecks, Optional, Prepared, { }, everything),
    functionPass(EliminateTailRecursion, eliminateTailRecursion, Optional, Prepared, { }, everything),
    readingOtherBodies(functionPass(ResolveAwaitedCalls, resolveAwaitedCalls, Optional, Prepared, { EffectSummaries }, everything)),
    functionPass(SinkConstants, sinkConstants, Optional, Prepared, { }, values),
    functionPass(ExpandLongArrayLiterals, expandLongArrayLiterals, Required, Prepared, { }, everything),
    functionPass(FinishBirths, finishBirths, Optional, Prepared, { }, values),
    readingOtherBodies(functionPass(RestartInGenericVersion, restartInGenericVersion, Optional, Prepared, { Flow, EffectSummaries }, everything)),

    producing(Selected, functionPass(Select, select, Required, Prepared, { Flow, Heat, EffectSummaries, CallingConventions, SlotPlan }, everything)),
    functionPass(ChooseRepresentations, chooseRepresentations, Required, Placed, { CallingConventions, Placement }, everything),
    functionPass(RemoveRedundantBarriers, removeRedundantBarriers, Optional, Placed, { EffectSummaries, CallingConventions, Placement }, nothing),
};

constexpr std::array typeSteps {
    ScheduleStep::run(ListLiveValuesAtSuspensions),
    ScheduleStep::run(TypeValues),
    ScheduleStep::run(CheckInt32Promises),
    ScheduleStep::run(CheckRegExpLiterals),
};

constexpr std::array simplifySteps {
    ScheduleStep::beginFixpoint(),
        ScheduleStep::beginFixpoint(),
            ScheduleStep::run(FoldConstants),
            ScheduleStep::run(SimplifyControlFlow),
            ScheduleStep::run(RemoveTrivialParameters),
            ScheduleStep::run(PropagateBranchConditions),
            ScheduleStep::run(HoistTestsIntoPredecessors),
            ScheduleStep::run(ThreadJumps),
            ScheduleStep::run(EliminateDeadCode),
        ScheduleStep::endFixpoint(),
        ScheduleStep::run(EliminateRedundantReads),
        ScheduleStep::run(RetypeValues),
    ScheduleStep::endFixpoint(),
    ScheduleStep::run(RetypeValues),
    ScheduleStep::run(CompactBlocks),
};

constexpr std::array prepareSteps {
    ScheduleStep::run(SimplifyCasts),
    ScheduleStep::run(InsertObjectCoercibleChecks),
    ScheduleStep::run(AddGenericVersion),
    ScheduleStep::run(ExpandProvenIterations),
    ScheduleStep::run(ResolveBuiltins),
    ScheduleStep::run(BindGlobalNames),
    ScheduleStep::run(SimplifyBuiltinCalls),
    ScheduleStep::run(ExpandData),
    ScheduleStep::run(InsertObjectCoercibleChecks),
    ScheduleStep::run(RemoveRedundantHandlers),
    ScheduleStep::include(FunctionSchedule::Simplify),
};

// InlineCalls pastes a body as it stands when the Optimize schedule begins.
constexpr std::array resolveSteps {
    ScheduleStep::run(ResolveAccesses),
};
constexpr std::array optimizeSteps {
    ScheduleStep::beginFixpoint(),
        ScheduleStep::run(ResolveBuiltins),
        ScheduleStep::run(BindGlobalNames),
        ScheduleStep::run(SimplifyBuiltinCalls),
        ScheduleStep::run(CallThroughMethodTable),
        ScheduleStep::run(ResolveCalls),
        ScheduleStep::run(ResolveAccesses),
        ScheduleStep::run(HoistLoopInvariantReads),
        ScheduleStep::run(SplitUnions),
        ScheduleStep::run(PlaceTypeTests),
        ScheduleStep::run(ExpandIteration),
        ScheduleStep::run(ExpandOwnKeys),
        ScheduleStep::run(SelectTypedResults),
        ScheduleStep::run(ResolveAwaitedCalls),
        ScheduleStep::run(FuseArrayChains),
        ScheduleStep::run(InlineCalls),
        ScheduleStep::run(EliminateTailRecursion),
        ScheduleStep::run(FuseGeneratorLoops),
        ScheduleStep::run(SimplifyCasts),
        ScheduleStep::run(RetypeValues),
        ScheduleStep::run(InsertObjectCoercibleChecks),
        ScheduleStep::run(RemoveRedundantHandlers),
        ScheduleStep::includeWithoutCompacting(FunctionSchedule::Simplify),
        ScheduleStep::run(SinkAllocations),
        ScheduleStep::run(UnrollShortLoops),
    ScheduleStep::endFixpoint(),
    ScheduleStep::run(ExpandIteration),
    ScheduleStep::run(SelectTypedResults),
    // What SelectTypedResults has put in calls helpers of the compiler, which are in no realm. Where the rounds above have ended, as they do after one for a generic version, nobody else makes direct calls of them.
    ScheduleStep::run(ResolveBuiltins),
    ScheduleStep::includeWithoutCompacting(FunctionSchedule::Simplify),
    ScheduleStep::run(RestartInGenericVersion),
    // Where generic code has gone, what it joined stands in one block with the test behind it.
    ScheduleStep::includeWithoutCompacting(FunctionSchedule::Simplify),
    ScheduleStep::run(RestartInGenericVersion),
    ScheduleStep::run(StartGeneratorsAtFirstStep),
    ScheduleStep::run(SeparateUnlikelyLoopPaths),
    ScheduleStep::run(ConvertCountersToIntegers),
    ScheduleStep::run(SimplifyCasts),
    ScheduleStep::run(SimplifyBuiltinCalls),
    ScheduleStep::run(ReduceIntegerDivisions),
    ScheduleStep::run(FoldConstantRegExps),
    ScheduleStep::run(ShareRegExpLiterals),
    ScheduleStep::run(FuseConcatenations),
    ScheduleStep::run(CreateStringBuilders),
    ScheduleStep::includeWithoutCompacting(FunctionSchedule::Simplify),
    ScheduleStep::beginFixpoint(LimitID::MaxSinkingRounds),
        ScheduleStep::run(SinkAllocations),
        ScheduleStep::includeWithoutCompacting(FunctionSchedule::Simplify),
    ScheduleStep::endFixpoint(),
    // The tests of a cold function of a very large program. Nothing is inlined, expanded or sunk any more.
    ScheduleStep::runBehindRounds(PlaceTypeTests),
    ScheduleStep::run(ResolveCalls),
    ScheduleStep::run(ResolveAccesses),
    // Where the rounds above were cut short by their limit, ResolveCalls still finds calls to resolve here, and makes of a call with a spread what only ExpandIteration removes.
    ScheduleStep::run(ExpandIteration),
    ScheduleStep::includeWithoutCompacting(FunctionSchedule::Simplify),
    ScheduleStep::run(RestartInGenericVersion),
    ScheduleStep::run(RemoveDeadCountedLoops),
    ScheduleStep::run(HoistLoopInvariantReads),
    ScheduleStep::run(EliminateRedundantReads),
    ScheduleStep::run(EliminateGuards),
    ScheduleStep::run(TestNarrowerTypesFirst),
    ScheduleStep::run(SinkConstants),
    ScheduleStep::run(PlanArrayCapacities),
    ScheduleStep::run(PlanCollectionCapacities),
    ScheduleStep::run(ExpandLongArrayLiterals),
    ScheduleStep::run(RetypeValues),
    ScheduleStep::run(CompactBlocks),
};

// What finishes a birth writes nothing that the program can see, so whoever cleans up takes it for dead. The passes that take fields, results and parameters away simplify the bodies they have changed.
constexpr std::array selectSteps {
    ScheduleStep::run(FinishBirths),
    ScheduleStep::run(Select),
};

constexpr std::array representSteps {
    ScheduleStep::run(ChooseRepresentations),
    ScheduleStep::run(RemoveRedundantBarriers),
};

constexpr unsigned indexOf(PassID id) { return static_cast<unsigned>(id); }
constexpr unsigned indexOf(FunctionSchedule schedule) { return static_cast<unsigned>(schedule); }

template<typename T> using ByPass = std::array<T, numberOfPasses>;

constexpr ByPass<PassDescriptor> descriptors = [] {
    ByPass<PassDescriptor> result { };
    for (const PassDescriptor& row : rows)
        result[indexOf(row.id)] = row;
    return result;
}();

constexpr const PassDescriptor& descriptorAt(PassID id) { return descriptors[indexOf(id)]; }

constexpr PassSet functionPassesExcept(PassID excluded)
{
    PassSet passes;
    for (const PassDescriptor& row : rows) {
        if (row.scope == PassScope::Function && row.id != excluded)
            passes.add(row.id);
    }
    return passes;
}

constexpr PassSet functionPasses = functionPassesExcept(None);

struct PassesLeftIdle {
    PassID changer;
    PassSet passes;
};

constexpr PassesLeftIdle passesLeftIdle[] {
    { CompactBlocks, functionPassesExcept(RetypeValues) },
    { EliminateDeadCode, { RetypeValues, RemoveTrivialParameters } },
    { HoistTestsIntoPredecessors, { HoistTestsIntoPredecessors } },
};

constexpr PassesLeftIdle passesThatCanWait[] {
    { CreateStringBuilders, { PlaceTypeTests } },
    { EliminateDeadCode, { ResolveBuiltins, ResolveCalls, ResolveAccesses, PlaceTypeTests, InlineCalls, SimplifyCasts, FoldConstants, PropagateBranchConditions, HoistTestsIntoPredecessors, ThreadJumps, EliminateDeadCode, EliminateRedundantReads } },
    { EliminateRedundantReads, { RemoveTrivialParameters, PropagateBranchConditions, HoistTestsIntoPredecessors, ThreadJumps, EliminateRedundantReads } },
    { FoldConstants, { RemoveTrivialParameters, EliminateRedundantReads } },
    { FuseConcatenations, { EliminateRedundantReads } },
    { PlanArrayCapacities, { RetypeValues } },
    { PlanCollectionCapacities, { RetypeValues } },
    { RemoveRedundantHandlers, { ResolveBuiltins, EliminateRedundantReads } },
    { RemoveTrivialParameters, { PlaceTypeTests, SimplifyCasts, SinkAllocations, RemoveTrivialParameters, PropagateBranchConditions, EliminateRedundantReads } },
    { ResolveBuiltins, { SimplifyControlFlow, RemoveTrivialParameters, EliminateRedundantReads } },
    { RetypeValues, { SimplifyCasts, RemoveTrivialParameters, PropagateBranchConditions, HoistTestsIntoPredecessors, ThreadJumps, EliminateRedundantReads } },
    { ShareRegExpLiterals, { EliminateRedundantReads } },
    { SimplifyBuiltinCalls, { SimplifyControlFlow, EliminateRedundantReads } },
    { SimplifyCasts, { ResolveBuiltins, SimplifyBuiltinCalls, ResolveCalls, ResolveAccesses, PlaceTypeTests, InlineCalls, SimplifyCasts, SinkAllocations, FoldConstants, SimplifyControlFlow, EliminateDeadCode, EliminateRedundantReads } },
    { SimplifyControlFlow, { ResolveBuiltins, ResolveCalls, InlineCalls, SimplifyCasts, SinkAllocations, SimplifyControlFlow, RemoveTrivialParameters, PropagateBranchConditions, EliminateRedundantReads } },
    { SinkAllocations, { EliminateRedundantReads } },
    { SinkConstants, { RetypeValues } },
    { ThreadJumps, { ResolveBuiltins, FoldConstants, PropagateBranchConditions, EliminateRedundantReads } },
};

constexpr ByPass<PassSet> passesThatCanWaitByChanger = [] {
    ByPass<PassSet> result { };
    for (const PassesLeftIdle& row : passesThatCanWait)
        result[indexOf(row.changer)] = row.passes;
    return result;
}();

constexpr ByPass<PassSet> enablingPasses = [] {
    ByPass<PassSet> changersWithoutEffect { };
    for (const PassesLeftIdle& row : passesLeftIdle) {
        row.passes.forEach([&](PassID pass) {
            changersWithoutEffect[indexOf(pass)].add(row.changer);
        });
    }
    ByPass<PassSet> result { };
    for (PassID pass : allMembersOf<PassID>) {
        for (PassID changer : allMembersOf<PassID>) {
            if (!changersWithoutEffect[indexOf(pass)].contains(changer))
                result[indexOf(pass)].add(changer);
        }
    }
    return result;
}();

constexpr bool namesFunctionPasses(const PassesLeftIdle& row)
{
    return functionPasses.contains(row.changer) && (row.passes | functionPasses) == functionPasses;
}

constexpr std::span<const ScheduleStep> stepsIn(FunctionSchedule schedule)
{
    switch (schedule) {
    case FunctionSchedule::Type:
        return typeSteps;
    case FunctionSchedule::Simplify:
        return simplifySteps;
    case FunctionSchedule::Prepare:
        return prepareSteps;
    case FunctionSchedule::Resolve:
        return resolveSteps;
    case FunctionSchedule::Optimize:
        return optimizeSteps;
    case FunctionSchedule::Select:
        return selectSteps;
    case FunctionSchedule::Represent:
        return representSteps;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr Results computeInputs(FunctionSchedule schedule)
{
    Results inputs;
    for (auto& step : stepsIn(schedule)) {
        switch (step.kind) {
        case ScheduleStep::Kind::RunPass:
            inputs |= descriptorAt(step.pass).inputs;
            break;
        case ScheduleStep::Kind::RunSchedule:
            inputs |= computeInputs(step.schedule);
            break;
        case ScheduleStep::Kind::BeginFixpoint:
        case ScheduleStep::Kind::EndFixpoint:
            break;
        }
    }
    return inputs;
}

constexpr ScheduleDescriptor schedule(FunctionSchedule id, ASCIILiteral name, ScheduleMode mode, ScheduledFunctions functions, IRForm form)
{
    return { id, name, mode, functions, form, stepsIn(id), functions == ScheduledFunctions::Reached ? computeInputs(id) | Results { CallGraph } : computeInputs(id) };
}

constexpr std::array schedules {
    schedule(FunctionSchedule::Type, "Type"_s, ScheduleMode::InPlace, ScheduledFunctions::All, AsRead),
    schedule(FunctionSchedule::Simplify, "Simplify"_s, ScheduleMode::InPlace, ScheduledFunctions::OneAtATime, Prepared),
    schedule(FunctionSchedule::Prepare, "Prepare"_s, ScheduleMode::OnCopies, ScheduledFunctions::All, Prepared),
    schedule(FunctionSchedule::Resolve, "Resolve"_s, ScheduleMode::InPlace, ScheduledFunctions::All, Prepared),
    schedule(FunctionSchedule::Optimize, "Optimize"_s, ScheduleMode::OnCopies, ScheduledFunctions::Reached, Prepared),
    schedule(FunctionSchedule::Select, "Select"_s, ScheduleMode::InPlace, ScheduledFunctions::All, Prepared),
    schedule(FunctionSchedule::Represent, "Represent"_s, ScheduleMode::InPlace, ScheduledFunctions::All, Placed),
};

constexpr const ScheduleDescriptor& scheduleAt(FunctionSchedule id) { return schedules[indexOf(id)]; }

constexpr bool everyPassHasOneRow()
{
    ByPass<unsigned> numberOfRows { };
    for (const PassDescriptor& row : rows)
        ++numberOfRows[indexOf(row.id)];
    for (unsigned index = 0; index < numberOfPasses; ++index) {
        if (numberOfRows[index] != (index == indexOf(None) ? 0u : 1u))
            return false;
    }
    return true;
}

constexpr bool hasValidProducedForm(const PassDescriptor& descriptor)
{
    bool isFormProducer = descriptor.id == ApplyFlowResults || descriptor.id == Select || descriptor.id == NumberAndEncode;
    if (descriptor.produces.has_value() != isFormProducer)
        return false;
    return !isFormProducer || descriptor.lastForm < *descriptor.produces;
}

constexpr bool canObtainInputs(const PassDescriptor& consumer)
{
    return std::ranges::all_of(rows, [&](const PassDescriptor& producer) {
        if (!consumer.inputs.containsAny(producer.outputs))
            return true;
        if (consumer.firstForm < producer.firstForm)
            return false;
        return !resultsComputedOnDemand.containsAny(producer.outputs) || consumer.lastForm <= producer.lastForm;
    });
}

constexpr bool listOfFunctionPassesMatchesTable()
{
    PassSet listed;
#define AOT_CHECK_FUNCTION_PASS(id, function) \
    if (descriptorAt(PassID::id).run != function) \
        return false; \
    listed.add(PassID::id);
    FOR_EACH_AOT_FUNCTION_PASS(AOT_CHECK_FUNCTION_PASS)
#undef AOT_CHECK_FUNCTION_PASS
    return listed == functionPasses;
}

constexpr bool resultsHaveOneProducer()
{
    Results produced;
    Results used;
    for (auto& row : rows) {
        if (produced.containsAny(row.outputs))
            return false;
        produced |= row.outputs;
        used |= row.inputs | row.invalidatedResults;
    }
    return produced.containsAll(used);
}

constexpr bool hasBalancedFixpoints(const ScheduleDescriptor& descriptor)
{
    unsigned depth = 0;
    for (auto& step : descriptor.steps) {
        switch (step.kind) {
        case ScheduleStep::Kind::BeginFixpoint:
            if (step.limit == LimitID::None)
                return false;
            ++depth;
            break;
        case ScheduleStep::Kind::EndFixpoint:
            if (!depth)
                return false;
            --depth;
            break;
        case ScheduleStep::Kind::RunPass:
        case ScheduleStep::Kind::RunSchedule:
            break;
        }
    }
    return !depth;
}

constexpr bool allPassesIn(const ScheduleDescriptor& descriptor, const Invocable<bool(const PassDescriptor&)> auto& predicate)
{
    return std::ranges::all_of(descriptor.steps, [&](const ScheduleStep& step) {
        return step.kind != ScheduleStep::Kind::RunPass || predicate(descriptorAt(step.pass));
    });
}

constexpr bool runsFunctionPassesOfDeclaredForm(const ScheduleDescriptor& descriptor)
{
    return allPassesIn(descriptor, [&](const PassDescriptor& pass) {
        return pass.scope == PassScope::Function && pass.firstForm == descriptor.form && pass.lastForm == descriptor.form;
    });
}

constexpr bool isScheduled(const PassDescriptor& row)
{
    return row.scope == PassScope::Program || std::ranges::any_of(schedules, [&](const ScheduleDescriptor& descriptor) {
        return !allPassesIn(descriptor, [&](const PassDescriptor& pass) {
            return pass.id != row.id;
        });
    });
}

constexpr bool includesOnlySimplify(const ScheduleDescriptor& descriptor)
{
    return std::ranges::all_of(descriptor.steps, [&](const ScheduleStep& step) {
        return step.kind != ScheduleStep::Kind::RunSchedule || (step.schedule == FunctionSchedule::Simplify && descriptor.id != FunctionSchedule::Simplify);
    });
}

constexpr bool isCompaction(const ScheduleStep& step) { return step.kind == ScheduleStep::Kind::RunPass && step.pass == CompactBlocks; }

constexpr bool compactsBehindIncludedSchedules(const ScheduleDescriptor& descriptor)
{
    return std::ranges::all_of(descriptor.steps, [&](const ScheduleStep& step) {
        return step.compacts || (isCompaction(scheduleAt(step.schedule).steps.back()) && isCompaction(descriptor.steps.back()));
    });
}

constexpr bool runsPassAfterUnverifiedPass(const ScheduleDescriptor& descriptor)
{
    for (size_t index = 0; index < descriptor.steps.size(); ++index) {
        const ScheduleStep& step = descriptor.steps[index];
        if (step.kind != ScheduleStep::Kind::RunPass || !descriptorAt(step.pass).isVerifiedAfterNextPass)
            continue;
        if (index + 1 == descriptor.steps.size()) {
            if (descriptor.id != FunctionSchedule::Type || descriptorAt(NarrowFieldTypes).necessity != Required)
                return false;
            continue;
        }
        if (descriptor.steps[index + 1].kind != ScheduleStep::Kind::RunPass)
            return false;
        if (descriptorAt(descriptor.steps[index + 1].pass).necessity != Required)
            return false;
    }
    return true;
}

constexpr bool isLocalToFunction(const PassDescriptor& pass) { return !pass.readsOtherBodies && !pass.derivesFunctions; }

constexpr bool modeFitsPasses(const ScheduleDescriptor& descriptor)
{
    if (descriptor.mode == ScheduleMode::OnCopies)
        return true;
    return descriptor.functions != ScheduledFunctions::Reached && allPassesIn(descriptor, isLocalToFunction);
}

// runOnCopies() scans a finished copy for function references only when the schedule is Reached.
constexpr bool schedulesDerivedFunctions(const ScheduleDescriptor& descriptor)
{
    return descriptor.functions == ScheduledFunctions::Reached || allPassesIn(descriptor, [](const PassDescriptor& pass) {
        return !pass.derivesFunctions;
    });
}

enum class LastRun : uint8_t { None, InsideFixpoint, OutsideFixpoints };

constexpr LastRun findLastRun(FunctionSchedule schedule, PassID id)
{
    LastRun lastRun = LastRun::None;
    unsigned depth = 0;
    for (auto& step : stepsIn(schedule)) {
        switch (step.kind) {
        case ScheduleStep::Kind::BeginFixpoint:
            ++depth;
            break;
        case ScheduleStep::Kind::EndFixpoint:
            --depth;
            break;
        case ScheduleStep::Kind::RunPass:
            if (step.pass == id)
                lastRun = depth ? LastRun::InsideFixpoint : LastRun::OutsideFixpoints;
            break;
        case ScheduleStep::Kind::RunSchedule:
            if (LastRun included = findLastRun(step.schedule, id); included != LastRun::None)
                lastRun = depth ? LastRun::InsideFixpoint : included;
            break;
        }
    }
    return lastRun;
}

constexpr bool runsUnconditionally(PassID id)
{
    if (descriptorAt(id).necessity != Required)
        return false;
    return std::ranges::none_of(schedules, [&](const ScheduleDescriptor& descriptor) {
        return findLastRun(descriptor.id, id) == LastRun::InsideFixpoint;
    });
}

// Only the front end makes what such a pass removes, so none is left once Prepare has taken every function.
constexpr bool runsOnEveryFunctionInPrepare(PassID id)
{
    return descriptorAt(id).necessity == Required
        && scheduleAt(FunctionSchedule::Prepare).functions == ScheduledFunctions::All
        && findLastRun(FunctionSchedule::Prepare, id) == LastRun::OutsideFixpoints;
}

constexpr bool removingPassesRunUnconditionally()
{
    auto isValid = [](PassID id) {
        return id == None || runsUnconditionally(id) || runsOnEveryFunctionInPrepare(id);
    };
    return std::ranges::all_of(allMembersOf<Opcode>, [&](Opcode opcode) { return isValid(removedByOf(opcode)); })
        && std::ranges::all_of(allMembersOf<TerminatorKind>, [&](TerminatorKind kind) { return isValid(removedByOf(kind)); });
}

constexpr bool canRunSimplify(PassID id)
{
    const PassDescriptor& pass = descriptorAt(id);
    const ScheduleDescriptor& simplify = scheduleAt(FunctionSchedule::Simplify);
    return pass.inputs.containsAll(simplify.inputs) && pass.firstForm == simplify.form && pass.lastForm == simplify.form;
}

constexpr bool canRunFunctionPass(PassID id, PassID nestedID)
{
    const PassDescriptor& pass = descriptorAt(id);
    const PassDescriptor& nested = descriptorAt(nestedID);
    return nested.scope == PassScope::Function && isLocalToFunction(nested) && pass.inputs.containsAll(nested.inputs)
        && nested.firstForm <= pass.firstForm && pass.lastForm <= nested.lastForm;
}

static_assert(everyPassHasOneRow());
static_assert(std::ranges::all_of(rows, [](const PassDescriptor& row) { return (row.scope == PassScope::Function) == !!row.run; }));
static_assert(std::ranges::all_of(rows, [](const PassDescriptor& row) { return row.firstForm <= row.lastForm; }));
static_assert(std::ranges::all_of(rows, hasValidProducedForm));
static_assert(std::ranges::all_of(rows, canObtainInputs));
static_assert(listOfFunctionPassesMatchesTable());
static_assert(resultsHaveOneProducer());
static_assert(std::ranges::all_of(passesLeftIdle, namesFunctionPasses));
static_assert(std::ranges::all_of(passesThatCanWait, namesFunctionPasses));
static_assert(std::ranges::all_of(enablingPasses, [](const PassSet& passes) { return passes.contains(None); }));
static_assert(!descriptorAt(ChooseRepresentations).inputs.contains(Heat));

static_assert(std::size(schedules) == numberOfFunctionSchedules);
static_assert(std::ranges::all_of(schedules, [](const ScheduleDescriptor& descriptor) { return &scheduleAt(descriptor.id) == &descriptor; }));
static_assert(std::ranges::all_of(schedules, hasBalancedFixpoints));
static_assert(std::ranges::all_of(schedules, runsFunctionPassesOfDeclaredForm));
static_assert(std::ranges::all_of(schedules, includesOnlySimplify));
static_assert(std::ranges::all_of(schedules, compactsBehindIncludedSchedules));
static_assert(std::ranges::all_of(schedules, modeFitsPasses));
static_assert(std::ranges::all_of(schedules, schedulesDerivedFunctions));
static_assert(std::ranges::all_of(schedules, runsPassAfterUnverifiedPass));
static_assert(std::ranges::all_of(rows, isScheduled));

static_assert(removingPassesRunUnconditionally());
static_assert(runsUnconditionally(SelectTypedResults));
static_assert(runsUnconditionally(RetypeValues));

static_assert(scheduleAt(FunctionSchedule::Simplify).inputs == simplifyInputs);
static_assert(scheduleAt(FunctionSchedule::Optimize).inputs.contains(CallGraph));
static_assert(canRunSimplify(RemoveUnreadFields));
static_assert(canRunSimplify(RemoveUnusedResults));
static_assert(canRunSimplify(ProveResults));
static_assert(canRunSimplify(RemoveUnusedParameters));
static_assert(canRunSimplify(SplitFreshResults));
static_assert(canRunSimplify(SplitCoroutines));
static_assert(canRunSimplify(ChooseCallingConventions));
static_assert(canRunFunctionPass(RemoveUnreadFields, RemoveDeadCountedLoops));
static_assert(canRunFunctionPass(RemoveUnusedResults, RemoveDeadCountedLoops));
static_assert(canRunFunctionPass(ProveResults, RemoveDeadCountedLoops));
static_assert(canRunFunctionPass(RemoveUnusedParameters, RemoveDeadCountedLoops));
static_assert(canRunFunctionPass(SplitFreshResults, RemoveDeadCountedLoops));
static_assert(canRunFunctionPass(ChooseCallingConventions, RemoveDeadCountedLoops));
static_assert(canRunFunctionPass(SplitFreshResults, SinkAllocations));

} // namespace PassTable

} // anonymous namespace

const PassDescriptor& descriptorOf(PassID id)
{
    RELEASE_ASSERT(id != PassID::None && PassTable::indexOf(id) < numberOfPasses);
    return PassTable::descriptorAt(id);
}

const PassSet& passesEnabling(PassID id)
{
    RELEASE_ASSERT(id != PassID::None && PassTable::indexOf(id) < numberOfPasses);
    return PassTable::enablingPasses[PassTable::indexOf(id)];
}

const PassSet& passesThatCanWaitBehind(PassID changer)
{
    RELEASE_ASSERT(PassTable::indexOf(changer) < numberOfPasses);
    return PassTable::passesThatCanWaitByChanger[PassTable::indexOf(changer)];
}

const ScheduleDescriptor& descriptorOf(FunctionSchedule schedule)
{
    return PassTable::scheduleAt(schedule);
}

FunctionID FunctionPassContext::derivedFunction(const DerivedFunctionKey& key)
{
    RELEASE_ASSERT(inputs.derivedFunctions);
    return inputs.derivedFunctions->ensure(key);
}

Expected<double> FunctionPassContext::heatOf(BlockID block)
{
    return inputs.heat.activationsOf(program, id) * analyses.blockFrequencies().frequencyOf(block);
}

bool FunctionPassContext::runsAtMostOnce(BlockID block)
{
    return analyses.executionCountOf(block) == ExecutionCount::AtMostOnce;
}

Subject FunctionPassContext::subject() const
{
    return analyses.subject();
}

Subject FunctionPassContext::subject(BlockID block) const
{
    Subject result = subject();
    if (!block)
        return result;
    if (result.function == id)
        result.block = block;
    for (ValueID value : function.at(block).values) {
        SourceLocation location = function.locationOf(value);
        if (!hasModule(location))
            continue;
        result.location = location;
        break;
    }
    return result;
}

Subject FunctionPassContext::subject(ValueID value) const
{
    return analyses.subject(value);
}

uint64_t FunctionPassContext::limit(LimitID limit) const
{
    return build.limit(limit);
}

bool FunctionPassContext::exceeds(LimitID limit, uint64_t amount, ValueID value)
{
    if (amount <= build.limit(limit))
        return false;
    build.gaveUp(limit, subject(value));
    return true;
}

GaveUp FunctionPassContext::gaveUp(LimitID limit, ValueID value)
{
    return build.gaveUp(limit, subject(value));
}

void FunctionPassContext::addDecline(Wire::DeclineKind kind, ValueID value, const DeclineReason& reason, LimitID limit, uint32_t count)
{
    if (!build.options().collectsDeclines())
        return;
    build.addDecline({ .kind = kind, .limit = limit, .count = count, .reason = encodeDeclineReason(kind, reason), .witness = witnessOf(subject(value)) });
}

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

    // This and the scan for placement: from any thread, no two for one function at a time, while the caches of the program and an interning phase of its types are open. The body holds no provisional id.
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

    void verifyRecordsAtBoundary(IRForm form)
    {
        PassRunner& runner = m_pipeline.m_runner;
        if (!verifiesProgram() || runner.isStopped())
            return;
        verifyRecords(m_pipeline.m_program, form, m_pipeline.m_build, runner.verifierInputs());
        runner.stopAfterErrors();
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
    case PassID::CopyClosureFactories:
        m_copiedFunctions = edit(id, copyClosureFactories, m_build);
        didAddFunctions();
        return m_runner.lastProgramPassOutcome();
    case PassID::AllocateTaggedLayouts:
        editProgram(id, [&] {
            allocateTaggedLayouts(m_program, m_build);
            giveLayoutsToCreatedObjects(m_program, std::exchange(m_copiedFunctions, { }), m_build);
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
            expectFieldsByNamesAsked(m_program, m_build);
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
        edit(id, deferClosuresInFields, inputsFor(id).heat, m_build);
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
    case PassID::ProveResults:
        edit(id, proveResults, inputsFor(id), m_runner);
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

// Whether a parameter that is tested at the entrance goes into a position as it is: the front end hands it on to a capture or a result of its own type without a conversion.
static bool storesTestedParameter(const Function& function)
{
    Vector<ValueID, 8> tests;
    for (size_t index = 0; index < function.values.size(); ++index) {
        if (function.values[index].opcode == Opcode::Convert && function.values[index].illTypedCause() == IllTypedCause::UnseenCaller)
            tests.append(ValueID(static_cast<uint32_t>(index)));
    }
    if (tests.isEmpty())
        return false;
    BitVector isTestedParameter = valuesThatAre(function, CFG(function), tests.span());
    bool result = false;
    auto look = [&](const auto& user) {
        user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            result |= isTestedParameter.get(operand.index()) && user.operandKind(operandIndex) == OperandKind::Declared;
        });
    };
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values)
            look(function.at(id));
        look(block.terminator);
    }
    return result;
}

// An expected type decides which test is worth making and never what is right, so a program means the same under any expected types.
// TestingSwitch::ShuffleExpectedTypes gives every expected position and conversion of the program's own code another of the types that are expected somewhere in it, drawn from the seed and the number of its type.
// So does the test of a parameter at the entrance: to the function every caller then hands over a value of another type than it says.
static void shuffleExpectedTypes(Program& program, BuildContext& build)
{
    if (!build.recordedOptions().has(TestingSwitch::ShuffleExpectedTypes)) [[likely]]
        return;

    auto isShuffled = [&](const DeclaredType& declared, bool isTestAtEntrance = false) {
        if ((!declared.isExpected() && !isTestAtEntrance) || !declared.written() || declared.has(PositionFlags::IsInt32Promise))
            return false;
        const Type& type = program.at(declared.written());
        // That a value is an int32 where the program promises one is shown at build time, from what is expected of the other int32s, and a build is refused where it cannot be shown.
        return type.isJavaScriptValue() && !(type.primitives & Primitive::Absent) && !type.isNever() && !type.isAnything() && !type.isInt32OrNullish();
    };
    auto forEachExpectation = [&](const Invocable<bool(DeclaredType&, bool isTestAtEntrance)> auto& functor) {
        for (Function& function : program.functions) {
            if (function.role == FunctionRole::Library)
                continue;
            for (DeclaredType& declared : function.parameters)
                functor(declared, false);
            for (DeclaredType& declared : function.results)
                functor(declared, false);
            bool shufflesTestsAtEntrance = !storesTestedParameter(function);
            for (Value& value : function.editAllValues()) {
                if (value.opcode != Opcode::Convert)
                    continue;
                DeclaredType declared = value.declared();
                if (functor(declared, shufflesTestsAtEntrance && value.illTypedCause() == IllTypedCause::UnseenCaller))
                    value.setDeclared(declared);
            }
        }
        for (Layout& layout : program.layouts) {
            if (layout.module && program.at(layout.module).flags.contains(ModuleFlags::IsLibrary))
                continue;
            for (Field& field : layout.fields)
                functor(field.declared, false);
        }
        for (Variable& variable : program.variables)
            functor(variable.declared, false);
    };

    BitVector isExpectedSomewhere(program.types.size());
    forEachExpectation([&](DeclaredType& declared, bool isTestAtEntrance) {
        if (isShuffled(declared, isTestAtEntrance))
            isExpectedSomewhere.set(declared.written().index());
        return false;
    });
    Vector<TypeID> drawn;
    for (size_t index : isExpectedSomewhere)
        drawn.append(TypeID(static_cast<uint32_t>(index)));
    if (drawn.size() < 2)
        return;
    uint32_t seed = build.recordedOptions().layoutSeed;
    forEachExpectation([&](DeclaredType& declared, bool isTestAtEntrance) {
        if (!isShuffled(declared, isTestAtEntrance))
            return false;
        // Always another one.
        size_t own = std::ranges::lower_bound(drawn, declared.written(), { }, [](TypeID type) { return type; }) - drawn.begin();
        size_t steps = 1 + WTF::pairIntHash(seed, declared.written().index()) % (drawn.size() - 1);
        declared.changeType(Proven::byConversion(ExpectedType(drawn[(own + steps) % drawn.size()])));
        return true;
    });
}

void Pipeline::runReadStage()
{
    TimedStage stage(*this, Stage::ReadAndVerify);
    stage.verifyRecordsAtBoundary(IRForm::AsRead);
    shuffleRecordedFrequencies(m_program, m_build);
    forgetLongExpectations(m_program, m_build);
    stage.run(PassID::CopyClosureFactories);
    stage.run(PassID::AllocateTaggedLayouts);
    stage.run(PassID::CheckTypesAtEntry);
    shuffleExpectedTypes(m_program, m_build);
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
    stage.run(FunctionSchedule::Optimize);
    stage.run(PassID::ProveResults);
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
    stage.run(PassID::ChooseCallingConventions);
    stage.run(PassID::DeferClosuresInFields);
    stage.run(PassID::AllocateInFrames);
    stage.run(PassID::ReuseEnvironments);
    if (m_build.options().verifiesAfterEveryStage)
        stage.verify(IRForm::Prepared);
    else if (stage.verifiesProgram()) {
        bool isRunning = !m_runner.isStopped();
        if (!read(PassID::VerifyProgram, verifyUnspecifiedValues, m_program, m_build) && isRunning)
            m_build.diagnostics().add(InternalError { .kind = InternalErrorKind::VerificationFailedAtTheEnd });
        stage.didFinish(nameOf(PassID::VerifyProgram));
    }
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
        auto findOutliningSitesOrReadFinishedFunction = [&](FunctionID id) {
            FunctionAnalyses analyses(finder.contextOf(id), std::as_const(m_program).at(id));
            if (findsOutliningSites)
                finder.findIn(id, analyses);
            if (hasOutliningSites[id.index()])
                return;
            facts.readReferences(id);
            facts.scansForPlacement().scan(id, &analyses);
        };
        ReaderOfScheduledFunction readers1[] = { observe, findOutliningSitesOrReadFinishedFunction };
        const ReaderOfScheduledFunction* readersOfSelectedFunction[] = { &readers1[0], &readers1[1] };
        stage.run(FunctionSchedule::Select, { .behind = readersOfSelectedFunction });
        advanceTo(IRForm::Selected, wasRunning);
        if (!m_runner.isStopped())
            m_output->accessSiteObservations = observer.finish();
        stage.didFinish("observe access sites"_s);

        auto noteAddedFunctions = [&] {
            facts.didAddFunctions();
        };
        auto readFinishedFunction = [&](FunctionID id) {
            facts.readReferences(id);
            facts.scansForPlacement().scan(id);
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

} // namespace Typed

bool isOptionalPass(PassID pass)
{
    return pass != PassID::None && Typed::descriptorOf(pass).necessity == Typed::PassNecessity::Optional;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
