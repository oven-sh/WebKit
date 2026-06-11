/*
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003-2020 Apple Inc. All rights reserved.
 *  Copyright (C) 2007 Eric Seidel (eric@webkit.org)
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "AuxiliaryBarrierInlines.h"
#include "BrandedStructure.h"
#include "ButterflyInlines.h"
#include "DeferGC.h"
#include "Error.h"
#include "JSArrayInlines.h"
#include "JSFunctionInlines.h"
#include "JSThreadsSafepoint.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSGlobalProxy.h"
#include "JSObject.h"
#include "JSTypedArrays.h"
#include "Lookup.h"
#include "MegamorphicCache.h"
#include "ObjectInitializationScope.h"
#include "SparseArrayValueMap.h"
#include "StructureInlines.h"
#include "TypedArrayType.h"
#include "VM.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {


inline JSCell* getJSFunction(JSValue value)
{
    if (value.isCell() && (value.asCell()->type() == JSFunctionType))
        return value.asCell();
    return nullptr;
}

inline Structure* JSObject::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

inline Structure* JSNonFinalObject::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

inline Structure* JSFinalObject::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, unsigned inlineCapacity)
{
    return Structure::create(vm, globalObject, prototype, typeInfo(), info(), defaultIndexingType, inlineCapacity);
}

#if USE(JSVALUE64)
// SPEC-objectmodel Task 2: flag-on, every butterfly install/replacement stamps
// the installing thread's TID into the high bits of the butterfly word (§2).
// The SW bit is monotonic (I4) and preserved verbatim on replacement. The
// 64-bit store/CAS is legal on PreciseAllocation cells too (the word at
// cell+8 is 8-byte-aligned; I36 only forbids the 128-bit DCAS there).
inline void JSObject::storeTaggedButterflyWordConcurrent(VM& vm, Butterfly* butterfly)
{
    ASSERT(Options::useJSThreads());
    ASSERT(type() != WebAssemblyGCObjectType);
    Atomic<uint64_t>* word = std::bit_cast<Atomic<uint64_t>*>(butterflyAddress());
    uint64_t old = word->load(std::memory_order_relaxed);
    // Foreign or SW=1 flat transitions never come through this path (they take
    // the segmented protocols, §3/§4.2/§4.3); element resizes of objects with
    // indexed properties go through casButterfly (I17) since Task 8
    // (ensureLengthSlow/reallocateAndShrinkButterfly/GT10 sites) - the
    // remaining users are E4 owner transitions and pre-escape installs.
    //
    // Review-round-1 hardening: the "owner-only" claim above is now a runtime
    // witness, not just the manifest-7 caller audit - a missed caller becomes
    // a trap, not a silent ownership steal.
    //
    // Review round 3 (§4.3(b2)/I21): the round-1 CAS loop FOLDED a racing
    // foreign F1 SW flip into a retry of the SAME desired word. That is the
    // forbidden taxonomy-(b2) merge whenever the desired payload REPLACES the
    // current one (a freshly copied flat butterfly): the flipper's follow-up
    // plain store lands in the OLD payload after the copy was taken, and
    // re-publishing the copy silently drops it - preserving the SW BIT does
    // not preserve the racing STORE. So a CAS failure is tolerated (SW folded,
    // retry) ONLY when the desired payload IS the observed payload (a pure
    // re-tag - the b1 shape); any payload-replacing divergence traps. Callers
    // must prove the word cannot move across their publication window: E4
    // sites hold both source TTL sets valid across a poll-free window (an F1
    // flip needs a fire, which needs a stop, which needs this thread parked at
    // a safepoint), and the §6 locked-add sites are gated by
    // JSObject::classifyConcurrentLockedAdd, which re-routes every regime
    // whose word a lock-free actor could legally move (foreign tag, SW=1,
    // fired writeThreadLocal with growth pending, segmented) BEFORE reaching
    // this helper.
    while (true) {
        // The word we are replacing must be empty or owner-tagged flat; a
        // foreign-tagged or segmented word here means a caller escaped the
        // protocol audit (§3/§4.2/§4.3 own those words).
        RELEASE_ASSERT(!old || (!isSegmentedButterfly(old) && butterflyTID(old) == currentButterflyTID()));
        uint64_t desired = butterfly ? encodeButterfly(butterfly, currentButterflyTID(), butterflySharedWrite(old)) : 0;
        uint64_t observed = word->compareExchangeStrong(old, desired, std::memory_order_seq_cst);
        if (observed == old)
            break;
        // b1-only fold: same payload, SW bit appeared (I4 monotone). Anything
        // else is a protocol escape by the caller - trap loudly rather than
        // silently lose a racing store (I21).
        RELEASE_ASSERT(butterfly
            && untaggedButterfly(observed) == untaggedButterfly(old)
            && untaggedButterfly(old) == butterfly);
        old = observed;
    }
    vm.writeBarrier(this);
}

inline void JSObject::setButterflyConcurrent(VM& vm, Butterfly* butterfly)
{
    ASSERT(Options::useJSThreads());
    // M8: flag-on, the fenced publication order is the only branch (manifest
    // entry 4b forces heap.m_mutatorShouldBeFenced; we fence unconditionally
    // here so the protocol holds even before that entry is applied).
    WTF::storeStoreFence();
    Atomic<uint64_t>* word = std::bit_cast<Atomic<uint64_t>*>(butterflyAddress());
    uint64_t old = word->load(std::memory_order_relaxed);
    if (!old && butterfly) {
        // §2.1 N3 first install: CAS the all-zero word to (b, currentTID, SW=0).
        uint64_t desired = encodeButterfly(butterfly, currentButterflyTID(), false);
        uint64_t previous = word->compareExchangeStrong(0, desired);
        // N3 failure = a racing first install won; the loser must re-dispatch on
        // the winner's tag. Every present caller installs into an object it
        // allocated or owns under E4-eligible conditions, so the CAS cannot
        // lose until the multi-mutator paths (Tasks 5-8) route racy installs
        // through casButterfly() (§9.3) with caller re-dispatch.
        RELEASE_ASSERT(!previous);
        vm.writeBarrier(this);
    } else
        storeTaggedButterflyWordConcurrent(vm, butterfly);
    WTF::storeStoreFence();
}

inline void JSObject::nukeStructureAndSetButterflyConcurrent(VM& vm, StructureID oldStructureID, Butterfly* butterfly)
{
    ASSERT(Options::useJSThreads());
    // M5: E4 transitions keep today's fenced nuke order — value (caller), nuke,
    // fence, tagged butterfly, fence, new StructureID (caller). This is also the
    // I36 publication order required on PreciseAllocation cells (which must
    // never use the 128-bit DCAS); their cell-locking discipline is wired by
    // Tasks 3/6.
    setStructureIDDirectly(oldStructureID.nuke());
    WTF::storeStoreFence();
    storeTaggedButterflyWordConcurrent(vm, butterfly);
    WTF::storeStoreFence();
}

// SPEC-objectmodel §6 (review round 3) - see the declaration comment in
// JSObject.h. Run UNDER the cell lock, after the structureID re-validation;
// the cell lock plus the caller's DeferGC make a Proceed classification stable
// through the table edit: no §10.6 stop can land in the poll-free locked
// window, so a still-valid writeThreadLocal set cannot fire there and an F1
// SW flip (which requires the fire first) cannot complete (I12/I13).
inline bool JSObject::classifyConcurrentLockedAdd(Structure* structure, ConcurrentLockedAddSlowAction& action)
{
    ASSERT(Options::useJSThreads());
    action = ConcurrentLockedAddSlowAction::None;
    uint64_t word = taggedButterflyWord();
    // Conservative growth bound for ONE add: the fresh-offset case assigns at
    // most maxOffset + 1 (deleted-offset reuse never grows capacity).
    size_t currentCapacity = structure->outOfLineCapacity();
    size_t capacityAfterFreshOffset = Structure::outOfLineCapacity(structure->maxOffset() + 1);
    bool growthPossible = capacityAfterFreshOffset != currentCapacity;

    if (!(word & butterflyPointerMask)) {
        // None: growth installs FRESH storage (no copy => no lost-write
        // hazard). The caller pre-nukes the structureID lane (CAS) before the
        // table edit so a racing lock-free N3 indexed first-install
        // (createInitialIndexedStorageConcurrent's nuke-CAS) loses and
        // re-dispatches instead of colliding with our publication.
        return true;
    }

    if (isSegmentedButterfly(word)) {
        if (growthPossible
            // TSAN r12 (reports 15/16): outOfLineFragmentCountConcurrent —
            // a foreign grower publishes the count with
            // butterflyConcurrentStore; the plain field read raced it.
            && static_cast<uint64_t>(butterflyFragmentSlots) * butterflySpine(word)->outOfLineFragmentCountConcurrent() < capacityAfterFreshOffset) {
            action = ConcurrentLockedAddSlowAction::GrowSegmentedOutOfLine;
            return false;
        }
        return true; // Coverage sufficient (monotone across replacement spines): the lambda only bumps maxOffset.
    }

    if (isCopyOnWrite(structure->indexingMode())) {
        action = ConcurrentLockedAddSlowAction::MaterializeCopyOnWrite; // §4.8 materialize-first (owner and foreign serialize on the cell-locked materializer).
        return false;
    }

    if (hasAnyArrayStorage(structure->indexingType())) {
        // I31: every AS access AND transition is cell-locked flag-on (E4
        // excludes AS shapes), so the under-lock copy-grow cannot race
        // lock-free stores; the growth publication preserves the tag verbatim
        // (AS-COPY form). A foreign first WRITE still runs the §4.6 per-event
        // SW stop first (I12).
        if (!butterflySharedWrite(word) && butterflyWriterIsForeign(word)) {
            action = ConcurrentLockedAddSlowAction::FireSharedWriteBit;
            return false;
        }
        return true;
    }

    if (!butterflySharedWrite(word) && butterflyWriterIsForeign(word)) {
        action = ConcurrentLockedAddSlowAction::FireSharedWriteBit; // F1 (I12) before any foreign value store into the butterfly.
        return false;
    }

    if (growthPossible && (butterflySharedWrite(word) || !structure->writeThreadLocalIsStillValid())) {
        // I27/§4.3(b2): a flat payload may be copy-grown under the cell lock
        // only while it is (currentTID, 0) AND writeThreadLocal(S) is still
        // valid - otherwise a lock-free F1 flip (no stop needed once the set
        // is fired) can land between the copy and the publication and its
        // follow-up plain store would be dropped (I21). Convert to segmented
        // instead: segmented growth appends fragments without relocating any
        // shared slot.
        action = ConcurrentLockedAddSlowAction::ConvertToSegmented;
        return false;
    }

    // Owner (currentTID, 0): growth (if any) is the safe copy window; SW=1
    // without growth is a plain §3 "owner or SW=1" store into an existing slot.
    return true;
}

inline void JSObject::performConcurrentLockedAddSlowAction(VM& vm, ConcurrentLockedAddSlowAction action)
{
    ASSERT(Options::useJSThreads());
    auto* object = static_cast<JSObjectWithButterfly*>(this);
    switch (action) {
    case ConcurrentLockedAddSlowAction::None:
        return; // Plain RESTART (a racing locked transition/flatten settled first).
    case ConcurrentLockedAddSlowAction::FireSharedWriteBit:
        ensureSharedWriteBit(vm, object);
        return;
    case ConcurrentLockedAddSlowAction::ConvertToSegmented:
        // nullptr-RESTART and success both re-dispatch at the caller (§4.2).
        convertToSegmentedButterfly(vm, object, nullptr, nullptr, invalidOffset, JSValue());
        return;
    case ConcurrentLockedAddSlowAction::GrowSegmentedOutOfLine:
        ensureSegmentedOutOfLineCapacity(vm, object, Structure::outOfLineCapacity(structure()->maxOffset() + 1));
        return;
    case ConcurrentLockedAddSlowAction::MaterializeCopyOnWrite:
        materializeCopyOnWriteButterflyConcurrent(vm, object);
        return;
    }
}

// See the declaration comment in JSObject.h. Caller holds the cell lock and a
// DeferGC, and classifyConcurrentLockedAdd returned Proceed under that same
// lock.
inline void JSObject::growOutOfLineStorageForConcurrentLockedAdd(VM& vm, StructureID structureID, Structure* structure, PropertyOffset newMaxOffset, unsigned oldOutOfLineCapacity, unsigned newOutOfLineCapacity)
{
    ASSERT(Options::useJSThreads());
    uint64_t lockedWord = taggedButterflyWord();
    if (isSegmentedButterfly(lockedWord)) {
        // Pre-grown by the GrowSegmentedOutOfLine slow action; out-of-line
        // fragment coverage is MONOTONE across replacement spines (every
        // §4.3-1/T2 replacement copies the fragment pointer prefix verbatim),
        // so the bound holds even if a racing §4.4 element resize republished
        // a newer spine since the classification. The butterfly word is left
        // alone (no copy, no nuke): the §4.5 segmented visit bounds itself by
        // the SPINE's coverage and didRaces on outOfLineSize overruns.
        // Concurrent accessor (r18 report, post-closeout review): a racing
        // sibling's ensureSegmentedOutOfLineCapacity publishes the count
        // with butterflyConcurrentStore; the plain read here was the lone
        // unpaired reader (the monotonicity argument above is what makes
        // the racy VALUE safe — the access itself must still be atomic).
        RELEASE_ASSERT(static_cast<uint64_t>(butterflyFragmentSlots) * butterflySpine(lockedWord)->outOfLineFragmentCountConcurrent() >= newOutOfLineCapacity);
        structure->setMaxOffset(vm, newMaxOffset);
        return;
    }
    if ((lockedWord & butterflyPointerMask)
        && (butterflySharedWrite(lockedWord) || butterflyWriterIsForeign(lockedWord))) {
        // Only ArrayStorage regimes reach the growth leg with a shared or
        // foreign word (classifyConcurrentLockedAdd re-routes every other
        // one). Every AS access/transition is cell-locked flag-on (I31 + the
        // E4 AS-shape exclusion), so the copy cannot race lock-free stores;
        // the publication preserves the tag VERBATIM (§4.6 AS-COPY form,
        // T3/I17) - never re-stamp a foreign installer's tag with ours.
        RELEASE_ASSERT(hasAnyArrayStorage(structure->indexingType()));
        Butterfly* newButterfly = allocateMoreOutOfLineStorage(vm, oldOutOfLineCapacity, newOutOfLineCapacity);
        setStructureIDDirectly(structureID.nuke());
        WTF::storeStoreFence();
        bool published = casButterfly(static_cast<JSObjectWithButterfly*>(this), lockedWord,
            encodeButterfly(newButterfly, butterflyTID(lockedWord), butterflySharedWrite(lockedWord)));
        RELEASE_ASSERT(published); // No lock-free actor may target an AS word (I31).
        structure->setMaxOffset(vm, newMaxOffset);
        WTF::storeStoreFence();
        setStructureIDDirectly(structureID);
        return;
    }
    // None (the caller pre-nuked the ID lane, so racing lock-free N3 installs
    // re-dispatch) or owner-(currentTID, 0) flat with writeThreadLocal
    // verified still valid under this lock: today's nuke-bracketed copy.
    // storeTaggedButterflyWordConcurrent's b1-only CAS independently witnesses
    // that the word never moved across the window.
    Butterfly* newButterfly = allocateMoreOutOfLineStorage(vm, oldOutOfLineCapacity, newOutOfLineCapacity);
    // bench-transition-regress (V5B-1 extension): this function is flag-on-only
    // (ASSERT(Options::useJSThreads()) at entry), so call the concurrent publish
    // directly instead of re-loading the flag through the runtime-dispatch
    // wrapper. Identical dispatch result to nukeStructureAndSetButterfly here.
    nukeStructureAndSetButterflyConcurrent(vm, structureID, newButterfly);
    structure->setMaxOffset(vm, newMaxOffset);
    WTF::storeStoreFence();
    setStructureIDDirectly(structureID);
}
#endif // USE(JSVALUE64)

inline void JSObject::setButterfly(VM& vm, Butterfly* butterfly)
{
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        setButterflyConcurrent(vm, butterfly);
        return;
    }
#endif
    if (isX86() || vm.heap.mutatorShouldBeFenced()) {
        WTF::storeStoreFence();
        butterflyRef().set(vm, this, butterfly);
        WTF::storeStoreFence();
        return;
    }

    butterflyRef().set(vm, this, butterfly);
}

inline void JSObject::nukeStructureAndSetButterfly(VM& vm, StructureID oldStructureID, Butterfly* butterfly)
{
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        nukeStructureAndSetButterflyConcurrent(vm, oldStructureID, butterfly);
        return;
    }
#endif
    if (isX86() || vm.heap.mutatorShouldBeFenced()) {
        setStructureIDDirectly(oldStructureID.nuke());
        WTF::storeStoreFence();
        butterflyRef().set(vm, this, butterfly);
        WTF::storeStoreFence();
        return;
    }

    butterflyRef().set(vm, this, butterfly);
}

inline JSValue JSObject::get(JSGlobalObject* globalObject, PropertyName propertyName) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::Get);
    bool hasProperty = const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);

    EXCEPTION_ASSERT(!scope.exception() || vm.hasPendingTerminationException() || !hasProperty);
    RETURN_IF_EXCEPTION(scope, jsUndefined());

    if (hasProperty)
        RELEASE_AND_RETURN(scope, slot.getValue(globalObject, propertyName));

    return jsUndefined();
}

inline JSValue JSObject::get(JSGlobalObject* globalObject, unsigned propertyName) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::Get);
    bool hasProperty = const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);

    EXCEPTION_ASSERT(!scope.exception() || vm.hasPendingTerminationException() || !hasProperty);
    RETURN_IF_EXCEPTION(scope, jsUndefined());

    if (hasProperty)
        RELEASE_AND_RETURN(scope, slot.getValue(globalObject, propertyName));

    return jsUndefined();
}

template<typename T, typename PropertyNameType>
inline T JSObject::getAs(JSGlobalObject* globalObject, PropertyNameType propertyName) const
{
    JSValue value = get(globalObject, propertyName);
#if ASSERT_ENABLED || ENABLE(SECURITY_ASSERTIONS)
    VM& vm = getVM(globalObject);
    if (vm.exceptionForInspection())
        return nullptr;
#endif
    return uncheckedDowncast<std::remove_pointer_t<T>>(value);
}

template<typename CellType, SubspaceAccess>
CompleteSubspace* JSFinalObject::subspaceFor(VM& vm)
{
    static_assert(CellType::needsDestruction == DoesNotNeedDestruction);
    return &vm.cellSpace();
}

// https://tc39.es/ecma262/#sec-createlistfromarraylike
template <typename Functor> // A functor should have a type like: (JSValue) -> bool
void forEachInArrayLike(JSGlobalObject* globalObject, JSObject* arrayLikeObject, Functor functor)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint64_t length = toLength(globalObject, arrayLikeObject);
    RETURN_IF_EXCEPTION(scope, void());
    for (uint64_t index = 0; index < length; index++) {
        JSValue value = arrayLikeObject->getIndex(globalObject, index);
        RETURN_IF_EXCEPTION(scope, void());
        if (!functor(value))
            return;
    }
}

ALWAYS_INLINE bool JSObject::canPerformFastPutInlineExcludingProto()
{
    // Check if there are any setters or getters in the prototype chain
    JSValue prototype;
    JSObject* obj = this;
    while (true) {
        Structure* structure = obj->structure();
        if (structure->hasReadOnlyOrGetterSetterPropertiesExcludingProto() || structure->typeInfo().overridesGetPrototype())
            return false;
        if (obj != this && structure->typeInfo().overridesPut())
            return false;

        prototype = obj->getPrototypeDirect();
        if (prototype.isNull())
            return true;

        obj = asObject(prototype);
    }

    ASSERT_NOT_REACHED();
}

ALWAYS_INLINE bool JSObject::canPerformFastPutInline(VM& vm, PropertyName propertyName)
{
    if (propertyName == vm.propertyNames->underscoreProto) [[unlikely]]
        return false;
    return canPerformFastPutInlineExcludingProto();
}

template<typename CallbackWhenNoException>
ALWAYS_INLINE typename std::invoke_result<CallbackWhenNoException, bool, PropertySlot&>::type JSObject::getPropertySlot(JSGlobalObject* globalObject, PropertyName propertyName, CallbackWhenNoException callback) const
{
    PropertySlot slot(this, PropertySlot::InternalMethodType::Get);
    return getPropertySlot(globalObject, propertyName, slot, callback);
}

template<typename CallbackWhenNoException>
ALWAYS_INLINE typename std::invoke_result<CallbackWhenNoException, bool, PropertySlot&>::type JSObject::getPropertySlot(JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot, CallbackWhenNoException callback) const
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool found = const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callback(found, slot));
}

ALWAYS_INLINE bool JSObject::getPropertySlot(JSGlobalObject* globalObject, unsigned propertyName, PropertySlot& slot)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = this;
    while (true) {
        Structure* structure = object->structureID().decode();
        bool hasSlot = structure->classInfoForCells()->methodTable.getOwnPropertySlotByIndex(object, globalObject, propertyName, slot);
        RETURN_IF_EXCEPTION(scope, false);
        if (hasSlot)
            return true;
        if (slot.isVMInquiry() && slot.isTaintedByOpaqueObject()) [[unlikely]]
            return false;
        if (object->type() == ProxyObjectType && slot.internalMethodType() == PropertySlot::InternalMethodType::HasProperty)
            return false;
        if (isTypedArrayType(object->type()) && propertyName >= uncheckedDowncast<JSArrayBufferView>(object)->length())
            return false;
        JSValue prototype;
        if (!structure->typeInfo().overridesGetPrototype() || slot.internalMethodType() == PropertySlot::InternalMethodType::VMInquiry) [[likely]]
            prototype = object->getPrototypeDirect();
        else {
            prototype = object->getPrototype(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (!prototype.isObject())
            return false;
        object = asObject(prototype);
    }
}

ALWAYS_INLINE bool JSObject::getPropertySlot(JSGlobalObject* globalObject, uint64_t propertyName, PropertySlot& slot)
{
    if (propertyName <= MAX_ARRAY_INDEX) [[likely]]
        return getPropertySlot(globalObject, static_cast<uint32_t>(propertyName), slot);
    return getPropertySlot(globalObject, Identifier::from(globalObject->vm(), propertyName), slot);
}

ALWAYS_INLINE bool JSObject::getNonIndexPropertySlot(JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    // This method only supports non-index PropertyNames.
    ASSERT(!parseIndex(propertyName));

    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* object = this;
    while (true) {
        Structure* structure = object->structureID().decode();
        if (Options::useJSThreads() && structure->isUncacheableDictionary() && !slot.isVMInquiry() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
            return false;
        if (!TypeInfo::overridesGetOwnPropertySlot(object->inlineTypeFlags())) [[likely]] {
            if (object->getOwnNonIndexPropertySlot(vm, structure, propertyName, slot))
                return true;
        } else {
            bool hasSlot = structure->classInfoForCells()->methodTable.getOwnPropertySlot(object, globalObject, propertyName, slot);
            RETURN_IF_EXCEPTION(scope, false);
            if (hasSlot)
                return true;
            if (slot.isVMInquiry() && slot.isTaintedByOpaqueObject()) [[unlikely]]
                return false;
            if (object->type() == ProxyObjectType && slot.internalMethodType() == PropertySlot::InternalMethodType::HasProperty)
                return false;
            if (isTypedArrayType(object->type()) && isCanonicalNumericIndexString(propertyName.uid()))
                return false;
        }
        JSValue prototype;
        if (!structure->typeInfo().overridesGetPrototype() || slot.internalMethodType() == PropertySlot::InternalMethodType::VMInquiry) [[likely]]
            prototype = object->getPrototypeDirect();
        else {
            prototype = object->getPrototype(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (!prototype.isObject())
            return false;
        object = asObject(prototype);
    }
}

inline bool JSObject::getOwnPropertySlotInline(JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    if (TypeInfo::overridesGetOwnPropertySlot(inlineTypeFlags())) [[unlikely]]
        return methodTable()->getOwnPropertySlot(this, globalObject, propertyName, slot);
    return JSObject::getOwnPropertySlot(this, globalObject, propertyName, slot);
}

template<typename PropertyNameType> inline JSValue JSObject::getIfPropertyExists(JSGlobalObject* globalObject, const PropertyNameType& propertyName)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PropertySlot slot(this, PropertySlot::InternalMethodType::HasProperty);
    bool hasProperty = getPropertySlot(globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, { });
    if (!hasProperty)
        return { };

    scope.release();
    if (slot.isTaintedByOpaqueObject()) [[unlikely]]
        return get(globalObject, propertyName);

    return slot.getValue(globalObject, propertyName);
}

// FIXME: Given the single special purpose this is used for, it's unclear if this needs to be a JSObject member function.
inline bool JSObject::noSideEffectMayHaveNonIndexProperty(VM& vm, PropertyName propertyName)
{
    // This function only supports non-index PropertyNames.
    ASSERT(!parseIndex(propertyName));
    ASSERT(propertyName != vm.propertyNames->length);
    for (auto* object = this; object; object = object->getPrototypeDirect().getObject()) {
        auto inlineTypeFlags = object->inlineTypeFlags();
        if (TypeInfo::overridesGetOwnPropertySlot(inlineTypeFlags) && object->classInfo() != ArrayPrototype::info()) [[unlikely]]
            return true;
        auto& structure = *object->structureID().decode();
        unsigned attributes;
        if (isValidOffset(structure.get(vm, propertyName, attributes))) [[unlikely]]
            return true;
        if (hasNonReifiedStaticProperties()) {
            for (auto* ancestorClass = object->classInfo(); ancestorClass; ancestorClass = ancestorClass->parentClass) {
                if (auto* table = ancestorClass->staticPropHashTable; table && table->entry(propertyName)) [[unlikely]]
                    return true;
            }
        }
        if (structure.typeInfo().overridesGetPrototype()) [[unlikely]]
            return true;
    }
    return false;
}

inline bool JSObject::mayInterceptIndexedAccesses()
{
    return structure()->mayInterceptIndexedAccesses();
}

inline void JSObject::putDirectWithoutTransition(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes)
{
    ASSERT(!value.isGetterSetter() && !(attributes & PropertyAttribute::Accessor));
    ASSERT(!value.isCustomGetterSetter());
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        // Review round 1: route through the cell-locked form (value stored in
        // the same critical section as the table edit - I9/L3/L4).
        putDirectWithoutTransitionConcurrent(vm, propertyName, value, attributes);
        if (attributes & PropertyAttribute::ReadOnly)
            structure()->setContainsReadOnlyProperties();
        return;
    }
#endif
    StructureID structureID = this->structureID();
    Structure* structure = structureID.decode();
    PropertyOffset offset = prepareToPutDirectWithoutTransition(vm, propertyName, attributes, structureID, structure);
    putDirectOffset(vm, offset, value);
    if (attributes & PropertyAttribute::ReadOnly)
        structure->setContainsReadOnlyProperties();
}

ALWAYS_INLINE PropertyOffset JSObject::prepareToPutDirectWithoutTransition(VM& vm, PropertyName propertyName, unsigned attributes, StructureID structureID, Structure* structure)
{
    // Flag-on, "without transition" adds go through
    // putDirectWithoutTransitionConcurrent (cell-locked, value stored inside
    // the same window). This unlocked form is the flag-off path only (I22).
    ASSERT(!Options::useJSThreads());
    unsigned oldOutOfLineCapacity = structure->outOfLineCapacity();
    PropertyOffset result;
    structure->addPropertyWithoutTransition(
        vm, propertyName, attributes,
        [&] (const GCSafeConcurrentJSLocker&, PropertyOffset offset, PropertyOffset newMaxOffset) {
            unsigned newOutOfLineCapacity = Structure::outOfLineCapacity(newMaxOffset);
            if (newOutOfLineCapacity != oldOutOfLineCapacity) {
                Butterfly* butterfly = allocateMoreOutOfLineStorage(vm, oldOutOfLineCapacity, newOutOfLineCapacity);
                // bench-transition-regress (V5B-1 extension): this function is
                // flag-off-only (I22; ASSERT(!Options::useJSThreads()) at entry,
                // all callers reroute to putDirectWithoutTransitionConcurrent
                // flag-on), so inline the pre-threads publish body - no
                // Options::useJSThreads() load on the transition fast path.
                if (isX86() || vm.heap.mutatorShouldBeFenced()) {
                    setStructureIDDirectly(structureID.nuke());
                    WTF::storeStoreFence();
                    butterflyRef().set(vm, this, butterfly);
                    WTF::storeStoreFence();
                } else
                    butterflyRef().set(vm, this, butterfly);
                structure->setMaxOffset(vm, newMaxOffset);
                WTF::storeStoreFence();
                setStructureIDDirectly(structureID);
            } else
                structure->setMaxOffset(vm, newMaxOffset);

            // This assertion verifies that the concurrent GC won't read garbage if the concurrentGC
            // is running at the same time we put without transitioning.
            ASSERT(!getDirect(offset) || !JSValue::encode(getDirect(offset)));
            result = offset;
        });
    if (mayBePrototype()) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
    return result;
}

#if USE(JSVALUE64)
// SPEC-objectmodel §6 L3/L4 + I9 (review round 1): flag-on, every "without
// transition" add - the pinned-table/dictionary form where the structure and
// the object mutate in tandem - runs under the cell lock, OUTER to the m_lock
// the table edit takes inside addPropertyWithoutTransition (I20 order:
// JSCellLock < Structure::m_lock; matches deletePropertyNamedConcurrent and
// flattenDictionaryStructureImpl). The VALUE is release-visible before the
// critical section ends, so no cell-locked dictionary reader (L3) can observe
// the table entry with a hole (I9). DeferGC discharges O1: the butterfly
// reallocation inside the lambda allocates under the cell lock (the
// sanctioned pre-lock DeferGC form), and the lock spans no poll/park.
inline NEVER_INLINE PropertyOffset JSObject::putDirectWithoutTransitionConcurrent(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes)
{
    ASSERT(Options::useJSThreads());
    DeferGC deferGC(vm);
    PropertyOffset result = invalidOffset;
    bool isPrototype = false;
    // Review round 3: §2 re-dispatch loop. The under-lock regime
    // classification re-routes every word the locked growth/store may not
    // touch (segmented coverage, foreign F1, SW=1/fired-set copy hazards, CoW)
    // through the matching protocol OUTSIDE the lock, then retries.
    while (true) {
        ConcurrentLockedAddSlowAction slowAction = ConcurrentLockedAddSlowAction::None;
        bool restart = false;
        {
            Locker locker { cellLock() };
            StructureID structureID = this->structureID(); // Snapshot UNDER the lock.
            if (structureID.isNuked())
                restart = true; // M5: a racing lock-free publication is mid-flight; spin outside the lock.
            else {
                Structure* structure = structureID.decode();
                if (!classifyConcurrentLockedAdd(structure, slowAction))
                    restart = true;
                else {
                    // None + possible growth: own the structureID lane FIRST
                    // (CAS old -> nuked) so a racing lock-free N3 indexed
                    // first-install (its protocol nuke-CASes the ID before its
                    // word CAS) loses and re-dispatches instead of colliding
                    // with our word publication. Readers spin on the nuked ID
                    // for the (bounded, poll-free, DeferGC'd) table edit.
                    auto* idAtomic = std::bit_cast<Atomic<uint32_t>*>(reinterpret_cast<char*>(this) + JSCell::structureIDOffset());
                    bool preNuked = false;
                    if (!taggedButterflyWord()
                        && Structure::outOfLineCapacity(structure->maxOffset() + 1) != structure->outOfLineCapacity()) {
                        if (idAtomic->compareExchangeStrong(structureID.bits(), structureID.nuke().bits()) != structureID.bits())
                            restart = true;
                        else
                            preNuked = true;
                    }
                    if (!restart) {
                        unsigned oldOutOfLineCapacity = structure->outOfLineCapacity();
                        structure->addPropertyWithoutTransition(
                            vm, propertyName, attributes,
                            [&] (const GCSafeConcurrentJSLocker&, PropertyOffset offset, PropertyOffset newMaxOffset) {
                                unsigned newOutOfLineCapacity = Structure::outOfLineCapacity(newMaxOffset);
                                if (newOutOfLineCapacity != oldOutOfLineCapacity)
                                    growOutOfLineStorageForConcurrentLockedAdd(vm, structureID, structure, newMaxOffset, oldOutOfLineCapacity, newOutOfLineCapacity);
                                else
                                    structure->setMaxOffset(vm, newMaxOffset);
                                ASSERT(!getDirect(offset) || !JSValue::encode(getDirect(offset)));
                                // I9: value stored INSIDE the locked window,
                                // with the table edit - no reader-observable
                                // hole.
                                putDirectOffset(vm, offset, value);
                                result = offset;
                            });
                        isPrototype = mayBePrototype();
                    }
                    if (preNuked && this->structureID().isNuked()) {
                        // Inline offset / no growth ran: restore the lane we
                        // pre-nuked (growth restores it itself).
                        WTF::storeStoreFence();
                        setStructureIDDirectly(structureID);
                    }
                }
            }
        }
        if (!restart)
            break;
        performConcurrentLockedAddSlowAction(vm, slowAction);
    }
    // Watchpoint-bearing invalidation OUTSIDE the cell lock (can take rank-6b
    // CodeBlock/jit locks; never acquire those holding a §6-ranked lock - O2).
    if (isPrototype) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
    return result;
}
#endif // USE(JSVALUE64)

// https://tc39.es/ecma262/#sec-ordinaryset
ALWAYS_INLINE bool JSObject::putInlineForJSObject(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = getVM(globalObject);

    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    ASSERT(value);
    ASSERT(!Heap::heap(value) || Heap::heap(value) == Heap::heap(thisObject));

    if (Options::useJSThreads() && thisObject->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, thisObject)) [[unlikely]]
        return false;

    // Try indexed put first. This is required for correctness, since loads on property names that appear like
    // valid indices will never look in the named property storage.
    if (std::optional<uint32_t> index = parseIndex(propertyName)) {
        if (isThisValueAltered(slot, thisObject)) [[unlikely]]
            return ordinarySetSlow(globalObject, thisObject, propertyName, value, slot.thisValue(), slot.isStrictMode());
        return thisObject->methodTable()->putByIndex(thisObject, globalObject, index.value(), value, slot.isStrictMode());
    }

    if (!thisObject->canPerformFastPutInline(vm, propertyName))
        return thisObject->putInlineSlow(globalObject, propertyName, value, slot);
    if (isThisValueAltered(slot, thisObject)) [[unlikely]]
        return definePropertyOnReceiver(globalObject, propertyName, value, slot);
    if (thisObject->hasNonReifiedStaticProperties()) [[unlikely]]
        return thisObject->putInlineFastReplacingStaticPropertyIfNeeded(globalObject, propertyName, value, slot);
    return thisObject->putInlineFast(globalObject, propertyName, value, slot);
}

ALWAYS_INLINE bool JSObject::putInlineFast(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto error = putDirectInternal<PutModePut>(vm, propertyName, value, 0, slot);
    if (!error.isNull())
        return typeError(globalObject, scope, slot.isStrictMode(), error);
    return true;
}

// https://tc39.es/ecma262/#sec-createdataproperty
ALWAYS_INLINE bool JSObject::createDataProperty(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, bool shouldThrow)
{
    PropertyDescriptor descriptor(value, static_cast<unsigned>(PropertyAttribute::None));
    return methodTable()->defineOwnProperty(this, globalObject, propertyName, descriptor, shouldThrow);
}

// HasOwnProperty(O, P) from section 7.3.11 in the spec.
// http://www.ecma-international.org/ecma-262/6.0/index.html#sec-hasownproperty
ALWAYS_INLINE bool JSObject::hasOwnProperty(JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot) const
{
    ASSERT(slot.internalMethodType() == PropertySlot::InternalMethodType::GetOwnProperty);
    if (const_cast<JSObject*>(this)->methodTable()->getOwnPropertySlot == JSObject::getOwnPropertySlot) [[likely]]
        return JSObject::getOwnPropertySlot(const_cast<JSObject*>(this), globalObject, propertyName, slot);
    return const_cast<JSObject*>(this)->methodTable()->getOwnPropertySlot(const_cast<JSObject*>(this), globalObject, propertyName, slot);
}

ALWAYS_INLINE bool JSObject::hasOwnProperty(JSGlobalObject* globalObject, PropertyName propertyName) const
{
    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);
    return hasOwnProperty(globalObject, propertyName, slot);
}

ALWAYS_INLINE bool JSObject::hasOwnProperty(JSGlobalObject* globalObject, unsigned propertyName) const
{
    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);
    return const_cast<JSObject*>(this)->methodTable()->getOwnPropertySlotByIndex(const_cast<JSObject*>(this), globalObject, propertyName, slot);
}

#if USE(JSVALUE64)
// See the declaration in JSObject.h. false = RESTART the whole operation.
inline NEVER_INLINE bool JSObject::tryPutDirectTransitionConcurrent(VM& vm, Structure* expectedSource, StructureID sourceID, Structure* newStructure, PropertyOffset offset, JSValue value)
{
    ASSERT(Options::useJSThreads());
    ASSERT(!expectedSource->isDictionary());
    ASSERT(type() != WebAssemblyGCObjectType);

    uint64_t word = taggedButterflyWord();
    size_t oldCapacity = expectedSource->outOfLineCapacity();
    size_t newCapacity = newStructure->outOfLineCapacity();

    // AB18-R1-H (N3/I21): a butterfly-less instance (word == 0, which includes
    // every N3 first out-of-line install) must never transition through this
    // lock-free leg. Its publication path (nukeStructureAndSetButterfly below)
    // claims the structureID lane with a PLAIN nuke store - or, for
    // inline/no-growth reshapes, a plain setStructure with no nuke at all -
    // while the lock-free N3 indexed first-install
    // (createInitialIndexedStorageConcurrent) claims the same all-zero-word
    // lane with a nuke-CAS, fires no TTL set, and takes no §10.6 stop. The E4
    // poll-free-window exclusivity proof (comment in the block below) only
    // excludes racers that fire sets or stop the world, so it does not exclude
    // that installer: the plain nuke aliases its claim, its failure-path
    // un-nuke hands the lane back mid-publication, and the two setStructure
    // stores race last-writer-wins - a silent lost add. Route word==0 through
    // the locked FirstInstall / N2 protocols below: they claim the lane by
    // nuke-CAS, return false on a lost race, and putDirectInternal's §2
    // RESTART loop replays the add against the winner's settled
    // structure/storage. NOTE (review): when E4 emission lands in the JIT
    // tiers, the SPEC-jit 5.5 mirrored predicate must carry this same word==0
    // exclusion, or the race re-opens from compiled code.
    if ((word & butterflyPointerMask) && expectedSource->mayTransitionLockFreeFromThisStructure(this, word)) {
        // ---- E4 lock-free path (THREAD.md "Watchpoint Optimizations"): the
        // owner of a (currentTID, 0) instance whose source TTL sets are valid
        // and watched transitions exactly as today's engine - no lock, no
        // CAS. I29 protocol: (1) allocate first; (2) revalidate with FRESH
        // loads; (3) poll-free value -> nuke -> butterfly -> new StructureID
        // (the value is stored BEFORE the new StructureID becomes visible -
        // no holes, I9; old-structure offsets stay valid in the copied
        // butterfly, so the earlier butterfly publication is benign);
        // (4) on revalidation failure fall to the locked protocols, never
        // spin here. Why no foreign write/flip can land inside the window:
        // F1/F2 fire the TTL sets UNDER a stop-the-world BEFORE flipping SW /
        // publishing (I10b/I13), and the stop must wait for this thread,
        // which has no poll between the revalidation (sets observed valid)
        // and the final store - so a foreign first write either strictly
        // precedes our revalidation (we observe the fired set / SW bit and
        // fall to the locked path) or strictly follows our publication.
        Butterfly* newButterfly = nullptr;
        if (oldCapacity != newCapacity) {
            ASSERT(newCapacity > oldCapacity);
            newButterfly = allocateMoreOutOfLineStorage(vm, oldCapacity, newCapacity); // May GC/poll => revalidate below (I29).
        }
        {
            AssertNoGC assertNoGC; // I29 step 3: no poll/allocation between revalidation and publication.
            if (this->structureID() == sourceID
                && expectedSource->revalidateLockFreeTransition(this, taggedButterflyWord())) {
                if (newButterfly)
                    nukeStructureAndSetButterfly(vm, sourceID, newButterfly);
                if (offset != invalidOffset) { // invalidOffset = structure-only reshape (attribute change): no value store.
                    // Flag-on, a quarantine-promoted deleted slot holds the
                    // D1 jsUndefined() store (I30), not EMPTY — accept both;
                    // anything else is still a real lost/aliased add.
                    ASSERT(!getDirect(offset) || !JSValue::encode(getDirect(offset)) || getDirect(offset).isUndefined());
                    putDirectOffset(vm, offset, value);
                }
                // I9/I29 step-3 release order: the slot store above (and, in the
                // growth case, the butterfly word published while the ID lane was
                // still nuked) must be globally visible BEFORE the new, un-nuked
                // StructureID. setStructure() performs a PLAIN m_structureID
                // store with no preceding barrier (JSCellInlines.h), and
                // putDirectOffset is a plain WriteBarrierBase store to a distinct
                // location — without this fence the compiler is free to sink the
                // slot store below the StructureID store (storeStoreFence is the
                // required compiler barrier on x86-64 and a dmb ishst on arm64).
                // This matches every other flag-on publish site in this file,
                // which all end "storeStoreFence(); setStructureIDDirectly(...)"
                // (growOutOfLineStorageForConcurrentLockedAdd, the §6 dictionary
                // leg, putDirectWithoutTransitionConcurrent).
                // NOTE: the fence does NOT order setStructure's trailing
                // m_flags/m_type/indexing-byte stores (they land after the ID
                // store); benign today because the E4 predicate excludes
                // ArrayStorage and these transitions change neither type nor
                // indexing mode — revisit if the E4 predicate is ever widened.
                // F3 triage (spawned-thread-butterfly-stress p8-for-p17 flake):
                // audited and exonerated — all five flag-on publish sites in
                // this file carry this protocol, the reader has the M7(d)
                // loadLoadFence + structureID re-validation, add transitions
                // never relocate existing offsets, and butterfly growth copies
                // are base-aligned, so no structure/butterfly mispairing can
                // return another property's value here. Do not add further
                // fences in this file for that signature; the suspect surface
                // is the JIT IC publication/refill path (bytecode/
                // PropertyInlineCache, InlineCacheHandler, InlineCacheCompiler,
                // Repatch), pending a concrete cited site.
                WTF::storeStoreFence();
                setStructure(vm, newStructure);
                return true;
            }
        }
        // Revalidation failed (racing F1/F2/foreign transition between the
        // allocation poll and the fresh loads): a speculatively allocated
        // butterfly is discarded unreferenced; take the locked protocols.
    }

    // ---- Locked protocols (§4.3 / N2). false => caller RESTART (fresh §2
    // dispatch: fresh target derivation, fresh F1/F2 checks).
    if (isOutOfLineOffset(offset))
        return trySegmentedTransition(vm, static_cast<JSObjectWithButterfly*>(this), expectedSource, newStructure, offset, value);
    return tryStructureOnlyTransition(vm, this, expectedSource, newStructure, offset, value);
}
#endif // USE(JSVALUE64)

#if USE(JSVALUE64)
// V5B-transition-heavy-regression: NEVER_INLINE forwarding trampoline for the
// jsThreads==true arm of putDirectInternal below. Previously BOTH template
// instantiations of the shared impl body were carried inline at every
// flag-off put site (putDirectInternal is ALWAYS_INLINE into its callers, and
// both lambda instantiations were taken inline at the dispatch), so every
// growth of the concurrent arm — most recently the §3 F1 SW-bit ensure on the
// out-of-line replace leg and the I18/I30 staleness-recheck block — inflated
// cold code, register pressure / spill code in the shared hot prologue, and
// i-cache/iTLB footprint at flag-off sites, and pushed enclosing callers over
// inlining budgets. That is the documented V5B-1 mechanism re-opening (see
// the V5B-1 comment in putDirectInternal). Routing the true arm through this
// out-of-line call makes flag-off codegen permanently insensitive to future
// growth of the concurrent path: flag-off sites carry only the frozen
// one-byte test plus a predicted-not-taken call.
//
// There is exactly ONE shared body (zero source duplication): the trampoline
// forwards to the same impl lambda, instantiated <true>, so the
// constexpr-false publish tails and loop back-edges remain statically
// unreachable in the true arm exactly as before, and no token of the body
// changed. The trampoline is instantiated once per PutMode (the closure type
// is unique per putDirectInternal<mode> instantiation, not per inlined call
// site), so the concurrent body is emitted out-of-line once and shared.
//
// Race/interleaving statement: this call boundary is not a safepoint — it
// takes no lock, performs no poll/park/allocation, and emits no fence — so
// the FIX-2 park-poll, M5 nuke-spin, I9 store-under-cell-lock, §3 F1 SW-bit
// ensure, and I18/I30 recheck->store orderings inside the body are unchanged;
// the recheck and putDirectOffset remain adjacent straight-line code inside
// the outlined instantiation, so the no-quarantine-promotion-inside-the-window
// proof carries verbatim. The dispatch still reads the frozen post-Config
// Options::useJSThreads() byte exactly once per call, and every §2 RESTART
// iteration stays inside the body's own while (true), so no iteration can
// change arms. The captured locals (including the structure-pinning
// snapshots) live in frames covered identically by conservative stack
// scanning.
template<typename Functor>
NEVER_INLINE auto putDirectInternalConcurrentOutOfLine(const Functor& functor) -> decltype(functor())
{
    ASSERT(Options::useJSThreads());
    return functor();
}
#endif // USE(JSVALUE64)

template<JSObject::PutMode mode>
ALWAYS_INLINE ASCIILiteral JSObject::putDirectInternal(VM& vm, PropertyName propertyName, JSValue value, unsigned newAttributes, PutPropertySlot& slot)
{
    ASSERT(value);
    ASSERT(value.isGetterSetter() == !!(newAttributes & PropertyAttribute::Accessor));
    ASSERT(value.isCustomGetterSetter() == !!(newAttributes & PropertyAttribute::CustomAccessorOrValue));
    ASSERT(!Heap::heap(value) || Heap::heap(value) == Heap::heap(this));
    ASSERT(!parseIndex(propertyName));

    // SPEC-objectmodel review round 1: flag-on, this function is the E5
    // named-property slow path, so it carries the §2 re-dispatch loop the
    // try* protocols require (false/RESTART => re-enter from a fresh
    // structureID/tag). Flag-off nothing ever RESTARTs and the loop body runs
    // exactly once - today's code (I22).
    // V5B-1: the I22 latch is now a compile-time template parameter instead
    // of a latched runtime bool. The flag is immutable after Config
    // finalization, so the frozen one-byte test is read exactly once at the
    // dispatch below and the arm taken is process-stable; splitting the latch
    // into a template parameter cannot change which arm any RESTART iteration
    // takes. The constexpr-false instantiation carries no park-poll call, no
    // nuked-ID spin, no staleness recheck, and no reachable loop back-edge -
    // the while (true) folds to straight-line code and every flag-off put
    // site emits exactly the pre-threads body. The constexpr-true
    // instantiation is token-identical to the previous jsThreads==true paths;
    // the lambda call boundary is not a safepoint (no poll/park/allocation),
    // so the M5, FIX-2, and I18/I30 proofs carry verbatim.
    // bench-transition-regress (V5B-1 extension): inside the templated impl
    // below the jsThreads discriminator is statically known, so bind it at
    // compile time at the butterfly publish sites - the flag-off instantiation
    // is byte-identical to the pre-threads publish body, with no
    // Options::useJSThreads() load on the transition fast path. The
    // JSObject::nukeStructureAndSetButterfly member keeps runtime dispatch for
    // all other callers.
    auto nukeStructureAndSetButterflyStatic = [&]<bool jsThreadsStatic>(StructureID oldStructureID, Butterfly* newButterfly) ALWAYS_INLINE_LAMBDA {
#if USE(JSVALUE64)
        if constexpr (jsThreadsStatic) {
            ASSERT(Options::useJSThreads());
            nukeStructureAndSetButterflyConcurrent(vm, oldStructureID, newButterfly);
            return;
        }
        ASSERT(!Options::useJSThreads());
#endif
        if (isX86() || vm.heap.mutatorShouldBeFenced()) {
            setStructureIDDirectly(oldStructureID.nuke());
            WTF::storeStoreFence();
            butterflyRef().set(vm, this, newButterfly);
            WTF::storeStoreFence();
            return;
        }

        butterflyRef().set(vm, this, newButterfly);
    };

    auto impl = [&]<bool jsThreads>() ALWAYS_INLINE_LAMBDA -> ASCIILiteral {
    while (true) {
#if USE(JSVALUE64)
    // FIX-2 class-(2) poll (stw-watchdog-timeout residual): this RESTART /
    // M5-nuke-spin loop is C++ straight-line with no bytecode poll site - a
    // mutator that keeps re-dispatching here while a SA.3 stop pends (e.g.
    // racing the very F2 fire that needs it to quiesce) holds heap access
    // for the whole spin and starves the conductor into the 30s watchdog.
    // Compiled only into the jsThreads instantiation; the helper parks
    // access-released across the window and the loop re-derives everything
    // afterwards (W1: every iteration is a fresh acquisition episode).
    if constexpr (jsThreads)
        JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld(vm);
#endif
    StructureID structureID = this->structureID();
#if USE(JSVALUE64)
    if constexpr (jsThreads) {
        if (structureID.isNuked()) [[unlikely]]
            continue; // M5: a racing publication is mid-flight; spin to the settled ID.
    }
#endif
    Structure* structure = structureID.decode();
    if (structure->isDictionary()) {
        ASSERT(!isCopyOnWrite(indexingMode()));
        if constexpr (mode == PutModePut) {
            if (!isStructureExtensible()) [[unlikely]]
                return putDirectToDictionaryWithoutExtensibility(vm, propertyName, value, slot);
        }

#if USE(JSVALUE64)
        if constexpr (jsThreads) {
            // §6 L3/L4 (review round 1): dictionary adds/replaces mutate the
            // structure and the object in tandem; serialize against deletes/
            // flatten/other adds with the cell lock (outer to the m_lock the
            // table edit takes - I20), and store the VALUE inside the same
            // critical section (no holes, I9; dictionary readers are
            // cell-locked, L3). DeferGC: the butterfly reallocation in the
            // lambda allocates under the cell lock (O1 sanctioned form).
            // Watchpoint-bearing steps (didReplaceProperty,
            // attributeChangeTransition, chain invalidation) run AFTER the
            // lock drops (O2: rank-6b locks are outer to §6-ranked locks).
            DeferGC deferGC(vm);
            PropertyOffset offset = invalidOffset;
            unsigned attributes = 0;
            bool isAdded = false;
            bool restart = false;
            bool readonlyError = false;
            ConcurrentLockedAddSlowAction slowAction = ConcurrentLockedAddSlowAction::None;
            {
                Locker cellLocker { cellLock() };
                StructureID lockedID = this->structureID();
                if (lockedID != structureID || !structureID.decode()->isDictionary()) {
                    restart = true; // A racing locked transition/flatten settled first.
                } else if (!classifyConcurrentLockedAdd(structure, slowAction)) {
                    // Review round 3 (§6 regime guard): the loaded word needs a
                    // protocol that cannot run under this lock (F1 flip, §4.2
                    // conversion, segmented coverage pre-grow, §4.8
                    // materialization) - run it outside and RESTART.
                    restart = true;
                } else {
                    // None + possible growth: own the structureID lane (CAS) so
                    // a racing lock-free N3 indexed first-install loses its
                    // nuke-CAS and re-dispatches (see
                    // putDirectWithoutTransitionConcurrent for the full note).
                    auto* idAtomic = std::bit_cast<Atomic<uint32_t>*>(reinterpret_cast<char*>(this) + JSCell::structureIDOffset());
                    bool preNuked = false;
                    if (!taggedButterflyWord()
                        && Structure::outOfLineCapacity(structure->maxOffset() + 1) != structure->outOfLineCapacity()) {
                        if (idAtomic->compareExchangeStrong(structureID.bits(), structureID.nuke().bits()) != structureID.bits())
                            restart = true;
                        else
                            preNuked = true;
                    }
                    if (!restart) {
#if ASSERT_ENABLED
                        // Discriminates fresh vs quarantine-reused offsets in the
                        // assert below (reused offsets never raise maxOffset,
                        // D1/I30). ASSERT_ENABLED-only so the release dictionary
                        // add path is byte-identical.
                        PropertyOffset preEditMaxOffset = structure->maxOffset();
#endif
                        std::tie(offset, attributes, isAdded) = structure->addOrReplacePropertyWithoutTransition(vm, propertyName, newAttributes, [&](const GCSafeConcurrentJSLocker&, PropertyOffset offset, PropertyOffset newMaxOffset) {
                            unsigned oldOutOfLineCapacity = structure->outOfLineCapacity();
                            unsigned newOutOfLineCapacity = Structure::outOfLineCapacity(newMaxOffset);
                            if (newOutOfLineCapacity != oldOutOfLineCapacity)
                                growOutOfLineStorageForConcurrentLockedAdd(vm, structureID, structure, newMaxOffset, oldOutOfLineCapacity, newOutOfLineCapacity);
                            else
                                structure->setMaxOffset(vm, newMaxOffset);
                            // FRESH offsets (offset > preEditMaxOffset) must be
                            // EMPTY: inline storage is zeroed at creation, and the
                            // only dictionary shrink (flattenDictionaryStructureImpl)
                            // runs only under a stop (Structure.cpp:1729) and
                            // gcSafeZeroMemory-clears all freed inline/out-of-line
                            // tails (Structure.cpp:1751-1773) — a future change
                            // letting flatten run unstopped or skip the zeroing
                            // breaks this arm.
                            // REUSED quarantine-promoted offsets must be NON-empty:
                            // normally they hold the D1 jsUndefined() store — but a
                            // sanctioned tardy putDirectOffset (a writer inside its
                            // poll-free check->store window when the delete landed)
                            // may have overwritten the undefined BEFORE the epoch
                            // bump that promoted the slot (INTEGRATE-objectmodel.md
                            // §54; D1 only promises tardy READERS "old value or
                            // undefined", SPEC-objectmodel §6). The residue is a
                            // fully formed, GC-visited JSValue (propertyStorageSize
                            // keeps counting quarantined/reusable slots) and is
                            // overwritten by putDirectOffset below inside this same
                            // cell-locked section (dictionary readers are
                            // cell-locked, L3), so it is never observable under the
                            // new name — accept it, but never EMPTY (D1 forbids a
                            // clear()-style delete; a quarantine bypass handing out
                            // never-zapped slots would trip this).
                            ASSERT_UNUSED(offset, offset <= preEditMaxOffset ? !!JSValue::encode(getDirect(offset)) : !JSValue::encode(getDirect(offset)));
                        });
                        if (!isAdded && mode == PutModePut && (attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor)) [[unlikely]]
                            readonlyError = true;
                        else
                            putDirectOffset(vm, offset, value); // I9: with the table edit, inside the lock.
                    }
                    if (preNuked && this->structureID().isNuked()) {
                        WTF::storeStoreFence();
                        setStructureIDDirectly(structureID); // Inline offset / replace: restore the pre-nuked lane.
                    }
                }
            }
            if (restart) {
                performConcurrentLockedAddSlowAction(vm, slowAction);
                continue;
            }
            if (readonlyError)
                return ReadonlyPropertyChangeError;
            if (!isAdded) {
                structure->didReplaceProperty(offset);
                if ((mode == PutModeDefineOwnProperty) && (newAttributes != attributes || (newAttributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))) {
                    DeferredStructureTransitionWatchpointFire deferred(vm, structure);
                    setStructure(vm, Structure::attributeChangeTransition(vm, structure, propertyName, newAttributes, &deferred));
                    if (mayBePrototype()) [[unlikely]]
                        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change);
                } else {
                    ASSERT(!(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue));
                    slot.setExistingProperty(this, offset);
                }
                return { };
            }
            validateOffset(offset);
            slot.setNewProperty(this, offset);
            if (attributes & PropertyAttribute::ReadOnly)
                this->structure()->setContainsReadOnlyProperties();
            if (mayBePrototype()) [[unlikely]]
                vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
            return { };
        }
#endif

        auto [offset, attributes, isAdded] = structure->addOrReplacePropertyWithoutTransition(vm, propertyName, newAttributes, [&](const GCSafeConcurrentJSLocker&, PropertyOffset offset, PropertyOffset newMaxOffset) {
            unsigned oldOutOfLineCapacity = structure->outOfLineCapacity();
            unsigned newOutOfLineCapacity = Structure::outOfLineCapacity(newMaxOffset);
            if (newOutOfLineCapacity != oldOutOfLineCapacity) {
                Butterfly* butterfly = allocateMoreOutOfLineStorage(vm, oldOutOfLineCapacity, newOutOfLineCapacity);
                // Statically jsThreads == false here: the constexpr jsThreads
                // dictionary leg above returns/continues on every path.
                nukeStructureAndSetButterflyStatic.template operator()<jsThreads>(structureID, butterfly);
                structure->setMaxOffset(vm, newMaxOffset);
                WTF::storeStoreFence();
                setStructureIDDirectly(structureID);
            } else
                structure->setMaxOffset(vm, newMaxOffset);

            // This assertion verifies that the concurrent GC won't read garbage if the concurrentGC
            // is running at the same time we put without transitioning.
            ASSERT_UNUSED(offset, !getDirect(offset) || !JSValue::encode(getDirect(offset)));
        });

        if (!isAdded) {
            if constexpr (mode == PutModePut) {
                if (attributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor) [[unlikely]]
                    return ReadonlyPropertyChangeError;
            }

            putDirectOffset(vm, offset, value);
            structure->didReplaceProperty(offset);

            // FIXME: Check attributes against PropertyAttribute::CustomAccessorOrValue. Changing GetterSetter should work w/o transition.
            // https://bugs.webkit.org/show_bug.cgi?id=214342
            if ((mode == PutModeDefineOwnProperty) && (newAttributes != attributes || (newAttributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))) {
                DeferredStructureTransitionWatchpointFire deferred(vm, structure);
                setStructure(vm, Structure::attributeChangeTransition(vm, structure, propertyName, newAttributes, &deferred));
                if (mayBePrototype()) [[unlikely]]
                    vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change);
            } else {
                ASSERT(!(attributes & PropertyAttribute::AccessorOrCustomAccessorOrValue));
                slot.setExistingProperty(this, offset);
            }
            return { };
        }

        validateOffset(offset);
        putDirectOffset(vm, offset, value);
        slot.setNewProperty(this, offset);
        if (attributes & PropertyAttribute::ReadOnly)
            this->structure()->setContainsReadOnlyProperties();
        if (mayBePrototype()) [[unlikely]]
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
        return { };
    }

    {
        PropertyOffset offset;
        Structure* newStructure = Structure::addPropertyTransitionToExistingStructure(structure, propertyName, newAttributes, offset);
        if (newStructure) {
            validateOffset(offset);
            ASSERT(newStructure->isValidOffset(offset));

#if USE(JSVALUE64)
            if constexpr (jsThreads) {
                // Review round 1: route through the E4 gate / locked
                // protocols instead of the unconditional lock-free sequence.
                if (!tryPutDirectTransitionConcurrent(vm, structure, structureID, newStructure, offset, value))
                    continue; // RESTART from a fresh structureID/tag (§2).
                slot.setNewProperty(this, offset);
                if (mayBePrototype()) [[unlikely]]
                    vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
                return { };
            }
#endif

            Butterfly* newButterfly = butterfly();
            if (structure->outOfLineCapacity() != newStructure->outOfLineCapacity()) {
                ASSERT(newStructure != this->structure());
                newButterfly = allocateMoreOutOfLineStorage(vm, structure->outOfLineCapacity(), newStructure->outOfLineCapacity());
                // Statically jsThreads == false here: the constexpr jsThreads
                // transition leg above returns or continues.
                nukeStructureAndSetButterflyStatic.template operator()<jsThreads>(structureID, newButterfly);
            }

            // This assertion verifies that the concurrent GC won't read garbage if the concurrentGC
            // is running at the same time we put without transitioning.
            ASSERT(!getDirect(offset) || !JSValue::encode(getDirect(offset)));
            putDirectOffset(vm, offset, value);
            setStructure(vm, newStructure);
            slot.setNewProperty(this, offset);
            if (mayBePrototype()) [[unlikely]]
                vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
            return { };
        }
    }

    unsigned currentAttributes;
    PropertyOffset offset = structure->get(vm, propertyName, currentAttributes);
    if (offset != invalidOffset) {
        if (mode == PutModePut && (currentAttributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor))
            return ReadonlyPropertyChangeError;

#if USE(JSVALUE64)
        // §3 F1 (review round 1): a REPLACE through an out-of-line offset is a
        // butterfly write; a foreign writer on an SW=0 flat word must fire
        // writeThreadLocal and flip SW before the plain store lands (I12/I21
        // - otherwise an owner T1 copying resize can silently drop it).
        // Inline replaces are cell stores (atomic for free; never copied by
        // resizes), and dictionary objects never reach this leg.
        if constexpr (jsThreads) {
            if (isOutOfLineOffset(offset)) [[unlikely]] {
                uint64_t word = taggedButterflyWord();
                if ((word & butterflyPointerMask) && !isSegmentedButterfly(word)
                    && !butterflySharedWrite(word) && butterflyWriterIsForeign(word)) // incl. §9.6 forceButterflySWBit
                    ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this));
            }
        }

        if constexpr (jsThreads) {
            // I18/I30 staleness guard (same pattern as the round-4
            // dictionary-delete fix in JSObject.cpp): `offset` was resolved at
            // the line above through the m_lock-holding getConcurrently
            // routing, and the SW-bit publication just above can take a
            // per-event stop — both are safepoints a parked/stopped writer
            // crosses with `offset` in hand. While we were parked, a racing
            // delete can retire this property (offset quarantined, D1
            // undefined stored), a safepoint can PROMOTE the quarantined
            // offset, and a racing add can reuse it for a DIFFERENT property.
            // Storing through the stale offset would clobber the new
            // property's value (lost add / type confusion downstream) and
            // fire replacement watchpoints against the wrong shape. This
            // applies to INLINE offsets too — the getConcurrently park
            // precedes both flavors and inline deleted offsets are
            // quarantined/reused the same way — so the recheck is
            // unconditional. Re-validate the structureID snapshot directly
            // before the store: between this load and putDirectOffset there
            // is no park, poll, lock, or allocation (straight-line code), and
            // quarantine promotion requires every mutator to cross a
            // safepoint, so no reuse can complete inside the recheck->store
            // window. The local `structure` pointer pins the snapshot against
            // ID recycling (conservative scan), non-dictionary tables are
            // never edited in place, and a nuked ID fails the == and
            // RESTARTs — so an unchanged structureID proves `offset` still
            // names this property. The recheck sits after ensureSharedWriteBit
            // (which can stop) and BEFORE didReplaceProperty (which can fire
            // rank-6b watchpoints and park); flag-on, didReplaceProperty moves
            // AFTER the validated store so nothing parkable separates the
            // recheck from the store. NOTE: the baseline/DFG/FTL PutById
            // replace ICs are unaffected — their inline structure-check ->
            // store sequence has no safepoint between check and store; this
            // runtime leg was the only unguarded replace path.
            if (this->structureID() != structureID)
                continue; // RESTART from a fresh structureID/tag (§2); offset/attributes re-derive at loop top.
            putDirectOffset(vm, offset, value);
            structure->didReplaceProperty(offset);
        } else {
#endif

        structure->didReplaceProperty(offset);
        putDirectOffset(vm, offset, value);
#if USE(JSVALUE64)
        }
#endif

        // FIXME: Check attributes against PropertyAttribute::CustomAccessorOrValue. Changing GetterSetter should work w/o transition.
        // https://bugs.webkit.org/show_bug.cgi?id=214342
        if ((mode == PutModeDefineOwnProperty) && (newAttributes != currentAttributes || (newAttributes & PropertyAttribute::AccessorOrCustomAccessorOrValue))) {
            // We want the structure transition watchpoint to fire after this object has switched structure.
            // This allows adaptive watchpoints to observe if the new structure is the one we want.
            DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, structure);
            Structure* attributeChanged = Structure::attributeChangeTransition(vm, structure, propertyName, newAttributes, &deferredWatchpointFire);
#if USE(JSVALUE64)
            if constexpr (jsThreads) {
                if (attributeChanged != structure) [[unlikely]] {
                    // Review round 1: an attribute change is a butterfly-untouched
                    // (N2) structure publication - route it through the E4 gate /
                    // locked header-CAS so racing transitions cannot clobber each
                    // other's setStructure. The value is already stored (above),
                    // so RESTART re-runs the replace idempotently.
                    if (!tryPutDirectTransitionConcurrent(vm, structure, structureID, attributeChanged, invalidOffset, JSValue()))
                        continue;
                } else
                    setStructure(vm, attributeChanged);
            } else
#endif
            setStructure(vm, attributeChanged);
            if (mayBePrototype()) [[unlikely]]
                vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change);
        } else {
            ASSERT(!(currentAttributes & PropertyAttribute::AccessorOrCustomAccessorOrValue));
            slot.setExistingProperty(this, offset);
        }

        return { };
    }

    if constexpr (mode == PutModePut) {
        if (!isStructureExtensible()) [[unlikely]]
            return NonExtensibleObjectPropertyDefineError;
    }

    // We want the structure transition watchpoint to fire after this object has switched structure.
    // This allows adaptive watchpoints to observe if the new structure is the one we want.
    DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, structure);
    Structure* newStructure = Structure::addNewPropertyTransition(vm, structure, propertyName, newAttributes, offset, slot.context(), &deferredWatchpointFire);

    validateOffset(offset);
    ASSERT(newStructure->isValidOffset(offset));

#if USE(JSVALUE64)
    if constexpr (jsThreads) {
        // Review round 1: same E4-gate / locked-protocol routing as the
        // existing-structure transition leg above.
        if (!tryPutDirectTransitionConcurrent(vm, structure, structureID, newStructure, offset, value))
            continue; // RESTART from a fresh structureID/tag (§2).
        slot.setNewProperty(this, offset);
        if (newAttributes & PropertyAttribute::ReadOnly)
            newStructure->setContainsReadOnlyProperties();
        if (mayBePrototype()) [[unlikely]]
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
        return { };
    }
#endif

    size_t oldCapacity = structure->outOfLineCapacity();
    size_t newCapacity = newStructure->outOfLineCapacity();
    ASSERT(oldCapacity <= newCapacity);
    if (oldCapacity != newCapacity) {
        Butterfly* newButterfly = allocateMoreOutOfLineStorage(vm, oldCapacity, newCapacity);
        // Statically jsThreads == false here: the constexpr jsThreads
        // transition leg above returns or continues.
        nukeStructureAndSetButterflyStatic.template operator()<jsThreads>(structureID, newButterfly);
    }

    // This assertion verifies that the concurrent GC won't read garbage if the concurrentGC
    // is running at the same time we put without transitioning.
    ASSERT(!getDirect(offset) || !JSValue::encode(getDirect(offset)));
    putDirectOffset(vm, offset, value);
    setStructure(vm, newStructure);
    slot.setNewProperty(this, offset);
    if (newAttributes & PropertyAttribute::ReadOnly)
        newStructure->setContainsReadOnlyProperties();
    if (mayBePrototype()) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
    return { };
    } // while (true)
    };

#if USE(JSVALUE64)
    // V5B-1 / V5B-transition-heavy-regression: frozen-Config one-byte test;
    // the threads instantiation is OUTLINED behind the NEVER_INLINE
    // trampoline above (one shared out-of-line copy per PutMode), so flag-off
    // put sites carry the pre-threads body plus this single predicted-false
    // branch and a cold call — insensitive to future growth of the
    // concurrent arm. See the trampoline comment for the full race statement.
    if (Options::useJSThreads()) [[unlikely]] {
        return putDirectInternalConcurrentOutOfLine([&]() ALWAYS_INLINE_LAMBDA {
            return impl.template operator()</* jsThreads */ true>();
        });
    }
