/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirBitMatrixLiveness.h"
#include "AirInstInlines.h"
#include "AirTmpInlines.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

struct BlockEnds {
    unsigned firstInst { 0 };
    unsigned exposedUses { 0 };
    unsigned defs { 0 };
    unsigned usesAtTail { 0 };
};

struct BlockFacts {
    std::span<const BlockEnds> blockEnds;
    std::span<const unsigned> exposedUses;
    std::span<const unsigned> defsInBlocks;
    std::span<const unsigned> usesAtTails;
    std::span<const uint8_t> crossesBoundary;
    std::span<const unsigned> indexToColumn;
    unsigned numberOfColumns { 0 };
};

class TmpBoundaryRows {
public:
    static constexpr bool recordsLayoutBoundaries = true;

    explicit TmpBoundaryRows(const BlockFacts& facts)
        : m_facts(facts)
    {
    }

    unsigned numIndices() const { return m_facts.numberOfColumns; }
    bool needsLiveAtTail(BasicBlock*) const { return false; }

    void willComputeInSlices(size_t wordsPerSlice)
    {
        m_wordsPerSlice = wordsPerSlice;
        const size_t wordsPerSet = (m_facts.numberOfColumns + 63) / 64;
        const unsigned numberOfBlocks = m_facts.blockEnds.size();
        m_endOfSlice.fill(0, (wordsPerSet + wordsPerSlice - 1) / wordsPerSlice);
        for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            forEachFactOf(blockIndex, [&](unsigned column, Row) {
                ++m_endOfSlice[column / 64 / wordsPerSlice];
            });
        }
        size_t numberOfFacts = 0;
        for (size_t& end : m_endOfSlice)
            numberOfFacts += std::exchange(end, numberOfFacts);
        m_factsBySlice.grow(numberOfFacts);
        for (unsigned blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
            forEachFactOf(blockIndex, [&](unsigned column, Row row) {
                m_factsBySlice[m_endOfSlice[column / 64 / wordsPerSlice]++] = Fact { blockIndex, column, row };
            });
        }
    }

    std::optional<unsigned> nextBlockInSlice(size_t firstWord, size_t) const
    {
        if (m_nextFact == m_endOfSlice[firstWord / m_wordsPerSlice])
            return std::nullopt;
        return m_factsBySlice[m_nextFact].blockIndex;
    }

    bool fillRows(BasicBlock* block, size_t firstWord, size_t numberOfWords, uint64_t* gen, uint64_t* kill, uint64_t* tail)
    {
        bool hasFact = false;
        auto setBit = [&](unsigned bit, Row row) ALWAYS_INLINE_LAMBDA {
            ASSERT_UNUSED(numberOfWords, bit / 64 < numberOfWords);
            uint64_t* words = nullptr;
            switch (row) {
            case Row::Gen:
                words = gen;
                break;
            case Row::Kill:
                words = kill;
                break;
            case Row::Tail:
                words = tail;
                break;
            }
            words[bit / 64] |= static_cast<uint64_t>(1) << (bit % 64);
            hasFact = true;
        };
        if (!m_wordsPerSlice) {
            forEachFactOf(block->index(), setBit);
            return hasFact;
        }
        const size_t slice = firstWord / m_wordsPerSlice;
        ASSERT(m_nextFact >= (slice ? m_endOfSlice[slice - 1] : 0) && m_nextFact <= m_endOfSlice[slice]);
        for (; m_nextFact < m_endOfSlice[slice] && m_factsBySlice[m_nextFact].blockIndex == block->index(); ++m_nextFact) {
            const Fact& fact = m_factsBySlice[m_nextFact];
            setBit(fact.column - firstWord * 64, fact.row);
        }
        return hasFact;
    }

private:
    enum class Row : uint8_t { Gen, Kill, Tail };

    struct Fact {
        unsigned blockIndex;
        unsigned column;
        Row row;
    };

    ALWAYS_INLINE void forEachFactOf(unsigned blockIndex, NOESCAPE const Invocable<void(unsigned column, Row)> auto& func) const
    {
        const BlockEnds begin = blockIndex ? m_facts.blockEnds[blockIndex - 1] : BlockEnds { };
        const BlockEnds end = m_facts.blockEnds[blockIndex];
        for (unsigned i = begin.exposedUses; i < end.exposedUses; ++i)
            func(m_facts.indexToColumn[m_facts.exposedUses[i]], Row::Gen);
        for (unsigned i = begin.defs; i < end.defs; ++i) {
            unsigned index = m_facts.defsInBlocks[i];
            if (m_facts.crossesBoundary[index])
                func(m_facts.indexToColumn[index], Row::Kill);
        }
        for (unsigned i = begin.usesAtTail; i < end.usesAtTail; ++i)
            func(m_facts.indexToColumn[m_facts.usesAtTails[i]], Row::Tail);
    }

    BlockFacts m_facts;
    Vector<Fact> m_factsBySlice;
    Vector<size_t> m_endOfSlice;
    size_t m_wordsPerSlice { 0 };
    size_t m_nextFact { 0 };
};

// What each block does to the Tmps that count, each named by its linear index.
struct UsesAndDefsByBlock {
    UsesAndDefsByBlock(Code& code, NOESCAPE const Invocable<bool(Tmp)> auto& counts)
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
                    if (counts(tmp))
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

// Goes from the last block to the first. What setIsLive(index, bool) has been told describes the block's tail in atTail(block) and its head in atHead(block).
void forEachBlockBackwards(Code& code, const BitMatrixLiveness<TmpBoundaryRows>& liveness, const UsesAndDefsByBlock& usesAndDefs, NOESCAPE const Invocable<void(unsigned, bool)> auto& setIsLive, NOESCAPE const Invocable<void(BasicBlock*)> auto& atTail, NOESCAPE const Invocable<void(BasicBlock*)> auto& atHead)
{
    for (unsigned blockIndex = code.size(); blockIndex--;) {
        BasicBlock* block = code[blockIndex];
        if (!block)
            continue;
        liveness.forEachLiveOnlyAtHeadOfNextBlock(block, [&](unsigned column) {
            setIsLive(usesAndDefs.columnToIndex[column], false);
        });
        liveness.forEachLiveOnlyAtTail(block, [&](unsigned column) {
            setIsLive(usesAndDefs.columnToIndex[column], true);
        });
        atTail(block);
        const BlockEnds begin = blockIndex ? usesAndDefs.blockEnds[blockIndex - 1] : BlockEnds { };
        const BlockEnds end = usesAndDefs.blockEnds[blockIndex];
        for (unsigned i = begin.defs; i < end.defs; ++i)
            setIsLive(usesAndDefs.defs[i], false);
        for (unsigned i = begin.exposedUses; i < end.exposedUses; ++i)
            setIsLive(usesAndDefs.exposedUses[i], true);
        atHead(block);
    }
}

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
