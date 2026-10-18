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
#include "GCMemoryOperations.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSONCacheInlines.h"
#include "JSONObject.h"
#include "ObjectConstructor.h"
#include "StructureCache.h"
#include <wtf/BitSet.h>
#include <wtf/FastFloat.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/unicode/UTF8Conversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr size_t jsonSourcePadding = 64;
static constexpr size_t minJSONSourceCapacity = 1 * KB;
static constexpr size_t maxRetainedJSONSourceCapacity = 64 * KB;
static constexpr unsigned maxSkippedJSONDepth = 64;
static constexpr unsigned maxExactMantissaDigits = 15;

static ALWAYS_INLINE uint8_t* poisonUnusedCapacity(uint8_t* start, size_t size, size_t capacity)
{
#if ASAN_ENABLED
    __asan_unpoison_memory_region(start, size);
    __asan_poison_memory_region(start + size, capacity - size);
#else
    UNUSED_PARAM(size);
    UNUSED_PARAM(capacity);
#endif
    return start;
}

static void freeJSONSource(Instance& instance)
{
#if ASAN_ENABLED
    __asan_unpoison_memory_region(instance.jsonSourceStart, instance.jsonSourceCapacity);
#endif
    fastFree(std::exchange(instance.jsonSourceStart, nullptr));
    instance.jsonSourceCapacity = 0;
}

static NEVER_INLINE uint8_t* replaceJSONSource(Instance* instance, size_t size)
{
    freeJSONSource(*instance);
    size_t capacity = std::max(WTF::roundUpToPowerOfTwo(size), minJSONSourceCapacity);
    void* storage = nullptr;
    if (!tryFastMalloc(capacity).getValue(storage))
        return nullptr;
    instance->jsonSourceStart = static_cast<uint8_t*>(storage);
    instance->jsonSourceCapacity = capacity;
    return poisonUnusedCapacity(instance->jsonSourceStart, size, capacity);
}

static ALWAYS_INLINE uint8_t* reserveJSONSource(Instance* instance, size_t size)
{
    size_t capacity = instance->jsonSourceCapacity;
    if (size <= capacity && (capacity <= maxRetainedJSONSourceCapacity || size > maxRetainedJSONSourceCapacity)) [[likely]]
        return poisonUnusedCapacity(instance->jsonSourceStart, size, capacity);
    return replaceJSONSource(instance, size);
}

static NEVER_INLINE const StringImpl* flattenJSONSource(Instance* instance, CallFrame* callFrame, JSString* string)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto value = string->tryGetValue();
    return value.data.impl();
}

static constexpr uint8_t saturatedCharacter = 0xff;

static NEVER_INLINE uint8_t* open16BitJSONSource(Instance* instance, std::span<const char16_t> characters)
{
    size_t length = characters.size();
    uint8_t* start = reserveJSONSource(instance, length + jsonSourcePadding);
    if (!start)
        return nullptr;
    for (size_t i = 0; i < length; ++i)
        start[i] = std::min<char16_t>(characters[i], saturatedCharacter);
    memset(start + length, 0, jsonSourcePadding);
    instance->jsonSourceEnd = start + length;
    instance->jsonSourceEncoding = JSONSourceEncoding::Saturated16Bit;
    instance->jsonSource16Bit = characters.data();
    return start;
}

static NEVER_INLINE bool isLatin1AfterDecoding(const Instance* instance, std::span<const uint8_t> characters)
{
    if (instance->jsonSourceEncoding == JSONSourceEncoding::UTF8)
        return charactersAreAllASCII(characters);
    return !memchr(characters.data(), saturatedCharacter, characters.size());
}

static ALWAYS_INLINE bool isLatin1(const Instance* instance, std::span<const uint8_t> characters)
{
    return instance->jsonSourceEncoding == JSONSourceEncoding::Latin1 || isLatin1AfterDecoding(instance, characters);
}

static ALWAYS_INLINE std::span<const char16_t> original16BitCharacters(const Instance* instance, std::span<const uint8_t> characters)
{
    return { instance->jsonSource16Bit + (characters.data() - instance->jsonSourceStart), characters.size() };
}

static std::span<const uint8_t> withoutByteOrderMark(std::span<const uint8_t> bytes)
{
    bool hasMark = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf;
    return bytes.subspan(hasMark ? 3 : 0);
}

static uint8_t* openUTF8JSONSource(Instance*, std::span<const uint8_t>, uint64_t frame);

static NEVER_INLINE uint8_t* openJSONBytes(Instance* instance, JSValue value, uint64_t frame)
{
    if (!value.isCell() || value.asCell()->type() != Uint8ArrayType || frame < instance->jsonParserFrame)
        return nullptr;
    return openUTF8JSONSource(instance, withoutByteOrderMark(uncheckedDowncast<JSArrayBufferView>(value.asCell())->span()), frame);
}

static uint8_t* openUTF8JSONSource(Instance* instance, std::span<const uint8_t> bytes, uint64_t frame)
{
    uint8_t* start = reserveJSONSource(instance, bytes.size() + jsonSourcePadding);
    if (!start)
        return nullptr;
    if (!bytes.empty())
        memcpy(start, bytes.data(), bytes.size());
    memset(start + bytes.size(), 0, jsonSourcePadding);
    instance->jsonSourceEnd = start + bytes.size();
    instance->jsonSourceEncoding = charactersAreAllASCII(bytes) ? JSONSourceEncoding::Latin1 : JSONSourceEncoding::UTF8;
    instance->jsonParserGaveUp = 0;
    instance->jsonParserFrame = frame;
    instance->jsonKeyOrderTop = instance->jsonKeyOrderStart;
    return start;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONDecodeSource, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isCell() || value.asCell()->type() != Uint8ArrayType)
        return encodedValue;
    String text = String::fromUTF8ReplacingInvalidSequences(withoutByteOrderMark(uncheckedDowncast<JSArrayBufferView>(value.asCell())->span()));
    return text.isNull() ? encodedValue : JSValue::encode(jsString(vm, WTF::move(text)));
}

static ALWAYS_INLINE uint8_t* openJSONString(Instance* instance, StringView text, uint64_t frame)
{
    instance->jsonParserGaveUp = 0;
    instance->jsonParserFrame = frame;
    instance->jsonKeyOrderTop = instance->jsonKeyOrderStart;
    if (!text.is8Bit()) [[unlikely]]
        return open16BitJSONSource(instance, text.span16());
    size_t length = text.length();
    uint8_t* start = reserveJSONSource(instance, length + jsonSourcePadding);
    if (!start) [[unlikely]]
        return nullptr;
    memcpy(start, text.span8().data(), length);
    memset(start + length, 0, jsonSourcePadding);
    instance->jsonSourceEnd = start + length;
    instance->jsonSourceEncoding = JSONSourceEncoding::Latin1;
    return start;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONOpen, void*, (Instance* instance, EncodedJSValue encodedSource, uint64_t frame))
{
    countOperationNamed(instance, __func__);
    JSValue text = JSValue::decode(encodedSource);
    if (!text.isString() || frame < instance->jsonParserFrame) [[unlikely]]
        return openJSONBytes(instance, text, frame);
    JSString* string = asString(text);
    const StringImpl* impl = string->tryGetValueImpl();
    if (!impl) [[unlikely]] {
        impl = flattenJSONSource(instance, DECLARE_CALL_FRAME(*instance->vm), string);
        if (!impl)
            return nullptr;
    }
    return openJSONString(instance, StringView { impl }, frame);
}

static ALWAYS_INLINE const uint8_t* skipJSONWhitespaceCharacters(const uint8_t* cursor)
{
    while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r' || *cursor == '\t')
        ++cursor;
    return cursor;
}

static ALWAYS_INLINE const uint8_t* findEndOfPlainJSONCharacters(const uint8_t* cursor)
{
    constexpr auto quotes = SIMD::splat<uint8_t>('"');
    constexpr auto escapes = SIMD::splat<uint8_t>('\\');
    constexpr auto spaces = SIMD::splat<uint8_t>(' ');
    for (;; cursor += SIMD::stride<uint8_t>) {
        auto input = SIMD::load(cursor);
        auto found = SIMD::bitOr(SIMD::equal(input, quotes), SIMD::equal(input, escapes), SIMD::lessThan(input, spaces));
        if (auto index = SIMD::findFirstNonZeroIndex(found))
            return cursor + *index;
    }
}

static ALWAYS_INLINE unsigned jsonEscapeLength(const uint8_t* backslash)
{
    switch (backslash[1]) {
    case '"':
    case '\\':
    case '/':
    case 'b':
    case 'f':
    case 'n':
    case 'r':
    case 't':
        return 2;
    case 'u':
        return isASCIIHexDigit(backslash[2]) && isASCIIHexDigit(backslash[3]) && isASCIIHexDigit(backslash[4]) && isASCIIHexDigit(backslash[5]) ? 6 : 0;
    default:
        return 0;
    }
}

static ALWAYS_INLINE const uint8_t* skipJSONString(const uint8_t* cursor, bool& hasEscapes)
{
    for (;;) {
        cursor = findEndOfPlainJSONCharacters(cursor);
        if (*cursor == '"') [[likely]]
            return cursor + 1;
        if (*cursor != '\\')
            return nullptr;
        unsigned length = jsonEscapeLength(cursor);
        if (!length)
            return nullptr;
        hasEscapes = true;
        cursor += length;
    }
}

static ALWAYS_INLINE const uint8_t* skipJSONNumber(const uint8_t* cursor)
{
    cursor += *cursor == '-';
    if (!isASCIIDigit(*cursor))
        return nullptr;
    if (*cursor++ != '0') {
        while (isASCIIDigit(*cursor))
            ++cursor;
    }
    if (*cursor == '.') {
        if (!isASCIIDigit(*++cursor))
            return nullptr;
        while (isASCIIDigit(*cursor))
            ++cursor;
    }
    if ((*cursor | 0x20) == 'e') {
        ++cursor;
        cursor += *cursor == '-' || *cursor == '+';
        if (!isASCIIDigit(*cursor))
            return nullptr;
        while (isASCIIDigit(*cursor))
            ++cursor;
    }
    return cursor;
}

