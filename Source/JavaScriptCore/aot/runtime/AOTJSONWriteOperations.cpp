/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONTextWriter.h"

#if ENABLE(AOT)

#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "JSCInlines.h"
#include <array>
#include <wtf/MathExtras.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/Vector.h>
#include <wtf/text/EscapedFormsForJSON.h>
#include <wtf/text/StringBuilderJSON.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using Typed::JSONOutcome;
using Typed::JSONPosition;

static constexpr size_t numberOfQuotes = 2;
static constexpr size_t maxEscapedCharacterLength = 6;

using ByteVector = decltype(SIMD::load(static_cast<const uint8_t*>(nullptr)));

static ALWAYS_INLINE ByteVector findEscapesInJSON(ByteVector input)
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

static ALWAYS_INLINE bool copyAndCheckForEscapes(std::span<const Latin1Character> characters, Latin1Character* to)
{
    const Latin1Character* from = characters.data();
    size_t size = characters.size();
    constexpr size_t stride = SIMD::stride<uint8_t>;
    if (size >= stride) {
        ByteVector found { };
        auto take = [&](size_t offset) ALWAYS_INLINE_LAMBDA {
            auto input = SIMD::load(std::bit_cast<const uint8_t*>(from + offset));
            SIMD::store(input, std::bit_cast<uint8_t*>(to + offset));
            found = SIMD::bitOr(found, findEscapesInJSON(input));
        };
        size_t offset = 0;
        for (; offset + stride <= size; offset += stride)
            take(offset);
        if (offset < size)
            take(size - stride);
        return SIMD::isNonZero(found);
    }
    if (size >= 8) {
        uint64_t first = WTF::unalignedLoad<uint64_t>(from);
        uint64_t last = WTF::unalignedLoad<uint64_t>(from + size - 8);
        WTF::unalignedStore<uint64_t>(to, first);
        WTF::unalignedStore<uint64_t>(to + size - 8, last);
        return needsEscapeInJSON(first, last);
    }
    if (size >= 4) {
        uint32_t first = WTF::unalignedLoad<uint32_t>(from);
        uint32_t last = WTF::unalignedLoad<uint32_t>(from + size - 4);
        WTF::unalignedStore<uint32_t>(to, first);
        WTF::unalignedStore<uint32_t>(to + size - 4, last);
        uint64_t both = first | static_cast<uint64_t>(last) << 32;
        return needsEscapeInJSON(both, both);
    }
    bool found = false;
    for (size_t i = 0; i < size; ++i) {
        found |= !!WTF::escapedFormsForJSON[from[i]];
        to[i] = from[i];
    }
    return found;
}

static ALWAYS_INLINE size_t roomForQuotedString(size_t maxLength)
{
    return maxLength + numberOfQuotes + maxStringBuilderReservation;
}

static ALWAYS_INLINE size_t roomForEscapedString(size_t length)
{
    return roomForQuotedString(length * maxEscapedCharacterLength);
}

static NEVER_INLINE Latin1Character* appendQuotedAndEscaped(Latin1Character* cursor, std::span<const Latin1Character> characters)
{
    *cursor++ = '"';
    std::span<Latin1Character> output { cursor, characters.size() * maxEscapedCharacterLength };
    WTF::appendEscapedJSONStringContent(output, characters);
    cursor = output.data();
    *cursor++ = '"';
    return cursor;
}

static NEVER_INLINE Latin1Character* tryAppendEscapedAtCursor(const StringBuilderStorage& builder, Latin1Character* cursor, std::span<const Latin1Character> characters)
{
    if (!builder.hasRoomAt(cursor, roomForEscapedString(characters.size())))
        return nullptr;
    return appendQuotedAndEscaped(cursor, characters);
}

static NEVER_INLINE Latin1Character* appendQuotedWithEscapes(JSGlobalObject* globalObject, StringBuilderStorage& builder, std::span<const Latin1Character> characters)
{
    Latin1Character* cursor = builder.reserve(globalObject, roomForEscapedString(characters.size()));
    if (!cursor)
        return nullptr;
    return appendQuotedAndEscaped(cursor, characters);
}

