/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "IntlCollator.h"
#include "IntlObjectInlines.h"
#include "JSCInlines.h"
#include "NumberPrototype.h"
#include "ParseInt.h"
#include <wtf/UnalignedAccess.h>
#include <wtf/text/AdaptiveStringSearcher.h>
#include <wtf/text/RapidHash.h>

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringFindCharacter8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t character, uint32_t start))
{
    return WTF::find(unsafeMakeSpan(characters, length), static_cast<Latin1Character>(character), start);
}

template<typename CharacterType>
static ALWAYS_INLINE size_t findSeparators(const CharacterType* characters, uint32_t length, uint32_t separator, uint32_t* positions, uint32_t wanted)
{
    bool countsAll = wanted >> 31;
    wanted &= INT32_MAX;
    uint32_t count = 0;
    for (uint32_t i = 0; i < length; ++i) {
        if (characters[i] != separator)
            continue;
        if (count < wanted)
            positions[count] = i;
        if (countsAll)
            positions[wanted] = i;
        if (++count >= wanted && !countsAll)
            break;
    }
    return count;
}

template<typename CharacterType>
static ALWAYS_INLINE size_t findLongSeparators(const CharacterType* characters, uint32_t length, std::span<const uint8_t> text, uint32_t* positions, uint32_t wanted)
{
    bool countsAll = wanted >> 31;
    wanted &= INT32_MAX;
    uint32_t count = 0;
    for (uint32_t i = 0; i + text.size() <= length;) {
        bool isHere = true;
        for (size_t k = 0; k < text.size() && isHere; ++k)
            isHere = characters[i + k] == text[k];
        if (!isHere) {
            ++i;
            continue;
        }
        if (count < wanted)
            positions[count] = i;
        if (countsAll)
            positions[wanted] = i;
        if (++count >= wanted && !countsAll)
            break;
        i += text.size();
    }
    return count;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFindLongSeparators8, size_t, (Instance* instance, uint64_t characters, uint32_t length, uint32_t text, uint64_t positions, uint32_t wanted))
{
    return findLongSeparators(std::bit_cast<const Latin1Character*>(characters), length, instance->typed->data.stringInImage(text), std::bit_cast<uint32_t*>(positions), wanted);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFindLongSeparators16, size_t, (Instance* instance, uint64_t characters, uint32_t length, uint32_t text, uint64_t positions, uint32_t wanted))
{
    return findLongSeparators(std::bit_cast<const char16_t*>(characters), length, instance->typed->data.stringInImage(text), std::bit_cast<uint32_t*>(positions), wanted);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFindSeparators8, size_t, (Instance*, uint64_t characters, uint32_t length, uint32_t separator, uint64_t positions, uint32_t wanted))
{
    return findSeparators(std::bit_cast<const Latin1Character*>(characters), length, separator, std::bit_cast<uint32_t*>(positions), wanted);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFindSeparators16, size_t, (Instance*, uint64_t characters, uint32_t length, uint32_t separator, uint64_t positions, uint32_t wanted))
{
    return findSeparators(std::bit_cast<const char16_t*>(characters), length, separator, std::bit_cast<uint32_t*>(positions), wanted);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringUTF8Length8, size_t, (Instance*, const Latin1Character* characters, uint32_t length))
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringFind8, size_t, (Instance* instance, const Latin1Character* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, uint32_t start))
{
    if (!otherLength)
        return start;
    if (otherLength > length - start)
        return notFound;
    return static_cast<size_t>(static_cast<ptrdiff_t>(searchString(instance->vm->adaptiveStringSearcherTables(), unsafeMakeSpan(characters, length), unsafeMakeSpan(other, otherLength), start)));
}

