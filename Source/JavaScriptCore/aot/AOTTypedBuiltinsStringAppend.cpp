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
#include "AOTTypedStringAppend.h"
#include "JSCInlines.h"
#include "NumberPrototype.h"
#include "StrongInlines.h"
#include <charconv>
#include <wtf/SIMDHelpers.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/dtoa.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr size_t minStringAppenderCapacity = 4 * KB;
static constexpr size_t maxRetainedStringAppenderCapacity = 256 * KB;
static constexpr size_t minLengthAppendedByReference = 64 * KB;
static constexpr uint64_t maxStringLength = JSString::MaxLength;
static constexpr size_t maxLengthCopiedInline = 16;

struct StringAppenderFrame {
    uint64_t owner { 0 };
    uint64_t base { 0 };
    size_t offset { 0 };
    uint32_t firstSegment { 0 };
    uint32_t firstStart { 0 };
    uint32_t region { 0 };
    bool isTooLong { false };
};

struct StringAppenderSegment {
    size_t offset { 0 };
    String text;
    Strong<JSString> rope;

    unsigned length() const { return rope ? rope->length() : text.length(); }
};

struct StringAppender {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StringAppender);

    Latin1Character* start { nullptr };
    size_t capacity { 0 };
    Vector<StringAppenderFrame, 8> frames;
    Vector<StringAppenderSegment> segments;
    uint64_t* starts { nullptr };
};

static ALWAYS_INLINE size_t stringAppenderOffset(Instance* instance)
{
    return instance->stringAppenderOrigin + instance->stringAppenderLength - instance->stringAppender->start;
}

static void updateStringAppenderLimit(Instance* instance)
{
    auto& appender = *instance->stringAppender;
    if (appender.frames.isEmpty() || appender.frames.last().isTooLong) {
        instance->stringAppenderLimit = 0;
        return;
    }
    uint64_t room = appender.capacity - stringAppenderOffset(instance);
    instance->stringAppenderLimit = std::min(instance->stringAppenderLength + room, appender.frames.last().base + maxStringLength);
}

static void popStringAppenderFrame(Instance* instance)
{
    auto& appender = *instance->stringAppender;
    auto frame = appender.frames.takeLast();
    appender.segments.shrink(frame.firstSegment);
    instance->stringAppenderStartsTop = appender.starts + frame.firstStart;
    instance->stringAppenderLength = frame.base;
    instance->stringAppenderOrigin = appender.start + frame.offset - frame.base;
    instance->stringAppenderOwner = appender.frames.isEmpty() ? std::numeric_limits<uint64_t>::max() : appender.frames.last().owner;
    if (appender.frames.isEmpty() && appender.capacity > maxRetainedStringAppenderCapacity) {
        fastFree(appender.start);
        appender.start = nullptr;
        appender.capacity = 0;
        instance->stringAppenderOrigin = nullptr;
        instance->stringAppenderLength = 0;
    }
    updateStringAppenderLimit(instance);
}

static NEVER_INLINE void popAbandonedStringAppenderFrames(Instance* instance, uint64_t frame)
{
    while (instance->stringAppenderOwner < frame)
        popStringAppenderFrame(instance);
}

static ALWAYS_INLINE void syncStringAppender(Instance* instance, uint64_t frame)
{
    if (instance->stringAppenderOwner < frame) [[unlikely]]
        popAbandonedStringAppenderFrames(instance, frame);
}

static NEVER_INLINE bool growStringAppender(Instance* instance, size_t size)
{
    auto& appender = *instance->stringAppender;
    size_t offset = stringAppenderOffset(instance);
    size_t capacity = std::max({ appender.capacity * 2, offset + size, minStringAppenderCapacity });
    void* storage = nullptr;
    if (!WTF::tryFastRealloc(appender.start, capacity).getValue(storage))
        return false;
    appender.start = static_cast<Latin1Character*>(storage);
    appender.capacity = capacity;
    instance->stringAppenderOrigin = appender.start + offset - instance->stringAppenderLength;
    updateStringAppenderLimit(instance);
    return true;
}

static NEVER_INLINE void growStringAppenderStarts(Instance* instance)
{
    auto& appender = *instance->stringAppender;
    size_t size = instance->stringAppenderStartsTop - appender.starts;
    size_t capacity = std::max<size_t>(size * 2, 64);
    appender.starts = static_cast<uint64_t*>(fastRealloc(appender.starts, capacity * sizeof(uint64_t)));
    instance->stringAppenderStartsTop = appender.starts + size;
    instance->stringAppenderStartsEnd = appender.starts + capacity;
}

static ALWAYS_INLINE void copyCharactersInline(Latin1Character* to, const Latin1Character* from, size_t size)
{
    if (size >= 16) {
        if (size > 32) {
            memcpy(to, from, size);
            return;
        }
        memcpy(to, from, 16);
        memcpy(to + size - 16, from + size - 16, 16);
        return;
    }
    if (size >= 8) {
        memcpy(to, from, 8);
        memcpy(to + size - 8, from + size - 8, 8);
        return;
    }
    if (size >= 4) {
        memcpy(to, from, 4);
        memcpy(to + size - 4, from + size - 4, 4);
        return;
    }
    if (size & 2)
        memcpy(to, from, 2);
    if (size & 1)
        to[size - 1] = from[size - 1];
}

enum class Appended : uint8_t { Yes, OutOfMemory };

static NEVER_INLINE Appended appendSlow(Instance* instance, uint64_t size, const ScopedLambda<void(Latin1Character*)>& write)
{
    auto& appender = *instance->stringAppender;
    RELEASE_ASSERT(!appender.frames.isEmpty());
    auto& frame = appender.frames.last();
    if (frame.isTooLong || instance->stringAppenderLength + size - frame.base > maxStringLength) {
        frame.isTooLong = true;
        instance->stringAppenderLength += size;
        instance->stringAppenderOrigin -= size;
        instance->stringAppenderLimit = 0;
        return Appended::Yes;
    }
    if (!growStringAppender(instance, size))
        return Appended::OutOfMemory;
    write(instance->stringAppenderOrigin + instance->stringAppenderLength);
    instance->stringAppenderLength += size;
    return Appended::Yes;
}

static ALWAYS_INLINE Appended appendCharacters(Instance* instance, std::span<const Latin1Character> characters)
{
    uint64_t length = instance->stringAppenderLength;
    if (length + characters.size() > instance->stringAppenderLimit) [[unlikely]] {
        return appendSlow(instance, characters.size(), [&](Latin1Character* to) {
            memcpy(to, characters.data(), characters.size());
        });
    }
    copyCharactersInline(instance->stringAppenderOrigin + length, characters.data(), characters.size());
    instance->stringAppenderLength = length + characters.size();
    return Appended::Yes;
}

static ALWAYS_INLINE bool areAllLatin1(std::span<const char16_t> characters)
{
    unsigned bits = 0;
    for (char16_t character : characters)
        bits |= character;
    return !(bits & 0xff00);
}

static ALWAYS_INLINE bool canBeViewedWithoutFlattening(JSString* string)
{
    return string->isSubstring() || !string->isRope();
}

