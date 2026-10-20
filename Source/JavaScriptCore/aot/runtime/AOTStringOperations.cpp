/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyCaches.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "MathCommon.h"
#include "NumberPrototype.h"
#include "NumericStrings.h"
#include <array>

namespace JSC { namespace AOT {

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToLiteralString, EncodedJSValue, (Instance* instance, EncodedJSValue encoded))
{
    JSValue value = JSValue::decode(encoded);
    return value && value.isString() ? static_cast<EncodedJSValue>(instance->state->strings->literalStringWordFor(asString(value))) : encoded;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLiteralStringToString, JSCell*, (Instance* instance, uint64_t word))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTLiteralStringToString);
    OPERATION_RETURN(scope, instance->state->strings->stringForLiteralStringWord(word));
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcat, JSCell*, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcat);
    JSString* leftString = cellArgument<JSString>(left, Entry::operationAOTConcat);
    JSString* rightString = cellArgument<JSString>(right, Entry::operationAOTConcat);
    OPERATION_RETURN(scope, jsString(globalObject, leftString, rightString));
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcatThree, JSCell*, (Instance* instance, JSCell* first, JSCell* second, JSCell* third))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatThree);
    JSString* firstString = cellArgument<JSString>(first, Entry::operationAOTConcatThree);
    JSString* secondString = cellArgument<JSString>(second, Entry::operationAOTConcatThree);
    JSString* thirdString = cellArgument<JSString>(third, Entry::operationAOTConcatThree);
    OPERATION_RETURN(scope, jsString(globalObject, firstString, secondString, thirdString));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSubstring, JSCell*, (Instance* instance, JSCell* string, int32_t from, int32_t to))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTSubstring);
    OPERATION_RETURN(scope, jsSubstring(globalObject, cellArgument<JSString>(string, Entry::operationAOTSubstring), from, to - from));
}

JSC_DEFINE_JIT_OPERATION(operationAOTFlatten, void*, (Instance* instance, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTFlatten);
    auto value = cellArgument<JSString>(string, Entry::operationAOTFlatten)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, value->impl());
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareStrings, size_t, (Instance* instance, JSCell* left, JSCell* right))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCompareStrings);
    JSString* leftString = cellArgument<JSString>(left, Entry::operationAOTCompareStrings);
    JSString* rightString = cellArgument<JSString>(right, Entry::operationAOTCompareStrings);
    auto first = leftString->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto second = rightString->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    auto order = codePointCompare(StringView { first.data }, StringView { second.data });
    OPERATION_RETURN(scope, static_cast<size_t>(static_cast<int64_t>(order < 0 ? -1 : order > 0 ? 1 : 0)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringHasAt, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t position))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringHasAt);
    JSString* base = cellArgument<JSString>(string, Entry::operationAOTStringHasAt);
    JSString* argument = cellArgument<JSString>(other, Entry::operationAOTStringHasAt);
    unsigned length = base->length();
    unsigned otherLength = argument->length();
    if (position < 0 || otherLength > length || static_cast<unsigned>(position) > length - otherLength)
        OPERATION_RETURN(scope, false);
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    auto otherCharacters = argument->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, false);
    OPERATION_RETURN(scope, characters->substring(position, otherLength) == otherCharacters.data);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringIndexOfFrom, size_t, (Instance* instance, JSCell* string, JSCell* other, int32_t startPosition))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringIndexOfFrom);
    constexpr size_t none = static_cast<size_t>(-1);
    JSString* base = cellArgument<JSString>(string, Entry::operationAOTStringIndexOfFrom);
    JSString* argument = cellArgument<JSString>(other, Entry::operationAOTStringIndexOfFrom);
    unsigned position = startPosition;
    if (base->length() < static_cast<uint64_t>(argument->length()) + position)
        OPERATION_RETURN(scope, none);
    auto otherCharacters = argument->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    if (argument->length() == 1 && base->isRope() && base->length() >= JSString::minLengthForRopeWalk) {
        if (auto found = base->tryFindOneChar(globalObject, otherCharacters[0], position))
            OPERATION_RETURN(scope, *found == notFound ? none : *found);
    }
    auto characters = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0);
    size_t found = characters->find(vm.adaptiveStringSearcherTables(), otherCharacters, position);
    OPERATION_RETURN(scope, found == notFound ? none : found);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIntToString, JSCell*, (Instance* instance, uint64_t bits))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTIntToString);
    int64_t value = static_cast<int64_t>(bits);
    if (value >= static_cast<int64_t>(NumericStrings::cacheSize) && value <= INT32_MAX) {
        std::array<Latin1Character, 10> digits;
        size_t first = digits.size();
        for (uint32_t rest = static_cast<uint32_t>(value); rest; rest /= 10)
            digits[--first] = static_cast<Latin1Character>('0' + rest % 10);
        JSString* string = instance->state->propertyCaches->stringKeys().findOrAdd(vm, std::span<const Latin1Character> { digits }.subspan(first));
        vm.numericStrings.setJSString(static_cast<int32_t>(value), string->tryGetValue(), string);
        OPERATION_RETURN(scope, string);
    }
    if (value == static_cast<int32_t>(value)) {
        JSString* string = int32ToString(vm, static_cast<int32_t>(value), 10);
        string->tryGetValueImpl()->hash();
        OPERATION_RETURN(scope, string);
    }
    OPERATION_RETURN(scope, jsString(vm, String::number(static_cast<double>(value))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDoubleToString, JSCell*, (Instance* instance, double value))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTDoubleToString);
    int32_t integer = truncateDoubleToInt32(value);
    if (integer == value)
        OPERATION_RETURN(scope, int32ToString(vm, integer, 10));
    OPERATION_RETURN(scope, vm.numericStrings.addJSString(vm, value));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCharToString, JSCell*, (Instance* instance, uint32_t character))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTCharToString);
    OPERATION_RETURN(scope, jsSingleCharacterString(vm, static_cast<char16_t>(character)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
