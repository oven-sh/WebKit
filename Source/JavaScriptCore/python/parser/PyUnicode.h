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

// Properties of characters as Python defines them.

bool isXIDStart(char32_t);
bool isXIDContinue(char32_t);

// str.isprintable(): everything but the categories "Other" and "Separator", and the ASCII space.
bool isPrintable(char32_t);

// str.isspace(): bidirectional type WS, B or S, or category Zs.
bool isWhitespace(char32_t);

String normalizeNFKC(const String&);

// unicodedata.lookup(), without named sequences: what \N{name} means.
std::optional<char32_t> codePointFromName(std::span<const uint8_t>);

} // namespace Python
} // namespace JSC
