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
#include "FFITestFixtures.h"

#include "InitializeThreading.h"
#include "Options.h"
#include <wtf/Compiler.h>
#include <wtf/DataLog.h>
#include <wtf/RawHex.h>
#include <wtf/WTFProcess.h>
#include <wtf/text/StringCommon.h>
#include <wtf/text/StringConcatenateNumbers.h>
#include <wtf/text/StringToIntegerConversion.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

#if USE(BUN_JSC_ADDITIONS) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))

#include "CallData.h"
#include "CallFrameInlines.h"
#include "ConstructData.h"
#include "ErrorInstance.h"
#include "FFICallingConvention.h"
#include "FFIContext.h"
#include "FFIConversions.h"
#include "FFIInvokeThunk.h"
#include "FFISignature.h"
#include "FFIType.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include "JSArrayBufferView.h"
#include "JSBigInt.h"
#include "JSBigIntInlines.h"
#include "JSCInlines.h"
#include "JSCJSValueInlines.h"
#include "JSFFICallback.h"
#include "JSFFIFunction.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSGlobalObjectInlines.h"
#include "JSLock.h"
#include "JSString.h"
#include "JSTypedArrays.h"
#include "MacroAssemblerCodeRef.h"
#include "ObjectConstructor.h"
#include "Protect.h"
#include "PureNaN.h"
#include "Symbol.h"
#include "TopExceptionScope.h"
#include "TypedArrayInlines.h"
#include "VM.h"
#include <bit>
#include <cmath>
#include <iterator>
#include <limits>
#include <wtf/MathExtras.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/CString.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/WTFString.h>

using namespace JSC;

namespace {

static unsigned s_failureCount;
static unsigned s_checkCount;

#define FFI_CHECK(condition) do { \
        s_checkCount++; \
        if (!(condition)) { \
            s_failureCount++; \
            dataLogLn("    FAIL: ", __FILE__, ":", __LINE__, ": ", #condition); \
        } \
    } while (false)

#define FFI_CHECK_EQ(actualExpression, expectedExpression) do { \
        s_checkCount++; \
        auto _ffiActual = (actualExpression); \
        auto _ffiExpected = (expectedExpression); \
        if (!(_ffiActual == static_cast<decltype(_ffiActual)>(_ffiExpected))) { \
            s_failureCount++; \
            dataLogLn("    FAIL: ", __FILE__, ":", __LINE__, ": ", #actualExpression, " == ", #expectedExpression, " (actual ", _ffiActual, ", expected ", _ffiExpected, ")"); \
        } \
    } while (false)

#define FFI_CHECK_EQ_HEX(actualExpression, expectedExpression) do { \
        s_checkCount++; \
        uint64_t _ffiActual = static_cast<uint64_t>(actualExpression); \
        uint64_t _ffiExpected = static_cast<uint64_t>(expectedExpression); \
        if (_ffiActual != _ffiExpected) { \
            s_failureCount++; \
            dataLogLn("    FAIL: ", __FILE__, ":", __LINE__, ": ", #actualExpression, " == ", #expectedExpression, " (actual ", RawHex(_ffiActual), ", expected ", RawHex(_ffiExpected), ")"); \
        } \
    } while (false)

static const char* s_filter;

static bool shouldRun(const char* testName)
{
    return !s_filter || WTF::findIgnoringASCIICaseWithoutLength(testName, s_filter) != WTF::notFound;
}

#define RUN(test) do { \
        if (!shouldRun(#test)) \
            break; \
        unsigned failuresBefore = s_failureCount; \
        dataLogLn(#test, "..."); \
        test; \
        dataLogLn(s_failureCount == failuresBefore ? "PASS: " : "FAIL: ", #test); \
    } while (false)

static RefPtr<VM> s_vm;
static JSGlobalObject* s_globalObject;

static FFI::FFIContext& ffiContext()
{
    return s_globalObject->ffiContext();
}

static FFI::StringArena* testStringArena()
{
    return &ffiContext().stringArena();
}

static const char* typeNameForLog(FFI::Type type)
{
    return FFI::name(type).characters();
}

static void testTypeTraits()
{
    using FFI::Type;

    FFI_CHECK_EQ(FFI::numberOfTypes, 22u);

    FFI_CHECK_EQ(static_cast<unsigned>(Type::Char), 0u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Int8), 1u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Uint8), 2u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Int16), 3u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Uint16), 4u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Int32), 5u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Uint32), 6u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Int64), 7u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Uint64), 8u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Double), 9u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Float), 10u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Bool), 11u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Pointer), 12u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Void), 13u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::CString), 14u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Int64Fast), 15u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Uint64Fast), 16u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Function), 17u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::RESERVED_WasNapiEnv), 18u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::JSValue), 19u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::Buffer), 20u);
    FFI_CHECK_EQ(static_cast<unsigned>(Type::BufferLength), 21u);

    static const struct { Type type; ASCIILiteral name; } names[] = {
        { Type::Char, "char"_s }, { Type::Int8, "i8"_s }, { Type::Uint8, "u8"_s }, { Type::Int16, "i16"_s },
        { Type::Uint16, "u16"_s }, { Type::Int32, "i32"_s }, { Type::Uint32, "u32"_s }, { Type::Int64, "i64"_s },
        { Type::Uint64, "u64"_s }, { Type::Double, "f64"_s }, { Type::Float, "f32"_s }, { Type::Bool, "bool"_s },
        { Type::Pointer, "ptr"_s }, { Type::Void, "void"_s }, { Type::CString, "cstring"_s }, { Type::Int64Fast, "i64_fast"_s },
        { Type::Uint64Fast, "u64_fast"_s }, { Type::Function, "function"_s }, { Type::JSValue, "jsvalue"_s }, { Type::Buffer, "buffer"_s },
        { Type::BufferLength, "buffer_length"_s },
    };
    for (auto& entry : names) {
        FFI_CHECK(!strcmp(FFI::name(entry.type).characters(), entry.name.characters()));
        std::optional<Type> parsed = FFI::parseType(StringView(entry.name));
        FFI_CHECK(parsed.has_value());
        if (parsed)
            FFI_CHECK(*parsed == entry.type);
    }

    static const struct { ASCIILiteral alias; Type type; } aliases[] = {
        { "int8_t"_s, Type::Int8 }, { "uint8_t"_s, Type::Uint8 }, { "int16_t"_s, Type::Int16 }, { "uint16_t"_s, Type::Uint16 },
        { "int32_t"_s, Type::Int32 }, { "int"_s, Type::Int32 }, { "c_int"_s, Type::Int32 }, { "uint32_t"_s, Type::Uint32 },
        { "c_uint"_s, Type::Uint32 }, { "int64_t"_s, Type::Int64 }, { "isize"_s, Type::Int64 }, { "uint64_t"_s, Type::Uint64 },
        { "usize"_s, Type::Uint64 }, { "size_t"_s, Type::Uint64 }, { "double"_s, Type::Double }, { "float"_s, Type::Float },
        { "void*"_s, Type::Pointer }, { "pointer"_s, Type::Pointer }, { "char*"_s, Type::Pointer }, { "callback"_s, Type::Function },
        { "fn"_s, Type::Function }, { "napi_value"_s, Type::JSValue },
        { "buffer_bytelength"_s, Type::BufferLength },
    };
    for (auto& entry : aliases) {
        std::optional<Type> parsed = FFI::parseType(StringView(entry.alias));
        FFI_CHECK(parsed.has_value());
        if (parsed)
            FFI_CHECK(*parsed == entry.type);
    }
    FFI_CHECK(!FFI::parseType(StringView("int128"_s)).has_value());
    FFI_CHECK(!FFI::parseType(StringView(""_s)).has_value());
    FFI_CHECK(!FFI::parseType(StringView("I32"_s)).has_value());

    for (unsigned tag = 0; tag < FFI::numberOfTypes; ++tag) {
        Type type = static_cast<Type>(tag);
        FFI::ArgClass expected = FFI::ArgClass::Int;
        if (type == Type::Void)
            expected = FFI::ArgClass::Void;
        else if (type == Type::Float)
            expected = FFI::ArgClass::Float;
        else if (type == Type::Double)
            expected = FFI::ArgClass::Double;
        FFI_CHECK(FFI::argClass(type) == expected);
        FFI_CHECK_EQ(FFI::isValidReturnType(type), type != Type::RESERVED_WasNapiEnv && type != Type::Buffer && type != Type::BufferLength);
        FFI_CHECK_EQ(FFI::isValidArgumentType(type), type != Type::Void && type != Type::RESERVED_WasNapiEnv);
    }

    FFI_CHECK(FFI::isSigned(Type::Char));
    FFI_CHECK(FFI::isSigned(Type::Int8));
    FFI_CHECK(FFI::isSigned(Type::Int16));
    FFI_CHECK(FFI::isSigned(Type::Int32));
    FFI_CHECK(FFI::isSigned(Type::Int64));
    FFI_CHECK(FFI::isSigned(Type::Int64Fast));
    FFI_CHECK(!FFI::isSigned(Type::Uint8));
    FFI_CHECK(!FFI::isSigned(Type::Uint16));
    FFI_CHECK(!FFI::isSigned(Type::Uint32));
    FFI_CHECK(!FFI::isSigned(Type::Uint64));
    FFI_CHECK(!FFI::isSigned(Type::Uint64Fast));
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Char), 1u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Int8), 1u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Uint8), 1u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Bool), 1u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Int16), 2u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Uint16), 2u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Int32), 4u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Uint32), 4u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Float), 4u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Int64), 8u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Uint64), 8u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Int64Fast), 8u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Uint64Fast), 8u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Double), 8u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Pointer), 8u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::CString), 8u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Function), 8u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::Buffer), 8u);
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::BufferLength), 8u);
    FFI_CHECK(!FFI::isSigned(Type::BufferLength));
    FFI_CHECK_EQ(FFI::nativeSizeInBytes(Type::JSValue), 8u);
    FFI_CHECK_EQ(FFI::slotSize, 8u);
}

static void testSignatures()
{
    using FFI::Type;

    Vector<Type> args { Type::Int32, Type::Double };
    RefPtr<FFI::Signature> a = FFI::Signature::tryCreate(args.span(), Type::Double);
    RefPtr<FFI::Signature> b = FFI::Signature::tryCreate(args.span(), Type::Double);
    FFI_CHECK(!!a);
    FFI_CHECK(!!b);
    if (!a || !b)
        return;

    FFI_CHECK(a.get() == b.get());
    FFI_CHECK(*a == *b);
    FFI_CHECK_EQ(a->hash(), b->hash());
    Ref<FFI::Signature> viaRegistry = FFI::SignatureRegistry::singleton().intern(args.span(), Type::Double);
    FFI_CHECK(viaRegistry.ptr() == a.get());
    FFI_CHECK(a->toString() == "f64(i32,f64)"_s);
    FFI_CHECK_EQ(a->argumentCount(), 2u);
    FFI_CHECK_EQ(a->slotCount(), 3u);
    FFI_CHECK_EQ(a->slotBufferBytes(), 24u);
    FFI_CHECK(a->argumentType(0) == Type::Int32);
    FFI_CHECK(a->argumentType(1) == Type::Double);
    FFI_CHECK(a->returnType() == Type::Double);

    RefPtr<FFI::Signature> c = FFI::Signature::tryCreate(args.span(), Type::Float);
    FFI_CHECK(!!c);
    if (c) {
        FFI_CHECK(c.get() != a.get());
        FFI_CHECK(!(*c == *a));
        FFI_CHECK(c->toString() == "f32(i32,f64)"_s);
    }

    Vector<Type> reversed { Type::Double, Type::Int32 };
    RefPtr<FFI::Signature> d = FFI::Signature::tryCreate(reversed.span(), Type::Double);
    FFI_CHECK(!!d);
    if (d) {
        FFI_CHECK(d.get() != a.get());
        FFI_CHECK(d->toString() == "f64(f64,i32)"_s);
    }

    Vector<Type> withJSValue { Type::Int32, Type::JSValue };
    RefPtr<FFI::Signature> jsvalueSignature = FFI::Signature::tryCreate(withJSValue.span(), Type::JSValue);
    FFI_CHECK(!!jsvalueSignature);
    if (jsvalueSignature) {
        FFI_CHECK_EQ(jsvalueSignature->argumentCount(), 2u);
        FFI_CHECK_EQ(jsvalueSignature->slotCount(), 3u);
        FFI_CHECK(jsvalueSignature->toString() == "jsvalue(i32,jsvalue)"_s);
    }

    RefPtr<FFI::Signature> empty = FFI::Signature::tryCreate({ }, Type::Void);
    FFI_CHECK(!!empty);
    if (empty) {
        FFI_CHECK_EQ(empty->argumentCount(), 0u);
        FFI_CHECK_EQ(empty->slotCount(), 1u);
        FFI_CHECK(empty->toString() == "void()"_s);
    }

    Vector<Type> thirtyTwo(FillWith { }, 32, Type::Int32);
    Vector<Type> thirtyThree(FillWith { }, 33, Type::Int32);
    FFI_CHECK(!!FFI::Signature::tryCreate(thirtyTwo.span(), Type::Int64));
    FFI_CHECK(!FFI::Signature::tryCreate(thirtyThree.span(), Type::Int64));
    FFI_CHECK_EQ(FFI::Signature::maxArguments, 32u);

    Vector<Type> voidArg { Type::Int32, Type::Void };
    FFI_CHECK(!FFI::Signature::tryCreate(voidArg.span(), Type::Int32));
    Vector<Type> oneInt { Type::Int32 };
    Vector<Type> reservedArg { Type::RESERVED_WasNapiEnv };
    FFI_CHECK(!FFI::Signature::tryCreate(oneInt.span(), Type::RESERVED_WasNapiEnv));
    FFI_CHECK(!FFI::Signature::tryCreate(reservedArg.span(), Type::Int32));
    FFI_CHECK(!FFI::Signature::tryCreate(oneInt.span(), Type::Buffer));
    Vector<Type> bufferLengthArg { Type::Pointer, Type::BufferLength };
    FFI_CHECK(!FFI::Signature::tryCreate(oneInt.span(), Type::BufferLength));
    FFI_CHECK(!!FFI::Signature::tryCreate(bufferLengthArg.span(), Type::Uint64));
    FFI_CHECK(!!FFI::Signature::tryCreate(oneInt.span(), Type::JSValue));
    FFI_CHECK(!!FFI::Signature::tryCreate(oneInt.span(), Type::Void));

    Vector<Type> everyArg {
        Type::Char, Type::Int8, Type::Uint8, Type::Int16, Type::Uint16, Type::Int32, Type::Uint32,
        Type::Int64, Type::Uint64, Type::Double, Type::Float, Type::Bool, Type::Pointer, Type::CString,
        Type::Int64Fast, Type::Uint64Fast, Type::Function, Type::JSValue, Type::Buffer, Type::BufferLength,
    };
    RefPtr<FFI::Signature> every = FFI::Signature::tryCreate(everyArg.span(), Type::Char);
    FFI_CHECK(!!every);
    if (every) {
        FFI_CHECK_EQ(every->argumentCount(), 20u);
        FFI_CHECK(every->toString() == "char(char,i8,u8,i16,u16,i32,u32,i64,u64,f64,f32,bool,ptr,cstring,i64_fast,u64_fast,function,jsvalue,buffer,buffer_length)"_s);
    }
}

struct GoldenCase {
    ASCIILiteral name;
    Vector<FFI::Type> arguments;
    FFI::Type returnType;
    ASCIILiteral sysv64;
    ASCIILiteral aapcs64EightByteSlots;
    ASCIILiteral darwinARM64Packed;
    ASCIILiteral win64;
};

struct ParsedGolden {
    struct Location {
        FFI::ArgLocation::Kind kind;
        unsigned index; // register index or stack offset
    };
    Vector<Location> locations;
    unsigned stackBytes { 0 };
    FFI::ArgClass returnClass { FFI::ArgClass::Void };
};

