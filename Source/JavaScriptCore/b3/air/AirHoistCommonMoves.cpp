/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirHoistCommonMoves.h"

#if ENABLE(B3_JIT)

#include "AirCode.h"
#include "AirInstInlines.h"
#include "AirOpcodeUtils.h"
#include "AirPhaseScope.h"
#include "AirRegLiveness.h"
#include <wtf/BitVector.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

bool isMoveToRegister(const Inst& inst)
{
    switch (inst.kind.opcode) {
    case Move:
    case Move32:
    case MoveFloat:
    case MoveDouble:
        return inst.args().size() == 2 && inst.args()[1].isReg() && (inst.args()[0].isReg() || inst.args()[0].isZeroReg());
    default:
        return false;
    }
}

// A block on the back edge from a loop's bottom test to its header that holds only moves inserted by the register allocator.
struct BackEdgeBlock {
    BasicBlock* latch;
    BasicBlock* block;
    BasicBlock* exit;
};

std::optional<BackEdgeBlock> findBackEdgeBlock(Code& code, BasicBlock* block)
{
    if (block->size() < 2 || block->last().kind.opcode != Jump || block->numPredecessors() != 1 || code.isEntrypoint(block))
        return std::nullopt;
    BasicBlock* latch = block->predecessor(0);
    if (latch == block || latch->numSuccessors() != 2 || latch->last().kind.opcode == Patch || latch->successorBlock(0) == latch->successorBlock(1))
        return std::nullopt;
    unsigned successorIndex = latch->successorBlock(0) == block ? 0 : 1;
    BasicBlock* exit = latch->successorBlock(1 - successorIndex);
    if (!latch->successor(successorIndex).isLikely() || 2 * block->frequency() <= latch->frequency())
        return std::nullopt;
    for (unsigned index = 0; index + 1 < block->size(); ++index) {
        if (!isMoveToRegister(block->at(index)) || (usesFlags(latch->last().kind.opcode) && !preservesFlags(block->at(index).kind.opcode)))
            return std::nullopt;
    }
    return BackEdgeBlock { latch, block, exit };
}

// A candidate's jump is a back edge if its target is still on the stack of a depth-first search from the entry.
BitVector findBlocksEndingInBackEdge(Code& code, const Vector<BackEdgeBlock, 4>& candidates)
{
    struct Visit {
        BasicBlock* block;
        unsigned nextSuccessor;
    };
    BitVector isCandidate(code.size());
    for (const BackEdgeBlock& candidate : candidates)
        isCandidate.quickSet(candidate.block->index());
    BitVector isVisited(code.size());
    BitVector isOnStack(code.size());
    BitVector result(code.size());
    Vector<Visit, 32> stack;
    auto enter = [&](BasicBlock* block) {
        if (isVisited.quickSet(block->index()))
            return;
        isOnStack.quickSet(block->index());
        stack.append({ block, 0 });
    };
    for (BasicBlock* root : code) {
        if (root->numPredecessors() && root != code[0] && !code.isEntrypoint(root))
            continue;
        enter(root);
        while (!stack.isEmpty()) {
            Visit& visit = stack.last();
            if (visit.nextSuccessor == visit.block->numSuccessors()) {
                isOnStack.quickClear(visit.block->index());
                stack.removeLast();
                continue;
            }
            BasicBlock* block = visit.block;
            BasicBlock* successor = block->successorBlock(visit.nextSuccessor++);
            if (isCandidate.quickGet(block->index()) && isOnStack.quickGet(successor->index()))
                result.quickSet(block->index());
            enter(successor);
        }
    }
    return result;
}

// Hoists the moves above the latch's branch, leaving only the jump in the back edge block. The moves now also run when the loop exits, so they must not clobber anything live at the exit.
bool hoistMovesFromBackEdgeBlocks(Code& code)
{
    Vector<BackEdgeBlock, 4> candidates;
    for (BasicBlock* block : code) {
        if (std::optional<BackEdgeBlock> candidate = findBackEdgeBlock(code, block))
            candidates.append(*candidate);
    }
    if (candidates.isEmpty())
        return false;
    BitVector endsInBackEdge = findBlocksEndingInBackEdge(code, candidates);
    if (endsInBackEdge.isEmpty())
        return false;

    // Liveness is not recomputed. A hoist only invalidates it at the head of the emptied block, which is only consulted when hoisting from the same latch's other successor, so each latch is used once.
    RegLiveness liveness(code);
    BitVector didHoistIntoLatch(code.size());
    bool changed = false;
    for (const BackEdgeBlock& candidate : candidates) {
        auto [latch, block, exit] = candidate;
        if (!endsInBackEdge.quickGet(block->index()) || didHoistIntoLatch.quickGet(latch->index()))
            continue;
        const RegisterSet& liveAtExit = liveness.liveAtHead(exit);
        bool clobbersLiveValue = false;
        for (unsigned index = 0; index + 1 < block->size(); ++index) {
            Reg destination = block->at(index).args()[1].reg();
            clobbersLiveValue |= liveAtExit.contains(destination, IgnoreVectors);
            latch->last().forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                clobbersLiveValue |= tmp.reg() == destination || (Arg::isAnyDef(role) && block->at(index).args()[0].isReg() && tmp.reg() == block->at(index).args()[0].reg());
            });
        }
        if (clobbersLiveValue)
            continue;
        Inst branch = latch->insts().takeLast();
        Inst jump = block->insts().takeLast();
        latch->insts().appendVector(WTF::move(block->insts()));
        latch->insts().append(WTF::move(branch));
        block->insts().shrink(0);
        block->insts().append(WTF::move(jump));
        didHoistIntoLatch.quickSet(latch->index());
        changed = true;
    }
    return changed;
}

} // anonymous namespace

bool hoistCommonMoves(Code& code)
{
    PhaseScope phaseScope(code, "hoistCommonMoves"_s);

    bool changed = false;
    for (BasicBlock* block : code) {
        if (block->numSuccessors() != 2 || block->last().kind.opcode == Patch)
            continue;
        BasicBlock* taken = block->successorBlock(0);
        BasicBlock* notTaken = block->successorBlock(1);
        if (taken == notTaken || taken == block || notTaken == block || taken->numPredecessors() != 1 || notTaken->numPredecessors() != 1 || code.isEntrypoint(taken) || code.isEntrypoint(notTaken))
            continue;
        while (taken->size() > 1 && notTaken->size() > 1) {
            Inst move = taken->at(0);
            Inst& other = notTaken->at(0);
            if (!isMoveToRegister(move) || move.kind != other.kind || other.args().size() != 2 || move.args()[0] != other.args()[0] || move.args()[1] != other.args()[1])
                break;
            if (usesFlags(block->last().kind.opcode) && !preservesFlags(move.kind.opcode))
                break;
            bool mattersToBranch = false;
            block->last().forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                mattersToBranch |= tmp == move.args()[1].tmp() || (Arg::isAnyDef(role) && move.args()[0].isTmp() && tmp == move.args()[0].tmp());
            });
            if (mattersToBranch)
                break;
            block->insts().insert(block->size() - 1, WTF::move(move));
            taken->insts().removeAt(0);
            notTaken->insts().removeAt(0);
            changed = true;
        }
    }
    changed |= hoistMovesFromBackEdgeBlocks(code);
    return changed;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
