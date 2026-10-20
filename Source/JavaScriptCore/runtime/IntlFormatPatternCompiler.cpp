/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "IntlFormatPatternCompiler.h"

#include "JSCInlines.h"
#include <wtf/unicode/CharacterNames.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

std::nullptr_t IntlFormatPatternCompiler::decline(PatternDecline reason)
{
    m_decline = reason;
    return nullptr;
}

String IntlFormatPatternCompiler::withNormalSpaces(std::span<const char16_t> characters)
{
    Vector<char16_t, 32> text(characters);
    for (auto& character : text) {
        if (character == narrowNoBreakSpace || character == thinSpace)
            character = space;
    }
    return String(text.span());
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
