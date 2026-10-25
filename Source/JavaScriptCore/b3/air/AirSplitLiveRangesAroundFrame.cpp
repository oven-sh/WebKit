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

// What each block does to the Tmps that are no registers, each named by its linear index.
struct UsesAndDefsByBlock {
    explicit UsesAndDefsByBlock(Code& code)
    {
        const unsigned numberOfIndices = Tmp::linearIndexEnd(code);
        Vector<unsigned> stamps(FillWith { }, numberOfIndices, 0u);
        crossesBoundary.fill(0, numberOfIndices);
        blockEnds.grow(code.size());
        Vector<std::pair<unsigned, Arg::Role>, 8> mentions;
        for (unsigned blockIndex = 0; blockIndex < code.size(); ++blockIndex) {
            const unsigned useStamp = (blockIndex + 1) << 1;
            const unsigned defStamp = useStamp | 1;
            auto noteUse = [&](unsigned index) {
                if ((stamps[index] | 1) == defStamp)
                    return;
                stamps[index] = useStamp;
                crossesBoundary[index] = 1;
                exposedUses.append(index);
            };
            auto noteDef = [&](unsigned index) {
                if (stamps[index] == defStamp)
                    return;
                stamps[index] = defStamp;
                defs.append(index);
            };
            BasicBlock* block = code[blockIndex];
            for (unsigned instIndex = 0; block && instIndex < block->size(); ++instIndex) {
                mentions.shrink(0);
                block->at(instIndex).forEachTmp([&](Tmp& tmp, Arg::Role role, Bank, Width) {
                    if (!tmp.isReg())
                        mentions.append({ tmp.linearlyIndexed(code).index(), role });
                });
                auto forEachMention = [&](bool (*hasTiming)(Arg::Role), NOESCAPE const Invocable<void(unsigned)> auto& func) {
                    for (auto [index, role] : mentions) {
                        if (hasTiming(role))
                            func(index);
                    }
                };
                forEachMention(Arg::isEarlyDef, noteDef);
                forEachMention(Arg::isEarlyUse, noteUse);
                forEachMention(Arg::isLateDef, noteDef);
                forEachMention(Arg::isLateUse, [&](unsigned index) {
                    if (instIndex + 1 < block->size()) {
                        noteUse(index);
                        return;
                    }
                    crossesBoundary[index] = 1;
                    usesAtTails.append(index);
                });
            }
            blockEnds[blockIndex].exposedUses = exposedUses.size();
            blockEnds[blockIndex].defs = defs.size();
            blockEnds[blockIndex].usesAtTail = usesAtTails.size();
        }
        indexToColumn.grow(numberOfIndices);
        for (unsigned index = 0; index < numberOfIndices; ++index) {
            if (!crossesBoundary[index])
                continue;
            indexToColumn[index] = columnToIndex.size();
            columnToIndex.append(index);
        }
    }

    BlockFacts facts() const
    {
        return BlockFacts { blockEnds.span(), exposedUses.span(), defs.span(), usesAtTails.span(), crossesBoundary.span(), indexToColumn.span(), static_cast<unsigned>(columnToIndex.size()) };
    }

    Vector<BlockEnds> blockEnds;
    Vector<unsigned> exposedUses;
    Vector<unsigned> defs;
    Vector<unsigned> usesAtTails;
    Vector<uint8_t> crossesBoundary;
    Vector<unsigned> indexToColumn;
    Vector<unsigned> columnToIndex;
};

class LiveRangeSplitter {
public:
    explicit LiveRangeSplitter(Code& code)
        : m_code(code)
        , m_usesAndDefs(code)
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
        for (unsigned blockIndex = m_code.size(); blockIndex--;) {
            BasicBlock* block = m_code[blockIndex];
            if (!block)
                continue;
            m_liveness.forEachLiveOnlyAtHeadOfNextBlock(block, [&](unsigned column) {
                setIsLive(m_usesAndDefs.columnToIndex[column], false);
            });
            m_liveness.forEachLiveOnlyAtTail(block, [&](unsigned column) {
                setIsLive(m_usesAndDefs.columnToIndex[column], true);
            });
            atTail(block);
            const BlockEnds begin = blockIndex ? m_usesAndDefs.blockEnds[blockIndex - 1] : BlockEnds { };
            const BlockEnds end = m_usesAndDefs.blockEnds[blockIndex];
            for (unsigned i = begin.defs; i < end.defs; ++i)
                setIsLive(m_usesAndDefs.defs[i], false);
            for (unsigned i = begin.exposedUses; i < end.exposedUses; ++i)
                setIsLive(m_usesAndDefs.exposedUses[i], true);
            atHead(block);
        }
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
            isTarget.quickSet(edge.block()->index());
        });
        forEachBlockBackwards([](BasicBlock*) { }, [&](BasicBlock* block) {
            if (!isTarget.quickGet(block->index()))
                return;
            for (size_t index : m_isLive)
                m_tmpsLiveAtHead[block].append(Tmp::tmpForLinearIndex(m_code, index));
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
