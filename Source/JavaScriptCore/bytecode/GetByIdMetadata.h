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
    CompiledField = 4,
    CustomAccessor = 5,
};

enum class GetByIdCompiledFieldKind : uint32_t {
    JSValue,
    Cell32,
    Int32OrNumberBox,
    Int32,
    Boolean,
    Double,
    Nothing,
    Uint8,
    Int8,
    Uint16,
    Int16,
    Uint32,
    Int64,
    OptionalInt64,
    OptionalBoolean,
    ExtraProperty,
};
enum class InByIdKind : uint32_t {
    ByStructure,
    JSValue,
    Cell32,
    Int32OrNumberBox,
    Double,
    AlwaysThere,
};
static constexpr uint32_t getByIdMaxCell32Immediate = 10;
static constexpr unsigned getByIdCell32Shift = 3;
static constexpr unsigned getByIdMarkedBlockCageBits = 35;
static constexpr int32_t getByIdMinInlineInt32 = -(1 << 30);
static constexpr int32_t getByIdAbsentInt32 = INT32_MIN + 1;
enum class PutByIdCompiledFieldKind : uint32_t {
    Int32OrNumberBox,
    Double,
    OptionalDouble,
    Int32,
    Boolean,
    JSValue,
    StringInCell32,
    ExtraProperty,
};
static constexpr unsigned putByIdCompiledFieldKindShift = 16;
static constexpr unsigned putByIdExtraPropertyOffsetShift = 20;
static constexpr uint32_t putByIdOffsetOfOrderWord = 8;
static constexpr uint32_t putByIdOrderWordIsFrozen = 1u << 29;
static constexpr uint64_t putByIdAbsentDouble = 0x7ffcull << 48;

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

struct GetByIdModeMetadataCompiledField {
    StructureID structureID;
    uint16_t offset;
    uint16_t absentIsUndefined;
    GetByIdCompiledFieldKind kind;
};
static_assert(sizeof(GetByIdModeMetadataCompiledField) == 12);

struct GetByIdModeMetadataProtoLoad {
    StructureID structureID;
    PropertyOffset cachedOffset;
    // Always 64 bits wide, so that the enclosing union has one layout on every target and
    // storing the slot always clears the bytes that overlap mode and hitCountForLLIntCaching.
    uint64_t cachedSlot;
};
static_assert(sizeof(GetByIdModeMetadataProtoLoad) == 16);

struct GetByIdModeMetadataCustomAccessor {
    static constexpr unsigned holderBits = 48;

    StructureID structureID;
    PropertyOffset cachedOffset;
    uint64_t holderAndMode;
};
static_assert(sizeof(GetByIdModeMetadataCustomAccessor) == 16);

// This union shares ProtoLoad's cachedSlot with "hitCountForLLIntCaching" and "mode".
// This is possible because these values must be zero if we use ProtoLoad mode.
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
    void setCompiledFieldMode(Structure*, uint16_t offset, GetByIdCompiledFieldKind, bool absentIsUndefined);
    bool trySetCustomAccessorMode(Structure*, PropertyOffset, JSObject* holder);
    JSObject* customAccessorHolder() const;

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
    GetByIdModeMetadataCompiledField compiledFieldMode;
    GetByIdModeMetadataCustomAccessor customAccessorMode;
};
static_assert(sizeof(GetByIdModeMetadata) == 16);

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

inline void GetByIdModeMetadata::setCompiledFieldMode(Structure* structure, uint16_t offset, GetByIdCompiledFieldKind kind, bool absentIsUndefined)
{
    mode = GetByIdMode::CompiledField;
    compiledFieldMode.structureID = structure->id();
    compiledFieldMode.offset = offset;
    compiledFieldMode.absentIsUndefined = absentIsUndefined;
    compiledFieldMode.kind = kind;
    hitCountForLLIntCaching = 0;
}

inline bool GetByIdModeMetadata::trySetCustomAccessorMode(Structure* structure, PropertyOffset offset, JSObject* holder)
{
    static_assert(OBJECT_OFFSETOF(GetByIdModeMetadata, mode) == OBJECT_OFFSETOF(GetByIdModeMetadataCustomAccessor, holderAndMode) + GetByIdModeMetadataCustomAccessor::holderBits / 8);
    uint64_t bits = static_cast<uint64_t>(std::bit_cast<uintptr_t>(holder));
    if (bits >> GetByIdModeMetadataCustomAccessor::holderBits)
        return false;
    customAccessorMode.structureID = structure->id();
    customAccessorMode.cachedOffset = offset;
    customAccessorMode.holderAndMode = bits | static_cast<uint64_t>(GetByIdMode::CustomAccessor) << GetByIdModeMetadataCustomAccessor::holderBits;
    ASSERT(mode == GetByIdMode::CustomAccessor);
    ASSERT(!hitCountForLLIntCaching);
    return true;
}

inline JSObject* GetByIdModeMetadata::customAccessorHolder() const
{
    ASSERT(mode == GetByIdMode::CustomAccessor);
    return std::bit_cast<JSObject*>(static_cast<uintptr_t>(customAccessorMode.holderAndMode & ((1ull << GetByIdModeMetadataCustomAccessor::holderBits) - 1)));
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
