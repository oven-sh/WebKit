/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTJSONHooks.h"
#include "AOTJSONTextWriter.h"
#include "AOTObjectModel.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTStringBuilderStorage.h"
#include "AOTVector.h"
#include "JSCInlines.h"
#include "JSONObject.h"
#include "NumberPrototype.h"
#include "NumericStrings.h"
#include <array>
#include <span>
#include <wtf/MathExtras.h>
#include <wtf/SIMDHelpers.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/EscapedFormsForJSON.h>
#include <wtf/text/StringBuilderJSON.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static constexpr size_t numberOfQuotes = 2;
static constexpr size_t maxEscapedCharacterLength = 6;

static_assert(maxLengthOfEscapedJSONString(1) == maxEscapedCharacterLength + numberOfQuotes && maxLengthOfEscapedJSONString(0) == numberOfQuotes);

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

static constexpr size_t maxLengthOfStringCheckedFirst = SIMD::stride<uint8_t> - 1;

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

NEVER_INLINE Latin1Character* appendQuotedAndEscapedJSONString(Latin1Character* cursor, std::span<const Latin1Character> characters)
{
    *cursor++ = '"';
    std::span<Latin1Character> output { cursor, characters.size() * maxEscapedCharacterLength };
    WTF::appendEscapedJSONStringContent(output, characters);
    cursor = output.data();
    *cursor++ = '"';
    return cursor;
}

using Typed::JSONOutcome;
using Typed::JSONPosition;

static ALWAYS_INLINE size_t roomForQuotedString(size_t maxLength)
{
    return maxLength + numberOfQuotes + maxStringBuilderReservation;
}

static ALWAYS_INLINE size_t roomForEscapedString(size_t length)
{
    return maxLengthOfEscapedJSONString(length) + maxStringBuilderReservation;
}

static NEVER_INLINE Latin1Character* tryAppendEscapedAtCursor(const StringBuilderStorage& builder, Latin1Character* cursor, std::span<const Latin1Character> characters)
{
    if (!builder.hasRoomAt(cursor, roomForEscapedString(characters.size())))
        return nullptr;
    return appendQuotedAndEscapedJSONString(cursor, characters);
}

static Latin1Character* reserveForEscapedString(JSGlobalObject* globalObject, StringBuilderStorage& builder, size_t length, size_t quotes)
{
    bool isKeptByPool = roomForEscapedString(length) <= StringBuilderStorage::maxRetainedCapacity;
    size_t roomForEscapes = (maxEscapedCharacterLength - 1) * (isKeptByPool ? length : 4096);
    if (Latin1Character* cursor = builder.tryReserve({ .length = length + quotes, .maxLengthOfRest = roomForEscapes + maxStringBuilderReservation })) [[likely]]
        return cursor;
    return builder.reserve(globalObject, { .length = length + quotes, .maxLengthOfRest = maxStringBuilderReservation });
}

static NEVER_INLINE Latin1Character* appendQuotedWithEscapes(JSGlobalObject* globalObject, StringBuilderStorage& builder, std::span<const Latin1Character> rest)
{
    constexpr size_t closingQuote = 1;
    Latin1Character* cursor = reserveForEscapedString(globalObject, builder, rest.size(), numberOfQuotes);
    if (!cursor)
        return nullptr;
    *cursor++ = '"';
    while (!rest.empty()) {
        auto buffer = builder.charactersUpToLimit();
        auto room = buffer.subspan(cursor - buffer.data());
        auto piece = rest.first(std::min(rest.size(), room.size() / maxEscapedCharacterLength));
        WTF::appendEscapedJSONStringContent(room, piece);
        cursor = room.data();
        rest = rest.subspan(piece.size());
        if (builder.hasRoomAt(cursor, rest.size() + closingQuote + maxStringBuilderReservation)) [[likely]]
            continue;
        builder.commit(cursor);
        cursor = reserveForEscapedString(globalObject, builder, rest.size(), closingQuote);
        if (!cursor)
            return nullptr;
    }
    *cursor++ = '"';
    return cursor;
}

static Latin1Character* appendEscapedJSONString(JSGlobalObject* globalObject, StringBuilderStorage& builder, Latin1Character* cursor, std::span<const Latin1Character> characters)
{
    if (Latin1Character* end = tryAppendEscapedAtCursor(builder, cursor, characters))
        return end;
    builder.commit(cursor);
    return appendQuotedWithEscapes(globalObject, builder, characters);
}

