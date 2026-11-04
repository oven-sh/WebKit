/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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
// Of an object that has extra properties, and no other flag in its order word, what one slot key makes of its words: they are held with that flag, and the keys are keysOfRunBesideExtraKeys().
struct alignas(32) KeyOrderRun {
    static constexpr ptrdiff_t offsetOfOrderAndShapeWordsBefore() { return 0; }
    static constexpr ptrdiff_t offsetOfSlotKeys() { return 8; }
    static constexpr ptrdiff_t offsetOfOrderAndCountAfter() { return 16; }

    uint64_t orderAndShapeWordsBefore;
    uint64_t slotKeys;
    uint32_t orderAndCountAfter;
};

// No run of slot keys looks like it: a run ends at its first key that is zero, and here the second is.
static constexpr uint64_t keysOfRunBesideExtraKeys(uint32_t slotKey, uint32_t numberOfExtraKeys) { return slotKey | static_cast<uint64_t>(numberOfExtraKeys | 1u << bitsPerSlotKey) << (2 * bitsPerSlotKey); }
static_assert(keysOfRunBesideExtraKeys(24, 3) == 0x0001000300000018ull && noSlotKey == 0);

static constexpr uint32_t numberOfKeyOrderRuns = 2048;
static constexpr unsigned logSizeOfKeyOrderRun = 5;
static_assert(sizeof(KeyOrderRun) == 1u << logSizeOfKeyOrderRun && !(numberOfKeyOrderRuns & (numberOfKeyOrderRuns - 1)));
static constexpr uint32_t indexOfKeyOrderRun(uint64_t orderAndShapeWordsBefore, uint64_t slotKeys)
{
    return (static_cast<uint32_t>(orderAndShapeWordsBefore >> 32) ^ static_cast<uint32_t>(orderAndShapeWordsBefore) ^ static_cast<uint32_t>(slotKeys)) % numberOfKeyOrderRuns;
}

// For an object with extra properties, whose order word says so, the word of its Structure stands in place of the shape word: that Structure names theirs.
struct KeyListCacheEntry {
    uint32_t orderWord;
    uint32_t shapeWord;
    JSCell* list;
};

static constexpr uint32_t numberOfKeyLists = 1024;
static constexpr uint32_t indexOfKeyList(uint32_t shapeWord, uint32_t orderWord) { return (shapeWord + orderWord) % numberOfKeyLists; }
static constexpr uint32_t keyListWordOfStructure(uint32_t structureID) { return structureID >> 1; }

// The enumerator that a for-in loop over a compiled object with this Structure and order word got last.
struct EnumeratorCacheEntry {
    uint32_t orderWord;
    uint32_t structureID;
    JSCell* enumerator;
};

static constexpr uint32_t numberOfCachedEnumerators = 256;
static constexpr unsigned enumeratorHashShift = 4;
static constexpr uint32_t indexOfCachedEnumerator(uint32_t structureID, uint32_t orderWord) { return ((structureID >> enumeratorHashShift) + orderWord) % numberOfCachedEnumerators; }

// The Structure that an object with the Structure `before` gets with another prototype. The headers of their cells differ in the StructureID alone.
struct PrototypeTransition {
    EncodedJSValue prototype;
    uint32_t before;
    uint32_t after;
};

// The table has two halves of numberOfPrototypeTransitions each. A transition of the first half sets IsPristine in the cell: the object stands below another prototype than its layout's.
// One of the second half leaves the flags alone: the class has taken what lies between the object and its prototype object (ClassTable::takesObjectsLaidBelowPrototype()).
static constexpr uint32_t numberOfPrototypeTransitions = 1024;
static constexpr unsigned prototypeTransitionHashShift = 6;
static constexpr uint32_t indexOfPrototypeTransition(uint32_t before, EncodedJSValue prototype) { return ((before ^ static_cast<uint32_t>(prototype)) >> prototypeTransitionHashShift) % numberOfPrototypeTransitions; }

// `{ ...source, name, other }`: what the copy of an ordinary object with this Structure is once the names are defined on it. It follows from the Structure and the names alone.
// Behind it lies a NameInCopy for each name, in the order of the definitions.
struct CopyWithNames {
    uint32_t structureOfSource;
    uint32_t firstName;
    // The first word of the cell of the copy.
    uint64_t header;
    uint16_t count;
    uint16_t cellSize;
    // In bytes, outside the cell. The copy has room for the names that its cell has none for.
    uint16_t storageSizeOfSource;
    uint16_t storageSize;
};

struct NameInCopy {
    // The number of the identifier.
    uint32_t name;
    // Of the value: from the cell, or below zero from the butterfly.
    int32_t offset;
};

// The table begins with the places: each holds the distance in bytes from the start of the table to a CopyWithNames, or zero. The next word holds the bytes that are taken behind it, and the records follow.
static constexpr uint32_t numberOfPlacesOfCopiesWithNames = 512;
static constexpr uint32_t sizeOfCopiesWithNames = 16 * 1024;
static constexpr uint32_t offsetOfTakenBytesOfCopiesWithNames = numberOfPlacesOfCopiesWithNames * sizeof(uint32_t);
static constexpr uint32_t offsetOfFirstCopyWithNames = offsetOfTakenBytesOfCopiesWithNames + sizeof(uint64_t);
static constexpr unsigned copyWithNamesHashShift = 6;
static constexpr uint32_t copiesThatTakeAnOccupiedPlaceMask = 63;
static constexpr uint32_t placeOfCopyWithNames(uint32_t structureOfSource, uint32_t firstName) { return ((structureOfSource >> copyWithNamesHashShift) + firstName) % numberOfPlacesOfCopiesWithNames; }
static_assert(sizeof(CopyWithNames) == 24 && sizeof(NameInCopy) == 8 && !(offsetOfFirstCopyWithNames % alignof(CopyWithNames)));

struct ObjectWords {
    // One word per allocation: the distance in bytes, zero or negative, from the empty record to the record of the allocation.
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

struct FieldData;

// Where the objects of a layout keep a private name that the class of another layout declares: an instance of `class B extends A {}` has a field for each name of A.
// Layouts and their fields are data of the image, so a place never goes stale. Only the names of a class that is evaluated once have places.
struct PrivateFieldPlace {
    // The number of the layout of the object above the number of the declaring class. They differ, so no place that is in use holds nought.
    uint64_t layouts;
    // In the declaring layout.
    uint32_t declaredField;
    uint32_t unused;
    // Of the layout of the object.
    const FieldData* field;
    uint64_t alsoUnused;
};
constexpr uint32_t numberOfPrivateFieldPlaces = 64;
constexpr unsigned shiftOfPrivateFieldPlace = 5;
constexpr size_t indexOfPrivateFieldPlace(uint32_t layoutOfObject, uint32_t declaredField) { return (layoutOfObject ^ declaredField) & (numberOfPrivateFieldPlaces - 1); }

struct ClassWords {
    JSCell** classPrototypes;
    const PlaceOfPrototypeProperty* placesOfPrototypeProperties;
    const PrototypeOfClassObject* prototypesOfClassObjects;
    PrivateFieldPlace* privateFieldPlaces;
};

static_assert(sizeof(PrivateFieldPlace) == 1u << shiftOfPrivateFieldPlace && !(numberOfPrivateFieldPlaces & (numberOfPrivateFieldPlaces - 1)));
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
