/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodedTexts.h"

#if ENABLE(AOT)

#include "AOTPrefixCode.h"
#include "AOTThreads.h"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

constexpr std::array<uint8_t, 11> punctuation { 0, '.', '(', ')', '_', '$', ' ', '"', '\'', '[', ']' };
constexpr unsigned uppercaseCode = 26;
constexpr unsigned digitCode = 27;
constexpr unsigned firstPunctuationCode = 28;
constexpr unsigned otherByteCode = firstPunctuationCode + punctuation.size();
constexpr unsigned prefixLengthCode = otherByteCode + 1;
constexpr unsigned numberOfTextCodes = prefixLengthCode + 1;

using TextCodes = PrefixCodes<numberOfTextCodes>;

constexpr auto codeByPreviousByte = [] {
    std::array<uint8_t, 256> codes { };
    codes.fill(otherByteCode);
    for (unsigned letter = 'a'; letter <= 'z'; ++letter)
        codes[letter] = letter - 'a';
    for (unsigned letter = 'A'; letter <= 'Z'; ++letter)
        codes[letter] = uppercaseCode;
    for (unsigned digit = '0'; digit <= '9'; ++digit)
        codes[digit] = digitCode;
    for (size_t index = 0; index < punctuation.size(); ++index)
        codes[punctuation[index]] = static_cast<uint8_t>(firstPunctuationCode + index);
    return codes;
}();

constexpr size_t sizeOfIndexFor(uint32_t count) { return (count + CodedTexts::textsInGroup - 1) / CodedTexts::textsInGroup * sizeof(uint32_t); }
constexpr size_t maxSharedPrefixLength = std::numeric_limits<uint8_t>::max();

} // anonymous namespace

bool CodedTexts::canHold(std::span<const uint8_t> text)
{
    return !text.empty() && std::ranges::all_of(text, [](uint8_t byte) { return byte && byte < 0x80; });
}

CodedTexts::CodedTexts(std::span<const uint8_t> bytes, uint32_t count)
    : m_bytes(bytes)
    , m_count(count)
{
}

Vector<uint8_t, 64> CodedTexts::at(uint32_t number) const
{
    Cursor cursor(*this);
    cursor.at(number);
    return WTF::move(cursor.m_text);
}

std::span<const uint8_t> CodedTexts::Cursor::at(uint32_t number)
{
    RELEASE_ASSERT(number < m_texts.m_count);
    const uint8_t* codes = m_texts.m_bytes.data();
    if (!m_reader || number + 1 < m_next || number / textsInGroup != (m_next - 1) / textsInGroup) {
        const uint8_t* index = TextCodes::endOfCodes(codes);
        uint32_t place;
        memcpy(&place, index + number / textsInGroup * sizeof(place), sizeof(place));
        m_reader.emplace(index + sizeOfIndexFor(m_texts.m_count) + place, codes + m_texts.m_bytes.size());
        m_next = number - number % textsInGroup;
    }
    for (; m_next <= number; ++m_next) {
        m_text.shrink(m_next % textsInGroup ? std::min<size_t>(m_reader->read(TextCodes::code(codes, prefixLengthCode)), m_text.size()) : 0);
        uint8_t previousByte = m_text.isEmpty() ? 0 : m_text.last();
        for (;;) {
            unsigned byte = m_reader->read(TextCodes::code(codes, codeByPreviousByte[previousByte]));
            if (!byte || byte == PrefixCode::endOfStream)
                break;
            previousByte = static_cast<uint8_t>(byte);
            m_text.append(previousByte);
        }
    }
    return m_text.span();
}

std::span<const uint8_t> CodedTexts::bytesOfGroup(uint32_t group) const
{
    RELEASE_ASSERT(group * textsInGroup < m_count);
    const uint8_t* index = TextCodes::endOfCodes(m_bytes.data());
    size_t firstGroup = index - m_bytes.data() + sizeOfIndexFor(m_count);
    auto beginOf = [&](uint32_t number) {
        uint32_t place;
        memcpy(&place, index + number * sizeof(place), sizeof(place));
        return firstGroup + place;
    };
    size_t begin = beginOf(group);
    size_t end = (group + 1) * textsInGroup < m_count ? beginOf(group + 1) : m_bytes.size();
    RELEASE_ASSERT(begin <= end && end <= m_bytes.size());
    return m_bytes.subspan(begin, end - begin);
}

