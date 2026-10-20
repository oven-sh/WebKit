/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBlockCycles.h"
#include "AOTCFG.h"
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/FrequencyEstimator.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(BlockFrequency::Normal < BlockFrequency::Unlikely && BlockFrequency::Unlikely < BlockFrequency::Rare);

constexpr BlockFrequency rarerOf(BlockFrequency a, BlockFrequency b) { return a < b ? b : a; }
constexpr BlockFrequency moreFrequentOf(BlockFrequency a, BlockFrequency b) { return a < b ? a : b; }
constexpr bool isRarerThan(BlockFrequency a, BlockFrequency b) { return b < a; }

inline constexpr WTF::FrequencyEstimatorConstants frequencyConstants { };

constexpr WTF::EdgeClass edgeClassOf(BlockFrequency frequency)
{
    switch (frequency) {
    case BlockFrequency::Normal:
        return WTF::EdgeClass::Normal;
    case BlockFrequency::Unlikely:
        return WTF::EdgeClass::Unlikely;
    case BlockFrequency::Rare:
        return WTF::EdgeClass::Rare;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr BlockFrequency blockFrequencyOf(WTF::EdgeClass edgeClass)
{
    switch (edgeClass) {
    case WTF::EdgeClass::Normal:
        return BlockFrequency::Normal;
    case WTF::EdgeClass::Unlikely:
        return BlockFrequency::Unlikely;
    case WTF::EdgeClass::Rare:
        return BlockFrequency::Rare;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

class BlockFrequencies {
    WTF_MAKE_NONCOPYABLE(BlockFrequencies);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BlockFrequencies);
public:
    BlockFrequencies(const AnalysisContext&, const Function&, CFG&, const BlockCycles&);

    double frequencyOf(BlockID) const;
    BlockFrequency classOf(BlockID) const;
    BlockFrequency classOf(Edge) const;
    void dump(PrintStream&, const Function&) const;

private:
    Vector<double> m_frequencies;
    Vector<BlockFrequency> m_blocks;
    Vector<BlockFrequency> m_edges;
    Vector<uint32_t> m_firstEdge;
    BitVector m_isReachable;
};

enum class RecordedFrequencyUse : uint8_t { NotRecorded, Ignored, Applied };

RecordedFrequencyUse recordedFrequencyUseOf(const Function&, const CFG&, BlockID, bool ignoresBranchHints);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
