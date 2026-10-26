/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3CheckVariablesAreSet.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "B3VariableValue.h"
#include <wtf/BitVector.h>

namespace JSC { namespace B3 {

bool checkVariablesAreSet(Procedure& proc)
{
    PhaseScope phaseScope(proc, "checkVariablesAreSet"_s);
    if (!proc.variables().size())
        return true;
    auto indexOf = [](Value* value) {
        return value->as<VariableValue>()->variable()->index();
    };
    Vector<BitVector> setAtHead(proc.size());
    BitVector isReached;
    isReached.set(proc.at(0)->index());
    Vector<BasicBlock*, 16> worklist { proc.at(0) };
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        BitVector isSet = setAtHead[block->index()];
        for (Value* value : *block) {
            if (value->opcode() == Set)
                isSet.set(indexOf(value));
        }
        for (BasicBlock* successor : block->successorBlocks()) {
            BitVector& atHead = setAtHead[successor->index()];
            if (!isReached.get(successor->index())) {
                isReached.set(successor->index());
                atHead = isSet;
                worklist.append(successor);
                continue;
            }
            BitVector onAllPaths = atHead;
            onAllPaths.filter(isSet);
            if (onAllPaths == atHead)
                continue;
            atHead = WTF::move(onAllPaths);
            worklist.append(successor);
        }
    }
    for (BasicBlock* block : proc) {
        if (!isReached.get(block->index()))
            continue;
        BitVector isSet = setAtHead[block->index()];
        for (Value* value : *block) {
            if (value->opcode() == Set)
                isSet.set(indexOf(value));
            else if (value->opcode() == Get && !isSet.get(indexOf(value))) {
                proc.fail(FailureKind::VariableReadBeforeSet, value->origin());
                return false;
            }
        }
    }
    return true;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