static ALWAYS_INLINE StringView programString(Instance* instance, uint32_t text)
{
    return StringView { byteCast<Latin1Character>(instance->typed->data.stringInImage(text)) };
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringPart, JSCell*, (Instance* instance, JSCell* string, uint64_t startAndCount))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    int32_t count = static_cast<int32_t>(startAndCount >> 32);
    if (count <= 0)
        OPERATION_RETURN(scope, jsEmptyString(vm));
    OPERATION_RETURN(scope, jsSubstring(globalObject, uncheckedDowncast<JSString>(string), static_cast<uint32_t>(startAndCount), count));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringTrim, JSCell*, (Instance* instance, JSCell* cell, uint32_t ends))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* string = uncheckedDowncast<JSString>(cell);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    unsigned start = 0;
    unsigned end = view->length();
    if (ends & 1) {
        while (start < end && isStrWhiteSpace(view[start]))
            ++start;
    }
    if (ends & 2) {
        while (end > start && isStrWhiteSpace(view[end - 1]))
            --end;
    }
    OPERATION_RETURN(scope, jsSubstring(globalObject, string, start, end - start));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringStartsWithLiteral, size_t, (Instance* instance, JSCell* string, uint32_t text))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    StringView other = programString(instance, text);
    if (uncheckedDowncast<JSString>(string)->length() < other.length())
        OPERATION_RETURN(scope, false);
    auto characters = uncheckedDowncast<JSString>(string)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->startsWith(other));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringEndsWithLiteral, size_t, (Instance* instance, JSCell* string, uint32_t text))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    StringView other = programString(instance, text);
    if (uncheckedDowncast<JSString>(string)->length() < other.length())
        OPERATION_RETURN(scope, false);
    auto characters = uncheckedDowncast<JSString>(string)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->endsWith(other));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringIndexOfLiteral, size_t, (Instance* instance, JSCell* string, uint32_t text, int32_t place))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* base = uncheckedDowncast<JSString>(string);
    StringView other = programString(instance, text);
    unsigned position = place;
    if (base->length() < static_cast<uint64_t>(other.length()) + position)
        OPERATION_RETURN(scope, none);
    if (other.length() == 1 && base->isRope() && base->length() >= JSString::minLengthForRopeWalk) {
        if (auto found = base->tryFindOneChar(globalObject, other[0], position))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    size_t found = characters->find(vm.adaptiveStringSearcherTables(), other, position);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringFindLiteral8, size_t, (Instance* instance, const Latin1Character* characters, uint32_t length, uint32_t text, uint32_t start))
{
    auto other = instance->typed->data.stringInImage(text);
    return operationAOTTypedStringFind8(instance, characters, length, byteCast<Latin1Character>(other.data()), other.size(), start);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringReverseFindLiteral8, size_t, (Instance* instance, const Latin1Character* characters, uint32_t length, uint32_t text, int32_t position))
{
    auto other = instance->typed->data.stringInImage(text);
    return operationAOTTypedStringReverseFind8(instance, characters, length, byteCast<Latin1Character>(other.data()), other.size(), position);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringLastIndexOfLiteral, size_t, (Instance* instance, JSCell* cell, uint32_t text, int32_t position))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* string = uncheckedDowncast<JSString>(cell);
    StringView other = programString(instance, text);
    unsigned length = string->length();
    unsigned otherLength = other.length();
    if (length < otherLength)
        OPERATION_RETURN(scope, none);
    unsigned start = std::min<unsigned>(std::max(position, 0), length - otherLength);
    if (otherLength == 1 && string->isRope() && length >= JSString::minLengthForRopeWalk) {
        if (auto found = string->tryFindLastOneChar(globalObject, other[0], start))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, none);
    size_t found;
    if (otherLength == 1)
        found = characters->reverseFind(other[0], start);
    else if (!start)
        found = characters->startsWith(other) ? 0 : notFound;
    else
        found = characters->reverseFind(other, start);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedCharactersToCompareWithLiterals, void*, (Instance*, JSCell* cell))
{
    static thread_local std::array<Latin1Character, Typed::maxInlineStringCompareLength> characters;
    auto* string = uncheckedDowncast<JSString>(cell);
    unsigned length = string->length();
    if (length > characters.size())
        return nullptr;
    std::array<char16_t, Typed::maxInlineStringCompareLength> units;
    string->resolveToBuffer(std::span { units }.first(length));
    for (unsigned i = 0; i < length; ++i) {
        if (!isLatin1(units[i]))
            return nullptr;
        characters[i] = static_cast<Latin1Character>(units[i]);
    }
    return characters.data();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringEqualsLiteralInRegister, size_t, (Instance*, JSCell* cell, uint64_t text))
{
    auto* string = uncheckedDowncast<JSString>(cell);
    unsigned length = string->length();
    RELEASE_ASSERT(length && length <= Typed::maxRegisterStringLength);
    std::array<char16_t, Typed::maxRegisterStringLength> units;
    string->resolveToBuffer(std::span { units }.first(length));
    for (unsigned i = 0; i < length; ++i) {
        if (!isLatin1(units[i]))
            return false;
    }
    return Typed::packStringIntoRegister(std::span<const char16_t> { units }.first(length)) == text;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedShortStringOfString, EncodedJSValue, (Instance*, EncodedJSValue encoded))
{
    JSString* string = asString(JSValue::decode(encoded));
    unsigned length = string->length();
    RELEASE_ASSERT(length <= Typed::maxShortStringLength);
    std::array<char16_t, Typed::maxShortStringLength> units;
    string->resolveToBuffer(std::span { units }.first(length));
    for (unsigned i = 0; i < length; ++i) {
        if (!isLatin1(units[i]))
            return static_cast<EncodedJSValue>(Typed::noShortString);
    }
    return static_cast<EncodedJSValue>(Typed::shortStringWord(std::span<const char16_t> { units }.first(length)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringEqualsLiteral, size_t, (Instance* instance, JSCell* cell, uint32_t text))
{
    auto* string = uncheckedDowncast<JSString>(cell);
    auto expected = byteCast<Latin1Character>(instance->typed->data.stringInImage(text));
    if (string->length() != expected.size())
        return false;
    if (!string->isRope())
        return StringView { *string->tryGetValueImpl() } == StringView { expected };
    if (string->is8Bit()) {
        Vector<Latin1Character, 64> characters(expected.size());
        string->resolveToBuffer(characters.mutableSpan());
        return equalSpans(characters.span(), expected);
    }
    Vector<char16_t, 64> characters(expected.size());
    string->resolveToBuffer(characters.mutableSpan());
    return StringView { characters.span() } == StringView { expected };
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringsAreEqual, size_t, (Instance*, JSCell* first, JSCell* second))
{
    return uncheckedDowncast<JSString>(first)->equalResolvingRopes(uncheckedDowncast<JSString>(second));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringReverseFindCharacter8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, uint32_t character, int32_t position))
{
    if (!length)
        return notFound;
    for (size_t i = std::min<size_t>(std::max(position, 0), length - 1);; --i) {
        if (characters[i] == character)
            return i;
        if (!i)
            return notFound;
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringReverseFind8, size_t, (Instance*, const Latin1Character* characters, uint32_t length, const Latin1Character* other, uint32_t otherLength, int32_t position))
{
    if (otherLength > length)
        return notFound;
    size_t start = std::min<size_t>(std::max(position, 0), length - otherLength);
    if (!otherLength)
        return start;
    Latin1Character first = other[0];
    auto rest = unsafeMakeSpan(other + 1, otherLength - 1);
    for (size_t i = start;; --i) {
        if (characters[i] == first && equal(characters + i + 1, rest))
            return i;
        if (!i)
            return notFound;
    }
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringLastIndexOf, size_t, (Instance* instance, JSCell* cell, JSCell* otherCell, int32_t position))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* string = uncheckedDowncast<JSString>(cell);
    JSString* other = uncheckedDowncast<JSString>(otherCell);
    unsigned length = string->length();
    unsigned otherLength = other->length();
    if (length < otherLength)
        OPERATION_RETURN(scope, none);
    unsigned start = std::min<unsigned>(std::max(position, 0), length - otherLength);
    auto otherCharacters = other->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, none);
    if (otherLength == 1 && string->isRope() && length >= JSString::minLengthForRopeWalk) {
        if (auto found = string->tryFindLastOneChar(globalObject, otherCharacters[0], start))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, none);
    size_t found;
    if (otherLength == 1)
        found = characters->reverseFind(otherCharacters[0], start);
    else if (!start)
        found = characters->startsWith(otherCharacters) ? 0 : notFound;
    else
        found = characters->reverseFind(otherCharacters, start);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

enum class LetterCase : uint8_t { Lower, Upper };

template<LetterCase letterCase>
static ALWAYS_INLINE JSString* convertCase(JSGlobalObject* globalObject, VM& vm, Typed::State* state, JSString* string)
{
    auto scope = DECLARE_THROW_SCOPE(vm);
    {
        auto view = string->view(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (view->is8Bit()) {
            const Latin1Character* characters = view->span8().data();
            size_t length = view->length();
            unsigned isNotASCII = 0;
            unsigned changes = 0;
            for (size_t i = 0; i < length; ++i) {
                isNotASCII |= characters[i] & 0x80;
                changes |= letterCase == LetterCase::Lower ? isASCIIUpper(characters[i]) : isASCIILower(characters[i]);
            }
            if (!isNotASCII) {
                if (!changes)
                    return string;
                if (state && length >= 2 && length <= Typed::State::maxCaseConvertedStringLength) {
                    std::array<Latin1Character, Typed::State::maxCaseConvertedStringLength> converted;
                    for (size_t i = 0; i < length; ++i)
                        converted[i] = letterCase == LetterCase::Lower ? toASCIILower(characters[i]) : toASCIIUpper(characters[i]);
                    return state->caseConvertedString(vm, std::span<const Latin1Character> { converted }.first(length));
                }
                std::span<Latin1Character> buffer;
                Ref impl = StringImpl::createUninitialized(length, buffer);
                Latin1Character* converted = buffer.data();
                for (size_t i = 0; i < length; ++i)
                    converted[i] = letterCase == LetterCase::Lower ? toASCIILower(characters[i]) : toASCIIUpper(characters[i]);
                return jsString(vm, String { WTF::move(impl) });
            }
        }
    }
    auto characters = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    String converted = letterCase == LetterCase::Lower ? characters->convertToLowercaseWithoutLocale() : characters->convertToUppercaseWithoutLocale();
    if (converted.impl() == characters->impl())
        return string;
    return jsString(vm, WTF::move(converted));
}

template<typename CharacterType>
static void fillWithPattern(std::span<CharacterType> buffer, StringView pattern)
{
    if (pattern.length() == 1) {
        std::ranges::fill(buffer, static_cast<CharacterType>(pattern[0]));
        return;
    }
    size_t filled = std::min<size_t>(pattern.length(), buffer.size());
    pattern.left(filled).getCharacters(buffer.first(filled));
    while (filled < buffer.size()) {
        size_t more = std::min(filled, buffer.size() - filled);
        memcpySpan(buffer.subspan(filled, more), buffer.first(more));
        filled += more;
    }
}

template<typename CharacterType>
static JSString* tryCreatePadded(VM& vm, StringView string, StringView pattern, unsigned length, bool isAtStart)
{
    std::span<CharacterType> buffer;
    auto impl = StringImpl::tryCreateUninitialized(length, buffer);
    if (!impl) [[unlikely]]
        return nullptr;
    if (!string.isEmpty())
        string.getCharacters(isAtStart ? buffer.last(string.length()) : buffer.first(string.length()));
    fillWithPattern(isAtStart ? buffer.first(length - string.length()) : buffer.last(length - string.length()), pattern);
    return jsString(vm, impl.releaseNonNull());
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringRepeat, JSCell*, (Instance* instance, JSCell* cell, uint32_t count))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* string = uncheckedDowncast<JSString>(cell);
    unsigned length = string->length() * count;
    if (!length)
        OPERATION_RETURN(scope, jsEmptyString(vm));
    if (count == 1)
        OPERATION_RETURN(scope, string);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSString* result = view->is8Bit() ? tryCreatePadded<Latin1Character>(vm, { }, view, length, false) : tryCreatePadded<char16_t>(vm, { }, view, length, false);
    if (!result) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope, result);
}

