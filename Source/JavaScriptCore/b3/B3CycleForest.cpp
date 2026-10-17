/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3CycleForest.h"

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlockInlines.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

class CycleForest::Builder {
public:
    Builder(Procedure& proc, CycleForest& forest)
        : m_proc(proc)
        , m_forest(forest)
        , m_number(proc.size(), 0u)
        , m_lowest(proc.size(), 0u)
        , m_isOnStack(proc.size(), false)
    {
    }

    void run()
    {
        Vector<BasicBlock*> blocks;
        blocks.reserveInitialCapacity(m_proc.size());
        for (BasicBlock* block : m_proc)
            blocks.append(block);
        decompose(blocks, none);
        while (!m_pending.isEmpty()) {
            Pending pending = m_pending.takeLast();
            decompose(pending.blocks, pending.cycle);
        }
        computePostOrder();
    }

private:
    struct Frame {
        BasicBlock* block;
        unsigned next;
    };

    struct Pending {
        unsigned cycle;
        Vector<BasicBlock*> blocks;
    };

    bool isIn(unsigned cycle, BasicBlock* block) const { return m_forest.m_innermost[block] == cycle; }

    void decompose(const Vector<BasicBlock*>& blocks, unsigned scope)
    {
        BasicBlock* scopeHeader = scope == none ? nullptr : m_forest.m_cycles[scope].header();
        for (BasicBlock* block : blocks)
            m_number[block] = 0;
        auto enter = [&](BasicBlock* block) {
            m_number[block] = m_lowest[block] = ++m_nextNumber;
            m_isOnStack[block] = true;
            m_stack.append(block);
            m_frames.append(Frame { block, 0 });
        };
        for (BasicBlock* start : blocks) {
            if (m_number[start])
                continue;
            enter(start);
            while (!m_frames.isEmpty()) {
                BasicBlock* block = m_frames.last().block;
                if (m_frames.last().next < block->numSuccessors()) {
                    BasicBlock* successor = block->successorBlock(m_frames.last().next++);
                    if (!isIn(scope, successor) || successor == scopeHeader)
                        continue;
                    if (!m_number[successor])
                        enter(successor);
                    else if (m_isOnStack[successor])
                        m_lowest[block] = std::min(m_lowest[block], m_number[successor]);
                    continue;
                }
                m_frames.removeLast();
                if (!m_frames.isEmpty())
                    m_lowest[m_frames.last().block] = std::min(m_lowest[m_frames.last().block], m_lowest[block]);
                if (m_lowest[block] != m_number[block])
                    continue;
                unsigned first = m_stack.size() - 1;
                while (m_stack[first] != block)
                    --first;
                auto members = m_stack.span().subspan(first);
                for (BasicBlock* member : members)
                    m_isOnStack[member] = false;
                if (members.size() > 1 || block->containsPredecessor(block))
                    addCycle(members, scope);
                m_stack.shrink(first);
            }
        }
    }

    void addCycle(std::span<BasicBlock* const> members, unsigned parent)
    {
        unsigned cycle = m_forest.m_cycles.size();
        m_forest.m_cycles.append(Cycle { });
        m_forest.m_cycles[cycle].parent = parent;
        m_forest.m_cycles[cycle].depth = m_forest.depth(parent) + 1;
        for (BasicBlock* member : members)
            m_forest.m_innermost[member] = cycle;

        Vector<BasicBlock*, 2> entries;
        for (BasicBlock* member : members) {
            for (BasicBlock* predecessor : member->predecessors()) {
                if (!isIn(cycle, predecessor)) {
                    entries.append(member);
                    break;
                }
            }
        }
        BasicBlock* header = chooseHeader(cycle, members, entries);
        auto& result = m_forest.m_cycles[cycle].entries;
        result.append(header);
        for (BasicBlock* entry : entries) {
            if (entry != header)
                result.append(entry);
        }
        if (members.size() == 1)
            return;
        Pending pending { cycle, { } };
        pending.blocks.reserveInitialCapacity(members.size() - 1);
        for (BasicBlock* member : members) {
            if (member != header)
                pending.blocks.append(member);
        }
        m_pending.append(WTF::move(pending));
    }

