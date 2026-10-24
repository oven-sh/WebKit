/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTKnownClosures.h"
#include "AOTValueFlowResultsInternal.h"
#include "AOTBlockCycles.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCFG.h"
#include "AOTFlowApply.h"
#include "AOTFlowRecords.h"
#include "AOTFlowSiteData.h"
#include "AOTLayout.h"
#include "AOTPhaseTimes.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowPositions.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRules.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <wtf/Atomics.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/UniqueArray.h>

namespace JSC { namespace AOT { namespace Typed {

// Whoever runs one of these on an object sees which keys it has and in which order.
inline constexpr Protocols protocolsListingKeys { Protocol::OwnKeys, Protocol::OwnValues, Protocol::ToJSON };

inline constexpr OptionSet<ObjectMutation> mutationsThatReach { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript };

inline bool hasNodeOfItsOwn(const Field& field)
{
    return field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden });
}

inline uint64_t numberOf(ValueFlowName name)
{
    return static_cast<uint64_t>(name.kind) << 32 | name.payload;
}

// Nobody has followed the values through such code.
inline bool isCompiledWithoutSolve(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, FunctionID id)
{
    return isUnsolvedCode(inputs, tables, id);
}

// The layout whose fields the objects of this one are expected to hold beside their own.
inline LayoutID layoutExpectedAbove(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    return layout.isDerived() ? layout.flow.base : predictedHeritageOf(layout);
}

inline LayoutID provenBaseOf(const FlowApplication& application, LayoutID id)
{
    const LayoutFlow& flow = application.layouts[id.index()].flow;
    return flow.baseState == BaseState::Layout ? flow.base : LayoutID();
}


using SetsByNumber = HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

// Each of the sets comes once.
inline ValueSetID unionOfDistinctSets(ValueSetMaker& maker, Vector<ValueSetID, 4>&& parts)
{
    if (parts.contains(ValueSets::anything))
        return ValueSets::anything;
    Vector<ValueSetID, 4> complements;
    parts.removeAllMatching([&](ValueSetID part) {
        if (!ValueSets::isComplement(part))
            return false;
        complements.append(part);
        return true;
    });
    ValueSetID united = maker.uniteAll(parts.span());
    for (ValueSetID complement : complements)
        united = maker.unite(united, complement);
    return united;
}

class UnionOfSets {
public:
    void add(ValueSetID set)
    {
        if (set == ValueSets::nothing || (!m_parts.isEmpty() && m_parts.last() == set))
            return;
        m_parts.append(set);
        if (m_parts.size() < m_sizeToRemoveRepeatsAt)
            return;
        removeRepeats();
        m_sizeToRemoveRepeatsAt = std::max<size_t>(m_sizeToRemoveRepeatsAt, 2 * m_parts.size());
    }

    void takeFrom(UnionOfSets& other)
    {
        for (ValueSetID part : std::exchange(other.m_parts, { }))
            add(part);
    }

    ValueSetID take(ValueSetMaker& maker)
    {
        removeRepeats();
        return unionOfDistinctSets(maker, std::exchange(m_parts, { }));
    }

    // With all that the placeholders in the union cover.
    ValueSetID takeExpanded(const ValueFlowHeapView& heap, ValueSetMaker& maker)
    {
        return heap.expand(maker, take(maker));
    }

    // The objects that the parts list themselves: nothing that a placeholder among them stands for, and nothing of a part that says what it leaves out.
    ValueSetID takeAsListed(ValueSetMaker& maker)
    {
        removeRepeats();
        Vector<ValueSetID, 4> parts = std::exchange(m_parts, { });
        parts.removeAllMatching([](ValueSetID part) {
            return ValueSets::isComplement(part);
        });
        return maker.uniteAll(parts.span());
    }

private:
    void removeRepeats()
    {
        AOT::sort(m_parts);
        removeRepeatedElements(m_parts);
    }

    Vector<ValueSetID, 4> m_parts;
    size_t m_sizeToRemoveRepeatsAt { 64 };
};

struct PartOfNumber {
    uint32_t number;
    ValueSetID set;

    friend auto operator<=>(const PartOfNumber&, const PartOfNumber&) = default;
};

// Sets that are to be united number by number, each listed once. Each thread of a loop fills one of its own.
class PartsByNumber {
public:
    void add(uint32_t number, ValueSetID set)
    {
        if (set == ValueSets::nothing || (!m_parts.isEmpty() && m_parts.last() == PartOfNumber { number, set }))
            return;
        if (m_isListed.add(static_cast<uint64_t>(number) << 32 | set.index()).isNewEntry)
            m_parts.append({ number, set });
    }

    Vector<PartOfNumber> take()
    {
        m_isListed.clear();
        return std::exchange(m_parts, { });
    }

private:
    Vector<PartOfNumber> m_parts;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_isListed;
};

// The parts of every thread: ascending, each once, the parts of a number side by side.
class PartsOfNumbers {
public:
    PartsOfNumbers(const ParallelLoops& loops, Vector<PartOfNumber>&& parts)
        : m_parts(WTF::move(parts))
    {
        sortAndRemoveDuplicates(loops, m_parts);
        for (uint32_t index = 0; index < m_parts.size(); ++index) {
            if (!index || m_parts[index - 1].number != m_parts[index].number)
                m_firstPartOfNumber.append(index);
        }
        m_firstPartOfNumber.append(m_parts.size());
    }

    // The numbers stand at the positions from zero up, ascending.
    unsigned size() const { return m_firstPartOfNumber.size() - 1; }
    uint32_t numberAt(unsigned position) const { return m_parts[m_firstPartOfNumber[position]].number; }
    std::span<const PartOfNumber> partsAt(unsigned position) const LIFETIME_BOUND { return m_parts.subspan(m_firstPartOfNumber[position], m_firstPartOfNumber[position + 1] - m_firstPartOfNumber[position]); }

