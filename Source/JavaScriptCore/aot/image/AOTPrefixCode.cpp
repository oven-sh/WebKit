/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPrefixCode.h"

#if ENABLE(AOT)

#include <algorithm>
#include <limits>
#include <utility>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

PrefixCode::Lengths PrefixCode::findLengths(const Counts& counts)
{
    struct Leaf {
        uint32_t count;
        uint16_t symbol;
    };

    Vector<Leaf, endOfStream + 1> leaves;
    leaves.append({ 0, endOfStream });
    for (unsigned byte = 0; byte < endOfStream; ++byte) {
        if (counts[byte])
            leaves.append({ counts[byte], static_cast<uint16_t>(byte) });
    }
    Lengths lengths { };
    size_t numberOfLeaves = leaves.size();
    if (numberOfLeaves == 1)
        return lengths;

    for (;;) {
        std::sort(leaves.begin() + 1, leaves.end(), [](const Leaf& a, const Leaf& b) {
            return a.count != b.count ? a.count < b.count : a.symbol < b.symbol;
        });
        size_t numberOfNodes = 2 * numberOfLeaves - 1;
        std::array<uint64_t, 2 * endOfStream + 1> weights;
        std::array<uint16_t, 2 * endOfStream + 1> parents;
        for (size_t leaf = 0; leaf < numberOfLeaves; ++leaf)
            weights[leaf] = leaves[leaf].count;
        size_t nextLeaf = 0;
        size_t nextInner = numberOfLeaves;
        for (size_t node = numberOfLeaves; node < numberOfNodes; ++node) {
            auto takeLightest = [&]() -> size_t {
                bool takesLeaf = nextLeaf < numberOfLeaves && (nextInner == node || weights[nextLeaf] <= weights[nextInner]);
                size_t lightest = takesLeaf ? nextLeaf++ : nextInner++;
                parents[lightest] = static_cast<uint16_t>(node);
                return lightest;
            };
            size_t first = takeLightest();
            size_t second = takeLightest();
            weights[node] = weights[first] + weights[second];
        }
        std::array<uint16_t, 2 * endOfStream + 1> depths;
        depths[numberOfNodes - 1] = 0;
        for (size_t node = numberOfNodes - 1; node--;)
            depths[node] = depths[parents[node]] + 1;
        if (depths[0] <= maxLength) {
            for (size_t leaf = 0; leaf < numberOfLeaves; ++leaf)
                lengths[leaves[leaf].symbol] = static_cast<uint8_t>(depths[leaf]);
            return lengths;
        }
        for (Leaf& leaf : leaves.mutableSpan().subspan(1))
            leaf.count -= leaf.count / 2;
    }
}

PrefixCode::PrefixCode(const Counts& counts)
{
    Lengths lengths = findLengths(counts);
    unsigned word = 0;
    for (unsigned length = 1; length <= maxLength; ++length) {
        word <<= 1;
        unsigned numberOfWords = 0;
        for (unsigned byte = 0; byte < endOfStream; ++byte) {
            if (lengths[byte] != length)
                continue;
            m_words[byte] = { static_cast<uint16_t>(word++), static_cast<uint8_t>(length) };
            m_bytes.append(static_cast<uint8_t>(byte));
            ++numberOfWords;
        }
        RELEASE_ASSERT(numberOfWords <= std::numeric_limits<uint8_t>::max());
        m_wordsOfLength[length - 1] = static_cast<uint8_t>(numberOfWords);
    }
}

unsigned PrefixCode::shortCodeBits(unsigned most) const
{
    unsigned longest = 1;
    for (const Word& word : m_words)
        longest = std::max<unsigned>(longest, word.length);
    return std::min(longest, most);
}

