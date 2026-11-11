/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <array>
#include <bit>
#include <iterator>
#include <ranges>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>
#include <wtf/LikelyDenseUnsignedIntegerSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace WTF {

// This file offers multiple implementations of a simple interference graph, with varying performance characteristics.
// Conceptually they are just sets of pairs of integers
// All of them have the same interface:
//   bool contains(IndexType u, IndexType v)
//   bool addAndReturnIsNewEntry(IndexType u, IndexType v)
//   void add(IndexType u, IndexType v)
//   void clear()
//   void mayClear(IndexType u)
//   void setMaxIndex(unsigned n)
//   void forEach(const Functor& functor)
//   unsigned size()
//   unsigned memoryUse()
//   void dumpMemoryUseInKB()
// Three useful type aliases are defined:
//   DenseInterferenceGraph, which is the fastest but has quadratic memory use in the maximum vertex index (8MB for n = 8192)
//   LargeInterferenceGraph, as long as the indices fit in a uint16_t
//   HugeInterferenceGraph otherwise
// If you need to iterate over the indices that interfere with a given index, you must use the following:
//   DenseIterableInterferenceGraph
//   LargeIterableInterferenceGraph
//   HugeIterableInterferenceGraph
// The large and huge versions are 2x the memory of their non-iterable versions, but offer an additional operator[] method, which returns an iterable object
// The iterable versions also offer these, where a set of indices is a span of 64-bit words:
//   void addAllBut(IndexType u, const LiveSet& others)
//   void mirrorEdges()
//   size_t countInterferingIn(IndexType u, std::span<const uint64_t> others)
//   void forEachInterferingIn(IndexType u, std::span<const uint64_t> others, const Functor& functor)
//   void forEachNonInterferingIn(IndexType u, std::span<const uint64_t> others, const Functor& functor)
// After a call of addAllBut(), only add() and addAllBut() may be called until mirrorEdges() is.

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

class InterferenceRows {
public:
    using IndexType = uint16_t;
    static constexpr unsigned minSizeToMirrorEdgesAtOnce = 1024;

    bool contains(IndexType u, IndexType v) { return row(u)[v / 64] >> (v % 64) & 1; }

    bool addAndReturnIsNewEntry(IndexType u, IndexType v)
    {
        bool isNewEntry = !contains(u, v);
        add(u, v);
        return isNewEntry;
    }

    void add(IndexType u, IndexType v)
    {
        row(u)[v / 64] |= static_cast<uint64_t>(1) << (v % 64);
        row(v)[u / 64] |= static_cast<uint64_t>(1) << (u % 64);
    }

    template<typename LiveSet>
    void addAllBut(IndexType u, const LiveSet& others)
    {
        if constexpr (requires { others.words(); })
            addAllWordsBut(u, others.words());
        else {
            others.forEachSetBit([&](unsigned v) {
                if (v != u)
                    add(u, static_cast<IndexType>(v));
            });
        }
    }

    template<typename LiveSet>
    void addAllBut(IndexType u, IndexType alsoNot, const LiveSet& others)
    {
        const uint64_t bit = static_cast<uint64_t>(1) << (alsoNot % 64);
        const bool hadEdge = row(u)[alsoNot / 64] & bit;
        addAllBut(u, others);
        if (hadEdge || u == alsoNot)
            return;
        row(u)[alsoNot / 64] &= ~bit;
        if (!m_mirrorsEdgesAtOnce)
            row(alsoNot)[u / 64] &= ~(static_cast<uint64_t>(1) << (u % 64));
    }

