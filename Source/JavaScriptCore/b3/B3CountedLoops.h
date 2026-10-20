/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3NaturalLoops.h"
#include "B3Opcode.h"
#include "B3Type.h"
#include <expected>
#include <wtf/Int128.h>
#include <wtf/ScopedLambda.h>
#include <wtf/TriState.h>

namespace JSC { namespace B3 {

class PhiChildren;

struct AffineForm {
    int64_t offset { 0 };
    IntegerInterval exactBaseRange;
};

struct LoopTest {
    BasicBlock* block { nullptr };
    Value* condition { nullptr };
};

struct CountedLoop {
    BasicBlock* preHeader { nullptr };
    BasicBlock* header { nullptr };
    BasicBlock* test { nullptr };
    unsigned latchSuccessorIndex { 0 };
    BasicBlock* latch { nullptr };
    bool isTopTested { false };
    Opcode relation { Oops };
    Value* inductionVariable { nullptr };
    Value* start { nullptr };
    Value* next { nullptr };
    int64_t step { 0 };
    Value* bound { nullptr };
    Int128 farthestSafeBound { 0 };
    bool boundIsSafe { false };
    bool counterRangeIsSafe { false };
    bool relationHoldsOnEntry { false };

    Value* testedValue() const { return isTopTested ? inductionVariable : next; }
    Type type() const;
    Signedness signedness() const { return isUnsigned() ? Signedness::Unsigned : Signedness::Signed; }
    bool cannotWrap() const { return boundIsSafe || counterRangeIsSafe; }
    bool isSafeBound(const IntegerInterval& boundInterval) const { return increases() ? boundInterval.max <= farthestSafeBound : boundInterval.min >= farthestSafeBound; }
    bool increases() const { return isLessRelation(relation); }
    bool isUnsigned() const { return isUnsignedRelation(relation); }
    bool includesEqual() const { return relation == LessEqual || relation == GreaterEqual || relation == BelowEqual || relation == AboveEqual; }
};

struct CountedLoopInputs {
    Procedure& proc;
    NaturalLoops& loops;
    const NaturalLoop& loop;
    PhiChildren& phiChildren;
    const ScopedLambda<bool(Value*)>& isInvariant;
    std::optional<LoopTest> test;
};

JS_EXPORT_PRIVATE std::expected<AffineForm, GaveUpReason> affineFormOf(const Procedure&, Value*, Value* base, Signedness);

JS_EXPORT_PRIVATE std::expected<CountedLoop, GaveUpReason> analyzeCountedLoop(const CountedLoopInputs&);

JS_EXPORT_PRIVATE bool relationHoldsOnEntry(Procedure&, const CountedLoop&, Value* boundOnEntry);

TriState evaluateComparisonWithBound(const CountedLoop&, Opcode, Value* left, Value* right);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
