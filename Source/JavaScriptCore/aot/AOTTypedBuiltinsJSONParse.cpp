/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSONCacheInlines.h"
#include "JSONObject.h"
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
    return instance->jsonSourceStart;
}

static ALWAYS_INLINE uint8_t* reserveJSONSource(Instance* instance, size_t size)
{
    size_t capacity = instance->jsonSourceCapacity;
    if (size <= capacity && (capacity <= maxRetainedJSONSourceCapacity || size > maxRetainedJSONSourceCapacity)) [[likely]]
        return instance->jsonSourceStart;
    return replaceJSONSource(instance, size);
}

static NEVER_INLINE const StringImpl* flattenJSONText(Instance* instance, CallFrame* callFrame, JSString* string)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto value = string->tryGetValue();
    return value.data.impl();
}

static NEVER_INLINE uint8_t* openWideJSONText(Instance* instance, std::span<const char16_t> characters)
{
    uint8_t* start = reserveJSONSource(instance, characters.size() * 3 + jsonSourcePadding);
    if (!start)
        return nullptr;
    auto converted = WTF::Unicode::convert(characters, std::span<char8_t> { byteCast<char8_t>(start), characters.size() * 3 });
    if (converted.code != WTF::Unicode::ConversionResultCode::Success)
        return nullptr;
    size_t length = converted.buffer.size();
    memset(start + length, 0, jsonSourcePadding);
    instance->jsonSourceEnd = start + length;
    instance->jsonSourceIsUTF8 = 1;
    return start;
}

static std::span<const uint8_t> withoutByteOrderMark(std::span<const uint8_t> bytes)
{
    bool hasMark = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf;
    return bytes.subspan(hasMark ? 3 : 0);
}

