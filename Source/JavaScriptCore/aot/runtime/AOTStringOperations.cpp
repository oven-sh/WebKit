/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTCaseConversions.h"
#include "AOTGrowingText.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTJSONTextWriter.h"
#include "AOTLimits.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTQuotedStringCache.h"
#include "AOTRuntimeReporter.h"
#include "AOTSlotRepresentation.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "AOTVector.h"
#include "GCDeferralContextInlines.h"
#include "IntlCollator.h"
#include "IntlObject.h"
#include "InterpreterInlines.h"
#include "IntlObjectInlines.h"
#include "JSCInlines.h"
#include "JSONObject.h"
#include "KeyAtomStringCacheInlines.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "NumericStrings.h"
#include "StringConstructor.h"
#include "StringPrototype.h"
#include "StringPrototypeInlines.h"
#include "StringSplitCacheInlines.h"
#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <span>
#include <tuple>
#include <wtf/SIMDHelpers.h>
#include <wtf/SIMDUTF.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/AdaptiveStringSearcher.h>
#include <wtf/text/EscapedFormsForJSON.h>
#include <wtf/text/IntegerToStringConversion.h>
#include <wtf/text/StringBuilderJSON.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(static_cast<int32_t>(notFound) == -1, "compiled code takes the low 32 bits of a search result as the index or -1");

static constexpr size_t maxRegisterStringLength = defaultLimitValue(LimitID::MaxRegisterStringLength, hostArchitecture());
static_assert(maxRegisterStringLength == sizeof(uint64_t));

static ALWAYS_INLINE StringView textOf(uint32_t text)
{
    return Image::current()->text().viewInline(StringID(text));
}

template<typename CharacterType>
static ALWAYS_INLINE bool canRepresent(uint32_t character)
{
    return !(character >> (8 * sizeof(CharacterType)));
}

static constexpr uint32_t maxLatin1Character = 0xff;

static NEVER_INLINE size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES findLatin1CharacterFarFromStart(const Latin1Character* characters, uint32_t length, uint32_t character, uint32_t start)
{
    return WTF::find(unsafeMakeSpan(characters, length), static_cast<Latin1Character>(character), start);
}

static ALWAYS_INLINE size_t findLatin1Character(const Latin1Character* characters, size_t length, Latin1Character character, size_t start)
{
    static_assert(std::endian::native == std::endian::little);
    constexpr size_t stride = SIMD::stride<uint8_t>;
    constexpr size_t stridesSearchedHere = 8;
    const uint8_t* bytes = byteCast<uint8_t>(characters);
    if (start >= length)
        return notFound;
    size_t i = start;
    if (length - i >= stride) {
        auto sought = SIMD::splat<uint8_t>(character);
        size_t lastStride = length - stride;
        for (size_t end = i + stridesSearchedHere * stride; i <= lastStride; i += stride) {
            if (i >= end) [[unlikely]]
                return findLatin1CharacterFarFromStart(characters, length, character, i);
            if (auto found = SIMD::findFirstNonZeroIndex(SIMD::equal(SIMD::load(bytes + i), sought)))
                return i + *found;
        }
        if (i == length)
            return notFound;
        auto found = SIMD::findFirstNonZeroIndex(SIMD::equal(SIMD::load(bytes + lastStride), sought));
        return found ? lastStride + *found : notFound;
    }
    if (length - i >= sizeof(uint64_t)) {
        constexpr uint64_t lowBits = 0x0101010101010101ULL;
        for (size_t place : { i, length - sizeof(uint64_t) }) {
            uint64_t differences = WTF::unalignedLoad<uint64_t>(bytes + place) ^ lowBits * character;
            if (uint64_t found = (differences - lowBits) & ~differences & lowBits << 7)
                return place + std::countr_zero(found) / 8;
        }
        return notFound;
    }
    for (; i < length; ++i) {
        if (bytes[i] == character)
            return i;
    }
    return notFound;
}

template<typename CharacterType>
static ALWAYS_INLINE size_t findCharacter(const CharacterType* characters, uint32_t length, uint32_t character, uint32_t start)
{
    if (!canRepresent<CharacterType>(character))
        return notFound;
    if constexpr (std::is_same_v<CharacterType, Latin1Character>)
        return findLatin1Character(characters, length, static_cast<Latin1Character>(character), start);
    else
        return WTF::find(unsafeMakeSpan(characters, length), static_cast<CharacterType>(character), start);
}

template<typename CharacterType>
static ALWAYS_INLINE size_t reverseFindCharacter(const CharacterType* characters, uint32_t length, uint32_t character, int32_t position)
{
    if (position < 0 || !length || !canRepresent<CharacterType>(character))
        return notFound;
    return WTF::reverseFind(unsafeMakeSpan(characters, length), static_cast<CharacterType>(character), std::min<unsigned>(position, length - 1));
}

constexpr uint32_t longestTextComparedAtEveryPlace = 32;
constexpr uint32_t charactersComparedWithoutCount = 16;

template<typename CharacterType>
class TextReadBackwards {
public:
    TextReadBackwards(const CharacterType* characters, size_t length)
        : m_last(characters + length - 1)
    {
    }

    CharacterType operator[](size_t index) const { return *(m_last - index); }

private:
    const CharacterType* m_last;
};

template<typename Sought>
static intptr_t lastOfLeftPart(Sought sought, intptr_t length, bool greaterComesFirst, intptr_t& period)
{
    intptr_t last = -1;
    intptr_t candidate = 0;
    intptr_t offset = 1;
    period = 1;
    while (candidate + offset < length) {
        auto ofCandidate = sought[candidate + offset];
        auto ofBest = sought[last + offset];
        if (ofCandidate == ofBest) {
            if (offset != period)
                ++offset;
            else {
                candidate += period;
                offset = 1;
            }
        } else if (greaterComesFirst ? ofCandidate > ofBest : ofCandidate < ofBest) {
            candidate += offset;
            offset = 1;
            period = candidate - last;
        } else {
            last = candidate++;
            offset = 1;
            period = 1;
        }
    }
    return last;
}

template<typename Text, typename Sought>
static size_t findInLinearTime(Text text, size_t length, Sought sought, size_t soughtLength, size_t start)
{
    const intptr_t end = soughtLength;
    intptr_t periodOfOne;
    intptr_t periodOfOther;
    intptr_t one = lastOfLeftPart(sought, end, false, periodOfOne);
    intptr_t other = lastOfLeftPart(sought, end, true, periodOfOther);
    const intptr_t lastOfLeft = std::max(one, other);
    intptr_t period = one > other ? periodOfOne : periodOfOther;

    bool leftPartComesAgain = true;
    for (intptr_t i = 0; i <= lastOfLeft && leftPartComesAgain; ++i)
        leftPartComesAgain = sought[i] == sought[i + period];
    if (!leftPartComesAgain)
        period = std::max(lastOfLeft + 1, end - lastOfLeft - 1) + 1;

    const intptr_t lastPlace = length - soughtLength;
    intptr_t lastKnownToAgree = -1;
    for (intptr_t place = start; place <= lastPlace;) {
        intptr_t i = std::max(lastOfLeft, lastKnownToAgree) + 1;
        while (i < end && sought[i] == text[place + i])
            ++i;
        if (i < end) {
            place += i - lastOfLeft;
            lastKnownToAgree = -1;
            continue;
        }
        i = lastOfLeft;
        while (i > lastKnownToAgree && sought[i] == text[place + i])
            --i;
        if (i <= lastKnownToAgree)
            return place;
        place += period;
        if (leftPartComesAgain)
            lastKnownToAgree = end - period - 1;
    }
    return notFound;
}

template<typename CharacterType, typename OtherCharacterType>
static ALWAYS_INLINE size_t findWhereStringMayStart(std::span<const CharacterType> starts, const OtherCharacterType* other, uint32_t otherLength, size_t i)
{
    CharacterType first = other[0];
    if constexpr (std::is_same_v<CharacterType, Latin1Character>) {
        constexpr size_t stride = SIMD::stride<uint8_t>;
        const uint8_t* firsts = byteCast<uint8_t>(starts.data());
        const uint8_t* lasts = firsts + otherLength - 1;
        auto firstOfText = SIMD::splat<uint8_t>(first);
        auto lastOfText = SIMD::splat<uint8_t>(other[otherLength - 1]);
        for (; i + stride <= starts.size(); i += stride) {
            if (auto found = SIMD::findFirstNonZeroIndex(SIMD::bitAnd(SIMD::equal(SIMD::load(firsts + i), firstOfText), SIMD::equal(SIMD::load(lasts + i), lastOfText))))
                return i + *found;
        }
        while (i < starts.size() && starts[i] != first)
            ++i;
        return i < starts.size() ? i : notFound;
    } else
        return WTF::find(starts, first, i);
}

template<typename CharacterType, typename OtherCharacterType>
static ALWAYS_INLINE bool longStringStandsAt(const CharacterType* place, const OtherCharacterType* other, uint32_t otherLength, bool& wasComparedFar)
{
    wasComparedFar = WTF::equal(place, unsafeMakeSpan(other, charactersComparedWithoutCount));
    return wasComparedFar && WTF::equal(place + charactersComparedWithoutCount, unsafeMakeSpan(other + charactersComparedWithoutCount, otherLength - charactersComparedWithoutCount));
}

template<typename CharacterType, typename OtherCharacterType>
static NEVER_INLINE size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES findLongString(const CharacterType* characters, uint32_t length, const OtherCharacterType* other, uint32_t otherLength, uint32_t start)
{
    auto starts = unsafeMakeSpan(characters, length - otherLength + 1);
    uint64_t comparedInVain = 0;
    for (size_t i = start;; ++i) {
        i = findWhereStringMayStart(starts, other, otherLength, i);
        bool wasComparedFar = false;
        if (i == notFound || longStringStandsAt(characters + i, other, otherLength, wasComparedFar))
            return i;
        if (!wasComparedFar)
            continue;
        comparedInVain += otherLength;
        if (comparedInVain > 2 * (i - start) + 4 * static_cast<uint64_t>(otherLength))
            return findInLinearTime(characters, length, other, otherLength, i + 1);
    }
}

template<typename CharacterType, typename OtherCharacterType>
static NEVER_INLINE size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES reverseFindLongString(const CharacterType* characters, uint32_t length, const OtherCharacterType* other, uint32_t otherLength, int32_t position)
{
    size_t start = std::min<size_t>(std::max(position, 0), length - otherLength);
    auto starts = unsafeMakeSpan(characters, start + 1);
    uint64_t comparedInVain = 0;
    for (size_t i = start;; --i) {
        i = WTF::reverseFind(starts, static_cast<CharacterType>(other[0]), i);
        bool wasComparedFar = false;
        if (i == notFound || longStringStandsAt(characters + i, other, otherLength, wasComparedFar))
            return i;
        if (!i)
            return notFound;
        if (!wasComparedFar)
            continue;
        comparedInVain += otherLength;
        if (comparedInVain > 2 * (start - i) + 4 * static_cast<uint64_t>(otherLength)) {
            size_t end = i - 1 + otherLength;
            size_t fromEnd = findInLinearTime(TextReadBackwards(characters, end), end, TextReadBackwards(other, otherLength), otherLength, 0);
            return fromEnd == notFound ? notFound : end - otherLength - fromEnd;
        }
    }
}

template<typename CharacterType>
static ALWAYS_INLINE size_t findString(const CharacterType* characters, uint32_t length, const CharacterType* other, uint32_t otherLength, uint32_t start)
{
    if (start > length)
        return notFound;
    if (!otherLength)
        return start;
    if (otherLength > length - start)
        return notFound;
    if (otherLength > longestTextComparedAtEveryPlace)
        return findLongString(characters, length, other, otherLength, start);
    CharacterType first = other[0];
    auto rest = unsafeMakeSpan(other + 1, otherLength - 1);
    auto starts = unsafeMakeSpan(characters, length - otherLength + 1);
    if constexpr (std::is_same_v<CharacterType, Latin1Character>) {
        constexpr size_t stride = SIMD::stride<uint8_t>;
        const uint8_t* firsts = byteCast<uint8_t>(characters);
        const uint8_t* lasts = firsts + otherLength - 1;
        auto firstOfText = SIMD::splat<uint8_t>(first);
        auto lastOfText = SIMD::splat<uint8_t>(other[otherLength - 1]);
        for (size_t i = start;; ++i) {
            if (i + stride <= starts.size()) {
                auto found = SIMD::findFirstNonZeroIndex(SIMD::bitAnd(SIMD::equal(SIMD::load(firsts + i), firstOfText), SIMD::equal(SIMD::load(lasts + i), lastOfText)));
                if (!found) {
                    i += stride - 1;
                    continue;
                }
                i += *found;
            } else {
                while (i < starts.size() && characters[i] != first)
                    ++i;
                if (i >= starts.size())
                    return notFound;
            }
            if (WTF::equal(characters + i + 1, rest))
                return i;
        }
    } else {
        for (size_t i = start;; ++i) {
            i = WTF::find(starts, first, i);
            if (i == notFound || WTF::equal(characters + i + 1, rest))
                return i;
        }
    }
}

static ALWAYS_INLINE size_t reverseFindString(const Latin1Character* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, int32_t position)
{
    if (otherLength > length)
        return notFound;
    if (otherLength > longestTextComparedAtEveryPlace)
        return reverseFindLongString(characters, length, other, otherLength, position);
    unsigned start = std::min<unsigned>(std::max(position, 0), length - otherLength);
    if (!otherLength)
        return start;
    Latin1Character first = other[0];
    auto rest = unsafeMakeSpan(other + 1, otherLength - 1);
    for (size_t i = start;; --i) {
        if (characters[i] == first && WTF::equal(characters + i + 1, rest))
            return i;
        if (!i)
            return notFound;
    }
}

