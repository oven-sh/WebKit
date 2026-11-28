/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTextEncoder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTPhaseTimes.h"
#include "AOTSectionWriter.h"
#include "AOTStringUses.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/StringHasherInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(ImageString::hashMask == StringHasher::maskHash && ImageIdentifier::hashMask == StringHasher::maskHash);
static_assert(!(ImageString::hashMask & ImageString::is16BitFlag));
static_assert(defaultLimitValue(LimitID::MaxIdentifiers, hostArchitecture()) == ImageString::hashMask + 1);
static_assert(defaultLimitValue(LimitID::MaxListedStrings, hostArchitecture()) == 1u << (32 - ImageString::lengthBits));
static_assert(numberOfEnumerators<PropertyKeyKind> && numberOfEnumerators<PropertyKeyKind> <= 1u << ImageIdentifier::kindBits);
static_assert(numberOfEnumerators<NameClass> && numberOfEnumerators<NameClass> <= 1u << ImageIdentifier::nameClassBits);
static_assert(numberOfEnumerators<Typed::WellKnownSymbol> && numberOfEnumerators<Typed::WellKnownSymbol> <= ImageIdentifier::hashMask + 1);

static constexpr size_t textHeadSize = sizeof(uint32_t);
static constexpr size_t maxSizeOfTextBeginning = 2 * textHeadSize;
static constexpr unsigned textHeadSlotBits = 22;
static constexpr unsigned textBeginningSlotBits = 23;
static constexpr uint64_t maxComparedCharactersPerCharacter = 1024;

namespace {

struct MeasuredTextOfString {
    bool is16Bit() const { return hashAndWidth & ImageString::is16BitFlag; }
    uint32_t hash() const { return hashAndWidth & ImageString::hashMask; }
    uint64_t sizeInBytes() const { return static_cast<uint64_t>(length) * (is16Bit() ? sizeof(char16_t) : sizeof(uint8_t)); }
    std::span<const uint8_t> bytes() const { return { characters, static_cast<size_t>(sizeInBytes()) }; }

    const uint8_t* characters;
    uint32_t length;
    uint32_t hashAndWidth;
};

struct TextsOfStrings {
    std::span<const uint8_t> operator[](uint32_t index) const { return measured[index].bytes(); }

    std::span<const MeasuredTextOfString> measured;
};

struct TextSharing {
    Vector<uint32_t> holders;
    Vector<uint32_t> gaps;
};

struct TextInOrder {
    const uint8_t* characters;
    uint32_t size;
    uint32_t index;
    uint64_t afterHead;
};

struct TextWithPrefix {
    uint64_t prefix;
    uint32_t index;
    uint32_t unused;
};
static_assert(sizeof(TextInOrder) >= sizeof(MeasuredTextOfString) && sizeof(TextInOrder) >= sizeof(TextWithPrefix) && sizeof(TextInOrder) >= sizeof(ImageString));
static_assert(!VectorTraits<MeasuredTextOfString>::needsInitialization && !VectorTraits<TextInOrder>::needsInitialization && !VectorTraits<TextWithPrefix>::needsInitialization, "the loops that fill a list of them are the first to touch its memory");

enum class EndOfText : bool { First, Last };

struct TextHeadRange {
    static constexpr unsigned shiftOfSizesOfPlainTexts = 8;
    static constexpr uint32_t maskOfSizes = (1u << shiftOfSizesOfPlainTexts) - 1;
    static_assert(maxSizeOfTextBeginning - textHeadSize < shiftOfSizesOfPlainTexts);

    bool isEmpty() const { return !end; }

    uint32_t head { 0 };
    uint32_t begin { 0 };
    uint32_t end { 0 };
    uint32_t sizesOfBeginnings { 0 };
};

struct IndexOfTextHeads {
    static constexpr uint32_t noText = std::numeric_limits<uint32_t>::max();

    const TextHeadRange* findRange(uint32_t head) const
    {
        const TextHeadRange* ranges = headRanges.span().data();
        size_t mask = headRanges.size() - 1;
        for (size_t slot = static_cast<size_t>(head * 0x9e3779b97f4a7c15ULL >> headRangeShift);; slot = (slot + 1) & mask) {
            const TextHeadRange& range = ranges[slot];
            if (range.isEmpty())
                return nullptr;
            if (range.head == head)
                return &range;
        }
    }

    Vector<TextInOrder> textsInOrder;
    Vector<uint64_t> afterHeads;
    Vector<uint32_t> longestHeads;
    Vector<TextHeadRange> headRanges;
    unsigned headRangeShift { 0 };
    Vector<uint64_t> mayBeHead;
    Vector<uint64_t> mayBeBeginning;
    Vector<uint64_t> mayBeHeadOfPlainText;
    Vector<uint64_t> mayBeBeginningOfPlainText;
    Vector<uint32_t> fromLongest;
};

struct LongStringOfTextChunk {
    uint32_t string;
    ImageLongString range;
};

struct ChunkOfEncodedText {
    uint64_t startOfText { 0 };
    uint64_t sizeOfText { 0 };
    uint32_t firstLongString { 0 };
    Vector<LongStringOfTextChunk> longStrings;
};

struct RetainedStrings {
    static constexpr uint32_t noPlace = std::numeric_limits<uint32_t>::max();

    void forEachInChunk(unsigned chunk, NOESCAPE const Invocable<void(unsigned)> auto& functor) const
    {
        for (unsigned place = division.firstIndexOf[chunk]; place < division.firstIndexOf[chunk + 1]; ++place)
            functor(inOrderOfText[place].index());
    }

    void forEachHolderInChunk(unsigned chunk, NOESCAPE const Invocable<void(unsigned)> auto& functor) const
    {
        for (unsigned place = division.firstIndexOf[chunk]; place < division.firstIndexOf[chunk + 1]; ++place) {
            uint32_t holder = sharing.holders[inOrderOfText[place].index()];
            if (firstPlacesOfHolders[holder] == place)
                functor(holder);
        }
    }

