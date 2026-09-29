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

#include "PyStateObject.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonOperations.h"
#include "PythonUnicodeData.h"

// The codecs of a byte to a character, and those that write characters out as escapes: Objects/unicodeobject.c of CPython, and PyBytes_DecodeEscape() of Objects/bytesobject.c.

namespace JSC { namespace Python {

void appendBackslashReplacement(ByteVector&, const CodePoints&, size_t start, size_t end);
void appendXMLCharacterReferences(ByteVector&, const CodePoints&, size_t start, size_t end);

static constexpr auto hexDigits = "0123456789abcdef"_s;

static void appendHex(ByteVector& out, char32_t c, unsigned count)
{
    while (count--)
        out.append(hexDigits[(c >> (4 * count)) & 0xF]);
}

// ---- unicode-escape

// What an escape that is not one is warned of with. False if that raised.
static bool warnOfInvalidEscape(JSGlobalObject* globalObject, int character, ASCIILiteral prefix)
{
    if (character == -1)
        return true;
    if (character > 0xFF) {
        StringBuilder octal;
        for (int shift = 6; shift >= 0; shift -= 3)
            octal.append(static_cast<char>('0' + ((character >> shift) & 7)));
        return warn(globalObject, BuiltinType::DeprecationWarning, concatenate(prefix, "\"\\"_s, octal.toString(), "\" is an invalid octal escape sequence. Such sequences will not work in the future. "_s));
    }
    return warn(globalObject, BuiltinType::DeprecationWarning, concatenate(prefix, "\"\\"_s, static_cast<char16_t>(character), "\" is an invalid escape sequence. Such sequences will not work in the future. "_s));
}

String decodeUnicodeEscape(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& errors, size_t* consumed)
{
    if (bytes.empty()) {
        if (consumed)
            *consumed = 0;
        return emptyString();
    }
    DecodeErrors handler(globalObject, errors, bytes);
    auto& input = handler.input;
    TextWriter writer;
    int firstInvalidEscape = -1;
    size_t s = 0;
    while (s < input.size()) {
        uint32_t c = input[s++];
        if (c != '\\') {
            writer.append(static_cast<char32_t>(c));
            continue;
        }
        size_t start = s - 1;
        ASCIILiteral message;
        bool isIncomplete = false;
        [&] {
            if (s >= input.size()) {
                message = "\\ at end of string"_s;
                isIncomplete = true;
                return;
            }
            c = input[s++];
            unsigned count;
            switch (c) {
            case '\n':
                return;
            case '\\':
            case '\'':
            case '"':
                writer.append(static_cast<char32_t>(c));
                return;
            case 'b':
                writer.append(static_cast<char32_t>('\b'));
                return;
            case 'f':
                writer.append(static_cast<char32_t>('\f'));
                return;
            case 't':
                writer.append(static_cast<char32_t>('\t'));
                return;
            case 'n':
                writer.append(static_cast<char32_t>('\n'));
                return;
            case 'r':
                writer.append(static_cast<char32_t>('\r'));
                return;
            case 'v':
                writer.append(static_cast<char32_t>('\v'));
                return;
            case 'a':
                writer.append(static_cast<char32_t>('\a'));
                return;
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                uint32_t value = c - '0';
                for (unsigned more = 0; more < 2 && s < input.size() && input[s] >= '0' && input[s] <= '7'; ++more)
                    value = (value << 3) + input[s++] - '0';
                if (value > 0377 && firstInvalidEscape == -1)
                    firstInvalidEscape = static_cast<int>(value);
                writer.append(static_cast<char32_t>(value));
                return;
            }
            case 'x':
                count = 2;
                message = "truncated \\xXX escape"_s;
                break;
            case 'u':
                count = 4;
                message = "truncated \\uXXXX escape"_s;
                break;
            case 'U':
                count = 8;
                message = "truncated \\UXXXXXXXX escape"_s;
                break;
            case 'N': {
                message = "malformed \\N character escape"_s;
                if (s >= input.size()) {
                    isIncomplete = true;
                    return;
                }
                if (input[s] != '{')
                    return;
                size_t nameStart = ++s;
                while (s < input.size() && input[s] != '}')
                    ++s;
                if (s >= input.size()) {
                    isIncomplete = true;
                    return;
                }
                size_t nameLength = s - nameStart;
                if (!nameLength)
                    return;
                ++s;
                if (auto named = Unicode::characterNamed(input.subspan(nameStart, nameLength))) {
                    writer.append(*named);
                    message = { };
                    return;
                }
                message = "unknown Unicode character name"_s;
                return;
            }
            default:
                if (firstInvalidEscape == -1)
                    firstInvalidEscape = static_cast<int>(c);
                writer.append(static_cast<char32_t>('\\'));
                writer.append(static_cast<char32_t>(c));
                return;
            }
            uint32_t value = 0;
            for (; count; ++s, --count) {
                if (s >= input.size()) {
                    isIncomplete = true;
                    return;
                }
                c = input[s];
                if (!isASCIIHexDigit(c))
                    return;
                value = (value << 4) + toASCIIHexValue(c);
            }
            if (value > 0x10FFFF) {
                message = "illegal Unicode character"_s;
                return;
            }
            writer.append(static_cast<char32_t>(value));
            message = { };
        }();
        if (message.isNull())
            continue;
        if (isIncomplete && consumed) {
            *consumed = start;
            consumed = nullptr;
            break;
        }
        if (!handler.handle("unicodeescape"_s, message, start, s, s, writer))
            return { };
    }
    if (consumed)
        *consumed = bytes.size();
    String result = writer.finish(globalObject);
    if (result.isNull() || !warnOfInvalidEscape(globalObject, firstInvalidEscape, ""_s))
        return { };
    return result;
}

std::optional<ByteVector> encodeUnicodeEscape(JSGlobalObject* globalObject, JSValue string)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String text = textOfString(globalObject, string);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    CodePoints characters(text);
    ByteVector out;
    for (size_t i = 0; i < characters.size(); ++i) {
        char32_t c = characters[i];
        if (c >= 0x10000) {
            out.appendList({ '\\', 'U' });
            appendHex(out, c, 8);
        } else if (c >= 0x100) {
            out.appendList({ '\\', 'u' });
            appendHex(out, c, 4);
        } else if (c >= ' ' && c < 127) {
            if (c == '\\')
                out.append('\\');
            out.append(static_cast<uint8_t>(c));
        } else if (c == '\t')
            out.appendList({ '\\', 't' });
        else if (c == '\n')
            out.appendList({ '\\', 'n' });
        else if (c == '\r')
            out.appendList({ '\\', 'r' });
        else {
            out.appendList({ '\\', 'x' });
            appendHex(out, c, 2);
        }
    }
    return out;
}

