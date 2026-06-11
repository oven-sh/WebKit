/*
 * Copyright (C) 2018-2019 Apple Inc. All rights reserved.
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

#include "Options.h"
#include "PropertyOffset.h"
#include "Structure.h"
#include <wtf/Atomics.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

enum class GetByIdMode : uint8_t {
    ProtoLoad = 0, // This must be zero to reuse the higher bits of the pointer as this ProtoLoad mode.
    Default = 1,
    Unset = 2,
    ArrayLength = 3,
};

// SPEC-jit §4.3 (Task 6): the frozen threaded form of a surviving LLInt metadata
// cache. An {id, offset} pair packed into ONE 8-byte-aligned word so that, under
// Options::useJSThreads(), writers publish/invalidate with a single relaxed
// 64-bit store and the LLInt fast path reads it with a single 64-bit load
// (compare the id half, use the offset half) — no torn pairs, no fences (F3).
// The repack is UNCONDITIONAL (D7); flag-off code keeps per-field accesses at
// the exact same offsets.
struct alignas(8) LLIntCachedIdAndOffset {
    StructureID structureID { };
    int32_t offset { 0 };

    uint64_t* word() { return reinterpret_cast_ptr<uint64_t*>(this); }

    static uint64_t encode(StructureID structureID, int32_t offset)
    {
        LLIntCachedIdAndOffset value;
        value.structureID = structureID;
        value.offset = offset;
        return std::bit_cast<uint64_t>(value);
    }

    // All-zero = invalid (ABA-safe: a zero structureID never matches a cell's).
    void clear()
    {
        WTF::atomicStore(word(), static_cast<uint64_t>(0), std::memory_order_relaxed);
    }

    // One-word publish, no lock; last-writer-wins (SPEC-jit §4.3).
    void setConcurrently(StructureID structureID, int32_t offset)
    {
        WTF::atomicStore(word(), encode(structureID, offset), std::memory_order_relaxed);
    }
};
static_assert(sizeof(LLIntCachedIdAndOffset) == 8);
static_assert(alignof(LLIntCachedIdAndOffset) == 8);
static_assert(OBJECT_OFFSETOF(LLIntCachedIdAndOffset, structureID) == 0);
static_assert(OBJECT_OFFSETOF(LLIntCachedIdAndOffset, offset) == 4);

// SPEC-jit §4.3 (I13): clears an adjacent, 8-byte-aligned {StructureID, int32_t/unsigned}
// pair (e.g. OpPutById::Metadata's {m_oldStructureID, m_offset} replace cache)
// with ONE all-zero 64-bit store, so concurrent single-load readers never see a
// valid id half with a mismatched offset half.
ALWAYS_INLINE void clearLLIntIdAndOffsetPairConcurrently(void* pairAddress)
{
    ASSERT(!(std::bit_cast<uintptr_t>(pairAddress) & 7));
    WTF::atomicStore(reinterpret_cast_ptr<uint64_t*>(pairAddress), static_cast<uint64_t>(0), std::memory_order_relaxed);
}

// Same, for the one-word publish of such a pair.
ALWAYS_INLINE void publishLLIntIdAndOffsetPairConcurrently(void* pairAddress, StructureID structureID, int32_t offset)
{
    ASSERT(!(std::bit_cast<uintptr_t>(pairAddress) & 7));
    WTF::atomicStore(reinterpret_cast_ptr<uint64_t*>(pairAddress), LLIntCachedIdAndOffset::encode(structureID, offset), std::memory_order_relaxed);
}

struct GetByIdModeMetadataDefault {
    StructureID structureID;
    PropertyOffset cachedOffset;
    unsigned padding1;
};
static_assert(sizeof(GetByIdModeMetadataDefault) == 12);

struct GetByIdModeMetadataUnset {
    StructureID structureID;
    unsigned padding1;
    unsigned padding2;
};
static_assert(sizeof(GetByIdModeMetadataUnset) == 12);

struct GetByIdModeMetadataArrayLength {
    unsigned padding1;
    unsigned padding2;
    unsigned padding3;
};
static_assert(sizeof(GetByIdModeMetadataArrayLength) == 12);

struct GetByIdModeMetadataProtoLoad {
    StructureID structureID;
    PropertyOffset cachedOffset;
    JSObject* cachedSlot;
};
#if CPU(LITTLE_ENDIAN) && CPU(ADDRESS64)
static_assert(sizeof(GetByIdModeMetadataProtoLoad) == 16);
#endif

// In 64bit Little endian architecture, this union shares ProtoLoad's JSObject* cachedSlot with "hitCountForLLIntCaching" and "mode".
// This is possible because these values must be zero if we use ProtoLoad mode.
#if CPU(LITTLE_ENDIAN) && CPU(ADDRESS64)
union GetByIdModeMetadata {
    GetByIdModeMetadata()
    {
        defaultMode.structureID = StructureID();
        defaultMode.cachedOffset = 0;
        defaultMode.padding1 = 0;
        mode = GetByIdMode::Default;
        hitCountForLLIntCaching = Options::prototypeHitCountForLLIntCaching();
    }

    void clearToDefaultModeWithoutCache();
    void setUnsetMode(Structure*);
    void setArrayLengthMode();
    void setProtoLoadMode(Structure*, PropertyOffset, JSObject*);

    // SPEC-jit §4.3 (Task 6): "word 1" = the first 8 bytes of this union, i.e.
    // Default mode's {structureID, cachedOffset} pair. Flag-on, ONLY Default
    // (publishes/invalidates word 1 as one 64-bit store) and ArrayLength (never
    // touches word 1; self-validates via the cell's indexing bits) are ever
    // observable (I18); ProtoLoad/Unset are disabled wholesale.
    uint64_t* defaultModeCacheWord() { return reinterpret_cast_ptr<uint64_t*>(this); }
    void setDefaultModeCacheConcurrently(StructureID, PropertyOffset);

    // TSAN ic-stubinfo residual (SPEC-jit §4.3, §5.7 racy-profiling tolerance):
    // a baseline JIT compiler thread snapshots this metadata while the owning
    // LLInt slow path rewrites it with the relaxed atomic stores above. The
    // compiler only consumes the mode byte (a profiling hint picking the
    // initial CacheType), so the blessed snapshot is ONE relaxed 1-byte load —
    // never a plain whole-struct copy. A stale mode is harmless: it can only
    // pick a different initial cache shape for the new code. Codegen is
    // identical to a plain byte load, so flag-off behavior is unchanged.
    GetByIdMode loadModeConcurrently() { return WTF::atomicLoad(&mode, std::memory_order_relaxed); }

    struct {
        uint32_t padding1;
        uint32_t padding2;
        uint32_t padding3;
        uint16_t padding4;
        GetByIdMode mode;
        uint8_t hitCountForLLIntCaching; // This must be zero when we use ProtoLoad mode.
    };
    static constexpr ptrdiff_t offsetOfMode() { return OBJECT_OFFSETOF(GetByIdModeMetadata, mode); }
    GetByIdModeMetadataDefault defaultMode;
    GetByIdModeMetadataUnset unsetMode;
    GetByIdModeMetadataArrayLength arrayLengthMode;
    GetByIdModeMetadataProtoLoad protoLoadMode;
};
static_assert(sizeof(GetByIdModeMetadata) == 16);
// SPEC-jit §4.3/I13: word 1 must be one aligned u64 = {structureID, cachedOffset}.
static_assert(alignof(GetByIdModeMetadata) == 8);
static_assert(OBJECT_OFFSETOF(GetByIdModeMetadata, defaultMode.structureID) == 0);
static_assert(OBJECT_OFFSETOF(GetByIdModeMetadata, defaultMode.cachedOffset) == 4);
#else
struct GetByIdModeMetadata {
    GetByIdModeMetadata()
    {
        defaultMode.structureID = StructureID();
        defaultMode.cachedOffset = 0;
        defaultMode.padding1 = 0;
        mode = GetByIdMode::Default;
        hitCountForLLIntCaching = Options::prototypeHitCountForLLIntCaching();
    }

    void clearToDefaultModeWithoutCache();
    void setUnsetMode(Structure*);
    void setArrayLengthMode();
    void setProtoLoadMode(Structure*, PropertyOffset, JSObject*);

    union {
        GetByIdModeMetadataDefault defaultMode;
        GetByIdModeMetadataUnset unsetMode;
        GetByIdModeMetadataArrayLength arrayLengthMode;
        GetByIdModeMetadataProtoLoad protoLoadMode;
    };
    GetByIdMode mode;
    static constexpr ptrdiff_t offsetOfMode() { return OBJECT_OFFSETOF(GetByIdModeMetadata, mode); }
    uint8_t hitCountForLLIntCaching;

    // See the LE64 variant: relaxed 1-byte snapshot for compiler-thread readers.
    GetByIdMode loadModeConcurrently() { return WTF::atomicLoad(&mode, std::memory_order_relaxed); }
};
#endif

inline void GetByIdModeMetadata::clearToDefaultModeWithoutCache()
{
#if CPU(LITTLE_ENDIAN) && CPU(ADDRESS64)
    if (Options::useJSThreads()) [[unlikely]] {
        // SPEC-jit §4.3: invalidate word 1 with ONE all-zero 64-bit store (F3);
        // mode-byte writes are 1-byte relaxed stores. A racing reader either
        // sees the old, internally-coherent word (stale-but-sound: the id half
        // still validates against the actual cell) or all-zero (guaranteed miss).
        WTF::atomicStore(defaultModeCacheWord(), static_cast<uint64_t>(0), std::memory_order_relaxed);
        WTF::atomicStore(&mode, GetByIdMode::Default, std::memory_order_relaxed);
        return;
    }
#endif
    mode = GetByIdMode::Default;
    defaultMode.structureID = StructureID();
    defaultMode.cachedOffset = 0;
}

inline void GetByIdModeMetadata::setUnsetMode(Structure* structure)
{
    // SPEC-jit §4.3/I18: Unset mode is poison under JS threads (the asm reads
    // the mode byte and word 1 non-coherently); flag-on this must be unreachable.
    ASSERT(!Options::useJSThreads());
    mode = GetByIdMode::Unset;
    unsetMode.structureID = structure->id();
    defaultMode.cachedOffset = 0;
}

inline void GetByIdModeMetadata::setArrayLengthMode()
{
#if CPU(LITTLE_ENDIAN) && CPU(ADDRESS64)
    if (Options::useJSThreads()) [[unlikely]] {
        // SPEC-jit §4.3: ArrayLength never reads word 1 and self-validates via
        // the cell's indexing byte, so it may publish flag-on. Clear word 1
        // first (one store), then flip the mode byte; a reader seeing the stale
        // Default mode byte reads a coherent (possibly all-zero) word 1.
        WTF::atomicStore(defaultModeCacheWord(), static_cast<uint64_t>(0), std::memory_order_relaxed);
        WTF::atomicStore(&mode, GetByIdMode::ArrayLength, std::memory_order_relaxed);
        // Prevent the prototype cache from ever happening.
        WTF::atomicStore(&hitCountForLLIntCaching, static_cast<uint8_t>(0), std::memory_order_relaxed);
        return;
    }
#endif
    mode = GetByIdMode::ArrayLength;
    // We should clear the structure ID to avoid the old structure ID being saved.
    defaultMode.structureID = StructureID();
    defaultMode.cachedOffset = 0;
    // Prevent the prototype cache from ever happening.
    hitCountForLLIntCaching = 0;
}

#if CPU(LITTLE_ENDIAN) && CPU(ADDRESS64)
inline void GetByIdModeMetadata::setDefaultModeCacheConcurrently(StructureID structureID, PropertyOffset offset)
{
    // SPEC-jit §4.3: one-word publish of Default mode's {structureID, cachedOffset},
    // no lock (last-writer-wins). Caller must have set mode = Default already
    // (clearToDefaultModeWithoutCache).
    ASSERT(Options::useJSThreads());
    WTF::atomicStore(defaultModeCacheWord(), LLIntCachedIdAndOffset::encode(structureID, static_cast<int32_t>(offset)), std::memory_order_relaxed);
}
#endif

inline void GetByIdModeMetadata::setProtoLoadMode(Structure* structure, PropertyOffset offset, JSObject* cachedSlot)
{
    // SPEC-jit §4.3/I18: ProtoLoad's 16-byte record cannot be published as one
    // word; flag-on its sole installer (setupGetByIdPrototypeCache) is disabled
    // wholesale, so this must be unreachable.
    ASSERT(!Options::useJSThreads());
#if CPU(LITTLE_ENDIAN) && CPU(ADDRESS64)
    // We rely on ProtoLoad being 0, or else the high bits of the pointer would write the wrong mode and hit count
    static_assert(!static_cast<std::underlying_type_t<GetByIdMode>>(GetByIdMode::ProtoLoad)); // In 64bit architecture, this field is shared with protoLoadMode.cachedSlot.
#else
    mode = GetByIdMode::ProtoLoad;
#endif

    protoLoadMode.structureID = structure->id();
    protoLoadMode.cachedOffset = offset;

    // We know that this pointer will remain valid because it will be cleared by either a watchpoint fire or
    // during GC when we clear the LLInt caches.

    // The write to cachedSlot also writes the mode, since they overlap in the struct layout. We know that
    // the mode ProtoLoad is 0 by the static assertion above.
    protoLoadMode.cachedSlot = cachedSlot;

    ASSERT(mode == GetByIdMode::ProtoLoad);
    ASSERT(!hitCountForLLIntCaching);
    ASSERT(protoLoadMode.structureID == structure->id());
    ASSERT(protoLoadMode.cachedOffset == offset);
    ASSERT(protoLoadMode.cachedSlot == cachedSlot);
}

} // namespace JSC