static uint32_t stringBuilderLengthBias(unsigned prefixLength)
{
    constexpr unsigned unbiasedLimit = JSString::MaxLength - Typed::stringBuilderCapacity;
    return prefixLength > unbiasedLimit ? prefixLength - unbiasedLimit : 0;
}

static JSString* concatenateStringBuilder(Instance* instance, JSGlobalObject* globalObject, JSCell* prefixCell, const Latin1Character* buffer, uint32_t length, JSValue operand)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::array<JSString*, 3> strings;
    unsigned count = 0;
    uint32_t bias = 0;
    if (prefixCell) {
        auto* prefix = uncheckedDowncast<JSString>(prefixCell);
        bias = stringBuilderLengthBias(prefix->length());
        if (prefix->length())
            strings[count++] = prefix;
    }
    bool isWholeString = !count && !operand;
    std::span characters { buffer + bias, length - bias };
    if (characters.size() == 1)
        strings[count++] = jsSingleCharacterString(vm, characters[0]);
    else if (!characters.empty()) {
        uint32_t integer = 0;
        bool isInteger = isWholeString && characters.size() <= 9 && characters[0] != '0';
        for (size_t i = 0; i < characters.size() && isInteger; ++i) {
            isInteger = isASCIIDigit(characters[i]);
            integer = integer * 10 + (characters[i] - '0');
        }
        JSString* string = isInteger ? int32ToString(vm, static_cast<int32_t>(integer), 10) : isWholeString && characters.size() <= Typed::State::maxCachedKeyLength ? instance->typed->cachedKeyFor(vm, characters) : jsNontrivialString(vm, String(characters));
        if (isWholeString)
            string->tryGetValueImpl()->hash();
        strings[count++] = string;
    }
    if (operand) {
        JSString* string = operand.toString(globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (string->length())
            strings[count++] = string;
    }
    switch (count) {
    case 0:
        return jsEmptyString(vm);
    case 1:
        return strings[0];
    case 2:
        RELEASE_AND_RETURN(scope, jsString(globalObject, strings[0], strings[1]));
    default:
        RELEASE_AND_RETURN(scope, jsString(globalObject, strings[0], strings[1], strings[2]));
    }
}

static NEVER_INLINE OperationReturnType<void*> JIT_OPERATION_ATTRIBUTES flushStringBuilderGeneric(Instance*, JSCell* prefix, const Latin1Character* buffer, uint32_t length, EncodedJSValue operand);

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringBuilderFlush, void*, (Instance* instance, JSCell* prefix, const Latin1Character* buffer, uint32_t length, EncodedJSValue operand))
{
    if (!prefix && !operand && length >= 2 && length <= Typed::State::maxCachedKeyLength && !Options::useAOTOperationCounters()) [[likely]] {
        std::span characters { buffer, length };
        if (JSString* key = instance->typed->tryFindCachedKey(characters, RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters)))
            return { key, nullptr };
    }
    [[clang::musttail]] return flushStringBuilderGeneric(instance, prefix, buffer, length, operand);
}

static OperationReturnType<void*> JIT_OPERATION_ATTRIBUTES flushStringBuilderGeneric(Instance* instance, JSCell* prefix, const Latin1Character* buffer, uint32_t length, EncodedJSValue operand)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    countOperationNamed(instance, "operationAOTTypedStringBuilderFlush");
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* result = concatenateStringBuilder(instance, globalObject, prefix, buffer, length, JSValue::decode(operand));
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, std::bit_cast<void*>(std::bit_cast<uintptr_t>(result) | static_cast<uintptr_t>(stringBuilderLengthBias(result->length())) << 48));
}

static NEVER_INLINE OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilderGeneric(Instance*, JSCell* prefix, const Latin1Character* buffer, uint32_t length);

static NEVER_INLINE OperationReturnType<JSCell*> makeStringBuilderKey(Instance* instance, CallFrame* callFrame, std::span<const Latin1Character> characters, unsigned hash)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return { instance->typed->makeKey(vm, characters, hash), nullptr };
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringBuilderFinish, JSCell*, (Instance* instance, JSCell* prefix, const Latin1Character* buffer, uint32_t length))
{
    if (!prefix && length >= 2 && length <= Typed::State::maxCachedKeyLength && !Options::useAOTOperationCounters()) [[likely]] {
        std::span characters { buffer, length };
        unsigned hash = RapidHash::computeHashAndMaskTop8Bits<Latin1Character>(characters);
        if (JSString* key = instance->typed->tryFindCachedKey(characters, hash)) [[likely]]
            return { key, nullptr };
        return makeStringBuilderKey(instance, DECLARE_CALL_FRAME(*instance->vm), characters, hash);
    }
    [[clang::musttail]] return finishStringBuilderGeneric(instance, prefix, buffer, length);
}

static OperationReturnType<JSCell*> JIT_OPERATION_ATTRIBUTES finishStringBuilderGeneric(Instance* instance, JSCell* prefix, const Latin1Character* buffer, uint32_t length)
{
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    countOperationNamed(instance, "operationAOTTypedStringBuilderFinish");
    auto scope = DECLARE_THROW_SCOPE(vm);
    OPERATION_RETURN(scope, concatenateStringBuilder(instance, globalObject, prefix, buffer, length, { }));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringPad, JSCell*, (Instance* instance, JSCell* cell, uint32_t length, JSCell* filler, uint32_t isAtStart))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* string = uncheckedDowncast<JSString>(cell);
    if (!uncheckedDowncast<JSString>(filler)->length())
        OPERATION_RETURN(scope, string);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    auto pattern = uncheckedDowncast<JSString>(filler)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSString* result = view->is8Bit() && pattern->is8Bit() ? tryCreatePadded<Latin1Character>(vm, view, pattern, length, isAtStart) : tryCreatePadded<char16_t>(vm, view, pattern, length, isAtStart);
    if (!result) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringLocaleCompare, size_t, (Instance* instance, JSCell* cell, JSCell* other))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto string = uncheckedDowncast<JSString>(cell)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto that = uncheckedDowncast<JSString>(other)->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (globalObject->canDoASCIIUCADUCETLocaleCompare() && string->is8Bit() && that->is8Bit()) {
        if (auto result = compareASCIIWithUCADUCET(string->span8(), that->span8()))
            OPERATION_RETURN(scope, static_cast<size_t>(static_cast<int32_t>(*result)));
    }
    auto* collator = globalObject->defaultCollator();
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    OPERATION_RETURN(scope, static_cast<size_t>(static_cast<int32_t>(collator->compareStrings(globalObject, string, that))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringToLowerCase, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSString* converted = convertCase<LetterCase::Lower>(globalObject, vm, instance->typed, uncheckedDowncast<JSString>(cell));
    if (converted && converted != cell)
        instance->cacheCaseConversion(cell, converted, false);
    OPERATION_RETURN(scope, converted);
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringToUpperCase, JSCell*, (Instance* instance, JSCell* cell))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSString* converted = convertCase<LetterCase::Upper>(globalObject, vm, instance->typed, uncheckedDowncast<JSString>(cell));
    if (converted && converted != cell)
        instance->cacheCaseConversion(cell, converted, true);
    OPERATION_RETURN(scope, converted);
}

