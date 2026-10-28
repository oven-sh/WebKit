/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3LoopUtilities.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3Limits.h"
#include "B3ProcedureInlines.h"

namespace JSC { namespace B3 {

bool canPollBetweenStretches(const Procedure& proc, const Value* value)
{
    if (!proc.policy().loops.pollsBetweenStretches || !isLoopPoll(value))
        return false;
    return value->as<PatchpointValue>()->turnsAreShort;
}

Value* appendBoundOfStretch(Procedure& proc, BasicBlock* block, Origin origin, const StretchOfTurns& stretch)
{
    Type type = stretch.from->type();
    bool rises = stretch.step > 0;
    // Where the bound lies beyond, the distance is no more than the type holds without a sign. The end is taken only where it lies nearer, so there it has not wrapped around.
    Value* distance = rises ? block->appendNew<Value>(proc, Sub, origin, stretch.bound, stretch.from) : block->appendNew<Value>(proc, Sub, origin, stretch.from, stretch.bound);
    Value* length = block->appendIntConstant(proc, origin, type, std::min<int64_t>(stretch.turns * (rises ? stretch.step : -stretch.step), type == Int32 ? 1ll << 30 : 1ll << 62));
    Value* end = block->appendNew<Value>(proc, rises ? Add : Sub, origin, stretch.from, length);
    Value* endIsNearer = block->appendNew<Value>(proc, Above, origin, distance, length);
    if (stretch.boundLiesBeyond == BoundLiesBeyond::No)
        endIsNearer = block->appendNew<Value>(proc, BitAnd, origin, endIsNearer, block->appendNew<Value>(proc, stretch.relation, origin, stretch.from, stretch.bound));
    return block->appendNew<Value>(proc, Select, origin, endIsNearer, end, stretch.bound);
}

BasicBlock* findPreHeader(const NaturalLoops& loops, const NaturalLoop& loop)
{
    BasicBlock* preHeader = nullptr;
    for (BasicBlock* predecessor : loop.header()->predecessors()) {
        if (loops.belongsTo(predecessor, loop))
            continue;
        if (preHeader)
            return nullptr;
        preHeader = predecessor;
    }
    if (!preHeader || preHeader->last()->opcode() != Jump)
        return nullptr;
    return preHeader;
}

IndexSet<BasicBlock*> findLikelyBlocks(const NaturalLoops& loops, const NaturalLoop& loop)
{
    IndexSet<BasicBlock*> result;
    Vector<BasicBlock*, 16> worklist { loop.header() };
    result.add(loop.header());
    while (!worklist.isEmpty()) {
        BasicBlock* block = worklist.takeLast();
        FrequencyClass likeliestClass = likeliestSuccessorClass(block);
        for (const FrequentedBlock& successor : block->successors()) {
            if (!isLikelySuccessor(likeliestClass, successor) || !loops.belongsTo(successor.block(), loop))
                continue;
            if (result.add(successor.block()))
                worklist.append(successor.block());
        }
    }
    return result;
}

bool hasLikelyBackEdge(const NaturalLoop& loop, const IndexSet<BasicBlock*>& likelyBlocks)
{
    for (BasicBlock* predecessor : loop.header()->predecessors()) {
        if (!likelyBlocks.contains(predecessor))
            continue;
        for (const FrequentedBlock& successor : predecessor->successors()) {
            if (successor.block() == loop.header() && isLikelySuccessor(predecessor, successor))
                return true;
        }
    }
    return false;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