static ParsedGolden parseGolden(ASCIILiteral literal)
{
    ParsedGolden result;
    String string(literal);
    for (StringView token : StringView(string).split(' ')) {
        if (token.isEmpty())
            continue;
        char16_t head = token[0];
        StringView rest = token.substring(1);
        if (head == 'g' || head == 'f' || head == 's') {
            std::optional<unsigned> value = parseInteger<unsigned>(rest);
            RELEASE_ASSERT(value);
            ParsedGolden::Location location;
            if (head == 'g')
                location.kind = FFI::ArgLocation::Kind::GPR;
            else if (head == 'f')
                location.kind = FFI::ArgLocation::Kind::FPR;
            else
                location.kind = FFI::ArgLocation::Kind::Stack;
            location.index = *value;
            result.locations.append(location);
            continue;
        }
        if (head == '#') {
            std::optional<unsigned> value = parseInteger<unsigned>(rest);
            RELEASE_ASSERT(value);
            result.stackBytes = *value;
            continue;
        }
        if (head == 'r') {
            RELEASE_ASSERT(rest.length() == 1);
            switch (rest[0]) {
            case 'v': result.returnClass = FFI::ArgClass::Void; break;
            case 'i': result.returnClass = FFI::ArgClass::Int; break;
            case 'f': result.returnClass = FFI::ArgClass::Float; break;
            case 'd': result.returnClass = FFI::ArgClass::Double; break;
            default: RELEASE_ASSERT_NOT_REACHED();
            }
            continue;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return result;
}

static const char* nativeCCName(FFI::NativeCC cc, FFI::StackPacking packing)
{
    switch (cc) {
    case FFI::NativeCC::SysV64: return "SysV64";
    case FFI::NativeCC::AAPCS64:
        return packing == FFI::StackPacking::Natural ? "AAPCS64(Darwin natural packing)" : "AAPCS64(8-byte slots)";
    case FFI::NativeCC::Win64: return "Win64";
    }
    return "?";
}

static void checkLayoutAgainstGolden(const GoldenCase& golden, FFI::NativeCC cc, FFI::StackPacking packing, ASCIILiteral expectedLiteral)
{
    ParsedGolden expected = parseGolden(expectedLiteral);
    RefPtr<FFI::Signature> signature = FFI::Signature::tryCreate(golden.arguments.span(), golden.returnType);
    FFI_CHECK(!!signature);
    if (!signature)
        return;

    FFI::CallLayout layout = FFI::computeCallLayout(cc, packing, *signature, FFI::Direction::Outgoing);
    bool ok = true;
    ok &= layout.cc == cc;
    ok &= layout.packing == packing;
    if (FFI::stackPackingForNativeCC(cc) == packing) {
        FFI::CallLayout hostSelected = FFI::computeCallLayout(cc, *signature, FFI::Direction::Outgoing);
        ok &= hostSelected.stackBytes == layout.stackBytes;
        ok &= hostSelected.packing == packing;
        ok &= hostSelected.arguments.size() == layout.arguments.size();
        for (unsigned i = 0; ok && i < layout.arguments.size(); ++i) {
            ok &= hostSelected.arguments[i].kind == layout.arguments[i].kind;
            ok &= hostSelected.arguments[i].regIndex == layout.arguments[i].regIndex;
            ok &= hostSelected.arguments[i].stackOffset == layout.arguments[i].stackOffset;
        }
    }
    ok &= layout.arguments.size() == expected.locations.size();
    ok &= layout.arguments.size() == golden.arguments.size();
    ok &= layout.stackBytes == expected.stackBytes;
    ok &= !(layout.stackBytes % 16);
    ok &= layout.returnClass == expected.returnClass;
    ok &= layout.returnClass == FFI::argClass(golden.returnType);
    if (ok) {
        for (unsigned i = 0; i < layout.arguments.size(); ++i) {
            const FFI::ArgLocation& location = layout.arguments[i];
            const ParsedGolden::Location& want = expected.locations[i];
            ok &= location.type == golden.arguments[i];
            ok &= location.kind == want.kind;
            if (!ok)
                break;
            switch (location.kind) {
            case FFI::ArgLocation::Kind::GPR:
            case FFI::ArgLocation::Kind::FPR:
                ok &= location.regIndex == want.index;
                break;
            case FFI::ArgLocation::Kind::Stack:
                ok &= location.stackOffset == want.index;
                ok &= location.stackOffset + FFI::nativeSizeInBytes(location.type) <= layout.stackBytes;
                break;
            }
            if (!ok)
                break;
        }
    }
    s_checkCount++;
    if (!ok) {
        s_failureCount++;
        dataLogLn("    FAIL: layout of ", golden.name.characters(), " for ", nativeCCName(cc, packing), " -- expected \"", expectedLiteral.characters(), "\"");
        dataLog("        got:");
        for (const FFI::ArgLocation& location : layout.arguments) {
            switch (location.kind) {
            case FFI::ArgLocation::Kind::GPR: dataLog(" g", static_cast<unsigned>(location.regIndex)); break;
            case FFI::ArgLocation::Kind::FPR: dataLog(" f", static_cast<unsigned>(location.regIndex)); break;
            case FFI::ArgLocation::Kind::Stack: dataLog(" s", location.stackOffset); break;
            }
        }
        dataLogLn(" #", layout.stackBytes, " returnClass=", static_cast<unsigned>(layout.returnClass), " cc=", static_cast<unsigned>(layout.cc));
    }

    FFI::CallLayout incoming = FFI::computeCallLayout(cc, packing, *signature, FFI::Direction::Incoming);
    FFI_CHECK_EQ(incoming.arguments.size(), layout.arguments.size());
    for (unsigned i = 0; i < layout.arguments.size() && i < incoming.arguments.size(); ++i) {
        FFI_CHECK(incoming.arguments[i].kind == layout.arguments[i].kind);
        if (layout.arguments[i].kind == FFI::ArgLocation::Kind::Stack) {
            FFI_CHECK_EQ(unsigned(incoming.arguments[i].stackOffset), unsigned(layout.arguments[i].stackOffset));
            FFI_CHECK_EQ(FFI::incomingStackOffset(incoming, i), layout.arguments[i].stackOffset + 16u);
        } else
            FFI_CHECK_EQ(unsigned(incoming.arguments[i].regIndex), unsigned(layout.arguments[i].regIndex));
    }
}

static Vector<GoldenCase> buildGoldenCorpus()
{
    using T = FFI::Type;
    Vector<GoldenCase> corpus;
    auto add = [&](ASCIILiteral name, Vector<FFI::Type>&& arguments, FFI::Type returnType, ASCIILiteral sysv, ASCIILiteral aapcs, ASCIILiteral darwin, ASCIILiteral win) {
        corpus.append(GoldenCase { name, WTF::move(arguments), returnType, sysv, aapcs, darwin, win });
    };

    add("void()"_s, { }, T::Void,
        "#0 rv"_s, "#0 rv"_s, "#0 rv"_s, "#32 rv"_s);
    add("i32(i32)"_s, { T::Int32 }, T::Int32,
        "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #32 ri"_s);
    add("f64(f64)"_s, { T::Double }, T::Double,
        "f0 #0 rd"_s, "f0 #0 rd"_s, "f0 #0 rd"_s, "f0 #32 rd"_s);
    add("f32(f32)"_s, { T::Float }, T::Float,
        "f0 #0 rf"_s, "f0 #0 rf"_s, "f0 #0 rf"_s, "f0 #32 rf"_s);
    add("i64(i64)"_s, { T::Int64 }, T::Int64,
        "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #32 ri"_s);
    add("ptr(ptr)"_s, { T::Pointer }, T::Pointer,
        "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #32 ri"_s);
    add("bool(bool)"_s, { T::Bool }, T::Bool,
        "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #32 ri"_s);
    add("char(char)"_s, { T::Char }, T::Char,
        "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #0 ri"_s, "g0 #32 ri"_s);
    add("i64(i32,i32)"_s, { T::Int32, T::Int32 }, T::Int64,
        "g0 g1 #0 ri"_s, "g0 g1 #0 ri"_s, "g0 g1 #0 ri"_s, "g0 g1 #32 ri"_s);
    add("f64(f64,f64)"_s, { T::Double, T::Double }, T::Double,
        "f0 f1 #0 rd"_s, "f0 f1 #0 rd"_s, "f0 f1 #0 rd"_s, "f0 f1 #32 rd"_s);
    add("f64(i32,f64)"_s, { T::Int32, T::Double }, T::Double,
        "g0 f0 #0 rd"_s, "g0 f0 #0 rd"_s, "g0 f0 #0 rd"_s, "g0 f1 #32 rd"_s);
    add("f64(f64,i32)"_s, { T::Double, T::Int32 }, T::Double,
        "f0 g0 #0 rd"_s, "f0 g0 #0 rd"_s, "f0 g0 #0 rd"_s, "f0 g1 #32 rd"_s);
    add("f64(i32,f64,i64,f32,ptr)"_s, { T::Int32, T::Double, T::Int64, T::Float, T::Pointer }, T::Double,
        "g0 f0 g1 f1 g2 #0 rd"_s, "g0 f0 g1 f1 g2 #0 rd"_s, "g0 f0 g1 f1 g2 #0 rd"_s, "g0 f1 g2 f3 s32 #48 rd"_s);
    add("i64(i32x6)"_s, { T::Int32, T::Int32, T::Int32, T::Int32, T::Int32, T::Int32 }, T::Int64,
        "g0 g1 g2 g3 g4 g5 #0 ri"_s, "g0 g1 g2 g3 g4 g5 #0 ri"_s, "g0 g1 g2 g3 g4 g5 #0 ri"_s, "g0 g1 g2 g3 s32 s40 #48 ri"_s);
    add("i64(i32x7)"_s, Vector<T>(FillWith { }, 7, T::Int32), T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 #16 ri"_s, "g0 g1 g2 g3 g4 g5 g6 #0 ri"_s, "g0 g1 g2 g3 g4 g5 g6 #0 ri"_s, "g0 g1 g2 g3 s32 s40 s48 #64 ri"_s);
    add("i64(i32x8)"_s, Vector<T>(FillWith { }, 8, T::Int32), T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 #16 ri"_s, "g0 g1 g2 g3 g4 g5 g6 g7 #0 ri"_s, "g0 g1 g2 g3 g4 g5 g6 g7 #0 ri"_s, "g0 g1 g2 g3 s32 s40 s48 s56 #64 ri"_s);
    add("i64(i32x9)"_s, Vector<T>(FillWith { }, 9, T::Int32), T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 #32 ri"_s, "g0 g1 g2 g3 g4 g5 g6 g7 s0 #16 ri"_s, "g0 g1 g2 g3 g4 g5 g6 g7 s0 #16 ri"_s, "g0 g1 g2 g3 s32 s40 s48 s56 s64 #80 ri"_s);
    add("i64(i32x12)"_s, Vector<T>(FillWith { }, 12, T::Int32), T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 s24 s32 s40 #48 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 s16 s24 #32 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s4 s8 s12 #16 ri"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 s80 s88 #96 ri"_s);
    add("i64(i32x16)"_s, Vector<T>(FillWith { }, 16, T::Int32), T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 s24 s32 s40 s48 s56 s64 s72 #80 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 s16 s24 s32 s40 s48 s56 #64 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s4 s8 s12 s16 s20 s24 s28 #32 ri"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 s80 s88 s96 s104 s112 s120 #128 ri"_s);
    add("f64(f64x8)"_s, Vector<T>(FillWith { }, 8, T::Double), T::Double,
        "f0 f1 f2 f3 f4 f5 f6 f7 #0 rd"_s, "f0 f1 f2 f3 f4 f5 f6 f7 #0 rd"_s, "f0 f1 f2 f3 f4 f5 f6 f7 #0 rd"_s, "f0 f1 f2 f3 s32 s40 s48 s56 #64 rd"_s);
    add("f64(f64x9)"_s, Vector<T>(FillWith { }, 9, T::Double), T::Double,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 #16 rd"_s, "f0 f1 f2 f3 f4 f5 f6 f7 s0 #16 rd"_s, "f0 f1 f2 f3 f4 f5 f6 f7 s0 #16 rd"_s, "f0 f1 f2 f3 s32 s40 s48 s56 s64 #80 rd"_s);
    add("f64(f64x12)"_s, Vector<T>(FillWith { }, 12, T::Double), T::Double,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 s16 s24 #32 rd"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 s16 s24 #32 rd"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 s16 s24 #32 rd"_s,
        "f0 f1 f2 f3 s32 s40 s48 s56 s64 s72 s80 s88 #96 rd"_s);
    add("i32(i32x4)"_s, Vector<T>(FillWith { }, 4, T::Int32), T::Int32,
        "g0 g1 g2 g3 #0 ri"_s, "g0 g1 g2 g3 #0 ri"_s, "g0 g1 g2 g3 #0 ri"_s, "g0 g1 g2 g3 #32 ri"_s);
    add("i32(i32x5)"_s, Vector<T>(FillWith { }, 5, T::Int32), T::Int32,
        "g0 g1 g2 g3 g4 #0 ri"_s, "g0 g1 g2 g3 g4 #0 ri"_s, "g0 g1 g2 g3 g4 #0 ri"_s, "g0 g1 g2 g3 s32 #48 ri"_s);
    add("f64(f64x4)"_s, Vector<T>(FillWith { }, 4, T::Double), T::Double,
        "f0 f1 f2 f3 #0 rd"_s, "f0 f1 f2 f3 #0 rd"_s, "f0 f1 f2 f3 #0 rd"_s, "f0 f1 f2 f3 #32 rd"_s);
    add("f64(f64x5)"_s, Vector<T>(FillWith { }, 5, T::Double), T::Double,
        "f0 f1 f2 f3 f4 #0 rd"_s, "f0 f1 f2 f3 f4 #0 rd"_s, "f0 f1 f2 f3 f4 #0 rd"_s, "f0 f1 f2 f3 s32 #48 rd"_s);
    add("mix_1"_s, { T::Int32, T::Double, T::Int64, T::Float, T::Pointer, T::Uint8, T::Double, T::Int16, T::Double, T::Int32 }, T::Double,
        "g0 f0 g1 f1 g2 g3 f2 g4 f3 g5 #0 rd"_s,
        "g0 f0 g1 f1 g2 g3 f2 g4 f3 g5 #0 rd"_s,
        "g0 f0 g1 f1 g2 g3 f2 g4 f3 g5 #0 rd"_s,
        "g0 f1 g2 f3 s32 s40 s48 s56 s64 s72 #80 rd"_s);
    add("mix_2"_s, { T::Float, T::Int32, T::Float, T::Int32, T::Float, T::Int32, T::Float, T::Int32, T::Float, T::Int32 }, T::Double,
        "f0 g0 f1 g1 f2 g2 f3 g3 f4 g4 #0 rd"_s,
        "f0 g0 f1 g1 f2 g2 f3 g3 f4 g4 #0 rd"_s,
        "f0 g0 f1 g1 f2 g2 f3 g3 f4 g4 #0 rd"_s,
        "f0 g1 f2 g3 s32 s40 s48 s56 s64 s72 #80 rd"_s);
    add("mix_3"_s, { T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Int32 }, T::Double,
        "f0 f1 f2 f3 f4 f5 f6 f7 g0 #0 rd"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 g0 #0 rd"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 g0 #0 rd"_s,
        "f0 f1 f2 f3 s32 s40 s48 s56 s64 #80 rd"_s);
    add("mix_4"_s, { T::Int64, T::Int64, T::Int64, T::Int64, T::Int64, T::Int64, T::Double, T::Int64, T::Double }, T::Double,
        "g0 g1 g2 g3 g4 g5 f0 s0 f1 #16 rd"_s,
        "g0 g1 g2 g3 g4 g5 f0 g6 f1 #0 rd"_s,
        "g0 g1 g2 g3 g4 g5 f0 g6 f1 #0 rd"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 #80 rd"_s);
    add("mix_5"_s, { T::Uint8, T::Int8, T::Uint16, T::Int16, T::Uint32, T::Int32, T::Uint64, T::Int64 }, T::Double,
        "g0 g1 g2 g3 g4 g5 s0 s8 #16 rd"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 #0 rd"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 #0 rd"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 #64 rd"_s);
    add("mix_6"_s, { T::Bool, T::Bool, T::Int32, T::Bool, T::Double, T::Bool, T::Float, T::Bool, T::Bool, T::Bool, T::Bool, T::Bool, T::Bool }, T::Double,
        "g0 g1 g2 g3 f0 g4 f1 g5 s0 s8 s16 s24 s32 #48 rd"_s,
        "g0 g1 g2 g3 f0 g4 f1 g5 g6 g7 s0 s8 s16 #32 rd"_s,
        "g0 g1 g2 g3 f0 g4 f1 g5 g6 g7 s0 s1 s2 #16 rd"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 s80 s88 s96 #112 rd"_s);
    add("mix_7"_s, { T::Pointer, T::Char, T::Pointer, T::Char, T::Pointer, T::Char, T::Pointer, T::Char, T::Pointer, T::Char }, T::Double,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 s24 #32 rd"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 #16 rd"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 #16 rd"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 #80 rd"_s);
    add("mix_8"_s, { T::Float, T::Double, T::Float, T::Double, T::Float, T::Double, T::Float, T::Double, T::Float, T::Double, T::Float, T::Double }, T::Double,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 s16 s24 #32 rd"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 s16 s24 #32 rd"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 s16 s24 #32 rd"_s,
        "f0 f1 f2 f3 s32 s40 s48 s56 s64 s72 s80 s88 #96 rd"_s);
    add("i64(u8x10)"_s, Vector<T>(FillWith { }, 10, T::Uint8), T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 s24 #32 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 #16 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s1 #16 ri"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 #80 ri"_s);
    add("i64(i16x10)"_s, Vector<T>(FillWith { }, 10, T::Int16), T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 s24 #32 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 #16 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s2 #16 ri"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 #80 ri"_s);
    add("i64(u8x12)"_s, Vector<T>(FillWith { }, 12, T::Uint8), T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 s24 s32 s40 #48 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 s16 s24 #32 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s1 s2 s3 #16 ri"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 s80 s88 #96 ri"_s);
    add("i64(i16x12)"_s, Vector<T>(FillWith { }, 12, T::Int16), T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 s24 s32 s40 #48 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 s16 s24 #32 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s2 s4 s6 #16 ri"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 s80 s88 #96 ri"_s);
    add("f32(f32x10)"_s, Vector<T>(FillWith { }, 10, T::Float), T::Float,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 #16 rf"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 #16 rf"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s4 #16 rf"_s,
        "f0 f1 f2 f3 s32 s40 s48 s56 s64 s72 #80 rf"_s);
    add("u64(cstring,u64,i64_fast,u64_fast,function,buffer,jsvalue)"_s,
        { T::CString, T::Uint64, T::Int64Fast, T::Uint64Fast, T::Function, T::Buffer, T::JSValue }, T::Uint64,
        "g0 g1 g2 g3 g4 g5 s0 #16 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 #0 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 #0 ri"_s,
        "g0 g1 g2 g3 s32 s40 s48 #64 ri"_s);
    add("i64(i64x8,i8,i8,i8)"_s,
        { T::Int64, T::Int64, T::Int64, T::Int64, T::Int64, T::Int64, T::Int64, T::Int64, T::Int8, T::Int8, T::Int8 }, T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 s24 s32 #48 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 s16 #32 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s1 s2 #16 ri"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 s80 #96 ri"_s);
    add("f32(f64x8,f32,f32,f32)"_s,
        { T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Float, T::Float, T::Float }, T::Float,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 s16 #32 rf"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s8 s16 #32 rf"_s,
        "f0 f1 f2 f3 f4 f5 f6 f7 s0 s4 s8 #16 rf"_s,
        "f0 f1 f2 f3 s32 s40 s48 s56 s64 s72 s80 #96 rf"_s);
    add("i64(i32x8,i8,i32,i8,i64)"_s,
        { T::Int32, T::Int32, T::Int32, T::Int32, T::Int32, T::Int32, T::Int32, T::Int32, T::Int8, T::Int32, T::Int8, T::Int64 }, T::Int64,
        "g0 g1 g2 g3 g4 g5 s0 s8 s16 s24 s32 s40 #48 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s8 s16 s24 #32 ri"_s,
        "g0 g1 g2 g3 g4 g5 g6 g7 s0 s4 s8 s16 #32 ri"_s,
        "g0 g1 g2 g3 s32 s40 s48 s56 s64 s72 s80 s88 #96 ri"_s);
    return corpus;
}

static void testCallLayoutGoldens()
{
    Vector<GoldenCase> corpus = buildGoldenCorpus();
    FFI_CHECK(corpus.size() >= 40);

    for (const GoldenCase& golden : corpus) {
        checkLayoutAgainstGolden(golden, FFI::NativeCC::SysV64, FFI::StackPacking::EightByteSlots, golden.sysv64);
        checkLayoutAgainstGolden(golden, FFI::NativeCC::Win64, FFI::StackPacking::EightByteSlots, golden.win64);
        checkLayoutAgainstGolden(golden, FFI::NativeCC::AAPCS64, FFI::StackPacking::EightByteSlots, golden.aapcs64EightByteSlots);
        checkLayoutAgainstGolden(golden, FFI::NativeCC::AAPCS64, FFI::StackPacking::Natural, golden.darwinARM64Packed);
    }
#if OS(DARWIN) && CPU(ARM64)
    FFI_CHECK(FFI::stackPackingForNativeCC(FFI::NativeCC::AAPCS64) == FFI::StackPacking::Natural);
#else
    FFI_CHECK(FFI::stackPackingForNativeCC(FFI::NativeCC::AAPCS64) == FFI::StackPacking::EightByteSlots);
#endif
    FFI_CHECK(FFI::stackPackingForNativeCC(FFI::NativeCC::SysV64) == FFI::StackPacking::EightByteSlots);
    FFI_CHECK(FFI::stackPackingForNativeCC(FFI::NativeCC::Win64) == FFI::StackPacking::EightByteSlots);

    FFI_CHECK_EQ(FFI::shadowStackBytes(FFI::NativeCC::SysV64), 0u);
    FFI_CHECK_EQ(FFI::shadowStackBytes(FFI::NativeCC::AAPCS64), 0u);
    FFI_CHECK_EQ(FFI::shadowStackBytes(FFI::NativeCC::Win64), 32u);
#if OS(WINDOWS) && CPU(X86_64)
    FFI_CHECK(FFI::hostNativeCC() == FFI::NativeCC::Win64);
#elif CPU(ARM64)
    FFI_CHECK(FFI::hostNativeCC() == FFI::NativeCC::AAPCS64);
#else
    FFI_CHECK(FFI::hostNativeCC() == FFI::NativeCC::SysV64);
#endif

#if CPU(X86_64)
    {
        auto ints = FFI::integerArgumentRegisters(FFI::NativeCC::SysV64);
        auto floats = FFI::floatArgumentRegisters(FFI::NativeCC::SysV64);
        FFI_CHECK_EQ(ints.size(), 6u);
        FFI_CHECK_EQ(floats.size(), 8u);
        if (ints.size() == 6) {
            FFI_CHECK(ints[0] == X86Registers::edi);
            FFI_CHECK(ints[1] == X86Registers::esi);
            FFI_CHECK(ints[2] == X86Registers::edx);
            FFI_CHECK(ints[3] == X86Registers::ecx);
            FFI_CHECK(ints[4] == X86Registers::r8);
            FFI_CHECK(ints[5] == X86Registers::r9);
        }
        for (unsigned i = 0; i < floats.size() && i < 8; ++i)
            FFI_CHECK(floats[i] == static_cast<FPRReg>(X86Registers::xmm0 + i));

        auto winInts = FFI::integerArgumentRegisters(FFI::NativeCC::Win64);
        auto winFloats = FFI::floatArgumentRegisters(FFI::NativeCC::Win64);
        FFI_CHECK_EQ(winInts.size(), 4u);
        FFI_CHECK_EQ(winFloats.size(), 4u);
        if (winInts.size() == 4) {
            FFI_CHECK(winInts[0] == X86Registers::ecx);
            FFI_CHECK(winInts[1] == X86Registers::edx);
            FFI_CHECK(winInts[2] == X86Registers::r8);
            FFI_CHECK(winInts[3] == X86Registers::r9);
        }
        for (unsigned i = 0; i < winFloats.size() && i < 4; ++i)
            FFI_CHECK(winFloats[i] == static_cast<FPRReg>(X86Registers::xmm0 + i));

        auto scratch = FFI::scratchGPRsForInvoke(FFI::hostNativeCC());
        FFI_CHECK(scratch[0] == X86Registers::ebx);
        FFI_CHECK(scratch[1] == X86Registers::r10);
        FFI_CHECK(scratch[0] != X86Registers::r11 && scratch[1] != X86Registers::r11);
        FFI_CHECK(scratch[0] != GPRInfo::returnValueGPR && scratch[1] != GPRInfo::returnValueGPR);
        for (GPRReg reg : FFI::integerArgumentRegisters(FFI::hostNativeCC()))
            FFI_CHECK(reg != scratch[0] && reg != scratch[1]);
    }
#elif CPU(ARM64)
    {
        auto ints = FFI::integerArgumentRegisters(FFI::NativeCC::AAPCS64);
        auto floats = FFI::floatArgumentRegisters(FFI::NativeCC::AAPCS64);
        FFI_CHECK_EQ(ints.size(), 8u);
        FFI_CHECK_EQ(floats.size(), 8u);
        for (unsigned i = 0; i < ints.size() && i < 8; ++i)
            FFI_CHECK(ints[i] == static_cast<GPRReg>(ARM64Registers::x0 + i));
        for (unsigned i = 0; i < floats.size() && i < 8; ++i)
            FFI_CHECK(floats[i] == static_cast<FPRReg>(ARM64Registers::q0 + i));

        auto scratch = FFI::scratchGPRsForInvoke(FFI::hostNativeCC());
        FFI_CHECK(scratch[0] == ARM64Registers::x19);
        FFI_CHECK(scratch[1] == ARM64Registers::x9);
        FFI_CHECK(scratch[0] != ARM64Registers::x16 && scratch[0] != ARM64Registers::x17);
        FFI_CHECK(scratch[1] != ARM64Registers::x16 && scratch[1] != ARM64Registers::x17);
        FFI_CHECK(scratch[0] != GPRInfo::returnValueGPR && scratch[1] != GPRInfo::returnValueGPR);
        for (GPRReg reg : FFI::integerArgumentRegisters(FFI::hostNativeCC()))
            FFI_CHECK(reg != scratch[0] && reg != scratch[1]);
    }
#endif
}

struct ReferenceLocation {
    FFI::ArgLocation::Kind kind;
    unsigned index { 0 };
    unsigned offset { 0 };
};

struct ReferenceLayout {
    Vector<ReferenceLocation> locations;
    unsigned stackBytes { 0 };
};

static ReferenceLayout referenceLayout(FFI::NativeCC cc, FFI::StackPacking packing, std::span<const FFI::Type> arguments)
{
    ReferenceLayout result;
    bool darwinPacking = cc == FFI::NativeCC::AAPCS64 && packing == FFI::StackPacking::Natural;
    unsigned gprLimit = cc == FFI::NativeCC::SysV64 ? 6 : 8;
    unsigned fprLimit = 8;
    unsigned gprIndex = 0;
    unsigned fprIndex = 0;
    unsigned nextStackAddress = 0;
    for (unsigned i = 0; i < arguments.size(); ++i) {
        FFI::Type type = arguments[i];
        FFI::ArgClass klass = FFI::argClass(type);
        bool isFloatClass = klass == FFI::ArgClass::Float || klass == FFI::ArgClass::Double;
        ReferenceLocation location;
        if (cc == FFI::NativeCC::Win64) {
            if (i < 4) {
                location.kind = isFloatClass ? FFI::ArgLocation::Kind::FPR : FFI::ArgLocation::Kind::GPR;
                location.index = i;
            } else {
                location.kind = FFI::ArgLocation::Kind::Stack;
                location.offset = 32 + (i - 4) * 8;
            }
        } else if (isFloatClass && fprIndex < fprLimit) {
            location.kind = FFI::ArgLocation::Kind::FPR;
            location.index = fprIndex++;
        } else if (!isFloatClass && gprIndex < gprLimit) {
            location.kind = FFI::ArgLocation::Kind::GPR;
            location.index = gprIndex++;
        } else {
            unsigned size = darwinPacking ? FFI::nativeSizeInBytes(type) : 8;
            unsigned offset = roundUpToMultipleOf(size, nextStackAddress);
            location.kind = FFI::ArgLocation::Kind::Stack;
            location.offset = offset;
            nextStackAddress = offset + size;
        }
        result.locations.append(location);
    }
    if (cc == FFI::NativeCC::Win64) {
        unsigned stackArgumentCount = arguments.size() > 4 ? arguments.size() - 4 : 0;
        result.stackBytes = roundUpToMultipleOf(16u, 32 + stackArgumentCount * 8);
    } else
        result.stackBytes = roundUpToMultipleOf(16u, nextStackAddress);
    return result;
}

static void testCallLayoutAgainstReferenceModel()
{
    static constexpr FFI::Type argumentTypes[] = {
        FFI::Type::Char, FFI::Type::Int8, FFI::Type::Uint8, FFI::Type::Int16, FFI::Type::Uint16,
        FFI::Type::Int32, FFI::Type::Uint32, FFI::Type::Int64, FFI::Type::Uint64, FFI::Type::Double,
        FFI::Type::Float, FFI::Type::Bool, FFI::Type::Pointer, FFI::Type::CString, FFI::Type::Int64Fast,
        FFI::Type::Uint64Fast, FFI::Type::Function, FFI::Type::JSValue, FFI::Type::Buffer, FFI::Type::BufferLength,
    };
    static constexpr FFI::Type returnTypes[] = {
        FFI::Type::Void, FFI::Type::Int32, FFI::Type::Uint32, FFI::Type::Int64, FFI::Type::Double, FFI::Type::Float, FFI::Type::Bool, FFI::Type::Pointer,
    };
    static constexpr struct { FFI::NativeCC cc; FFI::StackPacking packing; } modes[] = {
        { FFI::NativeCC::SysV64, FFI::StackPacking::EightByteSlots },
        { FFI::NativeCC::AAPCS64, FFI::StackPacking::EightByteSlots },
        { FFI::NativeCC::AAPCS64, FFI::StackPacking::Natural },
        { FFI::NativeCC::Win64, FFI::StackPacking::EightByteSlots },
    };

    WeakRandom random(0x5EEDCA11u);
    unsigned mismatches = 0;
    for (unsigned iteration = 0; iteration < 400; ++iteration) {
        unsigned count = random.getUint32(FFI::Signature::maxArguments + 1);
        Vector<FFI::Type> arguments;
        for (unsigned i = 0; i < count; ++i)
            arguments.append(argumentTypes[random.getUint32(std::size(argumentTypes))]);
        FFI::Type returnType = returnTypes[random.getUint32(std::size(returnTypes))];
        RefPtr<FFI::Signature> signature = FFI::Signature::tryCreate(arguments.span(), returnType);
        FFI_CHECK(!!signature);
        if (!signature)
            continue;
        for (const auto& mode : modes) {
            FFI::NativeCC cc = mode.cc;
            FFI::CallLayout layout = FFI::computeCallLayout(cc, mode.packing, *signature, FFI::Direction::Outgoing);
            ReferenceLayout reference = referenceLayout(cc, mode.packing, arguments.span());
            bool ok = layout.arguments.size() == reference.locations.size()
                && layout.stackBytes == reference.stackBytes
                && !(layout.stackBytes % 16)
                && layout.returnClass == FFI::argClass(returnType)
                && layout.cc == cc
                && layout.packing == mode.packing;
            if (ok) {
                for (unsigned i = 0; i < layout.arguments.size(); ++i) {
                    ok &= layout.arguments[i].kind == reference.locations[i].kind;
                    ok &= layout.arguments[i].type == arguments[i];
                    if (!ok)
                        break;
                    if (layout.arguments[i].kind == FFI::ArgLocation::Kind::Stack)
                        ok &= layout.arguments[i].stackOffset == reference.locations[i].offset;
                    else
                        ok &= layout.arguments[i].regIndex == reference.locations[i].index;
                    if (!ok)
                        break;
                }
            }
            s_checkCount++;
            if (!ok) {
                s_failureCount++;
                if (++mismatches < 10)
                    dataLogLn("    FAIL: layout mismatch vs reference model for ", signature->toString(), " under ", nativeCCName(cc, mode.packing));
            }
        }
    }
}

static void testDoubleToInt64()
{
    constexpr int64_t indefinite = std::numeric_limits<int64_t>::min(); // 0x8000000000000000
    UNUSED_PARAM(indefinite); // only referenced by the CPU-specific branch below
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    constexpr double twoTo63 = 9223372036854775808.0;
    constexpr double twoTo64 = 18446744073709551616.0;

    FFI_CHECK_EQ(FFI::doubleToInt64(0.0), 0);
    FFI_CHECK_EQ(FFI::doubleToInt64(-0.0), 0);
    FFI_CHECK_EQ(FFI::doubleToInt64(-1.5), -1);
    FFI_CHECK_EQ(FFI::doubleToInt64(1.5), 1);
    FFI_CHECK_EQ(FFI::doubleToInt64(2147483648.5), 2147483648ll);
    FFI_CHECK_EQ(FFI::doubleToInt64(-2147483649.75), -2147483649ll);
    FFI_CHECK_EQ(FFI::doubleToInt64(-twoTo63), indefinite); // exact -2^63
    FFI_CHECK_EQ(FFI::doubleToInt64(9223372036854774784.0), 9223372036854774784ll); // largest double < 2^63
    FFI_CHECK_EQ(FFI::doubleToInt64(4294967296.0), 4294967296ll);
    FFI_CHECK_EQ(FFI::doubleToInt64(0.9999999999999999), 0);
    FFI_CHECK_EQ(FFI::doubleToInt64(-0.9999999999999999), 0);

#if CPU(X86_64)
    FFI_CHECK_EQ(FFI::doubleToInt64(nan), indefinite);
    FFI_CHECK_EQ(FFI::doubleToInt64(inf), indefinite);
    FFI_CHECK_EQ(FFI::doubleToInt64(-inf), indefinite);
    FFI_CHECK_EQ(FFI::doubleToInt64(twoTo63), indefinite);
    FFI_CHECK_EQ(FFI::doubleToInt64(twoTo64), indefinite);
    FFI_CHECK_EQ(FFI::doubleToInt64(-twoTo64), indefinite);
#elif CPU(ARM64)
    FFI_CHECK_EQ(FFI::doubleToInt64(nan), 0);
    FFI_CHECK_EQ(FFI::doubleToInt64(inf), std::numeric_limits<int64_t>::max());
    FFI_CHECK_EQ(FFI::doubleToInt64(-inf), std::numeric_limits<int64_t>::min());
    FFI_CHECK_EQ(FFI::doubleToInt64(twoTo63), std::numeric_limits<int64_t>::max());
    FFI_CHECK_EQ(FFI::doubleToInt64(twoTo64), std::numeric_limits<int64_t>::max());
    FFI_CHECK_EQ(FFI::doubleToInt64(-twoTo64), std::numeric_limits<int64_t>::min());
#endif
}

enum class ExpectThrow : bool { No, Yes };

static bool convertToSlot(FFI::Type type, JSValue value, uint64_t& slotOut, ExpectThrow expectThrow)
{
    VM& vm = *s_vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    FFI::StringArena::Scope arenaScope(ffiContext());
    slotOut = 0xAAAAAAAAAAAAAAAAull;
    bool ok = FFI::writeSlotFromJSValue(s_globalObject, ffiContext(), type, value, slotOut, testStringArena());
    Exception* exception = scope.exception();
    if (exception) {
        JSValue error = exception->value();
        scope.clearException();
        s_checkCount++;
        if (expectThrow == ExpectThrow::No) {
            s_failureCount++;
            dataLogLn("    FAIL: writeSlotFromJSValue(", typeNameForLog(type), ") threw unexpectedly");
        }
        ErrorInstance* errorInstance = dynamicDowncast<ErrorInstance>(error);
        FFI_CHECK(errorInstance);
        if (errorInstance)
            FFI_CHECK(errorInstance->errorType() == ErrorType::TypeError);
        FFI_CHECK(!ok);
        return false;
    }
    s_checkCount++;
    if (expectThrow == ExpectThrow::Yes) {
        s_failureCount++;
        dataLogLn("    FAIL: writeSlotFromJSValue(", typeNameForLog(type), ") should have thrown");
    }
    FFI_CHECK(ok);
    return ok;
}

static void expectSlot(FFI::Type type, JSValue value, uint64_t expected)
{
    uint64_t slot = 0;
    if (!convertToSlot(type, value, slot, ExpectThrow::No))
        return;
    s_checkCount++;
    if (slot != expected) {
        s_failureCount++;
        dataLogLn("    FAIL: writeSlotFromJSValue(", typeNameForLog(type), ") slot ", RawHex(slot), " != expected ", RawHex(expected));
    }
}

static void expectSlotThrows(FFI::Type type, JSValue value)
{
    uint64_t slot = 0;
    convertToSlot(type, value, slot, ExpectThrow::Yes);
}

static JSValue slotToJS(FFI::Type type, uint64_t slot)
{
    VM& vm = *s_vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue result = FFI::jsValueFromSlot(s_globalObject, ffiContext(), type, slot);
    Exception* exception = scope.exception();
    if (exception) {
        s_checkCount++;
        s_failureCount++;
        dataLogLn("    FAIL: jsValueFromSlot(", typeNameForLog(type), ") threw");
        scope.clearException();
        return jsUndefined();
    }
    return result;
}

static void expectSlotToNumber(FFI::Type type, uint64_t slot, double expected)
{
    JSValue value = slotToJS(type, slot);
    s_checkCount++;
    if (!value.isNumber()) {
        s_failureCount++;
        dataLogLn("    FAIL: jsValueFromSlot(", typeNameForLog(type), ", ", RawHex(slot), ") is not a number");
        return;
    }
    double actual = value.asNumber();
    bool equal = (std::isnan(expected) && std::isnan(actual))
        || (actual == expected && !!std::signbit(actual) == !!std::signbit(expected));
    if (!equal) {
        s_failureCount++;
        dataLogLn("    FAIL: jsValueFromSlot(", typeNameForLog(type), ", ", RawHex(slot), ") -> ", actual, ", expected ", expected);
    }
}

static void expectSlotToBigInt(FFI::Type type, uint64_t slot, uint64_t expectedBits)
{
    JSValue value = slotToJS(type, slot);
    s_checkCount++;
    if (!value.isBigInt()) {
        s_failureCount++;
        dataLogLn("    FAIL: jsValueFromSlot(", typeNameForLog(type), ", ", RawHex(slot), ") is not a BigInt");
        return;
    }
    uint64_t actualBits = FFI::isSigned(type)
        ? static_cast<uint64_t>(JSBigInt::toBigInt64(value))
        : JSBigInt::toBigUInt64(value);
    if (actualBits != expectedBits) {
        s_failureCount++;
        dataLogLn("    FAIL: jsValueFromSlot(", typeNameForLog(type), ", ", RawHex(slot), ") bits ", RawHex(actualBits), " != ", RawHex(expectedBits));
    }
}

static JSUint8Array* makeUint8Array(unsigned length)
{
    return JSUint8Array::create(s_globalObject, s_globalObject->typedArrayStructure(TypeUint8, false), length);
}

static void testConversions()
{
    VM& vm = *s_vm;
    JSGlobalObject* globalObject = s_globalObject;
    using T = FFI::Type;

    FFI::StringArena::Scope arenaScope(ffiContext());

    constexpr uint64_t allOnes = ~static_cast<uint64_t>(0);
    const double nan = PNaN; // JSC's canonical (pure) NaN, runtime/PureNaN.h
    const double inf = std::numeric_limits<double>::infinity();

    expectSlot(T::Int32, jsNumber(2147483647), 0x000000007fffffffull);
    expectSlot(T::Int32, jsNumber(-2147483647 - 1), 0xffffffff80000000ull);
    expectSlot(T::Int32, jsNumber(0), 0);
    expectSlot(T::Int32, jsNumber(-1), allOnes);
    expectSlot(T::Int32, jsNumber(4294967301.0), 5); // 2^32 + 5 wraps
    expectSlot(T::Int32, jsNumber(2147483648.0), 0xffffffff80000000ull); // wraps to INT32_MIN
    expectSlot(T::Int32, jsNumber(-2147483649.0), 0x000000007fffffffull);
    expectSlot(T::Int32, jsNumber(0.9), 0);
    expectSlot(T::Int32, jsNumber(-0.9), 0);
    expectSlot(T::Int32, jsNumber(-0.0), 0);
    expectSlot(T::Int32, jsNumber(nan), 0);
    expectSlot(T::Int32, jsNumber(inf), 0);
    expectSlot(T::Int32, jsNumber(-inf), 0);
    expectSlot(T::Int32, jsUndefined(), 0);
    expectSlot(T::Int32, jsNull(), 0);
    expectSlot(T::Int32, jsBoolean(true), 1);
    expectSlot(T::Int32, jsBoolean(false), 0);
    expectSlot(T::Uint32, jsNumber(-1), 0x00000000ffffffffull);
    expectSlot(T::Uint32, jsNumber(4294967295.0), 0x00000000ffffffffull);
    expectSlot(T::Uint32, jsNumber(4294967296.0), 0);
    expectSlot(T::Uint32, jsNumber(2147483648.0), 0x0000000080000000ull);
    expectSlot(T::Uint32, jsNumber(-0.5), 0);
    expectSlot(T::Int8, jsNumber(511), allOnes); // 0x1ff -> low byte 0xff sign-extended
    expectSlot(T::Int8, jsNumber(128), 0xffffffffffffff80ull);
    expectSlot(T::Int8, jsNumber(127), 127);
    expectSlot(T::Int8, jsNumber(-129), 127);
    expectSlot(T::Int8, jsNumber(-1), allOnes);
    expectSlot(T::Char, jsNumber(-1), allOnes); // char is signed on every target
    expectSlot(T::Char, jsNumber(255), allOnes);
    expectSlot(T::Char, jsNumber(200), 0xffffffffffffffc8ull); // 200 -> -56
    expectSlot(T::Uint8, jsNumber(-1), 0xff);
    expectSlot(T::Uint8, jsNumber(256), 0);
    expectSlot(T::Uint8, jsNumber(511), 0xff);
    expectSlot(T::Uint8, jsNumber(255.9), 0xff);
    expectSlot(T::Int16, jsNumber(0x12345), 0x2345);
    expectSlot(T::Int16, jsNumber(32768), 0xffffffffffff8000ull);
    expectSlot(T::Int16, jsNumber(-32769), 0x7fff);
    expectSlot(T::Uint16, jsNumber(-1), 0xffff);
    expectSlot(T::Uint16, jsNumber(65536), 0);
    expectSlot(T::Uint16, jsNumber(70000), 70000 - 65536);

    expectSlot(T::Bool, jsNumber(0), 0);
    expectSlot(T::Bool, jsNumber(2), 1);
    expectSlot(T::Bool, jsNumber(-1), 1);
    expectSlot(T::Bool, jsNumber(0.5), 1);
    expectSlot(T::Bool, jsNumber(-0.0), 0);
    expectSlot(T::Bool, jsNumber(nan), 0);
    expectSlot(T::Bool, jsNumber(inf), 1);
    expectSlot(T::Bool, jsBoolean(true), 1);
    expectSlot(T::Bool, jsBoolean(false), 0);
    expectSlot(T::Bool, jsUndefined(), 0);
    expectSlot(T::Bool, jsNull(), 0);

    expectSlot(T::Int64, jsNumber(1), 1);
    expectSlot(T::Int64, jsNumber(-1), allOnes);
    expectSlot(T::Int64, jsNumber(-2147483647 - 1), 0xffffffff80000000ull);
    expectSlot(T::Int64, jsNumber(4294967296.0), 4294967296ull);
    expectSlot(T::Int64, jsNumber(1e18), 1000000000000000000ull);
    expectSlot(T::Int64, jsNumber(-1.5), allOnes);
    expectSlot(T::Int64, JSValue(JSValue::EncodeAsDouble, -1.0), allOnes); // the same number as jsNumber(-1), boxed as a double
    expectSlot(T::Int64, jsNumber(-4294967297.0), 0xfffffffeffffffffull);
    expectSlot(T::Int64, jsNumber(-9007199254740992.0), 0xffe0000000000000ull); // -2^53
    expectSlot(T::Int64, jsNumber(9223372036854774784.0), 0x7ffffffffffffc00ull); // largest double below 2^63
    expectSlot(T::Int64, jsNumber(-9223372036854775808.0), 0x8000000000000000ull); // INT64_MIN exactly
    // Out of range: the low 64 bits of the truncated value, like Int32 above, on every CPU.
    expectSlot(T::Int64, jsNumber(9223372036854775808.0), 0x8000000000000000ull); // 2^63 wraps to INT64_MIN
    expectSlot(T::Int64, jsNumber(13835058055282163712.0), 0xc000000000000000ull); // 2^63 + 2^62
    expectSlot(T::Int64, jsNumber(18446744073709551616.0), 0); // 2^64
    expectSlot(T::Int64, jsNumber(18446744073709555712.0), 4096); // 2^64 + 2^12 (the next double after 2^64)
    expectSlot(T::Int64, jsNumber(-18446744073709555712.0), static_cast<uint64_t>(-4096));
    expectSlot(T::Int64, jsNumber(1e300), 0); // no mantissa bit lands in the low 64 bits
    expectSlot(T::Int64, jsNumber(nan), 0);
    expectSlot(T::Int64, jsNumber(inf), 0);
    expectSlot(T::Int64, jsNumber(-inf), 0);
    expectSlot(T::Int64, jsNumber(-0.0), 0);
    expectSlot(T::Int64, jsNumber(4.9e-324), 0);
    expectSlot(T::Int64, JSBigInt::createFrom(globalObject, static_cast<int64_t>(std::numeric_limits<int64_t>::max())), 0x7fffffffffffffffull);
    expectSlot(T::Int64, JSBigInt::createFrom(globalObject, static_cast<int64_t>(std::numeric_limits<int64_t>::min())), 0x8000000000000000ull);
    expectSlot(T::Int64, JSBigInt::createFrom(globalObject, static_cast<int64_t>(-1)), allOnes);
    expectSlot(T::Int64, JSBigInt::createFrom(globalObject, static_cast<uint64_t>(0xffffffffffffffffull)), allOnes); // 2^64-1 mod 2^64
    expectSlot(T::Uint64, jsNumber(-1), allOnes);
    expectSlot(T::Uint64, jsNumber(2.5), 2);
    // A Number reaches a u64 with the same bits however it is boxed (int32 / double) and as the BigInt of the same value.
    expectSlot(T::Uint64, JSValue(JSValue::EncodeAsDouble, -1.0), allOnes);
    expectSlot(T::Uint64, jsNumber(-1.5), allOnes);
    expectSlot(T::Uint64, jsNumber(-7.5), allOnes - 6);
    expectSlot(T::Uint64, jsNumber(-4294967297.0), 0xfffffffeffffffffull);
    expectSlot(T::Uint64, JSBigInt::createFrom(globalObject, static_cast<int64_t>(-4294967297ll)), 0xfffffffeffffffffull);
    expectSlot(T::Uint64, jsNumber(9223372036854775808.0), 0x8000000000000000ull); // 2^63 is a plain in-range u64
    expectSlot(T::Uint64, jsNumber(13835058055282163712.0), 0xc000000000000000ull); // 2^63 + 2^62
    expectSlot(T::Uint64, jsNumber(18446744073709549568.0), 0xfffffffffffff800ull); // largest double below 2^64
    expectSlot(T::Uint64, jsNumber(18446744073709551616.0), 0); // 2^64 wraps like Uint32(2^32) above
    expectSlot(T::Uint64, jsNumber(18446744073709555712.0), 4096); // 2^64 + 2^12
    expectSlot(T::Uint64, jsNumber(nan), 0);
    expectSlot(T::Uint64, jsNumber(inf), 0);
    expectSlot(T::Uint64, jsNumber(-inf), 0);
    expectSlot(T::Uint64, jsNumber(-0.5), 0);
    expectSlot(T::Uint64, JSBigInt::createFrom(globalObject, static_cast<uint64_t>(0xdeadbeefcafebabeull)), 0xdeadbeefcafebabeull);
    expectSlot(T::Uint64, JSBigInt::createFrom(globalObject, static_cast<int64_t>(-2)), allOnes - 1); // BigInt(-2) mod 2^64
    expectSlot(T::Int64Fast, jsNumber(-42), std::bit_cast<uint64_t>(static_cast<int64_t>(-42)));
    expectSlot(T::Int64Fast, jsNumber(-1.5), allOnes);
    expectSlot(T::Int64Fast, jsNumber(9223372036854775808.0), 0x8000000000000000ull);
    expectSlot(T::Int64Fast, jsNumber(nan), 0);
    expectSlot(T::Int64Fast, JSBigInt::createFrom(globalObject, static_cast<int64_t>(1) << 60), static_cast<uint64_t>(1) << 60);
    expectSlot(T::Uint64Fast, jsNumber(9007199254740992.0), 9007199254740992ull);
    expectSlot(T::Uint64Fast, jsNumber(-1.5), allOnes);
    expectSlot(T::Uint64Fast, jsNumber(13835058055282163712.0), 0xc000000000000000ull);
    expectSlot(T::Uint64Fast, jsNumber(nan), 0);
    expectSlot(T::Uint64Fast, JSBigInt::createFrom(globalObject, static_cast<uint64_t>(1) << 63), static_cast<uint64_t>(1) << 63);

    expectSlot(T::Double, jsNumber(1.5), std::bit_cast<uint64_t>(1.5));
    expectSlot(T::Double, jsNumber(-0.0), 0x8000000000000000ull);
    expectSlot(T::Double, jsNumber(inf), std::bit_cast<uint64_t>(inf));
    expectSlot(T::Double, jsNumber(-inf), std::bit_cast<uint64_t>(-inf));
    expectSlot(T::Double, jsNumber(2147483647), std::bit_cast<uint64_t>(2147483647.0));
    expectSlot(T::Double, jsNumber(4.9e-324), std::bit_cast<uint64_t>(4.9e-324));
    {
        uint64_t slot = 0;
        if (convertToSlot(T::Double, jsNumber(nan), slot, ExpectThrow::No))
            FFI_CHECK(std::isnan(std::bit_cast<double>(slot)));
        if (convertToSlot(T::Double, jsUndefined(), slot, ExpectThrow::No))
            FFI_CHECK(std::isnan(std::bit_cast<double>(slot))); // Number(undefined) is NaN
    }
    expectSlot(T::Float, jsNumber(1.5), static_cast<uint64_t>(std::bit_cast<uint32_t>(1.5f)));
    expectSlot(T::Float, jsNumber(1.1), static_cast<uint64_t>(std::bit_cast<uint32_t>(1.1f)));
    expectSlot(T::Float, jsNumber(-0.0), 0x0000000080000000ull);
    expectSlot(T::Float, jsNumber(1e40), static_cast<uint64_t>(std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity())));
    expectSlot(T::Float, jsNumber(1e-46), static_cast<uint64_t>(std::bit_cast<uint32_t>(static_cast<float>(1e-46))));
    {
        uint64_t slot = 0;
        if (convertToSlot(T::Float, jsNumber(nan), slot, ExpectThrow::No)) {
            FFI_CHECK_EQ_HEX(slot >> 32, 0); // upper half zero even for NaN
            FFI_CHECK(std::isnan(std::bit_cast<float>(static_cast<uint32_t>(slot))));
        }
    }

    static uint32_t nativeScratch[16];
    JSUint8Array* uint8Array = makeUint8Array(32);
    void* uint8ArrayVector = uint8Array ? uint8Array->vector() : nullptr;
    expectSlot(T::Pointer, jsNumber(0), 0);
    expectSlot(T::Pointer, jsNull(), 0);
    expectSlot(T::Pointer, jsUndefined(), 0);
    expectSlot(T::Pointer, jsNumber(4096), 4096);
    expectSlot(T::Pointer, jsNumber(-1), allOnes); // int32 pointers are sign-extended
    expectSlot(T::Pointer, jsNumber(1099511627776.0), 1099511627776ull); // 2^40 via the double path
    expectSlot(T::Pointer, jsNumber(static_cast<double>(reinterpret_cast<uintptr_t>(&nativeScratch[0]))), reinterpret_cast<uintptr_t>(&nativeScratch[0]));
    if (uint8Array) {
        FFI_CHECK(uint8ArrayVector);
        expectSlot(T::Pointer, uint8Array, reinterpret_cast<uintptr_t>(uint8ArrayVector));
        expectSlot(T::CString, uint8Array, reinterpret_cast<uintptr_t>(uint8ArrayVector));
        expectSlot(T::Buffer, uint8Array, reinterpret_cast<uintptr_t>(uint8ArrayVector));
        expectSlot(T::Function, uint8Array, reinterpret_cast<uintptr_t>(uint8ArrayVector));
    }
    expectSlot(T::CString, jsNumber(0), 0);
    expectSlot(T::Function, jsNumber(0), 0);
    expectSlot(T::Function, jsNumber(65536), 65536);
    expectSlotThrows(T::Pointer, jsString(vm, String("hello"_s)));
    expectSlotThrows(T::Buffer, jsString(vm, String("hello"_s)));
    expectSlotThrows(T::Function, jsString(vm, String("hello"_s)));
    expectSlotThrows(T::Pointer, Symbol::create(vm, SymbolImpl::createNullSymbol().get()));
    expectSlotThrows(T::Int32, Symbol::create(vm, SymbolImpl::createNullSymbol().get()));
    expectSlotThrows(T::Double, Symbol::create(vm, SymbolImpl::createNullSymbol().get()));
    expectSlotThrows(T::Pointer, constructEmptyObject(globalObject));
    expectSlotThrows(T::Buffer, jsNumber(5)); // buffer requires a view
    expectSlotThrows(T::Buffer, jsNull());
    expectSlotThrows(T::Buffer, constructEmptyObject(globalObject));

    {
        uint64_t slot = 0;
        if (convertToSlot(T::CString, jsString(vm, String("hello"_s)), slot, ExpectThrow::No)) {
            FFI_CHECK(slot);
            if (slot)
                FFI_CHECK(!strcmp(reinterpret_cast<const char*>(slot), "hello"));
        }
        if (convertToSlot(T::CString, jsString(vm, emptyString()), slot, ExpectThrow::No)) {
            FFI_CHECK(slot);
            if (slot)
                FFI_CHECK_EQ(strlen(reinterpret_cast<const char*>(slot)), 0u);
        }
        static const char utf8Sample[] = "h\xC3\xA9llo \xE2\x86\x92 \xF0\x9D\x84\x9E"; // "héllo → 𝄞"
        String nonASCII = String::fromUTF8(utf8Sample);
        if (convertToSlot(T::CString, jsString(vm, nonASCII), slot, ExpectThrow::No)) {
            FFI_CHECK(slot);
            if (slot)
                FFI_CHECK(!strcmp(reinterpret_cast<const char*>(slot), utf8Sample));
        }
        JSString* concatenated = jsString(vm, makeString("ro"_s, "pe "_s, 12345));
        if (convertToSlot(T::CString, concatenated, slot, ExpectThrow::No)) {
            FFI_CHECK(slot);
            if (slot)
                FFI_CHECK(!strcmp(reinterpret_cast<const char*>(slot), "rope 12345"));
        }
    }

    {
        JSObject* object = constructEmptyObject(globalObject);
        JSString* string = jsString(vm, String("napi"_s));
        expectSlot(T::JSValue, jsNumber(3.5), static_cast<uint64_t>(JSValue::encode(jsNumber(3.5))));
        expectSlot(T::JSValue, jsBoolean(true), static_cast<uint64_t>(JSValue::encode(jsBoolean(true))));
        expectSlot(T::JSValue, jsUndefined(), static_cast<uint64_t>(JSValue::encode(jsUndefined())));
        expectSlot(T::JSValue, jsNull(), static_cast<uint64_t>(JSValue::encode(jsNull())));
        expectSlot(T::JSValue, object, static_cast<uint64_t>(JSValue::encode(object)));
        expectSlot(T::JSValue, string, static_cast<uint64_t>(JSValue::encode(string)));
        JSValue backObject = slotToJS(T::JSValue, static_cast<uint64_t>(JSValue::encode(object)));
        FFI_CHECK(backObject == JSValue(object));
        JSValue backNumber = slotToJS(T::JSValue, static_cast<uint64_t>(JSValue::encode(jsNumber(-7))));
        FFI_CHECK(backNumber == jsNumber(-7));
    }

    expectSlotToNumber(T::Int8, 0xff, -1);
    expectSlotToNumber(T::Int8, 0xffffffffffffff80ull, -128);
    expectSlotToNumber(T::Int8, 0x7f, 127);
    expectSlotToNumber(T::Char, 0xffffffffffffffffull, -1);
    expectSlotToNumber(T::Uint8, 0xff, 255);
    expectSlotToNumber(T::Uint8, 0, 0);
    expectSlotToNumber(T::Int16, 0xffff, -1);
    expectSlotToNumber(T::Int16, 0x8000, -32768);
    expectSlotToNumber(T::Uint16, 0xffff, 65535);
    expectSlotToNumber(T::Int32, 0xffffffff80000000ull, -2147483648.0);
    expectSlotToNumber(T::Int32, 0x7fffffff, 2147483647);
    expectSlotToNumber(T::Uint32, 0xffffffff, 4294967295.0);
    expectSlotToNumber(T::Uint32, 0x80000000, 2147483648.0);
    expectSlotToNumber(T::Uint32, 0x7fffffff, 2147483647);
    FFI_CHECK(slotToJS(T::Bool, 1) == jsBoolean(true));
    FFI_CHECK(slotToJS(T::Bool, 0) == jsBoolean(false));
    FFI_CHECK(slotToJS(T::Void, 0xdeadbeef) == jsUndefined());

    expectSlotToBigInt(T::Int64, 42, 42);
    expectSlotToBigInt(T::Int64, 0x8000000000000000ull, 0x8000000000000000ull);
    expectSlotToBigInt(T::Int64, allOnes, allOnes);
    expectSlotToBigInt(T::Uint64, allOnes, allOnes);
    expectSlotToBigInt(T::Uint64, 0, 0);
    expectSlotToNumber(T::Int64Fast, static_cast<uint64_t>(9007199254740991ll), 9007199254740991.0); // 2^53-1 stays a Number
    expectSlotToBigInt(T::Int64Fast, static_cast<uint64_t>(9007199254740992ll), 9007199254740992ull); // 2^53 becomes a BigInt
    expectSlotToNumber(T::Int64Fast, std::bit_cast<uint64_t>(static_cast<int64_t>(-9007199254740991ll)), -9007199254740991.0);
    expectSlotToBigInt(T::Int64Fast, std::bit_cast<uint64_t>(static_cast<int64_t>(-9007199254740992ll)), std::bit_cast<uint64_t>(static_cast<int64_t>(-9007199254740992ll)));
    expectSlotToNumber(T::Int64Fast, allOnes, -1.0);
    expectSlotToNumber(T::Uint64Fast, 9007199254740990ull, 9007199254740990.0); // < 2^53-1 stays a Number
    expectSlotToBigInt(T::Uint64Fast, 9007199254740991ull, 9007199254740991ull); // == 2^53-1 becomes a BigInt (strict <, Bun quirk)
    expectSlotToBigInt(T::Uint64Fast, allOnes, allOnes);

    expectSlotToNumber(T::Double, std::bit_cast<uint64_t>(-0.0), -0.0);
    expectSlotToNumber(T::Double, std::bit_cast<uint64_t>(1.5), 1.5);
    expectSlotToNumber(T::Double, std::bit_cast<uint64_t>(inf), inf);
    expectSlotToNumber(T::Double, std::bit_cast<uint64_t>(4.9e-324), 4.9e-324);
    expectSlotToNumber(T::Double, 0x7ff8000000000001ull, nan); // impure NaN must be purified (debug jsNumber ASSERT)
    expectSlotToNumber(T::Double, 0x7ff0000000000001ull, nan); // signaling NaN payload
    expectSlotToNumber(T::Double, 0xfff123456789abcdull, nan);
    expectSlotToNumber(T::Float, 0x7fc00001, nan);
    expectSlotToNumber(T::Float, 0x00000001, static_cast<double>(std::bit_cast<float>(0x00000001u))); // denormal
    expectSlotToNumber(T::Float, static_cast<uint64_t>(std::bit_cast<uint32_t>(-1.5f)), -1.5);
    expectSlotToNumber(T::Float, 0x80000000, -0.0);

    FFI_CHECK(slotToJS(T::Pointer, 0).isNull());
    FFI_CHECK(slotToJS(T::CString, 0).isNull());
    FFI_CHECK(slotToJS(T::Function, 0).isNull());
    FFI_CHECK(slotToJS(T::Buffer, 0).isNull());
    expectSlotToNumber(T::Pointer, 0x00007fffdeadbee0ull, static_cast<double>(0x00007fffdeadbee0ull));
    {
        static const char kHello[] = "hello";
        JSValue decoded = slotToJS(T::CString, reinterpret_cast<uintptr_t>(kHello));
        FFI_CHECK(decoded.isString());
        FFI_CHECK(asString(decoded)->value(s_globalObject) == String::fromUTF8("hello"));
    }
    expectSlotToNumber(T::Function, 1, 1);
    expectSlotToNumber(T::Buffer, 65536, 65536);

    static const int64_t roundTripValues[] = { 0, 1, -1, 127, -128, 255, 32767, -32768, 65535, 2147483647, -2147483647 - 1, static_cast<int64_t>(4294967295u) };
    for (int64_t value : roundTripValues) {
        static const struct { T type; int64_t min; int64_t max; } integerTypes[] = {
            { T::Int8, -128, 127 }, { T::Char, -128, 127 }, { T::Uint8, 0, 255 }, { T::Int16, -32768, 32767 },
            { T::Uint16, 0, 65535 }, { T::Int32, -2147483647ll - 1, 2147483647 }, { T::Uint32, 0, 4294967295ll },
        };
        for (auto& entry : integerTypes) {
            if (value < entry.min || value > entry.max)
                continue;
            uint64_t slot = 0;
            if (!convertToSlot(entry.type, jsNumber(static_cast<double>(value)), slot, ExpectThrow::No))
                continue;
            expectSlotToNumber(entry.type, slot, static_cast<double>(value));
        }
    }
}

static uint64_t s_scratchMemory[64];

template<typename T>
static T slotToNative(uint64_t slot)
{
    if constexpr (std::is_same_v<T, bool>)
        return !!slot;
    else if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(static_cast<uintptr_t>(slot));
    else if constexpr (std::is_same_v<T, float>)
        return std::bit_cast<float>(static_cast<uint32_t>(slot));
    else if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<double>(slot);
    else {
        static_assert(std::is_integral_v<T>);
        return static_cast<T>(slot);
    }
}

template<typename R, typename... Arguments, size_t... Indices>
static R callNativeFromSlotsImpl(R (*function)(Arguments...), const uint64_t* slots, std::index_sequence<Indices...>)
{
    return function(slotToNative<Arguments>(slots[Indices])...);
}

template<typename R, typename... Arguments>
static R callNativeFromSlots(R (*function)(Arguments...), const uint64_t* slots)
{
    return callNativeFromSlotsImpl(function, slots, std::index_sequence_for<Arguments...> { });
}

template<typename R>
static uint64_t nativeReturnRawBits(R value)
{
    if constexpr (std::is_same_v<R, bool>)
        return value ? 1 : 0;
    else if constexpr (std::is_pointer_v<R>)
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
    else if constexpr (std::is_same_v<R, float>)
        return static_cast<uint64_t>(std::bit_cast<uint32_t>(value));
    else if constexpr (std::is_same_v<R, double>)
        return std::bit_cast<uint64_t>(value);
    else if constexpr (std::is_signed_v<R>)
        return static_cast<uint64_t>(static_cast<int64_t>(value));
    else
        return static_cast<uint64_t>(value);
}

static uint64_t canonicalizeSlot(FFI::Type type, uint64_t raw)
{
    switch (type) {
    case FFI::Type::Char:
    case FFI::Type::Int8:
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(raw)));
    case FFI::Type::Uint8:
        return static_cast<uint8_t>(raw);
    case FFI::Type::Int16:
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(raw)));
    case FFI::Type::Uint16:
        return static_cast<uint16_t>(raw);
    case FFI::Type::Int32:
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(raw)));
    case FFI::Type::Uint32:
        return static_cast<uint32_t>(raw);
    case FFI::Type::Bool:
        return static_cast<uint8_t>(raw) ? 1 : 0;
    case FFI::Type::Float:
        return raw & 0xffffffffull;
    case FFI::Type::Int64:
    case FFI::Type::Uint64:
    case FFI::Type::Int64Fast:
    case FFI::Type::Uint64Fast:
    case FFI::Type::Double:
    case FFI::Type::Pointer:
    case FFI::Type::CString:
    case FFI::Type::Function:
    case FFI::Type::Buffer:
    case FFI::Type::BufferLength:
    case FFI::Type::RESERVED_WasNapiEnv:
    case FFI::Type::JSValue:
    case FFI::Type::Void:
        return raw;
    }
    return raw;
}

static uint64_t edgeBitsForType(FFI::Type type, WeakRandom& random)
{
    switch (type) {
    case FFI::Type::Char:
    case FFI::Type::Int8: {
        static const int8_t values[] = { 0, 1, -1, 127, -128, 64, -64, 100 };
        return static_cast<uint64_t>(static_cast<int64_t>(values[random.getUint32(std::size(values))]));
    }
    case FFI::Type::Uint8: {
        static const uint8_t values[] = { 0, 1, 255, 128, 127, 200 };
        return values[random.getUint32(std::size(values))];
    }
    case FFI::Type::Int16: {
        static const int16_t values[] = { 0, 1, -1, 32767, -32768, 0x1234, -0x1234 };
        return static_cast<uint64_t>(static_cast<int64_t>(values[random.getUint32(std::size(values))]));
    }
    case FFI::Type::Uint16: {
        static const uint16_t values[] = { 0, 1, 65535, 32768, 32767, 4660 };
        return values[random.getUint32(std::size(values))];
    }
    case FFI::Type::Int32: {
        static const int32_t values[] = { 0, 1, -1, 2147483647, -2147483647 - 1, 0x12345678, -0x12345678, 65536 };
        return static_cast<uint64_t>(static_cast<int64_t>(values[random.getUint32(std::size(values))]));
    }
    case FFI::Type::Uint32: {
        static const uint32_t values[] = { 0, 1, 4294967295u, 2147483648u, 2147483647u, 0x9abcdef0u };
        return values[random.getUint32(std::size(values))];
    }
    case FFI::Type::Int64:
    case FFI::Type::Uint64:
    case FFI::Type::Int64Fast:
    case FFI::Type::Uint64Fast:
    case FFI::Type::BufferLength: { // ABI-identical to Uint64 (a byte length)
        static const uint64_t values[] = {
            0, 1, ~0ull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x0123456789abcdefull,
            0xffffffff80000000ull, 0x00000000ffffffffull, 9007199254740992ull, 0xdeadbeefcafebabeull,
        };
        return values[random.getUint32(std::size(values))];
    }
    case FFI::Type::Double: {
        static const uint64_t values[] = {
            0, 0x8000000000000000ull, std::bit_cast<uint64_t>(1.5), std::bit_cast<uint64_t>(-1.5),
            std::bit_cast<uint64_t>(std::numeric_limits<double>::infinity()), std::bit_cast<uint64_t>(-std::numeric_limits<double>::infinity()),
            std::bit_cast<uint64_t>(std::numeric_limits<double>::max()), 1 , std::bit_cast<uint64_t>(9007199254740993.0),
            0x7ff8000000000001ull, std::bit_cast<uint64_t>(3.141592653589793),
        };
        return values[random.getUint32(std::size(values))];
    }
    case FFI::Type::Float: {
        static const uint32_t values[] = {
            0, 0x80000000u, std::bit_cast<uint32_t>(1.5f), std::bit_cast<uint32_t>(-1.5f),
            std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()), std::bit_cast<uint32_t>(-std::numeric_limits<float>::infinity()),
            std::bit_cast<uint32_t>(std::numeric_limits<float>::max()), 1, 0x7fc00001u, std::bit_cast<uint32_t>(2.5f),
        };
        return values[random.getUint32(std::size(values))];
    }
    case FFI::Type::Bool:
        return random.getUint32(2);
    case FFI::Type::Pointer:
    case FFI::Type::CString: {
        static const char* const strings[] = { "", "a", "hello", "0123456789", "h\xc3\xa9!" };
        if (!random.getUint32(6))
            return 0;
        return reinterpret_cast<uintptr_t>(strings[random.getUint32(std::size(strings))]);
    }
    case FFI::Type::Function:
    case FFI::Type::Buffer:
    case FFI::Type::RESERVED_WasNapiEnv: {
        static const uint64_t values[] = {
            0, 0x00007fffdeadbee0ull, ~0ull, 4096,
            reinterpret_cast<uintptr_t>(&s_scratchMemory[0]), reinterpret_cast<uintptr_t>(&s_scratchMemory[13]),
        };
        return values[random.getUint32(std::size(values))];
    }
    case FFI::Type::JSValue:
        return random.getUint64();
    case FFI::Type::Void:
        return 0;
    }
    return 0;
}

static uint64_t randomCanonicalSlot(FFI::Type type, WeakRandom& random)
{
    uint64_t raw = random.returnTrueWithProbability(0.7) ? edgeBitsForType(type, random) : random.getUint64();
    return canonicalizeSlot(type, raw);
}

static void dumpSlots(std::span<const uint64_t> slots)
{
    dataLog("        slots:");
    for (uint64_t slot : slots)
        dataLog(" ", RawHex(slot));
    dataLogLn();
}

template<typename R, typename... Arguments>
static void differentialCase(ASCIILiteral name, R (*function)(Arguments...), Vector<FFI::Type>&& argumentTypes, FFI::Type returnType, unsigned iterations, WeakRandom& random)
{
    constexpr size_t nativeArgumentCount = sizeof...(Arguments);
    RefPtr<FFI::Signature> signature = FFI::Signature::tryCreate(argumentTypes.span(), returnType);
    FFI_CHECK(!!signature);
    if (!signature)
        return;
    FFI_CHECK_EQ(signature->argumentCount(), static_cast<unsigned>(nativeArgumentCount));
    if (signature->argumentCount() != nativeArgumentCount)
        return;

    MacroAssemblerCodeRef<JITThunkPtrTag> code = FFI::generateInvokeThunk(*signature);
    FFI_CHECK(!!code);
    if (!code)
        return;
    CodePtr<JITThunkPtrTag> cached = signature->invokeThunk();
    FFI_CHECK(!!cached);

    auto freshThunk = reinterpret_cast<FFI::InvokeThunkFunction>(code.code().taggedPtr());
    auto cachedThunk = cached ? reinterpret_cast<FFI::InvokeThunkFunction>(cached.taggedPtr()) : freshThunk;

    unsigned argumentCount = signature->argumentCount();
    constexpr uint64_t frontGuard = 0xFEEDFACEFEEDFACEull;
    constexpr uint64_t backGuard = 0xBADC0FFEEBADC0FFull;
    constexpr uint64_t returnPoison = 0xF00DF00DF00DF00Dull;

    unsigned failuresAtEntry = s_failureCount;
    for (unsigned iteration = 0; iteration < iterations && s_failureCount - failuresAtEntry < 6; ++iteration) {
        Vector<uint64_t> buffer(FillWith { }, argumentCount + 3, 0);
        buffer[0] = frontGuard;
        uint64_t* slots = buffer.mutableSpan().data() + 1;
        for (unsigned i = 0; i < argumentCount; ++i)
            slots[i] = randomCanonicalSlot(argumentTypes[i], random);
        slots[argumentCount] = returnPoison;
        buffer[argumentCount + 2] = backGuard;

        Vector<uint64_t> nativeSlots(FillWith { }, argumentCount + 1, 0);
        memcpy(nativeSlots.mutableSpan().data(), slots, sizeof(uint64_t) * argumentCount);

        auto thunk = (iteration & 1) ? cachedThunk : freshThunk;
        thunk(reinterpret_cast<void*>(function), slots);

        s_checkCount++;
        bool ok = buffer[0] == frontGuard && buffer[argumentCount + 2] == backGuard;
        if constexpr (std::is_void_v<R>) {
            callNativeFromSlots(function, nativeSlots.span().data());
            ok &= slots[argumentCount] == returnPoison; // Void return slot untouched
        } else {
            R nativeResult = callNativeFromSlots(function, nativeSlots.span().data());
            uint64_t expected = canonicalizeSlot(returnType, nativeReturnRawBits(nativeResult));
            uint64_t actual = slots[argumentCount];
            bool returnsEqual = actual == expected;
            constexpr bool operandOrderCanFlipNaNPayload = sizeof...(Arguments) >= 2;
            if constexpr (operandOrderCanFlipNaNPayload) {
                if (!returnsEqual && returnType == FFI::Type::Double)
                    returnsEqual = std::isnan(std::bit_cast<double>(actual)) && std::isnan(std::bit_cast<double>(expected));
                else if (!returnsEqual && returnType == FFI::Type::Float)
                    returnsEqual = std::isnan(std::bit_cast<float>(static_cast<uint32_t>(actual))) && std::isnan(std::bit_cast<float>(static_cast<uint32_t>(expected)));
            }
            ok &= returnsEqual;
            if (!ok) {
                s_failureCount++;
                dataLogLn("    FAIL: invoke-thunk differential for ", name.characters(), " (", signature->toString(), ") iteration ", iteration,
                    ": thunk return slot ", RawHex(actual), " != native ", RawHex(expected));
                dumpSlots(nativeSlots.span().first(argumentCount));
                continue;
            }
        }
        if (!ok) {
            s_failureCount++;
            dataLogLn("    FAIL: invoke-thunk differential for ", name.characters(), " (", signature->toString(), ") iteration ", iteration,
                ": slot buffer guard corrupted or void return slot written (front ", RawHex(buffer[0]), ", back ", RawHex(buffer[argumentCount + 2]), ", ret ", RawHex(slots[argumentCount]), ")");
            dumpSlots(nativeSlots.span().first(argumentCount));
        }
    }
}

static int32_t staticCbI32(int32_t x) { return static_cast<int32_t>(static_cast<uint32_t>(x) * 3u + 1u); }
static double staticCbF64x8(double a, double b, double c, double d, double e, double f, double g, double h)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}
static double staticCbMix(int32_t a, double b, int64_t c, float d, void* e)
{
    return static_cast<double>(a) + 2 * b + 3 * static_cast<double>(c) + 4 * static_cast<double>(d) + 5 * static_cast<double>(reinterpret_cast<uintptr_t>(e));
}
static unsigned s_staticCbVoidCount;
static void staticCbVoid() { ++s_staticCbVoidCount; }
static int64_t staticCbI32x9(int32_t a0, int32_t a1, int32_t a2, int32_t a3, int32_t a4, int32_t a5, int32_t a6, int32_t a7, int32_t a8)
{
    return int64_t(a0) + 2 * int64_t(a1) + 3 * int64_t(a2) + 4 * int64_t(a3) + 5 * int64_t(a4) + 6 * int64_t(a5) + 7 * int64_t(a6) + 8 * int64_t(a7) + 9 * int64_t(a8);
}
static double staticCbF64x9(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, double a8)
{
    return a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7 + 9 * a8;
}
static int64_t staticCbU8x10(uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t a4, uint8_t a5, uint8_t a6, uint8_t a7, uint8_t a8, uint8_t a9)
{
    return int64_t(a0) + 2 * int64_t(a1) + 3 * int64_t(a2) + 4 * int64_t(a3) + 5 * int64_t(a4) + 6 * int64_t(a5) + 7 * int64_t(a6) + 8 * int64_t(a7) + 9 * int64_t(a8) + 10 * int64_t(a9);
}
static int8_t staticCbRetI8() { return -3; }
static uint8_t staticCbRetU8() { return 255; }
static int64_t staticCbRetI64() { return std::numeric_limits<int64_t>::min(); }
static uint64_t staticCbRetU64() { return std::numeric_limits<uint64_t>::max(); }
static bool staticCbRetBool() { return true; }
static float staticCbRetF32() { return -1.25f; }
static double staticCbRetF64() { return 6.02214076e23; }
static void* staticCbRetPtr() { return &s_scratchMemory[3]; }

static void testInvokeThunkDifferential()
{
    using T = FFI::Type;
    WeakRandom random(0xF00DFEED);

    differentialCase("ffi_echo_char"_s, ffi_echo_char, { T::Char }, T::Char, 40, random);
    differentialCase("ffi_echo_char(as i8)"_s, ffi_echo_char, { T::Int8 }, T::Int8, 20, random);
    differentialCase("ffi_echo_i8"_s, ffi_echo_i8, { T::Int8 }, T::Int8, 40, random);
    differentialCase("ffi_echo_u8"_s, ffi_echo_u8, { T::Uint8 }, T::Uint8, 40, random);
    differentialCase("ffi_echo_i16"_s, ffi_echo_i16, { T::Int16 }, T::Int16, 40, random);
    differentialCase("ffi_echo_u16"_s, ffi_echo_u16, { T::Uint16 }, T::Uint16, 40, random);
    differentialCase("ffi_echo_i32"_s, ffi_echo_i32, { T::Int32 }, T::Int32, 40, random);
    differentialCase("ffi_echo_u32"_s, ffi_echo_u32, { T::Uint32 }, T::Uint32, 40, random);
    differentialCase("ffi_echo_i64"_s, ffi_echo_i64, { T::Int64 }, T::Int64, 40, random);
    differentialCase("ffi_echo_i64(as i64_fast)"_s, ffi_echo_i64, { T::Int64Fast }, T::Int64Fast, 20, random);
    differentialCase("ffi_echo_u64"_s, ffi_echo_u64, { T::Uint64 }, T::Uint64, 40, random);
    differentialCase("ffi_echo_u64(as u64_fast)"_s, ffi_echo_u64, { T::Uint64Fast }, T::Uint64Fast, 20, random);
    differentialCase("ffi_echo_u64(as buffer_length)"_s, ffi_echo_u64, { T::BufferLength }, T::Uint64, 20, random);
    differentialCase("ffi_echo_f32"_s, ffi_echo_f32, { T::Float }, T::Float, 40, random);
    differentialCase("ffi_echo_f64"_s, ffi_echo_f64, { T::Double }, T::Double, 40, random);
    differentialCase("ffi_echo_bool"_s, ffi_echo_bool, { T::Bool }, T::Bool, 20, random);
    differentialCase("ffi_echo_ptr"_s, ffi_echo_ptr, { T::Pointer }, T::Pointer, 40, random);
    differentialCase("ffi_echo_ptr(as buffer)"_s, ffi_echo_ptr, { T::Buffer }, T::Pointer, 20, random);
    differentialCase("ffi_echo_ptr(as function)"_s, ffi_echo_ptr, { T::Function }, T::Pointer, 20, random);
    differentialCase("ffi_echo_cstring"_s, ffi_echo_cstring, { T::CString }, T::CString, 30, random);
    differentialCase("ffi_echo_jsvalue"_s, ffi_echo_jsvalue, { T::JSValue }, T::JSValue, 40, random);
    differentialCase("ffi_widen_char"_s, ffi_widen_char, { T::Char }, T::Int64, 30, random);
    differentialCase("ffi_widen_i8"_s, ffi_widen_i8, { T::Int8 }, T::Int64, 30, random);
    differentialCase("ffi_widen_u8"_s, ffi_widen_u8, { T::Uint8 }, T::Int64, 30, random);
    differentialCase("ffi_widen_i16"_s, ffi_widen_i16, { T::Int16 }, T::Int64, 30, random);
    differentialCase("ffi_widen_u16"_s, ffi_widen_u16, { T::Uint16 }, T::Int64, 30, random);
    differentialCase("ffi_ret_null_ptr"_s, ffi_ret_null_ptr, { }, T::Pointer, 4, random);
    differentialCase("ffi_ret_two_as_bool"_s, ffi_ret_two_as_bool, { }, T::Bool, 4, random); // declared bool, native returns 2 -> slot must be 1
    differentialCase("ffi_ret_two_as_bool(as i8)"_s, ffi_ret_two_as_bool, { }, T::Int8, 4, random);
    differentialCase("ffi_ret_neg_one_i8"_s, ffi_ret_neg_one_i8, { }, T::Int8, 4, random);
    differentialCase("ffi_ret_neg_one_i16"_s, ffi_ret_neg_one_i16, { }, T::Int16, 4, random);
    differentialCase("ffi_ret_neg_one_i32"_s, ffi_ret_neg_one_i32, { }, T::Int32, 4, random);
    differentialCase("ffi_ret_neg_one_i64"_s, ffi_ret_neg_one_i64, { }, T::Int64, 4, random);
    differentialCase("ffi_ret_neg_one_u8"_s, ffi_ret_neg_one_u8, { }, T::Uint8, 4, random);
    differentialCase("ffi_ret_neg_one_u16"_s, ffi_ret_neg_one_u16, { }, T::Uint16, 4, random);
    differentialCase("ffi_ret_neg_one_u32"_s, ffi_ret_neg_one_u32, { }, T::Uint32, 4, random);
    differentialCase("ffi_ret_neg_one_u64"_s, ffi_ret_neg_one_u64, { }, T::Uint64, 4, random);
    differentialCase("ffi_ret_nan_f32"_s, ffi_ret_nan_f32, { }, T::Float, 4, random);
    differentialCase("ffi_ret_impure_nan_f64"_s, ffi_ret_impure_nan_f64, { }, T::Double, 4, random);
    differentialCase("ffi_ret_neg_zero_f64"_s, ffi_ret_neg_zero_f64, { }, T::Double, 4, random);
    differentialCase("ffi_ret_denormal_f32"_s, ffi_ret_denormal_f32, { }, T::Float, 4, random);
    differentialCase("ffi_ret_inf_f64"_s, ffi_ret_inf_f64, { }, T::Double, 4, random);
    differentialCase("ffi_add_i32"_s, ffi_add_i32, { T::Int32, T::Int32 }, T::Int32, 60, random);
    differentialCase("ffi_add_f64"_s, ffi_add_f64, { T::Double, T::Double }, T::Double, 60, random);
    differentialCase("ffi_add_i64"_s, ffi_add_i64, { T::Int64, T::Int64 }, T::Int64, 60, random);
    differentialCase("ffi_add_u64"_s, ffi_add_u64, { T::Uint64, T::Uint64 }, T::Uint64, 60, random);
    differentialCase("ffi_add_f32"_s, ffi_add_f32, { T::Float, T::Float }, T::Float, 60, random);
    differentialCase("ffi_sum_i32_0"_s, ffi_sum_i32_0, { }, T::Int64, 4, random);
    differentialCase("ffi_sum_i32_1"_s, ffi_sum_i32_1, { T::Int32 }, T::Int64, 20, random);
    differentialCase("ffi_sum_i32_2"_s, ffi_sum_i32_2, Vector<T>(FillWith { }, 2, T::Int32), T::Int64, 20, random);
    differentialCase("ffi_sum_i32_4"_s, ffi_sum_i32_4, Vector<T>(FillWith { }, 4, T::Int32), T::Int64, 20, random);
    differentialCase("ffi_sum_i32_6"_s, ffi_sum_i32_6, Vector<T>(FillWith { }, 6, T::Int32), T::Int64, 30, random);
    differentialCase("ffi_sum_i32_7"_s, ffi_sum_i32_7, Vector<T>(FillWith { }, 7, T::Int32), T::Int64, 30, random);
    differentialCase("ffi_sum_i32_8"_s, ffi_sum_i32_8, Vector<T>(FillWith { }, 8, T::Int32), T::Int64, 30, random);
    differentialCase("ffi_sum_i32_9"_s, ffi_sum_i32_9, Vector<T>(FillWith { }, 9, T::Int32), T::Int64, 30, random);
    differentialCase("ffi_sum_i32_12"_s, ffi_sum_i32_12, Vector<T>(FillWith { }, 12, T::Int32), T::Int64, 30, random);
    differentialCase("ffi_sum_i32_16"_s, ffi_sum_i32_16, Vector<T>(FillWith { }, 16, T::Int32), T::Int64, 30, random);
    differentialCase("ffi_sum_f64_1"_s, ffi_sum_f64_1, { T::Double }, T::Double, 20, random);
    differentialCase("ffi_sum_f64_2"_s, ffi_sum_f64_2, Vector<T>(FillWith { }, 2, T::Double), T::Double, 20, random);
    differentialCase("ffi_sum_f64_7"_s, ffi_sum_f64_7, Vector<T>(FillWith { }, 7, T::Double), T::Double, 30, random);
    differentialCase("ffi_sum_f64_8"_s, ffi_sum_f64_8, Vector<T>(FillWith { }, 8, T::Double), T::Double, 30, random);
    differentialCase("ffi_sum_f64_9"_s, ffi_sum_f64_9, Vector<T>(FillWith { }, 9, T::Double), T::Double, 30, random);
    differentialCase("ffi_sum_f64_12"_s, ffi_sum_f64_12, Vector<T>(FillWith { }, 12, T::Double), T::Double, 30, random);
    differentialCase("ffi_sum_u8_10"_s, ffi_sum_u8_10, Vector<T>(FillWith { }, 10, T::Uint8), T::Int64, 30, random);
    differentialCase("ffi_sum_u8_12"_s, ffi_sum_u8_12, Vector<T>(FillWith { }, 12, T::Uint8), T::Int64, 30, random);
    differentialCase("ffi_sum_i16_10"_s, ffi_sum_i16_10, Vector<T>(FillWith { }, 10, T::Int16), T::Int64, 30, random);
    differentialCase("ffi_sum_i16_12"_s, ffi_sum_i16_12, Vector<T>(FillWith { }, 12, T::Int16), T::Int64, 30, random);
    differentialCase("ffi_mix_1"_s, ffi_mix_1, { T::Int32, T::Double, T::Int64, T::Float, T::Pointer, T::Uint8, T::Double, T::Int16, T::Double, T::Int32 }, T::Double, 60, random);
    differentialCase("ffi_mix_2"_s, ffi_mix_2, { T::Float, T::Int32, T::Float, T::Int32, T::Float, T::Int32, T::Float, T::Int32, T::Float, T::Int32 }, T::Double, 60, random);
    differentialCase("ffi_mix_3"_s, ffi_mix_3, { T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Int32 }, T::Double, 60, random);
    differentialCase("ffi_mix_4"_s, ffi_mix_4, { T::Int64, T::Int64, T::Int64, T::Int64, T::Int64, T::Int64, T::Double, T::Int64, T::Double }, T::Double, 60, random);
    differentialCase("ffi_mix_5"_s, ffi_mix_5, { T::Uint8, T::Int8, T::Uint16, T::Int16, T::Uint32, T::Int32, T::Uint64, T::Int64 }, T::Double, 60, random);
    differentialCase("ffi_mix_6"_s, ffi_mix_6, { T::Bool, T::Bool, T::Int32, T::Bool, T::Double, T::Bool, T::Float, T::Bool, T::Bool, T::Bool, T::Bool, T::Bool, T::Bool }, T::Double, 60, random);
    differentialCase("ffi_mix_7"_s, ffi_mix_7, { T::Pointer, T::Char, T::Pointer, T::Char, T::Pointer, T::Char, T::Pointer, T::Char, T::Pointer, T::Char }, T::Double, 60, random);
    differentialCase("ffi_mix_8"_s, ffi_mix_8, { T::Float, T::Double, T::Float, T::Double, T::Float, T::Double, T::Float, T::Double, T::Float, T::Double, T::Float, T::Double }, T::Double, 60, random);
    differentialCase("ffi_ptr_identity"_s, ffi_ptr_identity, { T::Pointer }, T::Pointer, 30, random);
    differentialCase("ffi_high_ptr"_s, ffi_high_ptr, { }, T::Pointer, 4, random);
    differentialCase("ffi_align_probe_0"_s, ffi_align_probe_0, { }, T::Double, 8, random);
    differentialCase("ffi_align_probe_9"_s, ffi_align_probe_9, Vector<T>(FillWith { }, 9, T::Int32), T::Double, 12, random);

    {
        auto runWithCallback = [&](ASCIILiteral name, auto* fixture, Vector<FFI::Type>&& argumentTypes, FFI::Type returnType, uint64_t callbackSlot, unsigned iterations) {
            RefPtr<FFI::Signature> signature = FFI::Signature::tryCreate(argumentTypes.span(), returnType);
            FFI_CHECK(!!signature);
            if (!signature)
                return;
            MacroAssemblerCodeRef<JITThunkPtrTag> code = FFI::generateInvokeThunk(*signature);
            FFI_CHECK(!!code);
            if (!code)
                return;
            auto thunk = reinterpret_cast<FFI::InvokeThunkFunction>(code.code().taggedPtr());
            unsigned argumentCount = signature->argumentCount();
            for (unsigned iteration = 0; iteration < iterations; ++iteration) {
                Vector<uint64_t> buffer(FillWith { }, argumentCount + 3, 0);
                buffer[0] = 0xFEEDFACEFEEDFACEull;
                uint64_t* slots = buffer.mutableSpan().data() + 1;
                slots[0] = callbackSlot;
                for (unsigned i = 1; i < argumentCount; ++i)
                    slots[i] = randomCanonicalSlot(argumentTypes[i], random);
                slots[argumentCount] = 0xF00DF00DF00DF00Dull;
                buffer[argumentCount + 2] = 0xBADC0FFEEBADC0FFull;
                Vector<uint64_t> nativeSlots(FillWith { }, argumentCount + 1, 0);
                memcpy(nativeSlots.mutableSpan().data(), slots, sizeof(uint64_t) * argumentCount);

                thunk(reinterpret_cast<void*>(fixture), slots);
                auto nativeResult = callNativeFromSlots(fixture, nativeSlots.span().data());
                uint64_t expected = canonicalizeSlot(returnType, nativeReturnRawBits(nativeResult));
                s_checkCount++;
                bool ok = slots[argumentCount] == expected
                    && buffer[0] == 0xFEEDFACEFEEDFACEull
                    && buffer[argumentCount + 2] == 0xBADC0FFEEBADC0FFull;
                if (!ok) {
                    s_failureCount++;
                    dataLogLn("    FAIL: callback differential for ", name.characters(), " iteration ", iteration, ": thunk ", RawHex(slots[argumentCount]), " native ", RawHex(expected));
                    dumpSlots(nativeSlots.span().first(argumentCount));
                }
            }
        };
        runWithCallback("ffi_call_cb_i32"_s, ffi_call_cb_i32, { T::Function, T::Int32 }, T::Int32,
            reinterpret_cast<uintptr_t>(&staticCbI32), 20);
        runWithCallback("ffi_call_cb_f64_x8"_s, ffi_call_cb_f64_x8, { T::Function, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double }, T::Double,
            reinterpret_cast<uintptr_t>(&staticCbF64x8), 20);
        runWithCallback("ffi_call_cb_mix"_s, ffi_call_cb_mix, { T::Function, T::Int32, T::Double, T::Int64, T::Float, T::Pointer }, T::Double,
            reinterpret_cast<uintptr_t>(&staticCbMix), 20);
        runWithCallback("ffi_call_cb_i32_x9"_s, ffi_call_cb_i32_x9, { T::Function, T::Int32, T::Int32, T::Int32, T::Int32, T::Int32, T::Int32, T::Int32, T::Int32, T::Int32 }, T::Int64,
            reinterpret_cast<uintptr_t>(&staticCbI32x9), 20);
        runWithCallback("ffi_call_cb_f64_x9"_s, ffi_call_cb_f64_x9, { T::Function, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double, T::Double }, T::Double,
            reinterpret_cast<uintptr_t>(&staticCbF64x9), 20);
        runWithCallback("ffi_call_cb_u8_x10"_s, ffi_call_cb_u8_x10, { T::Function, T::Uint8, T::Uint8, T::Uint8, T::Uint8, T::Uint8, T::Uint8, T::Uint8, T::Uint8, T::Uint8, T::Uint8 }, T::Int64,
            reinterpret_cast<uintptr_t>(&staticCbU8x10), 20);
        runWithCallback("ffi_call_cb_ret_i8"_s, ffi_call_cb_ret_i8, { T::Function }, T::Int64, reinterpret_cast<uintptr_t>(&staticCbRetI8), 3);
        runWithCallback("ffi_call_cb_ret_u8"_s, ffi_call_cb_ret_u8, { T::Function }, T::Int64, reinterpret_cast<uintptr_t>(&staticCbRetU8), 3);
        runWithCallback("ffi_call_cb_ret_i64"_s, ffi_call_cb_ret_i64, { T::Function }, T::Int64, reinterpret_cast<uintptr_t>(&staticCbRetI64), 3);
        runWithCallback("ffi_call_cb_ret_u64"_s, ffi_call_cb_ret_u64, { T::Function }, T::Uint64, reinterpret_cast<uintptr_t>(&staticCbRetU64), 3);
        runWithCallback("ffi_call_cb_ret_bool"_s, ffi_call_cb_ret_bool, { T::Function }, T::Int32, reinterpret_cast<uintptr_t>(&staticCbRetBool), 3);
        runWithCallback("ffi_call_cb_ret_f32"_s, ffi_call_cb_ret_f32, { T::Function }, T::Float, reinterpret_cast<uintptr_t>(&staticCbRetF32), 3);
        runWithCallback("ffi_call_cb_ret_f64"_s, ffi_call_cb_ret_f64, { T::Function }, T::Double, reinterpret_cast<uintptr_t>(&staticCbRetF64), 3);
        runWithCallback("ffi_call_cb_ret_ptr"_s, ffi_call_cb_ret_ptr, { T::Function }, T::Pointer, reinterpret_cast<uintptr_t>(&staticCbRetPtr), 3);
    }

    {
        Vector<T> argumentTypes { T::Function };
        RefPtr<FFI::Signature> signature = FFI::Signature::tryCreate(argumentTypes.span(), T::Void);
        FFI_CHECK(!!signature);
        if (signature) {
            MacroAssemblerCodeRef<JITThunkPtrTag> code = FFI::generateInvokeThunk(*signature);
            FFI_CHECK(!!code);
            if (code) {
                auto thunk = reinterpret_cast<FFI::InvokeThunkFunction>(code.code().taggedPtr());
                s_staticCbVoidCount = 0;
                uint64_t slots[2] = { static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&staticCbVoid)), 0 };
                thunk(reinterpret_cast<void*>(ffi_call_cb_void), slots);
                thunk(reinterpret_cast<void*>(ffi_call_cb_void), slots);
                ffi_call_cb_void(staticCbVoid);
                FFI_CHECK_EQ(s_staticCbVoidCount, 3u);
            }
        }
    }

    {
        Vector<T> writeTypes { T::Pointer, T::Uint32 };
        RefPtr<FFI::Signature> writeSignature = FFI::Signature::tryCreate(writeTypes.span(), T::Void);
        Vector<T> readTypes { T::Pointer };
        RefPtr<FFI::Signature> readSignature = FFI::Signature::tryCreate(readTypes.span(), T::Uint32);
        Vector<T> strlenTypes { T::CString };
        RefPtr<FFI::Signature> strlenSignature = FFI::Signature::tryCreate(strlenTypes.span(), T::Uint64);
        FFI_CHECK(!!writeSignature);
        FFI_CHECK(!!readSignature);
        FFI_CHECK(!!strlenSignature);
        if (writeSignature && readSignature && strlenSignature) {
            MacroAssemblerCodeRef<JITThunkPtrTag> writeCode = FFI::generateInvokeThunk(*writeSignature);
            MacroAssemblerCodeRef<JITThunkPtrTag> readCode = FFI::generateInvokeThunk(*readSignature);
            MacroAssemblerCodeRef<JITThunkPtrTag> strlenCode = FFI::generateInvokeThunk(*strlenSignature);
            FFI_CHECK(!!writeCode);
            FFI_CHECK(!!readCode);
            FFI_CHECK(!!strlenCode);
            if (writeCode && readCode && strlenCode) {
                auto writeThunk = reinterpret_cast<FFI::InvokeThunkFunction>(writeCode.code().taggedPtr());
                auto readThunk = reinterpret_cast<FFI::InvokeThunkFunction>(readCode.code().taggedPtr());
                auto strlenThunk = reinterpret_cast<FFI::InvokeThunkFunction>(strlenCode.code().taggedPtr());
                static uint32_t words[8];
                static const uint32_t patterns[] = { 0, 0xdeadbeefu, 0xffffffffu, 1, 0x80000000u, 0x12345678u };
                for (unsigned i = 0; i < std::size(words); ++i) {
                    for (uint32_t pattern : patterns) {
                        words[i] = ~pattern;
                        uint64_t writeSlots[3] = { static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&words[i])), pattern, 0xdead };
                        writeThunk(reinterpret_cast<void*>(ffi_ptr_write_u32), writeSlots);
                        FFI_CHECK_EQ(words[i], pattern);
                        uint64_t readSlots[2] = { static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&words[i])), 0xF00DF00DF00DF00Dull };
                        readThunk(reinterpret_cast<void*>(ffi_ptr_read_u32), readSlots);
                        FFI_CHECK_EQ_HEX(readSlots[1], static_cast<uint64_t>(pattern)); // u32 return zero-extended
                    }
                }
                static const char* strings[] = { "", "a", "hello, world", "0123456789012345678901234567890123456789012345678901234567890123456789" };
                for (const char* string : strings) {
                    uint64_t strlenSlots[2] = { static_cast<uint64_t>(reinterpret_cast<uintptr_t>(string)), 0xF00DF00DF00DF00Dull };
                    strlenThunk(reinterpret_cast<void*>(ffi_strlen), strlenSlots);
                    FFI_CHECK_EQ(strlenSlots[1], static_cast<uint64_t>(strlen(string)));
                }
            }
        }
    }
}

static struct {
    FFI::InvokeThunkFunction thunk { nullptr };
    void* target { nullptr };
    uint64_t slots[4];
} s_canaryInvokeState;

static void canaryInvokeBody()
{
    s_canaryInvokeState.thunk(s_canaryInvokeState.target, &s_canaryInvokeState.slots[0]);
}

static unsigned s_hostCanaryHits;

static JSC_DECLARE_HOST_FUNCTION(functionCanaryTarget);
JSC_DEFINE_HOST_FUNCTION(functionCanaryTarget, (JSGlobalObject*, CallFrame*))
{
    ++s_hostCanaryHits;
    return JSValue::encode(jsUndefined());
}

static JSC_DECLARE_HOST_FUNCTION(functionCanaryTargetAllocating);
JSC_DEFINE_HOST_FUNCTION(functionCanaryTargetAllocating, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    ++s_hostCanaryHits;
    JSObject* garbage = nullptr;
    for (unsigned i = 0; i < 200; ++i) {
        JSObject* object = constructEmptyObject(globalObject);
        object->putDirect(vm, vm.propertyNames->length, jsNumber(i));
        garbage = object;
    }
    return JSValue::encode(garbage ? JSValue(garbage) : jsUndefined());
}

static void testCanaries()
{
    VM& vm = *s_vm;
    JSGlobalObject* globalObject = s_globalObject;

    {
        Vector<FFI::Type> argumentTypes { FFI::Type::Int64, FFI::Type::Int64 };
        RefPtr<FFI::Signature> signature = FFI::Signature::tryCreate(argumentTypes.span(), FFI::Type::Int64);
        FFI_CHECK(!!signature);
        if (signature) {
            CodePtr<JITThunkPtrTag> thunk = signature->invokeThunk();
            FFI_CHECK(!!thunk);
            if (thunk) {
                s_canaryInvokeState.thunk = reinterpret_cast<FFI::InvokeThunkFunction>(thunk.taggedPtr());
                s_canaryInvokeState.target = reinterpret_cast<void*>(ffi_add_i64);
                s_canaryInvokeState.slots[0] = 40;
                s_canaryInvokeState.slots[1] = 2;
                s_canaryInvokeState.slots[2] = 0;
                int32_t mask = ffi_canary_call(canaryInvokeBody);
                FFI_CHECK_EQ(mask, 0);
                FFI_CHECK_EQ_HEX(s_canaryInvokeState.slots[2], 42);
            }
        }
    }

    {
        JSFunction* trivialTarget = JSFunction::create(vm, globalObject, 0, "canaryTarget"_s, functionCanaryTarget, ImplementationVisibility::Public);
        JSFunction* allocatingTarget = JSFunction::create(vm, globalObject, 0, "canaryTargetAllocating"_s, functionCanaryTargetAllocating, ImplementationVisibility::Public);
        RefPtr<FFI::Signature> voidSignature = FFI::Signature::tryCreate({ }, FFI::Type::Void);
        FFI_CHECK(!!voidSignature);
        if (voidSignature) {
            JSFFICallback* trivialCallback = JSFFICallback::create(vm, globalObject, globalObject->ffiCallbackStructure(), trivialTarget, voidSignature.releaseNonNull());
            FFI_CHECK(!!trivialCallback);
            if (trivialCallback) {
                gcProtect(trivialCallback);
                FFI_CHECK(trivialCallback->nativeEntrypoint());
                if (trivialCallback->nativeEntrypoint()) {
                    s_hostCanaryHits = 0;
                    auto entry = reinterpret_cast<void (*)(void)>(trivialCallback->nativeEntrypoint());
                    int32_t mask = ffi_canary_call(entry);
                    FFI_CHECK_EQ(mask, 0);
                    FFI_CHECK_EQ(s_hostCanaryHits, 1u);
                    entry();
                    FFI_CHECK_EQ(s_hostCanaryHits, 2u);
                    FFI_CHECK_EQ(ffi_align_probe_0(), 1.0);
                }
            }
        }
        RefPtr<FFI::Signature> objectSignature = FFI::Signature::tryCreate({ }, FFI::Type::JSValue);
        FFI_CHECK(!!objectSignature);
        if (objectSignature) {
            JSFFICallback* allocatingCallback = JSFFICallback::create(vm, globalObject, globalObject->ffiCallbackStructure(), allocatingTarget, objectSignature.releaseNonNull());
            FFI_CHECK(!!allocatingCallback);
            if (allocatingCallback) {
                gcProtect(allocatingCallback);
                s_hostCanaryHits = 0;
                int32_t mask = ffi_canary_call(reinterpret_cast<void (*)(void)>(allocatingCallback->nativeEntrypoint()));
                FFI_CHECK_EQ(mask, 0);
                FFI_CHECK_EQ(s_hostCanaryHits, 1u);
                gcUnprotect(allocatingCallback);
            }
        }
    }

    {
        Vector<FFI::Type> nine(FillWith { }, 9, FFI::Type::Int32);
        RefPtr<FFI::Signature> nineSignature = FFI::Signature::tryCreate(nine.span(), FFI::Type::Double);
        RefPtr<FFI::Signature> zeroSignature = FFI::Signature::tryCreate({ }, FFI::Type::Double);
        FFI_CHECK(!!nineSignature);
        FFI_CHECK(!!zeroSignature);
        if (nineSignature && zeroSignature) {
            CodePtr<JITThunkPtrTag> nineThunk = nineSignature->invokeThunk();
            CodePtr<JITThunkPtrTag> zeroThunk = zeroSignature->invokeThunk();
            FFI_CHECK(!!nineThunk);
            FFI_CHECK(!!zeroThunk);
            if (nineThunk && zeroThunk) {
                uint64_t nineSlots[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };
                reinterpret_cast<FFI::InvokeThunkFunction>(nineThunk.taggedPtr())(reinterpret_cast<void*>(ffi_align_probe_9), nineSlots);
                FFI_CHECK_EQ(std::bit_cast<double>(nineSlots[9]), 1.0);
                uint64_t zeroSlots[1] = { 0 };
                reinterpret_cast<FFI::InvokeThunkFunction>(zeroThunk.taggedPtr())(reinterpret_cast<void*>(ffi_align_probe_0), zeroSlots);
                FFI_CHECK_EQ(std::bit_cast<double>(zeroSlots[0]), 1.0);
            }
        }
    }
}

static JSC_DECLARE_HOST_FUNCTION(functionTimesThreePlusOne);
JSC_DEFINE_HOST_FUNCTION(functionTimesThreePlusOne, (JSGlobalObject*, CallFrame* callFrame))
{
    JSValue argument = callFrame->argument(0);
    double x = argument.isNumber() ? argument.asNumber() : 0.0;
    return JSValue::encode(jsNumber(x * 3.0 + 1.0));
}

static JSC_DECLARE_HOST_FUNCTION(functionSumWeightedArguments);
JSC_DEFINE_HOST_FUNCTION(functionSumWeightedArguments, (JSGlobalObject*, CallFrame* callFrame))
{
    double sum = 0;
    for (unsigned i = 0; i < callFrame->argumentCount(); ++i) {
        JSValue argument = callFrame->uncheckedArgument(i);
        double value = 0;
        if (argument.isNumber())
            value = argument.asNumber();
        else if (argument.isBoolean())
            value = argument.asBoolean() ? 1 : 0;
        sum += (i + 1) * value;
    }
    return JSValue::encode(jsNumber(sum));
}

static JSC_DECLARE_HOST_FUNCTION(functionReturnTwo);
JSC_DEFINE_HOST_FUNCTION(functionReturnTwo, (JSGlobalObject*, CallFrame*))
{
    return JSValue::encode(jsNumber(2));
}

static JSC_DECLARE_HOST_FUNCTION(functionReturnNaN);
JSC_DEFINE_HOST_FUNCTION(functionReturnNaN, (JSGlobalObject*, CallFrame*))
{
    return JSValue::encode(jsNaN());
}

static JSC_DECLARE_HOST_FUNCTION(functionReturnMinusOne);
JSC_DEFINE_HOST_FUNCTION(functionReturnMinusOne, (JSGlobalObject*, CallFrame*))
{
    return JSValue::encode(jsNumber(-1));
}

static JSValue callFunction(JSValue function, const MarkedArgumentBuffer& arguments)
{
    VM& vm = *s_vm;
    JSGlobalObject* globalObject = s_globalObject;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    CallData callData = getCallData(function);
    if (callData.type == CallData::Type::None) {
        s_checkCount++;
        s_failureCount++;
        dataLogLn("    FAIL: value is not callable");
        return jsUndefined();
    }
    JSValue result = call(globalObject, function, callData, jsUndefined(), arguments);
    Exception* exception = scope.exception();
    if (exception) {
        s_checkCount++;
        s_failureCount++;
        JSValue thrown = exception->value();
        scope.clearException();
        ErrorInstance* error = dynamicDowncast<ErrorInstance>(thrown);
        dataLogLn("    FAIL: unexpected exception from JSFFIFunction call (", error ? "an Error instance" : "a non-Error value", ")");
        return jsUndefined();
    }
    return result;
}

static void testJSFFIFunctionEndToEnd()
{
    VM& vm = *s_vm;
    JSGlobalObject* globalObject = s_globalObject;
    using T = FFI::Type;

    auto makeFunction = [&](Vector<T>&& argumentTypes, T returnType, void* target, ASCIILiteral name) -> JSFFIFunction* {
        RefPtr<FFI::Signature> signature = FFI::Signature::tryCreate(argumentTypes.span(), returnType);
        FFI_CHECK(!!signature);
        if (!signature)
            return nullptr;
        JSFFIFunction* function = JSFFIFunction::create(vm, globalObject, globalObject->ffiFunctionStructure(), signature.releaseNonNull(), target, String(name));
        FFI_CHECK(!!function);
        if (function)
            gcProtect(function);
        return function;
    };

    auto icStubsBefore = FFI::g_ffiCompileCounts.icStub.load();

    if (JSFFIFunction* addI32 = makeFunction({ T::Int32, T::Int32 }, T::Int32, reinterpret_cast<void*>(ffi_add_i32), "ffi_add_i32"_s)) {
        FFI_CHECK_EQ(addI32->signature().argumentCount(), 2u);
        FFI_CHECK(addI32->target() == reinterpret_cast<void*>(ffi_add_i32));
        MarkedArgumentBuffer arguments;
        arguments.append(jsNumber(40));
        arguments.append(jsNumber(2));
        FFI_CHECK(callFunction(addI32, arguments) == jsNumber(42));
        MarkedArgumentBuffer overflow;
        overflow.append(jsNumber(2147483647));
        overflow.append(jsNumber(1));
        FFI_CHECK(callFunction(addI32, overflow) == jsNumber(-2147483647 - 1)); // wraps
        MarkedArgumentBuffer missing;
        missing.append(jsNumber(7));
        FFI_CHECK(callFunction(addI32, missing) == jsNumber(7)); // missing argument -> 0
        MarkedArgumentBuffer extra;
        extra.append(jsNumber(1));
        extra.append(jsNumber(2));
        extra.append(jsNumber(4));
        FFI_CHECK(callFunction(addI32, extra) == jsNumber(3)); // extra arguments ignored
        MarkedArgumentBuffer doubles;
        doubles.append(jsDoubleNumber(1.9));
        doubles.append(jsDoubleNumber(-2.9));
        FFI_CHECK(callFunction(addI32, doubles) == jsNumber(-1)); // toInt32 truncation
    }

    if (JSFFIFunction* echoBool = makeFunction({ T::Bool }, T::Bool, reinterpret_cast<void*>(ffi_echo_bool), "ffi_echo_bool"_s)) {
        static const struct { double input; bool expected; } cases[] = {
            { 0, false }, { 1, true }, { 2, true }, { -1, true }, { 0.5, true }, { -0.0, false },
        };
        for (auto& c : cases) {
            MarkedArgumentBuffer arguments;
            arguments.append(jsNumber(c.input));
            FFI_CHECK(callFunction(echoBool, arguments) == jsBoolean(c.expected));
        }
        MarkedArgumentBuffer booleans;
        booleans.append(jsBoolean(true));
        FFI_CHECK(callFunction(echoBool, booleans) == jsBoolean(true));
        MarkedArgumentBuffer nanArguments;
        nanArguments.append(jsNaN());
        FFI_CHECK(callFunction(echoBool, nanArguments) == jsBoolean(false));
    }
    if (JSFFIFunction* twoAsBool = makeFunction({ }, T::Bool, reinterpret_cast<void*>(ffi_ret_two_as_bool), "ffi_ret_two_as_bool"_s)) {
        MarkedArgumentBuffer arguments;
        FFI_CHECK(callFunction(twoAsBool, arguments) == jsBoolean(true));
    }

    if (JSFFIFunction* nullPtr = makeFunction({ }, T::Pointer, reinterpret_cast<void*>(ffi_ret_null_ptr), "ffi_ret_null_ptr"_s)) {
        MarkedArgumentBuffer arguments;
        FFI_CHECK(callFunction(nullPtr, arguments).isNull());
    }
    if (JSFFIFunction* echoPtr = makeFunction({ T::Pointer }, T::Pointer, reinterpret_cast<void*>(ffi_echo_ptr), "ffi_echo_ptr"_s)) {
        MarkedArgumentBuffer zero;
        zero.append(jsNumber(0));
        FFI_CHECK(callFunction(echoPtr, zero).isNull());
        MarkedArgumentBuffer nullArgument;
        nullArgument.append(jsNull());
        FFI_CHECK(callFunction(echoPtr, nullArgument).isNull());
        MarkedArgumentBuffer minusOne;
        minusOne.append(jsNumber(-1));
        JSValue signExtended = callFunction(echoPtr, minusOne);
        FFI_CHECK(signExtended.isBigInt());
        if (signExtended.isBigInt())
            FFI_CHECK_EQ(JSBigInt::toBigUInt64(signExtended), 0xFFFFFFFFFFFFFFFFull);
        MarkedArgumentBuffer typedArray;
        JSUint8Array* uint8Array = makeUint8Array(16);
        if (uint8Array) {
            typedArray.append(uint8Array);
            JSValue address = callFunction(echoPtr, typedArray);
            FFI_CHECK(address.isNumber());
            if (address.isNumber())
                FFI_CHECK_EQ(address.asNumber(), static_cast<double>(reinterpret_cast<uintptr_t>(uint8Array->vector())));
        }
    }

    if (JSFFIFunction* nanF32 = makeFunction({ }, T::Float, reinterpret_cast<void*>(ffi_ret_nan_f32), "ffi_ret_nan_f32"_s)) {
        MarkedArgumentBuffer arguments;
        JSValue result = callFunction(nanF32, arguments);
        FFI_CHECK(result.isNumber());
        if (result.isNumber())
            FFI_CHECK(std::isnan(result.asNumber()));
    }
    if (JSFFIFunction* nanF64 = makeFunction({ }, T::Double, reinterpret_cast<void*>(ffi_ret_impure_nan_f64), "ffi_ret_impure_nan_f64"_s)) {
        MarkedArgumentBuffer arguments;
        JSValue result = callFunction(nanF64, arguments);
        FFI_CHECK(result.isNumber());
        if (result.isNumber())
            FFI_CHECK(std::isnan(result.asNumber()));
    }
    if (JSFFIFunction* denormal = makeFunction({ }, T::Float, reinterpret_cast<void*>(ffi_ret_denormal_f32), "ffi_ret_denormal_f32"_s)) {
        MarkedArgumentBuffer arguments;
        JSValue result = callFunction(denormal, arguments);
        FFI_CHECK(result.isNumber());
        if (result.isNumber())
            FFI_CHECK_EQ(result.asNumber(), static_cast<double>(std::bit_cast<float>(0x00000001u)));
    }
    if (JSFFIFunction* negZero = makeFunction({ }, T::Double, reinterpret_cast<void*>(ffi_ret_neg_zero_f64), "ffi_ret_neg_zero_f64"_s)) {
        MarkedArgumentBuffer arguments;
        JSValue result = callFunction(negZero, arguments);
        FFI_CHECK(result.isNumber());
        if (result.isNumber())
            FFI_CHECK(std::signbit(result.asNumber()) && result.asNumber() == 0.0);
    }

    if (JSFFIFunction* negOneI64 = makeFunction({ }, T::Int64, reinterpret_cast<void*>(ffi_ret_neg_one_i64), "ffi_ret_neg_one_i64"_s)) {
        MarkedArgumentBuffer arguments;
        JSValue result = callFunction(negOneI64, arguments);
        FFI_CHECK(result.isBigInt());
        if (result.isBigInt())
            FFI_CHECK_EQ(JSBigInt::toBigInt64(result), -1);
    }
    if (JSFFIFunction* negOneU64Fast = makeFunction({ }, T::Uint64Fast, reinterpret_cast<void*>(ffi_ret_neg_one_u64), "ffi_ret_neg_one_u64(as u64_fast)"_s)) {
        MarkedArgumentBuffer arguments;
        JSValue result = callFunction(negOneU64Fast, arguments);
        FFI_CHECK(result.isBigInt());
        if (result.isBigInt())
            FFI_CHECK_EQ_HEX(JSBigInt::toBigUInt64(result), 0xffffffffffffffffull);
    }
    if (JSFFIFunction* widenChar = makeFunction({ T::Char }, T::Int64, reinterpret_cast<void*>(ffi_widen_char), "ffi_widen_char"_s)) {
        MarkedArgumentBuffer arguments;
        arguments.append(jsNumber(255));
        JSValue result = callFunction(widenChar, arguments);
        FFI_CHECK(result.isBigInt());
        if (result.isBigInt())
            FFI_CHECK_EQ(JSBigInt::toBigInt64(result), -1); // char is signed
    }

    if (JSFFIFunction* strlenFunction = makeFunction({ T::CString }, T::Uint64, reinterpret_cast<void*>(ffi_strlen), "ffi_strlen"_s)) {
        MarkedArgumentBuffer arguments;
        arguments.append(jsString(vm, String("hello"_s)));
        JSValue result = callFunction(strlenFunction, arguments);
        FFI_CHECK(result.isBigInt());
        if (result.isBigInt())
            FFI_CHECK_EQ(JSBigInt::toBigUInt64(result), 5u);
        MarkedArgumentBuffer utf8;
        utf8.append(jsString(vm, String::fromUTF8("h\xC3\xA9llo")));
        JSValue utf8Result = callFunction(strlenFunction, utf8);
        FFI_CHECK(utf8Result.isBigInt());
        if (utf8Result.isBigInt())
            FFI_CHECK_EQ(JSBigInt::toBigUInt64(utf8Result), 6u);
    }

    if (Options::useFFIICStub())
        FFI_CHECK(FFI::g_ffiCompileCounts.icStub.load() > icStubsBefore);

    if (JSFFIFunction* addI32 = makeFunction({ T::Int32, T::Int32 }, T::Int32, reinterpret_cast<void*>(ffi_add_i32), "ffi_add_i32"_s)) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        MarkedArgumentBuffer arguments;
        arguments.append(jsNumber(1));
        arguments.append(jsNumber(2));
        JSValue constructed = construct(globalObject, addI32, arguments, "not a constructor"_s);
        FFI_CHECK(!!scope.exception());
        UNUSED_PARAM(constructed);
        scope.clearException();
    }
}

static void testCallbackThunkEndToEnd()
{
    VM& vm = *s_vm;
    JSGlobalObject* globalObject = s_globalObject;
    using T = FFI::Type;

    auto makeCallback = [&](Vector<T>&& argumentTypes, T returnType, JSFunction* target) -> JSFFICallback* {
        RefPtr<FFI::Signature> signature = FFI::Signature::tryCreate(argumentTypes.span(), returnType);
        FFI_CHECK(!!signature);
        if (!signature)
            return nullptr;
        JSFFICallback* callback = JSFFICallback::create(vm, globalObject, globalObject->ffiCallbackStructure(), target, signature.releaseNonNull());
        FFI_CHECK(!!callback);
        if (callback) {
            gcProtect(callback);
            FFI_CHECK(callback->nativeEntrypoint());
        }
        return callback;
    };

    JSFunction* timesThreePlusOne = JSFunction::create(vm, globalObject, 1, "timesThreePlusOne"_s, functionTimesThreePlusOne, ImplementationVisibility::Public);
    JSFunction* weighted = JSFunction::create(vm, globalObject, 0, "weighted"_s, functionSumWeightedArguments, ImplementationVisibility::Public);
    JSFunction* returnsTwo = JSFunction::create(vm, globalObject, 0, "returnsTwo"_s, functionReturnTwo, ImplementationVisibility::Public);
    JSFunction* returnsNaN = JSFunction::create(vm, globalObject, 0, "returnsNaN"_s, functionReturnNaN, ImplementationVisibility::Public);
    JSFunction* returnsMinusOne = JSFunction::create(vm, globalObject, 0, "returnsMinusOne"_s, functionReturnMinusOne, ImplementationVisibility::Public);

    if (JSFFICallback* callback = makeCallback({ T::Int32 }, T::Int32, timesThreePlusOne)) {
        auto function = reinterpret_cast<int32_t (*)(int32_t)>(callback->nativeEntrypoint());
        FFI_CHECK_EQ(ffi_call_cb_i32(function, 7), 22);
        FFI_CHECK_EQ(ffi_call_cb_i32(function, -1), -2);
        FFI_CHECK_EQ(ffi_call_cb_i32(function, 0), 1);
        FFI_CHECK_EQ(ffi_call_cb_i32(function, 2147483647), static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(2147483647.0 * 3.0 + 1.0))));
        FFI_CHECK_EQ(ffi_call_cb_reentrant(function, 100), [] {
            int32_t sum = 0;
            for (int32_t i = 0; i < 100; ++i)
                sum += i * 3 + 1;
            return sum;
        }());
    }
    if (JSFFICallback* callback = makeCallback(Vector<T>(FillWith { }, 9, T::Int32), T::Int64, weighted)) {
        auto function = reinterpret_cast<int64_t (*)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)>(callback->nativeEntrypoint());
        int64_t expected = 1 * 1 + 2 * -2 + 3 * 3 + 4 * -4 + 5 * 5 + 6 * -6 + 7 * 7 + 8 * -8 + 9 * 2147483647ll;
        FFI_CHECK_EQ(ffi_call_cb_i32_x9(function, 1, -2, 3, -4, 5, -6, 7, -8, 2147483647), expected);
    }
    if (JSFFICallback* callback = makeCallback(Vector<T>(FillWith { }, 9, T::Double), T::Double, weighted)) {
        auto function = reinterpret_cast<double (*)(double, double, double, double, double, double, double, double, double)>(callback->nativeEntrypoint());
        double expected = 1 * 0.5 + 2 * -0.25 + 3 * 3.5 + 4 * 4.5 + 5 * 5.5 + 6 * 6.5 + 7 * 7.5 + 8 * 8.5 + 9 * 1e10;
        FFI_CHECK_EQ(ffi_call_cb_f64_x9(function, 0.5, -0.25, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 1e10), expected);
    }
    if (JSFFICallback* callback = makeCallback(Vector<T>(FillWith { }, 10, T::Uint8), T::Int64, weighted)) {
        auto function = reinterpret_cast<int64_t (*)(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t)>(callback->nativeEntrypoint());
        int64_t expected = 1 * 255 + 2 * 0 + 3 * 128 + 4 * 1 + 5 * 200 + 6 * 17 + 7 * 254 + 8 * 3 + 9 * 99 + 10 * 250;
        FFI_CHECK_EQ(ffi_call_cb_u8_x10(function, 255, 0, 128, 1, 200, 17, 254, 3, 99, 250), expected);
    }
    if (JSFFICallback* callback = makeCallback({ T::Int32, T::Double, T::Int64Fast, T::Float, T::Pointer }, T::Double, weighted)) {
        auto function = reinterpret_cast<double (*)(int32_t, double, int64_t, float, void*)>(callback->nativeEntrypoint());
        void* pointer = reinterpret_cast<void*>(static_cast<uintptr_t>(8192));
        double expected = 1 * -5.0 + 2 * 2.5 + 3 * 4503599627370496.0 + 4 * static_cast<double>(1.25f) + 5 * 8192.0;
        FFI_CHECK_EQ(ffi_call_cb_mix(function, -5, 2.5, 4503599627370496ll, 1.25f, pointer), expected);
    }
    if (JSFFICallback* callback = makeCallback({ }, T::Bool, returnsTwo)) {
        auto function = reinterpret_cast<bool (*)(void)>(callback->nativeEntrypoint());
        FFI_CHECK_EQ(ffi_call_cb_ret_bool(function), 10); // JS 2 -> toBoolean -> true -> exactly 1 in the register
    }
    if (JSFFICallback* callback = makeCallback({ }, T::Uint8, returnsMinusOne)) {
        auto function = reinterpret_cast<uint8_t (*)(void)>(callback->nativeEntrypoint());
        FFI_CHECK_EQ(ffi_call_cb_ret_u8(function), 255);
    }
    if (JSFFICallback* callback = makeCallback({ }, T::Int8, returnsMinusOne)) {
        auto function = reinterpret_cast<int8_t (*)(void)>(callback->nativeEntrypoint());
        FFI_CHECK_EQ(ffi_call_cb_ret_i8(function), -1);
    }
    if (JSFFICallback* callback = makeCallback({ }, T::Int64, returnsMinusOne)) {
        auto function = reinterpret_cast<int64_t (*)(void)>(callback->nativeEntrypoint());
        FFI_CHECK_EQ(ffi_call_cb_ret_i64(function), -1);
    }
    if (JSFFICallback* callback = makeCallback({ }, T::Uint64, returnsMinusOne)) {
        auto function = reinterpret_cast<uint64_t (*)(void)>(callback->nativeEntrypoint());
        FFI_CHECK_EQ_HEX(ffi_call_cb_ret_u64(function), 0xffffffffffffffffull);
    }
    if (JSFFICallback* callback = makeCallback({ }, T::Float, returnsNaN)) {
        auto function = reinterpret_cast<float (*)(void)>(callback->nativeEntrypoint());
        FFI_CHECK(std::isnan(ffi_call_cb_ret_f32(function)));
    }
    if (JSFFICallback* callback = makeCallback({ }, T::Double, returnsNaN)) {
        auto function = reinterpret_cast<double (*)(void)>(callback->nativeEntrypoint());
        FFI_CHECK(std::isnan(ffi_call_cb_ret_f64(function)));
    }
    if (JSFFICallback* callback = makeCallback({ }, T::Pointer, returnsMinusOne)) {
        auto function = reinterpret_cast<void* (*)(void)>(callback->nativeEntrypoint());
        FFI_CHECK_EQ(reinterpret_cast<uintptr_t>(ffi_call_cb_ret_ptr(function)), static_cast<uintptr_t>(0xffffffffffffffffull));
    }
    if (JSFFICallback* callback = makeCallback({ T::Int32 }, T::Int32, timesThreePlusOne)) {
        void* before = callback->nativeEntrypoint();
        callback->close();
        callback->close();
        FFI_CHECK(callback->nativeEntrypoint() == before);
        FFI_CHECK(callback->callable() == timesThreePlusOne);
    }
}