static NEVER_INLINE uint8_t* openJSONBytes(Instance* instance, JSValue value, uint64_t frame)
{
    if (!value.isCell() || value.asCell()->type() != Uint8ArrayType || frame < instance->jsonParserFrame)
        return nullptr;
    auto bytes = withoutByteOrderMark(uncheckedDowncast<JSArrayBufferView>(value.asCell())->span());
    uint8_t* start = reserveJSONSource(instance, bytes.size() + jsonSourcePadding);
    if (!start)
        return nullptr;
    if (!bytes.empty())
        memcpy(start, bytes.data(), bytes.size());
    memset(start + bytes.size(), 0, jsonSourcePadding);
    instance->jsonSourceEnd = start + bytes.size();
    instance->jsonSourceIsUTF8 = 1;
    instance->jsonParserGaveUp = 0;
    instance->jsonParserFrame = frame;
    return start;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONTextOf, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isCell() || value.asCell()->type() != Uint8ArrayType)
        return encodedValue;
    String text = String::fromUTF8ReplacingInvalidSequences(withoutByteOrderMark(uncheckedDowncast<JSArrayBufferView>(value.asCell())->span()));
    return text.isNull() ? encodedValue : JSValue::encode(jsString(vm, WTF::move(text)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONOpen, void*, (Instance* instance, EncodedJSValue encodedText, uint64_t frame))
{
    countOperationNamed(instance, __func__);
    JSValue text = JSValue::decode(encodedText);
    if (!text.isString() || frame < instance->jsonParserFrame) [[unlikely]]
        return openJSONBytes(instance, text, frame);
    JSString* string = asString(text);
    const StringImpl* impl = string->tryGetValueImpl();
    if (!impl) [[unlikely]] {
        impl = flattenJSONText(instance, DECLARE_CALL_FRAME(*instance->vm), string);
        if (!impl)
            return nullptr;
    }
    instance->jsonParserGaveUp = 0;
    instance->jsonParserFrame = frame;
    if (!impl->is8Bit()) [[unlikely]]
        return openWideJSONText(instance, impl->span16());
    size_t length = impl->length();
    uint8_t* start = reserveJSONSource(instance, length + jsonSourcePadding);
    if (!start) [[unlikely]]
        return nullptr;
    memcpy(start, impl->span8().data(), length);
    memset(start + length, 0, jsonSourcePadding);
    instance->jsonSourceEnd = start + length;
    instance->jsonSourceIsUTF8 = 0;
    return start;
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
    if (!instance->jsonSourceIsUTF8 || charactersAreAllASCII(characters)) {
        builder.append(byteCast<Latin1Character>(characters));
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

static NEVER_INLINE JSCell* makeUncachedString(Instance* instance, CallFrame* callFrame, const uint8_t* start)
{
    VM& vm = *instance->vm;
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    const uint8_t* cursor = findEndOfPlainJSONCharacters(start);
    instance->jsonCursor = 0;
    if (*cursor != '"' || (instance->jsonSourceIsUTF8 && !charactersAreAllASCII(std::span<const uint8_t> { start, cursor }))) [[unlikely]]
        return makeStringWithEscapes(instance, vm, start, cursor);
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONStringAt, JSCell*, (Instance* instance, uint64_t cursorBits))
{
    countOperationNamed(instance, __func__);
    auto* start = std::bit_cast<const uint8_t*>(cursorBits);
    if (auto* entry = instance->vm->jsonCache().findString(std::span<const Latin1Character> { byteCast<Latin1Character>(start - 1), jsonSourcePadding })) [[likely]] {
        if (!instance->jsonSourceIsUTF8 || charactersAreAllASCII(std::span<const uint8_t> { start, entry->textLength })) [[likely]] {
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
    if (instance->jsonSourceIsUTF8 && !charactersAreAllASCII(std::span<const uint8_t> { start, cursor })) [[unlikely]]
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONNumberAt, void*, (Instance* instance, uint64_t cursorBits))
{
    auto* start = std::bit_cast<const uint8_t*>(cursorBits);
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedJSONValueAt, EncodedJSValue, (Instance* instance, uint64_t cursorBits))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* start = std::bit_cast<const uint8_t*>(cursorBits);
    instance->jsonCursor = 0;
    const uint8_t* end = skipJSONValue(instance, start);
    if (!end)
        return JSValue::encode(jsUndefined());
    std::span<const uint8_t> characters { start, end };
    JSValue parsed;
    if (!instance->jsonSourceIsUTF8 || charactersAreAllASCII(characters))
        parsed = JSONParse(globalObject, StringView { byteCast<Latin1Character>(characters) });
    else
        parsed = JSONParse(globalObject, String::fromUTF8(characters));
    if (auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm); catchScope.exception()) [[unlikely]] {
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

void destroyJSONSource(Instance& instance)
{
    fastFree(instance.jsonSourceStart);
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
    case InlineBuiltin::JSONSkipWhitespace:
        return Writes::Nothing;
    case InlineBuiltin::JSONOpen:
    case InlineBuiltin::JSONTextOf:
    case InlineBuiltin::JSONClose:
    case InlineBuiltin::JSONAbandon:
    case InlineBuiltin::JSONStringAt:
    case InlineBuiltin::JSONNumberAt:
    case InlineBuiltin::JSONValueAt:
    case InlineBuiltin::JSONKeyAt:
    case InlineBuiltin::JSONHasStackSpace:
    case InlineBuiltin::JSONSetCursor:
    case InlineBuiltin::JSONSkipValue:
    case InlineBuiltin::JSONSkipMember:
    case InlineBuiltin::JSONGiveUpIfEscaped:
    case InlineBuiltin::JSONGiveUpIfRepeated:
    case InlineBuiltin::JSONGiveUp:
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
        LBasicBlock hasTrailingText = newUnlikelyBlock();
        Integer cursor = in<Integer>(value.arguments[0]);
        LValue end = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONSourceEnd()));
        m_out.branch(m_out.equal(cursor.value(), end), usually(isAtEnd), rarely(hasTrailingText));
        m_out.appendTo(hasTrailingText);
        m_out.branch(m_out.equal(skipJSONWhitespace(cursor).value(), end), unsure(isAtEnd), unsure(fails));
        m_out.appendTo(isAtEnd);
        m_out.branch(m_out.isZero32(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONParserGaveUp()))), usually(holds), rarely(fails));
        return;
    }
    auto& textOperand = constantJSONParserOperand(value, 1);
    auto& isExpectedOperand = constantJSONParserOperand(value, 2);
    RELEASE_ASSERT(textOperand.opcode == Opcode::String && isExpectedOperand.opcode == Opcode::Integer);
    std::span<const uint8_t> text = m_program.string(textOperand.string());
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
    auto yieldCursor = [&](LValue cursor) {
        return ensure(Integer(cursor), cursorType, type);
    };
    switch (which) {
    case InlineBuiltin::Likely:
        if (m_isCarriedOutByBranch.get(result.index()))
            return yieldBoolean(boolean(false), type);
        return yieldBoolean(in<Word32>(arguments[0]), type);
    case InlineBuiltin::JSONStartsWith: {
        if (m_isCarriedOutByBranch.get(result.index()))
            return yieldBoolean(boolean(false), type);
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock fails = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        branchOnJSONParserTest(value, holds, fails);
        m_out.appendTo(holds);
        join.add(boolean(true));
        m_out.appendTo(fails);
        join.add(boolean(false));
        return yieldBoolean(join.finish().as<Word32>(), type);
    }
    case InlineBuiltin::JSONOpen:
        didEmitWrite(writes);
        return yieldCursor(AOT_TYPED_OPERATION(JSONOpen, in<Boxed>(arguments[0]), Integer(m_out.framePointer())).value());
    case InlineBuiltin::JSONTextOf:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONTextOf, in<Boxed>(arguments[0])), type);
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
        if (m_isCarriedOutByBranch.get(result.index()))
            return yieldBoolean(boolean(false), type);
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock fails = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        branchOnJSONParserTest(value, holds, fails);
        m_out.appendTo(holds);
        join.add(boolean(true));
        m_out.appendTo(fails);
        join.add(boolean(false));
        return yieldBoolean(join.finish().as<Word32>(), type);
    }
    case InlineBuiltin::JSONByteAt:
        return ensure(zeroExtended(Word32(m_out.load8ZeroExt32(m_out.address(m_heaps.AOTTyped_runtimeState, in<Integer>(arguments[0]).value(), 0)))), Type::integer({ 0, 255 }), type);
    case InlineBuiltin::JSONStringAt:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONStringAt, in<Integer>(arguments[0])), m_program.at(m_program.types.string), type);
    case InlineBuiltin::JSONNumberAt:
        didEmitWrite(writes);
        return yieldCursor(AOT_TYPED_OPERATION(JSONNumberAt, in<Integer>(arguments[0])).value());
    case InlineBuiltin::JSONNumber:
        return ensure(Number(m_out.loadDouble(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfJSONNumber()))), m_program.at(m_program.types.number), type);
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
        return yieldBoolean(Word32(m_out.phi(Int32, yes, no)), type);
    }
    case InlineBuiltin::JSONValueAt:
        didEmitWrite(writes);
        return ensure(AOT_TYPED_OPERATION(JSONValueAt, in<Integer>(arguments[0])), type);
    case InlineBuiltin::JSONCursor:
        return yieldCursor(m_out.load64(cursorSlot()));
    case InlineBuiltin::JSONSetCursor:
        didEmitWrite(writes);
        m_out.store64(in<Integer>(arguments[0]).value(), cursorSlot());
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::JSONSkipWhitespace:
        return yieldCursor(skipJSONWhitespace(in<Integer>(arguments[0])).value());
    case InlineBuiltin::JSONSkipValue:
        didEmitWrite(writes);
        return yieldCursor(AOT_TYPED_OPERATION(JSONSkipValue, in<Integer>(arguments[0])).value());
    case InlineBuiltin::JSONSkipMember:
        didEmitWrite(writes);
        return yieldCursor(AOT_TYPED_OPERATION(JSONSkipMember, in<Integer>(arguments[0])).value());
    case InlineBuiltin::JSONGiveUpIfEscaped:
        didEmitWrite(writes);
        AOT_TYPED_OPERATION(JSONGiveUpIfEscaped, in<Integer>(arguments[0]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case InlineBuiltin::JSONGiveUpIfRepeated:
        didEmitWrite(writes);
        AOT_TYPED_OPERATION(JSONGiveUpIfRepeated, in<Integer>(arguments[0]), in<CellPointer>(arguments[1]));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
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
