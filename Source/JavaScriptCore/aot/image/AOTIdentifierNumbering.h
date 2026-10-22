/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTStringUses.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct NumberedIdentifier {
    PropertyKeyID key;
    StringID text;
};

class IdentifierNumbering {
public:
    IdentifierNumbering() = default;

    std::optional<uint32_t> numberOf(PropertyKeyID) const;
    std::optional<uint32_t> numberOfText(StringID) const;
    const NumberedIdentifier& at(uint32_t number) const;

    uint32_t count() const { return m_identifiers.size(); }
    uint32_t countWithLookupTableColumn() const { return m_countWithLookupTableColumn; }

private:
    friend IdentifierNumbering numberIdentifiers(const Typed::Program&, const KeyUses&, const BitVector& keysWithColumn, BuildContext&);

    Vector<uint32_t> m_numbersByKey;
    Vector<uint32_t> m_numbersByText;
    Vector<NumberedIdentifier> m_identifiers { NumberedIdentifier { } };
    uint32_t m_countWithLookupTableColumn { 1 };
};

// The number of a name with a column in the lookup table is its column, so these come first, by use. The others are numbered in the order of the program's text, behind the symbols
// that the run time creates in one step. Most modules and functions of a large program never run, and what the image holds for their names then shares no page with what is read.
IdentifierNumbering numberIdentifiers(const Typed::Program&, const KeyUses&, const BitVector& keysWithColumn, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
