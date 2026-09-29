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
#include "PythonOperations.h"
#include <unicode/uchar.h>

// What the codecs are written with, and str.encode() and bytes.decode(): Objects/unicodeobject.c of CPython.

namespace JSC { namespace Python {

CodePoints::CodePoints(const String& string)
    : m_string(string)
{
    if (string.isEmpty())
        return;
    if (string.is8Bit()) {
        auto characters = string.span8();
        m_characters8 = characters.data();
        m_size = characters.size();
        m_isASCII = charactersAreAllASCII(characters);
        return;
    }
    auto units = string.span16();
    bool hasPairs = false;
    char16_t all = 0;
    for (size_t i = 0; i < units.size(); ++i) {
        all |= units[i];
        if (U16_IS_LEAD(units[i]) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1]))
            hasPairs = true;
    }
    m_isLatin1 = all < 0x100;
    m_isASCII = all < 0x80;
    if (!hasPairs) {
        m_characters16 = units.data();
        m_size = units.size();
        return;
    }
    m_expanded.reserveInitialCapacity(units.size());
    for (size_t i = 0; i < units.size(); ++i) {
        char32_t c = units[i];
        if (U16_IS_LEAD(c) && i + 1 < units.size() && U16_IS_TRAIL(units[i + 1]))
            c = U16_GET_SUPPLEMENTARY(c, units[++i]);
        m_expanded.append(c);
    }
    m_size = m_expanded.size();
}

String textOfString(JSGlobalObject* globalObject, JSValue string)
{
    String text = stringIn(string)->value(globalObject);
    return text.isNull() ? emptyString() : text;
}

String TextWriter::finish(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (m_builder.hasOverflowed()) {
        raiseMemoryError(globalObject, scope);
        return { };
    }
    return m_builder.isEmpty() ? emptyString() : m_builder.toString();
}

ErrorHandler errorHandlerNamed(const String& errors)
{
    if (errors.isNull() || errors == "strict"_s)
        return ErrorHandler::Strict;
    if (errors == "surrogateescape"_s)
        return ErrorHandler::SurrogateEscape;
    if (errors == "replace"_s)
        return ErrorHandler::Replace;
    if (errors == "ignore"_s)
        return ErrorHandler::Ignore;
    if (errors == "backslashreplace"_s)
        return ErrorHandler::BackslashReplace;
    if (errors == "surrogatepass"_s)
        return ErrorHandler::SurrogatePass;
    if (errors == "xmlcharrefreplace"_s)
        return ErrorHandler::XMLCharRefReplace;
    return ErrorHandler::Other;
}

// The exception is told where the trouble is now: PyUnicodeDecodeError_SetStart() and the rest.
static void setWhere(JSGlobalObject* globalObject, JSValue exception, size_t start, size_t end, ASCIILiteral reason)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    JSObject* object = asObject(exception);
    object->putDirect(vm, names.field_start, intFromInt64(globalObject, static_cast<int64_t>(start)));
    object->putDirect(vm, names.field_end, intFromInt64(globalObject, static_cast<int64_t>(end)));
    object->putDirect(vm, names.field_reason, jsString(vm, String(reason)));
}

// What a handler returned, taken apart as by PyArg_ParseTuple() with "On;message". False if it raised.
static bool parseHandlerResult(JSGlobalObject* globalObject, JSValue result, ASCIILiteral message, JSValue& replacement, int64_t& position)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!isInstance(globalObject, result, globalObject->pyRealm()->typeTuple()) || asTuple(result)->length() != 2) {
        raiseTypeError(globalObject, scope, message);
        return false;
    }
    replacement = asTuple(result)->at(0);
    auto index = toSsize(globalObject, asTuple(result)->at(1));
    RETURN_IF_EXCEPTION(scope, false);
    position = *index;
    return true;
}