    ValueSetID unionAt(unsigned position, ValueSetMaker& maker) const
    {
        std::span<const PartOfNumber> parts = partsAt(position);
        return unionOfDistinctSets(maker, Vector<ValueSetID, 4>(parts.size(), [&](size_t index) {
            return parts[index].set;
        }));
    }

private:
    Vector<PartOfNumber> m_parts;
    Vector<uint32_t> m_firstPartOfNumber;
};

using ListsOfSetsByNumber = HashMap<uint32_t, Vector<ValueSetID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

inline ValueSetID setAt(const SetsByNumber& sets, uint32_t number)
{
    auto found = sets.find(number);
    return found == sets.end() ? ValueSets::nothing : found->value;
}

// An allocation in the constructor of a class answers with an instance of the class whose prototype object it is given, so one object of the universe stands for the instances of a class and of the classes below it.
class LayoutsOfObjects {
public:
    LayoutsOfObjects(const ValueFlowResultInputs&, ValueFlowHeapView&);

    // Both may name a layout more than once.
    void forEachLayoutOf(ValueFlowObjectID object, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        forEachLayoutOfEach([&](const auto& visit) {
            visit(object);
        }, functor);
    }

    void forEachLayoutOfMembers(ValueSetID objects, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        forEachLayoutOfEach([&](const auto& visit) {
            m_inputs.sets().forEachMember(objects, visit);
        }, functor);
    }

    struct SharedLists {
        bool everyEscapedClass { false };
        bool everyLostClass { false };
    };

    // Without the classes of the two shared lists: the answer says which of the lists count as well.
    SharedLists forEachLayoutListedForMembers(ValueSetID objects, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        return forEachListedLayoutOfEach([&](const auto& visit) {
            m_inputs.sets().forEachMember(objects, visit);
        }, functor);
    }

    // Ascending.
    std::span<const LayoutID> escapedClasses() const LIFETIME_BOUND { return m_escapedClasses.span(); }
    std::span<const LayoutID> lostClasses() const LIFETIME_BOUND { return m_lostClasses.span(); }

    // Every object that may have the layout.
    ValueSetID objectsWith(LayoutID layout) const { return m_objectsOfLayout[layout.index()]; }

private:
    // Thousands of allocations may be given the prototype object of any class that has escaped, so those classes are listed once.
    struct SelectedLayouts {
        Vector<LayoutID, 2> listed;
        bool everyEscapedClass { false };
        bool everyLostClass { false };
    };

    SharedLists forEachListedLayoutOfEach(NOESCAPE const auto& forEachObject, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        SharedLists lists;
        forEachObject([&](ValueFlowObjectID object) {
            LayoutID layout = namedLayoutOf(object);
            if (!layout)
                return;
            functor(layout);
            auto selected = m_selectedLayouts.find(object.index());
            if (selected == m_selectedLayouts.end())
                return;
            for (LayoutID other : selected->value.listed)
                functor(other);
            lists.everyEscapedClass |= selected->value.everyEscapedClass;
            lists.everyLostClass |= selected->value.everyLostClass;
        });
        return lists;
    }

    void forEachLayoutOfEach(NOESCAPE const auto& forEachObject, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        SharedLists lists = forEachListedLayoutOfEach(forEachObject, functor);
        if (lists.everyEscapedClass) {
            for (LayoutID layout : m_escapedClasses)
                functor(layout);
        }
        if (lists.everyLostClass) {
            for (LayoutID layout : m_lostClasses)
                functor(layout);
        }
    }

    LayoutID namedLayoutOf(ValueFlowObjectID) const;
    bool selectsLayoutByPrototype(ValueFlowObjectID) const;
    ValueSetID collectClassLayouts(ValueFlowHeapView&);
    SetsByNumber collectWrittenPrototypeLinks(std::span<const ValueFlowObjectID> objects, ValueFlowHeapView&) const;
    SelectedLayouts layoutsSelectedBy(ValueSetID prototypes, LayoutID named, ValueSetID prototypeObjectsOfClasses, ValueFlowHeapView&) const;

    const ValueFlowResultInputs& m_inputs;
    HashMap<uint32_t, SelectedLayouts, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_selectedLayouts;
    Vector<LayoutID> m_escapedClasses;
    Vector<LayoutID> m_lostClasses;
    Vector<ValueSetID> m_objectsOfLayout;
};


struct StoredUnderKey {
    ValueSetID value { ValueSets::nothing };
    // By something else than the operation that allocates the object.
    bool isStoredAfterCreation { false };
    bool mayBeAccessor { false };
    // What the operations of the program do, for a guess at what the property is.
    ValueSetID valueFromOperations { ValueSets::nothing };
    ValueSetID getters { ValueSets::nothing };
    uint8_t accessorForms { 0 };
    // An operation puts a value there itself: a store, or an allocation that lists the field.
    bool isGivenValueByOperation { false };
    // An operation of the program stores under the key and spells it, or computes a key that can be no other. The code of a built-in and a store under a key that may be one of several do not count.
    bool isNamedByOperation { false };
    // A constructor function stores it into its `this`, so the objects that pass the constructor have it.
    bool isStoredByConstructor { false };
    // The numbers in `value`, by what the operations that store them make. Any number as soon as something else than an operation with types stores one.
    bool mayBeOtherThanIntegers { false };
    Range integers { Range::empty() };

    ValueFlowNumbers numbers() const { return { mayBeOtherThanIntegers, integers }; }