    BitVector isName;
    std::array<BitVector, 2> hasTextOfWidth;
    std::span<const StringID> inOrderOfText;
    Vector<MeasuredTextOfString> measured;
    Vector<Vector<DecodedString>> decodedStringsOfRanges;
    TextSharing sharing;
    Vector<uint64_t> leastFinds;
    Vector<uint32_t> firstPlacesOfHolders;
    Vector<uint32_t> endsOfText;
    ChunksOfIndices division;
    Vector<ChunkOfEncodedText> chunks;
};

} // anonymous namespace

DecodedString decodeForImage(std::span<const uint8_t> programString)
{
    DecodedString result;
    if (charactersAreAllASCII(byteCast<Latin1Character>(programString)))
        result.units8 = Vector<uint8_t>(programString);
    else {
        Vector<char16_t> units = Typed::decodeProgramString(programString);
        if (WTF::charactersAreAllLatin1(units.span())) {
            result.units8 = Vector<uint8_t>(units.size(), [&](size_t index) {
                return static_cast<uint8_t>(units[index]);
            });
        } else
            result.units16 = WTF::move(units);
    }
    result.hash = result.is16Bit()
        ? StringHasher::computeHashAndMaskTop8Bits(result.units16.span())
        : StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(result.units8.span()));
    return result;
}

static uint64_t prefixOfTextFrom(auto at, auto end)
{
    uint64_t result = 0;
    for (unsigned shift = 64; shift && at != end; ++at)
        result |= static_cast<uint64_t>(*at) << (shift -= 8);
    return result;
}

static uint64_t load64(const uint8_t* bytes)
{
    static_assert(std::endian::native == std::endian::little);
    uint64_t word;
    memcpy(&word, bytes, sizeof(word));
    return word;
}

static uint64_t prefixOfTextAt(EndOfText end, std::span<const uint8_t> text)
{
    if (end == EndOfText::Last)
        return text.size() >= sizeof(uint64_t) ? load64(text.data() + text.size() - sizeof(uint64_t)) : prefixOfTextFrom(text.rbegin(), text.rend());
    return text.size() >= sizeof(uint64_t) ? std::byteswap(load64(text.data())) : prefixOfTextFrom(text.begin(), text.end());
}

static uint32_t textHeadAt(const uint8_t* text)
{
    uint32_t head;
    memcpy(&head, text, textHeadSize);
    return head;
}

static uint64_t prefixAfterTextHead(std::span<const uint8_t> text)
{
    if (text.size() >= textHeadSize + sizeof(uint64_t))
        return std::byteswap(load64(text.data() + textHeadSize));
    uint64_t result = 0;
    for (size_t at = textHeadSize; at < text.size(); ++at)
        result |= static_cast<uint64_t>(text[at]) << (8 * (textHeadSize + sizeof(uint64_t) - 1 - at));
    return result;
}

static uint64_t prefixAfterTextHeadAt(std::span<const uint8_t> text, size_t at)
{
    size_t sizeOfRest = text.size() - at;
    if (sizeOfRest >= textHeadSize + sizeof(uint64_t))
        return std::byteswap(load64(text.data() + at + textHeadSize));
    if (sizeOfRest == textHeadSize)
        return 0;
    if (text.size() < sizeof(uint64_t))
        return prefixAfterTextHead(text.subspan(at));
    return std::byteswap(load64(text.data() + text.size() - sizeof(uint64_t))) << (8 * (textHeadSize + sizeof(uint64_t) - sizeOfRest));
}

static size_t slotOfTextBeginning(uint32_t head, uint64_t afterHead, size_t size)
{
    uint64_t charactersAfterHead = afterHead >> (8 * (textHeadSize + sizeof(uint64_t) - size));
    return static_cast<size_t>(((charactersAfterHead << 32 | head) ^ size << 59) * 0x9e3779b97f4a7c15ULL >> (64 - textBeginningSlotBits));
}

static size_t slotOfTextHead(uint32_t head)
{
    return static_cast<size_t>(head * 0x9e3779b97f4a7c15ULL >> (64 - textHeadSlotBits));
}

static bool endOrdersBefore(std::span<const uint8_t> a, std::span<const uint8_t> b, uint32_t indexOfA, uint32_t indexOfB)
{
    size_t shorterSize = std::min(a.size(), b.size());
    const uint8_t* endOfA = a.data() + a.size();
    const uint8_t* endOfB = b.data() + b.size();
    size_t same = shorterSize >= sizeof(uint64_t) ? sizeof(uint64_t) : 0;
    for (; shorterSize - same >= sizeof(uint64_t); same += sizeof(uint64_t)) {
        uint64_t wordOfA = load64(endOfA - same - sizeof(uint64_t));
        uint64_t wordOfB = load64(endOfB - same - sizeof(uint64_t));
        if (wordOfA != wordOfB)
            return wordOfA < wordOfB;
    }
    for (; same < shorterSize; ++same) {
        uint8_t characterOfA = endOfA[-1 - static_cast<ptrdiff_t>(same)];
        uint8_t characterOfB = endOfB[-1 - static_cast<ptrdiff_t>(same)];
        if (characterOfA != characterOfB)
            return characterOfA < characterOfB;
    }
    return a.size() != b.size() ? a.size() < b.size() : indexOfA < indexOfB;
}

static bool beginningOrdersBefore(std::span<const uint8_t> a, std::span<const uint8_t> b, uint32_t indexOfA, uint32_t indexOfB)
{
    int order = memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
    if (order)
        return order < 0;
    return a.size() != b.size() ? a.size() < b.size() : indexOfA < indexOfB;
}

namespace {

struct OrderOfTextsWithOnePrefix {
    bool operator()(const TextWithPrefix& a, const TextWithPrefix& b) const
    {
        if (end == EndOfText::Last)
            return endOrdersBefore(strings[a.index], strings[b.index], a.index, b.index);
        return beginningOrdersBefore(strings[a.index], strings[b.index], a.index, b.index);
    }

    TextsOfStrings strings;
    EndOfText end;
};

} // anonymous namespace

