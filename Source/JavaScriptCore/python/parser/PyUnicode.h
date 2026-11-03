/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <optional>
#include <span>
#include <wtf/text/WTFString.h>

namespace JSC {
namespace Python {

// Properties of characters as Python defines them. What str says about a character is part of the language, so they are those
// of the version of Unicode of the Python that this is, from tables of our own (tools/make_unicode_tables.py), whichever ICU
// there is. Names and normalization are still those of ICU.

namespace Unicode {

bool isAlpha(char32_t); // The categories Lm, Lt, Lu, Ll and Lo.
bool isLowercase(char32_t);
bool isUppercase(char32_t);
bool isTitlecase(char32_t);
bool isCased(char32_t);
bool isCaseIgnorable(char32_t);
bool isDecimal(char32_t);
bool isDigit(char32_t);
bool isNumeric(char32_t);
bool isLineBreak(char32_t);
int decimalValue(char32_t); // -1: not a decimal digit.

enum class CaseMapping : uint8_t { Lower, Upper, Title, Fold };

// The full mapping of one character, whatever is around it. Returns how many characters that is.
unsigned mapCase(char32_t, CaseMapping, char32_t (&result)[3]);
// The first of them, which is what CPython takes where it needs one.
char32_t mapCaseToOne(char32_t, CaseMapping);

} // namespace Unicode

bool isXIDStart(char32_t);
bool isXIDContinue(char32_t);

// str.isprintable(): everything but the categories "Other" and "Separator", and the ASCII space.
bool isPrintable(char32_t);

// str.isspace(): bidirectional type WS, B or S, or category Zs.
bool isWhitespace(char32_t);

String normalizeNFKC(const String&);

// unicodedata.lookup(), without named sequences: what \N{name} means.
std::optional<char32_t> codePointFromName(std::span<const uint8_t>);
// The same for the characters that ICU does not know yet (PyUnicodeNewNames.h), and the other way round. A null string: it is not one.
std::optional<char32_t> newCodePointFromName(std::span<const uint8_t>);
String nameOfNewCodePoint(char32_t);

} // namespace Python
} // namespace JSC
