/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTTypeTestCoverage.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class StretchOutcome : uint8_t { Unknown, Guarded, NotClonable };

struct Stretch {
    StretchOutcome outcome { StretchOutcome::Unknown };
    unsigned numberOfUses { 0 };
    unsigned numberOfTests { 0 };
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

    const BitVector& beforeRequirement(uint32_t requirement) const LIFETIME_BOUND { return m_beforeRequirement[requirement]; }
    bool isHoistedTestWanted(uint32_t hoistedTest) const { return m_wantedHoistedTests.get(hoistedTest); }
    bool isProofWanted(uint32_t existingProof) const { return m_wantedProofs.get(existingProof); }

    bool isInStretch(ValueID value) const { return m_valuesInStretch.get(value.index()); }
    bool isInStretchAtHead(BlockID block) const { return m_headsInStretch.get(block.index()); }
    bool isTerminatorInStretch(BlockID block) const { return m_terminatorsInStretch.get(block.index()); }

    // Slow paths that come back without a proof that is wanted behind them. They go on in the generic copy.
    std::span<const Edge> edgesIntoGenericCopy() const LIFETIME_BOUND { return m_edgesIntoGenericCopy.span(); }

private:
    enum class Recording : uint8_t { Off, On };

    BitVector wantedAtTail(BlockID, Recording);
    BitVector wantedOn(Edge, Recording);
    void walkBackward(BlockID, BitVector& wanted, Recording);
    void applyEventBackward(BlockID, const TestEvent&, BitVector& wanted, Recording);
    void markValues(BlockID, uint32_t begin, uint32_t end);

    const TestEvents& m_events;
    const TestCoverage& m_coverage;
    Vector<BitVector> m_wantedAtHead;
    Vector<BitVector> m_beforeRequirement;
    BitVector m_wantedHoistedTests;
    BitVector m_wantedProofs;
    BitVector m_valuesInStretch;
    BitVector m_headsInStretch;
    BitVector m_terminatorsInStretch;
    Vector<Edge, 4> m_edgesIntoGenericCopy;
};

class TestStretches {
    WTF_MAKE_NONCOPYABLE(TestStretches);
public:
    TestStretches(const TestEvents&, const TestCoverage&, const WantedProofs&);

    std::span<const Stretch> stretches() const LIFETIME_BOUND { return m_stretches.span(); }
    std::span<const StretchRange> ranges() const LIFETIME_BOUND { return m_ranges.span(); }

    unsigned stretchAt(ProgramPoint) const;
    unsigned stretchAtHeadOf(BlockID) const;

    Vector<Stretch> takeStretches() { return WTF::move(m_stretches); }
    Vector<StretchRange> takeRanges() { return WTF::move(m_ranges); }

private:
    bool startsNewCopy(ProgramPoint) const;
    bool isCopyEnteredAtHeadOf(BlockID) const;
    unsigned firstRangeOf(BlockID) const;
    void forEachNextRange(unsigned range, NOESCAPE const Invocable<void(unsigned)> auto& functor) const;

    void markTestPoints();
    void findRangesIn(BlockID);
    void appendRange(BlockID, uint32_t begin, uint32_t end);
    void joinRanges();
    void markClonedRanges();
    void measureStretches();

    const TestEvents& m_events;
    const TestCoverage& m_coverage;
    const WantedProofs& m_wanted;
    BitVector m_hasTestInFront;
    BitVector m_isPlannedUse;
    BitVector m_blocksWhereCopyIsEntered;
    Vector<StretchRange> m_ranges;
    Vector<std::optional<unsigned>> m_firstRangeOf;
    Vector<Stretch> m_stretches;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
