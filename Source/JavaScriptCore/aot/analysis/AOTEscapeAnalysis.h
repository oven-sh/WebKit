/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBlockCycles.h"
#include "AOTEffectsOfOperands.h"
#include "AOTLosses.h"
#include "AOTUseLists.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class EffectsCache;
class FunctionAnalyses;
struct PointsToResults;

using ObjectID = ID<struct ObjectIDTag>;
using ValueSetID = ID<struct ValueSetIDTag>;

using EscapeReason = AllocationEscapeReason;
using EscapeReasons = OptionSet<EscapeReason>;

enum class ObjectKind : uint8_t { Allocation, EntryParameter };
enum class EscapeState : uint8_t { Unknown, Escapes, DoesNotEscape };
enum class LiveInstances : uint8_t { Unknown, AtMostOne };
enum class AliasResult : uint8_t { MayAlias, NoAlias, MustAlias };

struct TrackedObject {
    ValueID definition;
    ObjectKind kind { ObjectKind::Allocation };
    EscapeState state { EscapeState::Unknown };
    EscapeReasons reasons;
    std::optional<Use> firstEscape;
    OperandUses uses { OperandUses::all() };
};

// Code outside the function can reach the object, so a store into it is an escape of what is stored.
inline bool isOpenContainer(const TrackedObject& object)
{
    return object.kind == ObjectKind::EntryParameter || !object.reasons.isEmpty();
}

// A set of objects is a row of bits, one for each object of the function. This holds the words of the row that are not zero, in ascending order.
// A function that makes thousands of objects has thousands of sets, and nearly every set holds an object or two: whole rows take sets times objects.
class ObjectWords {
public:
    using Word = uint64_t;
    static constexpr uint32_t objectsPerWord = 64;

    struct Entry {
        uint32_t index { 0 };
        Word word { 0 };

        friend bool operator==(const Entry&, const Entry&) = default;
    };

    static Entry entryOf(ObjectID object) { return { object.index() / objectsPerWord, Word { 1 } << (object.index() % objectsPerWord) }; }

    bool isEmpty() const { return m_entries.isEmpty(); }
    std::span<const Entry> entries() const LIFETIME_BOUND { return m_entries.span(); }
    bool contains(ObjectID object) const
    {
        Entry wanted = entryOf(object);
        size_t position = positionOf(wanted.index, 0);
        return position < m_entries.size() && m_entries[position].index == wanted.index && (m_entries[position].word & wanted.word);
    }

    bool overlaps(const ObjectWords&) const;

    // What was not there yet is added to `added` as well, which is another set. The entries lie outside both.
    void add(std::span<const Entry>, ObjectWords& added);
    void add(const Entry&);
    void remove(const ObjectWords&);
    // Keeps the room.
    void removeAll() { m_entries.shrink(0); }

    friend bool operator==(const ObjectWords&, const ObjectWords&) = default;

private:
    // Of the first entry from `from` on whose index is not below.
    size_t positionOf(uint32_t index, size_t from) const
    {
        AOT_THRESHOLD(size_t, maxSearchedEntries, 8);
        std::span<const Entry> rest = m_entries.subspan(from);
        if (rest.size() > maxSearchedEntries)
            return from + (std::ranges::lower_bound(rest, index, { }, &Entry::index) - rest.begin());
        size_t position = 0;
        while (position < rest.size() && rest[position].index < index)
            ++position;
        return from + position;
    }

    Vector<Entry, 1> m_entries;
};

class ObjectSet {
public:
    class Iterator {
    public:
        explicit Iterator(std::span<const ObjectWords::Entry> entries)
            : m_entries(entries)
            , m_rest(entries.empty() ? 0 : entries.front().word)
        {
        }

        ObjectID operator*() const { return ObjectID(m_entries.front().index * ObjectWords::objectsPerWord + std::countr_zero(m_rest)); }

        Iterator& operator++()
        {
            m_rest &= m_rest - 1;
            if (!m_rest) {
                m_entries = m_entries.subspan(1);
                m_rest = m_entries.empty() ? 0 : m_entries.front().word;
            }
            return *this;
        }

        friend bool operator==(const Iterator& a, const Iterator& b) { return a.m_entries.size() == b.m_entries.size() && a.m_rest == b.m_rest; }

    private:
        std::span<const ObjectWords::Entry> m_entries;
        ObjectWords::Word m_rest;
    };