static NEVER_INLINE Latin1Character* appendQuoted16Bit(JSGlobalObject* globalObject, StringBuilderStorage& builder, std::span<const char16_t> characters)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    size_t maxEscapedLength = characters.size() * maxEscapedCharacterLength;
    Vector<char16_t, 64> text;
    if (!text.tryGrow(maxEscapedLength + numberOfQuotes)) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    std::span<char16_t> quoted = text.mutableSpan();
    quoted[0] = '"';
    std::span<char16_t> output = quoted.subspan(1, maxEscapedLength);
    WTF::appendEscapedJSONStringContent(output, characters);
    size_t length = output.data() - quoted.data();
    quoted[length++] = '"';

    builder.append(globalObject, std::span<const char16_t> { quoted.first(length) });
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, builder.reserve(globalObject, maxStringBuilderReservation));
}

// A failed attempt may have written past the cursor. Those bytes are not text until a cursor past them is committed.
static ALWAYS_INLINE Latin1Character* tryAppendQuotedWithoutEscapes(const StringBuilderStorage& builder, Latin1Character* cursor, const StringImpl& string)
{
    if (!string.is8Bit()) [[unlikely]]
        return nullptr;
    auto characters = string.span8();
    if (!builder.hasRoomAt(cursor, roomForQuotedString(characters.size()))) [[unlikely]]
        return nullptr;
    *cursor = '"';
    if (copyAndCheckForEscapes(characters, cursor + 1)) [[unlikely]]
        return nullptr;
    cursor += characters.size() + 1;
    *cursor++ = '"';
    return cursor;
}

Latin1Character* appendQuotedJSONString(JSGlobalObject* globalObject, StringBuilderStorage& builder, Latin1Character* cursor, const StringImpl& string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!string.is8Bit()) [[unlikely]] {
        builder.commit(cursor);
        RELEASE_AND_RETURN(scope, appendQuoted16Bit(globalObject, builder, string.span16()));
    }

    auto characters = string.span8();
    if (!builder.hasRoomAt(cursor, roomForQuotedString(characters.size()))) [[unlikely]] {
        builder.commit(cursor);
        cursor = builder.reserve(globalObject, roomForQuotedString(characters.size()));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (Latin1Character* end = tryAppendQuotedWithoutEscapes(builder, cursor, string)) [[likely]]
        return end;
    if (Latin1Character* end = tryAppendEscapedAtCursor(builder, cursor, characters))
        return end;
    builder.commit(cursor);
    RELEASE_AND_RETURN(scope, appendQuotedWithEscapes(globalObject, builder, characters));
}

static ALWAYS_INLINE Latin1Character* appendPrimitive(Latin1Character* cursor, JSValue value)
{
    if (value.isNumber())
        return appendJSONNumber(cursor, value.asNumber());
    if (value.isBoolean())
        return value.isTrue() ? appendJSONLiteral(cursor, "true"_s) : appendJSONLiteral(cursor, "false"_s);
    return value.isNull() ? appendJSONLiteral(cursor, "null"_s) : nullptr;
}

static ALWAYS_INLINE Latin1Character* tryAppendValueAtCursor(const StringBuilderStorage& builder, Latin1Character* cursor, JSValue value)
{
    if (value.isCell()) {
        JSCell* cell = value.asCell();
        const StringImpl* flatString = cell && cell->isString() ? asString(value)->tryGetValueImpl() : nullptr;
        return flatString ? tryAppendQuotedWithoutEscapes(builder, cursor, *flatString) : nullptr;
    }
    if (!builder.hasRoomAt(cursor, jsonNumberBytes + maxStringBuilderReservation)) [[unlikely]]
        return nullptr;
    return appendPrimitive(cursor, value);
}

