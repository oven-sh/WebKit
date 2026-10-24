/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONScanner.h"

#if ENABLE(AOT)

#include "AOTJSONRuntime.h"
#include "AOTJSONValueParser.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTSchemaRuntimeOperations.h"
#include "DeferTermination.h"
#include "JSCInlines.h"
#include <cmath>
#include <wtf/FastFloat.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

static bool appendPlainCharacters(const JSONWords& words, StringBuilder& builder, std::span<const uint8_t> characters)
{
    if (isLatin1(words, characters)) {
        builder.append(byteCast<Latin1Character>(characters));
        return true;
    }
    if (words.sourceEncoding == JSONSourceEncoding::UTF16) {
        builder.append(original16BitCharacters(words, characters));
        return true;
    }
    if (!StringImpl::isValidLength<char16_t>(characters.size()))
        return false;
    String decoded = String::fromUTF8(characters);
    if (decoded.isNull())
        return false;
    builder.append(decoded);
    return true;
}

const uint8_t* decodeJSONString(const JSONWords& words, const uint8_t* start, const uint8_t* cursor, StringBuilder& builder)
{
    for (;;) {
        if (!appendPlainCharacters(words, builder, { start, cursor }))
            return nullptr;
        if (*cursor == '"')
            return cursor + 1;
        unsigned length = *cursor == '\\' ? jsonEscapeLength(cursor) : 0;
        if (!length)
            return nullptr;
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
}

NEVER_INLINE ParsedJSONString makeJSONStringWithEscapes(Instance& instance, VM& vm, const uint8_t* afterQuote, const uint8_t* firstSpecial)
{
    StringBuilder builder(OverflowPolicy::RecordOverflow);
    const uint8_t* end = decodeJSONString(instance.json, afterQuote, firstSpecial, builder);
    if (!end || builder.hasOverflowed())
        return { jsEmptyString(vm) };
    JSString* string = builder.is8Bit() ? vm.jsonCache().tryMakeJSString(vm, builder.span8()) : vm.jsonCache().tryMakeJSString(vm, builder.span16());
    if (!string)
        return { jsEmptyString(vm) };
    return { string, end };
}

NEVER_INLINE ParsedJSONString makeUncachedJSONString(Instance& instance, VM& vm, const uint8_t* start)
{
    ASSERT(start[-1] == '"');
    const JSONWords& words = instance.json;
    const uint8_t* cursor = findEndOfPlainJSONCharacters(start);
    if (*cursor != '"') [[unlikely]]
        return makeJSONStringWithEscapes(instance, vm, start, cursor);
    if (!isLatin1(words, { start, cursor })) [[unlikely]] {
        if (words.sourceEncoding != JSONSourceEncoding::UTF16)
            return makeJSONStringWithEscapes(instance, vm, start, cursor);
        auto characters = original16BitCharacters(words, { start, cursor });
        std::span<char16_t> copy;
        auto impl = StringImpl::tryCreateUninitialized(characters.size(), copy);
        if (!impl)
            return { jsEmptyString(vm) };
        memcpy(copy.data(), characters.data(), characters.size_bytes());
        return { jsString(vm, String { impl.releaseNonNull() }), cursor + 1 };
    }
    JSString* string = vm.jsonCache().tryMakeJSString(vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(cursor) });
    if (!string) [[unlikely]]
        return { jsEmptyString(vm) };
    // Only a source that is no Latin-1 text asks, so only there is it worth finding out.
    bool textIsASCII = words.sourceEncoding != JSONSourceEncoding::Latin1 && charactersAreAllASCII(std::span<const uint8_t> { start, cursor });
    vm.jsonCache().addString(std::span<const Latin1Character> { byteCast<Latin1Character>(start - 1), jsonSourcePadding }, cursor + 2 - start, string, textIsASCII);
    return { string, cursor + 1 };
}

static NEVER_INLINE NOT_TAIL_CALLED ParsedJSONString makeUncachedJSONStringInFrame(Instance& instance, CallerFrameAndPC* frameRecord, const uint8_t* start)
{
    AOT_OPERATION_BEGIN_IN_FRAME(&instance, frameRecord, Entry::operationAOTJSONParseString);
    return makeUncachedJSONString(instance, vm, start);
}

NEVER_INLINE ParsedJSONNumber parseJSONNumberSlow(const uint8_t* start, const uint8_t* sourceEnd)
{
    size_t length = 0;
    auto number = WTF::parseJSONDouble(std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(sourceEnd) }, length);
    if (!number || !std::isfinite(*number))
        return { };
    return { *number, start + length };
}