    void uniteNumbers(const ValueFlowNumbers& other)
    {
        mayBeOtherThanIntegers |= other.mayBeOtherThanIntegers;
        integers = integers.unionWith(other.integers);
    }
};
static_assert(sizeof(StoredUnderKey) == 40);

// Most objects have a few keys, which are searched one after the other. A long list has an index.
class StoresUnderKeys {
public:
    struct Entry {
        uint32_t key;
        StoredUnderKey value;
    };

    struct AddResult {
        Entry* iterator;
        bool isNewEntry;
    };

    size_t size() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }
    const Entry* begin() const LIFETIME_BOUND { return m_entries.span().data(); }
    const Entry* end() const LIFETIME_BOUND { return begin() + m_entries.size(); }

    const Entry* find(uint32_t key) const LIFETIME_BOUND
    {
        if (m_index) {
            auto found = m_index->find(key);
            return found == m_index->end() ? end() : begin() + found->value;
        }
        for (const Entry& entry : m_entries) {
            if (entry.key == key)
                return &entry;
        }
        return end();
    }

    AddResult add(uint32_t key, StoredUnderKey&& value) LIFETIME_BOUND
    {
        if (const Entry* found = find(key); found != end())
            return { m_entries.mutableSpan().data() + (found - begin()), false };
        if (m_index)
            m_index->add(key, m_entries.size());
        else if (m_entries.size() == mostEntriesWithoutIndex) {
            m_index = makeUnique<Index>();
            for (uint32_t position = 0; position < m_entries.size(); ++position)
                m_index->add(m_entries[position].key, position);
            m_index->add(key, m_entries.size());
        }
        m_entries.append(Entry { key, WTF::move(value) });
        return { &m_entries.last(), true };
    }

private:
    static constexpr size_t mostEntriesWithoutIndex = 12;
    using Index = HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    Vector<Entry, 0, CrashOnOverflow, 1> m_entries;
    std::unique_ptr<Index> m_index;
};

// A bit for each form that a definition may give an accessor, and one for a definition of which nobody can tell.
struct AccessorForms {
    static constexpr uint8_t unknown = 1 << 4;

    static constexpr uint8_t of(bool isEnumerable, bool isConfigurable) { return 1 << ((isEnumerable ? 0 : 2) + (isConfigurable ? 0 : 1)); }

    static std::optional<OptionSet<FieldForm>> onlyFormIn(uint8_t forms)
    {
        for (bool isEnumerable : { true, false }) {
            for (bool isConfigurable : { true, false }) {
                if (forms != of(isEnumerable, isConfigurable))
                    continue;
                OptionSet<FieldForm> form { FieldForm::IsAccessor };
                if (!isEnumerable)
                    form.add(FieldForm::IsNotEnumerable);
                if (!isConfigurable)
                    form.add(FieldForm::IsNotConfigurable);
                return form;
            }
        }
        return std::nullopt;
    }
};

// What is written into the objects that one layout, or one placeholder, stands for.
struct StoresIntoObjects {
    StoresUnderKeys underKey;
    ValueSetID underUnknownKey { ValueSets::nothing };
    ValueFlowNumbers numbersUnderUnknownKey;
    // A property under an index, an accessor under a key nobody knows, or the properties of other objects, of which nobody has followed which.
    bool isStoredUnderIndex { false };
    bool getsAccessorUnderUnknownKey { false };
    bool getsCopiedProperties { false };
    // `const { [key]: taken, ...rest } = source`: which names arrive is decided when the copy runs.
    bool getsCopyWithoutComputedKey { false };
    // What the operations of the program do under a key nobody knows: as StoredUnderKey has it.
    ValueSetID valueFromOperationsUnderUnknownKey { ValueSets::nothing };
    bool isGivenValueByOperationUnderUnknownKey { false };
    ValueSetID gettersUnderUnknownKey { ValueSets::nothing };
    uint8_t formsOfAccessorsUnderUnknownKey { 0 };

    bool getsNameNobodyLists() const { return underUnknownKey != ValueSets::nothing || isStoredUnderIndex || getsAccessorUnderUnknownKey || getsCopiedProperties; }
};

// One run of a statement that writes into objects, as far as the results about fields care.
struct Writer {
    const ValueFlowStatement& statement;
    // The objects that the statement writes into are those that it allocates.
    bool isAllocation { false };
    // The layouts that the type of the receiver admits. Empty where the type says nothing.
    std::span<const LayoutID> layoutsOfReceiver;
    LayoutSetID familyOfReceiver;
};

// A property is written through a set of objects under a name. This sorts what is written by the layouts of the objects.
class StoresByLayout {
public:
    StoresByLayout(const ValueFlowResultInputs&, const FlowResultTables&, ValueFlowHeapView&, const LayoutsOfObjects&);
    void noteListedWrites();