static Appended appendNarrowed(Instance* instance, std::span<const char16_t> characters)
{
    auto write = [&](Latin1Character* to) ALWAYS_INLINE_LAMBDA {
        for (char16_t character : characters)
            *to++ = static_cast<Latin1Character>(character);
    };
    countOperationNamed(instance, "StringAppend: Latin-1 in 16 bits");
    if (instance->stringAppenderLength + characters.size() > instance->stringAppenderLimit) [[unlikely]]
        return appendSlow(instance, characters.size(), write);
    write(instance->stringAppenderOrigin + instance->stringAppenderLength);
    instance->stringAppenderLength += characters.size();
    return Appended::Yes;
}

static NEVER_INLINE Appended appendStringSlow(Instance* instance, uint64_t frame, JSString* string)
{
    auto& appender = *instance->stringAppender;
    auto& top = appender.frames.last();
    unsigned size = string->length();
    auto skip = [&] {
        instance->stringAppenderLength += size;
        instance->stringAppenderOrigin -= size;
        updateStringAppenderLimit(instance);
        return Appended::Yes;
    };
    if (top.isTooLong || instance->stringAppenderLength + size - top.base > maxStringLength) {
        top.isTooLong = true;
        return skip();
    }
    if (string->isSubstring() && string->is8Bit() && size < minLengthAppendedByReference) {
        auto view = string->view(instance->globalObject);
        return appendCharacters(instance, view->span8());
    }
    if (!string->is8Bit() && size < minLengthAppendedByReference && canBeViewedWithoutFlattening(string)) {
        auto view = string->view(instance->globalObject);
        if (areAllLatin1(view->span16()))
            return appendNarrowed(instance, view->span16());
    }
    if (string->is8Bit() && size < minLengthAppendedByReference) {
        if (instance->stringAppenderLength + size > instance->stringAppenderLimit) {
            return appendSlow(instance, size, [&](Latin1Character* to) {
                string->resolveToBuffer(std::span { to, size });
            });
        }
        string->resolveToBuffer(std::span { instance->stringAppenderOrigin + instance->stringAppenderLength, size });
        instance->stringAppenderLength += size;
        return Appended::Yes;
    }
    if (string->isRope() && size >= minLengthAppendedByReference) {
        appender.segments.append(StringAppenderSegment { stringAppenderOffset(instance), String(), Strong<JSString>(*instance->vm, string) });
        return skip();
    }
    AOTOperationPrologueCallFrameTracer tracer(*instance->vm, std::bit_cast<CallFrame*>(frame));
    auto text = string->tryGetValue();
    if (text.data.isNull())
        return Appended::OutOfMemory;
    appender.segments.append(StringAppenderSegment { stringAppenderOffset(instance), text.data, { } });
    return skip();
}

static ALWAYS_INLINE Appended appendString(Instance* instance, uint64_t frame, JSString* string)
{
    const StringImpl* impl = string->tryGetValueImpl();
    if (!impl || !impl->is8Bit() || impl->length() >= minLengthAppendedByReference) [[unlikely]]
        return appendStringSlow(instance, frame, string);
    return appendCharacters(instance, impl->span8());
}

static NEVER_INLINE Appended appendNumberInRadix(Instance* instance, double number, int32_t radix)
{
    if (int32_t integer = truncateDoubleToInt32(number); integer == number) {
        std::array<char, 33> digits;
        return appendCharacters(instance, byteCast<Latin1Character>(std::span<const char> { digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), integer, radix).ptr }));
    }
    if (!std::isfinite(number))
        return appendCharacters(instance, String::number(number).span8());
    RadixBuffer buffer;
    return appendCharacters(instance, byteCast<Latin1Character>(unsafeSpan(toStringWithRadixInternal(buffer, number, radix))));
}

static NEVER_INLINE Appended appendPrimitiveSlow(Instance* instance, uint64_t frame, JSValue value)
{
    if (value.isDouble()) {
        NumberToStringBuffer buffer;
        return appendCharacters(instance, byteCast<Latin1Character>(numberToStringAndSize(value.asDouble(), buffer)));
    }
    if (value.isUndefined())
        return appendCharacters(instance, "undefined"_s.span8());
    if (value.isNull())
        return appendCharacters(instance, "null"_s.span8());
    if (value.isBoolean())
        return appendCharacters(instance, value.isTrue() ? "true"_s.span8() : "false"_s.span8());
    RELEASE_ASSERT(!value.isObject() && !value.isSymbol());
    AOT_OPERATION_BEGIN_IN_FRAME(instance, std::bit_cast<CallFrame*>(frame), "operationAOTTypedStringAppend");
    JSString* string = value.toString(globalObject);
    RETURN_IF_EXCEPTION(scope, Appended::OutOfMemory);
    RELEASE_AND_RETURN(scope, appendString(instance, frame, string));
}

static NEVER_INLINE OperationReturnType<void> throwStringAppendError(Instance* instance, uint64_t frame)
{
    AOT_OPERATION_BEGIN_IN_FRAME(instance, std::bit_cast<CallFrame*>(frame), "operationAOTTypedStringAppend");
    if (!scope.exception())
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope);
}

static ALWAYS_INLINE void ensureStringAppender(Instance* instance)
{
    if (instance->stringAppender) [[likely]]
        return;
    instance->stringAppender = new StringAppender;
    instance->stringAppenderOwner = std::numeric_limits<uint64_t>::max();
}

static ALWAYS_INLINE bool hasStringAppenderFrame(Instance* instance, uint64_t frame, uint32_t region)
{
    return instance->stringAppenderOwner == frame && instance->stringAppender->frames.last().region == region;
}

static NEVER_INLINE Appended pushStringAppenderFrame(Instance* instance, uint64_t frame, uint32_t region, EncodedJSValue* firstPiece)
{
    auto& appender = *instance->stringAppender;
    size_t offset = stringAppenderOffset(instance);
    appender.frames.constructAndAppend(frame, instance->stringAppenderLength, offset, static_cast<uint32_t>(appender.segments.size()), static_cast<uint32_t>(instance->stringAppenderStartsTop - appender.starts), region, false);
    instance->stringAppenderOwner = frame;
    instance->stringAppenderLimit = instance->stringAppenderLength + std::min<uint64_t>(appender.capacity - offset, maxStringLength);
    JSValue first = JSValue::decode(std::exchange(*firstPiece, 0));
    return first ? appendString(instance, frame, asString(first)) : Appended::Yes;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringAppendBegin, void, (Instance* instance, uint64_t frame, uint32_t region, EncodedJSValue* firstPiece))
{
    ensureStringAppender(instance);
    syncStringAppender(instance, frame);
    if (region >> 31) {
        while (instance->stringAppenderOwner == frame) [[unlikely]]
            popStringAppenderFrame(instance);
        region &= std::numeric_limits<int32_t>::max();
    }
    if (!hasStringAppenderFrame(instance, frame, region) && pushStringAppenderFrame(instance, frame, region, firstPiece) != Appended::Yes) [[unlikely]]
        return throwStringAppendError(instance, frame);
    return { nullptr };
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringAppendReset, void, (Instance* instance, uint64_t frame, uint32_t isNested))
{
    ensureStringAppender(instance);
    while (instance->stringAppenderOwner < frame || (!isNested && instance->stringAppenderOwner == frame))
        popStringAppenderFrame(instance);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedStringAppendSync, void, (Instance* instance, uint64_t frame))
{
    popAbandonedStringAppenderFrames(instance, frame);
}

