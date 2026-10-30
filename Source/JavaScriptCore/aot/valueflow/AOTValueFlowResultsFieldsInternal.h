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
#include "AOTLedgerFields.h"
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

namespace JSC { namespace AOT { namespace Typed {

inline constexpr Protocols protocolsListingKeys { Protocol::OwnKeys, Protocol::OwnValues, Protocol::ToJSON };
// Which own keys of an operand the program gets to see, in their order.
enum class ShownKeys : uint8_t { None, Enumerable, All };

inline constexpr OptionSet<ObjectMutation> mutationsThatReach { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript };

inline bool hasNodeOfItsOwn(const Field& field)
{
    return field.flags.containsAny({ FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden });
}

inline uint64_t numberOf(ValueFlowName name)
{
    return static_cast<uint64_t>(name.kind) << 32 | name.payload;
}

inline bool isCompiledWithoutSolve(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, FunctionID id)
{
    return isUnsolvedCode(inputs, tables, id);
}

inline LayoutID layoutExpectedAbove(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    return layout.isDerived() ? layout.flow.base : predictedHeritageOf(layout);
}

using SetsByNumber = HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

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

    ValueSetID takeExpanded(const ValueFlowHeapView& heap, ValueSetMaker& maker) { return heap.expand(maker, take(maker)); }

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

class LayoutsOfObjects {
public:
    LayoutsOfObjects(const ValueFlowResultInputs&, ValueFlowHeapView&);

    // Each layout once.
    void forEachLayoutOf(ValueFlowObjectID object, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        LayoutID named = namedLayoutOf(object);
        if (!named)
            return;
        functor(named);
        auto selected = m_selectedLayouts.find(object.index());
        if (selected == m_selectedLayouts.end())
            return;
        for (LayoutID other : m_listsOfSelectedLayouts[selected->value.list]) {
            if (other != named)
                functor(other);
        }
        forEachLayoutIn(SharedLists { selected->value.everyEscapedClass, selected->value.everyLostClass }, functor);
    }

    // A layout may come more than once.
    void forEachLayoutOfMembers(ValueSetID objects, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        forEachLayoutIn(forEachLayoutListedForMembers(objects, functor), functor);
    }

    struct SharedLists {
        bool everyEscapedClass { false };
        bool everyLostClass { false };
    };

    // A layout may come more than once. A list that many members share is walked once.
    SharedLists forEachLayoutListedForMembers(ValueSetID objects, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        Vector<uint32_t, 4> lists;
        SharedLists shared = forEachLayoutNamedByMembers(objects, lists, functor);
        for (uint32_t list : lists) {
            for (LayoutID layout : m_listsOfSelectedLayouts[list])
                functor(layout);
        }
        return shared;
    }

    // What each member names by itself, which may come more than once, and the lists that the members select from, each once.
    SharedLists forEachLayoutNamedByMembers(ValueSetID objects, Vector<uint32_t, 4>& lists, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
        SharedLists shared;
        m_inputs.sets().forEachMemberInLine(objects, [&](ValueFlowObjectID object) {
            LayoutID named = namedLayoutOf(object);
            if (!named)
                return;
            functor(named);
            auto selected = m_selectedLayouts.find(object.index());
            if (selected == m_selectedLayouts.end())
                return;
            if (lists.isEmpty() || lists.last() != selected->value.list)
                lists.append(selected->value.list);
            shared.everyEscapedClass |= selected->value.everyEscapedClass;
            shared.everyLostClass |= selected->value.everyLostClass;
        });
        AOT::sort(lists);
        removeRepeatedElements(lists);
        return shared;
    }

    std::span<const LayoutID> listOfSelectedLayouts(uint32_t list) const LIFETIME_BOUND { return m_listsOfSelectedLayouts[list].span(); }

    std::span<const LayoutID> escapedClasses() const LIFETIME_BOUND { return m_escapedClasses.span(); }
    std::span<const LayoutID> lostClasses() const LIFETIME_BOUND { return m_lostClasses.span(); }

    ValueSetID objectsWith(LayoutID layout) const { return m_objectsOfLayout[layout.index()]; }

private:
    struct SelectedLayouts {
        uint32_t list { 0 };
        bool everyEscapedClass { false };
        bool everyLostClass { false };
    };

