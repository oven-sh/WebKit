/*
 * Copyright (C) 2008-2025 Apple Inc. All rights reserved.
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

#include "ClassInfo.h"
#include "Concurrency.h"
#include "ConcurrentButterfly.h"
#include "ConcurrentJSLock.h"
#include "IndexingType.h"
#include "JSCJSValue.h"
#include "JSCJSValueCell.h"
#include "JSCast.h"
#include "JSTypeInfo.h"
#include "PropertyName.h"
#include "PropertyNameArray.h"
#include "PropertyOffset.h"
#include "PutPropertySlot.h"
#include "StructureRareData.h"
#include "StructureTransitionTable.h"
#include "TypeInfoBlob.h"
#include "Watchpoint.h"
#include <type_traits>
#include <wtf/Atomics.h>
#include <wtf/CompactPointerTuple.h>
#include <wtf/CompactPtr.h>
#include <wtf/CompactRefPtr.h>

namespace WTF {

class UniquedStringImpl;

} // namespace WTF

namespace JSC {

class DeferGC;
class DeferredStructureTransitionWatchpointFire;
class LLIntOffsetsExtractor;
class PropertyNameArrayBuilder;
class PropertyNameArray;
class PropertyTable;
class StructureChain;
class StructureShape;
class JSString;
struct DumpContext;
struct HashTable;
struct HashTableValue;

namespace Integrity {
class Analyzer;
}

class DeferredStructureTransitionWatchpointFire final : public DeferredWatchpointFire {
    WTF_MAKE_NONCOPYABLE(DeferredStructureTransitionWatchpointFire);
public:
    DeferredStructureTransitionWatchpointFire(VM& vm, Structure* structure)
        : DeferredWatchpointFire()
        , m_vm(vm)
        , m_structure(structure)
    {
    }

    ~DeferredStructureTransitionWatchpointFire()
    {
        if (watchpointsToFire().state() == IsWatched)
            fireAllSlow();
    }

    const Structure* structure() const { return m_structure; }


private:
    JS_EXPORT_PRIVATE void fireAllSlow();

    VM& m_vm;
    const Structure* m_structure;
};

// The out-of-line property storage capacity to use when first allocating out-of-line
// storage. Note that all objects start out without having any out-of-line storage;
// this comes into play only on the first property store that exhausts inline storage.
static constexpr unsigned initialOutOfLineCapacity = 4;

// The factor by which to grow out-of-line storage when it is exhausted, after the
// initial allocation.
static constexpr unsigned outOfLineGrowthFactor = 2;

class PropertyTableEntry;
class CompactPropertyTableEntry {
public:
    CompactPropertyTableEntry()
        : m_data(nullptr, 0)
    {
    }

    CompactPropertyTableEntry(UniquedStringImpl* key, PropertyOffset offset, unsigned attributes)
        : m_data(key, ((offset << 8) | attributes))
    {
        #ifndef BUN_SKIP_FAILING_ASSERTIONS
        ASSERT(this->attributes() == attributes);
        #endif
        ASSERT(this->offset() == offset);
    }

    CompactPropertyTableEntry(const PropertyTableEntry&);

    UniquedStringImpl* key() const { return m_data.pointer(); }
    void setKey(UniquedStringImpl* key) { m_data.setPointer(key); }
    PropertyOffset offset() const { return m_data.type() >> 8; }
    void setOffset(PropertyOffset offset)
    {
        m_data.setType((m_data.type() & 0x00ffU) | (offset << 8));
        #ifndef BUN_SKIP_FAILING_ASSERTIONS
        ASSERT(this->offset() == offset);
        #endif
    }
    uint8_t attributes() const { return m_data.type(); }
    void setAttributes(uint8_t attributes)
    {
        m_data.setType((m_data.type() & 0xff00U) | attributes);
        #ifndef BUN_SKIP_FAILING_ASSERTIONS
        ASSERT(this->attributes() == attributes);
        #endif
    }

private:
    CompactPointerTuple<UniquedStringImpl*, uint16_t> m_data;
};

class PropertyTableEntry {
public:
    PropertyTableEntry() = default;

    PropertyTableEntry(UniquedStringImpl* key, PropertyOffset offset, unsigned attributes)
        : m_key(key)
        , m_offset(offset)
        , m_attributes(attributes)
    {
        #ifndef BUN_SKIP_FAILING_ASSERTIONS
        ASSERT(this->attributes() == attributes);
        #endif
    }

    PropertyTableEntry(const CompactPropertyTableEntry& entry)
        : m_key(entry.key())
        , m_offset(entry.offset())
        , m_attributes(entry.attributes())
    {
    }

    UniquedStringImpl* key() const { return m_key; }
    void setKey(UniquedStringImpl* key) { m_key = key; }
    PropertyOffset offset() const { return m_offset; }
    void setOffset(PropertyOffset offset) { m_offset = offset; }
    uint8_t attributes() const { return m_attributes; }
    void setAttributes(uint8_t attributes) { m_attributes = attributes; }

private:
    UniquedStringImpl* m_key { nullptr };
    PropertyOffset m_offset { 0 };
    uint8_t m_attributes { 0 };
};


inline CompactPropertyTableEntry::CompactPropertyTableEntry(const PropertyTableEntry& entry)
    : m_data(entry.key(), ((entry.offset() << 8) | entry.attributes()))
{
}

class StructureFireDetail final : public FireDetail {
public:
    inline StructureFireDetail(const Structure*); // Defined in StructureInlines.h.

    void dump(PrintStream& out) const final;

private:
    explicit StructureFireDetail(ClangVTableWorkaroundTag);

    const Structure* m_structure;
};

class Structure : public JSCell {
    static constexpr uint16_t shortInvalidOffset = std::numeric_limits<uint16_t>::max() - 1;
    static constexpr uint16_t useRareDataFlag = std::numeric_limits<uint16_t>::max();
public:
    friend class StructureTransitionTable;

    typedef JSCell Base;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    static constexpr uint8_t numberOfLowerTierPreciseCells = 0;

    static_assert(JSCell::atomSize >= MarkedBlock::atomSize);

    static constexpr int s_maxTransitionLength = 128;
    static constexpr int s_maxTransitionLengthForNonEvalPutById = 512;
    static constexpr int s_maxTransitionLengthForRemove = 4096; // Picked from benchmarking measurement.

    using SeenProperties = TinyBloomFilter<CompactPtr<UniquedStringImpl>::StorageType>;

    enum PolyProtoTag { PolyProto };
    // SPEC-objectmodel Task 3b (SPEC-vmstate §5.3/N5): ID-creating Structure
    // cell allocations are serialized by the process-global
    // SharedVMState::StructureAllocationLocker (SAL, heap rank 7a; no-op
    // unless Options::useStructureAllocationLock()). The 7-argument create
    // and createStructure take it INTERNALLY (StructureCreateInlines.h),
    // threading the locker's GCDeferralContext into allocateCell; the
    // previous-structure create below is bracketed by its callers in
    // Structure.cpp. The lock is non-recursive: never call these while
    // already holding the SAL.
    inline static Structure* create(VM&, JSGlobalObject*, JSValue prototype, const TypeInfo&, const ClassInfo*, IndexingType = NonArray, unsigned inlineCapacity = 0); // Defined in StructureCreateInlines.h; takes the SAL internally.
    static Structure* create(PolyProtoTag, VM&, JSGlobalObject*, JSObject* prototype, const TypeInfo&, const ClassInfo*, IndexingType = NonArray, unsigned inlineCapacity = 0); // Delegates; SAL taken by the delegate.

    ~Structure();
    
    template<typename CellType, SubspaceAccess>
    inline static GCClient::IsoSubspace* subspaceFor(VM&); // Defined in StructureInlines.h

    JS_EXPORT_PRIVATE static bool isValidPrototype(JSValue);

protected:
    inline void finishCreation(VM& vm, const Structure* previous, DeferredStructureTransitionWatchpointFire* deferred); // Defined in StructureInlines.h

    void finishCreation(VM& vm)
    {
        Base::finishCreation(vm);
        ASSERT(m_prototype.get().isEmpty() || isValidPrototype(m_prototype.get()));
    }

private:
    inline void finishCreation(VM&, CreatingEarlyCellTag); // Defined in StructureInlines.h

    void NODELETE validateFlags();

public:
    StructureID id() const { return StructureID::encode(this); }

    uint32_t typeInfoBlob() const { return m_blob.blob(); }

    bool isProxy() const
    {
        JSType type = m_blob.type();
        return type == GlobalProxyType || type == ProxyObjectType;
    }

    static void dumpStatistics();

    inline bool shouldDoCacheableDictionaryTransitionForAdd(PutPropertySlot::Context context)
    {
        int maxTransitionLength;
        if (context == PutPropertySlot::PutById)
            maxTransitionLength = s_maxTransitionLengthForNonEvalPutById;
        else
            maxTransitionLength = s_maxTransitionLength;
        return transitionCountEstimate() > maxTransitionLength;
    }

    inline bool shouldDoCacheableDictionaryTransitionForRemoveAndAttributeChange()
    {
        return transitionCountEstimate() > s_maxTransitionLengthForRemove || transitionCountHasOverflowed();
    }

    ALWAYS_INLINE bool transitionCountHasOverflowed() const
    {
        int transitionCount = 0;
        for (auto* structure = this; structure; structure = structure->previousID()) {
            if (++transitionCount > s_maxTransitionLength)
                return true;
        }

        return false;
    }

    Structure* trySingleTransition() { return m_transitionTable.trySingleTransition(); }

    // SPEC-objectmodel L6/I37 (Task 3c) — shared-Structure table protocol,
    // flag-on (Options::useJSThreads()):
    // (i) MUTATOR transition-table LOOKUPS hold the source's m_lock: the
    //     plain *ToExistingStructure entry points below route to their
    //     m_lock-holding *Concurrently variants, and the direct
    //     m_transitionTable.get sites in Structure.cpp take m_lock in place.
    //     Inserts already run under m_lock; every insert site additionally
    //     dual-checks StructureTransitionTable::getMatching under that lock
    //     and adopts a racing winner (no lost/duplicated transitions).
    // (ii) PropertyTable steal/clone/materialize
    //     (takePropertyTableOrCloneIfPinned, copyPropertyTableForPinning,
    //     materializePropertyTable) read published tables only under the
    //     SOURCE's m_lock; a stolen/fresh table is private (mutate lock-free)
    //     until its new Structure publishes; every table mutation of a
    //     PUBLISHED Structure (add/remove/attributeChange families) holds ITS
    //     m_lock — as today. O1: allocation under m_lock only under a
    //     pre-lock DeferGC (GCSafeConcurrentJSLocker or explicit DeferGC).
    // (iii) MUTATOR uncached table WALKS (Structure::get in
    //     StructureInlinesLight.h, isSealed/isFrozen,
    //     getPropertyNamesFromStructure, addOrReplacePropertyWithoutTransition's
    //     find) hold m_lock across the walk; forEachProperty snapshots the
    //     entries under m_lock and runs the caller's functor after releasing
    //     it, because the functor may re-enter this structure's lock.
    // Flag-off, all paths are today's code, bit-identical (I22).
    // Compiler-thread Concurrently readers are unchanged.
    JS_EXPORT_PRIVATE static Structure* addPropertyTransition(VM&, Structure*, PropertyName, unsigned attributes, PropertyOffset&);
    JS_EXPORT_PRIVATE static Structure* addNewPropertyTransition(VM&, Structure*, PropertyName, unsigned attributes, PropertyOffset&, PutPropertySlot::Context = PutPropertySlot::UnknownContext, DeferredStructureTransitionWatchpointFire* = nullptr);
    static Structure* addPropertyTransitionToExistingStructureConcurrently(Structure*, UniquedStringImpl* uid, unsigned attributes, PropertyOffset&);
    static Structure* addPropertyTransitionToExistingStructure(Structure*, PropertyName, unsigned attributes, PropertyOffset&);
    static Structure* removeNewPropertyTransition(VM&, Structure*, PropertyName, PropertyOffset&, DeferredStructureTransitionWatchpointFire* = nullptr);
    static Structure* removePropertyTransition(VM&, Structure*, PropertyName, PropertyOffset&, DeferredStructureTransitionWatchpointFire* = nullptr);
    // SPEC-objectmodel S6 L3/L4 (flag-on): the pinned dictionary table, for
    // the cacheable-dictionary delete staleness guard (deletePropertyNamed-
    // Concurrent, JSObject.cpp). Pinned tables are never cleared or replaced,
    // so the pointer is stable; pair it with PropertyTable::
    // concurrentEditCount() to detect in-place edits since a plan-time clone.
    PropertyTable* pinnedPropertyTableForConcurrentDelete() const
    {
        ASSERT(isPinnedPropertyTable());
        return m_propertyTableUnsafe.get();
    }
    // SCAN-CAS-PROBE-STRUCTURE-SEGV (OM I34/L6, annex-N6/M7 single-snapshot
    // discipline): hand the M7(c) lock-free reader (JSObject::
    // getOwnNonIndexPropertySlot) the pinned table pointer so it can EXTEND
    // the S6 L3/L4 seqlock across its own getDirect() value read — a pinned
    // (dictionary) structure's structureID is never re-tagged across in-place
    // add/delete/attribute edits, so the M7(c) `structureID()==structure`
    // recheck alone cannot detect them and admits a torn {offset, m_butterfly,
    // value} triple (CVE A5 mc-df-delete-reuse.CRASH.log; SCAN-RESULTS.md
    // residual #5, property-cas-delete-undefined-sentinel-u5.js). Non-pinned
    // structures publish a fresh structureID on every table edit, so the
    // structureID recheck already suffices and this returns null (no extra
    // constraint). Pinned tables are never cleared or replaced (S6), so the
    // pointer is stable for the duration of a stamp pair. Non-asserting twin
    // of pinnedPropertyTableForConcurrentDelete(): callers branch on null.
    PropertyTable* pinnedPropertyTableForConcurrentReadStamp() const
    {
        if (!isPinnedPropertyTable())
            return nullptr;
        // Callers dereference the table, which another thread can have built
        // just before it published this structure.
        return WTF::atomicLoad(const_cast<Structure*>(this)->m_propertyTableUnsafe.slot(), std::memory_order_acquire);
    }
    // Acquire-load the S6 L3/L4 edit-stamp for the M7(c) reader's
    // single-snapshot recheck (0 if non-pinned — see above). Declared here,
    // defined in StructureInlines.h (PropertyTable.h is not includable from
    // this header; same declare/define split as Structure::get(VM&, ...)).
    uint32_t pinnedTableConcurrentEditCountForRead() const;
    static Structure* removePropertyTransitionFromExistingStructure(Structure*, PropertyName, PropertyOffset&);
    static Structure* removePropertyTransitionFromExistingStructureConcurrently(Structure*, PropertyName, PropertyOffset&);
    static Structure* changePrototypeTransition(VM&, Structure*, JSValue prototype, DeferredStructureTransitionWatchpointFire&);
    static Structure* changeGlobalProxyTargetTransition(VM&, Structure*, JSGlobalObject*, DeferredStructureTransitionWatchpointFire&);
    JS_EXPORT_PRIVATE static Structure* attributeChangeTransition(VM&, Structure*, PropertyName, unsigned attributes, DeferredStructureTransitionWatchpointFire* = nullptr);
    static Structure* attributeChangeTransitionToExistingStructureConcurrently(Structure*, PropertyName, unsigned attributes, PropertyOffset&);
    JS_EXPORT_PRIVATE static Structure* attributeChangeTransitionToExistingStructure(Structure*, PropertyName, unsigned attributes, PropertyOffset&);
    JS_EXPORT_PRIVATE static Structure* toCacheableDictionaryTransition(VM&, Structure*, DeferredStructureTransitionWatchpointFire* = nullptr);
    static Structure* toUncacheableDictionaryTransition(VM&, Structure*, DeferredStructureTransitionWatchpointFire* = nullptr);
    JS_EXPORT_PRIVATE static Structure* sealTransition(VM&, Structure*, DeferredStructureTransitionWatchpointFire* = nullptr);
    JS_EXPORT_PRIVATE static Structure* freezeTransition(VM&, Structure*, DeferredStructureTransitionWatchpointFire* = nullptr);
    static Structure* preventExtensionsTransition(VM&, Structure*, DeferredStructureTransitionWatchpointFire* = nullptr);
    static Structure* nonPropertyTransition(VM&, Structure*, TransitionKind, DeferredStructureTransitionWatchpointFire*);
    static Structure* setBrandTransitionFromExistingStructureConcurrently(Structure*, UniquedStringImpl*);
    static Structure* setBrandTransition(VM&, Structure*, Symbol* brand, DeferredStructureTransitionWatchpointFire* = nullptr);
    JS_EXPORT_PRIVATE static Structure* becomePrototypeTransition(VM&, Structure*, DeferredStructureTransitionWatchpointFire* = nullptr);

    JS_EXPORT_PRIVATE bool isSealed(VM&);
    JS_EXPORT_PRIVATE bool isFrozen(VM&);
    bool isStructureExtensible() const { return !didPreventExtensions(); }

    JS_EXPORT_PRIVATE Structure* flattenDictionaryStructure(VM&, JSObject*);

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    // Versions that take a func will call it after making the change but while still holding
    // the lock. The callback is not called if there is no change being made, like if you call
    // removePropertyWithoutTransition() and the property is not found.
    template<typename Func>
    PropertyOffset addPropertyWithoutTransition(VM&, PropertyName, unsigned attributes, const Func&);
    template<typename Func>
    PropertyOffset removePropertyWithoutTransition(VM&, PropertyName, const Func&);
    template<typename Func>
    PropertyOffset attributeChangeWithoutTransition(VM&, PropertyName, unsigned attributes, const Func&);
    template<typename Func>
    auto addOrReplacePropertyWithoutTransition(VM&, PropertyName, unsigned attributes, const Func&) -> decltype(auto);
    void setPrototypeWithoutTransition(VM&, JSValue prototype);
        
    bool isDictionary() const { return dictionaryKind() != NoneDictionaryKind; }
    bool isUncacheableDictionary() const { return dictionaryKind() == UncachedDictionaryKind; }
    bool isCacheableDictionary() const { return dictionaryKind() == CachedDictionaryKind; }
  
    bool prototypeQueriesAreCacheable()
    {
        return !typeInfo().prohibitsPropertyCaching();
    }
    
    bool propertyAccessesAreCacheable()
    {
        return dictionaryKind() != UncachedDictionaryKind
            && prototypeQueriesAreCacheable()
            && !(typeInfo().getOwnPropertySlotIsImpure() && !typeInfo().newImpurePropertyFiresWatchpoints());
    }

    bool propertyAccessesAreCacheableForAbsence()
    {
        // FIXME: dictionaries cannot be cached for absence, so check for dictionaries here instead
        // of at all call sites.
        return !typeInfo().getOwnPropertySlotIsImpureForPropertyAbsence();
    }

    bool needImpurePropertyWatchpoint()
    {
        return propertyAccessesAreCacheable()
            && typeInfo().getOwnPropertySlotIsImpure()
            && typeInfo().newImpurePropertyFiresWatchpoints();
    }

    bool isImmutablePrototypeExoticObject()
    {
        return typeInfo().isImmutablePrototypeExoticObject();
    }

    // We use SlowPath in GetByStatus for structures that may get new impure properties later to prevent
    // DFG from inlining property accesses since structures don't transition when a new impure property appears.
    bool takesSlowPathInDFGForImpureProperty()
    {
        return typeInfo().getOwnPropertySlotIsImpure();
    }

    bool hasNonReifiedStaticProperties() const
    {
        return typeInfo().hasStaticPropertyTable() && !staticPropertiesReified();
    }

    bool isNonExtensibleOrHasNonConfigurableProperties() const
    {
        return didPreventExtensions() || hasNonConfigurableProperties();
    }

    bool hasAnyOfBitFieldFlags(unsigned flags) const
    {
        // V7: relaxed read paired with the DEFINE_BITFIELD CAS writers (same codegen as the plain load).
        return racyLoad(m_bitField) & flags;
    }

    // Type accessors.
    // m_outOfLineTypeFlags and m_classInfo are constructor-written words read
    // lock-free by foreign threads; reader and constructor sides pair through
    // the TSAN-build-only relaxed helpers below (plain load non-TSAN, so the
    // optimizer can still fold and CSE these hot loads).
    TypeInfo typeInfo() const { return m_blob.typeInfo(tsanRelaxedLoad(m_outOfLineTypeFlags)); }
    bool isObject() const { return typeInfo().isObject(); }
    const ClassInfo* classInfoForCells() const { return tsanRelaxedLoad(m_classInfo); }
    CellState typeInfoDefaultCellState() const { return m_blob.defaultCellState(); }
protected:
    // You probably want typeInfo().type()
    JSType type() { return JSCell::type(); }
    // You probably want classInfoForCell()
    const ClassInfo* classInfo() const = delete;
public:

    IndexingType indexingType() const { return m_blob.indexingModeIncludingHistory() & AllWritableArrayTypes; }
    IndexingType indexingMode() const  { return m_blob.indexingModeIncludingHistory() & AllArrayTypes; }
    Dependency fencedIndexingMode(IndexingType& indexingType)
    {
        Dependency dependency = m_blob.fencedIndexingModeIncludingHistory(indexingType);
        indexingType &= AllArrayTypes;
        return dependency;
    }
    IndexingType indexingModeIncludingHistory() const { return m_blob.indexingModeIncludingHistory(); }
        
    inline bool mayInterceptIndexedAccesses() const;
    
    inline bool holesMustForwardToPrototype(JSObject*) const;
        
    JSGlobalObject* realm() const LIFETIME_BOUND { return m_realm.get(); }
#if USE(BUN_JSC_ADDITIONS)
    // Compat alias: upstream renamed Structure::globalObject() -> realm() in 7d4583947a7b.
    JSGlobalObject* globalObject() const LIFETIME_BOUND { return m_realm.get(); }
#endif

    // NOTE: This method should only be called during the creation of structures, since the realm
    // of a structure is presumed to be immutable in a bunch of places.
    void setRealm(VM&, JSGlobalObject*);

    ALWAYS_INLINE bool hasMonoProto() const
    {
        return !m_prototype.get().isEmpty();
    }
    ALWAYS_INLINE bool hasPolyProto() const
    {
        return !hasMonoProto();
    }
    ALWAYS_INLINE JSValue storedPrototype() const
    {
        ASSERT(hasMonoProto());
        return m_prototype.get();
    }
    JSValue storedPrototype(const JSObject*) const;
    JSObject* storedPrototypeObject(const JSObject*) const;
    Structure* storedPrototypeStructure(const JSObject*) const;

    JSObject* storedPrototypeObject() const;
    Structure* storedPrototypeStructure() const;
    JSValue prototypeForLookup(JSGlobalObject*) const;
    JSValue prototypeForLookup(JSGlobalObject*, JSCell* base) const;
    StructureChain* prototypeChain(VM&, JSGlobalObject*, JSObject* base) const;
    DECLARE_VISIT_CHILDREN;
    
    // A Structure is cheap to mark during GC if doing so would only add a small and bounded amount
    // to our heap footprint. For example, if the structure refers to a global object that is not
    // yet marked, then as far as we know, the decision to mark this Structure would lead to a large
    // increase in footprint because no other object refers to that global object. This method
    // returns true if all user-controlled (and hence unbounded in size) objects referenced from the
    // Structure are already marked.
    template<typename Visitor> bool isCheapDuringGC(Visitor&);
    
    // Returns true if this structure is now marked.
    template<typename Visitor> bool markIfCheap(Visitor&);
    
    // TSAN family structure-fields (OM §5 / UG §K): flag-on, the
    // m_previousOrRareData slot is install-raced by allocateRareData()'s
    // fence+CAS publish (Structure.cpp) and by clearPreviousID()'s CAS below,
    // so every lock-free read of the slot must be an atomic load — a plain
    // load against those CASes is C++ UB. Relaxed is sufficient: the install
    // CAS is release-or-stronger and the rare-data cell's fields are
    // fence-published before the pointer; readers that dereference pair the
    // relaxed load with the existing dependent-load fence (consume-style, free
    // on every supported target). Codegen is the identical plain mov/ldr, so
    // flag-off behavior is unchanged.
    ALWAYS_INLINE JSCell* previousOrRareDataConcurrently() const
    {
#if TSAN_ENABLED
        // §8.9 wave 3: acquire (TSAN builds only) pairs with allocateRareData's
        // CAS publish of the freshly constructed StructureRareData, making the
        // rare-data constructor stores happen-before every dereference below —
        // TSAN cannot see the writer-side storeStoreFence that carries this
        // ordering for real hardware. Non-TSAN builds keep the relaxed load
        // (consume-style dependent load, identical codegen, I22-safe).
        return WTF::atomicLoad(const_cast<WriteBarrier<JSCell>&>(m_previousOrRareData).slot(), std::memory_order_acquire);
#else
        return racyLoad(*const_cast<WriteBarrier<JSCell>&>(m_previousOrRareData).slot());
#endif
    }

    bool hasRareData() const
    {
        return isRareData(previousOrRareDataConcurrently());
    }

    StructureRareData* rareData()
    {
        JSCell* cell = previousOrRareDataConcurrently();
        ASSERT(isRareData(cell));
        return static_cast<StructureRareData*>(cell);
    }

    StructureRareData* tryRareData()
    {
        JSCell* value = previousOrRareDataConcurrently();
        WTF::dependentLoadLoadFence();
        if (isRareData(value))
            return static_cast<StructureRareData*>(value);
        return nullptr;
    }

    const StructureRareData* rareData() const
    {
        JSCell* cell = previousOrRareDataConcurrently();
        ASSERT(isRareData(cell));
        return static_cast<const StructureRareData*>(cell);
    }

    const StructureRareData* rareDataConcurrently() const
    {
        JSCell* cell = previousOrRareDataConcurrently();
        WTF::dependentLoadLoadFence();
        if (isRareData(cell))
            return static_cast<StructureRareData*>(cell);
        return nullptr;
    }

    StructureRareData* ensureRareData(VM& vm)
    {
        if (!hasRareData())
            allocateRareData(vm);
        return rareData();
    }
    
    inline Structure* previousID() const; // Defined below
    inline bool transitivelyTransitionedFrom(Structure* structureToFind); // Defined below

    inline PropertyOffset maxOffset() const; // Defined below

    inline void setMaxOffset(VM&, PropertyOffset); // Defined below

    inline PropertyOffset transitionOffset() const; // Defined below

    inline void setTransitionOffset(VM&, PropertyOffset); // Defined below

    static unsigned outOfLineCapacity(PropertyOffset maxOffset)
    {
        unsigned outOfLineSize = Structure::outOfLineSize(maxOffset);

        // This algorithm completely determines the out-of-line property storage growth algorithm.
        // The JSObject code will only trigger a resize if the value returned by this algorithm
        // changed between the new and old structure. So, it's important to keep this simple because
        // it's on a fast path.
        
        if (!outOfLineSize)
            return 0;

        if (outOfLineSize <= initialOutOfLineCapacity)
            return initialOutOfLineCapacity;

        ASSERT(outOfLineSize > initialOutOfLineCapacity);
        static_assert(outOfLineGrowthFactor == 2);
        return roundUpToPowerOfTwo(outOfLineSize);
    }
    
    static unsigned outOfLineSize(PropertyOffset maxOffset)
    {
        return numberOfOutOfLineSlotsForMaxOffset(maxOffset);
    }

    unsigned outOfLineCapacity() const
    {
        return outOfLineCapacity(maxOffset());
    }
    unsigned outOfLineSize() const
    {
        return outOfLineSize(maxOffset());
    }
    // TSAN family structure-fields: m_inlineCapacity is written only during
    // construction but read by concurrent threads holding stale/recycled cell
    // references (OM GT: stale reads re-dispatch); the relaxed atomic load is
    // the blessed access (identical codegen to the plain byte load).
    bool hasInlineStorage() const
    {
        return !!inlineCapacity();
    }
    unsigned inlineCapacity() const
    {
        return racyLoad(m_inlineCapacity);
    }
    unsigned inlineSize() const
    {
        return std::min<unsigned>(maxOffset() + 1, inlineCapacity());
    }
    unsigned totalStorageCapacity() const
    {
        ASSERT(structure()->classInfoForCells() == info());
        return outOfLineCapacity() + inlineCapacity();
    }

    bool isValidOffset(PropertyOffset offset) const
    {
        return JSC::isValidOffset(offset)
            && offset <= maxOffset()
            && (offset < static_cast<PropertyOffset>(inlineCapacity()) || offset >= firstOutOfLineOffset);
    }

    bool hijacksIndexingHeader() const
    {
        return isTypedView(m_blob.type());
    }
    
    bool couldHaveIndexingHeader() const
    {
        return hasIndexedProperties(indexingType())
            || hijacksIndexingHeader();
    }
    
    bool hasIndexingHeader(const JSCell*) const;    
    bool masqueradesAsUndefined(JSGlobalObject* lexicalGlobalObject)
    {
        return typeInfo().masqueradesAsUndefined() && realm() == lexicalGlobalObject;
    }

    // SPEC-objectmodel L6(iii) (Task 3c): flag-on these route to
    // getConcurrently (m_lock-holding chain walk) — do NOT call them while
    // holding this structure's m_lock (under-lock code queries the
    // PropertyTable directly instead). Flag-off: today's lock-free walk.
    PropertyOffset get(VM&, PropertyName);
    PropertyOffset get(VM&, PropertyName, unsigned& attributes);

    inline bool canPerformFastPropertyEnumerationCommon() const; // Defined below

    inline bool canPerformFastPropertyEnumeration() const; // Defined below

    // This is a somewhat internalish method. It will call your functor while possibly holding the
    // Structure's lock. There is no guarantee whether the lock is held or not in any particular
    // call. So, you have to assume the worst. Also, the functor returns true if it wishes for you
    // to continue or false if it's done.
    template<typename Functor>
    void forEachPropertyConcurrently(const Functor&);

    template<typename Functor>
    void forEachProperty(VM&, const Functor&);

    IGNORE_RETURN_TYPE_WARNINGS_BEGIN
    inline PropertyOffset get(VM&, Concurrency, UniquedStringImpl* uid, unsigned& attributes); // Defined in StructureInlines.h
    IGNORE_RETURN_TYPE_WARNINGS_END

    IGNORE_RETURN_TYPE_WARNINGS_BEGIN
    inline PropertyOffset get(VM&, Concurrency, UniquedStringImpl* uid); // Defined in StructureInlines.h
    IGNORE_RETURN_TYPE_WARNINGS_END

    inline PropertyOffset getConcurrently(UniquedStringImpl* uid); // Defined in StructureInlines.h
    PropertyOffset getConcurrently(UniquedStringImpl* uid, unsigned& attributes);
    
    Vector<PropertyTableEntry> getPropertiesConcurrently();
    
    void setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(bool is__proto__)
    {
        setHasAnyKindOfGetterSetterProperties(true);
        if (!is__proto__)
            setHasReadOnlyOrGetterSetterPropertiesExcludingProto(true);
    }
    
    void setContainsReadOnlyProperties() { setHasReadOnlyOrGetterSetterPropertiesExcludingProto(true); }
    
    void setCachedPropertyNameEnumerator(VM&, JSPropertyNameEnumerator*, StructureChain*);
    JSPropertyNameEnumerator* NODELETE cachedPropertyNameEnumerator() const;
    uintptr_t NODELETE cachedPropertyNameEnumeratorAndFlag() const;
    bool NODELETE canCachePropertyNameEnumerator(VM&) const;
    bool NODELETE canAccessPropertiesQuicklyForEnumeration() const;

    inline JSCellButterfly* cachedPropertyNames(CachedPropertyNamesKind kind) const; // Defined in StructureInlines.h
    inline JSCellButterfly* cachedPropertyNamesIgnoringSentinel(CachedPropertyNamesKind kind) const; // Defined in StructureInlines.h
    void setCachedPropertyNames(VM&, CachedPropertyNamesKind, JSCellButterfly*);
    bool canCacheOwnPropertyNames() const
    {
        if (isDictionary())
            return false;
        if (hasIndexedProperties(indexingType()))
            return false;
        if (typeInfo().overridesAnyFormOfGetOwnPropertyNames())
            return false;
        return true;
    }

    void getPropertyNamesFromStructure(VM&, PropertyNameArrayBuilder&, DontEnumPropertiesMode);

    inline JSValue cachedSpecialProperty(CachedSpecialPropertyKey key); // Defined in StructureInlines.h
    void cacheSpecialProperty(JSGlobalObject*, VM&, JSValue, CachedSpecialPropertyKey, const PropertySlot&);

    inline JSString* defaultToPrimitiveFastAndNonObservable(VM&);

    static constexpr ptrdiff_t prototypeOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_prototype);
    }

    static constexpr ptrdiff_t realmOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_realm);
    }

    static constexpr ptrdiff_t classInfoOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_classInfo);
    }

    static constexpr ptrdiff_t outOfLineTypeFlagsOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_outOfLineTypeFlags);
    }

    static constexpr ptrdiff_t indexingModeIncludingHistoryOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_blob) + TypeInfoBlob::indexingModeIncludingHistoryOffset();
    }
    
    static constexpr ptrdiff_t propertyTableUnsafeOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_propertyTableUnsafe);
    }

    static constexpr ptrdiff_t inlineCapacityOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_inlineCapacity);
    }

    static constexpr ptrdiff_t previousOrRareDataOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_previousOrRareData);
    }

    static constexpr ptrdiff_t bitFieldOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_bitField);
    }

    static constexpr ptrdiff_t propertyHashOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_propertyHash);
    }

    static constexpr ptrdiff_t seenPropertiesOffset()
    {
        return OBJECT_OFFSETOF(Structure, m_seenProperties) + SeenProperties::offsetOfBits();
    }

    static Structure* createStructure(VM&); // Defined in StructureCreateInlines.h; takes the SAL internally (Task 3b).

    bool transitionWatchpointSetHasBeenInvalidated() const
    {
        return m_transitionWatchpointSet.hasBeenInvalidated();
    }
        
    bool transitionWatchpointSetIsStillValid() const
    {
        return m_transitionWatchpointSet.isStillValid();
    }
    
    bool dfgMayWatchIfPossible() const
    {
        // FIXME: We would like to not watch things that are unprofitable to watch, like
        // dictionaries. Unfortunately, we can't do such things: a dictionary could get flattened,
        // in which case it will start to appear watchable and so the DFG will think that it is
        // watching it. We should come up with a comprehensive story for not watching things that
        // aren't profitable to watch.
        // https://bugs.webkit.org/show_bug.cgi?id=133625
        
        // - We don't watch Structures that either decided not to be watched, or whose predecessors
        //   decided not to be watched. This happens when a transition is fired while being watched.
        if (transitionWatchpointIsLikelyToBeFired())
            return false;

        // - Don't watch Structures that had been dictionaries.
        if (hasBeenDictionary())
            return false;
        
        return true;
    }
    
    bool dfgMayWatch() const
    {
        return dfgMayWatchIfPossible() && transitionWatchpointSetIsStillValid();
    }

    bool propertyNameEnumeratorMayWatch() const
    {
        return dfgMayWatch() && !hasPolyProto();
    }
        
    // Flag-on this returns false when a foreign transition fired the set
    // between the caller's validity check and the link; see WatchpointSet::add.
    inline bool addTransitionWatchpoint(Watchpoint* watchpoint) const; // Defined in StructureInlinesLight.h
    
    void NODELETE didTransitionFromThisStructureWithoutFiringWatchpoint() const;
    void fireStructureTransitionWatchpoint(DeferredStructureTransitionWatchpointFire*) const;

    InlineWatchpointSet& transitionWatchpointSet() const
    {
        return m_transitionWatchpointSet;
    }

    // ===== SPEC-objectmodel §9.4 / §5 - TTL watchpoint sets, N1 transition TID,
    // E1-E4 elision predicates, I29 re-validation (frozen interface; Task 3) =====
    //
    // Semantics (§5, monotone, fired only inside a stop-the-world window - I13):
    //   - transitionThreadLocal: valid <=> no instance of this structure ever
    //     carried butterfly TID == notTTLTID (I11).
    //   - writeThreadLocal: valid <=> no instance ever had the SW bit set (I12).
    // Both start IsWatched for new structures flag-on; flag-off they are inert
    // (ClearWatchpoint, never consulted, never fired - I22).

    InlineWatchpointSet& transitionThreadLocalWatchpointSet() const { return m_transitionThreadLocalWatchpointSet; }
    InlineWatchpointSet& writeThreadLocalWatchpointSet() const { return m_writeThreadLocalWatchpointSet; }

    bool transitionThreadLocalIsStillValid() const { return m_transitionThreadLocalWatchpointSet.isStillValid(); }
    bool writeThreadLocalIsStillValid() const { return m_writeThreadLocalWatchpointSet.isStillValid(); }

    // §2.1 N1: the sole lock-free BUTTERFLY-LESS transitioner of this shape
    // while the TTL sets are valid (creator's TID; copied to transition
    // targets). Butterfly-BEARING ownership is keyed on the instance's tag
    // (E4); this TID plays no part there.
    // TSAN family structure-fields (§8.9): constructor-written, read by
    // foreign lock-free transitioners; TSAN-relaxed pair (plain non-TSAN).
    // The direct read in StructureInlines.h:transitionThreadLocalIsCurrent
    // stays with that header's owning slice.
    ButterflyTID transitionThreadLocalTID() const { return concurrentRelaxedLoad(m_transitionThreadLocalTID); }
    static constexpr ptrdiff_t transitionThreadLocalTIDOffset() { return OBJECT_OFFSETOF(Structure, m_transitionThreadLocalTID); } // jit §5.5 Transition (N2-LF leg).
    // TID rebias (Heap.cpp): a dead thread's TID is restamped to 0 under the
    // shared stop, and the TTL sets are fired in that same stop so no compiled
    // code survives with the old TID baked in. Mutators are stopped, but
    // compiler threads may still read the word, hence the relaxed atomic store.
    void restampTransitionThreadLocalTID(ButterflyTID tid) { concurrentRelaxedStore(m_transitionThreadLocalTID, tid); }

    // E1 (I14): fast paths may omit the TID != notTTLTID check iff this is true
    // and a watchpoint is installed on the set. M6: JIT-side state reads need no
    // fences - the sets change state only inside a stop.
    bool transitionThreadLocalIsValidAndWatched() const { return m_transitionThreadLocalWatchpointSet.state() == IsWatched; }
    // E2 (I14): write fast paths may omit the SW branch iff this is true and a
    // watchpoint is installed; writes always keep the fused TID compare (jit D9/CS5).
    bool writeThreadLocalIsValidAndWatched() const { return m_writeThreadLocalWatchpointSet.state() == IsWatched; }

    // E4 (r12, per-object keying) - may an owner transition from this structure
    // run today's lock-free code (no cell lock, no (D)CAS, today's nuke order)?
    // True iff BOTH source sets are valid+watched AND !isPreciseAllocation(cell)
    // (I36) AND: butterfly-bearing => tag == (currentButterflyTID(), SW=0)
    // (instance ownership; NO structure-TID compare - foreign-thread shape reuse
    // stays lock-free); butterfly-less (incl. N2) => currentButterflyTID() ==
    // transitionThreadLocalTID() (N1). Returns true flag-off (E3/I22: today's
    // code IS the lock-free path). Sound only with I29 (see below).
    // Defined in StructureInlines.h.
    bool mayTransitionLockFreeFromThisStructure(const JSCell*, uint64_t taggedButterflyWord) const;

    // I29 helper: E4 sites (all tiers) must allocate BEFORE final validation and
    // must not poll / allocate / cross a safepoint between the validation and
    // the StructureID store. Call this with FRESH re-reads immediately before
    // the nuke/StructureID store; false => ownership or elision was lost in the
    // window (or an allocation intervened) and the caller must fall back to the
    // §4.3 locked protocol. Pair with AssertNoGC across the
    // validation->StructureID-store window in debug builds.
    bool revalidateLockFreeTransition(const JSCell*, uint64_t freshTaggedButterflyWord) const;

    // §9.4 fire functions (F1-F3 triggers). RELEASE_ASSERT(butterflyWorldIsStopped(vm))
    // - they may only run inside a §10.6 stop-the-world window (I13). Bodies call
    // fireAll on the InlineWatchpointSets (jit §5.6 intercepts do the
    // invalidation/jettison/epoch/ISB work). fireTransitionThreadLocal also fires
    // writeThreadLocal (§5). Both apply the F4 chain-fire (previousID chain +
    // transition-table successors, same stop; monotone => sound).
    JS_EXPORT_PRIVATE void fireTransitionThreadLocal(VM&, const char* reason);
    JS_EXPORT_PRIVATE void fireWriteThreadLocal(VM&, const char* reason);

    WatchpointSet* ensurePropertyReplacementWatchpointSet(VM&, PropertyOffset);
    inline void startWatchingPropertyForReplacements(VM& vm, PropertyOffset offset); // Defined in StructureInlines.h
    void startWatchingPropertyForReplacements(VM&, PropertyName);
    WatchpointSet* propertyReplacementWatchpointSet(PropertyOffset);
    WatchpointSet* firePropertyReplacementWatchpointSet(VM&, PropertyOffset, const char* reason);

    void didReplaceProperty(PropertyOffset offset)
    {
        if (!isWatchingReplacement()) [[likely]]
            return;
        didReplacePropertySlow(offset);
    }
    void didCachePropertyReplacement(VM&, PropertyOffset);
    
    inline void startWatchingInternalPropertiesIfNecessary(VM& vm); // Defined in StructureInlines.h
    
    Ref<StructureShape> toStructureShape(JSValue, bool& sawPolyProtoStructure);
    
    void dump(PrintStream&) const;
    void dumpInContext(PrintStream&, DumpContext*) const;
    void dumpBrief(PrintStream&, const CString&) const;
    
    static void dumpContextHeader(PrintStream&);
    
    ConcurrentJSLock& lock() LIFETIME_BOUND { return m_lock; }

    // TSAN family structure-fields (§8.9 + r4 "Structure::add (3)"): read
    // lock-free by getConcurrently-side planners. The member type
    // (ConcurrentPropertyHashWord, defined below) routes this load AND the
    // lock-held updaters in StructureInlines.h (Structure::add/remove/
    // attributeChange) through relaxed atomics — both sides of the pair are
    // now defined, with no edit to that header (another slice's file).
    unsigned propertyHash() const { return m_propertyHash; }
    // SeenProperties copy construction already goes through TinyBloomFilter's
    // relaxed-atomic m_bits copy ctor (heap/TinyBloomFilter.h), so this
    // by-value read is TSAN-paired as-is.
    SeenProperties seenProperties() const { return m_seenProperties; }

    static bool shouldConvertToPolyProto(const Structure* a, const Structure* b);

    UniquedStringImpl* transitionPropertyName() const { return m_transitionPropertyName.get(); }

    struct PropertyHashEntry {
        const HashTable* table;
        const HashTableValue* value;
    };
    std::optional<PropertyHashEntry> findPropertyHashEntry(PropertyName) const;
    
    DECLARE_EXPORT_INFO;

private:
    JS_EXPORT_PRIVATE void didReplacePropertySlow(PropertyOffset);

    typedef enum {
        NoneDictionaryKind = 0,
        CachedDictionaryKind = 1,
        UncachedDictionaryKind = 2
    } DictionaryKind;

public:
    enum class DefinitelyNonThenableState : uint8_t {
        NotComputed = 0,
        NonThenable = 1, // Cached `true`. Sound only while the realm's promiseThenWatchpointSet is intact.
        MaybeThenable = 2, // Cached `false`. Always safe (a stale `false` only loses the optimization).
        Uncacheable = 3, // Prototype chain isn't covered by the watchpoint; always recompute.
    };

    // Flag-on slow path of DEFINE_BITFIELD's set##upperName. GIL-off, two
    // mutators can run the setters for DIFFERENT fields of the SAME shared
    // Structure concurrently (e.g. T1 setIsPinnedPropertyTable vs T2
    // setIsWatchingReplacement); a plain load/mask/store RMW lets the
    // interleaving T1-load, T2-load, T2-store, T1-store erase T2's bit — a
    // replacement watchpoint that never fires (wrong-value caches survive) or
    // a lost pin (the property table reclaimed under a dictionary). So the
    // setters CAS when Options::useJSThreads() is on. The loop is outlined
    // (StructureInlines.h) so flag-off transition paths carry only the
    // predicted-false byte test + a never-taken call, keeping the pre-threads
    // plain RMW. Readers are relaxed atomic loads unconditionally (identical
    // MOV/LDR codegen), so TSAN sees them paired with the CAS writers; flag-off
    // this mixes the plain writer RMW with relaxed-atomic readers on
    // m_bitField, which is the pre-threads baseline (concurrent JIT/GC readers
    // predate threads) and must not be "fixed" by a flag-off TSAN pass — TSAN
    // rungs run flag-on, where writers CAS.
    NEVER_INLINE void setBitFieldConcurrently(uint32_t setBits, uint32_t fieldBits);

    // TSAN family structure-fields (triage §8.9 fixShape (2); wave-3 review
    // amendment §9.1): concurrently-readable scalar members. A Structure
    // under construction occupies recycled IsoSubspace memory that foreign
    // threads may still probe through stale StructureIDs (blessed reader
    // side, OM GT — stale reads re-dispatch), and after the single-word
    // publish (cell-header StructureID store / transition-table insert)
    // readers load these words without this structure's m_lock; the real
    // ordering is the constructor-tail storeStoreFence (UG §K), which TSAN
    // cannot model.
    //
    // Two helper pairs, per the campaign convention:
    // - concurrentRelaxedLoad/Store: UNCONDITIONAL relaxed atomics, used by
    //   every post-construction accessor site. Single-word relaxed atomic is
    //   the identical mov flag-off; this removes the mixed
    //   atomic-reader/plain-writer pairs in production builds.
    // - tsanRelaxedLoad/Store: TSAN-build-only, used ONLY inside the
    //   constructor member-init bulk sequences (Structure.cpp), where
    //   unconditional atomics would defeat the bench-sensitive store
    //   coalescing (ITEM-2, V5b gate). The retained production-UB acceptance
    //   for that class is recorded in docs/threads/TSAN-TRIAGE.md §9.1.
    template<typename T>
    static ALWAYS_INLINE T concurrentRelaxedLoad(const T& field)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T result;
        __atomic_load(const_cast<T*>(&field), &result, __ATOMIC_RELAXED);
        return result;
    }

    template<typename T>
    static ALWAYS_INLINE void concurrentRelaxedStore(T& field, std::type_identity_t<T> value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        __atomic_store(&field, &value, __ATOMIC_RELAXED);
    }

    template<typename T>
    static ALWAYS_INLINE T tsanRelaxedLoad(const T& field)
    {
#if TSAN_ENABLED
        return concurrentRelaxedLoad(field);
#else
        return field;
#endif
    }

    template<typename T>
    static ALWAYS_INLINE void tsanRelaxedStore(T& field, std::type_identity_t<T> value)
    {
#if TSAN_ENABLED
        concurrentRelaxedStore(field, value);
#else
        field = value;
#endif
    }

    // TSAN family structure-fields (r4 residual "Structure::add (3)"): the
    // m_propertyHash updaters live in StructureInlines.h
    // (add/remove/attributeChange: `m_propertyHash = m_propertyHash ^ hash`,
    // serialized by m_lock) — another slice's file — while compiler-side
    // planners read the word lock-free via propertyHash(). Routing every
    // access through the member TYPE fixes the writer without touching that
    // header: the conversion operator is the relaxed load, operator=(uint32_t)
    // the relaxed store (identical mov/ldr/str codegen everywhere, so flag-off
    // is unchanged). RMW atomicity is NOT required — updaters hold m_lock;
    // only word-level definedness against the lock-free readers is. The
    // constructor bulk-init sites (Structure.cpp) still go through
    // tsanRelaxedStore, which non-TSAN performs the plain copy-assignment and
    // so preserves the ITEM-2 store-coalescing shape.
    class ConcurrentPropertyHashWord {
    public:
        ConcurrentPropertyHashWord() = default;
        constexpr ConcurrentPropertyHashWord(uint32_t value)
            : m_word(value)
        {
        }
        ALWAYS_INLINE operator uint32_t() const { return concurrentRelaxedLoad(m_word); }
        ALWAYS_INLINE ConcurrentPropertyHashWord& operator=(uint32_t value)
        {
            concurrentRelaxedStore(m_word, value);
            return *this;
        }

    private:
        uint32_t m_word;
    };
    static_assert(std::is_trivially_copyable_v<ConcurrentPropertyHashWord>);

#define DEFINE_BITFIELD(type, lowerName, upperName, width, offset) \
    static constexpr uint32_t s_##lowerName##Shift = offset;\
    static constexpr uint32_t s_##lowerName##Mask = ((1 << (width - 1)) | ((1 << (width - 1)) - 1));\
    static constexpr uint32_t s_##lowerName##Bits = s_##lowerName##Mask << s_##lowerName##Shift;\
    static constexpr uint32_t s_bitWidthOf##upperName = width;\
    type lowerName() const { return static_cast<type>((racyLoad(m_bitField) >> offset) & s_##lowerName##Mask); }\
    void set##upperName(type newValue) \
    {\
        uint32_t setBits = (static_cast<uint32_t>(newValue) & s_##lowerName##Mask) << offset;\
        if (Options::useJSThreads()) [[unlikely]] {\
            setBitFieldConcurrently(setBits, s_##lowerName##Mask << offset);\
            return;\
        }\
        /* Flag off, only the mutator writes, but compiler threads read the word. */\
        uint32_t oldBits = racyLoad(m_bitField);\
        racyStore(m_bitField, (oldBits & ~(s_##lowerName##Mask << offset)) | setBits);\
    }

    DEFINE_BITFIELD(DictionaryKind, dictionaryKind, DictionaryKind, 2, 0);
    DEFINE_BITFIELD(bool, isPinnedPropertyTable, IsPinnedPropertyTable, 1, 2);
    DEFINE_BITFIELD(bool, hasAnyKindOfGetterSetterProperties, HasAnyKindOfGetterSetterProperties, 1, 3);
    DEFINE_BITFIELD(bool, hasReadOnlyOrGetterSetterPropertiesExcludingProto, HasReadOnlyOrGetterSetterPropertiesExcludingProto, 1, 4);
    DEFINE_BITFIELD(bool, isQuickPropertyAccessAllowedForEnumeration, IsQuickPropertyAccessAllowedForEnumeration, 1, 5);
    DEFINE_BITFIELD(bool, hasNonEnumerableProperties, HasNonEnumerableProperties, 1, 6);
    DEFINE_BITFIELD(bool, hasSpecialProperties, HasSpecialProperties, 1, 7);
    DEFINE_BITFIELD(DefinitelyNonThenableState, definitelyNonThenableState, DefinitelyNonThenableState, 2, 8); // This flag can be flipped on the main thread at any timing.
    DEFINE_BITFIELD(TransitionKind, transitionKind, TransitionKind, 5, 13);
    DEFINE_BITFIELD(bool, isWatchingReplacement, IsWatchingReplacement, 1, 18); // This flag can be fliped on the main thread at any timing.
    DEFINE_BITFIELD(bool, mayBePrototype, MayBePrototype, 1, 19);
    DEFINE_BITFIELD(bool, didPreventExtensions, DidPreventExtensions, 1, 20);
    DEFINE_BITFIELD(bool, didTransition, DidTransition, 1, 21);
    DEFINE_BITFIELD(bool, staticPropertiesReified, StaticPropertiesReified, 1, 22);
    DEFINE_BITFIELD(bool, hasBeenFlattenedBefore, HasBeenFlattenedBefore, 1, 23);
    DEFINE_BITFIELD(bool, didWatchInternalProperties, DidWatchInternalProperties, 1, 24);
    DEFINE_BITFIELD(bool, transitionWatchpointIsLikelyToBeFired, TransitionWatchpointIsLikelyToBeFired, 1, 25);
    DEFINE_BITFIELD(bool, hasBeenDictionary, HasBeenDictionary, 1, 26);
    DEFINE_BITFIELD(bool, protectPropertyTableWhileTransitioning, ProtectPropertyTableWhileTransitioning, 1, 27);
    DEFINE_BITFIELD(bool, hasUnderscoreProtoPropertyExcludingOriginalProto, HasUnderscoreProtoPropertyExcludingOriginalProto, 1, 28);
    DEFINE_BITFIELD(bool, hasNonConfigurableProperties, HasNonConfigurableProperties, 1, 29);
    DEFINE_BITFIELD(bool, hasNonConfigurableReadOnlyOrGetterSetterProperties, HasNonConfigurableReadOnlyOrGetterSetterProperties, 1, 30);

    enum class StructureVariant : uint8_t {
        Normal,
        Branded,
        WebAssemblyGC,
    };

    // TSAN family structure-fields (§8.9/§9.1): constructor-written,
    // immutable afterwards, read by concurrent stale-reference holders;
    // unconditional relaxed load (identical byte mov). Pairs with the
    // ctor-class tsanRelaxedStore — retained ctor-side UB is doc'd in §9.1.
    StructureVariant variant() const { return concurrentRelaxedLoad(m_structureVariant); }
    bool isBrandedStructure() { return variant() == StructureVariant::Branded; }

    static_assert(s_bitWidthOfTransitionKind <= sizeof(TransitionKind) * 8);

    static bool bitFieldFlagsCantBeChangedWithoutTransition(unsigned flags)
    {
        return flags == (flags & (
            s_didPreventExtensionsBits
            | s_isQuickPropertyAccessAllowedForEnumerationBits
            | s_hasNonEnumerablePropertiesBits
            | s_hasSpecialPropertiesBits
            | s_hasAnyKindOfGetterSetterPropertiesBits
            | s_hasReadOnlyOrGetterSetterPropertiesExcludingProtoBits
            | s_hasUnderscoreProtoPropertyExcludingOriginalProtoBits
            | s_hasNonConfigurablePropertiesBits
            | s_hasNonConfigurableReadOnlyOrGetterSetterPropertiesBits
        ));
    }

    // TSAN family structure-fields (§8.9/§9.1): written in the constructors
    // and on freshly created (not yet published) transition targets, read
    // lock-free by concurrent transition lookups; unconditional relaxed
    // atomics at this accessor pair (identical single-byte codegen).
    TransitionPropertyAttributes transitionPropertyAttributes() const { return concurrentRelaxedLoad(m_transitionPropertyAttributes); }
    void setTransitionPropertyAttributes(TransitionPropertyAttributes transitionPropertyAttributes) { concurrentRelaxedStore(m_transitionPropertyAttributes, transitionPropertyAttributes); }

    int transitionCountEstimate() const
    {
        // Since the number of transitions is often the same as the last offset (except if there are deletes)
        // we keep the size of Structure down by not storing both.
        return numberOfSlotsForMaxOffset(maxOffset(), inlineCapacity());
    }

    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope);

protected:
    Structure(VM&, StructureVariant, Structure* previous); // Branded/Normal only
    Structure(VM&, StructureVariant, JSGlobalObject*, const TypeInfo&, const ClassInfo*); // WebAssemblyGC only

private:
    friend class LLIntOffsetsExtractor;

    JS_EXPORT_PRIVATE Structure(VM&, JSGlobalObject*, JSValue prototype, const TypeInfo&, const ClassInfo*, IndexingType, unsigned inlineCapacity);
    Structure(VM&, CreatingEarlyCellTag);

    // SPEC-objectmodel Task 3b: callers (the transition factories in
    // Structure.cpp, plus BrandedStructure::create's caller) hold the
    // SharedVMState::StructureAllocationLocker ACROSS this call — it does
    // not take the SAL itself (its body, in StructureInlines.h, cannot
    // thread the locker's GCDeferralContext; callers pre-arm a flag-gated
    // DeferGC instead, per §6 O1 / SPEC-heap L5). With the SAL active,
    // `deferred` must be non-null so finishCreation never fires the previous
    // structure's transition watchpoints inline under the lock (watchpoint
    // firing may take rank-6b CodeBlock/jit locks, which are OUTER to the
    // SAL and must never be acquired while holding it).
    static Structure* create(VM&, Structure*, DeferredStructureTransitionWatchpointFire*);

    static Structure* addPropertyTransitionToExistingStructureImpl(Structure*, UniquedStringImpl* uid, unsigned attributes, PropertyOffset&);
    ALWAYS_INLINE static Structure* attributeChangeTransitionToExistingStructureImpl(Structure*, PropertyName, unsigned attributes, PropertyOffset&);
    static Structure* removePropertyTransitionFromExistingStructureImpl(Structure*, PropertyName, unsigned attributes, PropertyOffset&);
    static Structure* setBrandTransitionFromExistingStructureImpl(Structure*, UniquedStringImpl*);

    JS_EXPORT_PRIVATE static Structure* nonPropertyTransitionSlow(VM&, Structure*, TransitionKind, DeferredStructureTransitionWatchpointFire*);

    // This function does the both didTransitionFromThisStructureWithoutFiringWatchpoint and fireStructureTransitionWatchpoint.
    void didTransitionFromThisStructure(DeferredStructureTransitionWatchpointFire*) const;

    // This will return the structure that has a usable property table, that property table,
    // and the list of structures that we visited before we got to it. If it returns a
    // non-null structure, it will also lock the structure that it returns; it is your job
    // to unlock it.
    bool findStructuresAndMapForMaterialization(Vector<Structure*, 8>& structures, Structure*& structure, PropertyTable*&) WTF_ACQUIRES_LOCK_IF(true, structure->m_lock);
    
    static Structure* toDictionaryTransition(VM&, Structure*, DictionaryKind, DeferredStructureTransitionWatchpointFire* = nullptr);

    enum class ShouldPin : bool { No, Yes };
    template<ShouldPin, typename Func>
    PropertyOffset add(VM&, PropertyName, unsigned attributes, const Func&);
    PropertyOffset add(VM&, PropertyName, unsigned attributes);
    template<ShouldPin, typename Func>
    PropertyOffset remove(VM&, PropertyName, const Func&);
    PropertyOffset remove(VM&, PropertyName);
    template<ShouldPin, typename Func>
    PropertyOffset attributeChange(VM&, PropertyName, unsigned attributes, const Func&);
    PropertyOffset attributeChange(VM&, PropertyName, unsigned attributes);

#if ASSERT_ENABLED
    JS_EXPORT_PRIVATE void checkConsistency();
#else
    ALWAYS_INLINE void checkConsistency() { }
#endif

    // This may grab the lock, or not. Do not call when holding the Structure's lock.
    PropertyTable* ensurePropertyTableIfNotEmpty(VM& vm)
    {
        if (PropertyTable* result = m_propertyTableUnsafe.get())
            return result;
        if (!previousID())
            return nullptr;
        return materializePropertyTable(vm);
    }
    
    // This may grab the lock, or not. Do not call when holding the Structure's lock.
    PropertyTable* ensurePropertyTable(VM& vm)
    {
        if (PropertyTable* result = m_propertyTableUnsafe.get())
            return result;
        return materializePropertyTable(vm);
    }
    
    PropertyTable* propertyTableOrNull() const
    {
        return m_propertyTableUnsafe.get();
    }
    
    // This will grab the lock. Do not call when holding the Structure's lock.
    JS_EXPORT_PRIVATE PropertyTable* materializePropertyTable(VM&, bool setPropertyTable = true);
    // Flag-on Structure::get helper (StructureInlinesLight.h): publish this
    // structure's table so mutator lookups stop walking the chain. No-op for
    // structures a transition is currently building from (their table is
    // deliberately absent) and off mutator threads.
    JS_EXPORT_PRIVATE void materializePropertyTableForMutatorLookup(VM&);
    
    void setPropertyTable(VM& vm, PropertyTable* table);
    
    // Both produce the table for |transition|, a not-yet-published successor
    // of this structure. Flag-on they also re-copy m_seenProperties and
    // m_propertyHash into |transition| under this structure's m_lock, so the
    // filter and hash agree with the table even when a pinned source is
    // being mutated in place by another thread.
    PropertyTable* takePropertyTableOrCloneIfPinned(VM&, Structure* transition);
    PropertyTable* copyPropertyTableForPinning(VM&, Structure* transition);

    void setPreviousID(VM&, Structure*);

    void clearPreviousID()
    {
        if (!Options::useJSThreads()) [[likely]] {
            if (hasRareData())
                rareData()->clearPreviousID();
            else
                m_previousOrRareData.clear();
            return;
        }
        // Flag-on: allocateRareData()'s idempotent-CAS install (Structure.cpp)
        // can land between the hasRareData() check and the clear; a plain
        // clear() here would then wipe the freshly installed rare data off the
        // slot (losing watchpoint sets / caches it already carries). CAS so we
        // only clear the exact previous pointer we observed; if rare data
        // appears, clear its previousID field instead. The slot stays
        // monotonic: Structure*/null -> StructureRareData*, never back.
        JSCell** slot = m_previousOrRareData.slot();
        while (true) {
            JSCell* cell = WTF::atomicLoad(slot, std::memory_order_relaxed);
            if (isRareData(cell)) {
                // Route through the relaxed-atomic clear: previousID() readers
                // (Structure::previousID -> StructureRareData::previousID) are
                // lock-free relative to this writer.
                static_cast<StructureRareData*>(cell)->clearPreviousIDConcurrently();
                return;
            }
            if (!cell)
                return;
            if (WTF::atomicCompareExchangeStrong(slot, cell, static_cast<JSCell*>(nullptr)) == cell)
                return;
        }
    }

    bool isValid(JSGlobalObject*, StructureChain* cachedPrototypeChain, JSObject* base) const;

    // You have to hold the structure lock to do these.
    // Keep them inlined function since they are used in the critical path of Dictionary JSObject modification.
    void pin(const AbstractLocker&, VM&, PropertyTable*);
    void pinForCaching(const AbstractLocker&, VM&, PropertyTable*);

    // Flag-on only. Called on an unpublished transition while holding
    // |source|'s m_lock, at the moment source's table is stolen or cloned:
    // a pinned source's filter and hash change under that lock on every
    // in-place add, so the transition constructor's unlocked copies may
    // predate entries the table now holds.
    void copySeenPropertiesAndPropertyHashFrom(const AbstractLocker&, const Structure& source)
    {
        m_seenProperties.add(source.m_seenProperties.bits());
        m_propertyHash = static_cast<uint32_t>(source.m_propertyHash);
    }

    // SPEC-objectmodel F3 (Task 3): called on the RESULT structure right after a
    // pin()/pinForCaching() during a transition, with the transition's SOURCE,
    // OUTSIDE any §6-ranked lock. If any input TTL set (source's or result's) is
    // invalid, fires both of the result's sets under a §10.6 stop. No-op
    // flag-off. (GT#8: the poly-proto create/materialize/removeTransition
    // helpers at Structure.cpp:415-480 and :598-670 are NOT F3 sites and stay
    // unwired.)
    void fireTTLWatchpointSetsAfterPinning(VM&, const Structure* source);

    // SPEC-objectmodel F3 "flatten-under-stop" (Task 3; review round 2):
    // flag-on, flattenDictionaryStructure rearranges out-of-line storage in
    // place, so it ALWAYS runs per-event under the §10.6 veneer (read-only
    // foreign sharing is undetectable - foreign reads fire no watchpoint and
    // never flip SW - so no "unshared" fast path is sound), with its scratch
    // storage pre-allocated outside the stop (O4; refit => RESTART loop).
    // flattenTriggerIsShared decides only whether F3 FIRES the TTL sets
    // inside that stop (owner-local objects keep their sets).
    bool flattenTriggerIsShared(JSObject*) const;
    Structure* flattenDictionaryStructureUnderStop(VM&, JSObject*);
    // Sizing of the shrunk flat butterfly the impl's shift leg
    // (JSObject::shiftButterflyAfterFlattening) publishes. Flag-on it is
    // computed and allocated outside the stop and re-derived inside it; any
    // difference is a refit (O4).
    struct FlattenShrinkPlan {
        bool needsShift { false };
        size_t preCapacity { 0 };
        size_t outOfLineCapacityAfter { 0 };
        bool hasIndexingHeader { false };
        size_t indexingPayloadSizeInBytes { 0 };
        friend bool operator==(const FlattenShrinkPlan&, const FlattenShrinkPlan&) = default;
    };
    FlattenShrinkPlan flattenShrinkPlan(JSObject*);
    // Returns nullptr (flag-on, outside a §10.6 stop only; defensive - all
    // flag-on callers route through flattenDictionaryStructureUnderStop);
    // nothing is mutated in that case. preallocatedShrunkButterfly is the
    // flag-on pre-allocation for the shift leg; flag-off passes null and the
    // shift leg allocates in place as before.
    Structure* flattenDictionaryStructureImpl(VM&, JSObject*, Vector<JSValue>& preallocatedValues, Butterfly* preallocatedShrunkButterfly);

    // F4 chain-fire body shared by the §9.4 fire functions; runs world-stopped.
    void fireThreadLocalSetsWithChainUnderStop(VM&, const char* reason, bool alsoFireTransitionThreadLocal);

    static bool isRareData(JSCell* cell)
    {
        return cell && cell->type() != StructureType;
    }

    JS_EXPORT_PRIVATE void allocateRareData(VM&);

    template<typename DetailsFunc>
    void checkOffsetConsistency(PropertyTable*, const DetailsFunc&) const;
    void checkOffsetConsistency() const;

    void startWatchingInternalProperties(VM&);

    inline void clearCachedPrototypeChain(); // Defined in StructureInlines.h

    // TSAN family structure-fields: m_cachedPrototypeChain is written
    // lock-free (prototypeChain in StructureInlines.h), nulled by the flag-on
    // clearCachedPrototypeChain above and by concurrent marking
    // (visitChildrenImpl) — every read that can race those writers goes
    // through this relaxed atomic load (identical codegen).
    StructureChain* cachedPrototypeChainConcurrently() const
    {
        return WTF::atomicLoad(m_cachedPrototypeChain.slot(), std::memory_order_relaxed);
    }

    bool NODELETE holesMustForwardToPrototypeSlow(JSObject*) const;

#if TSAN_ENABLED
    // TSAN family structure-fields (triage §10.9 fixShape (1)): members whose
    // TYPE wraps WTF::Atomic words — m_lock's byte, the InlineWatchpointSet
    // state words, the seen-properties Bloom word, the transition-table word
    // — initialize through their constructors, and the std::atomic
    // CONSTRUCTOR is a plain (non-atomic) store. TSAN pairs those init-list
    // stores against concurrent relaxed-atomic readers probing recycled /
    // just-published Structures (TinyBloomFilter::ruleOut from
    // getConcurrently, the m_lock CAS from
    // findStructuresAndMapForMaterialization, watchpoint state loads): the
    // r3 keys at Structure.cpp:438/:440/:442. tsanRelaxedStore cannot help
    // after the fact — the constructor's plain store is already in the TSAN
    // shadow history — so under TSAN we DEFER construction: the member is a
    // union (no constructor runs on the member storage), the wrapped value
    // is built in a LOCAL buffer and its object representation is copied
    // into place with relaxed __atomic stores. Call sites are unchanged via
    // the conversion operator + the method forwarders below. Non-TSAN builds
    // use the plain member type (ConcurrentCtorMember<T> = T): zero codegen
    // change, per the flag-off rule; the retained production-UB acceptance
    // for the plain ctor stores (ordering = constructor-tail
    // storeStoreFence + single-word publish, blessed stale reads) stays
    // recorded in docs/threads/TSAN-TRIAGE.md §9.1.
    template<typename T>
    class TsanDeferredCtorMember {
        WTF_MAKE_NONCOPYABLE(TsanDeferredCtorMember);
    public:
        template<typename... Args>
        ALWAYS_INLINE TsanDeferredCtorMember(Args&&... args)
        {
            union Local {
                Local() { }
                ~Local() { }
                T value;
            } local;
            new (&local.value) T(std::forward<Args>(args)...);
            if constexpr (sizeof(T) == 1)
                __atomic_store_n(reinterpret_cast<uint8_t*>(&m_storage.value), *reinterpret_cast<uint8_t*>(&local.value), __ATOMIC_RELAXED);
            else if constexpr (sizeof(T) == 2)
                __atomic_store_n(reinterpret_cast<uint16_t*>(&m_storage.value), *reinterpret_cast<uint16_t*>(&local.value), __ATOMIC_RELAXED);
            else if constexpr (sizeof(T) == 4)
                __atomic_store_n(reinterpret_cast<uint32_t*>(&m_storage.value), *reinterpret_cast<uint32_t*>(&local.value), __ATOMIC_RELAXED);
            else {
                static_assert(!(sizeof(T) % sizeof(uintptr_t)));
                static_assert(alignof(T) >= alignof(uintptr_t));
                for (size_t i = 0; i < sizeof(T) / sizeof(uintptr_t); ++i)
                    __atomic_store_n(reinterpret_cast<uintptr_t*>(&m_storage.value) + i, reinterpret_cast<uintptr_t*>(&local.value)[i], __ATOMIC_RELAXED);
            }
            local.value.~T();
        }
        ALWAYS_INLINE ~TsanDeferredCtorMember() { unwrap().~T(); }

        ALWAYS_INLINE T& unwrap() const { return m_storage.value; }
        ALWAYS_INLINE operator T&() const { return unwrap(); }
        // No operator& overload: WTF::Locker takes &lockable internally and
        // must get a wrapper*, and OBJECT_OFFSETOF uses __builtin_offsetof.
        // Pointer-form call sites use Structure::lock() (identical codegen).

        // Forwarders for the direct member-call sites (Structure.h/.cpp,
        // StructureInlines.h, StructureInlinesLight.h) so the non-TSAN call
        // syntax is unchanged. Each is a template, so only the names actually
        // called on a given T are instantiated.
        // Trailing decltype return type (not decltype(auto)): clang parses
        // late-parsed member bodies in declaration order, so enclosing-class
        // members textually above this nested class could not use a forwarder
        // whose return type is only deduced from its body. The object
        // expression goes through unwrapDependent<Args...>() so it stays
        // type-dependent on the forwarder's own template parameters: member
        // lookup of methodName is deferred to each call site (only the names
        // actually called on a given T are looked up, as with the original
        // decltype(auto) form).
        template<typename... Args>
        ALWAYS_INLINE T& unwrapDependent() const { return unwrap(); }
#define JSC_TSAN_DEFERRED_MEMBER_FORWARD(methodName) \
        template<typename... Args> ALWAYS_INLINE auto methodName(Args&&... args) const -> decltype(this->template unwrapDependent<Args...>().methodName(std::forward<Args>(args)...)) { return unwrap().methodName(std::forward<Args>(args)...); }
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(lock)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(unlock)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(ruleOut)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(add)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(bits)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(hasBeenInvalidated)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(isStillValid)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(isBeingWatched)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(state)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(startWatching)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(invalidate)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(fireAll)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(touch)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(trySingleTransition)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(tryGetSingleSlotConcurrently)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(get)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(getMatching)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(contains)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(size)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(forEachTransition)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(finalizeUnconditionally)
        JSC_TSAN_DEFERRED_MEMBER_FORWARD(reconcileWeakReferencesAtGCEnd)
#undef JSC_TSAN_DEFERRED_MEMBER_FORWARD

    private:
        union Storage {
            Storage() { }
            ~Storage() { }
            T value;
        };
        static_assert(sizeof(Storage) == sizeof(T));
        mutable Storage m_storage;
    };
    template<typename T> using ConcurrentCtorMember = TsanDeferredCtorMember<T>;
#else
    template<typename T> using ConcurrentCtorMember = T;
#endif

    // These need to be properly aligned at the beginning of the 'Structure'
    // part of the object.
    TypeInfoBlob m_blob;
    TypeInfo::OutOfLineTypeFlags m_outOfLineTypeFlags;

    uint8_t m_inlineCapacity;

    // §10.9 fixShape (1): Atomic-bearing member types route through
    // ConcurrentCtorMember so TSAN builds skip the plain constructor store on
    // the member storage (see TsanDeferredCtorMember above). Non-TSAN this
    // alias IS the plain type — declarations, layout and codegen unchanged.
    ConcurrentCtorMember<ConcurrentJSLock> m_lock;

    uint32_t m_bitField;
    // §8.9 wave 3: no NSDMIs on the scalar members below — an NSDMI is a plain
    // store racing concurrent stale-reference readers; every constructor
    // initializes them via tsanRelaxedStore instead (plain store non-TSAN,
    // same instruction count as the NSDMI it replaces).
    TransitionPropertyAttributes m_transitionPropertyAttributes;

    // FIXME: We should probably have a brandedStructureStructure/webAssemblyGCStructureStructure instead of this.
    StructureVariant m_structureVariant;

    uint16_t m_transitionOffset;
    uint16_t m_maxOffset;

    // SPEC-objectmodel §5/§2.1 N1: creator's ButterflyTID, copied to transition
    // targets; keys lock-free butterfly-less transitions while the TTL sets are
    // valid. Sits in what was padding between m_maxOffset and m_propertyHash.
    // No NSDMI (§8.9 wave 3, see above): constructors tsanRelaxedStore it.
    uint16_t m_transitionThreadLocalTID;

    // Relaxed-atomic word wrapper; see ConcurrentPropertyHashWord above
    // (closes the StructureInlines.h add/remove/attributeChange plain RMWs
    // against the lock-free propertyHash() readers). Layout-identical to the
    // uint32_t it replaces (JIT/LLInt offset reads unchanged).
    ConcurrentPropertyHashWord m_propertyHash;
    static_assert(sizeof(ConcurrentPropertyHashWord) == sizeof(uint32_t));
    ConcurrentCtorMember<SeenProperties> m_seenProperties;


    WriteBarrier<JSGlobalObject> m_realm;
    WriteBarrier<Unknown> m_prototype;
    mutable WriteBarrier<StructureChain> m_cachedPrototypeChain;

    WriteBarrier<JSCell> m_previousOrRareData;

    CompactRefPtr<UniquedStringImpl> m_transitionPropertyName;

    const ClassInfo* m_classInfo;

    ConcurrentCtorMember<StructureTransitionTable> m_transitionTable;

    // Should be accessed through ensurePropertyTable(). During GC, it may be set to 0 by another thread.
    // During a Heap Snapshot GC we avoid clearing the table so it is safe to use.
    WriteBarrier<PropertyTable> m_propertyTableUnsafe;

    mutable ConcurrentCtorMember<InlineWatchpointSet> m_transitionWatchpointSet;

    // SPEC-objectmodel §5 (frozen member set, Structure.h:1107 anchor): the two
    // TTL watchpoint sets. NSDMI ClearWatchpoint (inert, I22) so the flag-off
    // constructors pay only a constant store; flag-on each constructor's single
    // Options::useJSThreads() block startWatching()es them, which for a thin
    // ClearWatchpoint set yields the identical IsWatched encoding as
    // constructing with IsWatched. Fired only world-stopped (I13), via the
    // §9.4 fire functions above.
    mutable ConcurrentCtorMember<InlineWatchpointSet> m_transitionThreadLocalWatchpointSet { ClearWatchpoint };
    mutable ConcurrentCtorMember<InlineWatchpointSet> m_writeThreadLocalWatchpointSet { ClearWatchpoint };

    static_assert(firstOutOfLineOffset < 256);

    friend class VMInspector;
    friend class JSDollarVMHelper;
    friend class Integrity::Analyzer;
};