    // All that a store may put under the key into an object of the layout.
    ValueSetID storedUnder(LayoutID layout, PropertyKeyID key) { return storedUnder(m_heap.maker(), layout, key); }
    ValueSetID storedUnder(ValueSetMaker&, LayoutID, PropertyKeyID) const;
    // The numbers among them.
    ValueFlowNumbers numbersStoredUnder(LayoutID, PropertyKeyID) const;
    bool isStoredAfterCreation(LayoutID, PropertyKeyID) const;
    // The properties of other objects are copied onto an object of the layout, and nobody has followed which.
    bool getsCopiedProperties(LayoutID) const;
    // Something may lie on an object of the layout under a name that forEachKeyStoredInto() does not list.
    bool getsNameNobodyLists(LayoutID) const;
    // Ascending, each once.
    Vector<PropertyKeyID> keysStoredInto(LayoutID) const;
    Vector<PropertyKeyID> keysStoredIntoKnownObjects(LayoutID) const;
    Vector<PropertyKeyID> keysNamedByStoresIntoKnownObjects(LayoutID) const;
    bool isStoredByConstructor(LayoutID, PropertyKeyID) const;
    const Vector<PropertyKeyID>& keysStoredThroughPlaceholders() const LIFETIME_BOUND { return m_keysThroughPlaceholders; }
    bool isReachedThroughPlaceholders(LayoutID layout) const
    {
        return hasEscapedObject(layout) || hasLostObject(layout) || !m_keysIntoEveryObject.isEmpty()
            || (m_isEscapedClass.quickGet(layout.index()) && !m_keysIntoEveryEscapedClass.isEmpty())
            || (m_isLostClass.quickGet(layout.index()) && !m_keysIntoEveryLostClass.isEmpty());
    }
    bool mayGetAccessorUnder(LayoutID, PropertyKeyID) const;
    // Where all that was seen to be defined under the key in the known objects of the layout are accessors of one form.
    FlowAccessorField accessorDefinedUnder(LayoutID layout, PropertyKeyID key) const { return accessorDefinedUnder(m_heap.maker(), layout, key); }
    FlowAccessorField accessorDefinedUnder(ValueSetMaker&, LayoutID, PropertyKeyID) const;
    // The keys that a definition of an accessor names and for which accessorDefinedUnder() has an answer, ascending.
    Vector<PropertyKeyID> keysOfAccessorsDefinedIn(ValueSetMaker&, LayoutID) const;
    // Whether accessorDefinedUnder() may have an answer for a key that no definition names.
    bool getsAccessorsUnderKeysNobodyKnows(LayoutID) const;
    // By key, the objects that a read by that name lists among its receivers. An accessor gets a slot for them alone: a slot does nothing for a read that starts below the object or that does not know its receiver.
    void setReceiversOfReads(ListsOfSetsByNumber&&);
    // For a layout that gets accessors under keys nobody knows: the names of the reads that list one of its objects among their receivers, ascending.
    std::span<const PropertyKeyID> keysReadFrom(LayoutID layout) const LIFETIME_BOUND { return m_keysReadFrom[layout.index()].span(); }

    // An object of the layout is in hands that nobody follows.
    bool hasEscapedObject(LayoutID layout) const { return m_hasEscapedObject.quickGet(layout.index()); }
    bool hasLostObject(LayoutID layout) const { return m_hasLostObject.quickGet(layout.index()); }
    bool hasObjectOfCodeWithoutSolve(LayoutID layout) const { return m_hasObjectOfCodeWithoutSolve.quickGet(layout.index()); }