namespace Typed {

using namespace B3;

template<> struct OperationParameterType<const Latin1Character*> { using Type = Pointer; };
template<> struct OperationParameterType<const char16_t*> { using Type = Pointer; };

static constexpr unsigned maxFlatStringLength = 1024;

static bool areStrings(std::span<const Type> types)
{
    for (auto& type : types) {
        if (!type.isString())
            return false;
    }
    return true;
}

static bool isNumberOrNullish(const Type& type)
{
    return type.isNumber() || repOf(type) == Rep::OptionalDouble;
}

std::optional<Writes> writesOfInlineString(InlineBuiltin which, std::span<const Type> arguments)
{
    switch (which) {
    case InlineBuiltin::StringLength:
        return Writes::Nothing;
    case InlineBuiltin::StringAt:
    case InlineBuiltin::StringCharAt:
    case InlineBuiltin::StringCharCodeAt:
    case InlineBuiltin::StringCodePointAt:
        return writesIf(arguments.size() == 1 && areNumbers(arguments), Writes::RuntimeState);
    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr:
        return writesIf(!arguments.empty() && arguments[0].isNumber() && std::ranges::all_of(arguments.subspan(1), isNumberOrUndefined), Writes::RuntimeState);
    case InlineBuiltin::StringRepeat:
        return writesIf(arguments.size() == 1 && isNumberOrNullish(arguments[0]), Writes::RuntimeState);
    case InlineBuiltin::StringPadStart:
    case InlineBuiltin::StringPadEnd:
        return writesIf(!arguments.empty() && arguments.size() <= 2 && isNumberOrNullish(arguments[0]) && (arguments.size() == 1 || arguments[1].isString()), Writes::RuntimeState);
    case InlineBuiltin::StringLocaleCompare:
        return writesIf(arguments.size() == 1 && arguments[0].isString(), Writes::RuntimeState);
    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
    case InlineBuiltin::StringIncludes:
    case InlineBuiltin::StringIndexOf:
        return writesIf(!arguments.empty() && arguments[0].isString() && (arguments.size() == 1 || arguments[1].isNumber()), Writes::RuntimeState);
    case InlineBuiltin::StringLastIndexOf:
        return writesIf(!arguments.empty() && arguments[0].isString() && (arguments.size() == 1 || (arguments[1].isInteger() && arguments[1].integers.isSubsetOf(Range::int32()))), Writes::RuntimeState);
    case InlineBuiltin::StringToLowerCase:
    case InlineBuiltin::StringToUpperCase:
    case InlineBuiltin::StringTrim:
    case InlineBuiltin::StringTrimStart:
    case InlineBuiltin::StringTrimEnd:
    case InlineBuiltin::StringIsWellFormed:
    case InlineBuiltin::StringToWellFormed:
    case InlineBuiltin::StringNormalize:
        return writesIf(arguments.empty(), Writes::RuntimeState);
    case InlineBuiltin::StringConcat:
        return writesIf(!arguments.empty() && arguments.size() <= 2 && areStrings(arguments), Writes::RuntimeState);
    case InlineBuiltin::StringFromCharCode:
        return writesIf(arguments.size() == 1 && arguments[0].isNumber(), Writes::Nothing);
    case InlineBuiltin::StringFromCodePoint:
        return writesIf(arguments.size() == 1 && arguments[0].isInteger(), Writes::RuntimeState);
    case InlineBuiltin::CallStringConstructor:
        if (arguments.size() != 1)
            return std::nullopt;
        return arguments[0].hasObjects() ? Writes::Heap : Writes::RuntimeState;
    case InlineBuiltin::StringUTF8ByteLength:
        return writesIf(!arguments.empty() && arguments.size() <= 2 && (arguments.size() == 1 || arguments[1].isString()), Writes::RuntimeState);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Word32 Lowering::hasStringAt(const Characters& characters, Integer place, const Characters& other)
{
    return either<Word32>(hasBits(bitAnd(characters.flags, other.flags), StringImpl::flagIs8Bit()), [&] {
        return areBytesEqual(offsetBy(characters.data, place, 0), other.data, other.length);
    }, [&] {
        return AOT_TYPED_OPERATION(StringHasAt, characters.owner, other.owner, low32(place));
    });
}

Word32 Lowering::lowerStartsOrEndsWith(bool isAtEnd, const StringReceiver& receiver, ValueID other, std::optional<ValueID> position)
{
    auto text = constantASCIIString(other);
    if (text && text->empty())
        return boolean(true);
    if (text && !position && !m_isInLoop) {
        bool comparesInline = text->size() <= maxInlineComparedLiteralLength;
        LBasicBlock isTooShort = m_out.newBlock();
        LBasicBlock isLongEnough = m_out.newBlock();
        LBasicBlock byStub = comparesInline ? newColdBlock() : m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        if (comparesInline) {
            Characters characters = charactersOf(receiver, byStub);
            orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), byStub);
            m_out.branch(below(characters.length, integer(text->size())).value(), unsure(isTooShort), unsure(isLongEnough));
            m_out.appendTo(isLongEnough);
            Word32 isSame = hasText(isAtEnd ? offsetBy(characters.data, sub(characters.length, integer(text->size())), 0) : characters.data, *text);
            ensureStillAliveHere(characters.owner);
            join.add(isSame);
        } else {
            m_out.branch(below(stringLength(receiver), integer(text->size())).value(), unsure(isTooShort), unsure(isLongEnough));
            m_out.appendTo(isLongEnough);
            m_out.jump(byStub);
        }
        m_out.appendTo(isTooShort);
        join.add(boolean(false));
        m_out.appendTo(byStub);
        LValue number = m_out.constInt64(m_function.at(stringConstantOf(other)).string().index());
        auto effects = isAtEnd ? effectsForOperation<operationAOTTypedStringEndsWithLiteral>() : effectsForOperation<operationAOTTypedStringStartsWithLiteral>();
        join.add(Word32(m_out.castToInt32(callAllocationStub(isAtEnd ? Stub::StringEndsWithLiteral : Stub::StringStartsWithLiteral, { { number, stubTemporaryGPRs[0] }, { receiver.string.value(), stubTemporaryGPRs[2] } }, effects, true))));
        Word32 has = join.finish().as<Word32>();
        ensureStillAliveHere(receiver.string);
        return has;
    }
    LBasicBlock hasRoom = m_out.newBlock();
    LBasicBlock hasNoRoom = m_out.newBlock();
    Join join(m_out, Rep::Int32, Int32);
    auto findPosition = [&](Integer length, Integer otherLength) {
        Integer place = isAtEnd ? length : integer(0);
        if (position)
            place = relativeStringIndex(*position, length, false);
        if (isAtEnd)
            place = sub(place, otherLength);
        m_out.branch((isAtEnd ? greaterThanOrEqual(place, integer(0)) : lessThanOrEqual(add(place, otherLength), length)).value(), unsure(hasRoom), unsure(hasNoRoom));
        m_out.appendTo(hasNoRoom);
        join.add(boolean(false));
        m_out.appendTo(hasRoom);
        return place;
    };
    if (text) {
        Integer place = findPosition(stringLength(receiver), integer(text->size()));
        join.add(hasSubstringAt(receiver, place, other, *text));
        return join.finish().as<Word32>();
    }
    Characters characters = charactersOf(receiver);
    Characters otherCharacters = charactersOf(stringReceiver(other));
    Integer place = findPosition(characters.length, otherCharacters.length);
    join.add(hasStringAt(characters, place, otherCharacters));
    Word32 has = join.finish().as<Word32>();
    ensureStillAliveHere(characters.owner);
    ensureStillAliveHere(otherCharacters.owner);
    return has;
}

Word32 Lowering::lowerStringSearch(InlineBuiltin which, const StringReceiver& receiver, const Arguments& arguments)
{
    bool isReverse = which == InlineBuiltin::StringLastIndexOf;
    ValueID other = arguments[0];
    if (isReverse && arguments.size() > 1 && typeOf(arguments[1]).integers.maximum <= 0)
        return select(lowerStartsOrEndsWith(false, receiver, other, std::nullopt), word32(0), word32(-1));
    Word32 position = word32(isReverse ? INT32_MAX : 0);
    if (arguments.size() > 1)
        position = low32(isReverse ? in<Integer>(arguments[1]) : relativeStringIndex(arguments[1], stringLength(receiver), false));

    if (auto text = constantASCIIString(other); text && !text->empty() && !m_isInLoop) {
        LBasicBlock otherwise = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        Word32 number = word32(m_function.at(stringConstantOf(other)).string().index());
        Characters characters = charactersOf(receiver, otherwise);
        orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), otherwise);
        Pointer data = characters.data;
        Word32 length = low32(characters.length);
        if (text->size() == 1) {
            Word32 character = word32((*text)[0]);
            join.add(isReverse ? AOT_TYPED_OPERATION(StringReverseFindCharacter8, data, length, character, position) : AOT_TYPED_OPERATION(StringFindCharacter8, data, length, character, position));
        } else
            join.add(isReverse ? AOT_TYPED_OPERATION(StringReverseFindLiteral8, data, length, number, position) : AOT_TYPED_OPERATION(StringFindLiteral8, data, length, number, position));
        m_out.appendTo(otherwise);
        join.add(isReverse ? AOT_TYPED_OPERATION(StringLastIndexOfLiteral, receiver.string, number, position) : AOT_TYPED_OPERATION(StringIndexOfLiteral, receiver.string, number, position));
        Word32 found = join.finish().as<Word32>();
        ensureStillAliveHere(receiver.string);
        return found;
    }
    LBasicBlock genericPath = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    Characters characters = charactersOf(receiver, genericPath);
    orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), genericPath);
    Word32 length = low32(characters.length);
    if (auto text = constantASCIIString(other); text && text->size() == 1) {
        Word32 character = word32((*text)[0]);
        join.add(isReverse ? AOT_TYPED_OPERATION(StringReverseFindCharacter8, characters.data, length, character, position) : AOT_TYPED_OPERATION(StringFindCharacter8, characters.data, length, character, position));
    } else {
        Characters otherCharacters = charactersOf(stringReceiver(other), genericPath);
        orElse(hasBits(otherCharacters.flags, StringImpl::flagIs8Bit()), genericPath);
        Word32 otherLength = low32(otherCharacters.length);
        if (!isReverse && arguments.size() > 1) {
            LBasicBlock hasAtMostOnePosition = m_out.newBlock();
            LBasicBlock hasOnePosition = m_out.newBlock();
            LBasicBlock hasNoSlot = m_out.newBlock();
            LBasicBlock hasMorePositions = m_out.newBlock();
            Word32 left(m_out.sub(length.value(), position.value()));
            m_out.branch(belowOrEqual(left, otherLength).value(), unsure(hasAtMostOnePosition), unsure(hasMorePositions));
            m_out.appendTo(hasAtMostOnePosition);
            m_out.branch(equal(left, otherLength).value(), unsure(hasOnePosition), unsure(hasNoSlot));
            m_out.appendTo(hasOnePosition);
            Word32 isThere = areBytesEqual(offsetBy(characters.data, zeroExtended(position), 0), otherCharacters.data, otherCharacters.length);
            ensureStillAliveHere(otherCharacters.owner);
            join.add(select(isThere, position, word32(-1)));
            m_out.appendTo(hasNoSlot);
            join.add(word32(-1));
            m_out.appendTo(hasMorePositions);
        }
        Word32 found = isReverse ? AOT_TYPED_OPERATION(StringReverseFind8, characters.data, length, otherCharacters.data, otherLength, position) : AOT_TYPED_OPERATION(StringFind8, characters.data, length, otherCharacters.data, otherLength, position);
        ensureStillAliveHere(otherCharacters.owner);
        join.add(found);
    }
    m_out.appendTo(genericPath);
    CellPointer otherString = in<CellPointer>(other);
    join.add(isReverse ? AOT_TYPED_OPERATION(StringLastIndexOf, receiver.string, otherString, position) : AOT_TYPED_OPERATION(StringIndexOf, receiver.string, otherString, position));
    Word32 found = join.finish().as<Word32>();
    ensureStillAliveHere(receiver.string);
    return found;
}

