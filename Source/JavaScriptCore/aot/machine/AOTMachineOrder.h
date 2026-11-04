/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"

namespace JSC { namespace AOT { namespace Machine {

// A place in the order of allocation. Every operation has two: it reads its operands at the early one, and writes its results and clobbers at the late one, which is one more.
// A block takes one pair for its head, where its Phis are defined and what sank into it stands, and one pair for each of its operations.
// The children of its Upsilons are read at the early place of its last operation.
// Behind a block with an unsplit edge two pairs are left free for each of its successors: those of the block that Order::splitLate() may put on the edge.
using Position = uint32_t;
inline constexpr Position noPosition = UINT32_MAX;
inline constexpr uint32_t noLoop = UINT32_MAX;
inline constexpr uint32_t noRareRegion = UINT32_MAX;

// An interval of the order that an edge leads back to the start of. Whatever is live at its start and read inside is live to its end.
// It goes by the order and not by dominance, so a cycle that is entered in two places is covered too.
struct Loop {
    uint32_t parent { noLoop };
    uint32_t firstBlock { 0 };
    // In the order, one behind the last.
    uint32_t endBlock { 0 };
    Position begin { 0 };
    Position end { 0 };
};

// Blocks that only rare edges lead to, joined by the edges among them.
// In the order of allocation they stand in one piece, behind every block from outside that leads to one of them.
struct RareRegion {
    // The one block of the region that edges from outside lead to, or null if there are several.
    Block* onlyEntry { nullptr };
};

// The layout has one part for each, in this order. With ProcedureTraits::fallsIntoWhatLiesBehind: the other way round.
enum class LayoutPart : uint8_t {
    // Normal edges alone lead to the block from an entrypoint.
    Likely,
    Unlikely,
    Rare,
};
inline constexpr uint32_t numberOfLayoutParts = 3;

struct BlockPlace {
    bool isReached() const { return orderIndex != Block::notOrdered; }
    bool isRare() const { return rareRegion != noRareRegion; }

    uint32_t orderIndex { Block::notOrdered };
    uint32_t layoutIndex { Block::notOrdered };
    Position head { noPosition };
    // The innermost.
    uint32_t loop { noLoop };
    uint32_t rareRegion { noRareRegion };
    bool isLoopHead { false };
    // An edge from outside its rare region leads to it. It has one predecessor, unless it is a landing pad or unsplit edges lead to it: then every predecessor stands outside rare regions.
    bool entersRareRegion { false };
    // Its one successor lies outside its rare region. Operation::isUnconditionalJump() holds of what ends it, unless that successor is a landing pad.
    bool leavesRareRegion { false };
    // Order::splitLate() has made it.
    bool wasSplitLate { false };
    // Which of the three parts of the layout it stands in.
    LayoutPart layoutPart { LayoutPart::Likely };
    // The block from which the search first came here, where it came over an unsplit edge. A block on that edge would stand just ahead of this one, behind every other block ahead that leads here.
    const Block* sourceOfUnsplitEdgeThatCountsLast { nullptr };
    // Reached blocks only, in the order of allocation. A predecessor with two edges to the block is listed twice.
    std::span<Block* const> predecessors;
};

// Step 2. It looks at blocks, edges, Phis and Upsilons, and at the operations of a block only where it compares two blocks or asks whether a join holds more than its Branch.
class Order {
    WTF_MAKE_NONCOPYABLE(Order);
public:
    // Reverse postorder from the entrypoints, normal successors first, unlikely ones behind them, each loop contiguous.
    // The ways through rare blocks count: a block stands ahead of what it leads to through them, unless that leads back to it.
    // A rare block stands just ahead of the block it rejoins, or behind the block it branches off if it never rejoins.
    std::span<Block* const> blocksInOrder() const { return m_blocksInOrder.span(); }
    // The order of the code: the blocks that normal edges alone lead to, then the others that are not rare, then the rare ones, each part as above. With ProcedureTraits::fallsIntoWhatLiesBehind: the three parts the other way round.
    // But a loop that ends with a jump back to its head is rotated, within the part of its head, until it ends with a block that tests whether to leave it.
    std::span<Block* const> blocksInLayout() const { return m_blocksInLayout.span(); }
    unsigned numberOfBlocksOutsideRareRegions() const { return m_numberOfBlocksOutsideRareRegions; }
    // Of a block that ends in a Branch neither way of which falls through: the successor that the conditional jump leads to. A jump to the other one stands behind it.
    // It is the likelier one, and of two that are as likely the one that stays in the innermost loop around the block.
    unsigned successorOfConditionalJump(const Block* block) const
    {
        const Edge& first = block->successor(0);
        const Edge& second = block->successor(1);
        if (first.edgeClass() != second.edgeClass())
            return first.edgeClass() < second.edgeClass() ? 0 : 1;
        uint32_t loop = of(block).loop;
        if (loop == noLoop)
            return 0;
        auto staysInLoop = [&](const Edge& edge) {
            uint32_t index = of(edge.block()).orderIndex;
            return m_loops[loop].firstBlock <= index && index < m_loops[loop].endBlock;
        };
        return staysInLoop(second) && !staysInLoop(first) ? 1 : 0;
    }