static NEVER_INLINE size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES reverseFindCharacterFarFromStart(const Latin1Character* characters, uint32_t length, Latin1Character character, int32_t position)
{
    return reverseFindCharacter(characters, length, character, position);
}

static ALWAYS_INLINE size_t reverseFindStringOrCharacter(const Latin1Character* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, int32_t position)
{
    constexpr int32_t leastPlacesSearchedByVectors = 32;
    if (otherLength == 1 && position >= leastPlacesSearchedByVectors && length > leastPlacesSearchedByVectors)
        return reverseFindCharacterFarFromStart(characters, length, other[0], position);
    return reverseFindString(characters, length, other, otherLength, position);
}

template<typename OtherCharacterType>
static ALWAYS_INLINE size_t reverseFindString(const char16_t* characters, uint32_t length, const OtherCharacterType* other, uint32_t otherLength, int32_t position)
{
    if (otherLength > length)
        return notFound;
    if (otherLength > longestTextComparedAtEveryPlace)
        return reverseFindLongString(characters, length, other, otherLength, position);
    unsigned start = std::min<unsigned>(std::max(position, 0), length - otherLength);
    if (!otherLength)
        return start;
    char16_t first = other[0];
    auto rest = unsafeMakeSpan(other + 1, otherLength - 1);
    auto starts = unsafeMakeSpan(characters, start + 1);
    for (size_t i = start;; --i) {
        i = WTF::reverseFind(starts, first, i);
        if (i == notFound || WTF::equal(characters + i + 1, rest))
            return i;
        if (!i)
            return notFound;
    }
}

