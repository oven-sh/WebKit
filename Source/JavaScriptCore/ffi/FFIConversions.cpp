/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FFIConversions.h"

#if USE(BUN_JSC_ADDITIONS)

#if USE(JSVALUE64)

#include "CallFrame.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "FFIContext.h"
#include "FFIType.h"
#include "FrameTracers.h"
#include "JSArrayBuffer.h"
#include "JSArrayBufferView.h"
#include "JSBigInt.h"
#include "JSBigIntInlines.h"
#include "JSCInlines.h"
#include "JSCJSValueInlines.h"
#include "JSFFICallback.h"
#include "JSGlobalObject.h"
#include "JSString.h"
#include "PureNaN.h"
#include <bit>
#include <cmath>
#include <limits>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringImpl.h>

#if CPU(X86_64)
#include <emmintrin.h>
#elif CPU(ARM64)
#include <arm_neon.h>
#endif

namespace JSC { namespace FFI {

JSValue pointerToJSValue(JSGlobalObject* globalObject, uint64_t address)
{
    if (!address)
        return jsNull();
    if (address <= static_cast<uint64_t>(9007199254740991ULL))
        return jsNumber(static_cast<double>(address));
    return JSBigInt::createFrom(globalObject, address);
}

static constexpr int64_t maxInt52 = 9007199254740991;

int64_t doubleToInt64(double value)
{
#if CPU(X86_64)
    return _mm_cvttsd_si64(_mm_set_sd(value));
#elif CPU(ARM64)
    return vcvtd_s64_f64(value);
#else
    if (std::isnan(value))
        return 0;
    if (value >= 9223372036854775808.0)
        return std::numeric_limits<int64_t>::max();
    if (value <= -9223372036854775808.0)
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(value);
#endif
}

static bool throwCannotConvert(JSGlobalObject* globalObject, ThrowScope& scope, Type type)
{
    throwTypeError(globalObject, scope, makeString("bun:ffi cannot convert argument to '"_s, name(type), '\''));
    return false;
}

static bool writeIntegerSlot(JSGlobalObject* globalObject, Type type, JSValue value, uint64_t& slotOut)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (type == Type::Bool) {
        slotOut = value.toBoolean(globalObject) ? 1 : 0;
        return true;
    }

    if (value.isBigInt()) [[unlikely]] {
        uint64_t bits = JSBigInt::toBigUInt64(value);
        switch (type) {
        case Type::Char:
        case Type::Int8:
            slotOut = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(bits)));
            return true;
        case Type::Uint8:
            slotOut = static_cast<uint64_t>(static_cast<uint8_t>(bits));
            return true;
        case Type::Int16:
            slotOut = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(bits)));
            return true;
        case Type::Uint16:
            slotOut = static_cast<uint64_t>(static_cast<uint16_t>(bits));
            return true;
        case Type::Int32:
            slotOut = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(bits)));
            return true;
        case Type::Uint32:
            slotOut = static_cast<uint64_t>(static_cast<uint32_t>(bits));
            return true;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return false;
        }
    }

    if (value.isString() || value.isSymbol()) [[unlikely]]
        return throwCannotConvert(globalObject, scope, type);

    if (type == Type::Uint32) {
        uint32_t truncated = value.toUInt32(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        slotOut = static_cast<uint64_t>(truncated);
        return true;
    }

    int32_t truncated = value.toInt32(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    switch (type) {
    case Type::Char:
    case Type::Int8:
        slotOut = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(truncated)));
        break;
    case Type::Uint8:
        slotOut = static_cast<uint64_t>(static_cast<uint8_t>(truncated));
        break;
    case Type::Int16:
        slotOut = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(truncated)));
        break;
    case Type::Uint16:
        slotOut = static_cast<uint64_t>(static_cast<uint16_t>(truncated));
        break;
    case Type::Int32:
        slotOut = static_cast<uint64_t>(static_cast<int64_t>(truncated));
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }
    return true;
}

