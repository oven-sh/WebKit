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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma once

#include "BigIntPrototype.h"
#include "BrandedStructure.h"
#include "HeapCellInlines.h"
#include "JSArrayBufferView.h"
#include "JSGlobalObject.h"
#include "JSObjectInlines.h"
#include "PropertyTable.h"
#include "StringPrototype.h"
#include "StructureArrayStorageInlines.h"
#include "StructureChain.h"
#include "StructureCreateInlines.h"
#include "StructureInlinesLight.h"
#include "StructureRareDataInlines.h"
#include "SymbolPrototype.h"
#include "WebAssemblyGCStructure.h"
#include "WriteBarrierInlines.h"
#include <wtf/IterationStatus.h>
#include <wtf/Threading.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

inline Structure* Structure::create(VM& vm, Structure* previous, DeferredStructureTransitionWatchpointFire* deferred)
{
    ASSERT(vm.structureStructure);
    switch (previous->variant()) {
    case StructureVariant::Normal: {
        auto* result = new (NotNull, allocateCell<Structure>(vm)) Structure(vm, previous->variant(), previous);
        result->finishCreation(vm, previous, deferred);
        return result;
    }
    case StructureVariant::Branded: {
        auto* result = new (NotNull, allocateCell<BrandedStructure>(vm)) BrandedStructure(vm, uncheckedDowncast<BrandedStructure>(previous));
        result->finishCreation(vm, previous, deferred);
        return result;
    }
    case StructureVariant::WebAssemblyGC: {
        RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("WebAssemblyGCStructure should not do transition");
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

template<typename Functor>
void Structure::forEachPropertyConcurrently(const Functor& functor)
{
    Vector<Structure*, 8> structures;
    Structure* tableStructure;
    PropertyTable* table;
    
    bool didFindStructure = findStructuresAndMapForMaterialization(structures, tableStructure, table);

    UncheckedKeyHashSet<UniquedStringImpl*> seenProperties;

    for (auto* structure : structures) {
        if (!structure->m_transitionPropertyName || seenProperties.contains(structure->m_transitionPropertyName.get()))
            continue;

        seenProperties.add(structure->m_transitionPropertyName.get());

        switch (structure->transitionKind()) {
        case TransitionKind::PropertyAddition:
        case TransitionKind::PropertyAttributeChange:
            break;
        case TransitionKind::PropertyDeletion:
        case TransitionKind::SetBrand:
            continue;
        default:
            ASSERT_NOT_REACHED();
            break;
        }

        if (!functor(PropertyTableEntry(structure->m_transitionPropertyName.get(), structure->transitionOffset(), structure->transitionPropertyAttributes()))) {
            if (didFindStructure) {
                assertIsHeld(tableStructure->m_lock); // Sadly Clang needs some help here.
                tableStructure->m_lock.unlock();
            }
            return;
        }
    }
    
    if (didFindStructure) {
        assertIsHeld(tableStructure->m_lock); // Sadly Clang needs some help here.
        table->forEachProperty([&](const auto& entry) {
            if (seenProperties.contains(entry.key()))
                return IterationStatus::Continue;

            if (!functor(entry))
                return IterationStatus::Done;

            return IterationStatus::Continue;
        });
        tableStructure->m_lock.unlock();
    }
}

template<typename Functor>
void Structure::forEachProperty(VM& vm, const Functor& functor)
{
    // SPEC-objectmodel L6(iii) (Task 3c): flag-on, this mutator uncached table
    // WALK holds m_lock across the iteration, so locked mutations/rehashes of
    // a published table (and steals via takePropertyTableOrCloneIfPinned)
    // cannot tear it (I37). Table order is preserved (callers, e.g. the JSON
    // fast stringifier, rely on insertion order — forEachPropertyConcurrently
    // would not provide it). GCSafe locker = m_lock + DeferGC: the functor may
    // GC-allocate under the lock, O1's sanctioned pre-lock-DeferGC form. The
    // retry loop re-materializes if the table was stolen between
    // materialization and lock acquisition. Flag-off: today's lock-free walk
    // (I22). Callers must not hold this structure's m_lock.
    if (Options::useJSThreads()) [[unlikely]] {
        while (true) {
            PropertyTable* table = ensurePropertyTableIfNotEmpty(vm);
            if (!table)
                return;
            GCSafeConcurrentJSLocker locker(m_lock, vm);
            if (propertyTableOrNull() != table)
                continue; // Stolen by a racing transition: rebuild and retry.
            table->forEachProperty([&](const auto& entry) {
                if (!functor(entry))
                    return IterationStatus::Done;
                return IterationStatus::Continue;
            });
            ensureStillAliveHere(table);
            return;
        }
    }

    if (PropertyTable* table = ensurePropertyTableIfNotEmpty(vm)) {
        table->forEachProperty([&](const auto& entry) {
            if (!functor(entry))
                return IterationStatus::Done;
            return IterationStatus::Continue;
        });
        ensureStillAliveHere(table);
    }
}

inline void Structure::setCachedPropertyNames(VM& vm, CachedPropertyNamesKind kind, JSCellButterfly* cached)
{
    ensureRareData(vm)->setCachedPropertyNames(vm, kind, cached);
}

ALWAYS_INLINE JSValue prototypeForLookupPrimitiveImpl(JSGlobalObject* globalObject, const Structure* structure)
{
    ASSERT(!structure->isObject());

    if (structure->typeInfo().type() == StringType)
        return globalObject->stringPrototype();
    
    if (structure->typeInfo().type() == HeapBigIntType)
        return globalObject->bigIntPrototype();

    ASSERT(structure->typeInfo().type() == SymbolType);
    return globalObject->symbolPrototype();
}

inline JSValue Structure::prototypeForLookup(JSGlobalObject* globalObject) const
{
    ASSERT(hasMonoProto());
    if (isObject())
        return storedPrototype();
    return prototypeForLookupPrimitiveImpl(globalObject, this);
}

inline JSValue Structure::prototypeForLookup(JSGlobalObject* globalObject, JSCell* base) const
{
    // SINGLE-MUTATOR staleness tripwire, reinterpreted exactly like
    // storedPrototype(object) (StructureInlinesLight.h): under useJSThreads a
    // racing foreign transition legitimately re-tags base's structureID while
    // a reader walks with its SAMPLED structure (SPEC-objectmodel M7/I24).
    // The body stays sound on the sample: mono-proto reads THIS structure's
    // immutable m_prototype; poly-proto reads base's inline slot (value
    // staleness blessed, OM C4). Flag-off: assert unchanged.
    ASSERT(Options::useJSThreads() || base->structure() == this);
    if (isObject())
        return storedPrototype(asObject(base));
    return prototypeForLookupPrimitiveImpl(globalObject, this);
}

inline StructureChain* Structure::prototypeChain(VM& vm, JSGlobalObject* globalObject, JSObject* base) const
{
    // See prototypeForLookup above: sampled-structure readers are legal under
    // useJSThreads (SPEC-objectmodel M7/I24). The chain is derived from and
    // cached on the SAMPLED structure — exactly the association its callers
    // (which hold the sample) want; the cache-slot publication is already
    // relaxed-atomic against concurrent readers (TSAN §10.9 note below).
    // Flag-off: assert unchanged.
    ASSERT(Options::useJSThreads() || base->structure() == this);
    // We cache our prototype chain so our clients can share it.
    if (!isValid(globalObject, m_cachedPrototypeChain.get(), base)) {
        JSValue prototype = prototypeForLookup(globalObject, base);
        const_cast<Structure*>(this)->clearCachedPrototypeChain();
        // TSAN family structure-fields (§10.9 prototypeChain key, 8 reports):
        // this is the LOCK-FREE writer of the chain slot; the readers
        // (cachedPrototypeChainConcurrently, canCachePropertyNameEnumerator,
        // the GC null store in visitChildrenImpl) are relaxed-atomic, but
        // .set()'s setEarlyValue is a RawPtrTraits::exchange PLAIN store.
        // Relaxed-atomic store + explicit barrier — identical codegen. The
        // chain's lanes were release-published by StructureChain::
        // finishCreation's constructor-tail fence before this publish store.
        StructureChain* chain = StructureChain::create(vm, prototype.isNull() ? nullptr : asObject(prototype));
        ASSERT(!Options::useConcurrentJIT() || !isCompilationThread()); // Same assert .set() performed.
        validateCell(chain);
        m_cachedPrototypeChain.setWithoutWriteBarrier(chain);
        vm.writeBarrier(this, chain);
    }
    return m_cachedPrototypeChain.get();
}

inline bool Structure::isValid(JSGlobalObject* globalObject, StructureChain* cachedPrototypeChain, JSObject* base) const
{
    if (!cachedPrototypeChain)
        return false;

    JSValue prototype = prototypeForLookup(globalObject, base);
    StructureID* cachedStructure = cachedPrototypeChain->head();
    // TSAN family structure-fields: relaxed 32-bit loads of the chain lanes —
    // StructureChain::finishCreation writes them with relaxed atomic stores on
    // possibly recycled auxiliary memory (see that function). Same mov codegen.
    auto loadCachedStructureID = [](StructureID* lane) {
        static_assert(sizeof(StructureID) == sizeof(uint32_t));
        return std::bit_cast<StructureID>(WTF::atomicLoad(reinterpret_cast<uint32_t*>(lane), std::memory_order_relaxed));
    };
    StructureID cachedStructureID = loadCachedStructureID(cachedStructure);
    while (cachedStructureID && !prototype.isNull()) {
        if (asObject(prototype)->structureID() != cachedStructureID)
            return false;
        ++cachedStructure;
        cachedStructureID = loadCachedStructureID(cachedStructure);
        prototype = asObject(prototype)->getPrototypeDirect();
    }
    return prototype.isNull() && !cachedStructureID;
}

inline void Structure::didCachePropertyReplacement(VM& vm, PropertyOffset offset)
{
    ASSERT(isValidOffset(offset));
    firePropertyReplacementWatchpointSet(vm, offset, "Did cache property replacement");
}

inline WatchpointSet* Structure::propertyReplacementWatchpointSet(PropertyOffset offset)
{
    ConcurrentJSLocker locker(m_lock);
    StructureRareData* rareData = tryRareData();
    if (!rareData)
        return nullptr;
    if (!rareData->m_replacementWatchpointSets.isNullStorage())
        return rareData->m_replacementWatchpointSets.get(offset);
    return nullptr;
}

inline size_t nextOutOfLineStorageCapacity(size_t currentCapacity)
{
    if (!currentCapacity)
        return initialOutOfLineCapacity;
    return currentCapacity * outOfLineGrowthFactor;
}

inline void Structure::cacheSpecialProperty(JSGlobalObject* globalObject, VM& vm, JSValue value, CachedSpecialPropertyKey key, const PropertySlot& slot)
{
    if (!hasRareData())
        allocateRareData(vm);
    rareData()->cacheSpecialProperty(globalObject, vm, this, value, key, slot);
}

template<Structure::ShouldPin shouldPin, typename Func>
inline PropertyOffset Structure::add(VM& vm, PropertyName propertyName, unsigned attributes, const Func& func)
{
    ASSERT(!isCompilationThread());
    PropertyTable* table = ensurePropertyTable(vm);

    GCSafeConcurrentJSLocker locker(m_lock, vm);

    switch (shouldPin) {
    case ShouldPin::Yes:
        pin(locker, vm, table);
        break;
    case ShouldPin::No:
        setPropertyTable(vm, table);
        break;
    }

    // SPEC-objectmodel L6 (Task 3c): query the table directly — m_lock is held
    // here, and flag-on Structure::get() itself acquires m_lock (getConcurrently
    // routing), so calling it from under the lock would self-deadlock.
    ASSERT(!JSC::isValidOffset(std::get<0>(table->get(propertyName.uid()))));

    checkConsistency();
    if (attributes & PropertyAttribute::DontEnum || propertyName.isSymbol())
        setIsQuickPropertyAccessAllowedForEnumeration(false);
    if (attributes & PropertyAttribute::ReadOnly)
        setContainsReadOnlyProperties();
    if (attributes & PropertyAttribute::DontEnum)
        setHasNonEnumerableProperties(true);
    if (attributes & PropertyAttribute::DontDelete) {
        setHasNonConfigurableProperties(true);
        if (attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)
            setHasNonConfigurableReadOnlyOrGetterSetterProperties(true);
    }
    if (propertyName == vm.propertyNames->underscoreProto)
        setHasUnderscoreProtoPropertyExcludingOriginalProto(true);
    else if (propertyName == vm.propertyNames->then)
        setHasSpecialProperties(true);

    auto rep = propertyName.uid();

    PropertyOffset newOffset = table->nextOffset(m_inlineCapacity);

    m_propertyHash = m_propertyHash ^ rep->existingSymbolAwareHash();
    m_seenProperties.add(CompactPtr<UniquedStringImpl>::encode(rep));

    auto [offset, attribute, result] = table->add(vm, PropertyTableEntry(rep, newOffset, attributes));
    ASSERT_UNUSED(result, result);
    ASSERT_UNUSED(offset, offset == newOffset);
    UNUSED_VARIABLE(attribute);
    auto newMaxOffset = std::max(newOffset, maxOffset());
    
    func(locker, newOffset, newMaxOffset);
    
    ASSERT(maxOffset() == newMaxOffset);

    checkConsistency();
    return newOffset;
}

template<Structure::ShouldPin shouldPin, typename Func>
inline PropertyOffset Structure::remove(VM& vm, PropertyName propertyName, const Func& func)
{
    ASSERT(!isCompilationThread());
    PropertyTable* table = ensurePropertyTable(vm);
    GCSafeConcurrentJSLocker locker(m_lock, vm);

    switch (shouldPin) {
    case ShouldPin::Yes:
        pin(locker, vm, table);
        break;
    case ShouldPin::No:
        setPropertyTable(vm, table);
        break;
    }

    // SPEC-objectmodel L6 (Task 3c): direct table query; see Structure::add.
    ASSERT(JSC::isValidOffset(std::get<0>(table->get(propertyName.uid()))));

    checkConsistency();

    auto rep = propertyName.uid();

    auto [offset, attributes] = table->take(vm, rep);
    UNUSED_VARIABLE(attributes);
    if (offset == invalidOffset)
        return invalidOffset;

    setIsQuickPropertyAccessAllowedForEnumeration(false);

    table->addDeletedOffset(offset);

    PropertyOffset newMaxOffset = maxOffset();

    func(locker, offset, newMaxOffset);

    ASSERT(maxOffset() == newMaxOffset);
    // SPEC-objectmodel L6 (Task 3c): direct table query; see Structure::add.
    ASSERT(!JSC::isValidOffset(std::get<0>(table->get(propertyName.uid()))));

    checkConsistency();
    return offset;
}

template<Structure::ShouldPin shouldPin, typename Func>
inline PropertyOffset Structure::attributeChange(VM& vm, PropertyName propertyName, unsigned attributes, const Func& func)
{
    ASSERT(!isCompilationThread());
    PropertyTable* table = ensurePropertyTable(vm);

    GCSafeConcurrentJSLocker locker(m_lock, vm);

    switch (shouldPin) {
    case ShouldPin::Yes:
        pin(locker, vm, table);
        break;
    case ShouldPin::No:
        setPropertyTable(vm, table);
        break;
    }

    // SPEC-objectmodel L6 (Task 3c): direct table query; see Structure::add.
    ASSERT(JSC::isValidOffset(std::get<0>(table->get(propertyName.uid()))));

    checkConsistency();
    PropertyOffset offset = table->updateAttributeIfExists(propertyName.uid(), attributes);
    if (offset == invalidOffset)
        return offset;

    if (attributes & PropertyAttribute::DontEnum) {
        setHasNonEnumerableProperties(true);
        setIsQuickPropertyAccessAllowedForEnumeration(false);
    }
    if (attributes & PropertyAttribute::DontDelete) {
        setHasNonConfigurableProperties(true);
        if (attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)
            setHasNonConfigurableReadOnlyOrGetterSetterProperties(true);
    }
    if (attributes & PropertyAttribute::ReadOnly)
        setContainsReadOnlyProperties();

    PropertyOffset newMaxOffset = maxOffset();

    func(locker, offset, newMaxOffset);

    ASSERT(maxOffset() == newMaxOffset);
    // SPEC-objectmodel L6 (Task 3c): direct table query; see Structure::add.
    ASSERT(JSC::isValidOffset(std::get<0>(table->get(propertyName.uid()))));

    checkConsistency();
    return offset;
}

template<typename Func>
inline PropertyOffset Structure::addPropertyWithoutTransition(VM& vm, PropertyName propertyName, unsigned attributes, const Func& func)
{
    return add<ShouldPin::Yes>(vm, propertyName, attributes, func);
}

template<typename Func>
inline PropertyOffset Structure::removePropertyWithoutTransition(VM& vm, PropertyName propertyName, const Func& func)
{
    ASSERT(isUncacheableDictionary());
    ASSERT(isPinnedPropertyTable());
    ASSERT(propertyTableOrNull());
    
    return remove<ShouldPin::Yes>(vm, propertyName, func);
}

template<typename Func>
ALWAYS_INLINE auto Structure::addOrReplacePropertyWithoutTransition(VM& vm, PropertyName propertyName, unsigned newAttributes, const Func& func) -> decltype(auto)
{
    ASSERT(!isCompilationThread());

    // SPEC-objectmodel L6(ii)/(iii) (Task 3c) — mode-split, I22: the flag-on
    // arm below holds m_lock across find+mutate (steal-recheck loop preserved
    // verbatim); the flag-off arm is today's pre-threads body, bit- and
    // branch-identical (no std::optional locker machinery; the lock is taken
    // AFTER the find, exactly the old placement). Race statement, flag-on:
    // the uncached find WALK and the subsequent mutation of this PUBLISHED
    // table must form one m_lock critical section, so a racing locked
    // add/rehash can neither tear the walk nor invalidate `findResult`
    // between find() and addAfterFind() (I37). The loop re-checks under the
    // lock that `table` is still this structure's published table — a racing
    // transition may have stolen it via takePropertyTableOrCloneIfPinned
    // (then it is private to the thief and must not be touched). GCSafe
    // locker = m_lock + DeferGC, O1's sanctioned form for the allocating
    // addAfterFind. Flag-off: exactly one mutator exists (no Thread()
    // without useJSThreads), so no steal and no concurrent rehash is
    // possible between find and lock — the pre-threads lock-after-find
    // placement is unconditionally correct (I22).
    //
    // DUPLICATION GUARD: the tail below (pin .. checkConsistency .. return)
    // appears VERBATIM in both arms — the flag-on arm cannot be outlined
    // into a member sibling without a Structure.h declaration (and a
    // friend-free file-local helper cannot compile: pin() is private). Any
    // change to one tail MUST be mirrored in the other.
    if (Options::useJSThreads()) [[unlikely]] {
        // ===== flag-on arm (mirror of the flag-off tail below) =====
        // F1 residual (AB17g): outlined into a NEVER_INLINE IIFE so that
        // flag-off instantiations of every put site carry only the
        // predicted-false byte test + a never-taken call, not the
        // GCSafeConcurrentJSLocker + steal-retry machinery inlined into an
        // ALWAYS_INLINE template (icache/register-pressure parity with
        // pre-threads). Pure code placement: an identical instruction
        // sequence executes in both modes — the m_lock critical section
        // (find + mutate under one locker, steal-recheck loop) is unchanged,
        // so no new interleaving exists. INVARIANT (apply-time check): every
        // control path in this arm returns THROUGH the lambda; any
        // fall-through into the flag-off tail below is a semantic change,
        // not code placement. If a CI toolchain rejects NEVER_INLINE in this
        // position, this outlining is deferred outside-scope (revert to the
        // plain arm) — do NOT substitute a file-local helper (see guard
        // note above).
        return ([&]() NEVER_INLINE -> std::tuple<PropertyOffset, unsigned, bool> {
        PropertyTable* table = ensurePropertyTable(vm);

        auto rep = propertyName.uid();

        std::optional<GCSafeConcurrentJSLocker> l6Locker;
        while (true) {
            l6Locker.emplace(m_lock, vm);
            if (propertyTableOrNull() == table)
                break;
            l6Locker.reset();
            table = ensurePropertyTable(vm);
        }
        const GCSafeConcurrentJSLocker& locker = *l6Locker;

        auto findResult = table->find(rep);
        if (findResult.offset != invalidOffset)
            return std::tuple { findResult.offset, findResult.attributes, false };

        pin(locker, vm, table);

        // SPEC-objectmodel L6 (Task 3c): direct table query; see Structure::add.
        ASSERT(!JSC::isValidOffset(std::get<0>(table->get(propertyName.uid()))));

        checkConsistency();
        if (newAttributes & PropertyAttribute::DontEnum || propertyName.isSymbol())
            setIsQuickPropertyAccessAllowedForEnumeration(false);
        if (newAttributes & PropertyAttribute::ReadOnly)
            setContainsReadOnlyProperties();
        if (newAttributes & PropertyAttribute::DontEnum)
            setHasNonEnumerableProperties(true);
        if (newAttributes & PropertyAttribute::DontDelete) {
            setHasNonConfigurableProperties(true);
            if (newAttributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)
                setHasNonConfigurableReadOnlyOrGetterSetterProperties(true);
        }
        if (propertyName == vm.propertyNames->underscoreProto)
            setHasUnderscoreProtoPropertyExcludingOriginalProto(true);
        else if (propertyName == vm.propertyNames->then)
            setHasSpecialProperties(true);

        PropertyOffset newOffset = table->nextOffset(m_inlineCapacity);

        m_propertyHash = m_propertyHash ^ rep->existingSymbolAwareHash();
        m_seenProperties.add(CompactPtr<UniquedStringImpl>::encode(rep));

        auto [offset, attributes, result] = table->addAfterFind(vm, PropertyTableEntry(rep, newOffset, newAttributes), WTF::move(findResult));
        ASSERT_UNUSED(result, result);
        ASSERT_UNUSED(offset, offset == newOffset);
        UNUSED_VARIABLE(attributes);
        auto newMaxOffset = std::max(newOffset, maxOffset());

        func(locker, newOffset, newMaxOffset);

        ASSERT(maxOffset() == newMaxOffset);

        checkConsistency();
        return std::tuple { newOffset, newAttributes, true };
        })();
    }

    // ===== flag-off arm: pre-threads body (mirror of the flag-on tail above) =====
    PropertyTable* table = ensurePropertyTable(vm);

    auto rep = propertyName.uid();

    auto findResult = table->find(rep);
    if (findResult.offset != invalidOffset)
        return std::tuple { findResult.offset, findResult.attributes, false };

    GCSafeConcurrentJSLocker locker(m_lock, vm);

    pin(locker, vm, table);

    // SPEC-objectmodel L6 (Task 3c): direct table query; see Structure::add.
    ASSERT(!JSC::isValidOffset(std::get<0>(table->get(propertyName.uid()))));

    checkConsistency();
    if (newAttributes & PropertyAttribute::DontEnum || propertyName.isSymbol())
        setIsQuickPropertyAccessAllowedForEnumeration(false);
    if (newAttributes & PropertyAttribute::ReadOnly)
        setContainsReadOnlyProperties();
    if (newAttributes & PropertyAttribute::DontEnum)
        setHasNonEnumerableProperties(true);
    if (newAttributes & PropertyAttribute::DontDelete) {
        setHasNonConfigurableProperties(true);
        if (newAttributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessorOrValue)
            setHasNonConfigurableReadOnlyOrGetterSetterProperties(true);
    }
    if (propertyName == vm.propertyNames->underscoreProto)
        setHasUnderscoreProtoPropertyExcludingOriginalProto(true);
    else if (propertyName == vm.propertyNames->then)
        setHasSpecialProperties(true);

    PropertyOffset newOffset = table->nextOffset(m_inlineCapacity);

    m_propertyHash = m_propertyHash ^ rep->existingSymbolAwareHash();
    m_seenProperties.add(CompactPtr<UniquedStringImpl>::encode(rep));

    auto [offset, attributes, result] = table->addAfterFind(vm, PropertyTableEntry(rep, newOffset, newAttributes), WTF::move(findResult));
    ASSERT_UNUSED(result, result);
    ASSERT_UNUSED(offset, offset == newOffset);
    UNUSED_VARIABLE(attributes);
    auto newMaxOffset = std::max(newOffset, maxOffset());

    func(locker, newOffset, newMaxOffset);

    ASSERT(maxOffset() == newMaxOffset);

    checkConsistency();
    return std::tuple { newOffset, newAttributes, true };
}

template<typename Func>
inline PropertyOffset Structure::attributeChangeWithoutTransition(VM& vm, PropertyName propertyName, unsigned attributes, const Func& func)
{
    return attributeChange<ShouldPin::Yes>(vm, propertyName, attributes, func);
}

ALWAYS_INLINE void Structure::setPrototypeWithoutTransition(VM& vm, JSValue prototype)
{
    ASSERT(isValidPrototype(prototype));
    m_prototype.set(vm, this, prototype);
}

ALWAYS_INLINE void Structure::setRealm(VM& vm, JSGlobalObject* globalObject)
{
    // TSAN family structure-fields: realm()/globalObject() readers are
    // relaxed-atomic (WriteBarrierBase::get) and may probe recycled /
    // just-published Structures; .set()'s setEarlyValue is a plain exchange.
    // Relaxed store + explicit barrier, identical codegen.
    ASSERT(globalObject);
    ASSERT(!Options::useConcurrentJIT() || !isCompilationThread()); // Same assert .set() performed.
    validateCell(globalObject);
    m_realm.setWithoutWriteBarrier(globalObject);
    vm.writeBarrier(this, globalObject);
}

ALWAYS_INLINE void Structure::setPropertyTable(VM& vm, PropertyTable* table)
{
    // TSAN families structure-fields/property-table (§10.9 fixShape (3)):
    // readers of this slot (propertyTableOrNull / ensurePropertyTable* via
    // WriteBarrierBase::get, and the GC clear in visitChildrenImpl) are
    // relaxed-atomic, but setMayBeNull stores through setEarlyValue ->
    // RawPtrTraits::exchange, a PLAIN store — the 26+36-report writer key.
    // Route the store through the relaxed-atomic storeCell
    // (setWithoutWriteBarrier) + an explicit barrier — same validation, same
    // barrier, identical single-mov codegen flag-off.
    if (table)
        validateCell(table);
    m_propertyTableUnsafe.setWithoutWriteBarrier(table);
    vm.writeBarrier(this, table);
}

ALWAYS_INLINE void Structure::setPreviousID(VM& vm, Structure* structure)
{
    if (hasRareData())
        rareData()->setPreviousID(vm, structure);
    else {
        // TSAN family structure-fields (§10.9 setPreviousID key, 5 reports):
        // previousID() readers load this slot lock-free with relaxed/acquire
        // atomics (previousOrRareDataConcurrently, Structure.h) and the
        // flag-on clearPreviousID/allocateRareData writers CAS it; .set()'s
        // setEarlyValue plain exchange was the last unpaired writer. Same
        // shape as setPropertyTable above: validate + relaxed store + barrier.
        ASSERT(structure);
        ASSERT(!Options::useConcurrentJIT() || !isCompilationThread()); // Same assert .set() performed.
        validateCell(static_cast<JSCell*>(structure));
        m_previousOrRareData.setWithoutWriteBarrier(structure);
        vm.writeBarrier(this, structure);
    }
}

inline void Structure::pin(const AbstractLocker&, VM& vm, PropertyTable* table)
{
    setIsPinnedPropertyTable(true);
    setPropertyTable(vm, table);
    clearPreviousID();
    m_transitionPropertyName = nullptr;
    // SPEC-objectmodel F3: transition-time callers follow this with
    // fireTTLWatchpointSetsAfterPinning(vm, source) AFTER releasing m_lock
    // (fires may STW; never stop-the-world while holding a §6-ranked lock - O2).
}

// Flag-on slow path of DEFINE_BITFIELD's set##upperName (see Structure.h):
// the lost-update CAS loop, outlined so flag-off setter call sites carry only
// a predicted-false byte test + a never-taken call. Flag-on, this path
// immediately enters a CAS retry loop, so one extra call is noise.
NEVER_INLINE inline void Structure::setBitFieldConcurrently(uint32_t setBits, uint32_t fieldBits)
{
    uint32_t oldWord = WTF::atomicLoad(&m_bitField, std::memory_order_relaxed);
    while (true) {
        uint32_t newWord = (oldWord & ~fieldBits) | setBits;
        if (newWord == oldWord)
            return;
        uint32_t observed = WTF::atomicCompareExchangeStrong(&m_bitField, oldWord, newWord);
        if (observed == oldWord)
            return;
        oldWord = observed;
    }
}

// SPEC-objectmodel E4 (Task 3). See the declaration in Structure.h for the full
// contract. The caller passes the freshly loaded 64-bit tagged butterfly word of
// the instance being transitioned (JSObject::taggedButterflyWord()).
ALWAYS_INLINE bool Structure::mayTransitionLockFreeFromThisStructure(const JSCell* cell, uint64_t taggedButterflyWord) const
{
    if (!Options::useJSThreads()) [[likely]]
        return true; // E3/I22: flag-off, today's lock-free code is unconditionally correct.

    // I15: both SOURCE sets valid AND watched (I14).
    if (!transitionThreadLocalIsValidAndWatched() || !writeThreadLocalIsValidAndWatched())
        return false;

    // I36: PreciseAllocation cells sit at 8-mod-16 addresses - no 16-byte
    // header+butterfly DCAS pairing exists for them, so every PA transition is
    // cell-locked; E4 is excluded outright (one bit test of the cell pointer).
    if (cell->isPreciseAllocation())
        return false;

    // I31 (review round 3): ArrayStorage-shaped instances are excluded from E4.
    // Flag-on, EVERY AS access is cell-locked and every AS relayout is the
    // cell-locked §4.6 AS-COPY; an E4 owner transition's lock-free butterfly
    // copy (allocateMoreOutOfLineStorage copies the AS payload too) must never
    // race those. With this exclusion, AS transitions always take the locked
    // protocols / the §4.6 per-event stops, whose publications preserve the tag
    // verbatim. (SPEC-jit §5.5 mirrors this predicate: the emitted form must
    // carry the same AS-shape exclusion - recorded in INTEGRATE-objectmodel.)
    if (hasAnyArrayStorage(indexingType()))
        return false;

    if (taggedButterflyWord & butterflyPointerMask) {
        // Butterfly-bearing: ownership is the INSTANCE tag - exactly
        // (currentButterflyTID(), SW=0). No structure-TID compare: a thread
        // transitioning its own instance through a foreign-created shape stays
        // lock-free (§5 E4/F2, r12 per-object keying).
        return (taggedButterflyWord & butterflyTagMask) == encodeButterflyTag(currentButterflyTID(), false);
    }

    ASSERT(!taggedButterflyWord); // §2: payload 0 + nonzero tag is illegal.
    // Butterfly-less (incl. N2 structure-only transitions): ownership is the
    // structure's transition TID (N1).
    return currentButterflyTID() == m_transitionThreadLocalTID;
}

// SPEC-objectmodel I29 (Task 3): the final re-validation of an E4 lock-free
// transition. Protocol at every E4 site (all tiers; runtime sites here, JIT
// emission in SPEC-jit §5.5):
//   1. allocate everything (new butterfly etc.) FIRST;
//   2. re-validate with FRESH loads via this helper;
//   3. with NO poll/allocation/safepoint in between (bracket the window with
//      AssertNoGC in debug builds), store the value, nuke, store the butterfly
//      word (currentTID, SW=0), store the new StructureID (today's order, M5);
//   4. on false: fall back to the §4.3 locked protocol (never spin here).
ALWAYS_INLINE bool Structure::revalidateLockFreeTransition(const JSCell* cell, uint64_t freshTaggedButterflyWord) const
{
    return mayTransitionLockFreeFromThisStructure(cell, freshTaggedButterflyWord);
}

ALWAYS_INLINE bool Structure::shouldConvertToPolyProto(const Structure* a, const Structure* b)
{
    if (!a || !b)
        return false;

    if (a == b)
        return false;

    if (a->propertyHash() != b->propertyHash())
        return false;

    // We only care about objects created via a constructor's to_this. These
    // all have Structures with rare data and a sharedPolyProtoWatchpoint.
    if (!a->hasRareData() || !b->hasRareData())
        return false;

    // We only care about Structure's generated from functions that share
    // the same executable.
    // TSAN family structure-fields (§10.9 item (4)): identity-only check, so
    // compare the Box pointer WORDS through the relaxed-atomic raw accessor
    // instead of dereferencing the member through a const Box& (a plain load
    // racing the flag-on CAS installer). Box::get() points at a fixed offset
    // inside the RefCountable, so word equality <=> get() equality; identical
    // load/compare codegen flag-off, and no ref-count traffic.
    uintptr_t aInlineWatchpointSetWord = a->rareData()->sharedPolyProtoWatchpointWord();
    uintptr_t bInlineWatchpointSetWord = b->rareData()->sharedPolyProtoWatchpointWord();
    if (!aInlineWatchpointSetWord || !bInlineWatchpointSetWord || aInlineWatchpointSetWord != bInlineWatchpointSetWord)
        return false;
    ASSERT(aInlineWatchpointSetWord && bInlineWatchpointSetWord && aInlineWatchpointSetWord == bInlineWatchpointSetWord);

    if (a->hasPolyProto() || b->hasPolyProto())
        return false;

    if (a->storedPrototype() == b->storedPrototype())
        return false;

    JSObject* aObj = a->storedPrototypeObject();
    JSObject* bObj = b->storedPrototypeObject();
    while (aObj && bObj) {
        a = aObj->structure();
        b = bObj->structure();

        if (a->propertyHash() != b->propertyHash())
            return false;

        aObj = a->storedPrototypeObject(aObj);
        bObj = b->storedPrototypeObject(bObj);
    }

    return !aObj && !bObj;
}

inline Structure* Structure::nonPropertyTransition(VM& vm, Structure* structure, TransitionKind transitionKind, DeferredStructureTransitionWatchpointFire* deferred)
{
    if (changesIndexingType(transitionKind)) {
        if (JSGlobalObject* globalObject = structure->m_realm.get()) {
            if (globalObject->isOriginalArrayStructure(structure)) {
                IndexingType indexingModeIncludingHistory = newIndexingType(structure->indexingModeIncludingHistory(), transitionKind);
                Structure* result = globalObject->originalArrayStructureForIndexingType(indexingModeIncludingHistory);
                if (result->indexingModeIncludingHistory() == indexingModeIncludingHistory) {
                    structure->didTransitionFromThisStructure(deferred);
                    return result;
                }
            }
        }
    }

    return nonPropertyTransitionSlow(vm, structure, transitionKind, deferred);
}

ALWAYS_INLINE Structure* Structure::addPropertyTransitionToExistingStructureImpl(Structure* structure, UniquedStringImpl* uid, unsigned attributes, PropertyOffset& offset)
{
    ASSERT(!structure->isDictionary());
    ASSERT(structure->isObject());

    offset = invalidOffset;

    if (structure->hasBeenDictionary())
        return nullptr;

    if (Structure* existingTransition = structure->m_transitionTable.get(uid, attributes, TransitionKind::PropertyAddition)) {
        validateOffset(existingTransition->transitionOffset(), existingTransition->inlineCapacity());
        offset = existingTransition->transitionOffset();
        return existingTransition;
    }

    return nullptr;
}

ALWAYS_INLINE Structure* Structure::addPropertyTransitionToExistingStructure(Structure* structure, PropertyName propertyName, unsigned attributes, PropertyOffset& offset)
{
    ASSERT(!isCompilationThread());
    // SPEC-objectmodel L6(i)/I37 (Task 3c): flag-on, every MUTATOR
    // transition-table lookup holds the source's m_lock — route to the
    // m_lock-holding Concurrently variant (inserts already run under m_lock,
    // so locked lookups can never observe a half-published single-slot->map
    // inflation or a map rehash). Routing here (the shared inline body)
    // covers every mutator caller, including the ones outside Structure.cpp
    // (JSObject.cpp, JSObjectInlines.h, LiteralParser.cpp). Flag-off: today's
    // lock-free lookup, bit-identical behavior (I22).
    //
    // FIX-5 family-1 closure note: this branch is the SOLE intentional
    // flag-off delta on the transition-lookup fast path (one frozen-Config
    // load + one predicted-false branch; the Concurrently body is out-of-line
    // per the note below, so flag-off inlines exactly the pre-threads Impl).
    // AB17g RULING (supersedes the earlier re-key suggestion here): KEEP
    // this branch keyed on Options::useJSThreads() and record it as within
    // SPEC-jit I1's permitted delta — it already reads
    // g_jscConfig.options.useJSThreads on the frozen read-only Config page,
    // i.e. it IS the single one-byte-test form. Explicitly DO NOT re-key it
    // to g_jscConfig.gilOffProcess: GIL-ON useJSThreads mode (V6) has N
    // mutators, and StructureTransitionTable::add's single-slot->map
    // inflation allocates (can GC => GIL yield) between map construction
    // and the m_data publish. Keyed on gilOffProcess (==0 GIL-on) a lookup
    // would take the lock-free Impl and trySingleTransition would load the
    // half-published m_data; keyed on useJSThreads (==1) it routes to the
    // locked Concurrently variant and cannot observe the torn word.
    // Flag-off both keys read 0 and the pre-threads Impl is inlined
    // unchanged. Race statement: flag-on inserts publish
    // single-slot->TransitionMap inflation and map rehashes under the
    // source's m_lock (Structure.cpp StructureTransitionTable::add), so a
    // flag-on lock-free get could observe a half-published m_data/map —
    // hence the reroute; flag-off has one mutator, so the lock-free get
    // (trySingleTransition plain m_data load below) can never race an insert
    // and stays byte-identical to upstream.
    if (Options::useJSThreads()) [[unlikely]]
        return addPropertyTransitionToExistingStructureConcurrently(structure, propertyName.uid(), attributes, offset);
    return addPropertyTransitionToExistingStructureImpl(structure, propertyName.uid(), attributes, offset);
}

// addPropertyTransitionToExistingStructureConcurrently is out-of-line in
// Structure.cpp (like its remove/attributeChange siblings): flag-off, the
// m_lock machinery and the duplicated Impl body must not be inlined into
// every put site through the ALWAYS_INLINE dispatcher above — that is pure
// icache/register-pressure cost on the transition fast path. Flag-on, this
// path immediately takes m_lock, so one extra call is noise.

ALWAYS_INLINE StructureTransitionTable::Hash::Key StructureTransitionTable::Hash::createKeyFromStructure(Structure* structure)
{
    switch (structure->transitionKind()) {
    case TransitionKind::ChangePrototype:
        return StructureTransitionTable::Hash::createKey(structure->storedPrototype().isNull() ? nullptr : asObject(structure->storedPrototype()), structure->transitionPropertyAttributes(), structure->transitionKind());
    default:
        return StructureTransitionTable::Hash::createKey(structure->m_transitionPropertyName.get(), structure->transitionPropertyAttributes(), structure->transitionKind());
    }
}

inline Structure* StructureTransitionTable::trySingleTransition() const
{
    uintptr_t pointer = static_cast<uintptr_t>(dataConcurrently()); // THREADS/TSAN: relaxed snapshot, same mov as the upstream plain load.
    if (pointer & UsingSingleSlotFlag)
        return std::bit_cast<Structure*>(pointer & ~UsingSingleSlotFlag);
    return nullptr;
}

template<typename Functor>
void StructureTransitionTable::forEachTransition(const Functor& functor) const
{
    if (isUsingSingleSlot()) {
        if (Structure* transition = trySingleTransition())
            functor(transition);
        return;
    }
    map()->forEach([&](Structure* transition) {
        functor(transition);
        return IterationStatus::Continue;
    });
}

inline Structure* StructureTransitionTable::get(PointerKey rep, unsigned attributes, TransitionKind transitionKind) const
{
    // Single snapshot of the table word (one load — upstream's plain code
    // loaded it twice via isUsingSingleSlot + trySingleTransition).
    uintptr_t data = static_cast<uintptr_t>(dataConcurrently());
    if (data & UsingSingleSlotFlag) {
        auto* transition = std::bit_cast<Structure*>(data & ~UsingSingleSlotFlag);
        if (!transition)
            return nullptr;
        if (Hash::createKeyFromStructure(transition) != Hash::createKey(rep, attributes, transitionKind))
            return nullptr;
        return transition;
    }
    return std::bit_cast<TransitionMap*>(data)->get(StructureTransitionTable::Hash::createKey(rep, attributes, transitionKind));
}

// SPEC-objectmodel L6/I37 (Task 3c): see the declaration. Mirrors add()'s
// keying exactly (createKeyFromStructure on the candidate), so a hit is
// precisely the Structure a subsequent add(candidate) would have clobbered.
// Caller holds the owning Structure's m_lock flag-on.
inline Structure* StructureTransitionTable::getMatching(Structure* candidate) const
{
    uintptr_t data = static_cast<uintptr_t>(dataConcurrently()); // Single snapshot (see get()).
    if (data & UsingSingleSlotFlag) {
        auto* transition = std::bit_cast<Structure*>(data & ~UsingSingleSlotFlag);
        if (!transition)
            return nullptr;
        if (Hash::createKeyFromStructure(transition) != Hash::createKeyFromStructure(candidate))
            return nullptr;
        return transition;
    }
    return std::bit_cast<TransitionMap*>(data)->get(Hash::createKeyFromStructure(candidate));
}

inline void StructureTransitionTable::finalizeUnconditionally(VM& vm, CollectionScope)
{
    if (auto* transition = trySingleTransition()) {
        if (!vm.heap.isMarked(transition))
            m_data = UsingSingleSlotFlag;
    }
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