static void sortTextsWithPrefixes(const ParallelLoops& loops, TextsOfStrings strings, EndOfText end, std::span<TextWithPrefix> texts)
{
    auto prefixOf = [](const TextWithPrefix& text) {
        return text.prefix;
    };
    sortByWordOnPoolKeepingEqualElementsInOrder(loops, texts, prefixOf);
    sortRunsOfEqualWordsOnPool(loops, texts, prefixOf, OrderOfTextsWithOnePrefix { strings, end });
}

static Vector<TextWithPrefix> listHoldersWithPrefixes(const ParallelLoops& loops, TextsOfStrings strings, const BitVector& hasText, const Vector<uint32_t>& holders, EndOfText end, size_t smallestSize)
{
    size_t numberOfStrings = holders.size();
    auto forEachHolderIn = [&](size_t first, size_t endOfRange, const auto& functor) {
        for (size_t bit = hasText.findBit(first, true); bit < endOfRange; bit = hasText.findBit(bit + 1, true)) {
            auto index = static_cast<uint32_t>(bit);
            if (holders[index] == index && strings[index].size() >= smallestSize)
                functor(index);
        }
    };
    Vector<size_t> firstPlaces(FillWith { }, numberOfRangesOfIndices(numberOfStrings) + 1, 0);
    forEachRangeOfIndicesInParallel(loops, numberOfStrings, [&](size_t first, size_t endOfRange) {
        size_t count = 0;
        forEachHolderIn(first, endOfRange, [&](uint32_t) {
            ++count;
        });
        firstPlaces[first / indicesInRangeOfLoop + 1] = count;
    });
    for (size_t range = 1; range < firstPlaces.size(); ++range)
        firstPlaces[range] += firstPlaces[range - 1];

    Vector<TextWithPrefix> list;
    list.grow(firstPlaces.last());
    forEachRangeOfIndicesInParallel(loops, numberOfStrings, [&](size_t first, size_t endOfRange) {
        size_t place = firstPlaces[first / indicesInRangeOfLoop];
        forEachHolderIn(first, endOfRange, [&](uint32_t index) {
            list[place++] = { prefixOfTextAt(end, strings[index]), index, 0 };
        });
    });
    return list;
}

static void shareTails(const ParallelLoops& loops, TextsOfStrings strings, const BitVector& hasText, const BitVector& isName, Vector<uint32_t>& holders)
{
    Vector<TextWithPrefix> byLastCharacters = listHoldersWithPrefixes(loops, strings, hasText, holders, EndOfText::Last, 0);
    sortTextsWithPrefixes(loops, strings, EndOfText::Last, byLastCharacters.mutableSpan());
    size_t numberOfPairs = byLastCharacters.size() - 1;
    Vector<uint8_t> isTailOfNext;
    isTailOfNext.grow(numberOfPairs);
    forEachRangeOfIndicesInParallel(loops, numberOfPairs, [&](size_t first, size_t end) {
        for (size_t place = first; place < end; ++place) {
            auto tail = strings[byLastCharacters[place].index];
            auto whole = strings[byLastCharacters[place + 1].index];
            isTailOfNext[place] = tail.size() <= whole.size() && equalSpans(tail, whole.last(tail.size()));
        }
    });
    forEachRangeOfIndicesInParallel(loops, numberOfPairs, [&](size_t first, size_t end) {
        for (size_t last = end; last-- > first;) {
            if (!isTailOfNext[last] || (last + 1 < numberOfPairs && isTailOfNext[last + 1]))
                continue;
            for (size_t place = last + 1; place-- && isTailOfNext[place];) {
                uint32_t tail = byLastCharacters[place].index;
                uint32_t holder = holders[byLastCharacters[place + 1].index];
                if (!isName.quickGet(tail) || isName.quickGet(holder))
                    holders[tail] = holder;
            }
        }
    });
}

