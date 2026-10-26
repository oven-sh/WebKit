/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Limits.h"
#include "B3Value.h"
#include <limits>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/IndexSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class FastPreHeader;
class LoopInvariance;
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

struct RangeCheckPlan {
    UncheckedKeyHashMap<Value*, bool> decidedComparisons;
    Vector<RangeCheckBound, 4> bounds;
    bool relationHoldsOnEntry { false };

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