// ---- raw-unicode-escape

String decodeRawUnicodeEscape(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& errors, size_t* consumed)
{
    if (bytes.empty()) {
        if (consumed)
            *consumed = 0;
        return emptyString();
    }
    DecodeErrors handler(globalObject, errors, bytes);
    auto& input = handler.input;
    TextWriter writer;
    size_t s = 0;
    bool stoppedEarly = false;
    while (s < input.size()) {
        uint32_t c = input[s++];
        if (c != '\\' || (s >= input.size() && !consumed)) {
            writer.append(static_cast<char32_t>(c));
            continue;
        }
        size_t start = s - 1;
        ASCIILiteral message = "\\ at end of string"_s;
        bool isIncomplete = s >= input.size();
        bool isWrong = false;
        if (!isIncomplete) {
            c = input[s++];
            if (c != 'u' && c != 'U') {
                writer.append(static_cast<char32_t>('\\'));
                writer.append(static_cast<char32_t>(c));
                continue;
            }
            unsigned count = c == 'u' ? 4 : 8;
            message = c == 'u' ? "truncated \\uXXXX escape"_s : "truncated \\UXXXXXXXX escape"_s;
            uint32_t value = 0;
            for (; count; ++s, --count) {
                if (s >= input.size()) {
                    isIncomplete = true;
                    break;
                }
                c = input[s];
                if (!isASCIIHexDigit(c)) {
                    isWrong = true;
                    break;
                }
                value = (value << 4) + toASCIIHexValue(c);
            }
            if (!isIncomplete && !isWrong) {
                if (value <= 0x10FFFF) {
                    writer.append(static_cast<char32_t>(value));
                    continue;
                }
                message = "\\Uxxxxxxxx out of range"_s;
            }
        }
        if (isIncomplete && consumed) {
            *consumed = start;
            stoppedEarly = true;
            break;
        }
        if (!handler.handle("rawunicodeescape"_s, message, start, s, s, writer))
            return { };
    }
    if (consumed && !stoppedEarly)
        *consumed = bytes.size();
    return writer.finish(globalObject);
}

