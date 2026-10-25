/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3CycleForest.h"

#if ENABLE(B3_JIT)

#include "B3BasicBlockInlines.h"
#include "B3LoopUtilities.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"

namespace JSC { namespace B3 {

namespace {

class HeaderChooser {
public:
    explicit HeaderChooser(Procedure& proc)
        : m_proc(proc)
    {
    }

    template<typename IsInCycle>
    BasicBlock* operator()(std::span<BasicBlock* const> members, const Vector<BasicBlock*, 2>& entries, const IsInCycle& isInCycle) const
    {
        ASSERT(entries.size() != 1);
        if (BasicBlock* header = findPollingHeader(members, entries, isInCycle))
            return header;
        if (BasicBlock* entry = findPreferredEntry(entries, isInCycle))
            return entry;
        return members[0];
    }

private:
    template<typename IsInCycle>
    BasicBlock* findPollingHeader(std::span<BasicBlock* const> members, const Vector<BasicBlock*, 2>& entries, const IsInCycle& isInCycle) const
    {
        findLikelyBlocks();
        auto isBetter = [&](BasicBlock* candidate, BasicBlock* best) {
            if (!best)
                return true;
            // All that enters a cycle counts as entering at its header: a hot loop and its rare copy that lead into each other are one cycle.
            if (m_isLikely[candidate] != m_isLikely[best])
                return m_isLikely[candidate];
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
                if (predecessor == member || !isInCycle(predecessor) || predecessor->numSuccessors() != 2)
                    break;
                BasicBlock* other = predecessor->successorBlock(predecessor->successorBlock(0) == header ? 1 : 0);
                if (other == header)
                    break;
                if (isInCycle(other)) {
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
        return aboveLeavingTest ? aboveLeavingTest : withoutTest;
    }

    template<typename IsInCycle>
    static BasicBlock* findPreferredEntry(const Vector<BasicBlock*, 2>& entries, const IsInCycle& isInCycle)
    {
        BasicBlock* likely = nullptr;
        BasicBlock* any = nullptr;
        for (BasicBlock* entry : entries) {
            if (!any || entry->index() < any->index())
                any = entry;
            bool isLikely = false;
            for (BasicBlock* predecessor : entry->predecessors()) {
                if (isInCycle(predecessor))
                    continue;
                for (const FrequentedBlock& successor : predecessor->successors())
                    isLikely |= successor.block() == entry && !successor.isRare();
            }
            if (isLikely && (!likely || entry->index() < likely->index()))
                likely = entry;
        }
        return likely ? likely : any;
    }

    void findLikelyBlocks() const
    {
        if (std::exchange(m_didFindLikelyBlocks, true))
            return;
        m_isLikely = IndexMap<BasicBlock*, bool>(m_proc.size(), false);
        Vector<BasicBlock*, 16> worklist { m_proc.at(0) };
        m_isLikely[m_proc.at(0)] = true;
        while (!worklist.isEmpty()) {
            for (const FrequentedBlock& successor : worklist.takeLast()->successors()) {
                if (successor.isRare() || std::exchange(m_isLikely[successor.block()], true))
                    continue;
                worklist.append(successor.block());
            }
        }
    }

    void findPolls() const
    {
        if (std::exchange(m_didFindPolls, true))
            return;
        m_isPoll = IndexMap<BasicBlock*, bool>(m_proc.size(), false);
        if (!m_proc.hasEverAdded(Patchpoint))
            return;
        for (BasicBlock* block : m_proc) {
            for (Value* value : *block) {
                RELEASE_ASSERT(value);
                if (isLoopPoll(value)) {
                    m_isPoll[block] = true;
                    break;
                }
            }
        }
    }

    Procedure& m_proc;
    mutable IndexMap<BasicBlock*, bool> m_isPoll;
    mutable IndexMap<BasicBlock*, bool> m_isLikely;
    mutable bool m_didFindPolls { false };
    mutable bool m_didFindLikelyBlocks { false };
};

} // anonymous namespace

CycleForest::CycleForest(Procedure& proc)
    : WTF::CycleForest<CFG>(proc.cfg(), HeaderChooser(proc))
{
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
