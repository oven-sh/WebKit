/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "AOTRuntimeRecords.h"
#include <array>
#include <type_traits>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

struct alignas(32) HashedSlot {
    static constexpr uintptr_t accessMask = 3;
    static constexpr unsigned ways = 4;
    static constexpr unsigned sizeShift = 5;
    static constexpr uint32_t structureMultiplier = 0x9e3779b1;
    static constexpr unsigned structureHashShift = 16 - sizeShift;
    static constexpr unsigned uidAlignmentShift = 4;
    static constexpr unsigned uidHashShift = sizeShift - uidAlignmentShift;

    static uintptr_t keyFor(const UniquedStringImpl* uid, SlotAccess access) { return std::bit_cast<uintptr_t>(uid) | static_cast<uintptr_t>(access); }
    static constexpr uint64_t offsetMaskFor(uint32_t numberOfEntries) { return static_cast<uint64_t>(numberOfEntries - 1) << sizeShift; }
    static uint32_t offsetOfFirstWay(StructureID structureID, const UniquedStringImpl* uid, uint64_t offsetMask)
    {
        uint32_t structureHash = (structureID.bits() * structureMultiplier) >> structureHashShift;
        uint32_t uidHash = static_cast<uint32_t>(std::bit_cast<uintptr_t>(uid)) << uidHashShift;
        return static_cast<uint32_t>((structureHash + uidHash) & offsetMask);
    }
    static constexpr uint32_t offsetOfWay(uint32_t offsetOfFirstWay, unsigned way) { return offsetOfFirstWay ^ (way << sizeShift); }

    bool isEmpty() const { return !key; }
    bool matches(StructureID structureID, uintptr_t expectedKey) const { return key == expectedKey && slot.structureID == structureID; }
    UniquedStringImpl* uid() const { return std::bit_cast<UniquedStringImpl*>(key & ~accessMask); }
    SlotAccess access() const { return static_cast<SlotAccess>(key & accessMask); }

    static constexpr ptrdiff_t offsetOfSlot() { return 0; }
    static constexpr ptrdiff_t offsetOfKey() { return 16; }

    Slot slot;
    uintptr_t key;
};

enum class SlotForm : uint8_t { Slot, PackedSlot };
enum class SlotOrigin : uint8_t { ById, ByIdInStubFrame, ByVal, FreeName, FreeNameForTypeOf };
enum class SlotOperation : uint8_t { Read, StoreStrict, StoreSloppy, Define };

constexpr SlotAccess accessOf(SlotOperation operation) { return operation == SlotOperation::Read ? SlotAccess::Read : operation == SlotOperation::Define ? SlotAccess::Define : SlotAccess::Store; }
constexpr SlotOperation operationFor(PutMode mode) { return mode == PutMode::Strict ? SlotOperation::StoreStrict : mode == PutMode::Sloppy ? SlotOperation::StoreSloppy : SlotOperation::Define; }

#define FOR_EACH_AOT_SLOT_HANDLER_TABLE(v) \
    v(ReadById, Read, ById, Slot) \
    v(ReadByIdPacked, Read, ById, PackedSlot) \
    v(ReadByIdInStubFrame, Read, ByIdInStubFrame, Slot) \
    v(ReadByIdPackedInStubFrame, Read, ByIdInStubFrame, PackedSlot) \
    v(ReadByVal, Read, ByVal, Slot) \
    v(ReadFreeName, Read, FreeName, Slot) \
    v(ReadFreeNameForTypeOf, Read, FreeNameForTypeOf, Slot) \
    v(StoreStrictById, StoreStrict, ById, Slot) \
    v(StoreStrictByIdPacked, StoreStrict, ById, PackedSlot) \
    v(StoreStrictByVal, StoreStrict, ByVal, Slot) \
    v(StoreStrictFreeName, StoreStrict, FreeName, Slot) \
    v(StoreSloppyById, StoreSloppy, ById, Slot) \
    v(StoreSloppyByIdPacked, StoreSloppy, ById, PackedSlot) \
    v(StoreSloppyByVal, StoreSloppy, ByVal, Slot) \
    v(StoreSloppyFreeName, StoreSloppy, FreeName, Slot) \
    v(DefineById, Define, ById, Slot) \
    v(DefineByIdPacked, Define, ById, PackedSlot) \
    v(DefineByVal, Define, ByVal, Slot)

enum class SlotHandlerTable : uint8_t {
#define AOT_DECLARE_SLOT_HANDLER_TABLE(name, operation, origin, form) name,
    FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_DECLARE_SLOT_HANDLER_TABLE)
#undef AOT_DECLARE_SLOT_HANDLER_TABLE
};

struct SlotHandlerTableTraits {
    SlotOperation operation;
    SlotOrigin origin;
    SlotForm form;

    friend constexpr bool operator==(SlotHandlerTableTraits, SlotHandlerTableTraits) = default;
};

