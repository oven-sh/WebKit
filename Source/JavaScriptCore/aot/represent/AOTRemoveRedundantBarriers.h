/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

// An allocation runs one where it takes the cell of an inner object.
inline constexpr OpcodeSet operationsThatMayRunBarrier = [] {
    OpcodeSet opcodes { Opcode::NewObject };
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (runsOwnerBarrierOf(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