template<typename CharacterType>
static ALWAYS_INLINE size_t findSeparators(const CharacterType* characters, uint32_t length, uint32_t separator, uint32_t countAndWhetherAllAreCounted, uint32_t* bounds)
{
    uint32_t count = countAndWhetherAllAreCounted & 0x7fffffff;
    uint32_t numberOfPieces = 0;
    uint32_t start = 0;
    while (numberOfPieces < count) {
        size_t found = findCharacter(characters, length, separator, start);
        bounds[2 * numberOfPieces] = start;
        bounds[2 * numberOfPieces + 1] = found == notFound ? length : static_cast<uint32_t>(found);
        ++numberOfPieces;
        if (found == notFound) {
            for (uint32_t absent = 2 * numberOfPieces; absent < 2 * count; ++absent)
                bounds[absent] = UINT32_MAX;
            return numberOfPieces;
        }
        start = static_cast<uint32_t>(found) + 1;
    }
    if (!(countAndWhetherAllAreCounted >> 31))
        return numberOfPieces;
    size_t behind = 1;
    if (canRepresent<CharacterType>(separator)) {
        for (uint32_t position = start; position < length; ++position)
            behind += characters[position] == static_cast<CharacterType>(separator);
    }
    return numberOfPieces + behind;
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFindSeparators8(const Latin1Character* characters, uint32_t length, uint32_t separator, uint32_t countAndWhetherAllAreCounted, uint32_t* bounds)
{
    return findSeparators(characters, length, separator, countAndWhetherAllAreCounted, bounds);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFindSeparators16(const char16_t* characters, uint32_t length, uint32_t separator, uint32_t countAndWhetherAllAreCounted, uint32_t* bounds)
{
    return findSeparators(characters, length, separator, countAndWhetherAllAreCounted, bounds);
}

template<typename CharacterType>
static ALWAYS_INLINE size_t findLongSeparators(const CharacterType* characters, uint32_t length, uint32_t text, uint32_t countAndWhetherAllAreCounted, uint32_t* bounds)
{
    StringView subject { std::span { characters, length } };
    StringView separator = textOf(text);
    uint32_t count = countAndWhetherAllAreCounted & 0x7fffffff;
    bool allAreCounted = countAndWhetherAllAreCounted >> 31;
    uint32_t numberOfPieces = 0;
    size_t start = 0;
    for (size_t found = 0; found != notFound && (numberOfPieces < count || allAreCounted); ++numberOfPieces) {
        found = subject.find(separator, start);
        if (numberOfPieces < count) {
            bounds[2 * numberOfPieces] = static_cast<uint32_t>(start);
            bounds[2 * numberOfPieces + 1] = found == notFound ? length : static_cast<uint32_t>(found);
        }
        start = found + separator.length();
    }
    for (uint32_t absent = 2 * numberOfPieces; absent < 2 * count; ++absent)
        bounds[absent] = UINT32_MAX;
    return numberOfPieces;
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFindLongSeparators8(const Latin1Character* characters, uint32_t length, uint32_t text, uint32_t countAndWhetherAllAreCounted, uint32_t* bounds)
{
    return findLongSeparators(characters, length, text, countAndWhetherAllAreCounted, bounds);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFindLongSeparators16(const char16_t* characters, uint32_t length, uint32_t text, uint32_t countAndWhetherAllAreCounted, uint32_t* bounds)
{
    return findLongSeparators(characters, length, text, countAndWhetherAllAreCounted, bounds);
}

static constexpr uint32_t latin1Stride = SIMD::stride<uint8_t>;

static ALWAYS_INLINE auto compareLatin1Stride(const Latin1Character* characters, uint32_t character)
{
    return SIMD::equal(SIMD::load(byteCast<uint8_t>(characters)), SIMD::splat<uint8_t>(static_cast<uint8_t>(character)));
}

static NEVER_INLINE size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES findCharacter8WithinStride(const Latin1Character* characters, uint32_t length, uint32_t character, uint32_t start)
{
    return findCharacter(characters, length, character, start);
}

static NEVER_INLINE size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES findCharacter8AfterFirstStride(const Latin1Character* characters, uint32_t length, uint32_t character, uint32_t start)
{
    constexpr uint32_t stridesSearchedHere = 8;
    uint32_t lastStride = length - latin1Stride;
    uint32_t i = start + latin1Stride;
    for (uint32_t strides = 0; i <= lastStride; i += latin1Stride) {
        if (++strides > stridesSearchedHere) [[unlikely]]
            return findLatin1CharacterFarFromStart(characters, length, character, i);
        if (auto found = SIMD::findFirstNonZeroIndex(compareLatin1Stride(characters + i, character)))
            return static_cast<size_t>(i) + *found;
    }
    if (i == length)
        return notFound;
    auto found = SIMD::findFirstNonZeroIndex(compareLatin1Stride(characters + lastStride, character));
    return found ? static_cast<size_t>(lastStride) + *found : notFound;
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFindCharacter8(const Latin1Character* characters, uint32_t length, uint32_t character, uint32_t start)
{
    if (start >= length || length - start < latin1Stride || character > maxLatin1Character) [[unlikely]]
        [[clang::musttail]] return findCharacter8WithinStride(characters, length, character, start);
    if (auto found = SIMD::findFirstNonZeroIndex(compareLatin1Stride(characters + start, character)))
        return static_cast<size_t>(start) + *found;
    [[clang::musttail]] return findCharacter8AfterFirstStride(characters, length, character, start);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFindCharacter16(const char16_t* characters, uint32_t length, uint32_t character, uint32_t start)
{
    return findCharacter(characters, length, character, start);
}

static NEVER_INLINE size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES reverseFindCharacter8WithinStride(const Latin1Character* characters, uint32_t length, uint32_t character, int32_t position)
{
    static_assert(std::endian::native == std::endian::little);
    constexpr uint64_t lowBits = 0x0101010101010101ULL;
    constexpr uint64_t otherBits = 0x7f * lowBits;
    constexpr uint32_t wordSize = sizeof(uint64_t);
    if (position < 0 || character > maxLatin1Character)
        return notFound;
    uint32_t end = static_cast<uint32_t>(position) < length ? static_cast<uint32_t>(position) + 1 : length;
    if (end >= wordSize) {
        for (uint32_t place : { end - wordSize, 0u }) {
            uint64_t differences = WTF::unalignedLoad<uint64_t>(characters + place) ^ lowBits * character;
            if (uint64_t found = ~(((differences & otherBits) + otherBits) | differences | otherBits))
                return static_cast<size_t>(place) + 7 - std::countl_zero(found) / 8;
        }
        return notFound;
    }
    while (end--) {
        if (characters[end] == character)
            return end;
    }
    return notFound;
}

static ALWAYS_INLINE uint32_t endOfReverseSearch(uint32_t length, int32_t position)
{
    return static_cast<uint32_t>(position) < length ? static_cast<uint32_t>(position) + 1 : length;
}

static NEVER_INLINE size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES reverseFindCharacter8BeforeLastStride(const Latin1Character* characters, uint32_t length, uint32_t character, int32_t position)
{
    constexpr uint32_t stridesSearchedHere = 8;
    uint32_t end = endOfReverseSearch(length, position) - latin1Stride;
    for (uint32_t strides = 0; end >= latin1Stride; end -= latin1Stride) {
        if (++strides > stridesSearchedHere) [[unlikely]]
            return WTF::reverseFind(unsafeMakeSpan(characters, end), static_cast<Latin1Character>(character), end - 1);
        if (auto found = SIMD::findLastNonZeroIndex(compareLatin1Stride(characters + end - latin1Stride, character)))
            return static_cast<size_t>(end - latin1Stride) + *found;
    }
    if (!end)
        return notFound;
    auto found = SIMD::findLastNonZeroIndex(compareLatin1Stride(characters, character));
    return found ? static_cast<size_t>(*found) : notFound;
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringReverseFindCharacter8(const Latin1Character* characters, uint32_t length, uint32_t character, int32_t position)
{
    uint32_t end = endOfReverseSearch(length, position);
    if (position < 0 || end < latin1Stride || character > maxLatin1Character) [[unlikely]]
        [[clang::musttail]] return reverseFindCharacter8WithinStride(characters, length, character, position);
    if (auto found = SIMD::findLastNonZeroIndex(compareLatin1Stride(characters + end - latin1Stride, character)))
        return static_cast<size_t>(end - latin1Stride) + *found;
    [[clang::musttail]] return reverseFindCharacter8BeforeLastStride(characters, length, character, position);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringReverseFindCharacter16(const char16_t* characters, uint32_t length, uint32_t character, int32_t position)
{
    return reverseFindCharacter(characters, length, character, position);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFind8(const Latin1Character* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, uint32_t start)
{
    return findString(characters, length, other, otherLength, start);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFind16(const char16_t* characters, uint32_t length, const char16_t* other, uint32_t otherLength, uint32_t start)
{
    return findString(characters, length, other, otherLength, start);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringReverseFind8(const Latin1Character* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, int32_t position)
{
    return reverseFindStringOrCharacter(characters, length, other, otherLength, position);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringReverseFind16(const char16_t* characters, uint32_t length, const char16_t* other, uint32_t otherLength, int32_t position)
{
    return reverseFindString(characters, length, other, otherLength, position);
}

static ALWAYS_INLINE bool blockIsASCII(const Latin1Character* block)
{
    auto word = [&](size_t index) ALWAYS_INLINE_LAMBDA {
        return WTF::unalignedLoad<uint64_t>(block + index * sizeof(uint64_t));
    };
    return !((word(0) | word(1) | word(2) | word(3) | word(4) | word(5) | word(6) | word(7)) & 0x8080808080808080ULL);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringUTF8Length8(const Latin1Character* characters, uint32_t length)
{
    static_assert(std::endian::native == std::endian::little);
    constexpr uint64_t highBits = 0x8080808080808080ULL;
    constexpr uint32_t blockLength = 8 * sizeof(uint64_t);
    size_t result = length;
    uint32_t i = 0;
    if (length >= blockLength) {
        uint32_t lastBlock = length - blockLength;
        while (true) {
            while (i <= lastBlock && blockIsASCII(characters + i))
                i += blockLength;
            if (i > lastBlock)
                break;
#if COMPILER(CLANG)
#pragma clang loop unroll(disable)
#endif
            for (uint32_t end = i + blockLength; i < end; i += sizeof(uint64_t))
                result += std::popcount(WTF::unalignedLoad<uint64_t>(characters + i) & highBits);
        }
        if (i == length || blockIsASCII(characters + lastBlock))
            return result;
    }
    for (; i + sizeof(uint64_t) <= length; i += sizeof(uint64_t))
        result += std::popcount(WTF::unalignedLoad<uint64_t>(characters + i) & highBits);
    uint32_t rest = length - i;
    if (!rest)
        return result;
    if (i)
        return result + std::popcount(WTF::unalignedLoad<uint64_t>(characters + length - sizeof(uint64_t)) & highBits << 8 * (sizeof(uint64_t) - rest));
    for (; i < length; ++i)
        result += characters[i] >> 7;
    return result;
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringUTF8Length16(const char16_t* characters, uint32_t length)
{
    return simdutf::utf8_length_from_utf16_with_replacement(characters, length).count;
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringImplsAreEqual(void* first, void* second)
{
    return WTF::equal(static_cast<StringImpl*>(first), static_cast<StringImpl*>(second));
}

extern "C" int64_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTCompareStringImpls(void* first, void* second)
{
    std::strong_ordering order = codePointCompare(StringView { *static_cast<StringImpl*>(first) }, StringView { *static_cast<StringImpl*>(second) });
    if (order < 0)
        return -1;
    return order > 0;
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringImplEqualsImageString(void* impl, uint32_t string)
{
    return Image::current()->text().equals(StringID(string), StringView { *static_cast<StringImpl*>(impl) });
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTCharactersEqualImageString(const Latin1Character* characters, uint32_t length, uint32_t string)
{
    return Image::current()->text().equals(StringID(string), StringView { std::span { characters, length } });
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTHashStringImpl(void* impl)
{
    return static_cast<StringImpl*>(impl)->hash();
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTFindKeptStringOfTwoCharacters(void* vmPointer, uint32_t units)
{
    VM& vm = *static_cast<VM*>(vmPointer);
    Latin1Character characters[] = { static_cast<Latin1Character>(units), static_cast<Latin1Character>(units >> 16) };
    WTF::HashTranslatorCharBuffer<Latin1Character> buffer { std::span<const Latin1Character> { characters } };
    return std::bit_cast<size_t>(vm.keyAtomStringCache.make(vm, buffer, [](VM&, auto&) -> JSString* {
        return nullptr;
    }));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringFindLiteral8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t text, uint32_t start))
{
    StringView other = textOf(text);
    if (!other.is8Bit()) [[unlikely]]
        return StringView(std::span { characters, length }).find(other, start);
    return operationAOTStringFind8(characters, length, other.span8().data(), other.length(), start);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringReverseFindLiteral8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t text, int32_t position))
{
    StringView other = textOf(text);
    if (!other.is8Bit()) [[unlikely]]
        return StringView(std::span { characters, length }).reverseFind(other, std::max(position, 0));
    return reverseFindString(characters, length, other.span8().data(), other.length(), position);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringFindLiteral16, size_t, (Instance*, const char16_t* characters, uint32_t length, uint32_t text, uint32_t start))
{
    return StringView(std::span { characters, length }).find(textOf(text), start);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringReverseFindLiteral16, size_t, (Instance*, const char16_t* characters, uint32_t length, uint32_t text, int32_t position))
{
    StringView other = textOf(text);
    if (!other.is8Bit()) [[unlikely]]
        return StringView(std::span { characters, length }).reverseFind(other, std::max(position, 0));
    return reverseFindString(characters, length, other.span8().data(), other.length(), position);
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcat, JSCell*, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcat);
    JSString* leftString = cellArgument<JSString>(left, Entry::operationAOTConcat);
    JSString* rightString = cellArgument<JSString>(right, Entry::operationAOTConcat);
    OPERATION_RETURN(scope, joinTexts(globalObject, *instance, leftString, rightString));
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcatThree, JSCell*, (Instance* instance, JSCell* first, JSCell* second, JSCell* third))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatThree);
    JSString* firstString = cellArgument<JSString>(first, Entry::operationAOTConcatThree);
    JSString* secondString = cellArgument<JSString>(second, Entry::operationAOTConcatThree);
    JSString* thirdString = cellArgument<JSString>(third, Entry::operationAOTConcatThree);
    if (firstString->isRope() && firstString->length() >= minLengthOfGrowingText) {
        JSString* pieces[] { secondString, thirdString };
        if (JSString* grown = tryAppendToGrowingText(*instance, firstString, pieces))
            OPERATION_RETURN(scope, grown);
    }
    OPERATION_RETURN(scope, jsString(globalObject, firstString, secondString, thirdString));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSubstring, JSCell*, (Instance* instance, JSCell* string, int32_t from, int32_t to))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSubstring);
    leaveCellsForRopes(instance);
    OPERATION_RETURN(scope, jsSubstring(globalObject, cellArgument<JSString>(string, Entry::operationAOTSubstring), from, to - from));
}

static ALWAYS_INLINE void copyShortString(std::span<const Latin1Character> characters, Latin1Character* to)
{
    const Latin1Character* from = characters.data();
    size_t size = characters.size();
    if (size >= 8) {
        uint64_t first = WTF::unalignedLoad<uint64_t>(from);
        uint64_t last = WTF::unalignedLoad<uint64_t>(from + size - 8);
        WTF::unalignedStore<uint64_t>(to, first);
        WTF::unalignedStore<uint64_t>(to + size - 8, last);
        return;
    }
    if (size >= 4) {
        uint32_t first = WTF::unalignedLoad<uint32_t>(from);
        uint32_t last = WTF::unalignedLoad<uint32_t>(from + size - 4);
        WTF::unalignedStore<uint32_t>(to, first);
        WTF::unalignedStore<uint32_t>(to + size - 4, last);
        return;
    }
    if (size) {
        Latin1Character first = from[0];
        Latin1Character middle = from[size / 2];
        Latin1Character last = from[size - 1];
        to[0] = first;
        to[size / 2] = middle;
        to[size - 1] = last;
    }
}

static ALWAYS_INLINE std::array<const JSString*, JSRopeString::s_maxInternalRopeLength> fibersOf(const JSRopeString* rope)
{
    using CompactFibers = JSRopeString::CompactFibers;
    static_assert(CompactFibers::widthOfFiber1LowerInLengthWord == 32 && CompactFibers::shiftOfFiber2InUpperWord == 16);
    auto* bytes = reinterpret_cast<const uint8_t*>(rope);
    uint64_t upperWord = WTF::unalignedLoad<uint64_t>(bytes + JSRopeString::offsetOfFiber2());
    uint64_t second = WTF::unalignedLoad<uint32_t>(bytes + JSRopeString::offsetOfFiber1Lower()) | (upperWord & 0xffff) << 32;
    return { std::bit_cast<const JSString*>(WTF::unalignedLoad<uint64_t>(bytes + JSRopeString::offsetOfFiber0()) & JSRopeString::stringMask), std::bit_cast<const JSString*>(second), std::bit_cast<const JSString*>(upperWord >> 16) };
}

static ALWAYS_INLINE const Latin1Character* latin1CharactersInPlace(const JSString* string, unsigned& length)
{
    if (const StringImpl* impl = string->tryGetValueImpl()) [[likely]] {
        length = impl->length();
        return impl->is8Bit() ? impl->span8().data() : nullptr;
    }
    auto* rope = uncheckedDowncast<JSRopeString>(string);
    if (!rope->isSubstring())
        return nullptr;
    length = rope->length();
    const StringImpl* base = rope->substringBase()->tryGetValueImpl();
    return base->is8Bit() ? base->span8().data() + rope->substringOffset() : nullptr;
}

static NEVER_INLINE bool tryResolveRopeOfShortRopes(VM& vm, JSRopeString* rope)
{
    constexpr unsigned maxLength = 1024;
    constexpr unsigned maxLengthOfShortString = 16;
    if (rope->isSubstring() || !rope->is8Bit() || rope->length() > maxLength)
        return false;
    std::span<Latin1Character> buffer;
    RefPtr<StringImpl> impl = StringImpl::tryCreateUninitialized(rope->length(), buffer);
    if (!impl)
        return false;
    Latin1Character* cursor = buffer.data();
    Latin1Character* end = cursor + buffer.size();
    auto tryCopy = [&](const JSString* string) ALWAYS_INLINE_LAMBDA {
        unsigned length;
        const Latin1Character* characters = latin1CharactersInPlace(string, length);
        if (!characters || length > static_cast<size_t>(end - cursor))
            return false;
        if (length <= maxLengthOfShortString)
            copyShortString({ characters, length }, cursor);
        else
            memcpy(cursor, characters, length);
        cursor += length;
        return true;
    };
    for (const JSString* fiber : fibersOf(rope)) {
        if (!fiber)
            break;
        if (tryCopy(fiber))
            continue;
        if (!fiber->isRope() || fiber->isSubstring())
            return false;
        for (const JSString* part : fibersOf(uncheckedDowncast<JSRopeString>(fiber))) {
            if (!part)
                break;
            if (!tryCopy(part))
                return false;
        }
    }
    RELEASE_ASSERT(cursor == end);
    size_t cost = impl->cost();
    rope->resolveBySharing(*impl);
    vm.heap.reportExtraMemoryAllocated(rope, cost);
    return true;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringCodeUnitAt, size_t, (Instance*, JSCell* cell, uint32_t index))
{
    auto* string = cellArgument<JSString>(cell, Entry::operationAOTStringCodeUnitAt);
    if (string->isRope()) {
        DeferGCForAWhile deferGC(string->vm());
        uncheckedDowncast<JSRopeString>(string)->resolveRope(nullptr);
    }
    const JSString* part = string;
    while (part->isRope()) {
        const auto* rope = uncheckedDowncast<JSRopeString>(part);
        if (rope->isSubstring()) {
            index += rope->substringOffset();
            part = rope->substringBase();
            continue;
        }
        for (const JSString* fiber : fibersOf(rope)) {
            part = fiber;
            if (index < fiber->length())
                break;
            index -= fiber->length();
        }
    }
    return part->tryGetValueImpl()->at(index);
}

JSC_DEFINE_JIT_OPERATION(operationAOTFlatten, void*, (Instance* instance, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFlatten);
    JSString* cell = cellArgument<JSString>(string, Entry::operationAOTFlatten);
    bool mayGrow = false;
    if (cell->isRope()) {
        auto* rope = uncheckedDowncast<JSRopeString>(cell);
        mayGrow = rope->length() >= minLengthOfGrowingText;
        if (mayGrow && tryResolveInGrowingBuffer(*instance, rope))
            OPERATION_RETURN(scope, cell->tryGetValueImpl());
        if (rope->tryResolveFlatLatin1Parts(vm) || tryResolveRopeOfShortRopes(vm, rope)) {
            if (mayGrow)
                noteResolvedText(*instance, cell);
            OPERATION_RETURN(scope, cell->tryGetValueImpl());
        }
    }
    auto value = cell->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (mayGrow)
        noteResolvedText(*instance, cell);
    OPERATION_RETURN(scope, value->impl());
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareStrings, size_t, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareStrings);
    JSString* leftString = cellArgument<JSString>(left, Entry::operationAOTCompareStrings);
    JSString* rightString = cellArgument<JSString>(right, Entry::operationAOTCompareStrings);
    auto first = leftString->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto second = rightString->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto order = codePointCompare(StringView { first.data }, StringView { second.data });
    OPERATION_RETURN(scope, static_cast<size_t>(static_cast<int64_t>(order < 0 ? -1 : order > 0 ? 1 : 0)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringsAreEqual, size_t, (Instance* instance, JSCell* first, JSCell* second))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringsAreEqual);
    JSString* firstString = cellArgument<JSString>(first, Entry::operationAOTStringsAreEqual);
    JSString* secondString = cellArgument<JSString>(second, Entry::operationAOTStringsAreEqual);
    OPERATION_RETURN(scope, firstString->equalResolvingRopes(secondString));
}

static thread_local std::array<const JSString*, 4> ropesComparedInPieces;

static bool isAskedAboutForTheFirstTime(const JSString* rope)
{
    return rope->isNonSubstringRope() && std::exchange(ropesComparedInPieces[(std::bit_cast<uintptr_t>(rope) / sizeof(JSRopeString)) % ropesComparedInPieces.size()], rope) != rope;
}

static bool standsAt(JSGlobalObject* globalObject, JSString* base, JSString* argument, unsigned position)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto characters = base->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (isAskedAboutForTheFirstTime(argument)) {
        unsigned start = position;
        for (JSRopeString::LeafIterator pieces(argument); !pieces.isAtEnd();) {
            StringView piece = pieces.next();
            if (characters->substring(start, piece.length()) != piece)
                return false;
            start += piece.length();
        }
        return true;
    }
    auto otherCharacters = argument->view(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    return characters->substring(position, argument->length()) == otherCharacters.data;
}

static NEVER_INLINE OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stringHasAtGeneric(Instance* instance, JSCell* string, JSCell* other, int32_t position)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringHasAt);
    JSString* base = cellArgument<JSString>(string, Entry::operationAOTStringHasAt);
    JSString* argument = cellArgument<JSString>(other, Entry::operationAOTStringHasAt);
    unsigned length = base->length();
    unsigned otherLength = argument->length();
    if (position < 0 || otherLength > length || static_cast<unsigned>(position) > length - otherLength)
        OPERATION_RETURN(scope, false);
    OPERATION_RETURN(scope, standsAt(globalObject, base, argument, position));
}

static ALWAYS_INLINE bool areFlatAndNoWiderThanSubject(const StringImpl* base, const StringImpl* argument)
{
    return base && argument && (argument->is8Bit() || !base->is8Bit());
}

static ALWAYS_INLINE bool flatStringHasAt(const StringImpl& base, const StringImpl& argument, unsigned position)
{
    if (!argument.is8Bit())
        return WTF::equal(base.span16().data() + position, argument.span16());
    if (base.is8Bit())
        return WTF::equal(base.span8().data() + position, argument.span8());
    return WTF::equal(base.span16().data() + position, argument.span8());
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringHasAt, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t position))
{
    const StringImpl* base = uncheckedDowncast<JSString>(string)->tryGetValueImpl();
    const StringImpl* argument = uncheckedDowncast<JSString>(other)->tryGetValueImpl();
    if (areFlatAndNoWiderThanSubject(base, argument)) [[likely]] {
        unsigned length = base->length();
        unsigned otherLength = argument->length();
        if (position < 0 || otherLength > length || static_cast<unsigned>(position) > length - otherLength)
            return { false, nullptr };
        return { flatStringHasAt(*base, *argument, position), nullptr };
    }
    [[clang::musttail]] return stringHasAtGeneric(instance, string, other, position);
}

static ALWAYS_INLINE size_t findNarrowStringInWideOne(const char16_t* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, uint32_t start)
{
    if (start > length)
        return notFound;
    if (!otherLength)
        return start;
    if (otherLength > length - start)
        return notFound;
    if (otherLength > longestTextComparedAtEveryPlace)
        return findLongString(characters, length, other, otherLength, start);
    auto starts = unsafeMakeSpan(characters, length - otherLength + 1);
    for (size_t i = start;; ++i) {
        i = WTF::find(starts, static_cast<char16_t>(other[0]), i);
        if (i == notFound || WTF::equal(other + 1, unsafeMakeSpan(characters + i + 1, otherLength - 1)))
            return i;
    }
}

static ALWAYS_INLINE std::optional<size_t> tryFindInFlatString(JSCell* string, JSCell* other, uint32_t start)
{
    const StringImpl* base = uncheckedDowncast<JSString>(string)->tryGetValueImpl();
    const StringImpl* argument = uncheckedDowncast<JSString>(other)->tryGetValueImpl();
    if (!base || !argument) [[unlikely]]
        return std::nullopt;
    unsigned length = base->length();
    unsigned otherLength = argument->length();
    if (otherLength == 1) {
        char16_t character = argument->is8Bit() ? static_cast<char16_t>(argument->span8()[0]) : argument->span16()[0];
        if (start >= length)
            return notFound;
        return base->is8Bit() ? findCharacter(base->span8().data(), length, character, start) : findCharacter(base->span16().data(), length, character, start);
    }
    if (argument->is8Bit()) {
        if (base->is8Bit())
            return findString(base->span8().data(), length, argument->span8().data(), otherLength, start);
        return findNarrowStringInWideOne(base->span16().data(), length, argument->span8().data(), otherLength, start);
    }
    if (!base->is8Bit())
        return findString(base->span16().data(), length, argument->span16().data(), otherLength, start);
    return std::nullopt;
}

static NEVER_INLINE OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stringIndexOfFromGeneric(Instance* instance, JSCell* string, JSCell* other, int32_t startPosition)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringIndexOfFrom);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* base = cellArgument<JSString>(string, Entry::operationAOTStringIndexOfFrom);
    JSString* argument = cellArgument<JSString>(other, Entry::operationAOTStringIndexOfFrom);
    unsigned position = startPosition;
    if (base->length() < static_cast<uint64_t>(argument->length()) + position)
        OPERATION_RETURN(scope, none);
    auto otherCharacters = argument->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (argument->length() == 1 && base->isRope() && base->length() >= JSString::minLengthForRopeWalk) {
        if (auto found = base->tryFindOneChar(globalObject, otherCharacters[0], position))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    size_t found = characters->find(vm.adaptiveStringSearcherTables(), otherCharacters, position);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringIndexOfFrom, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t startPosition))
{
    static_assert(notFound == static_cast<size_t>(-1));
    if (std::optional<size_t> found = tryFindInFlatString(string, other, startPosition)) [[likely]]
        return { *found, nullptr };
    [[clang::musttail]] return stringIndexOfFromGeneric(instance, string, other, startPosition);
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES stringIncludesGeneric(Instance* instance, JSCell* string, JSCell* other)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringIncludes);
    JSString* base = cellArgument<JSString>(string, Entry::operationAOTStringIncludes);
    JSString* argument = cellArgument<JSString>(other, Entry::operationAOTStringIncludes);
    if (base->length() < argument->length())
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    auto otherCharacters = argument->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(characters->find(vm.adaptiveStringSearcherTables(), otherCharacters, 0) != notFound)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringIncludes, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* other))
{
    if (std::optional<size_t> found = tryFindInFlatString(string, other, 0)) [[likely]]
        return { JSValue::encode(jsBoolean(*found != notFound)), nullptr };
    [[clang::musttail]] return stringIncludesGeneric(instance, string, other);
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES stringEndsWithGeneric(Instance* instance, JSCell* string, JSCell* other)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringEndsWith);
    JSString* base = cellArgument<JSString>(string, Entry::operationAOTStringEndsWith);
    JSString* argument = cellArgument<JSString>(other, Entry::operationAOTStringEndsWith);
    if (base->length() < argument->length())
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    bool answer = standsAt(globalObject, base, argument, base->length() - argument->length());
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(answer)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringEndsWith, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* other))
{
    const StringImpl* base = uncheckedDowncast<JSString>(string)->tryGetValueImpl();
    const StringImpl* argument = uncheckedDowncast<JSString>(other)->tryGetValueImpl();
    if (areFlatAndNoWiderThanSubject(base, argument)) [[likely]] {
        unsigned length = base->length();
        unsigned otherLength = argument->length();
        return { JSValue::encode(jsBoolean(otherLength <= length && flatStringHasAt(*base, *argument, length - otherLength))), nullptr };
    }
    [[clang::musttail]] return stringEndsWithGeneric(instance, string, other);
}

static NEVER_INLINE OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stringLastIndexOfGeneric(Instance* instance, JSCell* cell, JSCell* otherCell, int32_t position)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringLastIndexOf);
    JSString* string = cellArgument<JSString>(cell, Entry::operationAOTStringLastIndexOf);
    JSString* other = cellArgument<JSString>(otherCell, Entry::operationAOTStringLastIndexOf);
    unsigned length = string->length();
    unsigned otherLength = other->length();
    if (length < otherLength)
        OPERATION_RETURN(scope, notFound);
    unsigned start = std::min<unsigned>(std::max(position, 0), length - otherLength);
    auto otherCharacters = other->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, notFound);
    if (otherLength == 1 && string->isRope() && length >= JSString::minLengthForRopeWalk) {
        if (auto found = string->tryFindLastOneChar(globalObject, otherCharacters[0], start))
            OPERATION_RETURN(scope, *found);
    }
    auto characters = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, notFound);
    if (otherLength == 1)
        OPERATION_RETURN(scope, characters->reverseFind(otherCharacters[0], start));
    if (!start)
        OPERATION_RETURN(scope, characters->startsWith(otherCharacters) ? 0 : notFound);
    OPERATION_RETURN(scope, characters->reverseFind(otherCharacters, start));
}

