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
    StringLength = 4, // The length of a string. This mode also reads the length of an array, so that a site with both does not alternate.
};

// Every mode starts with the same eight bytes: the structure of the receiver, the offset of the
// property, and the two counters of the site. A mode that does not use a field keeps it zero.
struct GetByIdModeMetadataDefault {
    StructureID structureID;
    uint16_t cachedOffset;
    uint8_t missCount;
    uint8_t hitCountForLLIntCaching;
    unsigned padding1;
};
static_assert(sizeof(GetByIdModeMetadataDefault) == 12);

struct GetByIdModeMetadataUnset {
    StructureID structureID;
    uint16_t padding1;
    uint8_t missCount;
    uint8_t hitCountForLLIntCaching;
    unsigned padding2;
};
static_assert(sizeof(GetByIdModeMetadataUnset) == 12);

struct GetByIdModeMetadataArrayLength {
    unsigned padding1;
    uint16_t padding2;
    uint8_t missCount;
    uint8_t hitCountForLLIntCaching;
    unsigned padding3;
};
static_assert(sizeof(GetByIdModeMetadataArrayLength) == 12);

struct GetByIdModeMetadataProtoLoad {
    StructureID structureID;
    uint16_t cachedOffset;
    uint8_t missCount;
    uint8_t hitCountForLLIntCaching;
    // Always 64 bits wide, so that the enclosing union has one layout on every target and
    // storing the slot always clears the byte that overlaps mode.
    uint64_t cachedSlot;
};
static_assert(sizeof(GetByIdModeMetadataProtoLoad) == 16);

// This union shares ProtoLoad's cachedSlot with "mode".
// This is possible because this value must be zero if we use ProtoLoad mode.
union GetByIdModeMetadata {
    GetByIdModeMetadata()
    {
        defaultMode.structureID = StructureID();
        defaultMode.cachedOffset = 0;
        defaultMode.padding1 = 0;
        padding4 = 0;
        mode = GetByIdMode::Default;
        padding5 = 0;
        missCount = 0;
        hitCountForLLIntCaching = Options::prototypeHitCountForLLIntCaching();
    }

    // The cached offset has 16 bits. A property with a larger offset is not cached.
    static bool isCacheableOffset(PropertyOffset offset) { return static_cast<unsigned>(offset) <= std::numeric_limits<uint16_t>::max(); }

    static constexpr uint8_t maxMissCount = std::numeric_limits<uint8_t>::max();

    // A site that called the slow path maxMissCount times alternates between receivers that one
    // cache cannot serve. The countdown does not start again for it, so what it costs has a limit.
    bool canRearm() const { return Options::useLLIntPrototypeCacheRearming() && missCount != maxMissCount; }
    void rearmIfPossible();
    bool hasWatchedStructure() const { return (mode == GetByIdMode::ProtoLoad || mode == GetByIdMode::Unset) && !!defaultMode.structureID; }

    void clearToDefaultModeWithoutCache();
    void setUnsetMode(Structure*);
    void setArrayLengthMode();
    void setStringLengthMode();
    void setProtoLoadMode(Structure*, PropertyOffset, JSObject*);

    struct {
        uint32_t padding1;
        uint16_t padding2;
        uint8_t missCount; // Calls of the slow path from this site. It stops at maxMissCount.
        uint8_t hitCountForLLIntCaching; // Results from the prototype chain to see before one is cached. 0 means that none is.
        uint32_t padding3;
        uint16_t padding4;
        GetByIdMode mode;
        uint8_t padding5; // This must be zero when we use ProtoLoad mode.
    };
    static constexpr ptrdiff_t offsetOfMode() { return OBJECT_OFFSETOF(GetByIdModeMetadata, mode); }
    GetByIdModeMetadataDefault defaultMode;
    GetByIdModeMetadataUnset unsetMode;
    GetByIdModeMetadataArrayLength arrayLengthMode;
    GetByIdModeMetadataProtoLoad protoLoadMode;
};
static_assert(sizeof(GetByIdModeMetadata) == 16);

inline void GetByIdModeMetadata::rearmIfPossible()
{
    if (canRearm())
        hitCountForLLIntCaching = Options::prototypeHitCountForLLIntCaching();
}

inline void GetByIdModeMetadata::clearToDefaultModeWithoutCache()
{
    mode = GetByIdMode::Default;
    defaultMode.structureID = StructureID();
    defaultMode.cachedOffset = 0;
}

inline void GetByIdModeMetadata::setUnsetMode(Structure* structure)
{
    mode = GetByIdMode::Unset;
    unsetMode.structureID = structure->id();
    defaultMode.cachedOffset = 0;
}

inline void GetByIdModeMetadata::setArrayLengthMode()
{
    mode = GetByIdMode::ArrayLength;
    // We should clear the structure ID to avoid the old structure ID being saved.
    defaultMode.structureID = StructureID();
    defaultMode.cachedOffset = 0;
    // Prevent the prototype cache from ever happening.
    hitCountForLLIntCaching = 0;
}

inline void GetByIdModeMetadata::setStringLengthMode()
{
    mode = GetByIdMode::StringLength;
    defaultMode.structureID = StructureID();
    defaultMode.cachedOffset = 0;
    hitCountForLLIntCaching = 0;
}

inline void GetByIdModeMetadata::setProtoLoadMode(Structure* structure, PropertyOffset offset, JSObject* cachedSlot)
{
    // We rely on ProtoLoad being 0, or else the high bits of cachedSlot would write the wrong mode.
    static_assert(!static_cast<std::underlying_type_t<GetByIdMode>>(GetByIdMode::ProtoLoad));
    ASSERT(isCacheableOffset(offset));

    protoLoadMode.structureID = structure->id();
    protoLoadMode.cachedOffset = static_cast<uint16_t>(offset);

    // We know that this pointer will remain valid because it will be cleared by either a watchpoint fire or
    // during GC when we clear the LLInt caches.

    // The write to cachedSlot also writes the mode, since they overlap in the struct layout. We know that
    // the mode ProtoLoad is 0 by the static assertion above.
    protoLoadMode.cachedSlot = static_cast<uint64_t>(std::bit_cast<uintptr_t>(cachedSlot));

    ASSERT(mode == GetByIdMode::ProtoLoad);
    ASSERT(protoLoadMode.structureID == structure->id());
    ASSERT(protoLoadMode.cachedOffset == offset);
    ASSERT(protoLoadMode.cachedSlot == static_cast<uint64_t>(std::bit_cast<uintptr_t>(cachedSlot)));
}

} // namespace JSC
