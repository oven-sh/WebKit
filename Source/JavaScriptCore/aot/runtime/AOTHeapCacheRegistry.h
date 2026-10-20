/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CollectionScope.h"
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

enum class CacheKind : uint8_t { StrongRoots, WeakByKey, ClearedEachCollection };
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
    void addClearedEachCollection(ASCIILiteral name, std::span<uint8_t> zeroedByClear, RememberedSlotSet&);
    template<typename Owner> void addObserver(ASCIILiteral name, Owner&);

    Lock& lock() { return m_lock; }

    template<typename Visitor> void visitRoots(Visitor&, RootVolatility);
    bool hasVisitedExecutionRoots() const { return m_hasVisitedExecutionRoots; }
    void finalize(VM&, CollectionScope, CacheValidation);
    void willAnalyzeHeap(VM&);

private:
    enum class EntryKind : uint8_t { StrongRoots, WeakByKey, ClearedEachCollection, Observer };

    struct Entry {
        EntryKind kind;
        RootVolatility volatility { RootVolatility::GreyedByMarking };
        ASCIILiteral name;
        void* owner { nullptr };
        size_t size { 0 };
        RememberedSlotSet* writtenRegions { nullptr };
        void (*visitWithSlotVisitor)(Entry&, SlotVisitor&) { nullptr };
        void (*visitWithAbstractSlotVisitor)(Entry&, AbstractSlotVisitor&) { nullptr };
        size_t (*removeDeadEntries)(Entry&, VM&, CollectionScope) { nullptr };
        void (*clear)(Entry&) { nullptr };
        void (*didFinishCollection)(Entry&, VM&, CollectionScope) { nullptr };
        void (*willAnalyzeHeap)(Entry&, VM&) { nullptr };
    };

    static EntryKind entryKindFor(CacheKind);
    template<typename Owner> static void setHeapAnalysisCallback(Entry&);
    void append(Entry&&);

    SegmentedVector<Entry, 16> m_entries;
    Lock m_lock;
    bool m_hasVisitedExecutionRoots { false };
};

extern template void CacheRegistry::visitRoots<SlotVisitor>(SlotVisitor&, RootVolatility);
extern template void CacheRegistry::visitRoots<AbstractSlotVisitor>(AbstractSlotVisitor&, RootVolatility);

inline CacheRegistry::EntryKind CacheRegistry::entryKindFor(CacheKind kind)
{
    switch (kind) {
    case CacheKind::StrongRoots:
        return EntryKind::StrongRoots;
    case CacheKind::WeakByKey:
        return EntryKind::WeakByKey;
    case CacheKind::ClearedEachCollection:
        return EntryKind::ClearedEachCollection;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Owner>
void CacheRegistry::setHeapAnalysisCallback(Entry& entry)
{
    if constexpr (requires(Owner& candidate, VM& vm) { candidate.willAnalyzeHeap(vm); })
        entry.willAnalyzeHeap = [](Entry& entry, VM& vm) { static_cast<Owner*>(entry.owner)->willAnalyzeHeap(vm); };
}

template<typename Owner>
void CacheRegistry::add(CacheKind kind, ASCIILiteral name, Owner& owner, RootVolatility volatility)
{
    ASSERT(kind == CacheKind::StrongRoots || volatility == RootVolatility::GreyedByMarking);

    Entry entry { .kind = entryKindFor(kind), .volatility = volatility, .name = name, .owner = &owner };
    switch (kind) {
    case CacheKind::StrongRoots:
        if constexpr (requires(Owner& candidate, SlotVisitor& visitor, AbstractSlotVisitor& abstractVisitor) { candidate.visitRoots(visitor); candidate.visitRoots(abstractVisitor); }) {
            entry.visitWithSlotVisitor = [](Entry& entry, SlotVisitor& visitor) { static_cast<Owner*>(entry.owner)->visitRoots(visitor); };
            entry.visitWithAbstractSlotVisitor = [](Entry& entry, AbstractSlotVisitor& visitor) { static_cast<Owner*>(entry.owner)->visitRoots(visitor); };
        }
        RELEASE_ASSERT(entry.visitWithSlotVisitor && entry.visitWithAbstractSlotVisitor);
        break;
    case CacheKind::WeakByKey:
        if constexpr (requires(Owner& candidate, VM& vm, CollectionScope scope) { { candidate.removeDeadEntries(vm, scope) } -> std::convertible_to<size_t>; })
            entry.removeDeadEntries = [](Entry& entry, VM& vm, CollectionScope scope) -> size_t { return static_cast<Owner*>(entry.owner)->removeDeadEntries(vm, scope); };
        RELEASE_ASSERT(entry.removeDeadEntries);
        break;
    case CacheKind::ClearedEachCollection:
        if constexpr (requires(Owner& candidate) { candidate.clear(); })
            entry.clear = [](Entry& entry) { static_cast<Owner*>(entry.owner)->clear(); };
        RELEASE_ASSERT(entry.clear);
        break;
    }
    setHeapAnalysisCallback<Owner>(entry);
    append(WTF::move(entry));
}

template<typename Owner>
void CacheRegistry::addObserver(ASCIILiteral name, Owner& owner)
{
    Entry entry { .kind = EntryKind::Observer, .name = name, .owner = &owner };
    entry.didFinishCollection = [](Entry& entry, VM& vm, CollectionScope scope) { static_cast<Owner*>(entry.owner)->didFinishCollection(vm, scope); };
    setHeapAnalysisCallback<Owner>(entry);
    append(WTF::move(entry));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
