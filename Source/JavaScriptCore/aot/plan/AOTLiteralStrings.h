/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTSlotRepresentation.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// The strings that a value in the representation LiteralString names by a number: AOTSlotRepresentation.h, literalStringWord().
// Number 0 is the empty string, which compiled code and the run time write without asking.
// Milestone one: the empty string and no other. A test for any other string hands its StringID to the run time.
class LiteralStringTable {
public:
    LiteralStringTable() = default;
    LiteralStringTable(Vector<StringID>&& strings, Vector<StringID>&& stringsWithoutWord, Vector<uint32_t>&& numbersPlusOne)
        : m_strings(WTF::move(strings))
        , m_stringsWithoutWord(WTF::move(stringsWithoutWord))
        , m_numbersPlusOne(WTF::move(numbersPlusOne))
    {
    }

    std::span<const StringID> strings() const LIFETIME_BOUND { return m_strings.span(); }
    // Image keeps their texts.
    std::span<const StringID> stringsWithoutWord() const LIFETIME_BOUND { return m_stringsWithoutWord.span(); }
    std::optional<uint32_t> indexOf(StringID string) const
    {
        if (!string || string.index() >= m_numbersPlusOne.size() || !m_numbersPlusOne[string.index()])
            return std::nullopt;
        return m_numbersPlusOne[string.index()] - 1;
    }
    uint32_t numberOf(StringID string) const { return indexOf(string).value(); }

private:
    Vector<StringID> m_strings;
    Vector<StringID> m_stringsWithoutWord;
    // By string.
    Vector<uint32_t> m_numbersPlusOne;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
