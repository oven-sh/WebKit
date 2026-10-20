/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTUseLists.h"
#include <compare>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct LivenessOptions {
    const BitVector* trackedValues { nullptr };
    const BitVector* trackedBlocks { nullptr };
    std::span<const std::pair<BlockID, BlockID>> additionalEdges;
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

    bool isTrackedValue(ValueID id) const { return !m_trackedValues || m_trackedValues->get(id.index()); }
    bool isTrackedBlock(BlockID) const;
    ValueID baseOf(ValueID) const;
    template<typename Functor> void forEachBase(ValueID, NOESCAPE const Functor&) const;
    Vector<InteriorPointer> collectInteriorPointers() const;
    void computeLiveAtHeads(std::span<const InteriorPointer>);

    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    std::optional<BitVector> m_trackedValues;
    std::optional<BitVector> m_trackedBlocks;
    Vector<std::pair<BlockID, BlockID>> m_additionalEdges;
    Vector<Vector<ValueID>> m_liveAtHead;
    bool m_hasTrackedBases { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
