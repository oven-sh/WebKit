/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3SplitBranchConditions.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3CompareChains.h"
#include "B3PhaseScope.h"
#include "B3ProcedureInlines.h"
#include "B3UseCounts.h"
#include "B3ValueInlines.h"
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace B3 {

static bool isOneBitTest(const Value* value)
{
    if ((value->opcode() != Equal && value->opcode() != NotEqual) || !value->child(1)->isInt(0))
        return false;
    const Value* masked = value->child(0);
    return masked->opcode() == BitAnd && masked->child(1)->hasInt() && hasOneBitSet(static_cast<uint64_t>(masked->child(1)->asInt()));
}

static bool isEqualityWithConstant(Value* value)
{
    return (value->opcode() == Equal || value->opcode() == NotEqual) && value->child(1)->hasInt();
}

static bool canSplit(const Procedure& proc, BasicBlock* block, const UseCountsWithoutUsingInstructions& useCounts)
{
    Value* branch = block->last();
    if (branch->opcode() != Branch)
        return false;
    Value* condition = branch->child(0);
    if ((condition->opcode() != BitAnd && condition->opcode() != BitOr) || condition->owner != block || useCounts.numUses(condition) != 1)
        return false;
    Value* first = condition->child(0);
    Value* second = condition->child(1);
    if (!first->returnsBool() || !second->returnsBool() || first->isConstant() || second->isConstant() || first == second)
        return false;
    if (useCounts.numUses(first) != 1 || useCounts.numUses(second) != 1)
        return false;
    bool isCompareChain = isSelectedAsCompareChain(proc, condition, [&](Value* operand) {
        return useCounts.numUses(operand) == 1 && !isOneBitTest(operand);
    });
    if (isCompareChain)
        return false;
    if (isEqualityWithConstant(first) && isEqualityWithConstant(second) && first->child(0) == second->child(0))
        return false;
    return true;
}

static BasicBlock* split(Procedure& proc, BasicBlock* block, const UseCountsWithoutUsingInstructions& useCounts)
{
    Value* branch = block->last();
    Value* condition = branch->child(0);
    Value* first = condition->child(0);
    Value* second = condition->child(1);

    UncheckedKeyHashMap<Value*, unsigned> sinkableUses;
    sinkableUses.add(second, 1);
    Vector<Value*, 8> sunk;
    bool crossesWrite = false;
    auto& values = block->values();
    for (unsigned i = values.size() - 1; i--;) {
        Value* value = values[i];
        if (value == condition)
            continue;
        Effects effects = value->effects();
        auto found = sinkableUses.find(value);
        bool reads = effects.reads || effects.readsPinned;
        if (found != sinkableUses.end() && found->value == useCounts.numUses(value) && !effects.mustExecute() && !effects.readsLocalState && !(reads && crossesWrite)) {
            sunk.append(value);
            for (Value* child : value->children())
                sinkableUses.add(child, 0).iterator->value++;
            continue;
        }
        crossesWrite |= effects.isWrite();
    }

    FrequentedBlock taken = block->taken();
    FrequentedBlock notTaken = block->notTaken();
    BasicBlock* next = proc.addBlock();
    FrequentedBlock toNext(next, maxFrequency(taken.frequency(), notTaken.frequency()));
    for (unsigned i = sunk.size(); i--;)
        next->append(sunk[i]);
    values.removeAllMatching([&](Value* value) {
        return value == condition || value->owner == next;
    });
    next->appendNew<Value>(proc, Branch, branch->origin(), second);
    next->setSuccessors(taken, notTaken);
    branch->child(0) = first;
    if (condition->opcode() == BitAnd)
        block->setSuccessors(toNext, notTaken);
    else
        block->setSuccessors(taken, toNext);
    proc.deleteValue(condition);
    return next;
}

bool splitBranchConditions(Procedure& proc)
{
    PhaseScope phaseScope(proc, "splitBranchConditions"_s);
    Vector<BasicBlock*> worklist;
    for (BasicBlock* block : proc) {
        Value* last = block->last();
        if (last->opcode() == Branch && (last->child(0)->opcode() == BitAnd || last->child(0)->opcode() == BitOr))
            worklist.append(block);
    }
    if (worklist.isEmpty())
        return false;
    proc.resetValueOwners();
    UseCountsWithoutUsingInstructions useCounts(proc);
    bool changed = false;
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        if (!canSplit(proc, block, useCounts))
            continue;
        worklist.append(split(proc, block, useCounts));
        worklist.append(block);
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
