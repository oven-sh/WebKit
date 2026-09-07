/*
 * Copyright (C) 2013-2022 Apple Inc. All rights reserved.
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

#pragma once

#include "ClassInfo.h"
#include "JSCast.h"
#include "JSTypeInfo.h"
#include "Options.h"
#include "PropertyOffset.h"
#include "PropertySlot.h"
#include <wtf/Atomics.h>
#include <wtf/FixedVector.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class JSPropertyNameEnumerator;
class LLIntOffsetsExtractor;
class Structure;
class StructureChain;
class CachedSpecialPropertyAdaptiveStructureWatchpoint;
class CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint;
struct SpecialPropertyCache;
enum class CachedPropertyNamesKind : uint8_t {
    EnumerableStrings = 0,
    Strings,
    Symbols,
    StringsAndSymbols,
};
static constexpr unsigned numberOfCachedPropertyNames = 4;

enum class CachedSpecialPropertyKey : uint8_t {
    ToStringTag = 0,
    ToString,
    ValueOf,
    ToPrimitive,
    ToJSON,
};
static constexpr unsigned numberOfCachedSpecialPropertyKeys = 5;

class StructureRareData;
class StructureChainInvalidationWatchpoint;

// Enumerator-cache watchpoint vectors that foreign threads may still reach
// through watched Structures' transition watchpoint sets. Under useJSThreads a
// StructureRareData moves a replaced or cleared vector here instead of
// destroying it in place; the owning Heap destroys the contents with the world
// stopped (Heap::finalizeUnconditionalFinalizers) and at teardown. Flag-off the
// Heap never allocates one.
class RetiredStructureChainInvalidationWatchpoints {
    WTF_MAKE_TZONE_ALLOCATED(RetiredStructureChainInvalidationWatchpoints);
    WTF_MAKE_NONCOPYABLE(RetiredStructureChainInvalidationWatchpoints);
public:
    RetiredStructureChainInvalidationWatchpoints();
    ~RetiredStructureChainInvalidationWatchpoints();

    // Callable from any mutator; the caller holds its Structure's m_lock and
    // this lock is a leaf under it.
    void add(FixedVector<StructureChainInvalidationWatchpoint>&&);
    // World stopped: unlinks every retired watchpoint from its set.
    void destroyAll();

private:
    Lock m_lock;
    Vector<FixedVector<StructureChainInvalidationWatchpoint>> m_vectors WTF_GUARDED_BY_LOCK(m_lock);
};

class StructureRareData final : public JSCell {
public:
    typedef JSCell Base;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    template<typename CellType, SubspaceAccess>
    inline static GCClient::IsoSubspace* subspaceFor(VM&); // Defined in StructureRareDataInlines.h

    static StructureRareData* create(VM&, Structure*);

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    DECLARE_VISIT_CHILDREN;

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    Structure* previousID() const
    {
        // TSAN family structure-fields: read lock-free from Structure::
        // previousID() while clearPreviousID()/the allocateRareData refresh
        // loop write the word (flag-on). One relaxed atomic 32-bit load of
        // the StructureID word — identical codegen to m_previous.get()'s
        // single-read-to-local pattern (webkit.org/b/110854), now also
        // C++-defined against the concurrent writers.
        static_assert(sizeof(WriteBarrierStructureID) == sizeof(uint32_t));
        StructureID id = std::bit_cast<StructureID>(WTF::atomicLoad(reinterpret_cast<uint32_t*>(const_cast<WriteBarrierStructureID*>(&m_previous)), std::memory_order_relaxed));
        if (!id)
            return nullptr;
        return id.decode();
    }
    void setPreviousID(VM&, Structure*);
    void clearPreviousID();
    void clearPreviousIDConcurrently()
    {
        // Relaxed atomic twin of clearPreviousID() for the flag-on CAS path
        // in Structure::clearPreviousID (Structure.h): previousID() readers
        // are lock-free relative to this writer.
        WTF::atomicStore(reinterpret_cast<uint32_t*>(&m_previous), 0u, std::memory_order_relaxed);
    }

    JSValue cachedSpecialProperty(CachedSpecialPropertyKey) const;
    void cacheSpecialProperty(JSGlobalObject*, VM&, Structure* baseStructure, JSValue, CachedSpecialPropertyKey, const PropertySlot&);

    TriState cachedHasDefaultToPrimitiveFastAndNonObservable() const { return static_cast<TriState>(racyLoad(m_replacementCountAndToPrimitiveCache) >> toPrimitiveCacheShift); }
    void setCachedHasDefaultToPrimitiveFastAndNonObservable(TriState mode) { updatePackedWord(static_cast<unsigned>(mode) << toPrimitiveCacheShift, toPrimitiveCacheMask); }

    JSPropertyNameEnumerator* cachedPropertyNameEnumerator() const;
    uintptr_t cachedPropertyNameEnumeratorAndFlag() const;
    void setCachedPropertyNameEnumerator(VM&, Structure*, JSPropertyNameEnumerator*, StructureChain*);
    void clearCachedPropertyNameEnumerator();

    // useJSThreads only; callers hold the owning Structure's m_lock. The
    // watchpoint FixedVector is moved to the Heap's
    // RetiredStructureChainInvalidationWatchpoints instead of freed, because
    // foreign threads can still reach the watchpoints through watched
    // structures' transition watchpoint sets until the next world-stopped GC
    // end phase destroys them.
    void retireCachedPropertyNameEnumeratorWatchpoints();
    void clearCachedPropertyNameEnumeratorRetiringWatchpoints();

    JSCellButterfly* cachedPropertyNames(CachedPropertyNamesKind) const;
    JSCellButterfly* cachedPropertyNamesIgnoringSentinel(CachedPropertyNamesKind) const;
    JSCellButterfly* cachedPropertyNamesConcurrently(CachedPropertyNamesKind) const;
    void setCachedPropertyNames(VM&, CachedPropertyNamesKind, JSCellButterfly*);

    // TSAN family structure-fields (§10.9 item (4), "Box reassign" key):
    // flag-on the poly-proto Box's single pointer word is PUBLISH-ONCE — the
    // setter CAS-installs from null with winner-keeps (loser's Box is
    // deref'd locally; it was never published, so nothing a foreign reader
    // can hold is ever displaced or freed under it — no GC-deferred retire
    // needed once replacement cannot happen). The copy accessor loads the
    // word with an acquire load before taking its (thread-safe) ref: the
    // caller uses the set, which another thread may have just published.
    // sharedPolyProtoWatchpoint() returns by value for the same reason, so
    // no caller reads the word with a plain load. Flag off, that costs one
    // ref and one deref on the cold IC paths that call it.
    Box<InlineWatchpointSet> copySharedPolyProtoWatchpoint() const
    {
        if (!Options::useJSThreads()) [[likely]]
            return m_polyProtoWatchpoint;
        return copySharedPolyProtoWatchpointConcurrently();
    }
    Box<InlineWatchpointSet> sharedPolyProtoWatchpoint() const { return copySharedPolyProtoWatchpoint(); }
    void setSharedPolyProtoWatchpoint(Box<InlineWatchpointSet>&& sharedPolyProtoWatchpoint)
    {
        if (!Options::useJSThreads()) [[likely]] {
            m_polyProtoWatchpoint = WTF::move(sharedPolyProtoWatchpoint);
            return;
        }
        setSharedPolyProtoWatchpointConcurrently(WTF::move(sharedPolyProtoWatchpoint));
    }
    bool hasSharedPolyProtoWatchpoint() const { return !!sharedPolyProtoWatchpointWord(); }

    // Identity-only raw view of the poly-proto Box's pointer word, for
    // callers that need null/equality checks without taking a ref
    // (Structure::shouldConvertToPolyProto). Relaxed atomic single-word load
    // — identical codegen to the plain load it replaces, defined against the
    // flag-on CAS installer.
    uintptr_t sharedPolyProtoWatchpointWord() const
    {
        static_assert(sizeof(Box<InlineWatchpointSet>) == sizeof(uintptr_t));
        return WTF::atomicLoad(reinterpret_cast<uintptr_t*>(const_cast<Box<InlineWatchpointSet>*>(&m_polyProtoWatchpoint)), std::memory_order_relaxed);
    }

    JS_EXPORT_PRIVATE Box<InlineWatchpointSet> copySharedPolyProtoWatchpointConcurrently() const;
    JS_EXPORT_PRIVATE void setSharedPolyProtoWatchpointConcurrently(Box<InlineWatchpointSet>&&);

    static JSCellButterfly* cachedPropertyNamesSentinel() { return std::bit_cast<JSCellButterfly*>(static_cast<uintptr_t>(1)); }

    static constexpr ptrdiff_t offsetOfCachedPropertyNames(CachedPropertyNamesKind kind)
    {
        return OBJECT_OFFSETOF(StructureRareData, m_cachedPropertyNames) + sizeof(WriteBarrier<JSCellButterfly>) * static_cast<unsigned>(kind);
    }

    static constexpr ptrdiff_t offsetOfCachedPropertyNameEnumeratorAndFlag()
    {
        return OBJECT_OFFSETOF(StructureRareData, m_cachedPropertyNameEnumeratorAndFlag);
    }

    static constexpr ptrdiff_t offsetOfSpecialPropertyCache()
    {
        return OBJECT_OFFSETOF(StructureRareData, m_specialPropertyCache);
    }

    static constexpr ptrdiff_t offsetOfPrevious()
    {
        return OBJECT_OFFSETOF(StructureRareData, m_previous);
    }

    DECLARE_EXPORT_INFO;

    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope);

    static constexpr uintptr_t cachedPropertyNameEnumeratorIsValidatedViaTraversingFlag = 1;
    static constexpr uintptr_t cachedPropertyNameEnumeratorMask = ~static_cast<uintptr_t>(1);

    // Flag-off (single mutator inside the VM): exact, plain counter. Flag-on
    // (useJSThreads) the counter is ADVISORY ONLY: it is incremented under
    // Structure::m_lock at set creation, but the fire path never consults it
    // — Structure::firePropertyReplacementWatchpointSet instead rescans
    // m_replacementWatchpointSets under m_lock before clearing
    // isWatchingReplacement (see the T3-residual comment there). Do not add
    // flag-on callers that trust this counter.
    unsigned incrementActiveReplacementWatchpointSet() { return addToReplacementCount(1); }
    unsigned decrementActiveReplacementWatchpointSet() { return addToReplacementCount(-1); }

private:
    friend class LLIntOffsetsExtractor;
    friend class Structure;
    friend class CachedSpecialPropertyAdaptiveStructureWatchpoint;
    friend class CachedSpecialPropertyAdaptiveInferredPropertyValueWatchpoint;

    StructureRareData(VM&, Structure*);

    void clearCachedSpecialProperty(CachedSpecialPropertyKey);
    void cacheSpecialPropertySlow(JSGlobalObject*, VM&, Structure* baseStructure, JSValue, CachedSpecialPropertyKey, const PropertySlot&);

    SpecialPropertyCache& ensureSpecialPropertyCache();
    SpecialPropertyCache& ensureSpecialPropertyCacheSlow();
    bool canCacheSpecialProperty(CachedSpecialPropertyKey);
    void giveUpOnSpecialPropertyCache(CachedSpecialPropertyKey);

    bool tryCachePropertyNameEnumeratorViaWatchpoint(VM&, Structure*, StructureChain*);

    // TSAN family structure-fields (§8.9 clause 3, the 32 enumerator-cache
    // keys): this word is written under the owning Structure's m_lock by the
    // install path (StructureRareDataInlines.h setCachedPropertyNameEnumerator)
    // and by the clear paths, but read single-word LOCK-FREE by foreign fast
    // paths, the LLInt (loadp m_cachedPropertyNameEnumeratorAndFlag) and the
    // JIT; the plain C++ reads/writes in StructureRareDataInlines.h raced the
    // relaxed atomic clear in clearCachedPropertyNameEnumeratorRetiringWatchpoints.
    // The wrapper routes EVERY C++ access through relaxed atomics (identical
    // mov/ldr/str codegen) without touching the inlines header (another
    // slice's file): layout is a single uintptr_t word, so the LLInt/JIT
    // offset reads are unchanged.
    class RelaxedAtomicUintPtr {
    public:
        // §10.9 fixShape (1): construct through a relaxed atomic store too —
        // an NSDMI/default-member-init is a plain store that TSAN pairs
        // against the lock-free readers of recycled rare-data cells.
        // Identical single-store codegen.
        RelaxedAtomicUintPtr() { WTF::atomicStore(&m_word, static_cast<uintptr_t>(0), std::memory_order_relaxed); }
        operator uintptr_t() const { return WTF::atomicLoad(const_cast<uintptr_t*>(&m_word), std::memory_order_relaxed); }
        RelaxedAtomicUintPtr& operator=(uintptr_t value)
        {
            WTF::atomicStore(&m_word, value, std::memory_order_relaxed);
            return *this;
        }

    private:
        uintptr_t m_word;
    };
    static_assert(sizeof(RelaxedAtomicUintPtr) == sizeof(uintptr_t));

    // FIXME: We should have some story for clearing these property names caches in GC.
    // https://bugs.webkit.org/show_bug.cgi?id=192659
    RelaxedAtomicUintPtr m_cachedPropertyNameEnumeratorAndFlag;
    FixedVector<StructureChainInvalidationWatchpoint> m_cachedPropertyNameEnumeratorWatchpoints;
    WriteBarrier<JSCellButterfly> m_cachedPropertyNames[numberOfCachedPropertyNames] { };

    typedef UncheckedKeyHashMap<PropertyOffset, RefPtr<WatchpointSet>, WTF::IntHash<PropertyOffset>, WTF::UnsignedWithZeroKeyHashTraits<PropertyOffset>> PropertyWatchpointMap;
#ifdef NDEBUG
    static_assert(sizeof(PropertyWatchpointMap) == sizeof(void*), "StructureRareData should remain small");
#endif

    PropertyWatchpointMap m_replacementWatchpointSets;
    std::unique_ptr<SpecialPropertyCache> m_specialPropertyCache;
    // The pointer word is CAS-published (ensureSpecialPropertyCacheSlow) and
    // read lock-free by other mutators and compiler threads: read it as one
    // racy word (relaxed under TSAN) rather than through unique_ptr.
    SpecialPropertyCache* specialPropertyCachePointer() const { return racyLoad(*std::bit_cast<SpecialPropertyCache* const*>(&m_specialPropertyCache)); }
    Box<InlineWatchpointSet> m_polyProtoWatchpoint;

    WriteBarrierStructureID m_previous;
    PropertyOffset m_maxOffset;
    PropertyOffset m_transitionOffset;
    // Low 30 bits: active replacement watchpoint set count; high 2 bits: the
    // cached TriState above. One word (the class is size-capped); flag-on the
    // two writers run on different threads (the count under Structure::m_lock,
    // the cache from any toPrimitive), so updates are CAS read-modify-writes
    // then, plain otherwise.
    static constexpr unsigned toPrimitiveCacheShift = 30;
    static constexpr unsigned toPrimitiveCacheMask = 3u << toPrimitiveCacheShift;
    static constexpr unsigned replacementCountMask = ~toPrimitiveCacheMask;
    void updatePackedWord(unsigned bits, unsigned mask)
    {
        if (!Options::useJSThreads()) [[likely]] {
            m_replacementCountAndToPrimitiveCache = (m_replacementCountAndToPrimitiveCache & ~mask) | bits;
            return;
        }
        for (;;) {
            unsigned oldWord = WTF::atomicLoad(&m_replacementCountAndToPrimitiveCache, std::memory_order_relaxed);
            unsigned newWord = (oldWord & ~mask) | bits;
            if (oldWord == newWord || WTF::atomicCompareExchangeWeakRelaxed(&m_replacementCountAndToPrimitiveCache, oldWord, newWord))
                return;
        }
    }
    unsigned addToReplacementCount(int delta)
    {
        if (!Options::useJSThreads()) [[likely]] {
            unsigned count = ((m_replacementCountAndToPrimitiveCache & replacementCountMask) + delta) & replacementCountMask;
            m_replacementCountAndToPrimitiveCache = (m_replacementCountAndToPrimitiveCache & toPrimitiveCacheMask) | count;
            return count;
        }
        for (;;) {
            unsigned oldWord = WTF::atomicLoad(&m_replacementCountAndToPrimitiveCache, std::memory_order_relaxed);
            unsigned count = ((oldWord & replacementCountMask) + delta) & replacementCountMask;
            unsigned newWord = (oldWord & toPrimitiveCacheMask) | count;
            if (WTF::atomicCompareExchangeWeakRelaxed(&m_replacementCountAndToPrimitiveCache, oldWord, newWord))
                return count;
        }
    }
    unsigned m_replacementCountAndToPrimitiveCache { static_cast<unsigned>(TriState::Indeterminate) << toPrimitiveCacheShift };
};
#ifdef NDEBUG
static_assert(sizeof(StructureRareData) <= 96, "StructureRareData should remain small");
#endif

#if defined(NDEBUG) && CPU(ADDRESS64)
// Six 16-byte atoms; useJSThreads-only state belongs in Heap-owned side lists, not in every cell.
static_assert(sizeof(StructureRareData) <= 96, "StructureRareData should remain small");
#endif

} // namespace JSC