std::optional<Operand> Lowering::lowerInlineString(InlineBuiltin which, ValueID result, const Value& value, Writes)
{
    Operand yielded = lowerStringBuiltin(which, result, value);
    if (isOnMainPath() && m_builtins.at(value.builtin()).result.kinds == BuiltinResult::String && !m_builtins.at(value.builtin()).result.arguments)
        didPassNullishTest(result, m_currentBlockID);
    return yielded;
}

ValueID Lowering::stringDefinedBy(ValueID id) const
{
    while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() != CastReason::Coerce)
        id = m_function.at(id).child(0);
    return id;
}

std::optional<Integer> Lowering::lengthReadBefore(ValueID string) const
{
    auto found = m_stringLengths.find(stringDefinedBy(string).index());
    if (found == m_stringLengths.end())
        return std::nullopt;
    for (auto& read : found->value) {
        if (dominates(read.block, m_currentBlockID))
            return Integer(read.length);
    }
    return std::nullopt;
}

void Lowering::didReadLength(ValueID string, Integer length)
{
    if (m_activeHandler)
        return;
    m_stringLengths.add(stringDefinedBy(string).index(), Vector<KnownStringLength, 1> { }).iterator->value.append(KnownStringLength { m_currentBlockID, length.value() });
}

void Lowering::didCreateStringFromCodeUnit(ValueID id, Operand string, const Vector<ValueFromBlock, 2>& codeUnits)
{
    m_stringCodeUnits.set(id.index(), StringCodeUnit { string.value, m_out.phi(Int64, codeUnits) });
}

std::optional<Integer> Lowering::stringCodeUnit(ValueID id, bool mayBeUntracked)
{
    if (m_stringCodeUnits.isEmpty())
        return std::nullopt;
    id = stringDefinedBy(id);
    auto found = m_stringCodeUnits.find(id.index());
    if (found == m_stringCodeUnits.end() || m_values[id.index()].value != found->value.string || found->value.mayBeUntracked != mayBeUntracked)
        return std::nullopt;
    return Integer(found->value.codeUnit);
}

