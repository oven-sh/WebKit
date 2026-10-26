/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTVector.h"
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Dominators.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

class Dominators {
    WTF_MAKE_NONCOPYABLE(Dominators);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Dominators);
public:
    using Tree = WTF::Dominators<CFG>;

    Dominators(CFG&, bool selfCheck);
    ~Dominators();

    Tree& treeForChecking() const LIFETIME_BOUND;
    bool wasComparedWithNaiveSolution() const { return m_wasComparedWithNaiveSolution; }
    void didCompareWithNaiveSolution() const { m_wasComparedWithNaiveSolution = true; }

    BlockID idom(BlockID block) const { return placeOf(block).idom; }

    bool strictlyDominates(BlockID from, BlockID to) const
    {
        if (from == m_cfg.root())
            return to != from && m_cfg.isReachable(to);
        uint32_t numberOfDominated = placeOf(to).number;
        const Place& dominator = placeOf(from);
        return dominator.number < numberOfDominated && numberOfDominated <= dominator.lastNumberBelow;
    }

    bool dominates(BlockID from, BlockID to) const { return from == to || strictlyDominates(from, to); }

    std::span<const BlockID> children(BlockID block) const
    {
        RELEASE_ASSERT(block.index() < m_cfg.numNodes());
        if (m_firstChild.isEmpty()) [[unlikely]]
            computeChildren();
        uint32_t begin = m_firstChild[block.index()];
        return m_children.subspan(begin, m_firstChild[block.index() + 1] - begin);
    }

    uint32_t depthOf(BlockID block) const
    {
        if (m_depth.isEmpty()) [[unlikely]]
            computeJumpPointers();
        return m_depth[block.index()];
    }

    BlockID nearestCommonDominator(BlockID, BlockID) const;

    bool terminatorDominates(BlockID from, BlockID to) const;
    bool terminatorDominatesInSplitGraph(BlockID from, BlockID to) const;

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

    struct Place {
        BlockID idom;
        uint32_t number { UINT32_MAX };
        uint32_t lastNumberBelow { 0 };
    };

    const Place& placeOf(BlockID block) const
    {
        if (m_places.isEmpty()) [[unlikely]]
            computePlaces();
        return m_places[block.index()];
    }

    void computePlaces() const;
    bool tryFindDominatorsByOrder(std::span<uint32_t> dominatorByOrder) const;
    void computeChildren() const;
    void computeJumpPointers() const;
    const BitVector& blocksReachedFromHandlerOfDominator() const;

    BlockID jumpOf(BlockID block) const
    {
        if (m_depth.isEmpty()) [[unlikely]]
            computeJumpPointers();
        return m_jump[block.index()];
    }
    BlockID ancestorAtDepth(BlockID, uint32_t depth) const;

    CFG& m_cfg;
    mutable Vector<Place> m_places;
    mutable std::optional<Tree> m_treeForChecking;
    mutable Vector<BlockID> m_children;
    mutable Vector<uint32_t> m_firstChild;
    mutable Vector<uint32_t> m_depth;
    mutable Vector<BlockID> m_jump;
    bool m_selfCheck;
    mutable bool m_wasComparedWithNaiveSolution { false };
    mutable std::optional<BitVector> m_blocksReachedFromHandlerOfDominator;
    mutable std::unique_ptr<SplitBlockDominators> m_splitBlockDominators;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
