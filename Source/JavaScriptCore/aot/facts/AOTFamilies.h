/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayout.h"
#include "AOTTypeArena.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

class Program;

using FamilyID = ID<struct FamilyIDTag>;

class Families {
    WTF_MAKE_NONCOPYABLE(Families);
public:
    Families(Program&, const ParallelLoops&);

    uint32_t size() const { return m_layouts.size(); }
    LayoutSetID layoutsOf(FamilyID id) const { return m_layouts[id.index()]; }
    FamilyID familyOf(LayoutSetID) const;
    FamilyID nearestCommonFamily(LayoutID, LayoutID) const;
    FamilyID nearestCommonFamily(FamilyID, LayoutID) const;
    FamilyID nearestCommonFamily(FamilyID, FamilyID) const;
    FamilyID nearestFamilyOf(LayoutID id) const { return id.index() < m_nearestFamilyOfLayout.size() ? m_nearestFamilyOfLayout[id.index()] : FamilyID(); }
    FamilyID nearestFamilyAbove(FamilyID id) const { return m_waysUp[id.index()].above; }
    void forEachFamilyOf(LayoutID id, NOESCAPE const Invocable<void(FamilyID)> auto& functor) const
    {
        for (FamilyID family = nearestFamilyOf(id); family; family = m_waysUp[family.index()].above)
            functor(family);
    }
    BitVector familiesWithSomeMemberThat(NOESCAPE const Invocable<bool(LayoutID)> auto& matches) const
    {
        BitVector found(size());
        for (uint32_t layout = 0; layout < m_nearestFamilyOfLayout.size(); ++layout) {
            FamilyID nearest = m_nearestFamilyOfLayout[layout];
            if (nearest && !found.quickGet(nearest.index()) && matches(LayoutID(layout)))
                found.quickSet(nearest.index());
        }
        for (uint32_t family = 0; family < size(); ++family) {
            if (FamilyID above = m_waysUp[family].above; above && found.quickGet(family))
                found.quickSet(above.index());
        }
        return found;
    }

private:
    friend TypeID widenedToFamily(TypeArena&, const Families&, TypeID);

    FamilyID smallestFamilyHolding(const TypeArena&, LayoutSetID) const;

    Vector<LayoutSetID> m_layouts;
    struct WayUp {
        FamilyID above;
        FamilyID farAbove;
        uint32_t depth { 0 };
    };
    FamilyID nearestCommonFamilyOfFamiliesOrNone(FamilyID, FamilyID) const;

    Vector<WayUp> m_waysUp;
    Vector<FamilyID> m_nearestFamilyOfLayout;
    Vector<FamilyID> m_familiesBySet;
};

TypeID widenedToFamily(TypeArena&, const Families&, TypeID);
TypeID joinOf(TypeArena&, const Families&, TypeID, TypeID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
