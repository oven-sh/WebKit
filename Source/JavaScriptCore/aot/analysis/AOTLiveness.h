/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTUseLists.h"
#include "AOTVector.h"
#include <compare>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

struct LivenessOptions {
    enum class Solver : uint8_t { ChosenBySize, PerValue };

    const BitVector* trackedValues { nullptr };
    const BitVector* trackedBlocks { nullptr };
    std::span<const std::pair<BlockID, BlockID>> additionalEdges;
    const OpcodeSet* opcodesOfValues { nullptr };
    Solver solver { Solver::ChosenBySize };
};

class Liveness {
    WTF_MAKE_NONCOPYABLE(Liveness);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Liveness);
public:
    Liveness(const Function&, const CFG&, const UseLists&, const LivenessOptions& = { });

    std::span<const ValueID> liveAtHead(BlockID) const;
    bool isLiveAtHead(ValueID, BlockID) const;

    class LocalCalc {
    public:
        LocalCalc(const Liveness&, BlockID);

        void execute(uint32_t index);

        bool isLive(ValueID id) const
        {
            RELEASE_ASSERT(id.index() < m_liveness.m_useLists.numberOfValues());
            RELEASE_ASSERT(m_liveness.isTrackedValue(id));
            return m_live.quickGet(id.index());
        }

        void forEachLive(NOESCAPE const Invocable<void(ValueID)> auto& functor) const
        {
            for (size_t index : m_live)
                functor(ValueID(index));
        }

    private:
        void use(ValueID);

        const Liveness& m_liveness;
        BlockID m_block;
        uint32_t m_index { 0 };
        BitVector m_live;
    };

private:
    struct InteriorPointer {
        ValueID base;
        ValueID pointer;

        friend auto operator<=>(const InteriorPointer&, const InteriorPointer&) = default;
    };

    struct CrossBlockValue {
        ValueID value;
        BlockID definingBlock;
        uint32_t firstUseBlockIndex;
    };

    struct CrossBlockValues {
        std::span<const BlockID> useBlocksOf(size_t index) const LIFETIME_BOUND
        {
            size_t begin = values[index].firstUseBlockIndex;
            size_t end = index + 1 < values.size() ? values[index + 1].firstUseBlockIndex : useBlocks.size();
            return useBlocks.subspan(begin, end - begin);
        }

        Vector<CrossBlockValue, 32> values;
        Vector<BlockID, 64> useBlocks;
    };

    using AdditionalPredecessors = Vector<Vector<BlockID, 1>>;

    bool isTrackedValue(ValueID id) const { return !m_trackedValues || m_trackedValues->get(id.index()); }
    bool isTrackedBlock(BlockID block) const { return m_cfg.isReachable(block) && (!m_trackedBlocks || m_trackedBlocks->get(block.index())); }
    ValueID baseOf(ValueID) const;
    template<typename Functor> void forEachBase(ValueID, NOESCAPE const Functor&) const;
    CrossBlockValues collectCrossBlockValues(std::span<const InteriorPointer>) const;
    void solvePerValue(const CrossBlockValues&, const AdditionalPredecessors&);
    void solvePerBlock(const CrossBlockValues&, const AdditionalPredecessors&);

    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    std::optional<BitVector> m_trackedValues;
    std::optional<BitVector> m_trackedBlocks;
    Vector<std::pair<BlockID, BlockID>> m_additionalEdges;
    Vector<ValueID> m_liveValues;
    Vector<uint32_t> m_firstLiveValue;
    bool m_hasTrackedBases { false };
};

class LivenessOfAskedValues {
    WTF_MAKE_NONCOPYABLE(LivenessOfAskedValues);
public:
    LivenessOfAskedValues(const Function&, const CFG&, const UseLists&);

    static bool answersFor(const OpcodeSet& opcodesOfValues);

    bool isLiveAtHead(ValueID, BlockID);
    const BitVector& blocksEnteredWith(ValueID);

private:
    uint32_t findBlocksEnteredWith(ValueID);

    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    Vector<uint32_t> m_setOfValue;
    Vector<BitVector> m_blocksEnteredWithValue;
    std::optional<Vector<std::pair<ValueID, BlockID>>> m_narrowedOperandUses;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
