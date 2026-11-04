/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTSweepValueSets.h"
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

// The reached blocks and the edges that are taken, handler edges among them: an order in which every block stands behind the blocks that its edges lead forward to, and the loops.
// A loop is named by the number of its head. The flow may have any shape: a loop with several ways in has the head that the search met first (Havlak, "Nesting of Reducible and Irreducible Loops").
class SweepFlow {
    WTF_MAKE_NONCOPYABLE(SweepFlow);
public:
    static constexpr uint32_t none = UINT32_MAX;

    SweepFlow() = default;

    // The lists hold the taken edges alone: those that lead to block b are incomingEdges[firstIncomingEdge[b] .. firstIncomingEdge[b + 1]).
    void compute(const Code&, std::span<const uint32_t> firstIncomingEdge, std::span<const EdgeID> incomingEdges, const SweepBits& blocksAskedAbout);

    std::span<const BlockID> blocksBehindTheirSuccessors() const LIFETIME_BOUND { return m_order.span(); }
    bool mayLeadToBlockAskedAbout(BlockID block) const { return m_blocksThatMayLeadToBlocksAskedAbout.quickGet(block.index()); }
    uint32_t numberOfBlocks() const { return static_cast<uint32_t>(m_blockAtNumber.size()); }
    bool hasLoops() const { return m_hasLoops; }
    // Every loop is entered at its head alone.
    bool isReducible() const { return m_isReducible; }

    bool isHeadOfLoop(uint32_t number) const { return m_hasLoops && m_headsOfLoops.quickGet(number); }
    BlockID headOf(uint32_t loop) const { return m_blockAtNumber[loop]; }
    // The innermost one. A head lies in its own loop. An outer loop has the lower number.
    uint32_t loopAround(BlockID block) const
    {
        if (!m_hasLoops)
            return none;
        uint32_t number = m_numberOfBlock[block.index()];
        return m_headsOfLoops.quickGet(number) ? number : m_loopAroundNumber[number];
    }
    uint32_t loopAroundLoop(uint32_t loop) const { return m_loopAroundNumber[loop]; }
    // The head of the outermost loop that holds `to` and not `from`. Null: there is none.
    BlockID headOfOutermostLoopEntered(BlockID from, BlockID to) const
    {
        uint32_t entered = none;
        for (uint32_t loop = loopAround(to); loop != none && !holds(loop, from); loop = m_loopAroundNumber[loop])
            entered = loop;
        return entered == none ? BlockID() : m_blockAtNumber[entered];
    }

private:
    struct BlockOnPath {
        BlockID block;
        uint32_t nextEdge { 0 };
    };
    // A way into a loop that passes its head by, noted at the head of the loop around.
    struct WayInPastHead {
        uint32_t from;
        uint32_t next;
    };

    // In the tree of the search.
    bool isAboveOrSame(uint32_t upper, uint32_t lower) const { return upper <= lower && lower <= m_lastNumberBelow[upper]; }
    bool holds(uint32_t loop, BlockID block) const
    {
        if (!isAboveOrSame(loop, m_numberOfBlock[block.index()]))
            return false;
        for (uint32_t around = loopAround(block); around != none && around >= loop; around = m_loopAroundNumber[around]) {
            if (around == loop)
                return true;
        }
        return false;
    }
    // The head of the outermost loop found so far that holds the block, or the block itself.
    uint32_t outermostFoundAround(uint32_t number)
    {
        while (m_foundAround[number] != number) {
            m_foundAround[number] = m_foundAround[m_foundAround[number]];
            number = m_foundAround[number];
        }
        return number;
    }
    void findLoops(const Code&, std::span<const uint32_t> firstIncomingEdge, std::span<const EdgeID> incomingEdges);

    Vector<BlockID> m_order;
    Vector<BlockOnPath> m_path;
    // By block: its place in the order in which the search came to the blocks.
    Vector<uint32_t> m_numberOfBlock;
    // By that number.
    Vector<BlockID> m_blockAtNumber;
    // `none` while the search is below the block.
    Vector<uint32_t> m_lastNumberBelow;
    Vector<uint32_t> m_loopAroundNumber;
    SweepBits m_headsOfLoops;
    Vector<uint32_t> m_foundAround;
    // One more than the head of the last loop that the block, or the loop that it is the head of, was listed for.
    Vector<uint32_t> m_lastLoopListedFor;
    Vector<uint32_t> m_firstWayInPastHead;
    Vector<WayInPastHead> m_waysInPastHeads;
    Vector<uint32_t> m_members;
    SweepBits m_blocksThatMayLeadToBlocksAskedAbout;
    bool m_hasLoops { false };
    bool m_isReducible { true };
};

