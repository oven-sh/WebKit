/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

// A branch that may lead into the generic version tests a type, or something that holds for a stretch only.
inline constexpr OpcodeSet testsThatMayRestartInGenericVersion = [] {
    OpcodeSet opcodes { Opcode::TypeTest };
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (provesForStretch(provesOf(opcode)))
            opcodes.add(opcode);
    }
    return opcodes;
}();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
