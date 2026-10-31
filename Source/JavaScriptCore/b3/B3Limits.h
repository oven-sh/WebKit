/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirLimits.h"
#include <array>
#include <limits>
#include <wtf/Assertions.h>
#include <wtf/EnumTraits.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace B3 {

enum class LimitPurpose : uint8_t {
    CompileTime,
    CodeSize,
    RegisterPressure,
    SpeculatedWork,
    Memory,
};

#define FOR_EACH_B3_LIMIT(macro) \
    macro(MaxClonedValuesPerLoopVersion, 48, CodeSize, "the loop is not versioned"_s) \
    macro(MaxValuesPerHotRegion, 96, CompileTime, "the loop is not versioned"_s) \
    macro(MaxClonedValuesPerUnswitchedLoop, 64, CodeSize, "the branch is not unswitched"_s) \
    macro(MaxClonedValuesPerExcursion, 128, CodeSize, "the remaining rare blocks are not copied, and their edges leave the fast loop"_s) \
    macro(MaxBlocksScannedBeforeLoop, 4, CompileTime, "values computed farther above the loop are not reused"_s) \
    macro(MaxValuesScannedForExecutedLoad, 512, CompileTime, "the load counts as not executed"_s) \
    macro(MaxSourcesOfLoopTest, 16, CompileTime, "the test counts as loop-variant"_s) \
    macro(MaxRangeCheckBoundsPerLoop, 4, CodeSize, "further range checks stay in the loop"_s) \
    macro(MaxCopiedValuesPerRotatedLoop, 12, CodeSize, "the loop is not rotated"_s) \
    macro(MaxRareBlocksPerRotatedLoop, 8, CompileTime, "the loop is not rotated"_s) \
    macro(MaxValuesPerUnrolledLoop, 96, CodeSize, "fewer turns of the loop are written out, or none"_s) \
    macro(MaxThreadedTestCopyCost, 8, CodeSize, "the threaded path ends at the block"_s) \
    macro(MaxThreadedPathCost, 16, CodeSize, "the threaded path ends at the block"_s) \
    macro(MaxBlocksOnThreadedPath, 8, CodeSize, "the threaded path ends at the block"_s) \
    macro(MaxTailsCrossedByCopiedValues, 128, CodeSize, "the block is not copied"_s) \
    macro(MaxThreadedTestDepth, 8, CompileTime, "the test is undecided"_s) \
    macro(MaxPredecessorsForConditionQueries, 8, CompileTime, "the test is undecided"_s) \
    macro(MaxConditionQueriesPerBlock, 32, CompileTime, "the remaining dominating conditions are not asked"_s) \
    macro(MaxSelectNestingDepth, 1, SpeculatedWork, "the branch is not converted to a select"_s) \
    macro(MaxSpeculatedValuesPerSelect, 4, SpeculatedWork, "the branch is not converted to a select"_s) \
    macro(MaxSelectsPerJoin, 3, SpeculatedWork, "the branch is not converted to a select"_s) \
    macro(MaxValuesSearchedForDominatingLoad, 16, CompileTime, "the branch is not converted to a select"_s) \
    macro(MaxLoopsLeftBySinking, 8, CompileTime, "the value is not sunk"_s) \
    macro(MaxHoistedLoadsPerLoop, 8, RegisterPressure, "the load stays in the loop"_s) \
    macro(MaxRecordedWritesPerMemoryState, 256, CompileTime, "the state starts a new epoch"_s) \
    macro(MaxLivenessStepsPerBlock, 48, CompileTime, "the dense liveness runs, with the same result"_s) \
    macro(MaxLinearDominatorSteps, 32, CompileTime, "jump pointers are built, with the same result"_s) \
    macro(MaxPathConstantsScanned, 32, CompileTime, "an index is built, with the same result"_s) \
    macro(MaxBlocksPerPlacedConstant, 8192, CompileTime, "each stretch of blocks gets a copy of the constant"_s) \
    macro(MaxRangeAnalysisDepth, 5, CompileTime, "the range is top"_s) \
    macro(MaxLeavesToRebalance, 32, CompileTime, "the expression tree is not rebalanced"_s) \
    macro(MaxBitTestTargets, 3, CodeSize, "the cases are lowered as a binary search"_s) \
    macro(MaxCompareChainSearchSize, 16, CompileTime, "the condition is not selected as a compare chain"_s) \
    macro(MaxSlotsForDenseInterference, AirLimits::maxSlotsForDenseInterference, Memory, "the sparse interference graph is built, with the same result"_s) \
    macro(MaxBoundaryIndicesPerInst, AirLimits::maxBoundaryIndicesPerInst, Memory, "a Tmp or a spill slot counts as live in the blocks that lie between those where it is live"_s)

enum class Limit : uint8_t {
#define B3_LIMIT_ENUMERATOR(name, defaultValue, purpose, degradesTo) name,
    FOR_EACH_B3_LIMIT(B3_LIMIT_ENUMERATOR)
#undef B3_LIMIT_ENUMERATOR
};

#define B3_LIMIT_COUNT(name, defaultValue, purpose, degradesTo) + 1
static constexpr unsigned numberOfLimits = 0 FOR_EACH_B3_LIMIT(B3_LIMIT_COUNT);
#undef B3_LIMIT_COUNT

