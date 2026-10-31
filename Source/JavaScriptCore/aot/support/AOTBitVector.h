/*
 * Copyright (C) 2011-2023 Apple Inc. All rights reserved.
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

#include <stdio.h>
#include <wtf/Assertions.h>
#include <wtf/DataLog.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashTraits.h>
#include <wtf/PrintStream.h>
#include <wtf/StdLibExtras.h>

#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>

#if ENABLE(AOT)

// wtf/BitVector.h for the compiler's own sets. WTF's keeps 63 bits or a shifted pointer in one word, and every access first finds out which.
// This one is three words: where the words are (its own third word while 64 bits are enough), how many bits, and that word.
// It points into itself, so it does not move as bytes.

namespace JSC { namespace AOT {

using WTF::findBitInWord;

class BitVector final {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BitVector);
public: 
    BitVector()
        : m_words(&m_bitsOrPointer)
        , m_numBits(bitsInPointer())
        , m_bitsOrPointer(0)
    {
    }
    
    explicit BitVector(size_t numBits)
        : BitVector()
    {
        ensureSize(numBits);
    }
    
    BitVector(const BitVector& other)
        : BitVector()
    {
        (*this) = other;
    }

    BitVector(BitVector&& other)
    {
        adopt(other);
    }

    
    ~BitVector()
    {
        if (isInline())
            return;
        fastFree(m_words);
    }
    
    BitVector& operator=(const BitVector& other)
    {
        if (&other == this) [[unlikely]]
            return *this;

        if (isInline() && other.isInline())
            m_bitsOrPointer = other.m_bitsOrPointer;
        else
            setSlow(other);
        return *this;
    }

    BitVector& operator=(BitVector&& other)
    {
        if (&other == this) [[unlikely]]
            return *this;

        if (!isInline() && !isEmptyOrDeletedValue())
            fastFree(m_words);

        adopt(other);
        return *this;
    }

    size_t size() const
    {
        return m_numBits;
    }

    void ensureSize(size_t numBits)
    {
        if (numBits <= size())
            return;
        resizeOutOfLine(numBits);
    }
    
    // Like ensureSize(), but supports reducing the size of the bitvector.
    void resize(size_t numBits);
    
    void clearAll();

    bool quickGet(size_t bit) const
    {
        if (bit >= m_numBits) [[unlikely]]
            CRASH();
        return !!(m_words[bit / bitsInPointer()] & (static_cast<uintptr_t>(1) << (bit & (bitsInPointer() - 1))));
    }
    
    bool quickSet(size_t bit)
    {
        if (bit >= m_numBits) [[unlikely]]
            CRASH();
        uintptr_t& word = m_words[bit / bitsInPointer()];
        uintptr_t mask = static_cast<uintptr_t>(1) << (bit & (bitsInPointer() - 1));
        bool result = !!(word & mask);
        word |= mask;
        return result;
    }
    
    bool quickClear(size_t bit)
    {
        if (bit >= m_numBits) [[unlikely]]
            CRASH();
        uintptr_t& word = m_words[bit / bitsInPointer()];
        uintptr_t mask = static_cast<uintptr_t>(1) << (bit & (bitsInPointer() - 1));
        bool result = !!(word & mask);
        word &= ~mask;
        return result;
    }
    
    bool quickSet(size_t bit, bool value)
    {
        if (value)
            return quickSet(bit);
        return quickClear(bit);
    }
    
    bool get(size_t bit) const
    {
        if (bit >= size())
            return false;
        return quickGet(bit);
    }

    bool contains(size_t bit) const
    {
        return get(bit);
    }
    
    bool set(size_t bit)
    {
        ensureSize(bit + 1);
        return quickSet(bit);
    }

    // This works like the add methods of sets. Instead of returning the previous value, like set(),
    // it returns whether the bit transitioned from false to true.
    bool add(size_t bit)
    {
        return !set(bit);
    }

    bool ensureSizeAndSet(size_t bit, size_t size)
    {
        ensureSize(size);
        return quickSet(bit);
    }

    bool clear(size_t bit)
    {
        if (bit >= size())
            return false;
        return quickClear(bit);
    }

    bool remove(size_t bit)
    {
        return clear(bit);
    }
    
    bool set(size_t bit, bool value)
    {
        if (value)
            return set(bit);
        return clear(bit);
    }
    
    void merge(const BitVector& other)
    {
        if (!isInline() || !other.isInline()) {
            mergeSlow(other);
            return;
        }
        m_bitsOrPointer |= other.m_bitsOrPointer;
        ASSERT(isInline());
    }
    
    void filter(const BitVector& other)
    {
        if (!isInline() || !other.isInline()) {
            filterSlow(other);
            return;
        }
        m_bitsOrPointer &= other.m_bitsOrPointer;
        ASSERT(isInline());
    }

    void exclude(const BitVector& other)
    {
        if (!isInline() || !other.isInline()) {
            excludeSlow(other);
            return;
        }
        m_bitsOrPointer &= ~other.m_bitsOrPointer;
        ASSERT(isInline());
    }
    
    size_t bitCount() const
    {
        if (isInline())
            return std::popcount(cleanseInlineBits(m_bitsOrPointer));
        return bitCountSlow();
    }

    bool isEmpty() const
    {
        if (isInline())
            return !cleanseInlineBits(m_bitsOrPointer);
        return isEmptySlow();
    }
    
    size_t findBit(size_t index, bool value) const
    {
        size_t result = findBitFast(index, value);
        if (ASSERT_ENABLED) {
            size_t expectedResult = findBitSimple(index, value);
            if (result != expectedResult) {
                dataLog("findBit(", index, ", ", value, ") on ", *this, " should have gotten ", expectedResult, " but got ", result, "\n");
                ASSERT_NOT_REACHED();
            }
        }
        return result;
    }

    // If the lambda returns an IterationStatus, we use it. The lambda can also return
    // void, in which case, we'll iterate every set bit.
    template<typename Func>
    constexpr ALWAYS_INLINE void forEachSetBit(NOESCAPE const Func&) const;

    template<typename Func>
    constexpr ALWAYS_INLINE void forEachSetBit(size_t startIndex, NOESCAPE const Func&) const;
    
    void dump(PrintStream& out) const;
    
    enum EmptyValueTag { EmptyValue };
    enum DeletedValueTag { DeletedValue };
    
    BitVector(EmptyValueTag)
        : m_words(nullptr)
        , m_numBits(0)
        , m_bitsOrPointer(0)
    {
    }
    
    BitVector(DeletedValueTag)
        : m_words(reinterpret_cast<uintptr_t*>(1))
        , m_numBits(0)
        , m_bitsOrPointer(0)
    {
    }
    
    bool isEmptyValue() const { return !m_words; }
    bool isDeletedValue() const { return reinterpret_cast<uintptr_t>(m_words) == 1; }
    
    bool isEmptyOrDeletedValue() const { return reinterpret_cast<uintptr_t>(m_words) <= 1; }
    
    bool operator==(const BitVector& other) const
    {
        if (isInline() && other.isInline())
            return m_bitsOrPointer == other.m_bitsOrPointer;
        return equalsSlowCase(other);
    }
    
    unsigned hash() const
    {
        // This is a very simple hash. Just xor together the words that hold the various
        // bits and then compute the hash. This makes it very easy to deal with bitvectors
        // that have a lot of trailing zero's.
        uintptr_t value;
        if (isInline())
            value = cleanseInlineBits(m_bitsOrPointer);
        else
            value = hashSlowCase();
        return IntHash<uintptr_t>::hash(value);
    }
    
    class iterator {
        WTF_DEPRECATED_MAKE_FAST_ALLOCATED(iterator);
    public:
        iterator() = default;
        
        iterator(const BitVector& bitVector, size_t index)
            : m_bitVector(&bitVector)
            , m_index(index)
        {
        }
        
        size_t operator*() const { return m_index; }
        
        iterator& operator++()
        {
            m_index = m_bitVector->findBit(m_index + 1, true);
            return *this;
        }

        iterator operator++(int)
        {
            iterator result = *this;
            ++(*this);
            return result;
        }

        bool isAtEnd() const
        {
            return m_index >= m_bitVector->size();
        }
        
        bool operator==(const iterator& other) const
        {
            return m_index == other.m_index;
        }
        
    private:
        const BitVector* m_bitVector { nullptr };
        size_t m_index { 0 };
    };

    // Use this to iterate over set bits.
    iterator begin() const LIFETIME_BOUND { return iterator(*this, findBit(0, true)); }
    iterator end() const LIFETIME_BOUND { return iterator(*this, size()); }

    static unsigned outOfLineMemoryUse(size_t bitCount)
    {
        if (bitCount <= maxInlineBits())
            return 0;
        return byteCount(bitCount);
    }
    unsigned outOfLineMemoryUse() const { return outOfLineMemoryUse(size()); }
        
    void shiftRightByMultipleOf64(size_t);

private:
    static constexpr unsigned bitsInPointer()
    {
        return sizeof(void*) << 3;
    }

    static constexpr unsigned maxInlineBits()
    {
        return bitsInPointer();
    }

    static constexpr size_t byteCount(size_t bitCount)
    {
        return (bitCount + 7) >> 3;
    }

    static uintptr_t makeInlineBits(uintptr_t bits)
    {
        return bits;
    }
    
    static uintptr_t cleanseInlineBits(uintptr_t bits)
    {
        return bits;
    }

    size_t findBitFast(size_t startIndex, bool value) const
    {

        // value = true: casts to 1, then xors to 0, then negates to 0.
        // value = false: casts to 0, then xors to 1, then negates to -1 (i.e. all one bits).
        uintptr_t skipValue = -(static_cast<uintptr_t>(value) ^ 1);
        
        size_t wordIndex = startIndex / bitsInPointer();
        size_t startIndexInWord = startIndex - wordIndex * bitsInPointer();
        
        const uintptr_t* words = m_words;
        size_t numWords = (m_numBits + bitsInPointer() - 1) / bitsInPointer();
        while (wordIndex < numWords) {
            uintptr_t word = words[wordIndex];
            if (word != skipValue) {
                size_t index = startIndexInWord;
                if (findBitInWord(word, index, bitsInPointer(), value))
                    return wordIndex * bitsInPointer() + index;
            }
            
            ++wordIndex;
            startIndexInWord = 0;
        }
        
        return m_numBits;
    }
    
    size_t findBitSimple(size_t index, bool value) const
    {
        while (index < size()) {
            if (get(index) == value)
                return index;
            ++index;
        }
        return size();
    }
    
    bool isInline() const { return m_words == &m_bitsOrPointer; }

    void adopt(BitVector& other)
    {
        m_bitsOrPointer = other.m_bitsOrPointer;
        m_words = other.isInline() ? &m_bitsOrPointer : other.m_words;
        m_numBits = other.m_numBits;
        other.m_words = &other.m_bitsOrPointer;
        other.m_numBits = bitsInPointer();
        other.m_bitsOrPointer = 0;
    }
    

    
    void resizeOutOfLine(size_t numBits, size_t shiftInWords = 0);
    void setSlow(const BitVector& other);
    
    void mergeSlow(const BitVector& other);
    void filterSlow(const BitVector& other);
    void excludeSlow(const BitVector& other);
    
    size_t bitCountSlow() const;
    bool isEmptySlow() const;
    
    bool equalsSlowCase(const BitVector& other) const;
    uintptr_t hashSlowCase() const;
    
    std::span<uintptr_t> words() LIFETIME_BOUND
    {
        return unsafeMakeSpan(m_words, (m_numBits + bitsInPointer() - 1) / bitsInPointer());
    }

    std::span<const uintptr_t> words() const LIFETIME_BOUND
    {
        return unsafeMakeSpan(m_words, (m_numBits + bitsInPointer() - 1) / bitsInPointer());
    }

    std::span<uint8_t> byteSpan() LIFETIME_BOUND { return asMutableByteSpan(words()).first(byteCount(size())); }
    std::span<const uint8_t> byteSpan() const LIFETIME_BOUND { return asByteSpan(words()).first(byteCount(size())); }

    uintptr_t* m_words;
    size_t m_numBits;
    uintptr_t m_bitsOrPointer; // The inline word.
};

template<typename Func>
ALWAYS_INLINE constexpr void BitVector::forEachSetBit(NOESCAPE const Func& func) const
{
    auto words = this->words();
    WTF::forEachSetBit(words, func);
}

template<typename Func>
ALWAYS_INLINE constexpr void BitVector::forEachSetBit(size_t startIndex, NOESCAPE const Func& func) const
{
    auto words = this->words();
    WTF::forEachSetBit(words, startIndex, func);
}

NEVER_INLINE inline void BitVector::setSlow(const BitVector& other)
{
    uintptr_t* newWords = nullptr;
    if (!other.isInline() && !other.isEmptyOrDeletedValue()) {
        newWords = static_cast<uintptr_t*>(fastMalloc(other.words().size_bytes()));
        memcpySpan(unsafeMakeSpan(newWords, other.words().size()), other.words());
    }
    if (!isInline() && !isEmptyOrDeletedValue())
        fastFree(m_words);
    m_bitsOrPointer = other.m_bitsOrPointer;
    m_numBits = other.m_numBits;
    if (other.isInline())
        m_words = &m_bitsOrPointer;
    else
        m_words = newWords ? newWords : other.m_words;
}

NEVER_INLINE inline void BitVector::resize(size_t numBits)
{
    if (numBits <= maxInlineBits()) {
        if (isInline())
            return;
        uintptr_t* oldWords = m_words;
        m_bitsOrPointer = oldWords[0];
        m_words = &m_bitsOrPointer;
        m_numBits = bitsInPointer();
        fastFree(oldWords);
        return;
    }
    resizeOutOfLine(numBits);
}

NEVER_INLINE inline void BitVector::clearAll()
{
    zeroSpan(words());
}

NEVER_INLINE inline void BitVector::shiftRightByMultipleOf64(size_t shiftInBits)
{
    RELEASE_ASSERT(!(shiftInBits % 64));
    resizeOutOfLine(size() + shiftInBits, shiftInBits / bitsInPointer());
}

NEVER_INLINE inline void BitVector::resizeOutOfLine(size_t numBits, size_t shiftInWords)
{
    ASSERT(numBits > maxInlineBits());
    numBits = roundUpToMultipleOf<bitsInPointer()>(numBits);
    size_t numWords = numBits / bitsInPointer();
    auto newWords = unsafeMakeSpan(static_cast<uintptr_t*>(fastMalloc(numWords * sizeof(uintptr_t))), numWords);
    auto oldWords = words();
    if (numBits > size()) {
        zeroSpan(newWords.first(shiftInWords));
        memcpySpan(newWords.subspan(shiftInWords), oldWords);
        zeroSpan(newWords.subspan(shiftInWords + oldWords.size()));
    } else
        memcpySpan(newWords, oldWords.first(numWords));
    if (!isInline())
        fastFree(m_words);
    m_words = newWords.data();
    m_numBits = numBits;
}

NEVER_INLINE inline void BitVector::mergeSlow(const BitVector& other)
{
    ensureSize(other.size());
    auto a = words();
    auto b = other.words();
    for (size_t i = 0; i < b.size(); ++i)
        a[i] |= b[i];
}

NEVER_INLINE inline void BitVector::filterSlow(const BitVector& other)
{
    auto a = words();
    auto b = other.words();
    auto commonSize = std::min(a.size(), b.size());
    for (size_t i = 0; i < commonSize; ++i)
        a[i] &= b[i];
    if (a.size() > b.size())
        zeroSpan(a.subspan(b.size()));
}

NEVER_INLINE inline void BitVector::excludeSlow(const BitVector& other)
{
    auto a = words();
    auto b = other.words();
    auto commonSize = std::min(a.size(), b.size());
    for (size_t i = 0; i < commonSize; ++i)
        a[i] &= ~b[i];
}

NEVER_INLINE inline size_t BitVector::bitCountSlow() const
{
    size_t result = 0;
    for (auto word : words())
        result += std::popcount(word);
    return result;
}

NEVER_INLINE inline bool BitVector::isEmptySlow() const
{
    for (auto word : words()) {
        if (word)
            return false;
    }
    return true;
}

NEVER_INLINE inline bool BitVector::equalsSlowCase(const BitVector& other) const
{
    auto myWords = words();
    auto otherWords = other.words();
    std::span<const uintptr_t> extraBits;
    if (myWords.size() < otherWords.size()) {
        extraBits = otherWords.subspan(myWords.size());
        otherWords = otherWords.first(myWords.size());
    } else {
        extraBits = myWords.subspan(otherWords.size());
        myWords = myWords.first(otherWords.size());
    }
    for (auto word : extraBits) {
        if (word)
            return false;
    }
    return equalSpans(myWords, otherWords);
}

NEVER_INLINE inline uintptr_t BitVector::hashSlowCase() const
{
    uintptr_t result = 0;
    for (auto word : words())
        result ^= word;
    return result;
}

NEVER_INLINE inline void BitVector::dump(PrintStream& out) const
{
    for (size_t i = 0; i < size(); ++i)
        out.print(get(i) ? "1" : "-");
}

struct BitVectorHash {
    static unsigned hash(const BitVector& vector) { return vector.hash(); }
    static bool equal(const BitVector& a, const BitVector& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = false;
};

} } // namespace JSC::AOT

namespace WTF {

template<> struct DefaultHash<JSC::AOT::BitVector> : JSC::AOT::BitVectorHash { };
template<> struct HashTraits<JSC::AOT::BitVector> : public CustomHashTraits<JSC::AOT::BitVector> { };

} // namespace WTF

#endif // ENABLE(AOT)
