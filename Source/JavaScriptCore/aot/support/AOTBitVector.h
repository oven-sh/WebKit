/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashFunctions.h>
#include <wtf/IterationStatus.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

class BitVector final {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BitVector);
public:
    BitVector() = default;
    explicit BitVector(size_t numberOfBits) { ensureSize(numberOfBits); }
    BitVector(const BitVector& other) { *this = other; }
    BitVector(BitVector&& other) { take(other); }
    ~BitVector() { release(); }

    BitVector& operator=(const BitVector& other)
    {
        if (&other == this) [[unlikely]]
            return *this;
        if (m_numberOfWords != other.m_numberOfWords) {
            release();
            m_words = &m_ownWord;
            m_numberOfWords = 1;
            if (other.m_numberOfWords > 1) {
                m_words = allocate(other.m_numberOfWords);
                m_numberOfWords = other.m_numberOfWords;
            }
        }
        std::memcpy(m_words, other.m_words, m_numberOfWords * sizeof(uint64_t));
        return *this;
    }
    BitVector& operator=(BitVector&& other)
    {
        if (&other == this) [[unlikely]]
            return *this;
        release();
        take(other);
        return *this;
    }

    size_t size() const { return m_numberOfWords * bitsInWord; }
    ALWAYS_INLINE void ensureSize(size_t numberOfBits)
    {
        if (numberOfBits > size()) [[unlikely]]
            grow(numberOfBits);
    }
    void clearAll() { std::memset(m_words, 0, m_numberOfWords * sizeof(uint64_t)); }

    ALWAYS_INLINE bool quickGet(size_t bit) const
    {
        RELEASE_ASSERT(bit / bitsInWord < m_numberOfWords);
        return m_words[bit / bitsInWord] >> (bit % bitsInWord) & 1;
    }
    ALWAYS_INLINE bool quickSet(size_t bit)
    {
        RELEASE_ASSERT(bit / bitsInWord < m_numberOfWords);
        return setIn(m_words[bit / bitsInWord], bit);
    }
    ALWAYS_INLINE bool quickClear(size_t bit)
    {
        RELEASE_ASSERT(bit / bitsInWord < m_numberOfWords);
        return clearIn(m_words[bit / bitsInWord], bit);
    }
    bool quickSet(size_t bit, bool value) { return value ? quickSet(bit) : quickClear(bit); }

    ALWAYS_INLINE bool get(size_t bit) const { return bit / bitsInWord < m_numberOfWords && (m_words[bit / bitsInWord] >> (bit % bitsInWord) & 1); }
    ALWAYS_INLINE bool contains(size_t bit) const { return get(bit); }
    ALWAYS_INLINE bool set(size_t bit)
    {
        if (bit / bitsInWord >= m_numberOfWords) [[unlikely]]
            grow(bit + 1);
        return setIn(m_words[bit / bitsInWord], bit);
    }
    ALWAYS_INLINE bool add(size_t bit) { return !set(bit); }
    bool ensureSizeAndSet(size_t bit, size_t size)
    {
        ensureSize(size);
        return quickSet(bit);
    }
    ALWAYS_INLINE bool clear(size_t bit) { return bit / bitsInWord < m_numberOfWords && clearIn(m_words[bit / bitsInWord], bit); }
    bool remove(size_t bit) { return clear(bit); }
    bool set(size_t bit, bool value) { return value ? set(bit) : clear(bit); }

    void merge(const BitVector& other)
    {
        if (other.m_numberOfWords > m_numberOfWords)
            grow(other.size());
        for (size_t index = 0; index < other.m_numberOfWords; ++index)
            m_words[index] |= other.m_words[index];
    }
    void filter(const BitVector& other)
    {
        size_t common = std::min(m_numberOfWords, other.m_numberOfWords);
        for (size_t index = 0; index < common; ++index)
            m_words[index] &= other.m_words[index];
        std::fill(m_words + common, m_words + m_numberOfWords, 0);
    }
    void exclude(const BitVector& other)
    {
        size_t common = std::min(m_numberOfWords, other.m_numberOfWords);
        for (size_t index = 0; index < common; ++index)
            m_words[index] &= ~other.m_words[index];
    }

    size_t bitCount() const
    {
        size_t count = 0;
        for (uint64_t word : words())
            count += std::popcount(word);
        return count;
    }
    bool isEmpty() const
    {
        return std::ranges::all_of(words(), [](uint64_t word) {
            return !word;
        });
    }
    size_t findBit(size_t first, bool value) const
    {
        if (first >= size())
            return size();
        uint64_t skipped = value ? 0 : ~uint64_t { 0 };
        uint64_t mask = ~uint64_t { 0 } << (first % bitsInWord);
        for (size_t index = first / bitsInWord; index < m_numberOfWords; ++index) {
            if (uint64_t found = (m_words[index] ^ skipped) & mask)
                return index * bitsInWord + static_cast<size_t>(std::countr_zero(found));
            mask = ~uint64_t { 0 };
        }
        return size();
    }

    template<typename Functor> ALWAYS_INLINE void forEachSetBit(NOESCAPE const Functor& functor) const { WTF::forEachSetBit(words(), functor); }
    template<typename Functor> ALWAYS_INLINE void forEachSetBit(size_t first, NOESCAPE const Functor& functor) const { WTF::forEachSetBit(words(), first, functor); }

    bool operator==(const BitVector& other) const
    {
        size_t common = std::min(m_numberOfWords, other.m_numberOfWords);
        auto isZero = [](uint64_t word) {
            return !word;
        };
        return std::equal(m_words, m_words + common, other.m_words) && std::ranges::all_of(words().subspan(common), isZero) && std::ranges::all_of(other.words().subspan(common), isZero);
    }

    class iterator {
    public:
        iterator() = default;
        iterator(const BitVector& bits, size_t index)
            : m_bits(&bits)
            , m_index(index)
        {
        }

        size_t operator*() const { return m_index; }
        iterator& operator++()
        {
            m_index = m_bits->findBit(m_index + 1, true);
            return *this;
        }
        bool isAtEnd() const { return m_index >= m_bits->size(); }
        bool operator==(const iterator& other) const { return m_index == other.m_index; }

    private:
        const BitVector* m_bits { nullptr };
        size_t m_index { 0 };
    };
    iterator begin() const LIFETIME_BOUND { return iterator(*this, findBit(0, true)); }
    iterator end() const LIFETIME_BOUND { return iterator(*this, size()); }

    size_t outOfLineMemoryUse() const { return m_words == &m_ownWord ? 0 : m_numberOfWords * sizeof(uint64_t); }

