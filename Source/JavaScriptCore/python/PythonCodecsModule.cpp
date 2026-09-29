/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "PythonCodecs.h"

#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonCharacters.h"
#include "PythonIO.h"
#include "PythonOperations.h"

// _codecs: Modules/_codecsmodule.c of CPython.

namespace JSC { namespace Python {

// codec_tuple()
static JSValue codecTuple(JSGlobalObject* globalObject, JSValue result, size_t length)
{
    return PyTuple::create(globalObject, { result, intFromInt64(globalObject, static_cast<int64_t>(length)) });
}

// `str(accept={str, NoneType}) = None`: a null String if it is None or was not given. Nothing if it raised.
static std::optional<String> toErrors(JSGlobalObject* globalObject, JSValue value, ASCIILiteral function)
{
    if (!value)
        return String();
    return toTextArgument(globalObject, value, function, "argument 2"_s, true);
}

// `Py_buffer(accept={str, buffer})`: the bytes of something that has bytes, or of a str in UTF-8. What is returned is what has them, to be kept for as long as they are wanted. Empty if it raised.
static JSValue bytesOrText(JSGlobalObject* globalObject, JSValue value, Buffer& buffer)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (stringIn(value)) {
        auto encoded = encodeUTF8(globalObject, value, String());
        RETURN_IF_EXCEPTION(scope, { });
        JSValue bytes = newBytes(globalObject, *encoded);
        RETURN_IF_EXCEPTION(scope, { });
        buffer = Buffer(bytes);
        return bytes;
    }
    buffer = bufferOf(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    return value;
}

enum class Decoder : uint8_t { UTF7, UTF8, UTF16, UTF16LE, UTF16BE, UTF16Ex, UTF32, UTF32LE, UTF32BE, UTF32Ex, UnicodeEscape, RawUnicodeEscape, Latin1, ASCII, Charmap };

static constexpr ASCIILiteral decoderNames[] = { "utf_7_decode"_s, "utf_8_decode"_s, "utf_16_decode"_s, "utf_16_le_decode"_s, "utf_16_be_decode"_s, "utf_16_ex_decode"_s, "utf_32_decode"_s, "utf_32_le_decode"_s,
    "utf_32_be_decode"_s, "utf_32_ex_decode"_s, "unicode_escape_decode"_s, "raw_unicode_escape_decode"_s, "latin_1_decode"_s, "ascii_decode"_s, "charmap_decode"_s };

// Every function that decodes to a str: (data, errors=None, ...)
PYTHON_NATIVE(codecsDecodeWith)
{
    NATIVE_PROLOGUE();
    auto decoder = unpack<Decoder>(callFrame, 0);
    ASCIILiteral function = decoderNames[static_cast<unsigned>(decoder)];
    bool isEscape = decoder == Decoder::UnicodeEscape || decoder == Decoder::RawUnicodeEscape;
    bool isExtended = decoder == Decoder::UTF16Ex || decoder == Decoder::UTF32Ex;
    bool isStateless = decoder == Decoder::Latin1 || decoder == Decoder::ASCII || decoder == Decoder::Charmap;

    Buffer buffer;
    if (isEscape)
        bytesOrText(globalObject, args[0], buffer);
    else
        buffer = bufferOf(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    auto errors = toErrors(globalObject, args.at(1), function);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned next = 2;
    int byteOrder = decoder == Decoder::UTF16LE || decoder == Decoder::UTF32LE ? -1 : decoder == Decoder::UTF16BE || decoder == Decoder::UTF32BE ? 1 : 0;
    if (isExtended) {
        if (JSValue value = args.at(next)) {
            auto given = toCInt(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            byteOrder = *given;
        }
        ++next;
    }
    bool isFinal = isEscape;
    JSValue mapping;
    if (decoder == Decoder::Charmap) {
        mapping = args.at(next);
        if (mapping && isNone(mapping))
            mapping = JSValue();
    } else if (!isStateless) {
        if (JSValue value = args.at(next)) {
            isFinal = isTrue(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }

    auto bytes = buffer.span();
    size_t consumed = bytes.size();
    size_t* consumedIfNotFinal = isFinal ? nullptr : &consumed;
    String decoded;
    switch (decoder) {
    case Decoder::UTF7:
        decoded = decodeUTF7(globalObject, bytes, *errors, consumedIfNotFinal);
        break;
    case Decoder::UTF8:
        decoded = decodeUTF8(globalObject, bytes, *errors, consumedIfNotFinal);
        break;
    case Decoder::UTF16:
    case Decoder::UTF16LE:
    case Decoder::UTF16BE:
    case Decoder::UTF16Ex:
        decoded = decodeUTF16(globalObject, bytes, *errors, &byteOrder, consumedIfNotFinal);
        break;
    case Decoder::UTF32:
    case Decoder::UTF32LE:
    case Decoder::UTF32BE:
    case Decoder::UTF32Ex:
        decoded = decodeUTF32(globalObject, bytes, *errors, &byteOrder, consumedIfNotFinal);
        break;
    case Decoder::UnicodeEscape:
        decoded = decodeUnicodeEscape(globalObject, bytes, *errors, consumedIfNotFinal);
        break;
    case Decoder::RawUnicodeEscape:
        decoded = decodeRawUnicodeEscape(globalObject, bytes, *errors, consumedIfNotFinal);
        break;
    case Decoder::Latin1:
        decoded = decodeLatin1(globalObject, bytes);
        break;
    case Decoder::ASCII:
        decoded = decodeASCII(globalObject, bytes, *errors);
        break;
    case Decoder::Charmap:
        decoded = decodeCharmap(globalObject, buffer, mapping, *errors);
        break;
    }
    RETURN_IF_EXCEPTION(scope, { });
    JSValue text = strOrMemoryError(globalObject, decoded);
    RETURN_IF_EXCEPTION(scope, { });
    if (isExtended)
        RELEASE_AND_RETURN(scope, JSValue::encode(PyTuple::create(globalObject, { text, intFromInt64(globalObject, static_cast<int64_t>(consumed)), jsNumber(byteOrder) })));
    RELEASE_AND_RETURN(scope, JSValue::encode(codecTuple(globalObject, text, consumed)));
}

enum class Encoder : uint8_t { UTF7, UTF8, UTF16, UTF16LE, UTF16BE, UTF32, UTF32LE, UTF32BE, UnicodeEscape, RawUnicodeEscape, Latin1, ASCII, Charmap };

static constexpr ASCIILiteral encoderNames[] = { "utf_7_encode"_s, "utf_8_encode"_s, "utf_16_encode"_s, "utf_16_le_encode"_s, "utf_16_be_encode"_s, "utf_32_encode"_s, "utf_32_le_encode"_s, "utf_32_be_encode"_s,
    "unicode_escape_encode"_s, "raw_unicode_escape_encode"_s, "latin_1_encode"_s, "ascii_encode"_s, "charmap_encode"_s };

// Every function that encodes a str: (str, errors=None, ...)
PYTHON_NATIVE(codecsEncodeWith)
{
    NATIVE_PROLOGUE();
    auto encoder = unpack<Encoder>(callFrame, 0);
    ASCIILiteral function = encoderNames[static_cast<unsigned>(encoder)];
    JSValue string = args[0];
    if (!stringIn(string))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate(function, "() argument 1 must be str, not "_s, typeNameOfArgument(globalObject, string))));
    auto errors = toErrors(globalObject, args.at(1), function);
    RETURN_IF_EXCEPTION(scope, { });
    int byteOrder = encoder == Encoder::UTF16LE || encoder == Encoder::UTF32LE ? -1 : encoder == Encoder::UTF16BE || encoder == Encoder::UTF32BE ? 1 : 0;
    if (encoder == Encoder::UTF16 || encoder == Encoder::UTF32) {
        if (JSValue value = args.at(2)) {
            auto given = toCInt(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            byteOrder = *given;
        }
    }
    std::optional<ByteVector> encoded;
    switch (encoder) {
    case Encoder::UTF7:
        encoded = encodeUTF7(globalObject, string);
        break;
    case Encoder::UTF8:
        encoded = encodeUTF8(globalObject, string, *errors);
        break;
    case Encoder::UTF16:
    case Encoder::UTF16LE:
    case Encoder::UTF16BE:
        encoded = encodeUTF16(globalObject, string, *errors, byteOrder);
        break;
    case Encoder::UTF32:
    case Encoder::UTF32LE:
    case Encoder::UTF32BE:
        encoded = encodeUTF32(globalObject, string, *errors, byteOrder);
        break;
    case Encoder::UnicodeEscape:
        encoded = encodeUnicodeEscape(globalObject, string);
        break;
    case Encoder::RawUnicodeEscape:
        encoded = encodeRawUnicodeEscape(globalObject, string);
        break;
    case Encoder::Latin1:
        encoded = encodeLatin1(globalObject, string, *errors);
        break;
    case Encoder::ASCII:
        encoded = encodeASCII(globalObject, string, *errors);
        break;
    case Encoder::Charmap: {
        JSValue mapping = args.at(2);
        encoded = encodeCharmap(globalObject, string, mapping && !isNone(mapping) ? mapping : JSValue(), *errors);
        break;
    }
    }
    RETURN_IF_EXCEPTION(scope, { });
    JSValue bytes = newBytes(globalObject, *encoded);
    RETURN_IF_EXCEPTION(scope, { });
    String text = stringIn(string)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(codecTuple(globalObject, bytes, Characters(vm, text).count())));
}

// escape_decode(data, errors=None) and readbuffer_encode(data, errors=None)
PYTHON_NATIVE(codecsBytesFrom)
{
    NATIVE_PROLOGUE();
    bool decodesEscapes = unpack<bool>(callFrame, 0);
    Buffer buffer;
    bytesOrText(globalObject, args[0], buffer);
    RETURN_IF_EXCEPTION(scope, { });
    auto errors = toErrors(globalObject, args.at(1), decodesEscapes ? "escape_decode"_s : "readbuffer_encode"_s);
    RETURN_IF_EXCEPTION(scope, { });
    size_t size = buffer.size();
    JSValue result;
    if (decodesEscapes) {
        auto decoded = decodeEscape(globalObject, buffer.span(), *errors);
        RETURN_IF_EXCEPTION(scope, { });
        result = newBytes(globalObject, *decoded);
    } else
        result = newBytes(globalObject, buffer.span());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(codecTuple(globalObject, result, size)));
}

PYTHON_NATIVE(codecsEscapeEncode)
{
    NATIVE_PROLOGUE();
    if (!typeOf(globalObject, args[0])->hasFlag(PyType::IsBytes))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("escape_encode() argument 1 must be bytes, not "_s, typeNameOfArgument(globalObject, args[0]))));
    toErrors(globalObject, args.at(1), "escape_encode"_s);
    RETURN_IF_EXCEPTION(scope, { });
    auto data = *builtinBufferOf(args[0]);
    static constexpr auto digits = "0123456789abcdef"_s;
    ByteVector out;
    for (uint8_t c : data) {
        if (c == '\'' || c == '\\')
            out.appendList({ '\\', c });
        else if (c == '\t')
            out.appendList({ '\\', 't' });
        else if (c == '\n')
            out.appendList({ '\\', 'n' });
        else if (c == '\r')
            out.appendList({ '\\', 'r' });
        else if (c < ' ' || c >= 0x7F)
            out.appendList({ '\\', 'x', static_cast<uint8_t>(digits[c >> 4]), static_cast<uint8_t>(digits[c & 0xF]) });
        else
            out.append(c);
    }
    JSValue result = newBytes(globalObject, out);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(codecTuple(globalObject, result, data.size())));
}

