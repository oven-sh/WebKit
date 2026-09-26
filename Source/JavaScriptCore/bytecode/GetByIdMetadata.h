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

namespace JSC {

enum class GetByIdMode : uint8_t {
    ProtoLoad = 0, // This must be zero to reuse the higher bits of the pointer as this ProtoLoad mode.
    Default = 1,
    Unset = 2,
    ArrayLength = 3,
};

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
    // Always 64 bits wide, so that the enclosing union has one layout on every target and
    // storing the slot always clears the bytes that overlap mode and hitCountForLLIntCaching.
    uint64_t cachedSlot;
};
static_assert(sizeof(GetByIdModeMetadataProtoLoad) == 16);

// What a get_by_id site counts. The pointer to the slot base of a site in ProtoLoad mode is where the other
// modes have their counts, so the counts of such a site are in its entry of CodeBlock::llintGetByIdWatchpointMap().
struct GetByIdSiteCounts {
    uint8_t missCount { 0 }; // Calls of the slow path, for CodeBlock::noteLLIntInlineCacheMiss().
    uint8_t cacheSetupCount { 0 }; // Times that the site tried to make a prototype load or unset cache.
    uint8_t hitCountForLLIntCaching { 0 }; // Results that such a cache can be for, until the site tries to make one.

    // One try is what a site had before Options::useLLIntPrototypeCacheRearming(). A site whose receivers
    // alternate makes a cache with each try and gets no hit from it, so the tries have a limit.
    static constexpr uint8_t maxCacheSetupCount = 4;

    // Starts the countdown again, if the site can still try.
    void rearm()
    {
        bool canTry = Options::useLLIntPrototypeCacheRearming() && cacheSetupCount < maxCacheSetupCount;
        hitCountForLLIntCaching = canTry ? static_cast<uint8_t>(Options::prototypeHitCountForLLIntCaching()) : 0;
    }
};

// This union shares ProtoLoad's cachedSlot with "hitCountForLLIntCaching" and "mode".
// This is possible because these values must be zero if we use ProtoLoad mode.
union GetByIdModeMetadata {
    GetByIdModeMetadata()
    {
        defaultMode.structureID = StructureID();
        defaultMode.cachedOffset = 0;
        defaultMode.padding1 = 0;
        cacheSetupCount = 0;
        missCount = 0;
        mode = GetByIdMode::Default;
        hitCountForLLIntCaching = Options::prototypeHitCountForLLIntCaching();
    }

    // The setters are for a site in any mode. One that leaves ProtoLoad mode has bytes of the pointer to the slot
    // base where the counts are: the caller has the counts from CodeBlock::llintGetByIdSiteCounts().
    void clearToDefaultModeWithoutCache(GetByIdSiteCounts);
    void setUnsetMode(Structure*, GetByIdSiteCounts);
    void setArrayLengthMode(GetByIdSiteCounts);
    // The caller keeps counts() in the entry of the site in CodeBlock::llintGetByIdWatchpointMap().
    void setProtoLoadMode(Structure*, PropertyOffset, JSObject*);

    // True if the cache is one that watchpoints guard. The structure of the receiver is enough for the others.
    bool hasGuards() const { return mode == GetByIdMode::ProtoLoad || mode == GetByIdMode::Unset; }

    // False if the countdown of the site is over. True for a site in ProtoLoad mode whose entry has to say.
    bool mayCountDown() const
    {
        if (mode == GetByIdMode::ProtoLoad)
            return Options::useLLIntPrototypeCacheRearming();
        return !!hitCountForLLIntCaching;
    }

    GetByIdSiteCounts counts() const
    {
        ASSERT(mode != GetByIdMode::ProtoLoad);
        return { missCount, cacheSetupCount, hitCountForLLIntCaching };
    }
    void setCounts(GetByIdSiteCounts);

    struct {
        uint32_t padding1;
        uint32_t padding2;
        uint32_t padding3;
        uint8_t cacheSetupCount; // Not in ProtoLoad mode.
        uint8_t missCount; // Not in ProtoLoad mode.
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

inline void GetByIdModeMetadata::setCounts(GetByIdSiteCounts counts)
{
    padding3 = 0;
    cacheSetupCount = counts.cacheSetupCount;
    missCount = counts.missCount;
    hitCountForLLIntCaching = counts.hitCountForLLIntCaching;
}

inline void GetByIdModeMetadata::clearToDefaultModeWithoutCache(GetByIdSiteCounts counts)
{
    mode = GetByIdMode::Default;
    defaultMode.structureID = StructureID();
    defaultMode.cachedOffset = 0;
    setCounts(counts);
}

inline void GetByIdModeMetadata::setUnsetMode(Structure* structure, GetByIdSiteCounts counts)
{
    mode = GetByIdMode::Unset;
    unsetMode.structureID = structure->id();
    defaultMode.cachedOffset = 0;
    setCounts(counts);
}

inline void GetByIdModeMetadata::setArrayLengthMode(GetByIdSiteCounts counts)
{
    mode = GetByIdMode::ArrayLength;
    // We should clear the structure ID to avoid the old structure ID being saved.
    defaultMode.structureID = StructureID();
    defaultMode.cachedOffset = 0;
    // Prevent the prototype cache from ever happening.
    counts.hitCountForLLIntCaching = 0;
    setCounts(counts);
}

inline void GetByIdModeMetadata::setProtoLoadMode(Structure* structure, PropertyOffset offset, JSObject* cachedSlot)
{
    // We rely on ProtoLoad being 0, or else the high bits of cachedSlot would write the wrong mode and hit count.
    static_assert(!static_cast<std::underlying_type_t<GetByIdMode>>(GetByIdMode::ProtoLoad));

    protoLoadMode.structureID = structure->id();
    protoLoadMode.cachedOffset = offset;

    // We know that this pointer will remain valid because it will be cleared by either a watchpoint fire or
    // during GC when we clear the LLInt caches.

    // The write to cachedSlot also writes the mode, since they overlap in the struct layout. We know that
    // the mode ProtoLoad is 0 by the static assertion above.
    protoLoadMode.cachedSlot = static_cast<uint64_t>(std::bit_cast<uintptr_t>(cachedSlot));

    ASSERT(mode == GetByIdMode::ProtoLoad);
    ASSERT(!hitCountForLLIntCaching);
    ASSERT(protoLoadMode.structureID == structure->id());
    ASSERT(protoLoadMode.cachedOffset == offset);
    ASSERT(protoLoadMode.cachedSlot == static_cast<uint64_t>(std::bit_cast<uintptr_t>(cachedSlot)));
}

} // namespace JSC