#endif
    return impl.template operator()</* jsThreads */ false>();
}

inline bool JSObject::mayBePrototype() const
{
    return structure()->mayBePrototype();
}

inline bool JSObject::canGetIndexQuicklyForTypedArray(unsigned i) const
{
    switch (type()) {
#define CASE_TYPED_ARRAY_TYPE(name) \
    case name ## ArrayType :\
        return uncheckedDowncast<JS ## name ## Array>(this)->canGetIndexQuickly(i);
        FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(CASE_TYPED_ARRAY_TYPE)
#undef CASE_TYPED_ARRAY_TYPE
    default:
        return false;
    }
}

inline JSValue JSObject::getIndexQuicklyForTypedArray(unsigned i, ArrayProfile* arrayProfile) const
{
#if USE(LARGE_TYPED_ARRAYS)
    if (i > ArrayProfile::s_smallTypedArrayMaxLength && arrayProfile)
        arrayProfile->setMayBeLargeTypedArray();
#else
    UNUSED_PARAM(arrayProfile);
#endif

    switch (type()) {
#define CASE_TYPED_ARRAY_TYPE(name) \
    case name ## ArrayType : {\
        auto* typedArray = uncheckedDowncast<JS ## name ## Array>(this);\
        RELEASE_ASSERT(typedArray->canGetIndexQuickly(i));\
        return typedArray->getIndexQuickly(i);\
    }
        FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(CASE_TYPED_ARRAY_TYPE)
#undef CASE_TYPED_ARRAY_TYPE
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return JSValue();
    }
}