static bool writeInt64Slot(JSGlobalObject* globalObject, Type type, JSValue value, uint64_t& slotOut)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    bool isUnsigned = type == Type::Uint64 || type == Type::Uint64Fast;
    if (value.isInt32()) {
        slotOut = static_cast<uint64_t>(static_cast<int64_t>(value.asInt32()));
        return true;
    }
    if (value.isDouble()) {
        slotOut = isUnsigned ? doubleToUInt64(value.asDouble()) : static_cast<uint64_t>(doubleToInt64(value.asDouble()));
        return true;
    }
    if (value.isBigInt()) {
        slotOut = isUnsigned ? JSBigInt::toBigUInt64(value) : static_cast<uint64_t>(JSBigInt::toBigInt64(value));
        return true;
    }
    return throwCannotConvert(globalObject, scope, type);
}

static bool writeFloatingPointSlot(JSGlobalObject* globalObject, Type type, JSValue value, uint64_t& slotOut)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isString() || value.isSymbol()) [[unlikely]]
        return throwCannotConvert(globalObject, scope, type);

    double number;
    if (value.isNumber())
        number = value.asNumber();
    else if (value.isUndefined())
        number = PNaN; // Number(undefined) === NaN.
    else if (value.isBigInt())
        number = JSBigInt::toNumber(value).asNumber();
    else {
        number = value.toNumber(globalObject); // strings, booleans, null, objects; Symbols throw.
        RETURN_IF_EXCEPTION(scope, false);
    }

    if (type == Type::Double) {
        slotOut = std::bit_cast<uint64_t>(number);
        return true;
    }

    ASSERT(type == Type::Float);
    float narrowed = static_cast<float>(number); // Math.fround semantics.
    slotOut = static_cast<uint64_t>(std::bit_cast<uint32_t>(narrowed));
    return true;
}

static bool writeCStringSlot(JSGlobalObject* globalObject, FFIContext& context, JSString* jsString, uint64_t& slotOut, StringArena* arena)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto string = jsString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);

    if (!arena) {
        throwTypeError(globalObject, scope, "bun:ffi: a JavaScript string is not valid here; return it from a 'cstring'-returning callback, or pass a pointer/TypedArray"_s);
        return false;
    }
    StringArena& targetArena = *arena;
    ASSERT_WITH_MESSAGE(targetArena.depth(), "bun:ffi cstring conversion requires an active FFI arena bracket");

    StringImpl* impl = string->impl();
    if (!impl || !impl->length()) {
        auto storage = targetArena.allocate(1);
        if (storage.empty()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return false;
        }
        storage[0] = '\0';
        slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(storage.data()));
        return true;
    }

    if (impl->is8Bit()) {
        auto characters = impl->span8();
        if (charactersAreAllASCII(characters)) {
            auto storage = targetArena.allocate(characters.size() + 1);
            if (storage.empty()) [[unlikely]] {
                throwOutOfMemoryError(globalObject, scope);
                return false;
            }
            memcpySpan(storage, characters);
            storage[characters.size()] = '\0';
            slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(storage.data()));
            return true;
        }
    }

    const CString* utf8 = context.cachedUTF8(*impl);
    if (!utf8) {
        auto result = impl->tryGetUTF8();
        if (!result) [[unlikely]] {
            if (result.error() == UTF8ConversionError::OutOfMemory)
                throwOutOfMemoryError(globalObject, scope);
            else
                throwTypeError(globalObject, scope, "bun:ffi could not encode the string argument as UTF-8"_s);
            return false;
        }
        utf8 = &context.cacheUTF8(*impl, WTF::move(result.value()));
    }

    auto bytes = utf8->span();
    auto storage = targetArena.allocate(bytes.size() + 1);
    if (storage.empty()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return false;
    }
    memcpySpan(storage, bytes);
    storage[bytes.size()] = '\0';
    slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(storage.data()));
    return true;
}

static bool writeBufferLengthSlot(JSGlobalObject* globalObject, JSValue value, uint64_t& slotOut)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (auto* view = dynamicDowncast<JSArrayBufferView>(value)) {
        slotOut = static_cast<uint64_t>(view->byteLength());
        return true;
    }
    throwTypeError(globalObject, scope, "bun:ffi 'buffer_length' argument must be a TypedArray or DataView"_s);
    return false;
}