static ALWAYS_INLINE ASCIILiteral htmlEntityFor(char16_t character)
{
    switch (character) {
    case '"':
        return "&quot;"_s;
    case '&':
        return "&amp;"_s;
    case '\'':
        return "&#x27;"_s;
    case '<':
        return "&lt;"_s;
    case '>':
        return "&gt;"_s;
    default:
        return { };
    }
}

template<typename CharacterType>
static size_t lengthWithHTMLEntities(std::span<const CharacterType> characters)
{
    size_t length = characters.size();
    for (CharacterType character : characters) {
        if (auto entity = htmlEntityFor(character); !entity.isNull())
            length += entity.length() - 1;
    }
    return length;
}

template<typename CharacterType>
static void writeWithHTMLEntities(std::span<const CharacterType> characters, CharacterType* to)
{
    for (CharacterType character : characters) {
        auto entity = htmlEntityFor(character);
        if (entity.isNull()) {
            *to++ = character;
            continue;
        }
        for (char letter : entity.span())
            *to++ = letter;
    }
}

static ALWAYS_INLINE bool isHTMLMetacharacter(Latin1Character character)
{
    return character <= '>' && (0x5000'00c4'0000'0000ull >> character & 1);
}

static NEVER_INLINE Appended appendWithHTMLEntitiesSlow(Instance* instance, std::span<const Latin1Character> characters)
{
    size_t size = lengthWithHTMLEntities(characters);
    return appendSlow(instance, size, [&](Latin1Character* to) {
        writeWithHTMLEntities(characters, to);
    });
}

static ALWAYS_INLINE Appended appendHTMLEscapedCharacters(Instance* instance, std::span<const Latin1Character> characters)
{
    constexpr size_t longestEntity = 6;
    constexpr size_t stride = SIMD::stride<uint8_t>;
    if (instance->stringAppenderLength + characters.size() * longestEntity + stride > instance->stringAppenderLimit) [[unlikely]]
        return appendWithHTMLEntitiesSlow(instance, characters);
    Latin1Character* start = instance->stringAppenderOrigin + instance->stringAppenderLength;
    Latin1Character* to = start;
    const Latin1Character* from = characters.data();
    size_t remaining = characters.size();
    constexpr auto quote = SIMD::splat<uint8_t>('"');
    constexpr auto ampersand = SIMD::splat<uint8_t>('&');
    constexpr auto apostrophe = SIMD::splat<uint8_t>('\'');
    constexpr auto less = SIMD::splat<uint8_t>('<');
    constexpr auto greater = SIMD::splat<uint8_t>('>');
    while (remaining) {
#if ASAN_ENABLED
        bool mayLoadWholeVector = remaining >= stride;
#else
        bool mayLoadWholeVector = remaining >= stride || (std::bit_cast<uintptr_t>(from) & (4 * KB - 1)) <= 4 * KB - stride;
#endif
        if (!mayLoadWholeVector) [[unlikely]] {
            for (; remaining; --remaining, ++from) {
                Latin1Character character = *from;
                if (!isHTMLMetacharacter(character)) {
                    *to++ = character;
                    continue;
                }
                auto entity = htmlEntityFor(character).span8();
                memcpy(to, entity.data(), entity.size());
                to += entity.size();
            }
            break;
        }
        auto input = SIMD::load(std::bit_cast<const uint8_t*>(from));
        SIMD::store(input, std::bit_cast<uint8_t*>(to));
        auto found = SIMD::findFirstNonZeroIndex(SIMD::bitOr(SIMD::equal(input, quote), SIMD::equal(input, ampersand), SIMD::equal(input, apostrophe), SIMD::equal(input, less), SIMD::equal(input, greater)));
        size_t run = std::min<size_t>(found.value_or(stride), remaining);
        from += run;
        to += run;
        remaining -= run;
        if (run == stride || !remaining)
            continue;
        auto entity = htmlEntityFor(*from).span8();
        memcpy(to, entity.data(), entity.size());
        to += entity.size();
        ++from;
        --remaining;
    }
    instance->stringAppenderLength += to - start;
    return Appended::Yes;
}

static Appended appendHTMLEscapedNarrowed(Instance* instance, std::span<const char16_t> characters)
{
    size_t size = lengthWithHTMLEntities(characters);
    if (size == characters.size())
        return appendNarrowed(instance, characters);
    auto write = [&](Latin1Character* to) ALWAYS_INLINE_LAMBDA {
        for (char16_t character : characters) {
            auto entity = htmlEntityFor(character);
            if (entity.isNull()) {
                *to++ = static_cast<Latin1Character>(character);
                continue;
            }
            for (char letter : entity.span())
                *to++ = letter;
        }
    };
    countOperationNamed(instance, "StringAppend: Latin-1 in 16 bits");
    if (instance->stringAppenderLength + size > instance->stringAppenderLimit) [[unlikely]]
        return appendSlow(instance, size, write);
    write(instance->stringAppenderOrigin + instance->stringAppenderLength);
    instance->stringAppenderLength += size;
    return Appended::Yes;
}

static NEVER_INLINE Appended appendHTMLEscapedStringSlow(Instance* instance, uint64_t frame, JSString* string)
{
    if (string->isSubstring() && string->is8Bit() && string->length() < minLengthAppendedByReference) {
        auto view = string->view(instance->globalObject);
        return appendHTMLEscapedCharacters(instance, view->span8());
    }
    if (!string->is8Bit() && string->length() < minLengthAppendedByReference && canBeViewedWithoutFlattening(string)) {
        auto view = string->view(instance->globalObject);
        if (areAllLatin1(view->span16()))
            return appendHTMLEscapedNarrowed(instance, view->span16());
    }
    AOTOperationPrologueCallFrameTracer tracer(*instance->vm, std::bit_cast<CallFrame*>(frame));
    auto text = string->tryGetValue();
    if (text.data.isNull())
        return Appended::OutOfMemory;
    if (text.data.is8Bit())
        return appendHTMLEscapedCharacters(instance, text.data.span8());
    auto characters = text.data.span16();
    size_t size = lengthWithHTMLEntities(characters);
    if (size == characters.size())
        return appendStringSlow(instance, frame, string);
    if (size > maxStringLength)
        return Appended::OutOfMemory;
    std::span<char16_t> escaped;
    auto impl = StringImpl::tryCreateUninitialized(size, escaped);
    if (!impl)
        return Appended::OutOfMemory;
    writeWithHTMLEntities(characters, escaped.data());
    return appendStringSlow(instance, frame, jsString(*instance->vm, String { impl.releaseNonNull() }));
}

static ALWAYS_INLINE size_t relativeStringIndex(JSValue index, size_t length)
{
    if (index.isInt32()) [[likely]] {
        int32_t integer = index.asInt32();
        return integer < 0 ? static_cast<size_t>(std::max<int64_t>(static_cast<int64_t>(length) + integer, 0)) : std::min<size_t>(integer, length);
    }
    double number = index.asNumber();
    if (number != number)
        return 0;
    number = std::trunc(number);
    return number < 0 ? static_cast<size_t>(std::max<double>(length + number, 0)) : static_cast<size_t>(std::min<double>(number, length));
}

static NEVER_INLINE Appended appendHTMLEscapedSliceSlow(Instance* instance, uint64_t frame, JSString* string, JSValue from, JSValue to)
{
    AOT_OPERATION_BEGIN_IN_FRAME(instance, std::bit_cast<CallFrame*>(frame), "operationAOTTypedStringAppend");
    size_t start = relativeStringIndex(from, string->length());
    size_t end = std::max(relativeStringIndex(to, string->length()), start);
    if (string->is8Bit() && (string->isSubstring() || !string->isRope())) {
        auto view = string->view(globalObject);
        RELEASE_AND_RETURN(scope, appendHTMLEscapedCharacters(instance, view->span8().subspan(start, end - start)));
    }
    if (!string->is8Bit() && end - start < minLengthAppendedByReference && canBeViewedWithoutFlattening(string)) {
        auto view = string->view(globalObject);
        if (auto characters = view->span16().subspan(start, end - start); areAllLatin1(characters))
            RELEASE_AND_RETURN(scope, appendHTMLEscapedNarrowed(instance, characters));
    }
    JSString* slice = jsSubstring(globalObject, string, start, end - start);
    RETURN_IF_EXCEPTION(scope, Appended::OutOfMemory);
    RELEASE_AND_RETURN(scope, appendHTMLEscapedStringSlow(instance, frame, slice));
}

static ALWAYS_INLINE Appended appendHTMLEscapedSlice(Instance* instance, uint64_t frame, JSString* string, JSValue from, JSValue to)
{
    const StringImpl* impl = string->tryGetValueImpl();
    if (!impl || !impl->is8Bit()) [[unlikely]]
        return appendHTMLEscapedSliceSlow(instance, frame, string, from, to);
    size_t start = relativeStringIndex(from, impl->length());
    size_t end = std::max(relativeStringIndex(to, impl->length()), start);
    return appendHTMLEscapedCharacters(instance, impl->span8().subspan(start, end - start));
}

static ALWAYS_INLINE Appended appendHTMLEscapedString(Instance* instance, uint64_t frame, JSString* string)
{
    const StringImpl* impl = string->tryGetValueImpl();
    if (!impl || !impl->is8Bit() || impl->length() >= minLengthAppendedByReference) [[unlikely]]
        return appendHTMLEscapedStringSlow(instance, frame, string);
    return appendHTMLEscapedCharacters(instance, impl->span8());
}

static ALWAYS_INLINE size_t lengthAheadOfHTMLMetacharacters(std::span<const Latin1Character> characters)
{
    constexpr size_t stride = SIMD::stride<uint8_t>;
    constexpr auto quote = SIMD::splat<uint8_t>('"');
    constexpr auto ampersand = SIMD::splat<uint8_t>('&');
    constexpr auto apostrophe = SIMD::splat<uint8_t>('\'');
    constexpr auto less = SIMD::splat<uint8_t>('<');
    constexpr auto greater = SIMD::splat<uint8_t>('>');
    size_t position = 0;
    for (; position + stride <= characters.size(); position += stride) {
        auto input = SIMD::load(std::bit_cast<const uint8_t*>(characters.data() + position));
        if (auto found = SIMD::findFirstNonZeroIndex(SIMD::bitOr(SIMD::equal(input, quote), SIMD::equal(input, ampersand), SIMD::equal(input, apostrophe), SIMD::equal(input, less), SIMD::equal(input, greater))))
            return position + *found;
    }
    for (; position < characters.size(); ++position) {
        if (isHTMLMetacharacter(characters[position]))
            return position;
    }
    return position;
}

static ALWAYS_INLINE size_t lengthAheadOfHTMLMetacharacters(std::span<const char16_t> characters)
{
    size_t position = 0;
    while (position < characters.size() && htmlEntityFor(characters[position]).isNull())
        ++position;
    return position;
}

template<typename CharacterType>
static NEVER_INLINE JSString* createStringWithHTMLEntities(VM& vm, std::span<const CharacterType> characters, size_t lengthAhead)
{
    size_t size = lengthAhead + lengthWithHTMLEntities(characters.subspan(lengthAhead));
    if (size > maxStringLength)
        return nullptr;
    std::span<CharacterType> escaped;
    auto impl = StringImpl::tryCreateUninitialized(size, escaped);
    if (!impl)
        return nullptr;
    memcpy(escaped.data(), characters.data(), lengthAhead * sizeof(CharacterType));
    writeWithHTMLEntities(characters.subspan(lengthAhead), escaped.data() + lengthAhead);
    return jsString(vm, String { impl.releaseNonNull() });
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedEscapeHTML, JSCell*, (Instance* instance, JSCell* cell))
{
    auto* string = uncheckedDowncast<JSString>(cell);
    if (const StringImpl* impl = string->tryGetValueImpl(); impl && impl->is8Bit() && lengthAheadOfHTMLMetacharacters(impl->span8()) == impl->length()) [[likely]]
        return { string, nullptr };
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    JSString* escaped = string;
    if (view->is8Bit()) {
        if (size_t lengthAhead = lengthAheadOfHTMLMetacharacters(view->span8()); lengthAhead != view->length())
            escaped = createStringWithHTMLEntities(vm, view->span8(), lengthAhead);
    } else if (size_t lengthAhead = lengthAheadOfHTMLMetacharacters(view->span16()); lengthAhead != view->length())
        escaped = createStringWithHTMLEntities(vm, view->span16(), lengthAhead);
    if (!escaped) [[unlikely]]
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope, escaped);
}

