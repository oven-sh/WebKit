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

class ObjectSet {
public:
    class Iterator {
    public:
        explicit Iterator(BitVector::iterator position)
            : m_position(position)
        {
        }

        ObjectID operator*() const { return ObjectID(*m_position); }

        Iterator& operator++()
        {
            ++m_position;
            return *this;
        }

        friend bool operator==(const Iterator& a, const Iterator& b) { return a.m_position == b.m_position; }

    private:
        BitVector::iterator m_position;
    };

    explicit ObjectSet(const BitVector& bits LIFETIME_BOUND)
        : m_bits(bits)
    {
    }

    Iterator begin() const { return Iterator(m_bits.begin()); }
    Iterator end() const { return Iterator(m_bits.end()); }
    bool isEmpty() const { return m_bits.isEmpty(); }
    size_t size() const { return m_bits.bitCount(); }
    bool contains(ObjectID object) const { return m_bits.get(object.index()); }
    bool overlaps(ObjectSet other) const;
    std::optional<ObjectID> onlyMember() const;
    const BitVector& bits() const { return m_bits; }

    friend bool operator==(const ObjectSet& a, const ObjectSet& b) { return a.m_bits == b.m_bits; }

private:
    const BitVector& m_bits;
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
    std::span<const ObjectUse> usesOf(ObjectID object) const LIFETIME_BOUND
    {
        if (!m_hasListedUses) [[unlikely]]
            listUses();
        return entriesOf(object, m_uses, m_firstUse);
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
        BitVector objects;
        // Where some member is an entry parameter: the members that are allocations. Else `objects` are those.
        BitVector allocationsBesideEntryParameters;
        bool mayBeUntracked { true };
        bool holdsEntryParameter { false };
        bool holdsSuperConstructResult { false };
    };
    // What a value may be among the allocations of the function. An entry parameter is an object that nobody tracks.
    struct AllocationSet {
        const BitVector& objects;
        bool mayBeUntracked { true };
        bool existedAtEntryIfUntracked { false };
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
    mutable Vector<ValueID> m_aliases;
    mutable Vector<uint32_t> m_firstAlias;
    // The lists are made for the first who asks for one. Until then their entries lie in the order in which the solver found them.
    struct UnlistedUse {
        ObjectID object;
        ObjectUse use;
    };
    mutable bool m_hasListedUses { false };
    mutable Vector<UnlistedUse> m_unlistedUses;
    mutable Vector<ObjectUse> m_uses;
    Vector<uint32_t> m_firstUse;
    Vector<ObjectID> m_contents;
    Vector<uint32_t> m_firstContent;
    Vector<ObjectID> m_containers;
    Vector<uint32_t> m_firstContainer;
    Vector<BitVector> m_objectsRenewedAtMakers;
    Vector<AnswerFromTypes> m_answersFromTypes;
    Vector<TypesOfValue> m_typesWhenAnswered;
};

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
        return { ObjectSet(m_noObjects), true };
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
