/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStringBuilderStorage.h"
#include "AOTTextsOfFewDigits.h"
#include "JSCInlines.h"
#include "JSGlobalObjectFunctions.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "ParseInt.h"
#include <array>
#include <bit>
#include <charconv>
#include <climits>
#include <wtf/Float16.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/dtoa.h>

namespace JSC { namespace AOT {

static JSString* stringOfInt32WithRadix(VM& vm, int32_t value, uint32_t radix)
{
    static constexpr std::array<Latin1Character, 36> digits { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z' };
    static constexpr unsigned maxDigitsInWord = sizeof(uint64_t);
    static constexpr unsigned maxWords = sizeof(uint32_t) * CHAR_BIT / maxDigitsInWord;
    uint32_t left = static_cast<uint32_t>(value);
    if (value < 0 || radix == 10) [[unlikely]]
        return int32ToString(vm, value, radix);
    if (left < radix)
        return vm.smallStrings.singleCharacterString(digits[left]);

    std::array<uint64_t, maxWords> words;
    unsigned numberOfWords = 0;
    unsigned digitsInWord = 0;
    auto takeDigits = [&](NOESCAPE const auto& takeDigit) ALWAYS_INLINE_LAMBDA {
        do {
            uint64_t word = 0;
            digitsInWord = 0;
            do
                word = word << CHAR_BIT | digits[takeDigit()];
            while (++digitsInWord < maxDigitsInWord && left);
            words[numberOfWords++] = word;
        } while (left);
    };
    if (std::has_single_bit(radix)) {
        unsigned bitsOfDigit = std::countr_zero(radix);
        takeDigits([&] {
            uint32_t digit = left & (radix - 1);
            left >>= bitsOfDigit;
            return digit;
        });
    } else {
        takeDigits([&] {
            uint32_t digit = left % radix;
            left /= radix;
            return digit;
        });
    }

    std::span<Latin1Character> characters;
    Ref impl = StringImpl::createUninitialized((numberOfWords - 1) * maxDigitsInWord + digitsInWord, characters);
    uint64_t firstDigits = words[--numberOfWords];
    if (digitsInWord == 1)
        characters[0] = static_cast<Latin1Character>(firstDigits);
    else
        WTF::copySmallMemory<sizeof(uint16_t), maxDigitsInWord>(byteCast<uint8_t>(characters.data()), reinterpret_cast<const uint8_t*>(&firstDigits), digitsInWord);
    for (characters = characters.subspan(digitsInWord); numberOfWords; characters = characters.subspan(maxDigitsInWord))
        WTF::unalignedStore<uint64_t>(characters.data(), words[--numberOfWords]);
    return jsNontrivialString(vm, String { WTF::move(impl) });
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNumberToStringWithRadix, JSCell*, (Instance* instance, double value, int32_t radix))
{
    RELEASE_ASSERT(radix >= 2 && radix <= 36);
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNumberToStringWithRadix);
    if (int32_t integer = truncateDoubleToInt32(value); integer == value)
        OPERATION_RETURN(scope, stringOfInt32WithRadix(vm, integer, radix));
    OPERATION_RETURN(scope, numberToString(vm, value, radix));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTInt32ToStringWithRadix, JSCell*, (Instance* instance, int32_t value, int32_t radix))
{
    RELEASE_ASSERT(radix >= 2 && radix <= 36);
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInt32ToStringWithRadix);
    OPERATION_RETURN(scope, stringOfInt32WithRadix(vm, value, radix));
}

#define AOT_DEFINE_MATH_FUNCTION(capitalizedName, lowerName) \
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMath ## capitalizedName, double, (double x)) \
{ \
    return Math::lowerName(x); \
}
FOR_EACH_ARITH_UNARY_OP(AOT_DEFINE_MATH_FUNCTION)
#undef AOT_DEFINE_MATH_FUNCTION

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMathATan2, double, (double y, double x))
{
    return atan2(y, x);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMathHypot, double, (double x, double y))
{
    if (std::isinf(x) || std::isinf(y))
        return std::numeric_limits<double>::infinity();
    return std::hypot(x, y);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMathHypot3, double, (double x, double y, double z))
{
    if (std::isinf(x) || std::isinf(y) || std::isinf(z))
        return std::numeric_limits<double>::infinity();
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200100
    return std::hypotl(std::hypotl(x, y), z);
#else
    if (std::isnan(x) || std::isnan(y) || std::isnan(z))
        return PNaN;
    return std::hypot(x, y, z);
#endif
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMathF16Round, double, (double x))
{
    return Float16 { x };
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTParseIntNumber, double, (double number))
{
    if (std::optional<double> parsed = parseIntDouble(number)) [[likely]]
        return *parsed;
    NumberToStringBuffer buffer;
    return parseInt(byteCast<Latin1Character>(std::span<const char>(numberToStringAndSize(number, buffer))), 10);
}

