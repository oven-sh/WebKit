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

#pragma once

#include <wtf/Platform.h>

#if USE(BUN_JSC_ADDITIONS)

#include "JSExportMacros.h"
#include <atomic>
#include <optional>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace FFI {

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
    RESERVED_WasNapiEnv = 18,
    JSValue = 19,
    Buffer = 20,
    BufferLength = 21,
};

constexpr unsigned numberOfTypes = 22;

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
    case Type::RESERVED_WasNapiEnv:
        return "invalid"_s;
    case Type::JSValue:
        return "jsvalue"_s;
    case Type::Buffer:
        return "buffer"_s;
    case Type::BufferLength:
        return "buffer_length"_s;
    }
    return "invalid"_s;
}

inline std::optional<Type> parseType(StringView string)
{
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
    if (string == "jsvalue"_s || string == "napi_value"_s) // napi_value: legacy spelling of the same raw-JSValue type
        return Type::JSValue;
    if (string == "buffer"_s)
        return Type::Buffer;
    if (string == "buffer_length"_s || string == "buffer_bytelength"_s) // buffer_bytelength: alias of the same length twin
        return Type::BufferLength;

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
    if (string == "uint64_t"_s || string == "usize"_s || string == "size_t"_s)
        return Type::Uint64;
    if (string == "double"_s)
        return Type::Double;
    if (string == "float"_s)
        return Type::Float;
    if (string == "void*"_s || string == "pointer"_s || string == "char*"_s)
        return Type::Pointer;
    if (string == "callback"_s || string == "fn"_s)
        return Type::Function;

    return std::nullopt;
}

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
    case Type::RESERVED_WasNapiEnv:
    case Type::JSValue:
    case Type::Buffer:
    case Type::BufferLength:
        return ArgClass::Int;
    }
    return ArgClass::Int;
}

constexpr bool isValidReturnType(Type type)
{
    return type != Type::RESERVED_WasNapiEnv && type != Type::Buffer && type != Type::BufferLength;
}

constexpr bool isValidArgumentType(Type type)
{
    return type != Type::Void && type != Type::RESERVED_WasNapiEnv;
}

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
    case Type::RESERVED_WasNapiEnv:
    case Type::JSValue:
    case Type::Buffer:
    case Type::BufferLength: // uint64_t / size_t, exactly like Uint64
        return 8;
    }
    return 0;
}

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
    case Type::RESERVED_WasNapiEnv:
    case Type::JSValue:
    case Type::Buffer:
    case Type::BufferLength: // unsigned 64-bit, exactly like Uint64
        return false;
    }
    return false;
}

struct CompileCounts {
    std::atomic<uint64_t> icStub { 0 };
    std::atomic<uint64_t> dfgCallFFI { 0 };
    std::atomic<uint64_t> ftlCallFFI { 0 };
};

extern JS_EXPORT_PRIVATE CompileCounts g_ffiCompileCounts;

} } // namespace JSC::FFI

#endif // USE(BUN_JSC_ADDITIONS)
