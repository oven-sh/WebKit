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

inline constexpr uint8_t sweptBlockIsReached = 1 << 0;
inline constexpr uint8_t sweptBlockHasGivenUpHandler = 1 << 1;
inline constexpr uint8_t sweptBlockNeedsNoHandler = 1 << 2;
inline constexpr uint8_t sweptBlockHandsOverArguments = 1 << 3;
inline constexpr uint8_t sweptHandlerWasLookedAt = 1 << 4;
inline constexpr uint8_t sweptHandlerOnlyThrowsOutOfUnit = 1 << 5;
inline constexpr uint8_t sweptBlockDoesNothing = 1 << 6;
inline constexpr uint8_t sweptBranchDecidesNothing = 1 << 7;

inline EdgeID handlerOfSweptBlock(const Block& block, uint8_t state)
{
    return state & sweptBlockHasGivenUpHandler ? EdgeID() : block.handler;
}

inline bool isSweptEdgeTaken(const Code& code, const uint8_t* statesOfBlocks, EdgeID id, const Edge& edge)
{
    uint8_t state = statesOfBlocks[edge.from.index()];
    if (!(state & sweptBlockIsReached) || edge.isOpen())
        return false;
    return !(state & sweptBlockHasGivenUpHandler) || code.at(edge.from).handler != id;
}

class SweepFlow {
    WTF_MAKE_NONCOPYABLE(SweepFlow);
public:
    static constexpr uint32_t none = UINT32_MAX;

    SweepFlow() = default;

    void compute(const Code&, std::span<const uint8_t> statesOfBlocks, const SweepBits& blocksAskedAbout);
    void findLoops(const Code&, std::span<const uint32_t> firstIncomingEdge, std::span<const EdgeID> incomingEdges);

    std::span<const BlockID> blocksBehindTheirSuccessors() const LIFETIME_BOUND { return m_order.span(); }
    bool mayLeadToBlockAskedAbout(BlockID block) const { return m_blocksThatMayLeadToBlocksAskedAbout.quickGet(block.index()); }
    uint32_t numberOfBlocks() const { return static_cast<uint32_t>(m_blockAtNumber.size()); }
    bool hasLoops() const { return m_hasLoops; }
    bool isReducible() const { return m_isReducible; }

    bool isHeadOfLoop(uint32_t number) const { return m_hasLoops && m_headsOfLoops.quickGet(number); }
    BlockID headOf(uint32_t loop) const { return m_blockAtNumber[loop]; }
    uint32_t loopAround(BlockID block) const
    {
        if (!m_hasLoops)
            return none;
        uint32_t number = m_numberOfBlock[block.index()];
        return m_headsOfLoops.quickGet(number) ? number : m_loopAroundNumber[number];
    }
    uint32_t loopAroundLoop(uint32_t loop) const { return m_loopAroundNumber[loop]; }
    BlockID headOfOutermostLoopEntered(BlockID from, BlockID to) const
    {
        uint32_t entered = none;
        for (uint32_t loop = loopAround(to); loop != none && !holds(loop, from); loop = m_loopAroundNumber[loop])
            entered = loop;
        return entered == none ? BlockID() : m_blockAtNumber[entered];
    }

private:
    struct BlockOnPath {
        const Block* block;
        uint32_t index;
        uint32_t nextEdge;
    };
    struct WayInPastHead {
        uint32_t from;
        uint32_t next;
    };

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
    uint32_t outermostFoundAround(uint32_t number)
    {
        while (m_foundAround[number] != number) {
            m_foundAround[number] = m_foundAround[m_foundAround[number]];
            number = m_foundAround[number];
        }
        return number;
    }