static ALWAYS_INLINE UGPRPair toRegisterPair(ParsedJSONString parsed)
{
    return encodeResult(parsed.string, parsed.end);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONSourceOpen, size_t, (Instance* instance, EncodedJSValue text, void* stackPointer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONSourceOpen);
    OPERATION_RETURN(scope, instance->json.runtime->open(JSValue::decode(text), stackPointer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONSourceClose, size_t, (Instance* instance, const uint8_t* cursor))
{
    return instance->json.runtime->close(cursor);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONSourceAbandon, void, (Instance* instance))
{
    instance->json.runtime->abandon();
}

JSC_DEFINE_JIT_OPERATION(operationAOTJSONDecodeBytes, EncodedJSValue, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONDecodeBytes);
    OPERATION_RETURN(scope, JSValue::encode(decodeJSONBody(globalObject, JSValue::decode(value))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseString, UGPRPair, (Instance* instance, const uint8_t* cursor))
{
    VM& vm = *instance->vm;
    if (*cursor != '"') [[unlikely]]
        return toRegisterPair({ jsEmptyString(vm) });
    if (auto cached = findCachedJSONString(instance->json, vm, cursor)) [[likely]]
        return toRegisterPair(*cached);
    return toRegisterPair(makeUncachedJSONStringInFrame(*instance, AOT_OPERATION_FRAME_RECORD(), cursor + 1));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseKey, UGPRPair, (Instance* instance, const uint8_t* cursor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONParseKey);
    if (*cursor != '"') [[unlikely]]
        OPERATION_RETURN(scope, toRegisterPair({ jsEmptyString(vm) }));
    const uint8_t* start = cursor + 1;
    const uint8_t* end = findEndOfPlainJSONCharacters(start);
    if (*end != '"') [[unlikely]]
        OPERATION_RETURN(scope, toRegisterPair({ jsEmptyString(vm) }));
    if (!isLatin1(instance->json, { start, end })) [[unlikely]]
        OPERATION_RETURN(scope, toRegisterPair(makeJSONStringWithEscapes(*instance, vm, start, end)));
    JSString* string = vm.jsonCache().tryMakeJSString(vm, std::span<const Latin1Character> { byteCast<Latin1Character>(start), byteCast<Latin1Character>(end) });
    if (!string) [[unlikely]]
        OPERATION_RETURN(scope, toRegisterPair({ jsEmptyString(vm) }));
    OPERATION_RETURN(scope, toRegisterPair({ string, end + 1 }));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseValue, UGPRPair, (Instance* instance, const uint8_t* cursor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTJSONParseValue);
    DeferTerminationForAWhile deferTermination(vm);
    ParsedJSONValue parsed = parseJSONValueAt(*instance, vm, globalObject, cursor);
    OPERATION_RETURN(scope, makeUGPRPair(static_cast<UCPURegister>(JSValue::encode(parsed.value)), std::bit_cast<UCPURegister>(parsed.end)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseNumber, UGPRPair, (Instance* instance, const uint8_t* cursor))
{
    ParsedJSONNumber parsed = parseJSONNumber(cursor, instance->json.sourceEnd);
    return makeUGPRPair(std::bit_cast<uint64_t>(parsed.number), std::bit_cast<UCPURegister>(parsed.end));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONParseInteger, UGPRPair, (Instance*, const uint8_t* cursor))
{
    ParsedJSONInteger parsed = parseJSONInteger(cursor);
    return makeUGPRPair(static_cast<uint64_t>(parsed.integer), std::bit_cast<UCPURegister>(parsed.end));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONFindMember, UGPRPair, (Instance* instance, const uint8_t* cursor, uint32_t name))
{
    FoundJSONMember found = findJSONMember(instance->json, cursor, [&](StringView text) {
        return ProgramStrings::hasText(*instance->vm->aotState().image, StringID(name), text);
    });
    return makeUGPRPair(static_cast<UCPURegister>(found.search), std::bit_cast<UCPURegister>(found.value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONMemberRepeats, size_t, (Instance* instance, const uint8_t* valueStart, uint32_t name))
{
    return jsonMemberRepeats(instance->json, skipJSONValue(valueStart), [&](StringView text) {
        return ProgramStrings::hasText(*instance->vm->aotState().image, StringID(name), text);
    });
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTJSONKeyRepeats, size_t, (Instance* instance, const uint8_t* keyStart, const uint8_t* valueStart))
{
    return jsonKeyRepeats(instance->json, keyStart, valueStart);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
