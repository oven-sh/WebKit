/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3SimplifyTerminals.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3SwitchValue.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

static FrequentedBlock successorForCondition(BasicBlock* block, int64_t condition)
{
    if (block->last()->opcode() == Branch)
        return condition ? block->taken() : block->notTaken();
    SwitchValue* switchValue = block->last()->as<SwitchValue>();
    for (unsigned i = 0; i < switchValue->numCaseValues(); ++i) {
        if (switchValue->caseValue(i) == condition)
            return block->successor(i);
    }
    return block->fallThrough();
}

bool simplifyTerminals(Procedure& proc)
{
    PhaseScope phaseScope(proc, "simplifyTerminals"_s);
    bool changed = false;
    for (BasicBlock* block : proc) {
        Value* terminal = block->last();
        if (terminal->opcode() != Branch && terminal->opcode() != Switch)
            continue;
        Value* condition = terminal->child(0)->foldIdentity();
        if (!condition->hasInt())
            continue;
        terminal->replaceWithJump(block, successorForCondition(block, condition->asInt()));
        changed = true;
    }
    if (changed) {
        proc.resetReachability();
        proc.invalidateCFG();
    }
    return changed;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