static void testFixtureTable()
{
    auto fixtures = ffiTestFixtures();
    FFI_CHECK(fixtures.size() >= 90);
    for (const FFIFixtureEntry& entry : fixtures) {
        FFI_CHECK(entry.name);
        FFI_CHECK(entry.address);
        const FFIFixtureEntry* found = ffiTestFixtureNamed(entry.name);
        FFI_CHECK(found == &entry);
    }
    FFI_CHECK(!ffiTestFixtureNamed("ffi_no_such_fixture"));
    const FFIFixtureEntry* addI32 = ffiTestFixtureNamed("ffi_add_i32");
    FFI_CHECK(addI32);
    if (addI32)
        FFI_CHECK(addI32->address == reinterpret_cast<void*>(ffi_add_i32));

    FFI_CHECK_EQ(ffi_widen_char(-1), -1);
    FFI_CHECK_EQ(ffi_widen_i8(-1), -1);
    FFI_CHECK_EQ(ffi_widen_u8(255), 255);
    FFI_CHECK_EQ(ffi_ret_two_as_bool(), 2);
    FFI_CHECK(!ffi_ret_null_ptr());
    FFI_CHECK_EQ(reinterpret_cast<uintptr_t>(ffi_high_ptr()), static_cast<uintptr_t>(0x00007fffdeadbee0ull));
    FFI_CHECK(std::isnan(ffi_ret_nan_f32()));
    FFI_CHECK(std::isnan(ffi_ret_impure_nan_f64()));
    FFI_CHECK(std::signbit(ffi_ret_neg_zero_f64()));
    FFI_CHECK_EQ(std::bit_cast<uint32_t>(ffi_ret_denormal_f32()), 1u);
    FFI_CHECK_EQ(ffi_ret_inf_f64(), std::numeric_limits<double>::infinity());
    FFI_CHECK_EQ(ffi_align_probe_0(), 1.0);
    FFI_CHECK_EQ(ffi_align_probe_9(1, 2, 3, 4, 5, 6, 7, 8, 9), 1.0);
    FFI_CHECK_EQ(ffi_canary_call([] { }), 0);
    FFI_CHECK_EQ(ffi_mix_5(1, -1, 3, -4, 5, -6, 7, -8), 1 * 1.0 + 2 * -1.0 + 3 * 3.0 + 4 * -4.0 + 5 * 5.0 + 6 * -6.0 + 7 * 7.0 + 8 * -8.0);
}

} // anonymous namespace

