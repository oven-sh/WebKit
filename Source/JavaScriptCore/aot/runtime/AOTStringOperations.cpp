/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTImageText.h"
#include "AOTInstance.h"
#include "AOTLimits.h"
#include "AOTOperationArguments.h"
#include "AOTOperationData.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyCachesStringKeys.h"
#include "AOTSlotRepresentation.h"
#include "AOTState.h"
#include "AOTStringEmitter.h"
#include "AOTThreshold.h"
#include "AOTUnwinding.h"
#include "AOTVector.h"
#include "IntlCollator.h"
#include "IntlObject.h"
#include "IntlObjectInlines.h"
#include "JSCInlines.h"
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
#include <wtf/SIMDUTF.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/AdaptiveStringSearcher.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static_assert(static_cast<int32_t>(notFound) == -1, "compiled code takes the low 32 bits of a search result as the index or -1");

static constexpr size_t maxRegisterStringLength = defaultLimitValue(LimitID::MaxRegisterStringLength, hostArchitecture());
static_assert(maxRegisterStringLength == sizeof(uint64_t));

static StringView textOf(uint32_t text)
{
    return Image::current()->text().view(StringID(text));
}

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
    if constexpr (std::is_same_v<CharacterType, Latin1Character>) {
        // A place where the text may begin has its first character, and its last one as far behind as the text is long. In ordinary text few places have both.
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
        if (characters[i] == first && WTF::equal(characters + i + 1, rest))
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

template<typename CharacterType>
static ALWAYS_INLINE bool hasTextAt(const CharacterType* characters, std::span<const Latin1Character> text)
{
    for (size_t k = 0; k < text.size(); ++k) {
        if (characters[k] != text[k])
            return false;
    }
    return true;
}

template<typename CharacterType>
static ALWAYS_INLINE size_t findLongSeparators(const CharacterType* characters, uint32_t length, std::span<const Latin1Character> text, uint32_t* positions, uint32_t wanted)
{
    ASSERT(!text.empty());
    if (!wanted)
        return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i + text.size() <= length;) {
        if (!hasTextAt(characters + i, text)) {
            ++i;
            continue;
        }
        positions[count] = i;
        if (++count == wanted)
            break;
        i += text.size();
    }
    return count;
}

