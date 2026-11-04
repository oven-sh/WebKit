/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <utility>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// Strangers that meet are united with each other, not with everybody. Members are numbers from 0 on: layouts, functions, allocation sites.
// Each starts alone, in a family of its own. unite() makes one family of two. Nothing is ever taken apart, so whatever holds of a family holds of it from then on.
// A type keeps the number of ANY member and asks for representativeOf() when it is used: the number stays good when families merge, the representative does not.
// Near-linear: the smaller family goes into the larger, and a lookup halves the path it walks. The members of a family stand in a ring, and two rings become one by a swap.
class UnitedFamilies {
public:
    UnitedFamilies() = default;
    explicit UnitedFamilies(uint32_t numberOfMembers) { growTo(numberOfMembers); }

    uint32_t numberOfMembers() const { return static_cast<uint32_t>(m_parent.size()); }
    // For members that are made as the work goes. Each new one is alone.
    void growTo(uint32_t numberOfMembers)
    {
        uint32_t first = this->numberOfMembers();
        if (numberOfMembers <= first)
            return;
        m_parent.grow(numberOfMembers);
        m_next.grow(numberOfMembers);
        m_size.grow(numberOfMembers);
        for (uint32_t member = first; member < numberOfMembers; ++member) {
            m_parent[member] = member;
            m_next[member] = member;
            m_size[member] = 1;
        }
    }

    // Set for a thread while several of them ask: then nobody unites, and no lookup writes. A member that was made meanwhile has no place here yet, and is alone.
    static inline thread_local bool s_threadOnlyReads = false;

    uint32_t representativeOf(uint32_t member) const
    {
        if (s_threadOnlyReads)
            return representativeWithoutShortening(member);
        while (m_parent[member] != member) {
            m_parent[member] = m_parent[m_parent[member]];
            member = m_parent[member];
        }
        return member;
    }
    bool areUnited(uint32_t one, uint32_t other) const { return representativeOf(one) == representativeOf(other); }
    // It writes nothing, so several threads may ask while nobody unites.
    uint32_t representativeWithoutShortening(uint32_t member) const
    {
        if (member >= m_parent.size())
            return member;
        while (m_parent[member] != member)
            member = m_parent[member];
        return member;
    }
    bool isAlone(uint32_t member) const { return member >= m_next.size() || m_next[member] == member; }
    uint32_t sizeOfFamilyOf(uint32_t member) const { return m_size[representativeOf(member)]; }

    struct Union {
        // The representative of both from now on.
        uint32_t kept { 0 };
        // What was the representative of the smaller family. Whatever hangs on it has to be tied to what hangs on `kept`. Equal to `kept` where the two were one family already.
        uint32_t absorbed { 0 };
        bool hasMerged() const { return kept != absorbed; }
    };
    Union unite(uint32_t one, uint32_t other)
    {
        uint32_t kept = representativeOf(one);
        uint32_t absorbed = representativeOf(other);
        if (kept == absorbed)
            return { kept, kept };
        if (m_size[kept] < m_size[absorbed])
            std::swap(kept, absorbed);
        m_parent[absorbed] = kept;
        m_size[kept] += m_size[absorbed];
        std::swap(m_next[kept], m_next[absorbed]);
        return { kept, absorbed };
    }

    // Every member of the family of `member`, that one first. The functor may not unite anything.
    template<typename Functor>
    void forEachMemberOfFamilyOf(uint32_t member, const Functor& functor) const
    {
        uint32_t current = member;
        do {
            functor(current);
            current = m_next[current];
        } while (current != member);
    }

private:
    mutable Vector<uint32_t> m_parent;
    Vector<uint32_t> m_next;
    Vector<uint32_t> m_size;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