static NEVER_INLINE Latin1Character* appendQuoted16Bit(JSGlobalObject* globalObject, StringBuilderStorage& builder, std::span<const char16_t> rest)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    constexpr size_t maxLengthOfPiece = 256 * KB;
    size_t maxEscapedLength = std::min(rest.size(), maxLengthOfPiece) * maxEscapedCharacterLength;
    Vector<char16_t, 64> text;
    if (!text.tryGrow(maxEscapedLength + numberOfQuotes)) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return nullptr;
    }
    std::span<char16_t> quoted = text.mutableSpan();
    quoted[0] = '"';
    std::span<char16_t> output = quoted.subspan(1, maxEscapedLength);
    while (rest.size() > maxLengthOfPiece) [[unlikely]] {
        auto piece = rest.first(maxLengthOfPiece - !!U16_IS_LEAD(rest[maxLengthOfPiece - 1]));
        WTF::appendEscapedJSONStringContent(output, piece);
        builder.append(globalObject, std::span<const char16_t> { quoted.data(), output.data() });
        RETURN_IF_EXCEPTION(scope, nullptr);
        rest = rest.subspan(piece.size());
        output = quoted.first(maxEscapedLength);
    }
    WTF::appendEscapedJSONStringContent(output, rest);
    size_t length = output.data() - quoted.data();
    quoted[length++] = '"';

    builder.append(globalObject, std::span<const char16_t> { quoted.first(length) });
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, builder.reserve(globalObject, { .length = 0, .maxLengthOfRest = maxStringBuilderReservation }));
}

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

static Latin1Character* appendQuotedJSONString(JSGlobalObject* globalObject, StringBuilderStorage& builder, Latin1Character* cursor, const StringImpl& string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!string.is8Bit()) [[unlikely]] {
        builder.commit(cursor);
        if (!WTF::charactersAreAllLatin1(string.span16()))
            RELEASE_AND_RETURN(scope, appendQuoted16Bit(globalObject, builder, string.span16()));
        String narrow = String::make8Bit(string.span16());
        RELEASE_AND_RETURN(scope, appendQuotedWithEscapes(globalObject, builder, narrow.span8()));
    }

    auto characters = string.span8();
    if (!builder.hasRoomAt(cursor, roomForQuotedString(characters.size()))) [[unlikely]] {
        builder.commit(cursor);
        cursor = reserveForEscapedString(globalObject, builder, characters.size(), numberOfQuotes);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (Latin1Character* end = tryAppendQuotedWithoutEscapes(builder, cursor, string)) [[likely]]
        return end;
    RELEASE_AND_RETURN(scope, appendEscapedJSONString(globalObject, builder, cursor, characters));
}

static ALWAYS_INLINE Latin1Character* appendJSONPrimitive(Latin1Character* cursor, JSValue value)
{
    if (value.isNumber())
        return appendJSONNumber(cursor, value.asNumber());
    if (value.isBoolean())
        return value.isTrue() ? appendJSONLiteral(cursor, "true"_s) : appendJSONLiteral(cursor, "false"_s);
    return value.isNull() ? appendJSONLiteral(cursor, "null"_s) : nullptr;
}

static ALWAYS_INLINE bool isListOfNumbers(const JSCell& cell)
{
    return (cell.type() == ArrayType || cell.type() == DerivedArrayType) && cell.isPristine() && (hasInt32(cell.indexingType()) || hasDouble(cell.indexingType()));
}

static NEVER_INLINE bool isListOfBoxedNumbers(const JSCell& cell)
{
    if ((cell.type() != ArrayType && cell.type() != DerivedArrayType) || !cell.isPristine() || !hasContiguous(cell.indexingType()))
        return false;
    const JSArray& list = *uncheckedDowncast<JSArray>(&cell);
    return std::ranges::all_of(std::span { list.butterfly()->contiguous().data(), list.length() }, [](const WriteBarrier<Unknown>& element) {
        return !element || element.get().isNumber() || element.get().isUndefined();
    });
}

static constexpr size_t bytesOfNumberInList = jsonNumberBytes + 1;

static ALWAYS_INLINE Latin1Character* appendNumbersWithCommas(Latin1Character* cursor, const JSArray& list, unsigned begin, unsigned end)
{
    const Butterfly* butterfly = list.butterfly();
    if (hasInt32(list.indexingType())) {
        const WriteBarrier<Unknown>* numbers = butterfly->contiguousInt32().data();
        for (unsigned index = begin; index < end; ++index) {
            JSValue number = numbers[index].get();
            cursor = number ? appendJSONInteger(cursor, number.asInt32()) : appendJSONLiteral(cursor, "null"_s);
            *cursor++ = ',';
        }
        return cursor;
    }
    const double* numbers = butterfly->contiguousDouble().data();
    for (unsigned index = begin; index < end; ++index) {
        cursor = appendJSONNumber(cursor, numbers[index]);
        *cursor++ = ',';
    }
    return cursor;
}

static Latin1Character* appendBoxedNumbersWithCommas(Latin1Character* cursor, const JSArray& list, unsigned begin, unsigned end)
{
    const WriteBarrier<Unknown>* numbers = list.butterfly()->contiguous().data();
    for (unsigned index = begin; index < end; ++index) {
        JSValue number = numbers[index].get();
        cursor = number && number.isNumber() ? appendJSONNumber(cursor, number.asNumber()) : appendJSONLiteral(cursor, "null"_s);
        *cursor++ = ',';
    }
    return cursor;
}

static ALWAYS_INLINE Latin1Character* closeListOfNumbers(Latin1Character* cursor, unsigned length)
{
    cursor -= !!length;
    *cursor++ = ']';
    return cursor;
}