inline void JSObject::setIndexQuicklyForTypedArray(unsigned i, JSValue value)
{
    switch (type()) {
#define CASE_TYPED_ARRAY_TYPE(name) \
    case name ## ArrayType : {\
        auto* typedArray = uncheckedDowncast<JS ## name ## Array>(this);\
        RELEASE_ASSERT(typedArray->canSetIndexQuickly(i, value));\
        typedArray->setIndexQuickly(i, value);\
        break;\
    }
        FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(CASE_TYPED_ARRAY_TYPE)
#undef CASE_TYPED_ARRAY_TYPE
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }
}

ALWAYS_INLINE void JSObject::setIndexQuicklyForArrayStorageIndexingType(VM& vm, unsigned i, JSValue v)
{
    ArrayStorage* storage = this->butterfly()->arrayStorage();
    WriteBarrier<Unknown>& x = storage->m_vector[i];
    JSValue old = x.get();
    x.set(vm, this, v);
    if (!old) {
        ++storage->m_numValuesInVector;
        if (i >= storage->length())
            storage->setLength(i + 1);
    }
}

inline bool JSObject::trySetIndexQuicklyForTypedArray(unsigned i, JSValue v, ArrayProfile* arrayProfile)
{
    switch (type()) {
#if USE(LARGE_TYPED_ARRAYS)
#define UPDATE_ARRAY_PROFILE(i, arrayProfile) do { \
        if ((i > ArrayProfile::s_smallTypedArrayMaxLength) && arrayProfile)\
            arrayProfile->setMayBeLargeTypedArray();\
    } while (false)
#else
#define UPDATE_ARRAY_PROFILE(i, arrayProfile) do { \
    UNUSED_PARAM(arrayProfile);\
    } while (false)