static const uint8_t* skipJSONValue(Instance* instance, const uint8_t* cursor)
{
    uint64_t objectDepthBits = 0;
    unsigned depth = 0;
    bool hasEscapes = false;
    for (;;) {
        bool expectsKey = false;
        switch (*cursor) {
        case '"':
            cursor = skipJSONString(cursor + 1, hasEscapes);
            if (!cursor)
                return nullptr;
            break;
        case '{':
        case '[': {
            bool isObject = *cursor == '{';
            cursor = skipJSONWhitespaceCharacters(cursor + 1);
            if (*cursor == (isObject ? '}' : ']')) {
                ++cursor;
                break;
            }
            if (depth == maxSkippedJSONDepth) {
                instance->jsonParserGaveUp = 1;
                return nullptr;
            }
            objectDepthBits = objectDepthBits << 1 | isObject;
            ++depth;
            if (!isObject)
                continue;
            expectsKey = true;
            break;
        }
        case 't':
            if (WTF::unalignedLoad<uint32_t>(cursor) != ('t' | 'r' << 8 | 'u' << 16 | 'e' << 24))
                return nullptr;
            cursor += 4;
            break;
        case 'f':
            if (WTF::unalignedLoad<uint32_t>(cursor + 1) != ('a' | 'l' << 8 | 's' << 16 | 'e' << 24))
                return nullptr;
            cursor += 5;
            break;
        case 'n':
            if (WTF::unalignedLoad<uint32_t>(cursor) != ('n' | 'u' << 8 | 'l' << 16 | 'l' << 24))
                return nullptr;
            cursor += 4;
            break;
        default:
            cursor = skipJSONNumber(cursor);
            if (!cursor)
                return nullptr;
            break;
        }
        while (!expectsKey) {
            if (!depth)
                return cursor;
            cursor = skipJSONWhitespaceCharacters(cursor);
            bool isObject = objectDepthBits & 1;
            if (*cursor == ',') {
                cursor = skipJSONWhitespaceCharacters(cursor + 1);
                expectsKey = isObject;
                break;
            }
            if (*cursor != (isObject ? '}' : ']'))
                return nullptr;
            ++cursor;
            objectDepthBits >>= 1;
            --depth;
        }
        if (!expectsKey)
            continue;
        if (*cursor != '"')
            return nullptr;
        cursor = skipJSONString(cursor + 1, hasEscapes);
        if (!cursor)
            return nullptr;
        cursor = skipJSONWhitespaceCharacters(cursor);
        if (*cursor != ':')
            return nullptr;
        cursor = skipJSONWhitespaceCharacters(cursor + 1);
    }
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONSkipValue, void*, (Instance* instance, uint64_t cursor))
{
    return const_cast<uint8_t*>(skipJSONValue(instance, std::bit_cast<const uint8_t*>(cursor)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONSkipMember, void*, (Instance* instance, uint64_t cursorBits))
{
    auto* cursor = std::bit_cast<const uint8_t*>(cursorBits);
    if (*cursor != '"')
        return nullptr;
    bool hasEscapes = false;
    cursor = skipJSONString(cursor + 1, hasEscapes);
    if (!cursor)
        return nullptr;
    if (hasEscapes) [[unlikely]]
        instance->jsonParserGaveUp = 1;
    cursor = skipJSONWhitespaceCharacters(cursor);
    if (*cursor != ':')
        return nullptr;
    return const_cast<uint8_t*>(skipJSONValue(instance, skipJSONWhitespaceCharacters(cursor + 1)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONGiveUpIfEscaped, void, (Instance* instance, uint64_t cursorBits))
{
    auto* cursor = std::bit_cast<const uint8_t*>(cursorBits);
    if (*cursor != '"')
        return;
    bool hasEscapes = false;
    skipJSONString(cursor + 1, hasEscapes);
    if (hasEscapes)
        instance->jsonParserGaveUp = 1;
}

static bool hasNoLaterMember(Instance* instance, const uint8_t* cursor, std::span<const Latin1Character> name)
{
    cursor = skipJSONValue(instance, cursor);
    while (cursor) {
        cursor = skipJSONWhitespaceCharacters(cursor);
        if (*cursor == '}')
            return true;
        if (*cursor != ',')
            return false;
        cursor = skipJSONWhitespaceCharacters(cursor + 1);
        if (*cursor != '"')
            return false;
        const uint8_t* key = cursor + 1;
        bool hasEscapes = false;
        cursor = skipJSONString(key, hasEscapes);
        if (!cursor || hasEscapes)
            return false;
        if (static_cast<size_t>(cursor - 1 - key) == name.size() && !memcmp(key, name.data(), name.size()))
            return false;
        cursor = skipJSONWhitespaceCharacters(cursor);
        if (*cursor != ':')
            return false;
        cursor = skipJSONValue(instance, skipJSONWhitespaceCharacters(cursor + 1));
    }
    return false;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONGiveUpIfRepeated, void, (Instance* instance, uint64_t cursor, JSCell* nameCell))
{
    const StringImpl* name = uncheckedDowncast<JSString>(nameCell)->tryGetValueImpl();
    if (!name || !name->is8Bit() || !hasNoLaterMember(instance, std::bit_cast<const uint8_t*>(cursor), name->span8()))
        instance->jsonParserGaveUp = 1;
}

static JSCell* makeStringWithEscapes(Instance*, VM&, const uint8_t* start, const uint8_t* cursor);

static NEVER_INLINE bool isNameWrittenWithEscapes(Instance* instance, CallFrame* callFrame, const uint8_t* key, std::span<const Latin1Character> name)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    instance->jsonCursor = 0;
    JSCell* decoded = makeStringWithEscapes(instance, vm, key, findEndOfPlainJSONCharacters(key));
    return instance->jsonCursor && StringView { uncheckedDowncast<JSString>(decoded)->tryGetValueImpl() } == StringView { name };
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONFindMember, void*, (Instance* instance, uint64_t cursorBits, JSCell* nameCell))
{
    countOperationNamed(instance, __func__);
    auto name = uncheckedDowncast<JSString>(nameCell)->tryGetValueImpl()->span8();
    const uint8_t* cursor = skipJSONWhitespaceCharacters(std::bit_cast<const uint8_t*>(cursorBits));
    while (*cursor == '"') {
        const uint8_t* key = cursor + 1;
        bool hasEscapes = false;
        cursor = skipJSONString(key, hasEscapes);
        if (!cursor)
            return nullptr;
        bool isFound = hasEscapes ? isNameWrittenWithEscapes(instance, DECLARE_CALL_FRAME(*instance->vm), key, name) : static_cast<size_t>(cursor - 1 - key) == name.size() && !memcmp(key, name.data(), name.size());
        cursor = skipJSONWhitespaceCharacters(cursor);
        if (*cursor != ':')
            return nullptr;
        cursor = skipJSONWhitespaceCharacters(cursor + 1);
        if (isFound)
            return const_cast<uint8_t*>(cursor);
        cursor = skipJSONValue(instance, cursor);
        if (!cursor)
            return nullptr;
        cursor = skipJSONWhitespaceCharacters(cursor);
        if (*cursor != ',')
            return nullptr;
        cursor = skipJSONWhitespaceCharacters(cursor + 1);
    }
    return nullptr;
}

static bool appendPlainCharacters(Instance* instance, StringBuilder& builder, std::span<const uint8_t> characters)
{
    if (isLatin1(instance, characters)) {
        builder.append(byteCast<Latin1Character>(characters));
        return true;
    }
    if (instance->jsonSourceEncoding == JSONSourceEncoding::Saturated16Bit) {
        builder.append(original16BitCharacters(instance, characters));
        return true;
    }
    String decoded = String::fromUTF8(characters);
    if (decoded.isNull()) {
        instance->jsonParserGaveUp = 1;
        return false;
    }
    builder.append(decoded);
    return true;
}

static NEVER_INLINE JSCell* makeStringWithEscapes(Instance* instance, VM& vm, const uint8_t* start, const uint8_t* cursor)
{
    StringBuilder builder(OverflowPolicy::RecordOverflow);
    for (;;) {
        if (!appendPlainCharacters(instance, builder, { start, cursor }))
            return jsEmptyString(vm);
        if (*cursor == '"')
            break;
        unsigned length = *cursor == '\\' ? jsonEscapeLength(cursor) : 0;
        if (!length)
            return jsEmptyString(vm);
        switch (cursor[1]) {
        case 'b':
            builder.append('\b');
            break;
        case 'f':
            builder.append('\f');
            break;
        case 'n':
            builder.append('\n');
            break;
        case 'r':
            builder.append('\r');
            break;
        case 't':
            builder.append('\t');
            break;
        case 'u':
            builder.append(static_cast<char16_t>(toASCIIHexValue(cursor[2]) << 12 | toASCIIHexValue(cursor[3]) << 8 | toASCIIHexValue(cursor[4]) << 4 | toASCIIHexValue(cursor[5])));
            break;
        default:
            builder.append(static_cast<Latin1Character>(cursor[1]));
            break;
        }
        start = cursor + length;
        cursor = findEndOfPlainJSONCharacters(start);
    }
    if (builder.hasOverflowed())
        return jsEmptyString(vm);
    JSString* made = builder.is8Bit() ? vm.jsonCache().tryMakeJSString(vm, builder.span8()) : vm.jsonCache().tryMakeJSString(vm, builder.span16());
    if (!made)
        return jsEmptyString(vm);
    instance->jsonCursor = std::bit_cast<uint64_t>(cursor + 1);
    return made;
}

static JSCell* makeUncachedString(Instance*, VM&, const uint8_t* start);

static NEVER_INLINE JSCell* makeUncachedString(Instance* instance, CallFrame* callFrame, const uint8_t* start)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return makeUncachedString(instance, vm, start);
}

static NEVER_INLINE JSCell* makeUncachedString(Instance* instance, VM& vm, const uint8_t* start)
{
    const uint8_t* cursor = findEndOfPlainJSONCharacters(start);
    instance->jsonCursor = 0;
    if (*cursor != '"') [[unlikely]]
        return makeStringWithEscapes(instance, vm, start, cursor);
    if (!isLatin1(instance, { start, cursor })) [[unlikely]] {
        if (instance->jsonSourceEncoding != JSONSourceEncoding::Saturated16Bit)
            return makeStringWithEscapes(instance, vm, start, cursor);
        auto characters = original16BitCharacters(instance, { start, cursor });
        std::span<char16_t> copy;
        auto impl = StringImpl::tryCreateUninitialized(characters.size(), copy);
        if (!impl)
            return jsEmptyString(vm);
        memcpy(copy.data(), characters.data(), characters.size_bytes());
        instance->jsonCursor = std::bit_cast<uint64_t>(cursor + 1);
        return jsString(vm, String { impl.releaseNonNull() });
    }
    JSString* made = vm.jsonCache().tryMakeJSString(vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(cursor) });
    if (!made) [[unlikely]]
        return jsEmptyString(vm);
    vm.jsonCache().addString(std::span<const Latin1Character> { byteCast<Latin1Character>(start - 1), jsonSourcePadding }, cursor + 2 - start, made);
    instance->jsonCursor = std::bit_cast<uint64_t>(cursor + 1);
    return made;
}

static ALWAYS_INLINE bool isShortTextLatin1AfterDecoding(const Instance* instance, const uint8_t* text, unsigned length)
{
    constexpr size_t stride = SIMD::stride<uint8_t>;
    ASSERT(length <= 2 * stride);
    constexpr simde_uint8x16_t lowIndices { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    constexpr simde_uint8x16_t highIndices { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    auto lengths = SIMD::splat<uint8_t>(length);
    auto largestUnchanged = SIMD::splat<uint8_t>(instance->jsonSourceEncoding == JSONSourceEncoding::UTF8 ? 0x7f : saturatedCharacter - 1);
    auto low = SIMD::bitAnd(SIMD::lessThan(largestUnchanged, SIMD::load(text)), SIMD::lessThan(lowIndices, lengths));
    auto high = SIMD::bitAnd(SIMD::lessThan(largestUnchanged, SIMD::load(text + stride)), SIMD::lessThan(highIndices, lengths));
    return !SIMD::isNonZero(SIMD::bitOr(low, high));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONStringAt, JSCell*, (Instance* instance, uint64_t cursorBits))
{
    countOperationNamed(instance, __func__);
    auto* start = std::bit_cast<const uint8_t*>(cursorBits);
    if (auto* entry = instance->vm->jsonCache().findString(std::span<const Latin1Character> { byteCast<Latin1Character>(start - 1), jsonSourcePadding })) [[likely]] {
        if (instance->jsonSourceEncoding == JSONSourceEncoding::Latin1 || isShortTextLatin1AfterDecoding(instance, start - 1, entry->textLength)) [[likely]] {
            instance->jsonCursor = std::bit_cast<uint64_t>(start - 1 + entry->textLength);
            return entry->string;
        }
    }
    return makeUncachedString(instance, DECLARE_CALL_FRAME(*instance->vm), start);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONKeyAt, JSCell*, (Instance* instance, uint64_t cursorBits))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* start = std::bit_cast<const uint8_t*>(cursorBits);
    const uint8_t* cursor = findEndOfPlainJSONCharacters(start);
    instance->jsonCursor = 0;
    if (*cursor != '"') [[unlikely]] {
        instance->jsonParserGaveUp = 1;
        return jsEmptyString(vm);
    }
    if (!isLatin1(instance, { start, cursor })) [[unlikely]]
        return makeStringWithEscapes(instance, vm, start, cursor);
    JSString* made = vm.jsonCache().tryMakeJSString(vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(cursor) });
    if (!made) [[unlikely]]
        return jsEmptyString(vm);
    instance->jsonCursor = std::bit_cast<uint64_t>(cursor + 1);
    return made;
}

static NEVER_INLINE void* parseNumberSlow(Instance* instance, const uint8_t* start)
{
    size_t length = 0;
    auto number = WTF::parseJSONDouble(std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(instance->jsonSourceEnd) }, length);
    if (!number || !std::isfinite(*number))
        return nullptr;
    instance->jsonNumber = *number;
    return const_cast<uint8_t*>(start + length);
}

static ALWAYS_INLINE void* parseNumber(Instance* instance, const uint8_t* start)
{
    const uint8_t* cursor = start;
    bool isNegative = *cursor == '-';
    cursor += isNegative;
    const uint8_t* digits = cursor;
    unsigned digit = *cursor - '0';
    if (digit > 9) [[unlikely]]
        return nullptr;
    uint64_t mantissa = 0;
    if (!digit) {
        if (isASCIIDigit(*++cursor)) [[unlikely]]
            return nullptr;
    } else {
        do {
            mantissa = mantissa * 10 + digit;
            digit = *++cursor - '0';
        } while (digit <= 9);
    }
    size_t count = cursor - digits;
    size_t fractionDigitCount = 0;
    if (*cursor == '.') {
        const uint8_t* fraction = ++cursor;
        digit = *cursor - '0';
        if (digit > 9) [[unlikely]]
            return nullptr;
        do {
            mantissa = mantissa * 10 + digit;
            digit = *++cursor - '0';
        } while (digit <= 9);
        fractionDigitCount = cursor - fraction;
    }
    if (count + fractionDigitCount > maxExactMantissaDigits || (*cursor | 0x20) == 'e') [[unlikely]]
        return parseNumberSlow(instance, start);
    double number = static_cast<double>(static_cast<int64_t>(mantissa));
    if (fractionDigitCount)
        number /= powersOfTen[fractionDigitCount];
    instance->jsonNumber = isNegative ? -number : number;
    return const_cast<uint8_t*>(cursor);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONNumberAt, void*, (Instance* instance, uint64_t cursor))
{
    return parseNumber(instance, std::bit_cast<const uint8_t*>(cursor));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONIntegerAt, void*, (Instance* instance, uint64_t cursor))
{
    void* end = parseNumber(instance, std::bit_cast<const uint8_t*>(cursor));
    if (!end)
        return nullptr;
    double number = instance->jsonNumber;
    if (std::trunc(number) != number)
        return nullptr;
    if (std::abs(number) > static_cast<double>(Typed::maxExactInteger) || (!number && std::signbit(number))) {
        instance->jsonParserGaveUp = 1;
        return nullptr;
    }
    instance->jsonNumber = std::bit_cast<double>(static_cast<int64_t>(number));
    return end;
}

static EncodedJSValue parseJSONValueAt(Instance*, VM&, JSGlobalObject*, const uint8_t* start);

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONValueAt, EncodedJSValue, (Instance* instance, uint64_t cursorBits))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    return parseJSONValueAt(instance, vm, globalObject, std::bit_cast<const uint8_t*>(cursorBits));
}

static ALWAYS_INLINE std::optional<JSValue> tryParseJSONScalarAt(Instance* instance, VM& vm, const uint8_t* start)
{
    auto endsAfter = [&](size_t length, JSValue value) {
        instance->jsonCursor = std::bit_cast<uint64_t>(start + length);
        return value;
    };
    switch (*start) {
    case '"': {
        if (auto* entry = vm.jsonCache().findString(std::span<const Latin1Character> { byteCast<Latin1Character>(start), jsonSourcePadding })) {
            if (instance->jsonSourceEncoding == JSONSourceEncoding::Latin1 || isShortTextLatin1AfterDecoding(instance, start, entry->textLength))
                return endsAfter(entry->textLength, entry->string);
        }
        JSCell* string = makeUncachedString(instance, vm, start + 1);
        return instance->jsonCursor ? JSValue(string) : jsUndefined();
    }
    case 't':
        if (WTF::unalignedLoad<uint32_t>(start) == ('t' | 'r' << 8 | 'u' << 16 | 'e' << 24))
            return endsAfter(4, jsBoolean(true));
        return std::nullopt;
    case 'f':
        if (WTF::unalignedLoad<uint32_t>(start + 1) == ('a' | 'l' << 8 | 's' << 16 | 'e' << 24))
            return endsAfter(5, jsBoolean(false));
        return std::nullopt;
    case 'n':
        if (WTF::unalignedLoad<uint32_t>(start) == ('n' | 'u' << 8 | 'l' << 16 | 'l' << 24))
            return endsAfter(4, jsNull());
        return std::nullopt;
    case '{':
    case '[':
        return std::nullopt;
    default:
        if (void* end = parseNumber(instance, start))
            return endsAfter(static_cast<const uint8_t*>(end) - start, jsNumber(instance->jsonNumber));
        return std::nullopt;
    }
}

