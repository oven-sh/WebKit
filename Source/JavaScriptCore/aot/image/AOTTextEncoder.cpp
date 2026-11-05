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
static constexpr unsigned textHeadSlotBits = 22;
static constexpr uint64_t maxComparedCharactersPerCharacter = 1024;

namespace {

struct MeasuredTextOfString {
    bool is16Bit() const { return hashAndWidth & ImageString::is16BitFlag; }
    uint32_t hash() const { return hashAndWidth & ImageString::hashMask; }
    uint64_t sizeInBytes() const { return static_cast<uint64_t>(length) * (is16Bit() ? sizeof(char16_t) : sizeof(uint8_t)); }
    std::span<const uint8_t> bytes() const { return { characters, static_cast<size_t>(sizeInBytes()) }; }

    const uint8_t* characters { nullptr };
    uint32_t length { 0 };
    uint32_t hashAndWidth { 0 };
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
static_assert(sizeof(TextInOrder) >= sizeof(MeasuredTextOfString) && sizeof(TextInOrder) >= sizeof(std::pair<uint64_t, uint32_t>) && sizeof(TextInOrder) >= sizeof(ImageString));

struct TextHeadRange {
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
        size_t mask = headRanges.size() - 1;
        for (size_t slot = static_cast<size_t>(head * 0x9e3779b97f4a7c15ULL >> headRangeShift);; slot = (slot + 1) & mask) {
            const TextHeadRange& range = headRanges[slot];
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
    Vector<DecodedString> decodedStrings;
};

struct RetainedStrings {
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
    BitVector isRetained;
    std::span<const StringID> inOrderOfText;
    Vector<MeasuredTextOfString> measured;
    TextSharing sharing;
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

static void sortTextsByPrefixThenBy(const ParallelLoops& loops, Vector<uint32_t>& list, const auto& prefixOf, const auto& isLess)
{
    Vector<std::pair<uint64_t, uint32_t>> withPrefixes;
    withPrefixes.grow(list.size());
    forEachIndexInParallel(loops, static_cast<unsigned>(list.size()), [&](unsigned place) {
        withPrefixes[place] = { prefixOf(list[place]), list[place] };
    });
    parallelSort(loops, withPrefixes.mutableSpan(), [&](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : isLess(a.second, b.second);
    });
    for (size_t place = 0; place < list.size(); ++place)
        list[place] = withPrefixes[place].second;
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

static constexpr size_t maxSizeOfTextBeginning = 2 * textHeadSize;
static constexpr unsigned textBeginningSlotBits = 23;

static size_t slotOfTextBeginning(uint32_t head, uint64_t afterHead, size_t size)
{
    uint64_t charactersAfterHead = afterHead >> (8 * (textHeadSize + sizeof(uint64_t) - size));
    return static_cast<size_t>(((charactersAfterHead << 32 | head) ^ size << 59) * 0x9e3779b97f4a7c15ULL >> (64 - textBeginningSlotBits));
}

static size_t slotOfTextHead(uint32_t head)
{
    return static_cast<size_t>((static_cast<uint64_t>(head) + 1) * 0x9e3779b97f4a7c15ULL >> (64 - textHeadSlotBits));
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

static void shareTails(const ParallelLoops& loops, TextsOfStrings strings, const BitVector& hasText, const BitVector& isName, Vector<uint32_t>& holders)
{
    Vector<uint32_t> byLastCharacters;
    for (size_t index : hasText)
        byLastCharacters.append(static_cast<uint32_t>(index));
    sortTextsByPrefixThenBy(loops, byLastCharacters, [&](uint32_t index) {
        auto text = strings[index];
        if (text.size() >= sizeof(uint64_t))
            return load64(text.data() + text.size() - sizeof(uint64_t));
        return prefixOfTextFrom(text.rbegin(), text.rend());
    }, [&](uint32_t a, uint32_t b) {
        return endOrdersBefore(strings[a], strings[b], a, b);
    });
    Vector<uint8_t> isTailOfNext(FillWith { }, byLastCharacters.size(), 0);
    forEachIndexInParallel(loops, static_cast<unsigned>(byLastCharacters.size() - 1), [&](unsigned place) {
        auto tail = strings[byLastCharacters[place]];
        auto whole = strings[byLastCharacters[place + 1]];
        isTailOfNext[place] = tail.size() <= whole.size() && equalSpans(tail, whole.last(tail.size()));
    });
    for (size_t place = byLastCharacters.size() - 1; place--;) {
        if (!isTailOfNext[place])
            continue;
        uint32_t tail = byLastCharacters[place];
        uint32_t holder = holders[byLastCharacters[place + 1]];
        if (!isName.quickGet(tail) || isName.quickGet(holder))
            holders[tail] = holder;
    }
}

static IndexOfTextHeads buildHeadIndex(const ParallelLoops& loops, TextsOfStrings strings, const BitVector& hasText, const Vector<uint32_t>& holders)
{
    Vector<uint32_t> inOrder;
    for (size_t bit : hasText) {
        uint32_t index = static_cast<uint32_t>(bit);
        if (holders[index] == index && strings[index].size() >= textHeadSize)
            inOrder.append(index);
    }
    sortTextsByPrefixThenBy(loops, inOrder, [&](uint32_t index) {
        auto text = strings[index];
        if (text.size() >= sizeof(uint64_t))
            return std::byteswap(load64(text.data()));
        return prefixOfTextFrom(text.begin(), text.end());
    }, [&](uint32_t a, uint32_t b) {
        auto textOfA = strings[a];
        auto textOfB = strings[b];
        int order = memcmp(textOfA.data(), textOfB.data(), std::min(textOfA.size(), textOfB.size()));
        if (order)
            return order < 0;
        return textOfA.size() != textOfB.size() ? textOfA.size() < textOfB.size() : a < b;
    });

    IndexOfTextHeads result;
    size_t numberOfTexts = inOrder.size();
    result.textsInOrder.grow(numberOfTexts);
    result.afterHeads.grow(numberOfTexts);
    forEachRangeOfIndicesInParallel(loops, numberOfTexts, [&](size_t first, size_t end) {
        for (size_t position = first; position < end; ++position) {
            auto text = strings[inOrder[position]];
            uint64_t afterHead = prefixAfterTextHead(text);
            result.textsInOrder[position] = { text.data(), safeCast<uint32_t>(text.size()), inOrder[position], afterHead };
            result.afterHeads[position] = afterHead;
        }
    });

    result.longestHeads.grow(numberOfTexts);
    result.mayBeBeginning.fill(0, (1 << textBeginningSlotBits) / 64);
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
                size_t sizeOfBeginning = std::min<size_t>(text.size, maxSizeOfTextBeginning);
                sizesOfBeginnings |= 1u << (sizeOfBeginning - textHeadSize);
                if (sizeOfBeginning > textHeadSize) {
                    size_t slot = slotOfTextBeginning(head, text.afterHead, sizeOfBeginning);
                    AtomicRef(result.mayBeBeginning[slot / 64]).fetch_or(1ull << slot % 64, std::memory_order_relaxed);
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
    for (const Vector<TextHeadRange>& headRanges : headRangesOfLoopRanges) {
        for (const TextHeadRange& range : headRanges) {
            size_t slot = static_cast<size_t>(range.head * 0x9e3779b97f4a7c15ULL >> result.headRangeShift);
            while (!result.headRanges[slot].isEmpty())
                slot = (slot + 1) & (numberOfSlots - 1);
            result.headRanges[slot] = range;
            size_t slotOfHead = slotOfTextHead(range.head);
            result.mayBeHead[slotOfHead / 64] |= 1ull << slotOfHead % 64;
        }
    }
    Vector<uint64_t> lengthsAndNumbers(inOrder.size(), [&](size_t position) {
        return static_cast<uint64_t>(std::numeric_limits<uint32_t>::max() - result.textsInOrder[position].size) << 32 | inOrder[position];
    });
    parallelSort(loops, lengthsAndNumbers.mutableSpan(), std::less<uint64_t> { });
    result.fromLongest = Vector<uint32_t>(lengthsAndNumbers.size(), [&](size_t place) {
        return static_cast<uint32_t>(lengthsAndNumbers[place]);
    });
    return result;
}

static bool textOrdersBefore(std::span<const uint8_t> a, uint64_t afterHeadOfA, const TextInOrder& b, uint64_t& charactersCompared)
{
    constexpr size_t recordedSize = textHeadSize + sizeof(uint64_t);
    size_t shorterSize = std::min<size_t>(a.size(), b.size);
    uint64_t difference = afterHeadOfA ^ b.afterHead;
    size_t same = textHeadSize + (difference ? std::countl_zero(difference) / 8 : sizeof(uint64_t));
    if (same < std::min(shorterSize, recordedSize)) {
        charactersCompared += same;
        return afterHeadOfA < b.afterHead;
    }
    if (shorterSize <= recordedSize) {
        charactersCompared += shorterSize;
        return b.size > shorterSize;
    }
    for (same = recordedSize; shorterSize - same >= sizeof(uint64_t); same += sizeof(uint64_t)) {
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

static uint32_t findLastTextNotAfter(const IndexOfTextHeads& heads, const TextHeadRange& range, std::span<const uint8_t> rest, uint64_t afterHeadOfRest, uint64_t& charactersCompared)
{
    const uint64_t* afterHeads = heads.afterHeads.span().data();
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
            if (textOrdersBefore(rest, afterHeadOfRest, heads.textsInOrder[middle], charactersCompared))
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

static void findInnerParts(const ParallelLoops& loops, TextsOfStrings strings, const BitVector& isName, const IndexOfTextHeads& heads, size_t unitSize, TextSharing& sharing)
{
    Vector<uint64_t> leastFinds(FillWith { }, sharing.holders.size(), 0);
    forEachIndexInParallel(loops, static_cast<unsigned>(heads.fromLongest.size()), [&](unsigned place) {
        uint32_t whole = heads.fromLongest[place];
        auto text = strings[whole];
        bool wholeIsName = isName.quickGet(whole);
        uint64_t maxCharactersCompared = maxComparedCharactersPerCharacter * static_cast<uint64_t>(text.size());
        uint64_t charactersCompared = 0;
        const uint64_t* mayBeHead = heads.mayBeHead.span().data();
        const uint8_t* characters = text.data();
        size_t sizeOfText = text.size();
        size_t step = unitSize;
        for (size_t at = 0; at + textHeadSize <= sizeOfText && charactersCompared <= maxCharactersCompared; at += step) {
            uint32_t head = textHeadAt(characters + at);
            size_t slotOfHead = slotOfTextHead(head);
            if (!(mayBeHead[slotOfHead / 64] >> slotOfHead % 64 & 1))
                continue;
            const TextHeadRange* range = heads.findRange(head);
            if (!range)
                continue;
            auto rest = text.subspan(at);
            uint64_t afterHeadOfRest = prefixAfterTextHead(rest);
            bool mayStartWithText = range->sizesOfBeginnings & 1;
            for (uint32_t sizes = range->sizesOfBeginnings >> 1; sizes && !mayStartWithText; sizes &= sizes - 1) {
                size_t size = textHeadSize + 1 + std::countr_zero(sizes);
                if (size > rest.size())
                    break;
                size_t slot = slotOfTextBeginning(head, afterHeadOfRest, size);
                mayStartWithText = heads.mayBeBeginning[slot / 64] >> slot % 64 & 1;
            }
            if (!mayStartWithText)
                continue;
            uint64_t find = (static_cast<uint64_t>(place) + 1) << 32 | at;
            bool restStartsWithCandidate = false;
            for (uint32_t position = findLastTextNotAfter(heads, *range, rest, afterHeadOfRest, charactersCompared); position != IndexOfTextHeads::noText; position = heads.longestHeads[position]) {
                const TextInOrder& candidate = heads.textsInOrder[position];
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
    forEachIndexInParallel(loops, static_cast<unsigned>(heads.fromLongest.size()), [&](unsigned place) {
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

static TextSharing shareText(const ParallelLoops& loops, std::span<const MeasuredTextOfString> measured, size_t numberOfStringsAsRead, const BitVector& hasText, const BitVector& isName, StepTimes& times)
{
    TextsOfStrings strings { measured };
    TextSharing sharing;
    auto& holders = sharing.holders;
    holders = Vector<uint32_t>(measured.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    sharing.gaps.fill(0, measured.size());

    for (size_t unitSize : { sizeof(uint8_t), sizeof(char16_t) }) {
        BitVector hasTextOfWidth(measured.size());
        for (size_t index : hasText) {
            if (measured[index].is16Bit() == (unitSize == sizeof(char16_t)))
                hasTextOfWidth.quickSet(index);
        }
        if (hasTextOfWidth.isEmpty())
            continue;
        Vector<uint32_t> fronts = shareFrontsOfKeptTexts(strings, numberOfStringsAsRead, hasTextOfWidth, isName, sharing);
        shareTails(loops, strings, hasTextOfWidth, isName, holders);
        times.didFinish("share tails"_s);
        IndexOfTextHeads heads = buildHeadIndex(loops, strings, hasTextOfWidth, holders);
        times.didFinish("build the index of heads"_s);
        findInnerParts(loops, strings, isName, heads, unitSize, sharing);
        times.didFinish("find inner parts"_s);
        for (size_t index : hasTextOfWidth) {
            uint32_t holder = holders[index];
            if (holders[holder] == holder)
                continue;
            sharing.gaps[index] = sharing.gaps[holder];
            holders[index] = holders[holder];
        }
        for (uint32_t index : fronts) {
            uint32_t holder = holders[index];
            sharing.gaps[index] += sharing.gaps[holder];
            holders[index] = holders[holder];
        }
    }
    return sharing;
}

template<typename Offset>
static constexpr Offset alignForTextOf16Bits(Offset offset)
{
    return static_cast<Offset>(offset + offset % sizeof(char16_t));
}

static void measureTextOfChunk(const Typed::StringTable& programStrings, RetainedStrings& strings, unsigned chunkIndex)
{
    Vector<DecodedString>& decodedStrings = strings.chunks[chunkIndex].decodedStrings;
    strings.forEachInChunk(chunkIndex, [&](unsigned index) {
        std::span<const uint8_t> programString = programStrings[StringID(index)];
        if (charactersAreAllASCII(byteCast<Latin1Character>(programString))) {
            strings.measured[index] = { programString.data(), static_cast<uint32_t>(programString.size()), StringHasher::computeHashAndMaskTop8Bits(byteCast<Latin1Character>(programString)) };
            return;
        }
        decodedStrings.append(decodeForImage(programString));
        const DecodedString& decoded = decodedStrings.last();
        strings.measured[index] = { decoded.bytes().data(), decoded.length(), decoded.hash | (decoded.is16Bit() ? ImageString::is16BitFlag : 0) };
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

[[nodiscard]] static bool placeChunksOfText(RetainedStrings& strings, Vector<uint8_t>& text, BuildContext& context)
{
    uint64_t size = 1;
    for (ChunkOfEncodedText& chunk : strings.chunks) {
        chunk.startOfText = alignForTextOf16Bits(size);
        size = chunk.startOfText + chunk.sizeOfText;
    }
    if (!checkSectionSize(size, context))
        return false;
    text = Vector<uint8_t>(FillWith { }, static_cast<size_t>(size), 0);
    return true;
}

static void writeTextOfChunk(RetainedStrings& strings, unsigned chunkIndex, std::span<uint8_t> text)
{
    const ChunkOfEncodedText& chunk = strings.chunks[chunkIndex];
    uint32_t end = static_cast<uint32_t>(chunk.startOfText);
    strings.forEachHolderInChunk(chunkIndex, [&](unsigned index) {
        const MeasuredTextOfString& string = strings.measured[index];
        uint32_t offset = string.is16Bit() ? alignForTextOf16Bits(end) : end;
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
    strings.forEachInChunk(chunkIndex, [&](unsigned index) {
        StringID id(index);
        const MeasuredTextOfString& string = strings.measured[index];
        uint32_t end = strings.endsOfText[strings.sharing.holders[index]];
        uint64_t distanceToEnd = strings.sharing.gaps[index] + string.sizeInBytes();
        RELEASE_ASSERT(end > distanceToEnd);
        uint32_t offset = end - static_cast<uint32_t>(distanceToEnd);
        RELEASE_ASSERT(!string.is16Bit() || !(offset % sizeof(char16_t)));
        RELEASE_ASSERT(equalSpans(text.subspan(offset, string.bytes().size()), string.bytes()));

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

[[nodiscard]] static bool numberLongStringsOfText(RetainedStrings& strings, std::span<ImageString> records, Vector<ImageLongString>& longStrings, BuildContext& context)
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
    longStrings = Vector<ImageLongString>(FillWith { }, static_cast<size_t>(count), ImageLongString { 0, 0 });
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

EncodedText encodeText(const TextInputs& inputs, BuildContext& context)
{
    const Typed::StringTable& programStrings = inputs.program.strings;
    const IdentifierNumbering& identifiers = inputs.numbers.identifiers();
    const StringConstantNumbering& constants = inputs.numbers.stringConstants();
    const ParallelLoops& loops = context.parallelLoops();
    StepTimes times(context, "image: text"_s);
    if (identifiers.count() > context.limit(LimitID::MaxIdentifiers)) {
        context.reportLimitExceeded(LimitID::MaxIdentifiers, identifiers.count());
        return { };
    }
    if (!checkSectionSize(static_cast<uint64_t>(programStrings.size()) * sizeof(TextInOrder), context))
        return { };

    auto numberOfStrings = static_cast<unsigned>(programStrings.size());
    RetainedStrings strings;
    strings.isName.ensureSize(numberOfStrings);
    for (uint32_t number = 1; number < identifiers.count(); ++number) {
        if (StringID text = identifiers.at(number).text)
            strings.isName.quickSet(text.index());
    }
    strings.inOrderOfText = inputs.numbers.stringsInOrderOfText();
    strings.isRetained.ensureSize(numberOfStrings);
    for (StringID string : strings.inOrderOfText)
        strings.isRetained.quickSet(string.index());
    strings.measured = Vector<MeasuredTextOfString>(FillWith { }, numberOfStrings, MeasuredTextOfString { });
    strings.endsOfText = Vector<uint32_t>(FillWith { }, numberOfStrings, 0);
    strings.division = divideIntoChunks(static_cast<unsigned>(strings.inOrderOfText.size()));
    strings.chunks = Vector<ChunkOfEncodedText>(strings.division.size());
    forEachIndexInParallel(loops, strings.division.size(), [&](unsigned chunkIndex) {
        measureTextOfChunk(programStrings, strings, chunkIndex);
    });
    times.didFinish("measure"_s);
    strings.sharing = shareText(loops, strings.measured.span(), programStrings.numberOfStringsAsRead(), strings.isRetained, strings.isName, times);
    strings.firstPlacesOfHolders = Vector<uint32_t>(FillWith { }, numberOfStrings, 0);
    for (uint32_t place = static_cast<uint32_t>(strings.inOrderOfText.size()); place--;)
        strings.firstPlacesOfHolders[strings.sharing.holders[strings.inOrderOfText[place].index()]] = place;
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
    Vector<ImageString> records(FillWith { }, numberOfStrings, ImageString { 0, 0 });
    forEachIndexInParallel(loops, strings.division.size(), [&](unsigned chunkIndex) {
        writeStringRecordsOfChunk(inputs, strings, chunkIndex, result.text.span(), records.mutableSpan());
    });
    Vector<ImageLongString> longStrings;
    if (!numberLongStringsOfText(strings, records.mutableSpan(), longStrings, context))
        return { };
    result.strings = bytesOf(records);
    result.longStrings = bytesOf(longStrings);
    result.identifiers = bytesOf(Vector<ImageIdentifier>(identifiers.count(), [&](size_t number) {
        if (!number)
            return ImageIdentifier { 0, 0 };
        return encodeIdentifier(inputs.program, identifiers.at(static_cast<uint32_t>(number)), strings.measured.span());
    }));
    result.stringConstants = bytesOf(Vector<uint32_t>(2 * static_cast<size_t>(constants.count()), [&](size_t place) {
        return place < constants.count() ? 0 : constants.stringOf(static_cast<uint32_t>(place - constants.count())).index();
    }));
    times.didFinish("write the records"_s);
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