static constexpr uint64_t controlWord(Typed::StringAppendPart part)
{
    return static_cast<uint64_t>(part) << Typed::stringAppendPayloadShift | Typed::stringAppendControlTag;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringAppend, void, (Instance* instance, uint64_t frame, uint64_t count, const EncodedJSValue* parts, EncodedJSValue* firstPiece))
{
    syncStringAppender(instance, frame);
    if (uint32_t region = count >> 32) {
        count = static_cast<uint32_t>(count);
        if (!hasStringAppenderFrame(instance, frame, region) && pushStringAppenderFrame(instance, frame, region, firstPiece) != Appended::Yes) [[unlikely]]
            return throwStringAppendError(instance, frame);
    }
    const Typed::Data& data = instance->typed->data;
    Latin1Character* origin = instance->stringAppenderOrigin;
    uint64_t length = instance->stringAppenderLength;
    uint64_t limit = instance->stringAppenderLimit;
    auto reload = [&] ALWAYS_INLINE_LAMBDA {
        origin = instance->stringAppenderOrigin;
        length = instance->stringAppenderLength;
        limit = instance->stringAppenderLimit;
    };
    std::array<char, 12> digits;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t word = static_cast<uint64_t>(parts[i]);
        JSValue value = JSValue::decode(parts[i]);
        std::span<const Latin1Character> characters;
        if ((word & Typed::stringAppendTagMask) == Typed::stringAppendConstantTag && !(word >> 48))
            characters = byteCast<Latin1Character>(data.stringInImage(static_cast<uint32_t>(word >> Typed::stringAppendPayloadShift)));
        else if (limit && (word == controlWord(Typed::StringAppendPart::CheckLength) || word == controlWord(Typed::StringAppendPart::CheckRegionLength)))
            continue;
        else if (limit && word == controlWord(Typed::StringAppendPart::CheckLengthAndPop)) {
            --instance->stringAppenderStartsTop;
            continue;
        } else if (const StringImpl* impl = value.isCell() && value.asCell()->type() == StringType ? uncheckedDowncast<JSString>(value.asCell())->tryGetValueImpl() : nullptr; impl && impl->is8Bit() && impl->length() < minLengthAppendedByReference) [[likely]]
            characters = impl->span8();
        else if (value.isInt32())
            characters = byteCast<Latin1Character>(std::span<const char> { digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), value.asInt32()).ptr });
        else {
            instance->stringAppenderLength = length;
            Appended appended = Appended::Yes;
            if (value.isCell() && !((word & Typed::stringAppendTagMask) == Typed::stringAppendControlTag))
                appended = value.asCell()->type() == StringType ? appendStringSlow(instance, frame, uncheckedDowncast<JSString>(value.asCell())) : appendPrimitiveSlow(instance, frame, value);
            else if ((word & Typed::stringAppendTagMask) == Typed::stringAppendControlTag && !(word >> 48)) {
                auto& appender = *instance->stringAppender;
                uint64_t start = length;
                switch (static_cast<Typed::StringAppendPart>(word >> Typed::stringAppendPayloadShift)) {
                case Typed::StringAppendPart::PushStart:
                    if (instance->stringAppenderStartsTop == instance->stringAppenderStartsEnd) [[unlikely]]
                        growStringAppenderStarts(instance);
                    *instance->stringAppenderStartsTop++ = length;
                    break;
                case Typed::StringAppendPart::PopStart:
                    --instance->stringAppenderStartsTop;
                    break;
                case Typed::StringAppendPart::CheckLength:
                    start = instance->stringAppenderStartsTop[-1];
                    break;
                case Typed::StringAppendPart::CheckLengthAndPop:
                    start = *--instance->stringAppenderStartsTop;
                    break;
                case Typed::StringAppendPart::CheckRegionLength:
                    start = appender.frames.last().base;
                    break;
                case Typed::StringAppendPart::HTMLEscapedValue:
                    ++i;
                    if (JSValue escaped = JSValue::decode(parts[i + 1]); JSValue::decode(parts[i]).isTrue() && escaped.isCell() && escaped.asCell()->type() == StringType) [[likely]] {
                        ++i;
                        appended = appendHTMLEscapedString(instance, frame, uncheckedDowncast<JSString>(escaped.asCell()));
                    }
                    break;
                case Typed::StringAppendPart::HTMLEscapedSlice:
                    if (JSValue::decode(parts[i + 1]).isTrue()) [[likely]]
                        appended = appendHTMLEscapedSlice(instance, frame, asString(JSValue::decode(parts[i + 2])), JSValue::decode(parts[i + 3]), JSValue::decode(parts[i + 4]));
                    else
                        appended = appendStringSlow(instance, frame, asString(JSValue::decode(parts[i + 2])));
                    i += 4;
                    break;
                case Typed::StringAppendPart::NumberInRadix:
                    appended = appendNumberInRadix(instance, JSValue::decode(parts[i + 2]).asNumber(), JSValue::decode(parts[i + 1]).asInt32());
                    i += 2;
                    break;
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                }
                if (length - start > maxStringLength) [[unlikely]]
                    appended = Appended::OutOfMemory;
            } else
                appended = appendPrimitiveSlow(instance, frame, value);
            if (appended != Appended::Yes) [[unlikely]]
                return throwStringAppendError(instance, frame);
            reload();
            continue;
        }
        if (length + characters.size() > limit) [[unlikely]] {
            instance->stringAppenderLength = length;
            if (appendCharacters(instance, characters) != Appended::Yes)
                return throwStringAppendError(instance, frame);
            reload();
            continue;
        }
        copyCharactersInline(origin + length, characters.data(), characters.size());
        length += characters.size();
    }
    instance->stringAppenderLength = length;
    return { nullptr };
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringAppendMeasure, void, (Instance* instance, uint64_t frame, uint64_t count, const EncodedJSValue* parts))
{
    AOT_OPERATION_BEGIN_IN_FRAME(instance, std::bit_cast<CallFrame*>(frame), "operationAOTTypedStringAppendMeasure");
    uint64_t length = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t word = static_cast<uint64_t>(parts[i]);
        JSValue value = JSValue::decode(parts[i]);
        if ((word & Typed::stringAppendTagMask) == Typed::stringAppendConstantTag && !(word >> 48))
            length += instance->typed->data.stringInImage(static_cast<uint32_t>(word >> Typed::stringAppendPayloadShift)).size();
        else if (word == controlWord(Typed::StringAppendPart::HTMLEscapedValue)) {
            ++i;
            JSValue escaped = JSValue::decode(parts[i + 1]);
            if (!JSValue::decode(parts[i]).isTrue() || !escaped.isString())
                continue;
            ++i;
            auto view = asString(escaped)->view(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            length += view->is8Bit() ? lengthWithHTMLEntities(view->span8()) : lengthWithHTMLEntities(view->span16());
        } else if (word == controlWord(Typed::StringAppendPart::HTMLEscapedSlice)) {
            JSString* string = asString(JSValue::decode(parts[i + 2]));
            if (JSValue::decode(parts[i + 1]).isTrue()) {
                size_t start = relativeStringIndex(JSValue::decode(parts[i + 3]), string->length());
                size_t end = std::max(relativeStringIndex(JSValue::decode(parts[i + 4]), string->length()), start);
                auto view = string->view(globalObject);
                OPERATION_RETURN_IF_EXCEPTION(scope);
                length += view->is8Bit() ? lengthWithHTMLEntities(view->span8().subspan(start, end - start)) : lengthWithHTMLEntities(view->span16().subspan(start, end - start));
            } else
                length += string->length();
            i += 4;
        } else if (word == controlWord(Typed::StringAppendPart::NumberInRadix)) {
            length += toStringWithRadix(JSValue::decode(parts[i + 2]).asNumber(), JSValue::decode(parts[i + 1]).asInt32()).length();
            i += 2;
        } else if ((word & Typed::stringAppendTagMask) == Typed::stringAppendControlTag && !(word >> 48))
            continue;
        else {
            JSString* string = value.toString(globalObject);
            OPERATION_RETURN_IF_EXCEPTION(scope);
            length += string->length();
        }
    }
    if (length > maxStringLength)
        throwOutOfMemoryError(globalObject, scope);
    OPERATION_RETURN(scope);
}

