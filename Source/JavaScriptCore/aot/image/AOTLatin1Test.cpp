/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLatin1Test.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT {

static Latin1Test::Ranges rangesOfLatin1Set(const Latin1Set& set, bool isIn)
{
    Latin1Test::Ranges ranges;
    for (unsigned first = set.findBit(0, isIn); first < 256 && ranges.size() <= Latin1Test::maxRangesCompared;) {
        unsigned end = set.findBit(first, !isIn);
        ranges.append({ first, end - 1 });
        first = end < 256 ? set.findBit(end, isIn) : 256;
    }
    return ranges;
}

static bool holdsBothCasesOfLatin1(const Latin1Set& set)
{
    for (unsigned character = 0; character < 256; ++character) {
        if (set.get(character) != set.get(character ^ caseBitOfLatin1))
            return false;
    }
    return true;
}

Latin1Test Latin1Test::of(const Latin1Set& set)
{
    Latin1Test test { .ranges = rangesOfLatin1Set(set, true) };
    if (test.ranges.size() > 1 && holdsBothCasesOfLatin1(set)) {
        Latin1Set folded;
        for (unsigned character = 0; character < 256; ++character) {
            if (character & caseBitOfLatin1 && set.get(character))
                folded.set(character);
        }
        Ranges ranges = rangesOfLatin1Set(folded, true);
        if (ranges.size() == 1)
            return { .ranges = WTF::move(ranges), .isFolded = true };
    }
    auto isCheap = [](const Ranges& ranges) {
        return ranges.size() <= 1 || (ranges.size() == 2 && ranges[0].first == ranges[0].last && ranges[1].first == ranges[1].last);
    };
    if (isCheap(test.ranges))
        return test;
    Ranges others = rangesOfLatin1Set(set, false);
    if (isCheap(others))
        return { .ranges = WTF::move(others), .isInverted = true };
    return { .usesTable = true };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
