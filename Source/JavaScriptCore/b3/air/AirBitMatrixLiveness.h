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

    // The sets of one slice. A set is named by the number of its row, so a block that names no index of the slice shares
    // the rows of its successors, and a visit to it touches no words. Row 0 is empty.
    class SliceRows {
    public:
        explicit SliceRows(unsigned numberOfPositions)
            : gen(FillWith { }, numberOfPositions, 0u)
            , m_head(FillWith { }, numberOfPositions, 0u)
            , m_tail(FillWith { }, numberOfPositions, 0u)
        {
        }

        // No later slice is wider than the first.
        void beginSlice(size_t wordsPerRow)
        {
            if (m_chunks.isEmpty()) {
                constexpr size_t wordsPerChunk = 1 << 17;
                m_wordsPerChunk = std::max(wordsPerChunk, wordsPerRow);
                m_rowsPerChunkShift = std::bit_width(m_wordsPerChunk / wordsPerRow) - 1;
            }
            RELEASE_ASSERT((wordsPerRow << m_rowsPerChunkShift) <= m_wordsPerChunk);
            m_wordsPerRow = wordsPerRow;
            m_numberOfRows = 0;
            m_freeRows.shrink(0);
            m_lastUnion = { };
            addRows(1);
        }

        unsigned headOf(unsigned position) const { return m_head[position]; }
        unsigned tailOf(unsigned position) const { return m_tail[position]; }
        void setHead(unsigned position, unsigned row) { pointTo(m_head[position], row); }
        void setTail(unsigned position, unsigned row) { pointTo(m_tail[position], row); }

        void forget(unsigned position)
        {
            m_head[position] = 0;
            m_tail[position] = 0;
            gen[position] = 0;
        }

        size_t wordsPerRow() const { return m_wordsPerRow; }
        uint64_t* at(unsigned row) LIFETIME_BOUND { return m_chunks[row >> m_rowsPerChunkShift].mutableSpan().data() + offsetInChunk(row); }
        const uint64_t* at(unsigned row) const LIFETIME_BOUND { return m_chunks[row >> m_rowsPerChunkShift].span().data() + offsetInChunk(row); }

        unsigned addRows(unsigned count)
        {
            unsigned first = addRowsWithoutClearing(count);
            for (unsigned row = first; row < first + count; ++row)
                zeroSpan(std::span { at(row), m_wordsPerRow });
            return first;
        }

        void removeRowsFrom(unsigned row) { m_numberOfRows = row; }

        bool isEmpty(unsigned row) const
        {
            uint64_t any = 0;
            for (size_t i = 0; i < m_wordsPerRow; ++i)
                any |= at(row)[i];
            return !any;
        }

        unsigned unite(unsigned a, unsigned b)
        {
            if (a == b || !b)
                return a;
            if (!a)
                return b;
            if (m_lastUnion.a == a && m_lastUnion.b == b)
                return m_lastUnion.result;

            unsigned result = takeRow();
            const uint64_t* wordsOfA = at(a);
            const uint64_t* wordsOfB = at(b);
            uint64_t* wordsOfResult = at(result);
            uint64_t onlyInA = 0;
            uint64_t onlyInB = 0;
            for (size_t i = 0; i < m_wordsPerRow; ++i) {
                wordsOfResult[i] = wordsOfA[i] | wordsOfB[i];
                onlyInA |= wordsOfA[i] & ~wordsOfB[i];
                onlyInB |= wordsOfB[i] & ~wordsOfA[i];
            }
            if (!onlyInA || !onlyInB) {
                m_freeRows.append(result);
                result = onlyInB ? b : a;
            }
            m_lastUnion = { a, b, result };
            return result;
        }

        // The row of gen | (tail & ~kill). It is `current` when that row already holds the answer.
        unsigned applyTransfer(unsigned position, unsigned current)
        {
            const unsigned tailRow = m_tail[position];
            unsigned result = takeRow();
            const uint64_t* wordsOfGen = at(gen[position]);
            const uint64_t* wordsOfKill = at(gen[position] + 1);
            const uint64_t* wordsOfTail = at(tailRow);
            const uint64_t* wordsOfCurrent = at(current);
            uint64_t* wordsOfResult = at(result);
            uint64_t differsFromCurrent = 0;
            uint64_t differsFromTail = 0;
            uint64_t any = 0;
            for (size_t i = 0; i < m_wordsPerRow; ++i) {
                uint64_t word = wordsOfGen[i] | (wordsOfTail[i] & ~wordsOfKill[i]);
                wordsOfResult[i] = word;
                differsFromCurrent |= word ^ wordsOfCurrent[i];
                differsFromTail |= word ^ wordsOfTail[i];
                any |= word;
            }
            if (differsFromCurrent && differsFromTail && any)
                return result;
            m_freeRows.append(result);
            if (!differsFromCurrent)
                return current;
            return any ? tailRow : 0;
        }

        // By position in post order. The kill row of a block follows its gen row, and a block whose gen is 0 names no index of the slice.
        Vector<unsigned> gen;

    private:
        size_t offsetInChunk(unsigned row) const { return (row & ((1u << m_rowsPerChunkShift) - 1)) * m_wordsPerRow; }

        // Rows stay where they are, and the chunks serve every slice.
        unsigned addRowsWithoutClearing(unsigned count)
        {
            unsigned first = m_numberOfRows;
            m_numberOfRows += count;
            while ((m_chunks.size() << m_rowsPerChunkShift) < m_numberOfRows)
                m_chunks.append(Vector<uint64_t>(m_wordsPerChunk));
            if (m_numberOfSlotsPointingTo.size() < m_numberOfRows)
                m_numberOfSlotsPointingTo.grow(m_numberOfRows);
            for (unsigned row = first; row < m_numberOfRows; ++row)
                m_numberOfSlotsPointingTo[row] = 0;
            return first;
        }

        unsigned takeRow() { return m_freeRows.isEmpty() ? addRowsWithoutClearing(1) : m_freeRows.takeLast(); }

        // A row that no head and no tail points to any more is used again.
        void pointTo(unsigned& slot, unsigned row)
        {
            if (row)
                ++m_numberOfSlotsPointingTo[row];
            unsigned before = std::exchange(slot, row);
            if (!before || --m_numberOfSlotsPointingTo[before])
                return;
            m_freeRows.append(before);
            if (before == m_lastUnion.a || before == m_lastUnion.b || before == m_lastUnion.result)
                m_lastUnion = { };
        }

        struct Union {
            unsigned a { 0 };
            unsigned b { 0 };
            unsigned result { 0 };
        };

        size_t m_wordsPerRow { 0 };
        size_t m_wordsPerChunk { 0 };
        unsigned m_rowsPerChunkShift { 0 };
        unsigned m_numberOfRows { 0 };
        Vector<Vector<uint64_t>> m_chunks;
        Vector<unsigned> m_numberOfSlotsPointingTo;
        Vector<unsigned> m_freeRows;
        Vector<unsigned> m_head;
        Vector<unsigned> m_tail;
        Union m_lastUnion;
    };

    // The blocks by their position in post order, so that a solve walks arrays from front to back.
    struct BlocksInPostOrder {
        static constexpr unsigned none = UINT_MAX;

        std::span<const unsigned> predecessorsOf(unsigned position) const LIFETIME_BOUND
        {
            return predecessors.subspan(firstPredecessor[position], firstPredecessor[position + 1] - firstPredecessor[position]);
        }

        Vector<unsigned> blockIndexes;
        Vector<unsigned> positionOfBlock;
        Vector<unsigned> firstPredecessor;
        Vector<unsigned> predecessors;
        Vector<unsigned> previousInLayout;
        Vector<unsigned> nextInLayout;
    };

    BlocksInPostOrder orderBlocks() const
    {
        BlocksInPostOrder order;
        order.blockIndexes = computeBlocksInPostOrder();
        order.positionOfBlock.fill(0, m_code.size());
        for (unsigned position = 0; position < order.blockIndexes.size(); ++position)
            order.positionOfBlock[order.blockIndexes[position]] = position;
        for (unsigned blockIndex : order.blockIndexes) {
            order.firstPredecessor.append(order.predecessors.size());
            for (BasicBlock* predecessor : m_code[blockIndex]->predecessors())
                order.predecessors.append(order.positionOfBlock[predecessor->index()]);
        }
        order.firstPredecessor.append(order.predecessors.size());
        if constexpr (recordsLayoutBoundaries) {
            order.previousInLayout.fill(BlocksInPostOrder::none, order.blockIndexes.size());
            order.nextInLayout.fill(BlocksInPostOrder::none, order.blockIndexes.size());
            unsigned previous = BlocksInPostOrder::none;
            for (BasicBlock* block : m_code) {
                unsigned position = order.positionOfBlock[block->index()];
                order.previousInLayout[position] = previous;
                if (previous != BlocksInPostOrder::none)
                    order.nextInLayout[previous] = position;
                previous = position;
            }
        }
        return order;
    }

    void recordBoundaryAfter(const SliceRows& rows, const BlocksInPostOrder& order, size_t firstWord, unsigned position)
    {
        const size_t words = rows.wordsPerRow();
        const unsigned next = order.nextInLayout[position];
        const bool hasNext = next != BlocksInPostOrder::none;
        if (hasNext && rows.tailOf(position) == rows.headOf(next))
            return;
        const unsigned blockIndex = order.blockIndexes[position];
        const uint64_t* tail = rows.at(rows.tailOf(position));
        const uint64_t* headOfNext = hasNext ? rows.at(rows.headOf(next)) : nullptr;
        forEachInDifference(tail, headOfNext, firstWord, words, [&](unsigned index) {
            m_liveOnlyAtTail.append(blockIndex, index);
        });
        if (!headOfNext)
            return;
        forEachInDifference(headOfNext, tail, firstWord, words, [&](unsigned index) {
            m_liveOnlyAtHeadOfNextBlock.append(blockIndex, index);
        });
    }

    void recordBoundariesOfSlice(const SliceRows& rows, const BlocksInPostOrder& order, size_t firstWord, const Vector<unsigned>& visitedPositions, Vector<unsigned>& lastSliceRecordingBoundaryAfter, unsigned sliceNumber)
    {
        auto recordOnce = [&](unsigned position) {
            if (lastSliceRecordingBoundaryAfter[position] == sliceNumber)
                return;
            lastSliceRecordingBoundaryAfter[position] = sliceNumber;
            recordBoundaryAfter(rows, order, firstWord, position);
        };
        for (unsigned position : visitedPositions) {
            recordOnce(position);
            if (unsigned previous = order.previousInLayout[position]; previous != BlocksInPostOrder::none) {
                recordOnce(previous);
                continue;
            }
            forEachInDifference(rows.at(rows.headOf(position)), nullptr, firstWord, rows.wordsPerRow(), [&](unsigned index) {
                m_liveAtHeadOfFirstBlock.append(index);
            });
        }
    }

    NEVER_INLINE void computeInSlices()
    {
        m_usesSlices = true;
        const unsigned numberOfBlocks = m_code.size();

        m_rowOfBlock.fill(std::nullopt, numberOfBlocks);
        unsigned numberOfRows = 0;
        for (BasicBlock* block : m_code) {
            if (this->needsLiveAtTail(block))
                m_rowOfBlock[block->index()] = numberOfRows++;
        }
        Vector<TailSegment> segments;

        // A visit to a block that names nothing costs the same at any width, so wide slices save visits. How wide the kept tails are decides when the solve gives up.
        const size_t minWordsPerSlice = numberOfRows ? AirLimits::minWordsPerLivenessSlice : AirLimits::minWordsPerLivenessSliceWithoutTails;
        const size_t wordsPerSlice = std::max(minWordsPerSlice, AirLimits::maxWordsPerLivenessMatrix / numberOfBlocks);
        if constexpr (requires { this->willComputeInSlices(wordsPerSlice); })
            this->willComputeInSlices(wordsPerSlice);

        const BlocksInPostOrder order = orderBlocks();
        const unsigned numberOfPositions = order.blockIndexes.size();
        SliceRows rows(numberOfPositions);
        Vector<unsigned> lastSliceVisiting(FillWith { }, numberOfPositions, 0u);
        Vector<unsigned> lastSliceRecordingBoundaryAfter;
        if constexpr (recordsLayoutBoundaries)
            lastSliceRecordingBoundaryAfter.fill(0, numberOfPositions);
        Vector<unsigned> visitedPositions;
        unsigned sliceNumber = 0;

        LowestFirstWorklist worklist(numberOfPositions);
        for (size_t firstWord = 0; firstWord < m_wordsPerSet; firstWord += wordsPerSlice) {
            const size_t words = std::min(wordsPerSlice, m_wordsPerSet - firstWord);
            rows.beginSlice(words);
            ++sliceNumber;
            visitedPositions.shrink(0);

            auto markVisited = [&](unsigned position) {
                if (lastSliceVisiting[position] == sliceNumber)
                    return;
                lastSliceVisiting[position] = sliceNumber;
                visitedPositions.append(position);
            };

            auto fillRowsOf = [&](BasicBlock* block) {
                unsigned gen = rows.addRows(3);
                if (!this->fillRows(block, firstWord, words, rows.at(gen), rows.at(gen + 1), rows.at(gen + 2))) {
                    rows.removeRowsFrom(gen);
                    return;
                }
                unsigned position = order.positionOfBlock[block->index()];
                rows.gen[position] = gen;
                if (rows.isEmpty(gen + 2))
                    rows.removeRowsFrom(gen + 2);
                else
                    rows.setTail(position, gen + 2);
                markVisited(position);
                worklist.add(position);
            };
            // There can be as many slices as blocks, so an adapter that knows which blocks name an index of the slice says so.
            if constexpr (requires { this->nextBlockInSlice(firstWord, words); }) {
                while (std::optional<unsigned> blockIndex = this->nextBlockInSlice(firstWord, words))
                    fillRowsOf(m_code[*blockIndex]);
            } else {
                for (BasicBlock* block : m_code)
                    fillRowsOf(block);
            }

            while (auto position = worklist.takeLowest()) {
                unsigned head = rows.gen[*position] ? rows.applyTransfer(*position, rows.headOf(*position)) : rows.tailOf(*position);
                if (head == rows.headOf(*position))
                    continue;
                rows.setHead(*position, head);
                for (unsigned predecessor : order.predecessorsOf(*position)) {
                    unsigned united = rows.unite(rows.tailOf(predecessor), head);
                    if (united == rows.tailOf(predecessor))
                        continue;
                    rows.setTail(predecessor, united);
                    markVisited(predecessor);
                    worklist.add(predecessor);
                }
            }

            if constexpr (recordsLayoutBoundaries)
                recordBoundariesOfSlice(rows, order, firstWord, visitedPositions, lastSliceRecordingBoundaryAfter, sliceNumber);

            if (numberOfRows) {
                for (unsigned position : visitedPositions) {
                    std::optional<unsigned> row = m_rowOfBlock[order.blockIndexes[position]];
                    if (row && rows.tailOf(position)) {
                        segments.append(TailSegment { *row, static_cast<unsigned>(firstWord), static_cast<unsigned>(words), m_store.size() });
                        m_store.append(std::span<const uint64_t> { rows.at(rows.tailOf(position)), words });
                    }
                }
            }
            for (unsigned position : visitedPositions)
                rows.forget(position);
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
