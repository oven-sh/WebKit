/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3NaturalLoops.h"
#include "B3Origin.h"
#include "B3Value.h"
#include <limits>
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

struct RangeCheckPlan {
    UncheckedKeyHashMap<Value*, bool> decidedComparisons;
    Vector<RangeCheckBound, 4> bounds;
    Vector<EntryGuard, 8> entryGuards;
    bool relationHoldsOnEntry { false };
    // The number of turns is at most each limit, and at most the bound of a counter that starts at zero or above. One of these is the length of a view, of the characters of a string
    // or of an array, or a small number: any other number is the program's to choose.
    bool endsWithinData { false };
    // The length of an array may be set to more than the array holds. Where this is the only length that bounds the turns, the fast version is for an array that has room for as many.
    MemoryValue* lengthThatArrayMustHold { nullptr };

    bool clampsBound() const { return !bounds.isEmpty(); }
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
};

// For a loop that goes on while a counter that rises is below one that falls. Neither leaves the stretch between where the two started.
RangeCheckPlan planRangeChecksOfMeetingCounters(const MeetingCountersInputs&);
void appendEntryGuards(FastPreHeader&, Origin, std::span<const EntryGuard>, const ScopedLambda<Value*(Value*)>& valueInPreHeader);
void appendGuardThatArrayHoldsLength(FastPreHeader&, Origin, MemoryValue* length, const ScopedLambda<Value*(Value*)>& valueInPreHeader);

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