inline constexpr std::array allSlotHandlerTables {
#define AOT_LIST_SLOT_HANDLER_TABLE(name, operation, origin, form) SlotHandlerTable::name,
    FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_LIST_SLOT_HANDLER_TABLE)
#undef AOT_LIST_SLOT_HANDLER_TABLE
};
inline constexpr unsigned numberOfSlotHandlerTables = allSlotHandlerTables.size();

constexpr SlotHandlerTableTraits traitsOf(SlotHandlerTable table)
{
    constexpr SlotHandlerTableTraits traits[] = {
#define AOT_SLOT_HANDLER_TABLE_TRAITS(name, operation, origin, form) { SlotOperation::operation, SlotOrigin::origin, SlotForm::form },
        FOR_EACH_AOT_SLOT_HANDLER_TABLE(AOT_SLOT_HANDLER_TABLE_TRAITS)
#undef AOT_SLOT_HANDLER_TABLE_TRAITS
    };
    return traits[static_cast<unsigned>(table)];
}

constexpr SlotHandlerTable slotHandlerTable(SlotOperation operation, SlotOrigin origin, SlotForm form)
{
    for (SlotHandlerTable table : allSlotHandlerTables) {
        if (traitsOf(table) == SlotHandlerTableTraits { operation, origin, form })
            return table;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct StructureRowWords {
    static constexpr unsigned granuleShift = 6;
    static constexpr unsigned wordShift = 2;
};

struct AbsenceFilter {
    static constexpr unsigned numberOfFilters = 256;
    static constexpr unsigned sizeShift = 5;
    static constexpr unsigned bitsOfWordShift = 6;

    static constexpr unsigned indexFor(uint32_t structureIDBits) { return (structureIDBits >> StructureRowWords::granuleShift) & (numberOfFilters - 1); }
    static constexpr unsigned numberOfWordsForStores = 2;
    void addNameThatStopsStores(uint32_t identifierNumber) { namesThatStopStores[(identifierNumber >> bitsOfWordShift) & (numberOfWordsForStores - 1)] |= uint64_t { 1 } << (identifierNumber & 63); }
    bool letsStoresPass(uint32_t identifierNumber) const { return !(namesThatStopStores[(identifierNumber >> bitsOfWordShift) & (numberOfWordsForStores - 1)] >> (identifierNumber & 63) & 1); }

    uint32_t structureID;
    uint32_t generationForStores;
    uint64_t namesThatStopStores[numberOfWordsForStores];
    uint64_t unused;
};

struct NameOfText {
    static constexpr unsigned numberOfPairs = 64;
    static constexpr unsigned numberOfEntries = 2 * numberOfPairs;
    static constexpr unsigned sizeShift = 5;
    static constexpr uint32_t minLength = 2;
    static constexpr uint32_t maxLength = 16;
    static constexpr uint64_t multiplier = 0x9e3779b97f4a7c15ull;
    static constexpr unsigned pairShift = 64 - 6;

    static constexpr unsigned widthOfWordFor(uint32_t length) { return length >= 8 ? 8 : length >= 4 ? 4 : 2; }
    static constexpr unsigned firstIndexFor(uint64_t first, uint64_t last, uint32_t length) { return static_cast<unsigned>((first + 2 * last + length) * multiplier >> pairShift) * 2; }

    uint64_t first;
    uint64_t last;
    uint64_t length;
    UniquedStringImpl* uid;
};

// Where the objects of one Structure keep their own value under one key, found by the cell of the key. Nothing is asked of the key, so a string that is no atom is found as fast as one that is.
// Or that neither such an object nor any object above it has anything under the key.
struct KeyedValuePlace {
    static constexpr unsigned numberOfPlacesPerAccess = 512;
    static constexpr unsigned numberOfPlaces = 2 * numberOfPlacesPerAccess;
    static constexpr unsigned sizeShift = 4;
    static constexpr uint32_t offsetMask = (numberOfPlacesPerAccess - 1) << sizeShift;

    // The places for reads come first, then those for stores that replace the value.
    static constexpr int32_t offsetOfFirst(SlotAccess access)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(access != SlotAccess::Define);
        return access == SlotAccess::Read ? 0 : numberOfPlacesPerAccess << sizeShift;
    }

    // The low bits of a StructureID are zero. What a place says beside the absence bit holds until an object of some chain changes: PropertyCacheWords::placesSayAbsence.
    static constexpr uint32_t absenceBit = 1;
    static constexpr uint32_t fieldBit = 2;

    JSCell* key;
    uint32_t structureID;
    union {
        // Slot::wordIndexFor().
        int32_t wordIndex;
        // Beside the field bit: SlotData::bits() of a field that lies in the cell.
        uint32_t slotData;
    };
};

enum class TrapOfProxy : uint8_t { Get, Has, Set };
inline constexpr unsigned numberOfTrapsOfProxies = 3;

// What the stub that goes through a proxy keeps in its frame while code of the program runs.
struct ProxyAccessWords {
    JSObject* proxy;
    JSCell* key;
    JSObject* target;
    JSObject* handler;
    EncodedJSValue storedValue;
    // Of a store: whether the code is strict. Of a read: Slot::headerBits() of the slot that led here.
    uint64_t storeIsStrictOrHeaderOfSlot;
};

struct MissesOfPairs {
    static constexpr uint32_t numberOfCounters = 1u << 15;
    static constexpr uint32_t missesBetweenFilings = 16;
    static constexpr uint32_t missesBetweenFilingsOfDictionaries = 64;
};

struct FrontWords {
    const uint32_t* rowsForStructureIDZero;
    const PackedSlot* plane;
};

using SlotHandlers = void*[numberOfSlotHandlerTables][numberOfSlotKindsWithHandlers];

struct PropertyCacheWords {
    FrontWords front[numberOfSlotAccesses];
    const HashedSlot* hashedSlots;
    uint64_t hashedSlotOffsetMask;
    AbsenceFilter* absenceFilters;
    uint32_t absenceFilterGenerationForStores;
    const uint64_t* namesOfObjectPrototypes;
    const uint64_t* namesChangedOnChains;
    uint8_t* missesOfPairs;
    const NameOfText* namesOfTexts;
    uint8_t missIsDueForFiling;
    uint8_t placesSayAbsence;
    // A place for stores was filled while nobody watched the Structure for replacements. Such a place holds until somebody does.
    uint8_t placesTakeReplacementsAsUnwatched;
    const void* infoOfClassPrototypes;
    SiteCacheState* siteCacheStates;
    uint8_t missesOfEmptySites;
    uint8_t storesAtEmptySites;
    uint8_t storesToGuardedFields;
    const CustomAccessorRecord* customAccessors;
    uint32_t numberStructureID;
    uint32_t booleanStructureID;
    // By TrapOfProxy. Zero where the image does not have the name.
    uint32_t numbersOfNamesOfTraps[numberOfTrapsOfProxies];
    UniquedStringImpl* hasInstanceUID;
    UniquedStringImpl* prototypeUID;
    UniquedStringImpl* lengthUID;
    UniquedStringImpl* nameUID;
    const void* lookupOfOrdinaryObjects;
    const void* lookupOfArrays;
    const void* lookupOfFunctions;
    JSCell* defaultHasInstanceFunction;
    const FieldExit* fieldExits;
#if !defined(BUN_HARDENED)
    SlotHandlers handlers;
#endif

    static constexpr ptrdiff_t offsetOfFront(SlotAccess access) { return OBJECT_OFFSETOF(PropertyCacheWords, front) + static_cast<ptrdiff_t>(static_cast<unsigned>(access) * sizeof(FrontWords)); }
};

static_assert(sizeof(HashedSlot) == 1u << HashedSlot::sizeShift && alignof(HashedSlot) == sizeof(HashedSlot));
static_assert(OBJECT_OFFSETOF(HashedSlot, slot) == HashedSlot::offsetOfSlot() && OBJECT_OFFSETOF(HashedSlot, key) == HashedSlot::offsetOfKey());
static_assert(std::is_trivially_copyable_v<HashedSlot> && std::is_aggregate_v<HashedSlot>);
static_assert(static_cast<unsigned>(SlotAccess::Delete) <= HashedSlot::accessMask && alignof(UniquedStringImpl) > HashedSlot::accessMask);
static_assert(hasOneBitSet(HashedSlot::ways));
static_assert(sizeof(FrontWords) == 2 * sizeof(void*) && OBJECT_OFFSETOF(FrontWords, plane) == sizeof(void*));
static_assert(1u << StructureRowWords::wordShift == sizeof(uint32_t));
static_assert(sizeof(AbsenceFilter) == 1u << AbsenceFilter::sizeShift && hasOneBitSet(AbsenceFilter::numberOfFilters) && hasOneBitSet(AbsenceFilter::numberOfWordsForStores));
static_assert(std::is_trivially_copyable_v<AbsenceFilter> && std::is_aggregate_v<AbsenceFilter>);
static_assert(sizeof(NameOfText) == 1u << NameOfText::sizeShift && NameOfText::numberOfPairs == 1u << (64 - NameOfText::pairShift));
static_assert(sizeof(KeyedValuePlace) == 1u << KeyedValuePlace::sizeShift && hasOneBitSet(KeyedValuePlace::numberOfPlacesPerAccess));
static_assert(std::is_trivially_default_constructible_v<PropertyCacheWords>);
static_assert(OBJECT_OFFSETOF(PropertyCacheWords, hashedSlotOffsetMask) == OBJECT_OFFSETOF(PropertyCacheWords, hashedSlots) + sizeof(void*));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
