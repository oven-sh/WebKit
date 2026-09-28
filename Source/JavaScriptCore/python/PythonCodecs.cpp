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
#include <expected>
#include <unicode/uchar.h>
#include <wtf/text/StringBuilder.h>

// The codecs that are built in: UTF-8, UTF-16, UTF-32, ASCII and Latin-1. Where an error begins and ends, and what it is called, is as
// CPython has it, since that is what an error handler is given and what a message says.

namespace JSC { namespace Python {

enum class Codec : uint8_t { UTF8, UTF8Signature, ASCII, Latin1, UTF16, UTF16LE, UTF16BE, UTF32, UTF32LE, UTF32BE };

struct CodecInfo {
    Codec codec;
    ASCIILiteral name; // As it is in messages.
};

// utf8, UTF-8 and U8 are all one.
static std::optional<CodecInfo> findCodec(const String& encoding)
{
    if (encoding.isNull())
        return CodecInfo { Codec::UTF8, "utf-8"_s };
    StringBuilder builder;
    for (unsigned i = 0; i < encoding.length(); ++i) {
        char16_t c = encoding[i];
        builder.append(c == '-' || c == ' ' ? static_cast<char16_t>('_') : toASCIILower(c));
    }
    String name = builder.toString();
    static constexpr std::pair<ASCIILiteral, CodecInfo> table[] = {
        { "utf_8"_s, { Codec::UTF8, "utf-8"_s } }, { "utf8"_s, { Codec::UTF8, "utf-8"_s } }, { "u8"_s, { Codec::UTF8, "utf-8"_s } }, { "utf"_s, { Codec::UTF8, "utf-8"_s } }, { "cp65001"_s, { Codec::UTF8, "utf-8"_s } },
        { "utf_8_sig"_s, { Codec::UTF8Signature, "utf-8"_s } },
        { "ascii"_s, { Codec::ASCII, "ascii"_s } }, { "us_ascii"_s, { Codec::ASCII, "ascii"_s } }, { "646"_s, { Codec::ASCII, "ascii"_s } }, { "us"_s, { Codec::ASCII, "ascii"_s } }, { "ansi_x3.4_1968"_s, { Codec::ASCII, "ascii"_s } },
        { "latin_1"_s, { Codec::Latin1, "latin-1"_s } }, { "latin1"_s, { Codec::Latin1, "latin-1"_s } }, { "iso_8859_1"_s, { Codec::Latin1, "latin-1"_s } }, { "iso8859_1"_s, { Codec::Latin1, "latin-1"_s } },
        { "8859"_s, { Codec::Latin1, "latin-1"_s } }, { "cp819"_s, { Codec::Latin1, "latin-1"_s } }, { "latin"_s, { Codec::Latin1, "latin-1"_s } }, { "l1"_s, { Codec::Latin1, "latin-1"_s } },
        { "utf_16"_s, { Codec::UTF16, "utf-16"_s } }, { "utf16"_s, { Codec::UTF16, "utf-16"_s } }, { "u16"_s, { Codec::UTF16, "utf-16"_s } },
        { "utf_16_le"_s, { Codec::UTF16LE, "utf-16-le"_s } }, { "utf_16le"_s, { Codec::UTF16LE, "utf-16-le"_s } }, { "unicodelittleunmarked"_s, { Codec::UTF16LE, "utf-16-le"_s } },
        { "utf_16_be"_s, { Codec::UTF16BE, "utf-16-be"_s } }, { "utf_16be"_s, { Codec::UTF16BE, "utf-16-be"_s } }, { "unicodebigunmarked"_s, { Codec::UTF16BE, "utf-16-be"_s } },
        { "utf_32"_s, { Codec::UTF32, "utf-32"_s } }, { "utf32"_s, { Codec::UTF32, "utf-32"_s } }, { "u32"_s, { Codec::UTF32, "utf-32"_s } },
        { "utf_32_le"_s, { Codec::UTF32LE, "utf-32-le"_s } }, { "utf_32le"_s, { Codec::UTF32LE, "utf-32-le"_s } },
        { "utf_32_be"_s, { Codec::UTF32BE, "utf-32-be"_s } }, { "utf_32be"_s, { Codec::UTF32BE, "utf-32-be"_s } },
    };
    for (auto& [alias, info] : table) {
        if (name == alias)
            return info;
    }
    return std::nullopt;
}

enum class ErrorHandler : uint8_t { Strict, Ignore, Replace, BackslashReplace, XMLCharacterReference, NameReplace, SurrogateEscape, SurrogatePass };

static std::optional<ErrorHandler> findErrorHandler(const String& errors)
{
    if (errors.isNull() || errors == "strict"_s)
        return ErrorHandler::Strict;
    if (errors == "ignore"_s)
        return ErrorHandler::Ignore;
    if (errors == "replace"_s)
        return ErrorHandler::Replace;
    if (errors == "backslashreplace"_s)
        return ErrorHandler::BackslashReplace;
    if (errors == "xmlcharrefreplace"_s)
        return ErrorHandler::XMLCharacterReference;
    if (errors == "namereplace"_s)
        return ErrorHandler::NameReplace;
    if (errors == "surrogateescape"_s)
        return ErrorHandler::SurrogateEscape;
    if (errors == "surrogatepass"_s)
        return ErrorHandler::SurrogatePass;
    return std::nullopt;
}

static void raiseUnknownEncoding(JSGlobalObject* globalObject, ThrowScope& scope, const String& encoding)
{
    raise(globalObject, scope, BuiltinType::LookupError, makeString("unknown encoding: "_s, encoding));
}

static void raiseUnknownErrorHandler(JSGlobalObject* globalObject, ThrowScope& scope, const String& errors)
{
    raise(globalObject, scope, BuiltinType::LookupError, makeString("unknown error handler name '"_s, errors, '\''));
}

// UnicodeEncodeError(encoding, object, start, end, reason), and UnicodeDecodeError likewise.
static void raiseUnicodeError(JSGlobalObject* globalObject, ThrowScope& scope, BuiltinType type, ASCIILiteral encoding, JSValue object, size_t start, size_t end, ASCIILiteral reason)
{
    VM& vm = globalObject->vm();
    MarkedArgumentBuffer arguments;
    arguments.append(jsString(vm, String(encoding)));
    arguments.append(object);
    arguments.append(jsNumber(static_cast<int32_t>(start)));
    arguments.append(jsNumber(static_cast<int32_t>(end)));
    arguments.append(jsString(vm, String(reason)));
    JSValue exception = call(globalObject, globalObject->pyRealm()->type(type), arguments);
    RETURN_IF_EXCEPTION(scope, void());
    throwException(globalObject, scope, exception);
}

// ---- Encoding

static void appendASCII(ByteVector& output, const String& text)
{
    for (unsigned i = 0; i < text.length(); ++i)
        output.append(static_cast<uint8_t>(text[i]));
}

static String backslashEscape(char32_t c)
{
    if (c <= 0xFF)
        return makeString("\\x"_s, hex(static_cast<unsigned>(c), 2, Lowercase));
    if (c <= 0xFFFF)
        return makeString("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase));
    return makeString("\\U"_s, hex(static_cast<unsigned>(c), 8, Lowercase));
}

static String nameEscape(char32_t c)
{
    char buffer[128];
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = u_charName(c, U_UNICODE_CHAR_NAME, buffer, sizeof(buffer), &status);
    if (U_FAILURE(status) || !length)
        return backslashEscape(c);
    return makeString("\\N{"_s, String::fromLatin1(buffer), '}');
}

static void appendUnit(ByteVector& output, uint32_t unit, unsigned size, bool isBigEndian)
{
    for (unsigned i = 0; i < size; ++i)
        output.append(static_cast<uint8_t>(unit >> (8 * (isBigEndian ? size - 1 - i : i))));
}

std::optional<ByteVector> encodeString(JSGlobalObject* globalObject, JSValue stringValue, const String& encoding, const String& errors)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto info = findCodec(encoding);
    if (!info) {
        raiseUnknownEncoding(globalObject, scope, encoding);
        return std::nullopt;
    }
    String string = asString(stringValue)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    StringView view = string;
    ByteVector output;
    Codec codec = info->codec;

    // All of it, if it is all ASCII, whatever the codec, but for those that have wider units.
    if (view.is8Bit() && (codec == Codec::UTF8 || codec == Codec::ASCII || codec == Codec::Latin1)) {
        auto characters = view.span8();
        if (codec == Codec::Latin1 || charactersAreAllASCII(characters)) {
            output.append(characters);
            return output;
        }
    }

    // The characters, since errors are counted in them.
    Vector<char32_t, 64> characters;
    for (char32_t c : view.codePoints())
        characters.append(c);

    unsigned unitSize = 1;
    bool isBigEndian = false;
    switch (codec) {
    case Codec::UTF8Signature:
        output.appendList({ 0xEF, 0xBB, 0xBF });
        break;
    case Codec::UTF16:
        unitSize = 2;
        appendUnit(output, 0xFEFF, 2, false);
        break;
    case Codec::UTF16LE:
    case Codec::UTF16BE:
        unitSize = 2;
        isBigEndian = codec == Codec::UTF16BE;
        break;
    case Codec::UTF32:
        unitSize = 4;
        appendUnit(output, 0xFEFF, 4, false);
        break;
    case Codec::UTF32LE:
    case Codec::UTF32BE:
        unitSize = 4;
        isBigEndian = codec == Codec::UTF32BE;
        break;
    default:
        break;
    }
    bool isUTF = codec != Codec::ASCII && codec != Codec::Latin1;
    char32_t limit = codec == Codec::ASCII ? 0x80 : codec == Codec::Latin1 ? 0x100 : 0x110000;

    auto canEncode = [&] (char32_t c) { return c < limit && !(isUTF && U_IS_SURROGATE(c)); };
    auto encodeOne = [&] (char32_t c) {
        if (unitSize == 4)
            return appendUnit(output, c, 4, isBigEndian);
        if (unitSize == 2) {
            if (c >= 0x10000) {
                appendUnit(output, U16_LEAD(c), 2, isBigEndian);
                appendUnit(output, U16_TRAIL(c), 2, isBigEndian);
            } else
                appendUnit(output, c, 2, isBigEndian);
            return;
        }
        if (!isUTF || c < 0x80)
            return output.append(static_cast<uint8_t>(c));
        if (c < 0x800) {
            output.append(static_cast<uint8_t>(0xC0 | (c >> 6)));
            output.append(static_cast<uint8_t>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            output.append(static_cast<uint8_t>(0xE0 | (c >> 12)));
            output.append(static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F)));
            output.append(static_cast<uint8_t>(0x80 | (c & 0x3F)));
        } else {
            output.append(static_cast<uint8_t>(0xF0 | (c >> 18)));
            output.append(static_cast<uint8_t>(0x80 | ((c >> 12) & 0x3F)));
            output.append(static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F)));
            output.append(static_cast<uint8_t>(0x80 | (c & 0x3F)));
        }
    };
    // What a handler puts in place of a character is text, which is encoded in its turn.
    auto encodeText = [&] (const String& text) {
        for (unsigned i = 0; i < text.length(); ++i)
            encodeOne(text[i]);
    };

    std::optional<ErrorHandler> handler;
    for (size_t i = 0; i < characters.size();) {
        char32_t c = characters[i];
        if (canEncode(c)) [[likely]] {
            encodeOne(c);
            ++i;
            continue;
        }
        // All that cannot be encoded, up to something that can.
        size_t end = i + 1;
        while (end < characters.size() && !canEncode(characters[end]))
            end++;
        if (!handler) {
            handler = findErrorHandler(errors);
            if (!handler) {
                raiseUnknownErrorHandler(globalObject, scope, errors);
                return std::nullopt;
            }
        }
        ASCIILiteral reason = isUTF ? "surrogates not allowed"_s : codec == Codec::ASCII ? "ordinal not in range(128)"_s : "ordinal not in range(256)"_s;
        auto fail = [&] (size_t from, size_t to) {
            raiseUnicodeError(globalObject, scope, BuiltinType::UnicodeEncodeError, info->name, stringValue, from, to, reason);
            return std::nullopt;
        };
        switch (*handler) {
        case ErrorHandler::Strict:
            return fail(i, end);
        case ErrorHandler::Ignore:
            break;
        case ErrorHandler::Replace:
            for (size_t k = i; k < end; ++k)
                encodeOne('?');
            break;
        case ErrorHandler::BackslashReplace:
            for (size_t k = i; k < end; ++k)
                encodeText(backslashEscape(characters[k]));
            break;
        case ErrorHandler::XMLCharacterReference:
            for (size_t k = i; k < end; ++k)
                encodeText(makeString("&#"_s, static_cast<unsigned>(characters[k]), ';'));
            break;
        case ErrorHandler::NameReplace:
            for (size_t k = i; k < end; ++k)
                encodeText(nameEscape(characters[k]));
            break;
        case ErrorHandler::SurrogateEscape:
            // What a byte that could not be decoded was made into goes back to being that byte.
            for (size_t k = i; k < end; ++k) {
                if (characters[k] < 0xDC80 || characters[k] > 0xDCFF)
                    return fail(k, k + 1);
                output.append(static_cast<uint8_t>(characters[k] - 0xDC00));
            }
            break;
        case ErrorHandler::SurrogatePass:
            for (size_t k = i; k < end; ++k) {
                char32_t surrogate = characters[k];
                if (!isUTF || !U_IS_SURROGATE(surrogate))
                    return fail(k, k + 1);
                if (unitSize > 1)
                    appendUnit(output, surrogate, unitSize, isBigEndian);
                else {
                    output.append(static_cast<uint8_t>(0xE0 | (surrogate >> 12)));
                    output.append(static_cast<uint8_t>(0x80 | ((surrogate >> 6) & 0x3F)));
                    output.append(static_cast<uint8_t>(0x80 | (surrogate & 0x3F)));
                }
            }
            break;
        }
        i = end;
    }
    UNUSED_PARAM(appendASCII);
    return output;
}

