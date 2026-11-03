/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <span>
#include <wtf/NotFound.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// Where a long needle is in a haystack, in time linear in their lengths whatever they look like, and in constant space: the
// two-way algorithm (Crochemore and Perrin, "Two-way string-matching", 1991). The plain ways are faster for short needles, and
// take time proportional to the product of the lengths for the likes of 'aaa...ab' in 'aaa...a'.
//
// The needle is cut in two at a place such that a mismatch in the right part, compared from left to right, allows a shift past
// the characters that matched, and one in the left part, compared afterwards, a shift by the period of the needle.
//
// Backward: the last place, found by doing the same to the mirror images.
constexpr size_t shortestNeedleForTwoWaySearch = 24;

template<bool backward, typename HaystackCharacter, typename NeedleCharacter>
size_t twoWaySearch(std::span<const HaystackCharacter> haystackSpan, std::span<const NeedleCharacter> needleSpan)
{
    size_t length = needleSpan.size();
    size_t haystackLength = haystackSpan.size();
    if (length > haystackLength)
        return notFound;
    if (!length)
        return backward ? haystackLength : 0;

    auto needle = [&](size_t i) ALWAYS_INLINE_LAMBDA -> unsigned { return needleSpan[backward ? length - 1 - i : i]; };
    auto haystack = [&](size_t i) ALWAYS_INLINE_LAMBDA -> unsigned { return haystackSpan[backward ? haystackLength - 1 - i : i]; };

    // The cut comes after the start of the greatest suffix, by the order of the characters or by the opposite order, whichever starts later.
    // `start` is the index before the suffix, so -1 for the whole needle.
    auto greatestSuffix = [&](bool opposite, size_t& period) {
        size_t start = static_cast<size_t>(-1);
        size_t candidate = 0;
        size_t matched = 1;
        period = 1;
        while (candidate + matched < length) {
            unsigned a = needle(start + matched);
            unsigned b = needle(candidate + matched);
            if (a == b) {
                if (matched == period) {
                    candidate += period;
                    matched = 1;
                } else
                    ++matched;
            } else if (opposite ? a < b : a > b) {
                candidate += matched;
                matched = 1;
                period = candidate - start;
            } else {
                start = candidate++;
                matched = period = 1;
            }
        }
        return start;
    };
    size_t period, oppositePeriod;
    size_t cut = greatestSuffix(false, period);
    size_t oppositeCut = greatestSuffix(true, oppositePeriod);
    if (oppositeCut + 1 > cut + 1) {
        cut = oppositeCut;
        period = oppositePeriod;
    }
    size_t rightStart = cut + 1;

    // If that is the period of the whole needle, what matched before a shift by it need not be compared again.
    bool isPeriodic = true;
    for (size_t i = 0; i < rightStart && isPeriodic; ++i)
        isPeriodic = needle(i) == needle(i + period);
    size_t rememberedAfterShift = 0;
    if (isPeriodic)
        rememberedAfterShift = length - period;
    else
        period = std::max(rightStart, length - rightStart) + 1;

    size_t remembered = 0;
    for (size_t position = 0; position + length <= haystackLength;) {
        size_t i = std::max(rightStart, remembered);
        while (i < length && needle(i) == haystack(position + i))
            ++i;
        if (i < length) {
            position += i - cut;
            remembered = 0;
            continue;
        }
        i = rightStart;
        while (i > remembered && needle(i - 1) == haystack(position + i - 1))
            --i;
        if (i <= remembered)
            return backward ? haystackLength - length - position : position;
        position += period;
        remembered = rememberedAfterShift;
    }
    return notFound;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