inline void SweepFlow::compute(const Code& code, std::span<const uint32_t> firstIncomingEdge, std::span<const EdgeID> incomingEdges, const SweepBits& blocksAskedAbout)
{
    m_numberOfBlock.fill(none, code.numberOfBlocks());
    m_blockAtNumber.shrink(0);
    m_lastNumberBelow.shrink(0);
    m_order.shrink(0);
    m_path.shrink(0);
    m_blocksThatMayLeadToBlocksAskedAbout.clearAll();
    m_blocksThatMayLeadToBlocksAskedAbout.ensureSize(code.numberOfBlocks());
    m_hasLoops = false;
    m_isReducible = true;

    auto begin = [&](BlockID block) {
        m_numberOfBlock[block.index()] = numberOfBlocks();
        m_blockAtNumber.append(block);
        m_lastNumberBelow.append(none);
        if (blocksAskedAbout.quickGet(block.index()))
            m_blocksThatMayLeadToBlocksAskedAbout.quickSet(block.index());
    };
    for (const EntryOfCode& entry : code.entries()) {
        if (m_numberOfBlock[entry.block.index()] != none)
            continue;
        begin(entry.block);
        BlockID from = entry.block;
        uint32_t nextEdge = 0;
        for (;;) {
            const Terminator& terminator = code.at(from).terminator;
            uint32_t numberOfEdges = terminator.numberOfEdges;
            BlockID below;
            while (nextEdge <= numberOfEdges) {
                EdgeID edge = nextEdge < numberOfEdges ? terminator.edge(nextEdge) : code.handlerOf(from);
                ++nextEdge;
                if (!edge)
                    break;
                BlockID to = code.at(edge).to;
                uint32_t number = m_numberOfBlock[to.index()];
                if (number == none) {
                    below = to;
                    break;
                }
                if (m_lastNumberBelow[number] == none) {
                    m_hasLoops = true;
                    m_blocksThatMayLeadToBlocksAskedAbout.quickSet(from.index());
                } else if (m_blocksThatMayLeadToBlocksAskedAbout.quickGet(to.index()))
                    m_blocksThatMayLeadToBlocksAskedAbout.quickSet(from.index());
            }
            if (below) {
                begin(below);
                m_path.append({ from, nextEdge });
                from = below;
                nextEdge = 0;
                continue;
            }
            m_lastNumberBelow[m_numberOfBlock[from.index()]] = numberOfBlocks() - 1;
            m_order.append(from);
            if (m_path.isEmpty())
                break;
            BlockOnPath above = m_path.takeLast();
            if (m_blocksThatMayLeadToBlocksAskedAbout.quickGet(from.index()))
                m_blocksThatMayLeadToBlocksAskedAbout.quickSet(above.block.index());
            from = above.block;
            nextEdge = above.nextEdge;
        }
    }
    if (m_hasLoops)
        findLoops(code, firstIncomingEdge, incomingEdges);
}

// Inner loops first: a head has a higher number than the heads of the loops around it. A loop that has been found counts as one block, its head, from then on.
inline void SweepFlow::findLoops(const Code& code, std::span<const uint32_t> firstIncomingEdge, std::span<const EdgeID> incomingEdges)
{
    uint32_t count = numberOfBlocks();
    m_loopAroundNumber.fill(none, count);
    m_headsOfLoops.clearAll();
    m_headsOfLoops.ensureSize(count);
    m_foundAround.resize(count);
    for (uint32_t number = 0; number < count; ++number)
        m_foundAround[number] = number;
    m_lastLoopListedFor.fill(0, count);
    m_firstWayInPastHead.fill(none, count);
    m_waysInPastHeads.shrink(0);

    auto forEachBlockThatLeadsTo = [&](uint32_t number, NOESCAPE const Invocable<void(uint32_t)> auto& functor) {
        uint32_t block = m_blockAtNumber[number].index();
        for (EdgeID edge : incomingEdges.subspan(firstIncomingEdge[block], firstIncomingEdge[block + 1] - firstIncomingEdge[block])) {
            uint32_t from = m_numberOfBlock[code.at(edge).from.index()];
            RELEASE_ASSERT(from != none);
            functor(from);
        }
    };

    for (uint32_t head = count; head--;) {
        m_members.shrink(0);
        auto list = [&](uint32_t member) {
            if (member == head || m_lastLoopListedFor[member] == head + 1)
                return;
            m_lastLoopListedFor[member] = head + 1;
            m_members.append(member);
        };

        bool isHead = false;
        forEachBlockThatLeadsTo(head, [&](uint32_t from) {
            if (!isAboveOrSame(head, from))
                return;
            isHead = true;
            list(outermostFoundAround(from));
        });
        if (!isHead)
            continue;
        m_headsOfLoops.quickSet(head);

        // Whatever leads to a member without passing the head is a member too, if the search came to it below the head. Anything else enters the loop past its head.
        auto lookAtBlockThatLeadsToMember = [&](uint32_t from) {
            uint32_t outermost = outermostFoundAround(from);
            if (isAboveOrSame(head, outermost)) {
                list(outermost);
                return;
            }
            m_isReducible = false;
            m_waysInPastHeads.append({ outermost, m_firstWayInPastHead[head] });
            m_firstWayInPastHead[head] = static_cast<uint32_t>(m_waysInPastHeads.size() - 1);
        };
        for (size_t index = 0; index < m_members.size(); ++index) {
            uint32_t member = m_members[index];
            forEachBlockThatLeadsTo(member, [&](uint32_t from) {
                if (!isAboveOrSame(member, from))
                    lookAtBlockThatLeadsToMember(from);
            });
            for (uint32_t way = m_firstWayInPastHead[member]; way != none; way = m_waysInPastHeads[way].next)
                lookAtBlockThatLeadsToMember(m_waysInPastHeads[way].from);
        }
        for (uint32_t member : m_members) {
            m_loopAroundNumber[member] = head;
            m_foundAround[member] = head;
        }
    }
}