    void mirrorEdges()
    {
        if (!m_mirrorsEdgesAtOnce)
            return;
        std::array<uint64_t, 64> upper;
        std::array<uint64_t, 64> lower;
        uint64_t* rows = m_rows.mutableSpan().data();
        auto load = [&](std::array<uint64_t, 64>& tile, size_t tileRow, size_t tileColumn) {
            uint64_t any = 0;
            for (size_t i = 0; i < 64; ++i)
                any |= tile[i] = rows[(tileRow * 64 + i) * m_wordsPerRow + tileColumn];
            return any;
        };
        auto store = [&](const std::array<uint64_t, 64>& tile, size_t tileRow, size_t tileColumn) {
            for (size_t i = 0; i < 64; ++i)
                rows[(tileRow * 64 + i) * m_wordsPerRow + tileColumn] = tile[i];
        };
        for (size_t tileRow = 0; tileRow < m_wordsPerRow; ++tileRow) {
            for (size_t tileColumn = tileRow; tileColumn < m_wordsPerRow; ++tileColumn) {
                uint64_t any = load(upper, tileRow, tileColumn);
                any |= load(lower, tileColumn, tileRow);
                if (!any)
                    continue;
                transpose(lower);
                for (size_t i = 0; i < 64; ++i)
                    upper[i] |= lower[i];
                store(upper, tileRow, tileColumn);
                transpose(upper);
                store(upper, tileColumn, tileRow);
            }
        }
    }

    size_t countInterferingIn(IndexType u, std::span<const uint64_t> others)
    {
        ASSERT(others.size() == m_wordsPerRow);
        const uint64_t* uRow = row(u);
        size_t result = 0;
        for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex)
            result += std::popcount(uRow[wordIndex] & others[wordIndex]);
        return result;
    }

    void forEachInterferingIn(IndexType u, std::span<const uint64_t> others, NOESCAPE const Invocable<void(unsigned)> auto& func)
    {
        ASSERT(others.size() == m_wordsPerRow);
        const uint64_t* uRow = row(u);
        for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex) {
            for (uint64_t word = uRow[wordIndex] & others[wordIndex]; word; word &= word - 1)
                func(wordIndex * 64 + std::countr_zero(word));
        }
    }

    void forEachNonInterferingIn(IndexType u, std::span<const uint64_t> others, NOESCAPE const Invocable<void(unsigned)> auto& func)
    {
        ASSERT(others.size() == m_wordsPerRow);
        const uint64_t* uRow = row(u);
        for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex) {
            for (uint64_t word = others[wordIndex] & ~uRow[wordIndex]; word; word &= word - 1)
                func(wordIndex * 64 + std::countr_zero(word));
        }
    }

    void clear() { std::ranges::fill(m_rows, 0); }

    void mayClear(IndexType) { }

    void setMaxIndex(unsigned n)
    {
        ASSERT(n < std::numeric_limits<IndexType>::max());
        m_numElements = n;
        m_wordsPerRow = (n + 63) / 64;
        m_mirrorsEdgesAtOnce = n >= minSizeToMirrorEdgesAtOnce;
        m_rows.fill(0, (m_mirrorsEdgesAtOnce ? m_wordsPerRow * 64 : static_cast<size_t>(n)) * m_wordsPerRow);
    }

    template <typename Functor>
    void forEach(NOESCAPE const Functor& functor)
    {
        for (unsigned u = 0; u < m_numElements; ++u) {
            for (IndexType v : (*this)[static_cast<IndexType>(u)]) {
                if (v > u)
                    functor({ static_cast<IndexType>(u), v });
            }
        }
    }

    unsigned size() const
    {
        size_t numberOfBits = 0;
        for (uint64_t word : m_rows)
            numberOfBits += std::popcount(word);
        return numberOfBits / 2;
    }

    unsigned memoryUse() const { return m_rows.size() * sizeof(uint64_t); }

    void dumpMemoryUseInKB() const { dataLog(memoryUse() / 1024); }

    class Iterable {
    public:
        class iterator {
        public:
            iterator(const uint64_t* words, size_t numberOfWords)
                : m_words(words)
                , m_numberOfWords(numberOfWords)
            {
                if (m_numberOfWords)
                    m_word = m_words[0];
                skip();
            }

            IndexType operator*() const { return m_wordIndex * 64 + std::countr_zero(m_word); }

            iterator& operator++()
            {
                m_word &= m_word - 1;
                skip();
                return *this;
            }

            bool operator==(std::default_sentinel_t) const { return m_wordIndex == m_numberOfWords; }

        private:
            void skip()
            {
                while (!m_word && m_wordIndex < m_numberOfWords) {
                    if (++m_wordIndex < m_numberOfWords)
                        m_word = m_words[m_wordIndex];
                }
            }

            const uint64_t* m_words;
            size_t m_numberOfWords;
            size_t m_wordIndex { 0 };
            uint64_t m_word { 0 };
        };

        Iterable(const uint64_t* words, size_t numberOfWords)
            : m_words(words)
            , m_numberOfWords(numberOfWords)
        {
        }

        iterator begin() const { return iterator(m_words, m_numberOfWords); }
        std::default_sentinel_t end() const { return { }; }

    private:
        const uint64_t* m_words;
        size_t m_numberOfWords;
    };

    Iterable operator[](IndexType u) { return Iterable(row(u), m_wordsPerRow); }