static EncodedJSValue parseJSONValueAt(Instance* instance, VM& vm, JSGlobalObject* globalObject, const uint8_t* start)
{
    instance->jsonCursor = 0;
    if (instance->jsonParserGaveUp) [[unlikely]]
        return JSValue::encode(jsUndefined());
    if (auto scalar = tryParseJSONScalarAt(instance, vm, start))
        return JSValue::encode(*scalar);
    const uint8_t* end = skipJSONValue(instance, start);
    if (!end)
        return JSValue::encode(jsUndefined());
    std::span<const uint8_t> characters { start, end };
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue parsed;
    if (isLatin1(instance, characters))
        parsed = JSONParse(globalObject, StringView { byteCast<Latin1Character>(characters) });
    else if (instance->jsonSourceEncoding == JSONSourceEncoding::Saturated16Bit)
        parsed = JSONParse(globalObject, StringView { original16BitCharacters(instance, characters) });
    else
        parsed = JSONParse(globalObject, String::fromUTF8(characters));
    if (catchScope.exception()) [[unlikely]] {
        catchScope.clearException();
        parsed = JSValue();
    }
    if (!parsed) {
        instance->jsonParserGaveUp = 1;
        return JSValue::encode(jsUndefined());
    }
    instance->jsonCursor = std::bit_cast<uint64_t>(end);
    return JSValue::encode(parsed);
}

static constexpr size_t jsonKeyOrderCapacity = 4096;
static constexpr uint16_t recordsExtraProperties = 0x8000;
static constexpr unsigned maxNumberOfKnownKeyOrders = 1 << 14;
static constexpr unsigned minInlineCapacityOfExtraProperties = 2;
static constexpr unsigned minNumberOfKnownExtraMembers = 1 << 8;
static constexpr unsigned maxNumberOfKnownExtraMembers = 1 << 14;
static constexpr unsigned maxExtraMembersAddedAtOnce = 32;
static constexpr ptrdiff_t maxDigitsOfSmallJSONInteger = 9;

struct JSONKnownExtraMember {
    ALWAYS_INLINE bool isAt(const uint8_t* text) const
    {
        if ((WTF::unalignedLoad<uint64_t>(text) & firstBytesMask) != firstBytes)
            return false;
        if (size <= 8)
            return true;
        if (WTF::unalignedLoad<uint64_t>(text + size - 8) != lastBytes)
            return false;
        return size <= 16 || !memcmp(text + 8, after->transitionPropertyName()->span8().data() + 8, size - 16);
    }

    StructureID before;
    PropertyOffset offset { invalidOffset };
    Structure* after { nullptr };
    uint64_t firstBytes { 0 };
    uint64_t firstBytesMask { 0 };
    uint64_t lastBytes { 0 };
    unsigned size { 0 };
    unsigned likelyNext { 0 };
};

struct JSONKnownKeyOrder {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(JSONKnownKeyOrder);
    JSCell* declaredNames { nullptr };
    StructureID object;
    StructureID extraProperties;
    uint32_t orderAndCount { 0 };
    bool isUsedSinceFullCollection { true };
    Strong<Structure> withExtraProperties;
    Strong<Structure> ofExtraProperties;
    Vector<uint16_t, 16> recorded;
};

struct JSONKeyOrders {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(JSONKeyOrders);
    std::array<uint16_t, jsonKeyOrderCapacity> recorded;
    UncheckedKeyHashMap<uint64_t, std::unique_ptr<JSONKnownKeyOrder>> known;
    JSCell* lastDeclaredNames { nullptr };
    Structure* lastEmptyStructure { nullptr };
    UncheckedKeyHashMap<JSCell*, Strong<Structure>> emptyStructures;
    Vector<JSONKnownExtraMember> members = Vector<JSONKnownExtraMember>(minNumberOfKnownExtraMembers);
    unsigned shiftForMembers { 32 - WTF::fastLog2(minNumberOfKnownExtraMembers) };
    unsigned numberOfReplacedMembers { 0 };
    bool hasMembers { false };
};

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONReserveKeyOrder, void*, (Instance* instance))
{
    countOperationNamed(instance, __func__);
    auto* orders = new JSONKeyOrders;
    instance->jsonKeyOrders = orders;
    instance->jsonKeyOrderStart = instance->jsonKeyOrderTop = orders->recorded.data();
    instance->jsonKeyOrderEnd = orders->recorded.data() + orders->recorded.size();
    return instance->jsonKeyOrderTop;
}

static ALWAYS_INLINE void recordExtraProperty(Instance* instance, const uint16_t* mark, unsigned ordinal)
{
    uint16_t* top = instance->jsonKeyOrderTop;
    if (ordinal >= recordsExtraProperties) [[unlikely]] {
        instance->jsonParserGaveUp = 1;
        return;
    }
    uint16_t entry = recordsExtraProperties | ordinal;
    if (top > mark && top[-1] & recordsExtraProperties) {
        top[-1] = entry;
        return;
    }
    if (top == instance->jsonKeyOrderEnd) [[unlikely]] {
        instance->jsonParserGaveUp = 1;
        return;
    }
    *top = entry;
    instance->jsonKeyOrderTop = top + 1;
}

static NEVER_INLINE Structure* findEmptyStructureOfExtraProperties(Instance* instance, VM& vm, JSGlobalObject* globalObject, JSCell* declaredNames)
{
    auto& orders = *instance->jsonKeyOrders;
    auto& structure = orders.emptyStructures.ensure(declaredNames, [&] {
        return Strong<Structure>(vm, JSFinalObject::createStructure(vm, globalObject, jsNull(), minInlineCapacityOfExtraProperties));
    }).iterator->value;
    orders.lastDeclaredNames = declaredNames;
    orders.lastEmptyStructure = structure.get();
    return structure.get();
}

static ALWAYS_INLINE JSObject* createExtraProperties(Instance* instance, VM& vm, JSGlobalObject* globalObject, JSCell* declaredNames)
{
    auto& orders = *instance->jsonKeyOrders;
    Structure* structure = orders.lastDeclaredNames == declaredNames ? orders.lastEmptyStructure : findEmptyStructureOfExtraProperties(instance, vm, globalObject, declaredNames);
    return JSFinalObject::create(vm, structure);
}

static void widenExtraProperties(Instance* instance, VM& vm, JSCell* declaredNames, unsigned numberOfProperties)
{
    auto& orders = *instance->jsonKeyOrders;
    auto found = orders.emptyStructures.find(declaredNames);
    unsigned inlineCapacity = std::min<unsigned>(WTF::roundUpToMultipleOf<2>(numberOfProperties), JSFinalObject::maxInlineCapacity);
    if (found == orders.emptyStructures.end() || found->value->inlineCapacity() >= inlineCapacity)
        return;
    found->value.set(vm, JSFinalObject::createStructure(vm, instance->globalObject, jsNull(), inlineCapacity));
    orders.lastDeclaredNames = nullptr;
}

static ALWAYS_INLINE const uint8_t* skipJSONColon(const uint8_t* cursor)
{
    if (*cursor != ':') [[unlikely]] {
        cursor = skipJSONWhitespaceCharacters(cursor);
        if (*cursor != ':')
            return nullptr;
    }
    return cursor + 1;
}

static ALWAYS_INLINE unsigned numberOfPropertiesOf(Structure* structure)
{
    return numberOfSlotsForMaxOffset(structure->maxOffset(), structure->inlineCapacity());
}

static void addExtraProperty(Instance* instance, VM& vm, JSGlobalObject* globalObject, JSObject* extra, UniquedStringImpl* name, JSValue value, const uint16_t* mark)
{
    if (auto index = parseIndex(*name)) [[unlikely]] {
        extra->putDirectIndex(globalObject, *index, value);
        return;
    }
    Structure* from = extra->structure();
    unsigned attributes;
    if (PropertyOffset offset = from->get(vm, name, attributes); isValidOffset(offset)) {
        extra->putDirectOffset(vm, offset, value);
        return;
    }
    extra->putDirect(vm, name, value);
    recordExtraProperty(instance, mark, numberOfPropertiesOf(from));
}

static ALWAYS_INLINE JSONKnownExtraMember* pairOfKnownExtraMembersFor(JSONKeyOrders& orders, StructureID before, const uint8_t* name)
{
    uint32_t hash = ((before.bits() >> 4) ^ WTF::unalignedLoad<uint16_t>(name) * 0x85ebca6bu) * 0x9e3779b1u;
    return orders.members.mutableSpan().data() + (hash >> orders.shiftForMembers & ~1u);
}

static ALWAYS_INLINE JSONKnownExtraMember* findKnownExtraMember(JSONKeyOrders& orders, StructureID before, const uint8_t* name)
{
    auto* pair = pairOfKnownExtraMembersFor(orders, before, name);
    if (pair[0].before == before && pair[0].isAt(name)) [[likely]]
        return pair;
    return pair[1].before == before && pair[1].isAt(name) ? pair + 1 : nullptr;
}

static NEVER_INLINE Structure* findTransitionForName(Instance* instance, VM& vm, Structure* from, const uint8_t* name, const uint8_t*& afterName)
{
    const uint8_t* quote = findEndOfPlainJSONCharacters(name);
    if (*quote != '"' || !isLatin1(instance, { name, quote }) || from->isDictionary())
        return nullptr;
    afterName = quote + 1;
    std::span<const Latin1Character> key { byteCast<Latin1Character>(name), byteCast<Latin1Character>(quote) };
    Structure* to = from->trySingleTransition();
    if (to) {
        UniquedStringImpl* added = to->transitionPropertyName();
        if (to->transitionKind() != TransitionKind::PropertyAddition || to->transitionPropertyAttributes() || added->isSymbol() || !added->is8Bit() || !equalSpans(added->span8(), key))
            return nullptr;
    } else {
        RefPtr<AtomStringImpl> atom = vm.jsonCache().existingIdentifier(vm, key);
        if (!atom)
            atom = AtomStringImpl::lookUp(key);
        PropertyOffset offset;
        to = atom ? Structure::addPropertyTransitionToExistingStructure(from, atom.get(), 0, offset) : nullptr;
        if (!to)
            return nullptr;
    }
    if (*afterName != ':' || from->outOfLineCapacity() != to->outOfLineCapacity() || !charactersAreAllASCII(key))
        return to;
    auto& orders = *instance->jsonKeyOrders;
    auto* known = pairOfKnownExtraMembersFor(orders, from->id(), name);
    if (known[1].after && ++orders.numberOfReplacedMembers > orders.members.size() / 4 && orders.members.size() < maxNumberOfKnownExtraMembers) {
        orders.members = Vector<JSONKnownExtraMember>(2 * orders.members.size());
        --orders.shiftForMembers;
        orders.numberOfReplacedMembers = 0;
        known = pairOfKnownExtraMembersFor(orders, from->id(), name);
    }
    known[1] = known[0];
    unsigned size = key.size() + 2;
    uint64_t mask = size >= 8 ? std::numeric_limits<uint64_t>::max() : (1ULL << (8 * size)) - 1;
    *known = { from->id(), to->transitionOffset(), to, WTF::unalignedLoad<uint64_t>(name) & mask, mask, size > 8 ? WTF::unalignedLoad<uint64_t>(name + size - 8) : 0, size, 0 };
    orders.hasMembers = true;
    return to;
}

static ALWAYS_INLINE const uint8_t* tryParseSmallJSONInteger(const uint8_t* start, JSValue& value)
{
    unsigned digit = *start - '0';
    if (digit > 9)
        return nullptr;
    const uint8_t* cursor = start + 1;
    uint32_t number = digit;
    unsigned next = *cursor - '0';
    if (digit) {
        for (; next <= 9; next = *++cursor - '0')
            number = number * 10 + next;
    }
    if (next <= 9 || cursor - start > maxDigitsOfSmallJSONInteger || *cursor == '.' || (*cursor | 0x20) == 'e') [[unlikely]]
        return nullptr;
    value = jsNumber(static_cast<int32_t>(number));
    return cursor;
}

