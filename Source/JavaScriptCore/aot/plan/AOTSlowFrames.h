/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTRepresentation.h"

namespace JSC { namespace AOT { namespace Typed {

// The frame of a slow body that can be entered at every operation: each input value has a word of its own below the frame pointer, always boxed, and between two operations nothing lives in a register.
// Whoever leaves a fast body and whoever emits the slow one ask here, and nobody else knows: a coloring by last uses changes these two alone.

// Words below the frame pointer. The first word is 1.
inline uint32_t slotInSlowFrame(const Function&, ValueID value)
{
    return value.index() + 1;
}

// Below the frame record. A multiple of 2.
inline uint32_t wordsOfSlowFrame(const Function& function)
{
    return (function.body.numberOfValues() + 1) & ~1u;
}

// A value crosses in the form that it has where the routine boxes that form by arithmetic alone: formInLeaveRowOf() of codegen/AOTLeaveRowsOfCode.h, which the emitter holds against this.
// Every other form is boxed in the block that leaves: its boxing hangs on the type, or allocates. A raw word has no boxed form.
constexpr bool crossesInItsForm(Representation representation)
{
    switch (representation) {
    case Representation::JSValue:
    case Representation::Cell:
    case Representation::Int32:
    case Representation::Boolean:
    case Representation::Double:
    case Representation::Int64:
        return true;
    default:
        return false;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
