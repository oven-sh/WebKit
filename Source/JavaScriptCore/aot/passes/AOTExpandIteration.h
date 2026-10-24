/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ExpansionStep : uint8_t {
    ForInPositions = 1 << 0,
    ArrayIteratorSteps = 1 << 1,
    Spreads = 1 << 2,
    Iterations = 1 << 3,
};

constexpr OptionSet<ExpansionStep> stepThatExpands(Opcode opcode)
{
    switch (opcode) {
    case Opcode::ForInStep:
        return ExpansionStep::ForInPositions;
    case Opcode::StepArrayIterator:
        return ExpansionStep::ArrayIteratorSteps;
    case Opcode::AppendSpread:
        return ExpansionStep::Spreads;
    case Opcode::IteratorOpen:
    case Opcode::IteratorResume:
    case Opcode::IteratorClose:
        return ExpansionStep::Iterations;
    default:
        return { };
    }
}

constexpr OpcodeSet opcodesExpandedIn(OptionSet<ExpansionStep> steps)
{
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (stepThatExpands(opcode).containsAny(steps))
            opcodes.add(opcode);
    }
    return opcodes;
}

// ExpandIteration has nothing to do in a function without any of the first, ExpandProvenIterations in one without any of the second.
inline constexpr OpcodeSet expandedOpcodes = opcodesExpandedIn({ ExpansionStep::ForInPositions, ExpansionStep::ArrayIteratorSteps, ExpansionStep::Spreads, ExpansionStep::Iterations });
inline constexpr OpcodeSet iterationOpcodes = opcodesExpandedIn(ExpansionStep::Iterations);

enum class OutlinedIteration : uint32_t { AppendIterable = 1, OpenIterator, CloseIterator, CloseIteratorAfterThrow, OpenWrappedIterator, OpenStringOrIterator, StepString, CloseStringOrIterator, CloseStringOrIteratorAfterThrow };

bool isOutlinedIteration(const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
