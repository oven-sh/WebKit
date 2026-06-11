/*
 * Copyright (C) 2013-2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "StructureRareData.h"

#include "AdaptiveInferredPropertyValueWatchpointBase.h"
#include "CacheableIdentifierInlines.h"
#include "CachedSpecialPropertyAdaptiveStructureWatchpoint.h"
#include "JSCellButterfly.h"
#include "JSObjectInlines.h"
#include "JSPropertyNameEnumerator.h"
#include "JSString.h"
#include "ObjectPropertyConditionSet.h"
#include "StructureChain.h"
#include "StructureInlines.h"
#include "StructureRareDataInlines.h"
#include <wtf/Atomics.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

const ClassInfo StructureRareData::s_info = { "StructureRareData"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(StructureRareData) };

Structure* StructureRareData::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CellType, StructureFlags), info());
}

StructureRareData* StructureRareData::create(VM& vm, Structure* previous)
{
    StructureRareData* rareData = new (NotNull, allocateCell<StructureRareData>(vm)) StructureRareData(vm, previous);
    rareData->finishCreation(vm);
    return rareData;
}

void StructureRareData::destroy(JSCell* cell)
{
    static_cast<StructureRareData*>(cell)->StructureRareData::~StructureRareData();
}

StructureRareData::StructureRareData(VM& vm, Structure* previous)
    : JSCell(vm, vm.structureRareDataStructure.get())
    , m_previous(previous, WriteBarrierEarlyInit)
{
    // TSAN family structure-fields (§8.9): m_maxOffset/m_transitionOffset are
    // read lock-free through the relaxed atomic loads in Structure.h
    // (maxOffset()/transitionOffset() rare-data overflow arms) the instant a
    // stale/recycled reference is probed; the member-init-list plain stores
    // were the last unpaired writer. Relaxed atomic stores — identical
    // str/mov codegen, no ordering implied (publication ordering comes from
    // allocateRareData's fence + CAS, acquire-paired under TSAN by
    // Structure::previousOrRareDataConcurrently).
    WTF::atomicStore(&m_maxOffset, invalidOffset, std::memory_order_relaxed);
    WTF::atomicStore(&m_transitionOffset, invalidOffset, std::memory_order_relaxed);
}

template<typename Visitor>
void StructureRareData::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    StructureRareData* thisObject = uncheckedDowncast<StructureRareData>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());

    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_previous);
    if (thisObject->m_specialPropertyCache) {
        for (unsigned index = 0; index < numberOfCachedSpecialPropertyKeys; ++index)
            visitor.appendUnbarriered(thisObject->cachedSpecialProperty(static_cast<CachedSpecialPropertyKey>(index)));
    }
    visitor.appendUnbarriered(thisObject->cachedPropertyNameEnumerator());
    for (unsigned index = 0; index < numberOfCachedPropertyNames; ++index) {
        auto* cached = thisObject->m_cachedPropertyNames[index].unvalidatedGet();
        if (cached != cachedPropertyNamesSentinel())
            visitor.appendUnbarriered(cached);
    }
}

DEFINE_VISIT_CHILDREN(StructureRareData);

// ----------- Cached special properties helper watchpoint classes -----------

class CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint final : public AdaptiveInferredPropertyValueWatchpointBase {
    WTF_MAKE_TZONE_ALLOCATED(CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint);
public:
    typedef AdaptiveInferredPropertyValueWatchpointBase Base;
    CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint(const ObjectPropertyCondition&, StructureRareData*);

private:
    bool isValid() const final;
    void handleFire(VM&, const FireDetail&) final;

    StructureRareData* m_structureRareData;
};

SpecialPropertyCacheEntry::~SpecialPropertyCacheEntry() = default;

// =============================================================================
// UNGIL annex N7 RESOLVED-5 (AUD1.N4, BINDING; U-T8b) — StructureRareData
// runtime caches under N mutators.
//
// Ruling, consumed verbatim:
//   (1) m_specialPropertyCache is §K.3-class lazy publication: build, then
//       release-CAS the single pointer word; losers discard. (Below.)
//   (2) Cache-entry INSTALLS run under the owning Structure's m_lock (the
//       structure already owns its rare data lifecycle, OM GT order); each
//       JIT-read word (SpecialPropertyCacheEntry::m_value;
//       m_cachedPropertyNameEnumeratorAndFlag; m_cachedPropertyNames[k]) is
//       single-word release-published LAST, after every word it summarizes.
//   (3) The enumerator install {watchpoint vector filled first, immutable
//       after; flag word fence-published last} lives in
//       StructureRareDataInlines.h (setCachedPropertyNameEnumerator /
//       clearCachedPropertyNameEnumerator). RESOLVED (was OPEN for that
//       header's owning slice): the install now runs under the owning
//       Structure's m_lock with winner-keeps, taken in
//       Structure::setCachedPropertyNameEnumerator (Structure.cpp), and the
//       flag word is storeStoreFence-published last per rule (2).
//       Watchpoint-set FIRING stays K4.VI.2 (inside a §A.3 stop, jit
//       deopt machinery); the OM-annex cross-pointer is recorded in AUD1.
// Flag-off / GIL-on identity: the CAS cannot lose and the added lock is
// uncontended; no codegen or semantic change.
// =============================================================================

SpecialPropertyCache& StructureRareData::ensureSpecialPropertyCacheSlow()
{
    ASSERT(!isCompilationThread() && !Thread::mayBeGCThread());
    // AUD1.N4(1): §K.3-class CAS-publish. Two mutators iterating a shared
    // structure can race here; the old fence+plain-store pair leaked the
    // loser's cache AND let the loser hand out a pointer the winner's store
    // was about to clobber (unique_ptr double-publish => UAF). The CAS makes
    // exactly one buffer the published one; the loser's makeUnique result is
    // destroyed on scope exit, after it lost (it was never visible).
    auto cache = makeUnique<SpecialPropertyCache>();
    static_assert(sizeof(std::unique_ptr<SpecialPropertyCache>) == sizeof(SpecialPropertyCache*), "CAS-publish treats the unique_ptr as one pointer word");
    SpecialPropertyCache* prior = WTF::atomicCompareExchangeStrong(std::bit_cast<SpecialPropertyCache**>(&m_specialPropertyCache), static_cast<SpecialPropertyCache*>(nullptr), cache.get());
    if (prior)
        return *prior; // Loser: the winner's fully-constructed cache is the published one.
    // Winner: ownership transferred to the member by the CAS; the seq_cst
    // success edge carries the release the old storeStoreFence provided for
    // concurrent compilers' plain loads.
    return *cache.release();
}

inline void StructureRareData::giveUpOnSpecialPropertyCache(CachedSpecialPropertyKey key)
{
    // AUD1.N4 (AMENDMENT to the N7 RESOLVED-5 row, recorded here): the
    // sentinel publish stays UNLOCKED but is a CAS from EMPTY, not a plain
    // overwrite. The old plain setWithoutWriteBarrier raced the locked
    // installer's m_value.set on the same word (a TSAN-visible
    // unsynchronized write to a lock-guarded word) and could clobber a
    // freshly installed real value, leaving live watchpoints summarized by
    // a sentinel. The CAS publishes the sentinel only when no value is
    // cached; if a racing locked installer won, its real value stays (its
    // watchpoints carry validity). Flag-off/GIL-on identity: this slow path
    // is reached only when the cache lookup missed (entry empty or already
    // the sentinel), so the CAS succeeds exactly when the old store wrote
    // something new — behavior is bit-identical single-threaded.
    auto& slot = ensureSpecialPropertyCache().m_cache[static_cast<unsigned>(key)].m_value;
    static_assert(sizeof(WriteBarrier<Unknown>) == sizeof(EncodedJSValue), "single-word sentinel CAS treats the barrier as one EncodedJSValue word");
    WTF::atomicCompareExchangeStrong(std::bit_cast<EncodedJSValue*>(&slot), JSValue::encode(JSValue()), JSValue::encode(JSValue(JSCell::seenMultipleCalleeObjects())));
}

void StructureRareData::cacheSpecialPropertySlow(JSGlobalObject* globalObject, VM& vm, Structure* ownStructure, JSValue value, CachedSpecialPropertyKey key, const PropertySlot& slot)
{
    UniquedStringImpl* uid = nullptr;
    switch (key) {
    case CachedSpecialPropertyKey::ToStringTag:
        uid = vm.propertyNames->toStringTagSymbol.impl();
        break;
    case CachedSpecialPropertyKey::ToString:
        uid = vm.propertyNames->toString.impl();
        break;
    case CachedSpecialPropertyKey::ValueOf:
        uid = vm.propertyNames->valueOf.impl();
        break;
    case CachedSpecialPropertyKey::ToPrimitive:
        uid = vm.propertyNames->toPrimitiveSymbol.impl();
        break;
    case CachedSpecialPropertyKey::ToJSON:
        uid = vm.propertyNames->toJSON.impl();
        break;
    }

    if (!ownStructure->propertyAccessesAreCacheable() || ownStructure->isProxy()) {
        giveUpOnSpecialPropertyCache(key);
        return;
    }

    ObjectPropertyConditionSet conditionSet;
    if (slot.isValue()) {
        // We don't handle the own property case of special properties (toString, valueOf, @@toPrimitive, @@toStringTag) because we would never know if a new
        // object transitioning to the same structure had the same value stored in that property.
        // Additionally, this is a super unlikely case anyway.
        if (!slot.isCacheable() || slot.slotBase()->structure() == ownStructure)
            return;

        // This will not create a condition for the current structure but that is good because we know that property
        // is not on the ownStructure so we will transisition if one is added and this cache will no longer be used.
        auto cacheStatus = prepareChainForCaching(globalObject, ownStructure, uid, slot.slotBase());
        if (!cacheStatus) {
            giveUpOnSpecialPropertyCache(key);
            return;
        }
        conditionSet = generateConditionsForPrototypePropertyHit(vm, this, globalObject, ownStructure, slot.slotBase(), uid);
        ASSERT(!conditionSet.isValid() || conditionSet.hasOneSlotBaseCondition());
    } else if (slot.isUnset()) {
        if (!ownStructure->propertyAccessesAreCacheableForAbsence()) {
            giveUpOnSpecialPropertyCache(key);
            return;
        }

        auto cacheStatus = prepareChainForCaching(globalObject, ownStructure, uid, nullptr);
        if (!cacheStatus) {
            giveUpOnSpecialPropertyCache(key);
            return;
        }
        conditionSet = generateConditionsForPropertyMiss(vm, this, globalObject, ownStructure, uid);
    } else
        return;

    if (!conditionSet.isValid()) {
        giveUpOnSpecialPropertyCache(key);
        return;
    }

    ObjectPropertyCondition equivCondition;
    for (const ObjectPropertyCondition& condition : conditionSet) {
        if (condition.condition().kind() == PropertyCondition::Presence) {
            ASSERT(isValidOffset(condition.offset()));
            condition.object()->structure()->startWatchingPropertyForReplacements(vm, condition.offset());
            equivCondition = condition.attemptToMakeEquivalenceWithoutBarrier();

            // The equivalence condition won't be watchable if we have already seen a replacement.
            if (!equivCondition.isWatchable(PropertyCondition::MakeNoChanges)) {
                giveUpOnSpecialPropertyCache(key);
                return;
            }
        } else if (!condition.isWatchable(PropertyCondition::MakeNoChanges)) {
            giveUpOnSpecialPropertyCache(key);
            return;
        }
    }

    ASSERT(conditionSet.structuresEnsureValidity());
    {
        // AUD1.N4(2): the multi-word entry install {miss watchpoints,
        // equivalence watchpoint, value} runs under the owning Structure's
        // m_lock — two threads for-in/Object.prototype.toString-ing a shared
        // structure otherwise interleave Bag appends and unique_ptr resets
        // (freeing a watchpoint another thread just installed). Condition-set
        // derivation above stays OUTSIDE the lock (it walks other structures
        // and allocates). The JIT-read word cache.m_value is published LAST,
        // after the watchpoints it summarizes; foreign fast-path readers
        // load it single-word (unlocked), per the ruling. No GC-allocating
        // call runs under the lock (makeUnique/Bag = fastMalloc; install()
        // takes no ConcurrentJSLock — verified against
        // AdaptiveInferredPropertyValueWatchpointBase::install).
        ConcurrentJSLocker locker(ownStructure->lock());
        auto& cache = ensureSpecialPropertyCache().m_cache[static_cast<unsigned>(key)];
        if (cache.m_value.get())
            return; // A racing installer (or a giveUp sentinel) won under the lock; keep its entry.
        for (ObjectPropertyCondition condition : conditionSet) {
            if (condition.condition().kind() == PropertyCondition::Presence) {
                cache.m_equivalenceWatchpoint = makeUnique<CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint>(equivCondition, this);
                cache.m_equivalenceWatchpoint->install(vm);
            } else
                cache.m_missWatchpoints.add(condition, this)->install(vm);
        }
        // AUD1.N4(2): the JIT-read word is RELEASE-published LAST. Holding
        // ownStructure->lock() orders nothing for the foreign fast-path
        // readers that load this word UNLOCKED (StructureRareDataInlines.h
        // cachedSpecialProperty / canCacheSpecialProperty and the baseline/
        // DFG inline reads) — on arm64 the m_value store could otherwise
        // become visible before the watchpoint-install stores it summarizes.
        // Same fence-then-publish shape the cached-property-names path
        // already uses (StructureRareDataInlines.h setCachedPropertyNames).
        WTF::storeStoreFence();
        cache.m_value.set(vm, this, value);
    }
}

// TSAN family structure-fields (AUD1.N4(3), races/forin-enumerator-cache.js):
// GC-retirement of the enumerator watchpoint FixedVector. Freeing the vector
// in place (the FixedVector reassignments in StructureRareDataInlines.h /
// clearCachedPropertyNameEnumerator) destroys StructureChainInvalidation-
// Watchpoints that are still linked into OTHER structures' transition
// watchpoint sets; a concurrent thread firing/walking one of those sets then
// touches freed memory. Outside a stop-the-world window the vector must
// therefore be MOVED (buffer pointer swap — watchpoint addresses are stable)
// into the retirement list and destroyed only in finalizeUnconditionally,
// when mutators are stopped. Callers hold the owning Structure's m_lock
// (Structure::setCachedPropertyNameEnumerator install path and the flag-on
// Structure::clearCachedPrototypeChain), which serializes the moves.
// A retired watchpoint that later fires only clears this cache again —
// conservative over-invalidation, never unsoundness.
void StructureRareData::retireCachedPropertyNameEnumeratorWatchpoints()
{
    if (m_cachedPropertyNameEnumeratorWatchpoints.isEmpty())
        return;
    m_retiredCachedPropertyNameEnumeratorWatchpoints.append(WTF::move(m_cachedPropertyNameEnumeratorWatchpoints));
    m_cachedPropertyNameEnumeratorWatchpoints = FixedVector<StructureChainInvalidationWatchpoint>();
}

// TSAN family structure-fields (§10.9 item (4), "Box reassign" key): flag-on
// publish-once install of the shared poly-proto watchpoint Box. The slot is
// written by ObjectAllocationProfile initialization and the rare-data clone
// path while foreign threads (LLInt slow paths, IC compilation) read it
// lock-free; a plain RefPtr reassign both tears the word and can deref a Box
// a reader is concurrently copying. CAS from null with winner-keeps removes
// replacement entirely: the established Box is never displaced, so nothing
// readers can hold is ever released under them, and the loser's candidate —
// never published — is deref'd locally. Winner-keeps is the same convention
// as the locked enumerator / special-property installs above; in practice
// racing installers carry the same Box (the executable's), so keeping the
// first is observably identical. Flag-off never reaches here.
void StructureRareData::setSharedPolyProtoWatchpointConcurrently(Box<InlineWatchpointSet>&& box)
{
    ASSERT(Options::useJSThreads());
    static_assert(sizeof(Box<InlineWatchpointSet>) == sizeof(uintptr_t), "single-word CAS publish treats the Box as one pointer word");
    // Detach the incoming Box's pointer word without giving up its ref: on
    // CAS success that ref becomes the member's; on failure we re-form the
    // Box and let its destructor deref.
    union Incoming {
        Incoming() : word(0) { }
        ~Incoming() { } // ownership handling is explicit below
        uintptr_t word;
        Box<InlineWatchpointSet> box;
    } incoming;
    new (&incoming.box) Box<InlineWatchpointSet>(WTF::move(box));
    if (!incoming.word)
        return; // Installing an empty Box is a no-op flag-on (slot is monotonic null -> Box).
    uintptr_t prior = WTF::atomicCompareExchangeStrong(reinterpret_cast<uintptr_t*>(&m_polyProtoWatchpoint), static_cast<uintptr_t>(0), incoming.word);
    if (!prior)
        return; // Published: the ref moved into the member word.
    // Lost (or the same Box was already installed): keep the established
    // entry, drop our candidate's ref. It was never visible to anyone.
    incoming.box.~Box<InlineWatchpointSet>();
}

Box<InlineWatchpointSet> StructureRareData::copySharedPolyProtoWatchpointConcurrently() const
{
    ASSERT(Options::useJSThreads());
    static_assert(sizeof(Box<InlineWatchpointSet>) == sizeof(uintptr_t));
    // Relaxed atomic load of the pointer word, then a thread-safe ref through
    // a borrowed (non-owning) view. Lifetime: the slot is publish-once
    // flag-on (see above) and holds its ref until this cell is swept, and a
    // reader can only reach this rare data through a live Structure — so the
    // pointee cannot die between the load and the ref.
    union Borrowed {
        Borrowed() : word(0) { }
        ~Borrowed() { } // never run the Box dtor — this view owns no ref
        uintptr_t word;
        Box<InlineWatchpointSet> box;
    } borrowed;
    borrowed.word = WTF::atomicLoad(reinterpret_cast<uintptr_t*>(const_cast<Box<InlineWatchpointSet>*>(&m_polyProtoWatchpoint)), std::memory_order_relaxed);
    return borrowed.box;
}

void StructureRareData::clearCachedPropertyNameEnumeratorRetiringWatchpoints()
{
    // The flag word is read lock-free (single word) by foreign fast paths and
    // the JIT; the member's RelaxedAtomicUintPtr wrapper publishes the clear
    // atomically (relaxed — same str/mov).
    m_cachedPropertyNameEnumeratorAndFlag = 0;
    retireCachedPropertyNameEnumeratorWatchpoints();
}

// Lock context (AUD1.N4 / K4.VI.2): callers are watchpoint FIRES (GIL-off
// these run only inside a §A.3 stop or with the world stopped — jit deopt
// machinery) and finalizeUnconditionally (GC end phase, mutators stopped) —
// never a free-running foreign mutator, so the entry teardown below needs no
// lock against the locked install path above.
void StructureRareData::clearCachedSpecialProperty(CachedSpecialPropertyKey key)
{
    auto* objectToStringCache = m_specialPropertyCache.get();
    if (!objectToStringCache)
        return;
    auto& cache = objectToStringCache->m_cache[static_cast<unsigned>(key)];
    cache.m_missWatchpoints.clear();
    cache.m_equivalenceWatchpoint.reset();
    if (cache.m_value.get() != JSCell::seenMultipleCalleeObjects())
        cache.m_value.clear();
}

void StructureRareData::finalizeUnconditionally(VM& vm, CollectionScope)
{
    // Drain the GC-retired enumerator watchpoint vectors: mutators are
    // stopped here, so destroying the watchpoints (which unlinks them from
    // the watched structures' transition watchpoint sets) cannot race a
    // foreign walker. Cells die only at the following sweep, so the sets the
    // destructors touch are still valid memory.
    if (!m_retiredCachedPropertyNameEnumeratorWatchpoints.isEmpty()) [[unlikely]]
        m_retiredCachedPropertyNameEnumeratorWatchpoints.clear();

    if (m_specialPropertyCache) {
        auto clearCacheIfInvalidated = [&](CachedSpecialPropertyKey key) {
            auto& cache = m_specialPropertyCache->m_cache[static_cast<unsigned>(key)];
            if (cache.m_equivalenceWatchpoint) {
                if (!cache.m_equivalenceWatchpoint->key().isStillLive(vm)) {
                    clearCachedSpecialProperty(key);
                    return;
                }
            }
            for (auto* watchpoint : cache.m_missWatchpoints) {
                if (!watchpoint->key().isStillLive(vm)) {
                    clearCachedSpecialProperty(key);
                    return;
                }
            }
        };

        for (unsigned index = 0; index < numberOfCachedSpecialPropertyKeys; ++index)
            clearCacheIfInvalidated(static_cast<CachedSpecialPropertyKey>(index));
    }
}

// ------------- Methods for Object.prototype.toString() helper watchpoint classes --------------

WTF_MAKE_TZONE_ALLOCATED_IMPL(CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint);

CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint::CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint(const ObjectPropertyCondition& key, StructureRareData* structureRareData)
    : Base(key)
    , m_structureRareData(structureRareData)
{
}

bool CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint::isValid() const
{
    return !m_structureRareData->isPendingDestruction();
}

void CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint::handleFire(VM& vm, const FireDetail&)
{
    CachedSpecialPropertyKey key = CachedSpecialPropertyKey::ToStringTag;
    if (this->key().uid() == vm.propertyNames->toStringTagSymbol.impl())
        key = CachedSpecialPropertyKey::ToStringTag;
    else if (this->key().uid() == vm.propertyNames->toString.impl())
        key = CachedSpecialPropertyKey::ToString;
    else if (this->key().uid() == vm.propertyNames->valueOf.impl())
        key = CachedSpecialPropertyKey::ValueOf;
    else if (this->key().uid() == vm.propertyNames->toJSON.impl())
        key = CachedSpecialPropertyKey::ToJSON;
    else {
        ASSERT(this->key().uid() == vm.propertyNames->toPrimitiveSymbol.impl());
        key = CachedSpecialPropertyKey::ToPrimitive;
    }
    m_structureRareData->clearCachedSpecialProperty(key);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
