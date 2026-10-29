/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBlockCycles.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool BlockCycles::hasCycleWithSeveralEntries() const
{
    if (!m_cfg.hasCycle())
        return false;
    const Forest& forest = this->forest();
    for (unsigned cycle = 0; cycle < forest.size(); ++cycle) {
        if (!forest[cycle].isReducible())
            return true;
    }
    return false;
}

namespace {

class ForestBuilder {
public:
    using Forest = BlockCycles::Forest;

    ForestBuilder(const CFG& cfg, const Function* function)
        : m_cfg(cfg)
        , m_function(function)
        , m_numberOfBlocks(cfg.numNodes())
        , m_firstSuccessor(cfg.firstSuccessorOfBlocks().data())
        , m_successors(reinterpret_cast<const uint32_t*>(cfg.successorsOfAllBlocks().data()))
    {
        m_parts.innermost.fill(none, m_numberOfBlocks);
        m_innermost = m_parts.innermost.mutableSpan().data();
        m_frames.grow(m_numberOfBlocks);
    }

    void findOutermostCycles(Vector<uint32_t>& outermost, Vector<uint32_t>& members, Vector<uint32_t>& firstMember)
    {
        m_number.fill(0, m_numberOfBlocks);
        m_stack.grow(m_numberOfBlocks);
        uint32_t root = m_cfg.root().index();
        decompose(std::span<const uint32_t> { &root, 1 }, none, [&](std::span<const uint32_t> membersOfCycle) {
            uint32_t cycle = firstMember.size();
            firstMember.append(members.size());
            members.append(membersOfCycle);
            for (uint32_t member : membersOfCycle)
                m_innermost[member] = cycle;
        });
        firstMember.append(members.size());
        outermost = WTF::move(m_parts.innermost);
    }

    Forest::Parts run(std::span<const uint32_t> outermost, std::span<const uint32_t> members, std::span<const uint32_t> firstMember)
    {
        if (m_cfg.hasCycle()) {
            std::ranges::copy(outermost, m_innermost);
            m_number.grow(m_numberOfBlocks);
            m_stack.grow(m_numberOfBlocks);
            for (size_t cycle = 0; cycle + 1 < firstMember.size(); ++cycle)
                addCycle(members.subspan(firstMember[cycle], firstMember[cycle + 1] - firstMember[cycle]), none);
            while (!m_pending.isEmpty()) {
                Pending pending = m_pending.takeLast();
                uint32_t* number = m_number.mutableSpan().data();
                for (uint32_t block : pending.blocks)
                    number[block] = 0;
                decompose(pending.blocks.span(), pending.cycle, [&](std::span<const uint32_t> membersOfCycle) {
                    addCycle(membersOfCycle, pending.cycle);
                });
            }
        }
        computePostOrder();
        return WTF::move(m_parts);
    }

private:
    static constexpr uint32_t none = Forest::none;
    static constexpr uint32_t isFinished = UINT32_MAX;

    struct Frame {
        uint32_t block;
        uint32_t nextEdge;
        uint32_t endOfEdges;
        uint32_t lowest;
        bool hasSelfEdge;
    };

    struct Pending {
        uint32_t cycle;
        Vector<uint32_t> blocks;
    };

    void decompose(std::span<const uint32_t> blocks, uint32_t scope, NOESCAPE const Invocable<void(std::span<const uint32_t>)> auto& didFindCycle)
    {
        uint32_t scopeHeader = scope != none ? m_parts.cycles[scope].header().index() : none;
        uint32_t* number = m_number.mutableSpan().data();
        uint32_t* stack = m_stack.mutableSpan().data();
        Frame* frames = m_frames.mutableSpan().data();
        const uint32_t* firstSuccessor = m_firstSuccessor;
        const uint32_t* successors = m_successors;
        const uint32_t* innermost = m_innermost;
        size_t stackSize = 0;
        for (uint32_t start : blocks) {
            if (number[start])
                continue;
            size_t depth = 0;
            auto enter = [&](uint32_t block) ALWAYS_INLINE_LAMBDA {
                uint32_t numberOfBlock = ++m_nextNumber;
                number[block] = numberOfBlock;
                stack[stackSize++] = block;
                frames[depth++] = { block, firstSuccessor[block], firstSuccessor[block + 1], numberOfBlock, false };
            };
            enter(start);
            while (depth) {
                Frame& frame = frames[depth - 1];
                if (frame.nextEdge != frame.endOfEdges) {
                    uint32_t successor = successors[frame.nextEdge++];
                    if (innermost[successor] != scope || successor == scopeHeader)
                        continue;
                    uint32_t numberOfSuccessor = number[successor];
                    if (!numberOfSuccessor) {
                        enter(successor);
                        continue;
                    }
                    frame.hasSelfEdge |= successor == frame.block;
                    frame.lowest = std::min(frame.lowest, numberOfSuccessor);
                    continue;
                }
                uint32_t block = frame.block;
                uint32_t lowest = frame.lowest;
                bool hasSelfEdge = frame.hasSelfEdge;
                if (--depth)
                    frames[depth - 1].lowest = std::min(frames[depth - 1].lowest, lowest);
                if (lowest != number[block])
                    continue;
                size_t first = stackSize - 1;
                while (stack[first] != block)
                    --first;
                std::span<const uint32_t> members { stack + first, stackSize - first };
                for (uint32_t member : members)
                    number[member] = isFinished;
                if (members.size() > 1 || hasSelfEdge)
                    didFindCycle(members);
                stackSize = first;
            }
        }
    }