private:
    uint64_t* row(IndexType u) { return m_rows.mutableSpan().data() + static_cast<size_t>(u) * m_wordsPerRow; }

    void addAllWordsBut(IndexType u, std::span<const uint64_t> others)
    {
        ASSERT(others.size() == m_wordsPerRow);
        uint64_t* uRow = row(u);
        uint64_t* uWord = m_rows.mutableSpan().data() + u / 64;
        const uint64_t uBit = static_cast<uint64_t>(1) << (u % 64);
        if (m_mirrorsEdgesAtOnce) {
            for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex)
                uRow[wordIndex] |= others[wordIndex];
            uRow[u / 64] &= ~uBit;
            return;
        }
        for (size_t wordIndex = 0; wordIndex < m_wordsPerRow; ++wordIndex) {
            uint64_t word = others[wordIndex];
            if (wordIndex == u / 64)
                word &= ~uBit;
            uint64_t newOnes = word & ~uRow[wordIndex];
            uRow[wordIndex] |= word;
            for (; newOnes; newOnes &= newOnes - 1)
                uWord[(wordIndex * 64 + std::countr_zero(newOnes)) * m_wordsPerRow] |= uBit;
        }
    }

    static void transpose(std::array<uint64_t, 64>& tile)
    {
        uint64_t mask = 0x00000000ffffffffULL;
        for (unsigned distance = 32; distance; distance >>= 1, mask ^= mask << distance) {
            for (unsigned i = 0; i < 64; i = (i + distance + 1) & ~distance) {
                uint64_t swapped = ((tile[i] >> distance) ^ tile[i + distance]) & mask;
                tile[i] ^= swapped << distance;
                tile[i + distance] ^= swapped;
            }
        }
    }

    Vector<uint64_t> m_rows;
    size_t m_wordsPerRow { 0 };
    unsigned m_numElements { 0 };
    bool m_mirrorsEdgesAtOnce { false };
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

template <typename T, typename IT>
class InterferenceVector {
public:
    using IndexType = IT;
    bool contains(IndexType u, IndexType v)
    {
        return m_vector[u].contains(v);
    }

    bool addAndReturnIsNewEntry(IndexType u, IndexType v)
    {
        bool isNewEntry = m_vector[u].add(v).isNewEntry;
        m_size += isNewEntry;
        return isNewEntry;
    }

    void add(IndexType u, IndexType v)
    {
        (void) addAndReturnIsNewEntry(u, v);
    }

    void clear()
    {
        m_vector.clear();
        m_size = 0;
    }

    void mayClear(IndexType u) { m_vector[u].clear(); }

    void setMaxIndex(unsigned n)
    {
        ASSERT(n < std::numeric_limits<IndexType>::max());
        m_vector.resize(n);
    }

    template <typename Functor>
    void forEach(NOESCAPE const Functor& functor)
    {
        for (unsigned i = 0; i < m_vector.size() ; ++i) {
            for (IndexType j : m_vector[i])
                functor({ i, j });
        }
    }

    unsigned size() const { return m_size; }

    unsigned memoryUse() const
    {
        unsigned memory = 0;
        for (const T& set : m_vector)
            memory += set.memoryUse();
        return memory;
    }

    void dumpMemoryUseInKB() const { dataLog(memoryUse() / 1024); }

    struct Iterable {
        using iterator = typename T::iterator;
        iterator begin() const { return m_set.begin(); }
        iterator end() const { return m_set.end(); }
        const T& m_set;
    };

    Iterable operator[](IndexType u) const { return { m_vector[u] }; }

private:
    Vector<T> m_vector;
    unsigned m_size {0};
};

template<typename T>
class UndirectedEdgesDuplicatingAdapter {
public:
    using IndexType = typename T::IndexType;
    using Iterable = typename T::Iterable;
    bool contains(IndexType u, IndexType v) { return m_underlying.contains(u, v); }

