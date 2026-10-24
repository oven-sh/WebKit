/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

// SimplifyCasts has nothing to do in a function without any of these.
inline constexpr OpcodeSet simplifiableCasts = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (isCast(opcode) && opcode != Opcode::FinishObject)
            opcodes.add(opcode);
    }
    return opcodes;
}();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