    BasicBlock* chooseHeader(unsigned cycle, std::span<BasicBlock* const> members, const Vector<BasicBlock*, 2>& entries)
    {
        if (entries.size() == 1)
            return entries[0];
        auto isBetter = [&](BasicBlock* candidate, BasicBlock* best) {
            if (!best)
                return true;
            bool isEntry = entries.contains(candidate);
            if (isEntry != entries.contains(best))
                return isEntry;
            return candidate->index() < best->index();
        };
        findPolls();
        BasicBlock* aboveLeavingTest = nullptr;
        BasicBlock* withoutTest = nullptr;
        for (BasicBlock* member : members) {
            if (!m_isPoll[member])
                continue;
            BasicBlock* header = member;
            bool isInInnerLoop = false;
            while (header->numPredecessors() == 1) {
                BasicBlock* predecessor = header->predecessor(0);
                if (!isIn(cycle, predecessor) || predecessor->numSuccessors() != 2)
                    break;
                BasicBlock* other = predecessor->successorBlock(predecessor->successorBlock(0) == header ? 1 : 0);
                if (other == header)
                    break;
                if (isIn(cycle, other)) {
                    isInInnerLoop = header == member;
                    break;
                }
                header = predecessor;
            }
            if (isInInnerLoop)
                continue;
            BasicBlock*& best = header == member ? withoutTest : aboveLeavingTest;
            if (isBetter(header, best))
                best = header;
        }
        if (aboveLeavingTest)
            return aboveLeavingTest;
        if (withoutTest)
            return withoutTest;

        BasicBlock* likely = nullptr;
        BasicBlock* any = nullptr;
        for (BasicBlock* entry : entries) {
            if (!any || entry->index() < any->index())
                any = entry;
            bool isLikely = false;
            for (BasicBlock* predecessor : entry->predecessors()) {
                if (isIn(cycle, predecessor) || !predecessor->frequency())
                    continue;
                for (auto& successor : predecessor->successors())
                    isLikely |= successor.block() == entry && successor.frequency() != FrequencyClass::Rare;
            }
            if (isLikely && (!likely || entry->index() < likely->index()))
                likely = entry;
        }
        if (likely)
            return likely;
        return any ? any : members[0];
    }

    void findPolls()
    {
        if (std::exchange(m_didFindPolls, true))
            return;
        m_isPoll = IndexMap<BasicBlock*, bool>(m_proc.size(), false);
        if (!m_proc.loopPollGenerator())
            return;
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                auto* patchpoint = value->as<PatchpointValue>();
                if (patchpoint && patchpoint->generator() == m_proc.loopPollGenerator()) {
                    m_isPoll[block] = true;
                    break;
                }
            }
        }
    }

    BasicBlock* targetInOrder(BasicBlock* from, BasicBlock* to) const
    {
        unsigned cycle = m_forest.m_innermost[to];
        unsigned common = m_forest.nearestCommon(m_forest.m_innermost[from], cycle);
        if (cycle == common)
            return m_forest.isHeader(to) ? nullptr : to;
        while (m_forest.m_cycles[cycle].parent != common)
            cycle = m_forest.m_cycles[cycle].parent;
        return m_forest.m_cycles[cycle].header();
    }

    void computePostOrder()
    {
        IndexMap<BasicBlock*, bool> isSeen(m_proc.size(), false);
        m_forest.m_postOrder.reserveInitialCapacity(m_proc.size());
        isSeen[m_proc[0]] = true;
        m_frames.append(Frame { m_proc[0], 0 });
        while (!m_frames.isEmpty()) {
            BasicBlock* block = m_frames.last().block;
            if (m_frames.last().next < block->numSuccessors()) {
                BasicBlock* target = targetInOrder(block, block->successorBlock(m_frames.last().next++));
                if (target && !isSeen[target]) {
                    isSeen[target] = true;
                    m_frames.append(Frame { target, 0 });
                }
                continue;
            }
            m_forest.m_postOrder.append(block);
            m_frames.removeLast();
        }
    }

    Procedure& m_proc;
    CycleForest& m_forest;
    IndexMap<BasicBlock*, unsigned> m_number;
    IndexMap<BasicBlock*, unsigned> m_lowest;
    IndexMap<BasicBlock*, bool> m_isOnStack;
    IndexMap<BasicBlock*, bool> m_isPoll;
    Vector<BasicBlock*, 32> m_stack;
    Vector<Frame, 32> m_frames;
    Vector<Pending> m_pending;
    unsigned m_nextNumber { 0 };
    bool m_didFindPolls { false };
};

CycleForest::CycleForest(Procedure& proc)
    : m_innermost(proc.size(), none)
{
    Builder builder(proc, *this);
    builder.run();
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