// When the block ends the program has let go of the value.
struct PlaceWhereSweptValueIsLetGo {
    ValueID value;
    BlockID block;
    // No word of the program says so: a call that was walked where it stands alone held the value.
    bool isBehindCopiedBody { false };
    // The value is another name of what the hint names, which is made in this block. A way that comes by that block and not by the name never makes the value. Null: the hint names the value.
    BlockID blockOfValueNamedByHint { };
};

// What is known of one value that is let go of at one block.
struct WayOfSweptValueLetGo {
    // That of any other value reads as empty.
    ValueID value;
    // With isRead: the last operation of the block that reads the value. Null: the end of the block does.
    ValueID lastReader;
    bool isRead { false };
    bool isLiveAtHead { false };
    bool isLiveAtEnd { false };
    // Nothing has yet to read the value, and a place where the program has let go of it lies ahead.
    bool isLetGoAtHead { false };
    bool isLetGoAtEnd { false };
    // From a block that goes on to read the value another way.
    bool isEnteredFromReader { false };
    // On some way to the head the function has been suspended since it last read the value.
    bool isSuspendedAtHead { false };
};

// What Sweep::finishLiveness() and its steps work in. A thread keeps it from one unit to the next.
struct SweepLivenessStorage {
    struct ReadOfValue {
        ValueID value;
        // The last operation of the block that reads the value. Null: the end of the block does.
        ValueID reader;
    };
    struct ReadInBlock {
        BlockID block;
        ReadOfValue read;
    };
    // A needed operation with what is live behind it on the ways that lead forward. What lives through the loops around its block comes on top.
    struct PointAskedAbout {
        // A suspension. Of a hint: its place in Sweep::m_operationsKeptForOperands.
        uint32_t index;
        BlockID block;
        SweepValueSets::Set liveBehind;
    };

    SweepValueSets sets;
    SweepFlow flow;
    // By value. Operation::noBlock for a constant and for what is not needed. Of a hint: the block of what it names.
    Vector<uint32_t> blockOfValue;
    // By value.
    SweepBits operationsThatSuspend;
    SweepBits hintsThatStay;
    SweepBits valuesReadInOtherBlocks;
    SweepBits blocksAskedAbout;
    SweepBits blocksAwaited;
    Vector<ReadInBlock> reads;
    // The reads of block b are readsByBlock[firstReadOfBlock[b] .. firstReadOfBlock[b + 1]).
    Vector<ReadOfValue> readsByBlock;
    Vector<uint32_t> firstReadOfBlock;
    // By block, in Sweep::m_operationsKeptForOperands.
    Vector<uint32_t> firstKeptForOperandInBlock;
    // By block.
    Vector<SweepValueSets::Set> liveAtHeads;
    // By loop.
    Vector<SweepValueSets::Set> liveThroughLoops;
    Vector<PointAskedAbout> hints;
    Vector<PointAskedAbout> suspensions;
    // By value. Where nothing suspends: what a hint names or may name, and the hints.
    SweepBits valuesToFollow;
    Vector<PlaceWhereSweptValueIsLetGo> placesLetGo;
    Vector<ValueID> hintsBehindLastUses;
    Vector<ValueID> joinsLetGo;
    // By block.
    Vector<WayOfSweptValueLetGo> waysOfValueLetGo;
    Vector<ValueID> valuesLiveAtHeads;
    Vector<ValueID> valuesLiveAtEnds;
    Vector<BlockID> blocksToLookAbove;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