JS_EXPORT_PRIVATE void dumpTransitionKind(PrintStream&, TransitionKind);
MAKE_PRINT_ADAPTOR(TransitionKindDump, TransitionKind, dumpTransitionKind);

// Defined here rather than in JSCell.h because it needs Structure to be complete.
inline const ClassInfo* JSCell::classInfo() const
{
    // If the mutator is currently sweeping, then accessing the structure is not safe since the
    // structure may have been swept already (and we're probably being called from this object's
    // destructor). This can only be verified for the mutator thread since other threads might be
    // querying JSCells that are not being swept by the mutator.
    // validateIsNotSweeping() is out-of-line to avoid pulling vm() into this header.
    ASSERT(validateIsNotSweeping());
    return structure()->classInfoForCells();
}

inline bool JSCell::inherits(const ClassInfo* info) const
{
    return classInfo()->isSubClassOf(info);
}

template<typename Target>
inline bool JSCell::inherits() const
{
    return JSCastingHelpers::inherits<Target>(this);
}

inline Structure* Structure::previousID() const
{
    ASSERT(structure()->classInfoForCells() == info());
    // This is so written because it's used concurrently. We only load from m_previousOrRareData
    // once, via the relaxed atomic accessor (the slot is CAS-published by
    // allocateRareData/clearPreviousID flag-on; plain load would be UB).
    JSCell* cell = previousOrRareDataConcurrently();
    if (isRareData(cell))
        return static_cast<StructureRareData*>(cell)->previousID();
    return static_cast<Structure*>(cell);
}

