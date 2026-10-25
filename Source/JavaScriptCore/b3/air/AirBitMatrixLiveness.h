/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "AirCFG.h"
#include "AirCode.h"
#include "AirLimits.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/GraphOrdering.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace B3 { namespace Air {

class LowestFirstWorklist {
public:
    explicit LowestFirstWorklist(size_t size)
        : m_words(FillWith { }, (size + 63) / 64, static_cast<uint64_t>(0))
    {
    }

    void add(unsigned position)
    {
        m_words[position / 64] |= static_cast<uint64_t>(1) << (position % 64);
        m_lowerBound = std::min<size_t>(m_lowerBound, position);
    }

    std::optional<unsigned> takeLowest()
    {
        for (size_t wordIndex = m_lowerBound / 64; wordIndex < m_words.size(); ++wordIndex) {
            uint64_t word = m_words[wordIndex];
            if (!word)
                continue;
            m_words[wordIndex] = word & (word - 1);
            m_lowerBound = wordIndex * 64 + std::countr_zero(word);
            return m_lowerBound;
        }
        m_lowerBound = m_words.size() * 64;
        return std::nullopt;
    }

private:
    Vector<uint64_t> m_words;
    size_t m_lowerBound { 0 };
};

enum class BitMatrixLivenessStatus : uint8_t { NotComputed, Solved, GaveUp };

template<typename Adapter>
class BitMatrixLiveness : public Adapter {
    WTF_FORBID_HEAP_ALLOCATION;
public:
    class Workset {
    public:
        explicit Workset(size_t numberOfWords)
            : m_words(FillWith { }, numberOfWords, static_cast<uint64_t>(0))
        {
        }

        void add(unsigned index) { m_words[index / 64] |= static_cast<uint64_t>(1) << (index % 64); }
        void remove(unsigned index) { m_words[index / 64] &= ~(static_cast<uint64_t>(1) << (index % 64)); }
        bool contains(unsigned index) const { return m_words[index / 64] >> (index % 64) & 1; }
        std::span<const uint64_t> words() const { return m_words.span(); }
        std::span<uint64_t> mutableWords() { return m_words.mutableSpan(); }

        void forEachSetBit(NOESCAPE const Invocable<void(unsigned)> auto& func) const
        {
            for (size_t wordIndex = 0; wordIndex < m_words.size(); ++wordIndex) {
                for (uint64_t word = m_words[wordIndex]; word; word &= word - 1)
                    func(wordIndex * 64 + std::countr_zero(word));
            }
        }

    private:
        Vector<uint64_t, 8> m_words;
    };

    template<typename... Arguments>
    BitMatrixLiveness(Code& code, Arguments&&... arguments)
        : Adapter(std::forward<Arguments>(arguments)...)
        , m_code(code)
    {
    }

    void compute()
    {
        ASSERT(m_status == BitMatrixLivenessStatus::NotComputed);
        m_wordsPerSet = (this->numIndices() + 63) / 64;
        if (m_code.size() * m_wordsPerSet > AirLimits::maxWordsPerLivenessMatrix)
            computeInSlices();
        else
            computeDense();
    }

    BitMatrixLivenessStatus status() const { return m_status; }

    Workset makeWorkset() const { return Workset(m_wordsPerSet); }

    void copyLiveAtTailInto(Workset& workset, BasicBlock* block) const
    {
        ASSERT(m_status == BitMatrixLivenessStatus::Solved);
        uint64_t* words = workset.mutableWords().data();
        if (m_usesSlices) {
            for (size_t i = 0; i < m_wordsPerSet; ++i)
                words[i] = 0;
            ASSERT(m_rowOfBlock[block->index()]);
            unsigned row = *m_rowOfBlock[block->index()];
            for (unsigned i = m_firstSegmentOfRow[row]; i < m_firstSegmentOfRow[row + 1]; ++i) {
                const TailSegment& segment = m_tailSegments[i];
                const uint64_t* tail = m_liveAtTail + segment.offsetInStore;
                for (size_t word = 0; word < segment.numberOfWords; ++word)
                    words[segment.firstWord + word] = tail[word];
            }
            return;
        }
        const uint64_t* tail = m_liveAtTail + block->index() * m_wordsPerSet;
        for (size_t i = 0; i < m_wordsPerSet; ++i)
            words[i] = tail[i];
    }

