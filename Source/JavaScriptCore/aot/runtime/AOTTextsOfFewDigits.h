/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSString.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

ALWAYS_INLINE std::optional<double> valueOfFewDigits(std::span<const Latin1Character> characters)
{
    constexpr size_t maxNumberOfDigits = 15;
    if (characters.empty() || characters.size() > maxNumberOfDigits)
        return std::nullopt;
    uint64_t whole = 0;
    for (Latin1Character character : characters) {
        unsigned digit = character - '0';
        if (digit > 9)
            return std::nullopt;
        whole = whole * 10 + digit;
    }
    return static_cast<double>(whole);
}

ALWAYS_INLINE std::optional<double> valueOfTextOfFewDigits(JSValue value)
{
    if (!value.isString())
        return std::nullopt;
    const StringImpl* impl = asString(value)->tryGetValueImpl();
    if (!impl || !impl->is8Bit())
        return std::nullopt;
    return valueOfFewDigits(impl->span8());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
