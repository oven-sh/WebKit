/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTChainDependents.h"
#include "AOTLayoutData.h"
#include "AdaptiveInferredPropertyValueWatchpointBase.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class ClassTable;
struct State;

class MemberGuardWatchpoint final : public AdaptiveInferredPropertyValueWatchpointBase {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(MemberGuardWatchpoint);
public:
    MemberGuardWatchpoint(const ObjectPropertyCondition&, ClassTable&, const LayoutData& classLayout, ClassSide);

private:
    void handleFire(VM&, const FireDetail&) final;

    ClassTable& m_classes;
    const LayoutData& m_classLayout;
    ClassSide m_side;
};

// The numbers of the names that a class has without declaring them. A name that the program never mentions has none.
struct ImplicitMemberKeys {
    std::optional<uint32_t> constructor;
    std::optional<uint32_t> prototype;
    std::optional<uint32_t> name;
    std::optional<uint32_t> length;
};

class MemberGuardWatchpoints final : public ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(MemberGuardWatchpoints);
public:
    explicit MemberGuardWatchpoints(State&);
    ~MemberGuardWatchpoints();

    bool tryWatch(VM&, const LayoutData& classLayout, ClassSide, JSObject* holder, UniquedStringImpl*);
    void stopWatching(const LayoutData& classLayout);
    // For objects that stand in the chain of the class and hold no class of it.
    void watchOtherObjects(std::span<JSObject* const>, const LayoutData& classLayout, const MemberGuardData&, UniquedStringImpl*);

    // A guard is set at most once. One that has ended stays clear for the life of the program.
    bool hasEnded(const MemberGuardData& guard) const { return m_endedGuards.contains(&guard); }
    // A look at the chain that found a setter or a read-only property is not repeated until something is reported under the name.
    bool wasFoundNotToHold(const MemberGuardData& guard) const { return m_guardsFoundNotToHold.contains(&guard); }
    void didFindNotToHold(const MemberGuardData& guard) { m_guardsFoundNotToHold.add(&guard); }
    void mayHoldAgain(const MemberGuardData& guard) { m_guardsFoundNotToHold.remove(&guard); }
    // One entry a class, so that a member of a set is found by its number.
    void didLeaveGuardsOfStoresClear(uint32_t firstBitNumber, const LayoutData& classLayout)
    {
        m_classesWithClearGuardsOfStores.append({ firstBitNumber, &classLayout });
        m_classesWithClearGuardsOfStoresAreSorted = false;
    }
    bool& aStoreByNameHasAsked() { return m_aStoreByNameHasAsked; }
    // True from the second time on.
    bool wasAskedBefore(const MemberGuardData& guard) { return !m_guardsAskedOnce.add(&guard).isNewEntry; }
    void didEnd(const MemberGuardData& guard) { m_endedGuards.add(&guard); }

    bool isAboutExistingClass(uint32_t guardWord) const
    {
        size_t index = guardWord / 64;
        return index < m_guardWordsOfExistingClasses.size() && (m_guardWordsOfExistingClasses[index] >> guardWord % 64 & 1);
    }
    void didCreateClassWith(uint32_t guardWord)
    {
        size_t index = guardWord / 64;
        if (index >= m_guardWordsOfExistingClasses.size())
            m_guardWordsOfExistingClasses.insertFill(m_guardWordsOfExistingClasses.size(), 0, index + 1 - m_guardWordsOfExistingClasses.size());
        m_guardWordsOfExistingClasses[index] |= 1ull << guardWord % 64;
    }
    bool beginSpeakingOf(uint32_t classSide) { return !m_classSidesSpokenOf.set(classSide); }
    bool beginDisarming(const LayoutData& classLayout) { return !m_disarmedClasses.set(classLayout.index); }

    const ImplicitMemberKeys& implicitMemberKeys();

    using Names = Vector<uint32_t, 0>;
    enum class MemberVerdict : uint8_t { NotLookedAt, IsAsDeclared, IsNotAsDeclared };
    struct NamesOnHolder {
        std::optional<size_t> find(uint32_t key) const
        {
            auto found = std::ranges::lower_bound(keys, key);
            return found != keys.end() && *found == key ? std::optional<size_t>(found - keys.begin()) : std::nullopt;
        }

        Names keys;
        Vector<MemberVerdict> verdicts;
    };
    NamesOnHolder* namesLookedUpBelow(const LayoutData& classLayout);
    bool hasChangedPrototypeSide(const LayoutData& classLayout) const { return m_changedPrototypeSides.get(classLayout.index); }
    void didArmWholePrototypeSide(const LayoutData& classLayout, Names&&);
    void didChangeNameOnPrototypeSide(const LayoutData& classLayout, uint32_t key);
    void didChangePrototypeSide(const LayoutData& classLayout)
    {
        m_namesLookedUpBelowClass.remove(classLayout.index);
        m_changedPrototypeSides.set(classLayout.index);
    }

    void didCreateHolder(const LayoutData& classLayout, ClassSide side) { m_createdHolders.append({ &classLayout, side }); }
    void didArmHolder(const LayoutData& classLayout, ClassSide);

    // While the prototype object of a class is a stand-in, nobody knows what a member under a computed name will hide, so no name is looked up through it.
    void didLeaveMembersUnarmed(const LayoutData& classLayout) { m_classesWithMembersLeftUnarmed.set(classLayout.index); }
    bool takeMembersLeftUnarmed(const LayoutData& classLayout)
    {
        if (!m_classesWithMembersLeftUnarmed.get(classLayout.index))
            return false;
        m_classesWithMembersLeftUnarmed.clear(classLayout.index);
        return true;
    }

    // An entry of the method table that depends on a guard bit holds the compiled method while the bit is set, or while the class has no objects yet, and the lookup of its selector otherwise.
    class MethodTableUpdate {
        WTF_MAKE_NONCOPYABLE(MethodTableUpdate);
    public:
        explicit MethodTableUpdate(State&);
        ~MethodTableUpdate();

    private:
        MemberGuardWatchpoints& m_watchpoints;
    };
    void didChangeBit(uint32_t guardBit);
    void didSetMemberOf(uint32_t setBit, uint32_t offsetOfMembers)
    {
        if (m_recentSetsToComplete.add(setBit))
            m_setsToComplete.append({ setBit, offsetOfMembers, false });
    }
    void didClearSetThatHeld(uint32_t setBit, uint32_t offsetOfMembers)
    {
        m_recentSetsToComplete.add(setBit);
        m_setsToComplete.append({ setBit, offsetOfMembers, true });
    }

