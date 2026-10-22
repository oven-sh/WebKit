/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirSplitLiveRangesAroundFrame.h"

#if ENABLE(B3_JIT)

#include "AirBlockInsertionSet.h"
#include "AirCode.h"
#include "AirEmitShuffle.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirLiveness.h"
#include "AirPhaseScope.h"
#include "AirShrinkWrapFrame.h"
#include "AirTmpMap.h"
#include "AirTmpSet.h"
#include "B3Procedure.h"
#include <wtf/BitVector.h>
#include <wtf/IndexMap.h>

namespace JSC { namespace B3 { namespace Air {

namespace {

using CountByBank = std::array<unsigned, numBanks>;

// The registers that a function may write without a frame to save them in.
CountByBank countRegistersUsableWithoutFrame(Code& code)
{
    RegisterSet calleeSaves = RegisterSet::calleeSaveRegisters();
    calleeSaves.exclude(code.unsavedCalleeSaves());
    calleeSaves.merge(code.policy().registers.savedByCallee);
    CountByBank result { };
    forEachBank([&](Bank bank) {
        for (Reg reg : code.regsInPriorityOrder(bank))
            result[bank] += !calleeSaves.contains(reg, IgnoreVectors);
    });
    return result;
}

bool hasMoreTmpsThan(Code& code, const CountByBank& numberOfRegisters)
{
    return code.numTmps(GP) > numberOfRegisters[GP] || code.numTmps(FP) > numberOfRegisters[FP];
}

class LiveRangeSplitter {
public:
    explicit LiveRangeSplitter(Code& code)
        : m_code(code)
        , m_liveness(code)
        , m_tmpInRegion(code)
        , m_blockInsertionSet(code)
        , m_insertionSet(code)
    {
    }

    bool run(BitVector&& regionForCalls, const CountByBank& numberOfRegisters)
    {
        m_isInRegion = WTF::move(regionForCalls);
        BitVector crowdedBlocks = findCrowdedBlocks(numberOfRegisters);
        if (!crowdedBlocks.isEmpty()) {
            std::optional<BitVector> region = growFrameRegionBeforeAllocation(m_code, m_isInRegion, crowdedBlocks);
            if (!region)
                return false;
            m_isInRegion = WTF::move(*region);
        }
        if (m_isInRegion.isEmpty() || !isEnteredOnLikelyPath() || !canPlaceShufflesOnBorder())
            return false;
        findTmpsLiveAcrossBorder();
        if (!m_hasSplitTmp)
            return false;

        for (size_t blockIndex : m_isInRegion)
            renameIn(m_code[blockIndex]);
        for (BasicBlock* block : m_code)
            insertShufflesOnOutgoingEdges(block);
        if (m_blockInsertionSet.execute())
            m_code.resetReachability();
        m_code.proc().setUsesShuffle(true);
        return true;
    }

private:
    bool isInRegion(BasicBlock* block) const { return m_isInRegion.quickGet(block->index()); }

    template<typename Functor>
    void forEachBorderEdge(const Functor& functor)
    {
        for (BasicBlock* from : m_code) {
            for (FrequentedBlock& edge : from->successors()) {
                if (isInRegion(from) != isInRegion(edge.block()))
                    functor(from, edge);
            }
        }
    }

    // A block that holds more values than that takes a callee-save or a spill slot.
    BitVector findCrowdedBlocks(const CountByBank& numberOfRegisters)
    {
        BitVector result(m_code.size());
        if (!hasMoreTmpsThan(m_code, numberOfRegisters))
            return result;

        TmpSet live;
        Vector<Tmp, 32> added;
        for (BasicBlock* block : m_code) {
            CountByBank numberLive { };
            auto exceedsRegisters = [&] {
                return numberLive[GP] > numberOfRegisters[GP] || numberLive[FP] > numberOfRegisters[FP];
            };
            auto add = [&](Tmp tmp) {
                if (tmp.isReg() || !live.add(tmp))
                    return;
                ++numberLive[tmp.bank()];
                added.append(tmp);
            };
            for (Tmp tmp : m_liveness.liveAtTail(block))
                add(tmp);
            for (unsigned index = block->size(); index-- && !exceedsRegisters();) {
                Inst& inst = block->at(index);
                inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                    if (Arg::isAnyDef(role) && !tmp.isReg() && live.remove(tmp))
                        --numberLive[tmp.bank()];
                });
                inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                    if (Arg::isAnyUse(role))
                        add(tmp);
                });
            }
            if (exceedsRegisters())
                result.quickSet(block->index());
            for (Tmp tmp : added)
                live.remove(tmp);
            added.shrink(0);
        }
        return result;
    }

    // What lives across a call on a rare arm is split around the call by the allocator, and what a rare arm computes is spilled first.
    bool isEnteredOnLikelyPath()
    {
        bool result = false;
        forEachBorderEdge([&](BasicBlock* from, FrequentedBlock& edge) {
            result |= !isInRegion(from) && !edge.isRare();
        });
        return result;
    }

    // A landing pad is entered from the unwinder, with registers clobbered: nothing can run on the way to it.
    bool canPlaceShufflesOnBorder()
    {
        bool result = true;
        forEachBorderEdge([&](BasicBlock* from, FrequentedBlock& edge) {
            result &= from->last().kind.opcode != Patch && !edge.block()->at(0).clobbersAtBlockEntry();
        });
        return result;
    }