    void forEachLiveOnlyAtTail(BasicBlock* block, NOESCAPE const Invocable<void(unsigned)> auto& func) const
    {
        static_assert(recordsLayoutBoundaries);
        ASSERT(m_status == BitMatrixLivenessStatus::Solved);
        if (m_usesSlices) {
            m_liveOnlyAtTail.forEachOf(block->index(), func);
            return;
        }
        BasicBlock* next = m_code.findNextBlock(block);
        forEachInDifference(m_liveAtTail + block->index() * m_wordsPerSet, next ? m_liveAtHead + next->index() * m_wordsPerSet : nullptr, 0, m_wordsPerSet, func);
    }

    void forEachLiveOnlyAtHeadOfNextBlock(BasicBlock* block, NOESCAPE const Invocable<void(unsigned)> auto& func) const
    {
        static_assert(recordsLayoutBoundaries);
        ASSERT(m_status == BitMatrixLivenessStatus::Solved);
        if (m_usesSlices) {
            m_liveOnlyAtHeadOfNextBlock.forEachOf(block->index(), func);
            return;
        }
        BasicBlock* next = m_code.findNextBlock(block);
        if (!next)
            return;
        forEachInDifference(m_liveAtHead + next->index() * m_wordsPerSet, m_liveAtTail + block->index() * m_wordsPerSet, 0, m_wordsPerSet, func);
    }

    void forEachLiveAtHeadOfFirstBlock(NOESCAPE const Invocable<void(unsigned)> auto& func) const
    {
        static_assert(recordsLayoutBoundaries);
        ASSERT(m_status == BitMatrixLivenessStatus::Solved);
        if (m_usesSlices) {
            for (unsigned index : m_liveAtHeadOfFirstBlock)
                func(index);
            return;
        }
        unsigned first = m_code.findFirstBlockIndex(0);
        if (first == m_code.size())
            return;
        forEachInDifference(m_liveAtHead + first * m_wordsPerSet, nullptr, 0, m_wordsPerSet, func);
    }

private:
    static constexpr bool recordsLayoutBoundaries = [] {
        if constexpr (requires { Adapter::recordsLayoutBoundaries; })
            return Adapter::recordsLayoutBoundaries;
        else
            return false;
    }();

    struct TailSegment {
        unsigned row;
        unsigned firstWord;
        unsigned numberOfWords;
        size_t offsetInStore;
    };

    struct Matrices {
        uint64_t* liveAtHead;
        uint64_t* liveAtTail;
        const uint64_t* gens;
        const uint64_t* kills;
        size_t wordsPerRow;
    };

    struct IndexAtBlock {
        unsigned blockIndex;
        unsigned index;
    };

    class IndicesByBlock {
    public:
        void append(unsigned blockIndex, unsigned index) { m_unsorted.append(IndexAtBlock { blockIndex, index }); }

        void sortByBlock(unsigned numberOfBlocks)
        {
            m_firstOfBlock.fill(0, numberOfBlocks + 1);
            for (const IndexAtBlock& entry : m_unsorted)
                ++m_firstOfBlock[entry.blockIndex];
            unsigned sum = 0;
            for (unsigned& first : m_firstOfBlock)
                sum += std::exchange(first, sum);
            m_indices.grow(m_unsorted.size());
            for (const IndexAtBlock& entry : m_unsorted)
                m_indices[m_firstOfBlock[entry.blockIndex]++] = entry.index;
            for (unsigned blockIndex = numberOfBlocks; blockIndex; --blockIndex)
                m_firstOfBlock[blockIndex] = m_firstOfBlock[blockIndex - 1];
            m_firstOfBlock[0] = 0;
            m_unsorted.clear();
        }