AppendedJSONValue appendJSONValue(JSGlobalObject* globalObject, StringBuilderStorage& builder, Latin1Character* cursor, JSValue value, JSONPosition position)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSCell* cell = value.isCell() ? value.asCell() : nullptr;
    if (cell && cell->isString()) {
        auto string = asString(value)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        Latin1Character* end = appendQuotedJSONString(globalObject, builder, cursor, *string->impl());
        RETURN_IF_EXCEPTION(scope, { });
        return { end, JSONOutcome::Parsed };
    }

    builder.commit(cursor);
    cursor = builder.reserve(globalObject, jsonNumberBytes + maxStringBuilderReservation);
    RETURN_IF_EXCEPTION(scope, { });

    if (value.isUndefined() || (cell && cell->isSymbol())) {
        switch (position) {
        case JSONPosition::Root:
        case JSONPosition::Property:
            return { cursor, JSONOutcome::Rejected };
        case JSONPosition::Element:
            return { appendJSONLiteral(cursor, "null"_s), JSONOutcome::Parsed };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Latin1Character* end = appendPrimitive(cursor, value);
    if (!end)
        return { cursor, JSONOutcome::Unknown };
    return { end, JSONOutcome::Parsed };
}

static NEVER_INLINE NOT_TAIL_CALLED OperationReturnType<uint8_t*> appendJSONStringInFrame(Instance& instance, CallerFrameAndPC* frameRecord, StringBuilderStorage& builder, Latin1Character* cursor, JSString* string)
{
    AOT_OPERATION_BEGIN_IN_FRAME(&instance, frameRecord, Entry::operationAOTJSONAppendString);
    auto value = string->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, byteCast<uint8_t>(appendQuotedJSONString(globalObject, builder, cursor, *value->impl())));
}

JSC_DEFINE_JIT_OPERATION(operationAOTJSONAppendString, uint8_t*, (Instance* instance, JSCell* builderCell, uint8_t* cursorBytes, JSCell* stringCell))
{
    constexpr Entry entry = Entry::operationAOTJSONAppendString;
    StringBuilderStorage& builder = *cellArgument<StringBuilderStorage>(builderCell, entry);
    JSString* string = cellArgument<JSString>(stringCell, entry);
    Latin1Character* cursor = byteCast<Latin1Character>(cursorBytes);
    if (const StringImpl* flatString = string->tryGetValueImpl()) [[likely]] {
        if (Latin1Character* end = tryAppendQuotedWithoutEscapes(builder, cursor, *flatString)) [[likely]]
            return { byteCast<uint8_t>(end), nullptr };
    }
    return appendJSONStringInFrame(*instance, AOT_OPERATION_FRAME_RECORD(), builder, cursor, string);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONAppendInteger, uint8_t*, (uint8_t* cursor, int64_t integer))
{
    return byteCast<uint8_t>(appendJSONInteger(byteCast<Latin1Character>(cursor), integer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONAppendNumber, uint8_t*, (uint8_t* cursor, double number))
{
    return byteCast<uint8_t>(appendJSONNumber(byteCast<Latin1Character>(cursor), number));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONAppendDate, uint8_t*, (uint8_t* cursor, double timeValue))
{
    return byteCast<uint8_t>(appendJSONTimeValue(byteCast<Latin1Character>(cursor), timeValue));
}

static ALWAYS_INLINE UGPRPair toRegisterPair(AppendedJSONValue appended)
{
    return makeUGPRPair(std::bit_cast<UCPURegister>(appended.cursor), static_cast<UCPURegister>(appended.outcome));
}

static NEVER_INLINE NOT_TAIL_CALLED UGPRPair appendJSONValueInFrame(Instance& instance, CallerFrameAndPC* frameRecord, StringBuilderStorage& builder, Latin1Character* cursor, JSValue value, JSONPosition position)
{
    AOT_OPERATION_BEGIN_IN_FRAME(&instance, frameRecord, Entry::operationAOTJSONAppendValue);
    OPERATION_RETURN(scope, toRegisterPair(appendJSONValue(globalObject, builder, cursor, value, position)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTJSONAppendValue, UGPRPair, (Instance* instance, JSCell* builderCell, uint8_t* cursorBytes, EncodedJSValue encodedValue, uint32_t position))
{
    StringBuilderStorage& builder = *cellArgument<StringBuilderStorage>(builderCell, Entry::operationAOTJSONAppendValue);
    Latin1Character* cursor = byteCast<Latin1Character>(cursorBytes);
    JSValue value = JSValue::decode(encodedValue);
    if (Latin1Character* end = tryAppendValueAtCursor(builder, cursor, value)) [[likely]]
        return toRegisterPair({ end, JSONOutcome::Parsed });
    return appendJSONValueInFrame(*instance, AOT_OPERATION_FRAME_RECORD(), builder, cursor, value, static_cast<JSONPosition>(position));
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