std::optional<ByteVector> encodeRawUnicodeEscape(JSGlobalObject* globalObject, JSValue string)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String text = textOfString(globalObject, string);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    CodePoints characters(text);
    ByteVector out;
    for (size_t i = 0; i < characters.size(); ++i) {
        char32_t c = characters[i];
        if (c < 0x100)
            out.append(static_cast<uint8_t>(c));
        else if (c < 0x10000) {
            out.appendList({ '\\', 'u' });
            appendHex(out, c, 4);
        } else {
            out.appendList({ '\\', 'U' });
            appendHex(out, c, 8);
        }
    }
    return out;
}

// ---- The escapes of a bytes literal

std::optional<ByteVector> decodeEscape(JSGlobalObject* globalObject, std::span<const uint8_t> input, const String& errors)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    ByteVector out;
    int firstInvalidEscape = -1;
    size_t s = 0;
    size_t end = input.size();
    while (s < end) {
        if (input[s] != '\\') {
            out.append(input[s++]);
            continue;
        }
        if (++s == end) {
            raiseValueError(globalObject, scope, "Trailing \\ in string"_s);
            return std::nullopt;
        }
        uint8_t c = input[s++];
        switch (c) {
        case '\n':
            break;
        case '\\':
        case '\'':
        case '"':
            out.append(c);
            break;
        case 'b':
            out.append('\b');
            break;
        case 'f':
            out.append('\f');
            break;
        case 't':
            out.append('\t');
            break;
        case 'n':
            out.append('\n');
            break;
        case 'r':
            out.append('\r');
            break;
        case 'v':
            out.append('\v');
            break;
        case 'a':
            out.append('\a');
            break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            int value = c - '0';
            for (unsigned more = 0; more < 2 && s < end && input[s] >= '0' && input[s] <= '7'; ++more)
                value = (value << 3) + input[s++] - '0';
            if (value > 0377 && firstInvalidEscape == -1)
                firstInvalidEscape = value;
            out.append(static_cast<uint8_t>(value));
            break;
        }
        case 'x':
            if (s + 1 < end && isASCIIHexDigit(input[s]) && isASCIIHexDigit(input[s + 1])) {
                out.append(static_cast<uint8_t>((toASCIIHexValue(input[s]) << 4) + toASCIIHexValue(input[s + 1])));
                s += 2;
                break;
            }
            if (errors.isNull() || errors == "strict"_s) {
                raiseValueError(globalObject, scope, concatenate("invalid \\x escape at position "_s, s - 2));
                return std::nullopt;
            }
            if (errors == "replace"_s)
                out.append('?');
            else if (errors != "ignore"_s) {
                raiseValueError(globalObject, scope, concatenate("decoding error; unknown error handling code: "_s, errors));
                return std::nullopt;
            }
            if (s < end && isASCIIHexDigit(input[s]))
                ++s;
            break;
        default:
            if (firstInvalidEscape == -1)
                firstInvalidEscape = c;
            out.append('\\');
            --s;
        }
    }
    if (!warnOfInvalidEscape(globalObject, firstInvalidEscape, "b"_s))
        return std::nullopt;
    return out;
}

// ---- Latin-1 and ASCII

String decodeLatin1(JSGlobalObject* globalObject, std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return emptyString();
    TextWriter writer;
    writer.append(String(byteCast<Latin1Character>(bytes.first(std::min<size_t>(bytes.size(), String::MaxLength)))));
    if (bytes.size() > String::MaxLength)
        writer.append(String(byteCast<Latin1Character>(bytes.subspan(String::MaxLength).first(1))));
    return writer.finish(globalObject);
}