        void forEachOf(unsigned blockIndex, NOESCAPE const Invocable<void(unsigned)> auto& func) const
        {
            for (unsigned i = m_firstOfBlock[blockIndex]; i < m_firstOfBlock[blockIndex + 1]; ++i)
                func(m_indices[i]);
        }

    private:
        Vector<IndexAtBlock> m_unsorted;
        Vector<unsigned> m_firstOfBlock;
        Vector<unsigned> m_indices;
    };

    struct LayoutNeighbors {
        Vector<std::optional<unsigned>> previous;
        Vector<std::optional<unsigned>> next;
    };

    ALWAYS_INLINE static void forEachInDifference(const uint64_t* minuend, const uint64_t* subtrahend, size_t firstWord, size_t numberOfWords, NOESCAPE const Invocable<void(unsigned)> auto& func)
    {
        for (size_t wordIndex = 0; wordIndex < numberOfWords; ++wordIndex) {
            for (uint64_t word = minuend[wordIndex] & ~(subtrahend ? subtrahend[wordIndex] : 0); word; word &= word - 1)
                func((firstWord + wordIndex) * 64 + std::countr_zero(word));
        }
    }

    ALWAYS_INLINE void propagateLiveness(const Matrices& matrices, unsigned blockIndex, NOESCAPE const Invocable<void(unsigned)> auto& didChangeTail)
    {
        const size_t words = matrices.wordsPerRow;
        uint64_t* head = matrices.liveAtHead + blockIndex * words;
        const uint64_t* tail = matrices.liveAtTail + blockIndex * words;
        const uint64_t* gen = matrices.gens + blockIndex * words;
        const uint64_t* kill = matrices.kills + blockIndex * words;
        uint64_t changed = 0;
        for (size_t i = 0; i < words; ++i) {
            uint64_t newHead = gen[i] | (tail[i] & ~kill[i]);
            changed |= newHead ^ head[i];
            head[i] = newHead;
        }
        if (!changed)
            return;
        for (BasicBlock* predecessor : m_code[blockIndex]->predecessors()) {
            uint64_t* predecessorTail = matrices.liveAtTail + predecessor->index() * words;
            uint64_t predecessorChanged = 0;
            for (size_t i = 0; i < words; ++i) {
                predecessorChanged |= head[i] & ~predecessorTail[i];
                predecessorTail[i] |= head[i];
            }
            if (predecessorChanged)
                didChangeTail(predecessor->index());
        }
    }