#endif
#define CASE_TYPED_ARRAY_TYPE(name) \
    case name ## ArrayType : { \
        auto* typedArray = uncheckedDowncast<JS ## name ## Array>(this);\
        if (!typedArray->canSetIndexQuickly(i, v))\
            return false;\
        typedArray->setIndexQuickly(i, v);\
        UPDATE_ARRAY_PROFILE(i, arrayProfile);\
        return true;\
    }
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(CASE_TYPED_ARRAY_TYPE)
#undef CASE_TYPED_ARRAY_TYPE
#undef UPDATE_ARRAY_PROFILE
    default:
        return false;
    }
}

inline void JSObject::validatePutOwnDataProperty(VM& vm, PropertyName propertyName, JSValue value)
{
#if ASSERT_ENABLED
    ASSERT(value);
    ASSERT(!Heap::heap(value) || Heap::heap(value) == Heap::heap(this));
    unsigned attributes;
    PropertyOffset offset = structure()->get(vm, propertyName, attributes);
    if (isValidOffset(offset))
        ASSERT(!(attributes & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly)));
    else if (TypeInfo::hasStaticPropertyTable(inlineTypeFlags())) {
        if (auto entry = findPropertyHashEntry(propertyName))
            ASSERT(!(entry->value->attributes() & (PropertyAttribute::Accessor | PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly)));
    }
