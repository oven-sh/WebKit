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


#pragma once

#include <array>
#include <cstdint>

// What kind of thing each character is, and what it is in another case: Objects/unicodectype.c of CPython, over the same table. It is what str's methods, the lexer, int() and float(), repr() and regular expressions go
// by. It is not asked of ICU, which knows whatever version of Unicode came with the system, and has its own idea of some of it.

namespace JSC { namespace Python { namespace Unicode {

bool isAlpha(char32_t); // Ll, Lu, Lt, Lo or Lm
bool isDecimalDigit(char32_t);
bool isDigit(char32_t);
bool isNumeric(char32_t);
bool isLowercase(char32_t); // Has the property Lowercase
bool isUppercase(char32_t); // Has the property Uppercase
bool isTitlecase(char32_t); // Lt
bool isCased(char32_t);
bool isCaseIgnorable(char32_t);
bool isPrintable(char32_t); // Whether repr() shows it as it is.
bool isXIDStart(char32_t);
bool isXIDContinue(char32_t);
bool isWhitespace(char32_t);
bool isLineBreak(char32_t);
inline bool isAlphanumeric(char32_t c) { return isAlpha(c) || isDecimalDigit(c) || isDigit(c) || isNumeric(c); } // Py_UNICODE_ISALNUM()

int toDecimalDigit(char32_t); // 0 to 9. -1 if it is none.
int toDigit(char32_t); // Likewise
double toNumeric(char32_t); // -1.0 if it stands for no number.

// One character for one. Where the whole of it is more than one, it is the first of them.
char32_t toLowercase(char32_t);
char32_t toUppercase(char32_t);
char32_t toTitlecase(char32_t);

// The whole of it, which is at most three characters. Each returns how many.
using Mapping = std::array<char32_t, 3>;
unsigned toLowerFull(char32_t, Mapping&);
unsigned toUpperFull(char32_t, Mapping&);
unsigned toTitleFull(char32_t, Mapping&);
unsigned toFoldedFull(char32_t, Mapping&);

} } } // namespace JSC::Python::Unicode