inline bool Structure::transitivelyTransitionedFrom(Structure* structureToFind)
{
    for (Structure* current = this; current; current = current->previousID()) {
        if (current == structureToFind)
            return true;
    }
    return false;
}

// TSAN family structure-fields (OM I34): m_maxOffset/m_transitionOffset (and
// their rare-data overflow twins) are written under the structure's m_lock by
// the add/remove paths but read lock-free by getConcurrently-side planners and
// foreign mutators; a torn/plain-raced maxOffset breaks I34. All accesses go
// through relaxed WTF atomics — bitwise-identical codegen (plain mov/ldr/str),
// so flag-off behavior and the JIT's 16-bit field loads are unchanged. The
// rare-data overflow arm keeps its existing storeStoreFence publication
// (value-before-flag), now paired with atomic loads on the reader side.

inline PropertyOffset Structure::maxOffset() const
{
    uint16_t maxOffset = racyLoad(m_maxOffset);
    if (maxOffset == shortInvalidOffset)
        return invalidOffset;
    if (maxOffset == useRareDataFlag)
        return racyLoad(rareData()->m_maxOffset);
    return maxOffset;
}

inline void Structure::setMaxOffset(VM& vm, PropertyOffset offset)
{
    if (offset == invalidOffset)
        racyStore(m_maxOffset, shortInvalidOffset);
    else if (offset < useRareDataFlag && offset < shortInvalidOffset)
        racyStore(m_maxOffset, static_cast<uint16_t>(offset));
    else if (racyLoad(m_maxOffset) == useRareDataFlag)
        racyStore(rareData()->m_maxOffset, offset);
    else {
        racyStore(ensureRareData(vm)->m_maxOffset, offset);
        WTF::storeStoreFence();
        racyStore(m_maxOffset, useRareDataFlag);
    }
}

