/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTFunctionData.h"
#include "Structure.h"
#include <span>
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class LocalAllocator;

namespace AOT {

struct AllocationSiteRecord {
    uint64_t header;
    uint64_t orderAndShapeWords;
    void* allocator;
    uint64_t prototype;

    static constexpr ptrdiff_t offsetOfHeader() { return offsetof(AllocationSiteRecord, header); }
    static constexpr ptrdiff_t offsetOfOrderAndShapeWords() { return offsetof(AllocationSiteRecord, orderAndShapeWords); }
    static constexpr ptrdiff_t offsetOfAllocator() { return offsetof(AllocationSiteRecord, allocator); }
    static constexpr ptrdiff_t offsetOfPrototype() { return offsetof(AllocationSiteRecord, prototype); }
    static constexpr unsigned sizeShift = 5;
};
static_assert(sizeof(AllocationSiteRecord) == 1u << AllocationSiteRecord::sizeShift);
static_assert(std::is_trivially_default_constructible_v<AllocationSiteRecord> && std::is_standard_layout_v<AllocationSiteRecord>);

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

struct alignas(32) KeyOrderRun {
    static constexpr ptrdiff_t offsetOfOrderAndShapeWordsBefore() { return 0; }
    static constexpr ptrdiff_t offsetOfSlotKeys() { return 8; }
    static constexpr ptrdiff_t offsetOfOrderAndCountAfter() { return 16; }

    uint64_t orderAndShapeWordsBefore;
    uint64_t slotKeys;
    uint32_t orderAndCountAfter;
};

static constexpr uint64_t keysOfRunBesideExtraKeys(uint32_t slotKey, uint32_t numberOfExtraKeys) { return slotKey | static_cast<uint64_t>(numberOfExtraKeys | 1u << bitsPerSlotKey) << (2 * bitsPerSlotKey); }
static_assert(keysOfRunBesideExtraKeys(24, 3) == 0x0001000300000018ull && noSlotKey == 0);

static constexpr uint32_t numberOfKeyOrderRuns = 2048;
static constexpr unsigned logSizeOfKeyOrderRun = 5;
static_assert(sizeof(KeyOrderRun) == 1u << logSizeOfKeyOrderRun && !(numberOfKeyOrderRuns & (numberOfKeyOrderRuns - 1)));
static constexpr uint32_t indexOfKeyOrderRun(uint64_t orderAndShapeWordsBefore, uint64_t slotKeys)
{
    return (static_cast<uint32_t>(orderAndShapeWordsBefore >> 32) ^ static_cast<uint32_t>(orderAndShapeWordsBefore) ^ static_cast<uint32_t>(slotKeys)) % numberOfKeyOrderRuns;
}

struct KeyListCacheEntry {
    uint32_t orderWord;
    uint32_t shapeWord;
    JSCell* list;
};

static constexpr uint32_t numberOfKeyLists = 1024;
static constexpr uint32_t indexOfKeyList(uint32_t shapeWord, uint32_t orderWord) { return (shapeWord + orderWord) % numberOfKeyLists; }
static constexpr uint32_t keyListWordOfStructure(uint32_t structureID) { return structureID >> 1; }

struct EnumeratorCacheEntry {
    uint32_t orderWord;
    uint32_t structureID;
    JSCell* enumerator;
};

static constexpr uint32_t numberOfCachedEnumerators = 256;
static constexpr unsigned enumeratorHashShift = 4;
static constexpr uint32_t indexOfCachedEnumerator(uint32_t structureID, uint32_t orderWord) { return ((structureID >> enumeratorHashShift) + orderWord) % numberOfCachedEnumerators; }

struct PrototypeTransition {
    EncodedJSValue prototype;
    uint32_t before;
    uint32_t after;
};

static constexpr uint32_t numberOfPrototypeTransitions = 1024;
static constexpr unsigned prototypeTransitionHashShift = 4;
static constexpr uint32_t indexOfPrototypeTransition(uint32_t before, EncodedJSValue prototype) { return ((before ^ static_cast<uint32_t>(prototype)) >> prototypeTransitionHashShift) % numberOfPrototypeTransitions; }
static constexpr uint32_t maxIndexOfFirstElementStoredContiguously = 7;

struct CopyWithNames {
    uint32_t structureOfSource;
    uint32_t firstName;
    uint64_t header;
    uint16_t count;
    uint16_t cellSize;
    uint16_t storageSizeOfSource;
    uint16_t storageSize;
};

struct NameInCopy {
    uint32_t name;
    int32_t offset;
};

static constexpr uint32_t numberOfPlacesOfCopiesWithNames = 512;
static constexpr uint32_t sizeOfCopiesWithNames = 16 * 1024;
static constexpr uint32_t offsetOfTakenBytesOfCopiesWithNames = numberOfPlacesOfCopiesWithNames * sizeof(uint32_t);
static constexpr uint32_t offsetOfFirstCopyWithNames = offsetOfTakenBytesOfCopiesWithNames + sizeof(uint64_t);
static constexpr unsigned copyWithNamesHashShift = 6;
static constexpr uint32_t copiesThatTakeAnOccupiedPlaceMask = 63;
static constexpr uint32_t placeOfCopyWithNames(uint32_t structureOfSource, uint32_t firstName) { return ((structureOfSource >> copyWithNamesHashShift) + firstName) % numberOfPlacesOfCopiesWithNames; }
static_assert(sizeof(CopyWithNames) == 24 && sizeof(NameInCopy) == 8 && !(offsetOfFirstCopyWithNames % alignof(CopyWithNames)));

struct ObjectWords {
    int32_t* distancesToAllocationSites;
    KeyOrderTransition* keyOrderTransitions;
    uint32_t keyOrderTransitionMask;
    uint32_t unused;
    KeyListCacheEntry* keyLists;
    JSCell* emptyKeyList;
    PrototypeTransition* prototypeTransitions;
    uint8_t* copiesWithNames;
    uint32_t copiesThatFoundTheirPlaceOccupied;
    uint32_t unusedBehindCopies;
    EnumeratorCacheEntry* enumerators;
    KeyOrderRun* keyOrderRuns;

