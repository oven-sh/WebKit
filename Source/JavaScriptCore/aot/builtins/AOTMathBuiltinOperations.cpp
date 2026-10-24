/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTStringBuilderStorage.h"
#include "JSCInlines.h"
#include "JSGlobalObjectFunctions.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "ParseInt.h"
#include <array>
#include <charconv>
#include <wtf/Float16.h>
#include <wtf/dtoa.h>

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTNumberToStringWithRadix, JSCell*, (Instance* instance, double value, int32_t radix))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNumberToStringWithRadix);
    if (int32_t integer = truncateDoubleToInt32(value); integer == value)
        OPERATION_RETURN(scope, int32ToString(vm, integer, radix));
    OPERATION_RETURN(scope, numberToString(vm, value, radix));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTInt32ToStringWithRadix, JSCell*, (Instance* instance, int32_t value, int32_t radix))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTInt32ToStringWithRadix);
    OPERATION_RETURN(scope, int32ToString(vm, value, radix));
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

static ALWAYS_INLINE std::optional<double> valueOfFewDigits(std::span<const Latin1Character> characters)
{
    constexpr size_t maxNumberOfDigits = 15;
    if (characters.empty() || characters.size() > maxNumberOfDigits)
        return std::nullopt;
    uint64_t whole = 0;
    for (Latin1Character character : characters) {
        unsigned digit = character - '0';
        if (digit > 9)
            return std::nullopt;
        whole = whole * 10 + digit;
    }
    return static_cast<double>(whole);
}

extern "C" double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTParseInt8(const Latin1Character* characters, uint32_t length, int32_t radix)
{
    if (!radix || radix == 10) {
        if (std::optional<double> value = valueOfFewDigits(unsafeMakeSpan(characters, length)))
            return *value;
    }
    return parseInt(unsafeMakeSpan(characters, length), radix);
}

extern "C" double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTParseInt16(const char16_t* characters, uint32_t length, int32_t radix)
{
    return parseInt(unsafeMakeSpan(characters, length), radix);
}

extern "C" double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringToNumber8(const Latin1Character* characters, uint32_t length)
{
    if (std::optional<double> value = valueOfFewDigits(unsafeMakeSpan(characters, length)))
        return *value;
    return jsToNumber(StringView { unsafeMakeSpan(characters, length) });
}

extern "C" double AOT_PRESERVE_MOST_LEAF_ATTRIBUTES operationAOTStringToNumber16(const char16_t* characters, uint32_t length)
{
    return jsToNumber(StringView { unsafeMakeSpan(characters, length) });
}

JSC_DEFINE_JIT_OPERATION(operationAOTNumberOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNumberOfAnyValue);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isString()) {
        auto view = asString(value)->view(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (view->is8Bit()) {
            if (std::optional<double> few = valueOfFewDigits(view->span8()))
                OPERATION_RETURN(scope, JSValue::encode(jsNumber(*few)));
        }
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(jsToNumber(view))));
    }
    JSValue numeric = value.toNumeric(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (numeric.isNumber())
        OPERATION_RETURN(scope, JSValue::encode(numeric));
    ASSERT(numeric.isBigInt());
    OPERATION_RETURN(scope, JSValue::encode(JSBigInt::toNumber(numeric)));
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
