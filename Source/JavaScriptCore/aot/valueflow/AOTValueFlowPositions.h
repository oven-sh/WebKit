/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowResultsInternal.h"
#include "AOTValueSets.h"
#include "AOTVector.h"
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

class TypeArena;

enum class DeadZone : uint8_t { MayBeObserved, IsNeverObserved, UndefinedAtStartIsNeverObserved, UndefinedAtStartMayBeObserved };

enum class ValueFlowProof : uint8_t {
    Proven,
    IllTyped,
    Untracked,
    OutsideFamily,
    UndecidedLayout,
    NothingArrives,
    Disabled,
};

struct ValueFlowPositionResult {
    ValueFlowProof proof { ValueFlowProof::NothingArrives };
    std::optional<Type> narrowed;
    ValueFlowObjectID stranger;
    // The narrowed type is what flow holds: the family has neither taken anything out of it nor put anything in.
    bool isWhatFlowHolds { false };
};

struct ValueFlowLayoutsOfSite {
    Vector<LayoutID, 2> layouts;
    bool mayBeOtherObject { false };
};

struct ValueFlowLayoutsOfSites {
    HashMap<uint32_t, ValueFlowLayoutsOfSite, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> bySite;
};

ValueFlowLayoutsOfSites findLayoutsOfSites(const ValueFlowResultInputs&);

struct ValueFlowSharedTypeOfSet {
    struct Instance {
        ValueFlowObjectID object;
        BuiltinClassID builtinClass;
    };

    ValueSetID set;
    Type withoutLayoutsAndInstances;
    Vector<LayoutID> layouts;
    Vector<Instance> instances;
    uint32_t numberOfInstancesBeforeAnyObject { std::numeric_limits<uint32_t>::max() };
    bool firstInstanceMeetsOtherMembers { false };
};

enum class ValueFlowTypesOfSets : bool { Change, StayTheSame };

struct ValueFlowPositionCounts {
    WTF_MAKE_NONCOPYABLE(ValueFlowPositionCounts);
public:
    explicit ValueFlowPositionCounts(const ListenerForSetsOutsideTypes* listener, ValueFlowTypesOfSets typesOfSets = ValueFlowTypesOfSets::StayTheSame)
        : sharesTypesOfSets(typesOfSets == ValueFlowTypesOfSets::StayTheSame)
        , listener(listener)
    {
    }

    std::atomic<uint64_t> setsOutsideFamilies { 0 };
    std::atomic<uint64_t> provenPositions { 0 };
    std::atomic<uint64_t> unprovenPositions { 0 };
    std::atomic<uint64_t> narrowedPositions { 0 };
    const bool sharesTypesOfSets;
    Lock lockOfListener;
    const ListenerForSetsOutsideTypes* const listener;
    SharedMemo<ValueFlowSharedTypeOfSet> typesOfSets;
    std::atomic<size_t> bytesOfTypesOfSets { 0 };
    std::once_flag hasFoundLayoutsOfSites;
    ValueFlowLayoutsOfSites layoutsOfSites;
    std::once_flag hasFoundBirths;
    std::optional<ValueFlowBirthsOfUnknownObject> births;
};

class ValueFlowPositions {
    WTF_MAKE_NONCOPYABLE(ValueFlowPositions);
public:
    ValueFlowPositions(const ValueFlowResultInputs&, TypeArena&, ValueFlowPositionCounts&, uint32_t switchSubject, ValueSetMaker* makerOfThread = nullptr);
    ~ValueFlowPositions();

    ValueSetID withoutWhatPlaceholdersCover(ValueSetID);
    ValueSetMaker& maker();

    // The value lies in the family whatever flow holds: its primitives are taken from there, and flow narrows within it.
    ValueFlowPositionResult resultAt(ProvenType family, ValueSetID, DeadZone = DeadZone::MayBeObserved, const ValueFlowNumbers* = nullptr);
    // Nothing is known of the value: the family holds only if all that arrives lies in it.
    ValueFlowPositionResult resultAt(ExpectedType family, ValueSetID, DeadZone = DeadZone::MayBeObserved);
    ValueFlowPositionResult resultAt(const DeclaredType&, ValueSetID, DeadZone = DeadZone::MayBeObserved);
    ValueFlowPositionResult resultAmongAllValues(ValueSetID set) { return resultAt(Proven::byRulesBehindChecks(TypeTable::anything), set); }
    FlowPositionUpdate updateFrom(const ValueFlowPositionResult&, const Type& family);
    FlowPositionUpdate updateFor(const DeclaredType&, ValueFlowNodeID, DeadZone = DeadZone::MayBeObserved, const ValueFlowNumbers* = nullptr);
    void takeWhatArrives(const DeclaredType&, ValueFlowNodeID, FlowPositionUpdate&);

    std::optional<LayoutSet> receiversAt(ValueSetID);
    std::optional<CalleeSet> calleesAt(ValueSetID possible, ValueSetID seen, const ValueFlowOperand* calleeOfConstruction);

    bool isSurelyInside(ValueSetID, const Type&, std::optional<TypeID> numberOfType = std::nullopt);
    std::optional<Type> typeWhereNothingPasses(ValueSetID, const Type&);
    std::optional<Type> typeWhereOnlyInstancesOfBuiltinClassesAreTurnedAway(ValueSetID, const Type&);
    bool holdsObjectThatMayLieOutside(ValueSetID, const Type&);
    bool turnsAwayConstructedObject(ValueSetID, const Type&);
    bool turnsAwayEveryObject(ValueSetID, const Type&);

    Type typeOfObject(ValueFlowObjectID);

    FlowSiteLoss lossOf(const ValueFlowPositionResult&, ValueFlowNodeID = { });
    bool isDataFromOutside(ValueFlowObjectID);

private:
    bool isEnabled(FlowResultKind kind) const { return m_inputs.build.isEnabled(kind, m_switchSubject); }
    bool holds(ValueSetID set, ValueFlowObjectID member) const { return m_inputs.sets().contains(set, member); }
    const ValueFlowLayoutsOfSite* findLayoutsPickedBy(ValueFlowObjectID);
    bool addInstanceOfClass(Type&, ValueFlowObjectID, BuiltinClassID);
    BuiltinClassID addUnlessInstanceOfClass(Type&, Vector<LayoutID, 8>& layouts, ValueFlowObjectID);
    Type finish(Type&&, Type&& instances, std::span<const LayoutID> layouts);
    Type typeOfMembers(ValueSetID);
    Type typeOf(ValueSetID);
    Type withinLimits(Type&&);
    TypeID withTextsWithinLimit(TypeID);
    bool keepTextsOfContentsWithinLimit(ClassParts&);
    ValueFlowObjectID findStranger(ValueSetID, const Type& family);
    std::optional<Type> familyBehindDictionaries(const Type& family);
    template<typename KindOfType> ValueFlowPositionResult decide(KindOfType family, ValueSetID, DeadZone, const ValueFlowNumbers*, ValueFlowNodeID = { });
    String placeOf(ValueFlowNodeID) const;

    const ValueFlowResultInputs& m_inputs;
    TypeArena& m_types;
    ValueFlowPositionCounts& m_counts;
    uint32_t m_switchSubject;
    ValueSetMaker* m_maker { nullptr };
    bool m_hasBorrowedMaker { false };
    // The operations of the function that isDataFromOutside() was asked about last, by (site, position among the statements). Made at the second question about one function.
    FunctionID m_functionOfOperations;
    bool m_hasOperationsBySite { false };
    Vector<std::pair<uint32_t, uint32_t>> m_operationsBySite;
    HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_indexOfSet;
    Vector<Type> m_typesOfSets;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
