/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3Dominators.h"
#include "B3Procedure.h"
#include <wtf/IndexMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace B3 {

class DominatorJumpPointers {
    WTF_MAKE_NONCOPYABLE(DominatorJumpPointers);
public:
    explicit DominatorJumpPointers(Procedure& proc)
        : m_proc(proc)
        , m_dominators(proc.dominators())
        , m_jump(proc.size(), nullptr)
        , m_leastFrequentBelowJump(proc.size(), nullptr)
        , m_depth(proc.size(), 0u)
    {
        for (BasicBlock* block : proc.cachedBlocksInPreOrder()) {
            BasicBlock* parent = m_dominators.idom(block);
            m_leastFrequentBelowJump[block] = block;
            if (!parent) {
                m_jump[block] = block;
                continue;
            }
            m_depth[block] = m_depth[parent] + 1;
            BasicBlock* middle = m_jump[parent];
            if (m_depth[parent] - m_depth[middle] != m_depth[middle] - m_depth[m_jump[middle]]) {
                m_jump[block] = parent;
                continue;
            }
            m_jump[block] = m_jump[middle];
            if (parent == middle)
                continue;
            if (m_leastFrequentBelowJump[parent]->frequency() < m_leastFrequentBelowJump[block]->frequency())
                m_leastFrequentBelowJump[block] = m_leastFrequentBelowJump[parent];
            if (m_leastFrequentBelowJump[middle]->frequency() < m_leastFrequentBelowJump[block]->frequency())
                m_leastFrequentBelowJump[block] = m_leastFrequentBelowJump[middle];
        }
    }

    bool isReachable(BasicBlock* block) const { return !!m_jump[block]; }

    unsigned depth(BasicBlock* block) const
    {
        ASSERT(isReachable(block));
        return m_depth[block];
    }

    BasicBlock* nearestCommonDominator(BasicBlock* a, BasicBlock* b) const
    {
        ASSERT(isReachable(a));
        ASSERT(isReachable(b));
        while (!m_dominators.dominates(a, b))
            a = m_dominators.dominates(m_jump[a], b) ? m_dominators.idom(a) : m_jump[a];
        return a;
    }

    BasicBlock* dominatorAtDepth(BasicBlock* block, unsigned targetDepth) const
    {
        ASSERT(isReachable(block));
        ASSERT(targetDepth <= m_depth[block]);
        while (m_depth[block] > targetDepth)
            block = m_depth[m_jump[block]] >= targetDepth ? m_jump[block] : m_dominators.idom(block);
        return block;
    }

    BasicBlock* leastFrequentBetween(BasicBlock* from, BasicBlock* to) const
    {
        ASSERT(isReachable(from));
        ASSERT(m_dominators.dominates(to, from));
        BasicBlock* least = nullptr;
        while (from != to) {
            bool canJump = m_depth[m_jump[from]] >= m_depth[to];
            BasicBlock* candidate = canJump ? m_leastFrequentBelowJump[from] : from;
            if (!least || candidate->frequency() < least->frequency())
                least = candidate;
            from = canJump ? m_jump[from] : m_dominators.idom(from);
        }
        return least;
    }

    unsigned dominatorTreePreOrder(BasicBlock* block)
    {
        ASSERT(isReachable(block));
        if (!m_dominatorTreePreOrder.size())
            computeDominatorTreePreOrder();
        return m_dominatorTreePreOrder[block];
    }

private:
    void computeDominatorTreePreOrder()
    {
        m_dominatorTreePreOrder.resize(m_proc.size());
        IndexMap<BasicBlock*, BasicBlock*> firstChild(m_proc.size(), nullptr);
        IndexMap<BasicBlock*, BasicBlock*> nextSibling(m_proc.size(), nullptr);
        for (BasicBlock* block : m_proc.cachedBlocksInPreOrder()) {
            if (BasicBlock* parent = m_dominators.idom(block))
                nextSibling[block] = std::exchange(firstChild[parent], block);
        }
        Vector<BasicBlock*, 16> worklist { m_proc.at(0) };
        for (unsigned next = 0; !worklist.isEmpty(); ++next) {
            BasicBlock* block = worklist.takeLast();
            m_dominatorTreePreOrder[block] = next;
            for (BasicBlock* child = firstChild[block]; child; child = nextSibling[child])
                worklist.append(child);
        }
    }

    Procedure& m_proc;
    Dominators& m_dominators;
    IndexMap<BasicBlock*, BasicBlock*> m_jump;
    IndexMap<BasicBlock*, BasicBlock*> m_leastFrequentBelowJump;
    IndexMap<BasicBlock*, unsigned> m_depth;
    IndexMap<BasicBlock*, unsigned> m_dominatorTreePreOrder;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
