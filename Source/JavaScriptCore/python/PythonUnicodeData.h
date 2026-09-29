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

#include <optional>
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace Python { namespace Unicode {

// What the module unicodedata knows, for whatever else wants it: the lexer, the codecs, and what shows an exception. It is CPython's own tables that are looked in, and so it is CPython's version of Unicode that is gone by. See
// PythonUnicodeType.h for what a str goes by.

enum class NormalizationForm : uint8_t { NFD, NFKD, NFC, NFKC };

// Whether it can be told without working it out that some characters are in a form already.
bool isCertainlyNormalized(NormalizationForm, std::span<const char32_t>);
// unicodedata.normalize(). False if there is no room.
bool tryNormalize(NormalizationForm, std::span<const char32_t>, Vector<char32_t>& result);

// Whether unicodedata.east_asian_width() is "F" or "W": it takes two columns of a terminal.
bool isWide(char32_t);

// What _ucnhash_CAPI is for in CPython.
// getname(). Null if it has none. CPython keeps aliases, and the names of sequences, under characters that are for private use, and `withAliasesAndSequences` is whether those characters are to have those names.
String nameOfCharacter(char32_t, bool withAliasesAndSequences = false);
// getcode(), with no sequences: by its name, or by an alias, in capitals or not.
std::optional<char32_t> characterNamed(std::span<const uint8_t> name);

} } } // namespace JSC::Python::Unicode