inline PropertyOffset Structure::transitionOffset() const
{
    uint16_t transitionOffset = racyLoad(m_transitionOffset);
    if (transitionOffset == shortInvalidOffset)
        return invalidOffset;
    if (transitionOffset == useRareDataFlag)
        return racyLoad(rareData()->m_transitionOffset);
    return transitionOffset;
}

inline void Structure::setTransitionOffset(VM& vm, PropertyOffset offset)
{
    if (offset == invalidOffset)
        racyStore(m_transitionOffset, shortInvalidOffset);
    else if (offset < useRareDataFlag && offset < shortInvalidOffset)
        racyStore(m_transitionOffset, static_cast<uint16_t>(offset));
    else if (racyLoad(m_transitionOffset) == useRareDataFlag)
        racyStore(rareData()->m_transitionOffset, offset);
    else {
        racyStore(ensureRareData(vm)->m_transitionOffset, offset);
        WTF::storeStoreFence();
        racyStore(m_transitionOffset, useRareDataFlag);
    }
}

inline bool Structure::canPerformFastPropertyEnumerationCommon() const
{
    if (typeInfo().overridesGetOwnPropertySlot())
        return false;
    if (typeInfo().overridesAnyFormOfGetOwnPropertyNames())
        return false;
    if (hasAnyKindOfGetterSetterProperties())
        return false;
    if (isUncacheableDictionary())
        return false;
    // Cannot perform fast [[Put]] to |target| if the property names of the |source| contain "__proto__".
    if (hasUnderscoreProtoPropertyExcludingOriginalProto())
        return false;
    return true;
}

inline bool Structure::canPerformFastPropertyEnumeration() const
{
    if (!canPerformFastPropertyEnumerationCommon())
        return false;
    // FIXME: Indexed properties can be handled.
    // https://bugs.webkit.org/show_bug.cgi?id=185358
    if (hasIndexedProperties(indexingType()))
        return false;
    return true;
}

} // namespace JSC
