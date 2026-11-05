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
#include "AOTUnwinding.h"
#include "AOTVector.h"
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

// Only compared, never dereferenced.
static thread_local std::array<const JSString*, 4> ropesComparedInPieces;

// A rope made for one question is garbage after it. One that is asked about again is kept by somebody, and is flattened.
static bool isAskedAboutForTheFirstTime(const JSString* rope)
{
    return rope->isNonSubstringRope() && std::exchange(ropesComparedInPieces[(std::bit_cast<uintptr_t>(rope) / sizeof(JSRopeString)) % ropesComparedInPieces.size()], rope) != rope;
}

// Whether `argument` stands in `base` at `position`, where it fits. False behind an exception.
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

static ALWAYS_INLINE size_t findNarrowStringInWideOne(const char16_t* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, uint32_t start)
{
    if (start > length)
        return notFound;
    if (!otherLength)
        return start;
    if (otherLength > length - start)
        return notFound;
    auto starts = unsafeMakeSpan(characters, length - otherLength + 1);
    for (size_t i = start;; ++i) {
        i = WTF::find(starts, static_cast<char16_t>(other[0]), i);
        if (i == notFound || WTF::equal(other + 1, unsafeMakeSpan(characters + i + 1, otherLength - 1)))
            return i;
    }
}