template<typename CharacterType>
static ALWAYS_INLINE size_t countLongSeparators(const CharacterType* characters, uint32_t length, std::span<const Latin1Character> text)
{
    ASSERT(!text.empty());
    size_t count = 0;
    for (uint32_t i = 0; i + text.size() <= length;) {
        if (!hasTextAt(characters + i, text)) {
            ++i;
            continue;
        }
        ++count;
        i += text.size();
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

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTCharactersEqualImageString(const Latin1Character* characters, uint32_t length, uint32_t string)
{
    return Image::current()->text().equals(StringID(string), StringView { std::span { characters, length } });
}

extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTHashStringImpl(void* impl)
{
    return static_cast<StringImpl*>(impl)->hash();
}

// jsSubstringOfResolved() keeps the string that it makes of two ASCII characters. Makes none: 0 where none is kept.
extern "C" size_t AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTFindKeptStringOfTwoCharacters(void* vmPointer, uint32_t units)
{
    VM& vm = *static_cast<VM*>(vmPointer);
    Latin1Character characters[] = { static_cast<Latin1Character>(units), static_cast<Latin1Character>(units >> 16) };
    WTF::HashTranslatorCharBuffer<Latin1Character> buffer { std::span<const Latin1Character> { characters } };
    return std::bit_cast<size_t>(vm.keyAtomStringCache.make(vm, buffer, [](VM&, auto&) -> JSString* {
        return nullptr;
    }));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFindLongSeparators8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t text, uint32_t* positions, uint32_t wanted))
{
    return findLongSeparators(characters, length, textOf(text).span8(), positions, wanted);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFindLongSeparators16, size_t, (Instance*, const char16_t* characters, uint32_t length, uint32_t text, uint32_t* positions, uint32_t wanted))
{
    return findLongSeparators(characters, length, textOf(text).span8(), positions, wanted);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCountLongSeparators8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t text))
{
    return countLongSeparators(characters, length, textOf(text).span8());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCountLongSeparators16, size_t, (Instance*, const char16_t* characters, uint32_t length, uint32_t text))
{
    return countLongSeparators(characters, length, textOf(text).span8());
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringFindLiteral8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t text, uint32_t start))
{
    StringView other = textOf(text);
    return operationAOTStringFind8(characters, length, other.span8().data(), other.length(), start);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringReverseFindLiteral8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t text, int32_t position))
{
    StringView other = textOf(text);
    return operationAOTStringReverseFind8(characters, length, other.span8().data(), other.length(), position);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToLiteralString, EncodedJSValue, (Instance* instance, EncodedJSValue encoded))
{
    JSValue value = JSValue::decode(encoded);
    return value && value.isString() ? static_cast<EncodedJSValue>(instance->state->strings->literalStringWordFor(asString(value))) : encoded;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLiteralStringToString, JSCell*, (Instance* instance, uint64_t word))
{
    AOT_OPERATION_BEGIN_WITHOUT_COLLECTION(instance, Entry::operationAOTLiteralStringToString);
    OPERATION_RETURN(scope, instance->state->strings->stringForLiteralStringWord(word));
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcat, JSCell*, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcat);
    JSString* leftString = cellArgument<JSString>(left, Entry::operationAOTConcat);
    JSString* rightString = cellArgument<JSString>(right, Entry::operationAOTConcat);
    OPERATION_RETURN(scope, jsString(globalObject, leftString, rightString));
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcatThree, JSCell*, (Instance* instance, JSCell* first, JSCell* second, JSCell* third))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatThree);
    JSString* firstString = cellArgument<JSString>(first, Entry::operationAOTConcatThree);
    JSString* secondString = cellArgument<JSString>(second, Entry::operationAOTConcatThree);
    JSString* thirdString = cellArgument<JSString>(third, Entry::operationAOTConcatThree);
    OPERATION_RETURN(scope, jsString(globalObject, firstString, secondString, thirdString));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSubstring, JSCell*, (Instance* instance, JSCell* string, int32_t from, int32_t to))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSubstring);
    leaveCellsForRopes(instance);
    OPERATION_RETURN(scope, jsSubstring(globalObject, cellArgument<JSString>(string, Entry::operationAOTSubstring), from, to - from));
}

JSC_DEFINE_JIT_OPERATION(operationAOTFlatten, void*, (Instance* instance, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFlatten);
    JSString* cell = cellArgument<JSString>(string, Entry::operationAOTFlatten);
    if (cell->isRope() && uncheckedDowncast<JSRopeString>(cell)->tryResolveFlatLatin1Parts(vm))
        OPERATION_RETURN(scope, cell->tryGetValueImpl());
    auto value = cell->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
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

static NEVER_INLINE OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stringHasAtGeneric(Instance* instance, JSCell* string, JSCell* other, int32_t position)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringHasAt);
    JSString* base = cellArgument<JSString>(string, Entry::operationAOTStringHasAt);
    JSString* argument = cellArgument<JSString>(other, Entry::operationAOTStringHasAt);
    unsigned length = base->length();
    unsigned otherLength = argument->length();
    if (position < 0 || otherLength > length || static_cast<unsigned>(position) > length - otherLength)
        OPERATION_RETURN(scope, false);
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    auto otherCharacters = argument->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->substring(position, otherLength) == otherCharacters.data);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringHasAt, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t position))
{
    const StringImpl* base = uncheckedDowncast<JSString>(string)->tryGetValueImpl();
    const StringImpl* argument = uncheckedDowncast<JSString>(other)->tryGetValueImpl();
    if (base && argument && base->is8Bit() && argument->is8Bit()) [[likely]] {
        unsigned length = base->length();
        unsigned otherLength = argument->length();
        if (position < 0 || otherLength > length || static_cast<unsigned>(position) > length - otherLength)
            return { false, nullptr };
        return { WTF::equal(base->span8().data() + position, argument->span8()), nullptr };
    }
    [[clang::musttail]] return stringHasAtGeneric(instance, string, other, position);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringStartsWithLiteral, size_t, (Instance* instance, JSCell* cell, uint32_t text))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringStartsWithLiteral);
    JSString* string = cellArgument<JSString>(cell, Entry::operationAOTStringStartsWithLiteral);
    StringView other = textOf(text);
    if (string->length() < other.length())
        OPERATION_RETURN(scope, false);
    auto characters = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->startsWith(other));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringEndsWithLiteral, size_t, (Instance* instance, JSCell* cell, uint32_t text))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringEndsWithLiteral);
    JSString* string = cellArgument<JSString>(cell, Entry::operationAOTStringEndsWithLiteral);
    StringView other = textOf(text);
    if (string->length() < other.length())
        OPERATION_RETURN(scope, false);
    auto characters = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->endsWith(other));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringIndexOfFrom, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t startPosition))
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

