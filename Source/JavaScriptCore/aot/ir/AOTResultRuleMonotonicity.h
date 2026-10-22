/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include <optional>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// Two laws of the result rules.
// Narrowing an operand may never widen a result. A pass narrows an operand wherever it learns something: behind a test, on its failed side, in a copy.
// A rule that then answers with more takes the ground from under everything that was resolved by the narrower result.
// And computeResultTypeID(), which the propagation of types asks, answers what computeResultType() answers, which the verifier and the passes ask.
struct BrokenResultRule {
    enum class Law : uint8_t { NarrowerOperandGivesNarrowerResult, BothPathsAgree };

    Law law { Law::NarrowerOperandGivesNarrowerResult };
    ValueID value;
    uint32_t operandIndex { 0 };
    // For the second law: the type of the operand, what computeResultTypeID() answers and what computeResultType() answers.
    Type narrowOperand;
    Type resultOfNarrowOperand;
    Type wideOperand;
    Type resultOfWideOperand;

    void dump(PrintStream&, const Program&, const Function&) const;
};

// The operations with these operand types that have been looked at, in the ids of one arena.
using ShapesWithMonotoneResult = HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

// Types each operand of each operation in turn as parts of its type, and as any value where the operand may be one, and compares what the result rule answers.
// The function is as before when this returns.
std::optional<BrokenResultRule> findBrokenResultRule(const Program&, Function&, TypeArena&, ShapesWithMonotoneResult&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