    bool addAndReturnIsNewEntry(IndexType u, IndexType v)
    {
        bool isNewEntry1 = m_underlying.addAndReturnIsNewEntry(u, v);
        bool isNewEntry2 = m_underlying.addAndReturnIsNewEntry(v, u);
        ASSERT_UNUSED(isNewEntry2, isNewEntry1 == isNewEntry2);
        return isNewEntry1;
    }

    void add(IndexType u, IndexType v)
    {
        m_underlying.add(u, v);
        m_underlying.add(v, u);
    }

    template<typename LiveSet>
    void addAllBut(IndexType u, const LiveSet& others)
    {
        others.forEachSetBit([&](unsigned v) {
            if (v != u)
                add(u, static_cast<IndexType>(v));
        });
    }

    template<typename LiveSet>
    void addAllBut(IndexType u, IndexType alsoNot, const LiveSet& others)
    {
        others.forEachSetBit([&](unsigned v) {
            if (v != u && v != alsoNot)
                add(u, static_cast<IndexType>(v));
        });
    }

    void mirrorEdges() { }

    size_t countInterferingIn(IndexType u, std::span<const uint64_t> others)
    {
        size_t result = 0;
        for (IndexType v : m_underlying[u])
            result += others[v / 64] >> (v % 64) & 1;
        return result;
    }

    void forEachInterferingIn(IndexType u, std::span<const uint64_t> others, NOESCAPE const Invocable<void(unsigned)> auto& func)
    {
        for (IndexType v : m_underlying[u]) {
            if (others[v / 64] >> (v % 64) & 1)
                func(v);
        }
    }

    void forEachNonInterferingIn(IndexType u, std::span<const uint64_t> others, NOESCAPE const Invocable<void(unsigned)> auto& func)
    {
        WTF::forEachSetBit(others, [&](size_t v) {
            if (!contains(u, static_cast<IndexType>(v)))
                func(static_cast<unsigned>(v));
        });
    }

    void clear() { m_underlying.clear(); }
    void mayClear(IndexType u) { m_underlying.mayClear(u); }
    void setMaxIndex(unsigned n) { m_underlying.setMaxIndex(n); }
    template <typename Functor>
    void forEach(NOESCAPE const Functor& functor) { m_underlying.forEach(functor); }
    unsigned size() const { return m_underlying.size(); }
    unsigned memoryUse() const { return m_underlying.memoryUse(); }
    void dumpMemoryUseInKB() const { m_underlying.dumpMemoryUseInKB(); }
    Iterable operator[](IndexType u) const { return m_underlying[u]; }

private:
    T m_underlying;
};

// This is generally better than the Duplicating version, there are two reasons not to use it though:
// - For BitVector, it has no effect on memory use, and adds a branch to every call to contains/addAndReturnIsNewEntry
// - It makes it impossible to iterate over the indices that interfere with a particular index. This can be required for some uses (see AirAllocateStackByGraphColoring)
template<typename T>
class UndirectedEdgesDedupAdapter {
public:
    using IndexType = typename T::IndexType;
    bool contains(IndexType u, IndexType v)
    {
        if (v < u)
            std::swap(u, v);
        return m_underlying.contains(u, v);
    }

    bool addAndReturnIsNewEntry(IndexType u, IndexType v)
    {
        if (v < u)
            std::swap(u, v);
        return m_underlying.addAndReturnIsNewEntry(u, v);
    }

    void add(IndexType u, IndexType v)
    {
        if (v < u)
            std::swap(u, v);
        m_underlying.add(u, v);
    }

    void clear() { m_underlying.clear(); }
    void mayClear(IndexType u) { m_underlying.mayClear(u); }
    void setMaxIndex(unsigned n) { m_underlying.setMaxIndex(n); }
    template <typename Functor>
    void forEach(NOESCAPE const Functor& functor) { m_underlying.forEach(functor); }
    unsigned size() const { return m_underlying.size(); }
    unsigned memoryUse() const { return m_underlying.memoryUse(); }
    void dumpMemoryUseInKB() const { m_underlying.dumpMemoryUseInKB(); }

private:
    T m_underlying;
};

