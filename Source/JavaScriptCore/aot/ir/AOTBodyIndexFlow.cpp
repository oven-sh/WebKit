/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBodyIndexBuilder.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

template<typename Worker>
static Vector<uint32_t>& depthsInDominatorTree(Worker& worker)
{
    return worker.sizesOfSubtrees;
}

template<typename Worker>
static Vector<uint32_t>& skipsInDominatorTree(Worker& worker)
{
    return worker.nextNumbersBelow;
}

void BodyIndexBuilder::computeDominators()
{
    uint32_t numberOfBlocks = static_cast<uint32_t>(m_blocks.size());
    Vector<uint32_t>& depths = depthsInDominatorTree(m_worker);
    Vector<uint32_t>& skips = skipsInDominatorTree(m_worker);
    depths.fill(0, numberOfBlocks);
    skips.fill(entryBlock.index(), numberOfBlocks);
    m_blocks[entryBlock.index()].flags |= BodyIndex::IsReachable;

    BlockID source = entryBlock;
    for (BodyFlowEdge edge : m_worker.edges) {
        if (!(m_blocks[edge.from.index()].flags & BodyIndex::IsReachable))
            continue;

        if (edge.from != source) {
            source = edge.from;
            uint32_t dominator = m_blocks[source.index()].immediateDominator.index();
            uint32_t middle = skips[dominator];
            depths[source.index()] = depths[dominator] + 1;
            skips[source.index()] = depths[dominator] - depths[middle] == depths[middle] - depths[skips[middle]] ? skips[middle] : dominator;
        }

        if (edge.to <= edge.from)
            continue;
        BlockPlace& target = m_blocks[edge.to.index()];
        if (target.flags & BodyIndex::IsReachable) {
            target.immediateDominator = nearestCommonDominator(target.immediateDominator, edge.from);
            continue;
        }
        target.flags |= BodyIndex::IsReachable;
        target.immediateDominator = edge.from;
    }

    m_worker.retreatingEdges.removeAllMatching([&](const BodyFlowEdge& edge) {
        return !(m_blocks[edge.from.index()].flags & BodyIndex::IsReachable);
    });

    for (BodyFlowEdge edge : m_worker.retreatingEdges) {
        bool targetIsReachable = m_blocks[edge.to.index()].flags & BodyIndex::IsReachable;
        if (!targetIsReachable || nearestCommonDominator(edge.to, edge.from) != edge.to) {
            computeDominatorsOfIrreducibleFlow();
            return;
        }
    }
}

BlockID BodyIndexBuilder::nearestCommonDominator(BlockID a, BlockID b) const
{
    const Vector<uint32_t>& skips = skipsInDominatorTree(m_worker);
    while (a != b) {
        BlockID& later = a > b ? a : b;
        BlockID earlier = a > b ? b : a;
        BlockID skip(skips[later.index()]);
        later = skip >= earlier ? skip : m_blocks[later.index()].immediateDominator;
    }
    return a;
}

void BodyIndexBuilder::computeDominatorsOfIrreducibleFlow()
{
    m_index->m_hasIrreducibleFlow = true;

    for (BodyFlowEdge edge : m_worker.retreatingEdges)
        RELEASE_ASSERT(m_blocks[edge.to.index()].flags & BodyIndex::IsReachable);

    skipsInDominatorTree(m_worker).fill(entryBlock.index(), m_blocks.size());

    bool didChange;
    do {
        didChange = false;
        for (BodyFlowEdge edge : m_worker.edges) {
            if (edge.to == entryBlock || !(m_blocks[edge.from.index()].flags & BodyIndex::IsReachable))
                continue;
            BlockPlace& target = m_blocks[edge.to.index()];
            BlockID dominator = nearestCommonDominator(target.immediateDominator, edge.from);
            if (dominator == target.immediateDominator)
                continue;
            target.immediateDominator = dominator;
            didChange = true;
        }
    } while (didChange);

    treatBodyAsOneLoop();
}

void BodyIndexBuilder::numberDominatorTree()
{
    uint32_t numberOfBlocks = static_cast<uint32_t>(m_blocks.size());
    Vector<uint32_t>& sizesOfSubtrees = m_worker.sizesOfSubtrees;
    Vector<uint32_t>& nextNumbersBelow = m_worker.nextNumbersBelow;
    sizesOfSubtrees.fill(0, numberOfBlocks);
    nextNumbersBelow.resize(numberOfBlocks);

    for (uint32_t block = numberOfBlocks; block--;) {
        const BlockPlace& place = m_blocks[block];
        if (!(place.flags & BodyIndex::IsReachable))
            continue;
        ++sizesOfSubtrees[block];
        if (place.immediateDominator)
            sizesOfSubtrees[place.immediateDominator.index()] += sizesOfSubtrees[block];
    }

    for (uint32_t block = 0; block < numberOfBlocks; ++block) {
        BlockPlace& place = m_blocks[block];
        if (!(place.flags & BodyIndex::IsReachable))
            continue;
        uint32_t number = 0;
        if (place.immediateDominator) {
            uint32_t& nextNumber = nextNumbersBelow[place.immediateDominator.index()];
            number = nextNumber;
            nextNumber += sizesOfSubtrees[block];
        }
        nextNumbersBelow[block] = number + 1;
        place.dominatorNumber = number;
        place.lastDominatorNumberBelow = number + sizesOfSubtrees[block] - 1;
        m_blocksInDominatorOrder[number] = BlockID(block);
    }
}