    AllocationSiteRecord* emptyAllocationSite() const { return reinterpret_cast<AllocationSiteRecord*>(distancesToAllocationSites) - 1; }

    AllocationSiteRecord* allocationSite(uint32_t allocation) const
    {
        int32_t distance = distancesToAllocationSites[allocation];
        return distance ? reinterpret_cast<AllocationSiteRecord*>(reinterpret_cast<char*>(emptyAllocationSite()) + distance) : nullptr;
    }
};

struct FunctionTableWords {
    const uint64_t* closureWords;
};

struct ClosureWords {
    static constexpr unsigned numberOfExecutables = 3;
    static constexpr unsigned numberOfSlimClosureAllocators = 5;

    JSCell* executables[numberOfExecutables];
    uint64_t closureHeaders[numberOfClosureStructures];
    static constexpr unsigned numberOfStructuresWithPrototypeProperty = 4;
    uint32_t structuresWithPrototypeProperty[numberOfStructuresWithPrototypeProperty];
    LocalAllocator* slimClosureAllocators[numberOfSlimClosureAllocators];
};

struct ConstructWordCache {
    static constexpr unsigned numberOfEntriesLog2 = 6;

    struct Entry {
        uint64_t wordOfCall;
        uint64_t wordOfConstruct;
    };
    Entry entries[1u << numberOfEntriesLog2];
};
static_assert(sizeof(ConstructWordCache::Entry) == 16);

struct PlaceOfPrototypeProperty {
    uint32_t structureID;
    int32_t offset;
};
constexpr unsigned numberOfPlacesOfPrototypeProperties = 256;
constexpr uint32_t multiplierOfPlaceOfPrototypeProperty = 0x9E3779B1;
constexpr unsigned shiftOfProductOfPlaceOfPrototypeProperty = 24;
constexpr uint32_t indexOfPlaceOfPrototypeProperty(uint32_t structureID) { return (structureID * multiplierOfPlaceOfPrototypeProperty) >> shiftOfProductOfPlaceOfPrototypeProperty; }
static_assert(numberOfPlacesOfPrototypeProperties == 1u << (32 - shiftOfProductOfPlaceOfPrototypeProperty));

struct PrototypeOfClassObject {
    JSCell* classObject;
    JSCell* prototype;
};
constexpr unsigned numberOfPrototypesOfClassObjects = 32;
constexpr unsigned shiftOfClassObjectForPrototype = 4;

struct InstanceBelowPrototype {
    JSCell* prototype;
    uint64_t allocationOfSite;
    void* allocator;
    const uint64_t* words;
};
constexpr unsigned shiftOfInstanceBelowPrototype = 5;
constexpr uint64_t multiplierOfInstanceBelowPrototype = 0x9E3779B97F4A7C15;
constexpr unsigned shiftOfProductOfInstanceBelowPrototype = 32 - shiftOfInstanceBelowPrototype;
constexpr uint64_t firstOffsetOfInstanceBelowPrototype(uint64_t bitsOfPrototype, uint32_t allocationOfSite) { return ((bitsOfPrototype + allocationOfSite) * multiplierOfInstanceBelowPrototype) >> shiftOfProductOfInstanceBelowPrototype; }

struct InstancesBelowPrototypes {
    const InstanceBelowPrototype* entries;
    uint64_t maskOfOffsets;
};

struct FieldData;

struct PrivateFieldPlace {
    uint64_t layouts;
    uint32_t declaredField;
    uint32_t unused;
    const FieldData* field;
    uint64_t alsoUnused;
};
constexpr uint32_t numberOfPrivateFieldPlaces = 64;
constexpr unsigned shiftOfPrivateFieldPlace = 5;
constexpr unsigned shiftOfFieldInIndexOfPrivateFieldPlace = 3;
constexpr size_t indexOfPrivateFieldPlace(uint32_t layoutOfObject, uint32_t declaringClass, uint32_t declaredField) { return ((layoutOfObject + 3 * declaringClass) ^ declaredField << shiftOfFieldInIndexOfPrivateFieldPlace) & (numberOfPrivateFieldPlaces - 1); }
constexpr uint32_t layoutHasNoPlaceForPrivateField = 1u << 31;

struct StructuresOfHeir {
    static constexpr unsigned numberThatPass = 4;
    static constexpr unsigned numberRefused = 2;

