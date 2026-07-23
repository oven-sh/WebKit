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

#pragma once

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

// FFI-SPEC-GAP: spec section 14 asks for USE(JSVALUE64) around "everything" for
// 32-bit, but this header (and FFISignature.h) contains no JSValue-facing or
// JIT-emitting code, so it is guarded only by USE(BUN_JSC_ADDITIONS); the
// JSVALUE64 / ENABLE(JIT) guards are applied by the consuming rows where they
// are relevant (creation APIs, thunk generators, DFG/FTL).

#include "JSExportMacros.h"
#include <atomic>
#include <optional>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace FFI {

// The numeric tags are wire-compatible with Bun's existing FFIType map
// (bun/src/js/bun/ffi.ts) and MUST NOT change.
enum class Type : uint8_t {
    Char = 0,
    Int8 = 1,
    Uint8 = 2,
    Int16 = 3,
    Uint16 = 4,
    Int32 = 5,
    Uint32 = 6,
    Int64 = 7,
    Uint64 = 8,
    Double = 9,
    Float = 10,
    Bool = 11,
    Pointer = 12,
    Void = 13,
    CString = 14,
    Int64Fast = 15,
    Uint64Fast = 16,
    Function = 17,
    NapiEnv = 18,
    NapiValue = 19,
    Buffer = 20,
};

constexpr unsigned numberOfTypes = 21;

// Canonical name of a type (also used by Signature::toString()). Total over
// all numberOfTypes values.
constexpr ASCIILiteral name(Type type)
{
    switch (type) {
    case Type::Char:
        return "char"_s;
    case Type::Int8:
        return "i8"_s;
    case Type::Uint8:
        return "u8"_s;
    case Type::Int16:
        return "i16"_s;
    case Type::Uint16:
        return "u16"_s;
    case Type::Int32:
        return "i32"_s;
    case Type::Uint32:
        return "u32"_s;
    case Type::Int64:
        return "i64"_s;
    case Type::Uint64:
        return "u64"_s;
    case Type::Double:
        return "f64"_s;
    case Type::Float:
        return "f32"_s;
    case Type::Bool:
        return "bool"_s;
    case Type::Pointer:
        return "ptr"_s;
    case Type::Void:
        return "void"_s;
    case Type::CString:
        return "cstring"_s;
    case Type::Int64Fast:
        return "i64_fast"_s;
    case Type::Uint64Fast:
        return "u64_fast"_s;
    case Type::Function:
        return "function"_s;
    case Type::NapiEnv:
        return "napi_env"_s;
    case Type::NapiValue:
        return "napi_value"_s;
    case Type::Buffer:
        return "buffer"_s;
    }
    return "invalid"_s;
}

constexpr std::optional<Type> typeFromTag(unsigned tag)
{
    if (tag >= numberOfTypes)
        return std::nullopt;
    return static_cast<Type>(tag);
}

// Accepts the canonical names produced by name() plus the aliases Bun's
// FFIType map exposes. Matching is exact (case-sensitive), like Bun's map.
inline std::optional<Type> parseType(StringView string)
{
    // Canonical names.
    if (string == "char"_s)
        return Type::Char;
    if (string == "i8"_s)
        return Type::Int8;
    if (string == "u8"_s)
        return Type::Uint8;
    if (string == "i16"_s)
        return Type::Int16;
    if (string == "u16"_s)
        return Type::Uint16;
    if (string == "i32"_s)
        return Type::Int32;
    if (string == "u32"_s)
        return Type::Uint32;
    if (string == "i64"_s)
        return Type::Int64;
    if (string == "u64"_s)
        return Type::Uint64;
    if (string == "f64"_s)
        return Type::Double;
    if (string == "f32"_s)
        return Type::Float;
    if (string == "bool"_s)
        return Type::Bool;
    if (string == "ptr"_s)
        return Type::Pointer;
    if (string == "void"_s)
        return Type::Void;
    if (string == "cstring"_s)
        return Type::CString;
    if (string == "i64_fast"_s)
        return Type::Int64Fast;
    if (string == "u64_fast"_s)
        return Type::Uint64Fast;
    if (string == "function"_s)
        return Type::Function;
    if (string == "napi_env"_s)
        return Type::NapiEnv;
    if (string == "napi_value"_s)
        return Type::NapiValue;
    if (string == "buffer"_s)
        return Type::Buffer;

    // Aliases.
    if (string == "int8_t"_s)
        return Type::Int8;
    if (string == "uint8_t"_s)
        return Type::Uint8;
    if (string == "int16_t"_s)
        return Type::Int16;
    if (string == "uint16_t"_s)
        return Type::Uint16;
    if (string == "int32_t"_s || string == "int"_s || string == "c_int"_s)
        return Type::Int32;
    if (string == "uint32_t"_s || string == "c_uint"_s)
        return Type::Uint32;
    if (string == "int64_t"_s || string == "isize"_s)
        return Type::Int64;
    // FFI-SPEC-GAP: the spec lists "size_t" as an alias without a target; it
    // is mapped to Uint64 alongside Bun's "usize"/"uint64_t" (both tag 8).
    if (string == "uint64_t"_s || string == "usize"_s || string == "size_t"_s)
        return Type::Uint64;
    if (string == "double"_s)
        return Type::Double;
    if (string == "float"_s)
        return Type::Float;
    // FFI-SPEC-GAP: the spec lists "char*" as an alias without a target; Bun's
    // FFIType map spells "char*" as tag 12 (ptr), so it maps to Pointer here
    // rather than CString to keep tag parity with the JS glue.
    if (string == "void*"_s || string == "pointer"_s || string == "char*"_s)
        return Type::Pointer;
    if (string == "callback"_s || string == "fn"_s)
        return Type::Function;

    return std::nullopt;
}

