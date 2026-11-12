/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTID.h"
#include "AOTRegExpFlags.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class Facts;
class Program;

struct HeldRegExp {
    StringID pattern;
    OptionSet<RegExpFlags> flags;
    bool isPrivate { false };

    friend bool operator==(const HeldRegExp&, const HeldRegExp&) = default;
};

class HeldRegExps {
public:
    HeldRegExps() = default;
    HeldRegExps(const Program&, const Facts&);

    const HeldRegExp* find(FunctionID, uint32_t capture) const LIFETIME_BOUND;
    const HeldRegExp* find(const FieldReference&) const LIFETIME_BOUND;
    const HeldRegExp* find(VariableID) const LIFETIME_BOUND;

private:
    void findFields(const Program&, const BodyIndex&);
    void findPrivateFields(const Program&, const Facts&);
    void noteStore(const FieldReference&, const BodyIndex&, ValueID stored);
    void findVariables(const Program&, const Facts&);
    void findPrivateLiterals(const Program&, const Facts&, const BitVector& functionsWithLiterals, BitVector&& escaped);

    Vector<HeldRegExp> m_literals;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_firstCaptures;
    Vector<uint32_t> m_literalsOfCaptures;
    HashMap<uint64_t, std::optional<HeldRegExp>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_fields;
    HashMap<uint32_t, std::optional<HeldRegExp>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_variables;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