static NEVER_INLINE OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stringLastIndexOfInWideText(Instance* instance, JSCell* cell, JSCell* otherCell, int32_t position)
{
    const StringImpl* base = uncheckedDowncast<JSString>(cell)->tryGetValueImpl();
    const StringImpl* argument = uncheckedDowncast<JSString>(otherCell)->tryGetValueImpl();
    if (base && argument && !base->is8Bit()) {
        if (argument->is8Bit())
            return { reverseFindString(base->span16().data(), base->length(), argument->span8().data(), argument->length(), position), nullptr };
        return { reverseFindString(base->span16().data(), base->length(), argument->span16().data(), argument->length(), position), nullptr };
    }
    [[clang::musttail]] return stringLastIndexOfGeneric(instance, cell, otherCell, position);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringLastIndexOf, size_t, (Instance* instance, JSCell* cell, JSCell* otherCell, int32_t position))
{
    const StringImpl* base = uncheckedDowncast<JSString>(cell)->tryGetValueImpl();
    const StringImpl* argument = uncheckedDowncast<JSString>(otherCell)->tryGetValueImpl();
    if (base && argument && base->is8Bit() && argument->is8Bit()) [[likely]]
        return { reverseFindStringOrCharacter(base->span8().data(), base->length(), argument->span8().data(), argument->length(), position), nullptr };
    [[clang::musttail]] return stringLastIndexOfInWideText(instance, cell, otherCell, position);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringIndexOfLiteral, size_t, (Instance* instance, JSCell* string, uint32_t text, int32_t fromIndex))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringIndexOfLiteral);
    JSString* base = cellArgument<JSString>(string, Entry::operationAOTStringIndexOfLiteral);
    StringView other = textOf(text);
    unsigned position = fromIndex;
    if (base->length() < static_cast<uint64_t>(other.length()) + position)
        OPERATION_RETURN(scope, notFound);
    if (other.length() == 1 && base->isRope() && base->length() >= JSString::minLengthForRopeWalk) {
        if (auto found = base->tryFindOneChar(globalObject, other[0], position))
            OPERATION_RETURN(scope, *found);
    }
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, characters->find(vm.adaptiveStringSearcherTables(), other, position));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringIndexOfCharacter, size_t, (Instance* instance, JSCell* string, uint32_t character, int32_t fromIndex))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringIndexOfCharacter);
    JSString* base = cellArgument<JSString>(string, Entry::operationAOTStringIndexOfCharacter);
    unsigned position = fromIndex;
    if (base->length() <= position)
        OPERATION_RETURN(scope, notFound);
    if (base->isRope() && base->length() >= JSString::minLengthForRopeWalk) {
        if (auto found = base->tryFindOneChar(globalObject, static_cast<char16_t>(character), position))
            OPERATION_RETURN(scope, *found);
    }
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, characters->find(static_cast<char16_t>(character), position));
}

static NEVER_INLINE OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stringLastIndexOfLiteralGeneric(Instance* instance, JSCell* cell, uint32_t text, int32_t position)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringLastIndexOfLiteral);
    JSString* string = cellArgument<JSString>(cell, Entry::operationAOTStringLastIndexOfLiteral);
    StringView other = textOf(text);
    unsigned length = string->length();
    unsigned otherLength = other.length();
    if (length < otherLength)
        OPERATION_RETURN(scope, notFound);
    unsigned start = std::min<unsigned>(std::max(position, 0), length - otherLength);
    if (otherLength == 1 && string->isRope() && length >= JSString::minLengthForRopeWalk) {
        if (auto found = string->tryFindLastOneChar(globalObject, other[0], start))
            OPERATION_RETURN(scope, *found);
    }
    auto characters = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, notFound);
    if (otherLength == 1)
        OPERATION_RETURN(scope, characters->reverseFind(other[0], start));
    if (!start)
        OPERATION_RETURN(scope, characters->startsWith(other) ? 0 : notFound);
    OPERATION_RETURN(scope, characters->reverseFind(other, start));
}

static NEVER_INLINE OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stringLastIndexOfLiteralInWideText(Instance* instance, JSCell* cell, uint32_t text, int32_t position)
{
    if (const StringImpl* impl = uncheckedDowncast<JSString>(cell)->tryGetValueImpl(); impl && !impl->is8Bit()) {
        StringView other = textOf(text);
        if (other.is8Bit()) [[likely]]
            return { reverseFindString(impl->span16().data(), impl->length(), other.span8().data(), other.length(), position), nullptr };
    }
    [[clang::musttail]] return stringLastIndexOfLiteralGeneric(instance, cell, text, position);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringLastIndexOfLiteral, size_t, (Instance* instance, JSCell* cell, uint32_t text, int32_t position))
{
    if (const StringImpl* impl = uncheckedDowncast<JSString>(cell)->tryGetValueImpl(); position <= 0 && impl && impl->is8Bit()) {
        StringView other = textOf(text);
        if (other.is8Bit()) [[likely]] {
            if (impl->length() < other.length())
                return { notFound, nullptr };
            return { WTF::equal(impl->span8().data(), other.span8()) ? 0 : notFound, nullptr };
        }
    }
    [[clang::musttail]] return stringLastIndexOfLiteralInWideText(instance, cell, text, position);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringEqualsLiteralInRegister, size_t, (Instance*, JSCell* cell, uint64_t text))
{
    auto* string = cellArgument<JSString>(cell, Entry::operationAOTStringEqualsLiteralInRegister);
    unsigned length = string->length();
    RELEASE_ASSERT(length && length <= maxRegisterStringLength);
    static_assert(maxRegisterStringLength == sizeof(uint64_t));
    std::array<Latin1Character, maxRegisterStringLength> characters { };
    if (string->is8Bit())
        string->resolveToBuffer(std::span { characters }.first(length));
    else {
        std::array<char16_t, maxRegisterStringLength> units;
        string->resolveToBuffer(std::span { units }.first(length));
        for (unsigned i = 0; i < length; ++i) {
            if (!WTF::isLatin1(units[i]))
                return false;
            characters[i] = static_cast<Latin1Character>(units[i]);
        }
    }
    if (length == 7)
        return (WTF::unalignedLoad<uint32_t>(characters.data()) | static_cast<uint64_t>(WTF::unalignedLoad<uint32_t>(characters.data() + 3)) << 32) == text;
    return WTF::unalignedLoad<uint64_t>(characters.data()) == text;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNarrowCharactersOfString, size_t, (Instance*, JSCell* cell))
{
    static thread_local NarrowCharactersOfString answer;
    auto* string = cellArgument<JSString>(cell, Entry::operationAOTNarrowCharactersOfString);
    unsigned length = string->length();
    answer.length = length;
    if (length > answer.characters.size())
        return reinterpret_cast<size_t>(&answer);
    if (string->is8Bit()) {
        string->resolveToBuffer(std::span { byteCast<Latin1Character>(answer.characters.data()), length });
        return reinterpret_cast<size_t>(&answer);
    }
    std::array<char16_t, std::tuple_size_v<decltype(answer.characters)>> units;
    string->resolveToBuffer(std::span { units }.first(length));
    for (unsigned i = 0; i < length; ++i) {
        if (!WTF::isLatin1(units[i])) {
            answer.length = NarrowCharactersOfString::lengthOfNoNarrowText;
            break;
        }
        answer.characters[i] = static_cast<uint8_t>(units[i]);
    }
    return reinterpret_cast<size_t>(&answer);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringEqualsLiteral, size_t, (Instance*, JSCell* cell, uint32_t text))
{
    auto* string = cellArgument<JSString>(cell, Entry::operationAOTStringEqualsLiteral);
    StringView expected = textOf(text);
    if (string->length() != expected.length())
        return false;
    if (!string->isRope())
        return StringView { *string->tryGetValueImpl() } == expected;
    if (string->is8Bit()) {
        Vector<Latin1Character, 64> characters(expected.length());
        string->resolveToBuffer(characters.mutableSpan());
        return StringView { characters.span() } == expected;
    }
    Vector<char16_t, 64> characters(expected.length());
    string->resolveToBuffer(characters.mutableSpan());
    return StringView { characters.span() } == expected;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIntToString, JSCell*, (Instance* instance, uint64_t bits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTIntToString);
    int64_t value = static_cast<int64_t>(bits);
    if (value == static_cast<int32_t>(value))
        OPERATION_RETURN(scope, instance->state->stringKeys->textOfInt32(vm, static_cast<int32_t>(value)));
    OPERATION_RETURN(scope, jsString(vm, String::number(static_cast<double>(value))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDoubleToString, JSCell*, (Instance* instance, double value))
{
    int32_t integer = truncateDoubleToInt32(value);
    if (integer == value && integer >= static_cast<int32_t>(NumericStrings::cacheSize)) {
        if (JSString* key = instance->state->stringKeys->tryFindIntegerKey(static_cast<uint32_t>(integer)))
            return key;
    }
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDoubleToString);
    if (integer == value)
        OPERATION_RETURN(scope, instance->state->stringKeys->textOfInt32(vm, integer));
    OPERATION_RETURN(scope, vm.numericStrings.addJSString(vm, value));
}

static ALWAYS_INLINE JSString* findStringOfWideCharacter(Instance& instance, char16_t character)
{
    const WideCharacterStrings& kept = instance.state->wideCharacterStrings;
    unsigned place = character % WideCharacterStrings::size;
    return kept.characters[place] == character ? kept.strings[place] : nullptr;
}

JSString* stringOfCharacter(Instance& instance, char16_t character)
{
    VM& vm = *instance.vm;
    if (character <= maxSingleCharacterString)
        return vm.smallStrings.singleCharacterString(static_cast<unsigned char>(character));
    if (JSString* kept = findStringOfWideCharacter(instance, character))
        return kept;
    JSString* made = jsSingleCharacterString(vm, character);
    WideCharacterStrings& strings = instance.state->wideCharacterStrings;
    unsigned place = character % WideCharacterStrings::size;
    strings.strings[place] = made;
    strings.characters[place] = character;
    return made;
}

static NEVER_INLINE JSCell* JIT_OPERATION_ATTRIBUTES makeStringOfCharacter(Instance* instance, uint32_t character)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCharToString);
    OPERATION_RETURN(scope, stringOfCharacter(*instance, static_cast<char16_t>(character)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCharToString, JSCell*, (Instance* instance, uint32_t character))
{
    if (character > maxSingleCharacterString) {
        if (JSString* kept = findStringOfWideCharacter(*instance, static_cast<char16_t>(character)))
            return kept;
    }
    AOT_MUST_TAIL_CALL return makeStringOfCharacter(instance, character);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    JSValue value = JSValue::decode(encodedValue);
    if (value.isString()) [[likely]]
        return { encodedValue, nullptr };
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringOfAnyValue);
    if (isJSArray(value)) {
        JSString* joined = tryConvertListToString(globalObject, asArray(value));
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (joined)
            OPERATION_RETURN(scope, JSValue::encode(joined));
    }
    OPERATION_RETURN(scope, JSValue::encode(stringConstructor(globalObject, value)));
}

