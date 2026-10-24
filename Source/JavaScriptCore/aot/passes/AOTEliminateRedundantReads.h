/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIR.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr std::array accessedHeaps { AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields, AbstractHeap::ObjectFields };

inline constexpr AbstractHeapSet accessedHeapSet = [] {
    AbstractHeapSet heaps;
    for (AbstractHeap heap : accessedHeaps)
        heaps.add(heap);
    return heaps;
}();

// What memoryAccessOf() may take for a read of one of the heaps. EliminateRedundantReads has nothing to do in a function without any of these.
inline constexpr OpcodeSet opcodesReadingAccessedHeaps = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (resultRuleOf(opcode) == ResultRule::Declared && effectRuleOf(opcode) == EffectRule::Declared && declaredBoundOf(opcode).reads.containsAny(accessedHeapSet))
            opcodes.add(opcode);
    }
    return opcodes;
}();

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
