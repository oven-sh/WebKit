/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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
    v(ExpandProvenIterations, expandProvenIterations) \
    v(ExpandOwnKeys, expandOwnKeys) \
    v(SelectTypedResults, selectTypedResults) \
    v(RetypeValues, retypeValues) \
    v(InlineCalls, inlineCalls) \
    v(SimplifyCasts, simplifyCasts) \
    v(SinkAllocations, sinkAllocations) \
    v(EliminateRedundantReads, eliminateRedundantReads) \
    v(EliminateGuards, eliminateGuards) \
    v(SeparateUnlikelyLoopPaths, separateUnlikelyLoopPaths) \
    v(ConvertCountersToIntegers, convertCountersToIntegers) \
    v(RemoveDeadCountedLoops, removeDeadCountedLoops) \
    v(FuseArrayChains, fuseArrayChains) \
    v(FuseGeneratorLoops, fuseGeneratorLoops) \
    v(StartGeneratorsAtFirstStep, startGeneratorsAtFirstStep) \
    v(CreateStringBuilders, createStringBuilders) \
    v(FuseConcatenations, fuseConcatenations) \
    v(ExpandData, expandData) \
    v(FoldConstants, foldConstants) \
    v(SimplifyControlFlow, simplifyControlFlow) \
    v(RemoveTrivialParameters, removeTrivialParameters) \
    v(PropagateBranchConditions, propagateBranchConditions) \
    v(HoistTestsIntoPredecessors, hoistTestsIntoPredecessors) \
    v(TestNarrowerTypesFirst, testNarrowerTypesFirst) \
    v(ThreadJumps, threadJumps) \
    v(EliminateDeadCode, eliminateDeadCode) \
    v(CompactBlocks, compactBlocks) \
    v(RemoveRedundantHandlers, removeRedundantHandlers) \
    v(HoistLoopInvariantReads, hoistLoopInvariantReads) \
    v(SimplifyBuiltinCalls, simplifyBuiltinCalls) \
    v(ReduceIntegerDivisions, reduceIntegerDivisions) \
    v(PlanArrayCapacities, planArrayCapacities) \
    v(PlanCollectionCapacities, planCollectionCapacities) \
    v(FoldConstantRegExps, foldConstantRegExps) \
    v(ShareRegExpLiterals, shareRegExpLiterals) \
    v(InsertObjectCoercibleChecks, insertObjectCoercibleChecks) \
    v(EliminateTailRecursion, eliminateTailRecursion) \
    v(ResolveAwaitedCalls, resolveAwaitedCalls) \
    v(SinkConstants, sinkConstants) \
    v(ExpandLongArrayLiterals, expandLongArrayLiterals) \
    v(FinishBirths, finishBirths) \
    v(RestartInGenericVersion, restartInGenericVersion) \
    v(Select, select) \
    v(ChooseRepresentations, chooseRepresentations) \
    v(RemoveRedundantBarriers, removeRedundantBarriers)

#endif // ENABLE(AOT)
