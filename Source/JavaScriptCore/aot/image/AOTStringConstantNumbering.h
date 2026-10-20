/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTStringUses.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class StringConstantNumbering {
public:
    StringConstantNumbering() = default;

    std::optional<uint32_t> numberOf(StringID) const;
    StringID stringOf(uint32_t number) const { return m_strings[number]; }
    uint32_t count() const { return m_strings.size(); }
    std::span<const StringID> strings() const { return m_strings.span(); }

private:
    friend StringConstantNumbering numberStringConstants(const Typed::Program&, const StringUses&, BuildContext&);

    Vector<uint32_t> m_numbersPlusOne;
    Vector<StringID> m_strings;
};

inline std::optional<uint32_t> StringConstantNumbering::numberOf(StringID string) const
{
    if (!string || string.index() >= m_numbersPlusOne.size() || !m_numbersPlusOne[string.index()])
        return std::nullopt;
    return m_numbersPlusOne[string.index()] - 1;
}

StringConstantNumbering numberStringConstants(const Typed::Program&, const StringUses&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