static NEVER_INLINE const uint8_t* parseExtraMemberWithNewName(Instance* instance, VM& vm, JSGlobalObject* globalObject, JSObject* extra, JSCell* declaredNames, const uint8_t* start, const uint16_t* mark)
{
    const uint8_t* quote = findEndOfPlainJSONCharacters(start);
    const uint8_t* afterName = quote + 1;
    std::span<const uint8_t> characters { start, quote };
    RefPtr<UniquedStringImpl> name;
    if (*quote == '"' && isLatin1(instance, characters))
        name = vm.jsonCache().makeIdentifier(vm, byteCast<Latin1Character>(characters));
    else {
        instance->jsonCursor = 0;
        JSCell* decoded = makeStringWithEscapes(instance, vm, start, quote);
        afterName = std::bit_cast<const uint8_t*>(instance->jsonCursor);
        if (!afterName)
            return nullptr;
        name = uncheckedDowncast<JSString>(decoded)->toIdentifier(globalObject).impl();
        if (StringView { uncheckedDowncast<JSString>(declaredNames)->tryGetValueImpl() }.find(makeString('"', StringView { name.get() }, '"')) != notFound)
            return nullptr;
    }
    const uint8_t* valueStart = skipJSONColon(afterName);
    if (!valueStart)
        return nullptr;
    JSValue value = JSValue::decode(parseJSONValueAt(instance, vm, globalObject, skipJSONWhitespaceCharacters(valueStart)));
    auto* end = std::bit_cast<const uint8_t*>(instance->jsonCursor);
    if (!end)
        return nullptr;
    addExtraProperty(instance, vm, globalObject, extra, name.get(), value, mark);
    return instance->jsonParserGaveUp ? nullptr : end;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONExtraMembersAt, EncodedJSValue, (Instance* instance, uint64_t cursorBits, EncodedJSValue encodedExtra, JSCell* declaredNames, uint64_t markBits))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* cursor = std::bit_cast<const uint8_t*>(cursorBits);
    auto* mark = std::bit_cast<const uint16_t*>(markBits);
    instance->jsonCursor = 0;
    if (*cursor != '"') [[unlikely]] {
        instance->jsonParserGaveUp = 1;
        return encodedExtra;
    }
    JSValue given = JSValue::decode(encodedExtra);
    JSObject* extra = given.isCell() ? asObject(given) : createExtraProperties(instance, vm, globalObject, declaredNames);
    auto& orders = *instance->jsonKeyOrders;
    Structure* structure = extra->structure();
    EncodedJSValue values[maxExtraMembersAddedAtOnce];
    PropertyOffset offsets[maxExtraMembersAddedAtOnce];
    unsigned count = 0;
    auto addParsedMembers = [&] ALWAYS_INLINE_LAMBDA {
        if (!count)
            return;
        for (unsigned i = 0; i < count; ++i)
            extra->putDirectOffset(vm, offsets[i], JSValue::decode(values[i]));
        extra->setStructure(vm, structure);
        recordExtraProperty(instance, mark, numberOfPropertiesOf(structure) - 1);
        count = 0;
    };
    const uint8_t* end = nullptr;
    JSONKnownExtraMember* previous = nullptr;
    for (bool isKnownToBeExtra = true;; isKnownToBeExtra = false) {
        const uint8_t* name = cursor + 1;
        const uint8_t* valueStart = nullptr;
        Structure* next;
        PropertyOffset offset = invalidOffset;
        JSONKnownExtraMember* known = previous ? orders.members.mutableSpan().data() + previous->likelyNext : nullptr;
        if (!known || known->before != structure->id() || !known->isAt(name)) {
            known = findKnownExtraMember(orders, structure->id(), name);
            if (known && previous)
                previous->likelyNext = known - orders.members.span().data();
        }
        previous = known;
        if (known) [[likely]] {
            next = known->after;
            offset = known->offset;
            valueStart = name + known->size;
        } else {
            if (!isKnownToBeExtra) {
                addParsedMembers();
                instance->jsonCursor = std::bit_cast<uint64_t>(end);
                return JSValue::encode(extra);
            }
            const uint8_t* afterName = nullptr;
            next = findTransitionForName(instance, vm, structure, name, afterName);
            if (next && next->outOfLineCapacity() != structure->outOfLineCapacity())
                next = nullptr;
            if (!next) {
                addParsedMembers();
                end = parseExtraMemberWithNewName(instance, vm, globalObject, extra, declaredNames, name, mark);
                if (!end)
                    break;
                structure = extra->structure();
            } else {
                offset = next->transitionOffset();
                valueStart = skipJSONColon(afterName);
                if (!valueStart)
                    break;
            }
        }
        if (next) {
            if (*valueStart <= ' ') [[unlikely]]
                valueStart = skipJSONWhitespaceCharacters(valueStart);
            JSValue value;
            end = tryParseSmallJSONInteger(valueStart, value);
            if (!end) {
                auto scalar = tryParseJSONScalarAt(instance, vm, valueStart);
                value = scalar ? *scalar : JSValue::decode(parseJSONValueAt(instance, vm, globalObject, valueStart));
                end = std::bit_cast<const uint8_t*>(instance->jsonCursor);
                if (!end) [[unlikely]]
                    break;
            }
            values[count] = JSValue::encode(value);
            offsets[count] = offset;
            structure = next;
            if (++count == maxExtraMembersAddedAtOnce) [[unlikely]]
                addParsedMembers();
        }
        cursor = end + 1;
        if (*end != ',' || (*cursor != '"' && *(cursor = skipJSONWhitespaceCharacters(cursor)) != '"')) {
            addParsedMembers();
            instance->jsonCursor = std::bit_cast<uint64_t>(end);
            return JSValue::encode(extra);
        }
    }
    instance->jsonCursor = 0;
    instance->jsonParserGaveUp = 1;
    return JSValue::encode(extra);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONAddExtraMember, EncodedJSValue, (Instance* instance, EncodedJSValue encodedExtra, JSCell* nameCell, EncodedJSValue encodedValue, uint64_t markBits))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue given = JSValue::decode(encodedExtra);
    JSObject* extra = given.isCell() ? asObject(given) : constructEmptyObject(vm, globalObject->nullPrototypeObjectStructure());
    addExtraProperty(instance, vm, globalObject, extra, uncheckedDowncast<JSString>(nameCell)->toIdentifier(globalObject).impl(), JSValue::decode(encodedValue), std::bit_cast<const uint16_t*>(markBits));
    return JSValue::encode(extra);
}

static NEVER_INLINE void learnKeyOrder(Instance* instance, CallFrame* callFrame, Typed::Object* object, JSObject* extra, JSCell* declaredNames, std::span<const uint16_t> recorded, uint64_t hash)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    Typed::State& state = object->state();
    const Typed::LayoutData& layout = object->layout();
    instance->jsonParserGaveUp = 1;
    Vector<uint16_t, 16> fieldOfMember;
    const StringImpl* names = uncheckedDowncast<JSString>(declaredNames)->tryGetValueImpl();
    if (!names || !names->is8Bit())
        return;
    for (auto rest = names->span8(); !rest.empty();) {
        size_t length = find(rest.subspan(1), '"');
        RELEASE_ASSERT(rest[0] == '"' && length != notFound);
        const Typed::FieldData* field = state.field(vm, layout, Identifier::fromString(vm, rest.subspan(1, length)).impl());
        if (!field)
            return;
        fieldOfMember.append(field - layout.fields());
        rest = rest.subspan(length + 2);
    }
    Vector<uint16_t, 16> keys;
    for (uint16_t entry : recorded) {
        if (!(entry & recordsExtraProperties)) {
            keys.append(fieldOfMember[entry]);
            continue;
        }
        unsigned key = layout.numberOfFields + (entry & ~recordsExtraProperties);
        if (key >= UINT16_MAX)
            return;
        keys.append(key);
    }
    auto order = object->keyOrderAtBirth(vm, keys.span(), extra);
    if (!order)
        return;
    instance->jsonParserGaveUp = 0;
    if (extra && extra->structure()->outOfLineCapacity())
        widenExtraProperties(instance, vm, declaredNames, numberOfPropertiesOf(extra->structure()));
    StructureID before = object->structureID();
    object->setKeyOrderAtBirth(vm, *order, extra);
    if (state.isCounting() || (extra && extra->structure()->isDictionary()))
        return;
    auto& orders = instance->jsonKeyOrders->known;
    if (orders.size() >= maxNumberOfKnownKeyOrders)
        orders.clear();
    auto& known = *orders.ensure(hash, [] { return makeUnique<JSONKnownKeyOrder>(); }).iterator->value;
    known.declaredNames = declaredNames;
    known.object = before;
    known.extraProperties = extra ? extra->structureID() : StructureID();
    known.orderAndCount = order->orderAndCount;
    known.isUsedSinceFullCollection = true;
    if (extra) {
        known.withExtraProperties.set(vm, order->withExtraProperties);
        known.ofExtraProperties.set(vm, extra->structure());
    } else {
        known.withExtraProperties.clear();
        known.ofExtraProperties.clear();
    }
    known.recorded = Vector<uint16_t, 16>(recorded);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONApplyKeyOrder, void, (Instance* instance, JSCell* cell, EncodedJSValue encodedExtra, JSCell* declaredNames, uint64_t markBits))
{
    countOperationNamed(instance, __func__);
    auto* mark = std::bit_cast<uint16_t*>(markBits);
    std::span<const uint16_t> recorded { mark, instance->jsonKeyOrderTop };
    instance->jsonKeyOrderTop = mark;
    if (instance->jsonParserGaveUp) [[unlikely]]
        return;
    auto* object = uncheckedDowncast<Typed::Object>(cell);
    JSValue given = JSValue::decode(encodedExtra);
    JSObject* extra = given.isCell() ? asObject(given) : nullptr;
    StructureID ofExtraProperties = extra ? extra->structureID() : StructureID();
    uint64_t hash = std::bit_cast<uintptr_t>(declaredNames) ^ static_cast<uint64_t>(ofExtraProperties.bits()) << 32;
    for (uint16_t entry : recorded)
        hash = (hash ^ entry) * 0x9E3779B97F4A7C15ULL;
    hash = std::max<uint64_t>(hash, 1) - (hash == std::numeric_limits<uint64_t>::max());
    auto& orders = instance->jsonKeyOrders->known;
    if (auto found = orders.find(hash); found != orders.end()) [[likely]] {
        auto& known = *found->value;
        if (known.declaredNames == declaredNames && known.object == object->structureID() && known.extraProperties == ofExtraProperties && equalSpans(known.recorded.span(), recorded) && !object->hasFlagsInOrderWord()) [[likely]] {
            known.isUsedSinceFullCollection = true;
            object->setKeyOrderAtBirth(*instance->vm, { known.orderAndCount, known.withExtraProperties.get() }, extra);
            return;
        }
    }
    learnKeyOrder(instance, DECLARE_CALL_FRAME(*instance->vm), object, extra, declaredNames, recorded, hash);
}

static constexpr unsigned maxShapedMembers = 48;
static_assert(maxShapedMembers <= JSFinalObject::maxInlineCapacity);
static constexpr unsigned maxStructuresPerShape = 16;
static constexpr size_t maxLearnedNameSize = 50;
static constexpr unsigned maxDigitsOfSmallInteger = 9;
static constexpr size_t inlineElementCapacity = 32;
static constexpr unsigned maxShapesPerTable = 256;
static constexpr unsigned maxAlternativesPerShape = 16;
static constexpr size_t maxLengthParsedWithShapes = 1 * MB;
static constexpr unsigned maxRefusalsBeforeForgetting = 256;
static constexpr unsigned numberOfRememberedBeginnings = 1024;
static constexpr uint16_t textsBeforeFirstAttempt = 16;
static constexpr unsigned maxKindsAmongTextsThatGoBy = 8;
static constexpr uint8_t creditOfNewBeginning = 8;
static constexpr uint8_t creditOnTrial = 4;
static constexpr uint8_t maxCreditOfBeginning = 32;
static constexpr uint8_t creditLostByPoorParse = 2;
static constexpr unsigned textsSkippedAtFirst = 64;
static constexpr unsigned maxDoublingsOfTextsSkipped = 9;
static constexpr size_t bytesThatPayForCostlyStep = 64;
static constexpr size_t minLengthOfLongString = 64;
static constexpr unsigned lengthOfLongerBeginning = 48;
static constexpr uint8_t maxTimesSetAside = 127;
static constexpr uint16_t maxKindsToldApart = 64;

struct JSONShapeMember {
    JSONShapeMember(VM& vm, std::span<const uint8_t> quotedName, unsigned shape)
        : size(quotedName.size() + 1)
        , shape(shape)
        , name(Identifier::fromString(vm, byteCast<Latin1Character>(quotedName.subspan(1, quotedName.size() - 2))))
    {
        RELEASE_ASSERT(size <= jsonSourcePadding);
        auto byteAt = [&](size_t index) -> uint64_t {
            return index < quotedName.size() ? quotedName[index] : ':';
        };
        for (size_t i = 0; i < std::min<size_t>(size, 8); ++i) {
            firstBytes |= byteAt(i) << (8 * i);
            firstBytesMask |= 0xffULL << (8 * i);
        }
        for (size_t i = 0; size > 8 && i < 8; ++i)
            lastBytes |= byteAt(size - 8 + i) << (8 * i);
    }

    std::span<const Latin1Character> characters() const { return name.impl()->span8(); }

    uint64_t firstBytes { 0 };
    uint64_t firstBytesMask { 0 };
    uint64_t lastBytes { 0 };
    unsigned size;
    unsigned shape;
    Identifier name;
};

struct JSONShapeStructure {
    uint64_t membersInDeclaredOrder { 0 };
    Vector<uint8_t, 0, CrashOnOverflow, 1> order;
    Strong<Structure> structure;
};

struct JSONShape {
    enum class Kind : uint8_t { Anything, Object, Array, Dictionary, OneOf };
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(JSONShape);
    Kind kind { Kind::Anything };
    bool isLearned { false };
    unsigned element { 0 };
    uint64_t membersOfLastStructure { 0 };
    Structure* lastStructure { nullptr };
    Vector<JSONShapeMember, 0, CrashOnOverflow, 2> members;
    Vector<unsigned, 0, CrashOnOverflow, 2> alternatives;
    Vector<JSONShapeStructure, 0, CrashOnOverflow, 1> structures;
};

struct JSONShapeTable {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(JSONShapeTable);
    RefPtr<StringImpl> text;
    Vector<std::unique_ptr<JSONShape>> shapes;
    unsigned root { 0 };
    unsigned anything { 0 };
    unsigned refusals { 0 };
};

struct JSONBeginning {
    uint8_t credit { 0 };
    uint8_t timesSetAside : 7 { 0 };
    bool isToldApartByMore : 1 { false };
    uint16_t textsToSkip { 0 };
    unsigned root { 0 };
};

struct JSONShapeTables {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(JSONShapeTables);
    const StringImpl* lastText { nullptr };
    JSONShapeTable* lastTable { nullptr };
    std::unique_ptr<JSONShapeTable> learned;
    std::array<JSONBeginning, numberOfRememberedBeginnings> beginnings { };
    bool nextTextWasRefusedAsBytes { false };
    UncheckedKeyHashMap<const StringImpl*, std::unique_ptr<JSONShapeTable>> tables;
};

static NEVER_INLINE JSONShapeTables* makeJSONShapeTables(Instance* instance)
{
    auto* tables = new JSONShapeTables;
    instance->jsonShapeTables = tables;
    return tables;
}

static NEVER_INLINE JSONShapeTable& makeLearnedJSONShapeTable(JSONShapeTables& tables)
{
    tables.learned = makeUnique<JSONShapeTable>();
    tables.learned->shapes.append(makeUnique<JSONShape>());
    return *tables.learned;
}

static ALWAYS_INLINE uint64_t upToFirstColon(uint64_t bytes)
{
    uint64_t zeroWhereColon = bytes ^ 0x3A3A3A3A3A3A3A3AULL;
    uint64_t marks = (zeroWhereColon - 0x0101010101010101ULL) & ~zeroWhereColon & 0x8080808080808080ULL;
    if (!marks)
        return bytes;
    return bytes & (marks ^ (marks - 1));
}

static ALWAYS_INLINE uint64_t beginningOf(StringView text)
{
    if (text.is8Bit() && text.length() >= sizeof(uint64_t)) [[likely]]
        return upToFirstColon(WTF::unalignedLoad<uint64_t>(text.span8().data()));
    uint64_t beginning = 0;
    unsigned length = std::min<unsigned>(text.length(), sizeof(uint64_t));
    for (unsigned i = 0; i < length; ++i)
        beginning |= static_cast<uint64_t>(static_cast<uint8_t>(text[i])) << (8 * i);
    return upToFirstColon(beginning);
}

template<typename CharacterType>
static uint64_t hashOfFirstTwoStrings(std::span<const CharacterType> text)
{
    uint64_t mixed = 0;
    unsigned quotes = 0;
    for (CharacterType character : text.first(std::min<size_t>(text.size(), lengthOfLongerBeginning))) {
        if (character == '"') {
            if (++quotes == 4)
                break;
        }
        if (quotes & 1)
            mixed = (mixed ^ static_cast<uint8_t>(character)) * 0x100000001B3ULL;
    }
    return mixed ^ mixed >> 29;
}

