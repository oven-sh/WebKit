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

// UTF-7, UTF-8, UTF-16 and UTF-32: Objects/unicodeobject.c and Objects/stringlib/codecs.h of CPython. Where an error begins and ends and what it is called are as there, since that is what a handler is told
// and what a message says.

namespace JSC { namespace Python {

// backslashreplace() and xmlcharrefreplace(), for the encodings whose units are bytes.
void appendBackslashReplacement(ByteVector& out, const CodePoints& characters, size_t start, size_t end)
{
    static constexpr auto digits = "0123456789abcdef"_s;
    for (size_t i = start; i < end; ++i) {
        char32_t c = characters[i];
        unsigned count = c >= 0x10000 ? 8 : c >= 0x100 ? 4 : 2;
        out.append('\\');
        out.append(count == 8 ? 'U' : count == 4 ? 'u' : 'x');
        while (count--)
            out.append(digits[(c >> (4 * count)) & 0xF]);
    }
}

void appendXMLCharacterReferences(ByteVector& out, const CodePoints& characters, size_t start, size_t end)
{
    for (size_t i = start; i < end; ++i) {
        out.appendList({ '&', '#' });
        auto number = String::number(static_cast<unsigned>(characters[i]));
        out.append(number.span8());
        out.append(';');
    }
}

// ---- UTF-7

static bool isBase64(uint32_t c) { return isASCIIAlphanumeric(c) || c == '+' || c == '/'; }
static uint32_t fromBase64(uint32_t c) { return isASCIIUpper(c) ? c - 'A' : isASCIILower(c) ? c - 'a' + 26 : isASCIIDigit(c) ? c - '0' + 52 : c == '+' ? 62 : 63; }
static uint8_t toBase64(uint64_t n) { return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[n & 0x3F]; }
// The only byte of ASCII that does not stand for itself is the + that begins what is in base 64.
static bool decodesDirectly(uint32_t c) { return c <= 127 && c != '+'; }

// 0 is what RFC 2152 calls Set D, 1 is Set O, 2 is white space, and 3 is what has to be in base 64.
static constexpr uint8_t utf7Category[128] = {
    3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 3, 3, 2, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    2, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 3, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 1, 1, 1,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 3, 3,
};

// All that is kept from one call to the next is how far it got, so what ends in the middle of something in base 64 goes back to where that began.
String decodeUTF7(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& errors, size_t* consumed)
{
    if (bytes.empty()) {
        if (consumed)
            *consumed = 0;
        return emptyString();
    }
    size_t originalSize = bytes.size();
    DecodeErrors handler(globalObject, errors, bytes);
    auto& input = handler.input;
    TextWriter writer;
    size_t s = 0;
    size_t start = 0;
    bool inShift = false;
    unsigned shiftOutStart = 0;
    unsigned base64Bits = 0;
    uint64_t base64Buffer = 0;
    char32_t surrogate = 0;

    while (true) {
        while (s < input.size()) {
            ASCIILiteral message;
            uint32_t c = input[s];
            if (inShift) {
                if (isBase64(c)) {
                    base64Buffer = (base64Buffer << 6) | fromBase64(c);
                    base64Bits += 6;
                    ++s;
                    if (base64Bits < 16)
                        continue;
                    // There are enough bits for a code unit.
                    char32_t unit = static_cast<char32_t>(base64Buffer >> (base64Bits - 16));
                    base64Bits -= 16;
                    base64Buffer &= (1 << base64Bits) - 1;
                    if (surrogate) {
                        if (U16_IS_TRAIL(unit)) {
                            writer.append(static_cast<char32_t>(U16_GET_SUPPLEMENTARY(surrogate, unit)));
                            surrogate = 0;
                            continue;
                        }
                        writer.append(surrogate);
                        surrogate = 0;
                    }
                    if (U16_IS_LEAD(unit))
                        surrogate = unit;
                    else
                        writer.append(unit);
                    continue;
                }
                // It is the end of what is in base 64.
                inShift = false;
                if (base64Bits >= 6) {
                    ++s;
                    message = "partial character in shift sequence"_s;
                } else if (base64Bits > 0 && base64Buffer) {
                    ++s;
                    message = "non-zero padding bits in shift sequence"_s;
                } else {
                    if (surrogate && decodesDirectly(c))
                        writer.append(surrogate);
                    surrogate = 0;
                    // A - is used up in ending it, and anything else is kept.
                    if (c == '-')
                        ++s;
                    continue;
                }
            } else if (c == '+') {
                start = s++;
                if (s < input.size() && input[s] == '-') {
                    ++s;
                    writer.append(static_cast<char32_t>('+'));
                    continue;
                }
                if (s < input.size() && !isBase64(input[s])) {
                    ++s;
                    message = "ill-formed sequence"_s;
                } else {
                    inShift = true;
                    surrogate = 0;
                    shiftOutStart = writer.position();
                    base64Bits = 0;
                    base64Buffer = 0;
                    continue;
                }
            } else if (decodesDirectly(c)) {
                ++s;
                writer.append(static_cast<char32_t>(c));
                continue;
            } else {
                start = s++;
                message = "unexpected special character"_s;
            }
            if (!handler.handle("utf7"_s, message, start, s, s, writer))
                return { };
        }
        if (!inShift || consumed)
            break;
        // It ended in the middle, and there is no more to come.
        inShift = false;
        if (!surrogate && base64Bits < 6 && !(base64Bits > 0 && base64Buffer))
            break;
        if (!handler.handle("utf7"_s, "unterminated shift sequence"_s, start, originalSize, s, writer))
            return { };
        if (s >= input.size())
            break;
    }
    if (consumed) {
        if (inShift) {
            *consumed = start;
            writer.goBackTo(shiftOutStart);
        } else
            *consumed = s;
    }
    return writer.finish(globalObject);
}

std::optional<ByteVector> encodeUTF7(JSGlobalObject* globalObject, JSValue string)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String text = textOfString(globalObject, string);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    CodePoints characters(text);
    ByteVector out;
    bool inShift = false;
    unsigned base64Bits = 0;
    uint64_t base64Buffer = 0;
    // Set O and white space are written as they are.
    auto encodesDirectly = [] (char32_t c) { return c < 128 && c > 0 && utf7Category[c] != 3; };
    auto drain = [&] {
        while (base64Bits >= 6) {
            out.append(toBase64(base64Buffer >> (base64Bits - 6)));
            base64Bits -= 6;
        }
    };
    for (size_t i = 0; i < characters.size(); ++i) {
        char32_t c = characters[i];
        if (inShift) {
            if (encodesDirectly(c)) {
                if (base64Bits) {
                    out.append(toBase64(base64Buffer << (6 - base64Bits)));
                    base64Buffer = 0;
                    base64Bits = 0;
                }
                inShift = false;
                // What is not of base 64 ends it by being there, so it takes a - only before what is, or before a -.
                if (isBase64(c) || c == '-')
                    out.append('-');
                out.append(static_cast<uint8_t>(c));
                continue;
            }
        } else if (c == '+') {
            out.appendList({ '+', '-' });
            continue;
        } else if (encodesDirectly(c)) {
            out.append(static_cast<uint8_t>(c));
            continue;
        } else {
            out.append('+');
            inShift = true;
        }
        if (c >= 0x10000) {
            base64Bits += 16;
            base64Buffer = (base64Buffer << 16) | U16_LEAD(c);
            drain();
            c = U16_TRAIL(c);
        }
        base64Bits += 16;
        base64Buffer = (base64Buffer << 16) | c;
        drain();
    }
    if (base64Bits)
        out.append(toBase64(base64Buffer << (6 - base64Bits)));
    if (inShift)
        out.append('-');
    return out;
}

// ---- UTF-8

static bool isContinuationByte(uint32_t c) { return c >= 0x80 && c < 0xC0; }

// utf8_decode() of stringlib: decodes for as long as it can. What it returns is 0 at the end, or where what is left may be the beginning of a character, 1 for a byte that cannot begin one, and 2, 3 or 4 for a
// second, third or fourth byte that does not go on from what is before it.
static unsigned decodeUTF8Run(std::span<const uint8_t> input, size_t& position, TextWriter& writer)
{
    size_t s = position;
    size_t end = input.size();
    unsigned result = 0;
    while (s < end) {
        uint32_t c = input[s];
        if (c < 0x80) {
            ++s;
            writer.append(static_cast<char32_t>(c));
            continue;
        }
        size_t left = end - s;
        if (c < 0xE0) {
            if (c < 0xC2) {
                result = 1;
                break;
            }
            if (left < 2)
                break;
            uint32_t c2 = input[s + 1];
            if (!isContinuationByte(c2)) {
                result = 2;
                break;
            }
            writer.append(static_cast<char32_t>((c << 6) + c2 - ((0xC0 << 6) + 0x80)));
            s += 2;
            continue;
        }
        if (c < 0xF0) {
            if (left < 2)
                break;
            uint32_t c2 = input[s + 1];
            // What would be shorter written otherwise, and what would be a surrogate.
            if (!isContinuationByte(c2) || (c2 < 0xA0 ? c == 0xE0 : c == 0xED)) {
                result = 2;
                break;
            }
            if (left < 3)
                break;
            uint32_t c3 = input[s + 2];
            if (!isContinuationByte(c3)) {
                result = 3;
                break;
            }
            writer.append(static_cast<char32_t>((c << 12) + (c2 << 6) + c3 - ((0xE0 << 12) + (0x80 << 6) + 0x80)));
            s += 3;
            continue;
        }
        if (c < 0xF5) {
            if (left < 2)
                break;
            uint32_t c2 = input[s + 1];
            // What would be shorter written otherwise, and what is past the last character there is.
            if (!isContinuationByte(c2) || (c2 < 0x90 ? c == 0xF0 : c == 0xF4)) {
                result = 2;
                break;
            }
            if (left < 3)
                break;
            uint32_t c3 = input[s + 2];
            if (!isContinuationByte(c3)) {
                result = 3;
                break;
            }
            if (left < 4)
                break;
            uint32_t c4 = input[s + 3];
            if (!isContinuationByte(c4)) {
                result = 4;
                break;
            }
            writer.append(static_cast<char32_t>((c << 18) + (c2 << 12) + (c3 << 6) + c4 - ((0xF0 << 18) + (0x80 << 12) + (0x80 << 6) + 0x80)));
            s += 4;
            continue;
        }
        result = 1;
        break;
    }
    position = s;
    return result;
}

String decodeUTF8(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& errors, size_t* consumed)
{
    if (bytes.empty()) {
        if (consumed)
            *consumed = 0;
        return emptyString();
    }
    if (charactersAreAllASCII(bytes)) {
        if (consumed)
            *consumed = bytes.size();
        return decodeLatin1(globalObject, bytes);
    }
    DecodeErrors handler(globalObject, errors, bytes);
    auto& input = handler.input;
    TextWriter writer;
    ErrorHandler known = ErrorHandler::Unknown;
    size_t s = 0;
    while (s < input.size()) {
        unsigned problem = decodeUTF8Run(input, s, writer);
        ASCIILiteral message;
        size_t end;
        if (!problem) {
            if (s == input.size() || consumed)
                break;
            message = "unexpected end of data"_s;
            end = input.size();
        } else if (problem == 1) {
            message = "invalid start byte"_s;
            end = s + 1;
        } else {
            // It may be the beginning of a surrogate, which some handlers will have.
            if (problem == 2 && consumed && input[s] == 0xED && input.size() - s == 2 && input[s + 1] >= 0xA0 && input[s + 1] <= 0xBF)
                break;
            message = "invalid continuation byte"_s;
            end = s + problem - 1;
        }
        if (known == ErrorHandler::Unknown)
            known = errorHandlerNamed(errors);
        switch (known) {
        case ErrorHandler::Ignore:
            s = end;
            break;
        case ErrorHandler::Replace:
            writer.append(static_cast<char32_t>(0xFFFD));
            s = end;
            break;
        case ErrorHandler::SurrogateEscape:
            for (size_t i = s; i < end; ++i)
                writer.append(static_cast<char32_t>(0xDC00 + input[i]));
            s = end;
            break;
        default:
            if (!handler.handle("utf-8"_s, message, s, end, s, writer))
                return { };
        }
    }
    if (consumed)
        *consumed = s;
    return writer.finish(globalObject);
}

std::optional<ByteVector> encodeUTF8(JSGlobalObject* globalObject, JSValue string, const String& errors)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String text = textOfString(globalObject, string);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    ByteVector out;
    if (text.is8Bit() && charactersAreAllASCII(text.span8())) {
        out.append(text.span8());
        return out;
    }
    CodePoints characters(text);
    size_t size = characters.size();
    EncodeErrors handler(globalObject, errors, string, size);
    ErrorHandler known = ErrorHandler::Unknown;
    constexpr auto reason = "surrogates not allowed"_s;
    for (size_t i = 0; i < size;) {
        char32_t c = characters[i++];
        if (c < 0x80) {
            out.append(static_cast<uint8_t>(c));
            continue;
        }
        if (c < 0x800) {
            out.appendList({ static_cast<uint8_t>(0xC0 | (c >> 6)), static_cast<uint8_t>(0x80 | (c & 0x3F)) });
            continue;
        }
        if (!U_IS_SURROGATE(c)) {
            if (c < 0x10000)
                out.appendList({ static_cast<uint8_t>(0xE0 | (c >> 12)), static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F)), static_cast<uint8_t>(0x80 | (c & 0x3F)) });
            else
                out.appendList({ static_cast<uint8_t>(0xF0 | (c >> 18)), static_cast<uint8_t>(0x80 | ((c >> 12) & 0x3F)), static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F)), static_cast<uint8_t>(0x80 | (c & 0x3F)) });
            continue;
        }
        if (known == ErrorHandler::Unknown)
            known = errorHandlerNamed(errors);
        size_t start = i - 1;
        size_t end = start + 1;
        while (end < size && U_IS_SURROGATE(characters[end]))
            ++end;
        bool isHandled = true;
        switch (known) {
        case ErrorHandler::Replace:
            out.appendFill('?', end - start);
            break;
        case ErrorHandler::Ignore:
            break;
        case ErrorHandler::SurrogatePass:
            for (size_t k = start; k < end; ++k) {
                c = characters[k];
                out.appendList({ static_cast<uint8_t>(0xE0 | (c >> 12)), static_cast<uint8_t>(0x80 | ((c >> 6) & 0x3F)), static_cast<uint8_t>(0x80 | (c & 0x3F)) });
            }
            break;
        case ErrorHandler::BackslashReplace:
            appendBackslashReplacement(out, characters, start, end);
            break;
        case ErrorHandler::XMLCharRefReplace:
            appendXMLCharacterReferences(out, characters, start, end);
            break;
        case ErrorHandler::SurrogateEscape: {
            size_t k = start;
            for (; k < end; ++k) {
                c = characters[k];
                if (c < 0xDC80 || c > 0xDCFF)
                    break;
                out.append(static_cast<uint8_t>(c & 0xFF));
            }
            if (k < end) {
                start = k;
                isHandled = false;
            }
            break;
        }
        default:
            isHandled = false;
        }
        if (isHandled) {
            i = end;
            continue;
        }
        size_t newPosition;
        JSValue replacement = handler.handle("utf-8"_s, reason, start, end, newPosition);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (JSString* replacementString = stringIn(replacement)) {
            String replacementText = replacementString->value(globalObject);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (!replacementText.containsOnlyASCII()) {
                handler.raise("utf-8"_s, reason, start, end);
                return std::nullopt;
            }
            for (unsigned k = 0; k < replacementText.length(); ++k)
                out.append(static_cast<uint8_t>(replacementText[k]));
        } else
            out.append(*builtinBufferOf(replacement));
        i = newPosition;
    }
    return out;
}