static void indexTextsByHeads(const ParallelLoops& loops, TextsOfStrings strings, const BitVector& isName, Vector<TextWithPrefix>& inOrder, IndexOfTextHeads& result)
{
    sortTextsWithPrefixes(loops, strings, EndOfText::First, inOrder.mutableSpan());

    size_t numberOfTexts = inOrder.size();
    result.textsInOrder.grow(numberOfTexts);
    result.afterHeads.grow(numberOfTexts);
    forEachRangeOfIndicesInParallel(loops, numberOfTexts, [&](size_t first, size_t end) {
        for (size_t position = first; position < end; ++position) {
            auto text = strings[inOrder[position].index];
            uint64_t afterHead = prefixAfterTextHead(text);
            result.textsInOrder[position] = { text.data(), safeCast<uint32_t>(text.size()), inOrder[position].index, afterHead };
            result.afterHeads[position] = afterHead;
        }
    });

    result.longestHeads.grow(numberOfTexts);
    result.mayBeBeginning.fill(0, (1 << textBeginningSlotBits) / 64);
    result.mayBeBeginningOfPlainText.fill(0, (1 << textBeginningSlotBits) / 64);
    Vector<Vector<TextHeadRange>> headRangesOfLoopRanges(numberOfRangesOfIndices(numberOfTexts));
    forEachRangeOfIndicesInParallel(loops, numberOfTexts, [&](size_t first, size_t end) {
        auto headOf = [&](size_t position) {
            return textHeadAt(result.textsInOrder[position].characters);
        };
        Vector<TextHeadRange>& headRanges = headRangesOfLoopRanges[first / indicesInRangeOfLoop];
        Vector<uint32_t, 16> open;
        size_t position = first;
        while (position && position < end && headOf(position) == headOf(position - 1))
            ++position;
        while (position < end) {
            uint32_t head = headOf(position);
            size_t begin = position;
            uint32_t sizesOfBeginnings = 0;
            open.shrink(0);
            for (; position < numberOfTexts && headOf(position) == head; ++position) {
                const TextInOrder& text = result.textsInOrder[position];
                bool isPlainText = !isName.quickGet(text.index);
                size_t sizeOfBeginning = std::min<size_t>(text.size, maxSizeOfTextBeginning);
                sizesOfBeginnings |= 1u << (sizeOfBeginning - textHeadSize);
                if (isPlainText)
                    sizesOfBeginnings |= 1u << (TextHeadRange::shiftOfSizesOfPlainTexts + sizeOfBeginning - textHeadSize);
                if (sizeOfBeginning > textHeadSize) {
                    size_t slot = slotOfTextBeginning(head, text.afterHead, sizeOfBeginning);
                    AtomicRef(result.mayBeBeginning[slot / 64]).fetch_or(1ull << slot % 64, std::memory_order_relaxed);
                    if (isPlainText)
                        AtomicRef(result.mayBeBeginningOfPlainText[slot / 64]).fetch_or(1ull << slot % 64, std::memory_order_relaxed);
                }
                while (!open.isEmpty()) {
                    const TextInOrder& candidate = result.textsInOrder[open.last()];
                    if (candidate.size <= text.size && !memcmp(text.characters, candidate.characters, candidate.size))
                        break;
                    open.removeLast();
                }
                result.longestHeads[position] = open.isEmpty() ? IndexOfTextHeads::noText : open.last();
                open.append(static_cast<uint32_t>(position));
            }
            headRanges.append({ head, static_cast<uint32_t>(begin), static_cast<uint32_t>(position), sizesOfBeginnings });
        }
    });
    size_t numberOfHeads = 0;
    for (const Vector<TextHeadRange>& headRanges : headRangesOfLoopRanges)
        numberOfHeads += headRanges.size();
    size_t numberOfSlots = std::bit_ceil(numberOfHeads * 2 + 1);
    result.headRangeShift = 64 - std::countr_zero(numberOfSlots);
    result.headRanges.fill(TextHeadRange { }, numberOfSlots);
    result.mayBeHead.fill(0, (1 << textHeadSlotBits) / 64);
    result.mayBeHeadOfPlainText.fill(0, (1 << textHeadSlotBits) / 64);
    for (const Vector<TextHeadRange>& headRanges : headRangesOfLoopRanges) {
        for (const TextHeadRange& range : headRanges) {
            size_t slot = static_cast<size_t>(range.head * 0x9e3779b97f4a7c15ULL >> result.headRangeShift);
            while (!result.headRanges[slot].isEmpty())
                slot = (slot + 1) & (numberOfSlots - 1);
            result.headRanges[slot] = range;
            size_t slotOfHead = slotOfTextHead(range.head);
            result.mayBeHead[slotOfHead / 64] |= 1ull << slotOfHead % 64;
            if (range.sizesOfBeginnings >> TextHeadRange::shiftOfSizesOfPlainTexts)
                result.mayBeHeadOfPlainText[slotOfHead / 64] |= 1ull << slotOfHead % 64;
        }
    }
}

static IndexOfTextHeads buildHeadIndex(const ParallelLoops& loops, TextsOfStrings strings, const BitVector& hasText, const BitVector& isName, const Vector<uint32_t>& holders)
{
    Vector<TextWithPrefix> inOrder = listHoldersWithPrefixes(loops, strings, hasText, holders, EndOfText::First, textHeadSize);
    Vector<uint64_t> lengthsAndNumbers;
    lengthsAndNumbers.grow(inOrder.size());
    forEachRangeOfIndicesInParallel(loops, inOrder.size(), [&](size_t first, size_t end) {
        for (size_t place = first; place < end; ++place)
            lengthsAndNumbers[place] = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max() - safeCast<uint32_t>(strings[inOrder[place].index].size())) << 32 | inOrder[place].index;
    });

    IndexOfTextHeads result;
    runLanes(loops, {
        [&] {
            indexTextsByHeads(loops, strings, isName, inOrder, result);
        },
        [&] {
            radixSortKeepingEqualKeysInOrder<32>(lengthsAndNumbers, [](uint64_t lengthAndNumber) {
                return lengthAndNumber >> 32;
            });
            result.fromLongest = Vector<uint32_t>(lengthsAndNumbers.size(), [&](size_t place) {
                return static_cast<uint32_t>(lengthsAndNumbers[place]);
            });
        },
    });
    return result;
}

static bool textOrdersBeforeTextWithSameRecordedCharacters(std::span<const uint8_t> a, const TextInOrder& b, uint64_t& charactersCompared)
{
    constexpr size_t recordedSize = textHeadSize + sizeof(uint64_t);
    size_t shorterSize = std::min<size_t>(a.size(), b.size);
    if (shorterSize <= recordedSize) {
        charactersCompared += shorterSize;
        return b.size > shorterSize;
    }
    size_t same = recordedSize;
    for (; shorterSize - same >= sizeof(uint64_t); same += sizeof(uint64_t)) {
        uint64_t wordOfA = load64(a.data() + same);
        uint64_t wordOfB = load64(b.characters + same);
        if (wordOfA != wordOfB) {
            charactersCompared += same + std::countr_zero(wordOfA ^ wordOfB) / 8;
            return std::byteswap(wordOfA) < std::byteswap(wordOfB);
        }
    }
    while (same < shorterSize && a[same] == b.characters[same])
        ++same;
    charactersCompared += same;
    return same < b.size && (same == a.size() || a[same] < b.characters[same]);
}

static uint32_t findLastTextNotAfter(const uint64_t* afterHeads, const TextInOrder* textsInOrder, const TextHeadRange& range, std::span<const uint8_t> rest, uint64_t afterHeadOfRest, uint64_t& charactersCompared)
{
    uint32_t low = range.begin;
    for (uint32_t count = range.end - range.begin; count;) {
        uint32_t half = count / 2;
        bool isBefore = afterHeads[low + half] < afterHeadOfRest;
        low = isBefore ? low + half + 1 : low;
        count = isBefore ? count - half - 1 : half;
    }
    if (low < range.end && afterHeads[low] == afterHeadOfRest) {
        uint32_t high = low + 1;
        for (uint32_t count = range.end - high; count;) {
            uint32_t half = count / 2;
            bool isSame = afterHeads[high + half] == afterHeadOfRest;
            high = isSame ? high + half + 1 : high;
            count = isSame ? count - half - 1 : half;
        }
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            if (textOrdersBeforeTextWithSameRecordedCharacters(rest, textsInOrder[middle], charactersCompared))
                high = middle;
            else
                low = middle + 1;
        }
    }
    if (low == range.begin)
        return IndexOfTextHeads::noText;
    return low - 1;
}