bool DecodeErrors::handle(ASCIILiteral encoding, ASCIILiteral reason, size_t start, size_t end, size_t& position, TextWriter& writer)
{
    JSGlobalObject* globalObject = m_globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    constexpr auto message = "decoding error handler must return (str, int) tuple"_s;
    if (!m_handler) {
        m_handler = lookupErrorHandler(globalObject, m_errors);
        RETURN_IF_EXCEPTION(scope, false);
    }
    // make_decode_exception()
    if (!m_exception) {
        JSValue copy = newBytes(globalObject, input);
        RETURN_IF_EXCEPTION(scope, false);
        MarkedArgumentBuffer arguments;
        arguments.append(jsString(vm, String(encoding)));
        arguments.append(copy);
        arguments.append(intFromInt64(globalObject, static_cast<int64_t>(start)));
        arguments.append(intFromInt64(globalObject, static_cast<int64_t>(end)));
        arguments.append(jsString(vm, String(reason)));
        m_exception = call(globalObject, globalObject->pyRealm()->typeUnicodeDecodeError(), arguments);
        RETURN_IF_EXCEPTION(scope, false);
    } else
        setWhere(globalObject, m_exception, start, end, reason);

    JSValue result = call(globalObject, m_handler, m_exception);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue replacement;
    int64_t newPosition;
    if (!parseHandlerResult(globalObject, result, message, replacement, newPosition))
        return false;
    if (!stringIn(replacement)) {
        raiseTypeError(globalObject, scope, message);
        return false;
    }

    // What is being decoded is what the exception has now, which the handler may have changed.
    JSValue object = asObject(m_exception)->getDirect(vm, vm.pythonNames().field_subject);
    if (!object) {
        raiseTypeError(globalObject, scope, "UnicodeError 'object' attribute is not set"_s);
        return false;
    }
    if (!typeOf(globalObject, object)->hasFlag(PyType::IsBytes)) {
        raiseTypeError(globalObject, scope, "UnicodeError 'object' attribute must be a bytes"_s);
        return false;
    }
    input = *builtinBufferOf(object);
    int64_t size = static_cast<int64_t>(input.size());
    if (newPosition < 0)
        newPosition += size;
    if (newPosition < 0 || newPosition > size) {
        raise(globalObject, scope, BuiltinType::IndexError, concatenate("position "_s, newPosition, " from error handler out of bounds"_s));
        return false;
    }
    String text = stringIn(replacement)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    writer.append(text);
    position = static_cast<size_t>(newPosition);
    return true;
}

bool EncodeErrors::makeException(ASCIILiteral encoding, ASCIILiteral reason, size_t start, size_t end)
{
    JSGlobalObject* globalObject = m_globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_exception) {
        setWhere(globalObject, m_exception, start, end, reason);
        return true;
    }
    MarkedArgumentBuffer arguments;
    arguments.append(jsString(vm, String(encoding)));
    arguments.append(m_string);
    arguments.append(intFromInt64(globalObject, static_cast<int64_t>(start)));
    arguments.append(intFromInt64(globalObject, static_cast<int64_t>(end)));
    arguments.append(jsString(vm, String(reason)));
    m_exception = call(globalObject, globalObject->pyRealm()->typeUnicodeEncodeError(), arguments);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

void EncodeErrors::raise(ASCIILiteral encoding, ASCIILiteral reason, size_t start, size_t end)
{
    auto scope = DECLARE_THROW_SCOPE(m_globalObject->vm());
    if (!makeException(encoding, reason, start, end))
        return;
    setContext(m_globalObject, asObject(m_exception));
    throwException(m_globalObject, scope, m_exception);
}