static uint64_t hashUpToFourthQuote(std::span<const Latin1Character> text)
{
    constexpr uint64_t lowSevenBits = 0x7F7F7F7F7F7F7F7FULL;
    uint64_t mixed = 0;
    unsigned quotesLeft = 4;
    size_t length = std::min<size_t>(text.size(), lengthOfLongerBeginning);
    for (size_t at = 0; at < length; at += sizeof(uint64_t)) {
        uint64_t word = 0;
        if (at + sizeof(uint64_t) <= length) [[likely]]
            word = WTF::unalignedLoad<uint64_t>(text.data() + at);
        else
            memcpy(&word, text.data() + at, length - at);
        uint64_t zeroWhereQuote = word ^ 0x2222222222222222ULL;
        uint64_t marks = ~(((zeroWhereQuote & lowSevenBits) + lowSevenBits) | zeroWhereQuote | lowSevenBits);
        unsigned quotes = std::popcount(marks);
        if (quotes >= quotesLeft) {
            for (; quotesLeft > 1; --quotesLeft)
                marks &= marks - 1;
            word &= marks ^ (marks - 1);
            length = 0;
        }
        quotesLeft -= std::min(quotes, quotesLeft);
        mixed = (mixed ^ word) * 0x9E3779B97F4A7C15ULL;
        mixed ^= mixed >> 29;
    }
    return mixed;
}

static NEVER_INLINE uint64_t longerBeginningOf(StringView text)
{
    return text.is8Bit() ? hashUpToFourthQuote(text.span8()) : hashOfFirstTwoStrings(text.span16());
}

static NEVER_INLINE void noteKindWhileWaiting(JSONBeginning& record, StringView text)
{
    record.root |= 1u << (longerBeginningOf(text) & 31);
}

static ALWAYS_INLINE bool holdsSeveralKinds(const JSONBeginning& record)
{
    unsigned kinds = std::popcount(record.root);
    return kinds > 1 && kinds <= maxKindsAmongTextsThatGoBy;
}

static ALWAYS_INLINE JSONBeginning& recordOfBeginning(JSONShapeTables& tables, uint64_t beginning)
{
    return tables.beginnings[(beginning * 0x9E3779B97F4A7C15ULL) >> (64 - WTF::fastLog2(numberOfRememberedBeginnings))];
}

static NEVER_INLINE void forgetLearnedJSONShapes(Instance* instance, JSONShapeTables& tables)
{
    countOperationNamed(instance, "jsonLearnedShapesForgotten");
    tables.learned = nullptr;
    for (auto& record : tables.beginnings)
        record.root = 0;
}