using DefaultDenseInterferenceGraph = InterferenceRows;
using DefaultDenseIterableInterferenceGraph = DefaultDenseInterferenceGraph;
using DefaultLargeInterferenceGraph = UndirectedEdgesDedupAdapter<InterferenceVector<LikelyDenseUnsignedIntegerSet<uint16_t>, uint16_t>>;
using DefaultHugeInterferenceGraph = UndirectedEdgesDedupAdapter<InterferenceVector<LikelyDenseUnsignedIntegerSet<uint32_t>, uint32_t>>;
using DefaultLargeIterableInterferenceGraph = UndirectedEdgesDuplicatingAdapter<InterferenceVector<LikelyDenseUnsignedIntegerSet<uint16_t>, uint16_t>>;
using DefaultHugeIterableInterferenceGraph = UndirectedEdgesDuplicatingAdapter<InterferenceVector<LikelyDenseUnsignedIntegerSet<uint32_t>, uint32_t>>;

// Set to one to run a reference implementation in parallel with the optimized datastructure, verifying that they agree on every method call.
// Beware: this is naturally much slower and memory hungry
#define TEST_OPTIMIZED_INTERFERENCE_GRAPH 0
#if !TEST_OPTIMIZED_INTERFERENCE_GRAPH

using DenseInterferenceGraph = DefaultDenseInterferenceGraph;
using LargeInterferenceGraph = DefaultLargeInterferenceGraph;
using HugeInterferenceGraph = DefaultHugeInterferenceGraph;

using DenseIterableInterferenceGraph = DefaultDenseIterableInterferenceGraph;
using LargeIterableInterferenceGraph = DefaultLargeIterableInterferenceGraph;
using HugeIterableInterferenceGraph = DefaultHugeIterableInterferenceGraph;

#else // TEST_OPTIMIZED_INTERFERENCE_GRAPH

// This is an unoptimized reference implementation, for use with InstrumentedInterferenceGraph below
template <typename T>
class InterferenceHashSet {
public:
    using IndexType = T;
    using InterferenceEdge = std::pair<IndexType, IndexType>;
    bool contains(IndexType u, IndexType v)
    {
        return m_set.contains({ u, v });
    }

    bool addAndReturnIsNewEntry(IndexType u, IndexType v)
    {
        return m_set.add({ u, v }).isNewEntry;
    }

    void add(IndexType u, IndexType v)
    {
        m_set.add({ u, v });
    }

    void clear()
    {
        m_set.clear();
    }

    void setMaxIndex(unsigned n)
    {
        ASSERT_UNUSED(n, n < std::numeric_limits<IndexType>::max());
    }

    template <typename Functor>
    void forEach(NOESCAPE const Functor& functor)
    {
        for (auto edge : m_set)
            functor(edge);
    }

    unsigned size() const { return m_set.size(); }

    unsigned memoryUse() const { return m_set.capacity() * sizeof(InterferenceEdge); }

    void dumpMemoryUseInKB() const { dataLog(memoryUse() / 1024); }

private:
    UncheckedKeyHashSet<InterferenceEdge> m_set;
};

template <typename T, typename U>
class InstrumentedInterferenceGraph {
public:
    using IndexType = typename T::IndexType;
    static_assert(std::is_same<IndexType, typename U::IndexType>::value);
    bool contains(IndexType u, IndexType v)
    {
        bool containsInA = m_setA.contains(u, v);
        bool containsInB = m_setB.contains(u, v);
        RELEASE_ASSERT(containsInA == containsInB);
        return containsInA;
    }

    bool addAndReturnIsNewEntry(IndexType u, IndexType v)
    {
        bool isNewEntryA = m_setA.addAndReturnIsNewEntry(u, v);
        bool isNewEntryB = m_setB.addAndReturnIsNewEntry(u, v);
        RELEASE_ASSERT(isNewEntryA == isNewEntryB);
        return isNewEntryA;
    }

    void add(IndexType u, IndexType v)
    {
        m_setA.add(u, v);
        m_setB.add(u, v);
    }

    void clear()
    {
        m_setA.clear();
        m_setB.clear();
    }

    void mayClear(IndexType u)
    {
        m_setA.mayClear(u);
        m_setB.mayClear(u);
    }