static bool writePointerSlot(JSGlobalObject* globalObject, FFIContext& context, Type type, JSValue value, uint64_t& slotOut, StringArena* arena)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (type == Type::Buffer) {
        if (auto* view = dynamicDowncast<JSArrayBufferView>(value)) {
            slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view->vector()));
            return true;
        }
        throwTypeError(globalObject, scope, "bun:ffi 'buffer' argument must be a TypedArray or DataView"_s);
        return false;
    }

    if (value.isUndefinedOrNull()) {
        if (type == Type::Function) [[unlikely]] {
            throwTypeError(globalObject, scope, "bun:ffi: expected a callback (a JSCallback or an FFI function) but got undefined/null"_s);
            return false;
        }
        slotOut = 0;
        return true;
    }

    if (value.isInt32()) {
        slotOut = static_cast<uint64_t>(static_cast<uintptr_t>(static_cast<intptr_t>(value.asInt32())));
        return true;
    }

    if (value.isDouble()) {
        slotOut = static_cast<uint64_t>(static_cast<uintptr_t>(doubleToInt64(value.asDouble())));
        return true;
    }

    if (value.isBigInt()) {
        slotOut = JSBigInt::toBigUInt64(value);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }

    if (value.isCell()) {
        JSCell* cell = value.asCell();

        if (auto* view = dynamicDowncast<JSArrayBufferView>(cell)) {
            slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view->vector()));
            return true;
        }

        if (auto* buffer = dynamicDowncast<JSArrayBuffer>(cell)) {
            slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buffer->impl()->data()));
            return true;
        }

        if (auto* callback = dynamicDowncast<JSFFICallback>(cell)) {
            slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(callback->nativeEntrypoint()));
            return true;
        }

        if (cell->isString()) {
            if (type == Type::CString)
                RELEASE_AND_RETURN(scope, writeCStringSlot(globalObject, context, uncheckedDowncast<JSString>(cell), slotOut, arena));
            throwTypeError(globalObject, scope, "To convert a string to a pointer, encode it as a buffer"_s);
            return false;
        }

        if (type != Type::Buffer && cell->isObject()) {
            JSValue ptrValue = uncheckedDowncast<JSObject>(cell)->get(globalObject, Identifier::fromString(vm, "ptr"_s));
            RETURN_IF_EXCEPTION(scope, false);
            if (ptrValue.isNumber() || ptrValue.isBigInt())
                RELEASE_AND_RETURN(scope, writePointerSlot(globalObject, context, type, ptrValue, slotOut, arena));
        }
    }

    return throwCannotConvert(globalObject, scope, type);
}

