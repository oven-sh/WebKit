/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLinkFlow.h"

namespace JSC { namespace AOT { namespace Typed {

// The writers of Link turn what stands at each position into a type of the program, one position after the other, and most positions hold what another holds.
// LinkFlow::toTypeID() answers the same for the same type as long as a writer writes: the flow has ended, and whom the tables may name is settled. The first of a kind makes the types, in its turn.
class TypesOfFlowTurnedOnce {
public:
    TypeID turn(const LinkFlow& flow, TypeArena& types, const LinkType& type)
    {
        if (m_numberKept * 4 >= m_slots.size() * 3)
            grow();
        size_t mask = m_slots.size() - 1;
        for (size_t place = hashOf(type) & mask;; place = (place + 1) & mask) {
            Slot& slot = m_slots[place];
            if (!slot.isFilled) {
                slot = { type, flow.toTypeID(types, type), true };
                ++m_numberKept;
                return slot.turned;
            }
            if (slot.type == type)
                return slot.turned;
        }
    }

private:
    struct Slot {
        LinkType type;
        TypeID turned;
        bool isFilled { false };
    };

    static size_t hashOf(const LinkType& type)
    {
        auto mix = [](uint64_t hash, uint64_t bits) {
            hash = (hash ^ bits) * 0x9e3779b97f4a7c15ull;
            return hash ^ hash >> 32;
        };
        uint64_t small = type.primitives | static_cast<uint64_t>(type.objects) << 16 | static_cast<uint64_t>(type.functions) << 19 | static_cast<uint64_t>(type.classes) << 21
            | static_cast<uint64_t>(type.holdsUnseen) << 23 | static_cast<uint64_t>(type.holdsLostBuiltins) << 24
            | static_cast<uint64_t>(type.builtinClass.index()) << 32 | static_cast<uint64_t>(type.builtin.index()) << 48;
        uint64_t hash = mix(0, small);
        hash = mix(hash, static_cast<uint64_t>(type.layout.bits()) << 32 | type.family.bits());
        hash = mix(hash, static_cast<uint64_t>(type.function.bits()) << 32 | type.constructorsOf.bits());
        return static_cast<size_t>(mix(hash, type.site.bits()));
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