static NEVER_INLINE JSONShapeTable* makeJSONShapeTable(Instance* instance, VM& vm, const StringImpl* text)
{
    if (!text->is8Bit())
        return nullptr;
    if (!instance->jsonShapeTables)
        makeJSONShapeTables(instance);
    auto table = makeUnique<JSONShapeTable>();
    table->text = const_cast<StringImpl*>(text);
    auto characters = text->span8();
    size_t at = 0;
    auto readNumber = [&] {
        unsigned number = 0;
        while (at < characters.size() && isASCIIDigit(characters[at]))
            number = number * 10 + (characters[at++] - '0');
        return number;
    };
    while (at < characters.size()) {
        auto made = makeUnique<JSONShape>();
        JSONShape& shape = *made;
        switch (characters[at++]) {
        case '*':
            break;
        case '{':
            shape.kind = JSONShape::Kind::Object;
            while (at < characters.size() && characters[at] == '"') {
                size_t start = at++;
                while (at < characters.size() && characters[at] != '"')
                    ++at;
                ++at;
                RELEASE_ASSERT(at <= characters.size());
                auto quotedName = byteCast<uint8_t>(characters.subspan(start, at - start));
                shape.members.append(JSONShapeMember { vm, quotedName, readNumber() });
                if (!shape.members.last().name.impl()->is8Bit())
                    return nullptr;
            }
            RELEASE_ASSERT(at < characters.size() && characters[at] == '}' && shape.members.size() <= maxShapedMembers);
            ++at;
            break;
        case '[':
        case '<':
            shape.kind = characters[at - 1] == '[' ? JSONShape::Kind::Array : JSONShape::Kind::Dictionary;
            shape.element = readNumber();
            ++at;
            break;
        case '(':
            shape.kind = JSONShape::Kind::OneOf;
            do
                shape.alternatives.append(readNumber());
            while (at < characters.size() && characters[at++] == ',');
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        table->shapes.append(WTF::move(made));
        RELEASE_ASSERT(at == characters.size() || characters[at] == ';');
        ++at;
    }
    table->anything = table->shapes.size();
    table->shapes.append(makeUnique<JSONShape>());
    for (auto& shape : table->shapes) {
        RELEASE_ASSERT(shape->element < table->shapes.size());
        for (auto& member : shape->members)
            RELEASE_ASSERT(member.shape < table->shapes.size());
        for (unsigned alternative : shape->alternatives)
            RELEASE_ASSERT(alternative < table->shapes.size());
    }
    return instance->jsonShapeTables->tables.add(text, WTF::move(table)).iterator->value.get();
}

static ALWAYS_INLINE JSONShapeTable* jsonShapeTableFor(Instance* instance, VM& vm, const StringImpl* text)
{
    JSONShapeTables* tables = instance->jsonShapeTables;
    if (tables && tables->lastText == text) [[likely]]
        return tables->lastTable;
    JSONShapeTable* table = tables ? tables->tables.get(text) : nullptr;
    if (!table) [[unlikely]] {
        table = makeJSONShapeTable(instance, vm, text);
        if (!table)
            return nullptr;
    }
    instance->jsonShapeTables->lastText = text;
    instance->jsonShapeTables->lastTable = table;
    return table;
}

class ShapedJSONParser {
public:
    ShapedJSONParser(Instance* instance, VM& vm, JSGlobalObject* globalObject, JSONShapeTable& table)
        : m_instance(instance)
        , m_vm(vm)
        , m_globalObject(globalObject)
        , m_table(table)
    {
    }

    bool wasWorthwhile(size_t length) const
    {
        return m_costlySteps * bytesThatPayForCostlyStep <= length && m_bytesOfLongStrings * 2 <= length;
    }

    JSValue parse(const uint8_t* start, unsigned& root)
    {
        m_firstValue = skipWhitespace(start);
        Parsed parsed = parseValue(m_firstValue, root);
        if (!parsed.value)
            return { };
        if (skipWhitespace(parsed.end) != m_instance->jsonSourceEnd || m_instance->jsonParserGaveUp)
            return { };
        return JSValue::decode(parsed.value);
    }

private:
    using Cursor = const uint8_t*;

    struct Parsed {
        EncodedJSValue value;
        Cursor end;
    };

    static constexpr Parsed noAnswer { 0, nullptr };

    static ALWAYS_INLINE Cursor skipWhitespace(Cursor cursor)
    {
        if (*cursor <= ' ') [[unlikely]]
            return skipJSONWhitespaceCharacters(cursor);
        return cursor;
    }

    static ALWAYS_INLINE bool isAtNameAndColon(Cursor cursor, const JSONShapeMember& member)
    {
        if ((WTF::unalignedLoad<uint64_t>(cursor) & member.firstBytesMask) != member.firstBytes)
            return false;
        unsigned size = member.size;
        if (size <= 8)
            return true;
        if (WTF::unalignedLoad<uint64_t>(cursor + size - 8) != member.lastBytes)
            return false;
        return size <= 16 || !memcmp(cursor + 8, member.characters().data() + 7, size - 16);
    }

    static ALWAYS_INLINE bool isAtName(Cursor cursor, const JSONShapeMember& member)
    {
        unsigned size = member.size;
        uint64_t differenceAtStart = WTF::unalignedLoad<uint64_t>(cursor) ^ member.firstBytes;
        if (size <= 8)
            return !(differenceAtStart & member.firstBytesMask >> 8);
        constexpr uint64_t allButTheColon = 0x00FFFFFFFFFFFFFFULL;
        if (differenceAtStart || ((WTF::unalignedLoad<uint64_t>(cursor + size - 8) ^ member.lastBytes) & allButTheColon))
            return false;
        return size <= 16 || !memcmp(cursor + 8, member.characters().data() + 7, size - 16);
    }

    ALWAYS_INLINE Parsed parseNumberAt(Cursor cursor)
    {
        uint32_t number = *cursor - '0';
        if (number - 1 < 9) [[likely]] {
            Cursor at = cursor + 1;
            for (uint32_t digit = *at - '0'; digit <= 9; digit = *++at - '0')
                number = number * 10 + digit;
            if (static_cast<size_t>(at - cursor) <= maxDigitsOfSmallInteger && *at != '.' && (*at | 0x20) != 'e') [[likely]]
                return { JSValue::encode(jsNumber(static_cast<int32_t>(number))), at };
        }
        void* end = parseNumber(m_instance, cursor);
        if (!end) [[unlikely]]
            return noAnswer;
        return { JSValue::encode(jsNumber(m_instance->jsonNumber)), static_cast<Cursor>(end) };
    }

    NEVER_INLINE Parsed parseStringAt(Cursor quote)
    {
        if (auto* entry = m_vm.jsonCache().findString(std::span<const Latin1Character> { byteCast<Latin1Character>(quote), jsonSourcePadding })) [[likely]] {
            if (m_instance->jsonSourceEncoding == JSONSourceEncoding::Latin1 || isShortTextLatin1AfterDecoding(m_instance, quote, entry->textLength)) [[likely]]
                return { JSValue::encode(entry->string), quote + entry->textLength };
        }
        JSCell* string = makeUncachedString(m_instance, m_vm, quote + 1);
        if (!m_instance->jsonCursor) [[unlikely]]
            return noAnswer;
        Cursor end = std::bit_cast<Cursor>(m_instance->jsonCursor);
        if (static_cast<size_t>(end - quote) >= minLengthOfLongString)
            m_bytesOfLongStrings += end - quote;
        return { JSValue::encode(string), end };
    }

    ALWAYS_INLINE JSONShape& shapeAt(unsigned index) { return *m_table.shapes[index]; }

    ALWAYS_INLINE Parsed parseCommonValue(Cursor cursor, unsigned& shape)
    {
        uint8_t first = *cursor;
        if (first == '"')
            return parseStringAt(cursor);
        if (static_cast<uint8_t>(first - '0') <= 9)
            return parseNumberAt(cursor);
        if (first == '{')
            return parseObject(cursor, shape);
        return parseValue(cursor, shape);
    }

    NEVER_INLINE Parsed parseValue(Cursor cursor, unsigned& shape)
    {
        cursor = skipWhitespace(cursor);
        switch (*cursor) {
        case '"':
            return parseStringAt(cursor);
        case '{':
            return parseObject(cursor, shape);
        case '[':
            return parseArray(cursor, shape);
        case 't':
            if (WTF::unalignedLoad<uint32_t>(cursor) != ('t' | 'r' << 8 | 'u' << 16 | 'e' << 24))
                return noAnswer;
            return { JSValue::encode(jsBoolean(true)), cursor + 4 };
        case 'f':
            if (WTF::unalignedLoad<uint32_t>(cursor + 1) != ('a' | 'l' << 8 | 's' << 16 | 'e' << 24))
                return noAnswer;
            return { JSValue::encode(jsBoolean(false)), cursor + 5 };
        case 'n':
            if (WTF::unalignedLoad<uint32_t>(cursor) != ('n' | 'u' << 8 | 'l' << 16 | 'l' << 24))
                return noAnswer;
            return { JSValue::encode(jsNull()), cursor + 4 };
        default:
            return parseNumberAt(cursor);
        }
    }

    NEVER_INLINE Parsed parseWithoutShape(Cursor start)
    {
        countOperationNamed(m_instance, "jsonValueParsedWithoutShape");
        ++m_costlySteps;
        if (start == m_firstValue)
            return noAnswer;
        EncodedJSValue value = parseJSONValueAt(m_instance, m_vm, m_globalObject, start);
        if (!m_instance->jsonCursor)
            return noAnswer;
        return { value, std::bit_cast<Cursor>(m_instance->jsonCursor) };
    }

    unsigned learnShape(JSONShape::Kind kind)
    {
        countOperationNamed(m_instance, "jsonShapeLearned");
        auto shape = makeUnique<JSONShape>();
        shape->kind = kind;
        shape->isLearned = true;
        shape->element = m_table.anything;
        m_table.shapes.append(WTF::move(shape));
        return m_table.shapes.size() - 1;
    }

    bool canLearnShapes(unsigned count)
    {
        if (m_table.shapes.size() + count <= maxShapesPerTable)
            return true;
        m_table.refusals = maxRefusalsBeforeForgetting;
        return false;
    }

    JSONShape* learnAlternative(unsigned& place, JSONShape::Kind kind)
    {
        JSONShape& current = shapeAt(place);
        if (current.kind == JSONShape::Kind::Anything) {
            if (!canLearnShapes(1))
                return nullptr;
            place = learnShape(kind);
            return &shapeAt(place);
        }
        if (current.kind != JSONShape::Kind::OneOf) {
            if (!canLearnShapes(2))
                return nullptr;
            unsigned former = place;
            place = learnShape(JSONShape::Kind::OneOf);
            shapeAt(place).alternatives.append(former);
        } else if (current.alternatives.size() >= maxAlternativesPerShape || !canLearnShapes(1))
            return nullptr;
        unsigned learned = learnShape(kind);
        shapeAt(place).alternatives.append(learned);
        return &shapeAt(learned);
    }

    static bool holdsMembers(const JSONShape& shape) { return shape.kind == JSONShape::Kind::Object || shape.kind == JSONShape::Kind::Dictionary; }

    NEVER_INLINE JSONShape* objectShapeFor(unsigned& place, Cursor firstName)
    {
        JSONShape& current = shapeAt(place);
        if (current.kind == JSONShape::Kind::Dictionary)
            return &current;
        JSONShape* fallback = nullptr;
        for (unsigned alternative : current.alternatives) {
            JSONShape& candidate = shapeAt(alternative);
            if (candidate.kind == JSONShape::Kind::Object && !candidate.members.isEmpty() && isAtNameAndColon(firstName, candidate.members[0]))
                return &candidate;
        }
        ++m_costlySteps;
        for (unsigned alternative : current.alternatives) {
            JSONShape& candidate = shapeAt(alternative);
            if (!holdsMembers(candidate))
                continue;
            if (candidate.kind == JSONShape::Kind::Dictionary)
                return &candidate;
            if (!fallback && !candidate.isLearned)
                fallback = &candidate;
            for (auto& member : candidate.members) {
                if (isAtName(firstName, member))
                    return &candidate;
            }
        }
        if (JSONShape* learned = learnAlternative(place, JSONShape::Kind::Object))
            return learned;
        return fallback;
    }

    NEVER_INLINE JSONShape* arrayShapeFor(unsigned& place)
    {
        for (unsigned alternative : shapeAt(place).alternatives) {
            if (shapeAt(alternative).kind == JSONShape::Kind::Array)
                return &shapeAt(alternative);
        }
        return learnAlternative(place, JSONShape::Kind::Array);
    }

    struct FoundMember {
        unsigned at;
        Cursor value;
    };

    NEVER_INLINE std::optional<FoundMember> findMemberOutOfTurn(JSONShape& shape, Cursor cursor, uint64_t present)
    {
        if (*cursor != '"')
            return std::nullopt;
        unsigned size = shape.members.size();
        unsigned at = 0;
        for (; at < size; ++at) {
            if (isAtName(cursor, shape.members[at]))
                break;
        }
        if (at == size) {
            bool isAnotherKindOfObject = !present && shape.isLearned && size;
            if (isAnotherKindOfObject || !learnNameAt(shape, cursor))
                return std::nullopt;
        }
        if (present >> at & 1)
            return std::nullopt;
        Cursor colon = skipWhitespace(cursor + shape.members[at].size - 1);
        if (*colon != ':')
            return std::nullopt;
        return FoundMember { at, colon + 1 };
    }

    bool learnNameAt(JSONShape& shape, Cursor quote)
    {
        if (shape.members.size() >= maxShapedMembers) {
            if (shape.isLearned)
                shape.kind = JSONShape::Kind::Dictionary;
            return false;
        }
        Cursor end = findEndOfPlainJSONCharacters(quote + 1);
        std::span<const uint8_t> quotedName { quote, end + 1 };
        if (*end != '"' || *skipWhitespace(end + 1) != ':' || quotedName.size() > maxLearnedNameSize || !charactersAreAllASCII(quotedName))
            return false;
        JSONShapeMember member { m_vm, quotedName, m_table.anything };
        if (parseIndex(member.name) || member.name == m_vm.propertyNames->underscoreProto || !member.name.impl()->is8Bit())
            return false;
        shape.members.append(WTF::move(member));
        return true;
    }

    Parsed parseObject(Cursor brace, unsigned& place)
    {
        if (!m_vm.isSafeToRecurse()) [[unlikely]]
            return noAnswer;
        Cursor cursor = skipWhitespace(brace + 1);
        if (*cursor == '}') [[unlikely]]
            return { JSValue::encode(constructEmptyObject(m_globalObject)), cursor + 1 };
        EncodedJSValue values[JSFinalObject::maxInlineCapacity];
        for (unsigned i = 0; i < JSFinalObject::defaultInlineCapacity; ++i)
            values[i] = JSValue::encode(JSValue());
        JSONShape* shape = &shapeAt(place);
        if (shape->kind != JSONShape::Kind::Object) [[unlikely]] {
            shape = objectShapeFor(place, cursor);
            if (!shape)
                return parseWithoutShape(brace);
            if (shape->kind == JSONShape::Kind::Dictionary)
                return parseRemainingMembers(constructEmptyObject(m_globalObject), cursor, nullptr, shape->element);
        }
        JSONShapeMember* members = shape->members.mutableSpan().data();
        unsigned size = shape->members.size();
        unsigned count = 0;
        unsigned at = 0;
        uint64_t present = 0;
        for (;;) {
            while (at < size && !isAtNameAndColon(cursor, members[at]))
                ++at;
            if (at == size) [[unlikely]]
                return parseMembersInAnyOrder(shape, place, cursor, values, present, count);
            Parsed parsed = parseCommonValue(cursor + members[at].size, members[at].shape);
            if (!parsed.value) [[unlikely]]
                return noAnswer;
            members = shape->members.mutableSpan().data();
            values[count++] = parsed.value;
            present |= 1ULL << at++;
            cursor = parsed.end;
            if (*cursor != ',' && *cursor != '}') [[unlikely]]
                cursor = skipJSONWhitespaceCharacters(cursor);
            if (*cursor == ',') {
                cursor = skipWhitespace(cursor + 1);
                continue;
            }
            if (*cursor != '}') [[unlikely]]
                return noAnswer;
            ++cursor;
            break;
        }
        if (at > 4 * count + 8) [[unlikely]]
            ++m_costlySteps;
        if (present != shape->membersOfLastStructure) [[unlikely]]
            return makeObject(*shape, values, present, nullptr, count, cursor);
        return { JSValue::encode(makeObjectWithStructure(shape->lastStructure, values, count)), cursor };
    }

    ALWAYS_INLINE JSObject* makeObjectWithStructure(Structure* structure, EncodedJSValue* values, unsigned count)
    {
        unsigned inlineCapacity = structure->inlineCapacity();
        for (unsigned i = std::max(count, JSFinalObject::defaultInlineCapacity); i < inlineCapacity; ++i)
            values[i] = JSValue::encode(JSValue());
        return JSFinalObject::createWithButterflyCopyingInlineStorage(m_vm, structure, nullptr, reinterpret_cast<const WriteBarrierBase<Unknown>*>(values));
    }

    static void listInDeclaredOrder(uint64_t present, uint8_t* order)
    {
        for (unsigned count = 0; present; present &= present - 1)
            order[count++] = WTF::ctz(present);
    }

    NEVER_INLINE Parsed makeObject(JSONShape& shape, EncodedJSValue* values, uint64_t membersInDeclaredOrder, const uint8_t* order, unsigned count, Cursor end)
    {
        if (membersInDeclaredOrder) {
            for (auto& known : shape.structures) {
                if (known.membersInDeclaredOrder == membersInDeclaredOrder) {
                    shape.membersOfLastStructure = membersInDeclaredOrder;
                    shape.lastStructure = known.structure.get();
                    return { JSValue::encode(makeObjectWithStructure(shape.lastStructure, values, count)), end };
                }
            }
        }
        uint8_t declaredOrder[maxShapedMembers];
        if (!order) {
            listInDeclaredOrder(membersInDeclaredOrder, declaredOrder);
            order = declaredOrder;
        }
        Structure* structure = structureFor(shape, order, count, membersInDeclaredOrder);
        if (!structure)
            return { JSValue::encode(makeObjectMemberByMember(shape, values, order, count)), end };
        return { JSValue::encode(makeObjectWithStructure(structure, values, count)), end };
    }

    NEVER_INLINE Parsed parseMembersInAnyOrder(JSONShape* shape, unsigned& place, Cursor cursor, EncodedJSValue* values, uint64_t present, unsigned count)
    {
        uint8_t order[maxShapedMembers];
        listInDeclaredOrder(present, order);
        JSONShapeMember* members = shape->members.mutableSpan().data();
        unsigned size = shape->members.size();
        unsigned next = count ? order[count - 1] + 1 : 0;
        bool isInDeclaredOrder = true;
        for (;;) {
            unsigned at = next;
            while (at < size && !isAtNameAndColon(cursor, members[at]))
                ++at;
            Cursor valueStart;
            if (at < size && !(present >> at & 1)) [[likely]]
                valueStart = cursor + members[at].size;
            else {
                auto found = findMemberOutOfTurn(*shape, cursor, present);
                if (!found && !count && shape->isLearned && *cursor == '"') {
                    if (JSONShape* other = learnAlternative(place, JSONShape::Kind::Object)) {
                        shape = other;
                        found = findMemberOutOfTurn(*shape, cursor, present);
                    }
                }
                if (!found)
                    return parseObjectMemberByMember(*shape, cursor, values, order, count);
                members = shape->members.mutableSpan().data();
                size = shape->members.size();
                at = found->at;
                valueStart = found->value;
                isInDeclaredOrder &= at >= next;
            }
            Parsed parsed = parseCommonValue(valueStart, members[at].shape);
            if (!parsed.value) [[unlikely]]
                return noAnswer;
            members = shape->members.mutableSpan().data();
            values[count] = parsed.value;
            order[count++] = at;
            present |= 1ULL << at;
            next = at + 1;
            cursor = skipWhitespace(parsed.end);
            if (*cursor == ',') {
                cursor = skipWhitespace(cursor + 1);
                continue;
            }
            if (*cursor != '}') [[unlikely]]
                return noAnswer;
            if (next > 4 * count + 8) [[unlikely]]
                ++m_costlySteps;
            return makeObject(*shape, values, isInDeclaredOrder ? present : 0, order, count, cursor + 1);
        }
    }

    NEVER_INLINE Structure* structureFor(JSONShape& shape, const uint8_t* order, unsigned count, uint64_t membersInDeclaredOrder)
    {
        Structure* structure = nullptr;
        for (auto& known : shape.structures) {
            if (!membersInDeclaredOrder && known.order.size() == count && !memcmp(known.order.span().data(), order, count)) {
                structure = known.structure.get();
                break;
            }
        }
        if (!structure) {
            countOperationNamed(m_instance, "jsonStructureSoughtForShape");
            structure = makeStructure(shape, order, count);
            if (!structure || shape.structures.size() >= maxStructuresPerShape) {
                ++m_costlySteps;
                return structure;
            }
            JSONShapeStructure known;
            known.membersInDeclaredOrder = membersInDeclaredOrder;
            if (!membersInDeclaredOrder)
                known.order.append(std::span<const uint8_t> { order, count });
            known.structure = Strong<Structure>(m_vm, structure);
            shape.structures.append(WTF::move(known));
        }
        if (membersInDeclaredOrder) {
            shape.membersOfLastStructure = membersInDeclaredOrder;
            shape.lastStructure = structure;
        }
        return structure;
    }

    Structure* makeStructure(JSONShape& shape, const uint8_t* order, unsigned count)
    {
        Structure* structure = m_globalObject->objectStructureForObjectConstructor();
        if (count <= JSFinalObject::defaultInlineCapacity) {
            for (unsigned i = 0; i < count; ++i) {
                PropertyOffset offset = invalidOffset;
                structure = Structure::addPropertyTransition(m_vm, structure, shape.members[order[i]].name, 0, offset);
                if (structure->isDictionary() || offset != static_cast<PropertyOffset>(i))
                    return nullptr;
            }
            return structure;
        }
        Vector<UniquedStringImpl*, JSFinalObject::maxInlineCapacity> names(count);
        for (unsigned i = 0; i < count; ++i)
            names[i] = shape.members[order[i]].name.impl();
        size_t size = MarkedSpace::optimalSizeFor(JSFinalObject::allocationSize(count));
        unsigned inlineCapacity = std::min<unsigned>((size - JSFinalObject::allocationSize(0)) / sizeof(EncodedJSValue), JSFinalObject::maxInlineCapacity);
        Structure* empty = m_globalObject->structureCache().emptyObjectStructureForPrototype(m_globalObject, m_globalObject->objectPrototype(), inlineCapacity);
        structure = m_instance->literalStructure(empty, names.span());
        if (!structure || structure->isDictionary() || structure->inlineCapacity() < count || structure->outOfLineCapacity())
            return nullptr;
        return structure;
    }

    NEVER_INLINE JSObject* makeObjectMemberByMember(JSONShape& shape, const EncodedJSValue* values, const uint8_t* order, unsigned count)
    {
        JSObject* object = constructEmptyObject(m_globalObject);
        for (unsigned i = 0; i < count; ++i)
            object->putDirect(m_vm, shape.members[order[i]].name, JSValue::decode(values[i]));
        return object;
    }

    NEVER_INLINE Parsed parseObjectMemberByMember(JSONShape& shape, Cursor cursor, const EncodedJSValue* values, const uint8_t* order, unsigned count)
    {
        countOperationNamed(m_instance, "jsonObjectParsedMemberByMember");
        unsigned shapeOfOthers = m_table.anything;
        return parseRemainingMembers(makeObjectMemberByMember(shape, values, order, count), cursor, &shape, shapeOfOthers);
    }

    Cursor parseName(Cursor quote, Identifier& name)
    {
        Cursor start = quote + 1;
        Cursor end = findEndOfPlainJSONCharacters(start);
        if (*end == '"' && isLatin1(m_instance, { start, end })) [[likely]] {
            name = Identifier::fromString(m_vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(end) });
            return end + 1;
        }
        m_instance->jsonCursor = 0;
        JSCell* made = makeStringWithEscapes(m_instance, m_vm, start, end);
        if (!m_instance->jsonCursor)
            return nullptr;
        const StringImpl* impl = uncheckedDowncast<JSString>(made)->tryGetValueImpl();
        if (!impl)
            return nullptr;
        name = Identifier::fromString(m_vm, String { const_cast<StringImpl*>(impl) });
        return std::bit_cast<Cursor>(m_instance->jsonCursor);
    }

    Parsed parseRemainingMembers(JSObject* object, Cursor cursor, JSONShape* shape, unsigned& shapeOfOthers)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        for (;;) {
            if (*cursor != '"') [[unlikely]]
                return noAnswer;
            ++m_costlySteps;
            Identifier name;
            cursor = parseName(cursor, name);
            if (!cursor) [[unlikely]]
                return noAnswer;
            cursor = skipWhitespace(cursor);
            if (*cursor != ':') [[unlikely]]
                return noAnswer;
            unsigned* valueShape = &shapeOfOthers;
            if (shape) {
                for (auto& member : shape->members) {
                    if (member.name == name) {
                        valueShape = &member.shape;
                        break;
                    }
                }
            }
            Parsed parsed = parseValue(cursor + 1, *valueShape);
            if (!parsed.value) [[unlikely]]
                return noAnswer;
            if (std::optional<uint32_t> index = parseIndex(name)) {
                object->putDirectIndex(m_globalObject, *index, JSValue::decode(parsed.value));
                if (scope.exception()) [[unlikely]] {
                    scope.clearException();
                    return noAnswer;
                }
            } else
                object->putDirect(m_vm, name, JSValue::decode(parsed.value));
            cursor = skipWhitespace(parsed.end);
            if (*cursor == ',') {
                cursor = skipWhitespace(cursor + 1);
                continue;
            }
            if (*cursor != '}') [[unlikely]]
                return noAnswer;
            return { JSValue::encode(object), cursor + 1 };
        }
    }

    Parsed parseArray(Cursor bracket, unsigned& place)
    {
        if (!m_vm.isSafeToRecurse()) [[unlikely]]
            return noAnswer;
        JSONShape* shape = &shapeAt(place);
        if (shape->kind != JSONShape::Kind::Array) [[unlikely]] {
            shape = arrayShapeFor(place);
            if (!shape)
                return parseWithoutShape(bracket);
        }
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        Cursor cursor = skipWhitespace(bracket + 1);
        JSArray* array = nullptr;
        if (*cursor == ']') {
            ++cursor;
            array = constructEmptyArray(m_globalObject, nullptr);
        } else {
            size_t base = m_elements.size();
            for (;;) {
                Parsed parsed = parseCommonValue(cursor, shape->element);
                if (!parsed.value) [[unlikely]]
                    return noAnswer;
                m_elements.append(JSValue::decode(parsed.value));
                if (m_elements.hasOverflowed()) [[unlikely]]
                    return noAnswer;
                cursor = parsed.end;
                if (*cursor != ',' && *cursor != ']') [[unlikely]]
                    cursor = skipJSONWhitespaceCharacters(cursor);
                if (*cursor == ',') {
                    cursor = skipWhitespace(cursor + 1);
                    continue;
                }
                if (*cursor != ']') [[unlikely]]
                    return noAnswer;
                ++cursor;
                break;
            }
            array = makeArray(m_elements.begin() + base, m_elements.size() - base);
            m_elements.shrink(base);
        }
        if (scope.exception()) [[unlikely]] {
            scope.clearException();
            return noAnswer;
        }
        if (!array) [[unlikely]]
            return noAnswer;
        return { JSValue::encode(array), cursor };
    }

    JSArray* makeArray(const JSValue* values, unsigned length)
    {
        IndexingType indexingType = ArrayWithInt32;
        for (unsigned i = 0; i < length; ++i) {
            if (values[i].isInt32())
                continue;
            if (!values[i].isDouble()) {
                indexingType = ArrayWithContiguous;
                break;
            }
            indexingType = ArrayWithDouble;
        }
        ObjectInitializationScope initializationScope(m_vm);
        JSArray* array = JSArray::tryCreateUninitializedRestricted(initializationScope, m_globalObject->arrayStructureForIndexingTypeDuringAllocation(indexingType), length);
        if (!array) [[unlikely]]
            return nullptr;
        IndexingType shape = array->indexingType() & IndexingShapeMask;
        if (shape == DoubleShape) {
            double* data = array->butterfly()->contiguousDouble().data();
            for (unsigned i = 0; i < length; ++i)
                data[i] = values[i].asNumber();
        } else if (shape == Int32Shape)
            memcpy(std::bit_cast<JSValue*>(array->butterfly()->contiguous().data()), values, length * sizeof(JSValue));
        else if (shape == ContiguousShape) {
            gcSafeMemcpy(std::bit_cast<JSValue*>(array->butterfly()->contiguous().data()), values, length * sizeof(JSValue));
            m_vm.writeBarrier(array);
        } else {
            for (unsigned i = 0; i < length; ++i)
                array->initializeIndex(initializationScope, i, values[i]);
        }
        return array;
    }

    Instance* m_instance;
    VM& m_vm;
    JSGlobalObject* m_globalObject;
    JSONShapeTable& m_table;
    size_t m_costlySteps { 0 };
    size_t m_bytesOfLongStrings { 0 };
    const uint8_t* m_firstValue { nullptr };
    MarkedArgumentBufferWithSize<inlineElementCapacity> m_elements;
};

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONParseWithShape, EncodedJSValue, (Instance* instance, EncodedJSValue encodedText, JSCell* shapes))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    const StringImpl* shapesText = uncheckedDowncast<JSString>(shapes)->tryGetValueImpl();
    if (!JSValue::decode(encodedText).isString() || !shapesText) [[unlikely]]
        return JSValue::encode(jsUndefined());
    JSONShapeTable* table = jsonShapeTableFor(instance, vm, shapesText);
    if (!table) [[unlikely]]
        return JSValue::encode(jsUndefined());
    uint64_t frame = std::bit_cast<uint64_t>(callFrame);
    if (frame < instance->jsonParserFrame) [[unlikely]]
        return JSValue::encode(jsUndefined());
    JSString* string = asString(JSValue::decode(encodedText));
    const StringImpl* impl = string->tryGetValueImpl();
    if (!impl) [[unlikely]] {
        impl = flattenJSONSource(instance, callFrame, string);
        if (!impl)
            return JSValue::encode(jsUndefined());
    }
    if (impl->length() > maxLengthParsedWithShapes) [[unlikely]]
        return JSValue::encode(jsUndefined());
    const uint8_t* start = openJSONString(instance, StringView { impl }, frame);
    JSValue parsed = start ? ShapedJSONParser(instance, vm, globalObject, *table).parse(start, table->root) : JSValue();
    instance->jsonParserFrame = 0;
    if (table->refusals >= maxRefusalsBeforeForgetting) [[unlikely]] {
        instance->jsonShapeTables->lastText = nullptr;
        instance->jsonShapeTables->tables.remove(shapesText);
    }
    return JSValue::encode(parsed ? parsed : jsUndefined());
}

