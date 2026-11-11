/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkFlow.h"

namespace JSC { namespace AOT { namespace Typed {

class TypesOfFlowTurnedOnce {
public:
    ALWAYS_INLINE TypeID turn(const LinkFlow& flow, TypeArena& types, const LinkType& type)
    {
        if (type.holdsPrimitivesAlone())
            return type.isNever() ? TypeID() : type.primitivesWithoutLiteralsAsType();
        size_t hash = hashOf(type);
        std::span<const Slot> slots = m_slots.span();
        if (!slots.empty()) {
            size_t mask = slots.size() - 1;
            for (size_t place = hash & mask; slots[place].isFilled; place = (place + 1) & mask) {
                if (slots[place].type.holdsSameAs(type) && slots[place].type.counts == type.counts)
                    return slots[place].turned;
            }
        }
        return turnAndKeep(flow, types, type, hash);
    }

    TypeID& typeOfLiteralWithWhatIsCreatedBelow(LayoutID literal) { return m_typesOfLiteralsWithWhatIsCreatedBelow.add(literal.index(), TypeID()).iterator->value; }

private:
    struct Slot {
        LinkType type;
        TypeID turned;
        bool isFilled { false };
    };

    UncheckedKeyHashMap<uint32_t, TypeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_typesOfLiteralsWithWhatIsCreatedBelow;

    static ALWAYS_INLINE size_t hashOf(const LinkType& type)
    {
        LinkType::Words words = type.wordsWithoutShadows();
        uint64_t hash = (words[0] + static_cast<uint8_t>(type.counts)) * 0x9e3779b97f4a7c15ull ^ words[1] * 0xc2b2ae3d27d4eb4full ^ words[2] * 0x165667b19e3779f9ull ^ words[3] * 0x27d4eb2f165667c5ull;
        return static_cast<size_t>(hash ^ hash >> 32);
    }

    NEVER_INLINE TypeID turnAndKeep(const LinkFlow& flow, TypeArena& types, const LinkType& type, size_t hash)
    {
        if (m_numberKept * 4 >= m_slots.size() * 3)
            grow();
        size_t mask = m_slots.size() - 1;
        size_t place = hash & mask;
        while (m_slots[place].isFilled)
            place = (place + 1) & mask;
        m_slots[place] = { type, flow.toTypeID(types, type), true };
        ++m_numberKept;
        return m_slots[place].turned;
    }

    void grow()
    {
        Vector<Slot> before = std::exchange(m_slots, { });
        m_slots.grow(std::max<size_t>(1024, before.size() * 2));
        size_t mask = m_slots.size() - 1;
        for (const Slot& slot : before) {
            if (!slot.isFilled)
                continue;
            size_t place = hashOf(slot.type) & mask;
            while (m_slots[place].isFilled)
                place = (place + 1) & mask;
            m_slots[place] = slot;
        }
    }

    Vector<Slot> m_slots;
    size_t m_numberKept { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
