/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include <array>
#include <optional>
#include <span>
#include "AOTBitVector.h"

namespace JSC { namespace AOT {

using Typed::PropertyKeyID;

struct StringUses {
    BitVector isNamed;
    BitVector isNamedBesideCreation;
    BitVector isConstant;
    BitVector isCreatedRepeatedly;
    BitVector isConstantInTables;
    BitVector isAtom;
    Vector<Vector<uint32_t>> namedByFunction;
    Vector<uint32_t> functionsInSourceOrder;
};

struct KeyUse {
    uint32_t uses { 0 };
    uint32_t tableLookupUses { 0 };
    // Also counts code that runs once, which asks the table of fields and files nothing.
    uint32_t lookupsByName { 0 };
    FunctionID firstTableLookupFunction;
    std::array<uint32_t, numberOfEnumerators<Typed::Access>> usesByAccess { };
};

struct KeyUses {
    Vector<KeyUse> byKey;
    BitVector keysNamedByTables;
    BitVector stringsNamedAsKeysByTables;
};

struct ProgramUses {
    StringUses strings;
    KeyUses keys;
};

ProgramUses collectProgramUses(const Typed::PipelineOutput&, const BitVector& bodiesWithoutCode, BuildContext&);

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

private:
    friend StringConstantNumbering numberStringConstants(const Typed::Program&, const StringUses&, BuildContext&);

    Vector<uint32_t> m_numbersPlusOne;
    Vector<StringID> m_strings;
};

StringConstantNumbering numberStringConstants(const Typed::Program&, const StringUses&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
