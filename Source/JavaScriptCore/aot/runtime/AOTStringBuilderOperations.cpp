/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSchemaRuntimeOperations.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableBindings.h"
#include "AOTInstance.h"
#include "AOTJSONHooks.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTRuntimeCounters.h"
#include "AOTStringBuilderStorage.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

static ALWAYS_INLINE StringBuilderStorage::StringUse stringUse(uint32_t isLikelyKey)
{
    return isLikelyKey ? StringBuilderStorage::StringUse::LikelyKey : StringBuilderStorage::StringUse::Unknown;
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringBuilderBegin, JSCell*, (Instance* instance))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderBegin);
    OPERATION_RETURN(scope, StringBuilderStorage::acquire(*instance));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppend, void, (Instance* instance, JSCell* builder, uint32_t count, const EncodedJSValue* parts))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderAppend);
    cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderAppend)->append(globalObject, unsafeMakeSpan(parts, count));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendString, void, (Instance* instance, JSCell* builder, JSCell* string))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderAppendString);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderAppendString);
    storage->append(globalObject, cellArgument<JSString>(string, Entry::operationAOTStringBuilderAppendString));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendStringRange, void, (Instance* instance, JSCell* builder, JSCell* string, int32_t start, int32_t end))
{
    constexpr Entry entry = Entry::operationAOTStringBuilderAppendStringRange;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* storage = cellArgument<StringBuilderStorage>(builder, entry);
    JSString* base = cellArgument<JSString>(string, entry);
    if (start < 0 || end < start || static_cast<unsigned>(end) > base->length()) [[unlikely]]
        crashOnWrongOperand(entry);
    unsigned length = end - start;
    if (!length)
        OPERATION_RETURN(scope);
    if (length >= storage->minLengthAppendedByReference() || !base->is8Bit()) {
        JSString* range = jsSubstring(globalObject, base, start, length);
        OPERATION_RETURN_IF_EXCEPTION(scope);
        storage->append(globalObject, range);
        OPERATION_RETURN(scope);
    }
    auto view = base->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    storage->append(globalObject, view->span8().subspan(start, length));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendByHost, void, (Instance* instance, JSCell* builder, uint32_t builtin, EncodedJSValue thisValue, uint32_t count, const EncodedJSValue* arguments))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderAppendByHost);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderAppendByHost);
    auto function = reinterpret_cast<HostAppendForm>(const_cast<void*>(BuiltinBindings::appendFunctionOf(BuiltinID(builtin))));
    RELEASE_ASSERT(function);
    uint32_t checkpoint = storage->length();
    if (!function(globalObject, storage, thisValue, arguments, count))
        storage->rollBack(checkpoint);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendJSON, size_t, (Instance* instance, JSCell* builder, EncodedJSValue value, EncodedJSValue encodedProperty))
{
    constexpr Entry entry = Entry::operationAOTStringBuilderAppendJSON;
    AOT_OPERATION_BEGIN(instance, entry);
    auto* storage = cellArgument<StringBuilderStorage>(builder, entry);
    JSValue property = JSValue::decode(encodedProperty);
    if (!property.isNumber() && !property.isString()) [[unlikely]]
        crashOnWrongOperand(entry);
    uint32_t checkpoint = storage->length();
    bool isAppended = appendJSONTextOfProperty(globalObject, *storage, JSValue::decode(value), property);
    if (!isAppended)
        storage->rollBack(checkpoint);
    OPERATION_RETURN(scope, isAppended);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderReserve, uint8_t*, (Instance* instance, JSCell* builder, uint64_t size))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderReserve);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderReserve);
    OPERATION_RETURN(scope, byteCast<uint8_t>(storage->reserve(globalObject, size)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringBuilderRollBack, void, (Instance*, JSCell* builder, uint32_t checkpoint))
{
    constexpr Entry entry = Entry::operationAOTStringBuilderRollBack;
    auto* storage = cellArgument<StringBuilderStorage>(builder, entry);
    if (checkpoint > storage->length()) [[unlikely]]
        crashOnWrongOperand(entry);
    storage->rollBack(checkpoint);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderToString, JSCell*, (Instance* instance, JSCell* builder, uint32_t isLikelyKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderToString);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderToString);
    OPERATION_RETURN(scope, storage->toString(globalObject, *instance, stringUse(isLikelyKey)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderFinish, JSCell*, (Instance* instance, JSCell* builder, uint32_t isLikelyKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderFinish);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderFinish);
    OPERATION_RETURN(scope, storage->finish(globalObject, *instance, stringUse(isLikelyKey)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringBuilderDiscard, void, (Instance* instance, JSCell* builder))
{
    cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderDiscard)->release(*instance);
}

JSC_DEFINE_JIT_OPERATION(operationAOTConcatenate, JSCell*, (Instance* instance, uint32_t count, const EncodedJSValue* parts, uint32_t isLikelyKey))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTConcatenate);
    OPERATION_RETURN(scope, StringBuilderStorage::concatenate(globalObject, *instance, unsafeMakeSpan(parts, count), stringUse(isLikelyKey)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