// ---- Decoding

struct DecodeError {
    size_t length; // How many bytes are at fault.
    ASCIILiteral reason;
};

static bool isContinuation(uint8_t byte) { return (byte & 0xC0) == 0x80; }

// One character of UTF-8, at the front of the bytes. Either how many bytes it took, or what is wrong.
static std::expected<unsigned, DecodeError> decodeUTF8Character(std::span<const uint8_t> s, char32_t& result)
{
    constexpr ASCIILiteral invalidStart = "invalid start byte"_s;
    constexpr ASCIILiteral invalidContinuation = "invalid continuation byte"_s;
    auto unexpectedEnd = [&] { return std::unexpected(DecodeError { s.size(), "unexpected end of data"_s }); };
    uint8_t ch = s[0];
    if (ch < 0x80) {
        result = ch;
        return 1;
    }
    if (ch < 0xC2)
        return std::unexpected(DecodeError { 1, invalidStart });
    if (ch < 0xE0) {
        if (s.size() < 2)
            return unexpectedEnd();
        if (!isContinuation(s[1]))
            return std::unexpected(DecodeError { 1, invalidContinuation });
        result = ((ch & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (ch < 0xF0) {
        if (s.size() < 2)
            return unexpectedEnd();
        uint8_t ch2 = s[1];
        // Too long a way of writing something short, or half of a pair.
        if (!isContinuation(ch2) || (ch2 < 0xA0 ? ch == 0xE0 : ch == 0xED))
            return std::unexpected(DecodeError { 1, invalidContinuation });
        if (s.size() < 3)
            return unexpectedEnd();
        if (!isContinuation(s[2]))
            return std::unexpected(DecodeError { 2, invalidContinuation });
        result = ((ch & 0x0F) << 12) | ((ch2 & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if (ch < 0xF5) {
        if (s.size() < 2)
            return unexpectedEnd();
        uint8_t ch2 = s[1];
        if (!isContinuation(ch2) || (ch2 < 0x90 ? ch == 0xF0 : ch == 0xF4))
            return std::unexpected(DecodeError { 1, invalidContinuation });
        if (s.size() < 3)
            return unexpectedEnd();
        if (!isContinuation(s[2]))
            return std::unexpected(DecodeError { 2, invalidContinuation });
        if (s.size() < 4)
            return unexpectedEnd();
        if (!isContinuation(s[3]))
            return std::unexpected(DecodeError { 3, invalidContinuation });
        result = ((ch & 0x07) << 18) | ((ch2 & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    return std::unexpected(DecodeError { 1, invalidStart });
}

String decodeBytes(JSGlobalObject* globalObject, JSValue object, std::span<const uint8_t> input, const String& encoding, const String& errors)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto info = findCodec(encoding);
    if (!info) {
        raiseUnknownEncoding(globalObject, scope, encoding);
        return { };
    }
    Codec codec = info->codec;
    if (codec == Codec::Latin1)
        return String(byteCast<Latin1Character>(input));
    if ((codec == Codec::UTF8 || codec == Codec::ASCII) && charactersAreAllASCII(input))
        return String(byteCast<Latin1Character>(input));

    size_t position = 0;
    unsigned unitSize = 1;
    bool isBigEndian = false;
    ASCIILiteral name = info->name;
    auto readUnit = [&] (size_t at) {
        uint32_t unit = 0;
        for (unsigned i = 0; i < unitSize; ++i)
            unit |= static_cast<uint32_t>(input[at + i]) << (8 * (isBigEndian ? unitSize - 1 - i : i));
        return unit;
    };
    switch (codec) {
    case Codec::UTF8Signature:
        if (input.size() >= 3 && input[0] == 0xEF && input[1] == 0xBB && input[2] == 0xBF)
            position = 3;
        break;
    case Codec::UTF16:
    case Codec::UTF32:
        // A mark at the front says which way round the bytes are.
        unitSize = codec == Codec::UTF16 ? 2 : 4;
        if (input.size() >= unitSize) {
            uint32_t mark = readUnit(0);
            if (mark == 0xFEFF)
                position = unitSize;
            else if (mark == (unitSize == 2 ? 0xFFFEu : 0xFFFE0000u)) {
                isBigEndian = true;
                position = unitSize;
            }
        }
        break;
    case Codec::UTF16LE:
    case Codec::UTF16BE:
        unitSize = 2;
        isBigEndian = codec == Codec::UTF16BE;
        break;
    case Codec::UTF32LE:
    case Codec::UTF32BE:
        unitSize = 4;
        isBigEndian = codec == Codec::UTF32BE;
        break;
    default:
        break;
    }

    StringBuilder output;
    std::optional<ErrorHandler> handler;
    while (position < input.size()) {
        auto rest = input.subspan(position);
        char32_t character = 0;
        std::expected<unsigned, DecodeError> decoded = 0u;
        if (unitSize == 1) {
            if (codec == Codec::ASCII) {
                if (rest[0] < 0x80) {
                    character = rest[0];
                    decoded = 1u;
                } else
                    decoded = std::unexpected(DecodeError { 1, "ordinal not in range(128)"_s });
            } else
                decoded = decodeUTF8Character(rest, character);
        } else if (rest.size() < unitSize)
            decoded = std::unexpected(DecodeError { rest.size(), "truncated data"_s });
        else if (unitSize == 2) {
            character = readUnit(position);
            decoded = 2u;
            if (U16_IS_LEAD(character)) {
                if (rest.size() < 4)
                    decoded = std::unexpected(DecodeError { rest.size(), "unexpected end of data"_s });
                else if (uint32_t trail = readUnit(position + 2); U16_IS_TRAIL(trail)) {
                    character = U16_GET_SUPPLEMENTARY(character, trail);
                    decoded = 4u;
                } else
                    decoded = std::unexpected(DecodeError { 2, "illegal UTF-16 surrogate"_s });
            } else if (U16_IS_TRAIL(character))
                decoded = std::unexpected(DecodeError { 2, "illegal encoding"_s });
        } else {
            character = readUnit(position);
            decoded = 4u;
            if (U_IS_SURROGATE(character))
                decoded = std::unexpected(DecodeError { 4, "code point in surrogate code point range(0xd800, 0xe000)"_s });
            else if (character > 0x10FFFF)
                decoded = std::unexpected(DecodeError { 4, "code point not in range(0x110000)"_s });
        }
        if (decoded) [[likely]] {
            output.append(character);
            position += *decoded;
            continue;
        }

        DecodeError error = decoded.error();
        if (!handler) {
            handler = findErrorHandler(errors);
            if (!handler) {
                raiseUnknownErrorHandler(globalObject, scope, errors);
                return { };
            }
        }
        auto fail = [&] {
            raiseUnicodeError(globalObject, scope, BuiltinType::UnicodeDecodeError, name, object, position, position + error.length, error.reason);
            return String();
        };
        switch (*handler) {
        case ErrorHandler::Ignore:
            break;
        case ErrorHandler::Replace:
            output.append(static_cast<char16_t>(0xFFFD));
            break;
        case ErrorHandler::BackslashReplace:
            for (size_t k = 0; k < error.length; ++k)
                output.append("\\x"_s, hex(rest[k], 2, Lowercase));
            break;
        case ErrorHandler::SurrogateEscape: {
            // Each byte that is not ASCII becomes half of a pair, which encoding turns back into the byte.
            size_t escaped = 0;
            while (escaped < error.length && escaped < 4 && rest[escaped] >= 0x80) {
                output.append(static_cast<char16_t>(0xDC00 + rest[escaped]));
                ++escaped;
            }
            if (!escaped)
                return fail();
            error.length = escaped;
            break;
        }
        case ErrorHandler::SurrogatePass: {
            char32_t surrogate = 0;
            size_t size = 0;
            if (unitSize == 1 && codec != Codec::ASCII && rest.size() >= 3 && (rest[0] & 0xF0) == 0xE0 && isContinuation(rest[1]) && isContinuation(rest[2])) {
                surrogate = ((rest[0] & 0x0F) << 12) | ((rest[1] & 0x3F) << 6) | (rest[2] & 0x3F);
                size = 3;
            } else if (unitSize > 1 && rest.size() >= unitSize) {
                surrogate = readUnit(position);
                size = unitSize;
            }
            if (!size || !U_IS_SURROGATE(surrogate))
                return fail();
            output.append(static_cast<char16_t>(surrogate));
            error.length = size;
            break;
        }
        case ErrorHandler::Strict:
        case ErrorHandler::XMLCharacterReference:
        case ErrorHandler::NameReplace:
            if (*handler != ErrorHandler::Strict) {
                raiseTypeError(globalObject, scope, "don't know how to handle UnicodeDecodeError in error callback"_s);
                return { };
            }
            return fail();
        }
        position += error.length;
    }
    String result = output.toString();
    return result.isNull() ? emptyString() : result;
}

} } // namespace JSC::Python