String decodeASCII(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& errors)
{
    if (bytes.empty())
        return emptyString();
    if (charactersAreAllASCII(bytes))
        return decodeLatin1(globalObject, bytes);
    DecodeErrors handler(globalObject, errors, bytes);
    auto& input = handler.input;
    TextWriter writer;
    ErrorHandler known = ErrorHandler::Unknown;
    size_t s = 0;
    while (s < input.size()) {
        uint8_t c = input[s];
        if (c < 128) {
            writer.append(static_cast<char32_t>(c));
            ++s;
            continue;
        }
        if (known == ErrorHandler::Unknown)
            known = errorHandlerNamed(errors);
        switch (known) {
        case ErrorHandler::Replace:
            writer.append(static_cast<char32_t>(0xFFFD));
            ++s;
            break;
        case ErrorHandler::SurrogateEscape:
            writer.append(static_cast<char32_t>(0xDC00 + c));
            ++s;
            break;
        case ErrorHandler::Ignore:
            ++s;
            break;
        default:
            if (!handler.handle("ascii"_s, "ordinal not in range(128)"_s, s, s + 1, s, writer))
                return { };
        }
    }
    return writer.finish(globalObject);
}

// unicode_encode_ucs1()
static std::optional<ByteVector> encodeUpTo(JSGlobalObject* globalObject, JSValue string, const String& errors, char32_t limit)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String text = textOfString(globalObject, string);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    ByteVector out;
    if (text.is8Bit() && (limit == 256 || charactersAreAllASCII(text.span8()))) {
        out.append(text.span8());
        return out;
    }
    CodePoints characters(text);
    size_t size = characters.size();
    ASCIILiteral encoding = limit == 256 ? "latin-1"_s : "ascii"_s;
    ASCIILiteral reason = limit == 256 ? "ordinal not in range(256)"_s : "ordinal not in range(128)"_s;
    EncodeErrors handler(globalObject, errors, string, size);
    ErrorHandler known = ErrorHandler::Unknown;
    size_t position = 0;
    while (position < size) {
        char32_t c = characters[position];
        if (c < limit) {
            out.append(static_cast<uint8_t>(c));
            ++position;
            continue;
        }
        // All that cannot be encoded, one after another, is seen to at once.
        size_t start = position;
        size_t end = start + 1;
        while (end < size && characters[end] >= limit)
            ++end;
        if (known == ErrorHandler::Unknown)
            known = errorHandlerNamed(errors);
        bool isHandled = true;
        switch (known) {
        case ErrorHandler::Strict:
            handler.raise(encoding, reason, start, end);
            return std::nullopt;
        case ErrorHandler::Replace:
            out.appendFill('?', end - start);
            break;
        case ErrorHandler::Ignore:
            break;
        case ErrorHandler::BackslashReplace:
            appendBackslashReplacement(out, characters, start, end);
            break;
        case ErrorHandler::XMLCharRefReplace:
            appendXMLCharacterReferences(out, characters, start, end);
            break;
        case ErrorHandler::SurrogateEscape: {
            size_t i = start;
            for (; i < end; ++i) {
                c = characters[i];
                if (c < 0xDC80 || c > 0xDCFF)
                    break;
                out.append(static_cast<uint8_t>(c - 0xDC00));
            }
            if (i < end) {
                start = i;
                isHandled = false;
            }
            break;
        }
        default:
            isHandled = false;
        }
        if (isHandled) {
            position = end;
            continue;
        }
        size_t newPosition;
        JSValue replacement = handler.handle(encoding, reason, start, end, newPosition);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (JSString* replacementString = stringIn(replacement)) {
            String replacementText = replacementString->value(globalObject);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            CodePoints replacementCharacters(replacementText);
            if (limit == 256 ? !replacementCharacters.isLatin1() : !replacementCharacters.isASCII()) {
                handler.raise(encoding, reason, start, end);
                return std::nullopt;
            }
            for (size_t i = 0; i < replacementCharacters.size(); ++i)
                out.append(static_cast<uint8_t>(replacementCharacters[i]));
        } else
            out.append(*builtinBufferOf(replacement));
        position = newPosition;
    }
    return out;
}

std::optional<ByteVector> encodeLatin1(JSGlobalObject* globalObject, JSValue string, const String& errors) { return encodeUpTo(globalObject, string, errors, 256); }
std::optional<ByteVector> encodeASCII(JSGlobalObject* globalObject, JSValue string, const String& errors) { return encodeUpTo(globalObject, string, errors, 128); }

// ---- charmap

static constexpr auto undefinedMapping = "character maps to <undefined>"_s;

