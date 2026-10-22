/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuiltinOperationSupport.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTOperationScope.h"
#include "AOTSchemaRuntimeOperations.h"
#include "JSBigInt.h"
#include "JSArrayBufferViewInlines.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSTypedArrays.h"
#include <wtf/text/StringCommon.h>

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTNewBigInt64, JSCell*, (Instance* instance, uint64_t bits, uint32_t isSigned))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNewBigInt64);
    if (auto& data = instance->allocation.inlineAllocations[static_cast<unsigned>(InlineAllocation::BigInt)]; !data.allocator) {
        auto* allocator = subspaceFor<JSBigInt>(vm)->allocatorFor(JSBigInt::allocationSize(1), AllocatorForMode::EnsureAllocator).localAllocator();
        RELEASE_ASSERT(allocator->cellSize() == oneDigitBigIntSize);
        data.header = cellHeaderFor(vm.bigIntStructure.get());
        data.allocator = allocator;
    }
    JSBigInt* result = isSigned ? JSBigInt::createFrom(globalObject, static_cast<int64_t>(bits)) : JSBigInt::createFrom(globalObject, bits);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr);
    OPERATION_RETURN(scope, result);
}

// `Buffer.byteLength(value, encoding)` where no test stands in front of the call. Only a string names an encoding: a string beside anything else counts UTF-8, which is counted here.
JSC_DEFINE_JIT_OPERATION(operationAOTByteLengthOfAnyValue, EncodedJSValue, (Instance* instance, EncodedJSValue encodedValue, EncodedJSValue encodedEncoding))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTByteLengthOfAnyValue);
    JSValue value = JSValue::decode(encodedValue);
    JSValue encoding = JSValue::decode(encodedEncoding);
    if (value.isString() && !encoding.isString()) {
        auto view = asString(value)->view(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        size_t length = view->is8Bit() ? operationAOTStringUTF8Length8(view->span8().data(), view->length()) : operationAOTStringUTF8Length16(view->span16().data(), view->length());
        OPERATION_RETURN(scope, JSValue::encode(jsNumber(length)));
    }
    OPERATION_RETURN(scope, encodedJSValue());
}

enum class BytesAreRead : uint8_t { AsUTF8, AsASCII, AsLatin1 };

// The names as programs write them. Another spelling, another encoding and what is no string are for the host function.
static std::optional<BytesAreRead> wayToRead(JSValue encoding)
{
    if (encoding.isUndefined())
        return BytesAreRead::AsUTF8;
    const StringImpl* name = encoding.isString() ? asString(encoding)->tryGetValueImpl() : nullptr;
    if (!name)
        return std::nullopt;
    if (equal(name, "utf8"_s) || equal(name, "utf-8"_s))
        return BytesAreRead::AsUTF8;
    if (equal(name, "ascii"_s))
        return BytesAreRead::AsASCII;
    if (equal(name, "latin1"_s) || equal(name, "binary"_s))
        return BytesAreRead::AsLatin1;
    return std::nullopt;
}

// `buffer.toString(encoding, start, end)`, the function known.
JSC_DEFINE_JIT_OPERATION(operationAOTTextOfBytes, EncodedJSValue, (Instance* instance, EncodedJSValue encodedReceiver, EncodedJSValue encodedEncoding, EncodedJSValue encodedStart, EncodedJSValue encodedEnd))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTTextOfBytes);
    JSValue receiver = JSValue::decode(encodedReceiver);
    JSValue start = JSValue::decode(encodedStart);
    JSValue end = JSValue::decode(encodedEnd);
    std::optional<BytesAreRead> way = wayToRead(JSValue::decode(encodedEncoding));
    if (way && receiver.isCell() && receiver.asCell()->type() == Uint8ArrayType && (start.isUndefined() || start.isInt32()) && (end.isUndefined() || end.isInt32())) [[likely]] {
        auto* view = uncheckedDowncast<JSUint8Array>(receiver.asCell());
        int64_t length = view->lengthRaw();
        int64_t first = start.isUndefined() ? 0 : start.asInt32();
        int64_t last = end.isUndefined() ? length : end.asInt32();
        if (!isResizableOrGrowableShared(view->mode()) && !view->isDetached() && 0 <= first && first <= last && last <= length) [[likely]] {
            std::span<const Latin1Character> bytes = byteCast<Latin1Character>(std::span<const uint8_t> { view->typedVector() + first, static_cast<size_t>(last - first) });
            if (bytes.empty())
                OPERATION_RETURN(scope, JSValue::encode(jsEmptyString(vm)));
            if (*way == BytesAreRead::AsLatin1 || charactersAreAllASCII(bytes)) [[likely]] {
                if (bytes.size() == 1)
                    OPERATION_RETURN(scope, JSValue::encode(jsSingleCharacterString(vm, bytes[0])));
                if (bytes.size() <= String::MaxLength) [[likely]]
                    OPERATION_RETURN(scope, JSValue::encode(jsNontrivialString(vm, String(bytes))));
            } else if (*way == BytesAreRead::AsUTF8 && bytes.size() <= String::MaxLength) {
                // Well-formed UTF-8 has one reading. What stands for a malformed sequence is the host's to say.
                if (String text = String::fromUTF8(byteCast<char8_t>(bytes)); !text.isNull())
                    OPERATION_RETURN(scope, JSValue::encode(jsString(vm, WTF::move(text))));
            }
        }
    }
    OPERATION_RETURN(scope, encodedJSValue());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
