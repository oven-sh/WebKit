/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet testsThatProveForStretch = []() consteval {
    OpcodeSet opcodes;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        if (provesForStretch(provesOf(static_cast<Opcode>(index))))
            opcodes.add(static_cast<Opcode>(index));
    }
    return opcodes;
}();

// EliminateGuards folds branches on these tests and tidies lists of guards. A body that holds none of them gives it nothing to do.
inline constexpr OpcodeSet opcodesOfGuardElimination = testsThatProveForStretch | OpcodeSet { Opcode::BelongsToInstance, Opcode::AllGuards };

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