    explicit ObjectSet(const ObjectWords& words LIFETIME_BOUND)
        : m_words(words)
    {
    }

    Iterator begin() const { return Iterator(m_words.entries()); }
    Iterator end() const { return Iterator(std::span<const ObjectWords::Entry> { }); }
    bool isEmpty() const { return m_words.isEmpty(); }
    size_t size() const;
    bool contains(ObjectID object) const { return m_words.contains(object); }
    bool overlaps(ObjectSet other) const { return m_words.overlaps(other.m_words); }
    std::optional<ObjectID> onlyMember() const;
    void addTo(BitVector& bits) const
    {
        for (ObjectID object : *this)
            bits.set(object.index());
    }

    friend bool operator==(const ObjectSet& a, const ObjectSet& b) { return a.m_words == b.m_words; }

private:
    const ObjectWords& m_words;
};

struct PointsToSet {
    ObjectSet objects;
    bool mayBeUntracked { true };
};

struct ObjectUse {
    Use use;
    OperandUses uses;
    ValueID retainedBy;
    bool isEscape { true };
    bool isContentsStore { true };
    bool readsContents { true };

    friend bool operator==(const ObjectUse&, const ObjectUse&) = default;
};

// The uses of one object, in function order. Objects that reach the same use share its entry.
class ObjectUses {
public:
    class Iterator {
    public:
        using value_type = ObjectUse;
        using difference_type = std::ptrdiff_t;
        using reference = const ObjectUse&;
        using pointer = const ObjectUse*;
        using iterator_category = std::forward_iterator_tag;

        Iterator() = default;
        Iterator(const uint32_t* index, const ObjectUse* entries)
            : m_index(index)
            , m_entries(entries)
        {
        }

        const ObjectUse& operator*() const { return m_entries[*m_index]; }
        const ObjectUse* operator->() const { return &m_entries[*m_index]; }
        Iterator& operator++()
        {
            ++m_index;
            return *this;
        }
        Iterator operator++(int)
        {
            Iterator before = *this;
            ++m_index;
            return before;
        }
        friend bool operator==(const Iterator&, const Iterator&) = default;

    private:
        const uint32_t* m_index { nullptr };
        const ObjectUse* m_entries { nullptr };
    };

    ObjectUses(std::span<const uint32_t> indices, const ObjectUse* entries)
        : m_indices(indices)
        , m_entries(entries)
    {
    }

    Iterator begin() const { return { m_indices.data(), m_entries }; }
    Iterator end() const { return { m_indices.data() + m_indices.size(), m_entries }; }
    size_t size() const { return m_indices.size(); }
    bool empty() const { return m_indices.empty(); }
    const ObjectUse& operator[](size_t index) const { return m_entries[m_indices[index]]; }
    const ObjectUse& front() const { return (*this)[0]; }

private:
    std::span<const uint32_t> m_indices;
    const ObjectUse* m_entries;
};

// What the solver has taken from types: from those of the value or of the terminator that the question is about, and from those of its operands. It reads a type nowhere else.
struct AnswerFromTypes {
    enum class Question : uint8_t {
        IsReference,
        MayHoldReference,
        RefersToOperand,
        HasFreshResult,
        MayRunOtherCode,
        SummaryOfOperand,
        MakesObjectOnEdge,
        SummaryOfTerminatorOperand,
    };

    Question question { Question::IsReference };
    bool isTrue { false };
    // The index of a value. For the last two questions that of the block of the terminator.
    uint32_t subject { 0 };
    uint32_t operandIndex { 0 };
    OperandSummary summary;

    friend bool operator==(const AnswerFromTypes&, const AnswerFromTypes&) = default;
};

struct EscapeAnalysisInputs {
    const AnalysisContext& context;
    const EffectContext& effects;
    EffectsCache& effectsCache;
    const Function& function;
    const CFG& cfg;
    const UseLists& useLists;
    Subject subject;
    // For didRetypeValues(). A program pass makes its analyses for one look at a function, and nobody retypes it meanwhile.
    bool remembersAnswersFromTypes { false };
};

class EscapeAnalysis {
    WTF_MAKE_NONCOPYABLE(EscapeAnalysis);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(EscapeAnalysis);
public:
    explicit EscapeAnalysis(FunctionAnalyses&);

    // Then there is no object, and every value may be anything: whoever does not ask finds nothing to build on.
    bool gaveUp() const { return m_gaveUp; }

