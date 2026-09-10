/*
 * Copyright (C) 2023-2025 Apple Inc. All rights reserved.
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

#include "Structure.h"
#include <wtf/TZoneMalloc.h>
#include <atomic>
#include <memory>

namespace JSC {

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(MegamorphicCache);

class MegamorphicCache {
    WTF_MAKE_TZONE_ALLOCATED(MegamorphicCache);
    WTF_MAKE_NONCOPYABLE(MegamorphicCache);
public:
    static constexpr uint32_t loadCachePrimarySize = 2048;
    static constexpr uint32_t loadCacheSecondarySize = 512;
    static_assert(hasOneBitSet(loadCachePrimarySize), "size should be a power of two.");
    static_assert(hasOneBitSet(loadCacheSecondarySize), "size should be a power of two.");
    static constexpr uint32_t loadCachePrimaryMask = loadCachePrimarySize - 1;
    static constexpr uint32_t loadCacheSecondaryMask = loadCacheSecondarySize - 1;

    static constexpr uint32_t storeCachePrimarySize = 2048;
    static constexpr uint32_t storeCacheSecondarySize = 512;
    static_assert(hasOneBitSet(storeCachePrimarySize), "size should be a power of two.");
    static_assert(hasOneBitSet(storeCacheSecondarySize), "size should be a power of two.");
    static constexpr uint32_t storeCachePrimaryMask = storeCachePrimarySize - 1;
    static constexpr uint32_t storeCacheSecondaryMask = storeCacheSecondarySize - 1;

    static constexpr uint32_t hasCachePrimarySize = 512;
    static constexpr uint32_t hasCacheSecondarySize = 128;
    static_assert(hasOneBitSet(hasCachePrimarySize), "size should be a power of two.");
    static_assert(hasOneBitSet(hasCacheSecondarySize), "size should be a power of two.");
    static constexpr uint32_t hasCachePrimaryMask = hasCachePrimarySize - 1;
    static constexpr uint32_t hasCacheSecondaryMask = hasCacheSecondarySize - 1;

    static constexpr uint32_t getterCachePrimarySize = 256;
    static constexpr uint32_t getterCacheSecondarySize = 64;
    static_assert(hasOneBitSet(getterCachePrimarySize), "size should be a power of two.");
    static_assert(hasOneBitSet(getterCacheSecondarySize), "size should be a power of two.");
    static constexpr uint32_t getterCachePrimaryMask = getterCachePrimarySize - 1;
    static constexpr uint32_t getterCacheSecondaryMask = getterCacheSecondarySize - 1;

    static constexpr uint16_t invalidEpoch = 0;
    static constexpr PropertyOffset maxOffset = UINT16_MAX;

    struct LoadEntry {
        static constexpr ptrdiff_t offsetOfUid() { return OBJECT_OFFSETOF(LoadEntry, m_uid); }
        static constexpr ptrdiff_t offsetOfStructureID() { return OBJECT_OFFSETOF(LoadEntry, m_structureID); }
        static constexpr ptrdiff_t offsetOfEpoch() { return OBJECT_OFFSETOF(LoadEntry, m_epoch); }
        static constexpr ptrdiff_t offsetOfOffset() { return OBJECT_OFFSETOF(LoadEntry, m_offset); }
        static constexpr ptrdiff_t offsetOfHolder() { return OBJECT_OFFSETOF(LoadEntry, m_holder); }

        void initAsMiss(StructureID structureID, UniquedStringImpl* uid, uint16_t epoch)
        {
            m_uid = uid;
            m_structureID = structureID;
            m_epoch = epoch;
            m_offset = 0;
            m_holder = nullptr;
        }

        void initAsHit(StructureID structureID, UniquedStringImpl* uid, uint16_t epoch, JSCell* holder, uint16_t offset, bool ownProperty)
        {
            m_uid = uid;
            m_structureID = structureID;
            m_epoch = epoch;
            m_offset = offset;
            m_holder = (ownProperty) ? JSCell::seenMultipleCalleeObjects() : holder;
        }

        RefPtr<UniquedStringImpl> m_uid;
        StructureID m_structureID { };
        uint16_t m_epoch { invalidEpoch };
        uint16_t m_offset { 0 };
        JSCell* m_holder { nullptr };
    };

    struct StoreEntry {
        static constexpr ptrdiff_t offsetOfUid() { return OBJECT_OFFSETOF(StoreEntry, m_uid); }
        static constexpr ptrdiff_t offsetOfOldStructureID() { return OBJECT_OFFSETOF(StoreEntry, m_oldStructureID); }
        static constexpr ptrdiff_t offsetOfNewStructureID() { return OBJECT_OFFSETOF(StoreEntry, m_newStructureID); }
        static constexpr ptrdiff_t offsetOfEpoch() { return OBJECT_OFFSETOF(StoreEntry, m_epoch); }
        static constexpr ptrdiff_t offsetOfOffset() { return OBJECT_OFFSETOF(StoreEntry, m_offset); }
        static constexpr ptrdiff_t offsetOfReallocating() { return OBJECT_OFFSETOF(StoreEntry, m_reallocating); }

        void init(StructureID oldStructureID, StructureID newStructureID, UniquedStringImpl* uid, uint16_t epoch, uint16_t offset, bool reallocating)
        {
            m_uid = uid;
            m_oldStructureID = oldStructureID;
            m_newStructureID = newStructureID;
            m_epoch = epoch;
            m_offset = offset;
            m_reallocating = reallocating;
        }

        RefPtr<UniquedStringImpl> m_uid;
        StructureID m_oldStructureID { };
        StructureID m_newStructureID { };
        uint16_t m_epoch { invalidEpoch };
        uint16_t m_offset { 0 };
        uint8_t m_reallocating { 0 };
    };

    struct HasEntry {
        static constexpr ptrdiff_t offsetOfUid() { return OBJECT_OFFSETOF(HasEntry, m_uid); }
        static constexpr ptrdiff_t offsetOfStructureID() { return OBJECT_OFFSETOF(HasEntry, m_structureID); }
        static constexpr ptrdiff_t offsetOfEpoch() { return OBJECT_OFFSETOF(HasEntry, m_epoch); }
        static constexpr ptrdiff_t offsetOfResult() { return OBJECT_OFFSETOF(HasEntry, m_result); }

        void init(StructureID structureID, UniquedStringImpl* uid, uint16_t epoch, bool result)
        {
            m_uid = uid;
            m_structureID = structureID;
            m_epoch = epoch;
            m_result = !!result;
        }

        RefPtr<UniquedStringImpl> m_uid;
        StructureID m_structureID { };
        uint16_t m_epoch { invalidEpoch };
        uint16_t m_result { false };
    };

    using GetterEntry = LoadEntry;

    static constexpr ptrdiff_t offsetOfLoadCachePrimaryEntries() { return OBJECT_OFFSETOF(MegamorphicCache, m_loadCachePrimaryEntries); }
    static constexpr ptrdiff_t offsetOfLoadCacheSecondaryEntries() { return OBJECT_OFFSETOF(MegamorphicCache, m_loadCacheSecondaryEntries); }

    static constexpr ptrdiff_t offsetOfStoreCachePrimaryEntries() { return OBJECT_OFFSETOF(MegamorphicCache, m_storeCachePrimaryEntries); }
    static constexpr ptrdiff_t offsetOfStoreCacheSecondaryEntries() { return OBJECT_OFFSETOF(MegamorphicCache, m_storeCacheSecondaryEntries); }

    static constexpr ptrdiff_t offsetOfHasCachePrimaryEntries() { return OBJECT_OFFSETOF(MegamorphicCache, m_hasCachePrimaryEntries); }
    static constexpr ptrdiff_t offsetOfHasCacheSecondaryEntries() { return OBJECT_OFFSETOF(MegamorphicCache, m_hasCacheSecondaryEntries); }

    static constexpr ptrdiff_t offsetOfGetterCachePrimaryEntries() { return OBJECT_OFFSETOF(MegamorphicCache, m_getterCachePrimaryEntries); }
    static constexpr ptrdiff_t offsetOfGetterCacheSecondaryEntries() { return OBJECT_OFFSETOF(MegamorphicCache, m_getterCacheSecondaryEntries); }

    static constexpr ptrdiff_t offsetOfEpoch() { return OBJECT_OFFSETOF(MegamorphicCache, m_epoch); }

    MegamorphicCache() = default;

#if CPU(ADDRESS64)
    // Because Structure is allocated with 16-byte alignment, we should assume that StructureID's lower 4 bits are zeros.
    static constexpr unsigned structureIDHashShift1 = 4;
#else
    // With 32-bit addresses, all bits can be different. Thus we do not need to shift the first level.
    static constexpr unsigned structureIDHashShift1 = 0;
#endif
    static constexpr unsigned structureIDHashShift2 = structureIDHashShift1 + 11;
    static constexpr unsigned structureIDHashShift3 = structureIDHashShift1 + 9;

    static constexpr unsigned structureIDHashShift4 = structureIDHashShift1 + 11;
    static constexpr unsigned structureIDHashShift5 = structureIDHashShift1 + 9;

    static constexpr unsigned structureIDHashShift6 = structureIDHashShift1 + 9;
    static constexpr unsigned structureIDHashShift7 = structureIDHashShift1 + 7;

    ALWAYS_INLINE static uint32_t primaryHash(StructureID structureID, UniquedStringImpl* uid)
    {
        uint32_t sid = std::bit_cast<uint32_t>(structureID);
        return ((sid >> structureIDHashShift1) ^ (sid >> structureIDHashShift2)) + uid->hash();
    }

    ALWAYS_INLINE static uint32_t secondaryHash(StructureID structureID, UniquedStringImpl* uid)
    {
        uint32_t key = std::bit_cast<uint32_t>(structureID) + static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid));
        return key + (key >> structureIDHashShift3);
    }

    ALWAYS_INLINE static uint32_t storeCachePrimaryHash(StructureID structureID, UniquedStringImpl* uid)
    {
        uint32_t sid = std::bit_cast<uint32_t>(structureID);
        return ((sid >> structureIDHashShift1) ^ (sid >> structureIDHashShift4)) + uid->hash();
    }

    ALWAYS_INLINE static uint32_t storeCacheSecondaryHash(StructureID structureID, UniquedStringImpl* uid)
    {
        uint32_t key = std::bit_cast<uint32_t>(structureID) + static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid));
        return key + (key >> structureIDHashShift5);
    }

    ALWAYS_INLINE static uint32_t hasCachePrimaryHash(StructureID structureID, UniquedStringImpl* uid)
    {
        uint32_t sid = std::bit_cast<uint32_t>(structureID);
        return ((sid >> structureIDHashShift1) ^ (sid >> structureIDHashShift6)) + uid->hash();
    }

    ALWAYS_INLINE static uint32_t hasCacheSecondaryHash(StructureID structureID, UniquedStringImpl* uid)
    {
        uint32_t key = std::bit_cast<uint32_t>(structureID) + static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid));
        return key + (key >> structureIDHashShift7);
    }

    JS_EXPORT_PRIVATE void age(CollectionScope);

    // SPEC-jit history §37 (eighth round). In a GIL-off process the VM's cache
    // (this class as flag-off and GIL-on use it: one mutator at a time, r17)
    // cannot be shared - a fill is a multi-word entry write with a RefPtr
    // uid - so every JS thread owns one of these (VMLite::megamorphicCache,
    // created on the thread's first fill; VM::megamorphicCacheForFill) and only
    // its owner probes or fills it (G1). Invalidation is one process-wide
    // counter: what used to bump the VM cache's m_epoch bumps it, the JIT
    // probes compare an entry's 16-bit stamp with its low half, and a fill
    // stamps its entry with the value read BEFORE its lookup (beginFill(),
    // G3), so a mutation racing the lookup leaves a dead entry. Collection
    // end (world stopped) bumps it too and clears every thread's cache on a
    // Full collection. Flag-off / GIL-on: none of this is reached; one
    // predicted-false byte test on slow paths.
    ALWAYS_INLINE static bool usesPerThreadCaches()
    {
        return Options::useJSThreads() && g_jscConfig.gilOffProcess;
    }
    static uint32_t processEpoch() { return s_processEpoch.load(std::memory_order_acquire); }
    static const uint32_t* addressOfProcessEpoch() { return reinterpret_cast<const uint32_t*>(&s_processEpoch); } // JIT probes load its low 16 bits (little-endian).
    // Any thread. Skips values whose low half is invalidEpoch so a cleared
    // entry's stamp never equals the counter's low half.
    static void bumpProcessEpoch()
    {
        uint32_t value = s_processEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (static_cast<uint16_t>(value) == invalidEpoch) [[unlikely]]
            s_processEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    // Per-thread caches only, owner thread only, BEFORE the lookup whose
    // result the following init* call will cache (G3): adopts the process
    // counter's low half as the stamp for this fill, and clears the cache
    // first if the high half moved since the last fill (G4: a wrapped low half
    // must not revalidate old entries).
    void beginFill()
    {
        ASSERT(m_isThreadCache);
        uint32_t epoch = processEpoch();
        uint16_t high = static_cast<uint16_t>(epoch >> 16);
        if (high != m_seenProcessEpochHigh) [[unlikely]] {
            clearEntriesAndKeys();
            m_seenProcessEpochHigh = high;
        }
        m_epoch = static_cast<uint16_t>(epoch);
        if (m_epoch == invalidEpoch) [[unlikely]]
            m_epoch = static_cast<uint16_t>(processEpoch()); // a bump was skipping past it; take the next value (a stale stamp at worst).
    }

    static std::unique_ptr<MegamorphicCache> createForThread()
    {
        auto cache = makeUnique<MegamorphicCache>();
        cache->m_isThreadCache = true;
        cache->m_seenProcessEpochHigh = static_cast<uint16_t>(processEpoch() >> 16);
        return cache;
    }
    bool isThreadCache() const { return m_isThreadCache; }
    // World stopped (collection end) or owner thread: drop every entry and the
    // uid references, and zero the keys so no epoch value can match them.
    JS_EXPORT_PRIVATE void clearEntriesAndKeys();

    void initAsMiss(StructureID structureID, UniquedStringImpl* uid)
    {
        uint32_t primaryIndex = MegamorphicCache::primaryHash(structureID, uid) & loadCachePrimaryMask;
        auto& entry = m_loadCachePrimaryEntries[primaryIndex];
        if (entry.m_epoch == m_epoch) {
            uint32_t secondaryIndex = MegamorphicCache::secondaryHash(entry.m_structureID, entry.m_uid.get()) & loadCacheSecondaryMask;
            m_loadCacheSecondaryEntries[secondaryIndex] = WTF::move(entry);
        }
        m_loadCachePrimaryEntries[primaryIndex].initAsMiss(structureID, uid, m_epoch);
    }

    void initAsHit(StructureID structureID, UniquedStringImpl* uid, JSCell* holder, uint16_t offset, bool ownProperty)
    {
        uint32_t primaryIndex = MegamorphicCache::primaryHash(structureID, uid) & loadCachePrimaryMask;
        auto& entry = m_loadCachePrimaryEntries[primaryIndex];
        if (entry.m_epoch == m_epoch) {
            uint32_t secondaryIndex = MegamorphicCache::secondaryHash(entry.m_structureID, entry.m_uid.get()) & loadCacheSecondaryMask;
            m_loadCacheSecondaryEntries[secondaryIndex] = WTF::move(entry);
        }
        m_loadCachePrimaryEntries[primaryIndex].initAsHit(structureID, uid, m_epoch, holder, offset, ownProperty);
    }

    void initAsGetterHit(StructureID structureID, UniquedStringImpl* uid, JSCell* holder, uint16_t offset, bool ownProperty)
    {
        uint32_t primaryIndex = MegamorphicCache::primaryHash(structureID, uid) & getterCachePrimaryMask;
        auto& entry = m_getterCachePrimaryEntries[primaryIndex];
        if (entry.m_epoch == m_epoch) {
            uint32_t secondaryIndex = MegamorphicCache::secondaryHash(entry.m_structureID, entry.m_uid.get()) & getterCacheSecondaryMask;
            m_getterCacheSecondaryEntries[secondaryIndex] = WTF::move(entry);
        }
        m_getterCachePrimaryEntries[primaryIndex].initAsHit(structureID, uid, m_epoch, holder, offset, ownProperty);
    }

    void initAsTransition(StructureID oldStructureID, StructureID newStructureID, UniquedStringImpl* uid, uint16_t offset, bool reallocating)
    {
        // Flag-on the probe's transition arm is the claim-first form and refuses
        // PreciseAllocation, copy-on-write and ArrayStorage instances at runtime
        // (SPEC-jit §5.5); sources of those shapes are not worth an entry.
        if (Options::useJSThreads()) [[unlikely]] {
            IndexingType mode = oldStructureID.decode()->indexingMode();
            if (hasAnyArrayStorage(mode) || isCopyOnWrite(mode))
                return;
        }
        uint32_t primaryIndex = MegamorphicCache::storeCachePrimaryHash(oldStructureID, uid) & storeCachePrimaryMask;
        auto& entry = m_storeCachePrimaryEntries[primaryIndex];
        if (entry.m_epoch == m_epoch) {
            uint32_t secondaryIndex = MegamorphicCache::storeCacheSecondaryHash(entry.m_oldStructureID, entry.m_uid.get()) & storeCacheSecondaryMask;
            m_storeCacheSecondaryEntries[secondaryIndex] = WTF::move(entry);
        }
        m_storeCachePrimaryEntries[primaryIndex].init(oldStructureID, newStructureID, uid, m_epoch, offset, reallocating);
    }

    void initAsReplace(StructureID structureID, UniquedStringImpl* uid, uint16_t offset)
    {
        uint32_t primaryIndex = MegamorphicCache::storeCachePrimaryHash(structureID, uid) & storeCachePrimaryMask;
        auto& entry = m_storeCachePrimaryEntries[primaryIndex];
        if (entry.m_epoch == m_epoch) {
            uint32_t secondaryIndex = MegamorphicCache::storeCacheSecondaryHash(entry.m_oldStructureID, entry.m_uid.get()) & storeCacheSecondaryMask;
            m_storeCacheSecondaryEntries[secondaryIndex] = WTF::move(entry);
        }
        m_storeCachePrimaryEntries[primaryIndex].init(structureID, structureID, uid, m_epoch, offset, false);
    }

    void initAsHasHit(StructureID structureID, UniquedStringImpl* uid)
    {
        uint32_t primaryIndex = MegamorphicCache::hasCachePrimaryHash(structureID, uid) & hasCachePrimaryMask;
        auto& entry = m_hasCachePrimaryEntries[primaryIndex];
        if (entry.m_epoch == m_epoch) {
            uint32_t secondaryIndex = MegamorphicCache::hasCacheSecondaryHash(entry.m_structureID, entry.m_uid.get()) & hasCacheSecondaryMask;
            m_hasCacheSecondaryEntries[secondaryIndex] = WTF::move(entry);
        }
        m_hasCachePrimaryEntries[primaryIndex].init(structureID, uid, m_epoch, true);
    }

    void initAsHasMiss(StructureID structureID, UniquedStringImpl* uid)
    {
        uint32_t primaryIndex = MegamorphicCache::hasCachePrimaryHash(structureID, uid) & hasCachePrimaryMask;
        auto& entry = m_hasCachePrimaryEntries[primaryIndex];
        if (entry.m_epoch == m_epoch) {
            uint32_t secondaryIndex = MegamorphicCache::hasCacheSecondaryHash(entry.m_structureID, entry.m_uid.get()) & hasCacheSecondaryMask;
            m_hasCacheSecondaryEntries[secondaryIndex] = WTF::move(entry);
        }
        m_hasCachePrimaryEntries[primaryIndex].init(structureID, uid, m_epoch, false);
    }

    uint16_t epoch() const { return m_epoch; }

    void bumpEpoch()
    {
        ASSERT(!m_isThreadCache);
        if (usesPerThreadCaches()) [[unlikely]] {
            bumpProcessEpoch();
            return;
        }
        ++m_epoch;
        if (m_epoch == invalidEpoch) [[unlikely]]
            clearEntries();
    }

private:
    JS_EXPORT_PRIVATE void NODELETE clearEntries();

    std::array<LoadEntry, loadCachePrimarySize> m_loadCachePrimaryEntries { };
    std::array<LoadEntry, loadCacheSecondarySize> m_loadCacheSecondaryEntries { };
    std::array<StoreEntry, storeCachePrimarySize> m_storeCachePrimaryEntries { };
    std::array<StoreEntry, storeCacheSecondarySize> m_storeCacheSecondaryEntries { };
    std::array<HasEntry, hasCachePrimarySize> m_hasCachePrimaryEntries { };
    std::array<HasEntry, hasCacheSecondarySize> m_hasCacheSecondaryEntries { };
    std::array<GetterEntry, getterCachePrimarySize> m_getterCachePrimaryEntries { };
    std::array<GetterEntry, getterCacheSecondarySize> m_getterCacheSecondaryEntries { };
    uint16_t m_epoch { 1 };
    uint16_t m_seenProcessEpochHigh { 0 };
    bool m_isThreadCache { false };

    JS_EXPORT_PRIVATE static std::atomic<uint32_t> s_processEpoch;
};

} // namespace JSC
