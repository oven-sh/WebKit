/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(B3_JIT)

#include <cstddef>

namespace JSC { namespace B3 { namespace AirLimits {

inline constexpr unsigned shortJumpReach = 127;
inline constexpr unsigned pairPreIndexReach = 512;
inline constexpr unsigned preIndexReach = 256;

#if CPU(X86_64)
inline constexpr unsigned estimatedBytesPerInst = 5;
#else
inline constexpr unsigned estimatedBytesPerInst = 4;
#endif

inline constexpr size_t maxWordsPerLivenessMatrix = 1 << 20;
// Each slice pays for every block it visits, however narrow it is.
inline constexpr size_t minWordsPerLivenessSlice = 16;
inline constexpr size_t minWordsPerLivenessSliceWithoutTails = 64;
inline constexpr size_t maxWordsOfLivenessAtTails = 1 << 24;
inline constexpr unsigned minBlocksToSolveLivenessInPostOrder = 256;
// Ordinary code has about two.
inline constexpr size_t maxBoundaryIndicesPerInst = 32;

inline constexpr unsigned maxBlocksOnClusterPaths = 6;
inline constexpr unsigned maxAffinityGroupSizeWithoutMap = 24;
inline constexpr unsigned clobberPointsPerChunk = 64;
inline constexpr unsigned minClobberPointsForChunks = 1024;
inline constexpr size_t minClobberPointsToLeap = 4096;
inline constexpr unsigned maxConsecutiveClobberMisses = 64;
inline constexpr size_t minClobberPointsToCountPerRegister = 256;
// The moves at so many sites take no room worth counting.
inline constexpr size_t clobberSitesWithoutLeastCost = 32;
inline constexpr unsigned minInsertionsPerBlockToSort = 16;

inline constexpr unsigned maxSlotsForDenseInterference = 8192;
// As many as the slots at that limit can have.
inline constexpr size_t maxInterferenceEdgesAboveDenseLimit = static_cast<size_t>(maxSlotsForDenseInterference) * maxSlotsForDenseInterference / 2;
inline constexpr size_t bytesPerNeighborInSparseInterference = 6;
inline constexpr size_t minSlotsToOrderByUse = 13;
inline constexpr size_t minAssignedSlotsToWalkComplement = 256;
inline constexpr size_t minRatioOfAssignedToNonInterfering = 16;

inline constexpr unsigned maxBytesAboveFrameRecord = 480;
inline constexpr unsigned maxBytesBelowFrameRecord = 240;
inline constexpr unsigned maxFrameSizeWithPushedRegisters = 496;
inline constexpr unsigned maxInstsScannedForSpillsAtFrameSetup = 16;
inline constexpr unsigned maxRegistersPushedAtFrameSetup = 6;
inline constexpr unsigned maxWordsClearedAtFirstSuspension = 15;

inline constexpr unsigned maxBytesFromGuardToRareBlock = 24 * estimatedBytesPerInst;
inline constexpr unsigned maxBytesOfRareBlockNearGuard = 16 * estimatedBytesPerInst;
inline constexpr double maxRatioOfBlockToLikelyPredecessor = 64;
inline constexpr double minRatioOfLikelierEdge = 1.01;
inline constexpr size_t maxSuccessorsToBubbleSort = 16;

static_assert(maxBytesBelowFrameRecord <= preIndexReach);
#if CPU(X86_64)
static_assert(maxBytesFromGuardToRareBlock <= shortJumpReach);
#endif

} } } // namespace JSC::B3::AirLimits

#endif // ENABLE(B3_JIT)