static ALWAYS_INLINE std::tuple<unsigned, unsigned> computeTrimOffsets(StringView view, TrimmedEnds ends)
{
    switch (ends) {
    case TrimmedEnds::Start:
        return extractTrimOffsets<TrimKind::TrimStart>(view);
    case TrimmedEnds::End:
        return extractTrimOffsets<TrimKind::TrimEnd>(view);
    case TrimmedEnds::Both:
        return extractTrimOffsets<TrimKind::TrimBoth>(view);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringTrim, JSCell*, (Instance* instance, JSCell* cell, uint32_t ends))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringTrim);
    auto* string = cellArgument<JSString>(cell, Entry::operationAOTStringTrim);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto [left, right] = computeTrimOffsets(view, static_cast<TrimmedEnds>(ends));
    if (!left && right == view->length())
        OPERATION_RETURN(scope, string);
    if (!view->is8Bit()) {
        auto text = string->value(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
        OPERATION_RETURN(scope, jsString(vm, text->substringSharingImpl(left, right - left)));
    }
    OPERATION_RETURN(scope, jsSubstring(globalObject, string, left, right - left));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringLocaleCompare, size_t, (Instance* instance, JSCell* cell, JSCell* other))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringLocaleCompare);
    auto characters = cellArgument<JSString>(cell, Entry::operationAOTStringLocaleCompare)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto otherCharacters = cellArgument<JSString>(other, Entry::operationAOTStringLocaleCompare)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (globalObject->canDoASCIIUCADUCETLocaleCompare() && characters->is8Bit() && otherCharacters->is8Bit()) {
        if (auto result = compareASCIIWithUCADUCET(characters->span8(), otherCharacters->span8()))
            OPERATION_RETURN(scope, static_cast<size_t>(static_cast<int32_t>(*result)));
    }
    if (!ensureCollationIsAvailable(globalObject)) [[unlikely]]
        OPERATION_RETURN(scope, 0);
    auto* collator = globalObject->defaultCollator();
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, static_cast<size_t>(static_cast<int32_t>(collator->compareStrings(globalObject, characters, otherCharacters))));
}

enum class LetterCase : bool { Lower, Upper };

template<LetterCase letterCase>
static ALWAYS_INLINE auto asciiLettersOfOtherCase(auto characters)
{
    return SIMD::lessThan(SIMD::sub(characters, SIMD::splat<uint8_t>(letterCase == LetterCase::Lower ? 'A' : 'a')), SIMD::splat<uint8_t>(26));
}

static ALWAYS_INLINE auto accentedCapitals(auto characters)
{
    auto inRange = SIMD::lessThan(SIMD::sub(characters, SIMD::splat<uint8_t>(0xc0)), SIMD::splat<uint8_t>(0xdf - 0xc0));
    return SIMD::bitAnd(inRange, SIMD::bitNot(SIMD::equal(characters, SIMD::splat<uint8_t>(0xd7))));
}

template<LetterCase letterCase>
static ALWAYS_INLINE bool isLetterOfOtherCase(Latin1Character character)
{
    if constexpr (letterCase == LetterCase::Lower)
        return isASCIIUpper(character) || (character >= 0xc0 && character <= 0xde && character != 0xd7);
    else
        return isASCIILower(character);
}

template<LetterCase letterCase>
static ALWAYS_INLINE Latin1Character convertLatin1Character(Latin1Character character)
{
    if (!isLetterOfOtherCase<letterCase>(character))
        return character;
    return static_cast<Latin1Character>(letterCase == LetterCase::Lower ? character + 0x20 : character - 0x20);
}

struct EndsOfShortText {
    static ALWAYS_INLINE EndsOfShortText load(const uint8_t* from, size_t size)
    {
        if (size >= 8)
            return { WTF::unalignedLoad<uint64_t>(from), WTF::unalignedLoad<uint64_t>(from + size - 8) };
        if (size >= 4)
            return { WTF::unalignedLoad<uint32_t>(from), WTF::unalignedLoad<uint32_t>(from + size - 4) };
        return { WTF::unalignedLoad<uint16_t>(from), WTF::unalignedLoad<uint16_t>(from + size - 2) };
    }
    ALWAYS_INLINE void store(uint8_t* to, size_t size) const
    {
        if (size >= 8) {
            WTF::unalignedStore<uint64_t>(to, first);
            WTF::unalignedStore<uint64_t>(to + size - 8, last);
        } else if (size >= 4) {
            WTF::unalignedStore<uint32_t>(to, static_cast<uint32_t>(first));
            WTF::unalignedStore<uint32_t>(to + size - 4, static_cast<uint32_t>(last));
        } else {
            WTF::unalignedStore<uint16_t>(to, static_cast<uint16_t>(first));
            WTF::unalignedStore<uint16_t>(to + size - 2, static_cast<uint16_t>(last));
        }
    }
    bool isASCII() const { return !((first | last) & 0x8080808080808080ull); }
    friend bool operator==(const EndsOfShortText&, const EndsOfShortText&) = default;

    uint64_t first;
    uint64_t last;
};

template<LetterCase letterCase>
static ALWAYS_INLINE uint64_t convertASCIIBytes(uint64_t bytes)
{
    constexpr uint64_t ones = 0x0101010101010101ull;
    constexpr uint64_t firstLetter = letterCase == LetterCase::Lower ? 'A' : 'a';
    uint64_t atLeastFirst = bytes + ones * (0x80 - firstLetter);
    uint64_t aboveLast = bytes + ones * (0x80 - firstLetter - 26);
    return bytes ^ ((atLeastFirst & ~aboveLast & ones * 0x80) >> 2);
}

template<LetterCase letterCase>
static ALWAYS_INLINE JSString* convertCase(JSGlobalObject* globalObject, VM& vm, State& state, JSString* string)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr auto rememberedCase = letterCase == LetterCase::Lower ? CaseConversions::LetterCase::Lower : CaseConversions::LetterCase::Upper;
    if (const CaseConversions* conversions = state.instance.realm.caseConversions) {
        if (JSString* known = conversions->tryFind(string, rememberedCase))
            return known;
    }
    {
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        auto tryConvertLatin1 = [&](auto span) ALWAYS_INLINE_LAMBDA -> JSString* {
            const uint8_t* characters = byteCast<uint8_t>(span.data());
            size_t length = span.size();
            constexpr size_t stride = SIMD::stride<uint8_t>;
            if (!length)
                return string;
            if (length == 1) {
                if (!isASCII(characters[0]) && letterCase == LetterCase::Upper)
                    return nullptr;
                Latin1Character converted = convertLatin1Character<letterCase>(characters[0]);
                return converted == characters[0] ? string : vm.smallStrings.singleCharacterString(converted);
            }
            auto make = [&](auto&& writeTo) ALWAYS_INLINE_LAMBDA -> JSString* {
                if (length <= CaseConversions::maxLengthOfRememberedString) {
                    std::array<Latin1Character, CaseConversions::maxLengthOfRememberedString> converted;
                    writeTo(byteCast<uint8_t>(converted.data()));
                    CaseConversions& conversions = CaseConversions::ensure(state);
                    JSString* result = conversions.convertedString(vm, std::span<const Latin1Character> { converted }.first(length));
                    conversions.remember(string, rememberedCase, result);
                    return result;
                }
                std::span<Latin1Character> buffer;
                Ref impl = StringImpl::createUninitialized(length, buffer);
                writeTo(byteCast<uint8_t>(buffer.data()));
                return jsString(vm, String { WTF::move(impl) });
            };
            if (length < stride) {
                EndsOfShortText ends = EndsOfShortText::load(characters, length);
                if (ends.isASCII()) [[likely]] {
                    EndsOfShortText converted { convertASCIIBytes<letterCase>(ends.first), convertASCIIBytes<letterCase>(ends.last) };
                    if (converted == ends)
                        return string;
                    return make([&](uint8_t* to) ALWAYS_INLINE_LAMBDA {
                        converted.store(to, length);
                    });
                }
                if (letterCase == LetterCase::Upper)
                    return nullptr;
                bool changes = false;
                for (size_t i = 0; i < length; ++i)
                    changes |= isLetterOfOtherCase<letterCase>(characters[i]);
                if (!changes)
                    return string;
                return make([&](uint8_t* to) ALWAYS_INLINE_LAMBDA {
                    for (size_t i = 0; i < length; ++i)
                        to[i] = convertLatin1Character<letterCase>(characters[i]);
                });
            }
            auto forEachStride = [&](auto&& take) ALWAYS_INLINE_LAMBDA {
                size_t offset = 0;
                for (; offset + stride <= length; offset += stride)
                    take(offset);
                if (offset < length)
                    take(length - stride);
            };
            auto seen = SIMD::splat<uint8_t>(0);
            auto changed = SIMD::splat<uint8_t>(0);
            forEachStride([&](size_t offset) ALWAYS_INLINE_LAMBDA {
                auto loaded = SIMD::load(characters + offset);
                seen = SIMD::bitOr(seen, loaded);
                changed = SIMD::bitOr(changed, asciiLettersOfOtherCase<letterCase>(loaded));
            });
            bool isNotASCII = SIMD::isNonZero(SIMD::bitAnd(seen, SIMD::splat<uint8_t>(0x80)));
            if (isNotASCII && letterCase == LetterCase::Upper)
                return nullptr;
            if (isNotASCII) {
                forEachStride([&](size_t offset) ALWAYS_INLINE_LAMBDA {
                    changed = SIMD::bitOr(changed, accentedCapitals(SIMD::load(characters + offset)));
                });
            }
            if (!SIMD::isNonZero(changed))
                return string;
            return make([&](uint8_t* to) ALWAYS_INLINE_LAMBDA {
                forEachStride([&](size_t offset) ALWAYS_INLINE_LAMBDA {
                    auto loaded = SIMD::load(characters + offset);
                    auto letters = asciiLettersOfOtherCase<letterCase>(loaded);
                    if (isNotASCII)
                        letters = SIMD::bitOr(letters, accentedCapitals(loaded));
                    auto difference = SIMD::bitAnd(letters, SIMD::splat<uint8_t>(0x20));
                    SIMD::store(letterCase == LetterCase::Lower ? SIMD::bitOr(loaded, difference) : SIMD::sub(loaded, difference), to + offset);
                });
            });
        };
        if (JSString* converted = view->is8Bit() ? tryConvertLatin1(view->span8()) : nullptr)
            return converted;
    }
    auto characters = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    String converted = letterCase == LetterCase::Lower ? characters->convertToLowercaseWithoutLocale() : characters->convertToUppercaseWithoutLocale();
    JSString* result = converted.impl() == characters->impl() ? string : jsString(vm, WTF::move(converted));
    if (characters->length() > CaseConversions::maxLengthOfRememberedString)
        CaseConversions::ensure(state).remember(string, rememberedCase, result);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringToLowerCase, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringToLowerCase);
    OPERATION_RETURN(scope, convertCase<LetterCase::Lower>(globalObject, vm, *instance->state, cellArgument<JSString>(cell, Entry::operationAOTStringToLowerCase)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringToUpperCase, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringToUpperCase);
    OPERATION_RETURN(scope, convertCase<LetterCase::Upper>(globalObject, vm, *instance->state, cellArgument<JSString>(cell, Entry::operationAOTStringToUpperCase)));
}

