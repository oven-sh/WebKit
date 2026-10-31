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
#include "AOTBitVector.h"
#include <wtf/Forward.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class Program;

class LiteralStringTable {
public:
    std::span<const StringID> strings() const LIFETIME_BOUND { return m_strings.span(); }
    // Past the limit. Code that tests for one of them hands its StringID to the run time.
    std::span<const StringID> stringsWithoutWord() const LIFETIME_BOUND { return m_stringsWithoutWord.span(); }
    std::optional<uint32_t> indexOf(StringID) const;
    // A member of a type that the program spells nowhere else, and no string from outside is tested for it: no value is that string where the type holds, and the image is without its text.
    bool isLeftOut(StringID string) const { return m_leftOut.get(string.index()); }
    // A member that has a word and that the program spells nowhere. A value is that string only once a string from outside was: the image holds a digest of the text, and the first such string lends its text to the instance.
    bool isWithoutText(StringID string) const { return m_withoutText.get(string.index()); }
    uint32_t numberOf(StringID) const;
    void dump(PrintStream&, const Program&) const;

private:
    friend LiteralStringTable numberLiteralStrings(const Program&, const FlowResults&, BuildContext&);

    Vector<StringID> m_strings;
    Vector<StringID> m_stringsWithoutWord;
    BitVector m_leftOut;
    BitVector m_withoutText;
    UncheckedKeyHashMap<StringID, uint32_t> m_indexes;
};

LiteralStringTable numberLiteralStrings(const Program&, const FlowResults&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
