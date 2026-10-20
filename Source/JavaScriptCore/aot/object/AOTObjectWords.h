/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAllocationSiteRecord.h"
#include "AOTCellFormat.h"
#include "AOTFunctionData.h"
#include "Structure.h"
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class LocalAllocator;

namespace AOT {

ALWAYS_INLINE uint64_t cellHeaderFor(Structure* structure)
{
    static_assert(!JSCell::structureIDOffset() && JSCell::indexingTypeAndMiscOffset() == sizeof(StructureID) && sizeof(JSCell) == sizeof(uint64_t));
    return static_cast<uint64_t>(structure->typeInfoBlob()) << 32 | structure->id().bits();
}

static constexpr unsigned bitsPerSlotKey = 16;
static constexpr uint32_t slotKeyMask = (1u << bitsPerSlotKey) - 1;
static constexpr uint32_t outOfLineSlotKey = 1u << (bitsPerSlotKey - 1);
static constexpr uint32_t noSlotKey = 0;
static constexpr uint32_t slotKeyFor(uint32_t offset, bool isOutOfLine) { return offset < outOfLineSlotKey ? offset | (isOutOfLine ? outOfLineSlotKey : 0) : noSlotKey; }
// An extra key that has no position yet comes before the new field, so what the order becomes also depends on how many extra keys the object has.
static constexpr uint32_t transitionKeyFor(uint32_t slotKey, uint32_t numberOfExtraKeys) { return slotKey | numberOfExtraKeys << bitsPerSlotKey; }

struct alignas(16) KeyOrderTransition {
    static constexpr uint32_t hashMultiplier = 0x9e3779b1;
    static constexpr unsigned hashFoldShift = 16;
    static constexpr uint32_t hashOf(uint64_t orderAndShapeWordsBefore, uint32_t key)
    {
        uint32_t product = (static_cast<uint32_t>(orderAndShapeWordsBefore) ^ static_cast<uint32_t>(orderAndShapeWordsBefore >> 32) * hashMultiplier ^ key) * hashMultiplier;
        return product ^ product >> hashFoldShift;
    }

    bool isEmpty() const { return !orderAndShapeWordsBefore; }

    static constexpr ptrdiff_t offsetOfOrderAndShapeWordsBefore() { return 0; }
    static constexpr ptrdiff_t offsetOfKey() { return 8; }
    static constexpr ptrdiff_t offsetOfOrderAndCountAfter() { return 12; }

    uint64_t orderAndShapeWordsBefore;
    uint32_t key;
    uint32_t orderAndCountAfter;
};

struct KeyListCacheEntry {
    uint32_t orderWord;
    uint32_t shapeWord;
    JSCell* list;
};

static constexpr uint32_t numberOfKeyLists = 1024;
static constexpr uint32_t indexOfKeyList(uint32_t shapeWord, uint32_t orderWord) { return (shapeWord + orderWord) % numberOfKeyLists; }

// The enumerator that a for-in loop over a compiled object with this Structure and order word got last.
struct EnumeratorCacheEntry {
    uint32_t orderWord;
    uint32_t structureID;
    JSCell* enumerator;
};

static constexpr uint32_t numberOfCachedEnumerators = 256;
static constexpr unsigned enumeratorHashShift = 6;
static constexpr uint32_t indexOfCachedEnumerator(uint32_t structureID, uint32_t orderWord) { return ((structureID >> enumeratorHashShift) + orderWord) % numberOfCachedEnumerators; }

// The Structure that an object with the Structure `before` gets with another prototype. The headers of their cells differ in the StructureID alone.
struct PrototypeTransition {
    EncodedJSValue prototype;
    uint32_t before;
    uint32_t after;
};

static constexpr uint32_t numberOfPrototypeTransitions = 64;
static constexpr unsigned prototypeTransitionHashShift = 6;
static constexpr uint32_t indexOfPrototypeTransition(uint32_t before, EncodedJSValue prototype) { return ((before ^ static_cast<uint32_t>(prototype)) >> prototypeTransitionHashShift) % numberOfPrototypeTransitions; }

struct ObjectWords {
    // One word per allocation: the distance in bytes, zero or negative, from the empty record to the record of the allocation.
    // The empty record lies right below the words and the others below it, so a word that nobody has written names the empty record.
    int32_t* distancesToAllocationSites;
    KeyOrderTransition* keyOrderTransitions;
    uint32_t keyOrderTransitionMask;
    uint32_t unused;
    KeyListCacheEntry* keyLists;
    JSCell* emptyKeyList;
    PrototypeTransition* prototypeTransitions;
    EnumeratorCacheEntry* enumerators;

    AllocationSiteRecord* emptyAllocationSite() const { return reinterpret_cast<AllocationSiteRecord*>(distancesToAllocationSites) - 1; }

    AllocationSiteRecord* allocationSite(uint32_t allocation) const
    {
        int32_t distance = distancesToAllocationSites[allocation];
        return distance ? reinterpret_cast<AllocationSiteRecord*>(reinterpret_cast<char*>(emptyAllocationSite()) + distance) : nullptr;
    }
};

struct ClosureWords {
    static constexpr unsigned numberOfExecutables = 4;
    static constexpr unsigned numberOfSlimClosureAllocators = 3;

    const uint64_t* closureWords;
    JSCell* executables[numberOfExecutables];
    uint64_t closureHeaders[numberOfClosureStructures];
    // Allocator i serves cells of MarkedSpace::sizeStep * (i + 1) bytes. They are there before any compiled code runs.
    LocalAllocator* slimClosureAllocators[numberOfSlimClosureAllocators];
};

struct ClassWords {
    JSCell** classPrototypes;
};

static_assert(sizeof(KeyOrderTransition) == 16);
static_assert(sizeof(KeyListCacheEntry) == 16);
static_assert(!(numberOfKeyLists & (numberOfKeyLists - 1)));
static_assert(sizeof(EnumeratorCacheEntry) == 16);
static_assert(!(numberOfCachedEnumerators & (numberOfCachedEnumerators - 1)));
static_assert(sizeof(PrototypeTransition) == 16);
static_assert(!(numberOfPrototypeTransitions & (numberOfPrototypeTransitions - 1)));
static_assert(indexOfKeyList(3, numberOfKeyLists + 4) == 7);

static_assert(std::is_trivially_default_constructible_v<KeyOrderTransition>);
static_assert(std::is_trivially_default_constructible_v<KeyListCacheEntry>);
static_assert(std::is_trivially_default_constructible_v<EnumeratorCacheEntry>);
static_assert(std::is_trivially_default_constructible_v<PrototypeTransition>);
static_assert(std::is_trivially_default_constructible_v<ObjectWords>);
static_assert(std::is_trivially_default_constructible_v<ClosureWords>);
static_assert(std::is_trivially_default_constructible_v<ClassWords>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