static RefPtr<StringImpl> tryCreateStringWithSegments(std::span<const Latin1Character> characters, size_t charactersOffset, std::span<const StringAppenderSegment> segments, size_t length)
{
    bool is8Bit = std::ranges::all_of(segments, [](auto& segment) {
        return segment.text.is8Bit();
    });
    auto fill = [&](auto output) {
        size_t taken = 0;
        auto copyUpTo = [&](size_t end) {
            StringImpl::copyCharacters(output, characters.subspan(taken, end - taken));
            skip(output, end - taken);
            taken = end;
        };
        for (auto& segment : segments) {
            copyUpTo(segment.offset - charactersOffset);
            if (segment.text.is8Bit())
                StringImpl::copyCharacters(output, segment.text.span8());
            else if constexpr (std::is_same_v<decltype(output), std::span<char16_t>>)
                StringImpl::copyCharacters(output, segment.text.span16());
            skip(output, segment.text.length());
        }
        copyUpTo(characters.size());
    };
    if (is8Bit) {
        std::span<Latin1Character> output;
        auto impl = StringImpl::tryCreateUninitialized(length, output);
        if (impl)
            fill(output);
        return impl;
    }
    std::span<char16_t> output;
    auto impl = StringImpl::tryCreateUninitialized(length, output);
    if (impl)
        fill(output);
    return impl;
}

