/*
 * Copyright (C) 2015-2017 Apple Inc. All rights reserved.
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
#include "AirOptimizeBlockOrder.h"

#if ENABLE(B3_JIT)

#include "AirBlockWorklist.h"
#include "AirCFG.h"
#include "AirCode.h"
#include "AirLimits.h"
#include "AirPhaseScope.h"
#include <algorithm>
#include <cmath>
#include <wtf/BubbleSort.h>
#include <wtf/CycleForest.h>
#include <wtf/Deque.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

class ChainWorklist {
public:
    ChainWorklist()
        : startNewChain(false)
    {
    }

    bool LEGACY_NODELETE isEmpty() const
    {
        return blocks.isEmpty();
    }

    size_t LEGACY_NODELETE size() const
    {
        return blocks.size();
    }

    BasicBlock* LEGACY_NODELETE pop(IndexSet<BasicBlock*>& done)
    {
        if (startNewChain) {
            startNewChain = false;
            return popNewChain(done);
        }

        return popChain(done);
    }

    void LEGACY_NODELETE markStartNewChain()
    {
        startNewChain = true;
    }

    void append(BasicBlock* block)
    {
        blocks.append(block);
    }

    BasicBlock* LEGACY_NODELETE popChain(IndexSet<BasicBlock*>& done)
    {
        // Take the last added successor to continue the chain.
        // This transforms a jump into a fallthrough.
        while (!blocks.isEmpty()) {
            BasicBlock* block = blocks.takeLast();
            if (done.contains(block))
                continue;

            return block;
        }
        return nullptr;
    }

    BasicBlock* LEGACY_NODELETE popNewChain(IndexSet<BasicBlock*>& done)
    {
        // Take the oldest added successor to start a new chain.
        // We prefer this,
        // - because that keeps earlier blocks earlier
        // - earlier blocks can still have longer chains
        // - better locality, instead of iterating further and
        //   having to backtrack these left-over early blocks
        while (!blocks.isEmpty()) {
            BasicBlock* block = blocks.takeFirst();
            if (done.contains(block))
                continue;

            return block;
        }
        return nullptr;
    }

private:

    bool startNewChain;
    Deque<BasicBlock*, 16> blocks;
};


class SortedSuccessors {
public:
    SortedSuccessors()
    {
    }

    void append(BasicBlock* block)
    {
        m_successors.append(block);
    }

    void process(BlockWorklist& worklist)
    {
        sort();

        // Pushing the successors in ascending order of frequency ensures that the very next block we visit
        // is our highest-frequency successor (unless that successor has already been visited).
        for (unsigned i = 0; i < m_successors.size(); ++i)
            worklist.push(m_successors[i]);

        m_successors.shrink(0);
    }

    void process(ChainWorklist& worklist)
    {
        sort();

        // Pushing the successors in ascending order of frequency ensures that the very next block we visit
        // is our highest-frequency successor (unless that successor has already been visited).
        for (unsigned i = 0; i < m_successors.size(); ++i)
            worklist.append(m_successors[i]);

        m_successors.shrink(0);
    }

private:
    void sort()
    {
        // We prefer a stable sort, and we don't want it to go off the rails if we see NaN. Also, the number
        // of successors is bounded. In fact, it currently cannot be more than 2. :-)
        auto isLessFrequent = [] (BasicBlock* left, BasicBlock* right) {
            return left->frequency() < right->frequency();
        };
        auto hasNoFrequency = [] (BasicBlock* block) {
            return std::isnan(block->frequency());
        };
        if (m_successors.size() > AirLimits::maxSuccessorsToBubbleSort && std::none_of(m_successors.begin(), m_successors.end(), hasNoFrequency)) {
            std::stable_sort(m_successors.begin(), m_successors.end(), isLessFrequent);
            return;
        }
        bubbleSort(m_successors.mutableSpan(), isLessFrequent);
    }

    Vector<BasicBlock*, 2> m_successors;
};

class RareBlockPlacer {
public:
    RareBlockPlacer(Code& code, IndexSet<BasicBlock*>& done, Vector<BasicBlock*>& blocksInOrder, SortedSuccessors& slowSuccessors)
        : m_code(code)
        , m_done(done)
        , m_blocksInOrder(blocksInOrder)
        , m_slowSuccessors(slowSuccessors)
        , m_placesInGaps(code.policy().layout.placesRareBlocksInGaps)
        , m_placesAfterGuards(m_placesInGaps && code.policy().layout.placesRareBlocksAfterGuards && code.numEntrypoints() == 1 && code.entrypoint(0).block() == code[0])
    {
        if (m_placesAfterGuards)
            findBlocksInCycles();
    }

    bool isEnabled() const { return m_placesInGaps; }
    bool isPlacedInGap(BasicBlock* block) const { return m_placedInGaps.contains(block); }

    void didPlace(BasicBlock* block)
    {
        if (!m_placesInGaps)
            return;

        m_placedBytes += estimatedBytes(block);
        if (!isGuard(block))
            return;

        BasicBlock* rare = rareSuccessorOf(block);
        if (m_placesAfterGuards && !m_blocksInCycles.contains(block)) {
            bool branchLeadsToRareBlock = block->successor(0).isRare();
            bool canJumpOverRareBlock = block->last().kind.opcode != Patch && (!branchLeadsToRareBlock || canInvertBranch(block->last()));
            if (canJumpOverRareBlock && !m_done.contains(rare) && isSmall(rare) && hasOnlyRarePredecessors(rare)) {
                append(rare);
                for (BasicBlock* successor : rare->successorBlocks())
                    m_slowSuccessors.append(successor);
                return;
            }
        }
        m_waitingForGap.append({ rare, m_placedBytes });
    }

    void didEndChain(BasicBlock* last)
    {
        if (!m_placesInGaps)
            return;

        for (unsigned i = m_waitingForGap.size(); i--;) {
            auto [rare, bytesPlacedAtEndOfGuard] = m_waitingForGap[i];
            if (m_done.contains(rare))
                continue;
            bool hasUnplacedSuccessor = false;
            for (BasicBlock* successor : last->successorBlocks())
                hasUnplacedSuccessor |= successor != rare && !m_done.contains(successor);
            if (hasUnplacedSuccessor || !isSmall(rare) || m_placedBytes - bytesPlacedAtEndOfGuard + estimatedBytes(rare) > AirLimits::maxBytesFromGuardToRareBlock || !hasOnlyRarePredecessors(rare))
                continue;
            append(rare);
        }
        m_waitingForGap.shrink(0);
    }

    void fillGapsBeforeGuards()
    {
        if (!m_placesInGaps || !isX86())
            return;

        Vector<std::pair<unsigned, BasicBlock*>, 8> insertions;
        std::optional<unsigned> lastGapIndex;
        unsigned bytesSinceLastGap = 0;
        for (unsigned i = 0; i < m_blocksInOrder.size(); ++i) {
            BasicBlock* block = m_blocksInOrder[i];
            bytesSinceLastGap += estimatedBytes(block);
            if (lastGapIndex && isGuard(block)) {
                BasicBlock* rare = rareSuccessorOf(block);
                if (!m_done.contains(rare) && isSmall(rare) && bytesSinceLastGap + estimatedBytes(rare) <= AirLimits::maxBytesFromGuardToRareBlock && hasOnlyRarePredecessors(rare)) {
                    m_done.add(rare);
                    m_placedInGaps.add(rare);
                    insertions.append({ *lastGapIndex, rare });
                    bytesSinceLastGap += estimatedBytes(rare);
                }
            }
            bool fallsThrough = false;
            for (BasicBlock* successor : block->successorBlocks())
                fallsThrough |= i + 1 < m_blocksInOrder.size() && successor == m_blocksInOrder[i + 1];
            if (!fallsThrough) {
                lastGapIndex = i;
                bytesSinceLastGap = 0;
            }
        }
        if (insertions.isEmpty())
            return;

        Vector<BasicBlock*> withInsertions;
        withInsertions.reserveInitialCapacity(m_blocksInOrder.size() + insertions.size());
        unsigned next = 0;
        for (unsigned i = 0; i < m_blocksInOrder.size(); ++i) {
            withInsertions.append(m_blocksInOrder[i]);
            for (; next < insertions.size() && insertions[next].first == i; ++next)
                withInsertions.append(insertions[next].second);
        }
        m_blocksInOrder = WTF::move(withInsertions);
    }

private:
    struct WaitingBlock {
        BasicBlock* rare;
        unsigned bytesPlacedAtEndOfGuard;
    };

    static unsigned estimatedBytes(BasicBlock* block) { return block->size() * AirLimits::estimatedBytesPerInst; }

    static bool isGuard(BasicBlock* block)
    {
        return block->numSuccessors() == 2 && block->successor(0).isRare() != block->successor(1).isRare();
    }

    static BasicBlock* rareSuccessorOf(BasicBlock* guard)
    {
        return guard->successor(guard->successor(0).isRare() ? 0 : 1).block();
    }

    static bool isSmall(BasicBlock* rare) { return estimatedBytes(rare) <= AirLimits::maxBytesOfRareBlockNearGuard; }

    bool hasOnlyRarePredecessors(BasicBlock* block) const
    {
        for (BasicBlock* predecessor : block->predecessors()) {
            if (m_placedInGaps.contains(predecessor))
                continue;
            if (!m_done.contains(predecessor))
                return false;
            for (FrequentedBlock& successor : predecessor->successors()) {
                if (successor.block() == block && !successor.isRare())
                    return false;
            }
        }
        return true;
    }

    void append(BasicBlock* rare)
    {
        m_done.add(rare);
        m_placedInGaps.add(rare);
        m_blocksInOrder.append(rare);
        m_placedBytes += estimatedBytes(rare);
    }

    void findBlocksInCycles()
    {
        WTF::CycleForest<CFG> cycles(m_code.cfg(), [](std::span<BasicBlock* const> members, const auto&, const auto&) {
            return members[0];
        });
        for (BasicBlock* block : m_code) {
            if (cycles.isInCycle(block))
                m_blocksInCycles.add(block);
        }
    }

    Code& m_code;
    IndexSet<BasicBlock*>& m_done;
    Vector<BasicBlock*>& m_blocksInOrder;
    SortedSuccessors& m_slowSuccessors;
    const bool m_placesInGaps;
    const bool m_placesAfterGuards;
    IndexSet<BasicBlock*> m_placedInGaps;
    IndexSet<BasicBlock*> m_blocksInCycles;
    Vector<WaitingBlock, 8> m_waitingForGap;
    unsigned m_placedBytes { 0 };
};

} // anonymous namespace

static double frequencyOfEdgeToSharedSuccessor(BasicBlock* block, BasicBlock* exclusive, BasicBlock* shared)
{
    double remainder = block->frequency() - exclusive->frequency();
    double arriving = shared->frequency();
    for (BasicBlock* predecessor : shared->predecessors()) {
        if (predecessor == block)
            continue;
        if (predecessor->numSuccessors() != 1)
            return remainder;
        arriving -= predecessor->frequency();
    }
    return std::min(remainder, arriving);
}

static bool detectTriangleStructure(BasicBlock* blockA, ChainWorklist& worklist, IndexSet<BasicBlock*>& done, bool hasFrequencies)
{
    // A*
    // |-----.
    // |      |
    // |      C
    // |      |
    // |-----'
    // B

    // Since we don't have actual frequencies,
    // it is better to schedule C before B

    if (blockA->numSuccessors() != 2)
        return false;

    if (blockA->successor(0).isRare())
        return false;

    if (blockA->successor(1).isRare())
        return false;

    auto attemptToDetect = [&](BasicBlock* blockB, BasicBlock* blockC) {
        if (hasFrequencies && frequencyOfEdgeToSharedSuccessor(blockA, blockC, blockB) > blockC->frequency() * AirLimits::minRatioOfLikelierEdge)
            return false;
        if ((blockC->numSuccessors() >= 1 && blockC->successor(0).block() == blockB)
            || (blockC->numSuccessors() >= 2 && blockC->successor(1).block() == blockB)) {
            if (!done.contains(blockB))
                worklist.append(blockB);
            if (!done.contains(blockC))
                worklist.append(blockC);
            return true;
        }
        return false;
    };

    auto* block0 = blockA->successor(0).block();
    auto* block1 = blockA->successor(1).block();
    return attemptToDetect(block0, block1)
        || attemptToDetect(block1, block0);
}

static bool detectDiamondStructure(BasicBlock* blockB, ChainWorklist& worklist, IndexSet<BasicBlock*>& done, bool hasFrequencies)
{
    //     A
    //  .--'--.
    // |       |
    // B*      C
    // |       |
    //  '--.--'
    //     D

    // B* is the block we are currently looking at.

    // Since we don't have actual frequencies,
    // it is better to not decide which branch (B,C) is best and
    // assume both have equal chance.
    // With a small penalty we better organize it as:
    // A B C D
    // That way we have one small jump for case B and for case C,
    // instead of having no jumps for B and two long jumps for C.

    if (blockB->numSuccessors() != 1)
        return false;

    if (blockB->numPredecessors() != 1)
        return false;

    if (blockB->successor(0).isRare())
        return false;

    BasicBlock* blockD = blockB->successor(0).block();
    BasicBlock* blockA = blockB->predecessor(0);

    if (blockA->numSuccessors() != 2)
        return false;

    BasicBlock* blockC;
    if (blockA->successor(0).block() == blockB) {
        if (blockA->successor(1).isRare())
            return false;
        blockC = blockA->successor(1).block();
    } else if (blockA->successor(1).block() == blockB) {
        if (blockA->successor(0).isRare())
            return false;
        blockC = blockA->successor(0).block();
    } else
        return false;

    if (hasFrequencies && blockC->frequency() < blockB->frequency())
        return false;

    if (blockC->numSuccessors() != 1)
        return false;

    if (blockC->numPredecessors() != 1)
        return false;

    if (blockC->successor(0).block() != blockD)
        return false;

    if (!done.contains(blockD))
        worklist.append(blockD);
    if (!done.contains(blockC))
        worklist.append(blockC);
    return true;
}

static bool detectExclusiveSuccessor(BasicBlock* blockA, ChainWorklist& worklist, IndexSet<BasicBlock*>& done)
{
    //     A*       D
    //  .--'--.     |
    // |       |.---'
    // B       C

    // A* is the block we are currently looking at.

    // It's better to use successor B as the fallthrough block,
    // because C can still become the fallthrough block from the other predecessors.

    if (blockA->numSuccessors() != 2)
        return false;
    if (blockA->successor(0).isRare())
        return false;
    if (blockA->successor(1).isRare())
        return false;
    if (blockA->successor(0).block()->frequency() != blockA->successor(1).block()->frequency())
        return false;

    BasicBlock* blockB = blockA->successor(0).block();
    BasicBlock* blockC = blockA->successor(1).block();
    if (blockB->numPredecessors() == 1
        && blockC->numPredecessors() > 1) {
        // Same frequency, with succ[0] having only one predecessor.
        // and succ[1] having multiple predecessors.
        // It is better to add succ[0] as last to get a fallthrough.
        // Since except here there is no chance succ[0] can fallthrough, but succ[1] still can.
        if (!done.contains(blockC))
            worklist.append(blockC);
        if (!done.contains(blockB))
            worklist.append(blockB);
        return true;
    }

    return false;
}

static bool hasOneLikelyPredecessor(BasicBlock* block)
{
    unsigned numberOfLikelyPredecessors = 0;
    for (BasicBlock* predecessor : block->predecessors()) {
        bool isLikely = predecessor->frequency() * AirLimits::maxRatioOfBlockToLikelyPredecessor >= block->frequency();
        for (FrequentedBlock& successor : predecessor->successors())
            isLikely &= successor.block() != block || !successor.isRare();
        numberOfLikelyPredecessors += isLikely;
    }
    return numberOfLikelyPredecessors == 1;
}

static bool appendSuccessorsByEdgeFrequency(BasicBlock* block, ChainWorklist& worklist, IndexSet<BasicBlock*>& done)
{
    if (block->numSuccessors() != 2)
        return false;
    if (block->successor(0).isRare())
        return false;
    if (block->successor(1).isRare())
        return false;

    BasicBlock* first = block->successor(0).block();
    BasicBlock* second = block->successor(1).block();
    if (first == second || done.contains(first) || done.contains(second))
        return false;

    bool firstIsExclusive = hasOneLikelyPredecessor(first);
    if (firstIsExclusive == hasOneLikelyPredecessor(second))
        return false;

    BasicBlock* exclusive = firstIsExclusive ? first : second;
    BasicBlock* shared = firstIsExclusive ? second : first;
    bool sharedEdgeIsLikelier = frequencyOfEdgeToSharedSuccessor(block, exclusive, shared) > exclusive->frequency() * AirLimits::minRatioOfLikelierEdge;
    worklist.append(sharedEdgeIsLikelier ? exclusive : shared);
    worklist.append(sharedEdgeIsLikelier ? shared : exclusive);
    return true;
}

Vector<BasicBlock*> blocksInOptimizedOrder(Code& code)
{
    Vector<BasicBlock*> blocksInOrder;

    SortedSuccessors sortedSlowSuccessors;
    SortedSuccessors sortedSuccessors;
    ChainWorklist chainWorklist;
    IndexSet<BasicBlock*> done;
    RareBlockPlacer placer(code, done, blocksInOrder, sortedSlowSuccessors);

    // We expect entrypoint lowering to have already happened.
    RELEASE_ASSERT(code.numEntrypoints());

    auto appendSuccessor = [&] (const FrequentedBlock& block) {
        if (block.isRare())
            sortedSlowSuccessors.append(block.block());
        else
            sortedSuccessors.append(block.block());
    };
    
    // For everything but the first entrypoint, we push them in order of frequency and frequency
    // class.
    for (unsigned i = 1; i < code.numEntrypoints(); ++i)
        appendSuccessor(code.entrypoint(i));
    
    // Always push the primary successor last so that it gets highest priority.
    chainWorklist.append(code.entrypoint(0).block());

    while (BasicBlock* block = chainWorklist.pop(done)) {
        ASSERT(!done.contains(block));

        done.add(block);
        blocksInOrder.append(block);

        placer.didPlace(block);
        size_t size = chainWorklist.size();

        bool hasFrequencies = code.policy().layout.ordersBlocksByEdgeFrequency;
        if (!detectTriangleStructure(block, chainWorklist, done, hasFrequencies)
            && !detectDiamondStructure(block, chainWorklist, done, hasFrequencies)
            && !detectExclusiveSuccessor(block, chainWorklist, done)) {

            if (!(hasFrequencies && appendSuccessorsByEdgeFrequency(block, chainWorklist, done)))
            for (FrequentedBlock& successor : block->successors()) {
                if (!done.contains(successor.block()))
                    appendSuccessor(successor);
            }
        }
        sortedSuccessors.process(chainWorklist);

        // Detect if we added a successor. If not decide for a good candidate.
        if (size == chainWorklist.size()) {
            placer.didEndChain(block);
            chainWorklist.markStartNewChain();
        }
    }

    placer.fillGapsBeforeGuards();
    if (code.policy().layout.recordsRareCodeStart) [[unlikely]]
        code.setNumberOfBlocksInChains(blocksInOrder.size());
    BlockWorklist slowWorklist;
    sortedSlowSuccessors.process(slowWorklist);

    while (BasicBlock* block = slowWorklist.pop()) {
        // We might have already processed this block.
        if (done.contains(block) && !placer.isPlacedInGap(block))
            continue;

        if (!done.contains(block)) {
            done.add(block);
            blocksInOrder.append(block);
        }
        while (placer.isEnabled()) {
            BasicBlock* next = nullptr;
            for (BasicBlock* successor : block->successorBlocks()) {
                if (!done.contains(successor) && (!next || successor->frequency() > next->frequency()))
                    next = successor;
            }
            if (!next)
                break;
            for (BasicBlock* successor : block->successorBlocks())
                sortedSlowSuccessors.append(successor);
            sortedSlowSuccessors.process(slowWorklist);
            block = next;
            done.add(block);
            blocksInOrder.append(block);
        }
        for (BasicBlock* successor : block->successorBlocks())
            sortedSlowSuccessors.append(successor);
        sortedSlowSuccessors.process(slowWorklist);
    }

    ASSERT(chainWorklist.isEmpty());
    ASSERT(slowWorklist.isEmpty());

    return blocksInOrder;
}

bool canInvertBranch(const Inst& branch)
{
    // It's somewhat tempting to just say that if the block has two successors and the first arg is
    // invertible, then we can do the optimization. But that's wagging the dog. The fact that an
    // instruction happens to have an argument that is invertible doesn't mean it's a branch, even though
    // it is true that currently only branches have invertible arguments. It's also tempting to say that
    // the /branch flag in AirOpcode.opcodes tells us that something is a branch - except that there,
    // /branch also means Jump. The approach taken here means that if you add new branch instructions and
    // forget about this phase, then at worst your new instructions won't opt into the inversion
    // optimization.  You'll probably realize that as soon as you look at the disassembly, and it
    // certainly won't cause any correctness issues.

    switch (branch.kind.opcode) {
    case BranchOnFlags:
    case Branch8:
#if CPU(X86_64)
    case Branch16:
#endif
    case Branch32:
    case Branch64:
    case BranchTest8:
    case BranchTest32:
    case BranchTest64:
    case BranchTestBit32:
    case BranchTestBit64:
    case BranchFloat:
    case BranchDouble:
    case BranchFloatWithZero:
    case BranchDoubleWithZero:
    case BranchAdd32:
    case BranchAdd64:
    case BranchMul32:
    case BranchMul64:
    case BranchSub32:
    case BranchSub64:
    case BranchNeg32:
    case BranchNeg64:
    case BranchAtomicStrongCAS8:
    case BranchAtomicStrongCAS16:
    case BranchAtomicStrongCAS32:
    case BranchAtomicStrongCAS64:
        return branch.args()[0].isInvertible();

    default:
        return false;
    }
}

void invertBranch(BasicBlock* block)
{
    Inst& branch = block->last();
    std::swap(block->successor(0), block->successor(1));
    branch.args()[0] = branch.args()[0].inverted();
}

void optimizeBlockOrder(Code& code)
{
    PhaseScope phaseScope(code, "optimizeBlockOrder"_s);

    Vector<BasicBlock*> blocksInOrder = blocksInOptimizedOrder(code);

    code.adoptBlockOrder(blocksInOrder);

    // Finally, flip any branches that we recognize. It's most optimal if the taken successor does not point
    // at the next block.
    for (BasicBlock* block : code) {
        if (canInvertBranch(block->last()) && code.findNextBlock(block) == block->successorBlock(0))
            invertBranch(block);
    }
}

void moveBlocksWithoutSuccessorsToEnd(Code& code)
{
    PhaseScope phaseScope(code, "moveBlocksWithoutSuccessorsToEnd"_s);

    Vector<BasicBlock*> order;
    order.reserveInitialCapacity(code.size());
    for (BasicBlock* block : code) {
        if (block->numSuccessors() || block == code[0])
            order.append(block);
    }
    for (BasicBlock* block : code) {
        if (!block->numSuccessors() && block != code[0])
            order.append(block);
    }
    code.adoptBlockOrder(order);
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
