/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CollectionScope.h"
#include "MarkedSpace.h"
#include "RememberedSlot.h"
#include <concepts>
#include <span>
#include <type_traits>
#include <wtf/Assertions.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class AbstractSlotVisitor;
class JSCell;
class SlotVisitor;
class VM;

namespace AOT {

struct State;

enum class CacheKind : uint8_t { StrongRoots, WeakByKey, ClearedEachCollection, Observer };
enum class RootVolatility : uint8_t { GreyedByMarking, GreyedByExecution };
enum class CacheValidation : bool { Off, On };

class CacheRegistry {
    WTF_MAKE_NONCOPYABLE(CacheRegistry);
    WTF_MAKE_TZONE_ALLOCATED(CacheRegistry);
public:
    CacheRegistry();
    ~CacheRegistry();

    template<typename Owner> void add(CacheKind, ASCIILiteral name, Owner&, RootVolatility = RootVolatility::GreyedByMarking);
    void addStrongRoots(ASCIILiteral name, std::span<JSCell* const> cellsOrNull, RootVolatility = RootVolatility::GreyedByMarking);
    void addClearedEachCollection(ASCIILiteral name, std::span<uint8_t> zeroedByClear);
    template<typename Owner> void addObserver(ASCIILiteral name, Owner& owner) { add(CacheKind::Observer, name, owner); }

    Lock& lock() { return m_lock; }

    template<typename Visitor> void visitRoots(Visitor&, RootVolatility);
    bool hasVisitedExecutionRoots() const { return m_hasVisitedExecutionRoots; }
    void finalize(VM&, CollectionScope, CacheValidation);
    void willAnalyzeHeap(VM&);

private:
    struct Entry {
        CacheKind kind;
        RootVolatility volatility { RootVolatility::GreyedByMarking };
        ASCIILiteral name;
        void* owner { nullptr };
        size_t size { 0 };
        void (*visitWithSlotVisitor)(Entry&, SlotVisitor&) { nullptr };
        void (*visitWithAbstractSlotVisitor)(Entry&, AbstractSlotVisitor&) { nullptr };
        size_t (*removeDeadEntries)(Entry&, VM&, CollectionScope) { nullptr };
        void (*clear)(Entry&) { nullptr };
        void (*didFinishCollection)(Entry&, VM&, CollectionScope) { nullptr };
        void (*willAnalyzeHeap)(Entry&, VM&) { nullptr };
    };

    void append(Entry&&);

    SegmentedVector<Entry, 16> m_entries;
    Lock m_lock;
    bool m_hasVisitedExecutionRoots { false };
};

extern template void CacheRegistry::visitRoots<SlotVisitor>(SlotVisitor&, RootVolatility);
extern template void CacheRegistry::visitRoots<AbstractSlotVisitor>(AbstractSlotVisitor&, RootVolatility);

// What the kind needs, the owner must have.
template<typename Owner>
void CacheRegistry::add(CacheKind kind, ASCIILiteral name, Owner& owner, RootVolatility volatility)
{
    ASSERT(kind == CacheKind::StrongRoots || volatility == RootVolatility::GreyedByMarking);

    Entry entry { .kind = kind, .volatility = volatility, .name = name, .owner = &owner };
    if constexpr (requires(Owner& candidate, SlotVisitor& visitor, AbstractSlotVisitor& abstractVisitor) { candidate.visitRoots(visitor); candidate.visitRoots(abstractVisitor); }) {
        entry.visitWithSlotVisitor = [](Entry& entry, SlotVisitor& visitor) { static_cast<Owner*>(entry.owner)->visitRoots(visitor); };
        entry.visitWithAbstractSlotVisitor = [](Entry& entry, AbstractSlotVisitor& visitor) { static_cast<Owner*>(entry.owner)->visitRoots(visitor); };
    }
    if constexpr (requires(Owner& candidate, VM& vm, CollectionScope scope) { { candidate.removeDeadEntries(vm, scope) } -> std::convertible_to<size_t>; })
        entry.removeDeadEntries = [](Entry& entry, VM& vm, CollectionScope scope) -> size_t { return static_cast<Owner*>(entry.owner)->removeDeadEntries(vm, scope); };
    if constexpr (requires(Owner& candidate) { candidate.clear(); })
        entry.clear = [](Entry& entry) { static_cast<Owner*>(entry.owner)->clear(); };
    if constexpr (requires(Owner& candidate, VM& vm, CollectionScope scope) { candidate.didFinishCollection(vm, scope); })
        entry.didFinishCollection = [](Entry& entry, VM& vm, CollectionScope scope) { static_cast<Owner*>(entry.owner)->didFinishCollection(vm, scope); };
    if constexpr (requires(Owner& candidate, VM& vm) { candidate.willAnalyzeHeap(vm); })
        entry.willAnalyzeHeap = [](Entry& entry, VM& vm) { static_cast<Owner*>(entry.owner)->willAnalyzeHeap(vm); };
    RELEASE_ASSERT(kind == CacheKind::StrongRoots ? !!entry.visitWithSlotVisitor : kind == CacheKind::WeakByKey ? !!entry.removeDeadEntries : kind == CacheKind::ClearedEachCollection ? !!entry.clear : !!entry.didFinishCollection);
    append(WTF::move(entry));
}

class ImmortalCells {
    WTF_MAKE_NONCOPYABLE(ImmortalCells);
    WTF_MAKE_TZONE_ALLOCATED(ImmortalCells);
public:
    explicit ImmortalCells(State&);

    void add(JSCell*);
    template<typename Visitor> void visitRoots(Visitor&);

private:
    static constexpr size_t cellsPerSegment = 512;

    template<typename Functor> void forEachSegment(size_t firstIndex, const Functor&);

    State& m_state;
    SegmentedVector<JSCell*, cellsPerSegment> m_cells;
    size_t m_numberOfCellsVisited { 0 };
    HeapVersion m_lastVisitVersion { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
