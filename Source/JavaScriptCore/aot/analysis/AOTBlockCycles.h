/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/CycleForest.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class BlockCycles {
    WTF_MAKE_NONCOPYABLE(BlockCycles);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BlockCycles);
public:
    using Forest = WTF::CycleForest<CFG>;

    explicit BlockCycles(CFG&, const Function* functionWithFrequencies = nullptr);

    bool hasCycleWithSeveralEntries() const;

    bool isInCycle(BlockID block) const
    {
        RELEASE_ASSERT(block.index() < m_cfg.numNodes());
        return m_cfg.hasCycle() && m_outermost[block.index()] != Forest::none;
    }

    bool areInSameCycle(BlockID a, BlockID b) const { return isInCycle(a) && outermostCycleOf(a) == outermostCycleOf(b); }

    std::optional<unsigned> outermostCycleOf(BlockID block) const
    {
        if (!m_cfg.hasCycle() || m_outermost[block.index()] == Forest::none)
            return std::nullopt;
        return m_outermost[block.index()];
    }

    std::optional<unsigned> innermostCycleOf(BlockID block) const
    {
        if (!m_cfg.hasCycle())
            return std::nullopt;
        unsigned cycle = forest().innermost(block);
        if (cycle == Forest::none)
            return std::nullopt;
        return cycle;
    }

    bool isInEveryCycleOf(BlockID block, BlockID other) const
    {
        std::optional<unsigned> innermost = innermostCycleOf(other);
        return !innermost || m_forest->contains(*innermost, block);
    }

    unsigned depthOf(BlockID block) const { return m_cfg.hasCycle() ? forest().depth(m_forest->innermost(block)) : 0; }

    const Forest& forest() const LIFETIME_BOUND
    {
        if (!m_forest) [[unlikely]]
            computeForest();
        return *m_forest;
    }

private:
    void computeForest() const;

    CFG& m_cfg;
    const Function* m_functionWithFrequencies;
    mutable std::optional<Forest> m_forest;
    Vector<uint32_t> m_outermost;
    mutable Vector<uint32_t> m_membersOfOutermostCycles;
    mutable Vector<uint32_t> m_firstMemberOfOutermostCycle;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