static int runAll()
{
    JSC::initialize();

    if (!Options::useJIT()) {
        dataLogLn("testFFI: JIT is disabled; skipping.");
        return 0;
    }

    s_vm = VM::create();
    {
        JSLockHolder locker(*s_vm);
        s_globalObject = JSGlobalObject::create(*s_vm, JSGlobalObject::createStructure(*s_vm, jsNull()));
        gcProtect(s_globalObject);

        RUN(testTypeTraits());
        RUN(testSignatures());
        RUN(testFixtureTable());
        RUN(testCallLayoutGoldens());
        RUN(testCallLayoutAgainstReferenceModel());
        RUN(testDoubleToInt64());
        RUN(testConversions());
        RUN(testInvokeThunkDifferential());
        RUN(testCanaries());
        RUN(testJSFFIFunctionEndToEnd());
        RUN(testCallbackThunkEndToEnd());
    }

    dataLogLn(s_failureCount ? "FAILED: " : "OK: ", s_checkCount - s_failureCount, " checks passed, ", s_failureCount, " failed.");
    VM& leaked = s_vm.releaseNonNull().leakRef();
    UNUSED_PARAM(leaked);
    return s_failureCount ? 1 : 0;
}

#else

static int runAll()
{
    JSC::initialize();
    dataLogLn("testFFI: bun:ffi is not supported in this configuration (requires USE(BUN_JSC_ADDITIONS), ENABLE(JIT), x86-64 or arm64).");
    return 0;
}

#endif

int main(int argc, char** argv)
{
#if USE(BUN_JSC_ADDITIONS) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))
    if (argc == 2)
        s_filter = argv[1];
    else if (argc > 2) {
        dataLogLn("Usage: testFFI [<filter>]");
        return 1;
    }
#else
    UNUSED_PARAM(argc);
    UNUSED_PARAM(argv);
#endif
    return runAll();
}

#if OS(WINDOWS)
extern "C" __declspec(dllexport) int WINAPI dllLauncherEntryPoint(int argc, const char* argv[])
{
    return main(argc, const_cast<char**>(argv));
}
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
