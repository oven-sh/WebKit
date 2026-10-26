/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "testb3_bun.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInstInlines.h"

inline Policy selectionPolicy()
{
    return policyWith([](Policy& policy) {
        policy.selection.extendsInstructionSelection = true;
    });
}

template<typename Functor>
inline unsigned numberOfInsts(Procedure& proc, const Functor& matches)
{
    unsigned result = 0;
    for (Air::BasicBlock* block : proc.code()) {
        for (Air::Inst& inst : *block)
            result += matches(inst);
    }
    return result;
}

inline unsigned numberOfInsts(Procedure& proc, Air::Opcode opcode)
{
    return numberOfInsts(proc, [&](Air::Inst& inst) {
        return inst.kind.opcode == opcode;
    });
}

inline unsigned numberOfInsts(Procedure& proc)
{
    return numberOfInsts(proc, [](Air::Inst&) {
        return true;
    });
}

#endif // ENABLE(B3_JIT)
