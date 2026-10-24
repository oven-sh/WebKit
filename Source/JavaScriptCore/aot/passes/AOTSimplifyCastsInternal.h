/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet repeatableCasts { Opcode::Check, Opcode::CheckInitialized, Opcode::RequireObjectCoercible, Opcode::RequireObject };

constexpr bool isSimplifiable(Opcode opcode)
{
    return isCast(opcode) && opcode != Opcode::FinishObject;
}

// SimplifyCasts has nothing to do in a function without any of these.
inline constexpr OpcodeSet simplifiableCasts = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (isSimplifiable(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

static_assert((simplifiableCasts | repeatableCasts) == simplifiableCasts);
static_assert(simplifiableCasts.contains(Opcode::AssumeType));

size_t moveCastsToEdges(FunctionPassContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
