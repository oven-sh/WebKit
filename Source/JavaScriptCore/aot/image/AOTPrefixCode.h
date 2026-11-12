/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include "JSExportMacros.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <wtf/FlipBytes.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

class PrefixCode {
public:
    static constexpr unsigned maxLength = 15;
    static constexpr unsigned endOfStream = 256;
    static constexpr unsigned bitsOfShortWord = 7;
    static constexpr size_t sizeOfShortWords(unsigned bits) { return sizeof(uint16_t) << bits; }
    static constexpr size_t maxSize(unsigned bits) { return sizeOfShortWords(bits) + sizeof(LongWords) + maxLength + endOfStream; }
    using Counts = std::array<uint32_t, endOfStream>;

    struct LongWords {
        uint16_t firstWord;
        uint16_t shortWords;
    };
    static_assert(sizeof(LongWords) == 4);

    struct Word {
        uint16_t bits { 0 };
        uint8_t length { 0 };
    };

    explicit PrefixCode(const Counts&);

    const Word& word(uint8_t byte) const { return m_words[byte]; }
    unsigned shortCodeBits(unsigned most) const;
    void append(Vector<uint8_t>&, unsigned shortCodeBits) const;

private:
    using Lengths = std::array<uint8_t, endOfStream + 1>;

    static Lengths findLengths(const Counts&);

    std::array<Word, endOfStream> m_words { };
    std::array<uint8_t, maxLength> m_wordsOfLength { };
    Vector<uint8_t> m_bytes;
};

class PrefixCodeWriter {
public:
    explicit PrefixCodeWriter(uint8_t* firstReservedByte)
        : m_end(firstReservedByte)
    {
    }

    ALWAYS_INLINE void append(PrefixCode::Word word)
    {
        ASSERT(word.length);
        m_bits = m_bits << word.length | word.bits;
        m_numberOfBits += word.length;
        if (m_numberOfBits < 32)
            return;
        m_numberOfBits -= 32;
        uint32_t bytes = flipBytes(static_cast<uint32_t>(m_bits >> m_numberOfBits));
        memcpy(m_end, &bytes, sizeof(bytes));
        m_end += sizeof(bytes);
    }

    ALWAYS_INLINE uint8_t* finish()
    {
        if (unsigned numberOfOnes = -m_numberOfBits % 8)
            append({ static_cast<uint16_t>((1u << numberOfOnes) - 1), static_cast<uint8_t>(numberOfOnes) });
        while (m_numberOfBits) {
            m_numberOfBits -= 8;
            *m_end++ = static_cast<uint8_t>(m_bits >> m_numberOfBits);
        }
        return m_end;
    }

private:
    uint8_t* m_end;
    uint64_t m_bits { 0 };
    unsigned m_numberOfBits { 0 };
};

using PrefixSymbol = uint16_t;

class PrefixSymbolWriter {
public:
    explicit PrefixSymbolWriter(PrefixSymbol* firstReservedSymbol)
        : m_end(firstReservedSymbol)
    {
    }

    PrefixSymbol* end() const { return m_end; }
    void append(std::span<const PrefixSymbol> symbols)
    {
        if (!symbols.empty())
            memcpy(m_end, symbols.data(), symbols.size_bytes());
        m_end += symbols.size();
    }
    void append(unsigned code, uint8_t byte) { *m_end++ = static_cast<PrefixSymbol>(code << 8 | byte); }
    void appendVarint(unsigned codeOfFirst, unsigned codeOfOthers, uint64_t value)
    {
        unsigned code = codeOfFirst;
        for (; value >= 0x80; value >>= 7) {
            append(code, static_cast<uint8_t>(value) | 0x80);
            code = codeOfOthers;
        }
        append(code, static_cast<uint8_t>(value));
    }

private:
    PrefixSymbol* m_end;
};

struct ShortWordsOfOneSize {
    static constexpr unsigned bits(unsigned) { return PrefixCode::bitsOfShortWord; }
};

struct PrefixCodeView {
    static constexpr unsigned bitsOfShift = 6;

    PrefixCodeView(const uint8_t* codes, uint32_t place)
        : shortWords(codes + (place >> bitsOfShift))
        , shift(place & ((1u << bitsOfShift) - 1))
    {
    }

    const uint8_t* endOfShortWords() const { return shortWords + (sizeof(uint16_t) << (64 - shift)); }

    const uint8_t* shortWords;
    unsigned shift;
};

template<unsigned numberOfCodes, typename ShortWords = ShortWordsOfOneSize>
class PrefixCodes {
    static constexpr size_t maxSizeOfCodes()
    {
        size_t size = 0;
        for (unsigned code = 0; code < numberOfCodes; ++code)
            size += PrefixCode::maxSize(ShortWords::bits(code));
        return size;
    }

public:
    using Counts = std::array<uint32_t, numberOfCodes * PrefixCode::endOfStream>;
    static constexpr size_t sizeOfPlaces = (numberOfCodes + 1) * sizeof(uint32_t);
    static constexpr size_t maxSize = sizeOfPlaces + maxSizeOfCodes();
    static_assert(maxSize < (1u << (32 - PrefixCodeView::bitsOfShift)));

    explicit PrefixCodes(const Counts& counts)
    {
        m_bytes.grow(sizeOfPlaces);
        for (unsigned number = 0; number < numberOfCodes; ++number) {
            unsigned first = number * PrefixCode::endOfStream;
            PrefixCode::Counts countsOfCode;
            std::copy_n(counts.begin() + first, countsOfCode.size(), countsOfCode.begin());
            PrefixCode code(countsOfCode);
            for (unsigned byte = 0; byte < PrefixCode::endOfStream; ++byte)
                m_words[first + byte] = code.word(static_cast<uint8_t>(byte));
            unsigned shortCodeBits = code.shortCodeBits(ShortWords::bits(number));
            writePlace(number, shortCodeBits);
            code.append(m_bytes, shortCodeBits);
        }
        writePlace(numberOfCodes, 1);
    }