// Both are flat, as nearly all are: nothing is made, nothing throws, and no table of a searcher is set up for a text of a few characters. Nothing: a rope, or a wide text that is looked for in a narrow one.
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
    // `text.endsWith(key + "=")` in a function that is handed both: the rope was made for this one question.
    bool answer = standsAt(globalObject, base, argument, base->length() - argument->length());
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsBoolean(answer)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringEndsWith, EncodedJSValue, (Instance* instance, JSCell* string, JSCell* other))
{
    const StringImpl* base = uncheckedDowncast<JSString>(string)->tryGetValueImpl();
    const StringImpl* argument = uncheckedDowncast<JSString>(other)->tryGetValueImpl();
    if (base && argument && base->is8Bit() && argument->is8Bit()) [[likely]] {
        unsigned length = base->length();
        unsigned otherLength = argument->length();
        return { JSValue::encode(jsBoolean(otherLength <= length && WTF::equal(base->span8().data() + (length - otherLength), argument->span8()))), nullptr };
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

JSC_DEFINE_JIT_OPERATION(operationAOTStringLastIndexOf, size_t, (Instance* instance, JSCell* cell, JSCell* otherCell, int32_t position))
{
    const StringImpl* base = uncheckedDowncast<JSString>(cell)->tryGetValueImpl();
    const StringImpl* argument = uncheckedDowncast<JSString>(otherCell)->tryGetValueImpl();
    if (base && argument && base->is8Bit() && argument->is8Bit()) [[likely]]
        return { reverseFindString(base->span8().data(), base->length(), argument->span8().data(), argument->length(), position), nullptr };
    [[clang::musttail]] return stringLastIndexOfGeneric(instance, cell, otherCell, position);
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNarrowCharactersOfString, size_t, (Instance*, JSCell* cell))
{
    // Nothing runs between the call and the compares that read it.
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

// Most numbers that become text are ids and counts. Their text is asked for by the integer, as the stub IntegerToString asks, so that every road gives the same string.
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
            auto* characters = span.data();
            size_t length = span.size();
            unsigned isNotASCII = 0;
            unsigned changes = 0;
            constexpr size_t stride = SIMD::stride<uint8_t>;
            size_t lengthInStrides = length - length % stride;
            if (lengthInStrides) {
                auto seen = SIMD::splat<uint8_t>(0);
                auto changed = SIMD::splat<uint8_t>(0);
                for (size_t i = 0; i < lengthInStrides; i += stride) {
                    auto loaded = SIMD::load(byteCast<uint8_t>(characters) + i);
                    seen = SIMD::bitOr(seen, loaded);
                    changed = SIMD::bitOr(changed, asciiLettersOfOtherCase<letterCase>(loaded));
                }
                isNotASCII = SIMD::isNonZero(SIMD::bitAnd(seen, SIMD::splat<uint8_t>(0x80)));
                changes = SIMD::isNonZero(changed);
            }
            for (size_t i = lengthInStrides; i < length; ++i) {
                isNotASCII |= characters[i] & ~0x7f;
                changes |= isLetterOfOtherCase<letterCase>(characters[i]);
            }
            if (isNotASCII && letterCase == LetterCase::Upper)
                return nullptr;
            if (isNotASCII && !changes) {
                auto changed = SIMD::splat<uint8_t>(0);
                for (size_t i = 0; i < lengthInStrides; i += stride)
                    changed = SIMD::bitOr(changed, accentedCapitals(SIMD::load(byteCast<uint8_t>(characters) + i)));
                changes = SIMD::isNonZero(changed);
            }
            if (!changes)
                return string;
            if (length == 1)
                return vm.smallStrings.singleCharacterString(convertLatin1Character<letterCase>(characters[0]));
            if (length <= CaseConversions::maxLengthOfRememberedString) {
                std::array<Latin1Character, CaseConversions::maxLengthOfRememberedString> converted;
                for (size_t i = 0; i < length; ++i)
                    converted[i] = convertLatin1Character<letterCase>(characters[i]);
                CaseConversions& conversions = CaseConversions::ensure(state);
                JSString* result = conversions.convertedString(vm, std::span<const Latin1Character> { converted }.first(length));
                conversions.remember(string, rememberedCase, result);
                return result;
            }
            std::span<Latin1Character> buffer;
            Ref impl = StringImpl::createUninitialized(length, buffer);
            Latin1Character* converted = buffer.data();
            auto convertStrides = [&](auto lettersOfOtherCase) ALWAYS_INLINE_LAMBDA {
                for (size_t i = 0; i < lengthInStrides; i += stride) {
                    auto loaded = SIMD::load(byteCast<uint8_t>(characters) + i);
                    auto difference = SIMD::bitAnd(lettersOfOtherCase(loaded), SIMD::splat<uint8_t>(0x20));
                    SIMD::store(letterCase == LetterCase::Lower ? SIMD::bitOr(loaded, difference) : SIMD::sub(loaded, difference), byteCast<uint8_t>(converted) + i);
                }
            };
            if (isNotASCII) {
                convertStrides([](auto loaded) ALWAYS_INLINE_LAMBDA {
                    return SIMD::bitOr(asciiLettersOfOtherCase<letterCase>(loaded), accentedCapitals(loaded));
                });
            } else {
                convertStrides([](auto loaded) ALWAYS_INLINE_LAMBDA {
                    return asciiLettersOfOtherCase<letterCase>(loaded);
                });
            }
            for (size_t i = lengthInStrides; i < length; ++i)
                converted[i] = convertLatin1Character<letterCase>(characters[i]);
            return jsString(vm, String { WTF::move(impl) });
        };
        if (JSString* converted = view->is8Bit() ? tryConvertLatin1(view->span8()) : nullptr)
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

// String.prototype.padStart and padEnd make a longer string as a rope.
static constexpr unsigned maxLengthOfFlatPaddedString = 1024;

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

struct ShortKeyCharacters {
    std::span<const Latin1Character> span() const LIFETIME_BOUND { return std::span { characters }.first(length); }

    std::array<Latin1Character, StringKeyCache::maxCachedKeyLength> characters;
    size_t length { 0 };
    bool hasRopePart { false };
};
static_assert(StringKeyCache::maxCachedKeyLength >= StringKeyCache::minReadableBytes);

// The text of 8-bit strings and int32 numbers that fit together.
static ALWAYS_INLINE bool tryWriteShortKey(std::span<const EncodedJSValue> primitives, ShortKeyCharacters& key)
{
    size_t length = 0;
    for (EncodedJSValue encoded : primitives) {
        JSValue primitive = JSValue::decode(encoded);
        std::span<Latin1Character> room = std::span { key.characters }.subspan(length);
        if (primitive.isString()) {
            JSString* string = asString(primitive);
            uint32_t partLength = string->length();
            if (partLength > room.size() || !string->is8Bit())
                return false;
            if (const StringImpl* impl = string->tryGetValueImpl()) [[likely]]
                memcpySpan(room, impl->span8());
            else {
                string->resolveToBuffer(room.first(partLength));
                key.hasRopePart = true;
            }
            length += partLength;
            continue;
        }
        std::optional<int32_t> integer = primitive.tryGetAsInt32();
        if (!integer)
            return false;
        unsigned partLength = lengthOfIntegerAsString(*integer);
        if (partLength > room.size())
            return false;
        writeIntegerToBuffer(*integer, room.first(partLength));
        length += partLength;
    }
    key.length = length;
    return true;
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
    // The pieces lie in the frame, where the collector sees them.
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

// No number, boolean, undefined or null has a longer text: "-0.0000012345678901234567" has 25 characters.
static constexpr uint64_t boundOnTextOfPrimitiveThatIsNoCell = 32;

// Of a BigInt only the conversion tells how long its text is.
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

// Behind that question only the lack of memory is left to throw for.
JSC_DEFINE_JIT_OPERATION(operationAOTTryConcatenate, EncodedJSValue, (Instance* instance, uint32_t count, const EncodedJSValue* parts))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTryConcatenate);
    std::span<const EncodedJSValue> primitives = unsafeMakeSpan(parts, count);
    if (!textsSurelyFitInOneString(primitives))
        OPERATION_RETURN(scope, JSValue::encode(jsBoolean(false)));
    JSString* text = concatenatePrimitives(globalObject, *instance, primitives, UseOfJoinedText::LikelyKey);
    OPERATION_RETURN(scope, text ? JSValue::encode(text) : encodedJSValue());
}

// Whoever comes here has left in lastLookupByParts() what it looked for in vain.
static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES concatenateShortStringKey(Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey)
{
    ShortKeyCharacters characters;
    StringKeyCache& keys = *instance->state->stringKeys;
    if (tryWriteShortKey(unsafeMakeSpan(parts, count), characters) && characters.length >= 2) [[likely]] {
        // A rope is made anew each time, so nobody asks for these parts again.
        if (characters.hasRopePart)
            keys.lastLookupByParts().countAndKinds = 0;
        JSString* key;
        unsigned hash;
        switch (keys.findKeyForText(characters.span(), key, hash)) {
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

// The usual road calls nothing, so it saves no registers.
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

// The stub ConcatenateKeyOfPair has looked for the pair. An operation begins in its own frame, so this one leaves by a tail call as the others do.
JSC_DEFINE_JIT_OPERATION(operationAOTConcatenatePair, JSCell*, (Instance* instance, uint32_t isLikelyKey, const EncodedJSValue* parts, uint32_t))
{
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

// ---- JSON.stringify of a string. Most texts have nothing to escape: such a text is copied once, into a string of its own length and two quotes, and looked through on the way.

static constexpr size_t numberOfQuotesInJSON = 2;
static constexpr size_t maxLengthOfEscapedCharacterInJSON = 6;
// The engine reserves room for the longest escaped form of a string before it reads the string, and throws if no string is that long.
static constexpr size_t maxLengthOfStringQuotedHere = (String::MaxLength - numberOfQuotesInJSON) / maxLengthOfEscapedCharacterInJSON;
// A longer text pays for the engine's set-up many times over, and the room for its longest escaped form is no small thing.
static constexpr size_t maxLengthOfStringEscapedHere = 16 * KB;
// It is checked in a few instructions, and one that has something to escape is spared an allocation that it cannot use.
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

// Whether something of it has to be escaped.
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
    // A long text that has something to escape mostly has it early, and all that was copied is copied again.
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
    for (size_t i = 0; i < size; ++i)
        to[i] = from[i];
}

// `text` has room for the longest escaped form.
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

static JSString* quoteString(JSGlobalObject* globalObject, JSString* string)
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
                return jsNontrivialString(vm, String { impl.releaseNonNull() });
            }
            if (characters.size() <= maxLengthOfStringEscapedHere) {
                if (JSString* text = tryQuoteStringWithEscapes(vm, characters)) [[likely]]
                    return text;
            }
        } else if (!view->is8Bit() && view->length() <= maxLengthOfStringEscapedHere && !WTF::charactersAreAllLatin1(view->span16())) {
            // The engine writes a wide text that has narrow characters alone as a narrow one.
            if (JSString* text = tryQuoteStringWithEscapes(vm, view->span16())) [[likely]]
                return text;
        }
    }
    RELEASE_AND_RETURN(scope, quoteStringInEngine(globalObject, string));
}

JSC_DEFINE_JIT_OPERATION(operationAOTJSONStringifyPrimitive, JSCell*, (Instance* instance, EncodedJSValue encodedValue))
{
    constexpr Entry entry = Entry::operationAOTJSONStringifyPrimitive;
    AOT_OPERATION_BEGIN(instance, entry);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isCell())
        OPERATION_RETURN(scope, quoteString(globalObject, cellArgument<JSString>(value.asCell(), entry)));
    if (value.isNumber())
        OPERATION_RETURN(scope, numberToJSONText(vm, value));
    if (value.isBoolean())
        OPERATION_RETURN(scope, value.isTrue() ? vm.smallStrings.trueString() : vm.smallStrings.falseString());
    if (!value.isNull()) [[unlikely]]
        crashOnWrongOperand(entry);
    OPERATION_RETURN(scope, vm.smallStrings.nullString());
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
