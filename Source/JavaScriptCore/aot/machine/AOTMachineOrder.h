/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachine.h"

namespace JSC { namespace AOT { namespace Machine {

using Position = uint32_t;
inline constexpr Position noPosition = UINT32_MAX;
inline constexpr uint32_t noLoop = UINT32_MAX;
inline constexpr uint32_t noRareRegion = UINT32_MAX;

struct Loop {
    uint32_t parent { noLoop };
    uint32_t loopAroundOutsideRun { noLoop };
    uint32_t firstBlock { 0 };
    uint32_t endBlock { 0 };
    Position begin { 0 };
    Position end { 0 };
    bool hasSideEntrance { false };
};

inline uint32_t innermostLoopAroundBoth(const Loop* loops, uint32_t loopAroundEarlier, Position later)
{
    while (loopAroundEarlier != noLoop && loops[loopAroundEarlier].end <= later)
        loopAroundEarlier = loops[loopAroundEarlier].loopAroundOutsideRun;
    return loopAroundEarlier;
}

struct RareRegion {
    Block* onlyEntry { nullptr };
};

enum class LayoutPart : uint8_t {
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
    uint32_t loop { noLoop };
    uint32_t rareRegion { noRareRegion };
    bool isLoopHead { false };
    bool entersRareRegion { false };
    bool leavesRareRegion { false };
    bool wasSplitLate { false };
    LayoutPart layoutPart { LayoutPart::Likely };
    const Block* sourceOfUnsplitEdgeThatCountsLast { nullptr };
    std::span<Block* const> predecessors;
};

class Order {
    WTF_MAKE_NONCOPYABLE(Order);
public:
    std::span<Block* const> blocksInOrder() const { return m_blocksInOrder.span(); }
    std::span<Block* const> blocksInLayout() const { return m_blocksInLayout.span(); }
    unsigned numberOfBlocksOutsideRareRegions() const { return m_numberOfBlocksOutsideRareRegions; }
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
    const BlockPlace* places() const { return m_places.begin(); }
    std::span<const Loop> loops() const { return m_loops.span(); }
    uint32_t numberOfLoopsRotated() const { return m_numberOfLoopsRotated; }
    std::span<const RareRegion> rareRegions() const { return m_rareRegions.span(); }

    Position headOf(const Block* block) const { return of(block).head; }
    Position earlyPositionOf(const Block* block, unsigned indexInBlock) const { return of(block).head + 2 * (indexInBlock + 1); }
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
    Position endOf(const Block* block) const { return of(block).head + 2 * (block->size() + 1); }
    Position endOfProcedure() const { return m_end; }

    static inline void retarget(Block* from, unsigned successorIndex, Block* newTarget);
    static inline void retargetPast(Block* from, unsigned successorIndex, Edge passedBy);
    static inline void retargetToBlockThatStandsForTarget(Block* from, unsigned successorIndex, Block* newTarget);
    static void emptyBlock(Block*);
    static void letGoOfBlockThatNothingLeadsTo(Block*);
    static void replaceJumpWithBranch(Block* from, const Operation* phi, bool isInverted);
    static Block* split(Block* from, unsigned successorIndex, EdgeClass classOfEdgeToTarget);
    static Block* askBehind(Block* first, unsigned leavingSuccessorOfFirst, Block* second, unsigned leavingSuccessorOfSecond);
    static void leaveTestToNext(Block* first, unsigned leavingSuccessor);
    static void readWordWhereRequested(Block* asked, Operation* word);
    static void compareEarlierReadWord(Operation* comparison, Operation* word);
    static void mergeTwoByteTests(Block* first, unsigned wayOnOfFirst, Block* second, unsigned wayOnOfSecond, Operation* base, int64_t offsetOfWord, uint32_t mask, uint32_t value);
    static void askBothBoundsAtOnce(Block* first, unsigned leavingSuccessorOfFirst, Block* second, unsigned leavingSuccessorOfSecond, Operation* subject, int32_t low, int32_t high);
    struct CaseOfEquality {
        int64_t value;
        Edge target;
    };
    static void askEqualitiesBySwitch(Block* first, Operation* subject, std::span<Block* const> laterLinks, std::span<const CaseOfEquality> sortedCases, Edge otherwise);
    static void askBitsOfTwoTestsAtOnce(Block* first, unsigned wayOfFirstWhereBitIsSet, Operation* bitsOfFirst, Block* second, unsigned wayOfSecondWhereBitIsSet, Operation* bitsOfSecond);
    static void makePhiOfTruncationBehindJoin(Block* join, Operation* truncation);
    static void passJoinBy(Block* from, Block* target, Operation* phi, Operation* value);
    static void passTestToJoin(Block* from, Block* join, const Operation* phi, bool isInverted);
    static void moveTestToLatch(Block* header, Block* latch, Operation* test, unsigned successorInLoop);
    static bool isUnsplit(const Block* from, const BlockPlace& placeOfTarget)
    {
        return from->numSuccessors() > 1 && from->last()->opcode() != Patch && placeOfTarget.predecessors.size() > 1;
    }
    const Edge& edgeFromPredecessor(const Block* target, size_t numberOfPredecessor) const
    {
        std::span<Block* const> predecessors = of(target).predecessors;
        const Block* from = predecessors[numberOfPredecessor];
        if (from->numSuccessors() <= 2)
            return from->successor(from->successor(0).block() == target ? 0 : 1);
        return from->successor(successorIndexOfPredecessorWithManySuccessors(target, &predecessors[numberOfPredecessor]));
    }
    bool isRareEdgeFromPredecessor(const Block* target, size_t numberOfPredecessor) const
    {
        const BlockPlace& place = of(target);
        uint32_t regionOfFrom = of(place.predecessors[numberOfPredecessor]).rareRegion;
        return (regionOfFrom != noRareRegion && regionOfFrom != place.rareRegion) || edgeFromPredecessor(target, numberOfPredecessor).isRare();
    }
    bool hasBackEdgeOutsideRareRegions(const Loop& loop) const
    {
        const Block* head = blocksInOrder()[loop.firstBlock];
        std::span<Block* const> predecessors = of(head).predecessors;
        for (size_t index = predecessors.size(); index-- && of(predecessors[index]).orderIndex >= loop.firstBlock;) {
            if (!isRareEdgeFromPredecessor(head, index))
                return true;
        }
        return false;
    }
    uint32_t mostBlocks() const { return m_mostBlocks; }
    uint32_t mostOperations() const { return m_mostOperations; }
    static Block* splitLate(Procedure&, Block* from, unsigned successorIndex);
    static void placeBlocksSplitLate(Procedure&);

