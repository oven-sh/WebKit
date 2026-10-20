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
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class EffectsCache;
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
    LiveInstances liveInstances { LiveInstances::Unknown };
};

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

struct EscapeAnalysisInputs {
    const AnalysisContext& context;
    const EffectContext& effects;
    EffectsCache& effectsCache;
    const Function& function;
    const CFG& cfg;
    const UseLists& useLists;
    const BlockCycles& cycles;
    Subject subject;
};

class EscapeAnalysis {
    WTF_MAKE_NONCOPYABLE(EscapeAnalysis);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(EscapeAnalysis);
public:
    explicit EscapeAnalysis(const EscapeAnalysisInputs&);

    bool gaveUp() const { return m_gaveUp; }

    unsigned numberOfObjects() const { return m_objects.size(); }
    const TrackedObject& at(ObjectID) const LIFETIME_BOUND;
    std::optional<ObjectID> objectDefinedBy(ValueID) const;

    PointsToSet pointsTo(ValueID) const LIFETIME_BOUND;
    std::optional<ObjectID> singleObject(ValueID) const;
    bool isExactUse(ObjectID, const Use&) const;

    std::span<const ValueID> aliasesOf(ObjectID object) const LIFETIME_BOUND { return entriesOf(object, m_aliases, m_firstAlias); }
    std::span<const ObjectUse> usesOf(ObjectID object) const LIFETIME_BOUND { return entriesOf(object, m_uses, m_firstUse); }
    std::span<const ObjectID> contentsOf(ObjectID object) const LIFETIME_BOUND { return entriesOf(object, m_contents, m_firstContent); }
    std::span<const ObjectID> containersOf(ObjectID object) const LIFETIME_BOUND { return entriesOf(object, m_containers, m_firstContainer); }

    AliasResult alias(ValueID, ValueID) const;

    // The object, and every object that some value may name as well as it.
    BitVector objectsSharingValuesWith(ObjectID) const;
    // About an object that a terminator makes on an edge: those of objectsSharingValuesWith of which no value that is live in front of any such terminator
    // may name an earlier instance. What happened to them is over there. An object that is still in use there keeps its past and costs the others nothing.
    const BitVector& objectsRenewedAtMakersOf(ObjectID object) const LIFETIME_BOUND { return object.index() < m_objectsRenewedAtMakers.size() ? m_objectsRenewedAtMakers[object.index()] : m_noObjects; }

private:
    struct StoredSet {
        BitVector objects;
        bool mayBeUntracked { true };
    };

    void createObjects(const PointsToResults&);
    void takeSets(PointsToResults&);
    void computeAliases();
    void computeContents(const PointsToResults&);
    Vector<EscapeReasons> computeReasons(const PointsToResults&);
    void propagateEscapeToContents();
    void computeUses(const PointsToResults&, const UseLists&, std::span<const EscapeReasons> useReasons);
    void computeStates();
    void computeLiveInstances(const CFG&, const UseLists&, const BlockCycles&);

    template<typename T>
    std::span<const T> entriesOf(ObjectID object, const Vector<T>& entries, const Vector<uint32_t>& firstEntry) const
    {
        RELEASE_ASSERT(object.index() < m_objects.size());
        uint32_t first = firstEntry[object.index()];
        return entries.subspan(first, firstEntry[object.index() + 1] - first);
    }

    const Program& m_program;
    const Function& m_function;
    bool m_gaveUp { false };
    Vector<TrackedObject> m_objects;
    Vector<ValueSetID> m_setOfValue;
    Vector<StoredSet> m_sets;
    BitVector m_noObjects;
    Vector<ValueID> m_aliases;
    Vector<uint32_t> m_firstAlias;
    Vector<ObjectUse> m_uses;
    Vector<uint32_t> m_firstUse;
    Vector<ObjectID> m_contents;
    Vector<uint32_t> m_firstContent;
    Vector<ObjectID> m_containers;
    Vector<uint32_t> m_firstContainer;
    Vector<BitVector> m_objectsRenewedAtMakers;
};

// The blocks whose terminators declare a result on an edge to the parameter.
Vector<BlockID, 2> blocksWithResultIn(const Function&, const CFG&, BlockID blockOfParameter, ValueID parameter);

inline const TrackedObject& EscapeAnalysis::at(ObjectID object) const
{
    RELEASE_ASSERT(object.index() < m_objects.size());
    return m_objects[object.index()];
}

inline PointsToSet EscapeAnalysis::pointsTo(ValueID value) const
{
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