static bool startsWithTextInOrder(std::span<const uint8_t> rest, uint64_t afterHeadOfRest, const TextInOrder& candidate)
{
    constexpr size_t recordedSize = textHeadSize + sizeof(uint64_t);
    if (candidate.size > rest.size())
        return false;
    uint64_t difference = afterHeadOfRest ^ candidate.afterHead;
    if (candidate.size < recordedSize)
        return candidate.size == textHeadSize || !(difference >> (8 * (recordedSize - candidate.size)));
    return !difference && !memcmp(rest.data() + recordedSize, candidate.characters + recordedSize, candidate.size - recordedSize);
}

static NEVER_INLINE size_t findPositionWhoseHeadMayBeInSomeText(const uint8_t* characters, size_t at, size_t endOfHeads, size_t step, const uint64_t* mayBeHead)
{
    for (; at < endOfHeads; at += step) {
        size_t slotOfHead = slotOfTextHead(textHeadAt(characters + at));
        if (mayBeHead[slotOfHead / 64] >> slotOfHead % 64 & 1)
            break;
    }
    return at;
}

static void findInnerParts(const ParallelLoops& loops, TextsOfStrings strings, const BitVector& isName, const IndexOfTextHeads& heads, size_t unitSize, TextSharing& sharing, Vector<uint64_t>& leastFinds)
{
    forEachIndexInParallel(loops, static_cast<unsigned>(heads.fromLongest.size()), [&](unsigned place) {
        uint32_t whole = heads.fromLongest[place];
        auto text = strings[whole];
        bool wholeIsName = isName.quickGet(whole);
        uint64_t maxCharactersCompared = maxComparedCharactersPerCharacter * static_cast<uint64_t>(text.size());
        uint64_t charactersCompared = 0;
        const uint64_t* mayBeHead = (wholeIsName ? heads.mayBeHead : heads.mayBeHeadOfPlainText).span().data();
        const uint64_t* mayBeBeginning = (wholeIsName ? heads.mayBeBeginning : heads.mayBeBeginningOfPlainText).span().data();
        unsigned shiftOfSizes = wholeIsName ? 0 : TextHeadRange::shiftOfSizesOfPlainTexts;
        const uint64_t* afterHeads = heads.afterHeads.span().data();
        const TextInOrder* textsInOrder = heads.textsInOrder.span().data();
        const uint32_t* longestHeads = heads.longestHeads.span().data();
        const uint8_t* characters = text.data();
        if (text.size() < textHeadSize)
            return;
        size_t endOfHeads = text.size() - textHeadSize + 1;
        size_t step = unitSize;
        for (size_t at = 0; charactersCompared <= maxCharactersCompared; at += step) {
            at = findPositionWhoseHeadMayBeInSomeText(characters, at, endOfHeads, step, mayBeHead);
            if (at >= endOfHeads)
                return;
            uint32_t head = textHeadAt(characters + at);
            const TextHeadRange* range = heads.findRange(head);
            if (!range)
                continue;
            std::span<const uint8_t> rest(characters + at, text.size() - at);
            uint64_t afterHeadOfRest = prefixAfterTextHeadAt(text, at);
            uint32_t sizesOfBeginnings = range->sizesOfBeginnings >> shiftOfSizes & TextHeadRange::maskOfSizes;
            bool mayStartWithText = sizesOfBeginnings & 1;
            for (uint32_t sizes = sizesOfBeginnings >> 1; sizes && !mayStartWithText; sizes &= sizes - 1) {
                size_t size = textHeadSize + 1 + std::countr_zero(sizes);
                if (size > rest.size())
                    break;
                size_t slot = slotOfTextBeginning(head, afterHeadOfRest, size);
                mayStartWithText = mayBeBeginning[slot / 64] >> slot % 64 & 1;
            }
            if (!mayStartWithText)
                continue;
            uint64_t find = (static_cast<uint64_t>(place) + 1) << 32 | at;
            bool restStartsWithCandidate = false;
            for (uint32_t position = findLastTextNotAfter(afterHeads, textsInOrder, *range, rest, afterHeadOfRest, charactersCompared); position != IndexOfTextHeads::noText; position = longestHeads[position]) {
                const TextInOrder& candidate = textsInOrder[position];
                if (candidate.size >= text.size())
                    continue;
                if (!restStartsWithCandidate) {
                    charactersCompared += candidate.size;
                    restStartsWithCandidate = startsWithTextInOrder(rest, afterHeadOfRest, candidate);
                    if (!restStartsWithCandidate)
                        continue;
                }
                if (!wholeIsName && isName.quickGet(candidate.index))
                    continue;
                AtomicRef least(leastFinds[candidate.index]);
                uint64_t known = least.load(std::memory_order_relaxed);
                while ((!known || find < known) && !least.compare_exchange_weak(known, find, std::memory_order_relaxed)) { }
            }
        }
    });
    forEachRangeOfIndicesInParallel(loops, heads.fromLongest.size(), [&](size_t first, size_t end) {
        for (size_t place = first; place < end; ++place) {
            uint32_t part = heads.fromLongest[place];
            uint32_t holder = part;
            uint64_t gap = 0;
            while (uint64_t find = leastFinds[holder]) {
                uint32_t whole = heads.fromLongest[static_cast<uint32_t>(find >> 32) - 1];
                gap += strings[whole].size() - static_cast<uint32_t>(find) - strings[holder].size();
                holder = whole;
            }
            sharing.holders[part] = holder;
            sharing.gaps[part] = safeCast<uint32_t>(gap);
        }
    });
}