    static void count(Counts& counts, std::span<const PrefixSymbol> symbols)
    {
        uint32_t* countOf = counts.data();
        for (PrefixSymbol symbol : symbols)
            ++countOf[symbol];
    }
    static void add(Counts& sums, const Counts& counts)
    {
        for (size_t symbol = 0; symbol < counts.size(); ++symbol)
            sums[symbol] += counts[symbol];
    }

    uint8_t* write(uint8_t* firstReservedByte, std::span<const PrefixSymbol> symbols, std::span<const PrefixSymbol> laterSymbols = { }) const
    {
        PrefixCodeWriter writer(firstReservedByte);
        const PrefixCode::Word* words = m_words.data();
        for (PrefixSymbol symbol : symbols)
            writer.append(words[symbol]);
        for (PrefixSymbol symbol : laterSymbols)
            writer.append(words[symbol]);
        return writer.finish();
    }

    const Vector<uint8_t>& bytes() const { return m_bytes; }

    static PrefixCodeView code(const uint8_t* bytes, unsigned number)
    {
        uint32_t place;
        memcpy(&place, bytes + number * sizeof(place), sizeof(place));
        return { bytes, place };
    }
    static const uint8_t* endOfCodes(const uint8_t* bytes) { return code(bytes, numberOfCodes).shortWords; }

private:
    void writePlace(unsigned number, unsigned shortCodeBits)
    {
        uint32_t place = static_cast<uint32_t>(m_bytes.size()) << PrefixCodeView::bitsOfShift | (64 - shortCodeBits);
        memcpy(m_bytes.mutableSpan().data() + number * sizeof(place), &place, sizeof(place));
    }

    std::array<PrefixCode::Word, numberOfCodes * PrefixCode::endOfStream> m_words;
    Vector<uint8_t> m_bytes;
};

Vector<uint8_t> encodeVarintBytes(std::span<const uint8_t>);
JS_EXPORT_PRIVATE Vector<uint8_t> decodeVarintBytes(std::span<const uint8_t>);

class PrefixCodeReader {
public:
    PrefixCodeReader(const uint8_t* begin, const uint8_t* end)
        : m_cursor(begin)
        , m_end(end)
    {
    }

    ALWAYS_INLINE unsigned read(PrefixCodeView code)
    {
        if (m_numberOfBits < PrefixCode::maxLength)
            fill();
        uint16_t shortWord;
        memcpy(&shortWord, code.shortWords + sizeof(shortWord) * (m_bits >> code.shift), sizeof(shortWord));
        unsigned length = shortWord >> 8;
        unsigned byte = shortWord & 0xff;
        if (!length) [[unlikely]] {
            LongWord word = findLongWord(code.endOfShortWords(), 64 - code.shift, static_cast<unsigned>(m_bits >> (64 - PrefixCode::maxLength)));
            length = word.length;
            byte = word.byte;
        }
        m_bits <<= length;
        m_numberOfBits -= length;
        return byte;
    }

    const uint8_t* alignToByte()
    {
        unsigned numberOfBitsDropped = m_numberOfBits % 8;
        m_bits <<= numberOfBitsDropped;
        m_numberOfBits -= numberOfBitsDropped;
        return m_cursor - m_numberOfBits / 8;
    }

private:
    struct LongWord {
        unsigned length;
        unsigned byte;
    };

    ALWAYS_INLINE void fill()
    {
        uint32_t next;
        if (m_end - m_cursor >= static_cast<ptrdiff_t>(sizeof(next))) [[likely]] {
            memcpy(&next, m_cursor, sizeof(next));
            next = flipBytes(next);
            m_cursor += sizeof(next);
        } else {
            next = lastBytes(m_cursor, m_end);
            m_cursor = m_end;
        }
        m_bits |= static_cast<uint64_t>(next) << (32 - m_numberOfBits);
        m_numberOfBits += 32;
    }

    NEVER_INLINE static uint32_t lastBytes(const uint8_t* cursor, const uint8_t* end)
    {
        uint32_t next = UINT32_MAX;
        for (unsigned shift = 24; cursor < end; shift -= 8)
            next ^= static_cast<uint32_t>(static_cast<uint8_t>(~*cursor++)) << shift;
        return next;
    }

    NEVER_INLINE static LongWord findLongWord(const uint8_t* endOfShortWords, unsigned shortCodeBits, unsigned nextBits)
    {
        PrefixCode::LongWords longWords;
        memcpy(&longWords, endOfShortWords, sizeof(longWords));
        const uint8_t* numbersOfWords = endOfShortWords + sizeof(longWords);
        const uint8_t* bytes = numbersOfWords + PrefixCode::maxLength + longWords.shortWords;
        unsigned firstWord = longWords.firstWord;
        for (unsigned length = shortCodeBits + 1; length <= PrefixCode::maxLength; ++length) {
            unsigned word = nextBits >> (PrefixCode::maxLength - length);
            unsigned number = numbersOfWords[length - 1];
            if (word - firstWord < number)
                return { length, bytes[word - firstWord] };
            bytes += number;
            firstWord = (firstWord + number) << 1;
        }
        return { 0, PrefixCode::endOfStream };
    }

    const uint8_t* m_cursor;
    const uint8_t* m_end;
    uint64_t m_bits { 0 };
    unsigned m_numberOfBits { 0 };
};

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