    void computeDense()
    {
        const size_t wordsPerSet = m_wordsPerSet;
        const size_t wordsPerMatrix = m_code.size() * wordsPerSet;
        m_store.fill(0, 2 * wordsPerMatrix);
        Vector<uint64_t> transferStore(FillWith { }, 2 * wordsPerMatrix, static_cast<uint64_t>(0));
        uint64_t* liveAtHead = m_store.mutableSpan().data();
        uint64_t* liveAtTail = liveAtHead + wordsPerMatrix;
        uint64_t* gens = transferStore.mutableSpan().data();
        uint64_t* kills = gens + wordsPerMatrix;

        Vector<unsigned, 64> worklist;
        Vector<uint8_t, 64> isDirty(FillWith { }, m_code.size(), static_cast<uint8_t>(0));
        for (BasicBlock* block : m_code) {
            uint64_t* tail = liveAtTail + block->index() * wordsPerSet;
            uint64_t* gen = gens + block->index() * wordsPerSet;
            uint64_t* kill = kills + block->index() * wordsPerSet;
            if (!this->fillRows(block, 0, wordsPerSet, gen, kill, tail))
                continue;
            isDirty[block->index()] = 1;
            worklist.append(block->index());
        }

        const Matrices matrices { liveAtHead, liveAtTail, gens, kills, wordsPerSet };
        if (m_code.size() >= AirLimits::minBlocksToSolveLivenessInPostOrder) {
            const Vector<unsigned> blocksInPostOrder = computeBlocksInPostOrder();
            Vector<unsigned> positionOfBlock(FillWith { }, m_code.size(), 0u);
            for (unsigned position = 0; position < blocksInPostOrder.size(); ++position)
                positionOfBlock[blocksInPostOrder[position]] = position;
            LowestFirstWorklist orderedWorklist(blocksInPostOrder.size());
            for (unsigned blockIndex : worklist)
                orderedWorklist.add(positionOfBlock[blockIndex]);
            worklist.shrink(0);
            while (auto position = orderedWorklist.takeLowest()) {
                propagateLiveness(matrices, blocksInPostOrder[*position], [&](unsigned predecessorIndex) {
                    orderedWorklist.add(positionOfBlock[predecessorIndex]);
                });
            }
        }
        while (!worklist.isEmpty()) {
            unsigned blockIndex = worklist.takeLast();
            isDirty[blockIndex] = 0;
            propagateLiveness(matrices, blockIndex, [&](unsigned predecessorIndex) {
                if (isDirty[predecessorIndex])
                    return;
                isDirty[predecessorIndex] = 1;
                worklist.append(predecessorIndex);
            });
        }
        m_liveAtHead = liveAtHead;
        m_liveAtTail = liveAtTail;
        m_status = BitMatrixLivenessStatus::Solved;
    }

    Vector<unsigned> computeBlocksInPostOrder() const
    {
        Vector<unsigned> blocksInPostOrder;
        BitVector isInOrder(m_code.size());
        appendNodeIndicesInOrder(m_code.cfg(), GraphOrder::PostOrder, isInOrder, blocksInPostOrder);
        for (BasicBlock* block : m_code) {
            if (!isInOrder.quickSet(block->index()))
                blocksInPostOrder.append(block->index());
        }
        return blocksInPostOrder;
    }

    LayoutNeighbors computeLayoutNeighbors() const
    {
        LayoutNeighbors neighbors;
        neighbors.previous.fill(std::nullopt, m_code.size());
        neighbors.next.fill(std::nullopt, m_code.size());
        std::optional<unsigned> previous;
        for (BasicBlock* block : m_code) {
            neighbors.previous[block->index()] = previous;
            if (previous)
                neighbors.next[*previous] = block->index();
            previous = block->index();
        }
        return neighbors;
    }

    void sortSegmentsByRow(const Vector<TailSegment>& segments, unsigned numberOfRows)
    {
        m_firstSegmentOfRow.fill(0, numberOfRows + 1);
        for (const TailSegment& segment : segments)
            ++m_firstSegmentOfRow[segment.row];
        unsigned sum = 0;
        for (unsigned& first : m_firstSegmentOfRow)
            sum += std::exchange(first, sum);
        m_tailSegments.grow(segments.size());
        for (const TailSegment& segment : segments)
            m_tailSegments[m_firstSegmentOfRow[segment.row]++] = segment;
        for (unsigned row = numberOfRows; row; --row)
            m_firstSegmentOfRow[row] = m_firstSegmentOfRow[row - 1];
        m_firstSegmentOfRow[0] = 0;
    }

    void recordBoundaryAfter(const Matrices& matrices, const LayoutNeighbors& neighbors, size_t firstWord, unsigned blockIndex)
    {
        const size_t words = matrices.wordsPerRow;
        const uint64_t* tail = matrices.liveAtTail + blockIndex * words;
        std::optional<unsigned> next = neighbors.next[blockIndex];
        const uint64_t* headOfNext = next ? matrices.liveAtHead + *next * words : nullptr;
        forEachInDifference(tail, headOfNext, firstWord, words, [&](unsigned index) {
            m_liveOnlyAtTail.append(blockIndex, index);
        });
        if (!headOfNext)
            return;
        forEachInDifference(headOfNext, tail, firstWord, words, [&](unsigned index) {
            m_liveOnlyAtHeadOfNextBlock.append(blockIndex, index);
        });
    }