    // The same on all threads, in no order. `worker` is what forEachIndexInParallel() hands to the body of a loop.
    void forEachWriteInParallel(NOESCAPE const Invocable<void(unsigned worker, const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto&) const;
    // The same for the set that a write goes through, on any thread.
    void forEachLayoutWrittenThroughSetOfWrite(ValueSetID objects, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&);

    // The same in two parts, for whoever looks at every write: thousands of writes go through a placeholder, and a placeholder stands for the objects of thousands of layouts.
    struct PlaceholdersInSet {
        bool unknownObject { false };
        bool untrackedObject { false };
        bool almostEverything { false };
    };
    // Behind this the two functions below answer for these sets too.
    void lookAt(std::span<const ValueSetID>);
    // For the set that a write goes through, on any thread.
    PlaceholdersInSet placeholdersInSetOfWrite(ValueSetID objects) const;
    void forEachLayoutListedInSetOfWrite(ValueSetID objects, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&) const;

private:
    struct WrittenNode {
        uint8_t detail;
        uint32_t payload;
        uint32_t set;
        ValueSetID value;

        auto order() const { return std::tuple { detail, payload, set }; }
    };

    struct ObjectsOfSet {
        // Without the classes of the two lists that LayoutsOfObjects keeps once.
        Vector<LayoutID, 4> layouts;
        bool holdsObjectOfEveryEscapedClass { false };
        bool holdsObjectOfEveryLostClass { false };
        bool holdsUnknownObject { false };
        bool holdsUntrackedObject { false };
        bool holdsAlmostEverything { false };
    };

    struct WritesThroughSet {
        uint32_t set;
        uint32_t payload;
        uint32_t familyOfReceiver;
        uint32_t familyDeclaredForReceiver;
        uint8_t detail;
        bool fillsTypedResult;
        bool createsData;
        bool isStoredAfterCreation;
        bool isNamedByOperation;
        // By a constructor function into its `this`.
        bool isStoredByConstructor;
        bool isWrittenByOperation;
        bool givesValueItself;
        bool leavesOutComputedKey;
        uint8_t accessorForms;
        ValueSetID value;
        ValueFlowNumbers numbers;

        ValueFlowName name() const { return { static_cast<ValueFlowNameKind>(detail), payload }; }
        auto group() const { return std::tuple { set, detail, payload, familyOfReceiver, familyDeclaredForReceiver, fillsTypedResult, createsData }; }
        uint32_t hashOfGroup() const
        {
            uint64_t hash = (static_cast<uint64_t>(payload) << 32 | set) * 0x9e3779b97f4a7c15ull;
            hash = (hash ^ hash >> 32 ^ (static_cast<uint64_t>(familyOfReceiver) << 32 | familyDeclaredForReceiver)) * 0xbf58476d1ce4e5b9ull;
            hash = (hash ^ hash >> 32 ^ static_cast<uint64_t>(detail << 2 | fillsTypedResult << 1 | createsData)) * 0x94d049bb133111ebull;
            return static_cast<uint32_t>(hash >> 32);
        }
    };

    // What hangs on the statement and the name of a write, whatever set it goes through.
    struct StoreUnderName {
        const ValueFlowStatement* statement { nullptr };
        ValueFlowName name;
        bool spellsKey { false };
        bool isIntoThisOfConstructor { false };
        uint8_t accessorForms { 0 };
        uint32_t familyDeclaredForReceiver { UINT32_MAX };
        // None until a node under the name holds a number.
        std::optional<ValueFlowNumbers> numbers;
    };

    // A store that spells its key, through a set with a placeholder, whose receiver has a type that lists layouts.
    struct NameStoredThroughDeclaredReceiver {
        uint32_t family;
        uint8_t placeholders;
        bool isStoredByConstructor;
        PropertyKeyID key;

        auto order() const { return std::tuple { family, placeholders, isStoredByConstructor, key }; }
    };

    void findReceiversBehindChecks();
    static constexpr uint32_t isStatementInListOfWrites = 1u << 31;

    LayoutSetID familyOfReceiverOf(const ValueFlowStatement&) const;
    void listWrites();
    void forEachWriteAmong(std::span<const uint32_t>, NOESCAPE const Invocable<void(const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto&) const;
    ObjectsOfSet findObjectsOf(ValueSetID) const;
    const ObjectsOfSet& objectsOf(ValueSetID);
    std::optional<uint32_t> positionOfSetLookedAt(ValueSetID) const;
    void noteObjectsOf(ValueSetID, ObjectsOfSet&&);
    const ObjectsOfSet& objectsOfSetAt(uint32_t position) const;
    const ObjectsOfSet& objectsOfSetOfWrite(ValueSetID) const;
    void forEachLayoutListedAmong(const ObjectsOfSet&, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&) const;
    bool admits(std::span<const LayoutID> layoutsOfReceiver, LayoutID) const;
    bool admits(const Writer& writer, LayoutID layout) const { return admits(writer.layoutsOfReceiver, layout); }
    const Vector<LayoutID>& layoutsAdmittedBy(LayoutSetID familyOfReceiver);
    uint32_t familyDeclaredForReceiverOf(const Writer&) const;
    uint8_t formsOfAccessorDefinedBy(ValueSetMaker&, const Writer&, ValueFlowName) const;
    StoreUnderName describeStore(ValueSetMaker&, const Writer&, ValueFlowName) const;
    // The store is what the call before has left there: the description of the last statement and name that were looked at.
    WritesThroughSet describeWrite(ValueSetMaker&, StoreUnderName&, const Writer&, const ValueFlowNodeKey&, ValueSetID value) const;
    ParallelArray<WritesThroughSet> collectWrites(ValueSetMakers&) const;
    void noteWrites();
    void noteWritesInto(ValueSetMaker&, LayoutID, const WritesThroughSet&);
    void noteWritesThroughPlaceholder(ValueSetMaker&, StoresIntoObjects&, const WritesThroughSet&);
    void noteNamesStoredThroughDeclaredReceivers();
    Vector<LayoutID> receiversNamedBy(LayoutSetID family, uint8_t placeholders);
    // By the index of the key in the upper half and that of the layout in the lower.
    using AccessesByKeyAndLayout = HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    AccessesByKeyAndLayout countAccessesThroughTypesOfFewLayouts(const BitVector& keys, uint64_t mostLayouts);
    void unite(ValueSetID& into, ValueSetID value) { into = m_heap.maker().unite(into, value); }
    bool spellsKey(ValueSetMaker&, const Writer&, ValueFlowName) const;
    void note(ValueSetMaker&, StoresIntoObjects&, ValueFlowName, ValueSetID value, bool isAllocation, bool isNamedByOperation);
    bool someReceiverDeclares(PropertyKeyID, const ObjectsOfSet&) const;
    ValueFlowNumbers numbersStoredBy(const Writer&, ValueFlowName) const;
    void indexNumbersInData();
    ValueFlowNumbers numbersInData(LayoutID, ValueFlowName) const;
    void noteNumbers(StoresIntoObjects&, ValueFlowName, const ValueFlowNumbers&);
    void noteCodeWithoutSolve(const Function&);
    std::optional<Vector<LayoutID, 4>> layoutsCopiedOnto(LayoutID);
    void followCopiedProperties();
    void forEachSourceOf(LayoutID, NOESCAPE const Invocable<void(const StoresIntoObjects&)> auto&) const;

    const ValueFlowResultInputs& m_inputs;
    ValueFlowHeapView& m_heap;
    ListsOfSetsByNumber m_receiversOfReads;
    Vector<Vector<PropertyKeyID>> m_keysReadFrom;
    ValueSetID m_objectsPutAboveOthers { ValueSets::nothing };
    const LayoutsOfObjects& m_layoutsOfObjects;
    Vector<uint32_t> m_setsLookedAt;
    Vector<ObjectsOfSet> m_objectsOfSetsOfWrites;
    Vector<ObjectsOfSet> m_objectsOfOtherSets;
    HashedIndex m_positionOfSetLookedAt;
    Vector<StoresIntoObjects> m_storesOfLayout;
    Vector<Vector<LayoutID>> m_layoutsExpectedDirectlyBelow;
    HashMap<uint32_t, Vector<LayoutID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_layoutsAdmittedBy;
    Lock m_lockOfLayoutsAdmittedBy;
    // What is stored through the unknown object is stored into every object that has escaped, and what is stored through the untracked object into every object that the analysis has stopped following.
    StoresIntoObjects m_storesThroughUnknownObject;
    StoresIntoObjects m_storesThroughUntrackedObject;
    // Through a set that holds all of the universe but a few members.
    StoresIntoObjects m_storesIntoEveryObject;
    StoresIntoObjects m_storesIntoEveryEscapedClass;
    StoresIntoObjects m_storesIntoEveryLostClass;
    // The keys of the three, ascending: every layout with an object that has escaped asks for hundreds of them.
    Vector<PropertyKeyID> m_keysThroughUnknownObject;
    Vector<PropertyKeyID> m_keysThroughUntrackedObject;
    Vector<PropertyKeyID> m_keysIntoEveryObject;
    Vector<PropertyKeyID> m_keysIntoEveryEscapedClass;
    Vector<PropertyKeyID> m_keysIntoEveryLostClass;
    Vector<PropertyKeyID> m_keysThroughPlaceholders;
    BitVector m_hasEscapedObject;
    BitVector m_hasLostObject;
    BitVector m_isEscapedClass;
    BitVector m_isLostClass;
    Vector<NameStoredThroughDeclaredReceiver> m_namesStoredThroughDeclaredReceivers;
    // By function. Empty where no store of the function asks.
    Vector<BitVector> m_valuesBehindChecks;
    Vector<uint8_t> m_hasChecksNotLookedAt;
    // Every write of a statement that has run, in the order of the statements: the statement, marked, then what ValueFlowSolver::indexOfMadeNodes() says of each node it writes to.
    Vector<uint32_t> m_writes;
    BitVector m_hasObjectOfCodeWithoutSolve;
    BitVector m_keysDeclaredByEscapedClasses;
    BitVector m_keysDeclaredByLostClasses;
    ValueSetID m_unknownValue;
    // By layout and key: the numbers that the constant data of the program lists. Made when the first operation that creates data is looked at.
    std::optional<HashMap<uint64_t, ValueFlowNumbers, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_numbersInData;
};


// Who looks at the objects in ways that no mark records.
class UsesOfObjects {
public:
    UsesOfObjects(const ValueFlowResultInputs&, const ValueFlowHeapView&);

    // Somebody may list the keys of one of the objects.
    bool mayBeEnumerated(ValueSetID objects) const { return m_inputs.sets().meet(objects, m_enumerated); }
    ValueSetID objectsThatMayBeEnumerated() const { return m_enumerated; }
    // Somebody may read what lies under the key in one of the objects, or find out whether anything does. A read that mayBeCalledUnder() answers for does not count.
    bool mayBeObservedUnder(ValueSetID objects, PropertyKeyID) const;
    // An operation of emitted code reads by name what lies under the key in one of the objects, which is among its receivers, and does nothing with it but call it.
    bool mayBeCalledUnder(ValueSetID objects, PropertyKeyID key) const { return m_inputs.sets().meet(objects, setAt(m_calledUnderKey, key.index())); }
    // The same of an operation that does nothing with what it reads but read and store properties of it by name. mayBeObservedUnder() does not count that read either.
    bool mayBeAccessedUnder(ValueSetID objects, PropertyKeyID key) const { return m_inputs.sets().meet(objects, setAt(m_accessedUnderKey, key.index())); }
    // By key, the objects that a read by that name lists among its receivers.
    ListsOfSetsByNumber takeReceiversOfReads() { return std::exchange(m_receiversOfReads, { }); }

private:
    // Who has asked for a node that reads.
    enum class Reader : uint8_t {
        TakesValue = 1 << 0,
        OnlyCalls = 1 << 1,
        OnlyAccesses = 1 << 2,
    };

    // What one thread of a loop has noted. All of it is united in the end, so it makes no difference which thread notes a part, and when.
    struct Notes {
        UnionOfSets enumerated;
        UnionOfSets observedUnderAnyKey;
        PartsByNumber observedUnderKey;
        // The objects at which a test starts that goes on to what lies above them.
        PartsByNumber startsOfTestsUnderKey;
        PartsByNumber calledUnderKey;
        PartsByNumber accessedUnderKey;
        PartsByNumber receiversOfReads;
    };

    // A thread of a loop: where it notes, and what it makes sets with.
    struct Noter {
        Notes& notes;
        ValueSetMaker& maker;
    };

    // `worker` is what forEachIndexInParallel() hands to the body of the loop.
    Noter noterOf(ValueSetMakers& makers, unsigned worker) { return { m_notes[worker], makers.of(worker) }; }
    UnionOfSets takeUnion(UnionOfSets Notes::*);
    Vector<PartOfNumber> takeParts(PartsByNumber Notes::*);
    SetsByNumber takeExpanded(ValueSetMakers&, Vector<PartOfNumber>&&);
    void noteReads(ValueSetMakers&);
    void noteRead(Noter, const ValueFlowNodeKey&, Reader, ValueSetID startOfLookup);
    void noteOperations(ValueSetMakers&);
    void noteOperationsWithoutStatement();
    void noteBehaviors();
    void noteObjectsOfRuntimeModules();
    void noteApplication(Noter, const ValueFlowBuiltinApplication&);
    void noteRefusedOperands(Noter, const ValueFlowBuiltinApplication&);

    struct NotesOfBuiltins {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(NotesOfBuiltins);

        struct TestUnderName {
            BuiltinOperand object;
            uint32_t key;
            bool looksAtOwnPropertyAlone;

            friend bool operator==(const TestUnderName&, const TestUnderName&) = default;
        };
        struct TestUnderOperand {
            BuiltinOperand object;
            BuiltinOperand key;
            bool looksAtOwnPropertyAlone;

            friend bool operator==(const TestUnderOperand&, const TestUnderOperand&) = default;
        };
        struct RefusedOperand {
            BuiltinOperand operand;
            ValueSetID taken;
        };

        bool areToldApart { false };
        bool enumeratesAnything { false };
        Vector<BuiltinOperand, 4> enumerated;
        Vector<BuiltinOperand, 4> observedUnderAnyKey;
        Vector<TestUnderName, 4> testsUnderNames;
        Vector<TestUnderOperand, 2> testsUnderOperands;
        Vector<RefusedOperand, 4> refusedOperands;
    };
    class NotesOfSetsOfBuiltins {
    public:
        const NotesOfBuiltins& ensure(uint64_t question, NOESCAPE const Invocable<std::unique_ptr<NotesOfBuiltins>()> auto& find)
        {
            Part& part = m_parts[(question * 0x9e3779b97f4a7c15ull) >> 60];
            {
                Locker locker { part.lock };
                auto found = part.notes.find(question);
                if (found != part.notes.end())
                    return *found->value;
            }
            std::unique_ptr<NotesOfBuiltins> notes = find();
            Locker locker { part.lock };
            return *part.notes.add(question, WTF::move(notes)).iterator->value;
        }

    private:
        struct alignas(64) Part {
            Lock lock;
            HashMap<uint64_t, std::unique_ptr<NotesOfBuiltins>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> notes WTF_GUARDED_BY_LOCK(lock);
        };

        std::array<Part, 16> m_parts;
    };
    std::unique_ptr<NotesOfBuiltins> findNotesOf(ValueSetMaker&, ValueSetID builtinFunctions, BuiltinCallMode) const;
    void noteApplicationOfAll(Noter, const ValueFlowBuiltinApplication&, ValueSetID builtinCallees);
    void noteTestUnderKeys(Noter, const ValueFlowBuiltinApplication&, BuiltinOperand object, BuiltinOperand key, bool looksAtOwnPropertyAlone);
    void findHostFunctionsThatProgramHasStored(ValueSetMakers&);
    void noteParameter(Noter, const ValueFlowBuiltinApplication&, const BuiltinParameter&);
    void noteParameterWithoutAction(Noter, const ValueFlowBuiltinApplication&, const BuiltinOverload&, const BuiltinParameter&);
    void noteTest(Noter, const ValueFlowBuiltinApplication&, const BuiltinOverload&, const BuiltinPropertyAction&);
    void goOnToPrototypes(ValueSetMakers&);
    void findObjectsObservedUnderKeys(ValueSetMakers&);
    void listReceiversOfReads();

    const ValueFlowResultInputs& m_inputs;
    const ValueFlowHeapView& m_heap;
    ValueSetID m_enumerated { ValueSets::nothing };
    ValueSetID m_observedUnderAnyKey { ValueSets::nothing };
    SetsByNumber m_observedUnderKey;
    SetsByNumber m_calledUnderKey;
    SetsByNumber m_accessedUnderKey;
    ListsOfSetsByNumber m_receiversOfReads;
    // By worker. Empty once the unions are made.
    Vector<Notes> m_notes;
    ValueFlowAnswersAboutSets m_foundAboveReceivers;
    NotesOfSetsOfBuiltins m_notesOfSetsOfBuiltins;
    BitVector m_builtinsThatLookAtObjects;
    // By the key under which some call finds its callee: the functions of the host that refuse a receiver and that the program has stored under the key somewhere. No entry where there is none.
    SetsByNumber m_hostFunctionsStoredUnderKey;
};

struct ValueFlowUsesOfHeap {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValueFlowUsesOfHeap);

    explicit ValueFlowUsesOfHeap(const ValueFlowResultInputs& inputs)
        : heap(inputs)
        , uses(inputs, heap)
    {
    }

    ValueFlowHeapView heap;
    UsesOfObjects uses;
    std::optional<LayoutsOfObjects> layoutsOfObjects;
    std::optional<StoresByLayout> stores;
};

using ValueFlowBuiltinApplicationOnWorkerFunctor = ScopedLambda<void(unsigned worker, const ValueFlowBuiltinApplication&)>;
using ValueFlowBuiltinApplicationsOnWorkerFunctor = ScopedLambda<void(unsigned worker, const ValueFlowBuiltinApplication&, ValueSetID builtinCallees)>;
// The same applications, handed out on all threads and in no order. `worker` is what forEachIndexInParallel() hands to the body of a loop.
void forEachBuiltinApplicationInParallel(const ValueFlowResultInputs&, const ValueFlowHeapView&, ValueSetMakers&, const BitVector& onlyOfBuiltins, const ValueFlowBuiltinApplicationOnWorkerFunctor&, const ValueFlowBuiltinApplicationsOnWorkerFunctor&);

// What is collected comes in the order of the groups.
template<typename T>
inline Vector<T> collectFromCallGroups(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, const Invocable<void(const ValueFlowNodeKey&, ValueFlowNodeID first, Vector<T>&)> auto& collect)
{
    std::span<const ValueFlowNodeID> firstNodes = heap.index().firstNodesOfCallGroups();
    return collectInParallel<T>(inputs.loops(), firstNodes.size(), [&](unsigned position, Vector<T>& collected) {
        collect(*inputs.solver().keyOfMadeNode(firstNodes[position]), firstNodes[position], collected);
    });
}

inline bool haveCommonMember(std::span<const LayoutID> ascending, std::span<const LayoutID> otherAscending)
{
    if (ascending.size() > otherAscending.size())
        std::swap(ascending, otherAscending);
    return std::ranges::any_of(ascending, [&](LayoutID layout) {
        return std::ranges::binary_search(otherAscending, layout);
    });
}

inline void uniteSortedKeys(Vector<PropertyKeyID>& keys, const Vector<PropertyKeyID>& others)
{
    if (others.isEmpty())
        return;
    Vector<PropertyKeyID> united;
    united.reserveInitialCapacity(keys.size() + others.size());
    size_t index = 0;
    for (PropertyKeyID other : others) {
        while (index < keys.size() && keys[index] < other)
            united.append(keys[index++]);
        if (index < keys.size() && keys[index] == other)
            ++index;
        united.append(other);
    }
    united.append(keys.subspan(index));
    keys = WTF::move(united);
}

// The span holds all the writes of each of its statements.
void StoresByLayout::forEachWriteAmong(std::span<const uint32_t> writes, NOESCAPE const Invocable<void(const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto& functor) const
{
    const Program& program = m_inputs.program;
    const ValueFlowSolver& solver = m_inputs.solver();
    std::span<const ValueFlowStatement> statements = m_inputs.bodies().statements();
    for (size_t first = 0; first < writes.size();) {
        ValueFlowStatementID id(writes[first++] & ~isStatementInListOfWrites);
        uint32_t family = writes[first++];
        const ValueFlowStatement& statement = id.index() < statements.size() ? statements[id.index()] : m_inputs.solver().statementAt(id);
        Writer writer { statement };
        if (statement.kind == ValueFlowStatementKind::Operation)
            writer.isAllocation = resultRuleOf(statement.opcode) == ResultRule::Allocation || statement.opcode == Opcode::Data;
        if (family) {
            writer.familyOfReceiver = LayoutSetID(family - 1);
            writer.layoutsOfReceiver = program.types.at(writer.familyOfReceiver).members.span();
        }
        for (; first < writes.size() && !(writes[first] & isStatementInListOfWrites); ++first)
            functor(writer, solver.keyOfMadeNodesAt(writes[first]), solver.valueOf(solver.firstOfMadeNodesAt(writes[first])));
    }
}

void StoresByLayout::forEachWriteInParallel(NOESCAPE const Invocable<void(unsigned worker, const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto& functor) const
{
    constexpr size_t writesPerIndex = 2048;
    std::span<const uint32_t> writes = m_writes.span();
    // A run begins with the first statement that begins in it.
    auto firstOfRun = [&](size_t run) {
        size_t first = std::min(run * writesPerIndex, writes.size());
        while (first < writes.size() && !(writes[first] & isStatementInListOfWrites))
            ++first;
        return first;
    };
    forEachIndexInParallel(m_inputs.loops(), static_cast<unsigned>((writes.size() + writesPerIndex - 1) / writesPerIndex), [&](unsigned run, unsigned worker) {
        size_t first = firstOfRun(run);
        forEachWriteAmong(writes.subspan(first, firstOfRun(run + 1) - first), [&](const Writer& writer, const ValueFlowNodeKey& key, ValueSetID value) {
            functor(worker, writer, key, value);
        });
    });
}

void StoresByLayout::forEachLayoutWrittenThroughSetOfWrite(ValueSetID set, const Writer& writer, NOESCAPE const Invocable<void(LayoutID)> auto& functor)
{
    PlaceholdersInSet placeholders = placeholdersInSetOfWrite(set);
    bool holdsPlaceholder = placeholders.almostEverything || placeholders.unknownObject || placeholders.untrackedObject;
    auto isReached = [&](uint32_t index) {
        return placeholders.almostEverything || (placeholders.unknownObject && m_hasEscapedObject.quickGet(index)) || (placeholders.untrackedObject && m_hasLostObject.quickGet(index));
    };
    if (holdsPlaceholder && writer.layoutsOfReceiver.empty()) {
        for (uint32_t index = 0; index < m_inputs.program.layouts.size(); ++index) {
            if (isReached(index))
                functor(LayoutID(index));
        }
    } else if (holdsPlaceholder) {
        // The list lies in a table that moves when another family comes in.
        Vector<LayoutID> admitted;
        {
            Locker locker { m_lockOfLayoutsAdmittedBy };
            admitted = layoutsAdmittedBy(writer.familyOfReceiver);
        }
        for (LayoutID layout : admitted) {
            if (isReached(layout.index()))
                functor(layout);
        }
    }
    forEachLayoutListedInSetOfWrite(set, writer, functor);
}

void StoresByLayout::forEachLayoutListedInSetOfWrite(ValueSetID set, const Writer& writer, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
{
    forEachLayoutListedAmong(objectsOfSetOfWrite(set), writer, functor);
}

void StoresByLayout::forEachLayoutListedAmong(const ObjectsOfSet& objects, const Writer& writer, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
{
    for (LayoutID layout : objects.layouts) {
        if (admits(writer, layout))
            functor(layout);
    }
    if (objects.holdsObjectOfEveryEscapedClass) {
        for (LayoutID layout : m_layoutsOfObjects.escapedClasses()) {
            if (!std::ranges::binary_search(objects.layouts, layout) && admits(writer, layout))
                functor(layout);
        }
    }
    if (objects.holdsObjectOfEveryLostClass) {
        for (LayoutID layout : m_layoutsOfObjects.lostClasses()) {
            bool isNamedAlready = std::ranges::binary_search(objects.layouts, layout) || (objects.holdsObjectOfEveryEscapedClass && std::ranges::binary_search(m_layoutsOfObjects.escapedClasses(), layout));
            if (!isNamedAlready && admits(writer, layout))
                functor(layout);
        }
    }
}

void StoresByLayout::forEachSourceOf(LayoutID layout, NOESCAPE const Invocable<void(const StoresIntoObjects&)> auto& functor) const
{
    functor(m_storesOfLayout[layout.index()]);
    functor(m_storesIntoEveryObject);
    if (m_isEscapedClass.quickGet(layout.index()))
        functor(m_storesIntoEveryEscapedClass);
    if (m_isLostClass.quickGet(layout.index()))
        functor(m_storesIntoEveryLostClass);
    if (m_hasEscapedObject.quickGet(layout.index()))
        functor(m_storesThroughUnknownObject);
    if (m_hasLostObject.quickGet(layout.index()))
        functor(m_storesThroughUntrackedObject);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
