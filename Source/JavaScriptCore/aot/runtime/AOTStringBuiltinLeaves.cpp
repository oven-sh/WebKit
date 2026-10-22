/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTImageText.h"
#include <algorithm>
#include <bit>
#include <compare>
#include <wtf/SIMDUTF.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringCommon.h>
#include <wtf/text/StringImpl.h>
#include <wtf/text/StringView.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(static_cast<int32_t>(notFound) == -1, "compiled code takes the low 32 bits of a search result as the index or -1");

template<typename CharacterType>
static ALWAYS_INLINE bool canRepresent(uint32_t character)
{
    return !(character >> (8 * sizeof(CharacterType)));
}

template<typename CharacterType>
static ALWAYS_INLINE size_t findCharacter(const CharacterType* characters, uint32_t length, uint32_t character, uint32_t start)
{
    if (!canRepresent<CharacterType>(character))
        return notFound;
    return WTF::find(unsafeMakeSpan(characters, length), static_cast<CharacterType>(character), start);
}

template<typename CharacterType>
static ALWAYS_INLINE size_t reverseFindCharacter(const CharacterType* characters, uint32_t length, uint32_t character, int32_t position)
{
    if (position < 0 || !length || !canRepresent<CharacterType>(character))
        return notFound;
    return WTF::reverseFind(unsafeMakeSpan(characters, length), static_cast<CharacterType>(character), std::min<unsigned>(position, length - 1));
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
    CharacterType first = other[0];
    auto rest = unsafeMakeSpan(other + 1, otherLength - 1);
    auto starts = unsafeMakeSpan(characters, length - otherLength + 1);
    for (size_t i = start;; ++i) {
        i = WTF::find(starts, first, i);
        if (i == notFound || equal(characters + i + 1, rest))
            return i;
    }
}

template<typename CharacterType>
static ALWAYS_INLINE size_t reverseFindString(const CharacterType* characters, uint32_t length, const CharacterType* other, uint32_t otherLength, int32_t position)
{
    if (otherLength > length)
        return notFound;
    unsigned start = std::min<unsigned>(std::max(position, 0), length - otherLength);
    if (!otherLength)
        return start;
    CharacterType first = other[0];
    auto rest = unsafeMakeSpan(other + 1, otherLength - 1);
    for (size_t i = start;; --i) {
        if (characters[i] == first && equal(characters + i + 1, rest))
            return i;
        if (!i)
            return notFound;
    }
}

template<typename CharacterType>
static ALWAYS_INLINE size_t countSeparators(const CharacterType* characters, uint32_t length, uint32_t separator)
{
    if (!canRepresent<CharacterType>(separator))
        return 0;
    return WTF::countMatchedCharacters(unsafeMakeSpan(characters, length), static_cast<CharacterType>(separator));
}

template<typename CharacterType>
static ALWAYS_INLINE size_t findSeparators(const CharacterType* characters, uint32_t length, uint32_t separator, uint32_t* positions, uint32_t wanted)
{
    if (!wanted)
        return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < length; ++i) {
        if (characters[i] != separator)
            continue;
        positions[count] = i;
        if (++count == wanted)
            break;
    }
    return count;
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFindCharacter8(const Latin1Character* characters, uint32_t length, uint32_t character, uint32_t start)
{
    return findCharacter(characters, length, character, start);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringFindCharacter16(const char16_t* characters, uint32_t length, uint32_t character, uint32_t start)
{
    return findCharacter(characters, length, character, start);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringReverseFindCharacter8(const Latin1Character* characters, uint32_t length, uint32_t character, int32_t position)
{
    return reverseFindCharacter(characters, length, character, position);
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
    return reverseFindString(characters, length, other, otherLength, position);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringReverseFind16(const char16_t* characters, uint32_t length, const char16_t* other, uint32_t otherLength, int32_t position)
{
    return reverseFindString(characters, length, other, otherLength, position);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringUTF8Length8(const Latin1Character* characters, uint32_t length)
{
    static_assert(std::endian::native == std::endian::little);
    constexpr uint64_t highBits = 0x8080808080808080ULL;
    size_t result = length;
    uint32_t i = 0;
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

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTCountSeparators8(const Latin1Character* characters, uint32_t length, uint32_t separator)
{
    return countSeparators(characters, length, separator);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTCountSeparators16(const char16_t* characters, uint32_t length, uint32_t separator)
{
    return countSeparators(characters, length, separator);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTFindSeparators8(const Latin1Character* characters, uint32_t length, uint32_t separator, uint32_t* positions, uint32_t wanted)
{
    return findSeparators(characters, length, separator, positions, wanted);
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTFindSeparators16(const char16_t* characters, uint32_t length, uint32_t separator, uint32_t* positions, uint32_t wanted)
{
    return findSeparators(characters, length, separator, positions, wanted);
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

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTHashStringImpl(void* impl)
{
    return static_cast<StringImpl*>(impl)->hash();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
