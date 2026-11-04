/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/StackCheck.h>

namespace JSC { namespace Yarr {

struct PatternDisjunction;
struct YarrPattern;

// Bounds the vectors built to compare the alternatives of one group. More alternatives: Unknown.
static constexpr unsigned maxAlternativesForDeterminism = 2048;
// Up to this many alternatives every pair is compared. Above it, pairs come from a sweep over first-set ranges.
static constexpr unsigned maxAlternativesComparedPairwise = 16;
// Bounds the ranges swept and the pairs compared by that sweep. More: Unknown.
static constexpr unsigned maxSpansSweptForDeterminism = 8192;
// Bounds the characters of two alternatives compared position by position, and the longest counted atom treated as fixed text.
static constexpr unsigned maxFixedPrefixCompared = 32;
// Bounds the recursion from a backreference back to its lookahead. Deeper nesting: the backreference compares.
static constexpr unsigned maxGroupsCrossedToRepeatedLookahead = 8;

enum class AlternativeDeterminism : uint8_t {
    // After a failed continuation a later alternative may have to be tried.
    Unknown,
    // Once an alternative has matched, no other alternative can lead to a match from the same position.
    Deterministic,
    // Deterministic after sortAlternativesLongestFirst(), and that order gives the results of the source order.
    DeterministicIfLongestFirst,
};

void analyzeBacktracking(YarrPattern&, StackCheck&);

// Every alternative must be fixed text. The sort is stable, and the new last alternative gets m_isLastAlternative.
void sortAlternativesLongestFirst(PatternDisjunction&);

} } // namespace JSC::Yarr