// PyMapping_GetOptionalItem(), where LookupError of any kind is that there is none. Empty if there is none, or if it raised.
static JSValue lookUpInMapping(JSGlobalObject* globalObject, JSValue mapping, uint32_t key)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue item = getItem(globalObject, mapping, intFromUInt64(globalObject, key));
    if (scope.exception()) {
        catchException(globalObject, BuiltinType::LookupError);
        return { };
    }
    return item;
}

String decodeCharmap(JSGlobalObject* globalObject, const Buffer& buffer, JSValue mapping, const String& errors)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    auto bytes = buffer.span();
    if (!mapping)
        RELEASE_AND_RETURN(scope, decodeLatin1(globalObject, bytes));
    if (bytes.empty())
        return emptyString();
    DecodeErrors handler(globalObject, errors, bytes);
    auto& input = handler.input;
    TextWriter writer;
    size_t s = 0;

    // charmap_decode_string()
    if (mapping.isString()) {
        String table = asString(mapping)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        CodePoints characters(table);
        while (s < input.size()) {
            uint8_t c = input[s];
            char32_t x = c < characters.size() ? characters[c] : 0xFFFE;
            if (x != 0xFFFE) {
                writer.append(x);
                ++s;
                continue;
            }
            if (!handler.handle("charmap"_s, undefinedMapping, s, s + 1, s, writer))
                return { };
        }
        RELEASE_AND_RETURN(scope, writer.finish(globalObject));
    }

    // charmap_decode_mapping()
    while (s < input.size()) {
        JSValue item = lookUpInMapping(globalObject, mapping, input[s]);
        RETURN_IF_EXCEPTION(scope, { });
        if (handler.hasOriginalInput())
            input = buffer.span();
        bool isUndefined = !item || isNone(item);
        if (!isUndefined) {
            if (isInstance(globalObject, item, realm->typeInt())) {
                auto value = toCLong(globalObject, item);
                if (scope.exception()) {
                    // What does not fit is out of range, whatever else it is.
                    if (!scope.tryClearException())
                        return { };
                    value = -1;
                }
                if (*value == 0xFFFE)
                    isUndefined = true;
                else if (*value < 0 || *value > 0x10FFFF) {
                    raiseTypeError(globalObject, scope, "character mapping must be in range(0x110000)"_s);
                    return { };
                } else
                    writer.append(static_cast<char32_t>(*value));
            } else if (JSString* string = stringIn(item)) {
                String text = string->value(globalObject);
                RETURN_IF_EXCEPTION(scope, { });
                if (text.length() == 1 && text[0] == 0xFFFE)
                    isUndefined = true;
                else
                    writer.append(text);
            } else {
                raiseTypeError(globalObject, scope, "character mapping must return integer, None or str"_s);
                return { };
            }
        }
        if (!isUndefined) {
            ++s;
            continue;
        }
        // There may be less of it than there was.
        if (s >= input.size())
            break;
        if (!handler.handle("charmap"_s, undefinedMapping, s, s + 1, s, writer))
            return { };
    }
    RELEASE_AND_RETURN(scope, writer.finish(globalObject));
}

namespace {

// What codecs.charmap_build() makes of a table that decodes: the way back, in three levels.
struct EncodingMapState final : NativeState {
    PYTHON_NATIVE_STATE(EncodingMapState);

    // encoding_map_lookup(). -1 if there is none.
    int lookup(char32_t c) const
    {
        if (c > 0xFFFF)
            return -1;
        if (!c)
            return 0;
        unsigned i = level1[c >> 11];
        if (i == 0xFF)
            return -1;
        i = level23[16 * i + ((c >> 7) & 0xF)];
        if (i == 0xFF)
            return -1;
        i = level23[16 * count2 + 128 * i + (c & 0x7F)];
        return i ? static_cast<int>(i) : -1;
    }

    std::array<uint8_t, 32> level1;
    unsigned count2 { 0 };
    unsigned count3 { 0 };
    Vector<uint8_t> level23;
};

template<typename Visitor>
void EncodingMapState::visit(Visitor&)
{
}

} // anonymous namespace

