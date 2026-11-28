/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTRegExpCompiler.h"
#include "AOTVector.h"
#include <memory>

namespace JSC {

namespace Yarr {
struct YarrPattern;
}

namespace AOT {

struct BitParallelRegExp {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(BitParallelRegExp);

    struct Group {
        unsigned group { 0 };
        unsigned alternative { 0 };
        unsigned first { 0 };
        unsigned last { 0 };
        bool includesRepeats { false };
    };

    unsigned numberOfStates() const { return length * numberOfAlternatives; }

    char32_t firstCodePoint { 0 };
    Vector<uint64_t> states;
    unsigned length { 0 };
    unsigned numberOfAlternatives { 0 };
    unsigned maxRepeats { 0 };
    unsigned numberOfGroups { 0 };
    Vector<Group> groups;
};

std::unique_ptr<const BitParallelRegExp> planBitParallelRegExp(const Yarr::YarrPattern&);

void emitBitParallelRegExp(CCallHelpers&, ImageCodeAddressing&, const BitParallelRegExp&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