    void addCycle(std::span<const uint32_t> members, uint32_t parent)
    {
        uint32_t cycle = m_parts.cycles.size();
        m_parts.cycles.append(Forest::Cycle { });
        unsigned depth = parent == none ? 1 : m_parts.cycles[parent].depth + 1;
        for (uint32_t member : members)
            m_innermost[member] = cycle;

        Vector<BlockID, 2> entries;
        for (uint32_t member : members) {
            for (Edge edge : m_cfg.incomingEdges(BlockID(member))) {
                if (m_innermost[edge.from.index()] != cycle) {
                    entries.append(BlockID(member));
                    break;
                }
            }
        }
        BlockID header = entries.size() == 1 ? entries[0] : chooseHeader(members, entries);
        Forest::Cycle& added = m_parts.cycles[cycle];
        added.parent = parent;
        added.depth = depth;
        added.entries.append(header);
        for (BlockID entry : entries) {
            if (entry != header)
                added.entries.append(entry);
        }
        if (members.size() == 1)
            return;
        Pending pending { cycle, { } };
        pending.blocks.reserveInitialCapacity(members.size() - 1);
        for (uint32_t member : members) {
            if (member != header.index())
                pending.blocks.append(member);
        }
        m_pending.append(WTF::move(pending));
    }

    BlockID chooseHeader(std::span<const uint32_t> members, const Vector<BlockID, 2>& entries) const
    {
        auto rankOf = [&](BlockID block) {
            return std::pair { m_function ? m_function->at(block).frequency.predicted() : BlockFrequency::Normal, m_cfg.reversePostOrderIndex(block) };
        };
        if (!entries.isEmpty())
            return *std::ranges::min_element(entries, { }, rankOf);
        return BlockID(*std::ranges::min_element(members, { }, [&](uint32_t block) {
            return rankOf(BlockID(block));
        }));
    }

    uint32_t nearestCommon(uint32_t a, uint32_t b) const
    {
        const Forest::Cycle* cycles = m_parts.cycles.span().data();
        while (a != b) {
            if (a == none || b == none)
                return none;
            if (cycles[a].depth >= cycles[b].depth)
                a = cycles[a].parent;
            else
                b = cycles[b].parent;
        }
        return a;
    }

    uint32_t targetInOrder(uint32_t from, uint32_t to) const
    {
        uint32_t cycle = m_innermost[to];
        if (cycle == none)
            return to;
        const Forest::Cycle* cycles = m_parts.cycles.span().data();
        uint32_t cycleOfSource = m_innermost[from];
        uint32_t common = cycleOfSource == cycle ? cycle : nearestCommon(cycleOfSource, cycle);
        if (cycle == common)
            return cycles[cycle].header().index() == to ? none : to;
        while (cycles[cycle].parent != common)
            cycle = cycles[cycle].parent;
        return cycles[cycle].header().index();
    }

    void computePostOrder()
    {
        Vector<uint8_t, 256> isSeenStorage(FillWith { }, m_numberOfBlocks, static_cast<uint8_t>(false));
        uint8_t* isSeen = isSeenStorage.mutableSpan().data();
        Frame* frames = m_frames.mutableSpan().data();
        const uint32_t* firstSuccessor = m_firstSuccessor;
        const uint32_t* successors = m_successors;
        bool hasCycles = !m_parts.cycles.isEmpty();
        m_parts.postOrder.grow(m_cfg.reversePostOrder().size());
        BlockID* postOrder = m_parts.postOrder.mutableSpan().data();
        size_t size = 0;

        uint32_t root = m_cfg.root().index();
        isSeen[root] = true;
        size_t depth = 1;
        frames[0] = { root, firstSuccessor[root], firstSuccessor[root + 1], 0, false };
        while (depth) {
            Frame& frame = frames[depth - 1];
            if (frame.nextEdge == frame.endOfEdges) {
                postOrder[size++] = BlockID(frame.block);
                --depth;
                continue;
            }
            uint32_t target = successors[frame.nextEdge++];
            if (hasCycles) {
                target = targetInOrder(frame.block, target);
                if (target == none)
                    continue;
            }
            if (isSeen[target])
                continue;
            isSeen[target] = true;
            frames[depth++] = { target, firstSuccessor[target], firstSuccessor[target + 1], 0, false };
        }
        m_parts.postOrder.shrink(size);
    }

    const CFG& m_cfg;
    const Function* m_function;
    unsigned m_numberOfBlocks;
    const uint32_t* m_firstSuccessor;
    const uint32_t* m_successors;
    Forest::Parts m_parts;
    uint32_t* m_innermost { nullptr };
    Vector<uint32_t, 256> m_number;
    Vector<uint32_t, 256> m_stack;
    Vector<Frame, 128> m_frames;
    Vector<Pending> m_pending;
    uint32_t m_nextNumber { 0 };
};

} // anonymous namespace

BlockCycles::BlockCycles(CFG& cfg, const Function* function)
    : m_cfg(cfg)
    , m_functionWithFrequencies(function)
{
    if (cfg.hasCycle())
        ForestBuilder(cfg, function).findOutermostCycles(m_outermost, m_membersOfOutermostCycles, m_firstMemberOfOutermostCycle);
}

void BlockCycles::computeForest() const
{
    m_forest.emplace(m_cfg, ForestBuilder(m_cfg, m_functionWithFrequencies).run(m_outermost.span(), m_membersOfOutermostCycles.span(), m_firstMemberOfOutermostCycle.span()));
    m_membersOfOutermostCycles = { };
    m_firstMemberOfOutermostCycle = { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
