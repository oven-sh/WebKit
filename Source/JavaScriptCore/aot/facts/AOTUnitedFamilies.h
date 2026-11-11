/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <utility>
#include <wtf/Atomics.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

class UnitedFamilies {
public:
    UnitedFamilies() = default;
    explicit UnitedFamilies(uint32_t numberOfMembers) { growTo(numberOfMembers); }

    uint32_t numberOfMembers() const { return static_cast<uint32_t>(m_parent.size()); }
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

    uint32_t representativeOf(uint32_t member) const
    {
        if (member >= m_parent.size())
            return member;
        uint32_t* parents = m_parent.mutableSpan().data();
        for (uint32_t parent = parentOf(parents, member); parent != member; parent = parentOf(parents, member)) {
            uint32_t above = parentOf(parents, parent);
            if (above != parent)
                WTF::atomicStore(parents + member, above, std::memory_order_relaxed);
            member = above;
        }
        return member;
    }
    bool areUnited(uint32_t one, uint32_t other) const { return representativeOf(one) == representativeOf(other); }
    ALWAYS_INLINE uint32_t representativeWithoutShortening(uint32_t member) const
    {
        if (member >= m_parent.size())
            return member;
        const uint32_t* parents = m_parent.span().data();
        for (uint32_t parent = parentOf(parents, member); parent != member; parent = parentOf(parents, member))
            member = parent;
        return member;
    }
    bool isAlone(uint32_t member) const { return member >= m_next.size() || m_next[member] == member; }
    uint32_t sizeOfFamilyOf(uint32_t member) const { return m_size[representativeOf(member)]; }

    struct Union {
        uint32_t kept { 0 };
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
    static uint32_t parentOf(const uint32_t* parents, uint32_t member) { return WTF::atomicLoad(const_cast<uint32_t*>(parents) + member, std::memory_order_relaxed); }

    mutable Vector<uint32_t> m_parent;
    Vector<uint32_t> m_next;
    Vector<uint32_t> m_size;
};

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