void BodyIndexBuilder::findLoops()
{
    if (m_index->m_hasIrreducibleFlow || m_worker.retreatingEdges.isEmpty())
        return;

    Vector<BodyLoop>& loops = m_worker.loops;
    ASSERT(loops.isEmpty());
    for (BodyFlowEdge edge : m_worker.retreatingEdges)
        m_blocks[edge.to.index()].flags |= BodyIndex::IsLoopHead;
    for (uint32_t block = 0; block < m_blocks.size(); ++block) {
        BlockPlace& place = m_blocks[block];
        if (!(place.flags & BodyIndex::IsLoopHead))
            continue;
        place.loop = LoopID(loops.size());
        loops.append(BodyLoop { BlockID(block), BlockID(block), LoopID(), 0 });
    }
    uint32_t numberOfLoops = static_cast<uint32_t>(loops.size());

    collectPredecessors();
    auto predecessorsOf = [&](BlockID block) {
        uint32_t first = m_worker.firstPredecessor[block.index()];
        return m_worker.predecessors.span().subspan(first, m_worker.firstPredecessor[block.index() + 1] - first);
    };

    Vector<uint32_t>& loopsAround = m_worker.sizesOfSubtrees;
    loopsAround.resize(numberOfLoops);
    for (uint32_t loop = 0; loop < numberOfLoops; ++loop)
        loopsAround[loop] = loop;
    auto outermostLoopFoundAround = [&](uint32_t loop) {
        while (loopsAround[loop] != loop) {
            loopsAround[loop] = loopsAround[loopsAround[loop]];
            loop = loopsAround[loop];
        }
        return loop;
    };

    Vector<BlockID>& worklist = m_worker.worklist;
    for (uint32_t current = numberOfLoops; current--;) {
        BodyLoop& loop = loops[current];
        auto addToLoop = [&](BlockID block) {
            BlockPlace& place = m_blocks[block.index()];
            if (!place.loop) {
                ASSERT(block > loop.head);
                place.loop = LoopID(current);
                loop.last = std::max(loop.last, block);
                worklist.append(block);
                return;
            }

            uint32_t inner = outermostLoopFoundAround(place.loop.index());
            if (inner == current)
                return;
            ASSERT(inner > current);
            loopsAround[inner] = current;
            loops[inner].parent = LoopID(current);
            loop.last = std::max(loop.last, loops[inner].last);
            worklist.append(loops[inner].head);
        };

        for (BlockID predecessor : predecessorsOf(loop.head)) {
            if (predecessor >= loop.head)
                addToLoop(predecessor);
        }
        while (!worklist.isEmpty()) {
            for (BlockID predecessor : predecessorsOf(worklist.takeLast()))
                addToLoop(predecessor);
        }
    }

    for (BodyLoop& loop : loops)
        loop.depth = loop.parent ? loops[loop.parent.index()].depth + 1 : 1;
}

void BodyIndexBuilder::collectPredecessors()
{
    uint32_t numberOfBlocks = static_cast<uint32_t>(m_blocks.size());

    m_worker.firstPredecessor.fill(0, numberOfBlocks + 2);
    std::span<uint32_t> nextPredecessor = m_worker.firstPredecessor.mutableSpan().subspan(1);

    for (BodyFlowEdge edge : m_worker.edges) {
        if (m_blocks[edge.from.index()].flags & BodyIndex::IsReachable)
            ++nextPredecessor[edge.to.index() + 1];
    }
    for (uint32_t block = 0; block < numberOfBlocks; ++block)
        nextPredecessor[block + 1] += nextPredecessor[block];

    m_worker.predecessors.resize(nextPredecessor[numberOfBlocks]);
    for (BodyFlowEdge edge : m_worker.edges) {
        if (m_blocks[edge.from.index()].flags & BodyIndex::IsReachable)
            m_worker.predecessors[nextPredecessor[edge.to.index()]++] = edge.from;
    }
}

void BodyIndexBuilder::treatBodyAsOneLoop()
{
    ASSERT(m_worker.loops.isEmpty());
    LoopID wholeBody(0u);
    for (BlockPlace& place : m_blocks)
        place.loop = wholeBody;
    m_blocks[entryBlock.index()].flags |= BodyIndex::IsLoopHead;
    m_worker.loops.append(BodyLoop { entryBlock, BlockID(m_blocks.size() - 1), LoopID(), 1 });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