    // The body makes no object, so every object is an entry parameter, and its caller may have done anything with it before the function was entered.
    bool tracksOnlyEntryParameters() const { return m_tracksOnlyEntryParameters; }

    unsigned numberOfObjects() const
    {
        solveIfWaiting();
        return m_objects.size();
    }
    const TrackedObject& at(ObjectID) const LIFETIME_BOUND;
    std::optional<ObjectID> objectDefinedBy(ValueID) const;

    PointsToSet pointsTo(ValueID) const LIFETIME_BOUND;
    std::optional<ObjectID> singleObject(ValueID) const;
    bool isExactUse(ObjectID, const Use&) const;
    bool isOnlyOperandHolding(ObjectID, const Use&) const;

    std::span<const ValueID> aliasesOf(ObjectID) const LIFETIME_BOUND;
    ObjectUses usesOf(ObjectID object) const LIFETIME_BOUND
    {
        if (!m_hasListedUses) [[unlikely]]
            listUses();
        return { entriesOf(object, m_uses, m_firstUse), m_sharedUses.span().data() };
    }
    std::span<const ObjectID> contentsOf(ObjectID object) const LIFETIME_BOUND { return entriesOf(object, m_contents, m_firstContent); }
    std::span<const ObjectID> containersOf(ObjectID object) const LIFETIME_BOUND { return entriesOf(object, m_containers, m_firstContainer); }

    AliasResult alias(ValueID, ValueID) const;
    bool mayAlias(ValueID, ValueID) const;

    bool hasLiveInstances() const { return m_hasLiveInstances; }
    void computeLiveInstances(const CFG&, const UseLists&, const ScopedLambda<const BlockCycles&()>&);
    LiveInstances liveInstancesOf(ObjectID object) const
    {
        RELEASE_ASSERT(m_hasLiveInstances);
        return m_liveInstances[object.index()];
    }

    // The object, and every object that some value may name as well as it.
    BitVector objectsSharingValuesWith(ObjectID) const;
    // About an object that a terminator makes on an edge: those of objectsSharingValuesWith of which no value that is live in front of any such terminator may name an earlier instance.
    const BitVector& objectsRenewedAtMakersOf(ObjectID object) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(m_hasLiveInstances);
        return object.index() < m_objectsRenewedAtMakers.size() ? m_objectsRenewedAtMakers[object.index()] : m_noObjects;
    }

    // Types and expected types have changed, and nothing else. Answers whether the solver would be told all that it was told: then it would find what it has found.
    bool didRetypeValues();