Operand Lowering::lowerStringBuiltin(InlineBuiltin which, ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    auto& arguments = value.arguments;
    auto& stringType = m_program.at(m_program.types.string);
    auto genericPath = [&] {
        return asCell(callBuiltinFunction(value, builtinCallEffects(value)));
    };
    auto isIntegerWithin = [&](ValueID id, Range range) {
        return typeOf(id).isInteger() && typeOf(id).integers.isSubsetOf(range);
    };

    switch (which) {
    case InlineBuiltin::StringFromCharCode:
        if (isIntegerWithin(arguments[0], { 0, maxSingleCharacterString }))
            return ensure(CellPointer(singleCharacterString(toInt32(arguments[0]).value())), stringType, type);
        return ensure(stringFromCodeUnit(bitAnd(toInt32(arguments[0]), word32(UINT16_MAX))), stringType, type);
    case InlineBuiltin::StringFromCodePoint: {
        Integer codePoint = in<Integer>(arguments[0]);
        if (isIntegerWithin(arguments[0], { 0, maxSingleCharacterString }))
            return ensure(CellPointer(singleCharacterString(low32(codePoint).value())), stringType, type);
        return ensure(either<CellPointer>(belowOrEqual(codePoint, integer(maxSingleCharacterString)), [&] {
            return CellPointer(singleCharacterString(low32(codePoint).value()));
        }, genericPath), stringType, type);
    }
    case InlineBuiltin::CallStringConstructor: {
        if (!(typeOf(arguments[0]).primitives & Primitive::Symbol))
            return ensure(toStringCell(arguments[0]), stringType, type);
        Boxed boxed = in<Boxed>(arguments[0]);
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock notSymbol = m_out.newBlock();
        LBasicBlock isSymbol = newColdBlock();
        Join join(m_out, Rep::Cell, Int64);
        m_out.branch(isCell(boxed).value(), unsure(isCellCase), unsure(notSymbol));
        m_out.appendTo(isCellCase);
        m_out.branch(equal(loadCellType(asCell(boxed)), word32(SymbolType)).value(), rarely(isSymbol), usually(notSymbol));
        m_out.appendTo(notSymbol);
        join.add(toStringCell(arguments[0]));
        m_out.appendTo(isSymbol);
        join.add(genericPath());
        return ensure(join.finish().as<CellPointer>(), stringType, type);
    }
    case InlineBuiltin::StringUTF8ByteLength: {
        auto byFunction = [&] {
            return ensure(callBuiltinFunction(value, builtinCallEffects(value)), type);
        };
        if (arguments.size() > 1) {
            auto encoding = constantASCIIString(arguments[1]);
            if (!encoding || !(equalSpans(*encoding, "utf8"_span8) || equalSpans(*encoding, "utf-8"_span8)))
                return byFunction();
        }
        Rep rep = repOf(type);
        LBasicBlock otherwise = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        StringReceiver string = typeOf(arguments[0]).isString() ? stringReceiver(arguments[0]) : stringReceiver(speculateOrElse(Expectation { arguments[0], &stringType }, otherwise).as<CellPointer>());
        Characters characters = charactersOf(string);
        orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), otherwise);
        Integer bytes = zeroExtended(AOT_TYPED_OPERATION(StringUTF8Length8, characters.data, low32(characters.length)));
        ensureStillAliveHere(string.string);
        join.add(ensure(bytes, Type::integer({ 0, 2 * static_cast<int64_t>(INT32_MAX) }), type));
        m_out.appendTo(otherwise);
        join.add(byFunction());
        return join.finish();
    }
    default:
        break;
    }

    if (which == InlineBuiltin::StringLength) {
        if (auto text = constantASCIIString(value.child(0))) {
            int64_t length = text->size();
            return ensure(integer(length), Type::integer({ length, length }), type);
        }
    }
    bool readsAtIndex = which == InlineBuiltin::StringAt || which == InlineBuiltin::StringCharAt || which == InlineBuiltin::StringCharCodeAt || which == InlineBuiltin::StringCodePointAt;
    bool takesPart = which == InlineBuiltin::StringSlice || which == InlineBuiltin::StringSubstring || which == InlineBuiltin::StringSubstr;
    const Substring* part = readsAtIndex || takesPart || which == InlineBuiltin::StringLength ? substringCreatedBy(value.child(0)) : nullptr;
    const OpenedString* openedCapturedVariable = readsAtIndex && !part ? openedString(value.child(0)) : nullptr;
    if (openedCapturedVariable && (!openedCapturedVariable->capturedVariableCell || !openedCapturedVariable->isOpened || get(value.child(0)).rep == Rep::Cell || !typeOf(value.child(0)).mayBeUndefinedOrNull()))
        openedCapturedVariable = nullptr;
    auto storedInCapturedVariable = [&] {
        return Boxed(m_out.m_block->appendNew<VariableValue>(m_proc, B3::Get, m_out.origin(), openedCapturedVariable->capturedVariableCell));
    };
    StringReceiver receiver = part ? part->base : stringReceiver(openedCapturedVariable ? asCell(storedInCapturedVariable()) : nonNullish(value.child(0), m_program.stringOf(m_builtins.at(value.builtin()).nameName)));
    if (!part)
        receiver.id = value.child(0);
    switch (which) {
    case InlineBuiltin::StringLength: {
        if (part)
            return ensure(lengthOf(*part), Type::integer(Range::length()), type);
        if (auto length = lengthReadBefore(value.child(0)))
            return ensure(*length, Type::integer(Range::length()), type);
        Integer length = stringLength(receiver);
        if (isOnMainPath())
            didReadLength(value.child(0), length);
        return ensure(length, Type::integer(Range::length()), type);
    }

    case InlineBuiltin::StringAt:
    case InlineBuiltin::StringCharAt:
    case InlineBuiltin::StringCharCodeAt:
    case InlineBuiltin::StringCodePointAt: {
        Rep rep = repOf(type);
        LBasicBlock outside = newColdBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        std::optional<Word32> codeUnitFromFlatString;
        if (which == InlineBuiltin::StringCharCodeAt && !part && !openedStringCharacters(receiver)) {
            LBasicBlock flat = m_out.newBlock();
            LBasicBlock rope = newColdBlock();
            LBasicBlock isInside = m_out.newBlock();
            LBasicBlock narrow = m_out.newBlock();
            LBasicBlock wide = m_out.newBlock();
            LBasicBlock isInsideRope = newColdBlock();
            LBasicBlock hasUnit = m_out.newBlock();
            Integer place = positionOf(arguments[0]);
            Pointer fiber = fiberOf(receiver);
            m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), usually(flat), rarely(rope));
            m_out.appendTo(flat);
            Word32 isSubsetOf;
            if (auto known = lengthReadBefore(value.child(0)))
                isSubsetOf = below(place, *known);
            else if (isIntegerWithin(arguments[0], Range::int32()))
                isSubsetOf = below(low32(place), loadField<Word32>(fiber, m_heaps.StringImpl_length));
            else
                isSubsetOf = below(place, zeroExtended(loadField<Word32>(fiber, m_heaps.StringImpl_length)));
            m_out.branch(isSubsetOf.value(), usually(isInside), rarely(outside));
            m_out.appendTo(isInside);
            m_out.branch(hasBits(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed), StringImpl::flagIs8Bit()).value(), usually(narrow), rarely(wide));
            m_out.appendTo(narrow);
            ValueFromBlock fromNarrow = m_out.anchor(marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), offsetBy(loadField<Pointer>(fiber, m_heaps.StringImpl_data), place, 0).value())), IsFixed));
            m_out.jump(hasUnit);
            m_out.appendTo(wide);
            ValueFromBlock fromWide = m_out.anchor(marked(m_out.load16ZeroExt32(TypedPointer(m_heaps.characters16.atAnyIndex(), offsetBy(loadField<Pointer>(fiber, m_heaps.StringImpl_data), place, 1).value())), IsFixed));
            m_out.jump(hasUnit);
            m_out.appendTo(rope);
            LValue ofRope = callAllocationStub(Stub::RopeCodeUnit, { { receiver.string.value(), GPRInfo::toArgumentRegister(1) }, { place.value(), GPRInfo::toArgumentRegister(2) } }, effectsForOperation<operationAOTTypedRopeCodeUnit>(), true);
            m_out.branch(m_out.lessThan(ofRope, m_out.int64Zero), unsure(outside), unsure(isInsideRope));
            m_out.appendTo(isInsideRope);
            ValueFromBlock fromRope = m_out.anchor(m_out.castToInt32(ofRope));
            m_out.jump(hasUnit);
            m_out.appendTo(hasUnit);
            codeUnitFromFlatString = Word32(m_out.phi(Int32, fromNarrow, fromWide, fromRope));
            ensureStillAliveHere(receiver.string);
        }
        Characters characters;
        Integer index;
        if (!codeUnitFromFlatString) {
            characters = charactersOf(receiver);
            if (part)
                characters.length = lengthOf(*part);
            else if (auto length = lengthReadBefore(value.child(0)))
                characters.length = *length;
            else if (isOnMainPath())
                didReadLength(value.child(0), characters.length);
            index = positionOf(arguments[0]);
            if (which == InlineBuiltin::StringAt && !isIntegerWithin(arguments[0], Range::length()))
                index = select(lessThan(index, integer(0)), add(index, characters.length), index);
            if (isX86_64() && which != InlineBuiltin::StringAt && get(arguments[0]).rep != Rep::Int64) {
                LBasicBlock isInside = m_out.newBlock();
                LBasicBlock isNotInside = newColdBlock();
                LBasicBlock isNaN = newColdBlock();
                Number given = get(arguments[0]).as<Number>();
                Integer truncated = truncateToInteger(given);
                ValueFromBlock unchanged = m_out.anchor(truncated.value());
                m_out.branch(below(truncated, characters.length).value(), usually(isInside), rarely(isNotInside));
                m_out.appendTo(isNotInside);
                m_out.branch(equal(given, given).value(), unsure(outside), unsure(isNaN));
                m_out.appendTo(isNaN);
                ValueFromBlock atStart = m_out.anchor(m_out.int64Zero);
                m_out.branch(isZero(characters.length).value(), unsure(outside), unsure(isInside));
                m_out.appendTo(isInside);
                index = Integer(m_out.phi(Int64, unchanged, atStart));
            } else
                orElse(below(index, characters.length), outside);
            if (part) {
                index = add(index, part->from);
                characters.length = add(characters.length, part->from);
            }
        }
        bool tracksCodeUnit = ((which == InlineBuiltin::StringCharCodeAt && rep == Rep::Double) || (which == InlineBuiltin::StringCodePointAt && rep == Rep::OptionalInt64)) && isOnMainPath();
        bool tracksStringCodeUnit = (which == InlineBuiltin::StringCharAt || which == InlineBuiltin::StringAt) && isOnMainPath();
        Vector<ValueFromBlock, 2> codeUnits;
        if (which == InlineBuiltin::StringCharCodeAt) {
            Integer codeUnit = zeroExtended(codeUnitFromFlatString ? *codeUnitFromFlatString : codeUnitAt(characters, index));
            if (tracksCodeUnit)
                codeUnits.append(m_out.anchor(codeUnit.value()));
            join.add(ensure(codeUnit, Type::integer({ 0, UINT16_MAX }), type));
        } else if (which == InlineBuiltin::StringCodePointAt) {
            Integer codePoint = zeroExtended(codePointAt(characters, index));
            if (tracksCodeUnit)
                codeUnits.append(m_out.anchor(codePoint.value()));
            join.add(ensure(codePoint, Type::integer({ 0, 0x10ffff }), type));
        } else {
            Word32 codeUnit;
            CellPointer string = singleCharacterStringAt(characters, index, tracksStringCodeUnit ? &codeUnit : nullptr);
            if (tracksStringCodeUnit)
                codeUnits.append(m_out.anchor(zeroExtended(codeUnit).value()));
            join.add(ensure(string, stringType, type));
        }

        m_out.appendTo(outside);
        if (openedCapturedVariable) {
            LBasicBlock isNil = newColdBlock();
            LBasicBlock isString = newColdBlock();
            Boxed held = storedInCapturedVariable();
            m_out.branch(isCell(held, typeOf(value.child(0))).value(), unsure(isString), unsure(isNil));
            m_out.appendTo(isNil);
            throwNullish(held, m_program.stringOf(m_builtins.at(value.builtin()).nameName), asCell(held));
            m_out.appendTo(isString);
        }
        if (which == InlineBuiltin::StringCharCodeAt) {
            if (tracksCodeUnit)
                codeUnits.append(m_out.anchor(m_out.constInt64(INT64_MIN)));
            join.add(ensure(number(PNaN), m_program.at(m_program.types.number), type));
        } else if (which == InlineBuiltin::StringCharAt) {
            if (tracksStringCodeUnit)
                codeUnits.append(m_out.anchor(m_out.constInt64(emptyStringCodeUnit)));
            join.add(ensure(CellPointer(emptyString()), stringType, type));
        } else {
            static_assert(undefinedCodeUnit == INT64_MIN);
            if (tracksStringCodeUnit || tracksCodeUnit)
                codeUnits.append(m_out.anchor(m_out.constInt64(undefinedCodeUnit)));
            join.add(ensure(undefined(), m_program.at(m_program.types.undefined), type));
        }
        Operand joined = join.finish();
        if (tracksStringCodeUnit)
            didCreateStringFromCodeUnit(result, joined, codeUnits);
        if (tracksCodeUnit) {
            m_numbersAsIntegers.add(result.index(), NumberAsInteger { joined.value, m_out.phi(Int64, codeUnits), 0 });
        }
        return joined;
    }

    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr: {
        std::optional<Substring> whole;
        if (part)
            whole = *part;
        Integer length;
        if (whole)
            length = lengthOf(*whole);
        else if (auto known = lengthReadBefore(value.child(0)))
            length = *known;
        else {
            length = stringLength(receiver);
            if (isOnMainPath())
                didReadLength(value.child(0), length);
        }
        auto isNeverValidPosition = [&](ValueID id) {
            return typeOf(id).isInteger() && typeOf(id).integers.maximum < 0;
        };
        std::optional<Operand> givenEnd;
        if (arguments.size() > 1)
            givenEnd = numberOrIfUndefined(arguments[1], toNumber(length));
        LBasicBlock arePositions = nullptr;
        ValueFromBlock originalStart;
        ValueFromBlock originalEnd;
        if (which != InlineBuiltin::StringSubstr && !isNeverValidPosition(arguments[0]) && (arguments.size() == 1 || !isNeverValidPosition(arguments[1]))) {
            auto truncated = [&](Operand operand) {
                return operand.rep == Rep::Int64 ? operand.as<Integer>() : truncateToInteger(operand.as<Number>());
            };
            Integer start = truncated(get(arguments[0]));
            Integer end = givenEnd ? truncated(*givenEnd) : length;
            arePositions = m_out.newBlock();
            LBasicBlock areNotPositions = newColdBlock();
            originalStart = m_out.anchor(start.value());
            originalEnd = m_out.anchor(end.value());
            m_out.branch(logicalAnd(belowOrEqual(start, end), belowOrEqual(end, length)).value(), usually(arePositions), rarely(areNotPositions));
            m_out.appendTo(areNotPositions);
        }
        Integer from = relativeStringIndex(arguments[0], length, which != InlineBuiltin::StringSubstring);
        Integer to = length;
        if (arguments.size() > 1) {
            to = relativeStringIndex(*givenEnd, typeOf(arguments[1]), length, which == InlineBuiltin::StringSlice);
            if (which == InlineBuiltin::StringSubstr) {
                Integer end = add(from, to);
                to = select(greaterThan(end, length), length, end);
            } else if (which == InlineBuiltin::StringSubstring) {
                Word32 isReversed = greaterThan(from, to);
                Integer least = select(isReversed, to, from);
                to = select(isReversed, from, to);
                from = least;
            }
        }
        if (arePositions) {
            ValueFromBlock clampedStart = m_out.anchor(from.value());
            ValueFromBlock clampedEnd = m_out.anchor(to.value());
            m_out.jump(arePositions);
            m_out.appendTo(arePositions);
            from = Integer(m_out.phi(Int64, originalStart, clampedStart));
            to = Integer(m_out.phi(Int64, originalEnd, clampedEnd));
        }
        if (whole) {
            from = add(whole->from, from);
            to = add(whole->from, to);
            length = whole->lengthOfBase;
        }
        if (isOnMainPath()) {
            m_substrings.set(result.index(), Substring { receiver, length, from, to });
            if (m_substringsOnlyInspected.get(result.index()))
                return Operand(value.rep, nullptr);
        }
        return ensure(makeSubstring(receiver, length, from, to), stringType, type);
    }

    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
        return booleanResult(lowerStartsOrEndsWith(which == InlineBuiltin::StringEndsWith, receiver, arguments[0], arguments.size() > 1 ? std::optional { arguments[1] } : std::nullopt), type);

    case InlineBuiltin::StringIncludes:
        return booleanResult(notEqual(lowerStringSearch(which, receiver, arguments), word32(-1)), type);
    case InlineBuiltin::StringIndexOf:
    case InlineBuiltin::StringLastIndexOf:
        return ensure(signExtended(lowerStringSearch(which, receiver, arguments)), Type::integer({ -1, INT32_MAX }), type);

    case InlineBuiltin::StringRepeat: {
        Integer count = positionOf(arguments[0]);
        Integer limit = integer(maxFlatStringLength);
        return ensure(either<CellPointer>(bitAnd(belowOrEqual(count, limit), belowOrEqual(mul(count, stringLength(receiver)), limit)), [&] {
            return AOT_TYPED_OPERATION(StringRepeat, receiver.string, low32(count));
        }, genericPath), stringType, type);
    }
    case InlineBuiltin::StringPadStart:
    case InlineBuiltin::StringPadEnd: {
        Integer length = positionOf(arguments[0]);
        CellPointer filler = arguments.size() > 1 ? in<CellPointer>(arguments[1]) : CellPointer(singleCharacterString(m_out.constInt32(' ')));
        return ensure(either<CellPointer>(belowOrEqual(length, integer(maxFlatStringLength)), [&] {
            return either<CellPointer>(lessThanOrEqual(length, stringLength(receiver)), [&] {
                return receiver.string;
            }, [&] {
                return AOT_TYPED_OPERATION(StringPad, receiver.string, low32(length), filler, word32(which == InlineBuiltin::StringPadStart));
            });
        }, genericPath), stringType, type);
    }

    case InlineBuiltin::StringLocaleCompare:
        return ensure(signExtended(AOT_TYPED_OPERATION(StringLocaleCompare, receiver.string, in<CellPointer>(arguments[0]))), Type::integer({ -1, 1 }), type);

    case InlineBuiltin::StringToLowerCase:
        return ensure(AOT_TYPED_OPERATION(StringToLowerCase, receiver.string), stringType, type);
    case InlineBuiltin::StringToUpperCase:
        return ensure(AOT_TYPED_OPERATION(StringToUpperCase, receiver.string), stringType, type);

    case InlineBuiltin::StringTrim:
    case InlineBuiltin::StringTrimStart:
    case InlineBuiltin::StringTrimEnd: {
        bool trimsStart = which != InlineBuiltin::StringTrimEnd;
        bool trimsEnd = which != InlineBuiltin::StringTrimStart;
        LBasicBlock isFlat = m_out.newBlock();
        LBasicBlock byStub = m_out.newBlock();
        LBasicBlock byStubForFlat8 = m_out.newBlock();
        Stub generalStub = trimsStart && trimsEnd ? Stub::StringTrim : trimsStart ? Stub::StringTrimStart : Stub::StringTrimEnd;
        Stub stubForFlat8 = trimsStart && trimsEnd ? Stub::StringTrimFlat8 : trimsStart ? Stub::StringTrimStartFlat8 : Stub::StringTrimEndFlat8;
        Join join(m_out, Rep::Cell, pointerType());
        Pointer fiberAtFirst = fiberOf(receiver);
        LBasicBlock isRope = newColdBlock();
        ValueFromBlock wasFlat = m_out.anchor(fiberAtFirst.value());
        m_out.branch(hasNoBits(fiberAtFirst, JSString::isRopeInPointer).value(), usually(isFlat), rarely(isRope));
        m_out.appendTo(isFlat);
        Pointer fiber(m_out.phi(pointerType(), wasFlat));
        orElse(hasBits(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed), StringImpl::flagIs8Bit()), byStub);
        Word32 length = loadField<Word32>(fiber, m_heaps.StringImpl_length);
        orElse(Word32(m_out.notZero32(length.value())), byStub);
        LValue data = loadField<Pointer>(fiber, m_heaps.StringImpl_data).value();
        auto isPrintableASCII = [&](LValue address) {
            Word32 character(marked(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), address)), IsFixed));
            return belowOrEqual(add(character, word32(-0x21)), word32(0x7e - 0x21));
        };
        LValue wideLength = m_out.zeroExtPtr(length.value());
        if (trimsStart)
            orElse(isPrintableASCII(data), byStubForFlat8);
        if (trimsEnd)
            orElse(isPrintableASCII(m_out.add(data, m_out.sub(wideLength, m_out.constIntPtr(1)))), byStubForFlat8);
        ensureStillAliveHere(receiver.string);
        join.add(receiver.string);
        m_out.appendTo(byStubForFlat8);
        join.add(CellPointer(callAllocationStub(stubForFlat8, { { receiver.string.value(), stubTemporaryGPRs[2] }, { data, stubTemporaryGPRs[3] }, { wideLength, stubTemporaryGPRs[4] } }, effectsForOperation<operationAOTTypedStringPart>(), true)));
        m_out.appendTo(isRope);
        LBasicBlock notSubstring = newColdBlock();
        LBasicBlock isStillRope = newColdBlock();
        Pointer current = loadField<Pointer>(CellPointer(m_out.opaque(receiver.string.value())), m_heaps.JSRopeString_fiber0);
        m_out.branch(hasNoBits(current, JSRopeString::isSubstringInPointer).value(), unsure(notSubstring), unsure(byStub));
        m_out.appendTo(notSubstring);
        m_out.branch(hasNoBits(current, JSString::isRopeInPointer).value(), unsure(byStub), unsure(isStillRope));
        m_out.appendTo(isStillRope);
        m_out.addIncomingToPhi(fiber.value(), m_out.anchor(AOT_TYPED_OPERATION(Flatten, receiver.string).value()));
        m_out.jump(isFlat);
        m_out.appendTo(byStub);
        join.add(CellPointer(callAllocationStub(generalStub, { { receiver.string.value(), stubTemporaryGPRs[2] } }, effectsForOperation<operationAOTTypedStringTrim>(), true)));
        return ensure(join.finish().as<CellPointer>(), stringType, type);
    }

    case InlineBuiltin::StringIsWellFormed:
        return booleanResult(either<Word32>(hasBits(stringPartOf(receiver.string).flags, StringImpl::flagIs8Bit()), [&] {
            return boolean(true);
        }, [&] {
            return unboxedBoolean(callBuiltinFunction(value, builtinCallEffects(value)));
        }), type);
    case InlineBuiltin::StringToWellFormed:
    case InlineBuiltin::StringNormalize:
        return ensure(either<CellPointer>(hasBits(stringPartOf(receiver.string).flags, StringImpl::flagIs8Bit()), [&] {
            return receiver.string;
        }, genericPath), stringType, type);

    case InlineBuiltin::StringConcat: {
        StringPart first = stringPartOf(receiver.string);
        StringPart second = stringPartOf(arguments[0]);
        if (arguments.size() == 1)
            return ensure(concatenate(first, second).string, stringType, type);
        StringPart third = stringPartOf(arguments[1]);
        return ensure(concatenate(first, second, &third).string, stringType, type);
    }

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
