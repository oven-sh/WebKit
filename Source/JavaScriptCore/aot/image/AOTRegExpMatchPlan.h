/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <limits>
#include <memory>
#include <optional>
#include <wtf/BitSet.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

namespace Yarr {
struct YarrPattern;
}

namespace AOT {

using Latin1Set = WTF::BitSet<256>;

struct RegExpMatchNode {
    enum class Kind : uint8_t {
        Characters,
        Group,
        StartOfSubject,
        StartOfLine,
        EndOfSubject,
        EndOfLine,
        WordBoundary,
        NoWordBoundary,
        Lookahead,
        NegativeLookahead,
    };
    using Sequence = Vector<RegExpMatchNode>;
    static constexpr unsigned unbounded = std::numeric_limits<unsigned>::max();

    bool isRun() const { return kind == Kind::Characters && minimum != maximum; }

    Kind kind { Kind::Characters };
    bool isGreedy { true };
    bool givesBack { false };
    bool commits { true };
    unsigned minimum { 1 };
    unsigned maximum { 1 };
    unsigned group { 0 };
    unsigned firstInnerGroup { 0 };
    unsigned lastInnerGroup { 0 };
    Latin1Set characters { };
    Vector<Sequence> alternatives { };
    Vector<Latin1Set> beginnings { };
};

struct RegExpMatchPlan {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RegExpMatchPlan);

    using Sequence = RegExpMatchNode::Sequence;

    Vector<Sequence> alternatives;
    Vector<Latin1Set> beginnings;
    bool isSearched { false };
    unsigned numberOfGroups { 0 };
    unsigned cost { 0 };
    unsigned minimumLength { 0 };
    std::optional<unsigned> maximumLengthToEnd;
    std::optional<Latin1Set> firstCharacters;
    bool isPrefix { false };
};

unsigned minimumLengthOf(std::span<const RegExpMatchNode>);

std::unique_ptr<const RegExpMatchPlan> planRegExpMatch(const Yarr::YarrPattern&, bool isSticky);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