private:
    struct StoredSet {
        ObjectWords objects;
        // Where some member is an entry parameter: the members that are allocations. Else `objects` are those.
        ObjectWords allocationsBesideEntryParameters;
        bool mayBeUntracked { true };
        bool holdsEntryParameter { false };
        bool holdsSuperConstructResult { false };
        bool holdsNothingElseFromOutside { false };
        Vector<uint32_t, 2> groupsOfFrameWords;
    };
    // What a value may be among the allocations of the function. An entry parameter is an object that nobody tracks.
    struct AllocationSet {
        const ObjectWords& objects;
        bool mayBeUntracked { true };
        bool existedAtEntryIfUntracked { false };
        const Vector<uint32_t, 2>* groupsOfFrameWords { nullptr };
    };
    struct TypesOfValue {
        TypeID type;
        TypeID expected;

        friend bool operator==(const TypesOfValue&, const TypesOfValue&) = default;
    };
    AllocationSet allocationSetOf(ValueID) const LIFETIME_BOUND;

    // Answers how much work it was.
    uint64_t solve();
    void solveIfWaiting() const
    {
        if (!m_isSolved) [[unlikely]]
            const_cast<EscapeAnalysis*>(this)->solve();
    }

    void rememberTypes();
    void createObjects(const PointsToResults&);
    void takeSets(PointsToResults&);
    void computeAliases() const;
    void computeContents(const PointsToResults&);
    Vector<EscapeReasons> computeReasons(const PointsToResults&);
    void propagateEscapeToContents();
    void computeUses(PointsToResults&, const UseLists&, std::span<const EscapeReasons> useReasons);
    void listUses() const;
    void computeStates();
    AliasResult aliasWhere(ValueID, ValueID, NOESCAPE const Invocable<bool(ObjectID)> auto& hasAtMostOneLiveInstance) const;
    AliasResult aliasAmongAllocations(ValueID, ValueID, NOESCAPE const Invocable<bool(ObjectID)> auto& hasAtMostOneLiveInstance) const;

    template<typename T>
    std::span<const T> entriesOf(ObjectID object, const Vector<T>& entries, const Vector<uint32_t>& firstEntry) const
    {
        RELEASE_ASSERT(object.index() < m_objects.size());
        uint32_t first = firstEntry[object.index()];
        return entries.subspan(first, firstEntry[object.index() + 1] - first);
    }

    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const Function& m_function;
    bool m_tracksOnlyEntryParameters { false };
    bool m_isSolved { false };
    bool m_remembersAnswersFromTypes { false };
    bool m_gaveUp { false };
    bool m_hasLiveInstances { false };
    Vector<TrackedObject> m_objects;
    Vector<LiveInstances> m_liveInstances;
    Vector<ValueSetID> m_setOfValue;
    Vector<StoredSet> m_sets;
    BitVector m_noObjects;
    ObjectWords m_emptySet;
    mutable Vector<ValueID> m_aliases;
    mutable Vector<uint32_t> m_firstAlias;
    // Two entries per use that the solver recorded: not an escape, then an escape. That is all that differs between the objects of one use.
    Vector<ObjectUse> m_sharedUses;
    // The lists are built on the first call of usesOf(). Until then the uses stay in the order in which the solver found them.
    struct UnlistedUse {
        uint32_t sharedUse;
        uint32_t firstObject;
        uint32_t numberOfObjects;
    };
    mutable bool m_hasListedUses { false };
    mutable Vector<UnlistedUse> m_unlistedUses;
    mutable Vector<ObjectID> m_objectsOfUnlistedUses;
    mutable BitVector m_isEscapeOfUnlistedUse;
    // Indices into m_sharedUses.
    mutable Vector<uint32_t> m_uses;
    Vector<uint32_t> m_firstUse;
    Vector<ObjectID> m_contents;
    Vector<uint32_t> m_firstContent;
    Vector<ObjectID> m_containers;
    Vector<uint32_t> m_firstContainer;
    Vector<BitVector> m_objectsRenewedAtMakers;
    Vector<AnswerFromTypes> m_answersFromTypes;
    Vector<TypesOfValue> m_typesWhenAnswered;
};

// Words of different groups hold different objects.
bool isFrameWordWithAllocationGroup(const Function&, const Value&);

// False where every allocation that the value may name was read out of an object that is open to others, and so has a use that lets it escape.
bool mayNameUnescapedAllocation(const Function&, ValueID);
// Seen without the solve too, through the parameters of blocks as well: the values that make the objects which the value may name, or out of which what it names may have been read, however many objects lie between.
std::optional<Vector<ValueID, 8>> findMakersOfObjectsBehind(FunctionAnalyses&, ValueID);

// What the solve says of a use of a value for every object that the value names, seen from the use alone.
enum class UseSeenAlone : uint8_t {
    Unknown,
    // The user names each of the objects as well, and the use stands in no list.
    HandsOn,
    // It stands in usesOf() of each of the objects.
    IsListed,
    // And lets them escape.
    IsListedAsEscape,
};
UseSeenAlone classifyUseAlone(FunctionAnalyses&, const Use&);

// The blocks whose terminators declare a result on an edge to the parameter.
Vector<BlockID, 2> blocksWithResultIn(const Function&, const CFG&, BlockID blockOfParameter, ValueID parameter);

inline const TrackedObject& EscapeAnalysis::at(ObjectID object) const
{
    RELEASE_ASSERT(object.index() < m_objects.size());
    return m_objects[object.index()];
}

inline std::span<const ValueID> EscapeAnalysis::aliasesOf(ObjectID object) const
{
    if (m_firstAlias.isEmpty())
        computeAliases();
    return entriesOf(object, m_aliases, m_firstAlias);
}

inline PointsToSet EscapeAnalysis::pointsTo(ValueID value) const
{
    solveIfWaiting();
    RELEASE_ASSERT(value.index() < m_setOfValue.size());
    ValueSetID id = m_setOfValue[value.index()];
    if (!id)
        return { ObjectSet(m_emptySet), true };
    const StoredSet& set = m_sets[id.index()];
    return { ObjectSet(set.objects), set.mayBeUntracked };
}

inline std::optional<ObjectID> EscapeAnalysis::singleObject(ValueID value) const
{
    PointsToSet set = pointsTo(value);
    if (set.mayBeUntracked)
        return std::nullopt;
    return set.objects.onlyMember();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
