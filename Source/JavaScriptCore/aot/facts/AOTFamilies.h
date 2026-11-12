/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTLayout.h"
#include "AOTTypeArena.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT {

class ParallelLoops;

namespace Typed {

class Program;

using FamilyID = ID<struct FamilyIDTag>;

enum class SetsThatNoFamilyHolds : bool { Stay, BecomeAnyObject };
using ContentsWidenedToFamilies = UncheckedKeyHashMap<uint32_t, TypeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

using SetsOfLayoutsWritten = UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

enum class SourceOfSetForFamilies : uint8_t {
    Class,
    OtherLayoutOfClassesAlone,
    MetInFlow,
    OtherLayout,
    WrittenTypeAlone,
};

struct DeclaredSetForFamilies {
    LayoutSetID layouts;
    SourceOfSetForFamilies source { SourceOfSetForFamilies::WrittenTypeAlone };
    uint32_t size { 0 };
};

Vector<DeclaredSetForFamilies> setsDeclaredForFamilies(const Program&, const SetsOfLayoutsWritten& writtenInBodies, const ParallelLoops&);

class Families {
    WTF_MAKE_NONCOPYABLE(Families);
public:
    enum ClassesAloneTag { ClassesAlone };
    Families(Program&, ClassesAloneTag, const ParallelLoops&);
    Families(Program&, const SetsOfLayoutsWritten& writtenInBodies, std::span<const LayoutSetID> layoutsThatMet, const ParallelLoops&);
    Families(Program&, std::span<const DeclaredSetForFamilies> declaredInProgram, std::span<const LayoutSetID> layoutsThatMet, const ParallelLoops&);

    uint32_t size() const { return m_layouts.size(); }
    LayoutSetID layoutsOf(FamilyID id) const { return m_layouts[id.index()]; }
    FamilyID familyOf(LayoutSetID) const;
    FamilyID nearestCommonFamily(LayoutID, LayoutID) const;
    FamilyID nearestCommonFamily(FamilyID, LayoutID) const;
    FamilyID nearestCommonFamily(FamilyID, FamilyID) const;
    FamilyID nearestFamilyOf(LayoutID id) const { return id.index() < m_nearestFamilyOfLayout.size() ? m_nearestFamilyOfLayout[id.index()] : FamilyID(); }
    FamilyID nearestFamilyAbove(FamilyID id) const { return m_waysUp[id.index()].above; }
    FamilyID topmostFamilyAtOrAbove(FamilyID id) const
    {
        while (m_waysUp[id.index()].depth)
            id = m_waysUp[id.index()].farAbove;
        return id;
    }
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
    friend TypeID widenedToFamily(TypeArena&, const Families&, TypeID, SetsThatNoFamilyHolds, ContentsWidenedToFamilies*);

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

TypeID widenedToFamily(TypeArena&, const Families&, TypeID, SetsThatNoFamilyHolds = SetsThatNoFamilyHolds::Stay, ContentsWidenedToFamilies* foundAround = nullptr);
TypeID joinOf(TypeArena&, const Families&, TypeID, TypeID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
