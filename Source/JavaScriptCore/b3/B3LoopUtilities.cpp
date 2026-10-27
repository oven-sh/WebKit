/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3LoopUtilities.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"

namespace JSC { namespace B3 {

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
