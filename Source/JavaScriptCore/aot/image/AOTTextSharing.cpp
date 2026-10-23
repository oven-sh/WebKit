/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTextEncoder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <compare>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr size_t headSize = sizeof(uint32_t);
static constexpr unsigned headSlotBits = 22;

namespace {

class StringBytes {
public:
    explicit StringBytes(std::span<const DecodedString> strings)
        : m_strings(strings)
    {
    }

    size_t size() const { return m_strings.size(); }
    std::span<const uint8_t> operator[](uint32_t index) const { return m_strings[index].bytes(); }

private:
    std::span<const DecodedString> m_strings;
};

struct OrderedString {
    const uint8_t* characters;
    uint32_t size;
    uint32_t index;
    uint64_t afterHead;
};

struct HeadIndex {
    Vector<OrderedString> orderedStrings;
    Vector<std::optional<uint32_t>> longestHeads;
    UncheckedKeyHashMap<uint64_t, std::pair<uint32_t, uint32_t>> headRanges;
    BitVector mayBeHead;
    Vector<uint32_t> fromLongest;
};

struct InnerPartSearch {
    size_t unitSize;
    uint64_t maxComparedCharactersPerCharacter;
};

} // anonymous namespace

static void sortByPrefixThenBy(const ParallelLoops& loops, Vector<uint32_t>& list, const auto& prefixOf, const auto& isLess)
{
    Vector<std::pair<uint64_t, uint32_t>> withBytes(list.size(), [&](size_t i) {
        return std::pair { prefixOf(list[i]), list[i] };
    });
    parallelSort(loops, withBytes.mutableSpan(), [&](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : isLess(a.second, b.second);
    });
    for (size_t i = 0; i < list.size(); ++i)
        list[i] = withBytes[i].second;
}

static uint64_t prefixFrom(auto at, auto end)
{
    uint64_t result = 0;
    for (unsigned shift = 64; shift && at != end; ++at)
        result |= static_cast<uint64_t>(*at) << (shift -= 8);
    return result;
}

static uint64_t headAt(std::span<const uint8_t> text, size_t at)
{
    uint32_t head;
    memcpy(&head, text.data() + at, headSize);
    // 0 is the empty key of the hash map.
    return static_cast<uint64_t>(head) + 1;
}

static uint64_t prefixAfterHead(std::span<const uint8_t> text)
{
    if (text.size() >= headSize + sizeof(uint64_t)) {
        uint64_t bytes;
        memcpy(&bytes, text.data() + headSize, sizeof(bytes));
        static_assert(std::endian::native == std::endian::little);
        return std::byteswap(bytes);
    }
    uint64_t result = 0;
    size_t end = std::min(text.size(), headSize + sizeof(uint64_t));
    for (size_t at = headSize; at < end; ++at)
        result |= static_cast<uint64_t>(text[at]) << (8 * (headSize + sizeof(uint64_t) - 1 - at));
    return result;
}

static size_t headSlot(uint64_t head)
{
    return static_cast<size_t>(head * 0x9e3779b97f4a7c15ULL >> (64 - headSlotBits));
}

static void shareTails(const ParallelLoops& loops, StringBytes strings, const BitVector& hasText, const BitVector& isName, Vector<uint32_t>& holders)
{
    Vector<uint32_t> byLastCharacters;
    for (size_t index : hasText)
        byLastCharacters.append(static_cast<uint32_t>(index));
    sortByPrefixThenBy(loops, byLastCharacters, [&](uint32_t index) {
        return prefixFrom(strings[index].rbegin(), strings[index].rend());
    }, [&](uint32_t a, uint32_t b) {
        auto [endOfA, endOfB] = std::mismatch(strings[a].rbegin(), strings[a].rend(), strings[b].rbegin(), strings[b].rend());
        if (endOfA == strings[a].rend() || endOfB == strings[b].rend())
            return strings[a].size() != strings[b].size() ? strings[a].size() < strings[b].size() : a < b;
        return *endOfA < *endOfB;
    });
    for (size_t i = byLastCharacters.size(); i-- > 1;) {
        auto tail = strings[byLastCharacters[i - 1]];
        auto whole = strings[byLastCharacters[i]];
        if (tail.size() <= whole.size() && equalSpans(tail, whole.last(tail.size())) && (!isName.get(byLastCharacters[i - 1]) || isName.get(holders[byLastCharacters[i]])))
            holders[byLastCharacters[i - 1]] = holders[byLastCharacters[i]];
    }
}

