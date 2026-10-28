/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTFunctionData.h"
#include "AOTThreshold.h"
#include "Structure.h"
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class LocalAllocator;

namespace AOT {

// The allocator is never null. Where there is none yet it is one without room, so that the test for room, which every allocation makes, is the only one.
struct AllocationSiteRecord {
    uint64_t header;
    uint64_t orderAndShapeWords;
    void* allocator;
    // What the Structure of the header holds as its prototype, for an object. Zero for any other cell and where there is no header.
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

// What a run of up to four slot keys, as a call of DidCreateField takes them, makes of the words of an object without extra properties: the transitions of its keys taken one after the other.
struct alignas(32) KeyOrderRun {
    static constexpr ptrdiff_t offsetOfOrderAndShapeWordsBefore() { return 0; }
    static constexpr ptrdiff_t offsetOfSlotKeys() { return 8; }
    static constexpr ptrdiff_t offsetOfOrderAndCountAfter() { return 16; }

    uint64_t orderAndShapeWordsBefore;
    uint64_t slotKeys;
    uint32_t orderAndCountAfter;
};

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

// The fields of the objects with these two words, in the order of their keys, as indexes into the record of the layout. No entries: the order word does not list every own key.
struct KeyOrderEntriesCacheEntry {
    uint32_t orderWord;
    uint32_t shapeWord;
    const uint16_t* entries;
};

static constexpr uint32_t numberOfCachedKeyOrderEntries = 256;
static constexpr unsigned keyOrderEntriesHashShift = orderWordCountShift - 5;
static constexpr uint32_t indexOfKeyOrderEntries(uint32_t shapeWord, uint32_t orderWord) { return (shapeWord + orderWord + (orderWord >> keyOrderEntriesHashShift)) % numberOfCachedKeyOrderEntries; }

// The enumerator that a for-in loop over a compiled object with this Structure and order word got last.
struct EnumeratorCacheEntry {
    uint32_t orderWord;
    uint32_t structureID;
    JSCell* enumerator;
};

static constexpr uint32_t numberOfCachedEnumerators = 256;
static constexpr unsigned enumeratorHashShift = 4;
static constexpr uint32_t indexOfCachedEnumerator(uint32_t structureID, uint32_t orderWord) { return ((structureID >> enumeratorHashShift) + orderWord) % numberOfCachedEnumerators; }

// What lies in front of the first element is made with it.
AOT_THRESHOLD(uint32_t, maxIndexOfFirstElementMadeByStub, 7);

// The Structure that an object with the Structure `before` gets with another prototype. The headers of their cells differ in the StructureID alone.
// Where RealmWords::extraPropertiesWithElementsStructure stands in place of a prototype, which it never is: the Structure that the object gets with its first element, where nobody has to be asked or told.
struct PrototypeTransition {
    EncodedJSValue prototype;
    uint32_t before;
    uint32_t after;
};

static constexpr uint32_t numberOfPrototypeTransitions = 1024;
static constexpr unsigned prototypeTransitionHashShift = 6;
static constexpr uint32_t indexOfPrototypeTransition(uint32_t before, EncodedJSValue prototype) { return ((before ^ static_cast<uint32_t>(prototype)) >> prototypeTransitionHashShift) % numberOfPrototypeTransitions; }

// What the run time last answered at one place in compiled code where a store, or a run of stores, adds fields: an object with these two words has this order word once the fields are listed.
struct KeyOrderStepCache {
    uint32_t orderWordBefore;
    uint32_t shapeWord;
    uint32_t orderWordAfter;
    uint32_t turnPlusOne;
};

// Places share the entries by their numbers, and an entry names the place that wrote it: few places of a program run, and one entry for each would dirty a page for a handful of them.
// The shapes that come to one place have an entry each, and the places of one function do not meet each other there. The entry's two words and its index together say the place's number within its turn.
static constexpr unsigned logNumberOfKeyOrderStepCaches = 11;
static constexpr uint32_t numberOfKeyOrderStepCaches = 1u << logNumberOfKeyOrderStepCaches;
static constexpr uint32_t maxPlacesWithKeyOrderStepCache = numberOfKeyOrderStepCaches * UINT8_MAX;
static constexpr unsigned keyOrderStepCacheShift = 27;
static constexpr uint32_t indexOfKeyOrderStepCache(uint32_t place, uint64_t orderAndShapeWords) { return (place + (orderAndShapeWords >> keyOrderStepCacheShift)) % numberOfKeyOrderStepCaches; }

struct ObjectWords {
    // One word per allocation: the distance in bytes, zero or negative, from the empty record to the record of the allocation.
    int32_t* distancesToAllocationSites;
    KeyOrderTransition* keyOrderTransitions;
    uint32_t keyOrderTransitionMask;
    uint32_t unused;
    KeyListCacheEntry* keyLists;
    JSCell* emptyKeyList;
    PrototypeTransition* prototypeTransitions;
    EnumeratorCacheEntry* enumerators;
    KeyOrderRun* keyOrderRuns;
    KeyOrderEntriesCacheEntry* keyOrderEntries;

    AllocationSiteRecord* emptyAllocationSite() const { return reinterpret_cast<AllocationSiteRecord*>(distancesToAllocationSites) - 1; }

    AllocationSiteRecord* allocationSite(uint32_t allocation) const
    {
        int32_t distance = distancesToAllocationSites[allocation];
        return distance ? reinterpret_cast<AllocationSiteRecord*>(reinterpret_cast<char*>(emptyAllocationSite()) + distance) : nullptr;
    }
};

struct FunctionTableWords {
    // FunctionTables::words.
    const uint64_t* closureWords;
};

struct ClosureWords {
    static constexpr unsigned numberOfExecutables = 4;
    static constexpr unsigned numberOfSlimClosureAllocators = 3;

