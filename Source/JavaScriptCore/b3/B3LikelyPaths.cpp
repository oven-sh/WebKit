/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3LikelyPaths.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3LoopUtilities.h"
#include "B3Procedure.h"

namespace JSC { namespace B3 {

LikelyPathAnalysis::LikelyPathAnalysis(Procedure& proc, const NaturalLoops& loops)
    : m_proc(proc)
    , m_loops(loops)
    , m_postOrderNumber(proc.size())
    , m_immediateDominator(proc.size())
    , m_wayUp(proc.size())
{
}

void LikelyPathAnalysis::findBlocksOnEveryLikelyPath(const NaturalLoop& loop, IndexSet<BasicBlock*>& result)
{
    m_postOrder.shrink(0);
    m_iterationEnds.shrink(0);
    walkLikelyEdges(loop);

    if (m_iterationEnds.isEmpty())
        result.addAll(m_postOrder);
    else {
        computeImmediateDominators(loop);
        m_firstBlockWithFinalDominator = 0;
        BasicBlock* commonDominator = m_iterationEnds.first();
        for (BasicBlock* iterationEnd : m_iterationEnds)
            commonDominator = nearestCommonDominator(commonDominator, iterationEnd);
        for (BasicBlock* block = commonDominator; block != loop.header(); block = m_immediateDominator[block])
            result.add(block);
        result.add(loop.header());
    }

    for (BasicBlock* block : m_postOrder) {
        m_postOrderNumber[block] = std::nullopt;
        m_immediateDominator[block] = nullptr;
    }
    m_proc.count(Counter::StepsToCommonLikelyDominators, static_cast<uint32_t>(std::min<uint64_t>(std::exchange(m_stepsToCommonDominators, 0), std::numeric_limits<uint32_t>::max())));
}

bool LikelyPathAnalysis::isLikelyInnerEdge(const NaturalLoop& loop, FrequencyClass likeliestClassOfSource, const FrequentedBlock& successor) const
{
    return isLikelySuccessor(likeliestClassOfSource, successor) && successor.block() != loop.header() && m_loops.belongsTo(successor.block(), loop);
}

bool LikelyPathAnalysis::isIterationEnd(const NaturalLoop& loop, BasicBlock* block) const
{
    bool hasLikelyEdge = false;
    FrequencyClass likeliestClass = likeliestSuccessorClass(block);
    for (const FrequentedBlock& successor : block->successors()) {
        if (!isLikelySuccessor(likeliestClass, successor))
            continue;
        if (!isLikelyInnerEdge(loop, likeliestClass, successor))
            return true;
        hasLikelyEdge = true;
    }
    return !hasLikelyEdge;
}

void LikelyPathAnalysis::walkLikelyEdges(const NaturalLoop& loop)
{
    struct Frame {
        BasicBlock* block;
        FrequencyClass likeliestClass;
        unsigned nextSuccessorIndex;
    };
    Vector<Frame, 32> stack;

    constexpr unsigned notLeftYet = std::numeric_limits<unsigned>::max();
    m_mayHaveWaysUp = true;
    auto push = [&](BasicBlock* block) {
        m_postOrderNumber[block] = notLeftYet;
        if (isIterationEnd(loop, block))
            m_iterationEnds.append(block);
        stack.append(Frame { block, likeliestSuccessorClass(block), 0 });
    };

    push(loop.header());
    while (!stack.isEmpty()) {
        Frame& frame = stack.last();
        BasicBlock* block = frame.block;
        if (frame.nextSuccessorIndex == block->numSuccessors()) {
            m_postOrderNumber[block] = m_postOrder.size();
            m_postOrder.append(block);
            stack.removeLast();
            continue;
        }
        const FrequentedBlock& successor = block->successor(frame.nextSuccessorIndex++);
        if (!isLikelyInnerEdge(loop, frame.likeliestClass, successor))
            continue;
        if (!m_postOrderNumber[successor.block()]) {
            push(successor.block());
            continue;
        }
        if (*m_postOrderNumber[successor.block()] == notLeftYet) {
            const NaturalLoop* innerLoop = m_loops.headerOf(successor.block());
            m_mayHaveWaysUp &= innerLoop && m_loops.belongsTo(block, *innerLoop);
        }
    }
}

void LikelyPathAnalysis::linkToImmediateDominator(BasicBlock* block)
{
    BasicBlock* parent = m_immediateDominator[block];
    if (parent == block) {
        m_wayUp[block] = { 0, block };
        return;
    }
    const WayUp& ofParent = m_wayUp[parent];
    const WayUp& ofFarAncestor = m_wayUp[ofParent.farAncestor];
    bool areEquallyLong = ofParent.depth - ofFarAncestor.depth == ofFarAncestor.depth - m_wayUp[ofFarAncestor.farAncestor].depth;
    m_wayUp[block] = { ofParent.depth + 1, areEquallyLong ? ofFarAncestor.farAncestor : parent };
}

void LikelyPathAnalysis::computeImmediateDominators(const NaturalLoop& loop)
{
    m_immediateDominator[loop.header()] = loop.header();
    m_firstBlockWithWayUp = m_postOrder.size();
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned i = m_postOrder.size(); i--;) {
            BasicBlock* block = m_postOrder[i];
            m_firstBlockWithFinalDominator = i;
            FrequencyClass likeliestClass = likeliestSuccessorClass(block);
            for (const FrequentedBlock& successor : block->successors()) {
                if (!isLikelyInnerEdge(loop, likeliestClass, successor))
                    continue;
                // The header of an inner loop dominates the source of its back edges, so they change nothing. Meeting them costs a walk up the whole inner loop.
                const NaturalLoop* innerLoop = m_loops.headerOf(successor.block());
                if (innerLoop && m_loops.belongsTo(block, *innerLoop))
                    continue;
                BasicBlock*& dominator = m_immediateDominator[successor.block()];
                BasicBlock* candidate = dominator ? nearestCommonDominator(block, dominator) : block;
                changed |= candidate != dominator;
                dominator = candidate;
            }
        }
    }
}