static Vector<uint32_t> sortUnsharedStrings(const ParallelLoops& loops, StringBytes strings, const BitVector& hasText, const Vector<uint32_t>& holders)
{
    Vector<uint32_t> inOrder;
    for (size_t bit : hasText) {
        uint32_t index = static_cast<uint32_t>(bit);
        if (holders[index] == index && strings[index].size() >= headSize)
            inOrder.append(index);
    }
    sortByPrefixThenBy(loops, inOrder, [&](uint32_t index) {
        return prefixFrom(strings[index].begin(), strings[index].end());
    }, [&](uint32_t a, uint32_t b) {
        auto order = std::lexicographical_compare_three_way(strings[a].begin(), strings[a].end(), strings[b].begin(), strings[b].end());
        return order != 0 ? order < 0 : a < b;
    });
    return inOrder;
}

static HeadIndex buildHeadIndex(const ParallelLoops& loops, StringBytes strings, const BitVector& hasText, const Vector<uint32_t>& holders)
{
    Vector<uint32_t> inOrder = sortUnsharedStrings(loops, strings, hasText, holders);
    HeadIndex result;
    result.orderedStrings = Vector<OrderedString>(inOrder.size(), [&](size_t position) {
        auto text = strings[inOrder[position]];
        return OrderedString { text.data(), safeCast<uint32_t>(text.size()), inOrder[position], prefixAfterHead(text) };
    });
    result.longestHeads = Vector<std::optional<uint32_t>>(FillWith { }, inOrder.size(), std::nullopt);
    result.mayBeHead.ensureSize(1 << headSlotBits);
    Vector<uint32_t> open;
    for (uint32_t position = 0; position < inOrder.size(); ++position) {
        auto text = strings[inOrder[position]];
        while (!open.isEmpty() && !spanHasPrefix(text, strings[inOrder[open.last()]]))
            open.removeLast();
        if (!open.isEmpty())
            result.longestHeads[position] = open.last();
        open.append(position);
        result.headRanges.add(headAt(text, 0), std::pair { position, position }).iterator->value.second = position + 1;
        result.mayBeHead.quickSet(headSlot(headAt(text, 0)));
    }
    Vector<uint64_t> lengthsAndNumbers(inOrder.size(), [&](size_t position) {
        return static_cast<uint64_t>(std::numeric_limits<uint32_t>::max() - safeCast<uint32_t>(strings[inOrder[position]].size())) << 32 | inOrder[position];
    });
    parallelSort(loops, lengthsAndNumbers.mutableSpan(), std::less<uint64_t> { });
    result.fromLongest = Vector<uint32_t>(lengthsAndNumbers.size(), [&](size_t position) {
        return static_cast<uint32_t>(lengthsAndNumbers[position]);
    });
    return result;
}

static bool ordersBefore(std::span<const uint8_t> a, uint64_t afterHeadOfA, const OrderedString& b, uint64_t& charactersCompared)
{
    constexpr size_t recordedSize = headSize + sizeof(uint64_t);
    size_t shorterSize = std::min<size_t>(a.size(), b.size);
    uint64_t difference = afterHeadOfA ^ b.afterHead;
    size_t same = headSize + (difference ? std::countl_zero(difference) / 8 : sizeof(uint64_t));
    if (same < std::min(shorterSize, recordedSize)) {
        charactersCompared += same;
        return afterHeadOfA < b.afterHead;
    }
    if (shorterSize <= recordedSize) {
        charactersCompared += shorterSize;
        return b.size > shorterSize;
    }
    auto restOfA = a.subspan(recordedSize);
    std::span restOfB { b.characters + recordedSize, b.size - recordedSize };
    auto [endOfA, endOfB] = std::ranges::mismatch(restOfA, restOfB);
    charactersCompared += recordedSize + (endOfA - restOfA.begin());
    return endOfB != restOfB.end() && (endOfA == restOfA.end() || *endOfA < *endOfB);
}