static JSString* tryCreateRopeWithSegments(JSGlobalObject* globalObject, std::span<const Latin1Character> characters, size_t charactersOffset, std::span<const StringAppenderSegment> segments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer pieces;
    size_t taken = 0;
    size_t firstSmall = 0;
    for (size_t k = 0; k <= segments.size(); ++k) {
        if (k < segments.size() && segments[k].length() < minLengthAppendedByReference)
            continue;
        size_t end = k < segments.size() ? segments[k].offset - charactersOffset : characters.size();
        auto shortSegments = segments.subspan(firstSmall, k - firstSmall);
        size_t length = end - taken;
        for (auto& segment : shortSegments)
            length += segment.length();
        if (length) {
            auto text = tryCreateStringWithSegments(characters.subspan(taken, end - taken), charactersOffset + taken, shortSegments, length);
            if (!text)
                return nullptr;
            pieces.append(jsString(vm, String { text.releaseNonNull() }));
        }
        if (k < segments.size())
            pieces.append(segments[k].rope ? segments[k].rope.get() : jsString(vm, segments[k].text));
        taken = end;
        firstSmall = k + 1;
    }
    JSString* result = asString(pieces.at(0));
    for (size_t k = 1; k < pieces.size(); k += 2) {
        result = k + 1 < pieces.size() ? jsString(globalObject, result, asString(pieces.at(k)), asString(pieces.at(k + 1))) : jsString(globalObject, result, asString(pieces.at(k)));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return result;
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedStringAppendFinish, JSCell*, (Instance* instance, uint64_t frame, uint32_t region, EncodedJSValue* firstPiece))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    syncStringAppender(instance, frame);
    auto& appender = *instance->stringAppender;
    if (!hasStringAppenderFrame(instance, frame, region)) {
        JSValue first = JSValue::decode(*firstPiece);
        OPERATION_RETURN(scope, first ? first.asCell() : jsEmptyString(vm));
    }
    auto& top = appender.frames.last();
    uint64_t length = instance->stringAppenderLength - top.base;
    if (top.isTooLong || length > maxStringLength) [[unlikely]] {
        popStringAppenderFrame(instance);
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    std::span<const Latin1Character> characters { appender.start + top.offset, stringAppenderOffset(instance) - top.offset };
    if (!length) {
        popStringAppenderFrame(instance);
        OPERATION_RETURN(scope, jsEmptyString(vm));
    }
    if (length == 1 && characters.size() == 1) {
        Latin1Character character = characters[0];
        popStringAppenderFrame(instance);
        OPERATION_RETURN(scope, jsSingleCharacterString(vm, character));
    }
    RefPtr<StringImpl> text;
    if (appender.segments.size() == top.firstSegment) [[likely]] {
        std::span<Latin1Character> copy;
        text = StringImpl::tryCreateUninitialized(characters.size(), copy);
        if (text) [[likely]]
            memcpy(copy.data(), characters.data(), characters.size());
    } else {
        auto segments = appender.segments.span().subspan(top.firstSegment);
        bool hasLargeSegment = std::ranges::any_of(segments, [](auto& segment) {
            return segment.length() >= minLengthAppendedByReference;
        });
        if (hasLargeSegment) {
            JSString* rope = tryCreateRopeWithSegments(globalObject, characters, top.offset, segments);
            popStringAppenderFrame(instance);
            if (!rope && !scope.exception())
                throwOutOfMemoryError(globalObject, scope);
            OPERATION_RETURN(scope, rope);
        }
        text = tryCreateStringWithSegments(characters, top.offset, segments, length);
    }
    popStringAppenderFrame(instance);
    if (!text) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        OPERATION_RETURN(scope, nullptr);
    }
    OPERATION_RETURN(scope, jsString(vm, String { text.releaseNonNull() }));
}

void destroyStringAppender(Instance& instance)
{
    if (!instance.stringAppender)
        return;
    fastFree(instance.stringAppender->start);
    fastFree(instance.stringAppender->starts);
    delete instance.stringAppender;
}

namespace Typed {

using namespace B3;

std::optional<Writes> writesOfInlineStringAppend(InlineBuiltin which, std::span<const Type> arguments)
{
    if (which == InlineBuiltin::EscapeHTML)
        return writesIf(arguments.size() == 1 && arguments[0].hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull), Writes::RuntimeState);
    return Writes::RuntimeState;
}