PYTHON_NATIVE(encodingMapSize)
{
    NativeArguments args(callFrame);
    UNUSED_PARAM(globalObject);
    auto& map = stateOf<EncodingMapState>(args[0]);
    // What it comes to in CPython: sizeof(struct encoding_map) - 1, and the second and third levels.
    return JSValue::encode(intFromUInt64(globalObject, 63 + 16 * map.count2 + 128 * map.count3));
}

static PyType* encodingMapType(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    auto& slot = realm->codecRegistry().encodingMap;
    if (slot)
        return slot.get();
    PyType* type = createBuiltinType(globalObject, "EncodingMap"_s, realm->typeObject(), PyType::Layout::Native, 0);
    slot.set(vm, realm, type);
    type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
    addMethods(globalObject, type, { { "size"_s, encodingMapSize } });
    return type;
}

JSValue buildEncodingMap(JSGlobalObject* globalObject, JSValue string)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String text = textOfString(globalObject, string);
    RETURN_IF_EXCEPTION(scope, { });
    CodePoints characters(text);
    if (!characters.size())
        return raiseTypeError(globalObject, scope, "bad argument type for built-in operation"_s);
    unsigned length = static_cast<unsigned>(std::min<size_t>(characters.size(), 256));
    std::array<uint8_t, 32> level1;
    std::array<uint8_t, 512> level2;
    level1.fill(0xFF);
    level2.fill(0xFF);
    unsigned count2 = 0;
    unsigned count3 = 0;
    // If 0 does not stand for itself, or there is something past the first 65536 characters, it takes a dict.
    bool needsDict = !!characters[0];
    for (unsigned i = 1; i < length; ++i) {
        char32_t c = characters[i];
        if (!c || c > 0xFFFF) {
            needsDict = true;
            break;
        }
        if (c == 0xFFFE)
            continue;
        if (level1[c >> 11] == 0xFF)
            level1[c >> 11] = count2++;
        if (level2[c >> 7] == 0xFF)
            level2[c >> 7] = count3++;
    }
    if (count2 >= 0xFF || count3 >= 0xFF)
        needsDict = true;
    if (needsDict) {
        PyDict* result = PyDict::create(globalObject);
        for (unsigned i = 0; i < length; ++i) {
            result->set(globalObject, intFromUInt64(globalObject, static_cast<uint32_t>(characters[i])), intFromUInt64(globalObject, i));
            RETURN_IF_EXCEPTION(scope, { });
        }
        return result;
    }
    auto state = makeUnique<EncodingMapState>();
    state->level1 = level1;
    state->count2 = count2;
    state->count3 = count3;
    state->level23.fill(0xFF, 16 * count2);
    state->level23.grow(16 * count2 + 128 * count3);
    for (unsigned i = 16 * count2; i < state->level23.size(); ++i)
        state->level23[i] = 0;
    count3 = 0;
    for (unsigned i = 1; i < length; ++i) {
        char32_t c = characters[i];
        if (c == 0xFFFE)
            continue;
        unsigned i2 = 16 * level1[c >> 11] + ((c >> 7) & 0xF);
        if (state->level23[i2] == 0xFF)
            state->level23[i2] = count3++;
        state->level23[16 * count2 + 128 * state->level23[i2] + (c & 0x7F)] = static_cast<uint8_t>(i);
    }
    return PyStateObject::create(vm, encodingMapType(globalObject)->instanceStructure(), WTF::move(state));
}

enum class CharmapResult : uint8_t { Success, Failed, Exception };

// charmapencode_lookup(): an int, whose value is put in `replace`, a bytes, or None if there is nothing for the character. Empty if it raised.
static JSValue lookUpForEncoding(JSGlobalObject* globalObject, char32_t c, JSValue mapping, uint8_t& replace)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue item = lookUpInMapping(globalObject, mapping, c);
    RETURN_IF_EXCEPTION(scope, { });
    if (!item || isNone(item))
        return jsUndefined();
    if (isInstance(globalObject, item, globalObject->pyRealm()->typeInt())) {
        auto value = tryInt64(toInt(globalObject, item));
        if (!value || *value < 0 || *value > 255)
            return raiseTypeError(globalObject, scope, "character mapping must be in range(256)"_s);
        replace = static_cast<uint8_t>(*value);
        return item;
    }
    if (typeOf(globalObject, item)->hasFlag(PyType::IsBytes))
        return item;
    return raiseTypeError(globalObject, scope, concatenate("character mapping must return integer, bytes or None, not "_s, typeName(globalObject, item)));
}

