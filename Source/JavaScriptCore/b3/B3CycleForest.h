/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include "B3BasicBlock.h"
#include <wtf/IndexMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class Procedure;

class CycleForest {
    WTF_MAKE_NONCOPYABLE(CycleForest);
public:
    static constexpr unsigned none = UINT_MAX;

    enum class HeaderChoice : uint8_t { OnlyEntry, AbovePoll };

    struct Cycle {
        BasicBlock* header() const { return entries[0]; }
        bool isReducible() const { return entries.size() == 1; }

        Vector<BasicBlock*, 2> entries;
        unsigned parent { none };
        unsigned depth { 1 };
    };

    explicit CycleForest(Procedure&, HeaderChoice = HeaderChoice::OnlyEntry);

    unsigned size() const { return m_cycles.size(); }
    const Cycle& operator[](unsigned index) const LIFETIME_BOUND { return m_cycles[index]; }
    unsigned innermost(BasicBlock* block) const { return m_innermost[block]; }
    unsigned depth(unsigned cycle) const { return cycle == none ? 0 : m_cycles[cycle].depth; }
    bool isHeader(BasicBlock* block) const { return m_innermost[block] != none && m_cycles[m_innermost[block]].header() == block; }
    const Vector<BasicBlock*>& blocksInPostOrder() const LIFETIME_BOUND { return m_postOrder; }

    unsigned nearestCommon(unsigned a, unsigned b) const
    {
        while (a != b) {
            if (a == none || b == none)
                return none;
            if (m_cycles[a].depth >= m_cycles[b].depth)
                a = m_cycles[a].parent;
            else
                b = m_cycles[b].parent;
        }
        return a;
    }

    bool contains(unsigned cycle, BasicBlock* block) const { return nearestCommon(cycle, m_innermost[block]) == cycle; }

private:
    class Builder;

    Vector<Cycle> m_cycles;
    IndexMap<BasicBlock*, unsigned> m_innermost;
    Vector<BasicBlock*> m_postOrder;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