    static constexpr ptrdiff_t offsetOfOneThatPasses(unsigned index) { return offsetof(StructuresOfHeir, thatPass) + index * sizeof(uint32_t); }
    static constexpr ptrdiff_t offsetOfOneRefused(unsigned index) { return offsetof(StructuresOfHeir, refused) + index * sizeof(uint32_t); }
    static constexpr ptrdiff_t offsetOfDisplacementOfField(unsigned field) { return sizeof(StructuresOfHeir) + field * sizeof(int32_t); }
    static constexpr size_t sizeWithDisplacementsOfFields(size_t numberOfFields) { return sizeof(StructuresOfHeir) + numberOfFields * sizeof(int32_t); }

    bool holdsNoStructure() const { return !thatPass[0] && !refused[0]; }
    std::span<int32_t> displacementsOfFields(size_t numberOfFields) { return { reinterpret_cast<int32_t*>(this + 1), numberOfFields }; }

    void layInFront(uint32_t structureID, bool passes)
    {
        uint32_t* words = passes ? thatPass : refused;
        for (unsigned index = (passes ? numberThatPass : numberRefused) - 1; index; --index)
            words[index] = words[index - 1];
        words[0] = structureID;
    }

    void layAtBack(uint32_t structureID, bool passes)
    {
        (passes ? thatPass[numberThatPass - 1] : refused[numberRefused - 1]) = structureID;
    }

    uint32_t thatPass[numberThatPass];
    uint32_t refused[numberRefused];
};
static_assert(sizeof(StructuresOfHeir) == (StructuresOfHeir::numberThatPass + StructuresOfHeir::numberRefused) * sizeof(uint32_t) && !StructuresOfHeir::offsetOfOneThatPasses(0));
static_assert(std::is_trivially_default_constructible_v<StructuresOfHeir> && std::is_standard_layout_v<StructuresOfHeir>);

struct ClassWords {
    JSCell** classPrototypes;
    uint8_t* structuresOfHeirs;
    const PlaceOfPrototypeProperty* placesOfPrototypeProperties;
    const PrototypeOfClassObject* prototypesOfClassObjects;
    PrivateFieldPlace* privateFieldPlaces;
    InstancesBelowPrototypes instancesBelowPrototypes;
};

static_assert(sizeof(PrivateFieldPlace) == 1u << shiftOfPrivateFieldPlace && !(numberOfPrivateFieldPlaces & (numberOfPrivateFieldPlaces - 1)));
static_assert(sizeof(InstanceBelowPrototype) == 1u << shiftOfInstanceBelowPrototype);
static_assert(sizeof(KeyOrderTransition) == 16);
static_assert(sizeof(KeyListCacheEntry) == 16);
static_assert(!(numberOfKeyLists & (numberOfKeyLists - 1)));
static_assert(sizeof(EnumeratorCacheEntry) == 16);
static_assert(!(numberOfCachedEnumerators & (numberOfCachedEnumerators - 1)));
static_assert(sizeof(PrototypeTransition) == 16);
static_assert(!(numberOfPrototypeTransitions & (numberOfPrototypeTransitions - 1)));
static_assert(indexOfKeyList(3, numberOfKeyLists + 4) == 7);

static_assert(std::is_trivially_default_constructible_v<KeyOrderTransition>);
static_assert(std::is_trivially_default_constructible_v<KeyOrderRun>);
static_assert(std::is_trivially_default_constructible_v<KeyListCacheEntry>);
static_assert(std::is_trivially_default_constructible_v<EnumeratorCacheEntry>);
static_assert(std::is_trivially_default_constructible_v<PrototypeTransition>);
static_assert(std::is_trivially_default_constructible_v<ObjectWords>);
static_assert(std::is_trivially_default_constructible_v<ClosureWords>);
static_assert(std::is_trivially_default_constructible_v<ClassWords>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