#else // not ASSERT_ENABLED
    UNUSED_PARAM(vm);
    UNUSED_PARAM(propertyName);
    UNUSED_PARAM(value);
#endif // not ASSERT_ENABLED
}

inline bool JSObject::putOwnDataProperty(VM& vm, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    validatePutOwnDataProperty(vm, propertyName, value);
    return putDirectInternal<PutModePut>(vm, propertyName, value, 0, slot).isNull();
}

inline bool JSObject::putOwnDataPropertyMayBeIndex(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = getVM(globalObject);
    validatePutOwnDataProperty(vm, propertyName, value);
    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return putDirectIndex(globalObject, index.value(), value, 0, PutDirectIndexLikePutDirect);

    return putDirectInternal<PutModePut>(vm, propertyName, value, 0, slot).isNull();
}

ALWAYS_INLINE CallData getCallData(JSCell* cell)
{
    if (cell->type() == JSFunctionType)
        return JSFunction::getCallData(cell);
    CallData result = cell->methodTable()->getCallData(cell);
    ASSERT(result.type == CallData::Type::None || cell->isValidCallee());
    return result;
}

inline CallData getCallData(JSValue value)
{
    if (!value.isCell())
        return { };
    return getCallData(value.asCell());
}

ALWAYS_INLINE CallData getCallDataInline(JSCell* cell)
{
    if (cell->type() == JSFunctionType)
        return JSFunction::getCallDataInline(cell);
    CallData result = cell->methodTable()->getCallData(cell);
    ASSERT(result.type == CallData::Type::None || cell->isValidCallee());
    return result;
}

