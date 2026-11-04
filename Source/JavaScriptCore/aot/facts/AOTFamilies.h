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

// A family is a set of layouts that the program itself declares: a class with what extends it, a declared union or interface.
// The layouts of a type are one layout or one family, so no type grows with the program.
class Families {
    WTF_MAKE_NONCOPYABLE(Families);
public:
    Families(Program&, const ParallelLoops&);

    uint32_t size() const { return m_layouts.size(); }
    LayoutSetID layoutsOf(FamilyID id) const { return m_layouts[id.index()]; }
    // Null: the set is no family.
    FamilyID familyOf(LayoutSetID) const;
    // The smallest family that holds both. Null: there is none.
    FamilyID nearestCommonFamily(LayoutID, LayoutID) const;
    FamilyID nearestCommonFamily(FamilyID, LayoutID) const;
    FamilyID nearestCommonFamily(FamilyID, FamilyID) const;
    // Null: none holds it.
    FamilyID nearestFamilyOf(LayoutID id) const { return id.index() < m_nearestFamilyOfLayout.size() ? m_nearestFamilyOfLayout[id.index()] : FamilyID(); }
    FamilyID nearestFamilyAbove(FamilyID id) const { return m_waysUp[id.index()].above; }
    // The families that hold the layout, nearest first.
    void forEachFamilyOf(LayoutID id, NOESCAPE const Invocable<void(FamilyID)> auto& functor) const
    {
        for (FamilyID family = nearestFamilyOf(id); family; family = m_waysUp[family.index()].above)
            functor(family);
    }
    // The families that hold a layout of which the answer is yes. Each layout is asked once at most, and each family tells the nearest that holds it.
    // Whoever goes through the members of every family takes a step for each pair of classes in a row of classes that extend one another.
    BitVector familiesWithSomeMemberThat(NOESCAPE const Invocable<bool(LayoutID)> auto& matches) const
    {
        BitVector found(size());
        for (uint32_t layout = 0; layout < m_nearestFamilyOfLayout.size(); ++layout) {
            FamilyID nearest = m_nearestFamilyOfLayout[layout];
            if (nearest && !found.quickGet(nearest.index()) && matches(LayoutID(layout)))
                found.quickSet(nearest.index());
        }
        // What holds a family has a higher number.
        for (uint32_t family = 0; family < size(); ++family) {
            if (FamilyID above = m_waysUp[family].above; above && found.quickGet(family))
                found.quickSet(above.index());
        }
        return found;
    }

private:
    friend TypeID widenedToFamily(TypeArena&, const Families&, TypeID);

    // Of a set of several layouts that is no family. Null: there is none.
    FamilyID smallestFamilyHolding(const TypeArena&, LayoutSetID) const;

    // Smallest first, so a family has a lower number than every family that holds it.
    // Any two are nested or have no layout in common: a declared set that cuts across a family admitted in front of it is no family. So the families are a forest, and joinOf() answers the same in any order.
    // Admitted first are the classes with what extends them, then the other layouts with what is written as them, then the unions that no layout stands for. Among each the smallest first.
    Vector<LayoutSetID> m_layouts;
    // A row of classes that extend one another is a row of families that hold one another, as long as the program makes it. So no family and no layout lists all that hold it.
    struct WayUp {
        // The nearest family that holds this one. Null: none does.
        FamilyID above;
        // A family further up, or the family itself where none holds it. How far up goes by the depth alone, and so that any family above is come to in about as many steps as the depth has digits.
        FamilyID farAbove;
        // How many families hold it.
        uint32_t depth { 0 };
    };
    FamilyID nearestCommonFamilyOfFamiliesOrNone(FamilyID, FamilyID) const;

    Vector<WayUp> m_waysUp;
    Vector<FamilyID> m_nearestFamilyOfLayout;
    // Open addressing by the id of the set. A null entry ends the search.
    Vector<FamilyID> m_familiesBySet;
};

// Several layouts become their nearest common family, and anyObject where they have none. Named objects that stand for an allocation site become their layouts.
TypeID widenedToFamily(TypeArena&, const Families&, TypeID);
// The only join of the middle: TypeArena::unionOf(), then widenedToFamily().
TypeID joinOf(TypeArena&, const Families&, TypeID, TypeID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