BasicBlock* LikelyPathAnalysis::nearestCommonDominator(BasicBlock* a, BasicBlock* b)
{
    constexpr unsigned mostStepsBlockByBlock = 16;
    unsigned steps = 0;
    while (a != b) {
        if (++steps > mostStepsBlockByBlock && m_mayHaveWaysUp) {
            a = meetByWaysUp(a, b);
            break;
        }
        BasicBlock*& lower = *m_postOrderNumber[a] < *m_postOrderNumber[b] ? a : b;
        lower = m_immediateDominator[lower];
    }
    m_stepsToCommonDominators += steps;
    return a;
}

BasicBlock* LikelyPathAnalysis::meetByWaysUp(BasicBlock* a, BasicBlock* b)
{
    while (m_firstBlockWithWayUp > m_firstBlockWithFinalDominator)
        linkToImmediateDominator(m_postOrder[--m_firstBlockWithWayUp]);

    if (m_wayUp[a].depth < m_wayUp[b].depth)
        std::swap(a, b);
    unsigned depth = m_wayUp[b].depth;
    while (m_wayUp[a].depth > depth) {
        ++m_stepsToCommonDominators;
        a = m_wayUp[m_wayUp[a].farAncestor].depth >= depth ? m_wayUp[a].farAncestor : m_immediateDominator[a];
    }
    while (a != b) {
        ++m_stepsToCommonDominators;
        bool meetAtOrAboveFarAncestors = m_wayUp[a].farAncestor == m_wayUp[b].farAncestor;
        a = meetAtOrAboveFarAncestors ? m_immediateDominator[a] : m_wayUp[a].farAncestor;
        b = meetAtOrAboveFarAncestors ? m_immediateDominator[b] : m_wayUp[b].farAncestor;
    }
    return a;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