// ---- UTF-16 and UTF-32

static void appendUnit(ByteVector& out, uint32_t unit, unsigned size, bool isLittleEndian)
{
    for (unsigned i = 0; i < size; ++i)
        out.append(static_cast<uint8_t>(unit >> (8 * (isLittleEndian ? i : size - 1 - i))));
}

String decodeUTF32(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& errors, int* byteOrder, size_t* consumed)
{
    size_t q = 0;
    int order = byteOrder ? *byteOrder : 0;
    // A mark at the beginning says which, and is left out. If it has been said already, the mark is a character like any other.
    if (!order && bytes.size() >= 4) {
        uint32_t mark = (static_cast<uint32_t>(bytes[3]) << 24) | (bytes[2] << 16) | (bytes[1] << 8) | bytes[0];
        if (mark == 0x0000FEFF) {
            order = -1;
            q += 4;
        } else if (mark == 0xFFFE0000) {
            order = 1;
            q += 4;
        }
        if (byteOrder)
            *byteOrder = order;
    }
    if (q == bytes.size()) {
        if (consumed)
            *consumed = bytes.size();
        return emptyString();
    }
    bool isLittleEndian = order <= 0;
    ASCIILiteral encoding = isLittleEndian ? "utf-32-le"_s : "utf-32-be"_s;
    DecodeErrors handler(globalObject, errors, bytes);
    auto& input = handler.input;
    TextWriter writer;
    while (true) {
        ASCIILiteral message;
        size_t end;
        if (input.size() - q < 4) {
            if (q == input.size() || consumed)
                break;
            message = "truncated data"_s;
            end = input.size();
        } else {
            auto p = input.subspan(q);
            uint32_t c = isLittleEndian ? (static_cast<uint32_t>(p[3]) << 24) | (p[2] << 16) | (p[1] << 8) | p[0] : (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            if (!U_IS_SURROGATE(c) && c < 0x110000) {
                writer.append(static_cast<char32_t>(c));
                q += 4;
                continue;
            }
            message = U_IS_SURROGATE(c) ? "code point in surrogate code point range(0xd800, 0xe000)"_s : "code point not in range(0x110000)"_s;
            end = q + 4;
        }
        if (!handler.handle(encoding, message, q, end, q, writer))
            return { };
    }
    if (consumed)
        *consumed = q;
    return writer.finish(globalObject);
}

String decodeUTF16(JSGlobalObject* globalObject, std::span<const uint8_t> bytes, const String& errors, int* byteOrder, size_t* consumed)
{
    size_t q = 0;
    int order = byteOrder ? *byteOrder : 0;
    if (!order && bytes.size() >= 2) {
        uint32_t mark = (bytes[1] << 8) | bytes[0];
        if (mark == 0xFEFF) {
            q += 2;
            order = -1;
        } else if (mark == 0xFFFE) {
            q += 2;
            order = 1;
        }
        if (byteOrder)
            *byteOrder = order;
    }
    if (q == bytes.size()) {
        if (consumed)
            *consumed = bytes.size();
        return emptyString();
    }
    bool isLittleEndian = order <= 0;
    ASCIILiteral encoding = isLittleEndian ? "utf-16-le"_s : "utf-16-be"_s;
    DecodeErrors handler(globalObject, errors, bytes);
    auto& input = handler.input;
    TextWriter writer;
    auto unitAt = [&] (size_t i) -> uint32_t { return isLittleEndian ? (input[i + 1] << 8) | input[i] : (input[i] << 8) | input[i + 1]; };
    while (true) {
        ASCIILiteral message;
        size_t start;
        size_t end;
        if (input.size() - q < 2) {
            if (q == input.size() || consumed)
                break;
            message = "truncated data"_s;
            start = q;
            end = input.size();
        } else {
            uint32_t c = unitAt(q);
            if (!U_IS_SURROGATE(c)) {
                writer.append(static_cast<char32_t>(c));
                q += 2;
                continue;
            }
            start = q;
            if (!U16_IS_LEAD(c)) {
                message = "illegal encoding"_s;
                end = q + 2;
            } else if (input.size() - q < 4) {
                if (consumed)
                    break;
                message = "unexpected end of data"_s;
                end = input.size();
            } else {
                uint32_t c2 = unitAt(q + 2);
                if (U16_IS_TRAIL(c2)) {
                    writer.append(static_cast<char32_t>(U16_GET_SUPPLEMENTARY(c, c2)));
                    q += 4;
                    continue;
                }
                message = "illegal UTF-16 surrogate"_s;
                end = q + 2;
            }
        }
        if (!handler.handle(encoding, message, start, end, q, writer))
            return { };
    }
    if (consumed)
        *consumed = q;
    return writer.finish(globalObject);
}

// _PyUnicode_EncodeUTF16() and _PyUnicode_EncodeUTF32(), which differ in how wide a unit is.
static std::optional<ByteVector> encodeInUnits(JSGlobalObject* globalObject, JSValue string, const String& errors, int byteOrder, unsigned unitSize)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    String text = textOfString(globalObject, string);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    CodePoints characters(text);
    size_t size = characters.size();
    bool isLittleEndian = byteOrder <= 0;
    bool is16 = unitSize == 2;
    // What it is called goes by the sign for the one, and for the other only -1 and 1 will do.
    ASCIILiteral encoding = is16 ? (byteOrder < 0 ? "utf-16-le"_s : byteOrder > 0 ? "utf-16-be"_s : "utf-16"_s) : (byteOrder == -1 ? "utf-32-le"_s : byteOrder == 1 ? "utf-32-be"_s : "utf-32"_s);
    constexpr auto reason = "surrogates not allowed"_s;
    ByteVector out;
    if (!byteOrder)
        appendUnit(out, 0xFEFF, unitSize, isLittleEndian);
    EncodeErrors handler(globalObject, errors, string, size);
    for (size_t position = 0; position < size;) {
        char32_t c = characters[position];
        if (!U_IS_SURROGATE(c)) {
            if (is16 && c >= 0x10000) {
                appendUnit(out, U16_LEAD(c), 2, isLittleEndian);
                appendUnit(out, U16_TRAIL(c), 2, isLittleEndian);
            } else
                appendUnit(out, c, unitSize, isLittleEndian);
            ++position;
            continue;
        }
        size_t newPosition;
        JSValue replacement = handler.handle(encoding, reason, position, position + 1, newPosition);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (JSString* replacementString = stringIn(replacement)) {
            String replacementText = replacementString->value(globalObject);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (!replacementText.containsOnlyASCII()) {
                handler.raise(encoding, reason, position, position + 1);
                return std::nullopt;
            }
            for (unsigned k = 0; k < replacementText.length(); ++k)
                appendUnit(out, replacementText[k], unitSize, isLittleEndian);
        } else {
            auto bytes = *builtinBufferOf(replacement);
            if (bytes.size() & (unitSize - 1)) {
                handler.raise(encoding, reason, position, position + 1);
                return std::nullopt;
            }
            out.append(bytes);
        }
        position = newPosition;
    }
    return out;
}

std::optional<ByteVector> encodeUTF16(JSGlobalObject* globalObject, JSValue string, const String& errors, int byteOrder) { return encodeInUnits(globalObject, string, errors, byteOrder, 2); }
std::optional<ByteVector> encodeUTF32(JSGlobalObject* globalObject, JSValue string, const String& errors, int byteOrder) { return encodeInUnits(globalObject, string, errors, byteOrder, 4); }

} } // namespace JSC::Python