PYTHON_NATIVE(codecsCharmapBuild)
{
    NATIVE_PROLOGUE();
    if (!stringIn(args[0]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("charmap_build() argument must be str, not "_s, typeNameOfArgument(globalObject, args[0]))));
    RELEASE_AND_RETURN(scope, JSValue::encode(buildEncodingMap(globalObject, args[0])));
}

// ---- The registry

// register(search_function) and unregister(search_function)
PYTHON_NATIVE(codecsRegister)
{
    NATIVE_PROLOGUE();
    if (unpack<bool>(callFrame, 0))
        registerCodecSearchFunction(globalObject, args[0]);
    else
        unregisterCodecSearchFunction(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(codecsLookup)
{
    NATIVE_PROLOGUE();
    auto encoding = toTextArgument(globalObject, args[0], "lookup"_s, "argument"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(lookupCodec(globalObject, *encoding)));
}

// encode(obj, encoding='utf-8', errors='strict') and decode() likewise
PYTHON_NATIVE(codecsEncodeOrDecode)
{
    NATIVE_PROLOGUE();
    bool isEncoding = unpack<bool>(callFrame, 0);
    ASCIILiteral function = isEncoding ? "encode"_s : "decode"_s;
    String encoding = "utf-8"_s;
    if (JSValue value = args.at(1)) {
        auto given = toTextArgument(globalObject, value, function, "argument 'encoding'"_s);
        RETURN_IF_EXCEPTION(scope, { });
        encoding = *given;
    }
    String errors;
    if (JSValue value = args.at(2)) {
        auto given = toTextArgument(globalObject, value, function, "argument 'errors'"_s);
        RETURN_IF_EXCEPTION(scope, { });
        errors = *given;
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(isEncoding ? encodeWithCodec(globalObject, args.at(0), encoding, errors) : decodeWithCodec(globalObject, args.at(0), encoding, errors)));
}

PYTHON_NATIVE(codecsRegisterError)
{
    NATIVE_PROLOGUE();
    auto name = toTextArgument(globalObject, args[0], "register_error"_s, "argument 1"_s);
    RETURN_IF_EXCEPTION(scope, { });
    registerErrorHandler(globalObject, *name, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(codecsUnregisterError)
{
    NATIVE_PROLOGUE();
    auto name = toTextArgument(globalObject, args[0], "_unregister_error"_s, "argument"_s);
    RETURN_IF_EXCEPTION(scope, { });
    auto wasThere = unregisterErrorHandler(globalObject, *name);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(*wasThere));
}

PYTHON_NATIVE(codecsLookupError)
{
    NATIVE_PROLOGUE();
    auto name = toTextArgument(globalObject, args[0], "lookup_error"_s, "argument"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(lookupErrorHandler(globalObject, *name)));
}

JSObject* createCodecsModule(JSGlobalObject* globalObject)
{
    JSObject* module = newBuiltinModule(globalObject, "_codecs"_s);
    auto add = [&] (ASCIILiteral name, NativeFunction function, unsigned data = 0) { addFunction(globalObject, module, name, function, data); };
    add("register"_s, codecsRegister, pack(true));
    add("unregister"_s, codecsRegister, pack(false));
    add("lookup"_s, codecsLookup);
    add("encode"_s, codecsEncodeOrDecode, pack(true));
    add("decode"_s, codecsEncodeOrDecode, pack(false));
    add("escape_encode"_s, codecsEscapeEncode);
    add("escape_decode"_s, codecsBytesFrom, pack(true));
    add("readbuffer_encode"_s, codecsBytesFrom, pack(false));
    for (unsigned i = 0; i < std::size(decoderNames); ++i)
        add(decoderNames[i], codecsDecodeWith, pack(static_cast<Decoder>(i)));
    for (unsigned i = 0; i < std::size(encoderNames); ++i)
        add(encoderNames[i], codecsEncodeWith, pack(static_cast<Encoder>(i)));
    add("charmap_build"_s, codecsCharmapBuild);
    add("register_error"_s, codecsRegisterError);
    add("_unregister_error"_s, codecsUnregisterError);
    add("lookup_error"_s, codecsLookupError);
    return module;
}

} } // namespace JSC::Python