    // Liveness names a Tmp by its position among all Tmps, which a new Tmp changes: everything is read from it before the first one is made.
    void findTmpsLiveAcrossBorder()
    {
        m_tmpsLiveAtHead.resize(m_code.size());
        BitVector isTarget(m_code.size());
        forEachBorderEdge([&](BasicBlock*, FrequentedBlock& edge) {
            BasicBlock* target = edge.block();
            if (isTarget.quickSet(target->index()))
                return;
            for (Tmp tmp : m_liveness.liveAtHead(target)) {
                if (!tmp.isReg())
                    m_tmpsLiveAtHead[target].append(tmp);
            }
        });
        for (size_t blockIndex : isTarget) {
            for (Tmp tmp : m_tmpsLiveAtHead[m_code[blockIndex]]) {
                if (m_tmpInRegion[tmp])
                    continue;
                m_tmpInRegion[tmp] = m_code.newTmp(tmp.bank());
                m_hasSplitTmp = true;
            }
        }
    }

    // The allocator coalesces the two Tmps of a Move before it looks for registers, which would undo the split. It leaves a Shuffle alone.
    std::optional<Inst> createShuffleFor(BasicBlock* from, BasicBlock* to)
    {
        bool entersRegion = isInRegion(to);
        Vector<ShufflePair, 8> pairs;
        for (Tmp tmp : m_tmpsLiveAtHead[to]) {
            Tmp inRegion = m_tmpInRegion[tmp];
            RELEASE_ASSERT(inRegion);
            Width width = tmp.isGP() ? pointerWidth() : m_code.usesSIMD() ? Width128 : Width64;
            pairs.append(entersRegion ? ShufflePair(tmp, inRegion, width) : ShufflePair(inRegion, tmp, width));
        }
        if (pairs.isEmpty())
            return std::nullopt;
        return createShuffle(from->last().origin, pairs.span());
    }

    bool isOnlyEnteredAcrossBorder(BasicBlock* block)
    {
        if (block == m_code[0] || block->at(0).clobbersAtBlockEntry())
            return false;
        for (BasicBlock* predecessor : block->predecessors()) {
            if (isInRegion(predecessor) == isInRegion(block))
                return false;
        }
        return true;
    }

    // The Shuffles belong to the region: the Tmps of the region are live nowhere else.
    void insertShufflesOnOutgoingEdges(BasicBlock* from)
    {
        Vector<std::pair<BasicBlock*, BasicBlock*>, 2> splitEdges;
        for (FrequentedBlock& edge : from->successors()) {
            BasicBlock* to = edge.block();
            if (isInRegion(from) == isInRegion(to))
                continue;
            size_t split = splitEdges.findIf([&](auto& pair) {
                return pair.first == to;
            });
            if (split != notFound) {
                edge.block() = splitEdges[split].second;
                continue;
            }

            bool placesAtHead = isInRegion(to) && isOnlyEnteredAcrossBorder(to);
            if (placesAtHead && m_hasShuffleAtHead.set(to->index()))
                continue;
            std::optional<Inst> shuffle = createShuffleFor(from, to);
            if (!shuffle)
                continue;

            if (placesAtHead) {
                m_insertionSet.insertInst(0, WTF::move(*shuffle));
                m_insertionSet.execute(to);
                continue;
            }
            if (isInRegion(from) && from->last().kind.opcode == Jump) {
                m_insertionSet.insertInst(from->size() - 1, WTF::move(*shuffle));
                m_insertionSet.execute(from);
                continue;
            }

            double frequency = std::min(from->frequency(), to->frequency());
            BasicBlock* block = to == m_code[0] ? m_blockInsertionSet.insertAfter(from, frequency) : m_blockInsertionSet.insertBefore(to, frequency);
            Value* origin = shuffle->origin;
            block->appendInst(WTF::move(*shuffle));
            block->append(Jump, origin);
            block->setSuccessors(FrequentedBlock(to));
            edge.block() = block;
            splitEdges.append({ to, block });
        }
    }

    void renameIn(BasicBlock* block)
    {
        for (Inst& inst : *block) {
            inst.forEachTmpFast([&](Tmp& tmp) {
                if (tmp.isReg())
                    return;
                if (Tmp inRegion = m_tmpInRegion[tmp])
                    tmp = inRegion;
            });
        }
    }

    Code& m_code;
    BitVector m_isInRegion;
    UnifiedTmpLiveness m_liveness;
    IndexMap<BasicBlock*, Vector<Tmp>> m_tmpsLiveAtHead;
    TmpMap<Tmp> m_tmpInRegion;
    BlockInsertionSet m_blockInsertionSet;
    InsertionSet m_insertionSet;
    BitVector m_hasShuffleAtHead;
    bool m_hasSplitTmp { false };
};

} // anonymous namespace

bool splitLiveRangesAroundFrame(Code& code)
{
    PhaseScope phaseScope(code, "splitLiveRangesAroundFrame"_s);
    constexpr unsigned minBlocksForPathWithoutFrame = 3;
    if (code.size() < minBlocksForPathWithoutFrame)
        return false;
    std::optional<BitVector> regionForCalls = findFrameRegionBeforeAllocation(code);
    if (!regionForCalls)
        return false;
    CountByBank numberOfRegisters = countRegistersUsableWithoutFrame(code);
    if (regionForCalls->isEmpty() && !hasMoreTmpsThan(code, numberOfRegisters))
        return false;
    return LiveRangeSplitter(code).run(WTF::move(*regionForCalls), numberOfRegisters);
}

} } } // namespace JSC::B3::Air

#endif // ENABLE(B3_JIT)