    JSCell* executables[numberOfExecutables];
    uint64_t closureHeaders[numberOfClosureStructures];
    // The Structures of plain functions that have got their `prototype` property and nothing else, as they come up. Zero where there is none yet.
    static constexpr unsigned numberOfStructuresWithPrototypeProperty = 4;
    uint32_t structuresWithPrototypeProperty[numberOfStructuresWithPrototypeProperty];
    // Allocator i serves cells of MarkedSpace::sizeStep * (i + 1) bytes. They are there before any compiled code runs.
    LocalAllocator* slimClosureAllocators[numberOfSlimClosureAllocators];
};

// The entrypoints that allocate the object of a construction themselves, as ProgramTables::constructEntrypoints was searched for them: the word of a function's [[Call]], which is never zero, and the word of that entrypoint.
struct ConstructWordCache {
    static constexpr unsigned numberOfEntriesLog2 = 6;

    struct Entry {
        uint64_t wordOfCall;
        uint64_t wordOfConstruct;
    };
    Entry entries[1u << numberOfEntriesLog2];
};
static_assert(sizeof(ConstructWordCache::Entry) == 16);

// Where functions with one Structure keep their `prototype` property. No Structure has the number zero.
struct PlaceOfPrototypeProperty {
    uint32_t structureID;
    int32_t offset;
};
constexpr unsigned numberOfPlacesOfPrototypeProperties = 8;
constexpr unsigned shiftOfStructureIDForPlaceOfPrototypeProperty = 5;

// A class object whose Structure does not list `prototype`: the definition of the class was left to its record. No store changes that property of a class, so the object stands for it.
struct PrototypeOfClassObject {
    JSCell* classObject;
    JSCell* prototype;
};
constexpr unsigned numberOfPrototypesOfClassObjects = 32;
constexpr unsigned shiftOfClassObjectForPrototype = 4;

// What the constructor of a class makes where new.target has another prototype object than the record of the allocation names: that of a class below, or that of another evaluation of the class. It is an object in which nothing is stored yet.
struct InstanceOfClassBelow {
    JSCell* prototype;
    uint64_t header;
    // The cell without its header, in whole chunks of sixteen bytes. The subspace of objects has an allocator for so many bytes.
    const uint8_t* cellAtBirth;
    uint32_t allocation;
    // Half the bytes: how far the allocator for the cell lies behind the first one of the subspace.
    uint32_t offsetOfAllocator;
};
constexpr unsigned numberOfInstancesOfClassesBelow = 64;
// No two prototype objects begin closer to each other.
constexpr unsigned shiftOfPrototypeForInstanceOfClassBelow = 5;
// A record lies at one of two places, so how far apart the prototype objects lie does not decide how many of them have a record.
constexpr size_t firstPlaceOfInstanceOfClassBelow(uintptr_t prototype) { return prototype >> shiftOfPrototypeForInstanceOfClassBelow & (numberOfInstancesOfClassesBelow - 1); }
constexpr size_t secondPlaceOfInstanceOfClassBelow(uintptr_t prototype) { return firstPlaceOfInstanceOfClassBelow(prototype) ^ (firstPlaceOfInstanceOfClassBelow(prototype >> shiftOfPrototypeForInstanceOfClassBelow) | 1); }
// With this bit the number that operationAOTAllocateSlow() takes is the index of such a record, and the operation makes the whole object.
constexpr uint32_t allocatesInstanceOfClassBelow = 1u << 31;

struct ClassWords {
    JSCell** classPrototypes;
    const PlaceOfPrototypeProperty* placesOfPrototypeProperties;
    const PrototypeOfClassObject* prototypesOfClassObjects;
    const InstanceOfClassBelow* instancesOfClassesBelow;
    // By class, as classPrototypes: the brand that the Structure of a cell carries which a built-in constructor has made for `super(..)` of the class. Null until the first is made.
    UniquedStringImpl** brandsOfInstances;
};

static_assert(sizeof(KeyOrderTransition) == 16);
static_assert(sizeof(KeyListCacheEntry) == 16);
static_assert(!(numberOfKeyLists & (numberOfKeyLists - 1)));
static_assert(sizeof(KeyOrderEntriesCacheEntry) == 16 && !(numberOfCachedKeyOrderEntries & (numberOfCachedKeyOrderEntries - 1)));
static_assert(sizeof(EnumeratorCacheEntry) == 16);
static_assert(!(numberOfCachedEnumerators & (numberOfCachedEnumerators - 1)));
static_assert(sizeof(PrototypeTransition) == 16);
static_assert(sizeof(InstanceOfClassBelow) == 32 && !(numberOfInstancesOfClassesBelow & (numberOfInstancesOfClassesBelow - 1)));
static_assert(!(numberOfPrototypeTransitions & (numberOfPrototypeTransitions - 1)));
static_assert(indexOfKeyList(3, numberOfKeyLists + 4) == 7);

static_assert(std::is_trivially_default_constructible_v<KeyOrderTransition>);
static_assert(std::is_trivially_default_constructible_v<KeyListCacheEntry>);
static_assert(std::is_trivially_default_constructible_v<KeyOrderEntriesCacheEntry>);
static_assert(std::is_trivially_default_constructible_v<EnumeratorCacheEntry>);
static_assert(std::is_trivially_default_constructible_v<PrototypeTransition>);
static_assert(std::is_trivially_default_constructible_v<ObjectWords>);
static_assert(std::is_trivially_default_constructible_v<ClosureWords>);
static_assert(std::is_trivially_default_constructible_v<ClassWords>);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