    void recordBoundariesOfSlice(const Matrices& matrices, const LayoutNeighbors& neighbors, size_t firstWord, const Vector<unsigned>& visitedBlocks, Vector<unsigned>& lastSliceRecordingBoundaryAfter, unsigned sliceNumber)
    {
        auto recordOnce = [&](unsigned blockIndex) {
            if (lastSliceRecordingBoundaryAfter[blockIndex] == sliceNumber)
                return;
            lastSliceRecordingBoundaryAfter[blockIndex] = sliceNumber;
            recordBoundaryAfter(matrices, neighbors, firstWord, blockIndex);
        };
        for (unsigned blockIndex : visitedBlocks) {
            recordOnce(blockIndex);
            if (std::optional<unsigned> previous = neighbors.previous[blockIndex]) {
                recordOnce(*previous);
                continue;
            }
            forEachInDifference(matrices.liveAtHead + blockIndex * matrices.wordsPerRow, nullptr, firstWord, matrices.wordsPerRow, [&](unsigned index) {
                m_liveAtHeadOfFirstBlock.append(index);
            });
        }
    }

    NEVER_INLINE void computeInSlices()
    {
        m_usesSlices = true;
        const unsigned numberOfBlocks = m_code.size();
        const size_t wordsPerSlice = std::max<size_t>(1, AirLimits::maxWordsPerLivenessMatrix / numberOfBlocks);
        if constexpr (requires { this->willComputeInSlices(wordsPerSlice); })
            this->willComputeInSlices(wordsPerSlice);

        m_rowOfBlock.fill(std::nullopt, numberOfBlocks);
        unsigned numberOfRows = 0;
        for (BasicBlock* block : m_code) {
            if (this->needsLiveAtTail(block))
                m_rowOfBlock[block->index()] = numberOfRows++;
        }
        Vector<TailSegment> segments;

        Vector<uint64_t> scratch(FillWith { }, 4 * numberOfBlocks * std::min(wordsPerSlice, m_wordsPerSet), static_cast<uint64_t>(0));
        Vector<unsigned> lastSliceVisiting(FillWith { }, numberOfBlocks, 0u);
        Vector<unsigned> lastSliceRecordingBoundaryAfter;
        LayoutNeighbors neighbors;
        if constexpr (recordsLayoutBoundaries) {
            lastSliceRecordingBoundaryAfter.fill(0, numberOfBlocks);
            neighbors = computeLayoutNeighbors();
        }
        Vector<unsigned> visitedBlocks;
        unsigned sliceNumber = 0;

        const Vector<unsigned> blocksInPostOrder = computeBlocksInPostOrder();
        Vector<unsigned> positionOfBlock(FillWith { }, numberOfBlocks, 0u);
        for (unsigned position = 0; position < blocksInPostOrder.size(); ++position)
            positionOfBlock[blocksInPostOrder[position]] = position;
        LowestFirstWorklist worklist(blocksInPostOrder.size());
        for (size_t firstWord = 0; firstWord < m_wordsPerSet; firstWord += wordsPerSlice) {
            const size_t words = std::min(wordsPerSlice, m_wordsPerSet - firstWord);
            const size_t wordsPerMatrix = numberOfBlocks * words;
            uint64_t* liveAtHead = scratch.mutableSpan().data();
            uint64_t* liveAtTail = liveAtHead + wordsPerMatrix;
            uint64_t* gens = liveAtTail + wordsPerMatrix;
            uint64_t* kills = gens + wordsPerMatrix;
            ++sliceNumber;
            visitedBlocks.shrink(0);

            auto markVisited = [&](unsigned blockIndex) {
                if (lastSliceVisiting[blockIndex] == sliceNumber)
                    return;
                lastSliceVisiting[blockIndex] = sliceNumber;
                visitedBlocks.append(blockIndex);
            };

            auto fillRowsOf = [&](BasicBlock* block) {
                uint64_t* tail = liveAtTail + block->index() * words;
                uint64_t* gen = gens + block->index() * words;
                uint64_t* kill = kills + block->index() * words;
                if (!this->fillRows(block, firstWord, words, gen, kill, tail))
                    return;
                markVisited(block->index());
                worklist.add(positionOfBlock[block->index()]);
            };
            // There can be as many slices as blocks, so an adapter that knows which blocks name an index of the slice says so.
            if constexpr (requires { this->nextBlockInSlice(firstWord, words); }) {
                while (std::optional<unsigned> blockIndex = this->nextBlockInSlice(firstWord, words))
                    fillRowsOf(m_code[*blockIndex]);
            } else {
                for (BasicBlock* block : m_code)
                    fillRowsOf(block);
            }

            const Matrices matrices { liveAtHead, liveAtTail, gens, kills, words };
            while (auto position = worklist.takeLowest()) {
                propagateLiveness(matrices, blocksInPostOrder[*position], [&](unsigned predecessorIndex) {
                    markVisited(predecessorIndex);
                    worklist.add(positionOfBlock[predecessorIndex]);
                });
            }

            if constexpr (recordsLayoutBoundaries)
                recordBoundariesOfSlice(matrices, neighbors, firstWord, visitedBlocks, lastSliceRecordingBoundaryAfter, sliceNumber);

            for (unsigned blockIndex : visitedBlocks) {
                size_t offset = blockIndex * words;
                if (std::optional<unsigned> row = m_rowOfBlock[blockIndex]) {
                    uint64_t any = 0;
                    for (size_t i = 0; i < words; ++i)
                        any |= liveAtTail[offset + i];
                    if (any) {
                        segments.append(TailSegment { *row, static_cast<unsigned>(firstWord), static_cast<unsigned>(words), m_store.size() });
                        m_store.append(std::span<const uint64_t> { liveAtTail + offset, words });
                    }
                }
                for (uint64_t* matrix : { liveAtHead, liveAtTail, gens, kills }) {
                    for (size_t i = 0; i < words; ++i)
                        matrix[offset + i] = 0;
                }
            }
            if (m_store.size() > AirLimits::maxWordsOfLivenessAtTails) {
                m_status = BitMatrixLivenessStatus::GaveUp;
                return;
            }
        }

        m_liveAtTail = m_store.span().data();
        sortSegmentsByRow(segments, numberOfRows);
        if constexpr (recordsLayoutBoundaries) {
            m_liveOnlyAtTail.sortByBlock(numberOfBlocks);
            m_liveOnlyAtHeadOfNextBlock.sortByBlock(numberOfBlocks);
        }
        m_status = BitMatrixLivenessStatus::Solved;
    }

    Code& m_code;
    size_t m_wordsPerSet { 0 };
    Vector<uint64_t> m_store;
    Vector<std::optional<unsigned>> m_rowOfBlock;
    Vector<unsigned> m_firstSegmentOfRow;
    Vector<TailSegment> m_tailSegments;
    IndicesByBlock m_liveOnlyAtTail;
    IndicesByBlock m_liveOnlyAtHeadOfNextBlock;
    Vector<unsigned> m_liveAtHeadOfFirstBlock;
    const uint64_t* m_liveAtHead { nullptr };
    const uint64_t* m_liveAtTail { nullptr };
    bool m_usesSlices { false };
    BitMatrixLivenessStatus m_status { BitMatrixLivenessStatus::NotComputed };
};

} } } // namespace JSC::B3::Air

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(B3_JIT)
