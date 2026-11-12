/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

using Typed::PropertyKeyID;
using Typed::StringID;

template<typename ID>
std::optional<uint32_t> numberIn(const Vector<uint32_t>& numbers, ID id)
{
    if (!id || id.index() >= numbers.size() || !numbers[id.index()])
        return std::nullopt;
    return numbers[id.index()];
}

class StringConstantNumbering {
public:
    std::optional<uint32_t> numberOf(StringID string) const
    {
        std::optional<uint32_t> numberPlusOne = numberIn(m_numbersPlusOne, string);
        return numberPlusOne ? std::optional(*numberPlusOne - 1) : std::nullopt;
    }
    StringID stringOf(uint32_t number) const { return m_strings[number]; }
    uint32_t count() const { return m_strings.size(); }
    std::span<const StringID> strings() const { return m_strings.span(); }
    uint32_t numberOfPlaced() const { return m_numberOfPlaced; }
    bool isPlaced(StringID string) const
    {
        std::optional<uint32_t> number = numberOf(string);
        return number && *number < m_numberOfPlaced;
    }

private:
    friend struct ImageNumbersBuilder;

    Vector<uint32_t> m_numbersPlusOne;
    Vector<StringID> m_strings;
    uint32_t m_numberOfPlaced { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
