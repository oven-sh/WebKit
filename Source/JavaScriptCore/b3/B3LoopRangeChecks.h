/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3AffineIndex.h"
#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3NaturalLoops.h"
#include "B3Origin.h"
#include "B3Value.h"
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class FastPreHeader;
class LoopInvariance;
class MemoryValue;
class PhiChildren;
class Variable;
struct CountedLoop;
struct HotRegion;

struct RangeCheck {
    Value* index { nullptr };
    Value* limit { nullptr };
    bool isTrueInBounds { false };
};

std::optional<RangeCheck> rangeCheckFor(Value* comparison);

struct RangeCheckBound {
    Value* limit { nullptr };
    int64_t minOffset { 0 };
    int64_t maxOffset { 0 };
};

// relation(left, right + addend), as integers of 64 bits. Without a right side: relation(left, addend).
struct EntryGuard {
    Opcode relation { Oops };
    Value* left { nullptr };
    Signedness leftSignedness { Signedness::Signed };
    Value* right { nullptr };
    Signedness rightSignedness { Signedness::Signed };
    int64_t addend { 0 };
};

struct StridedGuards {
    int64_t minOffset { 0 };
    int64_t maxOffset { 0 };
    bool mustFitInt32 { false };
    bool isStrengthReduced { false };
    bool isIncreasing { false };
    bool needsScaleGuard { false };
    bool needsLowerGuard { false };
    bool needsLimitGuard { false };
};

// Checks of index + k against one limit, for constants k in [minOffset, maxOffset].
struct StridedRangeCheck : StridedGuards {
    AffineIndex index;
    Value* limit { nullptr };
};

// The value equals the index of checks[checkIndex] plus offset.
struct ReducedIndex {
    Value* value { nullptr };
    unsigned checkIndex { 0 };
    int64_t offset { 0 };
};

struct StridedRangeChecks {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StridedRangeChecks);

    std::array<StridedRangeCheck, Limits::maxStridedRangeChecksPerLoop> checks { };
    unsigned numberOfChecks { 0 };
    // In the order of the region's blocks and of the values in each.
    std::array<ReducedIndex, Limits::maxReducedIndicesPerLoop> reducedIndices { };
    unsigned numberOfReducedIndices { 0 };
    // Where the reduced indices advance. Null: on each back edge.
    BasicBlock* indexStepBlock { nullptr };
};

struct StridedIndexVariable {
    Variable* variable { nullptr };
    Value* stride { nullptr };
};
using StridedIndexVariables = std::array<StridedIndexVariable, Limits::maxStridedRangeChecksPerLoop>;

struct RangeCheckPlan {
    UncheckedKeyHashMap<Value*, bool> decidedComparisons;
    Vector<RangeCheckBound, 4> bounds;
    std::unique_ptr<StridedRangeChecks> strided;
    Vector<EntryGuard, 8> entryGuards;
    // What is known of the values that enter the loop, and has to be asked in front of a later turn.
    Vector<EntryGuard, 1> entryGuardsOfLaterTurns;
    bool relationHoldsOnEntry { false };
    // The guards name the counters and what the loop computes again, and nothing that was computed in front of the loop in their place.
    bool holdsOnAnyTurn { false };

    bool clampsBound() const { return !bounds.isEmpty(); }
    bool dependsOnLoopTest() const { return clampsBound() || strided; }
};

struct RangeCheckInputs {
    Procedure& proc;
    PhiChildren& phiChildren;
    const HotRegion& region;
    const LoopInvariance& invariance;
    const CountedLoop& loop;
    const IndexSet<BasicBlock*>& blocksBeforeTopTest;
    bool canClampBound;
};

RangeCheckPlan planRangeChecks(const RangeCheckInputs&);

struct MeetingCountersInputs {
    Procedure& proc;
    NaturalLoops& loops;
    PhiChildren& phiChildren;
    const HotRegion& region;
    const LoopInvariance& invariance;
    bool isEnteredOnAnyTurn;
};

// For a loop that goes on while a counter that rises is below one that falls. Neither leaves the stretch between where the two started.
RangeCheckPlan planRangeChecksOfMeetingCounters(const MeetingCountersInputs&);
void appendEntryGuards(FastPreHeader&, Origin, std::span<const EntryGuard>, const ScopedLambda<Value*(Value*)>& valueInPreHeader);
void appendStridedRangeCheckGuards(FastPreHeader&, const StridedRangeChecks&, const CountedLoop&, const ScopedLambda<Value*(Value*)>& valueInPreHeader, StridedIndexVariables&);

inline int64_t maxLimitForRangeChecks(Type type)
{
    return type == Int32 ? std::numeric_limits<int32_t>::max() : Limits::maxLoopBoundForRangeChecks;
}

struct FastLoopBounds {
    Value* bound { nullptr };
    Value* limit { nullptr };
};

FastLoopBounds appendEntryGuards(FastPreHeader&, const RangeCheckPlan&, const CountedLoop&, const ScopedLambda<Value*(Value*)>& valueInPreHeader);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
