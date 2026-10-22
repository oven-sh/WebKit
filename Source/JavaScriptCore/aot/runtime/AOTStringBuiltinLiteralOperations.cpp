/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTImageText.h"
#include "AOTLimits.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTSlotRepresentation.h"
#include "AOTStringEmitter.h"
#include "JSCInlines.h"
#include <algorithm>
#include <array>
#include <span>
#include <wtf/text/AdaptiveStringSearcher.h>
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr size_t maxRegisterStringLength = defaultLimitValue(LimitID::MaxRegisterStringLength, hostArchitecture());
static_assert(maxRegisterStringLength == sizeof(uint64_t));

static StringView textOf(uint32_t text)
{
    return Image::current()->text().view(StringID(text));
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
    std::array<char16_t, maxRegisterStringLength> units;
    string->resolveToBuffer(std::span { units }.first(length));
    for (unsigned i = 0; i < length; ++i) {
        if (!WTF::isLatin1(units[i]))
            return false;
    }
    return Typed::packStringIntoRegister(std::span<const char16_t> { units }.first(length)) == text;
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