static std::optional<uint32_t> findLastNotAfter(const HeadIndex& heads, std::span<const uint8_t> rest, uint64_t& charactersCompared)
{
    uint64_t head = headAt(rest, 0);
    if (!heads.mayBeHead.quickGet(headSlot(head)))
        return std::nullopt;
    auto found = heads.headRanges.find(head);
    if (found == heads.headRanges.end())
        return std::nullopt;
    uint64_t afterHeadOfRest = prefixAfterHead(rest);
    auto [first, afterLast] = found->value;
    uint32_t low = first;
    for (uint32_t high = afterLast; low < high;) {
        uint32_t middle = low + (high - low) / 2;
        if (ordersBefore(rest, afterHeadOfRest, heads.orderedStrings[middle], charactersCompared))
            high = middle;
        else
            low = middle + 1;
    }
    if (low == first)
        return std::nullopt;
    return low - 1;
}

static void findInnerParts(StringBytes strings, const BitVector& isName, const HeadIndex& heads, const InnerPartSearch& search, TextSharing& sharing)
{
    auto& holders = sharing.holders;
    auto& gaps = sharing.gaps;
    uint64_t charactersCompared = 0;
    for (uint32_t whole : heads.fromLongest) {
        if (holders[whole] != whole)
            continue;
        auto text = strings[whole];
        uint64_t maxCharactersCompared = charactersCompared + search.maxComparedCharactersPerCharacter * static_cast<uint64_t>(text.size());
        for (size_t at = 0; at + headSize <= text.size(); at += search.unitSize) {
            if (charactersCompared > maxCharactersCompared) {
                ++sharing.searchLimitHits;
                break;
            }
            auto rest = text.subspan(at);
            for (auto position = findLastNotAfter(heads, rest, charactersCompared); position; position = heads.longestHeads[*position]) {
                const OrderedString& candidate = heads.orderedStrings[*position];
                uint32_t part = candidate.index;
                std::span inner { candidate.characters, candidate.size };
                if (inner.size() >= text.size())
                    continue;
                charactersCompared += inner.size();
                if (!spanHasPrefix(rest, inner))
                    continue;
                if (isName.get(part) && !isName.get(whole))
                    continue;
                // The search that shared this string also visited its heads.
                if (holders[part] != part)
                    break;
                holders[part] = whole;
                gaps[part] = safeCast<uint32_t>(rest.size() - inner.size());
            }
        }
    }
}

static void flattenHolders(const BitVector& hasText, TextSharing& sharing)
{
    auto& holders = sharing.holders;
    auto& gaps = sharing.gaps;
    for (size_t index : hasText) {
        uint32_t holder = holders[index];
        if (holders[holder] == holder)
            continue;
        gaps[index] = gaps[holder];
        holders[index] = holders[holder];
    }
}

static void shareTextOfWidth(StringBytes strings, const BitVector& hasText, const BitVector& isName, size_t unitSize, BuildContext& context, TextSharing& sharing)
{
    if (hasText.isEmpty())
        return;
    const ParallelLoops& loops = context.parallelLoops();
    InnerPartSearch search { unitSize, context.limit(LimitID::MaxComparedCharactersPerCharacter) };
    shareTails(loops, strings, hasText, isName, sharing.holders);
    findInnerParts(strings, isName, buildHeadIndex(loops, strings, hasText, sharing.holders), search, sharing);
    flattenHolders(hasText, sharing);
}

TextSharing shareText(std::span<const DecodedString> decodedStrings, const BitVector& hasText, const BitVector& isName, BuildContext& context)
{
    StringBytes strings(decodedStrings);
    Vector<uint32_t> holders(strings.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    Vector<uint32_t> gaps(FillWith { }, strings.size(), 0);
    TextSharing sharing { .holders = WTF::move(holders), .gaps = WTF::move(gaps) };

    BitVector has8BitText(strings.size());
    BitVector has16BitText(strings.size());
    for (size_t index : hasText)
        (decodedStrings[index].is16Bit() ? has16BitText : has8BitText).quickSet(index);
    shareTextOfWidth(strings, has8BitText, isName, sizeof(uint8_t), context, sharing);
    shareTextOfWidth(strings, has16BitText, isName, sizeof(char16_t), context, sharing);
    return sharing;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
