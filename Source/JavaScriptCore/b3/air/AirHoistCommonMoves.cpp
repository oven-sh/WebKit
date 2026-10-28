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

bool isCopyIntoRegister(const Inst& inst)
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

// The block between the test at the bottom of a loop and the top of the loop, where the allocator has left copies in it.
struct WayBack {
    BasicBlock* latch;
    BasicBlock* block;
    BasicBlock* exit;
};

std::optional<WayBack> findWayBack(Code& code, BasicBlock* block)
{
    if (block->size() < 2 || block->last().kind.opcode != Jump || block->numPredecessors() != 1 || code.isEntrypoint(block))
        return std::nullopt;
    BasicBlock* latch = block->predecessor(0);
    if (latch == block || latch->numSuccessors() != 2 || latch->last().kind.opcode == Patch || latch->successorBlock(0) == latch->successorBlock(1))
        return std::nullopt;
    unsigned indexOfBlock = latch->successorBlock(0) == block ? 0 : 1;
    BasicBlock* exit = latch->successorBlock(1 - indexOfBlock);
    if (!latch->successor(indexOfBlock).isLikely() || 2 * block->frequency() <= latch->frequency())
        return std::nullopt;
    for (unsigned index = 0; index + 1 < block->size(); ++index) {
        if (!isCopyIntoRegister(block->at(index)) || (usesFlags(latch->last().kind.opcode) && !preservesFlags(block->at(index).kind.opcode)))
            return std::nullopt;
    }
    return WayBack { latch, block, exit };
}

// Whether each block is on the path from the entry on which a depth-first walk has found the block that is asked about.
BitVector findBlocksThatJumpToAncestor(Code& code, const Vector<WayBack, 4>& candidates)
{
    struct Visit {
        BasicBlock* block;
        unsigned nextSuccessor;
    };
    BitVector isCandidate(code.size());
    for (const WayBack& candidate : candidates)
        isCandidate.quickSet(candidate.block->index());
    BitVector isVisited(code.size());
    BitVector isOnPath(code.size());
    BitVector result(code.size());
    Vector<Visit, 32> path;
    auto enter = [&](BasicBlock* block) {
        if (isVisited.quickSet(block->index()))
            return;
        isOnPath.quickSet(block->index());
        path.append({ block, 0 });
    };
    for (BasicBlock* root : code) {
        if (root->numPredecessors() && root != code[0] && !code.isEntrypoint(root))
            continue;
        enter(root);
        while (!path.isEmpty()) {
            Visit& visit = path.last();
            if (visit.nextSuccessor == visit.block->numSuccessors()) {
                isOnPath.quickClear(visit.block->index());
                path.removeLast();
                continue;
            }
            BasicBlock* block = visit.block;
            BasicBlock* successor = block->successorBlock(visit.nextSuccessor++);
            if (isCandidate.quickGet(block->index()) && isOnPath.quickGet(successor->index()))
                result.quickSet(block->index());
            enter(successor);
        }
    }
    return result;
}

// `move; jump` behind the branch becomes `move` ahead of a branch that leads back itself. On the way out the copies are made in vain, so nothing there may read what they overwrite.
bool hoistCopiesOfWaysBack(Code& code)
{
    Vector<WayBack, 4> candidates;
    for (BasicBlock* block : code) {
        if (std::optional<WayBack> candidate = findWayBack(code, block))
            candidates.append(*candidate);
    }
    if (candidates.isEmpty())
        return false;
    BitVector jumpsToAncestor = findBlocksThatJumpToAncestor(code, candidates);
    if (jumpsToAncestor.isEmpty())
        return false;

    // What is live is asked once. Copies that have gone ahead of a test change it at the head of one block alone, the one they came from, and only the other side of the same test asks about that block.
    RegLiveness liveness(code);
    BitVector hasTakenCopies(code.size());
    bool changed = false;
    for (const WayBack& candidate : candidates) {
        auto [latch, block, exit] = candidate;
        if (!jumpsToAncestor.quickGet(block->index()) || hasTakenCopies.quickGet(latch->index()))
            continue;
        const RegisterSet& liveOnWayOut = liveness.liveAtHead(exit);
        bool isOverwrittenValueRead = false;
        for (unsigned index = 0; index + 1 < block->size(); ++index) {
            Reg destination = block->at(index).args()[1].reg();
            isOverwrittenValueRead |= liveOnWayOut.contains(destination, IgnoreVectors);
            latch->last().forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                isOverwrittenValueRead |= tmp.reg() == destination || (Arg::isAnyDef(role) && block->at(index).args()[0].isReg() && tmp.reg() == block->at(index).args()[0].reg());
            });
        }
        if (isOverwrittenValueRead)
            continue;
        Inst branch = latch->insts().takeLast();
        Inst jump = block->insts().takeLast();
        latch->insts().appendVector(WTF::move(block->insts()));
        latch->insts().append(WTF::move(branch));
        block->insts().shrink(0);
        block->insts().append(WTF::move(jump));
        hasTakenCopies.quickSet(latch->index());
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
            if (!isCopyIntoRegister(move) || move.kind != other.kind || other.args().size() != 2 || move.args()[0] != other.args()[0] || move.args()[1] != other.args()[1])
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
    changed |= hoistCopiesOfWaysBack(code);
    return changed;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
