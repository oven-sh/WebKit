/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include <memory>
#include <span>
#include <utility>
#include <wtf/Dominators.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Dominators : public WTF::Dominators<CFG> {
    WTF_MAKE_NONCOPYABLE(Dominators);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Dominators);
public:
    Dominators(CFG&, bool selfCheck);
    ~Dominators();

    std::span<const BlockID> children(BlockID block) const
    {
        RELEASE_ASSERT(block.index() < m_cfg.numNodes());
        uint32_t begin = m_firstChild[block.index()];
        return m_children.subspan(begin, m_firstChild[block.index() + 1] - begin);
    }

    uint32_t depthOf(BlockID block) const { return m_depth[block.index()]; }
    BlockID nearestCommonDominator(BlockID, BlockID) const;

    bool terminatorDominates(BlockID from, BlockID to) const;

    void walkTree(NOESCAPE const Invocable<void(BlockID)> auto& enter, NOESCAPE const Invocable<void(BlockID)> auto& leave) const
    {
        Vector<std::pair<BlockID, uint32_t>, 16> stack;
        BlockID root = m_cfg.root();
        enter(root);
        stack.append({ root, 0 });
        while (!stack.isEmpty()) {
            auto [block, next] = stack.last();
            auto childBlocks = children(block);
            if (next == childBlocks.size()) {
                leave(block);
                stack.removeLast();
                continue;
            }
            ++stack.last().second;
            BlockID child = childBlocks[next];
            enter(child);
            stack.append({ child, 0 });
        }
    }

private:
    class SplitBlockDominators;

    void computeChildren();
    void computeJumpPointers();

    BlockID jumpOf(BlockID block) const { return m_jump[block.index()]; }

    const CFG& m_cfg;
    Vector<BlockID> m_children;
    Vector<uint32_t> m_firstChild;
    Vector<uint32_t> m_depth;
    Vector<BlockID> m_jump;
    std::unique_ptr<SplitBlockDominators> m_splitBlockDominators;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