static Vector<uint32_t> shareFrontsOfKeptTexts(TextsOfStrings strings, size_t numberOfStringsAsRead, BitVector& hasText, const BitVector& isName, TextSharing& sharing)
{
    HashMap<const uint8_t*, uint32_t> longestAtAddresses;
    auto forEachInternedText = [&](const auto& functor) {
        for (size_t index = hasText.findBit(numberOfStringsAsRead, true); index < hasText.size(); index = hasText.findBit(index + 1, true)) {
            if (!strings[static_cast<uint32_t>(index)].empty())
                functor(static_cast<uint32_t>(index));
        }
    };
    forEachInternedText([&](uint32_t index) {
        uint32_t& longest = longestAtAddresses.add(strings[index].data(), index).iterator->value;
        if (strings[index].size() > strings[longest].size())
            longest = index;
    });
    Vector<uint32_t> fronts;
    forEachInternedText([&](uint32_t index) {
        uint32_t longest = longestAtAddresses.get(strings[index].data());
        if (longest == index || (isName.quickGet(index) && !isName.quickGet(longest)))
            return;
        sharing.holders[index] = longest;
        sharing.gaps[index] = safeCast<uint32_t>(strings[longest].size() - strings[index].size());
        fronts.append(index);
    });
    for (uint32_t index : fronts)
        hasText.quickClear(index);
    return fronts;
}

static void measureTexts(const ParallelLoops& loops, const Typed::StringTable& programStrings, const BitVector& isRetained, RetainedStrings& strings)
{
    static_assert(!(indicesInRangeOfLoop % 64), "a range of the loop shares no word of a bit vector with another range");
    size_t numberOfStrings = programStrings.size();
    strings.measured.grow(numberOfStrings);
    strings.sharing.holders.grow(numberOfStrings);
    strings.sharing.gaps.grow(numberOfStrings);
    strings.leastFinds.grow(numberOfStrings);
    strings.firstPlacesOfHolders.grow(numberOfStrings);
    strings.decodedStringsOfRanges = Vector<Vector<DecodedString>>(numberOfRangesOfIndices(numberOfStrings));
    for (BitVector& hasText : strings.hasTextOfWidth)
        hasText.ensureSize(numberOfStrings);
    forEachRangeOfIndicesInParallel(loops, numberOfStrings, [&](size_t first, size_t end) {
        Vector<DecodedString>& decodedStrings = strings.decodedStringsOfRanges[first / indicesInRangeOfLoop];
        for (size_t index = first; index < end; ++index) {
            MeasuredTextOfString& measured = strings.measured[index];
            strings.sharing.holders[index] = static_cast<uint32_t>(index);
            strings.sharing.gaps[index] = 0;
            strings.leastFinds[index] = 0;
            strings.firstPlacesOfHolders[index] = RetainedStrings::noPlace;
            if (!isRetained.quickGet(index)) {
                measured = { nullptr, 0, 0 };
                continue;
            }
            std::span<const uint8_t> programString = programStrings[StringID(index)];
            if (charactersAreAllASCII(byteCast<Latin1Character>(programString)))
                measured = { programString.data(), static_cast<uint32_t>(programString.size()), StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(programString)) };
            else {
                decodedStrings.append(decodeForImage(programString));
                const DecodedString& decoded = decodedStrings.last();
                measured = { decoded.bytes().data(), decoded.length(), decoded.hash | (decoded.is16Bit() ? ImageString::is16BitFlag : 0) };
            }
            strings.hasTextOfWidth[measured.is16Bit()].quickSet(index);
        }
    });
}

static void shareText(const ParallelLoops& loops, RetainedStrings& retained, size_t numberOfStringsAsRead, StepTimes& times)
{
    TextsOfStrings strings { retained.measured.span() };
    const BitVector& isName = retained.isName;
    TextSharing& sharing = retained.sharing;
    auto& holders = sharing.holders;

    for (size_t unitSize : { sizeof(uint8_t), sizeof(char16_t) }) {
        BitVector& hasTextOfWidth = retained.hasTextOfWidth[unitSize == sizeof(char16_t)];
        if (hasTextOfWidth.isEmpty())
            continue;
        Vector<uint32_t> fronts = shareFrontsOfKeptTexts(strings, numberOfStringsAsRead, hasTextOfWidth, isName, sharing);
        shareTails(loops, strings, hasTextOfWidth, isName, holders);
        times.didFinish("share tails"_s);
        IndexOfTextHeads heads = buildHeadIndex(loops, strings, hasTextOfWidth, isName, holders);
        times.didFinish("build the index of heads"_s);
        findInnerParts(loops, strings, isName, heads, unitSize, sharing, retained.leastFinds);
        times.didFinish("find inner parts"_s);
        forEachRangeOfIndicesInParallel(loops, holders.size(), [&](size_t first, size_t end) {
            for (size_t index = hasTextOfWidth.findBit(first, true); index < end; index = hasTextOfWidth.findBit(index + 1, true)) {
                uint32_t holder = holders[index];
                if (holders[holder] == holder)
                    continue;
                sharing.gaps[index] = sharing.gaps[holder];
                holders[index] = holders[holder];
            }
        });
        for (uint32_t index : fronts) {
            uint32_t holder = holders[index];
            sharing.gaps[index] += sharing.gaps[holder];
            holders[index] = holders[holder];
        }
    }

}

template<typename Offset>
static constexpr Offset alignForTextOf16Bits(Offset offset)
{
    return static_cast<Offset>(offset + offset % sizeof(char16_t));
}

static void findFirstPlacesOfHolders(const ParallelLoops& loops, RetainedStrings& strings)
{
    forEachRangeOfIndicesInParallel(loops, strings.inOrderOfText.size(), [&](size_t first, size_t end) {
        for (size_t place = first; place < end; ++place) {
            AtomicRef firstPlace(strings.firstPlacesOfHolders[strings.sharing.holders[strings.inOrderOfText[place].index()]]);
            uint32_t known = firstPlace.load(std::memory_order_relaxed);
            while (place < known && !firstPlace.compare_exchange_weak(known, static_cast<uint32_t>(place), std::memory_order_relaxed)) { }
        }
    });
}

static void measureHoldersOfTextChunk(RetainedStrings& strings, unsigned chunkIndex)
{
    uint64_t& size = strings.chunks[chunkIndex].sizeOfText;
    strings.forEachHolderInChunk(chunkIndex, [&](unsigned index) {
        const MeasuredTextOfString& string = strings.measured[index];
        if (string.is16Bit())
            size = alignForTextOf16Bits(size);
        size += string.sizeInBytes();
    });
}

