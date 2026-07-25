/*
 * Copyright (C) 2026 Oven-sh Inc. All rights reserved.
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

#if USE(BUN_JSC_ADDITIONS)

#if USE(JSVALUE64)

#include "FFIConversions.h"

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

// Bun's MAX_INT52 (FFI.h): the largest integer Bun's glue boxes as a Number
// on the i64_fast / u64_fast paths.
static constexpr int64_t maxInt52 = 9007199254740991;

int64_t doubleToInt64(double value)
{
#if CPU(X86_64)
    // cvttsd2si: NaN or |value| >= 2^63 -> 0x8000000000000000. This is the same
    // instruction MacroAssembler::truncateDoubleToInt64 emits, so the C++ and
    // JIT tiers agree bit-for-bit.
    return _mm_cvttsd_si64(_mm_set_sd(value));
#elif CPU(ARM64)
    // fcvtzs: saturates to the int64_t range, NaN -> 0 (MacroAssemblerARM64
    // truncateDoubleToInt64).
    return vcvtd_s64_f64(value);
#else
    // FFI is compiled out on other CPUs (SPEC section 14); provide the ARM64
    // saturating semantics without invoking undefined behavior so this stays
    // a total function.
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

// char, i8, u8, i16, u16, i32, u32, bool: number, boolean, undefined/null.
// ECMAScript modular integer conversion (toInt32 / toUInt32 then truncate to
// width) -- a deliberate divergence from the saturating clamps in Bun's JS
// glue, SPEC section 15 item 8.
// Bun parity: bun:ffi's shipped integer coercion is the `val|0` family
// (ToInt32(Number(val)) / ToUint32(Number(val))), i.e. FULLY loose JS numeric
// conversion -- strings ("42"), booleans, null/undefined and objects with
// valueOf all coerce; BigInts are additionally accepted here (Number()-style,
// oven-sh/bun#22751); only Symbols throw. Sub-word types then WRAP (mod 2^width,
// sign/zero-extended), matching the C cast the callee performs -- NOT clamped
// (the JS glue's historical clamping was the source of oven-sh/bun#7007).
static bool writeIntegerSlot(JSGlobalObject* globalObject, Type type, JSValue value, uint64_t& slotOut)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (type == Type::Bool) {
        // toBoolean semantics (never `& 1`); the producer normalizes the slot to
        // exactly 0 or 1 (SPEC section 4).
        slotOut = value.toBoolean(globalObject) ? 1 : 0;
        return true;
    }

    // Number(BigInt) semantics for the integer types: exact for values that
    // fit, wrapped mod 2^64 like the eventual C cast otherwise.
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

    // Strings and Symbols do NOT coerce into integer parameters (numbers,
    // booleans, null/undefined and BigInts do): a string passed where an int is
    // expected is a call-site bug, so reject it rather than silently
    // Number()-ing "300" into a plausible wrong value.
    if (value.isString() || value.isSymbol()) [[unlikely]]
        return throwCannotConvert(globalObject, scope, type);

    if (type == Type::Uint32) {
        // ToUint32(ToNumber(value)) over the remaining (numeric) inputs.
        uint32_t truncated = value.toUInt32(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        slotOut = static_cast<uint64_t>(truncated);
        return true;
    }

    // ToInt32(ToNumber(value)) -- the `val|0` the JS glue used.
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

// i64, i64_fast, u64, u64_fast: number or BigInt.
static bool writeInt64Slot(JSGlobalObject* globalObject, Type type, JSValue value, uint64_t& slotOut)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    bool isUnsigned = type == Type::Uint64 || type == Type::Uint64Fast;
    if (value.isInt32()) {
        // int32 -> sign-extend to 64 (then reinterpret for the unsigned types).
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

// Bun parity: bun:ffi's shipped f64 coercion is `typeof val === "number" ?
// val : Number(val)` -- plain JS Number() conversion: strings ("2.5" -> 2.5),
// booleans, null (-> +0) and undefined (-> NaN) all coerce; BigInts coerce like
// Number(5n) -> 5 (which strict ToNumber would throw on, hence the special
// case); only Symbols throw. This is pinned by bun's cc.test.ts snapshot.
static bool writeFloatingPointSlot(JSGlobalObject* globalObject, Type type, JSValue value, uint64_t& slotOut)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Strings and Symbols do NOT coerce into floating-point parameters (see the
    // integer rule): reject them instead of Number()-ing "2.5" -> 2.5.
    if (value.isString() || value.isSymbol()) [[unlikely]]
        return throwCannotConvert(globalObject, scope, type);

    double number;
    if (value.isNumber())
        number = value.asNumber();
    else if (value.isUndefined())
        number = PNaN; // Number(undefined) === NaN.
    else if (value.isBigInt()) {
        // Number(BigInt): exact conversion (huge magnitudes go to +/-Infinity).
        number = JSBigInt::toNumber(value).asNumber();
    } else {
        number = value.toNumber(globalObject); // strings, booleans, null, objects; Symbols throw.
        RETURN_IF_EXCEPTION(scope, false);
    }

    if (type == Type::Double) {
        slotOut = std::bit_cast<uint64_t>(number);
        return true;
    }

    ASSERT(type == Type::Float);
    float narrowed = static_cast<float>(number); // Math.fround semantics.
    // Bits [63:32] are zero per the canonical slot encoding (SPEC section 4).
    slotOut = static_cast<uint64_t>(std::bit_cast<uint32_t>(narrowed));
    return true;
}

// A JS string passed for a Type::CString argument is transcoded to a
// NUL-terminated UTF-8 copy owned by the call-scoped StringArena (a NEW
// capability, not Bun parity -- SPEC section 15 item 8b).
static bool writeCStringSlot(JSGlobalObject* globalObject, FFIContext& context, JSString* jsString, uint64_t& slotOut, StringArena* arena)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto string = jsString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);

    // FFI-SPEC-GAP: a null `arena` selects the context's own arena; either way
    // the conversion requires an open bracket, because arena storage is only
    // reclaimed at the next outermost enter() and StringArena::allocate() has
    // no owner outside a bracket. Every engine caller (host path, IC-stub slow
    // path, DFG/FTL operationFFIWriteSlot inside operationFFIArenaEnter/Exit,
    // callbackDispatch) is bracketed; an un-bracketed caller is a contract
    // violation and asserts.
    StringArena& targetArena = arena ? *arena : context.arena();
    ASSERT_WITH_MESSAGE(targetArena.depth(), "bun:ffi cstring conversion requires an active FFI arena bracket (StringArena::Scope / operationFFIArenaEnter)");

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

    // Fast path: an 8-bit StringImpl whose characters are all ASCII is
    // already valid UTF-8 -- memcpy plus the NUL terminator.
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

    // Slow path: UTF-8 transcode, memoized per resolved StringImpl in the
    // context's LRU. The cached CString is copied into the arena so that the
    // pointer handed to native code has call-scoped lifetime independent of
    // cache eviction.
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

// ptr, cstring, function, buffer.
static bool writePointerSlot(JSGlobalObject* globalObject, FFIContext& context, Type type, JSValue value, uint64_t& slotOut, StringArena* arena)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (type == Type::Buffer) {
        // Type::Buffer accepts a TypedArray/DataView only.
        if (auto* view = dynamicDowncast<JSArrayBufferView>(value)) {
            slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view->vector()));
            return true;
        }
        throwTypeError(globalObject, scope, "bun:ffi 'buffer' argument must be a TypedArray or DataView"_s);
        return false;
    }

    if (value.isUndefinedOrNull()) {
        // A null DATA pointer is legitimate (ptr / cstring / buffer callers pass null on purpose),
        // but a null FUNCTION pointer is never: an omitted or undefined callback argument used to
        // marshal NULL and the C callee would call through it (SIGSEGV) instead of the TypeError
        // the JS glue raised. Reject it here so a missing callback is a JS error, not a crash.
        if (type == Type::Function) [[unlikely]] {
            throwTypeError(globalObject, scope, "bun:ffi: expected a callback (a JSCallback or an FFI function) but got undefined/null"_s);
            return false;
        }
        slotOut = 0;
        return true;
    }

    if (value.isInt32()) {
        // int32 pointer arguments are sign-extended (Bun parity, FFI.h): -1
        // reads back as 0xFFFFFFFFFFFFFFFF.
        slotOut = static_cast<uint64_t>(static_cast<uintptr_t>(static_cast<intptr_t>(value.asInt32())));
        return true;
    }

    if (value.isDouble()) {
        slotOut = static_cast<uint64_t>(static_cast<uintptr_t>(doubleToInt64(value.asDouble())));
        return true;
    }

    if (value.isBigInt()) {
        // Round-trips pointers that were surfaced as BigInt because they
        // exceed 2^53 (jsValueFromSlot's pointer rule, oven-sh/bun#28068), and
        // accepts user-constructed BigInt addresses (oven-sh/bun#22751).
        slotOut = JSBigInt::toBigUInt64(value);
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }

    if (value.isCell()) {
        JSCell* cell = value.asCell();

        if (auto* view = dynamicDowncast<JSArrayBufferView>(cell)) {
            // FFI-SPEC-GAP: the section 5 table specifies "vector() (0 if
            // detached)" for pointer-family views, while the section 11.4 test
            // list mentions a TypeError for a detached buffer passed as ptr. The
            // normative conversion table wins: a detached view yields a null
            // pointer without throwing.
            slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view->vector()));
            return true;
        }

        if (auto* buffer = dynamicDowncast<JSArrayBuffer>(cell)) {
            slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buffer->impl()->data()));
            return true;
        }

        if (auto* callback = dynamicDowncast<JSFFICallback>(cell)) {
            // FFI-SPEC-GAP: SPEC section 9.1 assigns rejection of a close()d
            // callback to the $vm / Bun glue (which consults
            // JSFFICallback::isClosed(), e.g. tools/JSDollarVM.cpp's pointer
            // helper), so the engine-level conversion deliberately accepts a
            // closed callback and passes its nativeEntrypoint(), which stays
            // valid for the cell's lifetime even after close().
            slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(callback->nativeEntrypoint()));
            return true;
        }

        if (cell->isString()) {
            if (type == Type::CString)
                RELEASE_AND_RETURN(scope, writeCStringSlot(globalObject, context, uncheckedDowncast<JSString>(cell), slotOut, arena));
            // Only cstring transcodes; every other pointer-family type keeps
            // Bun's existing guidance for strings.
            throwTypeError(globalObject, scope, "To convert a string to a pointer, encode it as a buffer"_s);
            return false;
        }

        // An object carrying a numeric/BigInt `ptr` own or inherited property is accepted for the
        // pointer family (documented Bun API: FFIType.function / pointer accept a JSCallback,
        // Pointer or CString object, whose engine cell or address lives behind `.ptr`; the TinyCC-era
        // shim did `val && val.ptr`). The property get can run a getter, so exceptions propagate,
        // and only a number/BigInt result is unwrapped -- anything else falls to the type error.
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

    case Type::NapiValue:
        // Raw EncodedJSValue pass-through; no conversion.
        slotOut = static_cast<uint64_t>(JSValue::encode(value));
        return true;

    case Type::NapiEnv:
        // Synthetic argument: supplied by the engine from the context, never
        // read from the JS caller. Read live at conversion time.
        slotOut = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context.napiEnv()));
        return true;

    case Type::Void:
        // Void is only a return type; a callback returning void leaves the
        // return slot untouched (SPEC section 4).
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
        return JSBigInt::createFrom(globalObject, static_cast<uint64_t>(slot));
    case Type::Int64Fast: {
        int64_t value = static_cast<int64_t>(slot);
        if (value >= -maxInt52 && value <= maxInt52)
            return jsNumber(static_cast<double>(value));
        return JSBigInt::makeHeapBigIntOrBigInt32(globalObject, value);
    }
    case Type::Uint64Fast: {
        uint64_t value = static_cast<uint64_t>(slot);
        // Bun's UINT64_TO_JSVALUE uses a strict `<` against MAX_INT52 (FFI.h);
        // preserved for parity.
        if (value < static_cast<uint64_t>(maxInt52))
            return jsNumber(static_cast<double>(value));
        return JSBigInt::createFrom(globalObject, value);
    }
    case Type::Double:
        // Every native -> JS floating point value is purified exactly once at
        // the point it leaves its slot, so a native NaN payload can never be
        // NaN-boxed into a forged JSValue.
        return jsNumber(purifyNaN(std::bit_cast<double>(slot)));
    case Type::Float:
        return jsNumber(purifyNaN(static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(slot)))));
    case Type::Pointer:
    case Type::CString:
    case Type::Function:
    case Type::Buffer:
        // Bun's PTR_TO_JSVALUE returns null for a null pointer; otherwise a
        // pointer is exposed to JS as a double. Addresses above 2^53 (5-level
        // page tables / arm64 memory-tagged pointers, oven-sh/bun#28068) cannot be
        // represented exactly as a double, so those are surfaced as a BigInt
        // (the u64_fast rule) instead of silently losing bits.
        if (!slot)
            return jsNull();
        if (static_cast<uint64_t>(slot) <= static_cast<uint64_t>(maxInt52))
            return jsNumber(static_cast<double>(static_cast<uint64_t>(slot)));
        return JSBigInt::createFrom(globalObject, static_cast<uint64_t>(slot));
    case Type::NapiValue:
        return JSValue::decode(static_cast<EncodedJSValue>(slot));
    case Type::Void:
        return jsUndefined();
    case Type::NapiEnv:
        // napi_env is a synthetic argument and never a return type or a
        // callback argument (Signature validation rejects it as a return).
        ASSERT_NOT_REACHED();
        return jsUndefined();
    }

    RELEASE_ASSERT_NOT_REACHED();
    return jsUndefined();
}

} } // namespace JSC::FFI

namespace JSC {

JSC_DEFINE_JIT_OPERATION(operationFFIBoxSlot, EncodedJSValue, (JSGlobalObject* globalObject, uint32_t typeTag, uint64_t slot))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(typeTag < FFI::numberOfTypes);
    OPERATION_RETURN(scope, JSValue::encode(FFI::jsValueFromSlot(globalObject, globalObject->ffiContext(), static_cast<FFI::Type>(typeTag), slot)));
}

JSC_DEFINE_JIT_OPERATION(operationFFIWriteSlot, void, (JSGlobalObject* globalObject, FFI::FFIContext* context, uint32_t typeTag, EncodedJSValue value, uint64_t* slot))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(context);
    ASSERT(typeTag < FFI::numberOfTypes);
    // The DFG/FTL caller brackets this operation with
    // operationFFIArenaEnter / operationFFIArenaExit whenever a CString or
    // pointer-family argument is UntypedUse, so the context's arena is the
    // call-scoped storage for any JS-string transcode performed here.
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

    // The DFG/FTL CallFFI paths also emit this call on the exception edge of
    // operationFFIWriteSlot and right after the invoke thunk returns (SPEC
    // section 5), so a VM exception may already be pending here. The pop must
    // happen unconditionally and the pending exception must be left untouched:
    // no early return precedes exit(), and OPERATION_RETURN only reports the
    // (possibly pre-existing) exception back to the JIT caller.
    globalObject->ffiContext().arena().exit();
    OPERATION_RETURN(scope);
}

} // namespace JSC

#endif // USE(JSVALUE64)

#endif // USE(BUN_JSC_ADDITIONS)
