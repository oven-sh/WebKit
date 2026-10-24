/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreshold.h"
#include "AOTValueFlowResultsInternal.h"
#include "AOTValueSets.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

class TypeArena;

// Only the start of a location puts `absent` into its set.
enum class DeadZone : uint8_t { MayBeObserved, IsNeverObserved, UndefinedAtStartIsNeverObserved, UndefinedAtStartMayBeObserved };

enum class ValueFlowProof : uint8_t {
    // The set holds no placeholder, and every object it lists lies inside the family.
    Proven,
    // The unknown object arrives: a value from code that nobody compiles, or one that has passed an unsound edge.
    IllTyped,
    // The untracked object arrives, and the family does not say which objects of the program it can be.
    Untracked,
    // An object that the analysis follows arrives and lies outside the family: an unsound cast, or a fault in the family.
    OutsideFamily,
    // An allocation that picks its layout by the prototype it is given arrives. One object stands for all that it makes, only some of that lies inside the family, and nothing is known to keep the rest out.
    UndecidedLayout,
    NothingArrives,
    // The switch for narrowed types is off.
    Disabled,
};

struct ValueFlowPositionResult {
    ValueFlowProof proof { ValueFlowProof::NothingArrives };
    // The part of the family that the set leaves. None unless the position is proven, and where the set says no more than the family.
    std::optional<Type> narrowed;
    // With OutsideFamily and UndecidedLayout: the member with the lowest number that is not sure to lie inside.
    ValueFlowObjectID stranger;
};

// What an allocation makes that picks its layout by the prototype it is given.
struct ValueFlowLayoutsOfSite {
    Vector<LayoutID, 2> layouts;
    // An object with none of these layouts.
    bool mayBeOtherObject { false };
};

// The type of a set of many members, as far as it is the same in every arena. Whoever meets the set first walks its members, and every task makes its own type from what that walk has left.
struct ValueFlowSharedTypeOfSet {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValueFlowSharedTypeOfSet);

    struct Instance {
        ValueFlowObjectID object;
        BuiltinClassID builtinClass;
    };

    // What the members add that have no layout and are no instances of built-in classes.
    Type withoutLayoutsAndInstances;
    // Sorted, each once.
    Vector<LayoutID> layouts;
    // The instances of built-in classes after which the union says something else than before, ascending.
    Vector<Instance> instances;
    // How many of them come before the first member that makes the type any object.
    uint32_t numberOfInstancesBeforeAnyObject { std::numeric_limits<uint32_t>::max() };
    bool firstInstanceMeetsOtherMembers { false };
};

enum class ValueFlowTypesOfSets : bool {
    // What is proven about containers changes while the positions are asked, and the type of a set with it.
    Change,
    StayTheSame,
};

// What all the positions of one step share, on all its threads.
struct ValueFlowPositionCounts {
    WTF_MAKE_NONCOPYABLE(ValueFlowPositionCounts);
public:
    using LayoutsOfSites = HashMap<uint32_t, ValueFlowLayoutsOfSite, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    explicit ValueFlowPositionCounts(const ListenerForSetsOutsideTypes*, ValueFlowTypesOfSets = ValueFlowTypesOfSets::StayTheSame);
    ~ValueFlowPositionCounts();

    void note(const String& place, const Type& held, const Type& family);

    // None until somebody has added it.
    const ValueFlowSharedTypeOfSet* findTypeOfSet(ValueSetID);
    // Two threads may walk one set at the same time. What they find is the same, and the first one stays. Nothing is added once the types that are kept fill mostBytesOfTypesOfSets.
    void addTypeOfSet(ValueSetID, std::unique_ptr<ValueFlowSharedTypeOfSet>&&);

    // By the object of the site. Every other site has the layout that the universe files it under. That of the inputs, or made by whoever asks first.
    const LayoutsOfSites& layoutsOfSites(const ValueFlowResultInputs&);
    // None unless the solver remembers who logged each edge. Made by whoever asks first.
    const ValueFlowBirthsOfUnknownObject* births(const ValueFlowResultInputs&);

