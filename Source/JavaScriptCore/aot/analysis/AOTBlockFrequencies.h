/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBlockCycles.h"
#include "AOTCFG.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/FrequencyEstimator.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(BlockFrequency::Normal < BlockFrequency::Unlikely && BlockFrequency::Unlikely < BlockFrequency::Rare);

constexpr BlockFrequency rarerOf(BlockFrequency a, BlockFrequency b) { return a < b ? b : a; }
constexpr BlockFrequency moreFrequentOf(BlockFrequency a, BlockFrequency b) { return a < b ? a : b; }
constexpr bool isRarerThan(BlockFrequency a, BlockFrequency b) { return b < a; }

inline constexpr WTF::FrequencyEstimatorConstants frequencyConstants { };

struct FrequencyFacts {
    enum class BranchSide : uint8_t { Unknown, Taken, NotTaken };

    struct BlockFacts {
        bool isNormal() const { return applied == BlockFrequency::Normal && !isInherentlyRare; }
        bool isRare() const { return applied == BlockFrequency::Rare || isInherentlyRare; }

        BlockFrequency recorded { BlockFrequency::Normal };
        BlockFrequency applied { BlockFrequency::Normal };
        bool isInherentlyRare { false };
        bool isNormalExit { false };
        BranchSide unlikelySide { BranchSide::Unknown };
        BranchSide sideOffUsualPath { BranchSide::Unknown };
    };

    struct DecidedPredecessor {
        BlockID block;
        BlockID predecessor;
        uint32_t successorIndex { 0 };

        friend constexpr bool operator==(const DecidedPredecessor&, const DecidedPredecessor&) = default;
    };

    Vector<BlockFacts> blocks;
    Vector<DecidedPredecessor> decidedPredecessors;
};

class BlockFrequencies {
    WTF_MAKE_NONCOPYABLE(BlockFrequencies);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BlockFrequencies);
public:
    BlockFrequencies(const AnalysisContext&, const Function&, CFG&, const BlockCycles&);
    ~BlockFrequencies();

    bool describes(const AnalysisContext&, const Function&) const;

    double frequencyOf(BlockID) const;
    BlockFrequency classOf(BlockID) const;
    BlockFrequency classOf(Edge) const;
    BlockFrequency classForBlockOrder(Edge) const;
    void dump(PrintStream&, const Function&) const;

private:
    const BlockCycles& cycles() const LIFETIME_BOUND { return m_cyclesByLikelihood ? *m_cyclesByLikelihood : m_sharedCycles; }
    void computeClassesOfBlocks() const;
    void estimateCounts() const;

    CFG& m_cfg;
    const BlockCycles& m_sharedCycles;
    std::optional<BlockCycles> m_cyclesByLikelihood;
    FrequencyFacts m_facts;
    Vector<BlockFrequency> m_edges;
    Vector<uint8_t> m_weights;
    mutable Vector<BlockFrequency> m_blocks;
    mutable bool m_hasClassesOfBlocks { false };
    mutable bool m_hasCounts { false };
    mutable Vector<double> m_frequencies;
};

enum class RecordedFrequencyUse : uint8_t { NotRecorded, Ignored, Applied };

RecordedFrequencyUse recordedFrequencyUseOf(const Function&, const CFG&, BlockID, bool ignoresBranchHints);

inline bool isRecordedAsRare(const Function& function, BlockID block)
{
    return block != entryBlock && function.at(block).frequency == BlockFrequency::Rare;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