static NEVER_INLINE double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES parseInt8Slow(const Latin1Character* characters, uint32_t length, int32_t radix)
{
    return parseInt(unsafeMakeSpan(characters, length), radix);
}

extern "C" double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTParseInt8(const Latin1Character* characters, uint32_t length, int32_t radix)
{
    if (!radix || radix == 10) {
        if (std::optional<double> value = valueOfFewDigits(unsafeMakeSpan(characters, length)))
            return *value;
    }
    [[clang::musttail]] return parseInt8Slow(characters, length, radix);
}

extern "C" double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTParseInt16(const char16_t* characters, uint32_t length, int32_t radix)
{
    return parseInt(unsafeMakeSpan(characters, length), radix);
}

static NEVER_INLINE double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES stringToNumber8Slow(const Latin1Character* characters, uint32_t length)
{
    return jsToNumber(StringView { unsafeMakeSpan(characters, length) });
}

extern "C" double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringToNumber8(const Latin1Character* characters, uint32_t length)
{
    if (std::optional<double> value = numberOfShortText(unsafeMakeSpan(characters, length)))
        return *value;
    [[clang::musttail]] return stringToNumber8Slow(characters, length);
}

extern "C" double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringToNumber16(const char16_t* characters, uint32_t length)
{
    return jsToNumber(StringView { unsafeMakeSpan(characters, length) });
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES numberOfOtherValue(Instance* instance, EncodedJSValue encodedValue)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNumberOfAnyValue);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isCell() && value.asCell()->type() == CompiledObjectType) {
        value = primitiveIfKnown(instance, value, PreferNumber);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    if (value.isString()) {
        auto view = asString(value)->view(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(jsToNumber(view))));
    }
    JSValue numeric = timeIfDate(value).toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isNumber())
        OPERATION_RETURN(scope, JSValue::encode(numeric));
    ASSERT(numeric.isBigInt());
    OPERATION_RETURN(scope, JSValue::encode(JSBigInt::toNumber(numeric)));
}

static NEVER_INLINE OperationReturnType<EncodedJSValue> JIT_OPERATION_ATTRIBUTES numberOfTextOrOtherValue(Instance* instance, EncodedJSValue encodedValue)
{
    if (JSValue value = JSValue::decode(encodedValue); value.isString()) {
        auto characters = latin1CharactersWithoutResolving(asString(value));
        if (std::optional<double> number = characters ? numberOfShortText(*characters) : std::nullopt)
            return { JSValue::encode(jsNumber(*number)), nullptr };
    }
    AOT_MUST_TAIL_CALL return numberOfOtherValue(instance, encodedValue);
}

JSC_DEFINE_JIT_OPERATION(operationAOTNumberOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    if (std::optional<double> number = valueOfTextOfFewDigits(JSValue::decode(encodedValue)))
        return { JSValue::encode(jsNumber(*number)), nullptr };
    AOT_MUST_TAIL_CALL return numberOfTextOrOtherValue(instance, encodedValue);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

JSC_DEFINE_JIT_OPERATION(operationAOTAppendNumberWithRadix, void, (Instance* instance, JSCell* builder, double value, int32_t radix))
{
    constexpr Entry entry = Entry::operationAOTAppendNumberWithRadix;
    AOT_OPERATION_BEGIN(instance, entry);
    std::array<char, 33> digits;
    NumberToStringBuffer decimalBuffer;
    RadixBuffer buffer;
    std::span<const Latin1Character> characters;
    if (int32_t integer = truncateDoubleToInt32(value); integer == value)
        characters = byteCast<Latin1Character>(std::span<const char> { digits.data(), std::to_chars(digits.data(), digits.data() + digits.size(), integer, radix).ptr });
    else if (radix == 10 || !std::isfinite(value))
        characters = byteCast<Latin1Character>(numberToStringAndSize(value, decimalBuffer));
    else
        characters = byteCast<Latin1Character>(unsafeSpan(toStringWithRadixInternal(buffer, value, radix)));
    cellArgument<StringBuilderStorage>(builder, entry)->append(globalObject, characters);
    OPERATION_RETURN(scope);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
