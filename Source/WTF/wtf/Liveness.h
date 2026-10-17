/*
 * Copyright (C) 2015-2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <algorithm>
#include <bit>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/GraphOrdering.h>
#include <wtf/SparseBitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace WTF {

// HEADS UP: The algorithm here is duplicated in AirRegLiveness.h. That one uses sets rather
// than fancy vectors, because that's better for register liveness analysis.
template<typename Adapter>
class Liveness : public Adapter {
public:
    typedef typename Adapter::CFG CFG;
    typedef typename Adapter::Thing Thing;

    template<typename... Arguments>
    Liveness(CFG& cfg, Arguments&&... arguments)
        : Adapter(std::forward<Arguments>(arguments)...)
        , m_cfg(cfg)
    {
    }

private:
    static constexpr unsigned wordBits = 64;
#if USE(BUN_JSC_ADDITIONS)
    static constexpr size_t minimumWordsToCompact = 8;
#endif
    enum class Storage : uint8_t { Dense, Sparse };

    static size_t wordsForBits(unsigned numBits)
    {
        return (numBits + wordBits - 1) / wordBits;
    }

public:
    // A live set that a client can walk backwards through a block itself, in index space rather than
    // Thing space. Its representation follows whichever one the fixpoint used.
    class Workset {
    public:
        bool add(unsigned bit)
        {
            if (m_storage == Storage::Dense) {
                ASSERT(bit / wordBits < m_dense.size());
                uint64_t mask = static_cast<uint64_t>(1) << (bit % wordBits);
                uint64_t& word = m_dense[bit / wordBits];
                bool wasSet = word & mask;
                word |= mask;
                return !wasSet;
            }
            return m_sparse.set(bit);
        }

        bool remove(unsigned bit)
        {
            if (m_storage == Storage::Dense) {
                ASSERT(bit / wordBits < m_dense.size());
                uint64_t mask = static_cast<uint64_t>(1) << (bit % wordBits);
                uint64_t& word = m_dense[bit / wordBits];
                bool wasSet = word & mask;
                word &= ~mask;
                return wasSet;
            }
            return m_sparse.reset(bit);
        }

        bool contains(unsigned bit) const
        {
            if (m_storage == Storage::Dense) {
                if (bit / wordBits >= m_dense.size())
                    return false;
                return m_dense[bit / wordBits] & (static_cast<uint64_t>(1) << (bit % wordBits));
            }
            return m_sparse.contains(bit);
        }

        template<typename Func>
        void forEachSetBit(NOESCAPE const Func& func) const
        {
            if (m_storage == Storage::Sparse) {
                m_sparse.forEachSetBit(func);
                return;
            }
            WTF::forEachSetBit(m_dense.span(), [&] (size_t bit) {
                func(static_cast<unsigned>(bit));
            });
        }

    private:
        friend class Liveness;

        void initialize(Storage storage, size_t wordsPerSet)
        {
            m_storage = storage;
            if (storage == Storage::Dense)
                m_dense.fill(0, wordsPerSet);
            else
                m_dense.clear();
            m_sparse.clear();
        }

        void clear()
        {
            if (m_storage == Storage::Dense)
                std::ranges::fill(m_dense, 0);
            else
                m_sparse.clear();
        }

        void copyFromDense(std::span<const uint64_t> source)
        {
            ASSERT(m_storage == Storage::Dense);
            ASSERT(source.size() == m_dense.size());
            memcpySpan(m_dense.mutableSpan(), source);
        }

#if USE(BUN_JSC_ADDITIONS)
        void copyFromColumns(std::span<const uint64_t> source, std::span<const unsigned> columnIndex)
        {
            ASSERT(m_storage == Storage::Dense);
            std::ranges::fill(m_dense, 0);
            WTF::forEachSetBit(source, [&] (size_t column) {
                unsigned bit = columnIndex[column];
                m_dense[bit / wordBits] |= static_cast<uint64_t>(1) << (bit % wordBits);
            });
        }

#endif
        void copyFromSparse(const SparseBitVector<>& source)
        {
            ASSERT(m_storage == Storage::Sparse);
            m_sparse = source;
        }

        Storage storage() const { return m_storage; }
        std::span<const uint64_t> denseSpan() const LIFETIME_BOUND { return m_dense.span(); }
        const SparseBitVector<>& sparse() const LIFETIME_BOUND { return m_sparse; }

        Vector<uint64_t> m_dense;
        SparseBitVector<> m_sparse;
        Storage m_storage { Storage::Dense };
    };

    // Lets a client run its own backward walk over a block, for the passes that visit only some of
    // the boundaries and so cannot use LocalCalc. Only valid once compute() has run, since the
    // workset has to be built in whichever representation the fixpoint chose.
    Workset makeWorkset()
    {
        ASSERT(m_computed);
        Workset workset;
#if USE(BUN_JSC_ADDITIONS)
        workset.initialize(m_storage, m_wordsPerWorkset);
#else
        workset.initialize(m_storage, m_wordsPerSet);
#endif
        return workset;
    }

    void copyLiveAtTailInto(Workset& workset, typename CFG::Node block)
    {
        ASSERT(m_computed);
        ASSERT(workset.storage() == m_storage);
#if USE(BUN_JSC_ADDITIONS)
        if (m_storage == Storage::Dense) {
            if (m_isCompact)
                workset.copyFromColumns(denseTailSlice(block->index()), m_columnIndex.span());
            else
                workset.copyFromDense(denseTailSlice(block->index()));
        } else
#else
        if (m_storage == Storage::Dense)
            workset.copyFromDense(denseTailSlice(block->index()));
        else
#endif
            workset.copyFromSparse(sparseTail(block->index()));
    }

private:
    class Iterator {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Iterator);
    public:
        Iterator() = default;

#if USE(BUN_JSC_ADDITIONS)
        static Iterator makeBegin(Liveness& liveness, std::span<const uint64_t> dense, const SparseBitVector<>* sparse, Storage storage, std::span<const unsigned> columnIndex = { })
#else
        static Iterator makeBegin(Liveness& liveness, std::span<const uint64_t> dense, const SparseBitVector<>* sparse, Storage storage)
#endif
        {
            Iterator iter;
            iter.m_liveness = &liveness;
            iter.m_storage = storage;
            if (storage == Storage::Dense) {
                iter.m_denseSpan = dense;
#if USE(BUN_JSC_ADDITIONS)
                iter.m_columnIndex = columnIndex;
#endif
                iter.m_wordIndex = 0;
                iter.m_currentWord = dense.empty() ? 0 : dense[0];
                iter.advanceDense();
            } else
                iter.m_sparseIter = sparse->begin();
            return iter;
        }

        static Iterator makeEnd(Liveness& liveness, std::span<const uint64_t> dense, const SparseBitVector<>* sparse, Storage storage)
        {
            Iterator iter;
            iter.m_liveness = &liveness;
            iter.m_storage = storage;
            if (storage == Storage::Dense) {
                iter.m_denseSpan = dense;
                iter.m_wordIndex = dense.size();
                iter.m_currentWord = 0;
            } else
                iter.m_sparseIter = sparse->end();
            return iter;
        }

        Thing operator*() const
        {
            if (m_storage == Storage::Dense) {
                unsigned bit = static_cast<unsigned>(m_wordIndex * wordBits + std::countr_zero(m_currentWord));
#if USE(BUN_JSC_ADDITIONS)
                if (!m_columnIndex.empty())
                    bit = m_columnIndex[bit];
#endif
                return m_liveness->indexToValue(bit);
            }
            return m_liveness->indexToValue(*m_sparseIter);
        }

        Iterator& operator++()
        {
            if (m_storage == Storage::Dense) {
                m_currentWord &= m_currentWord - 1;
                advanceDense();
            } else
                ++m_sparseIter;
            return *this;
        }

        bool operator==(const Iterator& other) const
        {
            if (m_storage == Storage::Dense)
                return m_wordIndex == other.m_wordIndex && m_currentWord == other.m_currentWord;
            return m_sparseIter == other.m_sparseIter;
        }

    private:
        void advanceDense()
        {
            while (!m_currentWord && m_wordIndex + 1 < m_denseSpan.size()) {
                ++m_wordIndex;
                m_currentWord = m_denseSpan[m_wordIndex];
            }
            if (!m_currentWord)
                m_wordIndex = m_denseSpan.size();
        }

        Liveness* m_liveness { nullptr };
        Storage m_storage { Storage::Dense };
        std::span<const uint64_t> m_denseSpan;
#if USE(BUN_JSC_ADDITIONS)
        std::span<const unsigned> m_columnIndex;
#endif
        size_t m_wordIndex { 0 };
        uint64_t m_currentWord { 0 };
        typename SparseBitVector<>::iterator m_sparseIter;
    };

public:
    class Iterable {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Iterable);
    public:
        Iterable(Liveness& liveness, std::span<const uint64_t> dense, const SparseBitVector<>* sparse, Storage storage)
            : m_liveness(liveness)
            , m_dense(dense)
            , m_sparse(sparse)
            , m_storage(storage)
        {
        }

#if USE(BUN_JSC_ADDITIONS)
        Iterator begin() const LIFETIME_BOUND { return Iterator::makeBegin(m_liveness, m_dense, m_sparse, m_storage, m_liveness.m_columnIndex.span()); }
#else
        Iterator begin() const LIFETIME_BOUND { return Iterator::makeBegin(m_liveness, m_dense, m_sparse, m_storage); }
#endif
        Iterator end() const LIFETIME_BOUND { return Iterator::makeEnd(m_liveness, m_dense, m_sparse, m_storage); }

        bool contains(const Thing& thing) const
        {
            unsigned bit = m_liveness.valueToIndex(thing);
#if USE(BUN_JSC_ADDITIONS)
            if (m_storage == Storage::Dense)
                return m_liveness.denseSliceContains(m_dense, bit);
#else
            if (m_storage == Storage::Dense) {
                if (bit / wordBits >= m_dense.size())
                    return false;
                return m_dense[bit / wordBits] & (static_cast<uint64_t>(1) << (bit % wordBits));
            }
#endif
            return m_sparse->contains(bit);
        }

    private:
        Liveness& m_liveness;
        std::span<const uint64_t> m_dense;
        const SparseBitVector<>* m_sparse { nullptr };
        Storage m_storage { Storage::Dense };
    };

    // This calculator has to be run in reverse. It needs forEachUse/forEachDef, so an adapter whose
    // store cannot address an arbitrary boundary drives makeWorkset instead of this.
    class LocalCalc {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LocalCalc);
    public:
        LocalCalc(Liveness& liveness, typename CFG::Node block)
            : m_liveness(liveness)
            , m_block(block)
        {
            liveness.copyLiveAtTailInto(liveness.m_workset, block);
        }

        class Iterable {
            WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Iterable);
        public:
            Iterable(Liveness& liveness)
                : m_liveness(liveness)
            {
            }

            Iterator begin() const LIFETIME_BOUND
            {
                const Workset& workset = m_liveness.m_workset;
                return Iterator::makeBegin(m_liveness, workset.denseSpan(), &workset.sparse(), workset.storage());
            }
            Iterator end() const LIFETIME_BOUND
            {
                const Workset& workset = m_liveness.m_workset;
                return Iterator::makeEnd(m_liveness, workset.denseSpan(), &workset.sparse(), workset.storage());
            }

            bool contains(const Thing& thing) const
            {
                return m_liveness.m_workset.contains(m_liveness.valueToIndex(thing));
            }

        private:
            Liveness& m_liveness;
        };

        Iterable live() const LIFETIME_BOUND
        {
            return Iterable(m_liveness);
        }

        bool isLive(const Thing& thing) const
        {
            return m_liveness.m_workset.contains(m_liveness.valueToIndex(thing));
        }

        void execute(unsigned instIndex)
        {
            Workset& workset = m_liveness.m_workset;
            m_liveness.forEachDef(
                m_block, instIndex + 1,
                [&] (unsigned index) {
                    workset.remove(index);
                });
            m_liveness.forEachUse(
                m_block, instIndex,
                [&] (unsigned index) {
                    workset.add(index);
                });
        }

    private:
        Liveness& m_liveness;
        typename CFG::Node m_block;
    };

    Iterable liveAtHead(typename CFG::Node block) LIFETIME_BOUND
    {
        if (m_storage == Storage::Dense)
            return Iterable(*this, denseHeadSlice(block->index()), nullptr, Storage::Dense);
        return Iterable(*this, { }, &sparseHead(block->index()), Storage::Sparse);
    }

    Iterable liveAtTail(typename CFG::Node block) LIFETIME_BOUND
    {
        if (m_storage == Storage::Dense)
            return Iterable(*this, denseTailSlice(block->index()), nullptr, Storage::Dense);
        return Iterable(*this, { }, &sparseTail(block->index()), Storage::Sparse);
    }

    void forEachLiveAtHead(typename CFG::Node block, NOESCAPE const Invocable<void(const Thing&)> auto& func)
    {
        if (m_storage == Storage::Dense) {
            forEachInDense(denseHeadSlice(block->index()), func);
            return;
        }
        forEachInSparse(sparseHead(block->index()), func);
    }

    void forEachLiveAtTail(typename CFG::Node block, NOESCAPE const Invocable<void(const Thing&)> auto& func)
    {
        if (m_storage == Storage::Dense) {
            forEachInDense(denseTailSlice(block->index()), func);
            return;
        }
        forEachInSparse(sparseTail(block->index()), func);
    }

    void forEachLiveAtHeadNotLiveAtTail(typename CFG::Node headBlock, typename CFG::Node tailBlock, NOESCAPE const Invocable<void(const Thing&)> auto& func)
    {
        if (m_storage == Storage::Dense) {
            forEachInDenseDifference(denseHeadSlice(headBlock->index()), denseTailSlice(tailBlock->index()), func);
            return;
        }
        forEachInSparseDifference(sparseHead(headBlock->index()), sparseTail(tailBlock->index()), func);
    }

    void forEachLiveAtTailNotLiveAtHead(typename CFG::Node tailBlock, typename CFG::Node headBlock, NOESCAPE const Invocable<void(const Thing&)> auto& func)
    {
        if (m_storage == Storage::Dense) {
            forEachInDenseDifference(denseTailSlice(tailBlock->index()), denseHeadSlice(headBlock->index()), func);
            return;
        }
        forEachInSparseDifference(sparseTail(tailBlock->index()), sparseHead(headBlock->index()), func);
    }

    class LiveAtHead {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LiveAtHead);
    public:
        LiveAtHead(Liveness& liveness)
            : m_liveness(liveness)
        {
        }

        bool isLiveAtHead(typename CFG::Node block, const Thing& thing)
        {
            unsigned bit = m_liveness.valueToIndex(thing);
#if USE(BUN_JSC_ADDITIONS)
            if (m_liveness.m_storage == Storage::Dense)
                return m_liveness.denseSliceContains(m_liveness.denseHeadSlice(block->index()), bit);
#else
            if (m_liveness.m_storage == Storage::Dense) {
                std::span<const uint64_t> set = m_liveness.denseHeadSlice(block->index());
                if (bit / wordBits >= set.size())
                    return false;
                return set[bit / wordBits] & (static_cast<uint64_t>(1) << (bit % wordBits));
            }
#endif
            return m_liveness.sparseHead(block->index()).contains(bit);
        }

    private:
        Liveness& m_liveness;
    };

    LiveAtHead liveAtHead() LIFETIME_BOUND { return LiveAtHead(*this); }

protected:
    // An adapter that can report a block's uses and defs straight off its instructions does not need
    // the per-boundary table built up front, since the dense fixpoint reads each block exactly once.
    static constexpr bool adapterStreamsActions()
    {
        if constexpr (requires { Adapter::streamsActions; })
            return Adapter::streamsActions;
        else
            return false;
    }

    void compute()
    {
        uint64_t denseMatrixBits = static_cast<uint64_t>(m_cfg.numNodes()) * Adapter::numIndices();
        constexpr uint64_t denseMatrixBitBudget = 64 * 1024 * 1024; // 8 MB per live-set matrix.
        bool useSparse = denseMatrixBits > denseMatrixBitBudget;
#if ASSERT_ENABLED
        // Force the sparse storage on roughly half of the otherwise-dense functions in debug builds so
        // the sparse implementation gets coverage from the regular test suite.
        if (!useSparse && (m_cfg.numNodes() & 1))
            useSparse = true;
#endif
        if (useSparse)
            computeSparse();
        else
            computeDense();
    }

    void computeDense()
    {
#if USE(BUN_JSC_ADDITIONS)
        m_columnIndex.shrink(0);
        m_isCompact = false;
#endif
        if constexpr (!adapterStreamsActions())
            Adapter::prepareToCompute();

        unsigned numNodes = m_cfg.numNodes();
        unsigned numIndices = Adapter::numIndices();

        m_storage = Storage::Dense;
#if USE(BUN_JSC_ADDITIONS)
        m_wordsPerWorkset = wordsForBits(numIndices);
        m_workset.initialize(Storage::Dense, m_wordsPerWorkset);
        if (m_wordsPerWorkset >= minimumWordsToCompact) {
            computeDenseCompact(numNodes, numIndices);
            return;
        }
        m_wordsPerSet = m_wordsPerWorkset;
#else
        m_wordsPerSet = wordsForBits(numIndices);
#endif

        size_t matrixWords = static_cast<size_t>(numNodes) * m_wordsPerSet;
        m_denseStore.fill(0, 2 * matrixWords);
        Vector<uint64_t> genStore(matrixWords);
        Vector<uint64_t> killStore(matrixWords);
        // Vector's sized constructor does not zero POD storage, and the dataflow ORs into these.
        // gen is initialized with a value, zero initialization is not necessary (costly).
        std::ranges::fill(killStore, 0);
        std::span<uint64_t> store = m_denseStore.mutableSpan();
#if !USE(BUN_JSC_ADDITIONS)
        std::span<uint64_t> liveInMatrix = store.subspan(0, matrixWords);
#endif
        std::span<uint64_t> liveOutMatrix = store.subspan(matrixWords, matrixWords);
        std::span<uint64_t> genMatrix = genStore.mutableSpan();
        std::span<uint64_t> killMatrix = killStore.mutableSpan();

#if !USE(BUN_JSC_ADDITIONS)
        m_workset.initialize(Storage::Dense, m_wordsPerSet);

#endif
        auto setFor = [&] (std::span<uint64_t> matrix, unsigned blockIndex) -> std::span<uint64_t> {
            return matrix.subspan(static_cast<size_t>(blockIndex) * m_wordsPerSet, m_wordsPerSet);
        };
        auto setBit = [] (std::span<uint64_t> set, unsigned index) {
            set[index / wordBits] |= static_cast<uint64_t>(1) << (index % wordBits);
        };

        unsigned numActiveBlocks = 0;
        for (unsigned blockIndex = 0; blockIndex < numNodes; ++blockIndex) {
            auto block = m_cfg.node(blockIndex);
            if (!block)
                continue;

            auto killSet = setFor(killMatrix, blockIndex);
            auto genSet = setFor(genMatrix, blockIndex);
            auto liveOutSet = setFor(liveOutMatrix, blockIndex);

            // gen = the transfer function applied to an empty live-out: the uses exposed at the head.
            // The fixpoint below works purely on gen/kill/liveOut, so this is the only place that
            // walks the block.
            m_workset.clear();
            unsigned blockSize = Adapter::blockSize(block);
            auto visitBoundary = [&](unsigned boundary, auto group) {
                // The uses at the tail boundary are live-out rather than gen. A streaming adapter has
                // them in hand here; otherwise they are seeded from forEachUseAtTail below.
                if (boundary == blockSize) {
                    if constexpr (adapterStreamsActions())
                        Adapter::forEachUseInGroup(group, [&](unsigned index) { setBit(liveOutSet, index); });
                } else
                    Adapter::forEachUseInGroup(group, [&](unsigned index) { m_workset.add(index); });

                Adapter::forEachDefInGroup(group, [&](unsigned index) {
                    m_workset.remove(index);
                    setBit(killSet, index);
                });
            };

            if constexpr (adapterStreamsActions())
                Adapter::forEachActionGroupDescendingStreaming(block, visitBoundary);
            else {
                Adapter::forEachActionGroupDescending(block, visitBoundary);
                // liveOut automatically contains the LateUse's of the terminal.
                Adapter::forEachUseAtTail(block, [&](unsigned index) {
                    setBit(liveOutSet, index);
                });
            }
            std::span<const uint64_t> worksetSpan = m_workset.denseSpan();
            for (size_t i = 0; i < m_wordsPerSet; ++i)
                genSet[i] = worksetSpan[i];

            ++numActiveBlocks;
        }

#if USE(BUN_JSC_ADDITIONS)
        solveDense(genMatrix, killMatrix, numActiveBlocks);
    }

    void computeDenseCompact(unsigned numNodes, unsigned numIndices)
    {
        struct Ends {
            unsigned gen { 0 };
            unsigned kill { 0 };
            unsigned tail { 0 };
        };
        Vector<Ends> blockEnds(numNodes);
        Vector<unsigned> gens;
        Vector<unsigned> kills;
        Vector<unsigned> tails;
        Vector<unsigned, 64> added;
        kills.reserveInitialCapacity(numIndices);
        m_crossesBoundary.fill(0, m_wordsPerWorkset);
        std::span<uint64_t> crossesBoundary = m_crossesBoundary.mutableSpan();

        auto setBit = [] (std::span<uint64_t> set, unsigned index) {
            set[index / wordBits] |= static_cast<uint64_t>(1) << (index % wordBits);
        };

        unsigned numActiveBlocks = 0;
        for (unsigned blockIndex = 0; blockIndex < numNodes; ++blockIndex) {
            if (auto block = m_cfg.node(blockIndex)) {
                unsigned blockSize = Adapter::blockSize(block);
                auto visitBoundary = [&](unsigned boundary, auto group) {
                    if (boundary == blockSize) {
                        if constexpr (adapterStreamsActions())
                            Adapter::forEachUseInGroup(group, [&](unsigned index) { tails.append(index); });
                    } else {
                        Adapter::forEachUseInGroup(group, [&](unsigned index) {
                            if (m_workset.add(index))
                                added.append(index);
                        });
                    }

                    Adapter::forEachDefInGroup(group, [&](unsigned index) {
                        m_workset.remove(index);
                        kills.append(index);
                    });
                };

                if constexpr (adapterStreamsActions())
                    Adapter::forEachActionGroupDescendingStreaming(block, visitBoundary);
                else {
                    Adapter::forEachActionGroupDescending(block, visitBoundary);
                    Adapter::forEachUseAtTail(block, [&](unsigned index) {
                        tails.append(index);
                    });
                }
                for (unsigned index : added) {
                    if (m_workset.remove(index)) {
                        gens.append(index);
                        setBit(crossesBoundary, index);
                    }
                }
                added.shrink(0);
                ++numActiveBlocks;
            }
            blockEnds[blockIndex] = { static_cast<unsigned>(gens.size()), static_cast<unsigned>(kills.size()), static_cast<unsigned>(tails.size()) };
        }
        for (unsigned index : tails)
            setBit(crossesBoundary, index);

        m_isCompact = true;
        m_columnOfIndex.resize(numIndices);
        WTF::forEachSetBit(m_crossesBoundary.span(), [&] (size_t index) {
            m_columnOfIndex[index] = m_columnIndex.size();
            m_columnIndex.append(static_cast<unsigned>(index));
        });
        m_wordsPerSet = wordsForBits(m_columnIndex.size());

        size_t matrixWords = static_cast<size_t>(numNodes) * m_wordsPerSet;
        m_denseStore.fill(0, 2 * matrixWords);
        Vector<uint64_t> transferStore;
        transferStore.fill(0, 2 * matrixWords);
        std::span<uint64_t> liveOutMatrix = m_denseStore.mutableSpan().subspan(matrixWords, matrixWords);
        std::span<uint64_t> genMatrix = transferStore.mutableSpan().subspan(0, matrixWords);
        std::span<uint64_t> killMatrix = transferStore.mutableSpan().subspan(matrixWords, matrixWords);

        Ends begin;
        for (unsigned blockIndex = 0; blockIndex < numNodes; ++blockIndex) {
            Ends end = blockEnds[blockIndex];
            size_t offset = static_cast<size_t>(blockIndex) * m_wordsPerSet;
            std::span<uint64_t> genSet = genMatrix.subspan(offset, m_wordsPerSet);
            std::span<uint64_t> killSet = killMatrix.subspan(offset, m_wordsPerSet);
            std::span<uint64_t> liveOutSet = liveOutMatrix.subspan(offset, m_wordsPerSet);
            for (unsigned i = begin.gen; i < end.gen; ++i)
                setBit(genSet, m_columnOfIndex[gens[i]]);
            for (unsigned i = begin.kill; i < end.kill; ++i) {
                unsigned index = kills[i];
                if (crossesBoundary[index / wordBits] & (static_cast<uint64_t>(1) << (index % wordBits)))
                    setBit(killSet, m_columnOfIndex[index]);
            }
            for (unsigned i = begin.tail; i < end.tail; ++i)
                setBit(liveOutSet, m_columnOfIndex[tails[i]]);
            begin = end;
        }

        solveDense(genMatrix, killMatrix, numActiveBlocks);
    }

    void solveDense(std::span<const uint64_t> genMatrix, std::span<const uint64_t> killMatrix, unsigned numActiveBlocks)
    {
        unsigned numNodes = m_cfg.numNodes();
        size_t matrixWords = static_cast<size_t>(numNodes) * m_wordsPerSet;
        std::span<uint64_t> store = m_denseStore.mutableSpan();
        std::span<uint64_t> liveInMatrix = store.subspan(0, matrixWords);
        std::span<uint64_t> liveOutMatrix = store.subspan(matrixWords, matrixWords);

        auto setFor = [&] (auto matrix, unsigned blockIndex) {
            return matrix.subspan(static_cast<size_t>(blockIndex) * m_wordsPerSet, m_wordsPerSet);
        };

#endif
        // Reverse-post-order seed so the backward dataflow processes a block after its successors.
        Vector<unsigned, 64> worklist;
        worklist.reserveInitialCapacity(numActiveBlocks);
        BitVector dirtyBlocks(numNodes);
        appendNodeIndicesInOrder(m_cfg, GraphOrder::PostOrder, dirtyBlocks, worklist);
        worklist.reverse();
        // Active blocks the DFS did not reach (e.g. unreachable but not yet pruned).
        for (unsigned blockIndex = 0; blockIndex < numNodes; ++blockIndex) {
            if (m_cfg.node(blockIndex) && !dirtyBlocks.quickSet(blockIndex))
                worklist.append(blockIndex);
        }

        while (!worklist.isEmpty()) {
            unsigned blockIndex = worklist.takeLast();
            bool cleared = dirtyBlocks.quickClear(blockIndex);
            ASSERT_UNUSED(cleared, cleared);

            auto block = m_cfg.node(blockIndex);
            ASSERT(block);

            auto liveInSet = setFor(liveInMatrix, blockIndex);
            auto liveOutSet = setFor(liveOutMatrix, blockIndex);
            auto genSet = setFor(genMatrix, blockIndex);
            auto killSet = setFor(killMatrix, blockIndex);

            // liveIn = gen | (liveOut & ~kill)
            uint64_t changed = 0;
            for (size_t i = 0; i < m_wordsPerSet; ++i) {
                uint64_t newLiveIn = genSet[i] | (liveOutSet[i] & ~killSet[i]);
                uint64_t oldLiveIn = liveInSet[i];
                liveInSet[i] = newLiveIn;
                changed |= newLiveIn ^ oldLiveIn;
            }
            if (!changed)
                continue;

            for (auto predecessor : m_cfg.predecessors(block)) {
                auto predLiveOut = setFor(liveOutMatrix, predecessor->index());
                uint64_t predChanged = 0;
                for (size_t i = 0; i < m_wordsPerSet; ++i) {
                    uint64_t oldLiveOut = predLiveOut[i];
                    uint64_t newLiveOut = oldLiveOut | liveInSet[i];
                    predLiveOut[i] = newLiveOut;
                    predChanged |= newLiveOut ^ oldLiveOut;
                }
                if (predChanged && !dirtyBlocks.quickSet(predecessor->index()))
                    worklist.append(predecessor->index());
            }
        }

#if ASSERT_ENABLED
        m_computed = true;
#endif
    }

    // Sparse fallback for functions whose dense matrices would exceed the budget.
    void computeSparse()
    {
#if USE(BUN_JSC_ADDITIONS)
        m_columnIndex.shrink(0);
        m_isCompact = false;
#endif
        Adapter::prepareToCompute();

        unsigned numNodes = m_cfg.numNodes();

        m_storage = Storage::Sparse;
        m_wordsPerSet = 0;
        m_sparseStore.clear();
        m_sparseStore.grow(2 * numNodes);
        m_workset.initialize(Storage::Sparse, 0);

        BitVector dirtyBlocks(numNodes);
        Vector<unsigned, 64> worklist;
        for (unsigned blockIndex = 0; blockIndex < numNodes; ++blockIndex) {
            auto block = m_cfg.node(blockIndex);
            if (!block)
                continue;

            // liveAtTail automatically contains the LateUse's of the terminal.
            auto& liveAtTail = sparseTail(blockIndex);
            Adapter::forEachUseAtTail(
                block,
                [&] (unsigned index) {
                    liveAtTail.set(index);
                });

            dirtyBlocks.quickSet(blockIndex);
            worklist.append(blockIndex);
        }

#if USE(BUN_JSC_ADDITIONS)
        SparseBitVector<> delta;
#else
        Vector<unsigned, 64> delta;
#endif
        while (!worklist.isEmpty()) {
            unsigned blockIndex = worklist.takeLast();
            bool cleared = dirtyBlocks.quickClear(blockIndex);
            ASSERT_UNUSED(cleared, cleared);

            auto block = m_cfg.node(blockIndex);
            ASSERT(block);

#if USE(BUN_JSC_ADDITIONS)
            m_workset.copyFromSparse(sparseTail(blockIndex));
#else
            m_workset.clear();
            sparseTail(blockIndex).forEachSetBit(
                [&] (unsigned index) {
                    m_workset.add(index);
                });
#endif
            // The uses at the tail boundary are already in the tail set, so re-adding them here is a
            // no-op and needs no special case.
            Adapter::forEachActionGroupDescending(block,
                [&] (unsigned, typename Adapter::ActionGroup group) {
                    Adapter::forEachUseInGroup(group, [&] (unsigned index) { m_workset.add(index); });
                    Adapter::forEachDefInGroup(group, [&] (unsigned index) { m_workset.remove(index); });
                });

#if USE(BUN_JSC_ADDITIONS)
            if (!sparseHead(blockIndex).merge(m_workset.sparse(), &delta))
                continue;

            for (auto predecessor : m_cfg.predecessors(block)) {
                if (sparseTail(predecessor->index()).merge(delta) && !dirtyBlocks.quickSet(predecessor->index()))
                    worklist.append(predecessor->index());
            }
#else
            auto& liveAtHead = sparseHead(blockIndex);
            delta.shrink(0);
            m_workset.sparse().forEachSetBit([&] (unsigned index) {
                if (liveAtHead.set(index))
                    delta.append(index);
            });

            if (delta.isEmpty())
                continue;

            for (auto predecessor : m_cfg.predecessors(block)) {
                auto& predTail = sparseTail(predecessor->index());
                bool changed = false;
                for (unsigned index : delta) {
                    if (predTail.set(index))
                        changed = true;
                }
                if (changed && !dirtyBlocks.quickSet(predecessor->index()))
                    worklist.append(predecessor->index());
            }
#endif
        }

#if ASSERT_ENABLED
        m_computed = true;
#endif
    }

private:
    friend class LocalCalc;
    friend class LocalCalc::Iterable;
    friend class Iterable;
    friend class LiveAtHead;

#if USE(BUN_JSC_ADDITIONS)
    bool denseSliceContains(std::span<const uint64_t> set, unsigned bit) const
    {
        if (m_isCompact) {
            if (bit / wordBits >= m_crossesBoundary.size() || !(m_crossesBoundary[bit / wordBits] & (static_cast<uint64_t>(1) << (bit % wordBits))))
                return false;
            bit = m_columnOfIndex[bit];
        }
        if (bit / wordBits >= set.size())
            return false;
        return set[bit / wordBits] & (static_cast<uint64_t>(1) << (bit % wordBits));
    }

#endif
    void forEachInDense(std::span<const uint64_t> set, NOESCAPE const Invocable<void(const Thing&)> auto& func)
    {
#if USE(BUN_JSC_ADDITIONS)
        if (m_isCompact) {
            WTF::forEachSetBit(set, [&] (size_t column) {
                func(this->indexToValue(m_columnIndex[column]));
            });
            return;
        }
#endif
        WTF::forEachSetBit(set, [&] (size_t index) {
            func(this->indexToValue(static_cast<unsigned>(index)));
        });
    }

    void forEachInSparse(const SparseBitVector<>& set, NOESCAPE const Invocable<void(const Thing&)> auto& func)
    {
        set.forEachSetBit(
            [&](auto index) {
                func(this->indexToValue(static_cast<unsigned>(index)));
            });
    }

    void forEachInDenseDifference(std::span<const uint64_t> a, std::span<const uint64_t> b, NOESCAPE const Invocable<void(const Thing&)> auto& func)
    {
        for (size_t wordIndex = 0; wordIndex < m_wordsPerSet; ++wordIndex) {
            uint64_t word = a[wordIndex] & ~b[wordIndex];
            while (word) {
                unsigned bit = std::countr_zero(word);
                word &= word - 1;
#if USE(BUN_JSC_ADDITIONS)
                unsigned index = static_cast<unsigned>(wordIndex * wordBits + bit);
                func(this->indexToValue(m_isCompact ? m_columnIndex[index] : index));
#else
                func(this->indexToValue(static_cast<unsigned>(wordIndex * wordBits + bit)));
#endif
            }
        }
    }

    void forEachInSparseDifference(const SparseBitVector<>& a, const SparseBitVector<>& b, NOESCAPE const Invocable<void(const Thing&)> auto& func)
    {
#if USE(BUN_JSC_ADDITIONS)
        a.forEachSetBitNotIn(b,
            [&](auto index) {
                func(this->indexToValue(static_cast<unsigned>(index)));
            });
#else
        a.forEachSetBit(
            [&](auto index) {
                if (!b.contains(index))
                    func(this->indexToValue(static_cast<unsigned>(index)));
            });
#endif
    }

    size_t denseHalfWords() const { return m_denseStore.size() / 2; }
    std::span<const uint64_t> denseHeadSlice(unsigned blockIndex) const LIFETIME_BOUND
    {
        return m_denseStore.span().subspan(static_cast<size_t>(blockIndex) * m_wordsPerSet, m_wordsPerSet);
    }
    std::span<const uint64_t> denseTailSlice(unsigned blockIndex) const LIFETIME_BOUND
    {
        return m_denseStore.span().subspan(denseHalfWords() + static_cast<size_t>(blockIndex) * m_wordsPerSet, m_wordsPerSet);
    }

    size_t sparseHalfSize() const { return m_sparseStore.size() / 2; }
    SparseBitVector<>& sparseHead(unsigned blockIndex) LIFETIME_BOUND { return m_sparseStore[blockIndex]; }
    const SparseBitVector<>& sparseHead(unsigned blockIndex) const LIFETIME_BOUND { return m_sparseStore[blockIndex]; }
    SparseBitVector<>& sparseTail(unsigned blockIndex) LIFETIME_BOUND { return m_sparseStore[sparseHalfSize() + blockIndex]; }
    const SparseBitVector<>& sparseTail(unsigned blockIndex) const LIFETIME_BOUND { return m_sparseStore[sparseHalfSize() + blockIndex]; }

    CFG& m_cfg;
    Workset m_workset;
    Vector<uint64_t> m_denseStore;
    Vector<SparseBitVector<>> m_sparseStore;
#if USE(BUN_JSC_ADDITIONS)
    Vector<uint64_t> m_crossesBoundary;
    Vector<unsigned> m_columnOfIndex;
    Vector<unsigned> m_columnIndex;
#endif
    size_t m_wordsPerSet { 0 };
#if USE(BUN_JSC_ADDITIONS)
    size_t m_wordsPerWorkset { 0 };
#endif
    Storage m_storage { Storage::Dense };
#if USE(BUN_JSC_ADDITIONS)
    bool m_isCompact { false };
#endif
#if ASSERT_ENABLED
    bool m_computed { false };
#endif
};

} // namespace WTF

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