JSValue EncodeErrors::handle(ASCIILiteral encoding, ASCIILiteral reason, size_t start, size_t end, size_t& newPosition)
{
    JSGlobalObject* globalObject = m_globalObject;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    constexpr auto message = "encoding error handler must return (str/bytes, int) tuple"_s;
    if (!m_handler) {
        m_handler = lookupErrorHandler(globalObject, m_errors);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!makeException(encoding, reason, start, end))
        return { };
    JSValue result = call(globalObject, m_handler, m_exception);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue replacement;
    int64_t position;
    if (!parseHandlerResult(globalObject, result, message, replacement, position))
        return { };
    if (!stringIn(replacement) && !typeOf(globalObject, replacement)->hasFlag(PyType::IsBytes))
        return raiseTypeError(globalObject, scope, message);
    int64_t size = static_cast<int64_t>(m_length);
    if (position < 0)
        position += size;
    if (position < 0 || position > size)
        return Python::raise(globalObject, scope, BuiltinType::IndexError, concatenate("position "_s, position, " from error handler out of bounds"_s));
    newPosition = static_cast<size_t>(position);
    return replacement;
}

String nameOfCharacter(char32_t c)
{
    char buffer[256];
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = u_charName(c, U_UNICODE_CHAR_NAME, buffer, sizeof(buffer), &status);
    if (U_FAILURE(status) || !length)
        return { };
    return String::fromLatin1(buffer);
}

std::optional<char32_t> characterNamed(std::span<const uint8_t> name)
{
    Vector<char, 64> upper;
    for (uint8_t c : name) {
        if (c >= 0x80)
            return std::nullopt;
        upper.append(toASCIIUpper(static_cast<char>(c)));
    }
    upper.append('\0');
    for (UCharNameChoice choice : { U_UNICODE_CHAR_NAME, U_CHAR_NAME_ALIAS }) {
        UErrorCode status = U_ZERO_ERROR;
        char32_t value = u_charFromName(choice, upper.span().data(), &status);
        if (U_SUCCESS(status))
            return value;
    }
    return std::nullopt;
}

// ---- str.encode() and bytes.decode()

enum class Shortcut : uint8_t { None, UTF8, UTF16, UTF32, ASCII, Latin1 };

// The names that are known without asking: the "fast paths" of PyUnicode_Decode() and PyUnicode_AsEncodedString().
static Shortcut shortcutFor(const String& encoding)
{
    if (encoding.isNull())
        return Shortcut::UTF8;
    String lower = normalizeEncodingName(encoding);
    // What is longer than the longest of them, "iso_8859_1", is not looked at.
    if (lower.length() > 10)
        return Shortcut::None;
    if (lower.startsWith("utf"_s)) {
        StringView rest = StringView(lower).substring(3);
        if (rest.startsWith('_'))
            rest = rest.substring(1);
        if (rest == "8"_s)
            return Shortcut::UTF8;
        if (rest == "16"_s)
            return Shortcut::UTF16;
        if (rest == "32"_s)
            return Shortcut::UTF32;
        return Shortcut::None;
    }
    if (lower == "ascii"_s || lower == "us_ascii"_s)
        return Shortcut::ASCII;
    if (lower == "latin1"_s || lower == "latin_1"_s || lower == "iso_8859_1"_s || lower == "iso8859_1"_s)
        return Shortcut::Latin1;
    return Shortcut::None;
}

// What is decoded without asking the registry. False if it is for the registry.
static bool decodeIfKnown(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& encoding, const String& errors, String& result)
{
    if (bytes.empty()) {
        result = emptyString();
        return true;
    }
    switch (shortcutFor(encoding)) {
    case Shortcut::UTF8:
        result = decodeUTF8(globalObject, bytes, errors);
        return true;
    case Shortcut::UTF16:
        result = decodeUTF16(globalObject, bytes, errors);
        return true;
    case Shortcut::UTF32:
        result = decodeUTF32(globalObject, bytes, errors);
        return true;
    case Shortcut::ASCII:
        result = decodeASCII(globalObject, bytes, errors);
        return true;
    case Shortcut::Latin1:
        result = decodeLatin1(globalObject, bytes);
        return true;
    case Shortcut::None:
        break;
    }
    return false;
}