private:
    using Watchpoints = Vector<std::unique_ptr<MemberGuardWatchpoint>>;

    void dropChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) final;
    bool hasChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) const final;

    State& m_state;
    ChainDependentOwnerID m_chainDependentOwner;
    UncheckedKeyHashMap<uint32_t, Watchpoints, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_watchpointsByClass;
    UncheckedKeyHashSet<const MemberGuardData*> m_endedGuards;
    UncheckedKeyHashSet<const MemberGuardData*> m_guardsFoundNotToHold;
    UncheckedKeyHashSet<const MemberGuardData*> m_guardsAskedOnce;
    Vector<std::pair<uint32_t, const LayoutData*>> m_classesWithClearGuardsOfStores;
    bool m_classesWithClearGuardsOfStoresAreSorted { true };
    bool tryArmGuardOfStoresInTheWay(uint32_t listEntry, bool setHeldBefore);
    bool m_aStoreByNameHasAsked { false };
    // A bit for each word. Every member of every set that is read asks here.
    Vector<uint64_t> m_guardWordsOfExistingClasses;
    BitVector m_classSidesSpokenOf;
    BitVector m_disarmedClasses;
    BitVector m_classesWithMembersLeftUnarmed;
    std::optional<ImplicitMemberKeys> m_implicitMemberKeys;
    UncheckedKeyHashMap<uint32_t, NamesOnHolder, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_namesLookedUpBelowClass;
    BitVector m_changedPrototypeSides;
    Vector<std::pair<const LayoutData*, ClassSide>, 2> m_createdHolders;
    void speakOfCreatedHolders();
    void setCompleteSets();
    void setCompleteSetsNotedSoFar();
    void updateMethodTable();

    struct GuardSet {
        uint32_t bit;
        uint32_t offsetOfMembers;
        // Somebody has asked for it: it held until a class was defined whose guards of stores nobody has looked at.
        bool heldBefore;
    };

    // An entry counts while it bears the number of the round, so a new round writes one word and not the table.
    class RecentNumbers {
    public:
        bool add(uint32_t number)
        {
            uint64_t entry = static_cast<uint64_t>(m_round) << 32 | number;
            return std::exchange(m_entries[number % m_entries.size()], entry) != entry;
        }

        void clear()
        {
            if (!++m_round) [[unlikely]] {
                m_entries.fill(0);
                m_round = 1;
            }
        }

    private:
        std::array<uint64_t, 256> m_entries { };
        uint32_t m_round { 1 };
    };

    // Which member of a set stood in the way when the set was last found blocked: while it still does, the set is not read again.
    struct MemberInTheWay {
        uint32_t setBitPlusOne { 0 };
        uint32_t index { 0 };
    };
    uint32_t memberLastInTheWayOf(uint32_t setBit) const
    {
        const MemberInTheWay& recent = m_membersInTheWay[setBit % m_membersInTheWay.size()];
        if (recent.setBitPlusOne == setBit + 1)
            return recent.index;
        return m_otherMembersInTheWay.isEmpty() ? 0 : m_otherMembersInTheWay.get(setBit);
    }
    void didFindInTheWay(uint32_t setBit, uint32_t index)
    {
        MemberInTheWay& recent = m_membersInTheWay[setBit % m_membersInTheWay.size()];
        if (recent.setBitPlusOne && recent.setBitPlusOne != setBit + 1)
            m_otherMembersInTheWay.set(recent.setBitPlusOne - 1, recent.index);
        recent = { setBit + 1, index };
    }

    static constexpr size_t fewestMembersPassedToRemember = 16;
    std::array<MemberInTheWay, 256> m_membersInTheWay { };
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_otherMembersInTheWay;
    // From the first bit that an entry of the method table hangs on to the last.
    uint32_t m_firstBitWithDependents { 0 };
    uint32_t m_numberOfBitsAmongDependents { 0 };
    Vector<GuardSet, 8> m_setsToComplete;
    Vector<uint32_t, 8> m_changedBits;
    RecentNumbers m_recentSetsToComplete;
    RecentNumbers m_recentChangedBits;
    unsigned m_numberOfOpenUpdates { 0 };
};

bool declaresMember(const LayoutData& classLayout, ClassSide, uint32_t key);
void didEvaluateClassAgain(State&, const LayoutData& classLayout);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