template<bool isGivenAsBytes, typename Open>
static ALWAYS_INLINE JSValue tryParseWithLearnedShapes(JSGlobalObject* globalObject, StringView beginning, size_t length, const Open& open)
{
    Instance* instance = globalObject->aotInstance();
    uint64_t frame = std::bit_cast<uint64_t>(currentStackPointer());
    if (!instance || frame < instance->jsonParserFrame || !length || length > maxLengthParsedWithShapes)
        return { };
    JSONShapeTables* tables = instance->jsonShapeTables;
    if (!tables) [[unlikely]]
        tables = makeJSONShapeTables(instance);
    if (std::exchange(tables->nextTextWasRefusedAsBytes, false) && !isGivenAsBytes) [[unlikely]]
        return { };
    JSONBeginning& shortRecord = recordOfBeginning(*tables, beginningOf(beginning));
    bool isToldApartByMore = shortRecord.isToldApartByMore;
    JSONBeginning& record = isToldApartByMore ? recordOfBeginning(*tables, longerBeginningOf(beginning)) : shortRecord;
    if (!record.credit) [[unlikely]] {
        bool wasNeverTried = !record.timesSetAside;
        if (record.textsToSkip || wasNeverTried) {
            if (wasNeverTried && !isToldApartByMore)
                noteKindWhileWaiting(record, beginning);
            if (record.textsToSkip) {
                if (!--record.textsToSkip && wasNeverTried) {
                    if (!isToldApartByMore && holdsSeveralKinds(record)) {
                        countOperationNamed(instance, "jsonBeginningToldApartByMore");
                        record.isToldApartByMore = true;
                    } else
                        record.credit = creditOfNewBeginning;
                    record.root = 0;
                }
            } else {
                record.textsToSkip = textsBeforeFirstAttempt - 1;
                if (isToldApartByMore && ++shortRecord.textsToSkip > maxKindsToldApart) {
                    countOperationNamed(instance, "jsonBeginningSetAside");
                    shortRecord.isToldApartByMore = false;
                    shortRecord.credit = 0;
                    shortRecord.timesSetAside = 1;
                    shortRecord.textsToSkip = textsSkippedAtFirst;
                    forgetLearnedJSONShapes(instance, *tables);
                }
            }
            tables->nextTextWasRefusedAsBytes = isGivenAsBytes;
            return { };
        }
        record.credit = creditOnTrial;
    }
    countOperationNamed(instance, "jsonParsedWithLearnedShapes");
    VM& vm = globalObject->vm();
    JSONShapeTable& table = tables->learned ? *tables->learned : makeLearnedJSONShapeTable(*tables);
    const uint8_t* start = open(instance, frame);
    ShapedJSONParser parser(instance, vm, globalObject, table);
    JSValue parsed = start ? parser.parse(start, record.root) : JSValue();
    instance->jsonParserFrame = 0;
    if (parsed && parser.wasWorthwhile(length)) [[likely]]
        record.credit = std::min<uint8_t>(record.credit + 1, maxCreditOfBeginning);
    else if (record.credit > creditLostByPoorParse)
        record.credit -= creditLostByPoorParse;
    else {
        record.credit = 0;
        if (!isToldApartByMore && !record.timesSetAside) {
            countOperationNamed(instance, "jsonBeginningToldApartByMore");
            record.isToldApartByMore = true;
        } else {
            countOperationNamed(instance, "jsonBeginningSetAside");
            record.textsToSkip = textsSkippedAtFirst << std::min<unsigned>(record.timesSetAside, maxDoublingsOfTextsSkipped);
            if (record.timesSetAside < maxTimesSetAside)
                ++record.timesSetAside;
        }
        table.refusals = maxRefusalsBeforeForgetting;
    }
    if (table.refusals >= maxRefusalsBeforeForgetting) [[unlikely]]
        forgetLearnedJSONShapes(instance, *tables);
    tables->nextTextWasRefusedAsBytes = isGivenAsBytes && !parsed;
    return parsed;
}

JSValue tryParseJSONWithLearnedShapes(JSGlobalObject* globalObject, StringView text)
{
    return tryParseWithLearnedShapes<false>(globalObject, text, text.length(), [&](Instance* instance, uint64_t frame) {
        return openJSONString(instance, text, frame);
    });
}

JSValue tryParseUTF8JSONWithLearnedShapes(JSGlobalObject* globalObject, std::span<const uint8_t> bytes)
{
    return tryParseWithLearnedShapes<true>(globalObject, StringView { byteCast<Latin1Character>(bytes) }, bytes.size(), [&](Instance* instance, uint64_t frame) {
        return openUTF8JSONSource(instance, bytes, frame);
    });
}

void destroyJSONSource(Instance& instance)
{
    freeJSONSource(instance);
    delete instance.jsonShapeTables;
    delete instance.jsonKeyOrders;
}

void forgetUnusedJSONKeyOrders(Instance& instance, bool isYoungCollection)
{
    auto* orders = instance.jsonKeyOrders;
    if (!orders)
        return;
    if (std::exchange(orders->hasMembers, false))
        orders->members.fill({ });
    if (isYoungCollection)
        return;
    orders->known.removeIf([&](auto& entry) {
        return !std::exchange(entry.value->isUsedSinceFullCollection, false) || !instance.vm->heap.isMarked(entry.value->object.decode());
    });
}

void releaseLargeJSONSource(Instance& instance)
{
    if (instance.jsonSourceCapacity <= maxRetainedJSONSourceCapacity || instance.jsonParserFrame)
        return;
    freeJSONSource(instance);
}

namespace Typed {

using namespace B3;

static constexpr Range jsonCursorRange { 0, 1LL << 52 };
static constexpr int64_t stackReservedForJSONParsers = 64 * KB;

std::optional<Writes> writesOfInlineJSONParse(InlineBuiltin which, std::span<const Type>)
{
    switch (which) {
    case InlineBuiltin::Likely:
        return Writes::Pure;
    case InlineBuiltin::JSONByteAt:
    case InlineBuiltin::JSONStartsWith:
    case InlineBuiltin::JSONCursor:
    case InlineBuiltin::JSONNumber:
    case InlineBuiltin::JSONInteger:
    case InlineBuiltin::JSONSkipWhitespace:
        return Writes::Nothing;
    case InlineBuiltin::JSONOpen:
    case InlineBuiltin::JSONDecodeSource:
    case InlineBuiltin::JSONClose:
    case InlineBuiltin::JSONAbandon:
    case InlineBuiltin::JSONStringAt:
    case InlineBuiltin::JSONNumberAt:
    case InlineBuiltin::JSONIntegerAt:
    case InlineBuiltin::JSONValueAt:
    case InlineBuiltin::JSONKeyAt:
    case InlineBuiltin::JSONHasStackSpace:
    case InlineBuiltin::JSONSetCursor:
    case InlineBuiltin::JSONSkipValue:
    case InlineBuiltin::JSONSkipMember:
    case InlineBuiltin::JSONGiveUpIfEscaped:
    case InlineBuiltin::JSONGiveUpIfRepeated:
    case InlineBuiltin::JSONGiveUp:
    case InlineBuiltin::JSONParseWithShape:
    case InlineBuiltin::JSONKeyOrderMark:
    case InlineBuiltin::JSONRecordKey:
    case InlineBuiltin::JSONResetKeyOrder:
    case InlineBuiltin::JSONFindMember:
        return Writes::RuntimeState;
    case InlineBuiltin::JSONExtraMembersAt:
    case InlineBuiltin::JSONAddExtraMember:
    case InlineBuiltin::JSONApplyKeyOrder:
        return Writes::Heap;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

const Value& Lowering::constantJSONParserOperand(const Value& value, unsigned index) const
{
    ValueID id = value.arguments[index];
    while (m_function.at(id).opcode == Opcode::Cast)
        id = m_function.at(id).child(0);
    return m_function.at(id);
}

bool Lowering::isJSONParserTest(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Inline && (implementation.inlined == InlineBuiltin::JSONStartsWith || implementation.inlined == InlineBuiltin::JSONClose || implementation.inlined == InlineBuiltin::Likely);
}

void Lowering::branchOnJSONParserTest(const Value& value, LBasicBlock holds, LBasicBlock fails)
{
    if (m_builtins.at(value.builtin()).implementation.inlined == InlineBuiltin::Likely) {
        m_out.branch(in<Word32>(value.arguments[0]).value(), usually(holds), rarely(fails));
        return;
    }
    if (m_builtins.at(value.builtin()).implementation.inlined == InlineBuiltin::JSONClose) {
        LBasicBlock isAtEnd = m_out.newBlock();
        LBasicBlock hasTrailingCharacters = newUnlikelyBlock();
        Integer cursor = in<Integer>(value.arguments[0]);
        LValue end = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONSourceEnd()));
        m_out.branch(m_out.equal(cursor.value(), end), usually(isAtEnd), rarely(hasTrailingCharacters));
        m_out.appendTo(hasTrailingCharacters);
        m_out.branch(m_out.equal(skipJSONWhitespace(cursor).value(), end), unsure(isAtEnd), unsure(fails));
        m_out.appendTo(isAtEnd);
        m_out.branch(m_out.isZero32(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONParserGaveUp()))), usually(holds), rarely(fails));
        return;
    }
    auto& sourceOperand = constantJSONParserOperand(value, 1);
    auto& isExpectedOperand = constantJSONParserOperand(value, 2);
    RELEASE_ASSERT(sourceOperand.opcode == Opcode::String && isExpectedOperand.opcode == Opcode::Integer);
    std::span<const uint8_t> text = m_program.string(sourceOperand.string());
    bool isExpected = isExpectedOperand.integer();
    RELEASE_ASSERT(!text.empty());
    LValue cursor = in<Integer>(value.arguments[0]).value();
    size_t size = text.size();
    for (size_t offset = 0; offset < size;) {
        size_t remaining = size - offset;
        unsigned width = remaining >= 8 ? 8 : remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
        if (remaining > 4 && remaining < 8 && size >= 8) {
            width = 8;
            offset = size - 8;
        } else if (remaining == 3 && size >= 4) {
            width = 4;
            offset = size - 4;
        }
        uint64_t bits = 0;
        for (unsigned i = 0; i < width; ++i)
            bits |= static_cast<uint64_t>(text[offset + i]) << (8 * i);
        TypedPointer pointer = m_out.address(m_heaps.AOTTyped_runtimeState, cursor, offset);
        LValue isSame = nullptr;
        switch (width) {
        case 8:
            isSame = m_out.equal(m_out.load64(pointer), integer(static_cast<int64_t>(bits)).value());
            break;
        case 4:
            isSame = m_out.equal(m_out.load32(pointer), m_out.constInt32(static_cast<int32_t>(bits)));
            break;
        case 2:
            isSame = m_out.equal(m_out.load16ZeroExt32(pointer), m_out.constInt32(static_cast<int32_t>(bits)));
            break;
        default:
            isSame = m_out.equal(m_out.load8ZeroExt32(pointer), m_out.constInt32(static_cast<int32_t>(bits)));
            break;
        }
        offset += width;
        LBasicBlock next = offset < size ? m_out.newBlock() : holds;
        if (isExpected)
            m_out.branch(isSame, usually(next), rarely(fails));
        else
            m_out.branch(isSame, unsure(next), unsure(fails));
        if (offset < size)
            m_out.appendTo(next);
    }
}