    // The sets that list an object outside the family.
    std::atomic<uint64_t> count { 0 };
    // Of the positions that may hold an object as read and at which something arrives: those that are proven, and the others.
    std::atomic<uint64_t> provenPositions { 0 };
    std::atomic<uint64_t> unprovenPositions { 0 };
    std::atomic<uint64_t> narrowedPositions { 0 };
    const bool listsThem;
    const bool sharesTypesOfSets;
    Lock lock;
    const ListenerForSetsOutsideTypes* listener WTF_GUARDED_BY_LOCK(lock);

private:
    struct alignas(64) TypesOfSets {
        Lock lock;
        HashMap<uint32_t, std::unique_ptr<ValueFlowSharedTypeOfSet>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> bySet WTF_GUARDED_BY_LOCK(lock);
    };

    AOT_CLASS_THRESHOLD(size_t, mostBytesOfTypesOfSets, 256 * MB);

    TypesOfSets& typesOfSetsFor(ValueSetID set) { return m_typesOfSets[((set.index() >> 1) * 0x9e3779b1u) >> 26]; }

    std::array<TypesOfSets, 64> m_typesOfSets;
    std::atomic<size_t> m_bytesOfTypesOfSets { 0 };
    std::once_flag m_findLayoutsOfSites;
    LayoutsOfSites m_layoutsOfSites;
    std::once_flag m_findBirths;
    std::optional<ValueFlowBirthsOfUnknownObject> m_births;
};

struct ValueFlowLayoutsOfSites {
    ValueFlowPositionCounts::LayoutsOfSites bySite;
};

// It walks every object, every statement and every node on one thread. It reads the program and the solution alone, so it is the same for all the steps behind one solve.
ValueFlowLayoutsOfSites findLayoutsOfSites(const ValueFlowResultInputs&);

// The one reader of the sets of positions, of values and of operands.
class ValueFlowPositions {
    WTF_MAKE_NONCOPYABLE(ValueFlowPositions);
public:
    ValueFlowPositions(const ValueFlowResultInputs&, TypeArena&, ValueFlowPositionCounts&, uint32_t switchSubject, ValueSetMaker* makerOfThread = nullptr);
    ~ValueFlowPositions();

    // The set with the untracked object replaced by the objects of the family. As it is where the family is not made of layouts and functions of the program alone.
    ValueSetID expand(ValueSetID, const Type& family);
    ValueSetID withoutWhatPlaceholdersCover(ValueSetID);
    ValueSetMaker& maker();

    // The primitives of the family are taken on trust, and the proof speaks for the objects.
    ValueFlowPositionResult resultAt(const Type& family, ValueSetID, DeadZone = DeadZone::MayBeObserved, const ValueFlowNumbers* = nullptr);
    // Nothing checks an expected position: there the proof speaks for the primitives too.
    ValueFlowPositionResult resultAt(const DeclaredType&, ValueSetID, DeadZone = DeadZone::MayBeObserved);
    // The same for the set of the node. Counts the position.
    FlowPositionUpdate updateFor(const DeclaredType&, ValueFlowNodeID, DeadZone = DeadZone::MayBeObserved, const ValueFlowNumbers* = nullptr);

    // The layouts of the objects that arrive. None unless the set is complete: no placeholder is left in it, and every object has one of the layouts.
    std::optional<LayoutSet> receiversAt(const Type& family, ValueSetID);
    // The functions and the built-in functions that a call of the set enters. The set is not complete where something else that can be called may arrive. None where almost everything arrives.
    std::optional<CalleeSet> calleesAt(const Type& family, ValueSetID, bool isConstruction);

