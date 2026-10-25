/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStringUses.h"

namespace JSC { namespace AOT {

struct NumberedIdentifier {
    PropertyKeyID key;
    StringID text;
};

class IdentifierNumbering {
public:
    std::optional<uint32_t> numberOf(PropertyKeyID key) const { return numberIn(m_numbersByKey, key); }
    std::optional<uint32_t> numberOfText(StringID text) const { return numberIn(m_numbersByText, text); }
    const NumberedIdentifier& at(uint32_t number) const
    {
        RELEASE_ASSERT(number < count());
        return m_identifiers[number];
    }

    uint32_t count() const { return m_identifiers.size(); }
    uint32_t countWithLookupTableColumn() const { return m_countWithLookupTableColumn; }

private:
    friend IdentifierNumbering numberIdentifiers(const Typed::Program&, const KeyUses&, const BitVector& keysWithColumn, BuildContext&);

    Vector<uint32_t> m_numbersByKey;
    Vector<uint32_t> m_numbersByText;
    Vector<NumberedIdentifier> m_identifiers { NumberedIdentifier { } };
    uint32_t m_countWithLookupTableColumn { 1 };
};

IdentifierNumbering numberIdentifiers(const Typed::Program&, const KeyUses&, const BitVector& keysWithColumn, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
