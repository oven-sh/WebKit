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

static NEVER_INLINE OperationReturnType<void> JIT_OPERATION_ATTRIBUTES appendAnyString(Instance*, JSCell* builder, JSCell* string);

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendString, void, (Instance* instance, JSCell* builder, JSCell* string))
{
    if (uncheckedDowncast<StringBuilderStorage>(builder)->tryAppendShortSubstring(uncheckedDowncast<JSString>(string)))
        return { nullptr };
    [[clang::musttail]] return appendAnyString(instance, builder, string);
}

static OperationReturnType<void> JIT_OPERATION_ATTRIBUTES appendAnyString(Instance* instance, JSCell* builder, JSCell* string)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStringBuilderAppendString);
    auto* storage = cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderAppendString);
    storage->append(globalObject, cellArgument<JSString>(string, Entry::operationAOTStringBuilderAppendString));
    OPERATION_RETURN(scope);
}

static void appendStringRange(JSGlobalObject* globalObject, StringBuilderStorage& storage, JSString* base, int32_t start, int32_t end, Entry entry)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (start < 0 || end < start || static_cast<unsigned>(end) > base->length()) [[unlikely]]
        crashOnWrongOperand(entry);
    unsigned length = end - start;
    if (!length)
        return;
    if (length < storage.minLengthAppendedByReference()) {
        auto view = base->view(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (view->is8Bit()) {
            storage.append(globalObject, view->span8().subspan(start, length));
            return;
        }
        // One wide character anywhere makes a whole source text wide. Most pieces of it are plain.
        std::span<const char16_t> units = view->span16().subspan(start, length);
        if (WTF::charactersAreAllLatin1(units)) {
            storage.append(globalObject, units);
            return;
        }
    }
    JSString* range = jsSubstring(globalObject, base, start, length);
    RETURN_IF_EXCEPTION(scope, void());
    storage.append(globalObject, range);
}

JSC_DEFINE_JIT_OPERATION(operationAOTStringBuilderAppendStringRange, void, (Instance* instance, JSCell* builder, JSCell* string, int32_t start, int32_t end))
{
    constexpr Entry entry = Entry::operationAOTStringBuilderAppendStringRange;
    AOT_OPERATION_BEGIN(instance, entry);
    appendStringRange(globalObject, *cellArgument<StringBuilderStorage>(builder, entry), cellArgument<JSString>(string, entry), start, end, entry);
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

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderGrow, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderGrow);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    if (!stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor))) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, byteCast<uint8_t>(stackBuilder.didChangeStorage(globalObject)));
}

static NEVER_INLINE OperationReturnType<uint8_t*> JIT_OPERATION_ATTRIBUTES appendToStorageOfStackStringBuilder(Instance*, uint8_t* builder, uint8_t* cursor, EncodedJSValue primitive);

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderAppend, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor, EncodedJSValue primitive))
{
    JSValue value = JSValue::decode(primitive);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    size_t room = byteCast<uint8_t>(stackBuilder.limit) - cursor;
    if (value.isString()) {
        JSString* string = asString(value);
        size_t length = string->length();
        if (length <= room && length < StackStringBuilder::minLengthAppendedByReference && string->is8Bit()) {
            string->resolveToBuffer(std::span<Latin1Character> { byteCast<Latin1Character>(cursor), length });
            return { cursor + length, nullptr };
        }
    } else if (value.isInt32() && static_cast<uint32_t>(value.asInt32()) < 100'000'000 && room >= sizeof(uint64_t))
        return { cursor + writeUpToEightDigits(value.asInt32(), byteCast<Latin1Character>(cursor)), nullptr };
    [[clang::musttail]] return appendToStorageOfStackStringBuilder(instance, builder, cursor, primitive);
}

static OperationReturnType<uint8_t*> JIT_OPERATION_ATTRIBUTES appendToStorageOfStackStringBuilder(Instance* instance, uint8_t* builder, uint8_t* cursor, EncodedJSValue primitive)
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStackStringBuilderAppend);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    StringBuilderStorage* storage = stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor));
    if (!storage || storage->append(globalObject, JSValue::decode(primitive)) == StringBuilderStorage::AppendResult::Failed) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    OPERATION_RETURN(scope, byteCast<uint8_t>(stackBuilder.didChangeStorage(globalObject)));
}

static NEVER_INLINE OperationReturnType<uint8_t*> JIT_OPERATION_ATTRIBUTES appendStringRangeToStorageOfStackStringBuilder(Instance*, uint8_t* builder, uint8_t* cursor, JSCell* string, int32_t start, int32_t end);

JSC_DEFINE_JIT_OPERATION(operationAOTStackStringBuilderAppendStringRange, uint8_t*, (Instance* instance, uint8_t* builder, uint8_t* cursor, JSCell* string, int32_t start, int32_t end))
{
    const StringImpl* impl = uncheckedDowncast<JSString>(string)->tryGetValueImpl();
    size_t room = byteCast<uint8_t>(reinterpret_cast<StackStringBuilder*>(builder)->limit) - cursor;
    if (impl && !impl->is8Bit() && start >= 0 && start <= end && static_cast<unsigned>(end) <= impl->length() && static_cast<size_t>(end - start) <= std::min<size_t>(room, StackStringBuilder::minLengthAppendedByReference - 1)) {
        size_t length = end - start;
        std::span<const char16_t> units = impl->span16().subspan(start, length);
        if (WTF::charactersAreAllLatin1(units)) {
            StringImpl::copyCharacters(std::span<Latin1Character> { byteCast<Latin1Character>(cursor), length }, units);
            return { cursor + length, nullptr };
        }
    }
    [[clang::musttail]] return appendStringRangeToStorageOfStackStringBuilder(instance, builder, cursor, string, start, end);
}

static OperationReturnType<uint8_t*> JIT_OPERATION_ATTRIBUTES appendStringRangeToStorageOfStackStringBuilder(Instance* instance, uint8_t* builder, uint8_t* cursor, JSCell* string, int32_t start, int32_t end)
{
    constexpr Entry entry = Entry::operationAOTStackStringBuilderAppendStringRange;
    AOT_OPERATION_BEGIN(instance, entry);
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    StringBuilderStorage* storage = stackBuilder.moveToStorage(globalObject, *instance, byteCast<Latin1Character>(cursor));
    if (!storage) [[unlikely]]
        OPERATION_RETURN(scope, nullptr);
    appendStringRange(globalObject, *storage, cellArgument<JSString>(string, entry), start, end, entry);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, byteCast<uint8_t>(stackBuilder.didChangeStorage(globalObject)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStackStringBuilderDiscard, void, (Instance* instance, uint8_t* builder))
{
    auto& stackBuilder = *reinterpret_cast<StackStringBuilder*>(builder);
    if (!stackBuilder.isOnStack())
        stackBuilder.storage->release(*instance);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTStringBuilderDiscard, void, (Instance* instance, JSCell* builder))
{
    cellArgument<StringBuilderStorage>(builder, Entry::operationAOTStringBuilderDiscard)->release(*instance);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
