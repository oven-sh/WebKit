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
#include <wtf/Forward.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class Program;

class LiteralStringTable {
public:
    std::span<const StringID> strings() const LIFETIME_BOUND { return m_strings.span(); }
    // Past the limit. Code that tests for one of them hands its StringID to the run time.
    std::span<const StringID> stringsWithoutWord() const LIFETIME_BOUND { return m_stringsWithoutWord.span(); }
    std::optional<uint32_t> indexOf(StringID) const;
    uint32_t numberOf(StringID) const;
    void dump(PrintStream&, const Program&) const;

private:
    friend LiteralStringTable numberLiteralStrings(const Program&, BuildContext&);

    Vector<StringID> m_strings;
    Vector<StringID> m_stringsWithoutWord;
    UncheckedKeyHashMap<StringID, uint32_t> m_indexes;
};

LiteralStringTable numberLiteralStrings(const Program&, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
