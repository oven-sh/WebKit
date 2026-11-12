/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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

    friend bool operator==(const HeldRegExp&, const HeldRegExp&) = default;
};

class HeldRegExps {
public:
    HeldRegExps() = default;
    HeldRegExps(const Program&, const Facts&);

    const HeldRegExp* find(FunctionID, uint32_t capture) const LIFETIME_BOUND;

private:
    Vector<HeldRegExp> m_literals;
    HashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_firstCaptures;
    Vector<uint32_t> m_literalsOfCaptures;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
