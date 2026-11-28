/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRegExpMatchPlan.h"
#include "AOTVector.h"

namespace JSC { namespace AOT {

struct Latin1Range {
    unsigned first;
    unsigned last;
};

inline constexpr int32_t caseBitOfLatin1 = 0x20;

struct Latin1Test {
    static constexpr unsigned maxRangesCompared = 2;
    using Ranges = Vector<Latin1Range, maxRangesCompared + 1>;

    static Latin1Test of(const Latin1Set&);

    Ranges ranges;
    bool isFolded { false };
    bool isInverted { false };
    bool usesTable { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
