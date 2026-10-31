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
    : m_loops(loops)
    , m_postOrderNumber(proc.size())
    , m_immediateDominator(proc.size())
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

    auto push = [&](BasicBlock* block) {
        m_postOrderNumber[block] = 0;
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
        if (isLikelyInnerEdge(loop, frame.likeliestClass, successor) && !m_postOrderNumber[successor.block()])
            push(successor.block());
    }
}

void LikelyPathAnalysis::computeImmediateDominators(const NaturalLoop& loop)
{
    m_immediateDominator[loop.header()] = loop.header();
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned i = m_postOrder.size(); i--;) {
            BasicBlock* block = m_postOrder[i];
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

BasicBlock* LikelyPathAnalysis::nearestCommonDominator(BasicBlock* a, BasicBlock* b) const
{
    while (a != b) {
        while (*m_postOrderNumber[a] < *m_postOrderNumber[b])
            a = m_immediateDominator[a];
        while (*m_postOrderNumber[b] < *m_postOrderNumber[a])
            b = m_immediateDominator[b];
    }
    return a;
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