    Vector<BlockID> m_order;
    Vector<BlockOnPath> m_path;
    Vector<uint32_t> m_numberOfBlock;
    Vector<BlockID> m_blockAtNumber;
    Vector<uint32_t> m_lastNumberBelow;
    Vector<uint32_t> m_loopAroundNumber;
    SweepBits m_headsOfLoops;
    Vector<uint32_t> m_foundAround;
    Vector<uint32_t> m_lastLoopListedFor;
    Vector<uint32_t> m_firstWayInPastHead;
    Vector<WayInPastHead> m_waysInPastHeads;
    Vector<uint32_t> m_members;
    SweepBits m_blocksThatMayLeadToBlocksAskedAbout;
    bool m_hasLoops { false };
    bool m_isReducible { true };
};

inline void SweepFlow::compute(const Code& code, std::span<const uint8_t> statesOfBlocks, const SweepBits& blocksAskedAbout)
{
    uint32_t numberOfBlocksOfCode = code.numberOfBlocks();
    m_numberOfBlock.fill(none, numberOfBlocksOfCode);
    m_blockAtNumber.fill(BlockID(), numberOfBlocksOfCode);
    m_lastNumberBelow.fill(none, numberOfBlocksOfCode);
    m_order.fill(BlockID(), numberOfBlocksOfCode);
    if (m_path.size() < numberOfBlocksOfCode)
        m_path.grow(numberOfBlocksOfCode);
    m_blocksThatMayLeadToBlocksAskedAbout.clearAll();
    m_blocksThatMayLeadToBlocksAskedAbout.ensureSize(numberOfBlocksOfCode);
    m_hasLoops = false;
    m_isReducible = true;

    const uint8_t* const states = statesOfBlocks.data();
    uint32_t* const numberOfBlock = m_numberOfBlock.mutableSpan().data();
    BlockID* const blockAtNumber = m_blockAtNumber.mutableSpan().data();
    uint32_t* const lastNumberBelow = m_lastNumberBelow.mutableSpan().data();
    BlockID* const order = m_order.mutableSpan().data();
    BlockOnPath* const path = m_path.mutableSpan().data();
    SweepBits& mayLeadToBlocksAskedAbout = m_blocksThatMayLeadToBlocksAskedAbout;
    uint32_t numberOfBlocksBegun = 0;
    uint32_t numberOfBlocksEnded = 0;

    auto begin = [&](BlockID block) ALWAYS_INLINE_LAMBDA {
        numberOfBlock[block.index()] = numberOfBlocksBegun;
        blockAtNumber[numberOfBlocksBegun++] = block;
        if (blocksAskedAbout.quickGet(block.index()))
            mayLeadToBlocksAskedAbout.quickSet(block.index());
    };
    for (const EntryOfCode& entry : code.entries()) {
        if (numberOfBlock[entry.block.index()] != none)
            continue;
        begin(entry.block);
        const Block* from = &code.at(entry.block);
        uint32_t indexOfFrom = entry.block.index();
        uint32_t nextEdge = 0;
        uint32_t depth = 0;
        for (;;) {
            const Terminator& terminator = from->terminator;
            uint32_t numberOfEdges = terminator.numberOfEdges;
            BlockID below;
            while (nextEdge <= numberOfEdges) {
                EdgeID edge = nextEdge < numberOfEdges ? terminator.edge(nextEdge) : handlerOfSweptBlock(*from, states[indexOfFrom]);
                ++nextEdge;
                if (!edge)
                    break;
                BlockID to = code.at(edge).to;
                uint32_t number = numberOfBlock[to.index()];
                if (number == none) {
                    below = to;
                    break;
                }
                if (lastNumberBelow[number] == none) {
                    if (!m_hasLoops) {
                        m_hasLoops = true;
                        m_headsOfLoops.clearAll();
                        m_headsOfLoops.ensureSize(numberOfBlocksOfCode);
                    }
                    m_headsOfLoops.quickSet(number);
                    mayLeadToBlocksAskedAbout.quickSet(indexOfFrom);
                } else if (mayLeadToBlocksAskedAbout.quickGet(to.index()))
                    mayLeadToBlocksAskedAbout.quickSet(indexOfFrom);
            }
            if (below) {
                begin(below);
                path[depth++] = { from, indexOfFrom, nextEdge };
                from = &code.at(below);
                indexOfFrom = below.index();
                nextEdge = 0;
                continue;
            }
            lastNumberBelow[numberOfBlock[indexOfFrom]] = numberOfBlocksBegun - 1;
            order[numberOfBlocksEnded++] = BlockID(indexOfFrom);
            if (!depth)
                break;
            BlockOnPath above = path[--depth];
            if (mayLeadToBlocksAskedAbout.quickGet(indexOfFrom))
                mayLeadToBlocksAskedAbout.quickSet(above.index);
            from = above.block;
            indexOfFrom = above.index;
            nextEdge = above.nextEdge;
        }
    }
    m_blockAtNumber.shrink(numberOfBlocksBegun);
    m_lastNumberBelow.shrink(numberOfBlocksBegun);
    m_order.shrink(numberOfBlocksEnded);
}

