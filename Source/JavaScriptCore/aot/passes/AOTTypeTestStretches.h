/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypeTestCoverage.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include "AOTBitVector.h"
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

enum class StretchOutcome : uint8_t { Unknown, Guarded, NotClonable };

struct Stretch {
    StretchOutcome outcome { StretchOutcome::Unknown };
    unsigned numberOfUses { 0 };
    unsigned clonedBytes { 0 };
};

struct StretchRange {
    unsigned stretch { 0 };
    BlockID block;
    uint32_t begin { 0 };
    uint32_t end { 0 };
    bool isCloned { false };
};

class WantedProofs {
    WTF_MAKE_NONCOPYABLE(WantedProofs);
public:
    WantedProofs(const TestEvents&, const TestCoverage&);

    bool isHoistedTestWanted(uint32_t hoistedTest) const { return m_wantedHoistedTests.get(hoistedTest); }
    bool isProofWanted(uint32_t existingProof) const { return m_wantedProofs.get(existingProof); }

    bool isInStretch(ValueID value) const { return m_valuesInStretch.get(value.index()); }
    bool isInStretchAtHead(BlockID block) const { return !m_wantedAtHead[block.index()].isEmpty(); }

    // In front of the tests that stand at the point.
    bool isAnyProofWantedAt(ProgramPoint);

    // What has to hold for a path that joins the edge in front of the tests that stand on it. Such a path does not have what the edge itself proves.
    BitVector wantedInFrontOfTestsOn(Edge);

    // Slow paths that come back without a proof that is wanted behind them. They go on in the generic copy.
    std::span<const Edge> edgesIntoGenericCopy() const LIFETIME_BOUND { return m_edgesIntoGenericCopy.span(); }

private:
    BitVector wantedAtTail(BlockID);
    void walkBackward(BlockID, BitVector& wanted);
    void applyEventBackward(BlockID, const TestEvent&, BitVector& wanted);

    const TestEvents& m_events;
    const TestCoverage& m_coverage;
    // While the answers are written down: behind the solve, in one more walk.
    bool m_isRecording { false };
    Vector<BitVector> m_wantedAtHead;
    Vector<BitVector> m_wantedAtTail;
    BlockID m_blockOfEventFlags;
    BitVector m_isProofWantedInFrontOfEvent;
    BitVector m_wantedHoistedTests;
    BitVector m_wantedProofs;
    BitVector m_valuesInStretch;
    Vector<Edge, 4> m_edgesIntoGenericCopy;
};

class TestStretches {
    WTF_MAKE_NONCOPYABLE(TestStretches);
public:
    TestStretches(const TestEvents&, const TestCoverage&, const WantedProofs&);

    std::span<const StretchRange> ranges() const LIFETIME_BOUND { return m_ranges.span(); }

    unsigned stretchAt(ProgramPoint point) const
    {
        const StretchRange* range = findLastRangeThatBeginsBefore(point.block, point.index + 1);
        RELEASE_ASSERT(range && point.index < range->end);
        return range->stretch;
    }
    unsigned stretchAtHeadOf(BlockID block) const { return m_ranges[firstRangeOf(block)].stretch; }

    // Whether the values on both sides of the point belong to one range.
    bool isInsideRange(ProgramPoint point) const
    {
        const StretchRange* range = findLastRangeThatBeginsBefore(point.block, point.index);
        return range && point.index < range->end;
    }

    Vector<Stretch> takeStretches() { return WTF::move(m_stretches); }
    Vector<StretchRange> takeRanges() { return WTF::move(m_ranges); }

private:
    unsigned firstRangeOf(BlockID block) const
    {
        std::optional<unsigned> first = m_firstRangeOf[block.index()];
        RELEASE_ASSERT(first && !m_ranges[*first].begin);
        return *first;
    }

    const StretchRange* findLastRangeThatBeginsBefore(BlockID block, uint32_t index) const
    {
        std::optional<unsigned> first = m_firstRangeOf[block.index()];
        if (!first)
            return nullptr;
        std::span<const StretchRange> candidates = m_ranges.subspan(*first);
        auto firstLaterRange = std::ranges::partition_point(candidates, [&](const StretchRange& range) {
            return range.block == block && range.begin < index;
        });
        return firstLaterRange != candidates.begin() ? &*(firstLaterRange - 1) : nullptr;
    }

    Vector<StretchRange> m_ranges;
    Vector<std::optional<unsigned>> m_firstRangeOf;
    Vector<Stretch> m_stretches;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
