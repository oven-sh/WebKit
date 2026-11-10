/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"

namespace JSC { namespace AOT { namespace Typed {

class BackEdgeDominators {
public:
    BackEdgeDominators(const BodyIndex& index, LoopID loop)
        : m_index(index)
        , m_loop(loop)
    {
        m_loopsWithin.grow(index.loop(loop).numberOfLoopsWithin);
        for (BlockID member : index.membersOf(loop)) {
            for (BodyTarget target : index.block(member).terminator().targets()) {
                BlockID head = target.block();
                if (!index.isLoopHead(head) || !index.isInLoop(head, loop) || !index.isInLoop(member, index.loopOf(head)))
                    continue;
                BlockID& last = within(index.loopOf(head)).lastDominator;
                if (!last)
                    last = member;
                while (!index.dominates(last, member))
                    last = index.immediateDominator(last);
            }
        }
    }

    bool contains(BlockID block) { return contains(block, m_index.loopOf(block)); }
    BlockID lastOf(LoopID loop)
    {
        BlockID last = within(loop).lastDominator;
        return last ? last : m_index.loop(loop).head;
    }

private:
    struct LoopWithin {
        BlockID lastDominator;
        std::optional<bool> headIsPassed;
    };
    LoopWithin& within(LoopID loop) { return m_loopsWithin[m_index.loop(loop).placeAmongLoops - m_index.loop(m_loop).placeAmongLoops]; }
    bool headIsPassed(LoopID loop)
    {
        if (loop == m_loop)
            return true;
        if (!within(loop).headIsPassed)
            within(loop).headIsPassed = contains(m_index.loop(loop).head, m_index.loop(loop).parent);
        return *within(loop).headIsPassed;
    }
    bool contains(BlockID block, LoopID holder)
    {
        return headIsPassed(holder) && m_index.dominates(block, lastOf(holder));
    }

    const BodyIndex& m_index;
    LoopID m_loop;
    Vector<LoopWithin, 8> m_loopsWithin;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
