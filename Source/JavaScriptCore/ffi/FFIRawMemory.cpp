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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FFIRawMemory.h"

#if USE(BUN_JSC_ADDITIONS) && USE(JSVALUE64)

#include "FFIContext.h"
#include "FFIConversions.h"
#include "FFIType.h"
#include "JSBigInt.h"
#include "JSCJSValueInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSObject.h"
#include "ObjectConstructor.h"
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>

namespace JSC { namespace FFI {

// Slow paths for the `read` singleton (LLInt / baseline, and any input the DFG did not speculate).
// `read.<t>(address, byteOffset)`: the address goes through the same converter as an FFI
// `ptr` argument (number, BigInt, ArrayBufferView/ArrayBuffer, null/undefined -> 0), the offset
// through toInt32, and the loaded value is boxed by the shared native->JS rule for the type.
// No bounds checking, by design: the caller owns the memory (same contract as the C reads it wraps).
template<Type type>
static ALWAYS_INLINE EncodedJSValue rawRead(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    FFIContext& context = globalObject->ffiContext();

    // The address is a NUMBER (Bun parity: today's readers accept only a numeric address obtained
    // from ptr()), plus BigInt so the exact >2^53 addresses that read.ptr / FFI can now return
    // round-trip back in. Views/ArrayBuffers are NOT accepted here (call ptr() first).
    JSValue addressValue = callFrame->argument(0);
    uint64_t addressSlot;
    if (addressValue.isInt32())
        addressSlot = static_cast<uint64_t>(static_cast<int64_t>(addressValue.asInt32()));
    else if (addressValue.isDouble())
        addressSlot = static_cast<uint64_t>(doubleToInt64(addressValue.asDouble()));
    else if (addressValue.isBigInt())
        addressSlot = JSBigInt::toBigUInt64(addressValue);
    else
        return throwVMTypeError(globalObject, scope, "bun:ffi read.*() expects a pointer (a number from ptr())"_s);
    RETURN_IF_EXCEPTION(scope, { });

    int32_t byteOffset = callFrame->argument(1).toInt32(globalObject);
    RETURN_IF_EXCEPTION(scope, { });

    // Load into the canonical slot encoding (SPEC section 4) so jsValueFromSlot boxes it exactly
    // like an FFI return value of the same type (sign/zero extension, f32 widening, ptr rules).
    // WTF::unalignedLoad is the memcpy-based unaligned-safe load DataView/typed arrays use.
    // `type` is a template parameter, so `if constexpr` instantiates exactly one load per reader.
    uint64_t slot = 0;
    const void* address = reinterpret_cast<const void*>(static_cast<uintptr_t>(addressSlot) + static_cast<intptr_t>(byteOffset));
    if constexpr (type == Type::Uint8)
        slot = WTF::unalignedLoad<uint8_t>(address);
    else if constexpr (type == Type::Int8)
        slot = static_cast<uint64_t>(static_cast<int64_t>(WTF::unalignedLoad<int8_t>(address)));
    else if constexpr (type == Type::Uint16)
        slot = WTF::unalignedLoad<uint16_t>(address);
    else if constexpr (type == Type::Int16)
        slot = static_cast<uint64_t>(static_cast<int64_t>(WTF::unalignedLoad<int16_t>(address)));
    else if constexpr (type == Type::Uint32)
        slot = WTF::unalignedLoad<uint32_t>(address);
    else if constexpr (type == Type::Int32)
        slot = static_cast<uint64_t>(static_cast<int64_t>(WTF::unalignedLoad<int32_t>(address)));
    else if constexpr (type == Type::Float)
        slot = WTF::unalignedLoad<uint32_t>(address); // f32 bits in the low half (SPEC section 4)
    else {
        // i64, u64, ptr, intptr, f64: the raw 64 bits are the canonical slot encoding.
        static_assert(type == Type::Int64 || type == Type::Uint64 || type == Type::Pointer || type == Type::Int64Fast || type == Type::Double);
        slot = WTF::unalignedLoad<uint64_t>(address);
    }

    RELEASE_AND_RETURN(scope, JSValue::encode(jsValueFromSlot(globalObject, context, type, slot)));
}

#define DEFINE_FFI_RAW_READER(name, type) \
    JSC_DECLARE_HOST_FUNCTION(ffiRawRead##name); \
    JSC_DEFINE_HOST_FUNCTION(ffiRawRead##name, (JSGlobalObject* globalObject, CallFrame* callFrame)) \
    { \
        return rawRead<type>(globalObject, callFrame); \
    }

DEFINE_FFI_RAW_READER(U8, Type::Uint8)
DEFINE_FFI_RAW_READER(I8, Type::Int8)
DEFINE_FFI_RAW_READER(U16, Type::Uint16)
DEFINE_FFI_RAW_READER(I16, Type::Int16)
DEFINE_FFI_RAW_READER(U32, Type::Uint32)
DEFINE_FFI_RAW_READER(I32, Type::Int32)
DEFINE_FFI_RAW_READER(I64, Type::Int64)
DEFINE_FFI_RAW_READER(U64, Type::Uint64)
DEFINE_FFI_RAW_READER(F32, Type::Float)
DEFINE_FFI_RAW_READER(F64, Type::Double)
DEFINE_FFI_RAW_READER(Ptr, Type::Pointer)
DEFINE_FFI_RAW_READER(IntPtr, Type::Int64Fast)

#undef DEFINE_FFI_RAW_READER

// The JIT-lowered readers, keyed by their (distinct) native function pointer, with the DataViewData
// (byteSize / isSigned / isFloatingPoint) the DFG's FFIRawRead node needs. ptr / intptr are 8-byte
// integer reads surfaced as doubles. i64 / u64 (BigInt results) are deliberately absent: host path only.
struct RawReaderEntry {
    ASCIILiteral name;
    NativeFunction function;
    uint8_t byteSize;
    bool isSigned;
    bool isFloatingPoint;
    bool hasJITLowering;
};

static const RawReaderEntry* rawReaderTable(unsigned& count)
{
    static const RawReaderEntry table[] = {
        { "u8"_s, ffiRawReadU8, 1, false, false, true },
        { "i8"_s, ffiRawReadI8, 1, true, false, true },
        { "u16"_s, ffiRawReadU16, 2, false, false, true },
        { "i16"_s, ffiRawReadI16, 2, true, false, true },
        { "u32"_s, ffiRawReadU32, 4, false, false, true },
        { "i32"_s, ffiRawReadI32, 4, true, false, true },
        { "i64"_s, ffiRawReadI64, 8, true, false, false },
        { "u64"_s, ffiRawReadU64, 8, false, false, false },
        { "f32"_s, ffiRawReadF32, 4, false, true, true },
        { "f64"_s, ffiRawReadF64, 8, false, true, true },
        { "ptr"_s, ffiRawReadPtr, 8, false, false, true },
        { "intptr"_s, ffiRawReadIntPtr, 8, false, false, true },
    };
    count = std::size(table);
    return table;
}

std::optional<DFG::DataViewData> rawReaderDataViewData(TaggedNativeFunction function)
{
    unsigned count;
    const RawReaderEntry* table = rawReaderTable(count);
    for (unsigned i = 0; i < count; ++i) {
        // NativeFunction and TaggedNativeFunction carry different pointer tags; compare the
        // untagged raw function addresses (also correct under ARM64E pointer authentication).
        if (table[i].function.untaggedPtr() != function.untaggedPtr())
            continue;
        if (!table[i].hasJITLowering)
            return std::nullopt;
        DFG::DataViewData data { };
        data.byteSize = table[i].byteSize;
        data.isSigned = table[i].isSigned;
        data.isFloatingPoint = table[i].isFloatingPoint;
        data.isResizable = false;
        data.isLittleEndian = TriState::True; // native (little) endianness only
        return data;
    }
    return std::nullopt;
}

JSObject* createReadObject(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);

    unsigned count;
    const RawReaderEntry* table = rawReaderTable(count);
    JSObject* read = constructEmptyObject(globalObject, globalObject->objectPrototype(), count);
    for (unsigned i = 0; i < count; ++i) {
        // Every reader (JIT-lowered or not) shares FFIRawReadIntrinsic; the DFG bails out for the
        // BigInt ones via rawReaderDataViewDataQuadWord() returning nullopt.
        JSFunction* function = JSFunction::create(vm, globalObject, 2, table[i].name, table[i].function, ImplementationVisibility::Public, FFIRawReadIntrinsic);
        read->putDirect(vm, Identifier::fromString(vm, table[i].name), function, PropertyAttribute::None | 0);
    }
    return read;
}

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS) && USE(JSVALUE64)