private:
    static constexpr size_t bitsInWord = 64;

    std::span<const uint64_t> words() const LIFETIME_BOUND { return { m_words, m_numberOfWords }; }

    static ALWAYS_INLINE bool setIn(uint64_t& word, size_t bit)
    {
        uint64_t mask = uint64_t { 1 } << (bit % bitsInWord);
        bool wasSet = word & mask;
        word |= mask;
        return wasSet;
    }
    static ALWAYS_INLINE bool clearIn(uint64_t& word, size_t bit)
    {
        uint64_t mask = uint64_t { 1 } << (bit % bitsInWord);
        bool wasSet = word & mask;
        word &= ~mask;
        return wasSet;
    }
    static uint64_t* allocate(size_t numberOfWords) { return static_cast<uint64_t*>(FastMalloc::malloc(numberOfWords * sizeof(uint64_t))); }

    NEVER_INLINE void grow(size_t numberOfBits)
    {
        size_t numberOfWords = (numberOfBits + bitsInWord - 1) / bitsInWord;
        uint64_t* words = allocate(numberOfWords);
        std::memcpy(words, m_words, m_numberOfWords * sizeof(uint64_t));
        std::memset(words + m_numberOfWords, 0, (numberOfWords - m_numberOfWords) * sizeof(uint64_t));
        release();
        m_words = words;
        m_numberOfWords = numberOfWords;
    }
    ALWAYS_INLINE void release()
    {
        if (m_words != &m_ownWord)
            FastMalloc::free(m_words);
    }
    ALWAYS_INLINE void take(BitVector& other)
    {
        bool isOwnWord = other.m_words == &other.m_ownWord;
        m_words = isOwnWord ? &m_ownWord : other.m_words;
        m_ownWord = other.m_ownWord;
        m_numberOfWords = other.m_numberOfWords;
        other.m_words = &other.m_ownWord;
        other.m_numberOfWords = 1;
        other.m_ownWord = 0;
    }

    uint64_t* m_words { &m_ownWord };
    size_t m_numberOfWords { 1 };
    uint64_t m_ownWord { 0 };
};

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