// A str, or an instance of a class derived from it, which is what the codec gave. Empty if it raised.
static JSValue decodeByRegistry(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& encoding, const String& errors)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // What the codec is given is a view that cannot be written through, of bytes that stay where they are.
    JSValue copy = newBytes(globalObject, bytes);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue view = call(globalObject, globalObject->pyRealm()->typeMemoryView(), copy);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = decodeTextWithCodec(globalObject, view, encoding, errors);
    RETURN_IF_EXCEPTION(scope, { });
    if (!stringIn(result))
        return raiseTypeError(globalObject, scope, concatenate('\'', encoding, "' decoder returned '"_s, typeName(globalObject, result), "' instead of 'str'; use codecs.decode() to decode to arbitrary types"_s));
    return result;
}

String decodeBytes(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& encoding, const String& errors)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String text;
    if (decodeIfKnown(globalObject, bytes, encoding, errors, text))
        return text;
    JSValue result = decodeByRegistry(globalObject, bytes, encoding, errors);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, textOfString(globalObject, result));
}

JSValue decodeBytesToObject(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& encoding, const String& errors)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String text;
    if (!decodeIfKnown(globalObject, bytes, encoding, errors, text))
        RELEASE_AND_RETURN(scope, decodeByRegistry(globalObject, bytes, encoding, errors));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, text));
}

static bool encodeIfKnown(JSGlobalObject* globalObject, JSValue string, const String& encoding, const String& errors, std::optional<ByteVector>& result)
{
    switch (shortcutFor(encoding)) {
    case Shortcut::UTF8:
        result = encodeUTF8(globalObject, string, errors);
        return true;
    case Shortcut::UTF16:
        result = encodeUTF16(globalObject, string, errors, 0);
        return true;
    case Shortcut::UTF32:
        result = encodeUTF32(globalObject, string, errors, 0);
        return true;
    case Shortcut::ASCII:
        result = encodeASCII(globalObject, string, errors);
        return true;
    case Shortcut::Latin1:
        result = encodeLatin1(globalObject, string, errors);
        return true;
    case Shortcut::None:
        break;
    }
    return false;
}

// A bytes, or an instance of a class derived from it, which is what the codec gave. Empty if it raised.
static JSValue encodeByRegistry(JSGlobalObject* globalObject, JSValue string, const String& encoding, const String& errors)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = encodeTextWithCodec(globalObject, string, encoding, errors);
    RETURN_IF_EXCEPTION(scope, { });
    PyType* type = typeOf(globalObject, result);
    if (type->hasFlag(PyType::IsBytes))
        return result;
    if (type->isSubtypeOf(globalObject->pyRealm()->typeByteArray())) {
        if (!warn(globalObject, BuiltinType::RuntimeWarning, concatenate("encoder "_s, encoding, " returned bytearray instead of bytes; use codecs.encode() to encode to arbitrary types"_s)))
            return { };
        RELEASE_AND_RETURN(scope, newBytes(globalObject, *builtinBufferOf(result)));
    }
    return raiseTypeError(globalObject, scope, concatenate('\'', encoding, "' encoder returned '"_s, typeName(globalObject, result), "' instead of 'bytes'; use codecs.encode() to encode to arbitrary types"_s));
}

std::optional<ByteVector> encodeString(JSGlobalObject* globalObject, JSValue string, const String& encoding, const String& errors)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    std::optional<ByteVector> encoded;
    if (encodeIfKnown(globalObject, string, encoding, errors, encoded))
        return encoded;
    JSValue result = encodeByRegistry(globalObject, string, encoding, errors);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    ByteVector bytes;
    bytes.append(*builtinBufferOf(result));
    return bytes;
}

JSValue encodeStringToObject(JSGlobalObject* globalObject, JSValue string, const String& encoding, const String& errors)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    std::optional<ByteVector> encoded;
    if (!encodeIfKnown(globalObject, string, encoding, errors, encoded))
        RELEASE_AND_RETURN(scope, encodeByRegistry(globalObject, string, encoding, errors));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, newBytes(globalObject, *encoded));
}

} } // namespace JSC::Python
