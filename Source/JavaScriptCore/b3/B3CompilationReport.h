/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Limits.h"
#include "B3Origin.h"
#include <array>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace B3 {

enum class FailureKind : uint8_t {
    AbsoluteAddress,
    RelocationNotLowered,
    VariableReadBeforeSet,
    SpillSlotLacksItsValue,
    RegisterLacksItsValue,
};

struct Failure {
    FailureKind kind;
    Origin origin;
    String detail { };
};

enum class RemarkKind : uint8_t {
    LoopVersioned,
    LoopNotVersioned,
    LoopRotated,
    LoopVectorized,
    LoopUnrolled,
    RangeChecksRemoved,
    LoopBoundNotClamped,
};

struct Remark {
    RemarkKind kind;
    GaveUpReason reason;
    Origin origin;
    std::optional<Limit> limit;
};

enum class Counter : uint8_t {
    LoopsVersioned,
    LoopsRotated,
    LoopsVectorized,
    LoopsUnrolled,
    RangeChecksRemoved,
    LoopBoundsClamped,
    LoopPollsRemoved,
    WordsCarriedAroundLoops,
    SumsKeptAsIntegers,
    KnownInductionsUsed,
    TrapsFoldedIntoAccesses,
    ColdUsesKeptThroughSecondSplit,
    ColdUsesKeptThroughSecondSplitBetweenDefs,
    MovesReadForWidths,
    PredecessorsReadForRareBlocks,
    StepsToReleaseDeadValues,
    RangesHeldAgainstStackCells,
    ClobberPointsReadForCalls,
    AccessesReadForWordsCarriedAroundLoops,
    LoopsSearchedForWordsToCarry,
    LoopsWithNoBlockAheadToReadWordsIn,
    LoopsWithFencesSearchedForWords,
    WordsReadAndWrittenInLoops,
    WordsOnlyReadInLoops,
    WordsLeftForAddressThatChangesWithTurns,
    WordsLeftForAddressReadInLoop,
    WordsLeftForAddressNotMovedOutOfLoop,
    WordsLeftForWritesOnRarePaths,
    WordsLeftForWritesOnLikelyPaths,
    WordsLeftForStoresUnderOtherAddresses,
    WordsLeftThatMayNotBeTouched,
    WordsLeftThatAreSaidNeverToChange,
    WordsLeftBesideCallsOnLikelyPaths,
    WordsLeftToPhasesOfClient,
    StepsToCommonLikelyDominators,
};

static constexpr unsigned numberOfCounters = 35;
static_assert(static_cast<unsigned>(Counter::StepsToCommonLikelyDominators) + 1 == numberOfCounters);

struct LimitHit {
    uint32_t count { 0 };
    Origin firstOrigin { };
};

struct CompilationReport {
    std::optional<Failure> failure;
    Vector<Remark> remarks;
    std::array<LimitHit, numberOfLimits> limitHits { };
    std::array<uint32_t, numberOfCounters> counters { };

    uint32_t hitsOf(Limit limit) const { return limitHits[static_cast<size_t>(limit)].count; }
    Origin firstOriginOf(Limit limit) const { return limitHits[static_cast<size_t>(limit)].firstOrigin; }
    uint32_t countOf(Counter counter) const { return counters[static_cast<size_t>(counter)]; }
};

constexpr std::optional<Counter> counterOf(RemarkKind kind)
{
    switch (kind) {
    case RemarkKind::LoopVersioned:
        return Counter::LoopsVersioned;
    case RemarkKind::LoopRotated:
        return Counter::LoopsRotated;
    case RemarkKind::LoopVectorized:
        return Counter::LoopsVectorized;
    case RemarkKind::LoopUnrolled:
        return Counter::LoopsUnrolled;
    case RemarkKind::RangeChecksRemoved:
        return Counter::RangeChecksRemoved;
    case RemarkKind::LoopNotVersioned:
    case RemarkKind::LoopBoundNotClamped:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