Vector<uint8_t> CodedTexts::encode(std::span<const std::span<const uint8_t>> ascendingTexts, const ParallelLoops& loops)
{
    if (ascendingTexts.empty())
        return { };
    auto numberOfGroups = static_cast<unsigned>((ascendingTexts.size() + textsInGroup - 1) / textsInGroup);
    Vector<Vector<PrefixSymbol>> symbolsOfGroups(numberOfGroups);
    TextCodes::Counts counts { };
    Lock lockOfCounts;
    forEachRangeOfIndicesInParallel(loops, numberOfGroups, [&](size_t firstGroup, size_t endOfGroups) {
        TextCodes::Counts countsOfRange { };
        for (size_t group = firstGroup; group < endOfGroups; ++group) {
            size_t first = group * textsInGroup;
            std::span texts = ascendingTexts.subspan(first, std::min<size_t>(textsInGroup, ascendingTexts.size() - first));
            size_t maxSize = 0;
            for (std::span<const uint8_t> text : texts)
                maxSize += text.size() + 2;
            Vector<PrefixSymbol>& symbolsOfGroup = symbolsOfGroups[group];
            symbolsOfGroup.grow(maxSize);
            PrefixSymbol* firstSymbol = symbolsOfGroup.mutableSpan().data();
            PrefixSymbolWriter symbols(firstSymbol);
            std::span<const uint8_t> previousText;
            for (std::span<const uint8_t> text : texts) {
                RELEASE_ASSERT(canHold(text));
                size_t front = 0;
                if (!previousText.empty()) {
                    size_t longest = std::min({ text.size(), previousText.size(), maxSharedPrefixLength });
                    while (front < longest && text[front] == previousText[front])
                        ++front;
                    symbols.append(prefixLengthCode, static_cast<uint8_t>(front));
                }
                uint8_t previousByte = front ? text[front - 1] : 0;
                for (uint8_t byte : text.subspan(front))
                    symbols.append(codeByPreviousByte[std::exchange(previousByte, byte)], byte);
                symbols.append(codeByPreviousByte[previousByte], 0);
                previousText = text;
            }
            symbolsOfGroup.shrink(symbols.end() - firstSymbol);
            TextCodes::count(countsOfRange, symbolsOfGroup.span());
        }
        Locker locker { lockOfCounts };
        TextCodes::add(counts, countsOfRange);
    });

    TextCodes codes(counts);
    Vector<Vector<uint8_t>> groups(numberOfGroups);
    forEachIndexInParallel(loops, numberOfGroups, [&](unsigned group) {
        std::span symbols = symbolsOfGroups[group].span();
        groups[group].grow(symbols.size() * PrefixCode::maxLength / 8 + 1);
        uint8_t* firstByte = groups[group].mutableSpan().data();
        groups[group].shrink(codes.write(firstByte, symbols) - firstByte);
    });

    Vector<uint8_t> bytes(codes.bytes());
    size_t index = bytes.size();
    bytes.grow(index + sizeOfIndexFor(static_cast<uint32_t>(ascendingTexts.size())));
    size_t firstGroup = bytes.size();
    for (unsigned group = 0; group < numberOfGroups; ++group) {
        RELEASE_ASSERT(bytes.size() - firstGroup <= std::numeric_limits<uint32_t>::max());
        uint32_t place = static_cast<uint32_t>(bytes.size() - firstGroup);
        memcpy(bytes.mutableSpan().data() + index + group * sizeof(place), &place, sizeof(place));
        bytes.appendVector(groups[group]);
    }
    while (bytes.size() % sizeof(uint32_t))
        bytes.append(0);
    return bytes;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