std::optional<Operand> Lowering::lowerInlineStringAppend(InlineBuiltin which, ValueID result, const Value& value, Writes writes)
{
    auto& type = typeOf(result);
    if (which == InlineBuiltin::EscapeHTML)
        return ensure(AOT_TYPED_OPERATION(EscapeHTML, toStringCell(value.arguments[0])), m_program.at(m_program.types.string), type);
    didEmitWrite(writes);
    auto nothing = [&] {
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    };
    Integer frame(m_out.framePointer());
    BuiltinID operation = value.builtin();
    auto firstPieceOf = [&](unsigned region) -> LValue {
        if (!region)
            return m_out.constIntPtr(0);
        while (m_stringAppendSlots.size() <= region)
            m_stringAppendSlots.append(nullptr);
        if (!m_stringAppendSlots[region])
            m_stringAppendSlots[region] = m_proc.addStackSlot(sizeof(EncodedJSValue));
        return m_out.m_block->appendNew<B3::SlotBaseValue>(m_proc, m_out.origin(), m_stringAppendSlots[region]);
    };
    auto owner = [&] {
        return m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfStringAppenderOwner()));
    };
    if (operation == Operations::StringAppendBegin) {
        auto flags = static_cast<uint32_t>(m_function.at(value.arguments[0]).integer());
        LValue firstPiece = firstPieceOf(flags >> stringAppendBeginRegionShift);
        if (!(flags & stringAppendBeginIsEager) || flags & stringAppendBeginIsFirst)
            m_out.store64(m_out.constInt64(0), m_out.address(m_heaps.AOTTyped_argumentBuffer, firstPiece, 0));
        if (flags & stringAppendBeginIsEager) {
            AOT_TYPED_OPERATION(StringAppendBegin, frame, word32(flags >> stringAppendBeginRegionShift | (flags & stringAppendBeginIsFirst ? 1u << 31 : 0)), Pointer(firstPiece));
            return nothing();
        }
        bool isNested = flags & stringAppendBeginIsNested;
        LBasicBlock hasAbandonedFrames = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isNested ? m_out.below(owner(), frame.value()) : m_out.belowOrEqual(owner(), frame.value()), rarely(hasAbandonedFrames), usually(continuation));
        m_out.appendTo(hasAbandonedFrames);
        AOT_TYPED_OPERATION(StringAppendReset, frame, word32(isNested));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return nothing();
    }
    if (operation == Operations::StringAppendSync) {
        LBasicBlock hasAbandonedFrames = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(m_out.below(owner(), frame.value()), rarely(hasAbandonedFrames), usually(continuation));
        m_out.appendTo(hasAbandonedFrames);
        AOT_TYPED_OPERATION(StringAppendSync, frame);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return nothing();
    }
    if (operation == Operations::StringAppendMayEscape) {
        static const BuiltinID escapeHTML = m_builtins.findStatic(BuiltinKind::StaticFunction, m_builtins.nameOf(asBytes("Bun"_s.span8())), m_builtins.nameOf(asBytes("escapeHTML"_s.span8())));
        Word32 isOriginal = equal(in<Boxed>(value.arguments[0]), box(builtinCell(m_builtins.cellOf(escapeHTML))));
        Boxed text = in<Boxed>(value.arguments[1]);
        bool mustBeString = m_function.at(value.arguments[2]).integer();
        Word32 isShort = either<Word32>(isCell(text), [&] {
            return lessThan(stringLength(stringReceiver(asCell(text))), integer(maxLengthEscapedInPlace + 1));
        }, [&] {
            return boolean(!mustBeString);
        });
        return booleanResult(logicalAnd(isOriginal, isShort), type);
    }
    if (operation == Operations::StringAppend || operation == Operations::StringAppendMeasure) {
        uint64_t kinds = static_cast<uint64_t>(m_function.at(value.arguments[0]).integer());
        uint64_t region = kinds >> stringAppendRegionShift;
        kinds &= (1ull << stringAppendRegionShift) - 1;
        auto field = [&](ptrdiff_t offset) {
            return m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, offset);
        };
        auto isStringIfCell = [](const Type& type) {
            return type.primitives & Primitive::String && !type.hasObjects() && !(type.primitives & (Primitive::Symbol | Primitive::BigInt));
        };
        bool copiesInline = !prefersGenericPathInColdCode();
        LBasicBlock continuation = nullptr;
        if (operation == Operations::StringAppendMeasure) {
            Word32 isBounded = boolean(true);
            unsigned operand = 1;
            for (uint64_t rest = kinds; rest & ((1u << stringAppendPartBits) - 1); rest >>= stringAppendPartBits) {
                auto kind = static_cast<StringAppendPart>(rest & ((1u << stringAppendPartBits) - 1));
                bool hasFlag = kind == StringAppendPart::HTMLEscapedValue || kind == StringAppendPart::HTMLEscapedSlice;
                if (hasFlag)
                    isBounded = logicalAnd(isBounded, equal(in<Boxed>(value.arguments[operand]), Boxed(m_out.constInt64(JSValue::ValueTrue))));
                operand += hasFlag || kind == StringAppendPart::Value || kind == StringAppendPart::NumberInRadix;
            }
            LBasicBlock mayBeTooLong = newColdBlock();
            continuation = m_out.newBlock();
            m_out.branch(isBounded.value(), usually(continuation), rarely(mayBeTooLong));
            m_out.appendTo(mayBeTooLong);
        } else if (constexpr uint64_t valueAndCheck = static_cast<uint64_t>(StringAppendPart::Value) | static_cast<uint64_t>(StringAppendPart::CheckRegionLength) << stringAppendPartBits; region && (kinds == static_cast<uint64_t>(StringAppendPart::Value) || kinds == valueAndCheck) && (copiesInline ? isStringIfCell(typeOf(value.arguments[1])) : typeOf(value.arguments[1]).isString()) && !constantASCIIString(value.arguments[1])) {
            LBasicBlock mayBeFirst = m_out.newBlock();
            LBasicBlock isFirst = m_out.newBlock();
            LBasicBlock isNotFirst = m_out.newBlock();
            continuation = m_out.newBlock();
            LValue firstPiece = firstPieceOf(region);
            LValue piece;
            if (auto& pieceType = typeOf(value.arguments[1]); pieceType.isString())
                piece = in<CellPointer>(value.arguments[1]).value();
            else {
                LBasicBlock isString = m_out.newBlock();
                Boxed boxed = in<Boxed>(value.arguments[1]);
                m_out.branch(isCell(boxed, pieceType).value(), usually(isString), rarely(isNotFirst));
                m_out.appendTo(isString);
                piece = asCell(boxed).value();
            }
            LValue currentOwner = owner();
            if (copiesInline) {
                LBasicBlock isOpen = m_out.newBlock();
                LBasicBlock isFlat = m_out.newBlock();
                LBasicBlock isNarrow = m_out.newBlock();
                LBasicBlock isShort = m_out.newBlock();
                LBasicBlock fits = m_out.newBlock();
                LBasicBlock isOneCharacter = m_out.newBlock();
                LBasicBlock isNotOneCharacter = m_out.newBlock();
                LBasicBlock copiesNext = m_out.newBlock();
                LBasicBlock isCopied = m_out.newBlock();
                LBasicBlock isNotOpen = m_out.newBlock();
                m_out.branch(m_out.equal(currentOwner, frame.value()), usually(isOpen), rarely(isNotOpen));
                m_out.appendTo(isOpen);
                Pointer fiber = loadField<Pointer>(CellPointer(piece), m_heaps.JSString_value);
                m_out.branch(hasNoBits(fiber, JSString::isRopeInPointer).value(), usually(isFlat), rarely(isNotFirst));
                m_out.appendTo(isFlat);
                m_out.branch(hasBits(loadField<Word32>(fiber, m_heaps.StringImpl_hashAndFlags, IsFixed), StringImpl::flagIs8Bit()).value(), usually(isNarrow), rarely(isNotFirst));
                m_out.appendTo(isNarrow);
                LValue size = m_out.zeroExtPtr(loadField<Word32>(fiber, m_heaps.StringImpl_length).value());
                m_out.branch(m_out.belowOrEqual(size, m_out.constIntPtr(maxLengthCopiedInline)), usually(isShort), rarely(isNotFirst));
                m_out.appendTo(isShort);
                LValue length = m_out.load64(field(OBJECT_OFFSETOF(Instance, stringAppenderLength)));
                LValue end = m_out.add(length, size);
                m_out.branch(m_out.belowOrEqual(end, m_out.load64(field(OBJECT_OFFSETOF(Instance, stringAppenderLimit)))), usually(fits), rarely(isNotFirst));
                m_out.appendTo(fits);
                LValue from = loadField<Pointer>(fiber, m_heaps.StringImpl_data).value();
                LValue to = m_out.add(m_out.load64(field(OBJECT_OFFSETOF(Instance, stringAppenderOrigin))), length);
                auto copyCharacterAt = [&](LValue index) {
                    m_out.store32As8(m_out.load8ZeroExt32(TypedPointer(m_heaps.characters8.atAnyIndex(), m_out.add(from, index))), m_out.address(m_heaps.AOTTyped_runtimeState, m_out.add(to, index), 0));
                };
                m_out.branch(m_out.equal(size, m_out.constIntPtr(1)), unsure(isOneCharacter), unsure(isNotOneCharacter));
                m_out.appendTo(isOneCharacter);
                copyCharacterAt(m_out.constIntPtr(0));
                m_out.jump(isCopied);
                m_out.appendTo(isNotOneCharacter);
                ValueFromBlock firstIndex = m_out.anchor(m_out.constIntPtr(0));
                m_out.branch(m_out.isZero64(size), rarely(isCopied), usually(copiesNext));
                m_out.appendTo(copiesNext);
                LValue index = m_out.phi(pointerType(), firstIndex);
                copyCharacterAt(index);
                LValue nextIndex = m_out.add(index, m_out.constIntPtr(1));
                m_out.addIncomingToPhi(index, m_out.anchor(nextIndex));
                m_out.branch(m_out.below(nextIndex, size), usually(copiesNext), rarely(isCopied));
                m_out.appendTo(isCopied);
                m_out.store64(end, field(OBJECT_OFFSETOF(Instance, stringAppenderLength)));
                m_out.jump(continuation);
                m_out.appendTo(isNotOpen);
            }
            m_out.branch(m_out.above(currentOwner, frame.value()), unsure(mayBeFirst), unsure(isNotFirst));
            m_out.appendTo(mayBeFirst);
            m_out.branch(m_out.isZero64(m_out.load64(m_out.address(m_heaps.AOTTyped_argumentBuffer, firstPiece, 0))), unsure(isFirst), unsure(isNotFirst));
            m_out.appendTo(isFirst);
            m_out.store64(piece, m_out.address(m_heaps.AOTTyped_argumentBuffer, firstPiece, 0));
            m_out.jump(continuation);
            m_out.appendTo(isNotFirst);
        } else if (!region && (kinds == static_cast<uint64_t>(StringAppendPart::PushStart) || kinds == static_cast<uint64_t>(StringAppendPart::PopStart))) {
            LBasicBlock isSynced = m_out.newBlock();
            LBasicBlock slowPath = newColdBlock();
            continuation = m_out.newBlock();
            m_out.branch(m_out.below(m_out.load64(field(Instance::offsetOfStringAppenderOwner())), frame.value()), rarely(slowPath), usually(isSynced));
            m_out.appendTo(isSynced);
            LValue top = m_out.load64(field(Instance::offsetOfStringAppenderStartsTop()));
            if (kinds == static_cast<uint64_t>(StringAppendPart::PopStart))
                m_out.store64(m_out.sub(top, m_out.constInt64(sizeof(uint64_t))), field(Instance::offsetOfStringAppenderStartsTop()));
            else {
                LBasicBlock hasRoom = m_out.newBlock();
                m_out.branch(m_out.equal(top, m_out.load64(field(Instance::offsetOfStringAppenderStartsEnd()))), rarely(slowPath), usually(hasRoom));
                m_out.appendTo(hasRoom);
                m_out.store64(m_out.load64(field(Instance::offsetOfStringAppenderLength())), m_out.address(m_heaps.AOTTyped_runtimeState, top, 0));
                m_out.store64(m_out.add(top, m_out.constInt64(sizeof(uint64_t))), field(Instance::offsetOfStringAppenderStartsTop()));
            }
            m_out.jump(continuation);
            m_out.appendTo(slowPath);
        }
        Vector<LValue, 20> words;
        unsigned next = 1;
        for (;; kinds >>= stringAppendPartBits) {
            auto kind = static_cast<StringAppendPart>(kinds & ((1u << stringAppendPartBits) - 1));
            if (kind == StringAppendPart::End)
                break;
            if (kind != StringAppendPart::Value) {
                words.append(m_out.constInt64(static_cast<int64_t>(static_cast<uint64_t>(kind) << stringAppendPayloadShift | stringAppendControlTag)));
                if (kind == StringAppendPart::HTMLEscapedValue || kind == StringAppendPart::HTMLEscapedSlice || kind == StringAppendPart::NumberInRadix)
                    words.append(in<Boxed>(value.arguments[next++]).value());
                continue;
            }
            ValueID part = value.arguments[next++];
            if (auto text = constantASCIIString(part); text && !text->empty()) {
                words.append(m_out.constInt64(static_cast<int64_t>(static_cast<uint64_t>(m_function.at(stringConstantOf(part)).string().index()) << stringAppendPayloadShift | stringAppendConstantTag)));
                continue;
            }
            auto& partType = typeOf(part);
            Operand operand = get(part);
            if (operand.rep == Rep::Void)
                words.append(undefined().value());
            else if (operand.rep == Rep::Double) {
                Number number = operand.as<Number>();
                Word32 low = low32(truncateToInteger(number));
                words.append(select(equal(toNumber(low), number), boxInt32(low), box(purified(number))).value());
            } else
                words.append(convertTo<Boxed>(operand, partType).value());
        }
        LValue buffer = transientBuffer(words.size() * sizeof(EncodedJSValue));
        for (unsigned i = 0; i < words.size(); ++i)
            m_out.store64(words[i], m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
        if (operation == Operations::StringAppendMeasure)
            AOT_TYPED_OPERATION(StringAppendMeasure, frame, integer(words.size()), Pointer(buffer));
        else
            AOT_TYPED_OPERATION(StringAppend, frame, integer(words.size() | region << 32), Pointer(buffer), Pointer(firstPieceOf(region)));
        if (continuation) {
            m_out.jump(continuation);
            m_out.appendTo(continuation);
        }
        return nothing();
    }
    RELEASE_ASSERT(operation == Operations::StringAppendFinish);
    auto region = static_cast<uint32_t>(m_function.at(value.arguments[0]).integer());
    LValue firstPiece = firstPieceOf(region);
    LBasicBlock isNotOpen = m_out.newBlock();
    LBasicBlock isOpen = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(m_out.above(owner(), frame.value()), unsure(isNotOpen), unsure(isOpen));
    m_out.appendTo(isNotOpen);
    LValue first = m_out.load64(m_out.address(m_heaps.AOTTyped_argumentBuffer, firstPiece, 0));
    ValueFromBlock kept = m_out.anchor(m_out.select(m_out.isZero64(first), emptyString(), first));
    m_out.jump(continuation);
    m_out.appendTo(isOpen);
    CellPointer finished = AOT_TYPED_OPERATION(StringAppendFinish, frame, word32(region), Pointer(firstPiece));
    ValueFromBlock made = m_out.anchor(finished.value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return ensure(CellPointer(m_out.phi(pointerType(), kept, made)), m_program.at(m_program.types.string), type);
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