// The class of native location a value of a given type travels in.
enum class ArgClass : uint8_t { Void, Int, Float, Double };

constexpr ArgClass argClass(Type type)
{
    switch (type) {
    case Type::Void:
        return ArgClass::Void;
    case Type::Float:
        return ArgClass::Float;
    case Type::Double:
        return ArgClass::Double;
    case Type::Char:
    case Type::Int8:
    case Type::Uint8:
    case Type::Int16:
    case Type::Uint16:
    case Type::Int32:
    case Type::Uint32:
    case Type::Int64:
    case Type::Uint64:
    case Type::Bool:
    case Type::Pointer:
    case Type::CString:
    case Type::Int64Fast:
    case Type::Uint64Fast:
    case Type::Function:
    case Type::NapiEnv:
    case Type::NapiValue:
    case Type::Buffer:
        return ArgClass::Int;
    }
    return ArgClass::Int;
}

// NapiEnv and Buffer are argument-only (Bun compatibility). Void is a valid
// return type.
constexpr bool isValidReturnType(Type type)
{
    return type != Type::NapiEnv && type != Type::Buffer;
}

// Void is invalid as an argument; everything else is a valid argument type.
constexpr bool isValidArgumentType(Type type)
{
    return type != Type::Void;
}

// A synthetic argument is supplied by the engine (from FFIContext), not by
// the JS caller: it occupies a native parameter slot but no JS argument.
constexpr bool isSyntheticArgument(Type type)
{
    return type == Type::NapiEnv;
}

// Size in bytes of the native (C ABI) representation. Void has no
// representation; it returns 0 and callers must not query it for a location.
constexpr unsigned nativeSizeInBytes(Type type)
{
    switch (type) {
    case Type::Void:
        return 0;
    case Type::Char:
    case Type::Int8:
    case Type::Uint8:
    case Type::Bool:
        return 1;
    case Type::Int16:
    case Type::Uint16:
        return 2;
    case Type::Int32:
    case Type::Uint32:
    case Type::Float:
        return 4;
    case Type::Int64:
    case Type::Uint64:
    case Type::Double:
    case Type::Pointer:
    case Type::CString:
    case Type::Int64Fast:
    case Type::Uint64Fast:
    case Type::Function:
    case Type::NapiEnv:
    case Type::NapiValue:
    case Type::Buffer:
        return 8;
    }
    return 0;
}

// Governs sub-word extension. Type::Char is a SIGNED 8-bit integer on every
// target (identical to Int8); it deliberately does not follow the platform C
// `char` signedness.
constexpr bool isSigned(Type type)
{
    switch (type) {
    case Type::Char:
    case Type::Int8:
    case Type::Int16:
    case Type::Int32:
    case Type::Int64:
    case Type::Int64Fast:
        return true;
    case Type::Uint8:
    case Type::Uint16:
    case Type::Uint32:
    case Type::Uint64:
    case Type::Uint64Fast:
    case Type::Double:
    case Type::Float:
    case Type::Bool:
    case Type::Pointer:
    case Type::Void:
    case Type::CString:
    case Type::Function:
    case Type::NapiEnv:
    case Type::NapiValue:
    case Type::Buffer:
        return false;
    }
    return false;
}

// Process-global counters proving that each JIT tier actually compiled FFI
// code (read by $vm.ffiCompileCounts(); incremented by the IC-stub generator,
// SpeculativeJIT::compileCallFFI and the FTL lowering respectively).
struct CompileCounts {
    std::atomic<uint64_t> icStub { 0 };
    std::atomic<uint64_t> dfgCallFFI { 0 };
    std::atomic<uint64_t> ftlCallFFI { 0 };
};

extern JS_EXPORT_PRIVATE CompileCounts g_ffiCompileCounts;

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
