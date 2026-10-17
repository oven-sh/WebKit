/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
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

static NEVER_INLINE uint8_t* replaceJSONSource(Instance* instance, size_t size)
{
    fastFree(std::exchange(instance->jsonSourceStart, nullptr));
    instance->jsonSourceCapacity = 0;
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
    JSString* made = nullptr;
    if (size_t length = cursor - start; length > 1) [[likely]] {
        std::span<Latin1Character> copy;
        auto impl = StringImpl::tryCreateUninitialized(length, copy);
        if (!impl) [[unlikely]]
            return jsEmptyString(vm);
        memcpy(copy.data(), start, length);
        made = jsNontrivialString(vm, String { impl.releaseNonNull() });
    } else
        made = length ? jsSingleCharacterString(vm, static_cast<Latin1Character>(*start)) : jsEmptyString(vm);
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

static EncodedJSValue parseJSONValueAt(Instance* instance, VM& vm, JSGlobalObject* globalObject, const uint8_t* start)
{
    instance->jsonCursor = 0;
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

static constexpr unsigned maxShapedMembers = 48;
static_assert(maxShapedMembers <= JSFinalObject::maxInlineCapacity);
static constexpr unsigned maxStructuresPerShape = 4;
static constexpr size_t maxLearnedNameSize = 50;
static constexpr unsigned maxDigitsOfSmallInteger = 9;
static constexpr size_t inlineElementCapacity = 32;
static constexpr unsigned maxShapesPerTable = 256;
static constexpr unsigned maxAlternativesPerShape = 16;
static constexpr size_t maxLengthParsedWithShapes = 1 * MB;
static constexpr unsigned maxRefusalsBeforeForgetting = 256;
static constexpr unsigned numberOfRememberedBeginnings = 1024;
static constexpr uint8_t creditOfNewBeginning = 8;
static constexpr uint8_t creditOnTrial = 4;
static constexpr uint8_t maxCreditOfBeginning = 32;
static constexpr uint8_t creditLostByPoorParse = 2;
static constexpr unsigned textsSkippedAtFirst = 64;
static constexpr unsigned maxDoublingsOfTextsSkipped = 9;
static constexpr size_t bytesThatPayForCostlyStep = 64;
static constexpr size_t minLengthOfLongString = 64;

struct JSONShapeMember {
    JSONShapeMember(VM& vm, std::span<const uint8_t> quotedName, unsigned shape)
        : size(quotedName.size() + 1)
        , shape(shape)
        , name(Identifier::fromString(vm, byteCast<Latin1Character>(quotedName.subspan(1, quotedName.size() - 2))))
    {
        RELEASE_ASSERT(size <= jsonSourcePadding);
        nameAndColon.append(quotedName);
        nameAndColon.append(':');
        for (size_t i = 0; i < std::min<size_t>(size, 8); ++i) {
            firstBytes |= static_cast<uint64_t>(nameAndColon[i]) << (8 * i);
            firstBytesMask |= 0xffULL << (8 * i);
        }
        if (size > 8)
            lastBytes = WTF::unalignedLoad<uint64_t>(nameAndColon.span().data() + size - 8);
    }

    uint64_t firstBytes { 0 };
    uint64_t firstBytesMask { 0 };
    uint64_t lastBytes { 0 };
    unsigned size;
    unsigned shape;
    Identifier name;
    Vector<uint8_t, 24> nameAndColon;
};

struct JSONShapeStructure {
    Vector<uint8_t, 16> order;
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
    Vector<JSONShapeMember> members;
    Vector<unsigned> alternatives;
    Vector<JSONShapeStructure, 1> structures;
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
    uint8_t timesSetAside { 0 };
    uint16_t textsToSkip { 0 };
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

static ALWAYS_INLINE uint64_t beginningOf(StringView text)
{
    uint64_t beginning = 0;
    unsigned length = std::min<unsigned>(text.length(), sizeof(uint64_t));
    for (unsigned i = 0; i < length; ++i)
        beginning |= static_cast<uint64_t>(static_cast<uint8_t>(text[i])) << (8 * i);
    return beginning;
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

    JSValue parse(const uint8_t* start)
    {
        Parsed parsed = parseValue(start, m_table.root);
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
        return size <= 16 || !memcmp(cursor + 8, member.nameAndColon.span().data() + 8, size - 16);
    }

    static ALWAYS_INLINE bool isAtName(Cursor cursor, const JSONShapeMember& member)
    {
        return !memcmp(cursor, member.nameAndColon.span().data(), member.size - 1);
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
        EncodedJSValue value = parseJSONValueAt(m_instance, m_vm, m_globalObject, start);
        if (!m_instance->jsonCursor)
            return noAnswer;
        return { value, std::bit_cast<Cursor>(m_instance->jsonCursor) };
    }

    unsigned learnShape(JSONShape::Kind kind)
    {
        ++m_costlySteps;
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
        ++m_table.refusals;
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
        if (parseIndex(member.name) || member.name == m_vm.propertyNames->underscoreProto)
            return false;
        shape.members.append(WTF::move(member));
        ++m_costlySteps;
        return true;
    }

    Parsed parseObject(Cursor brace, unsigned& place)
    {
        if (!m_vm.isSafeToRecurse()) [[unlikely]]
            return noAnswer;
        Cursor cursor = skipWhitespace(brace + 1);
        if (*cursor == '}') [[unlikely]]
            return { JSValue::encode(constructEmptyObject(m_globalObject)), cursor + 1 };
        JSONShape* shape = &shapeAt(place);
        if (shape->kind != JSONShape::Kind::Object) [[unlikely]] {
            shape = objectShapeFor(place, cursor);
            if (!shape)
                return parseWithoutShape(brace);
        }
        if (shape->kind == JSONShape::Kind::Dictionary) [[unlikely]]
            return parseRemainingMembers(constructEmptyObject(m_globalObject), cursor, nullptr, shape->element);
        EncodedJSValue values[JSFinalObject::maxInlineCapacity];
        for (unsigned i = 0; i < JSFinalObject::defaultInlineCapacity; ++i)
            values[i] = JSValue::encode(JSValue());
        uint8_t order[maxShapedMembers];
        JSONShapeMember* members = shape->members.mutableSpan().data();
        unsigned size = shape->members.size();
        unsigned count = 0;
        unsigned next = 0;
        uint64_t present = 0;
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
        if (next > 4 * count + 8) [[unlikely]]
            ++m_costlySteps;
        Structure* structure = shape->lastStructure;
        if (!isInDeclaredOrder || present != shape->membersOfLastStructure) [[unlikely]] {
            structure = structureFor(*shape, order, count, isInDeclaredOrder ? present : 0);
            if (!structure)
                return { JSValue::encode(makeObjectMemberByMember(*shape, values, order, count)), cursor };
        }
        unsigned inlineCapacity = structure->inlineCapacity();
        for (unsigned i = std::max(count, JSFinalObject::defaultInlineCapacity); i < inlineCapacity; ++i)
            values[i] = JSValue::encode(JSValue());
        JSObject* object = JSFinalObject::createWithButterflyCopyingInlineStorage(m_vm, structure, nullptr, reinterpret_cast<const WriteBarrierBase<Unknown>*>(values));
        return { JSValue::encode(object), cursor };
    }

    NEVER_INLINE Structure* structureFor(JSONShape& shape, const uint8_t* order, unsigned count, uint64_t membersInDeclaredOrder)
    {
        Structure* structure = nullptr;
        for (auto& known : shape.structures) {
            if (known.order.size() == count && !memcmp(known.order.span().data(), order, count)) {
                structure = known.structure.get();
                break;
            }
        }
        if (!structure) {
            ++m_costlySteps;
            structure = makeStructure(shape, order, count);
            if (!structure || shape.structures.size() >= maxStructuresPerShape)
                return structure;
            JSONShapeStructure known;
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
    JSValue parsed = start ? ShapedJSONParser(instance, vm, globalObject, *table).parse(start) : JSValue();
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
    JSONBeginning& record = tables->beginnings[(beginningOf(beginning) * 0x9E3779B97F4A7C15ULL) >> (64 - WTF::fastLog2(numberOfRememberedBeginnings))];
    if (!record.credit) [[unlikely]] {
        bool isSeenForTheFirstTime = !record.timesSetAside;
        if (record.textsToSkip || isSeenForTheFirstTime) {
            if (record.textsToSkip)
                --record.textsToSkip;
            else
                record.credit = creditOfNewBeginning;
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
    JSValue parsed = start ? parser.parse(start) : JSValue();
    instance->jsonParserFrame = 0;
    if (parsed && parser.wasWorthwhile(length)) [[likely]]
        record.credit = std::min<uint8_t>(record.credit + 1, maxCreditOfBeginning);
    else if (record.credit > creditLostByPoorParse)
        record.credit -= creditLostByPoorParse;
    else {
        countOperationNamed(instance, "jsonBeginningSetAside");
        record.credit = 0;
        record.textsToSkip = textsSkippedAtFirst << std::min<unsigned>(record.timesSetAside, maxDoublingsOfTextsSkipped);
        if (record.timesSetAside < std::numeric_limits<uint8_t>::max())
            ++record.timesSetAside;
        table.refusals = maxRefusalsBeforeForgetting;
    }
    if (table.refusals >= maxRefusalsBeforeForgetting) [[unlikely]]
        tables->learned = nullptr;
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
    fastFree(instance.jsonSourceStart);
    delete instance.jsonShapeTables;
}

void releaseLargeJSONSource(Instance& instance)
{
    if (instance.jsonSourceCapacity <= maxRetainedJSONSourceCapacity || instance.jsonParserFrame)
        return;
    fastFree(std::exchange(instance.jsonSourceStart, nullptr));
    instance.jsonSourceCapacity = 0;
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
        return Writes::RuntimeState;
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
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