    static void joinEdgesToOneBlock(Block* from);

private:
    friend class Arena;
    friend Order* orderBlocks(Procedure&);

    template<typename Functor> static inline void retargetWithClass(Block* from, unsigned successorIndex, Block* newTarget, const Functor& raiseClass);

    static void setClassOf(Edge& edge, EdgeClass edgeClass)
    {
        edge.m_block->didChangeClassOfIncomingEdge(edge.m_class, edgeClass);
        edge.m_class = edgeClass;
    }

    Order() = default;
    JS_EXPORT_PRIVATE unsigned successorIndexOfPredecessorWithManySuccessors(const Block* target, Block* const* entry) const;
    static void dropUpsilonsHandedTo(const Block* formerTarget, Block* from);

    ArenaVector<Block*> m_blocksInOrder;
    ArenaVector<Block*> m_blocksInLayout;
    ArenaVector<BlockPlace> m_places;
    ArenaVector<Loop> m_loops;
    uint32_t m_numberOfLoopsRotated { 0 };
    ArenaVector<RareRegion> m_rareRegions;
    unsigned m_numberOfBlocksOutsideRareRegions { 0 };
    Position m_end { 0 };
    uint32_t m_mostBlocks { 0 };
    uint32_t m_mostOperations { 0 };
    ArenaVector<Block*> m_blocksSplitLate;
    std::span<Block* const> m_predecessors;
    std::span<const uint32_t> m_successorIndexesOfPredecessors;
};

ALWAYS_INLINE void Order::retarget(Block* from, unsigned successorIndex, Block* newTarget)
{
    retargetWithClass(from, successorIndex, newTarget, [&](Edge& edge, const Block* oldTarget) ALWAYS_INLINE_LAMBDA {
        constexpr uint8_t passesNoEdgeBy = UINT8_MAX;
        uint8_t classOfEdgePassedBy = passesNoEdgeBy;
        for (const Edge& passedBy : oldTarget->successors()) {
            if (passedBy.block() == newTarget)
                classOfEdgePassedBy = std::min(classOfEdgePassedBy, static_cast<uint8_t>(passedBy.edgeClass()));
        }
        if (classOfEdgePassedBy != passesNoEdgeBy)
            edge.m_class = std::max(edge.m_class, static_cast<EdgeClass>(classOfEdgePassedBy));
    });
}

ALWAYS_INLINE void Order::retargetPast(Block* from, unsigned successorIndex, Edge passedBy)
{
    retargetWithClass(from, successorIndex, passedBy.block(), [&](Edge& edge, const Block*) ALWAYS_INLINE_LAMBDA {
        edge.m_class = std::max(edge.m_class, passedBy.edgeClass());
    });
}

ALWAYS_INLINE void Order::retargetToBlockThatStandsForTarget(Block* from, unsigned successorIndex, Block* newTarget)
{
    retargetWithClass(from, successorIndex, newTarget, [](Edge&, const Block*) ALWAYS_INLINE_LAMBDA { });
}

template<typename Functor>
ALWAYS_INLINE void Order::retargetWithClass(Block* from, unsigned successorIndex, Block* newTarget, const Functor& raiseClass)
{
    Edge& edge = from->m_successors[successorIndex];
    Block* oldTarget = edge.m_block;
    if (oldTarget == newTarget)
        return;

    oldTarget->didLoseIncomingEdge(from, edge.m_class);
    if (newTarget->m_numberOfIncomingEdges)
        raiseClass(edge, oldTarget);

    edge.m_block = newTarget;
    newTarget->didGainIncomingEdge(from, edge.m_class);

    if (!oldTarget->m_phis.isEmpty() && !from->m_upsilons.isEmpty())
        dropUpsilonsHandedTo(oldTarget, from);
}

void threadJumps(Procedure&);
void mergeIdenticalBlocks(Procedure&);
Order* orderBlocks(Procedure&);

bool mayTestSumByOverflow(const Operation* branch);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