    const BlockPlace& of(const Block* block) const { return m_places[block->index()]; }
    // By Block::index(), for a loop that asks about many blocks. Order::splitLate() does not move them.
    const BlockPlace* places() const { return m_places.begin(); }
    std::span<const Loop> loops() const { return m_loops.span(); }
    std::span<const RareRegion> rareRegions() const { return m_rareRegions.span(); }

    Position headOf(const Block* block) const { return of(block).head; }
    Position earlyPositionOf(const Block* block, unsigned indexInBlock) const { return of(block).head + 2 * (indexInBlock + 1); }
    // Of an operation with a position. A Phi stands at the head, an Upsilon at the last operation of its block.
    Position earlyPositionOf(const Operation* operation) const
    {
        const Block* block = operation->owner();
        if (operation->opcode() == Phi)
            return headOf(block);
        if (operation->opcode() == Upsilon)
            return earlyPositionOf(block, block->size() - 1);
        return earlyPositionOf(block, operation->indexInBlock());
    }
    Position latePositionOf(const Operation* operation) const { return earlyPositionOf(operation) + 1; }
    // One behind the late place of the last operation.
    Position endOf(const Block* block) const { return of(block).head + 2 * (block->size() + 1); }
    Position endOfProcedure() const { return m_end; }

    // The one way an edge changes its target. The Upsilons of `from` for Phis of the old target are dead from here on.
    static void retarget(Block* from, unsigned successorIndex, Block* newTarget);
    // retarget() makes an edge that skips its old target as rare as the rarer of the two edges it replaces. This one keeps the class of the edge.
    // It is for skipping a Branch that goes one way for every block that reaches it: the class of that way says nothing.
    static void retargetKeepingClass(Block* from, unsigned successorIndex, Block* newTarget)
    {
        EdgeClass edgeClass = from->successor(successorIndex).edgeClass();
        retarget(from, successorIndex, newTarget);
        from->m_successors[successorIndex].m_class = edgeClass;
    }
    // The Jump that ends `from` leads to the block of the Phi, which a Branch on the Phi ends. The Jump becomes a Branch on what `from` hands the Phi, to the same two blocks, the other way round where `isInverted`.
    // Each edge is as rare as the rarer of the two that it replaces. The read of the Upsilon, which is dead from here on, is the read of the Branch.
    static void replaceJumpWithBranch(Block* from, const Operation* phi, bool isInverted);
    // A new block with a Jump to the old target and the class of the edge. The Upsilons of `from` for Phis of the target move along: they are appended to the new block again.
    static Block* split(Block* from, unsigned successorIndex);
    // A Branch ends `first`, which leaves by one edge and goes on to `second` by the other, and nothing else enters `second`, which a Branch ends too. Neither block has an Upsilon.
    // The Branch of `first` moves into a new block on the edge by which `second` leaves: there it leaves as it did, and else goes where `second` left for. A Jump to `second` ends `first`.
    static Block* askBehind(Block* first, unsigned leavingSuccessorOfFirst, Block* second, unsigned leavingSuccessorOfSecond);
    // An unsplit edge leads from a block outside rare regions that a Branch or a Switch ends, with several successors, to a block with several predecessors and no Phi.
    // orderBlocks() puts no block on it: most such edges never get a move. Where the allocator has one for it, or a frame is set up or torn down on it, the allocator splits it.
    static bool isUnsplit(const Block* from, const BlockPlace& placeOfTarget)
    {
        return from->numSuccessors() > 1 && from->last()->opcode() != Patch && placeOfTarget.predecessors.size() > 1;
    }
    // The edge behind an entry of BlockPlace::predecessors. A Switch may have as many edges as the procedure has blocks, and nobody looks for one among them.
    const Edge& edgeFromPredecessor(const Block* target, size_t numberOfPredecessor) const
    {
        std::span<Block* const> predecessors = of(target).predecessors;
        const Block* from = predecessors[numberOfPredecessor];
        if (from->numSuccessors() <= 2)
            return from->successor(from->successor(0).block() == target ? 0 : 1);
        return from->successor(successorIndexOfPredecessorWithManySuccessors(target, &predecessors[numberOfPredecessor]));
    }
    // The most blocks and operations that the procedure may come to have: each unsplit edge may get a block that holds a Jump.
    // Whoever keeps a list by Block::index() or Operation::index() between orderBlocks() and the end of allocateRegisters() makes it this long.
    uint32_t mostBlocks() const { return m_mostBlocks; }
    uint32_t mostOperations() const { return m_mostOperations; }
    // For allocateRegisters() alone. split(), and the new block has its BlockPlace at once, but for BlockPlace::layoutIndex: it stands outside rare regions, in the loop of `from`, and counts as standing where `from` does.
    // Its positions are those that were left free behind `from`. blocksInOrder() and blocksInLayout() do not list it until placeBlocksSplitLate().
    static Block* splitLate(Procedure&, Block* from, unsigned successorIndex);
    // Once, behind the last splitLate(). In the order each new block stands right behind the block that leads to it.
    // In the layout too where both stand in one part. Any other stands at the end of its part: the rare one where the edge was rare, else the unlikely one.
    static void placeBlocksSplitLate(Procedure&);