ALWAYS_INLINE CallData getCallDataInline(JSValue value)
{
    if (!value.isCell())
        return { };
    return getCallDataInline(value.asCell());
}

inline CallData getConstructData(JSValue value)
{
    if (!value.isCell())
        return { };
    JSCell* cell = value.asCell();
    if (cell->type() == JSFunctionType)
        return JSFunction::getConstructData(cell);
    CallData result = cell->methodTable()->getConstructData(cell);
    ASSERT(result.type == CallData::Type::None || cell->isValidCallee());
    return result;
}

ALWAYS_INLINE CallData getConstructDataInline(JSCell* cell)
{
    if (cell->type() == JSFunctionType)
        return JSFunction::getConstructDataInline(cell);
    CallData result = cell->methodTable()->getConstructData(cell);
    ASSERT(result.type == CallData::Type::None || cell->isValidCallee());
    return result;
}

ALWAYS_INLINE CallData getConstructDataInline(JSValue value)
{
    if (!value.isCell())
        return { };
    return getConstructDataInline(value.asCell());
}

inline bool JSObject::deleteProperty(JSGlobalObject* globalObject, PropertyName propertyName)
{
    DeletePropertySlot slot;
    return this->methodTable()->deleteProperty(this, globalObject, propertyName, slot);
}

