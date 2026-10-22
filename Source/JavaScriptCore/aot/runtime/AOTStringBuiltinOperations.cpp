/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "AOTStringBuiltinOperationData.h"
#include "IntlCollator.h"
#include "IntlObject.h"
#include "IntlObjectInlines.h"
#include "JSCInlines.h"
#include "StringConstructor.h"
#include "StringPrototypeInlines.h"
#include <algorithm>
#include <array>
#include <span>
#include <tuple>
#include <wtf/text/AdaptiveStringSearcher.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringsAreEqual, size_t, (Instance* instance, JSCell* first, JSCell* second))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringsAreEqual);
    JSString* firstString = cellArgument<JSString>(first, Entry::operationAOTStringsAreEqual);
    JSString* secondString = cellArgument<JSString>(second, Entry::operationAOTStringsAreEqual);
    OPERATION_RETURN(scope, firstString->equalResolvingRopes(secondString));
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
    OPERATION_RETURN(scope, jsSubstring(globalObject, string, left, right - left));
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
        if (JSString* converted = view->is8Bit() ? convertIfASCII(view->span8()) : convertIfASCII(view->span16()))
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
    OPERATION_RETURN(scope, convertCase<LetterCase::Lower>(globalObject, vm, instance->state->propertyCaches->stringKeys(), cellArgument<JSString>(cell, Entry::operationAOTStringToLowerCase)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringToUpperCase, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringToUpperCase);
    OPERATION_RETURN(scope, convertCase<LetterCase::Upper>(globalObject, vm, instance->state->propertyCaches->stringKeys(), cellArgument<JSString>(cell, Entry::operationAOTStringToUpperCase)));
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

template<typename CharacterType>
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

    std::span<CharacterType> buffer;
    auto impl = StringImpl::tryCreateUninitialized(length, buffer);
    if (!impl) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    fillWithPattern(buffer, pattern);
    RELEASE_AND_RETURN(scope, jsString(vm, impl.releaseNonNull()));
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

// `String(value)` where no test stands in front of the call. A string is its own answer, and that is asked first: nothing is set up for it, and no frame of a host function is made for the rest.
JSC_DEFINE_JIT_OPERATION(operationAOTStringOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    JSValue value = JSValue::decode(encodedValue);
    if (value.isString()) [[likely]]
        return { encodedValue, nullptr };
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringOfAnyValue);
    OPERATION_RETURN(scope, JSValue::encode(stringConstructor(globalObject, value)));
}

// decodeURI, decodeURIComponent, encodeURI, encodeURIComponent.
static bool isUnchangedByURICoding(StringView text, URICoding coding)
{
    static constexpr auto leftAloneInURI = makeLatin1CharacterBitSet("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$&'()*+,-./:;=?@_~"_s);
    static constexpr auto leftAloneInURIComponent = makeLatin1CharacterBitSet("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!'()*-._~"_s);
    switch (coding) {
    case URICoding::DecodeURI:
    case URICoding::DecodeURIComponent:
        return !text.contains('%');
    case URICoding::EncodeURI:
    case URICoding::EncodeURIComponent: {
        if (!text.is8Bit())
            return false;
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

JSC_DEFINE_JIT_OPERATION(operationAOTStringRepeat, JSCell*, (Instance* instance, JSCell* cell, uint32_t count))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringRepeat);
    auto* string = cellArgument<JSString>(cell, Entry::operationAOTStringRepeat);
    if (!string->length() || !count)
        OPERATION_RETURN(scope, jsEmptyString(vm));
    if (count == 1)
        OPERATION_RETURN(scope, string);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (view->length() == 1) {
        char16_t unit = view[0];
        if (isLatin1(unit))
            OPERATION_RETURN(scope, repeatCharacter(globalObject, static_cast<Latin1Character>(unit), count));
        OPERATION_RETURN(scope, repeatCharacter(globalObject, unit, count));
    }
    if (view->is8Bit())
        OPERATION_RETURN(scope, createRepeatedString<Latin1Character>(globalObject, view, count));
    OPERATION_RETURN(scope, createRepeatedString<char16_t>(globalObject, view, count));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringPad, JSCell*, (Instance* instance, JSCell* cell, uint32_t length, JSCell* fillerCell, uint32_t isAtStart))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringPad);
    auto* string = cellArgument<JSString>(cell, Entry::operationAOTStringPad);
    auto* filler = cellArgument<JSString>(fillerCell, Entry::operationAOTStringPad);
    if (!filler->length() || length <= string->length())
        OPERATION_RETURN(scope, string);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto pattern = filler->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    if (view->is8Bit() && pattern->is8Bit())
        OPERATION_RETURN(scope, createPaddedString<Latin1Character>(globalObject, view, pattern, length, !!isAtStart));
    OPERATION_RETURN(scope, createPaddedString<char16_t>(globalObject, view, pattern, length, !!isAtStart));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