static NEVER_INLINE Latin1Character* tryAppendListOfNumbersAtCursor(const StringBuilderStorage& builder, Latin1Character* cursor, const JSArray& list)
{
    unsigned length = list.length();
    if (!builder.hasRoomAt(cursor, 2 + static_cast<size_t>(length) * bytesOfNumberInList + maxStringBuilderReservation))
        return nullptr;
    *cursor++ = '[';
    return closeListOfNumbers(appendNumbersWithCommas(cursor, list, 0, length), length);
}

static ALWAYS_INLINE Latin1Character* tryAppendValueAtCursor(const StringBuilderStorage& builder, Latin1Character* cursor, JSValue value)
{
    if (value.isCell()) {
        JSCell* cell = value.asCell();
        const StringImpl* flatString = cell && cell->isString() ? asString(value)->tryGetValueImpl() : nullptr;
        if (flatString) [[likely]]
            return tryAppendQuotedWithoutEscapes(builder, cursor, *flatString);
        return cell && isListOfNumbers(*cell) ? tryAppendListOfNumbersAtCursor(builder, cursor, *uncheckedDowncast<JSArray>(cell)) : nullptr;
    }
    if (!builder.hasRoomAt(cursor, jsonNumberBytes + maxStringBuilderReservation)) [[unlikely]]
        return nullptr;
    return appendJSONPrimitive(cursor, value);
}

struct AppendedJSONValue {
    Latin1Character* cursor { nullptr };
    JSONOutcome outcome { JSONOutcome::Unknown };
};

static AppendedJSONValue appendJSONValue(JSGlobalObject* globalObject, StringBuilderStorage& builder, Latin1Character* cursor, JSValue value, JSONPosition position, unsigned levelsAround)
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

    bool holdsBoxedNumbers = cell && isListOfBoxedNumbers(*cell);
    if (holdsBoxedNumbers || (cell && isListOfNumbers(*cell))) {
        constexpr unsigned numbersWrittenAtOnce = 1024;
        const JSArray& list = *uncheckedDowncast<JSArray>(cell);
        unsigned length = list.length();
        builder.commit(cursor);
        cursor = builder.reserve(globalObject, { .length = 0, .maxLengthOfRest = 2 + maxStringBuilderReservation });
        RETURN_IF_EXCEPTION(scope, { });
        *cursor++ = '[';
        for (uint64_t begin = 0; begin < length; begin += numbersWrittenAtOnce) {
            uint64_t end = std::min<uint64_t>(length, begin + numbersWrittenAtOnce);
            builder.commit(cursor);
            cursor = builder.reserve(globalObject, { .length = 0, .maxLengthOfRest = (end - begin) * bytesOfNumberInList + maxStringBuilderReservation });
            RETURN_IF_EXCEPTION(scope, { });
            cursor = holdsBoxedNumbers ? appendBoxedNumbersWithCommas(cursor, list, begin, end) : appendNumbersWithCommas(cursor, list, begin, end);
        }
        return { closeListOfNumbers(cursor, length), JSONOutcome::Parsed };
    }

    if (cell && cell->isObject()) {
        Latin1Character* end = tryAppendJSONText(globalObject, builder, cursor, value, levelsAround);
        RETURN_IF_EXCEPTION(scope, { });
        return { end, end ? JSONOutcome::Parsed : JSONOutcome::Unknown };
    }

    builder.commit(cursor);
    cursor = builder.reserve(globalObject, { .length = 0, .maxLengthOfRest = jsonNumberBytes + maxStringBuilderReservation });
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

    Latin1Character* end = appendJSONPrimitive(cursor, value);
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

static_assert(std::max({ JSONPosition::Root, JSONPosition::Property, JSONPosition::Element }) < static_cast<JSONPosition>(1u << bitsOfJSONPosition));

static NEVER_INLINE NOT_TAIL_CALLED UGPRPair appendJSONValueInFrame(Instance& instance, CallerFrameAndPC* frameRecord, StringBuilderStorage& builder, Latin1Character* cursor, JSValue value, uint32_t positionAndLevels)
{
    AOT_OPERATION_BEGIN_IN_FRAME(&instance, frameRecord, Entry::operationAOTJSONAppendValue);
    OPERATION_RETURN(scope, toRegisterPair(appendJSONValue(globalObject, builder, cursor, value, static_cast<JSONPosition>(decodeJSONPosition(positionAndLevels)), decodeJSONLevelsAround(positionAndLevels))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTJSONAppendValue, UGPRPair, (Instance* instance, JSCell* builderCell, uint8_t* cursorBytes, EncodedJSValue encodedValue, uint32_t positionAndLevels))
{
    StringBuilderStorage& builder = *cellArgument<StringBuilderStorage>(builderCell, Entry::operationAOTJSONAppendValue);
    Latin1Character* cursor = byteCast<Latin1Character>(cursorBytes);
    JSValue value = JSValue::decode(encodedValue);
    if (Latin1Character* end = tryAppendValueAtCursor(builder, cursor, value)) [[likely]]
        return toRegisterPair({ end, JSONOutcome::Parsed });
    return appendJSONValueInFrame(*instance, AOT_OPERATION_FRAME_RECORD(), builder, cursor, value, positionAndLevels);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