template<typename CharacterType>
static void fillWithPattern(std::span<CharacterType> buffer, StringView pattern)
{
    unsigned length = buffer.size();
    unsigned copied = std::min(pattern.length(), length);
    pattern.left(copied).getCharacters(buffer.first(copied));
    while (copied < length) {
        unsigned count = std::min(copied, length - copied);
        memcpySpan(buffer.subspan(copied, count), buffer.first(count));
        copied += count;
    }
}

static JSString* createRepeatedString(JSGlobalObject* globalObject, StringView pattern, unsigned count)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto checkedLength = checkedProduct<unsigned>(pattern.length(), count);
    if (checkedLength.hasOverflowed() || checkedLength > JSString::MaxLength) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    unsigned length = checkedLength;

    std::span<Latin1Character> buffer;
    auto impl = StringImpl::tryCreateUninitialized(length, buffer);
    if (!impl) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    fillWithPattern(buffer, pattern);
    RELEASE_AND_RETURN(scope, jsString(vm, impl.releaseNonNull()));
}

static JSString* repeatAsRope(JSGlobalObject* globalObject, JSString* string, unsigned count)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (checkedProduct<unsigned>(string->length(), count).hasOverflowed() || string->length() * count > JSString::MaxLength) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    JSString* result = jsEmptyString(vm);
    JSString* operand = string;
    for (;;) {
        if (count & 1) {
            result = jsString(globalObject, result, operand);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        count >>= 1;
        if (!count)
            return result;
        operand = jsString(globalObject, operand, operand);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringRepeat, JSCell*, (Instance* instance, JSCell* cell, uint32_t count))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringRepeat);
    auto* string = cellArgument<JSString>(cell, Entry::operationAOTStringRepeat);
    if (!string->length() || !count)
        OPERATION_RETURN(scope, jsEmptyString(vm));
    if (count == 1)
        OPERATION_RETURN(scope, string);
    if (!string->is8Bit() && string->length() > 1)
        OPERATION_RETURN(scope, repeatAsRope(globalObject, string, count));
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (view->length() == 1) {
        char16_t unit = view[0];
        if (WTF::isLatin1(unit))
            OPERATION_RETURN(scope, repeatCharacter(globalObject, static_cast<Latin1Character>(unit), count));
        OPERATION_RETURN(scope, repeatCharacter(globalObject, unit, count));
    }
    OPERATION_RETURN(scope, createRepeatedString(globalObject, view, count));
}

template<typename CharacterType>
static JSString* createPaddedString(JSGlobalObject* globalObject, StringView string, StringView filler, unsigned length, bool isAtStart)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::span<CharacterType> buffer;
    auto impl = StringImpl::tryCreateUninitialized(length, buffer);
    if (!impl) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    unsigned stringLength = string.length();
    unsigned fillLength = length - stringLength;
    if (isAtStart) {
        fillWithPattern(buffer.first(fillLength), filler);
        string.getCharacters(buffer.last(stringLength));
    } else {
        string.getCharacters(buffer.first(stringLength));
        fillWithPattern(buffer.last(fillLength), filler);
    }
    RELEASE_AND_RETURN(scope, jsString(vm, impl.releaseNonNull()));
}

static constexpr unsigned maxLengthOfFlatPaddedString = 1024;

static JSString* padAsRope(JSGlobalObject* globalObject, JSString* string, JSString* filler, unsigned length, bool isAtStart)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    unsigned fillLength = length - string->length();
    JSString* fill;
    if (filler->length() == 1) {
        auto view = filler->view(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        char16_t unit = view[0];
        fill = WTF::isLatin1(unit) ? repeatCharacter(globalObject, static_cast<Latin1Character>(unit), fillLength) : repeatCharacter(globalObject, unit, fillLength);
        RETURN_IF_EXCEPTION(scope, nullptr);
    } else {
        unsigned count = fillLength / filler->length();
        fill = count ? repeatAsRope(globalObject, filler, count) : jsEmptyString(vm);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (unsigned rest = fillLength - count * filler->length()) {
            JSString* piece = jsSubstring(globalObject, filler, 0, rest);
            RETURN_IF_EXCEPTION(scope, nullptr);
            fill = jsString(globalObject, fill, piece);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }
    RELEASE_AND_RETURN(scope, isAtStart ? jsString(globalObject, fill, string) : jsString(globalObject, string, fill));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringPad, JSCell*, (Instance* instance, JSCell* cell, uint32_t length, JSCell* fillerCell, uint32_t isAtStart))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringPad);
    auto* string = cellArgument<JSString>(cell, Entry::operationAOTStringPad);
    auto* filler = cellArgument<JSString>(fillerCell, Entry::operationAOTStringPad);
    if (!filler->length() || length <= string->length())
        OPERATION_RETURN(scope, string);
    if (length > maxLengthOfFlatPaddedString && (!string->is8Bit() || !filler->is8Bit()))
        OPERATION_RETURN(scope, padAsRope(globalObject, string, filler, length, !!isAtStart));
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto pattern = filler->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (view->is8Bit() && pattern->is8Bit())
        OPERATION_RETURN(scope, createPaddedString<Latin1Character>(globalObject, view, pattern, length, !!isAtStart));
    OPERATION_RETURN(scope, createPaddedString<char16_t>(globalObject, view, pattern, length, !!isAtStart));
}

static bool isUnchangedByURICoding(StringView text, URICoding coding)
{
    static constexpr auto leftAloneInURI = makeLatin1CharacterBitSet("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$&'()*+,-./:;=?@_~"_s);
    static constexpr auto leftAloneInURIComponent = makeLatin1CharacterBitSet("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!'()*-._~"_s);
    if (!text.is8Bit())
        return false;
    switch (coding) {
    case URICoding::DecodeURI:
    case URICoding::DecodeURIComponent:
        return !text.contains('%');
    case URICoding::EncodeURI:
    case URICoding::EncodeURIComponent: {
        const auto& leftAlone = coding == URICoding::EncodeURI ? leftAloneInURI : leftAloneInURIComponent;
        return std::ranges::all_of(text.span8(), [&](Latin1Character character) {
            return leftAlone.get(character);
        });
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_JIT_OPERATION(operationAOTCodeURIOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue, uint32_t coding))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCodeURIOfAnyValue);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isString()) {
        auto view = asString(value)->view(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (isUnchangedByURICoding(view, static_cast<URICoding>(coding)))
            OPERATION_RETURN(scope, encodedValue);
    }
    OPERATION_RETURN(scope, encodedJSValue());
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* searchCell, JSCell* replacementCell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceString);
    JSString* string = cellArgument<JSString>(stringCell, Entry::operationAOTStringReplaceString);
    JSString* search = cellArgument<JSString>(searchCell, Entry::operationAOTStringReplaceString);
    JSString* replacement = cellArgument<JSString>(replacementCell, Entry::operationAOTStringReplaceString);
    if (JSString* result = tryReplaceOneCharUsingString<DollarCheck::Yes>(globalObject, string, search, replacement))
        OPERATION_RETURN(scope, result);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto stringView = string->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto searchView = search->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto replacementText = replacement->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, (stringReplaceStringString<StringReplaceSubstitutions::Yes, StringReplaceUseTable::No, BoyerMooreHorspoolTable<uint8_t>>(globalObject, string, stringView, searchView, replacement, replacementText, nullptr)));
}

static JSString* replaceRanges(JSGlobalObject* globalObject, VM& vm, StringView subject, std::span<const unsigned> matchStarts, unsigned matchLength, StringView replacement, unsigned resultLength)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<Latin1Character> buffer;
    auto impl = StringImpl::tryCreateUninitialized(resultLength, buffer);
    if (!impl) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    auto append = [&](StringView piece) ALWAYS_INLINE_LAMBDA {
        piece.getCharacters(buffer.first(piece.length()));
        skip(buffer, piece.length());
    };
    unsigned endOfLastMatch = 0;
    for (unsigned start : matchStarts) {
        append(subject.substring(endOfLastMatch, start - endOfLastMatch));
        append(replacement);
        endOfLastMatch = start + matchLength;
    }
    append(subject.substring(endOfLastMatch));
    RELEASE_AND_RETURN(scope, jsString(vm, impl.releaseNonNull()));
}

static JSString* tryReplaceAllWithPlainText(JSGlobalObject* globalObject, VM& vm, JSString* string, JSString* search, JSString* replacement)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    StringImpl* soughtImpl = search->tryGetValueImpl();
    StringImpl* replacementImpl = replacement->tryGetValueImpl();
    if (!soughtImpl || !replacementImpl || !soughtImpl->length())
        return nullptr;
    StringView sought { *soughtImpl };
    StringView replacementText { *replacementImpl };
    if (replacementText.find('$') != notFound)
        return nullptr;

    if (!string->is8Bit() || !replacementText.is8Bit())
        return nullptr;
    auto subject = string->view(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    unsigned matchLength = sought.length();
    auto findFrom = [&](unsigned start) ALWAYS_INLINE_LAMBDA {
        return matchLength == 1 ? subject->find(sought[0], start) : subject->find(vm.adaptiveStringSearcherTables(), sought, start);
    };
    size_t found = findFrom(0);
    if (found == notFound)
        return string;
    Vector<unsigned, 16> matchStarts;
    do {
        matchStarts.append(static_cast<unsigned>(found));
        found = findFrom(static_cast<unsigned>(found) + matchLength);
    } while (found != notFound);

    size_t resultLength = subject->length() - matchStarts.size() * matchLength + matchStarts.size() * static_cast<size_t>(replacementText.length());
    if (resultLength > JSString::MaxLength) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    if (!resultLength)
        return jsEmptyString(vm);
    RELEASE_AND_RETURN(scope, replaceRanges(globalObject, vm, subject, matchStarts.span(), matchLength, replacementText, static_cast<unsigned>(resultLength)));
}

static ALWAYS_INLINE std::optional<std::span<const Latin1Character>> tryGetLatin1Characters(JSString* string)
{
    if (const StringImpl* impl = string->tryGetValueImpl())
        return impl->is8Bit() ? std::optional(impl->span8()) : std::nullopt;
    if (!string->isSubstring())
        return std::nullopt;
    auto* piece = uncheckedDowncast<JSRopeString>(string);
    const StringImpl* whole = piece->substringBase()->tryGetValueImpl();
    if (!whole || !whole->is8Bit())
        return std::nullopt;
    return whole->span8().subspan(piece->substringOffset(), piece->length());
}

extern "C" int64_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTCompareStringsInPlace(JSCell* first, JSCell* second)
{
    auto firstCharacters = tryGetLatin1Characters(uncheckedDowncast<JSString>(first));
    auto secondCharacters = tryGetLatin1Characters(uncheckedDowncast<JSString>(second));
    if (!firstCharacters || !secondCharacters)
        return noOrderOfStringsInPlace;
    const Latin1Character* left = firstCharacters->data();
    const Latin1Character* right = secondCharacters->data();
    size_t commonLength = std::min(firstCharacters->size(), secondCharacters->size());
    size_t index = 0;
    for (; commonLength - index >= sizeof(uint64_t); index += sizeof(uint64_t)) {
        uint64_t leftWord = WTF::unalignedLoad<uint64_t>(left + index);
        uint64_t rightWord = WTF::unalignedLoad<uint64_t>(right + index);
        if (leftWord != rightWord)
            return std::byteswap(leftWord) < std::byteswap(rightWord) ? -1 : 1;
    }
    for (; index < commonLength; ++index) {
        if (left[index] != right[index])
            return left[index] < right[index] ? -1 : 1;
    }
    if (firstCharacters->size() == secondCharacters->size())
        return 0;
    return firstCharacters->size() < secondCharacters->size() ? -1 : 1;
}

static ALWAYS_INLINE bool containsShortText(std::span<const Latin1Character> characters, std::span<const Latin1Character> sought)
{
    ASSERT(!sought.empty());
    if (sought.size() > characters.size())
        return false;
    const Latin1Character* cursor = characters.data();
    const Latin1Character* endOfStarts = cursor + (characters.size() - sought.size() + 1);
    while ((cursor = static_cast<const Latin1Character*>(memchr(cursor, sought[0], endOfStarts - cursor)))) {
        if (WTF::equal(cursor + 1, sought.subspan(1)))
            return true;
        ++cursor;
    }
    return false;
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES replaceAllOfText(Instance* instance, JSCell* stringCell, JSCell* searchCell, JSCell* replacementCell)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringReplaceAllString);
    JSString* string = cellArgument<JSString>(stringCell, Entry::operationAOTStringReplaceAllString);
    JSString* search = cellArgument<JSString>(searchCell, Entry::operationAOTStringReplaceAllString);
    JSString* replacement = cellArgument<JSString>(replacementCell, Entry::operationAOTStringReplaceAllString);
    if (JSString* result = tryReplaceAllWithPlainText(globalObject, vm, string, search, replacement))
        OPERATION_RETURN(scope, result);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto stringView = string->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto searchView = search->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, replaceUsingStringSearch<StringReplaceMode::Global>(vm, globalObject, string, stringView, searchView, replacement));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceAllString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* searchCell, JSCell* replacementCell))
{
    static constexpr unsigned maxLengthOfTextSoughtByItsFirstCharacter = 8;
    const StringImpl* sought = uncheckedDowncast<JSString>(searchCell)->tryGetValueImpl();
    if (sought && sought->is8Bit() && sought->length() - 1 < maxLengthOfTextSoughtByItsFirstCharacter) [[likely]] {
        if (auto characters = tryGetLatin1Characters(uncheckedDowncast<JSString>(stringCell)); characters && !containsShortText(*characters, sought->span8()))
            return { stringCell, nullptr };
    }
    [[clang::musttail]] return replaceAllOfText(instance, stringCell, searchCell, replacementCell);
}

