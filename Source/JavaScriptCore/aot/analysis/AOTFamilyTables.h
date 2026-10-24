/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayout.h"
#include "AOTSortedSet.h"
#include "AOTVector.h"
#include <atomic>
#include <memory>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;
enum class TypeLevel : uint8_t;

using LayoutList = SortedSet<LayoutID, 4>;

struct ReceiverState {
    OptionSet<ObjectMutation> impossibleMutations;
    KeyEnumeration keys { KeyEnumeration::Unknown };
    bool addedKeysAreComplete { false };

    bool cannot(ObjectMutation mutation) const { return impossibleMutations.contains(mutation); }
    void meetWith(const ReceiverState&);

    friend bool operator==(const ReceiverState&, const ReceiverState&) = default;
};

// The set and type ids may be provisional ids of another arena: only read them, or pass them through TypeArena::adopt().
struct FamilyMember {
    LayoutSetID holders;
    LayoutSetID withoutField;
    FieldReference declaration;
    TypeID unionOfFieldTypes;
    TypeID intersectionOfFieldTypes;
    bool hasOneDeclaration { false };
    bool isAlwaysFilled { false };
    bool hasExpectedField { false };
};

// Caches everything that needs a loop over the members of a family (the LayoutSetID in a type), once per set instead of once per access.
class FamilyTables {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FamilyTables);
    WTF_MAKE_NONCOPYABLE(FamilyTables);
public:
    struct Candidates {
        LayoutList proven;
        LayoutList predicted;
        // Of the predicted ones, where nothing is proven: those that the expected type lists.
        LayoutList ofExpectedType;
    };

    explicit FamilyTables(const Program&);
    ~FamilyTables();

    std::span<const LayoutID> allocatedMembersOf(LayoutSetID) const LIFETIME_BOUND;
    ReceiverState stateOf(LayoutSetID) const;
    const FamilyMember& memberOf(LayoutSetID, PropertyKeyID) const LIFETIME_BOUND;
    Candidates candidatesOf(LayoutSetID receiver, LayoutSetID expected, LayoutSetID stamp) const;
    bool isSubsetOf(LayoutSetID, LayoutSetID) const;

    // Like the ids in FamilyMember, the result may be provisional.
    LayoutSetID membersWhoseFieldCannotHold(LayoutSetID, PropertyKeyID, TypeID read, TypeLevel) const;

    void didChangeLayouts() const { m_generation.fetch_add(1, std::memory_order_release); }

private:
    template<typename Entry> class ConcurrentCache;
    struct Family;
    struct Cache;
    using CacheList = Vector<std::unique_ptr<Cache>>;

    Cache& cache() const;
    Cache& replaceCache(uint64_t phase) const;

    std::span<const LayoutID> listedMembers(LayoutSetID) const;
    LayoutList computeAllocatedMembers(std::span<const LayoutID> members) const;
    const Family& familyOf(Cache&, LayoutSetID) const;
    Family computeFamily(std::span<const LayoutID> members) const;
    FamilyMember computeMember(const Family&, PropertyKeyID) const;
    LayoutSetID computeMembersWhoseFieldCannotHold(const LayoutList& allocatedMembers, PropertyKeyID, TypeID read, TypeLevel) const;

    const Program& m_program;
    mutable std::atomic<uint64_t> m_generation { 0 };
    mutable std::atomic<Cache*> m_cache { nullptr };
    mutable Lock m_lock;
    // A replaced cache is freed late: a caller may still be reading a result from it, and cache() may still be looking at its phase number.
    mutable CacheList m_currentPhaseCaches WTF_GUARDED_BY_LOCK(m_lock);
    mutable CacheList m_previousPhaseCaches WTF_GUARDED_BY_LOCK(m_lock);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
