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
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/FrequencyEstimator.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(BlockFrequency::Normal < BlockFrequency::Unlikely && BlockFrequency::Unlikely < BlockFrequency::Rare);

constexpr Expected<BlockFrequency> rarerOf(Expected<BlockFrequency> a, Expected<BlockFrequency> b) { return a.predicted() < b.predicted() ? b : a; }
constexpr Expected<BlockFrequency> moreFrequentOf(Expected<BlockFrequency> a, Expected<BlockFrequency> b) { return a.predicted() < b.predicted() ? a : b; }
constexpr Expected<bool> isRarerThan(Expected<BlockFrequency> a, Expected<BlockFrequency> b) { return b < a; }

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

    Expected<double> frequencyOf(BlockID) const;
    Expected<BlockFrequency> classOf(BlockID) const;
    Expected<BlockFrequency> classOf(Edge) const;
    Expected<BlockFrequency> classForBlockOrder(Edge) const;
    // Whichever way it goes on from the block, it ends in a throw, and the function has another way. A handler of the function that catches it makes the way no likelier.
    bool alwaysEndsInThrow(BlockID block) const { return m_blocksEndingInThrow.get(block.index()); }
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
    BitVector m_blocksEndingInThrow;
    mutable Vector<BlockFrequency> m_blocks;
    mutable bool m_hasClassesOfBlocks { false };
    mutable bool m_hasCounts { false };
    mutable Vector<double> m_frequencies;
};

// Heat decides what is worth doing and never what is right, so a program means the same under any heat. HeatOverride::Shuffled draws every class from the numbers of what it is asked about and the layout seed.
std::optional<uint32_t> seedOfShuffledHeat(const BuildContext&);
uint32_t drawForShuffledHeat(uint32_t seed, uint32_t first, uint32_t second, uint32_t numberOfChoices);
inline BlockFrequency drawFrequency(uint32_t seed, uint32_t first, uint32_t second) { return static_cast<BlockFrequency>(drawForShuffledHeat(seed, first, second, 3)); }
void shuffleRecordedFrequencies(Program&, const BuildContext&);

enum class RecordedFrequencyUse : uint8_t { NotRecorded, Ignored, Applied };

RecordedFrequencyUse recordedFrequencyUseOf(const Function&, const CFG&, BlockID, bool ignoresBranchHints);

inline Expected<bool> isRecordedAsRare(const Function& function, BlockID block)
{
    return block != entryBlock && function.at(block).frequency.predicted() == BlockFrequency::Rare;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