void PrefixCode::append(Vector<uint8_t>& bytes, unsigned shortCodeBits) const
{
    Vector<uint16_t> shortWords(FillWith { }, size_t { 1 } << shortCodeBits, 0);
    for (unsigned byte = 0; byte < endOfStream; ++byte) {
        auto [bits, length] = m_words[byte];
        if (!length || length > shortCodeBits)
            continue;
        unsigned bitsBehind = shortCodeBits - length;
        std::fill_n(shortWords.mutableSpan().data() + (bits << bitsBehind), 1u << bitsBehind, static_cast<uint16_t>(length << 8 | byte));
    }
    unsigned firstWord = 0;
    unsigned numberOfShortCodes = 0;
    for (unsigned length = 1; length <= shortCodeBits; ++length) {
        firstWord = (firstWord + m_wordsOfLength[length - 1]) << 1;
        numberOfShortCodes += m_wordsOfLength[length - 1];
    }
    LongWords longWords { static_cast<uint16_t>(firstWord), static_cast<uint16_t>(numberOfShortCodes) };
    RELEASE_ASSERT(longWords.firstWord == firstWord);
    bytes.append(asBytes(shortWords.span()));
    bytes.append(asByteSpan(longWords));
    bytes.append(std::span<const uint8_t>(m_wordsOfLength));
    bytes.appendVector(m_bytes);
}

namespace {

constexpr uint32_t rawBytesFlag = 1u << 31;
constexpr unsigned numberOfVarintCodes = 16;
using VarintCodes = PrefixCodes<numberOfVarintCodes>;

constexpr unsigned codeOfVarintByte(uint8_t previousByte, uint8_t secondPreviousByte)
{
    return (previousByte >> 7) << 3 | (secondPreviousByte >> 7) << 2 | (previousByte & 3);
}

} // anonymous namespace

Vector<uint8_t> encodeVarintBytes(std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return { };
    Vector<PrefixSymbol> symbols;
    symbols.grow(bytes.size());
    PrefixSymbolWriter writer(symbols.mutableSpan().data());
    uint8_t previousByte = 0;
    uint8_t secondPreviousByte = 0;
    for (uint8_t byte : bytes) {
        writer.append(codeOfVarintByte(previousByte, secondPreviousByte), byte);
        secondPreviousByte = std::exchange(previousByte, byte);
    }
    VarintCodes::Counts counts { };
    VarintCodes::count(counts, symbols.span());
    VarintCodes codes(counts);

    RELEASE_ASSERT(bytes.size() < rawBytesFlag);
    uint32_t size = static_cast<uint32_t>(bytes.size());
    Vector<uint8_t> result;
    result.append(asByteSpan(size));
    result.appendVector(codes.bytes());
    size_t previousSize = result.size();
    result.grow(previousSize + symbols.size() * PrefixCode::maxLength / 8 + 1);
    result.shrink(codes.write(result.mutableSpan().data() + previousSize, symbols.span()) - result.mutableSpan().data());
    if (result.size() < bytes.size() + sizeof(size))
        return result;
    size |= rawBytesFlag;
    result.shrink(0);
    result.append(asByteSpan(size));
    result.append(bytes);
    return result;
}

Vector<uint8_t> decodeVarintBytes(std::span<const uint8_t> coded)
{
    if (coded.empty())
        return { };
    uint32_t size;
    memcpy(&size, coded.data(), sizeof(size));
    coded = coded.subspan(sizeof(size));
    if (size & rawBytesFlag)
        return Vector<uint8_t>(coded.first(size & ~rawBytesFlag));
    PrefixCodeReader reader(VarintCodes::endOfCodes(coded.data()), coded.data() + coded.size());
    Vector<uint8_t> bytes;
    bytes.reserveInitialCapacity(size);
    uint8_t previousByte = 0;
    uint8_t secondPreviousByte = 0;
    for (uint32_t index = 0; index < size; ++index) {
        unsigned byte = reader.read(VarintCodes::code(coded.data(), codeOfVarintByte(previousByte, secondPreviousByte)));
        secondPreviousByte = std::exchange(previousByte, static_cast<uint8_t>(byte));
        bytes.append(previousByte);
    }
    return bytes;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