bool writeSlotFromJSValue(JSGlobalObject* globalObject, FFIContext& context, Type type, JSValue value, uint64_t& slotOut, StringArena* arena)
{
    switch (type) {
    case Type::Char:
    case Type::Int8:
    case Type::Uint8:
    case Type::Int16:
    case Type::Uint16:
    case Type::Int32:
    case Type::Uint32:
    case Type::Bool:
        return writeIntegerSlot(globalObject, type, value, slotOut);

    case Type::Int64:
    case Type::Uint64:
    case Type::Int64Fast:
    case Type::Uint64Fast:
        return writeInt64Slot(globalObject, type, value, slotOut);

    case Type::Double:
    case Type::Float:
        return writeFloatingPointSlot(globalObject, type, value, slotOut);

    case Type::Pointer:
    case Type::CString:
    case Type::Function:
    case Type::Buffer:
        return writePointerSlot(globalObject, context, type, value, slotOut, arena);

    case Type::BufferLength:
        return writeBufferLengthSlot(globalObject, value, slotOut);

    case Type::JSValue:
        slotOut = static_cast<uint64_t>(JSValue::encode(value));
        return true;

    case Type::RESERVED_WasNapiEnv:
        RELEASE_ASSERT_NOT_REACHED();
        return false;

    case Type::Void:
        return true;
    }

    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

JSValue jsValueFromSlot(JSGlobalObject* globalObject, FFIContext&, Type type, uint64_t slot)
{
    switch (type) {
    case Type::Char:
    case Type::Int8:
        return jsNumber(static_cast<int32_t>(static_cast<int8_t>(slot)));
    case Type::Uint8:
        return jsNumber(static_cast<int32_t>(static_cast<uint8_t>(slot)));
    case Type::Int16:
        return jsNumber(static_cast<int32_t>(static_cast<int16_t>(slot)));
    case Type::Uint16:
        return jsNumber(static_cast<int32_t>(static_cast<uint16_t>(slot)));
    case Type::Int32:
        return jsNumber(static_cast<int32_t>(slot));
    case Type::Uint32:
        return jsNumber(static_cast<uint32_t>(slot));
    case Type::Bool:
        return jsBoolean(!!slot);
    case Type::Int64:
        return JSBigInt::makeHeapBigIntOrBigInt32(globalObject, static_cast<int64_t>(slot));
    case Type::Uint64:
    case Type::BufferLength:
        return JSBigInt::createFrom(globalObject, static_cast<uint64_t>(slot));
    case Type::Int64Fast: {
        int64_t value = static_cast<int64_t>(slot);
        if (value >= -maxInt52 && value <= maxInt52)
            return jsNumber(static_cast<double>(value));
        return JSBigInt::makeHeapBigIntOrBigInt32(globalObject, value);
    }
    case Type::Uint64Fast: {
        uint64_t value = static_cast<uint64_t>(slot);
        if (value < static_cast<uint64_t>(maxInt52))
            return jsNumber(static_cast<double>(value));
        return JSBigInt::createFrom(globalObject, value);
    }
    case Type::Double:
        return jsNumber(purifyNaN(std::bit_cast<double>(slot)));
    case Type::Float:
        return jsNumber(purifyNaN(static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(slot)))));
    case Type::CString: {
        const char* string = reinterpret_cast<const char*>(static_cast<uintptr_t>(slot));
        if (!string)
            return jsNull();
        String decoded = String::fromUTF8WithLatin1Fallback(std::span<const char>(string, strlen(string)));
        return jsString(getVM(globalObject), decoded);
    }
    case Type::Pointer:
    case Type::Function:
    case Type::Buffer:
        return pointerToJSValue(globalObject, static_cast<uint64_t>(slot));
    case Type::JSValue:
        return JSValue::decode(static_cast<EncodedJSValue>(slot));
    case Type::Void:
        return jsUndefined();
    case Type::RESERVED_WasNapiEnv:
        RELEASE_ASSERT_NOT_REACHED();
        return jsUndefined();
    }

    RELEASE_ASSERT_NOT_REACHED();
    return jsUndefined();
}

} } // namespace JSC::FFI

namespace JSC {

JSC_DEFINE_JIT_OPERATION(operationFFIBoxSlot, EncodedJSValue, (JSGlobalObject* globalObject, uint32_t typeTag, uint64_t slot, int32_t exitArena))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(typeTag < FFI::numberOfTypes);
    JSValue boxed = FFI::jsValueFromSlot(globalObject, globalObject->ffiContext(), static_cast<FFI::Type>(typeTag), slot);
    if (exitArena)
        globalObject->ffiContext().stringArena().exit();
    OPERATION_RETURN(scope, JSValue::encode(boxed));
}

JSC_DEFINE_JIT_OPERATION(operationFFIWriteSlot, void, (JSGlobalObject* globalObject, FFI::FFIContext* context, uint32_t typeTag, EncodedJSValue value, uint64_t* slot))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(context);
    ASSERT(typeTag < FFI::numberOfTypes);
    FFI::writeSlotFromJSValue(globalObject, *context, static_cast<FFI::Type>(typeTag), JSValue::decode(value), *slot, &context->arena());
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationFFIArenaEnter, void, (JSGlobalObject* globalObject))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    globalObject->ffiContext().arena().enter();
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationFFIArenaExit, void, (JSGlobalObject* globalObject))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    globalObject->ffiContext().arena().exit();
    OPERATION_RETURN(scope);
}

} // namespace JSC

#endif // USE(JSVALUE64)

#endif // USE(BUN_JSC_ADDITIONS)
