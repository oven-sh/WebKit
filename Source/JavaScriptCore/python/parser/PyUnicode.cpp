/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyUnicode.h"

#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

bool isXIDStart(char32_t c)
{
    return u_hasBinaryProperty(c, UCHAR_XID_START);
}

bool isXIDContinue(char32_t c)
{
    return u_hasBinaryProperty(c, UCHAR_XID_CONTINUE);
}

bool isPrintable(char32_t c)
{
    if (c == ' ')
        return true;
    switch (u_charType(c)) {
    case U_CONTROL_CHAR:
    case U_FORMAT_CHAR:
    case U_SURROGATE:
    case U_PRIVATE_USE_CHAR:
    case U_UNASSIGNED:
    case U_LINE_SEPARATOR:
    case U_PARAGRAPH_SEPARATOR:
    case U_SPACE_SEPARATOR:
        return false;
    default:
        return true;
    }
}

bool isWhitespace(char32_t c)
{
    switch (c) {
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D:
    case 0x001C: case 0x001D: case 0x001E: case 0x001F: case 0x0020:
    case 0x0085: case 0x00A0: case 0x1680:
    case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004: case 0x2005:
    case 0x2006: case 0x2007: case 0x2008: case 0x2009: case 0x200A:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return false;
    }
}

String normalizeNFKC(const String& string)
{
    if (string.containsOnlyASCII())
        return string;
    UErrorCode status = U_ZERO_ERROR;
    const UNormalizer2* normalizer = unorm2_getNFKCInstance(&status);
    if (U_FAILURE(status))
        return string;
    auto characters = StringView(string).upconvertedCharacters();
    auto source = characters.span();
    if (unorm2_isNormalized(normalizer, source.data(), source.size(), &status) && U_SUCCESS(status))
        return string;
    status = U_ZERO_ERROR;
    Vector<char16_t, 64> buffer(64);
    int32_t length = unorm2_normalize(normalizer, source.data(), source.size(), buffer.mutableSpan().data(), buffer.size(), &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        buffer.grow(length);
        status = U_ZERO_ERROR;
        length = unorm2_normalize(normalizer, source.data(), source.size(), buffer.mutableSpan().data(), buffer.size(), &status);
    }
    if (U_FAILURE(status))
        return string;
    return String(buffer.span().first(length));
}

std::optional<char32_t> codePointFromName(std::span<const uint8_t> name)
{
    Vector<char, 128> buffer;
    buffer.append(byteCast<char>(name));
    buffer.append('\0');
    for (UCharNameChoice choice : { U_UNICODE_CHAR_NAME, U_CHAR_NAME_ALIAS }) {
        UErrorCode status = U_ZERO_ERROR;
        UChar32 result = u_charFromName(choice, buffer.span().data(), &status);
        if (U_SUCCESS(status))
            return static_cast<char32_t>(result);
    }
    return std::nullopt;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