inline bool JSObject::deleteProperty(JSGlobalObject* globalObject, uint32_t propertyName)
{
    return this->methodTable()->deletePropertyByIndex(this, globalObject, propertyName);
}

inline bool JSObject::deleteProperty(JSGlobalObject* globalObject, uint64_t propertyName)
{
    if (propertyName <= MAX_ARRAY_INDEX) [[likely]]
        return deleteProperty(globalObject, static_cast<uint32_t>(propertyName));
    ASSERT(propertyName <= maxSafeInteger());
    return deleteProperty(globalObject, Identifier::from(globalObject->vm(), propertyName));
}

inline JSValue JSObject::get(JSGlobalObject* globalObject, uint64_t propertyName) const
{
    if (propertyName <= MAX_ARRAY_INDEX) [[likely]]
        return get(globalObject, static_cast<uint32_t>(propertyName));
    ASSERT(propertyName <= maxSafeInteger());
    return get(globalObject, Identifier::from(globalObject->vm(), propertyName));
}

JSObject* createInvalidPrivateNameError(JSGlobalObject*);
JSObject* createRedefinedPrivateNameError(JSGlobalObject*);
JSObject* createReinstallPrivateMethodError(JSGlobalObject*);
JSObject* createPrivateMethodAccessError(JSGlobalObject*);

ALWAYS_INLINE bool JSObject::getPrivateFieldSlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    ASSERT(propertyName.isPrivateName());
    VM& vm = getVM(globalObject);
    Structure* structure = object->structure();

    unsigned attributes;
    PropertyOffset offset = structure->get(vm, propertyName, attributes);
    if (offset == invalidOffset)
        return false;

    JSValue value = object->getDirect(offset);
#if ASSERT_ENABLED
    ASSERT(value);
    if (value.isCell()) {
        JSCell* cell = value.asCell();
        JSType type = cell->type();
        UNUSED_PARAM(cell);
        ASSERT_UNUSED(type, type != GetterSetterType && type != CustomGetterSetterType);
        // FIXME: For now, private fields do not support getter/setter fields. Later on, we will need to fill in accessor metadata here,
        // as in JSObject::getOwnNonIndexPropertySlot()
        // https://bugs.webkit.org/show_bug.cgi?id=194435
    }
#endif

    slot.setValue(object, attributes, value, offset);
    return true;
}

inline bool JSObject::hasPrivateField(JSGlobalObject* globalObject, PropertyName propertyName)
{
    ASSERT(propertyName.isPrivateName());
    VM& vm = getVM(globalObject);
    unsigned attributes;
    return structure()->get(vm, propertyName, attributes) != invalidOffset;
}