// charmapencode_output()
static CharmapResult encodeOneWithMapping(JSGlobalObject* globalObject, char32_t c, JSValue mapping, const EncodingMapState* map, ByteVector& out)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (map) {
        int result = map->lookup(c);
        if (result == -1)
            return CharmapResult::Failed;
        out.append(static_cast<uint8_t>(result));
        return CharmapResult::Success;
    }
    uint8_t replace = 0;
    JSValue item = lookUpForEncoding(globalObject, c, mapping, replace);
    RETURN_IF_EXCEPTION(scope, CharmapResult::Exception);
    if (isNone(item))
        return CharmapResult::Failed;
    if (typeOf(globalObject, item)->hasFlag(PyType::IsBytes))
        out.append(*builtinBufferOf(item));
    else
        out.append(replace);
    return CharmapResult::Success;
}

std::optional<ByteVector> encodeCharmap(JSGlobalObject* globalObject, JSValue string, JSValue mapping, const String& errors)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!mapping)
        RELEASE_AND_RETURN(scope, encodeUpTo(globalObject, string, errors, 256));
    String text = textOfString(globalObject, string);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    CodePoints characters(text);
    size_t size = characters.size();
    const EncodingMapState* map = tryStateOf<EncodingMapState>(mapping);
    EncodeErrors handler(globalObject, errors, string, size);
    ErrorHandler known = ErrorHandler::Unknown;
    constexpr auto encoding = "charmap"_s;
    ByteVector out;
    size_t position = 0;
    while (position < size) {
        auto result = encodeOneWithMapping(globalObject, characters[position], mapping, map, out);
        if (result == CharmapResult::Exception)
            return std::nullopt;
        if (result == CharmapResult::Success) {
            ++position;
            continue;
        }

        // charmap_encoding_error()
        size_t start = position;
        size_t end = position + 1;
        while (end < size) {
            if (map) {
                if (map->lookup(characters[end]) != -1)
                    break;
            } else {
                uint8_t replace;
                JSValue item = lookUpForEncoding(globalObject, characters[end], mapping, replace);
                RETURN_IF_EXCEPTION(scope, std::nullopt);
                if (!isNone(item))
                    break;
            }
            ++end;
        }
        if (known == ErrorHandler::Unknown)
            known = errorHandlerNamed(errors);
        // What stands in for what cannot be encoded has to be encoded itself. False if it could not be, or if it raised.
        auto encodeReplacement = [&] (char32_t c) {
            auto replaced = encodeOneWithMapping(globalObject, c, mapping, map, out);
            if (replaced == CharmapResult::Failed)
                handler.raise(encoding, undefinedMapping, start, end);
            return replaced == CharmapResult::Success;
        };
        switch (known) {
        case ErrorHandler::Strict:
            handler.raise(encoding, undefinedMapping, start, end);
            return std::nullopt;
        case ErrorHandler::Replace:
            for (size_t i = start; i < end; ++i) {
                if (!encodeReplacement('?'))
                    return std::nullopt;
            }
            position = end;
            break;
        case ErrorHandler::Ignore:
            position = end;
            break;
        case ErrorHandler::XMLCharRefReplace:
            for (size_t i = start; i < end; ++i) {
                String reference = makeString("&#"_s, static_cast<unsigned>(characters[i]), ';');
                for (unsigned k = 0; k < reference.length(); ++k) {
                    if (!encodeReplacement(reference[k]))
                        return std::nullopt;
                }
            }
            position = end;
            break;
        default: {
            size_t newPosition;
            JSValue replacement = handler.handle(encoding, undefinedMapping, start, end, newPosition);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (JSString* replacementString = stringIn(replacement)) {
                String replacementText = replacementString->value(globalObject);
                RETURN_IF_EXCEPTION(scope, std::nullopt);
                CodePoints replacementCharacters(replacementText);
                for (size_t i = 0; i < replacementCharacters.size(); ++i) {
                    if (!encodeReplacement(replacementCharacters[i]))
                        return std::nullopt;
                }
            } else
                out.append(*builtinBufferOf(replacement));
            position = newPosition;
        }
        }
    }
    return out;
}

} } // namespace JSC::Python