    void forEachLayoutIn(SharedLists lists, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const
    {
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

    const ValueFlowResultInputs& m_inputs;
    HashMap<uint32_t, SelectedLayouts, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_selectedLayouts;
    // The classes whose prototype objects an allocation may be given, in order: one list for all the allocations that may be given the same ones.
    Vector<Vector<LayoutID>> m_listsOfSelectedLayouts;
    Vector<LayoutID> m_escapedClasses;
    Vector<LayoutID> m_lostClasses;
    Vector<ValueSetID> m_objectsOfLayout;
};

struct StoredUnderKey {
    ValueSetID value { ValueSets::nothing };
    ValueSetID valueFromOperations { ValueSets::nothing };
    ValueSetID getters { ValueSets::nothing };
    ValueSetID setters { ValueSets::nothing };
    bool isStoredAfterCreation : 1 { false };
    bool mayBeAccessor : 1 { false };
    bool isGivenValueByOperation : 1 { false };
    bool isGivenValueByDefinition : 1 { false };
    // `object.key = value`, which first checks the prototype chain for a setter. Any other write under the key may define it on the object itself, whatever the chain holds.
    bool isAssigned : 1 { false };
    bool isWrittenOtherThanByAssignment : 1 { false };
    bool isNamedByOperation : 1 { false };
    bool isStoredByConstructor : 1 { false };
    bool mayBeOtherThanIntegers : 1 { false };
    // Some store passes on the result of a field read, and that field's numbers have not been added yet: StoresIntoObjects::readsOfFields.
    bool waitsForNumbersOfFields : 1 { false };
    uint8_t accessorForms { 0 };
    // The forms in which calls that take a descriptor define a value under the key: ValueForms.
    uint8_t formsOfDefinedValues { 0 };
    OptionSet<Actor> writers;
    Range integers { Range::empty() };

    ValueFlowNumbers numbers() const { return waitsForNumbersOfFields ? ValueFlowNumbers::any() : numbersAddedSoFar(); }
    ValueFlowNumbers numbersAddedSoFar() const { return { mayBeOtherThanIntegers, integers }; }

    void uniteNumbers(const ValueFlowNumbers& other)
    {
        mayBeOtherThanIntegers |= other.mayBeOtherThanIntegers;
        integers = integers.unionWith(other.integers);
    }
};
static_assert(sizeof(StoredUnderKey) == 40);

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

// `Object.defineProperty(object, "name", { value, enumerable: false })`
struct ValueForms {
    static constexpr uint8_t unknown = 0xff;

    static constexpr uint8_t of(bool isWritable, bool isEnumerable, bool isConfigurable) { return 1 << ((isWritable ? 0 : 4) + (isEnumerable ? 0 : 2) + (isConfigurable ? 0 : 1)); }

    // `made.name ?? (made.name = { })` next to a definition: the definition is assumed to create the property.
    static std::optional<OptionSet<FieldForm>> formToPlanFor(uint8_t forms)
    {
        constexpr uint8_t plain = of(true, true, true);
        return onlyFormIn(forms == plain ? forms : forms & ~plain);
    }

    static std::optional<OptionSet<FieldForm>> onlyFormIn(uint8_t forms)
    {
        for (bool isWritable : { true, false }) {
            for (bool isEnumerable : { true, false }) {
                for (bool isConfigurable : { true, false }) {
                    if (forms != of(isWritable, isEnumerable, isConfigurable))
                        continue;
                    OptionSet<FieldForm> form;
                    if (!isWritable)
                        form.add(FieldForm::IsReadOnly);
                    if (!isEnumerable)
                        form.add(FieldForm::IsNotEnumerable);
                    if (!isConfigurable)
                        form.add(FieldForm::IsNotConfigurable);
                    return form;
                }
            }
        }
        return std::nullopt;
    }
};

inline constexpr uint64_t noCopyLeavingOutKeys = UINT64_MAX;

// `target.port = source.port`: what is stored under one key is what the objects hold under another.
struct ReadOfFields {
    uint32_t storedUnder;
    uint32_t key;
    ValueSetID objects;
};

struct StoresIntoObjects {
    StoresUnderKeys underKey;
    Vector<ReadOfFields> readsOfFields;
    ValueSetID underUnknownKey { ValueSets::nothing };
    ValueFlowNumbers numbersUnderUnknownKey;
    bool isStoredUnderIndex { false };
    bool getsAccessorUnderUnknownKey { false };
    bool getsCopiedProperties { false };
    bool getsCopyWithoutComputedKey { false };
    // `const { secret, ...visible } = record`: the names that every copy into such an object leaves out.
    std::optional<Vector<PropertyKeyID, 2>> keysLeftOutOfEveryCopy;
    ValueSetID valueFromOperationsUnderUnknownKey { ValueSets::nothing };
    bool isGivenValueByOperationUnderUnknownKey { false };
    ValueSetID gettersUnderUnknownKey { ValueSets::nothing };
    uint8_t formsOfAccessorsUnderUnknownKey { 0 };
    // Who wrote under a name that nobody lists: an unknown key, an accessor under one, a copy.
    OptionSet<Actor> writersUnderNamesNobodyLists;

    bool getsNameNobodyLists() const { return underUnknownKey != ValueSets::nothing || isStoredUnderIndex || getsAccessorUnderUnknownKey || getsCopiedProperties; }

    // Every member but `underKey`.
    void uniteWhatLiesOutsideKeys(ValueSetMaker& maker, const StoresIntoObjects& other)
    {
        underUnknownKey = maker.unite(underUnknownKey, other.underUnknownKey);
        numbersUnderUnknownKey.unite(other.numbersUnderUnknownKey);
        isStoredUnderIndex |= other.isStoredUnderIndex;
        getsAccessorUnderUnknownKey |= other.getsAccessorUnderUnknownKey;
        getsCopiedProperties |= other.getsCopiedProperties;
        getsCopyWithoutComputedKey |= other.getsCopyWithoutComputedKey;
        if (other.keysLeftOutOfEveryCopy && !keysLeftOutOfEveryCopy)
            keysLeftOutOfEveryCopy = other.keysLeftOutOfEveryCopy;
        else if (other.keysLeftOutOfEveryCopy) {
            keysLeftOutOfEveryCopy->removeAllMatching([&](PropertyKeyID key) {
                return !other.keysLeftOutOfEveryCopy->contains(key);
            });
        }
        valueFromOperationsUnderUnknownKey = maker.unite(valueFromOperationsUnderUnknownKey, other.valueFromOperationsUnderUnknownKey);
        isGivenValueByOperationUnderUnknownKey |= other.isGivenValueByOperationUnderUnknownKey;
        gettersUnderUnknownKey = maker.unite(gettersUnderUnknownKey, other.gettersUnderUnknownKey);
        formsOfAccessorsUnderUnknownKey |= other.formsOfAccessorsUnderUnknownKey;
        writersUnderNamesNobodyLists.add(other.writersUnderNamesNobodyLists);
    }
};

struct Writer {
    const ValueFlowStatement& statement;
    bool isAllocation { false };
    std::span<const LayoutID> layoutsOfReceiver;
    LayoutSetID familyOfReceiver;
};

// A store through a placeholder reaches the objects of a layout that escaped or were lost, and none that the analysis still follows.
enum class ObjectsOfLayout : uint8_t { All, Followed };

class StoresByLayout {
public:
    StoresByLayout(const ValueFlowResultInputs&, const FlowResultTables&, ValueFlowHeapView&, const LayoutsOfObjects&);
    void noteListedWrites();

    ValueSetID storedUnder(LayoutID layout, PropertyKeyID key) { return storedUnder(m_heap.maker(), layout, key); }
    ValueSetID storedUnder(ValueSetMaker&, LayoutID, PropertyKeyID) const;
    ValueSetID storedUnderExactName(ValueSetMaker&, LayoutID, PropertyKeyID) const;
    ValueFlowNumbers numbersStoredUnder(LayoutID, PropertyKeyID, ObjectsOfLayout = ObjectsOfLayout::All) const;
    bool isStoredAfterCreation(LayoutID, PropertyKeyID) const;
    OptionSet<Actor> writersSeenUnder(LayoutID, PropertyKeyID) const;
    bool getsCopiedProperties(LayoutID, ObjectsOfLayout = ObjectsOfLayout::All) const;
    bool getsNameNobodyLists(LayoutID) const;
    Vector<PropertyKeyID> keysStoredInto(LayoutID, ObjectsOfLayout = ObjectsOfLayout::All) const;
    Vector<PropertyKeyID> keysStoredIntoKnownObjects(LayoutID) const;
    bool isOnlyAssignedByStatements(LayoutID, PropertyKeyID) const;
    Vector<PropertyKeyID> keysNamedByStoresIntoKnownObjects(LayoutID) const;
    bool isStoredByConstructor(LayoutID, PropertyKeyID) const;
    const Vector<PropertyKeyID>& keysStoredThroughPlaceholders() const LIFETIME_BOUND { return m_keysThroughPlaceholders; }
    const Vector<PropertyKeyID>& keysDefinedOnObjectsWithoutKnownLayout() const LIFETIME_BOUND { return m_keysDefinedOnObjectsWithoutKnownLayout; }
    bool isReachedThroughPlaceholders(LayoutID layout) const
    {
        return hasEscapedObject(layout) || hasLostObject(layout) || !m_keysIntoEveryObject.isEmpty()
            || (m_isEscapedClass.quickGet(layout.index()) && !m_keysIntoEveryEscapedClass.isEmpty())
            || (m_isLostClass.quickGet(layout.index()) && !m_keysIntoEveryLostClass.isEmpty());
    }
    bool mayGetAccessorUnder(LayoutID, PropertyKeyID, ObjectsOfLayout = ObjectsOfLayout::All) const;
    FlowAccessorField accessorDefinedUnder(LayoutID layout, PropertyKeyID key) const { return accessorDefinedUnder(m_heap.maker(), layout, key); }
    FlowAccessorField accessorDefinedUnder(ValueSetMaker&, LayoutID, PropertyKeyID) const;
    Vector<PropertyKeyID> keysOfAccessorsDefinedIn(ValueSetMaker&, LayoutID) const;
    // ValueForms: the forms in which the objects are observed to get a value under the key. A store, a literal and a class field create the plain form.
    uint8_t formsOfValuesGivenUnder(LayoutID, PropertyKeyID) const;
    Vector<PropertyKeyID> keysOfValuesDefinedIn(LayoutID) const;
    // ValueForms: the forms in which the call defines a value, if it is an `Object.defineProperty(object, key, descriptor)`.
    uint8_t formsOfValueDefinedByCall(ValueSetMaker&, const ValueFlowStatement&) const;
    bool getsAccessorsUnderKeysNobodyKnows(LayoutID) const;
    void setReceiversOfReads(ListsOfSetsByNumber&&);
    void noteKeyBroughtByCopies(LayoutID target, PropertyKeyID, ValueSetID value);
    bool isAnyKeyReadFrom(LayoutID layout) const { return !m_groupsOfKeysReadFrom[layout.index()].isEmpty(); }
    // Sorted. Made where it is asked for: see m_groupsOfKeysReadFrom.
    Vector<PropertyKeyID> keysReadFrom(LayoutID) const;

    bool hasEscapedObject(LayoutID layout) const { return m_hasEscapedObject.quickGet(layout.index()); }
    bool hasLostObject(LayoutID layout) const { return m_hasLostObject.quickGet(layout.index()); }
    bool hasObjectOfCodeWithoutSolve(LayoutID layout) const { return m_hasObjectOfCodeWithoutSolve.quickGet(layout.index()); }

    void forEachWriteInParallel(NOESCAPE const Invocable<void(unsigned worker, const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto&) const;
    void forEachLayoutWrittenThroughSetOfWrite(ValueSetID objects, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&);

    struct PlaceholdersInSet {
        bool unknownObject { false };
        bool untrackedObject { false };
        bool almostEverything { false };
    };
    void lookAt(std::span<const ValueSetID>);
    PlaceholdersInSet placeholdersInSetOfWrite(ValueSetID objects) const;
    void forEachLayoutListedInSetOfWrite(ValueSetID objects, const Writer& writer, NOESCAPE const Invocable<void(LayoutID)> auto& functor) const { forEachLayoutListedAmong(objectsOfSetOfWrite(objects), writer, functor); }

private:
    struct ObjectsOfSet {
        Vector<LayoutID, 4> layouts;
        bool holdsObjectOfEveryEscapedClass { false };
        bool holdsObjectOfEveryLostClass { false };
        bool holdsUnknownObject { false };
        bool holdsUntrackedObject { false };
        bool holdsAlmostEverything { false };
        bool holdsObjectWithoutLayout { false };
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
        bool isStoredByConstructor;
        bool isWrittenByOperation;
        bool givesValueItself;
        bool isWrittenByCallOfBuiltin;
        bool definesValue;
        bool isAssignment;
        bool isOtherThanAssignment;
        bool leavesOutComputedKey;
        uint64_t copyLeavingOutKeys;
        uint8_t accessorForms;
        uint8_t formsOfDefinedValues;
        OptionSet<Actor> writers;
        ValueSetID value;
        ValueFlowNumbers numbers;
        // The numbers are those that these objects hold under the key, and `numbers` is empty.
        ValueSetID objectsRead;
        uint32_t keyRead;

        ValueFlowName name() const { return { static_cast<ValueFlowNameKind>(detail), payload }; }
        ValueFlowNumbers numbersWithoutFollowedReads() const { return objectsRead == ValueSets::nothing ? numbers : ValueFlowNumbers::any(); }
        auto group() const { return std::tuple { set, detail, payload, familyOfReceiver, familyDeclaredForReceiver, fillsTypedResult, createsData, copyLeavingOutKeys, objectsRead.index(), keyRead }; }
        uint32_t hashOfGroup() const
        {
            uint64_t hash = (static_cast<uint64_t>(payload) << 32 | set) * 0x9e3779b97f4a7c15ull;
            hash = (hash ^ hash >> 32 ^ (static_cast<uint64_t>(familyOfReceiver) << 32 | familyDeclaredForReceiver)) * 0xbf58476d1ce4e5b9ull;
            hash = (hash ^ hash >> 32 ^ static_cast<uint64_t>(detail << 2 | fillsTypedResult << 1 | createsData)) * 0x94d049bb133111ebull;
            return static_cast<uint32_t>(hash >> 32);
        }
    };

    struct StoreUnderName {
        const ValueFlowStatement* statement { nullptr };
        ValueFlowName name;
        bool spellsKey { false };
        bool isIntoThisOfConstructor { false };
        bool definesValue { false };
        uint8_t accessorForms { 0 };
        uint8_t formsOfDefinedValues { 0 };
        uint32_t familyDeclaredForReceiver { UINT32_MAX };
        std::optional<ValueFlowNumbers> numbers;
        ValueSetID objectsRead { ValueSets::nothing };
        uint32_t keyRead { 0 };
    };

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
    // `layouts` of a set whose members select from lists, in its parts: the lists, and what the members name that is in none of them.
    struct PartsOfLayouts {
        uint32_t position { 0 };
        Vector<LayoutID> named;
        Vector<uint32_t, 4> lists;
    };

    ObjectsOfSet findObjectsOf(ValueSetID, PartsOfLayouts* = nullptr) const;
    std::optional<uint32_t> positionOfSetLookedAt(ValueSetID) const;
    const ObjectsOfSet& objectsOfSetOfWrite(ValueSetID) const;
    void forEachLayoutListedAmong(const ObjectsOfSet&, const Writer&, NOESCAPE const Invocable<void(LayoutID)> auto&) const;
    bool admits(std::span<const LayoutID> layoutsOfReceiver, LayoutID) const;
    bool admits(const Writer& writer, LayoutID layout) const { return admits(writer.layoutsOfReceiver, layout); }
    const Vector<LayoutID>& layoutsAdmittedBy(LayoutSetID familyOfReceiver);
    uint32_t familyDeclaredForReceiverOf(const Writer&) const;
    std::optional<ValueSetID> descriptorsWrittenOutIn(ValueSetMaker&, const ValueFlowStatement&) const;
    uint8_t formsOfAccessorDefinedBy(ValueSetMaker&, const Writer&, ValueFlowName) const;
    bool definesValue(ValueSetMaker&, const Writer&, ValueFlowName) const;
    WritesThroughSet describeWrite(ValueSetMaker&, StoreUnderName&, const Writer&, const ValueFlowNodeKey&, ValueSetID value) const;
    ParallelArray<WritesThroughSet> collectWrites(ValueSetMakers&) const;
    void noteWrites();
    void noteWritesInto(ValueSetMaker&, StoresIntoObjects&, LayoutID, const WritesThroughSet&);
    static void noteWriters(StoresIntoObjects&, const WritesThroughSet&);
    std::span<const uint32_t> sharedStoresOf(LayoutID layout) const LIFETIME_BOUND
    {
        if (m_firstSharedStoresOfLayout.isEmpty())
            return { };
        uint32_t first = m_firstSharedStoresOfLayout[layout.index()];
        return m_sharedStoresOfLayouts.subspan(first, m_firstSharedStoresOfLayout[layout.index() + 1] - first);
    }
    void forEachTableOfKnownObjects(LayoutID, NOESCAPE const Invocable<void(const StoresIntoObjects&)> auto&) const;
    bool hasAllocationListing(LayoutID, PropertyKeyID) const;
    void noteNamesStoredThroughDeclaredReceivers();
    using AccessesByKeyAndLayout = HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
    void unite(ValueSetID& into, ValueSetID value) { into = m_heap.maker().unite(into, value); }
    bool spellsKey(ValueSetMaker&, const Writer&, ValueFlowName) const;
    void note(ValueSetMaker&, StoresIntoObjects&, ValueFlowName, ValueSetID value, bool isAllocation, bool isNamedByOperation);
    ValueFlowNumbers numbersStoredBy(const Writer&, ValueFlowName) const;
    ValueFlowNumbers numbersInData(LayoutID, ValueFlowName) const;
    void noteNumbers(StoresIntoObjects&, ValueFlowName, const ValueFlowNumbers&);
    void noteCodeWithoutSolve(const Function&);
    struct CopiedObjects {
        ValueSetID objects;
        Vector<LayoutID, 4> layouts;
    };
    std::optional<CopiedObjects> objectsCopiedOnto(ValueSetMaker&, LayoutID) const;
    std::optional<CopiedObjects> withLayoutsIfFollowed(ValueSetMaker&, ValueSetID objects) const;
    void followCopiedProperties();
    std::optional<ReadOfFields> readOfFieldsStoredBy(ValueSetMaker&, const Writer&, ValueFlowName) const;
    std::optional<CopiedObjects> objectsReadUnder(ValueSetMaker&, ValueSetID objects, PropertyKeyID) const;
    void followReadsOfFields();
    void forEachSourceOf(LayoutID, ObjectsOfLayout, NOESCAPE const Invocable<void(const StoresIntoObjects&)> auto&) const;

    const ValueFlowResultInputs& m_inputs;
    ValueFlowHeapView& m_heap;
    ListsOfSetsByNumber m_receiversOfReads;
    // Objects of many layouts travel in one set, and then every key read off that set is read from each of the layouts: layouts times keys.
    // So the keys that are read from the same layouts are one group, each key in one group, and a layout lists its groups.
    Vector<Vector<PropertyKeyID>> m_groupsOfKeys;
    Vector<Vector<uint32_t>> m_groupsOfKeysReadFrom;
    ValueSetID m_objectsPutAboveOthers { ValueSets::nothing };
    const LayoutsOfObjects& m_layoutsOfObjects;
    Vector<uint32_t> m_setsLookedAt;
    Vector<ObjectsOfSet> m_objectsOfSetsOfWrites;
    Vector<ObjectsOfSet> m_objectsOfOtherSets;
    HashedIndex m_positionOfSetLookedAt;
    Vector<StoresIntoObjects> m_storesOfLayout;
    // What is stored under keys through sets that reach many layouts together, once for all of them: the objects of a layout hold what its own table says and what these say.
    Vector<StoresIntoObjects> m_storesSharedByLayouts;
    Vector<Vector<PropertyKeyID>> m_keysOfSharedStores;
    Vector<Vector<PropertyKeyID>> m_keysNamedInSharedStores;
    Vector<uint32_t> m_firstSharedStoresOfLayout;
    Vector<uint32_t> m_sharedStoresOfLayouts;
    Vector<Vector<LayoutID>> m_layoutsExpectedDirectlyBelow;
    HashMap<uint32_t, Vector<LayoutID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_layoutsAdmittedBy;
    Lock m_lockOfLayoutsAdmittedBy;
    StoresIntoObjects m_storesThroughUnknownObject;
    StoresIntoObjects m_storesThroughUntrackedObject;
    StoresIntoObjects m_storesIntoEveryObject;
    StoresIntoObjects m_storesIntoEveryEscapedClass;
    StoresIntoObjects m_storesIntoEveryLostClass;
    Vector<PropertyKeyID> m_keysThroughUnknownObject;
    Vector<PropertyKeyID> m_keysThroughUntrackedObject;
    Vector<PropertyKeyID> m_keysIntoEveryObject;
    Vector<PropertyKeyID> m_keysIntoEveryEscapedClass;
    Vector<PropertyKeyID> m_keysIntoEveryLostClass;
    Vector<PropertyKeyID> m_keysThroughPlaceholders;
    Vector<PropertyKeyID> m_keysDefinedOnObjectsWithoutKnownLayout;
    BitVector m_hasEscapedObject;
    BitVector m_hasLostObject;
    BitVector m_isEscapedClass;
    BitVector m_isLostClass;
    Vector<NameStoredThroughDeclaredReceiver> m_namesStoredThroughDeclaredReceivers;
    Vector<BitVector> m_valuesBehindChecks;
    Vector<uint8_t> m_hasChecksNotLookedAt;
    Vector<uint32_t> m_writes;
    BitVector m_hasObjectOfCodeWithoutSolve;
    BitVector m_keysDeclaredByEscapedClasses;
    BitVector m_keysDeclaredByLostClasses;
    ValueSetID m_unknownValue;
    std::optional<HashMap<uint64_t, ValueFlowNumbers, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>> m_numbersInData;
};

class UsesOfObjects {
public:
    UsesOfObjects(const ValueFlowResultInputs&, const ValueFlowHeapView&);

    bool mayBeEnumerated(ValueSetID objects) const { return m_inputs.sets().meet(objects, m_enumerated); }
    ValueSetID objectsThatMayBeEnumerated() const { return m_enumerated; }
    ValueSetID objectsWithAllKeysShown() const { return m_withAllKeysShown; }
    ValueSetID objectsWithEnumerableKeysShown() const { return m_withEnumerableKeysShown; }
    bool mayBeExportedByCommonJSModule(ValueSetID objects) const { return m_inputs.sets().meet(objects, m_exportedByCommonJSModules); }
    bool mayBeObservedUnder(ValueSetID objects, PropertyKeyID) const;
    bool mayBeCalledUnder(ValueSetID objects, PropertyKeyID key) const { return m_inputs.sets().meet(objects, setAt(m_calledUnderKey, key.index())); }
    bool mayBeAccessedUnder(ValueSetID objects, PropertyKeyID key) const { return m_inputs.sets().meet(objects, setAt(m_accessedUnderKey, key.index())); }
    ListsOfSetsByNumber takeReceiversOfReads() { return std::exchange(m_receiversOfReads, { }); }

private:
    enum class Reader : uint8_t {
        TakesValue = 1 << 0,
        OnlyCalls = 1 << 1,
        OnlyAccesses = 1 << 2,
    };

    struct Notes {
        UnionOfSets enumerated;
        UnionOfSets withAllKeysShown;
        UnionOfSets withEnumerableKeysShown;
        UnionOfSets walkedForEnumerableKeys;
        UnionOfSets observedUnderAnyKey;
        PartsByNumber observedUnderKey;
        PartsByNumber startsOfTestsUnderKey;
        PartsByNumber calledUnderKey;
        PartsByNumber accessedUnderKey;
        PartsByNumber receiversOfReads;
    };

    struct Noter {
        Notes& notes;
        ValueSetMaker& maker;
    };

    bool isFullyKnown(ValueSetID objects) const;
    void noteShownKeys(Noter, ValueSetID objects, ShownKeys, bool goesOnToPrototypes = false) const;
    void noteShownKeys(Noter, const ValueFlowBuiltinApplication&, BuiltinOperand, ShownKeys) const;
    ValueSetID followPrototypeLinks(ValueSetMaker&, ValueSetID objects) const;
    ValueSetID withTargetsOfProxies(ValueSetMaker&, ValueSetID objects) const;
    ValueSetID chainThroughProxies(ValueSetMaker&, ValueSetID objects) const;
    Noter noterOf(ValueSetMakers& makers, unsigned worker) { return { m_notes[worker], makers.of(worker) }; }
    void noteReads(ValueSetMakers&);
    void noteRead(Noter, const ValueFlowNodeKey&, Reader, ValueSetID startOfLookup);
    void noteOperations(ValueSetMakers&);
    void noteOperationsWithoutStatement();
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
        Vector<BuiltinOperand, 2> withAllKeysShown;
        Vector<BuiltinOperand, 2> withEnumerableKeysShown;
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
    void goOnToPrototypes(ValueSetMakers&);

    const ValueFlowResultInputs& m_inputs;
    const ValueFlowHeapView& m_heap;
    ValueSetID m_enumerated { ValueSets::nothing };
    ValueSetID m_withAllKeysShown { ValueSets::nothing };
    ValueSetID m_withEnumerableKeysShown { ValueSets::nothing };
    ValueSetID m_exportedByCommonJSModules { ValueSets::nothing };
    ValueSetID m_observedUnderAnyKey { ValueSets::nothing };
    SetsByNumber m_observedUnderKey;
    SetsByNumber m_calledUnderKey;
    SetsByNumber m_accessedUnderKey;
    ListsOfSetsByNumber m_receiversOfReads;
    Vector<Notes> m_notes;
    ValueFlowAnswersAboutSets m_foundAboveReceivers;
    NotesOfSetsOfBuiltins m_notesOfSetsOfBuiltins;
    BitVector m_builtinsThatLookAtObjects;
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
    std::optional<LedgerOfFields> ledgerOfFields;
};

using ValueFlowBuiltinApplicationOnWorkerFunctor = ScopedLambda<void(unsigned worker, const ValueFlowBuiltinApplication&)>;
using ValueFlowBuiltinApplicationsOnWorkerFunctor = ScopedLambda<void(unsigned worker, const ValueFlowBuiltinApplication&, ValueSetID builtinCallees)>;
void forEachBuiltinApplicationInParallel(const ValueFlowResultInputs&, const ValueFlowHeapView&, ValueSetMakers&, const BitVector& onlyOfBuiltins, const ValueFlowBuiltinApplicationOnWorkerFunctor&, const ValueFlowBuiltinApplicationsOnWorkerFunctor&);

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
            writer.layoutsOfReceiver = program.types.at(writer.familyOfReceiver).completeMembers().value();
        }
        for (; first < writes.size() && !(writes[first] & isStatementInListOfWrites); ++first)
            functor(writer, solver.keyOfMadeNodesAt(writes[first]), solver.possibleValuesOf(solver.firstOfMadeNodesAt(writes[first])));
    }
}

void StoresByLayout::forEachWriteInParallel(NOESCAPE const Invocable<void(unsigned worker, const Writer&, const ValueFlowNodeKey&, ValueSetID value)> auto& functor) const
{
    constexpr size_t writesPerIndex = 2048;
    std::span<const uint32_t> writes = m_writes.span();
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

void StoresByLayout::forEachTableOfKnownObjects(LayoutID layout, NOESCAPE const Invocable<void(const StoresIntoObjects&)> auto& functor) const
{
    functor(m_storesOfLayout[layout.index()]);
    for (uint32_t table : sharedStoresOf(layout))
        functor(m_storesSharedByLayouts[table]);
}

void StoresByLayout::forEachSourceOf(LayoutID layout, ObjectsOfLayout objects, NOESCAPE const Invocable<void(const StoresIntoObjects&)> auto& functor) const
{
    forEachTableOfKnownObjects(layout, functor);
    functor(m_storesIntoEveryObject);
    if (m_isEscapedClass.quickGet(layout.index()))
        functor(m_storesIntoEveryEscapedClass);
    if (m_isLostClass.quickGet(layout.index()))
        functor(m_storesIntoEveryLostClass);
    if (objects == ObjectsOfLayout::Followed)
        return;
    if (m_hasEscapedObject.quickGet(layout.index()))
        functor(m_storesThroughUnknownObject);
    if (m_hasLostObject.quickGet(layout.index()))
        functor(m_storesThroughUntrackedObject);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