    // Whether every member passes a test for the type whatever happens. An allocation that picks its layout does where all that it picks lies inside.
    bool isSurelyInside(ValueSetID, const Type&);
    bool isSurelyInside(ValueSetID, TypeID);
    std::optional<Type> typeWhereNothingPasses(ValueSetID, const Type&);
    // Whether what a plain function without a layout constructs arrives and a test for the type turns it away. The program calls such an object by the type through a cast, and every one of them fails the test.
    bool turnsAwayConstructedObject(ValueSetID, const Type&);
    // Whether a member that a proof takes on trust may fail a test for the type: the object that nobody follows, or an allocation that picks its layout and picks one outside.
    bool holdsObjectThatMayLieOutside(ValueSetID, const Type&);

    // What is proven of the object by itself.
    Type typeOfObject(ValueFlowObjectID object) { return typeOfMember(object); }

    // What took the proof away, and where. The node is the one whose set was asked about, or none for a set of no node. Nothing for a result that is proven or at which nothing arrives.
    FlowSiteLoss lossOf(const ValueFlowPositionResult&, ValueFlowNodeID = { });

private:
    // How much of the family holds without flow. Each level takes on trust what the one before it does.
    enum class Trust : uint8_t {
        // An expected position: whatever arrives has to be shown to lie inside.
        Nothing,
        // The primitives lie inside.
        Primitives,
        // Every edge into the position is covered by the families or has a test. So an object that may or may not lie inside does.
        Position,
        // Whatever the node lists has passed a test for the family at run time.
        PassedTest,
    };

    bool isEnabled(FlowResultKind kind) const { return m_inputs.build.isEnabled(kind, m_switchSubject); }
    bool isSurelyInside(ValueSetID, const Type&, std::optional<TypeID> number);
    bool holds(ValueSetID set, ValueFlowObjectID member) const { return m_inputs.sets().contains(set, member); }
    const ValueFlowLayoutsOfSite* findLayoutsPickedBy(ValueFlowObjectID);
    void addObjectWithLayout(Type&, Vector<LayoutID, 8>& layouts, ValueFlowObjectID, LayoutID);
    // Whether the classes of the type say something else than before.
    bool addInstanceOfClass(Type&, ValueFlowObjectID, BuiltinClassID);
    // Asks no arena. Of an instance of a built-in class it answers the class and adds nothing.
    BuiltinClassID addUnlessInstanceOfClass(Type&, Vector<LayoutID, 8>& layouts, ValueFlowObjectID);
    void add(Type&, Vector<LayoutID, 8>& layouts, ValueFlowObjectID);
    Type finish(Type&&, std::span<const LayoutID> layouts);
    Type typeOfMember(ValueFlowObjectID);
    bool sharesTypeOf(ValueSetID) const;
    void listTexts(Type&, std::span<const StringID>);
    Type typeOfMembers(ValueSetID);
    Type typeOfMembersToShare(ValueSetID);
    Type typeFrom(const ValueFlowSharedTypeOfSet&);
    // Lists every member but the untracked object, however many there are.
    Type typeOf(ValueSetID);
    // What a type in the program may list of that.
    Type withinLimits(Type&&);
    ValueFlowObjectID findStranger(ValueSetID, const Type& family, Trust);
    std::optional<Type> familyBehindDictionaries(const Type& family);
    ValueFlowPositionResult decide(const Type& family, ValueSetID, DeadZone, Trust, const ValueFlowNumbers*, NOESCAPE const Invocable<String()> auto& place);
    String placeOf(ValueFlowNodeID) const;
    Witness witnessOfStranger(const ValueFlowPositionResult&) const;

    const ValueFlowResultInputs& m_inputs;
    TypeArena& m_types;
    ValueFlowPositionCounts& m_counts;
    uint32_t m_switchSubject;
    ValueSetMaker* m_maker { nullptr };
    bool m_hasBorrowedMaker { false };
    HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_indexOfSet;
    Vector<Type> m_typesOfSets;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