Integer Lowering::skipJSONWhitespace(Integer cursor)
{
    static constexpr int64_t whitespace = 1LL << ' ' | 1LL << '\n' | 1LL << '\r' | 1LL << '\t';
    auto byteAt = [&](LValue at) {
        return m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_runtimeState, at, 0));
    };
    LBasicBlock continuation = m_out.newBlock();
    LBasicBlock mayBeWhitespace = newUnlikelyBlock();
    LBasicBlock isWhitespace = newUnlikelyBlock();
    Vector<ValueFromBlock, 3> ends;
    LValue first = byteAt(cursor.value());
    ValueFromBlock start = m_out.anchor(cursor.value());
    ValueFromBlock firstByte = m_out.anchor(first);
    ends.append(m_out.anchor(cursor.value()));
    m_out.branch(m_out.above(first, m_out.constInt32(' ')), unsure(continuation), unsure(mayBeWhitespace));

    m_out.appendTo(mayBeWhitespace);
    LValue at = m_out.phi(Int64, start);
    LValue byte = m_out.phi(Int32, firstByte);
    ends.append(m_out.anchor(at));
    m_out.branch(m_out.testNonZero64(m_out.lShr(integer(whitespace).value(), byte), m_out.constInt64(1)), unsure(isWhitespace), unsure(continuation));

    m_out.appendTo(isWhitespace);
    LValue next = m_out.add(at, m_out.constInt64(1));
    LValue nextByte = byteAt(next);
    ends.append(m_out.anchor(next));
    m_out.addIncomingToPhi(at, m_out.anchor(next));
    m_out.addIncomingToPhi(byte, m_out.anchor(nextByte));
    m_out.branch(m_out.above(nextByte, m_out.constInt32(' ')), unsure(continuation), unsure(mayBeWhitespace));

    m_out.appendTo(continuation);
    return Integer(m_out.phi(Int64, ends));
}

std::optional<Operand> Lowering::lowerInlineJSONParse(InlineBuiltin which, ValueID result, const Value& value, Writes writes)
{
    auto& arguments = value.arguments;
    auto& type = typeOf(result);
    Type cursorType = Type::integer(jsonCursorRange);
    auto cursorSlot = [&] {
        return m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONCursor());
    };
    auto returnCursor = [&](LValue cursor) {
        return ensure(Integer(cursor), cursorType, type);
    };
    switch (which) {
    case InlineBuiltin::Likely:
        if (m_isFusedWithBranch.get(result.index()))
            return booleanResult(boolean(false), type);
        return booleanResult(in<Word32>(arguments[0]), type);
    case InlineBuiltin::JSONStartsWith: {
        if (m_isFusedWithBranch.get(result.index()))
            return booleanResult(boolean(false), type);
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock fails = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        branchOnJSONParserTest(value, holds, fails);
        m_out.appendTo(holds);
        join.add(boolean(true));
        m_out.appendTo(fails);
        join.add(boolean(false));
        return booleanResult(join.finish().as<Word32>(), type);
    }
    case InlineBuiltin::JSONOpen:
        didEmitWrite(writes);
        return returnCursor(AOT_TYPED_OPERATION(JSONOpen, in<Boxed>(arguments[0]), Integer(m_out.framePointer())).value());
    case InlineBuiltin::JSONDecodeSource:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONDecodeSource, in<Boxed>(arguments[0])), type);
    case InlineBuiltin::JSONAbandon: {
        didEmitWrite(writes);
        LBasicBlock isOpen = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        TypedPointer frame = m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONParserFrame());
        m_out.branch(m_out.equal(m_out.load64(frame), m_out.framePointer()), unsure(isOpen), unsure(continuation));
        m_out.appendTo(isOpen);
        m_out.store64(m_out.constInt64(0), frame);
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case InlineBuiltin::JSONClose: {
        didEmitWrite(writes);
        m_out.store64(m_out.constInt64(0), m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONParserFrame()));
        if (m_isFusedWithBranch.get(result.index()))
            return booleanResult(boolean(false), type);
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock fails = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        branchOnJSONParserTest(value, holds, fails);
        m_out.appendTo(holds);
        join.add(boolean(true));
        m_out.appendTo(fails);
        join.add(boolean(false));
        return booleanResult(join.finish().as<Word32>(), type);
    }
    case InlineBuiltin::JSONByteAt:
        return ensure(zeroExtended(Word32(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_runtimeState, in<Integer>(arguments[0]).value(), 0)))), Type::integer({ 0, 255 }), type);
    case InlineBuiltin::JSONStringAt:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONStringAt, in<Integer>(arguments[0])), m_program.at(m_program.types.string), type);
    case InlineBuiltin::JSONNumberAt:
    case InlineBuiltin::JSONIntegerAt: {
        didEmitWrite(writes);
        TypedPointer slot = m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONNumber());
        auto digitAt = [&](LValue at) {
            return m_out.sub(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_runtimeState, at, 0)), m_out.constInt32('0'));
        };
        Integer start = in<Integer>(arguments[0]);
        LBasicBlock continuation = m_out.newBlock();
        LBasicBlock nextDigit = m_out.newBlock();
        LBasicBlock hasDigit = m_out.newBlock();
        LBasicBlock hasNoMoreDigits = m_out.newBlock();
        LBasicBlock isIntegralNumber = m_out.newBlock();
        LBasicBlock genericPath = m_out.newBlock();
        LValue first = digitAt(start.value());
        ValueFromBlock firstCursor = m_out.anchor(start.value());
        ValueFromBlock firstDigits = m_out.anchor(m_out.zeroExt(first, Int64));
        m_out.branch(m_out.below(m_out.sub(first, m_out.constInt32(1)), m_out.constInt32(9)), unsure(nextDigit), unsure(genericPath));

        m_out.appendTo(nextDigit);
        LValue previous = m_out.phi(Int64, firstCursor);
        LValue digits = m_out.phi(Int64, firstDigits);
        LValue at = m_out.add(previous, m_out.constInt64(1));
        LValue digit = digitAt(at);
        m_out.branch(m_out.below(digit, m_out.constInt32(10)), unsure(hasDigit), unsure(hasNoMoreDigits));

        m_out.appendTo(hasDigit);
        m_out.addIncomingToPhi(previous, m_out.anchor(at));
        m_out.addIncomingToPhi(digits, m_out.anchor(m_out.add(m_out.mul(digits, m_out.constInt64(10)), m_out.zeroExt(digit, Int64))));
        m_out.jump(nextDigit);

        m_out.appendTo(hasNoMoreDigits);
        for (LValue continuesNumber : { m_out.equal(digit, m_out.constInt32('.' - '0')), m_out.equal(m_out.bitOr(digit, m_out.constInt32(0x20)), m_out.constInt32('e' - '0')) }) {
            LBasicBlock next = m_out.newBlock();
            m_out.branch(continuesNumber, unsure(genericPath), unsure(next));
            m_out.appendTo(next);
        }
        m_out.branch(m_out.above(m_out.sub(at, start.value()), m_out.constInt64(maxExactMantissaDigits)), rarely(genericPath), usually(isIntegralNumber));

        m_out.appendTo(isIntegralNumber);
        if (which == InlineBuiltin::JSONIntegerAt)
            m_out.store64(digits, slot);
        else
            m_out.storeDouble(m_out.intToDouble(digits), slot);
        ValueFromBlock inlinePath = m_out.anchor(at);
        m_out.jump(continuation);

        m_out.appendTo(genericPath);
        ValueFromBlock byOperation = m_out.anchor(which == InlineBuiltin::JSONIntegerAt ? AOT_TYPED_OPERATION(JSONIntegerAt, start).value() : AOT_TYPED_OPERATION(JSONNumberAt, start).value());
        m_out.jump(continuation);

        m_out.appendTo(continuation);
        return returnCursor(m_out.phi(Int64, inlinePath, byOperation));
    }
    case InlineBuiltin::JSONNumber:
        return ensure(Number(m_out.loadDouble(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONNumber()))), m_program.at(m_program.types.number), type);
    case InlineBuiltin::JSONInteger:
        return ensure(Integer(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONNumber()))), Type::integer({ -maxExactInteger, maxExactInteger }), type);
    case InlineBuiltin::JSONKeyAt:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONKeyAt, in<Integer>(arguments[0])), m_program.at(m_program.types.string), type);
    case InlineBuiltin::JSONHasStackSpace: {
        didEmitWrite(writes);
        LBasicBlock isOutOfStack = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock yes = m_out.anchor(m_out.constInt32(1));
        m_out.branch(m_out.aboveOrEqual(m_out.sub(m_out.framePointer(), m_out.constInt64(stackReservedForJSONParsers)), changing64(Instance::offsetOfSoftStackLimit())), usually(continuation), rarely(isOutOfStack));
        m_out.appendTo(isOutOfStack);
        m_out.store32(m_out.constInt32(1), m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONParserGaveUp()));
        ValueFromBlock no = m_out.anchor(m_out.constInt32(0));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return booleanResult(Word32(m_out.phi(Int32, yes, no)), type);
    }
    case InlineBuiltin::JSONValueAt:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONValueAt, in<Integer>(arguments[0])), type);
    case InlineBuiltin::JSONCursor:
        return returnCursor(m_out.load64(cursorSlot()));
    case InlineBuiltin::JSONSetCursor:
        didEmitWrite(writes);
        m_out.store64(in<Integer>(arguments[0]).value(), cursorSlot());
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::JSONSkipWhitespace:
        return returnCursor(skipJSONWhitespace(in<Integer>(arguments[0])).value());
    case InlineBuiltin::JSONSkipValue:
        didEmitWrite(writes);
        return returnCursor(AOT_TYPED_OPERATION(JSONSkipValue, in<Integer>(arguments[0])).value());
    case InlineBuiltin::JSONSkipMember:
        didEmitWrite(writes);
        return returnCursor(AOT_TYPED_OPERATION(JSONSkipMember, in<Integer>(arguments[0])).value());
    case InlineBuiltin::JSONGiveUpIfEscaped:
        didEmitWrite(writes);
        AOT_TYPED_OPERATION(JSONGiveUpIfEscaped, in<Integer>(arguments[0]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::JSONGiveUpIfRepeated:
        didEmitWrite(writes);
        AOT_TYPED_OPERATION(JSONGiveUpIfRepeated, in<Integer>(arguments[0]), in<CellPointer>(arguments[1]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::JSONParseWithShape:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONParseWithShape, in<Boxed>(arguments[0]), in<CellPointer>(arguments[1])), type);
    case InlineBuiltin::JSONGiveUp:
        didEmitWrite(writes);
        m_out.store32(m_out.constInt32(1), m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONParserGaveUp()));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::JSONKeyOrderMark: {
        didEmitWrite(writes);
        LBasicBlock isNotReserved = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue top = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONKeyOrderTop()));
        ValueFromBlock reserved = m_out.anchor(top);
        m_out.branch(m_out.notZero64(top), usually(continuation), rarely(isNotReserved));
        m_out.appendTo(isNotReserved);
        ValueFromBlock byOperation = m_out.anchor(AOT_TYPED_OPERATION(JSONReserveKeyOrder).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return returnCursor(m_out.phi(Int64, reserved, byOperation));
    }
    case InlineBuiltin::JSONRecordKey: {
        didEmitWrite(writes);
        LBasicBlock hasRoom = m_out.newBlock();
        LBasicBlock isFull = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        TypedPointer slot = m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONKeyOrderTop());
        LValue top = m_out.load64(slot);
        m_out.branch(m_out.notEqual(top, m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONKeyOrderEnd()))), usually(hasRoom), rarely(isFull));
        m_out.appendTo(hasRoom);
        m_out.store32As16(m_out.castToInt32(in<Integer>(arguments[0]).value()), m_out.address(m_heaps.AOTTyped_runtimeState, top, 0));
        m_out.store64(m_out.add(top, m_out.constInt64(sizeof(uint16_t))), slot);
        m_out.jump(continuation);
        m_out.appendTo(isFull);
        m_out.store32(m_out.constInt32(1), m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONParserGaveUp()));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case InlineBuiltin::JSONResetKeyOrder:
        didEmitWrite(writes);
        m_out.store64(in<Integer>(arguments[0]).value(), m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONKeyOrderTop()));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::JSONFindMember:
        didEmitWrite(writes);
        return returnCursor(AOT_TYPED_OPERATION(JSONFindMember, in<Integer>(arguments[0]), in<CellPointer>(arguments[1])).value());
    case InlineBuiltin::JSONExtraMembersAt:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONExtraMembersAt, in<Integer>(arguments[0]), in<Boxed>(arguments[1]), in<CellPointer>(arguments[2]), in<Integer>(arguments[3])), type);
    case InlineBuiltin::JSONAddExtraMember:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONAddExtraMember, in<Boxed>(arguments[0]), in<CellPointer>(arguments[1]), in<Boxed>(arguments[2]), in<Integer>(arguments[3])), type);
    case InlineBuiltin::JSONApplyKeyOrder:
        didEmitWrite(writes);
        AOT_TYPED_OPERATION(JSONApplyKeyOrder, in<CellPointer>(arguments[0]), in<Boxed>(arguments[1]), in<CellPointer>(arguments[2]), in<Integer>(arguments[3]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