    void setMaxIndex(unsigned n)
    {
        ASSERT(n < std::numeric_limits<IndexType>::max());
        m_setA.setMaxIndex(n);
        m_setB.setMaxIndex(n);
    }

    template <typename Functor>
    void forEach(NOESCAPE const Functor& functor)
    {
        m_setA.forEach(functor);
    }

    unsigned size() const
    {
        unsigned sizeA = m_setA.size();
        unsigned sizeB = m_setB.size();
        RELEASE_ASSERT(sizeA == sizeB);
        return sizeA;
    }

    unsigned memoryUse() const
    {
        return m_setA.memoryUse() + m_setB.memoryUse();
    }

    void dumpMemoryUseInKB() const
    {
        dataLog(m_setA.memoryUse() / 1024, " -> ", m_setB.memoryUse() / 1024);
    }

protected:
    T m_setA;
    U m_setB;
};

template <typename T, typename U>
class InstrumentedIterableInterferenceGraph : public InstrumentedInterferenceGraph<T, U> {
public:
    using Base = InstrumentedInterferenceGraph<T, U>;
    using IndexType = typename Base::IndexType;

    struct Iterable {
        Iterable(typename T::Iterable iterableA, typename U::Iterable iterableB)
        {
            for (IndexType value : iterableA)
                m_values.append(value);
            Vector<IndexType> valuesB;
            for (IndexType value : iterableB)
                valuesB.append(value);
            RELEASE_ASSERT(m_values.size() == valuesB.size());
            std::ranges::sort(m_values);
            std::ranges::sort(valuesB));
            for (unsigned i = 0; i < m_values.size(); ++i)
                RELEASE_ASSERT(m_values[i] == valuesB[i]);
        }
        using iterator = typename Vector<IndexType>::const_iterator;

        iterator begin() const
        {
            return m_values.begin();
        }
        iterator end() const
        {
            return m_values.end();
        }

        Vector<IndexType> m_values;
    };

    Iterable operator[](IndexType u) const
    {
        return { Base::m_setA[u], Base::m_setB[u] };
    }
};

using ReferenceInterferenceGraph = UndirectedEdgesDedupAdapter<InterferenceHashSet<uint16_t>>;
using HugeReferenceInterferenceGraph = UndirectedEdgesDedupAdapter<InterferenceHashSet<uint32_t>>;

using DenseInterferenceGraph = InstrumentedInterferenceGraph<ReferenceInterferenceGraph, DefaultDenseInterferenceGraph>;
using LargeInterferenceGraph = InstrumentedInterferenceGraph<ReferenceInterferenceGraph, DefaultLargeInterferenceGraph>;
using HugeInterferenceGraph = InstrumentedInterferenceGraph<HugeReferenceInterferenceGraph, DefaultHugeInterferenceGraph>;

using ReferenceIterableInterferenceGraph = UndirectedEdgesDuplicatingAdapter<InterferenceVector<UncheckedKeyHashSet<uint16_t, IntHash<uint16_t>, UnsignedWithZeroKeyHashTraits<uint16_t>>, uint16_t>>;
using HugeReferenceIterableInterferenceGraph = UndirectedEdgesDuplicatingAdapter<InterferenceVector<UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, UnsignedWithZeroKeyHashTraits<uint32_t>>, uint32_t>>;

using DenseIterableInterferenceGraph = InstrumentedIterableInterferenceGraph<ReferenceIterableInterferenceGraph, DefaultDenseIterableInterferenceGraph>;
using LargeIterableInterferenceGraph = InstrumentedIterableInterferenceGraph<ReferenceIterableInterferenceGraph, DefaultLargeIterableInterferenceGraph>;
using HugeIterableInterferenceGraph = InstrumentedIterableInterferenceGraph<HugeReferenceIterableInterferenceGraph, DefaultHugeIterableInterferenceGraph>;

#endif // TEST_OPTIMIZED_INTERFERENCE_GRAPH

} // namespace WTF

using WTF::DenseInterferenceGraph;
using WTF::LargeInterferenceGraph;
using WTF::HugeInterferenceGraph;
using WTF::DenseIterableInterferenceGraph;
using WTF::LargeIterableInterferenceGraph;
using WTF::HugeIterableInterferenceGraph;