inline void SweepFlow::findLoops(const Code& code, std::span<const uint32_t> firstIncomingEdge, std::span<const EdgeID> incomingEdges)
{
    uint32_t count = numberOfBlocks();
    m_loopAroundNumber.fill(none, count);

    m_foundAround.resize(count);
    for (uint32_t number = 0; number < count; ++number)
        m_foundAround[number] = number;
    m_lastLoopListedFor.fill(0, count);
    m_firstWayInPastHead.fill(none, count);
    m_waysInPastHeads.shrink(0);

    auto forEachBlockThatLeadsTo = [&](uint32_t number, NOESCAPE const Invocable<void(uint32_t)> auto& functor) ALWAYS_INLINE_LAMBDA {
        uint32_t block = m_blockAtNumber[number].index();
        for (EdgeID edge : incomingEdges.subspan(firstIncomingEdge[block], firstIncomingEdge[block + 1] - firstIncomingEdge[block])) {
            uint32_t from = m_numberOfBlock[code.at(edge).from.index()];
            RELEASE_ASSERT(from != none);
            functor(from);
        }
    };

    for (uint32_t head = count; head--;) {
        if (!m_headsOfLoops.quickGet(head))
            continue;

        m_members.shrink(0);
        auto list = [&](uint32_t member) ALWAYS_INLINE_LAMBDA {
            if (member == head || m_lastLoopListedFor[member] == head + 1)
                return;
            m_lastLoopListedFor[member] = head + 1;
            m_members.append(member);
        };

        forEachBlockThatLeadsTo(head, [&](uint32_t from) ALWAYS_INLINE_LAMBDA {
            if (isAboveOrSame(head, from))
                list(outermostFoundAround(from));
        });

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
            forEachBlockThatLeadsTo(member, [&](uint32_t from) ALWAYS_INLINE_LAMBDA {
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

struct SweepLivenessStorage {
    struct ReadOfValue {
        ValueID value;
        ValueID reader;
    };
    struct ReadInBlock {
        BlockID block;
        ReadOfValue read;
    };
    struct PointAskedAbout {
        uint32_t index;
        BlockID block;
        SweepValueSets::Set liveBehind;
    };

    SweepValueSets sets;
    SweepFlow flow;
    Vector<uint32_t> blockOfValue;
    SweepBits operationsThatSuspend;
    SweepBits hintsThatStay;
    SweepBits valuesReadInOtherBlocks;
    SweepBits blocksAskedAbout;
    SweepBits blocksAwaited;
    Vector<ReadInBlock> reads;
    Vector<ReadOfValue> readsByBlock;
    Vector<uint32_t> firstReadOfBlock;
    Vector<uint32_t> firstKeptForOperandInBlock;
    Vector<SweepValueSets::Set> liveAtHeads;
    Vector<SweepValueSets::Set> liveThroughLoops;
    Vector<PointAskedAbout> hints;
    Vector<PointAskedAbout> suspensions;
    SweepBits valuesToFollow;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
