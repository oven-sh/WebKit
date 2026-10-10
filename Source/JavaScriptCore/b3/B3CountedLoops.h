/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3NaturalLoops.h"
#include "B3Opcode.h"
#include <wtf/HashMap.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class UpsilonValue;

// An innermost loop with one latch that is tested at the bottom: it goes on while relation(next, bound) holds.
struct CountedLoop {
    BasicBlock* preHeader { nullptr };
    BasicBlock* header { nullptr };
    // Ends in the loop test. One successor leaves the loop, the other is the latch.
    BasicBlock* test { nullptr };
    unsigned indexOfLatchInSuccessors { 0 };
    BasicBlock* latch { nullptr };
    // One of the eight orderings of Int64.
    Opcode relation { Oops };
    // A Phi of the header: `start` from the pre-header, `next` from the latch.
    Value* inductionVariable { nullptr };
    Value* start { nullptr };
    // Add(inductionVariable, step), SExt32(Trunc()) of that, or a Phi of that and the constant it saturates at.
    Value* next { nullptr };
    int64_t step { 0 };
    // `(i + step) | 0`: then it is not monotonic: past one end of int32 it goes on from the other.
    bool wrapsAroundInt32 { false };
    std::optional<int64_t> saturation;
    // Loop invariant.
    Value* bound { nullptr };

    bool increases() const { return relation == LessThan || relation == LessEqual || relation == Below || relation == BelowEqual; }
    bool isUnsigned() const { return relation == Below || relation == BelowEqual || relation == Above || relation == AboveEqual; }
    bool admitsEqual() const { return relation == LessEqual || relation == GreaterEqual || relation == BelowEqual || relation == AboveEqual; }
};

static constexpr int64_t maxIterationsPerStrip = 1 << 16;

using UpsilonsByPhi = UncheckedKeyHashMap<Value*, Vector<UpsilonValue*, 2>>;
UpsilonsByPhi collectUpsilonsByPhi(Procedure&);

// The owners of values are up to date, and the header has one predecessor outside the loop.
std::optional<CountedLoop> analyzeCountedLoop(NaturalLoops&, const NaturalLoop&, const UpsilonsByPhi&, const ScopedLambda<bool(Value*)>& isInvariant);

// For strip mining. Where relation(from, bound) holds: at most maxIterationsPerStrip from there, not beyond the bound, and on this side of what the variable saturates at.
// With `relationMayNotHold`: where it does not hold, an end that no value of the variable is on this side of. Appended to the block, in front of its terminal if it has one.
Value* appendEndOfStrip(Procedure&, BasicBlock*, const CountedLoop&, Value* from, Value* bound, bool relationMayNotHold);

// Whether the pre-header is only reached by way of a Branch on relation(start, bound): the guard that rotateLoops() leaves.
bool relationHoldsOnEntry(const CountedLoop&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