static JSArray* trySplitAtPlainText(JSGlobalObject* globalObject, VM& vm, JSString* string, JSString* separator, uint32_t limit)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    StringImpl* separatorImpl = separator->tryGetValueImpl();
    if (!separatorImpl || !separatorImpl->length() || !limit || !string->is8Bit() || globalObject->isHavingABadTime())
        return nullptr;
    if (StringImpl* impl = string->tryGetValueImpl(); StringSplitCache::canBeKey(impl)) {
        bool canBeCached = limit == std::numeric_limits<uint32_t>::max() && StringSplitCache::canBeKey(separatorImpl);
        if (auto* cache = canBeCached ? vm.stringSplitCache() : nullptr) {
            if (auto* butterfly = cache->getForKeys(impl, separatorImpl))
                return JSArray::createWithButterfly(vm, nullptr, globalObject->originalArrayStructureForIndexingType(CopyOnWriteArrayWithContiguous), butterfly->toButterfly());
        }
        if (impl->isAtom() || (canBeCached && StringSplitCache::isWorthCaching(*impl)))
            return nullptr;
    }

    Vector<unsigned, 16> ends;
    unsigned separatorLength = separatorImpl->length();
    {
        auto subject = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        StringView sought { *separatorImpl };
        size_t found;
        unsigned position = 0;
        auto findSeparator = [&]() ALWAYS_INLINE_LAMBDA -> size_t {
            if (separatorLength != 1)
                return subject->find(vm.adaptiveStringSearcherTables(), sought, position);
            if (!subject->is8Bit())
                return subject->find(sought[0], position);
            return findCharacter(subject->span8().data(), subject->length(), sought[0], position);
        };
        while ((found = findSeparator()) != notFound) {
            ends.append(static_cast<unsigned>(found));
            if (ends.size() == limit)
                break;
            position = static_cast<unsigned>(found) + separatorLength;
        }
        if (found == notFound)
            ends.append(subject->length());
    }
    if (ends.size() >= MIN_SPARSE_ARRAY_INDEX)
        return nullptr;

    JSString* base = string;
    unsigned start = 0;
    if (string->isSubstring()) {
        auto* rope = uncheckedDowncast<JSRopeString>(string);
        base = rope->substringBase();
        start = rope->substringOffset();
    }
    Structure* structure = globalObject->originalArrayStructureForIndexingType(ArrayWithContiguous);
    JSArray* result;
    {
        GCDeferralContext deferralContext(vm);
        ObjectInitializationScope initializationScope(vm);
        result = JSArray::tryCreateUninitializedRestricted(initializationScope, &deferralContext, structure, ends.size());
        if (result) [[likely]] {
            if (ends.size() == 1 && ends[0] == string->length())
                result->butterfly()->contiguous().at(result, 0).setWithoutWriteBarrier(string);
            else {
                unsigned offsetInBase = start;
                for (unsigned i = 0; i < ends.size(); ++i) {
                    unsigned end = offsetInBase + ends[i];
                    result->butterfly()->contiguous().at(result, i).setWithoutWriteBarrier(jsSubstringOfResolved(vm, &deferralContext, base, start, end - start));
                    start = end + separatorLength;
                }
            }
        }
    }
    if (!result) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope);
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringSplitString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* separatorCell, uint32_t limit))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringSplitString);
    JSString* string = cellArgument<JSString>(stringCell, Entry::operationAOTStringSplitString);
    JSString* separator = cellArgument<JSString>(separatorCell, Entry::operationAOTStringSplitString);
    if (JSArray* result = trySplitAtPlainText(globalObject, vm, string, separator, limit))
        OPERATION_RETURN(scope, result);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, stringSplitFast(globalObject, string, separator, limit));
}

struct ShortKeyCharacters {
    std::span<const Latin1Character> span() const LIFETIME_BOUND { return std::span { characters }.first(length); }

    std::array<Latin1Character, StringKeyCache::maxCachedKeyLength + maxJSONIntegerLength> characters;
    size_t length { 0 };
    bool hasRopePart { false };
    bool mayBeIndex { true };
};
static_assert(StringKeyCache::maxCachedKeyLength >= StringKeyCache::minReadableBytes);

static ALWAYS_INLINE bool isSubstringOfManyKeys(JSString* rope)
{
    uint8_t& word = *(std::bit_cast<uint8_t*>(rope) + JSString::offsetOfValue());
    if (!(word & JSRopeString::isSubstringInPointer))
        return false;
    if (word & UsesOfSubstringAsKey::two)
        return true;
    word += UsesOfSubstringAsKey::one;
    return false;
}

static ALWAYS_INLINE bool tryWriteShortKey(std::span<const EncodedJSValue> primitives, ShortKeyCharacters& key)
{
    constexpr size_t mostCharactersCopiedByWords = 16;
    Latin1Character* cursor = key.characters.data();
    Latin1Character* const limit = cursor + StringKeyCache::maxCachedKeyLength;
    for (EncodedJSValue encoded : primitives) {
        JSValue primitive = JSValue::decode(encoded);
        if (primitive.isString()) {
            JSString* string = asString(primitive);
            size_t size = string->length();
            if (size > static_cast<size_t>(limit - cursor) || !string->is8Bit())
                return false;
            if (const StringImpl* impl = string->tryGetValueImpl()) [[likely]] {
                std::span<const Latin1Character> from = impl->span8();
                if (size > mostCharactersCopiedByWords) [[unlikely]]
                    memcpy(cursor, from.data(), size);
                else
                    copyShortString(from, cursor);
                key.mayBeIndex &= !size || isASCIIDigit(from.back());
            } else {
                string->resolveToBuffer(std::span<Latin1Character> { cursor, size });
                key.hasRopePart |= !isSubstringOfManyKeys(string);
            }
            cursor += size;
            continue;
        }
        std::optional<int32_t> integer = primitive.tryGetAsInt32();
        if (!integer)
            return false;
        cursor = appendJSONInteger(cursor, *integer);
        if (cursor > limit)
            return false;
    }
    key.length = cursor - key.characters.data();
    return true;
}

static ALWAYS_INLINE bool tryJoinFewCharacters(std::span<const EncodedJSValue> primitives, StringKeyCache::FewCharacters& joined)
{
    bool mayBeIndex = true;
    for (EncodedJSValue encoded : primitives) {
        JSValue primitive = JSValue::decode(encoded);
        if (primitive.isInt32()) {
            if (!joined.tryAppendInteger(static_cast<uint32_t>(primitive.asInt32())))
                return false;
            continue;
        }
        if (!primitive.isString()) {
            std::optional<int32_t> integer = primitive.tryGetAsInt32();
            if (!integer || !joined.tryAppendInteger(static_cast<uint32_t>(*integer)))
                return false;
            continue;
        }
        const StringImpl* impl = asString(primitive)->tryGetValueImpl();
        if (!impl || !impl->is8Bit() || !joined.tryAppend(impl->span8()))
            return false;
        mayBeIndex &= !impl->length() || isASCIIDigit(impl->span8().back());
    }
    return joined.length >= 2 && (!mayBeIndex || joined.length > StringKeyCache::maxIndexLength);
}

static ALWAYS_INLINE JSValue onlyKeyPartWithCharacters(VM& vm, std::span<const EncodedJSValue> primitives)
{
    if (primitives.size() > 2)
        return { };
    EncodedJSValue empty = JSValue::encode(vm.smallStrings.emptyString());
    JSValue only;
    for (EncodedJSValue primitive : primitives) {
        if (primitive == empty)
            continue;
        if (only)
            return { };
        only = JSValue::decode(primitive);
    }
    return only;
}

static JSString* tryCreateStringKeyOfIndex(VM& vm, StringKeyCache& keys, std::span<const Latin1Character> characters)
{
    if (characters.empty() || characters.size() > StringKeyCache::maxIndexLength || !isASCIIDigit(characters.back()))
        return nullptr;
    std::optional<uint32_t> index = parseIndex(characters);
    if (!index || *index > INT32_MAX)
        return nullptr;
    return keys.textOfInt32(vm, static_cast<int32_t>(*index));
}

static JSString* tryConcatenateShortStringKey(VM& vm, Instance& instance, std::span<const EncodedJSValue> primitives)
{
    StringKeyCache& keys = *instance.state->stringKeys;
    if (JSValue only = onlyKeyPartWithCharacters(vm, primitives)) {
        if (only.isInt32())
            return keys.textOfInt32(vm, only.asInt32());
        if (only.isString())
            return asString(only);
    }
    ShortKeyCharacters key;
    if (!tryWriteShortKey(primitives, key))
        return nullptr;
    if (JSString* index = tryCreateStringKeyOfIndex(vm, keys, key.span()))
        return index;
    JSString* string = keys.findOrAdd(vm, key.span());
    if (!string) [[unlikely]]
        return nullptr;
    StringKeyCache::KeyByParts parts;
    if (parts.tryTake(keys.cageBase(), primitives))
        keys.cacheKeyByParts(parts, string);
    return string;
}

