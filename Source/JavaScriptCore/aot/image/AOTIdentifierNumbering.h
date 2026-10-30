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
    // The table of fields goes on past the columns of the front table, up to the last name that code looks up.
    uint32_t countWithFieldTableColumn() const { return m_countWithFieldTableColumn; }
    bool hasFieldTableColumn(uint32_t number) const { return number && number < m_countWithFieldTableColumn; }

private:
    friend IdentifierNumbering numberIdentifiers(const Typed::Program&, const KeyUses&, const BitVector& keysWithColumn, BuildContext&);

    Vector<uint32_t> m_numbersByKey;
    Vector<uint32_t> m_numbersByText;
    Vector<NumberedIdentifier> m_identifiers { NumberedIdentifier { } };
    uint32_t m_countWithLookupTableColumn { 1 };
    uint32_t m_countWithFieldTableColumn { 1 };
};

IdentifierNumbering numberIdentifiers(const Typed::Program&, const KeyUses&, const BitVector& keysWithColumn, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
