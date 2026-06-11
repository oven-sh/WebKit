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

    JSPropertyNameEnumerator* cachedPropertyNameEnumerator() const;
    uintptr_t cachedPropertyNameEnumeratorAndFlag() const;
    void setCachedPropertyNameEnumerator(VM&, Structure*, JSPropertyNameEnumerator*, StructureChain*);
    void clearCachedPropertyNameEnumerator();

    // TSAN family structure-fields (AUD1.N4(3)): flag-on teardown/replacement
    // of the enumerator cache outside a stop-the-world window. Callers hold
    // the owning Structure's m_lock. The StructureChainInvalidationWatchpoint
    // FixedVector is MOVED to m_retiredCachedPropertyNameEnumeratorWatchpoints
    // instead of freed — the watchpoints stay reachable through watched
    // structures' transition watchpoint sets until the GC retires them in
    // finalizeUnconditionally (mutators stopped). Defined in
    // StructureRareData.cpp.
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
    // word with a relaxed atomic before taking its (thread-safe) ref.
    // Flag-off arms are bit-identical to the old plain code. Residual
    // (recorded for the triage doc): sharedPolyProtoWatchpoint() still
    // returns a const reference, so foreign call sites (LLIntSlowPaths.cpp,
    // InlineCacheCompiler.cpp — other slices' files) dereference the word
    // with a plain load; closing that pair needs a by-value accessor (a
    // flag-off codegen change at those call sites) or caller-side edits.
    Box<InlineWatchpointSet> copySharedPolyProtoWatchpoint() const
    {
        if (!Options::useJSThreads()) [[likely]]
            return m_polyProtoWatchpoint;
        return copySharedPolyProtoWatchpointConcurrently();
    }
    const Box<InlineWatchpointSet>& sharedPolyProtoWatchpoint() const { return m_polyProtoWatchpoint; }
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

    void finalizeUnconditionally(VM&, CollectionScope);

    static constexpr uintptr_t cachedPropertyNameEnumeratorIsValidatedViaTraversingFlag = 1;
    static constexpr uintptr_t cachedPropertyNameEnumeratorMask = ~static_cast<uintptr_t>(1);

    // Flag-off (single mutator inside the VM): exact, plain counter. Flag-on
    // (useJSThreads) the counter is ADVISORY ONLY: it is incremented under
    // Structure::m_lock at set creation, but the fire path never consults it
    // — Structure::firePropertyReplacementWatchpointSet instead rescans
    // m_replacementWatchpointSets under m_lock before clearing
    // isWatchingReplacement (see the T3-residual comment there). Do not add
    // flag-on callers that trust this counter.
    unsigned incrementActiveReplacementWatchpointSet()
    {
        return ++m_activeReplacementWatchpointSet;
    }

    unsigned decrementActiveReplacementWatchpointSet()
    {
        return --m_activeReplacementWatchpointSet;
    }

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
    // GC-retirement parking lot for replaced/cleared enumerator watchpoint
    // vectors (flag-on only; always empty flag-off). Drained in
    // finalizeUnconditionally. See retireCachedPropertyNameEnumeratorWatchpoints().
    Vector<FixedVector<StructureChainInvalidationWatchpoint>> m_retiredCachedPropertyNameEnumeratorWatchpoints;
    WriteBarrier<JSCellButterfly> m_cachedPropertyNames[numberOfCachedPropertyNames] { };

    typedef UncheckedKeyHashMap<PropertyOffset, RefPtr<WatchpointSet>, WTF::IntHash<PropertyOffset>, WTF::UnsignedWithZeroKeyHashTraits<PropertyOffset>> PropertyWatchpointMap;
#ifdef NDEBUG
    static_assert(sizeof(PropertyWatchpointMap) == sizeof(void*), "StructureRareData should remain small");
#endif

    PropertyWatchpointMap m_replacementWatchpointSets;
    std::unique_ptr<SpecialPropertyCache> m_specialPropertyCache;
    Box<InlineWatchpointSet> m_polyProtoWatchpoint;

    WriteBarrierStructureID m_previous;
    PropertyOffset m_maxOffset;
    PropertyOffset m_transitionOffset;
    unsigned m_activeReplacementWatchpointSet { 0 };
};

} // namespace JSC
