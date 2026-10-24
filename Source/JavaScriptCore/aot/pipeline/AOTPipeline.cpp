/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPipeline.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTOpcode.h"
#include "AOTPass.h"
#include "AOTPassRunner.h"
#include <algorithm>
#include <array>
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
    return {
        .id = id,
        .scope = PassScope::Program,
        .necessity = necessity,
        .firstForm = forms.first,
        .lastForm = forms.last,
        .produces = std::nullopt,
        .inputs = inputs,
        .outputs = outputs,
        .invalidatedResults = invalidatedResults,
        .invalidatedAnalyses = InvalidatedAnalyses::None,
        .editedFunctions = EditedFunctions::DoAnything,
        .effectsAddedByEdits = Effects::none(),
        .readsOtherBodies = false,
        .derivesFunctions = false,
        .countsOwnChanges = false,
        .isVerifiedAfterNextPass = false,
        .run = nullptr,
    };
}

constexpr PassDescriptor functionPass(PassID id, FunctionPass run, PassNecessity necessity, IRForm form, Results inputs, InvalidatedAnalyses invalidatedAnalyses)
{
    return {
        .id = id,
        .scope = PassScope::Function,
        .necessity = necessity,
        .firstForm = form,
        .lastForm = form,
        .produces = std::nullopt,
        .inputs = inputs,
        .outputs = { },
        .invalidatedResults = { },
        .invalidatedAnalyses = invalidatedAnalyses,
        .editedFunctions = EditedFunctions::DoAnything,
        .effectsAddedByEdits = Effects::none(),
        .readsOtherBodies = false,
        .derivesFunctions = false,
        .countsOwnChanges = false,
        .isVerifiedAfterNextPass = false,
        .run = run,
    };
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

constexpr PassDescriptor readingOtherBodies(PassDescriptor descriptor)
{
    descriptor.readsOtherBodies = true;
    return descriptor;
}

constexpr PassDescriptor derivingFunctions(PassDescriptor descriptor)
{
    descriptor.derivesFunctions = true;
    return descriptor;
}

constexpr PassDescriptor countingOwnChanges(PassDescriptor descriptor)
{
    descriptor.countsOwnChanges = true;
    return descriptor;
}

constexpr PassDescriptor verifiedAfterNextPass(PassDescriptor descriptor)
{
    descriptor.isVerifiedAfterNextPass = true;
    return descriptor;
}

constexpr InvalidatedAnalyses nothing = InvalidatedAnalyses::None;
constexpr InvalidatedAnalyses values = InvalidatedAnalyses::Values;
constexpr InvalidatedAnalyses everything = InvalidatedAnalyses::All;

constexpr Results slotInputs { Flow, CallGraph, Heat, FrameAllocation };
constexpr Results placementInputs { Flow, CallGraph, Heat, CallingConventions, FrameAllocation, LiteralStrings, ConstantCollections, SlotPlan, RegExpRequests };
constexpr Results codeGenerationInputs { Flow, EffectSummaries, Heat, CallingConventions, FrameAllocation, LiteralStrings, ConstantCollections, RegExpRequests, StackChecks, Placement, SiteCaches };

constexpr std::array rows {
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
    addingOnly({ Opcode::GetDeferredClosureSlot, Opcode::GetDeferredClosureSlotOrUndefined }, programPass(DeferClosuresInFields, Optional, { Prepared, Prepared }, { }, { }, resultsAboutBodies)),
    addingOnly({ Opcode::MaterializeEnvironment, Opcode::EmptyArgumentList }, programPass(AllocateInFrames, Optional, { Prepared, Prepared }, { Flow, CallGraph, Heat, EffectSummaries }, { FrameAllocation }, { EffectSummaries })),
    programPass(FlattenInnerObjects, Optional, { Prepared, Prepared }, { Flow, Heat }, { }, resultsAboutBodies),
    addingOnly({ }, programPass(AllocateInnerObjects, Optional, { Prepared, Prepared }, { Flow, CallGraph, Heat }, { }, { })),
    programPass(NumberLiteralStrings, Required, { Prepared, Placed }, { }, { LiteralStrings }, { }),
    addingOnly({ Opcode::Data, Opcode::NewSetFromData, Opcode::NewMapFromData }, programPass(EmbedConstantCollections, Optional, { Prepared, Prepared }, { Flow }, { ConstantCollections }, { })),
    countingOwnChanges(programPass(CollectRegExpLiterals, Required, { Selected, Selected }, { Flow }, { RegExpRequests }, { })),
    programPass(OutlineColdOperations, Optional, { Selected, Selected }, { Flow, Heat, EffectSummaries }, { }, resultsAboutBodies),
    programPass(InlineHeads, Optional, { Selected, Selected }, { Flow, Heat, EffectSummaries }, { }, resultsAboutBodies),
    programPass(ElideStackChecks, Optional, { Selected, Selected }, { CallGraph }, { StackChecks }, { }),
    programPass(PlanSlots, Required, { Prepared, Selected }, slotInputs, { SlotPlan }, { }),
    producing(Placed, programPass(NumberAndEncode, Required, { Selected, Selected }, placementInputs, { Placement }, { })),
    programPass(PlaceSiteCaches, Required, { Placed, Placed }, { Heat, Placement }, { SiteCaches }, { }),
    programPass(CheckPlacement, Required, { Placed, Placed }, placementInputs | Results { Placement, SiteCaches }, { }, { }),
    programPass(GenerateCode, Required, { Placed, Placed }, codeGenerationInputs, { }, { }),

    verifiedAfterNextPass(functionPass(TypeValues, typeValues, Required, AsRead, { }, everything)),
    verifiedAfterNextPass(functionPass(CheckInt32Promises, checkInt32Promises, Required, AsRead, { }, values)),
    verifiedAfterNextPass(functionPass(CheckRegExpLiterals, checkRegExpLiterals, Required, AsRead, { }, nothing)),

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
    functionPass(MergeDuplicateBlocks, mergeDuplicateBlocks, Optional, Prepared, { }, everything),
    functionPass(CompactBlocks, compactBlocks, Required, Prepared, { }, everything),
    functionPass(RemoveRedundantHandlers, removeRedundantHandlers, Optional, Prepared, { EffectSummaries }, everything),
    functionPass(HoistLoopInvariantReads, hoistLoopInvariantReads, Optional, Prepared, { Flow, EffectSummaries }, values),
    functionPass(SimplifyBuiltinCalls, simplifyBuiltinCalls, Optional, Prepared, { EffectSummaries }, everything),
    functionPass(ReduceIntegerDivisions, reduceIntegerDivisions, Optional, Prepared, { }, values),
    functionPass(PlanArrayCapacities, planArrayCapacities, Optional, Prepared, { Flow, EffectSummaries }, everything),
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
    functionPass(PlaceAllocationsAtTheirUse, placeAllocationsAtTheirUse, Optional, Prepared, { }, values),

    producing(Selected, functionPass(Select, select, Required, Prepared, { Flow, Heat, EffectSummaries, CallingConventions, SlotPlan }, everything)),
    functionPass(ChooseRepresentations, chooseRepresentations, Required, Placed, { CallingConventions, Placement }, everything),
    functionPass(RemoveRedundantBarriers, removeRedundantBarriers, Optional, Placed, { EffectSummaries, CallingConventions, Placement }, nothing),
};

constexpr std::array typeSteps {
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
    ScheduleStep::run(MergeDuplicateBlocks),
    ScheduleStep::run(RetypeValues),
    ScheduleStep::run(CompactBlocks),
};

constexpr std::array prepareSteps {
    ScheduleStep::run(SimplifyCasts),
    ScheduleStep::run(InsertObjectCoercibleChecks),
    ScheduleStep::run(PlaceAllocationsAtTheirUse),
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
        ScheduleStep::run(PlaceAllocationsAtTheirUse),
        ScheduleStep::run(FuseGeneratorLoops),
        ScheduleStep::run(SimplifyCasts),
        ScheduleStep::run(RetypeValues),
        ScheduleStep::run(InsertObjectCoercibleChecks),
        ScheduleStep::run(RemoveRedundantHandlers),
        ScheduleStep::includeWithoutCompacting(FunctionSchedule::Simplify),
        ScheduleStep::run(SinkAllocations),
    ScheduleStep::endFixpoint(),
    ScheduleStep::run(ExpandIteration),
    ScheduleStep::run(SelectTypedResults),
    // What SelectTypedResults has put in calls helpers of the compiler, which are in no realm. Where the rounds above have ended, as they do after one for a generic version, nobody else makes direct calls of them.
    ScheduleStep::run(ResolveBuiltins),
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

constexpr std::array<PassDescriptor, numberOfPasses> createDescriptors()
{
    std::array<PassDescriptor, numberOfPasses> result { };
    for (const PassDescriptor& row : rows)
        result[indexOf(row.id)] = row;
    return result;
}

constexpr std::array<PassDescriptor, numberOfPasses> descriptors = createDescriptors();

constexpr const PassDescriptor& descriptorAt(PassID id)
{
    return descriptors[indexOf(id)];
}

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
    { MergeDuplicateBlocks, { RemoveTrivialParameters, EliminateRedundantReads } },
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

constexpr std::array<PassSet, numberOfPasses> createPassesThatCanWait()
{
    std::array<PassSet, numberOfPasses> result { };
    for (const PassesLeftIdle& row : passesThatCanWait)
        result[indexOf(row.changer)] = row.passes;
    return result;
}

constexpr std::array<PassSet, numberOfPasses> passesThatCanWaitByChanger = createPassesThatCanWait();

constexpr std::array<PassSet, numberOfPasses> createEnablingPasses()
{
    std::array<PassSet, numberOfPasses> changersWithoutEffect { };
    for (const PassesLeftIdle& row : passesLeftIdle) {
        for (PassID pass : allMembersOf<PassID>) {
            if (row.passes.contains(pass))
                changersWithoutEffect[indexOf(pass)].add(row.changer);
        }
    }

    std::array<PassSet, numberOfPasses> result { };
    for (PassID pass : allMembersOf<PassID>) {
        for (PassID changer : allMembersOf<PassID>) {
            if (!changersWithoutEffect[indexOf(pass)].contains(changer))
                result[indexOf(pass)].add(changer);
        }
    }
    return result;
}

constexpr std::array<PassSet, numberOfPasses> enablingPasses = createEnablingPasses();

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

constexpr const ScheduleDescriptor& scheduleAt(FunctionSchedule id)
{
    return schedules[indexOf(id)];
}

constexpr bool everyPassHasOneRow()
{
    std::array<unsigned, numberOfPasses> numberOfRows { };
    for (const PassDescriptor& row : rows)
        ++numberOfRows[indexOf(row.id)];
    for (unsigned index = 0; index < numberOfPasses; ++index) {
        if (numberOfRows[index] != (index == indexOf(None) ? 0u : 1u))
            return false;
    }
    return true;
}

constexpr bool schedulesAreInEnumOrder()
{
    for (unsigned index = 0; index < schedules.size(); ++index) {
        if (indexOf(schedules[index].id) != index)
            return false;
    }
    return true;
}

constexpr bool hasValidEntryPoint(const PassDescriptor& descriptor)
{
    return (descriptor.scope == PassScope::Function) == !!descriptor.run;
}

constexpr bool hasValidForms(const PassDescriptor& descriptor)
{
    return descriptor.firstForm <= descriptor.lastForm;
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

constexpr bool passListsMatchTable()
{
    std::array<bool, numberOfPasses> isListed { };
#define AOT_CHECK_FUNCTION_PASS(id, function) \
    if (descriptorAt(PassID::id).run != function) \
        return false; \
    isListed[indexOf(PassID::id)] = true;
    FOR_EACH_AOT_FUNCTION_PASS(AOT_CHECK_FUNCTION_PASS)
#undef AOT_CHECK_FUNCTION_PASS
#define AOT_CHECK_PROGRAM_PASS(id) \
    if (descriptorAt(PassID::id).scope != PassScope::Program || isListed[indexOf(PassID::id)]) \
        return false; \
    isListed[indexOf(PassID::id)] = true;
    FOR_EACH_AOT_PROGRAM_PASS(AOT_CHECK_PROGRAM_PASS)
#undef AOT_CHECK_PROGRAM_PASS
    return std::ranges::all_of(rows, [&](const PassDescriptor& row) {
        return isListed[indexOf(row.id)];
    });
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

constexpr bool runsOnlyFunctionPasses(const ScheduleDescriptor& descriptor)
{
    return allPassesIn(descriptor, [](const PassDescriptor& pass) {
        return pass.scope == PassScope::Function;
    });
}

constexpr bool runsInDeclaredForm(const ScheduleDescriptor& descriptor)
{
    return allPassesIn(descriptor, [&](const PassDescriptor& pass) {
        return pass.firstForm == descriptor.form && pass.lastForm == descriptor.form;
    });
}

constexpr bool isScheduled(PassID id)
{
    return std::ranges::any_of(schedules, [&](const ScheduleDescriptor& descriptor) {
        return std::ranges::any_of(descriptor.steps, [&](const ScheduleStep& step) {
            return step.kind == ScheduleStep::Kind::RunPass && step.pass == id;
        });
    });
}

constexpr bool schedulesEveryFunctionPass()
{
    return std::ranges::all_of(rows, [](const PassDescriptor& row) {
        return row.scope == PassScope::Program || isScheduled(row.id);
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

constexpr bool isLocalToFunction(const PassDescriptor& pass)
{
    return !pass.readsOtherBodies && !pass.derivesFunctions;
}

constexpr bool modeFitsPasses(const ScheduleDescriptor& descriptor)
{
    if (descriptor.mode == ScheduleMode::OnCopies)
        return true;
    return descriptor.functions != ScheduledFunctions::Reached && allPassesIn(descriptor, isLocalToFunction);
}

// optimizeCopy() scans a finished copy for function references only when the schedule is Reached.
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
static_assert(std::ranges::all_of(rows, hasValidEntryPoint));
static_assert(std::ranges::all_of(rows, hasValidForms));
static_assert(std::ranges::all_of(rows, hasValidProducedForm));
static_assert(std::ranges::all_of(rows, canObtainInputs));
static_assert(passListsMatchTable());
static_assert(resultsHaveOneProducer());
static_assert(std::ranges::all_of(passesLeftIdle, namesFunctionPasses));
static_assert(std::ranges::all_of(passesThatCanWait, namesFunctionPasses));
static_assert(std::ranges::all_of(enablingPasses, [](const PassSet& passes) { return passes.contains(None); }));
static_assert(!descriptorAt(ChooseRepresentations).inputs.contains(Heat));

static_assert(std::size(schedules) == numberOfFunctionSchedules);
static_assert(schedulesAreInEnumOrder());
static_assert(std::ranges::all_of(schedules, hasBalancedFixpoints));
static_assert(std::ranges::all_of(schedules, runsOnlyFunctionPasses));
static_assert(std::ranges::all_of(schedules, runsInDeclaredForm));
static_assert(std::ranges::all_of(schedules, includesOnlySimplify));
static_assert(std::ranges::all_of(schedules, compactsBehindIncludedSchedules));
static_assert(std::ranges::all_of(schedules, modeFitsPasses));
static_assert(std::ranges::all_of(schedules, schedulesDerivedFunctions));
static_assert(std::ranges::all_of(schedules, runsPassAfterUnverifiedPass));
static_assert(schedulesEveryFunctionPass());

static_assert(removingPassesRunUnconditionally());
static_assert(runsUnconditionally(SelectTypedResults));
static_assert(runsUnconditionally(RetypeValues));

static_assert(scheduleAt(FunctionSchedule::Simplify).inputs == simplifyInputs);
static_assert(scheduleAt(FunctionSchedule::Optimize).inputs.contains(CallGraph));
static_assert(canRunSimplify(RemoveUnreadFields));
static_assert(canRunSimplify(RemoveUnusedResults));
static_assert(canRunSimplify(RemoveUnusedParameters));
static_assert(canRunSimplify(SplitFreshResults));
static_assert(canRunSimplify(SplitCoroutines));
static_assert(canRunSimplify(ChooseCallingConventions));
static_assert(canRunFunctionPass(RemoveUnreadFields, RemoveDeadCountedLoops));
static_assert(canRunFunctionPass(RemoveUnusedResults, RemoveDeadCountedLoops));
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