    // Where threadJumps() or mergeIdenticalBlocks() has led both edges of the Branch that ends the block to one block, the Branch decides nothing.
    // The block keeps one edge, as likely as the likelier of the two, and the Branch gives back the read of what it tested: Operation::isUnconditionalJump() holds of it from here on.
    static void joinEdgesToOneBlock(Block* from);

private:
    friend class Arena;
    friend Order* orderBlocks(Procedure&);

    Order() = default;
    JS_EXPORT_PRIVATE unsigned successorIndexOfPredecessorWithManySuccessors(const Block* target, Block* const* entry) const;

    ArenaVector<Block*> m_blocksInOrder;
    ArenaVector<Block*> m_blocksInLayout;
    // By Block::index().
    ArenaVector<BlockPlace> m_places;
    ArenaVector<Loop> m_loops;
    ArenaVector<RareRegion> m_rareRegions;
    unsigned m_numberOfBlocksOutsideRareRegions { 0 };
    Position m_end { 0 };
    uint32_t m_mostBlocks { 0 };
    uint32_t m_mostOperations { 0 };
    ArenaVector<Block*> m_blocksSplitLate;
    // What BlockPlace::predecessors are pieces of, but for those of a block that was split off late.
    std::span<Block* const> m_predecessors;
    // Beside m_predecessors: of an entry that holds a block with more than two successors, which of them the edge is. Empty where there is no such block.
    std::span<const uint32_t> m_successorIndexesOfPredecessors;
};

// In this order, each once.

// A Branch on a Phi whose block holds nothing else that is read or has to stay, no Upsilon either, is not a loop head, and whose Phi has no other use:
// each incoming edge whose Upsilon passes a constant goes straight to the successor that the constant chooses,
// and a block that passes any other value and ends in a Jump ends in a Branch on that value instead: Order::replaceJumpWithBranch().
// Where that value is the one Phi of the block, the edges into the block are threaded in turn.
// An edge from a block that nothing reaches stays. Where only one edge from a reached block leads to the block of the Phi, that edge keeps its class.
// And an edge to a block that holds a Jump alone, no Phi and no Upsilon, goes to the target of the Jump.
// So does an edge to such a block with Upsilons, where another edge of the same block leads to that target and the Upsilons of the two blocks hand its Phis the same values.
// Only the edges of a block that ends in a Jump, a Branch or a Switch move, and no edge is moved onto a landing pad.
// An edge into a block that holds nothing but a Branch and what the Branch alone reads, and is no loop head, goes straight to the arm where the way to the edge has decided the test.
// And where a Branch leaves its block, rarely, for one value of a word, and the block that it goes on to, which nothing else enters and which computes nothing but from words, would leave for that value too,
// the first Branch is asked behind the second, on the way that leaves: Order::askBehind().
void threadJumps(Procedure&);
// Blocks with the same operations on the same children, the same Upsilons and the same successors become one: the edges to the others are retargeted.
// A block with a Patch takes part only if the other has the same generator object and equal constraints, clobbers, effects and flags.
void mergeIdenticalBlocks(Procedure&);
// First makes one edge of the two that a Branch has to one block: Order::joinEdgesToOneBlock().
// Splits every edge from a block with several successors to a block with several predecessors or with Phis, every rare edge into a block with several predecessors,
// every edge out of a rare region from a block with several successors, and the edge of a block that a Patch ends to a block with several predecessors. Then numbers.
// But an edge of which Order::isUnsplit() holds stays as it is, where neither end is rare or the edge is rare and leads to a rare block that blocks outside rare regions alone lead to,
// unless another edge of the block leads to the same target or the Branch may come to go by the overflow flag: mayTestSumByOverflow().
// The last covers the one edge of a Patch out of its rare region: a block outside that a rare block leads to has a predecessor outside too.
// The first edge of a Patch with a tuple result to a block with one predecessor is never split: its Extracts stand at the head of that block. Nor is an edge to a landing pad.
Order* orderBlocks(Procedure&);

// AOTMachineFusion.cpp. By the operations of the block alone: the scan marks no other Branch with ValueFlag::GoesByOverflow. Each successor of such a Branch has one predecessor.
bool mayTestSumByOverflow(const Operation* branch);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