inline bool JSObject::getPrivateField(JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!slot.isVMInquiry());
    if (!JSObject::getPrivateFieldSlot(this, globalObject, propertyName, slot)) {
        throwException(globalObject, scope, createInvalidPrivateNameError(globalObject));
        RELEASE_AND_RETURN(scope, false);
    }
    EXCEPTION_ASSERT(!scope.exception());
    RELEASE_AND_RETURN(scope, true);
}

inline void JSObject::setPrivateField(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& putSlot)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::HasProperty);
    if (!JSObject::getPrivateFieldSlot(this, globalObject, propertyName, slot)) {
        throwException(globalObject, scope, createInvalidPrivateNameError(globalObject));
        RELEASE_AND_RETURN(scope, void());
    }
    EXCEPTION_ASSERT(!scope.exception());

    scope.release();
    putDirect(vm, propertyName, value, putSlot);
}

inline void JSObject::definePrivateField(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& putSlot)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::HasProperty);
    if (JSObject::getPrivateFieldSlot(this, globalObject, propertyName, slot)) {
        throwException(globalObject, scope, createRedefinedPrivateNameError(globalObject));
        RELEASE_AND_RETURN(scope, void());
    }
    EXCEPTION_ASSERT(!scope.exception());

    scope.release();
    putDirect(vm, propertyName, value, putSlot);
}

ALWAYS_INLINE void JSObject::getNonReifiedStaticPropertyNames(VM& vm, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    if (staticPropertiesReified())
        return;

    Structure* structure = this->structure();
    // Add properties from the static hashtables of properties
    for (const ClassInfo* info = classInfo(); info; info = info->parentClass) {
        const HashTable* table = info->staticPropHashTable;
        if (!table)
            continue;

        for (auto iter = table->begin(); iter != table->end(); ++iter) {
            if (mode == DontEnumPropertiesMode::Include || !(iter->attributes() & PropertyAttribute::DontEnum)) {
                auto identifier = Identifier::fromString(vm, iter.key());
                // If the structure is shadowing the static property use it's attributes to determine if
                // the property name is enumerable but add it here to preserve the right property order.
                unsigned structureAttributes;
                if (isValidOffset(structure->get(vm, identifier, structureAttributes)) && (mode == DontEnumPropertiesMode::Exclude && (structureAttributes & PropertyAttribute::DontEnum)))
                    continue;
                propertyNames.add(identifier);
            }
        }
    }
}

inline bool JSObject::hasPrivateBrand(JSGlobalObject*, JSValue brand)
{
    ASSERT(brand.isSymbol() && asSymbol(brand)->uid().isPrivate());
    Structure* structure = this->structure();
    return structure->isBrandedStructure() && uncheckedDowncast<BrandedStructure>(structure)->checkBrand(asSymbol(brand));
}

inline void JSObject::checkPrivateBrand(JSGlobalObject* globalObject, JSValue brand)
{
    ASSERT(brand.isSymbol() && asSymbol(brand)->uid().isPrivate());
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Structure* structure = this->structure();
    if (!structure->isBrandedStructure() || !uncheckedDowncast<BrandedStructure>(structure)->checkBrand(asSymbol(brand)))
        throwException(globalObject, scope, createPrivateMethodAccessError(globalObject));
}

inline void JSObject::setPrivateBrand(JSGlobalObject* globalObject, JSValue brand)
{
    ASSERT(brand.isSymbol() && asSymbol(brand)->uid().isPrivate());
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Structure* structure = this->structure();
    if (structure->isBrandedStructure() && uncheckedDowncast<BrandedStructure>(structure)->checkBrand(asSymbol(brand))) {
        throwException(globalObject, scope, createReinstallPrivateMethodError(globalObject));
        RELEASE_AND_RETURN(scope, void());
    }
    EXCEPTION_ASSERT(!scope.exception());

    scope.release();

    DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, structure);

    Structure* newStructure = Structure::setBrandTransition(vm, structure, asSymbol(brand), &deferredWatchpointFire);
    ASSERT(newStructure->isBrandedStructure());
    ASSERT(newStructure->outOfLineCapacity() || !this->structure()->outOfLineCapacity());
    this->setStructure(vm, newStructure);
}

// Function forEachOwnIndexedProperty should only used in the fast path
// for copying own non-GetterSetter indexed properties.
template<JSObject::SortMode mode, typename Functor>
void JSObject::forEachOwnIndexedProperty(JSGlobalObject* globalObject, const Functor& functor)
{
    ASSERT(structure()->canPerformFastPropertyEnumerationCommon());
    ASSERT(canHaveExistingOwnIndexedProperties() && !canHaveExistingOwnIndexedGetterSetterProperties());
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        break;

    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES:
    case ALL_DOUBLE_INDEXING_TYPES: {
        unsigned usedLength = butterfly()->publicLength();
        for (unsigned i = 0; i < usedLength; ++i) {
            JSValue value = getDirectIndex(globalObject, i);
            RETURN_IF_EXCEPTION(scope, void());
            if (value && functor(i, value) == IterationStatus::Done)
                return;
        }
        break;
    }

    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = butterfly()->arrayStorage();
        unsigned usedVectorLength = std::min(storage->length(), storage->vectorLength());
        for (unsigned i = 0; i < usedVectorLength; ++i) {
            auto value = storage->m_vector[i];
            if (!value)
                continue;
            if (functor(i, value.get()) == IterationStatus::Done)
                return;
        }

        if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
            MarkedArgumentBuffer values;
            if constexpr (mode == JSObject::SortMode::Default) {
                Vector<unsigned, 8> properties;
                for (auto& [key, value] : *map) {
                    if (!(value.attributes() & PropertyAttribute::DontEnum)) {
                        properties.append(key);
                        values.appendWithCrashOnOverflow(value.get());
                    }
                }

                for (size_t i = 0; i < properties.size(); ++i) {
                    if (functor(properties[i], values.at(i)) == IterationStatus::Done)
                        return;
                }
            } else {
                Vector<std::tuple<unsigned, unsigned>, 8> propertyAndValueIndexTuples;
                unsigned valueIndex = 0;
                for (auto& [key, value] : *map) {
                    if (!(value.attributes() & PropertyAttribute::DontEnum)) {
                        propertyAndValueIndexTuples.append({ key, valueIndex++ });
                        values.appendWithCrashOnOverflow(value.get());
                    }
                }

                std::ranges::sort(propertyAndValueIndexTuples, [](auto a, auto b) {
                    return std::get<0>(a) < std::get<0>(b);
                });
                for (size_t i = 0; i < propertyAndValueIndexTuples.size(); ++i) {
                    auto [property, valueIndex] = propertyAndValueIndexTuples.at(i);
                    if (functor(property, values.at(valueIndex)) == IterationStatus::Done)
                        return;
                }
            }
        }
        break;
    }

    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

inline void JSObject::initializeIndex(ObjectInitializationScope& scope, unsigned i, JSValue v)
{
    initializeIndex(scope, i, v, indexingType());
}

ALWAYS_INLINE void JSObject::initializeIndex(ObjectInitializationScope& scope, unsigned i, JSValue v, IndexingType indexingType)
{
    VM& vm = scope.vm();
    auto* butterfly = this->butterfly();
    switch (indexingType) {
    case ALL_UNDECIDED_INDEXING_TYPES: {
        setIndexQuicklyToUndecided(vm, i, v);
        break;
    }
    case ALL_INT32_INDEXING_TYPES: {
        ASSERT(i < butterfly->publicLength());
        ASSERT(i < butterfly->vectorLength());
        if (!v.isInt32()) {
            convertInt32ToDoubleOrContiguousWhilePerformingSetIndex(vm, i, v);
            break;
        }
        [[fallthrough]];
    }
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        ASSERT(i < butterfly->publicLength());
        ASSERT(i < butterfly->vectorLength());
        butterfly->contiguous().at(this, i).set(vm, this, v);
        break;
    }
    case ALL_DOUBLE_INDEXING_TYPES: {
        ASSERT(i < butterfly->publicLength());
        ASSERT(i < butterfly->vectorLength());
        if (!v.isNumber()) {
            convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
            return;
        }
        double value = v.asNumber();
        if (value != value) {
            convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
            return;
        }
        butterfly->contiguousDouble().at(this, i) = value;
        break;
    }
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = butterfly->arrayStorage();
        ASSERT(i < storage->length());
        ASSERT(i < storage->m_numValuesInVector);
        storage->m_vector[i].set(vm, this, v);
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

inline void JSObject::initializeIndexWithoutBarrier(ObjectInitializationScope& scope, unsigned i, JSValue v)
{
    initializeIndexWithoutBarrier(scope, i, v, indexingType());
}

ALWAYS_INLINE void JSObject::initializeIndexWithoutBarrier(ObjectInitializationScope&, unsigned i, JSValue v, IndexingType indexingType)
{
    auto* butterfly = this->butterfly();
    switch (indexingType) {
    case ALL_UNDECIDED_INDEXING_TYPES: {
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    case ALL_INT32_INDEXING_TYPES: {
        ASSERT(i < butterfly->publicLength());
        ASSERT(i < butterfly->vectorLength());
        RELEASE_ASSERT(v.isInt32());
        [[fallthrough]];
    }
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        ASSERT(i < butterfly->publicLength());
        ASSERT(i < butterfly->vectorLength());
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(v);
        break;
    }
    case ALL_DOUBLE_INDEXING_TYPES: {
        ASSERT(i < butterfly->publicLength());
        ASSERT(i < butterfly->vectorLength());
        RELEASE_ASSERT(v.isNumber());
        double value = v.asNumber();
        RELEASE_ASSERT(value == value);
        butterfly->contiguousDouble().at(this, i) = value;
        break;
    }
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = butterfly->arrayStorage();
        ASSERT(i < storage->length());
        ASSERT(i < storage->m_numValuesInVector);
        storage->m_vector[i].setWithoutWriteBarrier(v);
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

inline bool JSObject::canHaveExistingOwnIndexedGetterSetterProperties()
{
    if (!hasIndexedProperties(indexingType()))
        return false;

    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES:
    case ALL_DOUBLE_INDEXING_TYPES:
        return false;
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        SparseArrayValueMap* map = butterfly()->arrayStorage()->m_sparseMap.get();
        if (!map)
            return false;
        return map->hasAnyKindOfGetterSetterProperties();
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

inline unsigned JSObject::canHaveExistingOwnIndexedProperties() const
{
    if (!hasIndexedProperties(indexingType()))
        return false;

    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        return false;
    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES:
    case ALL_DOUBLE_INDEXING_TYPES:
        return butterfly()->publicLength();
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        auto* storage = butterfly()->arrayStorage();
        unsigned usedVectorLength = std::min(storage->length(), storage->vectorLength());
        if (usedVectorLength)
            return true;
        SparseArrayValueMap* map = storage->m_sparseMap.get();
        if (!map)
            return false;
        return map->size();
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

ALWAYS_INLINE JSFinalObject* JSFinalObject::createDefaultEmptyObject(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    JSFinalObject* finalObject = new (NotNull, allocateCell<JSFinalObject>(vm, allocationSize(defaultInlineCapacity))) JSFinalObject(CreatingWellDefinedBuiltinCell, globalObject->objectStructureIDForObjectConstructor());
    finalObject->finishCreation(vm);
    ASSERT(globalObject->objectStructureForObjectConstructor()->id() == globalObject->objectStructureIDForObjectConstructor());
    ASSERT(globalObject->objectStructureForObjectConstructor()->inlineCapacity() == defaultInlineCapacity);
    return finalObject;
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