static JSString* concatenatePrimitives(JSGlobalObject* globalObject, Instance& instance, std::span<const EncodedJSValue> primitives, UseOfJoinedText use)
{
    VM& vm = globalObject->vm();
    if (use != UseOfJoinedText::Unknown) {
        if (JSString* key = tryConcatenateShortStringKey(vm, instance, primitives))
            return key;
    }
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue first = primitives.empty() ? JSValue() : JSValue::decode(primitives.front());
    constexpr size_t maxNumberOfPiecesAddedAtOnce = 8;
    if (first && first.isString() && asString(first)->isRope() && asString(first)->length() >= minLengthOfGrowingText && primitives.size() <= maxNumberOfPiecesAddedAtOnce + 1) {
        std::array<JSString*, maxNumberOfPiecesAddedAtOnce> pieces;
        size_t numberOfPieces = 0;
        for (EncodedJSValue primitive : primitives.subspan(1)) {
            pieces[numberOfPieces++] = JSValue::decode(primitive).toString(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
        if (JSString* grown = tryAppendToGrowingText(instance, asString(first), std::span<JSString* const> { pieces }.first(numberOfPieces)))
            return grown;
    }
    JSRopeString::RopeBuilder<RecordOverflow> parts(vm);
    for (EncodedJSValue primitive : primitives) {
        JSString* part = JSValue::decode(primitive).toString(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!parts.append(part)) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return nullptr;
        }
    }
    return parts.release();
}

static ALWAYS_INLINE UseOfJoinedText useOfJoinedTextOf(uint32_t isLikelyKey)
{
    ASSERT(isLikelyKey <= static_cast<uint32_t>(UseOfJoinedText::LikelyName));
    return static_cast<UseOfJoinedText>(isLikelyKey);
}

template<uint32_t count>
static ALWAYS_INLINE JSString* tryFindStringKeyByParts(StringKeyCache& keys, const EncodedJSValue* parts)
{
    StringKeyCache::KeyByParts taken;
    if (!taken.tryTake<count>(keys.cageBase(), parts)) [[unlikely]]
        taken.countAndKinds = 0;
    else if (JSString* key = keys.tryFindKeyByParts(taken)) [[likely]]
        return key;
    keys.lastLookupByParts() = taken;
    return nullptr;
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateByContents(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatenate);
    OPERATION_RETURN(scope, concatenatePrimitives(globalObject, *instance, unsafeMakeSpan(parts, count), useOfJoinedTextOf(isLikelyKey)));
}

static constexpr uint64_t boundOnTextOfPrimitiveThatIsNoCell = 32;

static bool textsSurelyFitInOneString(std::span<const EncodedJSValue> primitives)
{
    uint64_t length = 0;
    for (EncodedJSValue primitive : primitives) {
        JSValue value = JSValue::decode(primitive);
        if (value.isString())
            length += asString(value)->length();
        else if (value.isCell())
            return false;
        else
            length += boundOnTextOfPrimitiveThatIsNoCell;
    }
    return length <= JSString::MaxLength;
}

static ALWAYS_INLINE bool isTooLongForStringKey(std::span<const EncodedJSValue> primitives)
{
    uint64_t length = 0;
    for (EncodedJSValue primitive : primitives) {
        if (JSValue value = JSValue::decode(primitive); value.isString())
            length += asString(value)->length();
    }
    return length > StringKeyCache::maxCachedKeyLength;
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateShortStringKey(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    if (isTooLongForStringKey(unsafeMakeSpan(parts, count))) [[unlikely]]
        AOT_MUST_TAIL_CALL return concatenateByContents(instance, count, parts, static_cast<uint32_t>(UseOfJoinedText::Unknown));
    StringKeyCache& keys = *instance->state->stringKeys;
    StringKeyCache::FewCharacters joined;
    if (tryJoinFewCharacters(unsafeMakeSpan(parts, count), joined)) [[likely]] {
        unsigned hash = joined.hash();
        if (JSString* key = keys.tryFindCachedKey(joined, hash)) [[likely]] {
            if (keys.lastLookupByParts().countAndKinds)
                keys.cacheKeyByParts(keys.lastLookupByParts(), key);
            return { key, nullptr };
        }
        auto text = joined.text();
        std::span<const Latin1Character> characters = std::span { text }.first(joined.length);
        ASSERT(hash == RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters));
        AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatenate);
        if (JSString* made = keys.makeKey(vm, characters, hash, keys.lastLookupByParts(), useOfJoinedTextOf(isLikelyKey) == UseOfJoinedText::LikelyName)) [[likely]]
            return { made, nullptr };
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, static_cast<JSCell*>(nullptr));
    }
    ShortKeyCharacters characters;
    if (tryWriteShortKey(unsafeMakeSpan(parts, count), characters) && characters.length >= 2) [[likely]] {
        if (characters.hasRopePart)
            keys.lastLookupByParts().countAndKinds = 0;
        JSString* key;
        unsigned hash;
        switch (keys.findKeyForText(characters.span(), key, hash, characters.mayBeIndex)) {
        case StringKeyCache::Lookup::Found:
            if (keys.lastLookupByParts().countAndKinds)
                keys.cacheKeyByParts(keys.lastLookupByParts(), key);
            return { key, nullptr };
        case StringKeyCache::Lookup::Absent: {
            AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatenate);
            if (JSString* made = keys.makeKey(vm, characters.span(), hash, keys.lastLookupByParts(), useOfJoinedTextOf(isLikelyKey) == UseOfJoinedText::LikelyName)) [[likely]]
                return { made, nullptr };
            throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, static_cast<JSCell*>(nullptr));
        }
        case StringKeyCache::Lookup::AbsentIndex:
            break;
        }
    }
    AOT_MUST_TAIL_CALL return concatenateByContents(instance, count, parts, isLikelyKey);
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateOtherNumberOfParts(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    StringKeyCache& keys = *instance->state->stringKeys;
    if (JSValue only = onlyKeyPartWithCharacters(*instance->vm, unsafeMakeSpan(parts, count))) {
        JSString* key = only.isString() ? asString(only) : only.isInt32() && only.asInt32() >= 0 ? keys.tryFindIntegerKey(only.asInt32()) : nullptr;
        if (key)
            return { key, nullptr };
        AOT_MUST_TAIL_CALL return concatenateByContents(instance, count, parts, isLikelyKey);
    }
    StringKeyCache::KeyByParts taken;
    if (!taken.tryTake(keys.cageBase(), unsafeMakeSpan(parts, count)))
        taken.countAndKinds = 0;
    else if (JSString* key = keys.tryFindKeyByParts(taken))
        return { key, nullptr };
    keys.lastLookupByParts() = taken;
    AOT_MUST_TAIL_CALL return concatenateShortStringKey(instance, count, parts, isLikelyKey);
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcatenate, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey))
{
    if (!isLikelyKey) [[unlikely]]
        AOT_MUST_TAIL_CALL return concatenateByContents(instance, count, parts, isLikelyKey);
    StringKeyCache& keys = *instance->state->stringKeys;
    JSString* key;
    switch (count) {
    case 2: {
        EncodedJSValue empty = JSValue::encode(instance->vm->smallStrings.emptyString());
        if (parts[0] == empty || parts[1] == empty) [[unlikely]]
            AOT_MUST_TAIL_CALL return concatenateOtherNumberOfParts(instance, count, parts, isLikelyKey);
        key = tryFindStringKeyByParts<2>(keys, parts);
        break;
    }
    case 3:
        key = tryFindStringKeyByParts<3>(keys, parts);
        break;
    case 4:
        key = tryFindStringKeyByParts<4>(keys, parts);
        break;
    default:
        AOT_MUST_TAIL_CALL return concatenateOtherNumberOfParts(instance, count, parts, isLikelyKey);
    }
    if (key) [[likely]]
        return { key, nullptr };
    AOT_MUST_TAIL_CALL return concatenateShortStringKey(instance, count, parts, isLikelyKey);
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES makeTextOfIntegerInFrontOfNothing(Instance* instance, uint32_t, const EncodedJSValue* parts, uint32_t)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatenate);
    OPERATION_RETURN(scope, static_cast<JSCell*>(instance->state->stringKeys->textOfInt32(vm, JSValue::decode(parts[0]).asInt32())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcatenatePair, JSCell*, (Instance* instance, uint32_t isLikelyKey, const EncodedJSValue* parts, uint32_t))
{
    if (JSValue first = JSValue::decode(parts[0]); first.isInt32() && first.asInt32() >= 0 && parts[1] == JSValue::encode(instance->vm->smallStrings.emptyString()))
        AOT_MUST_TAIL_CALL return makeTextOfIntegerInFrontOfNothing(instance, isLikelyKey, parts, 0);
    instance->state->stringKeys->lastLookupByParts().takePair(parts[0], parts[1]);
    AOT_MUST_TAIL_CALL return concatenateShortStringKey(instance, 2, parts, isLikelyKey);
}

static ALWAYS_INLINE JSString* numberToJSONText(VM& vm, JSValue value)
{
    if (value.isInt32())
        return int32ToString(vm, value.asInt32(), 10);
    double number = value.asDouble();
    if (!std::isfinite(number))
        return vm.smallStrings.nullString();
    int32_t integer = truncateDoubleToInt32(number);
    if (integer == number)
        return int32ToString(vm, integer, 10);
    return vm.numericStrings.addJSString(vm, number);
}

static constexpr size_t numberOfQuotesInJSON = 2;
static constexpr size_t maxLengthOfEscapedCharacterInJSON = 6;
static constexpr size_t maxLengthOfStringQuotedHere = (String::MaxLength - numberOfQuotesInJSON) / maxLengthOfEscapedCharacterInJSON;
static constexpr size_t maxLengthOfStringEscapedHere = 16 * KB;
static constexpr size_t maxLengthOfStringCheckedFirst = SIMD::stride<uint8_t> - 1;

using BytesOfJSONText = decltype(SIMD::load(static_cast<const uint8_t*>(nullptr)));

static ALWAYS_INLINE BytesOfJSONText findEscapesInJSON(BytesOfJSONText input)
{
    constexpr auto quote = SIMD::splat<uint8_t>('"');
    constexpr auto backslash = SIMD::splat<uint8_t>('\\');
    constexpr auto space = SIMD::splat<uint8_t>(' ');
    return SIMD::bitOr(SIMD::equal(input, quote), SIMD::equal(input, backslash), SIMD::lessThan(input, space));
}

static ALWAYS_INLINE bool needsEscapeInJSON(uint64_t first, uint64_t last)
{
    std::array<uint64_t, 2> words { first, last };
    return SIMD::isNonZero(findEscapesInJSON(SIMD::load(std::bit_cast<const uint8_t*>(words.data()))));
}

static ALWAYS_INLINE bool copyAndCheckForEscapes(std::span<const Latin1Character> characters, Latin1Character* to)
{
    static_assert(maxLengthOfStringCheckedFirst + 1 == SIMD::stride<uint8_t>);
    RELEASE_ASSERT(characters.size() > maxLengthOfStringCheckedFirst);
    const Latin1Character* from = characters.data();
    size_t size = characters.size();
    constexpr size_t stride = SIMD::stride<uint8_t>;
    BytesOfJSONText found { };
    auto take = [&](size_t offset) ALWAYS_INLINE_LAMBDA {
        auto input = SIMD::load(std::bit_cast<const uint8_t*>(from + offset));
        SIMD::store(input, std::bit_cast<uint8_t*>(to + offset));
        found = SIMD::bitOr(found, findEscapesInJSON(input));
    };
    size_t offset = 0;
    for (; offset + 4 * stride <= size; offset += 4 * stride) {
        take(offset);
        take(offset + stride);
        take(offset + 2 * stride);
        take(offset + 3 * stride);
        if (SIMD::isNonZero(found)) [[unlikely]]
            return true;
    }
    for (; offset + stride <= size; offset += stride)
        take(offset);
    if (offset < size)
        take(size - stride);
    return SIMD::isNonZero(found);
}

static ALWAYS_INLINE bool shortStringNeedsEscapeInJSON(std::span<const Latin1Character> characters)
{
    const Latin1Character* from = characters.data();
    size_t size = characters.size();
    if (size >= 8)
        return needsEscapeInJSON(WTF::unalignedLoad<uint64_t>(from), WTF::unalignedLoad<uint64_t>(from + size - 8));
    if (size >= 4) {
        uint64_t both = WTF::unalignedLoad<uint32_t>(from) | static_cast<uint64_t>(WTF::unalignedLoad<uint32_t>(from + size - 4)) << 32;
        return needsEscapeInJSON(both, both);
    }
    bool found = false;
    for (size_t i = 0; i < size; ++i)
        found |= !!WTF::escapedFormsForJSON[from[i]];
    return found;
}

template<typename CharacterType>
static ALWAYS_INLINE JSString* quoteStringWithEscapesIn(VM& vm, std::span<CharacterType> text, std::span<const CharacterType> characters)
{
    text[0] = '"';
    std::span<CharacterType> output = text.subspan(1, characters.size() * maxLengthOfEscapedCharacterInJSON);
    WTF::appendEscapedJSONStringContent(output, characters);
    size_t length = output.data() - text.data();
    text[length++] = '"';
    return jsNontrivialString(vm, String { std::span<const CharacterType> { text.first(length) } });
}

template<typename CharacterType>
static NEVER_INLINE JSString* tryQuoteStringWithEscapes(VM& vm, std::span<const CharacterType> characters)
{
    Vector<CharacterType, 1024 / sizeof(CharacterType)> text;
    if (!text.tryGrow(characters.size() * maxLengthOfEscapedCharacterInJSON + numberOfQuotesInJSON)) [[unlikely]]
        return nullptr;
    return quoteStringWithEscapesIn(vm, text.mutableSpan(), characters);
}

static NEVER_INLINE JSString* quoteShortStringWithEscapes(VM& vm, std::span<const Latin1Character> characters)
{
    std::array<Latin1Character, maxLengthOfStringCheckedFirst * maxLengthOfEscapedCharacterInJSON + numberOfQuotesInJSON> text;
    return quoteStringWithEscapesIn(vm, std::span<Latin1Character> { text }, characters);
}

static NEVER_INLINE JSString* quoteStringInEngine(JSGlobalObject* globalObject, JSString* string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    String text = JSONStringify(globalObject, string, jsUndefined());
    RETURN_IF_EXCEPTION(scope, nullptr);
    return jsString(vm, WTF::move(text));
}

QuotedStringCache& QuotedStringCache::ensure(State& state)
{
    if (QuotedStringCache* cache = state.instance.realm.quotedStringCache) [[likely]]
        return *cache;
    QuotedStringCache* cache = state.tableArena.allocate<QuotedStringCache>(1);
    cache->cageBase = state.instance.vm->heap.cage().base();
    state.caches->addClearedEachCollection("quoted strings"_s, asMutableByteSpan(std::span { cache->entries }));
    state.instance.realm.quotedStringCache = cache;
    return *cache;
}

void QuotedStringCache::remember(const StringImpl& text, const JSString* quoted)
{
    if (std::bit_cast<uintptr_t>(quoted) - cageBase >= MarkedBlockCage::maxSize)
        return;
    unsigned hash = text.existingHash();
    entries[hash % numberOfEntries] = static_cast<uint64_t>(hash) << 32 | MarkedBlockCage::encode(cageBase, quoted);
}

static JSString* quoteString(JSGlobalObject* globalObject, State& state, JSString* string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    {
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (view->is8Bit() && view->length() <= maxLengthOfStringQuotedHere) [[likely]] {
            auto characters = view->span8();
            bool isCheckedFirst = characters.size() <= maxLengthOfStringCheckedFirst;
            if (isCheckedFirst && shortStringNeedsEscapeInJSON(characters)) [[unlikely]]
                return quoteShortStringWithEscapes(vm, characters);
            std::span<Latin1Character> quoted;
            RefPtr impl = StringImpl::tryCreateUninitialized(characters.size() + numberOfQuotesInJSON, quoted);
            if (!impl) [[unlikely]] {
                throwOutOfMemoryError(globalObject, scope);
                return nullptr;
            }
            bool hasEscapes = false;
            if (isCheckedFirst)
                copyShortString(characters, quoted.data() + 1);
            else
                hasEscapes = copyAndCheckForEscapes(characters, quoted.data() + 1);
            if (!hasEscapes) [[likely]] {
                quoted.front() = '"';
                quoted.back() = '"';
                JSString* result = jsNontrivialString(vm, String { impl.releaseNonNull() });
                if (const StringImpl* text = string->tryGetValueImpl(); text && QuotedStringCache::canRemember(*text))
                    QuotedStringCache::ensure(state).remember(*text, result);
                return result;
            }
            if (characters.size() <= maxLengthOfStringEscapedHere) {
                if (JSString* text = tryQuoteStringWithEscapes(vm, characters)) [[likely]]
                    return text;
            }
        } else if (!view->is8Bit() && view->length() <= maxLengthOfStringEscapedHere && !WTF::charactersAreAllLatin1(view->span16())) {
            if (JSString* text = tryQuoteStringWithEscapes(vm, view->span16())) [[likely]]
                return text;
        }
    }
    RELEASE_AND_RETURN(scope, quoteStringInEngine(globalObject, string));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES stringifyPrimitive(Instance* instance, EncodedJSValue encodedValue)
{
    constexpr Entry entry = Entry::operationAOTJSONStringifyPrimitive;
    AOT_OPERATION_BEGIN(instance, entry);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isCell())
        OPERATION_RETURN(scope, quoteString(globalObject, *instance->state, cellArgument<JSString>(value.asCell(), entry)));
    if (value.isNumber())
        OPERATION_RETURN(scope, numberToJSONText(vm, value));
    if (value.isBoolean())
        OPERATION_RETURN(scope, value.isTrue() ? vm.smallStrings.trueString() : vm.smallStrings.falseString());
    if (!value.isNull()) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, vm.smallStrings.nullString());
}

JSC_DEFINE_JIT_OPERATION(operationAOTJSONStringifyPrimitive, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    if (const QuotedStringCache* cache = instance->realm.quotedStringCache; cache && JSValue::decode(encodedValue).isString()) {
        const StringImpl* text = asString(JSValue::decode(encodedValue))->tryGetValueImpl();
        if (JSString* quoted = text && QuotedStringCache::canRemember(*text) ? cache->tryFind(*text) : nullptr) {
            if (instance->vm->aotState().counts) [[unlikely]]
                countEvent(*instance->vm, RuntimeCounterID::QuotedStringCacheHits);
            return { quoted, nullptr };
        }
    }
    [[clang::musttail]] return stringifyPrimitive(instance, encodedValue);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
