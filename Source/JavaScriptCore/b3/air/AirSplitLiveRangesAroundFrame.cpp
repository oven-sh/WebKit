/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AirSplitLiveRangesAroundFrame.h"

#if ENABLE(B3_JIT)

#include "AirArgInlines.h"
#include "AirBlockInsertionSet.h"
#include "AirCode.h"
#include "AirEmitShuffle.h"
#include "AirInsertionSet.h"
#include "AirInstInlines.h"
#include "AirPhaseScope.h"
#include "AirShrinkWrapFrame.h"
#include "AirTmpBoundaryRows.h"
#include "AirTmpInlines.h"
#include "AirTmpMap.h"
#include "B3Procedure.h"
#include <wtf/BitVector.h>
#include <wtf/FrequencyEstimator.h>
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
        , m_usesAndDefs(code, [](Tmp tmp) { return !tmp.isReg(); })
        , m_liveness(code, m_usesAndDefs.facts())
        , m_firstIndexOfFP(Tmp::absoluteIndexEnd(code, GP))
        , m_tmpInRegion(code)
        , m_blockInsertionSet(code)
        , m_insertionSet(code)
    {
        m_liveness.compute();
        RELEASE_ASSERT(m_liveness.status() == BitMatrixLivenessStatus::Solved);
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
        if (m_isInRegion.isEmpty() || !canPlaceShufflesOnBorder())
            return false;
        // What lives across a call on a rare arm is split around the call by the allocator, and what a rare arm computes is spilled first.
        // Where a rare arm is crowded by itself, something that lives through it gives way, and it is spilled wherever it lives: that costs most in a loop.
        if (!isEnteredOnLikelyPath() && (crowdedBlocks.isEmpty() || !holdsLikelyCycle(Side::OutsideRegion)))
            return false;
        findTmpsLiveAcrossBorder();
        if (holdsLikelyCycle(Side::InRegion) && !savesMoreThanItCosts(numberOfRegisters))
            return false;
        makeTmpsOfRegion();
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

    Bank bankOf(unsigned index) const { return index < m_firstIndexOfFP ? GP : FP; }

    void setIsLive(unsigned index, bool isLive)
    {
        if (m_isLive.quickGet(index) == isLive)
            return;
        m_isLive.quickSet(index, isLive);
        if (isLive)
            ++m_numberLive[bankOf(index)];
        else
            --m_numberLive[bankOf(index)];
    }

    // Goes from the last block to the first. m_isLive and m_numberLive describe the block's tail in atTail(block) and its head in atHead(block).
    void forEachBlockBackwards(NOESCAPE const Invocable<void(BasicBlock*)> auto& atTail, NOESCAPE const Invocable<void(BasicBlock*)> auto& atHead)
    {
        m_isLive.clearAll();
        m_isLive.ensureSize(m_usesAndDefs.crossesBoundary.size());
        m_numberLive = { };
        Air::forEachBlockBackwards(m_code, m_liveness, m_usesAndDefs, [&](unsigned index, bool isLive) { setIsLive(index, isLive); }, atTail, atHead);
    }

    // A block that holds more values than that takes a callee-save or a spill slot.
    BitVector findCrowdedBlocks(const CountByBank& numberOfRegisters)
    {
        BitVector result(m_code.size());
        if (!hasMoreTmpsThan(m_code, numberOfRegisters))
            return result;

        Vector<unsigned, 32> changed;
        forEachBlockBackwards([&](BasicBlock* block) {
            auto exceedsRegisters = [&] {
                return m_numberLive[GP] > numberOfRegisters[GP] || m_numberLive[FP] > numberOfRegisters[FP];
            };
            auto change = [&](Tmp tmp, bool isLive) {
                if (tmp.isReg())
                    return;
                unsigned index = tmp.linearlyIndexed(m_code).index();
                if (m_isLive.quickGet(index) == isLive)
                    return;
                setIsLive(index, isLive);
                changed.append(index);
            };
            for (unsigned index = block->size(); index-- && !exceedsRegisters();) {
                Inst& inst = block->at(index);
                inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                    if (Arg::isAnyDef(role))
                        change(tmp, false);
                });
                inst.forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                    if (Arg::isAnyUse(role))
                        change(tmp, true);
                });
            }
            if (exceedsRegisters())
                result.quickSet(block->index());
            for (unsigned index : changed)
                setIsLive(index, !m_isLive.quickGet(index));
            changed.shrink(0);
        }, [](BasicBlock*) { });
        return result;
    }

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
            isTarget.quickSet(edge.block()->index());
        });
        forEachBlockBackwards([](BasicBlock*) { }, [&](BasicBlock* block) {
            if (!isTarget.quickGet(block->index()))
                return;
            for (size_t index : m_isLive)
                m_tmpsLiveAtHead[block].append(Tmp::tmpForLinearIndex(m_code, index));
        });
    }

    enum class Side : bool { InRegion, OutsideRegion };

    // Every turn of such a loop in the region needs the frame. What needs it only on an unlikely way out of a loop is entered far less often than its frequency says.
    bool holdsLikelyCycle(Side side)
    {
        enum class State : uint8_t { Unseen, Open, Done };
        IndexMap<BasicBlock*, State> states(m_code.size(), State::Unseen);
        Vector<std::pair<BasicBlock*, unsigned>, 16> path;
        auto isOnSide = [&](BasicBlock* block) {
            return isInRegion(block) == (side == Side::InRegion);
        };
        for (BasicBlock* start : m_code) {
            if (!isOnSide(start) || states[start] != State::Unseen)
                continue;
            states[start] = State::Open;
            path.append({ start, 0 });
            while (!path.isEmpty()) {
                auto& [block, next] = path.last();
                if (next == block->numSuccessors()) {
                    states[block] = State::Done;
                    path.removeLast();
                    continue;
                }
                const FrequentedBlock& edge = block->successor(next++);
                if (shareOf(edge) < 1 || !isOnSide(edge.block()))
                    continue;
                if (states[edge.block()] == State::Open)
                    return true;
                if (states[edge.block()] == State::Unseen) {
                    states[edge.block()] = State::Open;
                    path.append({ edge.block(), 0 });
                }
            }
        }
        return false;
    }

    static double shareOf(const FrequentedBlock& edge)
    {
        constexpr WTF::FrequencyEstimatorConstants constants;
        if (edge.isRare())
            return constants.rareEdgeShare;
        return edge.frequency() == FrequencyClass::Unlikely ? constants.unlikelyEdgeShare : 1;
    }

    static double frequencyOf(BasicBlock* from, const FrequentedBlock& edge)
    {
        double shares = 0;
        for (const FrequentedBlock& successor : from->successors())
            shares += shareOf(successor);
        return std::min(from->frequency() * shareOf(edge) / shares, edge.block()->frequency());
    }

    // A call that stays outside the region saves the frame. Each value that is live on an edge across the border costs a move there.
    bool savesMoreThanItCosts(const CountByBank& numberOfRegistersWithoutFrame)
    {
        constexpr double instructionsToEnterAndLeaveFrame = 5;
        constexpr double instructionsToSaveAndRestoreRegister = isARM64() ? 1 : 2;
        double frequencyOfEntering = 0;
        double moves = 0;
        CountByBank liveIntoRegion { };
        forEachBorderEdge([&](BasicBlock* from, FrequentedBlock& edge) {
            double frequency = frequencyOf(from, edge);
            const Vector<Tmp>& live = m_tmpsLiveAtHead[edge.block()];
            moves += frequency * live.size();
            if (isInRegion(from))
                return;
            frequencyOfEntering += frequency;
            CountByBank count { };
            for (Tmp tmp : live)
                ++count[tmp.bank()];
            forEachBank([&](Bank bank) {
                liveIntoRegion[bank] = std::max(liveIntoRegion[bank], count[bank]);
            });
        });
        double instructionsOfFrame = instructionsToEnterAndLeaveFrame;
        forEachBank([&](Bank bank) {
            unsigned calleeSaves = m_code.regsInPriorityOrder(bank).size() - numberOfRegistersWithoutFrame[bank];
            instructionsOfFrame += instructionsToSaveAndRestoreRegister * std::min(liveIntoRegion[bank], calleeSaves);
        });
        return (m_code[0]->frequency() - frequencyOfEntering) * instructionsOfFrame > moves;
    }

    void makeTmpsOfRegion()
    {
        for (BasicBlock* block : m_code) {
            for (Tmp tmp : m_tmpsLiveAtHead[block]) {
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
    UsesAndDefsByBlock m_usesAndDefs;
    BitMatrixLiveness<TmpBoundaryRows> m_liveness;
    const unsigned m_firstIndexOfFP;
    BitVector m_isLive;
    CountByBank m_numberLive { };
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
