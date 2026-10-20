/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "AirFixSpillsAfterTerminals.h"

#if ENABLE(B3_JIT)

#include "AirBlockInsertionSet.h"
#include "AirCode.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirLiveness.h"

namespace JSC { namespace B3 { namespace Air {

namespace {

struct InstsAfterTerminal {
    BasicBlock* block { nullptr };
    Vector<Tmp, 2> definedTmps;
    bool hasNonTmpEffects { false };
};

} // anonymous namespace

static bool hasInstsAfterTerminal(Code& code)
{
    for (BasicBlock* block : code) {
        if (block->size() && !block->last().isTerminal())
            return true;
    }
    return false;
}

static unsigned findTerminalIndex(BasicBlock* block)
{
    unsigned terminalIndex = block->size();
    bool foundTerminal = false;
    while (terminalIndex--) {
        if (block->at(terminalIndex).isTerminal()) {
            foundTerminal = true;
            break;
        }
    }
    ASSERT_UNUSED(foundTerminal, foundTerminal);
    return terminalIndex;
}

static Vector<InstsAfterTerminal, 4> collectInstsAfterTerminals(Code& code)
{
    Vector<InstsAfterTerminal, 4> result;
    for (BasicBlock* block : code) {
        unsigned terminalIndex = findTerminalIndex(block);
        if (terminalIndex == block->size() - 1)
            continue;

        if (!block->numSuccessors()) {
            block->resize(terminalIndex + 1);
            continue;
        }

        InstsAfterTerminal insts;
        insts.block = block;
        for (unsigned i = terminalIndex + 1; i < block->size(); i++) {
            Inst& inst = block->at(i);
            insts.hasNonTmpEffects |= inst.hasNonArgEffects();
            inst.forEachArg(
                [&] (Arg& arg, Arg::Role role, Bank, Width) {
                    if (!Arg::isAnyDef(role))
                        return;
                    if (arg.isTmp())
                        insts.definedTmps.append(arg.tmp());
                    else
                        insts.hasNonTmpEffects = true;
                });
        }
        result.append(WTF::move(insts));
    }
    return result;
}

bool fixSpillsAfterTerminals(Code& code)
{
    // Because there may be terminals that produce values, IRC may
    // want to spill those terminals. It'll happen to spill it after
    // the terminal. If we left the graph in this state, it'd be invalid
    // because a terminal must be the last instruction in a block.
    // We fix that here.

    if (!hasInstsAfterTerminal(code))
        return false;

    code.resetReachability();

    Vector<InstsAfterTerminal, 4> instsAfterTerminals = collectInstsAfterTerminals(code);

    // Liveness walks successor edges, so it is computed before any edge leads to a block that is not in the block list yet.
    std::optional<UnifiedTmpLiveness> liveness;
    if (instsAfterTerminals.containsIf([] (const InstsAfterTerminal& insts) { return !insts.hasNonTmpEffects; }))
        liveness.emplace(code);

    BlockInsertionSet blockInsertionSet(code);
    InsertionSet insertionSet(code);

    for (const InstsAfterTerminal& insts : instsAfterTerminals) {
        BasicBlock* block = insts.block;
        unsigned terminalIndex = findTerminalIndex(block);

        // There must be instructions after the terminal because it's not the last instruction.
        ASSERT(terminalIndex < block->size() - 1);
        Vector<Inst, 1> instsToMove;
        for (unsigned i = terminalIndex + 1; i < block->size(); i++)
            instsToMove.append(block->at(i));
        RELEASE_ASSERT(instsToMove.size());

        Vector<BasicBlock*, 2> successors;
        for (BasicBlock* successor : block->successorBlocks())
            successors.append(successor);

        for (FrequentedBlock& frequentedSuccessor : block->successors()) {
            BasicBlock* successor = frequentedSuccessor.block();
            if (!insts.hasNonTmpEffects) {
                auto liveAtHead = liveness->liveAtHead(successor);
                if (!insts.definedTmps.containsIf([&] (Tmp tmp) { return liveAtHead.contains(tmp); }))
                    continue;
            }
            // If successor's only predecessor is block, we can plant the spill inside
            // the successor. Otherwise, we must split the critical edge and create
            // a new block for the spill.
            if (successor->numPredecessors() == 1 && std::ranges::count(successors, successor) == 1) {
                insertionSet.insertInsts(successor->at(0).clobbersAtBlockEntry() ? 1 : 0, instsToMove);
                insertionSet.execute(successor);
            } else {
                BasicBlock* newBlock = blockInsertionSet.insertBefore(successor, successor->frequency());
                if (successor->at(0).clobbersAtBlockEntry())
                    newBlock->appendInst(successor->at(0));
                for (const Inst& inst : instsToMove)
                    newBlock->appendInst(inst);
                newBlock->appendInst(Inst(Jump, instsToMove.last().origin));
                newBlock->successors().append(successor);
                frequentedSuccessor.block() = newBlock;
            }
        }

        block->resize(terminalIndex + 1);
    }

    if (blockInsertionSet.execute())
        code.resetReachability();
    return true;
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)