static constexpr uint64_t sizeOfTextInFrontOfChunks = 1;

[[nodiscard]] static bool placeChunksOfText(RetainedStrings& strings, Vector<uint8_t>& text, BuildContext& context)
{
    uint64_t size = sizeOfTextInFrontOfChunks;
    for (ChunkOfEncodedText& chunk : strings.chunks) {
        chunk.startOfText = alignForTextOf16Bits(size);
        size = chunk.startOfText + chunk.sizeOfText;
    }
    if (!checkSectionSize(size, context))
        return false;
    text.grow(static_cast<size_t>(size));
    zeroSpan(text.mutableSpan().first(sizeOfTextInFrontOfChunks));
    return true;
}

static void writeTextOfChunk(RetainedStrings& strings, unsigned chunkIndex, std::span<uint8_t> text)
{
    const ChunkOfEncodedText& chunk = strings.chunks[chunkIndex];
    uint64_t endOfTextInFront = chunkIndex ? strings.chunks[chunkIndex - 1].startOfText + strings.chunks[chunkIndex - 1].sizeOfText : sizeOfTextInFrontOfChunks;
    zeroSpan(text.subspan(endOfTextInFront, chunk.startOfText - endOfTextInFront));
    uint32_t end = static_cast<uint32_t>(chunk.startOfText);
    strings.forEachHolderInChunk(chunkIndex, [&](unsigned index) {
        const MeasuredTextOfString& string = strings.measured[index];
        uint32_t offset = string.is16Bit() ? alignForTextOf16Bits(end) : end;
        if (offset != end)
            text[end] = 0;
        end = offset + static_cast<uint32_t>(string.sizeInBytes());
        memcpySpan(text.subspan(offset, end - offset), string.bytes());
        strings.endsOfText[index] = end;
    });
    RELEASE_ASSERT(end - chunk.startOfText == chunk.sizeOfText);
}

static void writeStringRecordsOfChunk(const TextInputs& inputs, RetainedStrings& strings, unsigned chunkIndex, std::span<const uint8_t> text, std::span<ImageString> records)
{
    const ImageNumbers& numbers = inputs.numbers;
    ChunkOfEncodedText& chunk = strings.chunks[chunkIndex];
    struct FrontFoundInText {
        uint32_t offset;
        size_t numberOfBytes;
    };
    HashMap<const uint8_t*, FrontFoundInText> frontsOfKeptTexts;
    size_t numberOfStringsAsRead = inputs.program.strings.numberOfStringsAsRead();
    strings.forEachInChunk(chunkIndex, [&](unsigned index) {
        StringID id(index);
        const MeasuredTextOfString& string = strings.measured[index];
        uint32_t end = strings.endsOfText[strings.sharing.holders[index]];
        uint64_t distanceToEnd = strings.sharing.gaps[index] + string.sizeInBytes();
        RELEASE_ASSERT(end > distanceToEnd);
        uint32_t offset = end - static_cast<uint32_t>(distanceToEnd);
        RELEASE_ASSERT(!string.is16Bit() || !(offset % sizeof(char16_t)));
        size_t numberOfBytesFound = 0;
        if (index >= numberOfStringsAsRead && string.length) {
            FrontFoundInText& front = frontsOfKeptTexts.add(string.characters, FrontFoundInText { offset, 0 }).iterator->value;
            if (front.offset != offset)
                front = { offset, 0 };
            numberOfBytesFound = std::min(std::exchange(front.numberOfBytes, std::max(front.numberOfBytes, string.bytes().size())), string.bytes().size());
        }
        RELEASE_ASSERT(equalSpans(text.subspan(offset + numberOfBytesFound, string.bytes().size() - numberOfBytesFound), string.bytes().subspan(numberOfBytesFound)));

        bool isName = strings.isName.quickGet(index);
        bool isAtom = isName || numbers.atomsOfStrings().get(index);
        bool isConstant = numbers.stringConstants().numberOf(id).has_value();
        bool isNamed = numbers.namedStrings().get(index);
        bool isReadThroughHeader = isAtom || (isNamed && !numbers.stringsOnlyShown().get(index));
        bool hasHeader = isReadThroughHeader || (!isConstant && !isNamed);
        uint32_t hashAndFlags = string.hash();
        if (std::optional<uint32_t> identifierNumber = numbers.identifiers().numberOfText(id)) {
            RELEASE_ASSERT(*identifierNumber <= ImageString::hashMask);
            hashAndFlags = ImageString::hasIdentifierNumberFlag | *identifierNumber;
        }
        if (!hasHeader)
            hashAndFlags |= ImageString::hasNoHeaderFlag;
        if (isConstant && !hasHeader)
            hashAndFlags |= ImageString::hasListedHashFlag;
        if (string.is16Bit())
            hashAndFlags |= ImageString::is16BitFlag;
        if (isAtom)
            hashAndFlags |= ImageString::isAtomFlag;

        ImageLongString range { offset, string.length };
        uint32_t location = offset << ImageString::lengthBits | string.length;
        if (!hasHeader || !ImageListedString::fitsOneWord(range)) {
            location = 0;
            chunk.longStrings.append({ index, range });
        }
        records[index] = ImageString { location, hashAndFlags };
    });
}

[[nodiscard]] static bool numberLongStringsOfText(RetainedStrings& strings, std::span<ImageString> records, Vector<uint8_t>& bytesOfLongStrings, BuildContext& context)
{
    uint64_t count = 1;
    for (ChunkOfEncodedText& chunk : strings.chunks) {
        chunk.firstLongString = static_cast<uint32_t>(count);
        count += chunk.longStrings.size();
    }
    if (count > context.limit(LimitID::MaxListedStrings)) {
        context.reportLimitExceeded(LimitID::MaxListedStrings, count);
        return false;
    }
    bytesOfLongStrings.grow(static_cast<size_t>(count) * sizeof(ImageLongString));
    std::span<ImageLongString> longStrings = spanReinterpretCast<ImageLongString>(bytesOfLongStrings.mutableSpan());
    longStrings[0] = ImageLongString { 0, 0 };
    forEachIndexInParallel(context.parallelLoops(), strings.division.size(), [&](unsigned chunkIndex) {
        const ChunkOfEncodedText& chunk = strings.chunks[chunkIndex];
        for (uint32_t place = 0; place < chunk.longStrings.size(); ++place) {
            uint32_t number = chunk.firstLongString + place;
            longStrings[number] = chunk.longStrings[place].range;
            records[chunk.longStrings[place].string].location = number << ImageString::lengthBits;
        }
    });
    return true;
}

