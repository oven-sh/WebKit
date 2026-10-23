/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#define FOR_EACH_AOT_PROGRAM_PASS(v) \
    v(AllocateTaggedLayouts) \
    v(CheckTypesAtEntry) \
    v(VerifyProgram) \
    v(NarrowFieldTypes) \
    v(NameCreatedSymbols) \
    v(GuardTaintedPositions) \
    v(AnalyzeFlow) \
    v(ApplyFlowResults) \
    v(CheckLockedBuiltinWrites) \
    v(BuildCallGraph) \
    v(ComputeEffectSummaries) \
    v(EstimateHeat) \
    v(SummarizeForInlining) \
    v(RemoveUnreadExports) \
    v(PromoteEnvironmentsToVariables) \
    v(DeferDefinitions) \
    v(NarrowConstantVariables) \
    v(DeferInitializers) \
    v(MarkResultsUsedAsKeys) \
    v(ReadCapturesFromEnvironments) \
    v(RemoveUnreadFields) \
    v(RemoveUnreachableFunctions) \
    v(SplitBehindEntryChecks) \
    v(SkipAnsweredEntryChecks) \
    v(PromoteAggregateParameters) \
    v(SplitCoroutines) \
    v(CallModules) \
    v(CreateEntrypoints) \
    v(ChooseCallingConventions) \
    v(DeferClosuresInFields) \
    v(AllocateInFrames) \
    v(AllocateInnerObjects) \
    v(ReuseEnvironments) \
    v(NumberLiteralStrings) \
    v(EmbedConstantCollections) \
    v(CollectRegExpLiterals) \
    v(OutlineColdOperations) \
    v(InlineHeads) \
    v(ElideStackChecks) \
    v(PlanSlots) \
    v(NumberAndEncode) \
    v(PlaceSiteCaches) \
    v(CheckPlacement) \
    v(GenerateCode)

#define FOR_EACH_AOT_FUNCTION_PASS(v) \
    v(TypeValues, typeValues) \
    v(CheckInt32Promises, checkInt32Promises) \
    v(CheckRegExpLiterals, checkRegExpLiterals) \
    v(AddGenericVersion, addGenericVersion) \
    v(ResolveBuiltins, resolveBuiltins) \
    v(BindGlobalNames, bindGlobalNames) \
    v(CallThroughMethodTable, callThroughMethodTable) \
    v(ResolveCalls, resolveCalls) \
    v(ResolveAccesses, resolveAccesses) \
    v(SplitUnions, splitUnions) \
    v(PlaceTypeTests, placeTypeTests) \
    v(ExpandIteration, expandIteration) \
    v(ExpandOwnKeys, expandOwnKeys) \
    v(SelectTypedResults, selectTypedResults) \
    v(RetypeValues, retypeValues) \
    v(InlineCalls, inlineCalls) \
    v(SimplifyCasts, simplifyCasts) \
    v(SinkAllocations, sinkAllocations) \
    v(EliminateRedundantReads, eliminateRedundantReads) \
    v(SeparateUnlikelyLoopPaths, separateUnlikelyLoopPaths) \
    v(RemoveDeadCountedLoops, removeDeadCountedLoops) \
    v(FuseGeneratorLoops, fuseGeneratorLoops) \
    v(CreateStringBuilders, createStringBuilders) \
    v(FuseConcatenations, fuseConcatenations) \
    v(ExpandData, expandData) \
    v(FoldConstants, foldConstants) \
    v(SimplifyControlFlow, simplifyControlFlow) \
    v(RemoveTrivialParameters, removeTrivialParameters) \
    v(HoistTestsIntoPredecessors, hoistTestsIntoPredecessors) \
    v(TestNarrowerTypesFirst, testNarrowerTypesFirst) \
    v(ThreadJumps, threadJumps) \
    v(InferSwitches, inferSwitches) \
    v(EliminateDeadCode, eliminateDeadCode) \
    v(CompactBlocks, compactBlocks) \
    v(RemoveRedundantHandlers, removeRedundantHandlers) \
    v(HoistLoopInvariantReads, hoistLoopInvariantReads) \
    v(SimplifyBuiltinCalls, simplifyBuiltinCalls) \
    v(PlanArrayCapacities, planArrayCapacities) \
    v(InsertObjectCoercibleChecks, insertObjectCoercibleChecks) \
    v(ResolveAwaitedCalls, resolveAwaitedCalls) \
    v(ExpandLongArrayLiterals, expandLongArrayLiterals) \
    v(FinishBirths, finishBirths) \
    v(RestartInGenericVersion, restartInGenericVersion) \
    v(Select, select) \
    v(ChooseRepresentations, chooseRepresentations) \
    v(RemoveRedundantBarriers, removeRedundantBarriers)

#endif // ENABLE(AOT)
