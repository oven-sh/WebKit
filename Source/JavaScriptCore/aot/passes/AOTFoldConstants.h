/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

// A parameter, a constant and a cast stay what they are, and so does a test whose proof lasts for a stretch.
inline constexpr OpcodeSet foldableOpcodes = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (opcode != Opcode::Parameter && resultRuleOf(opcode) != ResultRule::Constant && !isCast(opcode) && !provesForStretch(provesOf(opcode)))
            opcodes.add(opcode);
    }
    return opcodes;
}();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