static uint32_t hashOrSymbolOf(const Typed::PropertyKey& key, uint32_t textHash)
{
    switch (key.kind) {
    case PropertyKeyKind::WellKnownSymbol:
        RELEASE_ASSERT(key.symbol);
        return static_cast<uint32_t>(*key.symbol);
    case PropertyKeyKind::Text:
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return textHash;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ImageIdentifier encodeIdentifier(const Typed::Program& program, const NumberedIdentifier& identifier, std::span<const MeasuredTextOfString> measured)
{
    uint32_t textHash = identifier.text ? measured[identifier.text.index()].hash() : 0;
    if (!identifier.key) {
        RELEASE_ASSERT(identifier.text);
        return ImageIdentifier::create(identifier.text, textHash, PropertyKeyKind::Text, program.nameClass(identifier.text));
    }
    const Typed::PropertyKey& key = program.at(identifier.key);
    return ImageIdentifier::create(identifier.text, hashOrSymbolOf(key, textHash), key.kind, program.nameClassOf(identifier.key));
}

EncodedText encodeText(const TextInputs& inputs, BuildContext& context, NOESCAPE const ScopedLambda<bool()>& waitForNumbersOfStrings)
{
    static_assert(std::has_unique_object_representations_v<ImageString> && std::has_unique_object_representations_v<ImageLongString> && std::has_unique_object_representations_v<ImageIdentifier>, "padding would make the bytes of the image differ from build to build");
    const Typed::StringTable& programStrings = inputs.program.strings;
    const ParallelLoops& loops = context.parallelLoops();
    StepTimes times(context, "image: text"_s);
    auto numberOfStrings = static_cast<unsigned>(programStrings.size());
    const uint64_t sizeOfLargestList = static_cast<uint64_t>(programStrings.size()) * sizeof(TextInOrder);

    RetainedStrings strings;
    if (sizeOfLargestList <= context.limit(LimitID::VectorBytes) && sizeOfLargestList <= context.limit(LimitID::TableBytes)) {
        strings.isName = inputs.numbers.textsOfNames();
        strings.isName.ensureSize(numberOfStrings);
        BitVector isRetained = strings.isName;
        isRetained.merge(inputs.numbers.otherStringsWithText());
        measureTexts(loops, programStrings, isRetained, strings);
        times.didFinish("measure"_s);
        shareText(loops, strings, programStrings.numberOfStringsAsRead(), times);
    }
    if (!waitForNumbersOfStrings())
        return { };
    times.didFinish("wait for the numbers of strings"_s);

    const IdentifierNumbering& identifiers = inputs.numbers.identifiers();
    const StringConstantNumbering& constants = inputs.numbers.stringConstants();
    if (identifiers.count() > context.limit(LimitID::MaxIdentifiers)) {
        context.reportLimitExceeded(LimitID::MaxIdentifiers, identifiers.count());
        return { };
    }
    if (!checkSectionSize(sizeOfLargestList, context))
        return { };

    strings.inOrderOfText = inputs.numbers.stringsInOrderOfText();
    findFirstPlacesOfHolders(loops, strings);
    strings.endsOfText.grow(numberOfStrings);
    strings.division = divideIntoChunks(static_cast<unsigned>(strings.inOrderOfText.size()));
    strings.chunks = Vector<ChunkOfEncodedText>(strings.division.size());
    forEachIndexInParallel(loops, strings.division.size(), [&](unsigned chunkIndex) {
        measureHoldersOfTextChunk(strings, chunkIndex);
    });

    EncodedText result;
    if (!placeChunksOfText(strings, result.text, context))
        return { };
    forEachIndexInParallel(loops, strings.division.size(), [&](unsigned chunkIndex) {
        writeTextOfChunk(strings, chunkIndex, result.text.mutableSpan());
    });
    times.didFinish("place and write the text"_s);
    result.strings.grow(static_cast<size_t>(numberOfStrings) * sizeof(ImageString));
    std::span<ImageString> records = spanReinterpretCast<ImageString>(result.strings.mutableSpan());
    forEachRangeOfIndicesInParallel(loops, numberOfStrings, [&](size_t first, size_t end) {
        zeroSpan(records.subspan(first, end - first));
    });
    forEachIndexInParallel(loops, strings.division.size(), [&](unsigned chunkIndex) {
        writeStringRecordsOfChunk(inputs, strings, chunkIndex, result.text.span(), records);
    });
    if (!numberLongStringsOfText(strings, records, result.longStrings, context))
        return { };
    result.identifiers.grow(static_cast<size_t>(identifiers.count()) * sizeof(ImageIdentifier));
    std::span<ImageIdentifier> encodedIdentifiers = spanReinterpretCast<ImageIdentifier>(result.identifiers.mutableSpan());
    forEachRangeOfIndicesInParallel(loops, identifiers.count(), [&](size_t first, size_t end) {
        for (size_t number = first; number < end; ++number)
            encodedIdentifiers[number] = number ? encodeIdentifier(inputs.program, identifiers.at(static_cast<uint32_t>(number)), strings.measured.span()) : ImageIdentifier { 0, 0 };
    });
    size_t numberOfConstants = constants.count();
    result.stringConstants.grow((numberOfConstants + constants.numberOfPlaced()) * sizeof(uint32_t));
    std::span<uint32_t> stringsOfConstants = spanReinterpretCast<uint32_t>(result.stringConstants.mutableSpan());
    for (size_t number = 0; number < numberOfConstants; ++number)
        stringsOfConstants[number] = constants.stringOf(static_cast<uint32_t>(number)).index();
    times.didFinish("write the records"_s);
    destroyInBackground(context.backgroundJobs(), WTF::move(strings));
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