constexpr uint64_t defaultValueOf(Limit limit)
{
    switch (limit) {
#define B3_LIMIT_VALUE_CASE(name, defaultValue, purpose, degradesTo) \
    case Limit::name: \
        return defaultValue;
    FOR_EACH_B3_LIMIT(B3_LIMIT_VALUE_CASE)
#undef B3_LIMIT_VALUE_CASE
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr LimitPurpose purposeOf(Limit limit)
{
    switch (limit) {
#define B3_LIMIT_PURPOSE_CASE(name, defaultValue, purpose, degradesTo) \
    case Limit::name: \
        return LimitPurpose::purpose;
    FOR_EACH_B3_LIMIT(B3_LIMIT_PURPOSE_CASE)
#undef B3_LIMIT_PURPOSE_CASE
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr ASCIILiteral degradesTo(Limit limit)
{
    switch (limit) {
#define B3_LIMIT_DEGRADES_TO_CASE(name, defaultValue, purpose, degradesTo) \
    case Limit::name: \
        return degradesTo;
    FOR_EACH_B3_LIMIT(B3_LIMIT_DEGRADES_TO_CASE)
#undef B3_LIMIT_DEGRADES_TO_CASE
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr ASCIILiteral nameOf(Limit limit) { return enumName(limit); }

class LimitValues {
public:
    constexpr LimitValues()
    {
        for (unsigned i = 0; i < numberOfLimits; ++i)
            m_values[i] = defaultValueOf(static_cast<Limit>(i));
    }

    constexpr uint64_t get(Limit limit) const { return m_values[static_cast<size_t>(limit)]; }
    constexpr void set(Limit limit, uint64_t value) { m_values[static_cast<size_t>(limit)] = value; }

    friend bool operator==(const LimitValues&, const LimitValues&) = default;

private:
    std::array<uint64_t, numberOfLimits> m_values { };
};

namespace Limits {

inline constexpr int64_t maxCountedLoopStep = 1 << 16;
inline constexpr int64_t maxRangeCheckOffset = 1 << 16;
inline constexpr int64_t maxLoopBoundForRangeChecks = 1ll << 62;
static_assert(maxLoopBoundForRangeChecks < std::numeric_limits<int64_t>::max() - maxRangeCheckOffset - maxCountedLoopStep);

inline constexpr int64_t maxTurnsWithoutPoll = 1 << 20;
inline constexpr unsigned maxValuesPerTurnCopiedWhereLoopRunsRarely = 12;
inline constexpr unsigned maxValuesPerTurnRunInStretches = 32;
inline constexpr double frequencyLostToBranchesInFrontOfLoop = 4;
inline constexpr unsigned maxTermsPerInvariantSum = 4;
inline constexpr unsigned maxStridedRangeChecksPerLoop = 4;
inline constexpr unsigned maxReducedIndicesPerLoop = 16;

inline constexpr int64_t minPageSize = 4096;

inline constexpr unsigned minHoistedValuesPerLoopVersion = 2;
inline constexpr unsigned minTurnsPerUnrolledLoop = 4;
inline constexpr unsigned maxTurnsPerUnrolledLoop = 8;
inline constexpr unsigned maxBlocksBesideTestOfUnrolledLoop = 4;
inline constexpr unsigned maxDepthOfAddressInUnrolledLoop = 6;
inline constexpr unsigned maxValuesPerTurnOfVectorizedLoop = 64;
inline constexpr unsigned maxAccessesPerTurnOfVectorizedLoop = 8;
inline constexpr unsigned maxDistancesTestedInFrontOfVectorizedLoop = 3;
inline constexpr unsigned maxDepthOfAddressInVectorizedLoop = 8;
inline constexpr unsigned maxTermsOfAddressInVectorizedLoop = 3;
inline constexpr unsigned maxCountersOfVectorizedLoop = 3;
inline constexpr unsigned maxDistancesCheckedInFrontOfVectorizedLoop = 8;
inline constexpr unsigned maxSumsOfVectorizedLoop = 2;
inline constexpr unsigned maxChoicesPerTurnOfVectorizedLoop = 4;
inline constexpr int64_t minTurnsPerStretchOfVectorizedLoop = 1 << 10;
inline constexpr unsigned minCasesForBitTests = 4;
inline constexpr unsigned maxCompareChainSize = 4;
inline constexpr unsigned maxValuesBesideSecondTestOfInterval = 4;
inline constexpr unsigned maxValuesInPlaceOfBranchWithUnlikelySide = 1;
inline constexpr unsigned blocksPerConstantStretch = 1024;

inline constexpr unsigned minCasesToGrowSwitchInPlace = 128;
inline constexpr unsigned minPredecessorsForIndexedList = 64;
inline constexpr unsigned maxSizeToBubbleSort = 16;

} // namespace Limits

enum class GaveUpReason : uint8_t {
    None,
    NotInnermost,
    SeveralEntries,
    NoSingleLikelyPath,
    UncloneableValue,
    NothingToHoist,
    NotCounted,
    CounterMayWrap,
    BoundNotInvariant,
    TestNotOnLikelyPath,
    EffectsBeforeTopTest,
    LimitReached,
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