JSC_DEFINE_JIT_OPERATION(operationAOTStringLastIndexOf, size_t, (Instance* instance, JSCell* cell, JSCell* otherCell, int32_t position))
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

static NEVER_INLINE OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stringLastIndexOfLiteralGeneric(Instance*, JSCell*, uint32_t text, int32_t position);

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
    [[clang::musttail]] return stringLastIndexOfLiteralGeneric(instance, cell, text, position);
}

static OperationReturnType<size_t> JIT_OPERATION_ATTRIBUTES stringLastIndexOfLiteralGeneric(Instance* instance, JSCell* cell, uint32_t text, int32_t position)
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
    // As packStringIntoRegister packs.
    if (length == 7)
        return (WTF::unalignedLoad<uint32_t>(characters.data()) | static_cast<uint64_t>(WTF::unalignedLoad<uint32_t>(characters.data() + 3)) << 32) == text;
    return WTF::unalignedLoad<uint64_t>(characters.data()) == text;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTShortStringOfString, EncodedJSValue, (Instance*, EncodedJSValue encoded))
{
    auto* string = cellArgument<JSString>(JSValue::decode(encoded).asCell(), Entry::operationAOTShortStringOfString);
    unsigned length = string->length();
    RELEASE_ASSERT(length <= maxShortStringLength);
    std::array<char16_t, maxShortStringLength> units;
    string->resolveToBuffer(std::span { units }.first(length));
    for (unsigned i = 0; i < length; ++i) {
        if (!WTF::isLatin1(units[i]))
            return static_cast<EncodedJSValue>(noShortString);
    }
    return static_cast<EncodedJSValue>(shortStringWord(std::span<const char16_t> { units }.first(length)));
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
    if (value >= static_cast<int64_t>(NumericStrings::cacheSize) && value <= INT32_MAX) {
        StringKeyCache& keys = *instance->state->stringKeys;
        JSString* string = keys.tryFindIntegerKey(static_cast<uint32_t>(value));
        if (!string) {
            std::array<Latin1Character, 10> digits;
            size_t first = digits.size();
            for (uint32_t rest = static_cast<uint32_t>(value); rest; rest /= 10)
                digits[--first] = static_cast<Latin1Character>('0' + rest % 10);
            std::span characters = std::span<const Latin1Character> { digits }.subspan(first);
            if (!keys.wasAskedForIntegerKey(static_cast<uint32_t>(value))) {
                keys.cacheIntegerKey(vm, static_cast<uint32_t>(value), nullptr);
                OPERATION_RETURN(scope, jsNontrivialString(vm, String(StringImpl::create(characters))));
            }
            string = keys.makeIntegerKey(vm, static_cast<uint32_t>(value), characters);
            vm.numericStrings.setJSString(static_cast<int32_t>(value), string->tryGetValue(), string);
        }
        OPERATION_RETURN(scope, string);
    }
    if (value == static_cast<int32_t>(value)) {
        JSString* string = int32ToString(vm, static_cast<int32_t>(value), 10);
        string->tryGetValueImpl()->hash();
        OPERATION_RETURN(scope, string);
    }
    OPERATION_RETURN(scope, jsString(vm, String::number(static_cast<double>(value))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDoubleToString, JSCell*, (Instance* instance, double value))
{
    // Most numbers that become text are ids and counts. Their text is asked for by the integer, as the stub IntegerToString asks, so that every road gives the same string.
    int32_t integer = truncateDoubleToInt32(value);
    if (integer == value && integer >= static_cast<int32_t>(NumericStrings::cacheSize)) {
        if (JSString* key = instance->state->stringKeys->tryFindIntegerKey(static_cast<uint32_t>(integer)))
            return key;
        return operationAOTIntToString(instance, static_cast<uint64_t>(integer));
    }
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDoubleToString);
    if (integer == value)
        OPERATION_RETURN(scope, int32ToString(vm, integer, 10));
    OPERATION_RETURN(scope, vm.numericStrings.addJSString(vm, value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCharToString, JSCell*, (Instance* instance, uint32_t character))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCharToString);
    OPERATION_RETURN(scope, jsSingleCharacterString(vm, static_cast<char16_t>(character)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    JSValue value = JSValue::decode(encodedValue);
    if (value.isString()) [[likely]]
        return { encodedValue, nullptr };
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringOfAnyValue);
    OPERATION_RETURN(scope, JSValue::encode(stringConstructor(globalObject, value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringSplitPrefix, size_t, (Instance* instance, JSCell* string, JSCell* separator, uint32_t count, uint32_t* bounds))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringSplitPrefix);
    std::fill_n(bounds, 2 * static_cast<size_t>(count), static_cast<uint32_t>(absentBound));
    auto setPiece = [&](size_t index, unsigned start, unsigned end) {
        if (index >= count)
            return;
        bounds[2 * index] = start;
        bounds[2 * index + 1] = end;
    };
    auto subject = cellArgument<JSString>(string, Entry::operationAOTStringSplitPrefix)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto pattern = cellArgument<JSString>(separator, Entry::operationAOTStringSplitPrefix)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    unsigned length = subject->length();
    if (pattern->isEmpty()) {
        for (unsigned i = 0; i < std::min(count, length); ++i)
            setPiece(i, i, i + 1);
        OPERATION_RETURN(scope, static_cast<size_t>(length));
    }
    size_t pieceCount = 0;
    unsigned position = 0;
    size_t found;
    while ((found = subject->find(vm.adaptiveStringSearcherTables(), pattern, position)) != notFound) {
        setPiece(pieceCount++, position, static_cast<unsigned>(found));
        position = found + pattern->length();
    }
    setPiece(pieceCount, position, length);
    OPERATION_RETURN(scope, pieceCount + 1);
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

using LetterCase = StringKeyCache::LetterCase;

template<LetterCase letterCase>
static ALWAYS_INLINE JSString* convertCase(JSGlobalObject* globalObject, VM& vm, StringKeyCache& keys, JSString* string)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSString* known = keys.tryFindCaseConversion(string, letterCase))
        return known;
    {
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        auto convertIfASCII = [&](auto span) ALWAYS_INLINE_LAMBDA -> JSString* {
            auto* characters = span.data();
            size_t length = span.size();
            unsigned isNotASCII = 0;
            unsigned changes = 0;
            for (size_t i = 0; i < length; ++i) {
                isNotASCII |= characters[i] & ~0x7f;
                changes |= letterCase == LetterCase::Lower ? isASCIIUpper(characters[i]) : isASCIILower(characters[i]);
            }
            if (isNotASCII)
                return nullptr;
            if (!changes)
                return string;
            if (length == 1)
                return vm.smallStrings.singleCharacterString(static_cast<Latin1Character>(letterCase == LetterCase::Lower ? toASCIILower(characters[0]) : toASCIIUpper(characters[0])));
            if (length <= StringKeyCache::maxCaseConvertedStringLength) {
                std::array<Latin1Character, StringKeyCache::maxCaseConvertedStringLength> converted;
                for (size_t i = 0; i < length; ++i)
                    converted[i] = static_cast<Latin1Character>(letterCase == LetterCase::Lower ? toASCIILower(characters[i]) : toASCIIUpper(characters[i]));
                JSString* result = keys.caseConvertedString(vm, std::span<const Latin1Character> { converted }.first(length));
                keys.cacheCaseConversion(vm, string, letterCase, result);
                return result;
            }
            std::span<Latin1Character> buffer;
            Ref impl = StringImpl::createUninitialized(length, buffer);
            Latin1Character* converted = buffer.data();
            for (size_t i = 0; i < length; ++i)
                converted[i] = static_cast<Latin1Character>(letterCase == LetterCase::Lower ? toASCIILower(characters[i]) : toASCIIUpper(characters[i]));
            return jsString(vm, String { WTF::move(impl) });
        };
        if (JSString* converted = view->is8Bit() ? convertIfASCII(view->span8()) : nullptr)
            return converted;
    }
    auto characters = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    String converted = letterCase == LetterCase::Lower ? characters->convertToLowercaseWithoutLocale() : characters->convertToUppercaseWithoutLocale();
    if (converted.impl() == characters->impl())
        return string;
    return jsString(vm, WTF::move(converted));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringToLowerCase, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringToLowerCase);
    OPERATION_RETURN(scope, convertCase<LetterCase::Lower>(globalObject, vm, *instance->state->stringKeys, cellArgument<JSString>(cell, Entry::operationAOTStringToLowerCase)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringToUpperCase, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringToUpperCase);
    OPERATION_RETURN(scope, convertCase<LetterCase::Upper>(globalObject, vm, *instance->state->stringKeys, cellArgument<JSString>(cell, Entry::operationAOTStringToUpperCase)));
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

// The string stays as it is: a 16-bit rope may have 8-bit parts.
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

// String.prototype.padStart and padEnd make a longer string as a rope.
AOT_THRESHOLD(unsigned, maxLengthOfFlatPaddedString, 1024);

// The string and what fills the room beside it are the two parts of a rope: one of them may be an 8-bit string.
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

// A text that is sought as it stands in an 8-bit string and an 8-bit replacement that stands for itself: the two literals of most calls.
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

JSC_DEFINE_JIT_OPERATION(operationAOTStringReplaceAllString, JSCell*, (Instance* instance, JSCell* stringCell, JSCell* searchCell, JSCell* replacementCell))
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

// An 8-bit text cut at a flat text that is not empty.
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
        // stringSplitFast() keeps the answer.
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
        while ((found = separatorLength == 1 ? subject->find(sought[0], position) : subject->find(vm.adaptiveStringSearcherTables(), sought, position)) != notFound) {
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

    JSArray* result = JSArray::tryCreate(vm, globalObject->originalArrayStructureForIndexingType(ArrayWithContiguous), ends.size());
    if (!result) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    if (ends.size() == 1 && ends[0] == string->length()) {
        result->butterfly()->contiguous().at(result, 0).set(vm, result, string);
        return result;
    }
    // view() has made every rope flat but a piece of another string, whose pieces are pieces of that string.
    JSString* base = string;
    unsigned start = 0;
    if (string->isSubstring()) {
        auto* rope = uncheckedDowncast<JSRopeString>(string);
        base = rope->substringBase();
        start = rope->substringOffset();
    }
    unsigned offsetInBase = start;
    for (unsigned i = 0; i < ends.size(); ++i) {
        unsigned end = offsetInBase + ends[i];
        result->butterfly()->contiguous().at(result, i).set(vm, result, jsSubstringOfResolved(vm, base, start, end - start));
        start = end + separatorLength;
    }
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
